#include "Tools/SageTransactionTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "HAL/CriticalSection.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

// Process-wide transaction registry. Begin pushes a tx_id → UTransactor index
// mapping; Commit/Rollback consume one entry. UE's UTransactor stack is LIFO
// so callers must commit/rollback in reverse order of begin.
FCriticalSection& GetTxMutex()
{
    static FCriticalSection Mu;
    return Mu;
}

TMap<FString, int32>& GetActiveTransactions()
{
    static TMap<FString, int32> Map;
    return Map;
}

FString NewTxId()
{
    return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

// ---- begin_transaction ----------------------------------------------------

FSageToolDispatch::FOutcome BeginTransactionOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    FString Label;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("label"), Label);
    if (Label.IsEmpty()) Label = TEXT("Sage Transaction");

    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

    const FText LabelText = FText::FromString(Label);
    const int32 Index = GEditor->BeginTransaction(
        TEXT("Sage"), LabelText, /*PrimaryObject=*/nullptr);

    const FString TxId = NewTxId();
    {
        FScopeLock Lock(&GetTxMutex());
        GetActiveTransactions().Add(TxId, Index);
    }

    UE_LOG(LogSageBridge, Log, TEXT("BeginTransaction tx=%s index=%d label='%s'"),
           *TxId, Index, *Label);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("tx_id"), TxId);
    Result->SetStringField(TEXT("label"), Label);
    Result->SetNumberField(TEXT("index"), Index);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- commit_transaction ---------------------------------------------------

FSageToolDispatch::FOutcome CommitTransactionOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString TxId;
    if (!Args->TryGetStringField(TEXT("tx_id"), TxId) || TxId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'tx_id'"));
    }
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }

    int32 Index = INDEX_NONE;
    {
        FScopeLock Lock(&GetTxMutex());
        TMap<FString, int32>& Active = GetActiveTransactions();
        if (!Active.Contains(TxId))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("unknown tx_id: %s"), *TxId));
        }
        Index = Active[TxId];
        Active.Remove(TxId);
    }

    GEditor->EndTransaction();
    UE_LOG(LogSageBridge, Log, TEXT("CommitTransaction tx=%s index=%d"), *TxId, Index);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("tx_id"), TxId);
    Result->SetBoolField(TEXT("committed"), true);
    Result->SetNumberField(TEXT("index"), Index);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- rollback_transaction -------------------------------------------------

FSageToolDispatch::FOutcome RollbackTransactionOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    FString TxId;
    if (!Args->TryGetStringField(TEXT("tx_id"), TxId) || TxId.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'tx_id'"));
    }
    if (GEditor == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }

    int32 Index = INDEX_NONE;
    {
        FScopeLock Lock(&GetTxMutex());
        TMap<FString, int32>& Active = GetActiveTransactions();
        if (!Active.Contains(TxId))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("unknown tx_id: %s"), *TxId));
        }
        Index = Active[TxId];
        Active.Remove(TxId);
    }

    GEditor->CancelTransaction(Index);
    UE_LOG(LogSageBridge, Log, TEXT("RollbackTransaction tx=%s index=%d"), *TxId, Index);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("tx_id"), TxId);
    Result->SetBoolField(TEXT("rolled_back"), true);
    Result->SetNumberField(TEXT("index"), Index);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- get_active_transactions ----------------------------------------------

FSageToolDispatch::FOutcome GetActiveTransactionsOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    TArray<TSharedPtr<FJsonValue>> Items;
    {
        FScopeLock Lock(&GetTxMutex());
        for (const auto& [TxId, Index] : GetActiveTransactions())
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("tx_id"), TxId);
            Item->SetNumberField(TEXT("index"), Index);
            Items.Add(MakeShared<FJsonValueObject>(Item));
        }
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("transactions"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- handlers --------------------------------------------------------------

FSageToolDispatch::FOutcome BeginTransactionHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return BeginTransactionOnGameThread(Args); });
}
FSageToolDispatch::FOutcome CommitTransactionHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return CommitTransactionOnGameThread(Args); });
}
FSageToolDispatch::FOutcome RollbackTransactionHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return RollbackTransactionOnGameThread(Args); });
}
FSageToolDispatch::FOutcome GetActiveTransactionsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetActiveTransactionsOnGameThread(Args); });
}

}  // namespace

void RegisterTransactionTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("begin_transaction"),       &BeginTransactionHandler);
    Dispatch.RegisterHandler(TEXT("commit_transaction"),      &CommitTransactionHandler);
    Dispatch.RegisterHandler(TEXT("rollback_transaction"),    &RollbackTransactionHandler);
    Dispatch.RegisterHandler(TEXT("get_active_transactions"), &GetActiveTransactionsHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
