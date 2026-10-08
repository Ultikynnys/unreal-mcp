#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"

// Diagnostic snapshot of the bridge, written to Saved/MCP/bridge_state.json by a thread that
// is NOT the game thread: when the game thread blocks (a modal, a long save) the heartbeat
// still records the in-flight command, the stall length and the last modal. Plain data only.
class FMCPStateSnapshot
{
public:
    static FMCPStateSnapshot& Get();

    /** Start the heartbeat thread and the game-thread ticker. Safe to call twice. */
    void Start();

    /** Stop the heartbeat thread and unregister the ticker. */
    void Stop();

    /** Path of the snapshot file (Saved/MCP/bridge_state.json). */
    static FString SnapshotPath();

    // Called by the bridge around a dispatch. Game thread.
    void MarkDispatchBegin(const FString& Command);
    void MarkDispatchEnd(const FString& Command, bool bSuccess, const FString& Error);

    /** Refusal paths (modal active / editor busy) also record what happened. */
    void MarkRefused(const FString& Command, const FString& Reason);

private:
    FMCPStateSnapshot() = default;

    // Nested so it can call WriteSnapshot(); declared here, defined in the .cpp.
    class FHeartbeatRunnable;

    /** Game-thread ticker: stamps the heartbeat and remembers the active modal. */
    bool TickGameThread(float DeltaSeconds);

    void WriteSnapshot();

    struct FState
    {
        FString State = TEXT("idle");           // idle | busy
        FString Command;                        // command in flight
        double CommandStartSeconds = 0.0;
        FString LastCommand;
        double LastCommandSeconds = 0.0;
        FString LastError;
        FString ModalTitle;
        double LastGameThreadTickSeconds = 0.0;
        FString StartedAt;
    };

    FCriticalSection Mutex;
    FState Snapshot;
    double StartSeconds = 0.0;

    FHeartbeatRunnable* Heartbeat = nullptr;
    FRunnableThread* HeartbeatThread = nullptr;
    FTSTicker::FDelegateHandle TickerHandle;
    FThreadSafeBool bRunning{false};
};
