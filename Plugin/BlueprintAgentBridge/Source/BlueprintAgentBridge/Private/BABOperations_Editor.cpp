// Copyright (c) 2026 Matteo La Barbera. MIT License.
//
// Operations that only make sense inside an interactive editor: undo / redo of the
// transactions the bridge created (see FBABDispatcher).

#include "BABOpsCommon.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Misc/CoreMisc.h"

namespace BAB
{
	static bool TransactionsAvailable(FString& OutError)
	{
		if (IsRunningCommandlet() || !GEditor || !GEditor->CanTransact())
		{
			OutError = TEXT("Undo and redo are only available inside a running editor (a headless session keeps no undo history).");
			return false;
		}
		return true;
	}

	static bool IsBridgeTransaction(const FText& Title)
	{
		return Title.ToString().StartsWith(FBABDispatcher::TransactionPrefix());
	}
}

void BABRegisterOperations_Editor()
{
	using namespace BAB;
	FBABDispatcher& D = FBABDispatcher::Get();

	D.Register(TEXT("bp_undo"), TEXT("Undoes the last editor transaction if the bridge made it (editor mode only). force=true undoes any transaction, including the user's own."), [](const FJsonObject& Params) -> FBABResult
	{
		FString Error;
		if (!TransactionsAvailable(Error)) return FBABResult::Fail(Error);
		const FTransactionContext Context = GEditor->Trans->GetUndoContext(true);
		if (Context.Title.IsEmpty())
		{
			return FBABResult::Fail(TEXT("Nothing to undo."));
		}
		if (!IsBridgeTransaction(Context.Title) && !GetBool(Params, TEXT("force"), false))
		{
			return FBABResult::Fail(FString::Printf(TEXT("The last transaction \"%s\" was not made by the bridge. Pass force=true to undo it anyway."), *Context.Title.ToString()));
		}
		if (!GEditor->UndoTransaction())
		{
			return FBABResult::Fail(TEXT("Undo failed (another editor operation may be in progress)."));
		}
		FBABResult Result;
		Result.Data->SetStringField(TEXT("undone"), Context.Title.ToString());
		const FTransactionContext Next = GEditor->Trans->GetUndoContext(true);
		if (!Next.Title.IsEmpty()) Result.Data->SetStringField(TEXT("next"), Next.Title.ToString());
		return Result;
	}, EBABOpKind::NonTransactional);

	D.Register(TEXT("bp_redo"), TEXT("Redoes the last undone editor transaction (editor mode only)."), [](const FJsonObject& Params) -> FBABResult
	{
		FString Error;
		if (!TransactionsAvailable(Error)) return FBABResult::Fail(Error);
		const FTransactionContext Context = GEditor->Trans->GetRedoContext();
		if (Context.Title.IsEmpty())
		{
			return FBABResult::Fail(TEXT("Nothing to redo."));
		}
		if (!IsBridgeTransaction(Context.Title) && !GetBool(Params, TEXT("force"), false))
		{
			return FBABResult::Fail(FString::Printf(TEXT("The transaction to redo, \"%s\", was not made by the bridge. Pass force=true to redo it anyway."), *Context.Title.ToString()));
		}
		if (!GEditor->RedoTransaction())
		{
			return FBABResult::Fail(TEXT("Redo failed (another editor operation may be in progress)."));
		}
		FBABResult Result;
		Result.Data->SetStringField(TEXT("redone"), Context.Title.ToString());
		return Result;
	}, EBABOpKind::NonTransactional);
}
