#include "MCPServerRunnable.h"
#include "UnrealMCPBridge.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "JsonObjectConverter.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformTime.h"

FMCPServerRunnable::FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
    : Bridge(InBridge)
    , ListenerSocket(InListenerSocket)
    , bRunning(true)
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Created server runnable"));
}

FMCPServerRunnable::~FMCPServerRunnable()
{
    // Note: We don't delete the sockets here as they're owned by the bridge
}

bool FMCPServerRunnable::Init()
{
    return true;
}

uint32 FMCPServerRunnable::Run()
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread starting..."));
    
    while (bRunning)
    {
        // UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Waiting for client connection..."));
        
        bool bPending = false;
        if (ListenerSocket->HasPendingConnection(bPending) && bPending)
        {
            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client connection pending, accepting..."));
            
            ClientSocket = MakeShareable(ListenerSocket->Accept(TEXT("MCPClient")));
            if (ClientSocket.IsValid())
            {
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client connection accepted"));
                
                // Set socket options to improve connection stability
                ClientSocket->SetNoDelay(true);
                int32 SocketBufferSize = 65536;  // 64KB buffer
                ClientSocket->SetSendBufferSize(SocketBufferSize, SocketBufferSize);
                ClientSocket->SetReceiveBufferSize(SocketBufferSize, SocketBufferSize);
                
                uint8 Buffer[8192];
                TArray<uint8> Accumulated;
                constexpr int32 MaxCommandBytes = 16 * 1024 * 1024;  // 16 MB hard cap
                while (bRunning)
                {
                    int32 BytesRead = 0;
                    if (ClientSocket->Recv(Buffer, sizeof(Buffer), BytesRead))
                    {
                        if (BytesRead == 0)
                        {
                            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client disconnected (zero bytes)"));
                            break;
                        }

                        // Accumulate across reads: a command larger than one 8192-byte
                        // read arrives in several Recv calls, and parsing a lone read
                        // would drop it (the client then blocks until its socket timeout).
                        Accumulated.Append(Buffer, BytesRead);
                        if (Accumulated.Num() > MaxCommandBytes)
                        {
                            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Command exceeded %d bytes; dropping connection"), MaxCommandBytes);
                            Accumulated.Reset();
                            break;
                        }

                        // Convert the exact accumulated byte count to a string (no in-place
                        // NUL needed), then try to parse.
                        FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Accumulated.GetData()), Accumulated.Num());
                        FString ReceivedText(Converter.Length(), Converter.Get());
                        UE_LOG(LogTemp, Verbose, TEXT("MCPServerRunnable: Buffered %d byte(s)"), Accumulated.Num());

                        TSharedPtr<FJsonObject> JsonObject;
                        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReceivedText);
                        
                        if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
                        {
                            // Complete command: start fresh for the next one.
                            Accumulated.Reset();

                            // Get command type
                            FString CommandType;
                            if (JsonObject->TryGetStringField(TEXT("type"), CommandType))
                            {
                                // Execute command. Guard against a missing or non-object
                                // 'params' field: GetObjectField() yields an invalid object
                                // for those, and every handler dereferences it (null crash).
                                TSharedPtr<FJsonObject> CommandParams;
                                const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
                                if (JsonObject->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
                                {
                                    CommandParams = *ParamsPtr;
                                }
                                else
                                {
                                    CommandParams = MakeShared<FJsonObject>();
                                }
                                FString AccessKey;
                                JsonObject->TryGetStringField(TEXT("access_key"), AccessKey);
                                FString Response = Bridge->ExecuteCommand(CommandType, CommandParams, AccessKey);
                                
                                // Log response for debugging
                                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Sending response: %s"), *Response);
                                
                                // Send response. Send the UTF-8 byte count, not the TCHAR
                                // count: for any non-ASCII character those differ and the old
                                // code truncated the response (client reads partial JSON).
                                FTCHARToUTF8 Utf8Response(*Response);
                                int32 BytesSent = 0;
                                if (!ClientSocket->Send((const uint8*)Utf8Response.Get(), Utf8Response.Length(), BytesSent))
                                {
                                    UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to send response"));
                                }
                                else {
                                    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Response sent successfully, bytes: %d"), BytesSent);
                                }
                            }
                            else
                            {
                                UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Missing 'type' field in command"));
                            }
                        }
                        else
                        {
                            UE_LOG(LogTemp, Verbose, TEXT("MCPServerRunnable: Command not complete yet (%d byte(s) buffered); awaiting more"), Accumulated.Num());
                        }
                    }
                    else
                    {
                        int32 LastError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();
                        // Don't break the connection for WouldBlock error, which is normal for non-blocking sockets
                        bool bShouldBreak = true;
                        
                        // Check for "would block" error which isn't a real error for non-blocking sockets
                        if (LastError == SE_EWOULDBLOCK) 
                        {
                            UE_LOG(LogTemp, Verbose, TEXT("MCPServerRunnable: Socket would block, continuing..."));
                            bShouldBreak = false;
                            // Small sleep to prevent tight loop when no data
                            FPlatformProcess::Sleep(0.01f);
                        }
                        // Check for other transient errors we might want to tolerate
                        else if (LastError == SE_EINTR) // Interrupted system call
                        {
                            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Socket read interrupted, continuing..."));
                            bShouldBreak = false;
                        }
                        else 
                        {
                            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Client disconnected or error. Last error code: %d"), LastError);
                        }
                        
                        if (bShouldBreak)
                        {
                            break;
                        }
                    }
                }
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to accept client connection"));
            }
        }
        
        // Small sleep to prevent tight loop
        FPlatformProcess::Sleep(0.1f);
    }
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread stopping"));
    return 0;
}

void FMCPServerRunnable::Stop()
{
    bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

 