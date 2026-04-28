#include "SageBridge.h"
#include "Tools/SageDialogTools.h"

DEFINE_LOG_CATEGORY(LogSageBridge);

void FSageBridgeModule::StartupModule()
{
    UE_LOG(LogSageBridge, Log, TEXT("SageBridge plugin started"));
}

void FSageBridgeModule::ShutdownModule()
{
    // Defensive: hook is lazy-installed by editor.set_dialog_policy. If a
    // session installed it, unbind on shutdown to avoid a dangling delegate.
    sage::tools::RemoveDialogHook();
    UE_LOG(LogSageBridge, Log, TEXT("SageBridge plugin shutting down"));
}

IMPLEMENT_MODULE(FSageBridgeModule, SageBridge)
