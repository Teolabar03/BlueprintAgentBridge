// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UFunction;
class UBlueprintNodeSpawner;

/**
 * Creates K2 nodes from a small JSON spec. The "type" field selects the node kind and the
 * remaining fields depend on it; see docs/protocol.md for the full table.
 */
namespace BAB
{
	UEdGraphNode* CreateNodeFromSpec(UBlueprint* BP, UEdGraph* Graph, const FJsonObject& Spec, const FVector2D& Position, FString& OutError, TArray<FString>& OutWarnings);

	/** Finds an overridable event ("BeginPlay", "ReceiveBeginPlay", "Tick", "ActorBeginOverlap"...) on the parent class or an implemented interface. */
	UFunction* ResolveEventFunction(UBlueprint* BP, const FString& Name, const FString& ClassName, FString& OutError);

	/** Looks for a macro graph in the Blueprint itself, in the given library, or in the engine StandardMacros. */
	UEdGraph* FindMacroGraph(UBlueprint* BP, const FString& MacroName, const FString& LibraryPath, FString& OutError);

	/** Finds an action-database spawner by the signature string returned by bp_search_nodes. */
	UBlueprintNodeSpawner* FindSpawnerBySignature(const FString& Signature, UObject** OutOwner);

	/** Sets a reflected property on a node from a JSON value (class names and asset paths are resolved). */
	bool ApplyNodeProperty(UEdGraphNode* Node, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutError);

	TArray<FString> GetSupportedNodeTypes();
}
