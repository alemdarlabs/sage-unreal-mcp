#include "Tools/SageBlueprintTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_K2.h"
#include "Factories/BlueprintFactory.h"
#include "Factories/BlueprintInterfaceFactory.h"
#include "IAssetTools.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "ScopedTransaction.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "SageBlueprint"

namespace sage::tools
{
namespace
{

// ---- common helpers -------------------------------------------------------

UBlueprint* ResolveBlueprint(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    if (UBlueprint* BP = Cast<UBlueprint>(Obj)) return BP;
    // Sometimes the agent passes the generated class path
    // (e.g. "/Game/.../BP_Foo.BP_Foo_C") — walk back to the BP asset.
    if (UClass* Cls = Cast<UClass>(Obj))
    {
        if (UBlueprint* BP = Cast<UBlueprint>(Cls->ClassGeneratedBy)) return BP;
    }
    return nullptr;
}

UEdGraph* FindFunctionGraph(UBlueprint* BP, const FString& FnName)
{
    if (!BP) return nullptr;
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (G && G->GetName() == FnName) return G;
    }
    for (UEdGraph* G : BP->UbergraphPages)
    {
        if (G && G->GetName() == FnName) return G;
    }
    // r2g: also search macro graphs. Delegate signature graphs are
    // intentionally excluded — bp.add_function_parameter assumes a UFunction
    // owner, which delegate sig graphs don't have until compile, and routing
    // bp.add_function_parameter into them crashes UE 5.7. Use a dedicated
    // dispatcher-payload-param tool (r2h) instead.
    for (UEdGraph* G : BP->MacroGraphs)
    {
        if (G && G->GetName() == FnName) return G;
    }
    return nullptr;
}

FString FlagsForVariable(const FBPVariableDescription& V)
{
    TArray<FString> Flags;
    if (V.PropertyFlags & CPF_Edit)             Flags.Add(TEXT("EditAnywhere"));
    if (V.PropertyFlags & CPF_BlueprintReadOnly) Flags.Add(TEXT("BlueprintReadOnly"));
    if (V.PropertyFlags & CPF_BlueprintVisible)  Flags.Add(TEXT("BlueprintVisible"));
    if (V.PropertyFlags & CPF_Net)              Flags.Add(TEXT("Replicated"));
    if (V.PropertyFlags & CPF_RepNotify)        Flags.Add(TEXT("RepNotify"));
    if (V.PropertyFlags & CPF_Transient)        Flags.Add(TEXT("Transient"));
    if (V.PropertyFlags & CPF_SaveGame)         Flags.Add(TEXT("SaveGame"));
    return FString::Join(Flags, TEXT(","));
}

TSharedRef<FJsonObject> VariableToJson(const FBPVariableDescription& V)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"),  V.VarName.ToString());
    Obj->SetStringField(TEXT("type"),  V.VarType.PinCategory.ToString());
    if (V.VarType.PinSubCategoryObject.IsValid())
    {
        Obj->SetStringField(TEXT("type_object"),
            V.VarType.PinSubCategoryObject->GetName());
    }
    if (V.VarType.IsArray()) Obj->SetBoolField(TEXT("is_array"), true);
    if (V.VarType.IsMap())   Obj->SetBoolField(TEXT("is_map"),   true);
    if (V.VarType.IsSet())   Obj->SetBoolField(TEXT("is_set"),   true);
    Obj->SetStringField(TEXT("category"), V.Category.ToString());
    if (!V.DefaultValue.IsEmpty())
        Obj->SetStringField(TEXT("default_value"), V.DefaultValue);
    const FString F = FlagsForVariable(V);
    if (!F.IsEmpty()) Obj->SetStringField(TEXT("flags"), F);
    return Obj;
}

TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"),       Pin->GetName());
    Obj->SetStringField(TEXT("display"),    Pin->GetDisplayName().ToString());
    Obj->SetStringField(TEXT("direction"),  Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    Obj->SetStringField(TEXT("type"),       Pin->PinType.PinCategory.ToString());
    Obj->SetBoolField  (TEXT("is_array"),   Pin->PinType.IsArray());
    Obj->SetBoolField  (TEXT("is_reference"), Pin->PinType.bIsReference);
    if (!Pin->DefaultValue.IsEmpty()) Obj->SetStringField(TEXT("default"), Pin->DefaultValue);
    if (Pin->LinkedTo.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Links;
        for (const UEdGraphPin* L : Pin->LinkedTo)
        {
            if (!L || !L->GetOwningNode()) continue;
            auto LO = MakeShared<FJsonObject>();
            LO->SetStringField(TEXT("node"), L->GetOwningNode()->NodeGuid.ToString());
            LO->SetStringField(TEXT("pin"),  L->GetName());
            Links.Add(MakeShared<FJsonValueObject>(LO));
        }
        Obj->SetArrayField(TEXT("connections"), Links);
    }
    return Obj;
}

TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("id"),       Node->NodeGuid.ToString());
    Obj->SetStringField(TEXT("class"),    Node->GetClass()->GetName());
    Obj->SetStringField(TEXT("title"),    Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Obj->SetNumberField(TEXT("x"),        Node->NodePosX);
    Obj->SetNumberField(TEXT("y"),        Node->NodePosY);
    if (!Node->NodeComment.IsEmpty()) Obj->SetStringField(TEXT("comment"), Node->NodeComment);

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin) Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
    }
    Obj->SetArrayField(TEXT("pins"), Pins);
    return Obj;
}

UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidStr)
{
    if (!Graph) return nullptr;
    FGuid Guid;
    if (!FGuid::Parse(GuidStr, Guid)) return nullptr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid == Guid) return Node;
    }
    return nullptr;
}

UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->GetName() == PinName) return Pin;
    }
    return nullptr;
}

UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph* Graph)
{
    if (!Graph) return nullptr;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(N))
        {
            return Entry;
        }
    }
    return nullptr;
}

// Centralised pin-type construction. UE 5.0+ requires PC_Real to carry a
// PC_Float / PC_Double sub-category — without it KismetCompilerMisc.cpp
// asserts at compile time and the editor crashes. Earlier Sage handlers
// did `PinCategory = FName(*TypeStr)` directly, which produced PC_Real
// with PinSubCategory=None whenever the agent passed type='real' (or
// 'float' / 'double'). Verified the crash: "Erroneous pin subcategory
// for PC_Real: None" in KismetCompilerMisc.cpp:1453.
FEdGraphPinType MakePinType(const FString& TypeStr,
                            const FString& TypeObjStr,
                            bool bIsArray)
{
    FEdGraphPinType PinType;
    const FString L = TypeStr.ToLower();

    if (L == TEXT("bool") || L == TEXT("boolean"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    else if (L == TEXT("int") || L == TEXT("integer") || L == TEXT("int32"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    else if (L == TEXT("int64"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
    else if (L == TEXT("real") || L == TEXT("float") || L == TEXT("double"))
    {
        PinType.PinCategory    = UEdGraphSchema_K2::PC_Real;
        PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    }
    else if (L == TEXT("string") || L == TEXT("str"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_String;
    else if (L == TEXT("name"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
    else if (L == TEXT("text"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
    else if (L == TEXT("byte"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    else if (L == TEXT("object"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
    else if (L == TEXT("class"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
    else if (L == TEXT("struct"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
    else if (L == TEXT("interface"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
    else if (L == TEXT("softobject"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
    else if (L == TEXT("softclass"))
        PinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
    else
    {
        // Pass-through for anything else (e.g. agent passed a verbatim
        // PC_* enum). The compiler will reject obviously bad ones.
        PinType.PinCategory = FName(*TypeStr);
    }

    if (!TypeObjStr.IsEmpty())
    {
        if (UObject* SubObj = FindObject<UObject>(nullptr, *TypeObjStr))
        {
            PinType.PinSubCategoryObject = SubObj;
        }
    }
    if (bIsArray)
    {
        PinType.ContainerType = EPinContainerType::Array;
    }
    return PinType;
}

// Hoisted near common helpers so r2g/p5 (CDO read) AND r2f (asset create)
// can both reach it without forward-declaration churn.
UClass* ResolveParentClassByName(const FString& Name)
{
    if (Name.IsEmpty()) return nullptr;
    if (UClass* C = LoadObject<UClass>(nullptr, *Name)) return C;
    FString Stripped = Name;
    if (Stripped.Len() > 1 && (Stripped[0] == TEXT('A') || Stripped[0] == TEXT('U')))
    {
        const FString Try = Stripped.Mid(1);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->GetName() == Try || It->GetName() == Stripped) return *It;
        }
    }
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->GetName() == Name) return *It;
    }
    return nullptr;
}

// Hoisted near common helpers so r2g (dispatchers) AND r2e (function I/O)
// can both reach it without forward-declaration churn.
TSharedRef<FJsonObject> FunctionParamToJson(const TSharedPtr<FUserPinInfo>& Info,
                                             const TCHAR* Direction)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("name"),      Info->PinName.ToString());
    O->SetStringField(TEXT("type"),      Info->PinType.PinCategory.ToString());
    O->SetStringField(TEXT("direction"), Direction);
    if (Info->PinType.PinSubCategoryObject.IsValid())
    {
        O->SetStringField(TEXT("type_object"),
            Info->PinType.PinSubCategoryObject->GetName());
    }
    if (Info->PinType.IsArray()) O->SetBoolField(TEXT("is_array"), true);
    if (Info->PinType.IsMap())   O->SetBoolField(TEXT("is_map"),   true);
    if (Info->PinType.IsSet())   O->SetBoolField(TEXT("is_set"),   true);
    if (!Info->PinDefaultValue.IsEmpty())
    {
        O->SetStringField(TEXT("default_value"), Info->PinDefaultValue);
    }
    return O;
}

// ---- bp.read --------------------------------------------------------------

FSageToolDispatch::FOutcome BpReadImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("blueprint not found: %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("name"), BP->GetName());
    R->SetStringField(TEXT("path"), FSoftObjectPath(BP).ToString());
    if (BP->GeneratedClass)
    {
        R->SetStringField(TEXT("generated_class"), FSoftObjectPath(BP->GeneratedClass).ToString());
    }
    if (BP->ParentClass)
    {
        R->SetStringField(TEXT("parent_class"), FSoftObjectPath(BP->ParentClass).ToString());
        R->SetStringField(TEXT("parent_name"),  BP->ParentClass->GetName());
    }
    R->SetNumberField(TEXT("variable_count"), BP->NewVariables.Num());
    R->SetNumberField(TEXT("function_count"), BP->FunctionGraphs.Num());
    R->SetNumberField(TEXT("event_graph_count"), BP->UbergraphPages.Num());
    R->SetNumberField(TEXT("macro_graph_count"), BP->MacroGraphs.Num());

    // Implemented interfaces
    TArray<TSharedPtr<FJsonValue>> Iface;
    for (const FBPInterfaceDescription& I : BP->ImplementedInterfaces)
    {
        if (I.Interface) Iface.Add(MakeShared<FJsonValueString>(I.Interface->GetName()));
    }
    R->SetArrayField(TEXT("interfaces"), Iface);

    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.list_variables ----------------------------------------------------

FSageToolDispatch::FOutcome BpListVariablesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FBPVariableDescription& V : BP->NewVariables)
    {
        Out.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("variables"), Out);
    R->SetNumberField(TEXT("count"), Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.list_functions ----------------------------------------------------

FSageToolDispatch::FOutcome BpListFunctionsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    TArray<TSharedPtr<FJsonValue>> Out;
    auto Add = [&](UEdGraph* G, const FString& Kind) {
        if (!G) return;
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), G->GetName());
        Obj->SetStringField(TEXT("kind"), Kind);
        Obj->SetNumberField(TEXT("node_count"), G->Nodes.Num());
        Out.Add(MakeShared<FJsonValueObject>(Obj));
    };
    for (UEdGraph* G : BP->FunctionGraphs) Add(G, TEXT("function"));
    for (UEdGraph* G : BP->UbergraphPages) Add(G, TEXT("event_graph"));
    for (UEdGraph* G : BP->MacroGraphs)    Add(G, TEXT("macro"));

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("functions"), Out);
    R->SetNumberField(TEXT("count"), Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.read_function_graph -----------------------------------------------

FSageToolDispatch::FOutcome BpReadGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'function'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("function graph not found: %s"), *FnName));

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (N) Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(N)));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("graph"), Graph->GetName());
    R->SetArrayField(TEXT("nodes"), Nodes);
    R->SetNumberField(TEXT("count"), Nodes.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.get_execution_flow ------------------------------------------------

FSageToolDispatch::FOutcome BpExecFlowImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'function'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    // BFS from any FunctionEntry / Event node along exec pins.
    TArray<UEdGraphNode*> Frontier;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (Cast<UK2Node_FunctionEntry>(N) || Cast<UK2Node_Event>(N))
        {
            Frontier.Add(N);
        }
    }
    TSet<UEdGraphNode*> Visited;
    TArray<TSharedPtr<FJsonValue>> Steps;
    int32 Order = 0;

    while (Frontier.Num() > 0)
    {
        TArray<UEdGraphNode*> Next;
        for (UEdGraphNode* N : Frontier)
        {
            if (!N || Visited.Contains(N)) continue;
            Visited.Add(N);
            auto Step = MakeShared<FJsonObject>();
            Step->SetNumberField(TEXT("order"), Order++);
            Step->SetStringField(TEXT("id"),    N->NodeGuid.ToString());
            Step->SetStringField(TEXT("class"), N->GetClass()->GetName());
            Step->SetStringField(TEXT("title"), N->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Steps.Add(MakeShared<FJsonValueObject>(Step));

            for (UEdGraphPin* Pin : N->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output) continue;
                if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (Linked && Linked->GetOwningNode())
                    {
                        Next.Add(Linked->GetOwningNode());
                    }
                }
            }
        }
        Frontier = Next;
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("steps"), Steps);
    R->SetNumberField(TEXT("count"), Steps.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.read_components ---------------------------------------------------

FSageToolDispatch::FOutcome BpReadComponentsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    if (!SCS)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetArrayField(TEXT("components"), {});
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    TArray<TSharedPtr<FJsonValue>> Comps;
    TFunction<void(USCS_Node*, const FString&)> Walk;
    Walk = [&](USCS_Node* Node, const FString& ParentName) {
        if (!Node) return;
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
        if (Node->ComponentClass)
            Obj->SetStringField(TEXT("class"), Node->ComponentClass->GetName());
        if (!ParentName.IsEmpty())
            Obj->SetStringField(TEXT("parent"), ParentName);
        if (!Node->AttachToName.IsNone())
            Obj->SetStringField(TEXT("socket"), Node->AttachToName.ToString());
        Comps.Add(MakeShared<FJsonValueObject>(Obj));
        for (USCS_Node* Child : Node->GetChildNodes())
        {
            Walk(Child, Node->GetVariableName().ToString());
        }
    };
    for (USCS_Node* Root : SCS->GetRootNodes()) Walk(Root, FString{});

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("components"), Comps);
    R->SetNumberField(TEXT("count"), Comps.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.search_nodes ------------------------------------------------------

FSageToolDispatch::FOutcome BpSearchNodesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, KW;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("keyword"), KW))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'keyword'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    TArray<TSharedPtr<FJsonValue>> Hits;
    auto Scan = [&](UEdGraph* G, const FString& Kind) {
        if (!G) return;
        for (UEdGraphNode* N : G->Nodes)
        {
            if (!N) continue;
            const FString Title = N->GetNodeTitle(ENodeTitleType::ListView).ToString();
            if (Title.Contains(KW))
            {
                auto H = MakeShared<FJsonObject>();
                H->SetStringField(TEXT("graph"), G->GetName());
                H->SetStringField(TEXT("graph_kind"), Kind);
                H->SetStringField(TEXT("id"),    N->NodeGuid.ToString());
                H->SetStringField(TEXT("title"), Title);
                H->SetStringField(TEXT("class"), N->GetClass()->GetName());
                Hits.Add(MakeShared<FJsonValueObject>(H));
            }
        }
    };
    for (UEdGraph* G : BP->FunctionGraphs) Scan(G, TEXT("function"));
    for (UEdGraph* G : BP->UbergraphPages) Scan(G, TEXT("event_graph"));
    for (UEdGraph* G : BP->MacroGraphs)    Scan(G, TEXT("macro"));

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("hits"), Hits);
    R->SetNumberField(TEXT("count"), Hits.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.compile -----------------------------------------------------------

FSageToolDispatch::FOutcome BpCompileImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    FCompilerResultsLog Results;
    Results.SetSourcePath(BP->GetPathName());
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"), Results.NumErrors == 0);
    R->SetNumberField(TEXT("errors"),  Results.NumErrors);
    R->SetNumberField(TEXT("warnings"),Results.NumWarnings);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.add_variable ------------------------------------------------------

FSageToolDispatch::FOutcome BpAddVariableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, VarName, TypeStr;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), VarName)
        || !Args->TryGetStringField(TEXT("type"), TypeStr))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'name', or 'type'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    FString TypeObj;
    Args->TryGetStringField(TEXT("type_object"), TypeObj);
    bool bArr = false;
    Args->TryGetBoolField(TEXT("is_array"), bArr);
    FEdGraphPinType PinType = MakePinType(TypeStr, TypeObj, bArr);

    FScopedTransaction Tx(LOCTEXT("BpAddVar", "Sage: Add BP Variable"));
    BP->Modify();
    const FName VName(*VarName);
    if (FBlueprintEditorUtils::FindNewVariableIndex(BP, VName) != INDEX_NONE)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("variable already exists: %s"), *VarName));
    }
    if (!FBlueprintEditorUtils::AddMemberVariable(BP, VName, PinType))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("AddMemberVariable failed"));
    }
    if (Args->HasField(TEXT("default_value")))
    {
        FString DV;
        if (Args->TryGetStringField(TEXT("default_value"), DV))
        {
            // BP variable defaults are stored as serialised strings on the
            // FBPVariableDescription (compiler reads them at class build).
            for (FBPVariableDescription& V : BP->NewVariables)
            {
                if (V.VarName == VName) { V.DefaultValue = DV; break; }
            }
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("variable"),  VarName);
    R->SetStringField(TEXT("type"),      TypeStr);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.delete_variable ---------------------------------------------------

FSageToolDispatch::FOutcome BpDeleteVariableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, VarName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), VarName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    FScopedTransaction Tx(LOCTEXT("BpDelVar", "Sage: Remove BP Variable"));
    BP->Modify();
    FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*VarName));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("variable"),  VarName);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.set_variable_default ---------------------------------------------

FSageToolDispatch::FOutcome BpSetVariableDefaultImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, VarName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), VarName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    const TSharedPtr<FJsonValue>* ValueField = nullptr;
    Args->Values.Find(TEXT("value"));
    auto It = Args->Values.Find(TEXT("value"));
    if (!It) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));

    const FName VName(*VarName);
    bool bFound = false;
    FScopedTransaction Tx(LOCTEXT("BpSetVarDef", "Sage: Set BP Variable Default"));
    BP->Modify();
    for (FBPVariableDescription& V : BP->NewVariables)
    {
        if (V.VarName == VName)
        {
            // Convert JSON to string default
            const TSharedPtr<FJsonValue>& Val = *It;
            if (Val->Type == EJson::String) V.DefaultValue = Val->AsString();
            else if (Val->Type == EJson::Number) V.DefaultValue = FString::SanitizeFloat(Val->AsNumber());
            else if (Val->Type == EJson::Boolean) V.DefaultValue = Val->AsBool() ? TEXT("true") : TEXT("false");
            else V.DefaultValue.Empty();
            bFound = true;
            break;
        }
    }
    if (!bFound)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("variable not found: %s"), *VarName));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("variable"),  VarName);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.add_function ------------------------------------------------------

FSageToolDispatch::FOutcome BpAddFunctionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    FScopedTransaction Tx(LOCTEXT("BpAddFn", "Sage: Add BP Function"));
    BP->Modify();
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, FName(*FnName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph,
        /*bIsUserCreated*/ true, nullptr);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.delete_function ---------------------------------------------------

FSageToolDispatch::FOutcome BpDeleteFunctionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph || !BP->FunctionGraphs.Contains(Graph))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("function not found"));
    }
    FScopedTransaction Tx(LOCTEXT("BpDelFn", "Sage: Remove BP Function"));
    BP->Modify();
    FBlueprintEditorUtils::RemoveGraph(BP, Graph);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.set_cdo_property --------------------------------------------------

FSageToolDispatch::FOutcome BpSetCdoPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, PropName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("property"), PropName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'property'"));
    }
    auto It = Args->Values.Find(TEXT("value"));
    if (!It) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    if (!BP->GeneratedClass) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no generated class"));

    UObject* CDO = BP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded*/ true);
    if (!CDO) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("CDO unavailable"));
    FProperty* P = CDO->GetClass()->FindPropertyByName(FName(*PropName));
    if (!P) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("property not found"));

    FScopedTransaction Tx(LOCTEXT("BpCdo", "Sage: Set CDO Property"));
    CDO->Modify();
    CDO->PreEditChange(P);
    if (!detail::SetUPropertyFromJson(CDO, P, *It))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("value rejected by reflection set"));
    }
    FPropertyChangedEvent E(P);
    CDO->PostEditChangeProperty(E);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("property"),  PropName);
    R->SetField(TEXT("value"), *It);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.add_node + bp.delete_node + bp.connect_pins ----------------------

FSageToolDispatch::FOutcome BpDeleteNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, NodeId;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', or 'node_id'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));
    UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
    if (!Node) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("node not found"));

    FScopedTransaction Tx(LOCTEXT("BpDelNode", "Sage: Delete BP Node"));
    Graph->Modify();
    Node->Modify();
    Graph->RemoveNode(Node);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    R->SetStringField(TEXT("node_id"),   NodeId);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpConnectPinsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, FromNode, FromPin, ToNode, ToPin;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("from_node"), FromNode)
        || !Args->TryGetStringField(TEXT("from_pin"),  FromPin)
        || !Args->TryGetStringField(TEXT("to_node"),   ToNode)
        || !Args->TryGetStringField(TEXT("to_pin"),    ToPin))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing connection arguments"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    UEdGraphNode* SrcN = FindNodeByGuid(Graph, FromNode);
    UEdGraphNode* DstN = FindNodeByGuid(Graph, ToNode);
    if (!SrcN || !DstN) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("nodes not found"));
    UEdGraphPin* SrcP = FindPin(SrcN, FromPin);
    UEdGraphPin* DstP = FindPin(DstN, ToPin);
    if (!SrcP || !DstP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("pins not found"));

    FScopedTransaction Tx(LOCTEXT("BpConnect", "Sage: Connect BP Pins"));
    Graph->Modify();
    if (!Graph->GetSchema()->TryCreateConnection(SrcP, DstP))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("connection rejected by schema"));
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.set_variable_properties + bp.get_cdo_properties + ----------------
// ---- bp.get_dependencies  (Phase 4.2 round 2g/p5) ------------------------

FSageToolDispatch::FOutcome BpSetVariablePropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, VarName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), VarName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    FBPVariableDescription* Var = nullptr;
    for (FBPVariableDescription& V : BP->NewVariables)
    {
        if (V.VarName.ToString() == VarName) { Var = &V; break; }
    }
    if (!Var) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("variable not found: %s"), *VarName));

    FScopedTransaction Tx(LOCTEXT("BpSetVarProps", "Sage: Set BP Variable Properties"));
    BP->Modify();

    // instance_editable: when enabled, also strip the 'private' metadata
    // hint so the editor surface treats it as exposed (UE-MCP pattern).
    bool bInstanceEditable = false;
    if (Args->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable))
    {
        if (bInstanceEditable)
        {
            Var->PropertyFlags |= CPF_Edit;
            Var->RemoveMetaData(FBlueprintMetadata::MD_Private);
        }
        else
        {
            Var->PropertyFlags &= ~CPF_Edit;
        }
    }

    auto ApplyFlag = [&](const TCHAR* FieldName, EPropertyFlags Flag)
    {
        bool b = false;
        if (Args->TryGetBoolField(FieldName, b))
        {
            if (b) Var->PropertyFlags |=  Flag;
            else   Var->PropertyFlags &= ~Flag;
        }
    };
    ApplyFlag(TEXT("blueprint_readonly"),   CPF_BlueprintReadOnly);
    ApplyFlag(TEXT("transient"),            CPF_Transient);
    ApplyFlag(TEXT("save_game"),            CPF_SaveGame);

    bool bReplicated = false;
    if (Args->TryGetBoolField(TEXT("replicated"), bReplicated))
    {
        if (bReplicated) Var->PropertyFlags |=  (CPF_Net);
        else             Var->PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
    }

    bool bExposeOnSpawn = false;
    if (Args->TryGetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn))
    {
        if (bExposeOnSpawn)
        {
            Var->SetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
            Var->PropertyFlags |= CPF_ExposeOnSpawn;
        }
        else
        {
            Var->RemoveMetaData(FBlueprintMetadata::MD_ExposeOnSpawn);
            Var->PropertyFlags &= ~CPF_ExposeOnSpawn;
        }
    }

    FString Category;
    if (Args->TryGetStringField(TEXT("category"), Category))
    {
        Var->SetMetaData(FBlueprintMetadata::MD_FunctionCategory, *Category);
    }
    FString Tooltip;
    if (Args->TryGetStringField(TEXT("tooltip"), Tooltip))
    {
        Var->SetMetaData(FBlueprintMetadata::MD_Tooltip, *Tooltip);
    }

    // ROOT CAUSE FIX: do NOT manually MarkBlueprintAsModified /
    // MarkBlueprintAsStructurallyModified. Recompiling the BP
    // automatically reconciles class layout + dirties the package; the
    // manual Mark before compile leaves the BP in a half-mutated state
    // that crashes the editor on next mutation. UE-MCP just calls
    // CompileBlueprint, full stop.
    FKismetEditorUtilities::CompileBlueprint(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("variable"),  VarName);
    R->SetStringField(TEXT("flags"),     FlagsForVariable(*Var));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpGetCdoPropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ClassName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("class"), ClassName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'class'"));
    }
    UClass* Cls = ResolveParentClassByName(ClassName);
    if (!Cls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("class not found: %s"), *ClassName));

    UObject* CDO = Cls->GetDefaultObject();
    if (!CDO) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no CDO"));

    // Optional name filter
    TSet<FString> Filter;
    const TArray<TSharedPtr<FJsonValue>>* NamesArr = nullptr;
    if (Args->TryGetArrayField(TEXT("properties"), NamesArr) && NamesArr)
    {
        for (const auto& V : *NamesArr)
        {
            FString S; if (V->TryGetString(S)) Filter.Add(S);
        }
    }

    auto Props = MakeShared<FJsonObject>();
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Cls); It; ++It)
    {
        FProperty* P = *It;
        if (!P) continue;
        if (P->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
        const FString N = P->GetName();
        if (Filter.Num() > 0 && !Filter.Contains(N)) continue;
        TSharedPtr<FJsonValue> V = detail::GetUPropertyAsJson(CDO, P);
        if (V.IsValid()) { Props->SetField(N, V); ++Count; }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("class"),      Cls->GetPathName());
    R->SetStringField(TEXT("class_name"), Cls->GetName());
    R->SetObjectField(TEXT("properties"), Props);
    R->SetNumberField(TEXT("count"),      Count);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpGetDependenciesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    bool bReverse = false;
    Args->TryGetBoolField(TEXT("reverse"), bReverse);

    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = Module.Get();
    const FName PackageName = BP->GetOutermost()->GetFName();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetBoolField  (TEXT("reverse"),   bReverse);

    if (bReverse)
    {
        TArray<FName> Referencers;
        Registry.GetReferencers(PackageName, Referencers,
            UE::AssetRegistry::EDependencyCategory::Package);
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Referencers.Num());
        for (const FName& N : Referencers) Arr.Add(MakeShared<FJsonValueString>(N.ToString()));
        R->SetArrayField (TEXT("referencers"),       Arr);
        R->SetNumberField(TEXT("referencer_count"),  Arr.Num());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    TArray<FName> Deps;
    Registry.GetDependencies(PackageName, Deps,
        UE::AssetRegistry::EDependencyCategory::Package);
    TArray<TSharedPtr<FJsonValue>> DepArr;
    DepArr.Reserve(Deps.Num());
    for (const FName& N : Deps) DepArr.Add(MakeShared<FJsonValueString>(N.ToString()));
    R->SetArrayField(TEXT("dependencies"), DepArr);
    R->SetNumberField(TEXT("dependency_count"), DepArr.Num());

    // Class refs: parent + variable type objects
    TSet<FString> Classes;
    if (UClass* ParentCls = BP->ParentClass)
    {
        Classes.Add(ParentCls->GetPathName());
    }
    for (const FBPVariableDescription& V : BP->NewVariables)
    {
        if (UObject* Sub = V.VarType.PinSubCategoryObject.Get())
            Classes.Add(Sub->GetPathName());
    }
    TArray<TSharedPtr<FJsonValue>> ClassArr;
    for (const FString& C : Classes) ClassArr.Add(MakeShared<FJsonValueString>(C));
    R->SetArrayField(TEXT("referenced_classes"), ClassArr);

    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.validate + bp.run_construction_script  (Phase 4.2 round 2g/p4) ---

FSageToolDispatch::FOutcome BpValidateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    FCompilerResultsLog Log;
    Log.bSilentMode = true;
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Log);

    TArray<TSharedPtr<FJsonValue>> Messages;
    for (TSharedRef<FTokenizedMessage> Msg : Log.Messages)
    {
        auto O = MakeShared<FJsonObject>();
        const EMessageSeverity::Type Sev = Msg->GetSeverity();
        const TCHAR* SevStr =
            (Sev == EMessageSeverity::Error)            ? TEXT("error")
          : (Sev == EMessageSeverity::Warning)          ? TEXT("warning")
          : (Sev == EMessageSeverity::PerformanceWarning) ? TEXT("perf")
          : TEXT("info");
        O->SetStringField(TEXT("severity"), SevStr);
        O->SetStringField(TEXT("message"),  Msg->ToText().ToString());
        Messages.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),     BP->GetName());
    R->SetBoolField  (TEXT("valid"),         Log.NumErrors == 0);
    R->SetNumberField(TEXT("error_count"),   Log.NumErrors);
    R->SetNumberField(TEXT("warning_count"), Log.NumWarnings);
    R->SetArrayField (TEXT("messages"),      Messages);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpRunConstructionScriptImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    if (!BP->GeneratedClass)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("blueprint has no GeneratedClass (compile first?)"));
    }
    if (!BP->GeneratedClass->IsChildOf(AActor::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("blueprint is not Actor-derived; nothing to spawn"));
    }
    if (!GEditor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("editor world unavailable"));
    }

    FVector Loc = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Loc);

    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Spawn.bNoFail        = true;
    Spawn.ObjectFlags   |= RF_Transient;  // Don't dirty/persist this throwaway

    AActor* Temp = World->SpawnActor<AActor>(BP->GeneratedClass, Loc, FRotator::ZeroRotator, Spawn);
    if (!Temp)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("SpawnActor returned nullptr"));
    }
    // SpawnActor calls UCS automatically; this is a belt-and-braces re-run
    // in case the agent passes a pre-spawned, mutated transform later.
    Temp->RerunConstructionScripts();

    TArray<TSharedPtr<FJsonValue>> Comps;
    TArray<UActorComponent*> ActorComps;
    Temp->GetComponents(ActorComps);
    for (UActorComponent* C : ActorComps)
    {
        if (!C) continue;
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"),  C->GetName());
        O->SetStringField(TEXT("class"), C->GetClass()->GetName());
        if (USceneComponent* SC = Cast<USceneComponent>(C))
        {
            const FTransform T = SC->GetRelativeTransform();
            O->SetField(TEXT("location"), detail::Vec3ToJson(T.GetLocation()));
            O->SetField(TEXT("rotation"), detail::Rot3ToJson(T.GetRotation().Rotator()));
            O->SetField(TEXT("scale"),    detail::Vec3ToJson(T.GetScale3D()));
            O->SetBoolField(TEXT("is_root"), SC == Temp->GetRootComponent());
        }
        Comps.Add(MakeShared<FJsonValueObject>(O));
    }

    World->DestroyActor(Temp);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),  BP->GetName());
    R->SetStringField(TEXT("class"),      BP->GeneratedClass->GetName());
    R->SetArrayField (TEXT("components"), Comps);
    R->SetNumberField(TEXT("count"),      Comps.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.read_component_properties + bp.get_component_property + ----------
// ---- bp.reparent_component  (Phase 4.2 round 2g/p3) ----------------------

USCS_Node* FindSCSNode(UBlueprint* BP, const FString& Name)
{
    if (!BP) return nullptr;
    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    if (!SCS) return nullptr;
    return SCS->FindSCSNode(FName(*Name));
}

FSageToolDispatch::FOutcome BpReadComponentPropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, ComponentName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("component"), ComponentName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'component'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    USCS_Node* Node = FindSCSNode(BP, ComponentName);
    if (!Node) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("SCS component not found: %s"), *ComponentName));
    UActorComponent* Template = Node->ComponentTemplate;
    if (!Template) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("SCS node has no ComponentTemplate"));

    auto Props = MakeShared<FJsonObject>();
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Template->GetClass()); It; ++It)
    {
        FProperty* P = *It;
        if (!P) continue;
        // Skip transient / editor-only / non-editable noise
        if (P->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
        const FString PropName = P->GetName();
        TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Template, P);
        if (Value.IsValid())
        {
            Props->SetField(PropName, Value);
            ++Count;
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),    BP->GetName());
    R->SetStringField(TEXT("component"),    ComponentName);
    R->SetStringField(TEXT("class"),        Template->GetClass()->GetName());
    R->SetObjectField(TEXT("properties"),   Props);
    R->SetNumberField(TEXT("count"),        Count);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpGetComponentPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, ComponentName, PropName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("component"), ComponentName)
        || !Args->TryGetStringField(TEXT("property"), PropName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'component', or 'property'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    USCS_Node* Node = FindSCSNode(BP, ComponentName);
    if (!Node) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("SCS component not found: %s"), *ComponentName));
    UActorComponent* Template = Node->ComponentTemplate;
    if (!Template) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("SCS node has no ComponentTemplate"));

    FProperty* P = Template->GetClass()->FindPropertyByName(FName(*PropName));
    if (!P) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("property not found: %s on %s"),
                        *PropName, *Template->GetClass()->GetName()));

    TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Template, P);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("component"), ComponentName);
    R->SetStringField(TEXT("property"),  PropName);
    R->SetStringField(TEXT("type"),      P->GetClass()->GetName());
    if (Value.IsValid())
    {
        R->SetField(TEXT("value"), Value);
    }
    else
    {
        R->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpReparentComponentImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, ComponentName, NewParent;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("component"), ComponentName)
        || !Args->TryGetStringField(TEXT("new_parent"), NewParent))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'component', or 'new_parent'"));
    }
    if (ComponentName == NewParent)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("component cannot be its own parent"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    if (!SCS) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("blueprint has no SimpleConstructionScript"));

    USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
    if (!Node) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("SCS component not found: %s"), *ComponentName));
    USCS_Node* NewParentNode = SCS->FindSCSNode(FName(*NewParent));
    if (!NewParentNode) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("new_parent not found: %s"), *NewParent));

    // Cycle guard: walk up new_parent's chain and reject if we hit Node.
    USCS_Node* Walk = NewParentNode;
    while (Walk)
    {
        if (Walk == Node)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("reparent would create a cycle"));
        }
        Walk = SCS->FindParentNode(Walk);
    }

    FScopedTransaction Tx(LOCTEXT("BpReparentComp", "Sage: Reparent BP Component"));
    BP->Modify();
    SCS->Modify();

    // Detach from current parent (or root list).
    if (USCS_Node* OldParent = SCS->FindParentNode(Node))
    {
        OldParent->RemoveChildNode(Node, /*bRemoveFromAllNodes*/ false);
    }
    else
    {
        SCS->RemoveNode(Node, /*bValidateSceneRootNodes*/ true);
    }
    // Attach to new parent.
    NewParentNode->AddChildNode(Node, /*bAddToAllNodes*/ false);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),  BP->GetName());
    R->SetStringField(TEXT("component"),  ComponentName);
    R->SetStringField(TEXT("new_parent"), NewParent);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.export_nodes_t3d + bp.import_nodes_t3d  (Phase 4.2 round 2g/p2) ---

FSageToolDispatch::FOutcome BpExportNodesT3DImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'function'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    // Optional node_ids filter; default = whole graph.
    TArray<UEdGraphNode*> Selected;
    const TArray<TSharedPtr<FJsonValue>>* IdsPtr = nullptr;
    if (Args->TryGetArrayField(TEXT("node_ids"), IdsPtr) && IdsPtr && IdsPtr->Num() > 0)
    {
        for (const TSharedPtr<FJsonValue>& V : *IdsPtr)
        {
            if (!V.IsValid()) continue;
            const FString Id = V->AsString();
            UEdGraphNode* N = FindNodeByGuid(Graph, Id);
            if (!N) return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("node not found: %s"), *Id));
            Selected.AddUnique(N);
        }
    }
    else
    {
        for (UEdGraphNode* N : Graph->Nodes) if (N) Selected.Add(N);
    }

    if (Selected.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("no nodes to export"));
    }

    // ExportNodesToText only writes nodes flagged CanDuplicateNode (UE editor's
    // Copy filter — entry/return nodes are excluded). Pre-filter so the count
    // matches reality.
    TSet<UObject*> NodeSet;
    int32 Skipped = 0;
    for (UEdGraphNode* N : Selected)
    {
        if (N && N->CanDuplicateNode())
        {
            N->PrepareForCopying();
            NodeSet.Add(N);
        }
        else
        {
            ++Skipped;
        }
    }
    if (NodeSet.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("no duplicatable nodes (entry/return nodes can't be exported)"));
    }

    FString Exported;
    FEdGraphUtilities::ExportNodesToText(NodeSet, Exported);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    R->SetStringField(TEXT("t3d"),       Exported);
    R->SetNumberField(TEXT("count"),     NodeSet.Num());
    R->SetNumberField(TEXT("skipped"),   Skipped);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpImportNodesT3DImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, T3D;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("t3d"), T3D))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', or 't3d'"));
    }
    if (T3D.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("'t3d' is empty"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    if (!FEdGraphUtilities::CanImportNodesFromText(Graph, T3D))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("t3d not importable into this graph (schema mismatch or malformed)"));
    }

    FScopedTransaction Tx(LOCTEXT("BpImportT3D", "Sage: Import T3D Nodes"));
    Graph->Modify();

    TSet<UEdGraphNode*> Pasted;
    FEdGraphUtilities::ImportNodesFromText(Graph, T3D, /*out*/ Pasted);
    if (Pasted.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("ImportNodesFromText produced no nodes"));
    }

    // Optional re-center: anchor pasted nodes' centroid at (pos_x, pos_y).
    double AnchorX = 0.0, AnchorY = 0.0;
    const bool bRecenter = Args->HasField(TEXT("pos_x")) && Args->HasField(TEXT("pos_y"));
    if (bRecenter)
    {
        Args->TryGetNumberField(TEXT("pos_x"), AnchorX);
        Args->TryGetNumberField(TEXT("pos_y"), AnchorY);
        double AvgX = 0.0, AvgY = 0.0;
        for (UEdGraphNode* N : Pasted) { AvgX += N->NodePosX; AvgY += N->NodePosY; }
        AvgX /= Pasted.Num();
        AvgY /= Pasted.Num();
        for (UEdGraphNode* N : Pasted)
        {
            N->NodePosX = static_cast<int32>((N->NodePosX - AvgX) + AnchorX);
            N->NodePosY = static_cast<int32>((N->NodePosY - AvgY) + AnchorY);
        }
    }

    // Fresh GUIDs so re-pasting into the same graph doesn't collide with the
    // original nodes.
    TArray<TSharedPtr<FJsonValue>> Ids;
    for (UEdGraphNode* N : Pasted)
    {
        N->CreateNewGuid();
        N->PostPasteNode();
        if (UK2Node* K2 = Cast<UK2Node>(N)) K2->ReconstructNode();
        Ids.Add(MakeShared<FJsonValueString>(N->NodeGuid.ToString()));
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    R->SetNumberField(TEXT("count"),     Pasted.Num());
    R->SetArrayField (TEXT("node_ids"),  Ids);
    R->SetBoolField  (TEXT("recentered"), bRecenter);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.list_event_dispatchers + bp.add_event_dispatcher + ---------------
// ---- bp.remove_event_dispatcher  (Phase 4.2 round 2g) --------------------

// In UE, an event dispatcher is a multicast delegate stored as a member
// variable (PC_MCDelegate) PLUS a UEdGraph in BP->DelegateSignatureGraphs
// named "<Name>__DelegateSignature" that holds the entry node + signature.

const TCHAR* kSigGraphSuffix = TEXT("__DelegateSignature");

FSageToolDispatch::FOutcome BpListEventDispatchersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    TArray<TSharedPtr<FJsonValue>> Out;
    for (UEdGraph* G : BP->DelegateSignatureGraphs)
    {
        if (!G) continue;
        const FString GName = G->GetName();
        FString DispatcherName = GName;
        if (DispatcherName.EndsWith(kSigGraphSuffix))
        {
            DispatcherName.LeftChopInline(FCString::Strlen(kSigGraphSuffix));
        }

        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"),       DispatcherName);
        O->SetStringField(TEXT("graph"),      GName);
        O->SetNumberField(TEXT("node_count"), G->Nodes.Num());

        // Inputs (= dispatcher payload params), pulled from the entry node
        // of the signature graph.
        TArray<TSharedPtr<FJsonValue>> Params;
        for (UEdGraphNode* N : G->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(N))
            {
                for (const TSharedPtr<FUserPinInfo>& Info : Entry->UserDefinedPins)
                {
                    if (Info.IsValid())
                        Params.Add(MakeShared<FJsonValueObject>(
                            FunctionParamToJson(Info, TEXT("input"))));
                }
                break;
            }
        }
        O->SetArrayField(TEXT("parameters"), Params);

        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetArrayField (TEXT("dispatchers"), Out);
    R->SetNumberField(TEXT("count"),     Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpAddEventDispatcherImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, DispName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), DispName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    const FName DispFName(*DispName);

    // Idempotency: existing var with the same name?
    for (const FBPVariableDescription& V : BP->NewVariables)
    {
        if (V.VarName == DispFName)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("variable already exists: %s "
                                     "(dispatcher or member variable)"), *DispName));
        }
    }

    FScopedTransaction Tx(LOCTEXT("BpAddDispatcher", "Sage: Add Event Dispatcher"));
    BP->Modify();

    const FString SigGraphName = DispName + kSigGraphSuffix;
    UEdGraph* SigGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, FName(*SigGraphName),
        UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!SigGraph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("CreateNewGraph returned nullptr for delegate signature graph"));
    }
    BP->DelegateSignatureGraphs.AddUnique(SigGraph);
    SigGraph->SetFlags(RF_Transactional);
    SigGraph->GetSchema()->CreateDefaultNodesForGraph(*SigGraph);
    // Note: dispatcher payload parameters (configurable via bp.add_function_
    // parameter on "<Name>__DelegateSignature") are NOT supported by this
    // surface. Spawning UK2Node_FunctionEntry manually into a delegate
    // signature graph crashed the editor in UE 5.7 — the function-reference
    // codepath assumes a real UFunction owner. A dedicated dispatcher-
    // payload-param tool is queued for r2h.

    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    PinType.PinSubCategoryMemberReference.MemberName = SigGraph->GetFName();
    PinType.PinSubCategoryMemberReference.MemberGuid = SigGraph->GraphGuid;

    if (!FBlueprintEditorUtils::AddMemberVariable(BP, DispFName, PinType))
    {
        // Roll back the graph addition.
        BP->DelegateSignatureGraphs.Remove(SigGraph);
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("AddMemberVariable failed for dispatcher"));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),  BP->GetName());
    R->SetStringField(TEXT("dispatcher"), DispName);
    R->SetStringField(TEXT("graph"),      SigGraph->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpRemoveEventDispatcherImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, DispName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), DispName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    const FName DispFName(*DispName);
    const FString SigGraphName = DispName + kSigGraphSuffix;

    UEdGraph* SigGraph = nullptr;
    for (UEdGraph* G : BP->DelegateSignatureGraphs)
    {
        if (G && G->GetName() == SigGraphName) { SigGraph = G; break; }
    }

    bool bAnyRemoved = false;
    FScopedTransaction Tx(LOCTEXT("BpRemDispatcher", "Sage: Remove Event Dispatcher"));
    BP->Modify();

    // Remove the member variable side (only if it actually existed).
    if (FBlueprintEditorUtils::FindNewVariableIndex(BP, DispFName) != INDEX_NONE)
    {
        FBlueprintEditorUtils::RemoveMemberVariable(BP, DispFName);
        bAnyRemoved = true;
    }
    // Remove the signature graph side.
    if (SigGraph)
    {
        BP->DelegateSignatureGraphs.Remove(SigGraph);
        FBlueprintEditorUtils::RemoveGraph(BP, SigGraph, EGraphRemoveFlags::Default);
        bAnyRemoved = true;
    }

    if (bAnyRemoved)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),  BP->GetName());
    R->SetStringField(TEXT("dispatcher"), DispName);
    R->SetNumberField(TEXT("removed"),    bAnyRemoved ? 1 : 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.create + bp.create_interface  (Phase 4.2 round 2f) ----------------

// Split "/Game/Folder/Asset" or "/Game/Folder/Asset.Asset" into
// (PackagePath="/Game/Folder", AssetName="Asset"). Returns false on a
// malformed input.
bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
{
    FString Sanitised = AssetPath;
    int32 DotIdx;
    if (Sanitised.FindChar('.', DotIdx)) Sanitised.LeftInline(DotIdx);
    int32 SlashIdx;
    if (!Sanitised.FindLastChar('/', SlashIdx)) return false;
    OutPackagePath = Sanitised.Left(SlashIdx);
    OutAssetName   = Sanitised.Mid(SlashIdx + 1);
    return !OutPackagePath.IsEmpty() && !OutAssetName.IsEmpty();
}

// ResolveParentClassByName hoisted to common-helpers (top of file).

FSageToolDispatch::FOutcome BpCreateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, ParentName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    Args->TryGetStringField(TEXT("parent_class"), ParentName);
    if (ParentName.IsEmpty()) ParentName = TEXT("Actor");

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    UClass* ParentCls = ResolveParentClassByName(ParentName);
    if (!ParentCls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("parent class not found: %s (try '/Script/Engine.Actor' "
                                 "or short name 'Actor')"), *ParentName));
    }

    FString PackagePath, AssetName;
    if (!SplitAssetPath(Path, PackagePath, AssetName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("malformed path: %s"), *Path));
    }

    // Idempotency: if asset exists, return existing.
    if (UBlueprint* Existing = ResolveBlueprint(Path))
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"),         Path);
        R->SetStringField(TEXT("blueprint"),    Existing->GetName());
        R->SetStringField(TEXT("parent_class"),
            Existing->ParentClass ? Existing->ParentClass->GetPathName() : FString());
        R->SetBoolField  (TEXT("already"),      true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FAssetToolsModule& Module = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    IAssetTools& Tools = Module.Get();

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = ParentCls;

    FScopedTransaction Tx(LOCTEXT("BpCreate", "Sage: Create Blueprint"));
    UBlueprint* NewBP = Cast<UBlueprint>(
        Tools.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory));
    if (!NewBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("AssetTools::CreateAsset returned nullptr"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         NewBP->GetPathName());
    R->SetStringField(TEXT("blueprint"),    NewBP->GetName());
    R->SetStringField(TEXT("parent_class"), ParentCls->GetPathName());
    R->SetBoolField  (TEXT("already"),      false);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpCreateInterfaceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    FString PackagePath, AssetName;
    if (!SplitAssetPath(Path, PackagePath, AssetName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("malformed path: %s"), *Path));
    }

    if (UBlueprint* Existing = ResolveBlueprint(Path))
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"),      Path);
        R->SetStringField(TEXT("blueprint"), Existing->GetName());
        R->SetBoolField  (TEXT("already"),   true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FAssetToolsModule& Module = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    IAssetTools& Tools = Module.Get();

    UBlueprintInterfaceFactory* Factory = NewObject<UBlueprintInterfaceFactory>();

    FScopedTransaction Tx(LOCTEXT("BpCreateIface", "Sage: Create BP Interface"));
    UBlueprint* NewBP = Cast<UBlueprint>(
        Tools.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory));
    if (!NewBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("AssetTools::CreateAsset returned nullptr"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         NewBP->GetPathName());
    R->SetStringField(TEXT("blueprint"),    NewBP->GetName());
    R->SetStringField(TEXT("parent_class"),
        NewBP->ParentClass ? NewBP->ParentClass->GetPathName() : TEXT("/Script/CoreUObject.Interface"));
    R->SetBoolField  (TEXT("already"),      false);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.list_function_parameters + bp.add_function_parameter + ------------
// ---- bp.remove_function_parameter  (Phase 4.2 round 2e) ------------------

UK2Node_FunctionResult* FindOrCreateFunctionResult(UEdGraph* Graph,
                                                    UK2Node_FunctionEntry* Entry,
                                                    bool bAllowCreate)
{
    if (!Graph) return nullptr;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N))
        {
            return R;
        }
    }
    if (!bAllowCreate) return nullptr;

    // Auto-spawn FunctionResult to the right of Entry. UE displays it in
    // the function graph automatically once added; agent can wire return
    // pins via bp.connect_pins.
    UK2Node_FunctionResult* New = NewObject<UK2Node_FunctionResult>(Graph);
    Graph->AddNode(New, /*bSelectNewNode*/ false, /*bUpdateGraphCount*/ true);
    New->CreateNewGuid();
    if (Entry)
    {
        New->NodePosX = Entry->NodePosX + 400;
        New->NodePosY = Entry->NodePosY;
        // Match function reference so the result node is correctly bound
        // to the same function.
        New->FunctionReference = Entry->FunctionReference;
    }
    New->AllocateDefaultPins();
    New->PostPlacedNewNode();
    return New;
}

// FunctionParamToJson hoisted to common helpers (top of file).

FSageToolDispatch::FOutcome BpListFunctionParametersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'function'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;

    if (UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph))
    {
        for (const TSharedPtr<FUserPinInfo>& Info : Entry->UserDefinedPins)
        {
            if (Info.IsValid())
                Inputs.Add(MakeShared<FJsonValueObject>(
                    FunctionParamToJson(Info, TEXT("input"))));
        }
    }
    if (UK2Node_FunctionResult* Result = FindOrCreateFunctionResult(Graph, nullptr, /*allow_create*/ false))
    {
        for (const TSharedPtr<FUserPinInfo>& Info : Result->UserDefinedPins)
        {
            if (Info.IsValid())
                Outputs.Add(MakeShared<FJsonValueObject>(
                    FunctionParamToJson(Info, TEXT("output"))));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),  BP->GetName());
    R->SetStringField(TEXT("function"),   FnName);
    R->SetArrayField (TEXT("inputs"),     Inputs);
    R->SetArrayField (TEXT("outputs"),    Outputs);
    R->SetNumberField(TEXT("input_count"),  Inputs.Num());
    R->SetNumberField(TEXT("output_count"), Outputs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpAddFunctionParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, ParamName, TypeStr, DirStr;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("name"), ParamName)
        || !Args->TryGetStringField(TEXT("type"), TypeStr))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', 'name', or 'type'"));
    }
    Args->TryGetStringField(TEXT("direction"), DirStr);
    if (DirStr.IsEmpty()) DirStr = TEXT("input");
    const bool bIsOutput = DirStr.Equals(TEXT("output"), ESearchCase::IgnoreCase);
    if (!bIsOutput && !DirStr.Equals(TEXT("input"), ESearchCase::IgnoreCase))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("direction must be 'input' or 'output'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));
    UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph);
    if (!Entry) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("function entry node not found"));

    FString TypeObj;
    Args->TryGetStringField(TEXT("type_object"), TypeObj);
    bool bArr = false;
    Args->TryGetBoolField(TEXT("is_array"), bArr);
    FEdGraphPinType PinType = MakePinType(TypeStr, TypeObj, bArr);

    UK2Node_EditablePinBase* TargetNode = nullptr;
    // Direction relative to the underlying node:
    //   input parameter  → entry node's OUTPUT pin
    //   output parameter → result node's INPUT pin
    EEdGraphPinDirection PinDir;
    if (bIsOutput)
    {
        TargetNode = FindOrCreateFunctionResult(Graph, Entry, /*allow_create*/ true);
        if (!TargetNode) return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("could not create FunctionResult node"));
        PinDir = EGPD_Input;
    }
    else
    {
        TargetNode = Entry;
        PinDir = EGPD_Output;
    }

    // Idempotency
    const FName PName(*ParamName);
    for (const TSharedPtr<FUserPinInfo>& Info : TargetNode->UserDefinedPins)
    {
        if (Info.IsValid() && Info->PinName == PName)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("parameter already exists: %s (%s)"),
                                *ParamName, *DirStr));
        }
    }

    FScopedTransaction Tx(LOCTEXT("BpAddFnParam", "Sage: Add BP Function Parameter"));
    TargetNode->Modify();
    UEdGraphPin* NewPin = TargetNode->CreateUserDefinedPin(PName, PinType, PinDir);
    if (!NewPin)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("CreateUserDefinedPin returned nullptr"));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    R->SetStringField(TEXT("name"),      ParamName);
    R->SetStringField(TEXT("type"),      TypeStr);
    R->SetStringField(TEXT("direction"), bIsOutput ? TEXT("output") : TEXT("input"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpRemoveFunctionParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, ParamName, DirStr;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("name"), ParamName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', or 'name'"));
    }
    Args->TryGetStringField(TEXT("direction"), DirStr);
    if (DirStr.IsEmpty()) DirStr = TEXT("input");
    const bool bIsOutput = DirStr.Equals(TEXT("output"), ESearchCase::IgnoreCase);
    if (!bIsOutput && !DirStr.Equals(TEXT("input"), ESearchCase::IgnoreCase))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("direction must be 'input' or 'output'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    UK2Node_EditablePinBase* TargetNode = nullptr;
    if (bIsOutput)
    {
        TargetNode = FindOrCreateFunctionResult(Graph, nullptr, /*allow_create*/ false);
    }
    else
    {
        TargetNode = FindFunctionEntry(Graph);
    }
    if (!TargetNode)
    {
        // No entry/result node = no params to remove (idempotent).
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("blueprint"), BP->GetName());
        R->SetStringField(TEXT("function"),  FnName);
        R->SetStringField(TEXT("name"),      ParamName);
        R->SetNumberField(TEXT("removed"),   0);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    const FName PName(*ParamName);
    int32 Existed = 0;
    for (const TSharedPtr<FUserPinInfo>& Info : TargetNode->UserDefinedPins)
    {
        if (Info.IsValid() && Info->PinName == PName) { Existed = 1; break; }
    }

    if (Existed == 0)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("blueprint"), BP->GetName());
        R->SetStringField(TEXT("function"),  FnName);
        R->SetStringField(TEXT("name"),      ParamName);
        R->SetNumberField(TEXT("removed"),   0);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("BpRemFnParam", "Sage: Remove BP Function Parameter"));
    TargetNode->Modify();
    TargetNode->RemoveUserDefinedPinByName(PName);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    R->SetStringField(TEXT("name"),      ParamName);
    R->SetStringField(TEXT("direction"), bIsOutput ? TEXT("output") : TEXT("input"));
    R->SetNumberField(TEXT("removed"),   1);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.list_graphs + bp.rename_function (Phase 4.2 round 2d) -------------

FSageToolDispatch::FOutcome BpListGraphsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    auto Append = [](TArray<TSharedPtr<FJsonValue>>& Out,
                     const TArray<TObjectPtr<UEdGraph>>& Graphs,
                     const TCHAR* Kind)
    {
        for (UEdGraph* G : Graphs)
        {
            if (!G) continue;
            auto O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"),       G->GetName());
            O->SetStringField(TEXT("kind"),       Kind);
            O->SetNumberField(TEXT("node_count"), G->Nodes.Num());
            Out.Add(MakeShared<FJsonValueObject>(O));
        }
    };

    TArray<TSharedPtr<FJsonValue>> Out;
    Append(Out, BP->UbergraphPages,          TEXT("ubergraph"));
    Append(Out, BP->FunctionGraphs,          TEXT("function"));
    Append(Out, BP->DelegateSignatureGraphs, TEXT("delegate"));
    Append(Out, BP->MacroGraphs,             TEXT("macro"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetArrayField (TEXT("graphs"),    Out);
    R->SetNumberField(TEXT("count"),     Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpRenameFunctionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, OldName, NewName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("old_name"), OldName)
        || !Args->TryGetStringField(TEXT("new_name"), NewName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'old_name', or 'new_name'"));
    }
    if (OldName == NewName)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("old_name and new_name are identical"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    UEdGraph* Graph = FindFunctionGraph(BP, OldName);
    if (!Graph)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("function graph not found: %s"), *OldName));
    }
    // Reject collision: target name already exists in any graph collection.
    if (FindFunctionGraph(BP, NewName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("graph already exists with new_name: %s"), *NewName));
    }

    FScopedTransaction Tx(LOCTEXT("BpRenameFn", "Sage: Rename BP Function"));
    BP->Modify();
    Graph->Modify();
    FBlueprintEditorUtils::RenameGraph(Graph, NewName);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("old_name"),  OldName);
    R->SetStringField(TEXT("new_name"),  NewName);
    R->SetStringField(TEXT("actual"),    Graph->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.list_interfaces + bp.add_interface + bp.remove_interface ----------
// ---- (Phase 4.2 round 2c) ------------------------------------------------

UClass* ResolveInterfaceClass(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    if (UClass* C = LoadObject<UClass>(nullptr, *Path)) return C;
    FSoftObjectPath Soft(Path);
    if (UObject* O = Soft.ResolveObject()) return Cast<UClass>(O);
    if (UObject* O = Soft.TryLoad())       return Cast<UClass>(O);
    return nullptr;
}

FSageToolDispatch::FOutcome BpListInterfacesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FBPInterfaceDescription& Impl : BP->ImplementedInterfaces)
    {
        auto O = MakeShared<FJsonObject>();
        if (Impl.Interface)
        {
            O->SetStringField(TEXT("name"), Impl.Interface->GetName());
            O->SetStringField(TEXT("path"), Impl.Interface->GetPathName());
        }
        else
        {
            O->SetStringField(TEXT("name"), TEXT("<unresolved>"));
        }
        O->SetNumberField(TEXT("graph_count"), Impl.Graphs.Num());
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),  BP->GetName());
    R->SetArrayField (TEXT("interfaces"), Out);
    R->SetNumberField(TEXT("count"),      Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpAddInterfaceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, IfacePath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("interface_path"), IfacePath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'interface_path'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UClass* IfaceCls = ResolveInterfaceClass(IfacePath);
    if (!IfaceCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("interface class not found: %s"), *IfacePath));
    if (!IfaceCls->IsChildOf(UInterface::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class %s is not a UInterface"), *IfaceCls->GetName()));
    }

    // Idempotency: already implemented?
    for (const FBPInterfaceDescription& Impl : BP->ImplementedInterfaces)
    {
        if (Impl.Interface == IfaceCls)
        {
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("blueprint"),      BP->GetName());
            R->SetStringField(TEXT("interface"),      IfaceCls->GetName());
            R->SetStringField(TEXT("interface_path"), IfaceCls->GetPathName());
            R->SetBoolField  (TEXT("already"),        true);
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
    }

    FScopedTransaction Tx(LOCTEXT("BpAddInterface", "Sage: Implement BP Interface"));
    BP->Modify();
    const FTopLevelAssetPath IfaceAssetPath(IfaceCls->GetPathName());
    if (!FBlueprintEditorUtils::ImplementNewInterface(BP, IfaceAssetPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("ImplementNewInterface returned false"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),      BP->GetName());
    R->SetStringField(TEXT("interface"),      IfaceCls->GetName());
    R->SetStringField(TEXT("interface_path"), IfaceCls->GetPathName());
    R->SetBoolField  (TEXT("already"),        false);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpRemoveInterfaceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, IfacePath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("interface_path"), IfacePath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'interface_path'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UClass* IfaceCls = ResolveInterfaceClass(IfacePath);
    if (!IfaceCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("interface class not found: %s"), *IfacePath));

    bool bImplemented = false;
    for (const FBPInterfaceDescription& Impl : BP->ImplementedInterfaces)
    {
        if (Impl.Interface == IfaceCls) { bImplemented = true; break; }
    }
    if (!bImplemented)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("blueprint"), BP->GetName());
        R->SetStringField(TEXT("interface"), IfaceCls->GetName());
        R->SetNumberField(TEXT("removed"),   0);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    bool bPreserveFunctions = false;
    Args->TryGetBoolField(TEXT("preserve_functions"), bPreserveFunctions);

    FScopedTransaction Tx(LOCTEXT("BpRemoveIface", "Sage: Remove BP Interface"));
    BP->Modify();
    const FTopLevelAssetPath IfaceAssetPath(IfaceCls->GetPathName());
    FBlueprintEditorUtils::RemoveInterface(BP, IfaceAssetPath, bPreserveFunctions);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),          BP->GetName());
    R->SetStringField(TEXT("interface"),          IfaceCls->GetName());
    R->SetNumberField(TEXT("removed"),            1);
    R->SetBoolField  (TEXT("preserve_functions"), bPreserveFunctions);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.list_local_variables + bp.add_local_variable + --------------------
// ---- bp.delete_local_variable  (Phase 4.2 round 2b) -----------------------
// (FindFunctionEntry helper hoisted above r2e function-parameter handlers)

FSageToolDispatch::FOutcome BpListLocalVariablesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'function'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    TArray<TSharedPtr<FJsonValue>> Vars;
    if (UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph))
    {
        for (const FBPVariableDescription& V : Entry->LocalVariables)
        {
            Vars.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    R->SetArrayField (TEXT("variables"), Vars);
    R->SetNumberField(TEXT("count"),     Vars.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpAddLocalVariableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, VarName, TypeStr;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("name"), VarName)
        || !Args->TryGetStringField(TEXT("type"), TypeStr))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', 'name', or 'type'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    FString TypeObj;
    Args->TryGetStringField(TEXT("type_object"), TypeObj);
    bool bArr = false;
    Args->TryGetBoolField(TEXT("is_array"), bArr);
    FEdGraphPinType PinType = MakePinType(TypeStr, TypeObj, bArr);

    FString DefaultValue;
    Args->TryGetStringField(TEXT("default_value"), DefaultValue);

    FScopedTransaction Tx(LOCTEXT("BpAddLocalVar", "Sage: Add BP Local Variable"));
    BP->Modify();
    const FName VName(*VarName);

    // Pre-check duplicate; FBlueprintEditorUtils::AddLocalVariable silently
    // appends in 5.7 and we want a clear -32602 if name collides.
    if (UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph))
    {
        for (const FBPVariableDescription& V : Entry->LocalVariables)
        {
            if (V.VarName == VName)
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("local variable already exists: %s"), *VarName));
            }
        }
    }

    FBlueprintEditorUtils::AddLocalVariable(BP, Graph, VName, PinType, DefaultValue);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    R->SetStringField(TEXT("variable"),  VarName);
    R->SetStringField(TEXT("type"),      TypeStr);
    if (!DefaultValue.IsEmpty()) R->SetStringField(TEXT("default_value"), DefaultValue);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpDeleteLocalVariableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, VarName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("name"), VarName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));
    UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph);
    if (!Entry) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("function entry node not found"));

    const FName VName(*VarName);
    FScopedTransaction Tx(LOCTEXT("BpDelLocalVar", "Sage: Remove BP Local Variable"));
    Entry->Modify();
    const int32 Removed = Entry->LocalVariables.RemoveAll(
        [VName](const FBPVariableDescription& V) { return V.VarName == VName; });
    if (Removed > 0)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("function"),  FnName);
    R->SetStringField(TEXT("variable"),  VarName);
    R->SetNumberField(TEXT("removed"),   Removed);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.add_node + bp.set_node_property + bp.read_node_property + ----------
// ---- bp.list_node_types  (Phase 4.2 round 2a) ------------------------------

struct FNodeAlias { const TCHAR* Alias; const TCHAR* ClassName; };

UClass* ResolveEdGraphNodeClass(const FString& NameOrAlias)
{
    static const FNodeAlias kAliases[] = {
        {TEXT("CallFunction"), TEXT("K2Node_CallFunction")},
        {TEXT("Event"),        TEXT("K2Node_Event")},
        {TEXT("CustomEvent"),  TEXT("K2Node_CustomEvent")},
        {TEXT("GetVar"),       TEXT("K2Node_VariableGet")},
        {TEXT("SetVar"),       TEXT("K2Node_VariableSet")},
        {TEXT("Branch"),       TEXT("K2Node_IfThenElse")},
        {TEXT("If"),           TEXT("K2Node_IfThenElse")},
    };
    FString Resolved = NameOrAlias;
    for (const FNodeAlias& A : kAliases)
    {
        if (Resolved.Equals(A.Alias, ESearchCase::IgnoreCase))
        {
            Resolved = A.ClassName;
            break;
        }
    }
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->GetName() == Resolved && It->IsChildOf(UEdGraphNode::StaticClass()))
        {
            return *It;
        }
    }
    return nullptr;
}

FSageToolDispatch::FOutcome BpAddNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, NodeClass;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("node_class"), NodeClass))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', or 'node_class'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));

    UClass* NodeUClass = ResolveEdGraphNodeClass(NodeClass);
    if (!NodeUClass)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("node class not found: %s (must be UEdGraphNode subclass)"),
                            *NodeClass));
    }

    double NodeX = 0.0, NodeY = 0.0;
    Args->TryGetNumberField(TEXT("node_x"), NodeX);
    Args->TryGetNumberField(TEXT("node_y"), NodeY);

    const TSharedPtr<FJsonObject>* NodeParams = nullptr;
    Args->TryGetObjectField(TEXT("node_params"), NodeParams);

    FScopedTransaction Tx(LOCTEXT("BpAddNode", "Sage: Add BP Node"));
    Graph->Modify();

    UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeUClass);
    if (!NewNode)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("failed to NewObject node"));
    }
    NewNode->CreateNewGuid();
    NewNode->NodePosX = static_cast<int32>(NodeX);
    NewNode->NodePosY = static_cast<int32>(NodeY);

    // Special-case init BEFORE AllocateDefaultPins — pin layout depends on
    // the function reference / variable reference being set first.
    if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(NewNode))
    {
        if (NodeParams)
        {
            FString FnRef, TargetClassName;
            (*NodeParams)->TryGetStringField(TEXT("function_name"), FnRef);
            (*NodeParams)->TryGetStringField(TEXT("target_class"),  TargetClassName);

            // Allow "/Script/Engine.GameplayStatics:GetGameMode" shorthand.
            if (FnRef.Contains(TEXT(":")))
            {
                FString CP, Fn;
                FnRef.Split(TEXT(":"), &CP, &Fn);
                if (TargetClassName.IsEmpty()) TargetClassName = CP;
                FnRef = Fn;
            }

            UFunction* FoundFn = nullptr;
            if (!TargetClassName.IsEmpty())
            {
                FSoftObjectPath ClsPath(TargetClassName);
                UClass* C = Cast<UClass>(ClsPath.ResolveObject());
                if (!C) C = Cast<UClass>(ClsPath.TryLoad());
                if (C) FoundFn = C->FindFunctionByName(FName(*FnRef));
            }
            if (!FoundFn && BP->ParentClass)
            {
                FoundFn = BP->ParentClass->FindFunctionByName(FName(*FnRef));
            }
            if (FoundFn)
            {
                Call->SetFromFunction(FoundFn);
            }
        }
    }
    else if (UK2Node_VariableGet* VG = Cast<UK2Node_VariableGet>(NewNode))
    {
        if (NodeParams)
        {
            FString VarName;
            if ((*NodeParams)->TryGetStringField(TEXT("variable_name"), VarName))
            {
                VG->VariableReference.SetSelfMember(FName(*VarName));
            }
        }
    }
    else if (UK2Node_VariableSet* VS = Cast<UK2Node_VariableSet>(NewNode))
    {
        if (NodeParams)
        {
            FString VarName;
            if ((*NodeParams)->TryGetStringField(TEXT("variable_name"), VarName))
            {
                VS->VariableReference.SetSelfMember(FName(*VarName));
            }
        }
    }

    Graph->AddNode(NewNode, /*bSelectNewNode*/ false, /*bUpdateGraphCount*/ true);
    NewNode->AllocateDefaultPins();
    NewNode->PostPlacedNewNode();

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),  BP->GetName());
    R->SetStringField(TEXT("function"),   FnName);
    R->SetStringField(TEXT("node_id"),    NewNode->NodeGuid.ToString());
    R->SetStringField(TEXT("node_class"), NodeUClass->GetName());
    TArray<TSharedPtr<FJsonValue>> Pos;
    Pos.Add(MakeShared<FJsonValueNumber>(NewNode->NodePosX));
    Pos.Add(MakeShared<FJsonValueNumber>(NewNode->NodePosY));
    R->SetArrayField(TEXT("pos"), Pos);

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (UEdGraphPin* P : NewNode->Pins)
    {
        if (!P) continue;
        auto PJ = MakeShared<FJsonObject>();
        PJ->SetStringField(TEXT("name"), P->PinName.ToString());
        PJ->SetStringField(TEXT("direction"),
            P->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        PJ->SetStringField(TEXT("type"), P->PinType.PinCategory.ToString());
        Pins.Add(MakeShared<FJsonValueObject>(PJ));
    }
    R->SetArrayField(TEXT("pins"), Pins);

    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpSetNodePropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, NodeId, PinName, ValueStr;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("node_id"), NodeId)
        || !Args->TryGetStringField(TEXT("pin"), PinName)
        || !Args->TryGetStringField(TEXT("value"), ValueStr))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', 'node_id', 'pin', or 'value'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));
    UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
    if (!Node) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("node not found"));
    UEdGraphPin* Pin = FindPin(Node, PinName);
    if (!Pin) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("pin not found"));

    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("cannot set default value on execution pin (use bp.connect_pins)"));
    }

    FScopedTransaction Tx(LOCTEXT("BpSetNodeProp", "Sage: Set BP Pin Default"));
    Node->Modify();
    if (const UEdGraphSchema* Schema = Graph->GetSchema())
    {
        Schema->TrySetDefaultValue(*Pin, ValueStr);
    }
    else
    {
        Pin->DefaultValue = ValueStr;
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("node_id"),   NodeId);
    R->SetStringField(TEXT("pin"),       PinName);
    R->SetStringField(TEXT("value"),     Pin->DefaultValue);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpReadNodePropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName, NodeId, PinName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("function"), FnName)
        || !Args->TryGetStringField(TEXT("node_id"), NodeId)
        || !Args->TryGetStringField(TEXT("pin"), PinName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'function', 'node_id', or 'pin'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("graph not found"));
    UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
    if (!Node) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("node not found"));
    UEdGraphPin* Pin = FindPin(Node, PinName);
    if (!Pin) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("pin not found"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),     BP->GetName());
    R->SetStringField(TEXT("node_id"),       NodeId);
    R->SetStringField(TEXT("pin"),           PinName);
    R->SetStringField(TEXT("type"),          Pin->PinType.PinCategory.ToString());
    R->SetStringField(TEXT("direction"),
        Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    R->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    if (Pin->DefaultObject)
    {
        R->SetStringField(TEXT("default_object"),
            FSoftObjectPath(Pin->DefaultObject).ToString());
    }
    if (!Pin->DefaultTextValue.IsEmpty())
    {
        R->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
    }
    R->SetBoolField(TEXT("is_execution"),
        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
    R->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BpListNodeTypesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter;
    int32   Max = 200;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("filter"), Filter);
        double NumMax = 0;
        if (Args->TryGetNumberField(TEXT("max"), NumMax))
        {
            Max = FMath::Clamp(static_cast<int32>(NumMax), 1, 2000);
        }
    }

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Total = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* C = *It;
        if (!C->IsChildOf(UK2Node::StaticClass())) continue;
        if (C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            continue;
        const FString N = C->GetName();
        if (!Filter.IsEmpty() && !N.Contains(Filter)) continue;
        ++Total;
        if (Out.Num() >= Max) continue;
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), N);
        if (UPackage* Pkg = C->GetOuterUPackage())
        {
            O->SetStringField(TEXT("module"), Pkg->GetName());
        }
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("types"),    Out);
    R->SetNumberField(TEXT("returned"), Out.Num());
    R->SetNumberField(TEXT("total"),    Total);
    if (!Filter.IsEmpty()) R->SetStringField(TEXT("filter"), Filter);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.reparent ----------------------------------------------------------

FSageToolDispatch::FOutcome BpReparentImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, NewParent;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("new_parent_class"), NewParent))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'new_parent_class'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    FSoftObjectPath ParentPath(NewParent);
    UClass* NewClass = Cast<UClass>(ParentPath.ResolveObject());
    if (!NewClass) NewClass = Cast<UClass>(ParentPath.TryLoad());
    if (!NewClass) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("parent class not found"));

    FScopedTransaction Tx(LOCTEXT("BpReparent", "Sage: Reparent BP"));
    BP->Modify();
    BP->ParentClass = NewClass;
    FBlueprintEditorUtils::RefreshAllNodes(BP);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), BP->GetName());
    R->SetStringField(TEXT("new_parent"), NewClass->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.create_function  (new user-defined function in a Blueprint) --------

FSageToolDispatch::FOutcome BpCreateFunctionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    if (FindFunctionGraph(BP, FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("function already exists: %s"), *FnName));
    }

    FString Access = TEXT("public");
    Args->TryGetStringField(TEXT("access"), Access);

    FScopedTransaction Tx(LOCTEXT("BpCreateFn", "Sage: Create BP Function"));
    BP->Modify();
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, FName(*FnName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph,
        /*bIsUserCreated*/ true, nullptr);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("function_name"), FnName);
    R->SetStringField(TEXT("access"),        Access);
    R->SetStringField(TEXT("graph_guid"),    NewGraph->GraphGuid.ToString());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.read_graph_summary  (lightweight summary, read-only) ---------------

FSageToolDispatch::FOutcome BpReadGraphSummaryImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FnName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("fn_name"), FnName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'fn_name'"));
    }
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));
    UEdGraph* Graph = FindFunctionGraph(BP, FnName);
    if (!Graph) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("function graph not found: %s"), *FnName));

    int32 PinCount = 0;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (N) PinCount += N->Pins.Num();
    }

    // BFS exec-chain length from entry node.
    int32 ExecLen = 0;
    TSet<UEdGraphNode*> Visited;
    TArray<UEdGraphNode*> Frontier;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (Cast<UK2Node_FunctionEntry>(N) || Cast<UK2Node_Event>(N))
        {
            Frontier.Add(N);
        }
    }
    while (Frontier.Num() > 0)
    {
        TArray<UEdGraphNode*> Next;
        for (UEdGraphNode* N : Frontier)
        {
            if (!N || Visited.Contains(N)) continue;
            Visited.Add(N);
            ++ExecLen;
            for (UEdGraphPin* Pin : N->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output) continue;
                if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                for (UEdGraphPin* L : Pin->LinkedTo)
                {
                    if (L && L->GetOwningNode()) Next.Add(L->GetOwningNode());
                }
            }
        }
        Frontier = Next;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("fn_name"),           FnName);
    R->SetNumberField(TEXT("node_count"),         Graph->Nodes.Num());
    R->SetNumberField(TEXT("pin_count"),          PinCount);
    R->SetNumberField(TEXT("exec_chain_length"),  ExecLen);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.duplicate  (duplicate a Blueprint asset) ---------------------------

FSageToolDispatch::FOutcome BpDuplicateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Source, Destination;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("source"), Source)
        || !Args->TryGetStringField(TEXT("destination"), Destination))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'source' or 'destination'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    UBlueprint* OrigBP = ResolveBlueprint(Source);
    if (!OrigBP) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("source blueprint not found: %s"), *Source));

    if (ResolveBlueprint(Destination))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("destination already exists: %s"), *Destination));
    }

    FString PackagePath, AssetName;
    if (!SplitAssetPath(Destination, PackagePath, AssetName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("malformed destination path: %s"), *Destination));
    }

    FAssetToolsModule& Module = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    IAssetTools& Tools = Module.Get();

    FScopedTransaction Tx(LOCTEXT("BpDuplicate", "Sage: Duplicate Blueprint"));
    UObject* Dup = Tools.DuplicateAsset(AssetName, PackagePath, OrigBP);
    if (!Dup)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("DuplicateAsset returned nullptr"));
    }
    UBlueprint* NewBP = Cast<UBlueprint>(Dup);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("source"),      Source);
    R->SetStringField(TEXT("destination"), Dup->GetPathName());
    R->SetStringField(TEXT("class"),       NewBP && NewBP->ParentClass
                                               ? NewBP->ParentClass->GetName()
                                               : Dup->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.set_actor_tick_settings  (CDO tick config on Actor BP) -------------

FSageToolDispatch::FOutcome BpSetActorTickSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UBlueprint* BP = ResolveBlueprint(Path);
    if (!BP) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("blueprint not found"));

    if (!BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(AActor::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("blueprint is not Actor-derived"));
    }

    AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
    if (!CDO) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("CDO unavailable"));

    FScopedTransaction Tx(LOCTEXT("BpTickSettings", "Sage: Set Actor Tick Settings"));
    CDO->Modify();

    bool bCanTick = CDO->PrimaryActorTick.bCanEverTick;
    if (Args->TryGetBoolField(TEXT("can_tick"), bCanTick))
    {
        CDO->PrimaryActorTick.bCanEverTick = bCanTick;
    }

    double TickInterval = 0.0;
    if (Args->TryGetNumberField(TEXT("tick_interval"), TickInterval))
    {
        CDO->PrimaryActorTick.TickInterval = static_cast<float>(TickInterval);
    }

    FString TickGroupStr;
    if (Args->TryGetStringField(TEXT("tick_group"), TickGroupStr))
    {
        ETickingGroup Group = TG_PrePhysics;
        if      (TickGroupStr == TEXT("PrePhysics"))    Group = TG_PrePhysics;
        else if (TickGroupStr == TEXT("DuringPhysics")) Group = TG_DuringPhysics;
        else if (TickGroupStr == TEXT("PostPhysics"))   Group = TG_PostPhysics;
        else if (TickGroupStr == TEXT("PostUpdateWork"))Group = TG_PostUpdateWork;
        else
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("unknown tick_group: %s "
                    "(valid: PrePhysics/DuringPhysics/PostPhysics/PostUpdateWork)"),
                    *TickGroupStr));
        }
        CDO->PrimaryActorTick.TickGroup = Group;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),    BP->GetName());
    R->SetBoolField  (TEXT("can_tick"),     CDO->PrimaryActorTick.bCanEverTick);
    R->SetNumberField(TEXT("tick_interval"),CDO->PrimaryActorTick.TickInterval);
    R->SetNumberField(TEXT("tick_group"),   static_cast<int32>(CDO->PrimaryActorTick.TickGroup));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- bp.full_dump  (Phase 5 / Gap #6) ----------------------------------------
//
// Atomik snapshot: BP'nin tüm state'ini tek çağrıda toplar (header, variables,
// components+defaults, functions+graphs+params+locals, event_dispatchers,
// interfaces, cdo_properties, dependencies, optional T3D). Conversion-öncesi
// audit/restore reference için. Çağrı zaten game thread'de (GT wrapper),
// alt-impl'lere direct çağrı yapılır — double-marshalling yok.
//
// output_path verilirse JSON dosyaya yazılır; project-relative kabul edilir
// (FPaths::ProjectDir() altında resolve), absolute path da kabul edilir.

FSageToolDispatch::FOutcome BpFullDumpImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    bool bIncludeGraphs            = true;
    bool bIncludeT3d               = false;
    bool bIncludeDeps              = true;
    bool bIncludeComponentDefaults = true;
    Args->TryGetBoolField(TEXT("include_function_graphs"),  bIncludeGraphs);
    Args->TryGetBoolField(TEXT("include_t3d"),              bIncludeT3d);
    Args->TryGetBoolField(TEXT("include_referenced_assets"),bIncludeDeps);
    Args->TryGetBoolField(TEXT("include_component_defaults"),bIncludeComponentDefaults);

    FString OutputPath;
    Args->TryGetStringField(TEXT("output_path"), OutputPath);

    auto MakePathArgs = [&]() -> TSharedPtr<FJsonObject>
    {
        auto A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("path"), Path);
        return A;
    };

    // Helper: pull an array sub-field out of a sub-handler's Result, attach
    // under a (possibly-renamed) field on Dump. No-op if missing.
    auto AttachArray = [](TSharedPtr<FJsonObject> Dump,
                          const TSharedPtr<FJsonObject>& Result,
                          const TCHAR* SrcField,
                          const TCHAR* DstField)
    {
        if (!Dump.IsValid() || !Result.IsValid()) return;
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Result->TryGetArrayField(SrcField, Arr))
            Dump->SetArrayField(DstField, *Arr);
    };

    TSharedPtr<FJsonObject> Dump = MakeShared<FJsonObject>();
    Dump->SetStringField(TEXT("schema_version"), TEXT("1"));
    Dump->SetStringField(TEXT("captured_at"),    FDateTime::UtcNow().ToIso8601());
    Dump->SetStringField(TEXT("path"),           Path);

    // 1. source (header: parent class, generated class, BP type, ...)
    {
        auto Out = BpReadImpl(MakePathArgs());
        if (!Out.bSuccess) return Out;
        if (Out.Result.IsValid())
            Dump->SetObjectField(TEXT("source"), Out.Result);
    }

    // 2. variables
    {
        auto Out = BpListVariablesImpl(MakePathArgs());
        AttachArray(Dump, Out.Result, TEXT("variables"), TEXT("variables"));
    }

    // 3. components (+ optional per-component defaults)
    {
        auto Out = BpReadComponentsImpl(MakePathArgs());
        if (Out.bSuccess && Out.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Comps = nullptr;
            if (Out.Result->TryGetArrayField(TEXT("components"), Comps) && Comps)
            {
                if (bIncludeComponentDefaults)
                {
                    TArray<TSharedPtr<FJsonValue>> Enriched;
                    for (const auto& V : *Comps)
                    {
                        auto Obj = V.IsValid() ? V->AsObject() : nullptr;
                        if (!Obj.IsValid()) { Enriched.Add(V); continue; }
                        FString CompName;
                        Obj->TryGetStringField(TEXT("name"), CompName);
                        if (!CompName.IsEmpty())
                        {
                            auto PA = MakeShared<FJsonObject>();
                            PA->SetStringField(TEXT("path"),      Path);
                            PA->SetStringField(TEXT("component"), CompName);
                            auto P = BpReadComponentPropertiesImpl(PA);
                            if (P.bSuccess && P.Result.IsValid())
                                Obj->SetObjectField(TEXT("defaults"), P.Result);
                        }
                        Enriched.Add(MakeShared<FJsonValueObject>(Obj));
                    }
                    Dump->SetArrayField(TEXT("components"), Enriched);
                }
                else
                {
                    Dump->SetArrayField(TEXT("components"), *Comps);
                }
            }
        }
    }

    // 4. functions / graphs (+ optional graph detail, T3D, params, locals)
    {
        auto Out = BpListGraphsImpl(MakePathArgs());
        if (Out.bSuccess && Out.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
            if (Out.Result->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs)
            {
                TArray<TSharedPtr<FJsonValue>> Funcs;
                for (const auto& V : *Graphs)
                {
                    auto Obj = V.IsValid() ? V->AsObject() : nullptr;
                    if (!Obj.IsValid()) { Funcs.Add(V); continue; }
                    FString FuncName;
                    Obj->TryGetStringField(TEXT("name"), FuncName);
                    if (!FuncName.IsEmpty())
                    {
                        auto FA = MakeShared<FJsonObject>();
                        FA->SetStringField(TEXT("path"),          Path);
                        FA->SetStringField(TEXT("function"),      FuncName);
                        FA->SetStringField(TEXT("function_name"), FuncName);
                        // params
                        {
                            auto P = BpListFunctionParametersImpl(FA);
                            if (P.bSuccess && P.Result.IsValid())
                            {
                                const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
                                if (P.Result->TryGetArrayField(TEXT("parameters"), Arr) && Arr)
                                    Obj->SetArrayField(TEXT("params"), *Arr);
                            }
                        }
                        // local vars
                        {
                            auto L = BpListLocalVariablesImpl(FA);
                            if (L.bSuccess && L.Result.IsValid())
                            {
                                const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
                                if (L.Result->TryGetArrayField(TEXT("local_variables"), Arr) && Arr)
                                    Obj->SetArrayField(TEXT("local_vars"), *Arr);
                            }
                        }
                        // graph detail (nodes + pins + connections).
                        // Strip redundant inner fields — outer object already
                        // carries the function name + nodes array length.
                        if (bIncludeGraphs)
                        {
                            auto G = BpReadGraphImpl(FA);
                            if (G.bSuccess && G.Result.IsValid())
                            {
                                G.Result->RemoveField(TEXT("graph"));
                                G.Result->RemoveField(TEXT("count"));
                                Obj->SetObjectField(TEXT("graph"), G.Result);
                            }
                        }
                        // T3D node export (per-function, can be huge)
                        if (bIncludeT3d)
                        {
                            auto T = BpExportNodesT3DImpl(FA);
                            if (T.bSuccess && T.Result.IsValid())
                            {
                                FString T3dStr;
                                if (T.Result->TryGetStringField(TEXT("t3d"), T3dStr))
                                    Obj->SetStringField(TEXT("t3d"), T3dStr);
                            }
                        }
                    }
                    Funcs.Add(MakeShared<FJsonValueObject>(Obj));
                }
                Dump->SetArrayField(TEXT("functions"), Funcs);
            }
        }
    }

    // 5. event_dispatchers
    {
        auto Out = BpListEventDispatchersImpl(MakePathArgs());
        AttachArray(Dump, Out.Result, TEXT("dispatchers"),       TEXT("event_dispatchers"));
        AttachArray(Dump, Out.Result, TEXT("event_dispatchers"), TEXT("event_dispatchers"));
    }

    // 6. interfaces — already surfaced under source.interfaces by BpReadImpl;
    //    skipping the top-level duplicate to keep the dump compact.

    // 7. cdo_properties — BpGetCdoPropertiesImpl needs the generated UClass
    //    path under arg key 'class', NOT the Blueprint asset path. Resolve
    //    BP→GeneratedClass first, otherwise the call returns -32602
    //    "missing 'class'" and the dump silently loses CDO state — the
    //    most important field for pure-CDO BPs (CameraShake, DataAsset
    //    descendants, simple settings BPs).
    {
        UBlueprint* BP = ResolveBlueprint(Path);
        if (BP && BP->GeneratedClass)
        {
            auto CdoArgs = MakeShared<FJsonObject>();
            CdoArgs->SetStringField(TEXT("class"), BP->GeneratedClass->GetPathName());
            auto Out = BpGetCdoPropertiesImpl(CdoArgs);
            if (Out.bSuccess && Out.Result.IsValid())
            {
                const TSharedPtr<FJsonObject>* Sub = nullptr;
                if (Out.Result->TryGetObjectField(TEXT("properties"), Sub) && Sub && Sub->IsValid())
                    Dump->SetObjectField(TEXT("cdo_properties"), *Sub);
                else
                    Dump->SetObjectField(TEXT("cdo_properties"), Out.Result);
            }
        }
    }

    // 8. dependencies (asset + class refs)
    if (bIncludeDeps)
    {
        auto Out = BpGetDependenciesImpl(MakePathArgs());
        if (Out.bSuccess && Out.Result.IsValid())
        {
            // Strip echo fields — the dump's BP path is on the top-level,
            // and full_dump always wants forward-deps, never reverse.
            Out.Result->RemoveField(TEXT("blueprint"));
            Out.Result->RemoveField(TEXT("reverse"));
            Dump->SetObjectField(TEXT("dependencies"), Out.Result);
        }
    }

    // 9. output_path → write file. When set, the response collapses to a
    //    summary (meta + source header + counts) — full payload lives in the
    //    file. Without output_path the full dump is returned inline.
    //    Pattern: same as asset.list "capped" — avoid duplicating ~MB-sized
    //    payloads through the MCP response when the file is the source of
    //    truth (Gap #8: large BPs were eating the harness token budget).
    if (!OutputPath.IsEmpty())
    {
        FString JsonStr;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
        FJsonSerializer::Serialize(Dump.ToSharedRef(), Writer);

        FString AbsPath = OutputPath;
        if (FPaths::IsRelative(AbsPath))
            AbsPath = FPaths::ProjectDir() / AbsPath;
        FPaths::NormalizeFilename(AbsPath);

        const FString ParentDir = FPaths::GetPath(AbsPath);
        if (!ParentDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*ParentDir))
            IFileManager::Get().MakeDirectory(*ParentDir, /*Tree*/ true);

        if (!FFileHelper::SaveStringToFile(JsonStr, *AbsPath))
            return FSageToolDispatch::FOutcome::MakeError(-32000,
                FString::Printf(TEXT("could not write: %s"), *AbsPath));

        // Build summary response: meta + source header + counts. Caller
        // reads the file for full detail.
        auto CountArray = [&](const TCHAR* Field) -> int32
        {
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            return (Dump->TryGetArrayField(Field, Arr) && Arr) ? Arr->Num() : 0;
        };
        auto CountObjectFields = [&](const TCHAR* Field) -> int32
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            return (Dump->TryGetObjectField(Field, Obj) && Obj && Obj->IsValid())
                   ? (*Obj)->Values.Num() : 0;
        };

        auto Counts = MakeShared<FJsonObject>();
        Counts->SetNumberField(TEXT("variables"),         CountArray(TEXT("variables")));
        Counts->SetNumberField(TEXT("components"),        CountArray(TEXT("components")));
        Counts->SetNumberField(TEXT("functions"),         CountArray(TEXT("functions")));
        Counts->SetNumberField(TEXT("event_dispatchers"), CountArray(TEXT("event_dispatchers")));
        Counts->SetNumberField(TEXT("cdo_properties"),    CountObjectFields(TEXT("cdo_properties")));

        // Dependencies object has nested asset / referenced_classes arrays.
        const TSharedPtr<FJsonObject>* DepsObj = nullptr;
        if (Dump->TryGetObjectField(TEXT("dependencies"), DepsObj) && DepsObj && DepsObj->IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Sub = nullptr;
            if ((*DepsObj)->TryGetArrayField(TEXT("assets"), Sub) && Sub)
                Counts->SetNumberField(TEXT("dependencies"), Sub->Num());
            if ((*DepsObj)->TryGetArrayField(TEXT("referenced_classes"), Sub) && Sub)
                Counts->SetNumberField(TEXT("referenced_classes"), Sub->Num());
        }

        auto Summary = MakeShared<FJsonObject>();
        Summary->SetStringField(TEXT("schema_version"), TEXT("1"));
        FString CapturedAt;
        Dump->TryGetStringField(TEXT("captured_at"), CapturedAt);
        Summary->SetStringField(TEXT("captured_at"),  CapturedAt);
        Summary->SetStringField(TEXT("path"),         Path);
        Summary->SetStringField(TEXT("output_path"),  AbsPath);
        Summary->SetNumberField(TEXT("bytes_written"),JsonStr.Len());

        // Inline header keeps the basic shape visible without re-reading.
        const TSharedPtr<FJsonObject>* SourceObj = nullptr;
        if (Dump->TryGetObjectField(TEXT("source"), SourceObj) && SourceObj && SourceObj->IsValid())
            Summary->SetObjectField(TEXT("source"), *SourceObj);

        Summary->SetObjectField(TEXT("counts"), Counts);
        Summary->SetStringField(TEXT("note"),
            TEXT("Full payload in 'output_path'; this response collapsed to "
                 "header + counts to spare the MCP response budget. Read the "
                 "file for variables/components/functions/cdo_properties/"
                 "dependencies detail."));

        return FSageToolDispatch::FOutcome::MakeSuccess(Summary);
    }

    return FSageToolDispatch::FOutcome::MakeSuccess(Dump);
}

}  // namespace (anonymous)

void RegisterBlueprintTools(FSageToolDispatch& Dispatch)
{
    // All BP handlers must run on the game thread — UObject/UEdGraph/UClass
    // traversal is not thread-safe, and a worker-thread call into
    // FBlueprintEditorUtils crashes the editor (verified the hard way:
    // bp.add_variable on a worker thread killed UE in BlueprintEditorUtils
    // delegate broadcast). The lambda wrapper marshals to GT and waits.
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
    Dispatch.RegisterHandler(TEXT("bp.read"),                GT(&BpReadImpl));
    Dispatch.RegisterHandler(TEXT("bp.list_variables"),      GT(&BpListVariablesImpl));
    Dispatch.RegisterHandler(TEXT("bp.list_functions"),      GT(&BpListFunctionsImpl));
    Dispatch.RegisterHandler(TEXT("bp.read_function_graph"), GT(&BpReadGraphImpl));
    Dispatch.RegisterHandler(TEXT("bp.get_execution_flow"),  GT(&BpExecFlowImpl));
    Dispatch.RegisterHandler(TEXT("bp.read_components"),     GT(&BpReadComponentsImpl));
    Dispatch.RegisterHandler(TEXT("bp.search_nodes"),        GT(&BpSearchNodesImpl));

    // Write — variables (member + local)
    Dispatch.RegisterHandler(TEXT("bp.add_variable"),         GT(&BpAddVariableImpl));
    Dispatch.RegisterHandler(TEXT("bp.delete_variable"),      GT(&BpDeleteVariableImpl));
    Dispatch.RegisterHandler(TEXT("bp.set_variable_default"), GT(&BpSetVariableDefaultImpl));
    Dispatch.RegisterHandler(TEXT("bp.list_local_variables"), GT(&BpListLocalVariablesImpl));
    Dispatch.RegisterHandler(TEXT("bp.add_local_variable"),   GT(&BpAddLocalVariableImpl));
    Dispatch.RegisterHandler(TEXT("bp.delete_local_variable"),GT(&BpDeleteLocalVariableImpl));

    // Read+Write — interface CRUD (Phase 4.2 round 2c)
    Dispatch.RegisterHandler(TEXT("bp.list_interfaces"),      GT(&BpListInterfacesImpl));
    Dispatch.RegisterHandler(TEXT("bp.add_interface"),        GT(&BpAddInterfaceImpl));
    Dispatch.RegisterHandler(TEXT("bp.remove_interface"),     GT(&BpRemoveInterfaceImpl));

    // Read+Write — graph management (Phase 4.2 round 2d)
    Dispatch.RegisterHandler(TEXT("bp.list_graphs"),          GT(&BpListGraphsImpl));
    Dispatch.RegisterHandler(TEXT("bp.rename_function"),      GT(&BpRenameFunctionImpl));

    // Read+Write — function parameter I/O (Phase 4.2 round 2e)
    Dispatch.RegisterHandler(TEXT("bp.list_function_parameters"),   GT(&BpListFunctionParametersImpl));
    Dispatch.RegisterHandler(TEXT("bp.add_function_parameter"),     GT(&BpAddFunctionParameterImpl));
    Dispatch.RegisterHandler(TEXT("bp.remove_function_parameter"),  GT(&BpRemoveFunctionParameterImpl));

    // Write — asset creation (Phase 4.2 round 2f)
    Dispatch.RegisterHandler(TEXT("bp.create"),                     GT(&BpCreateImpl));
    Dispatch.RegisterHandler(TEXT("bp.create_interface"),           GT(&BpCreateInterfaceImpl));

    // Read+Write — event dispatchers (Phase 4.2 round 2g/p1)
    Dispatch.RegisterHandler(TEXT("bp.list_event_dispatchers"),     GT(&BpListEventDispatchersImpl));
    Dispatch.RegisterHandler(TEXT("bp.add_event_dispatcher"),       GT(&BpAddEventDispatcherImpl));
    Dispatch.RegisterHandler(TEXT("bp.remove_event_dispatcher"),    GT(&BpRemoveEventDispatcherImpl));

    // Read+Write — T3D node clipboard (Phase 4.2 round 2g/p2)
    Dispatch.RegisterHandler(TEXT("bp.export_nodes_t3d"),           GT(&BpExportNodesT3DImpl));
    Dispatch.RegisterHandler(TEXT("bp.import_nodes_t3d"),           GT(&BpImportNodesT3DImpl));

    // Read+Write — SCS component deep CRUD (Phase 4.2 round 2g/p3)
    Dispatch.RegisterHandler(TEXT("bp.read_component_properties"),  GT(&BpReadComponentPropertiesImpl));
    Dispatch.RegisterHandler(TEXT("bp.get_component_property"),     GT(&BpGetComponentPropertyImpl));
    Dispatch.RegisterHandler(TEXT("bp.reparent_component"),         GT(&BpReparentComponentImpl));

    // Read+Write — diagnostics + dry-run (Phase 4.2 round 2g/p4)
    Dispatch.RegisterHandler(TEXT("bp.validate"),                   GT(&BpValidateImpl));
    Dispatch.RegisterHandler(TEXT("bp.run_construction_script"),    GT(&BpRunConstructionScriptImpl));

    // Read+Write — variable props + CDO + deps (Phase 4.2 round 2g/p5)
    Dispatch.RegisterHandler(TEXT("bp.set_variable_properties"),    GT(&BpSetVariablePropertiesImpl));
    Dispatch.RegisterHandler(TEXT("bp.get_cdo_properties"),         GT(&BpGetCdoPropertiesImpl));
    Dispatch.RegisterHandler(TEXT("bp.get_dependencies"),           GT(&BpGetDependenciesImpl));

    // Write — functions
    Dispatch.RegisterHandler(TEXT("bp.add_function"),        GT(&BpAddFunctionImpl));
    Dispatch.RegisterHandler(TEXT("bp.delete_function"),     GT(&BpDeleteFunctionImpl));

    // Write — graph
    Dispatch.RegisterHandler(TEXT("bp.delete_node"),         GT(&BpDeleteNodeImpl));
    Dispatch.RegisterHandler(TEXT("bp.connect_pins"),        GT(&BpConnectPinsImpl));
    Dispatch.RegisterHandler(TEXT("bp.add_node"),            GT(&BpAddNodeImpl));
    Dispatch.RegisterHandler(TEXT("bp.set_node_property"),   GT(&BpSetNodePropertyImpl));
    Dispatch.RegisterHandler(TEXT("bp.read_node_property"),  GT(&BpReadNodePropertyImpl));
    Dispatch.RegisterHandler(TEXT("bp.list_node_types"),     GT(&BpListNodeTypesImpl));

    // Write — class shape
    Dispatch.RegisterHandler(TEXT("bp.set_cdo_property"),    GT(&BpSetCdoPropertyImpl));
    Dispatch.RegisterHandler(TEXT("bp.reparent"),            GT(&BpReparentImpl));

    // Write — function creation + actor tick
    Dispatch.RegisterHandler(TEXT("bp.create_function"),           GT(&BpCreateFunctionImpl));
    Dispatch.RegisterHandler(TEXT("bp.set_actor_tick_settings"),   GT(&BpSetActorTickSettingsImpl));

    // Write — asset duplication
    Dispatch.RegisterHandler(TEXT("bp.duplicate"),                 GT(&BpDuplicateImpl));

    // Read — lightweight graph summary
    Dispatch.RegisterHandler(TEXT("bp.read_graph_summary"),        GT(&BpReadGraphSummaryImpl));

    // Compile
    Dispatch.RegisterHandler(TEXT("bp.compile"),             GT(&BpCompileImpl));

    // Read — atomic full snapshot (Gap #6)
    Dispatch.RegisterHandler(TEXT("bp.full_dump"),           GT(&BpFullDumpImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
