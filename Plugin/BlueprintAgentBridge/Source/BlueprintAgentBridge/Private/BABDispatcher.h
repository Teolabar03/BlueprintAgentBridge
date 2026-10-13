// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "BABTypes.h"

using FBABHandler = TFunction<FBABResult(const FJsonObject& Params)>;

/** How the dispatcher treats an operation when it runs inside an interactive editor. */
enum class EBABOpKind : uint8
{
	/** Does not modify anything: no transaction, no warnings. */
	ReadOnly,
	/** Modifies graphs or members: wrapped in an editor transaction so the user can undo it (editor mode only). */
	Transactional,
	/** Modifies state that is not undoable (compile, save, asset creation): no transaction. */
	NonTransactional,
};

struct FBABOperation
{
	FString Name;
	FString Description;
	FBABHandler Handler;
	EBABOpKind Kind = EBABOpKind::Transactional;
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

	void Register(const FString& Name, const FString& Description, FBABHandler Handler, EBABOpKind Kind = EBABOpKind::Transactional);
	void SetKind(const FString& Name, EBABOpKind Kind);
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

	/** Prefix of the undo-history titles created by the bridge. */
	static const TCHAR* TransactionPrefix();

private:
	TSharedRef<FJsonObject> HandleSingle(const FJsonObject& Request);

	TMap<FString, FBABOperation> Operations;
	/** Transactional operations currently running (above 1 when one operation dispatches another). */
	int32 TransactionDepth = 0;
	/** Set when an operation dispatched inside another one fails. */
	bool bNestedFailure = false;
};

/** Registers every built-in operation. Safe to call more than once. */
void BABRegisterAllOperations();
