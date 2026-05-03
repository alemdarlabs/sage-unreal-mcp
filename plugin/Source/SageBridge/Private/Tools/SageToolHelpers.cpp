#include "Tools/SageToolHelpers.h"

#include "SageBridge.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Math/Color.h"
#include "Math/IntPoint.h"
#include "Math/IntVector.h"
#include "Math/Transform.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
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

namespace
{

UObject* ResolveAssetPath(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    if (UObject* Obj = Soft.ResolveObject()) return Obj;
    return Soft.TryLoad();
}

UClass* ResolveClassPath(const FString& Path, UClass* MetaClass)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    UClass* AsClass = Cast<UClass>(Obj);
    if (!AsClass) return nullptr;
    if (MetaClass && !AsClass->IsChildOf(MetaClass)) return nullptr;
    return AsClass;
}

// Shorthand JSON form for the half-dozen UE math structs the agent uses
// constantly. Returns nullptr if the struct isn't one we shorthand —
// caller falls back to USTRUCT ImportText/ExportText.
TSharedPtr<FJsonValue> StructShorthandToJson(UScriptStruct* Struct, const void* ValuePtr)
{
    if (Struct == TBaseStructure<FVector>::Get())
    {
        return Vec3ToJson(*static_cast<const FVector*>(ValuePtr));
    }
    if (Struct == TBaseStructure<FRotator>::Get())
    {
        return Rot3ToJson(*static_cast<const FRotator*>(ValuePtr));
    }
    if (Struct == TBaseStructure<FVector2D>::Get())
    {
        const auto* V = static_cast<const FVector2D*>(ValuePtr);
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(V->X));
        A.Add(MakeShared<FJsonValueNumber>(V->Y));
        return MakeShared<FJsonValueArray>(A);
    }
    if (Struct == TBaseStructure<FLinearColor>::Get())
    {
        const auto* C = static_cast<const FLinearColor*>(ValuePtr);
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(C->R));
        A.Add(MakeShared<FJsonValueNumber>(C->G));
        A.Add(MakeShared<FJsonValueNumber>(C->B));
        A.Add(MakeShared<FJsonValueNumber>(C->A));
        return MakeShared<FJsonValueArray>(A);
    }
    if (Struct == TBaseStructure<FColor>::Get())
    {
        const auto* C = static_cast<const FColor*>(ValuePtr);
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(C->R));
        A.Add(MakeShared<FJsonValueNumber>(C->G));
        A.Add(MakeShared<FJsonValueNumber>(C->B));
        A.Add(MakeShared<FJsonValueNumber>(C->A));
        return MakeShared<FJsonValueArray>(A);
    }
    if (Struct == TBaseStructure<FIntPoint>::Get())
    {
        const auto* V = static_cast<const FIntPoint*>(ValuePtr);
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(V->X));
        A.Add(MakeShared<FJsonValueNumber>(V->Y));
        return MakeShared<FJsonValueArray>(A);
    }
    if (Struct == TBaseStructure<FIntVector>::Get())
    {
        const auto* V = static_cast<const FIntVector*>(ValuePtr);
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(V->X));
        A.Add(MakeShared<FJsonValueNumber>(V->Y));
        A.Add(MakeShared<FJsonValueNumber>(V->Z));
        return MakeShared<FJsonValueArray>(A);
    }
    if (Struct == TBaseStructure<FTransform>::Get())
    {
        const auto* T = static_cast<const FTransform*>(ValuePtr);
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetField(TEXT("location"), Vec3ToJson(T->GetLocation()));
        Obj->SetField(TEXT("rotation"), Rot3ToJson(T->GetRotation().Rotator()));
        Obj->SetField(TEXT("scale"),    Vec3ToJson(T->GetScale3D()));
        return MakeShared<FJsonValueObject>(Obj);
    }
    return nullptr;
}

bool JsonToStructShorthand(UScriptStruct* Struct, void* ValuePtr,
                            const TSharedPtr<FJsonValue>& Val)
{
    if (!Val.IsValid()) return false;

    auto AsNumberArray = [&](int32 ExpectedLen, double* Out) -> bool {
        if (Val->Type != EJson::Array) return false;
        const auto& A = Val->AsArray();
        if (A.Num() != ExpectedLen) return false;
        for (int32 i = 0; i < ExpectedLen; ++i) Out[i] = A[i]->AsNumber();
        return true;
    };

    if (Struct == TBaseStructure<FVector>::Get())
    {
        double Buf[3]{};
        if (!AsNumberArray(3, Buf)) return false;
        *static_cast<FVector*>(ValuePtr) = FVector(Buf[0], Buf[1], Buf[2]);
        return true;
    }
    if (Struct == TBaseStructure<FRotator>::Get())
    {
        double Buf[3]{};
        if (!AsNumberArray(3, Buf)) return false;
        *static_cast<FRotator*>(ValuePtr) = FRotator(Buf[0], Buf[1], Buf[2]);
        return true;
    }
    if (Struct == TBaseStructure<FVector2D>::Get())
    {
        double Buf[2]{};
        if (!AsNumberArray(2, Buf)) return false;
        *static_cast<FVector2D*>(ValuePtr) = FVector2D(Buf[0], Buf[1]);
        return true;
    }
    if (Struct == TBaseStructure<FLinearColor>::Get())
    {
        double Buf[4]{0,0,0,1};
        if (Val->Type != EJson::Array) return false;
        const auto& A = Val->AsArray();
        if (A.Num() != 3 && A.Num() != 4) return false;
        for (int32 i = 0; i < A.Num(); ++i) Buf[i] = A[i]->AsNumber();
        *static_cast<FLinearColor*>(ValuePtr) =
            FLinearColor(Buf[0], Buf[1], Buf[2], Buf[3]);
        return true;
    }
    if (Struct == TBaseStructure<FColor>::Get())
    {
        double Buf[4]{0,0,0,255};
        if (Val->Type != EJson::Array) return false;
        const auto& A = Val->AsArray();
        if (A.Num() != 3 && A.Num() != 4) return false;
        for (int32 i = 0; i < A.Num(); ++i) Buf[i] = A[i]->AsNumber();
        *static_cast<FColor*>(ValuePtr) = FColor(
            static_cast<uint8>(Buf[0]), static_cast<uint8>(Buf[1]),
            static_cast<uint8>(Buf[2]), static_cast<uint8>(Buf[3]));
        return true;
    }
    if (Struct == TBaseStructure<FIntPoint>::Get())
    {
        double Buf[2]{};
        if (!AsNumberArray(2, Buf)) return false;
        *static_cast<FIntPoint*>(ValuePtr) =
            FIntPoint(static_cast<int32>(Buf[0]), static_cast<int32>(Buf[1]));
        return true;
    }
    if (Struct == TBaseStructure<FIntVector>::Get())
    {
        double Buf[3]{};
        if (!AsNumberArray(3, Buf)) return false;
        *static_cast<FIntVector*>(ValuePtr) = FIntVector(
            static_cast<int32>(Buf[0]),
            static_cast<int32>(Buf[1]),
            static_cast<int32>(Buf[2]));
        return true;
    }
    if (Struct == TBaseStructure<FTransform>::Get())
    {
        if (Val->Type != EJson::Object) return false;
        const auto& Obj = Val->AsObject();
        if (!Obj.IsValid()) return false;
        FVector  Loc{};
        FRotator Rot{};
        FVector  Sca{1,1,1};
        TSharedPtr<FJsonValue> LocV = Obj->TryGetField(TEXT("location"));
        TSharedPtr<FJsonValue> RotV = Obj->TryGetField(TEXT("rotation"));
        TSharedPtr<FJsonValue> ScaV = Obj->TryGetField(TEXT("scale"));
        double Buf[3]{};
        if (LocV.IsValid())
        {
            if (!JsonToStructShorthand(TBaseStructure<FVector>::Get(),  &Loc, LocV)) return false;
        }
        if (RotV.IsValid())
        {
            if (!JsonToStructShorthand(TBaseStructure<FRotator>::Get(), &Rot, RotV)) return false;
        }
        if (ScaV.IsValid())
        {
            if (!JsonToStructShorthand(TBaseStructure<FVector>::Get(),  &Sca, ScaV)) return false;
        }
        (void)Buf;
        *static_cast<FTransform*>(ValuePtr) = FTransform(Rot, Loc, Sca);
        return true;
    }
    return false;
}

}  // namespace (anonymous)

bool SetPropertyValueAtPtr(FProperty* Property, void* ValuePtr,
                           const TSharedPtr<FJsonValue>& Value)
{
    if (Property == nullptr || ValuePtr == nullptr || !Value.IsValid()) return false;
    UE_LOG(LogSageBridge, Verbose, TEXT("[SageProp] set type=%s json_type=%d"),
           Property ? *Property->GetClass()->GetName() : TEXT("<null>"),
           Value.IsValid() ? (int32)Value->Type : -1);

    // ---- primitives ------------------------------------------------------
    if (FBoolProperty* P = CastField<FBoolProperty>(Property))
    {
        P->SetPropertyValue(ValuePtr, Value->AsBool());
        return true;
    }
    if (FIntProperty* P = CastField<FIntProperty>(Property))
    {
        P->SetPropertyValue(ValuePtr, static_cast<int32>(Value->AsNumber()));
        return true;
    }
    if (FInt64Property* P = CastField<FInt64Property>(Property))
    {
        P->SetPropertyValue(ValuePtr, static_cast<int64>(Value->AsNumber()));
        return true;
    }
    if (FInt8Property* P = CastField<FInt8Property>(Property))
    {
        P->SetPropertyValue(ValuePtr, static_cast<int8>(Value->AsNumber()));
        return true;
    }
    if (FInt16Property* P = CastField<FInt16Property>(Property))
    {
        P->SetPropertyValue(ValuePtr, static_cast<int16>(Value->AsNumber()));
        return true;
    }
    if (FUInt16Property* P = CastField<FUInt16Property>(Property))
    {
        P->SetPropertyValue(ValuePtr, static_cast<uint16>(Value->AsNumber()));
        return true;
    }
    if (FUInt32Property* P = CastField<FUInt32Property>(Property))
    {
        P->SetPropertyValue(ValuePtr, static_cast<uint32>(Value->AsNumber()));
        return true;
    }
    if (FUInt64Property* P = CastField<FUInt64Property>(Property))
    {
        P->SetPropertyValue(ValuePtr, static_cast<uint64>(Value->AsNumber()));
        return true;
    }
    if (FFloatProperty* P = CastField<FFloatProperty>(Property))
    {
        P->SetPropertyValue(ValuePtr, static_cast<float>(Value->AsNumber()));
        return true;
    }
    if (FDoubleProperty* P = CastField<FDoubleProperty>(Property))
    {
        P->SetPropertyValue(ValuePtr, Value->AsNumber());
        return true;
    }
    if (FStrProperty* P = CastField<FStrProperty>(Property))
    {
        P->SetPropertyValue(ValuePtr, Value->AsString());
        return true;
    }
    if (FNameProperty* P = CastField<FNameProperty>(Property))
    {
        P->SetPropertyValue(ValuePtr, FName(*Value->AsString()));
        return true;
    }
    if (FTextProperty* P = CastField<FTextProperty>(Property))
    {
        P->SetPropertyValue(ValuePtr, FText::FromString(Value->AsString()));
        return true;
    }

    // ---- byte / enum-as-byte --------------------------------------------
    if (FByteProperty* P = CastField<FByteProperty>(Property))
    {
        if (P->Enum && Value->Type == EJson::String)
        {
            const int64 Idx = P->Enum->GetIndexByNameString(Value->AsString());
            if (Idx == INDEX_NONE) return false;
            P->SetPropertyValue(ValuePtr, static_cast<uint8>(P->Enum->GetValueByIndex(Idx)));
            return true;
        }
        P->SetPropertyValue(ValuePtr, static_cast<uint8>(Value->AsNumber()));
        return true;
    }

    // ---- enums (full UEnum, modern) -------------------------------------
    if (FEnumProperty* P = CastField<FEnumProperty>(Property))
    {
        FNumericProperty* Underlying = P->GetUnderlyingProperty();
        if (Value->Type == EJson::String)
        {
            const int64 Idx = P->GetEnum()->GetIndexByNameString(Value->AsString());
            if (Idx == INDEX_NONE) return false;
            Underlying->SetIntPropertyValue(ValuePtr, P->GetEnum()->GetValueByIndex(Idx));
            return true;
        }
        Underlying->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value->AsNumber()));
        return true;
    }

    // ---- object refs (TObjectPtr<T>, raw UObject*) ----------------------
    if (FObjectProperty* P = CastField<FObjectProperty>(Property))
    {
        if (Value->Type == EJson::Null)
        {
            P->SetObjectPropertyValue(ValuePtr, nullptr);
            return true;
        }
        // Distinguish UClass-typed (TSubclassOf is FClassProperty derived from this)
        if (FClassProperty* CP = CastField<FClassProperty>(Property))
        {
            UClass* Cls = ResolveClassPath(Value->AsString(), CP->MetaClass);
            CP->SetObjectPropertyValue(ValuePtr, Cls);
            return Cls != nullptr;
        }
        UObject* Obj = ResolveAssetPath(Value->AsString());
        if (Obj && !Obj->IsA(P->PropertyClass)) return false;
        P->SetObjectPropertyValue(ValuePtr, Obj);
        return true;
    }

    if (FSoftObjectProperty* P = CastField<FSoftObjectProperty>(Property))
    {
        if (Value->Type == EJson::Null)
        {
            P->SetPropertyValue(ValuePtr, FSoftObjectPtr{});
            return true;
        }
        FSoftObjectPath Soft(Value->AsString());
        // FSoftClassProperty is FSoftObjectProperty with a class meta — same path.
        P->SetPropertyValue(ValuePtr, FSoftObjectPtr(Soft));
        return true;
    }

    // ---- struct ---------------------------------------------------------
    if (FStructProperty* P = CastField<FStructProperty>(Property))
    {
        if (JsonToStructShorthand(P->Struct, ValuePtr, Value)) return true;
        // Fallback: serialize JSON to a string and use ImportText. Lossy
        // for nested asset refs but works for arbitrary plain USTRUCTs.
        if (Value->Type == EJson::String)
        {
            const FString S = Value->AsString();
            return P->Struct->ImportText(*S, ValuePtr, /*OwnerObject*/ nullptr,
                                          PPF_None, GError, P->Struct->GetName()) != nullptr;
        }
        return false;
    }

    // ---- containers -----------------------------------------------------
    if (FArrayProperty* P = CastField<FArrayProperty>(Property))
    {
        if (Value->Type != EJson::Array) return false;
        UE_LOG(LogSageBridge, Verbose, TEXT("[SageProp] array inner=%s"),
               P->Inner ? *P->Inner->GetClass()->GetName() : TEXT("<null>"));
        FScriptArrayHelper Helper(P, ValuePtr);
        Helper.EmptyValues();
        const auto& Arr = Value->AsArray();
        for (int32 i = 0; i < Arr.Num(); ++i)
        {
            const int32 Idx = Helper.AddValue();
            if (!SetPropertyValueAtPtr(P->Inner, Helper.GetRawPtr(Idx), Arr[i]))
            {
                UE_LOG(LogSageBridge, Verbose, TEXT("[SageProp] array element %d set failed"), i);
                Helper.Resize(i);  // keep what we did get
                return false;
            }
        }
        return true;
    }

    if (FSetProperty* P = CastField<FSetProperty>(Property))
    {
        if (Value->Type != EJson::Array) return false;
        FScriptSetHelper Helper(P, ValuePtr);
        Helper.EmptyElements();
        for (const auto& Elem : Value->AsArray())
        {
            const int32 Idx = Helper.AddDefaultValue_Invalid_NeedsRehash();
            if (!SetPropertyValueAtPtr(P->ElementProp,
                                        Helper.GetElementPtr(Idx), Elem))
            {
                return false;
            }
        }
        Helper.Rehash();
        return true;
    }

    if (FMapProperty* P = CastField<FMapProperty>(Property))
    {
        if (Value->Type != EJson::Object) return false;
        const auto& Obj = Value->AsObject();
        if (!Obj.IsValid()) return false;
        FScriptMapHelper Helper(P, ValuePtr);
        Helper.EmptyValues();
        for (const auto& KV : Obj->Values)
        {
            const int32 Idx = Helper.AddDefaultValue_Invalid_NeedsRehash();
            // Key is always serialised as the JSON object key (string);
            // for non-string key types we coerce via the inner property's
            // SetPropertyValueAtPtr by wrapping in a JsonValueString.
            const auto KeyVal = MakeShared<FJsonValueString>(KV.Key);
            if (!SetPropertyValueAtPtr(P->KeyProp,
                                        Helper.GetKeyPtr(Idx), KeyVal)) return false;
            if (!SetPropertyValueAtPtr(P->ValueProp,
                                        Helper.GetValuePtr(Idx), KV.Value)) return false;
        }
        Helper.Rehash();
        return true;
    }

    return false;
}

TSharedPtr<FJsonValue> GetPropertyValueAtPtr(const FProperty* Property,
                                               const void* ValuePtr,
                                               FInstancedRecurseCtx* Ctx)
{
    if (Property == nullptr || ValuePtr == nullptr) return nullptr;

    if (const FBoolProperty* P = CastField<FBoolProperty>(Property))
        return MakeShared<FJsonValueBoolean>(P->GetPropertyValue(ValuePtr));
    if (const FIntProperty* P = CastField<FIntProperty>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(P->GetPropertyValue(ValuePtr)));
    if (const FInt64Property* P = CastField<FInt64Property>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(P->GetPropertyValue(ValuePtr)));
    if (const FInt8Property* P = CastField<FInt8Property>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(P->GetPropertyValue(ValuePtr)));
    if (const FInt16Property* P = CastField<FInt16Property>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(P->GetPropertyValue(ValuePtr)));
    if (const FUInt16Property* P = CastField<FUInt16Property>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(P->GetPropertyValue(ValuePtr)));
    if (const FUInt32Property* P = CastField<FUInt32Property>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(P->GetPropertyValue(ValuePtr)));
    if (const FUInt64Property* P = CastField<FUInt64Property>(Property))
        return MakeShared<FJsonValueNumber>(static_cast<double>(P->GetPropertyValue(ValuePtr)));
    if (const FFloatProperty* P = CastField<FFloatProperty>(Property))
        return MakeShared<FJsonValueNumber>(P->GetPropertyValue(ValuePtr));
    if (const FDoubleProperty* P = CastField<FDoubleProperty>(Property))
        return MakeShared<FJsonValueNumber>(P->GetPropertyValue(ValuePtr));
    if (const FStrProperty* P = CastField<FStrProperty>(Property))
        return MakeShared<FJsonValueString>(P->GetPropertyValue(ValuePtr));
    if (const FNameProperty* P = CastField<FNameProperty>(Property))
        return MakeShared<FJsonValueString>(P->GetPropertyValue(ValuePtr).ToString());
    if (const FTextProperty* P = CastField<FTextProperty>(Property))
        return MakeShared<FJsonValueString>(P->GetPropertyValue(ValuePtr).ToString());

    if (const FByteProperty* P = CastField<FByteProperty>(Property))
    {
        const uint8 V = P->GetPropertyValue(ValuePtr);
        if (P->Enum)
        {
            return MakeShared<FJsonValueString>(P->Enum->GetNameStringByValue(V));
        }
        return MakeShared<FJsonValueNumber>(static_cast<double>(V));
    }

    if (const FEnumProperty* P = CastField<FEnumProperty>(Property))
    {
        const int64 V = P->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
        return MakeShared<FJsonValueString>(P->GetEnum()->GetNameStringByValue(V));
    }

    if (const FObjectProperty* P = CastField<FObjectProperty>(Property))
    {
        UObject* Obj = P->GetObjectPropertyValue(ValuePtr);
        if (!Obj) return MakeShared<FJsonValueNull>();

        // Instanced subobject recursion (CommonAIExport-style): when the
        // property is marked CPF_InstancedReference (UPROPERTY(Instanced))
        // or its declared class is CLASS_DefaultToInstanced (e.g. UObject
        // subclasses authored as inline GameFeatureActions), expand the
        // sub-object's reflected properties inline instead of emitting a
        // bare path string. Bounded by Ctx->MaxDepth and a Visited set so
        // cycles short-circuit cleanly.
        const bool bClassInstanced = P->PropertyClass != nullptr
            && P->PropertyClass->HasAnyClassFlags(CLASS_DefaultToInstanced);
        const bool bInstanced = Property->HasAnyPropertyFlags(
                                    CPF_InstancedReference | CPF_PersistentInstance)
                              || bClassInstanced;
        if (Ctx != nullptr && bInstanced
            && Ctx->CurrentDepth < Ctx->MaxDepth
            && !Ctx->Visited.Contains(Obj))
        {
            Ctx->Visited.Add(Obj);
            ++Ctx->CurrentDepth;

            auto Sub  = MakeShared<FJsonObject>();
            Sub->SetStringField(TEXT("_class"), Obj->GetClass()->GetPathName());
            Sub->SetStringField(TEXT("_path"),  FSoftObjectPath(Obj).ToString());

            auto Props = MakeShared<FJsonObject>();
            int32 Count = 0;
            for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
            {
                FProperty* SubP = *It;
                if (!SubP) continue;
                if (SubP->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
                    continue;
                auto V = GetPropertyValueAtPtr(SubP,
                    SubP->ContainerPtrToValuePtr<void>(Obj), Ctx);
                if (V.IsValid())
                {
                    Props->SetField(SubP->GetName(), V);
                    ++Count;
                }
            }
            Sub->SetObjectField(TEXT("_props"), Props);
            Sub->SetNumberField(TEXT("_count"), Count);

            --Ctx->CurrentDepth;
            return MakeShared<FJsonValueObject>(Sub);
        }
        return MakeShared<FJsonValueString>(FSoftObjectPath(Obj).ToString());
    }
    if (const FSoftObjectProperty* P = CastField<FSoftObjectProperty>(Property))
    {
        const FSoftObjectPtr& Ptr = P->GetPropertyValue(ValuePtr);
        const FString S = Ptr.ToString();
        if (S.IsEmpty()) return MakeShared<FJsonValueNull>();
        return MakeShared<FJsonValueString>(S);
    }

    if (const FStructProperty* P = CastField<FStructProperty>(Property))
    {
        if (auto S = StructShorthandToJson(P->Struct, ValuePtr)) return S;
        // Fallback: ExportText round-trip.
        FString Out;
        P->Struct->ExportText(Out, ValuePtr, ValuePtr,
                               /*OwnerObject*/ nullptr, PPF_None, nullptr);
        return MakeShared<FJsonValueString>(Out);
    }

    if (const FArrayProperty* P = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Helper(P, ValuePtr);
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Helper.Num());
        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            auto V = GetPropertyValueAtPtr(P->Inner, Helper.GetRawPtr(i), Ctx);
            Arr.Add(V.IsValid() ? V : MakeShared<FJsonValueNull>());
        }
        return MakeShared<FJsonValueArray>(Arr);
    }

    if (const FSetProperty* P = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper Helper(P, ValuePtr);
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i)) continue;
            auto V = GetPropertyValueAtPtr(P->ElementProp, Helper.GetElementPtr(i), Ctx);
            Arr.Add(V.IsValid() ? V : MakeShared<FJsonValueNull>());
        }
        return MakeShared<FJsonValueArray>(Arr);
    }

    if (const FMapProperty* P = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper Helper(P, ValuePtr);
        auto Obj = MakeShared<FJsonObject>();
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i)) continue;
            auto KeyV = GetPropertyValueAtPtr(P->KeyProp, Helper.GetKeyPtr(i), Ctx);
            auto ValV = GetPropertyValueAtPtr(P->ValueProp, Helper.GetValuePtr(i), Ctx);
            const FString Key = KeyV.IsValid() ? KeyV->AsString() : FString::FromInt(i);
            Obj->SetField(Key, ValV.IsValid() ? ValV : MakeShared<FJsonValueNull>());
        }
        return MakeShared<FJsonValueObject>(Obj);
    }

    return nullptr;
}

bool SetUPropertyFromJson(UObject* Container,
                          FProperty* Property,
                          const TSharedPtr<FJsonValue>& Value)
{
    if (Property == nullptr || Container == nullptr) return false;
    return SetPropertyValueAtPtr(Property,
        Property->ContainerPtrToValuePtr<void>(Container), Value);
}

TSharedPtr<FJsonValue> GetUPropertyAsJson(const UObject* Container,
                                           const FProperty* Property,
                                           FInstancedRecurseCtx* Ctx)
{
    if (Property == nullptr || Container == nullptr) return nullptr;
    return GetPropertyValueAtPtr(Property,
        Property->ContainerPtrToValuePtr<void>(Container), Ctx);
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
