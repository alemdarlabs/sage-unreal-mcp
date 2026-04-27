#include "Tools/SageAssetTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Class.h"
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

    FScopedTransaction Tx(TransactionLabel);
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
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("delete failed: %s"), *AssetPath));
    }

    UE_LOG(LogSageBridge, Log, TEXT("Deleted asset: %s"), *AssetPath);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("deleted"), AssetPath);
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

}  // namespace

void RegisterAssetTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("modify_asset_property"), &ModifyAssetPropertyHandler);
    Dispatch.RegisterHandler(TEXT("rename_asset"),          &RenameAssetHandler);
    Dispatch.RegisterHandler(TEXT("move_asset"),            &MoveAssetHandler);
    Dispatch.RegisterHandler(TEXT("duplicate_asset"),       &DuplicateAssetHandler);
    Dispatch.RegisterHandler(TEXT("delete_asset"),          &DeleteAssetHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
