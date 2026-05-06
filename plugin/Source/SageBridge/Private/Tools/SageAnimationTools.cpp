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
#include "Factories/BlueprintFactory.h"
#include "Features/IModularFeatures.h"
#include "IAssetTools.h"
#include "IPropertyAccessEditor.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

// AnimGraph + state machine authoring (Phase 4-r6, Lyra Sage Gap #16/#17/#18)
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_Base.h"
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
#include "AnimGraphNode_LinkedAnimLayer.h"  // Cluster G: master AnimGraph linked-layer call
#include "Animation/AnimNode_LinkedAnimLayer.h"  // Cluster G: inner FAnimNode_LinkedAnimLayer
#include "Animation/AnimNodeBase.h"  // FPoseLink for pose-pin category check
#include "BoneControllers/AnimNode_SkeletalControlBase.h"  // FComponentSpacePoseLink
#include "Animation/AnimNode_SequencePlayer.h"  // FAnimNode_SequencePlayer for inner Node mutation
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"  // FKismetNameValidator for RenameGraphWithSuggestion

// IAnimationDataController for AnimSequence curve add (UE 5.5+ canonical path)
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimCurveTypes.h"

// IBlueprintGeneratedClass + Anim* class hierarchy
#include "Animation/AnimBlueprintGeneratedClass.h"

// Cluster J — runtime character.* (PIE-only)
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
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

// Locate a state machine sub-graph by name. State machines are stored as
// EditorStateMachineGraph on UAnimGraphNode_StateMachineBase nodes inside the
// AnimGraph (not directly on AnimBP->FunctionGraphs).
UAnimationStateMachineGraph* FindStateMachineGraph(UAnimBlueprint* AnimBP, const FName& Name)
{
    UEdGraph* AnimGraph = FindAnimGraph(AnimBP);
    if (!AnimGraph) return nullptr;
    for (UEdGraphNode* Node : AnimGraph->Nodes)
    {
        UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
        if (SMNode && SMNode->EditorStateMachineGraph
            && SMNode->EditorStateMachineGraph->GetFName() == Name)
        {
            return Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
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

TSharedPtr<FJsonObject> PinSummaryJson(const UEdGraphPin* Pin)
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
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("skeleton"), SkeletonPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
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
            FString StateName;
            if (UAnimStateAliasNode* AsAlias = Cast<UAnimStateAliasNode>(S))
            {
                StateName = AsAlias->GetStateName();
            }
            else if (UAnimStateNode* AsState = Cast<UAnimStateNode>(S))
            {
                StateName = AsState->GetStateName();
            }
            else if (UAnimStateConduitNode* AsCon = Cast<UAnimStateConduitNode>(S))
            {
                if (AsCon->BoundGraph) StateName = AsCon->BoundGraph->GetFName().ToString();
            }
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

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), BP->GetPathName());

    TArray<TSharedPtr<FJsonValue>> Graphs;
    auto EmitGraph = [&Graphs](UEdGraph* G)
    {
        if (!G) return;
        auto GObj = MakeShared<FJsonObject>();
        GObj->SetStringField(TEXT("name"),   G->GetFName().ToString());
        GObj->SetStringField(TEXT("schema"), G->Schema ? G->Schema->GetName() : TEXT(""));

        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (UEdGraphNode* N : G->Nodes)
        {
            if (!N) continue;
            auto NObj = MakeShared<FJsonObject>();
            NObj->SetStringField(TEXT("node_id"),  N->NodeGuid.ToString(EGuidFormats::Digits));
            NObj->SetStringField(TEXT("class"),    N->GetClass()->GetPathName());
            NObj->SetNumberField(TEXT("x"),        N->NodePosX);
            NObj->SetNumberField(TEXT("y"),        N->NodePosY);
            NObj->SetNumberField(TEXT("pin_count"),N->Pins.Num());
            Nodes.Add(MakeShared<FJsonValueObject>(NObj));
        }
        GObj->SetArrayField(TEXT("nodes"), Nodes);
        GObj->SetNumberField(TEXT("node_count"), Nodes.Num());
        Graphs.Add(MakeShared<FJsonValueObject>(GObj));
    };

    if (!GraphName.IsEmpty())
    {
        EmitGraph(ResolveAnimGraphTarget(BP, GraphName));
    }
    else
    {
        // Emit every UAnimationGraphSchema-bound graph and every state-machine
        // sub-graph (via referencing UAnimGraphNode_StateMachineBase).
        for (UEdGraph* G : BP->FunctionGraphs)
        {
            if (G && G->Schema
                && G->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
            {
                EmitGraph(G);
            }
        }
    }

    R->SetArrayField(TEXT("graphs"), Graphs);
    R->SetNumberField(TEXT("graph_count"), Graphs.Num());
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
    return FSageToolDispatch::FOutcome::MakeError(-32000,
        TEXT("[NOT IMPLEMENTED] animation.set_bone_keyframes — use IAnimationDataController "
             "via Persona (UE 5.5+ canonical) or editor.run_python with animation editor"));
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
    return FSageToolDispatch::FOutcome::MakeError(-32000,
        TEXT("[NOT IMPLEMENTED] animation.set_montage_sequence — slot track editing "
             "requires Persona session; use editor.run_python with anim editor API"));
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
        if (Args->TryGetStringField(TEXT("source_ik_rig"), SourcePath) && !SourcePath.IsEmpty())
        {
            UObject* SrcRig = ResolveAsset(SourcePath);
            FObjectPropertyBase* SrcProp = CastField<FObjectPropertyBase>(
                Fac->GetClass()->FindPropertyByName(TEXT("SourceIKRigAsset")));
            if (SrcProp && SrcRig)
                SrcProp->SetObjectPropertyValue(SrcProp->ContainerPtrToValuePtr<void>(Fac), SrcRig);
        }
        if (Args->TryGetStringField(TEXT("target_ik_rig"), TargetPath) && !TargetPath.IsEmpty())
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
// animation.bake_root_motion_from_bone
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome BakeRootMotionFromBoneImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32000,
        TEXT("[NOT IMPLEMENTED] animation.bake_root_motion_from_bone — use editor.run_python "
             "with FBakingAnimationKeyHelper or open the sequence in Persona"));
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
    return FSageToolDispatch::FOutcome::MakeError(-32000,
        TEXT("[NOT IMPLEMENTED] animation.set_pose_search_schema — PoseSearch plugin required; "
             "use editor.run_python with PoseSearchEditor / Motion Matching API"));
}

// ---------------------------------------------------------------------------
// animation.add_pose_search_sequence
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome AddPoseSearchSequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32000,
        TEXT("[NOT IMPLEMENTED] animation.add_pose_search_sequence — PoseSearch plugin required; "
             "use editor.run_python with PoseSearchEditor API"));
}

// ---------------------------------------------------------------------------
// animation.build_pose_search_index
// ---------------------------------------------------------------------------

FSageToolDispatch::FOutcome BuildPoseSearchIndexImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32000,
        TEXT("[NOT IMPLEMENTED] animation.build_pose_search_index — PoseSearch plugin required; "
             "use editor.run_python with PoseSearchEditor build pipeline"));
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
    if (UEdGraph* AnimGraph = FindAnimGraph(AnimBP))
    {
        const FName Wanted(*GraphName);
        for (UEdGraphNode* N : AnimGraph->Nodes)
        {
            UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(N);
            if (!SMNode || !SMNode->EditorStateMachineGraph) continue;
            for (UEdGraphNode* SN : SMNode->EditorStateMachineGraph->Nodes)
            {
                UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(SN);
                if (!State) continue;
                UEdGraph* Bound = nullptr;
                if (UAnimStateNode* AsState = Cast<UAnimStateNode>(State))
                {
                    Bound = AsState->BoundGraph;
                }
                else if (UAnimStateConduitNode* AsCon = Cast<UAnimStateConduitNode>(State))
                {
                    Bound = AsCon->BoundGraph;
                }
                if (!Bound) continue;
                if (Bound->GetFName() == Wanted
                    || State->GetStateName().Equals(GraphName, ESearchCase::IgnoreCase))
                {
                    return Bound;
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

    FProperty* TargetProperty = AnimNode->GetPinProperty(BindingName);
    if (!TargetProperty)
    {
        OutError = FString::Printf(TEXT("property '%s' is not an AnimGraph input pin property on %s"),
                                   *BindingName.ToString(), *AnimNode->GetClass()->GetName());
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
    const int32 OptionalIndex = FindOptionalPinIndexForProperty(AnimNode, PropertyName);
    const UEdGraphPin* Pin = AnimNode->FindPin(PropertyName);
    const void* ValuePtr = Container ? Property->ContainerPtrToValuePtr<void>(Container) : nullptr;

    Obj->SetBoolField(TEXT("valid"), true);
    Obj->SetStringField(TEXT("name"), PropertyName.ToString());
    Obj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
    Obj->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
    Obj->SetStringField(TEXT("value_text"), ExportPropertyValueText(Property, Container));
    Obj->SetBoolField(TEXT("has_pin"), Pin != nullptr);
    Obj->SetBoolField(TEXT("has_binding"), AnimNode->HasBinding(PropertyName));
    Obj->SetNumberField(TEXT("optional_pin_index"), OptionalIndex);
    Obj->SetBoolField(TEXT("optional_pin"), OptionalIndex != INDEX_NONE);
    if (OptionalIndex != INDEX_NONE)
    {
        const FOptionalPinFromProperty& OptionalPin = AnimNode->ShowPinForProperties[OptionalIndex];
        Obj->SetBoolField(TEXT("pin_visible"), OptionalPin.bShowPin);
        Obj->SetBoolField(TEXT("can_toggle_visibility"), OptionalPin.bCanToggleVisibility);
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

// ---------------------------------------------------------------------------
// animation.remove_animgraph_node
// ---------------------------------------------------------------------------
FSageToolDispatch::FOutcome RemoveAnimGraphNodeImpl(const TSharedPtr<FJsonObject>& Args)
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
    UEdGraphNode* Node = FindGraphNodeByGuid(TargetGraph, NodeId);
    if (!Node)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node_id not found: %s"), *NodeId));
    }

    FScopedTransaction Tx(LOCTEXT("RemoveAGNode", "Sage: Remove AnimGraph Node"));
    AnimBP->Modify();
    FBlueprintEditorUtils::RemoveNode(AnimBP, Node, /*bDontRecompile=*/true);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"),  NodeId);
    R->SetBoolField  (TEXT("removed"),  true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
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
    const bool bWritten = detail::SetPropertyValueAtPtr(TargetProp, TargetValuePtr, Val);
    if (!bWritten)
    {
        Tx.Cancel();
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

    const FName BindingName(*PropName);
    FAnimGraphNodePropertyBinding PropertyBinding;
    FString BindingError;
    if (!BuildAnimNodePropertyBinding(AnimBP, AnimNode, BindingName,
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
    const int32 OptionalPinIndex = FindOptionalPinIndexForProperty(AnimNode, BindingName);
    if (OptionalPinIndex != INDEX_NONE)
    {
        bPinWasVisible = AnimNode->ShowPinForProperties[OptionalPinIndex].bShowPin;
        if (!bPinWasVisible)
        {
            AnimNode->SetPinVisibility(/*bInVisible=*/true, OptionalPinIndex);
            bPinExposed = true;
        }
    }

    if (UEdGraphPin* Pin = AnimNode->FindPin(BindingName))
    {
        Pin->BreakAllPinLinks();
    }

    AnimNode->RemoveBindings(BindingName);
    FString AddError;
    if (!AddBindingMapEntry(BindingObj, MapProp, ValueStruct,
                            BindingName, PropertyBinding, AddError))
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
    R->SetStringField(TEXT("path"), BindingPathToString(BindingPath));
    R->SetStringField(TEXT("binding_class"), BindingObj->GetClass()->GetPathName());
    R->SetBoolField(TEXT("bound"), true);
    R->SetBoolField(TEXT("pin_was_visible"), bPinWasVisible);
    R->SetBoolField(TEXT("pin_exposed"), bPinExposed);
    R->SetNumberField(TEXT("optional_pin_index"), OptionalPinIndex);
    R->SetNumberField(TEXT("binding_count"), BindingCount);
    R->SetObjectField(TEXT("binding"), BindingToJson(BindingName, PropertyBinding));
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

    TArray<TSharedPtr<FJsonValue>> Bindings;
    int32 BindingCount = 0;
    UObject* BindingObj = GetAnimNodeBindingObject(AnimNode);
    ReadBindingMapEntries(BindingObj, Bindings, BindingCount);

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
    R->SetArrayField(TEXT("pins"), Pins);
    R->SetArrayField(TEXT("properties"), Properties);
    R->SetArrayField(TEXT("bindings"), Bindings);
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
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.AnimBP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("node_id"),                Node->NodeGuid.ToString(EGuidFormats::Digits));
    R->SetStringField(TEXT("class"),                  Node->GetClass()->GetPathName());
    R->SetStringField(TEXT("state_machine_name"),     SMName);
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
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_state_entered_event — StateNode.OnEntered/OnExited "
             "event graph hook pending Sage impl"));
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
    // BlendParameters is protected on UBlendSpace in UE 5.7 — pending
    // UBlendSpaceEditorLibrary path or friend helper.
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_blendspace_axis — UBlendSpaceEditorLibrary "
             "path pending (BlendParameters is protected in UE 5.7)"));
}

FSageToolDispatch::FOutcome SetBlendSpaceSmoothingImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_blendspace_smoothing — pending Sage impl"));
}

FSageToolDispatch::FOutcome SetBlendSpaceTargetWeightImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_blendspace_target_weight_interpolation — pending Sage impl"));
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
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_curve_compression — canonical: assign UAnimCurveCompressionSettings asset to UAnimSequence::CurveCompressionSettings + RequestSyncAnimRecompression()"));
}

FSageToolDispatch::FOutcome RunAnimationModifierImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.run_animation_modifier — canonical: UAnimationModifier::ApplyToAnimationSequence (editor-only, requires AnimationModifierLibrary)"));
}

FSageToolDispatch::FOutcome AddAnimationModifierImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.add_animation_modifier — canonical: Sequence->AnimationModifier_AddInstance (editor-only)"));
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
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("function_name"), FuncName);
        R->SetStringField(TEXT("graph_name"), Graph->GetName());
        R->SetStringField(TEXT("schema"), TEXT("AnimationGraphSchema"));
        R->SetStringField(TEXT("root_node_id"),
            RootNode ? RootNode->NodeGuid.ToString(EGuidFormats::Digits) : FString());
        R->SetArrayField(TEXT("collision_warnings"), CollisionWarnings);
        R->SetNumberField(TEXT("collision_warning_count"), CollisionWarnings.Num());
        R->SetBoolField(TEXT("already"), true);
        R->SetBoolField(TEXT("compiled"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("AddLayerFunc", "Sage: Add Layer Function"));
    IfaceBP->Modify();

    FSpawnedLayerGraph Spawn = SpawnAnimLayerFunctionGraph(IfaceBP, FuncFName);
    if (!Spawn.Graph)
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("CreateNewGraph returned null"));
    IfaceBP->FunctionGraphs.Add(Spawn.Graph);

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
    R->SetArrayField(TEXT("collision_warnings"), CollisionWarnings);
    R->SetNumberField(TEXT("collision_warning_count"), CollisionWarnings.Num());
    R->SetBoolField(TEXT("already"), false);
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

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(MasterBP);

    bool bCompiled = false;
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(MasterBP);
        bCompiled = true;
    }

    UEdGraphPin* InputPosePin  = FindFirstInputPosePin(Node);
    UEdGraphPin* OutputPosePin = FindFirstOutputPosePin(Node);

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

    // Best-effort orphan implementer detection deferred — Sage's knowledge
    // graph gives a more reliable answer (DEPENDS_ON references). The _warning
    // surface here is a heuristic hint only. Production: run
    // animation.list_implemented_layers across known children before the
    // remove, or use `references_to(<interface_path>)` from the graph layer.
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
        TEXT("orphan implementer detection deferred — child AnimBPs that already implemented this function will retain their override graphs. Run animation.list_implemented_layers across known children, or use Sage knowledge graph references_to(<interface_path>) for a reliable list."));
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
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_sequence_additive_settings — canonical: IAnimationDataController bracket (additive type/base pose are private in UE 5.7)"));
}

FSageToolDispatch::FOutcome SetSequenceCompressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_sequence_compression_scheme — canonical: assign UAnimBoneCompressionSettings asset to UAnimSequence::BoneCompressionSettings + RequestSyncAnimRecompression()"));
}

FSageToolDispatch::FOutcome AddMontageBranchingPointImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.add_montage_branching_point — canonical: Persona montage editor (BranchingPointMarkers is private in UE 5.7)"));
}

FSageToolDispatch::FOutcome SetMontageBlendCurveImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_montage_blend_curve — canonical: assign UCurveFloat to UAnimMontage::BlendInProfile / BlendOutProfile (pending Sage impl)"));
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
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.copy_animation_curves — canonical: IAnimationDataController curve mutation API on the destination sequence"));
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
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.set_bone_translation_retargeting — pending Sage impl"));
}

FSageToolDispatch::FOutcome AddSkeletonCurveMetadataImpl(const TSharedPtr<FJsonObject>& Args)
{
    return FSageToolDispatch::FOutcome::MakeError(-32601,
        TEXT("[NOT IMPLEMENTED] animation.add_skeleton_curve_metadata — pending Sage impl"));
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
    // Phase 4-r6 (Lyra Sage Gap #17/#18 — new tools)
    Dispatch.RegisterHandler(TEXT("animation.create_anim_notify"),         GT(&CreateAnimNotifyImpl));
    Dispatch.RegisterHandler(TEXT("animation.create_anim_notify_state"),   GT(&CreateAnimNotifyStateImpl));
    Dispatch.RegisterHandler(TEXT("animation.add_blendspace_sample"),      GT(&AddBlendSpaceSampleImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_blendspace_samples"),     GT(&SetBlendSpaceSamplesImpl));
    // Phase 4-r6 Cluster A (AnimGraph node creation core)
    Dispatch.RegisterHandler(TEXT("animation.add_animgraph_node"),         GT(&AddAnimGraphNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.remove_animgraph_node"),      GT(&RemoveAnimGraphNodeImpl));
    Dispatch.RegisterHandler(TEXT("animation.connect_pose_pin"),           GT(&ConnectPosePinImpl));
    Dispatch.RegisterHandler(TEXT("animation.disconnect_pose_pin"),        GT(&DisconnectPosePinImpl));
    Dispatch.RegisterHandler(TEXT("animation.set_anim_node_property"),     GT(&SetAnimNodePropertyImpl));
    Dispatch.RegisterHandler(TEXT("animation.bind_anim_node_property"),    GT(&BindAnimNodePropertyImpl));
    Dispatch.RegisterHandler(TEXT("animation.read_anim_node_properties"),  GT(&ReadAnimNodePropertiesImpl));
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
