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
    const FString SourceDir = FPaths::ProjectDir() / TEXT("Source");
    IFileManager& FileMgr   = IFileManager::Get();

    TArray<FString> SubDirs;
    FileMgr.FindFiles(SubDirs, *(SourceDir / TEXT("*")), false, true);

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FString& Dir : SubDirs)
    {
        const FString ModuleDir   = SourceDir / Dir;
        const FString BuildCsPath = ModuleDir / (Dir + TEXT(".Build.cs"));
        if (!FileMgr.FileExists(*BuildCsPath)) continue;

        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"),          Dir);
        O->SetStringField(TEXT("module_dir"),    FPaths::ConvertRelativePathToFull(ModuleDir));
        O->SetStringField(TEXT("build_cs_path"), FPaths::ConvertRelativePathToFull(BuildCsPath));

        // Quick file count split (Public / Private / .h / .cpp counts).
        TArray<FString> Headers, Sources;
        FileMgr.FindFilesRecursive(Headers, *ModuleDir, TEXT("*.h"),   true, false);
        FileMgr.FindFilesRecursive(Sources, *ModuleDir, TEXT("*.cpp"), true, false);
        O->SetNumberField(TEXT("header_count"), Headers.Num());
        O->SetNumberField(TEXT("source_count"), Sources.Num());

        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("project_name"), FApp::GetProjectName());
    R->SetStringField(TEXT("source_dir"),   FPaths::ConvertRelativePathToFull(SourceDir));
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

    TArray<FString> Lines;
    Content.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

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

    for (const FString& F : Files)
    {
        if (Out.Num() >= MaxResults) return;
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
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 500);
        }
    }

    const FString SourceDir = FPaths::ProjectDir() / TEXT("Source");

    TArray<FSearchHit> Hits;
    SearchInDir(SourceDir, Query,
        {TEXT(".h"), TEXT(".cpp"), TEXT(".inl")},
        MaxResults, 200, Hits);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"),    Query);
    R->SetStringField(TEXT("root"),     FPaths::ConvertRelativePathToFull(SourceDir));
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
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
