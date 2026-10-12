// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "BABTypes.h"

using FBABHandler = TFunction<FBABResult(const FJsonObject& Params)>;

struct FBABOperation
{
	FString Name;
	FString Description;
	FBABHandler Handler;
};

/**
 * Registry of named operations ("bp_read_graph", ...). The HTTP server, the commandlet
 * and the automation tests all go through this single entry point, so every transport
 * behaves the same way.
 */
class FBABDispatcher
{
public:
	static FBABDispatcher& Get();

	void Register(const FString& Name, const FString& Description, FBABHandler Handler);
	bool HasOperation(const FString& Name) const { return Operations.Contains(Name); }
	TArray<FString> GetOperationNames() const;
	TSharedRef<FJsonObject> DescribeOperations() const;

	/** Runs one operation. Never throws; failures are reported inside the result. */
	FBABResult Dispatch(const FString& Op, const FJsonObject& Params);

	/**
	 * Handles a request object. Two shapes are accepted:
	 *   { "op": "bp_read_graph", "params": {...} }
	 *   { "batch": [ {"op":..,"params":..}, ... ], "stop_on_error": true }
	 */
	TSharedRef<FJsonObject> HandleRequest(const TSharedRef<FJsonObject>& Request);

	/** Parses text, handles the request and serializes the response. Always returns valid JSON. */
	FString HandleRequestText(const FString& RequestText, bool bPretty = false);

private:
	TSharedRef<FJsonObject> HandleSingle(const FJsonObject& Request);

	TMap<FString, FBABOperation> Operations;
};

/** Registers every built-in operation. Safe to call more than once. */
void BABRegisterAllOperations();
