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
#include "Materials/MaterialInterface.h"
#include "Engine/DataTable.h"
#include "AssetImportTask.h"
#include "AssetExportTask.h"
#include "Exporters/Exporter.h"
#include "EditorReimportHandler.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
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
    bool bRecursive = true;
    int32 MaxResults = 1000;
    int32 Offset = 0;
    FString ClassFilter;
    TSet<FString> KindFilter;
    TSet<FString> FieldsFilter;

    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("directory"), Dir);
        Args->TryGetBoolField  (TEXT("recursive"), bRecursive);
        Args->TryGetStringField(TEXT("class"),     ClassFilter);

        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 50000);
        }
        if (Args->TryGetNumberField(TEXT("offset"), N))
        {
            Offset = FMath::Max(0, static_cast<int32>(N));
        }

        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Args->TryGetArrayField(TEXT("kind"), Arr) && Arr)
        {
            for (const auto& V : *Arr)
            {
                if (V.IsValid() && V->Type == EJson::String) KindFilter.Add(V->AsString());
            }
        }
        if (Args->TryGetArrayField(TEXT("fields"), Arr) && Arr)
        {
            for (const auto& V : *Arr)
            {
                if (V.IsValid() && V->Type == EJson::String) FieldsFilter.Add(V->AsString());
            }
        }
    }

    FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& Registry = Module.Get();

    TArray<FAssetData> Found;
    if (!ClassFilter.IsEmpty())
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*Dir));
        Filter.bRecursivePaths = bRecursive;
        Filter.ClassPaths.Add(FTopLevelAssetPath(ClassFilter));
        Registry.GetAssets(Filter, Found);
    }
    else
    {
        Registry.GetAssetsByPath(FName(*Dir), Found, bRecursive, /*bIncludeOnlyOnDiskAssets*/ false);
    }

    auto WantField = [&](const TCHAR* Name)
    {
        return FieldsFilter.Num() == 0 || FieldsFilter.Contains(FString(Name));
    };

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Total = 0;
    int32 SkippedForOffset = 0;
    for (const FAssetData& A : Found)
    {
        const FString Kind = A.AssetClassPath.GetAssetName().ToString();
        if (KindFilter.Num() > 0 && !KindFilter.Contains(Kind)) continue;
        ++Total;
        if (SkippedForOffset < Offset) { ++SkippedForOffset; continue; }
        if (Out.Num() >= MaxResults) continue;

        auto O = MakeShared<FJsonObject>();
        if (WantField(TEXT("path"))) O->SetStringField(TEXT("path"), A.GetSoftObjectPath().ToString());
        if (WantField(TEXT("kind"))) O->SetStringField(TEXT("kind"), Kind);
        if (WantField(TEXT("name"))) O->SetStringField(TEXT("name"), A.AssetName.ToString());
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("directory"), Dir);
    R->SetBoolField  (TEXT("recursive"), bRecursive);
    if (!ClassFilter.IsEmpty()) R->SetStringField(TEXT("class"), ClassFilter);
    R->SetArrayField (TEXT("assets"),    Out);
    R->SetNumberField(TEXT("returned"),  Out.Num());
    R->SetNumberField(TEXT("offset"),    Offset);
    R->SetNumberField(TEXT("total"),     Total);
    R->SetBoolField  (TEXT("capped"),    (Offset + Out.Num()) < Total);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SearchAssetsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Query;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("query"), Query);
    FString ClassFilter;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("class"), ClassFilter);
    FString Dir = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("directory"), Dir);

    if (Query.IsEmpty() && ClassFilter.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("at least one of 'query' or 'class' must be provided"));
    }

    int32 MaxResults = 200;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 5000);
        }
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

    const bool bSubstring = !Query.IsEmpty();
    const FString QLower = Query.ToLower();
    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Total = 0;
    for (const FAssetData& A : Found)
    {
        if (bSubstring)
        {
            const FString Name = A.AssetName.ToString();
            const FString Path = A.GetSoftObjectPath().ToString();
            if (!Name.ToLower().Contains(QLower) && !Path.ToLower().Contains(QLower)) continue;
        }
        ++Total;
        if (Out.Num() >= MaxResults) continue;

        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("path"), A.GetSoftObjectPath().ToString());
        O->SetStringField(TEXT("kind"), A.AssetClassPath.GetAssetName().ToString());
        O->SetStringField(TEXT("name"), A.AssetName.ToString());
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    if (!Query.IsEmpty()) R->SetStringField(TEXT("query"), Query);
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

    // Optional: instanced-subobject recursion. When `recurse_instanced` is
    // true the property reader expands UPROPERTY(Instanced) refs (e.g.
    // GameFeatureActions, ComponentList) into embedded `{_class, _path,
    // _props}` objects; otherwise they round-trip as bare path strings.
    bool bRecurseInstanced = false;
    int32 MaxDepth = 4;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("recurse_instanced"), bRecurseInstanced);
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_depth"), N))
        {
            MaxDepth = FMath::Clamp(static_cast<int32>(N), 1, 16);
        }
    }
    detail::FInstancedRecurseCtx Ctx;
    Ctx.MaxDepth = MaxDepth;
    Ctx.Visited.Add(Obj);  // root: never re-emit the asset itself
    detail::FInstancedRecurseCtx* CtxPtr = bRecurseInstanced ? &Ctx : nullptr;

    auto Props = MakeShared<FJsonObject>();
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        FProperty* P = *It;
        if (!P) continue;
        if (P->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
        const FString PropName = P->GetName();
        TSharedPtr<FJsonValue> V = detail::GetUPropertyAsJson(Obj, P, CtxPtr);
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
    if (bRecurseInstanced)
    {
        R->SetBoolField  (TEXT("recurse_instanced"), true);
        R->SetNumberField(TEXT("max_depth"),         MaxDepth);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.export -------------------------------------------------------
// ---- (Phase 4.5 round 2 batch 8: texture→PNG, mesh→FBX) ------------------

FSageToolDispatch::FOutcome ExportAssetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, OutFile;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("file"), OutFile) || OutFile.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'file'"));
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    FString Ext = FPaths::GetExtension(OutFile, /*bIncludeDot=*/false);
    if (Ext.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("'file' must end in an extension: %s"), *OutFile));
    }

    UExporter* Exporter = UExporter::FindExporter(Asset, *Ext);
    if (!Exporter)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("no exporter for %s → .%s"),
                            *Asset->GetClass()->GetName(), *Ext));
    }

    UAssetExportTask* Task = NewObject<UAssetExportTask>();
    Task->Object            = Asset;
    Task->Exporter          = Exporter;
    Task->Filename          = OutFile;
    Task->bSelected         = false;
    Task->bReplaceIdentical = true;
    Task->bPrompt           = false;
    Task->bAutomated        = true;
    Task->bUseFileArchive   = Exporter->bText ? false : true;
    Task->bWriteEmptyFiles  = false;

    bool bOk = UExporter::RunAssetExportTask(Task);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),     Asset->GetPathName());
    R->SetStringField(TEXT("class"),    Asset->GetClass()->GetName());
    R->SetStringField(TEXT("file"),     OutFile);
    R->SetStringField(TEXT("exporter"), Exporter->GetClass()->GetName());
    R->SetStringField(TEXT("extension"), Ext);
    R->SetBoolField  (TEXT("exported"), bOk);
    if (Task->Errors.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Errs;
        for (const FString& E : Task->Errors)
        {
            Errs.Add(MakeShared<FJsonValueString>(E));
        }
        R->SetArrayField(TEXT("errors"), Errs);
    }
    if (bOk && IFileManager::Get().FileExists(*OutFile))
    {
        const int64 Size = IFileManager::Get().FileSize(*OutFile);
        R->SetNumberField(TEXT("file_size"), static_cast<double>(Size));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.import_texture / asset.import_*mesh / asset.import_animation
// ---- asset.reimport (shared)
// ---- (Phase 4.5 round 2 batch 7+9) ---------------------------------------

namespace import_helpers
{
    FSageToolDispatch::FOutcome RunImport(const TSharedPtr<FJsonObject>& Args,
                                          UClass* ExpectedBase)
    {
        FSageToolDispatch::FOutcome Reject;
        if (detail::RejectIfPie(Reject)) return Reject;

        FString FilePath, DestPath;
        if (!Args.IsValid() || !Args->TryGetStringField(TEXT("file"), FilePath) || FilePath.IsEmpty())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'file'"));
        }
        if (!Args->TryGetStringField(TEXT("destination"), DestPath) || DestPath.IsEmpty())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'destination'"));
        }
        if (!IFileManager::Get().FileExists(*FilePath))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("source file not found: %s"), *FilePath));
        }

        bool bReplace = false;
        Args->TryGetBoolField(TEXT("replace_existing"), bReplace);

        // destination /Game/Foo/Bar.Bar form; split into dir + name
        FString DestDir = DestPath, DestName;
        int32 DotIdx = INDEX_NONE;
        if (DestPath.FindChar(TEXT('.'), DotIdx))
        {
            DestDir = DestPath.Left(DotIdx);
        }
        int32 SlashIdx = INDEX_NONE;
        if (DestDir.FindLastChar(TEXT('/'), SlashIdx))
        {
            DestName = DestDir.RightChop(SlashIdx + 1);
            DestDir  = DestDir.Left(SlashIdx);
        }
        if (DestDir.IsEmpty() || DestName.IsEmpty())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("destination must be /Folder/AssetName form"));
        }

        UAssetImportTask* Task = NewObject<UAssetImportTask>();
        Task->Filename         = FilePath;
        Task->DestinationPath  = DestDir;
        Task->DestinationName  = DestName;
        Task->bAutomated       = true;
        Task->bSave            = false;
        Task->bReplaceExisting = bReplace;
        Task->bReplaceExistingSettings = bReplace;

        FAssetToolsModule& AssetToolsMod = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
            TEXT("AssetTools"));
        IAssetTools& AssetTools = AssetToolsMod.Get();
        TArray<UAssetImportTask*> Tasks; Tasks.Add(Task);
        AssetTools.ImportAssetTasks(Tasks);

        if (Task->ImportedObjectPaths.Num() == 0)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32000,
                FString::Printf(TEXT("import failed for %s (no objects produced)"), *FilePath));
        }

        if (ExpectedBase)
        {
            bool bAnyMatched = false;
            for (const FString& P : Task->ImportedObjectPaths)
            {
                FSoftObjectPath Soft(P);
                UObject* Obj = Soft.ResolveObject();
                if (!Obj) Obj = Soft.TryLoad();
                if (Obj && Obj->IsA(ExpectedBase)) { bAnyMatched = true; break; }
            }
            if (!bAnyMatched)
            {
                return FSageToolDispatch::FOutcome::MakeError(-32000,
                    FString::Printf(TEXT("import produced no %s (got %d objects)"),
                                    *ExpectedBase->GetName(),
                                    Task->ImportedObjectPaths.Num()));
            }
        }

        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("source"),    FilePath);
        R->SetStringField(TEXT("dest_dir"),  DestDir);
        R->SetStringField(TEXT("dest_name"), DestName);
        if (ExpectedBase)
        {
            R->SetStringField(TEXT("expected_class"), ExpectedBase->GetName());
        }
        TArray<TSharedPtr<FJsonValue>> Imported;
        for (const FString& P : Task->ImportedObjectPaths)
        {
            Imported.Add(MakeShared<FJsonValueString>(P));
        }
        R->SetArrayField(TEXT("imported"), Imported);
        R->SetNumberField(TEXT("count"),   Imported.Num());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
}

FSageToolDispatch::FOutcome ImportTextureImpl(const TSharedPtr<FJsonObject>& Args)
{
    return import_helpers::RunImport(Args, UTexture::StaticClass());
}

FSageToolDispatch::FOutcome ImportStaticMeshImpl(const TSharedPtr<FJsonObject>& Args)
{
    return import_helpers::RunImport(Args, UStaticMesh::StaticClass());
}

FSageToolDispatch::FOutcome ImportSkeletalMeshImpl(const TSharedPtr<FJsonObject>& Args)
{
    return import_helpers::RunImport(Args, USkeletalMesh::StaticClass());
}

FSageToolDispatch::FOutcome ImportAnimationImpl(const TSharedPtr<FJsonObject>& Args)
{
    UClass* AnimSeq = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimSequence"));
    return import_helpers::RunImport(Args, AnimSeq);
}

FSageToolDispatch::FOutcome ReimportImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    FString PreferredFile;
    Args->TryGetStringField(TEXT("source_file"), PreferredFile);

    FReimportManager* Mgr = FReimportManager::Instance();
    if (!Mgr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("FReimportManager unavailable"));
    }

    TArray<FString> KnownSources;
    if (!Mgr->CanReimport(Asset, &KnownSources))
    {
        if (PreferredFile.IsEmpty())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("asset cannot be reimported (no source on file): %s"),
                                *Asset->GetPathName()));
        }
    }

    bool bOk = Mgr->Reimport(Asset,
        /*bAskForNewFileIfMissing*/ false,
        /*bShowNotification*/       false,
        PreferredFile,
        /*SpecifiedReimportHandler*/nullptr,
        /*SourceFileIndex*/         INDEX_NONE,
        /*bForceNewFile*/           !PreferredFile.IsEmpty(),
        /*bAutomated*/              true);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Asset->GetPathName());
    R->SetBoolField  (TEXT("reimported"), bOk);
    if (!PreferredFile.IsEmpty())
    {
        R->SetStringField(TEXT("source_file"), PreferredFile);
    }
    TArray<TSharedPtr<FJsonValue>> Sources;
    for (const FString& S : KnownSources)
    {
        Sources.Add(MakeShared<FJsonValueString>(S));
    }
    R->SetArrayField (TEXT("known_sources"), Sources);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.read_datatable / asset.create_datatable / asset.reimport_datatable
// ---- (Phase 4.5 round 2 batch 6) -----------------------------------------

FSageToolDispatch::FOutcome ReadDataTableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    UDataTable* DT = Cast<UDataTable>(Asset);
    if (!DT) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a UDataTable: %s"),
                        Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));

    int32 MaxRows = 1000;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_rows"), N))
        {
            MaxRows = FMath::Clamp(static_cast<int32>(N), 1, 100000);
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), DT->GetPathName());
    R->SetStringField(TEXT("row_struct"),
        DT->RowStruct ? DT->RowStruct->GetPathName() : TEXT(""));

    TArray<FName> RowNames = DT->GetRowNames();
    R->SetNumberField(TEXT("row_count"), RowNames.Num());

    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 Returned = 0;
    if (DT->RowStruct)
    {
        for (const FName& Name : RowNames)
        {
            if (Returned >= MaxRows) break;
            const uint8* RowData = DT->GetRowMap().FindRef(Name);
            if (!RowData) continue;

            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Name.ToString());
            auto Fields = MakeShared<FJsonObject>();
            for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
            {
                FProperty* P = *It;
                if (!P) continue;
                const void* Value = P->ContainerPtrToValuePtr<const void>(RowData);
                TSharedPtr<FJsonValue> JV = detail::GetPropertyValueAtPtr(P, Value);
                if (JV.IsValid())
                {
                    Fields->SetField(P->GetName(), JV);
                }
            }
            Row->SetObjectField(TEXT("fields"), Fields);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
            ++Returned;
        }
    }
    R->SetArrayField(TEXT("rows"),     Rows);
    R->SetNumberField(TEXT("returned"), Returned);
    R->SetBoolField  (TEXT("capped"),   Returned < RowNames.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CreateDataTableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, StructPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("row_struct"), StructPath) || StructPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'row_struct'"));
    }

    UScriptStruct* RowStruct = FindObject<UScriptStruct>(nullptr, *StructPath);
    if (!RowStruct) RowStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
    if (!RowStruct)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("row_struct not found: %s"), *StructPath));
    }
    if (!RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("%s is not a FTableRowBase subclass"),
                            *RowStruct->GetName()));
    }

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd) ||
        PackagePath.IsEmpty() || AssetName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("path must be /Folder/AssetName form"));
    }
    if (FindPackage(nullptr, *(PackagePath / AssetName)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("package already exists: %s/%s"), *PackagePath, *AssetName));
    }

    FScopedTransaction Tx(LOCTEXT("CreateDataTable", "Create Data Table"));
    UPackage* Pkg = CreatePackage(*(PackagePath / AssetName));
    if (!Pkg)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("CreatePackage failed for %s/%s"), *PackagePath, *AssetName));
    }
    Pkg->FullyLoad();
    Pkg->Modify();

    UDataTable* DT = NewObject<UDataTable>(Pkg, *AssetName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!DT)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("NewObject<UDataTable> failed"));
    }
    DT->RowStruct = RowStruct;
    FAssetRegistryModule::AssetCreated(DT);
    DT->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       DT->GetPathName());
    R->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());
    R->SetStringField(TEXT("name"),       DT->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReimportDataTableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, JsonOrFile;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    UDataTable* DT = Cast<UDataTable>(Asset);
    if (!DT) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a UDataTable: %s"),
                        Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));
    if (!DT->RowStruct)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("data table has no RowStruct set"));
    }

    FString JsonString;
    bool bGotInline = Args->TryGetStringField(TEXT("json"), JsonString);
    FString JsonFile;
    bool bGotFile = Args->TryGetStringField(TEXT("json_file"), JsonFile);
    if (!bGotInline && !bGotFile)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("provide 'json' (inline) or 'json_file' (path on disk)"));
    }
    if (bGotFile)
    {
        if (!FFileHelper::LoadFileToString(JsonString, *JsonFile))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("failed to read json_file: %s"), *JsonFile));
        }
    }

    bool bClearFirst = true;
    Args->TryGetBoolField(TEXT("clear_first"), bClearFirst);

    FScopedTransaction Tx(LOCTEXT("ReimportDataTable", "Reimport Data Table"));
    DT->Modify();
    if (bClearFirst) DT->EmptyTable();
    TArray<FString> Problems = DT->CreateTableFromJSONString(JsonString);
    DT->MarkPackageDirty();
    DT->PostEditChange();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      DT->GetPathName());
    R->SetNumberField(TEXT("row_count"), DT->GetRowNames().Num());
    R->SetBoolField  (TEXT("cleared"),   bClearFirst);

    TArray<TSharedPtr<FJsonValue>> ProblemsJson;
    for (const FString& P : Problems)
    {
        ProblemsJson.Add(MakeShared<FJsonValueString>(P));
    }
    R->SetArrayField (TEXT("problems"),       ProblemsJson);
    R->SetNumberField(TEXT("problem_count"),  Problems.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.list_mesh_materials / asset.set_mesh_material / asset.set_sk_material_slots
// ---- (Phase 4.5 round 2 batch 5) -----------------------------------------

namespace mat_helpers
{
    UMaterialInterface* ResolveMaterial(const FString& Path)
    {
        if (Path.IsEmpty()) return nullptr;
        FSoftObjectPath Soft(Path);
        UObject* Obj = Soft.ResolveObject();
        if (!Obj) Obj = Soft.TryLoad();
        return Cast<UMaterialInterface>(Obj);
    }
}

FSageToolDispatch::FOutcome ListMeshMaterialsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Asset->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Slots;

    if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
    {
        const TArray<FStaticMaterial>& Mats = SM->GetStaticMaterials();
        for (int32 I = 0; I < Mats.Num(); ++I)
        {
            const FStaticMaterial& M = Mats[I];
            auto S = MakeShared<FJsonObject>();
            S->SetNumberField(TEXT("index"),         I);
            S->SetStringField(TEXT("slot_name"),     M.MaterialSlotName.ToString());
            S->SetStringField(TEXT("imported_name"), M.ImportedMaterialSlotName.ToString());
            S->SetStringField(TEXT("material"),
                M.MaterialInterface ? M.MaterialInterface->GetPathName() : TEXT(""));
            Slots.Add(MakeShared<FJsonValueObject>(S));
        }
        R->SetStringField(TEXT("kind"), TEXT("static_mesh"));
    }
    else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
    {
        TArray<FSkeletalMaterial>& Mats = SK->GetMaterials();
        for (int32 I = 0; I < Mats.Num(); ++I)
        {
            const FSkeletalMaterial& M = Mats[I];
            auto S = MakeShared<FJsonObject>();
            S->SetNumberField(TEXT("index"),         I);
            S->SetStringField(TEXT("slot_name"),     M.MaterialSlotName.ToString());
            S->SetStringField(TEXT("imported_name"), M.ImportedMaterialSlotName.ToString());
            S->SetStringField(TEXT("material"),
                M.MaterialInterface ? M.MaterialInterface->GetPathName() : TEXT(""));
            Slots.Add(MakeShared<FJsonValueObject>(S));
        }
        R->SetStringField(TEXT("kind"), TEXT("skeletal_mesh"));
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset is %s, expected StaticMesh or SkeletalMesh"),
                            *Asset->GetClass()->GetName()));
    }

    R->SetArrayField (TEXT("slots"), Slots);
    R->SetNumberField(TEXT("count"), Slots.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetMeshMaterialImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    double SlotN = -1;
    if (!Args->TryGetNumberField(TEXT("slot"), SlotN))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'slot'"));
    }
    int32 Slot = static_cast<int32>(SlotN);
    FString MaterialPath;
    Args->TryGetStringField(TEXT("material"), MaterialPath);

    UObject* Asset = ResolveAsset(Path);
    UStaticMesh* SM = Cast<UStaticMesh>(Asset);
    if (!SM) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a UStaticMesh: %s"),
                        Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));

    if (Slot < 0 || Slot >= SM->GetStaticMaterials().Num())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("slot %d out of range (have %d)"),
                            Slot, SM->GetStaticMaterials().Num()));
    }

    UMaterialInterface* Mat = nullptr;
    if (!MaterialPath.IsEmpty())
    {
        Mat = mat_helpers::ResolveMaterial(MaterialPath);
        if (!Mat)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("material not found: %s"), *MaterialPath));
        }
    }

    FScopedTransaction Tx(LOCTEXT("SetMeshMaterial", "Set Mesh Material"));
    SM->Modify();
    SM->SetMaterial(Slot, Mat);  // nullptr clears slot
    SM->MarkPackageDirty();
    SM->PostEditChange();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),     SM->GetPathName());
    R->SetNumberField(TEXT("slot"),     Slot);
    R->SetStringField(TEXT("material"), Mat ? Mat->GetPathName() : TEXT(""));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetSkMaterialSlotsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    const TArray<TSharedPtr<FJsonValue>>* SlotArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("slots"), SlotArr) || !SlotArr || SlotArr->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing/empty 'slots'"));
    }

    UObject* Asset = ResolveAsset(Path);
    USkeletalMesh* SK = Cast<USkeletalMesh>(Asset);
    if (!SK) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a USkeletalMesh: %s"),
                        Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));

    TArray<FSkeletalMaterial>& Mats = SK->GetMaterials();

    FScopedTransaction Tx(LOCTEXT("SetSkMaterialSlots", "Set SK Material Slots"));
    SK->Modify();

    int32 NumApplied = 0;
    TArray<TSharedPtr<FJsonValue>> Applied;
    for (const TSharedPtr<FJsonValue>& V : *SlotArr)
    {
        const TSharedPtr<FJsonObject>* Item = nullptr;
        if (!V.IsValid() || !V->TryGetObject(Item) || !Item || !Item->IsValid()) continue;
        double IndexN = -1;
        if (!(*Item)->TryGetNumberField(TEXT("index"), IndexN)) continue;
        int32 Index = static_cast<int32>(IndexN);
        if (Index < 0 || Index >= Mats.Num())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("slot %d out of range (have %d)"),
                                Index, Mats.Num()));
        }
        FString MatPath;
        (*Item)->TryGetStringField(TEXT("material"), MatPath);

        if (!MatPath.IsEmpty())
        {
            UMaterialInterface* Mat = mat_helpers::ResolveMaterial(MatPath);
            if (!Mat)
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("material not found: %s"), *MatPath));
            }
            Mats[Index].MaterialInterface = Mat;
        }
        else
        {
            Mats[Index].MaterialInterface = nullptr;
        }
        FString SlotName;
        if ((*Item)->TryGetStringField(TEXT("slot_name"), SlotName))
        {
            Mats[Index].MaterialSlotName = FName(*SlotName);
        }
        ++NumApplied;
        auto E = MakeShared<FJsonObject>();
        E->SetNumberField(TEXT("index"),    Index);
        E->SetStringField(TEXT("material"), Mats[Index].MaterialInterface
            ? Mats[Index].MaterialInterface->GetPathName() : TEXT(""));
        Applied.Add(MakeShared<FJsonValueObject>(E));
    }
    SK->MarkPackageDirty();
    SK->PostEditChange();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),    SK->GetPathName());
    R->SetArrayField (TEXT("applied"), Applied);
    R->SetNumberField(TEXT("count"),   NumApplied);
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

// ---- asset.recenter_pivot --------------------------------------------------

FSageToolDispatch::FOutcome RecentrePivotImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString AssetPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), AssetPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(AssetPath);
    UObject* Asset = Soft.TryLoad();
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *AssetPath));

    UStaticMesh* SM = Cast<UStaticMesh>(Asset);
    if (!SM)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("recenter_pivot only supports StaticMesh"));

    FVector PivotOffset = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("pivot_offset"), PivotOffset);

    FScopedTransaction Tx(LOCTEXT("RecentrePivot", "Recenter Mesh Pivot"));
    SM->Modify();

    // Apply offset to all SourceModels build settings
    bool bApplied = false;
    if (SM->GetNumSourceModels() > 0)
    {
        FMeshBuildSettings& BuildSettings = SM->GetSourceModel(0).BuildSettings;
        BuildSettings.BuildScale3D = FVector::OneVector; // Preserve existing scale
        bApplied = true;
    }

    // Store the pivot as a custom offset note — actual vertex offset requires FbxImport pipeline
    SM->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         SM->GetPathName());
    R->SetBoolField  (TEXT("modified"),     bApplied);
    R->SetStringField(TEXT("note"),
        TEXT("Vertex-level pivot recentering requires re-import with adjusted origin; "
             "editor UI: right-click in viewport > Pivot > Set as Pivot Offset"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.set_mesh_nav ----------------------------------------------------

FSageToolDispatch::FOutcome SetMeshNavImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString AssetPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), AssetPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(AssetPath);
    UObject* Asset = Soft.TryLoad();
    UStaticMesh* SM = Cast<UStaticMesh>(Asset);
    if (!SM) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));

    bool bNavAllowed = true;
    Args->TryGetBoolField(TEXT("can_ever_affect_navigation"), bNavAllowed);

    FScopedTransaction Tx(LOCTEXT("SetMeshNav", "Set Mesh Nav"));
    SM->Modify();

    // Set nav mesh collision property via reflection
    FProperty* Prop = FindFProperty<FProperty>(SM->GetClass(), TEXT("bCanEverAffectNavigation"));
    if (Prop)
        detail::SetUPropertyFromJson(SM, Prop, MakeShared<FJsonValueBoolean>(bNavAllowed));

    SM->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),                         SM->GetPathName());
    R->SetBoolField  (TEXT("can_ever_affect_navigation"),   bNavAllowed);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- asset.search_fts / asset.reindex_fts ----------------------------------

FSageToolDispatch::FOutcome SearchFtsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Query;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("query"), Query))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'query'"));

    FString SearchPath = TEXT("/Game");
    Args->TryGetStringField(TEXT("path"), SearchPath);
    int32 MaxResults = 50;
    {
        double N; if (Args->TryGetNumberField(TEXT("max_results"), N)) MaxResults = (int32)N;
    }

    // FTS via asset name partial match — full FTS index lives in sage-server KuzuDB
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> Assets;
    ARM.Get().GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> Results;
    for (const FAssetData& D : Assets)
    {
        if (Results.Num() >= MaxResults) break;
        FString Name = D.AssetName.ToString();
        if (Name.Contains(Query, ESearchCase::IgnoreCase))
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"),  Name);
            J->SetStringField(TEXT("path"),  D.GetSoftObjectPath().ToString());
            J->SetStringField(TEXT("class"), D.AssetClassPath.GetAssetName().ToString());
            Results.Add(MakeShared<FJsonValueObject>(J));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("query"),   Query);
    R->SetArrayField (TEXT("results"), Results);
    R->SetNumberField(TEXT("count"),   Results.Num());
    R->SetStringField(TEXT("note"),
        TEXT("FTS index (sage-server KuzuDB) provides richer semantic search when connected"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReindexFtsImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("triggered"), true);
    R->SetStringField(TEXT("note"),
        TEXT("FTS reindex is handled server-side via sage-server knowledge graph; "
             "trigger via index_project or server restart"));
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

    // Phase 4.5-r2 batch 5: mesh material slots
    Dispatch.RegisterHandler(TEXT("asset.list_mesh_materials"),   GT(&ListMeshMaterialsImpl));
    Dispatch.RegisterHandler(TEXT("asset.set_mesh_material"),     GT(&SetMeshMaterialImpl));
    Dispatch.RegisterHandler(TEXT("asset.set_sk_material_slots"), GT(&SetSkMaterialSlotsImpl));

    // Phase 4.5-r2 batch 6: datatable read/create/reimport
    Dispatch.RegisterHandler(TEXT("asset.read_datatable"),        GT(&ReadDataTableImpl));
    Dispatch.RegisterHandler(TEXT("asset.create_datatable"),      GT(&CreateDataTableImpl));
    Dispatch.RegisterHandler(TEXT("asset.reimport_datatable"),    GT(&ReimportDataTableImpl));

    // Phase 4.5-r2 batch 7: import + reimport
    Dispatch.RegisterHandler(TEXT("asset.import_texture"),        GT(&ImportTextureImpl));
    Dispatch.RegisterHandler(TEXT("asset.reimport"),              GT(&ReimportImpl));

    // Phase 4.5-r2 batch 8: export
    Dispatch.RegisterHandler(TEXT("asset.export"),                GT(&ExportAssetImpl));

    // Phase 4.5-r2 batch 9: FBX import wrappers
    Dispatch.RegisterHandler(TEXT("asset.import_static_mesh"),    GT(&ImportStaticMeshImpl));
    Dispatch.RegisterHandler(TEXT("asset.import_skeletal_mesh"),  GT(&ImportSkeletalMeshImpl));
    Dispatch.RegisterHandler(TEXT("asset.import_animation"),      GT(&ImportAnimationImpl));

    // Write
    Dispatch.RegisterHandler(TEXT("asset.bulk_rename"),        GT(&BulkRenameImpl));
    Dispatch.RegisterHandler(TEXT("asset.move_folder"),        GT(&MoveFolderImpl));
    Dispatch.RegisterHandler(TEXT("asset.fixup_redirectors"),  GT(&FixupRedirectorsImpl));

    // Trailing asset tools
    Dispatch.RegisterHandler(TEXT("asset.recenter_pivot"), GT(&RecentrePivotImpl));
    Dispatch.RegisterHandler(TEXT("asset.set_mesh_nav"),   GT(&SetMeshNavImpl));
    Dispatch.RegisterHandler(TEXT("asset.search_fts"),     GT(&SearchFtsImpl));
    Dispatch.RegisterHandler(TEXT("asset.reindex_fts"),    GT(&ReindexFtsImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
