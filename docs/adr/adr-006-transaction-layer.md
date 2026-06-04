# ADR-006: Transaction Layer

**Date:** 2026-04-27
**Status:** Accepted

## Context

AI-driven editor mutation must be reversible, inspectable, and safe in the face
of partial failure. Unreal already has a transaction and undo model, so Sage
should integrate with it instead of inventing an unrelated rollback mechanism.

## Decision

### Unreal Transactions

Execute editor mutation tools inside `FScopedTransaction` where the Unreal API
supports it. Objects that participate in the change must call `Modify()` so they
join the editor undo stack.

### Explicit Boundaries

Tools should expose transaction boundaries that match the user's mental model.
A multi-step operation such as building an arena should be one undo step, not a
sequence of unrelated tiny undo steps.

### Version Checks

Support optional `_expected_version` checks. A session-scoped
`verify_before_modify` mode can require these checks before mutation.

### Failure Policy

Validation failure or execution failure cancels the transaction. Multi-step
tools are atomic by default.

### Bulk Mutation

Bulk tools are atomic by default and may expose `_atomic: false` only for narrow
cases where partial success is useful and explicit.

### Save Policy

Tools may leave assets dirty, but they must not auto-save by default. The user
or agent must call save tools deliberately.

### PIE Policy

Editor asset mutation is rejected during PIE by default. Tools may expose
`_allow_pie: true` only when runtime mutation is intentional and safe.

### Revert

`revert_transaction` creates a compensating transaction. It does not rewrite
the existing undo stack.

## Consequences

Positive:

- Sage changes integrate with Unreal undo.
- Failed multi-step operations do not leave half-applied state by default.
- Disk writes remain explicit.

Negative:

- Some Unreal APIs have incomplete transaction support.
- Compensating revert creates a new undo entry.
- Concurrent modification still needs optimistic locking where correctness
  matters.
