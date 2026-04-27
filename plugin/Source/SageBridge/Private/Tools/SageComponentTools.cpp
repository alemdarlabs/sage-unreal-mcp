#include "Tools/SageComponentTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

// ---- add_component ---------------------------------------------------------

FSageToolDispatch::FOutcome AddComponentOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }

    FString ActorPath, ClassPath, ComponentName;
    if (!Args->TryGetStringField(TEXT("actor_id"), ActorPath) || ActorPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    }
    if (!Args->TryGetStringField(TEXT("component_class"), ClassPath) || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'component_class'"));
    }
    Args->TryGetStringField(TEXT("component_name"), ComponentName);

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    AActor* Owner = detail::ResolveActor(ActorPath);
    if (Owner == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("actor not found: %s"), *ActorPath));
    }

    UClass* CompClass = LoadClass<UActorComponent>(nullptr, *ClassPath);
    if (CompClass == nullptr)
    {
        CompClass = StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ClassPath);
    }
    if (CompClass == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("component class not found: %s"), *ClassPath));
    }

    FScopedTransaction Tx(LOCTEXT("AddComponent", "Sage: Add Component"));
    Owner->Modify();

    const FName CompName = ComponentName.IsEmpty() ? NAME_None : FName(*ComponentName);
    UActorComponent* NewComp = NewObject<UActorComponent>(
        Owner, CompClass, CompName, RF_Transactional);
    if (NewComp == nullptr)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("NewObject failed"));
    }

    NewComp->OnComponentCreated();
    NewComp->RegisterComponent();
    Owner->AddInstanceComponent(NewComp);

    UE_LOG(LogSageBridge, Log, TEXT("Added component: %s (class=%s) on %s"),
           *NewComp->GetPathName(), *CompClass->GetPathName(), *Owner->GetPathName());

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("component_id"), NewComp->GetPathName());
    Result->SetStringField(TEXT("actor_id"),     Owner->GetPathName());
    Result->SetStringField(TEXT("class"),        CompClass->GetPathName());
    Result->SetStringField(TEXT("name"),         NewComp->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- remove_component ------------------------------------------------------

FSageToolDispatch::FOutcome RemoveComponentOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString ComponentPath;
    if (!Args->TryGetStringField(TEXT("component_id"), ComponentPath) || ComponentPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'component_id'"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    UActorComponent* Comp = detail::ResolveComponent(ComponentPath);
    if (Comp == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("component not found: %s"), *ComponentPath));
    }
    AActor* Owner = Comp->GetOwner();

    const FString ResolvedPath = Comp->GetPathName();
    const FString ResolvedName = Comp->GetName();

    FScopedTransaction Tx(LOCTEXT("RemoveComponent", "Sage: Remove Component"));
    if (Owner != nullptr)
    {
        Owner->Modify();
    }
    Comp->Modify();
    Comp->UnregisterComponent();
    if (Owner != nullptr)
    {
        Owner->RemoveInstanceComponent(Comp);
    }
    Comp->DestroyComponent();

    UE_LOG(LogSageBridge, Log, TEXT("Removed component: %s"), *ResolvedPath);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("removed"), ResolvedPath);
    Result->SetStringField(TEXT("name"),    ResolvedName);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- modify_component_property --------------------------------------------

FSageToolDispatch::FOutcome ModifyComponentPropertyOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString ComponentPath, PropName;
    if (!Args->TryGetStringField(TEXT("component_id"), ComponentPath) || ComponentPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'component_id'"));
    }
    if (!Args->TryGetStringField(TEXT("property"), PropName) || PropName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'property'"));
    }
    const TSharedPtr<FJsonValue> ValueField = Args->Values.FindRef(TEXT("value"));
    if (!ValueField.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    UActorComponent* Comp = detail::ResolveComponent(ComponentPath);
    if (Comp == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("component not found: %s"), *ComponentPath));
    }

    FProperty* Property = Comp->GetClass()->FindPropertyByName(*PropName);
    if (Property == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("property not found: %s on %s"),
                            *PropName, *Comp->GetClass()->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("ModifyComponentProperty", "Sage: Modify Component Property"));
    Comp->Modify();
    Comp->PreEditChange(Property);

    if (!detail::SetUPropertyFromJson(Comp, Property, ValueField))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unsupported property type for '%s' (got %s)"),
                            *PropName, *Property->GetClass()->GetName()));
    }

    FPropertyChangedEvent ChangeEvent(Property);
    Comp->PostEditChangeProperty(ChangeEvent);

    UE_LOG(LogSageBridge, Log, TEXT("Modified component property '%s' on %s"),
           *PropName, *Comp->GetPathName());

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("component_id"), Comp->GetPathName());
    Result->SetStringField(TEXT("property"),     PropName);
    Result->SetField(TEXT("value"), ValueField);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- attach (scene component to scene component) --------------------------

FSageToolDispatch::FOutcome AttachOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString ChildPath, ParentPath, SocketName;
    if (!Args->TryGetStringField(TEXT("child_id"), ChildPath) || ChildPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'child_id'"));
    }
    if (!Args->TryGetStringField(TEXT("parent_id"), ParentPath) || ParentPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'parent_id'"));
    }
    Args->TryGetStringField(TEXT("socket"), SocketName);

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    USceneComponent* Child  = Cast<USceneComponent>(detail::ResolveComponent(ChildPath));
    USceneComponent* Parent = Cast<USceneComponent>(detail::ResolveComponent(ParentPath));
    if (Child == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("child SceneComponent not found: %s"), *ChildPath));
    }
    if (Parent == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("parent SceneComponent not found: %s"), *ParentPath));
    }

    FScopedTransaction Tx(LOCTEXT("AttachComponent", "Sage: Attach Component"));
    Child->Modify();
    if (Child->GetOwner() != nullptr) Child->GetOwner()->Modify();

    const FAttachmentTransformRules Rules = FAttachmentTransformRules::KeepRelativeTransform;
    const FName Socket = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);
    Child->AttachToComponent(Parent, Rules, Socket);

    UE_LOG(LogSageBridge, Log, TEXT("Attached %s to %s (socket=%s)"),
           *Child->GetPathName(), *Parent->GetPathName(), *Socket.ToString());

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("child_id"),  Child->GetPathName());
    Result->SetStringField(TEXT("parent_id"), Parent->GetPathName());
    Result->SetStringField(TEXT("socket"),    Socket.ToString());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- detach ----------------------------------------------------------------

FSageToolDispatch::FOutcome DetachOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString ChildPath;
    if (!Args->TryGetStringField(TEXT("child_id"), ChildPath) || ChildPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'child_id'"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    USceneComponent* Child = Cast<USceneComponent>(detail::ResolveComponent(ChildPath));
    if (Child == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("child SceneComponent not found: %s"), *ChildPath));
    }

    FScopedTransaction Tx(LOCTEXT("DetachComponent", "Sage: Detach Component"));
    Child->Modify();
    if (Child->GetOwner() != nullptr) Child->GetOwner()->Modify();

    Child->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);

    UE_LOG(LogSageBridge, Log, TEXT("Detached %s"), *Child->GetPathName());

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("child_id"), Child->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- handlers (thread marshalling) ----------------------------------------

FSageToolDispatch::FOutcome AddComponentHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return AddComponentOnGameThread(Args); });
}
FSageToolDispatch::FOutcome RemoveComponentHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return RemoveComponentOnGameThread(Args); });
}
FSageToolDispatch::FOutcome ModifyComponentPropertyHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return ModifyComponentPropertyOnGameThread(Args); });
}
FSageToolDispatch::FOutcome AttachHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return AttachOnGameThread(Args); });
}
FSageToolDispatch::FOutcome DetachHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return DetachOnGameThread(Args); });
}

}  // namespace

void RegisterComponentTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("add_component"),               &AddComponentHandler);
    Dispatch.RegisterHandler(TEXT("remove_component"),            &RemoveComponentHandler);
    Dispatch.RegisterHandler(TEXT("modify_component_property"),   &ModifyComponentPropertyHandler);
    Dispatch.RegisterHandler(TEXT("attach"),                       &AttachHandler);
    Dispatch.RegisterHandler(TEXT("detach"),                       &DetachHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
