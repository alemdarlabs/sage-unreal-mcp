#include "Tools/SageBulkTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

FSageToolDispatch::FOutcome BulkModifyOnGameThread(
    FSageToolDispatch& Dispatch,
    const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }

    const TArray<TSharedPtr<FJsonValue>>* OpsArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("operations"), OpsArr) || OpsArr == nullptr
        || OpsArr->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("'operations' must be a non-empty array of {tool, args} objects"));
    }

    bool bAtomic = true;
    Args->TryGetBoolField(TEXT("atomic"), bAtomic);

    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    auto AppendOpResult = [](TArray<TSharedPtr<FJsonValue>>& Results,
                              const FString& OpTool,
                              const FSageToolDispatch::FOutcome& Outcome)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("tool"),    OpTool);
        Item->SetBoolField(TEXT("success"),   Outcome.bSuccess);
        if (Outcome.Result.IsValid()) Item->SetObjectField(TEXT("result"), Outcome.Result);
        if (Outcome.Error.IsValid())  Item->SetObjectField(TEXT("error"),  Outcome.Error);
        Results.Add(MakeShared<FJsonValueObject>(Item));
    };

    auto ExtractOp = [](const TSharedPtr<FJsonValue>& OpVal,
                         FString& OutTool,
                         TSharedPtr<FJsonObject>& OutArgs) -> bool
    {
        const TSharedPtr<FJsonObject>* OpObjPtr = nullptr;
        if (!OpVal->TryGetObject(OpObjPtr) || OpObjPtr == nullptr) return false;
        const TSharedPtr<FJsonObject>& OpObj = *OpObjPtr;
        if (!OpObj->TryGetStringField(TEXT("tool"), OutTool) || OutTool.IsEmpty()) return false;
        // Recursion guard: bulk_modify must not call itself.
        if (OutTool == TEXT("bulk_modify")) return false;

        const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
        if (OpObj->TryGetObjectField(TEXT("args"), ArgsPtr) && ArgsPtr != nullptr)
        {
            OutArgs = *ArgsPtr;
        }
        else
        {
            OutArgs = MakeShared<FJsonObject>();
        }
        return true;
    };

    TArray<TSharedPtr<FJsonValue>> Results;
    Results.Reserve(OpsArr->Num());

    if (bAtomic)
    {
        FScopedTransaction Tx(LOCTEXT("BulkModify", "Sage: Bulk Modify"));

        for (int32 Index = 0; Index < OpsArr->Num(); ++Index)
        {
            FString OpTool;
            TSharedPtr<FJsonObject> OpArgs;
            if (!ExtractOp((*OpsArr)[Index], OpTool, OpArgs))
            {
                Tx.Cancel();
                return FSageToolDispatch::FOutcome::MakeError(-32602,
                    FString::Printf(TEXT("operations[%d]: invalid shape (must be "
                                          "{tool: string, args?: object}; "
                                          "'bulk_modify' nested calls forbidden)"),
                                     Index));
            }

            const auto Outcome = Dispatch.InvokeHandler(OpTool, OpArgs);
            AppendOpResult(Results, OpTool, Outcome);
            if (!Outcome.bSuccess)
            {
                Tx.Cancel();
                auto Err = MakeShared<FJsonObject>();
                Err->SetNumberField(TEXT("code"), -32000);
                Err->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("atomic bulk aborted at operations[%d] (%s)"),
                                     Index, *OpTool));
                Err->SetArrayField(TEXT("operations"), Results);
                return FSageToolDispatch::FOutcome{false, nullptr, Err};
            }
        }
    }
    else
    {
        for (int32 Index = 0; Index < OpsArr->Num(); ++Index)
        {
            FString OpTool;
            TSharedPtr<FJsonObject> OpArgs;
            if (!ExtractOp((*OpsArr)[Index], OpTool, OpArgs))
            {
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("tool"),    TEXT("?"));
                Item->SetBoolField(TEXT("success"),   false);
                auto Err = MakeShared<FJsonObject>();
                Err->SetNumberField(TEXT("code"), -32602);
                Err->SetStringField(TEXT("message"),
                    FString::Printf(TEXT("operations[%d] invalid shape"), Index));
                Item->SetObjectField(TEXT("error"), Err);
                Results.Add(MakeShared<FJsonValueObject>(Item));
                continue;
            }
            const auto Outcome = Dispatch.InvokeHandler(OpTool, OpArgs);
            AppendOpResult(Results, OpTool, Outcome);
        }
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("operations"), Results);
    Result->SetBoolField(TEXT("atomic"),       bAtomic);
    Result->SetNumberField(TEXT("count"),      Results.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

}  // namespace

void RegisterBulkTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("bulk_modify"),
        [&Dispatch](const TSharedPtr<FJsonObject>& Args) {
            return detail::RunOnGameThread(
                [&Dispatch, Args]() { return BulkModifyOnGameThread(Dispatch, Args); });
        });
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
