#include "ToolDispatch/SageToolDispatch.h"
#include "SageBridge.h"

#include "Dom/JsonObject.h"

FSageToolDispatch::FOutcome FSageToolDispatch::FOutcome::MakeSuccess(TSharedPtr<FJsonObject> InResult)
{
    return FOutcome{true, MoveTemp(InResult), nullptr};
}

FSageToolDispatch::FOutcome FSageToolDispatch::FOutcome::MakeError(int32 Code, FString Message)
{
    auto Err = MakeShared<FJsonObject>();
    Err->SetNumberField(TEXT("code"), Code);
    Err->SetStringField(TEXT("message"), MoveTemp(Message));
    return FOutcome{false, nullptr, MoveTemp(Err)};
}

void FSageToolDispatch::RegisterHandler(const FString& ToolName, FHandler Handler)
{
    Handlers.Add(ToolName, MoveTemp(Handler));
    UE_LOG(LogSageBridge, Log, TEXT("Registered tool handler '%s'"), *ToolName);
}

bool FSageToolDispatch::HasHandler(const FString& ToolName) const
{
    return Handlers.Contains(ToolName);
}

FSageToolDispatch::FOutcome FSageToolDispatch::InvokeHandler(
    const FString& ToolName,
    const TSharedPtr<FJsonObject>& Args)
{
    const FHandler* Handler = Handlers.Find(ToolName);
    if (Handler == nullptr)
    {
        return FOutcome::MakeError(-32601,
            FString::Printf(TEXT("unknown tool: %s"), *ToolName));
    }
    return (*Handler)(Args);
}

bool FSageToolDispatch::HandleEnvelope(const TSharedRef<FJsonObject>& Envelope, FSendFn Send)
{
    FString Type;
    if (!Envelope->TryGetStringField(TEXT("type"), Type) || Type != TEXT("tool_call"))
    {
        return false;
    }

    FString TxId;
    FString Tool;
    Envelope->TryGetStringField(TEXT("tx_id"), TxId);
    Envelope->TryGetStringField(TEXT("tool"),  Tool);

    TSharedPtr<FJsonObject> Args;
    {
        const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
        if (Envelope->TryGetObjectField(TEXT("args"), ArgsPtr) && ArgsPtr != nullptr)
        {
            Args = *ArgsPtr;
        }
    }
    if (!Args.IsValid())
    {
        Args = MakeShared<FJsonObject>();
    }

    UE_LOG(LogSageBridge, Verbose, TEXT("Tool dispatch: '%s' tx=%s"), *Tool, *TxId);

    FOutcome Outcome;
    const FHandler* Handler = Handlers.Find(Tool);
    if (Handler == nullptr)
    {
        Outcome = FOutcome::MakeError(-32601,
                                       FString::Printf(TEXT("unknown tool: %s"), *Tool));
    }
    else
    {
        Outcome = (*Handler)(Args);
    }

    auto Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("type"),    TEXT("tool_result"));
    Reply->SetStringField(TEXT("tx_id"),   TxId);
    Reply->SetBoolField(TEXT("success"),   Outcome.bSuccess);
    if (Outcome.Result.IsValid())
    {
        Reply->SetObjectField(TEXT("result"), Outcome.Result);
    }
    if (Outcome.Error.IsValid())
    {
        Reply->SetObjectField(TEXT("error"), Outcome.Error);
    }
    Send(Reply);
    return true;
}
