#include "Tools/SageAssetTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Class.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

UEditorAssetSubsystem* GetAssetSubsystem(FSageToolDispatch::FOutcome& OutErr)
{
    if (GEditor == nullptr)
    {
        OutErr = FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
        return nullptr;
    }
    UEditorAssetSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (Sub == nullptr)
    {
        OutErr = FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("UEditorAssetSubsystem unavailable"));
    }
    return Sub;
}

FString NormalizePackagePath(FString Path)
{
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    const int32 Dot = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (Dot != INDEX_NONE)
    {
        Path = Path.Left(Dot);
    }
    return Path;
}

FString PackageLeafName(const FString& PackagePath)
{
    FString Left;
    FString Right;
    if (PackagePath.Split(TEXT("/"), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
    {
        return Right;
    }
    return PackagePath;
}

FString ObjectPathForPackage(const FString& PackagePath)
{
    const FString Package = NormalizePackagePath(PackagePath);
    return Package + TEXT(".") + PackageLeafName(Package);
}

UObject* ResolveAssetForDelete(const FString& Path)
{
    if (Path.IsEmpty())
    {
        return nullptr;
    }
    FSoftObjectPath Soft(Path.Contains(TEXT(".")) ? Path : ObjectPathForPackage(Path));
    if (UObject* Obj = Soft.ResolveObject())
    {
        return Obj;
    }
    return Soft.TryLoad();
}

FString ExtractObjectPathString(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid() || Value->IsNull())
    {
        return FString();
    }
    FString Raw = Value->AsString().TrimStartAndEnd();
    int32 FirstQuote = INDEX_NONE;
    int32 LastQuote = INDEX_NONE;
    if (Raw.FindChar(TEXT('\''), FirstQuote)
        && Raw.FindLastChar(TEXT('\''), LastQuote)
        && LastQuote > FirstQuote)
    {
        Raw = Raw.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
    }
    return Raw;
}

TSharedPtr<FJsonObject> DeleteDiagnosticJson(const FString& AssetPath, UEditorAssetSubsystem* Sub)
{
    const FString Package = NormalizePackagePath(AssetPath);
    auto Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("asset_path"), AssetPath);
    Row->SetStringField(TEXT("package"), Package);
    Row->SetStringField(TEXT("object_path"), ObjectPathForPackage(Package));
    Row->SetBoolField(TEXT("editor_asset_subsystem_exists"), Sub && Sub->DoesAssetExist(AssetPath));
    Row->SetBoolField(TEXT("editor_asset_subsystem_package_exists"), Sub && Sub->DoesAssetExist(Package));

    UPackage* LoadedPackage = FindPackage(nullptr, *Package);
    Row->SetBoolField(TEXT("package_loaded"), LoadedPackage != nullptr);
    Row->SetBoolField(TEXT("package_dirty"), LoadedPackage && LoadedPackage->IsDirty());

    UObject* Obj = ResolveAssetForDelete(AssetPath);
    Row->SetBoolField(TEXT("object_loaded"), Obj != nullptr);
    if (Obj)
    {
        Row->SetStringField(TEXT("loaded_object_path"), Obj->GetPathName());
        Row->SetStringField(TEXT("class"), Obj->GetClass() ? Obj->GetClass()->GetName() : FString());
        Row->SetBoolField(TEXT("is_redirector"), Obj->IsA<UObjectRedirector>());
    }

    FString BaseFilename;
    bool bFileExists = false;
    FString ExistingFile;
    if (FPackageName::TryConvertLongPackageNameToFilename(Package, BaseFilename))
    {
        const FString UAsset = BaseFilename + TEXT(".uasset");
        const FString UMap = BaseFilename + TEXT(".umap");
        if (FPaths::FileExists(UAsset))
        {
            bFileExists = true;
            ExistingFile = UAsset;
        }
        else if (FPaths::FileExists(UMap))
        {
            bFileExists = true;
            ExistingFile = UMap;
        }
    }
    Row->SetBoolField(TEXT("file_exists"), bFileExists);
    Row->SetStringField(TEXT("file"), ExistingFile);
    if (!ExistingFile.IsEmpty())
    {
        Row->SetBoolField(TEXT("file_read_only"), IFileManager::Get().IsReadOnly(*ExistingFile));
    }

    TArray<FName> Referencers;
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    ARM.Get().GetReferencers(FName(*Package), Referencers, UE::AssetRegistry::EDependencyCategory::Package);
    TArray<TSharedPtr<FJsonValue>> RefRows;
    for (const FName& Referencer : Referencers)
    {
        RefRows.Add(MakeShared<FJsonValueString>(Referencer.ToString()));
    }
    Row->SetArrayField(TEXT("referencers"), RefRows);
    Row->SetNumberField(TEXT("referencer_count"), RefRows.Num());
    return Row;
}

FString DeleteFailureReason(const TSharedPtr<FJsonObject>& Diag)
{
    if (!Diag.IsValid())
    {
        return TEXT("DeleteAsset returned false");
    }
    bool bPackageLoaded = false;
    bool bPackageDirty = false;
    bool bObjectLoaded = false;
    bool bIsRedirector = false;
    bool bFileExists = false;
    bool bReadOnly = false;
    double ReferencerCount = 0.0;
    Diag->TryGetBoolField(TEXT("package_loaded"), bPackageLoaded);
    Diag->TryGetBoolField(TEXT("package_dirty"), bPackageDirty);
    Diag->TryGetBoolField(TEXT("object_loaded"), bObjectLoaded);
    Diag->TryGetBoolField(TEXT("is_redirector"), bIsRedirector);
    Diag->TryGetBoolField(TEXT("file_exists"), bFileExists);
    Diag->TryGetBoolField(TEXT("file_read_only"), bReadOnly);
    Diag->TryGetNumberField(TEXT("referencer_count"), ReferencerCount);

    TArray<FString> Parts;
    if (bPackageLoaded)
    {
        Parts.Add(bPackageDirty ? TEXT("loaded dirty package") : TEXT("loaded package"));
    }
    if (!bObjectLoaded)
    {
        Parts.Add(bFileExists
            ? TEXT("package file exists but asset object could not be loaded")
            : TEXT("asset object could not be loaded"));
    }
    if (bIsRedirector)
    {
        Parts.Add(TEXT("asset is a redirector; run asset.fixup_redirectors before retrying"));
    }
    if (ReferencerCount > 0.0)
    {
        Parts.Add(FString::Printf(TEXT("%.0f referencer(s) still point at this package"), ReferencerCount));
    }
    if (bReadOnly)
    {
        Parts.Add(TEXT("package file is read-only"));
    }
    if (Parts.Num() == 0)
    {
        Parts.Add(TEXT("EditorAssetSubsystem.DeleteAsset returned false"));
    }
    return FString::Join(Parts, TEXT("; "));
}

bool DeleteExistingDestinationForMove(
    UEditorAssetSubsystem* Sub,
    const FString& Destination,
    TSharedPtr<FJsonObject>& OutReport)
{
    OutReport = MakeShared<FJsonObject>();
    OutReport->SetStringField(TEXT("destination"), Destination);
    OutReport->SetObjectField(TEXT("before"), DeleteDiagnosticJson(Destination, Sub));
    const bool bDeleted = Sub && Sub->DeleteAsset(Destination);
    OutReport->SetBoolField(TEXT("delete_asset_returned"), bDeleted);
    if (!bDeleted)
    {
        UObject* Obj = ResolveAssetForDelete(Destination);
        OutReport->SetBoolField(TEXT("force_delete_attempted"), Obj != nullptr);
        if (Obj)
        {
            TArray<UObject*> Objects;
            Objects.Add(Obj);
            const int32 ForceDeleted = ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
            OutReport->SetNumberField(TEXT("force_delete_count"), ForceDeleted);
            if (ForceDeleted <= 0 && ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false))
            {
                OutReport->SetBoolField(TEXT("delete_single_object_fallback"), true);
            }
            CollectGarbage(RF_NoFlags, /*bPerformFullPurge=*/true);
        }
    }
    TSharedPtr<FJsonObject> After = DeleteDiagnosticJson(Destination, Sub);
    OutReport->SetObjectField(TEXT("after"), After);
    bool bFileStillExists = false;
    if (After.IsValid())
    {
        After->TryGetBoolField(TEXT("file_exists"), bFileStillExists);
    }
    const bool bStillExists = (Sub && (Sub->DoesAssetExist(Destination) || Sub->DoesAssetExist(NormalizePackagePath(Destination))))
        || bFileStillExists;
    OutReport->SetBoolField(TEXT("target_cleared"), !bStillExists);
    if (bStillExists)
    {
        const TSharedPtr<FJsonObject>* AfterPtr = nullptr;
        if (OutReport->TryGetObjectField(TEXT("after"), AfterPtr) && AfterPtr && AfterPtr->IsValid())
        {
            OutReport->SetStringField(TEXT("reason"), DeleteFailureReason(*AfterPtr));
        }
        else
        {
            OutReport->SetStringField(TEXT("reason"), TEXT("target still exists after overwrite delete"));
        }
    }
    return !bStillExists;
}

// ---- modify_asset_property -------------------------------------------------

FSageToolDispatch::FOutcome ModifyAssetPropertyOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString AssetPath, PropName;
    if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'asset_path'"));
    }
    if (!Args->TryGetStringField(TEXT("property"), PropName) || PropName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'property'"));
    }
    const TSharedPtr<FJsonValue> ValueField = Args->Values.FindRef(TEXT("value"));
    if (!ValueField.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    FSageToolDispatch::FOutcome SubErr;
    UEditorAssetSubsystem* Sub = GetAssetSubsystem(SubErr);
    if (Sub == nullptr) return SubErr;

    UObject* Asset = Sub->LoadAsset(AssetPath);
    if (Asset == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset not found: %s"), *AssetPath));
    }

    FProperty* Property = Asset->GetClass()->FindPropertyByName(*PropName);
    if (Property == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("property not found: %s on %s"),
                            *PropName, *Asset->GetClass()->GetName()));
    }

    if (PropName.Equals(TEXT("Skeleton"), ESearchCase::CaseSensitive))
    {
        if (UAnimationAsset* Anim = Cast<UAnimationAsset>(Asset))
        {
            const FString SkeletonPath = ExtractObjectPathString(ValueField);
            USkeleton* Skeleton = Cast<USkeleton>(ResolveAssetForDelete(SkeletonPath));
            if (!Skeleton)
            {
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("Skeleton write requires a USkeleton path; could not resolve: %s"), *SkeletonPath));
            }

            FScopedTransaction Tx(LOCTEXT("ModifyAnimationAssetSkeleton", "Sage: Modify Animation Asset Skeleton"));
            Anim->Modify();
            Anim->PreEditChange(Property);
            Anim->SetSkeleton(Skeleton);
            FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
            Anim->PostEditChangeProperty(ChangeEvent);
            Anim->MarkPackageDirty();

            USkeleton* Readback = Anim->GetSkeleton();
            if (Readback != Skeleton)
            {
                Tx.Cancel();
                return FSageToolDispatch::FOutcome::MakeError(-32603,
                    FString::Printf(TEXT("Skeleton write did not persist on readback for %s (requested=%s, readback=%s)"),
                                    *AssetPath,
                                    *Skeleton->GetPathName(),
                                    Readback ? *Readback->GetPathName() : TEXT("<null>")));
            }

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("asset_path"), AssetPath);
            Result->SetStringField(TEXT("property"), PropName);
            Result->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
            Result->SetStringField(TEXT("readback"), Skeleton->GetPathName());
            Result->SetBoolField(TEXT("used_animation_set_skeleton"), true);
            return FSageToolDispatch::FOutcome::MakeSuccess(Result);
        }
    }

    FScopedTransaction Tx(LOCTEXT("ModifyAssetProperty", "Sage: Modify Asset Property"));
    Asset->Modify();
    Asset->PreEditChange(Property);

    if (!detail::SetUPropertyFromJson(Asset, Property, ValueField))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unsupported property type for '%s' (got %s)"),
                            *PropName, *Property->GetClass()->GetName()));
    }

    FPropertyChangedEvent ChangeEvent(Property);
    Asset->PostEditChangeProperty(ChangeEvent);
    Asset->MarkPackageDirty();

    UE_LOG(LogSageBridge, Log, TEXT("Modified asset property '%s' on %s"),
           *PropName, *AssetPath);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("property"),   PropName);
    Result->SetField(TEXT("value"), ValueField);
    if (TSharedPtr<FJsonValue> Readback = detail::GetUPropertyAsJson(Asset, Property))
    {
        Result->SetField(TEXT("readback"), Readback);
    }
    if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
    {
        UObject* ClassObj = ClassProp->GetObjectPropertyValue(
            Property->ContainerPtrToValuePtr<void>(Asset));
        if (UClass* ReadbackClass = Cast<UClass>(ClassObj))
        {
            Result->SetStringField(TEXT("resolved_class"), ReadbackClass->GetPathName());
        }
        Result->SetStringField(TEXT("meta_class"), ClassProp->MetaClass
            ? ClassProp->MetaClass->GetPathName()
            : FString());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- rename_asset / move_asset (same UE call, different semantic intent) --

FSageToolDispatch::FOutcome RenameOrMoveOnGameThread(const TSharedPtr<FJsonObject>& Args,
                                                      const FText& TransactionLabel)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString SourcePath, DestPath;
    if (!Args->TryGetStringField(TEXT("source"), SourcePath) || SourcePath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'source'"));
    }
    if (!Args->TryGetStringField(TEXT("destination"), DestPath) || DestPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'destination'"));
    }
    bool bOverwrite = false;
    Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (bOverwrite && !bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("overwrite is destructive; pass confirmed:true with overwrite:true"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    FSageToolDispatch::FOutcome SubErr;
    UEditorAssetSubsystem* Sub = GetAssetSubsystem(SubErr);
    if (Sub == nullptr) return SubErr;

    if (!Sub->DoesAssetExist(SourcePath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("source asset not found: %s"), *SourcePath));
    }
    if (NormalizePackagePath(SourcePath).Equals(NormalizePackagePath(DestPath), ESearchCase::CaseSensitive))
    {
        auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("source"), SourcePath);
        Result->SetStringField(TEXT("destination"), DestPath);
        Result->SetBoolField(TEXT("already_at_destination"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(Result);
    }

    FScopedTransaction Tx(TransactionLabel);
    TSharedPtr<FJsonObject> OverwriteDeleteReport;
    const bool bDestExists = Sub->DoesAssetExist(DestPath)
        || Sub->DoesAssetExist(NormalizePackagePath(DestPath));
    if (bDestExists)
    {
        if (!bOverwrite)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("destination asset exists: %s (pass overwrite:true and confirmed:true to replace it)"), *DestPath));
        }
        if (!DeleteExistingDestinationForMove(Sub, DestPath, OverwriteDeleteReport))
        {
            Tx.Cancel();
            FString Reason = TEXT("failed to clear destination before overwrite");
            if (OverwriteDeleteReport.IsValid())
            {
                FString DetailReason;
                if (OverwriteDeleteReport->TryGetStringField(TEXT("reason"), DetailReason)
                    && !DetailReason.IsEmpty())
                {
                    Reason = FString::Printf(TEXT("%s: %s"), *Reason, *DetailReason);
                }
            }
            return FSageToolDispatch::FOutcome::MakeError(-32000,
                FString::Printf(TEXT("%s (%s -> %s)"), *Reason, *SourcePath, *DestPath));
        }
    }

    const bool bOk = Sub->RenameAsset(SourcePath, DestPath);
    if (!bOk)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("rename failed: %s -> %s"), *SourcePath, *DestPath));
    }

    UE_LOG(LogSageBridge, Log, TEXT("Renamed/moved asset: %s -> %s"), *SourcePath, *DestPath);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"),      SourcePath);
    Result->SetStringField(TEXT("destination"), DestPath);
    Result->SetBoolField(TEXT("overwrite"), bOverwrite);
    Result->SetBoolField(TEXT("destination_existed_before"), bDestExists);
    if (OverwriteDeleteReport.IsValid())
    {
        Result->SetObjectField(TEXT("overwrite_delete"), OverwriteDeleteReport);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

FSageToolDispatch::FOutcome RenameAssetOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    return RenameOrMoveOnGameThread(Args, LOCTEXT("RenameAsset", "Sage: Rename Asset"));
}

FSageToolDispatch::FOutcome MoveAssetOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    return RenameOrMoveOnGameThread(Args, LOCTEXT("MoveAsset", "Sage: Move Asset"));
}

// ---- duplicate_asset -------------------------------------------------------

FSageToolDispatch::FOutcome DuplicateAssetOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString SourcePath, DestPath;
    if (!Args->TryGetStringField(TEXT("source"), SourcePath) || SourcePath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'source'"));
    }
    if (!Args->TryGetStringField(TEXT("destination"), DestPath) || DestPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'destination'"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    FSageToolDispatch::FOutcome SubErr;
    UEditorAssetSubsystem* Sub = GetAssetSubsystem(SubErr);
    if (Sub == nullptr) return SubErr;

    if (!Sub->DoesAssetExist(SourcePath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("source asset not found: %s"), *SourcePath));
    }

    FScopedTransaction Tx(LOCTEXT("DuplicateAsset", "Sage: Duplicate Asset"));
    UObject* NewAsset = Sub->DuplicateAsset(SourcePath, DestPath);
    if (NewAsset == nullptr)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("duplicate failed: %s -> %s"), *SourcePath, *DestPath));
    }

    UE_LOG(LogSageBridge, Log, TEXT("Duplicated asset: %s -> %s"), *SourcePath, *DestPath);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"),       SourcePath);
    Result->SetStringField(TEXT("destination"),  DestPath);
    Result->SetStringField(TEXT("new_asset_id"), NewAsset->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- delete_asset ----------------------------------------------------------

FSageToolDispatch::FOutcome DeleteAssetOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString AssetPath;
    if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'asset_path'"));
    }

    bool bConfirmed = false;
    Args->TryGetBoolField(TEXT("confirmed"), bConfirmed);
    if (!bConfirmed)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("destructive operation; pass confirmed:true to proceed"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    FSageToolDispatch::FOutcome SubErr;
    UEditorAssetSubsystem* Sub = GetAssetSubsystem(SubErr);
    if (Sub == nullptr) return SubErr;

    if (!Sub->DoesAssetExist(AssetPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset not found: %s"), *AssetPath));
    }

    FScopedTransaction Tx(LOCTEXT("DeleteAsset", "Sage: Delete Asset"));
    const bool bOk = Sub->DeleteAsset(AssetPath);
    if (!bOk)
    {
        TSharedPtr<FJsonObject> Diag = DeleteDiagnosticJson(AssetPath, Sub);
        const FString Reason = DeleteFailureReason(Diag);
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("delete failed: %s (%s)"), *AssetPath, *Reason));
    }

    UE_LOG(LogSageBridge, Log, TEXT("Deleted asset: %s"), *AssetPath);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("deleted"), AssetPath);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- save_assets ----------------------------------------------------------

FSageToolDispatch::FOutcome SaveAssetsOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    bool bDryRun = false;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("dry_run"), bDryRun);

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    FSageToolDispatch::FOutcome SubErr;
    UEditorAssetSubsystem* Sub = GetAssetSubsystem(SubErr);
    if (Sub == nullptr) return SubErr;

    TArray<TSharedPtr<FJsonValue>> SavedArr;
    int32 SavedCount = 0;

    const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
    if (Args.IsValid() && Args->TryGetArrayField(TEXT("paths"), PathsArr) && PathsArr != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& V : *PathsArr)
        {
            const FString Path = V->AsString();
            if (Path.IsEmpty()) continue;
            if (bDryRun)
            {
                SavedArr.Add(MakeShared<FJsonValueString>(Path));
                continue;
            }
            if (Sub->SaveAsset(Path, /*bOnlyIfIsDirty=*/false))
            {
                SavedArr.Add(MakeShared<FJsonValueString>(Path));
                ++SavedCount;
            }
        }
    }
    else
    {
        TArray<UPackage*> DirtyPackages;
        FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
        FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);

        if (bDryRun)
        {
            for (UPackage* Pkg : DirtyPackages)
            {
                if (Pkg != nullptr)
                {
                    SavedArr.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
                }
            }
        }
        else
        {
            UEditorLoadingAndSavingUtils::SavePackages(DirtyPackages, /*bOnlyDirty=*/false);
            for (UPackage* Pkg : DirtyPackages)
            {
                if (Pkg != nullptr)
                {
                    SavedArr.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
                    ++SavedCount;
                }
            }
        }
    }

    UE_LOG(LogSageBridge, Log, TEXT("Saved %d package(s) (dry_run=%s)"),
           SavedCount, bDryRun ? TEXT("yes") : TEXT("no"));

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("saved"), SavedArr);
    Result->SetNumberField(TEXT("count"), SavedArr.Num());
    Result->SetBoolField(TEXT("dry_run"), bDryRun);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- get_dirty_assets -----------------------------------------------------

FSageToolDispatch::FOutcome GetDirtyAssetsOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    TArray<UPackage*> DirtyPackages;
    FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
    FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);

    TArray<TSharedPtr<FJsonValue>> Items;
    for (UPackage* Pkg : DirtyPackages)
    {
        if (Pkg != nullptr)
        {
            Items.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
        }
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("dirty"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- discard_changes ------------------------------------------------------

FSageToolDispatch::FOutcome DiscardChangesOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("paths"), PathsArr) || PathsArr == nullptr || PathsArr->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'paths' (non-empty array of asset/package paths)"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    TArray<UPackage*> Packages;
    Packages.Reserve(PathsArr->Num());
    TArray<FString> NotFound;
    for (const TSharedPtr<FJsonValue>& V : *PathsArr)
    {
        const FString Path = V->AsString();
        if (Path.IsEmpty()) continue;
        UPackage* Pkg = FindPackage(nullptr, *Path);
        if (Pkg == nullptr)
        {
            Pkg = LoadPackage(nullptr, *Path, LOAD_None);
        }
        if (Pkg != nullptr)
        {
            Packages.Add(Pkg);
        }
        else
        {
            NotFound.Add(Path);
        }
    }

    if (Packages.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("no resolvable packages from supplied paths"));
    }

    bool bAnyReloaded = false;
    FText ErrorMsg;
    UEditorLoadingAndSavingUtils::ReloadPackages(Packages, bAnyReloaded, ErrorMsg,
                                                  EReloadPackagesInteractionMode::AssumeNegative);

    TArray<TSharedPtr<FJsonValue>> ReloadedArr;
    if (bAnyReloaded)
    {
        // FEditorFileUtils::ReloadPackages reports a single bool, not per-package
        // status; treat success as "all attempted packages reloaded".
        for (UPackage* Pkg : Packages)
        {
            if (Pkg != nullptr) ReloadedArr.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
        }
    }
    TArray<TSharedPtr<FJsonValue>> NotFoundArr;
    for (const FString& P : NotFound)
    {
        NotFoundArr.Add(MakeShared<FJsonValueString>(P));
    }

    UE_LOG(LogSageBridge, Log, TEXT("Discard reloaded any=%s; %d not found"),
           bAnyReloaded ? TEXT("yes") : TEXT("no"), NotFound.Num());

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("reloaded"),  ReloadedArr);
    Result->SetArrayField(TEXT("not_found"), NotFoundArr);
    if (!ErrorMsg.IsEmpty())
    {
        Result->SetStringField(TEXT("error_message"), ErrorMsg.ToString());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- handlers --------------------------------------------------------------

FSageToolDispatch::FOutcome ModifyAssetPropertyHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return ModifyAssetPropertyOnGameThread(Args); });
}
FSageToolDispatch::FOutcome RenameAssetHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return RenameAssetOnGameThread(Args); });
}
FSageToolDispatch::FOutcome MoveAssetHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return MoveAssetOnGameThread(Args); });
}
FSageToolDispatch::FOutcome DuplicateAssetHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return DuplicateAssetOnGameThread(Args); });
}
FSageToolDispatch::FOutcome DeleteAssetHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return DeleteAssetOnGameThread(Args); });
}
FSageToolDispatch::FOutcome SaveAssetsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return SaveAssetsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome GetDirtyAssetsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetDirtyAssetsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome DiscardChangesHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return DiscardChangesOnGameThread(Args); });
}

}  // namespace

void RegisterAssetTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("modify_asset_property"), &ModifyAssetPropertyHandler);
    Dispatch.RegisterHandler(TEXT("rename_asset"),          &RenameAssetHandler);
    Dispatch.RegisterHandler(TEXT("move_asset"),            &MoveAssetHandler);
    Dispatch.RegisterHandler(TEXT("duplicate_asset"),       &DuplicateAssetHandler);
    Dispatch.RegisterHandler(TEXT("delete_asset"),          &DeleteAssetHandler);
    Dispatch.RegisterHandler(TEXT("save_assets"),           &SaveAssetsHandler);
    Dispatch.RegisterHandler(TEXT("get_dirty_assets"),      &GetDirtyAssetsHandler);
    Dispatch.RegisterHandler(TEXT("discard_changes"),       &DiscardChangesHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
