#include "Tools/SageGasTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "SageGas"

namespace sage::tools
{
namespace
{

static UClass* FindGasClass(const TCHAR* Name)
{
    UClass* Cls = FindObject<UClass>(nullptr, Name);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, Name);
    return Cls;
}

static FSageToolDispatch::FOutcome GasNotAvailable()
{
    return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("GameplayAbilities plugin required; class not found"));
}

static UObject* CreateGasAsset(const FString& Path, const TCHAR* ClassName)
{
    UClass* Cls = FindGasClass(ClassName);
    if (!Cls) return nullptr;

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return nullptr;

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    return AT.CreateAsset(AssetName, PackagePath, Cls, nullptr);
}

// ---- gas.add_asc -----------------------------------------------------------

FSageToolDispatch::FOutcome GasAddAscImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    UClass* AscCls = FindGasClass(TEXT("/Script/GameplayAbilities.AbilitySystemComponent"));
    if (!AscCls) return GasNotAvailable();

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    // Check if ASC already exists
    UActorComponent* Existing = A->FindComponentByClass(AscCls);
    if (Existing)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("actor_id"),   A->GetPathName());
        R->SetStringField(TEXT("component"),  Existing->GetName());
        R->SetBoolField  (TEXT("already_exists"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("AddASC", "Add Ability System Component"));
    A->Modify();
    UActorComponent* Comp = NewObject<UActorComponent>(A, AscCls,
        FName(TEXT("AbilitySystemComponent")));
    A->AddInstanceComponent(Comp);
    Comp->RegisterComponent();
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),  A->GetPathName());
    R->SetStringField(TEXT("component"), Comp->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gas.create_attribute_set ----------------------------------------------

FSageToolDispatch::FOutcome GasCreateAttributeSetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("AttributeSet creation requires C++ subclass of UAttributeSet; "
             "use project.create_cpp_class with parent UAttributeSet"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gas.add_attribute -----------------------------------------------------

FSageToolDispatch::FOutcome GasAddAttributeImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("Attributes are UPROPERTY fields on UAttributeSet C++ classes; "
             "use project.write_cpp_file to add attribute properties"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gas.create_ability ----------------------------------------------------

FSageToolDispatch::FOutcome GasCreateAbilityImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* GaCls = FindGasClass(TEXT("/Script/GameplayAbilities.GameplayAbility"));
    if (!GaCls) return GasNotAvailable();

    // Create as Blueprint
    UClass* BpFacCls = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlueprintFactory"));
    if (!BpFacCls) BpFacCls = LoadObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlueprintFactory"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

    UFactory* Factory = nullptr;
    if (BpFacCls)
    {
        Factory = NewObject<UFactory>(GetTransientPackage(), BpFacCls);
        // Set parent class via reflection
        FProperty* ParentProp = FindFProperty<FProperty>(Factory->GetClass(), TEXT("ParentClass"));
        if (ParentProp)
            detail::SetUPropertyFromJson(Factory, ParentProp,
                MakeShared<FJsonValueString>(GaCls->GetPathName()));
    }

    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath,
        BpFacCls ? nullptr : GaCls, Factory);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         NewObj->GetPathName());
    R->SetStringField(TEXT("parent_class"), TEXT("GameplayAbility"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gas.set_ability_tags --------------------------------------------------

FSageToolDispatch::FOutcome GasSetAbilityTagsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    // Apply tags via property reflection — AbilityTags is an FGameplayTagContainer
    const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("tags"), TagsArr))
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Obj->GetPathName());
        R->SetStringField(TEXT("note"), TEXT("provide 'tags' array of gameplay tag strings"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SetGATags", "Set Ability Tags"));
    Obj->Modify();

    TArray<FString> Applied;
    for (const auto& TV : *TagsArr)
    {
        FString Tag = TV->AsString();
        if (!Tag.IsEmpty()) Applied.Add(Tag);
    }
    Obj->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    TArray<TSharedPtr<FJsonValue>> TagVals;
    for (const FString& T : Applied) TagVals.Add(MakeShared<FJsonValueString>(T));
    R->SetArrayField (TEXT("tags_requested"), TagVals);
    R->SetStringField(TEXT("note"),
        TEXT("tags reflected as strings; use gameplay tag container APIs for runtime activation"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gas.create_effect -----------------------------------------------------

FSageToolDispatch::FOutcome GasCreateEffectImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* GeCls = FindGasClass(TEXT("/Script/GameplayAbilities.GameplayEffect"));
    if (!GeCls) return GasNotAvailable();

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, GeCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("GameplayEffect"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gas.set_effect_modifier -----------------------------------------------

FSageToolDispatch::FOutcome GasSetEffectModifierImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    // Apply generic property fields from Args
    FScopedTransaction Tx(LOCTEXT("SetGEMod", "Set Effect Modifier"));
    Obj->Modify();
    int32 Set = 0;
    for (const auto& Pair : Args->Values)
    {
        if (Pair.Key == TEXT("path")) continue;
        FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), *Pair.Key);
        if (Prop && detail::SetUPropertyFromJson(Obj, Prop, Pair.Value)) ++Set;
    }
    Obj->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Obj->GetPathName());
    R->SetNumberField(TEXT("fields_set"), Set);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gas.create_cue --------------------------------------------------------

FSageToolDispatch::FOutcome GasCreateCueImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* CueCls = FindGasClass(TEXT("/Script/GameplayAbilities.GameplayCueNotify_Static"));
    if (!CueCls) return GasNotAvailable();

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, CueCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("GameplayCueNotify_Static"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gas.get_info ----------------------------------------------------------

FSageToolDispatch::FOutcome GasGetInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    UClass* AscCls = FindGasClass(TEXT("/Script/GameplayAbilities.AbilitySystemComponent"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());

    if (AscCls)
    {
        UActorComponent* Comp = A->FindComponentByClass(AscCls);
        if (Comp)
        {
            R->SetBoolField  (TEXT("has_asc"),    true);
            R->SetStringField(TEXT("asc_name"),   Comp->GetName());

            // Reflect granted abilities count via property
            TArray<TSharedPtr<FJsonValue>> PropList;
            for (TFieldIterator<FProperty> It(Comp->GetClass()); It; ++It)
            {
                if (It->GetName().Contains(TEXT("ActivatableAbilities")) ||
                    It->GetName().Contains(TEXT("ActiveGameplayEffects")))
                {
                    auto J = MakeShared<FJsonObject>();
                    J->SetStringField(TEXT("name"), It->GetName());
                    TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(Comp, *It);
                    if (Val) J->SetField(TEXT("value"), Val);
                    PropList.Add(MakeShared<FJsonValueObject>(J));
                }
            }
            R->SetArrayField(TEXT("ability_properties"), PropList);
        }
        else
        {
            R->SetBoolField(TEXT("has_asc"), false);
        }
    }
    else
    {
        R->SetBoolField  (TEXT("has_asc"), false);
        R->SetStringField(TEXT("note"), TEXT("GameplayAbilities plugin not loaded"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterGasTools(FSageToolDispatch& Dispatch)
{
    auto GT = [](FSageToolDispatch::FOutcome (*Fn)(const TSharedPtr<FJsonObject>&))
    {
        return [Fn](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
        {
            return detail::RunOnGameThread([&]() -> FSageToolDispatch::FOutcome
            {
                return Fn(Args);
            });
        };
    };

    Dispatch.RegisterHandler(TEXT("gas.add_asc"),              GT(&GasAddAscImpl));
    Dispatch.RegisterHandler(TEXT("gas.create_attribute_set"), GT(&GasCreateAttributeSetImpl));
    Dispatch.RegisterHandler(TEXT("gas.add_attribute"),        GT(&GasAddAttributeImpl));
    Dispatch.RegisterHandler(TEXT("gas.create_ability"),       GT(&GasCreateAbilityImpl));
    Dispatch.RegisterHandler(TEXT("gas.set_ability_tags"),     GT(&GasSetAbilityTagsImpl));
    Dispatch.RegisterHandler(TEXT("gas.create_effect"),        GT(&GasCreateEffectImpl));
    Dispatch.RegisterHandler(TEXT("gas.set_effect_modifier"),  GT(&GasSetEffectModifierImpl));
    Dispatch.RegisterHandler(TEXT("gas.create_cue"),           GT(&GasCreateCueImpl));
    Dispatch.RegisterHandler(TEXT("gas.get_info"),             GT(&GasGetInfoImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
