# Compile Coordination (Live Coding vs Full Restart)

How Sage decides between Live Coding (hot patch) and full editor restart for C++ changes. The trickiest scenario in the system because it touches plugin lifecycle, multi-editor coordination, editor lifecycle integrity, and conversation continuity.

## The Yelpaze: 3 Reload Mechanisms

| Mechanism | Editor restart? | Notes |
|---|---|---|
| **Live Coding** (`Ctrl+Alt+F11`) | ❌ | Hot-patches running editor. UE 5+ default. |
| **Hot Reload** (deprecated) | ❌ but DLL unload+reload | UE 4.27 and earlier; crash-prone. **Sage does not use.** |
| **Full Recompile + Restart** | ✅ | UBT compile → editor close → relaunch |

Sage operates in two modes: **Live Coding** ↔ **Full Restart**.

## Decision Matrix

| Change kind | Live Coding sufficient? | Reason |
|---|---|---|
| `.cpp` function body change | ✅ | Code-only patch, vtable unchanged |
| New `private` member variable | ⚠️ Risky | Memory layout changes; existing UObjects incompatible |
| New `UFUNCTION` (header) | ⚠️ Often OK | Reflection metadata updated; LC patcher handles in UE 5.3+ |
| New `UPROPERTY` | ❌ Restart | Reflection + serialization layout changes |
| `UCLASS()` attribute change | ❌ Restart | Class flags reflection-cached |
| `Build.cs` change | ❌ Restart | Module link graph rebuild |
| `.uplugin` change | ❌ Restart + project regen | Plugin loader |
| `.Target.cs` change | ❌ Restart + UnrealVersionSelector | Target configuration |
| New `.h` with new `UCLASS/USTRUCT` | ❌ Restart | New reflection registration |
| `UINTERFACE` change | ❌ Restart | Interface vtable layout |
| Engine module change | ❌ Restart + recompile engine | Build setup |
| Comment / whitespace only | ✅ | No-op patch |
| Template instantiation change | ⚠️ Case-by-case | LC sometimes can't patch; fallback |

## Decision Logic

```cpp
enum class ECompileStrategy {
    LiveCoding,
    FullRestart,
    FullRestartWithRegen,    // .uplugin / .Target.cs touched
    Probe                    // try LC, escalate on failure
};

ECompileStrategy AnalyzeChanges(const TArray<FFileChange>& Changes) {
    // 1. Configuration touch
    for (const auto& c : Changes) {
        if (c.Path.EndsWith(".uplugin") || c.Path.EndsWith(".Target.cs"))
            return FullRestartWithRegen;
        if (c.Path.EndsWith("Build.cs"))
            return FullRestart;
    }
    
    // 2. Sage's own bridge plugin? Always full restart (don't LC ourselves)
    if (Changes.ContainsAny([&](auto& c){ 
        return c.Path.Contains("/Plugins/SageBridge/"); 
    })) {
        return FullRestart;
    }
    
    // 3. Header changes need reflection diff
    bool HasNewType = false;
    bool HasReflectionAnnotationChange = false;
    
    for (const auto& c : Changes) {
        if (!c.Path.EndsWith(".h")) continue;
        auto Diff = AnalyzeReflectionDiff(c.Before, c.After);
        HasNewType |= Diff.HasNewUClassUStruct;
        HasReflectionAnnotationChange |= Diff.HasUPropertyChange 
                                      || Diff.HasUFunctionSigChange
                                      || Diff.HasUClassFlagChange;
    }
    
    if (HasNewType || HasReflectionAnnotationChange)
        return FullRestart;
    
    // 4. Header touched but no reflection diff → probe
    if (Changes.ContainsAny([&](auto& c){ return c.Path.EndsWith(".h"); }))
        return Probe;
    
    // 5. Only .cpp changes
    return LiveCoding;
}
```

`AnalyzeReflectionDiff` is regex/line-based in V1 (`UCLASS`, `UPROPERTY`, `UFUNCTION` annotation lines). V2 may upgrade to clang AST.

## Live Coding Flow

```
T+0   Claude → Server: compile_and_reload("MyGameModule", _strategy: "auto")
T+1   Server: AnalyzeChanges() → LiveCoding
T+2   Server → Plugin: { request_live_coding, target: "MyGameModule" }
T+3   Plugin: ILiveCodingModule.Compile(...)
T+4   LC running; plugin captures stdout/stderr
T+5   Plugin → Server: streaming output
T+6   Server → Claude: streaming progress (SSE)
T+7   Plugin: OnPatchComplete fired
        ├─ Success → Server: { lc_complete, status: "success" }
        └─ Failure → Server: { lc_complete, status: "failed", reason }
T+8a  Success path:
        Server: AssetRegistry hash check → did reflection change?
              ├─ No → done
              └─ Yes → trigger T2 incremental reindex
T+8b  Failure path:
        Server: escalate decision (default auto-escalate)
              ├─ Auto-escalate → FullRestart flow
              └─ Or report-and-stop (config)
```

Typical LC duration: 2-15 seconds. Compare to full restart 45-90s.

## The Bridge Plugin Self-Reload Edge Case

Sage's own bridge plugin must NEVER be Live Coding'lendi. Reasons:

1. WS connection drops mid-patch → reconnect chaos
2. `StartupModule()` / `ShutdownModule()` not invoked on LC patch — new tools never register
3. Cached pointers (`IAssetRegistry*`, `ITransactor*`) become invalid
4. Heartbeat thread stops, server marks dead

Rule: any change touching `Plugins/SageBridge/` paths → automatic FullRestart.

```cpp
if (IsSageBridgeAffected(Changes))
    return ECompileStrategy::FullRestart;
```

## Compile Error UX

Structured error reporting:

```json
{
  "status": "compile_error",
  "strategy_attempted": "live_coding",
  "errors": [
    {
      "file": "MyGameModule/Source/Public/Enemy.h",
      "line": 42,
      "column": 8,
      "severity": "error",
      "code": "C2065",
      "message": "'NewProp': undeclared identifier",
      "context": ["...", "...", "..."]    // 3 lines surrounding
    }
  ],
  "warnings": [...],
  "build_log_path": "/tmp/sage-build-12345.log",
  "elapsed_ms": 4321
}
```

Token optimization: default 5 errors × 3 context lines. `verbose: true` for full log.

## Multi-Editor + Live Coding

LC patches single process memory. If a module is loaded in two editors, host's LC leaves client's binary stale → DLL hash mismatch potential.

Server policy:

```
fn handle_live_coding_request(target):
  affected = editors_loading(target)
  
  if len(affected) == 1:
    return live_coding(affected[0])
  
  if len(affected) > 1:
    if config.policy == "escalate_on_multi":
      return full_restart_all(affected)  # safer
    else:
      results = parallel([live_coding(e) for e in affected])
      if any(r.failed): return full_restart_all(affected)
      return ok()
```

Default: **multi-editor + LC = full restart escalation**. Power user can force `_strategy: "live_coding_multi"`.

## State Preservation During LC

| State | Outcome |
|---|---|
| Audit / slot metadata | Preserved in server-side SQLite/state where implemented |
| Operation queue | ✅ Preserved; paused during LC |
| Open assets | ✅ Preserved (UE memory) |
| Editor undo stack | ✅ Preserved |
| Plugin internal cache | ✅ (bridge plugin not LC'd) |
| WebSocket connection | ⚠️ Usually preserved; edge cases require reconnect |
| Pending tool calls | ✅ Queued during LC, resumed |
| In-flight transaction | ⚠️ Pre-LC: forced commit/cancel (default cancel) |

Server pre-LC steps:
1. Pause pending tool calls
2. Commit/cancel open transactions
3. Optional audit/job-state checkpoint when implemented

Server post-LC steps:
1. AssetRegistry hash check (reflection diff?)
2. Operation queue resume
3. T2 delta reindex if reflection changed

## Configuration Regen Flow (`.uplugin` / `.Target.cs`)

```
T+0  Server: AnalyzeChanges → FullRestartWithRegen
T+1  Server → Claude: "Configuration changed, project regen required (~30s-3min)"
T+2  Server: SAVE_AND_SHUTDOWN all affected editors
T+3  Server: subprocess UnrealVersionSelector / GenerateProjectFiles
T+4  Server: subprocess UnrealBuildTool full rebuild
T+5  Server: relaunch editors, await reconnect
T+6  Server → Claude: "Done, new module configuration loaded"
```

## Patch Fragmentation

LC is not unlimited. Each patch adds JIT-compiled code segments. After 50-100 patches:
- Editor RAM bloat
- Patch dispatch latency
- Crash risk increases

Plugin tracks `patches_applied_count`. At 30+, server returns `fragmentation_warning: true` in `get_live_coding_status`. At configurable limit (default 50), server auto-escalates next LC request to FullRestart.

Tools:
- `get_live_coding_status() → { available, patches_applied_count, fragmentation_warning, last_patch_at }`
- `suggest_full_restart()` — proactive hint

## Edge Cases

| Situation | Behavior |
|---|---|
| LC mid AssetRegistry change | Plugin queues events; processes after LC |
| User saves manually mid LC | Plugin LC mutex blocks save (UE native) |
| LC fails, escalation, but PIE active | Server: "PIE active, restart loses runtime state. Continue?" |
| Header-inline function change | LC can't patch; FullRestart |
| `GENERATED_BODY` regen needed | UnrealHeaderTool re-run; FullRestart |
| Two LC requests back-to-back | Server queues, sequential dispatch |
| LC timeout (default 30s) | Mark failed, escalate |

## Tool API

```typescript
compile_and_reload(
  target: string,
  _strategy?: "auto" | "live_coding" | "full_restart" | "probe",
  _editors?: string[],
  _on_failure?: "escalate" | "abort" | "ask"
) → CompileResult

analyze_change(diff_or_paths) → {
  recommended_strategy,
  reasoning,
  reflection_changed: boolean,
  config_changed: boolean,
  affected_modules: string[],
  affected_editors: Editor[],
  estimated_time_ms: number
}

get_live_coding_status(_editor?) → {
  available: boolean,
  patches_applied_count: number,
  fragmentation_warning: boolean,
  last_patch_at?: string
}
```

## See Also

- [Architecture](architecture.md) — system context
- [Transactions](transactions.md) — how tx interacts with LC
- [ADR-001](../decisions/adr-001-tech-stack.md) — engine version choice
- [ADR-004 Multi-Editor](../decisions/adr-004-multi-editor.md) — multi-editor compile coordination
