#include "Tools/SageCompileTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "HAL/Platform.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

#define LOCTEXT_NAMESPACE "Sage"

namespace sage::tools
{
namespace
{

#if PLATFORM_WINDOWS
ILiveCodingModule* GetLiveCodingModule()
{
    return FModuleManager::GetModulePtr<ILiveCodingModule>(TEXT("LiveCoding"));
}
#endif

constexpr const TCHAR* PlatformName()
{
#if PLATFORM_WINDOWS
    return TEXT("windows");
#elif PLATFORM_MAC
    return TEXT("mac");
#elif PLATFORM_LINUX
    return TEXT("linux");
#else
    return TEXT("unknown");
#endif
}

// ---- get_live_coding_status -----------------------------------------------

FSageToolDispatch::FOutcome GetLiveCodingStatusOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("platform"), PlatformName());

#if PLATFORM_WINDOWS
    if (ILiveCodingModule* LC = GetLiveCodingModule())
    {
        Result->SetBoolField(TEXT("available"),               true);
        Result->SetBoolField(TEXT("enabled_by_default"),      LC->IsEnabledByDefault());
        Result->SetBoolField(TEXT("enabled_for_session"),     LC->IsEnabledForSession());
        Result->SetBoolField(TEXT("can_enable_for_session"),  LC->CanEnableForSession());
        Result->SetBoolField(TEXT("compiling"),               LC->IsCompiling());
        Result->SetBoolField(TEXT("auto_compile_new"),        LC->AutomaticallyCompileNewClasses());
        const FText EnableErr = LC->GetEnableErrorText();
        if (!EnableErr.IsEmpty())
        {
            Result->SetStringField(TEXT("enable_error"), EnableErr.ToString());
        }
    }
    else
    {
        Result->SetBoolField(TEXT("available"), false);
        Result->SetStringField(TEXT("reason"),
            TEXT("LiveCoding module not loaded (build without LiveCoding support?)"));
    }
#else
    Result->SetBoolField(TEXT("available"), false);
    Result->SetStringField(TEXT("reason"),
        TEXT("Live Coding is Windows-only in UE 5.7"));
#endif

    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
}

// ---- compile_and_reload ---------------------------------------------------

FSageToolDispatch::FOutcome CompileAndReloadOnGameThread(const TSharedPtr<FJsonObject>& /*Args*/)
{
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;

#if PLATFORM_WINDOWS
    ILiveCodingModule* LC = GetLiveCodingModule();
    if (LC == nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32007,
            TEXT("LiveCoding module not loaded"));
    }
    if (!LC->IsEnabledForSession())
    {
        if (LC->CanEnableForSession())
        {
            LC->EnableForSession(true);
            UE_LOG(LogSageBridge, Log, TEXT("Live Coding enabled for session"));
        }
        else
        {
            const FString Reason = LC->GetEnableErrorText().ToString();
            return FSageToolDispatch::FOutcome::MakeError(-32007,
                FString::Printf(TEXT("Live Coding cannot be enabled: %s"), *Reason));
        }
    }
    if (LC->IsCompiling())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            TEXT("Live Coding compile already in progress"));
    }

    // Async fire-and-forget. Completion + patch state observable via
    // get_live_coding_status polls. Phase 2 will add a server-side promise
    // backed by the LC completion delegate.
    LC->Compile();
    UE_LOG(LogSageBridge, Log, TEXT("Live Coding compile triggered"));

    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("strategy"),    TEXT("live_coding"));
    Result->SetBoolField(TEXT("compile_started"), true);
    Result->SetStringField(TEXT("platform"), PlatformName());
    return FSageToolDispatch::FOutcome::MakeSuccess(Result);
#else
    return FSageToolDispatch::FOutcome::MakeError(-32007,
        FString::Printf(TEXT("Live Coding unavailable on %s (Windows-only in UE 5.7); "
                              "full-restart orchestration arrives in Phase 2"),
                         PlatformName()));
#endif
}

// ---- handlers --------------------------------------------------------------

FSageToolDispatch::FOutcome GetLiveCodingStatusHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return GetLiveCodingStatusOnGameThread(Args); });
}
FSageToolDispatch::FOutcome CompileAndReloadHandler(const TSharedPtr<FJsonObject>& Args)
{
    return detail::RunOnGameThread([Args]() { return CompileAndReloadOnGameThread(Args); });
}

}  // namespace

void RegisterCompileTools(FSageToolDispatch& Dispatch)
{
    Dispatch.RegisterHandler(TEXT("get_live_coding_status"), &GetLiveCodingStatusHandler);
    Dispatch.RegisterHandler(TEXT("compile_and_reload"),     &CompileAndReloadHandler);
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
