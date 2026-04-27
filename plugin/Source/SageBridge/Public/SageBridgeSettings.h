#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "SageBridgeSettings.generated.h"

UCLASS(config = SageBridge, defaultconfig, meta = (DisplayName = "Sage Bridge"))
class SAGEBRIDGE_API USageBridgeSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    USageBridgeSettings();

    /** Sage server WebSocket endpoint (server↔plugin bridge port). */
    UPROPERTY(EditAnywhere, Config, Category = "Connection")
    FString ServerUrl;

    /** Per-instance label. CLI override via -SageMCPLabel=<value>. */
    UPROPERTY(EditAnywhere, Config, Category = "Identity")
    FString EditorLabel;

    /** Initial reconnect delay (s). Exponential backoff up to MaxReconnectDelaySeconds. */
    UPROPERTY(EditAnywhere, Config, Category = "Connection",
              meta = (ClampMin = "0.5", ClampMax = "30.0"))
    float InitialReconnectDelaySeconds = 1.0f;

    /** Reconnect delay ceiling (s). */
    UPROPERTY(EditAnywhere, Config, Category = "Connection",
              meta = (ClampMin = "5.0", ClampMax = "300.0"))
    float MaxReconnectDelaySeconds = 30.0f;

    /** Heartbeat ping cadence (s). Server marks connection lost after 2x missed beats. */
    UPROPERTY(EditAnywhere, Config, Category = "Connection",
              meta = (ClampMin = "5.0", ClampMax = "60.0"))
    float HeartbeatIntervalSeconds = 15.0f;

    /** Connect on editor startup. */
    UPROPERTY(EditAnywhere, Config, Category = "Connection")
    bool bAutoConnect = true;

    /** Resolves effective label: CLI > config > empty. */
    [[nodiscard]] FString GetResolvedLabel() const;

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
};
