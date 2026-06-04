# Transactions

How Sage tools mutate engine state safely. See [ADR-006](../adr/adr-006-transaction-layer.md) for canonical decisions.

## Foundation: UTransactor

Unreal's native undo/redo system (`UTransactor`) is what we hook into. Tools execute inside `FScopedTransaction` blocks. On success, the transaction joins the editor's existing undo stack. The user's `Ctrl+Z` reverses MCP-initiated edits in the same stack as their manual edits.

```cpp
// Plugin-side dispatch
void FSageToolDispatch::ExecuteTool(const FToolCall& Call) {
    FScopedTransaction Transaction(LOCTEXT("SageToolLabel", "Sage: ") + FText::FromString(Call.Label));
    
    UObject* Target = ResolveTarget(Call.TargetRef);
    Target->Modify();    // ← takes snapshot; required before mutation
    
    ApplyMutation(Target, Call.Args);
    
    // FScopedTransaction destructor commits at scope exit
}
```

`Modify()` is non-negotiable — it tells UTransactor to snapshot the object's serialized state. Without it, undo doesn't work.

## Single-op vs Multi-step Transactions

### Single-op (default)

Each tool call wraps itself in a transaction. Atomic by definition.

```
modify_actor_property(actor, prop, value)
  → 1 FScopedTransaction
  → 1 entry in editor's undo stack
```

### Multi-step

Explicit boundaries for grouped operations:

```typescript
const tx = begin_transaction(label: "Sage: Setup boss arena")
spawn_actor(bp: "BP_Boss", ..., _tx: tx)
spawn_actor(bp: "BP_Spawner", ..., _tx: tx)
modify_actor_property(boss, "MaxHealth", 5000, _tx: tx)
commit_transaction(tx)    // or rollback_transaction(tx)
```

Plugin-side mapper:

```cpp
class FSageMultiStepTx {
    TUniquePtr<FScopedTransaction> NativeTx;
    TArray<FGuid> SubOpIds;
public:
    explicit FSageMultiStepTx(FText Label)
        : NativeTx(MakeUnique<FScopedTransaction>(Label)) {}
    
    void AddSubOp(FGuid Id) { SubOpIds.Add(Id); }
    void Commit() { NativeTx.Reset(); /* dtor commits */ }
    void Cancel() { NativeTx->Cancel(); NativeTx.Reset(); }
};
```

The outer `FScopedTransaction` wraps all sub-operations. Result: one undo step labeled "Sage: Setup boss arena", not seven.

## Optimistic Locking

Concurrent modification (multiple Claude sessions, or Claude + manual user edit) is detected via per-asset state hashes:

```typescript
modify_actor_property(
  actor: "BP_Enemy_C_1234",
  prop: "Health",
  value: 200,
  _expected_version: "sha:af3c..."   // from previous read
)
```

Plugin-side:

```cpp
FString CurrentHash = ComputePropertyHash(Target);
if (CurrentHash != ExpectedHash) {
    return MakeError("version_conflict", { 
        {"actual", CurrentHash}, 
        {"expected", ExpectedHash} 
    });
}
// proceed with modification
```

A session-scoped flag `verify_before_modify: true` makes `_expected_version` mandatory for that session.

## Auto-Rollback on Error

Any exception or validation failure during transaction execution triggers `Cancel()`:

```cpp
try {
    FScopedTransaction Tx(Label);
    Target->Modify();
    if (!ValidateInput(Args)) {
        Tx.Cancel();
        throw FSageError("validation_failed");
    }
    ApplyMutation(Target, Args);
} catch (...) {
    // FScopedTransaction destructor calls Cancel() on stack unwind
}
```

Multi-step transactions are atomic by default — one sub-failure cancels the whole. Opt-in `_atomic: false` allows partial commits but is discouraged.

## Bulk Operations

`bulk_modify` runs many ops under one transaction:

```typescript
bulk_modify([
  { tool: "modify_actor_property", args: {...} },
  { tool: "spawn_actor", args: {...} },
  ...
], _label: "Sage: bulk patch")
```

Default atomic. Token-optimized response shape:

```json
{
  "tx_id": "tx-...",
  "status": "committed",
  "success_count": 47,
  "failed_count": 0,
  "summary_only": true
}
```

Detail via separate `inspect_transaction(tx_id)`.

## Save Discipline

Tool execution leaves modified assets **dirty**, never auto-saves. The user (or a deliberate `save_assets` call) commits to disk.

Why:
1. User retains final write authority — AI cannot accidentally commit
2. Dirty state is cleanly revertible via `Ctrl+Z` before save
3. Save is the propagation point for cross-instance cache invalidation
4. Source control hooks (`ISourceControlModule::CheckOut`) only run at save time

Tools:
- `save_assets(paths?, dry_run?)` — persist; auto-checkout if needed
- `get_dirty_assets()` — list unsaved
- `discard_changes(paths)` — revert to disk state

## PIE Modification Policy

Modifications during Play In Editor are **hard-rejected** by default:

```cpp
if (GEditor && GEditor->PlayWorld) {
    if (!Call.AllowPie) {
        return MakeError("pie_active", { 
            {"hint", "PIE world modifications are transient. Set _allow_pie: true to override."} 
        });
    }
}
```

Why: PIE world changes don't persist; production data corruption while testing is the worst-case bug class.

## Source Control Integration

`save_assets` attempts auto-checkout via `ISourceControlModule`:

```cpp
ISourceControlModule& SCM = ISourceControlModule::Get();
if (!SCM.GetProvider().IsAvailable()) {
    return Result.WithWarning("source_control_unavailable");
}
TArray<FString> Files = ...;
ECommandResult::Type R = SCM.GetProvider().Execute(
    ISourceControlOperation::Create<FCheckOut>(),
    Files
);
if (R != ECommandResult::Succeeded) {
    return MakeError("needs_manual_checkout", {{"files", Files}});
}
```

Failures return `needs_checkout` for client-side resolution.

## Audit Log

Every transaction recorded in `~/.sage-mcp/slots/<slot_id>/audit.log` (SQLite). See [Database Schema](../archive/legacy-graph/database-schema.md) for table definition. Enables:

- "When did Claude last modify this BP?"
- "What did session X do today?"
- "Revert this transaction" (semantic, see below)

## Revert Semantics

`revert_transaction(tx_id)` does **not** splice into UE's undo history. It generates a **compensating transaction** that re-applies inverse operations:

```
Original tx: spawn_actor + modify_property
Revert tx:   delete_actor (with same GUID) + modify_property (to before-value)
```

Why:
- Splicing UE undo history risks engine invariant violation
- Compensating tx is itself undoable (safe)
- Audit trail integrity preserved

## Edge Cases

| Situation | Behavior |
|---|---|
| Editor crash mid-tx | UTransactor cleans on next start; server marks status `errored` |
| Live Coding while tx open | Plugin pre-LC: commit or cancel open txs (config; default cancel) |
| PIE active (default) | Hard error unless `_allow_pie: true` |
| Read-only asset | Hard error: `read_only_asset` |
| Asset not yet T1-indexed | Tool can still execute; audit logged with `pre_index` flag |
| Connection lost mid-tx | Server marks `abandoned`; reconcile on reconnect |
| Two Claudes modifying same asset | Optimistic lock → second one gets `version_conflict` |
