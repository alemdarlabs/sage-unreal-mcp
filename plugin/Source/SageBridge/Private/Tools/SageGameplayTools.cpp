#include "Tools/SageGameplayTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Animation/AnimInstance.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
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
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "IAssetTools.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "ScopedTransaction.h"

// Enhanced Input — Lyra Sage Gap #10 IMC mapping CRUD.
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
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

UWorld* GetPieWorld()
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

TSharedRef<FJsonObject> ActorSummaryJson(AActor* Actor, bool bIncludeComponents, bool bIncludeActorProperties, bool bIncludeComponentProperties)
{
    auto Obj = MakeShared<FJsonObject>();
    if (!Actor)
    {
        return Obj;
    }
    Obj->SetStringField(TEXT("actor_id"), Actor->GetPathName());
    Obj->SetStringField(TEXT("path"), Actor->GetPathName());
    Obj->SetStringField(TEXT("name"), Actor->GetName());
    Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
    Obj->SetField(TEXT("location"), detail::Vec3ToJson(Actor->GetActorLocation()));
    Obj->SetStringField(TEXT("owner"), Actor->GetOwner() ? Actor->GetOwner()->GetPathName() : FString());
    Obj->SetStringField(TEXT("instigator"), Actor->GetInstigator() ? Actor->GetInstigator()->GetPathName() : FString());

    if (bIncludeActorProperties)
    {
        auto Props = MakeShared<FJsonObject>();
        int32 Count = 0;
        for (TFieldIterator<FProperty> It(Actor->GetClass()); It && Count < 128; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
            if (TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Actor, Prop))
            {
                Props->SetField(Prop->GetName(), Value);
                ++Count;
            }
        }
        Obj->SetObjectField(TEXT("properties"), Props);
        Obj->SetNumberField(TEXT("property_count"), Count);
    }

    if (bIncludeComponents)
    {
        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);
        TArray<TSharedPtr<FJsonValue>> ComponentJson;
        for (UActorComponent* Component : Components)
        {
            if (!Component) continue;
            auto C = MakeShared<FJsonObject>();
            C->SetStringField(TEXT("name"), Component->GetName());
            C->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
            C->SetStringField(TEXT("path"), Component->GetPathName());
            C->SetBoolField(TEXT("registered"), Component->IsRegistered());
            C->SetBoolField(TEXT("active"), Component->IsActive());
            if (bIncludeComponentProperties)
            {
                auto Props = MakeShared<FJsonObject>();
                int32 Count = 0;
                for (TFieldIterator<FProperty> It(Component->GetClass()); It && Count < 96; ++It)
                {
                    FProperty* Prop = *It;
                    if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
                    if (TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Component, Prop))
                    {
                        Props->SetField(Prop->GetName(), Value);
                        ++Count;
                    }
                }
                C->SetObjectField(TEXT("properties"), Props);
                C->SetNumberField(TEXT("property_count"), Count);
            }
            ComponentJson.Add(MakeShared<FJsonValueObject>(C));
        }
        Obj->SetArrayField(TEXT("components"), ComponentJson);
        Obj->SetNumberField(TEXT("component_count"), ComponentJson.Num());
    }

    return Obj;
}

bool ActorMatchesFilter(AActor* Actor, const FString& Filter, const FString& ClassFilter)
{
    if (!Actor) return false;
    if (!ClassFilter.IsEmpty())
    {
        const FString ClassPath = Actor->GetClass()->GetPathName();
        const FString ClassName = Actor->GetClass()->GetName();
        if (!ClassPath.Contains(ClassFilter) && !ClassName.Contains(ClassFilter))
        {
            return false;
        }
    }
    if (Filter.IsEmpty())
    {
        return true;
    }
    return Actor->GetPathName().Contains(Filter)
        || Actor->GetName().Contains(Filter)
        || Actor->GetActorLabel().Contains(Filter)
        || Actor->GetActorNameOrLabel().Contains(Filter)
        || Actor->GetClass()->GetName().Contains(Filter)
        || Actor->GetClass()->GetPathName().Contains(Filter);
}

AActor* FindActorInPie(UWorld* PieWorld, const FString& ActorId)
{
    if (!PieWorld || ActorId.IsEmpty()) return nullptr;
    if (AActor* Actor = detail::ResolveActor(ActorId))
    {
        return Actor;
    }
    for (TActorIterator<AActor> It(PieWorld); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor) continue;
        if (Actor->GetPathName() == ActorId
            || Actor->GetName() == ActorId
            || Actor->GetActorLabel() == ActorId
            || Actor->GetActorNameOrLabel() == ActorId)
        {
            return Actor;
        }
    }
    return nullptr;
}

USkeletalMeshComponent* FindSkeletalMeshComponent(AActor* Actor, const FString& ComponentName)
{
    if (!Actor) return nullptr;
    TArray<USkeletalMeshComponent*> Meshes;
    Actor->GetComponents<USkeletalMeshComponent>(Meshes);
    if (!ComponentName.IsEmpty())
    {
        for (USkeletalMeshComponent* Mesh : Meshes)
        {
            if (Mesh && (Mesh->GetName() == ComponentName || Mesh->GetPathName() == ComponentName))
            {
                return Mesh;
            }
        }
        return nullptr;
    }
    return Meshes.Num() > 0 ? Meshes[0] : nullptr;
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
    FVector QueryExtent(50.0, 50.0, 200.0);
    if (Args.IsValid())
    {
        detail::ParseVector3(Args, TEXT("query_extent"), QueryExtent);
        detail::ParseVector3(Args, TEXT("extent"), QueryExtent);
    }

    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("input"), {
        MakeShared<FJsonValueNumber>(Point.X),
        MakeShared<FJsonValueNumber>(Point.Y),
        MakeShared<FJsonValueNumber>(Point.Z)
    });
    R->SetField(TEXT("query_extent"), detail::Vec3ToJson(QueryExtent));

    UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    if (!NavSys)
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("reason"), TEXT("no_navigation_system"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    ANavigationData* NavData = NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
    if (!NavData)
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("reason"), TEXT("no_navmesh"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FNavLocation Projected;
    const bool bProjected = NavSys->ProjectPointToNavigation(
        Point, Projected, QueryExtent, NavData);

    R->SetBoolField(TEXT("success"), bProjected);
    R->SetStringField(TEXT("nav_data"), NavData->GetPathName());
    if (bProjected)
    {
        R->SetField(TEXT("projected_location"), detail::Vec3ToJson(Projected.Location));
        R->SetNumberField(TEXT("node_ref"), static_cast<double>(Projected.NodeRef));
    }
    else
    {
        R->SetStringField(TEXT("reason"), TEXT("projection_failed"));
    }
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

// Lyra Sage Gap #10 fix — IMC mapping CRUD (replaces previous stub batch).
// Uses the typed UInputMappingContext API directly (EnhancedInput is a hard
// Build.cs dep; ships EnabledByDefault on every UE 5.7 project).

UInputMappingContext* ResolveImc(const FString& Path, FString& OutErr)
{
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    if (!Obj)
    {
        OutErr = FString::Printf(TEXT("IMC asset not found: %s"), *Path);
        return nullptr;
    }
    UInputMappingContext* IMC = Cast<UInputMappingContext>(Obj);
    if (!IMC)
    {
        OutErr = FString::Printf(TEXT("'%s' is %s, not UInputMappingContext"),
                                  *Path, *Obj->GetClass()->GetName());
        return nullptr;
    }
    return IMC;
}

UInputAction* ResolveInputAction(const FString& Path, FString& OutErr)
{
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    if (!Obj)
    {
        OutErr = FString::Printf(TEXT("InputAction not found: %s"), *Path);
        return nullptr;
    }
    UInputAction* IA = Cast<UInputAction>(Obj);
    if (!IA)
    {
        OutErr = FString::Printf(TEXT("'%s' is %s, not UInputAction"),
                                  *Path, *Obj->GetClass()->GetName());
        return nullptr;
    }
    return IA;
}

// Resolve a short-name (e.g. "Pressed") OR a full class path
// ("/Script/EnhancedInput.InputTriggerPressed") into a UClass derived from
// `Base`. Short-names use the convention "<Base>{Name}" — UInputTriggerPressed,
// UInputModifierNegate, etc.
UClass* ResolveInputClass(const FString& Name, UClass* Base, const TCHAR* Prefix)
{
    if (Name.IsEmpty()) return nullptr;
    // Full path?
    if (Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")))
    {
        UClass* Cls = FindObject<UClass>(nullptr, *Name);
        if (!Cls) Cls = LoadObject<UClass>(nullptr, *Name);
        if (Cls && Base && Cls->IsChildOf(Base)) return Cls;
        return nullptr;
    }
    // Short-name → /Script/EnhancedInput.<Prefix><Name>
    const FString FullPath = FString::Printf(
        TEXT("/Script/EnhancedInput.%s%s"), Prefix, *Name);
    UClass* Cls = FindObject<UClass>(nullptr, *FullPath);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *FullPath);
    if (Cls && Base && Cls->IsChildOf(Base)) return Cls;
    return nullptr;
}

// Pull "Pressed", "Hold", etc. from a JSON string array argument and resolve
// each into a UClass. Unknown names are silently dropped (caller can verify
// via length comparison).
TArray<UClass*> ResolveInputClassArray(const TSharedPtr<FJsonObject>& Args,
                                        const TCHAR* Field,
                                        UClass* Base, const TCHAR* Prefix,
                                        TArray<FString>& OutSkipped)
{
    TArray<UClass*> Out;
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Args.IsValid() || !Args->TryGetArrayField(Field, Arr) || !Arr) return Out;
    for (const TSharedPtr<FJsonValue>& V : *Arr)
    {
        if (!V.IsValid() || V->Type != EJson::String) continue;
        const FString Name = V->AsString();
        UClass* Cls = ResolveInputClass(Name, Base, Prefix);
        if (Cls) Out.Add(Cls);
        else     OutSkipped.Add(Name);
    }
    return Out;
}

FSageToolDispatch::FOutcome AddImcMappingImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ActionPath, KeyName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("action"), ActionPath)
        || !Args->TryGetStringField(TEXT("key"), KeyName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'action' or 'key'"));
    }

    FString Err;
    UInputMappingContext* IMC = ResolveImc(Path, Err);
    if (!IMC) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UInputAction* IA = ResolveInputAction(ActionPath, Err);
    if (!IA)  return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    const FKey K(*KeyName);
    if (!K.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid FKey name: %s (try 'SpaceBar', "
                                 "'Gamepad_FaceButton_Bottom', 'LeftMouseButton')"),
                            *KeyName));
    }

    TArray<FString> SkippedTriggers, SkippedModifiers;
    TArray<UClass*> TriggerClasses = ResolveInputClassArray(Args, TEXT("triggers"),
        UInputTrigger::StaticClass(),  TEXT("InputTrigger"),  SkippedTriggers);
    TArray<UClass*> ModifierClasses = ResolveInputClassArray(Args, TEXT("modifiers"),
        UInputModifier::StaticClass(), TEXT("InputModifier"), SkippedModifiers);

    FScopedTransaction Tx(LOCTEXT("AddImcMapping", "Sage: Add IMC Mapping"));
    IMC->Modify();

    FEnhancedActionKeyMapping& NewMapping = IMC->MapKey(IA, K);
    for (UClass* TC : TriggerClasses)
    {
        UInputTrigger* T = NewObject<UInputTrigger>(IMC, TC, NAME_None,
            RF_Public | RF_Transactional);
        NewMapping.Triggers.Add(T);
    }
    for (UClass* MC : ModifierClasses)
    {
        UInputModifier* M = NewObject<UInputModifier>(IMC, MC, NAME_None,
            RF_Public | RF_Transactional);
        NewMapping.Modifiers.Add(M);
    }
    IMC->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"), IMC->GetPathName());
    R->SetStringField(TEXT("action"),     IA->GetPathName());
    R->SetStringField(TEXT("key"),        KeyName);
    R->SetNumberField(TEXT("trigger_count"),  TriggerClasses.Num());
    R->SetNumberField(TEXT("modifier_count"), ModifierClasses.Num());
    R->SetNumberField(TEXT("mapping_count"),  IMC->GetMappings().Num());
    // Index of the mapping just appended (for subsequent remove/update calls).
    R->SetNumberField(TEXT("mapping_index"),  IMC->GetMappings().Num() - 1);
    if (SkippedTriggers.Num() > 0 || SkippedModifiers.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> S1, S2;
        for (const FString& s : SkippedTriggers)  S1.Add(MakeShared<FJsonValueString>(s));
        for (const FString& s : SkippedModifiers) S2.Add(MakeShared<FJsonValueString>(s));
        if (S1.Num() > 0) R->SetArrayField(TEXT("skipped_triggers"),  S1);
        if (S2.Num() > 0) R->SetArrayField(TEXT("skipped_modifiers"), S2);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// Helper: walk Mappings, return the index of the entry matching {Action,Key}
// or -1 if none. Both filters optional — null Action / invalid Key skips
// that constraint.
int32 FindMappingIndex(const UInputMappingContext* IMC,
                       const UInputAction* WantAction, FKey WantKey)
{
    const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
    for (int32 i = 0; i < Mappings.Num(); ++i)
    {
        const FEnhancedActionKeyMapping& M = Mappings[i];
        if (WantAction && M.Action != WantAction) continue;
        if (WantKey.IsValid() && M.Key != WantKey) continue;
        return i;
    }
    return -1;
}

void SetImcMappingDetails(TSharedPtr<FJsonObject> R,
                          const FEnhancedActionKeyMapping& Mapping,
                          int32 Index)
{
    R->SetNumberField(TEXT("index"), Index);
    R->SetStringField(TEXT("action"),
        Mapping.Action ? Mapping.Action->GetPathName() : TEXT(""));
    R->SetStringField(TEXT("key"), Mapping.Key.GetFName().ToString());
    R->SetNumberField(TEXT("trigger_count"), Mapping.Triggers.Num());
    R->SetNumberField(TEXT("modifier_count"), Mapping.Modifiers.Num());

    TArray<TSharedPtr<FJsonValue>> Triggers;
    for (UInputTrigger* Trigger : Mapping.Triggers)
    {
        auto J = MakeShared<FJsonObject>();
        if (Trigger)
        {
            J->SetStringField(TEXT("name"), Trigger->GetName());
            J->SetStringField(TEXT("class"), Trigger->GetClass()->GetPathName());
        }
        Triggers.Add(MakeShared<FJsonValueObject>(J));
    }
    R->SetArrayField(TEXT("triggers"), Triggers);

    TArray<TSharedPtr<FJsonValue>> Modifiers;
    for (UInputModifier* Modifier : Mapping.Modifiers)
    {
        auto J = MakeShared<FJsonObject>();
        if (Modifier)
        {
            J->SetStringField(TEXT("name"), Modifier->GetName());
            J->SetStringField(TEXT("class"), Modifier->GetClass()->GetPathName());
        }
        Modifiers.Add(MakeShared<FJsonValueObject>(J));
    }
    R->SetArrayField(TEXT("modifiers"), Modifiers);
}

void CopyInstancedInputObjects(const FEnhancedActionKeyMapping& Source,
                               FEnhancedActionKeyMapping& Target,
                               UObject* Outer)
{
    Target.Triggers.Empty(Source.Triggers.Num());
    for (UInputTrigger* Trigger : Source.Triggers)
    {
        UInputTrigger* Copy = Trigger
            ? DuplicateObject<UInputTrigger>(Trigger, Outer)
            : nullptr;
        if (Copy) Copy->SetFlags(RF_Public | RF_Transactional);
        Target.Triggers.Add(Copy);
    }

    Target.Modifiers.Empty(Source.Modifiers.Num());
    for (UInputModifier* Modifier : Source.Modifiers)
    {
        UInputModifier* Copy = Modifier
            ? DuplicateObject<UInputModifier>(Modifier, Outer)
            : nullptr;
        if (Copy) Copy->SetFlags(RF_Public | RF_Transactional);
        Target.Modifiers.Add(Copy);
    }
}

int32 RemapImcMappingViaTypedApi(UInputMappingContext* IMC,
                                 int32 ExistingIndex,
                                 UInputAction* OldAction,
                                 FKey OldKey,
                                 UInputAction* NewAction,
                                 FKey NewKey)
{
    const FEnhancedActionKeyMapping Existing = IMC->GetMappings()[ExistingIndex];

    IMC->UnmapKey(OldAction, OldKey);
    FEnhancedActionKeyMapping& NewMapping = IMC->MapKey(NewAction, NewKey);
    NewMapping = Existing;
    NewMapping.Action = NewAction;
    NewMapping.Key = NewKey;
    CopyInstancedInputObjects(Existing, NewMapping, IMC);

    IMC->PostEditChange();
    IMC->MarkPackageDirty();

    return FindMappingIndex(IMC, NewAction, NewKey);
}

FSageToolDispatch::FOutcome RemoveImcMappingImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ActionPath, KeyName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("action"), ActionPath)
        || !Args->TryGetStringField(TEXT("key"), KeyName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'action' or 'key'"));
    }

    FString Err;
    UInputMappingContext* IMC = ResolveImc(Path, Err);
    if (!IMC) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UInputAction* IA = ResolveInputAction(ActionPath, Err);
    if (!IA)  return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    const FKey K(*KeyName);
    if (!K.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid FKey: %s"), *KeyName));

    const int32 Idx = FindMappingIndex(IMC, IA, K);
    if (Idx < 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("no mapping for action=%s key=%s on %s"),
                            *IA->GetName(), *KeyName, *IMC->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("RemoveImcMapping", "Sage: Remove IMC Mapping"));
    IMC->Modify();
    IMC->UnmapKey(IA, K);
    IMC->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"),    IMC->GetPathName());
    R->SetStringField(TEXT("action"),        IA->GetPathName());
    R->SetStringField(TEXT("key"),           KeyName);
    R->SetNumberField(TEXT("removed_index"), Idx);
    R->SetNumberField(TEXT("mapping_count"), IMC->GetMappings().Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// Mappings is read-only via GetMappings() — for in-place mutation we need
// the property pointer.
TArray<FEnhancedActionKeyMapping>* MutableMappings(UInputMappingContext* IMC)
{
    FProperty* Prop = IMC->GetClass()->FindPropertyByName(TEXT("Mappings"));
    FArrayProperty* Arr = CastField<FArrayProperty>(Prop);
    if (!Arr) return nullptr;
    return reinterpret_cast<TArray<FEnhancedActionKeyMapping>*>(
        Arr->ContainerPtrToValuePtr<void>(IMC));
}

FSageToolDispatch::FOutcome SetImcMappingKeyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ActionPath, OldKeyName, NewKeyName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("action"), ActionPath)
        || !Args->TryGetStringField(TEXT("old_key"), OldKeyName)
        || !Args->TryGetStringField(TEXT("new_key"), NewKeyName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'action', 'old_key' or 'new_key'"));
    }

    FString Err;
    UInputMappingContext* IMC = ResolveImc(Path, Err);
    if (!IMC) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UInputAction* IA = ResolveInputAction(ActionPath, Err);
    if (!IA)  return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    const FKey OldK(*OldKeyName), NewK(*NewKeyName);
    if (!OldK.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid old FKey: %s"), *OldKeyName));
    if (!NewK.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid new FKey: %s"), *NewKeyName));

    const int32 Idx = FindMappingIndex(IMC, IA, OldK);
    if (Idx < 0)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("no mapping for action=%s old_key=%s"),
                            *IA->GetName(), *OldKeyName));

    bool bDryRun = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);

    const FEnhancedActionKeyMapping Existing = IMC->GetMappings()[Idx];

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("asset_path"), IMC->GetPathName());
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        R->SetStringField(TEXT("planned_new_key"), NewKeyName);
        SetImcMappingDetails(R, Existing, Idx);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (OldK == NewK)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("asset_path"), IMC->GetPathName());
        R->SetStringField(TEXT("action"),     IA->GetPathName());
        R->SetStringField(TEXT("old_key"),    OldKeyName);
        R->SetStringField(TEXT("new_key"),    NewKeyName);
        R->SetNumberField(TEXT("index"),      Idx);
        R->SetBoolField  (TEXT("already"),    true);
        R->SetBoolField  (TEXT("modified"),   false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SetImcMappingKey", "Sage: Set IMC Mapping Key"));
    IMC->Modify();
    const int32 NewIdx = RemapImcMappingViaTypedApi(IMC, Idx, IA, OldK, IA, NewK);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"), IMC->GetPathName());
    R->SetStringField(TEXT("action"),     IA->GetPathName());
    R->SetStringField(TEXT("old_key"),    OldKeyName);
    R->SetStringField(TEXT("new_key"),    NewKeyName);
    R->SetNumberField(TEXT("index"),      Idx);
    R->SetNumberField(TEXT("new_index"),  NewIdx);
    R->SetBoolField  (TEXT("modified"),   true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetImcMappingActionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, OldActionPath, NewActionPath, KeyName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("old_action"), OldActionPath)
        || !Args->TryGetStringField(TEXT("new_action"), NewActionPath)
        || !Args->TryGetStringField(TEXT("key"), KeyName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'old_action', 'new_action' or 'key'"));
    }

    FString Err;
    UInputMappingContext* IMC = ResolveImc(Path, Err);
    if (!IMC)         return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UInputAction* OldIA = ResolveInputAction(OldActionPath, Err);
    if (!OldIA)       return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UInputAction* NewIA = ResolveInputAction(NewActionPath, Err);
    if (!NewIA)       return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    const FKey K(*KeyName);
    if (!K.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid FKey: %s"), *KeyName));
    const int32 Idx = FindMappingIndex(IMC, OldIA, K);
    if (Idx < 0)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("no mapping for old_action=%s key=%s"),
                            *OldIA->GetName(), *KeyName));

    bool bDryRun = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);

    const FEnhancedActionKeyMapping Existing = IMC->GetMappings()[Idx];

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("asset_path"), IMC->GetPathName());
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        R->SetStringField(TEXT("planned_new_action"), NewIA->GetPathName());
        SetImcMappingDetails(R, Existing, Idx);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (OldIA == NewIA)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("asset_path"), IMC->GetPathName());
        R->SetStringField(TEXT("old_action"), OldIA->GetPathName());
        R->SetStringField(TEXT("new_action"), NewIA->GetPathName());
        R->SetStringField(TEXT("key"),        KeyName);
        R->SetNumberField(TEXT("index"),      Idx);
        R->SetBoolField  (TEXT("already"),    true);
        R->SetBoolField  (TEXT("modified"),   false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SetImcMappingAction", "Sage: Set IMC Mapping Action"));
    IMC->Modify();
    const int32 NewIdx = RemapImcMappingViaTypedApi(IMC, Idx, OldIA, K, NewIA, K);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"), IMC->GetPathName());
    R->SetStringField(TEXT("old_action"), OldIA->GetPathName());
    R->SetStringField(TEXT("new_action"), NewIA->GetPathName());
    R->SetStringField(TEXT("key"),        KeyName);
    R->SetNumberField(TEXT("index"),      Idx);
    R->SetNumberField(TEXT("new_index"),  NewIdx);
    R->SetBoolField  (TEXT("modified"),   true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetMappingModifiersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ActionPath, KeyName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("action"), ActionPath)
        || !Args->TryGetStringField(TEXT("key"), KeyName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'action' or 'key'"));
    }

    FString Err;
    UInputMappingContext* IMC = ResolveImc(Path, Err);
    if (!IMC) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UInputAction* IA = ResolveInputAction(ActionPath, Err);
    if (!IA)  return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    const FKey K(*KeyName);

    const int32 Idx = FindMappingIndex(IMC, IA, K);
    if (Idx < 0)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("no mapping for action=%s key=%s"),
                            *IA->GetName(), *KeyName));

    auto* Mappings = MutableMappings(IMC);
    if (!Mappings) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("Mappings property reflection failed"));

    TArray<FString> SkippedTriggers, SkippedModifiers;
    TArray<UClass*> TriggerClasses = ResolveInputClassArray(Args, TEXT("triggers"),
        UInputTrigger::StaticClass(),  TEXT("InputTrigger"),  SkippedTriggers);
    TArray<UClass*> ModifierClasses = ResolveInputClassArray(Args, TEXT("modifiers"),
        UInputModifier::StaticClass(), TEXT("InputModifier"), SkippedModifiers);

    FScopedTransaction Tx(LOCTEXT("SetMappingModifiers", "Sage: Set IMC Mapping Modifiers"));
    IMC->Modify();
    FEnhancedActionKeyMapping& M = (*Mappings)[Idx];
    // Replace, not append — semantics match `set_*`.
    M.Triggers.Empty();
    M.Modifiers.Empty();
    for (UClass* TC : TriggerClasses)
        M.Triggers.Add(NewObject<UInputTrigger>(IMC, TC, NAME_None,
            RF_Public | RF_Transactional));
    for (UClass* MC : ModifierClasses)
        M.Modifiers.Add(NewObject<UInputModifier>(IMC, MC, NAME_None,
            RF_Public | RF_Transactional));
    IMC->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"), IMC->GetPathName());
    R->SetStringField(TEXT("action"),     IA->GetPathName());
    R->SetStringField(TEXT("key"),        KeyName);
    R->SetNumberField(TEXT("index"),      Idx);
    R->SetNumberField(TEXT("trigger_count"),  TriggerClasses.Num());
    R->SetNumberField(TEXT("modifier_count"), ModifierClasses.Num());
    if (SkippedTriggers.Num() > 0 || SkippedModifiers.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> S1, S2;
        for (const FString& s : SkippedTriggers)  S1.Add(MakeShared<FJsonValueString>(s));
        for (const FString& s : SkippedModifiers) S2.Add(MakeShared<FJsonValueString>(s));
        if (S1.Num() > 0) R->SetArrayField(TEXT("skipped_triggers"),  S1);
        if (S2.Num() > 0) R->SetArrayField(TEXT("skipped_modifiers"), S2);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

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

FSageToolDispatch::FOutcome InspectPieImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!GEditor || !GEditor->IsPlayingSessionInEditor())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("PIE not running"));

    UWorld* PieWorld = GetPieWorld();
    if (!PieWorld) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no PIE world"));

    int32 ActorCount = 0;
    for (TActorIterator<AActor> It(PieWorld); It; ++It) ++ActorCount;

    FString ActorId;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("actor"), ActorId);
        if (ActorId.IsEmpty()) Args->TryGetStringField(TEXT("actor_id"), ActorId);
    }
    bool bIncludeComponents = true;
    bool bIncludeActorProperties = false;
    bool bIncludeComponentProperties = false;
    bool bIncludeActors = false;
    int32 MaxActors = 128;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
        Args->TryGetBoolField(TEXT("include_actor_properties"), bIncludeActorProperties);
        Args->TryGetBoolField(TEXT("include_component_properties"), bIncludeComponentProperties);
        Args->TryGetBoolField(TEXT("include_actors"), bIncludeActors);
        Args->TryGetNumberField(TEXT("max"), MaxActors);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("world"),        PieWorld->GetName());
    R->SetBoolField  (TEXT("is_paused"),    PieWorld->IsPaused());
    R->SetNumberField(TEXT("actor_count"),  ActorCount);
    R->SetNumberField(TEXT("time_seconds"), PieWorld->GetTimeSeconds());

    if (!ActorId.IsEmpty())
    {
        AActor* Actor = FindActorInPie(PieWorld, ActorId);
        if (!Actor)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("PIE actor not found: %s"), *ActorId));
        }
        R->SetObjectField(TEXT("actor"), ActorSummaryJson(Actor, bIncludeComponents, bIncludeActorProperties, bIncludeComponentProperties));
    }
    else if (bIncludeActors)
    {
        TArray<TSharedPtr<FJsonValue>> Actors;
        for (TActorIterator<AActor> It(PieWorld); It && Actors.Num() < MaxActors; ++It)
        {
            Actors.Add(MakeShared<FJsonValueObject>(ActorSummaryJson(*It, false, false, false)));
        }
        R->SetArrayField(TEXT("actors"), Actors);
        R->SetNumberField(TEXT("returned_actor_count"), Actors.Num());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ListPieActorsImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* PieWorld = GetPieWorld();
    if (!PieWorld) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("PIE not running"));

    FString Filter, ClassFilter;
    bool bIncludeComponents = false;
    int32 MaxActors = 256;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("filter"), Filter);
        Args->TryGetStringField(TEXT("class"), ClassFilter);
        Args->TryGetStringField(TEXT("class_filter"), ClassFilter);
        Args->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
        Args->TryGetNumberField(TEXT("max"), MaxActors);
    }

    TArray<TSharedPtr<FJsonValue>> Actors;
    int32 MatchedCount = 0;
    for (TActorIterator<AActor> It(PieWorld); It; ++It)
    {
        AActor* Actor = *It;
        if (!ActorMatchesFilter(Actor, Filter, ClassFilter)) continue;
        ++MatchedCount;
        if (Actors.Num() < MaxActors)
        {
            Actors.Add(MakeShared<FJsonValueObject>(ActorSummaryJson(Actor, bIncludeComponents, false, false)));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("world"), PieWorld->GetName());
    R->SetArrayField(TEXT("actors"), Actors);
    R->SetNumberField(TEXT("matched_count"), MatchedCount);
    R->SetNumberField(TEXT("returned_count"), Actors.Num());
    R->SetStringField(TEXT("filter"), Filter);
    R->SetStringField(TEXT("class_filter"), ClassFilter);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GetLocalPlayerImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* PieWorld = GetPieWorld();
    if (!PieWorld) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("PIE not running"));

    int32 PlayerIndex = 0;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("player_index"), PlayerIndex);
        Args->TryGetNumberField(TEXT("index"), PlayerIndex);
    }

    APlayerController* PC = UGameplayStatics::GetPlayerController(PieWorld, PlayerIndex);
    if (!PC)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("local player controller not found: %d"), PlayerIndex));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("world"), PieWorld->GetName());
    R->SetStringField(TEXT("world_path"), PieWorld->GetPathName());
    R->SetStringField(TEXT("map_name"), PieWorld->GetMapName());
    R->SetNumberField(TEXT("player_index"), PlayerIndex);
    R->SetObjectField(TEXT("controller"), ActorSummaryJson(PC, false, false, false));
    R->SetObjectField(TEXT("pawn"), ActorSummaryJson(PC->GetPawn(), true, false, false));
    R->SetObjectField(TEXT("player_state"), ActorSummaryJson(PC->PlayerState, false, false, false));
    R->SetStringField(TEXT("hud"), PC->GetHUD() ? PC->GetHUD()->GetPathName() : FString());
    R->SetStringField(TEXT("game_state"), PieWorld->GetGameState() ? PieWorld->GetGameState()->GetPathName() : FString());
    TArray<TSharedPtr<FJsonValue>> PieWorlds;
    if (GEngine)
    {
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            UWorld* World = Ctx.World();
            if (Ctx.WorldType != EWorldType::PIE || !World)
            {
                continue;
            }
            auto W = MakeShared<FJsonObject>();
            W->SetStringField(TEXT("name"), World->GetName());
            W->SetStringField(TEXT("path"), World->GetPathName());
            W->SetStringField(TEXT("map_name"), World->GetMapName());
            W->SetBoolField(TEXT("selected"), World == PieWorld);
            PieWorlds.Add(MakeShared<FJsonValueObject>(W));
        }
    }
    R->SetArrayField(TEXT("pie_worlds"), PieWorlds);
    R->SetNumberField(TEXT("pie_world_count"), PieWorlds.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.get_pie_anim_state -------------------------------------------

FSageToolDispatch::FOutcome GetPieAnimStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* PieWorld = GetPieWorld();
    if (!PieWorld) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("PIE not running"));

    FString ActorId, ComponentName;
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    Args->TryGetStringField(TEXT("actor_id"), ActorId);
    if (ActorId.IsEmpty()) Args->TryGetStringField(TEXT("actor"), ActorId);
    if (ActorId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    Args->TryGetStringField(TEXT("mesh_component"), ComponentName);
    Args->TryGetStringField(TEXT("component"), ComponentName);

    AActor* Actor = FindActorInPie(PieWorld, ActorId);
    if (!Actor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("PIE actor not found: %s"), *ActorId));
    }
    USkeletalMeshComponent* Mesh = FindSkeletalMeshComponent(Actor, ComponentName);
    if (!Mesh)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("actor has no matching SkeletalMeshComponent"));
    }
    UAnimInstance* Anim = Mesh->GetAnimInstance();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), Actor->GetPathName());
    R->SetStringField(TEXT("mesh_component"), Mesh->GetPathName());
    R->SetStringField(TEXT("skeletal_mesh"), Mesh->GetSkeletalMeshAsset() ? Mesh->GetSkeletalMeshAsset()->GetPathName() : FString());
    R->SetBoolField(TEXT("has_anim_instance"), Anim != nullptr);
    if (Anim)
    {
        R->SetStringField(TEXT("anim_instance"), Anim->GetPathName());
        R->SetStringField(TEXT("anim_class"), Anim->GetClass()->GetPathName());
        R->SetBoolField(TEXT("root_motion_mode_valid"), true);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- gameplay.get_pie_anim_properties / get_pie_subsystem_state -------------

FSageToolDispatch::FOutcome GetPieAnimPropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Base = GetPieAnimStateImpl(Args);
    if (!Base.bSuccess || !Base.Result.IsValid())
    {
        return Base;
    }

    FString ActorId, ComponentName;
    Args->TryGetStringField(TEXT("actor_id"), ActorId);
    if (ActorId.IsEmpty()) Args->TryGetStringField(TEXT("actor"), ActorId);
    Args->TryGetStringField(TEXT("mesh_component"), ComponentName);
    Args->TryGetStringField(TEXT("component"), ComponentName);
    UWorld* PieWorld = GetPieWorld();
    AActor* Actor = FindActorInPie(PieWorld, ActorId);
    USkeletalMeshComponent* Mesh = FindSkeletalMeshComponent(Actor, ComponentName);
    UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
    if (!Anim)
    {
        return Base;
    }

    int32 MaxProperties = 256;
    Args->TryGetNumberField(TEXT("max"), MaxProperties);
    TArray<TSharedPtr<FJsonValue>> Properties;
    for (TFieldIterator<FProperty> It(Anim->GetClass()); It && Properties.Num() < MaxProperties; ++It)
    {
        FProperty* Prop = *It;
        if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
        auto P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Prop->GetName());
        if (TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Anim, Prop))
        {
            P->SetField(TEXT("value"), Value);
        }
        Properties.Add(MakeShared<FJsonValueObject>(P));
    }
    Base.Result->SetArrayField(TEXT("properties"), Properties);
    Base.Result->SetNumberField(TEXT("property_count"), Properties.Num());
    return Base;
}

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

    bool bConfirmed = false;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("destructive op: pass 'confirmed':true to proceed "
                 "(gameplay.set_world_game_mode mutates WorldSettings)"));
    }

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
    // Fire targeted PostEditChangeProperty for DefaultGameMode so listeners
    // (PIE world settings, details panels) observe the change. Fall back to
    // PostEditChange() if reflection lookup fails.
    if (FProperty* GMProp = FindFProperty<FProperty>(
            AWorldSettings::StaticClass(), TEXT("DefaultGameMode")))
    {
        FPropertyChangedEvent Evt(GMProp, EPropertyChangeType::ValueSet);
        WS->PostEditChangeProperty(Evt);
    }
    else
    {
        WS->PostEditChange();
    }
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
    Dispatch.RegisterHandler(TEXT("gameplay.list_pie_actors"),        GT(&ListPieActorsImpl));
    Dispatch.RegisterHandler(TEXT("gameplay.get_local_player"),       GT(&GetLocalPlayerImpl));
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
