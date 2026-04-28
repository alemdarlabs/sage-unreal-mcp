#include "Tools/SageAssetAdvancedTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/DataAsset.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#define LOCTEXT_NAMESPACE "SageAssetAdv"

namespace sage::tools
{
namespace
{

UObject* ResolveAsset(const FString& Path)
{
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Obj;
}

// ---- asset.get_mesh_bounds ------------------------------------------------

FSageToolDispatch::FOutcome GetMeshBoundsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));

    auto BoundsJson = [](const FBoxSphereBounds& B, const FBox& LocalBox) {
        auto Out = MakeShared<FJsonObject>();
        Out->SetField(TEXT("origin"),    detail::Vec3ToJson(B.Origin));
        Out->SetField(TEXT("extent"),    detail::Vec3ToJson(B.BoxExtent));
        Out->SetNumberField(TEXT("sphere_radius"), B.SphereRadius);
        Out->SetField(TEXT("local_min"), detail::Vec3ToJson(LocalBox.Min));
        Out->SetField(TEXT("local_max"), detail::Vec3ToJson(LocalBox.Max));
        return Out;
    };

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), FSoftObjectPath(Asset).ToString());
    R->SetStringField(TEXT("class"), Asset->GetClass()->GetName());

    if (auto* SM = Cast<UStaticMesh>(Asset))
    {
        const FBoxSphereBounds B = SM->GetBounds();
        const FBox Local = SM->GetBoundingBox();
        R->SetObjectField(TEXT("bounds"), BoundsJson(B, Local));
        R->SetNumberField(TEXT("lod_count"),     SM->GetNumLODs());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    if (auto* SK = Cast<USkeletalMesh>(Asset))
    {
        const FBoxSphereBounds B = SK->GetBounds();
        const FBox Local(B.Origin - B.BoxExtent, B.Origin + B.BoxExtent);
        R->SetObjectField(TEXT("bounds"), BoundsJson(B, Local));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset is not a mesh: %s"), *Asset->GetClass()->GetName()));
}

// ---- asset.get_mesh_collision --------------------------------------------

FSageToolDispatch::FOutcome GetMeshCollisionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));

    UBodySetup* Body = nullptr;
    if (auto* SM = Cast<UStaticMesh>(Asset)) Body = SM->GetBodySetup();
    if (!Body)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("no body setup on asset"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), FSoftObjectPath(Asset).ToString());
    const TCHAR* ComplexityStr = TEXT("Default");
    switch (Body->CollisionTraceFlag)
    {
    case CTF_UseDefault:                ComplexityStr = TEXT("Default"); break;
    case CTF_UseSimpleAndComplex:       ComplexityStr = TEXT("UseSimpleAndComplex"); break;
    case CTF_UseSimpleAsComplex:        ComplexityStr = TEXT("UseSimpleAsComplex"); break;
    case CTF_UseComplexAsSimple:        ComplexityStr = TEXT("UseComplexAsSimple"); break;
    default:                            ComplexityStr = TEXT("Unknown"); break;
    }
    R->SetStringField(TEXT("collision_complexity"), ComplexityStr);
    R->SetNumberField(TEXT("box_count"),     Body->AggGeom.BoxElems.Num());
    R->SetNumberField(TEXT("sphere_count"),  Body->AggGeom.SphereElems.Num());
    R->SetNumberField(TEXT("capsule_count"), Body->AggGeom.SphylElems.Num());
    R->SetNumberField(TEXT("convex_count"),  Body->AggGeom.ConvexElems.Num());
    R->SetNumberField(TEXT("total_primitives"),
        Body->AggGeom.GetElementCount());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.bulk_rename ----------------------------------------------------

FSageToolDispatch::FOutcome BulkRenameImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    const TArray<TSharedPtr<FJsonValue>>* Pairs = nullptr;
    if (!Args->TryGetArrayField(TEXT("renames"), Pairs))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'renames'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    UEditorAssetSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!Sub) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("EditorAssetSubsystem unavailable"));

    FScopedTransaction Tx(LOCTEXT("BulkRename", "Sage: Bulk Rename Assets"));
    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Renamed = 0, Failed = 0;

    for (const auto& V : *Pairs)
    {
        const auto Obj = V->AsObject();
        if (!Obj.IsValid()) { ++Failed; continue; }
        FString Src, Dst;
        if (!Obj->TryGetStringField(TEXT("src"), Src) || !Obj->TryGetStringField(TEXT("dst"), Dst))
        {
            ++Failed;
            continue;
        }
        const bool bOk = Sub->RenameAsset(Src, Dst);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("src"),   Src);
        Row->SetStringField(TEXT("dst"),   Dst);
        Row->SetBoolField  (TEXT("ok"),    bOk);
        Results.Add(MakeShared<FJsonValueObject>(Row));
        if (bOk) ++Renamed; else ++Failed;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("results"),  Results);
    R->SetNumberField(TEXT("renamed"),  Renamed);
    R->SetNumberField(TEXT("failed"),   Failed);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.move_folder ---------------------------------------------------

FSageToolDispatch::FOutcome MoveFolderImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Src, Dst;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("src"), Src)
        || !Args->TryGetStringField(TEXT("dst"), Dst))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'src' or 'dst'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    if (!GEditor) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    UEditorAssetSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!Sub) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("EditorAssetSubsystem unavailable"));

    FScopedTransaction Tx(LOCTEXT("MoveFolder", "Sage: Move Folder"));
    TArray<FString> AssetsInFolder = Sub->ListAssets(Src, /*Recursive*/ true);

    int32 Moved = 0, Failed = 0;
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const FString& AssetPath : AssetsInFolder)
    {
        // Compute destination path by replacing src prefix with dst.
        FString NewPath = AssetPath;
        if (!NewPath.RemoveFromStart(Src))
        {
            ++Failed;
            continue;
        }
        NewPath = Dst / NewPath;
        const bool bOk = Sub->RenameAsset(AssetPath, NewPath);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("src"), AssetPath);
        Row->SetStringField(TEXT("dst"), NewPath);
        Row->SetBoolField  (TEXT("ok"),  bOk);
        Results.Add(MakeShared<FJsonValueObject>(Row));
        if (bOk) ++Moved; else ++Failed;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("src_folder"), Src);
    R->SetStringField(TEXT("dst_folder"), Dst);
    R->SetNumberField(TEXT("moved"),      Moved);
    R->SetNumberField(TEXT("failed"),     Failed);
    R->SetArrayField (TEXT("results"),    Results);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.list_redirectors ----------------------------------------------

FSageToolDispatch::FOutcome ListRedirectorsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Folder = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("folder"), Folder);

    FAssetRegistryModule& M = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& AR = M.Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
    Filter.PackagePaths.Add(FName(*Folder));
    Filter.bRecursivePaths = true;
    TArray<FAssetData> Found;
    AR.GetAssets(Filter, Found);

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FAssetData& D : Found)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
        Out.Add(MakeShared<FJsonValueObject>(Row));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("folder"),       Folder);
    R->SetArrayField (TEXT("redirectors"),  Out);
    R->SetNumberField(TEXT("count"),        Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.fixup_redirectors ---------------------------------------------

FSageToolDispatch::FOutcome FixupRedirectorsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    TArray<FString> Paths;
    if (Args.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* PathArr = nullptr;
        if (Args->TryGetArrayField(TEXT("paths"), PathArr))
        {
            for (const auto& V : *PathArr) Paths.Add(V->AsString());
        }
    }

    FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
        TEXT("AssetTools"));

    if (Paths.Num() == 0)
    {
        // Default to /Game
        Paths.Add(TEXT("/Game"));
    }

    // FixupReferencers takes UObjectRedirector*[]; load them first.
    TArray<UObjectRedirector*> Redirectors;
    FAssetRegistryModule& M = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& AR = M.Get();
    for (const FString& Folder : Paths)
    {
        FARFilter Filter;
        Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(FName(*Folder));
        Filter.bRecursivePaths = true;
        TArray<FAssetData> Found;
        AR.GetAssets(Filter, Found);
        for (const FAssetData& D : Found)
        {
            if (UObjectRedirector* R = Cast<UObjectRedirector>(D.GetAsset()))
            {
                Redirectors.Add(R);
            }
        }
    }

    AT.Get().FixupReferencers(Redirectors, /*bCheckoutDialogPrompt*/ false);

    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("fixed"), Redirectors.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.diagnose_registry ---------------------------------------------

FSageToolDispatch::FOutcome DiagnoseRegistryImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    FAssetRegistryModule& M = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& AR = M.Get();
    AR.WaitForCompletion();

    TArray<FAssetData> All;
    AR.GetAllAssets(All, /*bIncludeOnlyOnDiskAssets*/ false);

    int32 InMemory = 0, OnDiskOnly = 0, Transient = 0;
    for (const FAssetData& D : All)
    {
        if (D.IsAssetLoaded()) ++InMemory; else ++OnDiskOnly;
        if (D.PackagePath.IsNone() || D.PackageName.ToString().StartsWith(TEXT("/Engine/Transient")))
        {
            ++Transient;
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("total"),         All.Num());
    R->SetNumberField(TEXT("in_memory"),     InMemory);
    R->SetNumberField(TEXT("on_disk_only"),  OnDiskOnly);
    R->SetNumberField(TEXT("transient"),     Transient);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.list / asset.search / asset.read_properties ------------------
// ---- (Phase 4.5 round 2 batch 1: asset query) ----------------------------

FSageToolDispatch::FOutcome ListAssetsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Dir = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("directory"), Dir);
    bool bRecursive = true;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("recursive"), bRecursive);
    int32 MaxResults = 1000;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 50000);
        }
    }

    FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& Registry = Module.Get();

    TArray<FAssetData> Found;
    Registry.GetAssetsByPath(FName(*Dir), Found, bRecursive, /*bIncludeOnlyOnDiskAssets*/ false);

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Total = Found.Num();
    int32 Returned = 0;
    for (const FAssetData& A : Found)
    {
        if (Returned >= MaxResults) break;
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("path"),  A.GetSoftObjectPath().ToString());
        O->SetStringField(TEXT("kind"),  A.AssetClassPath.GetAssetName().ToString());
        O->SetStringField(TEXT("name"),  A.AssetName.ToString());
        Out.Add(MakeShared<FJsonValueObject>(O));
        ++Returned;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("directory"), Dir);
    R->SetBoolField  (TEXT("recursive"), bRecursive);
    R->SetArrayField (TEXT("assets"),    Out);
    R->SetNumberField(TEXT("returned"),  Returned);
    R->SetNumberField(TEXT("total"),     Total);
    R->SetBoolField  (TEXT("capped"),    Returned < Total);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SearchAssetsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Query;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/empty 'query'"));
    }
    FString ClassFilter;
    Args->TryGetStringField(TEXT("class"), ClassFilter);
    FString Dir = TEXT("/Game");
    Args->TryGetStringField(TEXT("directory"), Dir);
    int32 MaxResults = 200;
    double N = 0;
    if (Args->TryGetNumberField(TEXT("max_results"), N))
    {
        MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 5000);
    }

    FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& Registry = Module.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Dir));
    Filter.bRecursivePaths = true;
    if (!ClassFilter.IsEmpty())
    {
        Filter.ClassPaths.Add(FTopLevelAssetPath(ClassFilter));
    }

    TArray<FAssetData> Found;
    Registry.GetAssets(Filter, Found);

    TArray<TSharedPtr<FJsonValue>> Out;
    const FString QLower = Query.ToLower();
    int32 Total = 0;
    for (const FAssetData& A : Found)
    {
        const FString Name = A.AssetName.ToString();
        const FString Path = A.GetSoftObjectPath().ToString();
        if (!Name.ToLower().Contains(QLower) && !Path.ToLower().Contains(QLower)) continue;
        ++Total;
        if (Out.Num() >= MaxResults) continue;

        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("path"), Path);
        O->SetStringField(TEXT("kind"), A.AssetClassPath.GetAssetName().ToString());
        O->SetStringField(TEXT("name"), Name);
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"),     Query);
    if (!ClassFilter.IsEmpty()) R->SetStringField(TEXT("class"), ClassFilter);
    R->SetStringField(TEXT("directory"), Dir);
    R->SetArrayField (TEXT("matches"),   Out);
    R->SetNumberField(TEXT("returned"),  Out.Num());
    R->SetNumberField(TEXT("total"),     Total);
    R->SetBoolField  (TEXT("capped"),    Out.Num() < Total);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReadAssetPropertiesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Obj = nullptr;
    {
        FSoftObjectPath Soft(Path);
        Obj = Soft.ResolveObject();
        if (!Obj) Obj = Soft.TryLoad();
    }
    if (!Obj)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset not found: %s"), *Path));
    }

    auto Props = MakeShared<FJsonObject>();
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        FProperty* P = *It;
        if (!P) continue;
        if (P->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
        const FString PropName = P->GetName();
        TSharedPtr<FJsonValue> V = detail::GetUPropertyAsJson(Obj, P);
        if (V.IsValid())
        {
            Props->SetField(PropName, V);
            ++Count;
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Obj->GetPathName());
    R->SetStringField(TEXT("class"),      Obj->GetClass()->GetName());
    R->SetObjectField(TEXT("properties"), Props);
    R->SetNumberField(TEXT("count"),      Count);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.create_data_asset / asset.delete_batch / asset.reload_package
// ---- (Phase 4.5 round 2 batch 4: write essentials) -----------------------

FSageToolDispatch::FOutcome CreateDataAssetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("class"), ClassPath) || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'class'"));
    }

    UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
    if (!Cls)
    {
        Cls = LoadObject<UClass>(nullptr, *ClassPath);
    }
    if (!Cls)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class not found: %s"), *ClassPath));
    }
    if (!Cls->IsChildOf(UDataAsset::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class %s is not a UDataAsset"), *Cls->GetName()));
    }
    if (Cls->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("class %s is deprecated"), *Cls->GetName()));
    }

    // Split /Game/Foo/Bar/MyAsset into PackagePath=/Game/Foo/Bar AssetName=MyAsset
    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("path must include directory: %s"), *Path));
    }
    if (PackagePath.IsEmpty() || AssetName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("path must be /Folder/AssetName form"));
    }

    if (FindPackage(nullptr, *(PackagePath / AssetName)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("package already exists: %s/%s"), *PackagePath, *AssetName));
    }

    FScopedTransaction Tx(LOCTEXT("CreateDataAsset", "Create Data Asset"));
    UPackage* Pkg = CreatePackage(*(PackagePath / AssetName));
    if (!Pkg)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("CreatePackage failed for %s/%s"), *PackagePath, *AssetName));
    }
    Pkg->FullyLoad();
    Pkg->Modify();

    UObject* Created = NewObject<UObject>(Pkg, Cls, *AssetName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("NewObject failed for %s"), *Cls->GetName()));
    }
    FAssetRegistryModule::AssetCreated(Created);
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Created->GetPathName());
    R->SetStringField(TEXT("class"), Created->GetClass()->GetName());
    R->SetStringField(TEXT("name"),  Created->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome DeleteBatchImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("paths"), PathsArr) || !PathsArr || PathsArr->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/empty 'paths'"));
    }

    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UEditorAssetSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!Sub)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("UEditorAssetSubsystem unavailable"));
    }

    FScopedTransaction Tx(LOCTEXT("DeleteBatch", "Delete Asset Batch"));
    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Deleted = 0, Missing = 0, Failed = 0;
    for (const TSharedPtr<FJsonValue>& V : *PathsArr)
    {
        FString P;
        if (!V.IsValid() || !V->TryGetString(P) || P.IsEmpty()) continue;
        auto E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("path"), P);
        if (!Sub->DoesAssetExist(P))
        {
            E->SetStringField(TEXT("status"), TEXT("missing"));
            ++Missing;
        }
        else if (Sub->DeleteAsset(P))
        {
            E->SetStringField(TEXT("status"), TEXT("deleted"));
            ++Deleted;
        }
        else
        {
            E->SetStringField(TEXT("status"), TEXT("failed"));
            ++Failed;
        }
        Results.Add(MakeShared<FJsonValueObject>(E));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("results"), Results);
    R->SetNumberField(TEXT("deleted"), Deleted);
    R->SetNumberField(TEXT("missing"), Missing);
    R->SetNumberField(TEXT("failed"),  Failed);
    R->SetNumberField(TEXT("total"),   PathsArr->Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReloadPackageImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    // Path can be /Game/Foo/Bar.Bar or /Game/Foo/Bar — strip object name if present
    FString PackageName = Path;
    int32 DotIdx = INDEX_NONE;
    if (PackageName.FindChar(TEXT('.'), DotIdx))
    {
        PackageName = PackageName.Left(DotIdx);
    }

    UPackage* Pkg = FindPackage(nullptr, *PackageName);
    if (!Pkg)
    {
        Pkg = LoadPackage(nullptr, *PackageName, LOAD_None);
    }
    if (!Pkg)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("package not found: %s"), *PackageName));
    }

    TArray<UPackage*> ToReload;
    ToReload.Add(Pkg);
    UPackageTools::ReloadPackages(ToReload);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("package"),  PackageName);
    R->SetStringField(TEXT("status"),   TEXT("reloaded"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.list_textures / asset.get_texture_info / asset.set_texture_settings
// ---- (Phase 4.5 round 2 batch 3) -----------------------------------------

namespace tex_helpers
{
    const TCHAR* CompressionToStr(TextureCompressionSettings C)
    {
        switch (C)
        {
            case TC_Default:                return TEXT("Default");
            case TC_Normalmap:              return TEXT("Normalmap");
            case TC_Masks:                  return TEXT("Masks");
            case TC_Grayscale:              return TEXT("Grayscale");
            case TC_Displacementmap:        return TEXT("Displacementmap");
            case TC_VectorDisplacementmap:  return TEXT("VectorDisplacementmap");
            case TC_HDR:                    return TEXT("HDR");
            case TC_EditorIcon:             return TEXT("EditorIcon");
            case TC_Alpha:                  return TEXT("Alpha");
            case TC_DistanceFieldFont:      return TEXT("DistanceFieldFont");
            case TC_HDR_Compressed:         return TEXT("HDR_Compressed");
            case TC_BC7:                    return TEXT("BC7");
            case TC_HalfFloat:              return TEXT("HalfFloat");
            case TC_LQ:                     return TEXT("LQ");
            case TC_EncodedReflectionCapture: return TEXT("EncodedReflectionCapture");
            case TC_SingleFloat:            return TEXT("SingleFloat");
            case TC_HDR_F32:                return TEXT("HDR_F32");
            default:                        return TEXT("Unknown");
        }
    }

    bool ParseCompression(const FString& S, TextureCompressionSettings& Out)
    {
        const FString L = S.ToLower();
        if      (L == TEXT("default"))                 Out = TC_Default;
        else if (L == TEXT("normalmap"))               Out = TC_Normalmap;
        else if (L == TEXT("masks"))                   Out = TC_Masks;
        else if (L == TEXT("grayscale"))               Out = TC_Grayscale;
        else if (L == TEXT("displacementmap"))         Out = TC_Displacementmap;
        else if (L == TEXT("vectordisplacementmap"))   Out = TC_VectorDisplacementmap;
        else if (L == TEXT("hdr"))                     Out = TC_HDR;
        else if (L == TEXT("editoricon"))              Out = TC_EditorIcon;
        else if (L == TEXT("alpha"))                   Out = TC_Alpha;
        else if (L == TEXT("distancefieldfont"))       Out = TC_DistanceFieldFont;
        else if (L == TEXT("hdr_compressed"))          Out = TC_HDR_Compressed;
        else if (L == TEXT("bc7"))                     Out = TC_BC7;
        else if (L == TEXT("halffloat"))               Out = TC_HalfFloat;
        else if (L == TEXT("lq"))                      Out = TC_LQ;
        else if (L == TEXT("singlefloat"))             Out = TC_SingleFloat;
        else if (L == TEXT("hdr_f32"))                 Out = TC_HDR_F32;
        else return false;
        return true;
    }

    const TCHAR* AddressToStr(TextureAddress A)
    {
        switch (A)
        {
            case TA_Wrap:   return TEXT("Wrap");
            case TA_Clamp:  return TEXT("Clamp");
            case TA_Mirror: return TEXT("Mirror");
            default:        return TEXT("Unknown");
        }
    }

    bool ParseAddress(const FString& S, TextureAddress& Out)
    {
        const FString L = S.ToLower();
        if      (L == TEXT("wrap"))   Out = TA_Wrap;
        else if (L == TEXT("clamp"))  Out = TA_Clamp;
        else if (L == TEXT("mirror")) Out = TA_Mirror;
        else return false;
        return true;
    }

    const TCHAR* FilterToStr(TextureFilter F)
    {
        switch (F)
        {
            case TF_Nearest:   return TEXT("Nearest");
            case TF_Bilinear:  return TEXT("Bilinear");
            case TF_Trilinear: return TEXT("Trilinear");
            case TF_Default:   return TEXT("Default");
            default:           return TEXT("Unknown");
        }
    }

    bool ParseFilter(const FString& S, TextureFilter& Out)
    {
        const FString L = S.ToLower();
        if      (L == TEXT("nearest"))   Out = TF_Nearest;
        else if (L == TEXT("bilinear"))  Out = TF_Bilinear;
        else if (L == TEXT("trilinear")) Out = TF_Trilinear;
        else if (L == TEXT("default"))   Out = TF_Default;
        else return false;
        return true;
    }
}

FSageToolDispatch::FOutcome ListTexturesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Dir = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("directory"), Dir);
    int32 MaxResults = 1000;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 50000);
        }
    }

    FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& Registry = Module.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Dir));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.Texture2D")));
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine.Texture")));
    Filter.bRecursiveClasses = true;

    TArray<FAssetData> Found;
    Registry.GetAssets(Filter, Found);

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FAssetData& A : Found)
    {
        if (Out.Num() >= MaxResults) break;
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("path"), A.GetSoftObjectPath().ToString());
        O->SetStringField(TEXT("name"), A.AssetName.ToString());
        O->SetStringField(TEXT("kind"), A.AssetClassPath.GetAssetName().ToString());
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("directory"), Dir);
    R->SetArrayField (TEXT("textures"),  Out);
    R->SetNumberField(TEXT("returned"),  Out.Num());
    R->SetNumberField(TEXT("total"),     Found.Num());
    R->SetBoolField  (TEXT("capped"),    Out.Num() < Found.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GetTextureInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    UTexture* Tex = Cast<UTexture>(Asset);
    if (!Tex) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a UTexture: %s"),
                        Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         Tex->GetPathName());
    R->SetStringField(TEXT("class"),        Tex->GetClass()->GetName());
    R->SetStringField(TEXT("compression"),  tex_helpers::CompressionToStr(Tex->CompressionSettings));
    R->SetStringField(TEXT("address_x"),    tex_helpers::AddressToStr(static_cast<TextureAddress>(Tex->GetTextureAddressX())));
    R->SetStringField(TEXT("address_y"),    tex_helpers::AddressToStr(static_cast<TextureAddress>(Tex->GetTextureAddressY())));
    R->SetStringField(TEXT("filter"),       tex_helpers::FilterToStr(Tex->Filter));
    R->SetBoolField  (TEXT("srgb"),         Tex->SRGB);
    R->SetBoolField  (TEXT("never_stream"), Tex->NeverStream);
    R->SetNumberField(TEXT("lod_bias"),     Tex->LODBias);
    R->SetNumberField(TEXT("compression_quality"), Tex->CompressionQuality);

    if (UTexture2D* Tex2D = Cast<UTexture2D>(Asset))
    {
        R->SetNumberField(TEXT("width"),  Tex2D->GetSizeX());
        R->SetNumberField(TEXT("height"), Tex2D->GetSizeY());
        R->SetNumberField(TEXT("num_mips"), Tex2D->GetNumMips());
        EPixelFormat PF = Tex2D->GetPixelFormat();
        R->SetStringField(TEXT("pixel_format"), GetPixelFormatString(PF));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetTextureSettingsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    UTexture* Tex = Cast<UTexture>(Asset);
    if (!Tex) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a UTexture: %s"),
                        Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Tex->GetPathName());

    bool bChanged = false;
    {
        FScopedTransaction Tx(LOCTEXT("SetTextureSettings", "Set Texture Settings"));
        Tex->Modify();

        FString S;
        if (Args->TryGetStringField(TEXT("compression"), S))
        {
            TextureCompressionSettings V;
            if (!tex_helpers::ParseCompression(S, V))
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("unknown compression '%s'"), *S));
            }
            Tex->CompressionSettings = V;
            R->SetStringField(TEXT("compression_set"), S);
            bChanged = true;
        }
        if (Args->TryGetStringField(TEXT("address_x"), S))
        {
            TextureAddress V;
            if (!tex_helpers::ParseAddress(S, V))
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("unknown address_x '%s'"), *S));
            }
            if (UTexture2D* Tex2D = Cast<UTexture2D>(Asset)) Tex2D->AddressX = V;
            R->SetStringField(TEXT("address_x_set"), S);
            bChanged = true;
        }
        if (Args->TryGetStringField(TEXT("address_y"), S))
        {
            TextureAddress V;
            if (!tex_helpers::ParseAddress(S, V))
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("unknown address_y '%s'"), *S));
            }
            if (UTexture2D* Tex2D = Cast<UTexture2D>(Asset)) Tex2D->AddressY = V;
            R->SetStringField(TEXT("address_y_set"), S);
            bChanged = true;
        }
        if (Args->TryGetStringField(TEXT("filter"), S))
        {
            TextureFilter V;
            if (!tex_helpers::ParseFilter(S, V))
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("unknown filter '%s'"), *S));
            }
            Tex->Filter = V;
            R->SetStringField(TEXT("filter_set"), S);
            bChanged = true;
        }
        bool B = false;
        if (Args->TryGetBoolField(TEXT("srgb"), B))
        {
            Tex->SRGB = B;
            R->SetBoolField(TEXT("srgb_set"), B);
            bChanged = true;
        }
        if (Args->TryGetBoolField(TEXT("never_stream"), B))
        {
            Tex->NeverStream = B;
            R->SetBoolField(TEXT("never_stream_set"), B);
            bChanged = true;
        }
        double N = 0;
        if (Args->TryGetNumberField(TEXT("lod_bias"), N))
        {
            Tex->LODBias = static_cast<int32>(N);
            R->SetNumberField(TEXT("lod_bias_set"), N);
            bChanged = true;
        }

        if (bChanged)
        {
            Tex->PostEditChange();
            Tex->MarkPackageDirty();
        }
    }

    R->SetBoolField(TEXT("changed"), bChanged);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.list_sockets / asset.add_socket / asset.remove_socket --------
// ---- (Phase 4.5 round 2 batch 2: static + skeletal mesh sockets) --------

namespace socket_helpers
{
    TSharedPtr<FJsonObject> StaticSocketToJson(const UStaticMeshSocket* S)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),     S->SocketName.ToString());
        J->SetField(TEXT("location"),       detail::Vec3ToJson(S->RelativeLocation));
        J->SetField(TEXT("rotation"),       detail::Rot3ToJson(S->RelativeRotation));
        J->SetField(TEXT("scale"),          detail::Vec3ToJson(S->RelativeScale));
        if (!S->Tag.IsEmpty()) J->SetStringField(TEXT("tag"), S->Tag);
        return J;
    }

    TSharedPtr<FJsonObject> SkelSocketToJson(const USkeletalMeshSocket* S)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),         S->SocketName.ToString());
        J->SetStringField(TEXT("bone"),         S->BoneName.ToString());
        J->SetField(TEXT("location"),           detail::Vec3ToJson(S->RelativeLocation));
        J->SetField(TEXT("rotation"),           detail::Rot3ToJson(S->RelativeRotation));
        J->SetField(TEXT("scale"),              detail::Vec3ToJson(S->RelativeScale));
        J->SetBoolField(TEXT("force_always_animated"), S->bForceAlwaysAnimated);
        return J;
    }
}

FSageToolDispatch::FOutcome ListSocketsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Asset->GetPathName());
    R->SetStringField(TEXT("class"), Asset->GetClass()->GetName());

    TArray<TSharedPtr<FJsonValue>> SocketArr;
    if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
    {
        for (UStaticMeshSocket* S : SM->Sockets)
        {
            if (!S) continue;
            SocketArr.Add(MakeShared<FJsonValueObject>(socket_helpers::StaticSocketToJson(S)));
        }
        R->SetStringField(TEXT("kind"), TEXT("static_mesh"));
    }
    else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
    {
        for (const TObjectPtr<USkeletalMeshSocket>& S : SK->GetMeshOnlySocketList())
        {
            if (!S) continue;
            SocketArr.Add(MakeShared<FJsonValueObject>(socket_helpers::SkelSocketToJson(S.Get())));
        }
        R->SetStringField(TEXT("kind"), TEXT("skeletal_mesh"));
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset is %s, expected StaticMesh or SkeletalMesh"),
                            *Asset->GetClass()->GetName()));
    }
    R->SetArrayField(TEXT("sockets"), SocketArr);
    R->SetNumberField(TEXT("count"), SocketArr.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddSocketImpl(const TSharedPtr<FJsonObject>& Args)
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
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));

    FVector Location(0,0,0);
    FRotator Rotation(0,0,0);
    FVector Scale(1,1,1);
    detail::ParseVector3 (Args, TEXT("location"), Location);
    detail::ParseRotator3(Args, TEXT("rotation"), Rotation);
    detail::ParseVector3 (Args, TEXT("scale"),    Scale);
    FString BoneName;
    Args->TryGetStringField(TEXT("bone"), BoneName);

    FName SocketFName(*Name);

    if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
    {
        if (SM->FindSocket(SocketFName))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("socket '%s' already exists"), *Name));
        }
        FScopedTransaction Tx(LOCTEXT("AddSocket", "Add Socket"));
        SM->Modify();
        UStaticMeshSocket* S = NewObject<UStaticMeshSocket>(SM);
        S->SocketName       = SocketFName;
        S->RelativeLocation = Location;
        S->RelativeRotation = Rotation;
        S->RelativeScale    = Scale;
        SM->AddSocket(S);
        SM->MarkPackageDirty();
        SM->PostEditChange();

        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Asset->GetPathName());
        R->SetStringField(TEXT("kind"), TEXT("static_mesh"));
        R->SetField(TEXT("socket"), MakeShared<FJsonValueObject>(socket_helpers::StaticSocketToJson(S)));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
    {
        if (SK->FindSocket(SocketFName))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("socket '%s' already exists"), *Name));
        }
        FScopedTransaction Tx(LOCTEXT("AddSocket", "Add Socket"));
        SK->Modify();
        USkeletalMeshSocket* S = NewObject<USkeletalMeshSocket>(SK);
        S->SocketName       = SocketFName;
        S->BoneName         = BoneName.IsEmpty() ? NAME_None : FName(*BoneName);
        S->RelativeLocation = Location;
        S->RelativeRotation = Rotation;
        S->RelativeScale    = Scale;
        SK->GetMeshOnlySocketList().Add(TObjectPtr<USkeletalMeshSocket>(S));
        SK->MarkPackageDirty();
        SK->PostEditChange();

        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Asset->GetPathName());
        R->SetStringField(TEXT("kind"), TEXT("skeletal_mesh"));
        R->SetField(TEXT("socket"), MakeShared<FJsonValueObject>(socket_helpers::SkelSocketToJson(S)));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset is %s, expected StaticMesh or SkeletalMesh"),
                        *Asset->GetClass()->GetName()));
}

FSageToolDispatch::FOutcome RemoveSocketImpl(const TSharedPtr<FJsonObject>& Args)
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
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));

    FName SocketFName(*Name);

    if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
    {
        UStaticMeshSocket* Found = SM->FindSocket(SocketFName);
        if (!Found)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("socket '%s' not found"), *Name));
        }
        FScopedTransaction Tx(LOCTEXT("RemoveSocket", "Remove Socket"));
        SM->Modify();
        SM->RemoveSocket(Found);
        SM->MarkPackageDirty();
        SM->PostEditChange();

        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"),    Asset->GetPathName());
        R->SetStringField(TEXT("kind"),    TEXT("static_mesh"));
        R->SetStringField(TEXT("removed"), Name);
        R->SetNumberField(TEXT("remaining"), SM->Sockets.Num());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
    {
        TArray<TObjectPtr<USkeletalMeshSocket>>& MeshOnly = SK->GetMeshOnlySocketList();
        USkeletalMeshSocket* Found = nullptr;
        for (const TObjectPtr<USkeletalMeshSocket>& S : MeshOnly)
        {
            if (S && S->SocketName == SocketFName) { Found = S.Get(); break; }
        }
        if (!Found)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("mesh-only socket '%s' not found"), *Name));
        }
        FScopedTransaction Tx(LOCTEXT("RemoveSocket", "Remove Socket"));
        SK->Modify();
        MeshOnly.RemoveSingle(TObjectPtr<USkeletalMeshSocket>(Found));
        SK->MarkPackageDirty();
        SK->PostEditChange();

        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"),    Asset->GetPathName());
        R->SetStringField(TEXT("kind"),    TEXT("skeletal_mesh"));
        R->SetStringField(TEXT("removed"), Name);
        R->SetNumberField(TEXT("remaining"), SK->GetMeshOnlySocketList().Num());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset is %s, expected StaticMesh or SkeletalMesh"),
                        *Asset->GetClass()->GetName()));
}

}  // namespace (anonymous)

void RegisterAssetAdvancedTools(FSageToolDispatch& Dispatch)
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

    // Read
    Dispatch.RegisterHandler(TEXT("asset.get_mesh_bounds"),    GT(&GetMeshBoundsImpl));
    Dispatch.RegisterHandler(TEXT("asset.get_mesh_collision"), GT(&GetMeshCollisionImpl));
    Dispatch.RegisterHandler(TEXT("asset.list_redirectors"),   GT(&ListRedirectorsImpl));
    Dispatch.RegisterHandler(TEXT("asset.diagnose_registry"),  GT(&DiagnoseRegistryImpl));

    // Phase 4.5-r2 batch 1: asset query
    Dispatch.RegisterHandler(TEXT("asset.list"),               GT(&ListAssetsImpl));
    Dispatch.RegisterHandler(TEXT("asset.search"),             GT(&SearchAssetsImpl));
    Dispatch.RegisterHandler(TEXT("asset.read_properties"),    GT(&ReadAssetPropertiesImpl));

    // Phase 4.5-r2 batch 2: socket management
    Dispatch.RegisterHandler(TEXT("asset.list_sockets"),       GT(&ListSocketsImpl));
    Dispatch.RegisterHandler(TEXT("asset.add_socket"),         GT(&AddSocketImpl));
    Dispatch.RegisterHandler(TEXT("asset.remove_socket"),      GT(&RemoveSocketImpl));

    // Phase 4.5-r2 batch 3: textures
    Dispatch.RegisterHandler(TEXT("asset.list_textures"),         GT(&ListTexturesImpl));
    Dispatch.RegisterHandler(TEXT("asset.get_texture_info"),      GT(&GetTextureInfoImpl));
    Dispatch.RegisterHandler(TEXT("asset.set_texture_settings"),  GT(&SetTextureSettingsImpl));

    // Phase 4.5-r2 batch 4: write essentials
    Dispatch.RegisterHandler(TEXT("asset.create_data_asset"),     GT(&CreateDataAssetImpl));
    Dispatch.RegisterHandler(TEXT("asset.delete_batch"),          GT(&DeleteBatchImpl));
    Dispatch.RegisterHandler(TEXT("asset.reload_package"),        GT(&ReloadPackageImpl));

    // Write
    Dispatch.RegisterHandler(TEXT("asset.bulk_rename"),        GT(&BulkRenameImpl));
    Dispatch.RegisterHandler(TEXT("asset.move_folder"),        GT(&MoveFolderImpl));
    Dispatch.RegisterHandler(TEXT("asset.fixup_redirectors"),  GT(&FixupRedirectorsImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
