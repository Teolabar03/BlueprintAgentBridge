// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BABCommandlet.generated.h"

/**
 * Headless entry point. Examples:
 *   UnrealEditor-Cmd.exe Project.uproject -run=BlueprintAgentBridge -request=req.json -response=res.json
 *   UnrealEditor-Cmd.exe Project.uproject -run=BlueprintAgentBridge -serve -port=8765 [-idle=600]
 */
UCLASS()
class UBlueprintAgentBridgeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBlueprintAgentBridgeCommandlet();
	virtual int32 Main(const FString& Params) override;
};
