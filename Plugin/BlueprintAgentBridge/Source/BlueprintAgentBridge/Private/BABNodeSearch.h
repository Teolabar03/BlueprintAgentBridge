// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UBlueprint;
class UEdGraph;
class UEdGraphPin;

/**
 * Searches the editor action database (the same data that feeds the right-click node menu)
 * and returns ready-to-use node specs for bp_add_node.
 */
namespace BAB
{
	struct FSearchOptions
	{
		FString Query;
		int32 Limit = 25;
		bool bIncludePins = false;
		bool bContextSensitive = true;
		/** Optional filter: call_function, variable_get, variable_set, event, custom_event, node */
		FString KindFilter;
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UEdGraphPin* ContextPin = nullptr;
	};

	TArray<TSharedPtr<FJsonValue>> SearchNodes(const FSearchOptions& Options, int32& OutTotalMatches);
}
