#include "Tools/SageToolHelpers.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace sage::tools::detail
{

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

UActorComponent* ResolveComponent(const FString& ComponentPath)
{
    if (ComponentPath.IsEmpty()) return nullptr;
    if (UObject* Obj = StaticFindObject(UActorComponent::StaticClass(), nullptr, *ComponentPath))
    {
        return Cast<UActorComponent>(Obj);
    }
    FSoftObjectPath SoftPath(ComponentPath);
    return Cast<UActorComponent>(SoftPath.ResolveObject());
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

bool ParseVector3(const TSharedPtr<FJsonObject>& Args,
                  const FString& FieldName, FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Args->TryGetArrayField(FieldName, Arr) || Arr->Num() != 3) return false;
    Out.X = (*Arr)[0]->AsNumber();
    Out.Y = (*Arr)[1]->AsNumber();
    Out.Z = (*Arr)[2]->AsNumber();
    return true;
}

bool ParseRotator3(const TSharedPtr<FJsonObject>& Args,
                   const FString& FieldName, FRotator& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Args->TryGetArrayField(FieldName, Arr) || Arr->Num() != 3) return false;
    Out.Pitch = (*Arr)[0]->AsNumber();
    Out.Yaw   = (*Arr)[1]->AsNumber();
    Out.Roll  = (*Arr)[2]->AsNumber();
    return true;
}

TSharedRef<FJsonValue> Vec3ToJson(const FVector& V)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(V.X));
    Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
    Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
    return MakeShared<FJsonValueArray>(Arr);
}

TSharedRef<FJsonValue> Rot3ToJson(const FRotator& R)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
    Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
    Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
    return MakeShared<FJsonValueArray>(Arr);
}

bool SetUPropertyFromJson(UObject* Container,
                          FProperty* Property,
                          const TSharedPtr<FJsonValue>& Value)
{
    if (Property == nullptr || Container == nullptr || !Value.IsValid())
    {
        return false;
    }

    if (FBoolProperty* P = CastField<FBoolProperty>(Property))
    {
        P->SetPropertyValue_InContainer(Container, Value->AsBool());
        return true;
    }
    if (FIntProperty* P = CastField<FIntProperty>(Property))
    {
        P->SetPropertyValue_InContainer(Container, static_cast<int32>(Value->AsNumber()));
        return true;
    }
    if (FInt64Property* P = CastField<FInt64Property>(Property))
    {
        P->SetPropertyValue_InContainer(Container, static_cast<int64>(Value->AsNumber()));
        return true;
    }
    if (FFloatProperty* P = CastField<FFloatProperty>(Property))
    {
        P->SetPropertyValue_InContainer(Container, static_cast<float>(Value->AsNumber()));
        return true;
    }
    if (FDoubleProperty* P = CastField<FDoubleProperty>(Property))
    {
        P->SetPropertyValue_InContainer(Container, Value->AsNumber());
        return true;
    }
    if (FStrProperty* P = CastField<FStrProperty>(Property))
    {
        P->SetPropertyValue_InContainer(Container, Value->AsString());
        return true;
    }
    if (FNameProperty* P = CastField<FNameProperty>(Property))
    {
        P->SetPropertyValue_InContainer(Container, FName(*Value->AsString()));
        return true;
    }
    if (FTextProperty* P = CastField<FTextProperty>(Property))
    {
        P->SetPropertyValue_InContainer(Container, FText::FromString(Value->AsString()));
        return true;
    }
    if (FByteProperty* P = CastField<FByteProperty>(Property))
    {
        P->SetPropertyValue_InContainer(Container, static_cast<uint8>(Value->AsNumber()));
        return true;
    }
    return false;
}

TSharedPtr<FJsonValue> GetUPropertyAsJson(const UObject* Container,
                                           const FProperty* Property)
{
    if (Property == nullptr || Container == nullptr) return nullptr;

    if (const FBoolProperty* P = CastField<FBoolProperty>(Property))
        return MakeShared<FJsonValueBoolean>(P->GetPropertyValue_InContainer(Container));
    if (const FIntProperty* P = CastField<FIntProperty>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(
            P->GetPropertyValue_InContainer(Container)));
    if (const FInt64Property* P = CastField<FInt64Property>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(
            P->GetPropertyValue_InContainer(Container)));
    if (const FFloatProperty* P = CastField<FFloatProperty>(Property))
        return MakeShared<FJsonValueNumber>(P->GetPropertyValue_InContainer(Container));
    if (const FDoubleProperty* P = CastField<FDoubleProperty>(Property))
        return MakeShared<FJsonValueNumber>(P->GetPropertyValue_InContainer(Container));
    if (const FStrProperty* P = CastField<FStrProperty>(Property))
        return MakeShared<FJsonValueString>(P->GetPropertyValue_InContainer(Container));
    if (const FNameProperty* P = CastField<FNameProperty>(Property))
        return MakeShared<FJsonValueString>(
            P->GetPropertyValue_InContainer(Container).ToString());
    if (const FTextProperty* P = CastField<FTextProperty>(Property))
        return MakeShared<FJsonValueString>(
            P->GetPropertyValue_InContainer(Container).ToString());
    if (const FByteProperty* P = CastField<FByteProperty>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(
            P->GetPropertyValue_InContainer(Container)));
    return nullptr;
}

bool JsonValuesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
{
    const bool AValid = A.IsValid();
    const bool BValid = B.IsValid();
    if (!AValid || !BValid) return AValid == BValid;
    if (A->Type != B->Type) return false;
    switch (A->Type)
    {
    case EJson::Boolean: return A->AsBool() == B->AsBool();
    case EJson::Number:  return FMath::IsNearlyEqual(A->AsNumber(), B->AsNumber());
    case EJson::String:  return A->AsString() == B->AsString();
    case EJson::Null:    return true;
    default:             return false;  // Object/Array unsupported in CAS path
    }
}

}  // namespace sage::tools::detail
