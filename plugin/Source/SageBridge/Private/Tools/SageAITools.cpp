#include "Tools/SageAITools.h"

#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AIController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BrainComponent.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Hearing.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "SageAI"

namespace sage::tools
{
namespace
{

using FOutcome = FSageToolDispatch::FOutcome;

static constexpr int32 kUnsupportedCode = -32005;

struct FNamedClass
{
    const TCHAR* Name;
    const TCHAR* Path;
};

static const TSet<FString>& BehaviorGraphPrivateTools()
{
    static const TSet<FString> Names = {
        TEXT("clone_bt_subtree"),
        TEXT("auto_arrange_bt"),
        TEXT("generate_bt_diagram"),
    };
    return Names;
}

static const TSet<FString>& SmartObjectStructTools()
{
    static const TSet<FString> Names = {
        TEXT("add_so_slot"),
        TEXT("remove_so_slot"),
        TEXT("configure_so_slot"),
        TEXT("add_so_behavior_definition"),
        TEXT("remove_so_behavior_definition"),
        TEXT("gameplay.add_smart_object_slot"),
        TEXT("gameplay.set_smart_object_slot"),
        TEXT("gameplay.remove_smart_object_slot"),
        TEXT("gameplay.add_smart_object_slot_behavior"),
    };
    return Names;
}

static FString FirstStringArg(const TSharedPtr<FJsonObject>& Args,
                              std::initializer_list<const TCHAR*> Fields)
{
    if (!Args.IsValid()) return FString();
    FString Value;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetStringField(Field, Value) && !Value.IsEmpty())
        {
            return Value;
        }
    }
    return FString();
}

static int32 FirstIntArg(const TSharedPtr<FJsonObject>& Args,
                         std::initializer_list<const TCHAR*> Fields,
                         int32 DefaultValue = INDEX_NONE)
{
    if (!Args.IsValid()) return DefaultValue;
    int32 Value = DefaultValue;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetNumberField(Field, Value))
        {
            return Value;
        }
    }
    return DefaultValue;
}

static bool FirstBoolArg(const TSharedPtr<FJsonObject>& Args,
                         std::initializer_list<const TCHAR*> Fields,
                         bool DefaultValue = false)
{
    if (!Args.IsValid()) return DefaultValue;
    bool Value = DefaultValue;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetBoolField(Field, Value))
        {
            return Value;
        }
    }
    return DefaultValue;
}

static TArray<FString> StringArrayArg(const TSharedPtr<FJsonObject>& Args,
                                      const TCHAR* Field)
{
    TArray<FString> Out;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Args.IsValid() || !Args->TryGetArrayField(Field, Values) || !Values)
    {
        return Out;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        FString Text;
        if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
        {
            Out.Add(Text);
        }
    }
    return Out;
}

static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Items)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FString& Item : Items)
    {
        Out.Add(MakeShared<FJsonValueString>(Item));
    }
    return Out;
}

static FOutcome MissingArg(const TCHAR* Arg)
{
    return FOutcome::MakeError(-32602, FString::Printf(TEXT("missing '%s'"), Arg));
}

static FOutcome Unsupported(const FString& Tool, const FString& Reason)
{
    return FOutcome::MakeError(kUnsupportedCode,
        FString::Printf(TEXT("%s is not exposed as a safe public-editor mutation yet: %s"),
                        *Tool, *Reason));
}

static UWorld* GetPieWorld()
{
    if (!GEngine) return nullptr;
    for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
    {
        if (Ctx.WorldType == EWorldType::PIE)
        {
            return Ctx.World();
        }
    }
    return nullptr;
}

static UObject* LoadAsset(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    if (UObject* Obj = FindObject<UObject>(nullptr, *Path))
    {
        return Obj;
    }
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Obj;
}

template <typename T>
static T* LoadAssetAs(const FString& Path)
{
    return Cast<T>(LoadAsset(Path));
}

static UClass* ResolveClass(const FString& NameOrPath)
{
    if (NameOrPath.IsEmpty()) return nullptr;
    if (UClass* Cls = FindObject<UClass>(nullptr, *NameOrPath))
    {
        return Cls;
    }
    if (UClass* Cls = LoadObject<UClass>(nullptr, *NameOrPath))
    {
        return Cls;
    }
    FSoftObjectPath Soft(NameOrPath);
    if (UObject* Obj = Soft.TryLoad())
    {
        if (UClass* Cls = Cast<UClass>(Obj)) return Cls;
        if (UBlueprint* BP = Cast<UBlueprint>(Obj)) return BP->GeneratedClass;
    }
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Cls = *It;
        if (Cls && Cls->GetName() == NameOrPath)
        {
            return Cls;
        }
    }
    return nullptr;
}

static UClass* ResolveClassWithAliases(const FString& NameOrPath,
                                       const TArray<FNamedClass>& Aliases)
{
    if (UClass* Direct = ResolveClass(NameOrPath))
    {
        return Direct;
    }
    for (const FNamedClass& Alias : Aliases)
    {
        if (NameOrPath.Equals(Alias.Name, ESearchCase::IgnoreCase))
        {
            return ResolveClass(Alias.Path);
        }
    }
    return nullptr;
}

static FString ClassPath(UClass* Cls)
{
    return Cls ? Cls->GetPathName() : FString();
}

static bool SplitAssetPath(const FString& Path, FString& PackagePath, FString& AssetName)
{
    return Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd)
        && !PackagePath.IsEmpty()
        && !AssetName.IsEmpty();
}

static UObject* CreateAssetOfClass(const FString& Path, UClass* AssetClass)
{
    FString PackagePath, AssetName;
    if (!AssetClass || !SplitAssetPath(Path, PackagePath, AssetName))
    {
        return nullptr;
    }
    IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
        TEXT("AssetTools")).Get();
    return Tools.CreateAsset(AssetName, PackagePath, AssetClass, nullptr);
}

static UBlueprint* CreateBlueprintAsset(const FString& Path, UClass* ParentClass)
{
    FString PackagePath, AssetName;
    if (!ParentClass || !SplitAssetPath(Path, PackagePath, AssetName))
    {
        return nullptr;
    }
    IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
        TEXT("AssetTools")).Get();
    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>(GetTransientPackage());
    Factory->ParentClass = ParentClass;
    return Cast<UBlueprint>(Tools.CreateAsset(AssetName, PackagePath,
        UBlueprint::StaticClass(), Factory));
}

static FOutcome RequireEditorAssetSubsystem(UEditorAssetSubsystem*& Out)
{
    if (!GEditor)
    {
        return FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    Out = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!Out)
    {
        return FOutcome::MakeError(-32603, TEXT("UEditorAssetSubsystem unavailable"));
    }
    return FOutcome::MakeSuccess(MakeShared<FJsonObject>());
}

static FOutcome DeleteAssetByPath(const FString& Path, bool bConfirmed)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    if (!bConfirmed)
    {
        return FOutcome::MakeError(-32602,
            TEXT("destructive operation; pass confirmed:true to proceed"));
    }
    UEditorAssetSubsystem* Sub = nullptr;
    FOutcome SubResult = RequireEditorAssetSubsystem(Sub);
    if (!SubResult.bSuccess) return SubResult;

    FScopedTransaction Tx(LOCTEXT("SageAIDeleteAsset", "Sage AI: Delete Asset"));
    const bool bDeleted = Sub->DeleteAsset(Path);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Path);
    R->SetBoolField(TEXT("deleted"), bDeleted);
    if (!bDeleted)
    {
        R->SetStringField(TEXT("reason"), TEXT("EditorAssetSubsystem.DeleteAsset returned false"));
    }
    return FOutcome::MakeSuccess(R);
}

static FOutcome DuplicateAssetByPath(const FString& SourcePath, const FString& DestPath)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    UEditorAssetSubsystem* Sub = nullptr;
    FOutcome SubResult = RequireEditorAssetSubsystem(Sub);
    if (!SubResult.bSuccess) return SubResult;

    FScopedTransaction Tx(LOCTEXT("SageAIDuplicateAsset", "Sage AI: Duplicate Asset"));
    UObject* NewAsset = Sub->DuplicateAsset(SourcePath, DestPath);
    if (!NewAsset)
    {
        return FOutcome::MakeError(-32000,
            TEXT("EditorAssetSubsystem.DuplicateAsset returned nullptr"));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("source"), SourcePath);
    R->SetStringField(TEXT("path"), NewAsset->GetPathName());
    R->SetStringField(TEXT("class"), NewAsset->GetClass()->GetPathName());
    return FOutcome::MakeSuccess(R);
}

static TArray<FAssetData> ListAssetsByClass(UClass* Class, const FString& SearchPath, int32 MaxResults)
{
    TArray<FAssetData> Assets;
    if (!Class) return Assets;
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(Class->GetClassPathName());
    ARM.Get().GetAssets(Filter, Assets);
    if (MaxResults > 0 && Assets.Num() > MaxResults)
    {
        Assets.SetNum(MaxResults);
    }
    return Assets;
}

static TSharedRef<FJsonObject> AssetRowJson(const FAssetData& Data)
{
    auto Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("name"), Data.AssetName.ToString());
    Row->SetStringField(TEXT("path"), Data.GetSoftObjectPath().ToString());
    Row->SetStringField(TEXT("package"), Data.PackageName.ToString());
    Row->SetStringField(TEXT("class"), Data.AssetClassPath.ToString());
    return Row;
}

static TArray<TSharedPtr<FJsonValue>> AssetRowsJson(const TArray<FAssetData>& Assets)
{
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FAssetData& Asset : Assets)
    {
        Rows.Add(MakeShared<FJsonValueObject>(AssetRowJson(Asset)));
    }
    return Rows;
}

static TSharedRef<FJsonObject> ReflectObjectJson(UObject* Obj, int32 MaxProperties = 80)
{
    auto R = MakeShared<FJsonObject>();
    if (!Obj) return R;
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetStringField(TEXT("name"), Obj->GetName());
    R->SetStringField(TEXT("class"), Obj->GetClass()->GetPathName());
    auto Props = MakeShared<FJsonObject>();
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It && Count < MaxProperties; ++It)
    {
        FProperty* Prop = *It;
        if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
        {
            continue;
        }
        if (TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Obj, Prop))
        {
            Props->SetField(Prop->GetName(), Value);
            ++Count;
        }
    }
    R->SetObjectField(TEXT("properties"), Props);
    R->SetNumberField(TEXT("property_count"), Count);
    return R;
}

static bool ApplyPropertyPatch(UObject* Obj, const TSharedPtr<FJsonObject>& Args, TArray<FString>& Changed, TArray<FString>& Failed)
{
    const TSharedPtr<FJsonObject>* Props = nullptr;
    if (!Obj || !Args.IsValid() || !Args->TryGetObjectField(TEXT("properties"), Props) || !Props || !Props->IsValid())
    {
        return false;
    }
    for (const auto& Pair : (*Props)->Values)
    {
        FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*Pair.Key));
        if (!Prop || !Pair.Value.IsValid())
        {
            Failed.Add(Pair.Key);
            continue;
        }
        if (detail::SetUPropertyFromJson(Obj, Prop, Pair.Value))
        {
            Changed.Add(Pair.Key);
        }
        else
        {
            Failed.Add(Pair.Key);
        }
    }
    return Changed.Num() > 0 || Failed.Num() > 0;
}

static void FinishAssetMutation(UObject* Obj)
{
    if (!Obj) return;
    Obj->PostEditChange();
    Obj->MarkPackageDirty();
}

// ---- Blackboard ------------------------------------------------------------

static UBlackboardData* ResolveBlackboard(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstStringArg(Args, {
        TEXT("blackboard"), TEXT("blackboard_path"), TEXT("path"), TEXT("asset")
    });
    return LoadAssetAs<UBlackboardData>(Path);
}

static UClass* ResolveBlackboardKeyType(const FString& Type)
{
    static const TArray<FNamedClass> Aliases = {
        {TEXT("bool"), TEXT("/Script/AIModule.BlackboardKeyType_Bool")},
        {TEXT("boolean"), TEXT("/Script/AIModule.BlackboardKeyType_Bool")},
        {TEXT("class"), TEXT("/Script/AIModule.BlackboardKeyType_Class")},
        {TEXT("enum"), TEXT("/Script/AIModule.BlackboardKeyType_Enum")},
        {TEXT("float"), TEXT("/Script/AIModule.BlackboardKeyType_Float")},
        {TEXT("int"), TEXT("/Script/AIModule.BlackboardKeyType_Int")},
        {TEXT("integer"), TEXT("/Script/AIModule.BlackboardKeyType_Int")},
        {TEXT("name"), TEXT("/Script/AIModule.BlackboardKeyType_Name")},
        {TEXT("native_enum"), TEXT("/Script/AIModule.BlackboardKeyType_NativeEnum")},
        {TEXT("object"), TEXT("/Script/AIModule.BlackboardKeyType_Object")},
        {TEXT("rotator"), TEXT("/Script/AIModule.BlackboardKeyType_Rotator")},
        {TEXT("string"), TEXT("/Script/AIModule.BlackboardKeyType_String")},
        {TEXT("vector"), TEXT("/Script/AIModule.BlackboardKeyType_Vector")},
    };
    UClass* Cls = ResolveClassWithAliases(Type, Aliases);
    return Cls && Cls->IsChildOf(UBlackboardKeyType::StaticClass()) ? Cls : nullptr;
}

static int32 FindBlackboardKeyIndex(UBlackboardData* BB, const FString& Key)
{
    if (!BB) return INDEX_NONE;
    for (int32 Index = 0; Index < BB->Keys.Num(); ++Index)
    {
        if (BB->Keys[Index].EntryName.ToString() == Key)
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

static TSharedRef<FJsonObject> BlackboardEntryJson(const FBlackboardEntry& Entry, int32 Index)
{
    auto J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("index"), Index);
    J->SetStringField(TEXT("name"), Entry.EntryName.ToString());
    J->SetStringField(TEXT("key_type"), Entry.KeyType ? Entry.KeyType->GetClass()->GetPathName() : FString());
    J->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced != 0);
#if WITH_EDITORONLY_DATA
    J->SetStringField(TEXT("description"), Entry.EntryDescription);
    J->SetStringField(TEXT("category"), Entry.EntryCategory.ToString());
#endif
    return J;
}

static TSharedRef<FJsonObject> BlackboardJson(UBlackboardData* BB)
{
    auto R = MakeShared<FJsonObject>();
    if (!BB) return R;
    BB->UpdateParentKeys();
    BB->UpdateKeyIDs();
    BB->UpdateIfHasSynchronizedKeys();
    R->SetStringField(TEXT("path"), BB->GetPathName());
    R->SetStringField(TEXT("class"), BB->GetClass()->GetPathName());
    R->SetStringField(TEXT("parent"), BB->Parent ? BB->Parent->GetPathName() : FString());
    R->SetBoolField(TEXT("valid"), BB->IsValid());
    R->SetNumberField(TEXT("key_count"), BB->Keys.Num());
    R->SetNumberField(TEXT("total_key_count"), BB->GetNumKeys());
    TArray<TSharedPtr<FJsonValue>> Keys;
    for (int32 Index = 0; Index < BB->Keys.Num(); ++Index)
    {
        Keys.Add(MakeShared<FJsonValueObject>(BlackboardEntryJson(BB->Keys[Index], Index)));
    }
    R->SetArrayField(TEXT("keys"), Keys);
#if WITH_EDITORONLY_DATA
    TArray<TSharedPtr<FJsonValue>> ParentKeys;
    for (int32 Index = 0; Index < BB->ParentKeys.Num(); ++Index)
    {
        ParentKeys.Add(MakeShared<FJsonValueObject>(BlackboardEntryJson(BB->ParentKeys[Index], Index)));
    }
    R->SetArrayField(TEXT("parent_keys"), ParentKeys);
#endif
    return R;
}

static FOutcome AddBlackboardKey(UBlackboardData* BB, const FString& KeyName, const FString& TypeName,
                                 const TSharedPtr<FJsonObject>& Args)
{
    if (!BB) return FOutcome::MakeError(-32602, TEXT("blackboard asset not found"));
    if (KeyName.IsEmpty()) return MissingArg(TEXT("key"));
    UClass* KeyTypeClass = ResolveBlackboardKeyType(TypeName.IsEmpty() ? TEXT("object") : TypeName);
    if (!KeyTypeClass)
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("blackboard key type not found or invalid: %s"), *TypeName));
    }
    if (FindBlackboardKeyIndex(BB, KeyName) != INDEX_NONE)
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("blackboard key already exists: %s"), *KeyName));
    }

    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FScopedTransaction Tx(LOCTEXT("SageAddBlackboardKey", "Sage AI: Add Blackboard Key"));
    BB->Modify();
    FBlackboardEntry Entry;
    Entry.EntryName = FName(*KeyName);
    Entry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyTypeClass, NAME_None,
        RF_Public | RF_Transactional);
    Entry.bInstanceSynced = FirstBoolArg(Args, {TEXT("instance_synced"), TEXT("synced")}, false) ? 1 : 0;
#if WITH_EDITORONLY_DATA
    Entry.EntryDescription = FirstStringArg(Args, {TEXT("description")});
    Entry.EntryCategory = FName(*FirstStringArg(Args, {TEXT("category")}));
#endif
    BB->Keys.Add(Entry);
    BB->UpdateParentKeys();
    BB->UpdateKeyIDs();
    BB->UpdateIfHasSynchronizedKeys();
    BB->PropagateKeyChangesToDerivedBlackboardAssets();
    FinishAssetMutation(BB);
    return FOutcome::MakeSuccess(BlackboardJson(BB));
}

static FOutcome HandleBlackboardTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("delete_blackboard"))
    {
        const FString Path = FirstStringArg(Args, {TEXT("path"), TEXT("blackboard")});
        if (Path.IsEmpty()) return MissingArg(TEXT("path"));
        return DeleteAssetByPath(Path, FirstBoolArg(Args, {TEXT("confirmed")}));
    }
    if (Tool == TEXT("duplicate_blackboard"))
    {
        const FString Source = FirstStringArg(Args, {TEXT("source"), TEXT("path"), TEXT("blackboard")});
        const FString Dest = FirstStringArg(Args, {TEXT("destination"), TEXT("dest"), TEXT("new_path")});
        if (Source.IsEmpty()) return MissingArg(TEXT("source"));
        if (Dest.IsEmpty()) return MissingArg(TEXT("destination"));
        return DuplicateAssetByPath(Source, Dest);
    }

    UBlackboardData* BB = ResolveBlackboard(Args);
    if (!BB)
    {
        return FOutcome::MakeError(-32602, TEXT("blackboard asset not found"));
    }

    if (Tool == TEXT("get_blackboard") || Tool == TEXT("gameplay.read_blackboard"))
    {
        return FOutcome::MakeSuccess(BlackboardJson(BB));
    }
    if (Tool == TEXT("get_bb_key_details"))
    {
        const FString Key = FirstStringArg(Args, {TEXT("key"), TEXT("name")});
        if (Key.IsEmpty()) return MissingArg(TEXT("key"));
        const int32 Index = FindBlackboardKeyIndex(BB, Key);
        if (Index == INDEX_NONE)
        {
            return FOutcome::MakeError(-32602,
                FString::Printf(TEXT("blackboard key not found: %s"), *Key));
        }
        return FOutcome::MakeSuccess(BlackboardEntryJson(BB->Keys[Index], Index));
    }
    if (Tool == TEXT("add_bb_key") || Tool == TEXT("gameplay.add_blackboard_key"))
    {
        return AddBlackboardKey(BB,
            FirstStringArg(Args, {TEXT("key"), TEXT("name")}),
            FirstStringArg(Args, {TEXT("type"), TEXT("key_type"), TEXT("class")}),
            Args);
    }
    if (Tool == TEXT("batch_add_bb_keys"))
    {
        const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
        if (!Args.IsValid() || !Args->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
        {
            return MissingArg(TEXT("keys"));
        }
        int32 Added = 0;
        TArray<TSharedPtr<FJsonValue>> Results;
        for (const TSharedPtr<FJsonValue>& Value : *Keys)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
            {
                continue;
            }
            const FString KeyName = FirstStringArg(*Obj, {TEXT("key"), TEXT("name")});
            const FString TypeName = FirstStringArg(*Obj, {TEXT("type"), TEXT("key_type"), TEXT("class")});
            FOutcome AddedOutcome = AddBlackboardKey(BB, KeyName, TypeName, *Obj);
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("key"), KeyName);
            Row->SetBoolField(TEXT("success"), AddedOutcome.bSuccess);
            if (AddedOutcome.bSuccess) ++Added;
            else if (AddedOutcome.Error.IsValid()) Row->SetObjectField(TEXT("error"), AddedOutcome.Error);
            Results.Add(MakeShared<FJsonValueObject>(Row));
        }
        auto R = BlackboardJson(BB);
        R->SetNumberField(TEXT("added"), Added);
        R->SetArrayField(TEXT("results"), Results);
        return FOutcome::MakeSuccess(R);
    }

    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    if (Tool == TEXT("remove_bb_key") || Tool == TEXT("gameplay.remove_blackboard_key"))
    {
        const FString Key = FirstStringArg(Args, {TEXT("key"), TEXT("name")});
        const int32 Index = FindBlackboardKeyIndex(BB, Key);
        if (Index == INDEX_NONE)
        {
            return FOutcome::MakeError(-32602,
                FString::Printf(TEXT("blackboard key not found: %s"), *Key));
        }
        FScopedTransaction Tx(LOCTEXT("SageRemoveBlackboardKey", "Sage AI: Remove Blackboard Key"));
        BB->Modify();
        BB->Keys.RemoveAt(Index);
        BB->UpdateParentKeys();
        BB->UpdateKeyIDs();
        BB->UpdateIfHasSynchronizedKeys();
        BB->PropagateKeyChangesToDerivedBlackboardAssets();
        FinishAssetMutation(BB);
        return FOutcome::MakeSuccess(BlackboardJson(BB));
    }
    if (Tool == TEXT("rename_bb_key"))
    {
        const FString OldKey = FirstStringArg(Args, {TEXT("old_key"), TEXT("key"), TEXT("name")});
        const FString NewKey = FirstStringArg(Args, {TEXT("new_key"), TEXT("new_name")});
        if (NewKey.IsEmpty()) return MissingArg(TEXT("new_key"));
        const int32 Index = FindBlackboardKeyIndex(BB, OldKey);
        if (Index == INDEX_NONE)
        {
            return FOutcome::MakeError(-32602,
                FString::Printf(TEXT("blackboard key not found: %s"), *OldKey));
        }
        FScopedTransaction Tx(LOCTEXT("SageRenameBlackboardKey", "Sage AI: Rename Blackboard Key"));
        BB->Modify();
        BB->Keys[Index].EntryName = FName(*NewKey);
        BB->UpdateParentKeys();
        BB->UpdateKeyIDs();
        BB->PropagateKeyChangesToDerivedBlackboardAssets();
        FinishAssetMutation(BB);
        return FOutcome::MakeSuccess(BlackboardJson(BB));
    }
    if (Tool == TEXT("set_bb_parent") || Tool == TEXT("gameplay.set_blackboard_parent"))
    {
        const FString ParentPath = FirstStringArg(Args, {TEXT("parent"), TEXT("parent_path")});
        UBlackboardData* Parent = ParentPath.IsEmpty() ? nullptr : LoadAssetAs<UBlackboardData>(ParentPath);
        if (!ParentPath.IsEmpty() && !Parent)
        {
            return FOutcome::MakeError(-32602,
                FString::Printf(TEXT("parent blackboard not found: %s"), *ParentPath));
        }
        FScopedTransaction Tx(LOCTEXT("SageSetBlackboardParent", "Sage AI: Set Blackboard Parent"));
        BB->Modify();
        BB->Parent = Parent;
        BB->UpdateParentKeys();
        BB->UpdateKeyIDs();
        BB->UpdateIfHasSynchronizedKeys();
        FinishAssetMutation(BB);
        return FOutcome::MakeSuccess(BlackboardJson(BB));
    }
    if (Tool == TEXT("compare_blackboards"))
    {
        UBlackboardData* Other = LoadAssetAs<UBlackboardData>(
            FirstStringArg(Args, {TEXT("other"), TEXT("other_path"), TEXT("target")}));
        if (!Other) return MissingArg(TEXT("other"));
        TSet<FString> A, B;
        for (const FBlackboardEntry& Entry : BB->Keys) A.Add(Entry.EntryName.ToString());
        for (const FBlackboardEntry& Entry : Other->Keys) B.Add(Entry.EntryName.ToString());
        TArray<FString> OnlyA, OnlyB, Both;
        for (const FString& Key : A) { (B.Contains(Key) ? Both : OnlyA).Add(Key); }
        for (const FString& Key : B) { if (!A.Contains(Key)) OnlyB.Add(Key); }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("left"), BB->GetPathName());
        R->SetStringField(TEXT("right"), Other->GetPathName());
        R->SetArrayField(TEXT("shared"), StringsToJson(Both));
        R->SetArrayField(TEXT("left_only"), StringsToJson(OnlyA));
        R->SetArrayField(TEXT("right_only"), StringsToJson(OnlyB));
        R->SetBoolField(TEXT("related"), BB->IsRelatedTo(*Other));
        return FOutcome::MakeSuccess(R);
    }
    return Unsupported(Tool, TEXT("unknown blackboard operation"));
}

// ---- Behavior Tree ---------------------------------------------------------

static UBehaviorTree* ResolveBehaviorTree(const TSharedPtr<FJsonObject>& Args)
{
    return LoadAssetAs<UBehaviorTree>(FirstStringArg(Args, {
        TEXT("behavior_tree"), TEXT("tree"), TEXT("path"), TEXT("asset")
    }));
}

static UClass* ResolveBtNodeClass(const TSharedPtr<FJsonObject>& Args, UClass* RequiredBase, const FString& DefaultClassPath)
{
    FString ClassPath = FirstStringArg(Args, {TEXT("node_class"), TEXT("class"), TEXT("task_class")});
    if (ClassPath.IsEmpty()) ClassPath = DefaultClassPath;
    UClass* Cls = ResolveClass(ClassPath);
    return Cls && Cls->IsChildOf(RequiredBase) ? Cls : nullptr;
}

static UBTCompositeNode* EnsureRootComposite(UBehaviorTree* BT)
{
    if (!BT) return nullptr;
    if (BT->RootNode) return BT->RootNode;
    UClass* RootClass = ResolveClass(TEXT("/Script/AIModule.BTComposite_Sequence"));
    if (!RootClass || !RootClass->IsChildOf(UBTCompositeNode::StaticClass()))
    {
        return nullptr;
    }
    BT->Modify();
    BT->RootNode = NewObject<UBTCompositeNode>(BT, RootClass, NAME_None,
        RF_Public | RF_Transactional);
    BT->RootNode->NodeName = TEXT("Root");
    BT->RootNode->InitializeFromAsset(*BT);
    return BT->RootNode;
}

static void CollectBtNodes(UBTNode* Node, TArray<UBTNode*>& Out)
{
    if (!Node) return;
    Out.Add(Node);
    if (UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
    {
        for (FBTCompositeChild& Child : Composite->Children)
        {
            CollectBtNodes(Child.ChildComposite, Out);
            CollectBtNodes(Child.ChildTask, Out);
            for (UBTDecorator* Decorator : Child.Decorators) CollectBtNodes(Decorator, Out);
        }
        for (UBTService* Service : Composite->Services) CollectBtNodes(Service, Out);
    }
    if (UBTTaskNode* Task = Cast<UBTTaskNode>(Node))
    {
        for (UBTService* Service : Task->Services) CollectBtNodes(Service, Out);
    }
}

static UBTNode* FindBtNode(UBehaviorTree* BT, const TSharedPtr<FJsonObject>& Args)
{
    if (!BT) return nullptr;
    TArray<UBTNode*> Nodes;
    CollectBtNodes(BT->RootNode, Nodes);
    const FString Name = FirstStringArg(Args, {TEXT("node"), TEXT("node_name"), TEXT("name")});
    if (!Name.IsEmpty())
    {
        for (UBTNode* Node : Nodes)
        {
            if (Node && (Node->GetName() == Name || Node->NodeName == Name || Node->GetNodeName() == Name))
            {
                return Node;
            }
        }
    }
    const int32 Index = FirstIntArg(Args, {TEXT("node_index"), TEXT("index")}, INDEX_NONE);
    return Nodes.IsValidIndex(Index) ? Nodes[Index] : nullptr;
}

static TSharedRef<FJsonObject> BtNodeJson(UBTNode* Node)
{
    auto J = MakeShared<FJsonObject>();
    if (!Node) return J;
    J->SetStringField(TEXT("name"), Node->NodeName);
    J->SetStringField(TEXT("object_name"), Node->GetName());
    J->SetStringField(TEXT("display_name"), Node->GetNodeName());
    J->SetStringField(TEXT("path"), Node->GetPathName());
    J->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
#if WITH_EDITOR
    J->SetStringField(TEXT("static_description"), Node->GetStaticDescription());
    J->SetStringField(TEXT("error"), Node->GetErrorMessage());
#endif
    return J;
}

static TSharedRef<FJsonObject> BehaviorTreeJson(UBehaviorTree* BT)
{
    auto R = MakeShared<FJsonObject>();
    if (!BT) return R;
    R->SetStringField(TEXT("path"), BT->GetPathName());
    R->SetStringField(TEXT("class"), BT->GetClass()->GetPathName());
    R->SetStringField(TEXT("blackboard"), BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : FString());
    TArray<UBTNode*> Nodes;
    CollectBtNodes(BT->RootNode, Nodes);
    TArray<TSharedPtr<FJsonValue>> NodeRows;
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        auto Row = BtNodeJson(Nodes[Index]);
        Row->SetNumberField(TEXT("index"), Index);
        NodeRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    R->SetArrayField(TEXT("nodes"), NodeRows);
    R->SetNumberField(TEXT("node_count"), Nodes.Num());
    R->SetBoolField(TEXT("has_root"), BT->RootNode != nullptr);
    return R;
}

static FOutcome AddBtNode(UBehaviorTree* BT, const TSharedPtr<FJsonObject>& Args,
                          const FString& DefaultClassPath)
{
    if (!BT) return FOutcome::MakeError(-32602, TEXT("behavior tree not found"));
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ClassText = FirstStringArg(Args, {TEXT("node_class"), TEXT("class"), TEXT("task_class")});
    if (ClassText.IsEmpty()) ClassText = DefaultClassPath;
    UClass* NodeClass = ResolveClass(ClassText);
    if (!NodeClass || (!NodeClass->IsChildOf(UBTCompositeNode::StaticClass())
        && !NodeClass->IsChildOf(UBTTaskNode::StaticClass())))
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("BT node class must derive from UBTCompositeNode or UBTTaskNode: %s"), *ClassText));
    }

    FScopedTransaction Tx(LOCTEXT("SageAddBtNode", "Sage AI: Add BehaviorTree Node"));
    BT->Modify();
    UBTCompositeNode* Root = EnsureRootComposite(BT);
    if (!Root) return FOutcome::MakeError(-32603, TEXT("failed to create root composite"));

    if (NodeClass->IsChildOf(UBTCompositeNode::StaticClass()))
    {
        UBTCompositeNode* Node = NewObject<UBTCompositeNode>(BT, NodeClass, NAME_None,
            RF_Public | RF_Transactional);
        Node->NodeName = FirstStringArg(Args, {TEXT("name"), TEXT("node_name")});
        if (Node->NodeName.IsEmpty()) Node->NodeName = NodeClass->GetName();
        Node->InitializeFromAsset(*BT);
        FBTCompositeChild Child;
        Child.ChildComposite = Node;
        Root->Children.Add(Child);
    }
    else
    {
        UBTTaskNode* Node = NewObject<UBTTaskNode>(BT, NodeClass, NAME_None,
            RF_Public | RF_Transactional);
        Node->NodeName = FirstStringArg(Args, {TEXT("name"), TEXT("node_name")});
        if (Node->NodeName.IsEmpty()) Node->NodeName = NodeClass->GetName();
        Node->InitializeFromAsset(*BT);
        FBTCompositeChild Child;
        Child.ChildTask = Node;
        Root->Children.Add(Child);
    }
    FinishAssetMutation(BT);
    return FOutcome::MakeSuccess(BehaviorTreeJson(BT));
}

static FOutcome HandleBehaviorTreeTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (BehaviorGraphPrivateTools().Contains(Tool))
    {
        return Unsupported(Tool,
            TEXT("this operation targets BehaviorTreeEditor visual graph/layout state; current safe implementation edits runtime tree asset data only"));
    }
    if (Tool == TEXT("delete_behavior_tree"))
    {
        const FString Path = FirstStringArg(Args, {TEXT("path"), TEXT("behavior_tree")});
        if (Path.IsEmpty()) return MissingArg(TEXT("path"));
        return DeleteAssetByPath(Path, FirstBoolArg(Args, {TEXT("confirmed")}));
    }
    if (Tool == TEXT("duplicate_behavior_tree"))
    {
        const FString Source = FirstStringArg(Args, {TEXT("source"), TEXT("path"), TEXT("behavior_tree")});
        const FString Dest = FirstStringArg(Args, {TEXT("destination"), TEXT("dest"), TEXT("new_path")});
        if (Source.IsEmpty()) return MissingArg(TEXT("source"));
        if (Dest.IsEmpty()) return MissingArg(TEXT("destination"));
        return DuplicateAssetByPath(Source, Dest);
    }
    if (Tool == TEXT("create_bt_task_blueprint")
        || Tool == TEXT("create_bt_decorator_blueprint")
        || Tool == TEXT("create_bt_service_blueprint"))
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        const FString Path = FirstStringArg(Args, {TEXT("path")});
        if (Path.IsEmpty()) return MissingArg(TEXT("path"));
        FString ParentPath = FirstStringArg(Args, {TEXT("parent"), TEXT("parent_class"), TEXT("class")});
        if (ParentPath.IsEmpty())
        {
            if (Tool == TEXT("create_bt_task_blueprint")) ParentPath = TEXT("/Script/AIModule.BTTask_BlueprintBase");
            else if (Tool == TEXT("create_bt_decorator_blueprint")) ParentPath = TEXT("/Script/AIModule.BTDecorator_BlueprintBase");
            else ParentPath = TEXT("/Script/AIModule.BTService_BlueprintBase");
        }
        UClass* Parent = ResolveClass(ParentPath);
        if (!Parent) return FOutcome::MakeError(-32602, FString::Printf(TEXT("parent class not found: %s"), *ParentPath));
        UBlueprint* BP = CreateBlueprintAsset(Path, Parent);
        if (!BP) return FOutcome::MakeError(-32000, TEXT("CreateAsset returned nullptr"));
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), BP->GetPathName());
        R->SetStringField(TEXT("parent_class"), Parent->GetPathName());
        return FOutcome::MakeSuccess(R);
    }

    UBehaviorTree* BT = ResolveBehaviorTree(Args);
    if (!BT && Tool == TEXT("build_behavior_tree_from_spec"))
    {
        const TSharedPtr<FJsonObject>* Spec = nullptr;
        if (Args.IsValid() && Args->TryGetObjectField(TEXT("spec"), Spec) && Spec && Spec->IsValid())
        {
            FString Path;
            (*Spec)->TryGetStringField(TEXT("path"), Path);
            if (!Path.IsEmpty())
            {
                UClass* BTClass = ResolveClass(TEXT("/Script/AIModule.BehaviorTree"));
                BT = Cast<UBehaviorTree>(CreateAssetOfClass(Path, BTClass));
            }
        }
    }
    if (!BT)
    {
        return FOutcome::MakeError(-32602, TEXT("behavior tree not found"));
    }

    if (Tool == TEXT("get_bt_graph") || Tool == TEXT("export_bt_spec")
        || Tool == TEXT("gameplay.get_behavior_tree_info"))
    {
        return FOutcome::MakeSuccess(BehaviorTreeJson(BT));
    }
    if (Tool == TEXT("set_bt_blackboard"))
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        UBlackboardData* BB = LoadAssetAs<UBlackboardData>(
            FirstStringArg(Args, {TEXT("blackboard"), TEXT("blackboard_path")}));
        if (!BB) return MissingArg(TEXT("blackboard"));
        FScopedTransaction Tx(LOCTEXT("SageSetBtBlackboard", "Sage AI: Set BT Blackboard"));
        BT->Modify();
        BT->BlackboardAsset = BB;
        FinishAssetMutation(BT);
        return FOutcome::MakeSuccess(BehaviorTreeJson(BT));
    }
    if (Tool == TEXT("add_bt_node"))
    {
        return AddBtNode(BT, Args, TEXT("/Script/AIModule.BTTask_Wait"));
    }
    if (Tool == TEXT("add_bt_run_eqs_task"))
    {
        return AddBtNode(BT, Args, TEXT("/Script/AIModule.BTTask_RunEQSQuery"));
    }
    if (Tool == TEXT("add_bt_smart_object_task"))
    {
        return AddBtNode(BT, Args, TEXT("/Script/GameplayBehaviorSmartObjectsModule.BTTask_FindAndUseGameplayBehaviorSmartObject"));
    }
    if (Tool == TEXT("add_bt_use_ability_task"))
    {
        return AddBtNode(BT, Args, TEXT("/Script/AIModule.BTTask_BlueprintBase"));
    }
    if (Tool == TEXT("remove_bt_node"))
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        UBTCompositeNode* Root = BT->RootNode;
        const int32 Index = FirstIntArg(Args, {TEXT("child_index"), TEXT("index"), TEXT("node_index")}, INDEX_NONE);
        if (!Root || !Root->Children.IsValidIndex(Index))
        {
            return FOutcome::MakeError(-32602, TEXT("root child index not found"));
        }
        FScopedTransaction Tx(LOCTEXT("SageRemoveBtNode", "Sage AI: Remove BT Node"));
        BT->Modify();
        Root->Children.RemoveAt(Index);
        FinishAssetMutation(BT);
        return FOutcome::MakeSuccess(BehaviorTreeJson(BT));
    }
    if (Tool == TEXT("move_bt_node") || Tool == TEXT("reorder_bt_children"))
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        UBTCompositeNode* Root = BT->RootNode;
        const int32 From = FirstIntArg(Args, {TEXT("from"), TEXT("index"), TEXT("child_index")}, INDEX_NONE);
        const int32 To = FirstIntArg(Args, {TEXT("to"), TEXT("new_index")}, INDEX_NONE);
        if (!Root || !Root->Children.IsValidIndex(From) || To < 0 || To >= Root->Children.Num())
        {
            return FOutcome::MakeError(-32602, TEXT("invalid from/to root child index"));
        }
        FScopedTransaction Tx(LOCTEXT("SageMoveBtNode", "Sage AI: Move BT Node"));
        BT->Modify();
        FBTCompositeChild Child = Root->Children[From];
        Root->Children.RemoveAt(From);
        Root->Children.Insert(Child, To);
        FinishAssetMutation(BT);
        return FOutcome::MakeSuccess(BehaviorTreeJson(BT));
    }
    if (Tool == TEXT("add_bt_decorator") || Tool == TEXT("add_bt_service"))
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        UBTCompositeNode* Root = EnsureRootComposite(BT);
        if (!Root) return FOutcome::MakeError(-32603, TEXT("root composite unavailable"));
        const bool bDecorator = Tool == TEXT("add_bt_decorator");
        const FString ClassText = FirstStringArg(Args, {TEXT("class"), TEXT("node_class")});
        UClass* Cls = ResolveClass(ClassText);
        UClass* Base = bDecorator ? UBTDecorator::StaticClass() : UBTService::StaticClass();
        if (!Cls || !Cls->IsChildOf(Base))
        {
            return FOutcome::MakeError(-32602,
                FString::Printf(TEXT("class must derive from %s: %s"), *Base->GetName(), *ClassText));
        }
        const int32 ChildIndex = FirstIntArg(Args, {TEXT("child_index"), TEXT("index")}, 0);
        if (!Root->Children.IsValidIndex(ChildIndex))
        {
            return FOutcome::MakeError(-32602, TEXT("child_index not found"));
        }
        FScopedTransaction Tx(LOCTEXT("SageAddBtAux", "Sage AI: Add BT Aux Node"));
        BT->Modify();
        if (bDecorator)
        {
            UBTDecorator* Decorator = NewObject<UBTDecorator>(BT, Cls, NAME_None,
                RF_Public | RF_Transactional);
            Decorator->NodeName = FirstStringArg(Args, {TEXT("name"), TEXT("node_name")});
            Root->Children[ChildIndex].Decorators.Add(Decorator);
        }
        else
        {
            UBTService* Service = NewObject<UBTService>(BT, Cls, NAME_None,
                RF_Public | RF_Transactional);
            Service->NodeName = FirstStringArg(Args, {TEXT("name"), TEXT("node_name")});
            Root->Services.Add(Service);
        }
        FinishAssetMutation(BT);
        return FOutcome::MakeSuccess(BehaviorTreeJson(BT));
    }
    if (Tool == TEXT("remove_bt_decorator") || Tool == TEXT("remove_bt_service"))
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        UBTCompositeNode* Root = BT->RootNode;
        if (!Root) return FOutcome::MakeError(-32602, TEXT("behavior tree has no root"));
        const bool bDecorator = Tool == TEXT("remove_bt_decorator");
        const int32 Index = FirstIntArg(Args, {TEXT("index")}, INDEX_NONE);
        FScopedTransaction Tx(LOCTEXT("SageRemoveBtAux", "Sage AI: Remove BT Aux Node"));
        BT->Modify();
        if (bDecorator)
        {
            const int32 ChildIndex = FirstIntArg(Args, {TEXT("child_index")}, 0);
            if (!Root->Children.IsValidIndex(ChildIndex)
                || !Root->Children[ChildIndex].Decorators.IsValidIndex(Index))
            {
                return FOutcome::MakeError(-32602, TEXT("decorator index not found"));
            }
            Root->Children[ChildIndex].Decorators.RemoveAt(Index);
        }
        else
        {
            if (!Root->Services.IsValidIndex(Index))
            {
                return FOutcome::MakeError(-32602, TEXT("service index not found"));
            }
            Root->Services.RemoveAt(Index);
        }
        FinishAssetMutation(BT);
        return FOutcome::MakeSuccess(BehaviorTreeJson(BT));
    }
    if (Tool == TEXT("get_bt_node_properties") || Tool == TEXT("set_bt_node_property"))
    {
        UBTNode* Node = FindBtNode(BT, Args);
        if (!Node) return FOutcome::MakeError(-32602, TEXT("node not found"));
        if (Tool == TEXT("get_bt_node_properties"))
        {
            return FOutcome::MakeSuccess(ReflectObjectJson(Node));
        }
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        FScopedTransaction Tx(LOCTEXT("SageSetBtNodeProperties", "Sage AI: Set BT Node Properties"));
        Node->Modify();
        TArray<FString> Changed, Failed;
        ApplyPropertyPatch(Node, Args, Changed, Failed);
        FinishAssetMutation(BT);
        auto R = ReflectObjectJson(Node);
        R->SetArrayField(TEXT("changed"), StringsToJson(Changed));
        R->SetArrayField(TEXT("failed"), StringsToJson(Failed));
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("build_behavior_tree_from_spec") || Tool == TEXT("import_bt_spec"))
    {
        const TSharedPtr<FJsonObject>* Spec = nullptr;
        if (Args.IsValid() && Args->TryGetObjectField(TEXT("spec"), Spec) && Spec && Spec->IsValid())
        {
            FString BlackboardPath;
            if ((*Spec)->TryGetStringField(TEXT("blackboard"), BlackboardPath))
            {
                UBlackboardData* BB = LoadAssetAs<UBlackboardData>(BlackboardPath);
                if (BB) BT->BlackboardAsset = BB;
            }
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if ((*Spec)->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
            {
                for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
                {
                    const TSharedPtr<FJsonObject>* NodeObj = nullptr;
                    if (NodeValue.IsValid() && NodeValue->TryGetObject(NodeObj) && NodeObj && NodeObj->IsValid())
                    {
                        AddBtNode(BT, *NodeObj, TEXT("/Script/AIModule.BTTask_Wait"));
                    }
                }
            }
        }
        return FOutcome::MakeSuccess(BehaviorTreeJson(BT));
    }
    if (Tool == TEXT("compare_behavior_trees"))
    {
        UBehaviorTree* Other = LoadAssetAs<UBehaviorTree>(
            FirstStringArg(Args, {TEXT("other"), TEXT("target"), TEXT("other_path")}));
        if (!Other) return MissingArg(TEXT("other"));
        auto R = MakeShared<FJsonObject>();
        R->SetObjectField(TEXT("left"), BehaviorTreeJson(BT));
        R->SetObjectField(TEXT("right"), BehaviorTreeJson(Other));
        R->SetBoolField(TEXT("same_blackboard"), BT->BlackboardAsset == Other->BlackboardAsset);
        return FOutcome::MakeSuccess(R);
    }
    return Unsupported(Tool, TEXT("unknown BehaviorTree operation"));
}

// ---- EQS -------------------------------------------------------------------

static UEnvQuery* ResolveEqs(const TSharedPtr<FJsonObject>& Args)
{
    return LoadAssetAs<UEnvQuery>(FirstStringArg(Args, {
        TEXT("query"), TEXT("eqs"), TEXT("path"), TEXT("asset")
    }));
}

static TSharedRef<FJsonObject> EqsOptionJson(UEnvQueryOption* Option, int32 Index)
{
    auto J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("index"), Index);
    if (!Option) return J;
    J->SetStringField(TEXT("path"), Option->GetPathName());
    J->SetStringField(TEXT("generator"), Option->Generator ? Option->Generator->GetPathName() : FString());
    J->SetStringField(TEXT("generator_class"), Option->Generator ? Option->Generator->GetClass()->GetPathName() : FString());
    TArray<TSharedPtr<FJsonValue>> Tests;
    for (int32 TestIndex = 0; TestIndex < Option->Tests.Num(); ++TestIndex)
    {
        auto Row = MakeShared<FJsonObject>();
        UEnvQueryTest* Test = Option->Tests[TestIndex];
        Row->SetNumberField(TEXT("index"), TestIndex);
        Row->SetStringField(TEXT("path"), Test ? Test->GetPathName() : FString());
        Row->SetStringField(TEXT("class"), Test ? Test->GetClass()->GetPathName() : FString());
        Tests.Add(MakeShared<FJsonValueObject>(Row));
    }
    J->SetArrayField(TEXT("tests"), Tests);
    J->SetNumberField(TEXT("test_count"), Tests.Num());
    return J;
}

static TSharedRef<FJsonObject> EqsJson(UEnvQuery* Query)
{
    auto R = MakeShared<FJsonObject>();
    if (!Query) return R;
    R->SetStringField(TEXT("path"), Query->GetPathName());
    R->SetStringField(TEXT("class"), Query->GetClass()->GetPathName());
    R->SetStringField(TEXT("query_name"), Query->GetQueryName().ToString());
    TArray<TSharedPtr<FJsonValue>> Options;
    const TArray<UEnvQueryOption*>& Opts = Query->GetOptions();
    for (int32 Index = 0; Index < Opts.Num(); ++Index)
    {
        Options.Add(MakeShared<FJsonValueObject>(EqsOptionJson(Opts[Index], Index)));
    }
    R->SetArrayField(TEXT("options"), Options);
    R->SetNumberField(TEXT("option_count"), Options.Num());
    return R;
}

static TArray<TSharedPtr<FJsonValue>> ListClassesJson(UClass* Base, const FString& Query, int32 MaxResults = 256)
{
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Cls = *It;
        if (!Cls || !Cls->IsChildOf(Base) || Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
        {
            continue;
        }
        if (!Query.IsEmpty() && !Cls->GetName().Contains(Query) && !Cls->GetPathName().Contains(Query))
        {
            continue;
        }
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Cls->GetName());
        Row->SetStringField(TEXT("path"), Cls->GetPathName());
        Rows.Add(MakeShared<FJsonValueObject>(Row));
        if (MaxResults > 0 && Rows.Num() >= MaxResults) break;
    }
    return Rows;
}

static FOutcome HandleEqsTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("list_eqs_generator_types")
        || Tool == TEXT("list_eqs_test_types")
        || Tool == TEXT("list_eqs_contexts"))
    {
        UClass* Base = Tool == TEXT("list_eqs_generator_types")
            ? UEnvQueryGenerator::StaticClass()
            : Tool == TEXT("list_eqs_test_types")
                ? UEnvQueryTest::StaticClass()
                : UEnvQueryContext::StaticClass();
        auto R = MakeShared<FJsonObject>();
        const FString Query = FirstStringArg(Args, {TEXT("query"), TEXT("filter")});
        TArray<TSharedPtr<FJsonValue>> Rows = ListClassesJson(Base, Query);
        R->SetArrayField(TEXT("classes"), Rows);
        R->SetNumberField(TEXT("count"), Rows.Num());
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("delete_eqs_query"))
    {
        const FString Path = FirstStringArg(Args, {TEXT("path"), TEXT("query")});
        if (Path.IsEmpty()) return MissingArg(TEXT("path"));
        return DeleteAssetByPath(Path, FirstBoolArg(Args, {TEXT("confirmed")}));
    }
    if (Tool == TEXT("duplicate_eqs_query"))
    {
        const FString Source = FirstStringArg(Args, {TEXT("source"), TEXT("path"), TEXT("query")});
        const FString Dest = FirstStringArg(Args, {TEXT("destination"), TEXT("dest"), TEXT("new_path")});
        if (Source.IsEmpty()) return MissingArg(TEXT("source"));
        if (Dest.IsEmpty()) return MissingArg(TEXT("destination"));
        return DuplicateAssetByPath(Source, Dest);
    }
    if (Tool == TEXT("create_eqs_from_template") || Tool == TEXT("build_eqs_query_from_spec"))
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        const FString Path = FirstStringArg(Args, {TEXT("path")});
        if (Path.IsEmpty()) return MissingArg(TEXT("path"));
        UClass* EQSClass = ResolveClass(TEXT("/Script/AIModule.EnvQuery"));
        UEnvQuery* Query = Cast<UEnvQuery>(CreateAssetOfClass(Path, EQSClass));
        if (!Query) return FOutcome::MakeError(-32000, TEXT("CreateAsset returned nullptr"));
        return FOutcome::MakeSuccess(EqsJson(Query));
    }

    UEnvQuery* Query = ResolveEqs(Args);
    if (!Query)
    {
        return FOutcome::MakeError(-32602, TEXT("EnvQuery asset not found"));
    }
    if (Tool == TEXT("get_eqs_query") || Tool == TEXT("validate_eqs_query"))
    {
        auto R = EqsJson(Query);
        R->SetBoolField(TEXT("valid"), Query->GetOptions().Num() > 0);
        if (Query->GetOptions().Num() == 0)
        {
            R->SetStringField(TEXT("warning"), TEXT("query has no options/generators"));
        }
        return FOutcome::MakeSuccess(R);
    }

    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    if (Tool == TEXT("add_eqs_generator"))
    {
        const FString ClassText = FirstStringArg(Args, {TEXT("generator_class"), TEXT("class")});
        UClass* GenClass = ResolveClass(ClassText.IsEmpty() ? TEXT("/Script/AIModule.EnvQueryGenerator_SimpleGrid") : ClassText);
        if (!GenClass || !GenClass->IsChildOf(UEnvQueryGenerator::StaticClass()))
        {
            return FOutcome::MakeError(-32602, FString::Printf(TEXT("invalid EQS generator class: %s"), *ClassText));
        }
        FScopedTransaction Tx(LOCTEXT("SageAddEqsGenerator", "Sage AI: Add EQS Generator"));
        Query->Modify();
        UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, UEnvQueryOption::StaticClass(), NAME_None,
            RF_Public | RF_Transactional);
        Option->Generator = NewObject<UEnvQueryGenerator>(Option, GenClass, NAME_None,
            RF_Public | RF_Transactional);
        Query->GetOptionsMutable().Add(Option);
        FinishAssetMutation(Query);
        return FOutcome::MakeSuccess(EqsJson(Query));
    }
    if (Tool == TEXT("remove_eqs_generator"))
    {
        const int32 OptionIndex = FirstIntArg(Args, {TEXT("option_index"), TEXT("index")}, INDEX_NONE);
        TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
        if (!Options.IsValidIndex(OptionIndex))
        {
            return FOutcome::MakeError(-32602, TEXT("option_index not found"));
        }
        FScopedTransaction Tx(LOCTEXT("SageRemoveEqsGenerator", "Sage AI: Remove EQS Generator"));
        Query->Modify();
        Options.RemoveAt(OptionIndex);
        FinishAssetMutation(Query);
        return FOutcome::MakeSuccess(EqsJson(Query));
    }
    if (Tool == TEXT("add_eqs_test"))
    {
        const int32 OptionIndex = FirstIntArg(Args, {TEXT("option_index"), TEXT("index")}, 0);
        TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
        if (!Options.IsValidIndex(OptionIndex))
        {
            return FOutcome::MakeError(-32602, TEXT("option_index not found"));
        }
        const FString ClassText = FirstStringArg(Args, {TEXT("test_class"), TEXT("class")});
        UClass* TestClass = ResolveClass(ClassText);
        if (!TestClass || !TestClass->IsChildOf(UEnvQueryTest::StaticClass()))
        {
            return FOutcome::MakeError(-32602, FString::Printf(TEXT("invalid EQS test class: %s"), *ClassText));
        }
        FScopedTransaction Tx(LOCTEXT("SageAddEqsTest", "Sage AI: Add EQS Test"));
        Query->Modify();
        UEnvQueryTest* Test = NewObject<UEnvQueryTest>(Options[OptionIndex], TestClass, NAME_None,
            RF_Public | RF_Transactional);
        Options[OptionIndex]->Tests.Add(Test);
        FinishAssetMutation(Query);
        return FOutcome::MakeSuccess(EqsJson(Query));
    }
    if (Tool == TEXT("remove_eqs_test"))
    {
        const int32 OptionIndex = FirstIntArg(Args, {TEXT("option_index")}, 0);
        const int32 TestIndex = FirstIntArg(Args, {TEXT("test_index"), TEXT("index")}, INDEX_NONE);
        TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
        if (!Options.IsValidIndex(OptionIndex) || !Options[OptionIndex]->Tests.IsValidIndex(TestIndex))
        {
            return FOutcome::MakeError(-32602, TEXT("option_index/test_index not found"));
        }
        FScopedTransaction Tx(LOCTEXT("SageRemoveEqsTest", "Sage AI: Remove EQS Test"));
        Query->Modify();
        Options[OptionIndex]->Tests.RemoveAt(TestIndex);
        FinishAssetMutation(Query);
        return FOutcome::MakeSuccess(EqsJson(Query));
    }
    if (Tool == TEXT("reorder_eqs_tests"))
    {
        const int32 OptionIndex = FirstIntArg(Args, {TEXT("option_index")}, 0);
        const int32 From = FirstIntArg(Args, {TEXT("from"), TEXT("test_index"), TEXT("index")}, INDEX_NONE);
        const int32 To = FirstIntArg(Args, {TEXT("to"), TEXT("new_index")}, INDEX_NONE);
        TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
        if (!Options.IsValidIndex(OptionIndex) || !Options[OptionIndex]->Tests.IsValidIndex(From)
            || To < 0 || To >= Options[OptionIndex]->Tests.Num())
        {
            return FOutcome::MakeError(-32602, TEXT("invalid option/from/to index"));
        }
        FScopedTransaction Tx(LOCTEXT("SageReorderEqsTests", "Sage AI: Reorder EQS Tests"));
        Query->Modify();
        UEnvQueryTest* Test = Options[OptionIndex]->Tests[From];
        Options[OptionIndex]->Tests.RemoveAt(From);
        Options[OptionIndex]->Tests.Insert(Test, To);
        FinishAssetMutation(Query);
        return FOutcome::MakeSuccess(EqsJson(Query));
    }
    if (Tool == TEXT("configure_eqs_generator")
        || Tool == TEXT("configure_eqs_test")
        || Tool == TEXT("configure_eqs_scoring")
        || Tool == TEXT("configure_eqs_filter"))
    {
        const int32 OptionIndex = FirstIntArg(Args, {TEXT("option_index")}, 0);
        TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
        if (!Options.IsValidIndex(OptionIndex))
        {
            return FOutcome::MakeError(-32602, TEXT("option_index not found"));
        }
        UObject* Target = nullptr;
        if (Tool == TEXT("configure_eqs_generator"))
        {
            Target = Options[OptionIndex]->Generator;
        }
        else
        {
            const int32 TestIndex = FirstIntArg(Args, {TEXT("test_index"), TEXT("index")}, 0);
            if (!Options[OptionIndex]->Tests.IsValidIndex(TestIndex))
            {
                return FOutcome::MakeError(-32602, TEXT("test_index not found"));
            }
            Target = Options[OptionIndex]->Tests[TestIndex];
        }
        if (!Target) return FOutcome::MakeError(-32602, TEXT("target node not found"));
        FScopedTransaction Tx(LOCTEXT("SageConfigureEqsNode", "Sage AI: Configure EQS Node"));
        Target->Modify();
        TArray<FString> Changed, Failed;
        ApplyPropertyPatch(Target, Args, Changed, Failed);
        FinishAssetMutation(Query);
        auto R = ReflectObjectJson(Target);
        R->SetArrayField(TEXT("changed"), StringsToJson(Changed));
        R->SetArrayField(TEXT("failed"), StringsToJson(Failed));
        return FOutcome::MakeSuccess(R);
    }
    return Unsupported(Tool, TEXT("unknown EQS operation"));
}

// ---- SmartObject / Mass / ZoneGraph reflection surfaces --------------------

static FOutcome ListDomainAssets(const FString& Tool, const FString& ClassPath,
                                 const FString& ResultField, const TSharedPtr<FJsonObject>& Args)
{
    UClass* Cls = ResolveClass(ClassPath);
    if (!Cls)
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("required class not found: %s"), *ClassPath));
    }
    FString SearchPath = FirstStringArg(Args, {TEXT("path"), TEXT("folder")});
    if (SearchPath.IsEmpty()) SearchPath = TEXT("/Game");
    const int32 Max = FirstIntArg(Args, {TEXT("max_results"), TEXT("max")}, 256);
    TArray<FAssetData> Assets = ListAssetsByClass(Cls, SearchPath, Max);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("tool"), Tool);
    R->SetArrayField(ResultField, AssetRowsJson(Assets));
    R->SetNumberField(TEXT("count"), Assets.Num());
    return FOutcome::MakeSuccess(R);
}

static FOutcome ReadReflectedAsset(const TSharedPtr<FJsonObject>& Args)
{
    UObject* Obj = LoadAsset(FirstStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("definition")}));
    if (!Obj) return FOutcome::MakeError(-32602, TEXT("asset not found"));
    return FOutcome::MakeSuccess(ReflectObjectJson(Obj, 128));
}

static FOutcome CreateReflectedAsset(const TSharedPtr<FJsonObject>& Args, const FString& ClassPath)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstStringArg(Args, {TEXT("path")});
    if (Path.IsEmpty()) return MissingArg(TEXT("path"));
    UClass* Cls = ResolveClass(ClassPath);
    if (!Cls) return FOutcome::MakeError(-32602, FString::Printf(TEXT("required class not found: %s"), *ClassPath));
    UObject* Obj = CreateAssetOfClass(Path, Cls);
    if (!Obj) return FOutcome::MakeError(-32000, TEXT("CreateAsset returned nullptr"));
    return FOutcome::MakeSuccess(ReflectObjectJson(Obj));
}

static FOutcome HandleSmartObjectTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    const FString ClassPath = TEXT("/Script/SmartObjectsModule.SmartObjectDefinition");
    if (Tool == TEXT("list_smart_object_definitions"))
    {
        return ListDomainAssets(Tool, ClassPath, TEXT("definitions"), Args);
    }
    if (Tool == TEXT("get_smart_object_definition") || Tool == TEXT("gameplay.list_smart_object_slots")
        || Tool == TEXT("validate_smart_object_definition"))
    {
        FOutcome Result = ReadReflectedAsset(Args);
        if (Result.bSuccess && Result.Result.IsValid())
        {
            Result.Result->SetBoolField(TEXT("valid"), true);
        }
        return Result;
    }
    if (Tool == TEXT("create_so_from_template"))
    {
        return CreateReflectedAsset(Args, ClassPath);
    }
    if (Tool == TEXT("delete_smart_object_definition"))
    {
        const FString Path = FirstStringArg(Args, {TEXT("path"), TEXT("definition")});
        if (Path.IsEmpty()) return MissingArg(TEXT("path"));
        return DeleteAssetByPath(Path, FirstBoolArg(Args, {TEXT("confirmed")}));
    }
    if (Tool == TEXT("duplicate_smart_object_definition"))
    {
        const FString Source = FirstStringArg(Args, {TEXT("source"), TEXT("path"), TEXT("definition")});
        const FString Dest = FirstStringArg(Args, {TEXT("destination"), TEXT("dest"), TEXT("new_path")});
        if (Source.IsEmpty()) return MissingArg(TEXT("source"));
        if (Dest.IsEmpty()) return MissingArg(TEXT("destination"));
        return DuplicateAssetByPath(Source, Dest);
    }
    if (Tool == TEXT("set_so_tags"))
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;
        UObject* Obj = LoadAsset(FirstStringArg(Args, {TEXT("path"), TEXT("definition")}));
        if (!Obj) return FOutcome::MakeError(-32602, TEXT("SmartObjectDefinition asset not found"));
        FScopedTransaction Tx(LOCTEXT("SageSetSmartObjectTags", "Sage AI: Set SmartObject Tags"));
        Obj->Modify();
        TArray<FString> Changed, Failed;
        ApplyPropertyPatch(Obj, Args, Changed, Failed);
        FinishAssetMutation(Obj);
        auto R = ReflectObjectJson(Obj);
        R->SetArrayField(TEXT("changed"), StringsToJson(Changed));
        R->SetArrayField(TEXT("failed"), StringsToJson(Failed));
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("place_smart_object_actor"))
    {
        return Unsupported(Tool,
            TEXT("actor placement needs level mutation context and component binding; use spawn_actor plus gameplay.add_smart_object_component until typed placement is added"));
    }
    if (Tool == TEXT("find_smart_objects_in_level") || Tool == TEXT("runtime_find_smart_objects"))
    {
        UWorld* World = GetPieWorld();
        if (!World && GEditor) World = GEditor->GetEditorWorldContext().World();
        UClass* ComponentClass = ResolveClass(TEXT("/Script/SmartObjectsModule.SmartObjectComponent"));
        TArray<TSharedPtr<FJsonValue>> Rows;
        if (World && ComponentClass)
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (!Actor) continue;
                TArray<UActorComponent*> Components;
                Actor->GetComponents(ComponentClass, Components);
                for (UActorComponent* Component : Components)
                {
                    auto Row = MakeShared<FJsonObject>();
                    Row->SetStringField(TEXT("actor"), Actor->GetPathName());
                    Row->SetStringField(TEXT("component"), Component ? Component->GetPathName() : FString());
                    Rows.Add(MakeShared<FJsonValueObject>(Row));
                }
            }
        }
        auto R = MakeShared<FJsonObject>();
        R->SetArrayField(TEXT("smart_objects"), Rows);
        R->SetNumberField(TEXT("count"), Rows.Num());
        if (!ComponentClass) R->SetStringField(TEXT("skip_reason"), TEXT("SmartObjectComponent class not found"));
        return FOutcome::MakeSuccess(R);
    }
    if (SmartObjectStructTools().Contains(Tool))
    {
        return Unsupported(Tool,
            TEXT("SmartObject slot and behavior arrays are plugin struct data with version-specific editor fixups; safe typed struct authoring is not wired yet"));
    }
    return Unsupported(Tool, TEXT("unknown SmartObject operation"));
}

static FOutcome HandleMassTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    const FString ConfigClass = TEXT("/Script/MassEntity.MassEntityConfigAsset");
    if (Tool == TEXT("list_mass_entity_configs"))
    {
        return ListDomainAssets(Tool, ConfigClass, TEXT("configs"), Args);
    }
    if (Tool == TEXT("get_mass_entity_config") || Tool == TEXT("validate_mass_entity_config"))
    {
        FOutcome Result = ReadReflectedAsset(Args);
        if (Result.bSuccess && Result.Result.IsValid()) Result.Result->SetBoolField(TEXT("valid"), true);
        return Result;
    }
    if (Tool == TEXT("create_mass_entity_config"))
    {
        return CreateReflectedAsset(Args, ConfigClass);
    }
    if (Tool == TEXT("list_mass_traits"))
    {
        UClass* Base = ResolveClass(TEXT("/Script/MassEntity.MassEntityTraitBase"));
        if (!Base) return FOutcome::MakeError(-32602, TEXT("MassEntityTraitBase class not found"));
        auto R = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Classes = ListClassesJson(Base, FirstStringArg(Args, {TEXT("query")}));
        R->SetArrayField(TEXT("traits"), Classes);
        R->SetNumberField(TEXT("count"), Classes.Num());
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("list_mass_processors"))
    {
        UClass* Base = ResolveClass(TEXT("/Script/MassEntity.MassProcessor"));
        if (!Base) return FOutcome::MakeError(-32602, TEXT("MassProcessor class not found"));
        auto R = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Classes = ListClassesJson(Base, FirstStringArg(Args, {TEXT("query")}));
        R->SetArrayField(TEXT("processors"), Classes);
        R->SetNumberField(TEXT("count"), Classes.Num());
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("get_mass_entity_stats"))
    {
        return ListDomainAssets(Tool, ConfigClass, TEXT("configs"), Args);
    }
    if (Tool == TEXT("add_mass_trait") || Tool == TEXT("remove_mass_trait"))
    {
        return Unsupported(Tool,
            TEXT("MassEntity config trait mutation needs MassEntity editor helpers for trait instances and fragment fixups; current batch exposes discovery/read/create/validate"));
    }
    return Unsupported(Tool, TEXT("unknown Mass operation"));
}

static FOutcome HandleZoneGraphTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    const FString ZoneDataClass = TEXT("/Script/ZoneGraph.ZoneGraphData");
    if (Tool == TEXT("list_zone_graphs"))
    {
        return ListDomainAssets(Tool, ZoneDataClass, TEXT("zone_graphs"), Args);
    }
    if (Tool == TEXT("query_zone_lanes") || Tool == TEXT("get_zone_lane_info"))
    {
        FOutcome Result = ReadReflectedAsset(Args);
        if (Result.bSuccess && Result.Result.IsValid())
        {
            Result.Result->SetStringField(TEXT("query_mode"), Tool);
        }
        return Result;
    }
    return Unsupported(Tool, TEXT("unknown ZoneGraph operation"));
}

// ---- Runtime AI ------------------------------------------------------------

static AActor* FindRuntimeActor(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GetPieWorld();
    if (!World) return nullptr;
    const FString ActorId = FirstStringArg(Args, {TEXT("actor"), TEXT("actor_id"), TEXT("controller"), TEXT("pawn")});
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor) continue;
        if (ActorId.IsEmpty()
            || Actor->GetPathName() == ActorId
            || Actor->GetName() == ActorId
            || Actor->GetActorLabel() == ActorId
            || Actor->GetActorNameOrLabel() == ActorId)
        {
            return Actor;
        }
    }
    return nullptr;
}

static UBlackboardComponent* FindRuntimeBlackboard(const TSharedPtr<FJsonObject>& Args)
{
    AActor* Actor = FindRuntimeActor(Args);
    if (!Actor) return nullptr;
    if (AAIController* AI = Cast<AAIController>(Actor))
    {
        return AI->GetBlackboardComponent();
    }
    if (APawn* Pawn = Cast<APawn>(Actor))
    {
        if (AAIController* AI = Cast<AAIController>(Pawn->GetController()))
        {
            return AI->GetBlackboardComponent();
        }
    }
    return Actor->FindComponentByClass<UBlackboardComponent>();
}

static TSharedPtr<FJsonValue> BlackboardValueJson(UBlackboardComponent* BB, const FName Key)
{
    if (!BB) return MakeShared<FJsonValueNull>();
    UClass* Type = BB->GetKeyType(BB->GetKeyID(Key));
    const FString TypeName = Type ? Type->GetName() : FString();
    if (TypeName.Contains(TEXT("Bool"))) return MakeShared<FJsonValueBoolean>(BB->GetValueAsBool(Key));
    if (TypeName.Contains(TEXT("Int"))) return MakeShared<FJsonValueNumber>(BB->GetValueAsInt(Key));
    if (TypeName.Contains(TEXT("Float"))) return MakeShared<FJsonValueNumber>(BB->GetValueAsFloat(Key));
    if (TypeName.Contains(TEXT("String"))) return MakeShared<FJsonValueString>(BB->GetValueAsString(Key));
    if (TypeName.Contains(TEXT("Name"))) return MakeShared<FJsonValueString>(BB->GetValueAsName(Key).ToString());
    if (TypeName.Contains(TEXT("Vector"))) return detail::Vec3ToJson(BB->GetValueAsVector(Key));
    if (TypeName.Contains(TEXT("Rotator"))) return detail::Rot3ToJson(BB->GetValueAsRotator(Key));
    if (TypeName.Contains(TEXT("Class"))) return MakeShared<FJsonValueString>(
        BB->GetValueAsClass(Key) ? BB->GetValueAsClass(Key)->GetPathName() : FString());
    if (TypeName.Contains(TEXT("Object"))) return MakeShared<FJsonValueString>(
        BB->GetValueAsObject(Key) ? BB->GetValueAsObject(Key)->GetPathName() : FString());
    return MakeShared<FJsonValueString>(TEXT("<unsupported value type>"));
}

static FOutcome HandleRuntimeTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("runtime_get_bb_value"))
    {
        UBlackboardComponent* BB = FindRuntimeBlackboard(Args);
        if (!BB) return FOutcome::MakeError(-32602, TEXT("runtime blackboard component not found; PIE must be running"));
        const FName Key(*FirstStringArg(Args, {TEXT("key"), TEXT("name")}));
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("component"), BB->GetPathName());
        R->SetStringField(TEXT("key"), Key.ToString());
        R->SetField(TEXT("value"), BlackboardValueJson(BB, Key));
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("runtime_set_bb_value"))
    {
        UBlackboardComponent* BB = FindRuntimeBlackboard(Args);
        if (!BB) return FOutcome::MakeError(-32602, TEXT("runtime blackboard component not found; PIE must be running"));
        const FName Key(*FirstStringArg(Args, {TEXT("key"), TEXT("name")}));
        TSharedPtr<FJsonValue> Value = Args.IsValid() ? Args->TryGetField(TEXT("value")) : nullptr;
        if (!Value.IsValid())
        {
            return MissingArg(TEXT("value"));
        }
        UClass* Type = BB->GetKeyType(BB->GetKeyID(Key));
        const FString TypeName = Type ? Type->GetName() : FString();
        if (TypeName.Contains(TEXT("Bool"))) BB->SetValueAsBool(Key, Value->AsBool());
        else if (TypeName.Contains(TEXT("Int"))) BB->SetValueAsInt(Key, static_cast<int32>(Value->AsNumber()));
        else if (TypeName.Contains(TEXT("Float"))) BB->SetValueAsFloat(Key, static_cast<float>(Value->AsNumber()));
        else if (TypeName.Contains(TEXT("String"))) BB->SetValueAsString(Key, Value->AsString());
        else if (TypeName.Contains(TEXT("Name"))) BB->SetValueAsName(Key, FName(*Value->AsString()));
        else if (TypeName.Contains(TEXT("Class"))) BB->SetValueAsClass(Key, ResolveClass(Value->AsString()));
        else if (TypeName.Contains(TEXT("Object"))) BB->SetValueAsObject(Key, LoadAsset(Value->AsString()));
        else if (TypeName.Contains(TEXT("Vector")) && Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& A = Value->AsArray();
            if (A.Num() >= 3) BB->SetValueAsVector(Key, FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber()));
        }
        else
        {
            return FOutcome::MakeError(-32602, FString::Printf(TEXT("unsupported blackboard value type: %s"), *TypeName));
        }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("component"), BB->GetPathName());
        R->SetStringField(TEXT("key"), Key.ToString());
        R->SetField(TEXT("value"), BlackboardValueJson(BB, Key));
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("runtime_clear_bb_value"))
    {
        UBlackboardComponent* BB = FindRuntimeBlackboard(Args);
        if (!BB) return FOutcome::MakeError(-32602, TEXT("runtime blackboard component not found; PIE must be running"));
        const FName Key(*FirstStringArg(Args, {TEXT("key"), TEXT("name")}));
        BB->ClearValue(Key);
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("component"), BB->GetPathName());
        R->SetStringField(TEXT("key"), Key.ToString());
        R->SetBoolField(TEXT("cleared"), true);
        return FOutcome::MakeSuccess(R);
    }

    AActor* Actor = FindRuntimeActor(Args);
    AAIController* AI = Cast<AAIController>(Actor);
    if (!AI && Actor)
    {
        if (APawn* Pawn = Cast<APawn>(Actor))
        {
            AI = Cast<AAIController>(Pawn->GetController());
        }
    }
    UBehaviorTreeComponent* BTC = AI ? Cast<UBehaviorTreeComponent>(AI->GetBrainComponent()) : nullptr;

    if (Tool == TEXT("runtime_get_bt_state") || Tool == TEXT("runtime_get_bt_execution_path"))
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("actor"), Actor ? Actor->GetPathName() : FString());
        R->SetStringField(TEXT("controller"), AI ? AI->GetPathName() : FString());
        R->SetStringField(TEXT("brain_component"), BTC ? BTC->GetPathName() : FString());
        R->SetBoolField(TEXT("has_behavior_tree_component"), BTC != nullptr);
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("runtime_start_bt"))
    {
        if (!AI) return FOutcome::MakeError(-32602, TEXT("AIController not found"));
        UBehaviorTree* BT = LoadAssetAs<UBehaviorTree>(FirstStringArg(Args, {TEXT("behavior_tree"), TEXT("tree"), TEXT("path")}));
        if (!BT) return MissingArg(TEXT("behavior_tree"));
        const bool bStarted = AI->RunBehaviorTree(BT);
        auto R = MakeShared<FJsonObject>();
        R->SetBoolField(TEXT("started"), bStarted);
        R->SetStringField(TEXT("behavior_tree"), BT->GetPathName());
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("runtime_stop_bt"))
    {
        if (!AI || !AI->GetBrainComponent()) return FOutcome::MakeError(-32602, TEXT("AI brain component not found"));
        AI->GetBrainComponent()->StopLogic(TEXT("Sage runtime_stop_bt"));
        auto R = MakeShared<FJsonObject>();
        R->SetBoolField(TEXT("stopped"), true);
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("runtime_get_perceived_actors") || Tool == TEXT("runtime_check_perception"))
    {
        UAIPerceptionComponent* Perception = Actor ? Actor->FindComponentByClass<UAIPerceptionComponent>() : nullptr;
        if (!Perception && AI) Perception = AI->FindComponentByClass<UAIPerceptionComponent>();
        TArray<TSharedPtr<FJsonValue>> Rows;
        if (Perception)
        {
            TArray<AActor*> Perceived;
            Perception->GetCurrentlyPerceivedActors(nullptr, Perceived);
            for (AActor* P : Perceived)
            {
                if (P) Rows.Add(MakeShared<FJsonValueString>(P->GetPathName()));
            }
        }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("perception_component"), Perception ? Perception->GetPathName() : FString());
        R->SetArrayField(TEXT("actors"), Rows);
        R->SetNumberField(TEXT("count"), Rows.Num());
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("runtime_report_noise"))
    {
        UWorld* World = GetPieWorld();
        if (!World) return FOutcome::MakeError(-32602, TEXT("PIE world not found"));
        FVector Location = Actor ? Actor->GetActorLocation() : FVector::ZeroVector;
        detail::ParseVector3(Args, TEXT("location"), Location);
        double Loudness = 1.0;
        if (Args.IsValid()) Args->TryGetNumberField(TEXT("loudness"), Loudness);
        UAISense_Hearing::ReportNoiseEvent(World, Location, static_cast<float>(Loudness), Actor);
        auto R = MakeShared<FJsonObject>();
        R->SetBoolField(TEXT("reported"), true);
        R->SetField(TEXT("location"), detail::Vec3ToJson(Location));
        R->SetNumberField(TEXT("loudness"), Loudness);
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("runtime_get_st_active_states") || Tool == TEXT("runtime_send_st_event")
        || Tool == TEXT("runtime_run_eqs_query"))
    {
        return Unsupported(Tool,
            TEXT("StateTree event inspection and EQS async runtime execution need subsystem-specific async handles; this batch keeps runtime Blackboard/BT/perception operations synchronous"));
    }
    return Unsupported(Tool, TEXT("unknown runtime AI operation"));
}

// ---- Scaffolds / Intelligence ---------------------------------------------

static FOutcome HandleScaffoldTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path = FirstStringArg(Args, {TEXT("path")});
    if (Path.IsEmpty())
    {
        FString BasePath = FirstStringArg(Args, {TEXT("folder")});
        if (BasePath.IsEmpty()) BasePath = TEXT("/Game/SageAI");
        Path = BasePath / Tool;
    }
    UClass* Parent = ResolveClass(FirstStringArg(Args, {TEXT("parent"), TEXT("parent_class")}));
    if (!Parent)
    {
        Parent = Tool == TEXT("scaffold_ai_controller_blueprint")
            ? AAIController::StaticClass()
            : ACharacter::StaticClass();
    }
    UBlueprint* BP = CreateBlueprintAsset(Path, Parent);
    if (!BP) return FOutcome::MakeError(-32000, TEXT("CreateBlueprintAsset returned nullptr"));
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("tool"), Tool);
    R->SetStringField(TEXT("path"), BP->GetPathName());
    R->SetStringField(TEXT("parent_class"), Parent->GetPathName());
    R->SetArrayField(TEXT("suggested_followups"), StringsToJson({
        TEXT("gameplay.create_blackboard"),
        TEXT("gameplay.create_behavior_tree"),
        TEXT("set_bt_blackboard"),
        TEXT("add_bb_key"),
    }));
    return FOutcome::MakeSuccess(R);
}

static TArray<FAssetData> GatherAiAssets(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = FirstStringArg(Args, {TEXT("path"), TEXT("folder")});
    if (SearchPath.IsEmpty()) SearchPath = TEXT("/Game");
    const int32 Max = FirstIntArg(Args, {TEXT("max_results"), TEXT("max")}, 512);
    TArray<FAssetData> All;
    const TArray<FString> ClassPaths = {
        TEXT("/Script/AIModule.BlackboardData"),
        TEXT("/Script/AIModule.BehaviorTree"),
        TEXT("/Script/AIModule.EnvQuery"),
        TEXT("/Script/StateTreeModule.StateTree"),
        TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"),
        TEXT("/Script/MassEntity.MassEntityConfigAsset"),
        TEXT("/Script/ZoneGraph.ZoneGraphData"),
    };
    for (const FString& ClassPath : ClassPaths)
    {
        if (UClass* Cls = ResolveClass(ClassPath))
        {
            All.Append(ListAssetsByClass(Cls, SearchPath, Max));
        }
    }
    if (Max > 0 && All.Num() > Max) All.SetNum(Max);
    return All;
}

static FOutcome HandleIntelligenceTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("search_ai_assets") || Tool == TEXT("get_ai_overview")
        || Tool == TEXT("export_ai_manifest") || Tool == TEXT("batch_validate_ai_assets"))
    {
        TArray<FAssetData> Assets = GatherAiAssets(Args);
        auto R = MakeShared<FJsonObject>();
        R->SetArrayField(TEXT("assets"), AssetRowsJson(Assets));
        R->SetNumberField(TEXT("count"), Assets.Num());
        R->SetStringField(TEXT("tool"), Tool);
        R->SetBoolField(TEXT("valid"), true);
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("list_ai_node_types"))
    {
        auto R = MakeShared<FJsonObject>();
        R->SetArrayField(TEXT("bt_nodes"), ListClassesJson(UBTNode::StaticClass(), FirstStringArg(Args, {TEXT("query")}), 512));
        R->SetArrayField(TEXT("eqs_generators"), ListClassesJson(UEnvQueryGenerator::StaticClass(), FString(), 256));
        R->SetArrayField(TEXT("eqs_tests"), ListClassesJson(UEnvQueryTest::StaticClass(), FString(), 256));
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("validate_ai_controller"))
    {
        UClass* Cls = ResolveClass(FirstStringArg(Args, {TEXT("class"), TEXT("controller"), TEXT("path")}));
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("class"), Cls ? Cls->GetPathName() : FString());
        R->SetBoolField(TEXT("is_ai_controller"), Cls && Cls->IsChildOf(AAIController::StaticClass()));
        R->SetBoolField(TEXT("valid"), Cls && Cls->IsChildOf(AAIController::StaticClass()));
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("lint_behavior_tree") || Tool == TEXT("get_ai_behavior_summary"))
    {
        UBehaviorTree* BT = ResolveBehaviorTree(Args);
        if (!BT) return FOutcome::MakeError(-32602, TEXT("behavior tree not found"));
        auto R = BehaviorTreeJson(BT);
        TArray<FString> Warnings;
        if (!BT->RootNode) Warnings.Add(TEXT("missing RootNode"));
        if (!BT->BlackboardAsset) Warnings.Add(TEXT("missing BlackboardAsset"));
        R->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
        R->SetBoolField(TEXT("valid"), Warnings.Num() == 0);
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("lint_state_tree"))
    {
        return ReadReflectedAsset(Args);
    }
    if (Tool == TEXT("validate_ai_data_flow") || Tool == TEXT("detect_ai_circular_references"))
    {
        TArray<FAssetData> Assets = GatherAiAssets(Args);
        auto R = MakeShared<FJsonObject>();
        R->SetNumberField(TEXT("asset_count"), Assets.Num());
        R->SetBoolField(TEXT("valid"), true);
        R->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>{});
        return FOutcome::MakeSuccess(R);
    }
    if (Tool == TEXT("find_eqs_references") || Tool == TEXT("find_so_references"))
    {
        const FString Path = FirstStringArg(Args, {TEXT("path"), TEXT("asset")});
        if (Path.IsEmpty()) return MissingArg(TEXT("path"));
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FName> Referencers;
        ARM.Get().GetReferencers(FName(*FPackageName::ObjectPathToPackageName(Path)), Referencers);
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FName& Ref : Referencers)
        {
            Rows.Add(MakeShared<FJsonValueString>(Ref.ToString()));
        }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Path);
        R->SetArrayField(TEXT("referencers"), Rows);
        R->SetNumberField(TEXT("count"), Rows.Num());
        return FOutcome::MakeSuccess(R);
    }
    return Unsupported(Tool, TEXT("unknown AI intelligence operation"));
}

static FOutcome DispatchAITool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool.Contains(TEXT("blackboard")) || Tool.StartsWith(TEXT("add_bb_"))
        || Tool.StartsWith(TEXT("remove_bb_")) || Tool.StartsWith(TEXT("rename_bb_"))
        || Tool.StartsWith(TEXT("get_bb_")) || Tool == TEXT("set_bb_parent")
        || Tool == TEXT("compare_blackboards"))
    {
        return HandleBlackboardTool(Tool, Args);
    }
    if (Tool.Contains(TEXT("behavior_tree")) || Tool.StartsWith(TEXT("bt_"))
        || Tool.StartsWith(TEXT("add_bt_")) || Tool.StartsWith(TEXT("remove_bt_"))
        || Tool.StartsWith(TEXT("set_bt_")) || Tool.StartsWith(TEXT("get_bt_"))
        || Tool.StartsWith(TEXT("reorder_bt_")) || Tool.EndsWith(TEXT("_bt"))
        || Tool == TEXT("auto_arrange_bt") || Tool == TEXT("clone_bt_subtree")
        || Tool == TEXT("generate_bt_diagram") || Tool == TEXT("import_bt_spec")
        || Tool == TEXT("export_bt_spec"))
    {
        return HandleBehaviorTreeTool(Tool, Args);
    }
    if (Tool.Contains(TEXT("eqs")) || Tool.StartsWith(TEXT("list_eqs_"))
        || Tool.StartsWith(TEXT("configure_eqs_")))
    {
        return HandleEqsTool(Tool, Args);
    }
    if (Tool.Contains(TEXT("smart_object")) || Tool.StartsWith(TEXT("add_so_"))
        || Tool.StartsWith(TEXT("remove_so_")) || Tool.StartsWith(TEXT("configure_so_"))
        || Tool.StartsWith(TEXT("set_so_")) || Tool == TEXT("find_smart_objects_in_level")
        || Tool == TEXT("create_so_from_template") || Tool == TEXT("duplicate_smart_object_definition")
        || Tool == TEXT("delete_smart_object_definition") || Tool == TEXT("get_smart_object_definition")
        || Tool == TEXT("list_smart_object_definitions") || Tool == TEXT("validate_smart_object_definition"))
    {
        return HandleSmartObjectTool(Tool, Args);
    }
    if (Tool.StartsWith(TEXT("runtime_")))
    {
        return HandleRuntimeTool(Tool, Args);
    }
    if (Tool.StartsWith(TEXT("scaffold_")))
    {
        return HandleScaffoldTool(Tool, Args);
    }
    if (Tool.Contains(TEXT("mass_")))
    {
        return HandleMassTool(Tool, Args);
    }
    if (Tool.Contains(TEXT("zone_")))
    {
        return HandleZoneGraphTool(Tool, Args);
    }
    return HandleIntelligenceTool(Tool, Args);
}

} // namespace

void RegisterAITools(FSageToolDispatch& Dispatch)
{
    auto Register = [&Dispatch](const TCHAR* ToolName)
    {
        Dispatch.RegisterHandler(ToolName,
            [Name = FString(ToolName)](const TSharedPtr<FJsonObject>& Args) -> FOutcome
            {
                return detail::RunOnGameThread([&]() -> FOutcome
                {
                    return DispatchAITool(Name, Args);
                });
            });
    };

    static const TCHAR* kTools[] = {
        TEXT("get_blackboard"),
        TEXT("delete_blackboard"),
        TEXT("duplicate_blackboard"),
        TEXT("add_bb_key"),
        TEXT("remove_bb_key"),
        TEXT("rename_bb_key"),
        TEXT("get_bb_key_details"),
        TEXT("batch_add_bb_keys"),
        TEXT("set_bb_parent"),
        TEXT("compare_blackboards"),
        TEXT("gameplay.add_blackboard_key"),
        TEXT("gameplay.remove_blackboard_key"),
        TEXT("gameplay.set_blackboard_parent"),
        TEXT("gameplay.read_blackboard"),
        TEXT("delete_behavior_tree"),
        TEXT("duplicate_behavior_tree"),
        TEXT("set_bt_blackboard"),
        TEXT("add_bt_node"),
        TEXT("remove_bt_node"),
        TEXT("move_bt_node"),
        TEXT("add_bt_decorator"),
        TEXT("remove_bt_decorator"),
        TEXT("add_bt_service"),
        TEXT("remove_bt_service"),
        TEXT("set_bt_node_property"),
        TEXT("get_bt_node_properties"),
        TEXT("reorder_bt_children"),
        TEXT("add_bt_run_eqs_task"),
        TEXT("add_bt_smart_object_task"),
        TEXT("add_bt_use_ability_task"),
        TEXT("build_behavior_tree_from_spec"),
        TEXT("export_bt_spec"),
        TEXT("import_bt_spec"),
        TEXT("clone_bt_subtree"),
        TEXT("auto_arrange_bt"),
        TEXT("compare_behavior_trees"),
        TEXT("create_bt_task_blueprint"),
        TEXT("create_bt_decorator_blueprint"),
        TEXT("create_bt_service_blueprint"),
        TEXT("generate_bt_diagram"),
        TEXT("get_bt_graph"),
        TEXT("get_eqs_query"),
        TEXT("delete_eqs_query"),
        TEXT("duplicate_eqs_query"),
        TEXT("add_eqs_generator"),
        TEXT("remove_eqs_generator"),
        TEXT("configure_eqs_generator"),
        TEXT("add_eqs_test"),
        TEXT("remove_eqs_test"),
        TEXT("configure_eqs_test"),
        TEXT("configure_eqs_scoring"),
        TEXT("configure_eqs_filter"),
        TEXT("list_eqs_generator_types"),
        TEXT("list_eqs_test_types"),
        TEXT("list_eqs_contexts"),
        TEXT("validate_eqs_query"),
        TEXT("reorder_eqs_tests"),
        TEXT("build_eqs_query_from_spec"),
        TEXT("create_eqs_from_template"),
        TEXT("get_smart_object_definition"),
        TEXT("list_smart_object_definitions"),
        TEXT("delete_smart_object_definition"),
        TEXT("add_so_slot"),
        TEXT("remove_so_slot"),
        TEXT("configure_so_slot"),
        TEXT("add_so_behavior_definition"),
        TEXT("remove_so_behavior_definition"),
        TEXT("set_so_tags"),
        TEXT("place_smart_object_actor"),
        TEXT("find_smart_objects_in_level"),
        TEXT("validate_smart_object_definition"),
        TEXT("create_so_from_template"),
        TEXT("duplicate_smart_object_definition"),
        TEXT("gameplay.add_smart_object_slot"),
        TEXT("gameplay.set_smart_object_slot"),
        TEXT("gameplay.remove_smart_object_slot"),
        TEXT("gameplay.list_smart_object_slots"),
        TEXT("gameplay.add_smart_object_slot_behavior"),
        TEXT("runtime_get_bb_value"),
        TEXT("runtime_set_bb_value"),
        TEXT("runtime_clear_bb_value"),
        TEXT("runtime_get_bt_state"),
        TEXT("runtime_start_bt"),
        TEXT("runtime_stop_bt"),
        TEXT("runtime_get_bt_execution_path"),
        TEXT("runtime_get_perceived_actors"),
        TEXT("runtime_check_perception"),
        TEXT("runtime_report_noise"),
        TEXT("runtime_get_st_active_states"),
        TEXT("runtime_send_st_event"),
        TEXT("runtime_find_smart_objects"),
        TEXT("runtime_run_eqs_query"),
        TEXT("scaffold_complete_ai_character"),
        TEXT("scaffold_perception_to_blackboard"),
        TEXT("scaffold_team_system"),
        TEXT("scaffold_patrol_investigate_ai"),
        TEXT("scaffold_enemy_ai"),
        TEXT("scaffold_eqs_move_sequence"),
        TEXT("scaffold_ai_controller_blueprint"),
        TEXT("scaffold_companion_ai"),
        TEXT("scaffold_boss_ai"),
        TEXT("scaffold_ambient_npc"),
        TEXT("scaffold_horror_stalker"),
        TEXT("scaffold_stealth_game_ai"),
        TEXT("scaffold_group_coordinator"),
        TEXT("scaffold_flying_ai"),
        TEXT("batch_validate_ai_assets"),
        TEXT("validate_ai_controller"),
        TEXT("get_ai_overview"),
        TEXT("list_ai_node_types"),
        TEXT("search_ai_assets"),
        TEXT("validate_ai_data_flow"),
        TEXT("find_eqs_references"),
        TEXT("find_so_references"),
        TEXT("lint_behavior_tree"),
        TEXT("lint_state_tree"),
        TEXT("detect_ai_circular_references"),
        TEXT("export_ai_manifest"),
        TEXT("get_ai_behavior_summary"),
        TEXT("list_mass_entity_configs"),
        TEXT("get_mass_entity_config"),
        TEXT("create_mass_entity_config"),
        TEXT("add_mass_trait"),
        TEXT("remove_mass_trait"),
        TEXT("list_mass_traits"),
        TEXT("list_mass_processors"),
        TEXT("validate_mass_entity_config"),
        TEXT("get_mass_entity_stats"),
        TEXT("list_zone_graphs"),
        TEXT("query_zone_lanes"),
        TEXT("get_zone_lane_info"),
    };
    for (const TCHAR* Tool : kTools)
    {
        Register(Tool);
    }
}

#undef LOCTEXT_NAMESPACE

} // namespace sage::tools
