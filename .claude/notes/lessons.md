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
