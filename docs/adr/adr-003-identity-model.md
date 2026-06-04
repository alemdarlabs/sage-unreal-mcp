# ADR-003: Project Identity Model

**Date:** 2026-04-27
**Status:** Accepted
**Updated by:** ADR-014, ADR-018

## Context

Multi-editor support must distinguish these cases:

- Two instances of the same project are open for host/client testing.
- Two copies of the same project exist at different paths.
- Different projects share the same display name.
- The same project is opened with different Unreal Engine versions.
- A project is moved and should either keep identity or trigger a migration
  prompt.

Naive project-name identity collides. Full-path identity breaks when a project
moves. ProjectID-only identity can merge clones incorrectly.

## Decision

Use a composite slot identifier:

```text
slot_id = blake3(project_id || "\0" || canonical_path || "\0" || engine_major)
```

ADR-014 replaced the original SHA-256 algorithm with Blake3. The component
model remains unchanged.

Inputs:

- `project_id`: Unreal `[GeneralProjectSettings] ProjectID`.
- `canonical_path`: platform-normalized project path.
- `engine_major`: major engine version family, such as `5.4` or `5.5`.

## Additional Rules

- Duplicate ProjectID defaults to separate slots. Explicit merge remains a user
  action.
- Missing ProjectID triggers a user-facing prompt. If the user declines to write
  a ProjectID, Sage can use a synthetic path-based identity and mark it as
  synthetic.
- Path changes create an orphan candidate and should prompt for migration.
  Automatic migration is unsafe because clones can look like moves.

## Consequences

Positive:

- Multi-editor cases are distinguishable.
- Engine version changes do not silently corrupt metadata.
- Clone and move behavior is explicit.

Negative:

- Real shared clones may consume duplicate storage until explicitly merged.
- Synthetic identities require re-index or refresh behavior when project paths
  change.

## Current Status

The KuzuDB graph slot storage originally associated with this model was removed
by ADR-018. The identity model still applies to editor routing, project
disambiguation, and any future persistent project metadata.
