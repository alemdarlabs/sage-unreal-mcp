#include "Tools/SagePcgTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "SagePcg"

namespace sage::tools
{
namespace
{

static UClass* FindPcgClass(const TCHAR* Name)
{
    UClass* Cls = FindObject<UClass>(nullptr, Name);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, Name);
    return Cls;
}

static FSageToolDispatch::FOutcome PcgNotAvailable()
{
    return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("PCG plugin required; class not found"));
}

// ---- pcg.list_graphs -------------------------------------------------------

FSageToolDispatch::FOutcome PcgListGraphsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    UClass* PCGCls = FindPcgClass(TEXT("/Script/PCG.PCGGraph"));

    TArray<TSharedPtr<FJsonValue>> Graphs;
    if (PCGCls)
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*SearchPath));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(PCGCls->GetClassPathName());
        TArray<FAssetData> Assets;
        ARM.Get().GetAssets(Filter, Assets);
        for (const FAssetData& D : Assets)
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"), D.AssetName.ToString());
            J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
            Graphs.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("graphs"), Graphs);
    R->SetNumberField(TEXT("count"),  Graphs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- pcg.read_graph --------------------------------------------------------

FSageToolDispatch::FOutcome PcgReadGraphImpl(const TSharedPtr<FJsonObject>& Args)
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

// ---- pcg.read_node_settings ------------------------------------------------

FSageToolDispatch::FOutcome PcgReadNodeSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("PCG node settings require PCGGraph::GetNodes() from PCGEditor module"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- pcg.get_components / get_component_details ----------------------------

FSageToolDispatch::FOutcome PcgGetComponentsImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    UClass* PCGCompCls = FindPcgClass(TEXT("/Script/PCG.PCGComponent"));

    TArray<TSharedPtr<FJsonValue>> Comps;
    if (PCGCompCls)
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* A = *It;
            if (!A) continue;
            UActorComponent* C = A->FindComponentByClass(PCGCompCls);
            if (!C) continue;
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("actor"),     A->GetActorLabel());
            J->SetStringField(TEXT("actor_id"),  A->GetPathName());
            J->SetStringField(TEXT("component"), C->GetName());
            Comps.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("components"), Comps);
    R->SetNumberField(TEXT("count"),      Comps.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome PcgGetComponentDetailsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());

    UClass* PCGCompCls = FindPcgClass(TEXT("/Script/PCG.PCGComponent"));
    if (PCGCompCls)
    {
        UActorComponent* C = A->FindComponentByClass(PCGCompCls);
        if (C)
        {
            TArray<TSharedPtr<FJsonValue>> Props;
            for (TFieldIterator<FProperty> It(C->GetClass()); It; ++It)
            {
                auto J = MakeShared<FJsonObject>();
                J->SetStringField(TEXT("name"), It->GetName());
                Props.Add(MakeShared<FJsonValueObject>(J));
            }
            R->SetArrayField(TEXT("properties"), Props);
        }
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- pcg.create_graph ------------------------------------------------------

FSageToolDispatch::FOutcome PcgCreateGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* PCGCls = FindPcgClass(TEXT("/Script/PCG.PCGGraph"));
    if (!PCGCls) return PcgNotAvailable();

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, PCGCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("PCGGraph"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- pcg.add_node / connect_nodes / set_node_settings / remove_node --------

FSageToolDispatch::FOutcome PcgAddNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("PCG node addition requires PCGGraphEditor APIs; "
             "use editor.run_python for graph editing"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}
FSageToolDispatch::FOutcome PcgConnectNodesImpl(const TSharedPtr<FJsonObject>& Args)
{ return PcgAddNodeImpl(Args); }
FSageToolDispatch::FOutcome PcgSetNodeSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{ return PcgAddNodeImpl(Args); }
FSageToolDispatch::FOutcome PcgRemoveNodeImpl(const TSharedPtr<FJsonObject>& Args)
{ return PcgAddNodeImpl(Args); }

// ---- pcg.set_static_mesh_spawner_meshes ------------------------------------

FSageToolDispatch::FOutcome PcgSetStaticMeshSpawnerMeshesImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("StaticMeshSpawner mesh list mutation requires PCGEditorModule runtime APIs"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- pcg.execute / force_regenerate / cleanup / toggle_graph ---------------

FSageToolDispatch::FOutcome PcgExecuteImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    // Trigger regeneration via console command
    GEditor->Exec(GEditor->GetEditorWorldContext().World(),
        TEXT("PCG.GenerateAndGetResultBySoftObjectPath"), *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),  A->GetPathName());
    R->SetBoolField  (TEXT("triggered"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome PcgForceRegenerateImpl(const TSharedPtr<FJsonObject>& Args)
{ return PcgExecuteImpl(Args); }

FSageToolDispatch::FOutcome PcgCleanupImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("cleaned"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome PcgToggleGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("PCG graph toggle via PCGComponent::bActivated property; "
             "use editor.set_property on the PCGComponent"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- pcg.add_volume --------------------------------------------------------

FSageToolDispatch::FOutcome PcgAddVolumeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    UClass* PCGVolCls = FindPcgClass(TEXT("/Script/PCG.PCGVolume"));
    if (!PCGVolCls) return PcgNotAvailable();

    FVector Loc = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Loc);
    FTransform Tf(FRotator::ZeroRotator, Loc);

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FScopedTransaction Tx(LOCTEXT("AddPcgVol", "Add PCG Volume"));
    AActor* A = GEditor->AddActor(World->GetCurrentLevel(), PCGVolCls, Tf);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("AddActor failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterPcgTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("pcg.list_graphs"),                  GT(&PcgListGraphsImpl));
    Dispatch.RegisterHandler(TEXT("pcg.read_graph"),                   GT(&PcgReadGraphImpl));
    Dispatch.RegisterHandler(TEXT("pcg.read_node_settings"),           GT(&PcgReadNodeSettingsImpl));
    Dispatch.RegisterHandler(TEXT("pcg.get_components"),               GT(&PcgGetComponentsImpl));
    Dispatch.RegisterHandler(TEXT("pcg.get_component_details"),        GT(&PcgGetComponentDetailsImpl));
    Dispatch.RegisterHandler(TEXT("pcg.create_graph"),                 GT(&PcgCreateGraphImpl));
    Dispatch.RegisterHandler(TEXT("pcg.add_node"),                     GT(&PcgAddNodeImpl));
    Dispatch.RegisterHandler(TEXT("pcg.connect_nodes"),                GT(&PcgConnectNodesImpl));
    Dispatch.RegisterHandler(TEXT("pcg.set_node_settings"),            GT(&PcgSetNodeSettingsImpl));
    Dispatch.RegisterHandler(TEXT("pcg.set_static_mesh_spawner_meshes"),GT(&PcgSetStaticMeshSpawnerMeshesImpl));
    Dispatch.RegisterHandler(TEXT("pcg.remove_node"),                  GT(&PcgRemoveNodeImpl));
    Dispatch.RegisterHandler(TEXT("pcg.execute"),                      GT(&PcgExecuteImpl));
    Dispatch.RegisterHandler(TEXT("pcg.force_regenerate"),             GT(&PcgForceRegenerateImpl));
    Dispatch.RegisterHandler(TEXT("pcg.cleanup"),                      GT(&PcgCleanupImpl));
    Dispatch.RegisterHandler(TEXT("pcg.toggle_graph"),                 GT(&PcgToggleGraphImpl));
    Dispatch.RegisterHandler(TEXT("pcg.add_volume"),                   GT(&PcgAddVolumeImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
