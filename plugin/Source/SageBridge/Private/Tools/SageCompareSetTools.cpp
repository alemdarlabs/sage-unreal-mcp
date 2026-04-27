#include "Tools/SageCompareSetTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

UObject* ResolveTargetByKind(const FString& TargetKind, const FString& TargetId)
{
    if (TargetKind == TEXT("actor"))
    {
        return detail::ResolveActor(TargetId);
    }
    if (TargetKind == TEXT("component"))
    {
        return detail::ResolveComponent(TargetId);
    }
    if (TargetKind == TEXT("asset"))
    {
        if (GEditor == nullptr) return nullptr;
        if (UEditorAssetSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
        {
            return Sub->LoadAsset(TargetId);
        }
    }
    return nullptr;
}

FSageToolDispatch::FOutcome MakeVersionConflict(
    const FString& TargetKind,
    const FString& TargetId,
    const FString& PropName,
    const TSharedPtr<FJsonValue>& Current,
    const TSharedPtr<FJsonValue>& Expected)
{
    auto Err = MakeShared<FJsonObject>();
    Err->SetNumberField(TEXT("code"), -32003);
    Err->SetStringField(TEXT("message"),
        FString::Printf(TEXT("version conflict on %s.%s: expected != current"),
                         *TargetId, *PropName));
    auto Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("target_kind"), TargetKind);
    Data->SetStringField(TEXT("target_id"),   TargetId);
    Data->SetStringField(TEXT("property"),    PropName);
    if (Current.IsValid())  Data->SetField(TEXT("current"),  Current);
    if (Expected.IsValid()) Data->SetField(TEXT("expected"), Expected);
    Err->SetObjectField(TEXT("data"), Data);
    return FSageToolDispatch::FOutcome{false, nullptr, Err};
}

FSageToolDispatch::FOutcome CompareAndSetPropertyOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString TargetKind, TargetId, PropName;
    if (!Args->TryGetStringField(TEXT("target_kind"), TargetKind) || TargetKind.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'target_kind' (one of: actor, component, asset)"));
    }
    if (!Args->TryGetStringField(TEXT("target_id"), TargetId) || TargetId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'target_id'"));
    }
    if (!Args->TryGetStringField(TEXT("property"), PropName) || PropName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'property'"));
    }
    const TSharedPtr<FJsonValue> ExpectedField = Args->Values.FindRef(TEXT("expected"));
    const TSharedPtr<FJsonValue> NewField      = Args->Values.FindRef(TEXT("new_value"));
    if (!ExpectedField.IsValid() || !NewField.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'expected' and/or 'new_value'"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    UObject* Target = ResolveTargetByKind(TargetKind, TargetId);
    if (Target == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("target not found (%s): %s"), *TargetKind, *TargetId));
    }

    FProperty* Property = Target->GetClass()->FindPropertyByName(*PropName);
    if (Property == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("property not found: %s on %s"),
                             *PropName, *Target->GetClass()->GetName()));
    }

    const TSharedPtr<FJsonValue> Current = detail::GetUPropertyAsJson(Target, Property);
    if (!Current.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unsupported property type for read (%s)"),
                             *Property->GetClass()->GetName()));
    }

    if (!detail::JsonValuesEqual(Current, ExpectedField))
    {
        return MakeVersionConflict(TargetKind, TargetId, PropName, Current, ExpectedField);
    }

    FScopedTransaction Tx(LOCTEXT("CompareAndSet", "Sage: Compare And Set"));
    Target->Modify();
    Target->PreEditChange(Property);

    if (!detail::SetUPropertyFromJson(Target, Property, NewField))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unsupported property type for write (%s)"),
                             *Property->GetClass()->GetName()));
    }

    FPropertyChangedEvent ChangeEvent(Property);
    Target->PostEditChangeProperty(ChangeEvent);
    if (TargetKind == TEXT("asset"))
    {
        Target->MarkPackageDirty();
    }

    UE_LOG(LogSageBridge, Log, TEXT("CAS: %s.%s set on %s"),
           *TargetId, *PropName, *Target->GetClass()->GetName());

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("target_kind"), TargetKind);
    Result->SetStringField(TEXT("target_id"),   TargetId);
    Result->SetStringField(TEXT("property"),    PropName);
    Result->SetField(TEXT("previous"), Current);
    Result->SetField(TEXT("value"),    NewField);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

FSageToolDispatch::FOutcome CompareAndSetPropertyHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return CompareAndSetPropertyOnGameThread(Args); });
}

}  // namespace

void RegisterCompareSetTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("compare_and_set_property"),
                              &CompareAndSetPropertyHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
