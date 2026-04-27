#include "SageBridgeSubsystem.h"
#include "Connection/SageWebSocketClient.h"
#include "Identity/SageEditorIdentity.h"
#include "SageBridge.h"
#include "SageBridgeSettings.h"
#include "Tools/SageActorTools.h"
#include "Tools/SageAssetTools.h"
#include "Tools/SageComponentTools.h"
#include "Tools/SageEditorTools.h"
#include "Tools/SageMaterialTools.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

void USageBridgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    const USageBridgeSettings* Settings = GetDefault<USageBridgeSettings>();
    Label = Settings->GetResolvedLabel();

    RegisterBuiltinHandlers();
    BuildClientFromSettings();

    UE_LOG(LogSageBridge, Log,
           TEXT("SageBridgeSubsystem initialized (label='%s', auto_connect=%s, handlers=%d)"),
           *Label,
           Settings->bAutoConnect ? TEXT("yes") : TEXT("no"),
           ToolDispatch.NumHandlers());

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

void USageBridgeSubsystem::HandleIncomingMessage(const FString& RawText)
{
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawText);
    TSharedPtr<FJsonObject> Envelope;
    if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid())
    {
        UE_LOG(LogSageBridge, Warning, TEXT("Bridge: malformed JSON: %s"), *RawText);
        return;
    }

    TWeakPtr<FSageWebSocketClient> WeakClient = Client;
    auto SendFn = [WeakClient](const TSharedRef<FJsonObject>& Reply) {
        if (TSharedPtr<FSageWebSocketClient> Pinned = WeakClient.Pin())
        {
            Pinned->SendJson(Reply);
        }
    };

    if (ToolDispatch.HandleEnvelope(Envelope.ToSharedRef(), SendFn))
    {
        return;
    }

    // Other envelope types (welcome / heartbeat_ack / error) just log; full
    // connection state machine arrives in Milestone 1.5.
    FString Type;
    Envelope->TryGetStringField(TEXT("type"), Type);
    UE_LOG(LogSageBridge, Verbose, TEXT("Bridge: recv type='%s'"), *Type);
}

void USageBridgeSubsystem::RegisterBuiltinHandlers()
{
    // editor.ping — round-trip echo, mirrors the mock-plugin contract.
    ToolDispatch.RegisterHandler(TEXT("editor.ping"),
        [](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
        {
            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("echoed_by"), TEXT("plugin"));
            if (Args.IsValid())
            {
                FString Msg;
                if (Args->TryGetStringField(TEXT("message"), Msg))
                {
                    Result->SetStringField(TEXT("message"), Msg);
                }
            }
            return FSageToolDispatch::FOutcome::MakeSuccess(Result);
        });

    // Domain tool handlers (Milestone 1.3c+).
    sage::tools::RegisterActorTools(ToolDispatch);
    sage::tools::RegisterComponentTools(ToolDispatch);
    sage::tools::RegisterAssetTools(ToolDispatch);
    sage::tools::RegisterEditorTools(ToolDispatch);
    sage::tools::RegisterMaterialTools(ToolDispatch);
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
    Client->OnMessageReceived.AddUObject(this, &USageBridgeSubsystem::HandleIncomingMessage);
}
