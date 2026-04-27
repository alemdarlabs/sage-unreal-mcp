# Lessons

> Per CLAUDE.md §Self-Improvement Loop. Patterns observed during implementation
> that should change future behavior.

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

## vcpkg first-time install — surprisingly fast on this machine

`ixwebsocket[core,sectransp,ssl]` + zlib + dependencies took 8 seconds via
binary cache; `nlohmann-json + spdlog + cpp-httplib + catch2` initial install
30 seconds. Don't pre-pessimize cmake configure timing on a developer box with
warm vcpkg cache.
