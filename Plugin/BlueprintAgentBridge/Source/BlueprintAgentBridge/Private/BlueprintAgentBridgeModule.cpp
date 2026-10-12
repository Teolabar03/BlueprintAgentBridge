// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BlueprintAgentBridgeModule.h"
#include "BABDispatcher.h"
#include "BABHttpServer.h"
#include "BABSettings.h"

#include "Misc/CoreDelegates.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/CoreMisc.h"

DEFINE_LOG_CATEGORY(LogBlueprintAgentBridge);

void FBlueprintAgentBridgeModule::StartupModule()
{
	BABRegisterAllOperations();

	if (!IsRunningCommandlet())
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FBlueprintAgentBridgeModule::OnPostEngineInit);

		ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BAB.StartServer"),
			TEXT("Starts the Blueprint Agent Bridge HTTP server. Usage: BAB.StartServer [port]"),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FBlueprintAgentBridgeModule::StartServerCommand)));
		ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BAB.StopServer"),
			TEXT("Stops the Blueprint Agent Bridge HTTP server."),
			FConsoleCommandDelegate::CreateRaw(this, &FBlueprintAgentBridgeModule::StopServerCommand)));
	}
}

void FBlueprintAgentBridgeModule::ShutdownModule()
{
	FBABHttpServer::Get().Stop();
	for (IConsoleObject* Command : ConsoleCommands)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Command);
	}
	ConsoleCommands.Empty();
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	}
}

void FBlueprintAgentBridgeModule::OnPostEngineInit()
{
	const UBABSettings* Settings = GetDefault<UBABSettings>();
	int32 Port = Settings ? Settings->Port : 8765;
	FParse::Value(FCommandLine::Get(), TEXT("-BABPort="), Port);
	const bool bStart = (Settings && Settings->bAutoStartServer) || FParse::Param(FCommandLine::Get(), TEXT("BABServer"));
	if (bStart)
	{
		FString Error;
		if (!FBABHttpServer::Get().Start((uint32)Port, Error))
		{
			UE_LOG(LogBlueprintAgentBridge, Error, TEXT("%s"), *Error);
		}
	}
}

void FBlueprintAgentBridgeModule::StartServerCommand(const TArray<FString>& Args)
{
	int32 Port = GetDefault<UBABSettings>() ? GetDefault<UBABSettings>()->Port : 8765;
	if (Args.Num() > 0)
	{
		Port = FCString::Atoi(*Args[0]);
	}
	FString Error;
	if (!FBABHttpServer::Get().Start((uint32)Port, Error))
	{
		UE_LOG(LogBlueprintAgentBridge, Error, TEXT("%s"), *Error);
	}
}

void FBlueprintAgentBridgeModule::StopServerCommand()
{
	FBABHttpServer::Get().Stop();
}

IMPLEMENT_MODULE(FBlueprintAgentBridgeModule, BlueprintAgentBridge)
