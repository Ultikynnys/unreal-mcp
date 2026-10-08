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
#include "UObject/ObjectRedirector.h"
#include "ObjectTools.h"
#include "MCPBuildRevision.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
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
    // Asset organization helpers
    // ---------------------------------------------------------------------

    bool AssetMutationBusy();
    bool FixAndVerifyRedirector(const FString& PackageName, bool bDelete, FString& Error, bool bApplyFixup = true);

    struct FAssetMoveRequest
    {
        FString Source;         // object path of the asset to move
        FString NewPackagePath; // e.g. "/Game/Weapons/Pistol"
        FString NewName;        // e.g. "Pistol_01"
    };

    // Splits an asset reference ("/Game/A/Foo", "/Game/A/Foo.Foo", "/Game/A/Foo.Foo:Sub")
    // into its containing package path and asset name. Returns false when the path has no
    // package component.
    bool SplitObjectPath(const FString& InPath, FString& OutPackagePath, FString& OutName)
    {
        FString Path = InPath.TrimStartAndEnd();
        int32 DotIdx = INDEX_NONE;
        if (Path.FindLastChar(TEXT('.'), DotIdx))
        {
            Path = Path.Left(DotIdx);
        }
        int32 ColonIdx = INDEX_NONE;
        if (Path.FindChar(TEXT(':'), ColonIdx))
        {
            Path = Path.Left(ColonIdx);
        }
        while (Path.EndsWith(TEXT("/")))
        {
            Path.RemoveFromEnd(TEXT("/"));
        }
        int32 SlashIdx = INDEX_NONE;
        if (!Path.FindLastChar(TEXT('/'), SlashIdx))
        {
            return false;
        }
        OutPackagePath = Path.Left(SlashIdx);
        OutName = Path.Mid(SlashIdx + 1);
        return !OutPackagePath.IsEmpty() && !OutName.IsEmpty();
    }

    // ---------------------------------------------------------------------
    // Python pin cleanup for world switches
    // ---------------------------------------------------------------------

    // World switches tear down the old world package and GC it, and Public-scope execute_python
    // leaves UObject references in __main__ globals that pin it, tripping the "World memory
    // leaks" ensure. Clear the Python pins first.
    void ClearPythonPinsAndGC()
    {
        IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
        if (PythonPlugin && PythonPlugin->IsPythonAvailable())
        {
            FPythonCommandEx Command;
            Command.Command = TEXT(
                "import sys, gc\n"
                "_m = sys.modules.get('__main__')\n"
                "if _m is not None:\n"
                "    for _k in [k for k in list(vars(_m)) if not k.startswith('_')]:\n"
                "        try:\n"
                "            delattr(_m, _k)\n"
                "        except Exception:\n"
                "            pass\n"
                "gc.collect()\n");
            Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
            Command.FileExecutionScope = EPythonFileExecutionScope::Private;
            PythonPlugin->ExecPythonCommandEx(Command);
        }
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

    // Async blueprint-plan jobs (polled via get_plan_status): apply_blueprint_plan records a job
    // and returns an id at once, and a core-ticker lambda applies it a chunk at a time on the
    // game thread, so a large plan never blocks the socket or nests in the dispatching task.

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

    // Generic async jobs (polled via get_job_status): native asset operations can block a tick,
    // so callers poll between operations.
    struct FMcpJobState
    {
        FString Kind;               // e.g. "move_assets"
        FString State;              // queued | running | done | failed
        FString Phase;              // human-readable current step
        int32 Done = 0;
        int32 Total = 0;
        bool bCancelRequested = false;
        TArray<FString> Items;      // per-item results / affected object paths
        TArray<FString> Log;        // running notes
        FString Error;
    };

    TMap<FString, TSharedPtr<FMcpJobState>> GMcpJobs;
    FCriticalSection GMcpJobsMutex;

    TSharedPtr<FMcpJobState> FindMcpJob(const FString& JobId)
    {
        FScopeLock Lock(&GMcpJobsMutex);
        TSharedPtr<FMcpJobState>* Found = GMcpJobs.Find(JobId);
        return Found ? *Found : nullptr;
    }

    FString CreateMcpJob(const FString& Kind, int32 Total)
    {
        const FString JobId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        TSharedPtr<FMcpJobState> Job = MakeShared<FMcpJobState>();
        Job->Kind = Kind;
        Job->State = TEXT("queued");
        Job->Total = Total;
        FScopeLock Lock(&GMcpJobsMutex);
        GMcpJobs.Add(JobId, Job);
        return JobId;
    }

    // Drives a chunked job on the core ticker. Step runs once per tick and returns
    // true while work remains; each call must stay short and set Job->State to
    // done/failed when finished. The ticker unregisters itself when Step returns false.
    void RunJobChunked(const FString& JobId, TFunction<bool(const TSharedPtr<FMcpJobState>&)> Step)
    {
        TSharedPtr<FMcpJobState> Initial = FindMcpJob(JobId);
        if (!Initial.IsValid())
        {
            return;
        }
        Initial->State = TEXT("running");

        TSharedPtr<TFunction<bool(const TSharedPtr<FMcpJobState>&)>> StepPtr =
            MakeShared<TFunction<bool(const TSharedPtr<FMcpJobState>&)>>(MoveTemp(Step));

        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [JobId, StepPtr](float) -> bool
                {
                    TSharedPtr<FMcpJobState> Job = FindMcpJob(JobId);
                    if (!Job.IsValid() || Job->bCancelRequested)
                    {
                        return false;
                    }
                    bool bMore = false;
                    try
                    {
                        bMore = (*StepPtr)(Job);
                    }
                    catch (const std::exception& e)
                    {
                        Job->State = TEXT("failed");
                        Job->Error = UTF8_TO_TCHAR(e.what());
                        return false;
                    }
                    if (!bMore && Job->State == TEXT("running"))
                    {
                        Job->State = TEXT("done");
                    }
                    return bMore;
                }),
            0.0f);
    }

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

    // Destroy through UEditorActorSubsystem so the selection set, layers and typed-element
    // registry stay consistent; raw Actor->Destroy() trips the TypedElementRegistry assert.
    // Returns true only when every actor was destroyed: never fall back to Actor->Destroy().
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
    else if (CommandType == TEXT("spawn_actor"))
    {
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
    else if (CommandType == TEXT("set_actor_property"))
    {
        return HandleSetActorProperty(Params);
    }
    // Blueprint actor spawning
    else if (CommandType == TEXT("spawn_blueprint_actor"))
    {
        return HandleSpawnBlueprintActor(Params);
    }
    
    else if (CommandType == TEXT("get_actor_details")) { return HandleGetActorDetails(Params); }
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
    else if (CommandType == TEXT("get_job_status")) { return HandleGetJobStatus(Params); }
    else if (CommandType == TEXT("list_redirectors")) { return HandleListRedirectors(Params); }
    else if (CommandType == TEXT("fixup_redirectors")) { return HandleFixupRedirectors(Params); }
    else if (CommandType == TEXT("move_assets")) { return HandleMoveAssets(Params); }
    else if (CommandType == TEXT("move_folder")) { return HandleMoveFolder(Params); }
    else if (CommandType == TEXT("resave_packages")) { return HandleResavePackages(Params); }
    else if (CommandType == TEXT("get_asset_graph")) { return HandleGetAssetGraph(Params); }
    else if (CommandType == TEXT("delete_assets")) { return HandleDeleteAssets(Params); }
    else if (CommandType == TEXT("console_command")) { return HandleConsoleCommand(Params); }
    else if (CommandType == TEXT("editor_play")) { return HandleEditorPlay(Params); }
    else if (CommandType == TEXT("editor_stop")) { return HandleEditorStop(Params); }
    else if (CommandType == TEXT("list_levels")) { return HandleListLevels(Params); }
    else if (CommandType == TEXT("get_current_level")) { return HandleGetCurrentLevel(Params); }
    else if (CommandType == TEXT("recover_editor")) { return HandleRecoverEditor(Params); }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown editor command: %s"), *CommandType));
}

// Recover from the "restore unsaved files" prompt that stalls startup after an abnormal
// shutdown: delete Saved/Autosaves/PackageRestoreData.json so it cannot recur, then dismiss
// the stale modal. Runs via AsyncTask, which still dispatches while a modal is up.
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
    ResultObj->SetStringField(TEXT("revision"), MCP_REVISION);
    ResultObj->SetBoolField(TEXT("built_dirty"), MCP_REVISION_DIRTY != 0);
    ResultObj->SetStringField(TEXT("plugin"), TEXT("UnrealMCP"));
    ResultObj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

    static const TCHAR* SupportedCommands[] = {
        TEXT("get_actors_in_level"), TEXT("spawn_actor"),
        TEXT("delete_actor"), TEXT("set_actor_transform"),
        TEXT("get_actor_details"), TEXT("set_actor_property"),
        TEXT("spawn_blueprint_actor"), TEXT("spawn_mesh_actor"), TEXT("spawn_light_actor"),
        TEXT("spawn_mesh_grid"), TEXT("spawn_instanced_mesh"), TEXT("set_actor_material"),
        TEXT("set_actor_folder"), TEXT("delete_actors_by_prefix"),
        TEXT("capture_viewport_screenshot"), TEXT("capture_pie_screenshot"), TEXT("set_viewport_camera"),
        TEXT("create_level"), TEXT("save_level"), TEXT("load_level"), TEXT("delete_level"),
        TEXT("query_assets"), TEXT("get_asset_details"), TEXT("get_capabilities"),
        TEXT("batch_execute"), TEXT("execute_python"), TEXT("reload_server"),
        TEXT("import_asset"), TEXT("get_import_status"),
        TEXT("apply_blueprint_plan"), TEXT("get_plan_status"),
        TEXT("get_job_status"), TEXT("list_redirectors"), TEXT("fixup_redirectors"),
        TEXT("move_assets"), TEXT("move_folder"), TEXT("resave_packages"),
        TEXT("get_asset_graph"), TEXT("delete_assets"), TEXT("console_command"),
        TEXT("editor_play"), TEXT("editor_stop"),
        TEXT("list_levels"), TEXT("get_current_level"),
        TEXT("delete_blueprint_node"), TEXT("clear_blueprint_graph"),
        TEXT("disconnect_blueprint_pin"), TEXT("get_blueprint_graphs"),
        TEXT("set_blueprint_node_pin_default"),
        TEXT("get_blueprint_node_bounds"),
        TEXT("auto_layout_blueprint_graph"),
        // Blueprint asset commands (dispatched by FUnrealMCPBlueprintCommands). A capability
        // list that omits these tells an agent the plugin cannot do blueprints at all, so
        // check_tool_parity.py now fails when a served command is not advertised here.
        TEXT("create_blueprint"), TEXT("add_component_to_blueprint"),
        TEXT("set_component_property"), TEXT("set_physics_properties"),
        TEXT("compile_blueprint"), TEXT("set_blueprint_property"),
        TEXT("set_static_mesh_properties"),
        // Blueprint graph node commands (FUnrealMCPBlueprintNodeCommands)
        TEXT("add_blueprint_event_node"), TEXT("add_blueprint_input_action_node"),
        TEXT("add_blueprint_function_node"), TEXT("add_blueprint_variable"),
        TEXT("add_blueprint_node"), TEXT("add_blueprint_reroute_node"),
        TEXT("add_blueprint_self_reference"),
        TEXT("add_blueprint_get_self_component_reference"),
        TEXT("connect_blueprint_nodes"), TEXT("find_blueprint_nodes"),
        TEXT("set_blueprint_node_position"), TEXT("validate_blueprint_graph"),
        // UMG widget commands (FUnrealMCPUMGCommands)
        TEXT("create_umg_widget_blueprint"), TEXT("add_text_block_to_widget"),
        TEXT("add_button_to_widget"), TEXT("bind_widget_event"),
        TEXT("add_widget_to_viewport"), TEXT("set_text_block_binding"),
        // Project settings (FUnrealMCPProjectCommands)
        TEXT("create_input_mapping"),
        // Answered inline by the bridge, so there is no handler class for them
        TEXT("ping"),
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

    // Honor allow_duplicate: on a name clash, fail unless the caller permits a derived
    // free name (mirrors HandleSpawnActor).
    bool bAllowDuplicate = false;
    Params->TryGetBoolField(TEXT("allow_duplicate"), bAllowDuplicate);
    if (!ActorName.IsEmpty())
    {
        TArray<AActor*> ExistingActors;
        UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), ExistingActors);
        TSet<FString> ExistingNames;
        for (AActor* Existing : ExistingActors) { if (Existing) { ExistingNames.Add(Existing->GetName()); } }
        if (ExistingNames.Contains(ActorName))
        {
            if (!bAllowDuplicate)
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor with name '%s' already exists (set allow_duplicate=true)"), *ActorName));
            }
            const FString BaseName = ActorName;
            int32 Suffix = 1;
            while (ExistingNames.Contains(ActorName)) { ActorName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++); }
        }
    }

    FActorSpawnParameters SpawnParams;
    // NameMode Requested so any residual race still auto-renames instead of fatal.
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
        FString Mobility = TEXT("movable");
        Params->TryGetStringField(TEXT("mobility"), Mobility);
        EComponentMobility::Type MobilityType = EComponentMobility::Movable;
        if (Mobility.Equals(TEXT("static"), ESearchCase::IgnoreCase)) { MobilityType = EComponentMobility::Static; }
        else if (Mobility.Equals(TEXT("stationary"), ESearchCase::IgnoreCase)) { MobilityType = EComponentMobility::Stationary; }
        LC->SetMobility(MobilityType);
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
    // "name" is the internal object name (which UE may have uniquified, e.g. Actor_0);
    // "label" is the name the caller asked for, and what actor lookups resolve first.
    ResultObj->SetStringField(TEXT("name"), NewActor->GetName());
    ResultObj->SetStringField(TEXT("label"), NewActor->GetActorLabel());
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

    // The map switch GCs the outgoing world package; release lingering Python pins
    // first or the "World memory leaks" ensure can crash the editor (see helper).
    ClearPythonPinsAndGC();

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

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    bool bSaved = false;
    if (bHasDestination)
    {
        if (!bOverwrite && UEditorAssetLibrary::DoesAssetExist(DestinationPath))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Level already exists: %s (set overwrite=true)"), *DestinationPath));
        }
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
    // Same world-switch hazard as create_level: clear Python pins first.
    ClearPythonPinsAndGC();

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

    // Deleting the open map unloads its world, so release lingering Python pins first (the same
    // "World memory leaks" hazard as create_level / load_level). The file cannot be removed
    // while its world is active, so a forced delete switches to a blank map first.
    if (bForce && !CurrentPackage.IsEmpty() && MapPackage.Equals(CurrentPackage, ESearchCase::IgnoreCase))
    {
        if (GEditor)
        {
            GEditor->NewMap();
        }
    }
    ClearPythonPinsAndGC();

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
    // A batch with failed sub-actions must not report success, or the caller has to
    // notice by parsing the per-action results itself.
    ResultObj->SetBoolField(TEXT("success"), Failures == 0);
    if (Failures > 0)
    {
        TArray<FString> FailedCommands;
        for (const TSharedPtr<FJsonValue>& Value : Results)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry
                && (*Entry)->HasField(TEXT("success")) && !(*Entry)->GetBoolField(TEXT("success")))
            {
                FailedCommands.Add((*Entry)->GetStringField(TEXT("command")));
            }
        }
        ResultObj->SetStringField(TEXT("error"), FString::Printf(
            TEXT("%d/%d batch action(s) failed: %s%s"), Failures, Results.Num(),
            *FString::Join(FailedCommands, TEXT(", ")),
            bRolledBack ? TEXT(" (batch rolled back)") : TEXT(" (no rollback requested)")));
    }
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
    FString LastError;
    for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
    {
        LogArray.Add(MakeShared<FJsonValueString>(Entry.Output));
        if (Entry.Type == EPythonLogOutputType::Error) { LastError = Entry.Output.TrimEnd(); }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), bSuccess);
    ResultObj->SetStringField(TEXT("command_result"), Command.CommandResult);
    ResultObj->SetArrayField(TEXT("output"), LogArray);
    if (!bSuccess)
    {
        // The client surfaces a failure reason from "error"; without it a Python
        // exception inside the editor reaches the caller as "Unknown Unreal error".
        // CommandResult carries the trace on failure, LogOutput is the fallback.
        FString Reason = Command.CommandResult.TrimStartAndEnd();
        if (Reason.IsEmpty()) { Reason = LastError; }
        if (Reason.IsEmpty()) { Reason = TEXT("Python execution failed (see 'output')"); }
        ResultObj->SetStringField(TEXT("error"), Reason);
        ResultObj->SetStringField(TEXT("message"), Reason);
    }
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

// get_job_status: poll any generic async job (move_assets / fixup_redirectors /
// resave_packages). Returns {job_id, kind, state, phase, done, total, error, items, log}.
TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetJobStatus(const TSharedPtr<FJsonObject>& Params)
{
    FString JobId;
    if (!Params->TryGetStringField(TEXT("job_id"), JobId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'job_id' parameter"));
    }

    TSharedPtr<FMcpJobState> Job = FindMcpJob(JobId);
    if (!Job.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown job_id: %s"), *JobId));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("job_id"), JobId);
    ResultObj->SetStringField(TEXT("kind"), Job->Kind);
    ResultObj->SetStringField(TEXT("state"), Job->State);
    ResultObj->SetStringField(TEXT("phase"), Job->Phase);
    ResultObj->SetNumberField(TEXT("done"), Job->Done);
    ResultObj->SetNumberField(TEXT("total"), Job->Total);
    ResultObj->SetStringField(TEXT("error"), Job->Error);

    TArray<TSharedPtr<FJsonValue>> ItemsArr;
    for (const FString& Item : Job->Items) { ItemsArr.Add(MakeShared<FJsonValueString>(Item)); }
    ResultObj->SetArrayField(TEXT("items"), ItemsArr);

    TArray<TSharedPtr<FJsonValue>> LogArr;
    for (const FString& Line : Job->Log) { LogArr.Add(MakeShared<FJsonValueString>(Line)); }
    ResultObj->SetArrayField(TEXT("log"), LogArr);

    return ResultObj;
}

// list_redirectors: enumerate ObjectRedirectors under a path (the clutter a rename leaves).
// referencer_count is how many packages still resolve the old path; resolve_destination
// loads each to report where it points (slower, off by default).
TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleListRedirectors(const TSharedPtr<FJsonObject>& Params)
{
    FString Path = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), Path);
    bool bRecursive = true;
    if (Params->HasField(TEXT("recursive"))) { bRecursive = Params->GetBoolField(TEXT("recursive")); }
    bool bResolveDestination = false;
    if (Params->HasField(TEXT("resolve_destination"))) { bResolveDestination = Params->GetBoolField(TEXT("resolve_destination")); }

    FARFilter Filter;
    Filter.PackagePaths.Add(*Path);
    Filter.bRecursivePaths = bRecursive;
    Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());

    FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARModule.Get();
    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> RedirectorArray;
    int32 TotalReferencers = 0;
    for (const FAssetData& Asset : Assets)
    {
        TArray<FName> Referencers;
        AR.GetReferencers(Asset.PackageName, Referencers);

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Obj->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
        Obj->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
        Obj->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
        Obj->SetNumberField(TEXT("referencer_count"), Referencers.Num());
        TotalReferencers += Referencers.Num();

        // Load the package and find the redirector inside it: loading the object path
        // directly would follow the redirect to the destination instead.
        FString Destination;
        if (bResolveDestination)
        {
            UPackage* Pkg = LoadPackage(nullptr, *Asset.PackageName.ToString(), LOAD_None);
            UObjectRedirector* Redirector = Pkg ? FindObject<UObjectRedirector>(Pkg, *Asset.AssetName.ToString()) : nullptr;
            if (Redirector && Redirector->DestinationObject)
            {
                Destination = Redirector->DestinationObject->GetPathName();
            }
        }
        Obj->SetStringField(TEXT("destination"), Destination);
        RedirectorArray.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("path"), Path);
    ResultObj->SetBoolField(TEXT("recursive"), bRecursive);
    ResultObj->SetNumberField(TEXT("count"), RedirectorArray.Num());
    ResultObj->SetNumberField(TEXT("total_referencers"), TotalReferencers);
    ResultObj->SetArrayField(TEXT("redirectors"), RedirectorArray);
    return ResultObj;
}

// fixup_redirectors: scripted "Fix Up Redirectors in Folder". Resaves the packages still
// referencing the old path and (by default) deletes the redirector - what UE's Python API
// cannot do (no fix_references). Async job; poll get_job_status.
TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFixupRedirectors(const TSharedPtr<FJsonObject>& Params)
{
    if (AssetMutationBusy()) { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Another asset mutation job is running")); }
    FString Path = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), Path);
    bool bRecursive = true;
    if (Params->HasField(TEXT("recursive"))) { bRecursive = Params->GetBoolField(TEXT("recursive")); }
    bool bDeleteRedirectors = true;
    if (Params->HasField(TEXT("delete_redirectors"))) { bDeleteRedirectors = Params->GetBoolField(TEXT("delete_redirectors")); }
    int32 BatchSize = 1;
    if (Params->HasField(TEXT("batch_size"))) { BatchSize = FMath::Max(1, (int32)Params->GetNumberField(TEXT("batch_size"))); }

    FARFilter Filter;
    Filter.PackagePaths.Add(*Path);
    Filter.bRecursivePaths = bRecursive;
    Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());

    FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets;
    ARModule.Get().GetAssets(Filter, Assets);

    auto Pending = MakeShared<TArray<FString>>();
    for (const FAssetData& Asset : Assets) { Pending->Add(Asset.PackageName.ToString()); }

    const FString JobId = CreateMcpJob(TEXT("fixup_redirectors"), Pending->Num());
    const ERedirectFixupMode Mode = bDeleteRedirectors ? ERedirectFixupMode::DeleteFixedUpRedirectors : ERedirectFixupMode::LeaveFixedUpRedirectors;

    RunJobChunked(JobId, [Pending, Mode, BatchSize](const TSharedPtr<FMcpJobState>& Job) -> bool
    {
        int32 ProcessedThisTick = 0;
        while (Job->Done < Pending->Num() && ProcessedThisTick < BatchSize)
        {
            const FString& Name = (*Pending)[Job->Done];
            FString Error;
            if (!FixAndVerifyRedirector(Name, Mode == ERedirectFixupMode::DeleteFixedUpRedirectors, Error))
            {
                Job->State = TEXT("failed");
                Job->Error = Name + TEXT(": ") + Error;
                Job->Items.Add(Job->Error);
                return false;
            }
            Job->Items.Add(Name + TEXT(": cleanup verified"));

            Job->Done++;
            ProcessedThisTick++;
        }

        Job->Phase = FString::Printf(TEXT("%d/%d redirectors"), Job->Done, Job->Total);
        return Job->Done < Pending->Num();
    });

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("job_id"), JobId);
    ResultObj->SetStringField(TEXT("state"), TEXT("queued"));
    ResultObj->SetNumberField(TEXT("count"), Pending->Num());
    ResultObj->SetBoolField(TEXT("delete_redirectors"), bDeleteRedirectors);
    return ResultObj;
}

namespace
{
    bool AssetMutationBusy()
    {
        FScopeLock Lock(&GMcpJobsMutex);
        for (const auto& Entry : GMcpJobs)
        {
            const auto& Job = Entry.Value;
            if ((Job->Kind == TEXT("move_assets") || Job->Kind == TEXT("fixup_redirectors") || Job->Kind == TEXT("resave_packages")) &&
                (Job->State == TEXT("queued") || Job->State == TEXT("running"))) { return true; }
        }
        return false;
    }

    // A package moved in the same batch as its dependency can stay saved with the old import: the
    // in-memory rewire never re-saves it (observed live on MI_SchemePickup). Loading and saving the
    // referencers the registry still reports rewrites them; one registry query per move otherwise.
    int32 ResaveStaleReferencers(const TArray<FString>& MovedSources, FString& OutError)
    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        int32 Resaved = 0;
        TSet<FString> Handled;
        for (const FString& Source : MovedSources)
        {
            AssetRegistry.WaitForPackage(Source);
            TArray<FName> Referencers;
            AssetRegistry.GetReferencers(FName(*Source), Referencers);
            for (const FName& Referencer : Referencers)
            {
                const FString Name = Referencer.ToString();
                if (Handled.Contains(Name)) { continue; }
                Handled.Add(Name);

                UObject* Loaded = UEditorAssetLibrary::LoadAsset(Name);
                if (!Loaded) { continue; } // gone, or never loadable: nothing to write
                UPackage* LoadedPackage = Loaded->GetOutermost();
                UWorld* LoadedWorld = LoadedPackage ? UWorld::FindWorldInPackage(LoadedPackage) : nullptr;
                // Pass the PACKAGE, not the object's bare name: SaveAsset("M_Foo") quietly fails,
                // and a map needs the level path or it keeps its old import on disk.
                const bool bReSaved = LoadedWorld
                    ? (LoadedWorld->PersistentLevel
                        && FEditorFileUtils::SaveLevel(LoadedWorld->PersistentLevel, LoadedPackage->GetName()))
                    : UEditorAssetLibrary::SaveLoadedAsset(Loaded, false);
                if (bReSaved)
                {
                    ++Resaved;
                }
                else
                {
                    OutError = FString::Printf(TEXT("%s could not be re-saved"), *Name);
                }
            }
        }
        return Resaved;
    }

    // Runs the sweep above and records it in the job's items. Called when a move batch finishes,
    // from whichever completion path the batch took (verified fixup, or fixup disabled).
    void RecordResaveSweep(const TSharedPtr<FMcpJobState>& Job, const TArray<FString>& MovedSources)
    {
        FString SweepError;
        const int32 Resaved = ResaveStaleReferencers(MovedSources, SweepError);
        if (Resaved > 0)
        {
            Job->Items.Add(FString::Printf(TEXT("resaved %d package(s) still importing a moved path"), Resaved));
        }
        if (!SweepError.IsEmpty()) { Job->Items.Add(TEXT("resave sweep: ") + SweepError); }
    }

    bool NormalizeAssetPackage(const FString& Input, FString& Package)
    {
        Package = Input.TrimStartAndEnd();
        if (Package.Contains(TEXT(":"))) { return false; }
        FString ObjectName, PackagePart;
        if (Package.Split(TEXT("."), &PackagePart, &ObjectName))
        {
            Package = PackagePart;
            if (ObjectName != FPackageName::GetLongPackageAssetName(Package)) { return false; }
        }
        FString Filename;
        return FPackageName::IsValidLongPackageName(Package) && FPackageName::GetLongPackagePath(Package) != TEXT("") &&
            FPackageName::TryConvertLongPackageNameToFilename(Package, Filename);
    }

    bool PackageOccupied(const FString& Package)
    {
        TArray<FAssetData> Assets;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPackageName(FName(*Package), Assets);
        return Assets.Num() > 0 || FindPackage(nullptr, *Package) || FPackageName::DoesPackageExist(Package);
    }

    // Dialog-free FixupReferencers: IAssetTools ends in a modal report and ConsolidateObjects
    // raises "Critical Failure" on a stubborn redirector, so neither is called. Saving rewired
    // referencers before the GC+delete is what releases a loaded level/blueprint's old refs.
    bool FixAndVerifyRedirector(const FString& PackageName, bool bDelete, FString& Error, bool bApplyFixup)
    {
        IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        const FString Name = FPackageName::GetLongPackageAssetName(PackageName);

        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (!Package && FPackageName::DoesPackageExist(PackageName)) { Package = LoadPackage(nullptr, *PackageName, LOAD_None); }
        UObjectRedirector* Redirector = Package ? FindObject<UObjectRedirector>(Package, *Name) : nullptr;
        if (!Redirector)
        {
            TArray<FAssetData> Registered;
            AR.GetAssetsByPackageName(FName(*PackageName), Registered);
            if (Registered.Num() > 0 || FPackageName::DoesPackageExist(PackageName))
            {
                Error = TEXT("Source package exists but its redirector could not be loaded");
                return false;
            }
        }
        else if (bApplyFixup)
        {
            // Keep-alives guard only the rewire and save: a TStrongObjectPtr left in
            // scope roots the redirector, and a rooted object can never be collected,
            // so these MUST die before the CollectGarbage below.
            {
                TStrongObjectPtr<UObjectRedirector> KeepAlive(Redirector);
                TArray<FName> Referencers;
                AR.WaitForPackage(PackageName);
                AR.GetReferencers(FName(*PackageName), Referencers);
                if (Referencers.Num() > 0)
                {
                    UObject* Destination = Redirector->DestinationObject;
                    if (!Destination)
                    {
                        Error = TEXT("Redirector has no destination object to consolidate onto");
                        return false;
                    }
                    TStrongObjectPtr<UObject> KeepDestination(Destination);
                TArray<UPackage*> LoadedReferencers;
                for (const FName& Referencer : Referencers)
                {
                    UPackage* Loaded = FindPackage(nullptr, *Referencer.ToString());
                    if (!Loaded)
                    {
                        Loaded = LoadPackage(nullptr, *Referencer.ToString(), LOAD_None);
                        if (Loaded) { LoadedReferencers.Add(Loaded); }
                    }
                }
                // Rewire in-memory references, then persist them BEFORE touching the
                // redirector: a loaded level/blueprint keeps resolving through the old path
                // until its package is saved with the rewired references.
                TArray<UObject*> Olds;
                Olds.Add(Redirector);
                ObjectTools::ForceReplaceReferences(Destination, Olds);
                for (UPackage* Loaded : LoadedReferencers)
                {
                    const FString ReferencerName = Loaded ? Loaded->GetName() : FString();
                    UWorld* ReferencerWorld = Loaded ? UWorld::FindWorldInPackage(Loaded) : nullptr;
                    // A map cannot be written through the asset save path: saving it as an asset
                    // silently leaves its old import on disk, which is how dangling refs appear.
                    const bool bSavedReferencer = ReferencerWorld
                        ? (ReferencerWorld->PersistentLevel
                            && FEditorFileUtils::SaveLevel(ReferencerWorld->PersistentLevel, ReferencerName))
                        : (Loaded && UEditorAssetLibrary::SaveLoadedAsset(Loaded, false));
                    if (!bSavedReferencer)
                    {
                        Error = TEXT("Referencer package failed to save: ") + ReferencerName;
                        return false;
                    }
                }
                }
            }
            Redirector->RemoveFromRoot();
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            // Re-look-up after GC: the raw pointer is stale if the object was collected.
            UObject* PackageObject = FindObject<UPackage>(nullptr, *PackageName);
            UObjectRedirector* RemainingRedirector = PackageObject ? FindObject<UObjectRedirector>(PackageObject, *Name) : nullptr;
            if (RemainingRedirector)
            {
                // A loaded world can still hold the redirector in an import map resolved
                // before the rewire, so GC may miss it; the engine report path deletes it
                // directly in that case rather than failing.
                TArray<UObject*> ToForceDelete;
                ToForceDelete.Add(RemainingRedirector);
                if (PackageObject) { ToForceDelete.Add(PackageObject); }
                ObjectTools::ForceDeleteObjects(ToForceDelete, /*bShowConfirmation=*/false);
            }
            else if (PackageObject)
            {
                // The redirector object is gone; drop the emptied package. DeleteLoadedAsset
                // would create a NEW redirector, so bypass it with a force delete.
                TArray<UObject*> EmptyPackage;
                EmptyPackage.Add(PackageObject);
                ObjectTools::ForceDeleteObjects(EmptyPackage, /*bShowConfirmation=*/false);
            }
        }
        AR.WaitForPackage(PackageName);
        TArray<FAssetData> Remaining;
        AR.GetAssetsByPackageName(FName(*PackageName), Remaining);
        if (bDelete && (Remaining.Num() > 0 || FPackageName::DoesPackageExist(PackageName)))
        {
            Error = TEXT("Source redirector remains; check read-only packages/source control, then run fixup_redirectors");
            return false;
        }
        // A force-deleted source package leaves stale in-memory registry edges (the open map
        // resolved the import before the rewire). Edges to a package that exists in neither memory
        // nor on disk cannot resolve and are harmless; fail only when the package is still real.
        if (bDelete && !FPackageName::DoesPackageExist(PackageName) && !FindObject<UPackage>(nullptr, *PackageName))
        {
            return true;
        }
        TArray<FName> Referencers;
        AR.GetReferencers(FName(*PackageName), Referencers);
        if (Referencers.Num() > 0)
        {
            TArray<FString> Names;
            for (const FName& Referencer : Referencers) { Names.Add(Referencer.ToString()); }
            Error = TEXT("Registry still reports source referencers: ") + FString::Join(Names, TEXT(", "));
            return false;
        }
        return true;
    }
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleMoveFolder(const TSharedPtr<FJsonObject>& Params)
{
    FString Source, Destination;
    bool bRecursive = true, bDryRun = false;
    if (!Params->TryGetStringField(TEXT("source_path"), Source) || !Params->TryGetStringField(TEXT("destination_path"), Destination) ||
        (Params->HasField(TEXT("recursive")) && !Params->TryGetBoolField(TEXT("recursive"), bRecursive)) ||
        (Params->HasField(TEXT("dry_run")) && !Params->TryGetBoolField(TEXT("dry_run"), bDryRun)))
    { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("source_path/destination_path must be strings; recursive/dry_run must be booleans")); }
    Source = Source.TrimStartAndEnd();
    Destination = Destination.TrimStartAndEnd();
    Source.RemoveFromEnd(TEXT("/"));
    Destination.RemoveFromEnd(TEXT("/"));
    if (!FPackageName::IsValidLongPackageName(Source) || !FPackageName::IsValidLongPackageName(Destination) ||
        Source.Contains(TEXT(".")) || Destination.Contains(TEXT(".")) ||
        Source.Equals(Destination, ESearchCase::IgnoreCase) || Destination.StartsWith(Source + TEXT("/"), ESearchCase::IgnoreCase) ||
        Source.StartsWith(Destination + TEXT("/"), ESearchCase::IgnoreCase))
    { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Use valid, distinct, non-nested content folders")); }
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Source));
    Filter.bRecursivePaths = bRecursive;
    TArray<FAssetData> Assets;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);
    Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.ToString() < B.PackageName.ToString(); });
    TArray<TSharedPtr<FJsonValue>> Moves;
    for (const auto& Asset : Assets)
    {
        if (Asset.AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName()) { continue; }
        const FString Package = Asset.PackageName.ToString();
        if (!Package.StartsWith(Source + TEXT("/"), ESearchCase::IgnoreCase))
        { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Registry returned an asset outside the source folder")); }
        auto Move = MakeShared<FJsonObject>();
        Move->SetStringField(TEXT("source"), Package);
        Move->SetStringField(TEXT("destination"), Destination + Package.Mid(Source.Len()));
        Moves.Add(MakeShared<FJsonValueObject>(Move));
    }
    auto MoveParams = MakeShared<FJsonObject>();
    MoveParams->SetArrayField(TEXT("moves"), Moves);
    MoveParams->SetBoolField(TEXT("dry_run"), bDryRun);
    MoveParams->SetBoolField(TEXT("fixup_redirectors"), true);
    return HandleMoveAssets(MoveParams);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleMoveAssets(const TSharedPtr<FJsonObject>& Params)
{
    if (AssetMutationBusy()) { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Another asset mutation job is running; poll it before restructuring")); }
    bool bDryRun = false, bFixup = true;
    if ((Params->HasField(TEXT("dry_run")) && !Params->TryGetBoolField(TEXT("dry_run"), bDryRun)) ||
        (Params->HasField(TEXT("fixup_redirectors")) && !Params->TryGetBoolField(TEXT("fixup_redirectors"), bFixup)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("dry_run and fixup_redirectors must be booleans"));
    }
    TArray<TPair<FString, FString>> Inputs;
    const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
    if (Params->HasField(TEXT("moves")))
    {
        if (Params->HasField(TEXT("assets")) || Params->HasField(TEXT("destination_path")) || !Params->TryGetArrayField(TEXT("moves"), Array))
        { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Provide only moves, or assets with destination_path")); }
        for (const auto& Value : *Array)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString Source, Destination;
            if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object ||
                !(*Object)->TryGetStringField(TEXT("source"), Source) || !(*Object)->TryGetStringField(TEXT("destination"), Destination))
            { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Every move requires string source and destination")); }
            Inputs.Emplace(Source, Destination);
        }
    }
    else
    {
        FString DestinationPath;
        if (!Params->TryGetArrayField(TEXT("assets"), Array) || !Params->TryGetStringField(TEXT("destination_path"), DestinationPath) ||
            !FPackageName::IsValidLongPackageName(DestinationPath) || DestinationPath.Contains(TEXT(".")))
        { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Provide assets and a valid destination_path")); }
        for (const auto& Value : *Array)
        {
            FString Source, Package;
            if (!Value.IsValid() || !Value->TryGetString(Source) || !NormalizeAssetPackage(Source, Package))
            { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Every asset must be a valid package/object path")); }
            Inputs.Emplace(Source, DestinationPath / FPackageName::GetLongPackageAssetName(Package));
        }
    }
    if (Inputs.Num() == 0) { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No assets to move")); }
    auto Pending = MakeShared<TArray<FAssetMoveRequest>>();
    TSet<FName> Sources, Destinations;
    TArray<TSharedPtr<FJsonValue>> Preview;
    for (const auto& Input : Inputs)
    {
        FString Source, Destination;
        if (!NormalizeAssetPackage(Input.Key, Source) || !NormalizeAssetPackage(Input.Value, Destination))
        { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid source or destination package/object path")); }
        const FName SourceKey(*Source), DestinationKey(*Destination);
        if (SourceKey == DestinationKey || Sources.Contains(SourceKey) || Destinations.Contains(DestinationKey) || PackageOccupied(Destination))
        { return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Duplicate, self move, or occupied destination: %s -> %s"), *Source, *Destination)); }
        TArray<FAssetData> SourceAssets;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPackageName(SourceKey, SourceAssets);
        if (SourceAssets.Num() != 1 || SourceAssets[0].AssetName.ToString() != FPackageName::GetLongPackageAssetName(Source) ||
            SourceAssets[0].AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName())
        { return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Source must be one registered non-redirector asset: %s"), *Source)); }
        Sources.Add(SourceKey);
        Destinations.Add(DestinationKey);
        FAssetMoveRequest Request;
        Request.Source = Source;
        Request.NewPackagePath = FPackageName::GetLongPackagePath(Destination);
        Request.NewName = FPackageName::GetLongPackageAssetName(Destination);
        Pending->Add(Request);
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("source"), Source);
        Item->SetStringField(TEXT("destination"), Destination);
        Preview.Add(MakeShared<FJsonValueObject>(Item));
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("dry_run"), bDryRun);
    Result->SetBoolField(TEXT("fixup_redirectors"), bFixup);
    Result->SetNumberField(TEXT("count"), Pending->Num());
    Result->SetArrayField(TEXT("moves"), Preview);
    if (bDryRun) { return Result; }
    const FString JobId = CreateMcpJob(TEXT("move_assets"), Pending->Num());
    auto VerificationStarted = MakeShared<double>(0.0);
    RunJobChunked(JobId, [Pending, bFixup, VerificationStarted](const TSharedPtr<FMcpJobState>& Job) -> bool
    {
        if (Job->Done >= Pending->Num()) { return false; }
        const auto& Request = (*Pending)[Job->Done];
        const FString Destination = Request.NewPackagePath / Request.NewName;
        auto Fail = [&](const FString& Reason) -> bool
        {
            Job->State = TEXT("failed");
            Job->Error = FString::Printf(TEXT("%s -> %s: %s. Earlier moves are not rolled back; inspect both paths before retrying."), *Request.Source, *Destination, *Reason);
            Job->Items.Add(Job->Error);
            return false;
        };
        if (*VerificationStarted > 0.0)
        {
            Job->Phase = TEXT("verify: ") + Request.Source;
            FString Error;
            if (!FixAndVerifyRedirector(Request.Source, true, Error, false))
            {
                if (FPlatformTime::Seconds() - *VerificationStarted < 30.0) { return true; }
                return Fail(TEXT("Verification timed out: ") + Error);
            }
            *VerificationStarted = 0.0;
            Job->Items.Add(Request.Source + TEXT(" -> ") + Destination + TEXT(": saved, redirector cleanup verified"));
            Job->Done++;
            if (Job->Done < Pending->Num()) { return true; }

            // The batch is finished: resave anything still importing a moved path (see
            // ResaveStaleReferencers for why that is needed at all).
            TArray<FString> MovedSources;
            MovedSources.Reserve(Pending->Num());
            for (const auto& Moved : *Pending) { MovedSources.Add(Moved.Source); }
            RecordResaveSweep(Job, MovedSources);
            return false;
        }
        if (PackageOccupied(Destination)) { return Fail(TEXT("Destination became occupied")); }
        TStrongObjectPtr<UObject> Asset(UEditorAssetLibrary::LoadAsset(Request.Source));
        if (!Asset.IsValid() || Asset->GetOutermost()->GetName() != Request.Source) { return Fail(TEXT("Source is missing or changed")); }
        Job->Phase = TEXT("rename: ") + Request.Source;
        TArray<FAssetRenameData> RenameData;
        RenameData.Emplace(Asset.Get(), Request.NewPackagePath, Request.NewName);
        if (!FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().RenameAssets(RenameData))
        { return Fail(TEXT("Rename failed (it may have partially changed the asset)")); }
        Job->Phase = TEXT("save: ") + Destination;
        if (Asset->GetOutermost()->GetName() != Destination || !UEditorAssetLibrary::SaveLoadedAsset(Asset.Get(), false))
        { return Fail(TEXT("Destination save failed after rename")); }
        if (bFixup)
        {
            Job->Phase = TEXT("fixup: ") + Request.Source;
            FString Error;
            FixAndVerifyRedirector(Request.Source, true, Error);
            // Saved referencer dependency updates are consumed on later registry ticks.
            *VerificationStarted = FPlatformTime::Seconds();
            return true;
        }
        Job->Items.Add(FString::Printf(TEXT("%s -> %s: %s"), *Request.Source, *Destination, bFixup ? TEXT("saved, redirector cleanup verified") : TEXT("saved, redirector cleanup disabled")));
        Job->Done++;
        Job->Phase = FString::Printf(TEXT("%d/%d assets"), Job->Done, Job->Total);
        if (Job->Done < Pending->Num()) { return true; }

        // The batch is finished: the same resave sweep as the verified path above. A folder move
        // (fixup disabled) lands here, and it is the path that moved Haeretica's 257 hint textures.
        TArray<FString> MovedSources;
        MovedSources.Reserve(Pending->Num());
        for (const auto& Moved : *Pending) { MovedSources.Add(Moved.Source); }
        RecordResaveSweep(Job, MovedSources);
        return false;
    });
    Result->SetStringField(TEXT("job_id"), JobId);
    Result->SetStringField(TEXT("state"), TEXT("queued"));
    return Result;
}

// resave_packages: load + save packages (explicit list, or everything under a path). Loading
// resolves redirector imports and saving writes the new paths - the scriptable "load +
// resave every referencing package" before deleting redirectors. Async; poll get_job_status.
TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleResavePackages(const TSharedPtr<FJsonObject>& Params)
{
    if (AssetMutationBusy()) { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Another asset mutation job is running")); }
    // Resave works at the asset level: UEditorAssetLibrary::SaveAsset loads the asset (resolving
    // redirector imports through the new path) and saves its package. Preferred over
    // UEditorLoadingAndSavingUtils, whose header is not exported on this engine's path.
    TArray<FString> AssetPaths;

    FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARModule.Get();

    const TArray<TSharedPtr<FJsonValue>>* PkgsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("packages"), PkgsArr) && PkgsArr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *PkgsArr)
        {
            if (!Value.IsValid()) { continue; }
            const FString PackageName = Value->AsString();
            TArray<FAssetData> PkgAssets;
            AR.GetAssetsByPackageName(FName(*PackageName), PkgAssets);
            if (PkgAssets.Num() == 0)
            {
                // Not in the registry (e.g. a non-asset package); try the path directly.
                AssetPaths.Add(PackageName);
            }
            for (const FAssetData& AssetData : PkgAssets)
            {
                AssetPaths.Add(AssetData.GetObjectPathString());
            }
        }
    }

    if (AssetPaths.Num() == 0)
    {
        FString Path = TEXT("/Game");
        Params->TryGetStringField(TEXT("path"), Path);
        bool bRecursive = true;
        if (Params->HasField(TEXT("recursive"))) { bRecursive = Params->GetBoolField(TEXT("recursive")); }

        FARFilter Filter;
        Filter.PackagePaths.Add(*Path);
        Filter.bRecursivePaths = bRecursive;

        TArray<FAssetData> Assets;
        AR.GetAssets(Filter, Assets);
        for (const FAssetData& Asset : Assets)
        {
            AssetPaths.Add(Asset.GetObjectPathString());
        }
    }

    if (AssetPaths.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No packages to resave: pass 'packages' or a 'path' with assets under it"));
    }

    TSharedPtr<TArray<FString>> Pending = MakeShared<TArray<FString>>(MoveTemp(AssetPaths));
    const FString JobId = CreateMcpJob(TEXT("resave_packages"), Pending->Num());

    RunJobChunked(JobId, [Pending](const TSharedPtr<FMcpJobState>& Job) -> bool
    {
        if (Job->Done >= Pending->Num()) { return false; }
        const FString& AssetPath = (*Pending)[Job->Done];

        UObject* Loaded = UEditorAssetLibrary::LoadAsset(AssetPath);
        UPackage* LoadedPackage = Loaded ? Loaded->GetOutermost() : nullptr;
        UWorld* LoadedWorld = LoadedPackage ? UWorld::FindWorldInPackage(LoadedPackage) : nullptr;
        // resave_packages is the second step of a move's fixup, so a map here must take the level
        // save path too, or it reports "saved" while keeping its old import on disk.
        const bool bOk = LoadedWorld
            ? (LoadedWorld->PersistentLevel
                && FEditorFileUtils::SaveLevel(LoadedWorld->PersistentLevel, LoadedPackage->GetName()))
            : (Loaded && UEditorAssetLibrary::SaveLoadedAsset(Loaded, /*bOnlyIfIsDirty=*/false));
        Job->Items.Add(FString::Printf(TEXT("%s: %s"), *AssetPath, bOk ? TEXT("saved") : TEXT("FAILED")));

        Job->Done++;
        Job->Phase = FString::Printf(TEXT("%d/%d assets"), Job->Done, Job->Total);
        return Job->Done < Pending->Num();
    });

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("job_id"), JobId);
    ResultObj->SetStringField(TEXT("state"), TEXT("queued"));
    ResultObj->SetNumberField(TEXT("count"), Pending->Num());
    return ResultObj;
}

// apply_blueprint_plan: build a whole graph from a plan in one call. Ops are routed through
// the shared command router, so the real node handlers do the work and refs wire
// symbolically with no GUID round-trip. Plan schema: see the tool's documentation.
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
        // Defer placement to the placing phase: nodes are created first, then their REAL sizes are
        // measured. Estimating beforehand (the old flat 220x100) undercounted tall nodes (Print
        // String ~270) and produced colliding slots. Only the origin and gaps resolve here.
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
    FString ClassFilter;
    Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
    FString Search;
    Params->TryGetStringField(TEXT("search"), Search);
    int32 Limit = 200;
    if (Params->HasField(TEXT("limit"))) { Limit = (int32)Params->GetNumberField(TEXT("limit")); }
    int32 Offset = 0;
    if (Params->HasField(TEXT("offset"))) { Offset = (int32)Params->GetNumberField(TEXT("offset")); }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(FUnrealMCPCommonUtils::GetEditorWorld(), AActor::StaticClass(), AllActors);

    TArray<TSharedPtr<FJsonValue>> ActorArray;
    int32 Skipped = 0;
    for (AActor* Actor : AllActors)
    {
        if (!Actor) { continue; }
        if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter)) { continue; }
        if (!Search.IsEmpty() && !Actor->GetName().Contains(Search) && !Actor->GetActorLabel().Contains(Search)) { continue; }
        if (Skipped < Offset) { Skipped++; continue; }
        if (ActorArray.Num() >= Limit) { break; }
        ActorArray.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);
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

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorDetails(const TSharedPtr<FJsonObject>& Params)
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
    // Do NOT force SpawnParams.Name: a clash in the editor world raises a modal "name already
    // in use" dialog on the game thread, which blocks the bridge and times the command out.
    // Let UE pick a unique name and set the display label afterwards.
    AActor* NewActor = World->SpawnActor<AActor>(BlueprintClass, SpawnTransform, SpawnParams);
    if (NewActor)
    {
        NewActor->SetActorLabel(*ActorName);
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn blueprint actor"));
}

// Resolve an object path ("/Game/A/Foo", "/Game/A/Foo.Foo", possibly a redirector) to the
// asset it names. Redirectors are followed optionally so queries can run pre-cleanup.
static bool ResolveGraphAsset(const FString& ObjectPath, bool bFollowRedirectors, FAssetData& OutAsset, FString& Error)
{
    const FString Trimmed = ObjectPath.TrimStartAndEnd();
    FString PackageName;
    {
        FString AssetName;
        if (!SplitObjectPath(Trimmed, PackageName, AssetName))
        {
            Error = TEXT("Invalid asset path: ") + Trimmed;
            return false;
        }
    }

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    AR.WaitForPackage(PackageName);

    OutAsset = AR.GetAssetByObjectPath(FSoftObjectPath(Trimmed));
    if (!OutAsset.IsValid())
    {
        // Fall back to package-level lookup; also accept a bare package path.
        TArray<FAssetData> Assets;
        AR.GetAssetsByPackageName(FName(*PackageName), Assets);
        if (Assets.Num() > 0)
        {
            OutAsset = Assets[0];
        }
        else if (bFollowRedirectors && FPackageName::DoesPackageExist(PackageName))
        {
            UPackage* Package = FindPackage(nullptr, *PackageName);
            if (!Package) { Package = LoadPackage(nullptr, *PackageName, LOAD_None); }
            if (Package)
            {
                const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
                if (UObjectRedirector* Redirector = FindObject<UObjectRedirector>(Package, *Name))
                {
                    OutAsset = AR.GetAssetByObjectPath(FSoftObjectPath(Redirector->DestinationObject));
                }
            }
        }
    }

    if (!OutAsset.IsValid())
    {
        Error = TEXT("Asset not found in registry: ") + Trimmed;
        return false;
    }
    return true;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetAssetGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString ObjectPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), ObjectPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }
    FString Direction = TEXT("both");
    Params->TryGetStringField(TEXT("direction"), Direction);
    Direction = Direction.ToLower();
    if (Direction != TEXT("both") && Direction != TEXT("dependencies") && Direction != TEXT("referencers"))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("direction must be 'dependencies', 'referencers', or 'both'"));
    }
    bool bFollowRedirectors = true;
    if (Params->HasField(TEXT("follow_redirectors"))) { bFollowRedirectors = Params->GetBoolField(TEXT("follow_redirectors")); }

    FAssetData Asset;
    FString Error;
    if (!ResolveGraphAsset(ObjectPath, bFollowRedirectors, Asset, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    if (Asset.IsUAsset())
    {
        ResultObj->SetStringField(TEXT("asset"), Asset.GetSoftObjectPath().ToString());
        ResultObj->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
    }
    else
    {
        ResultObj->SetStringField(TEXT("asset"), Asset.PackageName.ToString());
        ResultObj->SetStringField(TEXT("class"), TEXT("Package"));
    }
    ResultObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());

    if (Direction == TEXT("both") || Direction == TEXT("dependencies"))
    {
        TArray<FAssetIdentifier> Dependencies;
        AR.GetDependencies(Asset.PackageName, Dependencies);
        TArray<TSharedPtr<FJsonValue>> DepArray;
        for (const FAssetIdentifier& Dep : Dependencies)
        {
            DepArray.Add(MakeShared<FJsonValueString>(Dep.PackageName.ToString()));
        }
        ResultObj->SetArrayField(TEXT("dependencies"), DepArray);
    }

    if (Direction == TEXT("both") || Direction == TEXT("referencers"))
    {
        TArray<FAssetIdentifier> Referencers;
        AR.GetReferencers(Asset.PackageName, Referencers);
        TArray<TSharedPtr<FJsonValue>> RefArray;
        for (const FAssetIdentifier& Ref : Referencers)
        {
            RefArray.Add(MakeShared<FJsonValueString>(Ref.PackageName.ToString()));
        }
        ResultObj->SetArrayField(TEXT("referencers"), RefArray);
    }

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleDeleteAssets(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* AssetPathsPtr = nullptr;
    if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsPtr) || !AssetPathsPtr || AssetPathsPtr->Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_paths' array"));
    }

    TArray<FString> ObjectPaths;
    for (const TSharedPtr<FJsonValue>& Value : *AssetPathsPtr)
    {
        if (Value.IsValid() && Value->Type == EJson::String)
        {
            ObjectPaths.Add(Value->AsString().TrimStartAndEnd());
        }
    }
    if (ObjectPaths.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_paths contains no valid strings"));
    }
    bool bForce = false;
    if (Params->HasField(TEXT("force"))) { bForce = Params->GetBoolField(TEXT("force")); }

    TArray<FString> Deleted;
    TArray<FString> Failed;
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    for (const FString& ObjectPath : ObjectPaths)
    {
        // SplitObjectPath is built for moves (it yields the destination FOLDER); a delete
        // needs the asset's own package: strip the ".Object" suffix and keep the rest.
        FString PackageName = ObjectPath;
        int32 DotIdx = INDEX_NONE;
        if (PackageName.FindLastChar(TEXT('.'), DotIdx)) { PackageName = PackageName.Left(DotIdx); }
        while (PackageName.EndsWith(TEXT("/"))) { PackageName.RemoveFromEnd(TEXT("/")); }
        if (PackageName.IsEmpty())
        {
            Failed.Add(ObjectPath + TEXT(": invalid path"));
            continue;
        }
        if (!UEditorAssetLibrary::DoesAssetExist(ObjectPath) && !FPackageName::DoesPackageExist(PackageName))
        {
            Failed.Add(ObjectPath + TEXT(": asset does not exist"));
            continue;
        }
        // Deleting a referenced asset silently nulls the reference in its consumers
        // (verified: a mesh's material slot becomes None). Refuse unless the caller
        // opts in with force, and always name what would break.
        AR.WaitForPackage(PackageName);
        TArray<FAssetIdentifier> Referencers;
        AR.GetReferencers(FName(*PackageName), Referencers);
        if (Referencers.Num() > 0 && !bForce)
        {
            TArray<FString> Names;
            for (const FAssetIdentifier& Referencer : Referencers) { Names.Add(Referencer.PackageName.ToString()); }
            Failed.Add(FString::Printf(TEXT("%s: still referenced by %d package(s) [%s]; pass force=true to delete anyway"),
                *ObjectPath, Names.Num(), *FString::Join(Names, TEXT(", "))));
            continue;
        }
        // DeleteLoadedAsset leaves a redirector behind by design; delete the asset then
        // run the same verified redirector cleanup moves use, so nothing stale remains.
        if (!UEditorAssetLibrary::DeleteAsset(ObjectPath))
        {
            Failed.Add(ObjectPath + TEXT(": DeleteAsset failed (referencers or read-only package)"));
            continue;
        }
        FString CleanupError;
        if (!FixAndVerifyRedirector(PackageName, /*bDelete*/ true, CleanupError, /*bApplyFixup*/ false))
        {
            Failed.Add(ObjectPath + TEXT(": deleted but redirector cleanup failed: ") + CleanupError);
            continue;
        }
        Deleted.Add(ObjectPath);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    const bool bAllDeleted = Failed.Num() == 0;
    ResultObj->SetBoolField(TEXT("success"), bAllDeleted || (bForce && Deleted.Num() > 0));
    ResultObj->SetNumberField(TEXT("deleted"), Deleted.Num());
    ResultObj->SetNumberField(TEXT("failed"), Failed.Num());
    if (!bAllDeleted)
    {
        ResultObj->SetStringField(TEXT("error"), FString::Join(Failed, TEXT("; ")));
    }
    TArray<TSharedPtr<FJsonValue>> DeletedArray;
    for (const FString& P : Deleted) { DeletedArray.Add(MakeShared<FJsonValueString>(P)); }
    ResultObj->SetArrayField(TEXT("deleted_paths"), DeletedArray);
    TArray<TSharedPtr<FJsonValue>> FailedArray;
    for (const FString& P : Failed) { FailedArray.Add(MakeShared<FJsonValueString>(P)); }
    ResultObj->SetArrayField(TEXT("failures"), FailedArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleConsoleCommand(const TSharedPtr<FJsonObject>& Params)
{
    FString Command;
    if (!Params->TryGetStringField(TEXT("command"), Command))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'command' parameter"));
    }
    Command = Command.TrimStartAndEnd();

    // Allowlist by prefix: editor-world state queries and scalability/viewmode tweaks only.
    // Mutating console commands (exec, quit, Log off, gunit, etc.) stay out of reach.
    static const TCHAR* AllowedPrefixes[] = {
        TEXT("stat "), TEXT("show "), TEXT("r."), TEXT("foliage."), TEXT("grass."),
        TEXT("sg."), TEXT("foliageLODDistanceScale"), TEXT("grassDensityScale"),
        TEXT("t.MaxFPS"), TEXT("displayfrequency"), TEXT("HighResShot")
    };
    bool bAllowed = false;
    for (const TCHAR* Prefix : AllowedPrefixes)
    {
        if (Command.StartsWith(Prefix, ESearchCase::IgnoreCase)) { bAllowed = true; break; }
    }
    if (!bAllowed)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Console command not allowlisted: %s (allowed: stat/show/r.*/sg.*/foliage.*/grass.*/t.MaxFPS/HighResShot)"), *Command));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No editor world"));
    }
    // Route through the PIE world too when one is live, so `stat` works during play.
    if (GEditor->PlayWorld)
    {
        GEditor->Exec(GEditor->PlayWorld, *Command);
    }
    const bool bHandled = GEditor->Exec(World, *Command);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("handled"), bHandled);
    ResultObj->SetStringField(TEXT("command"), Command);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleEditorPlay(const TSharedPtr<FJsonObject>& Params)
{
    UEditorEngine* Editor = GEditor;
    if (!Editor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No editor engine"));
    }
    if (Editor->PlayWorld)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetStringField(TEXT("state"), TEXT("already_playing"));
        return ResultObj;
    }

    // FRequestPlaySessionParams defaults to InProcess + PlayInEditor; the editor picks the
    // current map and game mode. The request is consumed on a later editor tick.
    FRequestPlaySessionParams PlayParams;
    Editor->RequestPlaySession(PlayParams);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("state"), TEXT("requested"));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleEditorStop(const TSharedPtr<FJsonObject>& Params)
{
    UEditorEngine* Editor = GEditor;
    if (!Editor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No editor engine"));
    }
    if (!Editor->PlayWorld)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetStringField(TEXT("state"), TEXT("not_playing"));
        return ResultObj;
    }
    Editor->RequestEndPlayMap();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("state"), TEXT("stopping"));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleListLevels(const TSharedPtr<FJsonObject>& Params)
{
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    FARFilter Filter;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("World")));
    Filter.PackagePaths.Add(TEXT("/Game"));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> Levels;
    AR.GetAssets(Filter, Levels);

    TArray<TSharedPtr<FJsonValue>> LevelArray;
    for (const FAssetData& Level : Levels)
    {
        TSharedPtr<FJsonObject> LevelObj = MakeShared<FJsonObject>();
        LevelObj->SetStringField(TEXT("name"), Level.AssetName.ToString());
        LevelObj->SetStringField(TEXT("path"), Level.GetSoftObjectPath().ToString());
        LevelArray.Add(MakeShared<FJsonValueObject>(LevelObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetNumberField(TEXT("count"), LevelArray.Num());
    ResultObj->SetArrayField(TEXT("levels"), LevelArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetCurrentLevel(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No editor world"));
    }
    UPackage* LevelPackage = World->GetOutermost();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("level_name"), World->GetName());
    ResultObj->SetStringField(TEXT("package"), LevelPackage ? LevelPackage->GetName() : FString());
    ResultObj->SetStringField(TEXT("path"), LevelPackage ? LevelPackage->GetName() + TEXT(".") + World->GetName() : FString());
    ResultObj->SetBoolField(TEXT("is_dirty"), LevelPackage ? LevelPackage->IsDirty() : false);
    return ResultObj;
}


 