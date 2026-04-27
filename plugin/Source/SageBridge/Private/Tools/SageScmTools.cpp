#include "Tools/SageScmTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Modules/ModuleManager.h"
#include "SourceControlOperations.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

// ---- get_source_control_state ---------------------------------------------

FSageToolDispatch::FOutcome GetScmStateOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto Result = MakeShared<FJsonObject>();

    const bool bModuleLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("SourceControl"));
    Result->SetBoolField(TEXT("loaded"), bModuleLoaded);
    if (!bModuleLoaded)
    {
        return FSageToolDispatch::FOutcome::MakeSuccess(Result);
    }

    ISourceControlModule& SCM = ISourceControlModule::Get();
    Result->SetBoolField(TEXT("enabled"), SCM.IsEnabled());

    if (SCM.IsEnabled())
    {
        ISourceControlProvider& Provider = SCM.GetProvider();
        Result->SetStringField(TEXT("provider"),  Provider.GetName().ToString());
        Result->SetBoolField(TEXT("available"),   Provider.IsAvailable());
        Result->SetStringField(TEXT("status_text"), Provider.GetStatusText().ToString());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- checkout_files -------------------------------------------------------

FSageToolDispatch::FOutcome CheckoutFilesOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing args"));
    }
    const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
    if (!Args->TryGetArrayField(TEXT("paths"), PathsArr) || PathsArr == nullptr
        || PathsArr->Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("'paths' must be a non-empty array of file or asset paths"));
    }

    if (!FModuleManager::Get().IsModuleLoaded(TEXT("SourceControl")))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32005,
            TEXT("source control module not loaded"));
    }
    ISourceControlModule& SCM = ISourceControlModule::Get();
    if (!SCM.IsEnabled())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32005,
            TEXT("source control disabled in this project"));
    }
    ISourceControlProvider& Provider = SCM.GetProvider();
    if (!Provider.IsAvailable())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32005,
            FString::Printf(TEXT("source control provider unavailable: %s"),
                             *Provider.GetName().ToString()));
    }

    TArray<FString> Files;
    Files.Reserve(PathsArr->Num());
    for (const TSharedPtr<FJsonValue>& V : *PathsArr)
    {
        const FString P = V->AsString();
        if (!P.IsEmpty()) Files.Add(P);
    }

    TSharedRef<FCheckOut, ESPMode::ThreadSafe> CheckOutOp = ISourceControlOperation::Create<FCheckOut>();
    const ECommandResult::Type Cmd = Provider.Execute(CheckOutOp, Files);

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("provider"), Provider.GetName().ToString());
    Result->SetNumberField(TEXT("file_count"), Files.Num());
    Result->SetBoolField(TEXT("succeeded"), Cmd == ECommandResult::Succeeded);
    switch (Cmd)
    {
    case ECommandResult::Succeeded: Result->SetStringField(TEXT("status"), TEXT("succeeded")); break;
    case ECommandResult::Failed:    Result->SetStringField(TEXT("status"), TEXT("failed"));    break;
    case ECommandResult::Cancelled: Result->SetStringField(TEXT("status"), TEXT("cancelled")); break;
    }
    UE_LOG(LogSageBridge, Log, TEXT("Checkout: %d file(s), status=%d"), Files.Num(), int32(Cmd));
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- handlers --------------------------------------------------------------

FSageToolDispatch::FOutcome GetScmStateHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetScmStateOnGameThread(Args); });
}
FSageToolDispatch::FOutcome CheckoutFilesHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return CheckoutFilesOnGameThread(Args); });
}

}  // namespace

void RegisterScmTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("get_source_control_state"), &GetScmStateHandler);
    Dispatch.RegisterHandler(TEXT("checkout_files"),           &CheckoutFilesHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
