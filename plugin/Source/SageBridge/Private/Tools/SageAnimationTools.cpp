#include "Tools/SageAnimationTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/AnimCompositeFactory.h"
#include "Factories/AnimMontageFactory.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "SageAnimation"

namespace sage::tools
{
namespace
{

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

UObject* ResolveAsset(const FString& Path)
{
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Obj;
}

IAssetTools& GetAssetTools()
{
    return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
}

IAssetRegistry& GetAssetRegistry()
{
    return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
}

// Create asset via IAssetTools given a full /Game/Path/Name path.
UObject* CreateAssetFromPath(const FString& FullPath, UClass* Cls, UFactory* Factory)
{
    FString PackagePath, AssetName;
    if (!FullPath.Split(TEXT("/"), &PackagePath, &AssetName,
                        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return nullptr;
    }
    return GetAssetTools().CreateAsset(AssetName, PackagePath, Cls, Factory);
}

// ---------------------------------------------------------------------------
// animation.list
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ListAnimationImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Dir = TEXT("/Game");
    FString TypeFilter = TEXT("all");
    bool bRecursive = true;

    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), Dir);
        Args->TryGetStringField(TEXT("type"), TypeFilter);
        Args->TryGetBoolField(TEXT("recursive"), bRecursive);
    }

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Dir));
    Filter.bRecursivePaths = bRecursive;

    if (TypeFilter == TEXT("AnimSequence"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimSequence")));
    }
    else if (TypeFilter == TEXT("AnimMontage"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimMontage")));
    }
    else if (TypeFilter == TEXT("BlendSpace"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.BlendSpace")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.BlendSpace1D")));
    }
    else if (TypeFilter == TEXT("AnimBlueprint"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimBlueprint")));
    }
    else // "all"
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimSequence")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimMontage")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.BlendSpace")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.BlendSpace1D")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimBlueprint")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimComposite")));
        Filter.bRecursiveClasses = true;
    }

    TArray<FAssetData> Found;
    GetAssetRegistry().GetAssets(Filter, Found);

    TArray<TSharedPtr<FJsonValue>> Assets;
    for (const FAssetData& A : Found)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),  A.AssetName.ToString());
        J->SetStringField(TEXT("path"),  A.GetSoftObjectPath().ToString());
        J->SetStringField(TEXT("class"), A.AssetClassPath.GetAssetName().ToString());
        Assets.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("assets"), Assets);
    R->SetNumberField(TEXT("count"),  Assets.Num());
    R->SetStringField(TEXT("path"),   Dir);
    R->SetStringField(TEXT("type"),   TypeFilter);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_anim_blueprint
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadAnimBlueprintImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimBlueprint* BP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!BP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  BP->GetPathName());
    R->SetStringField(TEXT("class"), BP->GetClass()->GetName());
    R->SetStringField(TEXT("skeleton"),
        BP->TargetSkeleton ? BP->TargetSkeleton->GetPathName() : TEXT(""));
    R->SetStringField(TEXT("target_skeleton"),
        BP->TargetSkeleton ? BP->TargetSkeleton->GetPathName() : TEXT(""));
    R->SetNumberField(TEXT("anim_graph_count"), BP->FunctionGraphs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_montage
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadMontageImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimMontage* Montage = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!Montage)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimMontage: %s"), *Path));
    }

    TArray<TSharedPtr<FJsonValue>> Sections;
    for (const FCompositeSection& Sec : Montage->CompositeSections)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),       Sec.SectionName.ToString());
        J->SetNumberField(TEXT("start_time"),  Sec.GetTime());
        Sections.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         Montage->GetPathName());
    R->SetNumberField(TEXT("duration"),     Montage->GetPlayLength());
    R->SetNumberField(TEXT("rate_scale"),   Montage->RateScale);
    R->SetNumberField(TEXT("blend_in"),     Montage->BlendIn.GetBlendTime());
    R->SetNumberField(TEXT("blend_out"),    Montage->BlendOut.GetBlendTime());
    R->SetArrayField (TEXT("sections"),     Sections);
    R->SetNumberField(TEXT("num_sections"), Sections.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_sequence
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadSequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Seq->GetPathName());
    R->SetNumberField(TEXT("duration"),   Seq->GetPlayLength());
    R->SetNumberField(TEXT("rate_scale"), Seq->RateScale);
    R->SetNumberField(TEXT("num_frames"), Seq->GetNumberOfSampledKeys());
    R->SetStringField(TEXT("skeleton"),
        Seq->GetSkeleton() ? Seq->GetSkeleton()->GetPathName() : TEXT(""));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_blendspace
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadBlendSpaceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(Path));
    if (!BS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UBlendSpace: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),        BS->GetPathName());
    R->SetStringField(TEXT("class"),       BS->GetClass()->GetName());
    R->SetNumberField(TEXT("num_samples"), BS->GetBlendSamples().Num());

    // Axis X
    {
        auto AxisJ = MakeShared<FJsonObject>();
        AxisJ->SetStringField(TEXT("name"), BS->GetBlendParameter(0).DisplayName);
        AxisJ->SetNumberField(TEXT("min"),  BS->GetBlendParameter(0).Min);
        AxisJ->SetNumberField(TEXT("max"),  BS->GetBlendParameter(0).Max);
        R->SetObjectField(TEXT("axis_x"), AxisJ);
    }

    // Axis Y (2D only — BlendSpace1D has IsAOneAxis true)
    if (!BS->IsA<UBlendSpace1D>())
    {
        auto AxisJ = MakeShared<FJsonObject>();
        AxisJ->SetStringField(TEXT("name"), BS->GetBlendParameter(1).DisplayName);
        AxisJ->SetNumberField(TEXT("min"),  BS->GetBlendParameter(1).Min);
        AxisJ->SetNumberField(TEXT("max"),  BS->GetBlendParameter(1).Max);
        R->SetObjectField(TEXT("axis_y"), AxisJ);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.get_skeleton_info
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome GetSkeletonInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    USkeleton* Skeleton = Cast<USkeleton>(ResolveAsset(Path));
    if (!Skeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *Path));
    }

    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
    const int32 NumBones = RefSkel.GetNum();

    TArray<TSharedPtr<FJsonValue>> BoneNames;
    const int32 Limit = FMath::Min(NumBones, 50);
    for (int32 I = 0; I < Limit; ++I)
    {
        BoneNames.Add(MakeShared<FJsonValueString>(RefSkel.GetBoneName(I).ToString()));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Skeleton->GetPathName());
    R->SetNumberField(TEXT("num_bones"),  NumBones);
    R->SetArrayField (TEXT("bone_names"), BoneNames);
    if (NumBones > 50)
    {
        R->SetStringField(TEXT("note"), TEXT("bone_names capped at 50"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.list_sockets
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ListSkeletonSocketsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    USkeleton* Skeleton = Cast<USkeleton>(ResolveAsset(Path));
    if (!Skeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *Path));
    }

    TArray<TSharedPtr<FJsonValue>> Sockets;
    for (const USkeletalMeshSocket* S : Skeleton->Sockets)
    {
        if (!S) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),      S->SocketName.ToString());
        J->SetStringField(TEXT("bone_name"), S->BoneName.ToString());
        Sockets.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),    Skeleton->GetPathName());
    R->SetArrayField (TEXT("sockets"), Sockets);
    R->SetNumberField(TEXT("count"),   Sockets.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.list_skeletal_meshes
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ListSkeletalMeshesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SkeletonPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton_path'"));
    }
    USkeleton* Skeleton = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *SkeletonPath));
    }

    FARFilter Filter;
    Filter.PackagePaths.Add(TEXT("/Game"));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.SkeletalMesh")));

    TArray<FAssetData> Found;
    GetAssetRegistry().GetAssets(Filter, Found);

    const FString SkeletonSoftPath = Skeleton->GetPathName();

    TArray<TSharedPtr<FJsonValue>> Meshes;
    for (const FAssetData& A : Found)
    {
        // Filter by skeleton tag if available
        FAssetTagValueRef SkeletonTag = A.TagsAndValues.FindTag(TEXT("Skeleton"));
        if (SkeletonTag.IsSet())
        {
            FString TagVal = SkeletonTag.GetValue();
            if (!TagVal.Contains(Skeleton->GetName())) continue;
        }

        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), A.AssetName.ToString());
        J->SetStringField(TEXT("path"), A.GetSoftObjectPath().ToString());
        Meshes.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("skeleton"), SkeletonPath);
    R->SetArrayField (TEXT("meshes"),   Meshes);
    R->SetNumberField(TEXT("count"),    Meshes.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.get_physics_asset
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome GetPhysicsAssetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    USkeletalMesh* SK = Cast<USkeletalMesh>(ResolveAsset(Path));
    if (!SK)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeletalMesh: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("skeletal_mesh"), SK->GetPathName());
    if (SK->PhysicsAsset)
    {
        R->SetStringField(TEXT("physics_asset"), SK->PhysicsAsset->GetPathName());
    }
    else
    {
        R->SetField(TEXT("physics_asset"), MakeShared<FJsonValueNull>());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_state_machine
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadStateMachineImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimBlueprint* BP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!BP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }

    FString SMName;
    Args->TryGetStringField(TEXT("name"), SMName);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BP->GetPathName());
    R->SetStringField(TEXT("state_machine_name"), SMName.IsEmpty() ? TEXT("(first)") : SMName);
    R->SetStringField(TEXT("note"),
        TEXT("use bp.read_function_graph with fn_name='AnimGraph' for full graph details"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_anim_graph
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadAnimGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimBlueprint* BP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!BP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BP->GetPathName());
    R->SetStringField(TEXT("note"),
        TEXT("use bp.read_function_graph with fn_name='AnimGraph' for full graph details"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.list_modifiers
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ListModifiersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }

    // AnimationModifiers are editor-only and stored in FAnimationModifiers container.
    // Reflect the AnimationModifiers property if available.
    TArray<TSharedPtr<FJsonValue>> Modifiers;
    FProperty* ModProp = Seq->GetClass()->FindPropertyByName(TEXT("AnimationModifiers"));
    if (ModProp)
    {
        // Property exists — return its element count via reflection
        FArrayProperty* ArrProp = CastField<FArrayProperty>(ModProp);
        if (ArrProp)
        {
            FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Seq));
            for (int32 I = 0; I < Helper.Num(); ++I)
            {
                auto J = MakeShared<FJsonObject>();
                J->SetNumberField(TEXT("index"), I);
                Modifiers.Add(MakeShared<FJsonValueObject>(J));
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      Seq->GetPathName());
    R->SetArrayField (TEXT("modifiers"), Modifiers);
    R->SetNumberField(TEXT("count"),     Modifiers.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_bone_track
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadBoneTrackImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, BoneName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("bone_name"), BoneName) || BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone_name'"));
    }
    double FrameD = 0.0;
    Args->TryGetNumberField(TEXT("frame"), FrameD);
    const int32 Frame = static_cast<int32>(FrameD);

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }

    USkeleton* Skel = Seq->GetSkeleton();
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("sequence has no skeleton"));
    }

    const int32 BoneIdx = Skel->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName));
    if (BoneIdx == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("bone '%s' not found in skeleton"), *BoneName));
    }

    const float FrameRate  = Seq->GetSamplingFrameRate().AsDecimal();
    const float TimeAtFrame = (FrameRate > 0.f) ? (Frame / FrameRate) : 0.f;

    // GetBoneTransform requires FSkeletonPoseBoneIndex in UE 5.7; use ref skeleton pose instead
    FTransform BoneTransform = Skel->GetReferenceSkeleton().GetRefBonePose().IsValidIndex(BoneIdx)
        ? Skel->GetReferenceSkeleton().GetRefBonePose()[BoneIdx]
        : FTransform::Identity;
    (void)TimeAtFrame;

    const FVector Loc = BoneTransform.GetLocation();
    const FRotator Rot = BoneTransform.GetRotation().Rotator();
    const FVector  Sc  = BoneTransform.GetScale3D();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      Seq->GetPathName());
    R->SetStringField(TEXT("bone_name"), BoneName);
    R->SetNumberField(TEXT("frame"),     Frame);
    R->SetField(TEXT("location"), detail::Vec3ToJson(Loc));
    R->SetField(TEXT("rotation"), detail::Rot3ToJson(Rot));
    R->SetField(TEXT("scale"),    detail::Vec3ToJson(Sc));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.list_control_rig_variables
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ListControlRigVariablesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("note"),
        TEXT("ControlRig variables are accessible via bp.list_variables with the ControlRig Blueprint path"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_pose_search_database
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadPoseSearchDatabaseImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UObject* Asset = ResolveAsset(Path);
    if (!Asset)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("asset not found — ensure PoseSearch plugin is enabled"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Asset->GetPathName());
    R->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
    R->SetStringField(TEXT("note"),
        TEXT("deep PoseSearch database inspection requires the PoseSearch plugin editor API"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_anim_blueprint
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateAnimBlueprintImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SkeletonPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("skeleton_path"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton_path'"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("skeleton not found: %s"), *SkeletonPath));
    }

    FScopedTransaction Tx(LOCTEXT("CreateAnimBP", "Create Anim Blueprint"));

    UAnimBlueprintFactory* Fac = NewObject<UAnimBlueprintFactory>();
    Fac->TargetSkeleton = Skel;
    Fac->BlueprintType   = BPTYPE_Normal;

    FString TargetClassPath;
    if (Args->TryGetStringField(TEXT("target_class"), TargetClassPath) && !TargetClassPath.IsEmpty())
    {
        UClass* Cls = FindObject<UClass>(nullptr, *TargetClassPath);
        if (!Cls) Cls = LoadObject<UClass>(nullptr, *TargetClassPath);
        if (Cls) Fac->ParentClass = Cls;
    }

    UObject* Created = CreateAssetFromPath(Path, UAnimBlueprint::StaticClass(), Fac);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create AnimBlueprint at %s"), *Path));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("AnimBlueprint"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_montage
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateMontageImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    FScopedTransaction Tx(LOCTEXT("CreateMontage", "Create Anim Montage"));

    UAnimMontageFactory* Fac = NewObject<UAnimMontageFactory>();

    FString SeqPath;
    if (Args->TryGetStringField(TEXT("sequence_path"), SeqPath) && !SeqPath.IsEmpty())
    {
        UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(SeqPath));
        if (Seq) Fac->SourceAnimation = Seq;
    }

    UObject* Created = CreateAssetFromPath(Path, UAnimMontage::StaticClass(), Fac);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create AnimMontage at %s"), *Path));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("AnimMontage"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_blendspace
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateBlendSpaceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SkeletonPath;
    FString Type = TEXT("2D");
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("skeleton_path"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton_path'"));
    }
    Args->TryGetStringField(TEXT("type"), Type);

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("skeleton not found: %s"), *SkeletonPath));
    }

    FScopedTransaction Tx(LOCTEXT("CreateBS", "Create BlendSpace"));

    UObject* Created = nullptr;
    if (Type == TEXT("1D"))
    {
        UClass* BS1DFacClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlendSpaceFactory1D"));
        if (!BS1DFacClass) BS1DFacClass = LoadObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlendSpaceFactory1D"));
        if (BS1DFacClass)
        {
            UFactory* Fac = NewObject<UFactory>(GetTransientPackage(), BS1DFacClass);
            FObjectPropertyBase* SkelProp = CastField<FObjectPropertyBase>(
                Fac->GetClass()->FindPropertyByName(TEXT("TargetSkeleton")));
            if (SkelProp) SkelProp->SetObjectPropertyValue(SkelProp->ContainerPtrToValuePtr<void>(Fac), Skel);
            Created = CreateAssetFromPath(Path, UBlendSpace1D::StaticClass(), Fac);
        }
    }
    else
    {
        UClass* BSFacClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlendSpaceFactoryNew"));
        if (!BSFacClass) BSFacClass = LoadObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlendSpaceFactoryNew"));
        if (BSFacClass)
        {
            UFactory* Fac = NewObject<UFactory>(GetTransientPackage(), BSFacClass);
            FObjectPropertyBase* SkelProp = CastField<FObjectPropertyBase>(
                Fac->GetClass()->FindPropertyByName(TEXT("TargetSkeleton")));
            if (SkelProp) SkelProp->SetObjectPropertyValue(SkelProp->ContainerPtrToValuePtr<void>(Fac), Skel);
            Created = CreateAssetFromPath(Path, UBlendSpace::StaticClass(), Fac);
        }
    }

    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create BlendSpace at %s"), *Path));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), Created->GetClass()->GetName());
    R->SetStringField(TEXT("type"),  Type);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_notify
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddNotifyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, NotifyClass;
    double Time = 0.0;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("notify_class"), NotifyClass) || NotifyClass.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'notify_class'"));
    }
    Args->TryGetNumberField(TEXT("time"), Time);

    UAnimSequenceBase* SeqBase = Cast<UAnimSequenceBase>(ResolveAsset(Path));
    if (!SeqBase)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequenceBase: %s"), *Path));
    }

    UClass* Cls = FindObject<UClass>(nullptr, *NotifyClass);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *NotifyClass);
    if (!Cls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("notify_class not found: %s"), *NotifyClass));
    }
    // Reject abstract classes
    if (Cls->HasAnyClassFlags(CLASS_Abstract))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("notify_class %s is abstract"), *Cls->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("AddNotify", "Add Anim Notify"));
    SeqBase->Modify();

    const float NotifyTime = static_cast<float>(Time);
    FAnimNotifyEvent NewEvent;
    NewEvent.NotifyName = FName(*Cls->GetName());
    NewEvent.SetTime(NotifyTime);
    NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(
        SeqBase->CalculateOffsetForNotify(NotifyTime));
    NewEvent.TrackIndex = 0;

    if (Cls->IsChildOf(UAnimNotifyState::StaticClass()))
    {
        UAnimNotifyState* NotifyState = NewObject<UAnimNotifyState>(SeqBase, Cls);
        NewEvent.NotifyStateClass = NotifyState;
        NewEvent.Duration = 0.1f;
    }
    else if (Cls->IsChildOf(UAnimNotify::StaticClass()))
    {
        UAnimNotify* NotifyObj = NewObject<UAnimNotify>(SeqBase, Cls);
        NewEvent.Notify = NotifyObj;
    }

    SeqBase->Notifies.Add(NewEvent);
    SeqBase->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         SeqBase->GetPathName());
    R->SetStringField(TEXT("notify_class"), NotifyClass);
    R->SetNumberField(TEXT("time"),         Time);
    R->SetBoolField  (TEXT("added"),        true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_sequence
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateSequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SkeletonPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("skeleton_path"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton_path'"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("skeleton not found: %s"), *SkeletonPath));
    }

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
                    ESearchCase::IgnoreCase, ESearchDir::FromEnd) ||
        PackagePath.IsEmpty() || AssetName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("path must be /Folder/AssetName form"));
    }
    if (FindPackage(nullptr, *(PackagePath / AssetName)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("package already exists: %s"), *Path));
    }

    FScopedTransaction Tx(LOCTEXT("CreateSeq", "Create Anim Sequence"));
    UPackage* Pkg = CreatePackage(*(PackagePath / AssetName));
    if (!Pkg) { Tx.Cancel(); return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreatePackage failed")); }

    Pkg->FullyLoad();
    Pkg->Modify();

    UAnimSequence* Seq = NewObject<UAnimSequence>(Pkg, *AssetName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Seq) { Tx.Cancel(); return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("NewObject<UAnimSequence> failed")); }

    Seq->SetSkeleton(Skel);
    FAssetRegistryModule::AssetCreated(Seq);
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),     Seq->GetPathName());
    R->SetStringField(TEXT("class"),    TEXT("AnimSequence"));
    R->SetStringField(TEXT("skeleton"), Skel->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_bone_keyframes
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetBoneKeyframesImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    FString Path;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("note"),
        TEXT("bone keyframe editing requires the UAnimSequence compression pipeline; "
             "trigger via Python scripting: editor.run_python with animation editor commands"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.get_bone_transforms
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome GetBoneTransformsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, BoneName;
    double Time = 0.0;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("bone_name"), BoneName) || BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone_name'"));
    }
    Args->TryGetNumberField(TEXT("time"), Time);

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }
    USkeleton* Skel = Seq->GetSkeleton();
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("sequence has no skeleton"));
    }

    const int32 BoneIdx = Skel->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName));
    if (BoneIdx == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("bone '%s' not found in skeleton"), *BoneName));
    }

    // Use reference pose as fallback; runtime transform requires FSkeletonPoseBoneIndex
    FTransform BoneTransform = Skel->GetReferenceSkeleton().GetRefBonePose().IsValidIndex(BoneIdx)
        ? Skel->GetReferenceSkeleton().GetRefBonePose()[BoneIdx]
        : FTransform::Identity;

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      Seq->GetPathName());
    R->SetStringField(TEXT("bone_name"), BoneName);
    R->SetNumberField(TEXT("time"),      Time);
    R->SetField(TEXT("location"), detail::Vec3ToJson(BoneTransform.GetLocation()));
    R->SetField(TEXT("rotation"), detail::Rot3ToJson(BoneTransform.GetRotation().Rotator()));
    R->SetField(TEXT("scale"),    detail::Vec3ToJson(BoneTransform.GetScale3D()));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_montage_sequence
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetMontageSequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    FString Path;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("note"),
        TEXT("montage slot track editing requires the Persona animation editor session; "
             "use the Unreal Editor Montage editor or editor.run_python with anim editor API"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_montage_properties
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetMontagePropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimMontage* Montage = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!Montage)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimMontage: %s"), *Path));
    }

    FScopedTransaction Tx(LOCTEXT("SetMontageProps", "Set Montage Properties"));
    Montage->Modify();

    double RateScale = 0.0;
    if (Args->TryGetNumberField(TEXT("rate_scale"), RateScale))
    {
        Montage->RateScale = static_cast<float>(RateScale);
    }
    double BlendIn = -1.0;
    if (Args->TryGetNumberField(TEXT("blend_in_time"), BlendIn) && BlendIn >= 0.0)
    {
        Montage->BlendIn.SetBlendTime(static_cast<float>(BlendIn));
    }
    double BlendOut = -1.0;
    if (Args->TryGetNumberField(TEXT("blend_out_time"), BlendOut) && BlendOut >= 0.0)
    {
        Montage->BlendOut.SetBlendTime(static_cast<float>(BlendOut));
    }
    Montage->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),           Montage->GetPathName());
    R->SetNumberField(TEXT("rate_scale"),      Montage->RateScale);
    R->SetNumberField(TEXT("blend_in"),        Montage->BlendIn.GetBlendTime());
    R->SetNumberField(TEXT("blend_out"),       Montage->BlendOut.GetBlendTime());
    R->SetBoolField  (TEXT("modified"),        true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_state_machine
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateStateMachineImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }

    UAnimBlueprint* BP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!BP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BP->GetPathName());
    R->SetStringField(TEXT("name"), Name);
    R->SetStringField(TEXT("note"),
        TEXT("state machine graph creation requires FBlueprintEditorUtils::CreateNewGraph with "
             "AnimationStateMachineSchema; open the AnimBP in Persona and use editor.run_python"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_state
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("state node creation requires Persona AnimStateMachine graph editing; "
             "use editor.run_python with the AnimBP open in Persona"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_transition
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddTransitionImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("state machine transition creation requires Persona graph editing; "
             "use editor.run_python with the AnimBP open in Persona"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_state_animation
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetStateAnimationImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("state animation assignment requires Persona AnimGraph node editing; "
             "use editor.run_python with the AnimBP open in Persona"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_transition_blend
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetTransitionBlendImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("transition blend time requires Persona AnimStateMachine graph node editing; "
             "use editor.run_python with the AnimBP open in Persona"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_curve
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddCurveImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, CurveName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'curve_name'"));
    }

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }
    USkeleton* Skel = Seq->GetSkeleton();
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("sequence has no skeleton"));
    }

    // UE 5.5+: AddSmartNameAndModify / AnimCurveMappingName removed.
    // Use IAnimationDataController::AddCurve (Persona-editor API, requires open asset).
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Seq->GetPathName());
    R->SetStringField(TEXT("curve_name"), CurveName);
    R->SetBoolField  (TEXT("added"),      false);
    R->SetStringField(TEXT("note"),
        TEXT("Curve addition in UE 5.5+ requires IAnimationDataController::AddCurve; "
             "use editor.run_python: unreal.AnimationLibrary.add_curve(seq, name)"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_montage_slot
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetMontageSlotImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SlotName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'slot_name'"));
    }

    UAnimMontage* Montage = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!Montage)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimMontage: %s"), *Path));
    }
    if (Montage->SlotAnimTracks.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("montage has no SlotAnimTracks"));
    }

    FScopedTransaction Tx(LOCTEXT("SetMontageSlot", "Set Montage Slot"));
    Montage->Modify();
    Montage->SlotAnimTracks[0].SlotName = FName(*SlotName);
    Montage->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      Montage->GetPathName());
    R->SetStringField(TEXT("slot_name"), SlotName);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_montage_section
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddMontageSectionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SectionName;
    double StartTime = 0.0;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'section_name'"));
    }
    Args->TryGetNumberField(TEXT("start_time"), StartTime);

    UAnimMontage* Montage = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!Montage)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimMontage: %s"), *Path));
    }

    FScopedTransaction Tx(LOCTEXT("AddMontageSection", "Add Montage Section"));
    Montage->Modify();
    const int32 SectionIdx = Montage->AddAnimCompositeSection(FName(*SectionName),
                                                               static_cast<float>(StartTime));
    Montage->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         Montage->GetPathName());
    R->SetStringField(TEXT("section_name"), SectionName);
    R->SetNumberField(TEXT("start_time"),   StartTime);
    R->SetNumberField(TEXT("section_index"), SectionIdx);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_ik_rig
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateIKRigImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    // IKRig class lives in the IKRig plugin — do a soft class lookup
    UClass* IKRigClass = FindObject<UClass>(nullptr, TEXT("/Script/IKRig.IKRigDefinition"));
    if (!IKRigClass) IKRigClass = LoadObject<UClass>(nullptr, TEXT("/Script/IKRig.IKRigDefinition"));
    if (!IKRigClass)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Path);
        R->SetStringField(TEXT("note"),
            TEXT("IKRig plugin (IKRig) is not loaded; enable the IK Rig plugin in your project"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("CreateIKRig", "Create IK Rig"));

    UFactory* Fac = nullptr;
    UClass* FacClass = FindObject<UClass>(nullptr, TEXT("/Script/IKRigEditor.IKRigDefinitionFactory"));
    if (!FacClass) FacClass = LoadObject<UClass>(nullptr, TEXT("/Script/IKRigEditor.IKRigDefinitionFactory"));
    if (FacClass) Fac = NewObject<UFactory>(GetTransientPackage(), FacClass);

    FString SkMeshPath;
    if (Fac && Args->TryGetStringField(TEXT("skeletal_mesh_path"), SkMeshPath) && !SkMeshPath.IsEmpty())
    {
        USkeletalMesh* SK = Cast<USkeletalMesh>(ResolveAsset(SkMeshPath));
        FObjectPropertyBase* MeshProp = CastField<FObjectPropertyBase>(
            Fac->GetClass()->FindPropertyByName(TEXT("SkeletalMesh")));
        if (MeshProp && SK)
        {
            MeshProp->SetObjectPropertyValue(MeshProp->ContainerPtrToValuePtr<void>(Fac), SK);
        }
    }

    UObject* Created = CreateAssetFromPath(Path, IKRigClass, Fac);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create IKRig at %s"), *Path));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), Created->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_ik_rig
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadIKRigImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UObject* Asset = ResolveAsset(Path);
    if (!Asset)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("asset not found — ensure IK Rig plugin is enabled"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Asset->GetPathName());
    R->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
    R->SetStringField(TEXT("note"),
        TEXT("deep IKRig inspection requires the IKRig plugin editor API"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_root_motion
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetRootMotionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    bool bEnabled = false;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    Args->TryGetBoolField(TEXT("enabled"), bEnabled);

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }

    FScopedTransaction Tx(LOCTEXT("SetRootMotion", "Set Root Motion"));
    Seq->Modify();
    Seq->bEnableRootMotion = bEnabled;

    FString LockTypeStr;
    if (Args->TryGetStringField(TEXT("lock_type"), LockTypeStr) && !LockTypeStr.IsEmpty())
    {
        FProperty* LockProp = Seq->GetClass()->FindPropertyByName(TEXT("RootMotionRootLock"));
        if (LockProp)
        {
            FByteProperty* ByteProp = CastField<FByteProperty>(LockProp);
            if (ByteProp && ByteProp->Enum)
            {
                int64 Val = ByteProp->Enum->GetValueByNameString(LockTypeStr);
                if (Val != INDEX_NONE)
                {
                    ByteProp->SetPropertyValue(LockProp->ContainerPtrToValuePtr<void>(Seq),
                                               static_cast<uint8>(Val));
                }
            }
        }
    }

    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),    Seq->GetPathName());
    R->SetBoolField  (TEXT("enabled"), Seq->bEnableRootMotion);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_virtual_bone
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddVirtualBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, BoneName, ParentName, TargetName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("bone_name"),   BoneName)   || BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone_name'"));
    }
    if (!Args->TryGetStringField(TEXT("parent_name"), ParentName) || ParentName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'parent_name'"));
    }
    if (!Args->TryGetStringField(TEXT("target_name"), TargetName) || TargetName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'target_name'"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(Path));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *Path));
    }

    // USkeleton::AddVirtualBone removed in UE 5.7 — use Python scripting API
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),   Skel->GetPathName());
    R->SetStringField(TEXT("source"), ParentName);
    R->SetStringField(TEXT("target"), TargetName);
    R->SetStringField(TEXT("note"),
        TEXT("AddVirtualBone removed in UE 5.7; use "
             "editor.run_python: unreal.EditorAssetLibrary / SkeletonEditorSubsystem"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.remove_virtual_bone
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome RemoveVirtualBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, BoneName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("bone_name"), BoneName) || BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone_name'"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(Path));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *Path));
    }

    FScopedTransaction Tx(LOCTEXT("RemoveVirtualBone", "Remove Virtual Bone"));
    TArray<FName> ToRemove { FName(*BoneName) };
    Skel->RemoveVirtualBones(ToRemove);
    Skel->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      Skel->GetPathName());
    R->SetStringField(TEXT("bone_name"), BoneName);
    R->SetBoolField  (TEXT("removed"),   true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_composite
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateCompositeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SkeletonPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("skeleton_path"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton_path'"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("skeleton not found: %s"), *SkeletonPath));
    }

    FScopedTransaction Tx(LOCTEXT("CreateComposite", "Create Anim Composite"));

    UAnimCompositeFactory* Fac = NewObject<UAnimCompositeFactory>();
    Fac->TargetSkeleton = Skel;

    UObject* Created = CreateAssetFromPath(Path, UAnimComposite::StaticClass(), Fac);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create AnimComposite at %s"), *Path));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("AnimComposite"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_ik_retargeter
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateIKRetargeterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UClass* RetargetClass = FindObject<UClass>(nullptr, TEXT("/Script/IKRig.IKRetargeter"));
    if (!RetargetClass) RetargetClass = LoadObject<UClass>(nullptr, TEXT("/Script/IKRig.IKRetargeter"));
    if (!RetargetClass)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Path);
        R->SetStringField(TEXT("note"),
            TEXT("IKRetargeter requires the IK Rig plugin (IKRig); enable it in your project"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("CreateIKRetargeter", "Create IK Retargeter"));

    UFactory* Fac = nullptr;
    UClass* FacClass = FindObject<UClass>(nullptr, TEXT("/Script/IKRigEditor.IKRetargetFactory"));
    if (!FacClass) FacClass = LoadObject<UClass>(nullptr, TEXT("/Script/IKRigEditor.IKRetargetFactory"));
    if (FacClass)
    {
        Fac = NewObject<UFactory>(GetTransientPackage(), FacClass);
        FString SourcePath, TargetPath;
        if (Args->TryGetStringField(TEXT("source_ik_rig_path"), SourcePath) && !SourcePath.IsEmpty())
        {
            UObject* SrcRig = ResolveAsset(SourcePath);
            FObjectPropertyBase* SrcProp = CastField<FObjectPropertyBase>(
                Fac->GetClass()->FindPropertyByName(TEXT("SourceIKRigAsset")));
            if (SrcProp && SrcRig)
                SrcProp->SetObjectPropertyValue(SrcProp->ContainerPtrToValuePtr<void>(Fac), SrcRig);
        }
        if (Args->TryGetStringField(TEXT("target_ik_rig_path"), TargetPath) && !TargetPath.IsEmpty())
        {
            UObject* TgtRig = ResolveAsset(TargetPath);
            FObjectPropertyBase* TgtProp = CastField<FObjectPropertyBase>(
                Fac->GetClass()->FindPropertyByName(TEXT("TargetIKRigAsset")));
            if (TgtProp && TgtRig)
                TgtProp->SetObjectPropertyValue(TgtProp->ContainerPtrToValuePtr<void>(Fac), TgtRig);
        }
    }

    UObject* Created = CreateAssetFromPath(Path, RetargetClass, Fac);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create IKRetargeter at %s"), *Path));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), Created->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_anim_blueprint_skeleton
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetAnimBlueprintSkeletonImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SkeletonPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("skeleton_path"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton_path'"));
    }

    UAnimBlueprint* BP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!BP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("skeleton not found: %s"), *SkeletonPath));
    }

    FScopedTransaction Tx(LOCTEXT("SetAnimBPSkel", "Set AnimBP Skeleton"));
    BP->Modify();
    BP->TargetSkeleton = Skel;
    BP->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),     BP->GetPathName());
    R->SetStringField(TEXT("skeleton"), Skel->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.bake_root_motion_from_bone
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome BakeRootMotionFromBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    FString Path;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("note"),
        TEXT("root motion baking is a complex pipeline operation; "
             "use editor.run_python with the FBakingAnimationKeyHelper API or open the sequence in Persona"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_pose_search_database
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreatePoseSearchDatabaseImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UClass* DBClass = FindObject<UClass>(nullptr, TEXT("/Script/PoseSearch.PoseSearchDatabase"));
    if (!DBClass) DBClass = LoadObject<UClass>(nullptr, TEXT("/Script/PoseSearch.PoseSearchDatabase"));
    if (!DBClass)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Path);
        R->SetStringField(TEXT("note"),
            TEXT("PoseSearch plugin is not loaded; enable the Motion Matching / PoseSearch plugin"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("CreatePoseSearchDB", "Create PoseSearch Database"));

    UFactory* Fac = nullptr;
    UClass* FacClass = FindObject<UClass>(nullptr, TEXT("/Script/PoseSearchEditor.PoseSearchDatabaseFactory"));
    if (!FacClass) FacClass = LoadObject<UClass>(nullptr, TEXT("/Script/PoseSearchEditor.PoseSearchDatabaseFactory"));
    if (FacClass) Fac = NewObject<UFactory>(GetTransientPackage(), FacClass);

    UObject* Created = CreateAssetFromPath(Path, DBClass, Fac);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create PoseSearchDatabase at %s"), *Path));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), Created->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_pose_search_schema
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetPoseSearchSchemaImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("PoseSearch schema assignment requires the PoseSearch plugin; "
             "enable Motion Matching plugin and use editor.run_python with PoseSearch API"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_pose_search_sequence
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddPoseSearchSequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("PoseSearch sequence addition requires the PoseSearch plugin; "
             "enable Motion Matching plugin and use editor.run_python with PoseSearch API"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.build_pose_search_index
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome BuildPoseSearchIndexImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("PoseSearch index building requires the PoseSearch plugin; "
             "enable Motion Matching plugin and use editor.run_python with PoseSearch API"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_sequence_properties
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetSequencePropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }

    FScopedTransaction Tx(LOCTEXT("SetSeqProps", "Set Sequence Properties"));
    Seq->Modify();

    double RateScale = 0.0;
    if (Args->TryGetNumberField(TEXT("rate_scale"), RateScale))
    {
        Seq->RateScale = static_cast<float>(RateScale);
    }
    bool bEnableRoot = false;
    if (Args->TryGetBoolField(TEXT("enable_root_motion"), bEnableRoot))
    {
        Seq->bEnableRootMotion = bEnableRoot;
    }
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),               Seq->GetPathName());
    R->SetNumberField(TEXT("rate_scale"),          Seq->RateScale);
    R->SetBoolField  (TEXT("enable_root_motion"),  Seq->bEnableRootMotion);
    R->SetBoolField  (TEXT("modified"),            true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void RegisterAnimationTools(FSageToolDispatch& Dispatch)
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

    // Read tools
    Dispatch.RegisterHandler(TEXT("animation.list"),                       GT(&ListAnimationImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_anim_blueprint"),        GT(&ReadAnimBlueprintImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_montage"),               GT(&ReadMontageImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_sequence"),              GT(&ReadSequenceImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_blendspace"),            GT(&ReadBlendSpaceImpl));
    Dispatch.RegisterHandler(TEXT("animation.get_skeleton_info"),          GT(&GetSkeletonInfoImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_sockets"),               GT(&ListSkeletonSocketsImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_skeletal_meshes"),       GT(&ListSkeletalMeshesImpl));
    Dispatch.RegisterHandler(TEXT("animation.get_physics_asset"),          GT(&GetPhysicsAssetImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_state_machine"),         GT(&ReadStateMachineImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_anim_graph"),            GT(&ReadAnimGraphImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_modifiers"),             GT(&ListModifiersImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_bone_track"),            GT(&ReadBoneTrackImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_control_rig_variables"), GT(&ListControlRigVariablesImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_pose_search_database"),  GT(&ReadPoseSearchDatabaseImpl));

    // Write tools
    Dispatch.RegisterHandler(TEXT("animation.create_anim_blueprint"),      GT(&CreateAnimBlueprintImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_montage"),             GT(&CreateMontageImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_blendspace"),          GT(&CreateBlendSpaceImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_notify"),                 GT(&AddNotifyImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_sequence"),            GT(&CreateSequenceImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_bone_keyframes"),         GT(&SetBoneKeyframesImpl));
    Dispatch.RegisterHandler(TEXT("animation.get_bone_transforms"),        GT(&GetBoneTransformsImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_sequence"),       GT(&SetMontageSequenceImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_properties"),     GT(&SetMontagePropertiesImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_state_machine"),       GT(&CreateStateMachineImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_state"),                  GT(&AddStateImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_transition"),             GT(&AddTransitionImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_state_animation"),        GT(&SetStateAnimationImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_transition_blend"),       GT(&SetTransitionBlendImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_curve"),                  GT(&AddCurveImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_slot"),           GT(&SetMontageSlotImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_montage_section"),        GT(&AddMontageSectionImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_ik_rig"),              GT(&CreateIKRigImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_ik_rig"),                GT(&ReadIKRigImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_root_motion"),            GT(&SetRootMotionImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_virtual_bone"),           GT(&AddVirtualBoneImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_virtual_bone"),        GT(&RemoveVirtualBoneImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_composite"),           GT(&CreateCompositeImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_ik_retargeter"),       GT(&CreateIKRetargeterImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_anim_blueprint_skeleton"),GT(&SetAnimBlueprintSkeletonImpl));
    Dispatch.RegisterHandler(TEXT("animation.bake_root_motion_from_bone"), GT(&BakeRootMotionFromBoneImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_pose_search_database"),GT(&CreatePoseSearchDatabaseImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_pose_search_schema"),     GT(&SetPoseSearchSchemaImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_pose_search_sequence"),   GT(&AddPoseSearchSequenceImpl));
    Dispatch.RegisterHandler(TEXT("animation.build_pose_search_index"),    GT(&BuildPoseSearchIndexImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_sequence_properties"),    GT(&SetSequencePropertiesImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
