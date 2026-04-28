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
#include "GenericPlatform/GenericPlatformMisc.h"
#include "LevelEditorViewport.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformOutputDevices.h"
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

    const FString LogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
    FString Contents;
    if (!FFileHelper::LoadFileToString(Contents, *LogPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("could not read log file: %s"), *LogPath));
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
    R->SetStringField(TEXT("log_path"),  LogPath);
    R->SetArrayField (TEXT("lines"),     Out);
    R->SetNumberField(TEXT("count"),     Out.Num());
    R->SetNumberField(TEXT("total_lines"), Lines.Num());
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
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
