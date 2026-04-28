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

    Dispatch.RegisterHandler(TEXT("project.get_info"),         GT(&ProjectGetInfoImpl));
    Dispatch.RegisterHandler(TEXT("project.list_modules"),     GT(&ProjectListModulesImpl));
    Dispatch.RegisterHandler(TEXT("project.read_cpp_header"),  GT(&ProjectReadCppHeaderImpl));
    Dispatch.RegisterHandler(TEXT("project.read_cpp_source"),  GT(&ProjectReadCppSourceImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
