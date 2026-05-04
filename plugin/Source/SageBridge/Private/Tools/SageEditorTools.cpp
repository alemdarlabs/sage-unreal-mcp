#include "Tools/SageEditorTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "PlayInEditorDataTypes.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

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
    if (bActive)
    {
        Result->SetStringField(TEXT("play_world_path"), GEditor->PlayWorld->GetPathName());
    }
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

FSageToolDispatch::FOutcome RunPieOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    if (GEditor->PlayWorld != nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("PIE already active"));
    }

    FRequestPlaySessionParams Params;
    GEditor->RequestPlaySession(Params);

    UE_LOG(LogSageBridge, Log, TEXT("PIE start requested"));

    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("requested"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- stop_pie -------------------------------------------------------------

FSageToolDispatch::FOutcome StopPieOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    if (GEditor->PlayWorld == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("PIE not active"));
    }

    GEditor->RequestEndPlayMap();
    UE_LOG(LogSageBridge, Log, TEXT("PIE end requested"));

    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("requested"), true);
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
