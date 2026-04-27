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
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

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

    // Reject mutations during PIE per api-spec.md §Error Codes (-32004).
    if (GEditor->PlayWorld != nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32004,
            TEXT("PIE active; mutation rejected"));
    }

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

FSageToolDispatch::FOutcome SpawnActorHandler(const TSharedPtr<FJsonObject>& Args)
{
    if (IsInGameThread())
    {
        return SpawnActorOnGameThread(Args);
    }
    auto Future = Async(EAsyncExecution::TaskGraphMainThread,
        [Args]() { return SpawnActorOnGameThread(Args); });
    return Future.Get();
}

}  // namespace

void RegisterActorTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("spawn_actor"), &SpawnActorHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
