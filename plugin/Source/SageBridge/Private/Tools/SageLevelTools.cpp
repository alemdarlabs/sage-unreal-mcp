#include "Tools/SageLevelTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SplineComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/TriggerVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "SageLevel"

namespace sage::tools
{
namespace
{

UWorld* GetEditorWorld()
{
    if (!GEditor) return nullptr;
    return GEditor->GetEditorWorldContext().World();
}

// ---- level.get_outliner ----------------------------------------------------

FSageToolDispatch::FOutcome LevelGetOutlinerImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    TArray<TSharedPtr<FJsonValue>> Actors;
    int32 Count = 0;
    for (TActorIterator<AActor> It(World); It && Count < 1000; ++It, ++Count)
    {
        AActor* A = *It;
        if (!A || A->IsA(AWorldSettings::StaticClass())) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),  A->GetActorLabel());
        J->SetStringField(TEXT("class"), A->GetClass()->GetName());
        J->SetStringField(TEXT("path"),  A->GetPathName());
        J->SetBoolField  (TEXT("hidden"), A->IsHidden());
        FVector Loc = A->GetActorLocation();
        TArray<TSharedPtr<FJsonValue>> LocArr = {
            MakeShared<FJsonValueNumber>(Loc.X),
            MakeShared<FJsonValueNumber>(Loc.Y),
            MakeShared<FJsonValueNumber>(Loc.Z)
        };
        J->SetArrayField(TEXT("location"), LocArr);
        Actors.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("actors"), Actors);
    R->SetNumberField(TEXT("count"),  Actors.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.get_actor_details -----------------------------------------------

FSageToolDispatch::FOutcome LevelGetActorDetailsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Id;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), Id))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    AActor* A = detail::ResolveActor(Id);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *Id));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("name"),   A->GetActorLabel());
    R->SetStringField(TEXT("class"),  A->GetClass()->GetName());
    R->SetStringField(TEXT("path"),   A->GetPathName());
    R->SetBoolField  (TEXT("hidden"), A->IsHidden());

    auto ToArr = [](const FVector& V) -> TArray<TSharedPtr<FJsonValue>> {
        return { MakeShared<FJsonValueNumber>(V.X),
                 MakeShared<FJsonValueNumber>(V.Y),
                 MakeShared<FJsonValueNumber>(V.Z) };
    };
    R->SetArrayField(TEXT("location"), ToArr(A->GetActorLocation()));
    FRotator Rot = A->GetActorRotation();
    R->SetArrayField(TEXT("rotation"), {
        MakeShared<FJsonValueNumber>(Rot.Pitch),
        MakeShared<FJsonValueNumber>(Rot.Yaw),
        MakeShared<FJsonValueNumber>(Rot.Roll)
    });
    R->SetArrayField(TEXT("scale"),    ToArr(A->GetActorScale3D()));

    TArray<TSharedPtr<FJsonValue>> Comps;
    TArray<UActorComponent*> CompArr;
    A->GetComponents(CompArr);
    for (UActorComponent* C : CompArr)
    {
        if (!C) continue;
        auto CJ = MakeShared<FJsonObject>();
        CJ->SetStringField(TEXT("name"),  C->GetName());
        CJ->SetStringField(TEXT("class"), C->GetClass()->GetName());
        Comps.Add(MakeShared<FJsonValueObject>(CJ));
    }
    R->SetArrayField(TEXT("components"), Comps);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.load ------------------------------------------------------------

FSageToolDispatch::FOutcome LevelLoadImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString LevelPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), LevelPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    GEditor->Exec(GEditor->GetEditorWorldContext().World(),
        *FString::Printf(TEXT("open %s"), *LevelPath), *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),   LevelPath);
    R->SetBoolField  (TEXT("loaded"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.list ------------------------------------------------------------

FSageToolDispatch::FOutcome LevelListImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    bool bRecursive    = true;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"),      SearchPath);
        Args->TryGetBoolField  (TEXT("recursive"), bRecursive);
    }

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARM.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths        = bRecursive;
    Filter.bRecursiveClasses      = false;
    Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> Levels;
    for (const FAssetData& D : Assets)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), D.AssetName.ToString());
        J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
        Levels.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("levels"), Levels);
    R->SetNumberField(TEXT("count"),  Levels.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.create ----------------------------------------------------------

FSageToolDispatch::FOutcome LevelCreateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("path must be /Folder/Name form"));

    if (FindPackage(nullptr, *(PackagePath / AssetName)))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("already exists: %s"), *Path));

    UPackage* Pkg = CreatePackage(*(PackagePath / AssetName));
    if (!Pkg) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreatePackage failed"));
    Pkg->FullyLoad();

    UWorld* NewWorld = NewObject<UWorld>(Pkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewWorld) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("NewObject<UWorld> failed"));

    FAssetRegistryModule::AssetCreated(NewWorld);
    NewWorld->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), NewWorld->GetPathName());
    R->SetStringField(TEXT("name"), NewWorld->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.spawn_volume ----------------------------------------------------

FSageToolDispatch::FOutcome LevelSpawnVolumeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ClassPath = TEXT("/Script/Engine.BlockingVolume");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("class"), ClassPath);

    UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassPath);
    if (!Cls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("class not found: %s"), *ClassPath));

    FVector Loc = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Loc);
    FRotator Rot = FRotator::ZeroRotator;
    detail::ParseRotator3(Args, TEXT("rotation"), Rot);
    FTransform Tf(Rot, Loc);

    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FScopedTransaction Tx(LOCTEXT("SpawnVol", "Spawn Volume"));
    AActor* NewActor = GEditor->AddActor(World->GetCurrentLevel(), Cls, Tf);
    if (!NewActor) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("AddActor failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), NewActor->GetPathName());
    R->SetStringField(TEXT("class"),    NewActor->GetClass()->GetName());
    FVector FL = NewActor->GetActorLocation();
    R->SetArrayField (TEXT("location"), {
        MakeShared<FJsonValueNumber>(FL.X),
        MakeShared<FJsonValueNumber>(FL.Y),
        MakeShared<FJsonValueNumber>(FL.Z)
    });
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.list_volumes ----------------------------------------------------

FSageToolDispatch::FOutcome LevelListVolumesImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FString ClassFilter;
    bool bFilter = Args.IsValid() && Args->TryGetStringField(TEXT("class"), ClassFilter);

    TArray<TSharedPtr<FJsonValue>> Volumes;
    for (TActorIterator<AVolume> It(World); It; ++It)
    {
        AVolume* V = *It;
        if (!V) continue;
        if (bFilter && V->GetClass()->GetName() != ClassFilter) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("actor_id"), V->GetPathName());
        J->SetStringField(TEXT("name"),     V->GetActorLabel());
        J->SetStringField(TEXT("class"),    V->GetClass()->GetName());
        Volumes.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("volumes"), Volumes);
    R->SetNumberField(TEXT("count"),   Volumes.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.set_volume_properties -------------------------------------------

FSageToolDispatch::FOutcome LevelSetVolumePropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Id;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), Id))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    AActor* A = detail::ResolveActor(Id);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *Id));

    FScopedTransaction Tx(LOCTEXT("SetVol", "Set Volume Properties"));
    A->Modify();

    bool bHidden;
    if (Args->TryGetBoolField(TEXT("hidden"), bHidden))
        A->SetIsTemporarilyHiddenInEditor(bHidden);

    A->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());
    R->SetBoolField  (TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.spawn_light -----------------------------------------------------

FSageToolDispatch::FOutcome LevelSpawnLightImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ClassPath = TEXT("/Script/Engine.PointLight");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("class"), ClassPath);

    UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassPath);
    if (!Cls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("class not found: %s"), *ClassPath));

    FVector Loc = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Loc);
    FRotator Rot = FRotator::ZeroRotator;
    detail::ParseRotator3(Args, TEXT("rotation"), Rot);
    FTransform Tf(Rot, Loc);

    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FScopedTransaction Tx(LOCTEXT("SpawnLight", "Spawn Light"));
    AActor* NewActor = GEditor->AddActor(World->GetCurrentLevel(), Cls, Tf);
    if (!NewActor) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("AddActor failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), NewActor->GetPathName());
    R->SetStringField(TEXT("class"),    NewActor->GetClass()->GetName());
    FVector FL = NewActor->GetActorLocation();
    R->SetArrayField (TEXT("location"), {
        MakeShared<FJsonValueNumber>(FL.X),
        MakeShared<FJsonValueNumber>(FL.Y),
        MakeShared<FJsonValueNumber>(FL.Z)
    });
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.set_light_properties --------------------------------------------

FSageToolDispatch::FOutcome LevelSetLightPropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Id;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), Id))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    AActor* A = detail::ResolveActor(Id);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *Id));

    ULightComponent* LC = Cast<ULightComponent>(
        A->FindComponentByClass(ULightComponent::StaticClass()));
    if (!LC) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("actor has no LightComponent"));

    FScopedTransaction Tx(LOCTEXT("SetLight", "Set Light Properties"));
    LC->Modify();
    A->Modify();

    double Intensity;
    if (Args->TryGetNumberField(TEXT("intensity"), Intensity))
    {
        LC->Intensity = static_cast<float>(Intensity);
    }

    const TArray<TSharedPtr<FJsonValue>>* ColorArr;
    if (Args->TryGetArrayField(TEXT("color"), ColorArr) && ColorArr->Num() == 3)
    {
        float R2 = static_cast<float>((*ColorArr)[0]->AsNumber());
        float G  = static_cast<float>((*ColorArr)[1]->AsNumber());
        float B  = static_cast<float>((*ColorArr)[2]->AsNumber());
        LC->LightColor = FColor(
            FMath::Clamp((int32)(R2 * 255), 0, 255),
            FMath::Clamp((int32)(G  * 255), 0, 255),
            FMath::Clamp((int32)(B  * 255), 0, 255));
    }

    FRotator Rot;
    if (detail::ParseRotator3(Args, TEXT("rotation"), Rot))
        A->SetActorRotation(Rot);

    LC->PostEditChange();
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());
    R->SetBoolField  (TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.set_fog_properties ----------------------------------------------

FSageToolDispatch::FOutcome LevelSetFogPropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    AExponentialHeightFog* FogActor = nullptr;
    FString Id;
    if (Args.IsValid() && Args->TryGetStringField(TEXT("actor_id"), Id))
    {
        FogActor = Cast<AExponentialHeightFog>(detail::ResolveActor(Id));
        if (!FogActor) return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("actor is not AExponentialHeightFog"));
    }
    else
    {
        for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
        {
            FogActor = *It;
            break;
        }
    }
    if (!FogActor) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("no ExponentialHeightFog found in world"));

    UExponentialHeightFogComponent* FC = FogActor->GetComponent();
    if (!FC) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("fog has no component"));

    FScopedTransaction Tx(LOCTEXT("SetFog", "Set Fog Properties"));
    FC->Modify();

    double Val;
    if (Args && Args->TryGetNumberField(TEXT("fog_density"), Val))
        FC->FogDensity = static_cast<float>(Val);
    if (Args && Args->TryGetNumberField(TEXT("fog_height_falloff"), Val))
        FC->FogHeightFalloff = static_cast<float>(Val);
    if (Args && Args->TryGetNumberField(TEXT("start_distance"), Val))
        FC->StartDistance = static_cast<float>(Val);

    FC->PostEditChange();
    FogActor->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), FogActor->GetPathName());
    R->SetBoolField  (TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.get_actors_by_class ---------------------------------------------

FSageToolDispatch::FOutcome LevelGetActorsByClassImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FString ClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("class"), ClassPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'class'"));

    UClass* FilterCls = FindObject<UClass>(nullptr, *ClassPath);
    if (!FilterCls) FilterCls = LoadObject<UClass>(nullptr, *ClassPath);
    if (!FilterCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("class not found: %s"), *ClassPath));

    double MaxD = 100;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("max_results"), MaxD);
    const int32 MaxResults = FMath::Clamp((int32)MaxD, 1, 10000);

    TArray<TSharedPtr<FJsonValue>> Actors;
    for (TActorIterator<AActor> It(World, FilterCls); It && Actors.Num() < MaxResults; ++It)
    {
        AActor* A = *It;
        if (!A) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("actor_id"), A->GetPathName());
        J->SetStringField(TEXT("name"),     A->GetActorLabel());
        FVector Loc = A->GetActorLocation();
        J->SetArrayField (TEXT("location"), {
            MakeShared<FJsonValueNumber>(Loc.X),
            MakeShared<FJsonValueNumber>(Loc.Y),
            MakeShared<FJsonValueNumber>(Loc.Z)
        });
        Actors.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("actors"), Actors);
    R->SetNumberField(TEXT("count"),  Actors.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.count_actors_by_class -------------------------------------------

FSageToolDispatch::FOutcome LevelCountActorsByClassImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    TMap<FString, int32> Hist;
    int32 Total = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (!A || A->IsA(AWorldSettings::StaticClass())) continue;
        FString ClassName = A->GetClass()->GetName();
        Hist.FindOrAdd(ClassName)++;
        Total++;
    }

    TArray<TSharedPtr<FJsonValue>> Arr;
    Hist.KeySort([](const FString& A, const FString& B) { return A < B; });
    for (const auto& KV : Hist)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("class"), KV.Key);
        J->SetNumberField(TEXT("count"), KV.Value);
        Arr.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("histogram"), Arr);
    R->SetNumberField(TEXT("total"),     Total);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.get_spline_info -------------------------------------------------

FSageToolDispatch::FOutcome LevelGetSplineInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Id;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), Id))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    AActor* A = detail::ResolveActor(Id);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *Id));

    USplineComponent* SC = Cast<USplineComponent>(
        A->FindComponentByClass(USplineComponent::StaticClass()));
    if (!SC) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("actor has no SplineComponent"));

    TArray<TSharedPtr<FJsonValue>> Points;
    const int32 NumPts = SC->GetNumberOfSplinePoints();
    for (int32 I = 0; I < NumPts; ++I)
    {
        FVector Loc = SC->GetLocationAtSplinePoint(I, ESplineCoordinateSpace::World);
        FVector Tan = SC->GetTangentAtSplinePoint(I, ESplineCoordinateSpace::World);
        auto J = MakeShared<FJsonObject>();
        J->SetArrayField(TEXT("location"), {
            MakeShared<FJsonValueNumber>(Loc.X),
            MakeShared<FJsonValueNumber>(Loc.Y),
            MakeShared<FJsonValueNumber>(Loc.Z)
        });
        J->SetArrayField(TEXT("tangent"), {
            MakeShared<FJsonValueNumber>(Tan.X),
            MakeShared<FJsonValueNumber>(Tan.Y),
            MakeShared<FJsonValueNumber>(Tan.Z)
        });
        Points.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("spline_component"), SC->GetName());
    R->SetNumberField(TEXT("num_points"),       NumPts);
    R->SetBoolField  (TEXT("closed"),           SC->IsClosedLoop());
    R->SetArrayField (TEXT("points"),           Points);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.set_spline_points -----------------------------------------------

FSageToolDispatch::FOutcome LevelSetSplinePointsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Id;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), Id))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    AActor* A = detail::ResolveActor(Id);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *Id));

    USplineComponent* SC = Cast<USplineComponent>(
        A->FindComponentByClass(USplineComponent::StaticClass()));
    if (!SC) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("actor has no SplineComponent"));

    const TArray<TSharedPtr<FJsonValue>>* PointsArr;
    if (!Args->TryGetArrayField(TEXT("points"), PointsArr))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'points'"));

    FScopedTransaction Tx(LOCTEXT("SetSpline", "Set Spline Points"));
    SC->Modify();
    SC->ClearSplinePoints(false);

    for (const TSharedPtr<FJsonValue>& PV : *PointsArr)
    {
        const TSharedPtr<FJsonObject>* PObj;
        if (!PV->TryGetObject(PObj)) continue;
        const TArray<TSharedPtr<FJsonValue>>* LocArr;
        if (!(*PObj)->TryGetArrayField(TEXT("location"), LocArr) || LocArr->Num() < 3) continue;
        FVector Loc(
            (*LocArr)[0]->AsNumber(),
            (*LocArr)[1]->AsNumber(),
            (*LocArr)[2]->AsNumber());
        SC->AddSplinePoint(Loc, ESplineCoordinateSpace::World, false);
    }
    SC->UpdateSpline();

    bool bClosed = false;
    if (Args->TryGetBoolField(TEXT("closed"), bClosed))
        SC->SetClosedLoop(bClosed, false);

    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),   A->GetPathName());
    R->SetNumberField(TEXT("num_points"), SC->GetNumberOfSplinePoints());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.set_actor_material ----------------------------------------------

FSageToolDispatch::FOutcome LevelSetActorMaterialImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Id, MatPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), Id))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    if (!Args->TryGetStringField(TEXT("material_path"), MatPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'material_path'"));

    AActor* A = detail::ResolveActor(Id);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *Id));

    UMeshComponent* MC = Cast<UMeshComponent>(
        A->FindComponentByClass(UMeshComponent::StaticClass()));
    if (!MC) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("actor has no MeshComponent"));

    UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *MatPath));

    double SlotD = 0;
    Args->TryGetNumberField(TEXT("slot_index"), SlotD);
    int32 Slot = FMath::Clamp((int32)SlotD, 0, MC->GetNumMaterials() - 1);

    FScopedTransaction Tx(LOCTEXT("SetActMat", "Set Actor Material"));
    MC->Modify();
    MC->SetMaterial(Slot, Mat);
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),     A->GetPathName());
    R->SetStringField(TEXT("material_path"), MatPath);
    R->SetNumberField(TEXT("slot_index"),    Slot);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.set_world_settings ----------------------------------------------

FSageToolDispatch::FOutcome LevelSetWorldSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    AWorldSettings* WS = World->GetWorldSettings();
    if (!WS) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no WorldSettings"));

    FScopedTransaction Tx(LOCTEXT("SetWS", "Set World Settings"));
    WS->Modify();

    double Val;
    if (Args.IsValid() && Args->TryGetNumberField(TEXT("gravity_z"), Val))
        WS->WorldGravityZ = static_cast<float>(Val);
    if (Args.IsValid() && Args->TryGetNumberField(TEXT("kill_z"), Val))
        WS->KillZ = Val;

    FString GMPath;
    if (Args.IsValid() && Args->TryGetStringField(TEXT("default_game_mode"), GMPath))
    {
        UClass* GMCls = FindObject<UClass>(nullptr, *GMPath);
        if (!GMCls) GMCls = LoadObject<UClass>(nullptr, *GMPath);
        if (GMCls) WS->DefaultGameMode = GMCls;
    }

    WS->PostEditChange();
    World->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("modified"),   true);
    R->SetNumberField(TEXT("gravity_z"),  WS->WorldGravityZ);
    R->SetNumberField(TEXT("kill_z"),     WS->KillZ);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.get_actor_bounds ------------------------------------------------

FSageToolDispatch::FOutcome LevelGetActorBoundsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Id;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), Id))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    AActor* A = detail::ResolveActor(Id);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *Id));

    FBox Box = A->GetComponentsBoundingBox(true);
    FVector Origin, Extent;
    Box.GetCenterAndExtents(Origin, Extent);

    auto ToArr = [](const FVector& V) -> TArray<TSharedPtr<FJsonValue>> {
        return { MakeShared<FJsonValueNumber>(V.X),
                 MakeShared<FJsonValueNumber>(V.Y),
                 MakeShared<FJsonValueNumber>(V.Z) };
    };

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("origin"),  ToArr(Origin));
    R->SetArrayField(TEXT("extent"),  ToArr(Extent));
    R->SetArrayField(TEXT("min"),     ToArr(Box.Min));
    R->SetArrayField(TEXT("max"),     ToArr(Box.Max));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.resolve_actor ---------------------------------------------------

FSageToolDispatch::FOutcome LevelResolveActorImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("name"), Name))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));

    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (!A) continue;
        if (A->GetName() == Name || A->GetActorLabel() == Name)
        {
            FVector Loc = A->GetActorLocation();
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("actor_id"), A->GetPathName());
            R->SetStringField(TEXT("label"),    A->GetActorLabel());
            R->SetStringField(TEXT("class"),    A->GetClass()->GetName());
            R->SetArrayField (TEXT("location"), {
                MakeShared<FJsonValueNumber>(Loc.X),
                MakeShared<FJsonValueNumber>(Loc.Y),
                MakeShared<FJsonValueNumber>(Loc.Z)
            });
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
    }
    return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found with name/label: %s"), *Name));
}

// ---- level.get_runtime_virtual_texture_summary -----------------------------

FSageToolDispatch::FOutcome LevelGetRvtSummaryImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    // Find RVT volumes by class name (avoid module dep)
    UClass* RvtCls = FindObject<UClass>(nullptr,
        TEXT("/Script/RuntimeVirtualTexture.RuntimeVirtualTextureVolume"));

    TArray<TSharedPtr<FJsonValue>> Volumes;
    if (RvtCls)
    {
        for (TActorIterator<AActor> It(World, RvtCls); It; ++It)
        {
            AActor* A = *It;
            if (!A) continue;
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("actor_id"), A->GetPathName());
            J->SetStringField(TEXT("name"),     A->GetActorLabel());
            Volumes.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("volumes"), Volumes);
    R->SetNumberField(TEXT("count"),   Volumes.Num());
    if (!RvtCls) R->SetStringField(TEXT("note"), TEXT("RuntimeVirtualTexture module not loaded"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.set_water_body_property -----------------------------------------

FSageToolDispatch::FOutcome LevelSetWaterBodyPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Id, PropName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), Id))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    if (!Args->TryGetStringField(TEXT("property"), PropName))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'property'"));

    AActor* A = detail::ResolveActor(Id);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *Id));

    const TSharedPtr<FJsonValue>* ValPtr = Args->Values.Find(TEXT("value"));
    if (!ValPtr) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));

    // Find property on actor or its first component
    FProperty* Prop = FindFProperty<FProperty>(A->GetClass(), *PropName);
    UObject* Target = A;
    if (!Prop)
    {
        // Try finding in components
        TArray<UActorComponent*> Comps;
        A->GetComponents(Comps);
        for (UActorComponent* C : Comps)
        {
            if (!C) continue;
            Prop = FindFProperty<FProperty>(C->GetClass(), *PropName);
            if (Prop) { Target = C; break; }
        }
    }
    if (!Prop) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("property not found: %s"), *PropName));

    FScopedTransaction Tx(LOCTEXT("SetWater", "Set Water Body Property"));
    Target->Modify();
    if (!detail::SetUPropertyFromJson(Target, Prop, *ValPtr))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("cannot set property %s"), *PropName));

    A->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());
    R->SetBoolField  (TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- level.build_lighting --------------------------------------------------

FSageToolDispatch::FOutcome LevelBuildLightingImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString QualityStr = TEXT("Preview");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("quality"), QualityStr);

    ELightingBuildQuality Quality = Quality_Preview;
    if      (QualityStr == TEXT("Medium"))     Quality = Quality_Medium;
    else if (QualityStr == TEXT("High"))       Quality = Quality_High;
    else if (QualityStr == TEXT("Production")) Quality = Quality_Production;

    // BuildLighting requires GUnrealEd and FLightingBuildOptions (UnrealEdEngine.h).
    // Trigger via console command which is always accessible.
    GEditor->Exec(GEditor->GetEditorWorldContext().World(),
        TEXT("BUILDLIGHTING"), *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("quality"), QualityStr);
    R->SetBoolField  (TEXT("started"), true);
    R->SetStringField(TEXT("note"),
        TEXT("lighting build dispatched via console; check editor Output Log for progress"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- world.export (Phase 4.6-r4 — CommonAIExport parity) -------------------
//
// Read-only structural snapshot of the currently-loaded editor world. To
// keep destructive risk near zero on production projects this tool refuses
// to switch maps: if `path` is provided and doesn't match the current
// PersistentLevel, returns -32602 telling the caller to load the map first
// (via editor UI or `level.load`).

FSageToolDispatch::FOutcome WorldExportImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GetEditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("no editor world"));

    bool bIncludeComponents = false;
    bool bIncludeActorProps = false;
    bool bIncludeWorldSettings = false;
    int32 MaxActors = 10000;
    FString PathArg, ClassFilter;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"),               PathArg);
        Args->TryGetStringField(TEXT("actor_class_filter"), ClassFilter);
        Args->TryGetBoolField  (TEXT("include_components"), bIncludeComponents);
        Args->TryGetBoolField  (TEXT("include_actor_props"),bIncludeActorProps);
        Args->TryGetBoolField  (TEXT("include_world_settings"), bIncludeWorldSettings);
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_actors"), N))
            MaxActors = FMath::Clamp(static_cast<int32>(N), 1, 200000);
    }

    const FString CurrentMapPath = World->GetOutermost()->GetName();
    if (!PathArg.IsEmpty() && !PathArg.Equals(CurrentMapPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("current world is '%s'; refuses to switch maps. "
                                 "Load the requested map (editor UI or "
                                 "level.load) and re-run."),
                            *CurrentMapPath));
    }

    UClass* Filter = nullptr;
    if (!ClassFilter.IsEmpty())
    {
        Filter = FindObject<UClass>(nullptr, *ClassFilter);
        if (!Filter)
        {
            Filter = LoadObject<UClass>(nullptr, *ClassFilter);
        }
        if (!Filter)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("actor_class_filter not found: %s"),
                                *ClassFilter));
        }
    }

    detail::FInstancedRecurseCtx ActorCtx;
    ActorCtx.MaxDepth = 2;  // shallow — components hold their own refs

    TArray<TSharedPtr<FJsonValue>> Actors;
    int32 Total = 0;
    int32 Truncated = 0;
    FBox WorldBounds(ForceInit);
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (!A) continue;
        if (A->IsA(AWorldSettings::StaticClass())) continue;
        if (Filter && !A->IsA(Filter)) continue;
        ++Total;
        if (Actors.Num() >= MaxActors) { ++Truncated; continue; }

        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),  A->GetActorLabel());
        J->SetStringField(TEXT("class"), A->GetClass()->GetPathName());
        J->SetStringField(TEXT("path"),  A->GetPathName());
        const FName Folder = A->GetFolderPath();
        if (!Folder.IsNone()) J->SetStringField(TEXT("folder"), Folder.ToString());

        const FTransform Tx = A->GetActorTransform();
        const FVector Loc = Tx.GetLocation();
        const FRotator Rot = Tx.Rotator();
        const FVector Scale = Tx.GetScale3D();
        TArray<TSharedPtr<FJsonValue>> LocArr = {
            MakeShared<FJsonValueNumber>(Loc.X),
            MakeShared<FJsonValueNumber>(Loc.Y),
            MakeShared<FJsonValueNumber>(Loc.Z),
        };
        TArray<TSharedPtr<FJsonValue>> RotArr = {
            MakeShared<FJsonValueNumber>(Rot.Pitch),
            MakeShared<FJsonValueNumber>(Rot.Yaw),
            MakeShared<FJsonValueNumber>(Rot.Roll),
        };
        TArray<TSharedPtr<FJsonValue>> ScaleArr = {
            MakeShared<FJsonValueNumber>(Scale.X),
            MakeShared<FJsonValueNumber>(Scale.Y),
            MakeShared<FJsonValueNumber>(Scale.Z),
        };
        auto TxJ = MakeShared<FJsonObject>();
        TxJ->SetArrayField(TEXT("location"), LocArr);
        TxJ->SetArrayField(TEXT("rotation"), RotArr);
        TxJ->SetArrayField(TEXT("scale"),    ScaleArr);
        J->SetObjectField(TEXT("transform"), TxJ);

        if (A->Tags.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> TagArr;
            for (FName T : A->Tags)
                TagArr.Add(MakeShared<FJsonValueString>(T.ToString()));
            J->SetArrayField(TEXT("tags"), TagArr);
        }

        if (bIncludeComponents)
        {
            TArray<TSharedPtr<FJsonValue>> Comps;
            TArray<UActorComponent*> ComponentList;
            A->GetComponents(ComponentList);
            for (UActorComponent* C : ComponentList)
            {
                if (!C) continue;
                auto CJ = MakeShared<FJsonObject>();
                CJ->SetStringField(TEXT("name"),  C->GetName());
                CJ->SetStringField(TEXT("class"), C->GetClass()->GetPathName());
                Comps.Add(MakeShared<FJsonValueObject>(CJ));
            }
            J->SetArrayField(TEXT("components"), Comps);
        }

        if (bIncludeActorProps)
        {
            // Reset visited per actor — actor instances are independent roots.
            ActorCtx.Visited.Reset();
            ActorCtx.Visited.Add(A);
            ActorCtx.CurrentDepth = 0;
            auto Props = MakeShared<FJsonObject>();
            int32 Count = 0;
            for (TFieldIterator<FProperty> It2(A->GetClass()); It2; ++It2)
            {
                FProperty* P = *It2;
                if (!P) continue;
                if (P->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
                    continue;
                auto V = detail::GetUPropertyAsJson(A, P, &ActorCtx);
                if (V.IsValid()) { Props->SetField(P->GetName(), V); ++Count; }
            }
            J->SetObjectField(TEXT("properties"), Props);
            J->SetNumberField(TEXT("property_count"), Count);
        }

        // Track world bounds across kept actors only — quicker, still useful.
        FBox B = A->GetComponentsBoundingBox(/*bNonColliding=*/true);
        if (B.IsValid) WorldBounds += B;

        Actors.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("world_path"),  CurrentMapPath);
    R->SetStringField(TEXT("world_name"),  World->GetName());
    R->SetArrayField (TEXT("actors"),      Actors);
    R->SetNumberField(TEXT("returned"),    Actors.Num());
    R->SetNumberField(TEXT("total"),       Total);
    R->SetBoolField  (TEXT("truncated"),   Truncated > 0);
    R->SetBoolField  (TEXT("is_world_partition"),
        World->GetWorldPartition() != nullptr);

    if (WorldBounds.IsValid)
    {
        TArray<TSharedPtr<FJsonValue>> MinArr = {
            MakeShared<FJsonValueNumber>(WorldBounds.Min.X),
            MakeShared<FJsonValueNumber>(WorldBounds.Min.Y),
            MakeShared<FJsonValueNumber>(WorldBounds.Min.Z),
        };
        TArray<TSharedPtr<FJsonValue>> MaxArr = {
            MakeShared<FJsonValueNumber>(WorldBounds.Max.X),
            MakeShared<FJsonValueNumber>(WorldBounds.Max.Y),
            MakeShared<FJsonValueNumber>(WorldBounds.Max.Z),
        };
        auto BJ = MakeShared<FJsonObject>();
        BJ->SetArrayField(TEXT("min"), MinArr);
        BJ->SetArrayField(TEXT("max"), MaxArr);
        R->SetObjectField(TEXT("level_bounds"), BJ);
    }

    // Streaming levels (top-level only — nested LevelInstance/WP unhandled).
    TArray<TSharedPtr<FJsonValue>> Streams;
    for (ULevelStreaming* SL : World->GetStreamingLevels())
    {
        if (!SL) continue;
        auto SJ = MakeShared<FJsonObject>();
        SJ->SetStringField(TEXT("name"),
            SL->GetWorldAssetPackageFName().ToString());
        SJ->SetStringField(TEXT("class"), SL->GetClass()->GetName());
        SJ->SetBoolField  (TEXT("loaded"),       SL->IsLevelLoaded());
        SJ->SetBoolField  (TEXT("visible"),      SL->IsLevelVisible());
        SJ->SetBoolField  (TEXT("should_be_loaded"),  SL->ShouldBeLoaded());
        SJ->SetBoolField  (TEXT("should_be_visible"), SL->ShouldBeVisible());
        Streams.Add(MakeShared<FJsonValueObject>(SJ));
    }
    R->SetArrayField (TEXT("streaming_levels"), Streams);
    R->SetNumberField(TEXT("streaming_count"),  Streams.Num());

    if (bIncludeWorldSettings)
    {
        AWorldSettings* WS = World->GetWorldSettings();
        if (WS)
        {
            detail::FInstancedRecurseCtx WsCtx;
            WsCtx.MaxDepth = 3;
            WsCtx.Visited.Add(WS);
            auto Props = MakeShared<FJsonObject>();
            int32 Count = 0;
            for (TFieldIterator<FProperty> It2(WS->GetClass()); It2; ++It2)
            {
                FProperty* P = *It2;
                if (!P) continue;
                if (P->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
                    continue;
                auto V = detail::GetUPropertyAsJson(WS, P, &WsCtx);
                if (V.IsValid()) { Props->SetField(P->GetName(), V); ++Count; }
            }
            auto WSJ = MakeShared<FJsonObject>();
            WSJ->SetStringField(TEXT("class"), WS->GetClass()->GetPathName());
            WSJ->SetObjectField(TEXT("properties"), Props);
            WSJ->SetNumberField(TEXT("property_count"), Count);
            R->SetObjectField(TEXT("world_settings"), WSJ);
        }
    }

    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterLevelTools(FSageToolDispatch& Dispatch)
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

    // Read
    Dispatch.RegisterHandler(TEXT("level.get_outliner"),         GT(&LevelGetOutlinerImpl));
    Dispatch.RegisterHandler(TEXT("level.get_actor_details"),    GT(&LevelGetActorDetailsImpl));
    Dispatch.RegisterHandler(TEXT("level.list"),                 GT(&LevelListImpl));
    Dispatch.RegisterHandler(TEXT("level.list_volumes"),         GT(&LevelListVolumesImpl));
    Dispatch.RegisterHandler(TEXT("level.get_actors_by_class"),  GT(&LevelGetActorsByClassImpl));
    Dispatch.RegisterHandler(TEXT("level.count_actors_by_class"),GT(&LevelCountActorsByClassImpl));
    Dispatch.RegisterHandler(TEXT("level.get_spline_info"),      GT(&LevelGetSplineInfoImpl));
    Dispatch.RegisterHandler(TEXT("level.get_actor_bounds"),     GT(&LevelGetActorBoundsImpl));
    Dispatch.RegisterHandler(TEXT("level.resolve_actor"),        GT(&LevelResolveActorImpl));
    Dispatch.RegisterHandler(TEXT("level.get_runtime_virtual_texture_summary"),
                                                                 GT(&LevelGetRvtSummaryImpl));
    // Write
    Dispatch.RegisterHandler(TEXT("level.load"),                 GT(&LevelLoadImpl));
    Dispatch.RegisterHandler(TEXT("level.create"),               GT(&LevelCreateImpl));
    Dispatch.RegisterHandler(TEXT("level.spawn_volume"),         GT(&LevelSpawnVolumeImpl));
    Dispatch.RegisterHandler(TEXT("level.set_volume_properties"),GT(&LevelSetVolumePropertiesImpl));
    Dispatch.RegisterHandler(TEXT("level.spawn_light"),          GT(&LevelSpawnLightImpl));
    Dispatch.RegisterHandler(TEXT("level.set_light_properties"), GT(&LevelSetLightPropertiesImpl));
    Dispatch.RegisterHandler(TEXT("level.set_fog_properties"),   GT(&LevelSetFogPropertiesImpl));
    Dispatch.RegisterHandler(TEXT("level.set_spline_points"),    GT(&LevelSetSplinePointsImpl));
    Dispatch.RegisterHandler(TEXT("level.set_actor_material"),   GT(&LevelSetActorMaterialImpl));
    Dispatch.RegisterHandler(TEXT("level.set_world_settings"),   GT(&LevelSetWorldSettingsImpl));
    Dispatch.RegisterHandler(TEXT("level.set_water_body_property"), GT(&LevelSetWaterBodyPropertyImpl));
    Dispatch.RegisterHandler(TEXT("level.build_lighting"),       GT(&LevelBuildLightingImpl));

    // World/Map structural exporter (Phase 4.6-r4 — CommonAIExport parity)
    Dispatch.RegisterHandler(TEXT("world.export"),               GT(&WorldExportImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
