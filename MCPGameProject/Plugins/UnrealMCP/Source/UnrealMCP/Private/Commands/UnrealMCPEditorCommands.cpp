#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#include "Misc/FileHelper.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Light.h"
#include "Engine/RectLight.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Components/MeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "LevelEditorSubsystem.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "Containers/Ticker.h"
#include "Engine/Texture2D.h"

namespace
{
    // Reads an [R,G,B(,A)] array as a linear color. Values above 1 are treated as 0-255.
    FLinearColor GetLinearColorFromJson(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Obj->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() == 0)
        {
            return FLinearColor::White;
        }
        float R = 1.f, G = 1.f, B = 1.f;
        if (Arr->Num() > 0) { R = (float)(*Arr)[0]->AsNumber(); }
        if (Arr->Num() > 1) { G = (float)(*Arr)[1]->AsNumber(); }
        if (Arr->Num() > 2) { B = (float)(*Arr)[2]->AsNumber(); }
        if (R > 1.f || G > 1.f || B > 1.f) { R /= 255.f; G /= 255.f; B /= 255.f; }
        return FLinearColor(R, G, B);
    }

    // ---------------------------------------------------------------------
    // Async asset import jobs (polled via get_import_status)
    // ---------------------------------------------------------------------

    struct FImportJobState
    {
        FString State;              // queued | running | done | failed
        TArray<FString> Assets;     // imported object paths
        TArray<FString> Log;
        FString Error;
    };

    TMap<FString, TSharedPtr<FImportJobState>> GImportJobs;
    FCriticalSection GImportJobsMutex;

    // Applies optional texture import options to an imported texture and re-saves it.
    void ApplyTextureImportOptions(UTexture2D* Tex, const TSharedPtr<FJsonObject>& Options)
    {
        if (!Tex || !Options.IsValid()) { return; }
        bool bChanged = false;

        bool bNormal = false;
        if (Options->TryGetBoolField(TEXT("is_normal_map"), bNormal) && bNormal)
        {
            Tex->CompressionSettings = TextureCompressionSettings::TC_Normalmap;
            Tex->SRGB = false;
            bChanged = true;
        }

        FString Compression;
        if (Options->TryGetStringField(TEXT("compression"), Compression))
        {
            Compression.ToLowerInline();
            if (Compression == TEXT("grayscale")) { Tex->CompressionSettings = TextureCompressionSettings::TC_Grayscale; Tex->SRGB = false; bChanged = true; }
            else if (Compression == TEXT("normalmap")) { Tex->CompressionSettings = TextureCompressionSettings::TC_Normalmap; Tex->SRGB = false; bChanged = true; }
            else if (Compression == TEXT("default")) { Tex->CompressionSettings = TextureCompressionSettings::TC_Default; bChanged = true; }
        }

        bool bSrgb = true;
        if (Options->TryGetBoolField(TEXT("srgb"), bSrgb))
        {
            Tex->SRGB = bSrgb;
            bChanged = true;
        }

        if (bChanged)
        {
            Tex->MarkPackageDirty();
            UEditorAssetLibrary::SaveLoadedAsset(Tex, false);
        }
    }

    // Runs on the core-ticker game-thread tick, i.e. OUTSIDE the game-thread task that
    // dispatched the command. Importing assets here avoids the nested-task assertion that
    // fires when AssetImportTask's internal task-graph flush runs inside a task.
    void RunImportJob(const FString& JobId, TArray<FString> Sources, FString DestinationPath,
                      bool bReplaceExisting, TSharedPtr<FJsonObject> Options)
    {
        TSharedPtr<FImportJobState> Job;
        {
            FScopeLock Lock(&GImportJobsMutex);
            TSharedPtr<FImportJobState>* Found = GImportJobs.Find(JobId);
            if (!Found) { return; }
            Job = *Found;
            Job->State = TEXT("running");
        }

        if (Sources.Num() == 0)
        {
            Job->State = TEXT("failed");
            Job->Error = TEXT("No sources provided");
            return;
        }

        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        IAssetTools& AssetTools = AssetToolsModule.Get();

        for (const FString& Src : Sources)
        {
            if (!FPaths::FileExists(Src))
            {
                Job->Log.Add(FString::Printf(TEXT("Skipped (file not found): %s"), *Src));
                continue;
            }

            UAssetImportTask* Task = NewObject<UAssetImportTask>();
            Task->AddToRoot();
            Task->Filename = Src;
            Task->DestinationPath = DestinationPath;
            Task->bReplaceExisting = bReplaceExisting;
            Task->bAutomated = true;
            Task->bSave = true;
            Task->bAsync = false;

            TArray<UAssetImportTask*> Tasks;
            Tasks.Add(Task);
            AssetTools.ImportAssetTasks(Tasks);

            const TArray<UObject*>& Objects = Task->GetObjects();
            if (Objects.Num() == 0)
            {
                Job->Log.Add(FString::Printf(TEXT("No objects produced for: %s"), *Src));
            }
            for (UObject* Obj : Objects)
            {
                if (!Obj) { continue; }
                if (UTexture2D* Tex = Cast<UTexture2D>(Obj))
                {
                    ApplyTextureImportOptions(Tex, Options);
                }
                Job->Assets.Add(Obj->GetPathName());
            }
            Task->RemoveFromRoot();
        }

        if (Job->State != TEXT("failed"))
        {
            Job->State = TEXT("done");
        }
    }

    // Destroys actors through UEditorActorSubsystem so the editor's selection set, layers,
    // and typed-element registry stay consistent. Bypassing this (raw Actor->Destroy())
    // leaves dangling typed-element references and trips the TypedElementRegistry assertion
    // ("Element type ID has not been registered"). Returns true only when every actor was
    // destroyed; callers must NOT silently fall back to Actor->Destroy().
    bool DestroyActorsViaEditorSubsystem(const TArray<AActor*>& Actors)
    {
        if (Actors.Num() == 0)
        {
            return true;
        }

        UEditorActorSubsystem* EditorActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
        if (!EditorActorSubsystem)
        {
            UE_LOG(LogTemp, Error, TEXT("DestroyActorsViaEditorSubsystem: UEditorActorSubsystem is unavailable"));
            return false;
        }

        return EditorActorSubsystem->DestroyActors(Actors);
    }
}

FUnrealMCPEditorCommands::FUnrealMCPEditorCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    // Actor manipulation commands
    if (CommandType == TEXT("get_actors_in_level"))
    {
        return HandleGetActorsInLevel(Params);
    }
    else if (CommandType == TEXT("find_actors_by_name"))
    {
        return HandleFindActorsByName(Params);
    }
    else if (CommandType == TEXT("spawn_actor") || CommandType == TEXT("create_actor"))
    {
        if (CommandType == TEXT("create_actor"))
        {
            UE_LOG(LogTemp, Warning, TEXT("'create_actor' command is deprecated and will be removed in a future version. Please use 'spawn_actor' instead."));
        }
        return HandleSpawnActor(Params);
    }
    else if (CommandType == TEXT("delete_actor"))
    {
        return HandleDeleteActor(Params);
    }
    else if (CommandType == TEXT("set_actor_transform"))
    {
        return HandleSetActorTransform(Params);
    }
    else if (CommandType == TEXT("get_actor_properties"))
    {
        return HandleGetActorProperties(Params);
    }
    else if (CommandType == TEXT("set_actor_property"))
    {
        return HandleSetActorProperty(Params);
    }
    // Blueprint actor spawning
    else if (CommandType == TEXT("spawn_blueprint_actor"))
    {
        return HandleSpawnBlueprintActor(Params);
    }
    // Editor viewport commands
    else if (CommandType == TEXT("focus_viewport"))
    {
        return HandleFocusViewport(Params);
    }
    else if (CommandType == TEXT("take_screenshot"))
    {
        return HandleTakeScreenshot(Params);
    }
    
    else if (CommandType == TEXT("get_actor_details")) { return HandleGetActorProperties(Params); }
    else if (CommandType == TEXT("get_capabilities")) { return HandleGetCapabilities(Params); }
    else if (CommandType == TEXT("query_assets")) { return HandleQueryAssets(Params); }
    else if (CommandType == TEXT("get_asset_details")) { return HandleGetAssetDetails(Params); }
    else if (CommandType == TEXT("spawn_mesh_actor")) { return HandleSpawnMeshActor(Params); }
    else if (CommandType == TEXT("spawn_light_actor")) { return HandleSpawnLightActor(Params); }
    else if (CommandType == TEXT("spawn_mesh_grid")) { return HandleSpawnMeshGrid(Params); }
    else if (CommandType == TEXT("spawn_instanced_mesh")) { return HandleSpawnInstancedMesh(Params); }
    else if (CommandType == TEXT("set_actor_material")) { return HandleSetActorMaterial(Params); }
    else if (CommandType == TEXT("set_actor_folder")) { return HandleSetActorFolder(Params); }
    else if (CommandType == TEXT("delete_actors_by_prefix")) { return HandleDeleteActorsByPrefix(Params); }
    else if (CommandType == TEXT("create_level")) { return HandleCreateLevel(Params); }
    else if (CommandType == TEXT("save_level")) { return HandleSaveLevel(Params); }
    else if (CommandType == TEXT("load_level")) { return HandleLoadLevel(Params); }
    else if (CommandType == TEXT("delete_level")) { return HandleDeleteLevel(Params); }
    else if (CommandType == TEXT("set_viewport_camera")) { return HandleSetViewportCamera(Params); }
    else if (CommandType == TEXT("capture_viewport_screenshot")) { return HandleCaptureViewportScreenshot(Params); }
    else if (CommandType == TEXT("batch_execute")) { return HandleBatchExecute(Params); }
    else if (CommandType == TEXT("execute_python")) { return HandleExecutePython(Params); }
    else if (CommandType == TEXT("import_asset")) { return HandleImportAsset(Params); }
    else if (CommandType == TEXT("get_import_status")) { return HandleGetImportStatus(Params); }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown editor command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetCapabilities(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("protocol_version"), TEXT("1.0"));
    ResultObj->SetStringField(TEXT("plugin"), TEXT("UnrealMCP"));
    ResultObj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

    static const TCHAR* SupportedCommands[] = {
        TEXT("get_actors_in_level"), TEXT("find_actors_by_name"), TEXT("spawn_actor"),
        TEXT("create_actor"), TEXT("delete_actor"), TEXT("set_actor_transform"),
        TEXT("get_actor_properties"), TEXT("get_actor_details"), TEXT("set_actor_property"),
        TEXT("spawn_blueprint_actor"), TEXT("spawn_mesh_actor"), TEXT("spawn_light_actor"),
        TEXT("spawn_mesh_grid"), TEXT("spawn_instanced_mesh"), TEXT("set_actor_material"),
        TEXT("set_actor_folder"), TEXT("delete_actors_by_prefix"), TEXT("focus_viewport"),
        TEXT("take_screenshot"), TEXT("capture_viewport_screenshot"), TEXT("set_viewport_camera"),
        TEXT("create_level"), TEXT("save_level"), TEXT("load_level"), TEXT("delete_level"),
        TEXT("query_assets"), TEXT("get_asset_details"), TEXT("get_capabilities"),
        TEXT("batch_execute"), TEXT("execute_python"), TEXT("reload_server"),
        TEXT("import_asset"), TEXT("get_import_status"),
        TEXT("delete_blueprint_node"), TEXT("clear_blueprint_graph"),
        TEXT("disconnect_blueprint_pin"), TEXT("get_blueprint_graphs")
    };
    TArray<TSharedPtr<FJsonValue>> CommandArray;
    for (const TCHAR* Cmd : SupportedCommands)
    {
        CommandArray.Add(MakeShared<FJsonValueString>(FString(Cmd)));
    }
    ResultObj->SetArrayField(TEXT("commands"), CommandArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleQueryAssets(const TSharedPtr<FJsonObject>& Params)
{
    FString Path = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), Path);
    FString ClassFilter;
    Params->TryGetStringField(TEXT("asset_class"), ClassFilter);
    FString Search;
    Params->TryGetStringField(TEXT("search"), Search);
    bool bRecursive = true;
    if (Params->HasField(TEXT("recursive"))) { bRecursive = Params->GetBoolField(TEXT("recursive")); }
    int32 Limit = 100;
    if (Params->HasField(TEXT("limit"))) { Limit = (int32)Params->GetNumberField(TEXT("limit")); }
    int32 Offset = 0;
    if (Params->HasField(TEXT("offset"))) { Offset = (int32)Params->GetNumberField(TEXT("offset")); }

    FARFilter Filter;
    Filter.PackagePaths.Add(*Path);
    Filter.bRecursivePaths = bRecursive;
    Filter.bRecursiveClasses = true;

    FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets;
    ARModule.Get().GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> AssetArray;
    int32 Matched = 0, Emitted = 0;
    for (const FAssetData& Asset : Assets)
    {
        const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
        if (!ClassFilter.IsEmpty() && !ClassName.Equals(ClassFilter, ESearchCase::IgnoreCase)) { continue; }
        const FString Name = Asset.AssetName.ToString();
        if (!Search.IsEmpty() && !Name.Contains(Search)) { continue; }
        Matched++;
        if (Matched <= Offset || Emitted >= Limit) { continue; }

        TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
        AssetObj->SetStringField(TEXT("name"), Name);
        AssetObj->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
        AssetObj->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
        AssetObj->SetStringField(TEXT("asset_class"), ClassName);
        AssetArray.Add(MakeShared<FJsonValueObject>(AssetObj));
        Emitted++;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("assets"), AssetArray);
    ResultObj->SetNumberField(TEXT("count"), Emitted);
    ResultObj->SetNumberField(TEXT("total_matched"), Matched);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetAssetDetails(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

    if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
    {
        const FBoxSphereBounds Bounds = Mesh->GetBounds();
        TSharedPtr<FJsonObject> Extents = MakeShared<FJsonObject>();
        Extents->SetNumberField(TEXT("x"), Bounds.BoxExtent.X);
        Extents->SetNumberField(TEXT("y"), Bounds.BoxExtent.Y);
        Extents->SetNumberField(TEXT("z"), Bounds.BoxExtent.Z);
        ResultObj->SetObjectField(TEXT("bounds_extents"), Extents);

        TSharedPtr<FJsonObject> Dims = MakeShared<FJsonObject>();
        Dims->SetNumberField(TEXT("x"), Bounds.BoxExtent.X * 2.0);
        Dims->SetNumberField(TEXT("y"), Bounds.BoxExtent.Y * 2.0);
        Dims->SetNumberField(TEXT("z"), Bounds.BoxExtent.Z * 2.0);
        ResultObj->SetObjectField(TEXT("dimensions"), Dims);

        TArray<TSharedPtr<FJsonValue>> Materials;
        for (const FStaticMaterial& Mat : Mesh->GetStaticMaterials())
        {
            Materials.Add(MakeShared<FJsonValueString>(Mat.MaterialInterface ? Mat.MaterialInterface->GetPathName() : FString()));
        }
        ResultObj->SetArrayField(TEXT("materials"), Materials);
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnMeshActor(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (!Params->TryGetStringField(TEXT("mesh_path"), MeshPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'mesh_path' parameter"));
    }

    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
    if (!Mesh)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FString ActorName = TEXT("MeshActor");
    Params->TryGetStringField(TEXT("name"), ActorName);

    FVector Location(0, 0, 0), Scale(1, 1, 1);
    FRotator Rotation(0, 0, 0);
    if (Params->HasField(TEXT("location"))) { Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")); }
    if (Params->HasField(TEXT("rotation"))) { Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")); }
    if (Params->HasField(TEXT("scale"))) { Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")); }

    FActorSpawnParameters SpawnParams;
    if (!ActorName.IsEmpty()) { SpawnParams.Name = *ActorName; }
    AStaticMeshActor* NewActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, Rotation, SpawnParams);
    if (!NewActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn mesh actor"));
    }

    if (UStaticMeshComponent* SMC = NewActor->GetStaticMeshComponent())
    {
        SMC->SetMobility(EComponentMobility::Movable);
        SMC->SetStaticMesh(Mesh);
    }
    FTransform T = NewActor->GetActorTransform();
    T.SetScale3D(Scale);
    NewActor->SetActorTransform(T);

    FString FolderPath;
    if (Params->TryGetStringField(TEXT("folder_path"), FolderPath) && !FolderPath.IsEmpty())
    {
        NewActor->SetFolderPath(FName(*FolderPath));
    }
    return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnLightActor(const TSharedPtr<FJsonObject>& Params)
{
    FString LightType = TEXT("PointLight");
    Params->TryGetStringField(TEXT("light_type"), LightType);
    FString ActorName;
    Params->TryGetStringField(TEXT("name"), ActorName);

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FVector Location(0, 0, 0);
    FRotator Rotation(0, 0, 0);
    if (Params->HasField(TEXT("location"))) { Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")); }
    if (Params->HasField(TEXT("rotation"))) { Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")); }

    FActorSpawnParameters SpawnParams;
    if (!ActorName.IsEmpty()) { SpawnParams.Name = *ActorName; }

    ALight* NewLight = nullptr;
    if (LightType.Equals(TEXT("PointLight"), ESearchCase::IgnoreCase))
    {
        NewLight = World->SpawnActor<APointLight>(APointLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (LightType.Equals(TEXT("SpotLight"), ESearchCase::IgnoreCase))
    {
        NewLight = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (LightType.Equals(TEXT("DirectionalLight"), ESearchCase::IgnoreCase))
    {
        NewLight = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (LightType.Equals(TEXT("RectLight"), ESearchCase::IgnoreCase))
    {
        NewLight = World->SpawnActor<ARectLight>(ARectLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown light type: %s"), *LightType));
    }

    if (!NewLight)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn light actor"));
    }

    if (ULightComponent* LC = NewLight->GetLightComponent())
    {
        LC->SetMobility(EComponentMobility::Movable);
        if (Params->HasField(TEXT("intensity"))) { LC->SetIntensity((float)Params->GetNumberField(TEXT("intensity"))); }
        if (Params->HasField(TEXT("color"))) { LC->SetLightColor(GetLinearColorFromJson(Params, TEXT("color"))); }
        if (Params->HasField(TEXT("attenuation_radius")))
        {
            if (ULocalLightComponent* LLC = Cast<ULocalLightComponent>(LC))
            {
                LLC->SetAttenuationRadius((float)Params->GetNumberField(TEXT("attenuation_radius")));
            }
        }
        if (Params->HasField(TEXT("source_radius")))
        {
            if (UPointLightComponent* PLC = Cast<UPointLightComponent>(LC))
            {
                PLC->SetSourceRadius((float)Params->GetNumberField(TEXT("source_radius")));
            }
        }
    }

    FString FolderPath;
    if (Params->TryGetStringField(TEXT("folder_path"), FolderPath) && !FolderPath.IsEmpty())
    {
        NewLight->SetFolderPath(FName(*FolderPath));
    }
    return FUnrealMCPCommonUtils::ActorToJsonObject(NewLight, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnMeshGrid(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (!Params->TryGetStringField(TEXT("mesh_path"), MeshPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'mesh_path' parameter"));
    }
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
    if (!Mesh)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    int32 Rows = 1, Cols = 1;
    float SpacingX = 200.f, SpacingY = 200.f;
    FString Prefix = TEXT("GridActor");
    FString FolderPath = TEXT("Environment/Grids");
    FVector Origin(0, 0, 0), Scale(1, 1, 1);
    FRotator Rotation(0, 0, 0);
    if (Params->HasField(TEXT("rows"))) { Rows = (int32)Params->GetNumberField(TEXT("rows")); }
    if (Params->HasField(TEXT("cols"))) { Cols = (int32)Params->GetNumberField(TEXT("cols")); }
    if (Params->HasField(TEXT("spacing_x"))) { SpacingX = (float)Params->GetNumberField(TEXT("spacing_x")); }
    if (Params->HasField(TEXT("spacing_y"))) { SpacingY = (float)Params->GetNumberField(TEXT("spacing_y")); }
    Params->TryGetStringField(TEXT("prefix"), Prefix);
    Params->TryGetStringField(TEXT("folder_path"), FolderPath);
    if (Params->HasField(TEXT("origin"))) { Origin = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("origin")); }
    if (Params->HasField(TEXT("rotation"))) { Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")); }
    if (Params->HasField(TEXT("scale"))) { Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")); }

    TArray<TSharedPtr<FJsonValue>> ActorArray;
    for (int32 r = 0; r < Rows; ++r)
    {
        for (int32 c = 0; c < Cols; ++c)
        {
            const FVector Loc = Origin + FVector(c * SpacingX, r * SpacingY, 0.f);
            FActorSpawnParameters SpawnParams;
            SpawnParams.Name = *FString::Printf(TEXT("%s_%d_%d"), *Prefix, r, c);
            AStaticMeshActor* NewActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Loc, Rotation, SpawnParams);
            if (!NewActor) { continue; }
            if (UStaticMeshComponent* SMC = NewActor->GetStaticMeshComponent())
            {
                SMC->SetMobility(EComponentMobility::Movable);
                SMC->SetStaticMesh(Mesh);
            }
            FTransform T = NewActor->GetActorTransform();
            T.SetScale3D(Scale);
            NewActor->SetActorTransform(T);
            if (!FolderPath.IsEmpty()) { NewActor->SetFolderPath(FName(*FolderPath)); }
            ActorArray.Add(FUnrealMCPCommonUtils::ActorToJson(NewActor));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);
    ResultObj->SetNumberField(TEXT("count"), ActorArray.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnInstancedMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (!Params->TryGetStringField(TEXT("mesh_path"), MeshPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'mesh_path' parameter"));
    }
    const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
    if (!Params->TryGetArrayField(TEXT("instances"), Instances) || !Instances)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'instances' array parameter"));
    }
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
    if (!Mesh)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FString ActorName = TEXT("InstancedActor");
    Params->TryGetStringField(TEXT("name"), ActorName);
    FString FolderPath = TEXT("Environment/Instances");
    Params->TryGetStringField(TEXT("folder_path"), FolderPath);

    AActor* NewActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
    if (!NewActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn instanced mesh actor"));
    }

    UHierarchicalInstancedStaticMeshComponent* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(NewActor, TEXT("HISM"));
    HISM->SetMobility(EComponentMobility::Movable);
    HISM->SetStaticMesh(Mesh);
    NewActor->SetRootComponent(HISM);
    NewActor->AddInstanceComponent(HISM);
    HISM->RegisterComponent();

    int32 Count = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Instances)
    {
        const TSharedPtr<FJsonObject>* InstObj = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(InstObj) || !InstObj) { continue; }
        FVector Loc(0, 0, 0), Scale(1, 1, 1);
        FRotator Rot(0, 0, 0);
        if ((*InstObj)->HasField(TEXT("location"))) { Loc = FUnrealMCPCommonUtils::GetVectorFromJson(*InstObj, TEXT("location")); }
        if ((*InstObj)->HasField(TEXT("rotation"))) { Rot = FUnrealMCPCommonUtils::GetRotatorFromJson(*InstObj, TEXT("rotation")); }
        if ((*InstObj)->HasField(TEXT("scale"))) { Scale = FUnrealMCPCommonUtils::GetVectorFromJson(*InstObj, TEXT("scale")); }
        HISM->AddInstance(FTransform(Rot, Loc, Scale));
        Count++;
    }

    NewActor->SetActorLabel(ActorName);
    if (!FolderPath.IsEmpty()) { NewActor->SetFolderPath(FName(*FolderPath)); }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), NewActor->GetName());
    ResultObj->SetStringField(TEXT("mesh_path"), MeshPath);
    ResultObj->SetNumberField(TEXT("instance_count"), Count);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName, MaterialPath;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }
    int32 SlotIndex = 0;
    if (Params->HasField(TEXT("slot_index"))) { SlotIndex = (int32)Params->GetNumberField(TEXT("slot_index")); }

    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName) { TargetActor = Actor; break; }
    }
    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    UMeshComponent* MeshComp = TargetActor->FindComponentByClass<UMeshComponent>();
    if (!MeshComp)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor '%s' has no mesh component"), *ActorName));
    }
    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
    }
    MeshComp->SetMaterial(SlotIndex, Material);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor"), ActorName);
    ResultObj->SetStringField(TEXT("material"), MaterialPath);
    ResultObj->SetNumberField(TEXT("slot_index"), SlotIndex);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorFolder(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName, FolderPath;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("folder_path"), FolderPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'folder_path' parameter"));
    }

    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName) { TargetActor = Actor; break; }
    }
    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }
    TargetActor->SetFolderPath(FName(*FolderPath));

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor"), ActorName);
    ResultObj->SetStringField(TEXT("folder_path"), FolderPath);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleDeleteActorsByPrefix(const TSharedPtr<FJsonObject>& Params)
{
    FString Prefix;
    if (!Params->TryGetStringField(TEXT("prefix"), Prefix))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'prefix' parameter"));
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);

    TArray<AActor*> ToDelete;
    for (AActor* Actor : AllActors)
    {
        if (Actor && (Actor->GetActorLabel().StartsWith(Prefix) || Actor->GetName().StartsWith(Prefix)))
        {
            ToDelete.Add(Actor);
        }
    }

    TArray<TSharedPtr<FJsonValue>> DeletedArray;
    for (AActor* Actor : ToDelete)
    {
        DeletedArray.Add(MakeShared<FJsonValueString>(Actor->GetName()));
    }

    // Delete via the editor subsystem (keeps selection/layers/typed-element registry
    // consistent). No silent fallback to Actor->Destroy().
    if (!DestroyActorsViaEditorSubsystem(ToDelete))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to delete one or more actors via the editor subsystem"));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("deleted"), DeletedArray);
    ResultObj->SetNumberField(TEXT("count"), DeletedArray.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCreateLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString MapPath;
    if (!Params->TryGetStringField(TEXT("map_path"), MapPath) || MapPath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'map_path' parameter"));
    }
    bool bOverwrite = false;
    if (Params->HasField(TEXT("overwrite"))) { bOverwrite = Params->GetBoolField(TEXT("overwrite")); }
    FString TemplatePath;
    Params->TryGetStringField(TEXT("template"), TemplatePath);

    ULevelEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    if (!Subsystem)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Level editor subsystem unavailable"));
    }
    if (!bOverwrite && UEditorAssetLibrary::DoesAssetExist(MapPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Level already exists: %s (set overwrite=true)"), *MapPath));
    }

    const bool bUseTemplate = !TemplatePath.IsEmpty() && !TemplatePath.Equals(TEXT("empty"), ESearchCase::IgnoreCase);
    const bool bCreated = bUseTemplate ? Subsystem->NewLevelFromTemplate(MapPath, TemplatePath) : Subsystem->NewLevel(MapPath);
    if (!bCreated)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to create level: %s"), *MapPath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("map_path"), MapPath);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSaveLevel(const TSharedPtr<FJsonObject>& Params)
{
    ULevelEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    if (!Subsystem)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Level editor subsystem unavailable"));
    }

    FString DestinationPath;
    const bool bHasDestination = Params->TryGetStringField(TEXT("destination_path"), DestinationPath) && !DestinationPath.IsEmpty();

    bool bSaved = false;
    if (bHasDestination)
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        bSaved = World ? UEditorLoadingAndSavingUtils::SaveMap(World, DestinationPath) : false;
    }
    else
    {
        bSaved = Subsystem->SaveCurrentLevel();
    }

    if (!bSaved)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to save level"));
    }
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    if (bHasDestination) { ResultObj->SetStringField(TEXT("destination_path"), DestinationPath); }
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleLoadLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString MapPath;
    if (!Params->TryGetStringField(TEXT("map_path"), MapPath) || MapPath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'map_path' parameter"));
    }
    ULevelEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    if (!Subsystem)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Level editor subsystem unavailable"));
    }
    if (!Subsystem->LoadLevel(MapPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load level: %s"), *MapPath));
    }
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("map_path"), MapPath);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleDeleteLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString MapPath;
    if (!Params->TryGetStringField(TEXT("map_path"), MapPath) || MapPath.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'map_path' parameter"));
    }
    bool bForce = false;
    if (Params->HasField(TEXT("force"))) { bForce = Params->GetBoolField(TEXT("force")); }

    if (!UEditorAssetLibrary::DoesAssetExist(MapPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Level not found: %s"), *MapPath));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    const FString CurrentPackage = World ? World->GetOutermost()->GetName() : FString();
    if (!bForce && !CurrentPackage.IsEmpty() && MapPath.Contains(CurrentPackage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Refusing to delete the currently open level (set force=true)"));
    }

    if (!UEditorAssetLibrary::DeleteAsset(MapPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to delete level: %s"), *MapPath));
    }
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("map_path"), MapPath);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetViewportCamera(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params->HasField(TEXT("location")) || !Params->HasField(TEXT("rotation")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Both 'location' and 'rotation' are required"));
    }
    const FVector Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    const FRotator Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));

    FLevelEditorViewportClient* ViewportClient = (GEditor && GEditor->GetActiveViewport()) ? (FLevelEditorViewportClient*)GEditor->GetActiveViewport()->GetClient() : nullptr;
    if (!ViewportClient)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get active viewport"));
    }
    ViewportClient->SetViewLocation(Location);
    ViewportClient->SetViewRotation(Rotation);
    if (Params->HasField(TEXT("game_view")))
    {
        ViewportClient->SetGameView(Params->GetBoolField(TEXT("game_view")));
    }
    ViewportClient->Invalidate();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCaptureViewportScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    FString FileName = TEXT("MCP_Screenshot.png");
    Params->TryGetStringField(TEXT("filename"), FileName);
    if (!FileName.EndsWith(TEXT(".png"))) { FileName += TEXT(".png"); }

    FString FilePath = FileName;
    if (FPaths::IsRelative(FilePath))
    {
        FilePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);
    }

    if (!GEditor || !GEditor->GetActiveViewport())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get active viewport"));
    }
    FViewport* Viewport = GEditor->GetActiveViewport();
    TArray<FColor> Bitmap;
    FIntRect ViewportRect(0, 0, Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y);
    if (!Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), ViewportRect))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to read viewport pixels"));
    }
    TArray<uint8> CompressedBitmap;
    FImageUtils::CompressImageArray(Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y, Bitmap, CompressedBitmap);
    if (!FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to save screenshot: %s"), *FilePath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("filepath"), FilePath);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleBatchExecute(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
    if (!Params->TryGetArrayField(TEXT("actions"), Actions) || !Actions)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actions' array parameter"));
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Failures = 0;
    for (const TSharedPtr<FJsonValue>& ActionValue : *Actions)
    {
        const TSharedPtr<FJsonObject>* ActionObj = nullptr;
        if (!ActionValue.IsValid() || !ActionValue->TryGetObject(ActionObj) || !ActionObj) { continue; }

        FString SubCommand;
        if (!(*ActionObj)->TryGetStringField(TEXT("type"), SubCommand))
        {
            (*ActionObj)->TryGetStringField(TEXT("command"), SubCommand);
        }
        TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
        const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
        if ((*ActionObj)->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj)
        {
            SubParams = *ParamsObj;
        }

        TSharedPtr<FJsonObject> SubResult = HandleCommand(SubCommand, SubParams);
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("command"), SubCommand);
        if (SubResult.IsValid())
        {
            const bool bOk = !SubResult->HasField(TEXT("error"));
            if (!bOk) { Failures++; }
            Entry->SetBoolField(TEXT("success"), bOk);
            Entry->SetObjectField(TEXT("result"), SubResult);
        }
        Results.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("results"), Results);
    ResultObj->SetNumberField(TEXT("count"), Results.Num());
    ResultObj->SetNumberField(TEXT("failures"), Failures);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleExecutePython(const TSharedPtr<FJsonObject>& Params)
{
    FString Code;
    if (!Params->TryGetStringField(TEXT("code"), Code))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'code' parameter"));
    }
    IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
    if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("PythonScriptPlugin is not available"));
    }

    FPythonCommandEx Command;
    Command.Command = Code;
    Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
    Command.FileExecutionScope = EPythonFileExecutionScope::Public;
    const bool bSuccess = PythonPlugin->ExecPythonCommandEx(Command);

    TArray<TSharedPtr<FJsonValue>> LogArray;
    for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
    {
        LogArray.Add(MakeShared<FJsonValueString>(Entry.Output));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), bSuccess);
    ResultObj->SetStringField(TEXT("command_result"), Command.CommandResult);
    ResultObj->SetArrayField(TEXT("output"), LogArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleImportAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString DestinationPath = TEXT("/Game");
    Params->TryGetStringField(TEXT("destination_path"), DestinationPath);

    bool bReplaceExisting = true;
    Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

    TArray<FString> Sources;
    const TArray<TSharedPtr<FJsonValue>>* SourcesArr = nullptr;
    if (Params->TryGetArrayField(TEXT("sources"), SourcesArr) && SourcesArr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *SourcesArr)
        {
            if (Value.IsValid()) { Sources.Add(Value->AsString()); }
        }
    }
    else
    {
        FString Single;
        if (Params->TryGetStringField(TEXT("source"), Single)) { Sources.Add(Single); }
    }
    if (Sources.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'sources' (array) or 'source' (string) parameter"));
    }

    TSharedPtr<FJsonObject> Options;
    const TSharedPtr<FJsonObject>* OptionsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("options"), OptionsObj) && OptionsObj)
    {
        Options = *OptionsObj;
    }

    const FString JobId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    {
        FScopeLock Lock(&GImportJobsMutex);
        TSharedPtr<FImportJobState> Job = MakeShared<FImportJobState>();
        Job->State = TEXT("queued");
        GImportJobs.Add(JobId, Job);
    }

    // Defer the actual import to the next core-ticker tick so it does NOT run inside the
    // game-thread task that dispatched this command (which would trip the importer's
    // nested task-graph assertion). Returns immediately with a job id.
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [JobId, Sources, DestinationPath, bReplaceExisting, Options](float) -> bool
            {
                RunImportJob(JobId, Sources, DestinationPath, bReplaceExisting, Options);
                return false; // one-shot
            }),
        0.0f);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("job_id"), JobId);
    ResultObj->SetStringField(TEXT("state"), TEXT("queued"));
    ResultObj->SetNumberField(TEXT("count"), Sources.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetImportStatus(const TSharedPtr<FJsonObject>& Params)
{
    FString JobId;
    if (!Params->TryGetStringField(TEXT("job_id"), JobId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'job_id' parameter"));
    }

    TSharedPtr<FImportJobState> Job;
    {
        FScopeLock Lock(&GImportJobsMutex);
        TSharedPtr<FImportJobState>* Found = GImportJobs.Find(JobId);
        if (!Found)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown job_id: %s"), *JobId));
        }
        Job = *Found;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("job_id"), JobId);
    ResultObj->SetStringField(TEXT("state"), Job->State);
    ResultObj->SetStringField(TEXT("error"), Job->Error);

    TArray<TSharedPtr<FJsonValue>> AssetsArr;
    for (const FString& AssetPath : Job->Assets) { AssetsArr.Add(MakeShared<FJsonValueString>(AssetPath)); }
    ResultObj->SetArrayField(TEXT("assets"), AssetsArr);

    TArray<TSharedPtr<FJsonValue>> LogArr;
    for (const FString& Line : Job->Log) { LogArr.Add(MakeShared<FJsonValueString>(Line)); }
    ResultObj->SetArrayField(TEXT("log"), LogArr);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params)
{
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    TArray<TSharedPtr<FJsonValue>> ActorArray;
    for (AActor* Actor : AllActors)
    {
        if (Actor)
        {
            ActorArray.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params)
{
    FString Pattern;
    if (!Params->TryGetStringField(TEXT("pattern"), Pattern))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'pattern' parameter"));
    }
    
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    TArray<TSharedPtr<FJsonValue>> MatchingActors;
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName().Contains(Pattern))
        {
            MatchingActors.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), MatchingActors);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActorType;
    if (!Params->TryGetStringField(TEXT("type"), ActorType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'type' parameter"));
    }

    // Get actor name (required parameter)
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Get optional transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    // Create the actor based on type
    AActor* NewActor = nullptr;
    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    // Check if an actor with this name already exists
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor with name '%s' already exists"), *ActorName));
        }
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;

    if (ActorType == TEXT("StaticMeshActor"))
    {
        NewActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("PointLight"))
    {
        NewActor = World->SpawnActor<APointLight>(APointLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("SpotLight"))
    {
        NewActor = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("DirectionalLight"))
    {
        NewActor = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("CameraActor"))
    {
        NewActor = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), Location, Rotation, SpawnParams);
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown actor type: %s"), *ActorType));
    }

    if (NewActor)
    {
        // Set scale (since SpawnActor only takes location and rotation)
        FTransform Transform = NewActor->GetTransform();
        Transform.SetScale3D(Scale);
        NewActor->SetActorTransform(Transform);

        // Return the created actor's details
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleDeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            // Store actor info before deletion for the response
            TSharedPtr<FJsonObject> ActorInfo = FUnrealMCPCommonUtils::ActorToJsonObject(Actor);
            
            // Delete the actor via the editor subsystem (keeps selection/layers/typed-element
            // registry consistent). No silent fallback to Actor->Destroy().
            TArray<AActor*> ActorsToDestroy;
            ActorsToDestroy.Add(Actor);
            if (!DestroyActorsViaEditorSubsystem(ActorsToDestroy))
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to delete actor via editor subsystem: %s"), *ActorName));
            }
            
            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetObjectField(TEXT("deleted_actor"), ActorInfo);
            return ResultObj;
        }
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Get transform parameters
    FTransform NewTransform = TargetActor->GetTransform();

    if (Params->HasField(TEXT("location")))
    {
        NewTransform.SetLocation(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        NewTransform.SetRotation(FQuat(FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"))));
    }
    if (Params->HasField(TEXT("scale")))
    {
        NewTransform.SetScale3D(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
    }

    // Set the new transform
    TargetActor->SetActorTransform(NewTransform);

    // Return updated actor info
    return FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Always return detailed properties for this command
    return FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Get property name
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    // Get property value
    if (!Params->HasField(TEXT("property_value")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
    }
    
    TSharedPtr<FJsonValue> PropertyValue = Params->Values.FindRef(TEXT("property_value"));
    
    // Set the property using our utility function
    FString ErrorMessage;
    if (FUnrealMCPCommonUtils::SetObjectProperty(TargetActor, PropertyName, PropertyValue, ErrorMessage))
    {
        // Property set successfully
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("actor"), ActorName);
        ResultObj->SetStringField(TEXT("property"), PropertyName);
        ResultObj->SetBoolField(TEXT("success"), true);
        
        // Also include the full actor details
        ResultObj->SetObjectField(TEXT("actor_details"), FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true));
        return ResultObj;
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }

    // Find the blueprint
    if (BlueprintName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint name is empty"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    UClass* BlueprintClass = Blueprint->GeneratedClass;
    if (!BlueprintClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint '%s' has no generated class; compile it first"), *BlueprintName));
    }

    // Get transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    // Spawn the actor
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FTransform SpawnTransform;
    SpawnTransform.SetLocation(Location);
    SpawnTransform.SetRotation(FQuat(Rotation));
    SpawnTransform.SetScale3D(Scale);

    FActorSpawnParameters SpawnParams;
    // Do NOT force SpawnParams.Name: in the editor world a name clash raises a
    // modal "name already in use" dialog on the game thread, which blocks the
    // bridge and makes the command time out. Let UE pick a unique name and set
    // the display label afterwards instead.
    AActor* NewActor = World->SpawnActor<AActor>(BlueprintClass, SpawnTransform, SpawnParams);
    if (NewActor)
    {
        NewActor->SetActorLabel(*ActorName);
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn blueprint actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFocusViewport(const TSharedPtr<FJsonObject>& Params)
{
    // Get target actor name if provided
    FString TargetActorName;
    bool HasTargetActor = Params->TryGetStringField(TEXT("target"), TargetActorName);

    // Get location if provided
    FVector Location(0.0f, 0.0f, 0.0f);
    bool HasLocation = false;
    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
        HasLocation = true;
    }

    // Get distance
    float Distance = 1000.0f;
    if (Params->HasField(TEXT("distance")))
    {
        Distance = Params->GetNumberField(TEXT("distance"));
    }

    // Get orientation if provided
    FRotator Orientation(0.0f, 0.0f, 0.0f);
    bool HasOrientation = false;
    if (Params->HasField(TEXT("orientation")))
    {
        Orientation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("orientation"));
        HasOrientation = true;
    }

    // Get the active viewport
    FLevelEditorViewportClient* ViewportClient = (FLevelEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
    if (!ViewportClient)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get active viewport"));
    }

    // If we have a target actor, focus on it
    if (HasTargetActor)
    {
        // Find the actor
        AActor* TargetActor = nullptr;
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
        
        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetName() == TargetActorName)
            {
                TargetActor = Actor;
                break;
            }
        }

        if (!TargetActor)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *TargetActorName));
        }

        // Focus on the actor
        ViewportClient->SetViewLocation(TargetActor->GetActorLocation() - FVector(Distance, 0.0f, 0.0f));
    }
    // Otherwise use the provided location
    else if (HasLocation)
    {
        ViewportClient->SetViewLocation(Location - FVector(Distance, 0.0f, 0.0f));
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Either 'target' or 'location' must be provided"));
    }

    // Set orientation if provided
    if (HasOrientation)
    {
        ViewportClient->SetViewRotation(Orientation);
    }

    // Force viewport to redraw
    ViewportClient->Invalidate();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    // Get file path parameter
    FString FilePath;
    if (!Params->TryGetStringField(TEXT("filepath"), FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'filepath' parameter"));
    }
    
    // Ensure the file path has a proper extension
    if (!FilePath.EndsWith(TEXT(".png")))
    {
        FilePath += TEXT(".png");
    }

    // Get the active viewport
    if (GEditor && GEditor->GetActiveViewport())
    {
        FViewport* Viewport = GEditor->GetActiveViewport();
        TArray<FColor> Bitmap;
        FIntRect ViewportRect(0, 0, Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y);
        
        if (Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), ViewportRect))
        {
            TArray<uint8> CompressedBitmap;
            FImageUtils::CompressImageArray(Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y, Bitmap, CompressedBitmap);
            
            if (FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath))
            {
                TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
                ResultObj->SetStringField(TEXT("filepath"), FilePath);
                return ResultObj;
            }
        }
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to take screenshot"));
} 