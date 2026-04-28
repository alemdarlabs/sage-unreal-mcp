# Lessons

> Per CLAUDE.md §Self-Improvement Loop. Patterns observed during implementation
> that should change future behavior.

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
