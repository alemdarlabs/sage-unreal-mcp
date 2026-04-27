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

## vcpkg first-time install — surprisingly fast on this machine

`ixwebsocket[core,sectransp,ssl]` + zlib + dependencies took 8 seconds via
binary cache; `nlohmann-json + spdlog + cpp-httplib + catch2` initial install
30 seconds. Don't pre-pessimize cmake configure timing on a developer box with
warm vcpkg cache.
