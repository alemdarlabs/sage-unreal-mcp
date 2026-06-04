#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Editor identity snapshot, sent to the Sage server in the handshake.
 * Matches docs/engineering/api-spec.md handshake schema.
 */
struct SAGEBRIDGE_API FSageEditorIdentity
{
    FString SlotId;
    FString InstanceId;     // "{ProjectName}@{ShortSession}" — unique per running editor
    FString Label;          // resolved per ADR-004 (CLI > config > "")
    FString ProjectId;
    FString ProjectPath;
    FString EngineVersion;  // full "5.7.4" (handshake field; slot_id uses major only)
    FString SessionId;      // 8-char lowercase hex (truncated GUID)
    int32   Pid = 0;
    FDateTime StartedAt;

    /** Snapshot the running editor. */
    [[nodiscard]] static FSageEditorIdentity Snapshot();

    /** Build the handshake JSON envelope (root: {type, version, slot_id, editor}). */
    [[nodiscard]] TSharedRef<FJsonObject> ToHandshakeJson() const;
};
