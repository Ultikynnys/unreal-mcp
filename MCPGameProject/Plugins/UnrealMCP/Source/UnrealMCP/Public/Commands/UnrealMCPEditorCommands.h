#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "Templates/Function.h"

/**
 * Handler class for Editor-related MCP commands
 * Handles viewport control, actor manipulation, and level management
 */
class UNREALMCP_API FUnrealMCPEditorCommands
{
public:
    FUnrealMCPEditorCommands();

    // Handle editor commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

    // Injected by the bridge (UUnrealMCPBridge::Initialize). Routes a single
    // batched sub-command through the full command surface, so batch_execute can
    // reach blueprint-node / blueprint / project / umg commands, not just editor
    // commands. When unset, batch_execute falls back to this class's own table.
    using FSubCommandRouter = TFunction<TSharedPtr<FJsonObject>(const FString&, const TSharedPtr<FJsonObject>&)>;
    void SetSubCommandRouter(FSubCommandRouter InRouter) { SubCommandRouter = MoveTemp(InRouter); }

private:
    // Actor manipulation commands
    TSharedPtr<FJsonObject> HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetActorProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params);

    // Blueprint actor spawning
    TSharedPtr<FJsonObject> HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params);

    // Editor viewport commands
    TSharedPtr<FJsonObject> HandleFocusViewport(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params);

    // Asset / capability inspection
    TSharedPtr<FJsonObject> HandleGetCapabilities(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleQueryAssets(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetAssetDetails(const TSharedPtr<FJsonObject>& Params);

    // Mesh / light spawning
    TSharedPtr<FJsonObject> HandleSpawnMeshActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnLightActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnMeshGrid(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnInstancedMesh(const TSharedPtr<FJsonObject>& Params);

    // Actor mutators
    TSharedPtr<FJsonObject> HandleSetActorMaterial(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorFolder(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteActorsByPrefix(const TSharedPtr<FJsonObject>& Params);

    // Level lifecycle
    TSharedPtr<FJsonObject> HandleCreateLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSaveLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLoadLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteLevel(const TSharedPtr<FJsonObject>& Params);

    // Viewport
    TSharedPtr<FJsonObject> HandleSetViewportCamera(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCaptureViewportScreenshot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCapturePIEScreenshot(const TSharedPtr<FJsonObject>& Params);

    // Misc
    TSharedPtr<FJsonObject> HandleBatchExecute(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleExecutePython(const TSharedPtr<FJsonObject>& Params);

    // Recover from the "restore unsaved files" crash-recovery state that stalls startup
    // after an abnormal shutdown: clears the on-disk recovery data and dismisses the
    // recovery modal so the editor resumes ticking.
    TSharedPtr<FJsonObject> HandleRecoverEditor(const TSharedPtr<FJsonObject>& Params);

    // Asset importing (async job + status polling)
    TSharedPtr<FJsonObject> HandleImportAsset(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetImportStatus(const TSharedPtr<FJsonObject>& Params);

    // Async blueprint-plan jobs (applied in chunks on the core ticker; polled via
    // get_plan_status). See HandleApplyBlueprintPlan for the plan schema.
    TSharedPtr<FJsonObject> HandleApplyBlueprintPlan(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetPlanStatus(const TSharedPtr<FJsonObject>& Params);
    // Applies up to Budget ops of the job and returns true while work remains
    // (the ticker reschedules itself until false).
    bool RunBlueprintPlanChunk(const FString& JobId, int32 Budget);

    // Optional full-command router, set by the bridge (see SetSubCommandRouter).
    // Empty until the bridge injects it, in which case batch_execute uses this
    // class's own editor-only HandleCommand table as a fallback.
    FSubCommandRouter SubCommandRouter;
}; 