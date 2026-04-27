#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Plugin-side tool dispatch. Receives `tool_call` envelopes from the server,
 * invokes a registered handler, and emits a `tool_result` envelope back via
 * the caller-provided send function.
 *
 * Threading: handlers fire on whatever thread the WebSocket callback delivers
 * (typically GameThread under FWebSocketsModule). Handlers that touch
 * UObjects must marshal to GameThread themselves with AsyncTask if not
 * already there. Phase 1 baseline ships immediate handlers (e.g. echo);
 * Milestone 1.3c onwards layers FScopedTransaction-wrapped mutations.
 */
class SAGEBRIDGE_API FSageToolDispatch
{
public:
    /** Outcome returned by a handler. Success populates Result; failure populates Error. */
    struct FOutcome
    {
        bool                    bSuccess = true;
        TSharedPtr<FJsonObject> Result;
        TSharedPtr<FJsonObject> Error;

        [[nodiscard]] static FOutcome MakeSuccess(TSharedPtr<FJsonObject> InResult);
        [[nodiscard]] static FOutcome MakeError(int32 Code, FString Message);
    };

    using FHandler = TFunction<FOutcome(const TSharedPtr<FJsonObject>& Args)>;
    using FSendFn  = TFunction<void(const TSharedRef<FJsonObject>& Reply)>;

    void RegisterHandler(const FString& ToolName, FHandler Handler);
    [[nodiscard]] bool HasHandler(const FString& ToolName) const;
    [[nodiscard]] int32 NumHandlers() const { return Handlers.Num(); }

    /**
     * Process an envelope. If `type == "tool_call"`, dispatch and emit
     * tool_result via Send; returns true. Otherwise returns false so the
     * caller can route to other handlers (welcome / heartbeat_ack / error).
     */
    bool HandleEnvelope(const TSharedRef<FJsonObject>& Envelope, FSendFn Send);

private:
    TMap<FString, FHandler> Handlers;
};
