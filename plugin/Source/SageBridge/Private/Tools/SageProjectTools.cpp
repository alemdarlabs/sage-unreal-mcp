#include "Tools/SageProjectTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "SageProject"

namespace sage::tools
{
namespace
{

// Resolve a user-supplied file path to an absolute path on disk:
//   - "/Game/..." or "/Engine/..." treated as content-mounted (rejected
//     here — those go through asset paths, not source paths)
//   - relative path: joined to ProjectDir
//   - absolute path: used as-is (only allowed under ProjectDir or
//     EngineDir to avoid arbitrary file reads)
FString ResolveSafeSourcePath(const FString& InPath, FString& OutError)
{
    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    const FString EngineDir  = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());

    FString Abs = InPath;
    if (FPaths::IsRelative(Abs))
    {
        Abs = FPaths::Combine(ProjectDir, Abs);
    }
    Abs = FPaths::ConvertRelativePathToFull(Abs);

    if (!Abs.StartsWith(ProjectDir) && !Abs.StartsWith(EngineDir))
    {
        OutError = FString::Printf(
            TEXT("path is outside project + engine root (rejected for safety): %s"),
            *Abs);
        return FString();
    }
    return Abs;
}

FSageToolDispatch::FOutcome ProjectGetInfoImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("project_name"),    FApp::GetProjectName());
    R->SetStringField(TEXT("project_dir"),     FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
    R->SetStringField(TEXT("engine_dir"),      FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));
    R->SetStringField(TEXT("project_log_dir"), FPaths::ProjectLogDir());

    const FString UProjectPath = FPaths::Combine(
        FPaths::ProjectDir(),
        FApp::GetProjectName() + FString(TEXT(".uproject")));

    FString UProjectStr;
    if (FFileHelper::LoadFileToString(UProjectStr, *UProjectPath))
    {
        TSharedPtr<FJsonObject> UProj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UProjectStr);
        if (FJsonSerializer::Deserialize(Reader, UProj) && UProj.IsValid())
        {
            FString EngineAssoc;
            if (UProj->TryGetStringField(TEXT("EngineAssociation"), EngineAssoc))
            {
                R->SetStringField(TEXT("engine_association"), EngineAssoc);
            }
            FString Description;
            if (UProj->TryGetStringField(TEXT("Description"), Description))
            {
                R->SetStringField(TEXT("description"), Description);
            }
            FString Category;
            if (UProj->TryGetStringField(TEXT("Category"), Category))
            {
                R->SetStringField(TEXT("category"), Category);
            }

            // Modules listed in .uproject (these have a Loadingphase, vs
            // bare Source/<X> directories which are still legal modules).
            const TArray<TSharedPtr<FJsonValue>>* ModulesArr = nullptr;
            if (UProj->TryGetArrayField(TEXT("Modules"), ModulesArr))
            {
                R->SetArrayField(TEXT("declared_modules"), *ModulesArr);
            }

            // Plugins, if any (with enabled flag).
            const TArray<TSharedPtr<FJsonValue>>* PluginsArr = nullptr;
            if (UProj->TryGetArrayField(TEXT("Plugins"), PluginsArr))
            {
                R->SetArrayField(TEXT("plugins"), *PluginsArr);
            }
        }
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ProjectListModulesImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    IFileManager& FileMgr = IFileManager::Get();

    auto AddModule = [&](const FString& Name, const FString& ModuleDir,
                         const FString& BuildCsPath, const FString& Plugin,
                         TArray<TSharedPtr<FJsonValue>>& Out)
    {
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"),          Name);
        O->SetStringField(TEXT("module_dir"),    FPaths::ConvertRelativePathToFull(ModuleDir));
        O->SetStringField(TEXT("build_cs_path"), FPaths::ConvertRelativePathToFull(BuildCsPath));
        if (!Plugin.IsEmpty()) O->SetStringField(TEXT("plugin"), Plugin);

        TArray<FString> Headers, Sources;
        FileMgr.FindFilesRecursive(Headers, *ModuleDir, TEXT("*.h"),   true, false);
        FileMgr.FindFilesRecursive(Sources, *ModuleDir, TEXT("*.cpp"), true, false);
        O->SetNumberField(TEXT("header_count"), Headers.Num());
        O->SetNumberField(TEXT("source_count"), Sources.Num());
        Out.Add(MakeShared<FJsonValueObject>(O));
    };

    TArray<TSharedPtr<FJsonValue>> Out;

    // 1. Project-side modules: Source/<Module>/<Module>.Build.cs
    const FString SourceDir = FPaths::ProjectDir() / TEXT("Source");
    {
        TArray<FString> SubDirs;
        FileMgr.FindFiles(SubDirs, *(SourceDir / TEXT("*")), false, true);
        for (const FString& Dir : SubDirs)
        {
            const FString ModuleDir   = SourceDir / Dir;
            const FString BuildCsPath = ModuleDir / (Dir + TEXT(".Build.cs"));
            if (!FileMgr.FileExists(*BuildCsPath)) continue;
            AddModule(Dir, ModuleDir, BuildCsPath, /*plugin*/ FString{}, Out);
        }
    }

    // 2. Project-local plugin modules: Plugins/<Plugin>/Source/<Module>/<Module>.Build.cs
    const FString PluginsDir = FPaths::ProjectPluginsDir();
    if (FileMgr.DirectoryExists(*PluginsDir))
    {
        TArray<FString> PluginDirs;
        FileMgr.FindFiles(PluginDirs, *(PluginsDir / TEXT("*")), false, true);
        for (const FString& Plugin : PluginDirs)
        {
            const FString PluginSrc = PluginsDir / Plugin / TEXT("Source");
            if (!FileMgr.DirectoryExists(*PluginSrc)) continue;

            TArray<FString> ModuleDirs;
            FileMgr.FindFiles(ModuleDirs, *(PluginSrc / TEXT("*")), false, true);
            for (const FString& Dir : ModuleDirs)
            {
                const FString ModuleDir   = PluginSrc / Dir;
                const FString BuildCsPath = ModuleDir / (Dir + TEXT(".Build.cs"));
                if (!FileMgr.FileExists(*BuildCsPath)) continue;
                AddModule(Dir, ModuleDir, BuildCsPath, Plugin, Out);
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("project_name"), FApp::GetProjectName());
    R->SetStringField(TEXT("source_dir"),   FPaths::ConvertRelativePathToFull(SourceDir));
    R->SetStringField(TEXT("plugins_dir"),  FPaths::ConvertRelativePathToFull(PluginsDir));
    R->SetArrayField (TEXT("modules"),      Out);
    R->SetNumberField(TEXT("count"),        Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ProjectReadCppHeaderImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString InPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), InPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    FString Err;
    const FString Abs = ResolveSafeSourcePath(InPath, Err);
    if (Abs.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *Abs))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("could not read header: %s"), *Abs));
    }

    TArray<FString> RawLines;
    Content.ParseIntoArrayLines(RawLines, /*bCullEmpty*/ false);

    // Strip C/C++ comments before macro scanning so we don't treat
    // `// UCLASS(...)`, `/* UCLASS */`, or doxygen mentions as real
    // declarations. We preserve line indices (one entry per source line)
    // so the reported line numbers still match the original file.
    TArray<FString> Lines;
    Lines.Reserve(RawLines.Num());
    {
        bool bInBlockComment = false;
        for (const FString& Raw : RawLines)
        {
            FString S; S.Reserve(Raw.Len());
            for (int32 i = 0; i < Raw.Len(); ++i)
            {
                if (bInBlockComment)
                {
                    if (i + 1 < Raw.Len() && Raw[i] == TEXT('*') && Raw[i + 1] == TEXT('/'))
                    {
                        bInBlockComment = false;
                        ++i;
                    }
                    continue;
                }
                // // line comment terminates the rest of this line.
                if (i + 1 < Raw.Len() && Raw[i] == TEXT('/') && Raw[i + 1] == TEXT('/')) break;
                if (i + 1 < Raw.Len() && Raw[i] == TEXT('/') && Raw[i + 1] == TEXT('*'))
                {
                    bInBlockComment = true;
                    ++i;
                    continue;
                }
                S.AppendChar(Raw[i]);
            }
            Lines.Add(MoveTemp(S));
        }
    }

    // Lightweight regex-y scan. Captures the *next* identifier after the
    // macro keyword. Multi-line UCLASS(...) is handled by reading until
    // the next `class`/`struct`/`enum` keyword on a subsequent line.
    auto Capture = [](const TArray<FString>& InLines, const TCHAR* Macro,
                      const TCHAR* Keyword) -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (int32 i = 0; i < InLines.Num(); ++i)
        {
            const FString& L = InLines[i];
            if (!L.Contains(Macro)) continue;
            // Require macro to appear in a declarative position — i.e. the
            // line, once leading whitespace is stripped, starts with the
            // macro identifier and `(`. Filters out incidental mentions
            // (string literals, identifiers like UCLASS_MARKER, etc.).
            FString T = L; T.TrimStartAndEndInline();
            const FString MacroParen = FString(Macro) + TEXT("(");
            if (!T.StartsWith(MacroParen) && !T.Equals(Macro)) continue;
            // Look forward up to 5 lines for the keyword line
            for (int32 j = i; j < FMath::Min(InLines.Num(), i + 5); ++j)
            {
                const FString& K = InLines[j];
                int32 KwIdx = K.Find(Keyword);
                if (KwIdx == INDEX_NONE) continue;
                int32 NameStart = KwIdx + FCString::Strlen(Keyword);
                while (NameStart < K.Len() && FChar::IsWhitespace(K[NameStart])) ++NameStart;
                // Skip optional API export macro tokens like UNREALED_API
                while (NameStart < K.Len() && (FChar::IsAlnum(K[NameStart]) || K[NameStart] == TEXT('_')))
                {
                    // Heuristic: if the identifier ends in _API, treat as the
                    // export macro and skip past it.
                    int32 IdEnd = NameStart;
                    while (IdEnd < K.Len() && (FChar::IsAlnum(K[IdEnd]) || K[IdEnd] == TEXT('_'))) ++IdEnd;
                    const FString Tok = K.Mid(NameStart, IdEnd - NameStart);
                    if (Tok.EndsWith(TEXT("_API")))
                    {
                        NameStart = IdEnd;
                        while (NameStart < K.Len() && FChar::IsWhitespace(K[NameStart])) ++NameStart;
                        continue;
                    }
                    auto O = MakeShared<FJsonObject>();
                    O->SetStringField(TEXT("name"), Tok);
                    O->SetNumberField(TEXT("line"), j + 1);
                    Out.Add(MakeShared<FJsonValueObject>(O));
                    break;
                }
                break;
            }
        }
        return Out;
    };

    TArray<TSharedPtr<FJsonValue>> Classes  = Capture(Lines, TEXT("UCLASS"),  TEXT("class"));
    TArray<TSharedPtr<FJsonValue>> Structs  = Capture(Lines, TEXT("USTRUCT"), TEXT("struct"));
    TArray<TSharedPtr<FJsonValue>> Enums    = Capture(Lines, TEXT("UENUM"),   TEXT("enum"));
    TArray<TSharedPtr<FJsonValue>> Includes;
    for (const FString& L : Lines)
    {
        if (!L.StartsWith(TEXT("#include"))) continue;
        Includes.Add(MakeShared<FJsonValueString>(L.TrimStartAndEnd()));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Abs);
    R->SetArrayField (TEXT("classes"),    Classes);
    R->SetArrayField (TEXT("structs"),    Structs);
    R->SetArrayField (TEXT("enums"),      Enums);
    R->SetArrayField (TEXT("includes"),   Includes);
    R->SetNumberField(TEXT("line_count"), Lines.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// Recursive case-sensitive substring search across .h/.cpp/.inl files
// in a directory subtree. Returns up to max_results hits with surrounding
// context. Used by project.search_cpp + project.find_engine_symbol.
struct FSearchHit
{
    FString File;
    int32   Line;
    FString Snippet;
};

void SearchInDir(const FString& Root, const FString& Query,
                 const TArray<FString>& Extensions,
                 int32 MaxResults, int32 ContextLen,
                 TArray<FSearchHit>& Out)
{
    IFileManager& FileMgr = IFileManager::Get();
    TArray<FString> Files;
    for (const FString& Ext : Extensions)
    {
        TArray<FString> Found;
        FileMgr.FindFilesRecursive(Found, *Root, *(FString(TEXT("*")) + Ext), true, false);
        Files.Append(MoveTemp(Found));
    }

    // Per-file cap to avoid OOM on huge codebases (e.g. Lyra at 1M+ LOC has
    // a few generated headers in the multi-MB range). 1MB is well above any
    // hand-written .h/.cpp; oversized files get skipped + reported.
    constexpr int64 kPerFileMaxBytes = 1 * 1024 * 1024;

    for (const FString& F : Files)
    {
        if (Out.Num() >= MaxResults) return;

        const int64 FileSize = FileMgr.FileSize(*F);
        if (FileSize > kPerFileMaxBytes) continue;  // skip oversized

        FString Content;
        if (!FFileHelper::LoadFileToString(Content, *F)) continue;
        if (!Content.Contains(Query)) continue;

        TArray<FString> Lines;
        Content.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
        for (int32 i = 0; i < Lines.Num(); ++i)
        {
            if (Out.Num() >= MaxResults) return;
            if (!Lines[i].Contains(Query)) continue;
            FSearchHit H;
            H.File    = F;
            H.Line    = i + 1;
            H.Snippet = Lines[i].TrimStartAndEnd().Left(ContextLen);
            Out.Add(MoveTemp(H));
        }
    }
}

TArray<TSharedPtr<FJsonValue>> SearchHitsToJson(const TArray<FSearchHit>& Hits)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FSearchHit& H : Hits)
    {
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("file"),    H.File);
        O->SetNumberField(TEXT("line"),    H.Line);
        O->SetStringField(TEXT("snippet"), H.Snippet);
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    return Arr;
}

FSageToolDispatch::FOutcome ProjectSearchCppImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Query;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/empty 'query'"));
    }
    int32 MaxResults = 50;
    bool  bIncludePlugins = true;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 500);
        }
        Args->TryGetBoolField(TEXT("include_plugins"), bIncludePlugins);
    }

    const FString SourceDir  = FPaths::ProjectDir() / TEXT("Source");
    const FString PluginsDir = FPaths::ProjectPluginsDir();

    TArray<FSearchHit> Hits;
    const TArray<FString> Exts{TEXT(".h"), TEXT(".cpp"), TEXT(".inl")};
    SearchInDir(SourceDir, Query, Exts, MaxResults, 200, Hits);

    // Project-local plugins (Plugins/<X>/Source/) — opt out via include_plugins=false.
    TArray<FString> PluginRoots;
    if (bIncludePlugins && IFileManager::Get().DirectoryExists(*PluginsDir))
    {
        TArray<FString> PluginDirs;
        IFileManager::Get().FindFiles(PluginDirs, *(PluginsDir / TEXT("*")), false, true);
        for (const FString& Plugin : PluginDirs)
        {
            const FString PluginSrc = PluginsDir / Plugin / TEXT("Source");
            if (!IFileManager::Get().DirectoryExists(*PluginSrc)) continue;
            if (Hits.Num() >= MaxResults) break;
            SearchInDir(PluginSrc, Query, Exts, MaxResults, 200, Hits);
            PluginRoots.Add(FPaths::ConvertRelativePathToFull(PluginSrc));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"),    Query);
    R->SetStringField(TEXT("root"),     FPaths::ConvertRelativePathToFull(SourceDir));
    if (PluginRoots.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Roots;
        for (const FString& P : PluginRoots) Roots.Add(MakeShared<FJsonValueString>(P));
        R->SetArrayField(TEXT("plugin_roots"), Roots);
    }
    R->SetArrayField (TEXT("hits"),     SearchHitsToJson(Hits));
    R->SetNumberField(TEXT("count"),    Hits.Num());
    R->SetBoolField  (TEXT("capped"),   Hits.Num() >= MaxResults);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ProjectListEngineModulesImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    const FString EngineSourceDir = FPaths::EngineDir() / TEXT("Source");
    IFileManager& FileMgr = IFileManager::Get();

    TArray<TSharedPtr<FJsonValue>> Out;
    // Engine/Source has Runtime/, Editor/, Developer/, Programs/, ThirdParty/.
    // Each has <Module>/<Module>.Build.cs underneath.
    const TArray<FString> Categories = {
        TEXT("Runtime"), TEXT("Editor"), TEXT("Developer"), TEXT("ThirdParty")
    };
    for (const FString& Cat : Categories)
    {
        const FString CatDir = EngineSourceDir / Cat;
        if (!FileMgr.DirectoryExists(*CatDir)) continue;

        TArray<FString> SubDirs;
        FileMgr.FindFiles(SubDirs, *(CatDir / TEXT("*")), false, true);
        for (const FString& Dir : SubDirs)
        {
            const FString ModuleDir   = CatDir / Dir;
            const FString BuildCsPath = ModuleDir / (Dir + TEXT(".Build.cs"));
            if (!FileMgr.FileExists(*BuildCsPath)) continue;

            auto O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"),       Dir);
            O->SetStringField(TEXT("category"),   Cat);
            O->SetStringField(TEXT("module_dir"), FPaths::ConvertRelativePathToFull(ModuleDir));
            Out.Add(MakeShared<FJsonValueObject>(O));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("engine_source_dir"),
        FPaths::ConvertRelativePathToFull(EngineSourceDir));
    R->SetArrayField (TEXT("modules"), Out);
    R->SetNumberField(TEXT("count"),   Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ProjectReadEngineHeaderImpl(const TSharedPtr<FJsonObject>& Args)
{
    // Just delegates to ReadCppHeader after asserting the path is under
    // EngineDir. The safety guard in ResolveSafeSourcePath already permits
    // EngineDir paths, so the heavy lifting is shared. Kept as a separate
    // tool for naming clarity in the agent's mental model.
    return ProjectReadCppHeaderImpl(Args);
}

FSageToolDispatch::FOutcome ProjectFindEngineSymbolImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Symbol;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("symbol"), Symbol) || Symbol.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/empty 'symbol'"));
    }
    int32 MaxResults = 50;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 500);
        }
    }
    FString Category;
    Args->TryGetStringField(TEXT("category"), Category);
    if (Category.IsEmpty()) Category = TEXT("Runtime");
    if (Category != TEXT("Runtime") && Category != TEXT("Editor")
        && Category != TEXT("Developer") && Category != TEXT("ThirdParty"))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("category must be Runtime/Editor/Developer/ThirdParty, got %s"),
                            *Category));
    }

    const FString SearchRoot = FPaths::EngineDir() / TEXT("Source") / Category;

    TArray<FSearchHit> Hits;
    SearchInDir(SearchRoot, Symbol,
        {TEXT(".h"), TEXT(".cpp")},
        MaxResults, 200, Hits);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("symbol"),    Symbol);
    R->SetStringField(TEXT("category"),  Category);
    R->SetStringField(TEXT("root"),      FPaths::ConvertRelativePathToFull(SearchRoot));
    R->SetArrayField (TEXT("hits"),      SearchHitsToJson(Hits));
    R->SetNumberField(TEXT("count"),     Hits.Num());
    R->SetBoolField  (TEXT("capped"),    Hits.Num() >= MaxResults);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// Phase 4.7-p3: INI config introspection.

// Resolve config name -> full path. Accepts:
//   "Game"             -> ProjectDir/Config/DefaultGame.ini
//   "DefaultGame"      -> ProjectDir/Config/DefaultGame.ini
//   "DefaultGame.ini"  -> ProjectDir/Config/DefaultGame.ini
//   "Engine"           -> ProjectDir/Config/DefaultEngine.ini
// Returns empty FString if the resolved file doesn't exist.
FString ResolveConfigPath(const FString& Name)
{
    const FString Dir = FPaths::ProjectDir() / TEXT("Config");
    FString Base = Name;
    if (Base.EndsWith(TEXT(".ini"))) Base.LeftChopInline(4);
    if (!Base.StartsWith(TEXT("Default"))) Base = FString(TEXT("Default")) + Base;
    const FString Full = Dir / (Base + TEXT(".ini"));
    if (!IFileManager::Get().FileExists(*Full)) return FString();
    return FPaths::ConvertRelativePathToFull(Full);
}

// Parse INI text into [{section, entries: [{key, value}]}]. UE INI is
// loose: handles +Key=Val (array append) and -Key=Val (array remove) by
// collapsing both to the bare key with a leading-char prefix in value
// metadata. Bracket-section header on its own line; comments start with
// `;` or `//`. We don't expand env vars or process .Build.cs-style
// includes — that's out of scope.
TSharedRef<FJsonObject> ParseIniContent(const FString& Content)
{
    auto Root = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Sections;

    TArray<FString> Lines;
    Content.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

    TSharedPtr<FJsonObject> CurrentSection;
    TArray<TSharedPtr<FJsonValue>> CurrentEntries;
    auto FlushSection = [&]() {
        if (CurrentSection.IsValid())
        {
            CurrentSection->SetArrayField(TEXT("entries"), CurrentEntries);
            Sections.Add(MakeShared<FJsonValueObject>(CurrentSection));
        }
        CurrentSection.Reset();
        CurrentEntries.Empty();
    };

    for (const FString& Raw : Lines)
    {
        FString L = Raw;
        L.TrimStartAndEndInline();
        if (L.IsEmpty()) continue;
        if (L.StartsWith(TEXT(";")) || L.StartsWith(TEXT("//"))) continue;

        if (L.StartsWith(TEXT("[")) && L.EndsWith(TEXT("]")))
        {
            FlushSection();
            CurrentSection = MakeShared<FJsonObject>();
            CurrentSection->SetStringField(TEXT("name"), L.Mid(1, L.Len() - 2));
            continue;
        }

        // Key=Value line. Allow leading +/- modifiers.
        FString Mod;
        if (!L.IsEmpty() && (L[0] == TEXT('+') || L[0] == TEXT('-') || L[0] == TEXT('!') || L[0] == TEXT('.')))
        {
            Mod = FString(1, &L[0]);
            L.RemoveAt(0);
        }
        int32 EqIdx;
        if (!L.FindChar(TEXT('='), EqIdx)) continue;
        const FString Key = L.Left(EqIdx).TrimStartAndEnd();
        const FString Val = L.Mid(EqIdx + 1);  // value preserved verbatim (may contain quotes / commas)

        auto E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("key"),   Key);
        E->SetStringField(TEXT("value"), Val);
        if (!Mod.IsEmpty()) E->SetStringField(TEXT("modifier"), Mod);
        CurrentEntries.Add(MakeShared<FJsonValueObject>(E));
    }
    FlushSection();

    Root->SetArrayField(TEXT("sections"), Sections);
    Root->SetNumberField(TEXT("section_count"), Sections.Num());
    return Root;
}

FSageToolDispatch::FOutcome ProjectReadConfigImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    const FString Path = ResolveConfigPath(Name);
    if (Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("config not found: %s (resolved miss)"), *Name));
    }

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("could not read: %s"), *Path));
    }

    bool bRaw = false;
    Args->TryGetBoolField(TEXT("raw"), bRaw);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("name"), Name);
    R->SetStringField(TEXT("path"), Path);
    R->SetNumberField(TEXT("size_bytes"), Content.Len());
    if (bRaw)
    {
        R->SetStringField(TEXT("content"), Content);
    }
    else
    {
        const TSharedRef<FJsonObject> Parsed = ParseIniContent(Content);
        R->SetArrayField (TEXT("sections"),       Parsed->GetArrayField(TEXT("sections")));
        R->SetNumberField(TEXT("section_count"),  Parsed->GetNumberField(TEXT("section_count")));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ProjectSearchConfigImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Query;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/empty 'query'"));
    }
    int32 MaxResults = 50;
    double Num = 0;
    if (Args->TryGetNumberField(TEXT("max_results"), Num))
    {
        MaxResults = FMath::Clamp(static_cast<int32>(Num), 1, 500);
    }

    const FString ConfigDir = FPaths::ProjectDir() / TEXT("Config");

    TArray<FSearchHit> Hits;
    SearchInDir(ConfigDir, Query, {TEXT(".ini")}, MaxResults, 200, Hits);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"),    Query);
    R->SetStringField(TEXT("root"),     FPaths::ConvertRelativePathToFull(ConfigDir));
    R->SetArrayField (TEXT("hits"),     SearchHitsToJson(Hits));
    R->SetNumberField(TEXT("count"),    Hits.Num());
    R->SetBoolField  (TEXT("capped"),   Hits.Num() >= MaxResults);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ProjectListConfigTagsImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    // Read DefaultGameplayTags.ini. Lines look like:
    //   +GameplayTagList=(Tag="Foo.Bar", DevComment="optional")
    // Some projects also use additional INI files under Config/Tags/ —
    // we scan all *.ini in Config/ for safety and grab any GameplayTag
    // / GameplayTagList line.
    const FString ConfigDir = FPaths::ProjectDir() / TEXT("Config");
    IFileManager& FileMgr   = IFileManager::Get();

    TArray<FString> IniFiles;
    FileMgr.FindFilesRecursive(IniFiles, *ConfigDir, TEXT("*.ini"), true, false);

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FString& IniPath : IniFiles)
    {
        FString Content;
        if (!FFileHelper::LoadFileToString(Content, *IniPath)) continue;
        if (!Content.Contains(TEXT("GameplayTag"))) continue;

        TArray<FString> Lines;
        Content.ParseIntoArrayLines(Lines, false);
        for (int32 i = 0; i < Lines.Num(); ++i)
        {
            const FString& L = Lines[i];
            const int32 TagIdx = L.Find(TEXT("Tag=\""));
            if (TagIdx == INDEX_NONE) continue;
            const int32 NameStart = TagIdx + 5;
            int32 EndQuote = L.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, NameStart);
            if (EndQuote == INDEX_NONE) continue;
            const FString Tag = L.Mid(NameStart, EndQuote - NameStart);
            if (Tag.IsEmpty()) continue;

            FString Comment;
            const int32 ComIdx = L.Find(TEXT("DevComment=\""));
            if (ComIdx != INDEX_NONE)
            {
                const int32 CStart = ComIdx + 12;
                const int32 CEnd = L.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, CStart);
                if (CEnd != INDEX_NONE) Comment = L.Mid(CStart, CEnd - CStart);
            }

            auto O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("tag"),     Tag);
            if (!Comment.IsEmpty()) O->SetStringField(TEXT("dev_comment"), Comment);
            O->SetStringField(TEXT("source"),  FPaths::ConvertRelativePathToFull(IniPath));
            O->SetNumberField(TEXT("line"),    i + 1);
            Out.Add(MakeShared<FJsonValueObject>(O));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("config_dir"), FPaths::ConvertRelativePathToFull(ConfigDir));
    R->SetArrayField (TEXT("tags"),       Out);
    R->SetNumberField(TEXT("count"),      Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// Phase 4.7-p4: INI write + plugin enable.

// Atomic file write: write to <path>.tmp, COPY original to .sage_bak (so the
// original stays at Path during the swap; if the editor crashes between the
// backup and the rename, Path is still intact), then rename temp → final and
// delete the backup on success. On failure, the temp file is removed.
bool AtomicWriteString(const FString& Path, const FString& Content, FString& OutError)
{
    const FString TempPath   = Path + TEXT(".sage_tmp");
    const FString BackupPath = Path + TEXT(".sage_bak");

    if (!FFileHelper::SaveStringToFile(Content, *TempPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = FString::Printf(TEXT("could not write temp: %s"), *TempPath);
        return false;
    }

    IFileManager& FM = IFileManager::Get();
    const bool bHadOriginal = FM.FileExists(*Path);
    if (bHadOriginal)
    {
        // Backup as COPY (NOT move). Original must stay at Path for the
        // entire swap window so a crash between here and the rename below
        // leaves the file intact at its expected location.
        FM.Delete(*BackupPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
        // Copy returns COPY_OK (0) on success, COPY_Fail (non-zero) on error.
        const uint32 CopyResult = FM.Copy(*BackupPath, *Path,
            /*bReplace*/ true, /*bEvenIfReadOnly*/ true, /*bAttributes*/ false);
        if (CopyResult != 0u)
        {
            // Backup failed; abort to avoid losing the original on rename.
            FM.Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
            OutError = FString::Printf(TEXT("could not create backup: %s"), *BackupPath);
            return false;
        }
    }

    // Atomic rename temp → final (replaces the still-present original).
    if (!FM.Move(*Path, *TempPath, /*bReplace*/ true, /*bEvenIfReadOnly*/ true,
                 /*bAttributes*/ false, /*bDoNotRetryOrError*/ true))
    {
        // Rename failed — temp leftover, original still at Path, backup
        // (if any) untouched. Best-effort cleanup of temp; keep backup so
        // the caller can recover manually if needed.
        FM.Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
        OutError = FString::Printf(TEXT("rename failed: %s -> %s"), *TempPath, *Path);
        return false;
    }

    // Success: drop the backup.
    if (bHadOriginal)
    {
        FM.Delete(*BackupPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
    }
    return true;
}

// Detect line ending convention used in an existing file body. Returns
// "\r\n" if any CRLF is present, "\n" otherwise. We bias toward CRLF on
// Windows when the file is empty/unknown so freshly-created INIs match
// UE's writer convention there.
static FString DetectLineEnding(const FString& Content)
{
    if (Content.Contains(TEXT("\r\n"))) return FString(TEXT("\r\n"));
    if (Content.Contains(TEXT("\n")))   return FString(TEXT("\n"));
#if PLATFORM_WINDOWS
    return FString(TEXT("\r\n"));
#else
    return FString(TEXT("\n"));
#endif
}

// Modify or insert a key=value pair under [section] in INI text. Preserves
// other lines verbatim AND original line-ending convention (CRLF vs LF) so
// VCS diffs stay clean on Windows where INI files are typically CRLF.
// Modifier prefix (+/-/!/.) becomes part of the emitted line if supplied.
// Returns true if anything changed.
bool UpsertIniKey(FString& Content, const FString& Section, const FString& Key,
                  const FString& Value, const FString& Modifier)
{
    const FString EOL = DetectLineEnding(Content);

    // ParseIntoArrayLines strips both \r\n and \n, so it's EOL-agnostic.
    TArray<FString> Lines;
    Content.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

    const FString SectionHeader = FString::Printf(TEXT("[%s]"), *Section);
    int32 SectionStart = INDEX_NONE;
    int32 SectionEnd   = INDEX_NONE;
    for (int32 i = 0; i < Lines.Num(); ++i)
    {
        const FString T = Lines[i].TrimStartAndEnd();
        if (T == SectionHeader)
        {
            SectionStart = i;
        }
        else if (SectionStart != INDEX_NONE && T.StartsWith(TEXT("[")) && T.EndsWith(TEXT("]")))
        {
            SectionEnd = i - 1;
            break;
        }
    }
    if (SectionStart != INDEX_NONE && SectionEnd == INDEX_NONE)
    {
        SectionEnd = Lines.Num() - 1;
    }

    const FString Emit = Modifier + Key + TEXT("=") + Value;

    // Section exists: replace first matching key (modifier-agnostic) or
    // append at end of section.
    if (SectionStart != INDEX_NONE)
    {
        for (int32 i = SectionStart + 1; i <= SectionEnd; ++i)
        {
            FString T = Lines[i];
            T.TrimStartAndEndInline();
            if (T.IsEmpty() || T.StartsWith(TEXT(";")) || T.StartsWith(TEXT("//"))) continue;
            // strip leading modifier
            int32 KeyStart = 0;
            while (KeyStart < T.Len() &&
                   (T[KeyStart] == TEXT('+') || T[KeyStart] == TEXT('-')
                 || T[KeyStart] == TEXT('!') || T[KeyStart] == TEXT('.')))
            {
                ++KeyStart;
            }
            int32 EqIdx;
            if (!T.FindChar(TEXT('='), EqIdx)) continue;
            const FString ExistingKey = T.Mid(KeyStart, EqIdx - KeyStart).TrimStartAndEnd();
            if (ExistingKey == Key)
            {
                Lines[i] = Emit;
                Content = FString::Join(Lines, *EOL);
                if (!Content.EndsWith(EOL)) Content += EOL;
                return true;
            }
        }
        // Insert at end of section.
        Lines.Insert(Emit, SectionEnd + 1);
        Content = FString::Join(Lines, *EOL);
        if (!Content.EndsWith(EOL)) Content += EOL;
        return true;
    }

    // Section absent: append a fresh section block.
    if (!Content.IsEmpty() && !Content.EndsWith(EOL)) Content += EOL;
    if (!Content.IsEmpty()) Content += EOL;
    Content += SectionHeader + EOL;
    Content += Emit + EOL;
    return true;
}

FSageToolDispatch::FOutcome ProjectSetConfigImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Name, Section, Key, Value, Modifier;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("name"),    Name)
        || !Args->TryGetStringField(TEXT("section"), Section)
        || !Args->TryGetStringField(TEXT("key"),     Key)
        || !Args->TryGetStringField(TEXT("value"),   Value))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'name', 'section', 'key', or 'value'"));
    }
    Args->TryGetStringField(TEXT("modifier"), Modifier);
    if (!Modifier.IsEmpty()
        && Modifier != TEXT("+") && Modifier != TEXT("-")
        && Modifier != TEXT("!") && Modifier != TEXT("."))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("modifier must be one of '', '+', '-', '!', '.'"));
    }

    const FString Path = ResolveConfigPath(Name);
    FString Resolved = Path;
    bool bCreated = false;
    if (Resolved.IsEmpty())
    {
        // Fall back to the canonical location even when the file doesn't
        // exist yet — set_config can also bootstrap a config.
        FString Base = Name;
        if (Base.EndsWith(TEXT(".ini"))) Base.LeftChopInline(4);
        if (!Base.StartsWith(TEXT("Default"))) Base = FString(TEXT("Default")) + Base;
        Resolved = FPaths::ConvertRelativePathToFull(
            FPaths::ProjectDir() / TEXT("Config") / (Base + TEXT(".ini")));
        bCreated = true;
    }

    FString Content;
    if (!bCreated && !FFileHelper::LoadFileToString(Content, *Resolved))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("could not read existing INI: %s"), *Resolved));
    }

    UpsertIniKey(Content, Section, Key, Value, Modifier);

    FString Err;
    if (!AtomicWriteString(Resolved, Content, Err))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, Err);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),     Resolved);
    R->SetStringField(TEXT("section"),  Section);
    R->SetStringField(TEXT("key"),      Key);
    R->SetStringField(TEXT("value"),    Value);
    R->SetBoolField  (TEXT("created"),  bCreated);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ProjectSetPluginEnabledImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString PluginName;
    bool    bEnabled = false;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("plugin"), PluginName)
        || !Args->TryGetBoolField  (TEXT("enabled"), bEnabled))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'plugin' or 'enabled'"));
    }

    // Modifying the .uproject Plugins[] array during a running editor session
    // can have non-trivial side effects (modules unloaded, asset references
    // dangling, editor state inconsistent until restart). Require explicit
    // confirmation so an agent cannot toggle plugins by accident.
    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("modifying .uproject Plugins[] requires confirmed=true "
                                 "(plugin='%s', enabled=%s); editor restart required after."),
                            *PluginName, bEnabled ? TEXT("true") : TEXT("false")));
    }

    const FString UProjectPath = FPaths::Combine(
        FPaths::ProjectDir(),
        FApp::GetProjectName() + FString(TEXT(".uproject")));

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *UProjectPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("could not read .uproject: %s"), *UProjectPath));
    }

    TSharedPtr<FJsonObject> UProj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
    if (!FJsonSerializer::Deserialize(Reader, UProj) || !UProj.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("malformed .uproject JSON"));
    }

    TArray<TSharedPtr<FJsonValue>> Plugins;
    const TArray<TSharedPtr<FJsonValue>>* PluginsPtr = nullptr;
    if (UProj->TryGetArrayField(TEXT("Plugins"), PluginsPtr))
    {
        Plugins = *PluginsPtr;
    }

    bool bUpdated = false;
    for (TSharedPtr<FJsonValue>& V : Plugins)
    {
        const TSharedPtr<FJsonObject>* O = nullptr;
        if (!V->TryGetObject(O) || !O->IsValid()) continue;
        FString N;
        if (!(*O)->TryGetStringField(TEXT("Name"), N) || N != PluginName) continue;
        (*O)->SetBoolField(TEXT("Enabled"), bEnabled);
        bUpdated = true;
        break;
    }
    if (!bUpdated)
    {
        auto NewEntry = MakeShared<FJsonObject>();
        NewEntry->SetStringField(TEXT("Name"),    PluginName);
        NewEntry->SetBoolField  (TEXT("Enabled"), bEnabled);
        Plugins.Add(MakeShared<FJsonValueObject>(NewEntry));
    }
    UProj->SetArrayField(TEXT("Plugins"), Plugins);

    FString OutJson;
    TSharedRef<TJsonWriter<>> Writer =
        TJsonWriterFactory<>::Create(&OutJson);
    FJsonSerializer::Serialize(UProj.ToSharedRef(), Writer);

    FString Err;
    if (!AtomicWriteString(UProjectPath, OutJson, Err))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, Err);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("plugin"),                  PluginName);
    R->SetBoolField  (TEXT("enabled"),                 bEnabled);
    R->SetStringField(TEXT("uproject_path"),           UProjectPath);
    R->SetBoolField  (TEXT("entry_existed"),           bUpdated);
    R->SetBoolField  (TEXT("requires_editor_restart"), true);
    R->SetStringField(TEXT("note"),
        TEXT("editor restart required for the change to take effect"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ProjectReadCppSourceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString InPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), InPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    int32 MaxBytes = 65536;  // 64KB default
    if (Args.IsValid())
    {
        double Num = 0;
        if (Args->TryGetNumberField(TEXT("max_bytes"), Num))
        {
            MaxBytes = FMath::Clamp(static_cast<int32>(Num), 1024, 524288);
        }
    }

    FString Err;
    const FString Abs = ResolveSafeSourcePath(InPath, Err);
    if (Abs.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *Abs))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("could not read source: %s"), *Abs));
    }

    const int32 OrigLen = Content.Len();
    bool bTruncated = false;
    if (Content.Len() > MaxBytes)
    {
        Content.LeftInline(MaxBytes);
        bTruncated = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Abs);
    R->SetStringField(TEXT("content"),    Content);
    R->SetNumberField(TEXT("size_bytes"), OrigLen);
    R->SetBoolField  (TEXT("truncated"),  bTruncated);
    R->SetNumberField(TEXT("max_bytes"),  MaxBytes);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.set_project ---------------------------------------------------

FSageToolDispatch::FOutcome ProjectSetProjectImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));

    const FString ConfigPath = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
    const TCHAR* const Section = TEXT("/Script/EngineSettings.GeneralProjectSettings");

    // Serialize a JSON value into the right INI textual form so booleans
    // emit as `True`/`False`, numbers as bare numerics, arrays as a single
    // string per element (UE INI convention), and strings verbatim. Naive
    // `AsString()` on a number still works (FJsonValueNumber stringifies),
    // but on a bool it returns "true"/"false" lowercase which UE's FConfig
    // accepts but doesn't round-trip cleanly.
    auto FormatValue = [](const TSharedPtr<FJsonValue>& V) -> FString
    {
        if (!V.IsValid()) return FString();
        switch (V->Type)
        {
            case EJson::Boolean: return V->AsBool() ? FString(TEXT("True")) : FString(TEXT("False"));
            case EJson::Number:
            {
                const double D = V->AsNumber();
                if (D == FMath::TruncToDouble(D))
                    return FString::Printf(TEXT("%lld"), static_cast<int64>(D));
                return FString::SanitizeFloat(D);
            }
            case EJson::String:  return V->AsString();
            case EJson::Null:    return FString();
            default:             return V->AsString();  // fallback for nested objects
        }
    };

    int32 Applied = 0;
    TArray<TSharedPtr<FJsonValue>> Skipped;
    for (const auto& Pair : Args->Values)
    {
        if (Pair.Key.IsEmpty()) continue;
        if (!Pair.Value.IsValid()) continue;

        if (Pair.Value->Type == EJson::Array)
        {
            // INI array convention: emit each element as a separate +Key=Val
            // entry. Use SetArray on GConfig — UE collapses to multiple lines.
            const TArray<TSharedPtr<FJsonValue>>& Arr = Pair.Value->AsArray();
            TArray<FString> AsStrings;
            AsStrings.Reserve(Arr.Num());
            for (const auto& E : Arr) AsStrings.Add(FormatValue(E));
            GConfig->SetArray(Section, *Pair.Key, AsStrings, ConfigPath);
            ++Applied;
            continue;
        }

        const FString Val = FormatValue(Pair.Value);
        switch (Pair.Value->Type)
        {
            case EJson::Boolean:
                GConfig->SetBool(Section, *Pair.Key, Pair.Value->AsBool(), ConfigPath);
                break;
            case EJson::Number:
                GConfig->SetDouble(Section, *Pair.Key, Pair.Value->AsNumber(), ConfigPath);
                break;
            case EJson::String:
                GConfig->SetString(Section, *Pair.Key, *Val, ConfigPath);
                break;
            default:
                Skipped.Add(MakeShared<FJsonValueString>(Pair.Key));
                continue;
        }
        ++Applied;
    }
    GConfig->Flush(false, ConfigPath);

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("settings_applied"), Applied);
    R->SetStringField(TEXT("config_path"),      ConfigPath);
    if (Skipped.Num() > 0) R->SetArrayField(TEXT("skipped_keys"), Skipped);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.read_module ---------------------------------------------------

FSageToolDispatch::FOutcome ProjectReadModuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ModuleName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("module"), ModuleName))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'module'"));

    // Search source directories for <ModuleName>.Build.cs
    TArray<FString> SearchRoots = {
        FPaths::GameSourceDir(),
        FPaths::ProjectPluginsDir(),
        FPaths::ProjectDir() / TEXT("Plugins"),
    };
    FString BuildCsPath;
    for (const FString& Root : SearchRoots)
    {
        TArray<FString> Found;
        IFileManager::Get().FindFilesRecursive(Found, *Root,
            *(ModuleName + TEXT(".Build.cs")), true, false);
        if (!Found.IsEmpty()) { BuildCsPath = Found[0]; break; }
    }

    if (BuildCsPath.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("Build.cs not found for module: %s"), *ModuleName));

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *BuildCsPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("could not read: %s"), *BuildCsPath));

    if (Content.Len() > 65536) Content.LeftInline(65536);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("module"),    ModuleName);
    R->SetStringField(TEXT("path"),      BuildCsPath);
    R->SetStringField(TEXT("content"),   Content);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.search_engine_cpp ---------------------------------------------

FSageToolDispatch::FOutcome ProjectSearchEngineCppImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Query;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("query"), Query))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'query'"));

    FString SearchRoot = FPaths::EngineSourceDir();
    FString SubPath;
    if (Args->TryGetStringField(TEXT("path"), SubPath) && !SubPath.IsEmpty())
        SearchRoot = FPaths::ConvertRelativePathToFull(SearchRoot / SubPath);

    int32 MaxResults = 20;
    if (Args.IsValid())
    {
        double N; if (Args->TryGetNumberField(TEXT("max_results"), N)) MaxResults = (int32)N;
    }

    TArray<FString> AllFiles;
    IFileManager::Get().FindFilesRecursive(AllFiles, *SearchRoot,
        TEXT("*.h"), true, false);
    TArray<FString> CppFiles;
    IFileManager::Get().FindFilesRecursive(CppFiles, *SearchRoot,
        TEXT("*.cpp"), true, false);
    AllFiles.Append(CppFiles);

    constexpr int64 kPerFileMaxBytes = 1 * 1024 * 1024;
    TArray<TSharedPtr<FJsonValue>> Matches;
    for (const FString& FilePath : AllFiles)
    {
        if (Matches.Num() >= MaxResults) break;
        const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
        if (FileSize > kPerFileMaxBytes) continue;  // skip oversized engine sources
        FString Content;
        if (!FFileHelper::LoadFileToString(Content, *FilePath)) continue;
        if (!Content.Contains(Query, ESearchCase::CaseSensitive)) continue;

        TArray<FString> Lines;
        Content.ParseIntoArrayLines(Lines);
        TArray<TSharedPtr<FJsonValue>> LineMatches;
        int32 LineNum = 1;
        for (const FString& Line : Lines)
        {
            if (Line.Contains(Query, ESearchCase::CaseSensitive))
            {
                auto LJ = MakeShared<FJsonObject>();
                LJ->SetNumberField(TEXT("line"), LineNum);
                LJ->SetStringField(TEXT("text"), Line.TrimStartAndEnd());
                LineMatches.Add(MakeShared<FJsonValueObject>(LJ));
                if (LineMatches.Num() >= 5) break;
            }
            ++LineNum;
        }
        if (!LineMatches.IsEmpty())
        {
            auto FJ = MakeShared<FJsonObject>();
            FJ->SetStringField(TEXT("file"),  FilePath);
            FJ->SetArrayField (TEXT("lines"), LineMatches);
            Matches.Add(MakeShared<FJsonValueObject>(FJ));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"),   Query);
    R->SetArrayField (TEXT("matches"), Matches);
    R->SetNumberField(TEXT("count"),   Matches.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.generate_project_files ----------------------------------------

FSageToolDispatch::FOutcome ProjectGenerateProjectFilesImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("Run GenerateProjectFiles.command (Mac) / GenerateProjectFiles.bat (Win) "
             "from the project directory, or use editor.run_python with "
             "unreal.PythonScriptLibrary.exec_on_project_update()"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.create_cpp_class ----------------------------------------------

// Parent class registry for known UE base classes: (a) UHT prefix
// ('A' for AActor descendants, 'U' for UObject descendants) and (b) include
// header path. UCLASS reflection depends on prefixes; UHT rejects a missing
// prefix with "Class deriving from 'AActor' must be prefixed with 'A'".
struct FParentClassInfo {
    const TCHAR* Prefix;          // "A", "U", ...
    const TCHAR* HeaderInclude;   // "GameFramework/Character.h"
};

static const TMap<FString, FParentClassInfo>& GetParentClassRegistry()
{
    // Static once-init; ~30 entry covers the %95-use-case of game C++ classes.
    static const TMap<FString, FParentClassInfo> R = {
        // AActor descendants
        { TEXT("AActor"),                    { TEXT("A"), TEXT("GameFramework/Actor.h") } },
        { TEXT("APawn"),                     { TEXT("A"), TEXT("GameFramework/Pawn.h") } },
        { TEXT("ACharacter"),                { TEXT("A"), TEXT("GameFramework/Character.h") } },
        { TEXT("APlayerController"),         { TEXT("A"), TEXT("GameFramework/PlayerController.h") } },
        { TEXT("APlayerState"),              { TEXT("A"), TEXT("GameFramework/PlayerState.h") } },
        { TEXT("AGameModeBase"),             { TEXT("A"), TEXT("GameFramework/GameModeBase.h") } },
        { TEXT("AGameMode"),                 { TEXT("A"), TEXT("GameFramework/GameMode.h") } },
        { TEXT("AGameStateBase"),            { TEXT("A"), TEXT("GameFramework/GameStateBase.h") } },
        { TEXT("AGameState"),                { TEXT("A"), TEXT("GameFramework/GameState.h") } },
        { TEXT("AHUD"),                      { TEXT("A"), TEXT("GameFramework/HUD.h") } },
        { TEXT("AInfo"),                     { TEXT("A"), TEXT("GameFramework/Info.h") } },
        { TEXT("AVolume"),                   { TEXT("A"), TEXT("GameFramework/Volume.h") } },
        { TEXT("AStaticMeshActor"),          { TEXT("A"), TEXT("Engine/StaticMeshActor.h") } },
        { TEXT("ATriggerVolume"),            { TEXT("A"), TEXT("Engine/TriggerVolume.h") } },
        { TEXT("AWorldSettings"),            { TEXT("A"), TEXT("GameFramework/WorldSettings.h") } },

        // UObject descendants
        { TEXT("UObject"),                       { TEXT("U"), TEXT("UObject/Object.h") } },
        { TEXT("UActorComponent"),               { TEXT("U"), TEXT("Components/ActorComponent.h") } },
        { TEXT("USceneComponent"),               { TEXT("U"), TEXT("Components/SceneComponent.h") } },
        { TEXT("UPrimitiveComponent"),           { TEXT("U"), TEXT("Components/PrimitiveComponent.h") } },
        { TEXT("UStaticMeshComponent"),          { TEXT("U"), TEXT("Components/StaticMeshComponent.h") } },
        { TEXT("USkeletalMeshComponent"),        { TEXT("U"), TEXT("Components/SkeletalMeshComponent.h") } },
        { TEXT("UCameraComponent"),              { TEXT("U"), TEXT("Camera/CameraComponent.h") } },
        { TEXT("USpringArmComponent"),           { TEXT("U"), TEXT("GameFramework/SpringArmComponent.h") } },
        { TEXT("UMovementComponent"),            { TEXT("U"), TEXT("GameFramework/MovementComponent.h") } },
        { TEXT("UPawnMovementComponent"),        { TEXT("U"), TEXT("GameFramework/PawnMovementComponent.h") } },
        { TEXT("UCharacterMovementComponent"),   { TEXT("U"), TEXT("GameFramework/CharacterMovementComponent.h") } },
        { TEXT("UFloatingPawnMovement"),         { TEXT("U"), TEXT("GameFramework/FloatingPawnMovement.h") } },
        { TEXT("UUserWidget"),                   { TEXT("U"), TEXT("Blueprint/UserWidget.h") } },
        { TEXT("UDataAsset"),                    { TEXT("U"), TEXT("Engine/DataAsset.h") } },
        { TEXT("UPrimaryDataAsset"),             { TEXT("U"), TEXT("Engine/DataAsset.h") } },
        { TEXT("UAnimInstance"),                 { TEXT("U"), TEXT("Animation/AnimInstance.h") } },
        { TEXT("UAnimNotify"),                   { TEXT("U"), TEXT("Animation/AnimNotifies/AnimNotify.h") } },
        { TEXT("UAnimNotifyState"),              { TEXT("U"), TEXT("Animation/AnimNotifies/AnimNotifyState.h") } },
        { TEXT("UBlueprintFunctionLibrary"),     { TEXT("U"), TEXT("Kismet/BlueprintFunctionLibrary.h") } },
        { TEXT("UDeveloperSettings"),            { TEXT("U"), TEXT("Engine/DeveloperSettings.h") } },
        { TEXT("USaveGame"),                     { TEXT("U"), TEXT("GameFramework/SaveGame.h") } },
        { TEXT("UGameInstance"),                 { TEXT("U"), TEXT("Engine/GameInstance.h") } },
        { TEXT("UGameViewportClient"),           { TEXT("U"), TEXT("Engine/GameViewportClient.h") } },
    };
    return R;
}

// Resolve the conventional UHT prefix for a parent class. Falls back to the
// parent's first character if it follows the UE convention; empty string
// means "couldn't infer — leave class name untouched."
static FString DerivePrefixFromParent(const FString& ParentClass)
{
    if (auto* Info = GetParentClassRegistry().Find(ParentClass))
        return FString(Info->Prefix);
    // Heuristic fallback: AActor convention is a single uppercase prefix
    // (A/U/F/T/I/E). If parent looks like one of those, reuse the same
    // prefix for the child.
    if (!ParentClass.IsEmpty())
    {
        const TCHAR Ch0 = ParentClass[0];
        if (Ch0 == TEXT('A') || Ch0 == TEXT('U'))
        {
            // Confirm second char is uppercase to avoid matching e.g. "Actor"
            // (already prefix-stripped; not common but be defensive).
            if (ParentClass.Len() > 1 && FChar::IsUpper(ParentClass[1]))
                return ParentClass.Left(1);
        }
    }
    return FString();
}

// Look up the conventional include header for a parent class. Empty if
// unknown — caller may fall back to user-supplied parent_header arg.
static FString DeriveHeaderFromParent(const FString& ParentClass)
{
    if (auto* Info = GetParentClassRegistry().Find(ParentClass))
        return FString(Info->HeaderInclude);
    return FString();
}

// Apply the UHT prefix to a user-provided class name if missing. Returns the
// (possibly-transformed) name and reports whether a prefix was prepended.
static FString ApplyPrefix(const FString& UserName, const FString& Prefix, bool& bOutPrepended)
{
    bOutPrepended = false;
    if (Prefix.IsEmpty() || UserName.IsEmpty()) return UserName;
    if (UserName.StartsWith(Prefix) && UserName.Len() > Prefix.Len()
        && FChar::IsUpper(UserName[Prefix.Len()]))
    {
        return UserName;  // already prefixed
    }
    bOutPrepended = true;
    return Prefix + UserName;
}

// ---- bootstrap helpers (BP-only project → C++ project upgrade) -------------

// Default Build.cs content for a freshly-bootstrapped Game module.
static FString MakeBuildCsContent(const FString& ModuleName)
{
    return FString::Printf(
        TEXT("using UnrealBuildTool;\n\n"
             "public class %s : ModuleRules\n"
             "{\n"
             "    public %s(ReadOnlyTargetRules Target) : base(Target)\n"
             "    {\n"
             "        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;\n\n"
             "        PublicDependencyModuleNames.AddRange(new string[] {\n"
             "            \"Core\", \"CoreUObject\", \"Engine\", \"InputCore\"\n"
             "        });\n\n"
             "        PrivateDependencyModuleNames.AddRange(new string[] { });\n"
             "    }\n"
             "}\n"),
        *ModuleName, *ModuleName);
}

static FString MakeGameTargetCsContent(const FString& ProjectName, const FString& ModuleName)
{
    return FString::Printf(
        TEXT("using UnrealBuildTool;\nusing System.Collections.Generic;\n\n"
             "public class %sTarget : TargetRules\n"
             "{\n"
             "    public %sTarget(TargetInfo Target) : base(Target)\n"
             "    {\n"
             "        Type = TargetType.Game;\n"
             "        DefaultBuildSettings = BuildSettingsVersion.V6;\n"
             "        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;\n"
             "        ExtraModuleNames.AddRange(new string[] { \"%s\" });\n"
             "    }\n"
             "}\n"),
        *ProjectName, *ProjectName, *ModuleName);
}

static FString MakeEditorTargetCsContent(const FString& ProjectName, const FString& ModuleName)
{
    return FString::Printf(
        TEXT("using UnrealBuildTool;\nusing System.Collections.Generic;\n\n"
             "public class %sEditorTarget : TargetRules\n"
             "{\n"
             "    public %sEditorTarget(TargetInfo Target) : base(Target)\n"
             "    {\n"
             "        Type = TargetType.Editor;\n"
             "        DefaultBuildSettings = BuildSettingsVersion.V6;\n"
             "        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;\n"
             "        ExtraModuleNames.AddRange(new string[] { \"%s\" });\n"
             "    }\n"
             "}\n"),
        *ProjectName, *ProjectName, *ModuleName);
}

static FString MakeModuleHeaderContent(const FString& ModuleName)
{
    return FString::Printf(
        TEXT("#pragma once\n\n#include \"CoreMinimal.h\"\n#include \"Modules/ModuleManager.h\"\n\n"
             "class F%sModule : public IModuleInterface\n"
             "{\n"
             "public:\n"
             "    virtual void StartupModule() override {}\n"
             "    virtual void ShutdownModule() override {}\n"
             "};\n"),
        *ModuleName);
}

static FString MakeModuleSourceContent(const FString& ModuleName)
{
    return FString::Printf(
        TEXT("#include \"%s.h\"\n\n"
             "IMPLEMENT_PRIMARY_GAME_MODULE(F%sModule, %s, \"%s\");\n"),
        *ModuleName, *ModuleName, *ModuleName, *ModuleName);
}

// Patch the .uproject: ensure Modules[] contains an entry for ModuleName.
// Returns true if the file was actually modified.
static bool PatchUProjectAddModule(const FString& ModuleName, FString& OutErr)
{
    const FString UProjectPath = FPaths::GetProjectFilePath();
    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *UProjectPath))
    {
        OutErr = TEXT("could not read .uproject");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    auto Reader = TJsonReaderFactory<>::Create(Content);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutErr = TEXT("could not parse .uproject");
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> Modules;
    if (Root->HasField(TEXT("Modules")))
    {
        const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
        if (Root->TryGetArrayField(TEXT("Modules"), Existing))
        {
            for (const auto& V : *Existing)
            {
                const TSharedPtr<FJsonObject>* MObj = nullptr;
                if (V.IsValid() && V->TryGetObject(MObj))
                {
                    FString Name;
                    (*MObj)->TryGetStringField(TEXT("Name"), Name);
                    if (Name == ModuleName)
                    {
                        // Already present — nothing to do.
                        return true;
                    }
                    Modules.Add(V);
                }
            }
        }
    }
    auto NewMod = MakeShared<FJsonObject>();
    NewMod->SetStringField(TEXT("Name"),         ModuleName);
    NewMod->SetStringField(TEXT("Type"),         TEXT("Runtime"));
    NewMod->SetStringField(TEXT("LoadingPhase"), TEXT("Default"));
    Modules.Add(MakeShared<FJsonValueObject>(NewMod));
    Root->SetArrayField(TEXT("Modules"), Modules);

    FString Out;
    auto Writer = TJsonWriterFactory<>::Create(&Out);
    if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
    {
        OutErr = TEXT("could not serialize .uproject");
        return false;
    }
    FString WriteErr;
    if (!AtomicWriteString(UProjectPath, Out, WriteErr))
    {
        OutErr = FString::Printf(TEXT("could not write .uproject: %s (%s)"),
                                  *UProjectPath, *WriteErr);
        return false;
    }
    return true;
}

// Bootstrap a freshly-scaffolded native module under Source/<ModuleName>/.
// Writes Build.cs + module .h/.cpp; writes Source/<Project>.Target.cs +
// <Project>Editor.Target.cs; patches the .uproject. Returns the list of
// files actually written.
static TArray<FString> BootstrapNativeModule(const FString& ModuleName, FString& OutErr)
{
    TArray<FString> Written;
    const FString SrcRoot   = FPaths::GameSourceDir();
    const FString ModuleDir = SrcRoot / ModuleName;
    const FString ProjectName = FApp::GetProjectName();

    if (!IFileManager::Get().DirectoryExists(*ModuleDir))
    {
        if (!IFileManager::Get().MakeDirectory(*ModuleDir, /*Tree*/ true))
        {
            OutErr = FString::Printf(TEXT("could not create module dir: %s"), *ModuleDir);
            return Written;
        }
    }

    auto WriteIfMissing = [&](const FString& Path, const FString& Body) -> bool
    {
        if (FPaths::FileExists(Path)) return true;
        if (!FFileHelper::SaveStringToFile(Body, *Path))
        {
            OutErr = FString::Printf(TEXT("could not write: %s"), *Path);
            return false;
        }
        Written.Add(Path);
        return true;
    };

    if (!WriteIfMissing(ModuleDir / (ModuleName + TEXT(".Build.cs")),
                        MakeBuildCsContent(ModuleName))) return Written;
    if (!WriteIfMissing(ModuleDir / (ModuleName + TEXT(".h")),
                        MakeModuleHeaderContent(ModuleName))) return Written;
    if (!WriteIfMissing(ModuleDir / (ModuleName + TEXT(".cpp")),
                        MakeModuleSourceContent(ModuleName))) return Written;
    if (!WriteIfMissing(SrcRoot / (ProjectName + TEXT(".Target.cs")),
                        MakeGameTargetCsContent(ProjectName, ModuleName))) return Written;
    if (!WriteIfMissing(SrcRoot / (ProjectName + TEXT("Editor.Target.cs")),
                        MakeEditorTargetCsContent(ProjectName, ModuleName))) return Written;

    if (!PatchUProjectAddModule(ModuleName, OutErr)) return Written;
    return Written;
}

FSageToolDispatch::FOutcome ProjectCreateCppClassImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ClassName, ParentClass, ModuleName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("class_name"), ClassName)
        || ClassName.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'class_name'"));

    Args->TryGetStringField(TEXT("parent_class"), ParentClass);
    Args->TryGetStringField(TEXT("module"),       ModuleName);

    bool bBootstrap = false;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("bootstrap_module"), bBootstrap);

    if (ParentClass.IsEmpty()) ParentClass = TEXT("UObject");
    if (ModuleName.IsEmpty())  ModuleName  = FApp::GetProjectName();

    const FString SrcRoot   = FPaths::GameSourceDir();
    const FString ModuleDir = SrcRoot / ModuleName;
    const FString BuildCs   = ModuleDir / (ModuleName + TEXT(".Build.cs"));

    // Bootstrap path: if no module exists and caller asked for it, scaffold
    // a fresh native module (Build.cs + module .h/.cpp + Target.cs +
    // EditorTarget.cs + .uproject Modules[] patch). This converts a BP-only
    // project into a C++-capable one in one shot.
    bool bDidBootstrap = false;
    TArray<FString> ScaffoldFiles;
    if (!FPaths::FileExists(BuildCs))
    {
        if (!bBootstrap)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("module '%s' has no Source/%s/%s.Build.cs; "
                                     "pass bootstrap_module=true to scaffold it "
                                     "(creates Build.cs, Target.cs files, and "
                                     "patches the .uproject) — editor restart "
                                     "required afterward to compile."),
                                *ModuleName, *ModuleName, *ModuleName));
        }
        FString Err;
        ScaffoldFiles = BootstrapNativeModule(ModuleName, Err);
        if (!Err.IsEmpty())
            return FSageToolDispatch::FOutcome::MakeError(-32603, Err);
        bDidBootstrap = true;
    }

    // Class lives under Source/<Module>/[<subfolder>/].
    FString Subfolder;
    Args->TryGetStringField(TEXT("subfolder"), Subfolder);
    FString TargetDir = ModuleDir;
    if (!Subfolder.IsEmpty())
    {
        TargetDir = TargetDir / Subfolder;
        if (!IFileManager::Get().DirectoryExists(*TargetDir))
            IFileManager::Get().MakeDirectory(*TargetDir, /*Tree*/ true);
    }

    // UHT compliance:
    //   1. Apply conventional prefix (AActor descendants → 'A', UObject → 'U').
    //   2. Include the parent class's header (CoreMinimal.h alone doesn't bring
    //      ACharacter/UActorComponent/...).
    // Falls back to user-supplied `parent_header` arg if registry doesn't know
    // the parent (custom base class scenario).
    const FString DerivedPrefix = DerivePrefixFromParent(ParentClass);
    bool bPrefixApplied = false;
    const FString FinalClassName = ApplyPrefix(ClassName, DerivedPrefix, bPrefixApplied);

    FString ParentHeader;
    Args->TryGetStringField(TEXT("parent_header"), ParentHeader);
    if (ParentHeader.IsEmpty())
        ParentHeader = DeriveHeaderFromParent(ParentClass);
    // Normalize: drop a leading slash if present.
    if (ParentHeader.StartsWith(TEXT("/"))) ParentHeader.RemoveAt(0);

    const FString HeaderPath = TargetDir / FinalClassName + TEXT(".h");
    const FString SourcePath = TargetDir / FinalClassName + TEXT(".cpp");

    if (FPaths::FileExists(HeaderPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("file already exists: %s"), *HeaderPath));

    // Compose includes block: CoreMinimal + parent header (if known) + the
    // generated.h sentinel must be LAST (UHT requirement).
    FString IncludeBlock = TEXT("#include \"CoreMinimal.h\"\n");
    if (!ParentHeader.IsEmpty())
        IncludeBlock += FString::Printf(TEXT("#include \"%s\"\n"), *ParentHeader);
    IncludeBlock += FString::Printf(TEXT("#include \"%s.generated.h\"\n"), *FinalClassName);

    const FString ModuleUpper = ModuleName.ToUpper();
    const FString Header = FString::Printf(
        TEXT("#pragma once\n%s\n"
             "UCLASS()\nclass %s_API %s : public %s\n{\n    GENERATED_BODY()\n};\n"),
        *IncludeBlock, *ModuleUpper, *FinalClassName, *ParentClass);

    const FString Source = FString::Printf(
        TEXT("#include \"%s.h\"\n"), *FinalClassName);

    // Header + source must be a transactional pair: a half-written class
    // (header alone) leaves UHT in an inconsistent state where the next
    // build fails noisily and the user has to clean up by hand. Roll back
    // the header if the source write fails.
    {
        FString WriteErr;
        if (!AtomicWriteString(HeaderPath, Header, WriteErr))
            return FSageToolDispatch::FOutcome::MakeError(-32000,
                FString::Printf(TEXT("could not write header %s: %s"), *HeaderPath, *WriteErr));
        if (!AtomicWriteString(SourcePath, Source, WriteErr))
        {
            // Best-effort rollback so the project doesn't have an orphan .h.
            IFileManager::Get().Delete(*HeaderPath,
                /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
            return FSageToolDispatch::FOutcome::MakeError(-32000,
                FString::Printf(TEXT("could not write source %s: %s; header rolled back"),
                                *SourcePath, *WriteErr));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("class_name"),     FinalClassName);
    R->SetStringField(TEXT("header_path"),    HeaderPath);
    R->SetStringField(TEXT("source_path"),    SourcePath);
    R->SetStringField(TEXT("parent_class"),   ParentClass);
    R->SetStringField(TEXT("module"),         ModuleName);
    R->SetBoolField  (TEXT("prefix_applied"), bPrefixApplied);
    if (bPrefixApplied)
        R->SetStringField(TEXT("prefix"), DerivedPrefix);
    if (!ParentHeader.IsEmpty())
        R->SetStringField(TEXT("parent_header"), ParentHeader);
    else
        R->SetStringField(TEXT("warning"),
            FString::Printf(TEXT("parent_class '%s' not in registry; no header "
                                 "auto-included. Pass parent_header=\"...\" or "
                                 "edit the .h to add the include manually."),
                            *ParentClass));
    R->SetBoolField(TEXT("bootstrapped"), bDidBootstrap);
    R->SetBoolField(TEXT("requires_editor_restart"), bDidBootstrap);
    if (bDidBootstrap)
    {
        TArray<TSharedPtr<FJsonValue>> ScaffoldArr;
        for (const FString& F : ScaffoldFiles)
            ScaffoldArr.Add(MakeShared<FJsonValueString>(F));
        R->SetArrayField(TEXT("scaffold_files"), ScaffoldArr);
        R->SetStringField(TEXT("note"),
            TEXT("Project upgraded to C++. Editor restart required (reopen "
                 ".uproject); UE will detect new modules and prompt to compile."));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.list_project_modules ------------------------------------------

FSageToolDispatch::FOutcome ProjectListProjectModulesImpl(const TSharedPtr<FJsonObject>& Args)
{
    // Parse .uproject JSON for Modules array
    FString UProjectPath = FPaths::GetProjectFilePath();
    FString UProjectContent;
    if (!FFileHelper::LoadFileToString(UProjectContent, *UProjectPath))
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("could not read .uproject"));

    TSharedPtr<FJsonObject> UProj;
    TSharedRef<TJsonReader<>> JR = TJsonReaderFactory<>::Create(UProjectContent);
    if (!FJsonSerializer::Deserialize(JR, UProj) || !UProj.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to parse .uproject"));

    const TArray<TSharedPtr<FJsonValue>>* ModArr = nullptr;
    TArray<TSharedPtr<FJsonValue>> Modules;

    if (UProj->TryGetArrayField(TEXT("Modules"), ModArr))
    {
        for (const auto& MV : *ModArr)
        {
            if (!MV.IsValid()) continue;
            const TSharedPtr<FJsonObject>* MObj;
            if (MV->TryGetObject(MObj))
                Modules.Add(MakeShared<FJsonValueObject>(*MObj));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("modules"), Modules);
    R->SetNumberField(TEXT("count"),   Modules.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.live_coding_compile -------------------------------------------

FSageToolDispatch::FOutcome ProjectLiveCodingCompileImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!GEditor)
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no GEditor"));

    GEditor->Exec(GEditor->GetEditorWorldContext().World(),
        TEXT("LiveCoding.Compile"), *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("triggered"), true);
    R->SetStringField(TEXT("note"), TEXT("live coding compile dispatched; check Output Log for result"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.write_cpp_file ------------------------------------------------

FSageToolDispatch::FOutcome ProjectWriteCppFileImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString RelPath, Content;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), RelPath)
        || !Args->TryGetStringField(TEXT("content"), Content))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path' or 'content'"));

    // Arbitrary-path source overwrite is high-risk: agent could clobber
    // critical project files, INI/uproject (which have dedicated tools that
    // do atomic + structural validation), or — if path resolution lets
    // EngineDir through — engine sources. Lock down with three gates.
    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("project.write_cpp_file requires confirmed=true (overwrites source on disk)"));

    FString Err;
    const FString Abs = ResolveSafeSourcePath(RelPath, Err);
    if (Abs.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    // Sandbox: must resolve under ProjectDir(); reject EngineDir.
    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    if (!Abs.StartsWith(ProjectDir))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("write_cpp_file rejected: path '%s' is outside ProjectDir; "
                                 "engine source writes are not permitted via this tool"),
                            *Abs));

    // Extension whitelist. Configs (.ini, .uproject, .uplugin) have dedicated
    // tools that do structural patching; refuse to clobber them through here.
    const FString Lower = Abs.ToLower();
    auto EndsWith = [&Lower](const TCHAR* Suffix) { return Lower.EndsWith(Suffix); };
    const bool bAllowed =
           EndsWith(TEXT(".h"))
        || EndsWith(TEXT(".cpp"))
        || EndsWith(TEXT(".inl"))
        || EndsWith(TEXT(".build.cs"))
        || EndsWith(TEXT(".target.cs"))
        || EndsWith(TEXT(".cs"));
    if (!bAllowed)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("write_cpp_file rejected: extension not in whitelist "
                                 "(.h/.cpp/.inl/.cs/.Build.cs/.Target.cs); for INI use "
                                 "project.set_config, for .uproject use project.set_plugin_enabled."
                                 " path='%s'"),
                            *Abs));

    {
        FString WriteErr;
        if (!AtomicWriteString(Abs, Content, WriteErr))
            return FSageToolDispatch::FOutcome::MakeError(-32000,
                FString::Printf(TEXT("could not write %s: %s"), *Abs, *WriteErr));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),          Abs);
    R->SetNumberField(TEXT("bytes_written"), Content.Len());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- project.add_module_dependency -----------------------------------------

FSageToolDispatch::FOutcome ProjectAddModuleDependencyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ModuleName, Dependency;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("module"),     ModuleName)
        || !Args->TryGetStringField(TEXT("dependency"), Dependency))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'module' or 'dependency'"));

    // Find Build.cs
    TArray<FString> SearchRoots = { FPaths::GameSourceDir(), FPaths::ProjectPluginsDir() };
    FString BuildCsPath;
    for (const FString& Root : SearchRoots)
    {
        TArray<FString> Found;
        IFileManager::Get().FindFilesRecursive(Found, *Root,
            *(ModuleName + TEXT(".Build.cs")), true, false);
        if (!Found.IsEmpty()) { BuildCsPath = Found[0]; break; }
    }

    if (BuildCsPath.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("Build.cs not found for: %s"), *ModuleName));

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *BuildCsPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("could not read Build.cs"));

    // Insert into PublicDependencyModuleNames (default) or
    // PrivateDependencyModuleNames (when private=true).
    bool bPrivate = false;
    Args->TryGetBoolField(TEXT("private"), bPrivate);
    const FString TargetArrayName = bPrivate
        ? TEXT("PrivateDependencyModuleNames")
        : TEXT("PublicDependencyModuleNames");

    FString InsertTarget = TargetArrayName + TEXT(".AddRange");
    int32 Idx = Content.Find(InsertTarget);
    if (Idx == INDEX_NONE)
    {
        InsertTarget = TargetArrayName + TEXT(".Add");
        Idx = Content.Find(InsertTarget);
    }

    // Lyra Gap #9 regression fix: substring-only `Content.Contains(*Dependency)`
    // produces false-positive when adding "Core" to a Build.cs that already has
    // "CoreUObject" / "CoreOnline" / etc. Parse the array literal block of the
    // target dependency list and check for exact-quoted entry instead.
    auto AlreadyPresentExact = [&]() -> bool
    {
        if (Idx == INDEX_NONE) return false;
        const int32 OpenBrace = Content.Find(TEXT("{"), ESearchCase::IgnoreCase,
                                              ESearchDir::FromStart, Idx);
        const int32 CloseBrace = (OpenBrace != INDEX_NONE)
            ? Content.Find(TEXT("}"), ESearchCase::IgnoreCase,
                           ESearchDir::FromStart, OpenBrace + 1)
            : INDEX_NONE;
        if (OpenBrace == INDEX_NONE || CloseBrace == INDEX_NONE) return false;
        const FString Block = Content.Mid(OpenBrace + 1, CloseBrace - OpenBrace - 1);
        TArray<FString> Parts;
        Block.ParseIntoArray(Parts, TEXT(","), /*bCullEmpty*/ true);
        for (FString Part : Parts)
        {
            Part.TrimStartAndEndInline();
            // Strip surrounding quotes.
            if (Part.StartsWith(TEXT("\""))) Part.RemoveAt(0);
            if (Part.EndsWith(TEXT("\""))) Part.RemoveAt(Part.Len() - 1);
            Part.TrimStartAndEndInline();
            if (Part == Dependency) return true;
        }
        return false;
    };

    if (AlreadyPresentExact())
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("module"),          ModuleName);
        R->SetStringField(TEXT("dependency"),      Dependency);
        R->SetStringField(TEXT("build_cs"),        BuildCsPath);
        R->SetBoolField  (TEXT("already_present"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    bool bPatched = false;
    if (Idx != INDEX_NONE)
    {
        // Locate the array literal:  AddRange(new string[] { ... });
        // The previous impl injected at `);` which falls between `}` and `)`,
        // producing  ..."InputCore"\n}\n   "EngineCameras",); — C# syntax error.
        // Correct insertion is just before the closing `}` of the array
        // literal, after the last existing element (handling trailing comma
        // both ways).
        const int32 OpenBrace = Content.Find(TEXT("{"), ESearchCase::IgnoreCase,
                                              ESearchDir::FromStart, Idx);
        const int32 CloseBrace = (OpenBrace != INDEX_NONE)
            ? Content.Find(TEXT("}"), ESearchCase::IgnoreCase,
                           ESearchDir::FromStart, OpenBrace + 1)
            : INDEX_NONE;
        if (OpenBrace != INDEX_NONE && CloseBrace != INDEX_NONE)
        {
            // Walk back from `}` to the last non-whitespace char to learn
            // whether the array already has a trailing comma.
            int32 LastChar = CloseBrace - 1;
            while (LastChar > OpenBrace && FChar::IsWhitespace(Content[LastChar]))
                --LastChar;

            FString Insert;
            const TCHAR LastCh = (LastChar > OpenBrace) ? Content[LastChar] : TEXT('{');
            if (LastCh == TEXT(',') || LastCh == TEXT('{'))
            {
                // Trailing comma (or empty array) — append a fresh entry.
                Insert = FString::Printf(TEXT("\n            \"%s\","), *Dependency);
                Content.InsertAt(LastChar + 1, Insert);
            }
            else
            {
                // Last char is `"` (closing quote of last existing string).
                // Add a leading comma to separate, then our entry.
                Insert = FString::Printf(TEXT(",\n            \"%s\""), *Dependency);
                Content.InsertAt(LastChar + 1, Insert);
            }
            bPatched = true;
        }
    }

    if (!bPatched)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("note"),
            FString::Printf(TEXT("could not auto-patch Build.cs; add \"%s\" manually to "
                "%s in %s"), *Dependency, *TargetArrayName, *BuildCsPath));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    {
        FString WriteErr;
        if (!AtomicWriteString(BuildCsPath, Content, WriteErr))
            return FSageToolDispatch::FOutcome::MakeError(-32000,
                FString::Printf(TEXT("could not write Build.cs: %s"), *WriteErr));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("module"),     ModuleName);
    R->SetStringField(TEXT("dependency"), Dependency);
    R->SetStringField(TEXT("build_cs"),   BuildCsPath);
    R->SetBoolField  (TEXT("patched"),    true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterProjectTools(FSageToolDispatch& Dispatch)
{
    auto GT = [](FSageToolDispatch::FOutcome (*Fn)(const TSharedPtr<FJsonObject>&))
    {
        return [Fn](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
        {
            return detail::RunOnGameThread([&]() -> FSageToolDispatch::FOutcome
            {
                return Fn(Args);
            });
        };
    };

    Dispatch.RegisterHandler(TEXT("project.get_info"),            GT(&ProjectGetInfoImpl));
    Dispatch.RegisterHandler(TEXT("project.list_modules"),        GT(&ProjectListModulesImpl));
    Dispatch.RegisterHandler(TEXT("project.read_cpp_header"),     GT(&ProjectReadCppHeaderImpl));
    Dispatch.RegisterHandler(TEXT("project.read_cpp_source"),     GT(&ProjectReadCppSourceImpl));

    // Phase 4.7 batch 2: engine source + search
    Dispatch.RegisterHandler(TEXT("project.search_cpp"),          GT(&ProjectSearchCppImpl));
    Dispatch.RegisterHandler(TEXT("project.list_engine_modules"), GT(&ProjectListEngineModulesImpl));
    Dispatch.RegisterHandler(TEXT("project.read_engine_header"),  GT(&ProjectReadEngineHeaderImpl));
    Dispatch.RegisterHandler(TEXT("project.find_engine_symbol"),  GT(&ProjectFindEngineSymbolImpl));

    // Phase 4.7 batch 3: INI config tree
    Dispatch.RegisterHandler(TEXT("project.read_config"),         GT(&ProjectReadConfigImpl));
    Dispatch.RegisterHandler(TEXT("project.search_config"),       GT(&ProjectSearchConfigImpl));
    Dispatch.RegisterHandler(TEXT("project.list_config_tags"),    GT(&ProjectListConfigTagsImpl));

    // Phase 4.7 batch 4: INI write + plugin enable
    Dispatch.RegisterHandler(TEXT("project.set_config"),          GT(&ProjectSetConfigImpl));
    Dispatch.RegisterHandler(TEXT("project.set_plugin_enabled"),  GT(&ProjectSetPluginEnabledImpl));

    // Trailing project tools
    Dispatch.RegisterHandler(TEXT("project.set_project"),             GT(&ProjectSetProjectImpl));
    Dispatch.RegisterHandler(TEXT("project.read_module"),             GT(&ProjectReadModuleImpl));
    Dispatch.RegisterHandler(TEXT("project.search_engine_cpp"),       GT(&ProjectSearchEngineCppImpl));
    Dispatch.RegisterHandler(TEXT("project.generate_project_files"),  GT(&ProjectGenerateProjectFilesImpl));
    Dispatch.RegisterHandler(TEXT("project.create_cpp_class"),        GT(&ProjectCreateCppClassImpl));
    Dispatch.RegisterHandler(TEXT("project.list_project_modules"),    GT(&ProjectListProjectModulesImpl));
    Dispatch.RegisterHandler(TEXT("project.live_coding_compile"),     GT(&ProjectLiveCodingCompileImpl));
    Dispatch.RegisterHandler(TEXT("project.write_cpp_file"),          GT(&ProjectWriteCppFileImpl));
    Dispatch.RegisterHandler(TEXT("project.add_module_dependency"),   GT(&ProjectAddModuleDependencyImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
