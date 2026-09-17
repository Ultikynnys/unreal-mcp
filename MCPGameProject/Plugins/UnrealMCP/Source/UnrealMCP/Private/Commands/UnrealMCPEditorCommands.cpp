#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "JsonObjectConverter.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
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
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "RenderingThread.h"

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

    // ---------------------------------------------------------------------
    // Async blueprint-plan jobs (polled via get_plan_status)
    //
    // apply_blueprint_plan parses a plan file, records a job here, and returns a
    // job id immediately. A core-ticker lambda then applies the plan a chunk at a
    // time on the game thread, so a large plan never blocks the MCP socket's
    // response and never runs inside the dispatching game-thread task.
    // ---------------------------------------------------------------------

    struct FBlueprintPlanJobState
    {
        FString State;                  // queued | running | done | failed
        FString Phase;                  // clearing | creating | connecting | defaults | finished
        FString BlueprintName;
        FString GraphName;

        TArray<TSharedPtr<FJsonObject>> NodeOps;    // plan.nodes
        TArray<TSharedPtr<FJsonObject>> EdgeOps;    // plan.edges
        TArray<TSharedPtr<FJsonObject>> DefaultOps; // plan.defaults

        int32 NodeCursor = 0;
        int32 EdgeCursor = 0;
        int32 DefaultCursor = 0;

        int32 Total = 0;                // NodeOps + EdgeOps + DefaultOps
        int32 Applied = 0;

        TMap<FString, FString> Refs;                // plan ref -> created node guid
        TArray<TSharedPtr<FJsonValue>> Failures;    // [{ "ref"/"op": ..., "error": ... }]

        bool bClear = false;
        bool bCleared = false;

        // Auto-layout: node positions come from NodePositions (computed from topology)
        // instead of each op's own "pos", and multi-column edges route through knot
        // chains (EdgeKnots) instead of one long wire.
        bool bAutoLayout = false;
        float ColGap = 36.0f;
        float RowGap = 48.0f;
        FVector2D Origin = FVector2D::ZeroVector;
        bool bPlaced = false;
        TArray<FVector2D> NodePositions;            // per NodeOps index (final, after measuring)
        TArray<TArray<FVector2D>> EdgeKnots;        // per EdgeOps index: intermediate knot positions
    };

    TMap<FString, TSharedPtr<FBlueprintPlanJobState>> GPlanJobs;
    FCriticalSection GPlanJobsMutex;

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
    else if (CommandType == TEXT("capture_pie_screenshot")) { return HandleCapturePIEScreenshot(Params); }
    else if (CommandType == TEXT("batch_execute")) { return HandleBatchExecute(Params); }
    else if (CommandType == TEXT("execute_python")) { return HandleExecutePython(Params); }
    else if (CommandType == TEXT("import_asset")) { return HandleImportAsset(Params); }
    else if (CommandType == TEXT("get_import_status")) { return HandleGetImportStatus(Params); }
    else if (CommandType == TEXT("apply_blueprint_plan")) { return HandleApplyBlueprintPlan(Params); }
    else if (CommandType == TEXT("get_plan_status")) { return HandleGetPlanStatus(Params); }
    else if (CommandType == TEXT("recover_editor")) { return HandleRecoverEditor(Params); }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown editor command: %s"), *CommandType));
}

// Recover from the "restore unsaved files" crash-recovery prompt that stalls startup after
// an abnormal shutdown. That prompt is driven by Saved/Autosaves/PackageRestoreData.json, so
// we (1) delete the recovery state so it cannot recur, then (2) dismiss the now-stale
// recovery modal so the core ticker resumes. Runs on the game thread via the bridge's
// AsyncTask, which still dispatches while a modal is up.
TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleRecoverEditor(const TSharedPtr<FJsonObject>& Params)
{
    int32 FilesRemoved = 0;
    const FString AutoDir = FPaths::ProjectSavedDir() / TEXT("Autosaves");
    IFileManager& FM = IFileManager::Get();

    const FString RestoreData = AutoDir / TEXT("PackageRestoreData.json");
    if (FM.FileExists(*RestoreData))
    {
        FM.Delete(*RestoreData, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
        ++FilesRemoved;
    }

    // Dismiss every active modal window (the recovery dialog). There is usually one.
    int32 ModalsDismissed = 0;
    if (FSlateApplication::IsInitialized())
    {
        for (int32 Guard = 0; Guard < 16; ++Guard)
        {
            TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
            if (!Modal.IsValid()) { break; }
            Modal->RequestDestroyWindow();
            ++ModalsDismissed;
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetNumberField(TEXT("restore_data_removed"), FilesRemoved);
    ResultObj->SetNumberField(TEXT("modals_dismissed"), ModalsDismissed);
    return ResultObj;
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
        TEXT("take_screenshot"), TEXT("capture_viewport_screenshot"), TEXT("capture_pie_screenshot"), TEXT("set_viewport_camera"),
        TEXT("create_level"), TEXT("save_level"), TEXT("load_level"), TEXT("delete_level"),
        TEXT("query_assets"), TEXT("get_asset_details"), TEXT("get_capabilities"),
        TEXT("batch_execute"), TEXT("execute_python"), TEXT("reload_server"),
        TEXT("import_asset"), TEXT("get_import_status"),
        TEXT("apply_blueprint_plan"), TEXT("get_plan_status"),
        TEXT("delete_blueprint_node"), TEXT("clear_blueprint_graph"),
        TEXT("disconnect_blueprint_pin"), TEXT("get_blueprint_graphs"),
        TEXT("set_blueprint_node_pin_default"),
        TEXT("get_blueprint_node_bounds"),
        TEXT("auto_layout_blueprint_graph"),
        TEXT("recover_editor")
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
    // A duplicate name must auto-rename, NOT fatal: FActorSpawnParameters defaults NameMode to
    // Required_Fatal, which crashes the editor when the supplied name is already in use.
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
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
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
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
            SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
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

    AActor* TargetActor = FUnrealMCPCommonUtils::ResolveActor(ActorName);
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

    AActor* TargetActor = FUnrealMCPCommonUtils::ResolveActor(ActorName);
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
    UGameplayStatics::GetAllActorsOfClass(FUnrealMCPCommonUtils::GetEditorWorld(), AActor::StaticClass(), AllActors);

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

    // Normalise the requested path to a package name and compare exactly. The old
    // substring test falsely matched sibling levels (e.g. ".../Main" vs ".../Main2").
    FString MapPackage = MapPath;
    int32 DotIndex = INDEX_NONE;
    if (MapPackage.FindChar(TEXT('.'), DotIndex)) { MapPackage = MapPackage.Left(DotIndex); }
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    const FString CurrentPackage = World ? World->GetOutermost()->GetName() : FString();
    if (!bForce && !CurrentPackage.IsEmpty() && MapPackage.Equals(CurrentPackage, ESearchCase::IgnoreCase))
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
    FImageUtils::ThumbnailCompressImageArray(Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y, Bitmap, CompressedBitmap);
    if (!FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to save screenshot: %s"), *FilePath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("filepath"), FilePath);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCapturePIEScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    // Render the PLAY world, not the editor world - requires an active PIE session.
    UWorld* PIEWorld = GEditor ? GEditor->PlayWorld : nullptr;
    if (!PIEWorld)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("PIE is not running (no play world). Start a Play-In-Editor session first."));
    }

    // Resolve the camera transform: explicit location/rotation, else the PIE player's view point.
    const bool bHasLocation = Params->HasField(TEXT("location"));
    const bool bHasRotation = Params->HasField(TEXT("rotation"));
    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    if (bHasLocation) { Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")); }
    if (bHasRotation) { Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")); }
    if (!bHasLocation || !bHasRotation)
    {
        APlayerController* PC = PIEWorld->GetFirstPlayerController();
        if (!PC)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No location/rotation supplied and no PIE player controller is available"));
        }
        FVector PlayerLocation = Location;
        FRotator PlayerRotation = Rotation;
        PC->GetPlayerViewPoint(PlayerLocation, PlayerRotation);
        if (!bHasLocation) { Location = PlayerLocation; }
        if (!bHasRotation) { Rotation = PlayerRotation; }
    }

    int32 Width = 1280;
    int32 Height = 720;
    if (Params->HasField(TEXT("width")))  { Width  = (int32)Params->GetNumberField(TEXT("width")); }
    if (Params->HasField(TEXT("height"))) { Height = (int32)Params->GetNumberField(TEXT("height")); }
    Width  = FMath::Clamp(Width, 16, 4096);
    Height = FMath::Clamp(Height, 16, 4096);

    FString FileName = TEXT("MCP_PIE_Screenshot.png");
    Params->TryGetStringField(TEXT("filename"), FileName);
    if (!FileName.EndsWith(TEXT(".png"))) { FileName += TEXT(".png"); }
    FString FilePath = FileName;
    if (FPaths::IsRelative(FilePath))
    {
        FilePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);
    }

    // Transient scene capture placed in the PIE world at the requested transform.
    FActorSpawnParameters SpawnParams;
    SpawnParams.ObjectFlags |= RF_Transient;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ASceneCapture2D* CaptureActor = PIEWorld->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), Location, Rotation, SpawnParams);
    if (!CaptureActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn SceneCapture2D in the PIE world"));
    }
    USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();

    UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
    RenderTarget->RenderTargetFormat = RTF_RGBA8;
    RenderTarget->ClearColor = FLinearColor::Black;
    RenderTarget->bAutoGenerateMips = false;
    RenderTarget->InitAutoFormat(Width, Height);

    Capture->TextureTarget = RenderTarget;
    Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    if (Params->HasField(TEXT("fov"))) { Capture->FOVAngle = (float)Params->GetNumberField(TEXT("fov")); }
    Capture->ShowFlags.SetAntiAliasing(true);
    Capture->ShowFlags.SetMotionBlur(false);

    Capture->CaptureScene();
    FlushRenderingCommands();

    TArray<FColor> Pixels;
    FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
    const bool bRead = RTResource && RTResource->ReadPixels(Pixels, FReadSurfaceDataFlags()) && Pixels.Num() == Width * Height;

    CaptureActor->Destroy();

    if (!bRead)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to read pixels from the PIE render target"));
    }

    TArray<uint8> CompressedBitmap;
    FImageUtils::ThumbnailCompressImageArray(Width, Height, Pixels, CompressedBitmap);
    if (!FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to save PIE screenshot: %s"), *FilePath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("filepath"), FilePath);
    ResultObj->SetStringField(TEXT("world"), TEXT("PIE"));
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ResultObj->SetArrayField(TEXT("location"), LocationArray);
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ResultObj->SetArrayField(TEXT("rotation"), RotationArray);
    ResultObj->SetNumberField(TEXT("width"), Width);
    ResultObj->SetNumberField(TEXT("height"), Height);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleBatchExecute(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
    if (!Params->TryGetArrayField(TEXT("actions"), Actions) || !Actions)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actions' array parameter"));
    }

    FString Description = TEXT("MCP Batch Operation");
    Params->TryGetStringField(TEXT("description"), Description);
    bool bRollbackOnFailure = true;
    Params->TryGetBoolField(TEXT("rollback_on_failure"), bRollbackOnFailure);

    // Deterministic rollback. The editor undo stack does NOT revert changes applied over
    // the socket bridge (verified), so instead of relying on CancelTransaction we snapshot
    // every actor and component up front and restore the snapshot if we have to roll back.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TSet<AActor*> ActorsBefore;
    TArray<AActor*> ActorRefs;
    TArray<TSharedPtr<FJsonObject>> ActorSnapshots;
    TArray<UActorComponent*> ComponentRefs;
    TArray<TSharedPtr<FJsonObject>> ComponentSnapshots;
    // Snapshot only when a rollback is actually possible AND there is work to do (an
    // empty batch can neither fail nor change anything). Callers that do not need
    // rollback should pass rollback_on_failure=false to skip the whole-level snapshot.
    if (bRollbackOnFailure && World && Actions->Num() > 0)
    {
        TArray<AActor*> ExistingActors;
        UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), ExistingActors);
        for (AActor* Actor : ExistingActors)
        {
            if (!Actor) { continue; }
            ActorsBefore.Add(Actor);

            TSharedRef<FJsonObject> ActorSnapshot = MakeShared<FJsonObject>();
            if (FJsonObjectConverter::UStructToJsonObject(Actor->GetClass(), Actor, ActorSnapshot, 0, 0))
            {
                ActorRefs.Add(Actor);
                ActorSnapshots.Add(ActorSnapshot);
            }
            for (UActorComponent* Component : Actor->GetComponents())
            {
                if (!Component) { continue; }
                TSharedRef<FJsonObject> ComponentSnapshot = MakeShared<FJsonObject>();
                if (FJsonObjectConverter::UStructToJsonObject(Component->GetClass(), Component, ComponentSnapshot, 0, 0))
                {
                    ComponentRefs.Add(Component);
                    ComponentSnapshots.Add(ComponentSnapshot);
                }
            }
        }
    }

    // A transaction is still opened so that handlers which record undo (e.g. blueprint
    // graph ops) participate; it is not relied on for the rollback below.
    int32 TransactionIndex = INDEX_NONE;
    if (GEditor)
    {
        TransactionIndex = GEditor->BeginTransaction(FText::FromString(Description));
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

        // Route through the bridge-injected full-command router when present so a
        // batch can drive blueprint-node / umg / project commands too; otherwise
        // fall back to this handler's own (editor-only) command table.
        TSharedPtr<FJsonObject> SubResult = SubCommandRouter
            ? SubCommandRouter(SubCommand, SubParams)
            : HandleCommand(SubCommand, SubParams);
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

    // Close out the batch: on a requested rollback, restore every actor/component snapshot
    // taken before the batch and destroy any actor the batch spawned; otherwise commit.
    const bool bRolledBack = (bRollbackOnFailure && Failures > 0);
    if (bRolledBack)
    {
        for (int32 i = 0; i < ActorRefs.Num(); ++i)
        {
            if (AActor* Actor = ActorRefs[i])
            {
                FJsonObjectConverter::JsonObjectToUStruct(ActorSnapshots[i].ToSharedRef(), Actor->GetClass(), Actor, 0, 0);
            }
        }
        for (int32 i = 0; i < ComponentRefs.Num(); ++i)
        {
            if (UActorComponent* Component = ComponentRefs[i])
            {
                FJsonObjectConverter::JsonObjectToUStruct(ComponentSnapshots[i].ToSharedRef(), Component->GetClass(), Component, 0, 0);
                // Restoring properties does not recompute the cached transform; without this
                // the relative location reverts but the actor still reports its moved position.
                if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
                {
                    SceneComponent->UpdateComponentToWorld();
                }
            }
        }
        if (World)
        {
            TArray<AActor*> CurrentActors;
            UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), CurrentActors);
            for (AActor* Actor : CurrentActors)
            {
                if (Actor && !ActorsBefore.Contains(Actor))
                {
                    Actor->Destroy();
                }
            }
        }
        if (TransactionIndex != INDEX_NONE && GEditor) { GEditor->CancelTransaction(TransactionIndex); }
    }
    else if (TransactionIndex != INDEX_NONE && GEditor)
    {
        GEditor->EndTransaction();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("results"), Results);
    ResultObj->SetNumberField(TEXT("count"), Results.Num());
    ResultObj->SetNumberField(TEXT("failures"), Failures);
    ResultObj->SetBoolField(TEXT("rolled_back"), bRolledBack);
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

// apply_blueprint_plan: build a whole graph from a plan file in one call.
//
// Plan schema:
//   {
//     "nodes":    [ {"ref":"n3","op":"node","node_type":"variable_get","params":{...},"pos":[x,y]},
//                   {"ref":"n4","op":"function","function_name":"Reset","target":"...","params":{...},"pos":[x,y]},
//                   {"ref":"n5","op":"component_ref","component_name":"Mesh","pos":[x,y]},
//                   {"ref":"k1","op":"reroute","pos":[x,y]} ],
//     "edges":    [ {"s":"n3","sp":"OutPin","t":"n4","tp":"InPin"} ],
//     "defaults": [ {"ref":"n4","pin":"B","value":1} ]
//   }
// Each op is routed through the shared command router (SubCommandRouter), so this
// reuses the real handlers (add_blueprint_node / add_blueprint_function_node /
// add_blueprint_get_self_component_reference / add_blueprint_reroute_node /
// connect_blueprint_nodes / set_blueprint_node_pin_default) rather than
// duplicating node-creation logic. Refs created here are recorded and resolved for
// edges/defaults, so the plan wires symbolically without a GUID round-trip.
TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleApplyBlueprintPlan(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // The plan itself: a path to a JSON file on disk (preferred for large plans)
    // or an inline object.
    TSharedPtr<FJsonObject> Plan;
    FString PlanPath;
    if (Params->TryGetStringField(TEXT("plan_path"), PlanPath) && !PlanPath.IsEmpty())
    {
        FString PlanText;
        if (!FFileHelper::LoadFileToString(PlanText, *PlanPath))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Could not read plan file: %s"), *PlanPath));
        }
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PlanText);
        if (!FJsonSerializer::Deserialize(Reader, Plan) || !Plan.IsValid())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Could not parse plan JSON: %s"), *PlanPath));
        }
    }
    else
    {
        const TSharedPtr<FJsonObject>* PlanObj = nullptr;
        if (Params->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj)
        {
            Plan = *PlanObj;
        }
    }
    if (!Plan.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'plan_path' (file) or 'plan' (object) parameter"));
    }

    TSharedPtr<FBlueprintPlanJobState> Job = MakeShared<FBlueprintPlanJobState>();
    Job->State = TEXT("queued");
    Job->Phase = TEXT("queued");
    Job->BlueprintName = BlueprintName;
    Job->GraphName = GraphName;
    Params->TryGetBoolField(TEXT("clear"), Job->bClear);

    auto CollectOps = [](const TSharedPtr<FJsonObject>& PlanObj, const TCHAR* Field, TArray<TSharedPtr<FJsonObject>>& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!PlanObj->TryGetArrayField(Field, Arr) || !Arr) { return; }
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            const TSharedPtr<FJsonObject>* O = nullptr;
            if (V.IsValid() && V->TryGetObject(O) && O) { Out.Add(*O); }
        }
    };
    CollectOps(Plan, TEXT("nodes"), Job->NodeOps);
    CollectOps(Plan, TEXT("edges"), Job->EdgeOps);
    CollectOps(Plan, TEXT("defaults"), Job->DefaultOps);
    Job->Total = Job->NodeOps.Num() + Job->EdgeOps.Num() + Job->DefaultOps.Num();

    bool bAutoLayoutParam = false;
    Params->TryGetBoolField(TEXT("auto_layout"), bAutoLayoutParam);
    if (bAutoLayoutParam && Job->NodeOps.Num() > 0)
    {
        // Defer placement to the placing phase: the nodes are created first, then their REAL
        // sizes are measured and the layout runs over those. Estimating sizes before creation
        // (the old flat 220x100) undercounted tall nodes -- e.g. Print String ~270 -- and
        // produced colliding slots. Only the origin and gaps are resolved here.
        float ColGap = 36.0f;
        if (Params->HasField(TEXT("col_gap"))) { ColGap = (float)Params->GetNumberField(TEXT("col_gap")); }
        float RowGap = 48.0f;
        if (Params->HasField(TEXT("row_gap"))) { RowGap = (float)Params->GetNumberField(TEXT("row_gap")); }
        float OriginX = 0.0f;
        float OriginY = 0.0f;
        if (Params->HasField(TEXT("origin_x"))) { OriginX = (float)Params->GetNumberField(TEXT("origin_x")); }
        if (Params->HasField(TEXT("origin_y"))) { OriginY = (float)Params->GetNumberField(TEXT("origin_y")); }

        // If the graph already has nodes (e.g. the template BeginPlay, or a function entry)
        // and we are not clearing it, start the layout below them so column 0 cannot collide.
        if (!Job->bClear)
        {
            UBlueprint* ExistingBP = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
            UEdGraph* ExistingGraph = ExistingBP ? FUnrealMCPCommonUtils::FindGraphByName(ExistingBP, GraphName) : nullptr;
            if (ExistingGraph)
            {
                FVector2D EMin, EMax;
                int32 ECount = 0;
                FUnrealMCPCommonUtils::ComputeGraphBounds(ExistingGraph, EMin, EMax, ECount, true);
                if (ECount > 0)
                {
                    if (!Params->HasField(TEXT("origin_x"))) { OriginX = EMin.X; }
                    if (!Params->HasField(TEXT("origin_y"))) { OriginY = EMax.Y + 400.0f; }
                }
            }
        }

        Job->bAutoLayout = true;
        Job->ColGap = ColGap;
        Job->RowGap = RowGap;
        Job->Origin = FVector2D(OriginX, OriginY);
    }

    const FString JobId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    {
        FScopeLock Lock(&GPlanJobsMutex);
        GPlanJobs.Add(JobId, Job);
    }

    bool bAsync = true;
    Params->TryGetBoolField(TEXT("async"), bAsync);

    if (!bAsync)
    {
        // Small plans only: apply inline and report the final status in the reply.
        // (A large plan must be async — the socket reply caps at ~5s.)
        while (RunBlueprintPlanChunk(JobId, TNumericLimits<int32>::Max())) {}
    }
    else
    {
        // Defer to the core ticker: a chunk per tick, on the game thread, outside
        // the dispatching task. Returns immediately so the socket never times out.
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [this, JobId](float) -> bool
                {
                    return RunBlueprintPlanChunk(JobId, 40);
                }),
            0.0f);
    }

    FString FinalState = TEXT("queued");
    {
        FScopeLock Lock(&GPlanJobsMutex);
        TSharedPtr<FBlueprintPlanJobState>* Found = GPlanJobs.Find(JobId);
        if (Found) { FinalState = (*Found)->State; }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("job_id"), JobId);
    ResultObj->SetNumberField(TEXT("total"), Job->Total);
    ResultObj->SetStringField(TEXT("state"), FinalState);
    return ResultObj;
}

// Applies up to Budget ops of a plan job. Returns true while work remains, so the
// core-ticker lambda can reschedule itself. Must run on the game thread.
bool FUnrealMCPEditorCommands::RunBlueprintPlanChunk(const FString& JobId, int32 Budget)
{
    TSharedPtr<FBlueprintPlanJobState> Job;
    {
        FScopeLock Lock(&GPlanJobsMutex);
        TSharedPtr<FBlueprintPlanJobState>* Found = GPlanJobs.Find(JobId);
        if (!Found) { return false; }
        Job = *Found;
    }

    if (!SubCommandRouter)
    {
        Job->State = TEXT("failed");
        Job->Phase = TEXT("finished");
        TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
        FailObj->SetStringField(TEXT("op"), TEXT("job"));
        FailObj->SetStringField(TEXT("error"), TEXT("No command router injected; cannot apply plan."));
        Job->Failures.Add(MakeShared<FJsonValueObject>(FailObj));
        return false;
    }

    Job->State = TEXT("running");

    auto bOk = [](const TSharedPtr<FJsonObject>& Res) -> bool
    {
        return Res.IsValid() && !Res->HasField(TEXT("error"));
    };
    auto RecordFailure = [&Job](const FString& What, const TSharedPtr<FJsonObject>& Res)
    {
        FString Err = TEXT("unknown error");
        if (Res.IsValid() && Res->HasField(TEXT("error"))) { Err = Res->GetStringField(TEXT("error")); }
        TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
        FailObj->SetStringField(TEXT("op"), What);
        FailObj->SetStringField(TEXT("error"), Err);
        Job->Failures.Add(MakeShared<FJsonValueObject>(FailObj));
    };
    // A ref we never created is passed through verbatim, so a plan can wire a
    // pre-existing node by its GUID or name.
    auto Resolve = [&Job](const FString& Ref) -> FString
    {
        if (const FString* Found = Job->Refs.Find(Ref)) { return *Found; }
        return Ref;
    };

    // One-time graph clear (keeps the entry node).
    if (Job->bClear && !Job->bCleared)
    {
        Job->Phase = TEXT("clearing");
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("blueprint_name"), Job->BlueprintName);
        P->SetStringField(TEXT("graph_name"), Job->GraphName);
        TSharedPtr<FJsonObject> Res = SubCommandRouter(TEXT("clear_blueprint_graph"), P);
        if (!bOk(Res)) { RecordFailure(TEXT("clear"), Res); }
        Job->bCleared = true;
    }

    int32 BudgetLeft = Budget;

    // 1) Create nodes.
    Job->Phase = TEXT("creating");
    while (Job->NodeCursor < Job->NodeOps.Num() && BudgetLeft > 0)
    {
        TSharedPtr<FJsonObject> Op = Job->NodeOps[Job->NodeCursor];
        FString OpKind;
        Op->TryGetStringField(TEXT("op"), OpKind);
        OpKind.ToLowerInline();

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("blueprint_name"), Job->BlueprintName);
        P->SetStringField(TEXT("graph_name"), Job->GraphName);

        // Effective position: under auto-layout, create the node at a scratch slot clear of
        // existing nodes; the placing phase moves it to its real spot once sizes are known.
        // Otherwise use the op's own "pos".
        const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
        bool bHasPos = false;
        if (Job->bAutoLayout)
        {
            const FVector2D Scratch(Job->Origin.X + 1500.0f * (float)Job->NodeCursor, Job->Origin.Y + 6000.0f);
            TArray<TSharedPtr<FJsonValue>> AutoPosArr;
            AutoPosArr.Add(MakeShared<FJsonValueNumber>(Scratch.X));
            AutoPosArr.Add(MakeShared<FJsonValueNumber>(Scratch.Y));
            P->SetArrayField(TEXT("node_position"), AutoPosArr);
            P->SetArrayField(TEXT("position"), AutoPosArr);
        }
        else if (Op->TryGetArrayField(TEXT("pos"), PosArr) && PosArr)
        {
            bHasPos = true;
        }

        FString Command;
        if (OpKind == TEXT("function"))
        {
            Command = TEXT("add_blueprint_function_node");
            FString FnName; Op->TryGetStringField(TEXT("function_name"), FnName);
            P->SetStringField(TEXT("function_name"), FnName);
            FString Target;
            if (Op->TryGetStringField(TEXT("target"), Target)) { P->SetStringField(TEXT("target"), Target); }
            const TSharedPtr<FJsonObject>* Sub = nullptr;
            if (Op->TryGetObjectField(TEXT("params"), Sub) && Sub) { P->SetObjectField(TEXT("params"), *Sub); }
            if (bHasPos) { P->SetArrayField(TEXT("node_position"), *PosArr); }
        }
        else if (OpKind == TEXT("component_ref"))
        {
            Command = TEXT("add_blueprint_get_self_component_reference");
            FString Comp; Op->TryGetStringField(TEXT("component_name"), Comp);
            P->SetStringField(TEXT("component_name"), Comp);
            if (bHasPos) { P->SetArrayField(TEXT("node_position"), *PosArr); }
        }
        else if (OpKind == TEXT("reroute"))
        {
            Command = TEXT("add_blueprint_reroute_node");
            if (bHasPos) { P->SetArrayField(TEXT("position"), *PosArr); }
        }
        else
        {
            // Default: node_type dispatch (variable_get, branch, cast, ...).
            Command = TEXT("add_blueprint_node");
            FString NodeType; Op->TryGetStringField(TEXT("node_type"), NodeType);
            P->SetStringField(TEXT("node_type"), NodeType);
            const TSharedPtr<FJsonObject>* Sub = nullptr;
            if (Op->TryGetObjectField(TEXT("params"), Sub) && Sub) { P->SetObjectField(TEXT("params"), *Sub); }
            if (bHasPos) { P->SetArrayField(TEXT("node_position"), *PosArr); }
        }

        TSharedPtr<FJsonObject> Res = SubCommandRouter(Command, P);
        FString Ref; Op->TryGetStringField(TEXT("ref"), Ref);
        if (bOk(Res))
        {
            FString NodeId;
            if (Res->TryGetStringField(TEXT("node_id"), NodeId) && !Ref.IsEmpty())
            {
                Job->Refs.Add(Ref, NodeId);
            }
        }
        else
        {
            RecordFailure(Ref.IsEmpty() ? OpKind : Ref, Res);
        }

        Job->NodeCursor++;
        Job->Applied++;
        BudgetLeft--;
    }
    if (Job->NodeCursor < Job->NodeOps.Num()) { return true; }

    // 1.5) Place: every node now exists and reports its real size, so run the layered layout
    // over MEASURED sizes and move the nodes into it.
    if (Job->bAutoLayout && !Job->bPlaced)
    {
        Job->Phase = TEXT("placing");
        UBlueprint* PlaceBP = FUnrealMCPCommonUtils::FindBlueprint(Job->BlueprintName);
        UEdGraph* PlaceGraph = PlaceBP ? FUnrealMCPCommonUtils::FindGraphByName(PlaceBP, Job->GraphName) : nullptr;
        if (PlaceGraph)
        {
            TMap<FString, int32> RefIndex;
            for (int32 i = 0; i < Job->NodeOps.Num(); ++i)
            {
                FString Ref; Job->NodeOps[i]->TryGetStringField(TEXT("ref"), Ref);
                if (!Ref.IsEmpty()) { RefIndex.Add(Ref, i); }
            }

            FUnrealMCPCommonUtils::FLayoutInput In;
            In.NodeCount = Job->NodeOps.Num();
            In.Sizes.SetNum(In.NodeCount);
            for (int32 i = 0; i < In.NodeCount; ++i)
            {
                FString Ref; Job->NodeOps[i]->TryGetStringField(TEXT("ref"), Ref);
                const FString* Id = Job->Refs.Find(Ref);
                UEdGraphNode* Nd = (Id && PlaceBP) ? FUnrealMCPCommonUtils::FindNodeByGuid(PlaceBP, *Id, PlaceGraph) : nullptr;
                In.Sizes[i] = Nd ? FUnrealMCPCommonUtils::EstimateNodeSize(Nd) : FVector2D(220.0f, 100.0f);
            }
            for (const TSharedPtr<FJsonObject>& E : Job->EdgeOps)
            {
                FString S, T;
                E->TryGetStringField(TEXT("s"), S);
                E->TryGetStringField(TEXT("t"), T);
                const int32* Si = RefIndex.Find(S);
                const int32* Ti = RefIndex.Find(T);
                In.Edges.Add((Si && Ti) ? TPair<int32, int32>(*Si, *Ti) : TPair<int32, int32>(-1, -1));
            }

            FUnrealMCPCommonUtils::FLayoutOutput Layout;
            FUnrealMCPCommonUtils::LayeredLayout(In, Job->ColGap, Job->RowGap, Job->Origin, Layout);
            Job->EdgeKnots = Layout.EdgeKnots;

            for (int32 i = 0; i < Job->NodeOps.Num(); ++i)
            {
                FString Ref; Job->NodeOps[i]->TryGetStringField(TEXT("ref"), Ref);
                const FString* Id = Job->Refs.Find(Ref);
                if (!Id || !Layout.Positions.IsValidIndex(i)) { continue; }
                TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
                P->SetStringField(TEXT("blueprint_name"), Job->BlueprintName);
                P->SetStringField(TEXT("graph_name"), Job->GraphName);
                P->SetStringField(TEXT("node_id"), *Id);
                TArray<TSharedPtr<FJsonValue>> Arr;
                Arr.Add(MakeShared<FJsonValueNumber>(Layout.Positions[i].X));
                Arr.Add(MakeShared<FJsonValueNumber>(Layout.Positions[i].Y));
                P->SetArrayField(TEXT("position"), Arr);
                P->SetBoolField(TEXT("force"), true); // layout already rule-validated; skip the transient move check
                TSharedPtr<FJsonObject> Res = SubCommandRouter(TEXT("set_blueprint_node_position"), P);
                if (!bOk(Res)) { RecordFailure(FString::Printf(TEXT("place:%s"), *Ref), Res); }
            }
        }
        Job->bPlaced = true;
    }

    // 2) Connect edges (refs resolved to the GUIDs captured above).
    Job->Phase = TEXT("connecting");
    while (Job->EdgeCursor < Job->EdgeOps.Num() && BudgetLeft > 0)
    {
        TSharedPtr<FJsonObject> Op = Job->EdgeOps[Job->EdgeCursor];
        FString S, T, SP, TP;
        Op->TryGetStringField(TEXT("s"), S);
        Op->TryGetStringField(TEXT("t"), T);
        Op->TryGetStringField(TEXT("sp"), SP);
        Op->TryGetStringField(TEXT("tp"), TP);

        if (Job->bAutoLayout)
        {
            // Trusted layout: wire with the low-level helper, bypassing the straight-line
            // crossing/length checks that a knot passing near a column can trip.
            UBlueprint* LayoutBP = FUnrealMCPCommonUtils::FindBlueprint(Job->BlueprintName);
            UEdGraph* LayoutGraph = LayoutBP ? FUnrealMCPCommonUtils::FindGraphByName(LayoutBP, Job->GraphName) : nullptr;
            UEdGraphNode* SrcNode = (LayoutGraph && LayoutBP) ? FUnrealMCPCommonUtils::FindNodeByGuid(LayoutBP, Resolve(S), LayoutGraph) : nullptr;
            UEdGraphNode* DstNode = (LayoutGraph && LayoutBP) ? FUnrealMCPCommonUtils::FindNodeByGuid(LayoutBP, Resolve(T), LayoutGraph) : nullptr;
            static const TArray<FVector2D> EmptyKnots;
            const TArray<FVector2D>& KnotPath = Job->EdgeKnots.IsValidIndex(Job->EdgeCursor) ? Job->EdgeKnots[Job->EdgeCursor] : EmptyKnots;
            if (LayoutGraph && SrcNode && DstNode && FUnrealMCPCommonUtils::ConnectWithKnots(LayoutGraph, SrcNode, SP, DstNode, TP, KnotPath))
            {
                // Connected.
            }
            else
            {
                TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
                ErrObj->SetStringField(TEXT("error"), TEXT("layout wiring failed (node not found or connect rejected)"));
                RecordFailure(FString::Printf(TEXT("%s.%s -> %s.%s"), *S, *SP, *T, *TP), ErrObj);
            }
        }
        else
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("blueprint_name"), Job->BlueprintName);
            P->SetStringField(TEXT("graph_name"), Job->GraphName);
            P->SetStringField(TEXT("source_node_id"), Resolve(S));
            P->SetStringField(TEXT("target_node_id"), Resolve(T));
            P->SetStringField(TEXT("source_pin"), SP);
            P->SetStringField(TEXT("target_pin"), TP);
            TSharedPtr<FJsonObject> Res = SubCommandRouter(TEXT("connect_blueprint_nodes"), P);
            if (!bOk(Res)) { RecordFailure(FString::Printf(TEXT("%s.%s -> %s.%s"), *S, *SP, *T, *TP), Res); }
        }

        Job->EdgeCursor++;
        Job->Applied++;
        BudgetLeft--;
    }
    if (Job->EdgeCursor < Job->EdgeOps.Num()) { return true; }

    // 3) Pin defaults.
    Job->Phase = TEXT("defaults");
    while (Job->DefaultCursor < Job->DefaultOps.Num() && BudgetLeft > 0)
    {
        TSharedPtr<FJsonObject> Op = Job->DefaultOps[Job->DefaultCursor];
        FString Ref, Pin;
        Op->TryGetStringField(TEXT("ref"), Ref);
        Op->TryGetStringField(TEXT("pin"), Pin);

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("blueprint_name"), Job->BlueprintName);
        P->SetStringField(TEXT("graph_name"), Job->GraphName);
        P->SetStringField(TEXT("node_id"), Resolve(Ref));
        P->SetStringField(TEXT("pin_name"), Pin);
        const TSharedPtr<FJsonValue> ValueJson = Op->TryGetField(TEXT("value"));
        if (ValueJson.IsValid()) { P->SetField(TEXT("value"), ValueJson); }

        TSharedPtr<FJsonObject> Res = SubCommandRouter(TEXT("set_blueprint_node_pin_default"), P);
        if (!bOk(Res)) { RecordFailure(FString::Printf(TEXT("%s.%s"), *Ref, *Pin), Res); }

        Job->DefaultCursor++;
        Job->Applied++;
        BudgetLeft--;
    }
    if (Job->DefaultCursor < Job->DefaultOps.Num()) { return true; }

    Job->Phase = TEXT("finished");
    Job->State = TEXT("done");
    return false;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetPlanStatus(const TSharedPtr<FJsonObject>& Params)
{
    FString JobId;
    if (!Params->TryGetStringField(TEXT("job_id"), JobId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'job_id' parameter"));
    }

    TSharedPtr<FBlueprintPlanJobState> Job;
    {
        FScopeLock Lock(&GPlanJobsMutex);
        TSharedPtr<FBlueprintPlanJobState>* Found = GPlanJobs.Find(JobId);
        if (!Found)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown job_id: %s"), *JobId));
        }
        Job = *Found;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("job_id"), JobId);
    ResultObj->SetStringField(TEXT("state"), Job->State);
    ResultObj->SetStringField(TEXT("phase"), Job->Phase);
    ResultObj->SetNumberField(TEXT("applied"), Job->Applied);
    ResultObj->SetNumberField(TEXT("total"), Job->Total);
    ResultObj->SetNumberField(TEXT("failure_count"), Job->Failures.Num());
    ResultObj->SetArrayField(TEXT("failures"), Job->Failures);

    TSharedPtr<FJsonObject> RefsObj = MakeShared<FJsonObject>();
    for (const TPair<FString, FString>& Pair : Job->Refs)
    {
        RefsObj->SetStringField(Pair.Key, Pair.Value);
    }
    ResultObj->SetObjectField(TEXT("refs"), RefsObj);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params)
{
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(FUnrealMCPCommonUtils::GetEditorWorld(), AActor::StaticClass(), AllActors);
    
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
    UGameplayStatics::GetAllActorsOfClass(FUnrealMCPCommonUtils::GetEditorWorld(), AActor::StaticClass(), AllActors);
    
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

    // Optional: permit a duplicate base name (a free, suffixed name is derived below).
    bool bAllowDuplicate = false;
    Params->TryGetBoolField(TEXT("allow_duplicate"), bAllowDuplicate);

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
    TSet<FString> ExistingNames;
    for (AActor* Actor : AllActors)
    {
        if (Actor)
        {
            ExistingNames.Add(Actor->GetName());
        }
    }
    if (ExistingNames.Contains(ActorName))
    {
        if (!bAllowDuplicate)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor with name '%s' already exists"), *ActorName));
        }
        // allow_duplicate: derive a free name so the spawn can proceed instead of failing.
        const FString BaseName = ActorName;
        int32 Suffix = 1;
        while (ExistingNames.Contains(ActorName))
        {
            ActorName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
        }
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    SpawnParams.Name = *ActorName;

    // Case-insensitive: callers may pass any casing (e.g. "POINTLIGHT", "pointlight").
    if (ActorType.Equals(TEXT("StaticMeshActor"), ESearchCase::IgnoreCase))
    {
        NewActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType.Equals(TEXT("PointLight"), ESearchCase::IgnoreCase))
    {
        NewActor = World->SpawnActor<APointLight>(APointLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType.Equals(TEXT("SpotLight"), ESearchCase::IgnoreCase))
    {
        NewActor = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType.Equals(TEXT("DirectionalLight"), ESearchCase::IgnoreCase))
    {
        NewActor = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType.Equals(TEXT("CameraActor"), ESearchCase::IgnoreCase))
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
    UGameplayStatics::GetAllActorsOfClass(FUnrealMCPCommonUtils::GetEditorWorld(), AActor::StaticClass(), AllActors);
    
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
    AActor* TargetActor = FUnrealMCPCommonUtils::ResolveActor(ActorName);

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
    AActor* TargetActor = FUnrealMCPCommonUtils::ResolveActor(ActorName);

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
    AActor* TargetActor = FUnrealMCPCommonUtils::ResolveActor(ActorName);

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
        AActor* TargetActor = FUnrealMCPCommonUtils::ResolveActor(TargetActorName);

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
            FImageUtils::ThumbnailCompressImageArray(Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y, Bitmap, CompressedBitmap);
            
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