#include "Tools/SageFoliageTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "SageFoliage"

namespace sage::tools
{
namespace
{

static UClass* FindFoliageClass(const TCHAR* Name)
{
    UClass* Cls = FindObject<UClass>(nullptr, Name);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, Name);
    return Cls;
}

// ---- foliage.list_types ----------------------------------------------------

FSageToolDispatch::FOutcome FoliageListTypesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    UClass* FTCls = FindFoliageClass(TEXT("/Script/Foliage.FoliageType_InstancedStaticMesh"));

    TArray<TSharedPtr<FJsonValue>> Types;
    if (FTCls)
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*SearchPath));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(FTCls->GetClassPathName());
        TArray<FAssetData> Assets;
        ARM.Get().GetAssets(Filter, Assets);
        for (const FAssetData& D : Assets)
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"), D.AssetName.ToString());
            J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
            Types.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("types"), Types);
    R->SetNumberField(TEXT("count"), Types.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- foliage.get_settings --------------------------------------------------

FSageToolDispatch::FOutcome FoliageGetSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Props;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), It->GetName());
        TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(Obj, *It);
        if (Val) J->SetField(TEXT("value"), Val);
        Props.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Obj->GetPathName());
    R->SetArrayField (TEXT("properties"), Props);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- foliage.sample --------------------------------------------------------

FSageToolDispatch::FOutcome FoliageSampleImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("foliage instance sampling via AInstancedFoliageActor; "
             "use level.get_actors_by_class with FoliageActor class"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- foliage.paint / erase -------------------------------------------------

FSageToolDispatch::FOutcome FoliagePaintImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("foliage painting requires FEdModeFoliage; use editor.run_python "
             "with unreal.FoliageEditorSubsystem or editor foliage mode APIs"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}
FSageToolDispatch::FOutcome FoliageEraseImpl(const TSharedPtr<FJsonObject>& Args)
{ return FoliagePaintImpl(Args); }

// ---- foliage.create_type ---------------------------------------------------

FSageToolDispatch::FOutcome FoliageCreateTypeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* FTCls = FindFoliageClass(TEXT("/Script/Foliage.FoliageType_InstancedStaticMesh"));
    if (!FTCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("FoliageType_InstancedStaticMesh class not found (Foliage module required)"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, FTCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("FoliageType_InstancedStaticMesh"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- foliage.set_settings --------------------------------------------------

FSageToolDispatch::FOutcome FoliageSetSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    FScopedTransaction Tx(LOCTEXT("SetFoliage", "Set Foliage Settings"));
    Obj->Modify();
    int32 Set = 0;
    for (const auto& Pair : Args->Values)
    {
        if (Pair.Key == TEXT("path")) continue;
        FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), *Pair.Key);
        if (Prop && detail::SetUPropertyFromJson(Obj, Prop, Pair.Value)) ++Set;
    }
    Obj->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         Obj->GetPathName());
    R->SetNumberField(TEXT("fields_set"),   Set);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterFoliageTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("foliage.list_types"),   GT(&FoliageListTypesImpl));
    Dispatch.RegisterHandler(TEXT("foliage.get_settings"), GT(&FoliageGetSettingsImpl));
    Dispatch.RegisterHandler(TEXT("foliage.sample"),       GT(&FoliageSampleImpl));
    Dispatch.RegisterHandler(TEXT("foliage.paint"),        GT(&FoliagePaintImpl));
    Dispatch.RegisterHandler(TEXT("foliage.erase"),        GT(&FoliageEraseImpl));
    Dispatch.RegisterHandler(TEXT("foliage.create_type"),  GT(&FoliageCreateTypeImpl));
    Dispatch.RegisterHandler(TEXT("foliage.set_settings"), GT(&FoliageSetSettingsImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
