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

    // Write
    Dispatch.RegisterHandler(TEXT("asset.bulk_rename"),        GT(&BulkRenameImpl));
    Dispatch.RegisterHandler(TEXT("asset.move_folder"),        GT(&MoveFolderImpl));
    Dispatch.RegisterHandler(TEXT("asset.fixup_redirectors"),  GT(&FixupRedirectorsImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
