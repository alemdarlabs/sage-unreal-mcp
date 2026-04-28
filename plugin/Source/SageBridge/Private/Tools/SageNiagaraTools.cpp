#include "Tools/SageNiagaraTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "SageNiagara"

namespace sage::tools
{
namespace
{

// Lazy class lookup helper
static UClass* FindNiagaraClass(const TCHAR* Name)
{
    UClass* Cls = FindObject<UClass>(nullptr, Name);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, Name);
    return Cls;
}

static FSageToolDispatch::FOutcome NiagaraNotAvailable()
{
    return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("Niagara plugin required; class not found in this project"));
}

static TArray<FAssetData> ListNiagaraAssets(const FString& ClassPath,
                                              const FString& SearchPath,
                                              bool bRecursive = true)
{
    TArray<FAssetData> Assets;
    UClass* Cls = FindNiagaraClass(*ClassPath);
    if (!Cls) return Assets;
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = bRecursive;
    Filter.ClassPaths.Add(Cls->GetClassPathName());
    ARM.Get().GetAssets(Filter, Assets);
    return Assets;
}

// ---- niagara.list ----------------------------------------------------------

FSageToolDispatch::FOutcome NiagaraListImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    TArray<FAssetData> Systems = ListNiagaraAssets(
        TEXT("/Script/Niagara.NiagaraSystem"), SearchPath);
    TArray<FAssetData> Emitters = ListNiagaraAssets(
        TEXT("/Script/Niagara.NiagaraEmitter"), SearchPath);

    TArray<TSharedPtr<FJsonValue>> Assets;
    auto Add = [&](const TArray<FAssetData>& Arr, const FString& Type)
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
    Add(Systems,  TEXT("NiagaraSystem"));
    Add(Emitters, TEXT("NiagaraEmitter"));

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("assets"), Assets);
    R->SetNumberField(TEXT("count"),  Assets.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.get_info ------------------------------------------------------

FSageToolDispatch::FOutcome NiagaraGetInfoImpl(const TSharedPtr<FJsonObject>& Args)
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

// ---- niagara.spawn ---------------------------------------------------------

FSageToolDispatch::FOutcome NiagaraSpawnImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* NiagaraActorCls = FindNiagaraClass(TEXT("/Script/Niagara.NiagaraActor"));
    if (!NiagaraActorCls) return NiagaraNotAvailable();

    FVector Loc = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Loc);
    FTransform Tf(FRotator::ZeroRotator, Loc);

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FScopedTransaction Tx(LOCTEXT("SpawnNiagara", "Spawn Niagara"));
    AActor* A = GEditor->AddActor(World->GetCurrentLevel(), NiagaraActorCls, Tf);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("AddActor failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());
    R->SetStringField(TEXT("system"),   Path);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.set_parameter -------------------------------------------------

FSageToolDispatch::FOutcome NiagaraSetParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ParamName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("name"), ParamName))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    // Set via property reflection
    FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), *ParamName);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      Path);
    R->SetStringField(TEXT("parameter"), ParamName);
    if (Prop)
    {
        const TSharedPtr<FJsonValue>* ValPtr = Args->Values.Find(TEXT("value"));
        if (ValPtr && detail::SetUPropertyFromJson(Obj, Prop, *ValPtr))
        {
            Obj->MarkPackageDirty();
            R->SetBoolField(TEXT("set"), true);
        }
        else
        {
            R->SetBoolField(TEXT("set"), false);
            R->SetStringField(TEXT("note"), TEXT("property found but value type mismatch"));
        }
    }
    else
    {
        R->SetBoolField  (TEXT("set"),  false);
        R->SetStringField(TEXT("note"), TEXT("use Niagara editor to set system-level parameters"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.create --------------------------------------------------------

FSageToolDispatch::FOutcome NiagaraCreateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* NSCls = FindNiagaraClass(TEXT("/Script/Niagara.NiagaraSystem"));
    if (!NSCls) return NiagaraNotAvailable();

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, NSCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("NiagaraSystem"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.create_emitter ------------------------------------------------

FSageToolDispatch::FOutcome NiagaraCreateEmitterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* NECls = FindNiagaraClass(TEXT("/Script/Niagara.NiagaraEmitter"));
    if (!NECls) return NiagaraNotAvailable();

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, NECls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("NiagaraEmitter"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.add_emitter ---------------------------------------------------

FSageToolDispatch::FOutcome NiagaraAddEmitterImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("Adding emitters to NiagaraSystem requires NiagaraSystemEditorData APIs; "
             "use the Niagara System editor directly or via Python scripting"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.list_emitters -------------------------------------------------

FSageToolDispatch::FOutcome NiagaraListEmittersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Emitters;
    // Reflect emitter handles property
    FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), TEXT("EmitterHandles"));
    if (Prop)
    {
        TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(Obj, Prop);
        if (Val) Emitters.Add(Val);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),     Obj->GetPathName());
    R->SetArrayField (TEXT("emitters"), Emitters);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.set_emitter_property ------------------------------------------

FSageToolDispatch::FOutcome NiagaraSetEmitterPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("Emitter property mutation requires NiagaraEditorModule; "
             "use editor.run_python or the Niagara editor"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.list_modules --------------------------------------------------

FSageToolDispatch::FOutcome NiagaraListModulesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    TArray<FAssetData> Modules = ListNiagaraAssets(
        TEXT("/Script/Niagara.NiagaraScript"), SearchPath);

    TArray<TSharedPtr<FJsonValue>> Result;
    for (const FAssetData& D : Modules)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), D.AssetName.ToString());
        J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
        Result.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("modules"), Result);
    R->SetNumberField(TEXT("count"),   Result.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.get_emitter_info ----------------------------------------------

FSageToolDispatch::FOutcome NiagaraGetEmitterInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    return NiagaraGetInfoImpl(Args);
}

// ---- niagara.list_renderers ------------------------------------------------

FSageToolDispatch::FOutcome NiagaraListRenderersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Renderers;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        if (It->GetName().Contains(TEXT("Renderer")))
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("property"), It->GetName());
            Renderers.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("renderers"), Renderers);
    R->SetNumberField(TEXT("count"),     Renderers.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.add_renderer / remove_renderer / set_renderer_property --------

FSageToolDispatch::FOutcome NiagaraAddRendererImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("Renderer CRUD requires NiagaraEditorModule runtime APIs"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}
FSageToolDispatch::FOutcome NiagaraRemoveRendererImpl(const TSharedPtr<FJsonObject>& Args)
{ return NiagaraAddRendererImpl(Args); }
FSageToolDispatch::FOutcome NiagaraSetRendererPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{ return NiagaraSetEmitterPropertyImpl(Args); }

// ---- niagara.inspect_data_interfaces ---------------------------------------

FSageToolDispatch::FOutcome NiagaraInspectDataInterfacesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Path);
    R->SetStringField(TEXT("note"), TEXT("Data interface inspection via NiagaraScript properties"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.create_system_from_spec ---------------------------------------

FSageToolDispatch::FOutcome NiagaraCreateSystemFromSpecImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    // Delegate to create
    return NiagaraCreateImpl(Args);
}

// ---- niagara.get_compiled_hlsl ---------------------------------------------

FSageToolDispatch::FOutcome NiagaraGetCompiledHlslImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("compiled HLSL access requires NiagaraScript::GetCompiledData — "
             "use editor.run_python to query"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.list_system_parameters ----------------------------------------

FSageToolDispatch::FOutcome NiagaraListSystemParametersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Params;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        if (It->GetName().StartsWith(TEXT("Niagara")) ||
            It->GetName().Contains(TEXT("Parameter")))
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"), It->GetName());
            J->SetStringField(TEXT("type"), It->GetCPPType());
            Params.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),   Obj->GetPathName());
    R->SetArrayField (TEXT("params"), Params);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.list_module_inputs / set_module_input --------------------------

FSageToolDispatch::FOutcome NiagaraListModuleInputsImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("Module input enumeration requires NiagaraScript::GetInputParameters"));
    R->SetArrayField (TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome NiagaraSetModuleInputImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("Module input mutation requires NiagaraEditorModule::SetParameterOverride"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.list_static_switches / set_static_switch -----------------------

FSageToolDispatch::FOutcome NiagaraListStaticSwitchesImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"), TEXT("static switch enumeration via NiagaraScript"));
    R->SetArrayField (TEXT("switches"), TArray<TSharedPtr<FJsonValue>>());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome NiagaraSetStaticSwitchImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("static switch mutation requires NiagaraEditorModule"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- niagara.create_module_from_hlsl / create_scratch_module ---------------

FSageToolDispatch::FOutcome NiagaraCreateModuleFromHlslImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("HLSL module creation requires NiagaraEditorModule::CreateScript"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome NiagaraCreateScratchModuleImpl(const TSharedPtr<FJsonObject>& Args)
{ return NiagaraCreateModuleFromHlslImpl(Args); }

// ---- niagara.batch ---------------------------------------------------------

FSageToolDispatch::FOutcome NiagaraBatchImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("batch dispatches multiple niagara operations; call individual tools instead"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterNiagaraTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("niagara.list"),                    GT(&NiagaraListImpl));
    Dispatch.RegisterHandler(TEXT("niagara.get_info"),                GT(&NiagaraGetInfoImpl));
    Dispatch.RegisterHandler(TEXT("niagara.spawn"),                   GT(&NiagaraSpawnImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_parameter"),           GT(&NiagaraSetParameterImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create"),                  GT(&NiagaraCreateImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create_emitter"),          GT(&NiagaraCreateEmitterImpl));
    Dispatch.RegisterHandler(TEXT("niagara.add_emitter"),             GT(&NiagaraAddEmitterImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_emitters"),           GT(&NiagaraListEmittersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_emitter_property"),    GT(&NiagaraSetEmitterPropertyImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_modules"),            GT(&NiagaraListModulesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.get_emitter_info"),        GT(&NiagaraGetEmitterInfoImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_renderers"),          GT(&NiagaraListRenderersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.add_renderer"),            GT(&NiagaraAddRendererImpl));
    Dispatch.RegisterHandler(TEXT("niagara.remove_renderer"),         GT(&NiagaraRemoveRendererImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_renderer_property"),   GT(&NiagaraSetRendererPropertyImpl));
    Dispatch.RegisterHandler(TEXT("niagara.inspect_data_interfaces"), GT(&NiagaraInspectDataInterfacesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create_system_from_spec"), GT(&NiagaraCreateSystemFromSpecImpl));
    Dispatch.RegisterHandler(TEXT("niagara.get_compiled_hlsl"),       GT(&NiagaraGetCompiledHlslImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_system_parameters"),  GT(&NiagaraListSystemParametersImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_module_inputs"),      GT(&NiagaraListModuleInputsImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_module_input"),        GT(&NiagaraSetModuleInputImpl));
    Dispatch.RegisterHandler(TEXT("niagara.list_static_switches"),    GT(&NiagaraListStaticSwitchesImpl));
    Dispatch.RegisterHandler(TEXT("niagara.set_static_switch"),       GT(&NiagaraSetStaticSwitchImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create_module_from_hlsl"), GT(&NiagaraCreateModuleFromHlslImpl));
    Dispatch.RegisterHandler(TEXT("niagara.create_scratch_module"),   GT(&NiagaraCreateScratchModuleImpl));
    Dispatch.RegisterHandler(TEXT("niagara.batch"),                   GT(&NiagaraBatchImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
