#include "Tools/SageEditorTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "ScopedTransaction.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "Sage"

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

FString PlayNetModeToString(EPlayNetMode Mode)
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

FString PlayModeTypeToString(EPlayModeType Mode)
{
    switch (Mode)
    {
    case PlayMode_InViewPort:
        return TEXT("in_viewport");
    case PlayMode_InEditorFloating:
        return TEXT("in_editor_floating");
    case PlayMode_InMobilePreview:
        return TEXT("in_mobile_preview");
    case PlayMode_InTargetedMobilePreview:
        return TEXT("in_targeted_mobile_preview");
    case PlayMode_InNewProcess:
        return TEXT("in_new_process");
    case PlayMode_InVR:
        return TEXT("in_vr");
    case PlayMode_Simulate:
        return TEXT("simulate");
    case PlayMode_QuickLaunch:
        return TEXT("quick_launch");
    default:
        return FString::Printf(TEXT("unknown_%d"), static_cast<int32>(Mode));
    }
}

TSharedRef<FJsonObject> PlaySettingsToJson(const ULevelEditorPlaySettings* Settings)
{
    auto O = MakeShared<FJsonObject>();
    if (!Settings)
    {
        O->SetBoolField(TEXT("valid"), false);
        return O;
    }

    O->SetBoolField(TEXT("valid"), true);
    EPlayNetMode NetMode = PIE_Standalone;
    const bool bNetModeActive = Settings->GetPlayNetMode(NetMode);
    O->SetStringField(TEXT("play_net_mode"), PlayNetModeToString(NetMode));
    O->SetNumberField(TEXT("play_net_mode_value"), static_cast<int32>(NetMode));
    O->SetBoolField(TEXT("play_net_mode_active"), bNetModeActive);

    int32 ClientCount = 1;
    const bool bClientCountActive = Settings->GetPlayNumberOfClients(ClientCount);
    O->SetNumberField(TEXT("play_number_of_clients"), ClientCount);
    O->SetBoolField(TEXT("play_number_of_clients_active"), bClientCountActive);

    bool bRunUnderOneProcess = true;
    const bool bRunUnderOneProcessActive = Settings->GetRunUnderOneProcess(bRunUnderOneProcess);
    O->SetBoolField(TEXT("run_under_one_process"), bRunUnderOneProcess);
    O->SetBoolField(TEXT("run_under_one_process_active"), bRunUnderOneProcessActive);

    O->SetNumberField(TEXT("primary_pie_client_index"), Settings->GetPrimaryPIEClientIndex());
    O->SetBoolField(TEXT("launch_separate_server"), Settings->bLaunchSeparateServer);
    O->SetBoolField(TEXT("game_gets_mouse_control"), Settings->GameGetsMouseControl);
    O->SetBoolField(TEXT("use_mouse_for_touch"), Settings->UseMouseForTouch);
    O->SetStringField(
        TEXT("last_executed_play_mode"),
        PlayModeTypeToString(static_cast<EPlayModeType>(Settings->LastExecutedPlayModeType.GetValue())));
    O->SetNumberField(
        TEXT("last_executed_play_mode_value"),
        static_cast<int32>(Settings->LastExecutedPlayModeType.GetValue()));

    FIntPoint ClientWindowSize(0, 0);
    const bool bClientWindowSizeActive = Settings->GetClientWindowSize(ClientWindowSize);
    auto ClientWindow = MakeShared<FJsonObject>();
    ClientWindow->SetNumberField(TEXT("x"), ClientWindowSize.X);
    ClientWindow->SetNumberField(TEXT("y"), ClientWindowSize.Y);
    ClientWindow->SetBoolField(TEXT("active"), bClientWindowSizeActive);
    O->SetObjectField(TEXT("client_window_size"), ClientWindow);
    return O;
}

bool ParsePlayNetModeString(const FString& InMode, EPlayNetMode& OutMode, FString& OutError)
{
    FString Mode = InMode;
    Mode.TrimStartAndEndInline();
    Mode = Mode.ToLower();
    Mode.ReplaceInline(TEXT("-"), TEXT("_"));
    Mode.ReplaceInline(TEXT(" "), TEXT("_"));

    if (Mode == TEXT("standalone")
        || Mode == TEXT("offline")
        || Mode == TEXT("local")
        || Mode == TEXT("pie_standalone")
        || Mode == TEXT("play_standalone"))
    {
        OutMode = PIE_Standalone;
        return true;
    }
    if (Mode == TEXT("listen")
        || Mode == TEXT("listen_server")
        || Mode == TEXT("server")
        || Mode == TEXT("pie_listen_server")
        || Mode == TEXT("play_as_listen_server"))
    {
        OutMode = PIE_ListenServer;
        return true;
    }
    if (Mode == TEXT("client")
        || Mode == TEXT("pie_client")
        || Mode == TEXT("play_as_client"))
    {
        OutMode = PIE_Client;
        return true;
    }

    OutError = FString::Printf(
        TEXT("invalid net_mode '%s'; expected standalone, listen_server, or client"),
        *InMode);
    return false;
}

bool TryGetBoolAlias(const TSharedPtr<FJsonObject>& Args,
                     const TCHAR* Name,
                     bool& OutValue)
{
    return Args.IsValid() && Args->TryGetBoolField(Name, OutValue);
}

bool TryGetNumberAlias(const TSharedPtr<FJsonObject>& Args,
                       const TCHAR* Name,
                       double& OutValue)
{
    return Args.IsValid() && Args->TryGetNumberField(Name, OutValue);
}

bool SetReflectedIntProperty(UObject* Obj, FName PropertyName, int32 Value)
{
    if (!Obj)
    {
        return false;
    }
    FProperty* Property = Obj->GetClass()->FindPropertyByName(PropertyName);
    FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
    if (!Numeric || !Numeric->IsInteger())
    {
        return false;
    }
    Numeric->SetIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Obj), static_cast<int64>(Value));
    return true;
}

void AddStringArrayField(TSharedRef<FJsonObject> Obj,
                         const TCHAR* Field,
                         const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Reserve(Values.Num());
    for (const FString& Value : Values)
    {
        Arr.Add(MakeShared<FJsonValueString>(Value));
    }
    Obj->SetArrayField(Field, Arr);
}

// ---- get_world ------------------------------------------------------------

FSageToolDispatch::FOutcome GetWorldOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("editor_active"), GEditor != nullptr);

    if (GEditor != nullptr)
    {
        if (UWorld* World = GEditor->GetEditorWorldContext().World())
        {
            Result->SetStringField(TEXT("world_path"), World->GetPathName());
            Result->SetStringField(TEXT("map_name"),   World->GetMapName());
            if (ULevel* Level = World->GetCurrentLevel())
            {
                // ULevel::Actors is a sparse array — destroyed entries become
                // nullptr but Num() does not shrink. Iterate to count valid.
                int32 ValidCount = 0;
                for (const AActor* A : Level->Actors)
                {
                    if (A != nullptr) ++ValidCount;
                }
                Result->SetNumberField(TEXT("actor_count"), ValidCount);
            }
        }
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- save_level ----------------------------------------------------------

FSageToolDispatch::FOutcome SaveLevelOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (World == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));
    }
    UPackage* Pkg = World->GetOutermost();
    if (Pkg == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("no package for world"));
    }

    const bool bWasDirty = Pkg->IsDirty();
    const TArray<UPackage*> Packages{Pkg};
    // bOnlyDirty=true: skip clean packages to avoid VCS noise + disk waste.
    // Callers asking save_level on an unmodified world should be a no-op,
    // not a forced rewrite that bumps mtimes and dirties source-control.
    UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty=*/true);

    UE_LOG(LogSageBridge, Log, TEXT("Saved level package: %s"), *Pkg->GetName());

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("world_path"), World->GetPathName());
    Result->SetStringField(TEXT("package"),    Pkg->GetName());
    Result->SetBoolField(TEXT("was_dirty"),    bWasDirty);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- get_current_level ----------------------------------------------------

FSageToolDispatch::FOutcome GetCurrentLevelOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (World == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("world_path"), World->GetPathName());
    Result->SetStringField(TEXT("map_name"),   World->GetMapName());
    if (ULevel* Level = World->GetCurrentLevel())
    {
        Result->SetStringField(TEXT("level_path"), Level->GetPathName());
        int32 ValidCount = 0;
        for (const AActor* A : Level->Actors)
        {
            if (A != nullptr) ++ValidCount;
        }
        Result->SetNumberField(TEXT("actor_count"), ValidCount);
    }

    TArray<TSharedPtr<FJsonValue>> SubLevels;
    for (ULevelStreaming* Sub : World->GetStreamingLevels())
    {
        if (Sub != nullptr)
        {
            SubLevels.Add(MakeShared<FJsonValueString>(Sub->GetWorldAssetPackageName()));
        }
    }
    Result->SetArrayField(TEXT("sub_levels"), SubLevels);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- get_pie_state --------------------------------------------------------

FSageToolDispatch::FOutcome GetPieStateOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto Result = MakeShared<FJsonObject>();
    const bool bActive = (GEditor != nullptr && GEditor->PlayWorld != nullptr);
    Result->SetBoolField(TEXT("active"), bActive);
    Result->SetObjectField(TEXT("play_settings"), PlaySettingsToJson(GetDefault<ULevelEditorPlaySettings>()));
    if (bActive)
    {
        Result->SetStringField(TEXT("play_world_path"), GEditor->PlayWorld->GetPathName());
    }
    TArray<TSharedPtr<FJsonValue>> PieWorlds;
    if (GEngine != nullptr)
    {
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            UWorld* World = Ctx.World();
            if (Ctx.WorldType != EWorldType::PIE || World == nullptr)
            {
                continue;
            }
            auto WorldJson = MakeShared<FJsonObject>();
            WorldJson->SetStringField(TEXT("name"), World->GetName());
            WorldJson->SetStringField(TEXT("path"), World->GetPathName());
            WorldJson->SetStringField(TEXT("map_name"), World->GetMapName());
            WorldJson->SetNumberField(TEXT("time_seconds"), World->GetTimeSeconds());
            PieWorlds.Add(MakeShared<FJsonValueObject>(WorldJson));
        }
    }
    Result->SetArrayField(TEXT("pie_worlds"), PieWorlds);
    Result->SetNumberField(TEXT("pie_world_count"), PieWorlds.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

FSageToolDispatch::FOutcome GetPlaySettingsOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetObjectField(TEXT("play_settings"), PlaySettingsToJson(GetDefault<ULevelEditorPlaySettings>()));
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- get_viewport_state ---------------------------------------------------

FSageToolDispatch::FOutcome GetViewportStateOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("editor_active"), GEditor != nullptr);

    if (GEditor != nullptr)
    {
        if (FViewport* ActiveVP = GEditor->GetActiveViewport())
        {
            const FIntPoint Size = ActiveVP->GetSizeXY();
            Result->SetNumberField(TEXT("active_viewport_width"),  Size.X);
            Result->SetNumberField(TEXT("active_viewport_height"), Size.Y);
        }
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- get_selected_actors --------------------------------------------------

FSageToolDispatch::FOutcome GetSelectedActorsOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UEditorActorSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (Sub == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EditorActorSubsystem unavailable"));
    }

    TArray<AActor*> Selected = Sub->GetSelectedLevelActors();
    TArray<TSharedPtr<FJsonValue>> Items;
    Items.Reserve(Selected.Num());
    for (AActor* A : Selected)
    {
        if (A != nullptr) Items.Add(MakeShared<FJsonValueString>(A->GetPathName()));
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("selected"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- select_actors --------------------------------------------------------

FSageToolDispatch::FOutcome SelectActorsOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("actor_ids"), PathsArr) || PathsArr == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_ids' array"));
    }

    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UEditorActorSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (Sub == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EditorActorSubsystem unavailable"));
    }

    TArray<AActor*> ToSelect;
    TArray<FString> NotFound;
    for (const TSharedPtr<FJsonValue>& V : *PathsArr)
    {
        const FString Path = V->AsString();
        AActor* A = detail::ResolveActor(Path);
        if (A != nullptr) ToSelect.Add(A);
        else NotFound.Add(Path);
    }

    Sub->SetSelectedLevelActors(ToSelect);

    TArray<TSharedPtr<FJsonValue>> SelectedArr;
    for (AActor* A : ToSelect)
    {
        if (A != nullptr) SelectedArr.Add(MakeShared<FJsonValueString>(A->GetPathName()));
    }
    TArray<TSharedPtr<FJsonValue>> NotFoundArr;
    for (const FString& P : NotFound) NotFoundArr.Add(MakeShared<FJsonValueString>(P));

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("selected"),  SelectedArr);
    Result->SetArrayField(TEXT("not_found"), NotFoundArr);
    Result->SetNumberField(TEXT("count"), SelectedArr.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- clear_selection ------------------------------------------------------

FSageToolDispatch::FOutcome ClearSelectionOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UEditorActorSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (Sub == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EditorActorSubsystem unavailable"));
    }

    Sub->SelectNothing();

    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("cleared"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- run_pie --------------------------------------------------------------

FSageToolDispatch::FOutcome RunPieOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    if (GEditor->PlayWorld != nullptr)
    {
        const TSharedRef<FJsonObject> CurrentSettings = PlaySettingsToJson(
            GetDefault<ULevelEditorPlaySettings>());
        FString NetMode = TEXT("unknown");
        double ClientCount = 0.0;
        CurrentSettings->TryGetStringField(TEXT("play_net_mode"), NetMode);
        CurrentSettings->TryGetNumberField(TEXT("play_number_of_clients"), ClientCount);
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("PIE already active; current editor play settings are PlayNetMode=%s, PlayNumberOfClients=%d"),
                            *NetMode,
                            static_cast<int32>(ClientCount)));
    }

    const ULevelEditorPlaySettings* Defaults = GetDefault<ULevelEditorPlaySettings>();
    if (Defaults == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("ULevelEditorPlaySettings defaults unavailable"));
    }

    ULevelEditorPlaySettings* PlaySettings = DuplicateObject<ULevelEditorPlaySettings>(
        Defaults,
        GetTransientPackage());
    if (PlaySettings == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("failed to create transient LevelEditorPlaySettings"));
    }

    TArray<FString> Warnings;
    bool bForceLocalPlayer = false;
    bool bSelectedViewport = true;
    bool bNewEditorWindow = false;
    bool bRestoreSettingsAfterStart = false;
    bool bAllowOnlineSubsystem = true;
    bool bForceLocalPlayerApplied = false;
    int32 RequestedLocalPlayerIndex = 0;

    if (Args.IsValid())
    {
        bool bValue = false;
        if (TryGetBoolAlias(Args, TEXT("force_local_player"), bValue))
        {
            bForceLocalPlayer = bValue;
        }
        if (TryGetBoolAlias(Args, TEXT("single_local_player"), bValue))
        {
            bForceLocalPlayer = bValue;
        }
        if (TryGetBoolAlias(Args, TEXT("selected_viewport"), bValue))
        {
            bSelectedViewport = bValue;
        }
        if (TryGetBoolAlias(Args, TEXT("use_selected_viewport"), bValue))
        {
            bSelectedViewport = bValue;
        }
        if (TryGetBoolAlias(Args, TEXT("new_editor_window"), bValue))
        {
            bNewEditorWindow = bValue;
        }
        if (TryGetBoolAlias(Args, TEXT("restore_settings_after_start"), bValue))
        {
            bRestoreSettingsAfterStart = bValue;
        }
        if (TryGetBoolAlias(Args, TEXT("allow_online_subsystem"), bValue))
        {
            bAllowOnlineSubsystem = bValue;
        }

        FString NetModeText;
        if (Args->TryGetStringField(TEXT("net_mode"), NetModeText)
            || Args->TryGetStringField(TEXT("play_net_mode"), NetModeText))
        {
            EPlayNetMode NetMode = PIE_Standalone;
            FString ParseError;
            if (!ParsePlayNetModeString(NetModeText, NetMode, ParseError))
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602, ParseError);
            }
            PlaySettings->SetPlayNetMode(NetMode);
        }

        double NumberValue = 0.0;
        if (TryGetNumberAlias(Args, TEXT("clients"), NumberValue)
            || TryGetNumberAlias(Args, TEXT("num_clients"), NumberValue)
            || TryGetNumberAlias(Args, TEXT("number_of_clients"), NumberValue)
            || TryGetNumberAlias(Args, TEXT("play_number_of_clients"), NumberValue))
        {
            PlaySettings->SetPlayNumberOfClients(FMath::Clamp(static_cast<int32>(NumberValue), 1, 64));
        }
        if (TryGetNumberAlias(Args, TEXT("local_player_index"), NumberValue)
            || TryGetNumberAlias(Args, TEXT("target_local_player_index"), NumberValue)
            || TryGetNumberAlias(Args, TEXT("primary_pie_client_index"), NumberValue))
        {
            RequestedLocalPlayerIndex = FMath::Clamp(static_cast<int32>(NumberValue), 0, 64);
            if (!SetReflectedIntProperty(
                    PlaySettings,
                    FName(TEXT("PrimaryPIEClientIndex")),
                    RequestedLocalPlayerIndex))
            {
                Warnings.Add(TEXT("could not set PrimaryPIEClientIndex on transient play settings"));
            }
        }

        if (TryGetBoolAlias(Args, TEXT("run_under_one_process"), bValue))
        {
            PlaySettings->SetRunUnderOneProcess(bValue);
        }
        if (TryGetBoolAlias(Args, TEXT("launch_separate_server"), bValue))
        {
            PlaySettings->bLaunchSeparateServer = bValue;
        }
        if (TryGetBoolAlias(Args, TEXT("game_gets_mouse_control"), bValue))
        {
            PlaySettings->GameGetsMouseControl = bValue;
        }
        if (TryGetBoolAlias(Args, TEXT("use_mouse_for_touch"), bValue))
        {
            PlaySettings->UseMouseForTouch = bValue;
        }
    }

    if (bForceLocalPlayer)
    {
        PlaySettings->SetPlayNetMode(PIE_Standalone);
        PlaySettings->SetPlayNumberOfClients(1);
        PlaySettings->SetRunUnderOneProcess(true);
        PlaySettings->bLaunchSeparateServer = false;
        PlaySettings->GameGetsMouseControl = true;
        bSelectedViewport = !bNewEditorWindow;
        bForceLocalPlayerApplied = true;
    }

    if (bNewEditorWindow)
    {
        PlaySettings->LastExecutedPlayModeType = PlayMode_InEditorFloating;
        bSelectedViewport = false;
    }
    else if (bSelectedViewport)
    {
        PlaySettings->LastExecutedPlayModeType = PlayMode_InViewPort;
    }

    FRequestPlaySessionParams Params;
    Params.EditorPlaySettings = PlaySettings;
    Params.WorldType = EPlaySessionWorldType::PlayInEditor;
    Params.SessionDestination = EPlaySessionDestinationType::InProcess;
    Params.bAllowOnlineSubsystem = bAllowOnlineSubsystem;

    FString MapOverride;
    if (Args.IsValid()
        && (Args->TryGetStringField(TEXT("map"), MapOverride)
            || Args->TryGetStringField(TEXT("map_path"), MapOverride)))
    {
        Params.GlobalMapOverride = MapOverride;
    }

    bool bDestinationViewportSet = false;
    if (bSelectedViewport)
    {
        FLevelEditorModule* LevelEditorModule = FModuleManager::Get().GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (LevelEditorModule == nullptr)
        {
            LevelEditorModule = &FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
        }
        if (LevelEditorModule != nullptr)
        {
            TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule->GetFirstActiveViewport();
            if (ActiveLevelViewport.IsValid())
            {
                Params.DestinationSlateViewport = ActiveLevelViewport;
                bDestinationViewportSet = true;
            }
            else
            {
                Warnings.Add(TEXT("selected_viewport requested but no active level viewport was found; Unreal may open a PIE window"));
            }
        }
    }

    GEditor->RequestPlaySession(Params);

    UE_LOG(LogSageBridge, Log, TEXT("PIE start requested with transient play settings"));

    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("requested"), true);
    Result->SetBoolField(TEXT("used_transient_play_settings"), true);
    Result->SetBoolField(TEXT("restore_settings_after_start"), bRestoreSettingsAfterStart);
    Result->SetStringField(
        TEXT("restore_settings_note"),
        TEXT("run_pie uses a transient ULevelEditorPlaySettings copy, so project/editor defaults are not mutated"));
    Result->SetBoolField(TEXT("force_local_player_applied"), bForceLocalPlayerApplied);
    Result->SetBoolField(TEXT("selected_viewport_requested"), bSelectedViewport);
    Result->SetBoolField(TEXT("destination_viewport_set"), bDestinationViewportSet);
    Result->SetBoolField(TEXT("new_editor_window_requested"), bNewEditorWindow);
    Result->SetNumberField(TEXT("target_local_player_index"), RequestedLocalPlayerIndex);
    Result->SetBoolField(TEXT("allow_online_subsystem"), bAllowOnlineSubsystem);
    if (!MapOverride.IsEmpty())
    {
        Result->SetStringField(TEXT("global_map_override"), MapOverride);
    }
    Result->SetObjectField(TEXT("default_play_settings_before"), PlaySettingsToJson(Defaults));
    Result->SetObjectField(TEXT("effective_play_settings"), PlaySettingsToJson(PlaySettings));
    AddStringArrayField(Result, TEXT("warnings"), Warnings);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- stop_pie -------------------------------------------------------------

FSageToolDispatch::FOutcome StopPieOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    if (GEditor->PlayWorld == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("PIE not active"));
    }

    bool bCleanupPythonRefs = true;
    bool bClearMainGlobals = true;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("cleanup_python_refs"), bCleanupPythonRefs);
        Args->TryGetBoolField(TEXT("clear_python_main_globals"), bClearMainGlobals);
    }

    detail::FPythonReferenceCleanupReport CleanupReport;
    if (bCleanupPythonRefs)
    {
        CleanupReport = detail::CleanupPythonReferences(
            bClearMainGlobals,
            /*bIncludePieWorlds=*/true,
            /*bIncludeEditorWorld=*/false,
            /*bCollectUnrealGarbage=*/true);
        if (CleanupReport.bPythonCommandRan && !CleanupReport.bPythonCommandSucceeded)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                FString::Printf(TEXT("Python reference cleanup failed before stop_pie: %s"),
                                *CleanupReport.Error));
        }
    }

    GEditor->RequestEndPlayMap();
    UE_LOG(LogSageBridge, Log, TEXT("PIE end requested"));

    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("requested"), true);
    if (bCleanupPythonRefs)
    {
        AddPythonCleanupReport(Result, CleanupReport);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- handlers --------------------------------------------------------------

FSageToolDispatch::FOutcome GetWorldHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetWorldOnGameThread(Args); });
}
FSageToolDispatch::FOutcome GetPieStateHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetPieStateOnGameThread(Args); });
}
FSageToolDispatch::FOutcome GetPlaySettingsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetPlaySettingsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome GetViewportStateHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetViewportStateOnGameThread(Args); });
}
FSageToolDispatch::FOutcome GetSelectedActorsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetSelectedActorsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome SelectActorsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return SelectActorsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome ClearSelectionHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return ClearSelectionOnGameThread(Args); });
}
FSageToolDispatch::FOutcome SaveLevelHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return SaveLevelOnGameThread(Args); });
}
FSageToolDispatch::FOutcome GetCurrentLevelHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetCurrentLevelOnGameThread(Args); });
}
FSageToolDispatch::FOutcome RunPieHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return RunPieOnGameThread(Args); });
}
FSageToolDispatch::FOutcome StopPieHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return StopPieOnGameThread(Args); });
}

}  // namespace

void RegisterEditorTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("get_world"),            &GetWorldHandler);
    Dispatch.RegisterHandler(TEXT("get_pie_state"),        &GetPieStateHandler);
    Dispatch.RegisterHandler(TEXT("editor.get_play_settings"), &GetPlaySettingsHandler);
    Dispatch.RegisterHandler(TEXT("get_viewport_state"),   &GetViewportStateHandler);
    Dispatch.RegisterHandler(TEXT("get_selected_actors"),  &GetSelectedActorsHandler);
    Dispatch.RegisterHandler(TEXT("select_actors"),        &SelectActorsHandler);
    Dispatch.RegisterHandler(TEXT("clear_selection"),      &ClearSelectionHandler);
    Dispatch.RegisterHandler(TEXT("save_level"),           &SaveLevelHandler);
    Dispatch.RegisterHandler(TEXT("get_current_level"),    &GetCurrentLevelHandler);
    Dispatch.RegisterHandler(TEXT("run_pie"),              &RunPieHandler);
    Dispatch.RegisterHandler(TEXT("stop_pie"),             &StopPieHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
