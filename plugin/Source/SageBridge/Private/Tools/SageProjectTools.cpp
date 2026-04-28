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

// Phase 4.7-p4: INI write + plugin enable.

// Atomic file write: write to <path>.tmp, then rename.
bool AtomicWriteString(const FString& Path, const FString& Content, FString& OutError)
{
    const FString TempPath = Path + TEXT(".sage_tmp");
    if (!FFileHelper::SaveStringToFile(Content, *TempPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = FString::Printf(TEXT("could not write temp: %s"), *TempPath);
        return false;
    }
    IFileManager& FM = IFileManager::Get();
    if (FM.FileExists(*Path))
    {
        // Best-effort backup. If the user runs set_config on a freshly-
        // generated INI the .bak gets overwritten on next call — that's
        // fine, this isn't an undo log, just a one-shot safety net.
        const FString BackupPath = Path + TEXT(".sage_bak");
        FM.Delete(*BackupPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
        FM.Move(*BackupPath, *Path, /*bReplace*/ true, /*bEvenIfReadOnly*/ true,
                /*bAttributes*/ false, /*bDoNotRetryOrError*/ true);
    }
    if (!FM.Move(*Path, *TempPath, /*bReplace*/ true, /*bEvenIfReadOnly*/ true,
                 /*bAttributes*/ false, /*bDoNotRetryOrError*/ true))
    {
        OutError = FString::Printf(TEXT("rename failed: %s -> %s"), *TempPath, *Path);
        return false;
    }
    return true;
}

// Modify or insert a key=value pair under [section] in INI text. Preserves
// other lines verbatim. Modifier prefix (+/-/!/.) becomes part of the
// emitted line if supplied. Returns true if anything changed.
bool UpsertIniKey(FString& Content, const FString& Section, const FString& Key,
                  const FString& Value, const FString& Modifier)
{
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
                Content = FString::Join(Lines, TEXT("\n"));
                if (!Content.EndsWith(TEXT("\n"))) Content += TEXT("\n");
                return true;
            }
        }
        // Insert at end of section.
        Lines.Insert(Emit, SectionEnd + 1);
        Content = FString::Join(Lines, TEXT("\n"));
        if (!Content.EndsWith(TEXT("\n"))) Content += TEXT("\n");
        return true;
    }

    // Section absent: append a fresh section block.
    if (!Content.IsEmpty() && !Content.EndsWith(TEXT("\n"))) Content += TEXT("\n");
    if (!Content.IsEmpty()) Content += TEXT("\n");
    Content += SectionHeader + TEXT("\n");
    Content += Emit + TEXT("\n");
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
    R->SetStringField(TEXT("backup"),   bCreated ? FString() : (Resolved + TEXT(".sage_bak")));
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
    R->SetStringField(TEXT("plugin"),         PluginName);
    R->SetBoolField  (TEXT("enabled"),        bEnabled);
    R->SetStringField(TEXT("uproject_path"),  UProjectPath);
    R->SetBoolField  (TEXT("entry_existed"),  bUpdated);
    R->SetStringField(TEXT("backup"),         UProjectPath + TEXT(".sage_bak"));
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

    FString Err;
    const FString ConfigPath = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));

    int32 Applied = 0;
    for (const auto& Pair : Args->Values)
    {
        if (Pair.Key.IsEmpty()) continue;
        FString Val = Pair.Value->AsString();
        // Write to DefaultGame.ini [/Script/EngineSettings.GeneralProjectSettings]
        GConfig->SetString(
            TEXT("/Script/EngineSettings.GeneralProjectSettings"),
            *Pair.Key, *Val, ConfigPath);
        ++Applied;
    }
    GConfig->Flush(false, ConfigPath);

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("settings_applied"), Applied);
    R->SetStringField(TEXT("config_path"),      ConfigPath);
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

    TArray<TSharedPtr<FJsonValue>> Matches;
    for (const FString& FilePath : AllFiles)
    {
        if (Matches.Num() >= MaxResults) break;
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

FSageToolDispatch::FOutcome ProjectCreateCppClassImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ClassName, ParentClass, ModuleName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("class_name"), ClassName)
        || ClassName.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'class_name'"));

    Args->TryGetStringField(TEXT("parent_class"), ParentClass);
    Args->TryGetStringField(TEXT("module"),       ModuleName);

    if (ParentClass.IsEmpty()) ParentClass = TEXT("UObject");
    if (ModuleName.IsEmpty())  ModuleName  = FApp::GetProjectName();

    // Locate module source dir
    FString SrcDir = FPaths::GameSourceDir() / ModuleName;
    if (!FPaths::DirectoryExists(SrcDir))
        SrcDir = FPaths::GameSourceDir();

    FString Subfolder;
    Args->TryGetStringField(TEXT("subfolder"), Subfolder);
    if (!Subfolder.IsEmpty()) SrcDir = SrcDir / Subfolder;

    FString HeaderPath = SrcDir / ClassName + TEXT(".h");
    FString SourcePath = SrcDir / ClassName + TEXT(".cpp");

    if (FPaths::FileExists(HeaderPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("file already exists: %s"), *HeaderPath));

    FString ModuleUpper = ModuleName.ToUpper();
    FString Header = FString::Printf(
        TEXT("#pragma once\n#include \"CoreMinimal.h\"\n#include \"%s.generated.h\"\n\n"
             "UCLASS()\nclass %s_API %s : public %s\n{\n    GENERATED_BODY()\n};\n"),
        *ClassName, *ModuleUpper, *ClassName, *ParentClass);

    FString Source = FString::Printf(
        TEXT("#include \"%s.h\"\n"), *ClassName);

    if (!FFileHelper::SaveStringToFile(Header, *HeaderPath))
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("could not write: %s"), *HeaderPath));
    if (!FFileHelper::SaveStringToFile(Source, *SourcePath))
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("could not write: %s"), *SourcePath));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("class_name"),   ClassName);
    R->SetStringField(TEXT("header_path"),  HeaderPath);
    R->SetStringField(TEXT("source_path"),  SourcePath);
    R->SetStringField(TEXT("parent_class"), ParentClass);
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

    FString Err;
    const FString Abs = ResolveSafeSourcePath(RelPath, Err);
    if (Abs.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    if (!FFileHelper::SaveStringToFile(Content, *Abs))
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("could not write: %s"), *Abs));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         Abs);
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

    // Check if already present
    if (Content.Contains(*Dependency))
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("module"),        ModuleName);
        R->SetStringField(TEXT("dependency"),    Dependency);
        R->SetBoolField  (TEXT("already_present"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    // Insert into PublicDependencyModuleNames or PrivateDependencyModuleNames
    FString InsertTarget = TEXT("PublicDependencyModuleNames.AddRange");
    int32 Idx = Content.Find(InsertTarget);
    if (Idx == INDEX_NONE)
    {
        InsertTarget = TEXT("PublicDependencyModuleNames.Add");
        Idx = Content.Find(InsertTarget);
    }

    bool bPatched = false;
    if (Idx != INDEX_NONE)
    {
        // Find the closing paren/bracket of this call
        int32 End = Content.Find(TEXT(");"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Idx);
        if (End != INDEX_NONE)
        {
            FString Insert = FString::Printf(TEXT("\n            \"%s\","), *Dependency);
            Content.InsertAt(End, Insert);
            bPatched = true;
        }
    }

    if (!bPatched)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("note"),
            FString::Printf(TEXT("could not auto-patch Build.cs; add \"%s\" manually to "
                "PublicDependencyModuleNames in %s"), *Dependency, *BuildCsPath));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (!FFileHelper::SaveStringToFile(Content, *BuildCsPath))
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("could not write Build.cs"));

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
