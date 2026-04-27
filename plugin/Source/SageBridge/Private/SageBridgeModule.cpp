#include "SageBridge.h"

DEFINE_LOG_CATEGORY(LogSageBridge);

void FSageBridgeModule::StartupModule()
{
    UE_LOG(LogSageBridge, Log, TEXT("SageBridge plugin started"));
}

void FSageBridgeModule::ShutdownModule()
{
    UE_LOG(LogSageBridge, Log, TEXT("SageBridge plugin shutting down"));
}

IMPLEMENT_MODULE(FSageBridgeModule, SageBridge)
