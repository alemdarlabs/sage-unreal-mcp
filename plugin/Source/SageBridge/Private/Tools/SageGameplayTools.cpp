#include "Tools/SageGameplayTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "IAssetTools.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "SageGameplay"

namespace sage::tools
{
namespace
{

UWorld* GetEditorWorld()
{
    return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

// Creates a Blueprint asset from a native parent class
UBlueprint* CreateBlueprintAsset(const FString& Path, UClass* ParentClass)
{
    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd)) return nullptr;
    if (FindPackage(nullptr, *(PackagePath / AssetName))) return nullptr;

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = ParentClass;
    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* Obj = AT.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);
    return Cast<UBlueprint>(Obj);
}

// ---- gameplay.set_collision_profile ----------------------------------------

FSageToolDispatch::FOutcome SetCollisionProfileImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId, Profile;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    if (!Args->TryGetStringField(TEXT("profile"), Profile))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'profile'"));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetColProf", "Set Collision Profile"));
    TArray<UActorComponent*> Comps;
    A->GetComponents(Comps);
    int32 Changed = 0;
    for (UActorComponent* C : Comps)
    {
        if (UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(C))
        {
            PC->Modify();
            PC->SetCollisionProfileName(FName(*Profile));
            ++Changed;
        }
    }
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),        ActorId);
    R->SetStringField(TEXT("profile"),         Profile);
    R->SetNumberField(TEXT("components_set"),  Changed);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.set_simulate_physics -----------------------------------------

FSageToolDispatch::FOutcome SetSimulatePhysicsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    bool bSimulate = true;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    Args->TryGetBoolField(TEXT("simulate"), bSimulate);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetSim", "Set Simulate Physics"));
    TArray<UActorComponent*> Comps;
    A->GetComponents(Comps);
    for (UActorComponent* C : Comps)
    {
        if (UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(C))
        {
            PC->Modify();
            PC->SetSimulatePhysics(bSimulate);
        }
    }
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), ActorId);
    R->SetBoolField  (TEXT("simulate"), bSimulate);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.set_collision_enabled ----------------------------------------

FSageToolDispatch::FOutcome SetCollisionEnabledImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId, ModeStr;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    if (!Args->TryGetStringField(TEXT("mode"), ModeStr))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'mode' (NoCollision/QueryOnly/PhysicsOnly/QueryAndPhysics)"));

    ECollisionEnabled::Type Mode = ECollisionEnabled::QueryAndPhysics;
    if      (ModeStr == TEXT("NoCollision"))       Mode = ECollisionEnabled::NoCollision;
    else if (ModeStr == TEXT("QueryOnly"))         Mode = ECollisionEnabled::QueryOnly;
    else if (ModeStr == TEXT("PhysicsOnly"))       Mode = ECollisionEnabled::PhysicsOnly;
    else if (ModeStr == TEXT("QueryAndPhysics"))   Mode = ECollisionEnabled::QueryAndPhysics;
    else return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("unknown mode: %s"), *ModeStr));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetColEn", "Set Collision Enabled"));
    TArray<UActorComponent*> Comps;
    A->GetComponents(Comps);
    for (UActorComponent* C : Comps)
    {
        if (UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(C))
        {
            PC->Modify();
            PC->SetCollisionEnabled(Mode);
        }
    }
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), ActorId);
    R->SetStringField(TEXT("mode"),     ModeStr);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.set_physics_properties ---------------------------------------

FSageToolDispatch::FOutcome SetPhysicsPropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetPhys", "Set Physics Properties"));
    TArray<UActorComponent*> Comps;
    A->GetComponents(Comps);
    for (UActorComponent* C : Comps)
    {
        UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(C);
        if (!PC) continue;
        PC->Modify();
        double Val;
        if (Args->TryGetNumberField(TEXT("mass"), Val))
            PC->SetMassOverrideInKg(NAME_None, static_cast<float>(Val), true);
        if (Args->TryGetNumberField(TEXT("linear_damping"), Val))
            PC->SetLinearDamping(static_cast<float>(Val));
        if (Args->TryGetNumberField(TEXT("angular_damping"), Val))
            PC->SetAngularDamping(static_cast<float>(Val));
        bool bGrav;
        if (Args->TryGetBoolField(TEXT("gravity_enabled"), bGrav))
            PC->SetEnableGravity(bGrav);
    }
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), ActorId);
    R->SetBoolField  (TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.rebuild_navigation -------------------------------------------

FSageToolDispatch::FOutcome RebuildNavigationImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    GEditor->Exec(GEditor->GetEditorWorldContext().World(),
        TEXT("RebuildNavigation"), *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("triggered"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.get_navmesh_info ---------------------------------------------

FSageToolDispatch::FOutcome GetNavmeshInfoImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    // Find nav system via world subsystem
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("world"), World->GetName());
    R->SetBoolField  (TEXT("nav_system_present"),
        World->GetNavigationSystem() != nullptr);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.project_to_nav -----------------------------------------------

FSageToolDispatch::FOutcome ProjectToNavImpl(const TSharedPtr<FJsonObject>& Args)
{
    FVector Point = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Point);

    // Project via console command output
    GEditor->Exec(GEditor->GetEditorWorldContext().World(),
        *FString::Printf(TEXT("Navigation.ProjectPoint %g %g %g"),
            Point.X, Point.Y, Point.Z), *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("input"), {
        MakeShared<FJsonValueNumber>(Point.X),
        MakeShared<FJsonValueNumber>(Point.Y),
        MakeShared<FJsonValueNumber>(Point.Z)
    });
    R->SetStringField(TEXT("note"), TEXT("projection triggered; query nav mesh for result"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.spawn_nav_modifier -------------------------------------------

FSageToolDispatch::FOutcome SpawnNavModifierImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FVector Loc = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Loc);
    FTransform Tf(FRotator::ZeroRotator, Loc);

    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    UClass* NavModCls = FindObject<UClass>(nullptr,
        TEXT("/Script/NavigationSystem.NavModifierVolume"));
    if (!NavModCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("NavModifierVolume class not found (NavigationSystem module required)"));

    FScopedTransaction Tx(LOCTEXT("SpawnNavMod", "Spawn Nav Modifier"));
    AActor* A = GEditor->AddActor(World->GetCurrentLevel(), NavModCls, Tf);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("AddActor failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.get_navmesh_details ------------------------------------------

FSageToolDispatch::FOutcome GetNavmeshDetailsImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    // Try to find RecastNavMesh actor
    UWorld* World = GetEditorWorld();
    UClass* NavMeshCls = FindObject<UClass>(nullptr,
        TEXT("/Script/NavigationSystem.RecastNavMesh"));

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("navmesh_found"), false);
    if (NavMeshCls && World)
    {
        for (TActorIterator<AActor> It(World, NavMeshCls); It; ++It)
        {
            AActor* A = *It;
            if (!A) continue;
            R->SetBoolField  (TEXT("navmesh_found"), true);
            R->SetStringField(TEXT("actor_id"),      A->GetPathName());
            break;
        }
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.create_input_action ------------------------------------------

FSageToolDispatch::FOutcome CreateInputActionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    UClass* IACls = FindObject<UClass>(nullptr,
        TEXT("/Script/EnhancedInput.InputAction"));
    if (!IACls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("InputAction class not found (EnhancedInput plugin required)"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, IACls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), NewObj->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.create_input_mapping -----------------------------------------

FSageToolDispatch::FOutcome CreateInputMappingImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    UClass* IMCCls = FindObject<UClass>(nullptr,
        TEXT("/Script/EnhancedInput.InputMappingContext"));
    if (!IMCCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("InputMappingContext class not found (EnhancedInput plugin required)"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, IMCCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), NewObj->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.list_input_assets --------------------------------------------

FSageToolDispatch::FOutcome ListInputAssetsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARM.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = true;

    // Try to find IA and IMC classes
    auto FindByClass = [&](const FString& ClsPath) -> TArray<FAssetData>
    {
        TArray<FAssetData> Out;
        UClass* Cls = FindObject<UClass>(nullptr, *ClsPath);
        if (!Cls) return Out;
        FARFilter F = Filter;
        F.ClassPaths.Add(Cls->GetClassPathName());
        AR.GetAssets(F, Out);
        return Out;
    };

    TArray<TSharedPtr<FJsonValue>> Assets;
    auto AddAssets = [&](const TArray<FAssetData>& Arr, const FString& Type)
    {
        for (const FAssetData& D : Arr)
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"), D.AssetName.ToString());
            J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
            J->SetStringField(TEXT("type"), Type);
            Assets.Add(MakeShared<FJsonValueObject>(J));
        }
    };
    AddAssets(FindByClass(TEXT("/Script/EnhancedInput.InputAction")),         TEXT("InputAction"));
    AddAssets(FindByClass(TEXT("/Script/EnhancedInput.InputMappingContext")), TEXT("InputMappingContext"));

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("assets"), Assets);
    R->SetNumberField(TEXT("count"),  Assets.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.read_imc / list_input_mappings / add_imc_mapping / etc. ------
// These require deep reflection into InputMappingContext which holds a
// TArray<FEnhancedActionKeyMapping> — use property reflection path.

FSageToolDispatch::FOutcome ReadImcImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    // Reflect all properties
    TArray<TSharedPtr<FJsonValue>> Props;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), It->GetName());
        J->SetStringField(TEXT("type"), It->GetCPPType());
        TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(Obj, *It);
        if (Val) J->SetField(TEXT("value"), Val);
        Props.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),    Obj->GetPathName());
    R->SetStringField(TEXT("class"),   Obj->GetClass()->GetName());
    R->SetArrayField (TEXT("properties"), Props);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ListInputMappingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    return ReadImcImpl(Args);
}

FSageToolDispatch::FOutcome AddImcMappingImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("note"),
        TEXT("IMC mapping mutation requires direct FEnhancedActionKeyMapping access; "
             "use Blueprint IMC editor API for full mapping CRUD"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetMappingModifiersImpl(const TSharedPtr<FJsonObject>& Args)
{ return AddImcMappingImpl(Args); }
FSageToolDispatch::FOutcome RemoveImcMappingImpl(const TSharedPtr<FJsonObject>& Args)
{ return AddImcMappingImpl(Args); }
FSageToolDispatch::FOutcome SetImcMappingKeyImpl(const TSharedPtr<FJsonObject>& Args)
{ return AddImcMappingImpl(Args); }
FSageToolDispatch::FOutcome SetImcMappingActionImpl(const TSharedPtr<FJsonObject>& Args)
{ return AddImcMappingImpl(Args); }

// ---- gameplay.list_behavior_trees ------------------------------------------

FSageToolDispatch::FOutcome ListBehaviorTreesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARM.Get();

    UClass* BTCls = FindObject<UClass>(nullptr, TEXT("/Script/AIModule.BehaviorTree"));

    TArray<FAssetData> Assets;
    if (BTCls)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*SearchPath));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(BTCls->GetClassPathName());
        AR.GetAssets(Filter, Assets);
    }

    TArray<TSharedPtr<FJsonValue>> BTs;
    for (const FAssetData& D : Assets)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), D.AssetName.ToString());
        J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
        BTs.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("behavior_trees"), BTs);
    R->SetNumberField(TEXT("count"),          BTs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.get_behavior_tree_info ---------------------------------------

FSageToolDispatch::FOutcome GetBtInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Obj->GetPathName());
    R->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.read_behavior_tree_graph -------------------------------------

FSageToolDispatch::FOutcome ReadBtGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    return GetBtInfoImpl(Args);
}

// ---- gameplay.create_blackboard --------------------------------------------

FSageToolDispatch::FOutcome CreateBlackboardImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    UClass* BBCls = FindObject<UClass>(nullptr, TEXT("/Script/AIModule.BlackboardData"));
    if (!BBCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("BlackboardData class not found (AIModule required)"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, BBCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("BlackboardData"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.create_behavior_tree -----------------------------------------

FSageToolDispatch::FOutcome CreateBehaviorTreeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    UClass* BTCls = FindObject<UClass>(nullptr, TEXT("/Script/AIModule.BehaviorTree"));
    if (!BTCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("BehaviorTree class not found (AIModule required)"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, BTCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("BehaviorTree"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.create_eqs_query ---------------------------------------------

FSageToolDispatch::FOutcome CreateEqsQueryImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    UClass* EQSCls = FindObject<UClass>(nullptr,
        TEXT("/Script/AIModule.EnvQuery"));
    if (!EQSCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("EnvQuery class not found (AIModule required)"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, EQSCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), NewObj->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.list_eqs_queries ---------------------------------------------

FSageToolDispatch::FOutcome ListEqsQueriesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    UClass* EQSCls = FindObject<UClass>(nullptr, TEXT("/Script/AIModule.EnvQuery"));
    TArray<TSharedPtr<FJsonValue>> Queries;
    if (EQSCls)
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*SearchPath));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(EQSCls->GetClassPathName());
        TArray<FAssetData> Assets;
        ARM.Get().GetAssets(Filter, Assets);
        for (const FAssetData& D : Assets)
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"), D.AssetName.ToString());
            J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
            Queries.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("queries"), Queries);
    R->SetNumberField(TEXT("count"),   Queries.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.add_perception / configure_sense -----------------------------

FSageToolDispatch::FOutcome AddPerceptionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString BpPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), BpPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BpPath);
    R->SetStringField(TEXT("note"),
        TEXT("AIPerception component addition requires FBlueprintEditorUtils SCS path; "
             "use bp.add_component with /Script/AIModule.AIPerceptionComponent"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ConfigureSenseImpl(const TSharedPtr<FJsonObject>& Args)
{ return AddPerceptionImpl(Args); }

// ---- gameplay.create_state_tree --------------------------------------------

FSageToolDispatch::FOutcome CreateStateTreeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    UClass* STCls = FindObject<UClass>(nullptr, TEXT("/Script/StateTreeModule.StateTree"));
    if (!STCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("StateTree class not found (StateTreeModule plugin required)"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, STCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("StateTree"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.list_state_trees ---------------------------------------------

FSageToolDispatch::FOutcome ListStateTreesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    UClass* STCls = FindObject<UClass>(nullptr, TEXT("/Script/StateTreeModule.StateTree"));
    TArray<TSharedPtr<FJsonValue>> STs;
    if (STCls)
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*SearchPath));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(STCls->GetClassPathName());
        TArray<FAssetData> Assets;
        ARM.Get().GetAssets(Filter, Assets);
        for (const FAssetData& D : Assets)
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"), D.AssetName.ToString());
            J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
            STs.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("state_trees"), STs);
    R->SetNumberField(TEXT("count"),       STs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.add_state_tree_component -------------------------------------

FSageToolDispatch::FOutcome AddStateTreeComponentImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("use bp.add_component with /Script/GameplayStateTreeModule.StateTreeComponent"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.create_smart_object_def --------------------------------------

FSageToolDispatch::FOutcome CreateSmartObjectDefImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* SOCls = FindObject<UClass>(nullptr,
        TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"));
    if (!SOCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("SmartObjectDefinition not found (SmartObjectsModule required)"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, SOCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), NewObj->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.add_smart_object_component -----------------------------------

FSageToolDispatch::FOutcome AddSmartObjectComponentImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("use bp.add_component with /Script/SmartObjectsModule.SmartObjectComponent"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.inspect_pie --------------------------------------------------

FSageToolDispatch::FOutcome InspectPieImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    if (!GEditor || !GEditor->IsPlayingSessionInEditor())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("PIE not running"));

    UWorld* PieWorld = nullptr;
    for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
    {
        if (Ctx.WorldType == EWorldType::PIE)
        {
            PieWorld = Ctx.World();
            break;
        }
    }
    if (!PieWorld) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no PIE world"));

    int32 ActorCount = 0;
    for (TActorIterator<AActor> It(PieWorld); It; ++It) ++ActorCount;

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("world"),        PieWorld->GetName());
    R->SetBoolField  (TEXT("is_paused"),    PieWorld->IsPaused());
    R->SetNumberField(TEXT("actor_count"),  ActorCount);
    R->SetNumberField(TEXT("time_seconds"), PieWorld->GetTimeSeconds());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.get_pie_anim_state -------------------------------------------

FSageToolDispatch::FOutcome GetPieAnimStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), ActorId);
    R->SetStringField(TEXT("note"), TEXT("use actor's SkeletalMeshComponent in PIE for anim state"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.get_pie_anim_properties / get_pie_subsystem_state -------------

FSageToolDispatch::FOutcome GetPieAnimPropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{ return GetPieAnimStateImpl(Args); }

FSageToolDispatch::FOutcome GetPieSubsystemStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SubsystemClass;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("subsystem_class"), SubsystemClass))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'subsystem_class'"));

    UWorld* PieWorld = nullptr;
    for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        if (Ctx.WorldType == EWorldType::PIE) { PieWorld = Ctx.World(); break; }

    if (!PieWorld) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("PIE not running"));

    UClass* Cls = FindObject<UClass>(nullptr, *SubsystemClass);
    if (!Cls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("class not found: %s"), *SubsystemClass));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("subsystem_class"), SubsystemClass);
    R->SetStringField(TEXT("world"),           PieWorld->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.apply_damage_in_pie ------------------------------------------

FSageToolDispatch::FOutcome ApplyDamageInPieImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    double Amount = 10.0;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    Args->TryGetNumberField(TEXT("amount"), Amount);

    if (!GEditor || !GEditor->IsPlayingSessionInEditor())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("PIE not running"));

    // Find actor in PIE world
    UWorld* PieWorld = nullptr;
    for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        if (Ctx.WorldType == EWorldType::PIE) { PieWorld = Ctx.World(); break; }
    if (!PieWorld) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no PIE world"));

    AActor* FoundActor = nullptr;
    for (TActorIterator<AActor> It(PieWorld); It; ++It)
    {
        if ((*It)->GetName() == ActorId || (*It)->GetActorLabel() == ActorId)
        { FoundActor = *It; break; }
    }
    if (!FoundActor) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found in PIE: %s"), *ActorId));

    UGameplayStatics::ApplyDamage(FoundActor, static_cast<float>(Amount),
        nullptr, nullptr, nullptr);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), ActorId);
    R->SetNumberField(TEXT("damage"),   Amount);
    R->SetBoolField  (TEXT("applied"),  true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.create_game_mode / game_state / etc. -------------------------

static UBlueprint* CreateGameFrameworkBP(const TSharedPtr<FJsonObject>& Args,
                                          UClass* ParentClass,
                                          FSageToolDispatch::FOutcome& OutErr)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) { OutErr = Reject; return nullptr; }

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        OutErr = FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
        return nullptr;
    }
    UBlueprint* BP = CreateBlueprintAsset(Path, ParentClass);
    if (!BP)
    {
        OutErr = FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));
        return nullptr;
    }
    return BP;
}

FSageToolDispatch::FOutcome CreateGameModeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Err;
    UBlueprint* BP = CreateGameFrameworkBP(Args, AGameModeBase::StaticClass(), Err);
    if (!BP) return Err;
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  BP->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("GameModeBase"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CreateGameStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Err;
    UBlueprint* BP = CreateGameFrameworkBP(Args, AGameStateBase::StaticClass(), Err);
    if (!BP) return Err;
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  BP->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("GameStateBase"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CreatePlayerControllerImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Err;
    UBlueprint* BP = CreateGameFrameworkBP(Args, APlayerController::StaticClass(), Err);
    if (!BP) return Err;
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  BP->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("PlayerController"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CreatePlayerStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Err;
    UBlueprint* BP = CreateGameFrameworkBP(Args, APlayerState::StaticClass(), Err);
    if (!BP) return Err;
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  BP->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("PlayerState"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CreateHudImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Err;
    UBlueprint* BP = CreateGameFrameworkBP(Args, AHUD::StaticClass(), Err);
    if (!BP) return Err;
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  BP->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("HUD"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.set_world_game_mode ------------------------------------------

FSageToolDispatch::FOutcome SetWorldGameModeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("game_mode_class"), ClassPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'game_mode_class'"));

    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    UClass* GMCls = FindObject<UClass>(nullptr, *ClassPath);
    if (!GMCls) GMCls = LoadObject<UClass>(nullptr, *ClassPath);
    if (!GMCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("class not found: %s"), *ClassPath));

    FScopedTransaction Tx(LOCTEXT("SetGM", "Set World Game Mode"));
    AWorldSettings* WS = World->GetWorldSettings();
    WS->Modify();
    WS->DefaultGameMode = GMCls;
    WS->PostEditChange();
    World->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("game_mode_class"), ClassPath);
    R->SetBoolField  (TEXT("modified"),        true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.get_framework_info -------------------------------------------

FSageToolDispatch::FOutcome GetFrameworkInfoImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    UWorld* World = GetEditorWorld();
    auto R = MakeShared<FJsonObject>();
    if (World)
    {
        AWorldSettings* WS = World->GetWorldSettings();
        if (WS && WS->DefaultGameMode)
            R->SetStringField(TEXT("game_mode"), WS->DefaultGameMode->GetPathName());
        else
            R->SetStringField(TEXT("game_mode"), TEXT("<default>"));
        R->SetStringField(TEXT("world"), World->GetName());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterGameplayTools(FSageToolDispatch& Dispatch)
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

    // Physics
    Dispatch.RegisterHandler(TEXT("gameplay.set_collision_profile"),  GT(&SetCollisionProfileImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.set_simulate_physics"),   GT(&SetSimulatePhysicsImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.set_collision_enabled"),  GT(&SetCollisionEnabledImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.set_physics_properties"), GT(&SetPhysicsPropertiesImpl));
    // Navigation
    Dispatch.RegisterHandler(TEXT("gameplay.rebuild_navigation"),     GT(&RebuildNavigationImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.get_navmesh_info"),       GT(&GetNavmeshInfoImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.project_to_nav"),         GT(&ProjectToNavImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.spawn_nav_modifier"),     GT(&SpawnNavModifierImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.get_navmesh_details"),    GT(&GetNavmeshDetailsImpl));
    // Enhanced Input
    Dispatch.RegisterHandler(TEXT("gameplay.create_input_action"),    GT(&CreateInputActionImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_input_mapping"),   GT(&CreateInputMappingImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.list_input_assets"),      GT(&ListInputAssetsImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.read_imc"),               GT(&ReadImcImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.list_input_mappings"),    GT(&ListInputMappingsImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.add_imc_mapping"),        GT(&AddImcMappingImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.set_mapping_modifiers"),  GT(&SetMappingModifiersImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.remove_imc_mapping"),     GT(&RemoveImcMappingImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.set_imc_mapping_key"),    GT(&SetImcMappingKeyImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.set_imc_mapping_action"), GT(&SetImcMappingActionImpl));
    // BehaviorTree / EQS
    Dispatch.RegisterHandler(TEXT("gameplay.list_behavior_trees"),    GT(&ListBehaviorTreesImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.get_behavior_tree_info"), GT(&GetBtInfoImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.read_behavior_tree_graph"),GT(&ReadBtGraphImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_blackboard"),      GT(&CreateBlackboardImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_behavior_tree"),   GT(&CreateBehaviorTreeImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_eqs_query"),       GT(&CreateEqsQueryImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.list_eqs_queries"),       GT(&ListEqsQueriesImpl));
    // Perception
    Dispatch.RegisterHandler(TEXT("gameplay.add_perception"),         GT(&AddPerceptionImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.configure_sense"),        GT(&ConfigureSenseImpl));
    // StateTree / SmartObject
    Dispatch.RegisterHandler(TEXT("gameplay.create_state_tree"),      GT(&CreateStateTreeImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.list_state_trees"),       GT(&ListStateTreesImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.add_state_tree_component"),GT(&AddStateTreeComponentImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_smart_object_def"),GT(&CreateSmartObjectDefImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.add_smart_object_component"),GT(&AddSmartObjectComponentImpl));
    // PIE inspection
    Dispatch.RegisterHandler(TEXT("gameplay.inspect_pie"),            GT(&InspectPieImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.get_pie_anim_state"),     GT(&GetPieAnimStateImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.get_pie_anim_properties"),GT(&GetPieAnimPropertiesImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.get_pie_subsystem_state"),GT(&GetPieSubsystemStateImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.apply_damage_in_pie"),    GT(&ApplyDamageInPieImpl));
    // Game framework
    Dispatch.RegisterHandler(TEXT("gameplay.create_game_mode"),       GT(&CreateGameModeImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_game_state"),      GT(&CreateGameStateImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_player_controller"),GT(&CreatePlayerControllerImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_player_state"),    GT(&CreatePlayerStateImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.create_hud"),             GT(&CreateHudImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.set_world_game_mode"),    GT(&SetWorldGameModeImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.get_framework_info"),     GT(&GetFrameworkInfoImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
