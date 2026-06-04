#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class IWebSocket;
class FJsonObject;

/**
 * Resilient WebSocket client.
 *
 *   Connect → handshake (JSON `hello`) → heartbeat loop
 *   Auto-reconnect with exponential backoff (initial → max delay)
 *   Heartbeat every IntervalSeconds; on close, schedule reconnect if intended
 *
 * Owned by USageBridgeSubsystem. Runs on GameThread (FWebSocketsModule
 * dispatches callbacks there). All deltas via FTSTicker.
 */
class SAGEBRIDGE_API FSageWebSocketClient : public TSharedFromThis<FSageWebSocketClient>
{
public:
    DECLARE_MULTICAST_DELEGATE(FOnConnected);
    DECLARE_MULTICAST_DELEGATE_OneParam(FOnDisconnected, const FString& /*Reason*/);
    DECLARE_MULTICAST_DELEGATE_OneParam(FOnMessageReceived, const FString& /*RawText*/);

    struct FConfig
    {
        FString Url{TEXT("ws://127.0.0.1:7778/bridge")};
        float InitialReconnectDelaySeconds = 1.0f;
        float MaxReconnectDelaySeconds     = 5.0f;
        float HeartbeatIntervalSeconds     = 15.0f;
        bool  bAutoReconnect               = true;
    };

    FSageWebSocketClient();
    ~FSageWebSocketClient();

    FSageWebSocketClient(const FSageWebSocketClient&) = delete;
    FSageWebSocketClient& operator=(const FSageWebSocketClient&) = delete;

    void Configure(const FConfig& InConfig);
    void Connect();
    void Disconnect();

    void SendJson(const TSharedRef<FJsonObject>& Object);
    void SendRaw(const FString& Payload);

    [[nodiscard]] bool IsConnected() const;
    [[nodiscard]] const FConfig& GetConfig() const { return Config; }

    FOnConnected        OnConnected;
    FOnDisconnected     OnDisconnected;
    FOnMessageReceived  OnMessageReceived;

private:
    bool Tick(float DeltaSeconds);
    void ScheduleReconnect();
    void SendHeartbeat();
    void HandleConnected();
    void HandleConnectionError(const FString& Error);
    void HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
    void HandleMessage(const FString& Message);

    FConfig                       Config;
    TSharedPtr<IWebSocket>        Socket;
    FTSTicker::FDelegateHandle    TickerHandle;
    float TimeUntilReconnect      = -1.0f;
    float TimeUntilHeartbeat      = -1.0f;
    float CurrentReconnectDelay   = 0.0f;
    int32 ConsecutiveFailures     = 0;
    bool  bConnectIntended        = false;
};
