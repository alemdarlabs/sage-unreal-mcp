#include "Tools/SageEditorAutomationTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Blueprint.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "GameplayTagContainer.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "IPythonScriptPlugin.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"
#include "LevelEditorViewport.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "HAL/PlatformFile.h"
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
#include "Settings/LevelEditorPlaySettings.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UnrealType.h"
#include "UnrealClient.h"

#define LOCTEXT_NAMESPACE "SageEditorAuto"

namespace sage::tools
{
namespace
{

void AddPythonCleanupReport(TSharedRef<FJsonObject> Result,
                            const detail::FPythonReferenceCleanupReport& Report)
{
    auto Cleanup = MakeShared<FJsonObject>();
    Cleanup->SetBoolField(TEXT("python_available"), Report.bPythonAvailable);
    Cleanup->SetBoolField(TEXT("python_command_ran"), Report.bPythonCommandRan);
    Cleanup->SetBoolField(TEXT("python_command_succeeded"), Report.bPythonCommandSucceeded);
    Cleanup->SetBoolField(TEXT("cleared_main_globals"), Report.bClearedMainGlobals);
    Cleanup->SetBoolField(TEXT("collected_unreal_garbage"), Report.bCollectedUnrealGarbage);
    Cleanup->SetNumberField(TEXT("cleansed_root_count"), Report.CleansedRootCount);
    if (!Report.Error.IsEmpty())
    {
        Cleanup->SetStringField(TEXT("error"), Report.Error);
    }
    TArray<TSharedPtr<FJsonValue>> Roots;
    for (const FString& Root : Report.CleansedRoots)
    {
        Roots.Add(MakeShared<FJsonValueString>(Root));
    }
    Cleanup->SetArrayField(TEXT("cleansed_roots"), Roots);
    Result->SetObjectField(TEXT("python_reference_cleanup"), Cleanup);
}

// Whitelist of console commands that are read-only or affect editor view
// state only. Anything else requires allow_unsafe=true.
//
// Matching is exact-or-prefix-with-space — this prevents bypasses such as
// `STATQUIT` matching `STAT`, or `OBJ DELETE` slipping past `OBJ`.
bool IsAllowedConsoleCommand(const FString& Cmd, const TArray<FString>& Whitelist)
{
    const FString Trimmed = Cmd.TrimStartAndEnd();
    for (const FString& W : Whitelist)
    {
        if (Trimmed.Equals(W, ESearchCase::IgnoreCase)) return true;
        if (Trimmed.StartsWith(W + TEXT(" "), ESearchCase::IgnoreCase)) return true;
    }
    return false;
}

// Reject obviously dangerous IO-redirecting console forms even if their head
// token is on the whitelist (e.g. `LOG OutputLog FILE=...`, redirection,
// piping, or arbitrary `EXEC <script>`).
bool IsConsoleCommandIODirected(const FString& Cmd)
{
    const FString Upper = Cmd.ToUpper();
    if (Upper.Contains(TEXT(" FILE=")))    return true;
    if (Upper.Contains(TEXT(" -FILE=")))   return true;
    if (Upper.Contains(TEXT("OUTPUTLOG"))) return true;  // LOG OutputLog FILE=...
    if (Upper.Contains(TEXT(" > ")))       return true;
    if (Upper.Contains(TEXT(" >> ")))      return true;
    if (Upper.Contains(TEXT(" | ")))       return true;
    if (Upper.StartsWith(TEXT("EXEC ")))   return true;  // run arbitrary script
    return false;
}

bool IsConsoleCommandWhitelisted(const FString& Cmd)
{
    static const TArray<FString> kWhitelist = {
        TEXT("STAT"),
        TEXT("SHOW"),
        TEXT("CAMERA"),
        TEXT("VIEWMODE"),
        TEXT("R.SCREENPERCENTAGE"),
        TEXT("FREEZERENDERING"),
        TEXT("LISTLIGHTS"),
        TEXT("MEMREPORT"),
        TEXT("OBJ"),
        TEXT("LOG"),
        TEXT("HELP"),
    };
    if (IsConsoleCommandIODirected(Cmd)) return false;
    return IsAllowedConsoleCommand(Cmd, kWhitelist);
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

    // IO-redirection (FILE=, OutputLog dump, pipes, EXEC script) is rejected
    // even when allow_unsafe=true unless the caller is explicit — these forms
    // can write arbitrary files outside the project.
    if (!AllowUnsafe && IsConsoleCommandIODirected(Cmd))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("console command rejected (IO redirection / file output): '%s'. "
                "Set allow_unsafe=true if intentional."), *Cmd));
    }
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

    // Sandbox: resolve to absolute, collapse .., and require the path to live
    // under <Project>/Saved/Screenshots. Reject otherwise — a tool that
    // accepts arbitrary paths could overwrite project files or leak data.
    {
        FString AbsPath = FPaths::ConvertRelativePathToFull(Path);
        FPaths::CollapseRelativeDirectories(AbsPath);
        const FString AllowedRoot = FPaths::ConvertRelativePathToFull(
            FPaths::ProjectSavedDir() / TEXT("Screenshots"));

#if PLATFORM_WINDOWS
        const ESearchCase::Type kCase = ESearchCase::IgnoreCase;
#else
        const ESearchCase::Type kCase = ESearchCase::CaseSensitive;
#endif
        if (!AbsPath.StartsWith(AllowedRoot, kCase))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("screenshot path must be under '%s' (got '%s')"),
                                *AllowedRoot, *AbsPath));
        }
        Path = AbsPath;
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

struct FLogReadResult
{
    FString RequestedPath;
    FString NormalizedPath;
    FString Contents;
    FString Method;
    FString Error;
    int64 FileSize = -1;
    int64 BytesRead = 0;
    bool bExists = false;
    bool bLoadFileToStringOk = false;
    bool bSharedReadAttempted = false;
    bool bSharedReadOk = false;
    bool bSharedOpenOk = false;
    bool bTruncatedFromStart = false;
};

FString ResolveLogReadPath(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), Path);
        Args->TryGetStringField(TEXT("log_path"), Path);
    }
    if (Path.IsEmpty())
    {
        Path = FPlatformOutputDevices::GetAbsoluteLogFilename();
    }
    Path = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Path);
    FPaths::CollapseRelativeDirectories(Path);
    return Path;
}

TSharedRef<FJsonObject> LogReadDiagnosticsToJson(const FLogReadResult& Read)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("requested_path"), Read.RequestedPath);
    O->SetStringField(TEXT("normalized_path"), Read.NormalizedPath);
    O->SetBoolField(TEXT("exists"), Read.bExists);
    O->SetNumberField(TEXT("file_size"), static_cast<double>(Read.FileSize));
    O->SetNumberField(TEXT("bytes_read"), static_cast<double>(Read.BytesRead));
    O->SetBoolField(TEXT("load_file_to_string_ok"), Read.bLoadFileToStringOk);
    O->SetBoolField(TEXT("shared_read_attempted"), Read.bSharedReadAttempted);
    O->SetBoolField(TEXT("shared_open_ok"), Read.bSharedOpenOk);
    O->SetBoolField(TEXT("shared_read_ok"), Read.bSharedReadOk);
    O->SetBoolField(TEXT("truncated_from_start"), Read.bTruncatedFromStart);
    O->SetStringField(TEXT("method"), Read.Method);
    if (!Read.Error.IsEmpty())
    {
        O->SetStringField(TEXT("error"), Read.Error);
    }
    return O;
}

bool ReadTextFileWithSharedFallback(const FString& Path,
                                    int64 MaxBytes,
                                    FLogReadResult& Out)
{
    Out.RequestedPath = Path;
    Out.NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Out.NormalizedPath);
    FPaths::CollapseRelativeDirectories(Out.NormalizedPath);
    Out.Method = TEXT("none");

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    Out.bExists = PlatformFile.FileExists(*Out.NormalizedPath);
    Out.FileSize = Out.bExists ? PlatformFile.FileSize(*Out.NormalizedPath) : -1;

    if (FFileHelper::LoadFileToString(Out.Contents, *Out.NormalizedPath))
    {
        Out.bLoadFileToStringOk = true;
        Out.Method = TEXT("FFileHelper::LoadFileToString");
        Out.BytesRead = Out.FileSize >= 0
            ? Out.FileSize
            : static_cast<int64>(Out.Contents.Len() * sizeof(TCHAR));
        return true;
    }

    Out.bSharedReadAttempted = true;
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(
        *Out.NormalizedPath,
        /*bAllowWrite=*/true));
    Out.bSharedOpenOk = Handle.IsValid();
    if (!Handle)
    {
        Out.Error = FString::Printf(TEXT("OpenRead(bAllowWrite=true) failed; exists=%s size=%lld"),
            Out.bExists ? TEXT("true") : TEXT("false"),
            static_cast<long long>(Out.FileSize));
        return false;
    }

    int64 Size = Handle->Size();
    if (Size < 0)
    {
        Size = Out.FileSize;
    }
    if (Size < 0)
    {
        Out.Error = TEXT("could not determine file size for shared read");
        return false;
    }

    const int64 ClampedMaxBytes = FMath::Clamp<int64>(MaxBytes, 1, 256ll * 1024ll * 1024ll);
    int64 Offset = 0;
    int64 BytesToRead = Size;
    if (BytesToRead > ClampedMaxBytes)
    {
        Offset = BytesToRead - ClampedMaxBytes;
        BytesToRead = ClampedMaxBytes;
        Out.bTruncatedFromStart = true;
        if (!Handle->Seek(Offset))
        {
            Out.Error = FString::Printf(TEXT("shared read seek failed at offset %lld"),
                static_cast<long long>(Offset));
            return false;
        }
    }
    if (BytesToRead > MAX_int32)
    {
        Out.Error = FString::Printf(TEXT("shared read size too large: %lld bytes"),
            static_cast<long long>(BytesToRead));
        return false;
    }

    TArray<uint8> Data;
    Data.SetNumUninitialized(static_cast<int32>(BytesToRead));
    if (BytesToRead > 0 && !Handle->Read(Data.GetData(), BytesToRead))
    {
        Out.Error = FString::Printf(TEXT("shared read failed after open; bytes=%lld"),
            static_cast<long long>(BytesToRead));
        return false;
    }

    FFileHelper::BufferToString(Out.Contents, Data.GetData(), Data.Num());
    Out.Method = TEXT("IPlatformFile::OpenRead(bAllowWrite=true)");
    Out.BytesRead = BytesToRead;
    Out.bSharedReadOk = true;
    return true;
}

FString LogReadErrorMessage(const FLogReadResult& Read)
{
    return FString::Printf(
        TEXT("could not read log file: %s (normalized=%s exists=%s size=%lld load_file_to_string_ok=%s shared_read_attempted=%s shared_open_ok=%s shared_read_ok=%s error=%s)"),
        *Read.RequestedPath,
        *Read.NormalizedPath,
        Read.bExists ? TEXT("true") : TEXT("false"),
        static_cast<long long>(Read.FileSize),
        Read.bLoadFileToStringOk ? TEXT("true") : TEXT("false"),
        Read.bSharedReadAttempted ? TEXT("true") : TEXT("false"),
        Read.bSharedOpenOk ? TEXT("true") : TEXT("false"),
        Read.bSharedReadOk ? TEXT("true") : TEXT("false"),
        *Read.Error);
}

// ---- editor.read_log -----------------------------------------------------

FSageToolDispatch::FOutcome ReadLogImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter;
    int32 MaxLines = 200;
    bool bCaseSensitive = false;
    double MaxBytesNum = 64.0 * 1024.0 * 1024.0;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("filter"), Filter);
        Args->TryGetNumberField(TEXT("max_lines"), MaxLines);
        Args->TryGetBoolField(TEXT("case_sensitive"), bCaseSensitive);
        Args->TryGetNumberField(TEXT("max_bytes"), MaxBytesNum);
    }
    MaxLines = FMath::Clamp(MaxLines, 1, 5000);

    const FString AbsoluteLogPath = ResolveLogReadPath(Args);
    FLogReadResult Read;
    if (!ReadTextFileWithSharedFallback(AbsoluteLogPath, static_cast<int64>(MaxBytesNum), Read))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            LogReadErrorMessage(Read));
    }

    TArray<FString> Lines;
    Read.Contents.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

    TArray<TSharedPtr<FJsonValue>> Out;
    const int32 Start = FMath::Max(0, Lines.Num() - MaxLines);
    const ESearchCase::Type SearchCase = bCaseSensitive
        ? ESearchCase::CaseSensitive
        : ESearchCase::IgnoreCase;
    for (int32 i = Start; i < Lines.Num(); ++i)
    {
        if (!Filter.IsEmpty() && !Lines[i].Contains(Filter, SearchCase)) continue;
        Out.Add(MakeShared<FJsonValueString>(Lines[i]));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("log_path"),  Read.NormalizedPath);
    R->SetArrayField (TEXT("lines"),     Out);
    R->SetNumberField(TEXT("count"),     Out.Num());
    R->SetNumberField(TEXT("total_lines"), Lines.Num());
    R->SetBoolField  (TEXT("case_sensitive"), bCaseSensitive);
    R->SetObjectField(TEXT("read_diagnostics"), LogReadDiagnosticsToJson(Read));
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

    bool bAllowUnsafeAssetMutation = false;
    Args->TryGetBoolField(TEXT("allow_unsafe_asset_mutation"), bAllowUnsafeAssetMutation);
    const FString LowerCode = Code.ToLower();
    const bool bLooksLikeMappingArrayMutation =
        LowerCode.Contains(TEXT(".mappings"))
        || LowerCode.Contains(TEXT("set_editor_property('mappings'"))
        || LowerCode.Contains(TEXT("set_editor_property(\"mappings\""))
        || LowerCode.Contains(TEXT("get_editor_property('mappings'"))
        || LowerCode.Contains(TEXT("get_editor_property(\"mappings\""));
    const bool bMentionsEnhancedInput =
        LowerCode.Contains(TEXT("inputmappingcontext"))
        || LowerCode.Contains(TEXT("input_mapping_context"))
        || LowerCode.Contains(TEXT("defaultkeymappings"))
        || LowerCode.Contains(TEXT("default_key_mappings"))
        || LowerCode.Contains(TEXT("enhancedinput"))
        || LowerCode.Contains(TEXT("enhanced_input"));
    const bool bLooksLikeImcMappingArrayMutation =
        bLooksLikeMappingArrayMutation || (bMentionsEnhancedInput && LowerCode.Contains(TEXT("mappings")));
    if (bLooksLikeImcMappingArrayMutation && !bAllowUnsafeAssetMutation)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("unsafe Python IMC Mappings mutation rejected; use gameplay.set_imc_mapping_key/action/remove_mapping or pass allow_unsafe_asset_mutation:true"));
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
    // Phase 5+ auth-gate territory: this tool grants arbitrary Python with
    // full UE editor access (filesystem, process spawn, asset write). Sage's
    // server-side auth layer will gate the handler before public release;
    // for now we surface a self-describing warning on every response.
    R->SetStringField(TEXT("_security_warning"),
        TEXT("executes arbitrary Python with full UE access; restrict before public release"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- editor.build_all / build_geometry / build_lighting / build_hlod /
// ---- get_build_status  (Phase 4.6-r3 batch 4) ----------------------------

// Build commands fire-and-forget — the real status query is best-effort
// (only IsLightingBuildCurrentlyRunning / Exporting are exposed in 5.7).

// Build operations are long-running (lighting can take hours on large levels)
// and consume the editor — we require explicit confirmation per the
// production project disciple in CLAUDE.md.
bool RequireConfirmed(const TSharedPtr<FJsonObject>& Args, const TCHAR* Tool, FSageToolDispatch::FOutcome& Out)
{
    bool bConfirmed = false;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
    {
        Out = FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("long-running operation; pass confirmed:true to proceed (%s)"), Tool));
        return true;
    }
    return false;
}

FSageToolDispatch::FOutcome BuildAllImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (RequireConfirmed(Args, TEXT("editor.build_all"), Reject)) return Reject;

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

FSageToolDispatch::FOutcome BuildGeometryImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (RequireConfirmed(Args, TEXT("editor.build_geometry"), Reject)) return Reject;

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
    FSageToolDispatch::FOutcome Reject;
    if (RequireConfirmed(Args, TEXT("editor.build_lighting"), Reject)) return Reject;

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

FSageToolDispatch::FOutcome BuildHlodImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (RequireConfirmed(Args, TEXT("editor.build_hlod"), Reject)) return Reject;

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
    bool bCaseSensitive = false;
    double MaxBytesNum = 64.0 * 1024.0 * 1024.0;
    Args->TryGetBoolField(TEXT("case_sensitive"), bCaseSensitive);
    Args->TryGetNumberField(TEXT("max_bytes"), MaxBytesNum);

    const FString AbsoluteLogPath = ResolveLogReadPath(Args);
    FLogReadResult Read;
    if (!ReadTextFileWithSharedFallback(AbsoluteLogPath, static_cast<int64>(MaxBytesNum), Read))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            LogReadErrorMessage(Read));
    }
    TArray<FString> Lines;
    Read.Contents.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

    TArray<TSharedPtr<FJsonValue>> Hits;
    const ESearchCase::Type SearchCase = bCaseSensitive
        ? ESearchCase::CaseSensitive
        : ESearchCase::IgnoreCase;
    for (int32 i = 0; i < Lines.Num(); ++i)
    {
        if (Hits.Num() >= MaxLines) break;
        if (!Lines[i].Contains(Query, SearchCase)) continue;
        auto O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("line"), i + 1);
        O->SetStringField(TEXT("text"), Lines[i].Left(500));
        Hits.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"),       Query);
    R->SetStringField(TEXT("log_path"),    Read.NormalizedPath);
    R->SetArrayField (TEXT("hits"),        Hits);
    R->SetNumberField(TEXT("count"),       Hits.Num());
    R->SetNumberField(TEXT("total_lines"), Lines.Num());
    R->SetBoolField  (TEXT("capped"),      Hits.Num() >= MaxLines);
    R->SetBoolField  (TEXT("case_sensitive"), bCaseSensitive);
    R->SetObjectField(TEXT("read_diagnostics"), LogReadDiagnosticsToJson(Read));
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

TArray<FString> GetAllowedCrashRoots()
{
    TArray<FString> Roots;
    auto AddRoot = [&Roots](const FString& Root)
    {
        FString Full = FPaths::ConvertRelativePathToFull(Root);
        FPaths::CollapseRelativeDirectories(Full);
        if (!Full.IsEmpty())
        {
            Roots.AddUnique(Full);
        }
    };
    AddRoot(GetUserCrashesRoot());
    if (!FPaths::ProjectSavedDir().IsEmpty())
    {
        AddRoot(FPaths::ProjectSavedDir() / TEXT("Crashes"));
    }
    return Roots;
}

TArray<TSharedPtr<FJsonValue>> CrashRootsToJson(const TArray<FString>& Roots)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FString& Root : Roots)
    {
        Out.Add(MakeShared<FJsonValueString>(Root));
    }
    return Out;
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
    const TArray<FString> Roots = GetAllowedCrashRoots();
    IFileManager& FM = IFileManager::Get();
    TArray<FString> Dirs;
    for (const FString& Root : Roots)
    {
        Dirs.Append(ListCrashDirs(FM, Root));
    }

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
        // Crash log filename: <ProjectName>.log (e.g. Lyra.log, Kale.log).
        // The legacy hardcoded "SageTest.log" only matched the dogfooding
        // sample project — broken for every real project. Compose dynamically.
        const FString ProjLog = FString(FApp::GetProjectName()) + TEXT(".log");
        O->SetBoolField(TEXT("has_log"),     FM.FileExists(*(D / ProjLog)) ||
                                              FM.FileExists(*(D / TEXT("UnrealEditor.log"))));
        O->SetBoolField(TEXT("has_dump"),    FM.FileExists(*(D / TEXT("minidump.dmp"))));
        O->SetBoolField(TEXT("has_context"), FM.FileExists(*(D / TEXT("CrashContext.runtime-xml"))));
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("crashes_root"),  Roots.Num() > 0 ? Roots[0] : FString());
    R->SetArrayField (TEXT("crashes_roots"), CrashRootsToJson(Roots));
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
    const TArray<FString> Roots = GetAllowedCrashRoots();
    IFileManager& FM = IFileManager::Get();
    TArray<FString> Dirs;
    for (const FString& Root : Roots)
    {
        Dirs.Append(ListCrashDirs(FM, Root));
    }

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
    R->SetArrayField (TEXT("crashes_roots"), CrashRootsToJson(Roots));
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
    // Windows paths are case-insensitive — `c:\users\...` vs `C:\Users\...`
    // would otherwise spuriously fail the prefix check.
    const TArray<FString> Roots = GetAllowedCrashRoots();
    FString       Abs  = FPaths::ConvertRelativePathToFull(CrashDir);
    FPaths::CollapseRelativeDirectories(Abs);
#if PLATFORM_WINDOWS
    const ESearchCase::Type kCase = ESearchCase::IgnoreCase;
#else
    const ESearchCase::Type kCase = ESearchCase::CaseSensitive;
#endif
    bool bAllowed = false;
    for (const FString& Root : Roots)
    {
        if (Abs.StartsWith(Root, kCase))
        {
            bAllowed = true;
            break;
        }
    }
    if (!bAllowed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("crash_dir outside allowed crashes roots: %s"), *Abs));
    }
    IFileManager& FM = IFileManager::Get();
    if (!FM.DirectoryExists(*Abs))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("crash_dir not found: %s"), *Abs));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("crash_dir"), Abs);
    R->SetArrayField (TEXT("crashes_roots"), CrashRootsToJson(Roots));
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

    // Tail of the crash log. Caller can request a specific size via `lines`
    // (default 50, max 10000) and skip from the end with `offset_from_end`.
    int32 TailLines = 50;
    int32 OffsetFromEnd = 0;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("lines"), N))
            TailLines = FMath::Clamp(static_cast<int32>(N), 1, 10000);
        if (Args->TryGetNumberField(TEXT("offset_from_end"), N))
            OffsetFromEnd = FMath::Max(0, static_cast<int32>(N));
    }

    // Crash log filename derives from project (e.g. Lyra.log); keep
    // UnrealEditor.log as a fallback for legacy/older crashes.
    const FString ProjLog = FString(FApp::GetProjectName()) + TEXT(".log");
    TArray<FString> Candidates = { TEXT("UnrealEditor.log"), ProjLog };
    for (const FString& Cand : Candidates)
    {
        const FString LP = Abs / Cand;
        FString LogTxt;
        if (!FFileHelper::LoadFileToString(LogTxt, *LP)) continue;
        TArray<FString> Lines;
        LogTxt.ParseIntoArrayLines(Lines, false);
        const int32 EndExclusive = FMath::Max(0, Lines.Num() - OffsetFromEnd);
        const int32 Tail         = FMath::Max(0, EndExclusive - TailLines);
        TArray<TSharedPtr<FJsonValue>> Out;
        for (int32 i = Tail; i < EndExclusive; ++i)
        {
            Out.Add(MakeShared<FJsonValueString>(Lines[i].Left(500)));
        }
        R->SetStringField(TEXT("log_path"),         LP);
        R->SetArrayField (TEXT("log_tail"),         Out);
        R->SetNumberField(TEXT("log_total_lines"),  Lines.Num());
        R->SetNumberField(TEXT("lines"),            TailLines);
        R->SetNumberField(TEXT("offset_from_end"),  OffsetFromEnd);
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

    if (PropName.Equals(TEXT("Skeleton"), ESearchCase::CaseSensitive))
    {
        if (UAnimationAsset* Anim = Cast<UAnimationAsset>(Obj))
        {
            auto ExtractObjectPath = [](const TSharedPtr<FJsonValue>& Value) -> FString
            {
                if (!Value.IsValid() || Value->IsNull())
                {
                    return FString();
                }
                FString Raw = Value->AsString().TrimStartAndEnd();
                int32 FirstQuote = INDEX_NONE;
                int32 LastQuote = INDEX_NONE;
                if (Raw.FindChar(TEXT('\''), FirstQuote)
                    && Raw.FindLastChar(TEXT('\''), LastQuote)
                    && LastQuote > FirstQuote)
                {
                    Raw = Raw.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
                }
                return Raw;
            };
            auto ResolveObjectOrPackage = [](const FString& RawPath) -> UObject*
            {
                auto TryResolve = [](const FString& Candidate) -> UObject*
                {
                    FSoftObjectPath Soft(Candidate);
                    if (UObject* Resolved = Soft.ResolveObject())
                    {
                        return Resolved;
                    }
                    return Soft.TryLoad();
                };
                if (UObject* Resolved = TryResolve(RawPath))
                {
                    return Resolved;
                }
                if (!RawPath.Contains(TEXT(".")))
                {
                    FString Package = RawPath;
                    Package.ReplaceInline(TEXT("\\"), TEXT("/"));
                    FString AssetName;
                    FString Unused;
                    if (!Package.Split(TEXT("/"), &Unused, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
                    {
                        AssetName = Package;
                    }
                    return TryResolve(Package + TEXT(".") + AssetName);
                }
                return nullptr;
            };

            const FString SkeletonPath = ExtractObjectPath(ValueJson);
            USkeleton* Skeleton = Cast<USkeleton>(ResolveObjectOrPackage(SkeletonPath));
            if (!Skeleton)
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("Skeleton write requires a USkeleton path; could not resolve: %s"), *SkeletonPath));
            }

            Anim->Modify();
            Anim->PreEditChange(Prop);
            Anim->SetSkeleton(Skeleton);
            FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
            Anim->PostEditChangeProperty(ChangeEvent);
            Anim->MarkPackageDirty();

            USkeleton* Readback = Anim->GetSkeleton();
            if (Readback != Skeleton)
            {
                return FSageToolDispatch::FOutcome::MakeError(-32603,
                    FString::Printf(TEXT("Skeleton write did not persist on readback for %s (requested=%s, readback=%s)"),
                                    *Path,
                                    *Skeleton->GetPathName(),
                                    Readback ? *Readback->GetPathName() : TEXT("<null>")));
            }

            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("path"), Path);
            R->SetStringField(TEXT("property"), PropName);
            R->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
            R->SetStringField(TEXT("readback"), Skeleton->GetPathName());
            R->SetBoolField(TEXT("used_animation_set_skeleton"), true);
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
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
    if (TSharedPtr<FJsonValue> Readback = sage::tools::detail::GetUPropertyAsJson(Obj, Prop))
    {
        R->SetField(TEXT("readback"), Readback);
    }
    if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
    {
        UObject* ClassObj = ClassProp->GetObjectPropertyValue(
            Prop->ContainerPtrToValuePtr<void>(Obj));
        if (UClass* ReadbackClass = Cast<UClass>(ClassObj))
        {
            R->SetStringField(TEXT("resolved_class"), ReadbackClass->GetPathName());
        }
        R->SetStringField(TEXT("meta_class"), ClassProp->MetaClass
            ? ClassProp->MetaClass->GetPathName()
            : FString());
    }
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

// ---- input.* PIE simulation ----------------------------------------------

struct FPieInputTarget
{
    UWorld* World = nullptr;
    APlayerController* PlayerController = nullptr;
    ULocalPlayer* LocalPlayer = nullptr;
    UGameViewportClient* ViewportClient = nullptr;
    FViewport* Viewport = nullptr;
    FInputDeviceId InputDevice = INPUTDEVICEID_NONE;
    int32 LocalPlayerIndex = 0;
    FString ResolutionSource;
    FString RequestedWorld;
    FString RequestedController;
    FString RequestedPawn;
    TArray<FString> CandidateWorlds;
    bool bEditorPlayWorldMismatch = false;
};

TSharedRef<FJsonObject> InputObjectIdentityJson(UObject* Obj)
{
    auto O = MakeShared<FJsonObject>();
    if (!Obj)
    {
        O->SetBoolField(TEXT("valid"), false);
        return O;
    }
    O->SetBoolField(TEXT("valid"), true);
    O->SetStringField(TEXT("name"), Obj->GetName());
    O->SetStringField(TEXT("path"), Obj->GetPathName());
    O->SetStringField(TEXT("class"), Obj->GetClass() ? Obj->GetClass()->GetPathName() : FString());
    return O;
}

bool ObjectMatchesInputIdentifier(UObject* Obj, const FString& Identifier)
{
    if (!Obj || Identifier.IsEmpty())
    {
        return Identifier.IsEmpty();
    }
    FString Normalized = Identifier;
    FPaths::NormalizeFilename(Normalized);
    return Obj->GetPathName() == Identifier
        || Obj->GetPathName() == Normalized
        || Obj->GetName() == Identifier
        || Obj->GetName() == Normalized
        || Obj->GetPathName().EndsWith(Identifier, ESearchCase::IgnoreCase)
        || Obj->GetPathName().EndsWith(Normalized, ESearchCase::IgnoreCase);
}

bool ActorMatchesInputIdentifier(AActor* Actor, const FString& Identifier)
{
    if (!Actor || Identifier.IsEmpty())
    {
        return Identifier.IsEmpty();
    }
    return ObjectMatchesInputIdentifier(Actor, Identifier)
        || Actor->GetActorLabel() == Identifier
        || Actor->GetActorNameOrLabel() == Identifier;
}

void AddPieWorldSummary(UWorld* World, TArray<TSharedPtr<FJsonValue>>& Out)
{
    auto O = MakeShared<FJsonObject>();
    if (!World)
    {
        O->SetBoolField(TEXT("valid"), false);
        Out.Add(MakeShared<FJsonValueObject>(O));
        return;
    }
    O->SetBoolField(TEXT("valid"), true);
    O->SetStringField(TEXT("name"), World->GetName());
    O->SetStringField(TEXT("path"), World->GetPathName());
    O->SetStringField(TEXT("map_name"), World->GetMapName());
    O->SetNumberField(TEXT("time_seconds"), World->GetTimeSeconds());
    O->SetBoolField(TEXT("is_paused"), World->IsPaused());
    int32 LocalPlayerCount = 0;
    if (UGameInstance* GI = World->GetGameInstance())
    {
        LocalPlayerCount = GI->GetLocalPlayers().Num();
    }
    O->SetNumberField(TEXT("local_player_count"), LocalPlayerCount);
    Out.Add(MakeShared<FJsonValueObject>(O));
}

TArray<UWorld*> GetLivePieWorldCandidates()
{
    TArray<UWorld*> Worlds;
    if (GEngine)
    {
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            UWorld* World = Ctx.World();
            if (Ctx.WorldType == EWorldType::PIE && World)
            {
                Worlds.AddUnique(World);
            }
        }
    }
    if (GEditor && GEditor->PlayWorld)
    {
        Worlds.AddUnique(GEditor->PlayWorld);
    }
    return Worlds;
}

void ParseInputTargetArgs(const TSharedPtr<FJsonObject>& Args,
                          FString& OutWorld,
                          FString& OutController,
                          FString& OutPawn)
{
    if (!Args.IsValid())
    {
        return;
    }
    Args->TryGetStringField(TEXT("world"), OutWorld);
    Args->TryGetStringField(TEXT("world_name"), OutWorld);
    Args->TryGetStringField(TEXT("world_path"), OutWorld);
    Args->TryGetStringField(TEXT("expected_world"), OutWorld);
    Args->TryGetStringField(TEXT("controller"), OutController);
    Args->TryGetStringField(TEXT("controller_id"), OutController);
    Args->TryGetStringField(TEXT("player_controller"), OutController);
    Args->TryGetStringField(TEXT("player_controller_id"), OutController);
    Args->TryGetStringField(TEXT("pawn"), OutPawn);
    Args->TryGetStringField(TEXT("pawn_id"), OutPawn);
    Args->TryGetStringField(TEXT("expected_pawn"), OutPawn);
}

bool ResolvePlayerInPieWorld(UWorld* World,
                             int32 LocalPlayerIndex,
                             const FString& ControllerId,
                             const FString& PawnId,
                             ULocalPlayer*& OutLocalPlayer,
                             APlayerController*& OutPlayerController,
                             FString& OutError)
{
    OutLocalPlayer = nullptr;
    OutPlayerController = nullptr;
    if (!World)
    {
        OutError = TEXT("world is null");
        return false;
    }

    if (UGameInstance* GI = World->GetGameInstance())
    {
        const TArray<ULocalPlayer*>& Players = GI->GetLocalPlayers();
        if (Players.IsValidIndex(LocalPlayerIndex))
        {
            OutLocalPlayer = Players[LocalPlayerIndex];
        }
    }

    if (!ControllerId.IsEmpty())
    {
        for (TActorIterator<APlayerController> It(World); It; ++It)
        {
            APlayerController* Candidate = *It;
            if (ActorMatchesInputIdentifier(Candidate, ControllerId))
            {
                OutPlayerController = Candidate;
                OutLocalPlayer = Candidate->GetLocalPlayer();
                break;
            }
        }
        if (!OutPlayerController)
        {
            OutError = FString::Printf(TEXT("controller target not found in PIE world %s: %s"),
                                       *World->GetPathName(), *ControllerId);
            return false;
        }
    }
    else
    {
        OutPlayerController = OutLocalPlayer
            ? OutLocalPlayer->GetPlayerController(World)
            : UGameplayStatics::GetPlayerController(World, LocalPlayerIndex);
        if (!OutLocalPlayer && OutPlayerController)
        {
            OutLocalPlayer = OutPlayerController->GetLocalPlayer();
        }
    }

    if (!PawnId.IsEmpty()
        && (!OutPlayerController || !ActorMatchesInputIdentifier(OutPlayerController->GetPawn(), PawnId)))
    {
        APlayerController* MatchedByPawn = nullptr;
        for (TActorIterator<APlayerController> It(World); It; ++It)
        {
            APlayerController* Candidate = *It;
            if (Candidate && ActorMatchesInputIdentifier(Candidate->GetPawn(), PawnId))
            {
                MatchedByPawn = Candidate;
                break;
            }
        }
        if (MatchedByPawn)
        {
            OutPlayerController = MatchedByPawn;
            OutLocalPlayer = MatchedByPawn->GetLocalPlayer();
        }
        else
        {
            OutError = FString::Printf(TEXT("pawn target not controlled in PIE world %s: %s"),
                                       *World->GetPathName(), *PawnId);
            return false;
        }
    }

    if (!OutPlayerController)
    {
        OutError = FString::Printf(TEXT("local player %d has no PlayerController in PIE world %s"),
                                   LocalPlayerIndex,
                                   *World->GetPathName());
        return false;
    }

    if (!PawnId.IsEmpty()
        && !ActorMatchesInputIdentifier(OutPlayerController->GetPawn(), PawnId))
    {
        OutError = FString::Printf(TEXT("resolved controller pawn does not match requested pawn %s"),
                                   *PawnId);
        return false;
    }
    return true;
}

TSharedRef<FJsonObject> PieInputTargetToJson(const FPieInputTarget& Target)
{
    auto O = MakeShared<FJsonObject>();
    O->SetObjectField(TEXT("world"), InputObjectIdentityJson(Target.World));
    O->SetObjectField(TEXT("player_controller"), InputObjectIdentityJson(Target.PlayerController));
    O->SetObjectField(TEXT("pawn"), InputObjectIdentityJson(
        Target.PlayerController ? Target.PlayerController->GetPawn() : nullptr));
    O->SetObjectField(TEXT("local_player"), InputObjectIdentityJson(Target.LocalPlayer));
    O->SetObjectField(TEXT("viewport_client"), InputObjectIdentityJson(Target.ViewportClient));
    O->SetBoolField(TEXT("pie_viewport_available"), Target.Viewport != nullptr);
    O->SetNumberField(TEXT("input_device_id"), Target.InputDevice.IsValid()
        ? Target.InputDevice.GetId()
        : -1);
    O->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
    O->SetStringField(TEXT("resolution_source"), Target.ResolutionSource);
    O->SetStringField(TEXT("requested_world"), Target.RequestedWorld);
    O->SetStringField(TEXT("requested_controller"), Target.RequestedController);
    O->SetStringField(TEXT("requested_pawn"), Target.RequestedPawn);
    O->SetBoolField(TEXT("editor_play_world_mismatch"), Target.bEditorPlayWorldMismatch);

    TArray<TSharedPtr<FJsonValue>> Worlds;
    for (const FString& WorldPath : Target.CandidateWorlds)
    {
        Worlds.Add(MakeShared<FJsonValueString>(WorldPath));
    }
    O->SetArrayField(TEXT("candidate_pie_worlds"), Worlds);
    O->SetNumberField(TEXT("candidate_pie_world_count"), Worlds.Num());
    return O;
}

FString InputPlayNetModeToString(EPlayNetMode Mode)
{
    switch (Mode)
    {
    case PIE_Standalone:
        return TEXT("standalone");
    case PIE_ListenServer:
        return TEXT("listen_server");
    case PIE_Client:
        return TEXT("client");
    default:
        return FString::Printf(TEXT("unknown_%d"), static_cast<int32>(Mode));
    }
}

FString BuildNoLocalPlayerInputError(const FPieInputTarget& Target, const TCHAR* ToolName)
{
    EPlayNetMode NetMode = PIE_Standalone;
    int32 ClientCount = 1;
    bool bRunUnderOneProcess = true;
    bool bLaunchSeparateServer = false;
    if (const ULevelEditorPlaySettings* Settings = GetDefault<ULevelEditorPlaySettings>())
    {
        Settings->GetPlayNetMode(NetMode);
        Settings->GetPlayNumberOfClients(ClientCount);
        Settings->GetRunUnderOneProcess(bRunUnderOneProcess);
        bLaunchSeparateServer = Settings->bLaunchSeparateServer;
    }

    const FString WorldPath = Target.World ? Target.World->GetPathName() : FString(TEXT("<none>"));
    const FString ControllerPath = Target.PlayerController
        ? Target.PlayerController->GetPathName()
        : FString(TEXT("<none>"));
    const FString PawnPath = (Target.PlayerController && Target.PlayerController->GetPawn())
        ? Target.PlayerController->GetPawn()->GetPathName()
        : FString(TEXT("<none>"));
    return FString::Printf(
        TEXT("%s resolved a PIE PlayerController without a ULocalPlayer. Current editor play settings: PlayNetMode=%s(%d), PlayNumberOfClients=%d, RunUnderOneProcess=%s, bLaunchSeparateServer=%s. Target: world=%s, player_controller=%s, pawn=%s, local_player_index=%d. Stop PIE and relaunch with run_pie {\"force_local_player\":true,\"net_mode\":\"standalone\",\"clients\":1,\"selected_viewport\":true}, or use a PIE world/controller backed by a local player."),
        ToolName,
        *InputPlayNetModeToString(NetMode),
        static_cast<int32>(NetMode),
        ClientCount,
        bRunUnderOneProcess ? TEXT("true") : TEXT("false"),
        bLaunchSeparateServer ? TEXT("true") : TEXT("false"),
        *WorldPath,
        *ControllerPath,
        *PawnPath,
        Target.LocalPlayerIndex);
}

int32 GetLocalPlayerIndexArg(const TSharedPtr<FJsonObject>& Args)
{
    double Index = 0.0;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("local_player_index"), Index);
    }
    return FMath::Max(0, static_cast<int32>(Index));
}

bool ResolvePieInputTarget(const TSharedPtr<FJsonObject>& Args,
                           FPieInputTarget& Out,
                           FString& OutError)
{
    if (!GEditor)
    {
        OutError = TEXT("GEditor unavailable");
        return false;
    }
    Out.LocalPlayerIndex = GetLocalPlayerIndexArg(Args);
    ParseInputTargetArgs(Args, Out.RequestedWorld, Out.RequestedController, Out.RequestedPawn);

    const TArray<UWorld*> CandidateWorlds = GetLivePieWorldCandidates();
    for (UWorld* World : CandidateWorlds)
    {
        if (World)
        {
            Out.CandidateWorlds.Add(World->GetPathName());
        }
    }
    if (CandidateWorlds.Num() <= 0)
    {
        OutError = TEXT("no live PIE world active - start PIE first (run_pie)");
        return false;
    }

    FString LastResolutionError;
    auto TryWorld = [&Out, &LastResolutionError](UWorld* World, const TCHAR* Source) -> bool
    {
        ULocalPlayer* LocalPlayer = nullptr;
        APlayerController* PlayerController = nullptr;
        FString Error;
        if (!ResolvePlayerInPieWorld(
                World,
                Out.LocalPlayerIndex,
                Out.RequestedController,
                Out.RequestedPawn,
                LocalPlayer,
                PlayerController,
                Error))
        {
            LastResolutionError = Error;
            return false;
        }
        Out.World = World;
        Out.LocalPlayer = LocalPlayer;
        Out.PlayerController = PlayerController;
        Out.ResolutionSource = Source;
        return true;
    };

    if (!Out.RequestedWorld.IsEmpty())
    {
        for (UWorld* World : CandidateWorlds)
        {
            if (!World || !ObjectMatchesInputIdentifier(World, Out.RequestedWorld))
            {
                continue;
            }
            if (TryWorld(World, TEXT("explicit_world")))
            {
                break;
            }
        }
        if (!Out.World)
        {
            OutError = FString::Printf(TEXT("requested PIE world target was not resolved: %s (%s)"),
                                       *Out.RequestedWorld,
                                       *LastResolutionError);
            return false;
        }
    }
    else
    {
        // Match gameplay.get_local_player/readback behavior by preferring the
        // first live PIE world context, not GEditor->PlayWorld, which can lag
        // behind after repeated PIE teardown/load cycles.
        for (UWorld* World : CandidateWorlds)
        {
            if (World && TryWorld(World, TEXT("active_pie_world_context")))
            {
                break;
            }
        }
        if (!Out.World)
        {
            OutError = LastResolutionError.IsEmpty()
                ? TEXT("no PIE local player target resolved")
                : LastResolutionError;
            return false;
        }
    }

    Out.bEditorPlayWorldMismatch = GEditor->PlayWorld
        && Out.World
        && GEditor->PlayWorld != Out.World;
    Out.ViewportClient = Out.World->GetGameViewport();
    Out.Viewport = GEditor->GetPIEViewport();
    IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
    if (Out.LocalPlayer)
    {
        Out.InputDevice = DeviceMapper.GetPrimaryInputDeviceForUser(
            Out.LocalPlayer->GetPlatformUserId());
    }
    if (!Out.InputDevice.IsValid())
    {
        Out.InputDevice = DeviceMapper.GetDefaultInputDevice();
    }
    return true;
}

bool ResolveInputKey(const TSharedPtr<FJsonObject>& Args, FKey& OutKey, FString& OutError)
{
    FString KeyName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
    {
        OutError = TEXT("missing 'key'");
        return false;
    }
    OutKey = FKey(*KeyName);
    if (!OutKey.IsValid())
    {
        OutError = FString::Printf(TEXT("invalid key: %s"), *KeyName);
        return false;
    }
    return true;
}

struct FInputRouteResult
{
    bool bViewportClientAttempted = false;
    bool bViewportClientHandled = false;
    bool bPlayerControllerAttempted = false;
    bool bPlayerControllerHandled = false;

    bool Handled() const
    {
        return bViewportClientHandled || bPlayerControllerHandled;
    }
};

TSharedRef<FJsonObject> InputRouteToJson(const FInputRouteResult& Route)
{
    auto O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("handled"), Route.Handled());
    O->SetBoolField(TEXT("viewport_client_attempted"), Route.bViewportClientAttempted);
    O->SetBoolField(TEXT("viewport_client_handled"), Route.bViewportClientHandled);
    O->SetBoolField(TEXT("player_controller_attempted"), Route.bPlayerControllerAttempted);
    O->SetBoolField(TEXT("player_controller_handled"), Route.bPlayerControllerHandled);
    return O;
}

FInputRouteResult InjectPieKeyEventDetailed(const FPieInputTarget& Target,
                                            const FKey& Key,
                                            EInputEvent Event,
                                            float Amount)
{
    FInputKeyEventArgs EventArgs = FInputKeyEventArgs::CreateSimulated(
        Key,
        Event,
        Amount,
        Key.IsAnalog() ? 1 : 0,
        Target.InputDevice,
        /*bIsTouchEvent=*/false,
        Target.Viewport);

    FInputRouteResult Route;
    if (Target.ViewportClient)
    {
        Route.bViewportClientAttempted = true;
        Route.bViewportClientHandled = Target.ViewportClient->InputKey(EventArgs);
    }
    if (!Route.Handled() && Target.PlayerController)
    {
        Route.bPlayerControllerAttempted = true;
        Route.bPlayerControllerHandled = Target.PlayerController->InputKey(EventArgs);
    }
    return Route;
}

bool InjectPieKeyEvent(const FPieInputTarget& Target,
                       const FKey& Key,
                       EInputEvent Event,
                       float Amount)
{
    return InjectPieKeyEventDetailed(Target, Key, Event, Amount).Handled();
}

void AddReleaseTargetArgs(const FPieInputTarget& Target,
                          const FKey& Key,
                          TSharedRef<FJsonObject> Args)
{
    Args->SetStringField(TEXT("key"), Key.GetFName().ToString());
    Args->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
    if (Target.World)
    {
        Args->SetStringField(TEXT("world_path"), Target.World->GetPathName());
    }
    if (Target.PlayerController)
    {
        Args->SetStringField(TEXT("controller_id"), Target.PlayerController->GetPathName());
        if (APawn* Pawn = Target.PlayerController->GetPawn())
        {
            Args->SetStringField(TEXT("pawn_id"), Pawn->GetPathName());
        }
    }
}

void ScheduleKeyRelease(const FKey& Key, const FPieInputTarget& Target, double Duration)
{
    const FString TargetWorldPath = Target.World ? Target.World->GetPathName() : FString();
    const FString TargetControllerPath = Target.PlayerController ? Target.PlayerController->GetPathName() : FString();
    const FString TargetPawnPath = (Target.PlayerController && Target.PlayerController->GetPawn())
        ? Target.PlayerController->GetPawn()->GetPathName()
        : FString();
    const int32 LocalPlayerIndex = Target.LocalPlayerIndex;
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([Key, TargetWorldPath, TargetControllerPath, TargetPawnPath, LocalPlayerIndex](float) -> bool
        {
            auto ReleaseArgs = MakeShared<FJsonObject>();
            ReleaseArgs->SetStringField(TEXT("key"), Key.GetFName().ToString());
            ReleaseArgs->SetNumberField(TEXT("local_player_index"), LocalPlayerIndex);
            if (!TargetWorldPath.IsEmpty())
            {
                ReleaseArgs->SetStringField(TEXT("world_path"), TargetWorldPath);
            }
            if (!TargetControllerPath.IsEmpty())
            {
                ReleaseArgs->SetStringField(TEXT("controller_id"), TargetControllerPath);
            }
            if (!TargetPawnPath.IsEmpty())
            {
                ReleaseArgs->SetStringField(TEXT("pawn_id"), TargetPawnPath);
            }
            FPieInputTarget ReleaseTarget;
            FString ReleaseError;
            if (ResolvePieInputTarget(ReleaseArgs, ReleaseTarget, ReleaseError))
            {
                InjectPieKeyEvent(ReleaseTarget, Key, IE_Released, 0.0f);
            }
            else
            {
                UE_LOG(LogSageBridge, Warning,
                    TEXT("scheduled PIE key release failed to resolve original target: %s"),
                    *ReleaseError);
            }
            return false;
        }),
        static_cast<float>(Duration));
}

FSageToolDispatch::FOutcome KeyInputImpl(const TSharedPtr<FJsonObject>& Args,
                                         EInputEvent Event,
                                         const TCHAR* Verb)
{
    FPieInputTarget Target;
    FString Error;
    if (!ResolvePieInputTarget(Args, Target, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004, Error);
    }
    FKey Key;
    if (!ResolveInputKey(Args, Key, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }
    if (!Target.LocalPlayer)
    {
        return FSageToolDispatch::FOutcome::MakeError(
            -32004,
            BuildNoLocalPlayerInputError(Target, TEXT("input.release_key")));
    }

    double AmountNum = (Event == IE_Released) ? 0.0 : 1.0;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("amount"), AmountNum);
    const FInputRouteResult Route = InjectPieKeyEventDetailed(Target, Key, Event, static_cast<float>(AmountNum));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("key"), Key.GetFName().ToString());
    R->SetStringField(TEXT("event"), Verb);
    R->SetBoolField(TEXT("handled"), Route.Handled());
    R->SetObjectField(TEXT("route"), InputRouteToJson(Route));
    R->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
    R->SetStringField(TEXT("world"), Target.World ? Target.World->GetPathName() : FString());
    R->SetObjectField(TEXT("target"), PieInputTargetToJson(Target));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome PressKeyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FPieInputTarget Target;
    FString Error;
    if (!ResolvePieInputTarget(Args, Target, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004, Error);
    }
    FKey Key;
    if (!ResolveInputKey(Args, Key, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }
    if (!Target.LocalPlayer)
    {
        return FSageToolDispatch::FOutcome::MakeError(
            -32004,
            BuildNoLocalPlayerInputError(Target, TEXT("input.press_key")));
    }

    double AmountNum = 1.0;
    double Duration = 0.0;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("amount"), AmountNum);
        Args->TryGetNumberField(TEXT("duration"), Duration);
        Args->TryGetNumberField(TEXT("duration_seconds"), Duration);
    }

    const FInputRouteResult PressRoute = InjectPieKeyEventDetailed(
        Target, Key, IE_Pressed, static_cast<float>(AmountNum));
    bool bReleased = false;
    bool bReleaseScheduled = false;
    FInputRouteResult ReleaseRoute;
    if (Duration > 0.0)
    {
        ScheduleKeyRelease(Key, Target, Duration);
        bReleaseScheduled = true;
    }
    else
    {
        ReleaseRoute = InjectPieKeyEventDetailed(Target, Key, IE_Released, 0.0f);
        bReleased = ReleaseRoute.Handled();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("key"), Key.GetFName().ToString());
    R->SetBoolField(TEXT("pressed"), PressRoute.Handled());
    R->SetBoolField(TEXT("released"), bReleased);
    R->SetBoolField(TEXT("release_scheduled"), bReleaseScheduled);
    R->SetObjectField(TEXT("press_route"), InputRouteToJson(PressRoute));
    if (!bReleaseScheduled)
    {
        R->SetObjectField(TEXT("release_route"), InputRouteToJson(ReleaseRoute));
    }
    R->SetNumberField(TEXT("duration"), Duration);
    R->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
    R->SetObjectField(TEXT("target"), PieInputTargetToJson(Target));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome HoldKeyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FPieInputTarget Target;
    FString Error;
    if (!ResolvePieInputTarget(Args, Target, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004, Error);
    }
    FKey Key;
    if (!ResolveInputKey(Args, Key, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }
    if (!Target.LocalPlayer)
    {
        return FSageToolDispatch::FOutcome::MakeError(
            -32004,
            BuildNoLocalPlayerInputError(Target, TEXT("input.hold_key")));
    }

    double AmountNum = 1.0;
    double Duration = 0.0;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("amount"), AmountNum);
        Args->TryGetNumberField(TEXT("duration"), Duration);
        Args->TryGetNumberField(TEXT("duration_seconds"), Duration);
    }
    const FInputRouteResult PressRoute = InjectPieKeyEventDetailed(
        Target, Key, IE_Pressed, static_cast<float>(AmountNum));

    bool bReleaseScheduled = false;
    if (Duration > 0.0)
    {
        ScheduleKeyRelease(Key, Target, Duration);
        bReleaseScheduled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("key"), Key.GetFName().ToString());
    R->SetStringField(TEXT("event"), TEXT("pressed"));
    R->SetBoolField(TEXT("handled"), PressRoute.Handled());
    R->SetObjectField(TEXT("route"), InputRouteToJson(PressRoute));
    R->SetBoolField(TEXT("release_scheduled"), bReleaseScheduled);
    R->SetNumberField(TEXT("duration"), Duration);
    R->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
    R->SetStringField(TEXT("world"), Target.World ? Target.World->GetPathName() : FString());
    R->SetObjectField(TEXT("target"), PieInputTargetToJson(Target));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReleaseKeyImpl(const TSharedPtr<FJsonObject>& Args)
{
    return KeyInputImpl(Args, IE_Released, TEXT("released"));
}

bool JsonToVector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
    if (!Value.IsValid())
    {
        Out = FVector(1.0, 0.0, 0.0);
        return true;
    }
    if (Value->Type == EJson::Boolean)
    {
        Out = FVector(Value->AsBool() ? 1.0 : 0.0, 0.0, 0.0);
        return true;
    }
    if (Value->Type == EJson::Number)
    {
        Out = FVector(Value->AsNumber(), 0.0, 0.0);
        return true;
    }
    if (Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
        if (Arr.Num() <= 0) return false;
        Out = FVector(Arr[0]->AsNumber(),
                      Arr.Num() > 1 ? Arr[1]->AsNumber() : 0.0,
                      Arr.Num() > 2 ? Arr[2]->AsNumber() : 0.0);
        return true;
    }
    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid()) return false;
        double X = 0.0, Y = 0.0, Z = 0.0;
        Obj->TryGetNumberField(TEXT("x"), X);
        Obj->TryGetNumberField(TEXT("y"), Y);
        Obj->TryGetNumberField(TEXT("z"), Z);
        Out = FVector(X, Y, Z);
        return true;
    }
    return false;
}

FInputActionValue MakeActionValue(EInputActionValueType Type, const FVector& Raw)
{
    return FInputActionValue(Type, Raw);
}

UInputAction* ResolveInputAction(const FString& ActionPath)
{
    FSoftObjectPath Soft(ActionPath);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Cast<UInputAction>(Obj);
}

FSageToolDispatch::FOutcome TriggerActionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FPieInputTarget Target;
    FString Error;
    if (!ResolvePieInputTarget(Args, Target, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004, Error);
    }
    if (!Target.LocalPlayer)
    {
        return FSageToolDispatch::FOutcome::MakeError(
            -32004,
            BuildNoLocalPlayerInputError(Target, TEXT("input.trigger_action")));
    }
    UEnhancedInputLocalPlayerSubsystem* Subsystem =
        Target.LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
    if (!Subsystem)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EnhancedInputLocalPlayerSubsystem unavailable"));
    }

    FString ActionPath;
    FString MappingName;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("action"), ActionPath);
        Args->TryGetStringField(TEXT("action_path"), ActionPath);
        Args->TryGetStringField(TEXT("mapping_name"), MappingName);
    }
    UInputAction* Action = nullptr;
    if (!ActionPath.IsEmpty())
    {
        Action = ResolveInputAction(ActionPath);
        if (!Action)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("InputAction not found: %s"), *ActionPath));
        }
    }
    if (!Action && MappingName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'action'/'action_path' or 'mapping_name'"));
    }

    FVector RawVector;
    const TSharedPtr<FJsonValue> Value = Args.IsValid()
        ? Args->Values.FindRef(TEXT("value"))
        : TSharedPtr<FJsonValue>();
    if (!JsonToVector(Value, RawVector))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("'value' must be boolean, number, array [x,y,z], or object {x,y,z}"));
    }

    EInputActionValueType ValueType = Action ? Action->ValueType : EInputActionValueType::Boolean;
    if (!Action && Value.IsValid())
    {
        ValueType = Value->Type == EJson::Array
            ? (Value->AsArray().Num() >= 3 ? EInputActionValueType::Axis3D
               : Value->AsArray().Num() >= 2 ? EInputActionValueType::Axis2D
                                             : EInputActionValueType::Axis1D)
            : (Value->Type == EJson::Number ? EInputActionValueType::Axis1D
                                             : EInputActionValueType::Boolean);
    }
    const FInputActionValue ActionValue = MakeActionValue(ValueType, RawVector);

    FString Mode = TEXT("once");
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("mode"), Mode);
        bool bContinuous = false;
        if (Args->TryGetBoolField(TEXT("continuous"), bContinuous) && bContinuous)
        {
            Mode = TEXT("start");
        }
    }
    Mode = Mode.ToLower();

    double Duration = 0.0;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("duration"), Duration);
        Args->TryGetNumberField(TEXT("duration_seconds"), Duration);
    }

    bool bInjected = false;
    bool bStopped = false;
    bool bStopScheduled = false;
    const TArray<UInputModifier*> Modifiers;
    const TArray<UInputTrigger*> Triggers;
    if (Mode == TEXT("stop") || Mode == TEXT("release"))
    {
        if (Action)
        {
            Subsystem->StopContinuousInputInjectionForAction(Action);
        }
        else
        {
            Subsystem->StopContinuousInputInjectionForPlayerMapping(FName(*MappingName));
        }
        bStopped = true;
    }
    else if (Mode == TEXT("start") || Mode == TEXT("hold"))
    {
        if (Action)
        {
            Subsystem->StartContinuousInputInjectionForAction(Action, ActionValue, Modifiers, Triggers);
        }
        else
        {
            Subsystem->StartContinuousInputInjectionForPlayerMapping(
                FName(*MappingName), ActionValue, Modifiers, Triggers);
        }
        bInjected = true;
        if (Duration > 0.0)
        {
            TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> WeakSubsystem(Subsystem);
            TWeakObjectPtr<UInputAction> WeakAction(Action);
            FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([WeakSubsystem, WeakAction, MappingName](float) -> bool
                {
                    if (UEnhancedInputLocalPlayerSubsystem* S = WeakSubsystem.Get())
                    {
                        if (UInputAction* A = WeakAction.Get())
                        {
                            S->StopContinuousInputInjectionForAction(A);
                        }
                        else if (!MappingName.IsEmpty())
                        {
                            S->StopContinuousInputInjectionForPlayerMapping(FName(*MappingName));
                        }
                    }
                    return false;
                }),
                static_cast<float>(Duration));
            bStopScheduled = true;
        }
    }
    else if (Mode == TEXT("update"))
    {
        if (Action)
        {
            Subsystem->UpdateValueOfContinuousInputInjectionForAction(Action, ActionValue);
        }
        else
        {
            Subsystem->UpdateValueOfContinuousInputInjectionForPlayerMapping(
                FName(*MappingName), ActionValue);
        }
        bInjected = true;
    }
    else
    {
        if (Action)
        {
            Subsystem->InjectInputForAction(Action, ActionValue, Modifiers, Triggers);
        }
        else
        {
            Subsystem->InjectInputForPlayerMapping(FName(*MappingName), ActionValue, Modifiers, Triggers);
        }
        bInjected = true;
    }

    auto R = MakeShared<FJsonObject>();
    if (Action) R->SetStringField(TEXT("action"), Action->GetPathName());
    if (!MappingName.IsEmpty()) R->SetStringField(TEXT("mapping_name"), MappingName);
    R->SetStringField(TEXT("mode"), Mode);
    R->SetBoolField(TEXT("injected"), bInjected);
    R->SetBoolField(TEXT("stopped"), bStopped);
    R->SetBoolField(TEXT("stop_scheduled"), bStopScheduled);
    R->SetNumberField(TEXT("duration"), Duration);
    R->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
    R->SetObjectField(TEXT("target"), PieInputTargetToJson(Target));
    R->SetStringField(TEXT("world"), Target.World ? Target.World->GetPathName() : FString());
    R->SetStringField(TEXT("player_controller"), Target.PlayerController ? Target.PlayerController->GetPathName() : FString());
    R->SetStringField(TEXT("pawn"), (Target.PlayerController && Target.PlayerController->GetPawn())
        ? Target.PlayerController->GetPawn()->GetPathName()
        : FString());
    R->SetBoolField(TEXT("routed_enhanced_input_subsystem"), true);
    R->SetBoolField(TEXT("process_ability_input_inline"), false);
    R->SetStringField(TEXT("process_ability_input_note"),
        TEXT("Enhanced Input injection is queued for PIE processing; use gameplay.trace_input_action for ASC/spec readback before and after injection."));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.trace_input_action / gas.trace_ability_activation ----------

TSharedRef<FJsonObject> VectorToJson(const FVector& V)
{
    auto O = MakeShared<FJsonObject>();
    O->SetNumberField(TEXT("x"), V.X);
    O->SetNumberField(TEXT("y"), V.Y);
    O->SetNumberField(TEXT("z"), V.Z);
    return O;
}

TSharedRef<FJsonObject> ObjectSummary(UObject* Obj)
{
    auto O = MakeShared<FJsonObject>();
    if (!Obj)
    {
        O->SetBoolField(TEXT("valid"), false);
        return O;
    }
    O->SetBoolField(TEXT("valid"), true);
    O->SetStringField(TEXT("name"), Obj->GetName());
    O->SetStringField(TEXT("path"), Obj->GetPathName());
    O->SetStringField(TEXT("class"), Obj->GetClass() ? Obj->GetClass()->GetPathName() : FString());
    return O;
}

TSet<FString> ReadStringSetArg(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName)
{
    TSet<FString> Out;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Args.IsValid() || !Args->TryGetArrayField(FieldName, Values) || !Values)
    {
        return Out;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        if (Value.IsValid() && Value->Type == EJson::String)
        {
            Out.Add(Value->AsString());
        }
    }
    return Out;
}

bool StringSetMatchesName(const TSet<FString>& Needles, const FString& Name)
{
    if (Needles.Num() <= 0)
    {
        return true;
    }
    for (const FString& Needle : Needles)
    {
        if (Name == Needle || Name.Contains(Needle, ESearchCase::IgnoreCase))
        {
            return true;
        }
    }
    return false;
}

UClass* ResolvePieSpawnActorClass(const FString& ClassPath, FString& OutError)
{
    if (ClassPath.IsEmpty())
    {
        OutError = TEXT("missing 'class'/'class_path'/'uclass'");
        return nullptr;
    }

    UClass* SpawnClass = LoadClass<AActor>(nullptr, *ClassPath);
    if (!SpawnClass)
    {
        SpawnClass = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
    }
    if (!SpawnClass)
    {
        FSoftObjectPath SoftPath(ClassPath);
        UObject* Obj = SoftPath.ResolveObject();
        if (!Obj)
        {
            Obj = SoftPath.TryLoad();
        }
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Obj))
        {
            SpawnClass = Blueprint->GeneratedClass;
        }
        else
        {
            SpawnClass = Cast<UClass>(Obj);
        }
    }
    if (!SpawnClass)
    {
        OutError = FString::Printf(TEXT("actor class not found: %s"), *ClassPath);
        return nullptr;
    }
    if (!SpawnClass->IsChildOf(AActor::StaticClass()))
    {
        OutError = FString::Printf(TEXT("class is not an AActor subclass: %s"), *SpawnClass->GetPathName());
        return nullptr;
    }
    return SpawnClass;
}

bool ParsePieSpawnTransform(const TSharedPtr<FJsonObject>& Args, FTransform& OutTransform)
{
    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    FVector Scale = FVector::OneVector;
    bool bAny = false;

    if (Args.IsValid())
    {
        bAny |= detail::ParseVector3(Args, TEXT("location"), Location);
        bAny |= detail::ParseRotator3(Args, TEXT("rotation"), Rotation);
        bAny |= detail::ParseVector3(Args, TEXT("scale"), Scale);

        const TSharedPtr<FJsonObject>* TransformObj = nullptr;
        if (Args->TryGetObjectField(TEXT("transform"), TransformObj)
            && TransformObj && (*TransformObj).IsValid())
        {
            bAny |= detail::ParseVector3(*TransformObj, TEXT("location"), Location);
            bAny |= detail::ParseRotator3(*TransformObj, TEXT("rotation"), Rotation);
            bAny |= detail::ParseVector3(*TransformObj, TEXT("scale"), Scale);
        }
    }

    OutTransform = FTransform(Rotation, Location, Scale);
    return bAny;
}

TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
{
    auto O = MakeShared<FJsonObject>();
    O->SetObjectField(TEXT("location"), VectorToJson(Transform.GetLocation()));
    const FRotator Rot = Transform.Rotator();
    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("pitch"), Rot.Pitch);
    R->SetNumberField(TEXT("yaw"), Rot.Yaw);
    R->SetNumberField(TEXT("roll"), Rot.Roll);
    O->SetObjectField(TEXT("rotation"), R);
    O->SetObjectField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
    return O;
}

TSharedRef<FJsonObject> SnapshotReflectedProperties(UObject* Obj,
                                                    const TSet<FString>& PropertyNames,
                                                    int32 MaxProperties,
                                                    int32 MaxDepth)
{
    auto O = MakeShared<FJsonObject>();
    O->SetObjectField(TEXT("object"), ObjectSummary(Obj));
    O->SetBoolField(TEXT("allow_listed"), PropertyNames.Num() > 0);
    O->SetNumberField(TEXT("max_properties"), MaxProperties);
    if (!Obj)
    {
        O->SetNumberField(TEXT("property_count"), 0);
        return O;
    }

    auto Props = MakeShared<FJsonObject>();
    detail::FInstancedRecurseCtx Ctx;
    Ctx.MaxDepth = FMath::Clamp(MaxDepth, 0, 8);
    int32 Count = 0;
    int32 Matched = 0;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property)
        {
            continue;
        }
        if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
        {
            continue;
        }
        const FString Name = Property->GetName();
        if (!StringSetMatchesName(PropertyNames, Name))
        {
            continue;
        }
        ++Matched;
        if (Count >= MaxProperties)
        {
            continue;
        }
        if (TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Obj, Property, &Ctx))
        {
            Props->SetField(Name, Value);
            ++Count;
        }
    }
    O->SetObjectField(TEXT("properties"), Props);
    O->SetNumberField(TEXT("property_count"), Count);
    O->SetNumberField(TEXT("matched_property_count"), Matched);
    O->SetBoolField(TEXT("properties_capped"), Matched > Count);
    return O;
}

bool ComponentMatchesSnapshotFilter(UActorComponent* Component, const TSet<FString>& Filters)
{
    if (!Component)
    {
        return false;
    }
    if (Filters.Num() <= 0)
    {
        return true;
    }
    const FString Name = Component->GetName();
    const FString Path = Component->GetPathName();
    const FString ClassPath = Component->GetClass() ? Component->GetClass()->GetPathName() : FString();
    for (const FString& Filter : Filters)
    {
        if (Name == Filter
            || Path == Filter
            || ClassPath == Filter
            || Name.Contains(Filter, ESearchCase::IgnoreCase)
            || Path.Contains(Filter, ESearchCase::IgnoreCase)
            || ClassPath.Contains(Filter, ESearchCase::IgnoreCase))
        {
            return true;
        }
    }
    return false;
}

TSharedRef<FJsonObject> SnapshotPieActor(AActor* Actor,
                                         const TSet<FString>& ActorPropertyNames,
                                         const TSet<FString>& ComponentFilters,
                                         const TSet<FString>& ComponentPropertyNames,
                                         bool bIncludeActorProperties,
                                         bool bIncludeComponents,
                                         bool bIncludeComponentProperties,
                                         int32 MaxProperties,
                                         int32 MaxComponents,
                                         int32 MaxDepth)
{
    auto O = MakeShared<FJsonObject>();
    O->SetObjectField(TEXT("actor"), ObjectSummary(Actor));
    if (!Actor)
    {
        return O;
    }

    O->SetStringField(TEXT("label"), Actor->GetActorLabel());
    O->SetObjectField(TEXT("transform"), TransformToJson(Actor->GetActorTransform()));
    O->SetObjectField(TEXT("velocity"), VectorToJson(Actor->GetVelocity()));
    O->SetBoolField(TEXT("replicates"), Actor->GetIsReplicated());
    O->SetBoolField(TEXT("replicate_movement"), Actor->IsReplicatingMovement());
    O->SetBoolField(TEXT("pending_kill_or_destroy"), Actor->IsActorBeingDestroyed());

    if (bIncludeActorProperties)
    {
        O->SetObjectField(TEXT("property_snapshot"), SnapshotReflectedProperties(
            Actor,
            ActorPropertyNames,
            MaxProperties,
            MaxDepth));
    }

    if (bIncludeComponents)
    {
        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);

        TArray<TSharedPtr<FJsonValue>> ComponentJson;
        int32 MatchedComponents = 0;
        for (UActorComponent* Component : Components)
        {
            if (!Component || !ComponentMatchesSnapshotFilter(Component, ComponentFilters))
            {
                continue;
            }
            ++MatchedComponents;
            if (ComponentJson.Num() >= MaxComponents)
            {
                continue;
            }

            auto C = MakeShared<FJsonObject>();
            C->SetObjectField(TEXT("component"), ObjectSummary(Component));
            C->SetBoolField(TEXT("registered"), Component->IsRegistered());
            C->SetBoolField(TEXT("active"), Component->IsActive());
            if (USceneComponent* SceneComp = Cast<USceneComponent>(Component))
            {
                C->SetObjectField(TEXT("world_transform"), TransformToJson(SceneComp->GetComponentTransform()));
                C->SetObjectField(TEXT("relative_transform"), TransformToJson(SceneComp->GetRelativeTransform()));
            }
            if (bIncludeComponentProperties || ComponentPropertyNames.Num() > 0)
            {
                C->SetObjectField(TEXT("property_snapshot"), SnapshotReflectedProperties(
                    Component,
                    ComponentPropertyNames,
                    MaxProperties,
                    MaxDepth));
            }
            ComponentJson.Add(MakeShared<FJsonValueObject>(C));
        }
        O->SetArrayField(TEXT("components"), ComponentJson);
        O->SetNumberField(TEXT("component_count"), Components.Num());
        O->SetNumberField(TEXT("matched_component_count"), MatchedComponents);
        O->SetBoolField(TEXT("components_capped"), MatchedComponents > ComponentJson.Num());
    }

    return O;
}

bool ResolvePieWorldForSpawnSnapshot(const TSharedPtr<FJsonObject>& Args,
                                     FPieInputTarget& OutTarget,
                                     FString& OutError)
{
    if (ResolvePieInputTarget(Args, OutTarget, OutError))
    {
        return true;
    }

    FString RequestedWorld;
    FString RequestedController;
    FString RequestedPawn;
    ParseInputTargetArgs(Args, RequestedWorld, RequestedController, RequestedPawn);
    if (!RequestedController.IsEmpty() || !RequestedPawn.IsEmpty())
    {
        return false;
    }

    const TArray<UWorld*> CandidateWorlds = GetLivePieWorldCandidates();
    for (UWorld* World : CandidateWorlds)
    {
        if (World)
        {
            OutTarget.CandidateWorlds.Add(World->GetPathName());
        }
    }
    if (CandidateWorlds.Num() <= 0)
    {
        OutError = TEXT("no live PIE world active - start PIE first (run_pie)");
        return false;
    }

    OutTarget.LocalPlayerIndex = GetLocalPlayerIndexArg(Args);
    OutTarget.RequestedWorld = RequestedWorld;
    OutTarget.RequestedController = RequestedController;
    OutTarget.RequestedPawn = RequestedPawn;
    for (UWorld* World : CandidateWorlds)
    {
        if (!World)
        {
            continue;
        }
        if (!RequestedWorld.IsEmpty() && !ObjectMatchesInputIdentifier(World, RequestedWorld))
        {
            continue;
        }
        OutTarget.World = World;
        OutTarget.ResolutionSource = RequestedWorld.IsEmpty()
            ? TEXT("active_pie_world_context_without_local_player")
            : TEXT("explicit_world_without_local_player");
        OutTarget.bEditorPlayWorldMismatch = GEditor && GEditor->PlayWorld && GEditor->PlayWorld != World;
        OutTarget.ViewportClient = World->GetGameViewport();
        OutTarget.Viewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
        OutError.Empty();
        return true;
    }

    OutError = FString::Printf(TEXT("requested PIE world target was not resolved: %s"), *RequestedWorld);
    return false;
}

ESpawnActorCollisionHandlingMethod ParseCollisionHandlingMethod(const TSharedPtr<FJsonObject>& Args)
{
    FString CollisionHandling = TEXT("always_spawn");
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("collision_handling"), CollisionHandling);
        Args->TryGetStringField(TEXT("spawn_collision_handling"), CollisionHandling);
    }
    CollisionHandling = CollisionHandling.ToLower();
    if (CollisionHandling == TEXT("adjust_if_possible_but_always_spawn"))
    {
        return ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    }
    if (CollisionHandling == TEXT("adjust_if_possible_but_dont_spawn_if_colliding")
        || CollisionHandling == TEXT("adjust_if_possible_but_do_not_spawn_if_colliding"))
    {
        return ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
    }
    if (CollisionHandling == TEXT("dont_spawn_if_colliding")
        || CollisionHandling == TEXT("do_not_spawn_if_colliding"))
    {
        return ESpawnActorCollisionHandlingMethod::DontSpawnIfColliding;
    }
    return ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
}

FSageToolDispatch::FOutcome SpawnPieActorSnapshotImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }

    FString ClassPath;
    Args->TryGetStringField(TEXT("class"), ClassPath);
    Args->TryGetStringField(TEXT("class_path"), ClassPath);
    Args->TryGetStringField(TEXT("uclass"), ClassPath);

    FString ClassError;
    UClass* SpawnClass = ResolvePieSpawnActorClass(ClassPath, ClassError);
    if (!SpawnClass)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, ClassError);
    }

    FPieInputTarget Target;
    FString Error;
    if (!ResolvePieWorldForSpawnSnapshot(Args, Target, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004, Error);
    }

    FTransform SpawnTransform = FTransform::Identity;
    ParsePieSpawnTransform(Args, SpawnTransform);

    FActorSpawnParameters SpawnParams;
    SpawnParams.ObjectFlags |= RF_Transient;
    SpawnParams.SpawnCollisionHandlingOverride = ParseCollisionHandlingMethod(Args);

    AActor* Spawned = Target.World->SpawnActor<AActor>(SpawnClass, SpawnTransform, SpawnParams);
    if (!Spawned)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("PIE actor spawn failed for class: %s"), *SpawnClass->GetPathName()));
    }

    bool bReplicates = false;
    bool bReplicateMovement = false;
    bool bAlwaysRelevant = false;
    bool bNetLoadOnClient = false;
    const bool bHasReplicates = Args->TryGetBoolField(TEXT("replicates"), bReplicates);
    const bool bHasReplicateMovement = Args->TryGetBoolField(TEXT("replicate_movement"), bReplicateMovement);
    const bool bHasAlwaysRelevant = Args->TryGetBoolField(TEXT("always_relevant"), bAlwaysRelevant);
    const bool bHasNetLoadOnClient = Args->TryGetBoolField(TEXT("net_load_on_client"), bNetLoadOnClient);
    if (bHasReplicates)
    {
        Spawned->SetReplicates(bReplicates);
    }
    if (bHasReplicateMovement)
    {
        Spawned->SetReplicateMovement(bReplicateMovement);
    }
    if (bHasAlwaysRelevant)
    {
        Spawned->bAlwaysRelevant = bAlwaysRelevant;
    }
    if (bHasNetLoadOnClient)
    {
        Spawned->bNetLoadOnClient = bNetLoadOnClient;
    }

    bool bIncludeActorProperties = true;
    bool bIncludeComponents = true;
    bool bIncludeComponentProperties = false;
    bool bCleanupPythonRefs = true;
    bool bClearMainGlobals = true;
    double MaxPropertiesNum = 64.0;
    double MaxComponentsNum = 64.0;
    double MaxDepthNum = 2.0;
    Args->TryGetBoolField(TEXT("include_actor_properties"), bIncludeActorProperties);
    Args->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
    Args->TryGetBoolField(TEXT("include_component_properties"), bIncludeComponentProperties);
    Args->TryGetBoolField(TEXT("cleanup_python_refs"), bCleanupPythonRefs);
    Args->TryGetBoolField(TEXT("clear_python_main_globals"), bClearMainGlobals);
    Args->TryGetNumberField(TEXT("max_properties"), MaxPropertiesNum);
    Args->TryGetNumberField(TEXT("max_components"), MaxComponentsNum);
    Args->TryGetNumberField(TEXT("max_depth"), MaxDepthNum);

    const TSet<FString> ActorPropertyNames = ReadStringSetArg(Args, TEXT("properties"));
    const TSet<FString> ComponentFilters = ReadStringSetArg(Args, TEXT("components"));
    const TSet<FString> ComponentPropertyNames = ReadStringSetArg(Args, TEXT("component_properties"));

    TSharedRef<FJsonObject> Snapshot = SnapshotPieActor(
        Spawned,
        ActorPropertyNames,
        ComponentFilters,
        ComponentPropertyNames,
        bIncludeActorProperties || ActorPropertyNames.Num() > 0,
        bIncludeComponents,
        bIncludeComponentProperties,
        FMath::Clamp(static_cast<int32>(MaxPropertiesNum), 1, 500),
        FMath::Clamp(static_cast<int32>(MaxComponentsNum), 1, 500),
        FMath::Clamp(static_cast<int32>(MaxDepthNum), 0, 8));

    const FString SpawnedPath = Spawned->GetPathName();
    const bool bDestroyed = Spawned->Destroy();

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("target"), PieInputTargetToJson(Target));
    R->SetObjectField(TEXT("world"), ObjectSummary(Target.World));
    R->SetStringField(TEXT("requested_class"), ClassPath);
    R->SetStringField(TEXT("resolved_class"), SpawnClass->GetPathName());
    R->SetStringField(TEXT("spawned_actor_path"), SpawnedPath);
    R->SetObjectField(TEXT("spawn_transform"), TransformToJson(SpawnTransform));
    R->SetObjectField(TEXT("snapshot"), Snapshot);
    R->SetBoolField(TEXT("destroyed_after_snapshot"), bDestroyed);
    R->SetBoolField(TEXT("transient_spawn"), true);
    R->SetBoolField(TEXT("cleanup_python_refs_requested"), bCleanupPythonRefs);

    if (bCleanupPythonRefs)
    {
        const detail::FPythonReferenceCleanupReport Cleanup =
            detail::CleanupPythonReferences(
                bClearMainGlobals,
                /*bIncludePieWorlds=*/true,
                /*bIncludeEditorWorld=*/false,
                /*bCollectUnrealGarbage=*/true);
        if (Cleanup.bPythonCommandRan && !Cleanup.bPythonCommandSucceeded)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                FString::Printf(TEXT("Python reference cleanup failed after PIE spawn snapshot: %s"),
                                *Cleanup.Error));
        }
        AddPythonCleanupReport(R, Cleanup);
    }

    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

TArray<TSharedPtr<FJsonValue>> StringSetToJsonArray(const TSet<FString>& Values)
{
    TArray<FString> Sorted = Values.Array();
    Sorted.Sort();

    TArray<TSharedPtr<FJsonValue>> Out;
    Out.Reserve(Sorted.Num());
    for (const FString& Value : Sorted)
    {
        Out.Add(MakeShared<FJsonValueString>(Value));
    }
    return Out;
}

void AddGameplayTagsFromContainer(const FGameplayTagContainer& Container,
                                  TSet<FString>& OutTags)
{
    TArray<FGameplayTag> Tags;
    Container.GetGameplayTagArray(Tags);
    for (const FGameplayTag& Tag : Tags)
    {
        if (Tag.IsValid())
        {
            OutTags.Add(Tag.ToString());
        }
    }
}

void AddGameplayTagsFromProperty(const FProperty* Property,
                                 const void* ValuePtr,
                                 TSet<FString>& OutTags,
                                 int32 Depth = 0)
{
    if (!Property || !ValuePtr || Depth > 4)
    {
        return;
    }

    if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
    {
        if (!StructProp->Struct)
        {
            return;
        }
        const FName StructName = StructProp->Struct->GetFName();
        if (StructName == FName(TEXT("GameplayTagContainer")))
        {
            AddGameplayTagsFromContainer(
                *reinterpret_cast<const FGameplayTagContainer*>(ValuePtr),
                OutTags);
            return;
        }
        if (StructName == FName(TEXT("GameplayTag")))
        {
            const FGameplayTag& Tag = *reinterpret_cast<const FGameplayTag*>(ValuePtr);
            if (Tag.IsValid())
            {
                OutTags.Add(Tag.ToString());
            }
            return;
        }
        if (Property->GetName().Contains(TEXT("Tag")))
        {
            for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
            {
                const FProperty* Inner = *It;
                AddGameplayTagsFromProperty(Inner,
                    Inner->ContainerPtrToValuePtr<void>(ValuePtr),
                    OutTags,
                    Depth + 1);
            }
        }
        return;
    }

    if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Helper(ArrayProp, ValuePtr);
        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            AddGameplayTagsFromProperty(ArrayProp->Inner, Helper.GetRawPtr(i), OutTags, Depth + 1);
        }
        return;
    }

    if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper Helper(SetProp, ValuePtr);
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (Helper.IsValidIndex(i))
            {
                AddGameplayTagsFromProperty(SetProp->ElementProp, Helper.GetElementPtr(i), OutTags, Depth + 1);
            }
        }
        return;
    }

    if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper Helper(MapProp, ValuePtr);
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i)) continue;
            AddGameplayTagsFromProperty(MapProp->KeyProp, Helper.GetKeyPtr(i), OutTags, Depth + 1);
            AddGameplayTagsFromProperty(MapProp->ValueProp, Helper.GetValuePtr(i), OutTags, Depth + 1);
        }
    }
}

TSet<FString> CollectGameplayTagsFromObject(UObject* Obj)
{
    TSet<FString> Tags;
    if (!Obj)
    {
        return Tags;
    }
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        const FProperty* Property = *It;
        if (!Property || !Property->GetName().Contains(TEXT("Tag")))
        {
            continue;
        }
        AddGameplayTagsFromProperty(Property,
            Property->ContainerPtrToValuePtr<void>(Obj),
            Tags);
    }
    return Tags;
}

UClass* FindRuntimeClass(const TCHAR* Path)
{
    UClass* Cls = FindObject<UClass>(nullptr, Path);
    if (!Cls)
    {
        Cls = LoadObject<UClass>(nullptr, Path);
    }
    return Cls;
}

UActorComponent* FindComponentOfRuntimeClass(AActor* Actor, UClass* ComponentClass)
{
    if (!Actor || !ComponentClass)
    {
        return nullptr;
    }
    return Actor->FindComponentByClass(ComponentClass);
}

UObject* ResolveAbilitySystemComponent(AActor* Actor, const FPieInputTarget& Target)
{
    UClass* AscCls = FindRuntimeClass(TEXT("/Script/GameplayAbilities.AbilitySystemComponent"));
    if (!AscCls)
    {
        return nullptr;
    }

    auto TryActor = [AscCls](AActor* Candidate) -> UObject*
    {
        if (!Candidate) return nullptr;
        if (UActorComponent* Comp = FindComponentOfRuntimeClass(Candidate, AscCls))
        {
            return Comp;
        }
        return nullptr;
    };

    if (UObject* Asc = TryActor(Actor))
    {
        return Asc;
    }
    if (APawn* Pawn = Cast<APawn>(Actor))
    {
        if (UObject* Asc = TryActor(Pawn->GetPlayerState()))
        {
            return Asc;
        }
        if (UObject* Asc = TryActor(Pawn->GetController()))
        {
            return Asc;
        }
    }
    if (Target.PlayerController)
    {
        if (UObject* Asc = TryActor(Target.PlayerController))
        {
            return Asc;
        }
        if (UObject* Asc = TryActor(Target.PlayerController->GetPawn()))
        {
            return Asc;
        }
        if (UObject* Asc = TryActor(Target.PlayerController->PlayerState))
        {
            return Asc;
        }
    }
    return nullptr;
}

AActor* ResolveTraceActor(const TSharedPtr<FJsonObject>& Args,
                          const FPieInputTarget& Target,
                          FString& OutSource)
{
    FString ActorId;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("actor_id"), ActorId);
        Args->TryGetStringField(TEXT("actor"), ActorId);
        Args->TryGetStringField(TEXT("pawn"), ActorId);
    }
    if (!ActorId.IsEmpty())
    {
        OutSource = TEXT("actor_id");
        return detail::ResolveActor(ActorId);
    }
    if (Target.PlayerController && Target.PlayerController->GetPawn())
    {
        OutSource = TEXT("local_player_pawn");
        return Target.PlayerController->GetPawn();
    }
    OutSource = TEXT("unresolved");
    return nullptr;
}

TSharedRef<FJsonObject> SnapshotMovement(AActor* Actor, UWorld* World)
{
    auto O = MakeShared<FJsonObject>();
    O->SetObjectField(TEXT("actor"), ObjectSummary(Actor));
    if (!Actor)
    {
        return O;
    }

    O->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
    O->SetObjectField(TEXT("velocity"), VectorToJson(Actor->GetVelocity()));
    if (World)
    {
        O->SetNumberField(TEXT("world_time_seconds"), World->TimeSeconds);
        O->SetNumberField(TEXT("world_delta_seconds"), World->GetDeltaSeconds());
    }

    if (ACharacter* Character = Cast<ACharacter>(Actor))
    {
        O->SetBoolField(TEXT("b_pressed_jump"), Character->bPressedJump);
        if (UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement())
        {
            auto Cmc = MakeShared<FJsonObject>();
            UEnum* MovementModeEnum = StaticEnum<EMovementMode>();
            Cmc->SetStringField(TEXT("movement_mode"),
                MovementModeEnum
                    ? MovementModeEnum->GetNameStringByValue(MoveComp->MovementMode)
                    : FString::FromInt(static_cast<int32>(MoveComp->MovementMode)));
            Cmc->SetNumberField(TEXT("movement_mode_value"), static_cast<int32>(MoveComp->MovementMode));
            Cmc->SetNumberField(TEXT("custom_movement_mode"), MoveComp->CustomMovementMode);
            Cmc->SetBoolField(TEXT("is_falling"), MoveComp->IsFalling());
            Cmc->SetBoolField(TEXT("is_moving_on_ground"), MoveComp->IsMovingOnGround());
            Cmc->SetObjectField(TEXT("velocity"), VectorToJson(MoveComp->Velocity));
            O->SetObjectField(TEXT("character_movement"), Cmc);
        }
    }
    return O;
}

struct FAbilityTraceFilter
{
    FString Ability;
    FString AbilityClass;
    FString AbilityPath;
    FString InputTag;
    FString Tag;
    int32 InputId = 0;
    bool bHasInputId = false;
    int32 MaxSpecs = 100;
    bool bIncludeRaw = false;
    bool bTryActivate = false;
    bool bAllowRemoteActivation = true;
    bool bProcessAbilityInput = true;
    float ProcessDeltaTime = 1.0f / 60.0f;
    bool bGamePaused = false;
};

FAbilityTraceFilter ParseAbilityTraceFilter(const TSharedPtr<FJsonObject>& Args)
{
    FAbilityTraceFilter Filter;
    if (!Args.IsValid())
    {
        return Filter;
    }
    Args->TryGetStringField(TEXT("ability"), Filter.Ability);
    Args->TryGetStringField(TEXT("ability_name"), Filter.Ability);
    Args->TryGetStringField(TEXT("ability_class"), Filter.AbilityClass);
    Args->TryGetStringField(TEXT("ability_path"), Filter.AbilityPath);
    Args->TryGetStringField(TEXT("input_tag"), Filter.InputTag);
    Args->TryGetStringField(TEXT("tag"), Filter.Tag);
    Args->TryGetBoolField(TEXT("include_raw"), Filter.bIncludeRaw);
    Args->TryGetBoolField(TEXT("try_activate"), Filter.bTryActivate);
    Args->TryGetBoolField(TEXT("allow_remote_activation"), Filter.bAllowRemoteActivation);
    Args->TryGetBoolField(TEXT("process_ability_input"), Filter.bProcessAbilityInput);
    Args->TryGetBoolField(TEXT("game_paused"), Filter.bGamePaused);

    double Num = 0.0;
    if (Args->TryGetNumberField(TEXT("input_id"), Num))
    {
        Filter.InputId = static_cast<int32>(Num);
        Filter.bHasInputId = true;
    }
    if (Args->TryGetNumberField(TEXT("max_specs"), Num))
    {
        Filter.MaxSpecs = FMath::Clamp(static_cast<int32>(Num), 1, 500);
    }
    if (Args->TryGetNumberField(TEXT("delta_time"), Num)
        || Args->TryGetNumberField(TEXT("process_delta_time"), Num))
    {
        Filter.ProcessDeltaTime = FMath::Max(0.0f, static_cast<float>(Num));
    }
    return Filter;
}

bool ContainsTraceNeedle(const FString& Haystack, const FString& Needle)
{
    return Needle.IsEmpty() || Haystack.Contains(Needle, ESearchCase::IgnoreCase);
}

bool SpecMatchesFilter(const FAbilityTraceFilter& Filter,
                       const FString& AbilityName,
                       const FString& AbilityPath,
                       const FString& AbilityClass,
                       int32 InputId,
                       bool bHasInputId,
                       const TSet<FString>& Tags)
{
    const FString Combined = AbilityName + TEXT(" ") + AbilityPath + TEXT(" ") + AbilityClass;
    if (!ContainsTraceNeedle(Combined, Filter.Ability))
    {
        return false;
    }
    if (!ContainsTraceNeedle(AbilityPath, Filter.AbilityPath))
    {
        return false;
    }
    if (!ContainsTraceNeedle(AbilityClass, Filter.AbilityClass))
    {
        return false;
    }
    if (Filter.bHasInputId && (!bHasInputId || InputId != Filter.InputId))
    {
        return false;
    }
    auto HasTagNeedle = [&Tags](const FString& Needle) -> bool
    {
        if (Needle.IsEmpty()) return true;
        for (const FString& Tag : Tags)
        {
            if (Tag.Contains(Needle, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    };
    return HasTagNeedle(Filter.InputTag) && HasTagNeedle(Filter.Tag);
}

TSharedRef<FJsonObject> TryActivateAbilityByHandle(UObject* Asc,
                                                   const FProperty* HandleProperty,
                                                   const void* HandlePtr,
                                                   bool bAllowRemoteActivation)
{
    auto O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("attempted"), false);
    O->SetBoolField(TEXT("callable"), false);

    if (!Asc || !HandleProperty || !HandlePtr)
    {
        O->SetStringField(TEXT("reason"), TEXT("missing ASC or ability spec handle"));
        return O;
    }

    UFunction* Fn = Asc->FindFunction(TEXT("TryActivateAbility"));
    if (!Fn)
    {
        O->SetStringField(TEXT("reason"), TEXT("ASC TryActivateAbility is not reflected"));
        return O;
    }

    TArray<uint8> Params;
    Params.SetNumZeroed(Fn->ParmsSize);
    bool bHandleSet = false;
    FBoolProperty* ReturnBool = nullptr;

    for (TFieldIterator<FProperty> It(Fn); It; ++It)
    {
        FProperty* Param = *It;
        if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm))
        {
            continue;
        }
        void* ParamPtr = Param->ContainerPtrToValuePtr<void>(Params.GetData());
        if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            ReturnBool = CastField<FBoolProperty>(Param);
            continue;
        }
        if (FBoolProperty* BoolParam = CastField<FBoolProperty>(Param))
        {
            BoolParam->SetPropertyValue(ParamPtr, bAllowRemoteActivation);
            continue;
        }
        if (Param->SameType(HandleProperty)
            || Param->GetName().Contains(TEXT("AbilityToActivate"))
            || Param->GetCPPType().Contains(TEXT("GameplayAbilitySpecHandle")))
        {
            Param->CopyCompleteValue(ParamPtr, HandlePtr);
            bHandleSet = true;
        }
    }

    if (!bHandleSet)
    {
        O->SetStringField(TEXT("reason"), TEXT("could not bind GameplayAbilitySpecHandle parameter"));
        return O;
    }

    Asc->ProcessEvent(Fn, Params.GetData());
    O->SetBoolField(TEXT("attempted"), true);
    O->SetBoolField(TEXT("callable"), true);
    O->SetStringField(TEXT("function"), Fn->GetName());
    if (ReturnBool)
    {
        O->SetBoolField(TEXT("result"), ReturnBool->GetPropertyValue(
            ReturnBool->ContainerPtrToValuePtr<void>(Params.GetData())));
    }
    return O;
}

TSharedRef<FJsonObject> ProcessAbilityInputIfReflected(UObject* Asc,
                                                       float DeltaTime,
                                                       bool bGamePaused)
{
    auto O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("attempted"), false);
    O->SetBoolField(TEXT("callable"), false);
    if (!Asc)
    {
        O->SetStringField(TEXT("reason"), TEXT("ASC unavailable"));
        return O;
    }

    UFunction* Fn = Asc->FindFunction(TEXT("ProcessAbilityInput"));
    if (!Fn)
    {
        O->SetStringField(TEXT("reason"), TEXT("ASC ProcessAbilityInput is not reflected"));
        return O;
    }

    TArray<uint8> Params;
    Params.SetNumZeroed(Fn->ParmsSize);
    for (TFieldIterator<FProperty> It(Fn); It; ++It)
    {
        FProperty* Param = *It;
        if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm)
            || Param->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            continue;
        }
        void* ParamPtr = Param->ContainerPtrToValuePtr<void>(Params.GetData());
        if (FFloatProperty* FloatParam = CastField<FFloatProperty>(Param))
        {
            FloatParam->SetPropertyValue(ParamPtr, DeltaTime);
        }
        else if (FDoubleProperty* DoubleParam = CastField<FDoubleProperty>(Param))
        {
            DoubleParam->SetPropertyValue(ParamPtr, DeltaTime);
        }
        else if (FBoolProperty* BoolParam = CastField<FBoolProperty>(Param))
        {
            BoolParam->SetPropertyValue(ParamPtr, bGamePaused);
        }
    }

    Asc->ProcessEvent(Fn, Params.GetData());
    O->SetBoolField(TEXT("attempted"), true);
    O->SetBoolField(TEXT("callable"), true);
    O->SetStringField(TEXT("function"), Fn->GetName());
    O->SetNumberField(TEXT("delta_time"), DeltaTime);
    O->SetBoolField(TEXT("game_paused"), bGamePaused);
    return O;
}

FArrayProperty* FindItemsArrayProperty(FStructProperty* ContainerProp)
{
    if (!ContainerProp || !ContainerProp->Struct)
    {
        return nullptr;
    }
    if (FArrayProperty* Items = FindFProperty<FArrayProperty>(ContainerProp->Struct, TEXT("Items")))
    {
        return Items;
    }
    for (TFieldIterator<FArrayProperty> It(ContainerProp->Struct); It; ++It)
    {
        if ((*It)->GetName().Contains(TEXT("Item"), ESearchCase::IgnoreCase))
        {
            return *It;
        }
    }
    return nullptr;
}

TArray<TSharedPtr<FJsonValue>> BuildAbilitySpecSummaries(UObject* Asc,
                                                         const FAbilityTraceFilter& Filter,
                                                         bool bTryActivateMatches,
                                                         int32& OutTotalSpecs,
                                                         int32& OutMatchedSpecs)
{
    OutTotalSpecs = 0;
    OutMatchedSpecs = 0;
    TArray<TSharedPtr<FJsonValue>> Out;
    if (!Asc)
    {
        return Out;
    }

    FProperty* ActivatableProp = FindFProperty<FProperty>(Asc->GetClass(), TEXT("ActivatableAbilities"));
    FStructProperty* ActivatableStructProp = CastField<FStructProperty>(ActivatableProp);
    if (!ActivatableStructProp)
    {
        return Out;
    }

    void* ActivatablePtr = ActivatableStructProp->ContainerPtrToValuePtr<void>(Asc);
    FArrayProperty* ItemsProp = FindItemsArrayProperty(ActivatableStructProp);
    if (!ItemsProp)
    {
        return Out;
    }

    void* ItemsPtr = ItemsProp->ContainerPtrToValuePtr<void>(ActivatablePtr);
    FScriptArrayHelper Helper(ItemsProp, ItemsPtr);
    OutTotalSpecs = Helper.Num();

    FStructProperty* SpecStructProp = CastField<FStructProperty>(ItemsProp->Inner);
    if (!SpecStructProp || !SpecStructProp->Struct)
    {
        return Out;
    }

    for (int32 i = 0; i < Helper.Num(); ++i)
    {
        void* SpecPtr = Helper.GetRawPtr(i);
        if (!SpecPtr)
        {
            continue;
        }

        auto SpecObj = MakeShared<FJsonObject>();
        SpecObj->SetNumberField(TEXT("index"), i);

        FString AbilityName;
        FString AbilityPath;
        FString AbilityClass;
        int32 InputId = 0;
        bool bHasInputId = false;
        TSet<FString> Tags;
        const FProperty* HandleProperty = nullptr;
        const void* HandlePtr = nullptr;

        auto FieldsObj = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> It(SpecStructProp->Struct); It; ++It)
        {
            FProperty* Field = *It;
            if (!Field)
            {
                continue;
            }
            const FString FieldName = Field->GetName();
            void* FieldPtr = Field->ContainerPtrToValuePtr<void>(SpecPtr);

            if (FieldName == TEXT("Handle"))
            {
                HandleProperty = Field;
                HandlePtr = FieldPtr;
            }
            if (FieldName == TEXT("InputID"))
            {
                if (const FIntProperty* IntProp = CastField<FIntProperty>(Field))
                {
                    InputId = IntProp->GetPropertyValue(FieldPtr);
                    bHasInputId = true;
                }
            }
            if (FieldName.Contains(TEXT("Tag")))
            {
                AddGameplayTagsFromProperty(Field, FieldPtr, Tags);
            }
            if (const FObjectProperty* ObjectProp = CastField<FObjectProperty>(Field);
                FieldName == TEXT("Ability") && ObjectProp)
            {
                if (UObject* Ability = ObjectProp->GetObjectPropertyValue(FieldPtr))
                {
                    AbilityName = Ability->GetName();
                    AbilityPath = Ability->GetPathName();
                    AbilityClass = Ability->GetClass() ? Ability->GetClass()->GetPathName() : FString();
                    SpecObj->SetObjectField(TEXT("ability"), ObjectSummary(Ability));

                    const TSet<FString> AbilityTags = CollectGameplayTagsFromObject(Ability);
                    Tags.Append(AbilityTags);

                    auto AbilityTagProps = MakeShared<FJsonObject>();
                    for (TFieldIterator<FProperty> TagIt(Ability->GetClass()); TagIt; ++TagIt)
                    {
                        FProperty* TagProp = *TagIt;
                        if (!TagProp || !TagProp->GetName().Contains(TEXT("Tag")))
                        {
                            continue;
                        }
                        TSharedPtr<FJsonValue> TagValue = detail::GetUPropertyAsJson(Ability, TagProp);
                        if (TagValue.IsValid())
                        {
                            AbilityTagProps->SetField(TagProp->GetName(), TagValue);
                        }
                    }
                    SpecObj->SetObjectField(TEXT("ability_tag_properties"), AbilityTagProps);
                }
            }

            const bool bKeepField =
                FieldName == TEXT("Ability")
                || FieldName == TEXT("Handle")
                || FieldName == TEXT("Level")
                || FieldName == TEXT("InputID")
                || FieldName == TEXT("InputPressed")
                || FieldName == TEXT("ActiveCount")
                || FieldName == TEXT("ActivationInfo")
                || FieldName == TEXT("DynamicAbilityTags")
                || FieldName.Contains(TEXT("Input"))
                || FieldName.Contains(TEXT("Active"))
                || FieldName.Contains(TEXT("Activation"))
                || FieldName.Contains(TEXT("Handle"))
                || FieldName.Contains(TEXT("Tag"));
            if (bKeepField)
            {
                TSharedPtr<FJsonValue> Value = detail::GetPropertyValueAtPtr(Field, FieldPtr);
                if (Value.IsValid())
                {
                    FieldsObj->SetField(FieldName, Value);
                }
            }
        }

        const bool bMatches = SpecMatchesFilter(Filter, AbilityName, AbilityPath,
            AbilityClass, InputId, bHasInputId, Tags);
        if (bMatches)
        {
            ++OutMatchedSpecs;
        }
        if (!bMatches)
        {
            continue;
        }
        if (Out.Num() >= Filter.MaxSpecs)
        {
            continue;
        }

        SpecObj->SetObjectField(TEXT("fields"), FieldsObj);
        SpecObj->SetArrayField(TEXT("tag_names"), StringSetToJsonArray(Tags));
        SpecObj->SetBoolField(TEXT("matched_filter"), true);
        if (bHasInputId)
        {
            SpecObj->SetNumberField(TEXT("input_id"), InputId);
        }
        if (bTryActivateMatches)
        {
            SpecObj->SetObjectField(TEXT("try_activate"), TryActivateAbilityByHandle(
                Asc,
                HandleProperty,
                HandlePtr,
                Filter.bAllowRemoteActivation));
        }
        Out.Add(MakeShared<FJsonValueObject>(SpecObj));
    }

    return Out;
}

TSharedRef<FJsonObject> SnapshotAbilitySystem(UObject* Asc,
                                              const FAbilityTraceFilter& Filter,
                                              bool bTryActivateMatches)
{
    auto O = MakeShared<FJsonObject>();
    O->SetObjectField(TEXT("asc"), ObjectSummary(Asc));
    if (!Asc)
    {
        O->SetBoolField(TEXT("gas_available"), false);
        O->SetStringField(TEXT("reason"), TEXT("AbilitySystemComponent not found on pawn/player state/controller"));
        return O;
    }
    O->SetBoolField(TEXT("gas_available"), true);

    TSet<FString> OwnedTags = CollectGameplayTagsFromObject(Asc);
    if (UFunction* GetOwnedTagsFn = Asc->FindFunction(TEXT("GetOwnedGameplayTags")))
    {
        struct FGetOwnedGameplayTagsParams
        {
            FGameplayTagContainer TagContainer;
        };
        FGetOwnedGameplayTagsParams Params;
        Asc->ProcessEvent(GetOwnedTagsFn, &Params);
        AddGameplayTagsFromContainer(Params.TagContainer, OwnedTags);
        O->SetBoolField(TEXT("get_owned_gameplay_tags_callable"), true);
    }
    else
    {
        O->SetBoolField(TEXT("get_owned_gameplay_tags_callable"), false);
    }
    O->SetArrayField(TEXT("owned_gameplay_tags"), StringSetToJsonArray(OwnedTags));

    auto InputHandles = MakeShared<FJsonObject>();
    auto ActivationDiagnostics = MakeShared<FJsonObject>();
    auto TagProperties = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Asc->GetClass()); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property)
        {
            continue;
        }
        const FString Name = Property->GetName();
        TSharedPtr<FJsonValue> Value;
        const bool bLooksLikeInputHandle =
            Name.Contains(TEXT("Input"))
            && (Name.Contains(TEXT("SpecHandle"))
                || Name.Contains(TEXT("Pressed"))
                || Name.Contains(TEXT("Held"))
                || Name.Contains(TEXT("Released")));
        const bool bLooksLikeActivationState =
            Name.Contains(TEXT("Failure"))
            || Name.Contains(TEXT("Failed"))
            || Name.Contains(TEXT("Activation"))
            || Name.Contains(TEXT("Prediction"))
            || Name.Contains(TEXT("Inhibit"))
            || Name.Contains(TEXT("Blocked"));
        const bool bLooksLikeTag = Name.Contains(TEXT("Tag"));

        if (bLooksLikeInputHandle || bLooksLikeActivationState || bLooksLikeTag)
        {
            Value = detail::GetUPropertyAsJson(Asc, Property);
        }
        if (bLooksLikeInputHandle && Value.IsValid())
        {
            InputHandles->SetField(Name, Value);
        }
        if (bLooksLikeActivationState && Value.IsValid())
        {
            ActivationDiagnostics->SetField(Name, Value);
        }
        if (bLooksLikeTag && Value.IsValid())
        {
            TagProperties->SetField(Name, Value);
        }
    }
    O->SetObjectField(TEXT("input_spec_handles"), InputHandles);
    O->SetObjectField(TEXT("activation_diagnostics"), ActivationDiagnostics);
    O->SetObjectField(TEXT("tag_properties"), TagProperties);

    int32 TotalSpecs = 0;
    int32 MatchedSpecs = 0;
    O->SetArrayField(TEXT("ability_specs"), BuildAbilitySpecSummaries(
        Asc,
        Filter,
        bTryActivateMatches,
        TotalSpecs,
        MatchedSpecs));
    O->SetNumberField(TEXT("ability_spec_count"), TotalSpecs);
    O->SetNumberField(TEXT("matched_ability_spec_count"), MatchedSpecs);
    O->SetBoolField(TEXT("ability_specs_capped"), MatchedSpecs > Filter.MaxSpecs);

    if (Filter.bIncludeRaw)
    {
        if (FProperty* ActivatableProp = FindFProperty<FProperty>(Asc->GetClass(), TEXT("ActivatableAbilities")))
        {
            if (TSharedPtr<FJsonValue> Raw = detail::GetUPropertyAsJson(Asc, ActivatableProp))
            {
                O->SetField(TEXT("activatable_abilities_raw"), Raw);
            }
        }
        if (FProperty* EffectsProp = FindFProperty<FProperty>(Asc->GetClass(), TEXT("ActiveGameplayEffects")))
        {
            if (TSharedPtr<FJsonValue> Raw = detail::GetUPropertyAsJson(Asc, EffectsProp))
            {
                O->SetField(TEXT("active_gameplay_effects_raw"), Raw);
            }
        }
    }
    return O;
}

const TCHAR* TriggerEventToString(ETriggerEvent Event)
{
    switch (Event)
    {
    case ETriggerEvent::Started:   return TEXT("Started");
    case ETriggerEvent::Ongoing:   return TEXT("Ongoing");
    case ETriggerEvent::Triggered: return TEXT("Triggered");
    case ETriggerEvent::Canceled:  return TEXT("Canceled");
    case ETriggerEvent::Completed: return TEXT("Completed");
    default:                       return TEXT("None");
    }
}

TSharedRef<FJsonObject> CallGameplayTagFunctionIfReflected(UObject* Obj,
                                                           const TCHAR* FunctionName,
                                                           const FGameplayTag& InputTag)
{
    auto O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("attempted"), false);
    O->SetBoolField(TEXT("callable"), false);
    O->SetStringField(TEXT("function"), FunctionName ? FunctionName : TEXT(""));
    if (!Obj)
    {
        O->SetStringField(TEXT("reason"), TEXT("target object unavailable"));
        return O;
    }

    UFunction* Fn = Obj->FindFunction(FunctionName);
    if (!Fn)
    {
        O->SetStringField(TEXT("reason"), TEXT("function is not reflected"));
        return O;
    }

    TArray<uint8> Params;
    Params.SetNumZeroed(Fn->ParmsSize);
    bool bTagSet = false;
    for (TFieldIterator<FProperty> It(Fn); It; ++It)
    {
        FProperty* Param = *It;
        if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm)
            || Param->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            continue;
        }
        void* ParamPtr = Param->ContainerPtrToValuePtr<void>(Params.GetData());
        if (FStructProperty* StructParam = CastField<FStructProperty>(Param))
        {
            if (StructParam->Struct && StructParam->Struct->GetFName() == FName(TEXT("GameplayTag")))
            {
                StructParam->CopyCompleteValue(ParamPtr, &InputTag);
                bTagSet = true;
            }
        }
        else if (FBoolProperty* BoolParam = CastField<FBoolProperty>(Param))
        {
            BoolParam->SetPropertyValue(ParamPtr, false);
        }
    }

    if (!bTagSet)
    {
        O->SetStringField(TEXT("reason"), TEXT("could not bind GameplayTag parameter"));
        return O;
    }

    Obj->ProcessEvent(Fn, Params.GetData());
    O->SetBoolField(TEXT("attempted"), true);
    O->SetBoolField(TEXT("callable"), true);
    O->SetStringField(TEXT("object"), Obj->GetPathName());
    return O;
}

UInputAction* CallFindAbilityInputActionForTag(UObject* Obj,
                                               const FGameplayTag& InputTag,
                                               FString& OutSource,
                                               TArray<TSharedPtr<FJsonValue>>& Diagnostics)
{
    if (!Obj)
    {
        return nullptr;
    }
    UFunction* Fn = Obj->FindFunction(TEXT("FindAbilityInputActionForTag"));
    if (!Fn)
    {
        return nullptr;
    }

    auto D = MakeShared<FJsonObject>();
    D->SetObjectField(TEXT("object"), ObjectSummary(Obj));
    D->SetStringField(TEXT("function"), Fn->GetName());

    TArray<uint8> Params;
    Params.SetNumZeroed(Fn->ParmsSize);
    bool bTagSet = false;
    FObjectPropertyBase* ReturnObjectParam = nullptr;
    void* ReturnObjectPtr = nullptr;
    for (TFieldIterator<FProperty> It(Fn); It; ++It)
    {
        FProperty* Param = *It;
        if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm))
        {
            continue;
        }
        void* ParamPtr = Param->ContainerPtrToValuePtr<void>(Params.GetData());
        if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            ReturnObjectParam = CastField<FObjectPropertyBase>(Param);
            ReturnObjectPtr = ParamPtr;
            continue;
        }
        if (FStructProperty* StructParam = CastField<FStructProperty>(Param))
        {
            if (StructParam->Struct && StructParam->Struct->GetFName() == FName(TEXT("GameplayTag")))
            {
                StructParam->CopyCompleteValue(ParamPtr, &InputTag);
                bTagSet = true;
            }
        }
        else if (FBoolProperty* BoolParam = CastField<FBoolProperty>(Param))
        {
            BoolParam->SetPropertyValue(ParamPtr, false);
        }
    }

    if (!bTagSet || !ReturnObjectParam || !ReturnObjectPtr)
    {
        D->SetBoolField(TEXT("callable"), false);
        D->SetStringField(TEXT("reason"), TEXT("unexpected FindAbilityInputActionForTag signature"));
        Diagnostics.Add(MakeShared<FJsonValueObject>(D));
        return nullptr;
    }

    Obj->ProcessEvent(Fn, Params.GetData());
    UObject* Returned = ReturnObjectParam->GetObjectPropertyValue(ReturnObjectPtr);
    UInputAction* Action = Cast<UInputAction>(Returned);
    D->SetBoolField(TEXT("callable"), true);
    D->SetObjectField(TEXT("action"), ObjectSummary(Action));
    Diagnostics.Add(MakeShared<FJsonValueObject>(D));
    if (Action)
    {
        OutSource = FString::Printf(TEXT("%s.FindAbilityInputActionForTag"), *Obj->GetPathName());
    }
    return Action;
}

UInputAction* FindInputActionInInputActionStruct(void* StructPtr,
                                                UStruct* Struct,
                                                const FString& InputTagString)
{
    if (!StructPtr || !Struct)
    {
        return nullptr;
    }

    UInputAction* CandidateAction = nullptr;
    bool bTagMatches = false;
    for (TFieldIterator<FProperty> It(Struct); It; ++It)
    {
        FProperty* Field = *It;
        if (!Field)
        {
            continue;
        }
        void* FieldPtr = Field->ContainerPtrToValuePtr<void>(StructPtr);
        const FString FieldName = Field->GetName();
        if (FieldName.Contains(TEXT("Tag")))
        {
            TSet<FString> Tags;
            AddGameplayTagsFromProperty(Field, FieldPtr, Tags);
            bTagMatches = bTagMatches || Tags.Contains(InputTagString);
        }
        if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Field))
        {
            UObject* Obj = ObjectProp->GetObjectPropertyValue(FieldPtr);
            if (UInputAction* Action = Cast<UInputAction>(Obj))
            {
                CandidateAction = Action;
            }
        }
    }
    return bTagMatches ? CandidateAction : nullptr;
}

UInputAction* FindInputActionByTagRecursive(UObject* Obj,
                                            const FGameplayTag& InputTag,
                                            const FString& InputTagString,
                                            int32 Depth,
                                            TSet<const UObject*>& Visited,
                                            FString& OutSource,
                                            TArray<TSharedPtr<FJsonValue>>& Diagnostics)
{
    if (!Obj || Depth > 4 || Visited.Contains(Obj))
    {
        return nullptr;
    }
    Visited.Add(Obj);

    if (UInputAction* Action = CallFindAbilityInputActionForTag(Obj, InputTag, OutSource, Diagnostics))
    {
        return Action;
    }

    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property)
        {
            continue;
        }
        const FString Name = Property->GetName();
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Obj);

        if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
        {
            FStructProperty* StructProp = CastField<FStructProperty>(ArrayProp->Inner);
            if (!StructProp || !StructProp->Struct)
            {
                continue;
            }
            FScriptArrayHelper Helper(ArrayProp, ValuePtr);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                if (UInputAction* Action = FindInputActionInInputActionStruct(
                        Helper.GetRawPtr(Index),
                        StructProp->Struct,
                        InputTagString))
                {
                    OutSource = FString::Printf(TEXT("%s.%s[%d]"), *Obj->GetPathName(), *Name, Index);
                    auto D = MakeShared<FJsonObject>();
                    D->SetStringField(TEXT("source"), OutSource);
                    D->SetObjectField(TEXT("object"), ObjectSummary(Obj));
                    D->SetObjectField(TEXT("action"), ObjectSummary(Action));
                    Diagnostics.Add(MakeShared<FJsonValueObject>(D));
                    return Action;
                }
            }
        }

        FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property);
        if (!ObjectProp)
        {
            continue;
        }
        UObject* Child = ObjectProp->GetObjectPropertyValue(ValuePtr);
        if (!Child)
        {
            continue;
        }
        const FString ChildClass = Child->GetClass() ? Child->GetClass()->GetName() : FString();
        const bool bLooksRelevant =
            Name.Contains(TEXT("Input"), ESearchCase::IgnoreCase)
            || Name.Contains(TEXT("Config"), ESearchCase::IgnoreCase)
            || Name.Contains(TEXT("PawnData"), ESearchCase::IgnoreCase)
            || Name.Contains(TEXT("Data"), ESearchCase::IgnoreCase)
            || ChildClass.Contains(TEXT("Input"), ESearchCase::IgnoreCase)
            || Child->FindFunction(TEXT("FindAbilityInputActionForTag")) != nullptr;
        if (!bLooksRelevant)
        {
            continue;
        }
        if (UInputAction* Action = FindInputActionByTagRecursive(
                Child,
                InputTag,
                InputTagString,
                Depth + 1,
                Visited,
                OutSource,
                Diagnostics))
        {
            return Action;
        }
    }
    return nullptr;
}

void AddActorAndComponentRoots(AActor* Actor, TArray<UObject*>& Roots)
{
    if (!Actor)
    {
        return;
    }
    Roots.AddUnique(Actor);
    if (APawn* Pawn = Cast<APawn>(Actor))
    {
        if (Pawn->InputComponent)
        {
            Roots.AddUnique(Pawn->InputComponent);
        }
    }
    if (APlayerController* PlayerController = Cast<APlayerController>(Actor))
    {
        if (PlayerController->InputComponent)
        {
            Roots.AddUnique(PlayerController->InputComponent);
        }
    }
    TArray<UActorComponent*> Components;
    Actor->GetComponents(Components);
    for (UActorComponent* Component : Components)
    {
        Roots.AddUnique(Component);
    }
}

UInputAction* FindInputActionForInputTag(const FPieInputTarget& Target,
                                         AActor* Actor,
                                         const FGameplayTag& InputTag,
                                         FString& OutSource,
                                         TArray<TSharedPtr<FJsonValue>>& Diagnostics)
{
    const FString InputTagString = InputTag.ToString();
    TArray<UObject*> Roots;
    AddActorAndComponentRoots(Actor, Roots);
    if (APawn* Pawn = Cast<APawn>(Actor))
    {
        AddActorAndComponentRoots(Pawn->GetPlayerState(), Roots);
        AddActorAndComponentRoots(Cast<AActor>(Pawn->GetController()), Roots);
    }
    AddActorAndComponentRoots(Target.PlayerController, Roots);
    AddActorAndComponentRoots(Target.PlayerController ? Target.PlayerController->GetPawn() : nullptr, Roots);
    AddActorAndComponentRoots(Target.PlayerController ? Target.PlayerController->PlayerState : nullptr, Roots);

    for (UObject* Root : Roots)
    {
        TSet<const UObject*> Visited;
        if (UInputAction* Action = FindInputActionByTagRecursive(
                Root,
                InputTag,
                InputTagString,
                0,
                Visited,
                OutSource,
                Diagnostics))
        {
            return Action;
        }
    }
    return nullptr;
}

TSharedRef<FJsonObject> ExecuteEnhancedInputActionBindings(APlayerController* PlayerController,
                                                           const UInputAction* Action,
                                                           ETriggerEvent TriggerEvent)
{
    auto O = MakeShared<FJsonObject>();
    O->SetObjectField(TEXT("action"), ObjectSummary(const_cast<UInputAction*>(Action)));
    O->SetStringField(TEXT("trigger_event"), TriggerEventToString(TriggerEvent));
    O->SetBoolField(TEXT("attempted"), false);
    O->SetNumberField(TEXT("checked_binding_count"), 0);
    O->SetNumberField(TEXT("executed_binding_count"), 0);
    if (!PlayerController)
    {
        O->SetStringField(TEXT("reason"), TEXT("PlayerController unavailable"));
        return O;
    }
    if (!Action)
    {
        O->SetStringField(TEXT("reason"), TEXT("InputAction unavailable for input_tag"));
        return O;
    }

    TArray<UInputComponent*> Components;
    if (PlayerController->InputComponent)
    {
        Components.AddUnique(PlayerController->InputComponent);
    }
    if (APawn* Pawn = PlayerController->GetPawn())
    {
        if (Pawn->InputComponent)
        {
            Components.AddUnique(Pawn->InputComponent);
        }
    }

    TArray<TSharedPtr<FJsonValue>> ComponentJson;
    TArray<TSharedPtr<FJsonValue>> ExecutedJson;
    int32 CheckedBindings = 0;
    int32 ExecutedBindings = 0;
    for (UInputComponent* Component : Components)
    {
        auto C = MakeShared<FJsonObject>();
        C->SetObjectField(TEXT("component"), ObjectSummary(Component));
        UEnhancedInputComponent* EnhancedComponent = Cast<UEnhancedInputComponent>(Component);
        C->SetBoolField(TEXT("enhanced_input_component"), EnhancedComponent != nullptr);
        int32 ComponentChecked = 0;
        int32 ComponentExecuted = 0;
        if (EnhancedComponent)
        {
            FInputActionInstance ActionInstance(Action);
            const TArray<TUniquePtr<FEnhancedInputActionEventBinding>>& Bindings =
                EnhancedComponent->GetActionEventBindings();
            for (const TUniquePtr<FEnhancedInputActionEventBinding>& BindingPtr : Bindings)
            {
                if (!BindingPtr.IsValid())
                {
                    continue;
                }
                const FEnhancedInputActionEventBinding* Binding = BindingPtr.Get();
                if (Binding->GetAction() != Action)
                {
                    continue;
                }
                ++CheckedBindings;
                ++ComponentChecked;
                if (Binding->GetTriggerEvent() != TriggerEvent)
                {
                    continue;
                }
                Binding->Execute(ActionInstance);
                ++ExecutedBindings;
                ++ComponentExecuted;

                auto B = MakeShared<FJsonObject>();
                B->SetNumberField(TEXT("handle"), Binding->GetHandle());
                B->SetStringField(TEXT("trigger_event"), TriggerEventToString(Binding->GetTriggerEvent()));
                B->SetObjectField(TEXT("bound_object"), ObjectSummary(Binding->GetUObject()));
                B->SetObjectField(TEXT("component"), ObjectSummary(Component));
                ExecutedJson.Add(MakeShared<FJsonValueObject>(B));
            }
        }
        C->SetNumberField(TEXT("checked_binding_count"), ComponentChecked);
        C->SetNumberField(TEXT("executed_binding_count"), ComponentExecuted);
        ComponentJson.Add(MakeShared<FJsonValueObject>(C));
    }

    O->SetBoolField(TEXT("attempted"), true);
    O->SetArrayField(TEXT("input_components"), ComponentJson);
    O->SetArrayField(TEXT("executed_bindings"), ExecutedJson);
    O->SetNumberField(TEXT("input_component_count"), Components.Num());
    O->SetNumberField(TEXT("checked_binding_count"), CheckedBindings);
    O->SetNumberField(TEXT("executed_binding_count"), ExecutedBindings);
    O->SetBoolField(TEXT("executed"), ExecutedBindings > 0);
    if (ExecutedBindings <= 0)
    {
        O->SetStringField(TEXT("reason"),
            TEXT("no Enhanced Input action binding matched the resolved InputAction and trigger event"));
    }
    return O;
}

TSharedRef<FJsonObject> PostProcessPlayerInput(APlayerController* PlayerController,
                                               float DeltaTime,
                                               bool bGamePaused)
{
    auto O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("attempted"), false);
    if (!PlayerController)
    {
        O->SetStringField(TEXT("reason"), TEXT("PlayerController unavailable"));
        return O;
    }
    PlayerController->PostProcessInput(DeltaTime, bGamePaused);
    O->SetBoolField(TEXT("attempted"), true);
    O->SetObjectField(TEXT("player_controller"), ObjectSummary(PlayerController));
    O->SetNumberField(TEXT("delta_time"), DeltaTime);
    O->SetBoolField(TEXT("game_paused"), bGamePaused);
    return O;
}

TSharedRef<FJsonObject> RunInputTagPhase(const FPieInputTarget& Target,
                                         UObject* Asc,
                                         const UInputAction* Action,
                                         const FGameplayTag& InputTag,
                                         const TCHAR* PhaseName,
                                         ETriggerEvent TriggerEvent,
                                         float DeltaTime,
                                         bool bGamePaused,
                                         bool bPostProcessInput,
                                         bool bReflectedAscFallback)
{
    auto Phase = MakeShared<FJsonObject>();
    Phase->SetStringField(TEXT("phase"), PhaseName);
    Phase->SetStringField(TEXT("trigger_event"), TriggerEventToString(TriggerEvent));

    TSharedRef<FJsonObject> Binding = ExecuteEnhancedInputActionBindings(
        Target.PlayerController,
        Action,
        TriggerEvent);
    Phase->SetObjectField(TEXT("enhanced_input_binding"), Binding);

    bool bExecutedBinding = false;
    Binding->TryGetBoolField(TEXT("executed"), bExecutedBinding);
    if (!bExecutedBinding && bReflectedAscFallback)
    {
        const TCHAR* FunctionName = TriggerEvent == ETriggerEvent::Completed
            ? TEXT("AbilityInputTagReleased")
            : TEXT("AbilityInputTagPressed");
        Phase->SetObjectField(TEXT("reflected_asc_fallback"),
            CallGameplayTagFunctionIfReflected(Asc, FunctionName, InputTag));
    }
    else
    {
        auto Fallback = MakeShared<FJsonObject>();
        Fallback->SetBoolField(TEXT("attempted"), false);
        Fallback->SetStringField(TEXT("reason"), bExecutedBinding
            ? TEXT("Enhanced Input binding executed; ASC fallback skipped")
            : TEXT("call_reflected_asc_fallback=false"));
        Phase->SetObjectField(TEXT("reflected_asc_fallback"), Fallback);
    }

    if (bPostProcessInput)
    {
        Phase->SetObjectField(TEXT("post_process_input"),
            PostProcessPlayerInput(Target.PlayerController, DeltaTime, bGamePaused));
    }
    else
    {
        auto Process = MakeShared<FJsonObject>();
        Process->SetBoolField(TEXT("attempted"), false);
        Process->SetStringField(TEXT("reason"), TEXT("post_process_input=false"));
        Phase->SetObjectField(TEXT("post_process_input"), Process);
        Phase->SetObjectField(TEXT("process_ability_input_reflected"),
            ProcessAbilityInputIfReflected(Asc, DeltaTime, bGamePaused));
    }
    return Phase;
}

void ScheduleInputTagRelease(const FPieInputTarget& Target,
                             UObject* Asc,
                             const UInputAction* Action,
                             const FGameplayTag& InputTag,
                             float DurationSeconds,
                             float DeltaTime,
                             bool bGamePaused,
                             bool bPostProcessInput,
                             bool bReflectedAscFallback)
{
    TWeakObjectPtr<APlayerController> WeakController(Target.PlayerController);
    TWeakObjectPtr<UObject> WeakAsc(Asc);
    TWeakObjectPtr<UInputAction> WeakAction(const_cast<UInputAction*>(Action));
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [WeakController, WeakAsc, WeakAction, InputTag, DeltaTime, bGamePaused,
             bPostProcessInput, bReflectedAscFallback](float) -> bool
            {
                FPieInputTarget ReleaseTarget;
                ReleaseTarget.PlayerController = WeakController.Get();
                if (ReleaseTarget.PlayerController)
                {
                    ReleaseTarget.World = ReleaseTarget.PlayerController->GetWorld();
                    ReleaseTarget.LocalPlayer = ReleaseTarget.PlayerController->GetLocalPlayer();
                }
                RunInputTagPhase(
                    ReleaseTarget,
                    WeakAsc.Get(),
                    WeakAction.Get(),
                    InputTag,
                    TEXT("scheduled_release"),
                    ETriggerEvent::Completed,
                    DeltaTime,
                    bGamePaused,
                    bPostProcessInput,
                    bReflectedAscFallback);
                return false;
            }),
        DurationSeconds);
}

FSageToolDispatch::FOutcome SimulateInputTagImpl(const TSharedPtr<FJsonObject>& Args)
{
    FPieInputTarget Target;
    FString Error;
    if (!ResolvePieInputTarget(Args, Target, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004, Error);
    }

    FString TagString;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("input_tag"), TagString);
        Args->TryGetStringField(TEXT("tag"), TagString);
    }
    if (TagString.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'input_tag' (e.g. InputTag.Jump)"));
    }

    const FGameplayTag InputTag = FGameplayTag::RequestGameplayTag(FName(*TagString), false);
    if (!InputTag.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("gameplay tag is not registered: %s"), *TagString));
    }

    FString ActorSource;
    AActor* Actor = ResolveTraceActor(Args, Target, ActorSource);
    if (!Actor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("input-tag actor not found; pass actor_id or use a PIE local player with a pawn"));
    }

    UObject* Asc = ResolveAbilitySystemComponent(Actor, Target);
    FAbilityTraceFilter Filter = ParseAbilityTraceFilter(Args);
    if (Filter.InputTag.IsEmpty())
    {
        Filter.InputTag = TagString;
    }
    if (Filter.Tag.IsEmpty())
    {
        Filter.Tag = TagString;
    }

    FString Mode = TEXT("tap");
    FString ActionPath;
    bool bPostProcessInput = true;
    bool bReflectedAscFallback = true;
    double Duration = 0.0;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("mode"), Mode);
        Args->TryGetStringField(TEXT("input_mode"), Mode);
        Args->TryGetStringField(TEXT("action"), ActionPath);
        Args->TryGetStringField(TEXT("action_path"), ActionPath);
        Args->TryGetBoolField(TEXT("post_process_input"), bPostProcessInput);
        Args->TryGetBoolField(TEXT("call_reflected_asc_fallback"), bReflectedAscFallback);
        Args->TryGetNumberField(TEXT("duration"), Duration);
        Args->TryGetNumberField(TEXT("duration_seconds"), Duration);
    }
    Mode = Mode.ToLower();

    UInputAction* Action = nullptr;
    FString ActionSource;
    TArray<TSharedPtr<FJsonValue>> ActionDiagnostics;
    if (!ActionPath.IsEmpty())
    {
        Action = ResolveInputAction(ActionPath);
        ActionSource = TEXT("explicit_action_path");
        if (!Action)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("InputAction not found: %s"), *ActionPath));
        }
    }
    else
    {
        Action = FindInputActionForInputTag(Target, Actor, InputTag, ActionSource, ActionDiagnostics);
    }

    FVector BeforeLocation = Actor->GetActorLocation();
    FVector BeforeVelocity = Actor->GetVelocity();
    auto Before = MakeShared<FJsonObject>();
    Before->SetObjectField(TEXT("movement"), SnapshotMovement(Actor, Target.World));
    Before->SetObjectField(TEXT("gas"), SnapshotAbilitySystem(Asc, Filter, /*bTryActivateMatches=*/false));

    TArray<TSharedPtr<FJsonValue>> Phases;
    bool bReleaseScheduled = false;
    const float DeltaTime = Filter.ProcessDeltaTime;
    const bool bGamePaused = Filter.bGamePaused;

    const bool bReleaseOnly = Mode == TEXT("release") || Mode == TEXT("up") || Mode == TEXT("stop");
    const bool bTap = Mode == TEXT("tap") || Mode == TEXT("once") || Mode == TEXT("press_and_release");
    if (bReleaseOnly)
    {
        Phases.Add(MakeShared<FJsonValueObject>(RunInputTagPhase(
            Target,
            Asc,
            Action,
            InputTag,
            TEXT("release"),
            ETriggerEvent::Completed,
            DeltaTime,
            bGamePaused,
            bPostProcessInput,
            bReflectedAscFallback)));
    }
    else
    {
        Phases.Add(MakeShared<FJsonValueObject>(RunInputTagPhase(
            Target,
            Asc,
            Action,
            InputTag,
            TEXT("press"),
            ETriggerEvent::Triggered,
            DeltaTime,
            bGamePaused,
            bPostProcessInput,
            bReflectedAscFallback)));

        if (Duration > 0.0)
        {
            ScheduleInputTagRelease(
                Target,
                Asc,
                Action,
                InputTag,
                static_cast<float>(Duration),
                DeltaTime,
                bGamePaused,
                bPostProcessInput,
                bReflectedAscFallback);
            bReleaseScheduled = true;
        }
        else if (bTap)
        {
            Phases.Add(MakeShared<FJsonValueObject>(RunInputTagPhase(
                Target,
                Asc,
                Action,
                InputTag,
                TEXT("release"),
                ETriggerEvent::Completed,
                DeltaTime,
                bGamePaused,
                bPostProcessInput,
                bReflectedAscFallback)));
        }
    }

    FVector AfterLocation = Actor->GetActorLocation();
    FVector AfterVelocity = Actor->GetVelocity();
    auto After = MakeShared<FJsonObject>();
    After->SetObjectField(TEXT("movement"), SnapshotMovement(Actor, Target.World));
    After->SetObjectField(TEXT("gas"), SnapshotAbilitySystem(Asc, Filter, /*bTryActivateMatches=*/false));

    auto Delta = MakeShared<FJsonObject>();
    Delta->SetObjectField(TEXT("location"), VectorToJson(AfterLocation - BeforeLocation));
    Delta->SetObjectField(TEXT("velocity"), VectorToJson(AfterVelocity - BeforeVelocity));

    auto ActionResolution = MakeShared<FJsonObject>();
    ActionResolution->SetStringField(TEXT("source"), ActionSource);
    ActionResolution->SetStringField(TEXT("explicit_action_path"), ActionPath);
    ActionResolution->SetObjectField(TEXT("action"), ObjectSummary(Action));
    ActionResolution->SetArrayField(TEXT("diagnostics"), ActionDiagnostics);
    ActionResolution->SetBoolField(TEXT("resolved"), Action != nullptr);

    auto Diagnostics = MakeShared<FJsonObject>();
    Diagnostics->SetStringField(TEXT("actor_source"), ActorSource);
    Diagnostics->SetBoolField(TEXT("asc_found"), Asc != nullptr);
    Diagnostics->SetBoolField(TEXT("uses_enhanced_input_bound_delegate"), true);
    Diagnostics->SetStringField(TEXT("lyra_native_route"),
        TEXT("Executes the Enhanced Input action binding for the tag, then calls PlayerController::PostProcessInput so Lyra's override can process ASC input."));
    Diagnostics->SetBoolField(TEXT("hard_lyra_dependency"), false);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("input_tag"), InputTag.ToString());
    R->SetStringField(TEXT("mode"), Mode);
    R->SetNumberField(TEXT("duration_seconds"), Duration);
    R->SetBoolField(TEXT("release_scheduled"), bReleaseScheduled);
    R->SetObjectField(TEXT("target"), PieInputTargetToJson(Target));
    R->SetObjectField(TEXT("world"), ObjectSummary(Target.World));
    R->SetObjectField(TEXT("actor"), ObjectSummary(Actor));
    R->SetObjectField(TEXT("controller"), ObjectSummary(Target.PlayerController));
    R->SetObjectField(TEXT("player_state"), ObjectSummary(Target.PlayerController ? Target.PlayerController->PlayerState : nullptr));
    R->SetObjectField(TEXT("asc"), ObjectSummary(Asc));
    R->SetObjectField(TEXT("action_resolution"), ActionResolution);
    R->SetObjectField(TEXT("before"), Before);
    R->SetArrayField(TEXT("phases"), Phases);
    R->SetObjectField(TEXT("after"), After);
    R->SetObjectField(TEXT("delta"), Delta);
    R->SetObjectField(TEXT("diagnostics"), Diagnostics);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

const TCHAR* InputActionValueTypeName(EInputActionValueType Type)
{
    switch (Type)
    {
    case EInputActionValueType::Boolean: return TEXT("Boolean");
    case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
    case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
    case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
    default:                             return TEXT("None");
    }
}

TSharedRef<FJsonObject> SnapshotEnhancedInput(const FPieInputTarget& Target,
                                              UInputAction* Action,
                                              const FString& ActionPath,
                                              const FString& MappingName,
                                              const FVector& RawValue,
                                              EInputActionValueType ValueType)
{
    auto O = MakeShared<FJsonObject>();
    O->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
    O->SetObjectField(TEXT("player_controller"), ObjectSummary(Target.PlayerController));
    O->SetObjectField(TEXT("local_player"), ObjectSummary(Target.LocalPlayer));
    O->SetStringField(TEXT("action_path_requested"), ActionPath);
    O->SetStringField(TEXT("mapping_name"), MappingName);
    O->SetObjectField(TEXT("value"), VectorToJson(RawValue));
    O->SetStringField(TEXT("value_type"), InputActionValueTypeName(ValueType));
    O->SetObjectField(TEXT("action"), ObjectSummary(Action));

    if (Action)
    {
        auto ActionFields = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> It(Action->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property)
            {
                continue;
            }
            const FString Name = Property->GetName();
            if (Name == TEXT("ValueType")
                || Name.Contains(TEXT("Trigger"))
                || Name.Contains(TEXT("Modifier"))
                || Name.Contains(TEXT("Mapping"))
                || Name.Contains(TEXT("Tag")))
            {
                if (TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Action, Property))
                {
                    ActionFields->SetField(Name, Value);
                }
            }
        }
        O->SetObjectField(TEXT("action_fields"), ActionFields);
    }

    auto SubsystemObj = MakeShared<FJsonObject>();
    UEnhancedInputLocalPlayerSubsystem* Subsystem = Target.LocalPlayer
        ? Target.LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>()
        : nullptr;
    SubsystemObj->SetObjectField(TEXT("object"), ObjectSummary(Subsystem));
    if (Subsystem)
    {
        auto Reflected = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> It(Subsystem->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property)
            {
                continue;
            }
            const FString Name = Property->GetName();
            if (Name.Contains(TEXT("Mapping"))
                || Name.Contains(TEXT("Context"))
                || Name.Contains(TEXT("Mappable"))
                || Name.Contains(TEXT("Injection"))
                || Name.Contains(TEXT("Input")))
            {
                if (TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Subsystem, Property))
                {
                    Reflected->SetField(Name, Value);
                }
            }
        }
        SubsystemObj->SetObjectField(TEXT("reflected_input_state"), Reflected);
    }
    O->SetObjectField(TEXT("enhanced_input_subsystem"), SubsystemObj);
    return O;
}

FSageToolDispatch::FOutcome InjectForTrace(const TSharedPtr<FJsonObject>& Args,
                                           const FPieInputTarget& Target,
                                           TSharedPtr<FJsonObject>& OutInjection,
                                           UInputAction*& OutAction,
                                           FString& OutActionPath,
                                           FString& OutMappingName,
                                           FVector& OutRawValue,
                                           EInputActionValueType& OutValueType)
{
    OutInjection = MakeShared<FJsonObject>();
    OutAction = nullptr;
    OutActionPath.Empty();
    OutMappingName.Empty();
    OutRawValue = FVector(1.0, 0.0, 0.0);
    OutValueType = EInputActionValueType::Boolean;

    FString KeyName;
    FString Mode;
    bool bHasAction = false;
    bool bHasKey = false;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("action"), OutActionPath);
        Args->TryGetStringField(TEXT("action_path"), OutActionPath);
        Args->TryGetStringField(TEXT("mapping_name"), OutMappingName);
        Args->TryGetStringField(TEXT("key"), KeyName);
        Args->TryGetStringField(TEXT("mode"), Mode);
        Args->TryGetStringField(TEXT("input_mode"), Mode);
    }
    bHasAction = !OutActionPath.IsEmpty() || !OutMappingName.IsEmpty();
    bHasKey = !KeyName.IsEmpty();

    const TSharedPtr<FJsonValue> Value = Args.IsValid()
        ? Args->Values.FindRef(TEXT("value"))
        : TSharedPtr<FJsonValue>();
    if (!JsonToVector(Value, OutRawValue))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("'value' must be boolean, number, array [x,y,z], or object {x,y,z}"));
    }

    if (!OutActionPath.IsEmpty())
    {
        OutAction = ResolveInputAction(OutActionPath);
        if (!OutAction)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("InputAction not found: %s"), *OutActionPath));
        }
        OutValueType = OutAction->ValueType;
    }
    else if (Value.IsValid())
    {
        OutValueType = Value->Type == EJson::Array
            ? (Value->AsArray().Num() >= 3 ? EInputActionValueType::Axis3D
               : Value->AsArray().Num() >= 2 ? EInputActionValueType::Axis2D
                                             : EInputActionValueType::Axis1D)
            : (Value->Type == EJson::Number ? EInputActionValueType::Axis1D
                                             : EInputActionValueType::Boolean);
    }

    if (bHasAction)
    {
        FSageToolDispatch::FOutcome Outcome = TriggerActionImpl(Args);
        if (!Outcome.bSuccess)
        {
            return Outcome;
        }
        OutInjection = Outcome.Result.IsValid() ? Outcome.Result : OutInjection;
        OutInjection->SetStringField(TEXT("kind"), TEXT("enhanced_input_action"));
        return FSageToolDispatch::FOutcome::MakeSuccess(OutInjection.ToSharedRef());
    }

    if (bHasKey)
    {
        FKey Key;
        FString Error;
        if (!ResolveInputKey(Args, Key, Error))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
        }
        if (!Target.LocalPlayer)
        {
            return FSageToolDispatch::FOutcome::MakeError(
                -32004,
                BuildNoLocalPlayerInputError(Target, TEXT("gameplay.trace_input_action")));
        }
        Mode = Mode.IsEmpty() ? TEXT("tap") : Mode.ToLower();
        double AmountNum = 1.0;
        double Duration = 0.0;
        if (Args.IsValid())
        {
            Args->TryGetNumberField(TEXT("amount"), AmountNum);
            Args->TryGetNumberField(TEXT("duration"), Duration);
            Args->TryGetNumberField(TEXT("duration_seconds"), Duration);
        }

        bool bPressed = false;
        bool bReleased = false;
        bool bReleaseScheduled = false;
        FInputRouteResult PressRoute;
        FInputRouteResult ReleaseRoute;
        if (Mode == TEXT("release") || Mode == TEXT("stop"))
        {
            ReleaseRoute = InjectPieKeyEventDetailed(Target, Key, IE_Released, 0.0f);
            bReleased = ReleaseRoute.Handled();
        }
        else
        {
            PressRoute = InjectPieKeyEventDetailed(Target, Key, IE_Pressed, static_cast<float>(AmountNum));
            bPressed = PressRoute.Handled();
            if (Mode == TEXT("tap") || Mode == TEXT("once") || Mode == TEXT("press"))
            {
                ReleaseRoute = InjectPieKeyEventDetailed(Target, Key, IE_Released, 0.0f);
                bReleased = ReleaseRoute.Handled();
            }
            else if (Duration > 0.0)
            {
                ScheduleKeyRelease(Key, Target, Duration);
                bReleaseScheduled = true;
            }
        }

        OutInjection->SetStringField(TEXT("kind"), TEXT("key"));
        OutInjection->SetStringField(TEXT("key"), Key.GetFName().ToString());
        OutInjection->SetStringField(TEXT("mode"), Mode);
        OutInjection->SetBoolField(TEXT("pressed"), bPressed);
        OutInjection->SetBoolField(TEXT("released"), bReleased);
        OutInjection->SetBoolField(TEXT("release_scheduled"), bReleaseScheduled);
        OutInjection->SetObjectField(TEXT("press_route"), InputRouteToJson(PressRoute));
        OutInjection->SetObjectField(TEXT("release_route"), InputRouteToJson(ReleaseRoute));
        OutInjection->SetNumberField(TEXT("duration"), Duration);
        OutInjection->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
        OutInjection->SetObjectField(TEXT("target"), PieInputTargetToJson(Target));
        return FSageToolDispatch::FOutcome::MakeSuccess(OutInjection.ToSharedRef());
    }

    OutInjection->SetStringField(TEXT("kind"), TEXT("none"));
    OutInjection->SetBoolField(TEXT("performed"), false);
    OutInjection->SetStringField(TEXT("reason"), TEXT("no key/action/mapping_name supplied; returning readback only"));
    return FSageToolDispatch::FOutcome::MakeSuccess(OutInjection.ToSharedRef());
}

FSageToolDispatch::FOutcome TraceInputActionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FPieInputTarget Target;
    FString Error;
    if (!ResolvePieInputTarget(Args, Target, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004, Error);
    }

    FString ActorSource;
    AActor* Actor = ResolveTraceActor(Args, Target, ActorSource);
    if (!Actor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("trace actor not found; pass actor_id or use a PIE local player with a pawn"));
    }

    UObject* Asc = ResolveAbilitySystemComponent(Actor, Target);
    const FAbilityTraceFilter Filter = ParseAbilityTraceFilter(Args);

    FVector BeforeLocation = Actor->GetActorLocation();
    FVector BeforeVelocity = Actor->GetVelocity();
    auto Before = MakeShared<FJsonObject>();
    Before->SetObjectField(TEXT("movement"), SnapshotMovement(Actor, Target.World));
    Before->SetObjectField(TEXT("gas"), SnapshotAbilitySystem(Asc, Filter, /*bTryActivateMatches=*/false));

    TSharedPtr<FJsonObject> Injection;
    UInputAction* Action = nullptr;
    FString ActionPath;
    FString MappingName;
    FVector RawValue;
    EInputActionValueType ValueType;
    FSageToolDispatch::FOutcome InjectionOutcome = InjectForTrace(
        Args,
        Target,
        Injection,
        Action,
        ActionPath,
        MappingName,
        RawValue,
        ValueType);
    if (!InjectionOutcome.bSuccess)
    {
        return InjectionOutcome;
    }

    TSharedRef<FJsonObject> ProcessResult = Filter.bProcessAbilityInput
        ? ProcessAbilityInputIfReflected(Asc, Filter.ProcessDeltaTime, Filter.bGamePaused)
        : MakeShared<FJsonObject>();
    if (!Filter.bProcessAbilityInput)
    {
        ProcessResult->SetBoolField(TEXT("attempted"), false);
        ProcessResult->SetStringField(TEXT("reason"), TEXT("process_ability_input=false"));
    }

    auto TryActivation = MakeShared<FJsonObject>();
    TryActivation->SetBoolField(TEXT("requested"), Filter.bTryActivate);
    if (Filter.bTryActivate)
    {
        int32 TryTotalSpecs = 0;
        int32 TryMatchedSpecs = 0;
        TryActivation->SetArrayField(TEXT("ability_specs"), BuildAbilitySpecSummaries(
            Asc,
            Filter,
            /*bTryActivateMatches=*/true,
            TryTotalSpecs,
            TryMatchedSpecs));
        TryActivation->SetNumberField(TEXT("ability_spec_count"), TryTotalSpecs);
        TryActivation->SetNumberField(TEXT("matched_ability_spec_count"), TryMatchedSpecs);
    }
    else
    {
        TryActivation->SetStringField(TEXT("reason"), TEXT("pass try_activate:true to call ASC TryActivateAbility for matched specs"));
    }

    FVector AfterLocation = Actor->GetActorLocation();
    FVector AfterVelocity = Actor->GetVelocity();
    auto After = MakeShared<FJsonObject>();
    After->SetObjectField(TEXT("movement"), SnapshotMovement(Actor, Target.World));
    After->SetObjectField(TEXT("gas"), SnapshotAbilitySystem(Asc, Filter, /*bTryActivateMatches=*/false));

    auto Delta = MakeShared<FJsonObject>();
    Delta->SetObjectField(TEXT("location"), VectorToJson(AfterLocation - BeforeLocation));
    Delta->SetObjectField(TEXT("velocity"), VectorToJson(AfterVelocity - BeforeVelocity));

    auto Diagnostics = MakeShared<FJsonObject>();
    Diagnostics->SetBoolField(TEXT("wait_frames_supported_inline"), false);
    Diagnostics->SetStringField(TEXT("wait_frames_note"),
        TEXT("Synchronous MCP handlers cannot advance PIE frames without blocking the game thread; call again after ticks for delayed readback."));
    Diagnostics->SetStringField(TEXT("actor_source"), ActorSource);
    Diagnostics->SetBoolField(TEXT("asc_found"), Asc != nullptr);

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("local_player_index"), Target.LocalPlayerIndex);
    R->SetObjectField(TEXT("world"), ObjectSummary(Target.World));
    R->SetObjectField(TEXT("target"), PieInputTargetToJson(Target));
    R->SetObjectField(TEXT("actor"), ObjectSummary(Actor));
    R->SetObjectField(TEXT("controller"), ObjectSummary(Target.PlayerController));
    R->SetObjectField(TEXT("player_state"), ObjectSummary(Target.PlayerController ? Target.PlayerController->PlayerState : nullptr));
    R->SetObjectField(TEXT("enhanced_input"), SnapshotEnhancedInput(
        Target,
        Action,
        ActionPath,
        MappingName,
        RawValue,
        ValueType));
    R->SetObjectField(TEXT("before"), Before);
    R->SetObjectField(TEXT("injection"), Injection);
    R->SetObjectField(TEXT("process_ability_input"), ProcessResult);
    R->SetObjectField(TEXT("try_activate"), TryActivation);
    R->SetObjectField(TEXT("after"), After);
    R->SetObjectField(TEXT("delta"), Delta);
    R->SetObjectField(TEXT("diagnostics"), Diagnostics);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CleanupPythonRefsImpl(const TSharedPtr<FJsonObject>& Args)
{
    bool bClearMainGlobals = true;
    bool bIncludePieWorlds = true;
    bool bIncludeEditorWorld = true;
    bool bCollectUnrealGarbage = true;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("clear_main_globals"), bClearMainGlobals);
        Args->TryGetBoolField(TEXT("clear_python_main_globals"), bClearMainGlobals);
        Args->TryGetBoolField(TEXT("include_pie_worlds"), bIncludePieWorlds);
        Args->TryGetBoolField(TEXT("include_editor_world"), bIncludeEditorWorld);
        Args->TryGetBoolField(TEXT("collect_unreal_garbage"), bCollectUnrealGarbage);
    }

    const detail::FPythonReferenceCleanupReport Cleanup =
        detail::CleanupPythonReferences(
            bClearMainGlobals,
            bIncludePieWorlds,
            bIncludeEditorWorld,
            bCollectUnrealGarbage);
    if (Cleanup.bPythonCommandRan && !Cleanup.bPythonCommandSucceeded)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("Python reference cleanup failed: %s"), *Cleanup.Error));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    AddPythonCleanupReport(R, Cleanup);
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
    // Guard against zero/negative delta — FApp::GetDeltaTime() can be 0 on
    // the very first tick of the editor or while paused, and 1/0 → inf
    // serializes as `null` (or `inf`) in JSON, which clients reject.
    double Delta = FApp::GetDeltaTime();
    if (Delta <= 0.0) Delta = 1.0 / 60.0;

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("fps"),              1.0 / Delta);
    R->SetNumberField(TEXT("frame_time_ms"),    Delta * 1000.0);
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
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), Directory);
        Args->TryGetStringField(TEXT("directory"), Directory);
    }

    UEditorAssetSubsystem* AssetSub = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    if (!AssetSub)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("UEditorAssetSubsystem not available"));
    }

    TArray<FAssetData> AssetsToValidate;
    TArray<TSharedPtr<FJsonValue>> MissingAssets;

    const TArray<TSharedPtr<FJsonValue>>* AssetArgs = nullptr;
    if (Args.IsValid() && Args->TryGetArrayField(TEXT("assets"), AssetArgs) && AssetArgs)
    {
        for (const TSharedPtr<FJsonValue>& V : *AssetArgs)
        {
            if (!V.IsValid() || V->Type != EJson::String) continue;
            const FString AssetPath = V->AsString();
            FAssetData Data = AssetSub->FindAssetData(AssetPath);
            if (Data.IsValid())
            {
                AssetsToValidate.Add(Data);
            }
            else
            {
                MissingAssets.Add(MakeShared<FJsonValueString>(AssetPath));
            }
        }
    }
    else
    {
        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*Directory));
        Filter.bRecursivePaths = true;
        ARM.Get().GetAssets(Filter, AssetsToValidate);
    }

    const bool bDirectoryExists = AssetSub->DoesDirectoryExist(Directory);
    if (AssetsToValidate.Num() == 0 && MissingAssets.Num() == 0 && !bDirectoryExists)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset directory not found or empty: %s"), *Directory));
    }

    UEditorValidatorSubsystem* ValidatorSub = GEditor
        ? GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>()
        : nullptr;
    if (!ValidatorSub)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("UEditorValidatorSubsystem not available; enable DataValidation plugin"));
    }

    FValidateAssetsSettings Settings;
    Settings.ValidationUsecase = EDataValidationUsecase::Manual;
    Settings.bCollectPerAssetDetails = true;
    FValidateAssetsResults Results;
    const int32 FailureOrWarningCount =
        ValidatorSub->ValidateAssetsWithSettings(AssetsToValidate, Settings, Results);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("directory"), Directory);
    R->SetBoolField(TEXT("directory_exists"), bDirectoryExists);
    R->SetNumberField(TEXT("requested"), Results.NumRequested);
    R->SetNumberField(TEXT("checked"), Results.NumChecked);
    R->SetNumberField(TEXT("valid"), Results.NumValid);
    R->SetNumberField(TEXT("invalid"), Results.NumInvalid);
    R->SetNumberField(TEXT("warnings"), Results.NumWarnings);
    R->SetNumberField(TEXT("skipped"), Results.NumSkipped);
    R->SetNumberField(TEXT("unable_to_validate"), Results.NumUnableToValidate);
    R->SetNumberField(TEXT("failure_or_warning_count"), FailureOrWarningCount);
    R->SetArrayField(TEXT("missing_assets"), MissingAssets);
    R->SetBoolField(TEXT("valid_result"),
        FailureOrWarningCount == 0 && Results.NumInvalid == 0 && MissingAssets.Num() == 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
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
    Dispatch.RegisterHandler(TEXT("input.press_key"),           GT(&PressKeyImpl));
    Dispatch.RegisterHandler(TEXT("input.hold_key"),            GT(&HoldKeyImpl));
    Dispatch.RegisterHandler(TEXT("input.release_key"),         GT(&ReleaseKeyImpl));
    Dispatch.RegisterHandler(TEXT("input.trigger_action"),      GT(&TriggerActionImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.trace_input_action"), GT(&TraceInputActionImpl));
    Dispatch.RegisterHandler(TEXT("gas.trace_ability_activation"), GT(&TraceInputActionImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.simulate_input_tag"), GT(&SimulateInputTagImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.spawn_pie_actor_snapshot"), GT(&SpawnPieActorSnapshotImpl));

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
    Dispatch.RegisterHandler(TEXT("editor.cleanup_python_refs"), GT(&CleanupPythonRefsImpl));

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
