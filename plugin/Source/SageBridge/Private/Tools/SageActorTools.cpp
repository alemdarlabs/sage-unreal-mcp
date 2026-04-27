#include "Tools/SageActorTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

// ---- helpers ---------------------------------------------------------------

bool ParseVector3(const TSharedPtr<FJsonObject>& Args,
                  const FString& FieldName,
                  FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Args->TryGetArrayField(FieldName, Arr) || Arr->Num() != 3)
    {
        return false;
    }
    Out.X = (*Arr)[0]->AsNumber();
    Out.Y = (*Arr)[1]->AsNumber();
    Out.Z = (*Arr)[2]->AsNumber();
    return true;
}

bool ParseRotator3(const TSharedPtr<FJsonObject>& Args,
                   const FString& FieldName,
                   FRotator& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Args->TryGetArrayField(FieldName, Arr) || Arr->Num() != 3)
    {
        return false;
    }
    Out.Pitch = (*Arr)[0]->AsNumber();
    Out.Yaw   = (*Arr)[1]->AsNumber();
    Out.Roll  = (*Arr)[2]->AsNumber();
    return true;
}

TSharedRef<FJsonValueArray> Vec3ToJson(const FVector& V)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(V.X));
    Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
    Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
    return MakeShared<FJsonValueArray>(Arr);
}

TSharedRef<FJsonValueArray> Rot3ToJson(const FRotator& R)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
    Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
    Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
    return MakeShared<FJsonValueArray>(Arr);
}

AActor* ResolveActor(const FString& ActorPath)
{
    if (ActorPath.IsEmpty()) return nullptr;
    if (UObject* Obj = StaticFindObject(AActor::StaticClass(), nullptr, *ActorPath))
    {
        return Cast<AActor>(Obj);
    }
    FSoftObjectPath SoftPath(ActorPath);
    return Cast<AActor>(SoftPath.ResolveObject());
}

bool RejectIfPie(FSageToolDispatch::FOutcome& OutErr)
{
    if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
    {
        OutErr = FSageToolDispatch::FOutcome::MakeError(-32004,
            TEXT("PIE active; mutation rejected"));
        return true;
    }
    return false;
}

// ---- spawn_actor -----------------------------------------------------------

FSageToolDispatch::FOutcome SpawnActorOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }

    FString ClassPath;
    if (!Args->TryGetStringField(TEXT("class"), ClassPath) || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'class' field (e.g. /Script/Engine.StaticMeshActor)"));
    }

    FVector Location = FVector::ZeroVector;
    ParseVector3(Args, TEXT("location"), Location);

    FRotator Rotation = FRotator::ZeroRotator;
    ParseRotator3(Args, TEXT("rotation"), Rotation);

    FString Label;
    Args->TryGetStringField(TEXT("label"), Label);

    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (RejectIfPie(PieErr)) return PieErr;

    UClass* SpawnClass = LoadClass<AActor>(nullptr, *ClassPath);
    if (SpawnClass == nullptr)
    {
        SpawnClass = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
    }
    if (SpawnClass == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class not found: %s"), *ClassPath));
    }

    UEditorActorSubsystem* EditorActor =
        GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (EditorActor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EditorActorSubsystem unavailable"));
    }

    FScopedTransaction Transaction(LOCTEXT("SpawnActor", "Sage: Spawn Actor"));

    AActor* Spawned = EditorActor->SpawnActorFromClass(
        SpawnClass, Location, Rotation, /*bTransient=*/false);
    if (Spawned == nullptr)
    {
        Transaction.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("spawn failed"));
    }

    Spawned->Modify();
    if (!Label.IsEmpty())
    {
        Spawned->SetActorLabel(Label);
    }

    UE_LOG(LogSageBridge, Log, TEXT("Spawned actor: %s (class=%s, label='%s')"),
           *Spawned->GetPathName(), *SpawnClass->GetPathName(), *Spawned->GetActorLabel());

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_id"), Spawned->GetPathName());
    Result->SetStringField(TEXT("label"),    Spawned->GetActorLabel());
    Result->SetStringField(TEXT("class"),    SpawnClass->GetPathName());
    Result->SetField(TEXT("location"), Vec3ToJson(Spawned->GetActorLocation()));
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- delete_actor ----------------------------------------------------------

FSageToolDispatch::FOutcome DeleteActorOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }

    FString ActorPath;
    if (!Args->TryGetStringField(TEXT("actor_id"), ActorPath) || ActorPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    }

    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (RejectIfPie(PieErr)) return PieErr;

    AActor* Actor = ResolveActor(ActorPath);
    if (Actor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("actor not found: %s"), *ActorPath));
    }

    UEditorActorSubsystem* EditorActor =
        GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (EditorActor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EditorActorSubsystem unavailable"));
    }

    const FString DestroyedPath = Actor->GetPathName();
    const FString DestroyedLabel = Actor->GetActorLabel();

    FScopedTransaction Transaction(LOCTEXT("DeleteActor", "Sage: Delete Actor"));
    Actor->Modify();
    const bool bDestroyed = EditorActor->DestroyActor(Actor);
    if (!bDestroyed)
    {
        Transaction.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("destroy failed"));
    }

    UE_LOG(LogSageBridge, Log, TEXT("Destroyed actor: %s"), *DestroyedPath);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("destroyed"), DestroyedPath);
    Result->SetStringField(TEXT("label"),     DestroyedLabel);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- set_transform ---------------------------------------------------------

FSageToolDispatch::FOutcome SetTransformOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }

    FString ActorPath;
    if (!Args->TryGetStringField(TEXT("actor_id"), ActorPath) || ActorPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (RejectIfPie(PieErr)) return PieErr;

    AActor* Actor = ResolveActor(ActorPath);
    if (Actor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("actor not found: %s"), *ActorPath));
    }

    FTransform New = Actor->GetActorTransform();
    bool bChanged = false;

    FVector Loc;
    if (ParseVector3(Args, TEXT("location"), Loc))
    {
        New.SetLocation(Loc);
        bChanged = true;
    }
    FRotator Rot;
    if (ParseRotator3(Args, TEXT("rotation"), Rot))
    {
        New.SetRotation(Rot.Quaternion());
        bChanged = true;
    }
    FVector Scale;
    if (ParseVector3(Args, TEXT("scale"), Scale))
    {
        New.SetScale3D(Scale);
        bChanged = true;
    }

    if (!bChanged)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("at least one of 'location'/'rotation'/'scale' required"));
    }

    FScopedTransaction Transaction(LOCTEXT("SetTransform", "Sage: Set Transform"));
    Actor->Modify();
    Actor->SetActorTransform(New);

    UE_LOG(LogSageBridge, Log, TEXT("Set transform on %s"), *Actor->GetPathName());

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_id"), Actor->GetPathName());
    Result->SetField(TEXT("location"), Vec3ToJson(Actor->GetActorLocation()));
    Result->SetField(TEXT("rotation"), Rot3ToJson(Actor->GetActorRotation()));
    Result->SetField(TEXT("scale"),    Vec3ToJson(Actor->GetActorScale3D()));
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- thread marshalling ----------------------------------------------------

template <typename Fn>
FSageToolDispatch::FOutcome RunOnGameThread(Fn&& Body)
{
    if (IsInGameThread())
    {
        return Body();
    }
    auto Future = Async(EAsyncExecution::TaskGraphMainThread, std::forward<Fn>(Body));
    return Future.Get();
}

FSageToolDispatch::FOutcome SpawnActorHandler(const TSharedPtr<FJsonObject>& Args)
{
    return RunOnGameThread([Args]() { return SpawnActorOnGameThread(Args); });
}

FSageToolDispatch::FOutcome DeleteActorHandler(const TSharedPtr<FJsonObject>& Args)
{
    return RunOnGameThread([Args]() { return DeleteActorOnGameThread(Args); });
}

FSageToolDispatch::FOutcome SetTransformHandler(const TSharedPtr<FJsonObject>& Args)
{
    return RunOnGameThread([Args]() { return SetTransformOnGameThread(Args); });
}

}  // namespace

void RegisterActorTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("spawn_actor"),   &SpawnActorHandler);
    Dispatch.RegisterHandler(TEXT("delete_actor"),  &DeleteActorHandler);
    Dispatch.RegisterHandler(TEXT("set_transform"), &SetTransformHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
