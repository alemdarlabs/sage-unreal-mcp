#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"

#include "ToolDispatch/SageToolDispatch.h"

#include "SageBridgeSubsystem.generated.h"

class FSageWebSocketClient;
struct FAssetData;
class FJsonObject;

/**
 * Lifecycle owner. Created automatically when the editor loads the plugin
 * (Type=Editor, LoadingPhase=PostEngineInit).
 *
 * Holds the WebSocket client + the tool dispatch table, sends the handshake
 * on connect, and routes incoming `tool_call` envelopes to registered
 * handlers.
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

    /** Tool dispatch registry. C++ extensions register handlers here. */
    [[nodiscard]] FSageToolDispatch& GetToolDispatch() { return ToolDispatch; }
    [[nodiscard]] const FSageToolDispatch& GetToolDispatch() const { return ToolDispatch; }

private:
    void HandleConnected();
    void SendHandshake();
    void HandleIncomingMessage(const FString& RawText);
    void RegisterBuiltinHandlers();

    void BuildClientFromSettings();

    // AssetRegistry delta hooks (Phase 2.3b). Bound on Initialize, unbound
    // on Deinitialize. Each fires a bridge `event` envelope so the server
    // can patch the slot's knowledge graph in place.
    void BindAssetRegistryDeltaHooks();
    void UnbindAssetRegistryDeltaHooks();
    void OnAssetAddedHook(const FAssetData& Data);
    void OnAssetRemovedHook(const FAssetData& Data);
    void OnAssetRenamedHook(const FAssetData& Data, const FString& OldObjectPath);
    void SendDeltaEvent(const FString& Kind, TSharedRef<FJsonObject> Payload);

    TSharedPtr<FSageWebSocketClient> Client;
    FSageToolDispatch                ToolDispatch;
    FString                          SlotId;
    FString                          Label;
};
