#pragma once

#include "CoreMinimal.h"

/**
 * Slot identity for the running editor.
 *
 * Per ADR-003 + ADR-014:
 *   slot_id = blake3(project_id || \x00 || canonical_path || \x00 || engine_major)
 *
 * Returns a 64-char lowercase hex string. UTF-8 encodes inputs.
 */
class SAGEBRIDGE_API FSageSlotID
{
public:
    [[nodiscard]] static FString Compute(const FString& ProjectId,
                                          const FString& CanonicalPath,
                                          const FString& EngineMajor);

    /** GUID from `[/Script/EngineSettings.GeneralProjectSettings] ProjectID`. Empty if absent. */
    [[nodiscard]] static FString ResolveProjectId();

    /** Full normalized .uproject path. Symlinks not resolved (Phase 2 polish). */
    [[nodiscard]] static FString ResolveCanonicalPath();

    /** Engine major.minor (e.g. "5.7"). Patch ignored per ADR-003. */
    [[nodiscard]] static FString ResolveEngineMajor();

    /** Convenience: snapshot all three components for the running editor. */
    [[nodiscard]] static FString ComputeForCurrentProject();
};
