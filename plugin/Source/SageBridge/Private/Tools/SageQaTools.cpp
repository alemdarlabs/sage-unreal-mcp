#include "Tools/SageQaTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

void EnsureEditorContextFilter()
{
    FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
    Framework.SetRequestedTestFilter(EAutomationTestFlags::EditorContext);
}

// ---- list_tests -----------------------------------------------------------

FSageToolDispatch::FOutcome ListTestsOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    EnsureEditorContextFilter();
    FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

    TArray<FAutomationTestInfo> Infos;
    Framework.GetValidTestNames(Infos);

    FString Filter;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("filter"), Filter);

    TArray<TSharedPtr<FJsonValue>> Items;
    Items.Reserve(Infos.Num());
    for (const FAutomationTestInfo& Info : Infos)
    {
        if (!Filter.IsEmpty() && !Info.GetTestName().Contains(Filter)) continue;
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"),         Info.GetTestName());
        Item->SetStringField(TEXT("display_name"), Info.GetDisplayName());
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("tests"),  Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    if (!Filter.IsEmpty()) Result->SetStringField(TEXT("filter"), Filter);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- run_tests ------------------------------------------------------------

FSageToolDispatch::FOutcome RunTestsOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    EnsureEditorContextFilter();
    FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

    TArray<FAutomationTestInfo> Infos;
    Framework.GetValidTestNames(Infos);

    FString Filter;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("filter"), Filter);

    TArray<TSharedPtr<FJsonValue>> Started;
    int32 SkippedCount = 0;
    for (const FAutomationTestInfo& Info : Infos)
    {
        if (!Filter.IsEmpty() && !Info.GetTestName().Contains(Filter))
        {
            ++SkippedCount;
            continue;
        }
        const FString TestName = Info.GetTestName();
        Framework.StartTestByName(TestName, /*RoleIndex=*/0);
        Started.Add(MakeShared<FJsonValueString>(TestName));
    }

    UE_LOG(LogSageBridge, Log, TEXT("Started %d test(s) (filter='%s', skipped %d)"),
           Started.Num(), *Filter, SkippedCount);

    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("started"),       Started);
    Result->SetNumberField(TEXT("count"),        Started.Num());
    Result->SetNumberField(TEXT("skipped"),      SkippedCount);
    Result->SetBoolField(TEXT("note_async"),     true);
    if (!Filter.IsEmpty()) Result->SetStringField(TEXT("filter"), Filter);
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- handlers --------------------------------------------------------------

FSageToolDispatch::FOutcome ListTestsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return ListTestsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome RunTestsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return RunTestsOnGameThread(Args); });
}

}  // namespace

void RegisterQaTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("list_tests"), &ListTestsHandler);
    Dispatch.RegisterHandler(TEXT("run_tests"),  &RunTestsHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
