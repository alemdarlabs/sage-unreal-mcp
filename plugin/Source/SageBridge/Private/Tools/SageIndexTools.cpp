#include "Tools/SageIndexTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

namespace sage::tools
{
namespace
{

FSageToolDispatch::FOutcome ScanAssetRegistry(const TSharedPtr<FJsonObject>& /*Args*/)
{
    const double StartSec = FPlatformTime::Seconds();

    FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& AR = Registry.Get();

    // The editor's registry may still be performing its initial scan when
    // a freshly-launched session calls index_slot — wait so we return a
    // complete snapshot rather than a partial one.
    AR.WaitForCompletion();

    TArray<FAssetData> All;
    AR.GetAllAssets(All, /*bIncludeOnlyOnDiskAssets=*/false);

    TArray<TSharedPtr<FJsonValue>> JsonAssets;
    JsonAssets.Reserve(All.Num());

    for (const FAssetData& Data : All)
    {
        // Skip transient/in-memory assets that have no disk presence — they
        // confuse downstream impact_of (Phase 2.4) since they can't be
        // referenced from saved content. The editor sometimes registers
        // engine-internal helper assets in /Engine/Transient/...
        if (Data.PackagePath.IsNone()) continue;

        auto Row = MakeShared<FJsonObject>();
        // SoftObjectPath yields the canonical "/Game/Foo/Bar.Bar" form used
        // by all of UE's loaders — matches what spawn_actor / move_asset
        // produce, so the graph and the runtime layer agree on identity.
        Row->SetStringField(TEXT("path"), Data.GetSoftObjectPath().ToString());
        // AssetClassPath is the modern (5.1+) replacement for AssetClass
        // FName. We store just the class name (e.g. "Blueprint", "Texture2D")
        // — full /Script/Engine.X path is reconstructible from the Class
        // table once 2.3 lands.
        Row->SetStringField(TEXT("kind"),
            Data.AssetClassPath.GetAssetName().ToString());
        JsonAssets.Add(MakeShared<FJsonValueObject>(Row));
    }

    const double ElapsedMs = (FPlatformTime::Seconds() - StartSec) * 1000.0;

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("assets"),  JsonAssets);
    Result->SetNumberField(TEXT("total"),  JsonAssets.Num());
    Result->SetNumberField(TEXT("scan_ms"), ElapsedMs);

    UE_LOG(LogSageBridge, Log,
           TEXT("AssetRegistry scan: %d assets in %.1f ms"),
           JsonAssets.Num(), ElapsedMs);

    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

}  // namespace

void RegisterIndexTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("_scan_asset_registry"), &ScanAssetRegistry);
}

}  // namespace sage::tools
