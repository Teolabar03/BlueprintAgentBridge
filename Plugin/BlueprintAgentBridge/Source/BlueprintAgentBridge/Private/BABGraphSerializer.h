// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/**
 * Turns graphs into the two representations agents consume: structured JSON (for tools)
 * and a compact text listing (for reading). Both use the same stable node ids, so an agent
 * can read the text and then target nodes and pins by name in follow-up operations.
 * The format is documented in docs/graph-format.md.
 */
namespace BAB
{
	struct FReadOptions
	{
		bool bIncludeHiddenPins = false;
		bool bIncludePositions = true;
		bool bIncludeGuids = false;
		bool bIncludePinTypes = true;
	};

	bool ShouldShowPin(const UEdGraphPin* Pin, const FReadOptions& Options);

	/** Returns the node kind (call_function, event, variable_get, ...) and fills detail fields. */
	FString DescribeNode(UEdGraphNode* Node, FJsonObject& OutDetails, FString& OutDetailText);

	TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin, const FReadOptions& Options);
	TSharedRef<FJsonObject> NodeToJson(UEdGraphNode* Node, const FReadOptions& Options);
	TSharedRef<FJsonObject> GraphToJson(UBlueprint* BP, UEdGraph* Graph, const FReadOptions& Options);

	FString NodeToText(UEdGraphNode* Node, const FReadOptions& Options);
	FString GraphToText(UBlueprint* BP, UEdGraph* Graph, const FReadOptions& Options);

	TSharedRef<FJsonObject> BlueprintToJson(UBlueprint* BP);
	FString BlueprintToText(UBlueprint* BP);

	/** Current default of a member variable: the staging string before the first compile, the class default object afterwards. */
	FString VariableDefaultString(UBlueprint* BP, const struct FBPVariableDescription& Var);
}
