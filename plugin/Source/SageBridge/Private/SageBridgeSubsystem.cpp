#include "SageBridgeSubsystem.h"
#include "Connection/SageWebSocketClient.h"
#include "Identity/SageEditorIdentity.h"
#include "SageBridge.h"
#include "SageBridgeSettings.h"

void USageBridgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    const USageBridgeSettings* Settings = GetDefault<USageBridgeSettings>();
    Label = Settings->GetResolvedLabel();

    BuildClientFromSettings();

    UE_LOG(LogSageBridge, Log,
           TEXT("SageBridgeSubsystem initialized (label='%s', auto_connect=%s)"),
           *Label,
           Settings->bAutoConnect ? TEXT("yes") : TEXT("no"));

    if (Settings->bAutoConnect)
    {
        Client->Connect();
    }
}

void USageBridgeSubsystem::Deinitialize()
{
    if (Client.IsValid())
    {
        Client->Disconnect();
        Client.Reset();
    }
    Super::Deinitialize();
}

bool USageBridgeSubsystem::IsConnected() const
{
    return Client.IsValid() && Client->IsConnected();
}

void USageBridgeSubsystem::Reconnect()
{
    if (Client.IsValid())
    {
        Client->Disconnect();
    }

    const USageBridgeSettings* Settings = GetDefault<USageBridgeSettings>();
    Label = Settings->GetResolvedLabel();

    BuildClientFromSettings();
    Client->Connect();
}

void USageBridgeSubsystem::HandleConnected()
{
    SendHandshake();
}

void USageBridgeSubsystem::SendHandshake()
{
    const FSageEditorIdentity Id = FSageEditorIdentity::Snapshot();
    SlotId = Id.SlotId;
    UE_LOG(LogSageBridge, Log,
           TEXT("Sending handshake (slot_id=%s, label='%s', engine=%s)"),
           *SlotId, *Label, *Id.EngineVersion);
    Client->SendJson(Id.ToHandshakeJson());
}

void USageBridgeSubsystem::BuildClientFromSettings()
{
    const USageBridgeSettings* Settings = GetDefault<USageBridgeSettings>();

    Client = MakeShared<FSageWebSocketClient>();

    FSageWebSocketClient::FConfig Cfg;
    Cfg.Url                          = Settings->ServerUrl;
    Cfg.InitialReconnectDelaySeconds = Settings->InitialReconnectDelaySeconds;
    Cfg.MaxReconnectDelaySeconds     = Settings->MaxReconnectDelaySeconds;
    Cfg.HeartbeatIntervalSeconds     = Settings->HeartbeatIntervalSeconds;
    Client->Configure(Cfg);

    Client->OnConnected.AddUObject(this, &USageBridgeSubsystem::HandleConnected);
}
