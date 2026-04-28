#include "Tools/SageIndexTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "UObject/TopLevelAssetPath.h"

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

    // First pass: build the {assets} array AND a PackageName → SoftObjectPath
    // map. Dependencies come back from AR.GetDependencies as PackageNames
    // (no asset-name suffix), so we need this map to project them onto the
    // canonical Asset.path that the graph uses.
    TArray<TSharedPtr<FJsonValue>> JsonAssets;
    JsonAssets.Reserve(All.Num());

    TMap<FName, FString> PackageToSoftPath;
    PackageToSoftPath.Reserve(All.Num());

    for (const FAssetData& Data : All)
    {
        if (Data.PackagePath.IsNone()) continue;

        const FString SoftPath = Data.GetSoftObjectPath().ToString();
        PackageToSoftPath.Add(Data.PackageName, SoftPath);

        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), SoftPath);
        Row->SetStringField(TEXT("kind"),
            Data.AssetClassPath.GetAssetName().ToString());
        JsonAssets.Add(MakeShared<FJsonValueObject>(Row));
    }

    // Second pass: collect Package-level dependencies and project to Asset
    // paths. Edges referencing packages we never saw (engine internals,
    // generated content) are skipped here so the server's "skipped"
    // counter stays accurate.
    TArray<TSharedPtr<FJsonValue>> JsonDeps;
    int32 EdgeCount = 0;
    JsonDeps.Reserve(All.Num() * 2);  // rough average

    for (const FAssetData& Data : All)
    {
        if (Data.PackagePath.IsNone()) continue;

        const FString* FromPath = PackageToSoftPath.Find(Data.PackageName);
        if (FromPath == nullptr) continue;

        TArray<FName> DepPackages;
        AR.GetDependencies(Data.PackageName, DepPackages,
            UE::AssetRegistry::EDependencyCategory::Package);

        for (const FName& DepPkg : DepPackages)
        {
            const FString* ToPath = PackageToSoftPath.Find(DepPkg);
            if (ToPath == nullptr) continue;
            if (*FromPath == *ToPath) continue;

            auto Edge = MakeShared<FJsonObject>();
            Edge->SetStringField(TEXT("from"), *FromPath);
            Edge->SetStringField(TEXT("to"),   *ToPath);
            JsonDeps.Add(MakeShared<FJsonValueObject>(Edge));
            ++EdgeCount;
        }
    }

    const double ElapsedMs = (FPlatformTime::Seconds() - StartSec) * 1000.0;

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("assets"),       JsonAssets);
    Result->SetArrayField(TEXT("dependencies"), JsonDeps);
    Result->SetNumberField(TEXT("total"),       JsonAssets.Num());
    Result->SetNumberField(TEXT("scan_ms"),     ElapsedMs);

    UE_LOG(LogSageBridge, Log,
           TEXT("AssetRegistry scan: %d assets, %d edges in %.1f ms"),
           JsonAssets.Num(), EdgeCount, ElapsedMs);

    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

}  // namespace

void RegisterIndexTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("_scan_asset_registry"), &ScanAssetRegistry);
}

}  // namespace sage::tools
