#include "SageBridgeSettings.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

USageBridgeSettings::USageBridgeSettings()
{
    CategoryName = TEXT("Plugins");
    ServerUrl = TEXT("ws://127.0.0.1:7778/bridge");
}

FString USageBridgeSettings::GetResolvedLabel() const
{
    FString CliLabel;
    if (FParse::Value(FCommandLine::Get(), TEXT("-SageMCPLabel="), CliLabel))
    {
        return CliLabel;
    }
    return EditorLabel;
}
