#include "Tools/SageNiagaraTools.h"

#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "IAssetTools.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraActor.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraParameterCollectionFactoryNew.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimCache.h"
#include "NiagaraSimCacheFunctionLibrary.h"
#include "NiagaraSimCacheJson.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "SageNiagara"

namespace sage::tools
{
namespace
{

using FOutcome = FSageToolDispatch::FOutcome;

template <typename TObject>
TObject* LoadTypedAsset(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    return Cast<TObject>(FSoftObjectPath(Path).TryLoad());
}

UObject* LoadAnyAsset(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    return FSoftObjectPath(Path).TryLoad();
}

UClass* LoadRendererClass(const FString& RendererClassOrKind)
{
    FString ClassPath = RendererClassOrKind;
    if (ClassPath.Equals(TEXT("sprite"), ESearchCase::IgnoreCase))
    {
        ClassPath = TEXT("/Script/Niagara.NiagaraSpriteRendererProperties");
    }
    else if (ClassPath.Equals(TEXT("mesh"), ESearchCase::IgnoreCase))
    {
        ClassPath = TEXT("/Script/Niagara.NiagaraMeshRendererProperties");
    }
    else if (ClassPath.Equals(TEXT("ribbon"), ESearchCase::IgnoreCase))
    {
        ClassPath = TEXT("/Script/Niagara.NiagaraRibbonRendererProperties");
    }
    else if (ClassPath.Equals(TEXT("light"), ESearchCase::IgnoreCase))
    {
        ClassPath = TEXT("/Script/Niagara.NiagaraLightRendererProperties");
    }
    else if (!ClassPath.StartsWith(TEXT("/Script/")) && !ClassPath.Contains(TEXT(".")))
    {
        ClassPath = FString::Printf(TEXT("/Script/Niagara.%s"), *ClassPath);
    }

    UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
    if (!Class) Class = LoadObject<UClass>(nullptr, *ClassPath);
    return Class && Class->IsChildOf(UNiagaraRendererProperties::StaticClass())
        ? Class
        : nullptr;
}

FString GuidToString(const FGuid& Guid)
{
    return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString();
}

bool ParseGuidString(const FString& Text, FGuid& Out)
{
    return FGuid::Parse(Text, Out);
}

FString UsageToString(ENiagaraScriptUsage Usage)
{
    switch (Usage)
    {
        case ENiagaraScriptUsage::Function: return TEXT("function");
        case ENiagaraScriptUsage::Module: return TEXT("module");
        case ENiagaraScriptUsage::DynamicInput: return TEXT("dynamic_input");
        case ENiagaraScriptUsage::ParticleSpawnScript: return TEXT("particle_spawn");
        case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated: return TEXT("particle_spawn_interpolated");
        case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("particle_update");
        case ENiagaraScriptUsage::ParticleEventScript: return TEXT("particle_event");
        case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("particle_simulation_stage");
        case ENiagaraScriptUsage::ParticleGPUComputeScript: return TEXT("particle_gpu_compute");
        case ENiagaraScriptUsage::EmitterSpawnScript: return TEXT("emitter_spawn");
        case ENiagaraScriptUsage::EmitterUpdateScript: return TEXT("emitter_update");
        case ENiagaraScriptUsage::SystemSpawnScript: return TEXT("system_spawn");
        case ENiagaraScriptUsage::SystemUpdateScript: return TEXT("system_update");
        default: return TEXT("unknown");
    }
}

bool ParseUsage(const FString& InUsage, ENiagaraScriptUsage& OutUsage)
{
    const FString S = InUsage.ToLower();
    if (S == TEXT("system_spawn")) { OutUsage = ENiagaraScriptUsage::SystemSpawnScript; return true; }
    if (S == TEXT("system_update")) { OutUsage = ENiagaraScriptUsage::SystemUpdateScript; return true; }
    if (S == TEXT("emitter_spawn")) { OutUsage = ENiagaraScriptUsage::EmitterSpawnScript; return true; }
    if (S == TEXT("emitter_update")) { OutUsage = ENiagaraScriptUsage::EmitterUpdateScript; return true; }
    if (S == TEXT("particle_spawn")) { OutUsage = ENiagaraScriptUsage::ParticleSpawnScript; return true; }
    if (S == TEXT("particle_update")) { OutUsage = ENiagaraScriptUsage::ParticleUpdateScript; return true; }
    if (S == TEXT("particle_event")) { OutUsage = ENiagaraScriptUsage::ParticleEventScript; return true; }
    if (S == TEXT("particle_simulation_stage")) { OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript; return true; }
    if (S == TEXT("module")) { OutUsage = ENiagaraScriptUsage::Module; return true; }
    if (S == TEXT("dynamic_input")) { OutUsage = ENiagaraScriptUsage::DynamicInput; return true; }
    return false;
}

TSharedPtr<FJsonValue> StringArrayToJson(const TArray<FString>& Items)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FString& Item : Items)
    {
        Arr.Add(MakeShared<FJsonValueString>(Item));
    }
    return MakeShared<FJsonValueArray>(Arr);
}

TArray<TSharedPtr<FJsonValue>> NameArrayToJson(const TArray<FName>& Names)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FName& Name : Names)
    {
        Arr.Add(MakeShared<FJsonValueString>(Name.ToString()));
    }
    return Arr;
}

FString OutcomeErrorString(const FOutcome& Outcome)
{
    if (!Outcome.Error.IsValid()) return TEXT("unknown error");
    FString Message;
    if (Outcome.Error->TryGetStringField(TEXT("message"), Message)) return Message;
    FString Serialized;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Outcome.Error.ToSharedRef(), Writer);
    return Serialized;
}

TSharedPtr<FJsonValue> Vec2ToJson(const FVector2D& V)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(V.X));
    Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
    return MakeShared<FJsonValueArray>(Arr);
}

TSharedPtr<FJsonValue> Vec4ToJson(double X, double Y, double Z, double W)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(X));
    Arr.Add(MakeShared<FJsonValueNumber>(Y));
    Arr.Add(MakeShared<FJsonValueNumber>(Z));
    Arr.Add(MakeShared<FJsonValueNumber>(W));
    return MakeShared<FJsonValueArray>(Arr);
}

TSharedPtr<FJsonObject> BoxToJsonObject(const FBox& Box)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetField(TEXT("min"), detail::Vec3ToJson(Box.Min));
    Obj->SetField(TEXT("max"), detail::Vec3ToJson(Box.Max));
    Obj->SetBoolField(TEXT("valid"), Box.IsValid != 0);
    return Obj;
}

bool JsonValueToVector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
    if (!Value.IsValid()) return false;
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Value->TryGetArray(Arr) && Arr && Arr->Num() >= 3)
    {
        Out.X = (*Arr)[0]->AsNumber();
        Out.Y = (*Arr)[1]->AsNumber();
        Out.Z = (*Arr)[2]->AsNumber();
        return true;
    }
    const TSharedPtr<FJsonObject>* Obj = nullptr;
    if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
    {
        double X = 0.0, Y = 0.0, Z = 0.0;
        if ((*Obj)->TryGetNumberField(TEXT("x"), X) &&
            (*Obj)->TryGetNumberField(TEXT("y"), Y) &&
            (*Obj)->TryGetNumberField(TEXT("z"), Z))
        {
            Out = FVector(X, Y, Z);
            return true;
        }
    }
    return false;
}

bool ArgsToBox(const TSharedPtr<FJsonObject>& Args, FBox& Out)
{
    if (!Args.IsValid()) return false;

    const TSharedPtr<FJsonValue> Bounds = Args->TryGetField(TEXT("bounds"));
    const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
    if (Bounds.IsValid() && Bounds->TryGetObject(BoundsObj) && BoundsObj && BoundsObj->IsValid())
    {
        FVector Min, Max;
        if (JsonValueToVector((*BoundsObj)->TryGetField(TEXT("min")), Min) &&
            JsonValueToVector((*BoundsObj)->TryGetField(TEXT("max")), Max))
        {
            Out = FBox(Min, Max);
            return true;
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* BoundsArr = nullptr;
    if (Bounds.IsValid() && Bounds->TryGetArray(BoundsArr) && BoundsArr && BoundsArr->Num() >= 6)
    {
        Out = FBox(
            FVector((*BoundsArr)[0]->AsNumber(), (*BoundsArr)[1]->AsNumber(), (*BoundsArr)[2]->AsNumber()),
            FVector((*BoundsArr)[3]->AsNumber(), (*BoundsArr)[4]->AsNumber(), (*BoundsArr)[5]->AsNumber()));
        return true;
    }

    FVector Min, Max;
    if (JsonValueToVector(Args->TryGetField(TEXT("min")), Min) &&
        JsonValueToVector(Args->TryGetField(TEXT("max")), Max))
    {
        Out = FBox(Min, Max);
        return true;
    }
    return false;
}

bool SaveLoadedAssetIfRequested(UObject* Asset, bool bSave, TSharedRef<FJsonObject> Result)
{
    Result->SetBoolField(TEXT("save_requested"), bSave);
    if (!bSave) return true;
    if (!Asset)
    {
        Result->SetStringField(TEXT("save_error"), TEXT("asset is null"));
        return false;
    }
    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    if (!AssetSubsystem)
    {
        Result->SetStringField(TEXT("save_error"), TEXT("EditorAssetSubsystem unavailable"));
        return false;
    }
    const bool bSaved = AssetSubsystem->SaveLoadedAsset(Asset, /*bOnlyIfIsDirty=*/false);
    Result->SetBoolField(TEXT("saved"), bSaved);
    if (!bSaved)
    {
        Result->SetStringField(TEXT("save_error"),
            FString::Printf(TEXT("SaveLoadedAsset returned false for %s"), *Asset->GetPathName()));
    }
    return bSaved;
}

void CompileIfRequested(UNiagaraSystem* System, bool bCompile, TSharedRef<FJsonObject> Result)
{
    Result->SetBoolField(TEXT("compile_requested"), bCompile);
    if (!bCompile || !System) return;
    const bool bRequested = System->RequestCompile(/*bForce=*/true);
    System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/false, /*bShowProgress=*/false);
    Result->SetBoolField(TEXT("compile_request_accepted"), bRequested);
    Result->SetBoolField(TEXT("compile_complete"), true);
}

TArray<FAssetData> ListNiagaraAssets(const FString& ClassPath,
                                     const FString& SearchPath,
                                     bool bRecursive = true)
{
    TArray<FAssetData> Assets;
    UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
    if (!Class) Class = LoadObject<UClass>(nullptr, *ClassPath);
    if (!Class) return Assets;

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = bRecursive;
    Filter.ClassPaths.Add(Class->GetClassPathName());
    ARM.Get().GetAssets(Filter, Assets);
    return Assets;
}

FString TypeDefToString(const FNiagaraTypeDefinition& Type)
{
    if (Type == FNiagaraTypeDefinition::GetFloatDef()) return TEXT("float");
    if (Type == FNiagaraTypeDefinition::GetBoolDef()) return TEXT("bool");
    if (Type == FNiagaraTypeDefinition::GetIntDef()) return TEXT("int");
    if (Type == FNiagaraTypeDefinition::GetVec2Def()) return TEXT("vec2");
    if (Type == FNiagaraTypeDefinition::GetVec3Def()) return TEXT("vec3");
    if (Type == FNiagaraTypeDefinition::GetPositionDef()) return TEXT("position");
    if (Type == FNiagaraTypeDefinition::GetVec4Def()) return TEXT("vec4");
    if (Type == FNiagaraTypeDefinition::GetColorDef()) return TEXT("color");
    if (Type == FNiagaraTypeDefinition::GetQuatDef()) return TEXT("quat");
    if (Type == FNiagaraTypeDefinition::GetMatrix4Def()) return TEXT("matrix4");
    UObject* Struct = Type.GetStruct();
    return Struct ? Struct->GetName() : Type.GetName();
}

bool TypeFromString(const FString& InType, FNiagaraTypeDefinition& Out)
{
    const FString S = InType.ToLower();
    if (S == TEXT("float") || S == TEXT("double") || S == TEXT("number")) { Out = FNiagaraTypeDefinition::GetFloatDef(); return true; }
    if (S == TEXT("bool") || S == TEXT("boolean")) { Out = FNiagaraTypeDefinition::GetBoolDef(); return true; }
    if (S == TEXT("int") || S == TEXT("integer")) { Out = FNiagaraTypeDefinition::GetIntDef(); return true; }
    if (S == TEXT("vec2") || S == TEXT("vector2") || S == TEXT("vector2d")) { Out = FNiagaraTypeDefinition::GetVec2Def(); return true; }
    if (S == TEXT("vec3") || S == TEXT("vector") || S == TEXT("vector3")) { Out = FNiagaraTypeDefinition::GetVec3Def(); return true; }
    if (S == TEXT("position")) { Out = FNiagaraTypeDefinition::GetPositionDef(); return true; }
    if (S == TEXT("vec4") || S == TEXT("vector4")) { Out = FNiagaraTypeDefinition::GetVec4Def(); return true; }
    if (S == TEXT("color") || S == TEXT("linearcolor")) { Out = FNiagaraTypeDefinition::GetColorDef(); return true; }
    if (S == TEXT("quat") || S == TEXT("quaternion")) { Out = FNiagaraTypeDefinition::GetQuatDef(); return true; }
    if (S == TEXT("matrix") || S == TEXT("matrix4")) { Out = FNiagaraTypeDefinition::GetMatrix4Def(); return true; }
    return false;
}

FString NormalizeUserParameterName(const FString& Name, bool bAddUserPrefix)
{
    if (!bAddUserPrefix || Name.Contains(TEXT("."))) return Name;
    return FString::Printf(TEXT("User.%s"), *Name);
}

TSharedPtr<FJsonValue> StoreValueToJson(const FNiagaraParameterStore& Store, const FNiagaraVariable& Var)
{
    const FNiagaraTypeDefinition& Type = Var.GetType();
    if (Type == FNiagaraTypeDefinition::GetFloatDef())
    {
        return MakeShared<FJsonValueNumber>(Store.GetParameterValue<float>(Var));
    }
    if (Type == FNiagaraTypeDefinition::GetBoolDef())
    {
        return MakeShared<FJsonValueBoolean>(Store.GetParameterValue<FNiagaraBool>(Var).GetValue());
    }
    if (Type == FNiagaraTypeDefinition::GetIntDef())
    {
        return MakeShared<FJsonValueNumber>(Store.GetParameterValue<int32>(Var));
    }
    if (Type == FNiagaraTypeDefinition::GetVec2Def())
    {
        const FVector2f V = Store.GetParameterValue<FVector2f>(Var);
        return Vec2ToJson(FVector2D(V.X, V.Y));
    }
    if (Type == FNiagaraTypeDefinition::GetVec3Def())
    {
        const FVector3f V = Store.GetParameterValue<FVector3f>(Var);
        return detail::Vec3ToJson(FVector(V.X, V.Y, V.Z));
    }
    if (Type == FNiagaraTypeDefinition::GetPositionDef())
    {
        const FNiagaraPosition V = Store.GetParameterValue<FNiagaraPosition>(Var);
        return detail::Vec3ToJson(FVector(V.X, V.Y, V.Z));
    }
    if (Type == FNiagaraTypeDefinition::GetVec4Def())
    {
        const FVector4f V = Store.GetParameterValue<FVector4f>(Var);
        return Vec4ToJson(V.X, V.Y, V.Z, V.W);
    }
    if (Type == FNiagaraTypeDefinition::GetColorDef())
    {
        const FLinearColor C = Store.GetParameterValue<FLinearColor>(Var);
        return Vec4ToJson(C.R, C.G, C.B, C.A);
    }
    return MakeShared<FJsonValueNull>();
}

TSharedPtr<FJsonObject> VariableToJson(const FNiagaraVariable& Var, const FNiagaraParameterStore* Store = nullptr)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Var.GetName().ToString());
    Obj->SetStringField(TEXT("type"), TypeDefToString(Var.GetType()));
    Obj->SetStringField(TEXT("type_name"), Var.GetType().GetName());
    Obj->SetBoolField(TEXT("has_value"), Store != nullptr && Store->IndexOf(Var) != INDEX_NONE);
    if (Store && Store->IndexOf(Var) != INDEX_NONE)
    {
        Obj->SetField(TEXT("value"), StoreValueToJson(*Store, Var));
    }
    return Obj;
}

TArray<TSharedPtr<FJsonValue>> ParameterStoreToJson(const FNiagaraParameterStore& Store)
{
    TArray<FNiagaraVariable> Params;
    Store.GetParameters(Params);

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FNiagaraVariable& Var : Params)
    {
        Out.Add(MakeShared<FJsonValueObject>(VariableToJson(Var, &Store)));
    }
    return Out;
}

bool SetStoreValue(FNiagaraParameterStore& Store,
                   const FNiagaraVariable& Var,
                   const TSharedPtr<FJsonValue>& Value,
                   FString& OutError)
{
    if (!Value.IsValid())
    {
        OutError = TEXT("missing value");
        return false;
    }

    const FNiagaraTypeDefinition& Type = Var.GetType();
    if (Type == FNiagaraTypeDefinition::GetFloatDef())
    {
        Store.SetParameterValue<float>(static_cast<float>(Value->AsNumber()), Var, true);
        return true;
    }
    if (Type == FNiagaraTypeDefinition::GetBoolDef())
    {
        Store.SetParameterValue<FNiagaraBool>(FNiagaraBool(Value->AsBool()), Var, true);
        return true;
    }
    if (Type == FNiagaraTypeDefinition::GetIntDef())
    {
        Store.SetParameterValue<int32>(static_cast<int32>(Value->AsNumber()), Var, true);
        return true;
    }
    if (Type == FNiagaraTypeDefinition::GetVec2Def())
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Value->TryGetArray(Arr) || !Arr || Arr->Num() < 2)
        {
            OutError = TEXT("vec2 value must be [x,y]");
            return false;
        }
        Store.SetParameterValue<FVector2f>(FVector2f((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber()), Var, true);
        return true;
    }
    if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
    {
        FVector V;
        if (!JsonValueToVector(Value, V))
        {
            OutError = TEXT("vec3 value must be [x,y,z] or {x,y,z}");
            return false;
        }
        if (Type == FNiagaraTypeDefinition::GetPositionDef())
        {
            Store.SetParameterValue<FNiagaraPosition>(FNiagaraPosition(V), Var, true);
        }
        else
        {
            Store.SetParameterValue<FVector3f>(FVector3f(V), Var, true);
        }
        return true;
    }
    if (Type == FNiagaraTypeDefinition::GetVec4Def() || Type == FNiagaraTypeDefinition::GetColorDef())
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Value->TryGetArray(Arr) || !Arr || Arr->Num() < 4)
        {
            OutError = TEXT("vec4/color value must be [x,y,z,w]");
            return false;
        }
        if (Type == FNiagaraTypeDefinition::GetColorDef())
        {
            Store.SetParameterValue<FLinearColor>(
                FLinearColor((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber(), (*Arr)[3]->AsNumber()),
                Var, true);
        }
        else
        {
            Store.SetParameterValue<FVector4f>(
                FVector4f((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber(), (*Arr)[3]->AsNumber()),
                Var, true);
        }
        return true;
    }

    OutError = FString::Printf(TEXT("unsupported Niagara parameter type for value write: %s"), *TypeDefToString(Type));
    return false;
}

TSharedPtr<FJsonObject> ReflectedObjectToJson(UObject* Obj, bool bEditableOnly = true)
{
    auto Out = MakeShared<FJsonObject>();
    if (!Obj) return Out;
    Out->SetStringField(TEXT("path"), Obj->GetPathName());
    Out->SetStringField(TEXT("class"), Obj->GetClass()->GetPathName());

    TArray<TSharedPtr<FJsonValue>> Props;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        FProperty* Prop = *It;
        if (bEditableOnly && !Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
        auto P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Prop->GetName());
        P->SetStringField(TEXT("type"), Prop->GetCPPType());
        P->SetBoolField(TEXT("editable"), Prop->HasAnyPropertyFlags(CPF_Edit));
        TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Obj, Prop);
        P->SetField(TEXT("value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
        Props.Add(MakeShared<FJsonValueObject>(P));
    }
    Out->SetArrayField(TEXT("properties"), Props);
    return Out;
}

struct FResolvedEmitterTarget
{
    UObject* RootAsset = nullptr;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* StandaloneEmitter = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    UNiagaraEmitter* MutableEmitter = nullptr;
    FVersionedNiagaraEmitterData* Data = nullptr;
    FGuid Version;
    FString Name;
    bool bSystemHandle = false;
};

bool EmitterMatches(const FNiagaraEmitterHandle& Handle,
                    int32 Index,
                    const FString& Selector,
                    int32 SelectorIndex,
                    const FGuid& SelectorGuid)
{
    if (SelectorIndex != INDEX_NONE && SelectorIndex == Index) return true;
    if (SelectorGuid.IsValid() && Handle.GetId() == SelectorGuid) return true;
    if (Selector.IsEmpty()) return Index == 0;
    return Handle.GetName().ToString().Equals(Selector, ESearchCase::IgnoreCase) ||
           Handle.GetIdName().ToString().Equals(Selector, ESearchCase::IgnoreCase) ||
           GuidToString(Handle.GetId()).Equals(Selector, ESearchCase::IgnoreCase);
}

FOutcome ResolveEmitterTarget(const TSharedPtr<FJsonObject>& Args,
                              FResolvedEmitterTarget& Out,
                              bool bRequireEmitterSelector = false)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UObject* Obj = LoadAnyAsset(Path);
    if (!Obj)
    {
        return FOutcome::MakeError(-32602, FString::Printf(TEXT("asset not found: %s"), *Path));
    }
    Out.RootAsset = Obj;

    FString Selector;
    Args->TryGetStringField(TEXT("emitter"), Selector);
    if (Selector.IsEmpty()) Args->TryGetStringField(TEXT("emitter_name"), Selector);
    FString GuidText;
    Args->TryGetStringField(TEXT("emitter_id"), GuidText);
    if (GuidText.IsEmpty()) Args->TryGetStringField(TEXT("handle_id"), GuidText);
    FGuid SelectorGuid;
    if (!GuidText.IsEmpty()) ParseGuidString(GuidText, SelectorGuid);
    int32 SelectorIndex = INDEX_NONE;
    Args->TryGetNumberField(TEXT("emitter_index"), SelectorIndex);
    Args->TryGetNumberField(TEXT("index"), SelectorIndex);

    if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Obj))
    {
        Out.StandaloneEmitter = Emitter;
        Out.MutableEmitter = Emitter;
        Out.Data = Emitter->GetLatestEmitterData();
        Out.Version = Emitter->GetExposedVersion().VersionGuid;
        Out.Name = Emitter->GetName();
        return Out.Data
            ? FOutcome::MakeSuccess(MakeShared<FJsonObject>())
            : FOutcome::MakeError(-32603, TEXT("Niagara emitter has no latest emitter data"));
    }

    UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj);
    if (!System)
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset is %s, expected UNiagaraSystem or UNiagaraEmitter"), *Obj->GetClass()->GetName()));
    }
    Out.System = System;

    TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
    if (Handles.Num() == 0)
    {
        return FOutcome::MakeError(-32602, TEXT("Niagara system has no emitters"));
    }
    if (bRequireEmitterSelector && Selector.IsEmpty() && SelectorIndex == INDEX_NONE && !SelectorGuid.IsValid())
    {
        return FOutcome::MakeError(-32602, TEXT("missing emitter selector for Niagara system path"));
    }

    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        FNiagaraEmitterHandle& Handle = Handles[Index];
        if (!EmitterMatches(Handle, Index, Selector, SelectorIndex, SelectorGuid)) continue;

        Out.Handle = &Handle;
        Out.bSystemHandle = true;
        const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
        Out.MutableEmitter = Instance.Emitter;
        Out.Version = Instance.Version;
        Out.Data = Handle.GetEmitterData();
        Out.Name = Handle.GetName().ToString();
        return Out.Data
            ? FOutcome::MakeSuccess(MakeShared<FJsonObject>())
            : FOutcome::MakeError(-32603, TEXT("selected emitter handle has no emitter data"));
    }

    return FOutcome::MakeError(-32602, FString::Printf(TEXT("emitter not found: %s"), *Selector));
}

TSharedPtr<FJsonObject> RendererToJson(UNiagaraRendererProperties* Renderer, int32 Index)
{
    auto Obj = ReflectedObjectToJson(Renderer, /*bEditableOnly=*/true);
    Obj->SetNumberField(TEXT("index"), Index);
    if (Renderer)
    {
        Obj->SetStringField(TEXT("display_name"), Renderer->GetWidgetDisplayName().ToString());
        Obj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
    }
    return Obj;
}

TSharedPtr<FJsonObject> EmitterTargetSummary(const FResolvedEmitterTarget& Target)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Target.Name);
    Obj->SetStringField(TEXT("root_asset"), Target.RootAsset ? Target.RootAsset->GetPathName() : FString());
    Obj->SetStringField(TEXT("version"), GuidToString(Target.Version));
    Obj->SetBoolField(TEXT("system_handle"), Target.bSystemHandle);
    if (Target.Handle)
    {
        Obj->SetStringField(TEXT("handle_id"), GuidToString(Target.Handle->GetId()));
        Obj->SetBoolField(TEXT("enabled"), Target.Handle->GetIsEnabled());
    }
    if (Target.MutableEmitter)
    {
        Obj->SetStringField(TEXT("emitter_asset"), Target.MutableEmitter->GetPathName());
        Obj->SetBoolField(TEXT("local_to_system"), Target.System && Target.MutableEmitter->GetOuter() == Target.System);
    }
    if (Target.Data)
    {
        Obj->SetNumberField(TEXT("renderer_count"), Target.Data->GetRenderers().Num());
        Obj->SetNumberField(TEXT("event_handler_count"), Target.Data->GetEventHandlers().Num());
        Obj->SetNumberField(TEXT("simulation_stage_count"), Target.Data->GetSimulationStages().Num());
        Obj->SetStringField(TEXT("sim_target"), StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(Target.Data->SimTarget)));
        Obj->SetStringField(TEXT("bounds_mode"), StaticEnum<ENiagaraEmitterCalculateBoundMode>()->GetNameStringByValue(static_cast<int64>(Target.Data->CalculateBoundsMode)));
        Obj->SetObjectField(TEXT("fixed_bounds"), BoxToJsonObject(Target.Data->FixedBounds));
    }
    return Obj;
}

UNiagaraScriptSource* GetEmitterScriptSource(FVersionedNiagaraEmitterData* Data)
{
    return Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
}

TArray<UNiagaraNodeFunctionCall*> CollectFunctionNodes(UNiagaraGraph* Graph)
{
    TArray<UNiagaraNodeFunctionCall*> Nodes;
    if (!Graph) return Nodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(Node))
        {
            Nodes.Add(FunctionNode);
        }
    }
    Nodes.Sort([](const UNiagaraNodeFunctionCall& A, const UNiagaraNodeFunctionCall& B)
    {
        if (A.NodePosY == B.NodePosY) return A.NodePosX < B.NodePosX;
        return A.NodePosY < B.NodePosY;
    });
    return Nodes;
}

TArray<UNiagaraNodeOutput*> CollectOutputNodes(UNiagaraGraph* Graph)
{
    TArray<UNiagaraNodeOutput*> Nodes;
    if (!Graph) return Nodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
        {
            Nodes.Add(OutputNode);
        }
    }
    return Nodes;
}

TSharedPtr<FJsonObject> PinToJson(UEdGraphPin* Pin)
{
    auto Obj = MakeShared<FJsonObject>();
    if (!Pin) return Obj;
    Obj->SetStringField(TEXT("pin_id"), GuidToString(Pin->PinId));
    Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
    Obj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    Obj->SetStringField(TEXT("type_category"), Pin->PinType.PinCategory.ToString());
    Obj->SetStringField(TEXT("type_subcategory"), Pin->PinType.PinSubCategory.ToString());
    Obj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    Obj->SetStringField(TEXT("default_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
    Obj->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
    return Obj;
}

TSharedPtr<FJsonObject> ModuleNodeToJson(UNiagaraNodeFunctionCall* Node, int32 Index)
{
    auto Obj = MakeShared<FJsonObject>();
    if (!Node) return Obj;
    Obj->SetNumberField(TEXT("index"), Index);
    Obj->SetStringField(TEXT("node_id"), GuidToString(Node->NodeGuid));
    Obj->SetStringField(TEXT("display_name"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Obj->SetStringField(TEXT("function_name"), Node->GetFunctionName());
    Obj->SetStringField(TEXT("function_script"), Node->FunctionScript ? Node->FunctionScript->GetPathName() : FString());
    Obj->SetStringField(TEXT("function_script_asset_path"), Node->FunctionScriptAssetObjectPath.ToString());
    Obj->SetStringField(TEXT("selected_script_version"), GuidToString(Node->SelectedScriptVersion));
    Obj->SetStringField(TEXT("called_usage"), UsageToString(Node->GetCalledUsage()));
    Obj->SetNumberField(TEXT("x"), Node->NodePosX);
    Obj->SetNumberField(TEXT("y"), Node->NodePosY);

    TArray<TSharedPtr<FJsonValue>> Inputs;
    for (const FNiagaraVariable& Var : Node->Signature.Inputs)
    {
        Inputs.Add(MakeShared<FJsonValueObject>(VariableToJson(Var)));
    }
    Obj->SetArrayField(TEXT("signature_inputs"), Inputs);

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
    }
    Obj->SetArrayField(TEXT("pins"), Pins);
    return Obj;
}

UNiagaraNodeFunctionCall* ResolveModuleNode(const TSharedPtr<FJsonObject>& Args,
                                            const TArray<UNiagaraNodeFunctionCall*>& Nodes,
                                            FString& OutError)
{
    FString Selector;
    Args->TryGetStringField(TEXT("module"), Selector);
    if (Selector.IsEmpty()) Args->TryGetStringField(TEXT("module_id"), Selector);
    if (Selector.IsEmpty()) Args->TryGetStringField(TEXT("display_name"), Selector);
    int32 Index = INDEX_NONE;
    Args->TryGetNumberField(TEXT("module_index"), Index);
    if (Index == INDEX_NONE) Args->TryGetNumberField(TEXT("index"), Index);

    FGuid SelectorGuid;
    const bool bGuidSelector = !Selector.IsEmpty() && ParseGuidString(Selector, SelectorGuid);

    if (Index != INDEX_NONE)
    {
        if (!Nodes.IsValidIndex(Index))
        {
            OutError = FString::Printf(TEXT("module index out of range: %d"), Index);
            return nullptr;
        }
        return Nodes[Index];
    }

    for (UNiagaraNodeFunctionCall* Node : Nodes)
    {
        if (!Node) continue;
        if (bGuidSelector && Node->NodeGuid == SelectorGuid) return Node;
        if (Node->GetFunctionName().Equals(Selector, ESearchCase::IgnoreCase) ||
            Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Equals(Selector, ESearchCase::IgnoreCase) ||
            (Node->FunctionScript && Node->FunctionScript->GetName().Equals(Selector, ESearchCase::IgnoreCase)))
        {
            return Node;
        }
    }

    OutError = Selector.IsEmpty() ? TEXT("missing module selector") : FString::Printf(TEXT("module not found: %s"), *Selector);
    return nullptr;
}

UEdGraphPin* FindInputPinByName(UNiagaraNodeFunctionCall* Node, const FName& PinName)
{
    if (!Node) return nullptr;
    const FString Wanted = PinName.ToString();
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input) continue;
        if (Pin->PinName == PinName || Pin->PinName.ToString().Equals(Wanted, ESearchCase::IgnoreCase))
        {
            return Pin;
        }
    }
    return nullptr;
}

FString JsonValueToPinDefault(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid()) return FString();
    switch (Value->Type)
    {
        case EJson::String: return Value->AsString();
        case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Number: return FString::SanitizeFloat(Value->AsNumber());
        default:
        {
            FString Out;
            TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
            FJsonSerializer::Serialize(Value, TEXT(""), Writer);
            return Out;
        }
    }
}

UNiagaraComponent* ResolveNiagaraComponentFromArgs(const TSharedPtr<FJsonObject>& Args, FString& OutError)
{
    FString ComponentPath;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("component"), ComponentPath);
        if (ComponentPath.IsEmpty()) Args->TryGetStringField(TEXT("component_id"), ComponentPath);
    }
    if (!ComponentPath.IsEmpty())
    {
        if (UNiagaraComponent* Component = Cast<UNiagaraComponent>(detail::ResolveComponent(ComponentPath)))
        {
            return Component;
        }
        if (UObject* Obj = FindObject<UObject>(nullptr, *ComponentPath))
        {
            if (UNiagaraComponent* Component = Cast<UNiagaraComponent>(Obj)) return Component;
        }
        OutError = FString::Printf(TEXT("Niagara component not found: %s"), *ComponentPath);
        return nullptr;
    }

    FString ActorPath;
    Args->TryGetStringField(TEXT("actor"), ActorPath);
    if (ActorPath.IsEmpty()) Args->TryGetStringField(TEXT("actor_id"), ActorPath);
    if (!ActorPath.IsEmpty())
    {
        AActor* Actor = detail::ResolveActor(ActorPath);
        if (!Actor)
        {
            OutError = FString::Printf(TEXT("actor not found: %s"), *ActorPath);
            return nullptr;
        }
        if (UNiagaraComponent* Component = Actor->FindComponentByClass<UNiagaraComponent>())
        {
            return Component;
        }
        OutError = FString::Printf(TEXT("actor has no Niagara component: %s"), *ActorPath);
        return nullptr;
    }

    OutError = TEXT("missing 'component' or 'actor'");
    return nullptr;
}

bool SetComponentParameter(UNiagaraComponent* Component,
                           const FString& Name,
                           const FString& TypeHint,
                           const TSharedPtr<FJsonValue>& Value,
                           FString& OutAppliedType,
                           FString& OutError)
{
    if (!Component)
    {
        OutError = TEXT("component is null");
        return false;
    }
    if (!Value.IsValid())
    {
        OutError = TEXT("missing value");
        return false;
    }

    const FName ParamName(*Name);
    const FString Type = TypeHint.ToLower();

    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Type == TEXT("bool") || Value->Type == EJson::Boolean)
    {
        Component->SetVariableBool(ParamName, Value->AsBool());
        OutAppliedType = TEXT("bool");
        return true;
    }
    if (Type == TEXT("int") || Type == TEXT("integer"))
    {
        Component->SetVariableInt(ParamName, static_cast<int32>(Value->AsNumber()));
        OutAppliedType = TEXT("int");
        return true;
    }
    if (Type == TEXT("float") || Type == TEXT("number") ||
        (Value->Type == EJson::Number && Type.IsEmpty()))
    {
        Component->SetVariableFloat(ParamName, static_cast<float>(Value->AsNumber()));
        OutAppliedType = TEXT("float");
        return true;
    }
    if (Value->TryGetArray(Arr) && Arr)
    {
        if (Arr->Num() >= 2 && (Arr->Num() == 2 || Type.Contains(TEXT("vec2"))))
        {
            Component->SetVariableVec2(ParamName, FVector2D((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber()));
            OutAppliedType = TEXT("vec2");
            return true;
        }
        if (Arr->Num() >= 3 && (Arr->Num() == 3 || Type.Contains(TEXT("vec3")) || Type == TEXT("vector") || Type == TEXT("position")))
        {
            const FVector V((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
            if (Type == TEXT("position"))
            {
                Component->SetVariablePosition(ParamName, V);
                OutAppliedType = TEXT("position");
            }
            else
            {
                Component->SetVariableVec3(ParamName, V);
                OutAppliedType = TEXT("vec3");
            }
            return true;
        }
        if (Arr->Num() >= 4)
        {
            if (Type == TEXT("color") || Type == TEXT("linearcolor"))
            {
                Component->SetVariableLinearColor(
                    ParamName,
                    FLinearColor((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber(), (*Arr)[3]->AsNumber()));
                OutAppliedType = TEXT("color");
            }
            else
            {
                Component->SetVariableVec4(
                    ParamName,
                    FVector4((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber(), (*Arr)[3]->AsNumber()));
                OutAppliedType = TEXT("vec4");
            }
            return true;
        }
    }
    if (Value->Type == EJson::String)
    {
        UObject* Asset = LoadAnyAsset(Value->AsString());
        if (!Asset)
        {
            OutError = FString::Printf(TEXT("asset value not found: %s"), *Value->AsString());
            return false;
        }
        if (Type == TEXT("material"))
        {
            Component->SetVariableMaterial(ParamName, Cast<UMaterialInterface>(Asset));
            OutAppliedType = TEXT("material");
            return true;
        }
        if (Type == TEXT("static_mesh") || Type == TEXT("staticmesh"))
        {
            Component->SetVariableStaticMesh(ParamName, Cast<UStaticMesh>(Asset));
            OutAppliedType = TEXT("static_mesh");
            return true;
        }
        if (Type == TEXT("texture"))
        {
            Component->SetVariableTexture(ParamName, Cast<UTexture>(Asset));
            OutAppliedType = TEXT("texture");
            return true;
        }
        Component->SetVariableObject(ParamName, Asset);
        OutAppliedType = TEXT("object");
        return true;
    }

    OutError = TEXT("unsupported component parameter value shape");
    return false;
}

TSharedPtr<FJsonObject> ComponentToJson(UNiagaraComponent* Component)
{
    auto Obj = MakeShared<FJsonObject>();
    if (!Component) return Obj;
    Obj->SetStringField(TEXT("component_id"), Component->GetPathName());
    Obj->SetStringField(TEXT("owner"), Component->GetOwner() ? Component->GetOwner()->GetPathName() : FString());
    Obj->SetStringField(TEXT("system"), Component->GetAsset() ? Component->GetAsset()->GetPathName() : FString());
    Obj->SetBoolField(TEXT("active"), Component->IsActive());
    Obj->SetObjectField(TEXT("bounds"), BoxToJsonObject(Component->Bounds.GetBox()));
    Obj->SetField(TEXT("location"), detail::Vec3ToJson(Component->GetComponentLocation()));
    Obj->SetArrayField(TEXT("override_parameters"), ParameterStoreToJson(Component->GetOverrideParameters()));
    if (Component->GetAsset())
    {
        TArray<TSharedPtr<FJsonValue>> Emitters;
        for (const FNiagaraEmitterHandle& Handle : Component->GetAsset()->GetEmitterHandles())
        {
            auto E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("name"), Handle.GetName().ToString());
            E->SetStringField(TEXT("id"), GuidToString(Handle.GetId()));
            E->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
            Emitters.Add(MakeShared<FJsonValueObject>(E));
        }
        Obj->SetArrayField(TEXT("emitters"), Emitters);
    }
    return Obj;
}

// ---- niagara.list ----------------------------------------------------------

FOutcome NiagaraListImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    int32 MaxResults = 0;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), SearchPath);
        Args->TryGetNumberField(TEXT("max_results"), MaxResults);
    }

    TArray<FAssetData> Systems = ListNiagaraAssets(TEXT("/Script/Niagara.NiagaraSystem"), SearchPath);
    TArray<FAssetData> Emitters = ListNiagaraAssets(TEXT("/Script/Niagara.NiagaraEmitter"), SearchPath);
    TArray<FAssetData> Collections = ListNiagaraAssets(TEXT("/Script/Niagara.NiagaraParameterCollection"), SearchPath);

    TArray<TSharedPtr<FJsonValue>> Assets;
    auto Add = [&](const TArray<FAssetData>& Arr, const FString& Type)
    {
        for (const FAssetData& D : Arr)
        {
            if (MaxResults > 0 && Assets.Num() >= MaxResults) return;
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"), D.AssetName.ToString());
            J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
            J->SetStringField(TEXT("type"), Type);
            Assets.Add(MakeShared<FJsonValueObject>(J));
        }
    };
    Add(Systems, TEXT("NiagaraSystem"));
    Add(Emitters, TEXT("NiagaraEmitter"));
    Add(Collections, TEXT("NiagaraParameterCollection"));

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("assets"), Assets);
    R->SetNumberField(TEXT("count"), Assets.Num());
    return FOutcome::MakeSuccess(R);
}

// ---- niagara.get_info ------------------------------------------------------

FOutcome NiagaraGetInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UObject* Obj = LoadAnyAsset(Path);
    if (!Obj)
    {
        return FOutcome::MakeError(-32602, FString::Printf(TEXT("asset not found: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetStringField(TEXT("class"), Obj->GetClass()->GetPathName());

    if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj))
    {
        R->SetObjectField(TEXT("fixed_bounds"), BoxToJsonObject(System->GetFixedBounds()));
        R->SetBoolField(TEXT("fixed_bounds_enabled"), System->bFixedBounds != 0);
        R->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
        R->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
        R->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
        R->SetStringField(TEXT("effect_type"), System->GetEffectType() ? System->GetEffectType()->GetPathName() : FString());
        R->SetArrayField(TEXT("parameters"), ParameterStoreToJson(System->GetExposedParameters()));

        TArray<TSharedPtr<FJsonValue>> Emitters;
        int32 Index = 0;
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            auto E = MakeShared<FJsonObject>();
            E->SetNumberField(TEXT("index"), Index++);
            E->SetStringField(TEXT("name"), Handle.GetName().ToString());
            E->SetStringField(TEXT("id"), GuidToString(Handle.GetId()));
            E->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
            if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
            {
                E->SetNumberField(TEXT("renderer_count"), Data->GetRenderers().Num());
                E->SetNumberField(TEXT("event_handler_count"), Data->GetEventHandlers().Num());
                E->SetNumberField(TEXT("simulation_stage_count"), Data->GetSimulationStages().Num());
            }
            Emitters.Add(MakeShared<FJsonValueObject>(E));
        }
        R->SetArrayField(TEXT("emitters"), Emitters);
    }
    else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Obj))
    {
        FResolvedEmitterTarget Target;
        Target.RootAsset = Obj;
        Target.StandaloneEmitter = Emitter;
        Target.MutableEmitter = Emitter;
        Target.Data = Emitter->GetLatestEmitterData();
        Target.Version = Emitter->GetExposedVersion().VersionGuid;
        Target.Name = Emitter->GetName();
        R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    }
    return FOutcome::MakeSuccess(R);
}

// ---- niagara.spawn / preview ----------------------------------------------

FOutcome NiagaraPreviewSpawnImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System)
    {
        return FOutcome::MakeError(-32602, FString::Printf(TEXT("not a Niagara system: %s"), *Path));
    }

    FVector Location = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Location);
    FTransform Transform(FRotator::ZeroRotator, Location);

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) return FOutcome::MakeError(-32603, TEXT("no editor world"));

    FScopedTransaction Tx(LOCTEXT("SpawnNiagara", "Sage: Spawn Niagara Preview"));
    ANiagaraActor* Actor = World->SpawnActor<ANiagaraActor>(ANiagaraActor::StaticClass(), Transform);
    if (!Actor) return FOutcome::MakeError(-32000, TEXT("failed to spawn ANiagaraActor"));
    Actor->SetActorLabel(FString::Printf(TEXT("SagePreview_%s"), *System->GetName()));
    Actor->Modify();

    UNiagaraComponent* Component = Actor->GetNiagaraComponent();
    if (!Component) return FOutcome::MakeError(-32000, TEXT("spawned Niagara actor has no component"));
    Component->Modify();
    Component->SetAsset(System, /*bResetExistingOverrideParameters=*/true);

    const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
    if (Args->TryGetArrayField(TEXT("parameters"), Params) && Params)
    {
        for (const TSharedPtr<FJsonValue>& ParamValue : *Params)
        {
            const TSharedPtr<FJsonObject>* ParamObj = nullptr;
            if (!ParamValue->TryGetObject(ParamObj) || !ParamObj || !ParamObj->IsValid()) continue;
            FString Name, Type;
            (*ParamObj)->TryGetStringField(TEXT("name"), Name);
            (*ParamObj)->TryGetStringField(TEXT("type"), Type);
            FString Applied, Error;
            SetComponentParameter(Component, Name, Type, (*ParamObj)->TryGetField(TEXT("value")), Applied, Error);
        }
    }

    bool bActivate = true;
    Args->TryGetBoolField(TEXT("activate"), bActivate);
    if (bActivate) Component->Activate(true);

    int32 AdvanceTicks = 0;
    double TickDelta = 1.0 / 60.0;
    Args->TryGetNumberField(TEXT("advance_ticks"), AdvanceTicks);
    Args->TryGetNumberField(TEXT("tick_delta"), TickDelta);
    if (AdvanceTicks > 0)
    {
        Component->AdvanceSimulation(AdvanceTicks, static_cast<float>(TickDelta));
    }

    auto R = ComponentToJson(Component);
    R->SetStringField(TEXT("actor_id"), Actor->GetPathName());
    R->SetStringField(TEXT("cleanup_token"), Actor->GetPathName());
    R->SetStringField(TEXT("world"), World->GetPathName());
    R->SetBoolField(TEXT("auto_destroy"), false);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSpawnImpl(const TSharedPtr<FJsonObject>& Args)
{
    return NiagaraPreviewSpawnImpl(Args);
}

FOutcome NiagaraCleanupPreviewImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Token;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("cleanup_token"), Token))
    {
        Args->TryGetStringField(TEXT("actor"), Token);
    }
    if (Token.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'cleanup_token'"));

    AActor* Actor = detail::ResolveActor(Token);
    if (!Actor) return FOutcome::MakeError(-32602, FString::Printf(TEXT("preview actor not found: %s"), *Token));

    FScopedTransaction Tx(LOCTEXT("CleanupNiagaraPreview", "Sage: Cleanup Niagara Preview"));
    UWorld* ActorWorld = Actor->GetWorld();
    const bool bDestroyed = ActorWorld && ActorWorld->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("cleanup_token"), Token);
    R->SetBoolField(TEXT("destroyed"), bDestroyed);
    return FOutcome::MakeSuccess(R);
}

// ---- runtime component parameters -----------------------------------------

FOutcome NiagaraSetParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Error;
    UNiagaraComponent* Component = ResolveNiagaraComponentFromArgs(Args, Error);
    if (!Component) return FOutcome::MakeError(-32602, Error);

    FString Name, Type;
    if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    Args->TryGetStringField(TEXT("type"), Type);

    FString AppliedType;
    if (!SetComponentParameter(Component, Name, Type, Args->TryGetField(TEXT("value")), AppliedType, Error))
    {
        return FOutcome::MakeError(-32602, Error);
    }

    auto R = ComponentToJson(Component);
    R->SetStringField(TEXT("parameter"), Name);
    R->SetStringField(TEXT("applied_type"), AppliedType);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetComponentParametersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Error;
    UNiagaraComponent* Component = ResolveNiagaraComponentFromArgs(Args, Error);
    if (!Component) return FOutcome::MakeError(-32602, Error);

    const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
    if (!Args->TryGetArrayField(TEXT("parameters"), Params) || !Params)
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'parameters' array"));
    }

    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Failed;
    for (const TSharedPtr<FJsonValue>& ParamValue : *Params)
    {
        const TSharedPtr<FJsonObject>* ParamObj = nullptr;
        if (!ParamValue->TryGetObject(ParamObj) || !ParamObj || !ParamObj->IsValid()) continue;
        FString Name, Type, AppliedType, LocalError;
        (*ParamObj)->TryGetStringField(TEXT("name"), Name);
        (*ParamObj)->TryGetStringField(TEXT("type"), Type);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Name);
        if (SetComponentParameter(Component, Name, Type, (*ParamObj)->TryGetField(TEXT("value")), AppliedType, LocalError))
        {
            Row->SetStringField(TEXT("applied_type"), AppliedType);
            Applied.Add(MakeShared<FJsonValueObject>(Row));
        }
        else
        {
            Row->SetStringField(TEXT("error"), LocalError);
            Failed.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    auto R = ComponentToJson(Component);
    R->SetArrayField(TEXT("applied"), Applied);
    R->SetArrayField(TEXT("failed"), Failed);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraReadComponentImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Error;
    UNiagaraComponent* Component = ResolveNiagaraComponentFromArgs(Args, Error);
    if (!Component) return FOutcome::MakeError(-32602, Error);
    return FOutcome::MakeSuccess(ComponentToJson(Component));
}

// ---- create assets ---------------------------------------------------------

FOutcome NiagaraCreateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return FOutcome::MakeError(-32602, TEXT("invalid path"));
    }

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UNiagaraSystemFactoryNew* Factory = NewObject<UNiagaraSystemFactoryNew>();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, UNiagaraSystem::StaticClass(), Factory);
    if (!NewObj) return FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("NiagaraSystem"));
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraCreateEmitterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return FOutcome::MakeError(-32602, TEXT("invalid path"));
    }

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UNiagaraEmitterFactoryNew* Factory = NewObject<UNiagaraEmitterFactoryNew>();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, UNiagaraEmitter::StaticClass(), Factory);
    if (!NewObj) return FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("NiagaraEmitter"));
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraCreateSystemFromSpecImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    if (!Args.IsValid() || !Args->HasField(TEXT("spec")))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'spec'"));
    }
    const TSharedPtr<FJsonObject>* Spec = nullptr;
    if (!Args->TryGetObjectField(TEXT("spec"), Spec) || !Spec || !Spec->IsValid())
    {
        return FOutcome::MakeError(-32602, TEXT("'spec' must be an object"));
    }

    FOutcome Created = NiagaraCreateImpl(Args);
    if (!Created.bSuccess) return Created;

    FString Path;
    Args->TryGetStringField(TEXT("path"), Path);
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32603, TEXT("created system could not be reloaded"));

    bool bSave = false, bCompile = false;
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    FScopedTransaction Tx(LOCTEXT("CreateNiagaraFromSpec", "Sage: Create Niagara System From Spec"));
    System->Modify();

    const TArray<TSharedPtr<FJsonValue>>* UserParams = nullptr;
    if ((*Spec)->TryGetArrayField(TEXT("user_parameters"), UserParams) && UserParams)
    {
        for (const TSharedPtr<FJsonValue>& ParamValue : *UserParams)
        {
            const TSharedPtr<FJsonObject>* ParamObj = nullptr;
            if (!ParamValue->TryGetObject(ParamObj) || !ParamObj || !ParamObj->IsValid()) continue;
            FString Name, TypeText;
            (*ParamObj)->TryGetStringField(TEXT("name"), Name);
            (*ParamObj)->TryGetStringField(TEXT("type"), TypeText);
            FNiagaraTypeDefinition Type;
            if (!TypeFromString(TypeText, Type)) continue;
            FNiagaraVariable Var(Type, FName(*NormalizeUserParameterName(Name, true)));
            System->GetExposedParameters().AddParameter(Var, true);
            FString IgnoredError;
            SetStoreValue(System->GetExposedParameters(), Var, (*ParamObj)->TryGetField(TEXT("value")), IgnoredError);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
    if ((*Spec)->TryGetArrayField(TEXT("emitters"), Emitters) && Emitters)
    {
        for (const TSharedPtr<FJsonValue>& EmitterValue : *Emitters)
        {
            const TSharedPtr<FJsonObject>* EmitterObj = nullptr;
            if (!EmitterValue->TryGetObject(EmitterObj) || !EmitterObj || !EmitterObj->IsValid()) continue;
            FString EmitterPath, Name;
            (*EmitterObj)->TryGetStringField(TEXT("path"), EmitterPath);
            (*EmitterObj)->TryGetStringField(TEXT("name"), Name);
            if (UNiagaraEmitter* SourceEmitter = LoadTypedAsset<UNiagaraEmitter>(EmitterPath))
            {
                const FGuid Version = SourceEmitter->GetExposedVersion().VersionGuid;
                System->AddEmitterHandle(*SourceEmitter, FName(*Name), Version);
            }
        }
    }

    FBox Bounds;
    if (ArgsToBox(*Spec, Bounds))
    {
        System->bFixedBounds = true;
        System->SetFixedBounds(Bounds);
    }

    System->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), System->GetPathName());
    R->SetArrayField(TEXT("parameters"), ParameterStoreToJson(System->GetExposedParameters()));
    CompileIfRequested(System, bCompile, R);
    SaveLoadedAssetIfRequested(System, bSave, R);
    return FOutcome::MakeSuccess(R);
}

// ---- emitters --------------------------------------------------------------

FOutcome NiagaraAddEmitterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, EmitterPath, Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("emitter"), EmitterPath))
    {
        Args->TryGetStringField(TEXT("emitter_path"), EmitterPath);
    }
    if (EmitterPath.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'emitter'"));

    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    UNiagaraEmitter* Emitter = LoadTypedAsset<UNiagaraEmitter>(EmitterPath);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));
    if (!Emitter) return FOutcome::MakeError(-32602, TEXT("'emitter' is not a Niagara emitter asset"));

    Args->TryGetStringField(TEXT("name"), Name);
    if (Name.IsEmpty()) Name = Emitter->GetName();
    bool bDryRun = false, bSave = false, bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), System->GetPathName());
    R->SetStringField(TEXT("emitter"), Emitter->GetPathName());
    R->SetStringField(TEXT("name"), Name);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (bDryRun) return FOutcome::MakeSuccess(R);

    FScopedTransaction Tx(LOCTEXT("AddNiagaraEmitter", "Sage: Add Niagara Emitter"));
    System->Modify();
    FNiagaraEmitterHandle NewHandle = System->AddEmitterHandle(*Emitter, FName(*Name), Emitter->GetExposedVersion().VersionGuid);
    System->MarkPackageDirty();
    R->SetStringField(TEXT("handle_id"), GuidToString(NewHandle.GetId()));
    CompileIfRequested(System, bCompile, R);
    SaveLoadedAssetIfRequested(System, bSave, R);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraListEmittersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Obj = LoadAnyAsset(Path);
    if (!Obj) return FOutcome::MakeError(-32602, FString::Printf(TEXT("asset not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Emitters;
    if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj))
    {
        int32 Index = 0;
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            FResolvedEmitterTarget Target;
            Target.RootAsset = System;
            Target.System = System;
            Target.Handle = const_cast<FNiagaraEmitterHandle*>(&Handle);
            Target.bSystemHandle = true;
            Target.MutableEmitter = Handle.GetInstance().Emitter;
            Target.Version = Handle.GetInstance().Version;
            Target.Data = Handle.GetEmitterData();
            Target.Name = Handle.GetName().ToString();
            auto E = EmitterTargetSummary(Target);
            E->SetNumberField(TEXT("index"), Index++);
            Emitters.Add(MakeShared<FJsonValueObject>(E));
        }
    }
    else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Obj))
    {
        FResolvedEmitterTarget Target;
        Target.RootAsset = Emitter;
        Target.StandaloneEmitter = Emitter;
        Target.MutableEmitter = Emitter;
        Target.Data = Emitter->GetLatestEmitterData();
        Target.Version = Emitter->GetExposedVersion().VersionGuid;
        Target.Name = Emitter->GetName();
        auto E = EmitterTargetSummary(Target);
        E->SetNumberField(TEXT("index"), 0);
        Emitters.Add(MakeShared<FJsonValueObject>(E));
    }
    else
    {
        return FOutcome::MakeError(-32602, TEXT("path is not a Niagara system or emitter"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetArrayField(TEXT("emitters"), Emitters);
    R->SetNumberField(TEXT("count"), Emitters.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetEmitterPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;

    FString PropertyName;
    if (!Args->TryGetStringField(TEXT("property"), PropertyName))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'property'"));
    }

    bool bDryRun = false, bSave = false, bCompile = true, bConfirmed = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);

    if (Target.StandaloneEmitter && !bDryRun && !bConfirmed)
    {
        return FOutcome::MakeError(-32602,
            TEXT("standalone NiagaraEmitter mutation can affect all referencing systems; pass confirmed:true"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("before"), EmitterTargetSummary(Target));
    R->SetStringField(TEXT("property"), PropertyName);
    R->SetBoolField(TEXT("dry_run"), bDryRun);

    if (PropertyName.Equals(TEXT("enabled"), ESearchCase::IgnoreCase))
    {
        if (!Target.Handle) return FOutcome::MakeError(-32602, TEXT("'enabled' is only valid for system emitter handles"));
        bool bEnabled = false;
        if (!Args->TryGetBoolField(TEXT("value"), bEnabled)) return FOutcome::MakeError(-32602, TEXT("'enabled' value must be boolean"));
        if (!bDryRun)
        {
            FScopedTransaction Tx(LOCTEXT("SetNiagaraEmitterEnabled", "Sage: Set Niagara Emitter Enabled"));
            Target.System->Modify();
            Target.Handle->SetIsEnabled(bEnabled, *Target.System, bCompile);
            Target.System->MarkPackageDirty();
        }
    }
    else if (PropertyName.Equals(TEXT("name"), ESearchCase::IgnoreCase))
    {
        if (!Target.Handle) return FOutcome::MakeError(-32602, TEXT("'name' is only valid for system emitter handles"));
        FString NewName;
        if (!Args->TryGetStringField(TEXT("value"), NewName) || NewName.IsEmpty())
        {
            return FOutcome::MakeError(-32602, TEXT("'name' value must be string"));
        }
        if (!bDryRun)
        {
            FScopedTransaction Tx(LOCTEXT("RenameNiagaraEmitter", "Sage: Rename Niagara Emitter"));
            Target.System->Modify();
            Target.Handle->SetName(FName(*NewName), *Target.System);
            Target.System->MarkPackageDirty();
        }
    }
    else
    {
        if (!Target.MutableEmitter) return FOutcome::MakeError(-32603, TEXT("selected emitter has no mutable emitter object"));
        FProperty* Prop = FindFProperty<FProperty>(Target.MutableEmitter->GetClass(), *PropertyName);
        if (!Prop)
        {
            return FOutcome::MakeError(-32602, FString::Printf(TEXT("emitter property not found: %s"), *PropertyName));
        }
        if (!bDryRun)
        {
            FScopedTransaction Tx(LOCTEXT("SetNiagaraEmitterProperty", "Sage: Set Niagara Emitter Property"));
            Target.MutableEmitter->Modify();
            if (!detail::SetUPropertyFromJson(Target.MutableEmitter, Prop, Args->TryGetField(TEXT("value"))))
            {
                return FOutcome::MakeError(-32602, TEXT("property value type mismatch"));
            }
            Target.MutableEmitter->MarkPackageDirty();
        }
    }

    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else if (Target.MutableEmitter)
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }

    FResolvedEmitterTarget After;
    ResolveEmitterTarget(Args, After);
    R->SetObjectField(TEXT("after"), EmitterTargetSummary(After.Data ? After : Target));
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraMakeEmittersLocalImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    bool bDryRun = false, bSave = false, bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    TArray<TSharedPtr<FJsonValue>> Rows;
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("MakeNiagaraEmittersLocal", "Sage: Make Niagara Emitters Local"));
        System->Modify();
        for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            FVersionedNiagaraEmitter Instance = Handle.GetInstance();
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Handle.GetName().ToString());
            Row->SetStringField(TEXT("handle_id"), GuidToString(Handle.GetId()));
            Row->SetStringField(TEXT("before_emitter"), Instance.Emitter ? Instance.Emitter->GetPathName() : FString());
            if (Instance.Emitter && Instance.Emitter->GetOuter() != System)
            {
                UNiagaraEmitter* LocalEmitter = DuplicateObject<UNiagaraEmitter>(
                    Instance.Emitter, System, *FString::Printf(TEXT("%s_Local"), *Handle.GetName().ToString()));
                if (LocalEmitter)
                {
                    Handle.SetInstance(FVersionedNiagaraEmitter(LocalEmitter, Instance.Version));
                    Row->SetBoolField(TEXT("localized"), true);
                    Row->SetStringField(TEXT("after_emitter"), LocalEmitter->GetPathName());
                }
            }
            else
            {
                Row->SetBoolField(TEXT("localized"), false);
                Row->SetStringField(TEXT("reason"), TEXT("already local or missing emitter"));
            }
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        System->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), System->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetArrayField(TEXT("emitters"), Rows);
    CompileIfRequested(System, bCompile && !bDryRun, R);
    SaveLoadedAssetIfRequested(System, bSave && !bDryRun, R);
    return FOutcome::MakeSuccess(R);
}

// ---- modules / graph -------------------------------------------------------

FOutcome NiagaraListModulesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;

    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph)
    {
        return FOutcome::MakeError(-32603, TEXT("selected emitter has no Niagara graph source"));
    }

    TArray<TSharedPtr<FJsonValue>> Modules;
    int32 Index = 0;
    for (UNiagaraNodeFunctionCall* Node : CollectFunctionNodes(Source->NodeGraph))
    {
        Modules.Add(MakeShared<FJsonValueObject>(ModuleNodeToJson(Node, Index++)));
    }

    TArray<TSharedPtr<FJsonValue>> Outputs;
    for (UNiagaraNodeOutput* Output : CollectOutputNodes(Source->NodeGraph))
    {
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("node_id"), GuidToString(Output->NodeGuid));
        O->SetStringField(TEXT("usage"), UsageToString(Output->GetUsage()));
        O->SetStringField(TEXT("usage_id"), GuidToString(Output->GetUsageId()));
        Outputs.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    R->SetArrayField(TEXT("modules"), Modules);
    R->SetArrayField(TEXT("outputs"), Outputs);
    R->SetNumberField(TEXT("count"), Modules.Num());
    R->SetStringField(TEXT("ordering"), TEXT("graph_node_position"));
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraGetEmitterInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    if (Target.MutableEmitter)
    {
        R->SetObjectField(TEXT("emitter_object"), ReflectedObjectToJson(Target.MutableEmitter, /*bEditableOnly=*/false));
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraListModuleInputsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<UNiagaraNodeFunctionCall*> Nodes = CollectFunctionNodes(Source->NodeGraph);
    FString Error;
    UNiagaraNodeFunctionCall* Node = ResolveModuleNode(Args, Nodes, Error);
    if (!Node) return FOutcome::MakeError(-32602, Error);

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("module"), ModuleNodeToJson(Node, Nodes.IndexOfByKey(Node)));
    TArray<TSharedPtr<FJsonValue>> Inputs;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input)
        {
            Inputs.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
        }
    }
    R->SetArrayField(TEXT("inputs"), Inputs);
    R->SetNumberField(TEXT("count"), Inputs.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetModuleInputImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<UNiagaraNodeFunctionCall*> Nodes = CollectFunctionNodes(Source->NodeGraph);
    FString Error;
    UNiagaraNodeFunctionCall* Node = ResolveModuleNode(Args, Nodes, Error);
    if (!Node) return FOutcome::MakeError(-32602, Error);

    FString Variable;
    if (!Args->TryGetStringField(TEXT("variable"), Variable))
    {
        Args->TryGetStringField(TEXT("input"), Variable);
    }
    if (Variable.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'variable'"));

    UEdGraphPin* TargetPin = nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input && Pin->PinName.ToString().Equals(Variable, ESearchCase::IgnoreCase))
        {
            TargetPin = Pin;
            break;
        }
    }
    if (!TargetPin)
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("input pin not found on module node: %s"), *Variable));
    }

    bool bDryRun = false, bSave = false, bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("before"), PinToJson(TargetPin));
    R->SetBoolField(TEXT("dry_run"), bDryRun);

    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("SetNiagaraModuleInput", "Sage: Set Niagara Module Input"));
        Source->NodeGraph->Modify();
        Node->Modify();
        TargetPin->Modify();
        TargetPin->DefaultValue = JsonValueToPinDefault(Args->TryGetField(TEXT("value")));
        Source->NodeGraph->NotifyGraphChanged();
        Target.RootAsset->MarkPackageDirty();
    }

    R->SetObjectField(TEXT("after"), PinToJson(TargetPin));
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraAddModuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    FString ModulePath, UsageText;
    if (!Args->TryGetStringField(TEXT("module_script"), ModulePath))
    {
        Args->TryGetStringField(TEXT("module"), ModulePath);
    }
    if (ModulePath.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'module_script'"));
    Args->TryGetStringField(TEXT("usage"), UsageText);
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
    if (!UsageText.IsEmpty() && !ParseUsage(UsageText, Usage))
    {
        return FOutcome::MakeError(-32602, TEXT("invalid usage"));
    }

    bool bDryRun = false, bSave = false, bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    bool bFoundModule = false;
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("module_script"), ModulePath);
    R->SetStringField(TEXT("usage"), UsageToString(Usage));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("AddNiagaraModule", "Sage: Add Niagara Module"));
        Source->NodeGraph->Modify();
        const bool bAdded = Source->AddModuleIfMissing(ModulePath, Usage, bFoundModule);
        R->SetBoolField(TEXT("added"), bAdded);
        R->SetBoolField(TEXT("found_existing"), bFoundModule);
        if (bAdded)
        {
            Source->NodeGraph->NotifyGraphChanged();
            Target.RootAsset->MarkPackageDirty();
        }
    }
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraRemoveModuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<UNiagaraNodeFunctionCall*> Nodes = CollectFunctionNodes(Source->NodeGraph);
    FString Error;
    UNiagaraNodeFunctionCall* Node = ResolveModuleNode(Args, Nodes, Error);
    if (!Node) return FOutcome::MakeError(-32602, Error);

    bool bDryRun = false, bSave = false, bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("removed_module"), ModuleNodeToJson(Node, Nodes.IndexOfByKey(Node)));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("RemoveNiagaraModule", "Sage: Remove Niagara Module"));
        Source->NodeGraph->Modify();
        Node->Modify();
        Node->DestroyNode();
        Source->NodeGraph->NotifyGraphChanged();
        Target.RootAsset->MarkPackageDirty();
        R->SetBoolField(TEXT("removed"), true);
    }
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraReplaceModuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<UNiagaraNodeFunctionCall*> Nodes = CollectFunctionNodes(Source->NodeGraph);
    FString Error;
    UNiagaraNodeFunctionCall* Node = ResolveModuleNode(Args, Nodes, Error);
    if (!Node) return FOutcome::MakeError(-32602, Error);

    FString NewScriptPath;
    if (!Args->TryGetStringField(TEXT("new_module_script"), NewScriptPath))
    {
        Args->TryGetStringField(TEXT("module_script"), NewScriptPath);
    }
    UNiagaraScript* NewScript = LoadTypedAsset<UNiagaraScript>(NewScriptPath);
    if (!NewScript) return FOutcome::MakeError(-32602, TEXT("'new_module_script' is not a Niagara script"));

    bool bDryRun = false, bSave = false, bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("before"), ModuleNodeToJson(Node, Nodes.IndexOfByKey(Node)));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("ReplaceNiagaraModule", "Sage: Replace Niagara Module"));
        Source->NodeGraph->Modify();
        Node->Modify();
        Node->FunctionScript = NewScript;
        Node->FunctionScriptAssetObjectPath = FName(*NewScript->GetPathName());
        Node->SelectedScriptVersion = NewScript->GetExposedVersion().VersionGuid;
        Source->NodeGraph->NotifyGraphChanged();
        Target.RootAsset->MarkPackageDirty();
    }
    R->SetObjectField(TEXT("after"), ModuleNodeToJson(Node, Nodes.IndexOfByKey(Node)));
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraMoveModuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.move_module is unsupported in UE 5.7 SageBridge because the public exported API does not expose safe stack reordering; use remove+add or replace_module"));
}

FOutcome NiagaraDuplicateModuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.duplicate_module is unsupported until Sage can call exported Niagara stack duplication APIs"));
}

// ---- renderers -------------------------------------------------------------

FOutcome NiagaraListRenderersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;

    TArray<TSharedPtr<FJsonValue>> Renderers;
    if (Target.Data)
    {
        const TArray<UNiagaraRendererProperties*>& RendererProps = Target.Data->GetRenderers();
        for (int32 Index = 0; Index < RendererProps.Num(); ++Index)
        {
            Renderers.Add(MakeShared<FJsonValueObject>(RendererToJson(RendererProps[Index], Index)));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    R->SetArrayField(TEXT("renderers"), Renderers);
    R->SetNumberField(TEXT("count"), Renderers.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraAddRendererImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    if (!Target.MutableEmitter) return FOutcome::MakeError(-32603, TEXT("selected emitter has no mutable emitter object"));

    FString RendererClassText;
    if (!Args->TryGetStringField(TEXT("renderer_class"), RendererClassText))
    {
        Args->TryGetStringField(TEXT("renderer_type"), RendererClassText);
    }
    if (RendererClassText.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'renderer_class'"));

    UClass* RendererClass = LoadRendererClass(RendererClassText);
    if (!RendererClass) return FOutcome::MakeError(-32602, FString::Printf(TEXT("invalid renderer class: %s"), *RendererClassText));

    bool bDryRun = false, bSave = false, bCompile = true, bConfirmed = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (Target.StandaloneEmitter && !bDryRun && !bConfirmed)
    {
        return FOutcome::MakeError(-32602,
            TEXT("standalone NiagaraEmitter renderer mutation can affect all referencing systems; pass confirmed:true"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    R->SetStringField(TEXT("renderer_class"), RendererClass->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("AddNiagaraRenderer", "Sage: Add Niagara Renderer"));
        Target.MutableEmitter->Modify();
        UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(Target.MutableEmitter, RendererClass, NAME_None, RF_Transactional);
        Target.MutableEmitter->AddRenderer(Renderer, Target.Version);
        Target.MutableEmitter->MarkPackageDirty();
        R->SetObjectField(TEXT("renderer"), RendererToJson(Renderer, Target.Data ? Target.Data->GetRenderers().Num() - 1 : INDEX_NONE));
    }
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraRemoveRendererImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    if (!Target.MutableEmitter || !Target.Data) return FOutcome::MakeError(-32603, TEXT("selected emitter has no renderer data"));

    int32 Index = INDEX_NONE;
    if (!Args->TryGetNumberField(TEXT("index"), Index) || !Target.Data->GetRenderers().IsValidIndex(Index))
    {
        return FOutcome::MakeError(-32602, TEXT("renderer index out of range"));
    }

    bool bDryRun = false, bSave = false, bCompile = true, bConfirmed = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (Target.StandaloneEmitter && !bDryRun && !bConfirmed)
    {
        return FOutcome::MakeError(-32602,
            TEXT("standalone NiagaraEmitter renderer mutation can affect all referencing systems; pass confirmed:true"));
    }

    UNiagaraRendererProperties* Renderer = Target.Data->GetRenderers()[Index];
    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("renderer"), RendererToJson(Renderer, Index));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("RemoveNiagaraRenderer", "Sage: Remove Niagara Renderer"));
        Target.MutableEmitter->Modify();
        Target.MutableEmitter->RemoveRenderer(Renderer, Target.Version);
        Target.MutableEmitter->MarkPackageDirty();
        R->SetBoolField(TEXT("removed"), true);
    }
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetRendererPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    if (!Target.Data) return FOutcome::MakeError(-32603, TEXT("selected emitter has no renderer data"));

    int32 Index = INDEX_NONE;
    if (!Args->TryGetNumberField(TEXT("index"), Index) || !Target.Data->GetRenderers().IsValidIndex(Index))
    {
        return FOutcome::MakeError(-32602, TEXT("renderer index out of range"));
    }
    FString PropertyName;
    if (!Args->TryGetStringField(TEXT("property"), PropertyName))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'property'"));
    }

    UNiagaraRendererProperties* Renderer = Target.Data->GetRenderers()[Index];
    FProperty* Prop = Renderer ? FindFProperty<FProperty>(Renderer->GetClass(), *PropertyName) : nullptr;
    if (!Prop) return FOutcome::MakeError(-32602, FString::Printf(TEXT("renderer property not found: %s"), *PropertyName));

    bool bDryRun = false, bSave = false, bCompile = true, bConfirmed = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (Target.StandaloneEmitter && !bDryRun && !bConfirmed)
    {
        return FOutcome::MakeError(-32602,
            TEXT("standalone NiagaraEmitter renderer mutation can affect all referencing systems; pass confirmed:true"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("before"), RendererToJson(Renderer, Index));
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("SetNiagaraRendererProperty", "Sage: Set Niagara Renderer Property"));
        Renderer->Modify();
        if (!detail::SetUPropertyFromJson(Renderer, Prop, Args->TryGetField(TEXT("value"))))
        {
            return FOutcome::MakeError(-32602, TEXT("property value type mismatch"));
        }
        Renderer->MarkPackageDirty();
        if (Target.MutableEmitter) Target.MutableEmitter->MarkPackageDirty();
    }
    R->SetObjectField(TEXT("after"), RendererToJson(Renderer, Index));
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else if (Target.MutableEmitter)
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

// ---- parameters ------------------------------------------------------------

FOutcome NiagaraListSystemParametersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), System->GetPathName());
    R->SetArrayField(TEXT("parameters"), ParameterStoreToJson(System->GetExposedParameters()));
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraUpsertUserParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, Name, TypeText;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    if (!Args->TryGetStringField(TEXT("type"), TypeText) || TypeText.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'type'"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    FNiagaraTypeDefinition Type;
    if (!TypeFromString(TypeText, Type))
    {
        return FOutcome::MakeError(-32602, FString::Printf(TEXT("unsupported type: %s"), *TypeText));
    }

    bool bDryRun = false, bSave = false, bCompile = true, bValidateOnly = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bValidateOnly);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    FNiagaraVariable Var(Type, FName(*NormalizeUserParameterName(Name, true)));
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), System->GetPathName());
    R->SetObjectField(TEXT("parameter"), VariableToJson(Var));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("validate_only"), bValidateOnly);
    if (bDryRun || bValidateOnly) return FOutcome::MakeSuccess(R);

    FScopedTransaction Tx(LOCTEXT("UpsertNiagaraUserParameter", "Sage: Upsert Niagara User Parameter"));
    System->Modify();
    System->GetExposedParameters().AddParameter(Var, true);
    FString Error;
    if (Args->HasField(TEXT("value")) &&
        !SetStoreValue(System->GetExposedParameters(), Var, Args->TryGetField(TEXT("value")), Error))
    {
        return FOutcome::MakeError(-32602, Error);
    }
    System->MarkPackageDirty();
    R->SetArrayField(TEXT("parameters"), ParameterStoreToJson(System->GetExposedParameters()));
    CompileIfRequested(System, bCompile, R);
    SaveLoadedAssetIfRequested(System, bSave, R);
    return FOutcome::MakeSuccess(R);
}

// ---- parameter collections -------------------------------------------------

FOutcome NiagaraCollectionListImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    int32 MaxResults = 0;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), SearchPath);
        Args->TryGetNumberField(TEXT("max_results"), MaxResults);
    }
    TArray<FAssetData> Collections = ListNiagaraAssets(TEXT("/Script/Niagara.NiagaraParameterCollection"), SearchPath);
    TArray<TSharedPtr<FJsonValue>> Assets;
    for (const FAssetData& D : Collections)
    {
        if (MaxResults > 0 && Assets.Num() >= MaxResults) break;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), D.AssetName.ToString());
        J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
        Assets.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("collections"), Assets);
    R->SetNumberField(TEXT("count"), Assets.Num());
    return FOutcome::MakeSuccess(R);
}

TSharedPtr<FJsonObject> CollectionToJson(UNiagaraParameterCollection* Collection)
{
    auto R = MakeShared<FJsonObject>();
    if (!Collection) return R;
    R->SetStringField(TEXT("path"), Collection->GetPathName());
    R->SetStringField(TEXT("namespace"), Collection->GetNamespace().ToString());
    TArray<TSharedPtr<FJsonValue>> Params;
    for (const FNiagaraVariable& Var : Collection->GetParameters())
    {
        Params.Add(MakeShared<FJsonValueObject>(VariableToJson(
            Var,
            Collection->GetDefaultInstance() ? &Collection->GetDefaultInstance()->GetParameterStore() : nullptr)));
    }
    R->SetArrayField(TEXT("parameters"), Params);
    return R;
}

FOutcome NiagaraCollectionReadImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraParameterCollection* Collection = LoadTypedAsset<UNiagaraParameterCollection>(Path);
    if (!Collection) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara Parameter Collection"));
    return FOutcome::MakeSuccess(CollectionToJson(Collection));
}

FOutcome NiagaraCollectionCreateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return FOutcome::MakeError(-32602, TEXT("invalid path"));
    }
    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UNiagaraParameterCollectionFactoryNew* Factory = NewObject<UNiagaraParameterCollectionFactoryNew>();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, UNiagaraParameterCollection::StaticClass(), Factory);
    if (!NewObj) return FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));
    return FOutcome::MakeSuccess(CollectionToJson(Cast<UNiagaraParameterCollection>(NewObj)));
}

FOutcome NiagaraCollectionUpsertParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, Name, TypeText;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    if (!Args->TryGetStringField(TEXT("type"), TypeText) || TypeText.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'type'"));
    }
    UNiagaraParameterCollection* Collection = LoadTypedAsset<UNiagaraParameterCollection>(Path);
    if (!Collection) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara Parameter Collection"));
    FNiagaraTypeDefinition Type;
    if (!TypeFromString(TypeText, Type)) return FOutcome::MakeError(-32602, TEXT("unsupported type"));

    bool bDryRun = false, bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FName FullName = Collection->ConditionalAddFullNamespace(FName(*Name));
    FNiagaraVariable Var(Type, FullName);
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetObjectField(TEXT("parameter"), VariableToJson(Var));
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("UpsertNiagaraCollectionParameter", "Sage: Upsert Niagara Collection Parameter"));
        Collection->Modify();
        Collection->AddParameter(FullName, Type);
        if (Collection->GetDefaultInstance())
        {
            FString Error;
            SetStoreValue(Collection->GetDefaultInstance()->GetParameterStore(), Var, Args->TryGetField(TEXT("value")), Error);
            Collection->GetDefaultInstance()->SyncWithCollection();
        }
        Collection->RefreshCompileId();
        Collection->MarkPackageDirty();
        SaveLoadedAssetIfRequested(Collection, bSave, R);
    }
    R->SetObjectField(TEXT("collection"), CollectionToJson(Collection));
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraCollectionSetDefaultImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    UNiagaraParameterCollection* Collection = LoadTypedAsset<UNiagaraParameterCollection>(Path);
    if (!Collection || !Collection->GetDefaultInstance())
    {
        return FOutcome::MakeError(-32602, TEXT("collection or default instance not found"));
    }

    FName FullName = Collection->ConditionalAddFullNamespace(FName(*Name));
    FNiagaraVariable* Existing = Collection->GetParameters().FindByPredicate([&](const FNiagaraVariable& Var)
    {
        return Var.GetName() == FullName;
    });
    if (!Existing) return FOutcome::MakeError(-32602, TEXT("collection parameter not found"));

    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);
    FScopedTransaction Tx(LOCTEXT("SetNiagaraCollectionDefault", "Sage: Set Niagara Collection Default"));
    Collection->Modify();
    FString Error;
    if (!SetStoreValue(Collection->GetDefaultInstance()->GetParameterStore(), *Existing, Args->TryGetField(TEXT("value")), Error))
    {
        return FOutcome::MakeError(-32602, Error);
    }
    Collection->GetDefaultInstance()->SetOverridesParameter(*Existing, true);
    Collection->GetDefaultInstance()->SyncWithCollection();
    Collection->RefreshCompileId();
    Collection->MarkPackageDirty();
    auto R = CollectionToJson(Collection);
    SaveLoadedAssetIfRequested(Collection, bSave, R.ToSharedRef());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraCollectionSetRuntimeValueImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.collection.set_runtime_value requires resolving a world-specific runtime collection instance; use collection.set_default for asset defaults until runtime world routing is added"));
}

// ---- scalability / bounds / validation ------------------------------------

FOutcome NiagaraReadScalabilityImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), System->GetPathName());
    R->SetBoolField(TEXT("fixed_bounds_enabled"), System->bFixedBounds != 0);
    R->SetObjectField(TEXT("fixed_bounds"), BoxToJsonObject(System->GetFixedBounds()));
    R->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
    R->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
    R->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
    R->SetStringField(TEXT("effect_type"), System->GetEffectType() ? System->GetEffectType()->GetPathName() : FString());
    R->SetBoolField(TEXT("override_scalability_settings"), System->GetOverrideScalabilitySettings());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetScalabilityImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    bool bOverride = false, bSave = false;
    bool bHasOverride = Args->TryGetBoolField(TEXT("override_scalability_settings"), bOverride);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FString Property;
    Args->TryGetStringField(TEXT("property"), Property);

    FScopedTransaction Tx(LOCTEXT("SetNiagaraScalability", "Sage: Set Niagara Scalability"));
    System->Modify();
    if (bHasOverride) System->SetOverrideScalabilitySettings(bOverride);
    if (!Property.IsEmpty())
    {
        FProperty* Prop = FindFProperty<FProperty>(System->GetClass(), *Property);
        if (!Prop) return FOutcome::MakeError(-32602, TEXT("scalability property not found on system"));
        if (!detail::SetUPropertyFromJson(System, Prop, Args->TryGetField(TEXT("value"))))
        {
            return FOutcome::MakeError(-32602, TEXT("property value type mismatch"));
        }
    }
    System->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("changed"), true);
    SaveLoadedAssetIfRequested(System, bSave, R);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetFixedBoundsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FBox Bounds;
    if (!ArgsToBox(Args, Bounds)) return FOutcome::MakeError(-32602, TEXT("missing bounds as {min,max} or [minx,miny,minz,maxx,maxy,maxz]"));

    FString Error;
    if (UNiagaraComponent* Component = ResolveNiagaraComponentFromArgs(Args, Error))
    {
        FString EmitterName;
        Args->TryGetStringField(TEXT("emitter"), EmitterName);
        if (EmitterName.IsEmpty())
        {
            Component->SetSystemFixedBounds(Bounds);
        }
        else
        {
            Component->SetEmitterFixedBounds(FName(*EmitterName), Bounds);
        }
        return FOutcome::MakeSuccess(ComponentToJson(Component));
    }

    FString Path;
    if (!Args->TryGetStringField(TEXT("path"), Path)) return FOutcome::MakeError(-32602, TEXT("missing 'path' or component"));
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);
    FScopedTransaction Tx(LOCTEXT("SetNiagaraFixedBounds", "Sage: Set Niagara Fixed Bounds"));
    System->Modify();
    System->bFixedBounds = true;
    System->SetFixedBounds(Bounds);
    System->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("fixed_bounds"), BoxToJsonObject(System->GetFixedBounds()));
    SaveLoadedAssetIfRequested(System, bSave, R);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraClearFixedBoundsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Error;
    if (UNiagaraComponent* Component = ResolveNiagaraComponentFromArgs(Args, Error))
    {
        FString EmitterName;
        Args->TryGetStringField(TEXT("emitter"), EmitterName);
        if (EmitterName.IsEmpty()) Component->ClearSystemFixedBounds();
        else Component->ClearEmitterFixedBounds(FName(*EmitterName));
        return FOutcome::MakeSuccess(ComponentToJson(Component));
    }

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path' or component"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));
    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);
    FScopedTransaction Tx(LOCTEXT("ClearNiagaraFixedBounds", "Sage: Clear Niagara Fixed Bounds"));
    System->Modify();
    System->bFixedBounds = false;
    System->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("fixed_bounds_enabled"), false);
    SaveLoadedAssetIfRequested(System, bSave, R);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetWarmupImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    double WarmupTime = System->GetWarmupTime();
    double TickDelta = System->GetWarmupTickDelta();
    Args->TryGetNumberField(TEXT("warmup_time"), WarmupTime);
    Args->TryGetNumberField(TEXT("tick_delta"), TickDelta);
    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);

    FScopedTransaction Tx(LOCTEXT("SetNiagaraWarmup", "Sage: Set Niagara Warmup"));
    System->Modify();
    System->SetWarmupTime(static_cast<float>(WarmupTime));
    System->SetWarmupTickDelta(static_cast<float>(TickDelta));
    if (FProperty* TickCountProp = FindFProperty<FProperty>(System->GetClass(), TEXT("WarmupTickCount")))
    {
        if (Args->HasField(TEXT("tick_count")))
        {
            detail::SetUPropertyFromJson(System, TickCountProp, Args->TryGetField(TEXT("tick_count")));
        }
    }
    System->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
    R->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
    R->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
    SaveLoadedAssetIfRequested(System, bSave, R);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetEffectTypeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, EffectTypePath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    Args->TryGetStringField(TEXT("effect_type"), EffectTypePath);
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));
    UNiagaraEffectType* EffectType = EffectTypePath.IsEmpty() ? nullptr : LoadTypedAsset<UNiagaraEffectType>(EffectTypePath);
    if (!EffectTypePath.IsEmpty() && !EffectType) return FOutcome::MakeError(-32602, TEXT("'effect_type' is not a NiagaraEffectType"));

    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);
    FScopedTransaction Tx(LOCTEXT("SetNiagaraEffectType", "Sage: Set Niagara Effect Type"));
    System->Modify();
    System->SetEffectType(EffectType);
    System->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("effect_type"), System->GetEffectType() ? System->GetEffectType()->GetPathName() : FString());
    SaveLoadedAssetIfRequested(System, bSave, R);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraValidateSystemImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraSystem* System = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!System) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (System->GetEmitterHandles().Num() == 0)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("system has no emitter handles")));
    }
    if (!System->bFixedBounds)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("system fixed bounds are disabled; long or fast GPU effects may cull incorrectly")));
    }
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
        if (!Data) continue;
        if (Data->SimTarget == ENiagaraSimTarget::GPUComputeSim && !System->bFixedBounds &&
            Data->CalculateBoundsMode != ENiagaraEmitterCalculateBoundMode::Fixed)
        {
            Warnings.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("GPU emitter '%s' has no fixed bounds safety"), *Handle.GetName().ToString())));
        }
        if (Data->GetRenderers().Num() == 0)
        {
            Warnings.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("emitter '%s' has no renderers"), *Handle.GetName().ToString())));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), System->GetPathName());
    R->SetBoolField(TEXT("ok"), Warnings.Num() == 0);
    R->SetArrayField(TEXT("warnings"), Warnings);
    return FOutcome::MakeSuccess(R);
}

// ---- sim cache / events / stages ------------------------------------------

FOutcome NiagaraCaptureSimCacheImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return FOutcome::MakeError(-32602, TEXT("invalid path"));
    }
    if (LoadAnyAsset(Path))
    {
        return FOutcome::MakeError(-32602, TEXT("SimCache asset already exists; choose a new path"));
    }

    FString Error;
    UNiagaraComponent* Component = ResolveNiagaraComponentFromArgs(Args, Error);
    if (!Component) return FOutcome::MakeError(-32602, Error);
    UWorld* World = Component->GetWorld();
    if (!World) return FOutcome::MakeError(-32603, TEXT("component has no world"));

    UNiagaraSimCache* SimCache = UNiagaraSimCacheFunctionLibrary::CreateNiagaraSimCache(World);
    if (!SimCache) return FOutcome::MakeError(-32000, TEXT("CreateNiagaraSimCache failed"));

    bool bAdvance = false;
    double Delta = 1.0 / 60.0;
    int32 AdvanceTicks = 0;
    int32 FrameCount = 1;
    double CaptureRate = 0.0;
    Args->TryGetBoolField(TEXT("advance_simulation"), bAdvance);
    Args->TryGetNumberField(TEXT("advance_ticks"), AdvanceTicks);
    Args->TryGetNumberField(TEXT("advance_delta_time"), Delta);
    Args->TryGetNumberField(TEXT("frames"), FrameCount);
    if (Args->TryGetNumberField(TEXT("capture_rate"), CaptureRate) && CaptureRate > 0.0)
    {
        Delta = 1.0 / CaptureRate;
    }
    FrameCount = FMath::Max(1, FrameCount);
    if (AdvanceTicks > 0)
    {
        Component->AdvanceSimulation(AdvanceTicks, static_cast<float>(Delta));
    }
    FNiagaraSimCacheCreateParameters Params = FNiagaraSimCacheCreateParameters::CreateForDebugging();
    UNiagaraSimCache* OutCache = nullptr;
    if (FrameCount > 1)
    {
        if (!SimCache->BeginWrite(Params, Component))
        {
            return FOutcome::MakeError(-32000, TEXT("SimCache BeginWrite failed; component may be inactive or incompatible"));
        }
        for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
        {
            if (FrameIndex > 0 || bAdvance)
            {
                Component->AdvanceSimulation(1, static_cast<float>(Delta));
            }
            if (!SimCache->WriteFrame(Component))
            {
                SimCache->EndWrite(/*bAllowAnalytics=*/false);
                return FOutcome::MakeError(-32000, FString::Printf(TEXT("SimCache WriteFrame failed at frame %d"), FrameIndex));
            }
        }
        SimCache->EndWrite(/*bAllowAnalytics=*/false);
        OutCache = SimCache;
    }
    else
    {
        const bool bOk = UNiagaraSimCacheFunctionLibrary::CaptureNiagaraSimCacheImmediate(
            SimCache, Params, Component, OutCache, bAdvance, static_cast<float>(Delta));
        if (!bOk || !OutCache)
        {
            return FOutcome::MakeError(-32000, TEXT("CaptureNiagaraSimCacheImmediate failed; component may be inactive or not capturable"));
        }
    }
    if (!OutCache)
    {
        return FOutcome::MakeError(-32000, TEXT("SimCache capture did not produce a cache"));
    }

    const FString PackageName = PackagePath / AssetName;
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package) return FOutcome::MakeError(-32000, TEXT("CreatePackage failed for SimCache path"));
    Package->FullyLoad();
    UNiagaraSimCache* AssetCache = DuplicateObject<UNiagaraSimCache>(OutCache, Package, FName(*AssetName));
    if (!AssetCache) return FOutcome::MakeError(-32000, TEXT("DuplicateObject failed for SimCache asset"));
    AssetCache->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    FAssetRegistryModule::AssetCreated(AssetCache);
    AssetCache->MarkPackageDirty();

    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("sim_cache"), AssetCache->GetPathName());
    R->SetStringField(TEXT("package"), PackageName);
    R->SetNumberField(TEXT("frames"), AssetCache->GetNumFrames());
    R->SetNumberField(TEXT("emitters"), AssetCache->GetNumEmitters());
    R->SetNumberField(TEXT("duration"), AssetCache->GetDurationSeconds());
    R->SetArrayField(TEXT("emitter_names"), NameArrayToJson(AssetCache->GetEmitterNames()));
    SaveLoadedAssetIfRequested(AssetCache, bSave, R);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraReadSimCacheImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        Args->TryGetStringField(TEXT("sim_cache"), Path);
    }
    if (Path.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UNiagaraSimCache* SimCache = LoadTypedAsset<UNiagaraSimCache>(Path);
    if (!SimCache)
    {
        SimCache = FindObject<UNiagaraSimCache>(nullptr, *Path);
    }
    if (!SimCache) return FOutcome::MakeError(-32602, TEXT("SimCache not found"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), SimCache->GetPathName());
    R->SetNumberField(TEXT("frames"), SimCache->GetNumFrames());
    R->SetNumberField(TEXT("emitters"), SimCache->GetNumEmitters());
    R->SetNumberField(TEXT("duration"), SimCache->GetDurationSeconds());
    R->SetArrayField(TEXT("emitter_names"), NameArrayToJson(SimCache->GetEmitterNames()));
    TArray<TSharedPtr<FJsonValue>> Counts;
    for (int32 EmitterIndex = 0; EmitterIndex < SimCache->GetNumEmitters(); ++EmitterIndex)
    {
        auto E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("name"), SimCache->GetEmitterName(EmitterIndex).ToString());
        TArray<TSharedPtr<FJsonValue>> Frames;
        for (int32 FrameIndex = 0; FrameIndex < SimCache->GetNumFrames(); ++FrameIndex)
        {
            auto F = MakeShared<FJsonObject>();
            F->SetNumberField(TEXT("frame"), FrameIndex);
            F->SetNumberField(TEXT("instances"), SimCache->GetEmitterNumInstances(EmitterIndex, FrameIndex));
            Frames.Add(MakeShared<FJsonValueObject>(F));
        }
        E->SetArrayField(TEXT("frames"), Frames);
        Counts.Add(MakeShared<FJsonValueObject>(E));
    }
    R->SetArrayField(TEXT("particle_counts"), Counts);

    bool bIncludeJson = false;
    Args->TryGetBoolField(TEXT("include_json"), bIncludeJson);
    if (bIncludeJson)
    {
        R->SetObjectField(TEXT("raw"), FNiagaraSimCacheJson::ToJson(*SimCache));
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraCompareSimCacheImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString APath, BPath;
    if (!Args.IsValid())
    {
        return FOutcome::MakeError(-32602, TEXT("missing arguments"));
    }
    Args->TryGetStringField(TEXT("a"), APath);
    Args->TryGetStringField(TEXT("b"), BPath);
    if (APath.IsEmpty()) Args->TryGetStringField(TEXT("baseline"), APath);
    if (BPath.IsEmpty()) Args->TryGetStringField(TEXT("candidate"), BPath);
    if (APath.IsEmpty() || BPath.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing baseline/candidate sim cache paths"));
    }
    UNiagaraSimCache* A = LoadTypedAsset<UNiagaraSimCache>(APath);
    UNiagaraSimCache* B = LoadTypedAsset<UNiagaraSimCache>(BPath);
    if (!A) A = FindObject<UNiagaraSimCache>(nullptr, *APath);
    if (!B) B = FindObject<UNiagaraSimCache>(nullptr, *BPath);
    if (!A || !B) return FOutcome::MakeError(-32602, TEXT("one or both SimCache assets not found"));

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("frame_delta"), B->GetNumFrames() - A->GetNumFrames());
    R->SetNumberField(TEXT("duration_delta"), B->GetDurationSeconds() - A->GetDurationSeconds());
    R->SetNumberField(TEXT("emitter_count_delta"), B->GetNumEmitters() - A->GetNumEmitters());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraListEventHandlersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    TArray<TSharedPtr<FJsonValue>> Events;
    if (Target.Data)
    {
        int32 Index = 0;
        for (const FNiagaraEventScriptProperties& Event : Target.Data->GetEventHandlers())
        {
            auto E = MakeShared<FJsonObject>();
            E->SetNumberField(TEXT("index"), Index++);
            E->SetStringField(TEXT("usage_id"), Event.Script ? GuidToString(Event.Script->GetUsageId()) : FString());
            E->SetStringField(TEXT("script"), Event.Script ? Event.Script->GetPathName() : FString());
            Events.Add(MakeShared<FJsonValueObject>(E));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("event_handlers"), Events);
    R->SetNumberField(TEXT("count"), Events.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraRemoveEventHandlerImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    if (!Target.MutableEmitter) return FOutcome::MakeError(-32603, TEXT("selected emitter has no mutable emitter"));
    FString UsageIdText;
    if (!Args->TryGetStringField(TEXT("usage_id"), UsageIdText))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'usage_id'"));
    }
    FGuid UsageId;
    if (!ParseGuidString(UsageIdText, UsageId)) return FOutcome::MakeError(-32602, TEXT("invalid usage_id"));
    FScopedTransaction Tx(LOCTEXT("RemoveNiagaraEventHandler", "Sage: Remove Niagara Event Handler"));
    Target.MutableEmitter->Modify();
    Target.MutableEmitter->RemoveEventHandlerByUsageId(UsageId, Target.Version);
    Target.MutableEmitter->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("removed"), true);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraAddEventHandlerImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.add_event_handler is unsupported until Sage exposes the full FNiagaraEventScriptProperties construction contract"));
}

FOutcome NiagaraListSimulationStagesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    TArray<TSharedPtr<FJsonValue>> Stages;
    if (Target.Data)
    {
        int32 Index = 0;
        for (UNiagaraSimulationStageBase* Stage : Target.Data->GetSimulationStages())
        {
            auto S = ReflectedObjectToJson(Stage, /*bEditableOnly=*/true);
            S->SetNumberField(TEXT("index"), Index++);
            S->SetStringField(TEXT("script"), Stage && Stage->Script ? Stage->Script->GetPathName() : FString());
            Stages.Add(MakeShared<FJsonValueObject>(S));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("simulation_stages"), Stages);
    R->SetNumberField(TEXT("count"), Stages.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraAddSimulationStageImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.add_simulation_stage is unsupported until Sage exposes the Niagara simulation stage factory/setup contract"));
}

FOutcome NiagaraRemoveSimulationStageImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    if (!Target.MutableEmitter || !Target.Data) return FOutcome::MakeError(-32603, TEXT("selected emitter has no mutable stage data"));
    int32 Index = INDEX_NONE;
    if (!Args->TryGetNumberField(TEXT("index"), Index) || !Target.Data->GetSimulationStages().IsValidIndex(Index))
    {
        return FOutcome::MakeError(-32602, TEXT("simulation stage index out of range"));
    }
    UNiagaraSimulationStageBase* Stage = Target.Data->GetSimulationStages()[Index];
    FScopedTransaction Tx(LOCTEXT("RemoveNiagaraSimulationStage", "Sage: Remove Niagara Simulation Stage"));
    Target.MutableEmitter->Modify();
    Target.MutableEmitter->RemoveSimulationStage(Stage, Target.Version);
    Target.MutableEmitter->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("removed"), true);
    return FOutcome::MakeSuccess(R);
}

// ---- dependencies ----------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> PackageNamesToJson(const TArray<FName>& Names)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FName& Name : Names)
    {
        Out.Add(MakeShared<FJsonValueString>(Name.ToString()));
    }
    return Out;
}

FOutcome NiagaraGetDependenciesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Obj = LoadAnyAsset(Path);
    if (!Obj) return FOutcome::MakeError(-32602, TEXT("asset not found"));
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FName> Deps;
    ARM.Get().GetDependencies(Obj->GetOutermost()->GetFName(), Deps);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetArrayField(TEXT("dependencies"), PackageNamesToJson(Deps));
    R->SetNumberField(TEXT("count"), Deps.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraFindReferencersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Obj = LoadAnyAsset(Path);
    if (!Obj) return FOutcome::MakeError(-32602, TEXT("asset not found"));
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FName> Refs;
    ARM.Get().GetReferencers(Obj->GetOutermost()->GetFName(), Refs);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetArrayField(TEXT("referencers"), PackageNamesToJson(Refs));
    R->SetNumberField(TEXT("count"), Refs.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraValidateDependenciesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Deps = NiagaraGetDependenciesImpl(Args);
    if (!Deps.bSuccess) return Deps;
    auto R = Deps.Result.IsValid() ? Deps.Result : MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Warnings;
    const TArray<TSharedPtr<FJsonValue>>* DepArr = nullptr;
    if (R->TryGetArrayField(TEXT("dependencies"), DepArr) && DepArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *DepArr)
        {
            const FString Package = V->AsString();
            if (Package.Contains(TEXT("Redirector")))
            {
                Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("redirector dependency: %s"), *Package)));
            }
        }
    }
    R->SetArrayField(TEXT("warnings"), Warnings);
    R->SetBoolField(TEXT("ok"), Warnings.Num() == 0);
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraAuditCrossAssetRefsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Deps = NiagaraGetDependenciesImpl(Args);
    if (!Deps.bSuccess) return Deps;
    auto R = Deps.Result.IsValid() ? Deps.Result : MakeShared<FJsonObject>();
    R->SetStringField(TEXT("audit_scope"), TEXT("AssetRegistry package dependencies plus renderer/material reflected refs"));
    return FOutcome::MakeSuccess(R);
}

// ---- data interfaces / static switches / HLSL ------------------------------

FOutcome NiagaraInspectDataInterfacesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;

    TArray<TSharedPtr<FJsonValue>> Interfaces;
    TArray<UNiagaraScript*> Scripts;
    if (Target.Data) Target.Data->GetScripts(Scripts, /*bCompilableOnly=*/false, /*bEnabledOnly=*/false);
    for (UNiagaraScript* Script : Scripts)
    {
        if (!Script) continue;
        for (const FNiagaraScriptDataInterfaceInfo& Info : Script->GetCachedDefaultDataInterfaces())
        {
            auto J = ReflectedObjectToJson(Info.DataInterface, /*bEditableOnly=*/true);
            J->SetStringField(TEXT("script"), Script->GetPathName());
            J->SetStringField(TEXT("name"), Info.Name.ToString());
            J->SetStringField(TEXT("compile_name"), Info.CompileName.ToString());
            J->SetStringField(TEXT("type"), TypeDefToString(Info.Type));
            Interfaces.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    R->SetArrayField(TEXT("data_interfaces"), Interfaces);
    R->SetNumberField(TEXT("count"), Interfaces.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraListStaticSwitchesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<TSharedPtr<FJsonValue>> Switches;
    for (UNiagaraNodeFunctionCall* Node : CollectFunctionNodes(Source->NodeGraph))
    {
        for (const FNiagaraPropagatedVariable& Propagated : Node->PropagatedStaticSwitchParameters)
        {
            auto S = MakeShared<FJsonObject>();
            const FNiagaraVariable Var = Propagated.ToVariable();
            S->SetStringField(TEXT("module"), Node->GetFunctionName());
            S->SetStringField(TEXT("module_id"), GuidToString(Node->NodeGuid));
            S->SetStringField(TEXT("name"), Var.GetName().ToString());
            S->SetStringField(TEXT("type"), TypeDefToString(Var.GetType()));
            if (UEdGraphPin* Pin = FindInputPinByName(Node, Var.GetName()))
            {
                S->SetObjectField(TEXT("pin"), PinToJson(Pin));
            }
            Switches.Add(MakeShared<FJsonValueObject>(S));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("switches"), Switches);
    R->SetNumberField(TEXT("count"), Switches.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetStaticSwitchImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<UNiagaraNodeFunctionCall*> Nodes = CollectFunctionNodes(Source->NodeGraph);
    FString Error;
    UNiagaraNodeFunctionCall* Node = ResolveModuleNode(Args, Nodes, Error);
    if (!Node) return FOutcome::MakeError(-32602, Error);

    FString SwitchName;
    if (!Args->TryGetStringField(TEXT("switch"), SwitchName))
    {
        Args->TryGetStringField(TEXT("name"), SwitchName);
    }
    if (SwitchName.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'switch'"));

    UEdGraphPin* Pin = FindInputPinByName(Node, FName(*SwitchName));
    if (!Pin) return FOutcome::MakeError(-32602, TEXT("static switch pin not found"));

    bool bDryRun = false, bSave = false, bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("before"), PinToJson(Pin));
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("SetNiagaraStaticSwitch", "Sage: Set Niagara Static Switch"));
        Source->NodeGraph->Modify();
        Pin->Modify();
        Pin->DefaultValue = JsonValueToPinDefault(Args->TryGetField(TEXT("value")));
        Source->NodeGraph->NotifyGraphChanged();
        Target.RootAsset->MarkPackageDirty();
    }
    R->SetObjectField(TEXT("after"), PinToJson(Pin));
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraGetCompiledHlslImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UNiagaraScript* Script = LoadTypedAsset<UNiagaraScript>(Path);
    if (!Script)
    {
        return FOutcome::MakeError(-32602,
            TEXT("'path' must be a UNiagaraScript asset for compiled HLSL readback; system/emitter graph HLSL requires private compiler context"));
    }
    FProperty* HlslProp = FindFProperty<FProperty>(Script->GetClass(), TEXT("LastHlslTranslation"));
    if (!HlslProp)
    {
        return FOutcome::MakeError(-32602, TEXT("LastHlslTranslation is not exposed on this engine build"));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Script->GetPathName());
    R->SetField(TEXT("hlsl"), detail::GetUPropertyAsJson(Script, HlslProp));
    R->SetStringField(TEXT("usage"), UsageToString(Script->GetUsage()));
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraCreateModuleFromHlslImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.create_module_from_hlsl is unsupported in this source pass; UE requires Niagara script source graph/HLSL node setup beyond exported stable API"));
}

FOutcome NiagaraCreateScratchModuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.create_scratch_module is unsupported until Sage owns the scratch-pad graph construction contract"));
}

FOutcome NiagaraListDynamicInputsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    int32 MaxResults = 500;
    int32 ModuleIndexFilter = INDEX_NONE;
    FString ModuleFilter;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("max_results"), MaxResults);
        Args->TryGetNumberField(TEXT("limit"), MaxResults);
        Args->TryGetNumberField(TEXT("module_index"), ModuleIndexFilter);
        if (ModuleIndexFilter == INDEX_NONE) Args->TryGetNumberField(TEXT("index"), ModuleIndexFilter);
        Args->TryGetStringField(TEXT("module"), ModuleFilter);
        if (ModuleFilter.IsEmpty()) Args->TryGetStringField(TEXT("module_id"), ModuleFilter);
        if (ModuleFilter.IsEmpty()) Args->TryGetStringField(TEXT("node_id"), ModuleFilter);
        if (ModuleFilter.IsEmpty()) Args->TryGetStringField(TEXT("display_name"), ModuleFilter);
    }
    MaxResults = FMath::Clamp(MaxResults, 1, 5000);

    FGuid ModuleGuid;
    const bool bHasModuleGuid = !ModuleFilter.IsEmpty() && ParseGuidString(ModuleFilter, ModuleGuid);
    TArray<UNiagaraNodeFunctionCall*> Modules = CollectFunctionNodes(Source->NodeGraph);
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (int32 ModuleIndex = 0; ModuleIndex < Modules.Num() && Rows.Num() < MaxResults; ++ModuleIndex)
    {
        UNiagaraNodeFunctionCall* ModuleNode = Modules[ModuleIndex];
        if (!ModuleNode) continue;
        if (ModuleIndexFilter != INDEX_NONE && ModuleIndexFilter != ModuleIndex) continue;
        if (!ModuleFilter.IsEmpty())
        {
            const bool bMatches =
                (bHasModuleGuid && ModuleNode->NodeGuid == ModuleGuid)
                || ModuleNode->GetFunctionName().Contains(ModuleFilter, ESearchCase::IgnoreCase)
                || ModuleNode->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(ModuleFilter, ESearchCase::IgnoreCase)
                || (ModuleNode->FunctionScript && ModuleNode->FunctionScript->GetPathName().Contains(ModuleFilter, ESearchCase::IgnoreCase))
                || ModuleNode->FunctionScriptAssetObjectPath.ToString().Contains(ModuleFilter, ESearchCase::IgnoreCase);
            if (!bMatches) continue;
        }

        for (UEdGraphPin* InputPin : ModuleNode->Pins)
        {
            if (!InputPin || InputPin->Direction != EGPD_Input || InputPin->LinkedTo.Num() == 0) continue;
            for (int32 LinkIndex = 0; LinkIndex < InputPin->LinkedTo.Num() && Rows.Num() < MaxResults; ++LinkIndex)
            {
                UEdGraphPin* SourcePin = InputPin->LinkedTo[LinkIndex];
                UEdGraphNode* SourceNode = SourcePin ? SourcePin->GetOwningNode() : nullptr;
                auto Row = MakeShared<FJsonObject>();
                Row->SetNumberField(TEXT("module_index"), ModuleIndex);
                Row->SetStringField(TEXT("module_node_id"), GuidToString(ModuleNode->NodeGuid));
                Row->SetStringField(TEXT("module_display_name"), ModuleNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
                Row->SetStringField(TEXT("module_function_name"), ModuleNode->GetFunctionName());
                Row->SetStringField(TEXT("module_script"), ModuleNode->FunctionScript ? ModuleNode->FunctionScript->GetPathName() : FString());
                Row->SetObjectField(TEXT("input_pin"), PinToJson(InputPin));
                Row->SetNumberField(TEXT("link_index"), LinkIndex);
                Row->SetObjectField(TEXT("source_pin"), PinToJson(SourcePin));
                if (SourceNode)
                {
                    Row->SetStringField(TEXT("source_node_id"), GuidToString(SourceNode->NodeGuid));
                    Row->SetStringField(TEXT("source_node_class"), SourceNode->GetClass()->GetPathName());
                    Row->SetStringField(TEXT("source_node_title"), SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
                    Row->SetNumberField(TEXT("source_node_x"), SourceNode->NodePosX);
                    Row->SetNumberField(TEXT("source_node_y"), SourceNode->NodePosY);
                    if (UNiagaraNodeFunctionCall* SourceFunction = Cast<UNiagaraNodeFunctionCall>(SourceNode))
                    {
                        Row->SetStringField(TEXT("source_function_name"), SourceFunction->GetFunctionName());
                        Row->SetStringField(TEXT("source_function_script"), SourceFunction->FunctionScript ? SourceFunction->FunctionScript->GetPathName() : FString());
                        Row->SetStringField(TEXT("source_called_usage"), UsageToString(SourceFunction->GetCalledUsage()));
                    }
                }
                Rows.Add(MakeShared<FJsonValueObject>(Row));
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    R->SetArrayField(TEXT("dynamic_inputs"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetBoolField(TEXT("capped"), Rows.Num() >= MaxResults);
    R->SetStringField(TEXT("mode"), TEXT("read_only_pin_link_traversal"));
    R->SetStringField(TEXT("readback_note"), TEXT("Reports linked module input pins as dynamic-input candidates without mutating Niagara stack state."));
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraSetDynamicInputImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.set_dynamic_input is unsupported until Sage can safely create linked dynamic input nodes through exported APIs"));
}

FOutcome NiagaraSetCustomExpressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.set_custom_expression is unsupported until Sage can safely create UNiagaraNodeCustomHlsl through exported APIs"));
}

FOutcome NiagaraExportGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : Source->NodeGraph->Nodes)
    {
        auto N = MakeShared<FJsonObject>();
        N->SetStringField(TEXT("id"), GuidToString(Node->NodeGuid));
        N->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
        N->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        N->SetNumberField(TEXT("x"), Node->NodePosX);
        N->SetNumberField(TEXT("y"), Node->NodePosY);
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
        }
        N->SetArrayField(TEXT("pins"), Pins);
        Nodes.Add(MakeShared<FJsonValueObject>(N));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    R->SetArrayField(TEXT("nodes"), Nodes);
    R->SetNumberField(TEXT("node_count"), Nodes.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome NiagaraCapturePreviewImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FOutcome::MakeError(-32602,
        TEXT("niagara.capture_preview is not implemented without a viewport/render-target capture pipeline; use preview_spawn + capture_sim_cache for numeric validation"));
}

// ---- reference benchmark discovery aliases --------------------------------

FString FirstNiagaraStringArg(const TSharedPtr<FJsonObject>& Args,
                              std::initializer_list<const TCHAR*> Names,
                              const FString& Fallback = FString())
{
    if (!Args.IsValid()) return Fallback;
    FString Value;
    for (const TCHAR* Name : Names)
    {
        if (Args->TryGetStringField(Name, Value) && !Value.IsEmpty())
        {
            return Value;
        }
    }
    return Fallback;
}

int32 NiagaraMaxResultsArg(const TSharedPtr<FJsonObject>& Args, int32 DefaultValue)
{
    int32 MaxResults = DefaultValue;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("max_results"), MaxResults);
        Args->TryGetNumberField(TEXT("limit"), MaxResults);
    }
    return FMath::Clamp(MaxResults, 1, 5000);
}

bool TextContainsInsensitive(const FString& Haystack, const FString& Needle)
{
    return Needle.IsEmpty() || Haystack.Contains(Needle, ESearchCase::IgnoreCase);
}

FString JsonObjectToString(const TSharedPtr<FJsonObject>& Obj)
{
    if (!Obj.IsValid()) return FString();
    FString Out;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
    return Out;
}

struct FNiagaraAssetRow
{
    FAssetData Asset;
    FString Type;
};

TArray<FNiagaraAssetRow> CollectNiagaraAssetRows(const FString& SearchPath)
{
    TArray<FNiagaraAssetRow> Rows;
    auto Append = [&Rows](const TArray<FAssetData>& Assets, const FString& Type)
    {
        for (const FAssetData& Asset : Assets)
        {
            Rows.Add({Asset, Type});
        }
    };
    Append(ListNiagaraAssets(TEXT("/Script/Niagara.NiagaraSystem"), SearchPath), TEXT("NiagaraSystem"));
    Append(ListNiagaraAssets(TEXT("/Script/Niagara.NiagaraEmitter"), SearchPath), TEXT("NiagaraEmitter"));
    Append(ListNiagaraAssets(TEXT("/Script/Niagara.NiagaraParameterCollection"), SearchPath), TEXT("NiagaraParameterCollection"));
    return Rows;
}

bool AddNiagaraSearchRow(TArray<TSharedPtr<FJsonValue>>& Rows,
                         int32 MaxResults,
                         const TSharedPtr<FJsonObject>& Row)
{
    if (!Row.IsValid() || Rows.Num() >= MaxResults) return false;
    Rows.Add(MakeShared<FJsonValueObject>(Row));
    return Rows.Num() < MaxResults;
}

void AddParameterMatchRows(const FString& AssetPath,
                           const FString& AssetType,
                           const FString& EmitterName,
                           const FString& Scope,
                           const TArray<FNiagaraVariable>& Vars,
                           const FNiagaraParameterStore* Store,
                           const FString& Query,
                           TArray<TSharedPtr<FJsonValue>>& Rows,
                           int32 MaxResults)
{
    for (const FNiagaraVariable& Var : Vars)
    {
        const FString Name = Var.GetName().ToString();
        const FString Type = TypeDefToString(Var.GetType());
        if (!TextContainsInsensitive(Name, Query) && !TextContainsInsensitive(Type, Query)) continue;
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("asset"), AssetPath);
        Row->SetStringField(TEXT("asset_type"), AssetType);
        Row->SetStringField(TEXT("emitter"), EmitterName);
        Row->SetStringField(TEXT("scope"), Scope);
        Row->SetObjectField(TEXT("parameter"), VariableToJson(Var, Store));
        if (!AddNiagaraSearchRow(Rows, MaxResults, Row)) return;
    }
}

void AddEmitterParameterMatches(const FString& AssetPath,
                                const FString& AssetType,
                                const FString& EmitterName,
                                FVersionedNiagaraEmitterData* Data,
                                const FString& Query,
                                TArray<TSharedPtr<FJsonValue>>& Rows,
                                int32 MaxResults)
{
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Data);
    if (!Source || !Source->NodeGraph) return;
    TArray<UNiagaraNodeFunctionCall*> Nodes = CollectFunctionNodes(Source->NodeGraph);
    for (UNiagaraNodeFunctionCall* Node : Nodes)
    {
        if (!Node) continue;
        AddParameterMatchRows(AssetPath, AssetType, EmitterName,
            FString::Printf(TEXT("module:%s"), *Node->GetFunctionName()),
            Node->Signature.Inputs, nullptr, Query, Rows, MaxResults);
        if (Rows.Num() >= MaxResults) return;
    }
}

void AddDataInterfaceMatchRows(const FString& AssetPath,
                               const FString& AssetType,
                               const FString& EmitterName,
                               FVersionedNiagaraEmitterData* Data,
                               const FString& Query,
                               TArray<TSharedPtr<FJsonValue>>& Rows,
                               int32 MaxResults)
{
    if (!Data) return;
    TArray<UNiagaraScript*> Scripts;
    Data->GetScripts(Scripts, /*bCompilableOnly=*/false, /*bEnabledOnly=*/false);
    for (UNiagaraScript* Script : Scripts)
    {
        if (!Script) continue;
        for (const FNiagaraScriptDataInterfaceInfo& Info : Script->GetCachedDefaultDataInterfaces())
        {
            const FString ClassName = Info.DataInterface ? Info.DataInterface->GetClass()->GetPathName() : FString();
            const FString TypeName = TypeDefToString(Info.Type);
            const FString Name = Info.Name.ToString();
            if (!TextContainsInsensitive(Name, Query)
                && !TextContainsInsensitive(Info.CompileName.ToString(), Query)
                && !TextContainsInsensitive(TypeName, Query)
                && !TextContainsInsensitive(ClassName, Query))
            {
                continue;
            }
            auto Row = ReflectedObjectToJson(Info.DataInterface, /*bEditableOnly=*/true);
            Row->SetStringField(TEXT("asset"), AssetPath);
            Row->SetStringField(TEXT("asset_type"), AssetType);
            Row->SetStringField(TEXT("emitter"), EmitterName);
            Row->SetStringField(TEXT("script"), Script->GetPathName());
            Row->SetStringField(TEXT("name"), Name);
            Row->SetStringField(TEXT("compile_name"), Info.CompileName.ToString());
            Row->SetStringField(TEXT("type"), TypeName);
            if (!AddNiagaraSearchRow(Rows, MaxResults, Row)) return;
        }
    }
}

void AddRendererMaterialMatchRows(const FString& AssetPath,
                                  const FString& AssetType,
                                  const FString& EmitterName,
                                  FVersionedNiagaraEmitterData* Data,
                                  const FString& Query,
                                  TArray<TSharedPtr<FJsonValue>>& Rows,
                                  int32 MaxResults)
{
    if (!Data) return;
    const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
    for (int32 Index = 0; Index < Renderers.Num(); ++Index)
    {
        UNiagaraRendererProperties* Renderer = Renderers[Index];
        TSharedPtr<FJsonObject> RendererJson = RendererToJson(Renderer, Index);
        const FString Serialized = JsonObjectToString(RendererJson);
        if (!TextContainsInsensitive(Serialized, Query)) continue;
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("asset"), AssetPath);
        Row->SetStringField(TEXT("asset_type"), AssetType);
        Row->SetStringField(TEXT("emitter"), EmitterName);
        Row->SetObjectField(TEXT("renderer"), RendererJson);
        if (!AddNiagaraSearchRow(Rows, MaxResults, Row)) return;
    }
}

void AddNiagaraSystemFeatureSet(UNiagaraSystem* System, TSet<FString>& Out)
{
    if (!System) return;
    TArray<FNiagaraVariable> Params;
    System->GetExposedParameters().GetParameters(Params);
    for (const FNiagaraVariable& Var : Params)
    {
        Out.Add(FString::Printf(TEXT("param:%s"), *Var.GetName().ToString().ToLower()));
        Out.Add(FString::Printf(TEXT("type:%s"), *TypeDefToString(Var.GetType()).ToLower()));
    }

    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        Out.Add(FString::Printf(TEXT("emitter:%s"), *Handle.GetName().ToString().ToLower()));
        FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
        if (!Data) continue;
        for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
        {
            if (Renderer) Out.Add(FString::Printf(TEXT("renderer:%s"), *Renderer->GetClass()->GetName().ToLower()));
        }
        UNiagaraScriptSource* Source = GetEmitterScriptSource(Data);
        if (Source && Source->NodeGraph)
        {
            for (UNiagaraNodeFunctionCall* Node : CollectFunctionNodes(Source->NodeGraph))
            {
                if (Node) Out.Add(FString::Printf(TEXT("module:%s"), *Node->GetFunctionName().ToLower()));
            }
        }
        TArray<UNiagaraScript*> Scripts;
        Data->GetScripts(Scripts, /*bCompilableOnly=*/false, /*bEnabledOnly=*/false);
        for (UNiagaraScript* Script : Scripts)
        {
            if (!Script) continue;
            for (const FNiagaraScriptDataInterfaceInfo& Info : Script->GetCachedDefaultDataInterfaces())
            {
                Out.Add(FString::Printf(TEXT("di:%s"), *Info.Name.ToString().ToLower()));
                Out.Add(FString::Printf(TEXT("di_type:%s"), *TypeDefToString(Info.Type).ToLower()));
            }
        }
    }
}

struct FCustomHlslNodeSelection
{
    UNiagaraNodeFunctionCall* FunctionNode = nullptr;
    UEdGraphNode* Node = nullptr;
    FProperty* TextProperty = nullptr;
    int32 Index = INDEX_NONE;
};

bool IsCustomHlslGraphNode(const UEdGraphNode* Node)
{
    return Node && Node->GetClass()->GetName().Contains(TEXT("CustomHlsl"), ESearchCase::IgnoreCase);
}

FProperty* FindCustomHlslTextProperty(UObject* Node)
{
    if (!Node) return nullptr;
    static const TCHAR* CandidateNames[] = {
        TEXT("CustomHlsl"),
        TEXT("CustomHlslText"),
        TEXT("HlslText"),
        TEXT("Hlsl"),
        TEXT("Code")
    };
    for (const TCHAR* Name : CandidateNames)
    {
        if (FProperty* Prop = FindFProperty<FProperty>(Node->GetClass(), Name))
        {
            return Prop;
        }
    }
    for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
    {
        FProperty* Prop = *It;
        if (!Prop) continue;
        const FString Name = Prop->GetName();
        if (Name.Contains(TEXT("Hlsl"), ESearchCase::IgnoreCase)
            && (CastField<FStrProperty>(Prop) || CastField<FTextProperty>(Prop)))
        {
            return Prop;
        }
    }
    return nullptr;
}

TSharedPtr<FJsonObject> CustomHlslNodeToJson(const FCustomHlslNodeSelection& Selection)
{
    auto Row = MakeShared<FJsonObject>();
    Row->SetNumberField(TEXT("index"), Selection.Index);
    Row->SetStringField(TEXT("node_id"), Selection.Node ? GuidToString(Selection.Node->NodeGuid) : FString());
    Row->SetStringField(TEXT("class"), Selection.Node ? Selection.Node->GetClass()->GetPathName() : FString());
    Row->SetStringField(TEXT("title"), Selection.Node ? Selection.Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
    Row->SetStringField(TEXT("text_property"), Selection.TextProperty ? Selection.TextProperty->GetName() : FString());
    if (Selection.Node && Selection.TextProperty)
    {
        Row->SetField(TEXT("hlsl"), detail::GetUPropertyAsJson(Selection.Node, Selection.TextProperty));
    }
    return Row;
}

TArray<FCustomHlslNodeSelection> CollectCustomHlslNodes(UNiagaraGraph* Graph)
{
    TArray<FCustomHlslNodeSelection> Nodes;
    if (!Graph) return Nodes;
    int32 Index = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!IsCustomHlslGraphNode(Node)) continue;
        FCustomHlslNodeSelection Selection;
        Selection.Node = Node;
        Selection.TextProperty = FindCustomHlslTextProperty(Node);
        Selection.Index = Index++;
        Nodes.Add(Selection);
    }
    Nodes.Sort([](const FCustomHlslNodeSelection& A, const FCustomHlslNodeSelection& B)
    {
        if (!A.Node || !B.Node) return A.Node != nullptr;
        if (A.Node->NodePosY == B.Node->NodePosY) return A.Node->NodePosX < B.Node->NodePosX;
        return A.Node->NodePosY < B.Node->NodePosY;
    });
    for (int32 I = 0; I < Nodes.Num(); ++I)
    {
        Nodes[I].Index = I;
    }
    return Nodes;
}

bool SelectCustomHlslNode(const TSharedPtr<FJsonObject>& Args,
                          const TArray<FCustomHlslNodeSelection>& Nodes,
                          FCustomHlslNodeSelection& Out,
                          FString& OutError)
{
    int32 Index = INDEX_NONE;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("custom_hlsl_index"), Index);
        if (Index == INDEX_NONE) Args->TryGetNumberField(TEXT("index"), Index);
    }
    if (Index != INDEX_NONE)
    {
        if (!Nodes.IsValidIndex(Index))
        {
            OutError = FString::Printf(TEXT("custom HLSL node index out of range: %d"), Index);
            return false;
        }
        Out = Nodes[Index];
        return true;
    }

    const FString NodeId = FirstNiagaraStringArg(Args, {TEXT("node_id"), TEXT("id")});
    FGuid Guid;
    const bool bGuid = !NodeId.IsEmpty() && ParseGuidString(NodeId, Guid);
    if (bGuid)
    {
        for (const FCustomHlslNodeSelection& Node : Nodes)
        {
            if (Node.Node && Node.Node->NodeGuid == Guid)
            {
                Out = Node;
                return true;
            }
        }
        OutError = FString::Printf(TEXT("custom HLSL node not found: %s"), *NodeId);
        return false;
    }

    if (Nodes.Num() == 1)
    {
        Out = Nodes[0];
        return true;
    }
    OutError = Nodes.Num() == 0
        ? TEXT("no CustomHlsl nodes found in selected Niagara emitter graph")
        : TEXT("multiple CustomHlsl nodes found; pass index/custom_hlsl_index or node_id");
    return false;
}

FOutcome GetCustomHlslTextImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<FCustomHlslNodeSelection> Nodes = CollectCustomHlslNodes(Source->NodeGraph);
    const bool bListAll = !Args.IsValid()
        || (!Args->HasField(TEXT("index")) && !Args->HasField(TEXT("custom_hlsl_index")) && !Args->HasField(TEXT("node_id")));

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FCustomHlslNodeSelection& Node : Nodes)
    {
        Rows.Add(MakeShared<FJsonValueObject>(CustomHlslNodeToJson(Node)));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    R->SetArrayField(TEXT("nodes"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    if (!bListAll)
    {
        FString Error;
        FCustomHlslNodeSelection Selected;
        if (!SelectCustomHlslNode(Args, Nodes, Selected, Error))
        {
            return FOutcome::MakeError(-32602, Error);
        }
        R->SetObjectField(TEXT("node"), CustomHlslNodeToJson(Selected));
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome SetCustomHlslTextImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Hlsl = FirstNiagaraStringArg(Args, {TEXT("hlsl"), TEXT("text"), TEXT("code")});
    if (Hlsl.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'hlsl'/'text'/'code'"));
    }

    FResolvedEmitterTarget Target;
    FOutcome Resolved = ResolveEmitterTarget(Args, Target);
    if (!Resolved.bSuccess) return Resolved;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Target.Data);
    if (!Source || !Source->NodeGraph) return FOutcome::MakeError(-32603, TEXT("selected emitter has no graph"));

    TArray<FCustomHlslNodeSelection> Nodes = CollectCustomHlslNodes(Source->NodeGraph);
    FString Error;
    FCustomHlslNodeSelection Selected;
    if (!SelectCustomHlslNode(Args, Nodes, Selected, Error))
    {
        return FOutcome::MakeError(-32602, Error);
    }
    if (!Selected.Node || !Selected.TextProperty)
    {
        return FOutcome::MakeError(-32602,
            TEXT("selected CustomHlsl node has no reflected editable HLSL text property"));
    }

    bool bDryRun = false, bSave = false, bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("emitter"), EmitterTargetSummary(Target));
    R->SetObjectField(TEXT("before"), CustomHlslNodeToJson(Selected));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("SetNiagaraCustomHlslText", "Sage: Set Niagara Custom HLSL Text"));
        Source->NodeGraph->Modify();
        Selected.Node->Modify();
        const bool bSet = detail::SetUPropertyFromJson(
            Selected.Node,
            Selected.TextProperty,
            MakeShared<FJsonValueString>(Hlsl));
        if (!bSet)
        {
            Tx.Cancel();
            return FOutcome::MakeError(-32602, TEXT("failed to set CustomHlsl text property"));
        }
        Source->NodeGraph->NotifyGraphChanged();
        Target.RootAsset->MarkPackageDirty();
    }
    R->SetObjectField(TEXT("after"), CustomHlslNodeToJson(Selected));
    if (Target.System)
    {
        CompileIfRequested(Target.System, bCompile && !bDryRun, R);
        SaveLoadedAssetIfRequested(Target.System, bSave && !bDryRun, R);
    }
    else
    {
        SaveLoadedAssetIfRequested(Target.MutableEmitter, bSave && !bDryRun, R);
    }
    return FOutcome::MakeSuccess(R);
}

FOutcome SearchByParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Query = FirstNiagaraStringArg(Args, {TEXT("parameter"), TEXT("name"), TEXT("query")});
    if (Query.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing parameter/name/query"));
    const FString SearchPath = FirstNiagaraStringArg(Args, {TEXT("path"), TEXT("folder"), TEXT("directory")}, TEXT("/Game"));
    const int32 MaxResults = NiagaraMaxResultsArg(Args, 100);

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FNiagaraAssetRow& Asset : CollectNiagaraAssetRows(SearchPath))
    {
        UObject* Obj = LoadAnyAsset(Asset.Asset.GetSoftObjectPath().ToString());
        if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj))
        {
            TArray<FNiagaraVariable> Params;
            System->GetExposedParameters().GetParameters(Params);
            AddParameterMatchRows(System->GetPathName(), Asset.Type, TEXT(""), TEXT("system_exposed"),
                Params, &System->GetExposedParameters(), Query, Rows, MaxResults);
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                AddEmitterParameterMatches(System->GetPathName(), Asset.Type, Handle.GetName().ToString(),
                    Handle.GetEmitterData(), Query, Rows, MaxResults);
                if (Rows.Num() >= MaxResults) break;
            }
        }
        else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Obj))
        {
            AddEmitterParameterMatches(Emitter->GetPathName(), Asset.Type, Emitter->GetName(),
                Emitter->GetLatestEmitterData(), Query, Rows, MaxResults);
        }
        if (Rows.Num() >= MaxResults) break;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"), Query);
    R->SetStringField(TEXT("path"), SearchPath);
    R->SetArrayField(TEXT("matches"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetBoolField(TEXT("capped"), Rows.Num() >= MaxResults);
    return FOutcome::MakeSuccess(R);
}

FOutcome SearchByDataInterfaceImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Query = FirstNiagaraStringArg(Args, {TEXT("data_interface"), TEXT("type"), TEXT("class"), TEXT("query"), TEXT("name")});
    if (Query.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing data_interface/type/class/query"));
    const FString SearchPath = FirstNiagaraStringArg(Args, {TEXT("path"), TEXT("folder"), TEXT("directory")}, TEXT("/Game"));
    const int32 MaxResults = NiagaraMaxResultsArg(Args, 100);

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FNiagaraAssetRow& Asset : CollectNiagaraAssetRows(SearchPath))
    {
        UObject* Obj = LoadAnyAsset(Asset.Asset.GetSoftObjectPath().ToString());
        if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj))
        {
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                AddDataInterfaceMatchRows(System->GetPathName(), Asset.Type, Handle.GetName().ToString(),
                    Handle.GetEmitterData(), Query, Rows, MaxResults);
                if (Rows.Num() >= MaxResults) break;
            }
        }
        else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Obj))
        {
            AddDataInterfaceMatchRows(Emitter->GetPathName(), Asset.Type, Emitter->GetName(),
                Emitter->GetLatestEmitterData(), Query, Rows, MaxResults);
        }
        if (Rows.Num() >= MaxResults) break;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"), Query);
    R->SetStringField(TEXT("path"), SearchPath);
    R->SetArrayField(TEXT("matches"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetBoolField(TEXT("capped"), Rows.Num() >= MaxResults);
    return FOutcome::MakeSuccess(R);
}

FOutcome SearchByMaterialImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Query = FirstNiagaraStringArg(Args, {TEXT("material"), TEXT("path"), TEXT("query"), TEXT("name")});
    if (Query.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing material/path/query"));
    const FString SearchPath = FirstNiagaraStringArg(Args, {TEXT("search_path"), TEXT("folder"), TEXT("directory")}, TEXT("/Game"));
    const int32 MaxResults = NiagaraMaxResultsArg(Args, 100);

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FNiagaraAssetRow& Asset : CollectNiagaraAssetRows(SearchPath))
    {
        UObject* Obj = LoadAnyAsset(Asset.Asset.GetSoftObjectPath().ToString());
        if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj))
        {
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                AddRendererMaterialMatchRows(System->GetPathName(), Asset.Type, Handle.GetName().ToString(),
                    Handle.GetEmitterData(), Query, Rows, MaxResults);
                if (Rows.Num() >= MaxResults) break;
            }
        }
        else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Obj))
        {
            AddRendererMaterialMatchRows(Emitter->GetPathName(), Asset.Type, Emitter->GetName(),
                Emitter->GetLatestEmitterData(), Query, Rows, MaxResults);
        }
        if (Rows.Num() >= MaxResults) break;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"), Query);
    R->SetStringField(TEXT("path"), SearchPath);
    R->SetArrayField(TEXT("matches"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetBoolField(TEXT("capped"), Rows.Num() >= MaxResults);
    return FOutcome::MakeSuccess(R);
}

FOutcome QueryNiagaraImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Query = FirstNiagaraStringArg(Args, {TEXT("query"), TEXT("q"), TEXT("name")});
    const FString SearchPath = FirstNiagaraStringArg(Args, {TEXT("path"), TEXT("folder"), TEXT("directory")}, TEXT("/Game"));
    const int32 MaxResults = NiagaraMaxResultsArg(Args, 100);

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FNiagaraAssetRow& Asset : CollectNiagaraAssetRows(SearchPath))
    {
        const FString AssetPath = Asset.Asset.GetSoftObjectPath().ToString();
        if (Query.IsEmpty()
            || TextContainsInsensitive(Asset.Asset.AssetName.ToString(), Query)
            || TextContainsInsensitive(AssetPath, Query)
            || TextContainsInsensitive(Asset.Type, Query))
        {
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("match_type"), TEXT("asset"));
            Row->SetStringField(TEXT("name"), Asset.Asset.AssetName.ToString());
            Row->SetStringField(TEXT("path"), AssetPath);
            Row->SetStringField(TEXT("asset_type"), Asset.Type);
            if (!AddNiagaraSearchRow(Rows, MaxResults, Row)) break;
        }

        UObject* Obj = LoadAnyAsset(AssetPath);
        if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj))
        {
            TArray<FNiagaraVariable> Params;
            System->GetExposedParameters().GetParameters(Params);
            AddParameterMatchRows(System->GetPathName(), Asset.Type, TEXT(""), TEXT("system_exposed"),
                Params, &System->GetExposedParameters(), Query, Rows, MaxResults);
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                AddEmitterParameterMatches(System->GetPathName(), Asset.Type, Handle.GetName().ToString(),
                    Handle.GetEmitterData(), Query, Rows, MaxResults);
                AddDataInterfaceMatchRows(System->GetPathName(), Asset.Type, Handle.GetName().ToString(),
                    Handle.GetEmitterData(), Query, Rows, MaxResults);
                AddRendererMaterialMatchRows(System->GetPathName(), Asset.Type, Handle.GetName().ToString(),
                    Handle.GetEmitterData(), Query, Rows, MaxResults);
                if (Rows.Num() >= MaxResults) break;
            }
        }
        else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Obj))
        {
            AddEmitterParameterMatches(Emitter->GetPathName(), Asset.Type, Emitter->GetName(),
                Emitter->GetLatestEmitterData(), Query, Rows, MaxResults);
            AddDataInterfaceMatchRows(Emitter->GetPathName(), Asset.Type, Emitter->GetName(),
                Emitter->GetLatestEmitterData(), Query, Rows, MaxResults);
            AddRendererMaterialMatchRows(Emitter->GetPathName(), Asset.Type, Emitter->GetName(),
                Emitter->GetLatestEmitterData(), Query, Rows, MaxResults);
        }
        if (Rows.Num() >= MaxResults) break;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"), Query);
    R->SetStringField(TEXT("path"), SearchPath);
    R->SetArrayField(TEXT("matches"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetBoolField(TEXT("capped"), Rows.Num() >= MaxResults);
    return FOutcome::MakeSuccess(R);
}

FOutcome FindSimilarSystemsImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstNiagaraStringArg(Args, {TEXT("path"), TEXT("system"), TEXT("asset")});
    if (Path.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'path'/'system'"));
    UNiagaraSystem* Base = LoadTypedAsset<UNiagaraSystem>(Path);
    if (!Base) return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system"));

    const FString SearchPath = FirstNiagaraStringArg(Args, {TEXT("search_path"), TEXT("folder"), TEXT("directory")}, TEXT("/Game"));
    const int32 MaxResults = NiagaraMaxResultsArg(Args, 25);
    TSet<FString> BaseFeatures;
    AddNiagaraSystemFeatureSet(Base, BaseFeatures);

    struct FSimilarity
    {
        double Score = 0.0;
        int32 Overlap = 0;
        int32 UnionCount = 0;
        TSharedPtr<FJsonObject> Row;
    };
    TArray<FSimilarity> Similarities;
    for (const FAssetData& Asset : ListNiagaraAssets(TEXT("/Script/Niagara.NiagaraSystem"), SearchPath))
    {
        UNiagaraSystem* Candidate = LoadTypedAsset<UNiagaraSystem>(Asset.GetSoftObjectPath().ToString());
        if (!Candidate || Candidate == Base) continue;
        TSet<FString> Features;
        AddNiagaraSystemFeatureSet(Candidate, Features);
        int32 Overlap = 0;
        for (const FString& Feature : BaseFeatures)
        {
            if (Features.Contains(Feature)) ++Overlap;
        }
        int32 UnionCount = BaseFeatures.Num();
        for (const FString& Feature : Features)
        {
            if (!BaseFeatures.Contains(Feature)) ++UnionCount;
        }
        const double Score = UnionCount > 0 ? static_cast<double>(Overlap) / static_cast<double>(UnionCount) : 0.0;
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), Candidate->GetPathName());
        Row->SetStringField(TEXT("name"), Candidate->GetName());
        Row->SetNumberField(TEXT("score"), Score);
        Row->SetNumberField(TEXT("overlap"), Overlap);
        Row->SetNumberField(TEXT("union"), UnionCount);
        Row->SetNumberField(TEXT("feature_count"), Features.Num());
        Similarities.Add({Score, Overlap, UnionCount, Row});
    }
    Similarities.Sort([](const FSimilarity& A, const FSimilarity& B)
    {
        if (FMath::IsNearlyEqual(A.Score, B.Score)) return A.Overlap > B.Overlap;
        return A.Score > B.Score;
    });

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FSimilarity& Similarity : Similarities)
    {
        if (Rows.Num() >= MaxResults) break;
        Rows.Add(MakeShared<FJsonValueObject>(Similarity.Row));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("base"), Base->GetPathName());
    R->SetStringField(TEXT("search_path"), SearchPath);
    R->SetNumberField(TEXT("base_feature_count"), BaseFeatures.Num());
    R->SetArrayField(TEXT("systems"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    return FOutcome::MakeSuccess(R);
}

FOutcome FindNiagaraReferencesImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (Args.IsValid() && Args->HasField(TEXT("path")))
    {
        return NiagaraFindReferencersImpl(Args);
    }
    const FString Asset = FirstNiagaraStringArg(Args, {TEXT("asset"), TEXT("system"), TEXT("emitter")});
    if (Asset.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'path'/'asset'"));
    TSharedPtr<FJsonObject> Effective = MakeShared<FJsonObject>();
    if (Args.IsValid()) Effective->Values = Args->Values;
    Effective->SetStringField(TEXT("path"), Asset);
    return NiagaraFindReferencersImpl(Effective);
}

FOutcome ListSystemDataInterfacesImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstNiagaraStringArg(Args, {TEXT("path"), TEXT("system"), TEXT("asset")});
    if (Path.IsEmpty()) return FOutcome::MakeError(-32602, TEXT("missing 'path'/'system'"));
    const int32 MaxResults = NiagaraMaxResultsArg(Args, 500);

    TArray<TSharedPtr<FJsonValue>> Rows;
    UObject* Obj = LoadAnyAsset(Path);
    if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Obj))
    {
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            AddDataInterfaceMatchRows(System->GetPathName(), TEXT("NiagaraSystem"), Handle.GetName().ToString(),
                Handle.GetEmitterData(), FString(), Rows, MaxResults);
            if (Rows.Num() >= MaxResults) break;
        }
    }
    else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Obj))
    {
        AddDataInterfaceMatchRows(Emitter->GetPathName(), TEXT("NiagaraEmitter"), Emitter->GetName(),
            Emitter->GetLatestEmitterData(), FString(), Rows, MaxResults);
    }
    else
    {
        return FOutcome::MakeError(-32602, TEXT("'path' is not a Niagara system or emitter"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetArrayField(TEXT("data_interfaces"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetBoolField(TEXT("capped"), Rows.Num() >= MaxResults);
    return FOutcome::MakeSuccess(R);
}

// ---- batch -----------------------------------------------------------------

FOutcome DispatchNiagaraLocal(const FString& Name, const TSharedPtr<FJsonObject>& Args)
{
    if (Name == TEXT("niagara.list_emitters")) return NiagaraListEmittersImpl(Args);
    if (Name == TEXT("niagara.list_renderers")) return NiagaraListRenderersImpl(Args);
    if (Name == TEXT("niagara.set_renderer_property")) return NiagaraSetRendererPropertyImpl(Args);
    if (Name == TEXT("niagara.add_renderer")) return NiagaraAddRendererImpl(Args);
    if (Name == TEXT("niagara.remove_renderer")) return NiagaraRemoveRendererImpl(Args);
    if (Name == TEXT("niagara.list_modules")) return NiagaraListModulesImpl(Args);
    if (Name == TEXT("niagara.list_module_inputs")) return NiagaraListModuleInputsImpl(Args);
    if (Name == TEXT("niagara.set_module_input")) return NiagaraSetModuleInputImpl(Args);
    if (Name == TEXT("niagara.upsert_user_parameter")) return NiagaraUpsertUserParameterImpl(Args);
    if (Name == TEXT("niagara.set_fixed_bounds")) return NiagaraSetFixedBoundsImpl(Args);
    return FOutcome::MakeError(-32602, FString::Printf(TEXT("niagara.batch does not support nested or unknown call: %s"), *Name));
}

FOutcome NiagaraBatchImpl(const TSharedPtr<FJsonObject>& Args)
{
    FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    const TArray<TSharedPtr<FJsonValue>>* Calls = nullptr;
    if (!Args.IsValid() || !Args->TryGetArrayField(TEXT("calls"), Calls) || !Calls)
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'calls' array"));
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    FScopedTransaction Tx(LOCTEXT("NiagaraBatch", "Sage: Niagara Batch"));
    for (int32 Index = 0; Index < Calls->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& CallValue = (*Calls)[Index];
        const TSharedPtr<FJsonObject>* CallObj = nullptr;
        if (!CallValue->TryGetObject(CallObj) || !CallObj || !CallObj->IsValid())
        {
            return FOutcome::MakeError(-32602, FString::Printf(TEXT("call %d is not an object"), Index));
        }
        FString Name;
        (*CallObj)->TryGetStringField(TEXT("name"), Name);
        const TSharedPtr<FJsonObject>* CallArgs = nullptr;
        TSharedPtr<FJsonObject> EmptyArgs = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> EffectiveArgs = EmptyArgs;
        if ((*CallObj)->TryGetObjectField(TEXT("arguments"), CallArgs) && CallArgs && CallArgs->IsValid())
        {
            EffectiveArgs = *CallArgs;
        }
        FOutcome Result = DispatchNiagaraLocal(Name, EffectiveArgs);
        auto Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), Index);
        Row->SetStringField(TEXT("name"), Name);
        Row->SetBoolField(TEXT("success"), Result.bSuccess);
        if (Result.bSuccess && Result.Result.IsValid()) Row->SetObjectField(TEXT("result"), Result.Result);
        else Row->SetStringField(TEXT("error"), OutcomeErrorString(Result));
        Results.Add(MakeShared<FJsonValueObject>(Row));
        if (!Result.bSuccess)
        {
            auto R = MakeShared<FJsonObject>();
            R->SetArrayField(TEXT("results"), Results);
            R->SetNumberField(TEXT("failed_index"), Index);
            return FOutcome::MakeSuccess(R);
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("count"), Results.Num());
    return FOutcome::MakeSuccess(R);
}

}  // namespace

void RegisterNiagaraTools(FSageToolDispatch& Dispatch)
{
    auto GT = [](FOutcome (*Fn)(const TSharedPtr<FJsonObject>&))
    {
        return [Fn](const TSharedPtr<FJsonObject>& Args) -> FOutcome
        {
            return detail::RunOnGameThread([&]() -> FOutcome
            {
                return Fn(Args);
            });
        };
    };

    Dispatch.RegisterHandler(TEXT("niagara.list"), GT(&NiagaraListImpl));
    Dispatch.RegisterHandler(TEXT("niagara.get_info"), GT(&NiagaraGetInfoImpl));
    Dispatch.RegisterHandler(TEXT("niagara.spawn"), GT(&NiagaraSpawnImpl));
    Dispatch.RegisterHandler(TEXT("niagara.preview_spawn"), GT(&NiagaraPreviewSpawnImpl));
    Dispatch.RegisterHandler(TEXT("niagara.cleanup_preview"), GT(&NiagaraCleanupPreviewImpl));
    Dispatch.RegisterHandler(TEXT("niagara.capture_preview"), GT(&NiagaraCapturePreviewImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_parameter"), GT(&NiagaraSetParameterImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_component_parameters"), GT(&NiagaraSetComponentParametersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.read_component"), GT(&NiagaraReadComponentImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create"), GT(&NiagaraCreateImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create_emitter"), GT(&NiagaraCreateEmitterImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create_system_from_spec"), GT(&NiagaraCreateSystemFromSpecImpl));
    Dispatch.RegisterHandler(TEXT("niagara.add_emitter"), GT(&NiagaraAddEmitterImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_emitters"), GT(&NiagaraListEmittersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_emitter_property"), GT(&NiagaraSetEmitterPropertyImpl));
    Dispatch.RegisterHandler(TEXT("niagara.make_emitters_local"), GT(&NiagaraMakeEmittersLocalImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_modules"), GT(&NiagaraListModulesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.get_emitter_info"), GT(&NiagaraGetEmitterInfoImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_module_inputs"), GT(&NiagaraListModuleInputsImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_module_input"), GT(&NiagaraSetModuleInputImpl));
    Dispatch.RegisterHandler(TEXT("niagara.add_module"), GT(&NiagaraAddModuleImpl));
    Dispatch.RegisterHandler(TEXT("niagara.remove_module"), GT(&NiagaraRemoveModuleImpl));
    Dispatch.RegisterHandler(TEXT("niagara.move_module"), GT(&NiagaraMoveModuleImpl));
    Dispatch.RegisterHandler(TEXT("niagara.duplicate_module"), GT(&NiagaraDuplicateModuleImpl));
    Dispatch.RegisterHandler(TEXT("niagara.replace_module"), GT(&NiagaraReplaceModuleImpl));
    Dispatch.RegisterHandler(TEXT("niagara.export_graph"), GT(&NiagaraExportGraphImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_dynamic_inputs"), GT(&NiagaraListDynamicInputsImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_dynamic_input"), GT(&NiagaraSetDynamicInputImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_custom_expression"), GT(&NiagaraSetCustomExpressionImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_renderers"), GT(&NiagaraListRenderersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.add_renderer"), GT(&NiagaraAddRendererImpl));
    Dispatch.RegisterHandler(TEXT("niagara.remove_renderer"), GT(&NiagaraRemoveRendererImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_renderer_property"), GT(&NiagaraSetRendererPropertyImpl));
    Dispatch.RegisterHandler(TEXT("niagara.inspect_data_interfaces"), GT(&NiagaraInspectDataInterfacesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.get_compiled_hlsl"), GT(&NiagaraGetCompiledHlslImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_system_parameters"), GT(&NiagaraListSystemParametersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.upsert_user_parameter"), GT(&NiagaraUpsertUserParameterImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_static_switches"), GT(&NiagaraListStaticSwitchesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_static_switch"), GT(&NiagaraSetStaticSwitchImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create_module_from_hlsl"), GT(&NiagaraCreateModuleFromHlslImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create_scratch_module"), GT(&NiagaraCreateScratchModuleImpl));
    Dispatch.RegisterHandler(TEXT("niagara.collection.list"), GT(&NiagaraCollectionListImpl));
    Dispatch.RegisterHandler(TEXT("niagara.collection.read"), GT(&NiagaraCollectionReadImpl));
    Dispatch.RegisterHandler(TEXT("niagara.collection.create"), GT(&NiagaraCollectionCreateImpl));
    Dispatch.RegisterHandler(TEXT("niagara.collection.upsert_parameter"), GT(&NiagaraCollectionUpsertParameterImpl));
    Dispatch.RegisterHandler(TEXT("niagara.collection.set_default"), GT(&NiagaraCollectionSetDefaultImpl));
    Dispatch.RegisterHandler(TEXT("niagara.collection.set_runtime_value"), GT(&NiagaraCollectionSetRuntimeValueImpl));
    Dispatch.RegisterHandler(TEXT("niagara.read_scalability"), GT(&NiagaraReadScalabilityImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_scalability"), GT(&NiagaraSetScalabilityImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_fixed_bounds"), GT(&NiagaraSetFixedBoundsImpl));
    Dispatch.RegisterHandler(TEXT("niagara.clear_fixed_bounds"), GT(&NiagaraClearFixedBoundsImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_warmup"), GT(&NiagaraSetWarmupImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_effect_type"), GT(&NiagaraSetEffectTypeImpl));
    Dispatch.RegisterHandler(TEXT("niagara.validate_system"), GT(&NiagaraValidateSystemImpl));
    Dispatch.RegisterHandler(TEXT("niagara.capture_sim_cache"), GT(&NiagaraCaptureSimCacheImpl));
    Dispatch.RegisterHandler(TEXT("niagara.read_sim_cache"), GT(&NiagaraReadSimCacheImpl));
    Dispatch.RegisterHandler(TEXT("niagara.compare_sim_cache"), GT(&NiagaraCompareSimCacheImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_event_handlers"), GT(&NiagaraListEventHandlersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.add_event_handler"), GT(&NiagaraAddEventHandlerImpl));
    Dispatch.RegisterHandler(TEXT("niagara.remove_event_handler"), GT(&NiagaraRemoveEventHandlerImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_simulation_stages"), GT(&NiagaraListSimulationStagesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.add_simulation_stage"), GT(&NiagaraAddSimulationStageImpl));
    Dispatch.RegisterHandler(TEXT("niagara.remove_simulation_stage"), GT(&NiagaraRemoveSimulationStageImpl));
    Dispatch.RegisterHandler(TEXT("niagara.get_dependencies"), GT(&NiagaraGetDependenciesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.find_referencers"), GT(&NiagaraFindReferencersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.validate_dependencies"), GT(&NiagaraValidateDependenciesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.audit_cross_asset_refs"), GT(&NiagaraAuditCrossAssetRefsImpl));
    Dispatch.RegisterHandler(TEXT("niagara.batch"), GT(&NiagaraBatchImpl));
    Dispatch.RegisterHandler(TEXT("get_custom_hlsl_text"), GT(&GetCustomHlslTextImpl));
    Dispatch.RegisterHandler(TEXT("set_custom_hlsl_text"), GT(&SetCustomHlslTextImpl));
    Dispatch.RegisterHandler(TEXT("search_by_parameter"), GT(&SearchByParameterImpl));
    Dispatch.RegisterHandler(TEXT("search_by_data_interface"), GT(&SearchByDataInterfaceImpl));
    Dispatch.RegisterHandler(TEXT("search_by_material"), GT(&SearchByMaterialImpl));
    Dispatch.RegisterHandler(TEXT("query_niagara"), GT(&QueryNiagaraImpl));
    Dispatch.RegisterHandler(TEXT("find_similar_systems"), GT(&FindSimilarSystemsImpl));
    Dispatch.RegisterHandler(TEXT("find_niagara_references"), GT(&FindNiagaraReferencesImpl));
    Dispatch.RegisterHandler(TEXT("list_system_data_interfaces"), GT(&ListSystemDataInterfacesImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
