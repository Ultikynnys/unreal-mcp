#include "MCPStateSnapshot.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Misc/DateTime.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/SWindow.h"

namespace
{
    constexpr float kHeartbeatSeconds = 0.25f;
}

/** Writes the snapshot every heartbeat, off the game thread. */
class FMCPStateSnapshot::FHeartbeatRunnable : public FRunnable
{
public:
    explicit FHeartbeatRunnable(FMCPStateSnapshot& InOwner) : Owner(InOwner) {}

    virtual uint32 Run() override
    {
        while (!bStop)
        {
            Owner.WriteSnapshot();
            FPlatformProcess::Sleep(kHeartbeatSeconds);
        }
        return 0;
    }

    virtual void Stop() override { bStop = true; }

private:
    FMCPStateSnapshot& Owner;
    FThreadSafeBool bStop{false};
};

FMCPStateSnapshot& FMCPStateSnapshot::Get()
{
    static FMCPStateSnapshot Instance;
    return Instance;
}

FString FMCPStateSnapshot::SnapshotPath()
{
    return FPaths::ProjectSavedDir() / TEXT("MCP") / TEXT("bridge_state.json");
}

void FMCPStateSnapshot::Start()
{
    if (bRunning) { return; }
    bRunning = true;
    StartSeconds = FPlatformTime::Seconds();

    {
        FScopeLock Lock(&Mutex);
        Snapshot = FState();
        Snapshot.StartedAt = FDateTime::UtcNow().ToIso8601();
        Snapshot.LastGameThreadTickSeconds = StartSeconds;
    }

    Heartbeat = new FHeartbeatRunnable(*this);
    HeartbeatThread = FRunnableThread::Create(Heartbeat, TEXT("UnrealMCPStateSnapshot"), 0, TPri_BelowNormal);

    // The game-thread side: stamps the heartbeat and notices a modal. If the game thread
    // blocks, this stops being called and the stall time starts climbing - which is the
    // whole point of the file.
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([this](float Delta) { return TickGameThread(Delta); }),
        kHeartbeatSeconds);
}

void FMCPStateSnapshot::Stop()
{
    if (!bRunning) { return; }
    bRunning = false;

    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }

    if (Heartbeat) { Heartbeat->Stop(); }
    if (HeartbeatThread)
    {
        HeartbeatThread->WaitForCompletion();
        delete HeartbeatThread;
        HeartbeatThread = nullptr;
    }
    delete Heartbeat;
    Heartbeat = nullptr;
}

bool FMCPStateSnapshot::TickGameThread(float /*DeltaSeconds*/)
{
    FString ModalTitle;
    if (FSlateApplication::IsInitialized())
    {
        if (TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow())
        {
            ModalTitle = Modal->GetTitle().ToString();
        }
    }

    FScopeLock Lock(&Mutex);
    Snapshot.LastGameThreadTickSeconds = FPlatformTime::Seconds();
    Snapshot.ModalTitle = ModalTitle;
    return bRunning; // keep ticking while running
}

void FMCPStateSnapshot::MarkDispatchBegin(const FString& Command)
{
    FScopeLock Lock(&Mutex);
    Snapshot.State = TEXT("busy");
    Snapshot.Command = Command;
    Snapshot.CommandStartSeconds = FPlatformTime::Seconds();
}

void FMCPStateSnapshot::MarkDispatchEnd(const FString& Command, bool bSuccess, const FString& Error)
{
    FScopeLock Lock(&Mutex);
    Snapshot.State = TEXT("idle");
    Snapshot.Command.Reset();
    Snapshot.LastCommand = Command;
    Snapshot.LastCommandSeconds = FPlatformTime::Seconds() - Snapshot.CommandStartSeconds;
    Snapshot.LastError = bSuccess ? FString() : Error;
}

void FMCPStateSnapshot::MarkRefused(const FString& Command, const FString& Reason)
{
    FScopeLock Lock(&Mutex);
    Snapshot.State = TEXT("idle");
    Snapshot.LastCommand = Command;
    Snapshot.LastError = Reason;
}

void FMCPStateSnapshot::WriteSnapshot()
{
    FString State, Command, LastCommand, LastError, ModalTitle, StartedAt;
    double CommandStart = 0.0, LastCommandSeconds = 0.0, LastTick = 0.0;
    {
        FScopeLock Lock(&Mutex);
        State = Snapshot.State;
        Command = Snapshot.Command;
        CommandStart = Snapshot.CommandStartSeconds;
        LastCommand = Snapshot.LastCommand;
        LastCommandSeconds = Snapshot.LastCommandSeconds;
        LastError = Snapshot.LastError;
        ModalTitle = Snapshot.ModalTitle;
        StartedAt = Snapshot.StartedAt;
        LastTick = Snapshot.LastGameThreadTickSeconds;
    }

    const double Now = FPlatformTime::Seconds();
    const double Stalled = FMath::Max(0.0, Now - LastTick);

    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
    Json->SetStringField(TEXT("project"), FApp::GetProjectName());
    Json->SetStringField(TEXT("started_at"), StartedAt);
    Json->SetStringField(TEXT("updated_at"), FDateTime::UtcNow().ToIso8601());
    Json->SetStringField(TEXT("state"), State);
    Json->SetStringField(TEXT("command"), Command);
    Json->SetNumberField(TEXT("command_elapsed_seconds"), Command.IsEmpty() ? 0.0 : (Now - CommandStart));
    Json->SetStringField(TEXT("last_command"), LastCommand);
    Json->SetNumberField(TEXT("last_command_seconds"), LastCommandSeconds);
    Json->SetStringField(TEXT("last_error"), LastError);
    Json->SetStringField(TEXT("modal_title"), ModalTitle);
    Json->SetNumberField(TEXT("game_thread_stalled_seconds"), Stalled);

    FString Serialized;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

    const FString Path = SnapshotPath();
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);

    // Write beside the target and move, so a reader never sees a half-written file.
    const FString Temp = Path + TEXT(".tmp");
    if (FFileHelper::SaveStringToFile(Serialized, *Temp))
    {
        IFileManager::Get().Move(*Path, *Temp, /*bReplace=*/true, /*bEvenIfReadOnly=*/true);
    }
}
