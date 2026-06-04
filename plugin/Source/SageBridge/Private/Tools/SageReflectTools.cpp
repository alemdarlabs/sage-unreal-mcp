#include "Tools/SageReflectTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace sage::tools
{
namespace
{

// ---- helpers --------------------------------------------------------------

bool IsChurnClassName(const FString& Name)
{
    return Name.StartsWith(TEXT("SKEL_"))
        || Name.StartsWith(TEXT("REINST_"))
        || Name.StartsWith(TEXT("HOTRELOADED_"))
        || Name.StartsWith(TEXT("TRASHCLASS_"))
        || Name.StartsWith(TEXT("PLACEHOLDER-"))
        || Name.StartsWith(TEXT("PROTO_BP_"));
}

FString ModuleNameOf(const UObject* Obj)
{
    if (!Obj) return {};
    if (UPackage* Pkg = Obj->GetOutermost())
    {
        const FString PkgName = Pkg->GetName();
        int32 SlashIdx = INDEX_NONE;
        if (PkgName.FindLastChar('/', SlashIdx))
        {
            return PkgName.Mid(SlashIdx + 1);
        }
        return PkgName;
    }
    return {};
}

UObject* ResolveByPath(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    if (UObject* Obj = Soft.ResolveObject()) return Obj;
    return Soft.TryLoad();
}

UClass* ResolveClass(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    if (UObject* Obj = ResolveByPath(Path))
    {
        if (UClass* Cls = Cast<UClass>(Obj)) return Cls;
    }
    // Some users pass /Script/Engine.Pawn (TopLevelAssetPath form).
    if (UClass* Cls = FindObject<UClass>(nullptr, *Path)) return Cls;
    return nullptr;
}

UScriptStruct* ResolveStruct(const FString& Path)
{
    if (UObject* Obj = ResolveByPath(Path))
    {
        if (UScriptStruct* S = Cast<UScriptStruct>(Obj)) return S;
    }
    return FindObject<UScriptStruct>(nullptr, *Path);
}

UEnum* ResolveEnum(const FString& Path)
{
    if (UObject* Obj = ResolveByPath(Path))
    {
        if (UEnum* E = Cast<UEnum>(Obj)) return E;
    }
    return FindObject<UEnum>(nullptr, *Path);
}

// ---- property reflection --------------------------------------------------

TArray<FString> PropertyAccessFlags(const FProperty* P)
{
    TArray<FString> Out;
    if (P->HasAnyPropertyFlags(CPF_Edit))             Out.Add(TEXT("EditAnywhere"));
    if (P->HasAnyPropertyFlags(CPF_DisableEditOnInstance)) Out.Add(TEXT("EditDefaultsOnly"));
    if (P->HasAnyPropertyFlags(CPF_DisableEditOnTemplate)) Out.Add(TEXT("EditInstanceOnly"));
    if (P->HasAnyPropertyFlags(CPF_BlueprintReadOnly)) Out.Add(TEXT("BlueprintReadOnly"));
    if (P->HasAnyPropertyFlags(CPF_BlueprintVisible))  Out.Add(TEXT("BlueprintVisible"));
    if (P->HasAnyPropertyFlags(CPF_BlueprintAssignable)) Out.Add(TEXT("BlueprintAssignable"));
    if (P->HasAnyPropertyFlags(CPF_BlueprintCallable))   Out.Add(TEXT("BlueprintCallable"));
    if (P->HasAnyPropertyFlags(CPF_Net))               Out.Add(TEXT("Replicated"));
    if (P->HasAnyPropertyFlags(CPF_RepNotify))         Out.Add(TEXT("RepNotify"));
    if (P->HasAnyPropertyFlags(CPF_Transient))         Out.Add(TEXT("Transient"));
    if (P->HasAnyPropertyFlags(CPF_Config))            Out.Add(TEXT("Config"));
    if (P->HasAnyPropertyFlags(CPF_SaveGame))          Out.Add(TEXT("SaveGame"));
    return Out;
}

TSharedRef<FJsonObject> ReflectProperty(const FProperty* P)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), P->GetName());
    Obj->SetStringField(TEXT("type"), P->GetCPPType());
    Obj->SetStringField(TEXT("class"), P->GetClass()->GetName());

    if (const FArrayProperty* AP = CastField<FArrayProperty>(P))
    {
        if (AP->Inner)
        {
            Obj->SetStringField(TEXT("inner_type"), AP->Inner->GetCPPType());
            Obj->SetStringField(TEXT("inner_class"), AP->Inner->GetClass()->GetName());
        }
    }
    else if (const FSetProperty* SP = CastField<FSetProperty>(P))
    {
        if (SP->ElementProp)
        {
            Obj->SetStringField(TEXT("inner_type"), SP->ElementProp->GetCPPType());
            Obj->SetStringField(TEXT("inner_class"), SP->ElementProp->GetClass()->GetName());
        }
    }
    else if (const FMapProperty* MP = CastField<FMapProperty>(P))
    {
        if (MP->KeyProp && MP->ValueProp)
        {
            Obj->SetStringField(TEXT("key_type"),   MP->KeyProp->GetCPPType());
            Obj->SetStringField(TEXT("value_type"), MP->ValueProp->GetCPPType());
        }
    }
    else if (const FObjectProperty* OP = CastField<FObjectProperty>(P))
    {
        if (OP->PropertyClass)
        {
            Obj->SetStringField(TEXT("ref_class"),
                FSoftObjectPath(OP->PropertyClass).ToString());
        }
    }
    else if (const FStructProperty* StP = CastField<FStructProperty>(P))
    {
        if (StP->Struct)
        {
            Obj->SetStringField(TEXT("struct"), StP->Struct->GetName());
        }
    }
    else if (const FByteProperty* BP = CastField<FByteProperty>(P))
    {
        if (BP->Enum)
        {
            Obj->SetStringField(TEXT("enum"), BP->Enum->GetName());
        }
    }
    else if (const FEnumProperty* EP = CastField<FEnumProperty>(P))
    {
        if (EP->GetEnum())
        {
            Obj->SetStringField(TEXT("enum"), EP->GetEnum()->GetName());
        }
    }

    const FString Category = P->GetMetaData(TEXT("Category"));
    if (!Category.IsEmpty()) Obj->SetStringField(TEXT("category"), Category);
    const FString Tooltip  = P->GetMetaData(TEXT("Tooltip"));
    if (!Tooltip.IsEmpty())  Obj->SetStringField(TEXT("tooltip"), Tooltip);

    TArray<FString> Flags = PropertyAccessFlags(P);
    if (Flags.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> FlagJson;
        for (const auto& F : Flags) FlagJson.Add(MakeShared<FJsonValueString>(F));
        Obj->SetArrayField(TEXT("flags"), FlagJson);
    }
    return Obj;
}

TSharedRef<FJsonObject> ReflectFunction(const UFunction* F)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), F->GetName());

    TArray<TSharedPtr<FJsonValue>> Params;
    TSharedPtr<FJsonObject> Return;
    for (TFieldIterator<FProperty> It(F); It; ++It)
    {
        FProperty* P = *It;
        const bool bIsParm   = P->HasAnyPropertyFlags(CPF_Parm);
        const bool bIsReturn = P->HasAnyPropertyFlags(CPF_ReturnParm);
        if (!bIsParm && !bIsReturn) continue;

        auto P_ = ReflectProperty(P);
        if (P->HasAnyPropertyFlags(CPF_OutParm) && !P->HasAnyPropertyFlags(CPF_ConstParm))
        {
            P_->SetBoolField(TEXT("is_out"), true);
        }
        if (P->HasAnyPropertyFlags(CPF_ReferenceParm))
        {
            P_->SetBoolField(TEXT("is_ref"), true);
        }
        if (bIsReturn)
        {
            Return = P_;
        }
        else
        {
            Params.Add(MakeShared<FJsonValueObject>(P_));
        }
    }
    Obj->SetArrayField(TEXT("parameters"), Params);
    if (Return.IsValid())
    {
        Obj->SetObjectField(TEXT("return"), Return);
    }

    TArray<FString> Flags;
    if (F->HasAnyFunctionFlags(FUNC_BlueprintCallable)) Flags.Add(TEXT("BlueprintCallable"));
    if (F->HasAnyFunctionFlags(FUNC_BlueprintPure))     Flags.Add(TEXT("BlueprintPure"));
    if (F->HasAnyFunctionFlags(FUNC_BlueprintEvent))    Flags.Add(TEXT("BlueprintEvent"));
    if (F->HasAnyFunctionFlags(FUNC_Native))            Flags.Add(TEXT("Native"));
    if (F->HasAnyFunctionFlags(FUNC_Static))            Flags.Add(TEXT("Static"));
    if (F->HasAnyFunctionFlags(FUNC_Public))            Flags.Add(TEXT("Public"));
    if (F->HasAnyFunctionFlags(FUNC_Protected))         Flags.Add(TEXT("Protected"));
    if (F->HasAnyFunctionFlags(FUNC_Private))           Flags.Add(TEXT("Private"));
    if (F->HasAnyFunctionFlags(FUNC_Net))               Flags.Add(TEXT("Net"));
    if (F->HasAnyFunctionFlags(FUNC_NetServer))         Flags.Add(TEXT("NetServer"));
    if (F->HasAnyFunctionFlags(FUNC_NetClient))         Flags.Add(TEXT("NetClient"));
    if (F->HasAnyFunctionFlags(FUNC_NetMulticast))      Flags.Add(TEXT("NetMulticast"));
    if (F->HasAnyFunctionFlags(FUNC_NetReliable))       Flags.Add(TEXT("Reliable"));
    if (F->HasAnyFunctionFlags(FUNC_Exec))              Flags.Add(TEXT("Exec"));
    if (Flags.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> FlagJson;
        for (const auto& F2 : Flags) FlagJson.Add(MakeShared<FJsonValueString>(F2));
        Obj->SetArrayField(TEXT("flags"), FlagJson);
    }

    const FString Tooltip = F->GetMetaData(TEXT("Tooltip"));
    if (!Tooltip.IsEmpty()) Obj->SetStringField(TEXT("tooltip"), Tooltip);
    const FString Category = F->GetMetaData(TEXT("Category"));
    if (!Category.IsEmpty()) Obj->SetStringField(TEXT("category"), Category);

    return Obj;
}

// ---- reflect_class --------------------------------------------------------

FSageToolDispatch::FOutcome ReflectClassImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString ClassPath;
    if (!Args->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'class_path'"));
    }

    UClass* Cls = ResolveClass(ClassPath);
    if (!Cls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class not found: %s"), *ClassPath));
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"),     Cls->GetName());
    Result->SetStringField(TEXT("path"),     FSoftObjectPath(Cls).ToString());
    Result->SetStringField(TEXT("module"),   ModuleNameOf(Cls));
    Result->SetBoolField  (TEXT("is_native"),    Cls->HasAnyClassFlags(CLASS_Native));
    Result->SetBoolField  (TEXT("is_abstract"),  Cls->HasAnyClassFlags(CLASS_Abstract));
    Result->SetBoolField  (TEXT("is_interface"), Cls->HasAnyClassFlags(CLASS_Interface));
    Result->SetBoolField  (TEXT("is_blueprint"),
                            !Cls->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic));
    Result->SetBoolField  (TEXT("is_deprecated"), Cls->HasAnyClassFlags(CLASS_Deprecated));
    if (UClass* Super = Cls->GetSuperClass())
    {
        Result->SetStringField(TEXT("parent"),      Super->GetName());
        Result->SetStringField(TEXT("parent_path"), FSoftObjectPath(Super).ToString());
    }

    // Interfaces
    {
        TArray<TSharedPtr<FJsonValue>> Iface;
        for (const FImplementedInterface& Imp : Cls->Interfaces)
        {
            if (Imp.Class)
            {
                Iface.Add(MakeShared<FJsonValueString>(Imp.Class->GetName()));
            }
        }
        Result->SetArrayField(TEXT("interfaces"), Iface);
    }

    // Properties (only this class — caller chains to parent via reflect_class
    // on the parent if they want the inherited set).
    {
        TArray<TSharedPtr<FJsonValue>> Props;
        for (TFieldIterator<FProperty> It(Cls, EFieldIterationFlags::None); It; ++It)
        {
            Props.Add(MakeShared<FJsonValueObject>(ReflectProperty(*It)));
        }
        Result->SetArrayField(TEXT("properties"), Props);
    }

    // Functions
    {
        TArray<TSharedPtr<FJsonValue>> Fns;
        for (TFieldIterator<UFunction> It(Cls, EFieldIterationFlags::None); It; ++It)
        {
            Fns.Add(MakeShared<FJsonValueObject>(ReflectFunction(*It)));
        }
        Result->SetArrayField(TEXT("functions"), Fns);
    }

    // Immediate children (one hop). Use list_classes with base_class for broader walks.
    {
        const bool bIncludeChildren = Args->GetBoolField(TEXT("include_children"))
                                   || !Args->HasField(TEXT("include_children"));
        if (bIncludeChildren)
        {
            TArray<TSharedPtr<FJsonValue>> Children;
            int32 Capped = 0;
            for (TObjectIterator<UClass> It; It && Capped < 200; ++It)
            {
                UClass* C = *It;
                if (!C || C->GetSuperClass() != Cls) continue;
                if (IsChurnClassName(C->GetName())) continue;
                Children.Add(MakeShared<FJsonValueString>(C->GetName()));
                ++Capped;
            }
            Result->SetArrayField(TEXT("children"), Children);
        }
    }

    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- reflect_struct -------------------------------------------------------

FSageToolDispatch::FOutcome ReflectStructImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString StructPath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("struct_path"), StructPath)
        || StructPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'struct_path'"));
    }
    UScriptStruct* S = ResolveStruct(StructPath);
    if (!S)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("struct not found: %s"), *StructPath));
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"),   S->GetName());
    Result->SetStringField(TEXT("path"),   FSoftObjectPath(S).ToString());
    Result->SetStringField(TEXT("module"), ModuleNameOf(S));
    if (UScriptStruct* Super = Cast<UScriptStruct>(S->GetSuperStruct()))
    {
        Result->SetStringField(TEXT("parent"), Super->GetName());
    }

    TArray<TSharedPtr<FJsonValue>> Fields;
    for (TFieldIterator<FProperty> It(S, EFieldIterationFlags::None); It; ++It)
    {
        Fields.Add(MakeShared<FJsonValueObject>(ReflectProperty(*It)));
    }
    Result->SetArrayField(TEXT("fields"), Fields);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- reflect_enum ---------------------------------------------------------

FSageToolDispatch::FOutcome ReflectEnumImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString EnumPath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("enum_path"), EnumPath)
        || EnumPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'enum_path'"));
    }
    UEnum* E = ResolveEnum(EnumPath);
    if (!E)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("enum not found: %s"), *EnumPath));
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"),   E->GetName());
    Result->SetStringField(TEXT("path"),   FSoftObjectPath(E).ToString());
    Result->SetStringField(TEXT("module"), ModuleNameOf(E));
    Result->SetStringField(TEXT("cpp_form"),
        E->GetCppForm() == UEnum::ECppForm::EnumClass ? TEXT("EnumClass") :
        E->GetCppForm() == UEnum::ECppForm::Namespaced ? TEXT("Namespaced") : TEXT("Regular"));

    TArray<TSharedPtr<FJsonValue>> Entries;
    const int32 N = E->NumEnums();
    for (int32 i = 0; i < N; ++i)
    {
        // Skip the synthetic _MAX entry that UE appends.
        const FString NameStr = E->GetNameStringByIndex(i);
        if (NameStr.EndsWith(TEXT("_MAX"))) continue;

        auto Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"),         NameStr);
        Entry->SetStringField(TEXT("display_name"), E->GetDisplayNameTextByIndex(i).ToString());
        Entry->SetNumberField(TEXT("value"),        static_cast<double>(E->GetValueByIndex(i)));
        const FString Tooltip = E->GetToolTipTextByIndex(i).ToString();
        if (!Tooltip.IsEmpty()) Entry->SetStringField(TEXT("tooltip"), Tooltip);
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Result->SetArrayField(TEXT("entries"), Entries);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- list_classes / list_structs / list_enums -----------------------------

template <typename TIter>
TArray<TSharedPtr<FJsonValue>> CollectListing(
    const FString& Filter,
    int32 MaxResults,
    TFunctionRef<void(typename TIter::ElementType*)> Visitor)
{
    return {};  // stub to satisfy templates — actual fns below use the ranges directly
}

FSageToolDispatch::FOutcome ListClassesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter;
    Args.IsValid() && Args->TryGetStringField(TEXT("filter"), Filter);
    FString BaseClassPath;
    Args.IsValid() && Args->TryGetStringField(TEXT("base_class"), BaseClassPath);
    UClass* Base = BaseClassPath.IsEmpty() ? nullptr : ResolveClass(BaseClassPath);
    int32 MaxResults = 500;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("max_results"), MaxResults);
    MaxResults = FMath::Clamp(MaxResults, 1, 5000);

    bool IncludeNative    = true;
    bool IncludeBlueprint = true;
    if (Args.IsValid())
    {
        bool b;
        if (Args->TryGetBoolField(TEXT("include_native"),    b)) IncludeNative    = b;
        if (Args->TryGetBoolField(TEXT("include_blueprint"), b)) IncludeBlueprint = b;
    }

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Count = 0;
    for (TObjectIterator<UClass> It; It && Count < MaxResults; ++It)
    {
        UClass* C = *It;
        if (!C) continue;
        const FString Name = C->GetName();
        if (IsChurnClassName(Name)) continue;
        if (!Filter.IsEmpty() && !Name.Contains(Filter)) continue;
        if (Base && !C->IsChildOf(Base)) continue;

        const bool bIsNative = C->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic);
        if (bIsNative && !IncludeNative) continue;
        if (!bIsNative && !IncludeBlueprint) continue;

        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"),   Name);
        Row->SetStringField(TEXT("path"),   FSoftObjectPath(C).ToString());
        Row->SetStringField(TEXT("module"), ModuleNameOf(C));
        Row->SetBoolField  (TEXT("is_native"), bIsNative);
        if (UClass* Super = C->GetSuperClass())
        {
            Row->SetStringField(TEXT("parent"), Super->GetName());
        }
        Out.Add(MakeShared<FJsonValueObject>(Row));
        ++Count;
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("classes"), Out);
    Result->SetNumberField(TEXT("count"),  Out.Num());
    Result->SetBoolField(TEXT("truncated"), Count == MaxResults);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

FSageToolDispatch::FOutcome ListStructsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter;
    Args.IsValid() && Args->TryGetStringField(TEXT("filter"), Filter);
    int32 MaxResults = 500;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("max_results"), MaxResults);
    MaxResults = FMath::Clamp(MaxResults, 1, 5000);

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Count = 0;
    for (TObjectIterator<UScriptStruct> It; It && Count < MaxResults; ++It)
    {
        UScriptStruct* S = *It;
        if (!S) continue;
        const FString Name = S->GetName();
        if (!Filter.IsEmpty() && !Name.Contains(Filter)) continue;

        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"),   Name);
        Row->SetStringField(TEXT("path"),   FSoftObjectPath(S).ToString());
        Row->SetStringField(TEXT("module"), ModuleNameOf(S));
        Out.Add(MakeShared<FJsonValueObject>(Row));
        ++Count;
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("structs"), Out);
    Result->SetNumberField(TEXT("count"),  Out.Num());
    Result->SetBoolField(TEXT("truncated"), Count == MaxResults);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

FSageToolDispatch::FOutcome ListEnumsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter;
    Args.IsValid() && Args->TryGetStringField(TEXT("filter"), Filter);
    int32 MaxResults = 500;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("max_results"), MaxResults);
    MaxResults = FMath::Clamp(MaxResults, 1, 5000);

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Count = 0;
    for (TObjectIterator<UEnum> It; It && Count < MaxResults; ++It)
    {
        UEnum* E = *It;
        if (!E) continue;
        const FString Name = E->GetName();
        if (!Filter.IsEmpty() && !Name.Contains(Filter)) continue;

        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"),   Name);
        Row->SetStringField(TEXT("path"),   FSoftObjectPath(E).ToString());
        Row->SetStringField(TEXT("module"), ModuleNameOf(E));
        Row->SetNumberField(TEXT("entry_count"), E->NumEnums());
        Out.Add(MakeShared<FJsonValueObject>(Row));
        ++Count;
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("enums"), Out);
    Result->SetNumberField(TEXT("count"), Out.Num());
    Result->SetBoolField(TEXT("truncated"), Count == MaxResults);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- find_implementers ----------------------------------------------------

FSageToolDispatch::FOutcome FindImplementersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString IfacePath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("interface_path"), IfacePath)
        || IfacePath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'interface_path'"));
    }
    UClass* Iface = ResolveClass(IfacePath);
    if (!Iface)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("interface not found: %s"), *IfacePath));
    }
    if (!Iface->HasAnyClassFlags(CLASS_Interface))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class is not an interface: %s"), *IfacePath));
    }

    int32 MaxResults = 500;
    if (Args->TryGetNumberField(TEXT("max_results"), MaxResults)) {}
    MaxResults = FMath::Clamp(MaxResults, 1, 5000);

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Count = 0;
    for (TObjectIterator<UClass> It; It && Count < MaxResults; ++It)
    {
        UClass* C = *It;
        if (!C || C == Iface) continue;
        if (IsChurnClassName(C->GetName())) continue;
        if (!C->ImplementsInterface(Iface)) continue;

        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"),   C->GetName());
        Row->SetStringField(TEXT("path"),   FSoftObjectPath(C).ToString());
        Row->SetStringField(TEXT("module"), ModuleNameOf(C));
        Row->SetBoolField  (TEXT("is_native"), C->HasAnyClassFlags(CLASS_Native));
        Out.Add(MakeShared<FJsonValueObject>(Row));
        ++Count;
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("interface"), Iface->GetName());
    Result->SetArrayField (TEXT("implementers"), Out);
    Result->SetNumberField(TEXT("count"), Out.Num());
    Result->SetBoolField  (TEXT("truncated"), Count == MaxResults);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- class_default_object -------------------------------------------------

FSageToolDispatch::FOutcome ClassDefaultObjectImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ClassPath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("class_path"), ClassPath)
        || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'class_path'"));
    }
    UClass* Cls = ResolveClass(ClassPath);
    if (!Cls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class not found: %s"), *ClassPath));
    }
    UObject* CDO = Cls->GetDefaultObject(/*bCreateIfNeeded=*/true);
    if (!CDO)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("CDO unavailable (abstract class?)"));
    }

    auto Props = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Cls, EFieldIterationFlags::Default); It; ++It)
    {
        FProperty* P = *It;
        // Skip transient/editor-only/deprecated for the agent surface.
        if (P->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
        TSharedPtr<FJsonValue> V = detail::GetUPropertyAsJson(CDO, P);
        if (V.IsValid())
        {
            Props->SetField(P->GetName(), V);
        }
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("class"), Cls->GetName());
    Result->SetStringField(TEXT("path"),  FSoftObjectPath(Cls).ToString());
    Result->SetObjectField(TEXT("properties"), Props);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- reflection.list_tags --------------------------------------------------

FSageToolDispatch::FOutcome ListTagsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("filter"), Filter);

    // UGameplayTagsManager is in GameplayTags module — use soft lookup
    // We access via GConfig from DefaultGameplayTags.ini
    TArray<TSharedPtr<FJsonValue>> Tags;

    TArray<FString> TagLines;
    FString ConfigPath = FPaths::ProjectConfigDir() / TEXT("DefaultGameplayTags.ini");
    if (FPaths::FileExists(ConfigPath))
    {
        GConfig->GetArray(TEXT("/Script/GameplayTags.GameplayTagsList"),
            TEXT("GameplayTagList"), TagLines, ConfigPath);
    }

    // Also check DefaultEngine.ini
    FString EngineConfigPath = FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini");
    TArray<FString> EngineTagLines;
    GConfig->GetArray(TEXT("/Script/GameplayTags.GameplayTagsList"),
        TEXT("GameplayTagList"), EngineTagLines, EngineConfigPath);
    TagLines.Append(EngineTagLines);

    for (const FString& Line : TagLines)
    {
        // Tag format: (TagName="Category.Tag",DevComment="",bRestrictedTag=False)
        FString TagName;
        if (FParse::Value(*Line, TEXT("TagName=\""), TagName))
        {
            int32 End = TagName.Find(TEXT("\""));
            if (End != INDEX_NONE) TagName.LeftInline(End);
            if (!Filter.IsEmpty() && !TagName.Contains(Filter, ESearchCase::IgnoreCase)) continue;
            Tags.Add(MakeShared<FJsonValueString>(TagName));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("tags"),  Tags);
    R->SetNumberField(TEXT("count"), Tags.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- reflection.create_tag -------------------------------------------------

FSageToolDispatch::FOutcome CreateTagImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString TagName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("tag"), TagName))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'tag'"));

    FString Comment;
    Args->TryGetStringField(TEXT("comment"), Comment);

    // Write to DefaultGameplayTags.ini
    FString ConfigPath = FPaths::ProjectConfigDir() / TEXT("DefaultGameplayTags.ini");
    FString TagEntry = FString::Printf(
        TEXT("(TagName=\"%s\",DevComment=\"%s\",bRestrictedTag=False)"),
        *TagName, *Comment);

    TArray<FString> Existing;
    GConfig->GetArray(TEXT("/Script/GameplayTags.GameplayTagsList"),
        TEXT("GameplayTagList"), Existing, ConfigPath);

    // Check duplicate
    for (const FString& E : Existing)
    {
        if (E.Contains(TagName))
        {
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("tag"),           TagName);
            R->SetBoolField  (TEXT("already_exists"), true);
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
    }

    Existing.Add(TagEntry);
    GConfig->SetArray(TEXT("/Script/GameplayTags.GameplayTagsList"),
        TEXT("GameplayTagList"), Existing, ConfigPath);
    GConfig->Flush(false, ConfigPath);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("tag"),     TagName);
    R->SetBoolField  (TEXT("created"), true);
    R->SetStringField(TEXT("config"),  ConfigPath);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterReflectTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("reflect_class"),       &ReflectClassImpl);
    Dispatch.RegisterHandler(TEXT("reflect_struct"),      &ReflectStructImpl);
    Dispatch.RegisterHandler(TEXT("reflect_enum"),        &ReflectEnumImpl);
    Dispatch.RegisterHandler(TEXT("list_classes"),        &ListClassesImpl);
    Dispatch.RegisterHandler(TEXT("list_structs"),        &ListStructsImpl);
    Dispatch.RegisterHandler(TEXT("list_enums"),          &ListEnumsImpl);
    Dispatch.RegisterHandler(TEXT("find_implementers"),   &FindImplementersImpl);
    Dispatch.RegisterHandler(TEXT("class_default_object"),&ClassDefaultObjectImpl);
    Dispatch.RegisterHandler(TEXT("reflection.list_tags"), &ListTagsImpl);
    Dispatch.RegisterHandler(TEXT("reflection.create_tag"),&CreateTagImpl);
}

}  // namespace sage::tools
