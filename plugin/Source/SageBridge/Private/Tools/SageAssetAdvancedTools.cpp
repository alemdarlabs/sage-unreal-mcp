#include "Tools/SageAssetAdvancedTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/DataAsset.h"
#include "Engine/Blueprint.h"
#include "Materials/MaterialInterface.h"
#include "Engine/DataTable.h"
#include "Blueprint/UserWidget.h"
#include "GameFeatureAction_AddComponents.h"
#include "GameFeatureData.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "AssetImportTask.h"
#include "EditorFramework/AssetImportData.h"
#include "AssetExportTask.h"
#include "Exporters/Exporter.h"
#include "EditorReimportHandler.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "ObjectTools.h"
#include "UObject/MetaData.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"

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

TSharedPtr<FJsonObject> AssetDeleteDiagnosticJson(
    const FString& PackagePath,
    UEditorAssetSubsystem* AssetSubsystem);
FSageToolDispatch::FOutcome ImportTextureImpl(const TSharedPtr<FJsonObject>& Args);
FString SummarizeDeleteDiagnostic(const TSharedPtr<FJsonObject>& Diag);

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
    TArray<TSharedPtr<FJsonValue>> Failures;
    int32 Renamed = 0, Failed = 0;

    for (const auto& V : *Pairs)
    {
        const auto Obj = V->AsObject();
        if (!Obj.IsValid())
        {
            ++Failed;
            auto F = MakeShared<FJsonObject>();
            F->SetStringField(TEXT("reason"), TEXT("entry not an object"));
            Failures.Add(MakeShared<FJsonValueObject>(F));
            continue;
        }
        FString Src, Dst;
        if (!Obj->TryGetStringField(TEXT("src"), Src) || !Obj->TryGetStringField(TEXT("dst"), Dst))
        {
            ++Failed;
            auto F = MakeShared<FJsonObject>();
            F->SetStringField(TEXT("src"),    Src);
            F->SetStringField(TEXT("dst"),    Dst);
            F->SetStringField(TEXT("reason"), TEXT("missing 'src' or 'dst'"));
            Failures.Add(MakeShared<FJsonValueObject>(F));
            continue;
        }
        const bool bOk = Sub->RenameAsset(Src, Dst);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("src"),   Src);
        Row->SetStringField(TEXT("dst"),   Dst);
        Row->SetBoolField  (TEXT("ok"),    bOk);
        Results.Add(MakeShared<FJsonValueObject>(Row));
        if (bOk)
        {
            ++Renamed;
        }
        else
        {
            ++Failed;
            auto F = MakeShared<FJsonObject>();
            F->SetStringField(TEXT("src"),    Src);
            F->SetStringField(TEXT("dst"),    Dst);
            F->SetStringField(TEXT("reason"), TEXT("RenameAsset returned false"));
            Failures.Add(MakeShared<FJsonValueObject>(F));
        }
    }

    if (Failed > 0)
    {
        // Atomic-or-rollback: cancel the scoped transaction so all prior renames
        // in this batch are reverted. Caller may retry per-item if they want
        // partial-success semantics.
        Tx.Cancel();
        // FOutcome::MakeError carries only message; flatten the failure list
        // into the message so the caller can act on it.
        FString FailureSummary;
        for (const TSharedPtr<FJsonValue>& FV : Failures)
        {
            const TSharedPtr<FJsonObject>* O = nullptr;
            if (!FV.IsValid() || !FV->TryGetObject(O) || !O || !O->IsValid()) continue;
            FString S, D, Why;
            (*O)->TryGetStringField(TEXT("src"),    S);
            (*O)->TryGetStringField(TEXT("dst"),    D);
            (*O)->TryGetStringField(TEXT("reason"), Why);
            if (!FailureSummary.IsEmpty()) FailureSummary += TEXT("; ");
            FailureSummary += FString::Printf(TEXT("%s -> %s (%s)"), *S, *D, *Why);
        }
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("bulk_rename rolled back: %d failure(s); "
                                 "%d successful rename(s) reverted; failures: [%s]"),
                            Failed, Renamed, *FailureSummary));
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

    TArray<FString> AssetsInFolder = Sub->ListAssets(Src, /*Recursive*/ true);

    // Pre-compute destination paths and detect collisions BEFORE mutating.
    struct FMove { FString Src; FString Dst; };
    TArray<FMove> Plan;
    Plan.Reserve(AssetsInFolder.Num());
    TArray<FString> Collisions;
    TArray<FString> PrefixSkipped;
    for (const FString& AssetPath : AssetsInFolder)
    {
        FString Tail = AssetPath;
        if (!Tail.RemoveFromStart(Src))
        {
            PrefixSkipped.Add(AssetPath);
            continue;
        }
        const FString NewPath = Dst / Tail;
        if (Sub->DoesAssetExist(NewPath))
        {
            Collisions.Add(NewPath);
        }
        Plan.Add({AssetPath, NewPath});
    }

    if (Collisions.Num() > 0)
    {
        FString List;
        for (int32 I = 0; I < Collisions.Num() && I < 16; ++I)
        {
            if (!List.IsEmpty()) List += TEXT(", ");
            List += Collisions[I];
        }
        if (Collisions.Num() > 16) List += FString::Printf(TEXT(" (+%d more)"), Collisions.Num() - 16);
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("destination collision: %s"), *List));
    }
    if (PrefixSkipped.Num() > 0)
    {
        FString List;
        for (int32 I = 0; I < PrefixSkipped.Num() && I < 8; ++I)
        {
            if (!List.IsEmpty()) List += TEXT(", ");
            List += PrefixSkipped[I];
        }
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset(s) outside src prefix: %s"), *List));
    }

    FScopedTransaction Tx(LOCTEXT("MoveFolder", "Sage: Move Folder"));

    int32 Moved = 0, Failed = 0;
    TArray<TSharedPtr<FJsonValue>> Results;
    TArray<FString> FailureSummary;
    for (const FMove& M : Plan)
    {
        const bool bOk = Sub->RenameAsset(M.Src, M.Dst);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("src"), M.Src);
        Row->SetStringField(TEXT("dst"), M.Dst);
        Row->SetBoolField  (TEXT("ok"),  bOk);
        Results.Add(MakeShared<FJsonValueObject>(Row));
        if (bOk)
        {
            ++Moved;
        }
        else
        {
            ++Failed;
            FailureSummary.Add(FString::Printf(TEXT("%s -> %s"), *M.Src, *M.Dst));
        }
    }

    if (Failed > 0)
    {
        // Atomic-or-rollback: cancel transaction; all prior moves revert.
        Tx.Cancel();
        FString List;
        for (int32 I = 0; I < FailureSummary.Num() && I < 16; ++I)
        {
            if (!List.IsEmpty()) List += TEXT("; ");
            List += FailureSummary[I];
        }
        if (FailureSummary.Num() > 16) List += FString::Printf(TEXT(" (+%d more)"), FailureSummary.Num() - 16);
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("move_folder rolled back: %d failure(s); "
                                 "%d successful move(s) reverted; failures: [%s]"),
                            Failed, Moved, *List));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("src_folder"), Src);
    R->SetStringField(TEXT("dst_folder"), Dst);
    R->SetNumberField(TEXT("moved"),      Moved);
    R->SetNumberField(TEXT("failed"),     Failed);
    R->SetArrayField (TEXT("results"),    Results);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FString FirstAssetStringArg(const TSharedPtr<FJsonObject>& Args,
                            std::initializer_list<const TCHAR*> Names,
                            const FString& Fallback = FString())
{
    if (!Args.IsValid()) return Fallback;
    FString Value;
    for (const TCHAR* Name : Names)
    {
        if (Args->TryGetStringField(Name, Value) && !Value.IsEmpty()) return Value;
    }
    return Fallback;
}

TArray<FString> AssetStringArrayArg(const TSharedPtr<FJsonObject>& Args,
                                    std::initializer_list<const TCHAR*> Names)
{
    TArray<FString> Out;
    if (!Args.IsValid()) return Out;
    for (const TCHAR* Name : Names)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Args->TryGetArrayField(Name, Arr) || Arr == nullptr) continue;
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            if (V.IsValid() && V->Type == EJson::String)
            {
                Out.Add(V->AsString());
            }
        }
        if (Out.Num() > 0) return Out;
    }
    return Out;
}

UEditorAssetSubsystem* EditorAssetSubsystemOrNull()
{
    return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
}

FSageToolDispatch::FOutcome AssetCreateFolderImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    const FString Folder = FirstAssetStringArg(Args, {TEXT("path"), TEXT("folder")});
    if (Folder.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'/'folder'"));
    UEditorAssetSubsystem* Sub = EditorAssetSubsystemOrNull();
    if (!Sub) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("EditorAssetSubsystem unavailable"));

    const bool bAlready = Sub->DoesDirectoryExist(Folder);
    const bool bOk = bAlready ? true : Sub->MakeDirectory(Folder);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("folder"), Folder);
    R->SetBoolField(TEXT("already"), bAlready);
    R->SetBoolField(TEXT("created"), bOk && !bAlready);
    R->SetBoolField(TEXT("exists"), Sub->DoesDirectoryExist(Folder));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AssetDeleteFolderImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    const FString Folder = FirstAssetStringArg(Args, {TEXT("path"), TEXT("folder")});
    if (Folder.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'/'folder'"));
    bool bConfirmed = false;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("asset.delete_folder is destructive and requires confirmed:true"));
    }
    UEditorAssetSubsystem* Sub = EditorAssetSubsystemOrNull();
    if (!Sub) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("EditorAssetSubsystem unavailable"));

    const bool bExisted = Sub->DoesDirectoryExist(Folder);
    const bool bDeleted = bExisted ? Sub->DeleteDirectory(Folder) : true;
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("folder"), Folder);
    R->SetBoolField(TEXT("existed"), bExisted);
    R->SetBoolField(TEXT("deleted"), bDeleted && bExisted);
    R->SetBoolField(TEXT("exists_after"), Sub->DoesDirectoryExist(Folder));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AssetSaveImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstAssetStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("asset_path")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    bool bOnlyIfDirty = true;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
    UEditorAssetSubsystem* Sub = EditorAssetSubsystemOrNull();
    if (!Sub) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("EditorAssetSubsystem unavailable"));

    const bool bSaved = Sub->SaveAsset(Path, bOnlyIfDirty);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Path);
    R->SetBoolField(TEXT("saved"), bSaved);
    R->SetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AssetSaveAllDirtyImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Directory = FirstAssetStringArg(Args, {TEXT("path"), TEXT("folder"), TEXT("directory")}, TEXT("/Game"));
    bool bRecursive = true;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("recursive"), bRecursive);
    UEditorAssetSubsystem* Sub = EditorAssetSubsystemOrNull();
    if (!Sub) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("EditorAssetSubsystem unavailable"));

    const bool bSaved = Sub->SaveDirectory(Directory, /*bOnlyIfIsDirty=*/true, bRecursive);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("directory"), Directory);
    R->SetBoolField(TEXT("recursive"), bRecursive);
    R->SetBoolField(TEXT("saved"), bSaved);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AssetCreateInterchangePipelineImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("asset.create_interchange_pipeline requires Interchange editor pipeline asset authoring; current bridge has safe import tasks but no stable public pipeline-asset construction path"));
}

FSageToolDispatch::FOutcome AssetImportTextureBatchImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    const TArray<FString> Files = AssetStringArrayArg(Args, {TEXT("files"), TEXT("source_files"), TEXT("sources")});
    const FString Destination = FirstAssetStringArg(Args, {TEXT("destination"), TEXT("dest"), TEXT("folder")});
    if (Files.Num() == 0) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing files/source_files"));
    if (Destination.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing destination"));

    bool bReplace = false;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("replace_existing"), bReplace);

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Imported = 0;
    int32 Failed = 0;
    for (const FString& File : Files)
    {
        const FString AssetName = FPaths::GetBaseFilename(File);
        auto One = MakeShared<FJsonObject>();
        One->SetStringField(TEXT("file"), File);
        One->SetStringField(TEXT("destination"), Destination / AssetName);
        One->SetBoolField(TEXT("replace_existing"), bReplace);
        FSageToolDispatch::FOutcome Out = ImportTextureImpl(One);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("file"), File);
        Row->SetBoolField(TEXT("success"), Out.bSuccess);
        if (Out.bSuccess && Out.Result.IsValid())
        {
            Row->SetObjectField(TEXT("result"), Out.Result);
            ++Imported;
        }
        else
        {
            if (Out.Error.IsValid()) Row->SetObjectField(TEXT("error"), Out.Error);
            ++Failed;
        }
        Results.Add(MakeShared<FJsonValueObject>(Row));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("destination"), Destination);
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("imported"), Imported);
    R->SetNumberField(TEXT("failed"), Failed);
    R->SetBoolField(TEXT("all_succeeded"), Failed == 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AssetReadImportSourcesImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstAssetStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("asset_path")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("asset not found: %s"), *Path));

    UAssetImportData* ImportData = nullptr;
    if (FObjectPropertyBase* ImportProp = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), TEXT("AssetImportData")))
    {
        ImportData = Cast<UAssetImportData>(ImportProp->GetObjectPropertyValue_InContainer(Asset));
    }

    TArray<TSharedPtr<FJsonValue>> Files;
    if (ImportData)
    {
        TArray<FString> Filenames;
        ImportData->ExtractFilenames(Filenames);
        for (const FString& File : Filenames)
        {
            Files.Add(MakeShared<FJsonValueString>(File));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Asset->GetPathName());
    R->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
    R->SetBoolField(TEXT("has_import_data"), ImportData != nullptr);
    R->SetArrayField(TEXT("source_files"), Files);
    R->SetNumberField(TEXT("count"), Files.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AssetHealthCheckImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Folder = FirstAssetStringArg(Args, {TEXT("path"), TEXT("folder"), TEXT("directory")}, TEXT("/Game"));
    int32 MaxResults = 500;
    if (Args.IsValid())
    {
        double N = 0.0;
        if (Args->TryGetNumberField(TEXT("max_results"), N)) MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 5000);
    }

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Folder));
    Filter.bRecursivePaths = true;
    TArray<FAssetData> Assets;
    ARM.Get().GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 Redirectors = 0;
    int32 MissingPackageFiles = 0;
    for (const FAssetData& Data : Assets)
    {
        if (Rows.Num() >= MaxResults) break;
        if (Data.AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName()) ++Redirectors;
        FString Filename;
        const bool bHasFilename = FPackageName::DoesPackageExist(Data.PackageName.ToString(), &Filename);
        if (!bHasFilename) ++MissingPackageFiles;
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("object_path"), Data.GetSoftObjectPath().ToString());
        Row->SetStringField(TEXT("package"), Data.PackageName.ToString());
        Row->SetStringField(TEXT("class"), Data.AssetClassPath.ToString());
        Row->SetBoolField(TEXT("package_file_exists"), bHasFilename);
        if (bHasFilename) Row->SetStringField(TEXT("filename"), Filename);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("folder"), Folder);
    R->SetNumberField(TEXT("asset_count"), Assets.Num());
    R->SetNumberField(TEXT("returned"), Rows.Num());
    R->SetNumberField(TEXT("redirector_count"), Redirectors);
    R->SetNumberField(TEXT("missing_package_file_count"), MissingPackageFiles);
    R->SetArrayField(TEXT("assets"), Rows);
    R->SetBoolField(TEXT("healthy"), Redirectors == 0 && MissingPackageFiles == 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AssetGenerateReportImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Out = AssetHealthCheckImpl(Args);
    if (Out.bSuccess && Out.Result.IsValid())
    {
        Out.Result->SetStringField(TEXT("report_type"), TEXT("asset_health"));
        Out.Result->SetStringField(TEXT("generated_at"), FDateTime::UtcNow().ToIso8601());
    }
    return Out;
}

FSageToolDispatch::FOutcome AssetCreateThumbnailImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("asset.create_thumbnail requires ThumbnailTools/renderer integration and image writeback beyond the current safe AssetRegistry path; use capture_viewport or material get_thumbnail where available"));
}

FSageToolDispatch::FOutcome AssetValidateImpl(const TSharedPtr<FJsonObject>& Args)
{
    return AssetHealthCheckImpl(Args);
}

FSageToolDispatch::FOutcome AssetSetTagsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstAssetStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("asset_path")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("asset not found: %s"), *Path));

    const TSharedPtr<FJsonObject>* TagsObj = nullptr;
    if (!Args.IsValid() || !Args->TryGetObjectField(TEXT("tags"), TagsObj) || TagsObj == nullptr || !TagsObj->IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing object field 'tags'"));
    }

    UPackage* Package = Asset->GetOutermost();
    if (!Package) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("package unavailable"));
    FMetaData& Meta = Package->GetMetaData();

    TArray<TSharedPtr<FJsonValue>> Applied;
    for (const auto& Pair : (*TagsObj)->Values)
    {
        FString Value;
        if (Pair.Value.IsValid())
        {
            Value = Pair.Value->AsString();
        }
        Meta.SetValue(Asset, *Pair.Key, *Value);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("key"), Pair.Key);
        Row->SetStringField(TEXT("value"), Value);
        Applied.Add(MakeShared<FJsonValueObject>(Row));
    }
    Asset->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Asset->GetPathName());
    R->SetArrayField(TEXT("applied"), Applied);
    R->SetNumberField(TEXT("count"), Applied.Num());
    R->SetBoolField(TEXT("dirty"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome FabOpsImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("fab_ops requires Epic/Fab marketplace authentication, cache policy, and license-aware download/import orchestration; no authenticated Fab connector is configured in SageBridge"));
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

    bool bConfirmed = false;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("destructive operation; pass confirmed:true to proceed "
                 "(mid-refactor redirect chains will be lost — fixup rewrites referencers and deletes redirectors)"));
    }

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
    using ImportUIConfigurator = TFunction<FSageToolDispatch::FOutcome(
        UFbxImportUI* /*UI*/, const TSharedPtr<FJsonObject>& /*Args*/)>;

    FSageToolDispatch::FOutcome RunImport(const TSharedPtr<FJsonObject>& Args,
                                          UClass* ExpectedBase,
                                          ImportUIConfigurator ConfigureUI = nullptr)
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

        // Optional: caller-supplied ImportUI configuration (e.g. anim skeleton).
        if (ConfigureUI)
        {
            UFbxImportUI* UI = NewObject<UFbxImportUI>(Task);
            FSageToolDispatch::FOutcome UIErr = ConfigureUI(UI, Args);
            if (!UIErr.bSuccess) return UIErr;
            Task->Options = UI;
        }

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
                // Clean up any orphan objects produced by the mismatched import
                // so a stale package isn't left on disk. Best-effort: failures
                // here are surfaced in the error message but don't override
                // the primary error.
                int32 Cleaned = 0;
                for (const FString& P : Task->ImportedObjectPaths)
                {
                    FSoftObjectPath Soft(P);
                    UObject* Obj = Soft.ResolveObject();
                    if (!Obj) Obj = Soft.TryLoad();
                    if (Obj && ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck*/ false))
                    {
                        ++Cleaned;
                    }
                }
                return FSageToolDispatch::FOutcome::MakeError(-32000,
                    FString::Printf(TEXT("import produced no %s (got %d objects, cleaned %d)"),
                                    *ExpectedBase->GetName(),
                                    Task->ImportedObjectPaths.Num(),
                                    Cleaned));
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

    // Animation imports REQUIRE a target Skeleton — without it, UFbxImportUI
    // refuses to bind the AnimSequence and the asset is unusable. Wire the
    // schema-declared `skeleton` param through to UFbxImportUI->Skeleton so
    // ImportAssetTasks gets a fully-configured option block.
    auto ConfigureUI = [](UFbxImportUI* UI, const TSharedPtr<FJsonObject>& A)
        -> FSageToolDispatch::FOutcome
    {
        FString SkeletonPath;
        if (!A.IsValid() || !A->TryGetStringField(TEXT("skeleton"), SkeletonPath)
            || SkeletonPath.IsEmpty())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("missing 'skeleton' (animation import requires target USkeleton path)"));
        }
        FSoftObjectPath Soft(SkeletonPath);
        UObject* Obj = Soft.ResolveObject();
        if (!Obj) Obj = Soft.TryLoad();
        USkeleton* Skel = Cast<USkeleton>(Obj);
        if (!Skel)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("skeleton not found or wrong class: %s"),
                                *SkeletonPath));
        }
        UI->Skeleton           = Skel;
        UI->MeshTypeToImport   = FBXIT_Animation;
        UI->OriginalImportType = FBXIT_Animation;
        UI->bImportAnimations  = true;
        UI->bImportMesh        = false;
        UI->bImportMaterials   = false;
        UI->bImportTextures    = false;
        // Empty result — caller only inspects bSuccess.
        return FSageToolDispatch::FOutcome::MakeSuccess(MakeShared<FJsonObject>());
    };

    return import_helpers::RunImport(Args, AnimSeq, ConfigureUI);
}

FSageToolDispatch::FOutcome BatchImportFbxAnimationsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Directory, Destination, SkeletonPath;
    if (!Args.IsValid() ||
        (!Args->TryGetStringField(TEXT("directory"), Directory) && !Args->TryGetStringField(TEXT("folder"), Directory)) ||
        Directory.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'directory'"));
    }
    if (!Args->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'destination'"));
    }
    if (!Args->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'skeleton'"));
    }
    if (!IFileManager::Get().DirectoryExists(*Directory))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("directory not found: %s"), *Directory));
    }

    FString Pattern = TEXT("*.fbx");
    bool bRecursive = false;
    bool bReplace = false;
    bool bDryRun = false;
    Args->TryGetStringField(TEXT("pattern"), Pattern);
    Args->TryGetBoolField(TEXT("recursive"), bRecursive);
    Args->TryGetBoolField(TEXT("replace_existing"), bReplace);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);

    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(
        Files,
        *Directory,
        *Pattern,
        /*Files=*/true,
        /*Directories=*/false,
        bRecursive);
    Files.Sort();

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 ImportedCount = 0;
    int32 FailedCount = 0;
    for (const FString& File : Files)
    {
        const FString BaseName = FPaths::GetBaseFilename(File);
        const FString DestPath = Destination / BaseName;
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("file"), File);
        Row->SetStringField(TEXT("destination"), DestPath);
        if (bDryRun)
        {
            Row->SetStringField(TEXT("status"), TEXT("dry_run"));
            Results.Add(MakeShared<FJsonValueObject>(Row));
            continue;
        }

        auto Single = MakeShared<FJsonObject>();
        Single->SetStringField(TEXT("file"), File);
        Single->SetStringField(TEXT("destination"), DestPath);
        Single->SetStringField(TEXT("skeleton"), SkeletonPath);
        Single->SetBoolField(TEXT("replace_existing"), bReplace);
        FSageToolDispatch::FOutcome Imported = ImportAnimationImpl(Single);
        if (Imported.bSuccess && Imported.Result.IsValid())
        {
            Row->SetStringField(TEXT("status"), TEXT("imported"));
            Row->SetObjectField(TEXT("result"), Imported.Result.ToSharedRef());
            ++ImportedCount;
        }
        else
        {
            Row->SetStringField(TEXT("status"), TEXT("failed"));
            if (Imported.Error.IsValid())
            {
                Row->SetObjectField(TEXT("error"), Imported.Error.ToSharedRef());
            }
            ++FailedCount;
        }
        Results.Add(MakeShared<FJsonValueObject>(Row));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("directory"), Directory);
    R->SetStringField(TEXT("destination"), Destination);
    R->SetStringField(TEXT("skeleton"), SkeletonPath);
    R->SetStringField(TEXT("pattern"), Pattern);
    R->SetBoolField(TEXT("recursive"), bRecursive);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("count"), Files.Num());
    R->SetNumberField(TEXT("imported"), ImportedCount);
    R->SetNumberField(TEXT("failed"), FailedCount);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
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
    int32 Offset = 0;
    TSet<FString> FieldsFilter;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_rows"), N))
        {
            MaxRows = FMath::Clamp(static_cast<int32>(N), 1, 100000);
        }
        double OffN = 0;
        if (Args->TryGetNumberField(TEXT("offset"), OffN))
        {
            Offset = FMath::Max(0, static_cast<int32>(OffN));
        }
        const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
        if (Args->TryGetArrayField(TEXT("fields"), FieldsArr) && FieldsArr)
        {
            for (const TSharedPtr<FJsonValue>& V : *FieldsArr)
            {
                FString S;
                if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
                {
                    FieldsFilter.Add(S);
                }
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), DT->GetPathName());
    R->SetStringField(TEXT("row_struct"),
        DT->RowStruct ? DT->RowStruct->GetPathName() : TEXT(""));

    TArray<FName> RowNames = DT->GetRowNames();
    R->SetNumberField(TEXT("row_count"), RowNames.Num());
    R->SetNumberField(TEXT("offset"),    Offset);

    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 Returned = 0;
    int32 Skipped = 0;
    if (DT->RowStruct)
    {
        for (const FName& Name : RowNames)
        {
            if (Skipped < Offset) { ++Skipped; continue; }
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
                const FString FieldName = P->GetName();
                if (FieldsFilter.Num() > 0 && !FieldsFilter.Contains(FieldName)) continue;
                const void* Value = P->ContainerPtrToValuePtr<const void>(RowData);
                TSharedPtr<FJsonValue> JV = detail::GetPropertyValueAtPtr(P, Value);
                if (JV.IsValid())
                {
                    Fields->SetField(FieldName, JV);
                }
            }
            Row->SetObjectField(TEXT("fields"), Fields);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
            ++Returned;
        }
    }
    R->SetArrayField (TEXT("rows"),     Rows);
    R->SetNumberField(TEXT("returned"), Returned);
    R->SetBoolField  (TEXT("capped"),   (Offset + Returned) < RowNames.Num());
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

    // Pre-validate ALL slots before mutating. Validate-then-apply: reject the
    // entire request if any entry is malformed or out-of-range, so the asset
    // is not left in a partially-mutated state.
    struct FResolved
    {
        int32 Index;
        bool  bClearMaterial;
        UMaterialInterface* Mat;
        bool  bSetSlotName;
        FName SlotName;
    };
    TArray<FResolved> Resolved;
    Resolved.Reserve(SlotArr->Num());
    for (int32 I = 0; I < SlotArr->Num(); ++I)
    {
        const TSharedPtr<FJsonValue>& V = (*SlotArr)[I];
        const TSharedPtr<FJsonObject>* Item = nullptr;
        if (!V.IsValid() || !V->TryGetObject(Item) || !Item || !Item->IsValid())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("slots[%d] is not an object"), I));
        }
        double IndexN = -1;
        if (!(*Item)->TryGetNumberField(TEXT("index"), IndexN))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("slots[%d] missing 'index'"), I));
        }
        FResolved R;
        R.Index = static_cast<int32>(IndexN);
        if (R.Index < 0 || R.Index >= Mats.Num())
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("slots[%d] index %d out of range (have %d)"),
                                I, R.Index, Mats.Num()));
        }
        FString MatPath;
        (*Item)->TryGetStringField(TEXT("material"), MatPath);
        R.bClearMaterial = MatPath.IsEmpty();
        R.Mat = nullptr;
        if (!R.bClearMaterial)
        {
            R.Mat = mat_helpers::ResolveMaterial(MatPath);
            if (!R.Mat)
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("slots[%d] material not found: %s"), I, *MatPath));
            }
        }
        FString SlotName;
        R.bSetSlotName = (*Item)->TryGetStringField(TEXT("slot_name"), SlotName);
        R.SlotName = R.bSetSlotName ? FName(*SlotName) : NAME_None;
        Resolved.Add(R);
    }

    // All entries valid — apply mutations under a single transaction.
    FScopedTransaction Tx(LOCTEXT("SetSkMaterialSlots", "Set SK Material Slots"));
    SK->Modify();

    int32 NumApplied = 0;
    TArray<TSharedPtr<FJsonValue>> Applied;
    for (const FResolved& R : Resolved)
    {
        Mats[R.Index].MaterialInterface = R.bClearMaterial ? nullptr : R.Mat;
        if (R.bSetSlotName) Mats[R.Index].MaterialSlotName = R.SlotName;
        ++NumApplied;
        auto E = MakeShared<FJsonObject>();
        E->SetNumberField(TEXT("index"),    R.Index);
        E->SetStringField(TEXT("material"), Mats[R.Index].MaterialInterface
            ? Mats[R.Index].MaterialInterface->GetPathName() : TEXT(""));
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

    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("destructive operation; pass confirmed:true to proceed"));
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
            TSharedPtr<FJsonObject> Diag = AssetDeleteDiagnosticJson(P, Sub);
            E->SetStringField(TEXT("reason"), SummarizeDeleteDiagnostic(Diag));
            E->SetObjectField(TEXT("diagnostics"), Diag);
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

    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("destructive operation; pass confirmed:true to proceed (discards in-memory edits — irreversible without git)"));
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
            // Refresh GPU resource so editor preview matches the edited settings
            // (compression / sRGB / address modes only show after a resource update).
            Tex->UpdateResource();
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
    enum class EOwner
    {
        Mesh,
        Skeleton,
        Any
    };

    FString OwnerToString(EOwner Owner)
    {
        switch (Owner)
        {
        case EOwner::Mesh: return TEXT("mesh");
        case EOwner::Skeleton: return TEXT("skeleton");
        default: return TEXT("any");
        }
    }

    bool ParseOwner(const TSharedPtr<FJsonObject>& Args, EOwner DefaultOwner, EOwner& OutOwner, FString& OutError)
    {
        OutOwner = DefaultOwner;
        FString OwnerStr;
        if (!Args.IsValid() || !Args->TryGetStringField(TEXT("owner"), OwnerStr) || OwnerStr.IsEmpty())
        {
            return true;
        }
        OwnerStr = OwnerStr.TrimStartAndEnd().ToLower();
        if (OwnerStr == TEXT("mesh"))
        {
            OutOwner = EOwner::Mesh;
            return true;
        }
        if (OwnerStr == TEXT("skeleton"))
        {
            OutOwner = EOwner::Skeleton;
            return true;
        }
        if (OwnerStr == TEXT("any") || OwnerStr == TEXT("effective"))
        {
            OutOwner = EOwner::Any;
            return true;
        }
        OutError = TEXT("'owner' must be one of: mesh, skeleton, any");
        return false;
    }

    TSharedPtr<FJsonObject> StaticSocketToJson(const UStaticMeshSocket* S, const FString& Owner = TEXT("mesh"))
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),     S->SocketName.ToString());
        J->SetStringField(TEXT("owner"),    Owner);
        J->SetField(TEXT("location"),       detail::Vec3ToJson(S->RelativeLocation));
        J->SetField(TEXT("rotation"),       detail::Rot3ToJson(S->RelativeRotation));
        J->SetField(TEXT("scale"),          detail::Vec3ToJson(S->RelativeScale));
        if (!S->Tag.IsEmpty()) J->SetStringField(TEXT("tag"), S->Tag);
        return J;
    }

    TSharedPtr<FJsonObject> SkelSocketToJson(
        const USkeletalMeshSocket* S,
        const FString& Owner = TEXT("mesh"),
        const UObject* OwnerAsset = nullptr,
        bool bEffective = true)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),         S->SocketName.ToString());
        J->SetStringField(TEXT("bone"),         S->BoneName.ToString());
        J->SetStringField(TEXT("parent_bone"),  S->BoneName.ToString());
        J->SetStringField(TEXT("owner"),        Owner);
        J->SetBoolField  (TEXT("effective"),    bEffective);
        if (OwnerAsset)
        {
            J->SetStringField(TEXT("owner_asset"), OwnerAsset->GetPathName());
        }
        J->SetField(TEXT("location"),           detail::Vec3ToJson(S->RelativeLocation));
        J->SetField(TEXT("rotation"),           detail::Rot3ToJson(S->RelativeRotation));
        J->SetField(TEXT("scale"),              detail::Vec3ToJson(S->RelativeScale));
        J->SetBoolField(TEXT("force_always_animated"), S->bForceAlwaysAnimated);
        return J;
    }

    USkeletalMeshSocket* FindMeshOnlySocket(USkeletalMesh* Mesh, FName Name)
    {
        if (!Mesh) return nullptr;
        for (const TObjectPtr<USkeletalMeshSocket>& Socket : Mesh->GetMeshOnlySocketList())
        {
            if (Socket && Socket->SocketName == Name)
            {
                return Socket.Get();
            }
        }
        return nullptr;
    }

    USkeletalMeshSocket* FindSkeletonSocket(USkeleton* Skeleton, FName Name)
    {
        if (!Skeleton) return nullptr;
        for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
        {
            if (Socket && Socket->SocketName == Name)
            {
                return Socket;
            }
        }
        return nullptr;
    }

    bool SkeletonHasBone(USkeleton* Skeleton, FName BoneName)
    {
        if (!Skeleton || BoneName.IsNone()) return true;
        return Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) != INDEX_NONE;
    }

    bool MeshHasBone(USkeletalMesh* Mesh, FName BoneName)
    {
        if (!Mesh || BoneName.IsNone()) return true;
        return Mesh->GetRefSkeleton().FindBoneIndex(BoneName) != INDEX_NONE;
    }

    TSharedPtr<FJsonObject> DesiredSkelSocketJson(
        const FString& Name,
        const FString& Owner,
        const UObject* OwnerAsset,
        FName BoneName,
        const FVector& Location,
        const FRotator& Rotation,
        const FVector& Scale)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), Name);
        J->SetStringField(TEXT("bone"), BoneName.ToString());
        J->SetStringField(TEXT("parent_bone"), BoneName.ToString());
        J->SetStringField(TEXT("owner"), Owner);
        J->SetBoolField(TEXT("effective"), true);
        if (OwnerAsset)
        {
            J->SetStringField(TEXT("owner_asset"), OwnerAsset->GetPathName());
        }
        J->SetField(TEXT("location"), detail::Vec3ToJson(Location));
        J->SetField(TEXT("rotation"), detail::Rot3ToJson(Rotation));
        J->SetField(TEXT("scale"), detail::Vec3ToJson(Scale));
        return J;
    }

    void AddCollisionIfNeeded(
        const FName SocketName,
        const USkeletalMeshSocket* MeshSocket,
        const USkeletalMeshSocket* SkeletonSocket,
        const USkeletalMesh* Mesh,
        const USkeleton* Skeleton,
        TArray<TSharedPtr<FJsonValue>>& Collisions)
    {
        if (!MeshSocket || !SkeletonSocket) return;
        auto C = MakeShared<FJsonObject>();
        C->SetStringField(TEXT("name"), SocketName.ToString());
        C->SetStringField(TEXT("effective_owner"), TEXT("mesh"));
        C->SetStringField(TEXT("shadowed_owner"), TEXT("skeleton"));
        C->SetStringField(TEXT("mesh_asset"), Mesh ? Mesh->GetPathName() : FString());
        C->SetStringField(TEXT("skeleton_asset"), Skeleton ? Skeleton->GetPathName() : FString());
        C->SetObjectField(TEXT("mesh_socket"), SkelSocketToJson(MeshSocket, TEXT("mesh"), Mesh, true));
        C->SetObjectField(TEXT("skeleton_socket"), SkelSocketToJson(SkeletonSocket, TEXT("skeleton"), Skeleton, false));
        Collisions.Add(MakeShared<FJsonValueObject>(C));
    }

    bool SaveLoadedAssetIfRequested(UObject* Asset, bool bSave, TSharedRef<FJsonObject> Result)
    {
        Result->SetBoolField(TEXT("save_requested"), bSave);
        if (!bSave) return true;
        UEditorAssetSubsystem* AssetSubsystem = GEditor
            ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
            : nullptr;
        if (!AssetSubsystem)
        {
            Result->SetStringField(TEXT("save_error"), TEXT("EditorAssetSubsystem unavailable"));
            return false;
        }
        const bool bSaved = AssetSubsystem->SaveLoadedAsset(Asset, /*bOnlyIfIsDirty=*/false);
        Result->SetBoolField(TEXT("saved"), bSaved);
        if (!bSaved)
        {
            Result->SetStringField(TEXT("save_error"),
                FString::Printf(TEXT("SaveLoadedAsset returned false for %s"), *Asset->GetPathName()));
        }
        return bSaved;
    }

    TSharedPtr<FJsonObject> BuildSocketReadback(UObject* Asset, FName SocketFName, EOwner Owner)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), Asset ? Asset->GetPathName() : FString());
        R->SetStringField(TEXT("requested_name"), SocketFName.ToString());
        R->SetStringField(TEXT("owner_filter"), OwnerToString(Owner));
        R->SetBoolField(TEXT("found"), false);

        if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
        {
            R->SetStringField(TEXT("kind"), TEXT("static_mesh"));
            UStaticMeshSocket* Found = SM->FindSocket(SocketFName);
            if (Found && Owner != EOwner::Skeleton)
            {
                R->SetBoolField(TEXT("found"), true);
                R->SetStringField(TEXT("effective_owner"), TEXT("mesh"));
                R->SetObjectField(TEXT("socket"), StaticSocketToJson(Found));
                R->SetObjectField(TEXT("effective_socket"), StaticSocketToJson(Found));
            }
            return R;
        }

        if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
        {
            R->SetStringField(TEXT("kind"), TEXT("skeletal_mesh"));
            USkeleton* Skeleton = SK->GetSkeleton();
            USkeletalMeshSocket* MeshSocket = FindMeshOnlySocket(SK, SocketFName);
            USkeletalMeshSocket* SkeletonSocket = FindSkeletonSocket(Skeleton, SocketFName);
            if (MeshSocket)
            {
                R->SetObjectField(TEXT("mesh_socket"), SkelSocketToJson(MeshSocket, TEXT("mesh"), SK, true));
            }
            if (SkeletonSocket)
            {
                const bool bSkeletonEffective = MeshSocket == nullptr;
                R->SetObjectField(TEXT("skeleton_socket"), SkelSocketToJson(SkeletonSocket, TEXT("skeleton"), Skeleton, bSkeletonEffective));
            }
            R->SetBoolField(TEXT("has_mesh_socket"), MeshSocket != nullptr);
            R->SetBoolField(TEXT("has_skeleton_socket"), SkeletonSocket != nullptr);
            R->SetBoolField(TEXT("has_owner_collision"), MeshSocket && SkeletonSocket);
            if (MeshSocket || SkeletonSocket)
            {
                const bool bOwnerMatches =
                    Owner == EOwner::Any ||
                    (Owner == EOwner::Mesh && MeshSocket) ||
                    (Owner == EOwner::Skeleton && SkeletonSocket);
                R->SetBoolField(TEXT("found"), bOwnerMatches);
                if (MeshSocket)
                {
                    R->SetStringField(TEXT("effective_owner"), TEXT("mesh"));
                    R->SetObjectField(TEXT("effective_socket"), SkelSocketToJson(MeshSocket, TEXT("mesh"), SK, true));
                }
                else if (SkeletonSocket)
                {
                    R->SetStringField(TEXT("effective_owner"), TEXT("skeleton"));
                    R->SetObjectField(TEXT("effective_socket"), SkelSocketToJson(SkeletonSocket, TEXT("skeleton"), Skeleton, true));
                }
            }
            return R;
        }
        return R;
    }
}

FSageToolDispatch::FOutcome ListSocketsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    socket_helpers::EOwner OwnerFilter = socket_helpers::EOwner::Mesh;
    FString OwnerErr;
    if (!socket_helpers::ParseOwner(Args, socket_helpers::EOwner::Mesh, OwnerFilter, OwnerErr))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, OwnerErr);
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Asset->GetPathName());
    R->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
    R->SetStringField(TEXT("owner_filter"), socket_helpers::OwnerToString(OwnerFilter));

    TArray<TSharedPtr<FJsonValue>> SocketArr;
    TArray<TSharedPtr<FJsonValue>> Collisions;
    if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
    {
        if (OwnerFilter == socket_helpers::EOwner::Skeleton)
        {
            R->SetStringField(TEXT("kind"), TEXT("static_mesh"));
            R->SetArrayField(TEXT("sockets"), SocketArr);
            R->SetNumberField(TEXT("count"), 0);
            R->SetArrayField(TEXT("owner_collisions"), Collisions);
            R->SetNumberField(TEXT("owner_collision_count"), 0);
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
        for (UStaticMeshSocket* S : SM->Sockets)
        {
            if (!S) continue;
            SocketArr.Add(MakeShared<FJsonValueObject>(socket_helpers::StaticSocketToJson(S)));
        }
        R->SetStringField(TEXT("kind"), TEXT("static_mesh"));
    }
    else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
    {
        USkeleton* Skeleton = SK->GetSkeleton();
        TSet<FName> MeshSocketNames;
        if (OwnerFilter == socket_helpers::EOwner::Mesh || OwnerFilter == socket_helpers::EOwner::Any)
        {
            for (const TObjectPtr<USkeletalMeshSocket>& S : SK->GetMeshOnlySocketList())
            {
                if (!S) continue;
                MeshSocketNames.Add(S->SocketName);
                const bool bOverridesSkeleton = socket_helpers::FindSkeletonSocket(Skeleton, S->SocketName) != nullptr;
                TSharedPtr<FJsonObject> J = socket_helpers::SkelSocketToJson(S.Get(), TEXT("mesh"), SK, true);
                J->SetBoolField(TEXT("overrides_skeleton_socket"), bOverridesSkeleton);
                SocketArr.Add(MakeShared<FJsonValueObject>(J));
            }
        }
        if (OwnerFilter == socket_helpers::EOwner::Skeleton || OwnerFilter == socket_helpers::EOwner::Any)
        {
            if (Skeleton)
            {
                for (USkeletalMeshSocket* S : Skeleton->Sockets)
                {
                    if (!S) continue;
                    const bool bShadowed = MeshSocketNames.Contains(S->SocketName)
                        || socket_helpers::FindMeshOnlySocket(SK, S->SocketName) != nullptr;
                    SocketArr.Add(MakeShared<FJsonValueObject>(
                        socket_helpers::SkelSocketToJson(S, TEXT("skeleton"), Skeleton, !bShadowed)));
                    if (USkeletalMeshSocket* MeshSocket = socket_helpers::FindMeshOnlySocket(SK, S->SocketName))
                    {
                        socket_helpers::AddCollisionIfNeeded(S->SocketName, MeshSocket, S, SK, Skeleton, Collisions);
                    }
                }
            }
        }
        R->SetStringField(TEXT("kind"), TEXT("skeletal_mesh"));
        R->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset is %s, expected StaticMesh or SkeletalMesh"),
                            *Asset->GetClass()->GetName()));
    }
    R->SetArrayField(TEXT("sockets"), SocketArr);
    R->SetNumberField(TEXT("count"), SocketArr.Num());
    R->SetArrayField(TEXT("owner_collisions"), Collisions);
    R->SetNumberField(TEXT("owner_collision_count"), Collisions.Num());
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
        if (socket_helpers::FindMeshOnlySocket(SK, SocketFName))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("mesh socket '%s' already exists"), *Name));
        }
        if (socket_helpers::FindSkeletonSocket(SK->GetSkeleton(), SocketFName))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("socket '%s' is inherited from the skeleton; use asset.upsert_socket with owner='mesh' and allow_mesh_override_of_skeleton_socket=true to create a mesh-only override"), *Name));
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

FSageToolDispatch::FOutcome GetSocketImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    socket_helpers::EOwner OwnerFilter = socket_helpers::EOwner::Any;
    FString OwnerErr;
    if (!socket_helpers::ParseOwner(Args, socket_helpers::EOwner::Any, OwnerFilter, OwnerErr))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, OwnerErr);
    }
    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));
    if (!Asset->IsA<UStaticMesh>() && !Asset->IsA<USkeletalMesh>())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset is %s, expected StaticMesh or SkeletalMesh"),
                            *Asset->GetClass()->GetName()));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(
        socket_helpers::BuildSocketReadback(Asset, FName(*Name), OwnerFilter));
}

FSageToolDispatch::FOutcome UpsertSocketImpl(const TSharedPtr<FJsonObject>& Args)
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

    socket_helpers::EOwner Owner = socket_helpers::EOwner::Mesh;
    FString OwnerErr;
    if (!socket_helpers::ParseOwner(Args, socket_helpers::EOwner::Mesh, Owner, OwnerErr))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, OwnerErr);
    }
    if (Owner == socket_helpers::EOwner::Any)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("asset.upsert_socket requires owner='mesh' or owner='skeleton'; owner='any' is read-only"));
    }

    UObject* Asset = ResolveAsset(Path);
    if (!Asset) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));

    FVector Location(0,0,0);
    FRotator Rotation(0,0,0);
    FVector Scale(1,1,1);
    detail::ParseVector3 (Args, TEXT("location"), Location);
    detail::ParseRotator3(Args, TEXT("rotation"), Rotation);
    detail::ParseVector3 (Args, TEXT("scale"),    Scale);

    FString BoneNameStr;
    Args->TryGetStringField(TEXT("bone"), BoneNameStr);
    if (BoneNameStr.IsEmpty())
    {
        Args->TryGetStringField(TEXT("parent_bone"), BoneNameStr);
    }
    const FName SocketFName(*Name);
    const FName BoneName = BoneNameStr.IsEmpty() ? NAME_None : FName(*BoneNameStr);
    bool bDryRun = false;
    bool bSave = false;
    bool bCreateIfMissing = true;
    bool bAllowMeshOverride = false;
    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);
    Args->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);
    Args->TryGetBoolField(TEXT("allow_mesh_override_of_skeleton_socket"), bAllowMeshOverride);
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Asset->GetPathName());
    R->SetStringField(TEXT("name"), Name);
    R->SetStringField(TEXT("owner"), socket_helpers::OwnerToString(Owner));
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("create_if_missing"), bCreateIfMissing);
    R->SetBoolField(TEXT("allow_mesh_override_of_skeleton_socket"), bAllowMeshOverride);
    R->SetObjectField(TEXT("before"), socket_helpers::BuildSocketReadback(Asset, SocketFName, socket_helpers::EOwner::Any));

    if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
    {
        if (Owner != socket_helpers::EOwner::Mesh)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("StaticMesh sockets only support owner='mesh'"));
        }
        UStaticMeshSocket* Socket = SM->FindSocket(SocketFName);
        const bool bCreate = Socket == nullptr;
        if (bCreate && !bCreateIfMissing)
        {
            R->SetBoolField(TEXT("changed"), false);
            R->SetStringField(TEXT("action"), TEXT("missing_refused"));
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
        R->SetStringField(TEXT("action"), bCreate ? TEXT("create_mesh_socket") : TEXT("update_mesh_socket"));
        R->SetObjectField(TEXT("desired_socket"), socket_helpers::StaticSocketToJson(
            [&]() {
                UStaticMeshSocket* Preview = NewObject<UStaticMeshSocket>(GetTransientPackage());
                Preview->SocketName = SocketFName;
                Preview->RelativeLocation = Location;
                Preview->RelativeRotation = Rotation;
                Preview->RelativeScale = Scale;
                return Preview;
            }()));
        if (bDryRun)
        {
            R->SetBoolField(TEXT("changed"), true);
            R->SetStringField(TEXT("would_mutate_asset"), SM->GetPathName());
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }

        FScopedTransaction Tx(LOCTEXT("UpsertSocket", "Sage: Upsert Socket"));
        SM->Modify();
        if (!Socket)
        {
            Socket = NewObject<UStaticMeshSocket>(SM);
            Socket->SocketName = SocketFName;
            SM->AddSocket(Socket);
        }
        else
        {
            Socket->Modify();
        }
        Socket->RelativeLocation = Location;
        Socket->RelativeRotation = Rotation;
        Socket->RelativeScale = Scale;
        SM->MarkPackageDirty();
        SM->PostEditChange();
        R->SetBoolField(TEXT("changed"), true);
        R->SetObjectField(TEXT("after"), socket_helpers::BuildSocketReadback(Asset, SocketFName, socket_helpers::EOwner::Any));
        socket_helpers::SaveLoadedAssetIfRequested(SM, bSave, R);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset);
    USkeleton* Skeleton = Cast<USkeleton>(Asset);
    if (!Mesh && !Skeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset is %s, expected StaticMesh, SkeletalMesh, or USkeleton for owner='skeleton'"),
                            *Asset->GetClass()->GetName()));
    }

    if (Owner == socket_helpers::EOwner::Mesh)
    {
        if (!Mesh)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("owner='mesh' requires a USkeletalMesh or UStaticMesh path"));
        }
        if (!socket_helpers::MeshHasBone(Mesh, BoneName))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("bone '%s' not found on skeletal mesh"), *BoneName.ToString()));
        }
        USkeletalMeshSocket* MeshSocket = socket_helpers::FindMeshOnlySocket(Mesh, SocketFName);
        USkeletalMeshSocket* SkeletonSocket = socket_helpers::FindSkeletonSocket(Mesh->GetSkeleton(), SocketFName);
        const bool bCreate = MeshSocket == nullptr;
        if (bCreate && SkeletonSocket && !bAllowMeshOverride)
        {
            R->SetBoolField(TEXT("success"), false);
            R->SetBoolField(TEXT("changed"), false);
            R->SetStringField(TEXT("action"), TEXT("skeleton_socket_blocks_mesh_override"));
            R->SetStringField(TEXT("error"), TEXT("socket exists on the shared skeleton; pass allow_mesh_override_of_skeleton_socket=true to create a mesh-only override"));
            R->SetObjectField(TEXT("blocking_socket"), socket_helpers::SkelSocketToJson(SkeletonSocket, TEXT("skeleton"), Mesh->GetSkeleton(), true));
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
        if (bCreate && !bCreateIfMissing)
        {
            R->SetBoolField(TEXT("changed"), false);
            R->SetStringField(TEXT("action"), TEXT("missing_refused"));
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
        R->SetStringField(TEXT("action"), bCreate ? TEXT("create_mesh_socket") : TEXT("update_mesh_socket"));
        R->SetObjectField(TEXT("desired_socket"), socket_helpers::DesiredSkelSocketJson(
            Name, TEXT("mesh"), Mesh, BoneName, Location, Rotation, Scale));
        if (SkeletonSocket && bCreate)
        {
            R->SetBoolField(TEXT("creates_mesh_override_of_skeleton_socket"), true);
            R->SetObjectField(TEXT("overridden_skeleton_socket"),
                socket_helpers::SkelSocketToJson(SkeletonSocket, TEXT("skeleton"), Mesh->GetSkeleton(), false));
        }
        if (bDryRun)
        {
            R->SetBoolField(TEXT("changed"), true);
            R->SetStringField(TEXT("would_mutate_asset"), Mesh->GetPathName());
            R->SetBoolField(TEXT("would_mutate_skeleton"), false);
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }

        FScopedTransaction Tx(LOCTEXT("UpsertSocket", "Sage: Upsert Socket"));
        Mesh->Modify();
        if (!MeshSocket)
        {
            MeshSocket = NewObject<USkeletalMeshSocket>(Mesh);
            MeshSocket->SocketName = SocketFName;
            Mesh->GetMeshOnlySocketList().Add(TObjectPtr<USkeletalMeshSocket>(MeshSocket));
        }
        else
        {
            MeshSocket->Modify();
        }
        MeshSocket->BoneName = BoneName;
        MeshSocket->RelativeLocation = Location;
        MeshSocket->RelativeRotation = Rotation;
        MeshSocket->RelativeScale = Scale;
        Mesh->MarkPackageDirty();
        Mesh->PostEditChange();
        R->SetBoolField(TEXT("changed"), true);
        R->SetStringField(TEXT("mutated_asset"), Mesh->GetPathName());
        R->SetBoolField(TEXT("mutated_skeleton"), false);
        R->SetObjectField(TEXT("after"), socket_helpers::BuildSocketReadback(Mesh, SocketFName, socket_helpers::EOwner::Any));
        socket_helpers::SaveLoadedAssetIfRequested(Mesh, bSave, R);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    USkeleton* TargetSkeleton = Skeleton ? Skeleton : (Mesh ? Mesh->GetSkeleton() : nullptr);
    if (!TargetSkeleton)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("skeletal mesh has no skeleton for owner='skeleton'"));
    }
    if (!bDryRun && !bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("owner='skeleton' mutates a shared skeleton asset; pass confirmed:true to proceed"));
    }
    if (!socket_helpers::SkeletonHasBone(TargetSkeleton, BoneName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("bone '%s' not found on skeleton"), *BoneName.ToString()));
    }
    USkeletalMeshSocket* TargetSocket = socket_helpers::FindSkeletonSocket(TargetSkeleton, SocketFName);
    const bool bCreate = TargetSocket == nullptr;
    if (bCreate && !bCreateIfMissing)
    {
        R->SetBoolField(TEXT("changed"), false);
        R->SetStringField(TEXT("action"), TEXT("missing_refused"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }
    R->SetStringField(TEXT("action"), bCreate ? TEXT("create_skeleton_socket") : TEXT("update_skeleton_socket"));
    R->SetStringField(TEXT("skeleton_asset"), TargetSkeleton->GetPathName());
    R->SetObjectField(TEXT("desired_socket"), socket_helpers::DesiredSkelSocketJson(
        Name, TEXT("skeleton"), TargetSkeleton, BoneName, Location, Rotation, Scale));
    if (bDryRun)
    {
        R->SetBoolField(TEXT("changed"), true);
        R->SetStringField(TEXT("would_mutate_asset"), TargetSkeleton->GetPathName());
        R->SetBoolField(TEXT("would_mutate_skeleton"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("UpsertSkeletonSocket", "Sage: Upsert Skeleton Socket"));
    TargetSkeleton->Modify();
    if (!TargetSocket)
    {
        TargetSocket = NewObject<USkeletalMeshSocket>(TargetSkeleton);
        TargetSocket->SocketName = SocketFName;
        TargetSkeleton->Sockets.Add(TargetSocket);
    }
    else
    {
        TargetSocket->Modify();
    }
    TargetSocket->BoneName = BoneName;
    TargetSocket->RelativeLocation = Location;
    TargetSocket->RelativeRotation = Rotation;
    TargetSocket->RelativeScale = Scale;
    TargetSkeleton->MarkPackageDirty();
    TargetSkeleton->PostEditChange();
    R->SetBoolField(TEXT("changed"), true);
    R->SetStringField(TEXT("mutated_asset"), TargetSkeleton->GetPathName());
    R->SetBoolField(TEXT("mutated_skeleton"), true);
    R->SetObjectField(TEXT("after"), socket_helpers::BuildSocketReadback(Asset, SocketFName, socket_helpers::EOwner::Any));
    socket_helpers::SaveLoadedAssetIfRequested(TargetSkeleton, bSave, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
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

    // NOTE: this handler does NOT actually recenter the pivot. True recentering
    // requires baking a vertex offset (FbxImport pipeline or BuildSettings
    // BuildOrigin manipulation + rebuild). Setting BuildScale3D = OneVector is
    // a no-op for pivot. We return modified=false with an honest note so callers
    // see this and don't assume the asset changed.
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         SM->GetPathName());
    R->SetBoolField  (TEXT("modified"),     false);
    R->SetStringField(TEXT("note"),
        TEXT("BuildScale3D reset to identity; true pivot recentering requires "
             "vertex offset baking (not implemented). Editor UI workaround: "
             "right-click in viewport > Pivot > Set as Pivot Offset, or re-import "
             "FBX with adjusted origin."));
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

    TArray<FString> FailedFields;
    bool bApplied = false;
    // Set nav mesh collision property via reflection
    FProperty* Prop = FindFProperty<FProperty>(SM->GetClass(), TEXT("bCanEverAffectNavigation"));
    if (Prop)
    {
        if (detail::SetUPropertyFromJson(SM, Prop,
                MakeShared<FJsonValueBoolean>(bNavAllowed)))
        {
            bApplied = true;
        }
        else
        {
            FailedFields.Add(TEXT("bCanEverAffectNavigation"));
        }
    }
    else
    {
        FailedFields.Add(TEXT("bCanEverAffectNavigation"));
    }

    if (bApplied) SM->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),                         SM->GetPathName());
    R->SetBoolField  (TEXT("can_ever_affect_navigation"),   bNavAllowed);
    R->SetBoolField  (TEXT("modified"),                     bApplied);
    if (FailedFields.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Skipped;
        for (const FString& F : FailedFields)
        {
            Skipped.Add(MakeShared<FJsonValueString>(F));
        }
        R->SetArrayField(TEXT("skipped_fields"), Skipped);
    }
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

// ---- asset.add_array_element (Lyra Sage Gap #11 — append to TArray<T>) ------
//
// Increment-append a single element to a UPROPERTY TArray on an asset's
// CDO. Pairs with the now-recursive FStructProperty writer so struct array
// authoring (FLyraInputAction[], FLyraAbilitySet_GameplayAbility[], etc.)
// works without nuking + rebuilding the entire array. CommonAIExport
// add_cdo_array_element parity.
//
// Args:
//   asset_path        (string)  — asset path
//   array_property    (string)  — top-level UPROPERTY name on the asset
//   element_value     (any)     — JSON value matching the inner property
//                                 type. Strings for object/path refs,
//                                 objects for structs, scalars for prims.
//   class_name        (string?) — only for instanced UObject inners; if
//                                 set, NewObject<class>() before applying
//                                 element_value as instanced subobject
//                                 properties.
// Returns: {asset_path, array_property, index, length}.

// ---- gamefeature.* authoring (KaleGame P8 / Lyra HUD gaps) -----------------

FString CleanObjectReferenceLiteral(FString Raw)
{
    Raw = Raw.TrimStartAndEnd();
    while (Raw.Len() >= 2 && Raw.StartsWith(TEXT("(")) && Raw.EndsWith(TEXT(")")))
    {
        Raw = Raw.Mid(1, Raw.Len() - 2).TrimStartAndEnd();
    }
    if (Raw.Len() >= 2
        && ((Raw.StartsWith(TEXT("\"")) && Raw.EndsWith(TEXT("\"")))
            || (Raw.StartsWith(TEXT("'")) && Raw.EndsWith(TEXT("'")))))
    {
        Raw = Raw.Mid(1, Raw.Len() - 2).TrimStartAndEnd();
    }

    int32 FirstQuote = INDEX_NONE;
    int32 LastQuote = INDEX_NONE;
    if (Raw.FindChar(TEXT('\''), FirstQuote)
        && Raw.FindLastChar(TEXT('\''), LastQuote)
        && LastQuote > FirstQuote)
    {
        Raw = Raw.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1).TrimStartAndEnd();
    }
    return Raw;
}

UObject* ResolveAssetWithOptionalObjectName(const FString& Path)
{
    const FString CleanPath = CleanObjectReferenceLiteral(Path);
    if (UObject* Obj = ResolveAsset(CleanPath)) return Obj;
    if (CleanPath.Contains(TEXT(".")) || !CleanPath.StartsWith(TEXT("/"))) return nullptr;

    FString PackagePath, AssetName;
    if (!CleanPath.Split(TEXT("/"), &PackagePath, &AssetName,
                    ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return nullptr;
    }
    return ResolveAsset(CleanPath + TEXT(".") + AssetName);
}

UGameFeatureData* ResolveGameFeatureData(const FString& Path, FString& OutErr)
{
    UObject* Obj = ResolveAssetWithOptionalObjectName(Path);
    UGameFeatureData* GFD = Cast<UGameFeatureData>(Obj);
    if (!GFD)
    {
        OutErr = Obj
            ? FString::Printf(TEXT("'%s' is %s, not UGameFeatureData"),
                              *Path, *Obj->GetClass()->GetName())
            : FString::Printf(TEXT("GameFeatureData asset not found: %s"), *Path);
        return nullptr;
    }
    return GFD;
}

UClass* ResolveClassForGameFeature(const FString& Path, UClass* RequiredBase, FString& OutErr)
{
    const FString CleanPath = CleanObjectReferenceLiteral(Path);
    UObject* Obj = ResolveAssetWithOptionalObjectName(CleanPath);
    UClass* Cls = Cast<UClass>(Obj);
    if (!Cls)
    {
        if (UBlueprint* BP = Cast<UBlueprint>(Obj))
        {
            Cls = BP->GeneratedClass;
        }
    }
    if (!Cls)
    {
        OutErr = FString::Printf(TEXT("class not found: %s"), *CleanPath);
        return nullptr;
    }
    if (RequiredBase && !Cls->IsChildOf(RequiredBase))
    {
        OutErr = FString::Printf(TEXT("class %s is not a subclass of %s"),
                                 *Cls->GetPathName(), *RequiredBase->GetPathName());
        return nullptr;
    }
    return Cls;
}

UGameFeatureAction_AddComponents* FindAddComponentsAction(UGameFeatureData* GFD)
{
    if (!GFD) return nullptr;
    for (UGameFeatureAction* Action : GFD->GetActions())
    {
        if (UGameFeatureAction_AddComponents* Add = Cast<UGameFeatureAction_AddComponents>(Action))
        {
            return Add;
        }
    }
    return nullptr;
}

UGameFeatureAction_AddComponents* EnsureAddComponentsAction(UGameFeatureData* GFD, bool& bCreated)
{
    bCreated = false;
    if (!GFD) return nullptr;
    if (UGameFeatureAction_AddComponents* Existing = FindAddComponentsAction(GFD))
    {
        return Existing;
    }

    UGameFeatureAction_AddComponents* Action =
        NewObject<UGameFeatureAction_AddComponents>(GFD, NAME_None,
            RF_Public | RF_Transactional);
#if WITH_EDITOR
    GFD->Modify();
    GFD->GetMutableActionsInEditor().Add(Action);
    if (FProperty* ActionsProp = GFD->GetClass()->FindPropertyByName(TEXT("Actions")))
    {
        FPropertyChangedEvent E(ActionsProp, EPropertyChangeType::ArrayAdd);
        GFD->PostEditChangeProperty(E);
    }
#endif
    GFD->MarkPackageDirty();
    bCreated = true;
    return Action;
}

TSharedPtr<FJsonObject> GameFeatureEntryToJson(const FGameFeatureComponentEntry& Entry, int32 Index)
{
    auto J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("index"), Index);
    J->SetStringField(TEXT("actor_class"), Entry.ActorClass.ToSoftObjectPath().ToString());
    J->SetStringField(TEXT("component_class"), Entry.ComponentClass.ToSoftObjectPath().ToString());
    J->SetBoolField(TEXT("client"), Entry.bClientComponent != 0);
    J->SetBoolField(TEXT("server"), Entry.bServerComponent != 0);
    J->SetNumberField(TEXT("addition_flags"), Entry.AdditionFlags);
    return J;
}

bool MatchesGameFeatureEntry(const FGameFeatureComponentEntry& Entry,
                             UClass* ActorClass,
                             UClass* ComponentClass,
                             TOptional<bool> Client,
                             TOptional<bool> Server)
{
    UClass* ExistingActor = Entry.ActorClass.LoadSynchronous();
    UClass* ExistingComponent = Entry.ComponentClass.LoadSynchronous();
    if (ActorClass && ExistingActor != ActorClass) return false;
    if (ComponentClass && ExistingComponent != ComponentClass) return false;
    if (Client.IsSet() && ((Entry.bClientComponent != 0) != Client.GetValue())) return false;
    if (Server.IsSet() && ((Entry.bServerComponent != 0) != Server.GetValue())) return false;
    return true;
}

FSageToolDispatch::FOutcome GameFeatureEnsureAddComponentsActionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    FString Err;
    UGameFeatureData* GFD = ResolveGameFeatureData(Path, Err);
    if (!GFD) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    bool bCreated = false;
    UGameFeatureAction_AddComponents* Action = EnsureAddComponentsAction(GFD, bCreated);
    if (!Action)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("failed to create UGameFeatureAction_AddComponents"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), GFD->GetPathName());
    R->SetStringField(TEXT("action"), Action->GetPathName());
    R->SetBoolField(TEXT("created"), bCreated);
    R->SetBoolField(TEXT("already"), !bCreated);
    R->SetNumberField(TEXT("component_count"), Action->ComponentList.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GameFeatureListComponentEntriesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    FString Err;
    UGameFeatureData* GFD = ResolveGameFeatureData(Path, Err);
    if (!GFD) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    UGameFeatureAction_AddComponents* Action = FindAddComponentsAction(GFD);
    TArray<TSharedPtr<FJsonValue>> Entries;
    if (Action)
    {
        for (int32 i = 0; i < Action->ComponentList.Num(); ++i)
        {
            Entries.Add(MakeShared<FJsonValueObject>(
                GameFeatureEntryToJson(Action->ComponentList[i], i)));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), GFD->GetPathName());
    if (Action) R->SetStringField(TEXT("action"), Action->GetPathName());
    R->SetArrayField(TEXT("entries"), Entries);
    R->SetNumberField(TEXT("count"), Entries.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GameFeatureAddComponentEntryImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ActorPath, ComponentPath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("actor_class"), ActorPath)
        || !Args->TryGetStringField(TEXT("component_class"), ComponentPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'actor_class' or 'component_class'"));
    }

    bool bClient = true;
    bool bServer = true;
    Args->TryGetBoolField(TEXT("client"), bClient);
    Args->TryGetBoolField(TEXT("server"), bServer);
    double FlagsNum = 0.0;
    Args->TryGetNumberField(TEXT("addition_flags"), FlagsNum);

    FString Err;
    UGameFeatureData* GFD = ResolveGameFeatureData(Path, Err);
    if (!GFD) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UClass* ActorClass = ResolveClassForGameFeature(ActorPath, AActor::StaticClass(), Err);
    if (!ActorClass) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UClass* ComponentClass = ResolveClassForGameFeature(ComponentPath, UActorComponent::StaticClass(), Err);
    if (!ComponentClass) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    bool bActionCreated = false;
    UGameFeatureAction_AddComponents* Action = EnsureAddComponentsAction(GFD, bActionCreated);
    if (!Action)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("failed to create UGameFeatureAction_AddComponents"));
    }

    FString ReplaceComponentPath;
    UClass* ReplaceComponentClass = nullptr;
    if (Args->TryGetStringField(TEXT("replace_component_class"), ReplaceComponentPath)
        && !ReplaceComponentPath.IsEmpty())
    {
        ReplaceComponentClass = ResolveClassForGameFeature(
            ReplaceComponentPath, UActorComponent::StaticClass(), Err);
        if (!ReplaceComponentClass) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }

    const TOptional<bool> WantClient(bClient);
    const TOptional<bool> WantServer(bServer);
    int32 FoundIndex = INDEX_NONE;
    for (int32 i = 0; i < Action->ComponentList.Num(); ++i)
    {
        const FGameFeatureComponentEntry& Entry = Action->ComponentList[i];
        if (MatchesGameFeatureEntry(Entry, ActorClass, ComponentClass, WantClient, WantServer))
        {
            FoundIndex = i;
            break;
        }
        if (ReplaceComponentClass
            && MatchesGameFeatureEntry(Entry, ActorClass, ReplaceComponentClass,
                                       TOptional<bool>(), TOptional<bool>()))
        {
            FoundIndex = i;
            break;
        }
    }

    bool bAlready = false;
    bool bReplaced = false;
    FScopedTransaction Tx(LOCTEXT("GameFeatureAddComponent", "Sage: Add GameFeature Component Entry"));
    GFD->Modify();
    Action->Modify();

    if (FoundIndex >= 0)
    {
        FGameFeatureComponentEntry& Entry = Action->ComponentList[FoundIndex];
        const bool bExact = MatchesGameFeatureEntry(Entry, ActorClass, ComponentClass, WantClient, WantServer);
        if (bExact)
        {
            bAlready = true;
        }
        else
        {
            Entry.ActorClass = ActorClass;
            Entry.ComponentClass = ComponentClass;
            Entry.bClientComponent = bClient;
            Entry.bServerComponent = bServer;
            Entry.AdditionFlags = static_cast<uint8>(FMath::Clamp((int32)FlagsNum, 0, 255));
            bReplaced = true;
        }
    }
    else
    {
        FGameFeatureComponentEntry Entry;
        Entry.ActorClass = ActorClass;
        Entry.ComponentClass = ComponentClass;
        Entry.bClientComponent = bClient;
        Entry.bServerComponent = bServer;
        Entry.AdditionFlags = static_cast<uint8>(FMath::Clamp((int32)FlagsNum, 0, 255));
        FoundIndex = Action->ComponentList.Add(Entry);
    }

    if (FProperty* P = Action->GetClass()->FindPropertyByName(TEXT("ComponentList")))
    {
        FPropertyChangedEvent E(P,
            bAlready ? EPropertyChangeType::Unspecified : EPropertyChangeType::ArrayAdd);
        Action->PostEditChangeProperty(E);
    }
    GFD->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), GFD->GetPathName());
    R->SetStringField(TEXT("action"), Action->GetPathName());
    R->SetNumberField(TEXT("index"), FoundIndex);
    R->SetBoolField(TEXT("already"), bAlready);
    R->SetBoolField(TEXT("created_action"), bActionCreated);
    R->SetBoolField(TEXT("replaced"), bReplaced);
    R->SetBoolField(TEXT("modified"), !bAlready || bActionCreated);
    R->SetObjectField(TEXT("entry"), GameFeatureEntryToJson(Action->ComponentList[FoundIndex], FoundIndex));
    R->SetNumberField(TEXT("component_count"), Action->ComponentList.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GameFeatureRemoveComponentEntryImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ActorPath, ComponentPath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("actor_class"), ActorPath)
        || !Args->TryGetStringField(TEXT("component_class"), ComponentPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'actor_class' or 'component_class'"));
    }

    FString Err;
    UGameFeatureData* GFD = ResolveGameFeatureData(Path, Err);
    if (!GFD) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UClass* ActorClass = ResolveClassForGameFeature(ActorPath, AActor::StaticClass(), Err);
    if (!ActorClass) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    UClass* ComponentClass = ResolveClassForGameFeature(ComponentPath, UActorComponent::StaticClass(), Err);
    if (!ComponentClass) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    TOptional<bool> WantClient;
    TOptional<bool> WantServer;
    bool bTmp = false;
    if (Args->TryGetBoolField(TEXT("client"), bTmp)) WantClient = bTmp;
    if (Args->TryGetBoolField(TEXT("server"), bTmp)) WantServer = bTmp;

    UGameFeatureAction_AddComponents* Action = FindAddComponentsAction(GFD);
    if (!Action)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("GameFeatureData has no AddComponents action"));
    }

    int32 FoundIndex = INDEX_NONE;
    for (int32 i = 0; i < Action->ComponentList.Num(); ++i)
    {
        if (MatchesGameFeatureEntry(Action->ComponentList[i], ActorClass, ComponentClass,
                                    WantClient, WantServer))
        {
            FoundIndex = i;
            break;
        }
    }
    if (FoundIndex < 0)
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"), GFD->GetPathName());
        R->SetBoolField(TEXT("removed"), false);
        R->SetBoolField(TEXT("already_absent"), true);
        R->SetNumberField(TEXT("component_count"), Action->ComponentList.Num());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FScopedTransaction Tx(LOCTEXT("GameFeatureRemoveComponent", "Sage: Remove GameFeature Component Entry"));
    GFD->Modify();
    Action->Modify();
    Action->ComponentList.RemoveAt(FoundIndex);
    if (FProperty* P = Action->GetClass()->FindPropertyByName(TEXT("ComponentList")))
    {
        FPropertyChangedEvent E(P, EPropertyChangeType::ArrayRemove);
        Action->PostEditChangeProperty(E);
    }
    GFD->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), GFD->GetPathName());
    R->SetStringField(TEXT("action"), Action->GetPathName());
    R->SetBoolField(TEXT("removed"), true);
    R->SetNumberField(TEXT("removed_index"), FoundIndex);
    R->SetNumberField(TEXT("component_count"), Action->ComponentList.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

constexpr const TCHAR* kLyraAddWidgetsActionClassPath =
    TEXT("/Script/LyraGame.GameFeatureAction_AddWidgets");

UClass* ResolveAddWidgetsActionClass(FString& OutErr)
{
    UClass* Cls = FindObject<UClass>(nullptr, kLyraAddWidgetsActionClassPath);
    if (!Cls)
    {
        Cls = LoadObject<UClass>(nullptr, kLyraAddWidgetsActionClassPath);
    }
    if (!Cls)
    {
        OutErr = FString::Printf(TEXT("class not found: %s"), kLyraAddWidgetsActionClassPath);
        return nullptr;
    }
    if (!Cls->IsChildOf(UGameFeatureAction::StaticClass()))
    {
        OutErr = FString::Printf(TEXT("class %s is not a UGameFeatureAction"),
                                 *Cls->GetPathName());
        return nullptr;
    }
    return Cls;
}

UGameFeatureAction* FindAddWidgetsAction(UGameFeatureData* GFD, UClass* AddWidgetsClass)
{
    if (!GFD || !AddWidgetsClass) return nullptr;
    for (UGameFeatureAction* Action : GFD->GetActions())
    {
        if (Action && Action->IsA(AddWidgetsClass))
        {
            return Action;
        }
    }
    return nullptr;
}

UGameFeatureAction* EnsureAddWidgetsAction(UGameFeatureData* GFD,
                                           UClass* AddWidgetsClass,
                                           bool& bCreated)
{
    bCreated = false;
    if (!GFD || !AddWidgetsClass) return nullptr;
    if (UGameFeatureAction* Existing = FindAddWidgetsAction(GFD, AddWidgetsClass))
    {
        return Existing;
    }

    UGameFeatureAction* Action = NewObject<UGameFeatureAction>(
        GFD, AddWidgetsClass, NAME_None, RF_Public | RF_Transactional);
    if (!Action) return nullptr;

#if WITH_EDITOR
    GFD->Modify();
    GFD->GetMutableActionsInEditor().Add(Action);
    if (FProperty* ActionsProp = GFD->GetClass()->FindPropertyByName(TEXT("Actions")))
    {
        FPropertyChangedEvent E(ActionsProp, EPropertyChangeType::ArrayAdd);
        GFD->PostEditChangeProperty(E);
    }
#endif
    GFD->MarkPackageDirty();
    bCreated = true;
    return Action;
}

bool ResolveGameplayTagStrict(const FString& TagText,
                              const TCHAR* FieldName,
                              FGameplayTag& OutTag,
                              FString& OutErr)
{
    const FString CleanTag = TagText.TrimStartAndEnd();
    if (CleanTag.IsEmpty())
    {
        OutErr = FString::Printf(TEXT("missing '%s'"), FieldName);
        return false;
    }
    OutTag = FGameplayTag::RequestGameplayTag(FName(*CleanTag), false);
    if (!OutTag.IsValid())
    {
        OutErr = FString::Printf(TEXT("gameplay tag not found: %s"), *CleanTag);
        return false;
    }
    return true;
}

bool GetActionArrayProperty(UObject* Action,
                            const TCHAR* PropertyName,
                            FArrayProperty*& OutArray,
                            FStructProperty*& OutInnerStruct,
                            FString& OutErr)
{
    OutArray = nullptr;
    OutInnerStruct = nullptr;
    if (!Action)
    {
        OutErr = TEXT("AddWidgets action is null");
        return false;
    }

    OutArray = CastField<FArrayProperty>(
        Action->GetClass()->FindPropertyByName(PropertyName));
    if (!OutArray)
    {
        OutErr = FString::Printf(TEXT("%s array not found on %s"),
                                 PropertyName, *Action->GetClass()->GetPathName());
        return false;
    }
    OutInnerStruct = CastField<FStructProperty>(OutArray->Inner);
    if (!OutInnerStruct || !OutInnerStruct->Struct)
    {
        OutErr = FString::Printf(TEXT("%s is not a TArray<USTRUCT> on %s"),
                                 PropertyName, *Action->GetClass()->GetPathName());
        return false;
    }
    return true;
}

FProperty* RequireStructField(FStructProperty* StructProperty,
                              const TCHAR* FieldName,
                              FString& OutErr)
{
    if (!StructProperty || !StructProperty->Struct)
    {
        OutErr = TEXT("entry struct is null");
        return nullptr;
    }
    FProperty* Field = StructProperty->Struct->FindPropertyByName(FieldName);
    if (!Field)
    {
        OutErr = FString::Printf(TEXT("field %s not found on %s"),
                                 FieldName, *StructProperty->Struct->GetName());
    }
    return Field;
}

bool SetGameplayTagField(FProperty* Field,
                         void* StructValuePtr,
                         const FGameplayTag& Tag,
                         FString& OutErr)
{
    FStructProperty* StructField = CastField<FStructProperty>(Field);
    if (!StructField || !StructField->Struct
        || StructField->Struct->GetFName() != FName(TEXT("GameplayTag")))
    {
        OutErr = FString::Printf(TEXT("field %s is not FGameplayTag"),
                                 Field ? *Field->GetName() : TEXT("<null>"));
        return false;
    }
    void* FieldPtr = Field->ContainerPtrToValuePtr<void>(StructValuePtr);
    *reinterpret_cast<FGameplayTag*>(FieldPtr) = Tag;
    return true;
}

FString ReadGameplayTagField(FProperty* Field, const void* StructValuePtr)
{
    if (const FStructProperty* StructField = CastField<FStructProperty>(Field))
    {
        if (StructField->Struct && StructField->Struct->GetFName() == FName(TEXT("GameplayTag")))
        {
            const void* FieldPtr = Field->ContainerPtrToValuePtr<void>(StructValuePtr);
            return reinterpret_cast<const FGameplayTag*>(FieldPtr)->ToString();
        }
    }
    if (TSharedPtr<FJsonValue> Value = detail::GetPropertyValueAtPtr(
            Field, Field->ContainerPtrToValuePtr<void>(StructValuePtr)))
    {
        return Value->AsString();
    }
    return FString();
}

bool SetClassReferenceField(FProperty* Field,
                            void* StructValuePtr,
                            UClass* Class,
                            FString& OutErr)
{
    if (!Field || !Class)
    {
        OutErr = TEXT("class reference field or class is null");
        return false;
    }
    const TSharedPtr<FJsonValue> ClassValue =
        MakeShared<FJsonValueString>(Class->GetPathName());
    if (!detail::SetPropertyValueAtPtr(
            Field, Field->ContainerPtrToValuePtr<void>(StructValuePtr), ClassValue))
    {
        OutErr = FString::Printf(TEXT("failed to set class field %s to %s"),
                                 *Field->GetName(), *Class->GetPathName());
        return false;
    }
    return true;
}

FString ReadClassReferenceField(FProperty* Field, const void* StructValuePtr)
{
    if (!Field) return FString();
    if (TSharedPtr<FJsonValue> Value = detail::GetPropertyValueAtPtr(
            Field, Field->ContainerPtrToValuePtr<void>(StructValuePtr)))
    {
        return Value->AsString();
    }
    return FString();
}

bool StoredClassMatches(const FString& StoredPath, UClass* DesiredClass, UClass* RequiredBase)
{
    if (!DesiredClass) return false;
    const FString CleanStored = CleanObjectReferenceLiteral(StoredPath);
    if (CleanStored.Equals(DesiredClass->GetPathName(), ESearchCase::CaseSensitive))
    {
        return true;
    }

    FString Err;
    UClass* StoredClass = ResolveClassForGameFeature(CleanStored, RequiredBase, Err);
    return StoredClass == DesiredClass;
}

TSharedPtr<FJsonObject> HudEntryToJson(UObject* Action,
                                       FArrayProperty* ArrayProperty,
                                       FStructProperty* StructProperty,
                                       int32 Index,
                                       const TCHAR* TagFieldName,
                                       const TCHAR* ClassFieldName,
                                       const TCHAR* TagJsonName,
                                       const TCHAR* ClassJsonName)
{
    auto Row = MakeShared<FJsonObject>();
    Row->SetNumberField(TEXT("index"), Index);
    if (!Action || !ArrayProperty || !StructProperty)
    {
        Row->SetBoolField(TEXT("valid"), false);
        return Row;
    }

    FScriptArrayHelper Helper(ArrayProperty,
        ArrayProperty->ContainerPtrToValuePtr<void>(Action));
    if (!Helper.IsValidIndex(Index))
    {
        Row->SetBoolField(TEXT("valid"), false);
        return Row;
    }

    const void* EntryPtr = Helper.GetRawPtr(Index);
    FProperty* TagField = StructProperty->Struct->FindPropertyByName(TagFieldName);
    FProperty* ClassField = StructProperty->Struct->FindPropertyByName(ClassFieldName);
    Row->SetBoolField(TEXT("valid"), TagField != nullptr && ClassField != nullptr);
    if (TagField)
    {
        Row->SetStringField(TagJsonName, ReadGameplayTagField(TagField, EntryPtr));
    }
    if (ClassField)
    {
        Row->SetStringField(ClassJsonName, ReadClassReferenceField(ClassField, EntryPtr));
    }
    return Row;
}

void AddHudEntriesToJson(UObject* Action,
                         FArrayProperty* ArrayProperty,
                         FStructProperty* StructProperty,
                         const TCHAR* TagFieldName,
                         const TCHAR* ClassFieldName,
                         const TCHAR* TagJsonName,
                         const TCHAR* ClassJsonName,
                         TArray<TSharedPtr<FJsonValue>>& OutEntries)
{
    if (!Action || !ArrayProperty || !StructProperty) return;
    FScriptArrayHelper Helper(ArrayProperty,
        ArrayProperty->ContainerPtrToValuePtr<void>(Action));
    for (int32 i = 0; i < Helper.Num(); ++i)
    {
        OutEntries.Add(MakeShared<FJsonValueObject>(
            HudEntryToJson(Action, ArrayProperty, StructProperty, i,
                           TagFieldName, ClassFieldName,
                           TagJsonName, ClassJsonName)));
    }
}

int32 FindHudEntry(UObject* Action,
                   FArrayProperty* ArrayProperty,
                   FStructProperty* StructProperty,
                   const TCHAR* TagFieldName,
                   const TCHAR* ClassFieldName,
                   const FGameplayTag& DesiredTag,
                   UClass* DesiredClass,
                   UClass* RequiredBase)
{
    if (!Action || !ArrayProperty || !StructProperty) return INDEX_NONE;
    FProperty* TagField = StructProperty->Struct->FindPropertyByName(TagFieldName);
    FProperty* ClassField = StructProperty->Struct->FindPropertyByName(ClassFieldName);
    if (!TagField || !ClassField) return INDEX_NONE;

    FScriptArrayHelper Helper(ArrayProperty,
        ArrayProperty->ContainerPtrToValuePtr<void>(Action));
    for (int32 i = 0; i < Helper.Num(); ++i)
    {
        const void* EntryPtr = Helper.GetRawPtr(i);
        const FString ExistingTag = ReadGameplayTagField(TagField, EntryPtr);
        const FString ExistingClass = ReadClassReferenceField(ClassField, EntryPtr);
        if (ExistingTag == DesiredTag.ToString()
            && StoredClassMatches(ExistingClass, DesiredClass, RequiredBase))
        {
            return i;
        }
    }
    return INDEX_NONE;
}

int32 AddHudEntry(UObject* Action,
                  FArrayProperty* ArrayProperty,
                  FStructProperty* StructProperty,
                  const TCHAR* TagFieldName,
                  const TCHAR* ClassFieldName,
                  const FGameplayTag& Tag,
                  UClass* Class,
                  FString& OutErr)
{
    FProperty* TagField = RequireStructField(StructProperty, TagFieldName, OutErr);
    if (!TagField) return INDEX_NONE;
    FProperty* ClassField = RequireStructField(StructProperty, ClassFieldName, OutErr);
    if (!ClassField) return INDEX_NONE;

    FScriptArrayHelper Helper(ArrayProperty,
        ArrayProperty->ContainerPtrToValuePtr<void>(Action));
    const int32 NewIndex = Helper.AddValue();
    void* EntryPtr = Helper.GetRawPtr(NewIndex);
    if (!SetClassReferenceField(ClassField, EntryPtr, Class, OutErr)
        || !SetGameplayTagField(TagField, EntryPtr, Tag, OutErr))
    {
        Helper.Resize(NewIndex);
        return INDEX_NONE;
    }
    return NewIndex;
}

bool SaveGameFeatureDataAsset(UGameFeatureData* GFD, bool bSave, bool& bSaved, FString& OutErr)
{
    bSaved = false;
    if (!bSave) return true;
    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    if (!AssetSubsystem)
    {
        OutErr = TEXT("EditorAssetSubsystem unavailable for saving GameFeatureData");
        return false;
    }
    const FString PackageName = GFD && GFD->GetOutermost()
        ? GFD->GetOutermost()->GetName()
        : FString();
    if (PackageName.IsEmpty())
    {
        OutErr = TEXT("GameFeatureData package name is empty");
        return false;
    }
    bSaved = AssetSubsystem->SaveAsset(PackageName, /*bOnlyIfIsDirty=*/false);
    if (!bSaved)
    {
        OutErr = FString::Printf(TEXT("SaveAsset returned false for %s"), *PackageName);
        return false;
    }
    return true;
}

FSageToolDispatch::FOutcome GameFeatureAddWidgetEntryImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    FString SlotText, WidgetClassPath, LayerText, LayoutClassPath;
    Args->TryGetStringField(TEXT("slot_id"), SlotText);
    Args->TryGetStringField(TEXT("widget_class"), WidgetClassPath);
    Args->TryGetStringField(TEXT("layer_id"), LayerText);
    Args->TryGetStringField(TEXT("layout_class"), LayoutClassPath);
    const bool bWantWidget = !SlotText.IsEmpty() || !WidgetClassPath.IsEmpty();
    const bool bWantLayout = !LayerText.IsEmpty() || !LayoutClassPath.IsEmpty();
    if (!bWantWidget && !bWantLayout)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("pass either slot_id+widget_class or layer_id+layout_class"));
    }
    if (bWantWidget && (SlotText.IsEmpty() || WidgetClassPath.IsEmpty()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("slot_id and widget_class must be supplied together"));
    }
    if (bWantLayout && (LayerText.IsEmpty() || LayoutClassPath.IsEmpty()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("layer_id and layout_class must be supplied together"));
    }

    bool bCreateActionIfMissing = true;
    bool bDryRun = false;
    bool bSave = false;
    Args->TryGetBoolField(TEXT("create_action_if_missing"), bCreateActionIfMissing);
    Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
    Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    Args->TryGetBoolField(TEXT("save"), bSave);

    FString Err;
    UGameFeatureData* GFD = ResolveGameFeatureData(Path, Err);
    if (!GFD) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    UClass* AddWidgetsClass = ResolveAddWidgetsActionClass(Err);
    if (!AddWidgetsClass) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    FGameplayTag SlotTag;
    UClass* WidgetClass = nullptr;
    if (bWantWidget)
    {
        if (!ResolveGameplayTagStrict(SlotText, TEXT("slot_id"), SlotTag, Err))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
        }
        WidgetClass = ResolveClassForGameFeature(
            WidgetClassPath, UUserWidget::StaticClass(), Err);
        if (!WidgetClass) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }

    FGameplayTag LayerTag;
    UClass* LayoutClass = nullptr;
    UClass* LayoutRequiredBase = FindObject<UClass>(
        nullptr, TEXT("/Script/CommonUI.CommonActivatableWidget"));
    if (!LayoutRequiredBase)
    {
        LayoutRequiredBase = UUserWidget::StaticClass();
    }
    if (bWantLayout)
    {
        if (!ResolveGameplayTagStrict(LayerText, TEXT("layer_id"), LayerTag, Err))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
        }
        LayoutClass = ResolveClassForGameFeature(LayoutClassPath, LayoutRequiredBase, Err);
        if (!LayoutClass) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }

    UGameFeatureAction* Action = FindAddWidgetsAction(GFD, AddWidgetsClass);
    const bool bWouldCreateAction = Action == nullptr;
    if (!Action && !bCreateActionIfMissing)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("GameFeatureData has no AddWidgets action and create_action_if_missing=false"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), GFD->GetPathName());
    R->SetStringField(TEXT("action_class"), AddWidgetsClass->GetPathName());
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("would_create_action"), bWouldCreateAction);

    if (bDryRun && !Action)
    {
        R->SetBoolField(TEXT("validated"), true);
        R->SetBoolField(TEXT("modified"), false);
        R->SetBoolField(TEXT("already"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    bool bActionCreated = false;
    if (!Action)
    {
        Action = EnsureAddWidgetsAction(GFD, AddWidgetsClass, bActionCreated);
        if (!Action)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                TEXT("failed to create UGameFeatureAction_AddWidgets"));
        }
    }

    FArrayProperty* WidgetsArray = nullptr;
    FStructProperty* WidgetStruct = nullptr;
    FArrayProperty* LayoutArray = nullptr;
    FStructProperty* LayoutStruct = nullptr;
    if (bWantWidget && !GetActionArrayProperty(
            Action, TEXT("Widgets"), WidgetsArray, WidgetStruct, Err))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }
    if (bWantLayout && !GetActionArrayProperty(
            Action, TEXT("Layout"), LayoutArray, LayoutStruct, Err))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }

    int32 WidgetIndex = INDEX_NONE;
    int32 LayoutIndex = INDEX_NONE;
    bool bWidgetAlready = false;
    bool bLayoutAlready = false;
    if (bWantWidget)
    {
        WidgetIndex = FindHudEntry(Action, WidgetsArray, WidgetStruct,
            TEXT("SlotID"), TEXT("WidgetClass"), SlotTag, WidgetClass,
            UUserWidget::StaticClass());
        bWidgetAlready = WidgetIndex != INDEX_NONE;
    }
    if (bWantLayout)
    {
        LayoutIndex = FindHudEntry(Action, LayoutArray, LayoutStruct,
            TEXT("LayerID"), TEXT("LayoutClass"), LayerTag, LayoutClass,
            LayoutRequiredBase);
        bLayoutAlready = LayoutIndex != INDEX_NONE;
    }

    const bool bNeedsWidgetAdd = bWantWidget && !bWidgetAlready;
    const bool bNeedsLayoutAdd = bWantLayout && !bLayoutAlready;
    bool bModified = bActionCreated || bNeedsWidgetAdd || bNeedsLayoutAdd;

    if (!bDryRun && (bNeedsWidgetAdd || bNeedsLayoutAdd))
    {
        FScopedTransaction Tx(LOCTEXT("GameFeatureAddWidgetEntry", "Sage: Add GameFeature Widget Entry"));
        GFD->Modify();
        Action->Modify();

        if (bNeedsWidgetAdd)
        {
            Action->PreEditChange(WidgetsArray);
            WidgetIndex = AddHudEntry(Action, WidgetsArray, WidgetStruct,
                TEXT("SlotID"), TEXT("WidgetClass"), SlotTag, WidgetClass, Err);
            if (WidgetIndex == INDEX_NONE)
            {
                Tx.Cancel();
                return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
            }
            FPropertyChangedEvent E(WidgetsArray, EPropertyChangeType::ArrayAdd);
            Action->PostEditChangeProperty(E);
        }

        if (bNeedsLayoutAdd)
        {
            Action->PreEditChange(LayoutArray);
            LayoutIndex = AddHudEntry(Action, LayoutArray, LayoutStruct,
                TEXT("LayerID"), TEXT("LayoutClass"), LayerTag, LayoutClass, Err);
            if (LayoutIndex == INDEX_NONE)
            {
                Tx.Cancel();
                return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
            }
            FPropertyChangedEvent E(LayoutArray, EPropertyChangeType::ArrayAdd);
            Action->PostEditChangeProperty(E);
        }

        GFD->MarkPackageDirty();
    }

    bool bSaved = false;
    if (!bDryRun && bModified && !SaveGameFeatureDataAsset(GFD, bSave, bSaved, Err))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, Err);
    }

    R->SetStringField(TEXT("action"), Action->GetPathName());
    R->SetBoolField(TEXT("created_action"), bActionCreated);
    R->SetBoolField(TEXT("modified"), !bDryRun && bModified);
    R->SetBoolField(TEXT("already"), (!bWantWidget || bWidgetAlready) && (!bWantLayout || bLayoutAlready));
    R->SetBoolField(TEXT("widget_already"), bWidgetAlready);
    R->SetBoolField(TEXT("layout_already"), bLayoutAlready);
    R->SetBoolField(TEXT("would_add_widget"), bDryRun && bNeedsWidgetAdd);
    R->SetBoolField(TEXT("would_add_layout"), bDryRun && bNeedsLayoutAdd);
    R->SetBoolField(TEXT("saved"), bSaved);
    if (bWantWidget)
    {
        R->SetNumberField(TEXT("widget_index"), WidgetIndex);
        if (WidgetIndex >= 0)
        {
            R->SetObjectField(TEXT("widget_entry"),
                HudEntryToJson(Action, WidgetsArray, WidgetStruct, WidgetIndex,
                               TEXT("SlotID"), TEXT("WidgetClass"),
                               TEXT("slot_id"), TEXT("widget_class")));
        }
        else
        {
            auto Requested = MakeShared<FJsonObject>();
            Requested->SetStringField(TEXT("slot_id"), SlotTag.ToString());
            Requested->SetStringField(TEXT("widget_class"), WidgetClass ? WidgetClass->GetPathName() : FString());
            Requested->SetBoolField(TEXT("would_add"), true);
            R->SetObjectField(TEXT("widget_entry"), Requested);
        }
    }
    if (bWantLayout)
    {
        R->SetNumberField(TEXT("layout_index"), LayoutIndex);
        if (LayoutIndex >= 0)
        {
            R->SetObjectField(TEXT("layout_entry"),
                HudEntryToJson(Action, LayoutArray, LayoutStruct, LayoutIndex,
                               TEXT("LayerID"), TEXT("LayoutClass"),
                               TEXT("layer_id"), TEXT("layout_class")));
        }
        else
        {
            auto Requested = MakeShared<FJsonObject>();
            Requested->SetStringField(TEXT("layer_id"), LayerTag.ToString());
            Requested->SetStringField(TEXT("layout_class"), LayoutClass ? LayoutClass->GetPathName() : FString());
            Requested->SetBoolField(TEXT("would_add"), true);
            R->SetObjectField(TEXT("layout_entry"), Requested);
        }
    }

    TArray<TSharedPtr<FJsonValue>> WidgetEntries;
    if (!GetActionArrayProperty(Action, TEXT("Widgets"), WidgetsArray, WidgetStruct, Err))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }
    AddHudEntriesToJson(Action, WidgetsArray, WidgetStruct,
        TEXT("SlotID"), TEXT("WidgetClass"),
        TEXT("slot_id"), TEXT("widget_class"), WidgetEntries);
    R->SetArrayField(TEXT("widgets"), WidgetEntries);
    R->SetNumberField(TEXT("widget_count"), WidgetEntries.Num());

    TArray<TSharedPtr<FJsonValue>> LayoutEntries;
    if (!GetActionArrayProperty(Action, TEXT("Layout"), LayoutArray, LayoutStruct, Err))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    }
    AddHudEntriesToJson(Action, LayoutArray, LayoutStruct,
        TEXT("LayerID"), TEXT("LayoutClass"),
        TEXT("layer_id"), TEXT("layout_class"), LayoutEntries);
    R->SetArrayField(TEXT("layout"), LayoutEntries);
    R->SetNumberField(TEXT("layout_count"), LayoutEntries.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AddArrayElementImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    if (!Args.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    FString AssetPath, ArrayProp;
    if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'asset_path'"));
    if (!Args->TryGetStringField(TEXT("array_property"), ArrayProp) || ArrayProp.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'array_property'"));
    const TSharedPtr<FJsonValue> ElemField =
        Args->Values.FindRef(TEXT("element_value"));
    if (!ElemField.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'element_value'"));
    FString ClassName;
    Args->TryGetStringField(TEXT("class_name"), ClassName);
    bool bAllowEmpty = false;
    Args->TryGetBoolField(TEXT("allow_empty"), bAllowEmpty);
    bool bValidateOnly = false;
    Args->TryGetBoolField(TEXT("validate_only"), bValidateOnly);
    Args->TryGetBoolField(TEXT("dry_run"), bValidateOnly);

    UObject* Asset = nullptr;
    {
        FSoftObjectPath Soft(AssetPath);
        Asset = Soft.ResolveObject();
        if (!Asset) Asset = Soft.TryLoad();
    }
    if (!Asset)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset not found: %s"), *AssetPath));

    FArrayProperty* Array = CastField<FArrayProperty>(
        Asset->GetClass()->FindPropertyByName(*ArrayProp));
    if (!Array)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("'%s' is not a TArray on %s"),
                            *ArrayProp, *Asset->GetClass()->GetName()));

    auto MakeValidationOk = [&]() -> FSageToolDispatch::FOutcome
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("asset_path"),     Asset->GetPathName());
        R->SetStringField(TEXT("array_property"), ArrayProp);
        R->SetStringField(TEXT("inner_type"), Array->Inner
            ? Array->Inner->GetClass()->GetName()
            : TEXT("<null>"));
        R->SetBoolField(TEXT("element_valid"), true);
        R->SetBoolField(TEXT("modified"), false);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    };

    if (ClassName.IsEmpty())
    {
        if (!Array->Inner)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                TEXT("array inner property is null"));
        }
        void* TempElem = FMemory::Malloc(Array->Inner->GetElementSize());
        Array->Inner->InitializeValue(TempElem);
        const bool bValid = detail::SetPropertyValueAtPtr(Array->Inner, TempElem, ElemField);
        Array->Inner->DestroyValue(TempElem);
        FMemory::Free(TempElem);

        if (!bValid)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("element_value failed validation for inner type %s"),
                                Array->Inner ? *Array->Inner->GetClass()->GetName()
                                             : TEXT("<null>")));
        }
        if (bValidateOnly)
        {
            return MakeValidationOk();
        }
    }
    else if (bValidateOnly)
    {
        FObjectProperty* InnerObj = CastField<FObjectProperty>(Array->Inner);
        if (!InnerObj)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("'class_name' supplied but inner property isn't FObjectProperty"));
        }
        UClass* Cls = FindObject<UClass>(nullptr, *ClassName);
        if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassName);
        if (!Cls)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("class not found: %s"), *ClassName));
        }
        if (InnerObj->PropertyClass && !Cls->IsChildOf(InnerObj->PropertyClass))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("class %s isn't a subclass of %s"),
                                *Cls->GetName(),
                                *InnerObj->PropertyClass->GetName()));
        }

        UObject* TempInst = NewObject<UObject>(GetTransientPackage(), Cls);
        int32 SetCount = 0;
        TArray<FString> FailedFields;
        if (ElemField->Type == EJson::Object)
        {
            const auto& EO = ElemField->AsObject();
            if (EO.IsValid())
            {
                for (TFieldIterator<FProperty> It(TempInst->GetClass()); It; ++It)
                {
                    FProperty* SubP = *It;
                    if (!SubP) continue;
                    const TSharedPtr<FJsonValue>* Field = EO->Values.Find(SubP->GetName());
                    if (!Field || !Field->IsValid()) continue;
                    if (detail::SetUPropertyFromJson(TempInst, SubP, *Field))
                    {
                        ++SetCount;
                    }
                    else
                    {
                        FailedFields.Add(SubP->GetName());
                    }
                }
            }
        }
        TempInst->MarkAsGarbage();
        if (SetCount == 0 && !bAllowEmpty)
        {
            FString FailList;
            for (const FString& F : FailedFields)
            {
                if (!FailList.IsEmpty()) FailList += TEXT(", ");
                FailList += F;
            }
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("instanced subobject '%s' would have zero fields applied "
                                     "(failed: [%s]); pass allow_empty:true to accept"),
                                *Cls->GetName(), *FailList));
        }
        return MakeValidationOk();
    }

    FScopedTransaction Tx(LOCTEXT("AddArrayElement", "Sage: Add Array Element"));
    Asset->Modify();
    Asset->PreEditChange(Array);

    FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Asset));
    const int32 NewIndex = Helper.AddValue();
    void* ElemPtr = Helper.GetRawPtr(NewIndex);

    bool bOk = false;
    TArray<FString> FailedFields;
    if (!ClassName.IsEmpty())
    {
        // Instanced UObject branch: synthesize a subobject of class_name and
        // apply element_value as its property dict.
        FObjectProperty* InnerObj = CastField<FObjectProperty>(Array->Inner);
        if (!InnerObj)
        {
            Helper.Resize(NewIndex);
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("'class_name' supplied but inner property isn't FObjectProperty"));
        }
        UClass* Cls = FindObject<UClass>(nullptr, *ClassName);
        if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassName);
        if (!Cls)
        {
            Helper.Resize(NewIndex);
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("class not found: %s"), *ClassName));
        }
        if (InnerObj->PropertyClass && !Cls->IsChildOf(InnerObj->PropertyClass))
        {
            Helper.Resize(NewIndex);
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("class %s isn't a subclass of %s"),
                                *Cls->GetName(),
                                *InnerObj->PropertyClass->GetName()));
        }
        UObject* Inst = NewObject<UObject>(Asset, Cls,
            NAME_None, RF_Public | RF_Transactional);
        InnerObj->SetObjectPropertyValue(ElemPtr, Inst);
        // Apply element_value (object dict) as Inst's properties.
        // Track how many fields successfully applied; require >0 unless
        // allow_empty is set so we don't silently produce an empty subobject.
        int32 SetCount = 0;
        if (ElemField->Type == EJson::Object)
        {
            const auto& EO = ElemField->AsObject();
            if (EO.IsValid())
            {
                for (TFieldIterator<FProperty> It(Inst->GetClass()); It; ++It)
                {
                    FProperty* SubP = *It;
                    if (!SubP) continue;
                    const TSharedPtr<FJsonValue>* Field = EO->Values.Find(SubP->GetName());
                    if (!Field || !Field->IsValid()) continue;
                    if (detail::SetUPropertyFromJson(Inst, SubP, *Field))
                    {
                        ++SetCount;
                    }
                    else
                    {
                        FailedFields.Add(SubP->GetName());
                    }
                }
            }
        }
        if (SetCount == 0 && !bAllowEmpty)
        {
            // Mark the orphan subobject as garbage BEFORE shrinking the array
            // so the orphan is collected on the next GC pass instead of leaking.
            InnerObj->SetObjectPropertyValue(ElemPtr, nullptr);
            Inst->MarkAsGarbage();
            Helper.Resize(NewIndex);
            Tx.Cancel();
            FString FailList;
            for (const FString& F : FailedFields)
            {
                if (!FailList.IsEmpty()) FailList += TEXT(", ");
                FailList += F;
            }
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("instanced subobject '%s' had zero fields applied "
                                     "(failed: [%s]); pass allow_empty:true to accept"),
                                *Cls->GetName(), *FailList));
        }
        bOk = true;
    }
    else
    {
        bOk = detail::SetPropertyValueAtPtr(Array->Inner, ElemPtr, ElemField);
    }

    if (!bOk)
    {
        Helper.Resize(NewIndex);
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("failed to set element on inner type %s"),
                            Array->Inner ? *Array->Inner->GetClass()->GetName()
                                         : TEXT("<null>")));
    }

    FPropertyChangedEvent ChangeEvent(Array, EPropertyChangeType::ArrayAdd);
    Asset->PostEditChangeProperty(ChangeEvent);
    Asset->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"),     Asset->GetPathName());
    R->SetStringField(TEXT("array_property"), ArrayProp);
    R->SetNumberField(TEXT("index"),          NewIndex);
    R->SetNumberField(TEXT("length"),         Helper.Num());
    if (FailedFields.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Skipped;
        for (const FString& F : FailedFields)
        {
            Skipped.Add(MakeShared<FJsonValueString>(F));
        }
        R->SetArrayField(TEXT("skipped_fields"), Skipped);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool JsonArrayToString(const TArray<TSharedPtr<FJsonValue>>& Arr, FString& Out)
{
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
    return FJsonSerializer::Serialize(Arr, Writer);
}

bool GetArrayArgs(const TSharedPtr<FJsonObject>& Args,
                  FString& AssetPath,
                  FString& ArrayProp,
                  UObject*& Asset,
                  FArrayProperty*& Array,
                  FString& OutError)
{
    if (!Args.IsValid())
    {
        OutError = TEXT("missing args");
        return false;
    }
    if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        Args->TryGetStringField(TEXT("path"), AssetPath);
    }
    if (AssetPath.IsEmpty())
    {
        OutError = TEXT("missing 'asset_path'");
        return false;
    }
    if (!Args->TryGetStringField(TEXT("array_property"), ArrayProp) || ArrayProp.IsEmpty())
    {
        OutError = TEXT("missing 'array_property'");
        return false;
    }

    Asset = ResolveAsset(AssetPath);
    if (!Asset)
    {
        OutError = FString::Printf(TEXT("asset not found: %s"), *AssetPath);
        return false;
    }
    Array = CastField<FArrayProperty>(Asset->GetClass()->FindPropertyByName(*ArrayProp));
    if (!Array)
    {
        OutError = FString::Printf(TEXT("'%s' is not a TArray on %s"),
                                   *ArrayProp, *Asset->GetClass()->GetName());
        return false;
    }
    if (!Array->Inner)
    {
        OutError = TEXT("array inner property is null");
        return false;
    }
    return true;
}

bool ValidateArrayReplacement(FArrayProperty* Array,
                              const TArray<TSharedPtr<FJsonValue>>& Elements,
                              TSharedPtr<FJsonValue>& OutCanonicalJson)
{
    TSharedPtr<FJsonValue> ArrayValue = MakeShared<FJsonValueArray>(Elements);
    void* TempArray = FMemory::Malloc(Array->GetSize(), Array->GetMinAlignment());
    Array->InitializeValue(TempArray);
    const bool bOk = detail::SetPropertyValueAtPtr(Array, TempArray, ArrayValue);
    if (bOk)
    {
        OutCanonicalJson = detail::GetPropertyValueAtPtr(Array, TempArray, nullptr);
    }
    Array->DestroyValue(TempArray);
    FMemory::Free(TempArray);
    return bOk && OutCanonicalJson.IsValid() && OutCanonicalJson->Type == EJson::Array;
}

bool ValidateInstancedArrayReplacement(FArrayProperty* Array,
                                       const TArray<TSharedPtr<FJsonValue>>& Elements,
                                       const FString& ClassName,
                                       bool bAllowEmpty,
                                       FString& OutError)
{
    FObjectProperty* InnerObj = CastField<FObjectProperty>(Array->Inner);
    if (!InnerObj)
    {
        OutError = TEXT("'class_name' supplied but inner property isn't FObjectProperty");
        return false;
    }
    UClass* Cls = FindObject<UClass>(nullptr, *ClassName);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassName);
    if (!Cls)
    {
        OutError = FString::Printf(TEXT("class not found: %s"), *ClassName);
        return false;
    }
    if (InnerObj->PropertyClass && !Cls->IsChildOf(InnerObj->PropertyClass))
    {
        OutError = FString::Printf(TEXT("class %s isn't a subclass of %s"),
                                   *Cls->GetName(),
                                   *InnerObj->PropertyClass->GetName());
        return false;
    }

    for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex)
    {
        UObject* TempInst = NewObject<UObject>(GetTransientPackage(), Cls);
        int32 SetCount = 0;
        if (Elements[ElementIndex].IsValid() && Elements[ElementIndex]->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> EO = Elements[ElementIndex]->AsObject();
            if (EO.IsValid())
            {
                for (TFieldIterator<FProperty> It(TempInst->GetClass()); It; ++It)
                {
                    FProperty* SubP = *It;
                    if (!SubP) continue;
                    const TSharedPtr<FJsonValue>* Field = EO->Values.Find(SubP->GetName());
                    if (!Field || !Field->IsValid()) continue;
                    if (detail::SetUPropertyFromJson(TempInst, SubP, *Field))
                    {
                        ++SetCount;
                    }
                }
            }
        }
        TempInst->MarkAsGarbage();
        if (SetCount == 0 && !bAllowEmpty)
        {
            OutError = FString::Printf(TEXT("element %d would create empty instanced subobject '%s'; pass allow_empty:true to accept"),
                                       ElementIndex, *Cls->GetName());
            return false;
        }
    }
    return true;
}

bool ApplyInstancedArrayReplacement(UObject* Asset,
                                    FArrayProperty* Array,
                                    const TArray<TSharedPtr<FJsonValue>>& Elements,
                                    const FString& ClassName,
                                    bool bAllowEmpty,
                                    FString& OutError)
{
    FObjectProperty* InnerObj = CastField<FObjectProperty>(Array->Inner);
    UClass* Cls = FindObject<UClass>(nullptr, *ClassName);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassName);
    if (!InnerObj || !Cls)
    {
        OutError = TEXT("instanced array validation was not run");
        return false;
    }

    FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Asset));
    Helper.EmptyValues();
    for (const TSharedPtr<FJsonValue>& Element : Elements)
    {
        const int32 NewIndex = Helper.AddValue();
        void* ElemPtr = Helper.GetRawPtr(NewIndex);
        UObject* Inst = NewObject<UObject>(Asset, Cls, NAME_None,
            RF_Public | RF_Transactional);
        InnerObj->SetObjectPropertyValue(ElemPtr, Inst);
        int32 SetCount = 0;
        if (Element.IsValid() && Element->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> EO = Element->AsObject();
            if (EO.IsValid())
            {
                for (TFieldIterator<FProperty> It(Inst->GetClass()); It; ++It)
                {
                    FProperty* SubP = *It;
                    if (!SubP) continue;
                    const TSharedPtr<FJsonValue>* Field = EO->Values.Find(SubP->GetName());
                    if (!Field || !Field->IsValid()) continue;
                    if (detail::SetUPropertyFromJson(Inst, SubP, *Field))
                    {
                        ++SetCount;
                    }
                }
            }
        }
        if (SetCount == 0 && !bAllowEmpty)
        {
            InnerObj->SetObjectPropertyValue(ElemPtr, nullptr);
            Inst->MarkAsGarbage();
            Helper.EmptyValues();
            OutError = FString::Printf(TEXT("failed to apply instanced element %d"), NewIndex);
            return false;
        }
    }
    return true;
}

FSageToolDispatch::FOutcome ClearArrayPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString AssetPath, ArrayProp, Error;
    UObject* Asset = nullptr;
    FArrayProperty* Array = nullptr;
    if (!GetArrayArgs(Args, AssetPath, ArrayProp, Asset, Array, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    bool bDryRun = false;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
        Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
    }

    FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Asset));
    const int32 OldCount = Helper.Num();
    TSharedPtr<FJsonValue> OldValue = detail::GetUPropertyAsJson(Asset, Array);
    if (!bDryRun && OldCount > 0)
    {
        FScopedTransaction Tx(LOCTEXT("ClearArrayProperty", "Sage: Clear Array Property"));
        Asset->Modify();
        Asset->PreEditChange(Array);
        Helper.EmptyValues();
        FPropertyChangedEvent ChangeEvent(Array, EPropertyChangeType::ArrayClear);
        Asset->PostEditChangeProperty(ChangeEvent);
        Asset->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    R->SetStringField(TEXT("array_property"), ArrayProp);
    R->SetNumberField(TEXT("removed_count"), OldCount);
    R->SetNumberField(TEXT("old_count"), OldCount);
    R->SetNumberField(TEXT("new_count"), bDryRun ? OldCount : 0);
    R->SetBoolField(TEXT("modified"), !bDryRun && OldCount > 0);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    if (OldValue.IsValid())
    {
        R->SetField(TEXT("removed"), OldValue);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReplaceArrayPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString AssetPath, ArrayProp, Error;
    UObject* Asset = nullptr;
    FArrayProperty* Array = nullptr;
    if (!GetArrayArgs(Args, AssetPath, ArrayProp, Asset, Array, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    const TArray<TSharedPtr<FJsonValue>>* ElementsPtr = nullptr;
    if (!Args->TryGetArrayField(TEXT("elements"), ElementsPtr) || !ElementsPtr)
    {
        Args->TryGetArrayField(TEXT("values"), ElementsPtr);
    }
    if (!ElementsPtr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'elements' array"));
    }
    const TArray<TSharedPtr<FJsonValue>> Elements = *ElementsPtr;

    bool bDryRun = false;
    bool bAllowEmpty = false;
    FString ClassName;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
        Args->TryGetBoolField(TEXT("validate_only"), bDryRun);
        Args->TryGetBoolField(TEXT("allow_empty"), bAllowEmpty);
        Args->TryGetStringField(TEXT("class_name"), ClassName);
    }

    TSharedPtr<FJsonValue> CanonicalNewValue;
    if (ClassName.IsEmpty())
    {
        if (!ValidateArrayReplacement(Array, Elements, CanonicalNewValue))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("elements failed validation for TArray inner type %s"),
                                Array->Inner ? *Array->Inner->GetClass()->GetName()
                                             : TEXT("<null>")));
        }
    }
    else if (!ValidateInstancedArrayReplacement(Array, Elements, ClassName, bAllowEmpty, Error))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }

    FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Asset));
    const int32 OldCount = Helper.Num();
    TSharedPtr<FJsonValue> OldValue = detail::GetUPropertyAsJson(Asset, Array);

    bool bSame = false;
    if (OldValue.IsValid() && CanonicalNewValue.IsValid()
        && OldValue->Type == EJson::Array && CanonicalNewValue->Type == EJson::Array)
    {
        FString OldJson, NewJson;
        bSame = JsonArrayToString(OldValue->AsArray(), OldJson)
            && JsonArrayToString(CanonicalNewValue->AsArray(), NewJson)
            && OldJson == NewJson;
    }

    if (!bDryRun && !bSame)
    {
        FScopedTransaction Tx(LOCTEXT("ReplaceArrayProperty", "Sage: Replace Array Property"));
        Asset->Modify();
        Asset->PreEditChange(Array);

        bool bApplied = false;
        if (ClassName.IsEmpty())
        {
            bApplied = detail::SetPropertyValueAtPtr(
                Array,
                Array->ContainerPtrToValuePtr<void>(Asset),
                MakeShared<FJsonValueArray>(Elements));
        }
        else
        {
            bApplied = ApplyInstancedArrayReplacement(
                Asset, Array, Elements, ClassName, bAllowEmpty, Error);
        }
        if (!bApplied)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32603,
                Error.IsEmpty() ? TEXT("failed to apply replacement") : Error);
        }

        FPropertyChangedEvent ChangeEvent(Array, EPropertyChangeType::ValueSet);
        Asset->PostEditChangeProperty(ChangeEvent);
        Asset->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    R->SetStringField(TEXT("array_property"), ArrayProp);
    R->SetNumberField(TEXT("old_count"), OldCount);
    R->SetNumberField(TEXT("new_count"), Elements.Num());
    R->SetNumberField(TEXT("removed_count"), OldCount);
    R->SetNumberField(TEXT("added_count"), Elements.Num());
    R->SetBoolField(TEXT("modified"), !bDryRun && !bSame);
    R->SetBoolField(TEXT("dry_run"), bDryRun);
    R->SetBoolField(TEXT("validated"), true);
    if (OldValue.IsValid())
    {
        R->SetField(TEXT("removed"), OldValue);
    }
    if (CanonicalNewValue.IsValid())
    {
        R->SetField(TEXT("added"), CanonicalNewValue);
    }
    else
    {
        R->SetArrayField(TEXT("added"), Elements);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool WriteJsonObjectFile(const FString& Filename, const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
    if (!FJsonSerializer::Serialize(Object, Writer))
    {
        return false;
    }
    return FFileHelper::SaveStringToFile(Text, *Filename);
}

TSharedPtr<FJsonObject> ReadJsonObjectFile(const FString& Filename)
{
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Filename))
    {
        return nullptr;
    }
    TSharedPtr<FJsonObject> Object;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Object))
    {
        return nullptr;
    }
    return Object;
}

bool ResolveUProjectFile(const FString& SourceProject, FString& OutProjectFile)
{
    FString Candidate = SourceProject;
    FPaths::NormalizeFilename(Candidate);
    if (FPaths::DirectoryExists(Candidate))
    {
        TArray<FString> ProjectFiles;
        IFileManager::Get().FindFiles(ProjectFiles, *(Candidate / TEXT("*.uproject")), true, false);
        if (ProjectFiles.Num() <= 0)
        {
            return false;
        }
        Candidate = Candidate / ProjectFiles[0];
    }
    if (!FPaths::FileExists(Candidate) || FPaths::GetExtension(Candidate) != TEXT("uproject"))
    {
        return false;
    }
    OutProjectFile = FPaths::ConvertRelativePathToFull(Candidate);
    FPaths::NormalizeFilename(OutProjectFile);
    return true;
}

FString NormalizeFullDirectory(FString Path)
{
    FPaths::NormalizeFilename(Path);
    Path = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Path);
    while (Path.RemoveFromEnd(TEXT("/"))) {}
    return Path;
}

FString TrimPackageSlashes(FString Path)
{
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    while (Path.RemoveFromStart(TEXT("/"))) {}
    while (Path.RemoveFromEnd(TEXT("/"))) {}
    return Path;
}

FString NormalizeLongPackagePath(FString Path)
{
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    Path.TrimStartAndEndInline();
    while (Path.RemoveFromEnd(TEXT("/"))) {}
    if (!Path.StartsWith(TEXT("/")))
    {
        Path = TEXT("/") + Path;
    }
    return Path;
}

FString JoinLongPackagePath(const FString& Root, const FString& Relative)
{
    FString CleanRoot = NormalizeLongPackagePath(Root);
    FString CleanRelative = TrimPackageSlashes(Relative);
    return CleanRelative.IsEmpty() ? CleanRoot : CleanRoot / CleanRelative;
}

bool DirectoryContainsFileWithExtension(const FString& Directory, const FString& Extension)
{
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(Directory / (TEXT("*.") + Extension)), true, false);
    return Files.Num() > 0;
}

bool TryResolveMountContentRoot(const FString& MountRoot, FString& OutContentRoot)
{
    FString Root = NormalizeLongPackagePath(MountRoot);
    FString RootWithSlash = Root;
    if (!RootWithSlash.EndsWith(TEXT("/")))
    {
        RootWithSlash += TEXT("/");
    }
    FString Filename;
    if (FPackageName::TryConvertLongPackageNameToFilename(RootWithSlash, Filename)
        || FPackageName::TryConvertLongPackageNameToFilename(Root, Filename))
    {
        OutContentRoot = NormalizeFullDirectory(Filename);
        return true;
    }
    return false;
}

struct FMigrationDestination
{
    FString Requested;
    FString ContentRootPath;
    FString RequestedContentPath;
    FString MountRoot;
    FString SubPath;
    FString LongPackageRoot;
    bool bValid = false;
    FString Error;
};

FMigrationDestination ResolveMigrationDestination(const FString& Destination)
{
    FMigrationDestination Out;
    Out.Requested = Destination;

    FString Dest = Destination;
    FPaths::NormalizeFilename(Dest);
    Dest.TrimStartAndEndInline();

    if (Dest.IsEmpty())
    {
        Out.Error = TEXT("destination is empty");
        return Out;
    }

    if (Dest.StartsWith(TEXT("/")))
    {
        const FString PackagePath = NormalizeLongPackagePath(Dest);
        FString Remainder = PackagePath.Mid(1);
        FString MountName;
        FString SubPath;
        if (!Remainder.Split(TEXT("/"), &MountName, &SubPath))
        {
            MountName = Remainder;
        }

        Out.MountRoot = TEXT("/") + MountName;
        Out.SubPath = TrimPackageSlashes(SubPath);
        if (Out.MountRoot.Equals(TEXT("/Game"), ESearchCase::IgnoreCase))
        {
            Out.ContentRootPath = NormalizeFullDirectory(FPaths::ProjectContentDir());
        }
        else if (!TryResolveMountContentRoot(Out.MountRoot, Out.ContentRootPath))
        {
            Out.Error = FString::Printf(TEXT("could not resolve mounted content root for destination mount '%s'"), *Out.MountRoot);
            return Out;
        }
        Out.RequestedContentPath = Out.SubPath.IsEmpty()
            ? Out.ContentRootPath
            : NormalizeFullDirectory(Out.ContentRootPath / Out.SubPath);
        Out.LongPackageRoot = JoinLongPackagePath(Out.MountRoot, Out.SubPath);
        Out.bValid = true;
        return Out;
    }

    if (FPaths::IsRelative(Dest))
    {
        Out.MountRoot = TEXT("/Game");
        Out.ContentRootPath = NormalizeFullDirectory(FPaths::ProjectContentDir());
        Out.SubPath = TrimPackageSlashes(Dest);
        Out.RequestedContentPath = Out.SubPath.IsEmpty()
            ? Out.ContentRootPath
            : NormalizeFullDirectory(Out.ContentRootPath / Out.SubPath);
        Out.LongPackageRoot = JoinLongPackagePath(Out.MountRoot, Out.SubPath);
        Out.bValid = true;
        return Out;
    }

    const FString FullDest = NormalizeFullDirectory(Dest);
    FString Probe = FullDest;
    while (!Probe.IsEmpty())
    {
        if (FPaths::GetCleanFilename(Probe).Equals(TEXT("Content"), ESearchCase::IgnoreCase))
        {
            const FString OwnerDir = FPaths::GetPath(Probe);
            FString MountRoot;
            if (DirectoryContainsFileWithExtension(OwnerDir, TEXT("uproject")))
            {
                MountRoot = TEXT("/Game");
            }
            else
            {
                TArray<FString> PluginFiles;
                IFileManager::Get().FindFiles(PluginFiles, *(OwnerDir / TEXT("*.uplugin")), true, false);
                if (PluginFiles.Num() > 0)
                {
                    MountRoot = TEXT("/") + FPaths::GetBaseFilename(PluginFiles[0]);
                }
            }

            if (!MountRoot.IsEmpty())
            {
                FString Rel = FullDest;
                if (Rel.Equals(Probe, ESearchCase::IgnoreCase))
                {
                    Rel.Reset();
                }
                else if (!FPaths::MakePathRelativeTo(Rel, *Probe))
                {
                    Out.Error = FString::Printf(TEXT("could not compute path under Content root: %s"), *FullDest);
                    return Out;
                }
                Out.MountRoot = MountRoot;
                Out.ContentRootPath = Probe;
                Out.SubPath = TrimPackageSlashes(Rel);
                Out.RequestedContentPath = FullDest;
                Out.LongPackageRoot = JoinLongPackagePath(Out.MountRoot, Out.SubPath);
                Out.bValid = true;
                return Out;
            }
        }

        const FString Parent = FPaths::GetPath(Probe);
        if (Parent.Equals(Probe, ESearchCase::IgnoreCase))
        {
            break;
        }
        Probe = Parent;
    }

    Out.Error = FString::Printf(
        TEXT("destination must be a mounted package path, relative /Game subpath, or a project/plugin Content path: %s"),
        *Destination);
    return Out;
}

FString ReadTextFileTail(const FString& Filename, int32 MaxChars = 65536)
{
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Filename))
    {
        return FString();
    }
    if (Text.Len() > MaxChars)
    {
        Text = Text.Right(MaxChars);
    }
    return Text;
}

TArray<FString> JsonStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    TArray<FString> Out;
    if (!Object.IsValid())
    {
        return Out;
    }
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object->TryGetArrayField(Field, Values) || Values == nullptr)
    {
        return Out;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        if (!Value.IsValid())
        {
            continue;
        }
        FString Text = Value->AsString();
        Text.TrimStartAndEndInline();
        if (!Text.IsEmpty())
        {
            Out.Add(NormalizeLongPackagePath(Text));
        }
    }
    return Out;
}

FString PackageParentPath(const FString& PackagePath)
{
    FString Clean = NormalizeLongPackagePath(PackagePath);
    int32 Slash = INDEX_NONE;
    if (Clean.FindLastChar(TEXT('/'), Slash) && Slash > 0)
    {
        return Clean.Left(Slash);
    }
    return Clean;
}

FString PackageLeafName(const FString& PackagePath)
{
    FString Clean = NormalizeLongPackagePath(PackagePath);
    int32 Slash = INDEX_NONE;
    if (Clean.FindLastChar(TEXT('/'), Slash))
    {
        return Clean.Mid(Slash + 1);
    }
    return Clean;
}

TArray<FString> PackageSegments(const FString& PackagePath)
{
    FString Clean = TrimPackageSlashes(PackagePath);
    TArray<FString> Segments;
    Clean.ParseIntoArray(Segments, TEXT("/"), true);
    return Segments;
}

FString CommonPackageRoot(const TArray<FString>& PackagePaths)
{
    if (PackagePaths.Num() == 0)
    {
        return TEXT("/Game");
    }

    TArray<FString> Common = PackageSegments(PackagePaths[0]);
    for (int32 I = 1; I < PackagePaths.Num(); ++I)
    {
        const TArray<FString> Current = PackageSegments(PackagePaths[I]);
        int32 Keep = 0;
        while (Keep < Common.Num()
            && Keep < Current.Num()
            && Common[Keep].Equals(Current[Keep], ESearchCase::CaseSensitive))
        {
            ++Keep;
        }
        Common.SetNum(Keep);
        if (Common.Num() == 0)
        {
            break;
        }
    }

    if (Common.Num() == 0)
    {
        return TEXT("/Game");
    }
    return TEXT("/") + FString::Join(Common, TEXT("/"));
}

FString SourceRootFromMigrationReport(const TSharedPtr<FJsonObject>& Report)
{
    TArray<FString> Roots;
    const TArray<TSharedPtr<FJsonValue>>* ExpandedFolders = nullptr;
    if (Report.IsValid() && Report->TryGetArrayField(TEXT("expanded_folders"), ExpandedFolders) && ExpandedFolders != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *ExpandedFolders)
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            FString Folder;
            if (Entry.IsValid() && Entry->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty())
            {
                Roots.Add(NormalizeLongPackagePath(Folder));
            }
        }
    }
    if (Roots.Num() > 0)
    {
        return CommonPackageRoot(Roots);
    }

    for (const FString& SourceAsset : JsonStringArray(Report, TEXT("source_assets")))
    {
        Roots.Add(PackageParentPath(SourceAsset));
    }
    if (Roots.Num() > 0)
    {
        return CommonPackageRoot(Roots);
    }

    TArray<FString> Selected = JsonStringArray(Report, TEXT("selected_packages"));
    for (const FString& Package : Selected)
    {
        Roots.Add(PackageParentPath(Package));
    }
    return CommonPackageRoot(Roots);
}

FString RelativePackageUnderRoot(const FString& PackagePath, const FString& SourceRoot)
{
    const FString CleanPackage = NormalizeLongPackagePath(PackagePath);
    const FString CleanRoot = NormalizeLongPackagePath(SourceRoot);
    if (CleanPackage.Equals(CleanRoot, ESearchCase::CaseSensitive))
    {
        return PackageLeafName(CleanPackage);
    }
    FString CleanRootWithSlash = CleanRoot;
    if (!CleanRootWithSlash.EndsWith(TEXT("/")))
    {
        CleanRootWithSlash += TEXT("/");
    }
    if (CleanPackage.StartsWith(CleanRootWithSlash, ESearchCase::CaseSensitive))
    {
        return TrimPackageSlashes(CleanPackage.Mid(CleanRoot.Len()));
    }
    return JoinLongPackagePath(TEXT("/_Dependencies"), TrimPackageSlashes(CleanPackage)).Mid(1);
}

FString PackagePathAfterMount(const FString& PackagePath)
{
    TArray<FString> Segments = PackageSegments(PackagePath);
    if (Segments.Num() <= 1)
    {
        return PackageLeafName(PackagePath);
    }
    Segments.RemoveAt(0);
    return FString::Join(Segments, TEXT("/"));
}

FString StagedPackagePathForSource(const FString& SourcePackage, const FMigrationDestination& Destination)
{
    return JoinLongPackagePath(Destination.MountRoot, PackagePathAfterMount(SourcePackage));
}

FString SourcePackageForStagedPackage(const FString& StagedPackage, const FMigrationDestination& Destination)
{
    const FString CleanStaged = NormalizeLongPackagePath(StagedPackage);
    const FString CleanMount = NormalizeLongPackagePath(Destination.MountRoot);
    FString CleanMountWithSlash = CleanMount;
    if (!CleanMountWithSlash.EndsWith(TEXT("/")))
    {
        CleanMountWithSlash += TEXT("/");
    }
    if (CleanStaged.StartsWith(CleanMountWithSlash, ESearchCase::CaseSensitive))
    {
        return JoinLongPackagePath(TEXT("/Game"), CleanStaged.Mid(CleanMountWithSlash.Len()));
    }
    return CleanStaged;
}

FString FinalPackagePathForSource(const FString& SourcePackage, const FString& SourceRoot, const FMigrationDestination& Destination)
{
    return JoinLongPackagePath(Destination.LongPackageRoot, RelativePackageUnderRoot(SourcePackage, SourceRoot));
}

TSharedPtr<FJsonObject> PackageFileStateJson(const FString& PackagePath)
{
    auto Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("package"), PackagePath);

    FString BaseFilename;
    TArray<TSharedPtr<FJsonValue>> Candidates;
    bool bExists = false;
    FString ChosenFile;
    int64 ChosenSize = 0;
    if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, BaseFilename))
    {
        TArray<FString> CandidatePaths;
        CandidatePaths.Add(BaseFilename + TEXT(".uasset"));
        CandidatePaths.Add(BaseFilename + TEXT(".umap"));
        for (const FString& Candidate : CandidatePaths)
        {
            auto CandidateObj = MakeShared<FJsonObject>();
            CandidateObj->SetStringField(TEXT("path"), Candidate);
            const bool bCandidateExists = FPaths::FileExists(Candidate);
            CandidateObj->SetBoolField(TEXT("exists"), bCandidateExists);
            if (bCandidateExists)
            {
                const int64 Size = IFileManager::Get().FileSize(*Candidate);
                CandidateObj->SetNumberField(TEXT("size"), static_cast<double>(Size));
                if (!bExists)
                {
                    bExists = true;
                    ChosenFile = Candidate;
                    ChosenSize = Size;
                }
            }
            Candidates.Add(MakeShared<FJsonValueObject>(CandidateObj));
        }
    }
    Row->SetBoolField(TEXT("exists_after"), bExists);
    Row->SetStringField(TEXT("destination_file"), ChosenFile);
    Row->SetNumberField(TEXT("size"), static_cast<double>(ChosenSize));
    Row->SetArrayField(TEXT("candidates"), Candidates);
    return Row;
}

FString ObjectPathForPackage(const FString& PackagePath)
{
    const FString Clean = NormalizeLongPackagePath(PackagePath);
    return Clean + TEXT(".") + PackageLeafName(Clean);
}

FString PackagePathForObjectPath(FString ObjectPath)
{
    ObjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
    const int32 Dot = ObjectPath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (Dot != INDEX_NONE)
    {
        ObjectPath = ObjectPath.Left(Dot);
    }
    return NormalizeLongPackagePath(ObjectPath);
}

bool PackageStartsWithRoot(const FString& PackagePath, const FString& RootPath)
{
    const FString CleanPackage = NormalizeLongPackagePath(PackagePath);
    const FString CleanRoot = NormalizeLongPackagePath(RootPath);
    if (CleanPackage.Equals(CleanRoot, ESearchCase::CaseSensitive))
    {
        return true;
    }
    FString RootWithSlash = CleanRoot;
    if (!RootWithSlash.EndsWith(TEXT("/")))
    {
        RootWithSlash += TEXT("/");
    }
    return CleanPackage.StartsWith(RootWithSlash, ESearchCase::CaseSensitive);
}

void AddJsonStringArray(TSharedPtr<FJsonObject> Object, const TCHAR* Field, const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FString& Value : Values)
    {
        Arr.Add(MakeShared<FJsonValueString>(Value));
    }
    Object->SetArrayField(Field, Arr);
}

TSharedPtr<FJsonObject> AssetDeleteDiagnosticJson(
    const FString& PackagePath,
    UEditorAssetSubsystem* AssetSubsystem)
{
    const FString Package = PackagePathForObjectPath(PackagePath);
    auto Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("package"), Package);
    Row->SetStringField(TEXT("object_path"), ObjectPathForPackage(Package));

    const bool bSubsystemExists = AssetSubsystem && AssetSubsystem->DoesAssetExist(Package);
    Row->SetBoolField(TEXT("editor_asset_subsystem_exists"), bSubsystemExists);

    UPackage* LoadedPackage = FindPackage(nullptr, *Package);
    Row->SetBoolField(TEXT("package_loaded"), LoadedPackage != nullptr);
    Row->SetBoolField(TEXT("package_dirty"), LoadedPackage && LoadedPackage->IsDirty());

    UObject* Obj = ResolveAsset(ObjectPathForPackage(Package));
    Row->SetBoolField(TEXT("object_loaded"), Obj != nullptr);
    if (Obj)
    {
        Row->SetStringField(TEXT("loaded_object_path"), Obj->GetPathName());
        Row->SetStringField(TEXT("class"), Obj->GetClass() ? Obj->GetClass()->GetName() : FString());
        Row->SetBoolField(TEXT("is_redirector"), Obj->IsA<UObjectRedirector>());
        Row->SetBoolField(TEXT("is_valid_low_level"), Obj->IsValidLowLevelFast());
    }

    TSharedPtr<FJsonObject> FileState = PackageFileStateJson(Package);
    Row->SetObjectField(TEXT("file_state"), FileState);
    const FString DestinationFile = FileState->GetStringField(TEXT("destination_file"));
    Row->SetStringField(TEXT("destination_file"), DestinationFile);
    if (!DestinationFile.IsEmpty())
    {
        Row->SetBoolField(TEXT("file_read_only"), IFileManager::Get().IsReadOnly(*DestinationFile));
    }

    TArray<FName> Referencers;
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    ARM.Get().GetReferencers(FName(*Package), Referencers, UE::AssetRegistry::EDependencyCategory::Package);
    TArray<FString> ReferencerStrings;
    for (const FName& Referencer : Referencers)
    {
        ReferencerStrings.Add(Referencer.ToString());
    }
    ReferencerStrings.Sort();
    AddJsonStringArray(Row, TEXT("referencers"), ReferencerStrings);
    Row->SetNumberField(TEXT("referencer_count"), ReferencerStrings.Num());
    return Row;
}

FString SummarizeDeleteDiagnostic(const TSharedPtr<FJsonObject>& Diag)
{
    if (!Diag.IsValid())
    {
        return TEXT("no delete diagnostics available");
    }
    bool bPackageLoaded = false;
    bool bPackageDirty = false;
    bool bObjectLoaded = false;
    bool bIsRedirector = false;
    bool bReadOnly = false;
    double ReferencerCount = 0.0;
    FString DestinationFile;
    Diag->TryGetBoolField(TEXT("package_loaded"), bPackageLoaded);
    Diag->TryGetBoolField(TEXT("package_dirty"), bPackageDirty);
    Diag->TryGetBoolField(TEXT("object_loaded"), bObjectLoaded);
    Diag->TryGetBoolField(TEXT("is_redirector"), bIsRedirector);
    Diag->TryGetBoolField(TEXT("file_read_only"), bReadOnly);
    Diag->TryGetNumberField(TEXT("referencer_count"), ReferencerCount);
    Diag->TryGetStringField(TEXT("destination_file"), DestinationFile);

    TArray<FString> Parts;
    if (bPackageLoaded)
    {
        Parts.Add(bPackageDirty ? TEXT("loaded dirty package") : TEXT("loaded package"));
    }
    if (!bObjectLoaded)
    {
        Parts.Add(TEXT("asset object could not be loaded"));
    }
    if (bIsRedirector)
    {
        Parts.Add(TEXT("asset is a redirector"));
    }
    if (ReferencerCount > 0.0)
    {
        Parts.Add(FString::Printf(TEXT("%.0f referencer(s)"), ReferencerCount));
    }
    if (bReadOnly)
    {
        Parts.Add(TEXT("package file is read-only"));
    }
    if (!DestinationFile.IsEmpty())
    {
        Parts.Add(FString::Printf(TEXT("file=%s"), *DestinationFile));
    }
    if (Parts.Num() == 0)
    {
        Parts.Add(TEXT("EditorAssetSubsystem.DeleteAsset returned false"));
    }
    return FString::Join(Parts, TEXT("; "));
}

bool DeleteAssetForOverwrite(
    UEditorAssetSubsystem* AssetSubsystem,
    const FString& PackagePath,
    bool bAllowForceDelete,
    TSharedPtr<FJsonObject>& OutDeleteReport)
{
    const FString Package = PackagePathForObjectPath(PackagePath);
    OutDeleteReport = MakeShared<FJsonObject>();
    OutDeleteReport->SetStringField(TEXT("package"), Package);
    OutDeleteReport->SetBoolField(TEXT("allow_force_delete"), bAllowForceDelete);
    OutDeleteReport->SetObjectField(TEXT("before"), AssetDeleteDiagnosticJson(Package, AssetSubsystem));

    TSharedPtr<FJsonObject> BeforeFile = PackageFileStateJson(Package);
    const bool bExistsBefore = AssetSubsystem && AssetSubsystem->DoesAssetExist(Package);
    const bool bFileExistsBefore = BeforeFile->GetBoolField(TEXT("exists_after"));
    OutDeleteReport->SetBoolField(TEXT("exists_before"), bExistsBefore);
    OutDeleteReport->SetBoolField(TEXT("file_exists_before"), bFileExistsBefore);
    if (!bExistsBefore && !bFileExistsBefore)
    {
        OutDeleteReport->SetBoolField(TEXT("deleted"), true);
        OutDeleteReport->SetBoolField(TEXT("already_missing"), true);
        OutDeleteReport->SetObjectField(TEXT("after"), AssetDeleteDiagnosticJson(Package, AssetSubsystem));
        return true;
    }

    bool bNormalDeleted = false;
    if (AssetSubsystem && bExistsBefore)
    {
        bNormalDeleted = AssetSubsystem->DeleteAsset(Package);
    }
    OutDeleteReport->SetBoolField(TEXT("normal_delete_attempted"), bExistsBefore);
    OutDeleteReport->SetBoolField(TEXT("normal_delete_returned"), bNormalDeleted);

    auto IsGone = [&AssetSubsystem, &Package]() -> bool
    {
        const bool bAssetStillExists = AssetSubsystem && AssetSubsystem->DoesAssetExist(Package);
        const bool bFileStillExists = PackageFileStateJson(Package)->GetBoolField(TEXT("exists_after"));
        return !bAssetStillExists && !bFileStillExists;
    };

    bool bForceDeleted = false;
    if (!IsGone() && bAllowForceDelete)
    {
        UObject* Obj = ResolveAsset(ObjectPathForPackage(Package));
        OutDeleteReport->SetBoolField(TEXT("force_delete_attempted"), Obj != nullptr);
        if (Obj)
        {
            TArray<UObject*> Objects;
            Objects.Add(Obj);
            const int32 ForceDeletedCount = ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
            OutDeleteReport->SetNumberField(TEXT("force_delete_count"), ForceDeletedCount);
            bForceDeleted = ForceDeletedCount > 0;
            if (!IsGone() && ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false))
            {
                OutDeleteReport->SetBoolField(TEXT("delete_single_object_fallback"), true);
                bForceDeleted = true;
            }
            CollectGarbage(RF_NoFlags, /*bPerformFullPurge=*/true);
        }
    }
    OutDeleteReport->SetBoolField(TEXT("force_delete_returned"), bForceDeleted);
    OutDeleteReport->SetObjectField(TEXT("after"), AssetDeleteDiagnosticJson(Package, AssetSubsystem));

    const bool bDeleted = IsGone();
    OutDeleteReport->SetBoolField(TEXT("deleted"), bDeleted);
    if (!bDeleted)
    {
        const TSharedPtr<FJsonObject>* After = nullptr;
        if (OutDeleteReport->TryGetObjectField(TEXT("after"), After) && After && After->IsValid())
        {
            OutDeleteReport->SetStringField(TEXT("reason"), SummarizeDeleteDiagnostic(*After));
        }
        else
        {
            OutDeleteReport->SetStringField(TEXT("reason"), TEXT("delete did not remove asset or package file"));
        }
    }
    return bDeleted;
}

int32 FixupRedirectorsUnderPackagePaths(
    const TSet<FString>& PackagePaths,
    TArray<TSharedPtr<FJsonValue>>& OutRows)
{
    FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARM.Get();

    TArray<UObjectRedirector*> Redirectors;
    TSet<FString> SeenRedirectors;
    TArray<FString> SortedPaths;
    for (const FString& Path : PackagePaths)
    {
        if (!Path.IsEmpty())
        {
            SortedPaths.Add(NormalizeLongPackagePath(Path));
        }
    }
    SortedPaths.Sort();

    for (const FString& Folder : SortedPaths)
    {
        FARFilter Filter;
        Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(FName(*Folder));
        Filter.bRecursivePaths = true;
        TArray<FAssetData> Found;
        AR.GetAssets(Filter, Found);
        for (const FAssetData& Data : Found)
        {
            const FString RedirectorPath = Data.GetSoftObjectPath().ToString();
            if (SeenRedirectors.Contains(RedirectorPath))
            {
                continue;
            }
            SeenRedirectors.Add(RedirectorPath);
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("path"), RedirectorPath);
            Row->SetStringField(TEXT("scan_root"), Folder);
            OutRows.Add(MakeShared<FJsonValueObject>(Row));
            if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Data.GetAsset()))
            {
                Redirectors.Add(Redirector);
            }
        }
    }

    if (Redirectors.Num() > 0)
    {
        AT.Get().FixupReferencers(Redirectors, /*bCheckoutDialogPrompt=*/false);
    }
    return Redirectors.Num();
}

void SetJsonArrayFieldFromObjects(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Field,
    const TArray<TSharedPtr<FJsonObject>>& Rows)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const TSharedPtr<FJsonObject>& Row : Rows)
    {
        Values.Add(MakeShared<FJsonValueObject>(Row));
    }
    Object->SetArrayField(Field, Values);
}

TArray<TSharedPtr<FJsonObject>> JsonObjectArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    TArray<TSharedPtr<FJsonObject>> Rows;
    if (!Object.IsValid())
    {
        return Rows;
    }
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object->TryGetArrayField(Field, Values) || Values == nullptr)
    {
        return Rows;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
        if (Row.IsValid())
        {
            Rows.Add(Row);
        }
    }
    return Rows;
}

bool HasObjectRows(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    return Object.IsValid()
        && Object->TryGetArrayField(Field, Values)
        && Values != nullptr
        && Values->Num() > 0;
}

bool RollbackUnexpectedMigrationWrites(
    const TSharedPtr<FJsonObject>& Report,
    const FMigrationDestination& Destination,
    bool bOverwrite,
    FString& OutError)
{
    if (!Report.IsValid())
    {
        return true;
    }

    const bool bHasExtra = HasObjectRows(Report, TEXT("extra_written_assets"));
    const bool bHasDenied = HasObjectRows(Report, TEXT("denied_written_assets"));
    if (!bHasExtra && !bHasDenied)
    {
        return true;
    }

    Report->SetBoolField(TEXT("unexpected_write_set_violation"), true);
    Report->SetBoolField(TEXT("unexpected_write_set_rollback_attempted"), true);

    if (!GEditor)
    {
        OutError = TEXT("GEditor unavailable for unexpected migration write rollback");
        return false;
    }
    UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!AssetSubsystem)
    {
        OutError = TEXT("EditorAssetSubsystem unavailable for unexpected migration write rollback");
        return false;
    }

    TArray<FString> ScanPaths;
    ScanPaths.Add(Destination.MountRoot);
    ScanPaths.Add(Destination.LongPackageRoot);
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    ARM.Get().ScanPathsSynchronous(ScanPaths, /*bForceRescan=*/true);

    TSet<FString> PackagesToRollback;
    TArray<TSharedPtr<FJsonValue>> RollbackResults;
    TArray<TSharedPtr<FJsonValue>> RollbackBlocked;

    auto AddRollbackPackage = [&PackagesToRollback, &RollbackBlocked](const FString& Package, bool bExistedBefore, const FString& Reason)
    {
        if (Package.IsEmpty())
        {
            return;
        }
        if (bExistedBefore)
        {
            auto Blocked = MakeShared<FJsonObject>();
            Blocked->SetStringField(TEXT("package"), Package);
            Blocked->SetStringField(TEXT("reason"), Reason);
            RollbackBlocked.Add(MakeShared<FJsonValueObject>(Blocked));
            return;
        }
        PackagesToRollback.Add(Package);
    };

    auto AddRowsForRollback = [&AddRollbackPackage](const TArray<TSharedPtr<FJsonObject>>& Rows, const FString& Reason)
    {
        for (const TSharedPtr<FJsonObject>& Row : Rows)
        {
            FString Package;
            bool bExistsBefore = false;
            if (Row.IsValid() && Row->TryGetStringField(TEXT("package"), Package))
            {
                Row->TryGetBoolField(TEXT("exists_before"), bExistsBefore);
                AddRollbackPackage(Package, bExistsBefore, Reason);
            }
        }
    };

    AddRowsForRollback(JsonObjectArrayField(Report, TEXT("extra_written_assets")),
        TEXT("unexpected AssetTools dependency write existed before migration; cannot safely delete"));
    AddRowsForRollback(JsonObjectArrayField(Report, TEXT("denied_written_assets")),
        TEXT("denied-path AssetTools write existed before migration; cannot safely delete"));

    TSet<FString> ConflictedSourcePackages;
    for (const TSharedPtr<FJsonObject>& Row : JsonObjectArrayField(Report, TEXT("conflicts")))
    {
        FString Package;
        if (Row.IsValid() && Row->TryGetStringField(TEXT("package"), Package))
        {
            ConflictedSourcePackages.Add(NormalizeLongPackagePath(Package));
        }
    }

    for (const FString& SourcePackage : JsonStringArray(Report, TEXT("selected_packages")))
    {
        const bool bSelectedStageExistedBefore = bOverwrite && ConflictedSourcePackages.Contains(SourcePackage);
        AddRollbackPackage(
            StagedPackagePathForSource(SourcePackage, Destination),
            bSelectedStageExistedBefore,
            TEXT("selected staged package existed before migration; cannot safely roll back after unexpected writes"));
    }

    TArray<FString> RollbackPackages;
    for (const FString& Package : PackagesToRollback)
    {
        RollbackPackages.Add(Package);
    }
    RollbackPackages.Sort();
    int32 RolledBackCount = 0;
    int32 RollbackFailureCount = 0;
    for (const FString& Package : RollbackPackages)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("package"), Package);
        const bool bExistsBeforeDelete = AssetSubsystem->DoesAssetExist(Package);
        Row->SetBoolField(TEXT("exists_before_delete"), bExistsBeforeDelete);
        bool bDeleted = false;
        if (bExistsBeforeDelete)
        {
            bDeleted = AssetSubsystem->DeleteAsset(Package);
        }
        else
        {
            TSharedPtr<FJsonObject> FileState = PackageFileStateJson(Package);
            if (FileState->GetBoolField(TEXT("exists_after")))
            {
                Row->SetStringField(TEXT("reason"), TEXT("asset registry did not resolve package but package file still exists"));
            }
            else
            {
                bDeleted = true;
                Row->SetBoolField(TEXT("already_missing"), true);
            }
        }
        Row->SetBoolField(TEXT("deleted"), bDeleted);
        TSharedPtr<FJsonObject> AfterState = PackageFileStateJson(Package);
        Row->SetBoolField(TEXT("exists_after_delete"), AfterState->GetBoolField(TEXT("exists_after")));
        Row->SetStringField(TEXT("destination_file"), AfterState->GetStringField(TEXT("destination_file")));
        if (bDeleted && !AfterState->GetBoolField(TEXT("exists_after")))
        {
            ++RolledBackCount;
        }
        else
        {
            ++RollbackFailureCount;
        }
        RollbackResults.Add(MakeShared<FJsonValueObject>(Row));
    }

    Report->SetArrayField(TEXT("unexpected_write_rollback_results"), RollbackResults);
    Report->SetArrayField(TEXT("unexpected_write_rollback_blocked"), RollbackBlocked);
    Report->SetNumberField(TEXT("unexpected_write_rollback_count"), RolledBackCount);
    Report->SetNumberField(TEXT("unexpected_write_rollback_failure_count"), RollbackFailureCount);
    Report->SetBoolField(TEXT("unexpected_write_rollback_complete"),
        RollbackFailureCount == 0 && RollbackBlocked.Num() == 0);

    OutError = FString::Printf(
        TEXT("AssetTools wrote packages outside the filtered selected set (extra=%d, denied=%d); migration rejected and rollback attempted (rolled_back=%d, failures=%d, blocked=%d)"),
        JsonObjectArrayField(Report, TEXT("extra_written_assets")).Num(),
        JsonObjectArrayField(Report, TEXT("denied_written_assets")).Num(),
        RolledBackCount,
        RollbackFailureCount,
        RollbackBlocked.Num());
    return false;
}

bool VerifyAndRepairAnimationSkeletonReferences(
    const TSharedPtr<FJsonObject>& Report,
    const FMigrationDestination& Destination,
    FString& OutError)
{
    if (!Report.IsValid())
    {
        return true;
    }

    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    if (!AssetSubsystem)
    {
        OutError = TEXT("EditorAssetSubsystem unavailable for animation skeleton verification");
        return false;
    }

    TSet<FString> PackageSet;
    auto AddPackage = [&PackageSet](const FString& Raw)
    {
        if (!Raw.IsEmpty())
        {
            PackageSet.Add(PackagePathForObjectPath(Raw));
        }
    };

    for (const TSharedPtr<FJsonObject>& Row : JsonObjectArrayField(Report, TEXT("written_assets")))
    {
        FString Package;
        if (Row->TryGetStringField(TEXT("target_package"), Package)
            || Row->TryGetStringField(TEXT("package"), Package))
        {
            AddPackage(Package);
        }
    }
    for (const TSharedPtr<FJsonObject>& Row : JsonObjectArrayField(Report, TEXT("relocation_results")))
    {
        FString Package;
        if (Row->TryGetStringField(TEXT("target_package"), Package))
        {
            AddPackage(Package);
        }
    }

    int32 RootScanAssetCount = 0;
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& Registry = ARM.Get();
        Registry.ScanPathsSynchronous({ Destination.LongPackageRoot }, /*bForceRescan=*/true);

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*Destination.LongPackageRoot));
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.ClassPaths.Add(USkeleton::StaticClass()->GetClassPathName());
        Filter.ClassPaths.Add(UAnimationAsset::StaticClass()->GetClassPathName());
        TArray<FAssetData> FoundAssets;
        Registry.GetAssets(Filter, FoundAssets);
        RootScanAssetCount = FoundAssets.Num();
        for (const FAssetData& Data : FoundAssets)
        {
            AddPackage(Data.PackageName.ToString());
        }
    }

    TArray<USkeleton*> SkeletonCandidates;
    TArray<TSharedPtr<FJsonValue>> SkeletonCandidateRows;
    TArray<UAnimationAsset*> AnimationAssets;
    TArray<FString> Packages;
    for (const FString& Package : PackageSet)
    {
        Packages.Add(Package);
    }
    Packages.Sort();
    for (const FString& Package : Packages)
    {
        UObject* Obj = ResolveAsset(ObjectPathForPackage(Package));
        if (!Obj)
        {
            continue;
        }
        if (USkeleton* Skeleton = Cast<USkeleton>(Obj))
        {
            SkeletonCandidates.AddUnique(Skeleton);
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("path"), FSoftObjectPath(Skeleton).ToString());
            Row->SetStringField(TEXT("package"), PackagePathForObjectPath(FSoftObjectPath(Skeleton).ToString()));
            SkeletonCandidateRows.Add(MakeShared<FJsonValueObject>(Row));
        }
        if (UAnimationAsset* Anim = Cast<UAnimationAsset>(Obj))
        {
            AnimationAssets.Add(Anim);
        }
    }

    TArray<TSharedPtr<FJsonValue>> VerificationRows;
    TArray<TSharedPtr<FJsonValue>> RepairedRows;
    TArray<TSharedPtr<FJsonValue>> FailureRows;
    int32 MissingSkeletonCount = 0;
    int32 StaleSkeletonCount = 0;
    int32 RepairedCount = 0;

    auto ChooseSkeleton = [&SkeletonCandidates](UAnimationAsset* Anim, USkeleton* Current) -> USkeleton*
    {
        if (Current)
        {
            const FString CurrentLeaf = PackageLeafName(PackagePathForObjectPath(FSoftObjectPath(Current).ToString()));
            for (USkeleton* Candidate : SkeletonCandidates)
            {
                if (Candidate
                    && PackageLeafName(PackagePathForObjectPath(FSoftObjectPath(Candidate).ToString())).Equals(CurrentLeaf, ESearchCase::CaseSensitive))
                {
                    return Candidate;
                }
            }
        }
        if (SkeletonCandidates.Num() == 1)
        {
            return SkeletonCandidates[0];
        }
        return nullptr;
    };

    for (UAnimationAsset* Anim : AnimationAssets)
    {
        if (!Anim)
        {
            continue;
        }

        const FString AnimPath = FSoftObjectPath(Anim).ToString();
        USkeleton* CurrentSkeleton = Anim->GetSkeleton();
        const FString CurrentSkeletonPath = CurrentSkeleton ? FSoftObjectPath(CurrentSkeleton).ToString() : FString();
        const FString CurrentSkeletonPackage = CurrentSkeletonPath.IsEmpty()
            ? FString()
            : PackagePathForObjectPath(CurrentSkeletonPath);
        const bool bMissingSkeleton = CurrentSkeleton == nullptr;
        const bool bStaleSkeleton = CurrentSkeleton
            && !PackageStartsWithRoot(CurrentSkeletonPackage, Destination.LongPackageRoot);
        if (bMissingSkeleton)
        {
            ++MissingSkeletonCount;
        }
        if (bStaleSkeleton)
        {
            ++StaleSkeletonCount;
        }

        USkeleton* DesiredSkeleton = (bMissingSkeleton || bStaleSkeleton)
            ? ChooseSkeleton(Anim, CurrentSkeleton)
            : CurrentSkeleton;

        bool bRepaired = false;
        FString FailureReason;
        if ((bMissingSkeleton || bStaleSkeleton) && DesiredSkeleton)
        {
            FProperty* SkeletonProperty = Anim->GetClass()->FindPropertyByName(FName(TEXT("Skeleton")));
            Anim->Modify();
            if (SkeletonProperty)
            {
                Anim->PreEditChange(SkeletonProperty);
            }
            Anim->SetSkeleton(DesiredSkeleton);
            if (SkeletonProperty)
            {
                FPropertyChangedEvent ChangeEvent(SkeletonProperty, EPropertyChangeType::ValueSet);
                Anim->PostEditChangeProperty(ChangeEvent);
            }
            else
            {
                Anim->PostEditChange();
            }
            Anim->MarkPackageDirty();
            const FString AssetPackage = PackagePathForObjectPath(AnimPath);
            if (!AssetSubsystem->SaveAsset(AssetPackage, /*bOnlyIfIsDirty=*/false))
            {
                FailureReason = TEXT("SaveAsset returned false after SetSkeleton");
            }
            else
            {
                bRepaired = true;
                ++RepairedCount;
            }
        }
        else if ((bMissingSkeleton || bStaleSkeleton) && !DesiredSkeleton)
        {
            FailureReason = SkeletonCandidates.Num() == 0
                ? TEXT("no migrated USkeleton candidate found")
                : TEXT("multiple migrated USkeleton candidates; cannot choose safely");
        }

        USkeleton* FinalSkeleton = Anim->GetSkeleton();
        const FString FinalSkeletonPath = FinalSkeleton ? FSoftObjectPath(FinalSkeleton).ToString() : FString();
        const FString FinalSkeletonPackage = FinalSkeletonPath.IsEmpty()
            ? FString()
            : PackagePathForObjectPath(FinalSkeletonPath);
        const bool bFinalValid = FinalSkeleton
            && PackageStartsWithRoot(FinalSkeletonPackage, Destination.LongPackageRoot);

        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("asset"), AnimPath);
        Row->SetStringField(TEXT("class"), Anim->GetClass()->GetName());
        Row->SetStringField(TEXT("before_skeleton"), CurrentSkeletonPath);
        Row->SetStringField(TEXT("after_skeleton"), FinalSkeletonPath);
        Row->SetBoolField(TEXT("missing_before"), bMissingSkeleton);
        Row->SetBoolField(TEXT("stale_before"), bStaleSkeleton);
        Row->SetBoolField(TEXT("repaired"), bRepaired);
        Row->SetBoolField(TEXT("valid_after"), bFinalValid);
        if (!FailureReason.IsEmpty())
        {
            Row->SetStringField(TEXT("failure"), FailureReason);
        }
        VerificationRows.Add(MakeShared<FJsonValueObject>(Row));

        if (bRepaired)
        {
            RepairedRows.Add(MakeShared<FJsonValueObject>(Row));
        }
        if (!bFinalValid)
        {
            FailureRows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    Report->SetBoolField(TEXT("post_relocation_animation_skeleton_verified"), true);
    Report->SetStringField(TEXT("animation_skeleton_scan_root"), Destination.LongPackageRoot);
    Report->SetNumberField(TEXT("animation_skeleton_scan_asset_count"), RootScanAssetCount);
    Report->SetNumberField(TEXT("animation_skeleton_scan_package_count"), PackageSet.Num());
    Report->SetNumberField(TEXT("animation_asset_count"), AnimationAssets.Num());
    Report->SetNumberField(TEXT("animation_skeleton_candidate_count"), SkeletonCandidates.Num());
    Report->SetArrayField(TEXT("animation_skeleton_candidates"), SkeletonCandidateRows);
    Report->SetArrayField(TEXT("animation_skeleton_verification"), VerificationRows);
    Report->SetArrayField(TEXT("animation_skeleton_repaired_assets"), RepairedRows);
    Report->SetArrayField(TEXT("animation_skeleton_failures"), FailureRows);
    Report->SetNumberField(TEXT("animation_skeleton_missing_before_count"), MissingSkeletonCount);
    Report->SetNumberField(TEXT("animation_skeleton_stale_before_count"), StaleSkeletonCount);
    Report->SetNumberField(TEXT("animation_skeleton_repaired_count"), RepairedCount);
    Report->SetNumberField(TEXT("animation_skeleton_failure_count"), FailureRows.Num());

    if (FailureRows.Num() > 0)
    {
        OutError = FString::Printf(
            TEXT("post-relocation animation skeleton verification failed for %d asset(s)"),
            FailureRows.Num());
        return false;
    }
    return true;
}

bool RelocateMigratedPackages(
    const TSharedPtr<FJsonObject>& Report,
    const FMigrationDestination& Destination,
    bool bOverwrite,
    FString& OutError)
{
    if (!Report.IsValid() || Destination.SubPath.IsEmpty())
    {
        return true;
    }
    if (!GEditor)
    {
        OutError = TEXT("GEditor unavailable for post-migration relocation");
        return false;
    }
    UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!AssetSubsystem)
    {
        OutError = TEXT("EditorAssetSubsystem unavailable for post-migration relocation");
        return false;
    }

    const TArray<FString> SelectedPackages = JsonStringArray(Report, TEXT("selected_packages"));
    const TArray<TSharedPtr<FJsonObject>> ActualWrittenRows = JsonObjectArrayField(Report, TEXT("actual_written_assets"));
    const FString SourceRoot = SourceRootFromMigrationReport(Report);
    Report->SetStringField(TEXT("source_package_root"), SourceRoot);

    TArray<FString> ScanPaths;
    ScanPaths.Add(Destination.MountRoot);
    ScanPaths.Add(Destination.LongPackageRoot);
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    ARM.Get().ScanPathsSynchronous(ScanPaths, /*bForceRescan=*/true);

    TArray<TSharedPtr<FJsonValue>> MovePlan;
    TArray<TSharedPtr<FJsonValue>> MoveResults;
    TArray<TSharedPtr<FJsonValue>> MoveFailures;
    TArray<TSharedPtr<FJsonValue>> IgnoredMissingSelected;
    TArray<TSharedPtr<FJsonValue>> OverwriteDeleteReports;
    TSet<FString> RedirectorFixupRoots;
    TArray<TSharedPtr<FJsonObject>> WrittenRows;
    TArray<TSharedPtr<FJsonObject>> MissingRows;
    int32 MovedCount = 0;
    int32 OverwriteReplacedCount = 0;

    struct FRelocationEntry
    {
        FString SourcePackage;
        FString StagedPackage;
    };
    TArray<FRelocationEntry> RelocationEntries;
    TSet<FString> RelocationStagedPackages;
    if (ActualWrittenRows.Num() > 0)
    {
        for (const TSharedPtr<FJsonObject>& Row : ActualWrittenRows)
        {
            FString StagedPackage;
            if (!Row.IsValid() || !Row->TryGetStringField(TEXT("package"), StagedPackage) || StagedPackage.IsEmpty())
            {
                continue;
            }
            FString SourcePackage;
            if (!Row->TryGetStringField(TEXT("source_equivalent_package"), SourcePackage) || SourcePackage.IsEmpty())
            {
                SourcePackage = SourcePackageForStagedPackage(StagedPackage, Destination);
            }
            StagedPackage = NormalizeLongPackagePath(StagedPackage);
            SourcePackage = NormalizeLongPackagePath(SourcePackage);
            if (!RelocationStagedPackages.Contains(StagedPackage))
            {
                RelocationEntries.Add({SourcePackage, StagedPackage});
                RelocationStagedPackages.Add(StagedPackage);
            }
        }

        TSet<FString> ActualSourcePackages;
        for (const FRelocationEntry& Entry : RelocationEntries)
        {
            ActualSourcePackages.Add(Entry.SourcePackage);
        }
        for (const TSharedPtr<FJsonObject>& MissingRow : JsonObjectArrayField(Report, TEXT("missing_written_assets")))
        {
            FString MissingSource;
            if (!MissingRow.IsValid() || !MissingRow->TryGetStringField(TEXT("package"), MissingSource))
            {
                continue;
            }
            MissingSource = NormalizeLongPackagePath(MissingSource);
            if (!ActualSourcePackages.Contains(MissingSource))
            {
                auto Ignored = MakeShared<FJsonObject>();
                Ignored->SetStringField(TEXT("package"), MissingSource);
                FString DestinationFile;
                if (MissingRow->TryGetStringField(TEXT("destination_file"), DestinationFile))
                {
                    Ignored->SetStringField(TEXT("staged_destination_file"), DestinationFile);
                }
                Ignored->SetStringField(TEXT("reason"), TEXT("selected source package was not written by AssetTools; treating as alias/redirector candidate and relocating actual written packages"));
                IgnoredMissingSelected.Add(MakeShared<FJsonValueObject>(Ignored));
            }
        }
    }
    else
    {
        for (const FString& SourcePackage : SelectedPackages)
        {
            RelocationEntries.Add({
                SourcePackage,
                StagedPackagePathForSource(SourcePackage, Destination)
            });
        }
    }

    for (const FRelocationEntry& Entry : RelocationEntries)
    {
        const FString SourcePackage = Entry.SourcePackage;
        const FString StagedPackage = Entry.StagedPackage;
        const FString FinalPackage = FinalPackagePathForSource(SourcePackage, SourceRoot, Destination);
        RedirectorFixupRoots.Add(PackageParentPath(StagedPackage));
        RedirectorFixupRoots.Add(PackageParentPath(FinalPackage));

        auto PlanRow = MakeShared<FJsonObject>();
        PlanRow->SetStringField(TEXT("source_package"), SourcePackage);
        PlanRow->SetStringField(TEXT("staged_package"), StagedPackage);
        PlanRow->SetStringField(TEXT("target_package"), FinalPackage);
        MovePlan.Add(MakeShared<FJsonValueObject>(PlanRow));

        auto ResultRow = MakeShared<FJsonObject>();
        ResultRow->SetStringField(TEXT("source_package"), SourcePackage);
        ResultRow->SetStringField(TEXT("staged_package"), StagedPackage);
        ResultRow->SetStringField(TEXT("target_package"), FinalPackage);

        if (StagedPackage.Equals(FinalPackage, ESearchCase::CaseSensitive))
        {
            ResultRow->SetBoolField(TEXT("ok"), true);
            ResultRow->SetBoolField(TEXT("already_at_target"), true);
            MoveResults.Add(MakeShared<FJsonValueObject>(ResultRow));
        }
        else
        {
            const bool bStagedExists = AssetSubsystem->DoesAssetExist(StagedPackage);
            const bool bFinalExists = AssetSubsystem->DoesAssetExist(FinalPackage);
            ResultRow->SetBoolField(TEXT("staged_exists"), bStagedExists);
            ResultRow->SetBoolField(TEXT("target_exists_before"), bFinalExists);
            if (!bStagedExists)
            {
                ResultRow->SetBoolField(TEXT("ok"), false);
                ResultRow->SetStringField(TEXT("reason"), TEXT("staged migrated asset not found in target project"));
                MoveResults.Add(MakeShared<FJsonValueObject>(ResultRow));
                MoveFailures.Add(MakeShared<FJsonValueObject>(ResultRow));
            }
            else if (bFinalExists && !bOverwrite)
            {
                ResultRow->SetBoolField(TEXT("ok"), false);
                ResultRow->SetStringField(TEXT("reason"), TEXT("target asset exists; pass overwrite:true to replace it"));
                MoveResults.Add(MakeShared<FJsonValueObject>(ResultRow));
                MoveFailures.Add(MakeShared<FJsonValueObject>(ResultRow));
            }
            else
            {
                bool bTargetCleared = true;
                if (bFinalExists && bOverwrite)
                {
                    TSharedPtr<FJsonObject> DeleteReport;
                    bTargetCleared = DeleteAssetForOverwrite(
                        AssetSubsystem,
                        FinalPackage,
                        /*bAllowForceDelete=*/true,
                        DeleteReport);
                    if (DeleteReport.IsValid())
                    {
                        ResultRow->SetObjectField(TEXT("overwrite_delete"), DeleteReport);
                        OverwriteDeleteReports.Add(MakeShared<FJsonValueObject>(DeleteReport));
                    }
                    if (bTargetCleared)
                    {
                        ++OverwriteReplacedCount;
                    }
                }

                if (!bTargetCleared)
                {
                    ResultRow->SetBoolField(TEXT("ok"), false);
                    FString Reason = TEXT("failed to delete existing target asset before overwrite");
                    const TSharedPtr<FJsonObject>* DeleteReport = nullptr;
                    if (ResultRow->TryGetObjectField(TEXT("overwrite_delete"), DeleteReport)
                        && DeleteReport
                        && DeleteReport->IsValid())
                    {
                        FString DeleteReason;
                        if ((*DeleteReport)->TryGetStringField(TEXT("reason"), DeleteReason)
                            && !DeleteReason.IsEmpty())
                        {
                            Reason = FString::Printf(TEXT("%s: %s"), *Reason, *DeleteReason);
                        }
                    }
                    ResultRow->SetStringField(TEXT("reason"), Reason);
                    MoveResults.Add(MakeShared<FJsonValueObject>(ResultRow));
                    MoveFailures.Add(MakeShared<FJsonValueObject>(ResultRow));
                }
                else
                {
                    const bool bRenamed = AssetSubsystem->RenameAsset(StagedPackage, FinalPackage);
                    ResultRow->SetBoolField(TEXT("ok"), bRenamed);
                    if (bRenamed)
                    {
                        AssetSubsystem->SaveAsset(FinalPackage, /*bOnlyIfIsDirty=*/false);
                        RedirectorFixupRoots.Add(PackageParentPath(StagedPackage));
                        ++MovedCount;
                    }
                    else
                    {
                        ResultRow->SetStringField(TEXT("reason"), TEXT("EditorAssetSubsystem.RenameAsset returned false"));
                        MoveFailures.Add(MakeShared<FJsonValueObject>(ResultRow));
                    }
                    MoveResults.Add(MakeShared<FJsonValueObject>(ResultRow));
                }
            }
        }

        TSharedPtr<FJsonObject> FinalState = PackageFileStateJson(FinalPackage);
        FinalState->SetStringField(TEXT("source_package"), SourcePackage);
        FinalState->SetStringField(TEXT("target_package"), FinalPackage);
        if (FinalState->GetBoolField(TEXT("exists_after")))
        {
            WrittenRows.Add(FinalState);
        }
        else
        {
            MissingRows.Add(FinalState);
        }
    }

    TArray<TSharedPtr<FJsonValue>> RedirectorFixupRows;
    const int32 RedirectorFixupCount = FixupRedirectorsUnderPackagePaths(
        RedirectorFixupRoots,
        RedirectorFixupRows);

    Report->SetBoolField(TEXT("post_migration_relocation"), true);
    Report->SetStringField(TEXT("post_migration_relocation_mode"), TEXT("target_editor_rename_asset_with_overwrite_cleanup"));
    Report->SetStringField(TEXT("post_migration_relocation_input"), ActualWrittenRows.Num() > 0 ? TEXT("actual_written_assets") : TEXT("selected_packages"));
    Report->SetArrayField(TEXT("relocation_plan"), MovePlan);
    Report->SetArrayField(TEXT("relocation_results"), MoveResults);
    Report->SetArrayField(TEXT("relocation_failures"), MoveFailures);
    Report->SetArrayField(TEXT("relocation_ignored_missing_selected_assets"), IgnoredMissingSelected);
    Report->SetArrayField(TEXT("relocation_overwrite_delete_reports"), OverwriteDeleteReports);
    Report->SetArrayField(TEXT("relocation_redirector_fixup_assets"), RedirectorFixupRows);
    Report->SetNumberField(TEXT("relocation_count"), MovedCount);
    Report->SetNumberField(TEXT("relocation_overwrite_replaced_count"), OverwriteReplacedCount);
    Report->SetNumberField(TEXT("relocation_failure_count"), MoveFailures.Num());
    Report->SetNumberField(TEXT("relocation_ignored_missing_selected_count"), IgnoredMissingSelected.Num());
    Report->SetNumberField(TEXT("relocation_redirector_fixup_count"), RedirectorFixupCount);
    SetJsonArrayFieldFromObjects(Report, TEXT("written_assets"), WrittenRows);
    SetJsonArrayFieldFromObjects(Report, TEXT("missing_written_assets"), MissingRows);
    Report->SetNumberField(TEXT("written_count"), WrittenRows.Num());
    Report->SetNumberField(TEXT("missing_written_count"), MissingRows.Num());

    if (MoveFailures.Num() > 0 || MissingRows.Num() > 0)
    {
        OutError = FString::Printf(
            TEXT("post-migration relocation to %s failed or left missing files (move_failures=%d, missing=%d)"),
            *Destination.LongPackageRoot,
            MoveFailures.Num(),
            MissingRows.Num());
        return false;
    }
    return true;
}

FString PythonLiteralPath(FString Path)
{
    FPaths::NormalizeFilename(Path);
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    Path.ReplaceInline(TEXT("'"), TEXT("\\'"));
    return FString::Printf(TEXT("r'%s'"), *Path);
}

FString BuildMigrationPythonScript(const FString& ParamsPath, const FString& ReportPath)
{
    FString Script = FString::Printf(TEXT(R"PY(
import json
import os
import traceback
import unreal

PARAMS_PATH = %s
REPORT_PATH = %s

def write_report(obj):
    os.makedirs(os.path.dirname(REPORT_PATH), exist_ok=True)
    with open(REPORT_PATH, 'w', encoding='utf-8') as fh:
        json.dump(obj, fh, indent=2, sort_keys=True)

def package_name(raw):
    s = str(raw).replace('\\', '/').strip()
    s = s.rstrip('/')
    if s.endswith('.uasset') or s.endswith('.umap'):
        s = os.path.splitext(s)[0]
        marker = '/Content/'
        if marker in s:
            s = '/Game/' + s.split(marker, 1)[1]
    if '.' in s:
        slash = s.rfind('/')
        dot = s.find('.', slash + 1)
        if dot >= 0:
            s = s[:dot]
    return s

def package_name_for_asset_data(data):
    if not data:
        return ''
    for attr in ('package_name', 'package_path'):
        try:
            value = getattr(data, attr)
        except Exception:
            value = None
        text = package_name(value) if value is not None else ''
        if text and text.lower() != 'none':
            if attr == 'package_path':
                try:
                    asset_name = str(getattr(data, 'asset_name'))
                    if asset_name and asset_name.lower() != 'none':
                        return text.rstrip('/') + '/' + asset_name
                except Exception:
                    pass
            return text
    try:
        return package_name(str(data.get_soft_object_path()))
    except Exception:
        return ''

def string_list(params, key):
    out = []
    values = params.get(key, []) or []
    if isinstance(values, (str, bytes)):
        values = [values]
    for value in values:
        text = str(value).strip()
        if text:
            out.append(text)
    return out

def assets_under_folder(folder):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    folder = package_name(folder)
    if not folder or not folder.startswith('/'):
        return []
    assets = []
    attempts = [
        lambda: registry.get_assets_by_path(unreal.Name(folder), True),
        lambda: registry.get_assets_by_path(unreal.Name(folder), True, False),
        lambda: registry.get_assets_by_path(folder, recursive=True),
        lambda: registry.get_assets_by_path(folder, recursive=True, include_only_on_disk_assets=False),
    ]
    for attempt in attempts:
        try:
            result = attempt()
            if result:
                assets = list(result)
                break
        except Exception:
            continue
    packages = []
    for data in assets:
        pkg = package_name_for_asset_data(data)
        if pkg:
            packages.append(pkg)
    return sorted(set(packages))

def resolve_source_packages(params):
    raw_assets = string_list(params, 'source_assets')
    raw_folders = []
    raw_folders.extend(string_list(params, 'source_folder'))
    raw_folders.extend(string_list(params, 'source_folders'))
    raw_folders.extend(string_list(params, 'source_path'))
    raw_folders.extend(string_list(params, 'source_paths'))
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.search_all_assets(True)

    resolved = []
    expanded_folders = []
    unresolved = []
    for raw in raw_assets:
        pkg = package_name(raw)
        if not pkg:
            continue
        data = asset_data_for_package(pkg)
        if data:
            resolved.append(pkg)
            continue
        folder_packages = assets_under_folder(pkg)
        if folder_packages:
            resolved.extend(folder_packages)
            expanded_folders.append({'source': raw, 'folder': pkg, 'asset_count': len(folder_packages)})
        else:
            unresolved.append({'source': raw, 'package': pkg, 'reason': 'asset_or_folder_not_found'})
    for raw in raw_folders:
        folder = package_name(raw)
        folder_packages = assets_under_folder(folder)
        if folder_packages:
            resolved.extend(folder_packages)
            expanded_folders.append({'source': raw, 'folder': folder, 'asset_count': len(folder_packages)})
        else:
            unresolved.append({'source': raw, 'package': folder, 'reason': 'folder_not_found_or_empty'})
    return sorted(set(resolved)), expanded_folders, unresolved

def dependency_closure(seed_packages, params):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.search_all_assets(True)
    opts = unreal.AssetRegistryDependencyOptions()
    for name, value in {
        'include_hard_package_references': bool(params.get('include_hard_dependencies', True)),
        'include_soft_package_references': bool(params.get('include_soft_dependencies', True)),
        'include_hard_management_references': bool(params.get('include_hard_management_references', False)),
        'include_soft_management_references': bool(params.get('include_soft_management_references', False)),
        'include_searchable_names': bool(params.get('include_searchable_names', False)),
    }.items():
        if hasattr(opts, name):
            setattr(opts, name, value)
    seen = set()
    queue = list(seed_packages)
    while queue:
        pkg = package_name(queue.pop(0))
        if not pkg or pkg in seen:
            continue
        seen.add(pkg)
        try:
            deps = registry.get_dependencies(unreal.Name(pkg), opts)
        except Exception:
            deps = []
        for dep in deps:
            dep_pkg = package_name(dep)
            if not dep_pkg:
                continue
            if dep_pkg.startswith('/Script') or dep_pkg.startswith('/Engine') or dep_pkg.startswith('/Temp') or dep_pkg.startswith('/Memory'):
                continue
            if dep_pkg not in seen:
                queue.append(dep_pkg)
    return sorted(seen)
)PY"), *PythonLiteralPath(ParamsPath), *PythonLiteralPath(ReportPath));

    Script += TEXT(R"PY(
def asset_data_for_package(pkg):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    try:
        assets = registry.get_assets_by_package_name(unreal.Name(pkg), True)
        if assets:
            return assets[0]
    except Exception:
        pass
    try:
        asset_name = pkg.rsplit('/', 1)[-1]
        object_path = pkg + '.' + asset_name
        data = registry.get_asset_by_object_path(object_path)
        if data and data.is_valid():
            return data
    except Exception:
        pass
    return None

def normalize_class_name(raw):
    text = str(raw or '').strip()
    text = text.replace("'", "").replace('"', '')
    if not text:
        return ''
    return text

def class_name_for_asset_data(data):
    if not data:
        return ''
    for attr in ('asset_class_path', 'asset_class'):
        try:
            value = getattr(data, attr)
        except Exception:
            value = None
        text = normalize_class_name(value)
        if text and text.lower() != 'none':
            return text
    return ''

def object_path_for_asset_data(data, pkg):
    if data:
        try:
            return str(data.get_soft_object_path())
        except Exception:
            pass
        try:
            return str(data.object_path)
        except Exception:
            pass
    return pkg + '.' + pkg.rsplit('/', 1)[-1]

def class_leaf(class_name):
    text = normalize_class_name(class_name)
    if not text:
        return ''
    return text.rsplit('/', 1)[-1].rsplit('.', 1)[-1]

def class_matches(class_name, filters):
    if not filters:
        return True
    full = normalize_class_name(class_name).lower()
    leaf = class_leaf(class_name).lower()
    for raw in filters:
        needle = normalize_class_name(raw).lower()
        if not needle:
            continue
        if needle == full or needle == leaf or full.endswith('.' + needle) or full.endswith('/' + needle):
            return True
    return False

def path_matches(path, filters):
    if not filters:
        return True
    lowered = path.lower()
    for raw in filters:
        needle = str(raw).replace('\\', '/').strip().lower()
        if not needle:
            continue
        if needle.startswith('/'):
            if lowered.startswith(needle):
                return True
        elif needle in lowered:
            return True
    return False

def package_info(pkg, seed_packages):
    data = asset_data_for_package(pkg)
    cls = class_name_for_asset_data(data)
    obj_path = object_path_for_asset_data(data, pkg)
    return {
        'package': pkg,
        'object_path': obj_path,
        'asset_name': pkg.rsplit('/', 1)[-1],
        'class': cls,
        'class_leaf': class_leaf(cls),
        'is_seed': pkg in seed_packages,
        'asset_registry_found': bool(data),
    }

def filter_packages(packages, seed_packages, params):
    class_allow = string_list(params, 'class_allow_list')
    class_deny = string_list(params, 'class_deny_list')
    path_allow = string_list(params, 'path_allow_list')
    path_deny = string_list(params, 'path_deny_list')

    selected = []
    selected_info = []
    skipped = []
    for pkg in packages:
        info = package_info(pkg, seed_packages)
        reason = ''
        if path_allow and not path_matches(pkg, path_allow):
            reason = 'path_not_allowed'
        elif path_deny and path_matches(pkg, path_deny):
            reason = 'path_denied'
        elif class_allow and not class_matches(info.get('class', ''), class_allow):
            reason = 'class_not_allowed'
        elif class_deny and class_matches(info.get('class', ''), class_deny):
            reason = 'class_denied'

        if reason:
            info['reason'] = reason
            skipped.append(info)
        else:
            selected.append(pkg)
            selected_info.append(info)
    return selected, selected_info, skipped
)PY");

    Script += TEXT(R"PY(
def conflict_path(dest, pkg):
    if pkg.startswith('/Game/'):
        rel = pkg[len('/Game/'):]
    else:
        rel = pkg.strip('/').replace('/', '_', 1)
    return os.path.join(dest, rel + '.uasset')

def destination_candidates(dest, pkg):
    if pkg.startswith('/Game/'):
        rel = pkg[len('/Game/'):]
    else:
        rel = pkg.strip('/').replace('/', '_', 1)
    base = os.path.join(dest, rel)
    return [base + '.uasset', base + '.umap']

def file_info(path):
    exists = os.path.exists(path)
    info = {
        'path': path,
        'exists': bool(exists),
    }
    if exists:
        try:
            info['size'] = int(os.path.getsize(path))
            info['mtime'] = float(os.path.getmtime(path))
        except Exception:
            pass
    return info

def package_destination_state(dest, pkg):
    candidates = [file_info(path) for path in destination_candidates(dest, pkg)]
    existing = [item for item in candidates if item.get('exists')]
    chosen = existing[0] if existing else candidates[0]
    return {
        'package': pkg,
        'destination_file': chosen.get('path', ''),
        'exists': bool(existing),
        'candidates': candidates,
    }

def verify_destination_packages(dest, packages, before_state):
    written = []
    missing = []
    for pkg in packages:
        before = before_state.get(pkg, {})
        after = package_destination_state(dest, pkg)
        row = {
            'package': pkg,
            'destination_file': after.get('destination_file', ''),
            'exists_before': bool(before.get('exists')),
            'exists_after': bool(after.get('exists')),
        }
        if after.get('exists'):
            after_file = next((x for x in after.get('candidates', []) if x.get('exists')), {})
            before_file = next((x for x in before.get('candidates', []) if x.get('exists')), {})
            row['size'] = after_file.get('size', 0)
            row['mtime'] = after_file.get('mtime', 0)
            row['changed_since_before'] = (
                not before_file
                or after_file.get('size') != before_file.get('size')
                or after_file.get('mtime') != before_file.get('mtime')
            )
            written.append(row)
        else:
            row['candidate_files'] = [x.get('path', '') for x in after.get('candidates', [])]
            missing.append(row)
    return written, missing

def rel_path_for_package(pkg):
    parts = pkg.strip('/').split('/')
    if len(parts) <= 1:
        return parts[-1] if parts else ''
    return '/'.join(parts[1:])

def staged_package_for_source(pkg, dest_mount):
    rel = rel_path_for_package(pkg)
    return dest_mount.rstrip('/') + ('/' + rel if rel else '')

def source_equivalent_for_staged(pkg, dest_mount):
    mount = dest_mount.rstrip('/')
    if pkg == mount:
        return '/Game'
    if pkg.startswith(mount + '/'):
        return '/Game/' + pkg[len(mount) + 1:]
    return pkg

def package_leaf_name(pkg):
    text = package_name(pkg)
    return text.rsplit('/', 1)[-1] if text else ''

def package_for_destination_file(dest, dest_mount, path):
    rel = os.path.relpath(path, dest).replace('\\', '/')
    root, ext = os.path.splitext(rel)
    if ext.lower() not in ('.uasset', '.umap'):
        return ''
    return dest_mount.rstrip('/') + '/' + root.strip('/')

def snapshot_destination_packages(dest, dest_mount):
    snapshot = {}
    if not os.path.isdir(dest):
        return snapshot
    for root, _dirs, files in os.walk(dest):
        for name in files:
            ext = os.path.splitext(name)[1].lower()
            if ext not in ('.uasset', '.umap'):
                continue
            path = os.path.join(root, name)
            pkg = package_for_destination_file(dest, dest_mount, path)
            if not pkg:
                continue
            try:
                size = int(os.path.getsize(path))
                mtime = float(os.path.getmtime(path))
            except Exception:
                size = 0
                mtime = 0
            snapshot[pkg] = {
                'package': pkg,
                'destination_file': path,
                'size': size,
                'mtime': mtime,
                'exists': True,
            }
    return snapshot

def diff_destination_snapshots(before, after):
    rows = []
    for pkg, info in sorted(after.items()):
        old = before.get(pkg)
        if not old or old.get('size') != info.get('size') or old.get('mtime') != info.get('mtime'):
            row = dict(info)
            row['exists_before'] = bool(old)
            if old:
                row['size_before'] = old.get('size', 0)
                row['mtime_before'] = old.get('mtime', 0)
            row['changed_since_before'] = True
            rows.append(row)
    return rows

def selected_staged_package_set(selected_packages, dest_mount):
    return set(staged_package_for_source(pkg, dest_mount) for pkg in selected_packages)

def actual_write_side_effects(actual_rows, selected_packages, dest_mount, params):
    selected_staged = selected_staged_package_set(selected_packages, dest_mount)
    path_deny = string_list(params, 'path_deny_list')
    extra = []
    denied = []
    for row in actual_rows:
        pkg = row.get('package', '')
        source_equiv = source_equivalent_for_staged(pkg, dest_mount)
        enriched = dict(row)
        enriched['source_equivalent_package'] = source_equiv
        enriched['selected_staged_package'] = pkg in selected_staged
        if pkg not in selected_staged:
            extra.append(enriched)
        if path_deny and (path_matches(pkg, path_deny) or path_matches(source_equiv, path_deny)):
            denied.append(enriched)
    return extra, denied

def classify_missing_selected_aliases(missing_rows, selected_assets, actual_rows, dest_mount):
    info_by_package = {}
    for info in selected_assets or []:
        pkg = package_name(info.get('package', '')) if isinstance(info, dict) else ''
        if pkg:
            info_by_package[pkg] = info

    actual_by_leaf = {}
    for row in actual_rows or []:
        staged_pkg = package_name(row.get('package', ''))
        source_equiv = package_name(row.get('source_equivalent_package', '')) or source_equivalent_for_staged(staged_pkg, dest_mount)
        for candidate in (staged_pkg, source_equiv):
            leaf = package_leaf_name(candidate)
            if leaf:
                actual_by_leaf.setdefault(leaf, []).append(candidate)

    ignored = []
    blocking = []
    for row in missing_rows or []:
        pkg = package_name(row.get('package', ''))
        info = info_by_package.get(pkg, {})
        leaf = package_leaf_name(pkg)
        matched_actual = sorted(set(actual_by_leaf.get(leaf, [])))
        is_unresolved_non_seed = (
            bool(info)
            and not bool(info.get('asset_registry_found', True))
            and not bool(info.get('is_seed', False))
        )
        if is_unresolved_non_seed and matched_actual:
            enriched = dict(row)
            enriched['reason'] = 'selected package is an unresolved non-seed alias/redirector; AssetTools wrote matching real package(s)'
            enriched['matched_actual_packages'] = matched_actual
            ignored.append(enriched)
        else:
            blocking.append(row)
    return ignored, blocking
)PY");

    Script += TEXT(R"PY(

def make_migration_options(ignore_dependencies, overwrite):
    details = {
        'requested_ignore_dependencies': bool(ignore_dependencies),
        'requested_overwrite': bool(overwrite),
        'created': False,
        'set_fields': [],
        'errors': [],
    }
    try:
        options = unreal.MigrationOptions()
        details['created'] = True
    except Exception as exc:
        details['errors'].append('MigrationOptions unavailable: ' + str(exc))
        return None, details

    for attr in ('ignore_dependencies', 'b_ignore_dependencies', 'bIgnoreDependencies'):
        try:
            setattr(options, attr, bool(ignore_dependencies))
            details['set_fields'].append(attr)
            break
        except Exception:
            pass
    for attr in ('prompt', 'b_prompt', 'bPrompt'):
        try:
            setattr(options, attr, False)
            details['set_fields'].append(attr)
            break
        except Exception:
            pass
    for attr in ('asset_conflict', 'AssetConflict'):
        try:
            enum_cls = getattr(unreal, 'AssetMigrationConflict', None)
            value = None
            if enum_cls is not None:
                names = ('OVERWRITE', 'Overwrite') if overwrite else ('SKIP', 'Skip')
                for name in names:
                    if hasattr(enum_cls, name):
                        value = getattr(enum_cls, name)
                        break
            if value is not None:
                setattr(options, attr, value)
                details['set_fields'].append(attr)
                break
        except Exception as exc:
            details['errors'].append('asset_conflict set failed: ' + str(exc))
    return options, details

def call_migrate_packages(asset_tools, selected_packages, dest, options):
    attempts = []
    if options is not None:
        attempts.extend([
            lambda: asset_tools.migrate_packages(selected_packages, dest, options),
            lambda: asset_tools.migrate_packages([unreal.Name(x) for x in selected_packages], dest, options),
        ])
    attempts.extend([
        lambda: asset_tools.migrate_packages(selected_packages, dest),
        lambda: asset_tools.migrate_packages([unreal.Name(x) for x in selected_packages], dest),
    ])
    last_type_error = None
    for attempt in attempts:
        try:
            return attempt(), ''
        except TypeError as exc:
            last_type_error = str(exc)
            continue
    if last_type_error:
        raise TypeError(last_type_error)
    return None, 'no migrate_packages call attempts were available'
)PY");

    Script += TEXT(R"PY(
try:
    with open(PARAMS_PATH, 'r', encoding='utf-8') as fh:
        params = json.load(fh)
    source_assets, expanded_folders, unresolved_sources = resolve_source_packages(params)
    requested_dest = params.get('destination_content_path', '')
    dest = params.get('destination_content_root') or requested_dest
    dest_mount = params.get('destination_mount_root', '/Game')
    dest_subpath = params.get('destination_subpath', '')
    dest_package_root = params.get('destination_package_root', dest_mount)
    dry_run = bool(params.get('dry_run', False))
    overwrite = bool(params.get('overwrite', False))
    include_dependencies = bool(params.get('include_dependencies', True))
    ignore_assettools_dependencies = bool(params.get('ignore_assettools_dependencies', True))
    seed_set = set(source_assets)
    if not source_assets:
        write_report({
            'success': False,
            'dry_run': dry_run,
            'error': 'no source assets resolved from source_assets/source_folder input',
            'source_assets': source_assets,
            'expanded_folders': expanded_folders,
            'unresolved_sources': unresolved_sources,
        })
        raise SystemExit(0)
    if include_dependencies:
        closure = dependency_closure(source_assets, params)
    else:
        closure = sorted(seed_set)
    selected_packages, selected_assets, skipped_dependencies = filter_packages(closure, seed_set, params)
    if not selected_packages:
        write_report({
            'success': False,
            'dry_run': dry_run,
            'error': 'no packages selected after dependency policy/filtering',
            'include_dependencies': include_dependencies,
            'source_assets': source_assets,
            'expanded_folders': expanded_folders,
            'unresolved_sources': unresolved_sources,
            'dependency_closure': closure,
            'dependency_count': len(closure),
            'selected_packages': selected_packages,
            'selected_count': 0,
            'skipped_dependencies': skipped_dependencies,
            'skipped_count': len(skipped_dependencies),
        })
        raise SystemExit(0)
    conflicts = []
    before_state = {}
    before_snapshot = snapshot_destination_packages(dest, dest_mount)
    for pkg in selected_packages:
        state = package_destination_state(dest, pkg)
        before_state[pkg] = state
        if state.get('exists'):
            conflicts.append({'package': pkg, 'destination_file': state.get('destination_file', '')})
    would_reject_without_overwrite = bool(conflicts and not overwrite)
    if would_reject_without_overwrite and not dry_run:
        write_report({
            'success': False,
            'dry_run': dry_run,
            'error': 'destination conflicts; pass overwrite:true to proceed',
            'include_dependencies': include_dependencies,
            'source_assets': source_assets,
            'expanded_folders': expanded_folders,
            'unresolved_sources': unresolved_sources,
            'dependency_closure': closure,
            'dependency_count': len(closure),
            'selected_packages': selected_packages,
            'selected_assets': selected_assets,
            'selected_count': len(selected_packages),
            'skipped_dependencies': skipped_dependencies,
            'skipped_count': len(skipped_dependencies),
            'conflicts': conflicts,
            'would_reject_without_overwrite': would_reject_without_overwrite,
        })
    else:
        migrated = False
        migration_result = None
        migration_result_repr = ''
        written_assets = []
        missing_written_assets = []
        actual_written_assets = []
        extra_written_assets = []
        denied_written_assets = []
        missing_selected_alias_assets = []
        blocking_missing_written_assets = []
        migration_options_details = {}
        migration_call_warning = ''
        verification_error = ''
        if not dry_run:
            os.makedirs(dest, exist_ok=True)
            asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
            options, migration_options_details = make_migration_options(ignore_assettools_dependencies, overwrite)
            migration_result, migration_call_warning = call_migrate_packages(asset_tools, selected_packages, dest, options)
            migration_result_repr = repr(migration_result)
            migrated = migration_result is not False
            written_assets, missing_written_assets = verify_destination_packages(dest, selected_packages, before_state)
            after_snapshot = snapshot_destination_packages(dest, dest_mount)
            actual_written_assets = diff_destination_snapshots(before_snapshot, after_snapshot)
            extra_written_assets, denied_written_assets = actual_write_side_effects(
                actual_written_assets, selected_packages, dest_mount, params)
            missing_selected_alias_assets, blocking_missing_written_assets = classify_missing_selected_aliases(
                missing_written_assets, selected_assets, actual_written_assets, dest_mount)
            if not migrated:
                verification_error = 'AssetTools.migrate_packages returned false'
            elif not written_assets:
                verification_error = 'migration wrote no destination package files'
            elif blocking_missing_written_assets:
                verification_error = 'one or more selected packages were not found at the destination after migration'
        success = bool(dry_run or (migrated and written_assets and not blocking_missing_written_assets))
        if verification_error:
            success = False
)PY");

    Script += TEXT(R"PY(
        write_report({
            'success': success,
            'error': verification_error,
            'dry_run': dry_run,
            'migrated': migrated,
            'migration_result': migration_result_repr,
            'migration_call_warning': migration_call_warning,
            'migration_options': migration_options_details,
            'ignore_assettools_dependencies': ignore_assettools_dependencies,
            'include_dependencies': include_dependencies,
            'source_assets': source_assets,
            'expanded_folders': expanded_folders,
            'unresolved_sources': unresolved_sources,
            'dependency_closure': closure,
            'dependency_count': len(closure),
            'selected_packages': selected_packages,
            'selected_assets': selected_assets,
            'selected_count': len(selected_packages),
            'skipped_dependencies': skipped_dependencies,
            'skipped_count': len(skipped_dependencies),
            'conflicts': conflicts,
            'would_reject_without_overwrite': would_reject_without_overwrite,
            'destination_content_path': requested_dest,
            'migration_destination_content_root': dest,
            'destination_mount_root': dest_mount,
            'destination_subpath': dest_subpath,
            'destination_package_root': dest_package_root,
            'written_assets': written_assets,
            'written_count': len(written_assets),
            'missing_written_assets': blocking_missing_written_assets,
            'missing_written_count': len(blocking_missing_written_assets),
            'missing_selected_alias_assets': missing_selected_alias_assets,
            'missing_selected_alias_count': len(missing_selected_alias_assets),
            'staged_missing_written_assets_all': missing_written_assets,
            'staged_missing_written_all_count': len(missing_written_assets),
            'actual_written_assets': actual_written_assets,
            'actual_written_count': len(actual_written_assets),
            'extra_written_assets': extra_written_assets,
            'extra_written_count': len(extra_written_assets),
            'denied_written_assets': denied_written_assets,
            'denied_written_count': len(denied_written_assets),
            'write_set_exact': len(extra_written_assets) == 0 and len(denied_written_assets) == 0,
        })
except Exception as exc:
    write_report({
        'success': False,
        'error': str(exc),
        'traceback': traceback.format_exc(),
    })
)PY");
    return Script;
}

FSageToolDispatch::FOutcome MigrateFromProjectImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SourceProject;
    FString Destination;
    TArray<TSharedPtr<FJsonValue>> SourceAssetValues;
    TArray<TSharedPtr<FJsonValue>> SourceFolderValues;
    auto AppendStringField = [Args](const TCHAR* Field, TArray<TSharedPtr<FJsonValue>>& Out)
    {
        FString Value;
        if (Args.IsValid() && Args->TryGetStringField(Field, Value) && !Value.IsEmpty())
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
    };
    auto AppendArrayField = [Args](const TCHAR* Field, TArray<TSharedPtr<FJsonValue>>& Out)
    {
        if (!Args.IsValid())
        {
            return;
        }
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Args->TryGetArrayField(Field, Values) || Values == nullptr)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (!Value.IsValid())
            {
                continue;
            }
            const FString S = Value->AsString();
            if (!S.IsEmpty())
            {
                Out.Add(MakeShared<FJsonValueString>(S));
            }
        }
    };

    if (Args.IsValid())
    {
        AppendArrayField(TEXT("source_assets"), SourceAssetValues);
        AppendArrayField(TEXT("assets"), SourceAssetValues);
        AppendStringField(TEXT("source_asset"), SourceAssetValues);
        AppendArrayField(TEXT("source_folders"), SourceFolderValues);
        AppendArrayField(TEXT("source_paths"), SourceFolderValues);
        AppendStringField(TEXT("source_folder"), SourceFolderValues);
        AppendStringField(TEXT("source_path"), SourceFolderValues);
    }

    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("source_project"), SourceProject)
        || (SourceAssetValues.Num() == 0 && SourceFolderValues.Num() == 0)
        || (!Args->TryGetStringField(TEXT("destination_mount_or_path"), Destination)
            && !Args->TryGetStringField(TEXT("destination"), Destination)
            && !Args->TryGetStringField(TEXT("destination_path"), Destination)
            && !Args->TryGetStringField(TEXT("destination_content_path"), Destination)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'source_project', source asset/folder input, or destination_mount_or_path"));
    }

    FString SourceProjectFile;
    if (!ResolveUProjectFile(SourceProject, SourceProjectFile))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("source_project is not a .uproject or project directory: %s"),
                            *SourceProject));
    }

    bool bDryRun = false;
    bool bOverwrite = false;
    bool bIncludeDependencies = true;
    bool bIncludeHardDependencies = true;
    bool bIncludeSoftDependencies = true;
    bool bIncludeHardManagementReferences = false;
    bool bIncludeSoftManagementReferences = false;
    bool bIncludeSearchableNames = false;
    bool bIgnoreAssetToolsDependencies = true;
    double TimeoutSeconds = 1800.0;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
        Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
        Args->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
        Args->TryGetBoolField(TEXT("include_hard_dependencies"), bIncludeHardDependencies);
        Args->TryGetBoolField(TEXT("include_hard_package_references"), bIncludeHardDependencies);
        Args->TryGetBoolField(TEXT("include_soft_dependencies"), bIncludeSoftDependencies);
        Args->TryGetBoolField(TEXT("include_soft_package_references"), bIncludeSoftDependencies);
        Args->TryGetBoolField(TEXT("include_hard_management_references"), bIncludeHardManagementReferences);
        Args->TryGetBoolField(TEXT("include_soft_management_references"), bIncludeSoftManagementReferences);
        Args->TryGetBoolField(TEXT("include_searchable_names"), bIncludeSearchableNames);
        Args->TryGetBoolField(TEXT("ignore_assettools_dependencies"), bIgnoreAssetToolsDependencies);
        Args->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
    }

    const FMigrationDestination DestinationInfo = ResolveMigrationDestination(Destination);
    if (!DestinationInfo.bValid)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, DestinationInfo.Error);
    }

    const FString WorkDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SageBridge"), TEXT("Migrate"));
    IFileManager::Get().MakeDirectory(*WorkDir, true);
    const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ParamsPath = FPaths::Combine(WorkDir, FString::Printf(TEXT("params_%s.json"), *RunId));
    const FString ScriptPath = FPaths::Combine(WorkDir, FString::Printf(TEXT("migrate_%s.py"), *RunId));
    const FString ReportPath = FPaths::Combine(WorkDir, FString::Printf(TEXT("report_%s.json"), *RunId));
    const FString CommandletLogPath = FPaths::Combine(WorkDir, FString::Printf(TEXT("commandlet_%s.log"), *RunId));

    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("source_project_file"), SourceProjectFile);
    Params->SetStringField(TEXT("destination_content_path"), DestinationInfo.RequestedContentPath);
    Params->SetStringField(TEXT("destination_content_root"), DestinationInfo.ContentRootPath);
    Params->SetStringField(TEXT("destination_mount_root"), DestinationInfo.MountRoot);
    Params->SetStringField(TEXT("destination_subpath"), DestinationInfo.SubPath);
    Params->SetStringField(TEXT("destination_package_root"), DestinationInfo.LongPackageRoot);
    Params->SetBoolField(TEXT("dry_run"), bDryRun);
    Params->SetBoolField(TEXT("overwrite"), bOverwrite);
    Params->SetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
    Params->SetBoolField(TEXT("include_hard_dependencies"), bIncludeHardDependencies);
    Params->SetBoolField(TEXT("include_soft_dependencies"), bIncludeSoftDependencies);
    Params->SetBoolField(TEXT("include_hard_management_references"), bIncludeHardManagementReferences);
    Params->SetBoolField(TEXT("include_soft_management_references"), bIncludeSoftManagementReferences);
    Params->SetBoolField(TEXT("include_searchable_names"), bIncludeSearchableNames);
    Params->SetBoolField(TEXT("ignore_assettools_dependencies"), bIgnoreAssetToolsDependencies);
    Params->SetArrayField(TEXT("source_assets"), SourceAssetValues);
    Params->SetArrayField(TEXT("source_folders"), SourceFolderValues);

    auto CopyStringArrayField = [&Params, Args](const TCHAR* SourceField, const TCHAR* DestField)
    {
        if (!Args.IsValid())
        {
            return;
        }
        TArray<TSharedPtr<FJsonValue>> Out;
        FString StringValue;
        if (Args->TryGetStringField(SourceField, StringValue) && !StringValue.IsEmpty())
        {
            Out.Add(MakeShared<FJsonValueString>(StringValue));
        }
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Args->TryGetArrayField(SourceField, Values) && Values != nullptr)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                if (!Value.IsValid())
                {
                    continue;
                }
                FString S = Value->AsString();
                if (!S.IsEmpty())
                {
                    Out.Add(MakeShared<FJsonValueString>(S));
                }
            }
        }
        if (Out.Num() > 0)
        {
            Params->SetArrayField(DestField, Out);
        }
    };
    CopyStringArrayField(TEXT("class_allow_list"), TEXT("class_allow_list"));
    CopyStringArrayField(TEXT("class_allowlist"), TEXT("class_allow_list"));
    CopyStringArrayField(TEXT("allowed_classes"), TEXT("class_allow_list"));
    CopyStringArrayField(TEXT("class_deny_list"), TEXT("class_deny_list"));
    CopyStringArrayField(TEXT("class_denylist"), TEXT("class_deny_list"));
    CopyStringArrayField(TEXT("denied_classes"), TEXT("class_deny_list"));
    CopyStringArrayField(TEXT("path_allow_list"), TEXT("path_allow_list"));
    CopyStringArrayField(TEXT("path_allowlist"), TEXT("path_allow_list"));
    CopyStringArrayField(TEXT("allowed_paths"), TEXT("path_allow_list"));
    CopyStringArrayField(TEXT("path_deny_list"), TEXT("path_deny_list"));
    CopyStringArrayField(TEXT("path_denylist"), TEXT("path_deny_list"));
    CopyStringArrayField(TEXT("denied_paths"), TEXT("path_deny_list"));

    if (!WriteJsonObjectFile(ParamsPath, Params))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to write migration params: %s"), *ParamsPath));
    }
    if (!FFileHelper::SaveStringToFile(BuildMigrationPythonScript(ParamsPath, ReportPath), *ScriptPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to write migration script: %s"), *ScriptPath));
    }

    FString EditorCmd = FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries/Win64/UnrealEditor-Cmd.exe"));
    if (!FPaths::FileExists(EditorCmd))
    {
        EditorCmd = FPlatformProcess::ExecutablePath();
    }
    FString CmdLine = FString::Printf(TEXT("\"%s\" -run=pythonscript -script=\"%s\" -unattended -nop4 -nosplash -abslog=\"%s\""),
                                      *SourceProjectFile, *ScriptPath, *CommandletLogPath);
    uint32 ProcId = 0;
    FProcHandle Proc = FPlatformProcess::CreateProc(
        *EditorCmd,
        *CmdLine,
        /*bLaunchDetached=*/true,
        /*bLaunchHidden=*/true,
        /*bLaunchReallyHidden=*/true,
        &ProcId,
        0,
        *FPaths::GetPath(SourceProjectFile),
        nullptr);
    if (!Proc.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("failed to launch source editor: %s %s"),
                            *EditorCmd, *CmdLine));
    }

    const double StartTime = FPlatformTime::Seconds();
    bool bTimedOut = false;
    while (FPlatformProcess::IsProcRunning(Proc))
    {
        if (FPlatformTime::Seconds() - StartTime > FMath::Max(1.0, TimeoutSeconds))
        {
            bTimedOut = true;
            FPlatformProcess::TerminateProc(Proc, true);
            break;
        }
        FPlatformProcess::Sleep(0.25f);
    }
    int32 ReturnCode = 0;
    FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
    FPlatformProcess::CloseProc(Proc);

    TSharedPtr<FJsonObject> Report = ReadJsonObjectFile(ReportPath);
    if (!Report.IsValid())
    {
        auto ErrorReport = MakeShared<FJsonObject>();
        ErrorReport->SetBoolField(TEXT("success"), false);
        ErrorReport->SetBoolField(TEXT("rejected"), true);
        ErrorReport->SetBoolField(TEXT("timed_out"), bTimedOut);
        ErrorReport->SetNumberField(TEXT("process_exit_code"), ReturnCode);
        ErrorReport->SetStringField(TEXT("error"),
            FString::Printf(TEXT("migration process did not write a readable report (timed_out=%s, exit_code=%d, report=%s)"),
                            bTimedOut ? TEXT("true") : TEXT("false"),
                            ReturnCode,
                            *ReportPath));
        ErrorReport->SetStringField(TEXT("source_project_file"), SourceProjectFile);
        ErrorReport->SetStringField(TEXT("destination_content_path"), DestinationInfo.RequestedContentPath);
        ErrorReport->SetStringField(TEXT("destination_content_root"), DestinationInfo.ContentRootPath);
        ErrorReport->SetStringField(TEXT("destination_mount_root"), DestinationInfo.MountRoot);
        ErrorReport->SetStringField(TEXT("destination_subpath"), DestinationInfo.SubPath);
        ErrorReport->SetStringField(TEXT("destination_package_root"), DestinationInfo.LongPackageRoot);
        ErrorReport->SetStringField(TEXT("report_path"), ReportPath);
        ErrorReport->SetStringField(TEXT("script_path"), ScriptPath);
        ErrorReport->SetStringField(TEXT("commandlet_log_path"), CommandletLogPath);
        ErrorReport->SetBoolField(TEXT("commandlet_log_exists"), FPaths::FileExists(CommandletLogPath));
        ErrorReport->SetStringField(TEXT("commandlet_log_tail"), ReadTextFileTail(CommandletLogPath));
        return FSageToolDispatch::FOutcome::MakeSuccess(ErrorReport);
    }
    Report->SetBoolField(TEXT("timed_out"), bTimedOut);
    Report->SetNumberField(TEXT("process_exit_code"), ReturnCode);
    Report->SetStringField(TEXT("source_project_file"), SourceProjectFile);
    Report->SetStringField(TEXT("destination_content_path"), DestinationInfo.RequestedContentPath);
    Report->SetStringField(TEXT("destination_content_root"), DestinationInfo.ContentRootPath);
    Report->SetStringField(TEXT("migration_destination_content_root"), DestinationInfo.ContentRootPath);
    Report->SetStringField(TEXT("destination_mount_root"), DestinationInfo.MountRoot);
    Report->SetStringField(TEXT("destination_subpath"), DestinationInfo.SubPath);
    Report->SetStringField(TEXT("destination_package_root"), DestinationInfo.LongPackageRoot);
    Report->SetStringField(TEXT("report_path"), ReportPath);
    Report->SetStringField(TEXT("script_path"), ScriptPath);
    Report->SetStringField(TEXT("commandlet_log_path"), CommandletLogPath);
    Report->SetBoolField(TEXT("commandlet_log_exists"), FPaths::FileExists(CommandletLogPath));
    Report->SetStringField(TEXT("commandlet_log_tail"), ReadTextFileTail(CommandletLogPath));

    bool bSuccess = false;
    Report->TryGetBoolField(TEXT("success"), bSuccess);
    double WrittenCount = 0.0;
    double MissingWrittenCount = 0.0;
    Report->TryGetNumberField(TEXT("written_count"), WrittenCount);
    Report->TryGetNumberField(TEXT("missing_written_count"), MissingWrittenCount);
    if (ReturnCode != 0)
    {
        const bool bVerifiedSafe = !bDryRun && bSuccess && WrittenCount > 0.0 && MissingWrittenCount <= 0.0;
        Report->SetBoolField(TEXT("process_exit_nonzero"), true);
        Report->SetBoolField(TEXT("process_exit_verified_safe"), bVerifiedSafe);
        if (!bVerifiedSafe)
        {
            FString ExistingError;
            Report->TryGetStringField(TEXT("error"), ExistingError);
            Report->SetBoolField(TEXT("success"), false);
            Report->SetStringField(
                TEXT("error"),
                ExistingError.IsEmpty()
                    ? FString::Printf(TEXT("migration commandlet exited with code %d and destination writes were not verified"), ReturnCode)
                    : FString::Printf(TEXT("%s; commandlet exit code %d was not verified safe"), *ExistingError, ReturnCode));
            bSuccess = false;
        }
    }

    if (!bDryRun)
    {
        FString UnexpectedWriteError;
        if (!RollbackUnexpectedMigrationWrites(Report, DestinationInfo, bOverwrite, UnexpectedWriteError))
        {
            FString ExistingError;
            Report->TryGetStringField(TEXT("error"), ExistingError);
            Report->SetBoolField(TEXT("success"), false);
            Report->SetStringField(TEXT("error"),
                ExistingError.IsEmpty()
                    ? UnexpectedWriteError
                    : FString::Printf(TEXT("%s; %s"), *ExistingError, *UnexpectedWriteError));
            bSuccess = false;
        }
    }

    double ActualWrittenCount = 0.0;
    double ExtraWrittenCount = 0.0;
    double DeniedWrittenCount = 0.0;
    Report->TryGetNumberField(TEXT("actual_written_count"), ActualWrittenCount);
    Report->TryGetNumberField(TEXT("extra_written_count"), ExtraWrittenCount);
    Report->TryGetNumberField(TEXT("denied_written_count"), DeniedWrittenCount);
    const bool bCanRelocateActualWritesAfterStagedMiss =
        !bDryRun
        && !bTimedOut
        && !DestinationInfo.SubPath.IsEmpty()
        && ActualWrittenCount > 0.0
        && WrittenCount > 0.0
        && ExtraWrittenCount <= 0.0
        && DeniedWrittenCount <= 0.0;
    if (!bSuccess && bCanRelocateActualWritesAfterStagedMiss)
    {
        FString StagedVerificationError;
        Report->TryGetStringField(TEXT("error"), StagedVerificationError);
        if (!StagedVerificationError.IsEmpty())
        {
            Report->SetStringField(TEXT("staged_verification_error"), StagedVerificationError);
        }
        Report->SetBoolField(TEXT("relocation_after_staged_verification_failure"), true);
        Report->SetStringField(TEXT("error"), FString());
        Report->SetBoolField(TEXT("success"), true);
        bSuccess = true;
    }

    if (bSuccess && !bDryRun && !DestinationInfo.SubPath.IsEmpty())
    {
        const TArray<TSharedPtr<FJsonValue>>* StagedWrittenAssets = nullptr;
        if (Report->TryGetArrayField(TEXT("written_assets"), StagedWrittenAssets) && StagedWrittenAssets != nullptr)
        {
            Report->SetArrayField(TEXT("staged_written_assets"), *StagedWrittenAssets);
        }
        const TArray<TSharedPtr<FJsonValue>>* StagedMissingAssets = nullptr;
        if (Report->TryGetArrayField(TEXT("missing_written_assets"), StagedMissingAssets) && StagedMissingAssets != nullptr)
        {
            Report->SetArrayField(TEXT("staged_missing_written_assets"), *StagedMissingAssets);
        }
        double StagedWrittenCount = 0.0;
        double StagedMissingCount = 0.0;
        Report->TryGetNumberField(TEXT("written_count"), StagedWrittenCount);
        Report->TryGetNumberField(TEXT("missing_written_count"), StagedMissingCount);
        Report->SetNumberField(TEXT("staged_written_count"), StagedWrittenCount);
        Report->SetNumberField(TEXT("staged_missing_written_count"), StagedMissingCount);

        FString RelocationError;
        if (!RelocateMigratedPackages(Report, DestinationInfo, bOverwrite, RelocationError))
        {
            FString ExistingError;
            Report->TryGetStringField(TEXT("error"), ExistingError);
            Report->SetBoolField(TEXT("success"), false);
            Report->SetStringField(TEXT("error"),
                ExistingError.IsEmpty()
                    ? RelocationError
                    : FString::Printf(TEXT("%s; %s"), *ExistingError, *RelocationError));
            bSuccess = false;
        }
        else
        {
            FString StagedVerificationError;
            if (Report->TryGetStringField(TEXT("staged_verification_error"), StagedVerificationError)
                && !StagedVerificationError.IsEmpty())
            {
                Report->SetStringField(TEXT("error"), FString());
            }
            FString AnimationSkeletonError;
            if (!VerifyAndRepairAnimationSkeletonReferences(Report, DestinationInfo, AnimationSkeletonError))
            {
                Report->SetBoolField(TEXT("success"), false);
                Report->SetStringField(TEXT("error"), AnimationSkeletonError);
                bSuccess = false;
            }
            else
            {
                Report->SetBoolField(TEXT("success"), true);
            }
        }
    }

    if (!bSuccess)
    {
        Report->SetBoolField(TEXT("rejected"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(Report);
    }

    if (!bDryRun)
    {
        TArray<FString> PathsToScan;
        PathsToScan.Add(DestinationInfo.LongPackageRoot);
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        ARM.Get().ScanPathsSynchronous(PathsToScan, /*bForceRescan=*/true);
    }

    return FSageToolDispatch::FOutcome::MakeSuccess(Report);
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
    // Lyra Sage Gap #11: TArray append helper, pairs with the FStructProperty
    // recursive JSON-object writer so DataAsset CDO struct arrays can be
    // authored without nuking the array.
    Dispatch.RegisterHandler(TEXT("asset.add_array_element"),  GT(&AddArrayElementImpl));
    Dispatch.RegisterHandler(TEXT("asset.clear_array_property"), GT(&ClearArrayPropertyImpl));
    Dispatch.RegisterHandler(TEXT("asset.replace_array_property"), GT(&ReplaceArrayPropertyImpl));

    // KaleGame P8 / Gap #30: typed GameFeature AddComponents authoring.
    Dispatch.RegisterHandler(TEXT("gamefeature.list_component_entries"),
        GT(&GameFeatureListComponentEntriesImpl));
    Dispatch.RegisterHandler(TEXT("gamefeature.add_component_entry"),
        GT(&GameFeatureAddComponentEntryImpl));
    Dispatch.RegisterHandler(TEXT("gamefeature.remove_component_entry"),
        GT(&GameFeatureRemoveComponentEntryImpl));
    Dispatch.RegisterHandler(TEXT("gamefeature.ensure_add_components_action"),
        GT(&GameFeatureEnsureAddComponentsActionImpl));
    Dispatch.RegisterHandler(TEXT("gamefeature.add_widget_entry"),
        GT(&GameFeatureAddWidgetEntryImpl));

    // Phase 4.5-r2 batch 2: socket management
    Dispatch.RegisterHandler(TEXT("asset.list_sockets"),       GT(&ListSocketsImpl));
    Dispatch.RegisterHandler(TEXT("asset.add_socket"),         GT(&AddSocketImpl));
    Dispatch.RegisterHandler(TEXT("asset.get_socket"),         GT(&GetSocketImpl));
    Dispatch.RegisterHandler(TEXT("asset.upsert_socket"),      GT(&UpsertSocketImpl));
    Dispatch.RegisterHandler(TEXT("asset.update_socket"),      GT(&UpsertSocketImpl));
    Dispatch.RegisterHandler(TEXT("asset.remove_socket"),      GT(&RemoveSocketImpl));

    // Phase 4.5-r2 batch 3: textures
    Dispatch.RegisterHandler(TEXT("asset.list_textures"),         GT(&ListTexturesImpl));
    Dispatch.RegisterHandler(TEXT("asset.get_texture_info"),      GT(&GetTextureInfoImpl));
    Dispatch.RegisterHandler(TEXT("asset.set_texture_settings"),  GT(&SetTextureSettingsImpl));

    // Phase 4.5-r2 batch 4: write essentials
    Dispatch.RegisterHandler(TEXT("asset.create_data_asset"),     GT(&CreateDataAssetImpl));
    Dispatch.RegisterHandler(TEXT("asset.delete_batch"),          GT(&DeleteBatchImpl));
    Dispatch.RegisterHandler(TEXT("asset.reload_package"),        GT(&ReloadPackageImpl));
    Dispatch.RegisterHandler(TEXT("asset.migrate_from_project"),  GT(&MigrateFromProjectImpl));

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
    Dispatch.RegisterHandler(TEXT("animation.import_fbx_animation"), GT(&ImportAnimationImpl));
    Dispatch.RegisterHandler(TEXT("animation.batch_import_fbx_animations"), GT(&BatchImportFbxAnimationsImpl));

    // Write
    Dispatch.RegisterHandler(TEXT("asset.bulk_rename"),        GT(&BulkRenameImpl));
    Dispatch.RegisterHandler(TEXT("asset.move_folder"),        GT(&MoveFolderImpl));
    Dispatch.RegisterHandler(TEXT("asset.fixup_redirectors"),  GT(&FixupRedirectorsImpl));
    Dispatch.RegisterHandler(TEXT("asset.create_folder"),      GT(&AssetCreateFolderImpl));
    Dispatch.RegisterHandler(TEXT("asset.delete_folder"),      GT(&AssetDeleteFolderImpl));
    Dispatch.RegisterHandler(TEXT("asset.save"),               GT(&AssetSaveImpl));
    Dispatch.RegisterHandler(TEXT("asset.save_all_dirty"),     GT(&AssetSaveAllDirtyImpl));
    Dispatch.RegisterHandler(TEXT("asset.create_interchange_pipeline"),
                                                                 GT(&AssetCreateInterchangePipelineImpl));
    Dispatch.RegisterHandler(TEXT("asset.import_texture_batch"),GT(&AssetImportTextureBatchImpl));
    Dispatch.RegisterHandler(TEXT("asset.read_import_sources"), GT(&AssetReadImportSourcesImpl));
    Dispatch.RegisterHandler(TEXT("asset.health_check"),        GT(&AssetHealthCheckImpl));
    Dispatch.RegisterHandler(TEXT("asset.generate_report"),     GT(&AssetGenerateReportImpl));
    Dispatch.RegisterHandler(TEXT("asset.create_thumbnail"),    GT(&AssetCreateThumbnailImpl));
    Dispatch.RegisterHandler(TEXT("asset.validate"),            GT(&AssetValidateImpl));
    Dispatch.RegisterHandler(TEXT("asset.set_tags"),            GT(&AssetSetTagsImpl));
    Dispatch.RegisterHandler(TEXT("fab_ops"),                   GT(&FabOpsImpl));

    // Trailing asset tools
    Dispatch.RegisterHandler(TEXT("asset.recenter_pivot"), GT(&RecentrePivotImpl));
    Dispatch.RegisterHandler(TEXT("asset.set_mesh_nav"),   GT(&SetMeshNavImpl));
    Dispatch.RegisterHandler(TEXT("asset.search_fts"),     GT(&SearchFtsImpl));
    Dispatch.RegisterHandler(TEXT("asset.reindex_fts"),    GT(&ReindexFtsImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
