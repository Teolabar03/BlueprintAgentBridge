// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "BABSettings.generated.h"

/** Editor Preferences > Plugins > Blueprint Agent Bridge */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Blueprint Agent Bridge"))
class UBABSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Start the local HTTP server when the editor starts (the -BABServer command line switch does the same). */
	UPROPERTY(config, EditAnywhere, Category = "Server")
	bool bAutoStartServer = false;

	/** TCP port of the local HTTP server (loopback only). */
	UPROPERTY(config, EditAnywhere, Category = "Server", meta = (ClampMin = 1024, ClampMax = 65535))
	int32 Port = 8765;

	virtual FName GetContainerName() const override { return FName(TEXT("Editor")); }
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
};
