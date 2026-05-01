#include "Tools/SageEditorAutomationTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "IPythonScriptPlugin.h"
#include "Kismet/GameplayStatics.h"
#include "LevelEditorViewport.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "HighResScreenshot.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"

#define LOCTEXT_NAMESPACE "SageEditorAuto"

namespace sage::tools
{
namespace
{

// Whitelist of console commands that are read-only or affect editor view
// state only. Anything else requires allow_unsafe=true.
bool IsConsoleCommandWhitelisted(const FString& Cmd)
{
    static const TArray<FString> kPrefixes = {
        TEXT("STAT "),       TEXT("STAT"),
        TEXT("SHOW "),       TEXT("SHOW"),
        TEXT("CAMERA "),
        TEXT("VIEWMODE "),   TEXT("VIEWMODE"),
        TEXT("R.SCREENPERCENTAGE"),
        TEXT("FREEZERENDERING"),
        TEXT("LISTLIGHTS"),
        TEXT("MEMREPORT"),
        TEXT("OBJ LIST"),    TEXT("OBJ"),
        TEXT("LOG "),        TEXT("LOG"),
        TEXT("HELP "),       TEXT("HELP"),
    };
    for (const FString& P : kPrefixes)
    {
        if (Cmd.StartsWith(P, ESearchCase::IgnoreCase)) return true;
    }
    return false;
}

// ---- editor.console_command ----------------------------------------------

FSageToolDispatch::FOutcome ConsoleCommandImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Cmd;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("cmd"), Cmd) || Cmd.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'cmd'"));
    }
    bool AllowUnsafe = false;
    Args->TryGetBoolField(TEXT("allow_unsafe"), AllowUnsafe);

    if (!AllowUnsafe && !IsConsoleCommandWhitelisted(Cmd))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("console command not whitelisted: '%s'. "
                "Set allow_unsafe=true to bypass (can crash the editor)."), *Cmd));
    }

    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    UWorld* World = GEditor->GetEditorWorldContext().World();
    GEngine->Exec(World, *Cmd);

    UE_LOG(LogSageBridge, Log, TEXT("Console exec: %s"), *Cmd);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("command"), Cmd);
    R->SetBoolField  (TEXT("ran"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.take_screenshot ----------------------------------------------

FSageToolDispatch::FOutcome TakeScreenshotImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), Path);

    if (Path.IsEmpty())
    {
        const FString TS = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
        Path = FPaths::ProjectSavedDir() / TEXT("Screenshots") /
               FString::Printf(TEXT("Sage_%s.png"), *TS);
    }

    FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
    Config.FilenameOverride = Path;
    Config.bDumpBufferVisualizationTargets = false;
    Config.bDateTimeBasedNaming = false;

    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));

    bool bRequested = false;
    if (FViewport* Viewport = GEditor->GetActiveViewport())
    {
        Viewport->TakeHighResScreenShot();
        bRequested = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),     Path);
    R->SetBoolField  (TEXT("requested"), bRequested);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.get_engine_version / editor.get_project_version --------------

FSageToolDispatch::FOutcome GetEngineVersionImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    R->SetStringField(TEXT("compatible_version"),
                      FEngineVersion::CompatibleWith().ToString());
    R->SetStringField(TEXT("build_configuration"),
                      LexToString(FApp::GetBuildConfiguration()));
    R->SetStringField(TEXT("project_dir"), FPaths::ProjectDir());
    R->SetStringField(TEXT("engine_dir"),  FPaths::EngineDir());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GetProjectVersionImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("project_name"),    FApp::GetProjectName());
    R->SetStringField(TEXT("project_dir"),     FPaths::ProjectDir());
    R->SetStringField(TEXT("project_log_dir"), FPaths::ProjectLogDir());
    R->SetStringField(TEXT("project_saved_dir"),  FPaths::ProjectSavedDir());
    R->SetStringField(TEXT("project_content_dir"), FPaths::ProjectContentDir());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.get_log_file_path --------------------------------------------

FSageToolDispatch::FOutcome GetLogFilePathImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), FPlatformOutputDevices::GetAbsoluteLogFilename());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.read_log -----------------------------------------------------

FSageToolDispatch::FOutcome ReadLogImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter;
    int32 MaxLines = 200;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("filter"), Filter);
        Args->TryGetNumberField(TEXT("max_lines"), MaxLines);
    }
    MaxLines = FMath::Clamp(MaxLines, 1, 5000);

    const FString AbsoluteLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
    FString Contents;
    if (!FFileHelper::LoadFileToString(Contents, *AbsoluteLogPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("could not read log file: %s"), *AbsoluteLogPath));
    }

    TArray<FString> Lines;
    Contents.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

    TArray<TSharedPtr<FJsonValue>> Out;
    const int32 Start = FMath::Max(0, Lines.Num() - MaxLines);
    for (int32 i = Start; i < Lines.Num(); ++i)
    {
        if (!Filter.IsEmpty() && !Lines[i].Contains(Filter)) continue;
        Out.Add(MakeShared<FJsonValueString>(Lines[i]));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("log_path"),  AbsoluteLogPath);
    R->SetArrayField (TEXT("lines"),     Out);
    R->SetNumberField(TEXT("count"),     Out.Num());
    R->SetNumberField(TEXT("total_lines"), Lines.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.run_python  (Phase 4.6-r3 batch 5) ---------------------------

FSageToolDispatch::FOutcome RunPythonImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Code;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("code"), Code) || Code.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/empty 'code'"));
    }

    IPythonScriptPlugin* Py = IPythonScriptPlugin::Get();
    if (!Py || !Py->IsPythonAvailable())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("Python scripting unavailable. Enable the PythonScriptPlugin "
                 "in the project's .uproject and restart the editor."));
    }

    FPythonCommandEx Cmd;
    Cmd.Command            = Code;
    Cmd.ExecutionMode      = EPythonCommandExecutionMode::ExecuteFile;
    Cmd.FileExecutionScope = EPythonFileExecutionScope::Public;

    const bool bOk = Py->ExecPythonCommandEx(Cmd);

    TArray<TSharedPtr<FJsonValue>> Logs;
    for (const FPythonLogOutputEntry& E : Cmd.LogOutput)
    {
        auto O = MakeShared<FJsonObject>();
        // EPythonLogOutputType: Info / Warning / Error
        O->SetStringField(TEXT("type"),   LexToString(E.Type));
        O->SetStringField(TEXT("output"), E.Output);
        Logs.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),     bOk);
    R->SetStringField(TEXT("result"),      Cmd.CommandResult);
    R->SetArrayField (TEXT("log_output"),  Logs);
    R->SetNumberField(TEXT("log_count"),   Logs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.build_all / build_geometry / build_lighting / build_hlod /
// ---- get_build_status  (Phase 4.6-r3 batch 4) ----------------------------

// Build commands fire-and-forget — the real status query is best-effort
// (only IsLightingBuildCurrentlyRunning / Exporting are exposed in 5.7).

FSageToolDispatch::FOutcome BuildAllImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("editor world unavailable"));
    GEngine->Exec(World, TEXT("MAP REBUILD"));
    GEngine->Exec(World, TEXT("BUILD LIGHTING"));
    GEngine->Exec(World, TEXT("RebuildNavigation"));
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("message"),
        TEXT("Build All triggered: MAP REBUILD + BUILD LIGHTING + RebuildNavigation"));
    R->SetStringField(TEXT("note"),
        TEXT("Async; poll editor.get_build_status for lighting progress"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BuildGeometryImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("editor world unavailable"));
    GEngine->Exec(World, TEXT("MAP REBUILD"));
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("message"), TEXT("Geometry (BSP) rebuild triggered: MAP REBUILD"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BuildLightingImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("editor world unavailable"));

    // Optional quality: Preview / Medium / High / Production. Default Preview.
    FString Quality = TEXT("Preview");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("quality"), Quality);

    const FString Cmd = FString::Printf(TEXT("BUILD LIGHTING %s"), *Quality);
    GEngine->Exec(World, *Cmd);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("message"), Cmd);
    R->SetStringField(TEXT("quality"), Quality);
    R->SetStringField(TEXT("note"),
        TEXT("Async; poll editor.get_build_status for progress"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BuildHlodImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("editor world unavailable"));
    GEngine->Exec(World, TEXT("BuildHLODs"));
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("message"), TEXT("HLOD build triggered: BuildHLODs"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GetBuildStatusImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto R = MakeShared<FJsonObject>();
    if (!GEditor)
    {
        R->SetStringField(TEXT("status"), TEXT("editor_unavailable"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    const bool bLightingRunning   = GEditor->IsLightingBuildCurrentlyRunning();
    const bool bLightingExporting = GEditor->IsLightingBuildCurrentlyExporting();

    FString Status;
    if (bLightingRunning)        Status = TEXT("lighting_running");
    else if (bLightingExporting) Status = TEXT("lighting_exporting");
    else                         Status = TEXT("idle");

    R->SetStringField(TEXT("status"),             Status);
    R->SetBoolField  (TEXT("lighting_running"),   bLightingRunning);
    R->SetBoolField  (TEXT("lighting_exporting"), bLightingExporting);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.search_log / list_crashes / check_for_crashes /
// ---- get_crash_info  (Phase 4.6-r3 batch 3) ------------------------------

FSageToolDispatch::FOutcome SearchLogImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Query;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/empty 'query'"));
    }
    int32 MaxLines = 100;
    double Num = 0;
    if (Args->TryGetNumberField(TEXT("max_lines"), Num))
    {
        MaxLines = FMath::Clamp(static_cast<int32>(Num), 1, 5000);
    }

    const FString AbsoluteLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
    FString Contents;
    if (!FFileHelper::LoadFileToString(Contents, *AbsoluteLogPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("could not read log: %s"), *AbsoluteLogPath));
    }
    TArray<FString> Lines;
    Contents.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

    TArray<TSharedPtr<FJsonValue>> Hits;
    for (int32 i = 0; i < Lines.Num(); ++i)
    {
        if (Hits.Num() >= MaxLines) break;
        if (!Lines[i].Contains(Query)) continue;
        auto O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("line"), i + 1);
        O->SetStringField(TEXT("text"), Lines[i].Left(500));
        Hits.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"),       Query);
    R->SetStringField(TEXT("log_path"),    AbsoluteLogPath);
    R->SetArrayField (TEXT("hits"),        Hits);
    R->SetNumberField(TEXT("count"),       Hits.Num());
    R->SetNumberField(TEXT("total_lines"), Lines.Num());
    R->SetBoolField  (TEXT("capped"),      Hits.Num() >= MaxLines);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// macOS: ~/Library/Application Support/Epic/UnrealEngine/<Version>/Saved/Crashes/
// Win:   %LOCALAPPDATA%/UnrealEngine/<Version>/Saved/Crashes/
// Linux: ~/Unreal Projects/.../Saved/Crashes/  (best-effort)
//
// We scan the per-user UE crash root because that's where the editor
// drops crash dumps (verified on Mac; structure mirrors UE-MCP's expectations).
FString GetUserCrashesRoot()
{
    // FPlatformProcess::UserSettingsDir() on Mac:
    //   ~/Library/Application Support/Epic/   (already includes /Epic)
    // On Win:
    //   %LOCALAPPDATA%/   (no /Epic suffix)
    // The crash root is always <UserRoot>/UnrealEngine/<MAJ.MIN>/Saved/Crashes
    // — the editor doesn't add a project-name dir under there.
    const FString UserRoot = FPlatformProcess::UserSettingsDir();
    const FString EngineVer = FString::Printf(TEXT("%d.%d"),
        FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor());
    return UserRoot / TEXT("UnrealEngine") / EngineVer
                    / TEXT("Saved") / TEXT("Crashes");
}

TArray<FString> ListCrashDirs(IFileManager& FM, const FString& Root)
{
    TArray<FString> Dirs;
    if (!FM.DirectoryExists(*Root)) return Dirs;
    TArray<FString> SubDirs;
    FM.FindFiles(SubDirs, *(Root / TEXT("CrashReport*")), false, true);
    Dirs.Reserve(SubDirs.Num());
    for (const FString& D : SubDirs)
    {
        Dirs.Add(Root / D);
    }
    return Dirs;
}

FSageToolDispatch::FOutcome ListCrashesImpl(const TSharedPtr<FJsonObject>& Args)
{
    int32 MaxResults = 25;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 200);
        }
    }
    const FString Root = GetUserCrashesRoot();
    IFileManager& FM = IFileManager::Get();
    TArray<FString> Dirs = ListCrashDirs(FM, Root);

    // Sort by mtime descending (newest first)
    Dirs.Sort([&FM](const FString& A, const FString& B) {
        return FM.GetTimeStamp(*A) > FM.GetTimeStamp(*B);
    });

    TArray<TSharedPtr<FJsonValue>> Out;
    for (int32 i = 0; i < FMath::Min(Dirs.Num(), MaxResults); ++i)
    {
        const FString& D = Dirs[i];
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("crash_dir"), FPaths::ConvertRelativePathToFull(D));
        O->SetStringField(TEXT("name"),      FPaths::GetCleanFilename(D));
        const FDateTime TS = FM.GetTimeStamp(*D);
        O->SetStringField(TEXT("timestamp"), TS.ToIso8601());
        // Quick file presence flags (each crash dir always has these in modern UE).
        O->SetBoolField(TEXT("has_log"),     FM.FileExists(*(D / TEXT("SageTest.log"))) ||
                                              FM.FileExists(*(D / TEXT("UnrealEditor.log"))));
        O->SetBoolField(TEXT("has_dump"),    FM.FileExists(*(D / TEXT("minidump.dmp"))));
        O->SetBoolField(TEXT("has_context"), FM.FileExists(*(D / TEXT("CrashContext.runtime-xml"))));
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("crashes_root"),  FPaths::ConvertRelativePathToFull(Root));
    R->SetArrayField (TEXT("crashes"),       Out);
    R->SetNumberField(TEXT("count"),         Out.Num());
    R->SetNumberField(TEXT("total_on_disk"), Dirs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CheckForCrashesImpl(const TSharedPtr<FJsonObject>& Args)
{
    int32 WithinHours = 24;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("within_hours"), N))
        {
            WithinHours = FMath::Clamp(static_cast<int32>(N), 1, 24 * 365);
        }
    }
    const FString Root = GetUserCrashesRoot();
    IFileManager& FM = IFileManager::Get();
    TArray<FString> Dirs = ListCrashDirs(FM, Root);

    const FDateTime Now = FDateTime::UtcNow();
    int32 Recent = 0;
    FDateTime Latest{};
    for (const FString& D : Dirs)
    {
        const FDateTime TS = FM.GetTimeStamp(*D);
        if (TS == FDateTime::MinValue()) continue;
        const FTimespan Age = Now - TS;
        if (FMath::Abs(Age.GetTotalHours()) <= WithinHours)
        {
            ++Recent;
            if (TS > Latest) Latest = TS;
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("within_hours"), WithinHours);
    R->SetNumberField(TEXT("recent_count"), Recent);
    R->SetBoolField  (TEXT("has_recent"),   Recent > 0);
    if (Latest != FDateTime::MinValue())
    {
        R->SetStringField(TEXT("latest_timestamp"), Latest.ToIso8601());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GetCrashInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString CrashDir;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("crash_dir"), CrashDir))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'crash_dir'"));
    }
    // Safety: the crash_dir must live under the per-user crashes root.
    const FString Root = FPaths::ConvertRelativePathToFull(GetUserCrashesRoot());
    const FString Abs  = FPaths::ConvertRelativePathToFull(CrashDir);
    if (!Abs.StartsWith(Root))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("crash_dir outside crashes root: %s"), *Abs));
    }
    IFileManager& FM = IFileManager::Get();
    if (!FM.DirectoryExists(*Abs))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("crash_dir not found: %s"), *Abs));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("crash_dir"), Abs);
    R->SetStringField(TEXT("name"),      FPaths::GetCleanFilename(Abs));
    R->SetStringField(TEXT("timestamp"), FM.GetTimeStamp(*Abs).ToIso8601());

    // Try to read CrashContext.runtime-xml's <ErrorMessage> + <CallStack>.
    const FString XmlPath = Abs / TEXT("CrashContext.runtime-xml");
    FString Xml;
    if (FFileHelper::LoadFileToString(Xml, *XmlPath))
    {
        auto Extract = [&Xml](const FString& Tag) -> FString
        {
            const FString Open  = FString::Printf(TEXT("<%s>"),  *Tag);
            const FString Close = FString::Printf(TEXT("</%s>"), *Tag);
            const int32 S = Xml.Find(Open);
            if (S == INDEX_NONE) return FString();
            const int32 SE = S + Open.Len();
            const int32 E = Xml.Find(Close, ESearchCase::IgnoreCase, ESearchDir::FromStart, SE);
            if (E == INDEX_NONE) return FString();
            return Xml.Mid(SE, E - SE).TrimStartAndEnd();
        };
        const FString ErrMsg   = Extract(TEXT("ErrorMessage"));
        const FString Stack    = Extract(TEXT("CallStack"));
        if (!ErrMsg.IsEmpty()) R->SetStringField(TEXT("error_message"), ErrMsg.Left(2000));
        if (!Stack.IsEmpty())  R->SetStringField(TEXT("call_stack"),    Stack.Left(8192));
    }

    // Tail of the crash log (last 50 lines).
    TArray<FString> Candidates = { TEXT("UnrealEditor.log"), TEXT("SageTest.log") };
    for (const FString& Cand : Candidates)
    {
        const FString LP = Abs / Cand;
        FString LogTxt;
        if (!FFileHelper::LoadFileToString(LogTxt, *LP)) continue;
        TArray<FString> Lines;
        LogTxt.ParseIntoArrayLines(Lines, false);
        const int32 Tail = FMath::Max(0, Lines.Num() - 50);
        TArray<TSharedPtr<FJsonValue>> Out;
        for (int32 i = Tail; i < Lines.Num(); ++i)
        {
            Out.Add(MakeShared<FJsonValueString>(Lines[i].Left(500)));
        }
        R->SetStringField(TEXT("log_path"),  LP);
        R->SetArrayField (TEXT("log_tail"),  Out);
        R->SetNumberField(TEXT("log_total_lines"), Lines.Num());
        break;
    }

    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.set_property / editor.set_pie_time_scale  (Phase 4.6-r3 b2) --

FSageToolDispatch::FOutcome SetPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, PropName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("property"), PropName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'property'"));
    }
    TSharedPtr<FJsonValue> ValueJson = Args->TryGetField(TEXT("value"));
    if (!ValueJson.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (sage::tools::detail::RejectIfPie(PieErr)) return PieErr;

    // Resolve UObject. Accept asset paths and engine class paths.
    UObject* Obj = nullptr;
    {
        FSoftObjectPath Soft(Path);
        Obj = Soft.ResolveObject();
        if (!Obj) Obj = Soft.TryLoad();
    }
    if (!Obj)
    {
        Obj = LoadObject<UObject>(nullptr, *Path);
    }
    if (!Obj)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("object not found: %s"), *Path));
    }

    FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*PropName));
    if (!Prop)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("property not found: %s on %s"),
                            *PropName, *Obj->GetClass()->GetName()));
    }

    Obj->Modify();
    const bool bOk = sage::tools::detail::SetUPropertyFromJson(Obj, Prop, ValueJson);
    if (!bOk)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("could not coerce JSON value into property %s"), *PropName));
    }
    Obj->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),     Path);
    R->SetStringField(TEXT("property"), PropName);
    R->SetStringField(TEXT("class"),    Obj->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetPieTimeScaleImpl(const TSharedPtr<FJsonObject>& Args)
{
    double Factor = 1.0;
    if (!Args.IsValid() || !Args->TryGetNumberField(TEXT("factor"), Factor))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'factor'"));
    }
    if (Factor <= 0.0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("'factor' must be > 0"));
    }
    if (!GEditor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UWorld* World = GEditor->PlayWorld;
    if (!World)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004,
            TEXT("no PIE world active — start PIE first (run_pie)"));
    }
    AWorldSettings* WS = World->GetWorldSettings();
    if (!WS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("WorldSettings unavailable on PIE world"));
    }

    // Lift dilation caps so 'Factor' isn't clamped silently.
    const float Cap = FMath::Max(1000.0f, static_cast<float>(Factor) * 2.0f);
    WS->MaxGlobalTimeDilation = FMath::Max(WS->MaxGlobalTimeDilation, Cap);
    WS->MinGlobalTimeDilation = FMath::Min(WS->MinGlobalTimeDilation, 0.0001f);
    UGameplayStatics::SetGlobalTimeDilation(World, static_cast<float>(Factor));

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("factor"),  Factor);
    R->SetNumberField(TEXT("max_cap"), WS->MaxGlobalTimeDilation);
    R->SetNumberField(TEXT("min_cap"), WS->MinGlobalTimeDilation);
    R->SetStringField(TEXT("world"),   World->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.undo / editor.redo  (Phase 4.6-r3) ---------------------------

FSageToolDispatch::FOutcome UndoImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    const bool bDid = GEditor->UndoTransaction(/*bCanRedo*/ true);
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("undid"), bDid);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RedoImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    const bool bDid = GEditor->RedoTransaction();
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("redid"), bDid);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.focus_on_actor / editor.set_viewport  (Phase 4.6-r3) --------

FLevelEditorViewportClient* GetActiveLevelViewportClient()
{
    if (!GEditor) return nullptr;
    if (FLevelEditorViewportClient* C = GCurrentLevelEditingViewportClient) return C;
    const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
    return Clients.Num() > 0 ? Clients[0] : nullptr;
}

FSageToolDispatch::FOutcome FocusOnActorImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    }
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));

    AActor* A = sage::tools::detail::ResolveActor(ActorPath);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorPath));

    bool bActiveOnly = false;
    Args->TryGetBoolField(TEXT("active_viewport_only"), bActiveOnly);

    GEditor->MoveViewportCamerasToActor(*A, bActiveOnly);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"), ActorPath);
    R->SetBoolField  (TEXT("active_viewport_only"), bActiveOnly);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetViewportImpl(const TSharedPtr<FJsonObject>& Args)
{
    FLevelEditorViewportClient* VPC = GetActiveLevelViewportClient();
    if (!VPC) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("no level viewport available"));

    bool bSetLoc = false, bSetRot = false;
    FVector  Loc{};
    FRotator Rot{};
    if (Args.IsValid())
    {
        if (sage::tools::detail::ParseVector3(Args, TEXT("location"), Loc))
        {
            VPC->SetViewLocation(Loc);
            bSetLoc = true;
        }
        if (sage::tools::detail::ParseRotator3(Args, TEXT("rotation"), Rot))
        {
            VPC->SetViewRotation(Rot);
            bSetRot = true;
        }
    }
    if (!bSetLoc && !bSetRot)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("provide 'location' [x,y,z] and/or 'rotation' [pitch,yaw,roll]"));
    }
    VPC->Invalidate();

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("location_set"), bSetLoc);
    R->SetBoolField(TEXT("rotation_set"), bSetRot);
    if (bSetLoc) R->SetField(TEXT("location"), sage::tools::detail::Vec3ToJson(Loc));
    if (bSetRot) R->SetField(TEXT("rotation"), sage::tools::detail::Rot3ToJson(Rot));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.hot_reload -----------------------------------------------------

FSageToolDispatch::FOutcome HotReloadImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    // Trigger Live Coding / hot reload via console command
    GEditor->Exec(GEditor->GetEditorWorldContext().World(),
        TEXT("LiveCoding.Compile"), *GLog);
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("triggered"), true);
    R->SetStringField(TEXT("note"), TEXT("hot reload triggered via LiveCoding.Compile"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.get_perf_stats -------------------------------------------------

FSageToolDispatch::FOutcome GetPerfStatsImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("fps"),              1.0f / FApp::GetDeltaTime());
    R->SetNumberField(TEXT("frame_time_ms"),    FApp::GetDeltaTime() * 1000.0);
    R->SetNumberField(TEXT("real_time"),        FApp::GetCurrentTime());
    R->SetBoolField  (TEXT("is_pie"),           GEditor && GEditor->IsPlayingSessionInEditor());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.set_scalability ------------------------------------------------

FSageToolDispatch::FOutcome SetScalabilityImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString GroupStr;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("group"), GroupStr))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'group' (e.g. 'sg.ResolutionQuality')"));

    double Level;
    if (!Args->TryGetNumberField(TEXT("level"), Level))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'level' (0-3 or 0-100)"));

    FString Cmd = FString::Printf(TEXT("%s %g"), *GroupStr, Level);
    GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Cmd, *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("group"),   GroupStr);
    R->SetNumberField(TEXT("level"),   Level);
    R->SetBoolField  (TEXT("applied"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.capture_scene_png ----------------------------------------------

FSageToolDispatch::FOutcome CaptureScenePngImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString OutputPath = FPaths::ProjectSavedDir() / TEXT("Screenshots") / TEXT("scene_capture.png");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("output_path"), OutputPath);

    double SizeD = 512;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("size"), SizeD);
    int32 Size = FMath::Clamp((int32)SizeD, 32, 4096);

    // Use high-res screenshot system
    FHighResScreenshotConfig& HRSS = GetHighResScreenshotConfig();
    HRSS.FilenameOverride = OutputPath;
    GEditor->Exec(GEditor->GetEditorWorldContext().World(),
        *FString::Printf(TEXT("HighResShot %d"), Size), *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("output_path"), OutputPath);
    R->SetNumberField(TEXT("size"),        Size);
    R->SetBoolField  (TEXT("triggered"),   true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.play_sequence --------------------------------------------------

FSageToolDispatch::FOutcome PlaySequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    // Trigger via console command - USequencer not easily accessible headlessly
    FString Cmd = FString::Printf(TEXT("Sequencer.PlaySequence %s"), *Path);
    GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Cmd, *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),    Path);
    R->SetBoolField  (TEXT("playing"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.validate_assets ------------------------------------------------

FSageToolDispatch::FOutcome ValidateAssetsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Directory = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("directory"), Directory);

    // Run via editor subsystem
    if (UEditorAssetSubsystem* Sub =
        GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
    {
        // DoesAssetExist validates path resolution
        bool bResult = Sub->DoesDirectoryExist(Directory);
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("directory"),       Directory);
        R->SetBoolField  (TEXT("directory_exists"), bResult);
        R->SetStringField(TEXT("note"), TEXT("use editor.console_command with 'AssetCheck' for full validation"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("UEditorAssetSubsystem not available"));
}

// ---- editor.cook_content ---------------------------------------------------

FSageToolDispatch::FOutcome CookContentImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Platform = TEXT("WindowsNoEditor");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("platform"), Platform);

    FString Cmd = FString::Printf(TEXT("cook -TargetPlatform=%s"), *Platform);
    // Cook is typically triggered via UAT — surface the command
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("platform"),  Platform);
    R->SetBoolField  (TEXT("triggered"), false);
    R->SetStringField(TEXT("note"),
        TEXT("use UAT: RunUAT BuildCookRun -cook -TargetPlatform=<platform>"));
    R->SetStringField(TEXT("uat_command"), Cmd);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.get_message_log ------------------------------------------------

FSageToolDispatch::FOutcome GetMessageLogImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Category = TEXT("AssetCheck");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("category"), Category);

    // Message log is Slate-based; we surface recent log lines from the output log
    // that match the category prefix as a proxy.
    FString AbsoluteLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
    TArray<FString> Lines;
    FFileHelper::LoadFileToStringArray(Lines, *AbsoluteLogPath);

    TArray<TSharedPtr<FJsonValue>> Messages;
    for (int32 I = Lines.Num() - 1; I >= 0 && Messages.Num() < 50; --I)
    {
        if (Lines[I].Contains(Category))
            Messages.Insert(MakeShared<FJsonValueString>(Lines[I]), 0);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("category"), Category);
    R->SetArrayField (TEXT("messages"), Messages);
    R->SetNumberField(TEXT("count"),    Messages.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.open_asset -----------------------------------------------------

FSageToolDispatch::FOutcome OpenAssetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Asset = Soft.ResolveObject();
    if (!Asset) Asset = Soft.TryLoad();
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    if (GEditor)
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),   Asset->GetPathName());
    R->SetStringField(TEXT("class"),  Asset->GetClass()->GetName());
    R->SetBoolField  (TEXT("opened"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterEditorAutomationTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("editor.console_command"),    GT(&ConsoleCommandImpl));
    Dispatch.RegisterHandler(TEXT("editor.take_screenshot"),    GT(&TakeScreenshotImpl));
    Dispatch.RegisterHandler(TEXT("editor.get_engine_version"), GT(&GetEngineVersionImpl));
    Dispatch.RegisterHandler(TEXT("editor.get_project_version"),GT(&GetProjectVersionImpl));
    Dispatch.RegisterHandler(TEXT("editor.get_log_file_path"),  GT(&GetLogFilePathImpl));
    Dispatch.RegisterHandler(TEXT("editor.read_log"),           GT(&ReadLogImpl));

    // Phase 4.6-r3 batch 1: editor state control
    Dispatch.RegisterHandler(TEXT("editor.undo"),               GT(&UndoImpl));
    Dispatch.RegisterHandler(TEXT("editor.redo"),               GT(&RedoImpl));
    Dispatch.RegisterHandler(TEXT("editor.focus_on_actor"),     GT(&FocusOnActorImpl));
    Dispatch.RegisterHandler(TEXT("editor.set_viewport"),       GT(&SetViewportImpl));

    // Phase 4.6-r3 batch 2: runtime state mutation
    Dispatch.RegisterHandler(TEXT("editor.set_property"),       GT(&SetPropertyImpl));
    Dispatch.RegisterHandler(TEXT("editor.set_pie_time_scale"), GT(&SetPieTimeScaleImpl));

    // Phase 4.6-r3 batch 3: log + crash forensics
    Dispatch.RegisterHandler(TEXT("editor.search_log"),         GT(&SearchLogImpl));
    Dispatch.RegisterHandler(TEXT("editor.list_crashes"),       GT(&ListCrashesImpl));
    Dispatch.RegisterHandler(TEXT("editor.check_for_crashes"),  GT(&CheckForCrashesImpl));
    Dispatch.RegisterHandler(TEXT("editor.get_crash_info"),     GT(&GetCrashInfoImpl));

    // Phase 4.6-r3 batch 4: level building
    Dispatch.RegisterHandler(TEXT("editor.build_all"),          GT(&BuildAllImpl));
    Dispatch.RegisterHandler(TEXT("editor.build_geometry"),     GT(&BuildGeometryImpl));
    Dispatch.RegisterHandler(TEXT("editor.build_lighting"),     GT(&BuildLightingImpl));
    Dispatch.RegisterHandler(TEXT("editor.build_hlod"),         GT(&BuildHlodImpl));
    Dispatch.RegisterHandler(TEXT("editor.get_build_status"),   GT(&GetBuildStatusImpl));

    // Phase 4.6-r3 batch 5: Python scripting
    Dispatch.RegisterHandler(TEXT("editor.run_python"),         GT(&RunPythonImpl));

    // Phase 4.6 remaining
    Dispatch.RegisterHandler(TEXT("editor.hot_reload"),         GT(&HotReloadImpl));
    Dispatch.RegisterHandler(TEXT("editor.get_perf_stats"),     GT(&GetPerfStatsImpl));
    Dispatch.RegisterHandler(TEXT("editor.set_scalability"),    GT(&SetScalabilityImpl));
    Dispatch.RegisterHandler(TEXT("editor.capture_scene_png"),  GT(&CaptureScenePngImpl));
    Dispatch.RegisterHandler(TEXT("editor.play_sequence"),      GT(&PlaySequenceImpl));
    Dispatch.RegisterHandler(TEXT("editor.validate_assets"),    GT(&ValidateAssetsImpl));
    Dispatch.RegisterHandler(TEXT("editor.cook_content"),       GT(&CookContentImpl));
    Dispatch.RegisterHandler(TEXT("editor.get_message_log"),    GT(&GetMessageLogImpl));
    Dispatch.RegisterHandler(TEXT("editor.open_asset"),         GT(&OpenAssetImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
