#include "Tools/SagePluginDomainTools.h"

#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace sage::tools
{
namespace
{

UObject* ResolvePluginDomainAsset(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Obj;
}

FString FirstPluginDomainStringArg(const TSharedPtr<FJsonObject>& Args,
                                   std::initializer_list<const TCHAR*> Names,
                                   const FString& Fallback = FString())
{
    if (!Args.IsValid()) return Fallback;
    FString Value;
    for (const TCHAR* Name : Names)
    {
        if (Args->TryGetStringField(Name, Value) && !Value.IsEmpty())
        {
            return Value;
        }
    }
    return Fallback;
}

int32 PluginDomainMaxResultsArg(const TSharedPtr<FJsonObject>& Args, int32 DefaultValue)
{
    int32 MaxResults = DefaultValue;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("max_results"), MaxResults);
        Args->TryGetNumberField(TEXT("limit"), MaxResults);
    }
    return FMath::Clamp(MaxResults, 1, 5000);
}

bool TextContainsAny(const FString& Text, const TArray<FString>& Terms)
{
    for (const FString& Term : Terms)
    {
        if (!Term.IsEmpty() && Text.Contains(Term, ESearchCase::IgnoreCase)) return true;
    }
    return false;
}

TArray<FString> DomainKeywords(const FString& Domain)
{
    if (Domain == TEXT("logicdriver"))
    {
        return {
            TEXT("LogicDriver"),
            TEXT("StateMachine"),
            TEXT("SMBlueprint"),
            TEXT("SMInstance"),
            TEXT("SMGraph"),
            TEXT("SMNode")
        };
    }
    return {
        TEXT("ComboGraph"),
        TEXT("Combo Graph"),
        TEXT("ComboNode"),
        TEXT("ComboEdge"),
        TEXT("Combo")
    };
}

TArray<FString> DomainModules(const FString& Domain)
{
    if (Domain == TEXT("logicdriver"))
    {
        return {TEXT("SMSystem"), TEXT("SMSystemEditor"), TEXT("LogicDriver"), TEXT("LogicDriverEditor")};
    }
    return {TEXT("ComboGraph"), TEXT("ComboGraphEditor")};
}

TSharedPtr<FJsonObject> DomainStatusJson(const FString& Domain)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("domain"), Domain);
    TArray<TSharedPtr<FJsonValue>> Modules;
    for (const FString& Module : DomainModules(Domain))
    {
        auto M = MakeShared<FJsonObject>();
        M->SetStringField(TEXT("name"), Module);
        M->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(FName(*Module)));
        Modules.Add(MakeShared<FJsonValueObject>(M));
    }
    R->SetArrayField(TEXT("modules"), Modules);
    return R;
}

TSharedPtr<FJsonObject> AssetDataToDomainJson(const FAssetData& Data)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("name"), Data.AssetName.ToString());
    R->SetStringField(TEXT("path"), Data.GetSoftObjectPath().ToString());
    R->SetStringField(TEXT("package"), Data.PackageName.ToString());
    R->SetStringField(TEXT("class"), Data.AssetClassPath.ToString());
    return R;
}

TSharedPtr<FJsonObject> ObjectPropertiesToDomainJson(UObject* Obj, int32 MaxProperties)
{
    auto R = MakeShared<FJsonObject>();
    if (!Obj) return R;
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetStringField(TEXT("class"), Obj->GetClass()->GetPathName());

    TArray<TSharedPtr<FJsonValue>> Props;
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Obj->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        FProperty* Prop = *It;
        if (!Prop) continue;
        if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
        auto P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Prop->GetName());
        P->SetStringField(TEXT("type"), Prop->GetCPPType());
        P->SetBoolField(TEXT("editable"), Prop->HasAnyPropertyFlags(CPF_Edit));
        TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Obj, Prop);
        P->SetField(TEXT("value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
        Props.Add(MakeShared<FJsonValueObject>(P));
        if (++Count >= MaxProperties) break;
    }
    R->SetArrayField(TEXT("properties"), Props);
    R->SetNumberField(TEXT("property_count"), Props.Num());
    return R;
}

FSageToolDispatch::FOutcome PluginDomainQueryImpl(const TSharedPtr<FJsonObject>& Args,
                                                  const FString& Domain)
{
    const FString Op = FirstPluginDomainStringArg(Args, {TEXT("op"), TEXT("action"), TEXT("operation")}, TEXT("list")).ToLower();
    const TArray<FString> Keywords = DomainKeywords(Domain);

    if (Op == TEXT("status"))
    {
        return FSageToolDispatch::FOutcome::MakeSuccess(DomainStatusJson(Domain));
    }

    if (Op == TEXT("read") || Op == TEXT("get") || Op == TEXT("inspect"))
    {
        const FString Path = FirstPluginDomainStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("graph")});
        if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'/'asset'"));
        UObject* Obj = ResolvePluginDomainAsset(Path);
        if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("asset not found: %s"), *Path));
        const int32 MaxProperties = PluginDomainMaxResultsArg(Args, 200);
        auto R = ObjectPropertiesToDomainJson(Obj, MaxProperties);
        R->SetObjectField(TEXT("plugin_status"), DomainStatusJson(Domain));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (Op == TEXT("list") || Op == TEXT("search") || Op == TEXT("discover") || Op == TEXT("query"))
    {
        const FString SearchPath = FirstPluginDomainStringArg(Args, {TEXT("path"), TEXT("folder"), TEXT("directory")}, TEXT("/Game"));
        const FString Query = FirstPluginDomainStringArg(Args, {TEXT("query"), TEXT("q"), TEXT("name")});
        const int32 MaxResults = PluginDomainMaxResultsArg(Args, 200);

        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*SearchPath));
        Filter.bRecursivePaths = true;
        TArray<FAssetData> Assets;
        ARM.Get().GetAssets(Filter, Assets);

        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const FAssetData& Data : Assets)
        {
            const FString Text = FString::Printf(TEXT("%s %s %s"),
                *Data.AssetName.ToString(),
                *Data.GetSoftObjectPath().ToString(),
                *Data.AssetClassPath.ToString());
            const bool bDomainMatch = TextContainsAny(Text, Keywords);
            const bool bQueryMatch = Query.IsEmpty() || Text.Contains(Query, ESearchCase::IgnoreCase);
            if (!bDomainMatch || !bQueryMatch) continue;
            Rows.Add(MakeShared<FJsonValueObject>(AssetDataToDomainJson(Data)));
            if (Rows.Num() >= MaxResults) break;
        }

        auto R = DomainStatusJson(Domain);
        R->SetStringField(TEXT("search_path"), SearchPath);
        if (!Query.IsEmpty()) R->SetStringField(TEXT("query"), Query);
        R->SetArrayField(TEXT("assets"), Rows);
        R->SetNumberField(TEXT("count"), Rows.Num());
        R->SetBoolField(TEXT("capped"), Rows.Num() >= MaxResults);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("%s_query op '%s' requires the vendor plugin graph SDK; supported safe ops are status, list/search/query, and read/inspect"),
            *Domain, *Op));
}

FSageToolDispatch::FOutcome LogicDriverQueryImpl(const TSharedPtr<FJsonObject>& Args)
{
    return PluginDomainQueryImpl(Args, TEXT("logicdriver"));
}

FSageToolDispatch::FOutcome ComboGraphQueryImpl(const TSharedPtr<FJsonObject>& Args)
{
    return PluginDomainQueryImpl(Args, TEXT("combograph"));
}

}  // namespace

void RegisterPluginDomainTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("logicdriver_query"), GT(&LogicDriverQueryImpl));
    Dispatch.RegisterHandler(TEXT("combograph_query"), GT(&ComboGraphQueryImpl));
}

}  // namespace sage::tools
