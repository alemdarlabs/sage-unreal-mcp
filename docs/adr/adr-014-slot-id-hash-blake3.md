# ADR-014: Slot ID Hash - Blake3

**Date:** 2026-04-28
**Status:** Accepted
**Supersedes:** ADR-003 hash algorithm only

## Context

ADR-003 originally specified SHA-256 for slot identity:

```text
sha256(project_id || canonical_path || engine_major)
```

During Unreal plugin scaffolding, UE 5.x source inspection showed that Blake3 is
available through Unreal's built-in hash utilities.

## Decision

Use Blake3 for slot identity hashing:

```text
slot_id = blake3(project_id || "\0" || canonical_path || "\0" || engine_major)
```

Only the hash algorithm changes. ADR-003's identity components remain active.

## Rationale

1. Unreal provides Blake3 in engine code, so the plugin does not need an extra
   hashing dependency.
2. Blake3 provides modern cryptographic strength and a 256-bit output.
3. It is faster than SHA-256 for this workload.
4. vcpkg can provide server-side parity if needed.
5. Using Unreal's built-in implementation avoids maintaining a bundled SHA-256
   implementation.

## Consequences

Positive:

- No external plugin dependency.
- Faster identity hashing.
- Clear parity path for the server.

Negative:

- Blake3 is less universally familiar than SHA-256.
- Documentation must mention that ADR-014 supersedes only the algorithm in
  ADR-003.

## Notes

Use `\0` between components to preserve unambiguous boundaries. Encode input as
UTF-8 on the Unreal side.
