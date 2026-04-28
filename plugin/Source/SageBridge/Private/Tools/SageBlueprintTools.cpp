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
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "ScopedTransaction.h"
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

    FEdGraphPinType PinType;
    PinType.PinCategory = FName(*TypeStr);
    if (Args->HasField(TEXT("type_object")))
    {
        FString TypeObj;
        Args->TryGetStringField(TEXT("type_object"), TypeObj);
        PinType.PinSubCategoryObject = FindObject<UObject>(nullptr, *TypeObj);
    }
    if (Args->HasField(TEXT("is_array")))
    {
        bool bArr = false;
        Args->TryGetBoolField(TEXT("is_array"), bArr);
        if (bArr) PinType.ContainerType = EPinContainerType::Array;
    }

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

    FEdGraphPinType PinType;
    PinType.PinCategory = FName(*TypeStr);
    if (Args->HasField(TEXT("type_object")))
    {
        FString TypeObj;
        Args->TryGetStringField(TEXT("type_object"), TypeObj);
        PinType.PinSubCategoryObject = FindObject<UObject>(nullptr, *TypeObj);
    }
    if (Args->HasField(TEXT("is_array")))
    {
        bool bArr = false;
        Args->TryGetBoolField(TEXT("is_array"), bArr);
        if (bArr) PinType.ContainerType = EPinContainerType::Array;
    }
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

    // Compile
    Dispatch.RegisterHandler(TEXT("bp.compile"),             GT(&BpCompileImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
