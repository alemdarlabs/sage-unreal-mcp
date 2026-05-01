# Lessons

> Per CLAUDE.md §Self-Improvement Loop. Patterns observed during implementation
> that should change future behavior.

## Polling on the agent side is a server-side missing-tool problem

**Symptom**: Post-`restart_editor`, the agent had no way to know when the
plugin handshake completed. Workaround in HeroFlight (and earlier sessions):
`ScheduleWakeup 90s → list_editors → if count==0 wait again`. Editor
opens in 30s → 60s wasted; opens in 200s → premature retry, restart of
the wait loop. Every cycle felt sluggish for no good reason.

**Root**: the right shape for "block until X" is a **server-side blocking
tool**, not agent-side polling. The server already has a `condition_variable`
candidate (sessions map). Polling existed because the corresponding tool
didn't.

**Rule**: When you catch yourself building a polling loop on the agent side,
ask first: *can the server expose a blocking tool that returns the instant
the condition is true?* For sage that's `wait_for_editor(slot_id?,
timeout_ms?)` — `BridgeServer::waitForSession` parks on a cv that
`handleHello` signals. The agent does one tool call instead of N polls;
the server sleeps efficiently on the cv instead of replying to N tools/list
spam.

**How to apply**:
- New "wait until X" need? Add a server tool, not an agent loop.
- The cv MUST also be signaled on shutdown / failure paths (`stop()`),
  otherwise the tool blocks past the timeout and the parent process can't
  exit cleanly.
- Always include a fast path: if the condition is already satisfied at
  call time, return immediately without ever locking the cv. Keeps the
  hot path cheap.
- Pair the blocking tool with a best-effort `notifications/message`
  publisher (spec-canonical envelope) so MCP clients that DO surface
  notifications get an even faster path. The blocking tool is the
  deterministic floor; the notification is the optional ceiling.

## Bridge socket close must fail-fast pending RPCs, not let them time out

**Symptom**: HeroFlight reported "4 paralel tool call → user reject ×4".
The actual sequence: bridge WebSocket closed (1006 abnormal closure from the
plugin side), `BridgeServer::onClientMessage` Close branch erased the session
but **left the 4 pending RPCs sitting on their 30s timeout**. After 30s each
dispatch returned a generic "tool dispatch timeout"; the MCP harness rendered
that as "user reject" — the misleading symptom.

**Root**: The Close case used to do `sessions_.erase(session_id)` and stop —
no walk over `pending_` to fail-fast outstanding promises whose target session
just disappeared. PendingRpc didn't even carry a `session_id` field, so the
walk wasn't expressible. The dispatcher and the close handler shared no
correlation key.

**Rule**:
- Every pending RPC structure carries the originating session id (`PendingRpc::session_id`).
- The close branch walks `pending_` and resolves matching entries with
  `EditorNotConnected (-32001)` immediately — no 30s wait, no misleading
  timeout error code. The actual cause (editor disconnected mid-call)
  surfaces directly.
- Same applies on `stop()`: every pending must be resolved (already covered;
  this is the analogous discipline at shutdown).

**How to apply**: When adding any in-flight tracker keyed by transaction
id, include the *resource* it depends on (session, file lock, pipe). On
resource teardown, walk the tracker and fail-fast — never rely on timeouts
to clean up, they hide the actual cause and look like generic flakiness.

## Stdio transport: stdout has TWO producers, mutex it

**Symptom**: Adding a notification publisher (bridge worker thread → MCP
notification → stdout) on top of an existing stdio reader (main thread →
response → stdout) created an unsynchronized two-writer setup. Without a
mutex, two `std::cout << json << '\n' << flush` calls from different threads
can interleave at any character — a single torn line breaks JSON-RPC framing
in the client, which then drops the connection or pollutes its parse buffer.

**Rule**: All stdout writes go through `StdioMcp::writeJson(envelope)`,
which:
1. Serializes the json with `dump()` outside the mutex (don't hold the lock
   during a potentially slow stringification).
2. Acquires the stdout mutex.
3. Writes the serialized form + `'\n'` + `flush` atomically.

There is exactly **one** legal route to stdout from inside the server.
`std::cout <<` from any other site is a bug.

**How to apply**: When wiring a second producer to a stream that already has
a producer, the answer is always a mutex (or a single-writer queue). Don't
assume "stdout is a stream, that handles it" — `std::ostream` is not
thread-safe; only individual `<<` operations on certain types are atomic at
the OS level for pipes ≤ PIPE_BUF, which is much smaller than a real
JSON-RPC envelope. Treat shared stdout exactly like shared mutable state.



## BP→C++ pipeline: empty the BP before reparenting it

**Symptom**: 2026-05-01 HeroFlight ABP_Player conversion. Reparent →
`bp.refresh_nodes` → save sequence produced 30 orphan pins, then 17
disconnected `Target` pins after orphan removal, then 60+ "Can't connect
pins" errors after the link-migration pass found types had silently
degraded under a Component reparent. Recovery via Autosave restore +
explicit variable retype + a fresh full conversion took ~4 hours.

**Root**: Two correct mental models had not been wired together —
*"the BP must be a shell after conversion (0 graphs/0 vars/0 fns)"* and
*"reparent only walks FName equality."* The naive ordering was
`reparent → refresh-and-pray`. With graphs still present, every stale
pin became an orphan; orphan removal then severed the link the next
node depended on; and meanwhile a Component reparent had silently
retyped a downstream variable from the BP child class to the C++
parent, so the surviving links would have been type-incompatible
anyway. Each individual step was defensible, but their composition
was a trap.

**Rule**: One BP, ten gates. The pipeline is documented in
`docs/bp-cpp-conversion-pipeline.md`. Step 6 (EMPTY) runs *before*
step 7 (REPARENT), not after — so pin orphans are structurally
unreachable when the reparent fires. Step 3 (DEPS READY) verifies
that any class hierarchy the BP touches is fully migrated; this is the
gate that catches the variable-type-silent-degrade case.

**How to apply**:
- New BP→C++ conversion task: read
  `docs/bp-cpp-conversion-pipeline.md` first, every time, even if you
  think you remember the order. The order is the whole point.
- New gate failure mode discovered: add a step to that document, don't
  silently work around it.
- Tool added that makes a gate mechanically reachable (for example
  `bp.clear_graphs` enabling step 6, `bp.set_variable_type` enabling the
  step-3 fix path): document it in the tools section of that file with
  the specific failure mode it addresses.

---

## BP variable FNames are not C++ identifiers

**Symptom**: `BP_ConversionTest` accepts `Variable 01 Black`, `100Damage`,
`Hız`, `class`, `🚀Rocket`, `auto`, `Damage(per sec)` etc. as variable
FNames and compiles cleanly. The BP saves, the variable is settable from
the editor, the value flows. None of those FNames are valid C++
identifiers.

**Root**: UE's BP variable name validation is far looser than C++ —
spaces, non-ASCII characters, leading digits, reserved keywords, control
characters, all permitted by the engine. The conversion target (a
generated C++ UPROPERTY) is much stricter. FName equality is what makes
reparent merge BP variables onto the parent C++ UPROPERTY, so the BP-side
FName must already be the C++ identifier *before* reparent — not a
translation produced at C++-write time.

**Rule**:
- Sanitise the BP first. `bp.sanitize_variable_names dry_run=true`
  surfaces the mapping; `dry_run=false` applies it. The algorithm is
  built into the tool — Turkish transliteration, non-ASCII strip, run
  collapse, leading-digit fix, C++ keyword suffix, 128-char cap, and
  collision resolution by `_2`/`_3` suffixes.
- Both `bp.rename_variable` and `bp.sanitize_variable_names` enforce
  the same 128-char cap and the same identifier rules — they round-trip
  losslessly.
- The C++ header writes the sanitised name, never the BP display name.
  Display continuity, if the designer wants the original visible in the
  editor, lives in `meta=(DisplayName="…")`.

**How to apply**: When sketching a header from a BP dump, run the
sanitizer (dry-run is fine) first and write the C++ to the new names.
The reparent in step 7 will then match without `_0` orphan suffixes.

---

## UE Python `replace_variable_references` is not a rename

**Symptom**: `BlueprintEditorLibrary.replace_variable_references(bp,
old, new)` returned without error, the editor logged
`"X rename: Y → Z"`-shaped messages, but
`bp.list_variables` showed the variable still under its original name.
Subsequent compile reported the variable as missing on the new pins.

**Root**: The Python API's name says it all — *references*. It walks
`K2Node_VariableGet`/`Set` nodes and rewrites their
`VariableReference` to point at the new name. It does NOT touch
`FBPVariableDescription::VarName`. The variable's definition is
unchanged; the references now point at a non-existent variable. UE
Python's BP authoring surface omits a member-variable-rename function
entirely.

**Rule**: Use `bp.rename_variable` (sage), which wraps the C++
`FBlueprintEditorUtils::RenameMemberVariable` — that one updates the
description AND walks the references AND renames `OnRep_<X>` for
replicated variables.

**How to apply**: If the project is BP-authoring-heavy, the sage tool
surface is the right layer for these operations. Don't reach for
`editor.run_python` as a fallback for write-side BP work — its
`BlueprintEditorLibrary` is read-leaning and quietly omits exactly the
write paths you'd need most.

---

## Don't scope-cut for "MVP" reasons

**Symptom**: Proposed Tier B as Blueprint-read-only "to fit a 1-week MVP",
splitting read/write across phases. User pushed back: "bir daha bir MVP'ye
sığdırmak için bir şey yapma, sana ne amk? Sen işini yap!"

**Root**: Sage is a production system, not a sprint MVP. Splitting natural
feature pairs (read/write, get/set, ingest/query) into separate phases to
make a calendar look smaller is engineering theatre — the work doesn't
shrink, it just gets re-labelled. The user wants features delivered whole.

**Rule**:
- Never reach for "MVP", "v1 minimum", "first cut" framing in roadmaps.
- A feature is read+write together. A subsystem is its full surface,
  including the dark corners (TMap, TSubclassOf, asset references, etc).
- Time estimates are fine; using them as a reason to delete scope is not.
- If something genuinely can't ship in one phase, the dependency belongs
  in the prerequisite, not in a future "Tier B+" purgatory.
- The right unit of deferral is "we don't know how to do this yet" or
  "no concrete user need" — not "looks like a lot of work."

**How to apply**: When sketching a roadmap, list every capability the
user might exercise. Group them by shared infrastructure, not by guessed
calendar weeks. Phases land when the work lands; the graph of work is
the plan, the timeline is a side effect.

## UE Blueprint mutation must be on the GameThread

**Symptom**: First call to `bp.add_variable` (FBlueprintEditorUtils::
AddMemberVariable) deadlocked the editor and crashed it on the next tick;
WebSocket dispatch had been calling UE editor APIs from the bridge worker
thread.

**Rule**: All UE editor mutations (UObject Modify, FBlueprintEditorUtils::*,
SCS edits, transaction begin/commit, package marks dirty, etc.) must be
marshalled to the game thread. Wrap dispatch lambdas with
`detail::RunOnGameThread([&]() { ... })`. The Phase-1 SageActorTools
already followed this pattern; SageBlueprintTools missed it on the first
pass and crashed on the very first write call.

Reads-only that touch the reflection registry (`TObjectIterator<UClass>`)
appear to work off-thread, but it's fragile — when in doubt, marshal.

## Don't pass nullptr to FBlueprintEditorUtils::PropertyValueFromString_Direct

**Symptom**: SIGSEGV at 0x8 inside `FBlueprintEditorUtils::
PropertyValueFromString_Direct(FProperty const*, FString const&, unsigned
char*, UObject*, int)` after a duplicate-asset → bp.add_variable sequence.
The first arg was passed `nullptr` to satisfy a "set the default" intent.

**Root**: `_Direct` writes the parsed value into a raw byte buffer offset
of a non-null `FProperty*`. Passing nullptr Property + nullptr buffer
makes it deref the property's class on a null receiver.

**Rule**: BP variable defaults are persistent serialised strings on
`FBPVariableDescription::DefaultValue`. Just set the string field; the
compiler reads it at the next compile pass. Do not call
PropertyValueFromString_Direct unless you have a real FProperty + real
buffer pointer.

## File-local helper shadows the public version

**Symptom**: `nm` showed two `SetUPropertyFromJson` symbols in the plugin
dylib — `sage::tools::detail::SetUPropertyFromJson` (the public form
declared in SageToolHelpers.h) and `sage::tools::(anonymous)::SetUPropertyFromJson`
(a Phase-1 file-local copy in SageActorTools.cpp). All callers in the
file resolved to the anonymous-namespace shadow, never the public one.
Phase 4.0's collection support landed in `detail::` and was simply
unreachable from `modify_actor_property`.

**Root**: When a public helper grows, a stale file-local copy in another
TU silently keeps shipping the old behaviour. Compiler warns nothing
because both symbols are well-formed.

**Rule**:
- Before adding a `SetXFromJson` / `JsonToX` style helper to a file,
  grep the whole project for an existing one with the same role; if it
  exists in a header, use that.
- When refactoring a public helper, search for shadowed copies in
  anonymous namespaces by name (`grep -n 'bool YourFunc(' --include='*.cpp'`)
  before declaring the refactor done.
- `nm <dylib> | c++filt` is the fastest way to detect this — duplicate
  symbol names from "(anonymous namespace)" vs the named namespace are
  the smoking gun.

## UE_LOG `LogTemp` is filtered by default

**Symptom**: `UE_LOG(LogTemp, Log, TEXT("..."))` produced no output in
SageTest.log even at default Display level.

**Rule**: Use a project-specific log category (`LogSageBridge` here);
LogTemp is meant for ad-hoc throwaway prints and is filtered in many
configs. Verbose-level diagnostics for our reflection helpers go through
`LogSageBridge, Verbose` so they're enabled by adding
`-LogCmds="LogSageBridge Verbose"` (or in an .ini) without spamming
the default log.

## UE 5.7 — `AssetDependencyInfo.h` does not exist

**Symptom**: `fatal error: 'AssetRegistry/AssetDependencyInfo.h' file not found`

**Root**: Despite some online docs/snippets referencing it, UE 5.7 does not
ship `AssetDependencyInfo.h`. The dependency-category enum
(`UE::AssetRegistry::EDependencyCategory::{Package, SearchableName,
Manage, ...}`) is declared inside `AssetRegistry/IAssetRegistry.h` — already
pulled in by `AssetRegistryModule.h`.

**Rule**: For AssetRegistry dependency APIs in UE 5.7, the include set is
just `AssetRegistryModule.h` + `IAssetRegistry.h` + `AssetData.h`. Do not
add a separate `AssetDependencyInfo.h`. If a stale snippet asks for one,
the symbol is already visible from the modular include.

## Kuzu Cypher string escape uses `\'`, not `''`

**Symptom**: `Parser exception: Invalid input <CREATE (:Asset {path: '/Game/A''s_Folder/Asset'>: expected rule oC_SingleQuery`

**Root**: Kuzu (0.11) follows C-style string escapes, *not* SQL-style
quote-doubling. PostgreSQL/SQL: `'it''s'` is valid. Kuzu: `'it\'s'`. Backslash
itself doubles to `\\`.

**Rule**: When generating Cypher literals for Kuzu, escape order is `\` →
`\\` first, then `'` → `\'`. The same `escapeCypherStr` works for kuzu's
double-quoted form too if you ever switch.

## UE 5.7 — `UEditorLoadingAndSavingUtils` lives in `FileHelpers.h`

**Symptom**: `fatal error: 'EditorLoadingAndSavingUtils.h' file not found`

**Root**: Despite the class name suggesting a dedicated header, UE 5.7 declares
`UEditorLoadingAndSavingUtils` (UCLASS, BlueprintFunctionLibrary) inside
`Editor/UnrealEd/Public/FileHelpers.h`, alongside `FEditorFileUtils` (a plain
class). Two distinct types, single header.

**Class membership cheat sheet (UE 5.7)**:
- `UEditorLoadingAndSavingUtils::SavePackages / SavePackagesWithDialog / SaveDirtyPackages / ReloadPackages / FullyLoadPackages`
- `FEditorFileUtils::GetDirtyContentPackages / GetDirtyWorldPackages / GetDirtyPackages` (with optional ignore predicate)

**Rule for future tools**: When reaching for a UE editor utility, `grep -n "class.*Foo\|UCLASS"` on the *plausible header* before assuming the path. UE often co-locates class declarations.

## UE template helpers must live in headers

**Symptom**: Refactoring helper template (`RunOnGameThread<Fn>`) into a `.cpp`
broke linking once a second TU started using it.

**Rule**: Function templates instantiated across translation units must be
defined in a header. Anonymous-namespace helpers can stay TU-local; templates
must be `inline` in a `detail::` namespace header.

## Edit tool — only `old_string` / `new_string` / `replace_all`

**Symptom**: `InputValidationError: An unexpected parameter 'new_str_DELETE_THIS'`

**Rule**: Triple-check the Edit tool schema before invoking. Only three
parameters: `file_path`, `old_string`, `new_string`, `replace_all`.

## Monitor pattern — pin to a specific file, not a glob

**Symptom**: Monitor exited prematurely with `=== UAT done ===` while UAT was
still mid-build. `until grep -q "..." /tmp/.../*.output | tail -1; do ...`
matched older completed output files via the glob.

**Rule**: Monitor's `until grep -q PATTERN FILE` must use the **exact file
path** for the background bash invocation, not `*.output`. Pipe-to-tail does
nothing when grep is `-q` (silent).

## UE 5.7 — `ISourceControlModule` does not have a static `IsLoaded`

**Symptom**: `error: no member named 'IsLoaded' in 'ISourceControlModule'; did you mean 'FChaosVDRuntimeModule::IsLoaded'?`

**Rule**: Use `FModuleManager::Get().IsModuleLoaded(TEXT("SourceControl"))` to
check whether the module is loaded before calling `ISourceControlModule::Get()`
(which auto-loads but throws if disabled). Many UE module-singletons follow
this split — module-existence question is `FModuleManager`'s job; provider /
state queries belong on the singleton.

## UE — `ULevel::Actors` is a sparse array

**Symptom**: After `delete_actor`, `get_world.actor_count` did not decrement
(76 → 76 instead of 75). Outliner separately reported 67 / 68 / etc.

**Root**: `ULevel::Actors` is a sparse `TArray<TObjectPtr<AActor>>`. Destroyed
entries become `nullptr` rather than removed; `Num()` includes the holes.

**Rule**: When surfacing actor counts (or iterating), filter null:
```cpp
int32 ValidCount = 0;
for (const AActor* A : Level->Actors) if (A != nullptr) ++ValidCount;
```
Or use `TActorIterator<AActor>(World)` which already skips nulls + handles
streaming sub-levels.

## UE 5.7 — `FAutomationTestFramework::StartTestByName` returns `void`

**Symptom**: `error: value of type 'void' is not contextually convertible to 'bool'`

**Rule**: `StartTestByName(name, roleIndex)` is fire-and-forget; treat the
returned value as void. Test results are reported asynchronously through
the framework's delegates / log — there is no synchronous success bool.

## KuzuDB v0.11 — header incompatible with C++23 + libc++

**Symptom**: Compiling user code that includes `kuzu.hpp` under
`-std=c++23` (AppleClang + libc++ 17+) fails with `static_assert(sizeof(_Tp) >= 0,
"cannot delete an incomplete type")` from
`unique_ptr<kuzu::common::ExtraTypeInfo>::~unique_ptr()`.

**Root**: kuzu.hpp forward-declares `ExtraTypeInfo` and uses it via
`std::unique_ptr` in default-argument positions (e.g. line 2601). Under
C++23, `_LIBCPP_CONSTEXPR_SINCE_CXX23 ~unique_ptr()` becomes constexpr,
and the constexpr static_assert fires at the declaration site rather than
at first call. Pre-C++23 it only fires when actually destructing the
unique_ptr in user code, which kuzu's surface never triggers.

**Rule**: Compile any TU that includes `kuzu.hpp` with
`CXX_STANDARD 20` (kuzu's own build standard). The rest of Sage stays
on C++23 — only the graph translation units need the downgrade. Set per
target:

```cmake
set_target_properties(sage-kuzu-* PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
```

Also: kuzu `Database(path)` v0.11 expects a *file* path, not a directory
— create the parent dir, point at `graph.kuzu` inside it.

## vcpkg first-time install — surprisingly fast on this machine

`ixwebsocket[core,sectransp,ssl]` + zlib + dependencies took 8 seconds via
binary cache; `nlohmann-json + spdlog + cpp-httplib + catch2` initial install
30 seconds. Don't pre-pessimize cmake configure timing on a developer box with
warm vcpkg cache.

## UE 5.7 — FCoreDelegates::ModalMessageDialog has V2 signature

**Symptom**: `BindStatic(&MyHandler)` against
`FCoreDelegates::ModalMessageDialog` fails to compile with "no matching
function" if the handler takes the old `(EAppMsgType, FText, FText)`
signature.

**Root**: UE 5.4+ added an `EAppMsgCategory` first parameter to the
delegate. In 5.7 the bound function MUST be
`(EAppMsgCategory, EAppMsgType, const FText&, const FText&)`.

**Rule**: For the dialog auto-respond hook in `SageDialogTools.cpp`, the
bound function is named `HandleModalDialogV2` and takes the V2 sig. Even
if you don't care about Category, accept it as `/*Category*/` and pass
through to the V1-shaped helper. Header: `GenericPlatform/GenericPlatformMisc.h`
for `EAppMsgCategory`.

```cpp
EAppReturnType::Type HandleModalDialogV2(
    EAppMsgCategory /*Category*/,
    EAppMsgType::Type MsgType,
    const FText& Text, const FText& Title);
FCoreDelegates::ModalMessageDialog.BindStatic(&HandleModalDialogV2);
```

Confirmed live in SageTest UE 5.7.4: hook fires on `FMessageDialog::Open`
calls; auto-respond + default-response paths both verified via
LogSageBridge.

## Test policy — type-matrix coverage for any pin / property handler change

**The mistake**: Phase 4.2-r1's `bp.add_variable` was tested with
`type='int'` and `type='bool'`. Both work via `PinCategory =
FName(*UserStr)` direct assignment. Real / Float / Double need a sub-
category that the direct path doesn't set. The bug stayed latent for
**5 commits** (r2a → r2g/p5) until a smoke test happened to ship
`type='real'`, at which point an Editor crash + 30-minute crash loop
exposed it.

**Rule**: Any change to `MakePinType` / `bp.add_variable` /
`bp.add_local_variable` / `bp.add_function_parameter` / pin-type
construction in general MUST be smoked across the full UE 5.7 type
matrix. Specifically:
- 8 primitives: bool, byte, int, int64, real, string, name, text
- 5 references: object, class, interface, softobject, softclass
  (each with a `type_object` like `/Script/Engine.Actor`)
- ≥4 structs: Vector, Rotator, Transform, LinearColor
- 2 enums: PC_Enum (modern) + PC_Byte-with-UEnum (legacy)
- Each type also in array form (`is_array=true`)
- Followed by `bp.compile` — its assertion path is what catches a
  half-formed pin (e.g. PC_Real with PinSubCategory=None).

**Apply**: `scripts/smoke/type_matrix.py` is the canonical runner.
Before merging any pin-type change run:
```
python3 scripts/smoke/type_matrix.py
```
38/38 must pass + compile errors=0. The runner restores the
DefaultGame.ini it touches from `.sage_bak`, so it's safe to repeat.

**Do not** ship a "I tested with int and bool" claim again. The whole
matrix or it didn't happen.

## Kuzu 0.11 — UNWIND+MATCH×2+CREATE is ~30x slower than COPY FROM CSV for bulk edge insert

**Symptom**: `index_slot` on 8K-asset SageTest took ~30 seconds end-to-
end. Per-stage profiling fingered the edge inserts:
```
DEPENDS_ON insert (16K edges):  12,514 ms
INHERITS_FROM insert (8K edges): 5,999 ms
Class insert (8K nodes):         3,012 ms
Asset insert (8K nodes):         1,913 ms
```
The pattern was UNWIND+MATCH-by-PK×2+CREATE in 200-element batches.
Bumping the batch to 2000 changed nothing — query parse wasn't the
bottleneck. Per-row hash lookups on the PK index + Kuzu's per-CREATE
WAL fsync were.

**Rule**: For node + relationship bulk ingest into Kuzu, write a CSV
to a temp file and run `COPY <Table> FROM '<path>' (HEADER=true)`.
Rel COPY automatically resolves PK columns into internal IDs — no
MATCH needed in the agent's code path.

**Numbers (8K-asset SageTest, Kuzu 0.11):**
```
                         UNWIND/CREATE   COPY FROM CSV   speedup
Asset insert (8K nodes)     1,913 ms        266 ms       7.2x
DEPENDS_ON   (16K edges)   12,514 ms        339 ms      37x
Class        (8K nodes)     3,012 ms        215 ms      14x
INHERITS_FROM (8K edges)    5,999 ms        118 ms      51x
TOTAL                      30,000 ms      4,000 ms      7.5x
```

**Apply**: see `insertAssetsViaCopy / insertDepsViaCopy /
insertClassesViaCopy / insertInheritsFromViaCopy` in
`server/src/graph/asset_indexer.cpp`. Pattern: filesystem temp path
(`std::filesystem::temp_directory_path()` + random suffix), CSV
escape (double-quote field, escape internal quotes by doubling),
header row matching the table's PK / property column names. Best-
effort cleanup with `std::error_code` so a failed COPY doesn't crash
the server on tmp removal.

**Caveat**: COPY assumes the rel CSV's first two columns are the
FROM-table PK and TO-table PK respectively, in declaration order.
For DEPENDS_ON (Asset → Asset) and INHERITS_FROM (Class → Class)
this means `from,to` headers. If you reuse the table for two-hop
joins later, the COPY semantics still resolve the right way.

## Profiling before optimising — UNWIND batch size was a red herring

**Story**: Initial intuition for Phase 4.4 was "30s ingest must mean
per-query overhead — bump batch size from 200 to 2000". Built it,
re-measured: 30s → 28s. **5% improvement.** Real bottleneck was
elsewhere.

**Rule**: Don't speculate-optimise a 30-second pipeline. Add
per-stage `spdlog::info` timing FIRST, look at the histogram, then
attack the worst stage. The two-edit cost of adding/removing the
timing logs is trivial relative to chasing the wrong optimisation.

**Numbers for the same case:**
```
batch=200 (baseline):   wipe 1.1s + Assets 1.9s + Deps 12.5s
                                + Classes 3.0s + Inherits 6.0s
batch=2000 (intuition): wipe 1.2s + Assets 1.9s + Deps ~12s
                                + Classes ~3s + Inherits ~6s
COPY FROM CSV:          wipe 1.2s + Assets 0.27s + Deps 0.34s
                                + Classes 0.22s + Inherits 0.12s
```
The bottleneck was the *per-row hash-lookup + per-row CREATE WAL
fsync* inside the UNWIND, not the per-statement parse cost that
batch sizing addresses.

## UE 5.0+ — PC_Real REQUIRES a PC_Float / PC_Double sub-category, or KismetCompiler asserts

**Symptom**: Adding a BP variable / local variable / function parameter
with `type='real'` (or `'float'` / `'double'`) silently saves a
malformed `FEdGraphPinType { PinCategory: PC_Real, PinSubCategory: None }`.
The next BP compile, asset scan, or duplicate hits
```
Assertion failed: false [File:./Editor/KismetCompiler/Private/KismetCompilerMisc.cpp] [Line: 1453]
Erroneous pin subcategory for PC_Real: None
```
and the editor crashes. If the BP got saved before the crash, the
project enters a CRASH LOOP — every relaunch tries to compile the
corrupt asset and dies again. Recover by deleting the .uasset from
disk before opening the editor.

**Root**: UE 5.0 split the legacy `PC_Float` pin category into
`PC_Real` (the actual type) plus a precision sub-category
(`PC_Float` for 32-bit, `PC_Double` for 64-bit). The compiler reads
the sub-category to lay out memory and rejects `None` outright. The
editor UI sets this automatically when an artist adds a Real variable;
code paths that build `FEdGraphPinType` from a string need to do it
themselves.

**Rule**: NEVER do `PinType.PinCategory = FName(*UserTypeStr)`
directly. Route every type-string through a centralised
`MakePinType(TypeStr, TypeObjStr, bIsArray)` helper that maps:
- `bool / boolean` → `PC_Boolean`
- `int / integer / int32` → `PC_Int`
- `int64` → `PC_Int64`
- `byte` → `PC_Byte`
- `real / float / double` → `PC_Real` + `PinSubCategory = PC_Double`
- `string / str` → `PC_String`
- `name` / `text` → `PC_Name` / `PC_Text`
- `object / class / struct / interface / softobject / softclass`
  → matching `PC_*` (sub-category-object expected separately)
- anything else: pass through as `FName` (compiler will reject bad
  ones with a clearer error than this lurking crash)

See `MakePinType` in SageBlueprintTools.cpp for the canonical impl.

This bug was latent in `bp.add_variable` since Phase 4.2-r1 — only
surfaced in r2g/p5 smoke when Health:real was the trigger type.
Earlier rounds tested with `int / bool / object` types that don't
need a sub-category, so the path stayed cold.

## UE 5.7 — set_variable_properties needs CompileBlueprint at the end, not MarkBlueprintAsStructurallyModified

**Symptom**: Toggling `FBPVariableDescription::PropertyFlags` (CPF_Edit,
CPF_Net, CPF_BlueprintReadOnly, …) and metadata (MD_Tooltip,
MD_FunctionCategory) then calling `MarkBlueprintAsStructurallyModified`
without a follow-up compile leaves the BP's compiled class layout out
of sync with the variable description. Next mutation crashes.

**Rule**: Don't manually `MarkBlueprintAs*Modified` and then leave the
BP uncompiled. Either (a) call `FKismetEditorUtilities::CompileBlueprint(BP)`
after the mutation (UE-MCP pattern, what `set_variable_properties` now
does), or (b) skip the manual Mark entirely and let a subsequent
`bp.compile` do the structural reconcile. Mixing manual Mark with no
compile = corrupt half-state.

## UE 5.7 — manually spawning UK2Node_FunctionEntry into a delegate signature graph CRASHES the editor

**Symptom**: Adding a delegate signature graph via
`FBlueprintEditorUtils::CreateNewGraph` then manually
`NewObject<UK2Node_FunctionEntry>(SigGraph) + AllocateDefaultPins` to make
the graph "look like" a function (so `bp.add_function_parameter` could
attach payload pins) crashed the editor mid-MCP-tool call. Plugin tool
dispatch timed out after 30s, WebSocket dropped with abnormal closure,
editor process gone.

**Root**: `UK2Node_FunctionEntry` derefs its `FunctionReference`
(`UFunction*`) when `AllocateDefaultPins` walks the function's signature
to lay out user-defined pins. A delegate signature graph has no
compiled `UFunction` until the BP is compiled — the field is null and
the codepath is not null-safe.

**Rule**: Never spawn `UK2Node_FunctionEntry` into a `UEdGraph` that
isn't backed by a real `UFunction`. The schema's
`CreateDefaultNodesForGraph` is the only safe path; for delegate
signature graphs in 5.7 it deliberately does NOT create an entry node.
Configure dispatcher payload params via a dedicated tool that goes
through the `FBlueprintEditorUtils` delegate API (or directly via the
`FBPVariableDescription`'s `PinSubCategoryMemberReference`), not via
`bp.add_function_parameter`.

**Apply**: `FindFunctionGraph` deliberately excludes
`DelegateSignatureGraphs` so accidental routing into them via
`bp.add_function_parameter` is impossible. A future r2h
`bp.set_dispatcher_payload_params` tool is the correct surface.

## C++ — helper hoisting when growing a single-TU plugin file

**Symptom**: Adding a new handler section above an existing one that uses
a helper defined further down in the same TU fails with
"use of undeclared identifier" (e.g. `FindFunctionEntry` defined in r2b
section, used in newly added r2e section above it). UE-MCP-style monolithic
TUs are common in the Sage plugin (SageBlueprintTools.cpp >1000 LoC).

**Rule**: When a helper crosses two or more handler sections, hoist its
*definition* into the file's "common helpers" block at the top (where
`ResolveBlueprint` / `FindFunctionGraph` / `FindNodeByGuid` / `FindPin`
live in SageBlueprintTools.cpp). Cheaper than forward declarations, and
keeps the helper logically grouped with its peers.

**How to spot it**: a `grep -n "^Type\* FuncName\|FuncName(" file.cpp`
shows multiple call sites but only one definition far below. If the
definition line number is greater than ANY call site, hoist it.

## UE 5.7 — Slate modal click sim needs InputCore + ApplicationCore in Build.cs

**Symptom**: `FPointerEvent` constructor + `FKeyEvent(EKeys::Escape, ...)`
+ `OnMouseButtonDown/Up` in a plugin Build.cs that lists only
`UnrealEd, EditorSubsystem, ...` fails to link with missing-symbol errors
on `EKeys::*` and `FSlateApplication::ProcessKeyDownEvent`.

**Root**: `EKeys` lives in `InputCore`, `FSlateApplication` lives in
`Slate` + `SlateCore`, modifier-key state in `ApplicationCore`. UnrealEd
brings these transitively for HOSTED tools but not for handler-only
plugin TUs.

**Rule**: Any plugin TU that simulates input or walks the active modal's
widget tree (SButton / STextBlock / SWindow) must add `Slate, SlateCore,
ApplicationCore, InputCore` to `PrivateDependencyModuleNames` in the
`.Build.cs`. See SageBridge.Build.cs (Phase 4.6 r2) for the canonical
form.

## Plugin restart loop — UE Editor must be relaunched after .dylib swap

**Symptom**: `tools/list` returns 200 tools (instead of 456); calling
`audio.create_cue` returns `unknown tool: audio.create_cue` even though
the plugin has registered the handler and the new server schemas list it.

**Root**: macOS keeps the plugin .dylib memory-mapped while UE Editor is
running. A fresh build/copy into `SageTest/Plugins/SageBridge/Binaries/Mac/`
does NOT take effect until the editor process exits and reloads. Server
restart alone is insufficient — plugin handlers live in the editor.

**Rule**: After ANY change that touches `plugin/Source/`, run the
`unreal-close` skill, then `unreal-open`. Wait until the second
`Bridge handshake:` line appears in `/tmp/sage-server.log` before
issuing tool calls — the first one is the old editor's stale connection.
Both the server (if newer) AND the editor must be cycled.

## Phase 4 domain smoke — read C++ before writing Python tests

**Symptom**: First-pass smoke for `animation.read_bone_track` used
`bone` as the JSON field name (matching the high-level concept). The
plugin handler reads `bone_name`. Same drift on `add_curve` (`name` vs
`curve_name`), `add_notify` (no `notify_class`), `add_virtual_bone`
(`source_bone` / `target_bone` vs `parent_name` / `target_name`),
`get_physics_asset` (expects USkeletalMesh path, not the PhysicsAsset
itself). Several iterations of "run → fix → run" before all 46 tools
dispatched cleanly.

**Rule**: When writing a domain smoke (Phase 4 onward), `grep -B1 -A12
"FSageToolDispatch::FOutcome <Tool>Impl"` in the matching
`Sage<Domain>Tools.cpp` BEFORE drafting the call. The argument names in
`Args->TryGetStringField(TEXT("..."))` are the schema, full stop —
documentation guesses don't survive a real call.

**How to apply**: For any new smoke, the workflow is (a) list all
handler names via `grep "RegisterHandler"`, (b) for each non-trivial
handler, read the impl's first 10–15 lines to capture required field
names + plugin-gated `NotAvailable()` paths, (c) wrap plugin-gated
calls in try/except so the smoke can pass when the plugin isn't loaded
in SageTest (PCG, GAS, SmartObjects, PoseSearch are common gaps).

## nlohmann::json brace-init silently corrupts MCP `required` arrays

**Symptom**: First real test against Claude Code MCP client failed with
234/456 tools rejected by Zod validator. Three patterns:
1. `{"path":"name"}` where `["path","name"]` was intended (75 tools)
2. `[["path"]]` where `["path"]` was intended (136 tools)
3. `properties: null` where `properties: {}` was intended (22 tools)

**Root**: nlohmann::json's brace-init constructor disambiguates poorly.
- `{{"a","b"}}` — two strings inside outer brace: nlohmann interprets
  the inner pair as a key:value entry → `{"a":"b"}` object, NOT
  `["a","b"]` array.
- `{{"a"}}` — single string inside outer brace: outer wraps inner array
  → `[["a"]]`, NOT `["a"]`.
- `{}` passed as a `nlohmann::json` argument → constructs as JSON null
  (default-constructed value), NOT empty object. `is_null()` returns
  true; `is_object()` false.

The bug was latent in `phase4_schemas.cpp`'s `obj()` helper. Smoke
tests passed because they invoked `tools/call` directly (which doesn't
re-validate input schemas) — only a strict MCP client (Claude Code
uses Zod) caught it on `tools/list`.

**Rule**:
- For string-array fields like `required`, use
  `std::initializer_list<const char*>` parameters in helpers, then
  build a `nlohmann::json::array()` explicitly inside. This forbids
  the ambiguous brace-init at call sites:
  ```cpp
  static nlohmann::json obj(nlohmann::json props,
                            std::initializer_list<const char*> required = {}) {
      auto req = nlohmann::json::array();
      for (const char* k : required) req.push_back(k);
      ...
  }
  // Call: obj(props, {"path"}) — flat single-brace, unambiguous.
  ```
- For "no properties" object schemas, write
  `nlohmann::json::object()` explicitly, not `{}`.
- For "any JSON value" schema, write
  `{{"description","any JSON value"}}` (real one-key object), not
  `{}` (which is null).

**How to verify before merge**: After any `tools/list`-affecting
change, fetch the JSON and Zod-validate it from the client side, OR run:
```python
import json
data = json.loads(...)
for i,t in enumerate(data['result']['tools']):
    s = t['inputSchema']
    assert s.get('properties') is not None, f"[{i}] {t['name']} props=null"
    r = s.get('required')
    if r is not None:
        assert isinstance(r, list), f"[{i}] {t['name']} required not array"
        for x in r: assert isinstance(x, str), f"[{i}] {t['name']} required[*] not string"
```

**Apply**: This is a paketleme-blocker. ANY MCP server's `tools/list`
output MUST validate against the JSON-Schema-of-JSON-Schema before
shipping; smoke that only exercises `tools/call` is insufficient.

## MCP Streamable HTTP — partial spec implementation surfaces as "Capabilities: none"

**Symptom**: Claude Code MCP panel showed
`Status: ✔ connected · Auth: ✘ not authenticated · Capabilities: none`
plus an OAuth 404. Logs revealed "Failed to open SSE stream: Not Found"
followed by the schema validation errors (the actual blocker).

**Root**: Sage server only implements POST `/mcp` for the JSON-RPC
envelope. The MCP "Streamable HTTP" transport spec also requires:
- `Mcp-Session-Id` response header on initialize (for stateful sessions)
- `Mcp-Protocol-Version` header (negotiation)
- GET `/mcp` SSE long-poll endpoint (for server-pushed notifications)
- DELETE `/mcp` for session termination
- An OAuth metadata discovery endpoint (`.well-known/oauth-authorization-server`)
  — server returns 404, Claude treats as "no auth" (warning, non-blocking)

The schema validation errors blocked tool registration *before* the
SSE stream warning would have mattered. Once schemas are valid, Claude
proceeds even without SSE (stateless fallback).

**Rule**: For paketleme-readiness, sage-server's `http_sse_server.cpp`
must add:
1. `Mcp-Session-Id` (random UUID) on initialize response, echoed by
   subsequent client requests
2. `Mcp-Protocol-Version: 2025-03-26` response header
3. GET `/mcp` endpoint that returns `text/event-stream` (can be empty
   stream — server-pushed notifications are optional capability)
4. (Lower priority) OAuth metadata stub returning `{}` instead of 404
   to silence the discovery warning

Until then, Claude Code works in stateless mode but with reduced
capability awareness; that's fine for dev, not for shipping.

## BridgeServer routing — `getClients()[0]` was a deferred TODO that became a real bug

**Symptom**: Pre-Milestone-1.5b dispatchTool used
`server_->getClients()[0]` — i.e. picked a non-deterministic "first
client" from an `std::set<std::shared_ptr<WebSocket>>`. The
`activeSessionId_` pointer set by `set_active_editor` was *never read*
by the dispatcher. Multi-editor scenarios silently behaved like
"random editor wins."

**Root**: In Phase 1.3a's bridge scaffolding, the comment
`// today dispatchTool() targets the first active session` was a TODO
flag for Milestone 1.5b. But getClients() returns a std::set — there's
no "first" in a set, ordering is hash-bucket-dependent. Worked fine
with one editor (only one element); broke silently with two.

**Rule**:
- Never write `getClients()[0]` or `*set.begin()` for routing — sets
  have no deterministic ordering. If you must pick "any one element"
  for a single-editor convenience path, document the assumption
  explicitly *and* fall through to error on multiple.
- TODO comments like "lands in Milestone X" must be wired to a
  failing test or an explicit error path before merge — silent
  deferred TODOs rot for months.
- Multi-target routing belongs at the **dispatcher** layer
  (`BridgeServer::dispatchTool`), not at each tool's handler. The
  handler is single-session by definition (it's running inside one
  plugin); routing is the bridge's job.

**Apply**: In Sage, EditorSession now carries an `ix::WebSocket* ws`
field; `dispatchTool(target_id_or_label)` resolves explicit > active >
single-implicit > ambiguity-error and uses
`sessions_[targetSessionId].ws` for send().

## DRY middleware injection beats per-tool repetition

**Story**: Adding a uniform parameter (`_editor`) to 200+ tool schemas
had two paths: (a) edit every tool definition source-side; (b) inject
once at the response layer. Path (a) is what the schema validation
disaster (235 invalid schemas, lessons.md "nlohmann brace-init
silently corrupts") was made of — repetition begets typos.

**Rule**: Cross-cutting schema concerns (auth, routing, sessioning,
rate-limit hints) should be applied at the **registry** or
**response** layer, never per-tool. For Sage:
- `MCPServer::onToolsList` injects `_editor` for every
  `tool.remote == true`, skipping tools that already declare it.
- 12 server-side tools (knowledge graph queries, ping, list_editors,
  ...) automatically excluded by the `remote == true` predicate —
  no explicit allowlist needed.
- New remote tools added later inherit `_editor` for free; no
  source-side opt-in.

**Apply**: When tempted to add a parameter to "every tool that ...",
first ask: can the registry/response layer add it once, conditional
on a tool predicate? If yes, do that. Source-side per-tool
repetition is allowed only when the value differs per-tool (which
schema-routing metadata never does).

## C++ proje plugin install: dylib + Source/ ikisi birden gerek

**Symptom**: SuperheroFlightAnimations bootstrap_module ile C++ projesine
yükseltildikten sonra editor "could not compile plugin SageBridge" hatası
verdi ve açılmadı. BP-only Kale'de aynı plugin (sadece dylib + uplugin) iyi
çalışıyordu.

**Root**: UE'nin "shipped plugin = sadece Binaries yeter" optimizasyonu
yalnız BP-only projeler için çalışır. Proje C++ olunca (`.uproject` Modules
array'i dolduğunda) UBT proje target'ı oluştururken bağlı plugin'leri de
target ağacına alır ve onları **source'tan** rebuild eder. Plugin
`.uplugin`'inde `Installed: true` olsa bile bu kural değişmez — Installed
flag yalnız "Marketplace store" senaryosunu işaretler, UBT host-target
build'ini etkilemez.

**Rule**: Per-project plugin install'ında her projeye **3 şey kopyalanmalı**:
1. `<Project>/Plugins/SageBridge/SageBridge.uplugin`
2. `<Project>/Plugins/SageBridge/Binaries/<Platform>/UnrealEditor-SageBridge.dylib` + `UnrealEditor.modules` (BP-only proje için yeter)
3. `<Project>/Plugins/SageBridge/Source/` (C++ proje için + sürdürülebilir editor build için)

Source size ~1MB, ihmal edilebilir. BP-only projede Source/ olması zarar
vermez (UE precompiled binary'i hâlâ görür ve kullanır), C++ projede
mecburi. **Default: hep ikisini birden kopyala.**

**Apply**: `sage init` (npm-pkg phase) bu üçünü birden kopyalar; Mac/Linux
için `cp -R Source` + `cp Binaries/Mac/*`. Dev döngüsünde dylib swap
yaparken Source mismatch'i olmasın diye Source da güncel tutulmalı (rev
mismatch UE compile fail eder).

**Engine plugin alternatifi**: `<UE>/Engine/Plugins/Marketplace/SageBridge/`
altına engine-level kurulum hâlinde Source dahil her şey bir kez kurulur,
tüm projeler paylaşır. Ama bu engine version başına ayrı kurulum gerektirir
(5.4/5.5/5.6/5.7) — `sage init` per-project install'ı default tuttuğumuz
için (ADR-016) Engine kurulumu opsiyonel "advanced install" path'i olarak
ileride eklenir.
