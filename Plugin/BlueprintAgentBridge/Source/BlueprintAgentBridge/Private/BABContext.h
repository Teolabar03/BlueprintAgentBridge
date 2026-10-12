// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UFunction;
class UClass;
class UScriptStruct;
class UEnum;

/**
 * Resolution helpers shared by every operation: turning the strings an agent sends
 * (asset paths, graph names, short node ids, pin names, type names) into engine objects,
 * and the reverse (stable ids and readable type strings).
 */
namespace BAB
{
	// ---- Assets -------------------------------------------------------------------------
	void EnsureAssetRegistryScanned(const FString& Path);
	/** Accepts /Game/Folder/BP_Name, /Game/Folder/BP_Name.BP_Name, a .uasset file path or a bare asset name. */
	UBlueprint* FindBlueprint(const FString& PathOrName, FString& OutError);
	FString GetObjectPath(const UBlueprint* BP);
	FString GetPackageFilename(const UBlueprint* BP);
	bool CheckPackageWritable(const UBlueprint* BP, FString& OutReason);

	// ---- Graphs, nodes, pins -------------------------------------------------------------
	void GetAllGraphs(const UBlueprint* BP, TArray<UEdGraph*>& OutGraphs);
	/** Empty name resolves to the event graph. */
	UEdGraph* FindGraph(UBlueprint* BP, const FString& GraphName, FString& OutError);
	/** Graph may be null to search every graph of the Blueprint. Accepts a full GUID, a short id prefix or a unique node title. */
	UEdGraphNode* FindNode(UBlueprint* BP, UEdGraph* Graph, const FString& NodeId, FString& OutError);
	/** Direction is "", "in" or "out". The pin name may also carry a suffix: "then:out". */
	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, const FString& Direction, FString& OutError);
	/** Accepts {"node":..,"pin":..,"direction":..} or the string "NODEID.PinName". */
	UEdGraphPin* ResolvePinRef(UBlueprint* BP, UEdGraph* Graph, const TSharedPtr<FJsonValue>& Value, FString& OutError);

	/** Stable short id: first 8 hex digits of the node GUID. */
	FString NodeId(const UEdGraphNode* Node);
	FString GraphKind(const UBlueprint* BP, const UEdGraph* Graph);
	FString NodeTitle(const UEdGraphNode* Node);
	FString StatusToString(const UBlueprint* BP);
	FString BlueprintTypeToString(const UBlueprint* BP);

	// ---- Reflection lookups ---------------------------------------------------------------
	UClass* ResolveClass(const FString& Name, FString& OutError);
	UScriptStruct* ResolveStruct(const FString& Name, FString& OutError);
	UEnum* ResolveEnum(const FString& Name, FString& OutError);
	/** Spec is "Function", "Class.Function", "Class:Function" or "/Script/Module.Class:Function". */
	UFunction* ResolveFunction(UBlueprint* BP, const FString& Spec, const FString& ClassHint, FString& OutError);
	bool FunctionNameMatches(const UFunction* Function, const FString& Query);
	FString ClassDisplayName(const UClass* Class);
	FString FunctionDisplay(const UFunction* Function);
	/** Skeleton class when available, else generated class, else parent class. */
	UClass* GetBlueprintSelfClass(const UBlueprint* BP);

	// ---- Pin types --------------------------------------------------------------------------
	/** Grammar: bool, byte, int, int64, float, name, string, text, object:Actor, class:Actor, struct:Vector, enum:ECollisionChannel, array<T>, set<T>, map<K,V>, trailing & for by-reference. */
	bool ParsePinType(const FString& Text, FEdGraphPinType& Out, FString& OutError);
	FString PinTypeToString(const FEdGraphPinType& Type);

	// ---- Pin defaults -----------------------------------------------------------------------
	FString PinDefaultToString(const UEdGraphPin* Pin);
	/** Converts any JSON value to the text form the K2 schema expects for this pin. */
	FString JsonValueToPinDefault(const UEdGraphPin* Pin, const TSharedPtr<FJsonValue>& Value);
	/** Generic conversion to UE text import form; objects become (Key=Value,...). */
	FString JsonValueToUEString(const TSharedPtr<FJsonValue>& Value, const UStruct* StructHint);

	// ---- Modification -----------------------------------------------------------------------
	/** Marks the Blueprint modified, structurally when the node requires it. */
	void MarkModified(UBlueprint* BP, UEdGraphNode* Node = nullptr);
	/** Enables a disabled template ("ghost") node that new Blueprints contain and clears its placeholder comment. Returns true when something changed. */
	bool EnableGhostNode(UEdGraphNode* Node);
}
