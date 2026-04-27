#include "Connection/SageWebSocketClient.h"
#include "SageBridge.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WebSocketsModule.h"

FSageWebSocketClient::FSageWebSocketClient() = default;

FSageWebSocketClient::~FSageWebSocketClient()
{
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
    if (Socket.IsValid() && Socket->IsConnected())
    {
        Socket->Close();
    }
}

void FSageWebSocketClient::Configure(const FConfig& InConfig)
{
    Config = InConfig;
}

void FSageWebSocketClient::Connect()
{
    if (Socket.IsValid() && Socket->IsConnected())
    {
        UE_LOG(LogSageBridge, Verbose, TEXT("WebSocket already connected; ignoring Connect()"));
        return;
    }

    bConnectIntended = true;
    if (CurrentReconnectDelay <= 0.0f)
    {
        CurrentReconnectDelay = Config.InitialReconnectDelaySeconds;
    }

    if (!FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))
    {
        FModuleManager::Get().LoadModule(TEXT("WebSockets"));
    }

    Socket = FWebSocketsModule::Get().CreateWebSocket(Config.Url);
    Socket->OnConnected().AddSP(AsShared(),         &FSageWebSocketClient::HandleConnected);
    Socket->OnConnectionError().AddSP(AsShared(),   &FSageWebSocketClient::HandleConnectionError);
    Socket->OnClosed().AddSP(AsShared(),            &FSageWebSocketClient::HandleClosed);
    Socket->OnMessage().AddSP(AsShared(),           &FSageWebSocketClient::HandleMessage);

    UE_LOG(LogSageBridge, Log, TEXT("Connecting WebSocket to %s"), *Config.Url);
    Socket->Connect();

    if (!TickerHandle.IsValid())
    {
        TWeakPtr<FSageWebSocketClient> WeakSelf = AsShared();
        TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([WeakSelf](float DeltaSeconds) {
                if (TSharedPtr<FSageWebSocketClient> Self = WeakSelf.Pin())
                {
                    return Self->Tick(DeltaSeconds);
                }
                return false;
            }),
            1.0f);
    }
}

void FSageWebSocketClient::Disconnect()
{
    bConnectIntended = false;
    TimeUntilReconnect = -1.0f;
    if (Socket.IsValid())
    {
        if (Socket->IsConnected())
        {
            Socket->Close();
        }
        Socket.Reset();
    }
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
}

void FSageWebSocketClient::SendJson(const TSharedRef<FJsonObject>& Object)
{
    FString Payload;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
    FJsonSerializer::Serialize(Object, Writer);
    SendRaw(Payload);
}

void FSageWebSocketClient::SendRaw(const FString& Payload)
{
    if (!Socket.IsValid() || !Socket->IsConnected())
    {
        UE_LOG(LogSageBridge, Warning, TEXT("Send dropped: socket not connected"));
        return;
    }
    Socket->Send(Payload);
}

bool FSageWebSocketClient::IsConnected() const
{
    return Socket.IsValid() && Socket->IsConnected();
}

bool FSageWebSocketClient::Tick(float DeltaSeconds)
{
    if (TimeUntilReconnect > 0.0f)
    {
        TimeUntilReconnect -= DeltaSeconds;
        if (TimeUntilReconnect <= 0.0f && bConnectIntended)
        {
            UE_LOG(LogSageBridge, Log, TEXT("Reconnecting WebSocket"));
            Connect();
        }
    }
    if (Socket.IsValid() && Socket->IsConnected() && Config.HeartbeatIntervalSeconds > 0.0f)
    {
        TimeUntilHeartbeat -= DeltaSeconds;
        if (TimeUntilHeartbeat <= 0.0f)
        {
            SendHeartbeat();
            TimeUntilHeartbeat = Config.HeartbeatIntervalSeconds;
        }
    }
    return true;
}

void FSageWebSocketClient::ScheduleReconnect()
{
    if (!Config.bAutoReconnect || !bConnectIntended)
    {
        return;
    }
    TimeUntilReconnect = CurrentReconnectDelay;
    UE_LOG(LogSageBridge, Log, TEXT("Reconnect scheduled in %.1fs (next backoff: %.1fs)"),
           TimeUntilReconnect,
           FMath::Min(CurrentReconnectDelay * 2.0f, Config.MaxReconnectDelaySeconds));
    CurrentReconnectDelay = FMath::Min(CurrentReconnectDelay * 2.0f,
                                       Config.MaxReconnectDelaySeconds);
}

void FSageWebSocketClient::SendHeartbeat()
{
    const TSharedRef<FJsonObject> Beat = MakeShared<FJsonObject>();
    Beat->SetStringField(TEXT("type"), TEXT("heartbeat"));
    Beat->SetNumberField(TEXT("ts"), static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp()));
    SendJson(Beat);
}

void FSageWebSocketClient::HandleConnected()
{
    UE_LOG(LogSageBridge, Log, TEXT("WebSocket connected"));
    ConsecutiveFailures   = 0;
    CurrentReconnectDelay = Config.InitialReconnectDelaySeconds;
    TimeUntilHeartbeat    = Config.HeartbeatIntervalSeconds;
    OnConnected.Broadcast();
}

void FSageWebSocketClient::HandleConnectionError(const FString& Error)
{
    UE_LOG(LogSageBridge, Warning, TEXT("WebSocket connection error: %s"), *Error);
    ++ConsecutiveFailures;
    OnDisconnected.Broadcast(Error);
    ScheduleReconnect();
}

void FSageWebSocketClient::HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
    UE_LOG(LogSageBridge, Log, TEXT("WebSocket closed (status=%d, clean=%s): %s"),
           StatusCode,
           bWasClean ? TEXT("yes") : TEXT("no"),
           *Reason);
    OnDisconnected.Broadcast(Reason);
    if (bConnectIntended)
    {
        ScheduleReconnect();
    }
}

void FSageWebSocketClient::HandleMessage(const FString& Message)
{
    UE_LOG(LogSageBridge, Verbose, TEXT("WebSocket recv: %d bytes"), Message.Len());
    OnMessageReceived.Broadcast(Message);
}
