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
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
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

    // Write
    Dispatch.RegisterHandler(TEXT("asset.bulk_rename"),        GT(&BulkRenameImpl));
    Dispatch.RegisterHandler(TEXT("asset.move_folder"),        GT(&MoveFolderImpl));
    Dispatch.RegisterHandler(TEXT("asset.fixup_redirectors"),  GT(&FixupRedirectorsImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
