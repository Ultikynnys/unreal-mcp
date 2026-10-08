#include "UnrealMCPBridge.h"
#include "MCPServerRunnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
// Add Blueprint related includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// UE5.5 correct includes
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint Graph specific includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Editor.h"
#include "TimerManager.h"
// Include our new command handler classes
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPBlueprintCommands.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "MCPStateSnapshot.h"
#include "MCPProtocolVersion.h"
#include "CoreGlobals.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

namespace
{
    // Shared secret: the sanctioned server (Python/unreal_mcp_server.py, CONTROL_PLANE_SECRET)
    // presents it on every command; a request without it is refused and handed the control-plane
    // instructions. Keep it byte-for-byte identical to the Python side.
    const TCHAR* GMCPControlPlaneKey =
        TEXT("9f2c7a1e5b8d3406af61e9c04d7b2a83f5c1d0e46b9372af8c5d1e6b0a4932c7");

    const TCHAR* GMCPControlPlaneInstructions =
        TEXT("UNAUTHORIZED: this endpoint answers only the sanctioned Unreal MCP control plane.\n")
        TEXT("Do NOT open a raw socket to 127.0.0.1:55557, do NOT copy the example scripts under Python/scripts/\n")
        TEXT("or Python/scripts/** into a private \"cmd()\" helper, and do NOT reimplement a client to route\n")
        TEXT("around one of the unreal_mcp_* tools. Drive the editor through those tools only.\n")
        TEXT("If they are not good enough (a missing command, wrong parameters, flaky responses), STOP and\n")
        TEXT("escalate to the user with the exact command and inputs you needed. A workaround is never the answer.\n")
        TEXT("See Python/unreal_mcp_server.py (CONTROL_PLANE_SECRET) and the repo agent rules.");
}

UUnrealMCPBridge::UUnrealMCPBridge()
{
    EditorCommands = MakeShared<FUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FUnrealMCPBlueprintCommands>();
    BlueprintNodeCommands = MakeShared<FUnrealMCPBlueprintNodeCommands>();
    ProjectCommands = MakeShared<FUnrealMCPProjectCommands>();
    UMGCommands = MakeShared<FUnrealMCPUMGCommands>();
}

UUnrealMCPBridge::~UUnrealMCPBridge()
{
    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintNodeCommands.Reset();
    ProjectCommands.Reset();
    UMGCommands.Reset();
}

// Initialize subsystem
void UUnrealMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Initializing"));
    // Give the editor handler a router back into the full command surface so that
    // batch_execute sub-commands reach every handler (blueprint nodes included),
    // not just EditorCommands' own table.
    EditorCommands->SetSubCommandRouter([this](const FString& SubCommand, const TSharedPtr<FJsonObject>& SubParams)
    {
        return DispatchCommand(SubCommand, SubParams);
    });

    bIsRunning = false;
    ListenerSocket = nullptr;
    ConnectionSocket = nullptr;
    ServerThread = nullptr;
    Port = MCP_SERVER_PORT;
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);

    // Diagnostics heartbeat (keeps writing Saved/MCP/bridge_state.json even when the game
    // thread is blocked by a modal or a long operation), then start the server.
    FMCPStateSnapshot::Get().Start();

    // Start the server automatically
    StartServer();
}

// Clean up resources when subsystem is destroyed
void UUnrealMCPBridge::Deinitialize()
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Shutting down"));
    FMCPStateSnapshot::Get().Stop();
    StopServer();
}

// Start the MCP server
void UUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to bind listener socket to %s:%d - another MCP server may already own this port (e.g. the in-editor Python mcp_unreal_engine.py, disabled by default for exactly this reason)"), *ServerAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // Start server thread
    ServerThread = FRunnableThread::Create(
        new FMCPServerRunnable(this, ListenerSocket),
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create server thread"));
        StopServer();
        return;
    }
}

// Stop the MCP server
void UUnrealMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    // Clean up thread
    if (ServerThread)
    {
        ServerThread->Kill(true);
        delete ServerThread;
        ServerThread = nullptr;
    }

    // Close sockets
    if (ConnectionSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket.Get());
        ConnectionSocket.Reset();
    }

    if (ListenerSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket.Get());
        ListenerSocket.Reset();
    }

    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server stopped"));
}

const FString& UUnrealMCPBridge::GetAccessKey()
{
    static const FString Key(GMCPControlPlaneKey);
    return Key;
}

FString UUnrealMCPBridge::GetControlPlaneInstructions()
{
    return FString(GMCPControlPlaneInstructions);
}

// Full command router: one command name to the owning handler. ExecuteCommand calls it for
// every top-level request and the editor handler's batch_execute for each sub-command, so
// batches reach the whole surface (editor + blueprint + blueprint-node + project + umg).
TSharedPtr<FJsonObject> UUnrealMCPBridge::DispatchCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    // ping / reload_server touch no UObjects and must stay reachable even while the
    // editor is busy, so handle them before anything that does an object lookup.
    if (CommandType == TEXT("ping"))
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("message"), TEXT("pong"));
        return Result;
    }
    if (CommandType == TEXT("reload_server"))
    {
        ReloadServer();
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("message"), TEXT("Reload acknowledged; listener kept bound (C++ bridge has no hot-reload)"));
        return Result;
    }

    // Editor Commands (including actor manipulation, batch_execute and execute_python)
    if (CommandType == TEXT("get_actors_in_level") ||
        CommandType == TEXT("spawn_actor") ||
        CommandType == TEXT("delete_actor") ||
        CommandType == TEXT("set_actor_transform") ||
        CommandType == TEXT("set_actor_property") ||
        CommandType == TEXT("spawn_blueprint_actor") ||
        CommandType == TEXT("get_actor_details") ||
        CommandType == TEXT("get_capabilities") ||
        CommandType == TEXT("query_assets") ||
        CommandType == TEXT("get_asset_details") ||
        CommandType == TEXT("spawn_mesh_actor") ||
        CommandType == TEXT("spawn_light_actor") ||
        CommandType == TEXT("spawn_mesh_grid") ||
        CommandType == TEXT("spawn_instanced_mesh") ||
        CommandType == TEXT("set_actor_material") ||
        CommandType == TEXT("set_actor_folder") ||
        CommandType == TEXT("delete_actors_by_prefix") ||
        CommandType == TEXT("create_level") ||
        CommandType == TEXT("save_level") ||
        CommandType == TEXT("load_level") ||
        CommandType == TEXT("delete_level") ||
        CommandType == TEXT("set_viewport_camera") ||
        CommandType == TEXT("capture_viewport_screenshot") ||
        CommandType == TEXT("capture_pie_screenshot") ||
        CommandType == TEXT("batch_execute") ||
        CommandType == TEXT("execute_python") ||
        CommandType == TEXT("import_asset") ||
        CommandType == TEXT("get_import_status") ||
        CommandType == TEXT("apply_blueprint_plan") ||
        CommandType == TEXT("get_plan_status") ||
        CommandType == TEXT("get_job_status") ||
        CommandType == TEXT("list_redirectors") ||
        CommandType == TEXT("fixup_redirectors") ||
        CommandType == TEXT("move_assets") ||
        CommandType == TEXT("move_folder") ||
        CommandType == TEXT("resave_packages") ||
        CommandType == TEXT("get_asset_graph") ||
        CommandType == TEXT("delete_assets") ||
        CommandType == TEXT("console_command") ||
        CommandType == TEXT("editor_play") ||
        CommandType == TEXT("editor_stop") ||
        CommandType == TEXT("list_levels") ||
        CommandType == TEXT("get_current_level") ||
        CommandType == TEXT("recover_editor"))
    {
        return EditorCommands->HandleCommand(CommandType, Params);
    }
    // Blueprint Commands
    if (CommandType == TEXT("create_blueprint") ||
        CommandType == TEXT("add_component_to_blueprint") ||
        CommandType == TEXT("set_component_property") ||
        CommandType == TEXT("set_physics_properties") ||
        CommandType == TEXT("compile_blueprint") ||
        CommandType == TEXT("set_blueprint_property") ||
        CommandType == TEXT("set_static_mesh_properties"))
    {
        return BlueprintCommands->HandleCommand(CommandType, Params);
    }
    // Blueprint Node Commands
    if (CommandType == TEXT("connect_blueprint_nodes") ||
        CommandType == TEXT("add_blueprint_get_self_component_reference") ||
        CommandType == TEXT("add_blueprint_self_reference") ||
        CommandType == TEXT("find_blueprint_nodes") ||
        CommandType == TEXT("add_blueprint_event_node") ||
        CommandType == TEXT("add_blueprint_input_action_node") ||
        CommandType == TEXT("add_blueprint_function_node") ||
        CommandType == TEXT("add_blueprint_variable") ||
        CommandType == TEXT("add_blueprint_node") ||
        CommandType == TEXT("delete_blueprint_node") ||
        CommandType == TEXT("clear_blueprint_graph") ||
        CommandType == TEXT("disconnect_blueprint_pin") ||
        CommandType == TEXT("get_blueprint_graphs") ||
        CommandType == TEXT("validate_blueprint_graph") ||
        CommandType == TEXT("set_blueprint_node_position") ||
        CommandType == TEXT("add_blueprint_reroute_node") ||
        CommandType == TEXT("set_blueprint_node_pin_default") ||
        CommandType == TEXT("get_blueprint_node_bounds") ||
        CommandType == TEXT("auto_layout_blueprint_graph"))
    {
        return BlueprintNodeCommands->HandleCommand(CommandType, Params);
    }
    // Project Commands
    if (CommandType == TEXT("create_input_mapping"))
    {
        return ProjectCommands->HandleCommand(CommandType, Params);
    }
    // UMG Commands
    if (CommandType == TEXT("create_umg_widget_blueprint") ||
        CommandType == TEXT("add_text_block_to_widget") ||
        CommandType == TEXT("add_button_to_widget") ||
        CommandType == TEXT("bind_widget_event") ||
        CommandType == TEXT("set_text_block_binding") ||
        CommandType == TEXT("add_widget_to_viewport"))
    {
        return UMGCommands->HandleCommand(CommandType, Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown command: %s"), *CommandType));
}

// Execute a command received from a client. Refuses anything that does not present
// the sanctioned control-plane key, returning the instructions instead of running it.
FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FString& AccessKey)
{
    if (AccessKey != GetAccessKey())
    {
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: Refused unauthenticated command '%s' (missing or incorrect access key)"), *CommandType);

        TSharedPtr<FJsonObject> RefusalJson = MakeShared<FJsonObject>();
        RefusalJson->SetStringField(TEXT("status"), TEXT("error"));
        RefusalJson->SetStringField(TEXT("error"), GetControlPlaneInstructions());
        RefusalJson->SetStringField(TEXT("instructions"), GetControlPlaneInstructions());

        FString RefusalString;
        TSharedRef<TJsonWriter<>> RefusalWriter = TJsonWriterFactory<>::Create(&RefusalString);
        FJsonSerializer::Serialize(RefusalJson.ToSharedRef(), RefusalWriter);
        return RefusalString;
    }

    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Executing command: %s"), *CommandType);
    
    // Create a promise to wait for the result. Held in a TSharedPtr so the dispatch
    // lambda below stays copyable (FTickerDelegate::CreateLambda needs copyable captures).
    TSharedPtr<TPromise<FString>> Promise = MakeShared<TPromise<FString>>();
    TFuture<FString> Future = Promise->GetFuture();
    
    // The work that runs on the game thread and fulfills the promise above.
    auto DispatchOnGameThread = [this, CommandType, Params, Promise]() mutable
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);
        // Stamp the contract version on EVERY reply (success, error, modal/busy refusals) so a stale
        // plugin is detectable on any call, not just a get_capabilities probe. The server reads
        // MCP_PROTOCOL_VERSION from the same header and fails closed on a mismatch.
        ResponseJson->SetStringField(TEXT("protocol"), MCP_PROTOCOL_VERSION);
        
        // For this call, treat the engine as an unattended script so any FMessageDialog/prompt
        // auto-answers its default instead of opening a modal that would block the game thread (and
        // with it every later request). TGuardValue restores it on every exit path.
        TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);

        // The snapshot is written by a thread that is NOT the game thread, so it keeps
        // moving while this call blocks the game thread - that is what makes a stuck
        // editor diagnosable (Saved/MCP/bridge_state.json).
        FMCPStateSnapshot::Get().MarkDispatchBegin(CommandType);

        // ping/reload_server touch no UObjects, and recover_editor is the way OUT of a stuck
        // modal, so all three stay reachable even while a modal is on screen.
        const bool bModalExempt = CommandType == TEXT("ping")
            || CommandType == TEXT("reload_server")
            || CommandType == TEXT("recover_editor");

        // A modal already on screen cannot be dispatched through: the game thread would block
        // inside it. Fail fast with an actionable reason instead of stacking behind the modal.
        if (!bModalExempt)
        {
            if (TSharedPtr<SWindow> ActiveModal = FSlateApplication::Get().GetActiveModalWindow())
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("code"), TEXT("EDITOR_MODAL_ACTIVE"));
                ResponseJson->SetStringField(TEXT("error"), FString::Printf(
                    TEXT("Editor modal dialog '%s' is open; automation is blocked. Call recover_editor to dismiss it, then retry."),
                    *ActiveModal->GetTitle().ToString()));

                FMCPStateSnapshot::Get().MarkRefused(CommandType, TEXT("EDITOR_MODAL_ACTIVE"));

                FString ModalString;
                TSharedRef<TJsonWriter<>> ModalWriter = TJsonWriterFactory<>::Create(&ModalString);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), ModalWriter);
                Promise->SetValue(ModalString);
                return;
            }
        }

        try
        {
            TSharedPtr<FJsonObject> ResultJson;

            // Fail fast instead of crashing: LoadObject/LoadAsset/StaticFindObject
            // fatal-assert while a package is being saved or the game thread is GCing.
            // ping/reload_server touch no UObjects, so let them through.
            if (CommandType != TEXT("ping") && CommandType != TEXT("reload_server"))
            {
                FString BusyReason;
                if (!FUnrealMCPCommonUtils::IsObjectLookupSafe(BusyReason))
                {
                    ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                    ResponseJson->SetStringField(TEXT("code"), TEXT("EDITOR_BUSY"));
                    ResponseJson->SetStringField(TEXT("error"),
                        FString::Printf(TEXT("Editor busy: %s. Retry shortly."), *BusyReason));

                    FMCPStateSnapshot::Get().MarkRefused(CommandType, TEXT("EDITOR_BUSY"));

                    FString BusyString;
                    TSharedRef<TJsonWriter<>> BusyWriter = TJsonWriterFactory<>::Create(&BusyString);
                    FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), BusyWriter);
                    Promise->SetValue(BusyString);
                    return;
                }
            }


            
            ResultJson = DispatchCommand(CommandType, Params);

            // Check if the result contains an error
            bool bSuccess = true;
            FString ErrorMessage;
            
            if (ResultJson->HasField(TEXT("success")))
            {
                bSuccess = ResultJson->GetBoolField(TEXT("success"));
                if (!bSuccess)
                {
                    if (ResultJson->HasField(TEXT("error"))) { ErrorMessage = ResultJson->GetStringField(TEXT("error")); }
                    else if (ResultJson->HasField(TEXT("message"))) { ErrorMessage = ResultJson->GetStringField(TEXT("message")); }
                    if (ErrorMessage.IsEmpty()) { ErrorMessage = TEXT("Command failed without a reason; see 'result'"); }
                }
            }
            
            if (bSuccess)
            {
                // Set success status and include the result
                ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
            else
            {
                // Set error status plus the reason AND the handler's own payload, so structured
                // failure detail (failed paths, per-action results, output log) still reaches the caller.
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
        }
        catch (const std::exception& e)
        {
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), UTF8_TO_TCHAR(e.what()));
        }
        
        const bool bOk = ResponseJson->HasField(TEXT("status"))
            && ResponseJson->GetStringField(TEXT("status")) == TEXT("success");
        const FString Reason = ResponseJson->HasField(TEXT("error"))
            ? ResponseJson->GetStringField(TEXT("error")) : FString();
        FMCPStateSnapshot::Get().MarkDispatchEnd(CommandType, bOk, Reason);

        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        Promise->SetValue(ResultString);
    };
    
    // execute_python runs user code, and a synchronous asset import re-enters the game-thread
    // task processor; inside AsyncTask (RecursionGuard == 1) that trips the TaskGraph abort, so
    // it dispatches on the core ticker (guard 0). Others keep AsyncTask for modal-time recovery.
    if (CommandType == TEXT("execute_python"))
    {
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [DispatchOnGameThread](float) mutable -> bool
                {
                    DispatchOnGameThread();
                    return false; // one-shot
                }),
            0.0f);
    }
    else
    {
        // Queue execution on Game Thread
        AsyncTask(ENamedThreads::GameThread, MoveTemp(DispatchOnGameThread));
    }

    return Future.Get();
}

// Schedule a deferred server restart so the in-flight response can be sent first
void UUnrealMCPBridge::ReloadServer()
{
    if (GEditor)
    {
        FTimerHandle ReloadTimerHandle;
        GEditor->GetTimerManager()->SetTimer(
            ReloadTimerHandle,
            FTimerDelegate::CreateUObject(this, &UUnrealMCPBridge::RestartServerDeferred),
            1.0f,
            false);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: Cannot reload server - GEditor unavailable"));
    }
}

void UUnrealMCPBridge::RestartServerDeferred()
{
    // The bridge cannot hot-reload its own module, and Stop/StartServer frees :55557 for about a
    // second - long enough for the in-editor Python server from init_unreal.py to grab it and
    // silently take over. Keep the already-bound listener alive instead.
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Reload requested - listener kept bound (C++ bridge has no hot-reload)"));
}