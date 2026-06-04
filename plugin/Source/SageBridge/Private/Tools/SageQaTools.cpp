#include "Tools/SageQaTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/FileHelper.h"
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

FSageToolDispatch::FOutcome ListAutomationTestsOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Out = ListTestsOnGameThread(Args);
    if (Out.bSuccess && Out.Result.IsValid())
    {
        Out.Result->SetStringField(TEXT("alias_of"), TEXT("list_tests"));
        Out.Result->SetStringField(TEXT("framework"), TEXT("AutomationTestFramework"));
    }
    return Out;
}

FSageToolDispatch::FOutcome RunAutomationTestsOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Out = RunTestsOnGameThread(Args);
    if (Out.bSuccess && Out.Result.IsValid())
    {
        Out.Result->SetStringField(TEXT("alias_of"), TEXT("run_tests"));
        Out.Result->SetStringField(TEXT("framework"), TEXT("AutomationTestFramework"));
    }
    return Out;
}

FSageToolDispatch::FOutcome RunVisualTestsOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    TSharedPtr<FJsonObject> EffectiveArgs = Args;
    FString Filter;
    if (!EffectiveArgs.IsValid() || !EffectiveArgs->TryGetStringField(TEXT("filter"), Filter) || Filter.IsEmpty())
    {
        EffectiveArgs = MakeShared<FJsonObject>();
        EffectiveArgs->SetStringField(TEXT("filter"), TEXT("Visual"));
        Filter = TEXT("Visual");
    }

    FSageToolDispatch::FOutcome Out = RunTestsOnGameThread(EffectiveArgs);
    if (Out.bSuccess && Out.Result.IsValid())
    {
        Out.Result->SetStringField(TEXT("tool"), TEXT("run_visual_tests"));
        Out.Result->SetStringField(TEXT("filter"), Filter);
        Out.Result->SetStringField(TEXT("framework"), TEXT("AutomationTestFramework"));
    }
    return Out;
}

FSageToolDispatch::FOutcome GetTestLogOnGameThread(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter = TEXT("Automation");
    int32 MaxLines = 200;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("filter"), Filter);
        double N = 0.0;
        if (Args->TryGetNumberField(TEXT("max_lines"), N) || Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxLines = FMath::Clamp(static_cast<int32>(N), 1, 5000);
        }
    }

    const FString LogFilePath = FPlatformOutputDevices::GetAbsoluteLogFilename();
    TArray<FString> Lines;
    if (!FFileHelper::LoadFileToStringArray(Lines, *LogFilePath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            FString::Printf(TEXT("could not read editor log: %s"), *LogFilePath));
    }

    TArray<TSharedPtr<FJsonValue>> Matches;
    for (int32 I = Lines.Num() - 1; I >= 0 && Matches.Num() < MaxLines; --I)
    {
        const bool bMatch = Filter.IsEmpty()
            || Lines[I].Contains(Filter, ESearchCase::IgnoreCase)
            || Lines[I].Contains(TEXT("Automation"), ESearchCase::IgnoreCase)
            || Lines[I].Contains(TEXT("Test"), ESearchCase::IgnoreCase);
        if (!bMatch) continue;

        auto Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("line"), I + 1);
        Row->SetStringField(TEXT("text"), Lines[I]);
        Matches.Insert(MakeShared<FJsonValueObject>(Row), 0);
    }

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("log_path"), LogFilePath);
    Result->SetStringField(TEXT("filter"), Filter);
    Result->SetArrayField(TEXT("entries"), Matches);
    Result->SetNumberField(TEXT("count"), Matches.Num());
    Result->SetNumberField(TEXT("max_lines"), MaxLines);
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
FSageToolDispatch::FOutcome ListAutomationTestsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return ListAutomationTestsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome RunAutomationTestsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return RunAutomationTestsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome RunVisualTestsHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return RunVisualTestsOnGameThread(Args); });
}
FSageToolDispatch::FOutcome GetTestLogHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetTestLogOnGameThread(Args); });
}

}  // namespace

void RegisterQaTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("list_tests"), &ListTestsHandler);
    Dispatch.RegisterHandler(TEXT("run_tests"),  &RunTestsHandler);
    Dispatch.RegisterHandler(TEXT("list_automation_tests"), &ListAutomationTestsHandler);
    Dispatch.RegisterHandler(TEXT("run_automation_tests"),  &RunAutomationTestsHandler);
    Dispatch.RegisterHandler(TEXT("run_visual_tests"),      &RunVisualTestsHandler);
    Dispatch.RegisterHandler(TEXT("get_test_log"),          &GetTestLogHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
