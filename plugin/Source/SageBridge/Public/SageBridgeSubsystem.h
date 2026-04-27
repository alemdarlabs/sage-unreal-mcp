#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"

#include "SageBridgeSubsystem.generated.h"

class FSageWebSocketClient;

/**
 * Lifecycle owner. Created automatically when the editor loads the plugin
 * (Type=Editor, LoadingPhase=PostEngineInit).
 *
 * Holds the WebSocket client, sends the handshake on connect, exposes
 * connection state to other plugin code (and Blueprints, for ergonomics).
 */
UCLASS()
class SAGEBRIDGE_API USageBridgeSubsystem : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "Sage")
    bool IsConnected() const;

    UFUNCTION(BlueprintCallable, Category = "Sage")
    FString GetSlotId() const { return SlotId; }

    UFUNCTION(BlueprintCallable, Category = "Sage")
    FString GetInstanceLabel() const { return Label; }

    /** Force reconnect (e.g. after settings change). */
    UFUNCTION(BlueprintCallable, Category = "Sage")
    void Reconnect();

private:
    void HandleConnected();
    void SendHandshake();

    void BuildClientFromSettings();

    TSharedPtr<FSageWebSocketClient> Client;
    FString SlotId;
    FString Label;
};
