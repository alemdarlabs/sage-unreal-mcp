#include "Tools/SageMaterialTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

// modify_material_parameter — auto-detects scalar (number) vs vector (array
// of 3-4 numbers, treated as FLinearColor).
FSageToolDispatch::FOutcome ModifyMaterialParameterOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString AssetPath, ParamName;
    if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'asset_path'"));
    }
    if (!Args->TryGetStringField(TEXT("parameter"), ParamName) || ParamName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'parameter'"));
    }
    const TSharedPtr<FJsonValue> ValueField = Args->Values.FindRef(TEXT("value"));
    if (!ValueField.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));
    }

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UEditorAssetSubsystem* AssetSub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (AssetSub == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("EditorAssetSubsystem unavailable"));
    }

    UObject* Asset = AssetSub->LoadAsset(AssetPath);
    UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Asset);
    if (MIC == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UMaterialInstanceConstant: %s"), *AssetPath));
    }

    FScopedTransaction Tx(LOCTEXT("ModifyMaterialParam", "Sage: Modify Material Parameter"));
    MIC->Modify();

    FString ValueKind;
    if (ValueField->Type == EJson::Number || ValueField->Type == EJson::Boolean)
    {
        const float Scalar = static_cast<float>(ValueField->AsNumber());
        UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(
            MIC, FName(*ParamName), Scalar);
        ValueKind = TEXT("scalar");
    }
    else if (ValueField->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
        if (Arr.Num() < 3)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("vector value requires 3 or 4 numeric elements"));
        }
        const FLinearColor Color(
            static_cast<float>(Arr[0]->AsNumber()),
            static_cast<float>(Arr[1]->AsNumber()),
            static_cast<float>(Arr[2]->AsNumber()),
            Arr.Num() >= 4 ? static_cast<float>(Arr[3]->AsNumber()) : 1.0f);
        UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(
            MIC, FName(*ParamName), Color);
        ValueKind = TEXT("vector");
    }
    else
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("'value' must be a number (scalar) or array (vector)"));
    }

    MIC->PostEditChange();
    MIC->MarkPackageDirty();

    UE_LOG(LogSageBridge, Log, TEXT("Set material %s parameter '%s' (%s) on %s"),
           *ValueKind, *ParamName, *ValueKind, *AssetPath);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("parameter"),  ParamName);
    Result->SetStringField(TEXT("value_kind"), ValueKind);
    Result->SetField(TEXT("value"), ValueField);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

FSageToolDispatch::FOutcome ModifyMaterialParameterHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return ModifyMaterialParameterOnGameThread(Args); });
}

}  // namespace

void RegisterMaterialTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("modify_material_parameter"),
                              &ModifyMaterialParameterHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
