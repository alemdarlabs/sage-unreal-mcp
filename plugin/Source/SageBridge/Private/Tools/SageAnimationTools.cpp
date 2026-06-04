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
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/BlendProfile.h"
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
#include "Factories/BlueprintFactory.h"
#include "Features/IModularFeatures.h"
#include "IAssetTools.h"
#include "IPropertyAccessEditor.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

// AnimGraph + state machine authoring (Phase 4-r6, Lyra Sage Gap #16/#17/#18)
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_CustomProperty.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateAliasNode.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_SkeletalControlBase.h"
#include "AnimGraphNode_BlendListBase.h"
#include "AnimGraphNode_LinkedInputPose.h"  // Cluster G: ALI linked input pose parameters
#include "AnimGraphNode_LinkedAnimLayer.h"  // Cluster G: master AnimGraph linked-layer call
#include "Animation/AnimNode_LinkedAnimLayer.h"  // Cluster G: inner FAnimNode_LinkedAnimLayer
#include "Animation/AnimNodeBase.h"  // FPoseLink for pose-pin category check
#include "BoneControllers/AnimNode_SkeletalControlBase.h"  // FComponentSpacePoseLink
#include "Animation/AnimNode_SequencePlayer.h"  // FAnimNode_SequencePlayer for inner Node mutation
#include "AnimNodes/AnimNode_RetargetPoseFromMesh.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"  // FKismetNameValidator for RenameGraphWithSuggestion
#include "IAnimationModifiersModule.h"
#include "Misc/PackageName.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetOps.h"
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "Retargeter/RetargetOps/IKChainsOp.h"
#include "RigEditor/IKRigAutoCharacterizer.h"
#include "RigEditor/IKRigAutoFBIK.h"
#include "RigEditor/IKRigController.h"
#include "IKRigLogger.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigProcessor.h"
#include "Rig/IKRigSkeleton.h"

// IAnimationDataController for AnimSequence curve add (UE 5.5+ canonical path)
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AttributeCurve.h"
#include "Animation/AnimCurveTypes.h"
#include "AnimationModifier.h"
#include "AnimationModifiersAssetUserData.h"
#include "ControlRigBlueprintEditorLibrary.h"
#include "ControlRigBlueprintLegacy.h"
#include "EditorAnimUtils.h"
#include "Engine/Blueprint.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"

// IBlueprintGeneratedClass + Anim* class hierarchy
#include "Animation/AnimBlueprintGeneratedClass.h"

// Cluster J — runtime character.* (PIE-only)
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/RootMotionSource.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"  // TActorIterator
#include "Curves/CurveVector.h"  // FRootMotionSource_JumpForce::PathOffsetCurve
#include "Curves/CurveFloat.h"   // FRootMotionSource_JumpForce::TimeMappingCurve

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

FString ObjectPathForPackage(const FString& PackagePath)
{
    FString Clean = PackagePath;
    Clean.ReplaceInline(TEXT("\\"), TEXT("/"));
    const int32 Dot = Clean.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (Dot != INDEX_NONE)
    {
        Clean = Clean.Left(Dot);
    }
    FString AssetName;
    FString Unused;
    if (!Clean.Split(TEXT("/"), &Unused, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
    {
        AssetName = Clean;
    }
    return Clean + TEXT(".") + AssetName;
}

UObject* ResolveAssetOrPackage(const FString& Path)
{
    if (UObject* Obj = ResolveAsset(Path))
    {
        return Obj;
    }
    if (!Path.Contains(TEXT(".")))
    {
        return ResolveAsset(ObjectPathForPackage(Path));
    }
    return nullptr;
}

FString PackagePathForObjectPath(FString ObjectPath)
{
    ObjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
    const int32 Dot = ObjectPath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (Dot != INDEX_NONE)
    {
        ObjectPath = ObjectPath.Left(Dot);
    }
    return ObjectPath;
}

FString ExtractObjectPathString(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid() || Value->IsNull())
    {
        return FString();
    }
    FString Raw = Value->AsString().TrimStartAndEnd();
    int32 FirstQuote = INDEX_NONE;
    int32 LastQuote = INDEX_NONE;
    if (Raw.FindChar(TEXT('\''), FirstQuote)
        && Raw.FindLastChar(TEXT('\''), LastQuote)
        && LastQuote > FirstQuote)
    {
        Raw = Raw.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
    }
    return Raw;
}

FString CleanObjectReferenceLiteral(FString Raw)
{
    Raw = Raw.TrimStartAndEnd();
    while (Raw.Len() >= 2 && Raw.StartsWith(TEXT("(")) && Raw.EndsWith(TEXT(")")))
    {
        Raw = Raw.Mid(1, Raw.Len() - 2).TrimStartAndEnd();
    }
    if (Raw.Len() >= 2
        && ((Raw.StartsWith(TEXT("\"")) && Raw.EndsWith(TEXT("\"")))
            || (Raw.StartsWith(TEXT("'")) && Raw.EndsWith(TEXT("'")))))
    {
        Raw = Raw.Mid(1, Raw.Len() - 2).TrimStartAndEnd();
    }
    int32 FirstQuote = INDEX_NONE;
    int32 LastQuote = INDEX_NONE;
    if (Raw.FindChar(TEXT('\''), FirstQuote)
        && Raw.FindLastChar(TEXT('\''), LastQuote)
        && LastQuote > FirstQuote)
    {
        Raw = Raw.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1).TrimStartAndEnd();
    }
    return Raw;
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

TSharedRef<FJsonObject> TransformToJsonObject(const FTransform& T)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetField(TEXT("translation"), detail::Vec3ToJson(T.GetTranslation()));
    Obj->SetField(TEXT("location"),    detail::Vec3ToJson(T.GetLocation()));
    Obj->SetField(TEXT("rotation"),    detail::Rot3ToJson(T.GetRotation().Rotator()));
    Obj->SetField(TEXT("scale"),       detail::Vec3ToJson(T.GetScale3D()));
    return Obj;
}

TSharedRef<FJsonValue> TransformToJsonValue(const FTransform& T)
{
    return MakeShared<FJsonValueObject>(TransformToJsonObject(T));
}

bool ReadVectorArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, Arr) || !Arr || Arr->Num() < 3)
    {
        return false;
    }
    Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
    return true;
}

bool ReadRotatorArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FRotator& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, Arr) || !Arr || Arr->Num() < 3)
    {
        return false;
    }
    Out = FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
    return true;
}

bool ReadQuatArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FQuat& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, Arr) || !Arr || Arr->Num() < 4)
    {
        return false;
    }
    Out = FQuat(
        (*Arr)[0]->AsNumber(),
        (*Arr)[1]->AsNumber(),
        (*Arr)[2]->AsNumber(),
        (*Arr)[3]->AsNumber());
    Out.Normalize();
    return true;
}

bool ReadStringArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TArray<FString>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, Arr) || !Arr)
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& V : *Arr)
    {
        if (V.IsValid() && V->Type == EJson::String)
        {
            Out.Add(V->AsString());
        }
    }
    return true;
}

bool ReadAnimationTransform(const TSharedPtr<FJsonObject>& Obj, FTransform& Out)
{
    if (!Obj.IsValid())
    {
        return false;
    }

    FVector Translation = Out.GetTranslation();
    FVector Scale = Out.GetScale3D();
    FQuat Rotation = Out.GetRotation();
    FRotator Rotator;
    if (ReadVectorArray(Obj, TEXT("translation"), Translation) ||
        ReadVectorArray(Obj, TEXT("location"), Translation))
    {
        Out.SetTranslation(Translation);
    }
    if (ReadVectorArray(Obj, TEXT("scale"), Scale))
    {
        Out.SetScale3D(Scale);
    }
    if (ReadQuatArray(Obj, TEXT("quaternion"), Rotation))
    {
        Out.SetRotation(Rotation);
    }
    else if (ReadRotatorArray(Obj, TEXT("rotation"), Rotator))
    {
        Out.SetRotation(Rotator.Quaternion());
    }
    return true;
}

UClass* ResolveClassAsset(const FString& ClassPath)
{
    if (ClassPath.IsEmpty())
    {
        return nullptr;
    }
    if (UClass* Cls = FindObject<UClass>(nullptr, *ClassPath))
    {
        return Cls;
    }
    if (UClass* Cls = LoadObject<UClass>(nullptr, *ClassPath))
    {
        return Cls;
    }
    if (UObject* Obj = ResolveAsset(ClassPath))
    {
        if (UClass* Cls = Cast<UClass>(Obj))
        {
            return Cls;
        }
        if (UBlueprint* BP = Cast<UBlueprint>(Obj))
        {
            return BP->GeneratedClass;
        }
    }
    const FSoftClassPath SoftClass(ClassPath);
    return SoftClass.TryLoadClass<UObject>();
}

bool HasCurveNameFilter(const TSet<FName>& Names, FName Name)
{
    return Names.Num() == 0 || Names.Contains(Name);
}

FString RetargetSideToString(ERetargetSourceOrTarget Side)
{
    return Side == ERetargetSourceOrTarget::Source ? TEXT("source") : TEXT("target");
}

bool ParseRetargetSide(const FString& Raw, ERetargetSourceOrTarget& Out)
{
    if (Raw.Equals(TEXT("source"), ESearchCase::IgnoreCase) ||
        Raw.Equals(TEXT("src"), ESearchCase::IgnoreCase))
    {
        Out = ERetargetSourceOrTarget::Source;
        return true;
    }
    if (Raw.Equals(TEXT("target"), ESearchCase::IgnoreCase) ||
        Raw.Equals(TEXT("dst"), ESearchCase::IgnoreCase) ||
        Raw.Equals(TEXT("destination"), ESearchCase::IgnoreCase))
    {
        Out = ERetargetSourceOrTarget::Target;
        return true;
    }
    return false;
}

FString RetargetModeToString(EBoneTranslationRetargetingMode::Type Mode)
{
    switch (Mode)
    {
    case EBoneTranslationRetargetingMode::Animation:         return TEXT("Animation");
    case EBoneTranslationRetargetingMode::Skeleton:          return TEXT("Skeleton");
    case EBoneTranslationRetargetingMode::AnimationScaled:   return TEXT("AnimationScaled");
    case EBoneTranslationRetargetingMode::AnimationRelative: return TEXT("AnimationRelative");
    case EBoneTranslationRetargetingMode::OrientAndScale:    return TEXT("OrientAndScale");
    default:                                                 return TEXT("Unknown");
    }
}

bool ParseRetargetMode(const FString& Raw, EBoneTranslationRetargetingMode::Type& Out)
{
    if (Raw.Equals(TEXT("Animation"), ESearchCase::IgnoreCase))         { Out = EBoneTranslationRetargetingMode::Animation; return true; }
    if (Raw.Equals(TEXT("Skeleton"), ESearchCase::IgnoreCase))          { Out = EBoneTranslationRetargetingMode::Skeleton; return true; }
    if (Raw.Equals(TEXT("AnimationScaled"), ESearchCase::IgnoreCase))   { Out = EBoneTranslationRetargetingMode::AnimationScaled; return true; }
    if (Raw.Equals(TEXT("AnimationRelative"), ESearchCase::IgnoreCase)) { Out = EBoneTranslationRetargetingMode::AnimationRelative; return true; }
    if (Raw.Equals(TEXT("OrientAndScale"), ESearchCase::IgnoreCase))    { Out = EBoneTranslationRetargetingMode::OrientAndScale; return true; }
    return false;
}

int32 CountAnimationModifiers(UAnimSequence* Seq)
{
    if (!Seq)
    {
        return 0;
    }
    const UAnimationModifiersAssetUserData* UserData = Seq->GetAssetUserData<UAnimationModifiersAssetUserData>();
    return UserData ? UserData->GetAnimationModifierInstances().Num() : 0;
}

TSharedPtr<FJsonValue> JsonStringValue(const FString& Value)
{
    return MakeShared<FJsonValueString>(Value);
}

TSharedPtr<FJsonValue> JsonNumberValue(double Value)
{
    return MakeShared<FJsonValueNumber>(Value);
}

TSharedPtr<FJsonValue> JsonBoolValue(bool Value)
{
    return MakeShared<FJsonValueBoolean>(Value);
}

void NotifyObjectPropertyChanged(UObject* Obj, FProperty* Property)
{
    if (!Obj || !Property)
    {
        return;
    }
    FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
    Obj->PostEditChangeProperty(Event);
}

bool SetReflectedProperty(UObject* Obj, const TCHAR* PropertyName, const TSharedPtr<FJsonValue>& Value)
{
    if (!Obj)
    {
        return false;
    }
    FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), PropertyName);
    if (!Prop)
    {
        return false;
    }
    if (!detail::SetUPropertyFromJson(Obj, Prop, Value))
    {
        return false;
    }
    NotifyObjectPropertyChanged(Obj, Prop);
    return true;
}

bool SetStructField(UStruct* Struct, void* StructValue, const TCHAR* FieldName, const TSharedPtr<FJsonValue>& Value)
{
    if (!Struct || !StructValue)
    {
        return false;
    }
    FProperty* Field = Struct->FindPropertyByName(FName(FieldName));
    if (!Field)
    {
        return false;
    }
    void* FieldValue = Field->ContainerPtrToValuePtr<void>(StructValue);
    return detail::SetPropertyValueAtPtr(Field, FieldValue, Value);
}

FString GetObjectPathProperty(UObject* Obj, const TCHAR* PropertyName)
{
    if (!Obj)
    {
        return FString();
    }
    if (FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), PropertyName))
    {
        TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Obj, Prop);
        if (Value.IsValid() && Value->Type == EJson::String)
        {
            return Value->AsString();
        }
    }
    return FString();
}

bool ResolveExpectedObject(const FString& Path, const TCHAR* ExpectedClassPath, UObject*& OutObject, FString& OutError)
{
    OutObject = nullptr;
    if (Path.IsEmpty())
    {
        return true;
    }

    UObject* Obj = ResolveAsset(Path);
    if (!Obj)
    {
        OutError = FString::Printf(TEXT("asset not found: %s"), *Path);
        return false;
    }

    if (ExpectedClassPath && ExpectedClassPath[0] != TEXT('\0'))
    {
        UClass* ExpectedClass = FindObject<UClass>(nullptr, ExpectedClassPath);
        if (!ExpectedClass)
        {
            ExpectedClass = LoadObject<UClass>(nullptr, ExpectedClassPath);
        }
        if (ExpectedClass && !Obj->IsA(ExpectedClass))
        {
            OutError = FString::Printf(TEXT("asset %s is not a %s"), *Path, ExpectedClassPath);
            return false;
        }
    }

    OutObject = Obj;
    return true;
}

int32 ParseBlendSpaceAxisIndex(const TSharedPtr<FJsonObject>& Args, UBlendSpace* BlendSpace, FString& OutAxisName)
{
    FString Axis;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("axis"), Axis);
    }
    Axis = Axis.TrimStartAndEnd();
    OutAxisName = Axis;
    if (Axis.Equals(TEXT("X"), ESearchCase::IgnoreCase) || Axis.Equals(TEXT("0"), ESearchCase::IgnoreCase))
    {
        return 0;
    }
    if (Axis.Equals(TEXT("Y"), ESearchCase::IgnoreCase) || Axis.Equals(TEXT("1"), ESearchCase::IgnoreCase))
    {
        return 1;
    }
    if (Axis.Equals(TEXT("Z"), ESearchCase::IgnoreCase) || Axis.Equals(TEXT("2"), ESearchCase::IgnoreCase))
    {
        return 2;
    }
    return INDEX_NONE;
}

void GetAnimCurveCounts(const UAnimSequence* Seq, int32& OutFloatCurves, int32& OutTransformCurves, int32& OutAttributes)
{
    OutFloatCurves = 0;
    OutTransformCurves = 0;
    OutAttributes = 0;
    const IAnimationDataModel* Model = Seq ? Seq->GetDataModel() : nullptr;
    if (!Model)
    {
        return;
    }
    OutFloatCurves = Model->GetFloatCurves().Num();
    OutTransformCurves = Model->GetTransformCurves().Num();
    OutAttributes = Model->GetAttributes().Num();
}

void AppendAssetDataJson(const FAssetData& AssetData, TArray<TSharedPtr<FJsonValue>>& Out)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
    Obj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
    Obj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
    Obj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
    Out.Add(MakeShared<FJsonValueObject>(Obj));
}

FString AssetPathOrEmpty(const UObject* Obj)
{
    return Obj ? Obj->GetPathName() : FString();
}

bool TryGetAnyStringField(const TSharedPtr<FJsonObject>& Args, const TArray<const TCHAR*>& Names, FString& Out)
{
    if (!Args.IsValid())
    {
        return false;
    }
    for (const TCHAR* Name : Names)
    {
        FString Value;
        if (Args->TryGetStringField(Name, Value) && !Value.TrimStartAndEnd().IsEmpty())
        {
            Out = Value.TrimStartAndEnd();
            return true;
        }
    }
    return false;
}

bool TrySaveLoadedAssetIfRequested(UObject* Asset, bool bSave, TSharedRef<FJsonObject> Result)
{
    Result->SetBoolField(TEXT("save_requested"), bSave);
    if (!bSave)
    {
        return true;
    }
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
        Result->SetStringField(TEXT("save_error"), FString::Printf(TEXT("SaveLoadedAsset returned false for %s"), *Asset->GetPathName()));
    }
    return bSaved;
}

TArray<TSharedPtr<FJsonValue>> NamesToJsonArray(const TArray<FName>& Names)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FName& Name : Names)
    {
        Out.Add(MakeShared<FJsonValueString>(Name.ToString()));
    }
    return Out;
}

TSharedRef<FJsonObject> IKRigChainToJson(const FBoneChain& Chain, const UIKRigController* Controller)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Chain.ChainName.ToString());
    Obj->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
    Obj->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
    Obj->SetStringField(TEXT("goal"), Chain.IKGoalName.ToString());
    if (Controller)
    {
        TSet<int32> ChainIndices;
        Obj->SetBoolField(TEXT("valid"), Controller->ValidateChain(Chain.ChainName, nullptr, ChainIndices));
        Obj->SetNumberField(TEXT("bone_count"), ChainIndices.Num());
    }
    return Obj;
}

TSharedRef<FJsonObject> IKRigToJson(UIKRigDefinition* Rig, UIKRigController* Controller)
{
    auto R = MakeShared<FJsonObject>();
    if (!Rig)
    {
        R->SetBoolField(TEXT("valid"), false);
        return R;
    }

    R->SetBoolField(TEXT("valid"), true);
    R->SetStringField(TEXT("path"), Rig->GetPathName());
    R->SetStringField(TEXT("class"), Rig->GetClass()->GetName());
    R->SetStringField(TEXT("preview_skeletal_mesh"), Rig->PreviewSkeletalMesh.ToSoftObjectPath().ToString());

    USkeletalMesh* Mesh = Controller ? Controller->GetSkeletalMesh() : Rig->PreviewSkeletalMesh.LoadSynchronous();
    R->SetStringField(TEXT("skeletal_mesh"), AssetPathOrEmpty(Mesh));
    R->SetStringField(TEXT("retarget_root"), Controller ? Controller->GetRetargetRoot().ToString() : Rig->GetPelvis().ToString());

    const FIKRigSkeleton& RigSkeleton = Controller ? Controller->GetIKRigSkeleton() : Rig->GetSkeleton();
    R->SetNumberField(TEXT("bone_count"), RigSkeleton.BoneNames.Num());
    TArray<TSharedPtr<FJsonValue>> Bones;
    for (int32 BoneIndex = 0; BoneIndex < RigSkeleton.BoneNames.Num(); ++BoneIndex)
    {
        auto Bone = MakeShared<FJsonObject>();
        Bone->SetStringField(TEXT("name"), RigSkeleton.BoneNames[BoneIndex].ToString());
        Bone->SetNumberField(TEXT("index"), BoneIndex);
        Bone->SetNumberField(TEXT("parent_index"), RigSkeleton.ParentIndices.IsValidIndex(BoneIndex) ? RigSkeleton.ParentIndices[BoneIndex] : INDEX_NONE);
        Bone->SetBoolField(TEXT("excluded"), RigSkeleton.IsBoneExcluded(BoneIndex));
        if (RigSkeleton.RefPoseGlobal.IsValidIndex(BoneIndex))
        {
            Bone->SetObjectField(TEXT("ref_pose_global"), TransformToJsonObject(RigSkeleton.RefPoseGlobal[BoneIndex]));
        }
        Bones.Add(MakeShared<FJsonValueObject>(Bone));
    }
    R->SetArrayField(TEXT("bones"), Bones);
    R->SetArrayField(TEXT("excluded_bones"), NamesToJsonArray(RigSkeleton.ExcludedBones));

    TArray<TSharedPtr<FJsonValue>> Chains;
    const TArray<FBoneChain>& RetargetChains = Controller ? Controller->GetRetargetChains() : Rig->GetRetargetChains();
    for (const FBoneChain& Chain : RetargetChains)
    {
        Chains.Add(MakeShared<FJsonValueObject>(IKRigChainToJson(Chain, Controller)));
    }
    R->SetArrayField(TEXT("chains"), Chains);
    R->SetNumberField(TEXT("chain_count"), Chains.Num());

    TArray<TSharedPtr<FJsonValue>> Goals;
    int32 GoalConnectionCount = 0;
    const TArray<UIKRigEffectorGoal*>& AllGoals = Controller ? Controller->GetAllGoals() : Rig->GetGoalArray();
    for (const UIKRigEffectorGoal* Goal : AllGoals)
    {
        if (!Goal)
        {
            continue;
        }
        auto GoalObj = MakeShared<FJsonObject>();
        GoalObj->SetStringField(TEXT("name"), Goal->GoalName.ToString());
        GoalObj->SetStringField(TEXT("bone"), Goal->BoneName.ToString());
        GoalObj->SetNumberField(TEXT("position_alpha"), Goal->PositionAlpha);
        GoalObj->SetNumberField(TEXT("rotation_alpha"), Goal->RotationAlpha);
        GoalObj->SetObjectField(TEXT("current_transform"), TransformToJsonObject(Goal->CurrentTransform));
        GoalObj->SetObjectField(TEXT("initial_transform"), TransformToJsonObject(Goal->InitialTransform));
        if (Controller)
        {
            TArray<TSharedPtr<FJsonValue>> ConnectedSolverIndices;
            TArray<TSharedPtr<FJsonValue>> ConnectedSolverNames;
            for (int32 SolverIndex = 0; SolverIndex < Controller->GetNumSolvers(); ++SolverIndex)
            {
                if (Controller->IsGoalConnectedToSolver(Goal->GoalName, SolverIndex))
                {
                    ConnectedSolverIndices.Add(MakeShared<FJsonValueNumber>(SolverIndex));
                    ConnectedSolverNames.Add(MakeShared<FJsonValueString>(Controller->GetSolverUniqueName(SolverIndex)));
                    ++GoalConnectionCount;
                }
            }
            GoalObj->SetArrayField(TEXT("connected_solver_indices"), ConnectedSolverIndices);
            GoalObj->SetArrayField(TEXT("connected_solver_names"), ConnectedSolverNames);
            GoalObj->SetBoolField(TEXT("connected_to_any_solver"), ConnectedSolverIndices.Num() > 0);
        }
#if WITH_EDITORONLY_DATA
        GoalObj->SetBoolField(TEXT("expose_position"), Goal->bExposePosition);
        GoalObj->SetBoolField(TEXT("expose_rotation"), Goal->bExposeRotation);
#endif
        Goals.Add(MakeShared<FJsonValueObject>(GoalObj));
    }
    R->SetArrayField(TEXT("goals"), Goals);
    R->SetNumberField(TEXT("goal_count"), Goals.Num());
    R->SetNumberField(TEXT("goal_connection_count"), GoalConnectionCount);

    TArray<TSharedPtr<FJsonValue>> Solvers;
    const int32 NumSolvers = Controller ? Controller->GetNumSolvers() : Rig->GetSolverStructs().Num();
    for (int32 SolverIndex = 0; SolverIndex < NumSolvers; ++SolverIndex)
    {
        auto SolverObj = MakeShared<FJsonObject>();
        SolverObj->SetNumberField(TEXT("index"), SolverIndex);
        if (Controller)
        {
            SolverObj->SetStringField(TEXT("name"), Controller->GetSolverUniqueName(SolverIndex));
            SolverObj->SetBoolField(TEXT("enabled"), Controller->GetSolverEnabled(SolverIndex));
            SolverObj->SetStringField(TEXT("start_bone"), Controller->GetStartBone(SolverIndex).ToString());
            SolverObj->SetStringField(TEXT("end_bone"), Controller->GetEndBone(SolverIndex).ToString());
            if (const FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex))
            {
                SolverObj->SetStringField(TEXT("struct"), SolverStruct->GetScriptStruct() ? SolverStruct->GetScriptStruct()->GetPathName() : FString());
            }
        }
        else if (Rig->GetSolverStructs().IsValidIndex(SolverIndex))
        {
            const FInstancedStruct& SolverStruct = Rig->GetSolverStructs()[SolverIndex];
            SolverObj->SetStringField(TEXT("struct"), SolverStruct.GetScriptStruct() ? SolverStruct.GetScriptStruct()->GetPathName() : FString());
        }
        Solvers.Add(MakeShared<FJsonValueObject>(SolverObj));
    }
    R->SetArrayField(TEXT("solvers"), Solvers);
    R->SetNumberField(TEXT("solver_count"), Solvers.Num());

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (!Controller)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("UIKRigController unavailable; returned direct asset readback only")));
    }
    if (!Mesh)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("IK Rig has no preview skeletal mesh")));
    }
    R->SetArrayField(TEXT("warnings"), Warnings);
    return R;
}

TArray<TSharedPtr<FJsonValue>> TextArrayToJsonArray(const TArray<FText>& Texts)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FText& Text : Texts)
    {
        Out.Add(MakeShared<FJsonValueString>(Text.ToString()));
    }
    return Out;
}

TSharedRef<FJsonObject> IKRigMeshCompatibilityToJson(UIKRigDefinition* Rig, USkeletalMesh* Mesh)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("rig"), Rig ? Rig->GetPathName() : FString());
    R->SetStringField(TEXT("skeletal_mesh"), AssetPathOrEmpty(Mesh));
    R->SetBoolField(TEXT("compatible"), false);

    if (!Rig)
    {
        TArray<TSharedPtr<FJsonValue>> Errors;
        Errors.Add(MakeShared<FJsonValueString>(TEXT("IK Rig asset is null")));
        R->SetArrayField(TEXT("errors"), Errors);
        R->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
        R->SetArrayField(TEXT("messages"), TArray<TSharedPtr<FJsonValue>>());
        return R;
    }
    if (!Mesh)
    {
        TArray<TSharedPtr<FJsonValue>> Errors;
        Errors.Add(MakeShared<FJsonValueString>(TEXT("Skeletal mesh is null")));
        R->SetArrayField(TEXT("errors"), Errors);
        R->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
        R->SetArrayField(TEXT("messages"), TArray<TSharedPtr<FJsonValue>>());
        return R;
    }

    FIKRigLogger Logger;
    Logger.SetLogTarget(Rig);
    const FIKRigInputSkeleton InputSkeleton(Mesh);
    const bool bCompatible = FIKRigProcessor::IsIKRigCompatibleWithSkeleton(Rig, InputSkeleton, &Logger);
    R->SetBoolField(TEXT("compatible"), bCompatible);
    R->SetArrayField(TEXT("errors"), TextArrayToJsonArray(Logger.GetErrors()));
    R->SetArrayField(TEXT("warnings"), TextArrayToJsonArray(Logger.GetWarnings()));
    R->SetArrayField(TEXT("messages"), TextArrayToJsonArray(Logger.GetMessages()));
    R->SetNumberField(TEXT("candidate_bone_count"), InputSkeleton.BoneNames.Num());
    return R;
}

FSageToolDispatch::FOutcome ResolveIKRigController(
    const TSharedPtr<FJsonObject>& Args,
    UIKRigDefinition*& OutRig,
    UIKRigController*& OutController)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    OutRig = Cast<UIKRigDefinition>(ResolveAsset(Path));
    if (!OutRig)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UIKRigDefinition: %s"), *Path));
    }
    OutController = UIKRigController::GetController(OutRig);
    if (!OutController)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to get IK Rig controller: %s"), *Path));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(MakeShared<FJsonObject>());
}

FString RichCurveInterpToString(ERichCurveInterpMode Mode)
{
    switch (Mode)
    {
    case RCIM_Linear:   return TEXT("linear");
    case RCIM_Constant: return TEXT("constant");
    case RCIM_Cubic:    return TEXT("cubic");
    default:            return TEXT("none");
    }
}

TSharedRef<FJsonObject> RichCurveKeyToJson(const FRichCurveKey& Key)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("time"), Key.Time);
    Obj->SetNumberField(TEXT("value"), Key.Value);
    Obj->SetStringField(TEXT("interpolation"), RichCurveInterpToString(Key.InterpMode));
    Obj->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
    Obj->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
    Obj->SetNumberField(TEXT("arrive_tangent_weight"), Key.ArriveTangentWeight);
    Obj->SetNumberField(TEXT("leave_tangent_weight"), Key.LeaveTangentWeight);
    return Obj;
}

void AddRootMotionSummary(TSharedRef<FJsonObject> Result, const UAnimSequence* Seq)
{
    const IAnimationDataModel* Model = Seq ? Seq->GetDataModel() : nullptr;
    USkeleton* Skel = Seq ? Seq->GetSkeleton() : nullptr;
    if (!Model || !Skel || Skel->GetReferenceSkeleton().GetNum() == 0)
    {
        return;
    }

    const FName RootBone = Skel->GetReferenceSkeleton().GetBoneName(0);
    TArray<FTransform> RootTransforms;
    Model->GetBoneTrackTransforms(RootBone, RootTransforms);
    if (RootTransforms.Num() == 0)
    {
        return;
    }

    const FTransform& First = RootTransforms[0];
    const FTransform& Last = RootTransforms.Last();
    const FVector TotalTranslation = Last.GetLocation() - First.GetLocation();
    const FQuat TotalRotation = Last.GetRotation() * First.GetRotation().Inverse();

    auto Summary = MakeShared<FJsonObject>();
    Summary->SetStringField(TEXT("root_bone"), RootBone.ToString());
    Summary->SetField(TEXT("total_translation"), detail::Vec3ToJson(TotalTranslation));
    Summary->SetField(TEXT("total_rotation"), detail::Rot3ToJson(TotalRotation.Rotator()));
    Summary->SetNumberField(TEXT("total_distance"), TotalTranslation.Size());
    Summary->SetNumberField(TEXT("total_yaw_degrees"), TotalRotation.Rotator().Yaw);
    const bool bHasRootMotion = TotalTranslation.Size() > 1.0 || FMath::Abs(TotalRotation.GetAngle()) > FMath::DegreesToRadians(1.0);
    Summary->SetStringField(TEXT("classification"), bHasRootMotion ? TEXT("root_motion") : TEXT("in_place"));
    Result->SetObjectField(TEXT("root_motion"), Summary);
}

void AddAnimSequenceTrackReadback(TSharedRef<FJsonObject> Result,
                                  const UAnimSequence* Seq,
                                  const FString& BoneName,
                                  int32 StartFrame,
                                  int32 EndFrame)
{
    const IAnimationDataModel* Model = Seq ? Seq->GetDataModel() : nullptr;
    if (!Model) return;

    TArray<FTransform> Transforms;
    Model->GetBoneTrackTransforms(FName(*BoneName), Transforms);
    const int32 NumKeys = Transforms.Num();
    if (NumKeys == 0)
    {
        TArray<TSharedPtr<FJsonValue>> EmptyKeys;
        Result->SetArrayField(TEXT("keys"), EmptyKeys);
        Result->SetNumberField(TEXT("key_count"), 0);
        return;
    }

    StartFrame = FMath::Clamp(StartFrame, 0, NumKeys - 1);
    EndFrame = EndFrame < 0 ? NumKeys - 1 : FMath::Clamp(EndFrame, 0, NumKeys - 1);
    if (EndFrame < StartFrame)
    {
        Swap(StartFrame, EndFrame);
    }

    const FFrameRate FrameRate = Model->GetFrameRate();
    const double FrameRateDecimal = FrameRate.AsDecimal();
    TArray<TSharedPtr<FJsonValue>> Keys;
    for (int32 Frame = StartFrame; Frame <= EndFrame; ++Frame)
    {
        const FTransform& T = Transforms[Frame];
        auto K = TransformToJsonObject(T);
        K->SetNumberField(TEXT("frame"), Frame);
        K->SetNumberField(TEXT("time"), FrameRateDecimal > 0.0 ? Frame / FrameRateDecimal : 0.0);
        Keys.Add(MakeShared<FJsonValueObject>(K));
    }
    Result->SetArrayField(TEXT("keys"), Keys);
    Result->SetNumberField(TEXT("key_count"), NumKeys);
    Result->SetNumberField(TEXT("returned_key_count"), Keys.Num());
    Result->SetNumberField(TEXT("start_frame"), StartFrame);
    Result->SetNumberField(TEXT("end_frame"), EndFrame);
}

// ---------------------------------------------------------------------------
// AnimGraph helpers (Lyra Sage Gap #16/#20 — state machine + node CRUD)
// ---------------------------------------------------------------------------

// Resolve the AnimGraph (UAnimationGraphSchema-bound graph) on an AnimBP.
UEdGraph* FindAnimGraph(UAnimBlueprint* AnimBP)
{
    if (!AnimBP) return nullptr;
    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (Graph && Graph->Schema
            && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
        {
            return Graph;
        }
    }
    return nullptr;
}

void AddUniqueGraph(TArray<UEdGraph*>& Out, TSet<UEdGraph*>& Seen, UEdGraph* Graph)
{
    if (!Graph || Seen.Contains(Graph))
    {
        return;
    }
    Seen.Add(Graph);
    Out.Add(Graph);
}

UEdGraph* GetStateBoundGraph(UAnimStateNodeBase* State)
{
    if (!State) return nullptr;
    if (UAnimStateNode* AsState = Cast<UAnimStateNode>(State))
    {
        return AsState->BoundGraph;
    }
    if (UAnimStateConduitNode* AsConduit = Cast<UAnimStateConduitNode>(State))
    {
        return AsConduit->BoundGraph;
    }
    return nullptr;
}

FString GetStateDisplayName(UAnimStateNodeBase* State)
{
    if (!State) return FString();
    FString Name = State->GetStateName();
    if ((Name.IsEmpty() || Name == TEXT("BaseState")) && GetStateBoundGraph(State))
    {
        Name = GetStateBoundGraph(State)->GetFName().ToString();
    }
    return Name;
}

// Collect every AnimGraph-like graph that can legally contain UAnimGraphNode_*
// nodes: the root AnimGraph, anim layer override graphs, and nested state
// bound graphs reachable through state-machine nodes. Transition rule graphs
// are intentionally not included here because they are K2 boolean graphs.
void CollectAnimBlueprintAnimGraphs(UAnimBlueprint* AnimBP, TArray<UEdGraph*>& Out)
{
    Out.Reset();
    if (!AnimBP) return;

    TSet<UEdGraph*> Seen;
    TFunction<void(UEdGraph*)> AddRecursive = [&](UEdGraph* Graph)
    {
        if (!Graph || Seen.Contains(Graph))
        {
            return;
        }
        AddUniqueGraph(Out, Seen, Graph);

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UAnimGraphNode_StateMachineBase* SMNode =
                Cast<UAnimGraphNode_StateMachineBase>(Node);
            UAnimationStateMachineGraph* SMGraph = SMNode
                ? Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph)
                : nullptr;
            if (!SMGraph) continue;

            for (UEdGraphNode* SMGraphNode : SMGraph->Nodes)
            {
                if (UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(SMGraphNode))
                {
                    AddRecursive(GetStateBoundGraph(State));
                }
            }
        }
    };

    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (Graph && Graph->Schema
            && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
        {
            AddRecursive(Graph);
        }
    }

    for (FBPInterfaceDescription& Impl : AnimBP->ImplementedInterfaces)
    {
        for (UEdGraph* Graph : Impl.Graphs)
        {
            if (Graph && Graph->Schema
                && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
            {
                AddRecursive(Graph);
            }
        }
    }
}

struct FStateMachineReference
{
    UAnimGraphNode_StateMachineBase* Node = nullptr;
    UEdGraph* ContainerGraph = nullptr;
};

TArray<FStateMachineReference> FindStateMachineReferences(
    UAnimBlueprint* AnimBP,
    const UAnimationStateMachineGraph* TargetGraph)
{
    TArray<FStateMachineReference> Refs;
    TArray<UEdGraph*> Graphs;
    CollectAnimBlueprintAnimGraphs(AnimBP, Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UAnimGraphNode_StateMachineBase* SMNode =
                Cast<UAnimGraphNode_StateMachineBase>(Node);
            if (!SMNode || !SMNode->EditorStateMachineGraph) continue;
            if (TargetGraph && SMNode->EditorStateMachineGraph != TargetGraph) continue;
            Refs.Add(FStateMachineReference{SMNode, Graph});
        }
    }
    return Refs;
}

TArray<UAnimationStateMachineGraph*> CollectStateMachineGraphs(UAnimBlueprint* AnimBP)
{
    TArray<UAnimationStateMachineGraph*> Out;
    TSet<UAnimationStateMachineGraph*> Seen;
    for (const FStateMachineReference& Ref : FindStateMachineReferences(AnimBP, nullptr))
    {
        UAnimationStateMachineGraph* Graph = Ref.Node
            ? Cast<UAnimationStateMachineGraph>(Ref.Node->EditorStateMachineGraph)
            : nullptr;
        if (!Graph || Seen.Contains(Graph)) continue;
        Seen.Add(Graph);
        Out.Add(Graph);
    }
    return Out;
}

// Locate a state machine sub-graph by name. State machines are stored as
// EditorStateMachineGraph on UAnimGraphNode_StateMachineBase nodes inside
// AnimGraph-like graphs (not directly on AnimBP->FunctionGraphs).
UAnimationStateMachineGraph* FindStateMachineGraph(UAnimBlueprint* AnimBP, const FName& Name)
{
    if (!AnimBP) return nullptr;
    for (UAnimationStateMachineGraph* Graph : CollectStateMachineGraphs(AnimBP))
    {
        if (Graph && Graph->GetFName() == Name)
        {
            return Graph;
        }
    }
    return nullptr;
}

// Parse a state/transition id (FGuid serialized). Accepts both default and
// Digits format — clients can round-trip whatever GetGuid() returned.
bool ParseNodeGuid(const FString& IdString, FGuid& OutGuid)
{
    if (IdString.IsEmpty()) return false;
    if (FGuid::Parse(IdString, OutGuid)) return true;
    return FGuid::ParseExact(IdString, EGuidFormats::Digits, OutGuid);
}

UAnimStateNodeBase* FindStateNodeByGuid(UEdGraph* SMGraph, const FString& IdString)
{
    if (!SMGraph) return nullptr;
    FGuid Id;
    if (!ParseNodeGuid(IdString, Id)) return nullptr;
    for (UEdGraphNode* N : SMGraph->Nodes)
    {
        UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(N);
        if (StateNode && StateNode->NodeGuid == Id) return StateNode;
    }
    return nullptr;
}

UAnimStateTransitionNode* FindTransitionByGuid(UEdGraph* SMGraph, const FString& IdString)
{
    if (!SMGraph) return nullptr;
    FGuid Id;
    if (!ParseNodeGuid(IdString, Id)) return nullptr;
    for (UEdGraphNode* N : SMGraph->Nodes)
    {
        UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N);
        if (T && T->NodeGuid == Id) return T;
    }
    return nullptr;
}

// AnimGraph "Output Pose" sink — root AnimGraph uses UAnimGraphNode_Root,
// state BoundGraphs (UAnimationStateGraph) use UAnimGraphNode_StateResult.
// Returning UEdGraphNode* covers both.
UEdGraphNode* FindAnimGraphOutput(UEdGraph* Graph)
{
    if (!Graph) return nullptr;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (N && (N->IsA<UAnimGraphNode_Root>() || N->IsA<UAnimGraphNode_StateResult>()))
        {
            return N;
        }
    }
    return nullptr;
}

// Forward declarations for helpers defined later in the file (Cluster A core).
UEdGraph* ResolveAnimGraphTarget(UAnimBlueprint* AnimBP, const FString& GraphName);

struct FAnimGraphReadOptions
{
    bool bIncludeProperties = false;
    bool bIncludePins = false;
    bool bIncludeConnections = false;
    FString NodeClassFilter;
    FString AssetSubstringFilter;
    TSet<FString> NodeIds;
};

void ReadAnimGraphOptions(
    const TSharedPtr<FJsonObject>& Args,
    FAnimGraphReadOptions& Options,
    bool bDefaultProperties,
    bool bDefaultPins,
    bool bDefaultConnections);
FString GraphKind(UEdGraph* Graph);
TSharedPtr<FJsonObject> AnimGraphToJson(
    UAnimBlueprint* AnimBP,
    UEdGraph* Graph,
    const FAnimGraphReadOptions& Options);
UAnimStateNodeBase* ResolveStateNodeForRead(
    UAnimBlueprint* AnimBP,
    const FString& StateMachineName,
    const FString& StateName,
    const FString& StateId,
    UAnimationStateMachineGraph*& OutSMGraph,
    FString& OutError);

// Pose-pin predicate. AnimGraph: PinCategory == PC_Struct, SubCategoryObject
// == FPoseLink::StaticStruct() OR FComponentSpacePoseLink::StaticStruct().
// State machine transitions use category "Transition". Filtering this way
// avoids returning data pins (Alpha/Sequence ref) on nodes like
// UAnimGraphNode_BlendListByBool / UAnimGraphNode_LayeredBoneBlend.
bool IsPosePin(const UEdGraphPin* Pin)
{
    if (!Pin) return false;
    if (Pin->PinType.PinCategory == TEXT("Transition"))
    {
        return true;
    }
    if (Pin->PinType.PinCategory == UAnimationGraphSchema::PC_Struct)
    {
        UScriptStruct* SS = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
        if (SS == FPoseLink::StaticStruct()) return true;
        if (SS == FComponentSpacePoseLink::StaticStruct()) return true;
    }
    return false;
}

// Strict pose-pin lookup. Drops the "first directional pin" fallback so callers
// can detect lookup failure and either pass an explicit pin name or surface an
// error to the user (Lyra Sage Gap #16/Cluster A audit — silent fallback was
// landing data pins like Alpha/Sequence on BlendList / SkeletalControl nodes).
UEdGraphPin* FindFirstOutputPosePin(UEdGraphNode* Node)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output && IsPosePin(Pin)) return Pin;
    }
    return nullptr;
}

UEdGraphPin* FindFirstInputPosePin(UEdGraphNode* Node)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input && IsPosePin(Pin)) return Pin;
    }
    return nullptr;
}

UEdGraphPin* FindFirstInputPinByCategory(UEdGraphNode* Node, const FName& Category)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input
            && Pin->PinType.PinCategory == Category)
        {
            return Pin;
        }
    }
    return nullptr;
}

UEdGraphPin* FindFirstOutputPinByCategory(UEdGraphNode* Node, const FName& Category)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == Category)
        {
            return Pin;
        }
    }
    return nullptr;
}

FString DescribePinsForError(const UEdGraphNode* Node)
{
    if (!Node) return TEXT("<null node>");
    FString Out = FString::Printf(TEXT("%s pins=["), *Node->GetClass()->GetName());
    bool bFirst = true;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin) continue;
        if (!bFirst) Out += TEXT("; ");
        bFirst = false;
        Out += FString::Printf(TEXT("%s dir=%s cat=%s sub=%s links=%d"),
            *Pin->PinName.ToString(),
            Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"),
            *Pin->PinType.PinCategory.ToString(),
            *Pin->PinType.PinSubCategory.ToString(),
            Pin->LinkedTo.Num());
    }
    Out += TEXT("]");
    return Out;
}

TSharedPtr<FJsonObject> PinSummaryJson(const UEdGraphPin* Pin, bool bIncludeLinks = true)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Pin)
    {
        Obj->SetBoolField(TEXT("found"), false);
        return Obj;
    }
    Obj->SetBoolField(TEXT("found"), true);
    Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
    Obj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    Obj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
    Obj->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
    Obj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    Obj->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
    if (!bIncludeLinks)
    {
        return Obj;
    }
    TArray<TSharedPtr<FJsonValue>> Links;
    for (const UEdGraphPin* Linked : Pin->LinkedTo)
    {
        if (!Linked) continue;
        TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
        Link->SetStringField(TEXT("pin"), Linked->PinName.ToString());
        if (const UEdGraphNode* Owner = Linked->GetOwningNode())
        {
            Link->SetStringField(TEXT("node_id"), Owner->NodeGuid.ToString(EGuidFormats::Digits));
            Link->SetStringField(TEXT("node_class"), Owner->GetClass()->GetName());
        }
        Links.Add(MakeShared<FJsonValueObject>(Link));
    }
    Obj->SetArrayField(TEXT("linked_nodes"), Links);
    return Obj;
}

UAnimGraphNode_TransitionResult* FindTransitionResultNode(UAnimationTransitionGraph* Graph)
{
    if (!Graph) return nullptr;
    if (UAnimGraphNode_TransitionResult* Result = Graph->GetResultNode())
    {
        return Result;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_TransitionResult* Result = Cast<UAnimGraphNode_TransitionResult>(Node))
        {
            return Result;
        }
    }
    return nullptr;
}

UEdGraphPin* FindCanEnterTransitionPin(UAnimGraphNode_TransitionResult* ResultNode)
{
    if (!ResultNode) return nullptr;
    if (UEdGraphPin* Pin = ResultNode->FindPin(TEXT("bCanEnterTransition"), EGPD_Input))
    {
        return Pin;
    }
    return FindFirstInputPinByCategory(ResultNode, UEdGraphSchema_K2::PC_Boolean);
}

UEdGraphPin* FindBoolReturnPin(UEdGraphNode* Node)
{
    if (!Node) return nullptr;
    if (UEdGraphPin* Pin = Node->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output))
    {
        return Pin;
    }
    return FindFirstOutputPinByCategory(Node, UEdGraphSchema_K2::PC_Boolean);
}

bool ParseLiteralBoolExpression(FString Expression, bool& OutValue)
{
    Expression.TrimStartAndEndInline();
    if (Expression.Equals(TEXT("true"), ESearchCase::IgnoreCase)
        || Expression == TEXT("1"))
    {
        OutValue = true;
        return true;
    }
    if (Expression.Equals(TEXT("false"), ESearchCase::IgnoreCase)
        || Expression == TEXT("0"))
    {
        OutValue = false;
        return true;
    }
    return false;
}

enum class ETransitionRuleValueKind
{
    Unknown,
    Bool,
    Int,
    Int64,
    Real,
    String,
    Name,
    Byte
};

FString RuleValueKindName(ETransitionRuleValueKind Kind)
{
    switch (Kind)
    {
    case ETransitionRuleValueKind::Bool:   return TEXT("bool");
    case ETransitionRuleValueKind::Int:    return TEXT("int");
    case ETransitionRuleValueKind::Int64:  return TEXT("int64");
    case ETransitionRuleValueKind::Real:   return TEXT("real");
    case ETransitionRuleValueKind::String: return TEXT("string");
    case ETransitionRuleValueKind::Name:   return TEXT("name");
    case ETransitionRuleValueKind::Byte:   return TEXT("byte");
    default:                               return TEXT("unknown");
    }
}

bool IsNumericRuleKind(ETransitionRuleValueKind Kind)
{
    return Kind == ETransitionRuleValueKind::Int
        || Kind == ETransitionRuleValueKind::Int64
        || Kind == ETransitionRuleValueKind::Real
        || Kind == ETransitionRuleValueKind::Byte;
}

ETransitionRuleValueKind RuleKindFromPinType(const FEdGraphPinType& PinType)
{
    const FName& Category = PinType.PinCategory;
    if (Category == UEdGraphSchema_K2::PC_Boolean) return ETransitionRuleValueKind::Bool;
    if (Category == UEdGraphSchema_K2::PC_Int)     return ETransitionRuleValueKind::Int;
    if (Category == UEdGraphSchema_K2::PC_Int64)   return ETransitionRuleValueKind::Int64;
    if (Category == UEdGraphSchema_K2::PC_Real
        || Category == UEdGraphSchema_K2::PC_Float
        || Category == UEdGraphSchema_K2::PC_Double)
    {
        return ETransitionRuleValueKind::Real;
    }
    if (Category == UEdGraphSchema_K2::PC_String) return ETransitionRuleValueKind::String;
    if (Category == UEdGraphSchema_K2::PC_Name)   return ETransitionRuleValueKind::Name;
    if (Category == UEdGraphSchema_K2::PC_Byte)   return ETransitionRuleValueKind::Byte;
    return ETransitionRuleValueKind::Unknown;
}

struct FTransitionRuleValue
{
    ETransitionRuleValueKind Kind = ETransitionRuleValueKind::Unknown;
    UEdGraphPin* Pin = nullptr;
    FString DefaultValue;
    bool bLiteral = false;
    bool bIntegralNumber = false;
    FString Debug;
};

struct FTransitionRuleBuildContext
{
    UAnimBlueprint* AnimBP = nullptr;
    UAnimationTransitionGraph* RuleGraph = nullptr;
    const UEdGraphSchema* Schema = nullptr;
    const UEdGraphSchema_K2* K2Schema = nullptr;
    int32 BaseX = 0;
    int32 BaseY = 0;
    int32 NodeIndex = 0;
    FString Error;
    TArray<TSharedPtr<FJsonValue>> AuthoredNodes;
};

FTransitionRuleValue BuildTransitionRuleExpression(FTransitionRuleBuildContext& Ctx, const TSharedPtr<FJsonValue>& Expr);
FTransitionRuleValue BuildTransitionRuleStringExpression(FTransitionRuleBuildContext& Ctx, FString Expression);

bool HasTransitionRuleVariable(UAnimBlueprint* AnimBP, const FName& VarName)
{
    if (!AnimBP || VarName.IsNone()) return false;
    if (FBlueprintEditorUtils::FindNewVariableIndex(AnimBP, VarName) != INDEX_NONE)
    {
        return true;
    }
    UClass* Classes[] = {
        AnimBP->SkeletonGeneratedClass,
        AnimBP->GeneratedClass,
        AnimBP->ParentClass
    };
    for (UClass* Cls : Classes)
    {
        if (Cls && FindFProperty<FProperty>(Cls, VarName))
        {
            return true;
        }
    }
    return false;
}

void PositionTransitionRuleNode(FTransitionRuleBuildContext& Ctx, UEdGraphNode* Node)
{
    if (!Node) return;
    const int32 Column = Ctx.NodeIndex % 4;
    const int32 Row = Ctx.NodeIndex / 4;
    Node->NodePosX = Ctx.BaseX - (Column * 300);
    Node->NodePosY = Ctx.BaseY + (Row * 140);
    ++Ctx.NodeIndex;
}

void AddAuthoredRuleNode(FTransitionRuleBuildContext& Ctx, const UEdGraphNode* Node, const FString& Kind)
{
    if (!Node) return;
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::Digits));
    Obj->SetStringField(TEXT("kind"), Kind);
    Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
    Obj->SetNumberField(TEXT("x"), Node->NodePosX);
    Obj->SetNumberField(TEXT("y"), Node->NodePosY);
    Ctx.AuthoredNodes.Add(MakeShared<FJsonValueObject>(Obj));
}

UK2Node_CallFunction* CreateTransitionRuleFunctionNode(
    FTransitionRuleBuildContext& Ctx,
    UClass* FunctionOwner,
    const FName& FunctionName,
    const FString& Kind)
{
    if (!Ctx.RuleGraph)
    {
        Ctx.Error = TEXT("rule graph missing");
        return nullptr;
    }
    if (!FunctionOwner)
    {
        Ctx.Error = FString::Printf(TEXT("function owner missing for %s"), *FunctionName.ToString());
        return nullptr;
    }
    UFunction* Fn = FunctionOwner->FindFunctionByName(FunctionName);
    if (!Fn)
    {
        Ctx.Error = FString::Printf(TEXT("function not found: %s.%s"),
            *FunctionOwner->GetName(), *FunctionName.ToString());
        return nullptr;
    }

    UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Ctx.RuleGraph);
    Node->CreateNewGuid();
    PositionTransitionRuleNode(Ctx, Node);
    Node->SetFromFunction(Fn);
    Ctx.RuleGraph->AddNode(Node, /*bSelectNewNode=*/false, /*bFromUI=*/true);
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    AddAuthoredRuleNode(Ctx, Node, Kind);
    return Node;
}

UEdGraphPin* FindVariableGetOutputPin(UK2Node_VariableGet* Node, const FName& VarName)
{
    if (!Node) return nullptr;
    if (UEdGraphPin* Pin = Node->FindPin(VarName, EGPD_Output))
    {
        return Pin;
    }
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            return Pin;
        }
    }
    return nullptr;
}

FTransitionRuleValue BuildTransitionRuleVariable(FTransitionRuleBuildContext& Ctx, const FString& VarName)
{
    FTransitionRuleValue Out;
    const FName VarFName(*VarName);
    if (!HasTransitionRuleVariable(Ctx.AnimBP, VarFName))
    {
        Ctx.Error = FString::Printf(TEXT("transition rule variable not found on AnimBlueprint/self: %s"), *VarName);
        return Out;
    }

    UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Ctx.RuleGraph);
    Node->CreateNewGuid();
    PositionTransitionRuleNode(Ctx, Node);
    Node->VariableReference.SetSelfMember(VarFName);
    Ctx.RuleGraph->AddNode(Node, /*bSelectNewNode=*/false, /*bFromUI=*/true);
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    AddAuthoredRuleNode(Ctx, Node, FString::Printf(TEXT("var:%s"), *VarName));

    UEdGraphPin* OutputPin = FindVariableGetOutputPin(Node, VarFName);
    if (!OutputPin)
    {
        Ctx.Error = FString::Printf(TEXT("variable get output pin lookup failed for '%s': node=%s"),
            *VarName, *DescribePinsForError(Node));
        return Out;
    }

    Out.Kind = RuleKindFromPinType(OutputPin->PinType);
    Out.Pin = OutputPin;
    Out.bLiteral = false;
    Out.Debug = FString::Printf(TEXT("var:%s"), *VarName);
    if (Out.Kind == ETransitionRuleValueKind::Unknown)
    {
        Ctx.Error = FString::Printf(TEXT("unsupported variable pin type for '%s': category=%s subcategory=%s"),
            *VarName,
            *OutputPin->PinType.PinCategory.ToString(),
            *OutputPin->PinType.PinSubCategory.ToString());
    }
    return Out;
}

FString DefaultStringFromNumber(double Number, bool bIntegral)
{
    if (bIntegral)
    {
        return FString::Printf(TEXT("%lld"), static_cast<long long>(FMath::RoundToDouble(Number)));
    }
    return FString::SanitizeFloat(Number);
}

FTransitionRuleValue MakeBoolLiteralRuleValue(bool bValue)
{
    FTransitionRuleValue Out;
    Out.Kind = ETransitionRuleValueKind::Bool;
    Out.DefaultValue = bValue ? TEXT("true") : TEXT("false");
    Out.bLiteral = true;
    Out.Debug = Out.DefaultValue;
    return Out;
}

FTransitionRuleValue MakeNumberLiteralRuleValue(double Number)
{
    FTransitionRuleValue Out;
    const double Rounded = FMath::RoundToDouble(Number);
    Out.bIntegralNumber = FMath::IsNearlyEqual(Number, Rounded);
    Out.Kind = Out.bIntegralNumber ? ETransitionRuleValueKind::Int : ETransitionRuleValueKind::Real;
    Out.DefaultValue = DefaultStringFromNumber(Number, Out.bIntegralNumber);
    Out.bLiteral = true;
    Out.Debug = Out.DefaultValue;
    return Out;
}

FTransitionRuleValue MakeStringLiteralRuleValue(const FString& Value, ETransitionRuleValueKind Kind = ETransitionRuleValueKind::String)
{
    FTransitionRuleValue Out;
    Out.Kind = Kind;
    Out.DefaultValue = Value;
    Out.bLiteral = true;
    Out.Debug = FString::Printf(TEXT("%s:%s"), *RuleValueKindName(Kind), *Value);
    return Out;
}

bool IsNumericToken(const FString& Token, double& OutNumber)
{
    if (Token.IsEmpty()) return false;
    bool bHasDigit = false;
    bool bHasDot = false;
    for (int32 Index = 0; Index < Token.Len(); ++Index)
    {
        const TCHAR Ch = Token[Index];
        if ((Ch == TEXT('-') || Ch == TEXT('+')) && Index == 0)
        {
            continue;
        }
        if (Ch == TEXT('.'))
        {
            if (bHasDot) return false;
            bHasDot = true;
            continue;
        }
        if (!FChar::IsDigit(Ch))
        {
            return false;
        }
        bHasDigit = true;
    }
    if (!bHasDigit) return false;
    OutNumber = FCString::Atod(*Token);
    return true;
}

bool IsQuotedToken(const FString& Token)
{
    return Token.Len() >= 2
        && ((Token[0] == TEXT('"') && Token[Token.Len() - 1] == TEXT('"'))
            || (Token[0] == TEXT('\'') && Token[Token.Len() - 1] == TEXT('\'')));
}

FString UnquoteToken(FString Token)
{
    Token.TrimStartAndEndInline();
    if (IsQuotedToken(Token))
    {
        Token = Token.Mid(1, Token.Len() - 2);
        Token.ReplaceInline(TEXT("\\\""), TEXT("\""));
        Token.ReplaceInline(TEXT("\\'"), TEXT("'"));
    }
    return Token;
}

FTransitionRuleValue BuildLiteralJsonValue(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid())
    {
        return FTransitionRuleValue();
    }
    switch (Value->Type)
    {
    case EJson::Boolean:
        return MakeBoolLiteralRuleValue(Value->AsBool());
    case EJson::Number:
        return MakeNumberLiteralRuleValue(Value->AsNumber());
    case EJson::String:
        return MakeStringLiteralRuleValue(Value->AsString());
    default:
        return FTransitionRuleValue();
    }
}

bool CoerceLiteralForPin(const FTransitionRuleValue& InValue, const UEdGraphPin* TargetPin, FTransitionRuleValue& OutValue, FString& OutError)
{
    OutValue = InValue;
    if (!InValue.bLiteral || !TargetPin)
    {
        return true;
    }

    const ETransitionRuleValueKind TargetKind = RuleKindFromPinType(TargetPin->PinType);
    if (TargetKind == ETransitionRuleValueKind::Unknown)
    {
        return true;
    }

    if (TargetKind == InValue.Kind)
    {
        return true;
    }

    if (TargetKind == ETransitionRuleValueKind::Real && IsNumericRuleKind(InValue.Kind))
    {
        OutValue.Kind = ETransitionRuleValueKind::Real;
        return true;
    }

    if ((TargetKind == ETransitionRuleValueKind::Int
            || TargetKind == ETransitionRuleValueKind::Int64
            || TargetKind == ETransitionRuleValueKind::Byte)
        && IsNumericRuleKind(InValue.Kind))
    {
        if (!InValue.bIntegralNumber && InValue.Kind == ETransitionRuleValueKind::Real)
        {
            OutError = FString::Printf(TEXT("cannot set non-integral literal '%s' on %s pin '%s'"),
                *InValue.DefaultValue,
                *RuleValueKindName(TargetKind),
                *TargetPin->PinName.ToString());
            return false;
        }
        OutValue.Kind = TargetKind;
        return true;
    }

    if (TargetKind == ETransitionRuleValueKind::Name && InValue.Kind == ETransitionRuleValueKind::String)
    {
        OutValue.Kind = ETransitionRuleValueKind::Name;
        return true;
    }

    OutError = FString::Printf(TEXT("literal type mismatch for pin '%s': value=%s target=%s"),
        *TargetPin->PinName.ToString(),
        *RuleValueKindName(InValue.Kind),
        *RuleValueKindName(TargetKind));
    return false;
}

bool ConnectOrSetTransitionRuleInput(
    FTransitionRuleBuildContext& Ctx,
    const FTransitionRuleValue& Value,
    UEdGraphPin* InputPin)
{
    if (!InputPin)
    {
        Ctx.Error = TEXT("target input pin missing");
        return false;
    }
    InputPin->Modify();
    InputPin->BreakAllPinLinks();

    if (Value.Pin)
    {
        if (!Ctx.Schema || !Ctx.Schema->TryCreateConnection(Value.Pin, InputPin))
        {
            Ctx.Error = FString::Printf(TEXT("failed to connect %s to pin '%s': source=%s"),
                *Value.Debug,
                *InputPin->PinName.ToString(),
                *DescribePinsForError(Value.Pin ? Value.Pin->GetOwningNode() : nullptr));
            return false;
        }
        return true;
    }

    if (!Value.bLiteral)
    {
        Ctx.Error = FString::Printf(TEXT("expression '%s' produced neither pin nor literal"), *Value.Debug);
        return false;
    }
    if (!Ctx.K2Schema)
    {
        Ctx.Error = TEXT("transition rule graph schema is not UEdGraphSchema_K2; cannot set literal default");
        return false;
    }

    FTransitionRuleValue Coerced;
    FString CoerceError;
    if (!CoerceLiteralForPin(Value, InputPin, Coerced, CoerceError))
    {
        Ctx.Error = CoerceError;
        return false;
    }
    Ctx.K2Schema->TrySetDefaultValue(*InputPin, Coerced.DefaultValue, false);
    return true;
}

bool EnsureBoolRuleValue(FTransitionRuleBuildContext& Ctx, const FTransitionRuleValue& Value, const FString& Context)
{
    if (Value.Kind == ETransitionRuleValueKind::Bool)
    {
        return true;
    }
    Ctx.Error = FString::Printf(TEXT("%s must produce bool, got %s (%s)"),
        *Context, *RuleValueKindName(Value.Kind), *Value.Debug);
    return false;
}

FTransitionRuleValue BuildBoolLiteralProducer(FTransitionRuleBuildContext& Ctx, bool bValue)
{
    FTransitionRuleValue Literal = MakeBoolLiteralRuleValue(bValue);
    UK2Node_CallFunction* Node = CreateTransitionRuleFunctionNode(
        Ctx,
        UKismetMathLibrary::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_BoolBool),
        TEXT("literal:bool"));
    FTransitionRuleValue Out;
    if (!Node) return Out;

    UEdGraphPin* APin = Node->FindPin(TEXT("A"), EGPD_Input);
    UEdGraphPin* BPin = Node->FindPin(TEXT("B"), EGPD_Input);
    UEdGraphPin* ReturnPin = FindBoolReturnPin(Node);
    if (!APin || !BPin || !ReturnPin)
    {
        Ctx.Error = FString::Printf(TEXT("literal bool node pin lookup failed: node=%s"),
            *DescribePinsForError(Node));
        return Out;
    }

    ConnectOrSetTransitionRuleInput(Ctx, Literal, APin);
    ConnectOrSetTransitionRuleInput(Ctx, MakeBoolLiteralRuleValue(true), BPin);
    if (!Ctx.Error.IsEmpty()) return Out;

    Out.Kind = ETransitionRuleValueKind::Bool;
    Out.Pin = ReturnPin;
    Out.Debug = Literal.Debug;
    return Out;
}

FName BoolFunctionNameForOp(const FString& Op)
{
    if (Op == TEXT("and") || Op == TEXT("&&")) return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanAND);
    if (Op == TEXT("or")  || Op == TEXT("||")) return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanOR);
    return NAME_None;
}

FTransitionRuleValue BuildBoolChain(FTransitionRuleBuildContext& Ctx, const FString& Op, const TArray<FTransitionRuleValue>& Values)
{
    FTransitionRuleValue Out;
    if (Values.Num() == 0)
    {
        Ctx.Error = FString::Printf(TEXT("'%s' expression requires at least one operand"), *Op);
        return Out;
    }
    if (!EnsureBoolRuleValue(Ctx, Values[0], Op)) return Out;
    Out = Values[0];
    if (Values.Num() == 1)
    {
        return Out;
    }

    const FName FunctionName = BoolFunctionNameForOp(Op);
    if (FunctionName.IsNone())
    {
        Ctx.Error = FString::Printf(TEXT("unsupported bool op: %s"), *Op);
        return FTransitionRuleValue();
    }

    for (int32 Index = 1; Index < Values.Num(); ++Index)
    {
        if (!EnsureBoolRuleValue(Ctx, Values[Index], Op)) return FTransitionRuleValue();
        UK2Node_CallFunction* Node = CreateTransitionRuleFunctionNode(
            Ctx,
            UKismetMathLibrary::StaticClass(),
            FunctionName,
            FString::Printf(TEXT("bool:%s"), *Op));
        if (!Node) return FTransitionRuleValue();

        UEdGraphPin* APin = Node->FindPin(TEXT("A"), EGPD_Input);
        UEdGraphPin* BPin = Node->FindPin(TEXT("B"), EGPD_Input);
        UEdGraphPin* ReturnPin = FindBoolReturnPin(Node);
        if (!APin || !BPin || !ReturnPin)
        {
            Ctx.Error = FString::Printf(TEXT("bool op node pin lookup failed: node=%s"), *DescribePinsForError(Node));
            return FTransitionRuleValue();
        }
        if (!ConnectOrSetTransitionRuleInput(Ctx, Out, APin)) return FTransitionRuleValue();
        if (!ConnectOrSetTransitionRuleInput(Ctx, Values[Index], BPin)) return FTransitionRuleValue();

        Out = FTransitionRuleValue();
        Out.Kind = ETransitionRuleValueKind::Bool;
        Out.Pin = ReturnPin;
        Out.Debug = FString::Printf(TEXT("(%s %s ...)"), *Out.Debug, *Op);
    }
    return Out;
}

FTransitionRuleValue BuildBoolNot(FTransitionRuleBuildContext& Ctx, const FTransitionRuleValue& Value)
{
    FTransitionRuleValue Out;
    if (!EnsureBoolRuleValue(Ctx, Value, TEXT("not"))) return Out;
    UK2Node_CallFunction* Node = CreateTransitionRuleFunctionNode(
        Ctx,
        UKismetMathLibrary::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool),
        TEXT("bool:not"));
    if (!Node) return Out;

    UEdGraphPin* APin = Node->FindPin(TEXT("A"), EGPD_Input);
    UEdGraphPin* ReturnPin = FindBoolReturnPin(Node);
    if (!APin || !ReturnPin)
    {
        Ctx.Error = FString::Printf(TEXT("not node pin lookup failed: node=%s"), *DescribePinsForError(Node));
        return Out;
    }
    if (!ConnectOrSetTransitionRuleInput(Ctx, Value, APin)) return FTransitionRuleValue();

    Out.Kind = ETransitionRuleValueKind::Bool;
    Out.Pin = ReturnPin;
    Out.Debug = FString::Printf(TEXT("not(%s)"), *Value.Debug);
    return Out;
}

FString NormalizeCompareOp(FString Op)
{
    Op.TrimStartAndEndInline();
    Op.ToLowerInline();
    if (Op == TEXT("=")) return TEXT("==");
    if (Op == TEXT("eq")) return TEXT("==");
    if (Op == TEXT("ne")) return TEXT("!=");
    if (Op == TEXT("gt")) return TEXT(">");
    if (Op == TEXT("gte")) return TEXT(">=");
    if (Op == TEXT("ge")) return TEXT(">=");
    if (Op == TEXT("lt")) return TEXT("<");
    if (Op == TEXT("lte")) return TEXT("<=");
    if (Op == TEXT("le")) return TEXT("<=");
    return Op;
}

FName CompareFunctionName(ETransitionRuleValueKind Kind, const FString& Op, UClass*& OutOwner)
{
    OutOwner = UKismetMathLibrary::StaticClass();
    if (Kind == ETransitionRuleValueKind::Bool)
    {
        if (Op == TEXT("==")) return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_BoolBool);
        if (Op == TEXT("!=")) return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_BoolBool);
        return NAME_None;
    }
    if (Kind == ETransitionRuleValueKind::String)
    {
        OutOwner = UKismetStringLibrary::StaticClass();
        if (Op == TEXT("==")) return GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, EqualEqual_StrStr);
        if (Op == TEXT("!=")) return GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, NotEqual_StrStr);
        return NAME_None;
    }
    if (Kind == ETransitionRuleValueKind::Name)
    {
        if (Op == TEXT("==")) return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_NameName);
        if (Op == TEXT("!=")) return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_NameName);
        return NAME_None;
    }

    auto NumericName = [&Op](const TCHAR* Prefix, const TCHAR* Suffix) -> FName
    {
        if (Op == TEXT("<"))  return FName(FString::Printf(TEXT("Less_%s%s"), Prefix, Suffix));
        if (Op == TEXT(">"))  return FName(FString::Printf(TEXT("Greater_%s%s"), Prefix, Suffix));
        if (Op == TEXT("<=")) return FName(FString::Printf(TEXT("LessEqual_%s%s"), Prefix, Suffix));
        if (Op == TEXT(">=")) return FName(FString::Printf(TEXT("GreaterEqual_%s%s"), Prefix, Suffix));
        if (Op == TEXT("==")) return FName(FString::Printf(TEXT("EqualEqual_%s%s"), Prefix, Suffix));
        if (Op == TEXT("!=")) return FName(FString::Printf(TEXT("NotEqual_%s%s"), Prefix, Suffix));
        return NAME_None;
    };

    if (Kind == ETransitionRuleValueKind::Real)  return NumericName(TEXT("Double"), TEXT("Double"));
    if (Kind == ETransitionRuleValueKind::Int64) return NumericName(TEXT("Int64"), TEXT("Int64"));
    if (Kind == ETransitionRuleValueKind::Byte)  return NumericName(TEXT("Byte"), TEXT("Byte"));
    return NumericName(TEXT("Int"), TEXT("Int"));
}

ETransitionRuleValueKind ComparisonKindForValues(
    FTransitionRuleBuildContext& Ctx,
    FTransitionRuleValue& Left,
    FTransitionRuleValue& Right,
    const FString& Op)
{
    const FString NormalizedOp = NormalizeCompareOp(Op);
    const bool bEqualityOnly = NormalizedOp == TEXT("==") || NormalizedOp == TEXT("!=");

    if (Left.Kind == ETransitionRuleValueKind::String && Right.Kind == ETransitionRuleValueKind::Name && Right.bLiteral)
    {
        Right.Kind = ETransitionRuleValueKind::String;
    }
    if (Left.Kind == ETransitionRuleValueKind::Name && Right.Kind == ETransitionRuleValueKind::String && Right.bLiteral)
    {
        Right.Kind = ETransitionRuleValueKind::Name;
    }

    if (Left.Kind == ETransitionRuleValueKind::Bool || Right.Kind == ETransitionRuleValueKind::Bool)
    {
        if (Left.Kind != ETransitionRuleValueKind::Bool || Right.Kind != ETransitionRuleValueKind::Bool || !bEqualityOnly)
        {
            Ctx.Error = FString::Printf(TEXT("bool comparison supports only bool ==/!= bool; got %s %s %s"),
                *RuleValueKindName(Left.Kind), *Op, *RuleValueKindName(Right.Kind));
            return ETransitionRuleValueKind::Unknown;
        }
        return ETransitionRuleValueKind::Bool;
    }

    if (Left.Kind == ETransitionRuleValueKind::String || Right.Kind == ETransitionRuleValueKind::String)
    {
        if (Left.Kind != ETransitionRuleValueKind::String || Right.Kind != ETransitionRuleValueKind::String || !bEqualityOnly)
        {
            Ctx.Error = FString::Printf(TEXT("string comparison supports only string ==/!= string; got %s %s %s"),
                *RuleValueKindName(Left.Kind), *Op, *RuleValueKindName(Right.Kind));
            return ETransitionRuleValueKind::Unknown;
        }
        return ETransitionRuleValueKind::String;
    }

    if (Left.Kind == ETransitionRuleValueKind::Name || Right.Kind == ETransitionRuleValueKind::Name)
    {
        if (Left.Kind != ETransitionRuleValueKind::Name || Right.Kind != ETransitionRuleValueKind::Name || !bEqualityOnly)
        {
            Ctx.Error = FString::Printf(TEXT("name comparison supports only name ==/!= name; got %s %s %s"),
                *RuleValueKindName(Left.Kind), *Op, *RuleValueKindName(Right.Kind));
            return ETransitionRuleValueKind::Unknown;
        }
        return ETransitionRuleValueKind::Name;
    }

    if (!IsNumericRuleKind(Left.Kind) || !IsNumericRuleKind(Right.Kind))
    {
        Ctx.Error = FString::Printf(TEXT("comparison operands must be compatible bool/string/name/numeric values; got %s %s %s"),
            *RuleValueKindName(Left.Kind), *Op, *RuleValueKindName(Right.Kind));
        return ETransitionRuleValueKind::Unknown;
    }

    if (Left.Kind == ETransitionRuleValueKind::Real || Right.Kind == ETransitionRuleValueKind::Real)
    {
        return ETransitionRuleValueKind::Real;
    }
    if (Left.Kind == ETransitionRuleValueKind::Int64 || Right.Kind == ETransitionRuleValueKind::Int64)
    {
        return ETransitionRuleValueKind::Int64;
    }
    if (Left.Kind == ETransitionRuleValueKind::Byte && Right.Kind == ETransitionRuleValueKind::Byte)
    {
        return ETransitionRuleValueKind::Byte;
    }
    return ETransitionRuleValueKind::Int;
}

FTransitionRuleValue BuildCompareRuleValue(
    FTransitionRuleBuildContext& Ctx,
    FTransitionRuleValue Left,
    FTransitionRuleValue Right,
    FString Op)
{
    FTransitionRuleValue Out;
    Op = NormalizeCompareOp(Op);
    const ETransitionRuleValueKind CompareKind = ComparisonKindForValues(Ctx, Left, Right, Op);
    if (!Ctx.Error.IsEmpty() || CompareKind == ETransitionRuleValueKind::Unknown)
    {
        return Out;
    }

    UClass* FunctionOwner = nullptr;
    const FName FunctionName = CompareFunctionName(CompareKind, Op, FunctionOwner);
    if (FunctionName.IsNone())
    {
        Ctx.Error = FString::Printf(TEXT("unsupported comparison op '%s' for %s"),
            *Op, *RuleValueKindName(CompareKind));
        return Out;
    }

    UK2Node_CallFunction* Node = CreateTransitionRuleFunctionNode(
        Ctx,
        FunctionOwner,
        FunctionName,
        FString::Printf(TEXT("compare:%s"), *Op));
    if (!Node) return Out;

    UEdGraphPin* APin = Node->FindPin(TEXT("A"), EGPD_Input);
    UEdGraphPin* BPin = Node->FindPin(TEXT("B"), EGPD_Input);
    UEdGraphPin* ReturnPin = FindBoolReturnPin(Node);
    if (!APin || !BPin || !ReturnPin)
    {
        Ctx.Error = FString::Printf(TEXT("compare node pin lookup failed: node=%s"), *DescribePinsForError(Node));
        return Out;
    }
    if (!ConnectOrSetTransitionRuleInput(Ctx, Left, APin)) return FTransitionRuleValue();
    if (!ConnectOrSetTransitionRuleInput(Ctx, Right, BPin)) return FTransitionRuleValue();

    Out.Kind = ETransitionRuleValueKind::Bool;
    Out.Pin = ReturnPin;
    Out.Debug = FString::Printf(TEXT("compare(%s %s %s)"), *Left.Debug, *Op, *Right.Debug);
    return Out;
}

bool IsOuterParenthesized(const FString& Expression)
{
    if (Expression.Len() < 2 || Expression[0] != TEXT('(') || Expression[Expression.Len() - 1] != TEXT(')'))
    {
        return false;
    }
    int32 Depth = 0;
    bool bInSingleQuote = false;
    bool bInDoubleQuote = false;
    for (int32 Index = 0; Index < Expression.Len(); ++Index)
    {
        const TCHAR Ch = Expression[Index];
        if (Ch == TEXT('"') && !bInSingleQuote) bInDoubleQuote = !bInDoubleQuote;
        else if (Ch == TEXT('\'') && !bInDoubleQuote) bInSingleQuote = !bInSingleQuote;
        if (bInSingleQuote || bInDoubleQuote) continue;

        if (Ch == TEXT('(')) ++Depth;
        else if (Ch == TEXT(')'))
        {
            --Depth;
            if (Depth == 0 && Index != Expression.Len() - 1)
            {
                return false;
            }
        }
    }
    return Depth == 0;
}

FString StripOuterParens(FString Expression)
{
    Expression.TrimStartAndEndInline();
    while (IsOuterParenthesized(Expression))
    {
        Expression = Expression.Mid(1, Expression.Len() - 2);
        Expression.TrimStartAndEndInline();
    }
    return Expression;
}

TArray<FString> SplitTopLevelByOperator(const FString& Expression, const FString& Operator)
{
    TArray<FString> Parts;
    int32 Depth = 0;
    bool bInSingleQuote = false;
    bool bInDoubleQuote = false;
    int32 Start = 0;

    for (int32 Index = 0; Index < Expression.Len(); ++Index)
    {
        const TCHAR Ch = Expression[Index];
        if (Ch == TEXT('"') && !bInSingleQuote) bInDoubleQuote = !bInDoubleQuote;
        else if (Ch == TEXT('\'') && !bInDoubleQuote) bInSingleQuote = !bInSingleQuote;
        if (bInSingleQuote || bInDoubleQuote) continue;

        if (Ch == TEXT('(')) ++Depth;
        else if (Ch == TEXT(')')) --Depth;

        if (Depth == 0 && Expression.Mid(Index, Operator.Len()) == Operator)
        {
            Parts.Add(Expression.Mid(Start, Index - Start).TrimStartAndEnd());
            Index += Operator.Len() - 1;
            Start = Index + 1;
        }
    }
    if (Parts.Num() > 0)
    {
        Parts.Add(Expression.Mid(Start).TrimStartAndEnd());
    }
    return Parts;
}

bool FindTopLevelCompare(const FString& Expression, FString& OutLeft, FString& OutOp, FString& OutRight)
{
    static const TCHAR* Operators[] = { TEXT(">="), TEXT("<="), TEXT("=="), TEXT("!="), TEXT(">"), TEXT("<") };
    int32 Depth = 0;
    bool bInSingleQuote = false;
    bool bInDoubleQuote = false;

    for (int32 Index = 0; Index < Expression.Len(); ++Index)
    {
        const TCHAR Ch = Expression[Index];
        if (Ch == TEXT('"') && !bInSingleQuote) bInDoubleQuote = !bInDoubleQuote;
        else if (Ch == TEXT('\'') && !bInDoubleQuote) bInSingleQuote = !bInSingleQuote;
        if (bInSingleQuote || bInDoubleQuote) continue;

        if (Ch == TEXT('(')) ++Depth;
        else if (Ch == TEXT(')')) --Depth;
        if (Depth != 0) continue;

        for (const TCHAR* Operator : Operators)
        {
            const FString Op(Operator);
            if (Expression.Mid(Index, Op.Len()) == Op)
            {
                OutLeft = Expression.Mid(0, Index).TrimStartAndEnd();
                OutOp = Op;
                OutRight = Expression.Mid(Index + Op.Len()).TrimStartAndEnd();
                return !OutLeft.IsEmpty() && !OutRight.IsEmpty();
            }
        }
    }
    return false;
}

FTransitionRuleValue BuildStringAtom(FTransitionRuleBuildContext& Ctx, FString Token, bool bMissingVariableAsStringLiteral)
{
    Token.TrimStartAndEndInline();
    bool bBool = false;
    if (ParseLiteralBoolExpression(Token, bBool))
    {
        return MakeBoolLiteralRuleValue(bBool);
    }
    double Number = 0.0;
    if (IsNumericToken(Token, Number))
    {
        return MakeNumberLiteralRuleValue(Number);
    }
    if (IsQuotedToken(Token))
    {
        return MakeStringLiteralRuleValue(UnquoteToken(Token));
    }
    if (HasTransitionRuleVariable(Ctx.AnimBP, FName(*Token)))
    {
        return BuildTransitionRuleVariable(Ctx, Token);
    }
    if (bMissingVariableAsStringLiteral)
    {
        return MakeStringLiteralRuleValue(Token);
    }

    Ctx.Error = FString::Printf(TEXT("unsupported bare transition rule atom '%s'; expected bool literal, number, quoted string, or AnimBP variable"), *Token);
    return FTransitionRuleValue();
}

FTransitionRuleValue BuildTransitionRuleStringExpression(FTransitionRuleBuildContext& Ctx, FString Expression)
{
    Expression = StripOuterParens(Expression);
    if (Expression.IsEmpty())
    {
        Ctx.Error = TEXT("transition rule expression string is empty");
        return FTransitionRuleValue();
    }

    TArray<FString> OrParts = SplitTopLevelByOperator(Expression, TEXT("||"));
    if (OrParts.Num() > 0)
    {
        TArray<FTransitionRuleValue> Values;
        for (const FString& Part : OrParts)
        {
            Values.Add(BuildTransitionRuleStringExpression(Ctx, Part));
            if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();
        }
        return BuildBoolChain(Ctx, TEXT("or"), Values);
    }

    TArray<FString> AndParts = SplitTopLevelByOperator(Expression, TEXT("&&"));
    if (AndParts.Num() > 0)
    {
        TArray<FTransitionRuleValue> Values;
        for (const FString& Part : AndParts)
        {
            Values.Add(BuildTransitionRuleStringExpression(Ctx, Part));
            if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();
        }
        return BuildBoolChain(Ctx, TEXT("and"), Values);
    }

    if (Expression.StartsWith(TEXT("!")))
    {
        return BuildBoolNot(Ctx, BuildTransitionRuleStringExpression(Ctx, Expression.Mid(1)));
    }

    FString LeftText, Op, RightText;
    if (FindTopLevelCompare(Expression, LeftText, Op, RightText))
    {
        FTransitionRuleValue Left = BuildStringAtom(Ctx, LeftText, /*bMissingVariableAsStringLiteral=*/false);
        if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();
        FTransitionRuleValue Right = BuildStringAtom(Ctx, RightText, /*bMissingVariableAsStringLiteral=*/true);
        if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();
        return BuildCompareRuleValue(Ctx, Left, Right, Op);
    }

    return BuildStringAtom(Ctx, Expression, /*bMissingVariableAsStringLiteral=*/false);
}

FTransitionRuleValue BuildJsonObjectRuleExpression(FTransitionRuleBuildContext& Ctx, const TSharedPtr<FJsonObject>& Obj)
{
    if (!Obj.IsValid())
    {
        Ctx.Error = TEXT("transition rule expression object is null");
        return FTransitionRuleValue();
    }

    const TSharedPtr<FJsonValue>* LiteralPtr = Obj->Values.Find(TEXT("literal"));
    if (LiteralPtr)
    {
        FTransitionRuleValue Literal = BuildLiteralJsonValue(*LiteralPtr);
        if (Literal.Kind == ETransitionRuleValueKind::Unknown)
        {
            Ctx.Error = TEXT("'literal' supports only boolean, number, or string values");
        }
        return Literal;
    }

    bool bBoolValue = false;
    if (Obj->TryGetBoolField(TEXT("bool"), bBoolValue))
    {
        return MakeBoolLiteralRuleValue(bBoolValue);
    }

    double Number = 0.0;
    if (Obj->TryGetNumberField(TEXT("number"), Number) || Obj->TryGetNumberField(TEXT("float"), Number))
    {
        return MakeNumberLiteralRuleValue(Number);
    }

    int32 IntValue = 0;
    if (Obj->TryGetNumberField(TEXT("int"), Number))
    {
        IntValue = static_cast<int32>(Number);
        FTransitionRuleValue Out = MakeNumberLiteralRuleValue(IntValue);
        Out.Kind = ETransitionRuleValueKind::Int;
        Out.bIntegralNumber = true;
        return Out;
    }

    FString StringValue;
    if (Obj->TryGetStringField(TEXT("string"), StringValue))
    {
        return MakeStringLiteralRuleValue(StringValue);
    }
    if (Obj->TryGetStringField(TEXT("name"), StringValue))
    {
        return MakeStringLiteralRuleValue(StringValue, ETransitionRuleValueKind::Name);
    }

    FString VarName;
    FString Op;
    const bool bHasVar = Obj->TryGetStringField(TEXT("var"), VarName);
    const bool bHasOp = Obj->TryGetStringField(TEXT("op"), Op) || Obj->TryGetStringField(TEXT("compare"), Op);
    if (bHasVar && !bHasOp && !Obj->Values.Contains(TEXT("value")) && !Obj->Values.Contains(TEXT("right")))
    {
        return BuildTransitionRuleVariable(Ctx, VarName);
    }

    const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
    if (Obj->TryGetArrayField(TEXT("and"), Parts))
    {
        TArray<FTransitionRuleValue> Values;
        for (const TSharedPtr<FJsonValue>& Part : *Parts)
        {
            Values.Add(BuildTransitionRuleExpression(Ctx, Part));
            if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();
        }
        return BuildBoolChain(Ctx, TEXT("and"), Values);
    }
    if (Obj->TryGetArrayField(TEXT("or"), Parts))
    {
        TArray<FTransitionRuleValue> Values;
        for (const TSharedPtr<FJsonValue>& Part : *Parts)
        {
            Values.Add(BuildTransitionRuleExpression(Ctx, Part));
            if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();
        }
        return BuildBoolChain(Ctx, TEXT("or"), Values);
    }

    const TSharedPtr<FJsonValue>* NotPtr = Obj->Values.Find(TEXT("not"));
    if (NotPtr)
    {
        return BuildBoolNot(Ctx, BuildTransitionRuleExpression(Ctx, *NotPtr));
    }

    if (bHasOp)
    {
        Op = NormalizeCompareOp(Op);
        if (Op == TEXT("and") || Op == TEXT("&&") || Op == TEXT("or") || Op == TEXT("||"))
        {
            const TArray<TSharedPtr<FJsonValue>>* Args = nullptr;
            if (!Obj->TryGetArrayField(TEXT("args"), Args))
            {
                Ctx.Error = FString::Printf(TEXT("bool op '%s' requires args array"), *Op);
                return FTransitionRuleValue();
            }
            TArray<FTransitionRuleValue> Values;
            for (const TSharedPtr<FJsonValue>& Arg : *Args)
            {
                Values.Add(BuildTransitionRuleExpression(Ctx, Arg));
                if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();
            }
            return BuildBoolChain(Ctx, Op == TEXT("||") ? TEXT("or") : Op, Values);
        }
        if (Op == TEXT("not") || Op == TEXT("!"))
        {
            const TSharedPtr<FJsonValue>* ValuePtr = Obj->Values.Find(TEXT("value"));
            if (!ValuePtr)
            {
                Ctx.Error = TEXT("'not' op requires value");
                return FTransitionRuleValue();
            }
            return BuildBoolNot(Ctx, BuildTransitionRuleExpression(Ctx, *ValuePtr));
        }

        FTransitionRuleValue Left;
        const TSharedPtr<FJsonValue>* LeftPtr = Obj->Values.Find(TEXT("left"));
        if (LeftPtr)
        {
            Left = BuildTransitionRuleExpression(Ctx, *LeftPtr);
        }
        else if (bHasVar)
        {
            Left = BuildTransitionRuleVariable(Ctx, VarName);
        }
        else
        {
            Ctx.Error = TEXT("comparison requires either 'left' expression or 'var'");
            return FTransitionRuleValue();
        }
        if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();

        FTransitionRuleValue Right;
        const TSharedPtr<FJsonValue>* RightPtr = Obj->Values.Find(TEXT("right"));
        const TSharedPtr<FJsonValue>* ValuePtr = Obj->Values.Find(TEXT("value"));
        if (RightPtr)
        {
            Right = BuildTransitionRuleExpression(Ctx, *RightPtr);
        }
        else if (ValuePtr)
        {
            Right = BuildLiteralJsonValue(*ValuePtr);
            if (Right.Kind == ETransitionRuleValueKind::Unknown)
            {
                Ctx.Error = TEXT("'value' supports only boolean, number, or string literals");
                return FTransitionRuleValue();
            }
        }
        else
        {
            Ctx.Error = TEXT("comparison requires either 'right' expression or literal 'value'");
            return FTransitionRuleValue();
        }
        if (!Ctx.Error.IsEmpty()) return FTransitionRuleValue();

        return BuildCompareRuleValue(Ctx, Left, Right, Op);
    }

    Ctx.Error = TEXT("unsupported transition rule expression object; expected var/literal/bool/number/string/name, and/or/or/not, or op+left/right");
    return FTransitionRuleValue();
}

FTransitionRuleValue BuildTransitionRuleExpression(FTransitionRuleBuildContext& Ctx, const TSharedPtr<FJsonValue>& Expr)
{
    if (!Expr.IsValid())
    {
        Ctx.Error = TEXT("missing transition rule expression");
        return FTransitionRuleValue();
    }

    if (Expr->Type == EJson::Boolean)
    {
        return MakeBoolLiteralRuleValue(Expr->AsBool());
    }
    if (Expr->Type == EJson::Number)
    {
        return MakeNumberLiteralRuleValue(Expr->AsNumber());
    }
    if (Expr->Type == EJson::String)
    {
        return BuildTransitionRuleStringExpression(Ctx, Expr->AsString());
    }
    if (Expr->Type == EJson::Object)
    {
        return BuildJsonObjectRuleExpression(Ctx, Expr->AsObject());
    }

    Ctx.Error = TEXT("transition rule expression must be string, bool, number, or object");
    return FTransitionRuleValue();
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
    else if (TypeFilter == TEXT("AnimComposite"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimComposite")));
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
    // Variables (mirror bp.list_variables shape: name + type-string).
    TArray<TSharedPtr<FJsonValue>> Vars;
    for (const FBPVariableDescription& V : BP->NewVariables)
    {
        auto VObj = MakeShared<FJsonObject>();
        VObj->SetStringField(TEXT("name"),  V.VarName.ToString());
        VObj->SetStringField(TEXT("type"),  V.VarType.PinCategory.ToString());
        VObj->SetStringField(TEXT("guid"),  V.VarGuid.ToString(EGuidFormats::Digits));
        Vars.Add(MakeShared<FJsonValueObject>(VObj));
    }
    R->SetArrayField(TEXT("variables"), Vars);

    // Function graphs categorized by schema. Anim graphs are
    // UAnimationGraphSchema-bound; everything else is plain event/function.
    TArray<TSharedPtr<FJsonValue>> AnimGraphs;
    TArray<TSharedPtr<FJsonValue>> EventGraphs;
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (!G) continue;
        auto GObj = MakeShared<FJsonObject>();
        GObj->SetStringField(TEXT("name"),       G->GetFName().ToString());
        GObj->SetStringField(TEXT("schema"),     G->Schema ? G->Schema->GetName() : TEXT(""));
        GObj->SetNumberField(TEXT("node_count"), G->Nodes.Num());
        if (G->Schema && G->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
        {
            AnimGraphs.Add(MakeShared<FJsonValueObject>(GObj));
        }
        else
        {
            EventGraphs.Add(MakeShared<FJsonValueObject>(GObj));
        }
    }
    // Interface override graphs (Cluster G ALI overrides + plain UInterface
    // overrides). Same anim/event split — anim layer interface overrides are
    // AnimationGraphSchema-bound, plain interface overrides are not. Tagged
    // with interface_function:true + interface info so callers can identify
    // and route them via the same graph_name lookup as native function graphs
    // (Lyra Sage Gap #25).
    for (FBPInterfaceDescription& Impl : BP->ImplementedInterfaces)
    {
        if (!Impl.Interface) continue;
        for (UEdGraph* G : Impl.Graphs)
        {
            if (!G) continue;
            auto GObj = MakeShared<FJsonObject>();
            GObj->SetStringField(TEXT("name"),       G->GetFName().ToString());
            GObj->SetStringField(TEXT("schema"),     G->Schema ? G->Schema->GetName() : TEXT(""));
            GObj->SetNumberField(TEXT("node_count"), G->Nodes.Num());
            GObj->SetBoolField  (TEXT("interface_function"), true);
            GObj->SetStringField(TEXT("interface"),       Impl.Interface->GetName());
            GObj->SetStringField(TEXT("interface_path"),  Impl.Interface->GetPathName());
            if (G->Schema && G->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
            {
                AnimGraphs.Add(MakeShared<FJsonValueObject>(GObj));
            }
            else
            {
                EventGraphs.Add(MakeShared<FJsonValueObject>(GObj));
            }
        }
    }
    // anim_graph_count = total anim-schema-bound graphs (FunctionGraphs anim
    // entries + interface override anim entries). Previously was
    // BP->FunctionGraphs.Num() which counted non-anim graphs too AND missed
    // every interface override.
    R->SetNumberField(TEXT("anim_graph_count"), AnimGraphs.Num());
    R->SetArrayField(TEXT("anim_graphs"),  AnimGraphs);
    R->SetArrayField(TEXT("event_graphs"), EventGraphs);
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
    TArray<TSharedPtr<FJsonValue>> Bones;
    for (int32 I = 0; I < NumBones; ++I)
    {
        const FName BoneName = RefSkel.GetBoneName(I);
        const int32 ParentIndex = RefSkel.GetParentIndex(I);
        BoneNames.Add(MakeShared<FJsonValueString>(BoneName.ToString()));

        auto B = MakeShared<FJsonObject>();
        B->SetNumberField(TEXT("index"), I);
        B->SetStringField(TEXT("name"), BoneName.ToString());
        B->SetNumberField(TEXT("parent_index"), ParentIndex);
        B->SetStringField(TEXT("parent_name"),
            ParentIndex != INDEX_NONE ? RefSkel.GetBoneName(ParentIndex).ToString() : FString());
        if (RefSkel.GetRefBonePose().IsValidIndex(I))
        {
            B->SetObjectField(TEXT("ref_pose"), TransformToJsonObject(RefSkel.GetRefBonePose()[I]));
        }
        FString RetargetMode;
        switch (Skeleton->GetBoneTranslationRetargetingMode(I))
        {
        case EBoneTranslationRetargetingMode::Animation:         RetargetMode = TEXT("Animation"); break;
        case EBoneTranslationRetargetingMode::Skeleton:          RetargetMode = TEXT("Skeleton"); break;
        case EBoneTranslationRetargetingMode::AnimationScaled:   RetargetMode = TEXT("AnimationScaled"); break;
        case EBoneTranslationRetargetingMode::AnimationRelative: RetargetMode = TEXT("AnimationRelative"); break;
        case EBoneTranslationRetargetingMode::OrientAndScale:    RetargetMode = TEXT("OrientAndScale"); break;
        default:                                                 RetargetMode = TEXT("Unknown"); break;
        }
        B->SetStringField(TEXT("translation_retargeting"), RetargetMode);
        Bones.Add(MakeShared<FJsonValueObject>(B));
    }

    TArray<TSharedPtr<FJsonValue>> VirtualBones;
    for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
    {
        auto V = MakeShared<FJsonObject>();
        V->SetStringField(TEXT("name"), VB.VirtualBoneName.ToString());
        V->SetStringField(TEXT("source_bone"), VB.SourceBoneName.ToString());
        V->SetStringField(TEXT("target_bone"), VB.TargetBoneName.ToString());
        VirtualBones.Add(MakeShared<FJsonValueObject>(V));
    }

    TArray<TSharedPtr<FJsonValue>> SlotGroups;
    for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
    {
        auto G = MakeShared<FJsonObject>();
        G->SetStringField(TEXT("group_name"), Group.GroupName.ToString());
        TArray<TSharedPtr<FJsonValue>> Slots;
        for (const FName& SlotName : Group.SlotNames)
        {
            Slots.Add(MakeShared<FJsonValueString>(SlotName.ToString()));
        }
        G->SetArrayField(TEXT("slots"), Slots);
        SlotGroups.Add(MakeShared<FJsonValueObject>(G));
    }

    TArray<TSharedPtr<FJsonValue>> Sockets;
    for (const USkeletalMeshSocket* S : Skeleton->Sockets)
    {
        if (!S) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), S->SocketName.ToString());
        J->SetStringField(TEXT("parent_bone"), S->BoneName.ToString());
        J->SetField(TEXT("location"), detail::Vec3ToJson(S->RelativeLocation));
        J->SetField(TEXT("rotation"), detail::Rot3ToJson(S->RelativeRotation));
        J->SetField(TEXT("scale"), detail::Vec3ToJson(S->RelativeScale));
        J->SetBoolField(TEXT("force_always_animated"), S->bForceAlwaysAnimated);
        J->SetStringField(TEXT("owner"), TEXT("skeleton"));
        Sockets.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Skeleton->GetPathName());
    R->SetNumberField(TEXT("num_bones"),  NumBones);
    R->SetArrayField (TEXT("bone_names"), BoneNames);
    R->SetArrayField (TEXT("bones"),      Bones);
    R->SetArrayField (TEXT("virtual_bones"), VirtualBones);
    R->SetNumberField(TEXT("virtual_bone_count"), VirtualBones.Num());
    R->SetArrayField (TEXT("slot_groups"), SlotGroups);
    R->SetNumberField(TEXT("slot_group_count"), SlotGroups.Num());
    R->SetArrayField (TEXT("sockets"), Sockets);
    R->SetNumberField(TEXT("socket_count"), Sockets.Num());
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
    FString OwnerFilter = TEXT("any");
    Args->TryGetStringField(TEXT("owner"), OwnerFilter);
    OwnerFilter = OwnerFilter.TrimStartAndEnd().ToLower();
    if (OwnerFilter.IsEmpty()) OwnerFilter = TEXT("any");
    if (OwnerFilter != TEXT("any") && OwnerFilter != TEXT("mesh") && OwnerFilter != TEXT("skeleton"))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("'owner' must be one of: mesh, skeleton, any"));
    }
    UObject* Asset = ResolveAsset(Path);
    USkeleton* Skeleton = Cast<USkeleton>(Asset);
    USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset);
    if (!Skeleton && !Mesh)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton or USkeletalMesh: %s"), *Path));
    }

    TArray<TSharedPtr<FJsonValue>> Sockets;
    TArray<TSharedPtr<FJsonValue>> Collisions;
    auto AddSocket = [&Sockets](const USkeletalMeshSocket* S, const FString& Owner, const UObject* OwnerAsset, bool bEffective)
    {
        if (!S) return;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),        S->SocketName.ToString());
        J->SetStringField(TEXT("bone_name"),   S->BoneName.ToString());
        J->SetStringField(TEXT("parent_bone"), S->BoneName.ToString());
        J->SetStringField(TEXT("owner"),       Owner);
        J->SetBoolField  (TEXT("effective"),   bEffective);
        if (OwnerAsset)
        {
            J->SetStringField(TEXT("owner_asset"), OwnerAsset->GetPathName());
        }
        J->SetField(TEXT("location"), detail::Vec3ToJson(S->RelativeLocation));
        J->SetField(TEXT("rotation"), detail::Rot3ToJson(S->RelativeRotation));
        J->SetField(TEXT("scale"), detail::Vec3ToJson(S->RelativeScale));
        J->SetBoolField(TEXT("force_always_animated"), S->bForceAlwaysAnimated);
        Sockets.Add(MakeShared<FJsonValueObject>(J));
    };
    if (Skeleton)
    {
        if (OwnerFilter == TEXT("any") || OwnerFilter == TEXT("skeleton"))
        {
            for (const USkeletalMeshSocket* S : Skeleton->Sockets)
            {
                AddSocket(S, TEXT("skeleton"), Skeleton, true);
            }
        }
    }
    if (Mesh)
    {
        USkeleton* OwningSkeleton = Mesh->GetSkeleton();
        TMap<FName, const USkeletalMeshSocket*> MeshSocketsByName;
        if (OwnerFilter == TEXT("any") || OwnerFilter == TEXT("mesh"))
        {
            for (const TObjectPtr<USkeletalMeshSocket>& S : Mesh->GetMeshOnlySocketList())
            {
                if (!S) continue;
                MeshSocketsByName.Add(S->SocketName, S.Get());
                AddSocket(S.Get(), TEXT("mesh"), Mesh, true);
            }
        }
        if (OwnerFilter == TEXT("any") || OwnerFilter == TEXT("skeleton"))
        {
            if (OwningSkeleton)
            {
                for (const USkeletalMeshSocket* S : OwningSkeleton->Sockets)
                {
                    if (!S) continue;
                    const USkeletalMeshSocket* MeshOverride = MeshSocketsByName.FindRef(S->SocketName);
                    if (!MeshOverride)
                    {
                        for (const TObjectPtr<USkeletalMeshSocket>& MeshSocket : Mesh->GetMeshOnlySocketList())
                        {
                            if (MeshSocket && MeshSocket->SocketName == S->SocketName)
                            {
                                MeshOverride = MeshSocket.Get();
                                break;
                            }
                        }
                    }
                    const bool bShadowed = MeshOverride != nullptr;
                    AddSocket(S, TEXT("skeleton"), OwningSkeleton, !bShadowed);
                    if (bShadowed)
                    {
                        auto C = MakeShared<FJsonObject>();
                        C->SetStringField(TEXT("name"), S->SocketName.ToString());
                        C->SetStringField(TEXT("effective_owner"), TEXT("mesh"));
                        C->SetStringField(TEXT("shadowed_owner"), TEXT("skeleton"));
                        C->SetStringField(TEXT("mesh_asset"), Mesh->GetPathName());
                        C->SetStringField(TEXT("skeleton_asset"), OwningSkeleton->GetPathName());
                        Collisions.Add(MakeShared<FJsonValueObject>(C));
                    }
                }
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),    Asset->GetPathName());
    R->SetStringField(TEXT("asset_type"), Skeleton ? TEXT("USkeleton") : TEXT("USkeletalMesh"));
    R->SetStringField(TEXT("owner_filter"), OwnerFilter);
    R->SetArrayField (TEXT("sockets"), Sockets);
    R->SetNumberField(TEXT("count"),   Sockets.Num());
    R->SetArrayField(TEXT("owner_collisions"), Collisions);
    R->SetNumberField(TEXT("owner_collision_count"), Collisions.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.list_skeletal_meshes
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ListSkeletalMeshesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SkeletonPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("skeleton"), SkeletonPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    USkeleton* Skeleton = Cast<USkeleton>(ResolveAssetOrPackage(SkeletonPath));
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

const FReferenceSkeleton* ResolveReferenceSkeletonLike(
    const FString& Path,
    UObject*& OutAsset,
    USkeleton*& OutSkeleton,
    USkeletalMesh*& OutMesh,
    FString& OutError)
{
    OutAsset = ResolveAsset(Path);
    OutSkeleton = nullptr;
    OutMesh = nullptr;
    if (!OutAsset)
    {
        OutError = FString::Printf(TEXT("asset not found: %s"), *Path);
        return nullptr;
    }
    if (USkeleton* Skeleton = Cast<USkeleton>(OutAsset))
    {
        OutSkeleton = Skeleton;
        return &Skeleton->GetReferenceSkeleton();
    }
    if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(OutAsset))
    {
        OutMesh = Mesh;
        OutSkeleton = Mesh->GetSkeleton();
        return &Mesh->GetRefSkeleton();
    }
    if (UAnimationAsset* Anim = Cast<UAnimationAsset>(OutAsset))
    {
        OutSkeleton = Anim->GetSkeleton();
        if (OutSkeleton)
        {
            return &OutSkeleton->GetReferenceSkeleton();
        }
        OutError = FString::Printf(TEXT("animation asset has no skeleton: %s"), *Path);
        return nullptr;
    }
    OutError = FString::Printf(TEXT("not a USkeleton, USkeletalMesh, or UAnimationAsset: %s"), *Path);
    return nullptr;
}

TArray<FTransform> BuildGlobalRefPose(const FReferenceSkeleton& RefSkel)
{
    TArray<FTransform> Global;
    const TArray<FTransform>& Local = RefSkel.GetRefBonePose();
    Global.SetNum(Local.Num());
    for (int32 Index = 0; Index < Local.Num(); ++Index)
    {
        const int32 ParentIndex = RefSkel.GetParentIndex(Index);
        Global[Index] = ParentIndex != INDEX_NONE && Global.IsValidIndex(ParentIndex)
            ? Local[Index] * Global[ParentIndex]
            : Local[Index];
    }
    return Global;
}

TSharedRef<FJsonObject> RefBoneToJson(
    const FReferenceSkeleton& RefSkel,
    int32 BoneIndex,
    const TArray<FTransform>* GlobalPose = nullptr,
    const USkeleton* Skeleton = nullptr)
{
    auto Bone = MakeShared<FJsonObject>();
    const FName BoneName = RefSkel.GetBoneName(BoneIndex);
    const int32 ParentIndex = RefSkel.GetParentIndex(BoneIndex);
    Bone->SetNumberField(TEXT("index"), BoneIndex);
    Bone->SetStringField(TEXT("name"), BoneName.ToString());
    Bone->SetNumberField(TEXT("parent_index"), ParentIndex);
    Bone->SetStringField(TEXT("parent_name"),
        ParentIndex != INDEX_NONE ? RefSkel.GetBoneName(ParentIndex).ToString() : FString());
    if (RefSkel.GetRefBonePose().IsValidIndex(BoneIndex))
    {
        Bone->SetObjectField(TEXT("local_ref_pose"), TransformToJsonObject(RefSkel.GetRefBonePose()[BoneIndex]));
    }
    if (GlobalPose && GlobalPose->IsValidIndex(BoneIndex))
    {
        Bone->SetObjectField(TEXT("global_ref_pose"), TransformToJsonObject((*GlobalPose)[BoneIndex]));
    }
    if (Skeleton)
    {
        Bone->SetStringField(TEXT("translation_retargeting"),
            RetargetModeToString(Skeleton->GetBoneTranslationRetargetingMode(BoneIndex)));
    }
    return Bone;
}

TSet<FName> ReadBoneNameFilter(const TSharedPtr<FJsonObject>& Args)
{
    TSet<FName> Filter;
    FString SingleBone;
    if (Args.IsValid() && Args->TryGetStringField(TEXT("bone"), SingleBone) && !SingleBone.IsEmpty())
    {
        Filter.Add(FName(*SingleBone));
    }
    TArray<FString> BoneStrings;
    ReadStringArrayField(Args, TEXT("bones"), BoneStrings);
    for (const FString& Bone : BoneStrings)
    {
        if (!Bone.IsEmpty())
        {
            Filter.Add(FName(*Bone));
        }
    }
    return Filter;
}

FString NormalizeToken(FString Raw);

// ---------------------------------------------------------------------------
// animation.inspect_skeleton
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome InspectSkeletonImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("path"), TEXT("skeleton"), TEXT("skeletal_mesh") }, Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UObject* Asset = nullptr;
    USkeleton* Skeleton = nullptr;
    USkeletalMesh* Mesh = nullptr;
    FString Error;
    const FReferenceSkeleton* RefSkel = ResolveReferenceSkeletonLike(Path, Asset, Skeleton, Mesh, Error);
    if (!RefSkel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    bool bIncludeRefPose = true;
    Args->TryGetBoolField(TEXT("include_ref_pose"), bIncludeRefPose);
    const TSet<FName> BoneFilter = ReadBoneNameFilter(Args);
    TArray<FTransform> GlobalPose = bIncludeRefPose ? BuildGlobalRefPose(*RefSkel) : TArray<FTransform>();

    TArray<TSharedPtr<FJsonValue>> Bones;
    TArray<TSharedPtr<FJsonValue>> BoneNames;
    for (int32 Index = 0; Index < RefSkel->GetNum(); ++Index)
    {
        const FName BoneName = RefSkel->GetBoneName(Index);
        BoneNames.Add(MakeShared<FJsonValueString>(BoneName.ToString()));
        if (BoneFilter.Num() > 0 && !BoneFilter.Contains(BoneName))
        {
            continue;
        }
        Bones.Add(MakeShared<FJsonValueObject>(RefBoneToJson(
            *RefSkel,
            Index,
            bIncludeRefPose ? &GlobalPose : nullptr,
            Skeleton)));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Asset->GetPathName());
    R->SetStringField(TEXT("asset_type"), Asset->GetClass()->GetName());
    R->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());
    R->SetStringField(TEXT("skeletal_mesh"), Mesh ? Mesh->GetPathName() : FString());
    R->SetNumberField(TEXT("num_bones"), RefSkel->GetNum());
    R->SetArrayField(TEXT("bone_names"), BoneNames);
    R->SetArrayField(TEXT("bones"), Bones);
    R->SetNumberField(TEXT("returned_bone_count"), Bones.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.inspect_ref_pose
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome InspectRefPoseImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("path"), TEXT("skeleton"), TEXT("skeletal_mesh") }, Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UObject* Asset = nullptr;
    USkeleton* Skeleton = nullptr;
    USkeletalMesh* Mesh = nullptr;
    FString Error;
    const FReferenceSkeleton* RefSkel = ResolveReferenceSkeletonLike(Path, Asset, Skeleton, Mesh, Error);
    if (!RefSkel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    const TSet<FName> BoneFilter = ReadBoneNameFilter(Args);
    TArray<FTransform> GlobalPose = BuildGlobalRefPose(*RefSkel);
    TArray<TSharedPtr<FJsonValue>> Bones;
    TArray<TSharedPtr<FJsonValue>> Missing;
    for (const FName& Requested : BoneFilter)
    {
        if (RefSkel->FindBoneIndex(Requested) == INDEX_NONE)
        {
            Missing.Add(MakeShared<FJsonValueString>(Requested.ToString()));
        }
    }
    for (int32 Index = 0; Index < RefSkel->GetNum(); ++Index)
    {
        const FName BoneName = RefSkel->GetBoneName(Index);
        if (BoneFilter.Num() > 0 && !BoneFilter.Contains(BoneName))
        {
            continue;
        }
        Bones.Add(MakeShared<FJsonValueObject>(RefBoneToJson(*RefSkel, Index, &GlobalPose, Skeleton)));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Asset->GetPathName());
    R->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());
    R->SetStringField(TEXT("skeletal_mesh"), Mesh ? Mesh->GetPathName() : FString());
    R->SetArrayField(TEXT("bones"), Bones);
    R->SetNumberField(TEXT("count"), Bones.Num());
    R->SetArrayField(TEXT("missing_bones"), Missing);
    R->SetNumberField(TEXT("missing_count"), Missing.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.list_skeletons
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ListSkeletonsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path = TEXT("/Game");
    FString Query;
    int32 MaxResults = 200;
    bool bIncludeDetails = false;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), Path);
        Args->TryGetStringField(TEXT("query"), Query);
        double MaxD = MaxResults;
        if (Args->TryGetNumberField(TEXT("max_results"), MaxD))
        {
            MaxResults = FMath::Max(1, static_cast<int32>(MaxD));
        }
        Args->TryGetBoolField(TEXT("include_details"), bIncludeDetails);
    }

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Path));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.Skeleton")));
    TArray<FAssetData> Found;
    GetAssetRegistry().GetAssets(Filter, Found);

    TArray<TSharedPtr<FJsonValue>> Items;
    int32 MatchedTotal = 0;
    for (const FAssetData& AssetData : Found)
    {
        if (!Query.IsEmpty() &&
            !AssetData.AssetName.ToString().Contains(Query) &&
            !AssetData.GetObjectPathString().Contains(Query))
        {
            continue;
        }
        ++MatchedTotal;
        if (Items.Num() >= MaxResults)
        {
            continue;
        }
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
        Obj->SetStringField(TEXT("path"), AssetData.GetSoftObjectPath().ToString());
        if (bIncludeDetails)
        {
            if (USkeleton* Skeleton = Cast<USkeleton>(AssetData.GetAsset()))
            {
                Obj->SetNumberField(TEXT("num_bones"), Skeleton->GetReferenceSkeleton().GetNum());
            }
        }
        Items.Add(MakeShared<FJsonValueObject>(Obj));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("query"), Query);
    R->SetArrayField(TEXT("skeletons"), Items);
    R->SetNumberField(TEXT("count"), Items.Num());
    R->SetNumberField(TEXT("matched_total"), MatchedTotal);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

USkeleton* ResolveSkeletonForProfile(const FString& Path, UObject*& OutAsset, FString& OutError)
{
    OutAsset = ResolveAsset(Path);
    if (!OutAsset)
    {
        OutError = FString::Printf(TEXT("asset not found: %s"), *Path);
        return nullptr;
    }
    if (USkeleton* Skeleton = Cast<USkeleton>(OutAsset))
    {
        return Skeleton;
    }
    if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(OutAsset))
    {
        if (USkeleton* Skeleton = Mesh->GetSkeleton())
        {
            return Skeleton;
        }
        OutError = FString::Printf(TEXT("skeletal mesh has no skeleton: %s"), *Path);
        return nullptr;
    }
    if (UAnimationAsset* Anim = Cast<UAnimationAsset>(OutAsset))
    {
        if (USkeleton* Skeleton = Anim->GetSkeleton())
        {
            return Skeleton;
        }
        OutError = FString::Printf(TEXT("animation asset has no skeleton: %s"), *Path);
        return nullptr;
    }
    OutError = FString::Printf(TEXT("not a USkeleton, USkeletalMesh, or UAnimationAsset: %s"), *Path);
    return nullptr;
}

TSharedRef<FJsonObject> BlendProfileToJson(UBlendProfile* Profile)
{
    auto Obj = MakeShared<FJsonObject>();
    if (!Profile)
    {
        Obj->SetBoolField(TEXT("valid"), false);
        return Obj;
    }
    USkeleton* Skeleton = Profile->GetSkeleton();
    Obj->SetBoolField(TEXT("valid"), true);
    Obj->SetStringField(TEXT("name"), Profile->GetName());
    Obj->SetStringField(TEXT("path"), Profile->GetPathName());
    Obj->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());
    Obj->SetStringField(TEXT("mode"), Profile->IsBlendMask() ? TEXT("BlendMask") : TEXT("BlendProfile"));
    Obj->SetBoolField(TEXT("is_blend_mask"), Profile->IsBlendMask());
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FBlendProfileBoneEntry& Entry : Profile->ProfileEntries)
    {
        auto EntryObj = MakeShared<FJsonObject>();
        EntryObj->SetStringField(TEXT("bone"), Entry.BoneReference.BoneName.ToString());
        EntryObj->SetNumberField(TEXT("scale"), Entry.BlendScale);
        if (Skeleton)
        {
            EntryObj->SetNumberField(TEXT("bone_index"),
                Skeleton->GetReferenceSkeleton().FindBoneIndex(Entry.BoneReference.BoneName));
        }
        Entries.Add(MakeShared<FJsonValueObject>(EntryObj));
    }
    Obj->SetArrayField(TEXT("entries"), Entries);
    Obj->SetNumberField(TEXT("entry_count"), Entries.Num());
    return Obj;
}

// ---------------------------------------------------------------------------
// animation.read_blend_profiles
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadBlendProfilesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("skeleton"), TEXT("path"), TEXT("skeletal_mesh") }, Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    UObject* Asset = nullptr;
    FString Error;
    USkeleton* Skeleton = ResolveSkeletonForProfile(Path, Asset, Error);
    if (!Skeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }
    FString Name;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("name"), TEXT("profile_name") }, Name);
    bool bBlendMasksOnly = false;
    Args->TryGetBoolField(TEXT("blend_masks_only"), bBlendMasksOnly);

    TArray<TSharedPtr<FJsonValue>> Profiles;
    for (TObjectPtr<UBlendProfile> ProfilePtr : Skeleton->BlendProfiles)
    {
        UBlendProfile* Profile = ProfilePtr.Get();
        if (!Profile)
        {
            continue;
        }
        if (!Name.IsEmpty() && Profile->GetFName() != FName(*Name))
        {
            continue;
        }
        if (bBlendMasksOnly && !Profile->IsBlendMask())
        {
            continue;
        }
        Profiles.Add(MakeShared<FJsonValueObject>(BlendProfileToJson(Profile)));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    R->SetStringField(TEXT("source_asset"), Asset ? Asset->GetPathName() : FString());
    R->SetArrayField(TEXT("profiles"), Profiles);
    R->SetNumberField(TEXT("count"), Profiles.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_blend_mask
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CreateBlendMaskImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    FString Name;
    if (!Args.IsValid() || !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("skeleton"), TEXT("path"), TEXT("skeletal_mesh") }, Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("name"), TEXT("profile_name"), TEXT("mask_name") }, Name) || Name.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }

    UObject* Asset = nullptr;
    FString Error;
    USkeleton* Skeleton = ResolveSkeletonForProfile(Path, Asset, Error);
    if (!Skeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    bool bDryRun = false;
    bool bSave = false;
    bool bClearExisting = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("clear_existing"), bClearExisting);

    const TArray<TSharedPtr<FJsonValue>>* EntriesArg = nullptr;
    Args->TryGetArrayField(TEXT("entries"), EntriesArg);
    if (!EntriesArg || EntriesArg->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing non-empty 'entries' array; each entry needs {bone, scale, recursive?}"));
    }

    TArray<TSharedPtr<FJsonValue>> Planned;
    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
    for (const TSharedPtr<FJsonValue>& Value : *EntriesArg)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("entries[] must contain objects"));
        }
        FString Bone;
        if (!(*Obj)->TryGetStringField(TEXT("bone"), Bone) || Bone.IsEmpty())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("entries[].bone is required"));
        }
        const int32 BoneIndex = RefSkel.FindBoneIndex(FName(*Bone));
        if (BoneIndex == INDEX_NONE)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("bone not found on skeleton: %s"), *Bone));
        }
        double Scale = 1.0;
        (*Obj)->TryGetNumberField(TEXT("scale"), Scale);
        bool bRecursive = true;
        (*Obj)->TryGetBoolField(TEXT("recursive"), bRecursive);
        auto PlannedObj = MakeShared<FJsonObject>();
        PlannedObj->SetStringField(TEXT("bone"), Bone);
        PlannedObj->SetNumberField(TEXT("bone_index"), BoneIndex);
        PlannedObj->SetNumberField(TEXT("scale"), Scale);
        PlannedObj->SetBoolField(TEXT("recursive"), bRecursive);
        Planned.Add(MakeShared<FJsonValueObject>(PlannedObj));
    }

    UBlendProfile* Existing = Skeleton->GetBlendProfile(FName(*Name));
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
        R->SetStringField(TEXT("name"), Name);
        R->SetBoolField(TEXT("exists"), Existing != nullptr);
        R->SetBoolField(TEXT("clear_existing"), bClearExisting);
        R->SetArrayField(TEXT("planned_entries"), Planned);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageCreateBlendMask", "Sage: Create Blend Mask"));
    Skeleton->Modify();
    UBlendProfile* Profile = Existing ? Existing : Skeleton->CreateNewBlendProfile(FName(*Name));
    if (!Profile)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to create blend mask: %s"), *Name));
    }
    Profile->Modify();
    Profile->Mode = EBlendProfileMode::BlendMask;
    Profile->SetSkeleton(Skeleton);
    if (bClearExisting)
    {
        Profile->ProfileEntries.Reset();
    }
    for (const TSharedPtr<FJsonValue>& Value : *EntriesArg)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        Value->TryGetObject(Obj);
        FString Bone;
        (*Obj)->TryGetStringField(TEXT("bone"), Bone);
        double Scale = 1.0;
        (*Obj)->TryGetNumberField(TEXT("scale"), Scale);
        bool bRecursive = true;
        (*Obj)->TryGetBoolField(TEXT("recursive"), bRecursive);
        Profile->SetBoneBlendScale(FName(*Bone), static_cast<float>(Scale), bRecursive, /*bCreate=*/true);
    }
    Profile->CleanupBoneEntries();
    Skeleton->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    R->SetStringField(TEXT("source_asset"), Asset ? Asset->GetPathName() : FString());
    R->SetBoolField(TEXT("created"), Existing == nullptr);
    R->SetBoolField(TEXT("updated"), Existing != nullptr);
    R->SetBoolField(TEXT("clear_existing"), bClearExisting);
    R->SetObjectField(TEXT("profile"), BlendProfileToJson(Profile));
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Skeleton, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_skeleton_bone
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddSkeletonBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SkeletonPath, BoneName, SourceMeshPath;
    if (!Args.IsValid() || !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("skeleton"), TEXT("path") }, SkeletonPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("bone"), TEXT("bone_name"), TEXT("name") }, BoneName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone'"));
    }
    Args->TryGetStringField(TEXT("source_skeletal_mesh"), SourceMeshPath);

    USkeleton* Skeleton = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *SkeletonPath));
    }
    if (Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName)) != INDEX_NONE)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
        R->SetStringField(TEXT("bone"), BoneName);
        R->SetBoolField(TEXT("already"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    bool bDryRun = false;
    bool bConfirmed = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    Args->TryGetBoolField(TEXT("save"), bSave);
    if (SourceMeshPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("safe standalone parent/transform bone insertion is not exposed by UE; pass source_skeletal_mesh to merge a bone from an existing skeletal mesh"));
    }

    USkeletalMesh* SourceMesh = Cast<USkeletalMesh>(ResolveAsset(SourceMeshPath));
    if (!SourceMesh)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeletalMesh: %s"), *SourceMeshPath));
    }
    if (SourceMesh->GetRefSkeleton().FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("bone not found on source_skeletal_mesh: %s"), *BoneName));
    }
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
        R->SetStringField(TEXT("source_skeletal_mesh"), SourceMesh->GetPathName());
        R->SetStringField(TEXT("bone"), BoneName);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("destructive skeleton hierarchy mutation; pass confirmed:true after dry_run"));
    }

    FScopedTransaction Tx(LOCTEXT("SageAddSkeletonBone", "Sage: Add Skeleton Bone From Mesh"));
    Skeleton->Modify();
    const bool bMerged = Skeleton->MergeAllBonesToBoneTree(SourceMesh, /*bShowProgress=*/false);
    const bool bAdded = Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName)) != INDEX_NONE;
    if (!bMerged || !bAdded)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("MergeAllBonesToBoneTree did not add bone: %s"), *BoneName));
    }
    Skeleton->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    R->SetStringField(TEXT("source_skeletal_mesh"), SourceMesh->GetPathName());
    R->SetStringField(TEXT("bone"), BoneName);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Skeleton, bSave, R);
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
    UPhysicsAsset* PhysicsAsset = SK->GetPhysicsAsset();
    if (PhysicsAsset)
    {
        R->SetStringField(TEXT("physics_asset"), PhysicsAsset->GetPathName());
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
    if (SMName.IsEmpty())
    {
        Args->TryGetStringField(TEXT("state_machine_name"), SMName);
    }

    // Resolve the SM sub-graph: explicit name or first one found in the AnimGraph.
    UAnimationStateMachineGraph* SMGraph = nullptr;
    if (!SMName.IsEmpty())
    {
        SMGraph = FindStateMachineGraph(BP, FName(*SMName));
    }
    else if (UEdGraph* AG = FindAnimGraph(BP))
    {
        for (UEdGraphNode* N : AG->Nodes)
        {
            if (UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(N))
            {
                if (SMNode->EditorStateMachineGraph)
                {
                    SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
                    SMName  = SMGraph ? SMGraph->GetFName().ToString() : SMName;
                    break;
                }
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BP->GetPathName());
    R->SetStringField(TEXT("state_machine_name"), SMName.IsEmpty() ? TEXT("") : SMName);

    if (!SMGraph)
    {
        R->SetBoolField(TEXT("found"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    R->SetBoolField(TEXT("found"), true);

    // States — mirror ListStatesImpl shape.
    TArray<TSharedPtr<FJsonValue>> States;
    for (UEdGraphNode* N : SMGraph->Nodes)
    {
        if (UAnimStateNodeBase* S = Cast<UAnimStateNodeBase>(N))
        {
            auto Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("state_id"), S->NodeGuid.ToString(EGuidFormats::Digits));
            Obj->SetStringField(TEXT("class"),    S->GetClass()->GetName());
            FString StateName = GetStateDisplayName(S);
            Obj->SetStringField(TEXT("name"), StateName);
            Obj->SetNumberField(TEXT("x"), S->NodePosX);
            Obj->SetNumberField(TEXT("y"), S->NodePosY);
            States.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }
    R->SetArrayField(TEXT("states"), States);
    R->SetNumberField(TEXT("state_count"), States.Num());

    // Transitions
    TArray<TSharedPtr<FJsonValue>> Trans;
    for (UEdGraphNode* N : SMGraph->Nodes)
    {
        if (UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N))
        {
            auto Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("transition_id"),
                                T->NodeGuid.ToString(EGuidFormats::Digits));
            UAnimStateNodeBase* From = T->GetPreviousState();
            UAnimStateNodeBase* To   = T->GetNextState();
            Obj->SetStringField(TEXT("from_state_id"),
                From ? From->NodeGuid.ToString(EGuidFormats::Digits) : FString());
            Obj->SetStringField(TEXT("to_state_id"),
                To   ? To->NodeGuid.ToString(EGuidFormats::Digits)   : FString());
            Obj->SetNumberField(TEXT("blend_time"),    T->CrossfadeDuration);
            Obj->SetNumberField(TEXT("priority"),      T->PriorityOrder);
            Obj->SetBoolField  (TEXT("bidirectional"), T->Bidirectional);
            Trans.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }
    R->SetArrayField(TEXT("transitions"), Trans);
    R->SetNumberField(TEXT("transition_count"), Trans.Num());
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

    // Optional graph name — default to the root AnimGraph.
    FString GraphName;
    Args->TryGetStringField(TEXT("graph_name"), GraphName);
    FAnimGraphReadOptions Options;
    ReadAnimGraphOptions(Args, Options,
                         /*bDefaultProperties=*/false,
                         /*bDefaultPins=*/false,
                         /*bDefaultConnections=*/false);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BP->GetPathName());

    TArray<TSharedPtr<FJsonValue>> Graphs;
    auto EmitGraph = [&](UEdGraph* G)
    {
        if (!G) return;
        Graphs.Add(MakeShared<FJsonValueObject>(AnimGraphToJson(BP, G, Options)));
    };

    if (!GraphName.IsEmpty())
    {
        UEdGraph* TargetGraph = ResolveAnimGraphTarget(BP, GraphName);
        if (!TargetGraph)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("graph '%s' not found"), *GraphName));
        }
        EmitGraph(TargetGraph);
    }
    else
    {
        TArray<UEdGraph*> AnimGraphs;
        CollectAnimBlueprintAnimGraphs(BP, AnimGraphs);
        for (UEdGraph* G : AnimGraphs)
        {
            EmitGraph(G);
        }
    }

    R->SetArrayField(TEXT("graphs"), Graphs);
    R->SetNumberField(TEXT("graph_count"), Graphs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_state_graph
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadStateGraphImpl(const TSharedPtr<FJsonObject>& Args)
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

    FString SMName, StateName, StateId;
    Args->TryGetStringField(TEXT("state_machine_name"), SMName);
    if (SMName.IsEmpty())
    {
        Args->TryGetStringField(TEXT("graph_name"), SMName);
    }
    Args->TryGetStringField(TEXT("state_name"), StateName);
    Args->TryGetStringField(TEXT("state_id"), StateId);
    if (StateName.IsEmpty() && StateId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'state_name' or 'state_id'"));
    }

    UAnimationStateMachineGraph* SMGraph = nullptr;
    FString ResolveError;
    UAnimStateNodeBase* State = ResolveStateNodeForRead(
        BP, SMName, StateName, StateId, SMGraph, ResolveError);
    if (!State)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, ResolveError);
    }
    UEdGraph* StateGraph = GetStateBoundGraph(State);
    if (!StateGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state '%s' has no bound graph"),
                *GetStateDisplayName(State)));
    }

    FAnimGraphReadOptions Options;
    ReadAnimGraphOptions(Args, Options,
                         /*bDefaultProperties=*/true,
                         /*bDefaultPins=*/true,
                         /*bDefaultConnections=*/true);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BP->GetPathName());
    R->SetStringField(TEXT("state_machine_name"), SMGraph ? SMGraph->GetName() : FString());
    R->SetStringField(TEXT("state_id"), State->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("state_name"), GetStateDisplayName(State));
    R->SetStringField(TEXT("state_class"), State->GetClass()->GetPathName());
    R->SetStringField(TEXT("graph_name"), StateGraph->GetName());
    R->SetStringField(TEXT("graph_kind"), GraphKind(StateGraph));
    R->SetObjectField(TEXT("graph"), AnimGraphToJson(BP, StateGraph, Options));

    bool bIncludeT3d = false;
    Args->TryGetBoolField(TEXT("include_t3d"), bIncludeT3d);
    if (bIncludeT3d)
    {
        R->SetStringField(TEXT("t3d_skip_reason"),
            TEXT("animation.read_state_graph is compact JSON-only; use bp.full_dump(include_t3d:true) for clipboard T3D"));
    }
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
    if (!Args->TryGetStringField(TEXT("bone"), BoneName) || BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone'"));
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

    const int32 BoneIdx = Skel->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName));
    if (BoneIdx == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("bone '%s' not found in skeleton"), *BoneName));
    }

    const IAnimationDataModel* Model = Seq->GetDataModel();
    if (!Model)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("sequence has no animation data model"));
    }
    if (!Model->IsValidBoneTrackName(FName(*BoneName)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("AnimSequence has no animated track for bone '%s'"), *BoneName));
    }

    double StartFrameD = 0.0;
    double EndFrameD = -1.0;
    if (Args.IsValid())
    {
        if (Args->HasField(TEXT("frame")))
        {
            Args->TryGetNumberField(TEXT("frame"), StartFrameD);
            EndFrameD = StartFrameD;
        }
        Args->TryGetNumberField(TEXT("start_frame"), StartFrameD);
        Args->TryGetNumberField(TEXT("end_frame"), EndFrameD);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      Seq->GetPathName());
    R->SetStringField(TEXT("bone_name"), BoneName);
    R->SetNumberField(TEXT("bone_index"), BoneIdx);
    R->SetNumberField(TEXT("duration"), Model->GetPlayLength());
    R->SetNumberField(TEXT("sample_rate"), Model->GetFrameRate().AsDecimal());
    R->SetNumberField(TEXT("number_of_frames"), Model->GetNumberOfFrames());
    R->SetNumberField(TEXT("number_of_keys"), Model->GetNumberOfKeys());
    AddAnimSequenceTrackReadback(R, Seq, BoneName,
        static_cast<int32>(StartFrameD),
        static_cast<int32>(EndFrameD));
    AddRootMotionSummary(R, Seq);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_animation_curves
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadAnimationCurvesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }
    const IAnimationDataModel* Model = Seq->GetDataModel();
    if (!Model)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("sequence has no animation data model"));
    }

    bool bIncludeKeys = true;
    Args->TryGetBoolField(TEXT("include_keys"), bIncludeKeys);
    TArray<FString> RequestedNames;
    ReadStringArrayField(Args, TEXT("curve_names"), RequestedNames);
    TSet<FName> Filter;
    for (const FString& Name : RequestedNames)
    {
        if (!Name.IsEmpty())
        {
            Filter.Add(FName(*Name));
        }
    }

    const double FrameRateDecimal = Model->GetFrameRate().AsDecimal();
    auto TimeToFrame = [FrameRateDecimal](float Time) -> int32
    {
        return FrameRateDecimal > 0.0 ? FMath::RoundToInt(static_cast<double>(Time) * FrameRateDecimal) : 0;
    };

    TArray<TSharedPtr<FJsonValue>> FloatCurves;
    for (const FFloatCurve& Curve : Model->GetFloatCurves())
    {
        const FName CurveName = Curve.GetName();
        if (!HasCurveNameFilter(Filter, CurveName)) continue;

        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), CurveName.ToString());
        Obj->SetStringField(TEXT("type"), TEXT("float"));
        Obj->SetNumberField(TEXT("flags"), Curve.GetCurveTypeFlags());
        Obj->SetNumberField(TEXT("key_count"), Curve.FloatCurve.GetNumKeys());
#if WITH_EDITORONLY_DATA
        const FLinearColor Color = Curve.GetColor();
        TArray<TSharedPtr<FJsonValue>> ColorArr;
        ColorArr.Add(MakeShared<FJsonValueNumber>(Color.R));
        ColorArr.Add(MakeShared<FJsonValueNumber>(Color.G));
        ColorArr.Add(MakeShared<FJsonValueNumber>(Color.B));
        ColorArr.Add(MakeShared<FJsonValueNumber>(Color.A));
        Obj->SetArrayField(TEXT("color"), ColorArr);
#endif
        if (bIncludeKeys)
        {
            TArray<TSharedPtr<FJsonValue>> Keys;
            for (const FRichCurveKey& Key : Curve.FloatCurve.GetConstRefOfKeys())
            {
                auto KeyObj = RichCurveKeyToJson(Key);
                KeyObj->SetNumberField(TEXT("frame"), TimeToFrame(Key.Time));
                Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
            Obj->SetArrayField(TEXT("keys"), Keys);
        }
        FloatCurves.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TArray<TSharedPtr<FJsonValue>> TransformCurves;
    for (const FTransformCurve& Curve : Model->GetTransformCurves())
    {
        const FName CurveName = Curve.GetName();
        if (!HasCurveNameFilter(Filter, CurveName)) continue;

        TArray<float> Times;
        TArray<FTransform> Values;
        Curve.GetKeys(Times, Values);

        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), CurveName.ToString());
        Obj->SetStringField(TEXT("type"), TEXT("transform"));
        Obj->SetNumberField(TEXT("flags"), Curve.GetCurveTypeFlags());
        Obj->SetNumberField(TEXT("key_count"), Values.Num());
        if (bIncludeKeys)
        {
            TArray<TSharedPtr<FJsonValue>> Keys;
            for (int32 I = 0; I < Values.Num(); ++I)
            {
                auto KeyObj = TransformToJsonObject(Values[I]);
                const float Time = Times.IsValidIndex(I) ? Times[I] : 0.0f;
                KeyObj->SetNumberField(TEXT("time"), Time);
                KeyObj->SetNumberField(TEXT("frame"), TimeToFrame(Time));
                Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
            Obj->SetArrayField(TEXT("keys"), Keys);
        }
        TransformCurves.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TArray<TSharedPtr<FJsonValue>> Attributes;
    for (const FAnimatedBoneAttribute& Attribute : Model->GetAttributes())
    {
        const FAnimationAttributeIdentifier& Identifier = Attribute.Identifier;
        if (!HasCurveNameFilter(Filter, Identifier.GetName())) continue;

        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Identifier.GetName().ToString());
        Obj->SetStringField(TEXT("type"), TEXT("attribute"));
        Obj->SetStringField(TEXT("bone_name"), Identifier.GetBoneName().ToString());
        Obj->SetNumberField(TEXT("bone_index"), Identifier.GetBoneIndex());
        Obj->SetStringField(TEXT("value_type"), Identifier.GetScriptStructPath().ToString());
        Obj->SetNumberField(TEXT("key_count"), Attribute.Curve.GetConstRefOfKeys().Num());
        if (bIncludeKeys)
        {
            TArray<TSharedPtr<FJsonValue>> Keys;
            for (const FAttributeKey& Key : Attribute.Curve.GetConstRefOfKeys())
            {
                auto KeyObj = MakeShared<FJsonObject>();
                KeyObj->SetNumberField(TEXT("time"), Key.Time);
                KeyObj->SetNumberField(TEXT("frame"), TimeToFrame(Key.Time));
                Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
            Obj->SetArrayField(TEXT("keys"), Keys);
        }
        Attributes.Add(MakeShared<FJsonValueObject>(Obj));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetNumberField(TEXT("duration"), Model->GetPlayLength());
    R->SetNumberField(TEXT("sample_rate"), Model->GetFrameRate().AsDecimal());
    R->SetNumberField(TEXT("float_curve_count"), FloatCurves.Num());
    R->SetNumberField(TEXT("transform_curve_count"), TransformCurves.Num());
    R->SetNumberField(TEXT("attribute_count"), Attributes.Num());
    R->SetArrayField(TEXT("float_curves"), FloatCurves);
    R->SetArrayField(TEXT("transform_curves"), TransformCurves);
    R->SetArrayField(TEXT("attributes"), Attributes);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.list_control_rig_variables
// ---------------------------------------------------------------------------

UControlRigBlueprint* ResolveControlRigBlueprintAsset(const FString& Path)
{
    UObject* Asset = ResolveAsset(Path);
    if (UControlRigBlueprint* RigBP = Cast<UControlRigBlueprint>(Asset))
    {
        return RigBP;
    }
    if (UBlueprint* BP = Cast<UBlueprint>(Asset))
    {
        return Cast<UControlRigBlueprint>(BP);
    }
    return nullptr;
}

FString RigElementTypeToString(ERigElementType Type)
{
    switch (Type)
    {
    case ERigElementType::Bone: return TEXT("bone");
    case ERigElementType::Null: return TEXT("null");
    case ERigElementType::Control: return TEXT("control");
    case ERigElementType::Curve: return TEXT("curve");
    case ERigElementType::Connector: return TEXT("connector");
    case ERigElementType::Socket: return TEXT("socket");
    case ERigElementType::Reference: return TEXT("reference");
    default: return TEXT("unknown");
    }
}

bool ParseRigElementType(const FString& InType, ERigElementType& OutType)
{
    const FString Type = InType.TrimStartAndEnd().ToLower();
    if (Type.IsEmpty() || Type == TEXT("none"))
    {
        OutType = ERigElementType::None;
        return true;
    }
    if (Type == TEXT("bone")) { OutType = ERigElementType::Bone; return true; }
    if (Type == TEXT("null") || Type == TEXT("space")) { OutType = ERigElementType::Null; return true; }
    if (Type == TEXT("control")) { OutType = ERigElementType::Control; return true; }
    if (Type == TEXT("curve")) { OutType = ERigElementType::Curve; return true; }
    if (Type == TEXT("connector")) { OutType = ERigElementType::Connector; return true; }
    if (Type == TEXT("socket")) { OutType = ERigElementType::Socket; return true; }
    if (Type == TEXT("reference")) { OutType = ERigElementType::Reference; return true; }
    return false;
}

FString RigControlTypeToString(ERigControlType Type)
{
    switch (Type)
    {
    case ERigControlType::Bool: return TEXT("bool");
    case ERigControlType::Float: return TEXT("float");
    case ERigControlType::Integer: return TEXT("integer");
    case ERigControlType::Vector2D: return TEXT("vector2d");
    case ERigControlType::Position: return TEXT("position");
    case ERigControlType::Scale: return TEXT("scale");
    case ERigControlType::Rotator: return TEXT("rotator");
    case ERigControlType::Transform: return TEXT("transform");
    case ERigControlType::TransformNoScale: return TEXT("transform_no_scale");
    case ERigControlType::EulerTransform: return TEXT("euler_transform");
    case ERigControlType::ScaleFloat: return TEXT("scale_float");
    default: return TEXT("unknown");
    }
}

bool ParseRigControlType(const FString& InType, ERigControlType& OutType)
{
    const FString Type = InType.TrimStartAndEnd().ToLower();
    if (Type.IsEmpty() || Type == TEXT("transform") || Type == TEXT("euler_transform"))
    {
        OutType = ERigControlType::EulerTransform;
        return true;
    }
    if (Type == TEXT("bool")) { OutType = ERigControlType::Bool; return true; }
    if (Type == TEXT("float")) { OutType = ERigControlType::Float; return true; }
    if (Type == TEXT("integer") || Type == TEXT("int")) { OutType = ERigControlType::Integer; return true; }
    if (Type == TEXT("vector2d")) { OutType = ERigControlType::Vector2D; return true; }
    if (Type == TEXT("position") || Type == TEXT("vector")) { OutType = ERigControlType::Position; return true; }
    if (Type == TEXT("scale")) { OutType = ERigControlType::Scale; return true; }
    if (Type == TEXT("rotator") || Type == TEXT("rotation")) { OutType = ERigControlType::Rotator; return true; }
    if (Type == TEXT("transform_no_scale")) { OutType = ERigControlType::TransformNoScale; return true; }
    if (Type == TEXT("scale_float")) { OutType = ERigControlType::ScaleFloat; return true; }
    return false;
}

FString RigControlAnimationTypeToString(ERigControlAnimationType Type)
{
    switch (Type)
    {
    case ERigControlAnimationType::AnimationControl: return TEXT("animation_control");
    case ERigControlAnimationType::AnimationChannel: return TEXT("animation_channel");
    case ERigControlAnimationType::ProxyControl: return TEXT("proxy_control");
    case ERigControlAnimationType::VisualCue: return TEXT("visual_cue");
    default: return TEXT("unknown");
    }
}

bool ParseRigControlAnimationType(const FString& InType, ERigControlAnimationType& OutType)
{
    const FString Type = InType.TrimStartAndEnd().ToLower();
    if (Type.IsEmpty() || Type == TEXT("animation_control") || Type == TEXT("control"))
    {
        OutType = ERigControlAnimationType::AnimationControl;
        return true;
    }
    if (Type == TEXT("animation_channel") || Type == TEXT("channel"))
    {
        OutType = ERigControlAnimationType::AnimationChannel;
        return true;
    }
    if (Type == TEXT("proxy_control") || Type == TEXT("proxy"))
    {
        OutType = ERigControlAnimationType::ProxyControl;
        return true;
    }
    if (Type == TEXT("visual_cue") || Type == TEXT("visual"))
    {
        OutType = ERigControlAnimationType::VisualCue;
        return true;
    }
    return false;
}

TSharedRef<FJsonObject> RigElementKeyToJson(const FRigElementKey& Key)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Key.Name.ToString());
    Obj->SetStringField(TEXT("type"), RigElementTypeToString(Key.Type));
    return Obj;
}

TArray<TSharedPtr<FJsonValue>> RigElementKeysToJson(const TArray<FRigElementKey>& Keys)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FRigElementKey& Key : Keys)
    {
        Out.Add(MakeShared<FJsonValueObject>(RigElementKeyToJson(Key)));
    }
    return Out;
}

TSharedRef<FJsonObject> RigControlValueToJson(
    const FRigControlValue& Value,
    ERigControlType ControlType,
    ERigControlAxis PrimaryAxis)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetBoolField(TEXT("valid"), Value.IsValid());
    Obj->SetStringField(TEXT("control_type"), RigControlTypeToString(ControlType));
    if (!Value.IsValid())
    {
        return Obj;
    }

    switch (ControlType)
    {
    case ERigControlType::Bool:
        Obj->SetBoolField(TEXT("value"), Value.Get<bool>());
        break;
    case ERigControlType::Float:
    case ERigControlType::ScaleFloat:
        Obj->SetNumberField(TEXT("value"), Value.Get<float>());
        break;
    case ERigControlType::Integer:
        Obj->SetNumberField(TEXT("value"), Value.Get<int32>());
        break;
    case ERigControlType::Vector2D:
    {
        const FVector3f V = Value.Get<FVector3f>();
        Obj->SetField(TEXT("value"), detail::Vec3ToJson(FVector(V.X, V.Y, 0.0)));
        break;
    }
    case ERigControlType::Position:
    case ERigControlType::Scale:
    case ERigControlType::Rotator:
    {
        const FVector3f V = Value.Get<FVector3f>();
        Obj->SetField(TEXT("value"), detail::Vec3ToJson(FVector(V)));
        break;
    }
    default:
        Obj->SetObjectField(TEXT("transform"), TransformToJsonObject(Value.GetAsTransform(ControlType, PrimaryAxis)));
        break;
    }
    Obj->SetObjectField(TEXT("as_transform"), TransformToJsonObject(Value.GetAsTransform(ControlType, PrimaryAxis)));
    return Obj;
}

TSharedRef<FJsonObject> RigElementToJson(URigHierarchy* Hierarchy, const FRigElementKey& Key, bool bIncludeTransforms)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Key.Name.ToString());
    Obj->SetStringField(TEXT("type"), RigElementTypeToString(Key.Type));
    if (!Hierarchy)
    {
        return Obj;
    }

    Obj->SetArrayField(TEXT("parents"), RigElementKeysToJson(Hierarchy->GetParents(Key, false)));
    Obj->SetArrayField(TEXT("children"), RigElementKeysToJson(Hierarchy->GetChildren(Key, false)));

    if (Key.Type == ERigElementType::Curve)
    {
        Obj->SetNumberField(TEXT("value"), Hierarchy->GetCurveValue(Key));
    }
    if (bIncludeTransforms &&
        (Key.Type == ERigElementType::Bone ||
         Key.Type == ERigElementType::Null ||
         Key.Type == ERigElementType::Control ||
         Key.Type == ERigElementType::Socket))
    {
        Obj->SetObjectField(TEXT("current_local"), TransformToJsonObject(Hierarchy->GetLocalTransform(Key, false)));
        Obj->SetObjectField(TEXT("current_global"), TransformToJsonObject(Hierarchy->GetGlobalTransform(Key, false)));
        Obj->SetObjectField(TEXT("initial_local"), TransformToJsonObject(Hierarchy->GetLocalTransform(Key, true)));
        Obj->SetObjectField(TEXT("initial_global"), TransformToJsonObject(Hierarchy->GetGlobalTransform(Key, true)));
    }
    return Obj;
}

TSharedRef<FJsonObject> RigControlToJson(URigHierarchy* Hierarchy, FRigControlElement* Control, bool bIncludeTransforms)
{
    const FRigElementKey Key = Control ? Control->GetKey() : FRigElementKey();
    TSharedRef<FJsonObject> Obj = RigElementToJson(Hierarchy, Key, bIncludeTransforms);
    if (!Hierarchy || !Control)
    {
        return Obj;
    }

    const FRigControlSettings& Settings = Control->Settings;
    Obj->SetStringField(TEXT("control_type"), RigControlTypeToString(Settings.ControlType));
    Obj->SetStringField(TEXT("animation_type"), RigControlAnimationTypeToString(Settings.AnimationType));
    Obj->SetStringField(TEXT("display_name"), Settings.DisplayName.ToString());
    Obj->SetStringField(TEXT("primary_axis"), StaticEnum<ERigControlAxis>()->GetNameStringByValue(static_cast<int64>(Settings.PrimaryAxis)));
    Obj->SetBoolField(TEXT("shape_visible"), Settings.bShapeVisible);
    Obj->SetStringField(TEXT("shape_name"), Settings.ShapeName.ToString());
    Obj->SetBoolField(TEXT("selectable"), Settings.IsSelectable(/*bRespectVisibility=*/true));
    Obj->SetBoolField(TEXT("animatable"), Settings.IsAnimatable());
    Obj->SetObjectField(TEXT("shape_color"), [&Settings]()
    {
        auto C = MakeShared<FJsonObject>();
        C->SetNumberField(TEXT("r"), Settings.ShapeColor.R);
        C->SetNumberField(TEXT("g"), Settings.ShapeColor.G);
        C->SetNumberField(TEXT("b"), Settings.ShapeColor.B);
        C->SetNumberField(TEXT("a"), Settings.ShapeColor.A);
        return C;
    }());
    Obj->SetObjectField(TEXT("current_value"), RigControlValueToJson(
        Hierarchy->GetControlValue(Control, ERigControlValueType::Current),
        Settings.ControlType,
        Settings.PrimaryAxis));
    Obj->SetObjectField(TEXT("initial_value"), RigControlValueToJson(
        Hierarchy->GetControlValue(Control, ERigControlValueType::Initial),
        Settings.ControlType,
        Settings.PrimaryAxis));
    Obj->SetObjectField(TEXT("minimum_value"), RigControlValueToJson(
        Settings.MinimumValue,
        Settings.ControlType,
        Settings.PrimaryAxis));
    Obj->SetObjectField(TEXT("maximum_value"), RigControlValueToJson(
        Settings.MaximumValue,
        Settings.ControlType,
        Settings.PrimaryAxis));
    if (bIncludeTransforms)
    {
        Obj->SetObjectField(TEXT("offset_current_local"), TransformToJsonObject(Control->GetOffsetTransform()[ERigTransformType::CurrentLocal].Get()));
        Obj->SetObjectField(TEXT("offset_initial_local"), TransformToJsonObject(Control->GetOffsetTransform()[ERigTransformType::InitialLocal].Get()));
        Obj->SetObjectField(TEXT("shape_current_local"), TransformToJsonObject(Control->GetShapeTransform()[ERigTransformType::CurrentLocal].Get()));
        Obj->SetObjectField(TEXT("shape_initial_local"), TransformToJsonObject(Control->GetShapeTransform()[ERigTransformType::InitialLocal].Get()));
    }
    return Obj;
}

TSharedRef<FJsonObject> ControlRigSummaryToJson(UControlRigBlueprint* RigBP, bool bIncludeElements, bool bIncludeControls, bool bIncludeTransforms)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), RigBP ? RigBP->GetPathName() : FString());
    if (!RigBP)
    {
        return R;
    }

    URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(RigBP);
    USkeletalMesh* PreviewMesh = UControlRigBlueprintEditorLibrary::GetPreviewMesh(RigBP);
    R->SetStringField(TEXT("class"), RigBP->GetClass()->GetName());
    R->SetStringField(TEXT("generated_class"), RigBP->GeneratedClass ? RigBP->GeneratedClass->GetPathName() : FString());
    R->SetStringField(TEXT("control_rig_class"), RigBP->GetControlRigClass() ? RigBP->GetControlRigClass()->GetPathName() : FString());
    R->SetStringField(TEXT("preview_mesh"), PreviewMesh ? PreviewMesh->GetPathName() : FString());
    R->SetBoolField(TEXT("has_hierarchy"), Hierarchy != nullptr);
    if (!Hierarchy)
    {
        return R;
    }

    const TArray<FRigElementKey> BoneKeys = Hierarchy->GetBoneKeys(true);
    const TArray<FRigElementKey> NullKeys = Hierarchy->GetNullKeys(true);
    const TArray<FRigElementKey> ControlKeys = Hierarchy->GetControlKeys(true);
    const TArray<FRigElementKey> CurveKeys = Hierarchy->GetCurveKeys();
    const TArray<FRigElementKey> ConnectorKeys = Hierarchy->GetConnectorKeys(true);
    const TArray<FRigElementKey> SocketKeys = Hierarchy->GetSocketKeys(true);
    const TArray<FRigElementKey> RootKeys = Hierarchy->GetRootElementKeys();
    R->SetNumberField(TEXT("bone_count"), BoneKeys.Num());
    R->SetNumberField(TEXT("null_count"), NullKeys.Num());
    R->SetNumberField(TEXT("control_count"), ControlKeys.Num());
    R->SetNumberField(TEXT("curve_count"), CurveKeys.Num());
    R->SetNumberField(TEXT("connector_count"), ConnectorKeys.Num());
    R->SetNumberField(TEXT("socket_count"), SocketKeys.Num());
    R->SetNumberField(TEXT("root_count"), RootKeys.Num());
    R->SetArrayField(TEXT("root_elements"), RigElementKeysToJson(RootKeys));

    if (bIncludeControls)
    {
        TArray<TSharedPtr<FJsonValue>> Controls;
        for (FRigControlElement* Control : Hierarchy->GetControls(true))
        {
            if (!Control) continue;
            Controls.Add(MakeShared<FJsonValueObject>(RigControlToJson(Hierarchy, Control, bIncludeTransforms)));
        }
        R->SetArrayField(TEXT("controls"), Controls);
    }
    if (bIncludeElements)
    {
        auto KeysToElements = [Hierarchy, bIncludeTransforms](const TArray<FRigElementKey>& Keys)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const FRigElementKey& Key : Keys)
            {
                Arr.Add(MakeShared<FJsonValueObject>(RigElementToJson(Hierarchy, Key, bIncludeTransforms)));
            }
            return Arr;
        };
        R->SetArrayField(TEXT("bones"), KeysToElements(BoneKeys));
        R->SetArrayField(TEXT("nulls"), KeysToElements(NullKeys));
        R->SetArrayField(TEXT("curves"), KeysToElements(CurveKeys));
        R->SetArrayField(TEXT("connectors"), KeysToElements(ConnectorKeys));
        R->SetArrayField(TEXT("sockets"), KeysToElements(SocketKeys));
    }
    return R;
}

bool ParseControlRigTransformArgs(const TSharedPtr<FJsonObject>& Args, FTransform& Out)
{
    bool bParsed = false;
    const TSharedPtr<FJsonObject>* TransformObj = nullptr;
    if (Args.IsValid() && Args->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj && TransformObj->IsValid())
    {
        bParsed = ReadAnimationTransform(*TransformObj, Out) || bParsed;
    }
    if (Args.IsValid())
    {
        FVector Location = Out.GetLocation();
        FVector Scale = Out.GetScale3D();
        FRotator Rotation = Out.GetRotation().Rotator();
        if (ReadVectorArray(Args, TEXT("location"), Location))
        {
            Out.SetLocation(Location);
            bParsed = true;
        }
        if (ReadVectorArray(Args, TEXT("scale"), Scale))
        {
            Out.SetScale3D(Scale);
            bParsed = true;
        }
        if (ReadRotatorArray(Args, TEXT("rotation"), Rotation))
        {
            Out.SetRotation(Rotation.Quaternion());
            bParsed = true;
        }
    }
    return bParsed;
}

bool CompileControlRigIfRequested(UControlRigBlueprint* RigBP, bool bCompile, TSharedRef<FJsonObject> Result)
{
    Result->SetBoolField(TEXT("compile_requested"), bCompile);
    if (!bCompile)
    {
        return true;
    }
    if (!RigBP)
    {
        Result->SetStringField(TEXT("compile_error"), TEXT("ControlRigBlueprint is null"));
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(RigBP);
    Result->SetBoolField(TEXT("compiled"), true);
    return true;
}

FSageToolDispatch::FOutcome ControlRigReadImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UControlRigBlueprint* RigBP = ResolveControlRigBlueprintAsset(Path);
    if (!RigBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UControlRigBlueprint: %s"), *Path));
    }
    bool bIncludeElements = true;
    bool bIncludeControls = true;
    bool bIncludeTransforms = true;
    Args->TryGetBoolField(TEXT("include_elements"), bIncludeElements);
    Args->TryGetBoolField(TEXT("include_controls"), bIncludeControls);
    Args->TryGetBoolField(TEXT("include_transforms"), bIncludeTransforms);
    return FSageToolDispatch::FOutcome::MakeSuccess(
        ControlRigSummaryToJson(RigBP, bIncludeElements, bIncludeControls, bIncludeTransforms));
}

FSageToolDispatch::FOutcome ControlRigListControlsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UControlRigBlueprint* RigBP = ResolveControlRigBlueprintAsset(Path);
    if (!RigBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UControlRigBlueprint: %s"), *Path));
    }
    URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(RigBP);
    if (!Hierarchy)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("ControlRig hierarchy unavailable"));
    }

    bool bIncludeTransforms = true;
    FString NameContains;
    Args->TryGetBoolField(TEXT("include_transforms"), bIncludeTransforms);
    Args->TryGetStringField(TEXT("name_contains"), NameContains);

    TArray<TSharedPtr<FJsonValue>> Controls;
    for (FRigControlElement* Control : Hierarchy->GetControls(true))
    {
        if (!Control) continue;
        const FString Name = Control->GetKey().Name.ToString();
        if (!NameContains.IsEmpty() && !Name.Contains(NameContains, ESearchCase::IgnoreCase))
        {
            continue;
        }
        Controls.Add(MakeShared<FJsonValueObject>(RigControlToJson(Hierarchy, Control, bIncludeTransforms)));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), RigBP->GetPathName());
    R->SetArrayField(TEXT("controls"), Controls);
    R->SetNumberField(TEXT("count"), Controls.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ControlRigSetPreviewMeshImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, MeshPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    const TArray<const TCHAR*> PreviewMeshFields{TEXT("preview_mesh"), TEXT("skeletal_mesh")};
    if (!TryGetAnyStringField(Args, PreviewMeshFields, MeshPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'preview_mesh'"));
    }
    UControlRigBlueprint* RigBP = ResolveControlRigBlueprintAsset(Path);
    if (!RigBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UControlRigBlueprint"));
    USkeletalMesh* Mesh = Cast<USkeletalMesh>(ResolveAsset(MeshPath));
    if (!Mesh) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("preview_mesh is not a USkeletalMesh"));

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), RigBP->GetPathName());
    R->SetStringField(TEXT("before_preview_mesh"), AssetPathOrEmpty(UControlRigBlueprintEditorLibrary::GetPreviewMesh(RigBP)));
    R->SetStringField(TEXT("requested_preview_mesh"), Mesh->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (bDryRun)
    {
        R->SetBoolField(TEXT("changed"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SetControlRigPreviewMesh", "Sage: Set Control Rig Preview Mesh"));
    RigBP->UBlueprint::Modify();
    UControlRigBlueprintEditorLibrary::SetPreviewMesh(RigBP, Mesh, true);
    UControlRigBlueprintEditorLibrary::RequestControlRigInit(RigBP);
    RigBP->MarkPackageDirty();
    R->SetBoolField(TEXT("changed"), true);
    R->SetStringField(TEXT("after_preview_mesh"), AssetPathOrEmpty(UControlRigBlueprintEditorLibrary::GetPreviewMesh(RigBP)));
    TrySaveLoadedAssetIfRequested(RigBP, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ControlRigSetControlTransformImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, ControlName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("control"), ControlName) && !Args->TryGetStringField(TEXT("name"), ControlName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'control'"));
    }
    UControlRigBlueprint* RigBP = ResolveControlRigBlueprintAsset(Path);
    if (!RigBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UControlRigBlueprint"));
    URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(RigBP);
    if (!Hierarchy) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("ControlRig hierarchy unavailable"));
    const FRigElementKey Key(FName(*ControlName), ERigElementType::Control);
    FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key);
    if (!Control)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("control '%s' not found"), *ControlName));
    }

    FTransform Transform = Hierarchy->GetLocalTransform(Key, false);
    if (!ParseControlRigTransformArgs(Args, Transform))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing transform/location/rotation/scale"));
    }
    FString Space = TEXT("local");
    Args->TryGetStringField(TEXT("space"), Space);
    Space = Space.TrimStartAndEnd().ToLower();
    if (Space.IsEmpty()) Space = TEXT("local");
    if (Space != TEXT("local") && Space != TEXT("global"))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("'space' must be local or global"));
    }
    bool bInitial = false;
    bool bAffectChildren = true;
    bool bDryRun = false;
    bool bSave = false;
    bool bCompile = true;
    Args->TryGetBoolField(TEXT("initial"), bInitial);
    Args->TryGetBoolField(TEXT("affect_children"), bAffectChildren);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), RigBP->GetPathName());
    R->SetStringField(TEXT("control"), ControlName);
    R->SetStringField(TEXT("space"), Space);
    R->SetBoolField(TEXT("initial"), bInitial);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetObjectField(TEXT("before"), RigControlToJson(Hierarchy, Control, true));
    R->SetObjectField(TEXT("requested_transform"), TransformToJsonObject(Transform));
    if (bDryRun)
    {
        R->SetBoolField(TEXT("changed"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SetControlRigControlTransform", "Sage: Set Control Rig Control Transform"));
    RigBP->UBlueprint::Modify();
    Hierarchy->Modify();
    if (Space == TEXT("global"))
    {
        Hierarchy->SetGlobalTransform(Key, Transform, bInitial, bAffectChildren, /*bSetupUndo=*/true);
    }
    else
    {
        Hierarchy->SetLocalTransform(Key, Transform, bInitial, bAffectChildren, /*bSetupUndo=*/true);
    }
    RigBP->MarkPackageDirty();
    CompileControlRigIfRequested(RigBP, bCompile, R);
    R->SetBoolField(TEXT("changed"), true);
    R->SetObjectField(TEXT("after"), RigControlToJson(Hierarchy, Control, true));
    TrySaveLoadedAssetIfRequested(RigBP, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ControlRigAddControlImpl(const TSharedPtr<FJsonObject>& Args)
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
    UControlRigBlueprint* RigBP = ResolveControlRigBlueprintAsset(Path);
    if (!RigBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UControlRigBlueprint"));
    URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(RigBP);
    URigHierarchyController* Controller = UControlRigBlueprintEditorLibrary::GetHierarchyController(RigBP);
    if (!Hierarchy || !Controller) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("ControlRig hierarchy/controller unavailable"));

    const FRigElementKey NewKey(FName(*Name), ERigElementType::Control);
    if (Hierarchy->Contains(NewKey))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("control '%s' already exists"), *Name));
    }

    FString ControlTypeStr;
    Args->TryGetStringField(TEXT("control_type"), ControlTypeStr);
    ERigControlType ControlType = ERigControlType::EulerTransform;
    if (!ParseRigControlType(ControlTypeStr, ControlType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("unsupported control_type"));
    }
    FString AnimationTypeStr;
    Args->TryGetStringField(TEXT("animation_type"), AnimationTypeStr);
    ERigControlAnimationType AnimationType = ERigControlAnimationType::AnimationControl;
    if (!ParseRigControlAnimationType(AnimationTypeStr, AnimationType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("unsupported animation_type"));
    }

    FString ParentName;
    FString ParentTypeStr = TEXT("null");
    Args->TryGetStringField(TEXT("parent"), ParentName);
    Args->TryGetStringField(TEXT("parent_name"), ParentName);
    Args->TryGetStringField(TEXT("parent_type"), ParentTypeStr);
    ERigElementType ParentType = ERigElementType::None;
    FRigElementKey ParentKey;
    if (!ParentName.IsEmpty())
    {
        if (!ParseRigElementType(ParentTypeStr, ParentType) || ParentType == ERigElementType::None)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("unsupported parent_type"));
        }
        ParentKey = FRigElementKey(FName(*ParentName), ParentType);
        if (!Hierarchy->Contains(ParentKey))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("parent '%s' of type '%s' not found"), *ParentName, *ParentTypeStr));
        }
    }

    FTransform ValueTransform = FTransform::Identity;
    ParseControlRigTransformArgs(Args, ValueTransform);
    FTransform OffsetTransform = FTransform::Identity;
    const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
    if (Args->TryGetObjectField(TEXT("offset_transform"), OffsetObj) && OffsetObj && OffsetObj->IsValid())
    {
        ReadAnimationTransform(*OffsetObj, OffsetTransform);
    }
    FTransform ShapeTransform = FTransform::Identity;
    const TSharedPtr<FJsonObject>* ShapeObj = nullptr;
    if (Args->TryGetObjectField(TEXT("shape_transform"), ShapeObj) && ShapeObj && ShapeObj->IsValid())
    {
        ReadAnimationTransform(*ShapeObj, ShapeTransform);
    }

    bool bDryRun = false;
    bool bSave = false;
    bool bCompile = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    FRigControlSettings Settings;
    Settings.ControlType = ControlType;
    Settings.AnimationType = AnimationType;
    FString DisplayName;
    if (Args->TryGetStringField(TEXT("display_name"), DisplayName))
    {
        Settings.DisplayName = FName(*DisplayName);
    }
    FString ShapeName;
    if (Args->TryGetStringField(TEXT("shape_name"), ShapeName))
    {
        Settings.ShapeName = FName(*ShapeName);
    }
    bool bShapeVisible = Settings.bShapeVisible;
    if (Args->TryGetBoolField(TEXT("shape_visible"), bShapeVisible))
    {
        Settings.bShapeVisible = bShapeVisible;
    }
    FRigControlValue Value;
    Value.SetFromTransform(ValueTransform, Settings.ControlType, Settings.PrimaryAxis);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), RigBP->GetPathName());
    R->SetStringField(TEXT("name"), Name);
    R->SetStringField(TEXT("control_type"), RigControlTypeToString(ControlType));
    R->SetStringField(TEXT("animation_type"), RigControlAnimationTypeToString(AnimationType));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (bDryRun)
    {
        R->SetBoolField(TEXT("changed"), true);
        R->SetStringField(TEXT("would_add"), Name);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("AddControlRigControl", "Sage: Add Control Rig Control"));
    RigBP->UBlueprint::Modify();
    Hierarchy->Modify();
    const FRigElementKey AddedKey = Controller->AddControl(
        FName(*Name),
        ParentKey,
        Settings,
        Value,
        OffsetTransform,
        ShapeTransform,
        /*bSetupUndo=*/true,
        /*bPrintPythonCommand=*/false);
    if (!AddedKey.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("URigHierarchyController::AddControl failed"));
    }
    RigBP->MarkPackageDirty();
    CompileControlRigIfRequested(RigBP, bCompile, R);
    R->SetBoolField(TEXT("changed"), true);
    R->SetObjectField(TEXT("control"), RigControlToJson(Hierarchy, Hierarchy->Find<FRigControlElement>(AddedKey), true));
    TrySaveLoadedAssetIfRequested(RigBP, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ControlRigRemoveControlImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("control"), Name) && !Args->TryGetStringField(TEXT("name"), Name))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'control'"));
    }
    UControlRigBlueprint* RigBP = ResolveControlRigBlueprintAsset(Path);
    if (!RigBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UControlRigBlueprint"));
    URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(RigBP);
    URigHierarchyController* Controller = UControlRigBlueprintEditorLibrary::GetHierarchyController(RigBP);
    if (!Hierarchy || !Controller) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("ControlRig hierarchy/controller unavailable"));

    const FRigElementKey Key(FName(*Name), ERigElementType::Control);
    FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key);
    if (!Control)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("control '%s' not found"), *Name));
    }
    bool bDryRun = false;
    bool bSave = false;
    bool bCompile = true;
    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), RigBP->GetPathName());
    R->SetStringField(TEXT("control"), Name);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetObjectField(TEXT("before"), RigControlToJson(Hierarchy, Control, true));
    if (bDryRun)
    {
        R->SetBoolField(TEXT("changed"), true);
        R->SetStringField(TEXT("would_remove"), Name);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("controlrig.remove_control is destructive; pass confirmed:true to proceed"));
    }

    FScopedTransaction Tx(LOCTEXT("RemoveControlRigControl", "Sage: Remove Control Rig Control"));
    RigBP->UBlueprint::Modify();
    Hierarchy->Modify();
    const bool bRemoved = Controller->RemoveElement(Key, /*bSetupUndo=*/true, /*bPrintPythonCommand=*/false);
    RigBP->MarkPackageDirty();
    CompileControlRigIfRequested(RigBP, bCompile, R);
    R->SetBoolField(TEXT("removed"), bRemoved);
    R->SetBoolField(TEXT("changed"), bRemoved);
    R->SetObjectField(TEXT("after_summary"), ControlRigSummaryToJson(RigBP, false, false, false));
    TrySaveLoadedAssetIfRequested(RigBP, bSave && bRemoved, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ListControlRigVariablesImpl(const TSharedPtr<FJsonObject>& Args)
{
    return ControlRigReadImpl(Args);
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
// animation.find_animations
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome FindAnimationsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path = TEXT("/Game");
    FString Query;
    FString ClassFilter;
    int32 MaxResults = 200;
    bool bIncludeSkeleton = true;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), Path);
        Args->TryGetStringField(TEXT("folder"), Path);
        Args->TryGetStringField(TEXT("query"), Query);
        Args->TryGetStringField(TEXT("class"), ClassFilter);
        Args->TryGetStringField(TEXT("type"), ClassFilter);
        double MaxD = MaxResults;
        if (Args->TryGetNumberField(TEXT("max_results"), MaxD))
        {
            MaxResults = FMath::Max(1, static_cast<int32>(MaxD));
        }
        Args->TryGetBoolField(TEXT("include_skeleton"), bIncludeSkeleton);
    }

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Path));
    Filter.bRecursivePaths = true;
    const FString ClassKey = NormalizeToken(ClassFilter);
    if (ClassKey.IsEmpty() || ClassKey == TEXT("all"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimSequence")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimMontage")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.BlendSpace")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.BlendSpace1D")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimBlueprint")));
    }
    else if (ClassKey == TEXT("sequence") || ClassKey == TEXT("animsequence"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimSequence")));
    }
    else if (ClassKey == TEXT("montage") || ClassKey == TEXT("animmontage"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimMontage")));
    }
    else if (ClassKey == TEXT("blendspace"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.BlendSpace")));
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.BlendSpace1D")));
    }
    else if (ClassKey == TEXT("animblueprint") || ClassKey == TEXT("abp"))
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimBlueprint")));
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unsupported class/type filter: %s"), *ClassFilter));
    }

    TArray<FAssetData> Found;
    GetAssetRegistry().GetAssets(Filter, Found);

    TArray<TSharedPtr<FJsonValue>> Items;
    int32 MatchedTotal = 0;
    for (const FAssetData& AssetData : Found)
    {
        if (!Query.IsEmpty() &&
            !AssetData.AssetName.ToString().Contains(Query) &&
            !AssetData.GetObjectPathString().Contains(Query))
        {
            continue;
        }
        ++MatchedTotal;
        if (Items.Num() >= MaxResults)
        {
            continue;
        }
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
        Obj->SetStringField(TEXT("path"), AssetData.GetSoftObjectPath().ToString());
        Obj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
        if (bIncludeSkeleton)
        {
            if (FAssetTagValueRef SkeletonTag = AssetData.TagsAndValues.FindTag(TEXT("Skeleton")); SkeletonTag.IsSet())
            {
                Obj->SetStringField(TEXT("skeleton_tag"), SkeletonTag.GetValue());
            }
        }
        Items.Add(MakeShared<FJsonValueObject>(Obj));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("query"), Query);
    R->SetStringField(TEXT("class_filter"), ClassFilter);
    R->SetArrayField(TEXT("animations"), Items);
    R->SetNumberField(TEXT("count"), Items.Num());
    R->SetNumberField(TEXT("matched_total"), MatchedTotal);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.inspect_animation
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome InspectAnimationImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimationAsset* Asset = Cast<UAnimationAsset>(ResolveAsset(Path));
    if (!Asset)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimationAsset: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Asset->GetPathName());
    R->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
    R->SetStringField(TEXT("skeleton"), Asset->GetSkeleton() ? Asset->GetSkeleton()->GetPathName() : FString());
    if (UAnimSequence* Seq = Cast<UAnimSequence>(Asset))
    {
        const IAnimationDataModel* Model = Seq->GetDataModel();
        R->SetNumberField(TEXT("play_length"), Seq->GetPlayLength());
        R->SetNumberField(TEXT("rate_scale"), Seq->RateScale);
        R->SetBoolField(TEXT("root_motion_enabled"), Seq->bEnableRootMotion);
        R->SetBoolField(TEXT("additive"), Seq->IsValidAdditive());
        if (Model)
        {
            R->SetNumberField(TEXT("duration"), Model->GetPlayLength());
            R->SetNumberField(TEXT("sample_rate"), Model->GetFrameRate().AsDecimal());
            R->SetNumberField(TEXT("number_of_frames"), Model->GetNumberOfFrames());
            R->SetNumberField(TEXT("number_of_keys"), Model->GetNumberOfKeys());
            int32 FloatCurves = 0, TransformCurves = 0, Attributes = 0;
            GetAnimCurveCounts(Seq, FloatCurves, TransformCurves, Attributes);
            R->SetNumberField(TEXT("float_curve_count"), FloatCurves);
            R->SetNumberField(TEXT("transform_curve_count"), TransformCurves);
            R->SetNumberField(TEXT("attribute_count"), Attributes);
            TArray<FName> TrackNames;
            Model->GetBoneTrackNames(TrackNames);
            R->SetArrayField(TEXT("track_names"), NamesToJsonArray(TrackNames));
            R->SetNumberField(TEXT("track_count"), TrackNames.Num());
        }
        AddRootMotionSummary(R, Seq);
    }
    else if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
    {
        R->SetNumberField(TEXT("play_length"), Montage->GetPlayLength());
        R->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
        R->SetNumberField(TEXT("slot_track_count"), Montage->SlotAnimTracks.Num());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.sample_bone_tracks
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SampleBoneTracksImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }
    const IAnimationDataModel* Model = Seq->GetDataModel();
    if (!Model)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("sequence has no animation data model"));
    }

    TSet<FName> BoneFilter = ReadBoneNameFilter(Args);
    if (BoneFilter.Num() == 0)
    {
        FString RootBone = Seq->GetSkeleton() && Seq->GetSkeleton()->GetReferenceSkeleton().GetNum() > 0
            ? Seq->GetSkeleton()->GetReferenceSkeleton().GetBoneName(0).ToString()
            : FString(TEXT("root"));
        BoneFilter.Add(FName(*RootBone));
    }

    TArray<int32> Frames;
    const TArray<TSharedPtr<FJsonValue>>* FrameValues = nullptr;
    if (Args->TryGetArrayField(TEXT("frames"), FrameValues) && FrameValues)
    {
        for (const TSharedPtr<FJsonValue>& Value : *FrameValues)
        {
            if (Value.IsValid() && Value->Type == EJson::Number)
            {
                Frames.Add(FMath::RoundToInt(Value->AsNumber()));
            }
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* TimeValues = nullptr;
    if (Args->TryGetArrayField(TEXT("times"), TimeValues) && TimeValues)
    {
        const double Rate = Model->GetFrameRate().AsDecimal();
        for (const TSharedPtr<FJsonValue>& Value : *TimeValues)
        {
            if (Value.IsValid() && Value->Type == EJson::Number)
            {
                Frames.Add(Rate > 0.0 ? FMath::RoundToInt(Value->AsNumber() * Rate) : 0);
            }
        }
    }
    if (Frames.Num() == 0)
    {
        int32 MaxSamples = 20;
        double MaxD = MaxSamples;
        if (Args->TryGetNumberField(TEXT("max_samples"), MaxD))
        {
            MaxSamples = FMath::Max(1, static_cast<int32>(MaxD));
        }
        const int32 LastFrame = FMath::Max(0, Model->GetNumberOfFrames());
        const int32 Step = FMath::Max(1, LastFrame / MaxSamples);
        for (int32 Frame = 0; Frame <= LastFrame && Frames.Num() < MaxSamples; Frame += Step)
        {
            Frames.Add(Frame);
        }
    }

    TArray<TSharedPtr<FJsonValue>> BoneResults;
    const double Rate = Model->GetFrameRate().AsDecimal();
    for (const FName& BoneName : BoneFilter)
    {
        auto BoneObj = MakeShared<FJsonObject>();
        BoneObj->SetStringField(TEXT("bone"), BoneName.ToString());
        const bool bHasTrack = Model->IsValidBoneTrackName(BoneName);
        BoneObj->SetBoolField(TEXT("has_track"), bHasTrack);
        TArray<TSharedPtr<FJsonValue>> Samples;
        if (bHasTrack)
        {
            for (int32 Frame : Frames)
            {
                const int32 ClampedFrame = FMath::Clamp(Frame, 0, Model->GetNumberOfFrames());
                const FTransform T = Model->GetBoneTrackTransform(BoneName, FFrameNumber(ClampedFrame));
                auto Sample = TransformToJsonObject(T);
                Sample->SetNumberField(TEXT("frame"), ClampedFrame);
                Sample->SetNumberField(TEXT("time"), Rate > 0.0 ? static_cast<double>(ClampedFrame) / Rate : 0.0);
                Samples.Add(MakeShared<FJsonValueObject>(Sample));
            }
        }
        BoneObj->SetArrayField(TEXT("samples"), Samples);
        BoneObj->SetNumberField(TEXT("sample_count"), Samples.Num());
        BoneResults.Add(MakeShared<FJsonValueObject>(BoneObj));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetArrayField(TEXT("bones"), BoneResults);
    R->SetNumberField(TEXT("bone_count"), BoneResults.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.compare_retarget_bones
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CompareRetargetBonesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SourcePath, TargetPath;
    if (!Args.IsValid() ||
        !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("source"), TEXT("source_path"), TEXT("source_skeleton") }, SourcePath) ||
        !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("target"), TEXT("target_path"), TEXT("target_skeleton") }, TargetPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'source' or 'target'"));
    }

    UObject* SourceAsset = nullptr;
    UObject* TargetAsset = nullptr;
    USkeleton* SourceSkeleton = nullptr;
    USkeleton* TargetSkeleton = nullptr;
    USkeletalMesh* SourceMesh = nullptr;
    USkeletalMesh* TargetMesh = nullptr;
    FString Error;
    const FReferenceSkeleton* SourceRef = ResolveReferenceSkeletonLike(SourcePath, SourceAsset, SourceSkeleton, SourceMesh, Error);
    if (!SourceRef)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }
    const FReferenceSkeleton* TargetRef = ResolveReferenceSkeletonLike(TargetPath, TargetAsset, TargetSkeleton, TargetMesh, Error);
    if (!TargetRef)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    TSet<FName> BoneFilter = ReadBoneNameFilter(Args);
    if (BoneFilter.Num() == 0)
    {
        for (int32 Index = 0; Index < SourceRef->GetNum(); ++Index)
        {
            BoneFilter.Add(SourceRef->GetBoneName(Index));
        }
        for (int32 Index = 0; Index < TargetRef->GetNum(); ++Index)
        {
            BoneFilter.Add(TargetRef->GetBoneName(Index));
        }
    }

    TArray<FTransform> SourceGlobal = BuildGlobalRefPose(*SourceRef);
    TArray<FTransform> TargetGlobal = BuildGlobalRefPose(*TargetRef);
    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 MissingSource = 0;
    int32 MissingTarget = 0;
    int32 Common = 0;
    for (const FName& BoneName : BoneFilter)
    {
        const int32 SourceIndex = SourceRef->FindBoneIndex(BoneName);
        const int32 TargetIndex = TargetRef->FindBoneIndex(BoneName);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("bone"), BoneName.ToString());
        Row->SetNumberField(TEXT("source_index"), SourceIndex);
        Row->SetNumberField(TEXT("target_index"), TargetIndex);
        Row->SetBoolField(TEXT("source_found"), SourceIndex != INDEX_NONE);
        Row->SetBoolField(TEXT("target_found"), TargetIndex != INDEX_NONE);
        if (SourceIndex == INDEX_NONE) ++MissingSource;
        if (TargetIndex == INDEX_NONE) ++MissingTarget;
        if (SourceIndex != INDEX_NONE && TargetIndex != INDEX_NONE)
        {
            ++Common;
            const FVector Delta = TargetGlobal[TargetIndex].GetTranslation() - SourceGlobal[SourceIndex].GetTranslation();
            Row->SetField(TEXT("ref_pose_translation_delta"), detail::Vec3ToJson(Delta));
            Row->SetNumberField(TEXT("ref_pose_translation_distance"), Delta.Size());
            Row->SetNumberField(TEXT("ref_pose_rotation_angle_delta_degrees"),
                FMath::RadiansToDegrees(SourceGlobal[SourceIndex].GetRotation().AngularDistance(TargetGlobal[TargetIndex].GetRotation())));
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("source"), SourceAsset->GetPathName());
    R->SetStringField(TEXT("target"), TargetAsset->GetPathName());
    R->SetArrayField(TEXT("bones"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetNumberField(TEXT("common_count"), Common);
    R->SetNumberField(TEXT("missing_source_count"), MissingSource);
    R->SetNumberField(TEXT("missing_target_count"), MissingTarget);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.copy_bone_tracks
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CopyBoneTracksImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SourcePath, TargetPath;
    if (!Args.IsValid() ||
        !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("source"), TEXT("source_anim") }, SourcePath) ||
        !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("target"), TEXT("target_anim"), TEXT("path") }, TargetPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing source/target animation"));
    }
    UAnimSequence* SourceSeq = Cast<UAnimSequence>(ResolveAsset(SourcePath));
    UAnimSequence* TargetSeq = Cast<UAnimSequence>(ResolveAsset(TargetPath));
    if (!SourceSeq || !TargetSeq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("source and target must be UAnimSequence assets"));
    }
    const IAnimationDataModel* SourceModel = SourceSeq->GetDataModel();
    if (!SourceModel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("source sequence has no animation data model"));
    }

    TSet<FName> BoneFilter = ReadBoneNameFilter(Args);
    if (BoneFilter.Num() == 0)
    {
        TArray<FName> TrackNames;
        SourceModel->GetBoneTrackNames(TrackNames);
        for (const FName& Name : TrackNames)
        {
            BoneFilter.Add(Name);
        }
    }

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Copied = 0;
    int32 Failed = 0;
    IAnimationDataController* TargetController = bDryRun ? nullptr : &TargetSeq->GetController();
    TUniquePtr<IAnimationDataController::FScopedBracket> Bracket;
    if (TargetController)
    {
        TargetSeq->Modify();
        Bracket = MakeUnique<IAnimationDataController::FScopedBracket>(
            TargetController,
            LOCTEXT("SageCopyBoneTracks", "Sage: Copy Bone Tracks"));
    }
    for (const FName& BoneName : BoneFilter)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("bone"), BoneName.ToString());
        const bool bSourceTrackFound = SourceModel->IsValidBoneTrackName(BoneName);
        const bool bTargetHasBone = TargetSeq->GetSkeleton() &&
            TargetSeq->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(BoneName) != INDEX_NONE;
        Row->SetBoolField(TEXT("source_track_found"), bSourceTrackFound);
        Row->SetBoolField(TEXT("target_bone_found"), bTargetHasBone);
        if (!bSourceTrackFound || !bTargetHasBone)
        {
            Row->SetStringField(TEXT("status"), TEXT("skipped"));
            ++Failed;
        }
        else if (bDryRun)
        {
            Row->SetStringField(TEXT("status"), TEXT("dry_run"));
        }
        else
        {
            if (!TargetSeq->GetDataModel() || !TargetSeq->GetDataModel()->IsValidBoneTrackName(BoneName))
            {
                TargetController->AddBoneCurve(BoneName, false);
            }
            TArray<FTransform> SourceTransforms;
            SourceModel->GetBoneTrackTransforms(BoneName, SourceTransforms);
            TArray<FVector> Positions;
            TArray<FQuat> Rotations;
            TArray<FVector> Scales;
            Positions.Reserve(SourceTransforms.Num());
            Rotations.Reserve(SourceTransforms.Num());
            Scales.Reserve(SourceTransforms.Num());
            for (const FTransform& Transform : SourceTransforms)
            {
                Positions.Add(Transform.GetTranslation());
                Rotations.Add(Transform.GetRotation());
                Scales.Add(Transform.GetScale3D());
            }
            const bool bOk = TargetController->SetBoneTrackKeys(
                BoneName,
                Positions,
                Rotations,
                Scales,
                false);
            Row->SetStringField(TEXT("status"), bOk ? TEXT("copied") : TEXT("failed"));
            Row->SetNumberField(TEXT("pos_key_count"), Positions.Num());
            Row->SetNumberField(TEXT("rot_key_count"), Rotations.Num());
            Row->SetNumberField(TEXT("scale_key_count"), Scales.Num());
            bOk ? ++Copied : ++Failed;
        }
        Results.Add(MakeShared<FJsonValueObject>(Row));
    }

    if (!bDryRun)
    {
        TargetSeq->RefreshCacheData();
        TargetSeq->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("source"), SourceSeq->GetPathName());
    R->SetStringField(TEXT("target"), TargetSeq->GetPathName());
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("copied"), Copied);
    R->SetNumberField(TEXT("failed"), Failed);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("modified"), !bDryRun && Copied > 0);
    TrySaveLoadedAssetIfRequested(TargetSeq, bSave && !bDryRun && Copied > 0, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.diagnose_retarget_animation
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome DiagnoseRetargetAnimationImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }
    const IAnimationDataModel* Model = Seq->GetDataModel();
    if (!Model)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("sequence has no animation data model"));
    }

    double PopThreshold = 50.0;
    Args->TryGetNumberField(TEXT("pop_threshold"), PopThreshold);
    TSet<FName> BoneFilter = ReadBoneNameFilter(Args);
    if (BoneFilter.Num() == 0 && Seq->GetSkeleton() && Seq->GetSkeleton()->GetReferenceSkeleton().GetNum() > 0)
    {
        BoneFilter.Add(Seq->GetSkeleton()->GetReferenceSkeleton().GetBoneName(0));
    }

    TArray<TSharedPtr<FJsonValue>> Issues;
    TArray<TSharedPtr<FJsonValue>> BoneReports;
    for (const FName& BoneName : BoneFilter)
    {
        auto BoneReport = MakeShared<FJsonObject>();
        BoneReport->SetStringField(TEXT("bone"), BoneName.ToString());
        const bool bHasTrack = Model->IsValidBoneTrackName(BoneName);
        BoneReport->SetBoolField(TEXT("has_track"), bHasTrack);
        if (!bHasTrack)
        {
            auto Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("type"), TEXT("missing_track"));
            Issue->SetStringField(TEXT("bone"), BoneName.ToString());
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
            BoneReports.Add(MakeShared<FJsonValueObject>(BoneReport));
            continue;
        }

        int32 PopCount = 0;
        int32 FlipCount = 0;
        FTransform Prev = Model->GetBoneTrackTransform(BoneName, FFrameNumber(0));
        for (int32 Frame = 1; Frame <= Model->GetNumberOfFrames(); ++Frame)
        {
            const FTransform Current = Model->GetBoneTrackTransform(BoneName, FFrameNumber(Frame));
            if ((Current.GetTranslation() - Prev.GetTranslation()).Size() > PopThreshold)
            {
                ++PopCount;
            }
            if ((Current.GetRotation() | Prev.GetRotation()) < 0.0)
            {
                ++FlipCount;
            }
            Prev = Current;
        }
        BoneReport->SetNumberField(TEXT("position_pop_count"), PopCount);
        BoneReport->SetNumberField(TEXT("quaternion_flip_count"), FlipCount);
        if (PopCount > 0)
        {
            auto Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("type"), TEXT("position_pop"));
            Issue->SetStringField(TEXT("bone"), BoneName.ToString());
            Issue->SetNumberField(TEXT("count"), PopCount);
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
        }
        if (FlipCount > 0)
        {
            auto Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("type"), TEXT("quaternion_flip"));
            Issue->SetStringField(TEXT("bone"), BoneName.ToString());
            Issue->SetNumberField(TEXT("count"), FlipCount);
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
        }
        BoneReports.Add(MakeShared<FJsonValueObject>(BoneReport));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("skeleton"), Seq->GetSkeleton() ? Seq->GetSkeleton()->GetPathName() : FString());
    R->SetNumberField(TEXT("duration"), Model->GetPlayLength());
    R->SetNumberField(TEXT("sample_rate"), Model->GetFrameRate().AsDecimal());
    R->SetNumberField(TEXT("number_of_frames"), Model->GetNumberOfFrames());
    R->SetBoolField(TEXT("root_motion_enabled"), Seq->bEnableRootMotion);
    AddRootMotionSummary(R, Seq);
    R->SetArrayField(TEXT("bone_reports"), BoneReports);
    R->SetArrayField(TEXT("issues"), Issues);
    R->SetNumberField(TEXT("issue_count"), Issues.Num());
    R->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.save_animation_asset
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SaveAnimationAssetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset || !Asset->IsA<UAnimationAsset>())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimationAsset: %s"), *Path));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Asset->GetPathName());
    TrySaveLoadedAssetIfRequested(Asset, true, R);
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
    if (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
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
    int32 Dimensions = 2;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    // Schema declares `dimensions: integer (1|2)`. Tolerate legacy `type: "1D"|"2D"`
    // for clients still passing the old key (warn-free).
    if (!Args->TryGetNumberField(TEXT("dimensions"), Dimensions))
    {
        FString LegacyType;
        if (Args->TryGetStringField(TEXT("type"), LegacyType))
        {
            if (LegacyType == TEXT("1D")) Dimensions = 1;
            else if (LegacyType == TEXT("2D")) Dimensions = 2;
        }
    }
    if (Dimensions != 1 && Dimensions != 2)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("'dimensions' must be 1 or 2"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("skeleton not found: %s"), *SkeletonPath));
    }

    FScopedTransaction Tx(LOCTEXT("CreateBS", "Create BlendSpace"));

    UObject* Created = nullptr;
    if (Dimensions == 1)
    {
        UClass* BS1DFacClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlendSpaceFactory1D"));
        if (!BS1DFacClass) BS1DFacClass = LoadObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlendSpaceFactory1D"));
        if (!BS1DFacClass)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                TEXT("BlendSpaceFactory1D class not found (UnrealEd module not loaded)"));
        }
        UFactory* Fac = NewObject<UFactory>(GetTransientPackage(), BS1DFacClass);
        FObjectPropertyBase* SkelProp = CastField<FObjectPropertyBase>(
            Fac->GetClass()->FindPropertyByName(TEXT("TargetSkeleton")));
        if (SkelProp) SkelProp->SetObjectPropertyValue(SkelProp->ContainerPtrToValuePtr<void>(Fac), Skel);
        Created = CreateAssetFromPath(Path, UBlendSpace1D::StaticClass(), Fac);
    }
    else
    {
        UClass* BSFacClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlendSpaceFactoryNew"));
        if (!BSFacClass) BSFacClass = LoadObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlendSpaceFactoryNew"));
        if (!BSFacClass)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                TEXT("BlendSpaceFactoryNew class not found (UnrealEd module not loaded)"));
        }
        UFactory* Fac = NewObject<UFactory>(GetTransientPackage(), BSFacClass);
        FObjectPropertyBase* SkelProp = CastField<FObjectPropertyBase>(
            Fac->GetClass()->FindPropertyByName(TEXT("TargetSkeleton")));
        if (SkelProp) SkelProp->SetObjectPropertyValue(SkelProp->ContainerPtrToValuePtr<void>(Fac), Skel);
        Created = CreateAssetFromPath(Path, UBlendSpace::StaticClass(), Fac);
    }

    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create BlendSpace at %s"), *Path));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Created->GetPathName());
    R->SetStringField(TEXT("class"),      Created->GetClass()->GetName());
    R->SetNumberField(TEXT("dimensions"), Dimensions);
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
    // Trim engine prefix/suffix so the displayed notify name matches Persona
    // ("AnimNotify_Foo_C" → "Foo"). UE convention: the runtime gameplay tag
    // matches `Notify.<TrimmedName>` so leaving prefixes corrupts gameplay
    // event lookups.
    {
        FString CleanName = Cls->GetName();
        CleanName.RemoveFromStart(TEXT("AnimNotify_"));
        CleanName.RemoveFromEnd(TEXT("_C"));
        NewEvent.NotifyName = FName(*CleanName);
    }
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
    if (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
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
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, BoneName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("bone"), BoneName) || BoneName.IsEmpty())
    {
        Args->TryGetStringField(TEXT("bone_name"), BoneName);
    }
    if (BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone'"));
    }
    const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
    if (!Args->TryGetArrayField(TEXT("keyframes"), KeyValues) || !KeyValues || KeyValues->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing non-empty 'keyframes' array"));
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

    const FName BoneF(*BoneName);
    const int32 BoneIdx = Skel->GetReferenceSkeleton().FindBoneIndex(BoneF);
    if (BoneIdx == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("bone '%s' not found in skeleton"), *BoneName));
    }

    TArray<FVector> Positions;
    TArray<FQuat> Rotations;
    TArray<FVector> Scales;
    Positions.Reserve(KeyValues->Num());
    Rotations.Reserve(KeyValues->Num());
    Scales.Reserve(KeyValues->Num());

    for (int32 I = 0; I < KeyValues->Num(); ++I)
    {
        const TSharedPtr<FJsonValue>& V = (*KeyValues)[I];
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!V.IsValid() || !V->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("keyframes[%d] must be an object"), I));
        }

        FTransform T = FTransform::Identity;
        ReadAnimationTransform(*Obj, T);
        Positions.Add(T.GetTranslation());
        Rotations.Add(T.GetRotation());
        Scales.Add(T.GetScale3D());
    }

    IAnimationDataController& Controller = Seq->GetController();
    {
        IAnimationDataController::FScopedBracket Bracket(&Controller,
            LOCTEXT("SageSetBoneKeyframes", "Sage: Set Bone Keyframes"));
        Seq->Modify();
        if (!Seq->GetDataModel() || !Seq->GetDataModel()->IsValidBoneTrackName(BoneF))
        {
            Controller.AddBoneCurve(BoneF, false);
        }
        if (!Controller.SetBoneTrackKeys(BoneF, Positions, Rotations, Scales, false))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                FString::Printf(TEXT("SetBoneTrackKeys failed for bone '%s'"), *BoneName));
        }
    }

    Seq->RefreshCacheData();
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("bone"), BoneName);
    R->SetNumberField(TEXT("bone_index"), BoneIdx);
    R->SetNumberField(TEXT("written_key_count"), Positions.Num());
    R->SetBoolField(TEXT("modified"), true);
    AddAnimSequenceTrackReadback(R, Seq, BoneName, 0, Positions.Num() - 1);
    AddRootMotionSummary(R, Seq);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.get_bone_transforms
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome GetBoneTransformsImpl(const TSharedPtr<FJsonObject>& Args)
{
    // Schema (phase4_schemas.cpp:253) declares { skeleton, bones[] } — handler
    // was reading the legacy { path, bone_name } shape from a pre-Phase-4
    // iteration which silently returned -32602 for every modern call.
    FString SkeletonPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("skeleton"), SkeletonPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("bones"), BonesArr) || !BonesArr || BonesArr->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bones' array"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *SkeletonPath));
    }

    const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
    const TArray<FTransform>& RefPose = RefSkel.GetRefBonePose();

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const TSharedPtr<FJsonValue>& V : *BonesArr)
    {
        if (!V.IsValid() || V->Type != EJson::String) continue;
        const FString BoneName = V->AsString();
        const int32 BoneIdx = RefSkel.FindBoneIndex(FName(*BoneName));
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("bone"), BoneName);
        if (BoneIdx == INDEX_NONE || !RefPose.IsValidIndex(BoneIdx))
        {
            Obj->SetBoolField(TEXT("found"), false);
        }
        else
        {
            const FTransform& T = RefPose[BoneIdx];
            Obj->SetBoolField(TEXT("found"),    true);
            Obj->SetField(TEXT("location"), detail::Vec3ToJson(T.GetLocation()));
            Obj->SetField(TEXT("rotation"), detail::Rot3ToJson(T.GetRotation().Rotator()));
            Obj->SetField(TEXT("scale"),    detail::Vec3ToJson(T.GetScale3D()));
        }
        Out.Add(MakeShared<FJsonValueObject>(Obj));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("skeleton"), Skel->GetPathName());
    R->SetArrayField (TEXT("bones"),    Out);
    R->SetNumberField(TEXT("count"),    Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_montage_sequence
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetMontageSequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SequencePath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("sequence"), SequencePath) || SequencePath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("sequence_path"), SequencePath);
    }
    if (SequencePath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'sequence'"));
    }

    UAnimMontage* Montage = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!Montage)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimMontage: %s"), *Path));
    }
    UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(ResolveAsset(SequencePath));
    if (!Sequence || Sequence->IsA<UAnimMontage>())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("sequence not a supported UAnimSequenceBase: %s"), *SequencePath));
    }
    if (!Sequence->CanBeUsedInComposition())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("sequence cannot be used in animation compositions: %s"), *SequencePath));
    }

    int32 SlotIndex = 0;
    double SlotIndexNumber = 0.0;
    if (Args->TryGetNumberField(TEXT("slot_index"), SlotIndexNumber))
    {
        SlotIndex = static_cast<int32>(SlotIndexNumber);
    }

    double StartTime = 0.0;
    double EndTime = Sequence->GetPlayLength();
    double PlayRate = 1.0;
    double LoopingCountNumber = 1.0;
    Args->TryGetNumberField(TEXT("start_time"), StartTime);
    Args->TryGetNumberField(TEXT("anim_start_time"), StartTime);
    Args->TryGetNumberField(TEXT("end_time"), EndTime);
    Args->TryGetNumberField(TEXT("anim_end_time"), EndTime);
    Args->TryGetNumberField(TEXT("play_rate"), PlayRate);
    Args->TryGetNumberField(TEXT("looping_count"), LoopingCountNumber);
    const int32 LoopingCount = FMath::Max(1, static_cast<int32>(LoopingCountNumber));
    const float ClampedStart = FMath::Clamp(static_cast<float>(StartTime), 0.0f, Sequence->GetPlayLength());
    const float ClampedEnd = FMath::Clamp(static_cast<float>(EndTime), ClampedStart, Sequence->GetPlayLength());

    FScopedTransaction Tx(LOCTEXT("SageSetMontageSequence", "Sage: Set Montage Sequence"));
    Montage->Modify();
    if (Montage->SlotAnimTracks.Num() == 0)
    {
        Montage->AddSlot(FAnimSlotGroup::DefaultSlotName);
    }
    if (!Montage->SlotAnimTracks.IsValidIndex(SlotIndex))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("slot_index %d out of range (slots=%d)"),
                            SlotIndex, Montage->SlotAnimTracks.Num()));
    }

    FSlotAnimationTrack& SlotTrack = Montage->SlotAnimTracks[SlotIndex];
    SlotTrack.AnimTrack.AnimSegments.Reset();
    FAnimSegment Segment;
    Segment.SetAnimReference(Sequence, true);
    Segment.StartPos = 0.0f;
    Segment.AnimStartTime = ClampedStart;
    Segment.AnimEndTime = ClampedEnd;
    Segment.AnimPlayRate = FMath::IsNearlyZero(static_cast<float>(PlayRate)) ? 1.0f : static_cast<float>(PlayRate);
    Segment.LoopingCount = LoopingCount;
    SlotTrack.AnimTrack.AnimSegments.Add(Segment);

    if (Montage->CompositeSections.Num() == 0)
    {
        FCompositeSection Section;
        Section.SectionName = TEXT("Default");
        Section.SetTime(0.0f);
        Montage->CompositeSections.Add(Section);
    }
    Montage->RefreshCacheData();
    Montage->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Montage->GetPathName());
    R->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    R->SetNumberField(TEXT("slot_index"), SlotIndex);
    R->SetStringField(TEXT("slot"), SlotTrack.SlotName.ToString());
    R->SetNumberField(TEXT("start_time"), ClampedStart);
    R->SetNumberField(TEXT("end_time"), ClampedEnd);
    R->SetNumberField(TEXT("play_rate"), Segment.AnimPlayRate);
    R->SetNumberField(TEXT("looping_count"), Segment.LoopingCount);
    R->SetNumberField(TEXT("duration"), Montage->GetPlayLength());
    R->SetBoolField(TEXT("modified"), true);
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
    if (Args->TryGetNumberField(TEXT("blend_in"), BlendIn) && BlendIn >= 0.0)
    {
        Montage->BlendIn.SetBlendTime(static_cast<float>(BlendIn));
    }
    double BlendOut = -1.0;
    if (Args->TryGetNumberField(TEXT("blend_out"), BlendOut) && BlendOut >= 0.0)
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
// animation.create_state_machine (Lyra Sage Gap #16 — was stub)
// ---------------------------------------------------------------------------
//
// Pipeline (engine canonical):
//   1. Locate AnimBP's AnimGraph (UAnimationGraphSchema-bound function graph)
//   2. Spawn UAnimGraphNode_StateMachine inside AnimGraph
//   3. CreateNewGraph(UAnimationStateMachineGraph + UAnimationStateMachineSchema)
//      — schema's CreateDefaultNodesForGraph produces the Entry node
//   4. Bind sub-graph to the SM node via EditorStateMachineGraph
//   5. Wire SM node's Output Pose to AnimGraph Root (Output Pose pin)
//   6. MarkBlueprintAsStructurallyModified + (optional) compile

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
    bool bConnectToRoot = true;
    Args->TryGetBoolField(TEXT("connect_to_root"), bConnectToRoot);
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }

    UEdGraph* AnimGraph = FindAnimGraph(AnimBP);
    if (!AnimGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("AnimBlueprint has no AnimGraph (UAnimationGraphSchema)"));
    }

    // Reject duplicate name
    if (FindStateMachineGraph(AnimBP, FName(*Name)) != nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state machine '%s' already exists on %s"),
                            *Name, *AnimBP->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("CreateSM", "Sage: Create State Machine"));
    AnimBP->Modify();
    AnimGraph->Modify();

    // 1) Spawn UAnimGraphNode_StateMachine. PostPlacedNewNode is the engine
    // canonical entry point — it allocates EditorStateMachineGraph (with the
    // correct outer = SM node), sets OwnerAnimGraphNode, registers the
    // sub-graph in ParentGraph->SubGraphs, and runs the schema's
    // CreateDefaultNodesForGraph (Entry node). We must call it BEFORE
    // AllocateDefaultPins (PostPlacedNewNode also runs AllocateDefaultPins
    // implicitly on UEdGraphNode and the sub-graph schema setup expects pins
    // to be uninitialized when called).
    UAnimGraphNode_StateMachine* SMNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
    SMNode->CreateNewGuid();
    SMNode->NodePosX = 0;
    SMNode->NodePosY = 0;
    AnimGraph->AddNode(SMNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    SMNode->PostPlacedNewNode();
    SMNode->AllocateDefaultPins();

    // 2) Rename the engine-created sub-graph to the user-supplied name.
    UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
    if (!SMGraph)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("PostPlacedNewNode did not produce EditorStateMachineGraph"));
    }
    SMGraph->bAllowDeletion = false;
    if (SMGraph->GetFName() != FName(*Name))
    {
        // RenameGraphWithSuggestion + a node-aware FNameValidatorFactory
        // validator is the engine canonical for SM rename — guarantees the
        // chosen name is unique and stable inside the AnimBP namespace
        // (mirrors UAnimGraphNode_StateMachineBase::PostPlacedNewNode line
        // 153). Plain RenameGraph skips the validator pass and can collide
        // silently with an existing graph name.
        TSharedPtr<INameValidatorInterface> NameValidator =
            FNameValidatorFactory::MakeValidator(SMNode);
        FBlueprintEditorUtils::RenameGraphWithSuggestion(SMGraph, NameValidator, Name);
    }

    // 3) Wire to AnimGraph Output Pose root (optional)
    bool bConnected = false;
    if (bConnectToRoot)
    {
        UEdGraphNode* Root = FindAnimGraphOutput(AnimGraph);
        if (Root)
        {
            UEdGraphPin* SMOut   = FindFirstOutputPosePin(SMNode);
            UEdGraphPin* RootIn  = FindFirstInputPosePin(Root);
            if (SMOut && RootIn)
            {
                // Disconnect anything currently feeding the root pose
                RootIn->BreakAllPinLinks();
                SMOut->MakeLinkTo(RootIn);
                bConnected = true;
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(AnimBP);
        bCompiled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),                AnimBP->GetPathName());
    R->SetStringField(TEXT("state_machine_name"),  Name);
    R->SetStringField(TEXT("state_machine_node_id"),
                      SMNode->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetBoolField  (TEXT("connected_to_root"),   bConnected);
    R->SetBoolField  (TEXT("compiled"),            bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_state (Lyra Sage Gap #16 — was stub)
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, StateName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName) || SMName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'state_machine_name'"));
    }
    if (!Args->TryGetStringField(TEXT("state_name"), StateName) || StateName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_name'"));
    }
    double X = 0.0, Y = 0.0;
    Args->TryGetNumberField(TEXT("x"), X);
    Args->TryGetNumberField(TEXT("y"), Y);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state machine '%s' not found on %s"), *SMName, *AnimBP->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("AddState", "Sage: Add State"));
    AnimBP->Modify();
    SMGraph->Modify();

    UAnimStateNode* StateNode = NewObject<UAnimStateNode>(SMGraph);
    StateNode->CreateNewGuid();
    StateNode->NodePosX = static_cast<int32>(X);
    StateNode->NodePosY = static_cast<int32>(Y);
    SMGraph->AddNode(StateNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    // PostPlacedNewNode is what creates StateNode->BoundGraph (UAnimationStateGraph
    // with the StateResult output node). Without this, BoundGraph is null and
    // the state can't drive any pose. Engine: AnimStateNode.cpp:131-156.
    StateNode->PostPlacedNewNode();
    StateNode->AllocateDefaultPins();

    if (StateNode->BoundGraph && StateNode->BoundGraph->GetFName() != FName(*StateName))
    {
        // Use the validator-aware rename so colliding state names get an
        // automatic suffix instead of silently overwriting (engine canonical;
        // mirrors UAnimStateNode::PostPlacedNewNode).
        TSharedPtr<INameValidatorInterface> NameValidator =
            FNameValidatorFactory::MakeValidator(StateNode);
        FBlueprintEditorUtils::RenameGraphWithSuggestion(StateNode->BoundGraph,
                                                         NameValidator, StateName);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("state_machine_name"), SMName);
    R->SetStringField(TEXT("state_name"),         StateName);
    R->SetStringField(TEXT("state_id"),
                      StateNode->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetNumberField(TEXT("x"), X);
    R->SetNumberField(TEXT("y"), Y);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_transition (Lyra Sage Gap #16 — was stub)
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddTransitionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, FromId, ToId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName) || SMName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'state_machine_name'"));
    }
    if (!Args->TryGetStringField(TEXT("from_state_id"), FromId) || FromId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'from_state_id'"));
    }
    if (!Args->TryGetStringField(TEXT("to_state_id"), ToId) || ToId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'to_state_id'"));
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state machine '%s' not found"), *SMName));
    }

    UAnimStateNodeBase* FromNode = FindStateNodeByGuid(SMGraph, FromId);
    UAnimStateNodeBase* ToNode   = FindStateNodeByGuid(SMGraph, ToId);
    if (!FromNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("from_state_id not found: %s"), *FromId));
    }
    if (!ToNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("to_state_id not found: %s"), *ToId));
    }

    FScopedTransaction Tx(LOCTEXT("AddTransition", "Sage: Add Transition"));
    AnimBP->Modify();
    SMGraph->Modify();

    UAnimStateTransitionNode* T = NewObject<UAnimStateTransitionNode>(SMGraph);
    T->CreateNewGuid();
    T->NodePosX = (FromNode->NodePosX + ToNode->NodePosX) / 2;
    T->NodePosY = (FromNode->NodePosY + ToNode->NodePosY) / 2;
    SMGraph->AddNode(T, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    // PostPlacedNewNode creates the transition's BoundGraph (UAnimationTransitionGraph
    // — holds the rule expression). Without it the transition compiles as
    // default-true with no editable canvas. Engine: AnimStateTransitionNode.cpp:109.
    T->PostPlacedNewNode();
    T->AllocateDefaultPins();

    UEdGraphPin* FromOut = FindFirstOutputPosePin(FromNode);
    UEdGraphPin* TInPin  = FindFirstInputPosePin(T);
    UEdGraphPin* TOutPin = FindFirstOutputPosePin(T);
    UEdGraphPin* ToIn    = FindFirstInputPosePin(ToNode);
    if (!FromOut || !TInPin || !TOutPin || !ToIn)
    {
        Tx.Cancel();
        // SM-context pose pins use PinCategory == "Transition" — IsPosePin
        // already accepts that. Surface which exact pin was missing so engine
        // API drift gets diagnosed at first call instead of a runtime crash.
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("transition pin lookup failed — expected category Transition pin not found (from_out=%d, t_in=%d, t_out=%d, to_in=%d)"),
                            FromOut ? 1 : 0, TInPin ? 1 : 0, TOutPin ? 1 : 0, ToIn ? 1 : 0));
    }
    FromOut->MakeLinkTo(TInPin);
    TOutPin->MakeLinkTo(ToIn);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("transition_id"),  T->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("from_state_id"),  FromId);
    R->SetStringField(TEXT("to_state_id"),    ToId);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_state_animation (Lyra Sage Gap #16 — was stub)
// ---------------------------------------------------------------------------
//
// Each UAnimStateNode has a sub-graph (BoundGraph) that drives its output pose.
// This handler clears the sub-graph's player nodes (sequence/blendspace) and
// installs a fresh UAnimGraphNode_SequencePlayer driving the supplied animation,
// wired to the BoundGraph's Output Pose root.

FSageToolDispatch::FOutcome SetStateAnimationImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, StateId, AnimPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName) || SMName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'state_machine_name'"));
    }
    if (!Args->TryGetStringField(TEXT("state_id"), StateId) || StateId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_id'"));
    }
    if (!Args->TryGetStringField(TEXT("animation"), AnimPath) || AnimPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'animation'"));
    }
    bool bLoop = true;
    Args->TryGetBoolField(TEXT("loop"), bLoop);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state machine '%s' not found"), *SMName));
    }
    UAnimStateNode* State = Cast<UAnimStateNode>(FindStateNodeByGuid(SMGraph, StateId));
    if (!State || !State->BoundGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state_id %s not a UAnimStateNode with BoundGraph"), *StateId));
    }
    UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(ResolveAsset(AnimPath));
    if (!Anim)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("animation not a UAnimSequenceBase: %s"), *AnimPath));
    }

    FScopedTransaction Tx(LOCTEXT("SetStateAnim", "Sage: Set State Animation"));
    AnimBP->Modify();
    UEdGraph* StateGraph = State->BoundGraph;
    StateGraph->Modify();

    // Clean any pre-existing asset-player nodes so the call is idempotent.
    // UAnimGraphNode_AssetPlayerBase covers SequencePlayer, BlendSpacePlayer,
    // RandomPlayer, and other asset-driven players (engine canonical "this
    // state plays one asset" base).
    TArray<UEdGraphNode*> ToRemove;
    for (UEdGraphNode* N : StateGraph->Nodes)
    {
        if (N && N->IsA<UAnimGraphNode_AssetPlayerBase>())
        {
            ToRemove.Add(N);
        }
    }
    for (UEdGraphNode* N : ToRemove)
    {
        FBlueprintEditorUtils::RemoveNode(AnimBP, N, /*bDontRecompile=*/true);
    }

    // Spawn new SequencePlayer
    UAnimGraphNode_SequencePlayer* SeqPlayer = NewObject<UAnimGraphNode_SequencePlayer>(StateGraph);
    SeqPlayer->CreateNewGuid();
    SeqPlayer->NodePosX = -300;
    SeqPlayer->NodePosY = 0;
    StateGraph->AddNode(SeqPlayer, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    SeqPlayer->AllocateDefaultPins();

    // Drive the FAnimNode_SequencePlayer struct directly. SetAnimationAsset
    // takes UAnimSequenceBase — both UAnimSequence and UAnimComposite resolve
    // through the same overload, so the historical type-split is dead code.
    SeqPlayer->SetAnimationAsset(Anim);
    // TODO(loop): UE 5.7 made FAnimNode_SequencePlayer::bLoopAnimation +
    // PlayRate protected; direct write fails. Reach via reflection (Node
    // FStructProperty) or wait for an engine accessor. Leave loop-as-default
    // (true) for now; users can override via animation.set_anim_node_property
    // with property="bLoopAnimation".
    (void)bLoop;

    // Connect SeqPlayer.Pose → state output (UAnimGraphNode_StateResult inside
    // a UAnimationStateGraph, NOT UAnimGraphNode_Root).
    UEdGraphNode* Root = FindAnimGraphOutput(StateGraph);
    bool bConnected = false;
    if (Root)
    {
        UEdGraphPin* PoseOut = FindFirstOutputPosePin(SeqPlayer);
        UEdGraphPin* RootIn  = FindFirstInputPosePin(Root);
        if (PoseOut && RootIn)
        {
            RootIn->BreakAllPinLinks();
            PoseOut->MakeLinkTo(RootIn);
            bConnected = true;
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("state_id"),       StateId);
    R->SetStringField(TEXT("animation"),      Anim->GetPathName());
    R->SetBoolField  (TEXT("connected"),      bConnected);
    R->SetStringField(TEXT("player_node_id"),
                      SeqPlayer->NodeGuid.ToString(EGuidFormats::Digits));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_transition_blend (Lyra Sage Gap #16 — was stub)
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetTransitionBlendImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, TransitionId;
    double BlendTime = 0.2;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName) || SMName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'state_machine_name'"));
    }
    if (!Args->TryGetStringField(TEXT("transition_id"), TransitionId) || TransitionId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'transition_id'"));
    }
    Args->TryGetNumberField(TEXT("blend_time"), BlendTime);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state machine '%s' not found"), *SMName));
    }
    UAnimStateTransitionNode* T = FindTransitionByGuid(SMGraph, TransitionId);
    if (!T)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("transition_id not found: %s"), *TransitionId));
    }

    FScopedTransaction Tx(LOCTEXT("SetTransitionBlend", "Sage: Set Transition Blend"));
    AnimBP->Modify();
    T->Modify();
    T->CrossfadeDuration = static_cast<float>(BlendTime);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("transition_id"), TransitionId);
    R->SetNumberField(TEXT("blend_time"),    BlendTime);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_transition_automatic_rule (Lyra Sage Gap #29)
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetTransitionAutomaticRuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, TransitionId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName) || SMName.IsEmpty())
    {
        Args->TryGetStringField(TEXT("graph_name"), SMName);
    }
    if (SMName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'state_machine_name'"));
    }
    if (!Args->TryGetStringField(TEXT("transition_id"), TransitionId) || TransitionId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'transition_id'"));
    }

    bool bAutomatic = true;
    Args->TryGetBoolField(TEXT("automatic_rule"), bAutomatic);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state machine '%s' not found"), *SMName));
    }
    UAnimStateTransitionNode* T = FindTransitionByGuid(SMGraph, TransitionId);
    if (!T)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("transition_id not found: %s"), *TransitionId));
    }

    double TriggerTime = T->AutomaticRuleTriggerTime;
    const bool bHasTriggerTime =
        Args->TryGetNumberField(TEXT("automatic_rule_trigger_time"), TriggerTime)
        || Args->TryGetNumberField(TEXT("auto_trigger_time"), TriggerTime);

    double BlendTime = T->CrossfadeDuration;
    const bool bHasBlendTime =
        Args->TryGetNumberField(TEXT("blend_time"), BlendTime)
        || Args->TryGetNumberField(TEXT("auto_blend_in_time"), BlendTime);

    FScopedTransaction Tx(LOCTEXT("SetTransitionAutomaticRule", "Sage: Set Transition Automatic Rule"));
    AnimBP->Modify();
    T->Modify();
    T->bAutomaticRuleBasedOnSequencePlayerInState = bAutomatic;
    if (bHasTriggerTime)
    {
        T->AutomaticRuleTriggerTime = static_cast<float>(TriggerTime);
    }
    if (bHasBlendTime)
    {
        T->CrossfadeDuration = static_cast<float>(FMath::Max(0.0, BlendTime));
    }

    FPropertyChangedEvent ChangeEvent(
        FindFProperty<FProperty>(UAnimStateTransitionNode::StaticClass(),
            GET_MEMBER_NAME_CHECKED(UAnimStateTransitionNode, bAutomaticRuleBasedOnSequencePlayerInState)),
        EPropertyChangeType::ValueSet);
    T->PostEditChangeProperty(ChangeEvent);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("transition_id"), TransitionId);
    R->SetBoolField(TEXT("automatic_rule"), T->bAutomaticRuleBasedOnSequencePlayerInState);
    R->SetNumberField(TEXT("automatic_rule_trigger_time"), T->AutomaticRuleTriggerTime);
    R->SetNumberField(TEXT("blend_time"), T->CrossfadeDuration);
    R->SetBoolField(TEXT("set"), true);
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

    // UE 5.5+ canonical path: IAnimationDataController on the sequence handles
    // curve table mutation through transactional brackets.
    FScopedTransaction Tx(LOCTEXT("AddCurve", "Sage: Add Anim Curve"));
    Seq->Modify();
    IAnimationDataController& Controller = Seq->GetController();
    const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
    Controller.OpenBracket(LOCTEXT("AddCurveBracket", "Add Curve"), /*bShouldTransact=*/false);
    const bool bAdded = Controller.AddCurve(CurveId, AACF_Editable);
    Controller.CloseBracket(/*bShouldTransact=*/false);
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Seq->GetPathName());
    R->SetStringField(TEXT("curve_name"), CurveName);
    R->SetBoolField  (TEXT("added"),      bAdded);
    if (!bAdded)
    {
        R->SetStringField(TEXT("note"),
            TEXT("AddCurve returned false (already exists or controller rejected)"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.create_anim_notify (Lyra Sage Gap #18 — new tool)
// ---------------------------------------------------------------------------
//
// Create a UAnimNotify subclass Blueprint asset. The BP starts blank — caller
// can wire `Received_Notify` event in the BP graph via existing Sage BP graph
// tools (bp.add_event_node, bp.add_function_call, etc.).

FSageToolDispatch::FOutcome CreateAnimNotifyClassImpl(
    const TSharedPtr<FJsonObject>& Args, UClass* DefaultParent)
{
    FString FullPath, ParentClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), FullPath) || FullPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    Args->TryGetStringField(TEXT("parent_class"), ParentClassPath);

    UClass* ParentCls = nullptr;
    if (!ParentClassPath.IsEmpty())
    {
        ParentCls = FindObject<UClass>(nullptr, *ParentClassPath);
        if (!ParentCls) ParentCls = LoadObject<UClass>(nullptr, *ParentClassPath);
        if (!ParentCls)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("parent_class not found: %s"), *ParentClassPath));
        }
        if (!ParentCls->IsChildOf(DefaultParent))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("parent_class %s isn't a subclass of %s"),
                                *ParentCls->GetName(), *DefaultParent->GetName()));
        }
    }
    else
    {
        ParentCls = DefaultParent;
    }

    FString PackagePath, AssetName;
    if (!FullPath.Split(TEXT("/"), &PackagePath, &AssetName,
                        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid asset path: %s"), *FullPath));
    }

    FScopedTransaction Tx(LOCTEXT("CreateNotifyBP", "Sage: Create Anim Notify Blueprint"));
    UBlueprintFactory* Fac = NewObject<UBlueprintFactory>();
    Fac->ParentClass = ParentCls;

    UObject* Created = GetAssetTools().CreateAsset(
        AssetName, PackagePath, UBlueprint::StaticClass(), Fac);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create Blueprint at %s"), *FullPath));
    }
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         Created->GetPathName());
    R->SetStringField(TEXT("parent_class"), ParentCls->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CreateAnimNotifyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    return CreateAnimNotifyClassImpl(Args, UAnimNotify::StaticClass());
}

FSageToolDispatch::FOutcome CreateAnimNotifyStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    return CreateAnimNotifyClassImpl(Args, UAnimNotifyState::StaticClass());
}

// ---------------------------------------------------------------------------
// animation.add_blendspace_sample (Lyra Sage Gap #17 — new tool)
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddBlendSpaceSampleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, AnimPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("animation"), AnimPath) || AnimPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'animation'"));
    }

    double X = 0.0, Y = 0.0;
    bool bHaveX = Args->TryGetNumberField(TEXT("x"), X);
    Args->TryGetNumberField(TEXT("y"), Y);
    bool bHavePos = false;
    {
        const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
        if (Args->TryGetArrayField(TEXT("position"), PosArr) && PosArr && PosArr->Num() >= 1)
        {
            bHavePos = true;
            // Apply position only when scalar `x` was not given. When both
            // are provided, `x`/`y` win (caller-explicit beats generic). We
            // surface a `_warning` below.
            if (!bHaveX)
            {
                X = (*PosArr)[0]->AsNumber();
                if (PosArr->Num() >= 2) Y = (*PosArr)[1]->AsNumber();
            }
        }
    }

    UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(Path));
    if (!BS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UBlendSpace: %s"), *Path));
    }
    UAnimSequence* Anim = Cast<UAnimSequence>(ResolveAsset(AnimPath));
    if (!Anim)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("animation not a UAnimSequence: %s"), *AnimPath));
    }

    // 1D blendspaces ignore the Y axis at runtime; force-clamp it so the
    // echo back to the caller matches the stored sample (Cluster E audit —
    // clients reported "y=3 went in but readback shows 0" confusion).
    const bool bIs1D = (Cast<UBlendSpace1D>(BS) != nullptr);
    if (bIs1D) Y = 0.0;

    FScopedTransaction Tx(LOCTEXT("AddBSSample", "Sage: Add BlendSpace Sample"));
    BS->Modify();

    const FVector SamplePos(static_cast<float>(X), static_cast<float>(Y), 0.f);
    const int32 SampleIdx = BS->AddSample(Anim, SamplePos);
    const bool bAdded = (SampleIdx != INDEX_NONE);
    BS->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         BS->GetPathName());
    R->SetStringField(TEXT("animation"),    Anim->GetPathName());
    R->SetNumberField(TEXT("x"),            X);
    R->SetNumberField(TEXT("y"),            Y);
    R->SetBoolField  (TEXT("added"),        bAdded);
    R->SetNumberField(TEXT("sample_count"), BS->GetBlendSamples().Num());
    if (bHaveX && bHavePos)
    {
        R->SetStringField(TEXT("_warning"),
            TEXT("both 'x' and 'position' provided; 'x' wins"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_blendspace_samples (Lyra Sage Gap #17 — bulk replace)
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetBlendSpaceSamplesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    const TArray<TSharedPtr<FJsonValue>>* SamplesArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("samples"), SamplesArr) || !SamplesArr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'samples' array"));
    }

    UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(Path));
    if (!BS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UBlendSpace: %s"), *Path));
    }

    FScopedTransaction Tx(LOCTEXT("SetBSSamples", "Sage: Replace BlendSpace Samples"));
    BS->Modify();

    // Wipe existing samples by removing in reverse index order. UE 5.7
    // UBlendSpaceBase exposes DeleteSample(int32) returning bool.
    for (int32 Idx = BS->GetBlendSamples().Num() - 1; Idx >= 0; --Idx)
    {
        BS->DeleteSample(Idx);
    }

    int32 Added = 0;
    TArray<TSharedPtr<FJsonValue>> Skipped;
    int32 InputIdx = 0;
    for (const TSharedPtr<FJsonValue>& Val : *SamplesArr)
    {
        const int32 ThisIdx = InputIdx++;
        if (!Val.IsValid() || Val->Type != EJson::Object)
        {
            auto SkObj = MakeShared<FJsonObject>();
            SkObj->SetNumberField(TEXT("index"),  ThisIdx);
            SkObj->SetStringField(TEXT("reason"), TEXT("not_object"));
            Skipped.Add(MakeShared<FJsonValueObject>(SkObj));
            continue;
        }
        const TSharedPtr<FJsonObject> Obj = Val->AsObject();
        FString AnimPath;
        if (!Obj->TryGetStringField(TEXT("animation"), AnimPath) || AnimPath.IsEmpty())
        {
            // Structured skip — string-only entries lost the index context
            // and made it impossible to correlate failures with the input
            // array (lessons.md "silent fail anti-pattern" follow-up).
            auto SkObj = MakeShared<FJsonObject>();
            SkObj->SetNumberField(TEXT("index"),  ThisIdx);
            SkObj->SetStringField(TEXT("reason"), TEXT("missing"));
            Skipped.Add(MakeShared<FJsonValueObject>(SkObj));
            continue;
        }
        UAnimSequence* Anim = Cast<UAnimSequence>(ResolveAsset(AnimPath));
        if (!Anim)
        {
            auto SkObj = MakeShared<FJsonObject>();
            SkObj->SetNumberField(TEXT("index"),     ThisIdx);
            SkObj->SetStringField(TEXT("reason"),    TEXT("unresolved"));
            SkObj->SetStringField(TEXT("animation"), AnimPath);
            Skipped.Add(MakeShared<FJsonValueObject>(SkObj));
            continue;
        }

        double X = 0.0, Y = 0.0;
        if (!Obj->TryGetNumberField(TEXT("x"), X))
        {
            const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
            if (Obj->TryGetArrayField(TEXT("position"), PosArr) && PosArr
                && PosArr->Num() >= 1)
            {
                X = (*PosArr)[0]->AsNumber();
                if (PosArr->Num() >= 2) Y = (*PosArr)[1]->AsNumber();
            }
        }
        else
        {
            Obj->TryGetNumberField(TEXT("y"), Y);
        }

        const int32 NewIdx = BS->AddSample(
            Anim, FVector(static_cast<float>(X), static_cast<float>(Y), 0.f));
        if (NewIdx != INDEX_NONE)
        {
            ++Added;
        }
    }

    BS->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         BS->GetPathName());
    R->SetNumberField(TEXT("sample_count"), BS->GetBlendSamples().Num());
    R->SetNumberField(TEXT("added"),        Added);
    if (Skipped.Num() > 0)
    {
        R->SetArrayField(TEXT("skipped"), Skipped);
    }
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
    if (!Args->TryGetStringField(TEXT("slot"), SlotName) || SlotName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'slot'"));
    }
    int32 SlotIndex = 0;
    Args->TryGetNumberField(TEXT("slot_index"), SlotIndex);

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
    if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("'slot_index' %d out of range [0, %d)"),
                            SlotIndex, Montage->SlotAnimTracks.Num()));
    }

    FScopedTransaction Tx(LOCTEXT("SetMontageSlot", "Set Montage Slot"));
    Montage->Modify();
    Montage->SlotAnimTracks[SlotIndex].SlotName = FName(*SlotName);
    Montage->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Montage->GetPathName());
    R->SetStringField(TEXT("slot"),       SlotName);
    R->SetNumberField(TEXT("slot_index"), SlotIndex);
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
    // Persona-side timeline only sees a new section after RefreshCacheData()
    // rebuilds the marker tracks (UAnimMontage::RefreshCacheData override
    // handles section + branch-point cache).
    Montage->RefreshCacheData();
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
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FString SkMeshPath;
    TryGetAnyStringField(Args,
        TArray<const TCHAR*>{ TEXT("skeletal_mesh"), TEXT("skeletal_mesh_path"), TEXT("mesh"), TEXT("mesh_path") },
        SkMeshPath);
    FString LegacySkeletonPath;
    Args->TryGetStringField(TEXT("skeleton"), LegacySkeletonPath);

    USkeletalMesh* SK = nullptr;
    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (!SkMeshPath.IsEmpty())
    {
        SK = Cast<USkeletalMesh>(ResolveAsset(SkMeshPath));
        if (!SK)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a USkeletalMesh: %s"), *SkMeshPath));
        }
    }
    else if (!LegacySkeletonPath.IsEmpty())
    {
        SK = Cast<USkeletalMesh>(ResolveAsset(LegacySkeletonPath));
        if (SK)
        {
            SkMeshPath = LegacySkeletonPath;
            Warnings.Add(MakeShared<FJsonValueString>(
                TEXT("'skeleton' was treated as a legacy skeletal mesh alias; prefer 'skeletal_mesh'")));
        }
        else
        {
            Warnings.Add(MakeShared<FJsonValueString>(
                TEXT("'skeleton' is deprecated for create_ik_rig and cannot initialize an IK Rig without a skeletal mesh; prefer 'skeletal_mesh'")));
        }
    }

    FString RetargetRoot;
    Args->TryGetStringField(TEXT("retarget_root"), RetargetRoot);
    const FReferenceSkeleton* RefSkeleton = SK ? &SK->GetRefSkeleton() : nullptr;
    if (RefSkeleton && !RetargetRoot.IsEmpty() && RefSkeleton->FindBoneIndex(FName(*RetargetRoot)) == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("retarget_root bone not found on skeletal mesh: %s"), *RetargetRoot));
    }

    struct FPendingIKChain
    {
        FString Name;
        FString StartBone;
        FString EndBone;
        FString Goal;
    };
    TArray<FPendingIKChain> PendingChains;
    const TArray<TSharedPtr<FJsonValue>>* ChainValues = nullptr;
    if (Args->TryGetArrayField(TEXT("chains"), ChainValues) && ChainValues)
    {
        for (int32 ChainIndex = 0; ChainIndex < ChainValues->Num(); ++ChainIndex)
        {
            const TSharedPtr<FJsonValue>& ChainValue = (*ChainValues)[ChainIndex];
            const TSharedPtr<FJsonObject>* ChainObj = nullptr;
            if (!ChainValue.IsValid() || !ChainValue->TryGetObject(ChainObj) || !ChainObj || !(*ChainObj).IsValid())
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("chains[%d] must be an object"), ChainIndex));
            }

            FPendingIKChain Pending;
            if (!TryGetAnyStringField(*ChainObj, TArray<const TCHAR*>{ TEXT("name"), TEXT("chain_name") }, Pending.Name))
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("chains[%d] missing 'name'"), ChainIndex));
            }
            TryGetAnyStringField(*ChainObj, TArray<const TCHAR*>{ TEXT("start_bone"), TEXT("start") }, Pending.StartBone);
            TryGetAnyStringField(*ChainObj, TArray<const TCHAR*>{ TEXT("end_bone"), TEXT("end") }, Pending.EndBone);
            TryGetAnyStringField(*ChainObj, TArray<const TCHAR*>{ TEXT("goal"), TEXT("goal_name") }, Pending.Goal);
            if (RefSkeleton)
            {
                if (!Pending.StartBone.IsEmpty() && RefSkeleton->FindBoneIndex(FName(*Pending.StartBone)) == INDEX_NONE)
                {
                    return FSageToolDispatch::FOutcome::MakeError(-32602,
                        FString::Printf(TEXT("chains[%d].start_bone not found: %s"), ChainIndex, *Pending.StartBone));
                }
                if (!Pending.EndBone.IsEmpty() && RefSkeleton->FindBoneIndex(FName(*Pending.EndBone)) == INDEX_NONE)
                {
                    return FSageToolDispatch::FOutcome::MakeError(-32602,
                        FString::Printf(TEXT("chains[%d].end_bone not found: %s"), ChainIndex, *Pending.EndBone));
                }
            }
            PendingChains.Add(MoveTemp(Pending));
        }
    }

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Path);
        R->SetStringField(TEXT("skeletal_mesh"), SkMeshPath);
        R->SetStringField(TEXT("retarget_root"), RetargetRoot);
        R->SetNumberField(TEXT("chain_count"), PendingChains.Num());
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        R->SetArrayField(TEXT("warnings"), Warnings);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

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

    if (Fac && SK)
    {
        FObjectPropertyBase* MeshProp = CastField<FObjectPropertyBase>(
            Fac->GetClass()->FindPropertyByName(TEXT("SkeletalMesh")));
        if (MeshProp)
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

    UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Created);
    UIKRigController* Controller = Rig ? UIKRigController::GetController(Rig) : nullptr;
    if (Rig && Controller)
    {
        Rig->Modify();
        if (SK)
        {
            Controller->SetSkeletalMesh(SK);
        }
        if (!RetargetRoot.IsEmpty())
        {
            Controller->SetRetargetRoot(FName(*RetargetRoot));
        }
        for (const FPendingIKChain& Chain : PendingChains)
        {
            Controller->AddRetargetChain(
                FName(*Chain.Name),
                FName(*Chain.StartBone),
                FName(*Chain.EndBone),
                FName(*Chain.Goal));
        }
        Controller->SortRetargetChains();
        Controller->BroadcastNeedsReinitialized();
        Rig->MarkPackageDirty();
    }

    TSharedRef<FJsonObject> R = Rig ? IKRigToJson(Rig, Controller) : MakeShared<FJsonObject>();
    if (!Rig)
    {
        R->SetStringField(TEXT("path"), Created->GetPathName());
        R->SetStringField(TEXT("class"), Created->GetClass()->GetName());
    }
    R->SetBoolField(TEXT("created"), true);
    R->SetBoolField(TEXT("modified"), true);
    R->SetArrayField(TEXT("warnings"), Warnings);
    TrySaveLoadedAssetIfRequested(Created, bSave, R);
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

    UIKRigDefinition* Rig = Cast<UIKRigDefinition>(ResolveAsset(Path));
    if (!Rig)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("asset not found — ensure IK Rig plugin is enabled"));
    }

    UIKRigController* Controller = UIKRigController::GetController(Rig);
    TSharedRef<FJsonObject> R = IKRigToJson(Rig, Controller);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_ik_rig_skeletal_mesh
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetIKRigSkeletalMeshImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString MeshPath;
    if (!TryGetAnyStringField(Args,
        TArray<const TCHAR*>{ TEXT("skeletal_mesh"), TEXT("skeletal_mesh_path"), TEXT("mesh"), TEXT("mesh_path") },
        MeshPath) || MeshPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeletal_mesh'"));
    }

    USkeletalMesh* Mesh = Cast<USkeletalMesh>(ResolveAsset(MeshPath));
    if (!Mesh)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeletalMesh: %s"), *MeshPath));
    }

    bool bDryRun = false;
    bool bValidateOnly = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bValidateOnly);
    Args->TryGetBoolField(TEXT("save"), bSave);
    bDryRun = bDryRun || bValidateOnly;

    TSharedRef<FJsonObject> Before = IKRigToJson(Rig, Controller);
    USkeletalMesh* BeforeMesh = Controller->GetSkeletalMesh();
    const bool bWouldModify = BeforeMesh != Mesh;
    TSharedRef<FJsonObject> Compatibility = IKRigMeshCompatibilityToJson(Rig, Mesh);
    const bool bCompatible = Compatibility->GetBoolField(TEXT("compatible"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Rig->GetPathName());
    R->SetStringField(TEXT("skeletal_mesh"), Mesh->GetPathName());
    R->SetStringField(TEXT("before_skeletal_mesh"), AssetPathOrEmpty(BeforeMesh));
    R->SetBoolField(TEXT("compatible"), bCompatible);
    R->SetBoolField(TEXT("would_modify"), bWouldModify);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("validate_only"), bValidateOnly);
    R->SetBoolField(TEXT("modified"), false);
    R->SetObjectField(TEXT("compatibility"), Compatibility);
    R->SetObjectField(TEXT("before"), Before);

    if (!bCompatible)
    {
        R->SetBoolField(TEXT("rejected"), true);
        R->SetStringField(TEXT("reason"), TEXT("UIKRigController::IsSkeletalMeshCompatible returned false"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (bDryRun)
    {
        R->SetBoolField(TEXT("rejected"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    bool bSet = true;
    if (bWouldModify)
    {
        FScopedTransaction Tx(LOCTEXT("SageSetIKRigSkeletalMesh", "Sage: Set IK Rig Skeletal Mesh"));
        Rig->Modify();
        bSet = Controller->SetSkeletalMesh(Mesh);
        if (!bSet)
        {
            Tx.Cancel();
            R->SetBoolField(TEXT("set_result"), false);
            R->SetBoolField(TEXT("rejected"), true);
            R->SetStringField(TEXT("reason"), TEXT("UIKRigController::SetSkeletalMesh returned false"));
            R->SetObjectField(TEXT("after"), IKRigToJson(Rig, Controller));
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
        Controller->BroadcastNeedsReinitialized();
        Rig->MarkPackageDirty();
    }

    TSharedRef<FJsonObject> After = IKRigToJson(Rig, Controller);
    R->SetBoolField(TEXT("set_result"), bSet);
    R->SetBoolField(TEXT("rejected"), false);
    R->SetBoolField(TEXT("modified"), bWouldModify);
    R->SetObjectField(TEXT("after"), After);

    FString BeforeRoot;
    FString AfterRoot;
    Before->TryGetStringField(TEXT("retarget_root"), BeforeRoot);
    After->TryGetStringField(TEXT("retarget_root"), AfterRoot);
    R->SetBoolField(TEXT("retarget_root_preserved"), BeforeRoot == AfterRoot);

    double BeforeCount = 0.0;
    double AfterCount = 0.0;
    Before->TryGetNumberField(TEXT("chain_count"), BeforeCount);
    After->TryGetNumberField(TEXT("chain_count"), AfterCount);
    R->SetBoolField(TEXT("chain_count_preserved"), FMath::RoundToInt(BeforeCount) == FMath::RoundToInt(AfterCount));
    Before->TryGetNumberField(TEXT("goal_count"), BeforeCount);
    After->TryGetNumberField(TEXT("goal_count"), AfterCount);
    R->SetBoolField(TEXT("goal_count_preserved"), FMath::RoundToInt(BeforeCount) == FMath::RoundToInt(AfterCount));
    Before->TryGetNumberField(TEXT("solver_count"), BeforeCount);
    After->TryGetNumberField(TEXT("solver_count"), AfterCount);
    R->SetBoolField(TEXT("solver_count_preserved"), FMath::RoundToInt(BeforeCount) == FMath::RoundToInt(AfterCount));
    Before->TryGetNumberField(TEXT("goal_connection_count"), BeforeCount);
    After->TryGetNumberField(TEXT("goal_connection_count"), AfterCount);
    R->SetBoolField(TEXT("goal_connection_count_preserved"), FMath::RoundToInt(BeforeCount) == FMath::RoundToInt(AfterCount));

    TrySaveLoadedAssetIfRequested(Rig, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddIKRetargetChainImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString ChainName, StartBone, EndBone, GoalName;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("name"), TEXT("chain_name") }, ChainName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("start_bone"), TEXT("start") }, StartBone);
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("end_bone"), TEXT("end") }, EndBone);
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("goal"), TEXT("goal_name") }, GoalName);

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Rig->GetPathName());
        R->SetStringField(TEXT("name"), ChainName);
        R->SetStringField(TEXT("start_bone"), StartBone);
        R->SetStringField(TEXT("end_bone"), EndBone);
        R->SetStringField(TEXT("goal"), GoalName);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageAddIKRetargetChain", "Sage: Add IK Retarget Chain"));
    Rig->Modify();
    const FName AddedName = Controller->AddRetargetChain(FName(*ChainName), FName(*StartBone), FName(*EndBone), FName(*GoalName));
    Controller->SortRetargetChains();
    Controller->BroadcastNeedsReinitialized();
    Rig->MarkPackageDirty();

    TSharedRef<FJsonObject> R = IKRigToJson(Rig, Controller);
    R->SetStringField(TEXT("added_chain"), AddedName.ToString());
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Rig, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RemoveIKRetargetChainImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString ChainName;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("name"), TEXT("chain_name") }, ChainName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Rig->GetPathName());
        R->SetStringField(TEXT("name"), ChainName);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageRemoveIKRetargetChain", "Sage: Remove IK Retarget Chain"));
    Rig->Modify();
    const bool bRemoved = Controller->RemoveRetargetChain(FName(*ChainName));
    if (!bRemoved)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("retarget chain not found: %s"), *ChainName));
    }
    Controller->BroadcastNeedsReinitialized();
    Rig->MarkPackageDirty();

    TSharedRef<FJsonObject> R = IKRigToJson(Rig, Controller);
    R->SetStringField(TEXT("removed_chain"), ChainName);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Rig, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RenameIKRetargetChainImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString ChainName, NewName;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("name"), TEXT("chain_name") }, ChainName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("new_name"), TEXT("to") }, NewName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'new_name'"));
    }
    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);

    FScopedTransaction Tx(LOCTEXT("SageRenameIKRetargetChain", "Sage: Rename IK Retarget Chain"));
    Rig->Modify();
    const FName ActualName = Controller->RenameRetargetChain(FName(*ChainName), FName(*NewName));
    if (ActualName == FName(*ChainName))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("failed to rename retarget chain: %s"), *ChainName));
    }
    Rig->MarkPackageDirty();
    TSharedRef<FJsonObject> R = IKRigToJson(Rig, Controller);
    R->SetStringField(TEXT("old_name"), ChainName);
    R->SetStringField(TEXT("new_name"), ActualName.ToString());
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Rig, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetIKRetargetChainBonesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString ChainName, StartBone, EndBone;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("name"), TEXT("chain_name") }, ChainName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("start_bone"), TEXT("start") }, StartBone);
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("end_bone"), TEXT("end") }, EndBone);
    if (StartBone.IsEmpty() && EndBone.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'start_bone' or 'end_bone'"));
    }
    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargetChainBones", "Sage: Set IK Retarget Chain Bones"));
    Rig->Modify();
    bool bOk = true;
    if (!StartBone.IsEmpty())
    {
        bOk &= Controller->SetRetargetChainStartBone(FName(*ChainName), FName(*StartBone));
    }
    if (!EndBone.IsEmpty())
    {
        bOk &= Controller->SetRetargetChainEndBone(FName(*ChainName), FName(*EndBone));
    }
    if (!bOk)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("failed to update retarget chain bones: %s"), *ChainName));
    }
    Controller->SortRetargetChains();
    Controller->BroadcastNeedsReinitialized();
    Rig->MarkPackageDirty();

    TSharedRef<FJsonObject> R = IKRigToJson(Rig, Controller);
    R->SetStringField(TEXT("updated_chain"), ChainName);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Rig, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetIKRetargetChainGoalImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString ChainName, GoalName;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("name"), TEXT("chain_name") }, ChainName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("goal"), TEXT("goal_name") }, GoalName);
    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargetChainGoal", "Sage: Set IK Retarget Chain Goal"));
    Rig->Modify();
    if (!Controller->SetRetargetChainGoal(FName(*ChainName), FName(*GoalName)))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("failed to update retarget chain goal: %s"), *ChainName));
    }
    Controller->BroadcastNeedsReinitialized();
    Rig->MarkPackageDirty();

    TSharedRef<FJsonObject> R = IKRigToJson(Rig, Controller);
    R->SetStringField(TEXT("updated_chain"), ChainName);
    R->SetStringField(TEXT("goal"), Controller->GetRetargetChainGoal(FName(*ChainName)).ToString());
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Rig, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetIKRetargetRootImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString RootBone;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("root_bone"), TEXT("retarget_root"), TEXT("bone") }, RootBone))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'root_bone'"));
    }
    bool bSave = false;
    Args->TryGetBoolField(TEXT("save"), bSave);

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargetRoot", "Sage: Set IK Retarget Root"));
    Rig->Modify();
    if (!Controller->SetRetargetRoot(FName(*RootBone)))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("failed to set retarget root: %s"), *RootBone));
    }
    Controller->BroadcastNeedsReinitialized();
    Rig->MarkPackageDirty();

    TSharedRef<FJsonObject> R = IKRigToJson(Rig, Controller);
    R->SetStringField(TEXT("retarget_root"), Controller->GetRetargetRoot().ToString());
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Rig, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AutoGenerateIKRetargetDefinitionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FAutoCharacterizeResults Results;
    Controller->AutoGenerateRetargetDefinition(Results);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Rig->GetPathName());
    R->SetBoolField(TEXT("used_template"), Results.bUsedTemplate);
    R->SetStringField(TEXT("best_template"), Results.BestTemplateName.ToString());
    R->SetNumberField(TEXT("best_matching_bones"), Results.BestNumMatchingBones);
    R->SetNumberField(TEXT("best_template_score"), Results.BestPercentageOfTemplateScore);
    R->SetArrayField(TEXT("missing_bones"), NamesToJsonArray(Results.MissingBones));
    R->SetArrayField(TEXT("bones_with_missing_parent"), NamesToJsonArray(Results.BonesWithMissingParent));
    TArray<TSharedPtr<FJsonValue>> ExpandedChains;
    for (const TPair<FName, int32>& Pair : Results.ExpandedChains)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("chain"), Pair.Key.ToString());
        Item->SetNumberField(TEXT("expanded_by"), Pair.Value);
        ExpandedChains.Add(MakeShared<FJsonValueObject>(Item));
    }
    R->SetArrayField(TEXT("expanded_chains"), ExpandedChains);
    R->SetNumberField(TEXT("generated_chain_count"), Results.AutoRetargetDefinition.RetargetDefinition.BoneChains.Num());
    R->SetStringField(TEXT("generated_root"), Results.AutoRetargetDefinition.RetargetDefinition.RootBone.ToString());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (bDryRun)
    {
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageAutoGenerateIKRetargetDefinition", "Sage: Auto Generate IK Retarget Definition"));
    Rig->Modify();
    const bool bApplied = Controller->ApplyAutoGeneratedRetargetDefinition();
    if (!bApplied)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("ApplyAutoGeneratedRetargetDefinition failed"));
    }
    Controller->BroadcastNeedsReinitialized();
    Rig->MarkPackageDirty();

    R->SetBoolField(TEXT("applied"), bApplied);
    R->SetBoolField(TEXT("modified"), true);
    R->SetObjectField(TEXT("readback"), IKRigToJson(Rig, Controller));
    TrySaveLoadedAssetIfRequested(Rig, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FString AutoFBIKOutcomeToString(EAutoFBIKResult Outcome)
{
    switch (Outcome)
    {
    case EAutoFBIKResult::AllOk:               return TEXT("AllOk");
    case EAutoFBIKResult::MissingMesh:         return TEXT("MissingMesh");
    case EAutoFBIKResult::UnknownSkeletonType: return TEXT("UnknownSkeletonType");
    case EAutoFBIKResult::MissingChains:       return TEXT("MissingChains");
    case EAutoFBIKResult::MissingRootBone:     return TEXT("MissingRootBone");
    default:                                   return TEXT("Unknown");
    }
}

FSageToolDispatch::FOutcome AutoGenerateIKFBIKImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRigDefinition* Rig = nullptr;
    UIKRigController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRigController(Args, Rig, Controller);
    if (!Resolved.bSuccess) return Resolved;

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FAutoFBIKResults Results;
    Controller->AutoGenerateFBIK(Results);
    if (!bDryRun)
    {
        FScopedTransaction Tx(LOCTEXT("SageAutoGenerateIKFBIK", "Sage: Auto Generate IK FBIK"));
        Rig->Modify();
        if (!Controller->ApplyAutoFBIK())
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                FString::Printf(TEXT("ApplyAutoFBIK failed: %s"), *AutoFBIKOutcomeToString(Results.Outcome)));
        }
        Controller->BroadcastNeedsReinitialized();
        Rig->MarkPackageDirty();
    }

    TSharedRef<FJsonObject> R = IKRigToJson(Rig, Controller);
    R->SetStringField(TEXT("outcome"), AutoFBIKOutcomeToString(Results.Outcome));
    R->SetArrayField(TEXT("missing_chains"), NamesToJsonArray(Results.MissingChains));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("modified"), !bDryRun);
    TrySaveLoadedAssetIfRequested(Rig, bSave && !bDryRun, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_root_motion
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetRootMotionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }

    TArray<FString> Paths;
    FString Path;
    if (Args->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
    {
        Paths.Add(Path);
    }
    TArray<FString> ArrayPaths;
    ReadStringArrayField(Args, TEXT("assets"), ArrayPaths);
    for (const FString& Item : ArrayPaths)
    {
        if (!Item.IsEmpty())
        {
            Paths.AddUnique(Item);
        }
    }
    if (Paths.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path' or 'assets'"));
    }

    bool bEnabled = false;
    bool bHaveEnabled = Args->TryGetBoolField(TEXT("enabled"), bEnabled);
    bool bForceRootLock = false;
    bool bHaveForceRootLock = Args->TryGetBoolField(TEXT("force_root_lock"), bForceRootLock);
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FString LockTypeStr;
    if (!Args->TryGetStringField(TEXT("lock_type"), LockTypeStr))
    {
        Args->TryGetStringField(TEXT("root_lock"), LockTypeStr);
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Modified = 0;
    int32 Failed = 0;
    auto RootLockToString = [](ERootMotionRootLock::Type Mode) -> FString
    {
        switch (Mode)
        {
        case ERootMotionRootLock::RefPose:        return TEXT("RefPose");
        case ERootMotionRootLock::AnimFirstFrame: return TEXT("AnimFirstFrame");
        case ERootMotionRootLock::Zero:           return TEXT("Zero");
        default:                                  return TEXT("Unknown");
        }
    };
    FScopedTransaction Tx(LOCTEXT("SetRootMotion", "Set Root Motion"));
    for (const FString& SeqPath : Paths)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), SeqPath);
        UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(SeqPath));
        if (!Seq)
        {
            Row->SetStringField(TEXT("status"), TEXT("failed"));
            Row->SetStringField(TEXT("reason"), TEXT("not a UAnimSequence"));
            Results.Add(MakeShared<FJsonValueObject>(Row));
            ++Failed;
            continue;
        }
        Row->SetStringField(TEXT("resolved_path"), Seq->GetPathName());
        Row->SetBoolField(TEXT("before_enabled"), Seq->bEnableRootMotion);
        Row->SetBoolField(TEXT("before_force_root_lock"), Seq->bForceRootLock);
        Row->SetStringField(TEXT("before_lock_type"), RootLockToString(Seq->RootMotionRootLock));
        if (bDryRun)
        {
            Row->SetStringField(TEXT("status"), TEXT("dry_run"));
            Row->SetBoolField(TEXT("modified"), false);
            Results.Add(MakeShared<FJsonValueObject>(Row));
            continue;
        }

        Seq->Modify();
        if (bHaveEnabled)
        {
            Seq->bEnableRootMotion = bEnabled;
        }
        if (bHaveForceRootLock)
        {
            Seq->bForceRootLock = bForceRootLock;
        }
        if (!LockTypeStr.IsEmpty())
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
        ++Modified;

        Row->SetStringField(TEXT("status"), TEXT("updated"));
        Row->SetBoolField(TEXT("enabled"), Seq->bEnableRootMotion);
        Row->SetBoolField(TEXT("force_root_lock"), Seq->bForceRootLock);
        Row->SetStringField(TEXT("lock_type"), RootLockToString(Seq->RootMotionRootLock));
        Row->SetBoolField(TEXT("modified"), true);
        if (bSave)
        {
            TrySaveLoadedAssetIfRequested(Seq, true, Row);
        }
        Results.Add(MakeShared<FJsonValueObject>(Row));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("count"), Paths.Num());
    R->SetNumberField(TEXT("modified"), Modified);
    R->SetNumberField(TEXT("failed"), Failed);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("save"), bSave);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_virtual_bone
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddVirtualBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SkeletonPath, BoneName, SourceBone, TargetBone;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("skeleton"), SkeletonPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    if (!Args->TryGetStringField(TEXT("name"),        BoneName)   || BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    if (!Args->TryGetStringField(TEXT("source_bone"), SourceBone) || SourceBone.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'source_bone'"));
    }
    if (!Args->TryGetStringField(TEXT("target_bone"), TargetBone) || TargetBone.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'target_bone'"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *SkeletonPath));
    }

    const FName SourceF(*SourceBone);
    const FName TargetF(*TargetBone);
    FName VirtualF(*BoneName);
    if (!BoneName.StartsWith(TEXT("VB ")))
    {
        VirtualF = FName(*(TEXT("VB ") + BoneName));
    }

    const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
    if (RefSkel.FindBoneIndex(SourceF) == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("source_bone not found on skeleton: %s"), *SourceBone));
    }
    if (RefSkel.FindBoneIndex(TargetF) == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("target_bone not found on skeleton: %s"), *TargetBone));
    }

    for (const FVirtualBone& VB : Skel->GetVirtualBones())
    {
        const bool bSameName = (VB.VirtualBoneName == VirtualF);
        const bool bSamePair = (VB.SourceBoneName == SourceF && VB.TargetBoneName == TargetF);
        if (bSameName && bSamePair)
        {
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("path"),        Skel->GetPathName());
            R->SetStringField(TEXT("bone_name"),   VirtualF.ToString());
            R->SetStringField(TEXT("source_bone"), SourceBone);
            R->SetStringField(TEXT("target_bone"), TargetBone);
            R->SetBoolField  (TEXT("added"),       false);
            R->SetBoolField  (TEXT("already"),     true);
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
        if (bSameName)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("virtual bone name already exists: %s"), *VirtualF.ToString()));
        }
        if (bSamePair)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("virtual bone already exists for %s -> %s as %s"),
                    *SourceBone, *TargetBone, *VB.VirtualBoneName.ToString()));
        }
    }

    FScopedTransaction Tx(LOCTEXT("AddVirtualBone", "Add Virtual Bone"));
    Skel->Modify();
    if (!Skel->AddNewNamedVirtualBone(SourceF, TargetF, VirtualF))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("AddNewNamedVirtualBone failed: %s"), *VirtualF.ToString()));
    }
    Skel->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),        Skel->GetPathName());
    R->SetStringField(TEXT("bone_name"),   VirtualF.ToString());
    R->SetStringField(TEXT("source_bone"), SourceBone);
    R->SetStringField(TEXT("target_bone"), TargetBone);
    R->SetBoolField  (TEXT("added"),       true);
    R->SetBoolField  (TEXT("already"),     false);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.remove_virtual_bone
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome RemoveVirtualBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SkeletonPath, BoneName;
    if (!Args.IsValid()
        || (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath)
            && !Args->TryGetStringField(TEXT("path"), SkeletonPath)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    if (!Args->TryGetStringField(TEXT("bone_name"), BoneName) || BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone_name'"));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *SkeletonPath));
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
    if (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
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

bool ParseAutoMapChainType(const FString& Raw, EAutoMapChainType& Out);
void AddIKRetargeterReadback(UIKRetargeter* Retargeter, TSharedRef<FJsonObject> Result);
TSharedRef<FJsonObject> FKChainSettingsToJson(const FRetargetFKChainSettings& Settings);
TSharedRef<FJsonObject> IKChainSettingsToJson(const FRetargetIKChainSettings& Settings);

FSageToolDispatch::FOutcome CreateIKRetargeterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    bool bDryRun = false;
    bool bSave = false;
    bool bAddDefaultOps = true;
    bool bAssignOps = true;
    bool bCleanAsset = true;
    bool bAutoMap = true;
    bool bForceRemap = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
    Args->TryGetBoolField(TEXT("assign_ops"), bAssignOps);
    Args->TryGetBoolField(TEXT("assign_ik_rigs"), bAssignOps);
    Args->TryGetBoolField(TEXT("clean_asset"), bCleanAsset);
    Args->TryGetBoolField(TEXT("auto_map"), bAutoMap);
    Args->TryGetBoolField(TEXT("force_remap"), bForceRemap);
    FString AutoMapTypeString = TEXT("fuzzy");
    Args->TryGetStringField(TEXT("auto_map_type"), AutoMapTypeString);
    EAutoMapChainType AutoMapType = EAutoMapChainType::Fuzzy;
    if (!ParseAutoMapChainType(AutoMapTypeString, AutoMapType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid auto_map_type: %s"), *AutoMapTypeString));
    }

    FString SourcePath;
    FString TargetPath;
    Args->TryGetStringField(TEXT("source_ik_rig"), SourcePath);
    Args->TryGetStringField(TEXT("target_ik_rig"), TargetPath);
    UIKRigDefinition* SourceRig = nullptr;
    UIKRigDefinition* TargetRig = nullptr;
    if (!SourcePath.IsEmpty())
    {
        SourceRig = Cast<UIKRigDefinition>(ResolveAsset(SourcePath));
        if (!SourceRig)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a source UIKRigDefinition: %s"), *SourcePath));
        }
    }
    if (!TargetPath.IsEmpty())
    {
        TargetRig = Cast<UIKRigDefinition>(ResolveAsset(TargetPath));
        if (!TargetRig)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a target UIKRigDefinition: %s"), *TargetPath));
        }
    }

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Path);
        R->SetStringField(TEXT("source_ik_rig"), SourcePath);
        R->SetStringField(TEXT("target_ik_rig"), TargetPath);
        R->SetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
        R->SetBoolField(TEXT("assign_ops"), bAssignOps);
        R->SetBoolField(TEXT("clean_asset"), bCleanAsset);
        R->SetBoolField(TEXT("auto_map"), bAutoMap);
        R->SetStringField(TEXT("auto_map_type"), AutoMapTypeString);
        R->SetBoolField(TEXT("force_remap"), bForceRemap);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
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
        if (SourceRig)
        {
            FObjectPropertyBase* SrcProp = CastField<FObjectPropertyBase>(
                Fac->GetClass()->FindPropertyByName(TEXT("SourceIKRigAsset")));
            if (SrcProp)
            {
                SrcProp->SetObjectPropertyValue(SrcProp->ContainerPtrToValuePtr<void>(Fac), SourceRig);
            }
        }
        if (TargetRig)
        {
            FObjectPropertyBase* TgtProp = CastField<FObjectPropertyBase>(
                Fac->GetClass()->FindPropertyByName(TEXT("TargetIKRigAsset")));
            if (TgtProp)
            {
                TgtProp->SetObjectPropertyValue(TgtProp->ContainerPtrToValuePtr<void>(Fac), TargetRig);
            }
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
    if (UIKRetargeter* CreatedRetargeter = Cast<UIKRetargeter>(Created))
    {
        if (UIKRetargeterController* Controller = UIKRetargeterController::GetController(CreatedRetargeter))
        {
            CreatedRetargeter->Modify();
            if (SourceRig)
            {
                Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
            }
            if (TargetRig)
            {
                Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
            }
            if (bAddDefaultOps)
            {
                Controller->AddDefaultOps();
            }
            if (bAssignOps)
            {
                if (const UIKRigDefinition* ActiveSourceRig = Controller->GetIKRig(ERetargetSourceOrTarget::Source))
                {
                    Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, ActiveSourceRig);
                }
                if (const UIKRigDefinition* ActiveTargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target))
                {
                    Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, ActiveTargetRig);
                }
            }
            if (bAutoMap)
            {
                Controller->AutoMapChains(AutoMapType, bForceRemap);
            }
            if (bCleanAsset)
            {
                Controller->CleanAsset();
            }
            CreatedRetargeter->MarkPackageDirty();
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), Created->GetClass()->GetName());
    R->SetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
    R->SetBoolField(TEXT("assign_ops"), bAssignOps);
    R->SetBoolField(TEXT("clean_asset"), bCleanAsset);
    R->SetBoolField(TEXT("auto_map"), bAutoMap);
    R->SetStringField(TEXT("auto_map_type"), AutoMapTypeString);
    R->SetBoolField(TEXT("force_remap"), bForceRemap);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Created, bSave, R);
    if (UIKRetargeter* CreatedRetargeter = Cast<UIKRetargeter>(Created))
    {
        AddIKRetargeterReadback(CreatedRetargeter, R);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_ik_retargeter
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadIKRetargeterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(ResolveAsset(Path));
    if (!Retargeter)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UIKRetargeter: %s"), *Path));
    }

    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to get IK Retargeter controller"));
    }

    auto AssetPathOrEmpty = [](const UObject* Obj) -> FString
    {
        return Obj ? Obj->GetPathName() : FString();
    };
    auto RigToJson = [&AssetPathOrEmpty](const UIKRigDefinition* Rig) -> TSharedRef<FJsonObject>
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("path"), AssetPathOrEmpty(Rig));
        if (Rig)
        {
            Obj->SetStringField(TEXT("root_bone"), Rig->GetPelvis().ToString());
            TArray<TSharedPtr<FJsonValue>> Chains;
            for (const FBoneChain& Chain : Rig->GetRetargetChains())
            {
                auto ChainObj = MakeShared<FJsonObject>();
                ChainObj->SetStringField(TEXT("name"), Chain.ChainName.ToString());
                ChainObj->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
                ChainObj->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
                ChainObj->SetStringField(TEXT("ik_goal"), Chain.IKGoalName.ToString());
                Chains.Add(MakeShared<FJsonValueObject>(ChainObj));
            }
            Obj->SetArrayField(TEXT("chains"), Chains);
            Obj->SetNumberField(TEXT("chain_count"), Chains.Num());
        }
        return Obj;
    };
    auto PoseSideToJson = [Controller](ERetargetSourceOrTarget Side) -> TSharedRef<FJsonObject>
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("side"), RetargetSideToString(Side));
        Obj->SetStringField(TEXT("current_pose"), Controller->GetCurrentRetargetPoseName(Side).ToString());
        Obj->SetField(TEXT("root_offset"), detail::Vec3ToJson(Controller->GetRootOffsetInRetargetPose(Side)));
        TArray<TSharedPtr<FJsonValue>> Poses;
        for (const TPair<FName, FIKRetargetPose>& Pair : Controller->GetRetargetPoses(Side))
        {
            auto PoseObj = MakeShared<FJsonObject>();
            PoseObj->SetStringField(TEXT("name"), Pair.Key.ToString());
            PoseObj->SetField(TEXT("root_offset"), detail::Vec3ToJson(Pair.Value.GetRootTranslationDelta()));
            TArray<TSharedPtr<FJsonValue>> Rotations;
            for (const TPair<FName, FQuat>& RotationPair : Pair.Value.GetAllDeltaRotations())
            {
                auto RotObj = MakeShared<FJsonObject>();
                RotObj->SetStringField(TEXT("bone"), RotationPair.Key.ToString());
                RotObj->SetField(TEXT("rotation"), detail::Rot3ToJson(RotationPair.Value.Rotator()));
                Rotations.Add(MakeShared<FJsonValueObject>(RotObj));
            }
            PoseObj->SetArrayField(TEXT("bone_rotations"), Rotations);
            PoseObj->SetNumberField(TEXT("bone_rotation_count"), Rotations.Num());
            Poses.Add(MakeShared<FJsonValueObject>(PoseObj));
        }
        Obj->SetArrayField(TEXT("poses"), Poses);
        Obj->SetNumberField(TEXT("pose_count"), Poses.Num());
        return Obj;
    };

    TArray<TSharedPtr<FJsonValue>> Ops;
    TArray<TSharedPtr<FJsonValue>> ChainMappings;
    const int32 NumOps = Controller->GetNumRetargetOps();
    for (int32 I = 0; I < NumOps; ++I)
    {
        const FName OpName = Controller->GetOpName(I);
        auto OpObj = MakeShared<FJsonObject>();
        OpObj->SetNumberField(TEXT("index"), I);
        OpObj->SetStringField(TEXT("name"), OpName.ToString());
        OpObj->SetBoolField(TEXT("enabled"), Controller->GetRetargetOpEnabled(I));
        OpObj->SetNumberField(TEXT("parent_index"), Controller->GetParentOpIndex(I));
        OpObj->SetStringField(TEXT("parent_op_name"), Controller->GetParentOpByName(OpName).ToString());
        if (const FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(I))
        {
            if (const UScriptStruct* OpType = Op->GetType())
            {
                OpObj->SetStringField(TEXT("type"), OpType->GetPathName());
            }
            if (const UScriptStruct* ParentType = Op->GetParentOpType())
            {
                OpObj->SetStringField(TEXT("parent_type"), ParentType->GetPathName());
            }
        }
        if (FInstancedStruct* OpStruct = Controller->GetRetargetOpStructAtIndex(I))
        {
            if (const UScriptStruct* ScriptStruct = OpStruct->GetScriptStruct())
            {
                OpObj->SetStringField(TEXT("struct"), ScriptStruct->GetPathName());
            }
        }
        if (UIKRetargetFKChainsController* FKController = Cast<UIKRetargetFKChainsController>(Controller->GetOpController(I)))
        {
            const FIKRetargetFKChainsOpSettings FKSettings = FKController->GetSettings();
            TArray<TSharedPtr<FJsonValue>> FKChains;
            for (const FRetargetFKChainSettings& ChainSettings : FKSettings.ChainsToRetarget)
            {
                FKChains.Add(MakeShared<FJsonValueObject>(FKChainSettingsToJson(ChainSettings)));
            }
            auto FKObj = MakeShared<FJsonObject>();
            FKObj->SetArrayField(TEXT("chains"), FKChains);
            FKObj->SetNumberField(TEXT("chain_count"), FKChains.Num());
            OpObj->SetObjectField(TEXT("fk_settings"), FKObj);
        }
        if (UIKRetargetIKChainsController* IKController = Cast<UIKRetargetIKChainsController>(Controller->GetOpController(I)))
        {
            const FIKRetargetOpBase* IKOp = Controller->GetRetargetOpByIndex(I);
            const FIKRetargetIKChainsOpSettings* IKSettings = IKOp
                ? reinterpret_cast<const FIKRetargetIKChainsOpSettings*>(IKOp->GetSettingsConst())
                : nullptr;
            TArray<TSharedPtr<FJsonValue>> IKChains;
            if (IKSettings)
            {
                for (const FRetargetIKChainSettings& ChainSettings : IKSettings->ChainsToRetarget)
                {
                    IKChains.Add(MakeShared<FJsonValueObject>(IKChainSettingsToJson(ChainSettings)));
                }
            }
            auto IKObj = MakeShared<FJsonObject>();
            IKObj->SetArrayField(TEXT("chains"), IKChains);
            IKObj->SetNumberField(TEXT("chain_count"), IKChains.Num());
            if (IKSettings)
            {
                IKObj->SetBoolField(TEXT("draw_final_goals"), IKSettings->bDrawFinalGoals);
                IKObj->SetBoolField(TEXT("draw_source_locations"), IKSettings->bDrawSourceLocations);
                IKObj->SetNumberField(TEXT("goal_draw_size"), IKSettings->GoalDrawSize);
                IKObj->SetNumberField(TEXT("goal_draw_thickness"), IKSettings->GoalDrawThickness);
            }
            OpObj->SetObjectField(TEXT("ik_settings"), IKObj);
        }
        Ops.Add(MakeShared<FJsonValueObject>(OpObj));

        const FRetargetChainMapping* Mapping = Controller->GetChainMapping(OpName);
        if (!Mapping)
        {
            continue;
        }
        for (const FRetargetChainPair& Pair : Mapping->GetChainPairs())
        {
            auto MapObj = MakeShared<FJsonObject>();
            MapObj->SetStringField(TEXT("op_name"), OpName.ToString());
            MapObj->SetStringField(TEXT("target_chain"), Pair.TargetChainName.ToString());
            MapObj->SetStringField(TEXT("source_chain"), Pair.SourceChainName.ToString());
            ChainMappings.Add(MakeShared<FJsonValueObject>(MapObj));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetStringField(TEXT("class"), Retargeter->GetClass()->GetName());
    R->SetObjectField(TEXT("source"), RigToJson(Controller->GetIKRig(ERetargetSourceOrTarget::Source)));
    R->SetObjectField(TEXT("target"), RigToJson(Controller->GetIKRig(ERetargetSourceOrTarget::Target)));
    R->SetStringField(TEXT("source_preview_mesh"), AssetPathOrEmpty(Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source)));
    R->SetStringField(TEXT("target_preview_mesh"), AssetPathOrEmpty(Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target)));
    R->SetArrayField(TEXT("ops"), Ops);
    R->SetNumberField(TEXT("op_count"), Ops.Num());
    R->SetArrayField(TEXT("chain_mappings"), ChainMappings);
    R->SetNumberField(TEXT("chain_mapping_count"), ChainMappings.Num());
    R->SetObjectField(TEXT("source_poses"), PoseSideToJson(ERetargetSourceOrTarget::Source));
    R->SetObjectField(TEXT("target_poses"), PoseSideToJson(ERetargetSourceOrTarget::Target));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FString NormalizeToken(FString Raw)
{
    Raw = Raw.TrimStartAndEnd();
    Raw.ReplaceInline(TEXT(" "), TEXT(""));
    Raw.ReplaceInline(TEXT("_"), TEXT(""));
    Raw.ReplaceInline(TEXT("-"), TEXT(""));
    Raw.ReplaceInline(TEXT("."), TEXT(""));
    Raw.ToLowerInline();
    return Raw;
}

bool TryGetAnyBoolField(const TSharedPtr<FJsonObject>& Args, const TArray<const TCHAR*>& Names, bool& Out)
{
    if (!Args.IsValid())
    {
        return false;
    }
    for (const TCHAR* Name : Names)
    {
        bool Value = false;
        if (Args->TryGetBoolField(Name, Value))
        {
            Out = Value;
            return true;
        }
    }
    return false;
}

bool TryGetAnyNumberField(const TSharedPtr<FJsonObject>& Args, const TArray<const TCHAR*>& Names, double& Out)
{
    if (!Args.IsValid())
    {
        return false;
    }
    for (const TCHAR* Name : Names)
    {
        double Value = 0.0;
        if (Args->TryGetNumberField(Name, Value))
        {
            Out = Value;
            return true;
        }
    }
    return false;
}

bool TryGetAnyIntField(const TSharedPtr<FJsonObject>& Args, const TArray<const TCHAR*>& Names, int32& Out)
{
    double Value = 0.0;
    if (!TryGetAnyNumberField(Args, Names, Value))
    {
        return false;
    }
    Out = FMath::RoundToInt(Value);
    return true;
}

bool ParseAutoMapChainType(const FString& Raw, EAutoMapChainType& Out)
{
    const FString Key = NormalizeToken(Raw);
    if (Key.IsEmpty() || Key == TEXT("fuzzy") || Key == TEXT("best") || Key == TEXT("levenshtein"))
    {
        Out = EAutoMapChainType::Fuzzy;
        return true;
    }
    if (Key == TEXT("exact") || Key == TEXT("name"))
    {
        Out = EAutoMapChainType::Exact;
        return true;
    }
    if (Key == TEXT("clear") || Key == TEXT("none") || Key == TEXT("reset"))
    {
        Out = EAutoMapChainType::Clear;
        return true;
    }
    return false;
}

bool ParseRetargetAutoAlignMethod(const FString& Raw, ERetargetAutoAlignMethod& Out)
{
    const FString Key = NormalizeToken(Raw);
    if (Key.IsEmpty() || Key == TEXT("chaintochain") || Key == TEXT("chain") || Key == TEXT("direction"))
    {
        Out = ERetargetAutoAlignMethod::ChainToChain;
        return true;
    }
    if (Key == TEXT("localrotationaxes") || Key == TEXT("localaxes") || Key == TEXT("local"))
    {
        Out = ERetargetAutoAlignMethod::LocalRotationAxes;
        return true;
    }
    if (Key == TEXT("globalrotationaxes") || Key == TEXT("globalaxes") || Key == TEXT("global"))
    {
        Out = ERetargetAutoAlignMethod::GlobalRotationAxes;
        return true;
    }
    if (Key == TEXT("meshtomesh") || Key == TEXT("mesh"))
    {
        Out = ERetargetAutoAlignMethod::MeshToMesh;
        return true;
    }
    return false;
}

bool ParseFKRotationMode(const FString& Raw, EFKChainRotationMode& Out)
{
    const FString Key = NormalizeToken(Raw);
    if (Key == TEXT("none"))
    {
        Out = EFKChainRotationMode::None;
        return true;
    }
    if (Key.IsEmpty() || Key == TEXT("interpolated") || Key == TEXT("interpolate"))
    {
        Out = EFKChainRotationMode::Interpolated;
        return true;
    }
    if (Key == TEXT("onetoone") || Key == TEXT("1to1"))
    {
        Out = EFKChainRotationMode::OneToOne;
        return true;
    }
    if (Key == TEXT("onetoonereversed") || Key == TEXT("1to1reversed") || Key == TEXT("reversed"))
    {
        Out = EFKChainRotationMode::OneToOneReversed;
        return true;
    }
    if (Key == TEXT("matchchain"))
    {
        Out = EFKChainRotationMode::MatchChain;
        return true;
    }
    if (Key == TEXT("matchscaledchain"))
    {
        Out = EFKChainRotationMode::MatchScaledChain;
        return true;
    }
    if (Key == TEXT("copylocal"))
    {
        Out = EFKChainRotationMode::CopyLocal;
        return true;
    }
    return false;
}

bool ParseFKTranslationMode(const FString& Raw, EFKChainTranslationMode& Out)
{
    const FString Key = NormalizeToken(Raw);
    if (Key.IsEmpty() || Key == TEXT("none"))
    {
        Out = EFKChainTranslationMode::None;
        return true;
    }
    if (Key == TEXT("globallyscaled") || Key == TEXT("scaled"))
    {
        Out = EFKChainTranslationMode::GloballyScaled;
        return true;
    }
    if (Key == TEXT("absolute"))
    {
        Out = EFKChainTranslationMode::Absolute;
        return true;
    }
    if (Key == TEXT("stretchbonelengthuniformly") || Key == TEXT("stretchuniform"))
    {
        Out = EFKChainTranslationMode::StretchBoneLengthUniformly;
        return true;
    }
    if (Key == TEXT("stretchbonelengthnonuniformly") || Key == TEXT("stretchnonuniform"))
    {
        Out = EFKChainTranslationMode::StretchBoneLengthNonUniformly;
        return true;
    }
    if (Key == TEXT("orientandscale"))
    {
        Out = EFKChainTranslationMode::OrientAndScale;
        return true;
    }
    return false;
}

FString NormalizeIKRetargetOpType(const FString& Raw)
{
    FString Type = Raw.TrimStartAndEnd();
    if (Type.StartsWith(TEXT("/Script/")))
    {
        return Type;
    }

    const FString Key = NormalizeToken(Type);
    static const TMap<FString, FString> Aliases = {
        {TEXT("pelvis"), TEXT("/Script/IKRig.IKRetargetPelvisMotionOp")},
        {TEXT("pelvismotion"), TEXT("/Script/IKRig.IKRetargetPelvisMotionOp")},
        {TEXT("fk"), TEXT("/Script/IKRig.IKRetargetFKChainsOp")},
        {TEXT("fkchains"), TEXT("/Script/IKRig.IKRetargetFKChainsOp")},
        {TEXT("ik"), TEXT("/Script/IKRig.IKRetargetIKChainsOp")},
        {TEXT("ikchains"), TEXT("/Script/IKRig.IKRetargetIKChainsOp")},
        {TEXT("ikgoals"), TEXT("/Script/IKRig.IKRetargetIKChainsOp")},
        {TEXT("runik"), TEXT("/Script/IKRig.IKRetargetRunIKRigOp")},
        {TEXT("runikrig"), TEXT("/Script/IKRig.IKRetargetRunIKRigOp")},
        {TEXT("iksolve"), TEXT("/Script/IKRig.IKRetargetRunIKRigOp")},
        {TEXT("root"), TEXT("/Script/IKRig.IKRetargetRootMotionOp")},
        {TEXT("rootmotion"), TEXT("/Script/IKRig.IKRetargetRootMotionOp")},
        {TEXT("copypose"), TEXT("/Script/IKRig.IKRetargetCopyBasePoseOp")},
        {TEXT("copybasepose"), TEXT("/Script/IKRig.IKRetargetCopyBasePoseOp")},
        {TEXT("additivepose"), TEXT("/Script/IKRig.IKRetargetAdditivePoseOp")},
        {TEXT("pinbone"), TEXT("/Script/IKRig.IKRetargetPinBoneOp")},
        {TEXT("pinbones"), TEXT("/Script/IKRig.IKRetargetPinBoneOp")},
        {TEXT("polevector"), TEXT("/Script/IKRig.IKRetargetAlignPoleVectorOp")},
        {TEXT("alignpolevector"), TEXT("/Script/IKRig.IKRetargetAlignPoleVectorOp")},
        {TEXT("floor"), TEXT("/Script/IKRig.IKRetargetFloorConstraintOp")},
        {TEXT("floorconstraint"), TEXT("/Script/IKRig.IKRetargetFloorConstraintOp")},
        {TEXT("stretch"), TEXT("/Script/IKRig.IKRetargetStretchChainOp")},
        {TEXT("stretchchain"), TEXT("/Script/IKRig.IKRetargetStretchChainOp")},
        {TEXT("stride"), TEXT("/Script/IKRig.IKRetargetStrideWarpingOp")},
        {TEXT("stridewarping"), TEXT("/Script/IKRig.IKRetargetStrideWarpingOp")},
        {TEXT("speedplant"), TEXT("/Script/IKRig.IKRetargetSpeedPlantingOp")},
        {TEXT("speedplanting"), TEXT("/Script/IKRig.IKRetargetSpeedPlantingOp")},
        {TEXT("scalesource"), TEXT("/Script/IKRig.IKRetargetScaleSourceOp")},
        {TEXT("filterbone"), TEXT("/Script/IKRig.IKRetargetFilterBoneOp")},
        {TEXT("filterbones"), TEXT("/Script/IKRig.IKRetargetFilterBoneOp")},
        {TEXT("curveremap"), TEXT("/Script/IKRig.IKRetargetCurveRemapOp")},
        {TEXT("remapcurves"), TEXT("/Script/IKRig.IKRetargetCurveRemapOp")},
    };
    if (const FString* Mapped = Aliases.Find(Key))
    {
        return *Mapped;
    }
    if (Type.StartsWith(TEXT("IKRetarget")))
    {
        return FString::Printf(TEXT("/Script/IKRig.%s"), *Type);
    }
    return FString::Printf(TEXT("/Script/IKRig.IKRetarget%sOp"), *Type);
}

UScriptStruct* ResolveIKRetargetOpStruct(const FString& Raw, FString& OutPath)
{
    OutPath = NormalizeIKRetargetOpType(Raw);
    UScriptStruct* OpStruct = FindObject<UScriptStruct>(nullptr, *OutPath);
    if (!OpStruct)
    {
        OpStruct = LoadObject<UScriptStruct>(nullptr, *OutPath);
    }
    return OpStruct;
}

FSageToolDispatch::FOutcome ResolveIKRetargeterController(
    const TSharedPtr<FJsonObject>& Args,
    UIKRetargeter*& OutRetargeter,
    UIKRetargeterController*& OutController)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    OutRetargeter = Cast<UIKRetargeter>(ResolveAsset(Path));
    if (!OutRetargeter)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UIKRetargeter: %s"), *Path));
    }
    OutController = UIKRetargeterController::GetController(OutRetargeter);
    if (!OutController)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to get IK Retargeter controller"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(MakeShared<FJsonObject>());
}

bool TryResolveIKRetargeterOpIndex(
    const TSharedPtr<FJsonObject>& Args,
    UIKRetargeterController* Controller,
    int32& OutIndex,
    FString& OutError)
{
    if (!Controller)
    {
        OutError = TEXT("IK Retargeter controller unavailable");
        return false;
    }
    if (TryGetAnyIntField(Args, TArray<const TCHAR*>{ TEXT("index"), TEXT("op_index") }, OutIndex))
    {
        if (OutIndex >= 0 && OutIndex < Controller->GetNumRetargetOps())
        {
            return true;
        }
        OutError = FString::Printf(TEXT("retarget op index out of range: %d"), OutIndex);
        return false;
    }

    FString OpName;
    if (TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("op_name"), TEXT("name") }, OpName))
    {
        OutIndex = Controller->GetIndexOfOpByName(FName(*OpName));
        if (OutIndex != INDEX_NONE)
        {
            return true;
        }
        OutError = FString::Printf(TEXT("retarget op not found: %s"), *OpName);
        return false;
    }

    OutError = TEXT("missing 'index' or 'op_name'");
    return false;
}

int32 FindFirstFKRetargetOpIndex(UIKRetargeterController* Controller)
{
    if (!Controller)
    {
        return INDEX_NONE;
    }
    for (int32 Index = 0; Index < Controller->GetNumRetargetOps(); ++Index)
    {
        FInstancedStruct* OpStruct = Controller->GetRetargetOpStructAtIndex(Index);
        const UScriptStruct* ScriptStruct = OpStruct ? OpStruct->GetScriptStruct() : nullptr;
        if (ScriptStruct && ScriptStruct->IsChildOf(FIKRetargetFKChainsOp::StaticStruct()))
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

int32 FindFirstIKRetargetOpIndex(UIKRetargeterController* Controller)
{
    if (!Controller)
    {
        return INDEX_NONE;
    }
    for (int32 Index = 0; Index < Controller->GetNumRetargetOps(); ++Index)
    {
        FInstancedStruct* OpStruct = Controller->GetRetargetOpStructAtIndex(Index);
        const UScriptStruct* ScriptStruct = OpStruct ? OpStruct->GetScriptStruct() : nullptr;
        if (ScriptStruct && ScriptStruct->IsChildOf(FIKRetargetIKChainsOp::StaticStruct()))
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

void AddIKRetargeterReadback(UIKRetargeter* Retargeter, TSharedRef<FJsonObject> Result)
{
    if (!Retargeter)
    {
        return;
    }
    auto Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("path"), Retargeter->GetPathName());
    FSageToolDispatch::FOutcome Snapshot = ReadIKRetargeterImpl(Args);
    if (Snapshot.bSuccess && Snapshot.Result.IsValid())
    {
        Result->SetObjectField(TEXT("readback"), Snapshot.Result.ToSharedRef());
    }
}

TSharedRef<FJsonObject> FKChainSettingsToJson(const FRetargetFKChainSettings& Settings)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("target_chain"), Settings.TargetChainName.ToString());
    Obj->SetBoolField(TEXT("enable_fk"), Settings.EnableFK);
    Obj->SetStringField(TEXT("rotation_mode"), StaticEnum<EFKChainRotationMode>()->GetNameStringByValue(static_cast<int64>(Settings.RotationMode)));
    Obj->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
    Obj->SetStringField(TEXT("translation_mode"), StaticEnum<EFKChainTranslationMode>()->GetNameStringByValue(static_cast<int64>(Settings.TranslationMode)));
    Obj->SetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
    return Obj;
}

TSharedRef<FJsonObject> IKChainSettingsToJson(const FRetargetIKChainSettings& Settings)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("target_chain"), Settings.TargetChainName.ToString());
    Obj->SetBoolField(TEXT("enable_ik"), Settings.EnableIK);
    Obj->SetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
    Obj->SetNumberField(TEXT("blend_to_source_translation"), Settings.BlendToSourceTranslation);
    Obj->SetNumberField(TEXT("blend_to_source_rotation"), Settings.BlendToSourceRotation);
    Obj->SetField(TEXT("blend_to_source_weights"), detail::Vec3ToJson(Settings.BlendToSourceWeights));
    Obj->SetBoolField(TEXT("apply_pelvis_offset_to_source_goals"), Settings.ApplyPelvisOffsetToSourceGoals);
    Obj->SetField(TEXT("static_offset"), detail::Vec3ToJson(Settings.StaticOffset));
    Obj->SetField(TEXT("static_local_offset"), detail::Vec3ToJson(Settings.StaticLocalOffset));
    Obj->SetField(TEXT("static_rotation_offset"), detail::Rot3ToJson(Settings.StaticRotationOffset));
    Obj->SetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
    Obj->SetNumberField(TEXT("extension"), Settings.Extension);
    return Obj;
}

// ---------------------------------------------------------------------------
// animation.set_ik_retargeter_rigs
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetIKRetargeterRigsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(ResolveAsset(Path));
    if (!Retargeter)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UIKRetargeter: %s"), *Path));
    }
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to get IK Retargeter controller"));
    }

    FString SourceRigPath, TargetRigPath, SourceMeshPath, TargetMeshPath;
    UIKRigDefinition* SourceRig = nullptr;
    UIKRigDefinition* TargetRig = nullptr;
    if (Args->TryGetStringField(TEXT("source_ik_rig"), SourceRigPath) && !SourceRigPath.IsEmpty())
    {
        SourceRig = Cast<UIKRigDefinition>(ResolveAsset(SourceRigPath));
        if (!SourceRig)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a source UIKRigDefinition: %s"), *SourceRigPath));
        }
    }
    if (Args->TryGetStringField(TEXT("target_ik_rig"), TargetRigPath) && !TargetRigPath.IsEmpty())
    {
        TargetRig = Cast<UIKRigDefinition>(ResolveAsset(TargetRigPath));
        if (!TargetRig)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a target UIKRigDefinition: %s"), *TargetRigPath));
        }
    }
    USkeletalMesh* SourceMesh = nullptr;
    USkeletalMesh* TargetMesh = nullptr;
    if (Args->TryGetStringField(TEXT("source_preview_mesh"), SourceMeshPath) && !SourceMeshPath.IsEmpty())
    {
        SourceMesh = Cast<USkeletalMesh>(ResolveAsset(SourceMeshPath));
        if (!SourceMesh)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a source USkeletalMesh: %s"), *SourceMeshPath));
        }
    }
    if (Args->TryGetStringField(TEXT("target_preview_mesh"), TargetMeshPath) && !TargetMeshPath.IsEmpty())
    {
        TargetMesh = Cast<USkeletalMesh>(ResolveAsset(TargetMeshPath));
        if (!TargetMesh)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a target USkeletalMesh: %s"), *TargetMeshPath));
        }
    }

    bool bAddDefaultOps = true;
    bool bAssignOps = true;
    bool bCleanAsset = true;
    bool bCleanChainMaps = false;
    bool bAutoMap = true;
    bool bForceRemap = true;
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
    Args->TryGetBoolField(TEXT("assign_ops"), bAssignOps);
    Args->TryGetBoolField(TEXT("assign_ik_rigs"), bAssignOps);
    Args->TryGetBoolField(TEXT("clean_asset"), bCleanAsset);
    Args->TryGetBoolField(TEXT("clean_chain_maps"), bCleanChainMaps);
    Args->TryGetBoolField(TEXT("auto_map"), bAutoMap);
    Args->TryGetBoolField(TEXT("force_remap"), bForceRemap);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    FString AutoMapTypeString = TEXT("fuzzy");
    Args->TryGetStringField(TEXT("auto_map_type"), AutoMapTypeString);
    EAutoMapChainType AutoMapType = EAutoMapChainType::Fuzzy;
    if (!ParseAutoMapChainType(AutoMapTypeString, AutoMapType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid auto_map_type: %s"), *AutoMapTypeString));
    }

    TSharedPtr<FJsonObject> Before;
    {
        auto SnapshotArgs = MakeShared<FJsonObject>();
        SnapshotArgs->SetStringField(TEXT("path"), Retargeter->GetPathName());
        FSageToolDispatch::FOutcome Snapshot = ReadIKRetargeterImpl(SnapshotArgs);
        if (Snapshot.bSuccess && Snapshot.Result.IsValid())
        {
            Before = Snapshot.Result;
        }
    }

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetStringField(TEXT("source_ik_rig"), SourceRigPath);
        R->SetStringField(TEXT("target_ik_rig"), TargetRigPath);
        R->SetStringField(TEXT("source_preview_mesh"), SourceMeshPath);
        R->SetStringField(TEXT("target_preview_mesh"), TargetMeshPath);
        R->SetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
        R->SetBoolField(TEXT("assign_ops"), bAssignOps);
        R->SetBoolField(TEXT("clean_asset"), bCleanAsset);
        R->SetBoolField(TEXT("clean_chain_maps"), bCleanChainMaps);
        R->SetBoolField(TEXT("auto_map"), bAutoMap);
        R->SetStringField(TEXT("auto_map_type"), AutoMapTypeString);
        R->SetBoolField(TEXT("force_remap"), bForceRemap);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        if (Before.IsValid())
        {
            R->SetObjectField(TEXT("before"), Before.ToSharedRef());
        }
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargeterRigs", "Sage: Set IK Retargeter Rigs"));
    Retargeter->Modify();
    if (SourceRig)
    {
        Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
    }
    if (TargetRig)
    {
        Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
    }
    if (SourceMesh)
    {
        Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourceMesh);
    }
    if (TargetMesh)
    {
        Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetMesh);
    }
    if (bAddDefaultOps)
    {
        Controller->AddDefaultOps();
    }
    if (bAssignOps)
    {
        if (const UIKRigDefinition* ActiveSourceRig = Controller->GetIKRig(ERetargetSourceOrTarget::Source))
        {
            Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, ActiveSourceRig);
        }
        if (const UIKRigDefinition* ActiveTargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target))
        {
            Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, ActiveTargetRig);
        }
    }
    if (bCleanChainMaps)
    {
        Controller->CleanChainMaps();
    }
    if (bAutoMap)
    {
        Controller->AutoMapChains(AutoMapType, bForceRemap);
    }
    if (bCleanAsset)
    {
        Controller->CleanAsset();
    }
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetStringField(TEXT("source_ik_rig"), Controller->GetIKRig(ERetargetSourceOrTarget::Source)
        ? Controller->GetIKRig(ERetargetSourceOrTarget::Source)->GetPathName() : FString());
    R->SetStringField(TEXT("target_ik_rig"), Controller->GetIKRig(ERetargetSourceOrTarget::Target)
        ? Controller->GetIKRig(ERetargetSourceOrTarget::Target)->GetPathName() : FString());
    R->SetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
    R->SetBoolField(TEXT("assign_ops"), bAssignOps);
    R->SetBoolField(TEXT("clean_asset"), bCleanAsset);
    R->SetBoolField(TEXT("clean_chain_maps"), bCleanChainMaps);
    R->SetBoolField(TEXT("auto_map"), bAutoMap);
    R->SetStringField(TEXT("auto_map_type"), AutoMapTypeString);
    R->SetBoolField(TEXT("force_remap"), bForceRemap);
    R->SetBoolField(TEXT("modified"), true);
    if (Before.IsValid())
    {
        R->SetObjectField(TEXT("before"), Before.ToSharedRef());
    }
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.setup_ik_retargeter_ops
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetupIKRetargeterOpsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    bool bDryRun = false;
    bool bSave = false;
    bool bAddDefaultOps = true;
    bool bAssignRigs = true;
    bool bCleanAsset = true;
    bool bCleanChainMaps = true;
    bool bAutoMap = true;
    bool bForceRemap = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
    Args->TryGetBoolField(TEXT("assign_ik_rigs"), bAssignRigs);
    Args->TryGetBoolField(TEXT("clean_asset"), bCleanAsset);
    Args->TryGetBoolField(TEXT("clean_chain_maps"), bCleanChainMaps);
    Args->TryGetBoolField(TEXT("auto_map"), bAutoMap);
    Args->TryGetBoolField(TEXT("force_remap"), bForceRemap);

    FString AutoMapTypeString = TEXT("fuzzy");
    Args->TryGetStringField(TEXT("auto_map_type"), AutoMapTypeString);
    EAutoMapChainType AutoMapType = EAutoMapChainType::Fuzzy;
    if (!ParseAutoMapChainType(AutoMapTypeString, AutoMapType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid auto_map_type: %s"), *AutoMapTypeString));
    }

    FString OpName;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("op_name"), TEXT("name") }, OpName);

    const int32 BeforeOps = Controller->GetNumRetargetOps();
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetNumberField(TEXT("op_count_before"), BeforeOps);
        R->SetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
        R->SetBoolField(TEXT("assign_ik_rigs"), bAssignRigs);
        R->SetBoolField(TEXT("clean_asset"), bCleanAsset);
        R->SetBoolField(TEXT("clean_chain_maps"), bCleanChainMaps);
        R->SetBoolField(TEXT("auto_map"), bAutoMap);
        R->SetBoolField(TEXT("force_remap"), bForceRemap);
        R->SetStringField(TEXT("op_name"), OpName);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetupIKRetargeterOps", "Sage: Setup IK Retargeter Ops"));
    Retargeter->Modify();
    if (bAddDefaultOps)
    {
        Controller->AddDefaultOps();
    }
    if (bAssignRigs)
    {
        if (const UIKRigDefinition* SourceRig = Controller->GetIKRig(ERetargetSourceOrTarget::Source))
        {
            Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, SourceRig);
        }
        if (const UIKRigDefinition* TargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target))
        {
            Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, TargetRig);
        }
    }
    if (bCleanChainMaps)
    {
        Controller->CleanChainMaps(OpName.IsEmpty() ? NAME_None : FName(*OpName));
    }
    if (bAutoMap)
    {
        Controller->AutoMapChains(AutoMapType, bForceRemap, OpName.IsEmpty() ? NAME_None : FName(*OpName));
    }
    if (bCleanAsset)
    {
        Controller->CleanAsset();
    }
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetNumberField(TEXT("op_count_before"), BeforeOps);
    R->SetNumberField(TEXT("op_count_after"), Controller->GetNumRetargetOps());
    R->SetBoolField(TEXT("add_default_ops"), bAddDefaultOps);
    R->SetBoolField(TEXT("assign_ik_rigs"), bAssignRigs);
    R->SetBoolField(TEXT("clean_asset"), bCleanAsset);
    R->SetBoolField(TEXT("clean_chain_maps"), bCleanChainMaps);
    R->SetBoolField(TEXT("auto_map"), bAutoMap);
    R->SetBoolField(TEXT("force_remap"), bForceRemap);
    R->SetStringField(TEXT("op_name"), OpName);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_ik_retargeter_op
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddIKRetargeterOpImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString RawType;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("op_type"), TEXT("type"), TEXT("struct"), TEXT("class") }, RawType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'op_type'"));
    }

    FString NormalizedType;
    UScriptStruct* OpStruct = ResolveIKRetargetOpStruct(RawType, NormalizedType);
    if (!OpStruct)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("retarget op struct not found: %s"), *NormalizedType));
    }

    FString ParentName;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("parent_op_name"), TEXT("parent") }, ParentName);
    FString RequestedName;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("op_name"), TEXT("name") }, RequestedName);
    bool bRunInitialSetup = true;
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("run_initial_setup"), bRunInitialSetup);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetStringField(TEXT("op_type"), NormalizedType);
        R->SetStringField(TEXT("parent_op_name"), ParentName);
        R->SetStringField(TEXT("requested_name"), RequestedName);
        R->SetBoolField(TEXT("run_initial_setup"), bRunInitialSetup);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageAddIKRetargeterOp", "Sage: Add IK Retargeter Op"));
    Retargeter->Modify();
    int32 OpIndex = Controller->AddRetargetOp(OpStruct, ParentName.IsEmpty() ? NAME_None : FName(*ParentName));
    if (OpIndex == INDEX_NONE)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to add retarget op: %s"), *NormalizedType));
    }
    FName ActualName = Controller->GetOpName(OpIndex);
    if (!RequestedName.IsEmpty())
    {
        ActualName = Controller->SetOpName(FName(*RequestedName), OpIndex);
        OpIndex = Controller->GetIndexOfOpByName(ActualName);
    }
    if (bRunInitialSetup && OpIndex != INDEX_NONE)
    {
        Controller->RunOpInitialSetup(OpIndex);
    }
    Controller->CleanAsset();
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetStringField(TEXT("op_type"), NormalizedType);
    R->SetNumberField(TEXT("index"), OpIndex);
    R->SetStringField(TEXT("op_name"), ActualName.ToString());
    R->SetStringField(TEXT("parent_op_name"), Controller->GetParentOpByName(ActualName).ToString());
    R->SetBoolField(TEXT("run_initial_setup"), bRunInitialSetup);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.remove_ik_retargeter_op
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome RemoveIKRetargeterOpImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    int32 OpIndex = INDEX_NONE;
    FString Error;
    if (!TryResolveIKRetargeterOpIndex(Args, Controller, OpIndex, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    const FName OpName = Controller->GetOpName(OpIndex);
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetNumberField(TEXT("index"), OpIndex);
        R->SetStringField(TEXT("op_name"), OpName.ToString());
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageRemoveIKRetargeterOp", "Sage: Remove IK Retargeter Op"));
    Retargeter->Modify();
    const bool bRemoved = Controller->RemoveRetargetOp(OpIndex);
    if (!bRemoved)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to remove retarget op: %s"), *OpName.ToString()));
    }
    Controller->CleanAsset();
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetNumberField(TEXT("removed_index"), OpIndex);
    R->SetStringField(TEXT("removed_op_name"), OpName.ToString());
    R->SetBoolField(TEXT("removed"), true);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.move_ik_retargeter_op
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome MoveIKRetargeterOpImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    int32 FromIndex = INDEX_NONE;
    FString Error;
    if (!TryResolveIKRetargeterOpIndex(Args, Controller, FromIndex, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    int32 ToIndex = INDEX_NONE;
    if (!TryGetAnyIntField(Args, TArray<const TCHAR*>{ TEXT("to_index"), TEXT("target_index"), TEXT("new_index") }, ToIndex))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'to_index'"));
    }

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    const FName OpName = Controller->GetOpName(FromIndex);
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetStringField(TEXT("op_name"), OpName.ToString());
        R->SetNumberField(TEXT("from_index"), FromIndex);
        R->SetNumberField(TEXT("to_index"), ToIndex);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageMoveIKRetargeterOp", "Sage: Move IK Retargeter Op"));
    Retargeter->Modify();
    const bool bMoved = Controller->MoveRetargetOpInStack(FromIndex, ToIndex);
    if (!bMoved)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to move retarget op '%s' to index %d"), *OpName.ToString(), ToIndex));
    }
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetStringField(TEXT("op_name"), OpName.ToString());
    R->SetNumberField(TEXT("from_index"), FromIndex);
    R->SetNumberField(TEXT("requested_to_index"), ToIndex);
    R->SetNumberField(TEXT("actual_index"), Controller->GetIndexOfOpByName(OpName));
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_ik_retargeter_op_enabled
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetIKRetargeterOpEnabledImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    int32 OpIndex = INDEX_NONE;
    FString Error;
    if (!TryResolveIKRetargeterOpIndex(Args, Controller, OpIndex, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    bool bEnabled = true;
    if (!TryGetAnyBoolField(Args, TArray<const TCHAR*>{ TEXT("enabled"), TEXT("enable") }, bEnabled))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'enabled'"));
    }
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    const FName OpName = Controller->GetOpName(OpIndex);
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetNumberField(TEXT("index"), OpIndex);
        R->SetStringField(TEXT("op_name"), OpName.ToString());
        R->SetBoolField(TEXT("enabled"), bEnabled);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargeterOpEnabled", "Sage: Set IK Retargeter Op Enabled"));
    Retargeter->Modify();
    const bool bOk = Controller->SetRetargetOpEnabled(OpIndex, bEnabled);
    if (!bOk)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to set enabled on retarget op: %s"), *OpName.ToString()));
    }
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetNumberField(TEXT("index"), OpIndex);
    R->SetStringField(TEXT("op_name"), OpName.ToString());
    R->SetBoolField(TEXT("enabled"), Controller->GetRetargetOpEnabled(OpIndex));
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.auto_map_ik_retargeter_chains
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AutoMapIKRetargeterChainsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString AutoMapTypeString = TEXT("fuzzy");
    Args->TryGetStringField(TEXT("auto_map_type"), AutoMapTypeString);
    EAutoMapChainType AutoMapType = EAutoMapChainType::Fuzzy;
    if (!ParseAutoMapChainType(AutoMapTypeString, AutoMapType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid auto_map_type: %s"), *AutoMapTypeString));
    }

    FString OpName;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("op_name"), TEXT("name") }, OpName);
    bool bForceRemap = true;
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("force_remap"), bForceRemap);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetStringField(TEXT("auto_map_type"), AutoMapTypeString);
        R->SetStringField(TEXT("op_name"), OpName);
        R->SetBoolField(TEXT("force_remap"), bForceRemap);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageAutoMapIKRetargeterChains", "Sage: Auto Map IK Retargeter Chains"));
    Retargeter->Modify();
    Controller->AutoMapChains(AutoMapType, bForceRemap, OpName.IsEmpty() ? NAME_None : FName(*OpName));
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetStringField(TEXT("auto_map_type"), AutoMapTypeString);
    R->SetStringField(TEXT("op_name"), OpName);
    R->SetBoolField(TEXT("force_remap"), bForceRemap);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.reset_ik_retargeter_chain_settings
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ResetIKRetargeterChainSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString TargetChain;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("target_chain"), TEXT("chain"), TEXT("chain_name") }, TargetChain))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'target_chain'"));
    }

    FString OpName;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("op_name"), TEXT("name") }, OpName);
    bool bAllOps = OpName.IsEmpty();
    Args->TryGetBoolField(TEXT("all_ops"), bAllOps);
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetStringField(TEXT("target_chain"), TargetChain);
        R->SetStringField(TEXT("op_name"), OpName);
        R->SetBoolField(TEXT("all_ops"), bAllOps);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageResetIKRetargeterChainSettings", "Sage: Reset IK Retargeter Chain Settings"));
    Retargeter->Modify();
    if (bAllOps)
    {
        Controller->ResetChainSettingsInAllOps(FName(*TargetChain));
    }
    else
    {
        Controller->ResetChainSettingsToDefault(FName(*TargetChain), FName(*OpName));
    }
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetStringField(TEXT("target_chain"), TargetChain);
    R->SetStringField(TEXT("op_name"), OpName);
    R->SetBoolField(TEXT("all_ops"), bAllOps);
    R->SetBoolField(TEXT("modified"), true);
    if (!bAllOps)
    {
        R->SetBoolField(TEXT("at_default"), Controller->AreChainSettingsAtDefault(FName(*TargetChain), FName(*OpName)));
    }
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_ik_retargeter_ik_chain_settings
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetIKRetargeterIKChainSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString TargetChain;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("target_chain"), TEXT("chain"), TEXT("chain_name") }, TargetChain))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'target_chain'"));
    }

    int32 OpIndex = INDEX_NONE;
    FString OpError;
    FString OpName;
    if (TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("op_name"), TEXT("name") }, OpName) ||
        TryGetAnyIntField(Args, TArray<const TCHAR*>{ TEXT("index"), TEXT("op_index") }, OpIndex))
    {
        if (!TryResolveIKRetargeterOpIndex(Args, Controller, OpIndex, OpError))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, OpError);
        }
    }
    else
    {
        OpIndex = FindFirstIKRetargetOpIndex(Controller);
        if (OpIndex == INDEX_NONE)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("no IK Chains op found; add default ops or pass op_name/index"));
        }
    }

    UIKRetargetIKChainsController* IKController = Cast<UIKRetargetIKChainsController>(Controller->GetOpController(OpIndex));
    if (!IKController)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("retarget op is not an IK Chains op: %s"), *Controller->GetOpName(OpIndex).ToString()));
    }

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FIKRetargetOpBase* RetargetOp = Controller->GetRetargetOpByIndex(OpIndex);
    FIKRetargetIKChainsOpSettings* Settings = RetargetOp
        ? reinterpret_cast<FIKRetargetIKChainsOpSettings*>(RetargetOp->GetSettings())
        : nullptr;
    if (!Settings)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("IK Chains op settings unavailable"));
    }

    const int32 ChainIndex = Settings->ChainsToRetarget.IndexOfByPredicate(
        [&TargetChain](const FRetargetIKChainSettings& Item)
        {
            return Item.TargetChainName == FName(*TargetChain);
        });
    if (ChainIndex == INDEX_NONE)
    {
        TArray<FString> ExistingChains;
        for (const FRetargetIKChainSettings& Existing : Settings->ChainsToRetarget)
        {
            ExistingChains.Add(Existing.TargetChainName.ToString());
        }
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("target chain not found in IK Chains op settings: %s (op=%s, available=%s)"),
                *TargetChain,
                *Controller->GetOpName(OpIndex).ToString(),
                *FString::Join(ExistingChains, TEXT(", "))));
    }

    const FRetargetIKChainSettings Before = Settings->ChainsToRetarget[ChainIndex];
    FRetargetIKChainSettings Updated = Before;
    bool bAnyField = false;

    auto ApplyBool = [&](const TArray<const TCHAR*>& Names, bool& Field)
    {
        bool Value = Field;
        if (TryGetAnyBoolField(Args, Names, Value))
        {
            Field = Value;
            bAnyField = true;
        }
    };
    auto ApplyNumber = [&](const TArray<const TCHAR*>& Names, double& Field)
    {
        double Value = Field;
        if (TryGetAnyNumberField(Args, Names, Value))
        {
            Field = Value;
            bAnyField = true;
        }
    };
    auto ApplyVector = [&](const TArray<const TCHAR*>& Names, FVector& Field)
    {
        for (const TCHAR* Name : Names)
        {
            FVector Value = Field;
            if (ReadVectorArray(Args, Name, Value))
            {
                Field = Value;
                bAnyField = true;
                return;
            }
        }
    };
    auto ApplyRotator = [&](const TArray<const TCHAR*>& Names, FRotator& Field)
    {
        for (const TCHAR* Name : Names)
        {
            FRotator Value = Field;
            if (ReadRotatorArray(Args, Name, Value))
            {
                Field = Value;
                bAnyField = true;
                return;
            }
        }
    };

    ApplyBool(TArray<const TCHAR*>{ TEXT("enable_ik"), TEXT("enabled"), TEXT("enable") }, Updated.EnableIK);
    ApplyNumber(TArray<const TCHAR*>{ TEXT("blend_to_source") }, Updated.BlendToSource);
    ApplyNumber(TArray<const TCHAR*>{ TEXT("blend_to_source_translation"), TEXT("blend_translation") }, Updated.BlendToSourceTranslation);
    ApplyNumber(TArray<const TCHAR*>{ TEXT("blend_to_source_rotation"), TEXT("blend_rotation") }, Updated.BlendToSourceRotation);
    ApplyVector(TArray<const TCHAR*>{ TEXT("blend_to_source_weights"), TEXT("blend_weights") }, Updated.BlendToSourceWeights);
    ApplyBool(TArray<const TCHAR*>{ TEXT("apply_pelvis_offset_to_source_goals"), TEXT("apply_pelvis_offset") }, Updated.ApplyPelvisOffsetToSourceGoals);
    ApplyVector(TArray<const TCHAR*>{ TEXT("static_offset"), TEXT("offset") }, Updated.StaticOffset);
    ApplyVector(TArray<const TCHAR*>{ TEXT("static_local_offset"), TEXT("local_offset") }, Updated.StaticLocalOffset);
    ApplyRotator(TArray<const TCHAR*>{ TEXT("static_rotation_offset"), TEXT("rotation_offset") }, Updated.StaticRotationOffset);
    ApplyNumber(TArray<const TCHAR*>{ TEXT("scale_vertical"), TEXT("vertical_scale") }, Updated.ScaleVertical);
    ApplyNumber(TArray<const TCHAR*>{ TEXT("extension"), TEXT("scale_length") }, Updated.Extension);

    if (!bAnyField)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("no IK chain setting field supplied"));
    }

    const bool bWouldModify = !(Before == Updated);
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetNumberField(TEXT("op_index"), OpIndex);
        R->SetStringField(TEXT("op_name"), Controller->GetOpName(OpIndex).ToString());
        R->SetStringField(TEXT("target_chain"), TargetChain);
        R->SetObjectField(TEXT("before_settings"), IKChainSettingsToJson(Before));
        R->SetObjectField(TEXT("after_settings"), IKChainSettingsToJson(Updated));
        R->SetBoolField(TEXT("would_modify"), bWouldModify);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargeterIKChainSettings", "Sage: Set IK Retargeter IK Chain Settings"));
    Retargeter->Modify();
    Settings->ChainsToRetarget[ChainIndex] = Updated;
    FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
    Controller->OnOpPropertyChanged(Controller->GetOpName(OpIndex), Event);
    if (bWouldModify)
    {
        Retargeter->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetNumberField(TEXT("op_index"), OpIndex);
    R->SetStringField(TEXT("op_name"), Controller->GetOpName(OpIndex).ToString());
    R->SetStringField(TEXT("target_chain"), TargetChain);
    R->SetObjectField(TEXT("before_settings"), IKChainSettingsToJson(Before));
    R->SetObjectField(TEXT("after_settings"), IKChainSettingsToJson(Updated));
    R->SetBoolField(TEXT("modified"), bWouldModify);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave && bWouldModify, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_ik_retargeter_fk_chain_settings
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetIKRetargeterFKChainSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    FString TargetChain;
    if (!TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("target_chain"), TEXT("chain"), TEXT("chain_name") }, TargetChain))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'target_chain'"));
    }

    int32 OpIndex = INDEX_NONE;
    FString OpError;
    FString OpName;
    if (TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("op_name"), TEXT("name") }, OpName) ||
        TryGetAnyIntField(Args, TArray<const TCHAR*>{ TEXT("index"), TEXT("op_index") }, OpIndex))
    {
        if (!TryResolveIKRetargeterOpIndex(Args, Controller, OpIndex, OpError))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, OpError);
        }
    }
    else
    {
        OpIndex = FindFirstFKRetargetOpIndex(Controller);
        if (OpIndex == INDEX_NONE)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("no FK Chains op found; add default ops or pass op_name/index"));
        }
    }

    UIKRetargetFKChainsController* FKController = Cast<UIKRetargetFKChainsController>(Controller->GetOpController(OpIndex));
    if (!FKController)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("retarget op is not an FK Chains op: %s"), *Controller->GetOpName(OpIndex).ToString()));
    }

    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FIKRetargetFKChainsOpSettings Settings = FKController->GetSettings();
    int32 ChainIndex = Settings.ChainsToRetarget.IndexOfByPredicate(
        [&TargetChain](const FRetargetFKChainSettings& Item)
        {
            return Item.TargetChainName == FName(*TargetChain);
        });
    if (ChainIndex == INDEX_NONE)
    {
        ChainIndex = Settings.ChainsToRetarget.Add(FRetargetFKChainSettings(FName(*TargetChain)));
    }
    FRetargetFKChainSettings Updated = Settings.ChainsToRetarget[ChainIndex];

    bool bEnableFK = Updated.EnableFK;
    if (TryGetAnyBoolField(Args, TArray<const TCHAR*>{ TEXT("enable_fk"), TEXT("enabled") }, bEnableFK))
    {
        Updated.EnableFK = bEnableFK;
    }

    FString RotationModeString;
    if (TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("rotation_mode"), TEXT("fk_rotation_mode") }, RotationModeString))
    {
        EFKChainRotationMode Mode = Updated.RotationMode;
        if (!ParseFKRotationMode(RotationModeString, Mode))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("invalid rotation_mode: %s"), *RotationModeString));
        }
        Updated.RotationMode = Mode;
    }

    FString TranslationModeString;
    if (TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("translation_mode"), TEXT("fk_translation_mode") }, TranslationModeString))
    {
        EFKChainTranslationMode Mode = Updated.TranslationMode;
        if (!ParseFKTranslationMode(TranslationModeString, Mode))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("invalid translation_mode: %s"), *TranslationModeString));
        }
        Updated.TranslationMode = Mode;
    }

    double Number = 0.0;
    if (TryGetAnyNumberField(Args, TArray<const TCHAR*>{ TEXT("rotation_alpha"), TEXT("fk_rotation_alpha") }, Number))
    {
        Updated.RotationAlpha = Number;
    }
    if (TryGetAnyNumberField(Args, TArray<const TCHAR*>{ TEXT("translation_alpha"), TEXT("fk_translation_alpha") }, Number))
    {
        Updated.TranslationAlpha = Number;
    }

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetNumberField(TEXT("op_index"), OpIndex);
        R->SetStringField(TEXT("op_name"), Controller->GetOpName(OpIndex).ToString());
        R->SetObjectField(TEXT("settings"), FKChainSettingsToJson(Updated));
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargeterFKChainSettings", "Sage: Set IK Retargeter FK Chain Settings"));
    Retargeter->Modify();
    Settings.ChainsToRetarget[ChainIndex] = Updated;
    FKController->SetSettings(Settings);
    FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
    Controller->OnOpPropertyChanged(Controller->GetOpName(OpIndex), Event);
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetNumberField(TEXT("op_index"), OpIndex);
    R->SetStringField(TEXT("op_name"), Controller->GetOpName(OpIndex).ToString());
    R->SetObjectField(TEXT("settings"), FKChainSettingsToJson(Updated));
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_ik_retargeter_chain_mapping
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetIKRetargeterChainMappingImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, TargetChain, SourceChain, OpName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("target_chain"), TargetChain) || TargetChain.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'target_chain'"));
    }
    if (!Args->TryGetStringField(TEXT("source_chain"), SourceChain))
    {
        SourceChain = TEXT("None");
    }
    Args->TryGetStringField(TEXT("op_name"), OpName);
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(ResolveAsset(Path));
    if (!Retargeter)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UIKRetargeter: %s"), *Path));
    }
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to get IK Retargeter controller"));
    }

    const FName TargetChainName(*TargetChain);
    const FName OpFName(*OpName);
    const FName BeforeSource = Controller->GetSourceChain(TargetChainName, OpFName);
    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetStringField(TEXT("target_chain"), TargetChain);
        R->SetStringField(TEXT("source_chain"), SourceChain);
        R->SetStringField(TEXT("before_source_chain"), BeforeSource.ToString());
        R->SetStringField(TEXT("op_name"), OpName);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargeterChainMapping", "Sage: Set IK Retargeter Chain Mapping"));
    Retargeter->Modify();
    const bool bOk = Controller->SetSourceChain(FName(*SourceChain), TargetChainName, OpFName);
    if (!bOk)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("failed to map target chain '%s' to source chain '%s'"), *TargetChain, *SourceChain));
    }
    Retargeter->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetStringField(TEXT("target_chain"), TargetChain);
    R->SetStringField(TEXT("source_chain"), Controller->GetSourceChain(TargetChainName, OpFName).ToString());
    R->SetStringField(TEXT("before_source_chain"), BeforeSource.ToString());
    R->SetStringField(TEXT("op_name"), OpName);
    R->SetBoolField(TEXT("modified"), true);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_ik_retargeter_pose
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetIKRetargeterPoseImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SideString, PoseName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("side"), SideString) || SideString.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'side'"));
    }
    ERetargetSourceOrTarget Side = ERetargetSourceOrTarget::Target;
    if (!ParseRetargetSide(SideString, Side))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("side must be 'source' or 'target'"));
    }
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("pose_name"), TEXT("pose") }, PoseName);

    UIKRetargeter* Retargeter = nullptr;
    UIKRetargeterController* Controller = nullptr;
    FSageToolDispatch::FOutcome Resolved = ResolveIKRetargeterController(Args, Retargeter, Controller);
    if (!Resolved.bSuccess) return Resolved;

    bool bCreate = false;
    bool bRemove = false;
    bool bDuplicate = false;
    bool bRename = false;
    bool bReset = false;
    bool bAutoAlign = false;
    bool bSnapToGround = false;
    bool bSetCurrent = true;
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("create"), bCreate);
    Args->TryGetBoolField(TEXT("remove"), bRemove);
    Args->TryGetBoolField(TEXT("delete"), bRemove);
    Args->TryGetBoolField(TEXT("duplicate"), bDuplicate);
    Args->TryGetBoolField(TEXT("rename"), bRename);
    Args->TryGetBoolField(TEXT("reset"), bReset);
    Args->TryGetBoolField(TEXT("auto_align"), bAutoAlign);
    Args->TryGetBoolField(TEXT("snap_to_ground"), bSnapToGround);
    Args->TryGetBoolField(TEXT("current"), bSetCurrent);
    Args->TryGetBoolField(TEXT("set_current"), bSetCurrent);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FString NewPoseName;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("new_name"), TEXT("to") }, NewPoseName);
    FString SourcePoseName;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("from_pose"), TEXT("duplicate_from"), TEXT("source_pose") }, SourcePoseName);
    FString OldPoseName;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("old_name"), TEXT("from") }, OldPoseName);

    TArray<FString> ResetBoneStrings;
    if (!ReadStringArrayField(Args, TEXT("reset_bones"), ResetBoneStrings))
    {
        ReadStringArrayField(Args, TEXT("bones"), ResetBoneStrings);
    }
    TArray<FString> AlignBoneStrings;
    if (!ReadStringArrayField(Args, TEXT("align_bones"), AlignBoneStrings))
    {
        ReadStringArrayField(Args, TEXT("bones"), AlignBoneStrings);
    }

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Retargeter->GetPathName());
        R->SetStringField(TEXT("side"), RetargetSideToString(Side));
        R->SetStringField(TEXT("pose_name"), PoseName);
        R->SetStringField(TEXT("new_name"), NewPoseName);
        R->SetBoolField(TEXT("create"), bCreate);
        R->SetBoolField(TEXT("remove"), bRemove);
        R->SetBoolField(TEXT("duplicate"), bDuplicate);
        R->SetBoolField(TEXT("rename"), bRename);
        R->SetBoolField(TEXT("reset"), bReset);
        R->SetBoolField(TEXT("auto_align"), bAutoAlign);
        R->SetBoolField(TEXT("snap_to_ground"), bSnapToGround);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetIKRetargeterPose", "Sage: Set IK Retargeter Pose"));
    Retargeter->Modify();

    bool bModified = false;
    FString Action;

    if (bRemove)
    {
        if (PoseName.IsEmpty())
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'pose_name' for remove=true"));
        }
        if (!Controller->RemoveRetargetPose(FName(*PoseName), Side))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("retarget pose not found: %s"), *PoseName));
        }
        bSetCurrent = false;
        bModified = true;
        Action = TEXT("remove");
    }

    if (bDuplicate)
    {
        if (SourcePoseName.IsEmpty())
        {
            SourcePoseName = PoseName;
        }
        if (SourcePoseName.IsEmpty() || NewPoseName.IsEmpty())
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("duplicate=true requires pose_name/from_pose and new_name"));
        }
        const FName Duplicated = Controller->DuplicateRetargetPose(FName(*SourcePoseName), FName(*NewPoseName), Side);
        if (Duplicated == NAME_None)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("retarget pose not found: %s"), *SourcePoseName));
        }
        PoseName = Duplicated.ToString();
        bSetCurrent = true;
        bModified = true;
        Action = TEXT("duplicate");
    }

    if (bRename)
    {
        if (OldPoseName.IsEmpty())
        {
            OldPoseName = PoseName;
        }
        if (OldPoseName.IsEmpty() || NewPoseName.IsEmpty())
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("rename=true requires pose_name/old_name and new_name"));
        }
        if (!Controller->RenameRetargetPose(FName(*OldPoseName), FName(*NewPoseName), Side))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("retarget pose not found: %s"), *OldPoseName));
        }
        PoseName = NewPoseName;
        bModified = true;
        Action = TEXT("rename");
    }

    if (bCreate)
    {
        if (PoseName.IsEmpty())
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'pose_name' for create=true"));
        }
        PoseName = Controller->CreateRetargetPose(FName(*PoseName), Side).ToString();
        bSetCurrent = true;
        bModified = true;
        Action = TEXT("create");
    }

    if (!PoseName.IsEmpty() && bSetCurrent)
    {
        if (!Controller->SetCurrentRetargetPose(FName(*PoseName), Side))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("retarget pose not found: %s"), *PoseName));
        }
        bModified = true;
    }

    if (bReset)
    {
        const FName PoseToReset = PoseName.IsEmpty()
            ? Controller->GetCurrentRetargetPoseName(Side)
            : FName(*PoseName);
        TArray<FName> ResetBones;
        for (const FString& Bone : ResetBoneStrings)
        {
            if (!Bone.IsEmpty())
            {
                ResetBones.Add(FName(*Bone));
            }
        }
        Controller->ResetRetargetPose(PoseToReset, ResetBones, Side);
        bModified = true;
        Action = Action.IsEmpty() ? TEXT("reset") : Action;
    }

    FVector RootOffset;
    if (ReadVectorArray(Args, TEXT("root_offset"), RootOffset))
    {
        Controller->SetRootOffsetInRetargetPose(RootOffset, Side);
        bModified = true;
    }

    int32 RotationCount = 0;
    const TArray<TSharedPtr<FJsonValue>>* RotationValues = nullptr;
    if (Args->TryGetArrayField(TEXT("bone_rotations"), RotationValues) && RotationValues)
    {
        for (const TSharedPtr<FJsonValue>& Value : *RotationValues)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
            {
                continue;
            }
            FString BoneName;
            if (!(*Obj)->TryGetStringField(TEXT("bone"), BoneName) || BoneName.IsEmpty())
            {
                continue;
            }
            FQuat Rotation = FQuat::Identity;
            FRotator Rotator;
            if (!ReadQuatArray(*Obj, TEXT("quaternion"), Rotation))
            {
                if (ReadRotatorArray(*Obj, TEXT("rotation"), Rotator))
                {
                    Rotation = Rotator.Quaternion();
                }
            }
            Controller->SetRotationOffsetForRetargetPoseBone(FName(*BoneName), Rotation, Side);
            ++RotationCount;
            bModified = true;
        }
    }

    if (bAutoAlign)
    {
        FString MethodString = TEXT("chain_to_chain");
        Args->TryGetStringField(TEXT("align_method"), MethodString);
        ERetargetAutoAlignMethod Method = ERetargetAutoAlignMethod::ChainToChain;
        if (!ParseRetargetAutoAlignMethod(MethodString, Method))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("invalid align_method: %s"), *MethodString));
        }
        TArray<FName> AlignBones;
        for (const FString& Bone : AlignBoneStrings)
        {
            if (!Bone.IsEmpty())
            {
                AlignBones.Add(FName(*Bone));
            }
        }
        if (AlignBones.Num() > 0)
        {
            Controller->AutoAlignBones(AlignBones, Method, Side);
        }
        else
        {
            Controller->AutoAlignAllBones(Side, Method);
        }
        bModified = true;
        Action = Action.IsEmpty() ? TEXT("auto_align") : Action;
    }

    if (bSnapToGround)
    {
        FString ReferenceBone;
        TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("reference_bone"), TEXT("ground_bone"), TEXT("bone") }, ReferenceBone);
        FName ReferenceBoneName = ReferenceBone.IsEmpty()
            ? Controller->GetPelvisBone(Side)
            : FName(*ReferenceBone);
        Controller->SnapBoneToGround(ReferenceBoneName, Side);
        bModified = true;
        Action = Action.IsEmpty() ? TEXT("snap_to_ground") : Action;
    }

    if (bModified)
    {
        Retargeter->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetStringField(TEXT("side"), RetargetSideToString(Side));
    R->SetStringField(TEXT("action"), Action);
    R->SetStringField(TEXT("current_pose"), Controller->GetCurrentRetargetPoseName(Side).ToString());
    R->SetField(TEXT("root_offset"), detail::Vec3ToJson(Controller->GetRootOffsetInRetargetPose(Side)));
    R->SetNumberField(TEXT("bone_rotation_count"), RotationCount);
    R->SetBoolField(TEXT("modified"), bModified);
    TrySaveLoadedAssetIfRequested(Retargeter, bSave && bModified, R);
    AddIKRetargeterReadback(Retargeter, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.retarget_animations
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome RetargetAnimationsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(ResolveAsset(Path));
    if (!Retargeter)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UIKRetargeter: %s"), *Path));
    }

    const TArray<TSharedPtr<FJsonValue>>* AssetValues = nullptr;
    if (!Args->TryGetArrayField(TEXT("assets"), AssetValues) || !AssetValues || AssetValues->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing non-empty 'assets' array"));
    }

    TArray<TWeakObjectPtr<UObject>> AssetsToRetarget;
    TArray<TSharedPtr<FJsonValue>> InputAssets;
    for (const TSharedPtr<FJsonValue>& V : *AssetValues)
    {
        if (!V.IsValid() || V->Type != EJson::String) continue;
        const FString AssetPath = V->AsString();
        UObject* Asset = ResolveAsset(AssetPath);
        if (!Asset)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("asset not found: %s"), *AssetPath));
        }
        AssetsToRetarget.Add(Asset);
        InputAssets.Add(MakeShared<FJsonValueString>(Asset->GetPathName()));
    }

    FString SourceMeshPath, TargetMeshPath;
    USkeletalMesh* SourceMesh = nullptr;
    USkeletalMesh* TargetMesh = nullptr;
    if (Args->TryGetStringField(TEXT("source_mesh"), SourceMeshPath) && !SourceMeshPath.IsEmpty())
    {
        SourceMesh = Cast<USkeletalMesh>(ResolveAsset(SourceMeshPath));
        if (!SourceMesh)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a source USkeletalMesh: %s"), *SourceMeshPath));
        }
    }
    if (Args->TryGetStringField(TEXT("target_mesh"), TargetMeshPath) && !TargetMeshPath.IsEmpty())
    {
        TargetMesh = Cast<USkeletalMesh>(ResolveAsset(TargetMeshPath));
        if (!TargetMesh)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a target USkeletalMesh: %s"), *TargetMeshPath));
        }
    }

    bool bDryRun = false;
    bool bOverwrite = false;
    bool bIncludeReferenced = true;
    bool bUseSourcePath = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    Args->TryGetBoolField(TEXT("include_referenced_assets"), bIncludeReferenced);
    Args->TryGetBoolField(TEXT("use_source_path"), bUseSourcePath);

    FString DestinationPath = TEXT("/Game");
    Args->TryGetStringField(TEXT("destination_path"), DestinationPath);
    Args->TryGetStringField(TEXT("destination_package"), DestinationPath);
    if (!DestinationPath.StartsWith(TEXT("/")))
    {
        DestinationPath = TEXT("/Game/") + DestinationPath;
    }
    DestinationPath.RemoveFromEnd(TEXT("/"));

    EditorAnimUtils::FNameDuplicationRule NameRule;
    Args->TryGetStringField(TEXT("prefix"), NameRule.Prefix);
    Args->TryGetStringField(TEXT("suffix"), NameRule.Suffix);
    Args->TryGetStringField(TEXT("search"), NameRule.ReplaceFrom);
    Args->TryGetStringField(TEXT("replace"), NameRule.ReplaceTo);
    NameRule.FolderPath = DestinationPath;

    TArray<TSharedPtr<FJsonValue>> PlannedOutputs;
    TArray<TSharedPtr<FJsonValue>> Conflicts;
    for (TWeakObjectPtr<UObject> WeakAsset : AssetsToRetarget)
    {
        UObject* Asset = WeakAsset.Get();
        if (!Asset) continue;
        const FString Folder = bUseSourcePath
            ? FPackageName::GetLongPackagePath(Asset->GetPathName())
            : NameRule.FolderPath;
        const FString NewName = NameRule.Rename(Asset);
        const FString ObjectPath = Folder / NewName + TEXT(".") + NewName;
        PlannedOutputs.Add(MakeShared<FJsonValueString>(ObjectPath));
        if (!bOverwrite && ResolveAsset(ObjectPath))
        {
            auto Conflict = MakeShared<FJsonObject>();
            Conflict->SetStringField(TEXT("object_path"), ObjectPath);
            Conflict->SetStringField(TEXT("reason"), TEXT("exists_and_overwrite_false"));
            Conflicts.Add(MakeShared<FJsonValueObject>(Conflict));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Retargeter->GetPathName());
    R->SetArrayField(TEXT("input_assets"), InputAssets);
    R->SetArrayField(TEXT("planned_outputs"), PlannedOutputs);
    R->SetArrayField(TEXT("conflicts"), Conflicts);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("overwrite"), bOverwrite);
    R->SetStringField(TEXT("destination_path"), DestinationPath);
    if (bDryRun)
    {
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    if (Conflicts.Num() > 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("retarget output conflicts exist; pass overwrite=true or choose another destination"));
    }

    IAssetRegistry& Registry = GetAssetRegistry();
    TArray<FAssetData> BeforeAssets;
    Registry.GetAssetsByPath(FName(*DestinationPath), BeforeAssets, true);
    TSet<FName> BeforeObjectPaths;
    for (const FAssetData& AssetData : BeforeAssets)
    {
        BeforeObjectPaths.Add(AssetData.GetObjectPathString().IsEmpty()
            ? AssetData.PackageName
            : FName(*AssetData.GetObjectPathString()));
    }

    FIKRetargetBatchOperationContext Context;
    Context.AssetsToRetarget = AssetsToRetarget;
    Context.SourceMesh = SourceMesh;
    Context.TargetMesh = TargetMesh;
    Context.IKRetargetAsset = Retargeter;
    Context.NameRule = NameRule;
    Context.bUseSourcePath = bUseSourcePath;
    Context.bOverwriteExistingFiles = bOverwrite;
    Context.bIncludeReferencedAssets = bIncludeReferenced;

    UIKRetargetBatchOperation* Batch = NewObject<UIKRetargetBatchOperation>();
    Batch->AddToRoot();
    Batch->RunRetarget(Context);
    Batch->RemoveFromRoot();

    Registry.ScanPathsSynchronous({ DestinationPath }, true);
    TArray<FAssetData> AfterAssets;
    Registry.GetAssetsByPath(FName(*DestinationPath), AfterAssets, true);
    TArray<TSharedPtr<FJsonValue>> CreatedAssets;
    for (const FAssetData& AssetData : AfterAssets)
    {
        const FName ObjectPathName = AssetData.GetObjectPathString().IsEmpty()
            ? AssetData.PackageName
            : FName(*AssetData.GetObjectPathString());
        if (!BeforeObjectPaths.Contains(ObjectPathName) || bOverwrite)
        {
            AppendAssetDataJson(AssetData, CreatedAssets);
        }
    }
    R->SetBoolField(TEXT("modified"), true);
    R->SetArrayField(TEXT("created_assets"), CreatedAssets);
    R->SetNumberField(TEXT("created_asset_count"), CreatedAssets.Num());
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
    if (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
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
    // Without a structural-modify + recompile, runtime AnimBP keeps stale class
    // pointers that reference the old skeleton's bone container — leads to a
    // crash in pose-link evaluation on first instantiation.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);
    BP->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       BP->GetPathName());
    R->SetStringField(TEXT("skeleton"),   Skel->GetPathName());
    R->SetBoolField  (TEXT("recompiled"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_animation_asset_skeleton
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetAnimationAssetSkeletonImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }

    FString SkeletonPath;
    if (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    USkeleton* Skeleton = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("skeleton not found or not a USkeleton: %s"), *SkeletonPath));
    }

    bool bDryRun = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    bool bSave = true;
    Args->TryGetBoolField(TEXT("save"), bSave);

    TSet<FString> AssetPaths;
    auto AddAssetPath = [&AssetPaths](const FString& Raw)
    {
        if (!Raw.IsEmpty())
        {
            AssetPaths.Add(Raw);
        }
    };

    FString SinglePath;
    if (Args->TryGetStringField(TEXT("path"), SinglePath) && !SinglePath.IsEmpty())
    {
        UObject* MaybeAsset = ResolveAssetOrPackage(SinglePath);
        if (Cast<UAnimationAsset>(MaybeAsset))
        {
            AddAssetPath(FSoftObjectPath(MaybeAsset).ToString());
        }
        else
        {
            FARFilter Filter;
            Filter.PackagePaths.Add(FName(*PackagePathForObjectPath(SinglePath)));
            Filter.bRecursivePaths = true;
            Filter.bRecursiveClasses = true;
            Filter.ClassPaths.Add(UAnimationAsset::StaticClass()->GetClassPathName());
            FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            TArray<FAssetData> Found;
            ARM.Get().ScanPathsSynchronous({ PackagePathForObjectPath(SinglePath) }, /*bForceRescan=*/true);
            ARM.Get().GetAssets(Filter, Found);
            for (const FAssetData& Data : Found)
            {
                AddAssetPath(Data.GetSoftObjectPath().ToString());
            }
        }
    }

    FString Directory;
    if ((Args->TryGetStringField(TEXT("directory"), Directory)
            || Args->TryGetStringField(TEXT("folder"), Directory))
        && !Directory.IsEmpty())
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*PackagePathForObjectPath(Directory)));
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.ClassPaths.Add(UAnimationAsset::StaticClass()->GetClassPathName());
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Found;
        ARM.Get().ScanPathsSynchronous({ PackagePathForObjectPath(Directory) }, /*bForceRescan=*/true);
        ARM.Get().GetAssets(Filter, Found);
        for (const FAssetData& Data : Found)
        {
            AddAssetPath(Data.GetSoftObjectPath().ToString());
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
    if (Args->TryGetArrayField(TEXT("assets"), AssetsArray) && AssetsArray)
    {
        for (const TSharedPtr<FJsonValue>& Value : *AssetsArray)
        {
            AddAssetPath(Value.IsValid() ? Value->AsString() : FString());
        }
    }

    if (AssetPaths.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing animation asset target: pass path, directory/folder, or assets[]"));
    }

    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    if (bSave && !AssetSubsystem)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EditorAssetSubsystem unavailable for saving animation skeleton assignments"));
    }

    TArray<FString> SortedPaths;
    for (const FString& Path : AssetPaths)
    {
        SortedPaths.Add(Path);
    }
    SortedPaths.Sort();

    TArray<TSharedPtr<FJsonValue>> Repaired;
    TArray<TSharedPtr<FJsonValue>> AlreadyValid;
    TArray<TSharedPtr<FJsonValue>> Failed;
    int32 ModifiedCount = 0;
    int32 SavedCount = 0;

    FScopedTransaction Tx(LOCTEXT("SetAnimationAssetSkeleton", "Set Animation Asset Skeleton"));
    for (const FString& AssetPath : SortedPaths)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("asset"), AssetPath);
        Row->SetStringField(TEXT("requested_skeleton"), Skeleton->GetPathName());

        UAnimationAsset* Anim = Cast<UAnimationAsset>(ResolveAssetOrPackage(AssetPath));
        if (!Anim)
        {
            Row->SetStringField(TEXT("reason"), TEXT("not a UAnimationAsset or asset could not be loaded"));
            Failed.Add(MakeShared<FJsonValueObject>(Row));
            continue;
        }

        USkeleton* Before = Anim->GetSkeleton();
        Row->SetStringField(TEXT("class"), Anim->GetClass()->GetName());
        Row->SetStringField(TEXT("before_skeleton"), Before ? Before->GetPathName() : FString());
        const bool bAlready = Before == Skeleton;
        Row->SetBoolField(TEXT("already_valid"), bAlready);
        if (bAlready)
        {
            Row->SetStringField(TEXT("after_skeleton"), Skeleton->GetPathName());
            AlreadyValid.Add(MakeShared<FJsonValueObject>(Row));
            continue;
        }

        if (!bDryRun)
        {
            FProperty* SkeletonProperty = Anim->GetClass()->FindPropertyByName(FName(TEXT("Skeleton")));
            Anim->Modify();
            if (SkeletonProperty)
            {
                Anim->PreEditChange(SkeletonProperty);
            }
            Anim->SetSkeleton(Skeleton);
            if (SkeletonProperty)
            {
                FPropertyChangedEvent ChangeEvent(SkeletonProperty, EPropertyChangeType::ValueSet);
                Anim->PostEditChangeProperty(ChangeEvent);
            }
            else
            {
                Anim->PostEditChange();
            }
            Anim->MarkPackageDirty();
            ++ModifiedCount;
            if (bSave && AssetSubsystem)
            {
                const FString Package = PackagePathForObjectPath(FSoftObjectPath(Anim).ToString());
                const bool bSaved = AssetSubsystem->SaveAsset(Package, /*bOnlyIfIsDirty=*/false);
                Row->SetBoolField(TEXT("saved"), bSaved);
                if (bSaved)
                {
                    ++SavedCount;
                }
            }
        }

        USkeleton* After = bDryRun ? Skeleton : Anim->GetSkeleton();
        Row->SetStringField(TEXT("after_skeleton"), After ? After->GetPathName() : FString());
        Row->SetBoolField(TEXT("dry_run"), bDryRun);
        Row->SetBoolField(TEXT("repaired"), !bDryRun && After == Skeleton);
        if (bDryRun || After == Skeleton)
        {
            Repaired.Add(MakeShared<FJsonValueObject>(Row));
        }
        else
        {
            Row->SetStringField(TEXT("reason"), TEXT("UAnimationAsset::SetSkeleton did not persist on readback"));
            Failed.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    if (Failed.Num() > 0)
    {
        Tx.Cancel();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("save"), bSave);
    R->SetArrayField(TEXT("repaired_assets"), Repaired);
    R->SetArrayField(TEXT("already_valid_assets"), AlreadyValid);
    R->SetArrayField(TEXT("failed_assets"), Failed);
    R->SetNumberField(TEXT("repaired_count"), Repaired.Num());
    R->SetNumberField(TEXT("already_valid_count"), AlreadyValid.Num());
    R->SetNumberField(TEXT("failed_count"), Failed.Num());
    R->SetNumberField(TEXT("modified_count"), ModifiedCount);
    R->SetNumberField(TEXT("saved_count"), SavedCount);
    R->SetNumberField(TEXT("target_count"), SortedPaths.Num());
    R->SetBoolField(TEXT("success"), Failed.Num() == 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.bake_root_motion_from_bone
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome BakeRootMotionFromBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SourceBone, RootBoneName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("bone"), SourceBone) || SourceBone.IsEmpty())
    {
        Args->TryGetStringField(TEXT("source_bone"), SourceBone);
    }
    if (SourceBone.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone'"));
    }

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }
    USkeleton* Skel = Seq->GetSkeleton();
    const IAnimationDataModel* Model = Seq->GetDataModel();
    if (!Skel || !Model)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("sequence has no skeleton/data model"));
    }

    const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
    if (RefSkel.GetNum() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("skeleton has no reference bones"));
    }
    Args->TryGetStringField(TEXT("root_bone"), RootBoneName);
    if (RootBoneName.IsEmpty())
    {
        RootBoneName = RefSkel.GetBoneName(0).ToString();
    }

    const FName SourceBoneF(*SourceBone);
    const FName RootBoneF(*RootBoneName);
    if (RefSkel.FindBoneIndex(SourceBoneF) == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("source bone '%s' not found in skeleton"), *SourceBone));
    }
    const int32 RootBoneIndex = RefSkel.FindBoneIndex(RootBoneF);
    if (RootBoneIndex == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("root bone '%s' not found in skeleton"), *RootBoneName));
    }
    if (!Model->IsValidBoneTrackName(SourceBoneF))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("AnimSequence has no animated track for source bone '%s'"), *SourceBone));
    }

    bool bDryRun = false;
    bool bConfirmed = false;
    bool bRelative = true;
    bool bEnableRootMotion = true;
    bool bZeroSourceTranslation = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    Args->TryGetBoolField(TEXT("relative"), bRelative);
    Args->TryGetBoolField(TEXT("enable_root_motion"), bEnableRootMotion);
    Args->TryGetBoolField(TEXT("zero_source_translation"), bZeroSourceTranslation);
    if (!bDryRun && !bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("bake_root_motion_from_bone requires confirmed=true unless dry_run=true"));
    }

    TArray<FTransform> SourceTransforms;
    TArray<FTransform> ExistingRootTransforms;
    Model->GetBoneTrackTransforms(SourceBoneF, SourceTransforms);
    Model->GetBoneTrackTransforms(RootBoneF, ExistingRootTransforms);
    if (SourceTransforms.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("source bone track has no keys"));
    }

    const FTransform RefRoot = RefSkel.GetRefBonePose().IsValidIndex(RootBoneIndex)
        ? RefSkel.GetRefBonePose()[RootBoneIndex]
        : FTransform::Identity;
    const FTransform SourceBase = SourceTransforms[0];

    TArray<FVector> RootPositions;
    TArray<FQuat> RootRotations;
    TArray<FVector> RootScales;
    TArray<FVector> SourcePositions;
    TArray<FQuat> SourceRotations;
    TArray<FVector> SourceScales;
    RootPositions.Reserve(SourceTransforms.Num());
    RootRotations.Reserve(SourceTransforms.Num());
    RootScales.Reserve(SourceTransforms.Num());
    SourcePositions.Reserve(SourceTransforms.Num());
    SourceRotations.Reserve(SourceTransforms.Num());
    SourceScales.Reserve(SourceTransforms.Num());

    for (int32 I = 0; I < SourceTransforms.Num(); ++I)
    {
        const FTransform ExistingRoot = ExistingRootTransforms.IsValidIndex(I) ? ExistingRootTransforms[I] : RefRoot;
        FTransform Baked = bRelative
            ? SourceTransforms[I].GetRelativeTransform(SourceBase)
            : SourceTransforms[I];
        Baked.SetScale3D(ExistingRoot.GetScale3D());

        RootPositions.Add(Baked.GetTranslation());
        RootRotations.Add(Baked.GetRotation());
        RootScales.Add(Baked.GetScale3D());

        FTransform SourceOut = SourceTransforms[I];
        if (bZeroSourceTranslation)
        {
            SourceOut.SetTranslation(FVector::ZeroVector);
        }
        SourcePositions.Add(SourceOut.GetTranslation());
        SourceRotations.Add(SourceOut.GetRotation());
        SourceScales.Add(SourceOut.GetScale3D());
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("source_bone"), SourceBone);
    R->SetStringField(TEXT("root_bone"), RootBoneName);
    R->SetNumberField(TEXT("key_count"), SourceTransforms.Num());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("relative"), bRelative);
    R->SetBoolField(TEXT("zero_source_translation"), bZeroSourceTranslation);
    R->SetField(TEXT("first_source_transform"), TransformToJsonValue(SourceTransforms[0]));
    R->SetField(TEXT("last_source_transform"), TransformToJsonValue(SourceTransforms.Last()));
    if (bDryRun)
    {
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    IAnimationDataController& Controller = Seq->GetController();
    {
        IAnimationDataController::FScopedBracket Bracket(&Controller,
            LOCTEXT("SageBakeRootMotion", "Sage: Bake Root Motion From Bone"));
        Seq->Modify();
        if (!Model->IsValidBoneTrackName(RootBoneF))
        {
            Controller.AddBoneCurve(RootBoneF, false);
        }
        if (!Controller.SetBoneTrackKeys(RootBoneF, RootPositions, RootRotations, RootScales, false))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to write root bone track"));
        }
        if (bZeroSourceTranslation)
        {
            if (!Controller.SetBoneTrackKeys(SourceBoneF, SourcePositions, SourceRotations, SourceScales, false))
            {
                return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to update source bone track"));
            }
        }
    }

    if (bEnableRootMotion)
    {
        Seq->bEnableRootMotion = true;
    }
    Seq->RefreshCacheData();
    Seq->MarkPackageDirty();

    R->SetBoolField(TEXT("modified"), true);
    R->SetBoolField(TEXT("root_motion_enabled"), Seq->bEnableRootMotion);
    AddAnimSequenceTrackReadback(R, Seq, RootBoneName, 0, RootPositions.Num() - 1);
    AddRootMotionSummary(R, Seq);
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
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SchemaPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("schema"), SchemaPath) || SchemaPath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("schema_path"), SchemaPath);
    }
    if (SchemaPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'schema'"));
    }

    UObject* DB = ResolveAsset(Path);
    if (!DB || !DB->GetClass()->GetPathName().Contains(TEXT("PoseSearchDatabase")))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a PoseSearchDatabase: %s"), *Path));
    }

    UObject* Schema = nullptr;
    FString Error;
    if (!ResolveExpectedObject(SchemaPath, TEXT("/Script/PoseSearch.PoseSearchSchema"), Schema, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetPoseSearchSchema", "Sage: Set PoseSearch Schema"));
    DB->Modify();
    if (!SetReflectedProperty(DB, TEXT("Schema"), JsonStringValue(SchemaPath)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("failed to set PoseSearch Schema property"));
    }
    DB->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), DB->GetPathName());
    R->SetStringField(TEXT("schema"), Schema ? Schema->GetPathName() : FString());
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_pose_search_sequence
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddPoseSearchSequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SequencePath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("sequence"), SequencePath) || SequencePath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("sequence_path"), SequencePath);
    }
    if (SequencePath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'sequence'"));
    }

    UObject* DB = ResolveAsset(Path);
    if (!DB || !DB->GetClass()->GetPathName().Contains(TEXT("PoseSearchDatabase")))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a PoseSearchDatabase: %s"), *Path));
    }
    UObject* Sequence = ResolveAsset(SequencePath);
    if (!Sequence || !Sequence->IsA<UAnimationAsset>())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimationAsset: %s"), *SequencePath));
    }

    FArrayProperty* AssetsProp = FindFProperty<FArrayProperty>(DB->GetClass(), TEXT("DatabaseAnimationAssets"));
    FStructProperty* EntryStruct = AssetsProp ? CastField<FStructProperty>(AssetsProp->Inner) : nullptr;
    if (!AssetsProp || !EntryStruct)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("PoseSearch DatabaseAnimationAssets property not found"));
    }

    FScopedTransaction Tx(LOCTEXT("SageAddPoseSearchSequence", "Sage: Add PoseSearch Sequence"));
    DB->Modify();
    FScriptArrayHelper Helper(AssetsProp, AssetsProp->ContainerPtrToValuePtr<void>(DB));
    const int32 ExistingCount = Helper.Num();
    int32 ExistingIndex = INDEX_NONE;
    FProperty* AnimAssetField = EntryStruct->Struct->FindPropertyByName(FName(TEXT("AnimAsset")));
    for (int32 Index = 0; Index < ExistingCount; ++Index)
    {
        void* Entry = Helper.GetRawPtr(Index);
        if (AnimAssetField)
        {
            TSharedPtr<FJsonValue> Existing = detail::GetPropertyValueAtPtr(
                AnimAssetField, AnimAssetField->ContainerPtrToValuePtr<void>(Entry));
            if (Existing.IsValid() && Existing->Type == EJson::String && Existing->AsString() == Sequence->GetPathName())
            {
                ExistingIndex = Index;
                break;
            }
        }
    }

    int32 EntryIndex = ExistingIndex;
    bool bAdded = false;
    if (EntryIndex == INDEX_NONE)
    {
        EntryIndex = Helper.AddValue();
        EntryStruct->InitializeValue(Helper.GetRawPtr(EntryIndex));
        bAdded = true;
    }

    void* Entry = Helper.GetRawPtr(EntryIndex);
    if (!SetStructField(EntryStruct->Struct, Entry, TEXT("AnimAsset"), JsonStringValue(Sequence->GetPathName())))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to set PoseSearch AnimAsset field"));
    }

    bool bEnabled = true;
    if (Args->TryGetBoolField(TEXT("enabled"), bEnabled))
    {
        SetStructField(EntryStruct->Struct, Entry, TEXT("bEnabled"), JsonBoolValue(bEnabled));
    }

    NotifyObjectPropertyChanged(DB, AssetsProp);
    DB->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), DB->GetPathName());
    R->SetStringField(TEXT("sequence"), Sequence->GetPathName());
    R->SetNumberField(TEXT("index"), EntryIndex);
    R->SetBoolField(TEXT("added"), bAdded);
    R->SetNumberField(TEXT("count"), Helper.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.build_pose_search_index
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome BuildPoseSearchIndexImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UObject* DB = ResolveAsset(Path);
    if (!DB || !DB->GetClass()->GetPathName().Contains(TEXT("PoseSearchDatabase")))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a PoseSearchDatabase: %s"), *Path));
    }

    DB->Modify();
    DB->PostEditChange();
    DB->BeginCacheForCookedPlatformData(nullptr);
    DB->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), DB->GetPathName());
    R->SetBoolField(TEXT("build_requested"), true);
    R->SetBoolField(TEXT("cache_loaded"), DB->IsCachedCookedPlatformDataLoaded(nullptr));
    R->SetStringField(TEXT("note"), TEXT("PoseSearch derived-data rebuild requested through UObject cooked-platform cache API"));
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

// ===========================================================================
// Cluster A — AnimGraph node creation core (Phase 4-r6)
// Generic primitives that the convenience cluster (B) and state-machine
// authoring (C) build upon. Every handler accepts `path` (UAnimBlueprint)
// and an optional `graph_name` to disambiguate root AnimGraph from a state
// machine sub-graph.
// ===========================================================================

// Resolve a target UEdGraph inside an AnimBP given an optional graph name.
// Empty / "AnimGraph" → root AnimGraph. Otherwise look up:
//   1) state machine sub-graph by name (UAnimationStateMachineGraph)
//   2) FunctionGraphs entry by FName
//   3) state's BoundGraph by state name (UAnimStateNodeBase::GetStateName()
//      OR BoundGraph FName) — lets callers target a state's inner pose graph
//      directly instead of round-tripping through state_id.
UEdGraph* ResolveAnimGraphTarget(UAnimBlueprint* AnimBP, const FString& GraphName)
{
    if (!AnimBP) return nullptr;
    if (GraphName.IsEmpty() || GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
    {
        return FindAnimGraph(AnimBP);
    }
    if (UAnimationStateMachineGraph* SM = FindStateMachineGraph(AnimBP, FName(*GraphName)))
    {
        return SM;
    }
    for (UEdGraph* G : AnimBP->FunctionGraphs)
    {
        if (G && G->GetFName() == FName(*GraphName)) return G;
    }
    // State bound-graph fallback — walk every state machine, every state,
    // match on either the BoundGraph's FName or the engine's GetStateName()
    // override (which the Persona graph editor displays).
    {
        const FName Wanted(*GraphName);
        for (UAnimationStateMachineGraph* SMGraph : CollectStateMachineGraphs(AnimBP))
        {
            if (!SMGraph) continue;
            for (UEdGraphNode* SN : SMGraph->Nodes)
            {
                UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(SN);
                if (!State) continue;
                UEdGraph* Bound = GetStateBoundGraph(State);
                if (!Bound) continue;
                if (Bound->GetFName() == Wanted
                    || State->NodeGuid.ToString(EGuidFormats::Digits).Equals(GraphName, ESearchCase::IgnoreCase)
                    || GetStateDisplayName(State).Equals(GraphName, ESearchCase::IgnoreCase))
                {
                    return Bound;
                }
            }
        }
    }
    {
        const FName Wanted(*GraphName);
        for (UAnimationStateMachineGraph* SMGraph : CollectStateMachineGraphs(AnimBP))
        {
            if (!SMGraph) continue;
            for (UEdGraphNode* SN : SMGraph->Nodes)
            {
                UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(SN);
                if (!Transition || !Transition->BoundGraph) continue;
                if (Transition->BoundGraph->GetFName() == Wanted
                    || Transition->NodeGuid.ToString(EGuidFormats::Digits).Equals(GraphName, ESearchCase::IgnoreCase))
                {
                    return Transition->BoundGraph;
                }
            }
        }
    }
    // AnimLayerInterface override graphs (Cluster G fix — Lyra Sage Gap #25).
    // Override graphs live in AnimBP->ImplementedInterfaces[].Graphs, NOT in
    // FunctionGraphs. Without this fallback Cluster A/D node injection
    // (animation.add_animgraph_node graph_name="<override>", add_sequence_player,
    // add_state_machine_node, connect_pose_pin, etc.) returns "graph not found"
    // for any graph spawned by animation.add_layer_function_override.
    {
        const FName Wanted(*GraphName);
        for (FBPInterfaceDescription& Impl : AnimBP->ImplementedInterfaces)
        {
            for (UEdGraph* G : Impl.Graphs)
            {
                if (G && G->GetFName() == Wanted) return G;
            }
        }
    }
    return nullptr;
}

// Find an arbitrary UEdGraphNode by FGuid (covers UAnimGraphNode_Base + others).
UEdGraphNode* FindGraphNodeByGuid(UEdGraph* Graph, const FString& IdString)
{
    if (!Graph) return nullptr;
    FGuid Id;
    if (!ParseNodeGuid(IdString, Id)) return nullptr;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (N && N->NodeGuid == Id) return N;
    }
    return nullptr;
}

// Locate an EdGraph pin by name on a node (handles direction filter).
UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Dir)
{
    if (!Node) return nullptr;
    const FName N(*PinName);
    for (UEdGraphPin* P : Node->Pins)
    {
        if (P && P->Direction == Dir && P->PinName == N) return P;
    }
    return nullptr;
}

// Reach the inner FAnimNode_* struct on a UAnimGraphNode_Base. Returns the
// FStructProperty + container pointer for use with reflection writes.
bool GetAnimNodeStructTarget(UAnimGraphNode_Base* AnimNode, FStructProperty*& OutProp,
                             void*& OutPtr)
{
    OutProp = nullptr; OutPtr = nullptr;
    if (!AnimNode) return false;
    UClass* Cls = AnimNode->GetClass();
    for (TFieldIterator<FStructProperty> It(Cls); It; ++It)
    {
        FStructProperty* SP = *It;
        if (!SP || !SP->Struct) continue;
        // Match on convention: most UAnimGraphNode_X expose `FAnimNode_X Node`.
        // Additionally require the struct to be a FAnimNode_Base descendant —
        // state machine, transition, and link nodes also expose a `Node`
        // FStructProperty but with a non-AnimNode shape. Filtering on the
        // struct hierarchy avoids landing on those by mistake.
        if (SP->GetFName() == FName(TEXT("Node"))
            && SP->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
        {
            OutProp = SP;
            OutPtr  = SP->ContainerPtrToValuePtr<void>(AnimNode);
            return true;
        }
    }
    return false;
}

const UStruct* GetAnimBindingSourceRoot(const UAnimBlueprint* AnimBP)
{
    if (!AnimBP) return nullptr;
    if (AnimBP->SkeletonGeneratedClass) return AnimBP->SkeletonGeneratedClass;
    return AnimBP->GeneratedClass;
}

int32 FindOptionalPinIndexForProperty(const UAnimGraphNode_Base* AnimNode, FName PropertyName)
{
    if (!AnimNode) return INDEX_NONE;
    for (int32 Index = 0; Index < AnimNode->ShowPinForProperties.Num(); ++Index)
    {
        if (AnimNode->ShowPinForProperties[Index].PropertyName == PropertyName)
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

TArray<FOptionalPinFromProperty>* GetCustomPinProperties(UAnimGraphNode_Base* AnimNode)
{
    UAnimGraphNode_CustomProperty* CustomNode = Cast<UAnimGraphNode_CustomProperty>(AnimNode);
    if (!CustomNode) return nullptr;

    FArrayProperty* CustomPinsProp = FindFProperty<FArrayProperty>(
        UAnimGraphNode_CustomProperty::StaticClass(), TEXT("CustomPinProperties"));
    if (!CustomPinsProp)
    {
        return nullptr;
    }
    return CustomPinsProp->ContainerPtrToValuePtr<TArray<FOptionalPinFromProperty>>(CustomNode);
}

const TArray<FOptionalPinFromProperty>* GetCustomPinProperties(const UAnimGraphNode_Base* AnimNode)
{
    return GetCustomPinProperties(const_cast<UAnimGraphNode_Base*>(AnimNode));
}

bool IsPropertyOnAnimNodeStruct(const UAnimGraphNode_Base* AnimNode, const FProperty* Property)
{
    if (!AnimNode || !Property) return false;

    const FStructProperty* NodeStructProperty = AnimNode->GetFNodeProperty();
    const UScriptStruct* OwnerStruct = Property->GetOwner<UScriptStruct>();
    return NodeStructProperty && NodeStructProperty->Struct && OwnerStruct
        && NodeStructProperty->Struct->IsChildOf(OwnerStruct);
}

struct FResolvedAnimNodePinBinding
{
    FName PinName = NAME_None;
    FName BindingName = NAME_None;
    FProperty* PinProperty = nullptr;
    int32 OptionalPinIndex = INDEX_NONE;
    bool bOptionalPin = false;
    bool bCustomPropertyPin = false;
    bool bPinVisible = false;
};

bool ResolveAnimNodePinBinding(const UAnimGraphNode_Base* AnimNode,
                               FName PinName,
                               FResolvedAnimNodePinBinding& OutInfo)
{
    OutInfo = FResolvedAnimNodePinBinding();
    OutInfo.PinName = PinName;
    if (!AnimNode || PinName == NAME_None)
    {
        return false;
    }

    FProperty* PinProperty = AnimNode->GetPinProperty(PinName);
    if (!PinProperty)
    {
        return false;
    }

    OutInfo.PinProperty = PinProperty;
    OutInfo.bCustomPropertyPin =
        AnimNode->IsA<UAnimGraphNode_CustomProperty>()
        && !IsPropertyOnAnimNodeStruct(AnimNode, PinProperty);

    if (OutInfo.bCustomPropertyPin)
    {
        const UAnimGraphNode_CustomProperty* CustomNode =
            Cast<UAnimGraphNode_CustomProperty>(AnimNode);
        OutInfo.BindingName = CustomNode
            ? FName(*CustomNode->GetPinTargetVariableName(PinName))
            : PinName;

        const TArray<FOptionalPinFromProperty>* CustomPins = GetCustomPinProperties(AnimNode);
        if (CustomPins)
        {
            OutInfo.OptionalPinIndex = CustomPins->IndexOfByPredicate(
                [PinName](const FOptionalPinFromProperty& OptionalPin)
                {
                    return OptionalPin.PropertyName == PinName;
                });
        }
        OutInfo.bOptionalPin = OutInfo.OptionalPinIndex != INDEX_NONE;
        OutInfo.bPinVisible =
            CustomPins && CustomPins->IsValidIndex(OutInfo.OptionalPinIndex)
                ? (*CustomPins)[OutInfo.OptionalPinIndex].bShowPin
                : AnimNode->FindPin(PinName, EGPD_Input) != nullptr;
        return true;
    }

    OutInfo.BindingName = PinName;
    OutInfo.OptionalPinIndex = FindOptionalPinIndexForProperty(AnimNode, PinName);
    OutInfo.bOptionalPin = OutInfo.OptionalPinIndex != INDEX_NONE;
    OutInfo.bPinVisible =
        AnimNode->ShowPinForProperties.IsValidIndex(OutInfo.OptionalPinIndex)
            ? AnimNode->ShowPinForProperties[OutInfo.OptionalPinIndex].bShowPin
            : AnimNode->FindPin(PinName, EGPD_Input) != nullptr;
    return true;
}

bool SetResolvedAnimNodePinVisible(UAnimGraphNode_Base* AnimNode,
                                   const FResolvedAnimNodePinBinding& PinInfo,
                                   bool bReconstruct)
{
    if (!AnimNode || !PinInfo.bOptionalPin || PinInfo.OptionalPinIndex == INDEX_NONE)
    {
        return false;
    }

    if (PinInfo.bCustomPropertyPin)
    {
        TArray<FOptionalPinFromProperty>* CustomPins = GetCustomPinProperties(AnimNode);
        if (!CustomPins || !CustomPins->IsValidIndex(PinInfo.OptionalPinIndex))
        {
            return false;
        }

        FOptionalPinFromProperty& OptionalPin = (*CustomPins)[PinInfo.OptionalPinIndex];
        if (OptionalPin.bShowPin)
        {
            return false;
        }

        AnimNode->Modify();
        OptionalPin.bShowPin = true;
        if (bReconstruct)
        {
            AnimNode->ReconstructNode();
        }
        return true;
    }

    if (!AnimNode->ShowPinForProperties.IsValidIndex(PinInfo.OptionalPinIndex)
        || AnimNode->ShowPinForProperties[PinInfo.OptionalPinIndex].bShowPin)
    {
        return false;
    }

    AnimNode->SetPinVisibility(/*bInVisible=*/true, PinInfo.OptionalPinIndex);
    return true;
}

void ExposeAllCustomPropertyPins(UAnimGraphNode_Base* AnimNode,
                                 TArray<FName>& OutVisiblePins,
                                 TArray<FName>& OutNewlyExposedPins)
{
    OutVisiblePins.Reset();
    OutNewlyExposedPins.Reset();

    TArray<FOptionalPinFromProperty>* CustomPins = GetCustomPinProperties(AnimNode);
    if (!AnimNode || !CustomPins)
    {
        return;
    }

    bool bChanged = false;
    for (int32 Index = 0; Index < CustomPins->Num(); ++Index)
    {
        FOptionalPinFromProperty& OptionalPin = (*CustomPins)[Index];
        if (OptionalPin.PropertyName == NAME_None)
        {
            continue;
        }

        FResolvedAnimNodePinBinding PinInfo;
        if (!ResolveAnimNodePinBinding(AnimNode, OptionalPin.PropertyName, PinInfo)
            || !PinInfo.bCustomPropertyPin)
        {
            continue;
        }

        OutVisiblePins.Add(OptionalPin.PropertyName);
        if (!OptionalPin.bShowPin)
        {
            OptionalPin.bShowPin = true;
            OutNewlyExposedPins.Add(OptionalPin.PropertyName);
            bChanged = true;
        }
    }

    if (bChanged)
    {
        AnimNode->Modify();
        AnimNode->ReconstructNode();
    }
}

UObject* GetAnimNodeBindingObject(const UAnimGraphNode_Base* AnimNode)
{
    if (!AnimNode) return nullptr;
    const FObjectPropertyBase* BindingProp = FindFProperty<FObjectPropertyBase>(
        UAnimGraphNode_Base::StaticClass(), TEXT("Binding"));
    return BindingProp ? BindingProp->GetObjectPropertyValue_InContainer(AnimNode) : nullptr;
}

UObject* GetOrCreateAnimNodeBindingObject(UAnimBlueprint* AnimBP,
                                          UAnimGraphNode_Base* AnimNode,
                                          FString& OutError)
{
    OutError.Reset();
    if (!AnimBP || !AnimNode)
    {
        OutError = TEXT("invalid AnimBP or AnimGraph node");
        return nullptr;
    }

    FObjectPropertyBase* BindingProp = FindFProperty<FObjectPropertyBase>(
        UAnimGraphNode_Base::StaticClass(), TEXT("Binding"));
    if (!BindingProp)
    {
        OutError = TEXT("UAnimGraphNode_Base.Binding property not found");
        return nullptr;
    }

    if (UObject* Existing = BindingProp->GetObjectPropertyValue_InContainer(AnimNode))
    {
        return Existing;
    }

    UClass* BindingClass = AnimBP->GetDefaultBindingClass();
    if (!BindingClass)
    {
        BindingClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimGraphNodeBinding_Base"));
    }
    if (!BindingClass)
    {
        BindingClass = LoadObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimGraphNodeBinding_Base"));
    }
    if (!BindingClass || BindingClass->HasAnyClassFlags(CLASS_Abstract))
    {
        OutError = TEXT("UE 5.7 default AnimGraph node binding class could not be resolved");
        return nullptr;
    }

    UObject* BindingObj = NewObject<UObject>(AnimNode, BindingClass, NAME_None, RF_Transactional);
    if (!BindingObj)
    {
        OutError = FString::Printf(TEXT("could not instantiate binding class %s"),
                                   *BindingClass->GetPathName());
        return nullptr;
    }
    BindingProp->SetObjectPropertyValue_InContainer(AnimNode, BindingObj);
    return BindingObj;
}

bool GetAnimNodeBindingMap(UObject* BindingObj,
                           FMapProperty*& OutMapProp,
                           FStructProperty*& OutValueStruct,
                           FString& OutError)
{
    OutMapProp = nullptr;
    OutValueStruct = nullptr;
    OutError.Reset();

    if (!BindingObj)
    {
        OutError = TEXT("binding object is null");
        return false;
    }

    OutMapProp = CastField<FMapProperty>(
        BindingObj->GetClass()->FindPropertyByName(TEXT("PropertyBindings")));
    if (!OutMapProp)
    {
        OutError = FString::Printf(TEXT("%s has no PropertyBindings map"),
                                   *BindingObj->GetClass()->GetName());
        return false;
    }
    if (!OutMapProp->KeyProp || !OutMapProp->KeyProp->IsA<FNameProperty>())
    {
        OutError = TEXT("PropertyBindings key is not FName");
        return false;
    }

    OutValueStruct = CastField<FStructProperty>(OutMapProp->ValueProp);
    if (!OutValueStruct
        || OutValueStruct->Struct != FAnimGraphNodePropertyBinding::StaticStruct())
    {
        OutError = TEXT("PropertyBindings value is not FAnimGraphNodePropertyBinding");
        return false;
    }
    return true;
}

void RemoveBindingMapEntries(UObject* BindingObj, FMapProperty* MapProp, FName BindingName)
{
    if (!BindingObj || !MapProp) return;

    void* MapPtr = MapProp->ContainerPtrToValuePtr<void>(BindingObj);
    FScriptMapHelper Helper(MapProp, MapPtr);
    FNameProperty* KeyProp = CastFieldChecked<FNameProperty>(MapProp->KeyProp);

    bool bRemoved = false;
    for (int32 InternalIndex = Helper.GetMaxIndex() - 1; InternalIndex >= 0; --InternalIndex)
    {
        if (!Helper.IsValidIndex(InternalIndex)) continue;

        const FName ExistingName = KeyProp->GetPropertyValue(Helper.GetKeyPtr(InternalIndex));
        if (ExistingName == BindingName || FName(ExistingName, 0) == BindingName)
        {
            Helper.RemoveAt(InternalIndex);
            bRemoved = true;
        }
    }
    if (bRemoved)
    {
        Helper.Rehash();
    }
}

bool AddBindingMapEntry(UObject* BindingObj,
                        FMapProperty* MapProp,
                        FStructProperty* ValueStruct,
                        FName BindingName,
                        const FAnimGraphNodePropertyBinding& Binding,
                        FString& OutError)
{
    OutError.Reset();
    if (!BindingObj || !MapProp || !ValueStruct)
    {
        OutError = TEXT("invalid PropertyBindings map");
        return false;
    }

    RemoveBindingMapEntries(BindingObj, MapProp, BindingName);

    void* MapPtr = MapProp->ContainerPtrToValuePtr<void>(BindingObj);
    FScriptMapHelper Helper(MapProp, MapPtr);
    FNameProperty* KeyProp = CastFieldChecked<FNameProperty>(MapProp->KeyProp);

    const int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
    KeyProp->SetPropertyValue(Helper.GetKeyPtr(NewIndex), BindingName);
    ValueStruct->CopyCompleteValue(Helper.GetValuePtr(NewIndex), &Binding);
    Helper.Rehash();
    return true;
}

bool AppendBindingPathSegments(const FString& Raw, TArray<FString>& OutPath)
{
    FString Trimmed = Raw;
    Trimmed.TrimStartAndEndInline();
    if (Trimmed.IsEmpty()) return false;

    TArray<FString> Parts;
    Trimmed.ParseIntoArray(Parts, TEXT("."), /*CullEmpty=*/true);
    if (Parts.IsEmpty()) return false;

    for (FString& Part : Parts)
    {
        Part.TrimStartAndEndInline();
        if (Part.IsEmpty()) return false;
        OutPath.Add(Part);
    }
    return true;
}

bool ParseAnimNodeBindingExpression(const TSharedPtr<FJsonObject>& Args,
                                    TArray<FString>& OutPath,
                                    FString& OutError)
{
    OutPath.Reset();
    OutError.Reset();
    if (!Args.IsValid())
    {
        OutError = TEXT("missing args");
        return false;
    }

    if (TSharedPtr<FJsonValue> Expr = Args->TryGetField(TEXT("expression")))
    {
        if (Expr->Type == EJson::String)
        {
            if (!AppendBindingPathSegments(Expr->AsString(), OutPath))
            {
                OutError = TEXT("'expression' string must be a variable path like FlightLean.X");
                return false;
            }
            return true;
        }
        if (Expr->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> ExprObj = Expr->AsObject();
            if (!ExprObj.IsValid())
            {
                OutError = TEXT("'expression' object is invalid");
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
            if (ExprObj->TryGetArrayField(TEXT("path"), PathValues))
            {
                for (const TSharedPtr<FJsonValue>& SegmentValue : *PathValues)
                {
                    if (!SegmentValue.IsValid() || SegmentValue->Type != EJson::String
                        || !AppendBindingPathSegments(SegmentValue->AsString(), OutPath))
                    {
                        OutError = TEXT("'expression.path' must contain non-empty string segments");
                        return false;
                    }
                }
                if (!OutPath.IsEmpty()) return true;
            }

            FString VarName;
            if (!ExprObj->TryGetStringField(TEXT("var"), VarName))
            {
                ExprObj->TryGetStringField(TEXT("variable"), VarName);
            }
            if (VarName.IsEmpty() || !AppendBindingPathSegments(VarName, OutPath))
            {
                OutError = TEXT("'expression' object must include 'var' or 'path'");
                return false;
            }

            FString MemberName;
            if (ExprObj->TryGetStringField(TEXT("member"), MemberName)
                && !MemberName.IsEmpty()
                && !AppendBindingPathSegments(MemberName, OutPath))
            {
                OutError = TEXT("'expression.member' must be a non-empty path segment");
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
            if (ExprObj->TryGetArrayField(TEXT("members"), Members))
            {
                for (const TSharedPtr<FJsonValue>& MemberValue : *Members)
                {
                    if (!MemberValue.IsValid() || MemberValue->Type != EJson::String
                        || !AppendBindingPathSegments(MemberValue->AsString(), OutPath))
                    {
                        OutError = TEXT("'expression.members' must contain non-empty string segments");
                        return false;
                    }
                }
            }
            return !OutPath.IsEmpty();
        }

        OutError = TEXT("'expression' must be a string or object");
        return false;
    }

    FString VarName;
    if (Args->TryGetStringField(TEXT("variable"), VarName)
        && !VarName.IsEmpty()
        && AppendBindingPathSegments(VarName, OutPath))
    {
        return true;
    }

    OutError = TEXT("missing 'expression' (or legacy 'variable')");
    return false;
}

FString BindingPathToString(const TArray<FString>& Path)
{
    return FString::Join(Path, TEXT("."));
}

bool ResolveBindingLeafProperty(const UAnimBlueprint* AnimBP,
                                const TArray<FString>& BindingPath,
                                FProperty*& OutLeafProperty,
                                int32& OutArrayIndex,
                                FString& OutError)
{
    OutLeafProperty = nullptr;
    OutArrayIndex = INDEX_NONE;
    OutError.Reset();

    const UStruct* SourceRoot = GetAnimBindingSourceRoot(AnimBP);
    if (!SourceRoot)
    {
        OutError = TEXT("AnimBlueprint has no SkeletonGeneratedClass/GeneratedClass; compile it once before binding");
        return false;
    }

    const FName FeatureName(TEXT("PropertyAccessEditor"));
    if (!IModularFeatures::Get().IsModularFeatureAvailable(FeatureName))
    {
        OutError = TEXT("PropertyAccessEditor modular feature is not available");
        return false;
    }

    IPropertyAccessEditor& PropertyAccessEditor =
        IModularFeatures::Get().GetModularFeature<IPropertyAccessEditor>(FeatureName);
    const FPropertyAccessResolveResult Result =
        PropertyAccessEditor.ResolvePropertyAccess(SourceRoot, BindingPath, OutLeafProperty, OutArrayIndex);
    if (Result.Result == EPropertyAccessResolveResult::Failed || !OutLeafProperty)
    {
        OutError = FString::Printf(TEXT("could not resolve AnimBP property path '%s' on %s"),
                                   *BindingPathToString(BindingPath), *SourceRoot->GetName());
        return false;
    }
    return true;
}

bool BuildAnimNodePropertyBinding(UAnimBlueprint* AnimBP,
                                  UAnimGraphNode_Base* AnimNode,
                                  FName PinName,
                                  FName BindingName,
                                  const TArray<FString>& BindingPath,
                                  FAnimGraphNodePropertyBinding& OutBinding,
                                  FString& OutError)
{
    OutBinding = FAnimGraphNodePropertyBinding();
    OutError.Reset();
    if (!AnimBP || !AnimNode)
    {
        OutError = TEXT("invalid AnimBP or AnimGraph node");
        return false;
    }

    FProperty* TargetProperty = AnimNode->GetPinProperty(PinName);
    if (!TargetProperty)
    {
        OutError = FString::Printf(TEXT("property '%s' is not an AnimGraph input pin property on %s"),
                                   *PinName.ToString(), *AnimNode->GetClass()->GetName());
        return false;
    }

    FProperty* CompatibilityTarget = TargetProperty;
    if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(CompatibilityTarget))
    {
        CompatibilityTarget = ArrayProperty->Inner;
    }

    FProperty* LeafProperty = nullptr;
    int32 SourceArrayIndex = INDEX_NONE;
    if (!ResolveBindingLeafProperty(AnimBP, BindingPath, LeafProperty, SourceArrayIndex, OutError))
    {
        return false;
    }

    const FName FeatureName(TEXT("PropertyAccessEditor"));
    IPropertyAccessEditor& PropertyAccessEditor =
        IModularFeatures::Get().GetModularFeature<IPropertyAccessEditor>(FeatureName);
    const EPropertyAccessCompatibility Compatibility =
        PropertyAccessEditor.GetPropertyCompatibility(LeafProperty, CompatibilityTarget);
    if (Compatibility == EPropertyAccessCompatibility::Incompatible)
    {
        OutError = FString::Printf(TEXT("binding type mismatch: source '%s' (%s) cannot feed target '%s' (%s)"),
            *BindingPathToString(BindingPath),
            *LeafProperty->GetCPPType(),
            *BindingName.ToString(),
            *CompatibilityTarget->GetCPPType());
        return false;
    }

    const UAnimationGraphSchema* Schema = GetDefault<UAnimationGraphSchema>();
    OutBinding.PropertyName = BindingName;
    OutBinding.ArrayIndex = INDEX_NONE;
    OutBinding.PropertyPath = BindingPath;
    OutBinding.PathAsText = PropertyAccessEditor.MakeTextPath(BindingPath, GetAnimBindingSourceRoot(AnimBP));
    OutBinding.Type = EAnimGraphNodePropertyBindingType::Property;
    OutBinding.bIsBound = true;
    OutBinding.bOnlyUpdateWhenActive = false;
    Schema->ConvertPropertyToPinType(LeafProperty, OutBinding.PinType);
    OutBinding.bIsPromotion = (Compatibility == EPropertyAccessCompatibility::Promotable);
    OutBinding.PromotedPinType = OutBinding.PinType;
    return true;
}

TSharedPtr<FJsonObject> PinTypeToJson(const FEdGraphPinType& PinType)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
    Obj->SetStringField(TEXT("subcategory"), PinType.PinSubCategory.ToString());
    if (UObject* SubCategoryObject = PinType.PinSubCategoryObject.Get())
    {
        Obj->SetStringField(TEXT("subcategory_object"), SubCategoryObject->GetPathName());
    }
    return Obj;
}

FString BindingTypeToString(EAnimGraphNodePropertyBindingType Type)
{
    switch (Type)
    {
    case EAnimGraphNodePropertyBindingType::Property:
        return TEXT("property");
    case EAnimGraphNodePropertyBindingType::Function:
        return TEXT("function");
    case EAnimGraphNodePropertyBindingType::None:
    default:
        return TEXT("none");
    }
}

TSharedPtr<FJsonObject> BindingToJson(FName BindingName,
                                      const FAnimGraphNodePropertyBinding& Binding)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), BindingName.ToString());
    Obj->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
    Obj->SetNumberField(TEXT("array_index"), Binding.ArrayIndex);
    Obj->SetStringField(TEXT("path"), BindingPathToString(Binding.PropertyPath));
    Obj->SetStringField(TEXT("path_text"), Binding.PathAsText.ToString());
    Obj->SetStringField(TEXT("type"), BindingTypeToString(Binding.Type));
    Obj->SetBoolField(TEXT("bound"), Binding.bIsBound);
    Obj->SetBoolField(TEXT("is_promotion"), Binding.bIsPromotion);
    Obj->SetBoolField(TEXT("only_update_when_active"), Binding.bOnlyUpdateWhenActive);
    Obj->SetObjectField(TEXT("pin_type"), PinTypeToJson(Binding.PinType));
    Obj->SetObjectField(TEXT("promoted_pin_type"), PinTypeToJson(Binding.PromotedPinType));

    TArray<TSharedPtr<FJsonValue>> Segments;
    for (const FString& Segment : Binding.PropertyPath)
    {
        Segments.Add(MakeShared<FJsonValueString>(Segment));
    }
    Obj->SetArrayField(TEXT("path_segments"), Segments);
    return Obj;
}

void ReadBindingMapEntries(UObject* BindingObj,
                           TArray<TSharedPtr<FJsonValue>>& OutBindings,
                           int32& OutCount)
{
    OutBindings.Reset();
    OutCount = 0;
    if (!BindingObj) return;

    FMapProperty* MapProp = nullptr;
    FStructProperty* ValueStruct = nullptr;
    FString Error;
    if (!GetAnimNodeBindingMap(BindingObj, MapProp, ValueStruct, Error)) return;

    const void* MapPtr = MapProp->ContainerPtrToValuePtr<void>(BindingObj);
    FScriptMapHelper Helper(MapProp, MapPtr);
    const FNameProperty* KeyProp = CastFieldChecked<FNameProperty>(MapProp->KeyProp);

    for (int32 InternalIndex = 0; InternalIndex < Helper.GetMaxIndex(); ++InternalIndex)
    {
        if (!Helper.IsValidIndex(InternalIndex)) continue;

        const FName BindingName = KeyProp->GetPropertyValue(Helper.GetKeyPtr(InternalIndex));
        const FAnimGraphNodePropertyBinding* Binding =
            reinterpret_cast<const FAnimGraphNodePropertyBinding*>(Helper.GetValuePtr(InternalIndex));
        if (!Binding) continue;

        OutBindings.Add(MakeShared<FJsonValueObject>(BindingToJson(BindingName, *Binding)));
        ++OutCount;
    }
}

FString ExportPropertyValueText(const FProperty* Property, const void* Container)
{
    if (!Property || !Container) return FString();
    const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
    FString Out;
    Property->ExportTextItem_Direct(Out, ValuePtr, nullptr, nullptr, PPF_None);
    return Out;
}

TSharedPtr<FJsonObject> AnimNodePropertyToJson(UAnimGraphNode_Base* AnimNode,
                                               const FProperty* Property,
                                               const void* Container)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!AnimNode || !Property)
    {
        Obj->SetBoolField(TEXT("valid"), false);
        return Obj;
    }

    const FName PropertyName = Property->GetFName();
    FResolvedAnimNodePinBinding PinInfo;
    const bool bHasPinInfo = ResolveAnimNodePinBinding(AnimNode, PropertyName, PinInfo);
    const FName BindingName = bHasPinInfo ? PinInfo.BindingName : PropertyName;
    const int32 OptionalIndex = bHasPinInfo
        ? PinInfo.OptionalPinIndex
        : FindOptionalPinIndexForProperty(AnimNode, PropertyName);
    const UEdGraphPin* Pin = AnimNode->FindPin(PropertyName);
    const void* ValuePtr = Container ? Property->ContainerPtrToValuePtr<void>(Container) : nullptr;

    Obj->SetBoolField(TEXT("valid"), true);
    Obj->SetStringField(TEXT("name"), PropertyName.ToString());
    Obj->SetStringField(TEXT("binding_name"), BindingName.ToString());
    Obj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
    Obj->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
    Obj->SetStringField(TEXT("value_text"), ExportPropertyValueText(Property, Container));
    Obj->SetBoolField(TEXT("has_pin"), Pin != nullptr);
    Obj->SetBoolField(TEXT("has_binding"), AnimNode->HasBinding(BindingName));
    Obj->SetNumberField(TEXT("optional_pin_index"), OptionalIndex);
    Obj->SetBoolField(TEXT("optional_pin"), OptionalIndex != INDEX_NONE);
    if (OptionalIndex != INDEX_NONE)
    {
        if (PinInfo.bCustomPropertyPin)
        {
            const TArray<FOptionalPinFromProperty>* CustomPins = GetCustomPinProperties(AnimNode);
            if (CustomPins && CustomPins->IsValidIndex(OptionalIndex))
            {
                const FOptionalPinFromProperty& OptionalPin = (*CustomPins)[OptionalIndex];
                Obj->SetBoolField(TEXT("pin_visible"), OptionalPin.bShowPin);
                Obj->SetBoolField(TEXT("can_toggle_visibility"), OptionalPin.bCanToggleVisibility);
            }
        }
        else if (AnimNode->ShowPinForProperties.IsValidIndex(OptionalIndex))
        {
            const FOptionalPinFromProperty& OptionalPin = AnimNode->ShowPinForProperties[OptionalIndex];
            Obj->SetBoolField(TEXT("pin_visible"), OptionalPin.bShowPin);
            Obj->SetBoolField(TEXT("can_toggle_visibility"), OptionalPin.bCanToggleVisibility);
        }
    }
    if (Pin)
    {
        Obj->SetObjectField(TEXT("pin"), PinSummaryJson(Pin));
    }
    if (ValuePtr)
    {
        if (TSharedPtr<FJsonValue> JsonValue = detail::GetPropertyValueAtPtr(Property, ValuePtr))
        {
            Obj->SetField(TEXT("value"), JsonValue);
        }
    }
    return Obj;
}

void AppendCustomAnimNodePropertiesToJson(UAnimGraphNode_Base* AnimNode,
                                          TArray<TSharedPtr<FJsonValue>>& Properties)
{
    const TArray<FOptionalPinFromProperty>* CustomPins = GetCustomPinProperties(AnimNode);
    if (!AnimNode || !CustomPins)
    {
        return;
    }

    for (int32 Index = 0; Index < CustomPins->Num(); ++Index)
    {
        const FOptionalPinFromProperty& OptionalPin = (*CustomPins)[Index];
        if (OptionalPin.PropertyName == NAME_None)
        {
            continue;
        }

        FResolvedAnimNodePinBinding PinInfo;
        if (!ResolveAnimNodePinBinding(AnimNode, OptionalPin.PropertyName, PinInfo)
            || !PinInfo.bCustomPropertyPin
            || !PinInfo.PinProperty)
        {
            continue;
        }

        const UEdGraphPin* Pin = AnimNode->FindPin(OptionalPin.PropertyName);
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetBoolField(TEXT("valid"), true);
        Obj->SetBoolField(TEXT("custom_property"), true);
        Obj->SetStringField(TEXT("name"), OptionalPin.PropertyName.ToString());
        Obj->SetStringField(TEXT("binding_name"), PinInfo.BindingName.ToString());
        Obj->SetStringField(TEXT("cpp_type"), PinInfo.PinProperty->GetCPPType());
        Obj->SetStringField(TEXT("property_class"), PinInfo.PinProperty->GetClass()->GetName());
        Obj->SetStringField(TEXT("value_text"), Pin ? Pin->DefaultValue : FString());
        Obj->SetBoolField(TEXT("has_pin"), Pin != nullptr);
        Obj->SetBoolField(TEXT("has_binding"), AnimNode->HasBinding(PinInfo.BindingName));
        Obj->SetNumberField(TEXT("optional_pin_index"), PinInfo.OptionalPinIndex);
        Obj->SetBoolField(TEXT("optional_pin"), true);
        Obj->SetBoolField(TEXT("pin_visible"), OptionalPin.bShowPin);
        Obj->SetBoolField(TEXT("can_toggle_visibility"), OptionalPin.bCanToggleVisibility);
        if (Pin)
        {
            Obj->SetObjectField(TEXT("pin"), PinSummaryJson(Pin));
        }
        Properties.Add(MakeShared<FJsonValueObject>(Obj));
    }
}

FString GraphKind(UEdGraph* Graph)
{
    if (!Graph) return TEXT("unknown");
    if (Graph->IsA<UAnimationStateMachineGraph>()) return TEXT("state_machine");
    if (Graph->IsA<UAnimationStateGraph>()) return TEXT("state_bound_graph");
    if (Graph->IsA<UAnimationTransitionGraph>()) return TEXT("transition_rule_graph");
    if (Graph->Schema && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
    {
        return TEXT("anim_graph");
    }
    return TEXT("graph");
}

TArray<TSharedPtr<FJsonValue>> OuterChainJson(const UObject* Obj)
{
    TArray<TSharedPtr<FJsonValue>> Chain;
    for (const UObject* Cur = Obj; Cur; Cur = Cur->GetOuter())
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Cur->GetName());
        Entry->SetStringField(TEXT("class"), Cur->GetClass()->GetPathName());
        Entry->SetStringField(TEXT("path"), Cur->GetPathName());
        Chain.Add(MakeShared<FJsonValueObject>(Entry));
    }
    return Chain;
}

TSharedPtr<FJsonObject> StateMachineReferenceJson(
    UAnimBlueprint* AnimBP,
    UAnimGraphNode_StateMachineBase* SMNode,
    UEdGraph* ContainerGraph)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!SMNode)
    {
        Obj->SetBoolField(TEXT("found"), false);
        return Obj;
    }

    UAnimationStateMachineGraph* SMGraph =
        Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    const TArray<FStateMachineReference> Refs =
        FindStateMachineReferences(AnimBP, SMGraph);
    const bool bGraphOuterIsNode = SMGraph && SMGraph->GetOuter() == SMNode;
    const bool bOwnerAnimGraphNode = SMGraph && SMGraph->OwnerAnimGraphNode == SMNode;

    Obj->SetBoolField(TEXT("found"), true);
    Obj->SetStringField(TEXT("node_id"), SMNode->NodeGuid.ToString(EGuidFormats::Digits));
    Obj->SetStringField(TEXT("node_class"), SMNode->GetClass()->GetPathName());
    Obj->SetStringField(TEXT("container_graph"),
        ContainerGraph ? ContainerGraph->GetName() : FString());
    Obj->SetBoolField(TEXT("has_state_machine_graph"), SMGraph != nullptr);
    Obj->SetBoolField(TEXT("graph_outer_is_node"), bGraphOuterIsNode);
    Obj->SetBoolField(TEXT("is_owner_anim_graph_node"), bOwnerAnimGraphNode);
    Obj->SetNumberField(TEXT("reference_count"), Refs.Num());
    Obj->SetBoolField(TEXT("safe_reference_delete"),
        SMGraph != nullptr && Refs.Num() > 1 && !bGraphOuterIsNode && !bOwnerAnimGraphNode);

    if (SMGraph)
    {
        Obj->SetStringField(TEXT("state_machine_name"), SMGraph->GetName());
        Obj->SetStringField(TEXT("state_machine_path"), SMGraph->GetPathName());
        Obj->SetStringField(TEXT("state_machine_outer"), SMGraph->GetOuter()
            ? SMGraph->GetOuter()->GetPathName()
            : FString());
        Obj->SetArrayField(TEXT("state_machine_outer_chain"), OuterChainJson(SMGraph));
        if (SMGraph->OwnerAnimGraphNode)
        {
            Obj->SetStringField(TEXT("owner_node_id"),
                SMGraph->OwnerAnimGraphNode->NodeGuid.ToString(EGuidFormats::Digits));
            Obj->SetStringField(TEXT("owner_node_graph"),
                SMGraph->OwnerAnimGraphNode->GetGraph()
                    ? SMGraph->OwnerAnimGraphNode->GetGraph()->GetName()
                    : FString());
        }
    }

    TArray<TSharedPtr<FJsonValue>> RefArr;
    for (const FStateMachineReference& Ref : Refs)
    {
        if (!Ref.Node) continue;
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("node_id"), Ref.Node->NodeGuid.ToString(EGuidFormats::Digits));
        R->SetStringField(TEXT("graph"), Ref.ContainerGraph ? Ref.ContainerGraph->GetName() : FString());
        R->SetBoolField(TEXT("graph_outer_is_node"),
            SMGraph != nullptr && SMGraph->GetOuter() == Ref.Node);
        R->SetBoolField(TEXT("is_owner_anim_graph_node"),
            SMGraph != nullptr && SMGraph->OwnerAnimGraphNode == Ref.Node);
        RefArr.Add(MakeShared<FJsonValueObject>(R));
    }
    Obj->SetArrayField(TEXT("references"), RefArr);
    return Obj;
}

TArray<TSharedPtr<FJsonValue>> CollectAnimNodeAssetReferences(UAnimGraphNode_Base* AnimNode)
{
    TArray<TSharedPtr<FJsonValue>> Assets;
    if (!AnimNode) return Assets;

    FStructProperty* NodeStructProp = nullptr;
    void* NodeStructPtr = nullptr;
    if (!GetAnimNodeStructTarget(AnimNode, NodeStructProp, NodeStructPtr)
        || !NodeStructProp || !NodeStructProp->Struct || !NodeStructPtr)
    {
        return Assets;
    }

    auto AddAsset = [&Assets](const FProperty* Property, const FString& Path, const UObject* Obj)
    {
        if (Path.IsEmpty() && !Obj) return;
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("property"), Property ? Property->GetName() : FString());
        if (!Path.IsEmpty()) A->SetStringField(TEXT("path"), Path);
        if (Obj)
        {
            A->SetStringField(TEXT("path"), Obj->GetPathName());
            A->SetStringField(TEXT("class"), Obj->GetClass()->GetPathName());
            A->SetStringField(TEXT("name"), Obj->GetName());
        }
        Assets.Add(MakeShared<FJsonValueObject>(A));
    };

    for (TFieldIterator<FProperty> It(NodeStructProp->Struct); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property) continue;
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(NodeStructPtr);
        if (!ValuePtr) continue;

        if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Property))
        {
            const FString Path = SoftProp->GetPropertyValue(ValuePtr).ToString();
            if (!Path.IsEmpty())
            {
                AddAsset(Property, Path, nullptr);
            }
            continue;
        }

        if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
        {
            UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
            if (Obj && Obj->IsA<UAnimationAsset>())
            {
                AddAsset(Property, FString(), Obj);
            }
        }
    }
    return Assets;
}

void ReadAnimGraphOptions(
    const TSharedPtr<FJsonObject>& Args,
    FAnimGraphReadOptions& Options,
    bool bDefaultProperties,
    bool bDefaultPins,
    bool bDefaultConnections)
{
    Options.bIncludeProperties = bDefaultProperties;
    Options.bIncludePins = bDefaultPins;
    Options.bIncludeConnections = bDefaultConnections;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("include_properties"), Options.bIncludeProperties);
        Args->TryGetBoolField(TEXT("include_pins"), Options.bIncludePins);
        Args->TryGetBoolField(TEXT("include_connections"), Options.bIncludeConnections);
        Args->TryGetStringField(TEXT("node_class"), Options.NodeClassFilter);
        if (Options.NodeClassFilter.IsEmpty())
        {
            Args->TryGetStringField(TEXT("class"), Options.NodeClassFilter);
        }
        Args->TryGetStringField(TEXT("asset_substring"), Options.AssetSubstringFilter);
        FString SingleNodeId;
        if (Args->TryGetStringField(TEXT("node_id"), SingleNodeId) && !SingleNodeId.IsEmpty())
        {
            Options.NodeIds.Add(SingleNodeId);
        }
        const TArray<TSharedPtr<FJsonValue>>* NodeIdValues = nullptr;
        if (Args->TryGetArrayField(TEXT("node_ids"), NodeIdValues) && NodeIdValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *NodeIdValues)
            {
                if (Value.IsValid() && Value->Type == EJson::String && !Value->AsString().IsEmpty())
                {
                    Options.NodeIds.Add(Value->AsString());
                }
            }
        }
    }
    if (Options.bIncludeConnections)
    {
        Options.bIncludePins = true;
    }
}

bool NodePassesAnimGraphFilters(
    UEdGraphNode* Node,
    const FAnimGraphReadOptions& Options,
    const TArray<TSharedPtr<FJsonValue>>& AssetRefs)
{
    if (!Node) return false;
    const FString NodeId = Node->NodeGuid.ToString(EGuidFormats::Digits);
    if (Options.NodeIds.Num() > 0 && !Options.NodeIds.Contains(NodeId))
    {
        return false;
    }
    if (!Options.NodeClassFilter.IsEmpty())
    {
        const FString ClassPath = Node->GetClass()->GetPathName();
        const FString ClassName = Node->GetClass()->GetName();
        if (!ClassPath.Contains(Options.NodeClassFilter, ESearchCase::IgnoreCase)
            && !ClassName.Contains(Options.NodeClassFilter, ESearchCase::IgnoreCase))
        {
            return false;
        }
    }
    if (!Options.AssetSubstringFilter.IsEmpty())
    {
        bool bMatched = false;
        for (const TSharedPtr<FJsonValue>& Value : AssetRefs)
        {
            TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Obj.IsValid())
            {
                continue;
            }
            FString Path;
            if (Obj->TryGetStringField(TEXT("path"), Path)
                && Path.Contains(Options.AssetSubstringFilter, ESearchCase::IgnoreCase))
            {
                bMatched = true;
                break;
            }
        }
        if (!bMatched)
        {
            return false;
        }
    }
    return true;
}

void AppendAnimNodeDeepReadback(
    UAnimGraphNode_Base* AnimNode,
    TSharedPtr<FJsonObject>& Obj,
    const FAnimGraphReadOptions& Options,
    const TArray<TSharedPtr<FJsonValue>>& AssetRefs)
{
    if (!AnimNode || !Obj.IsValid()) return;

    if (UScriptStruct* FNodeType = AnimNode->GetFNodeType())
    {
        Obj->SetStringField(TEXT("fnode_type"), FNodeType->GetPathName());
    }

    Obj->SetArrayField(TEXT("animation_assets"), AssetRefs);
    Obj->SetNumberField(TEXT("animation_asset_count"), AssetRefs.Num());

    if (!Options.bIncludeProperties)
    {
        return;
    }

    TArray<TSharedPtr<FJsonValue>> Properties;
    FStructProperty* NodeStructProp = nullptr;
    void* NodeStructPtr = nullptr;
    if (GetAnimNodeStructTarget(AnimNode, NodeStructProp, NodeStructPtr)
        && NodeStructProp && NodeStructProp->Struct)
    {
        Obj->SetStringField(TEXT("inner_struct"), NodeStructProp->Struct->GetPathName());
        for (TFieldIterator<FProperty> It(NodeStructProp->Struct); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property) continue;
            Properties.Add(MakeShared<FJsonValueObject>(
                AnimNodePropertyToJson(AnimNode, Property, NodeStructPtr)));
        }
    }
    AppendCustomAnimNodePropertiesToJson(AnimNode, Properties);

    TArray<TSharedPtr<FJsonValue>> Bindings;
    int32 BindingCount = 0;
    UObject* BindingObj = GetAnimNodeBindingObject(AnimNode);
    ReadBindingMapEntries(BindingObj, Bindings, BindingCount);
    if (BindingObj)
    {
        Obj->SetStringField(TEXT("binding_class"), BindingObj->GetClass()->GetPathName());
    }
    Obj->SetNumberField(TEXT("property_count"), Properties.Num());
    Obj->SetNumberField(TEXT("binding_count"), BindingCount);
    Obj->SetArrayField(TEXT("properties"), Properties);
    Obj->SetArrayField(TEXT("bindings"), Bindings);
}

TSharedPtr<FJsonObject> AnimGraphNodeToJson(
    UAnimBlueprint* AnimBP,
    UEdGraph* Graph,
    UEdGraphNode* Node,
    const FAnimGraphReadOptions& Options,
    const TArray<TSharedPtr<FJsonValue>>& AssetRefs)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::Digits));
    Obj->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
    Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Obj->SetNumberField(TEXT("x"), Node->NodePosX);
    Obj->SetNumberField(TEXT("y"), Node->NodePosY);
    Obj->SetNumberField(TEXT("pin_count"), Node->Pins.Num());

    const TCHAR* Kind = TEXT("other");
    if (Node->IsA<UAnimGraphNode_StateMachineBase>())         Kind = TEXT("state_machine");
    else if (Node->IsA<UAnimGraphNode_AssetPlayerBase>())     Kind = TEXT("asset_player");
    else if (Node->IsA<UAnimGraphNode_BlendListBase>())       Kind = TEXT("blend_list");
    else if (Node->IsA<UAnimGraphNode_SkeletalControlBase>()) Kind = TEXT("bone_control");
    else if (Node->IsA<UAnimGraphNode_Base>())                Kind = TEXT("anim_node");
    Obj->SetStringField(TEXT("kind"), Kind);

    if (Options.bIncludePins)
    {
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            Pins.Add(MakeShared<FJsonValueObject>(
                PinSummaryJson(Pin, Options.bIncludeConnections)));
        }
        Obj->SetArrayField(TEXT("pins"), Pins);
    }

    if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
    {
        AppendAnimNodeDeepReadback(AnimNode, Obj, Options, AssetRefs);
    }

    if (UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node))
    {
        Obj->SetObjectField(TEXT("state_machine"),
            StateMachineReferenceJson(AnimBP, SMNode, Graph));
    }

    return Obj;
}

TSharedPtr<FJsonObject> OutputPosePathJson(UEdGraph* Graph)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    UEdGraphNode* OutputNode = FindAnimGraphOutput(Graph);
    Obj->SetBoolField(TEXT("found"), OutputNode != nullptr);
    if (!OutputNode)
    {
        return Obj;
    }

    Obj->SetStringField(TEXT("output_node_id"),
        OutputNode->NodeGuid.ToString(EGuidFormats::Digits));
    Obj->SetStringField(TEXT("output_node_class"), OutputNode->GetClass()->GetPathName());
    UEdGraphPin* OutputIn = FindFirstInputPosePin(OutputNode);
    Obj->SetObjectField(TEXT("input_pin"), PinSummaryJson(OutputIn));

    TArray<TSharedPtr<FJsonValue>> Sources;
    if (OutputIn)
    {
        for (UEdGraphPin* Linked : OutputIn->LinkedTo)
        {
            if (!Linked) continue;
            UEdGraphNode* SourceNode = Linked->GetOwningNode();
            TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
            Source->SetStringField(TEXT("pin"), Linked->PinName.ToString());
            if (SourceNode)
            {
                Source->SetStringField(TEXT("node_id"),
                    SourceNode->NodeGuid.ToString(EGuidFormats::Digits));
                Source->SetStringField(TEXT("node_class"), SourceNode->GetClass()->GetPathName());
                Source->SetStringField(TEXT("title"),
                    SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
            }
            Sources.Add(MakeShared<FJsonValueObject>(Source));
        }
    }
    Obj->SetArrayField(TEXT("sources"), Sources);
    Obj->SetNumberField(TEXT("source_count"), Sources.Num());
    return Obj;
}

TSharedPtr<FJsonObject> AnimGraphToJson(
    UAnimBlueprint* AnimBP,
    UEdGraph* Graph,
    const FAnimGraphReadOptions& Options)
{
    TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
    if (!Graph)
    {
        GObj->SetBoolField(TEXT("found"), false);
        return GObj;
    }

    GObj->SetBoolField(TEXT("found"), true);
    GObj->SetStringField(TEXT("name"), Graph->GetFName().ToString());
    GObj->SetStringField(TEXT("graph"), Graph->GetName());
    GObj->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetPathName());
    GObj->SetStringField(TEXT("kind"), GraphKind(Graph));
    GObj->SetStringField(TEXT("schema"), Graph->Schema ? Graph->Schema->GetPathName() : FString());
    GObj->SetArrayField(TEXT("outer_chain"), OuterChainJson(Graph));

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;
        TArray<TSharedPtr<FJsonValue>> AssetRefs;
        if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
        {
            AssetRefs = CollectAnimNodeAssetReferences(AnimNode);
        }
        if (!NodePassesAnimGraphFilters(Node, Options, AssetRefs))
        {
            continue;
        }
        Nodes.Add(MakeShared<FJsonValueObject>(
            AnimGraphNodeToJson(AnimBP, Graph, Node, Options, AssetRefs)));
    }

    GObj->SetArrayField(TEXT("nodes"), Nodes);
    GObj->SetNumberField(TEXT("node_count"), Nodes.Num());
    GObj->SetObjectField(TEXT("output_pose"), OutputPosePathJson(Graph));
    return GObj;
}

UAnimStateNodeBase* ResolveStateNodeForRead(
    UAnimBlueprint* AnimBP,
    const FString& StateMachineName,
    const FString& StateName,
    const FString& StateId,
    UAnimationStateMachineGraph*& OutSMGraph,
    FString& OutError)
{
    OutSMGraph = nullptr;
    if (!AnimBP)
    {
        OutError = TEXT("not a UAnimBlueprint");
        return nullptr;
    }

    TArray<UAnimationStateMachineGraph*> CandidateSMs;
    if (!StateMachineName.IsEmpty())
    {
        if (UAnimationStateMachineGraph* SM = FindStateMachineGraph(AnimBP, FName(*StateMachineName)))
        {
            CandidateSMs.Add(SM);
        }
        else
        {
            OutError = FString::Printf(TEXT("state machine not found: %s"), *StateMachineName);
            return nullptr;
        }
    }
    else
    {
        CandidateSMs = CollectStateMachineGraphs(AnimBP);
    }

    TArray<UAnimStateNodeBase*> Matches;
    TArray<UAnimationStateMachineGraph*> MatchSMs;
    for (UAnimationStateMachineGraph* SMGraph : CandidateSMs)
    {
        if (!SMGraph) continue;
        UAnimStateNodeBase* ById = !StateId.IsEmpty()
            ? FindStateNodeByGuid(SMGraph, StateId)
            : nullptr;
        if (ById)
        {
            Matches.Add(ById);
            MatchSMs.Add(SMGraph);
            continue;
        }
        if (StateName.IsEmpty()) continue;
        for (UEdGraphNode* Node : SMGraph->Nodes)
        {
            UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Node);
            if (!State) continue;
            UEdGraph* Bound = GetStateBoundGraph(State);
            if (GetStateDisplayName(State).Equals(StateName, ESearchCase::IgnoreCase)
                || State->GetName().Equals(StateName, ESearchCase::IgnoreCase)
                || (Bound && Bound->GetName().Equals(StateName, ESearchCase::IgnoreCase)))
            {
                Matches.Add(State);
                MatchSMs.Add(SMGraph);
            }
        }
    }

    if (Matches.Num() == 0)
    {
        OutError = StateId.IsEmpty()
            ? FString::Printf(TEXT("state not found: %s"), *StateName)
            : FString::Printf(TEXT("state_id not found: %s"), *StateId);
        return nullptr;
    }
    if (Matches.Num() > 1)
    {
        OutError = FString::Printf(
            TEXT("state graph lookup is ambiguous: %d states match; pass state_machine_name or state_id"),
            Matches.Num());
        return nullptr;
    }

    OutSMGraph = MatchSMs[0];
    return Matches[0];
}

// ---------------------------------------------------------------------------
// animation.add_animgraph_node
// ---------------------------------------------------------------------------
//
// Generic node ctor: spawn a UAnimGraphNode_Base subclass into a target graph.
// Args: path (anim_bp), graph_name? (default AnimGraph), node_class
// (UClass path or "/Script/AnimGraph.AnimGraphNode_X"), x?, y?
// Returns: { node_id, class }
FSageToolDispatch::FOutcome AddAnimGraphNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, NodeClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("node_class"), NodeClassPath) || NodeClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'node_class'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);
    double X = 0.0, Y = 0.0;
    Args->TryGetNumberField(TEXT("x"), X);
    Args->TryGetNumberField(TEXT("y"), Y);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found on %s"),
                            *GraphName, *AnimBP->GetName()));
    }

    UClass* NodeCls = FindObject<UClass>(nullptr, *NodeClassPath);
    if (!NodeCls) NodeCls = LoadObject<UClass>(nullptr, *NodeClassPath);
    if (!NodeCls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_class not found: %s"), *NodeClassPath));
    }
    if (!NodeCls->IsChildOf(UAnimGraphNode_Base::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("%s isn't a UAnimGraphNode_Base subclass"),
                            *NodeCls->GetName()));
    }
    if (NodeCls->HasAnyClassFlags(CLASS_Abstract))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_class %s is abstract"), *NodeCls->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("AddAGNode", "Sage: Add AnimGraph Node"));
    AnimBP->Modify();
    TargetGraph->Modify();

    UEdGraphNode* Node = NewObject<UEdGraphNode>(TargetGraph, NodeCls);
    Node->CreateNewGuid();
    Node->NodePosX = static_cast<int32>(X);
    Node->NodePosY = static_cast<int32>(Y);
    TargetGraph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    // PostPlacedNewNode is the engine canonical pivot — for composite nodes
    // (StateMachine, BlendListByInt, BlendSpaceGraphBase, LinkedInputPose,
    // Mirror, MultiWayBlend) UAnimGraphNode_Base::PostPlacedNewNode invokes
    // EnsureBindingsArePresent + UAnimBlueprintExtension::RequestExtensionsForNode.
    // Skipping it leaves the node with no extensions registered and the BP
    // compiler later asserts.
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"),  Node->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("class"),    NodeCls->GetPathName());
    R->SetStringField(TEXT("graph"),    TargetGraph->GetFName().ToString());
    R->SetNumberField(TEXT("x"),        X);
    R->SetNumberField(TEXT("y"),        Y);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RemoveAnimGraphNodeShared(
    const TSharedPtr<FJsonObject>& Args,
    bool bRequireStateMachineNode)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, NodeId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'node_id'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);
    bool bDryRun = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }
    UEdGraphNode* Node = FindGraphNodeByGuid(TargetGraph, NodeId);
    if (!Node)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id not found: %s"), *NodeId));
    }

    UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
    if (bRequireStateMachineNode && !SMNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id %s is not a UAnimGraphNode_StateMachineBase"), *NodeId));
    }

    UAnimationStateMachineGraph* SMGraph = SMNode
        ? Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph)
        : nullptr;
    const TArray<FStateMachineReference> SMRefs =
        SMNode ? FindStateMachineReferences(AnimBP, SMGraph) : TArray<FStateMachineReference>();
    const bool bSMGraphOuterIsNode = SMGraph && SMGraph->GetOuter() == SMNode;
    const bool bSMOwnerNode = SMGraph && SMGraph->OwnerAnimGraphNode == SMNode;
    const bool bSafeSMReferenceDelete =
        SMNode && SMGraph && SMRefs.Num() > 1 && !bSMGraphOuterIsNode && !bSMOwnerNode;

    auto BuildResponse = [&](bool bRemoved, const FString& Mode, const FString& Reason)
    {
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("node_id"), NodeId);
        R->SetStringField(TEXT("graph"), TargetGraph->GetName());
        R->SetBoolField(TEXT("removed"), bRemoved);
        R->SetBoolField(TEXT("dry_run"), bDryRun);
        if (!Mode.IsEmpty()) R->SetStringField(TEXT("mode"), Mode);
        if (!Reason.IsEmpty()) R->SetStringField(TEXT("reason"), Reason);
        if (SMNode)
        {
            R->SetObjectField(TEXT("state_machine"),
                StateMachineReferenceJson(AnimBP, SMNode, TargetGraph));
            R->SetBoolField(TEXT("can_remove"), !SMGraph || bSafeSMReferenceDelete);
        }
        else
        {
            R->SetBoolField(TEXT("can_remove"), true);
        }
        return R;
    };

    if (bDryRun)
    {
        FString Reason;
        FString Mode = TEXT("remove_node");
        if (SMNode)
        {
            if (!SMGraph)
            {
                Reason = TEXT("state-machine node has no EditorStateMachineGraph; normal node removal is safe");
                Mode = TEXT("remove_null_state_machine_node");
            }
            else if (bSafeSMReferenceDelete)
            {
                Reason = TEXT("non-owning duplicate state-machine reference; removal will detach the graph pointer before RemoveNode");
                Mode = TEXT("detach_state_machine_reference");
            }
            else
            {
                Reason = TEXT("state-machine graph appears owned/anchored by this node or has no replacement reference; deletion is rejected");
                Mode = TEXT("reject_state_machine_anchor_delete");
            }
        }
        return FSageToolDispatch::FOutcome::MakeSuccess(BuildResponse(false, Mode, Reason));
    }

    if (SMNode && SMGraph && !bSafeSMReferenceDelete)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(
                TEXT("refusing to remove state-machine graph anchor/reference node %s: graph=%s references=%d graph_outer_is_node=%s owner_anim_graph_node=%s; call animation.remove_state_machine_reference_node with dry_run:true for diagnostics and remove only non-owning duplicate references"),
                *NodeId,
                *SMGraph->GetPathName(),
                SMRefs.Num(),
                bSMGraphOuterIsNode ? TEXT("true") : TEXT("false"),
                bSMOwnerNode ? TEXT("true") : TEXT("false")));
    }

    FScopedTransaction Tx(LOCTEXT("RemoveAGNode", "Sage: Remove AnimGraph Node"));
    AnimBP->Modify();
    TargetGraph->Modify();
    if (SMNode && SMGraph && bSafeSMReferenceDelete)
    {
        // UAnimGraphNode_StateMachineBase::DestroyNode always removes
        // EditorStateMachineGraph. For non-owning duplicate references, clear
        // the pointer first so RemoveNode deletes only the visual reference.
        SMNode->Modify();
        SMNode->EditorStateMachineGraph = nullptr;
    }
    FBlueprintEditorUtils::RemoveNode(AnimBP, Node, /*bDontRecompile=*/true);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    const FString Mode = SMNode && SMGraph && bSafeSMReferenceDelete
        ? TEXT("detach_state_machine_reference")
        : TEXT("remove_node");
    return FSageToolDispatch::FOutcome::MakeSuccess(BuildResponse(true, Mode, FString()));
}

// ---------------------------------------------------------------------------
// animation.remove_animgraph_node
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome RemoveAnimGraphNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    return RemoveAnimGraphNodeShared(Args, /*bRequireStateMachineNode=*/false);
}

// ---------------------------------------------------------------------------
// animation.remove_state_machine_reference_node
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome RemoveStateMachineReferenceNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    return RemoveAnimGraphNodeShared(Args, /*bRequireStateMachineNode=*/true);
}

// ---------------------------------------------------------------------------
// animation.connect_pose_pin / animation.disconnect_pose_pin
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome ConnectPosePinImplShared(const TSharedPtr<FJsonObject>& Args, bool bConnect)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, FromId, ToId, FromPinName, ToPinName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("from_node_id"), FromId) || FromId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'from_node_id'"));
    }
    if (!Args->TryGetStringField(TEXT("to_node_id"), ToId) || ToId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'to_node_id'"));
    }
    Args->TryGetStringField(TEXT("graph_name"),   GraphName);
    Args->TryGetStringField(TEXT("from_pin"),     FromPinName);
    Args->TryGetStringField(TEXT("to_pin"),       ToPinName);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }
    UEdGraphNode* FromNode = FindGraphNodeByGuid(TargetGraph, FromId);
    UEdGraphNode* ToNode   = FindGraphNodeByGuid(TargetGraph, ToId);
    if (!FromNode || !ToNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("from_node_id or to_node_id not found in graph"));
    }

    UEdGraphPin* FromPin = FromPinName.IsEmpty()
        ? FindFirstOutputPosePin(FromNode)
        : FindPinByName(FromNode, FromPinName, EGPD_Output);
    UEdGraphPin* ToPin = ToPinName.IsEmpty()
        ? FindFirstInputPosePin(ToNode)
        : FindPinByName(ToNode, ToPinName, EGPD_Input);
    if (!FromPin || !ToPin)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("pin lookup failed (check from_pin / to_pin or pose-pin convention)"));
    }

    FScopedTransaction Tx(bConnect
        ? LOCTEXT("ConnectPin", "Sage: Connect Pose Pin")
        : LOCTEXT("DisconnectPin", "Sage: Disconnect Pose Pin"));
    AnimBP->Modify();
    TargetGraph->Modify();

    bool bChanged = false;
    if (bConnect)
    {
        // Pose input pins are single-connection by schema. Without
        // BreakAllPinLinks first, MakeLinkTo would create a multiply-linked
        // input that the BP compiler later rejects. Mirror the cleanup the
        // canonical create_state_machine + set_animgraph_root_pose paths do.
        if (ToPin->Direction == EGPD_Input)
        {
            ToPin->BreakAllPinLinks();
        }
        FromPin->MakeLinkTo(ToPin);
        bChanged = true;
    }
    else
    {
        FromPin->BreakLinkTo(ToPin);
        bChanged = true;
    }
    // Pin link toggles aren't structural — they don't add/remove pins or
    // change the AnimGraph compile shape, just rewire existing pose flow.
    // MarkBlueprintAsModified is the right grain (avoids unnecessary
    // skeleton-class recompiles which Structurally would force).
    FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("from_node_id"), FromId);
    R->SetStringField(TEXT("to_node_id"),   ToId);
    R->SetBoolField  (bConnect ? TEXT("connected") : TEXT("disconnected"), bChanged);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ConnectPosePinImpl(const TSharedPtr<FJsonObject>& Args)
{
    return ConnectPosePinImplShared(Args, /*bConnect=*/true);
}
FSageToolDispatch::FOutcome DisconnectPosePinImpl(const TSharedPtr<FJsonObject>& Args)
{
    return ConnectPosePinImplShared(Args, /*bConnect=*/false);
}

// ---------------------------------------------------------------------------
// animation.set_anim_node_property
// ---------------------------------------------------------------------------
//
// Mutate a single property inside the inner FAnimNode_* struct of a
// UAnimGraphNode_*. JSON value type maps:
//   string  → FName / object path / FString / enum-by-name
//   number  → float / double / int / bool (clamped)
//   array   → FVector / FRotator / FLinearColor (size-tagged)
//   object  → reserved for nested struct (deferred to richer reflection tool)

bool ExtractLayerSetupValue(const FString& Text, const TCHAR* Key, FString& OutValue)
{
    const FString Needle = FString::Printf(TEXT("%s="), Key);
    const int32 KeyIndex = Text.Find(Needle, ESearchCase::IgnoreCase);
    if (KeyIndex == INDEX_NONE)
    {
        return false;
    }

    int32 ValueStart = KeyIndex + Needle.Len();
    while (ValueStart < Text.Len() && FChar::IsWhitespace(Text[ValueStart]))
    {
        ++ValueStart;
    }

    if (ValueStart >= Text.Len())
    {
        return false;
    }

    if (Text[ValueStart] == TEXT('"'))
    {
        const int32 QuoteStart = ValueStart + 1;
        int32 QuoteEnd = INDEX_NONE;
        for (int32 Index = QuoteStart; Index < Text.Len(); ++Index)
        {
            if (Text[Index] == TEXT('"'))
            {
                QuoteEnd = Index;
                break;
            }
        }
        if (QuoteEnd != INDEX_NONE)
        {
            OutValue = Text.Mid(QuoteStart, QuoteEnd - QuoteStart);
            return true;
        }
    }

    int32 ValueEnd = ValueStart;
    while (ValueEnd < Text.Len()
        && Text[ValueEnd] != TEXT(',')
        && Text[ValueEnd] != TEXT(')')
        && Text[ValueEnd] != TEXT(']'))
    {
        ++ValueEnd;
    }

    OutValue = Text.Mid(ValueStart, ValueEnd - ValueStart).TrimStartAndEnd();
    return !OutValue.IsEmpty();
}

bool SetLayeredBoneBlendLayerSetup(FArrayProperty* LayerSetupProperty, void* LayerSetupValuePtr, const TSharedPtr<FJsonValue>& Value, FString& OutError)
{
    if ((LayerSetupProperty == nullptr) || (LayerSetupValuePtr == nullptr) || !Value.IsValid() || (Value->Type != EJson::String))
    {
        OutError = TEXT("LayerSetup writer requires a string value and valid array target");
        return false;
    }

    FStructProperty* LayerStructProperty = CastField<FStructProperty>(LayerSetupProperty->Inner);
    if ((LayerStructProperty == nullptr) || (LayerStructProperty->Struct == nullptr))
    {
        OutError = TEXT("LayerSetup inner property is not an FInputBlendPose struct");
        return false;
    }

    FArrayProperty* BranchFiltersProperty = FindFProperty<FArrayProperty>(LayerStructProperty->Struct, TEXT("BranchFilters"));
    if (BranchFiltersProperty == nullptr)
    {
        OutError = FString::Printf(TEXT("BranchFilters array not found on %s"), *LayerStructProperty->Struct->GetName());
        return false;
    }

    FStructProperty* BranchFilterStructProperty = CastField<FStructProperty>(BranchFiltersProperty->Inner);
    if ((BranchFilterStructProperty == nullptr) || (BranchFilterStructProperty->Struct == nullptr))
    {
        OutError = TEXT("BranchFilters inner property is not an FBranchFilter struct");
        return false;
    }

    FNameProperty* BoneNameProperty = FindFProperty<FNameProperty>(BranchFilterStructProperty->Struct, TEXT("BoneName"));
    FIntProperty* BlendDepthProperty = FindFProperty<FIntProperty>(BranchFilterStructProperty->Struct, TEXT("BlendDepth"));
    if ((BoneNameProperty == nullptr) || (BlendDepthProperty == nullptr))
    {
        OutError = FString::Printf(TEXT("BoneName/BlendDepth properties not found on %s"), *BranchFilterStructProperty->Struct->GetName());
        return false;
    }

    FString Text = Value->AsString().TrimStartAndEnd();
    if (Text.Len() >= 2 && Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\"")))
    {
        Text = Text.Mid(1, Text.Len() - 2);
        Text.ReplaceInline(TEXT("\\\""), TEXT("\""));
    }

    FString BoneNameText;
    if (!ExtractLayerSetupValue(Text, TEXT("BoneName"), BoneNameText))
    {
        BoneNameText = TEXT("spine_01");
    }

    FString BlendDepthText;
    int32 BlendDepth = 10;
    if (ExtractLayerSetupValue(Text, TEXT("BlendDepth"), BlendDepthText))
    {
        BlendDepth = FCString::Atoi(*BlendDepthText);
    }

    FScriptArrayHelper LayerSetupHelper(LayerSetupProperty, LayerSetupValuePtr);
    LayerSetupHelper.EmptyValues();
    const int32 LayerIndex = LayerSetupHelper.AddValue();
    void* LayerValuePtr = LayerSetupHelper.GetRawPtr(LayerIndex);

    void* BranchFiltersValuePtr = BranchFiltersProperty->ContainerPtrToValuePtr<void>(LayerValuePtr);
    FScriptArrayHelper BranchFiltersHelper(BranchFiltersProperty, BranchFiltersValuePtr);
    BranchFiltersHelper.EmptyValues();
    const int32 BranchIndex = BranchFiltersHelper.AddValue();
    void* BranchFilterValuePtr = BranchFiltersHelper.GetRawPtr(BranchIndex);

    BoneNameProperty->SetPropertyValue(
        BoneNameProperty->ContainerPtrToValuePtr<void>(BranchFilterValuePtr),
        FName(*BoneNameText));
    BlendDepthProperty->SetPropertyValue(
        BlendDepthProperty->ContainerPtrToValuePtr<void>(BranchFilterValuePtr),
        BlendDepth);

    return true;
}

FSageToolDispatch::FOutcome SetAnimNodePropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, NodeId, PropName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'node_id'"));
    }
    if (!Args->TryGetStringField(TEXT("property"), PropName) || PropName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'property'"));
    }
    TSharedPtr<FJsonValue> Val = Args->TryGetField(TEXT("value"));
    if (!Val.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }
    UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(
        FindGraphNodeByGuid(TargetGraph, NodeId));
    if (!AnimNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id %s isn't a UAnimGraphNode_Base"), *NodeId));
    }

    // Property may live either on the wrapper UObject or inside the inner
    // FAnimNode_* struct. Try wrapper first (rare, e.g. node display flags),
    // then descend into the Node struct.
    FProperty* TargetProp = AnimNode->GetClass()->FindPropertyByName(FName(*PropName));
    void* TargetContainer = AnimNode;

    if (!TargetProp)
    {
        FStructProperty* OuterStruct = nullptr;
        void* StructPtr = nullptr;
        if (GetAnimNodeStructTarget(AnimNode, OuterStruct, StructPtr) && OuterStruct)
        {
            TargetProp = OuterStruct->Struct->FindPropertyByName(FName(*PropName));
            TargetContainer = StructPtr;
        }
    }

    if (!TargetProp)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("property '%s' not found on node class %s"),
                            *PropName, *AnimNode->GetClass()->GetName()));
    }

    // Reject `value: null` unless the target prop is an object reference
    // (FObjectProperty / FSoftObjectProperty etc). For numeric / bool / FName /
    // struct fields, a JSON null is almost always a client mistake — silently
    // coercing it to 0/false/NAME_None corrupts the AnimBP under the user's
    // nose (lessons.md "silent fail anti-pattern").
    if (Val->Type == EJson::Null && !TargetProp->IsA<FObjectPropertyBase>())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("property '%s' (%s) does not accept null; pass an explicit value"),
                            *PropName, *TargetProp->GetClass()->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("SetAnimNodeProp", "Sage: Set Anim Node Property"));
    AnimBP->Modify();
    AnimNode->Modify();

    void* TargetValuePtr = TargetProp->ContainerPtrToValuePtr<void>(TargetContainer);
    bool bWritten = false;
    bool bLayerSetupWriteAttempted = false;
    FString LayerSetupWriteError;
    if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(TargetProp))
    {
        if ((PropName == TEXT("LayerSetup"))
            && (AnimNode->GetClass()->GetName() == TEXT("AnimGraphNode_LayeredBoneBlend")))
        {
            bLayerSetupWriteAttempted = true;
            bWritten = SetLayeredBoneBlendLayerSetup(ArrayProperty, TargetValuePtr, Val, LayerSetupWriteError);
        }
    }
    if (!bWritten)
    {
        bWritten = detail::SetPropertyValueAtPtr(TargetProp, TargetValuePtr, Val);
    }
    if (!bWritten)
    {
        Tx.Cancel();
        if (bLayerSetupWriteAttempted && !LayerSetupWriteError.IsEmpty())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, LayerSetupWriteError);
        }
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("could not coerce JSON value into property %s (%s)"),
                            *PropName, *TargetProp->GetClass()->GetName()));
    }

    // Fire UE's PostEditChangeProperty pipeline so node-class hooks
    // (e.g. UAnimGraphNode_LinkedAnimLayer's ChangeLayer for Layer FName,
    // UAnimGraphNode_StateMachine's sub-graph rebind, etc.) run. Without
    // this, raw CDO writes are invisible to the compile path even when the
    // value is present at the property level (Lyra Sage Gap #28). Use the
    // wrapper-class property name when available so the event reflects what
    // the AnimGraphNode actually saw.
    {
        FProperty* EventProp = AnimNode->GetClass()->FindPropertyByName(FName(*PropName));
        if (!EventProp) EventProp = TargetProp;  // inner-struct fallback
        FPropertyChangedEvent ChangeEvent(EventProp, EPropertyChangeType::ValueSet);
        AnimNode->PostEditChangeProperty(ChangeEvent);
    }
    AnimNode->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"),  NodeId);
    R->SetStringField(TEXT("property"), PropName);
    R->SetBoolField  (TEXT("set"),      true);
    R->SetStringField(TEXT("node_title"),
        AnimNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool ParseLayeredBoneBlendMode(const FString& Text,
                               ELayeredBoneBlendMode& OutMode,
                               FString& OutErr)
{
    const FString Clean = Text.TrimStartAndEnd();
    if (Clean.Equals(TEXT("BlendMask"), ESearchCase::IgnoreCase)
        || Clean.EndsWith(TEXT("::BlendMask"), ESearchCase::IgnoreCase))
    {
        OutMode = ELayeredBoneBlendMode::BlendMask;
        return true;
    }
    if (Clean.Equals(TEXT("BranchFilter"), ESearchCase::IgnoreCase)
        || Clean.EndsWith(TEXT("::BranchFilter"), ESearchCase::IgnoreCase))
    {
        OutMode = ELayeredBoneBlendMode::BranchFilter;
        return true;
    }
    OutErr = FString::Printf(TEXT("invalid blend_mode '%s'; expected BlendMask or BranchFilter"),
                             *Text);
    return false;
}

FString LayeredBoneBlendModeToString(ELayeredBoneBlendMode Mode)
{
    return Mode == ELayeredBoneBlendMode::BlendMask
        ? TEXT("BlendMask")
        : TEXT("BranchFilter");
}

bool ParseCurveBlendOption(const FString& Text,
                           ECurveBlendOption::Type& OutOption,
                           FString& OutErr)
{
    const FString Clean = Text.TrimStartAndEnd();
    struct FEntry { const TCHAR* Name; ECurveBlendOption::Type Value; };
    const FEntry Entries[] = {
        { TEXT("Override"), ECurveBlendOption::Override },
        { TEXT("DoNotOverride"), ECurveBlendOption::DoNotOverride },
        { TEXT("NormalizeByWeight"), ECurveBlendOption::NormalizeByWeight },
        { TEXT("BlendByWeight"), ECurveBlendOption::BlendByWeight },
        { TEXT("UseBasePose"), ECurveBlendOption::UseBasePose },
        { TEXT("UseMaxValue"), ECurveBlendOption::UseMaxValue },
        { TEXT("UseMinValue"), ECurveBlendOption::UseMinValue },
    };
    for (const FEntry& Entry : Entries)
    {
        if (Clean.Equals(Entry.Name, ESearchCase::IgnoreCase)
            || Clean.EndsWith(FString::Printf(TEXT("::%s"), Entry.Name),
                              ESearchCase::IgnoreCase))
        {
            OutOption = Entry.Value;
            return true;
        }
    }
    OutErr = FString::Printf(TEXT("invalid curve_blend_option '%s'"), *Text);
    return false;
}

FString CurveBlendOptionToString(ECurveBlendOption::Type Option)
{
    switch (Option)
    {
    case ECurveBlendOption::Override: return TEXT("Override");
    case ECurveBlendOption::DoNotOverride: return TEXT("DoNotOverride");
    case ECurveBlendOption::NormalizeByWeight: return TEXT("NormalizeByWeight");
    case ECurveBlendOption::BlendByWeight: return TEXT("BlendByWeight");
    case ECurveBlendOption::UseBasePose: return TEXT("UseBasePose");
    case ECurveBlendOption::UseMaxValue: return TEXT("UseMaxValue");
    case ECurveBlendOption::UseMinValue: return TEXT("UseMinValue");
    default: return FString::FromInt(static_cast<int32>(Option));
    }
}

UBlendProfile* ResolveBlendProfileReference(const FString& RawPath,
                                            FString& OutErr)
{
    const FString Clean = CleanObjectReferenceLiteral(RawPath);
    if (Clean.IsEmpty())
    {
        OutErr = TEXT("empty blend profile path");
        return nullptr;
    }

    if (UObject* Obj = ResolveAsset(Clean))
    {
        if (UBlendProfile* Profile = Cast<UBlendProfile>(Obj))
        {
            if (!Profile->IsBlendMask())
            {
                OutErr = FString::Printf(TEXT("%s is a UBlendProfile but not a BlendMask"),
                                         *Clean);
                return nullptr;
            }
            return Profile;
        }
        if (!Clean.Contains(TEXT(":")))
        {
            OutErr = FString::Printf(TEXT("%s resolved to %s, not UBlendProfile"),
                                     *Clean, *Obj->GetClass()->GetPathName());
            return nullptr;
        }
        // Some subobject paths resolve the owner asset before the profile
        // subobject is materialized. Continue into the owner/profile-name path.
    }

    FString OwnerPath;
    FString ProfileName;
    if (!Clean.Split(TEXT(":"), &OwnerPath, &ProfileName,
                     ESearchCase::CaseSensitive, ESearchDir::FromEnd)
        || OwnerPath.IsEmpty() || ProfileName.IsEmpty())
    {
        OutErr = FString::Printf(TEXT("blend profile not found: %s"), *Clean);
        return nullptr;
    }

    UObject* Owner = ResolveAssetOrPackage(OwnerPath);
    USkeleton* Skeleton = Cast<USkeleton>(Owner);
    if (!Skeleton)
    {
        if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Owner))
        {
            Skeleton = Mesh->GetSkeleton();
        }
    }
    if (!Skeleton)
    {
        OutErr = FString::Printf(TEXT("blend profile owner %s is not a USkeleton or USkeletalMesh"),
                                 *OwnerPath);
        return nullptr;
    }

    UBlendProfile* Profile = Skeleton->GetBlendProfile(FName(*ProfileName));
    if (!Profile)
    {
        OutErr = FString::Printf(TEXT("blend profile %s not found on skeleton %s"),
                                 *ProfileName, *Skeleton->GetPathName());
        return nullptr;
    }
    if (!Profile->IsBlendMask())
    {
        OutErr = FString::Printf(TEXT("%s:%s is a UBlendProfile but not a BlendMask"),
                                 *OwnerPath, *ProfileName);
        return nullptr;
    }
    return Profile;
}

bool ParseBlendMaskArray(const TSharedPtr<FJsonValue>& Value,
                         TArray<TObjectPtr<UBlendProfile>>& OutMasks,
                         TArray<TSharedPtr<FJsonValue>>& OutResolved,
                         FString& OutErr)
{
    if (!Value.IsValid() || Value->Type != EJson::Array)
    {
        OutErr = TEXT("blend_masks must be an array of UBlendProfile paths");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
    OutMasks.Reset();
    OutResolved.Reset();
    for (int32 i = 0; i < Arr.Num(); ++i)
    {
        FString Path;
        if (!Arr[i].IsValid() || !Arr[i]->TryGetString(Path))
        {
            OutErr = FString::Printf(TEXT("blend_masks[%d] must be a string path"), i);
            return false;
        }
        UBlendProfile* Profile = ResolveBlendProfileReference(Path, OutErr);
        if (!Profile) return false;
        OutMasks.Add(Profile);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("requested_path"), Path);
        Row->SetStringField(TEXT("resolved_path"), FSoftObjectPath(Profile).ToString());
        Row->SetStringField(TEXT("name"), Profile->GetName());
        OutResolved.Add(MakeShared<FJsonValueObject>(Row));
    }
    return true;
}

bool ParseFloatArrayValue(const TSharedPtr<FJsonValue>& Value,
                          TArray<float>& OutValues,
                          FString& OutErr)
{
    if (!Value.IsValid() || Value->Type != EJson::Array)
    {
        OutErr = TEXT("blend_weights must be an array of numbers");
        return false;
    }
    OutValues.Reset();
    const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
    for (int32 i = 0; i < Arr.Num(); ++i)
    {
        if (!Arr[i].IsValid())
        {
            OutErr = FString::Printf(TEXT("blend_weights[%d] is null"), i);
            return false;
        }
        OutValues.Add(static_cast<float>(Arr[i]->AsNumber()));
    }
    return true;
}

bool ParseBranchFilterObject(const TSharedPtr<FJsonObject>& Obj,
                             FBranchFilter& OutFilter,
                             FString& OutErr)
{
    if (!Obj.IsValid())
    {
        OutErr = TEXT("BranchFilter entry must be an object");
        return false;
    }
    FString BoneName;
    if (!Obj->TryGetStringField(TEXT("BoneName"), BoneName))
    {
        Obj->TryGetStringField(TEXT("bone_name"), BoneName);
    }
    if (BoneName.IsEmpty())
    {
        OutErr = TEXT("BranchFilter entry missing BoneName");
        return false;
    }
    double Depth = 0.0;
    if (!Obj->TryGetNumberField(TEXT("BlendDepth"), Depth))
    {
        Obj->TryGetNumberField(TEXT("blend_depth"), Depth);
    }
    OutFilter.BoneName = FName(*BoneName);
    OutFilter.BlendDepth = static_cast<int32>(Depth);
    return true;
}

bool ParseLayerSetupArray(const TSharedPtr<FJsonValue>& Value,
                          TArray<FInputBlendPose>& OutLayerSetup,
                          FString& OutErr)
{
    OutLayerSetup.Reset();
    if (!Value.IsValid())
    {
        OutErr = TEXT("layer_setup is null");
        return false;
    }
    if (Value->Type == EJson::String)
    {
        FString Text = Value->AsString().TrimStartAndEnd();
        if (Text.Len() >= 2 && Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\"")))
        {
            Text = Text.Mid(1, Text.Len() - 2);
            Text.ReplaceInline(TEXT("\\\""), TEXT("\""));
        }
        FString BoneNameText;
        if (!ExtractLayerSetupValue(Text, TEXT("BoneName"), BoneNameText))
        {
            BoneNameText = TEXT("spine_01");
        }
        FString BlendDepthText;
        int32 BlendDepth = 10;
        if (ExtractLayerSetupValue(Text, TEXT("BlendDepth"), BlendDepthText))
        {
            BlendDepth = FCString::Atoi(*BlendDepthText);
        }
        FBranchFilter Filter;
        Filter.BoneName = FName(*BoneNameText);
        Filter.BlendDepth = BlendDepth;
        FInputBlendPose Pose;
        Pose.BranchFilters.Add(Filter);
        OutLayerSetup.Add(Pose);
        return true;
    }
    if (Value->Type != EJson::Array)
    {
        OutErr = TEXT("layer_setup must be an array of {BranchFilters:[...]} entries or an ImportText string");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>& Layers = Value->AsArray();
    for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
    {
        if (!Layers[LayerIndex].IsValid() || Layers[LayerIndex]->Type != EJson::Object)
        {
            OutErr = FString::Printf(TEXT("layer_setup[%d] must be an object"), LayerIndex);
            return false;
        }
        const TSharedPtr<FJsonObject> LayerObj = Layers[LayerIndex]->AsObject();
        const TArray<TSharedPtr<FJsonValue>>* Filters = nullptr;
        if (!LayerObj->TryGetArrayField(TEXT("BranchFilters"), Filters))
        {
            LayerObj->TryGetArrayField(TEXT("branch_filters"), Filters);
        }
        if (!Filters)
        {
            OutErr = FString::Printf(TEXT("layer_setup[%d] missing BranchFilters"), LayerIndex);
            return false;
        }
        FInputBlendPose Pose;
        for (int32 FilterIndex = 0; FilterIndex < Filters->Num(); ++FilterIndex)
        {
            if (!(*Filters)[FilterIndex].IsValid() || (*Filters)[FilterIndex]->Type != EJson::Object)
            {
                OutErr = FString::Printf(TEXT("layer_setup[%d].BranchFilters[%d] must be an object"),
                                         LayerIndex, FilterIndex);
                return false;
            }
            FBranchFilter Filter;
            if (!ParseBranchFilterObject((*Filters)[FilterIndex]->AsObject(), Filter, OutErr))
            {
                return false;
            }
            Pose.BranchFilters.Add(Filter);
        }
        OutLayerSetup.Add(Pose);
    }
    return true;
}

TArray<TSharedPtr<FJsonValue>> BlendMasksToJson(const TArray<TObjectPtr<UBlendProfile>>& Masks)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const TObjectPtr<UBlendProfile>& Mask : Masks)
    {
        Arr.Add(MakeShared<FJsonValueString>(
            Mask ? FSoftObjectPath(Mask.Get()).ToString() : FString()));
    }
    return Arr;
}

TArray<TSharedPtr<FJsonValue>> BlendWeightsToJson(const TArray<float>& Weights)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (float Weight : Weights)
    {
        Arr.Add(MakeShared<FJsonValueNumber>(Weight));
    }
    return Arr;
}

TArray<TSharedPtr<FJsonValue>> LayerSetupToJson(const TArray<FInputBlendPose>& LayerSetup)
{
    TArray<TSharedPtr<FJsonValue>> Layers;
    for (const FInputBlendPose& Pose : LayerSetup)
    {
        auto LayerObj = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Filters;
        for (const FBranchFilter& Filter : Pose.BranchFilters)
        {
            auto FilterObj = MakeShared<FJsonObject>();
            FilterObj->SetStringField(TEXT("BoneName"), Filter.BoneName.ToString());
            FilterObj->SetNumberField(TEXT("BlendDepth"), Filter.BlendDepth);
            Filters.Add(MakeShared<FJsonValueObject>(FilterObj));
        }
        LayerObj->SetArrayField(TEXT("BranchFilters"), Filters);
        Layers.Add(MakeShared<FJsonValueObject>(LayerObj));
    }
    return Layers;
}

TSharedPtr<FJsonObject> LayeredBoneBlendReadbackJson(const FAnimNode_LayeredBoneBlend& Node)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("BlendMode"), LayeredBoneBlendModeToString(Node.BlendMode));
    Obj->SetArrayField(TEXT("BlendMasks"), BlendMasksToJson(Node.BlendMasks));
    Obj->SetArrayField(TEXT("BlendWeights"), BlendWeightsToJson(Node.BlendWeights));
    Obj->SetArrayField(TEXT("LayerSetup"), LayerSetupToJson(Node.LayerSetup));
    Obj->SetBoolField(TEXT("bMeshSpaceRotationBlend"), Node.bMeshSpaceRotationBlend);
    Obj->SetBoolField(TEXT("bRootSpaceRotationBlend"), Node.bRootSpaceRotationBlend);
    Obj->SetBoolField(TEXT("bMeshSpaceScaleBlend"), Node.bMeshSpaceScaleBlend);
    Obj->SetStringField(TEXT("CurveBlendOption"),
        CurveBlendOptionToString(Node.CurveBlendOption.GetValue()));
    Obj->SetNumberField(TEXT("blend_pose_count"), Node.BlendPoses.Num());
    return Obj;
}

FSageToolDispatch::FOutcome SetLayeredBoneBlendConfigImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, NodeId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'node_id'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }
    UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(
        FindGraphNodeByGuid(TargetGraph, NodeId));
    if (!AnimNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id %s isn't a UAnimGraphNode_Base"), *NodeId));
    }

    FStructProperty* NodeStructProp = nullptr;
    void* NodeStructPtr = nullptr;
    if (!GetAnimNodeStructTarget(AnimNode, NodeStructProp, NodeStructPtr)
        || !NodeStructProp || !NodeStructPtr
        || NodeStructProp->Struct != FAnimNode_LayeredBoneBlend::StaticStruct())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id %s is %s, not FAnimNode_LayeredBoneBlend"),
                            *NodeId,
                            NodeStructProp && NodeStructProp->Struct
                                ? *NodeStructProp->Struct->GetPathName()
                                : TEXT("<unknown>")));
    }

    FAnimNode_LayeredBoneBlend* LBB =
        reinterpret_cast<FAnimNode_LayeredBoneBlend*>(NodeStructPtr);
    const FAnimNode_LayeredBoneBlend OldNode = *LBB;
    const int32 PoseCount = LBB->BlendPoses.Num();

    FString Err;
    ELayeredBoneBlendMode TargetMode = LBB->BlendMode;
    FString ModeText;
    if (Args->TryGetStringField(TEXT("blend_mode"), ModeText) && !ModeText.IsEmpty())
    {
        if (!ParseLayeredBoneBlendMode(ModeText, TargetMode, Err))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
        }
    }
    else if (Args->HasField(TEXT("blend_masks")))
    {
        TargetMode = ELayeredBoneBlendMode::BlendMask;
    }
    else if (Args->HasField(TEXT("layer_setup")))
    {
        TargetMode = ELayeredBoneBlendMode::BranchFilter;
    }

    TArray<TObjectPtr<UBlendProfile>> NewBlendMasks = LBB->BlendMasks;
    TArray<TSharedPtr<FJsonValue>> ResolvedMasks;
    if (Args->HasField(TEXT("blend_masks")))
    {
        if (!ParseBlendMaskArray(Args->TryGetField(TEXT("blend_masks")),
                                 NewBlendMasks, ResolvedMasks, Err))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
        }
    }
    if (TargetMode == ELayeredBoneBlendMode::BlendMask)
    {
        if (!Args->HasField(TEXT("blend_masks")))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("blend_masks is required when blend_mode=BlendMask"));
        }
        if (NewBlendMasks.Num() != PoseCount)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("blend_masks length %d does not match blend pose count %d"),
                                NewBlendMasks.Num(), PoseCount));
        }
    }

    TArray<FInputBlendPose> NewLayerSetup = LBB->LayerSetup;
    if (Args->HasField(TEXT("layer_setup")))
    {
        if (!ParseLayerSetupArray(Args->TryGetField(TEXT("layer_setup")),
                                  NewLayerSetup, Err))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
        }
    }
    if (TargetMode == ELayeredBoneBlendMode::BranchFilter)
    {
        if (NewLayerSetup.Num() != PoseCount)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("layer_setup length %d does not match blend pose count %d"),
                                NewLayerSetup.Num(), PoseCount));
        }
    }

    TArray<float> NewBlendWeights = LBB->BlendWeights;
    if (Args->HasField(TEXT("blend_weights")))
    {
        if (!ParseFloatArrayValue(Args->TryGetField(TEXT("blend_weights")),
                                  NewBlendWeights, Err))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
        }
    }
    else if (NewBlendWeights.Num() != PoseCount)
    {
        NewBlendWeights.Init(1.0f, PoseCount);
    }
    if (NewBlendWeights.Num() != PoseCount)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("blend_weights length %d does not match blend pose count %d"),
                            NewBlendWeights.Num(), PoseCount));
    }

    ECurveBlendOption::Type NewCurveBlendOption = LBB->CurveBlendOption.GetValue();
    FString CurveOptionText;
    if (Args->TryGetStringField(TEXT("curve_blend_option"), CurveOptionText)
        && !CurveOptionText.IsEmpty())
    {
        if (!ParseCurveBlendOption(CurveOptionText, NewCurveBlendOption, Err))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
        }
    }

    bool bMeshSpaceRotationBlend = LBB->bMeshSpaceRotationBlend;
    bool bRootSpaceRotationBlend = LBB->bRootSpaceRotationBlend;
    bool bMeshSpaceScaleBlend = LBB->bMeshSpaceScaleBlend;
    Args->TryGetBoolField(TEXT("mesh_space_rotation_blend"), bMeshSpaceRotationBlend);
    Args->TryGetBoolField(TEXT("root_space_rotation_blend"), bRootSpaceRotationBlend);
    Args->TryGetBoolField(TEXT("mesh_space_scale_blend"), bMeshSpaceScaleBlend);

    bool bCompile = false;
    bool bSave = false;
    bool bDryRun = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"), NodeId);
    R->SetStringField(TEXT("graph"), TargetGraph->GetFName().ToString());
    R->SetStringField(TEXT("node_class"), AnimNode->GetClass()->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetObjectField(TEXT("before"), LayeredBoneBlendReadbackJson(*LBB));
    R->SetArrayField(TEXT("resolved_blend_masks"), ResolvedMasks);
    if (bDryRun)
    {
        R->SetBoolField(TEXT("modified"), false);
        R->SetBoolField(TEXT("validated"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SetLayeredBoneBlendConfig", "Sage: Set Layered Bone Blend Config"));
    AnimBP->Modify();
    AnimNode->Modify();

    LBB->BlendMode = TargetMode;
    if (TargetMode == ELayeredBoneBlendMode::BlendMask)
    {
        LBB->BlendMasks = NewBlendMasks;
        LBB->LayerSetup.Reset();
    }
    else
    {
        LBB->LayerSetup = NewLayerSetup;
        LBB->BlendMasks.Reset();
    }
    LBB->BlendWeights = NewBlendWeights;
    LBB->bMeshSpaceRotationBlend = bMeshSpaceRotationBlend;
    LBB->bRootSpaceRotationBlend = bRootSpaceRotationBlend;
    LBB->bMeshSpaceScaleBlend = bMeshSpaceScaleBlend;
    LBB->CurveBlendOption = NewCurveBlendOption;
    LBB->InvalidatePerBoneBlendWeights();

    FProperty* NodeProperty = AnimNode->GetClass()->FindPropertyByName(TEXT("Node"));
    if (NodeProperty)
    {
        FPropertyChangedEvent ChangeEvent(NodeProperty, EPropertyChangeType::ValueSet);
        AnimNode->PostEditChangeProperty(ChangeEvent);
    }
    else
    {
        AnimNode->PostEditChange();
    }
    AnimNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    AnimBP->MarkPackageDirty();
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(AnimBP);
    }

    bool bSaved = false;
    if (bSave)
    {
        UEditorAssetSubsystem* AssetSubsystem = GEditor
            ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
            : nullptr;
        if (!AssetSubsystem)
        {
            *LBB = OldNode;
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                TEXT("EditorAssetSubsystem unavailable for saving AnimBlueprint"));
        }
        const FString Package = PackagePathForObjectPath(FSoftObjectPath(AnimBP).ToString());
        bSaved = AssetSubsystem->SaveAsset(Package, /*bOnlyIfIsDirty=*/false);
        if (!bSaved)
        {
            *LBB = OldNode;
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                FString::Printf(TEXT("SaveAsset returned false for %s"), *Package));
        }
    }

    R->SetBoolField(TEXT("modified"), true);
    R->SetBoolField(TEXT("compiled"), bCompile);
    R->SetBoolField(TEXT("saved"), bSaved);
    R->SetObjectField(TEXT("after"), LayeredBoneBlendReadbackJson(*LBB));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.bind_anim_node_property
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome BindAnimNodePropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, NodeId, PropName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'node_id'"));
    }
    if (!Args->TryGetStringField(TEXT("property"), PropName) || PropName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'property'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);

    TArray<FString> BindingPath;
    FString ParseError;
    if (!ParseAnimNodeBindingExpression(Args, BindingPath, ParseError))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, ParseError);
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }
    UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(
        FindGraphNodeByGuid(TargetGraph, NodeId));
    if (!AnimNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id %s isn't a UAnimGraphNode_Base"), *NodeId));
    }

    const FName PinName(*PropName);
    FResolvedAnimNodePinBinding PinInfo;
    if (!ResolveAnimNodePinBinding(AnimNode, PinName, PinInfo))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("property '%s' is not a bindable AnimGraph input pin property on %s"),
                            *PropName, *AnimNode->GetClass()->GetName()));
    }

    FAnimGraphNodePropertyBinding PropertyBinding;
    FString BindingError;
    if (!BuildAnimNodePropertyBinding(AnimBP, AnimNode, PinName, PinInfo.BindingName,
                                      BindingPath, PropertyBinding, BindingError))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, BindingError);
    }

    FScopedTransaction Tx(LOCTEXT("BindAnimNodeProp", "Sage: Bind Anim Node Property"));
    AnimBP->Modify();
    TargetGraph->Modify();
    AnimNode->Modify();

    FString ObjectError;
    UObject* BindingObj = GetOrCreateAnimNodeBindingObject(AnimBP, AnimNode, ObjectError);
    if (!BindingObj)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, ObjectError);
    }

    FMapProperty* MapProp = nullptr;
    FStructProperty* ValueStruct = nullptr;
    FString MapError;
    if (!GetAnimNodeBindingMap(BindingObj, MapProp, ValueStruct, MapError))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, MapError);
    }
    BindingObj->Modify();

    bool bPinWasVisible = false;
    bool bPinExposed = false;
    const int32 OptionalPinIndex = PinInfo.OptionalPinIndex;
    if (PinInfo.bOptionalPin)
    {
        bPinWasVisible = PinInfo.bPinVisible;
        if (!bPinWasVisible)
        {
            bPinExposed = SetResolvedAnimNodePinVisible(AnimNode, PinInfo, /*bReconstruct=*/true);
        }
    }

    if (UEdGraphPin* Pin = AnimNode->FindPin(PinName))
    {
        Pin->BreakAllPinLinks();
    }

    AnimNode->RemoveBindings(PinInfo.BindingName);
    FString AddError;
    if (!AddBindingMapEntry(BindingObj, MapProp, ValueStruct,
                            PinInfo.BindingName, PropertyBinding, AddError))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, AddError);
    }

    AnimNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    TArray<TSharedPtr<FJsonValue>> Bindings;
    int32 BindingCount = 0;
    ReadBindingMapEntries(BindingObj, Bindings, BindingCount);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"), NodeId);
    R->SetStringField(TEXT("graph"), TargetGraph->GetFName().ToString());
    R->SetStringField(TEXT("property"), PropName);
    R->SetStringField(TEXT("binding_name"), PinInfo.BindingName.ToString());
    R->SetStringField(TEXT("path"), BindingPathToString(BindingPath));
    R->SetStringField(TEXT("binding_class"), BindingObj->GetClass()->GetPathName());
    R->SetBoolField(TEXT("bound"), true);
    R->SetBoolField(TEXT("pin_was_visible"), bPinWasVisible);
    R->SetBoolField(TEXT("pin_exposed"), bPinExposed);
    R->SetBoolField(TEXT("custom_property_pin"), PinInfo.bCustomPropertyPin);
    R->SetNumberField(TEXT("optional_pin_index"), OptionalPinIndex);
    R->SetNumberField(TEXT("binding_count"), BindingCount);
    R->SetObjectField(TEXT("binding"), BindingToJson(PinInfo.BindingName, PropertyBinding));
    R->SetArrayField(TEXT("bindings"), Bindings);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.read_anim_node_properties
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome ReadAnimNodePropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, NodeId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'node_id'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }
    UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(
        FindGraphNodeByGuid(TargetGraph, NodeId));
    if (!AnimNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id %s isn't a UAnimGraphNode_Base"), *NodeId));
    }

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (const UEdGraphPin* Pin : AnimNode->Pins)
    {
        Pins.Add(MakeShared<FJsonValueObject>(PinSummaryJson(Pin)));
    }

    TArray<TSharedPtr<FJsonValue>> Properties;
    FStructProperty* NodeStructProp = nullptr;
    void* NodeStructPtr = nullptr;
    if (GetAnimNodeStructTarget(AnimNode, NodeStructProp, NodeStructPtr)
        && NodeStructProp && NodeStructProp->Struct)
    {
        for (TFieldIterator<FProperty> It(NodeStructProp->Struct); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property) continue;
            Properties.Add(MakeShared<FJsonValueObject>(
                AnimNodePropertyToJson(AnimNode, Property, NodeStructPtr)));
        }
    }
    AppendCustomAnimNodePropertiesToJson(AnimNode, Properties);

    TArray<TSharedPtr<FJsonValue>> Bindings;
    int32 BindingCount = 0;
    UObject* BindingObj = GetAnimNodeBindingObject(AnimNode);
    ReadBindingMapEntries(BindingObj, Bindings, BindingCount);
    const TArray<TSharedPtr<FJsonValue>> AssetRefs =
        CollectAnimNodeAssetReferences(AnimNode);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"), NodeId);
    R->SetStringField(TEXT("graph"), TargetGraph->GetFName().ToString());
    R->SetStringField(TEXT("node_class"), AnimNode->GetClass()->GetPathName());
    if (UScriptStruct* FNodeType = AnimNode->GetFNodeType())
    {
        R->SetStringField(TEXT("fnode_type"), FNodeType->GetPathName());
    }
    if (NodeStructProp && NodeStructProp->Struct)
    {
        R->SetStringField(TEXT("inner_struct"), NodeStructProp->Struct->GetPathName());
    }
    if (BindingObj)
    {
        R->SetStringField(TEXT("binding_class"), BindingObj->GetClass()->GetPathName());
    }
    R->SetNumberField(TEXT("pin_count"), Pins.Num());
    R->SetNumberField(TEXT("property_count"), Properties.Num());
    R->SetNumberField(TEXT("binding_count"), BindingCount);
    R->SetNumberField(TEXT("animation_asset_count"), AssetRefs.Num());
    R->SetArrayField(TEXT("pins"), Pins);
    R->SetArrayField(TEXT("properties"), Properties);
    R->SetArrayField(TEXT("bindings"), Bindings);
    R->SetArrayField(TEXT("animation_assets"), AssetRefs);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool ParseRetargetSourceMode(const FString& Raw, ERetargetSourceMode& Out)
{
    const FString Key = NormalizeToken(Raw);
    if (Key.IsEmpty() || Key == TEXT("parent") || Key == TEXT("parentskeletalmeshcomponent"))
    {
        Out = ERetargetSourceMode::ParentSkeletalMeshComponent;
        return true;
    }
    if (Key == TEXT("custom") || Key == TEXT("customskeletalmeshcomponent") || Key == TEXT("component"))
    {
        Out = ERetargetSourceMode::CustomSkeletalMeshComponent;
        return true;
    }
    if (Key == TEXT("pose") || Key == TEXT("sourcepose") || Key == TEXT("sourceposepin") || Key == TEXT("pin"))
    {
        Out = ERetargetSourceMode::SourcePosePin;
        return true;
    }
    return false;
}

FString RetargetSourceModeToString(ERetargetSourceMode Mode)
{
    switch (Mode)
    {
    case ERetargetSourceMode::ParentSkeletalMeshComponent: return TEXT("ParentSkeletalMeshComponent");
    case ERetargetSourceMode::CustomSkeletalMeshComponent: return TEXT("CustomSkeletalMeshComponent");
    case ERetargetSourceMode::SourcePosePin:               return TEXT("SourcePosePin");
    default:                                               return TEXT("Unknown");
    }
}

TSharedRef<FJsonObject> RetargetProfileToJson(const FRetargetProfile& Profile)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetBoolField(TEXT("apply_target_pose"), Profile.bApplyTargetRetargetPose);
    Obj->SetStringField(TEXT("target_pose"), Profile.TargetRetargetPoseName.ToString());
    Obj->SetBoolField(TEXT("apply_source_pose"), Profile.bApplySourceRetargetPose);
    Obj->SetStringField(TEXT("source_pose"), Profile.SourceRetargetPoseName.ToString());
    Obj->SetBoolField(TEXT("force_ik_off"), Profile.bForceAllIKOff);
    TArray<TSharedPtr<FJsonValue>> OpProfiles;
    for (const FRetargetOpProfile& OpProfile : Profile.RetargetOpProfiles)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("op_name"), OpProfile.OpToApplySettingsTo.ToString());
        Row->SetStringField(TEXT("settings_type"), OpProfile.SettingsToApply.GetScriptStruct()
            ? OpProfile.SettingsToApply.GetScriptStruct()->GetPathName()
            : FString());
        OpProfiles.Add(MakeShared<FJsonValueObject>(Row));
    }
    Obj->SetArrayField(TEXT("op_profiles"), OpProfiles);
    Obj->SetNumberField(TEXT("op_profile_count"), OpProfiles.Num());
    return Obj;
}

bool ResolveRetargetPoseFromMeshNode(
    const TSharedPtr<FJsonObject>& Args,
    UAnimBlueprint*& OutAnimBP,
    UEdGraph*& OutGraph,
    UAnimGraphNode_Base*& OutGraphNode,
    FAnimNode_RetargetPoseFromMesh*& OutNode,
    FString& OutError)
{
    FString Path, GraphName, NodeId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        OutError = TEXT("missing 'path'");
        return false;
    }
    if (!Args->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    {
        OutError = TEXT("missing 'node_id'");
        return false;
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);

    OutAnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!OutAnimBP)
    {
        OutError = FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path);
        return false;
    }
    OutGraph = ResolveAnimGraphTarget(OutAnimBP, GraphName);
    if (!OutGraph)
    {
        OutError = FString::Printf(TEXT("graph '%s' not found"), *GraphName);
        return false;
    }
    OutGraphNode = Cast<UAnimGraphNode_Base>(FindGraphNodeByGuid(OutGraph, NodeId));
    if (!OutGraphNode)
    {
        OutError = FString::Printf(TEXT("node_id %s isn't a UAnimGraphNode_Base"), *NodeId);
        return false;
    }

    FStructProperty* NodeStructProp = nullptr;
    void* NodeStructPtr = nullptr;
    if (!GetAnimNodeStructTarget(OutGraphNode, NodeStructProp, NodeStructPtr) ||
        !NodeStructProp ||
        !NodeStructProp->Struct ||
        !NodeStructProp->Struct->IsChildOf(FAnimNode_RetargetPoseFromMesh::StaticStruct()))
    {
        OutError = FString::Printf(TEXT("node_id %s is not Retarget Pose From Mesh"), *NodeId);
        return false;
    }
    OutNode = reinterpret_cast<FAnimNode_RetargetPoseFromMesh*>(NodeStructPtr);
    return OutNode != nullptr;
}

// ---------------------------------------------------------------------------
// animation.read_retarget_pose_from_mesh_node
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadRetargetPoseFromMeshNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    UAnimGraphNode_Base* GraphNode = nullptr;
    FAnimNode_RetargetPoseFromMesh* Node = nullptr;
    FString Error;
    if (!ResolveRetargetPoseFromMeshNode(Args, AnimBP, Graph, GraphNode, Node, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    FSageToolDispatch::FOutcome GenericRead = ReadAnimNodePropertiesImpl(Args);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), AnimBP->GetPathName());
    R->SetStringField(TEXT("graph"), Graph->GetFName().ToString());
    R->SetStringField(TEXT("node_id"), GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    R->SetStringField(TEXT("node_class"), GraphNode->GetClass()->GetPathName());
    R->SetStringField(TEXT("retarget_from"), RetargetSourceModeToString(Node->RetargetFrom));
    R->SetStringField(TEXT("ik_retargeter"), Node->IKRetargeterAsset ? Node->IKRetargeterAsset->GetPathName() : FString());
    R->SetNumberField(TEXT("lod_threshold"), Node->LODThreshold);
    R->SetNumberField(TEXT("ik_lod_threshold"), Node->LODThresholdForIK);
    R->SetBoolField(TEXT("suppress_warnings"), Node->bSuppressWarnings);
    R->SetObjectField(TEXT("custom_retarget_profile"), RetargetProfileToJson(Node->CustomRetargetProfile));
    if (GenericRead.bSuccess && GenericRead.Result.IsValid())
    {
        R->SetObjectField(TEXT("generic_readback"), GenericRead.Result.ToSharedRef());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_retarget_pose_from_mesh_node
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetRetargetPoseFromMeshNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    UAnimGraphNode_Base* GraphNode = nullptr;
    FAnimNode_RetargetPoseFromMesh* Node = nullptr;
    FString Error;
    if (!ResolveRetargetPoseFromMeshNode(Args, AnimBP, Graph, GraphNode, Node, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    bool bDryRun = false;
    bool bCompile = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FString RetargeterPath;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("ik_retargeter"), TEXT("retargeter"), TEXT("ik_retargeter_asset") }, RetargeterPath);
    FString SourceModeString;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("source_mode"), TEXT("retarget_from") }, SourceModeString);
    bool bSuppressWarnings = Node->bSuppressWarnings;
    bool bHaveSuppressWarnings = TryGetAnyBoolField(Args, TArray<const TCHAR*>{ TEXT("suppress_warnings"), TEXT("bSuppressWarnings") }, bSuppressWarnings);
    bool bExposeSourceMeshPin = false;
    Args->TryGetBoolField(TEXT("expose_source_mesh_pin"), bExposeSourceMeshPin);

    int32 LODThreshold = Node->LODThreshold;
    int32 IKLODThreshold = Node->LODThresholdForIK;
    TryGetAnyIntField(Args, TArray<const TCHAR*>{ TEXT("lod_threshold"), TEXT("LODThreshold") }, LODThreshold);
    TryGetAnyIntField(Args, TArray<const TCHAR*>{ TEXT("ik_lod_threshold"), TEXT("LODThresholdForIK") }, IKLODThreshold);

    FString TargetPose, SourcePose;
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("target_pose"), TEXT("target_retarget_pose") }, TargetPose);
    TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("source_pose"), TEXT("source_retarget_pose") }, SourcePose);
    bool bForceIKOff = Node->CustomRetargetProfile.bForceAllIKOff;
    bool bHaveForceIKOff = TryGetAnyBoolField(Args, TArray<const TCHAR*>{ TEXT("force_ik_off"), TEXT("force_all_ik_off") }, bForceIKOff);

    if (bDryRun)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), AnimBP->GetPathName());
        R->SetStringField(TEXT("node_id"), GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
        R->SetStringField(TEXT("ik_retargeter"), RetargeterPath);
        R->SetStringField(TEXT("source_mode"), SourceModeString);
        R->SetNumberField(TEXT("lod_threshold"), LODThreshold);
        R->SetNumberField(TEXT("ik_lod_threshold"), IKLODThreshold);
        R->SetBoolField(TEXT("suppress_warnings"), bSuppressWarnings);
        R->SetBoolField(TEXT("dry_run"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageSetRetargetPoseFromMeshNode", "Sage: Set Retarget Pose From Mesh Node"));
    AnimBP->Modify();
    GraphNode->Modify();
    bool bModified = false;

    if (!RetargeterPath.IsEmpty())
    {
        UIKRetargeter* Retargeter = Cast<UIKRetargeter>(ResolveAsset(RetargeterPath));
        if (!Retargeter)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a UIKRetargeter: %s"), *RetargeterPath));
        }
        Node->IKRetargeterAsset = Retargeter;
        bModified = true;
    }
    if (!SourceModeString.IsEmpty())
    {
        ERetargetSourceMode Mode = Node->RetargetFrom;
        if (!ParseRetargetSourceMode(SourceModeString, Mode))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("invalid source_mode: %s"), *SourceModeString));
        }
        Node->RetargetFrom = Mode;
        bModified = true;
    }
    if (TryGetAnyIntField(Args, TArray<const TCHAR*>{ TEXT("lod_threshold"), TEXT("LODThreshold") }, LODThreshold))
    {
        Node->LODThreshold = LODThreshold;
        bModified = true;
    }
    if (TryGetAnyIntField(Args, TArray<const TCHAR*>{ TEXT("ik_lod_threshold"), TEXT("LODThresholdForIK") }, IKLODThreshold))
    {
        Node->LODThresholdForIK = IKLODThreshold;
        bModified = true;
    }
    if (bHaveSuppressWarnings)
    {
        Node->bSuppressWarnings = bSuppressWarnings;
        bModified = true;
    }
    if (!TargetPose.IsEmpty())
    {
        Node->CustomRetargetProfile.bApplyTargetRetargetPose = true;
        Node->CustomRetargetProfile.TargetRetargetPoseName = FName(*TargetPose);
        bModified = true;
    }
    if (!SourcePose.IsEmpty())
    {
        Node->CustomRetargetProfile.bApplySourceRetargetPose = true;
        Node->CustomRetargetProfile.SourceRetargetPoseName = FName(*SourcePose);
        bModified = true;
    }
    if (bHaveForceIKOff)
    {
        Node->CustomRetargetProfile.bForceAllIKOff = bForceIKOff;
        bModified = true;
    }
    if (bExposeSourceMeshPin)
    {
        FResolvedAnimNodePinBinding PinInfo;
        if (ResolveAnimNodePinBinding(GraphNode, TEXT("SourceMeshComponent"), PinInfo))
        {
            bModified |= SetResolvedAnimNodePinVisible(GraphNode, PinInfo, /*bReconstruct=*/true);
        }
    }

    if (bModified)
    {
        FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
        GraphNode->PostEditChangeProperty(Event);
        GraphNode->ReconstructNode();
        AnimBP->MarkPackageDirty();
    }
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(AnimBP);
    }

    FSageToolDispatch::FOutcome Readback = ReadRetargetPoseFromMeshNodeImpl(Args);
    TSharedPtr<FJsonObject> R = Readback.bSuccess && Readback.Result.IsValid()
        ? Readback.Result
        : MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("modified"), bModified);
    R->SetBoolField(TEXT("compiled"), bCompile);
    TrySaveLoadedAssetIfRequested(AnimBP, bSave && bModified, R.ToSharedRef());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_retarget_pose_from_mesh_node
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddRetargetPoseFromMeshNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    auto AddArgs = MakeShared<FJsonObject>();
    FString Path, GraphName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    AddArgs->SetStringField(TEXT("path"), Path);
    if (Args->TryGetStringField(TEXT("graph_name"), GraphName))
    {
        AddArgs->SetStringField(TEXT("graph_name"), GraphName);
    }
    double X = 0.0;
    double Y = 0.0;
    if (Args->TryGetNumberField(TEXT("x"), X)) AddArgs->SetNumberField(TEXT("x"), X);
    if (Args->TryGetNumberField(TEXT("y"), Y)) AddArgs->SetNumberField(TEXT("y"), Y);
    AddArgs->SetStringField(TEXT("node_class"), TEXT("/Script/IKRigDeveloper.AnimGraphNode_RetargetPoseFromMesh"));

    FSageToolDispatch::FOutcome Added = AddAnimGraphNodeImpl(AddArgs);
    if (!Added.bSuccess || !Added.Result.IsValid())
    {
        return Added;
    }
    FString NodeId;
    Added.Result->TryGetStringField(TEXT("node_id"), NodeId);
    if (NodeId.IsEmpty())
    {
        return Added;
    }

    auto SetArgs = MakeShared<FJsonObject>();
    SetArgs->SetStringField(TEXT("path"), Path);
    SetArgs->SetStringField(TEXT("node_id"), NodeId);
    if (!GraphName.IsEmpty()) SetArgs->SetStringField(TEXT("graph_name"), GraphName);
    const TCHAR* StringFields[] = {
        TEXT("ik_retargeter"), TEXT("retargeter"), TEXT("ik_retargeter_asset"),
        TEXT("source_mode"), TEXT("retarget_from"),
        TEXT("target_pose"), TEXT("source_pose")
    };
    for (const TCHAR* Field : StringFields)
    {
        FString Value;
        if (Args->TryGetStringField(Field, Value))
        {
            SetArgs->SetStringField(Field, Value);
        }
    }
    const TCHAR* NumberFields[] = {
        TEXT("lod_threshold"), TEXT("ik_lod_threshold")
    };
    for (const TCHAR* Field : NumberFields)
    {
        double Value = 0.0;
        if (Args->TryGetNumberField(Field, Value))
        {
            SetArgs->SetNumberField(Field, Value);
        }
    }
    const TCHAR* BoolFields[] = {
        TEXT("suppress_warnings"), TEXT("expose_source_mesh_pin"),
        TEXT("force_ik_off"), TEXT("compile"), TEXT("save")
    };
    for (const TCHAR* Field : BoolFields)
    {
        bool Value = false;
        if (Args->TryGetBoolField(Field, Value))
        {
            SetArgs->SetBoolField(Field, Value);
        }
    }
    FSageToolDispatch::FOutcome Configured = SetRetargetPoseFromMeshNodeImpl(SetArgs);
    if (Configured.bSuccess && Configured.Result.IsValid())
    {
        Configured.Result->SetObjectField(TEXT("created_node"), Added.Result.ToSharedRef());
    }
    return Configured;
}

// ---------------------------------------------------------------------------
// animation.read_retarget_profile
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome ReadRetargetProfileImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString RetargeterPath;
    if (Args.IsValid() && TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("retargeter"), TEXT("ik_retargeter") }, RetargeterPath))
    {
        UIKRetargeter* Retargeter = Cast<UIKRetargeter>(ResolveAsset(RetargeterPath));
        if (!Retargeter)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a UIKRetargeter: %s"), *RetargeterPath));
        }
        FRetargetProfile Profile;
        Profile.FillProfileWithAssetSettings(Retargeter);
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("retargeter"), Retargeter->GetPathName());
        R->SetObjectField(TEXT("profile"), RetargetProfileToJson(Profile));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    UAnimGraphNode_Base* GraphNode = nullptr;
    FAnimNode_RetargetPoseFromMesh* Node = nullptr;
    FString Error;
    if (!ResolveRetargetPoseFromMeshNode(Args, AnimBP, Graph, GraphNode, Node, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), AnimBP->GetPathName());
    R->SetStringField(TEXT("node_id"), GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    R->SetObjectField(TEXT("profile"), RetargetProfileToJson(Node->CustomRetargetProfile));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_retarget_profile
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SetRetargetProfileImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto MutableArgs = MakeShared<FJsonObject>();
    if (Args.IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Args->Values)
        {
            MutableArgs->SetField(Pair.Key, Pair.Value);
        }
    }
    return SetRetargetPoseFromMeshNodeImpl(MutableArgs);
}

// ---------------------------------------------------------------------------
// animation.copy_retarget_profile_from_asset
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome CopyRetargetProfileFromAssetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString RetargeterPath;
    if (!Args.IsValid() || !TryGetAnyStringField(Args, TArray<const TCHAR*>{ TEXT("retargeter"), TEXT("ik_retargeter") }, RetargeterPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'retargeter'"));
    }
    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(ResolveAsset(RetargeterPath));
    if (!Retargeter)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UIKRetargeter: %s"), *RetargeterPath));
    }
    FRetargetProfile Profile;
    Profile.FillProfileWithAssetSettings(Retargeter);

    FString Path, NodeId;
    if (Args->TryGetStringField(TEXT("path"), Path) && Args->TryGetStringField(TEXT("node_id"), NodeId))
    {
        UAnimBlueprint* AnimBP = nullptr;
        UEdGraph* Graph = nullptr;
        UAnimGraphNode_Base* GraphNode = nullptr;
        FAnimNode_RetargetPoseFromMesh* Node = nullptr;
        FString Error;
        if (!ResolveRetargetPoseFromMeshNode(Args, AnimBP, Graph, GraphNode, Node, Error))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
        }
        FScopedTransaction Tx(LOCTEXT("SageCopyRetargetProfileFromAsset", "Sage: Copy Retarget Profile From Asset"));
        AnimBP->Modify();
        GraphNode->Modify();
        Node->CustomRetargetProfile = Profile;
        GraphNode->ReconstructNode();
        AnimBP->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("retargeter"), Retargeter->GetPathName());
    R->SetObjectField(TEXT("profile"), RetargetProfileToJson(Profile));
    R->SetBoolField(TEXT("applied_to_node"), !Path.IsEmpty() && !NodeId.IsEmpty());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_owner_locomotion_update
// ---------------------------------------------------------------------------

static const TCHAR* OwnerLocomotionUpdateMarker = TEXT("SAGE_ANIM_OWNER_LOCOMOTION_UPDATE");

struct FOwnerLocomotionValue
{
    UEdGraphPin* Pin = nullptr;
    FString Source;
    FString SourceNodeId;
};

struct FOwnerLocomotionBuildContext
{
    UAnimBlueprint* AnimBP = nullptr;
    UEdGraph* Graph = nullptr;
    const UEdGraphSchema* Schema = nullptr;
    const UEdGraphSchema_K2* K2Schema = nullptr;
    int32 BaseX = 0;
    int32 BaseY = 0;
    int32 NodeIndex = 0;
    FString Error;
    TArray<TSharedPtr<FJsonValue>> AuthoredNodes;
};

bool IsOwnerLocomotionNode(const UEdGraphNode* Node)
{
    return Node && Node->NodeComment.Contains(OwnerLocomotionUpdateMarker);
}

void PositionOwnerLocomotionNode(FOwnerLocomotionBuildContext& Ctx, UEdGraphNode* Node)
{
    if (!Node) return;
    const int32 Column = Ctx.NodeIndex % 5;
    const int32 Row = Ctx.NodeIndex / 5;
    Node->NodePosX = Ctx.BaseX + (Column * 290);
    Node->NodePosY = Ctx.BaseY + (Row * 170);
    ++Ctx.NodeIndex;
}

void MarkOwnerLocomotionNode(UEdGraphNode* Node, const FString& Kind)
{
    if (!Node) return;
    Node->NodeComment = FString::Printf(TEXT("%s:%s"), OwnerLocomotionUpdateMarker, *Kind);
    Node->bCommentBubblePinned = false;
    Node->bCommentBubbleVisible = false;
    Node->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction=*/false);
}

void AddOwnerLocomotionAuthoredNode(FOwnerLocomotionBuildContext& Ctx, const UEdGraphNode* Node, const FString& Kind)
{
    if (!Node) return;
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::Digits));
    Obj->SetStringField(TEXT("kind"), Kind);
    Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
    Obj->SetNumberField(TEXT("x"), Node->NodePosX);
    Obj->SetNumberField(TEXT("y"), Node->NodePosY);
    Ctx.AuthoredNodes.Add(MakeShared<FJsonValueObject>(Obj));
}

UEdGraphPin* FindExecInputPin(UEdGraphNode* Node)
{
    return Node ? Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
}

UEdGraphPin* FindThenOutputPin(UEdGraphNode* Node)
{
    return Node ? Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
}

UEdGraphPin* FindOutputDataPin(UEdGraphNode* Node, FName PreferredName = UEdGraphSchema_K2::PN_ReturnValue)
{
    if (!Node) return nullptr;
    if (!PreferredName.IsNone())
    {
        if (UEdGraphPin* Pin = Node->FindPin(PreferredName, EGPD_Output))
        {
            if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                return Pin;
            }
        }
    }
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            return Pin;
        }
    }
    return nullptr;
}

bool TryConnectOwnerLocomotionPins(
    FOwnerLocomotionBuildContext& Ctx,
    UEdGraphPin* From,
    UEdGraphPin* To,
    const FString& Context,
    bool bBreakInput = true)
{
    if (!From || !To)
    {
        Ctx.Error = FString::Printf(TEXT("%s pin lookup failed: from=%s to=%s"),
            *Context,
            From ? *From->PinName.ToString() : TEXT("<null>"),
            To ? *To->PinName.ToString() : TEXT("<null>"));
        return false;
    }
    if (From->Direction != EGPD_Output || To->Direction != EGPD_Input)
    {
        Ctx.Error = FString::Printf(TEXT("%s connection direction mismatch: from=%s dir=%s to=%s dir=%s"),
            *Context,
            *From->PinName.ToString(),
            From->Direction == EGPD_Input ? TEXT("input") : TEXT("output"),
            *To->PinName.ToString(),
            To->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        return false;
    }
    if (bBreakInput)
    {
        To->Modify();
        To->BreakAllPinLinks();
    }
    if (!Ctx.Schema || !Ctx.Schema->TryCreateConnection(From, To))
    {
        Ctx.Error = FString::Printf(TEXT("%s connection failed: from node=%s to node=%s"),
            *Context,
            *DescribePinsForError(From->GetOwningNode()),
            *DescribePinsForError(To->GetOwningNode()));
        return false;
    }
    return true;
}

bool TrySetOwnerLocomotionPinDefault(
    FOwnerLocomotionBuildContext& Ctx,
    UEdGraphPin* Pin,
    const FString& Value,
    const FString& Context)
{
    if (!Pin)
    {
        Ctx.Error = FString::Printf(TEXT("%s default target pin missing"), *Context);
        return false;
    }
    if (!Ctx.K2Schema)
    {
        Ctx.Error = TEXT("EventGraph schema is not UEdGraphSchema_K2; cannot set default pin value");
        return false;
    }
    Pin->Modify();
    Pin->BreakAllPinLinks();
    Ctx.K2Schema->TrySetDefaultValue(*Pin, Value, false);
    return true;
}

UClass* ResolveOwnerLocomotionClassOrDefault(
    const TSharedPtr<FJsonObject>& Args,
    const TCHAR* FieldName,
    UClass* DefaultClass,
    UClass* RequiredBase,
    FString& OutPath,
    FString& OutError)
{
    if (Args.IsValid())
    {
        Args->TryGetStringField(FieldName, OutPath);
    }
    UClass* Resolved = OutPath.IsEmpty() ? DefaultClass : ResolveClassAsset(OutPath);
    if (!Resolved)
    {
        OutError = FString::Printf(TEXT("class not found for '%s': %s"), FieldName, *OutPath);
        return nullptr;
    }
    if (RequiredBase && !Resolved->IsChildOf(RequiredBase))
    {
        OutError = FString::Printf(TEXT("'%s' must be a %s subclass, got %s"),
            FieldName, *RequiredBase->GetName(), *Resolved->GetPathName());
        return nullptr;
    }
    OutPath = Resolved->GetPathName();
    return Resolved;
}

FProperty* ResolveBlueprintVisibleProperty(UClass* ScopeClass, FName PropertyName, FString& OutError)
{
    if (!ScopeClass || PropertyName.IsNone())
    {
        OutError = TEXT("property scope or name missing");
        return nullptr;
    }
    FProperty* Property = FindFProperty<FProperty>(ScopeClass, PropertyName);
    if (!Property)
    {
        OutError = FString::Printf(TEXT("property '%s' not found on %s"),
            *PropertyName.ToString(), *ScopeClass->GetPathName());
        return nullptr;
    }
    if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
    {
        OutError = FString::Printf(TEXT("property '%s' on %s is not BlueprintVisible"),
            *PropertyName.ToString(), *ScopeClass->GetPathName());
        return nullptr;
    }
    return Property;
}

bool HasAnimBlueprintVariable(UAnimBlueprint* AnimBP, FName VarName, FString& OutFoundIn)
{
    if (!AnimBP || VarName.IsNone())
    {
        return false;
    }
    if (FBlueprintEditorUtils::FindNewVariableIndex(AnimBP, VarName) != INDEX_NONE)
    {
        OutFoundIn = TEXT("NewVariables");
        return true;
    }
    UClass* Classes[] = {
        AnimBP->SkeletonGeneratedClass,
        AnimBP->GeneratedClass,
        AnimBP->ParentClass
    };
    for (UClass* Class : Classes)
    {
        if (Class && FindFProperty<FProperty>(Class, VarName))
        {
            OutFoundIn = Class->GetPathName();
            return true;
        }
    }
    return false;
}

UK2Node_CallFunction* CreateOwnerLocomotionCallNode(
    FOwnerLocomotionBuildContext& Ctx,
    UClass* FunctionOwner,
    const FName& FunctionName,
    const FString& Kind)
{
    if (!Ctx.Graph)
    {
        Ctx.Error = TEXT("EventGraph missing");
        return nullptr;
    }
    UFunction* Function = FunctionOwner ? FunctionOwner->FindFunctionByName(FunctionName) : nullptr;
    if (!Function)
    {
        Ctx.Error = FString::Printf(TEXT("function not found: %s.%s"),
            FunctionOwner ? *FunctionOwner->GetName() : TEXT("<null>"),
            *FunctionName.ToString());
        return nullptr;
    }

    UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Ctx.Graph);
    Node->CreateNewGuid();
    PositionOwnerLocomotionNode(Ctx, Node);
    Node->SetFromFunction(Function);
    Ctx.Graph->AddNode(Node, /*bSelectNewNode=*/false, /*bFromUI=*/true);
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    MarkOwnerLocomotionNode(Node, Kind);
    AddOwnerLocomotionAuthoredNode(Ctx, Node, Kind);
    return Node;
}

UK2Node_DynamicCast* CreateOwnerLocomotionCastNode(
    FOwnerLocomotionBuildContext& Ctx,
    UClass* TargetClass,
    const FString& Kind)
{
    if (!Ctx.Graph || !TargetClass)
    {
        Ctx.Error = TEXT("cast node target missing");
        return nullptr;
    }
    UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Ctx.Graph);
    Node->CreateNewGuid();
    PositionOwnerLocomotionNode(Ctx, Node);
    Node->TargetType = TargetClass;
    Ctx.Graph->AddNode(Node, /*bSelectNewNode=*/false, /*bFromUI=*/true);
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    if (Node->IsNodePure())
    {
        Node->SetPurity(false);
    }
    MarkOwnerLocomotionNode(Node, Kind);
    AddOwnerLocomotionAuthoredNode(Ctx, Node, Kind);
    return Node;
}

UK2Node_VariableGet* CreateOwnerLocomotionVariableGetNode(
    FOwnerLocomotionBuildContext& Ctx,
    UClass* ScopeClass,
    FName PropertyName,
    UEdGraphPin* SourceObjectPin,
    const FString& Kind,
    UEdGraphPin*& OutValuePin)
{
    OutValuePin = nullptr;
    FString PropertyError;
    FProperty* Property = ResolveBlueprintVisibleProperty(ScopeClass, PropertyName, PropertyError);
    if (!Property)
    {
        Ctx.Error = PropertyError;
        return nullptr;
    }

    UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Ctx.Graph);
    Node->CreateNewGuid();
    PositionOwnerLocomotionNode(Ctx, Node);
    UClass* OwnerClass = Property->GetOwner<UClass>();
    Node->SetFromProperty(Property, /*bSelfContext=*/false, OwnerClass ? OwnerClass : ScopeClass);
    Ctx.Graph->AddNode(Node, /*bSelectNewNode=*/false, /*bFromUI=*/true);
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    MarkOwnerLocomotionNode(Node, Kind);
    AddOwnerLocomotionAuthoredNode(Ctx, Node, Kind);

    if (SourceObjectPin)
    {
        UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
        if (!SelfPin)
        {
            Ctx.Error = FString::Printf(TEXT("self pin missing for property get '%s': node=%s"),
                *PropertyName.ToString(), *DescribePinsForError(Node));
            return nullptr;
        }
        if (!TryConnectOwnerLocomotionPins(Ctx, SourceObjectPin, SelfPin,
            FString::Printf(TEXT("connect self for get %s"), *PropertyName.ToString())))
        {
            return nullptr;
        }
    }

    OutValuePin = FindVariableGetOutputPin(Node, PropertyName);
    if (!OutValuePin)
    {
        Ctx.Error = FString::Printf(TEXT("output pin missing for property get '%s': node=%s"),
            *PropertyName.ToString(), *DescribePinsForError(Node));
        return nullptr;
    }
    return Node;
}

UEdGraphPin* FindOwnerLocomotionVariableSetValuePin(UK2Node_VariableSet* Node, FName VariableName)
{
    if (!Node) return nullptr;
    // VariableSet's assignment pin is an input; UK2Node_Variable::GetValuePin asserts on input pins.
    if (UEdGraphPin* Pin = Node->FindPin(VariableName, EGPD_Input))
    {
        return Pin;
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
            && Pin->PinName != UEdGraphSchema_K2::PN_Self)
        {
            return Pin;
        }
    }
    return nullptr;
}

UK2Node_VariableSet* CreateOwnerLocomotionVariableSetNode(
    FOwnerLocomotionBuildContext& Ctx,
    FName VariableName,
    const FString& Kind,
    UEdGraphPin*& OutValuePin)
{
    OutValuePin = nullptr;
    UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Ctx.Graph);
    Node->CreateNewGuid();
    PositionOwnerLocomotionNode(Ctx, Node);
    Node->VariableReference.SetSelfMember(VariableName);
    Ctx.Graph->AddNode(Node, /*bSelectNewNode=*/false, /*bFromUI=*/true);
    Node->AllocateDefaultPins();
    Node->PostPlacedNewNode();
    MarkOwnerLocomotionNode(Node, Kind);
    AddOwnerLocomotionAuthoredNode(Ctx, Node, Kind);

    OutValuePin = FindOwnerLocomotionVariableSetValuePin(Node, VariableName);
    if (!OutValuePin)
    {
        Ctx.Error = FString::Printf(TEXT("value pin missing for variable set '%s': node=%s"),
            *VariableName.ToString(), *DescribePinsForError(Node));
        return nullptr;
    }
    return Node;
}

bool AppendOptionalOwnerLocomotionExecCall(
    FOwnerLocomotionBuildContext& Ctx,
    UEdGraphPin*& ExecTail,
    UK2Node_CallFunction* CallNode,
    const FString& Context)
{
    if (!CallNode)
    {
        return false;
    }
    UEdGraphPin* ExecIn = FindExecInputPin(CallNode);
    UEdGraphPin* ThenOut = FindThenOutputPin(CallNode);
    if (!ExecIn && !ThenOut)
    {
        return true;
    }
    if (!ExecTail || !ExecIn || !ThenOut)
    {
        Ctx.Error = FString::Printf(TEXT("%s has incomplete exec pins: node=%s"),
            *Context, *DescribePinsForError(CallNode));
        return false;
    }
    if (!TryConnectOwnerLocomotionPins(Ctx, ExecTail, ExecIn, Context))
    {
        return false;
    }
    ExecTail = ThenOut;
    return true;
}

FOwnerLocomotionValue MakeOwnerLocomotionValue(UEdGraphPin* Pin, const FString& Source)
{
    FOwnerLocomotionValue Value;
    Value.Pin = Pin;
    Value.Source = Source;
    if (Pin && Pin->GetOwningNode())
    {
        Value.SourceNodeId = Pin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::Digits);
    }
    return Value;
}

FOwnerLocomotionValue CreateOwnerLocomotionPropertyValue(
    FOwnerLocomotionBuildContext& Ctx,
    UClass* ScopeClass,
    FName PropertyName,
    UEdGraphPin* SourceObjectPin,
    const FString& Kind,
    const FString& SourceText)
{
    UEdGraphPin* OutputPin = nullptr;
    UK2Node_VariableGet* Node = CreateOwnerLocomotionVariableGetNode(
        Ctx, ScopeClass, PropertyName, SourceObjectPin, Kind, OutputPin);
    if (!Node || !OutputPin)
    {
        return FOwnerLocomotionValue();
    }
    return MakeOwnerLocomotionValue(OutputPin, SourceText);
}

FOwnerLocomotionValue CreateOwnerLocomotionByteEquals(
    FOwnerLocomotionBuildContext& Ctx,
    UEdGraphPin* SourcePin,
    int32 Literal,
    const FString& Kind,
    const FString& SourceText)
{
    if (!SourcePin)
    {
        Ctx.Error = FString::Printf(TEXT("%s source pin missing"), *Kind);
        return FOwnerLocomotionValue();
    }
    UK2Node_CallFunction* EqNode = CreateOwnerLocomotionCallNode(
        Ctx, UKismetMathLibrary::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_ByteByte),
        Kind);
    if (!EqNode) return FOwnerLocomotionValue();
    UEdGraphPin* APin = EqNode->FindPin(TEXT("A"), EGPD_Input);
    UEdGraphPin* BPin = EqNode->FindPin(TEXT("B"), EGPD_Input);
    if (!TryConnectOwnerLocomotionPins(Ctx, SourcePin, APin, Kind)
        || !TrySetOwnerLocomotionPinDefault(Ctx, BPin, FString::FromInt(Literal), Kind))
    {
        return FOwnerLocomotionValue();
    }
    return MakeOwnerLocomotionValue(FindBoolReturnPin(EqNode), SourceText);
}

FOwnerLocomotionValue CreateOwnerLocomotionBoolAnd(
    FOwnerLocomotionBuildContext& Ctx,
    const FOwnerLocomotionValue& A,
    const FOwnerLocomotionValue& B,
    const FString& Kind,
    const FString& SourceText)
{
    if (!A.Pin || !B.Pin)
    {
        Ctx.Error = FString::Printf(TEXT("%s source pin missing: A=%s B=%s"),
            *Kind,
            A.Pin ? *A.Pin->PinName.ToString() : TEXT("<null>"),
            B.Pin ? *B.Pin->PinName.ToString() : TEXT("<null>"));
        return FOwnerLocomotionValue();
    }
    UK2Node_CallFunction* AndNode = CreateOwnerLocomotionCallNode(
        Ctx, UKismetMathLibrary::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanAND),
        Kind);
    if (!AndNode) return FOwnerLocomotionValue();
    UEdGraphPin* APin = AndNode->FindPin(TEXT("A"), EGPD_Input);
    UEdGraphPin* BPin = AndNode->FindPin(TEXT("B"), EGPD_Input);
    if (!TryConnectOwnerLocomotionPins(Ctx, A.Pin, APin, Kind)
        || !TryConnectOwnerLocomotionPins(Ctx, B.Pin, BPin, Kind))
    {
        return FOwnerLocomotionValue();
    }
    return MakeOwnerLocomotionValue(FindBoolReturnPin(AndNode), SourceText);
}

FOwnerLocomotionValue CreateOwnerLocomotionUnaryVectorFunction(
    FOwnerLocomotionBuildContext& Ctx,
    FName FunctionName,
    UEdGraphPin* VectorPin,
    const FString& Kind,
    const FString& SourceText)
{
    if (!VectorPin)
    {
        Ctx.Error = FString::Printf(TEXT("%s vector source pin missing"), *Kind);
        return FOwnerLocomotionValue();
    }
    UK2Node_CallFunction* Node = CreateOwnerLocomotionCallNode(
        Ctx, UKismetMathLibrary::StaticClass(), FunctionName, Kind);
    if (!Node) return FOwnerLocomotionValue();
    UEdGraphPin* APin = Node->FindPin(TEXT("A"), EGPD_Input);
    if (!TryConnectOwnerLocomotionPins(Ctx, VectorPin, APin, Kind))
    {
        return FOwnerLocomotionValue();
    }
    return MakeOwnerLocomotionValue(FindOutputDataPin(Node), SourceText);
}

FOwnerLocomotionValue CreateOwnerLocomotionDot(
    FOwnerLocomotionBuildContext& Ctx,
    UEdGraphPin* A,
    UEdGraphPin* B,
    const FString& Kind,
    const FString& SourceText)
{
    if (!A || !B)
    {
        Ctx.Error = FString::Printf(TEXT("%s source pin missing: A=%s B=%s"),
            *Kind,
            A ? *A->PinName.ToString() : TEXT("<null>"),
            B ? *B->PinName.ToString() : TEXT("<null>"));
        return FOwnerLocomotionValue();
    }
    UK2Node_CallFunction* DotNode = CreateOwnerLocomotionCallNode(
        Ctx, UKismetMathLibrary::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Dot_VectorVector),
        Kind);
    if (!DotNode) return FOwnerLocomotionValue();
    UEdGraphPin* APin = DotNode->FindPin(TEXT("A"), EGPD_Input);
    UEdGraphPin* BPin = DotNode->FindPin(TEXT("B"), EGPD_Input);
    if (!TryConnectOwnerLocomotionPins(Ctx, A, APin, Kind)
        || !TryConnectOwnerLocomotionPins(Ctx, B, BPin, Kind))
    {
        return FOwnerLocomotionValue();
    }
    return MakeOwnerLocomotionValue(FindOutputDataPin(DotNode), SourceText);
}

FOwnerLocomotionValue CreateOwnerLocomotionBreakVectorZ(
    FOwnerLocomotionBuildContext& Ctx,
    UEdGraphPin* VectorPin,
    const FString& Kind,
    const FString& SourceText)
{
    if (!VectorPin)
    {
        Ctx.Error = FString::Printf(TEXT("%s vector source pin missing"), *Kind);
        return FOwnerLocomotionValue();
    }
    UK2Node_CallFunction* BreakNode = CreateOwnerLocomotionCallNode(
        Ctx, UKismetMathLibrary::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BreakVector),
        Kind);
    if (!BreakNode) return FOwnerLocomotionValue();
    UEdGraphPin* InVec = BreakNode->FindPin(TEXT("InVec"), EGPD_Input);
    UEdGraphPin* ZPin = BreakNode->FindPin(TEXT("Z"), EGPD_Output);
    if (!TryConnectOwnerLocomotionPins(Ctx, VectorPin, InVec, Kind))
    {
        return FOwnerLocomotionValue();
    }
    return MakeOwnerLocomotionValue(ZPin, SourceText);
}

UEdGraph* EnsureOwnerLocomotionEventGraph(UAnimBlueprint* AnimBP, bool& bCreated)
{
    bCreated = false;
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(AnimBP);
    if (EventGraph)
    {
        return EventGraph;
    }
    EventGraph = FBlueprintEditorUtils::CreateNewGraph(
        AnimBP,
        UEdGraphSchema_K2::GN_EventGraph,
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    if (!EventGraph)
    {
        return nullptr;
    }
    EventGraph->bAllowDeletion = false;
    FBlueprintEditorUtils::AddUbergraphPage(AnimBP, EventGraph);
    bCreated = true;
    return EventGraph;
}

UK2Node_Event* EnsureOwnerLocomotionUpdateEvent(
    UAnimBlueprint* AnimBP,
    UEdGraph*& EventGraph,
    int32 NodeX,
    int32 NodeY,
    bool& bCreated,
    FString& OutError)
{
    bCreated = false;
    const FName EventName(TEXT("BlueprintUpdateAnimation"));
    if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
        AnimBP, UAnimInstance::StaticClass(), EventName))
    {
        Existing->Modify();
        Existing->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction=*/false);
        EventGraph = Existing->GetGraph();
        return Existing;
    }

    if (!EventGraph)
    {
        bool bGraphCreated = false;
        EventGraph = EnsureOwnerLocomotionEventGraph(AnimBP, bGraphCreated);
    }
    if (!EventGraph)
    {
        OutError = TEXT("could not create EventGraph");
        return nullptr;
    }

    int32 EventY = NodeY;
    UK2Node_Event* EventNode = FKismetEditorUtilities::AddDefaultEventNode(
        AnimBP, EventGraph, EventName, UAnimInstance::StaticClass(), EventY);
    if (!EventNode)
    {
        UFunction* UpdateFunction = UAnimInstance::StaticClass()->FindFunctionByName(EventName);
        if (!UpdateFunction)
        {
            OutError = TEXT("UAnimInstance.BlueprintUpdateAnimation function not found");
            return nullptr;
        }
        EventNode = NewObject<UK2Node_Event>(EventGraph);
        EventNode->EventReference.SetFromField<UFunction>(UpdateFunction, false);
        EventNode->bOverrideFunction = true;
        EventNode->CreateNewGuid();
        EventGraph->AddNode(EventNode, /*bSelectNewNode=*/false, /*bFromUI=*/true);
        EventNode->AllocateDefaultPins();
        EventNode->PostPlacedNewNode();
    }
    EventNode->Modify();
    EventNode->NodePosX = NodeX;
    EventNode->NodePosY = NodeY;
    EventNode->bCommentBubblePinned = false;
    EventNode->bCommentBubbleVisible = false;
    EventNode->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction=*/false);
    bCreated = true;
    return EventNode;
}

int32 RemoveExistingOwnerLocomotionNodes(UAnimBlueprint* AnimBP, UEdGraph* Graph)
{
    if (!AnimBP || !Graph) return 0;
    int32 Removed = 0;
    TArray<UEdGraphNode*> Nodes = Graph->Nodes;
    for (UEdGraphNode* Node : Nodes)
    {
        if (!Node || Node->IsA<UK2Node_Event>() || !IsOwnerLocomotionNode(Node))
        {
            continue;
        }
        FBlueprintEditorUtils::RemoveNode(AnimBP, Node, /*bDontRecompile=*/true);
        ++Removed;
    }
    return Removed;
}

bool ParseOwnerLocomotionVariableMap(
    const TSharedPtr<FJsonObject>& Args,
    TMap<FString, FString>& OutVariables,
    FString& OutError)
{
    struct FDefaultVar
    {
        const TCHAR* Role;
        const TCHAR* Variable;
    };
    const FDefaultVar Defaults[] = {
        { TEXT("flight_active"), TEXT("bIsFlightActive") },
        { TEXT("sprint_active"), TEXT("bIsSprintActive") },
        { TEXT("current_velocity"), TEXT("CurrentVelocity") },
        { TEXT("planar_speed"), TEXT("PlanarSpeed") },
        { TEXT("vertical_speed"), TEXT("VerticalSpeed") },
        { TEXT("lean_x"), TEXT("LeanX") },
        { TEXT("lean_y"), TEXT("LeanY") },
        { TEXT("movement_input_forward"), TEXT("MovementInputForward") },
        { TEXT("movement_input_right"), TEXT("MovementInputRight") },
    };
    for (const FDefaultVar& Entry : Defaults)
    {
        OutVariables.Add(Entry.Role, Entry.Variable);
    }

    const TSharedPtr<FJsonObject>* VariablesObj = nullptr;
    if (Args.IsValid() && Args->TryGetObjectField(TEXT("variables"), VariablesObj) && VariablesObj && VariablesObj->IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*VariablesObj)->Values)
        {
            if (!OutVariables.Contains(Pair.Key))
            {
                continue;
            }
            if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
            {
                OutError = FString::Printf(TEXT("variables.%s must be a string variable name"), *Pair.Key);
                return false;
            }
            OutVariables[Pair.Key] = Pair.Value->AsString().TrimStartAndEnd();
        }
    }
    return true;
}

TSharedPtr<FJsonObject> OwnerLocomotionAssignmentJson(
    const FString& Role,
    const FString& VariableName,
    const FOwnerLocomotionValue& Value,
    const UK2Node_VariableSet* SetNode)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("role"), Role);
    Obj->SetStringField(TEXT("variable"), VariableName);
    Obj->SetStringField(TEXT("source"), Value.Source);
    Obj->SetStringField(TEXT("source_node_id"), Value.SourceNodeId);
    Obj->SetStringField(TEXT("set_node_id"),
        SetNode ? SetNode->NodeGuid.ToString(EGuidFormats::Digits) : FString());
    Obj->SetBoolField(TEXT("written"), SetNode != nullptr && Value.Pin != nullptr);
    return Obj;
}

FSageToolDispatch::FOutcome SetOwnerLocomotionUpdateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }

    FString OwnerClassPath, MovementClassPath, ClassError;
    UClass* OwnerClass = ResolveOwnerLocomotionClassOrDefault(
        Args, TEXT("owner_class"), ACharacter::StaticClass(), ACharacter::StaticClass(),
        OwnerClassPath, ClassError);
    if (!OwnerClass)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, ClassError);
    }
    UClass* MovementClass = ResolveOwnerLocomotionClassOrDefault(
        Args, TEXT("movement_class"), UCharacterMovementComponent::StaticClass(),
        UCharacterMovementComponent::StaticClass(), MovementClassPath, ClassError);
    if (!MovementClass)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, ClassError);
    }

    TMap<FString, FString> Variables;
    FString VariablesError;
    if (!ParseOwnerLocomotionVariableMap(Args, Variables, VariablesError))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, VariablesError);
    }

    TArray<TSharedPtr<FJsonValue>> MissingVariables;
    for (const TPair<FString, FString>& Pair : Variables)
    {
        if (Pair.Value.IsEmpty())
        {
            continue;
        }
        FString FoundIn;
        if (!HasAnimBlueprintVariable(AnimBP, FName(*Pair.Value), FoundIn))
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("role"), Pair.Key);
            Obj->SetStringField(TEXT("variable"), Pair.Value);
            MissingVariables.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }
    if (MissingVariables.Num() > 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("one or more target AnimBlueprint variables are missing (count=%d)"),
                MissingVariables.Num()));
    }

    FString FlightActiveProperty = TEXT("bWantsFlight");
    FString SprintActiveProperty = TEXT("bWantsFlightSprint");
    Args->TryGetStringField(TEXT("flight_active_property"), FlightActiveProperty);
    Args->TryGetStringField(TEXT("sprint_active_property"), SprintActiveProperty);

    double CustomModeNumber = 0.0;
    const bool bUseCustomFlightMode = Args->TryGetNumberField(TEXT("flight_custom_mode"), CustomModeNumber);
    const int32 FlightCustomMode = static_cast<int32>(FMath::RoundToDouble(CustomModeNumber));

    bool bReplaceExisting = true;
    bool bAllowOverwriteExec = false;
    bool bCompile = true;
    Args->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
    Args->TryGetBoolField(TEXT("allow_overwrite_exec"), bAllowOverwriteExec);
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    double X = 0.0;
    double Y = 0.0;
    Args->TryGetNumberField(TEXT("x"), X);
    Args->TryGetNumberField(TEXT("y"), Y);

    bool bEventGraphCreated = false;
    UEdGraph* EventGraph = EnsureOwnerLocomotionEventGraph(AnimBP, bEventGraphCreated);
    if (!EventGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("could not find or create EventGraph"));
    }
    const UEdGraphSchema* Schema = EventGraph->GetSchema();
    const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Schema);
    if (!K2Schema)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EventGraph schema is not UEdGraphSchema_K2"));
    }

    FScopedTransaction Tx(LOCTEXT("SetOwnerLocomotionUpdate", "Sage: Set Owner Locomotion Update"));
    AnimBP->Modify();
    EventGraph->Modify();

    int32 RemovedNodeCount = 0;
    if (bReplaceExisting)
    {
        RemovedNodeCount = RemoveExistingOwnerLocomotionNodes(AnimBP, EventGraph);
    }

    bool bEventCreated = false;
    FString EventError;
    UK2Node_Event* UpdateEvent = EnsureOwnerLocomotionUpdateEvent(
        AnimBP, EventGraph, static_cast<int32>(X), static_cast<int32>(Y), bEventCreated, EventError);
    if (!UpdateEvent)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, EventError);
    }
    EventGraph = UpdateEvent->GetGraph();
    EventGraph->Modify();

    UEdGraphPin* EventThen = FindThenOutputPin(UpdateEvent);
    if (!EventThen)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("BlueprintUpdateAnimation event has no Then pin: %s"),
                *DescribePinsForError(UpdateEvent)));
    }

    TArray<TSharedPtr<FJsonValue>> ExistingExecLinks;
    for (UEdGraphPin* LinkedPin : EventThen->LinkedTo)
    {
        UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
        if (LinkedNode && IsOwnerLocomotionNode(LinkedNode))
        {
            continue;
        }
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("pin"), LinkedPin ? LinkedPin->PinName.ToString() : FString());
        Obj->SetStringField(TEXT("node_id"), LinkedNode ? LinkedNode->NodeGuid.ToString(EGuidFormats::Digits) : FString());
        Obj->SetStringField(TEXT("node_class"), LinkedNode ? LinkedNode->GetClass()->GetName() : FString());
        ExistingExecLinks.Add(MakeShared<FJsonValueObject>(Obj));
    }
    if (ExistingExecLinks.Num() > 0 && !bAllowOverwriteExec)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("BlueprintUpdateAnimation Then pin is already linked to %d non-Sage node(s); pass allow_overwrite_exec:true to replace that exec chain"),
                ExistingExecLinks.Num()));
    }
    EventThen->Modify();
    EventThen->BreakAllPinLinks();

    FOwnerLocomotionBuildContext Ctx;
    Ctx.AnimBP = AnimBP;
    Ctx.Graph = EventGraph;
    Ctx.Schema = EventGraph->GetSchema();
    Ctx.K2Schema = Cast<UEdGraphSchema_K2>(Ctx.Schema);
    Ctx.BaseX = static_cast<int32>(X) + 260;
    Ctx.BaseY = static_cast<int32>(Y);

    UK2Node_CallFunction* TryOwnerNode = CreateOwnerLocomotionCallNode(
        Ctx, UAnimInstance::StaticClass(),
        GET_FUNCTION_NAME_CHECKED(UAnimInstance, TryGetPawnOwner),
        TEXT("try_get_pawn_owner"));
    if (!TryOwnerNode)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }
    UEdGraphPin* PawnPin = FindOutputDataPin(TryOwnerNode);
    if (!PawnPin)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("TryGetPawnOwner return pin missing: %s"), *DescribePinsForError(TryOwnerNode)));
    }

    UEdGraphPin* ExecTail = EventThen;
    if (!AppendOptionalOwnerLocomotionExecCall(Ctx, ExecTail, TryOwnerNode, TEXT("exec TryGetPawnOwner")))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }

    UK2Node_DynamicCast* OwnerCast = CreateOwnerLocomotionCastNode(Ctx, OwnerClass, TEXT("cast_owner"));
    if (!OwnerCast
        || !TryConnectOwnerLocomotionPins(Ctx, ExecTail, FindExecInputPin(OwnerCast), TEXT("exec cast owner"))
        || !TryConnectOwnerLocomotionPins(Ctx, PawnPin, OwnerCast->GetCastSourcePin(), TEXT("source cast owner")))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }
    UEdGraphPin* OwnerPin = OwnerCast->GetCastResultPin();
    ExecTail = OwnerCast->GetValidCastPin();

    UEdGraphPin* CharacterMovementPin = nullptr;
    UK2Node_VariableGet* CharacterMovementGet = CreateOwnerLocomotionVariableGetNode(
        Ctx, ACharacter::StaticClass(), FName(TEXT("CharacterMovement")), OwnerPin,
        TEXT("get_character_movement"), CharacterMovementPin);
    if (!CharacterMovementGet || !CharacterMovementPin)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }

    UEdGraphPin* MovementPin = CharacterMovementPin;
    if (MovementClass != UCharacterMovementComponent::StaticClass())
    {
        UK2Node_DynamicCast* MovementCast = CreateOwnerLocomotionCastNode(Ctx, MovementClass, TEXT("cast_movement"));
        if (!MovementCast
            || !TryConnectOwnerLocomotionPins(Ctx, ExecTail, FindExecInputPin(MovementCast), TEXT("exec cast movement"))
            || !TryConnectOwnerLocomotionPins(Ctx, CharacterMovementPin, MovementCast->GetCastSourcePin(), TEXT("source cast movement")))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
        }
        MovementPin = MovementCast->GetCastResultPin();
        ExecTail = MovementCast->GetValidCastPin();
    }

    TMap<FString, FOwnerLocomotionValue> Values;
    if (bUseCustomFlightMode)
    {
        FOwnerLocomotionValue MovementMode = CreateOwnerLocomotionPropertyValue(
            Ctx, UCharacterMovementComponent::StaticClass(), FName(TEXT("MovementMode")), MovementPin,
            TEXT("get_movement_mode"), TEXT("movement.MovementMode"));
        FOwnerLocomotionValue CustomMode = CreateOwnerLocomotionPropertyValue(
            Ctx, UCharacterMovementComponent::StaticClass(), FName(TEXT("CustomMovementMode")), MovementPin,
            TEXT("get_custom_movement_mode"), TEXT("movement.CustomMovementMode"));
        FOwnerLocomotionValue IsCustom = CreateOwnerLocomotionByteEquals(
            Ctx, MovementMode.Pin, static_cast<int32>(MOVE_Custom),
            TEXT("movement_mode_is_custom"), TEXT("movement.MovementMode == MOVE_Custom"));
        FOwnerLocomotionValue IsFlightMode = CreateOwnerLocomotionByteEquals(
            Ctx, CustomMode.Pin, FlightCustomMode,
            TEXT("custom_mode_is_flight"), FString::Printf(TEXT("movement.CustomMovementMode == %d"), FlightCustomMode));
        Values.Add(TEXT("flight_active"), CreateOwnerLocomotionBoolAnd(
            Ctx, IsCustom, IsFlightMode, TEXT("flight_active_from_mode"),
            FString::Printf(TEXT("movement.MovementMode == MOVE_Custom && movement.CustomMovementMode == %d"), FlightCustomMode)));
    }
    else
    {
        Values.Add(TEXT("flight_active"), CreateOwnerLocomotionPropertyValue(
            Ctx, MovementClass, FName(*FlightActiveProperty), MovementPin,
            TEXT("get_flight_active"), FString::Printf(TEXT("movement.%s"), *FlightActiveProperty)));
    }
    Values.Add(TEXT("sprint_active"), CreateOwnerLocomotionPropertyValue(
        Ctx, MovementClass, FName(*SprintActiveProperty), MovementPin,
        TEXT("get_sprint_active"), FString::Printf(TEXT("movement.%s"), *SprintActiveProperty)));
    Values.Add(TEXT("current_velocity"), CreateOwnerLocomotionPropertyValue(
        Ctx, UMovementComponent::StaticClass(), FName(TEXT("Velocity")), MovementPin,
        TEXT("get_velocity"), TEXT("movement.Velocity")));

    if (!Ctx.Error.IsEmpty())
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }

    const FOwnerLocomotionValue* CurrentVelocityPtr = Values.Find(TEXT("current_velocity"));
    if (!CurrentVelocityPtr || !CurrentVelocityPtr->Pin)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("CurrentVelocity source was not produced"));
    }
    const FOwnerLocomotionValue CurrentVelocity = *CurrentVelocityPtr;

    Values.Add(TEXT("planar_speed"), CreateOwnerLocomotionUnaryVectorFunction(
        Ctx, GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, VSizeXY), CurrentVelocity.Pin,
        TEXT("planar_speed"), TEXT("VSizeXY(movement.Velocity)")));
    Values.Add(TEXT("vertical_speed"), CreateOwnerLocomotionBreakVectorZ(
        Ctx, CurrentVelocity.Pin, TEXT("vertical_speed"), TEXT("movement.Velocity.Z")));

    UK2Node_CallFunction* ForwardNode = CreateOwnerLocomotionCallNode(
        Ctx, AActor::StaticClass(), GET_FUNCTION_NAME_CHECKED(AActor, GetActorForwardVector),
        TEXT("get_actor_forward"));
    UK2Node_CallFunction* RightNode = CreateOwnerLocomotionCallNode(
        Ctx, AActor::StaticClass(), GET_FUNCTION_NAME_CHECKED(AActor, GetActorRightVector),
        TEXT("get_actor_right"));
    UK2Node_CallFunction* InputNode = CreateOwnerLocomotionCallNode(
        Ctx, APawn::StaticClass(), GET_FUNCTION_NAME_CHECKED(APawn, GetLastMovementInputVector),
        TEXT("get_last_movement_input"));
    if (!ForwardNode || !RightNode || !InputNode)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }
    if (!TryConnectOwnerLocomotionPins(Ctx, OwnerPin, ForwardNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input), TEXT("self GetActorForwardVector"))
        || !TryConnectOwnerLocomotionPins(Ctx, OwnerPin, RightNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input), TEXT("self GetActorRightVector"))
        || !TryConnectOwnerLocomotionPins(Ctx, OwnerPin, InputNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input), TEXT("self GetLastMovementInputVector")))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }
    if (!AppendOptionalOwnerLocomotionExecCall(Ctx, ExecTail, ForwardNode, TEXT("exec GetActorForwardVector"))
        || !AppendOptionalOwnerLocomotionExecCall(Ctx, ExecTail, RightNode, TEXT("exec GetActorRightVector"))
        || !AppendOptionalOwnerLocomotionExecCall(Ctx, ExecTail, InputNode, TEXT("exec GetLastMovementInputVector")))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }

    UEdGraphPin* ForwardPin = FindOutputDataPin(ForwardNode);
    UEdGraphPin* RightPin = FindOutputDataPin(RightNode);
    UEdGraphPin* InputVectorPin = FindOutputDataPin(InputNode);
    Values.Add(TEXT("lean_x"), CreateOwnerLocomotionDot(
        Ctx, CurrentVelocity.Pin, RightPin, TEXT("lean_x"),
        TEXT("Dot(movement.Velocity, owner.GetActorRightVector())")));
    Values.Add(TEXT("lean_y"), CreateOwnerLocomotionDot(
        Ctx, CurrentVelocity.Pin, ForwardPin, TEXT("lean_y"),
        TEXT("Dot(movement.Velocity, owner.GetActorForwardVector())")));
    Values.Add(TEXT("movement_input_forward"), CreateOwnerLocomotionDot(
        Ctx, InputVectorPin, ForwardPin, TEXT("movement_input_forward"),
        TEXT("Dot(owner.GetLastMovementInputVector(), owner.GetActorForwardVector())")));
    Values.Add(TEXT("movement_input_right"), CreateOwnerLocomotionDot(
        Ctx, InputVectorPin, RightPin, TEXT("movement_input_right"),
        TEXT("Dot(owner.GetLastMovementInputVector(), owner.GetActorRightVector())")));

    if (!Ctx.Error.IsEmpty())
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
    }

    const FString RoleOrder[] = {
        TEXT("flight_active"),
        TEXT("sprint_active"),
        TEXT("current_velocity"),
        TEXT("planar_speed"),
        TEXT("vertical_speed"),
        TEXT("lean_x"),
        TEXT("lean_y"),
        TEXT("movement_input_forward"),
        TEXT("movement_input_right"),
    };

    TArray<TSharedPtr<FJsonValue>> Assignments;
    for (const FString& Role : RoleOrder)
    {
        const FString* VariableName = Variables.Find(Role);
        const FOwnerLocomotionValue* Value = Values.Find(Role);
        if (!VariableName || VariableName->IsEmpty())
        {
            continue;
        }
        if (!Value || !Value->Pin)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                FString::Printf(TEXT("source value missing for role '%s'"), *Role));
        }

        UEdGraphPin* SetValuePin = nullptr;
        UK2Node_VariableSet* SetNode = CreateOwnerLocomotionVariableSetNode(
            Ctx, FName(**VariableName),
            FString::Printf(TEXT("set_%s"), *Role),
            SetValuePin);
        if (!SetNode
            || !TryConnectOwnerLocomotionPins(Ctx, ExecTail, FindExecInputPin(SetNode),
                FString::Printf(TEXT("exec set %s"), **VariableName))
            || !TryConnectOwnerLocomotionPins(Ctx, Value->Pin, SetValuePin,
                FString::Printf(TEXT("value set %s"), **VariableName)))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603, Ctx.Error);
        }
        ExecTail = FindThenOutputPin(SetNode);
        Assignments.Add(MakeShared<FJsonValueObject>(
            OwnerLocomotionAssignmentJson(Role, *VariableName, *Value, SetNode)));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    AnimBP->MarkPackageDirty();
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(AnimBP);
    }

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), AnimBP->GetPathName());
    R->SetStringField(TEXT("event_graph"), EventGraph->GetName());
    R->SetStringField(TEXT("event_node_id"), UpdateEvent->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("owner_class"), OwnerClassPath);
    R->SetStringField(TEXT("movement_class"), MovementClassPath);
    R->SetBoolField(TEXT("event_graph_created"), bEventGraphCreated);
    R->SetBoolField(TEXT("event_created"), bEventCreated);
    R->SetNumberField(TEXT("removed_existing_nodes"), RemovedNodeCount);
    R->SetBoolField(TEXT("compiled"), bCompile);
    R->SetBoolField(TEXT("flight_custom_mode_enabled"), bUseCustomFlightMode);
    if (bUseCustomFlightMode)
    {
        R->SetNumberField(TEXT("flight_custom_mode"), FlightCustomMode);
    }
    R->SetArrayField(TEXT("existing_exec_links_replaced"), ExistingExecLinks);
    R->SetArrayField(TEXT("assignments"), Assignments);
    R->SetArrayField(TEXT("authored_nodes"), Ctx.AuthoredNodes);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.list_animgraph_nodes
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome ListAnimGraphNodesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }

    TArray<TSharedPtr<FJsonValue>> NodesArr;
    for (UEdGraphNode* N : TargetGraph->Nodes)
    {
        if (!N) continue;
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("node_id"), N->NodeGuid.ToString(EGuidFormats::Digits));
        Obj->SetStringField(TEXT("class"),   N->GetClass()->GetPathName());
        Obj->SetNumberField(TEXT("x"),       N->NodePosX);
        Obj->SetNumberField(TEXT("y"),       N->NodePosY);
        Obj->SetNumberField(TEXT("pin_count"), N->Pins.Num());
        // Classifier: lets callers filter without knowing the engine class
        // hierarchy. Order matters — most-specific first.
        const TCHAR* Kind = TEXT("other");
        if (N->IsA<UAnimGraphNode_StateMachineBase>())          Kind = TEXT("state_machine");
        else if (N->IsA<UAnimGraphNode_AssetPlayerBase>())      Kind = TEXT("asset_player");
        else if (N->IsA<UAnimGraphNode_BlendListBase>())        Kind = TEXT("blend_list");
        else if (N->IsA<UAnimGraphNode_SkeletalControlBase>())  Kind = TEXT("bone_control");
        else if (N->IsA<UAnimGraphNode_Base>())                 Kind = TEXT("anim_node");
        Obj->SetStringField(TEXT("kind"), Kind);
        if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(N))
        {
            const TArray<TSharedPtr<FJsonValue>> AssetRefs =
                CollectAnimNodeAssetReferences(AnimNode);
            Obj->SetArrayField(TEXT("animation_assets"), AssetRefs);
            Obj->SetNumberField(TEXT("animation_asset_count"), AssetRefs.Num());
        }
        if (UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(N))
        {
            Obj->SetObjectField(TEXT("state_machine"),
                StateMachineReferenceJson(AnimBP, SMNode, TargetGraph));
        }
        NodesArr.Add(MakeShared<FJsonValueObject>(Obj));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("graph"),  TargetGraph->GetFName().ToString());
    R->SetArrayField (TEXT("nodes"),  NodesArr);
    R->SetNumberField(TEXT("count"),  NodesArr.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.set_animgraph_root_pose
// ---------------------------------------------------------------------------
//
// Convenience: wire a node's first-output-pose to the AnimGraph Root's
// first-input-pose (the canonical "Output Pose" node in the root AnimGraph
// or any state's BoundGraph). Replaces whatever is currently feeding root.

FSageToolDispatch::FOutcome SetAnimGraphRootPoseImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, NodeId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'node_id'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }
    // Transition graphs hold a boolean rule expression, not a pose flow —
    // wiring a pose-source there silently corrupts the rule node's input.
    // Surface the error explicitly so callers know to pivot to the future
    // animation.set_transition_rule tool.
    if (Cast<UAnimationTransitionGraph>(TargetGraph))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("use animation.set_transition_rule for transition graphs"));
    }
    UEdGraphNode* SrcNode = FindGraphNodeByGuid(TargetGraph, NodeId);
    if (!SrcNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id not found: %s"), *NodeId));
    }
    UEdGraphNode* Root = FindAnimGraphOutput(TargetGraph);
    if (!Root)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("graph has no Output Pose (UAnimGraphNode_Root or UAnimGraphNode_StateResult)"));
    }

    UEdGraphPin* SrcOut = FindFirstOutputPosePin(SrcNode);
    UEdGraphPin* RootIn = FindFirstInputPosePin(Root);
    if (!SrcOut || !RootIn)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("pose pin lookup failed"));
    }

    FScopedTransaction Tx(LOCTEXT("SetAGRoot", "Sage: Wire AnimGraph Root"));
    AnimBP->Modify();
    TargetGraph->Modify();
    RootIn->BreakAllPinLinks();
    SrcOut->MakeLinkTo(RootIn);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"),  NodeId);
    R->SetStringField(TEXT("root_id"),  Root->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetBoolField  (TEXT("connected"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ===========================================================================
// Cluster B — AnimGraph convenience nodes (Phase 4-r6)
// Thin specialised wrappers over the Cluster A primitive AddAnimGraphNodeImpl
// + node-specific default property set. Returned shape always includes
// node_id so the caller can chain pose-pin connect/property set.
// ===========================================================================

// Generic spawn helper — replicates the canonical UE pipeline:
// NewObject(Outer=Graph, Class) → CreateNewGuid → AddNode → PostPlacedNewNode
// → AllocateDefaultPins. PostPlacedNewNode is critical for composite nodes
// (state machines, blend list by bool with sub-graphs).
template <typename TNode>
TNode* SpawnAnimGraphNode(UEdGraph* Graph, double X, double Y)
{
    if (!Graph) return nullptr;
    TNode* Node = NewObject<TNode>(Graph);
    Node->CreateNewGuid();
    Node->NodePosX = static_cast<int32>(X);
    Node->NodePosY = static_cast<int32>(Y);
    Graph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();
    return Node;
}

// Body shared by every Cluster B handler — argument parse + AnimBP + graph
// resolve. Returns null on validation failure (Outcome already populated).
struct FClusterBContext
{
    UAnimBlueprint* AnimBP   = nullptr;
    UEdGraph*       Graph    = nullptr;
    double          X        = 0.0;
    double          Y        = 0.0;
};

bool ResolveClusterBContext(const TSharedPtr<FJsonObject>& Args,
                            FClusterBContext& Out, FSageToolDispatch::FOutcome& OutErr)
{
    FString Path, GraphName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        OutErr = FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
        return false;
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);
    Args->TryGetNumberField(TEXT("x"), Out.X);
    Args->TryGetNumberField(TEXT("y"), Out.Y);

    Out.AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!Out.AnimBP)
    {
        OutErr = FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
        return false;
    }
    Out.Graph = ResolveAnimGraphTarget(Out.AnimBP, GraphName);
    if (!Out.Graph)
    {
        OutErr = FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
        return false;
    }
    return true;
}

// Helper: resolve UClass by path string (with FindObject + LoadObject fallback).
UClass* ResolveAnyClass(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    UClass* Cls = FindObject<UClass>(nullptr, *Path);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *Path);
    return Cls;
}

// ---------------------------------------------------------------------------
// animation.add_sequence_player
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome AddSequencePlayerImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FClusterBContext Ctx;
    FSageToolDispatch::FOutcome Err;
    if (!ResolveClusterBContext(Args, Ctx, Err)) return Err;

    FString SeqPath;
    Args->TryGetStringField(TEXT("sequence"), SeqPath);
    bool bLoop = true;
    Args->TryGetBoolField(TEXT("loop"), bLoop);
    double Rate = 1.0;
    Args->TryGetNumberField(TEXT("rate"), Rate);

    FScopedTransaction Tx(LOCTEXT("AddSeqPlayer", "Sage: Add Sequence Player"));
    Ctx.AnimBP->Modify();
    Ctx.Graph->Modify();

    UAnimGraphNode_SequencePlayer* Node = SpawnAnimGraphNode<UAnimGraphNode_SequencePlayer>(
        Ctx.Graph, Ctx.X, Ctx.Y);
    if (!Node)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("spawn failed"));
    }
    if (!SeqPath.IsEmpty())
    {
        if (UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(ResolveAsset(SeqPath)))
        {
            Node->SetAnimationAsset(Anim);
        }
    }
    // Apply loop + rate by writing the inner FAnimNode_SequencePlayer struct.
    // TODO(loop/rate): UE 5.7 protected FAnimNode_SequencePlayer::bLoopAnimation
    // + PlayRate. Direct write fails. Use animation.set_anim_node_property
    // (which goes through FStructProperty reflection) post-spawn for now.
    (void)bLoop; (void)Rate;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"),  Node->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("class"),    Node->GetClass()->GetPathName());
    if (!SeqPath.IsEmpty()) R->SetStringField(TEXT("sequence"), SeqPath);
    R->SetBoolField  (TEXT("loop"),     bLoop);
    R->SetNumberField(TEXT("rate"),     Rate);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_blendspace_player
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome AddBlendSpacePlayerImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FClusterBContext Ctx;
    FSageToolDispatch::FOutcome Err;
    if (!ResolveClusterBContext(Args, Ctx, Err)) return Err;

    FString BSPath;
    Args->TryGetStringField(TEXT("blendspace"), BSPath);

    UClass* BSPlayerCls = ResolveAnyClass(TEXT("/Script/AnimGraph.AnimGraphNode_BlendSpacePlayer"));
    if (!BSPlayerCls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("UAnimGraphNode_BlendSpacePlayer class not loadable"));
    }

    FScopedTransaction Tx(LOCTEXT("AddBSPlayer", "Sage: Add BlendSpace Player"));
    Ctx.AnimBP->Modify();
    Ctx.Graph->Modify();

    UAnimGraphNode_AssetPlayerBase* Node = nullptr;
    {
        UEdGraphNode* Raw = NewObject<UEdGraphNode>(Ctx.Graph, BSPlayerCls);
        Raw->CreateNewGuid();
        Raw->NodePosX = static_cast<int32>(Ctx.X);
        Raw->NodePosY = static_cast<int32>(Ctx.Y);
        Ctx.Graph->AddNode(Raw, false, false);
        Raw->PostPlacedNewNode();
        Raw->AllocateDefaultPins();
        Node = Cast<UAnimGraphNode_AssetPlayerBase>(Raw);
    }
    if (!Node)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("spawn failed"));
    }
    if (!BSPath.IsEmpty())
    {
        // Reject sequence/montage assets — UAnimGraphNode_BlendSpacePlayer is
        // strict about its asset class; setting a UAnimSequence here silently
        // leaves the player with a null reference at runtime.
        if (UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(BSPath)))
        {
            Node->SetAnimationAsset(BS);
        }
        else
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("blendspace must be a UBlendSpace (or UBlendSpace1D); got %s"),
                                *BSPath));
        }
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("class"),   Node->GetClass()->GetPathName());
    if (!BSPath.IsEmpty()) R->SetStringField(TEXT("blendspace"), BSPath);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// animation.add_state_machine_node
// ---------------------------------------------------------------------------
//
// Spawn a UAnimGraphNode_StateMachine that *references an existing* state
// machine sub-graph by name. Engine canonical: one SM sub-graph + one node
// referencing it; you can have multiple references but it's unusual. If the
// named sub-graph doesn't exist, defer to create_state_machine.

FSageToolDispatch::FOutcome AddStateMachineNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FClusterBContext Ctx;
    FSageToolDispatch::FOutcome Err;
    if (!ResolveClusterBContext(Args, Ctx, Err)) return Err;

    FString SMName;
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName) || SMName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'state_machine_name'"));
    }
    UAnimationStateMachineGraph* ExistingSM = FindStateMachineGraph(Ctx.AnimBP, FName(*SMName));
    if (!ExistingSM)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("state machine sub-graph '%s' not found — call create_state_machine first"),
                            *SMName));
    }

    FScopedTransaction Tx(LOCTEXT("AddSMNode", "Sage: Add State Machine Reference Node"));
    Ctx.AnimBP->Modify();
    Ctx.Graph->Modify();

    // SM-reuse path — we want this node to point at the EXISTING
    // sub-graph, not a freshly-spawned stub. PostPlacedNewNode would create
    // a brand-new empty UAnimationStateMachineGraph as EditorStateMachineGraph
    // (orphan, GC'd later), which is wasteful and noisy. Spawn raw, run
    // AllocateDefaultPins for the canonical Output Pose pin shape, then
    // bind directly.
    UAnimGraphNode_StateMachine* Node = NewObject<UAnimGraphNode_StateMachine>(Ctx.Graph);
    Node->CreateNewGuid();
    Node->NodePosX = static_cast<int32>(Ctx.X);
    Node->NodePosY = static_cast<int32>(Ctx.Y);
    Ctx.Graph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    Node->AllocateDefaultPins();
    Node->EditorStateMachineGraph = ExistingSM;
    Node->NodeComment = FString::Printf(TEXT("SAGE_STATE_MACHINE_REFERENCE:%s"), *SMName);
    Node->bCommentBubblePinned = false;
    Node->bCommentBubbleVisible = false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"),                Node->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("class"),                  Node->GetClass()->GetPathName());
    R->SetStringField(TEXT("state_machine_name"),     SMName);
    R->SetObjectField(TEXT("state_machine"),
        StateMachineReferenceJson(Ctx.AnimBP, Node, Ctx.Graph));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// Generic Cluster B helper for class-only spawns (BlendListByBool, BlendListByEnum,
// LayeredBlendPerBone, ApplyAdditive, TwoBoneIK, SkeletalControl, PlayMontageNotifyWindow,
// LinkAnimLayer). Each handler resolves its specific UClass, calls Generic, then
// applies node-specific properties (where applicable).
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome SpawnByClassPathImpl(const TSharedPtr<FJsonObject>& Args,
                                                 const TCHAR* ClassPath,
                                                 const TCHAR* DisplayName)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FClusterBContext Ctx;
    FSageToolDispatch::FOutcome Err;
    if (!ResolveClusterBContext(Args, Ctx, Err)) return Err;

    UClass* Cls = ResolveAnyClass(ClassPath);
    if (!Cls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("class %s not loadable (module not enabled?)"), ClassPath));
    }
    if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class %s is abstract/deprecated"), *Cls->GetName()));
    }

    FScopedTransaction Tx(FText::Format(
        LOCTEXT("SpawnByClass", "Sage: Add {0}"),
        FText::FromString(DisplayName)));
    Ctx.AnimBP->Modify();
    Ctx.Graph->Modify();

    UEdGraphNode* Node = NewObject<UEdGraphNode>(Ctx.Graph, Cls);
    Node->CreateNewGuid();
    Node->NodePosX = static_cast<int32>(Ctx.X);
    Node->NodePosY = static_cast<int32>(Ctx.Y);
    Ctx.Graph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("class"),   Cls->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddBlendListByIntImpl(const TSharedPtr<FJsonObject>& Args)
{
    return SpawnByClassPathImpl(Args,
        TEXT("/Script/AnimGraph.AnimGraphNode_BlendListByInt"),
        TEXT("Blend List By Int"));
}
FSageToolDispatch::FOutcome AddBlendListByBoolImpl(const TSharedPtr<FJsonObject>& Args)
{
    return SpawnByClassPathImpl(Args,
        TEXT("/Script/AnimGraph.AnimGraphNode_BlendListByBool"),
        TEXT("Blend List By Bool"));
}
FSageToolDispatch::FOutcome AddBlendListByEnumImpl(const TSharedPtr<FJsonObject>& Args)
{
    return SpawnByClassPathImpl(Args,
        TEXT("/Script/AnimGraph.AnimGraphNode_BlendListByEnum"),
        TEXT("Blend List By Enum"));
}
FSageToolDispatch::FOutcome AddBlendListPosePinImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, NodeId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'node_id'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    UEdGraph* TargetGraph = ResolveAnimGraphTarget(AnimBP, GraphName);
    if (!TargetGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph '%s' not found"), *GraphName));
    }
    UAnimGraphNode_BlendListByInt* Node = Cast<UAnimGraphNode_BlendListByInt>(
        FindGraphNodeByGuid(TargetGraph, NodeId));
    if (!Node)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("animation.add_blend_list_pose_pin currently supports UAnimGraphNode_BlendListByInt nodes"));
    }

    const int32 OldPinCount = Node->Pins.Num();
    Node->AddPinToBlendList();

    int32 InputPosePins = 0;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input && IsPosePin(Pin))
        {
            ++InputPosePins;
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"), NodeId);
    R->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
    R->SetNumberField(TEXT("old_pin_count"), OldPinCount);
    R->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
    R->SetNumberField(TEXT("input_pose_pin_count"), InputPosePins);
    R->SetBoolField(TEXT("added"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}
FSageToolDispatch::FOutcome AddLayeredBlendPerBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    return SpawnByClassPathImpl(Args,
        TEXT("/Script/AnimGraph.AnimGraphNode_LayeredBoneBlend"),
        TEXT("Layered Blend Per Bone"));
}
FSageToolDispatch::FOutcome AddApplyAdditiveImpl(const TSharedPtr<FJsonObject>& Args)
{
    return SpawnByClassPathImpl(Args,
        TEXT("/Script/AnimGraph.AnimGraphNode_ApplyAdditive"),
        TEXT("Apply Additive"));
}
FSageToolDispatch::FOutcome AddTwoBoneIKImpl(const TSharedPtr<FJsonObject>& Args)
{
    return SpawnByClassPathImpl(Args,
        TEXT("/Script/AnimGraph.AnimGraphNode_TwoBoneIK"),
        TEXT("Two Bone IK"));
}
FSageToolDispatch::FOutcome AddSkeletalControlNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    // Generic FAnimNode_SkeletalControlBase derivative — caller passes
    // `control_class` as a UClass path. Validates IsChildOf(SkeletalControlBase).
    FString ClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("control_class"), ClassPath)
        || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'control_class' (UClass path of UAnimGraphNode_SkeletalControlBase derivative)"));
    }
    // Header is included up top — use the StaticClass() directly for the
    // type check. Avoids a redundant FindObject<UClass> on every call.
    UClass* CtrlCls = ResolveAnyClass(ClassPath);
    if (!CtrlCls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("control_class not found: %s"), *ClassPath));
    }
    if (!CtrlCls->IsChildOf(UAnimGraphNode_SkeletalControlBase::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("%s is not a UAnimGraphNode_SkeletalControlBase subclass"),
                            *CtrlCls->GetName()));
    }
    return SpawnByClassPathImpl(Args, *ClassPath, TEXT("Skeletal Control"));
}
// animation.add_slot_node — spawn a UAnimGraphNode_Slot in the AnimGraph
// (Montage slot routing). Returns node_id.
FSageToolDispatch::FOutcome AddSlotNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    return SpawnByClassPathImpl(Args,
        TEXT("/Script/AnimGraph.AnimGraphNode_Slot"),
        TEXT("Slot"));
}
// AddLinkAnimLayerImpl removed — replaced by Phase 4-r6 Cluster G's
// AddLinkedAnimLayerNodeImpl (sets Interface UClass + Layer FName before
// ReconstructNode so InputPose/OutputPose pins are wired automatically).
// See Cluster G impl block below.

// ===========================================================================
// Cluster J — Runtime character.* namespace (Lyra Sage Gap #19 + ek)
// PIE-only — runtime manipulation of a live AActor's animation state.
// Inverse of editor-only handlers: requires PIE world to be running.
// ===========================================================================

UWorld* GetPieWorldOrNull()
{
    return (GEditor && GEditor->PlayWorld) ? GEditor->PlayWorld.Get() : nullptr;
}

// Resolve an AActor from a path string. Accepts FSoftObjectPath form
// ("/Temp/UEDPIE_X_Map.Map:PersistentLevel.MyActor_2"), short label match
// ("MyActor_2"), or class-based first-instance lookup ("/Script/.../MyClass").
//
// PIE shutdown race: between GetPieWorldOrNull() and the caller's use of
// the returned actor the user might end PIE. The actor itself is GC-rooted
// while we hold the pointer in this frame, but the World can become invalid.
// Re-check IsValid(World) before returning so the caller doesn't dereference
// a half-torn-down world. Caller still owns the responsibility of treating
// the returned actor as transient.
AActor* ResolveActorRuntime(const FString& Path)
{
    UWorld* World = GetPieWorldOrNull();
    if (!World) return nullptr;
    if (Path.IsEmpty()) return nullptr;

    AActor* Found = nullptr;

    // Direct soft path resolve
    FSoftObjectPath Soft(Path);
    if (UObject* Obj = Soft.ResolveObject())
    {
        Found = Cast<AActor>(Obj);
    }

    // Label / name match
    if (!Found)
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* A = *It;
            if (!A) continue;
            if (A->GetName() == Path || A->GetActorLabel() == Path
                || A->GetPathName() == Path)
            {
                Found = A;
                break;
            }
        }
    }

    // PIE shutdown race re-check — if the world tore down mid-iteration,
    // bail rather than hand back an actor whose world is gone.
    if (!IsValid(World) || !GetPieWorldOrNull())
    {
        return nullptr;
    }
    return Found;
}

USkeletalMeshComponent* FindSkeletalMeshComp(AActor* Actor)
{
    if (!Actor) return nullptr;
    return Actor->FindComponentByClass<USkeletalMeshComponent>();
}

UAnimInstance* FindAnimInstance(AActor* Actor)
{
    USkeletalMeshComponent* Mesh = FindSkeletalMeshComp(Actor);
    return Mesh ? Mesh->GetAnimInstance() : nullptr;
}

// ---------------------------------------------------------------------------
// character.play_root_motion_source (Lyra Sage Gap #19)
// ---------------------------------------------------------------------------
//
// Apply a FRootMotionSource_* to an ACharacter's UCharacterMovementComponent.
// Currently supported source types: ConstantForce (linear push), JumpForce
// (vertical impulse with optional curves). Radial / MoveTo deferred to
// follow-up (need callback wiring).

FSageToolDispatch::FOutcome CharacterPlayRootMotionSourceImpl(
    const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("requires PIE world (start Play In Editor first)"));
    }

    FString ActorPath, SourceType = TEXT("ConstantForce"), DebugName, AccumulateMode = TEXT("Override"), FinishVelMode = TEXT("MaintainLastRootMotion");
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    Args->TryGetStringField(TEXT("source_type"), SourceType);
    Args->TryGetStringField(TEXT("debug_name"), DebugName);
    Args->TryGetStringField(TEXT("accumulate_mode"), AccumulateMode);
    Args->TryGetStringField(TEXT("finish_velocity_mode"), FinishVelMode);
    double Strength = 1000.0, Duration = 0.5;
    Args->TryGetNumberField(TEXT("strength"), Strength);
    Args->TryGetNumberField(TEXT("duration"), Duration);

    FVector Direction(1, 0, 0);
    const TArray<TSharedPtr<FJsonValue>>* DirArr = nullptr;
    if (Args->TryGetArrayField(TEXT("direction"), DirArr) && DirArr && DirArr->Num() >= 3)
    {
        Direction = FVector((*DirArr)[0]->AsNumber(),
                            (*DirArr)[1]->AsNumber(),
                            (*DirArr)[2]->AsNumber());
    }

    AActor* Actor = ResolveActorRuntime(ActorPath);
    if (!Actor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("actor not found in PIE world: %s"), *ActorPath));
    }
    ACharacter* Character = Cast<ACharacter>(Actor);
    if (!Character)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("actor %s isn't an ACharacter"), *Actor->GetName()));
    }
    UCharacterMovementComponent* CMC = Character->GetCharacterMovement();
    if (!CMC)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("character has no UCharacterMovementComponent"));
    }

    ERootMotionAccumulateMode AccMode = ERootMotionAccumulateMode::Override;
    if (AccumulateMode.Equals(TEXT("Additive"), ESearchCase::IgnoreCase))
    {
        AccMode = ERootMotionAccumulateMode::Additive;
    }
    ERootMotionFinishVelocityMode FinMode = ERootMotionFinishVelocityMode::MaintainLastRootMotionVelocity;
    if (FinishVelMode.Equals(TEXT("SetVelocity"), ESearchCase::IgnoreCase))
    {
        FinMode = ERootMotionFinishVelocityMode::SetVelocity;
    }
    else if (FinishVelMode.Equals(TEXT("ClampVelocity"), ESearchCase::IgnoreCase))
    {
        FinMode = ERootMotionFinishVelocityMode::ClampVelocity;
    }

    // Optional sensitive-liftoff toggle (default true preserves prior
    // behaviour for ConstantForce). JumpForce always sets it.
    bool bSensitiveLiftoff = true;
    Args->TryGetBoolField(TEXT("sensitive_liftoff_check"), bSensitiveLiftoff);

    auto ParseVec3 = [&Args](const TCHAR* Field, FVector& OutV) -> bool
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Args->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3) return false;
        OutV = FVector((*Arr)[0]->AsNumber(),
                       (*Arr)[1]->AsNumber(),
                       (*Arr)[2]->AsNumber());
        return true;
    };

    uint16 SourceId = 0;
    if (SourceType.Equals(TEXT("ConstantForce"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<FRootMotionSource_ConstantForce> Src =
            MakeShared<FRootMotionSource_ConstantForce>();
        Src->InstanceName     = FName(*(DebugName.IsEmpty() ? TEXT("Sage_ConstantForce") : DebugName));
        Src->AccumulateMode   = AccMode;
        if (bSensitiveLiftoff)
            Src->Settings.SetFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck);
        Src->Force            = Direction.GetSafeNormal() * static_cast<float>(Strength);
        Src->Duration         = static_cast<float>(Duration);
        Src->FinishVelocityParams.Mode = FinMode;
        SourceId = CMC->ApplyRootMotionSource(Src);
    }
    else if (SourceType.Equals(TEXT("JumpForce"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<FRootMotionSource_JumpForce> Src =
            MakeShared<FRootMotionSource_JumpForce>();
        Src->InstanceName     = FName(*(DebugName.IsEmpty() ? TEXT("Sage_JumpForce") : DebugName));
        Src->AccumulateMode   = AccMode;
        Src->Duration         = static_cast<float>(Duration);
        Src->Distance         = static_cast<float>(Strength);
        Src->Rotation         = Direction.Rotation();
        Src->FinishVelocityParams.Mode = FinMode;
        if (bSensitiveLiftoff)
            Src->Settings.SetFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck);
        // Optional path / time-mapping curves — engine FRootMotionSource_JumpForce
        // exposes PathOffsetCurve (UCurveVector) and TimeMappingCurve (UCurveFloat)
        // for arc shaping. Resolve via ResolveAsset; null path = no curve.
        FString PathCurvePath, TimeCurvePath;
        if (Args->TryGetStringField(TEXT("path_offset_curve"), PathCurvePath)
            && !PathCurvePath.IsEmpty())
        {
            if (UCurveVector* PCurve = Cast<UCurveVector>(ResolveAsset(PathCurvePath)))
            {
                Src->PathOffsetCurve = PCurve;
            }
        }
        if (Args->TryGetStringField(TEXT("time_mapping_curve"), TimeCurvePath)
            && !TimeCurvePath.IsEmpty())
        {
            if (UCurveFloat* TCurve = Cast<UCurveFloat>(ResolveAsset(TimeCurvePath)))
            {
                Src->TimeMappingCurve = TCurve;
            }
        }
        SourceId = CMC->ApplyRootMotionSource(Src);
    }
    else if (SourceType.Equals(TEXT("RadialForce"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<FRootMotionSource_RadialForce> Src =
            MakeShared<FRootMotionSource_RadialForce>();
        Src->InstanceName     = FName(*(DebugName.IsEmpty() ? TEXT("Sage_RadialForce") : DebugName));
        Src->AccumulateMode   = AccMode;
        Src->Duration         = static_cast<float>(Duration);
        Src->Strength         = static_cast<float>(Strength);
        Src->FinishVelocityParams.Mode = FinMode;
        FVector RadialLoc(0, 0, 0);
        if (!ParseVec3(TEXT("location"), RadialLoc))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("RadialForce needs 'location':[x,y,z]"));
        }
        Src->Location = RadialLoc;
        double Radius = 100.0;
        Args->TryGetNumberField(TEXT("radius"), Radius);
        Src->Radius = static_cast<float>(Radius);
        SourceId = CMC->ApplyRootMotionSource(Src);
    }
    else if (SourceType.Equals(TEXT("MoveToForce"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<FRootMotionSource_MoveToForce> Src =
            MakeShared<FRootMotionSource_MoveToForce>();
        Src->InstanceName     = FName(*(DebugName.IsEmpty() ? TEXT("Sage_MoveToForce") : DebugName));
        Src->AccumulateMode   = AccMode;
        Src->Duration         = static_cast<float>(Duration);
        Src->FinishVelocityParams.Mode = FinMode;
        // StartLocation defaults to actor location; caller can override.
        Src->StartLocation = Character->GetActorLocation();
        ParseVec3(TEXT("start_location"), Src->StartLocation);
        FVector TargetLoc(0, 0, 0);
        if (!ParseVec3(TEXT("target_location"), TargetLoc))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("MoveToForce needs 'target_location':[x,y,z]"));
        }
        Src->TargetLocation = TargetLoc;
        SourceId = CMC->ApplyRootMotionSource(Src);
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unsupported source_type '%s' (try ConstantForce, JumpForce, RadialForce, MoveToForce)"),
                            *SourceType));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"),       Actor->GetPathName());
    R->SetStringField(TEXT("source_type"), SourceType);
    R->SetNumberField(TEXT("source_id"),   SourceId);
    R->SetNumberField(TEXT("duration"),    Duration);
    R->SetNumberField(TEXT("strength"),    Strength);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// character.remove_root_motion_source
// ---------------------------------------------------------------------------
//
// Cancel a running root-motion source by ID returned from
// character.play_root_motion_source. PIE-only.
FSageToolDispatch::FOutcome CharacterRemoveRootMotionSourceImpl(
    const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("requires PIE world"));
    }
    FString ActorPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    double SourceIdD = -1.0;
    if (!Args->TryGetNumberField(TEXT("source_id"), SourceIdD) || SourceIdD < 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/negative 'source_id'"));
    }
    AActor* Actor = ResolveActorRuntime(ActorPath);
    if (!Actor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("actor not found in PIE world: %s"), *ActorPath));
    }
    ACharacter* Character = Cast<ACharacter>(Actor);
    if (!Character)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("actor is not an ACharacter"));
    }
    UCharacterMovementComponent* CMC = Character->GetCharacterMovement();
    if (!CMC)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("character has no CharacterMovementComponent"));
    }
    const uint16 Id = static_cast<uint16>(SourceIdD);
    CMC->RemoveRootMotionSourceByID(Id);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"),     Actor->GetPathName());
    R->SetNumberField(TEXT("source_id"), Id);
    R->SetBoolField  (TEXT("removed"),   true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// character.play_montage
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome CharacterPlayMontageImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("requires PIE world"));
    }
    FString ActorPath, MontagePath, StartSection;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    if (!Args->TryGetStringField(TEXT("montage"), MontagePath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'montage'"));
    }
    Args->TryGetStringField(TEXT("start_section"), StartSection);
    double Rate = 1.0;
    Args->TryGetNumberField(TEXT("play_rate"), Rate);
    double StartPosition = -1.0;
    bool bHaveStartPosition = Args->TryGetNumberField(TEXT("start_position"), StartPosition);

    AActor* Actor = ResolveActorRuntime(ActorPath);
    UAnimInstance* AnimInst = FindAnimInstance(Actor);
    if (!AnimInst)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("actor has no SkeletalMeshComponent / AnimInstance"));
    }
    UAnimMontage* Montage = Cast<UAnimMontage>(ResolveAsset(MontagePath));
    if (!Montage)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimMontage: %s"), *MontagePath));
    }

    const float Duration = AnimInst->Montage_Play(Montage, static_cast<float>(Rate));
    if (Duration <= 0.f)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("Montage_Play returned 0 (ignored)"));
    }
    if (!StartSection.IsEmpty())
    {
        AnimInst->Montage_JumpToSection(FName(*StartSection), Montage);
    }
    if (bHaveStartPosition && StartPosition >= 0.0)
    {
        // Honor explicit start time after Play+JumpToSection — Montage_SetPosition
        // moves the play head while keeping play state intact (UE 5.7 canonical).
        AnimInst->Montage_SetPosition(Montage, static_cast<float>(StartPosition));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"),    Actor->GetPathName());
    R->SetStringField(TEXT("montage"),  Montage->GetPathName());
    R->SetNumberField(TEXT("length"),   Duration);
    R->SetNumberField(TEXT("play_rate"), Rate);
    if (bHaveStartPosition && StartPosition >= 0.0)
    {
        R->SetNumberField(TEXT("start_position"), StartPosition);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// character.stop_montage
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome CharacterStopMontageImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("requires PIE world"));
    }
    FString ActorPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    double BlendOut = 0.25;
    Args->TryGetNumberField(TEXT("blend_out_time"), BlendOut);

    AActor* Actor = ResolveActorRuntime(ActorPath);
    UAnimInstance* AnimInst = FindAnimInstance(Actor);
    if (!AnimInst)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("actor has no AnimInstance"));
    }

    // Optional `montage` argument — when supplied, only stops that specific
    // montage instance (engine `Montage_Stop(BlendOut, SpecificMontage)`
    // overload). Default: stop all montages.
    FString MontagePath;
    UAnimMontage* SpecificMontage = nullptr;
    if (Args->TryGetStringField(TEXT("montage"), MontagePath) && !MontagePath.IsEmpty())
    {
        SpecificMontage = Cast<UAnimMontage>(ResolveAsset(MontagePath));
        if (!SpecificMontage)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a UAnimMontage: %s"), *MontagePath));
        }
    }
    AnimInst->Montage_Stop(static_cast<float>(BlendOut), SpecificMontage);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"),  Actor->GetPathName());
    R->SetBoolField  (TEXT("stopped"), true);
    if (SpecificMontage)
    {
        R->SetStringField(TEXT("montage"), SpecificMontage->GetPathName());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// character.set_anim_instance_class
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome CharacterSetAnimInstanceClassImpl(
    const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("requires PIE world"));
    }
    FString ActorPath, ClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    if (!Args->TryGetStringField(TEXT("anim_class"), ClassPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'anim_class'"));
    }

    AActor* Actor = ResolveActorRuntime(ActorPath);
    USkeletalMeshComponent* Mesh = FindSkeletalMeshComp(Actor);
    if (!Mesh)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("actor has no SkeletalMeshComponent"));
    }
    UClass* Cls = ResolveAnyClass(ClassPath);
    if (!Cls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class not loadable: %s"), *ClassPath));
    }
    if (!Cls->IsChildOf(UAnimInstance::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("%s is not a UAnimInstance subclass"), *Cls->GetName()));
    }

    Mesh->SetAnimInstanceClass(Cls);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"),      Actor->GetPathName());
    R->SetStringField(TEXT("anim_class"), Cls->GetPathName());
    R->SetBoolField  (TEXT("swapped"),    true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// character.list_active_montages
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome CharacterListActiveMontagesImpl(
    const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("requires PIE world"));
    }
    FString ActorPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    AActor* Actor = ResolveActorRuntime(ActorPath);
    UAnimInstance* AnimInst = FindAnimInstance(Actor);
    if (!AnimInst)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("actor has no AnimInstance"));
    }

    TArray<TSharedPtr<FJsonValue>> Montages;
    for (FAnimMontageInstance* Inst : AnimInst->MontageInstances)
    {
        if (!Inst || !Inst->Montage) continue;
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("montage"),  Inst->Montage->GetPathName());
        Obj->SetNumberField(TEXT("position"), Inst->GetPosition());
        Obj->SetNumberField(TEXT("weight"),   Inst->GetWeight());
        Obj->SetNumberField(TEXT("play_rate"), Inst->GetPlayRate());
        Montages.Add(MakeShared<FJsonValueObject>(Obj));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"),    Actor->GetPathName());
    R->SetArrayField (TEXT("montages"), Montages);
    R->SetNumberField(TEXT("count"),    Montages.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// character.set_animation_mode
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome CharacterSetAnimationModeImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("requires PIE world"));
    }
    FString ActorPath, ModeStr;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    if (!Args->TryGetStringField(TEXT("mode"), ModeStr))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'mode' (AnimBlueprint | AnimAsset | Custom)"));
    }
    AActor* Actor = ResolveActorRuntime(ActorPath);
    USkeletalMeshComponent* Mesh = FindSkeletalMeshComp(Actor);
    if (!Mesh)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("actor has no SkeletalMeshComponent"));
    }
    EAnimationMode::Type Mode = EAnimationMode::AnimationBlueprint;
    if (ModeStr.Equals(TEXT("AnimAsset"), ESearchCase::IgnoreCase)
     || ModeStr.Equals(TEXT("AnimationSingleNode"), ESearchCase::IgnoreCase))
    {
        Mode = EAnimationMode::AnimationSingleNode;
    }
    else if (ModeStr.Equals(TEXT("Custom"), ESearchCase::IgnoreCase)
          || ModeStr.Equals(TEXT("AnimationCustomMode"), ESearchCase::IgnoreCase))
    {
        Mode = EAnimationMode::AnimationCustomMode;
    }
    Mesh->SetAnimationMode(Mode);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"), Actor->GetPathName());
    R->SetStringField(TEXT("mode"),  ModeStr);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// character.play_animation (single-asset playback, side-steps AnimBP)
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome CharacterPlayAnimationImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("requires PIE world"));
    }
    FString ActorPath, AnimPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    if (!Args->TryGetStringField(TEXT("animation"), AnimPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'animation'"));
    }
    bool bLoop = true;
    Args->TryGetBoolField(TEXT("looping"), bLoop);

    AActor* Actor = ResolveActorRuntime(ActorPath);
    USkeletalMeshComponent* Mesh = FindSkeletalMeshComp(Actor);
    if (!Mesh)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("actor has no SkeletalMeshComponent"));
    }
    UAnimationAsset* Anim = Cast<UAnimationAsset>(ResolveAsset(AnimPath));
    if (!Anim)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimationAsset: %s"), *AnimPath));
    }
    Mesh->PlayAnimation(Anim, bLoop);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"),     Actor->GetPathName());
    R->SetStringField(TEXT("animation"), Anim->GetPathName());
    R->SetBoolField  (TEXT("looping"),   bLoop);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ===========================================================================
// Cluster C ek — State machine deep CRUD (conduit/alias/transition rule)
// ===========================================================================

FSageToolDispatch::FOutcome AddConduitImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));
    if (!Args->TryGetStringField(TEXT("name"), Name)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    double X = 0.0, Y = 0.0;
    Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));

    FScopedTransaction Tx(LOCTEXT("AddConduit", "Sage: Add Conduit"));
    AnimBP->Modify(); SMGraph->Modify();

    UAnimStateConduitNode* ConduitNode = NewObject<UAnimStateConduitNode>(SMGraph);
    ConduitNode->CreateNewGuid();
    ConduitNode->NodePosX = (int32)X; ConduitNode->NodePosY = (int32)Y;
    SMGraph->AddNode(ConduitNode, false, false);
    ConduitNode->PostPlacedNewNode();
    ConduitNode->AllocateDefaultPins();
    if (ConduitNode->BoundGraph && ConduitNode->BoundGraph->GetFName() != FName(*Name))
    {
        FBlueprintEditorUtils::RenameGraph(ConduitNode->BoundGraph, Name);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("conduit_id"), ConduitNode->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("name"), Name);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddStateAliasImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));
    if (!Args->TryGetStringField(TEXT("name"), Name)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    double X = 0.0, Y = 0.0;
    Args->TryGetNumberField(TEXT("x"), X); Args->TryGetNumberField(TEXT("y"), Y);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));

    FScopedTransaction Tx(LOCTEXT("AddStateAlias", "Sage: Add State Alias"));
    AnimBP->Modify(); SMGraph->Modify();

    UAnimStateAliasNode* AliasNode = NewObject<UAnimStateAliasNode>(SMGraph);
    AliasNode->CreateNewGuid();
    AliasNode->NodePosX = (int32)X; AliasNode->NodePosY = (int32)Y;
    SMGraph->AddNode(AliasNode, false, false);
    AliasNode->PostPlacedNewNode();
    AliasNode->AllocateDefaultPins();

    // Optional: bGlobalAlias (alias represents *every* state in the SM) +
    // explicit aliased_states list (FGuid string array). Without populating
    // AliasedStateNodes, BP compile errors with "alias is not aliasing any
    // states". Engine UAnimStateAliasNode::AliasedStateNodes is a
    // TSet<TWeakObjectPtr<UAnimStateNodeBase>> exposed via GetAliasedStates()
    // mutable accessor.
    bool bGlobalAlias = false;
    Args->TryGetBoolField(TEXT("global_alias"), bGlobalAlias);
    AliasNode->bGlobalAlias = bGlobalAlias;

    int32 AliasedCount = 0;
    const TArray<TSharedPtr<FJsonValue>>* AliasedArr = nullptr;
    if (Args->TryGetArrayField(TEXT("aliased_states"), AliasedArr) && AliasedArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *AliasedArr)
        {
            if (!V.IsValid() || V->Type != EJson::String) continue;
            UAnimStateNodeBase* StateRef =
                FindStateNodeByGuid(SMGraph, V->AsString());
            if (StateRef)
            {
                AliasNode->GetAliasedStates().Add(StateRef);
                ++AliasedCount;
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("alias_id"),     AliasNode->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("name"),         Name);
    R->SetBoolField  (TEXT("global_alias"), bGlobalAlias);
    R->SetNumberField(TEXT("aliased_count"), AliasedCount);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetTransitionPriorityImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, TId;
    int32 Priority = 1;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));
    if (!Args->TryGetStringField(TEXT("transition_id"), TId)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'transition_id'"));
    Args->TryGetNumberField(TEXT("priority"), Priority);

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));
    UAnimStateTransitionNode* T = FindTransitionByGuid(SMGraph, TId);
    if (!T) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("transition_id not found"));

    FScopedTransaction Tx(LOCTEXT("SetTransitionPriority", "Sage: Set Transition Priority"));
    T->Modify();
    T->PriorityOrder = Priority;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("transition_id"), TId);
    R->SetNumberField(TEXT("priority"), Priority);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetStateMachineInitialStateImpl(const TSharedPtr<FJsonObject>& Args)
{
    // Initial state in UE state machine = state pointed-to by the entry node's
    // single output link. UE 5.7's entry pin is PC_Exec/"Entry", while state
    // nodes use hidden PC_Transition pins, so the generic pose-pin helper is
    // intentionally not used here.
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, StateId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));
    if (!Args->TryGetStringField(TEXT("state_id"), StateId)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_id'"));

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));
    UAnimStateNodeBase* Target = FindStateNodeByGuid(SMGraph, StateId);
    if (!Target) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state_id not found"));

    UAnimStateEntryNode* Entry = nullptr;
    for (UEdGraphNode* N : SMGraph->Nodes)
    {
        if ((Entry = Cast<UAnimStateEntryNode>(N))) break;
    }
    if (!Entry) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("entry node missing"));

    FScopedTransaction Tx(LOCTEXT("SetSMInitial", "Sage: Set Initial State"));
    AnimBP->Modify(); SMGraph->Modify(); Entry->Modify(); Target->Modify();

    UEdGraphPin* EntryOut = Entry->GetOutputPin();
    UEdGraphPin* TargetIn = Target->GetInputPin();
    if (!EntryOut || !TargetIn)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("pin lookup failed: entry=%s target=%s"),
                *DescribePinsForError(Entry), *DescribePinsForError(Target)));
    }

    const UEdGraphSchema* Schema = SMGraph->GetSchema();
    if (!Schema)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("state machine schema missing"));
    }
    EntryOut->BreakAllPinLinks();
    const bool bConnected = Schema->TryCreateConnection(EntryOut, TargetIn);
    if (!bConnected || EntryOut->LinkedTo.Num() == 0)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("schema connection failed: entry=%s target=%s"),
                *DescribePinsForError(Entry), *DescribePinsForError(Target)));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("state_id"), StateId);
    R->SetBoolField(TEXT("set"), true);
    R->SetStringField(TEXT("entry_pin"), EntryOut->PinName.ToString());
    R->SetStringField(TEXT("target_pin"), TargetIn->PinName.ToString());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ListStatesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));

    TArray<TSharedPtr<FJsonValue>> States;
    for (UEdGraphNode* N : SMGraph->Nodes)
    {
        if (UAnimStateNodeBase* S = Cast<UAnimStateNodeBase>(N))
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("state_id"), S->NodeGuid.ToString(EGuidFormats::Digits));
            Obj->SetStringField(TEXT("class"),    S->GetClass()->GetName());
            // Each derived state node has its own GetStateName() override —
            // alias nodes return the alias label, conduits return the bound
            // graph name, plain states return the bound graph name. Use the
            // virtual to stay future-proof.
            FString StateName = S->GetStateName();
            // Fallback to BoundGraph FName for nodes without a label override.
            if (StateName.IsEmpty() || StateName == TEXT("BaseState"))
            {
                if (UAnimStateNode* AsState = Cast<UAnimStateNode>(S))
                {
                    if (AsState->BoundGraph) StateName = AsState->BoundGraph->GetFName().ToString();
                }
                else if (UAnimStateConduitNode* AsCon = Cast<UAnimStateConduitNode>(S))
                {
                    if (AsCon->BoundGraph) StateName = AsCon->BoundGraph->GetFName().ToString();
                }
            }
            Obj->SetStringField(TEXT("name"),     StateName);
            Obj->SetNumberField(TEXT("x"),        S->NodePosX);
            Obj->SetNumberField(TEXT("y"),        S->NodePosY);
            // Classifier so callers don't need to parse `class` strings —
            // alias / conduit / state are the three runtime kinds.
            const TCHAR* Kind = TEXT("state");
            if (Cast<UAnimStateAliasNode>(S))   Kind = TEXT("alias");
            else if (Cast<UAnimStateConduitNode>(S)) Kind = TEXT("conduit");
            Obj->SetStringField(TEXT("kind"), Kind);
            States.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("states"), States);
    R->SetNumberField(TEXT("count"), States.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ListTransitionsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));

    TArray<TSharedPtr<FJsonValue>> Trans;
    for (UEdGraphNode* N : SMGraph->Nodes)
    {
        if (UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(N))
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("transition_id"), T->NodeGuid.ToString(EGuidFormats::Digits));
            // Endpoints — engine GetPreviousState/GetNextState walk the
            // transition's pin links so the result is authoritative even if
            // the SM was authored visually without going through Sage.
            UAnimStateNodeBase* From = T->GetPreviousState();
            UAnimStateNodeBase* To   = T->GetNextState();
            Obj->SetStringField(TEXT("from_state_id"),
                From ? From->NodeGuid.ToString(EGuidFormats::Digits) : FString());
            Obj->SetStringField(TEXT("to_state_id"),
                To   ? To->NodeGuid.ToString(EGuidFormats::Digits)   : FString());
            Obj->SetNumberField(TEXT("blend_time"),    T->CrossfadeDuration);
            Obj->SetNumberField(TEXT("priority"),      T->PriorityOrder);
            Obj->SetBoolField  (TEXT("bidirectional"), T->Bidirectional);
            Trans.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("transitions"), Trans);
    R->SetNumberField(TEXT("count"), Trans.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetTransitionRuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, TId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));
    if (!Args->TryGetStringField(TEXT("transition_id"), TId)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'transition_id'"));
    const TSharedPtr<FJsonValue>* ExpressionPtr = Args->Values.Find(TEXT("expression"));
    if (!ExpressionPtr || !ExpressionPtr->IsValid()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'expression'"));

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));
    UAnimStateTransitionNode* T = FindTransitionByGuid(SMGraph, TId);
    if (!T) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("transition_id not found"));

    if (!T->BoundGraph)
    {
        T->PostPlacedNewNode();
    }
    UAnimationTransitionGraph* RuleGraph = Cast<UAnimationTransitionGraph>(T->BoundGraph);
    if (!RuleGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("transition BoundGraph is not UAnimationTransitionGraph"));
    }
    UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(RuleGraph);
    UEdGraphPin* CanEnterPin = FindCanEnterTransitionPin(ResultNode);
    if (!ResultNode || !CanEnterPin)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("transition result pin lookup failed: result=%s"),
                *DescribePinsForError(ResultNode)));
    }

    FScopedTransaction Tx(LOCTEXT("SetTransitionRule", "Sage: Set Transition Rule"));
    AnimBP->Modify();
    SMGraph->Modify();
    T->Modify();
    RuleGraph->Modify();
    ResultNode->Modify();

    T->bAutomaticRuleBasedOnSequencePlayerInState = false;

    TArray<UEdGraphNode*> ToRemove;
    for (UEdGraphNode* Node : RuleGraph->Nodes)
    {
        if (Node && Node != ResultNode)
        {
            ToRemove.Add(Node);
        }
    }
    for (UEdGraphNode* Node : ToRemove)
    {
        FBlueprintEditorUtils::RemoveNode(AnimBP, Node, /*bDontRecompile=*/true);
    }

    const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(RuleGraph->GetSchema());
    const UEdGraphSchema* Schema = RuleGraph->GetSchema();
    if (!K2Schema || !Schema)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("transition rule graph schema is not K2-compatible"));
    }

    FTransitionRuleBuildContext BuildCtx;
    BuildCtx.AnimBP = AnimBP;
    BuildCtx.RuleGraph = RuleGraph;
    BuildCtx.Schema = Schema;
    BuildCtx.K2Schema = K2Schema;
    BuildCtx.BaseX = ResultNode->NodePosX - 320;
    BuildCtx.BaseY = ResultNode->NodePosY;

    FTransitionRuleValue RootValue = BuildTransitionRuleExpression(BuildCtx, *ExpressionPtr);
    if (!BuildCtx.Error.IsEmpty())
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602, BuildCtx.Error);
    }
    if (!EnsureBoolRuleValue(BuildCtx, RootValue, TEXT("transition rule root")))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602, BuildCtx.Error);
    }
    if (!RootValue.Pin && RootValue.bLiteral)
    {
        bool bRootLiteral = false;
        if (!ParseLiteralBoolExpression(RootValue.DefaultValue, bRootLiteral))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("root literal must be boolean"));
        }
        RootValue = BuildBoolLiteralProducer(BuildCtx, bRootLiteral);
        if (!BuildCtx.Error.IsEmpty())
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603, BuildCtx.Error);
        }
    }
    if (!RootValue.Pin)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("transition rule root did not produce a pin"));
    }

    CanEnterPin->Modify();
    CanEnterPin->BreakAllPinLinks();
    const bool bConnected = Schema->TryCreateConnection(RootValue.Pin, CanEnterPin);
    if (!bConnected || CanEnterPin->LinkedTo.Num() == 0)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to connect rule root to CanEnterTransition: source=%s result=%s"),
                *DescribePinsForError(RootValue.Pin ? RootValue.Pin->GetOwningNode() : nullptr),
                *DescribePinsForError(ResultNode)));
    }

    RuleGraph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("transition_id"), TId);
    if ((*ExpressionPtr)->Type == EJson::String)
    {
        R->SetStringField(TEXT("expression"), (*ExpressionPtr)->AsString());
    }
    else
    {
        R->SetStringField(TEXT("expression"), TEXT("<json>"));
    }
    R->SetStringField(TEXT("rule_graph"), RuleGraph->GetName());
    R->SetStringField(TEXT("result_node_id"), ResultNode->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("root_kind"), RuleValueKindName(RootValue.Kind));
    R->SetStringField(TEXT("root_pin"), RootValue.Pin->PinName.ToString());
    R->SetArrayField(TEXT("authored_nodes"), BuildCtx.AuthoredNodes);
    R->SetObjectField(TEXT("can_enter_pin"), PinSummaryJson(CanEnterPin));
    R->SetBoolField(TEXT("connected"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReadTransitionRuleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, SMName, TId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));
    if (!Args->TryGetStringField(TEXT("transition_id"), TId)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'transition_id'"));

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));
    UAnimStateTransitionNode* T = FindTransitionByGuid(SMGraph, TId);
    if (!T) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("transition_id not found"));

    UAnimationTransitionGraph* RuleGraph = Cast<UAnimationTransitionGraph>(T->BoundGraph);
    UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(RuleGraph);
    UEdGraphPin* CanEnterPin = FindCanEnterTransitionPin(ResultNode);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("transition_id"), TId);
    R->SetBoolField(TEXT("has_rule_graph"), RuleGraph != nullptr);
    R->SetBoolField(TEXT("automatic_rule"), T->bAutomaticRuleBasedOnSequencePlayerInState);
    if (RuleGraph)
    {
        R->SetStringField(TEXT("rule_graph"), RuleGraph->GetName());
        R->SetNumberField(TEXT("node_count"), RuleGraph->Nodes.Num());
    }
    if (ResultNode)
    {
        R->SetStringField(TEXT("result_node_id"), ResultNode->NodeGuid.ToString(EGuidFormats::Digits));
    }
    R->SetObjectField(TEXT("can_enter_pin"), PinSummaryJson(CanEnterPin));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetStateEnteredEventImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SMName, StateId, EventName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("state_machine_name"), SMName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_machine_name'"));
    if (!Args->TryGetStringField(TEXT("state_id"), StateId)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'state_id'"));
    if (!Args->TryGetStringField(TEXT("custom_event_name"), EventName) || EventName.IsEmpty())
    {
        Args->TryGetStringField(TEXT("event_name"), EventName);
    }
    if (EventName.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'custom_event_name'"));

    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!AnimBP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimBlueprint"));
    UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, FName(*SMName));
    if (!SMGraph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state machine not found"));

    UAnimStateNode* State = nullptr;
    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        UAnimStateNode* Candidate = Cast<UAnimStateNode>(Node);
        if (!Candidate) continue;
        const FString Guid = Candidate->NodeGuid.ToString(EGuidFormats::Digits);
        if (Guid == StateId || Candidate->GetStateName() == StateId || Candidate->GetName() == StateId)
        {
            State = Candidate;
            break;
        }
    }
    if (!State) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("state_id not found"));

    FString EventKind = TEXT("entered");
    Args->TryGetStringField(TEXT("event"), EventKind);
    Args->TryGetStringField(TEXT("event_kind"), EventKind);
    FAnimNotifyEvent* TargetEvent = &State->StateEntered;
    if (EventKind.Equals(TEXT("left"), ESearchCase::IgnoreCase) ||
        EventKind.Equals(TEXT("exited"), ESearchCase::IgnoreCase) ||
        EventKind.Equals(TEXT("exit"), ESearchCase::IgnoreCase))
    {
        TargetEvent = &State->StateLeft;
        EventKind = TEXT("left");
    }
    else if (EventKind.Equals(TEXT("fully_blended"), ESearchCase::IgnoreCase) ||
             EventKind.Equals(TEXT("full"), ESearchCase::IgnoreCase))
    {
        TargetEvent = &State->StateFullyBlended;
        EventKind = TEXT("fully_blended");
    }
    else
    {
        EventKind = TEXT("entered");
    }

    FScopedTransaction Tx(LOCTEXT("SageSetStateNotifyEvent", "Sage: Set State Notify Event"));
    State->Modify();
    TargetEvent->NotifyName = FName(*EventName);
    TargetEvent->Notify = nullptr;
    TargetEvent->NotifyStateClass = nullptr;
    TargetEvent->MontageTickType = EMontageNotifyTickType::Queued;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), AnimBP->GetPathName());
    R->SetStringField(TEXT("state_machine_name"), SMName);
    R->SetStringField(TEXT("state_id"), State->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("state_name"), State->GetStateName());
    R->SetStringField(TEXT("event"), EventKind);
    R->SetStringField(TEXT("custom_event_name"), EventName);
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ===========================================================================
// Cluster D ek — Anim notify track CRUD
// ===========================================================================

FSageToolDispatch::FOutcome AddNotifyTrackImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, TrackName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("track_name"), TrackName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'track_name'"));

    UAnimSequenceBase* Seq = Cast<UAnimSequenceBase>(ResolveAsset(Path));
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequenceBase"));

    // Persona allows duplicate track names but the timeline UI conflates
    // them. Surface the situation so callers can rename rather than silently
    // shadow an existing track.
    bool bDuplicate = false;
    const FName WantedTrack(*TrackName);
    for (const FAnimNotifyTrack& Existing : Seq->AnimNotifyTracks)
    {
        if (Existing.TrackName == WantedTrack) { bDuplicate = true; break; }
    }

    FScopedTransaction Tx(LOCTEXT("AddNotifyTrack", "Sage: Add Notify Track"));
    Seq->Modify();
    FAnimNotifyTrack NewTrack;
    NewTrack.TrackName = WantedTrack;
    NewTrack.TrackColor = FLinearColor::White;
    Seq->AnimNotifyTracks.Add(NewTrack);
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("track_name"), TrackName);
    R->SetNumberField(TEXT("track_index"), Seq->AnimNotifyTracks.Num() - 1);
    if (bDuplicate)
    {
        R->SetStringField(TEXT("_warning"), TEXT("duplicate track name"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ListNotifiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UAnimSequenceBase* Seq = Cast<UAnimSequenceBase>(ResolveAsset(Path));
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequenceBase"));

    TArray<TSharedPtr<FJsonValue>> Out;
    for (int32 i = 0; i < Seq->Notifies.Num(); ++i)
    {
        const FAnimNotifyEvent& E = Seq->Notifies[i];
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"),    i);
        Obj->SetStringField(TEXT("name"),     E.NotifyName.ToString());
        Obj->SetNumberField(TEXT("time"),     E.GetTime());
        Obj->SetNumberField(TEXT("duration"), E.GetDuration());
        Obj->SetNumberField(TEXT("track"),    E.TrackIndex);
        // Distinguish single-frame notifies from notify-state windows so the
        // caller doesn't have to infer from `duration > 0`. Engine populates
        // exactly one of FAnimNotifyEvent::Notify / NotifyStateClass.
        if (E.Notify)
        {
            Obj->SetStringField(TEXT("class"), E.Notify->GetClass()->GetPathName());
            Obj->SetStringField(TEXT("kind"),  TEXT("notify"));
        }
        else if (E.NotifyStateClass)
        {
            Obj->SetStringField(TEXT("class"), E.NotifyStateClass->GetClass()->GetPathName());
            Obj->SetStringField(TEXT("kind"),  TEXT("notify_state"));
        }
        else
        {
            Obj->SetStringField(TEXT("kind"),  TEXT("native"));
        }
        Out.Add(MakeShared<FJsonValueObject>(Obj));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("notifies"), Out);
    R->SetNumberField(TEXT("count"), Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RemoveNotifyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path;
    int32 Idx = -1;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    Args->TryGetNumberField(TEXT("index"), Idx);
    UAnimSequenceBase* Seq = Cast<UAnimSequenceBase>(ResolveAsset(Path));
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequenceBase"));
    if (Idx < 0 || Idx >= Seq->Notifies.Num()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("index out of range"));

    FScopedTransaction Tx(LOCTEXT("RemoveNotify", "Sage: Remove Notify"));
    Seq->Modify();
    Seq->Notifies.RemoveAt(Idx);
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("index"), Idx);
    R->SetBoolField(TEXT("removed"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetNotifyPositionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path;
    int32 Idx = -1;
    double Time = 0.0, Dur = -1.0;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    Args->TryGetNumberField(TEXT("index"), Idx);
    Args->TryGetNumberField(TEXT("time"), Time);
    Args->TryGetNumberField(TEXT("duration"), Dur);
    UAnimSequenceBase* Seq = Cast<UAnimSequenceBase>(ResolveAsset(Path));
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequenceBase"));
    if (Idx < 0 || Idx >= Seq->Notifies.Num()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("index out of range"));

    FScopedTransaction Tx(LOCTEXT("SetNotifyPos", "Sage: Set Notify Position"));
    Seq->Modify();
    Seq->Notifies[Idx].SetTime(static_cast<float>(Time));
    if (Dur >= 0.0) Seq->Notifies[Idx].SetDuration(static_cast<float>(Dur));
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("index"), Idx);
    R->SetNumberField(TEXT("time"),  Time);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ===========================================================================
// Cluster E ek — BlendSpace axis settings + sample CRUD
// ===========================================================================

FSageToolDispatch::FOutcome RemoveBlendSpaceSampleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path;
    int32 Idx = -1;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    Args->TryGetNumberField(TEXT("index"), Idx);
    UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(Path));
    if (!BS) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UBlendSpace"));
    if (Idx < 0 || Idx >= BS->GetBlendSamples().Num()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("index out of range"));

    FScopedTransaction Tx(LOCTEXT("RemoveBSSample", "Sage: Remove BlendSpace Sample"));
    BS->Modify();
    BS->DeleteSample(Idx);
    BS->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("index"), Idx);
    R->SetBoolField(TEXT("removed"), true);
    R->SetNumberField(TEXT("sample_count"), BS->GetBlendSamples().Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetBlendSpaceAxisImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(Path));
    if (!BS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UBlendSpace"));
    }

    FString AxisName;
    const int32 AxisIndex = ParseBlendSpaceAxisIndex(Args, BS, AxisName);
    const int32 AxisCount = Cast<UBlendSpace1D>(BS) ? 1 : 2;
    if (AxisIndex < 0 || AxisIndex >= AxisCount)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid axis '%s' for %s"), *AxisName, *BS->GetClass()->GetName()));
    }

    FStructProperty* BlendParamsProp = FindFProperty<FStructProperty>(BS->GetClass(), TEXT("BlendParameters"));
    if (!BlendParamsProp || BlendParamsProp->ArrayDim <= AxisIndex)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("BlendParameters property unavailable"));
    }

    FScopedTransaction Tx(LOCTEXT("SageSetBlendSpaceAxis", "Sage: Set BlendSpace Axis"));
    BS->Modify();
    void* AxisValue = BlendParamsProp->ContainerPtrToValuePtr<void>(BS, AxisIndex);
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("name")))
    {
        SetStructField(BlendParamsProp->Struct, AxisValue, TEXT("DisplayName"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("display_name")))
    {
        SetStructField(BlendParamsProp->Struct, AxisValue, TEXT("DisplayName"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("min")))
    {
        SetStructField(BlendParamsProp->Struct, AxisValue, TEXT("Min"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("max")))
    {
        SetStructField(BlendParamsProp->Struct, AxisValue, TEXT("Max"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("grid_divisions")))
    {
        SetStructField(BlendParamsProp->Struct, AxisValue, TEXT("GridNum"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("grid_num")))
    {
        SetStructField(BlendParamsProp->Struct, AxisValue, TEXT("GridNum"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("snap_to_grid")))
    {
        SetStructField(BlendParamsProp->Struct, AxisValue, TEXT("bSnapToGrid"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("wrap_input")))
    {
        SetStructField(BlendParamsProp->Struct, AxisValue, TEXT("bWrapInput"), V);
    }
    BS->ValidateSampleData();
    NotifyObjectPropertyChanged(BS, BlendParamsProp);
    BS->MarkPackageDirty();

    const FBlendParameter& P = BS->GetBlendParameter(AxisIndex);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BS->GetPathName());
    R->SetNumberField(TEXT("axis_index"), AxisIndex);
    R->SetStringField(TEXT("axis"), AxisIndex == 0 ? TEXT("X") : TEXT("Y"));
    R->SetStringField(TEXT("name"), P.DisplayName);
    R->SetNumberField(TEXT("min"), P.Min);
    R->SetNumberField(TEXT("max"), P.Max);
    R->SetNumberField(TEXT("grid_divisions"), P.GridNum);
    R->SetBoolField(TEXT("snap_to_grid"), P.bSnapToGrid);
    R->SetBoolField(TEXT("wrap_input"), P.bWrapInput);
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetBlendSpaceSmoothingImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(Path));
    if (!BS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UBlendSpace"));
    }

    FString AxisName;
    const int32 AxisIndex = ParseBlendSpaceAxisIndex(Args, BS, AxisName);
    const int32 AxisCount = Cast<UBlendSpace1D>(BS) ? 1 : 2;
    if (AxisIndex < 0 || AxisIndex >= AxisCount)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid axis '%s' for %s"), *AxisName, *BS->GetClass()->GetName()));
    }

    FStructProperty* InterpProp = FindFProperty<FStructProperty>(BS->GetClass(), TEXT("InterpolationParam"));
    if (!InterpProp || InterpProp->ArrayDim <= AxisIndex)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("InterpolationParam property unavailable"));
    }

    FScopedTransaction Tx(LOCTEXT("SageSetBlendSpaceSmoothing", "Sage: Set BlendSpace Smoothing"));
    BS->Modify();
    void* InterpValue = InterpProp->ContainerPtrToValuePtr<void>(BS, AxisIndex);
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("interpolation_speed")))
    {
        SetStructField(InterpProp->Struct, InterpValue, TEXT("InterpolationTime"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("interpolation_time")))
    {
        SetStructField(InterpProp->Struct, InterpValue, TEXT("InterpolationTime"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("time")))
    {
        SetStructField(InterpProp->Struct, InterpValue, TEXT("InterpolationTime"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("damping_ratio")))
    {
        SetStructField(InterpProp->Struct, InterpValue, TEXT("DampingRatio"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("max_speed")))
    {
        SetStructField(InterpProp->Struct, InterpValue, TEXT("MaxSpeed"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("interpolation_type")))
    {
        SetStructField(InterpProp->Struct, InterpValue, TEXT("InterpolationType"), V);
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(TEXT("smoothing_type")))
    {
        SetStructField(InterpProp->Struct, InterpValue, TEXT("InterpolationType"), V);
    }
    NotifyObjectPropertyChanged(BS, InterpProp);
    BS->MarkPackageDirty();

    const FInterpolationParameter& P = BS->InterpolationParam[AxisIndex];
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BS->GetPathName());
    R->SetNumberField(TEXT("axis_index"), AxisIndex);
    R->SetStringField(TEXT("axis"), AxisIndex == 0 ? TEXT("X") : TEXT("Y"));
    R->SetNumberField(TEXT("interpolation_time"), P.InterpolationTime);
    R->SetNumberField(TEXT("damping_ratio"), P.DampingRatio);
    R->SetNumberField(TEXT("max_speed"), P.MaxSpeed);
    R->SetNumberField(TEXT("interpolation_type"), static_cast<int32>(P.InterpolationType.GetValue()));
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetBlendSpaceTargetWeightImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(Path));
    if (!BS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UBlendSpace"));
    }

    FScopedTransaction Tx(LOCTEXT("SageSetBlendSpaceTargetWeight", "Sage: Set BlendSpace Target Weight Interpolation"));
    BS->Modify();
    double Speed = BS->TargetWeightInterpolationSpeedPerSec;
    Args->TryGetNumberField(TEXT("time"), Speed);
    Args->TryGetNumberField(TEXT("speed"), Speed);
    Args->TryGetNumberField(TEXT("target_weight_interpolation_speed"), Speed);
    Args->TryGetNumberField(TEXT("target_weight_interpolation_speed_per_sec"), Speed);
    BS->TargetWeightInterpolationSpeedPerSec = FMath::Max(0.0f, static_cast<float>(Speed));
    Args->TryGetBoolField(TEXT("ease_in_out"), BS->bTargetWeightInterpolationEaseInOut);
    Args->TryGetBoolField(TEXT("smoothing"), BS->bTargetWeightInterpolationEaseInOut);
    if (FProperty* Prop = FindFProperty<FProperty>(BS->GetClass(), TEXT("TargetWeightInterpolationSpeedPerSec")))
    {
        NotifyObjectPropertyChanged(BS, Prop);
    }
    BS->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BS->GetPathName());
    R->SetNumberField(TEXT("target_weight_interpolation_speed_per_sec"), BS->TargetWeightInterpolationSpeedPerSec);
    R->SetBoolField(TEXT("ease_in_out"), BS->bTargetWeightInterpolationEaseInOut);
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReadBlendSpaceSamplesImpl(const TSharedPtr<FJsonObject>& Args)
{
    // Be uniform with sibling read handlers (e.g. ReadAnimGraphImpl /
    // ListSyncMarkersImpl) — refuse to dereference editor-only data on the
    // PIE world. Even though this is read-only, UBlendSpace BlendSamples are
    // stale during PIE replay and the PIE world's transient asset can mask
    // the editor authoring asset by name.
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UBlendSpace* BS = Cast<UBlendSpace>(ResolveAsset(Path));
    if (!BS) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UBlendSpace"));

    TArray<TSharedPtr<FJsonValue>> Samples;
    for (const FBlendSample& S : BS->GetBlendSamples())
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("animation"), S.Animation ? S.Animation->GetPathName() : FString());
        Obj->SetNumberField(TEXT("x"), S.SampleValue.X);
        Obj->SetNumberField(TEXT("y"), S.SampleValue.Y);
        Samples.Add(MakeShared<FJsonValueObject>(Obj));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("samples"), Samples);
    R->SetNumberField(TEXT("count"), Samples.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ===========================================================================
// Cluster F — Sync markers, curve compression, animation modifier
// ===========================================================================

FSageToolDispatch::FOutcome AddSyncMarkerImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, Name;
    double Time = 0.0;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("marker_name"), Name)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'marker_name'"));
    Args->TryGetNumberField(TEXT("time"), Time);
    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequence"));

    // Bounds clamp — engine's sync-group runtime indexes markers as fractional
    // positions over [0, PlayLength]; out-of-range markers crash the sync graph.
    const float PlayLen = Seq->GetPlayLength();
    if (Time < 0.0 || Time > static_cast<double>(PlayLen))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("'time' %.4f out of [0, %.4f] — sequence play length"),
                            Time, PlayLen));
    }

    // Soft-warn duplicate marker name. The runtime allows duplicates but the
    // sync-group comparator picks an arbitrary one — surface the conflict.
    bool bDuplicate = false;
    const FName WantedMarker(*Name);
    for (const FAnimSyncMarker& Existing : Seq->AuthoredSyncMarkers)
    {
        if (Existing.MarkerName == WantedMarker) { bDuplicate = true; break; }
    }

    FScopedTransaction Tx(LOCTEXT("AddSync", "Sage: Add Sync Marker"));
    Seq->Modify();
    FAnimSyncMarker M;
    M.MarkerName = WantedMarker;
    M.Time       = static_cast<float>(Time);
    Seq->AuthoredSyncMarkers.Add(M);
    // Without RefreshSyncMarkerDataFromAuthored the sequence's
    // UniqueMarkerNames cache stays stale until reload — sync groups won't
    // see the new marker. UE 5.7 ENGINE_API public on AnimSequence.h:736.
    Seq->RefreshSyncMarkerDataFromAuthored();
    Seq->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("marker_name"),  Name);
    R->SetNumberField(TEXT("marker_index"), Seq->AuthoredSyncMarkers.Num() - 1);
    if (bDuplicate)
    {
        R->SetStringField(TEXT("_warning"), TEXT("duplicate marker name"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RemoveSyncMarkerImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path;
    int32 Idx = -1;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    Args->TryGetNumberField(TEXT("index"), Idx);
    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequence"));
    if (Idx < 0 || Idx >= Seq->AuthoredSyncMarkers.Num()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("index out of range"));

    FScopedTransaction Tx(LOCTEXT("RemoveSync", "Sage: Remove Sync Marker"));
    Seq->Modify();
    Seq->AuthoredSyncMarkers.RemoveAt(Idx);
    // Refresh UniqueMarkerNames cache so sync groups see the removal
    // immediately (mirror AddSyncMarkerImpl).
    Seq->RefreshSyncMarkerDataFromAuthored();
    Seq->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("index"), Idx);
    R->SetBoolField(TEXT("removed"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ListSyncMarkersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequence"));

    TArray<TSharedPtr<FJsonValue>> Out;
    for (int32 i = 0; i < Seq->AuthoredSyncMarkers.Num(); ++i)
    {
        const FAnimSyncMarker& M = Seq->AuthoredSyncMarkers[i];
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), i);
        Obj->SetStringField(TEXT("name"),  M.MarkerName.ToString());
        Obj->SetNumberField(TEXT("time"),  M.Time);
        Out.Add(MakeShared<FJsonValueObject>(Obj));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("markers"), Out);
    R->SetNumberField(TEXT("count"), Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetCurveCompressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SettingsPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    Args->TryGetStringField(TEXT("codec"), SettingsPath);
    if (SettingsPath.IsEmpty()) Args->TryGetStringField(TEXT("settings"), SettingsPath);
    if (SettingsPath.IsEmpty()) Args->TryGetStringField(TEXT("curve_compression_settings"), SettingsPath);

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequence"));
    }

    UObject* Settings = nullptr;
    FString Error;
    if (!ResolveExpectedObject(SettingsPath, TEXT("/Script/Engine.AnimCurveCompressionSettings"), Settings, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    const FString Before = GetObjectPathProperty(Seq, TEXT("CurveCompressionSettings"));
    FScopedTransaction Tx(LOCTEXT("SageSetCurveCompression", "Sage: Set Curve Compression"));
    Seq->Modify();
    if (!SetReflectedProperty(Seq, TEXT("CurveCompressionSettings"), Settings ? JsonStringValue(Settings->GetPathName()) : MakeShared<FJsonValueNull>()))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to set CurveCompressionSettings"));
    }
    Seq->RefreshCacheData();
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("curve_compression_settings_before"), Before);
    R->SetStringField(TEXT("curve_compression_settings"), GetObjectPathProperty(Seq, TEXT("CurveCompressionSettings")));
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RunAnimationModifierImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ModifierClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("modifier_class_path"), ModifierClassPath) || ModifierClassPath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("modifier_class"), ModifierClassPath);
    }
    if (ModifierClassPath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("modifier"), ModifierClassPath);
    }
    if (ModifierClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'modifier_class_path'"));
    }

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequence"));
    }
    UClass* ModifierClass = ResolveClassAsset(ModifierClassPath);
    if (!ModifierClass || !ModifierClass->IsChildOf(UAnimationModifier::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimationModifier class: %s"), *ModifierClassPath));
    }

    bool bDryRun = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    int32 BeforeFloat = 0;
    int32 BeforeTransform = 0;
    int32 BeforeAttributes = 0;
    GetAnimCurveCounts(Seq, BeforeFloat, BeforeTransform, BeforeAttributes);
    const int32 BeforeNotifies = Seq->Notifies.Num();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("modifier_class"), ModifierClass->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (bDryRun)
    {
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageRunAnimationModifier", "Sage: Run Animation Modifier"));
    Seq->Modify();
    UAnimationModifier* Modifier = NewObject<UAnimationModifier>(GetTransientPackage(), ModifierClass);
    const TSharedPtr<FJsonObject>* Properties = nullptr;
    if (Args->TryGetObjectField(TEXT("properties"), Properties) && Properties && Properties->IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
        {
            if (FProperty* Prop = FindFProperty<FProperty>(ModifierClass, FName(*Pair.Key)))
            {
                detail::SetUPropertyFromJson(Modifier, Prop, Pair.Value);
            }
        }
    }

    {
        UE::Anim::FApplyModifiersScope Scope(UE::Anim::FApplyModifiersScope::SuppressWarningAndError);
        Modifier->ApplyToAnimationSequence(Seq);
    }
    Seq->RefreshCacheData();
    Seq->MarkPackageDirty();

    int32 AfterFloat = 0;
    int32 AfterTransform = 0;
    int32 AfterAttributes = 0;
    GetAnimCurveCounts(Seq, AfterFloat, AfterTransform, AfterAttributes);
    R->SetBoolField(TEXT("modified"), true);
    R->SetNumberField(TEXT("float_curve_delta"), AfterFloat - BeforeFloat);
    R->SetNumberField(TEXT("transform_curve_delta"), AfterTransform - BeforeTransform);
    R->SetNumberField(TEXT("attribute_delta"), AfterAttributes - BeforeAttributes);
    R->SetNumberField(TEXT("notify_delta"), Seq->Notifies.Num() - BeforeNotifies);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddAnimationModifierImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ModifierClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("modifier_class"), ModifierClassPath) || ModifierClassPath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("modifier"), ModifierClassPath);
    }
    if (ModifierClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'modifier_class'"));
    }

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimSequence: %s"), *Path));
    }
    UClass* ModifierClass = ResolveClassAsset(ModifierClassPath);
    if (!ModifierClass || !ModifierClass->IsChildOf(UAnimationModifier::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimationModifier class: %s"), *ModifierClassPath));
    }

    bool bApply = true;
    bool bForceApply = true;
    bool bDryRun = false;
    Args->TryGetBoolField(TEXT("apply"), bApply);
    Args->TryGetBoolField(TEXT("force_apply"), bForceApply);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);

    int32 BeforeFloat = 0;
    int32 BeforeTransform = 0;
    int32 BeforeAttributes = 0;
    GetAnimCurveCounts(Seq, BeforeFloat, BeforeTransform, BeforeAttributes);
    const int32 BeforeModifiers = CountAnimationModifiers(Seq);
    const int32 BeforeNotifies = Seq->Notifies.Num();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("modifier_class"), ModifierClass->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("apply"), bApply);
    R->SetBoolField(TEXT("force_apply"), bForceApply);
    R->SetNumberField(TEXT("modifiers_before"), BeforeModifiers);
    if (bDryRun)
    {
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("SageAddAnimationModifier", "Sage: Add Animation Modifier"));
    Seq->Modify();
    if (!UAnimationModifiersAssetUserData::AddAnimationModifierOfClass(Seq, ModifierClass))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to add animation modifier: %s"), *ModifierClass->GetName()));
    }
    if (bApply)
    {
        TArray<UAnimSequence*> Sequences;
        Sequences.Add(Seq);
        FModuleManager::LoadModuleChecked<IAnimationModifiersModule>(TEXT("AnimationModifiers"))
            .ApplyAnimationModifiers(Sequences, bForceApply);
    }

    int32 AfterFloat = 0;
    int32 AfterTransform = 0;
    int32 AfterAttributes = 0;
    GetAnimCurveCounts(Seq, AfterFloat, AfterTransform, AfterAttributes);
    const int32 AfterModifiers = CountAnimationModifiers(Seq);
    Seq->RefreshCacheData();
    Seq->MarkPackageDirty();

    R->SetBoolField(TEXT("modified"), true);
    R->SetNumberField(TEXT("modifiers_after"), AfterModifiers);
    R->SetNumberField(TEXT("added_modifier_count"), AfterModifiers - BeforeModifiers);
    R->SetNumberField(TEXT("float_curve_delta"), AfterFloat - BeforeFloat);
    R->SetNumberField(TEXT("transform_curve_delta"), AfterTransform - BeforeTransform);
    R->SetNumberField(TEXT("attribute_delta"), AfterAttributes - BeforeAttributes);
    R->SetNumberField(TEXT("notify_delta"), Seq->Notifies.Num() - BeforeNotifies);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ===========================================================================
// Cluster G — Animation Layer Interface (REAL impl, Phase 4-r6 + Lyra Gap #24)
// ===========================================================================
//
// Lyra-canonical linked-layer pattern (B_WeaponInstanceBase.cpp:110 +
// ALI_ItemAnimLayers + ABP_Mannequin_Pistol/Rifle override BPs):
//
//   1. ALI = UAnimBlueprint with BPTYPE_Interface, AnimationGraphSchema.
//      Each declared function is one animation layer with a pose output.
//   2. Child AnimBPs implement the ALI; per-function override graphs
//      (state machines / blendspaces / etc.) flow into the function's
//      Output Pose.
//   3. Master AnimBP also implements the ALI and spawns one
//      UAnimGraphNode_LinkedAnimLayer node per function — that node calls
//      INTO the linked child class at runtime.
//   4. Runtime: Mesh->LinkAnimClassLayers(ChildClass) routes the master's
//      LinkedAnimLayer call into the chosen child override.
//
// 11 tools cover all four stages plus diagnostics and a PIE smoke test.

// --- Cluster G shared helpers ---------------------------------------------

// Detect anim layer interface (UAnimBlueprint with BPTYPE_Interface).
static bool IsAnimLayerInterface(UBlueprint* BP)
{
    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(BP);
    return AnimBP && AnimBP->BlueprintType == BPTYPE_Interface;
}

static bool ShouldScanAnimLayerPackage(const FString& PackageName)
{
    if (PackageName.IsEmpty()) return false;
    if (PackageName.StartsWith(TEXT("/Engine/"))) return false;
    if (PackageName.StartsWith(TEXT("/Script/"))) return false;
    if (PackageName.StartsWith(TEXT("/Temp/"))) return false;
    if (PackageName.StartsWith(TEXT("/Transient"))) return false;
    return true;
}

// Filter UBlueprint::FunctionGraphs to only AnimationGraphSchema-bound graphs
// (the ones that act as layer functions on an ALI).
static TArray<UEdGraph*> CollectAnimLayerFunctionGraphs(UAnimBlueprint* AnimBP)
{
    TArray<UEdGraph*> Out;
    if (!AnimBP) return Out;
    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (!Graph || !Graph->Schema) continue;
        if (Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
        {
            Out.Add(Graph);
        }
    }
    return Out;
}

static TArray<UAnimBlueprint*> LoadAnimLayerInterfacesForCollisionScan()
{
    TArray<UAnimBlueprint*> Out;
    TSet<UAnimBlueprint*> Seen;

    auto AddAnimBP = [&](UAnimBlueprint* AnimBP)
    {
        if (!AnimBP || Seen.Contains(AnimBP) || !IsAnimLayerInterface(AnimBP)) return;
        UPackage* Package = AnimBP->GetOutermost();
        if (!Package || !ShouldScanAnimLayerPackage(Package->GetName())) return;
        Seen.Add(AnimBP);
        Out.Add(AnimBP);
    };

    FARFilter Filter;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimBlueprint")));
    Filter.bRecursiveClasses = true;

    TArray<FAssetData> Found;
    GetAssetRegistry().GetAssets(Filter, Found);
    for (const FAssetData& Asset : Found)
    {
        if (!ShouldScanAnimLayerPackage(Asset.PackageName.ToString())) continue;
        AddAnimBP(Cast<UAnimBlueprint>(Asset.GetAsset()));
    }

    for (TObjectIterator<UAnimBlueprint> It; It; ++It)
    {
        AddAnimBP(*It);
    }

    return Out;
}

static TArray<TSharedPtr<FJsonValue>> BuildAnimLayerFunctionCollisionWarnings(
    UAnimBlueprint* CurrentBP,
    FName FunctionName)
{
    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (!CurrentBP || FunctionName.IsNone()) return Warnings;

    for (UAnimBlueprint* OtherBP : LoadAnimLayerInterfacesForCollisionScan())
    {
        if (!OtherBP || OtherBP == CurrentBP) continue;

        for (UEdGraph* Graph : CollectAnimLayerFunctionGraphs(OtherBP))
        {
            if (!Graph || Graph->GetFName() != FunctionName) continue;

            auto O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("type"), TEXT("same_named_anim_layer_function"));
            O->SetStringField(TEXT("blueprint"), OtherBP->GetPathName());
            O->SetStringField(TEXT("graph_name"), Graph->GetName());
            O->SetStringField(TEXT("interface_class"),
                OtherBP->GeneratedClass ? OtherBP->GeneratedClass->GetPathName() : FString());
            O->SetStringField(TEXT("message"),
                TEXT("another AnimLayerInterface already declares this function name; linked-layer renames must stay interface-aware"));
            Warnings.Add(MakeShared<FJsonValueObject>(O));
        }
    }

    return Warnings;
}

// Resolve a UClass for an anim layer interface from a path. Accepts both
// /Game/.../ALI.ALI_C (BPGC) and /Game/.../ALI.ALI (the BP itself); the latter
// resolves through ClassGeneratedBy.
static UClass* ResolveAnimLayerInterfaceClass(const FString& Path)
{
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    if (!Obj) return nullptr;
    if (UClass* Cls = Cast<UClass>(Obj)) return Cls;
    if (UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(Obj))
    {
        return IfaceBP->GeneratedClass;
    }
    return nullptr;
}

// Find FBPInterfaceDescription mutably for a given interface class on a child BP.
static FBPInterfaceDescription* FindImplementedInterfaceMutable(UBlueprint* BP, UClass* IfaceClass)
{
    if (!BP || !IfaceClass) return nullptr;
    for (FBPInterfaceDescription& Impl : BP->ImplementedInterfaces)
    {
        if (Impl.Interface == IfaceClass) return &Impl;
    }
    return nullptr;
}

// Return the override graph (if any) on a child BP for an implemented anim layer function.
static UEdGraph* FindLayerFunctionOverrideGraph(UBlueprint* BP, UClass* IfaceClass, FName FunctionName)
{
    FBPInterfaceDescription* Desc = FindImplementedInterfaceMutable(BP, IfaceClass);
    if (!Desc) return nullptr;
    for (UEdGraph* Graph : Desc->Graphs)
    {
        if (Graph && Graph->GetFName() == FunctionName) return Graph;
    }
    return nullptr;
}

// Spawn an AnimationGraphSchema-bound function graph. Used for both interface
// declarations (animation.add_layer_function) and child overrides
// (animation.add_layer_function_override). The schema's CreateDefaultNodesForGraph
// produces the Output Pose root; we ensure it explicitly to be robust against
// variations across UE point releases.
struct FSpawnedLayerGraph
{
    UEdGraph* Graph = nullptr;
    UAnimGraphNode_Root* Root = nullptr;
};

static FSpawnedLayerGraph SpawnAnimLayerFunctionGraph(UBlueprint* BP, FName FunctionName)
{
    FSpawnedLayerGraph Out;
    if (!BP) return Out;

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, FunctionName, UEdGraph::StaticClass(),
        UAnimationGraphSchema::StaticClass());
    if (!NewGraph) return Out;

    UAnimGraphNode_Root* Root = nullptr;
    for (UEdGraphNode* Node : NewGraph->Nodes)
    {
        if (UAnimGraphNode_Root* R = Cast<UAnimGraphNode_Root>(Node))
        {
            Root = R; break;
        }
    }
    if (!Root)
    {
        Root = NewObject<UAnimGraphNode_Root>(NewGraph);
        Root->CreateNewGuid();
        Root->NodePosX = 0;
        Root->NodePosY = 0;
        NewGraph->AddNode(Root, /*bUserAction=*/false, /*bSelectNewNode=*/false);
        Root->PostPlacedNewNode();
        Root->AllocateDefaultPins();
    }

    Out.Graph = NewGraph;
    Out.Root = Root;
    return Out;
}

struct FLayerFunctionInputPinSpec
{
    FName Name;
    FString DisplayName;
    FString TypeName;
    FEdGraphPinType PinType;
    bool bPose = false;
};

static bool IsLayerPoseTypeName(const FString& TypeName)
{
    const FString L = TypeName.ToLower();
    return L == TEXT("pose")
        || L == TEXT("poselink")
        || L == TEXT("pose_link")
        || L == TEXT("fposelink")
        || L == TEXT("localpose")
        || L == TEXT("local_space_pose");
}

static bool MakeLayerFunctionParameterPinType(const FString& TypeName,
                                              const FString& TypeObjectPath,
                                              bool bArray,
                                              FEdGraphPinType& OutType,
                                              FString& OutError)
{
    OutType.ResetToDefaults();
    const FString L = TypeName.ToLower();

    if (L == TEXT("bool") || L == TEXT("boolean"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (L == TEXT("int") || L == TEXT("integer") || L == TEXT("int32"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (L == TEXT("int64"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
    }
    else if (L == TEXT("real") || L == TEXT("double") || L == TEXT("float"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutType.PinSubCategory = (L == TEXT("float"))
            ? UEdGraphSchema_K2::PC_Float
            : UEdGraphSchema_K2::PC_Double;
    }
    else if (L == TEXT("string") || L == TEXT("str"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (L == TEXT("name"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
    }
    else if (L == TEXT("text"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
    }
    else if (L == TEXT("byte"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    }
    else if (L == TEXT("struct"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
    }
    else if (L == TEXT("object"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
    }
    else if (L == TEXT("class"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Class;
    }
    else if (L == TEXT("interface"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Interface;
    }
    else if (L == TEXT("softobject"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
    }
    else if (L == TEXT("softclass"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
    }
    else if (L == TEXT("pc_real"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    }
    else
    {
        OutType.PinCategory = FName(*TypeName);
    }

    if (!TypeObjectPath.IsEmpty())
    {
        UObject* TypeObject = ResolveAsset(TypeObjectPath);
        if (!TypeObject)
        {
            OutError = FString::Printf(TEXT("type_object not found for pin type '%s': %s"),
                *TypeName, *TypeObjectPath);
            return false;
        }
        OutType.PinSubCategoryObject = TypeObject;
    }
    else if (OutType.PinCategory == UEdGraphSchema_K2::PC_Struct)
    {
        OutError = TEXT("struct pins require 'type_object' (use type:'pose' for FPoseLink input poses)");
        return false;
    }

    if (bArray)
    {
        OutType.ContainerType = EPinContainerType::Array;
    }
    return true;
}

static bool ParseLayerFunctionInputPins(
    const TArray<TSharedPtr<FJsonValue>>& PinValues,
    TArray<FLayerFunctionInputPinSpec>& OutPins,
    FString& OutError)
{
    TSet<FString> SeenNames;
    for (const TSharedPtr<FJsonValue>& PinValue : PinValues)
    {
        const TSharedPtr<FJsonObject>* PinObj = nullptr;
        if (!PinValue.IsValid() || !PinValue->TryGetObject(PinObj) || !PinObj || !PinObj->IsValid())
        {
            OutError = TEXT("each pins[] entry must be an object");
            return false;
        }

        FString Name;
        FString TypeName;
        if (!(*PinObj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()
            || !(*PinObj)->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty())
        {
            OutError = TEXT("each pins[] entry requires non-empty 'name' and 'type'");
            return false;
        }

        if (SeenNames.Contains(Name))
        {
            OutError = FString::Printf(TEXT("duplicate input pin name: %s"), *Name);
            return false;
        }
        SeenNames.Add(Name);

        bool bArray = false;
        (*PinObj)->TryGetBoolField(TEXT("array"), bArray);

        FLayerFunctionInputPinSpec Spec;
        Spec.Name = FName(*Name);
        Spec.DisplayName = Name;
        Spec.TypeName = TypeName;
        Spec.bPose = IsLayerPoseTypeName(TypeName);
        if (Spec.bPose)
        {
            if (bArray)
            {
                OutError = FString::Printf(TEXT("pose input pin '%s' cannot be an array"), *Name);
                return false;
            }
            Spec.PinType = UAnimationGraphSchema::MakeLocalSpacePosePin();
        }
        else
        {
            FString TypeObjectPath;
            (*PinObj)->TryGetStringField(TEXT("type_object"), TypeObjectPath);
            if (!MakeLayerFunctionParameterPinType(TypeName, TypeObjectPath, bArray,
                                                   Spec.PinType, OutError))
            {
                return false;
            }
            if (UAnimationGraphSchema::IsPosePin(Spec.PinType))
            {
                OutError = FString::Printf(TEXT("pin '%s' resolves to a pose type; use type:'pose'"),
                    *Name);
                return false;
            }
        }
        OutPins.Add(Spec);
    }

    int32 PoseCount = 0;
    for (const FLayerFunctionInputPinSpec& Spec : OutPins)
    {
        if (Spec.bPose) ++PoseCount;
    }
    if (PoseCount == 0)
    {
        OutError = TEXT("pins[] must include at least one pose pin, e.g. {name:'SourcePose', type:'pose'}");
        return false;
    }
    return true;
}

static UEdGraph* FindLayerFunctionGraph(UAnimBlueprint* IfaceBP, FName FunctionName)
{
    if (!IfaceBP) return nullptr;
    for (UEdGraph* Graph : CollectAnimLayerFunctionGraphs(IfaceBP))
    {
        if (Graph && Graph->GetFName() == FunctionName)
        {
            return Graph;
        }
    }
    return nullptr;
}

static bool HasAnyLinkedPins(const UEdGraphNode* Node)
{
    if (!Node) return false;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->LinkedTo.Num() > 0)
        {
            return true;
        }
    }
    return false;
}

static void FireLinkedInputPoseInputChange(UAnimGraphNode_LinkedInputPose* Node)
{
    if (!Node) return;
    if (FProperty* InputsProp = UAnimGraphNode_LinkedInputPose::StaticClass()
            ->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UAnimGraphNode_LinkedInputPose, Inputs)))
    {
        FPropertyChangedEvent Event(InputsProp, EPropertyChangeType::ValueSet);
        Node->PostEditChangeProperty(Event);
    }
    else
    {
        Node->ReconstructNode();
    }
}

static TSharedPtr<FJsonObject> BuildLayerFunctionInputPinReadback(UEdGraph* Graph)
{
    TArray<UAnimGraphNode_LinkedInputPose*> Nodes;
    if (Graph)
    {
        Graph->GetNodesOfClass(Nodes);
    }
    Nodes.Sort([](const UAnimGraphNode_LinkedInputPose& A,
                  const UAnimGraphNode_LinkedInputPose& B)
    {
        if (A.NodePosY == B.NodePosY) return A.NodePosX < B.NodePosX;
        return A.NodePosY < B.NodePosY;
    });

    TArray<TSharedPtr<FJsonValue>> NodeArr;
    TArray<TSharedPtr<FJsonValue>> FunctionPins;
    for (UAnimGraphNode_LinkedInputPose* Node : Nodes)
    {
        if (!Node) continue;

        TArray<TSharedPtr<FJsonValue>> OutputPins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output || Pin->bOrphanedPin) continue;
            TSharedPtr<FJsonObject> PinObj = PinSummaryJson(Pin);
            OutputPins.Add(MakeShared<FJsonValueObject>(PinObj));

            TSharedPtr<FJsonObject> FunctionPin = MakeShared<FJsonObject>();
            if (IsPosePin(Pin))
            {
                FunctionPin->SetStringField(TEXT("name"), Node->Node.Name.ToString());
                FunctionPin->SetStringField(TEXT("kind"), TEXT("pose"));
            }
            else
            {
                FunctionPin->SetStringField(TEXT("name"), Pin->PinName.ToString());
                FunctionPin->SetStringField(TEXT("kind"), TEXT("parameter"));
            }
            FunctionPin->SetObjectField(TEXT("pin_type"), PinTypeToJson(Pin->PinType));
            FunctionPins.Add(MakeShared<FJsonValueObject>(FunctionPin));
        }

        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::Digits));
        NodeObj->SetStringField(TEXT("pose_name"), Node->Node.Name.ToString());
        NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        NodeObj->SetNumberField(TEXT("input_parameter_count"), Node->Inputs.Num());
        NodeObj->SetArrayField(TEXT("output_pins"), OutputPins);
        NodeArr.Add(MakeShared<FJsonValueObject>(NodeObj));
    }

    TSharedPtr<FJsonObject> Readback = MakeShared<FJsonObject>();
    Readback->SetArrayField(TEXT("linked_input_pose_nodes"), NodeArr);
    Readback->SetArrayField(TEXT("function_pins"), FunctionPins);
    Readback->SetNumberField(TEXT("linked_input_pose_node_count"), NodeArr.Num());
    Readback->SetNumberField(TEXT("function_pin_count"), FunctionPins.Num());
    return Readback;
}

static bool ApplyLayerFunctionInputPinsToGraph(
    UAnimBlueprint* IfaceBP,
    UEdGraph* Graph,
    const TArray<FLayerFunctionInputPinSpec>& Pins,
    bool bConnectFirstPoseToOutput,
    FString& OutError,
    TSharedPtr<FJsonObject>& OutReadback,
    bool& bOutConnectedToOutput)
{
    if (!IfaceBP || !Graph)
    {
        OutError = TEXT("missing Anim Layer Interface function graph");
        return false;
    }

    TArray<FLayerFunctionInputPinSpec> PosePins;
    TArray<FLayerFunctionInputPinSpec> ParameterPins;
    for (const FLayerFunctionInputPinSpec& Pin : Pins)
    {
        if (Pin.bPose) PosePins.Add(Pin);
        else           ParameterPins.Add(Pin);
    }
    if (PosePins.Num() == 0)
    {
        OutError = TEXT("at least one pose pin is required");
        return false;
    }

    TArray<FAnimBlueprintFunctionPinInfo> ParameterInfos;
    for (const FLayerFunctionInputPinSpec& Pin : ParameterPins)
    {
        ParameterInfos.Add(FAnimBlueprintFunctionPinInfo(Pin.Name, Pin.PinType));
    }

    TArray<UAnimGraphNode_LinkedInputPose*> ExistingNodes;
    Graph->GetNodesOfClass(ExistingNodes);
    TSet<UAnimGraphNode_LinkedInputPose*> UsedNodes;
    TArray<UAnimGraphNode_LinkedInputPose*> DesiredNodes;

    auto FindUnusedNodeByPoseName = [&](FName PoseName) -> UAnimGraphNode_LinkedInputPose*
    {
        for (UAnimGraphNode_LinkedInputPose* Node : ExistingNodes)
        {
            if (Node && !UsedNodes.Contains(Node) && Node->Node.Name == PoseName)
            {
                return Node;
            }
        }
        return nullptr;
    };

    auto FindAnyUnusedNode = [&]() -> UAnimGraphNode_LinkedInputPose*
    {
        for (UAnimGraphNode_LinkedInputPose* Node : ExistingNodes)
        {
            if (Node && !UsedNodes.Contains(Node))
            {
                return Node;
            }
        }
        return nullptr;
    };

    for (int32 PoseIndex = 0; PoseIndex < PosePins.Num(); ++PoseIndex)
    {
        const FLayerFunctionInputPinSpec& PoseSpec = PosePins[PoseIndex];
        UAnimGraphNode_LinkedInputPose* Node = FindUnusedNodeByPoseName(PoseSpec.Name);
        if (!Node)
        {
            Node = FindAnyUnusedNode();
        }
        if (!Node)
        {
            const FVector2D Pos = UAnimationGraphSchema::GetPositionForNewLinkedInputPoseNode(*Graph);
            Node = NewObject<UAnimGraphNode_LinkedInputPose>(Graph);
            if (!Node)
            {
                OutError = TEXT("failed to create UAnimGraphNode_LinkedInputPose");
                return false;
            }
            Node->CreateNewGuid();
            Node->NodePosX = static_cast<int32>(Pos.X);
            Node->NodePosY = static_cast<int32>(Pos.Y);
            Graph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
            Node->PostPlacedNewNode();
            ExistingNodes.Add(Node);
        }

        UsedNodes.Add(Node);
        DesiredNodes.Add(Node);

        Node->Modify();
        Node->Node.Name = PoseSpec.Name;
        Node->InputPoseIndex = INDEX_NONE;
        Node->Inputs = (PoseIndex == 0) ? ParameterInfos : TArray<FAnimBlueprintFunctionPinInfo>();
        FireLinkedInputPoseInputChange(Node);
    }

    for (UAnimGraphNode_LinkedInputPose* Node : ExistingNodes)
    {
        if (!Node || UsedNodes.Contains(Node)) continue;
        if (HasAnyLinkedPins(Node))
        {
            OutError = FString::Printf(
                TEXT("refusing to remove obsolete linked input pose '%s' because it has linked pins"),
                *Node->Node.Name.ToString());
            return false;
        }
        Graph->RemoveNode(Node);
    }

    if (bConnectFirstPoseToOutput && DesiredNodes.Num() > 0)
    {
        UEdGraphNode* RootNode = FindAnimGraphOutput(Graph);
        UEdGraphPin* RootInput = FindFirstInputPosePin(RootNode);
        UEdGraphPin* PoseOutput = FindFirstOutputPosePin(DesiredNodes[0]);
        if (RootInput && PoseOutput)
        {
            const bool bAlreadyLinked = RootInput->LinkedTo.Contains(PoseOutput);
            if (bAlreadyLinked)
            {
                bOutConnectedToOutput = true;
            }
            else if (RootInput->LinkedTo.Num() == 0)
            {
                RootInput->Modify();
                PoseOutput->Modify();
                bOutConnectedToOutput = Graph->GetSchema()->TryCreateConnection(PoseOutput, RootInput);
            }
        }
    }

    UAnimationGraphSchema::AutoArrangeInterfaceGraph(*Graph);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(IfaceBP);
    OutReadback = BuildLayerFunctionInputPinReadback(Graph);
    return true;
}

// --- (1) animation.create_anim_layer_interface ----------------------------

FSageToolDispatch::FOutcome CreateAnimLayerInterfaceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    if (!Args.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));

    FString Path, PackagePath, Name, SkelPath;
    Args->TryGetStringField(TEXT("path"), Path);
    Args->TryGetStringField(TEXT("package_path"), PackagePath);
    Args->TryGetStringField(TEXT("name"), Name);
    if (!Args->TryGetStringField(TEXT("skeleton"), SkelPath) || SkelPath.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    if (Path.IsEmpty())
    {
        if (PackagePath.IsEmpty() || Name.IsEmpty())
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("either 'path' or both 'package_path'+'name' must be provided"));
        Path = PackagePath / Name;
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkelPath));
    if (!Skel)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("skeleton not found: %s"), *SkelPath));

    FScopedTransaction Tx(LOCTEXT("CreateALI", "Sage: Create Anim Layer Interface"));

    UAnimBlueprintFactory* Fac = NewObject<UAnimBlueprintFactory>();
    Fac->TargetSkeleton = Skel;
    Fac->BlueprintType = BPTYPE_Interface;

    UObject* Created = CreateAssetFromPath(Path, UAnimBlueprint::StaticClass(), Fac);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("failed to create ALI at %s"), *Path));
    }
    Created->MarkPackageDirty();
    UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(Created);

    bool bCompiled = false;
    if (bCompile && IfaceBP)
    {
        FKismetEditorUtilities::CompileBlueprint(IfaceBP);
        bCompiled = true;
    }

    int32 FunctionCount = IfaceBP ? CollectAnimLayerFunctionGraphs(IfaceBP).Num() : 0;

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Created->GetPathName());
    R->SetStringField(TEXT("class_path"),
        IfaceBP && IfaceBP->GeneratedClass
            ? IfaceBP->GeneratedClass->GetPathName()
            : Created->GetPathName());
    R->SetStringField(TEXT("schema"), TEXT("AnimationGraphSchema"));
    R->SetStringField(TEXT("blueprint_type"), TEXT("BPTYPE_Interface"));
    R->SetNumberField(TEXT("function_count"), FunctionCount);
    R->SetBoolField(TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (2) animation.add_layer_function -------------------------------------

FSageToolDispatch::FOutcome AddLayerFunctionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, FuncName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function_name"), FuncName)
        || FuncName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'function_name'"));
    }
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    bool bConnectFirstPoseToOutput = true;
    Args->TryGetBoolField(TEXT("connect_first_pose_to_output"), bConnectFirstPoseToOutput);

    const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
    bool bHasInputPins = Args->TryGetArrayField(TEXT("pins"), PinValues);
    if (!bHasInputPins)
    {
        bHasInputPins = Args->TryGetArrayField(TEXT("input_pins"), PinValues);
    }
    TArray<FLayerFunctionInputPinSpec> InputPins;
    if (bHasInputPins)
    {
        FString ParseError;
        if (!PinValues || !ParseLayerFunctionInputPins(*PinValues, InputPins, ParseError))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, ParseError);
        }
    }

    UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!IfaceBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    if (!IsAnimLayerInterface(IfaceBP))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("AnimBP is not BPTYPE_Interface: %s"), *Path));

    const FName FuncFName(*FuncName);
    const TArray<TSharedPtr<FJsonValue>> CollisionWarnings =
        BuildAnimLayerFunctionCollisionWarnings(IfaceBP, FuncFName);

    // Idempotency
    for (UEdGraph* Graph : IfaceBP->FunctionGraphs)
    {
        if (!Graph || Graph->GetFName() != FuncFName) continue;
        UAnimGraphNode_Root* RootNode = nullptr;
        for (UEdGraphNode* N : Graph->Nodes)
        {
            if (UAnimGraphNode_Root* R = Cast<UAnimGraphNode_Root>(N)) { RootNode = R; break; }
        }
        TSharedPtr<FJsonObject> InputReadback;
        bool bConnectedToOutput = false;
        bool bInputPinsUpdated = false;
        bool bCompiled = false;
        if (bHasInputPins)
        {
            FScopedTransaction Tx(LOCTEXT("SetLayerFuncPinsExisting", "Sage: Set Layer Function Input Pins"));
            IfaceBP->Modify();
            Graph->Modify();
            FString ApplyError;
            if (!ApplyLayerFunctionInputPinsToGraph(IfaceBP, Graph, InputPins,
                                                    bConnectFirstPoseToOutput,
                                                    ApplyError, InputReadback,
                                                    bConnectedToOutput))
            {
                Tx.Cancel();
                return FSageToolDispatch::FOutcome::MakeError(-32603, ApplyError);
            }
            bInputPinsUpdated = true;
            if (bCompile)
            {
                FKismetEditorUtilities::CompileBlueprint(IfaceBP);
                bCompiled = true;
            }
        }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("function_name"), FuncName);
        R->SetStringField(TEXT("graph_name"), Graph->GetName());
        R->SetStringField(TEXT("schema"), TEXT("AnimationGraphSchema"));
        R->SetStringField(TEXT("root_node_id"),
            RootNode ? RootNode->NodeGuid.ToString(EGuidFormats::Digits) : FString());
        if (InputReadback.IsValid())
        {
            R->SetObjectField(TEXT("linked_input_pose_readback"), InputReadback);
        }
        R->SetBoolField(TEXT("input_pins_updated"), bInputPinsUpdated);
        R->SetBoolField(TEXT("connected_first_pose_to_output"), bConnectedToOutput);
        R->SetArrayField(TEXT("collision_warnings"), CollisionWarnings);
        R->SetNumberField(TEXT("collision_warning_count"), CollisionWarnings.Num());
        R->SetBoolField(TEXT("already"), true);
        R->SetBoolField(TEXT("compiled"), bCompiled);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("AddLayerFunc", "Sage: Add Layer Function"));
    IfaceBP->Modify();

    FSpawnedLayerGraph Spawn = SpawnAnimLayerFunctionGraph(IfaceBP, FuncFName);
    if (!Spawn.Graph)
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("CreateNewGraph returned null"));
    IfaceBP->FunctionGraphs.Add(Spawn.Graph);

    TSharedPtr<FJsonObject> InputReadback;
    bool bConnectedToOutput = false;
    if (bHasInputPins)
    {
        FString ApplyError;
        if (!ApplyLayerFunctionInputPinsToGraph(IfaceBP, Spawn.Graph, InputPins,
                                                bConnectFirstPoseToOutput,
                                                ApplyError, InputReadback,
                                                bConnectedToOutput))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603, ApplyError);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(IfaceBP);

    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(IfaceBP);
        bCompiled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("function_name"), FuncName);
    R->SetStringField(TEXT("graph_name"), Spawn.Graph->GetName());
    R->SetStringField(TEXT("schema"), TEXT("AnimationGraphSchema"));
    R->SetStringField(TEXT("root_node_id"),
        Spawn.Root ? Spawn.Root->NodeGuid.ToString(EGuidFormats::Digits) : FString());
    if (InputReadback.IsValid())
    {
        R->SetObjectField(TEXT("linked_input_pose_readback"), InputReadback);
    }
    R->SetBoolField(TEXT("input_pins_updated"), bHasInputPins);
    R->SetBoolField(TEXT("connected_first_pose_to_output"), bConnectedToOutput);
    R->SetArrayField(TEXT("collision_warnings"), CollisionWarnings);
    R->SetNumberField(TEXT("collision_warning_count"), CollisionWarnings.Num());
    R->SetBoolField(TEXT("already"), false);
    R->SetBoolField(TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (2b) animation.set_layer_function_input_pins -------------------------

FSageToolDispatch::FOutcome SetLayerFunctionInputPinsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, FuncName;
    const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function_name"), FuncName)
        || FuncName.IsEmpty()
        || !Args->TryGetArrayField(TEXT("pins"), PinValues)
        || !PinValues)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function_name', or 'pins'"));
    }
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    bool bConnectFirstPoseToOutput = true;
    Args->TryGetBoolField(TEXT("connect_first_pose_to_output"), bConnectFirstPoseToOutput);

    TArray<FLayerFunctionInputPinSpec> InputPins;
    FString ParseError;
    if (!ParseLayerFunctionInputPins(*PinValues, InputPins, ParseError))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, ParseError);
    }

    UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!IfaceBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    }
    if (!IsAnimLayerInterface(IfaceBP))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("AnimBP is not BPTYPE_Interface: %s"), *Path));
    }

    UEdGraph* Graph = FindLayerFunctionGraph(IfaceBP, FName(*FuncName));
    if (!Graph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("layer function not found: %s"), *FuncName));
    }

    FScopedTransaction Tx(LOCTEXT("SetLayerFuncPins", "Sage: Set Layer Function Input Pins"));
    IfaceBP->Modify();
    Graph->Modify();

    TSharedPtr<FJsonObject> InputReadback;
    bool bConnectedToOutput = false;
    FString ApplyError;
    if (!ApplyLayerFunctionInputPinsToGraph(IfaceBP, Graph, InputPins,
                                            bConnectFirstPoseToOutput,
                                            ApplyError, InputReadback,
                                            bConnectedToOutput))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, ApplyError);
    }

    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(IfaceBP);
        bCompiled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), IfaceBP->GetPathName());
    R->SetStringField(TEXT("function_name"), FuncName);
    R->SetStringField(TEXT("graph_name"), Graph->GetName());
    R->SetStringField(TEXT("schema"), TEXT("AnimationGraphSchema"));
    R->SetObjectField(TEXT("linked_input_pose_readback"), InputReadback);
    R->SetBoolField(TEXT("connected_first_pose_to_output"), bConnectedToOutput);
    R->SetBoolField(TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (3) animation.add_layer_function_override ----------------------------

FSageToolDispatch::FOutcome AddLayerFunctionOverrideImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, IfacePath, FuncName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("interface_path"), IfacePath)
        || !Args->TryGetStringField(TEXT("function_name"), FuncName)
        || FuncName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'interface_path', or 'function_name'"));
    }
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    UBlueprint* ChildBP = Cast<UBlueprint>(ResolveAsset(Path));
    if (!ChildBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("blueprint not found: %s"), *Path));
    UClass* IfaceCls = ResolveAnimLayerInterfaceClass(IfacePath);
    if (!IfaceCls)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface class not found: %s"), *IfacePath));

    FBPInterfaceDescription* Desc = FindImplementedInterfaceMutable(ChildBP, IfaceCls);
    if (!Desc)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface %s not implemented on %s"),
                            *IfaceCls->GetName(), *ChildBP->GetName()));

    UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(IfaceCls->ClassGeneratedBy);
    if (!IfaceBP || !IsAnimLayerInterface(IfaceBP))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface %s is not an animation layer interface"),
                            *IfaceCls->GetName()));

    const FName FuncFName(*FuncName);
    bool bDeclared = false;
    for (UEdGraph* G : IfaceBP->FunctionGraphs)
    {
        if (G && G->GetFName() == FuncFName) { bDeclared = true; break; }
    }
    if (!bDeclared)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("function '%s' not declared on interface %s"),
                            *FuncName, *IfaceCls->GetName()));

    // Idempotency
    if (UEdGraph* Existing = FindLayerFunctionOverrideGraph(ChildBP, IfaceCls, FuncFName))
    {
        UAnimGraphNode_Root* RootNode = nullptr;
        for (UEdGraphNode* N : Existing->Nodes)
        {
            if (UAnimGraphNode_Root* R = Cast<UAnimGraphNode_Root>(N)) { RootNode = R; break; }
        }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("function_name"), FuncName);
        R->SetStringField(TEXT("graph_name"), Existing->GetName());
        R->SetStringField(TEXT("schema"), TEXT("AnimationGraphSchema"));
        R->SetStringField(TEXT("output_node_id"),
            RootNode ? RootNode->NodeGuid.ToString(EGuidFormats::Digits) : FString());
        R->SetBoolField(TEXT("already"), true);
        R->SetBoolField(TEXT("compiled"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("AddOverride", "Sage: Add Layer Function Override"));
    ChildBP->Modify();

    FSpawnedLayerGraph Spawn = SpawnAnimLayerFunctionGraph(ChildBP, FuncFName);
    if (!Spawn.Graph)
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("CreateNewGraph returned null"));
    Desc->Graphs.Add(Spawn.Graph);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ChildBP);

    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(ChildBP);
        bCompiled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("function_name"), FuncName);
    R->SetStringField(TEXT("graph_name"), Spawn.Graph->GetName());
    R->SetStringField(TEXT("schema"), TEXT("AnimationGraphSchema"));
    R->SetStringField(TEXT("output_node_id"),
        Spawn.Root ? Spawn.Root->NodeGuid.ToString(EGuidFormats::Digits) : FString());
    R->SetBoolField(TEXT("already"), false);
    R->SetBoolField(TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (4) animation.implement_anim_layer_interface -------------------------

FSageToolDispatch::FOutcome ImplementAnimLayerInterfaceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, IfacePath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("interface_path"), IfacePath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'interface_path'"));
    }
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    UAnimBlueprint* ChildBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!ChildBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    UClass* IfaceCls = ResolveAnimLayerInterfaceClass(IfacePath);
    if (!IfaceCls)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface class not found: %s"), *IfacePath));
    UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(IfaceCls->ClassGeneratedBy);
    if (!IfaceBP || !IsAnimLayerInterface(IfaceBP))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not an animation layer interface: %s"), *IfacePath));

    FScopedTransaction Tx(LOCTEXT("ImplementALI", "Sage: Implement Anim Layer Interface"));
    ChildBP->Modify();

    bool bAlreadyImplemented = (FindImplementedInterfaceMutable(ChildBP, IfaceCls) != nullptr);
    if (!bAlreadyImplemented)
    {
        const FTopLevelAssetPath IfaceAssetPath(IfaceCls->GetPathName());
        if (!FBlueprintEditorUtils::ImplementNewInterface(ChildBP, IfaceAssetPath))
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                TEXT("ImplementNewInterface returned false"));
    }
    FBPInterfaceDescription* Desc = FindImplementedInterfaceMutable(ChildBP, IfaceCls);
    if (!Desc)
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("interface not implemented after ImplementNewInterface call"));

    TArray<TSharedPtr<FJsonValue>> Implemented, Already, Errors;
    for (UEdGraph* IfaceGraph : CollectAnimLayerFunctionGraphs(IfaceBP))
    {
        if (!IfaceGraph) continue;
        const FName FuncFName = IfaceGraph->GetFName();
        if (FindLayerFunctionOverrideGraph(ChildBP, IfaceCls, FuncFName))
        {
            Already.Add(MakeShared<FJsonValueString>(FuncFName.ToString()));
            continue;
        }
        FSpawnedLayerGraph Spawn = SpawnAnimLayerFunctionGraph(ChildBP, FuncFName);
        if (!Spawn.Graph)
        {
            auto E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("function_name"), FuncFName.ToString());
            E->SetStringField(TEXT("error"), TEXT("CreateNewGraph returned null"));
            Errors.Add(MakeShared<FJsonValueObject>(E));
            continue;
        }
        Desc->Graphs.Add(Spawn.Graph);
        Implemented.Add(MakeShared<FJsonValueString>(FuncFName.ToString()));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ChildBP);

    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(ChildBP);
        bCompiled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("interface_path"), IfaceCls->GetPathName());
    R->SetStringField(TEXT("interface"), IfaceCls->GetName());
    R->SetBoolField(TEXT("interface_already_implemented"), bAlreadyImplemented);
    R->SetArrayField(TEXT("functions_implemented"), Implemented);
    R->SetArrayField(TEXT("functions_already"), Already);
    R->SetArrayField(TEXT("errors"), Errors);
    R->SetBoolField(TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (5) animation.add_linked_anim_layer_node -----------------------------

FSageToolDispatch::FOutcome AddLinkedAnimLayerNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, GraphName, IfacePath, FuncName, InstClassPath;
    double X = -400.0, Y = 0.0;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("interface_path"), IfacePath)
        || !Args->TryGetStringField(TEXT("function_name"), FuncName)
        || FuncName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'interface_path', or 'function_name'"));
    }
    Args->TryGetStringField(TEXT("graph_name"), GraphName);
    Args->TryGetStringField(TEXT("instance_class_path"), InstClassPath);
    Args->TryGetNumberField(TEXT("x"), X);
    Args->TryGetNumberField(TEXT("y"), Y);
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);
    bool bExposeInputProperties = true;
    Args->TryGetBoolField(TEXT("expose_input_properties"), bExposeInputProperties);

    UAnimBlueprint* MasterBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!MasterBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    UClass* IfaceCls = ResolveAnimLayerInterfaceClass(IfacePath);
    if (!IfaceCls)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface class not found: %s"), *IfacePath));
    if (!FindImplementedInterfaceMutable(MasterBP, IfaceCls))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface %s not implemented on master %s"),
                            *IfaceCls->GetName(), *MasterBP->GetName()));

    UEdGraph* AnimGraph = ResolveAnimGraphTarget(MasterBP, GraphName);
    if (!AnimGraph)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            GraphName.IsEmpty()
                ? TEXT("master AnimBP has no AnimGraph")
                : *FString::Printf(TEXT("graph '%s' not found"), *GraphName));

    UClass* InstCls = nullptr;
    if (!InstClassPath.IsEmpty())
    {
        InstCls = ResolveAnyClass(InstClassPath);
        if (!InstCls)
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("instance_class_path not found: %s"), *InstClassPath));
    }

    FScopedTransaction Tx(LOCTEXT("AddLinkedLayer", "Sage: Add Linked Anim Layer Node"));
    MasterBP->Modify();
    AnimGraph->Modify();

    UAnimGraphNode_LinkedAnimLayer* Node = NewObject<UAnimGraphNode_LinkedAnimLayer>(AnimGraph);
    Node->CreateNewGuid();
    Node->NodePosX = static_cast<int32>(X);
    Node->NodePosY = static_cast<int32>(Y);
    AnimGraph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);

    // Step 1: Initial property writes BEFORE PostPlacedNewNode so the spawn
    // pipeline can read Interface/Layer when allocating default pose pins.
    Node->Node.Interface = IfaceCls;
    Node->Node.Layer = FName(*FuncName);
    if (InstCls)
    {
        Node->Node.InstanceClass = InstCls;
    }

    // Step 2: Canonical spawn pipeline.
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    // Step 3: Fire UE's PostEditChangeProperty pipeline for the inner
    // FAnimNode_LinkedAnimLayer fields. Without this, Layer/Interface are set
    // at the CDO level but UE's "ChangeLayer" pipeline never runs — node
    // title stays "<Interface> - None", bp.validate emits "Linked anim layer
    // node ... does not specify a layer", and the runtime layer binding is
    // never wired (Lyra Sage Gap #28). Mirror of SetLinkedLayerNameForRename
    // (Gap #26) but at spawn time instead of rename time.
    auto FirePropertyChange = [&](const TCHAR* PropertyName)
    {
        if (FProperty* P = FAnimNode_LinkedAnimLayer::StaticStruct()
                ->FindPropertyByName(FName(PropertyName)))
        {
            FPropertyChangedEvent E(P, EPropertyChangeType::ValueSet);
            Node->PostEditChangeProperty(E);
        }
    };
    FirePropertyChange(TEXT("Interface"));
    FirePropertyChange(TEXT("Layer"));
    if (InstCls)
    {
        FirePropertyChange(TEXT("InstanceClass"));
    }

    // Step 4: Final reconstruct — idempotent on top of PostEditChangeProperty
    // events, guarantees pin reallocation + title cache regeneration.
    Node->ReconstructNode();

    TArray<FName> InputPropertyPins;
    TArray<FName> NewlyExposedInputPropertyPins;
    if (bExposeInputProperties)
    {
        // Linked-layer scalar ALI params live in
        // UAnimGraphNode_CustomProperty::CustomPinProperties, not the inner
        // FAnimNode struct. Programmatic construction leaves those optional
        // pins hidden unless we expose them explicitly.
        ExposeAllCustomPropertyPins(Node, InputPropertyPins, NewlyExposedInputPropertyPins);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(MasterBP);

    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(MasterBP);
        bCompiled = true;
    }

    UEdGraphPin* InputPosePin  = FindFirstInputPosePin(Node);
    UEdGraphPin* OutputPosePin = FindFirstOutputPosePin(Node);
    TArray<TSharedPtr<FJsonValue>> PinsArr;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        PinsArr.Add(MakeShared<FJsonValueObject>(PinSummaryJson(Pin)));
    }

    // Step 5: Verify the Layer actually took effect by reading the rendered
    // node title — the same surface bp.validate/bp.search_nodes use. If the
    // title still shows "<Interface> - None" the UE pipeline silently failed;
    // surface that as a _warning so callers don't see a misleading success
    // (Gap #28 B-should).
    const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
    const bool bLayerInTitle =
        !FuncName.IsEmpty() && NodeTitle.Contains(FuncName, ESearchCase::CaseSensitive);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("class"), TEXT("/Script/AnimGraph.AnimGraphNode_LinkedAnimLayer"));
    R->SetStringField(TEXT("interface"), IfaceCls->GetName());
    R->SetStringField(TEXT("interface_path"), IfaceCls->GetPathName());
    R->SetStringField(TEXT("interface_function"), FuncName);
    R->SetStringField(TEXT("instance_class"), InstCls ? InstCls->GetPathName() : FString());
    R->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
    R->SetStringField(TEXT("input_pose_pin"),
        InputPosePin ? InputPosePin->PinName.ToString() : FString());
    R->SetStringField(TEXT("output_pose_pin"),
        OutputPosePin ? OutputPosePin->PinName.ToString() : FString());
    R->SetArrayField(TEXT("input_property_pins"), NamesToJsonArray(InputPropertyPins));
    R->SetArrayField(TEXT("newly_exposed_input_property_pins"), NamesToJsonArray(NewlyExposedInputPropertyPins));
    R->SetArrayField(TEXT("pins"), PinsArr);
    R->SetStringField(TEXT("node_title"), NodeTitle);
    R->SetBoolField  (TEXT("layer_resolved"), bLayerInTitle);
    if (!bLayerInTitle)
    {
        R->SetStringField(TEXT("_warning"),
            FString::Printf(TEXT("Layer '%s' set at CDO level but UE 'ChangeLayer' pipeline did not propagate to node title (still shows '%s'); call animation.repair_linked_anim_layer_nodes or compile the BP to force reconstruction"),
                            *FuncName, *NodeTitle));
    }
    R->SetBoolField(TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (6) animation.list_implemented_layers --------------------------------

FSageToolDispatch::FOutcome ListImplementedLayersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UBlueprint* BP = Cast<UBlueprint>(ResolveAsset(Path));
    if (!BP)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("blueprint not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> InterfaceArr;
    for (FBPInterfaceDescription& Impl : BP->ImplementedInterfaces)
    {
        if (!Impl.Interface) continue;
        UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(Impl.Interface->ClassGeneratedBy);
        if (!IfaceBP || !IsAnimLayerInterface(IfaceBP)) continue;  // anim layer filter

        TArray<TSharedPtr<FJsonValue>> FuncArr;
        for (UEdGraph* IfaceGraph : CollectAnimLayerFunctionGraphs(IfaceBP))
        {
            if (!IfaceGraph) continue;
            const FName FuncFName = IfaceGraph->GetFName();
            UEdGraph* OverrideGraph = nullptr;
            for (UEdGraph* G : Impl.Graphs)
            {
                if (G && G->GetFName() == FuncFName) { OverrideGraph = G; break; }
            }
            auto F = MakeShared<FJsonObject>();
            F->SetStringField(TEXT("name"), FuncFName.ToString());
            if (OverrideGraph)
            {
                F->SetStringField(TEXT("override_graph"), OverrideGraph->GetName());
                F->SetNumberField(TEXT("node_count"), OverrideGraph->Nodes.Num());
            }
            else
            {
                F->SetField(TEXT("override_graph"), MakeShared<FJsonValueNull>());
                F->SetNumberField(TEXT("node_count"), 0);
            }
            FuncArr.Add(MakeShared<FJsonValueObject>(F));
        }

        auto I = MakeShared<FJsonObject>();
        I->SetStringField(TEXT("interface_path"), Impl.Interface->GetPathName());
        I->SetStringField(TEXT("interface_class"), Impl.Interface->GetName());
        I->SetArrayField(TEXT("functions"), FuncArr);
        InterfaceArr.Add(MakeShared<FJsonValueObject>(I));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BP->GetPathName());
    R->SetArrayField(TEXT("interfaces"), InterfaceArr);
    R->SetNumberField(TEXT("count"), InterfaceArr.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (7) animation.list_layer_functions -----------------------------------

FSageToolDispatch::FOutcome ListLayerFunctionsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!IfaceBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    if (!IsAnimLayerInterface(IfaceBP))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("AnimBP is not BPTYPE_Interface: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> FuncArr;
    for (UEdGraph* Graph : CollectAnimLayerFunctionGraphs(IfaceBP))
    {
        if (!Graph) continue;
        bool bHasRoot = false;
        FString OutputPosePin;
        for (UEdGraphNode* N : Graph->Nodes)
        {
            if (UAnimGraphNode_Root* RootNode = Cast<UAnimGraphNode_Root>(N))
            {
                bHasRoot = true;
                if (UEdGraphPin* P = FindFirstInputPosePin(RootNode))
                {
                    OutputPosePin = P->PinName.ToString();
                }
                break;
            }
        }
        auto F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("name"), Graph->GetName());
        F->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        F->SetBoolField(TEXT("has_root_output"), bHasRoot);
        F->SetStringField(TEXT("output_pose_pin"), OutputPosePin);
        FuncArr.Add(MakeShared<FJsonValueObject>(F));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("interface_path"), IfaceBP->GetPathName());
    R->SetStringField(TEXT("interface_class"),
        IfaceBP->GeneratedClass ? IfaceBP->GeneratedClass->GetName() : IfaceBP->GetName());
    R->SetStringField(TEXT("schema"), TEXT("AnimationGraphSchema"));
    R->SetArrayField(TEXT("functions"), FuncArr);
    R->SetNumberField(TEXT("count"), FuncArr.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (8) animation.remove_layer_function_override -------------------------

FSageToolDispatch::FOutcome RemoveLayerFunctionOverrideImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, IfacePath, FuncName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("interface_path"), IfacePath)
        || !Args->TryGetStringField(TEXT("function_name"), FuncName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'interface_path', or 'function_name'"));
    }
    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("destructive op requires 'confirmed': true"));
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    UBlueprint* ChildBP = Cast<UBlueprint>(ResolveAsset(Path));
    if (!ChildBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("blueprint not found: %s"), *Path));
    UClass* IfaceCls = ResolveAnimLayerInterfaceClass(IfacePath);
    if (!IfaceCls)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface class not found: %s"), *IfacePath));
    FBPInterfaceDescription* Desc = FindImplementedInterfaceMutable(ChildBP, IfaceCls);
    if (!Desc)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("interface not implemented on this BP"));
    const FName FuncFName(*FuncName);
    UEdGraph* OverrideGraph = nullptr;
    for (UEdGraph* G : Desc->Graphs)
    {
        if (G && G->GetFName() == FuncFName) { OverrideGraph = G; break; }
    }
    if (!OverrideGraph)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("function_name"), FuncName);
        R->SetNumberField(TEXT("removed_node_count"), 0);
        R->SetBoolField(TEXT("already_absent"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    const int32 RemovedNodes = OverrideGraph->Nodes.Num();
    const FString RemovedName = OverrideGraph->GetName();

    FScopedTransaction Tx(LOCTEXT("RemoveOverride", "Sage: Remove Layer Function Override"));
    ChildBP->Modify();
    Desc->Graphs.RemoveAll([OverrideGraph](UEdGraph* G){ return G == OverrideGraph; });
    FBlueprintEditorUtils::RemoveGraph(ChildBP, OverrideGraph,
        EGraphRemoveFlags::Default);  // == Recompile | MarkTransient

    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(ChildBP);
        bCompiled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("removed_graph"), RemovedName);
    R->SetStringField(TEXT("function_name"), FuncName);
    R->SetNumberField(TEXT("removed_node_count"), RemovedNodes);
    R->SetBoolField(TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (9) animation.remove_layer_function ----------------------------------

FSageToolDispatch::FOutcome RemoveLayerFunctionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, FuncName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function_name"), FuncName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'function_name'"));
    }
    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("destructive op requires 'confirmed': true"));
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(ResolveAsset(Path));
    if (!IfaceBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UAnimBlueprint: %s"), *Path));
    if (!IsAnimLayerInterface(IfaceBP))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("AnimBP is not BPTYPE_Interface: %s"), *Path));

    const FName FuncFName(*FuncName);
    UEdGraph* DeclGraph = nullptr;
    for (UEdGraph* G : IfaceBP->FunctionGraphs)
    {
        if (G && G->GetFName() == FuncFName) { DeclGraph = G; break; }
    }
    if (!DeclGraph)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("function_name"), FuncName);
        R->SetBoolField(TEXT("already_absent"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    // Best-effort orphan implementer detection is intentionally conservative.
    // Production: run animation.list_implemented_layers across known child
    // AnimBPs before removing an AnimLayerInterface function.
    TArray<TSharedPtr<FJsonValue>> Orphans;

    const int32 NodeCount = DeclGraph->Nodes.Num();
    const FString DeclName = DeclGraph->GetName();

    FScopedTransaction Tx(LOCTEXT("RemoveLayerFunc", "Sage: Remove Layer Function"));
    IfaceBP->Modify();
    IfaceBP->FunctionGraphs.RemoveAll([DeclGraph](UEdGraph* G){ return G == DeclGraph; });
    FBlueprintEditorUtils::RemoveGraph(IfaceBP, DeclGraph,
        EGraphRemoveFlags::Default);  // == Recompile | MarkTransient

    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(IfaceBP);
        bCompiled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("removed_function"), FuncName);
    R->SetStringField(TEXT("removed_graph"), DeclName);
    R->SetNumberField(TEXT("removed_node_count"), NodeCount);
    R->SetArrayField(TEXT("orphan_implementers"), Orphans);
    R->SetStringField(TEXT("_warning"),
        TEXT("orphan implementer detection deferred - child AnimBPs that already implemented this function will retain their override graphs. Run animation.list_implemented_layers across known children before cleanup."));
    R->SetBoolField(TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (10) animation.create_linked_layer_pattern ---------------------------

FSageToolDispatch::FOutcome CreateLinkedLayerPatternImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString IfacePath, ChildPath, MasterPath, MasterGraph, SingleFunc;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("interface_path"), IfacePath)
        || !Args->TryGetStringField(TEXT("child_path"), ChildPath)
        || !Args->TryGetStringField(TEXT("master_path"), MasterPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'interface_path', 'child_path', or 'master_path'"));
    }
    Args->TryGetStringField(TEXT("master_graph"), MasterGraph);
    Args->TryGetStringField(TEXT("function_name"), SingleFunc);
    bool bCompile = false;
    Args->TryGetBoolField(TEXT("compile"), bCompile);

    UClass* IfaceCls = ResolveAnimLayerInterfaceClass(IfacePath);
    if (!IfaceCls)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface class not found: %s"), *IfacePath));
    UAnimBlueprint* IfaceBP = Cast<UAnimBlueprint>(IfaceCls->ClassGeneratedBy);
    if (!IfaceBP || !IsAnimLayerInterface(IfaceBP))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("not an animation layer interface"));

    UAnimBlueprint* ChildBP  = Cast<UAnimBlueprint>(ResolveAsset(ChildPath));
    UAnimBlueprint* MasterBP = Cast<UAnimBlueprint>(ResolveAsset(MasterPath));
    if (!ChildBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("child AnimBP not found"));
    if (!MasterBP)
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("master AnimBP not found"));

    TArray<FName> TargetFuncs;
    if (!SingleFunc.IsEmpty())
    {
        TargetFuncs.Add(FName(*SingleFunc));
    }
    else
    {
        for (UEdGraph* G : CollectAnimLayerFunctionGraphs(IfaceBP))
        {
            if (G) TargetFuncs.Add(G->GetFName());
        }
    }

    TArray<TSharedPtr<FJsonValue>> Errors, Functions, OverrideGraphsOut;
    FString LinkedNodeId;

    FScopedTransaction Tx(LOCTEXT("LinkedLayerPattern", "Sage: Linked Layer Pattern"));
    ChildBP->Modify();
    MasterBP->Modify();

    // Step 1: child implements interface
    bool bChildImplemented = (FindImplementedInterfaceMutable(ChildBP, IfaceCls) != nullptr);
    if (!bChildImplemented)
    {
        const FTopLevelAssetPath IfaceAssetPath(IfaceCls->GetPathName());
        if (!FBlueprintEditorUtils::ImplementNewInterface(ChildBP, IfaceAssetPath))
        {
            auto E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("step"), TEXT("child_implement"));
            E->SetStringField(TEXT("error"), TEXT("ImplementNewInterface returned false on child"));
            Errors.Add(MakeShared<FJsonValueObject>(E));
        }
    }
    FBPInterfaceDescription* ChildDesc = FindImplementedInterfaceMutable(ChildBP, IfaceCls);

    // Step 2: child override graphs spawn (per target function)
    if (ChildDesc)
    {
        for (FName FuncFName : TargetFuncs)
        {
            if (FindLayerFunctionOverrideGraph(ChildBP, IfaceCls, FuncFName))
            {
                Functions.Add(MakeShared<FJsonValueString>(FuncFName.ToString()));
                continue;
            }
            FSpawnedLayerGraph Spawn = SpawnAnimLayerFunctionGraph(ChildBP, FuncFName);
            if (!Spawn.Graph)
            {
                auto E = MakeShared<FJsonObject>();
                E->SetStringField(TEXT("step"), TEXT("child_override"));
                E->SetStringField(TEXT("function_name"), FuncFName.ToString());
                E->SetStringField(TEXT("error"), TEXT("CreateNewGraph returned null"));
                Errors.Add(MakeShared<FJsonValueObject>(E));
                continue;
            }
            ChildDesc->Graphs.Add(Spawn.Graph);
            OverrideGraphsOut.Add(MakeShared<FJsonValueString>(Spawn.Graph->GetName()));
            Functions.Add(MakeShared<FJsonValueString>(FuncFName.ToString()));
        }
    }

    // Step 3: master implements interface
    bool bMasterImplemented = (FindImplementedInterfaceMutable(MasterBP, IfaceCls) != nullptr);
    if (!bMasterImplemented)
    {
        const FTopLevelAssetPath IfaceAssetPath(IfaceCls->GetPathName());
        if (!FBlueprintEditorUtils::ImplementNewInterface(MasterBP, IfaceAssetPath))
        {
            auto E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("step"), TEXT("master_implement"));
            E->SetStringField(TEXT("error"), TEXT("ImplementNewInterface returned false on master"));
            Errors.Add(MakeShared<FJsonValueObject>(E));
        }
    }

    // Step 4: master AnimGraph spawns one LinkedAnimLayer node (first target func).
    // Caller can repeat add_linked_anim_layer_node for additional functions.
    UEdGraph* AnimGraph = ResolveAnimGraphTarget(MasterBP, MasterGraph);
    if (AnimGraph && TargetFuncs.Num() > 0)
    {
        AnimGraph->Modify();
        UAnimGraphNode_LinkedAnimLayer* Node = NewObject<UAnimGraphNode_LinkedAnimLayer>(AnimGraph);
        Node->CreateNewGuid();
        Node->NodePosX = -400;
        Node->NodePosY = 0;
        AnimGraph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
        Node->Node.Interface = IfaceCls;
        Node->Node.Layer = TargetFuncs[0];
        if (ChildBP->GeneratedClass)
        {
            Node->Node.InstanceClass = ChildBP->GeneratedClass;
        }
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Node->ReconstructNode();
        LinkedNodeId = Node->NodeGuid.ToString(EGuidFormats::Digits);
    }
    else if (TargetFuncs.Num() == 0)
    {
        auto E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("step"), TEXT("master_link_node"));
        E->SetStringField(TEXT("error"), TEXT("no functions declared on interface; nothing to link"));
        Errors.Add(MakeShared<FJsonValueObject>(E));
    }
    else
    {
        auto E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("step"), TEXT("master_link_node"));
        E->SetStringField(TEXT("error"), TEXT("master AnimGraph not found"));
        Errors.Add(MakeShared<FJsonValueObject>(E));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ChildBP);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(MasterBP);

    bool bChildCompiled = false, bMasterCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(ChildBP);  bChildCompiled = true;
        FKismetEditorUtilities::CompileBlueprint(MasterBP); bMasterCompiled = true;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("interface_path"), IfaceCls->GetPathName());
    R->SetStringField(TEXT("child_path"),  ChildBP->GetPathName());
    R->SetStringField(TEXT("master_path"), MasterBP->GetPathName());
    R->SetArrayField (TEXT("functions"), Functions);
    R->SetArrayField (TEXT("override_graphs"), OverrideGraphsOut);
    R->SetStringField(TEXT("linked_layer_node_id"), LinkedNodeId);
    R->SetBoolField  (TEXT("child_compiled"),  bChildCompiled);
    R->SetBoolField  (TEXT("master_compiled"), bMasterCompiled);
    R->SetArrayField (TEXT("errors"), Errors);
    if (Errors.Num() > 0)
    {
        R->SetStringField(TEXT("rollback_advice"),
            TEXT("inspect 'errors[]' for partial state; clean restart = animation.remove_layer_function_override per spawned override + bp.remove_interface on master/child"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (11) animation.set_linked_anim_layer (PIE/preview runtime) -----------

FSageToolDispatch::FOutcome SetLinkedAnimLayerImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorPath, LayerClassPath, MeshComp, Mode = TEXT("link");
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("actor"), ActorPath)
        || !Args->TryGetStringField(TEXT("layer_class"), LayerClassPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'actor' or 'layer_class'"));
    }
    Args->TryGetStringField(TEXT("mesh_component"), MeshComp);
    Args->TryGetStringField(TEXT("mode"), Mode);
    Mode = Mode.ToLower();

    UClass* LayerCls = ResolveAnyClass(LayerClassPath);
    if (LayerCls && !LayerCls->IsChildOf(UAnimInstance::StaticClass()))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("layer_class %s is not a UAnimInstance subclass"), *LayerClassPath));

    AActor* Actor = ResolveActorRuntime(ActorPath);
    USkeletalMeshComponent* Mesh = nullptr;
    if (Actor)
    {
        if (!MeshComp.IsEmpty())
        {
            for (UActorComponent* C : Actor->GetComponents())
            {
                if (C && C->GetName() == MeshComp)
                {
                    Mesh = Cast<USkeletalMeshComponent>(C);
                    if (Mesh) break;
                }
            }
            if (!Mesh)
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("mesh_component '%s' not found on actor"), *MeshComp));
        }
        else
        {
            Mesh = FindSkeletalMeshComp(Actor);
        }
    }

    auto R = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Errors;
    R->SetStringField(TEXT("actor"), Actor ? Actor->GetPathName() : ActorPath);
    R->SetStringField(TEXT("mesh"),  Mesh ? Mesh->GetName() : FString());
    R->SetStringField(TEXT("layer_class"), LayerCls ? LayerCls->GetPathName() : LayerClassPath);
    R->SetStringField(TEXT("mode"), Mode);

    if (!Actor || !Mesh)
    {
        R->SetBoolField(TEXT("linked"), false);
        R->SetStringField(TEXT("_warning"),
            TEXT("editor preview only — no PIE actor / mesh component resolved; nothing applied. Run during PIE for runtime effect."));
        R->SetArrayField(TEXT("errors"), Errors);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    TArray<TSharedPtr<FJsonValue>> Previous;
    if (UAnimInstance* AI = Mesh->GetAnimInstance())
    {
        if (UClass* AICls = AI->GetClass())
        {
            Previous.Add(MakeShared<FJsonValueString>(AICls->GetPathName()));
        }
    }
    R->SetArrayField(TEXT("previous_layers"), Previous);

    if (Mode == TEXT("link"))
    {
        if (!LayerCls)
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("link mode requires a resolvable layer_class"));
        Mesh->LinkAnimClassLayers(LayerCls);
        R->SetBoolField(TEXT("linked"), true);
    }
    else if (Mode == TEXT("unlink"))
    {
        if (!LayerCls)
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("unlink mode requires a resolvable layer_class"));
        Mesh->UnlinkAnimClassLayers(LayerCls);
        R->SetBoolField(TEXT("linked"), false);
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid mode '%s'; must be 'link' or 'unlink'"), *Mode));
    }

    R->SetArrayField(TEXT("errors"), Errors);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// --- (12) animation.repair_linked_anim_layer_nodes ------------------------
// Lyra Sage Gap #28 C nice-to-have — sweeps every UAnimGraphNode_LinkedAnimLayer
// in the project (or a single BP if `path` provided), detects nodes whose
// inner FAnimNode_LinkedAnimLayer::Layer FName is set but whose rendered node
// title still shows "<Interface> - None" (UE's ChangeLayer pipeline never
// fired). For each, fires PostEditChangeProperty(Layer) + ReconstructNode to
// repair. This rescues ABPs corrupted by older Sage spawn paths.

FSageToolDispatch::FOutcome RepairLinkedAnimLayerNodesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString TargetPath;
    bool bDryRun = false;
    bool bCompile = false;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), TargetPath);
        Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
        Args->TryGetBoolField(TEXT("compile"), bCompile);
    }

    TArray<UAnimBlueprint*> TargetBPs;
    if (!TargetPath.IsEmpty())
    {
        UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(ResolveAsset(TargetPath));
        if (!AnimBP)
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a UAnimBlueprint: %s"), *TargetPath));
        TargetBPs.Add(AnimBP);
    }
    else
    {
        TargetBPs = LoadAnimLayerInterfacesForCollisionScan();  // ALI filter
        // Plus all anim BPs that may host linked-layer nodes — wider scan.
        FARFilter Filter;
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.AnimBlueprint")));
        Filter.bRecursiveClasses = true;
        TArray<FAssetData> Found;
        GetAssetRegistry().GetAssets(Filter, Found);
        TSet<UAnimBlueprint*> Seen(TargetBPs);
        for (const FAssetData& Asset : Found)
        {
            const FString Pkg = Asset.PackageName.ToString();
            if (!ShouldScanAnimLayerPackage(Pkg)) continue;
            if (UAnimBlueprint* BP = Cast<UAnimBlueprint>(Asset.GetAsset()))
            {
                if (!Seen.Contains(BP)) { Seen.Add(BP); TargetBPs.Add(BP); }
            }
        }
        for (TObjectIterator<UAnimBlueprint> It; It; ++It)
        {
            UAnimBlueprint* BP = *It;
            if (!BP) continue;
            UPackage* Pkg = BP->GetOutermost();
            if (!Pkg || !ShouldScanAnimLayerPackage(Pkg->GetName())) continue;
            if (!Seen.Contains(BP)) { Seen.Add(BP); TargetBPs.Add(BP); }
        }
    }

    FScopedTransaction Tx(LOCTEXT("RepairLinkedLayer", "Sage: Repair Linked Anim Layer Nodes"));

    int32 ScannedNodeCount = 0;
    int32 RepairedNodeCount = 0;
    int32 NeedsRepairCount = 0;
    TArray<TSharedPtr<FJsonValue>> Details;
    TSet<FString> AffectedBlueprints;
    TSet<UBlueprint*> StructurallyChangedBPs;

    auto FirePropertyChange = [](UAnimGraphNode_LinkedAnimLayer* Node, const TCHAR* PropertyName)
    {
        if (FProperty* P = FAnimNode_LinkedAnimLayer::StaticStruct()
                ->FindPropertyByName(FName(PropertyName)))
        {
            FPropertyChangedEvent E(P, EPropertyChangeType::ValueSet);
            Node->PostEditChangeProperty(E);
        }
    };

    // Local graph traversal — FBpGraphEntry/CollectAllGraphs is in
    // SageBlueprintTools.cpp's anonymous namespace and not visible here.
    // We replicate the relevant subset (function graphs + ubergraph pages +
    // macro graphs + interface override graphs) inline. LinkedAnimLayer nodes
    // can only live inside AnimGraph schema graphs, so this coverage is
    // complete for the repair sweep.
    auto CollectAnimGraphs = [](UAnimBlueprint* BP) -> TArray<UEdGraph*>
    {
        TArray<UEdGraph*> Out;
        if (!BP) return Out;
        for (UEdGraph* G : BP->FunctionGraphs)  if (G) Out.Add(G);
        for (UEdGraph* G : BP->UbergraphPages)  if (G) Out.Add(G);
        for (UEdGraph* G : BP->MacroGraphs)     if (G) Out.Add(G);
        for (FBPInterfaceDescription& Impl : BP->ImplementedInterfaces)
        {
            for (UEdGraph* G : Impl.Graphs) if (G) Out.Add(G);
        }
        return Out;
    };

    for (UAnimBlueprint* AnimBP : TargetBPs)
    {
        if (!AnimBP) continue;
        const TArray<UEdGraph*> AllGraphs = CollectAnimGraphs(AnimBP);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (!Graph) continue;
            for (UEdGraphNode* GraphNode : Graph->Nodes)
            {
                UAnimGraphNode_LinkedAnimLayer* Node =
                    Cast<UAnimGraphNode_LinkedAnimLayer>(GraphNode);
                if (!Node) continue;
                ++ScannedNodeCount;

                const FName Layer = Node->Node.Layer;
                if (Layer.IsNone()) continue;  // genuinely empty — not corrupt

                const FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
                const bool bTitleResolved =
                    Title.Contains(Layer.ToString(), ESearchCase::CaseSensitive);
                if (bTitleResolved) continue;  // healthy

                ++NeedsRepairCount;

                auto D = MakeShared<FJsonObject>();
                D->SetStringField(TEXT("blueprint"), AnimBP->GetPathName());
                D->SetStringField(TEXT("graph"), Graph->GetName());
                D->SetStringField(TEXT("node_id"),
                    Node->NodeGuid.ToString(EGuidFormats::Digits));
                D->SetStringField(TEXT("interface"),
                    Node->Node.Interface.Get()
                        ? Node->Node.Interface.Get()->GetPathName()
                        : FString());
                D->SetStringField(TEXT("layer"), Layer.ToString());
                D->SetStringField(TEXT("before_title"), Title);

                if (!bDryRun)
                {
                    AnimBP->Modify();
                    Graph->Modify();
                    Node->Modify();
                    FirePropertyChange(Node, TEXT("Interface"));
                    FirePropertyChange(Node, TEXT("Layer"));
                    Node->ReconstructNode();
                    StructurallyChangedBPs.Add(AnimBP);

                    const FString After =
                        Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
                    D->SetStringField(TEXT("after_title"), After);
                    D->SetBoolField(TEXT("repaired"),
                        After.Contains(Layer.ToString(), ESearchCase::CaseSensitive));
                    ++RepairedNodeCount;
                }
                else
                {
                    D->SetStringField(TEXT("after_title"), TEXT("(dry_run — not modified)"));
                    D->SetBoolField(TEXT("repaired"), false);
                }

                AffectedBlueprints.Add(AnimBP->GetPathName());
                Details.Add(MakeShared<FJsonValueObject>(D));
            }
        }
    }

    if (!bDryRun)
    {
        for (UBlueprint* BP : StructurallyChangedBPs)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        }
    }
    else
    {
        Tx.Cancel();
    }

    bool bCompiled = false;
    if (bCompile && !bDryRun)
    {
        for (UBlueprint* BP : StructurallyChangedBPs)
        {
            FKismetEditorUtilities::CompileBlueprint(BP);
        }
        bCompiled = StructurallyChangedBPs.Num() > 0;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("scanned_node_count"), ScannedNodeCount);
    R->SetNumberField(TEXT("needs_repair_count"), NeedsRepairCount);
    R->SetNumberField(TEXT("repaired_node_count"), RepairedNodeCount);
    R->SetBoolField  (TEXT("dry_run"), bDryRun);
    {
        TArray<FString> Sorted = AffectedBlueprints.Array();
        Sorted.Sort();
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& S : Sorted) Arr.Add(MakeShared<FJsonValueString>(S));
        R->SetArrayField(TEXT("affected_blueprints"), Arr);
    }
    R->SetArrayField(TEXT("details"), Details);
    R->SetBoolField  (TEXT("compiled"), bCompiled);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ===========================================================================
// Cluster H — Sequence/Montage advanced
// ===========================================================================

FSageToolDispatch::FOutcome SetSequenceAdditiveImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, AdditiveTypeRaw;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("additive_type"), AdditiveTypeRaw))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'additive_type'"));
    }

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequence"));
    }

    auto ParseAdditiveType = [](const FString& Raw, EAdditiveAnimationType& Out) -> bool
    {
        if (Raw.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("AAT_None"), ESearchCase::IgnoreCase))
        {
            Out = AAT_None;
            return true;
        }
        if (Raw.Equals(TEXT("LocalSpaceBase"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("LocalSpace"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("AAT_LocalSpaceBase"), ESearchCase::IgnoreCase))
        {
            Out = AAT_LocalSpaceBase;
            return true;
        }
        if (Raw.Equals(TEXT("MeshSpaceBase"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("MeshSpace"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("RotationOffsetMeshSpace"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("AAT_RotationOffsetMeshSpace"), ESearchCase::IgnoreCase))
        {
            Out = AAT_RotationOffsetMeshSpace;
            return true;
        }
        return false;
    };
    auto ParseBasePoseType = [](const FString& Raw, EAdditiveBasePoseType& Out) -> bool
    {
        if (Raw.IsEmpty()) { return true; }
        if (Raw.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("ABPT_None"), ESearchCase::IgnoreCase))
        {
            Out = ABPT_None;
            return true;
        }
        if (Raw.Equals(TEXT("RefPose"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("ReferencePose"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("SkeletonReferencePose"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("ABPT_RefPose"), ESearchCase::IgnoreCase))
        {
            Out = ABPT_RefPose;
            return true;
        }
        if (Raw.Equals(TEXT("AnimScaled"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("ABPT_AnimScaled"), ESearchCase::IgnoreCase))
        {
            Out = ABPT_AnimScaled;
            return true;
        }
        if (Raw.Equals(TEXT("AnimFrame"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("ABPT_AnimFrame"), ESearchCase::IgnoreCase))
        {
            Out = ABPT_AnimFrame;
            return true;
        }
        if (Raw.Equals(TEXT("LocalAnimFrame"), ESearchCase::IgnoreCase) || Raw.Equals(TEXT("ABPT_LocalAnimFrame"), ESearchCase::IgnoreCase))
        {
            Out = ABPT_LocalAnimFrame;
            return true;
        }
        return false;
    };

    EAdditiveAnimationType AdditiveType = AAT_None;
    if (!ParseAdditiveType(AdditiveTypeRaw, AdditiveType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid additive_type: %s"), *AdditiveTypeRaw));
    }
    FString BasePoseTypeRaw;
    Args->TryGetStringField(TEXT("base_pose_type"), BasePoseTypeRaw);
    EAdditiveBasePoseType BasePoseType = Seq->RefPoseType;
    if (!ParseBasePoseType(BasePoseTypeRaw, BasePoseType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid base_pose_type: %s"), *BasePoseTypeRaw));
    }
    FString BaseAnimationPath;
    Args->TryGetStringField(TEXT("base_animation"), BaseAnimationPath);
    if (BaseAnimationPath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("base_sequence"), BaseAnimationPath);
    }
    UAnimSequence* BaseSequence = nullptr;
    if (!BaseAnimationPath.IsEmpty())
    {
        BaseSequence = Cast<UAnimSequence>(ResolveAsset(BaseAnimationPath));
        if (!BaseSequence)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("base_animation is not a UAnimSequence: %s"), *BaseAnimationPath));
        }
    }
    double RefFrame = Seq->RefFrameIndex;
    Args->TryGetNumberField(TEXT("base_frame"), RefFrame);
    Args->TryGetNumberField(TEXT("ref_frame_index"), RefFrame);

    FScopedTransaction Tx(LOCTEXT("SageSetSequenceAdditive", "Sage: Set Sequence Additive Settings"));
    Seq->Modify();
    Seq->AdditiveAnimType = AdditiveType;
    Seq->RefPoseType = BasePoseType;
    Seq->RefPoseSeq = BaseSequence;
    Seq->RefFrameIndex = FMath::Max(0, static_cast<int32>(RefFrame));
    Seq->RefreshCacheData();
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("additive_type"), StaticEnum<EAdditiveAnimationType>()->GetNameStringByValue(Seq->AdditiveAnimType));
    R->SetStringField(TEXT("base_pose_type"), StaticEnum<EAdditiveBasePoseType>()->GetNameStringByValue(Seq->RefPoseType));
    R->SetStringField(TEXT("base_animation"), Seq->RefPoseSeq ? Seq->RefPoseSeq->GetPathName() : FString());
    R->SetNumberField(TEXT("ref_frame_index"), Seq->RefFrameIndex);
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetSequenceCompressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, SettingsPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    Args->TryGetStringField(TEXT("scheme_path"), SettingsPath);
    if (SettingsPath.IsEmpty()) Args->TryGetStringField(TEXT("settings"), SettingsPath);
    if (SettingsPath.IsEmpty()) Args->TryGetStringField(TEXT("bone_compression_settings"), SettingsPath);

    UAnimSequence* Seq = Cast<UAnimSequence>(ResolveAsset(Path));
    if (!Seq)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimSequence"));
    }

    UObject* Settings = nullptr;
    FString Error;
    if (!ResolveExpectedObject(SettingsPath, TEXT("/Script/Engine.AnimBoneCompressionSettings"), Settings, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    const FString Before = GetObjectPathProperty(Seq, TEXT("BoneCompressionSettings"));
    FScopedTransaction Tx(LOCTEXT("SageSetSequenceCompression", "Sage: Set Sequence Compression"));
    Seq->Modify();
    if (!SetReflectedProperty(Seq, TEXT("BoneCompressionSettings"), Settings ? JsonStringValue(Settings->GetPathName()) : MakeShared<FJsonValueNull>()))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to set BoneCompressionSettings"));
    }
    Seq->RefreshCacheData();
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("bone_compression_settings_before"), Before);
    R->SetStringField(TEXT("bone_compression_settings"), GetObjectPathProperty(Seq, TEXT("BoneCompressionSettings")));
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddMontageBranchingPointImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, BranchName;
    double Time = 0.0;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("branch_name"), BranchName) || BranchName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'branch_name'"));
    }
    Args->TryGetNumberField(TEXT("time"), Time);
    int32 TrackIndex = 0;
    int32 SlotIndex = 0;
    Args->TryGetNumberField(TEXT("track_index"), TrackIndex);
    Args->TryGetNumberField(TEXT("slot_index"), SlotIndex);

    UAnimMontage* M = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!M)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimMontage"));
    }
    const float ClampedTime = FMath::Clamp(static_cast<float>(Time), 0.0f, M->GetPlayLength());

    FScopedTransaction Tx(LOCTEXT("SageAddMontageBranchingPoint", "Sage: Add Montage Branching Point"));
    M->Modify();
    FAnimNotifyEvent NewEvent;
    NewEvent.NotifyName = FName(*BranchName);
    NewEvent.MontageTickType = EMontageNotifyTickType::BranchingPoint;
    NewEvent.TrackIndex = TrackIndex;
    NewEvent.Link(M, ClampedTime, SlotIndex);
    NewEvent.SetTime(ClampedTime);
    NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(M->CalculateOffsetForNotify(ClampedTime));
    M->Notifies.Add(NewEvent);
    M->RefreshCacheData();
    M->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), M->GetPathName());
    R->SetStringField(TEXT("branch_name"), BranchName);
    R->SetNumberField(TEXT("time"), ClampedTime);
    R->SetNumberField(TEXT("index"), M->Notifies.Num() - 1);
    R->SetBoolField(TEXT("branching_point"), M->Notifies.Last().IsBranchingPoint());
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetMontageBlendCurveImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, Direction, CurvePath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    Args->TryGetStringField(TEXT("blend_in_or_out"), Direction);
    if (Direction.IsEmpty()) Args->TryGetStringField(TEXT("direction"), Direction);
    Args->TryGetStringField(TEXT("curve_path"), CurvePath);
    if (CurvePath.IsEmpty()) Args->TryGetStringField(TEXT("curve"), CurvePath);

    UAnimMontage* M = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!M)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimMontage"));
    }
    UCurveFloat* Curve = nullptr;
    if (!CurvePath.IsEmpty())
    {
        Curve = Cast<UCurveFloat>(ResolveAsset(CurvePath));
        if (!Curve)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("not a UCurveFloat: %s"), *CurvePath));
        }
    }

    const bool bBlendIn = Direction.Equals(TEXT("in"), ESearchCase::IgnoreCase)
        || Direction.Equals(TEXT("blend_in"), ESearchCase::IgnoreCase)
        || Direction.Equals(TEXT("BlendIn"), ESearchCase::CaseSensitive);
    const bool bBlendOut = Direction.Equals(TEXT("out"), ESearchCase::IgnoreCase)
        || Direction.Equals(TEXT("blend_out"), ESearchCase::IgnoreCase)
        || Direction.Equals(TEXT("BlendOut"), ESearchCase::CaseSensitive);
    if (!bBlendIn && !bBlendOut)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blend_in_or_out must be 'in' or 'out'"));
    }

    FAlphaBlend& Blend = bBlendIn ? M->BlendIn : M->BlendOut;
    FScopedTransaction Tx(LOCTEXT("SageSetMontageBlendCurve", "Sage: Set Montage Blend Curve"));
    M->Modify();
    Blend.SetCustomCurve(Curve);
    if (Curve)
    {
        Blend.SetBlendOption(EAlphaBlendOption::Custom);
    }
    double BlendTime = Blend.GetBlendTime();
    if (Args->TryGetNumberField(TEXT("blend_time"), BlendTime))
    {
        Blend.SetBlendTime(FMath::Max(0.0f, static_cast<float>(BlendTime)));
    }
    M->RefreshCacheData();
    M->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), M->GetPathName());
    R->SetStringField(TEXT("blend_in_or_out"), bBlendIn ? TEXT("in") : TEXT("out"));
    R->SetStringField(TEXT("curve_path"), Curve ? Curve->GetPathName() : FString());
    R->SetNumberField(TEXT("blend_time"), Blend.GetBlendTime());
    R->SetNumberField(TEXT("blend_option"), static_cast<int32>(Blend.GetBlendOption()));
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetMontageSectionLoopImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, Section;
    bool bLoop = true;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("section_name"), Section)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'section_name'"));
    Args->TryGetBoolField(TEXT("loop"), bLoop);
    UAnimMontage* M = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimMontage"));

    FScopedTransaction Tx(LOCTEXT("SetSecLoop", "Sage: Set Section Loop"));
    M->Modify();
    bool bFound = false;
    for (FCompositeSection& S : M->CompositeSections)
    {
        if (S.SectionName == FName(*Section))
        {
            S.NextSectionName = bLoop ? S.SectionName : NAME_None;
            bFound = true;
            break;
        }
    }
    M->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("section_name"), Section);
    R->SetBoolField(TEXT("loop"), bLoop);
    if (!bFound)
    {
        // Surface the missing section explicitly — silent success was the
        // pattern that hid the typo'd "Default" → "Defualt" bug in dogfooding.
        R->SetStringField(TEXT("_warning"),
            FString::Printf(TEXT("no section named '%s'"), *Section));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetMontageSectionNextImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, Section, Next;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("section_name"), Section)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'section_name'"));
    Args->TryGetStringField(TEXT("next_section_name"), Next);
    UAnimMontage* M = Cast<UAnimMontage>(ResolveAsset(Path));
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a UAnimMontage"));

    FScopedTransaction Tx(LOCTEXT("SetSecNext", "Sage: Set Section Next"));
    M->Modify();
    bool bFound = false;
    for (FCompositeSection& S : M->CompositeSections)
    {
        if (S.SectionName == FName(*Section))
        {
            S.NextSectionName = Next.IsEmpty() ? NAME_None : FName(*Next);
            bFound = true;
            break;
        }
    }
    M->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("section_name"), Section);
    R->SetStringField(TEXT("next_section_name"), Next);
    if (!bFound)
    {
        R->SetStringField(TEXT("_warning"),
            FString::Printf(TEXT("no section named '%s'"), *Section));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CopyAnimationCurvesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SourcePath, DestPath;
    if (!Args.IsValid() ||
        (!Args->TryGetStringField(TEXT("source_sequence"), SourcePath) &&
         !Args->TryGetStringField(TEXT("from_sequence"), SourcePath)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'source_sequence'"));
    }
    if (!Args->TryGetStringField(TEXT("destination_sequence"), DestPath) &&
        !Args->TryGetStringField(TEXT("to_sequence"), DestPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'destination_sequence'"));
    }

    UAnimSequence* Source = Cast<UAnimSequence>(ResolveAsset(SourcePath));
    UAnimSequence* Dest = Cast<UAnimSequence>(ResolveAsset(DestPath));
    if (!Source)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a source UAnimSequence: %s"), *SourcePath));
    }
    if (!Dest)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a destination UAnimSequence: %s"), *DestPath));
    }
    const IAnimationDataModel* SourceModel = Source->GetDataModel();
    const IAnimationDataModel* DestModel = Dest->GetDataModel();
    if (!SourceModel || !DestModel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("source or destination sequence has no data model"));
    }

    bool bDryRun = false;
    bool bOverwrite = true;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    TArray<FString> RequestedNames;
    ReadStringArrayField(Args, TEXT("curve_names"), RequestedNames);
    TSet<FName> Filter;
    for (const FString& Name : RequestedNames)
    {
        if (!Name.IsEmpty()) Filter.Add(FName(*Name));
    }

    TSet<FName> ExistingFloat;
    TSet<FName> ExistingTransform;
    for (const FFloatCurve& Curve : DestModel->GetFloatCurves())
    {
        ExistingFloat.Add(Curve.GetName());
    }
    for (const FTransformCurve& Curve : DestModel->GetTransformCurves())
    {
        ExistingTransform.Add(Curve.GetName());
    }

    TArray<TSharedPtr<FJsonValue>> CopiedFloat;
    TArray<TSharedPtr<FJsonValue>> CopiedTransform;
    TArray<TSharedPtr<FJsonValue>> Conflicts;
    auto AddNameValue = [](TArray<TSharedPtr<FJsonValue>>& Arr, const FName& Name)
    {
        Arr.Add(MakeShared<FJsonValueString>(Name.ToString()));
    };
    auto AddConflict = [&Conflicts](const FName& Name, const FString& Type)
    {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name.ToString());
        Obj->SetStringField(TEXT("type"), Type);
        Obj->SetStringField(TEXT("reason"), TEXT("exists_and_overwrite_false"));
        Conflicts.Add(MakeShared<FJsonValueObject>(Obj));
    };

    IAnimationDataController* ControllerPtr = bDryRun ? nullptr : &Dest->GetController();
    TUniquePtr<IAnimationDataController::FScopedBracket> Bracket;
    if (ControllerPtr)
    {
        Dest->Modify();
        Bracket = MakeUnique<IAnimationDataController::FScopedBracket>(
            ControllerPtr, LOCTEXT("SageCopyAnimationCurves", "Sage: Copy Animation Curves"));
    }

    for (const FFloatCurve& Curve : SourceModel->GetFloatCurves())
    {
        const FName CurveName = Curve.GetName();
        if (!HasCurveNameFilter(Filter, CurveName)) continue;

        const bool bExists = ExistingFloat.Contains(CurveName);
        if (bExists && !bOverwrite)
        {
            AddConflict(CurveName, TEXT("float"));
            continue;
        }
        if (!bDryRun)
        {
            const FAnimationCurveIdentifier Id(CurveName, ERawCurveTrackTypes::RCT_Float);
            if (bExists)
            {
                ControllerPtr->RemoveCurve(Id, false);
            }
            ControllerPtr->AddCurve(Id, Curve.GetCurveTypeFlags(), false);
            ControllerPtr->SetCurveKeys(Id, Curve.FloatCurve.GetConstRefOfKeys(), false);
        }
        AddNameValue(CopiedFloat, CurveName);
    }

    for (const FTransformCurve& Curve : SourceModel->GetTransformCurves())
    {
        const FName CurveName = Curve.GetName();
        if (!HasCurveNameFilter(Filter, CurveName)) continue;

        const bool bExists = ExistingTransform.Contains(CurveName);
        if (bExists && !bOverwrite)
        {
            AddConflict(CurveName, TEXT("transform"));
            continue;
        }
        if (!bDryRun)
        {
            TArray<float> Times;
            TArray<FTransform> Values;
            Curve.GetKeys(Times, Values);
            const FAnimationCurveIdentifier Id(CurveName, ERawCurveTrackTypes::RCT_Transform);
            if (bExists)
            {
                ControllerPtr->RemoveCurve(Id, false);
            }
            ControllerPtr->AddCurve(Id, Curve.GetCurveTypeFlags(), false);
            ControllerPtr->SetTransformCurveKeys(Id, Values, Times, false);
        }
        AddNameValue(CopiedTransform, CurveName);
    }

    Bracket.Reset();
    if (!bDryRun)
    {
        Dest->RefreshCacheData();
        Dest->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("source_sequence"), Source->GetPathName());
    R->SetStringField(TEXT("destination_sequence"), Dest->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("overwrite"), bOverwrite);
    R->SetBoolField(TEXT("modified"), !bDryRun && (CopiedFloat.Num() + CopiedTransform.Num()) > 0);
    R->SetArrayField(TEXT("copied_float_curves"), CopiedFloat);
    R->SetArrayField(TEXT("copied_transform_curves"), CopiedTransform);
    R->SetArrayField(TEXT("conflicts"), Conflicts);
    R->SetNumberField(TEXT("copied_float_curve_count"), CopiedFloat.Num());
    R->SetNumberField(TEXT("copied_transform_curve_count"), CopiedTransform.Num());
    R->SetNumberField(TEXT("conflict_count"), Conflicts.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ===========================================================================
// Cluster I — Skeleton authoring
// ===========================================================================

FSageToolDispatch::FOutcome AddSkeletonSocketImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, SocketName, ParentBone;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("socket_name"), SocketName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'socket_name'"));
    if (!Args->TryGetStringField(TEXT("parent_bone"), ParentBone)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'parent_bone'"));
    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(Path));
    if (!Skel) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a USkeleton"));

    // Validate parent bone exists — silently accepting an invalid bone leaves
    // the socket orphaned (BoneName points nowhere) and editor crashes when
    // selecting the socket in Persona.
    const FName ParentBoneName(*ParentBone);
    if (Skel->GetReferenceSkeleton().FindBoneIndex(ParentBoneName) == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("bone '%s' not found in skeleton ref hierarchy"), *ParentBone));
    }

    // Reject duplicate socket name (USkeletalMeshSocket lookup is by name —
    // duplicates make Persona's socket-list ambiguous).
    const FName SocketFName(*SocketName);
    for (USkeletalMeshSocket* Existing : Skel->Sockets)
    {
        if (Existing && Existing->SocketName == SocketFName)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("socket '%s' already exists"), *SocketName));
        }
    }

    FScopedTransaction Tx(LOCTEXT("AddSkSocket", "Sage: Add Skeleton Socket"));
    Skel->Modify();
    USkeletalMeshSocket* Sock = NewObject<USkeletalMeshSocket>(Skel);
    Sock->SocketName = SocketFName;
    Sock->BoneName   = ParentBoneName;

    // Optional transform — schema accepts {location:[x,y,z], rotation:[p,y,r],
    // scale:[x,y,z]}. Fill USkeletalMeshSocket::Relative* fields. Without
    // these, sockets default to identity which is rarely what callers want.
    const TSharedPtr<FJsonObject>* TransformObj = nullptr;
    if (Args->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj && (*TransformObj).IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
        if ((*TransformObj)->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
        {
            Sock->RelativeLocation = FVector(
                (*LocArr)[0]->AsNumber(),
                (*LocArr)[1]->AsNumber(),
                (*LocArr)[2]->AsNumber());
        }
        const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
        if ((*TransformObj)->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
        {
            // Schema [pitch, yaw, roll] → FRotator(P, Y, R).
            Sock->RelativeRotation = FRotator(
                (*RotArr)[0]->AsNumber(),
                (*RotArr)[1]->AsNumber(),
                (*RotArr)[2]->AsNumber());
        }
        const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
        if ((*TransformObj)->TryGetArrayField(TEXT("scale"), ScaleArr) && ScaleArr && ScaleArr->Num() >= 3)
        {
            Sock->RelativeScale = FVector(
                (*ScaleArr)[0]->AsNumber(),
                (*ScaleArr)[1]->AsNumber(),
                (*ScaleArr)[2]->AsNumber());
        }
    }

    Skel->Sockets.Add(Sock);
    Skel->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("socket_name"), SocketName);
    R->SetStringField(TEXT("parent_bone"), ParentBone);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RemoveSkeletonSocketImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, SocketName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("socket_name"), SocketName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'socket_name'"));
    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(Path));
    if (!Skel) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a USkeleton"));

    FScopedTransaction Tx(LOCTEXT("RemoveSkSocket", "Sage: Remove Skeleton Socket"));
    Skel->Modify();
    int32 RemovedCount = 0;
    for (int32 i = Skel->Sockets.Num() - 1; i >= 0; --i)
    {
        if (Skel->Sockets[i] && Skel->Sockets[i]->SocketName == FName(*SocketName))
        {
            Skel->Sockets.RemoveAt(i);
            ++RemovedCount;
        }
    }
    Skel->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("socket_name"),   SocketName);
    R->SetBoolField  (TEXT("removed"),       RemovedCount > 0);
    // removed_count is more useful when duplicates ever existed (e.g. legacy
    // skeleton imported from before AddSkeletonSocketImpl's dup-name guard).
    R->SetNumberField(TEXT("removed_count"), RemovedCount);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddSlotImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, SlotName, Group = TEXT("DefaultGroup");
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("slot_name"), SlotName)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'slot_name'"));
    Args->TryGetStringField(TEXT("group_name"), Group);
    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(Path));
    if (!Skel) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a USkeleton"));

    FScopedTransaction Tx(LOCTEXT("AddSlot", "Sage: Add Anim Slot"));
    Skel->Modify();
    // Engine USkeleton requires the slot to be in the registered slot list
    // BEFORE SetSlotGroupName re-binds it to a group. Calling SetSlotGroupName
    // alone on an unregistered slot is a silent no-op in some 5.7 paths
    // (depending on whether the slot already lives in another group).
    // RegisterSlotNode is idempotent — returns true on first call, false
    // when already registered, never throws.
    const FName SlotFName(*SlotName);
    const bool bRegistered = Skel->RegisterSlotNode(SlotFName);
    Skel->SetSlotGroupName(SlotFName, FName(*Group));
    Skel->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("slot_name"),  SlotName);
    R->SetStringField(TEXT("group"),      Group);
    R->SetBoolField  (TEXT("registered"), bRegistered);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddSlotGroupImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FString Path, Group;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("group_name"), Group)) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'group_name'"));
    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(Path));
    if (!Skel) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("not a USkeleton"));

    FScopedTransaction Tx(LOCTEXT("AddSlotGroup", "Sage: Add Slot Group"));
    Skel->Modify();
    Skel->AddSlotGroupName(FName(*Group));
    Skel->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("group_name"), Group);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetBoneRetargetingImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SkeletonPath, BoneName, ModeString;
    if (!Args.IsValid() ||
        (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath) &&
         !Args->TryGetStringField(TEXT("path"), SkeletonPath)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    if (!Args->TryGetStringField(TEXT("bone"), BoneName) || BoneName.IsEmpty())
    {
        Args->TryGetStringField(TEXT("bone_name"), BoneName);
    }
    if (BoneName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'bone'"));
    }
    if (!Args->TryGetStringField(TEXT("mode"), ModeString) || ModeString.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'mode'"));
    }

    EBoneTranslationRetargetingMode::Type Mode = EBoneTranslationRetargetingMode::Animation;
    if (!ParseRetargetMode(ModeString, Mode))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid retargeting mode: %s"), *ModeString));
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *SkeletonPath));
    }
    const int32 BoneIndex = Skel->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName));
    if (BoneIndex == INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("bone '%s' not found in skeleton"), *BoneName));
    }

    bool bChildrenToo = false;
    Args->TryGetBoolField(TEXT("children"), bChildrenToo);
    Args->TryGetBoolField(TEXT("children_too"), bChildrenToo);

    FScopedTransaction Tx(LOCTEXT("SageSetBoneRetargeting", "Sage: Set Bone Retargeting"));
    Skel->Modify();
    Skel->SetBoneTranslationRetargetingMode(BoneIndex, Mode, bChildrenToo);
    Skel->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Skel->GetPathName());
    R->SetStringField(TEXT("bone"), BoneName);
    R->SetNumberField(TEXT("bone_index"), BoneIndex);
    R->SetStringField(TEXT("mode"), RetargetModeToString(Skel->GetBoneTranslationRetargetingMode(BoneIndex)));
    R->SetBoolField(TEXT("children"), bChildrenToo);
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddSkeletonCurveMetadataImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SkeletonPath, CurveName, Type;
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    Args->TryGetStringField(TEXT("path"), SkeletonPath);
    if (SkeletonPath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("skeleton"), SkeletonPath);
    }
    if (SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'curve_name'"));
    }
    if (!Args->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
    {
        Type = TEXT("attribute");
    }

    USkeleton* Skel = Cast<USkeleton>(ResolveAsset(SkeletonPath));
    if (!Skel)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a USkeleton: %s"), *SkeletonPath));
    }

    bool bMaterial = false;
    bool bMorphTarget = false;
    if (Type.Equals(TEXT("material"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("material_curve"), ESearchCase::IgnoreCase))
    {
        bMaterial = true;
    }
    else if (Type.Equals(TEXT("morph"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("morph_target"), ESearchCase::IgnoreCase)
        || Type.Equals(TEXT("morphtarget"), ESearchCase::IgnoreCase))
    {
        bMorphTarget = true;
    }
    else if (Type.Equals(TEXT("both"), ESearchCase::IgnoreCase))
    {
        bMaterial = true;
        bMorphTarget = true;
    }
    else if (!Type.Equals(TEXT("attribute"), ESearchCase::IgnoreCase) && !Type.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid curve metadata type: %s"), *Type));
    }
    Args->TryGetBoolField(TEXT("material"), bMaterial);
    Args->TryGetBoolField(TEXT("morph_target"), bMorphTarget);

    const FName Name(*CurveName);
    const bool bAlready = Skel->GetCurveMetaData(Name) != nullptr;
    FScopedTransaction Tx(LOCTEXT("SageAddSkeletonCurveMetadata", "Sage: Add Skeleton Curve Metadata"));
    Skel->Modify();
    const bool bAdded = bAlready || Skel->AddCurveMetaData(Name, false);
    if (!bAdded)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("failed to add curve metadata"));
    }
    Skel->SetCurveMetaDataMaterial(Name, bMaterial);
    Skel->SetCurveMetaDataMorphTarget(Name, bMorphTarget);
    Skel->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Skel->GetPathName());
    R->SetStringField(TEXT("curve_name"), CurveName);
    R->SetStringField(TEXT("type"), Type);
    R->SetBoolField(TEXT("already"), bAlready);
    R->SetBoolField(TEXT("added"), !bAlready);
    R->SetBoolField(TEXT("material"), Skel->GetCurveMetaDataMaterial(Name));
    R->SetBoolField(TEXT("morph_target"), Skel->GetCurveMetaDataMorphTarget(Name));
    R->SetNumberField(TEXT("metadata_count"), Skel->GetNumCurveMetaData());
    R->SetBoolField(TEXT("modified"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---------------------------------------------------------------------------
// character.set_morph_target
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome CharacterSetMorphTargetImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!GetPieWorldOrNull())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("requires PIE world"));
    }
    FString ActorPath, TargetName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor"), ActorPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));
    }
    if (!Args->TryGetStringField(TEXT("target_name"), TargetName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'target_name'"));
    }
    double Value = 0.0;
    Args->TryGetNumberField(TEXT("value"), Value);

    AActor* Actor = ResolveActorRuntime(ActorPath);
    USkeletalMeshComponent* Mesh = FindSkeletalMeshComp(Actor);
    if (!Mesh)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("actor has no SkeletalMeshComponent"));
    }
    Mesh->SetMorphTarget(FName(*TargetName), static_cast<float>(Value));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"),       Actor->GetPathName());
    R->SetStringField(TEXT("target_name"), TargetName);
    R->SetNumberField(TEXT("value"),       Value);
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
    Dispatch.RegisterHandler(TEXT("animation.inspect_skeleton"),           GT(&InspectSkeletonImpl));
    Dispatch.RegisterHandler(TEXT("animation.inspect_ref_pose"),           GT(&InspectRefPoseImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_skeletons"),             GT(&ListSkeletonsImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_blend_profiles"),        GT(&ReadBlendProfilesImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_sockets"),               GT(&ListSkeletonSocketsImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_skeletal_meshes"),       GT(&ListSkeletalMeshesImpl));
    Dispatch.RegisterHandler(TEXT("animation.get_physics_asset"),          GT(&GetPhysicsAssetImpl));
    Dispatch.RegisterHandler(TEXT("animation.find_animations"),            GT(&FindAnimationsImpl));
    Dispatch.RegisterHandler(TEXT("animation.inspect_animation"),          GT(&InspectAnimationImpl));
    Dispatch.RegisterHandler(TEXT("animation.sample_bone_tracks"),         GT(&SampleBoneTracksImpl));
    Dispatch.RegisterHandler(TEXT("animation.compare_retarget_bones"),     GT(&CompareRetargetBonesImpl));
    Dispatch.RegisterHandler(TEXT("animation.diagnose_retarget_animation"),GT(&DiagnoseRetargetAnimationImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_state_machine"),         GT(&ReadStateMachineImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_anim_graph"),            GT(&ReadAnimGraphImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_state_graph"),           GT(&ReadStateGraphImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_modifiers"),             GT(&ListModifiersImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_bone_track"),            GT(&ReadBoneTrackImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_animation_curves"),       GT(&ReadAnimationCurvesImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_control_rig_variables"), GT(&ListControlRigVariablesImpl));
    Dispatch.RegisterHandler(TEXT("controlrig.read"),                      GT(&ControlRigReadImpl));
    Dispatch.RegisterHandler(TEXT("controlrig.list_controls"),             GT(&ControlRigListControlsImpl));
    Dispatch.RegisterHandler(TEXT("controlrig.set_preview_mesh"),          GT(&ControlRigSetPreviewMeshImpl));
    Dispatch.RegisterHandler(TEXT("controlrig.set_control_transform"),     GT(&ControlRigSetControlTransformImpl));
    Dispatch.RegisterHandler(TEXT("controlrig.add_control"),               GT(&ControlRigAddControlImpl));
    Dispatch.RegisterHandler(TEXT("controlrig.remove_control"),            GT(&ControlRigRemoveControlImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_pose_search_database"),  GT(&ReadPoseSearchDatabaseImpl));

    // Write tools
    Dispatch.RegisterHandler(TEXT("animation.create_anim_blueprint"),      GT(&CreateAnimBlueprintImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_montage"),             GT(&CreateMontageImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_blendspace"),          GT(&CreateBlendSpaceImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_notify"),                 GT(&AddNotifyImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_sequence"),            GT(&CreateSequenceImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_bone_keyframes"),         GT(&SetBoneKeyframesImpl));
    Dispatch.RegisterHandler(TEXT("animation.get_bone_transforms"),        GT(&GetBoneTransformsImpl));
    Dispatch.RegisterHandler(TEXT("animation.copy_bone_tracks"),           GT(&CopyBoneTracksImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_sequence"),       GT(&SetMontageSequenceImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_properties"),     GT(&SetMontagePropertiesImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_state_machine"),       GT(&CreateStateMachineImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_state"),                  GT(&AddStateImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_transition"),             GT(&AddTransitionImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_state_animation"),        GT(&SetStateAnimationImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_transition_blend"),       GT(&SetTransitionBlendImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_transition_automatic_rule"), GT(&SetTransitionAutomaticRuleImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_curve"),                  GT(&AddCurveImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_slot"),           GT(&SetMontageSlotImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_montage_section"),        GT(&AddMontageSectionImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_ik_rig"),              GT(&CreateIKRigImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_ik_rig"),                GT(&ReadIKRigImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_rig_skeletal_mesh"),   GT(&SetIKRigSkeletalMeshImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_ik_retarget_chain"),      GT(&AddIKRetargetChainImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_ik_retarget_chain"),   GT(&RemoveIKRetargetChainImpl));
    Dispatch.RegisterHandler(TEXT("animation.rename_ik_retarget_chain"),   GT(&RenameIKRetargetChainImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retarget_chain_bones"),GT(&SetIKRetargetChainBonesImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retarget_chain_goal"), GT(&SetIKRetargetChainGoalImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retarget_root"),       GT(&SetIKRetargetRootImpl));
    Dispatch.RegisterHandler(TEXT("animation.auto_generate_ik_retarget_definition"), GT(&AutoGenerateIKRetargetDefinitionImpl));
    Dispatch.RegisterHandler(TEXT("animation.auto_generate_ik_fbik"),      GT(&AutoGenerateIKFBIKImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_root_motion"),            GT(&SetRootMotionImpl));
    Dispatch.RegisterHandler(TEXT("animation.save_animation_asset"),       GT(&SaveAnimationAssetImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_skeleton_bone"),          GT(&AddSkeletonBoneImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_blend_mask"),          GT(&CreateBlendMaskImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_virtual_bone"),           GT(&AddVirtualBoneImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_virtual_bone"),        GT(&RemoveVirtualBoneImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_composite"),           GT(&CreateCompositeImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_ik_retargeter"),       GT(&CreateIKRetargeterImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_ik_retargeter"),         GT(&ReadIKRetargeterImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retargeter_rigs"),     GT(&SetIKRetargeterRigsImpl));
    Dispatch.RegisterHandler(TEXT("animation.setup_ik_retargeter_ops"),    GT(&SetupIKRetargeterOpsImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_ik_retargeter_op"),       GT(&AddIKRetargeterOpImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_ik_retargeter_op"),    GT(&RemoveIKRetargeterOpImpl));
    Dispatch.RegisterHandler(TEXT("animation.move_ik_retargeter_op"),      GT(&MoveIKRetargeterOpImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retargeter_op_enabled"), GT(&SetIKRetargeterOpEnabledImpl));
    Dispatch.RegisterHandler(TEXT("animation.auto_map_ik_retargeter_chains"), GT(&AutoMapIKRetargeterChainsImpl));
    Dispatch.RegisterHandler(TEXT("animation.reset_ik_retargeter_chain_settings"), GT(&ResetIKRetargeterChainSettingsImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retargeter_ik_chain_settings"), GT(&SetIKRetargeterIKChainSettingsImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retargeter_fk_chain_settings"), GT(&SetIKRetargeterFKChainSettingsImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retargeter_chain_mapping"), GT(&SetIKRetargeterChainMappingImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_ik_retargeter_pose"),     GT(&SetIKRetargeterPoseImpl));
    Dispatch.RegisterHandler(TEXT("animation.retarget_animations"),        GT(&RetargetAnimationsImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_anim_blueprint_skeleton"),GT(&SetAnimBlueprintSkeletonImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_animation_asset_skeleton"),GT(&SetAnimationAssetSkeletonImpl));
    Dispatch.RegisterHandler(TEXT("animation.bake_root_motion_from_bone"), GT(&BakeRootMotionFromBoneImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_pose_search_database"),GT(&CreatePoseSearchDatabaseImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_pose_search_schema"),     GT(&SetPoseSearchSchemaImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_pose_search_sequence"),   GT(&AddPoseSearchSequenceImpl));
    Dispatch.RegisterHandler(TEXT("animation.build_pose_search_index"),    GT(&BuildPoseSearchIndexImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_sequence_properties"),    GT(&SetSequencePropertiesImpl));
    // Phase 4-r6 (Lyra Sage Gap #17/#18 — new tools)
    Dispatch.RegisterHandler(TEXT("animation.create_anim_notify"),         GT(&CreateAnimNotifyImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_anim_notify_state"),   GT(&CreateAnimNotifyStateImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_blendspace_sample"),      GT(&AddBlendSpaceSampleImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_blendspace_samples"),     GT(&SetBlendSpaceSamplesImpl));
    // Phase 4-r6 Cluster A (AnimGraph node creation core)
    Dispatch.RegisterHandler(TEXT("animation.add_animgraph_node"),         GT(&AddAnimGraphNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_animgraph_node"),      GT(&RemoveAnimGraphNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_state_machine_reference_node"), GT(&RemoveStateMachineReferenceNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.connect_pose_pin"),           GT(&ConnectPosePinImpl));
    Dispatch.RegisterHandler(TEXT("animation.disconnect_pose_pin"),        GT(&DisconnectPosePinImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_anim_node_property"),     GT(&SetAnimNodePropertyImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_layered_bone_blend_config"), GT(&SetLayeredBoneBlendConfigImpl));
    Dispatch.RegisterHandler(TEXT("animation.bind_anim_node_property"),    GT(&BindAnimNodePropertyImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_anim_node_properties"),  GT(&ReadAnimNodePropertiesImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_retarget_pose_from_mesh_node"), GT(&AddRetargetPoseFromMeshNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_retarget_pose_from_mesh_node"), GT(&SetRetargetPoseFromMeshNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_retarget_pose_from_mesh_node"), GT(&ReadRetargetPoseFromMeshNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_retarget_profile"),      GT(&ReadRetargetProfileImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_retarget_profile"),       GT(&SetRetargetProfileImpl));
    Dispatch.RegisterHandler(TEXT("animation.copy_retarget_profile_from_asset"), GT(&CopyRetargetProfileFromAssetImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_owner_locomotion_update"),GT(&SetOwnerLocomotionUpdateImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_animgraph_nodes"),       GT(&ListAnimGraphNodesImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_animgraph_root_pose"),    GT(&SetAnimGraphRootPoseImpl));
    // Phase 4-r6 Cluster B (AnimGraph convenience nodes)
    Dispatch.RegisterHandler(TEXT("animation.add_sequence_player"),        GT(&AddSequencePlayerImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_blendspace_player"),      GT(&AddBlendSpacePlayerImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_state_machine_node"),     GT(&AddStateMachineNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_blend_list_by_int"),      GT(&AddBlendListByIntImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_blend_list_by_bool"),     GT(&AddBlendListByBoolImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_blend_list_by_enum"),     GT(&AddBlendListByEnumImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_blend_list_pose_pin"),    GT(&AddBlendListPosePinImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_layered_blend_per_bone"), GT(&AddLayeredBlendPerBoneImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_apply_additive"),         GT(&AddApplyAdditiveImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_two_bone_ik"),            GT(&AddTwoBoneIKImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_skeletal_control_node"),  GT(&AddSkeletalControlNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_slot_node"),              GT(&AddSlotNodeImpl));
    // animation.add_link_anim_layer removed — see Phase 4-r6 Cluster G's
    // animation.add_linked_anim_layer_node (interface + function aware spawn).
    // Phase 4-r6 Cluster J (runtime character.* — PIE-only)
    Dispatch.RegisterHandler(TEXT("character.play_root_motion_source"),    GT(&CharacterPlayRootMotionSourceImpl));
    Dispatch.RegisterHandler(TEXT("character.remove_root_motion_source"),  GT(&CharacterRemoveRootMotionSourceImpl));
    Dispatch.RegisterHandler(TEXT("character.play_montage"),               GT(&CharacterPlayMontageImpl));
    Dispatch.RegisterHandler(TEXT("character.stop_montage"),               GT(&CharacterStopMontageImpl));
    Dispatch.RegisterHandler(TEXT("character.set_anim_instance_class"),    GT(&CharacterSetAnimInstanceClassImpl));
    Dispatch.RegisterHandler(TEXT("character.list_active_montages"),       GT(&CharacterListActiveMontagesImpl));
    Dispatch.RegisterHandler(TEXT("character.set_animation_mode"),         GT(&CharacterSetAnimationModeImpl));
    Dispatch.RegisterHandler(TEXT("character.play_animation"),             GT(&CharacterPlayAnimationImpl));
    Dispatch.RegisterHandler(TEXT("character.set_morph_target"),           GT(&CharacterSetMorphTargetImpl));
    // Phase 4-r6 Cluster C ek (state machine deep CRUD)
    Dispatch.RegisterHandler(TEXT("animation.add_conduit"),                GT(&AddConduitImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_state_alias"),            GT(&AddStateAliasImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_transition_priority"),    GT(&SetTransitionPriorityImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_state_machine_initial_state"), GT(&SetStateMachineInitialStateImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_states"),                GT(&ListStatesImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_transitions"),           GT(&ListTransitionsImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_transition_rule"),        GT(&SetTransitionRuleImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_transition_rule"),       GT(&ReadTransitionRuleImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_state_entered_event"),    GT(&SetStateEnteredEventImpl));
    // Phase 4-r6 Cluster D ek (anim notify track CRUD)
    Dispatch.RegisterHandler(TEXT("animation.add_notify_track"),           GT(&AddNotifyTrackImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_notifies"),              GT(&ListNotifiesImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_notify"),              GT(&RemoveNotifyImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_notify_position"),        GT(&SetNotifyPositionImpl));
    // Phase 4-r6 Cluster E ek (blendspace axis + sample CRUD)
    Dispatch.RegisterHandler(TEXT("animation.remove_blendspace_sample"),   GT(&RemoveBlendSpaceSampleImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_blendspace_axis"),        GT(&SetBlendSpaceAxisImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_blendspace_smoothing"),   GT(&SetBlendSpaceSmoothingImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_blendspace_target_weight_interpolation"), GT(&SetBlendSpaceTargetWeightImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_blendspace_samples"),    GT(&ReadBlendSpaceSamplesImpl));
    // Phase 4-r6 Cluster F (sync markers + curve compression + modifier)
    Dispatch.RegisterHandler(TEXT("animation.add_sync_marker"),            GT(&AddSyncMarkerImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_sync_marker"),         GT(&RemoveSyncMarkerImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_sync_markers"),          GT(&ListSyncMarkersImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_curve_compression"),      GT(&SetCurveCompressionImpl));
    Dispatch.RegisterHandler(TEXT("animation.run_animation_modifier"),     GT(&RunAnimationModifierImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_animation_modifier"),     GT(&AddAnimationModifierImpl));
    // Phase 4-r6 Cluster G (Animation Layer Interface — REAL impl + Lyra Gap #24)
    Dispatch.RegisterHandler(TEXT("animation.create_anim_layer_interface"),    GT(&CreateAnimLayerInterfaceImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_layer_function"),             GT(&AddLayerFunctionImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_layer_function_input_pins"),  GT(&SetLayerFunctionInputPinsImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_layer_function_override"),    GT(&AddLayerFunctionOverrideImpl));
    Dispatch.RegisterHandler(TEXT("animation.implement_anim_layer_interface"), GT(&ImplementAnimLayerInterfaceImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_linked_anim_layer_node"),     GT(&AddLinkedAnimLayerNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_implemented_layers"),        GT(&ListImplementedLayersImpl));
    Dispatch.RegisterHandler(TEXT("animation.list_layer_functions"),           GT(&ListLayerFunctionsImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_layer_function_override"), GT(&RemoveLayerFunctionOverrideImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_layer_function"),          GT(&RemoveLayerFunctionImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_linked_layer_pattern"),    GT(&CreateLinkedLayerPatternImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_linked_anim_layer"),          GT(&SetLinkedAnimLayerImpl));
    Dispatch.RegisterHandler(TEXT("animation.repair_linked_anim_layer_nodes"), GT(&RepairLinkedAnimLayerNodesImpl));
    // Phase 4-r6 Cluster H (sequence/montage advanced)
    Dispatch.RegisterHandler(TEXT("animation.set_sequence_additive_settings"), GT(&SetSequenceAdditiveImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_sequence_compression_scheme"), GT(&SetSequenceCompressionImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_montage_branching_point"),GT(&AddMontageBranchingPointImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_blend_curve"),    GT(&SetMontageBlendCurveImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_section_loop"),   GT(&SetMontageSectionLoopImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_montage_section_next"),   GT(&SetMontageSectionNextImpl));
    Dispatch.RegisterHandler(TEXT("animation.copy_animation_curves"),      GT(&CopyAnimationCurvesImpl));
    // Phase 4-r6 Cluster I (skeleton authoring)
    Dispatch.RegisterHandler(TEXT("animation.add_skeleton_socket"),        GT(&AddSkeletonSocketImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_skeleton_socket"),     GT(&RemoveSkeletonSocketImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_slot"),                   GT(&AddSlotImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_slot_group"),             GT(&AddSlotGroupImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_bone_translation_retargeting"), GT(&SetBoneRetargetingImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_skeleton_curve_metadata"),GT(&AddSkeletonCurveMetadataImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
