#include "Tools/SageEditorTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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
                Result->SetNumberField(TEXT("actor_count"), Level->Actors.Num());
            }
        }
    }
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

}  // namespace

void RegisterEditorTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("get_world"),            &GetWorldHandler);
    Dispatch.RegisterHandler(TEXT("get_pie_state"),        &GetPieStateHandler);
    Dispatch.RegisterHandler(TEXT("get_viewport_state"),   &GetViewportStateHandler);
    Dispatch.RegisterHandler(TEXT("get_selected_actors"),  &GetSelectedActorsHandler);
    Dispatch.RegisterHandler(TEXT("select_actors"),        &SelectActorsHandler);
    Dispatch.RegisterHandler(TEXT("clear_selection"),      &ClearSelectionHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
