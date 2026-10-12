// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "HAL/IConsoleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBlueprintAgentBridge, Log, All);

class FBlueprintAgentBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnPostEngineInit();
	void StartServerCommand(const TArray<FString>& Args);
	void StopServerCommand();

	FDelegateHandle PostEngineInitHandle;
	TArray<IConsoleObject*> ConsoleCommands;
};
