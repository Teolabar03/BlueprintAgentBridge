// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABDispatcher.h"
#include "BlueprintAgentBridgeModule.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreMisc.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Editor/Transactor.h"

#define LOCTEXT_NAMESPACE "BlueprintAgentBridge"

FBABDispatcher& FBABDispatcher::Get()
{
	static FBABDispatcher Instance;
	return Instance;
}

const TCHAR* FBABDispatcher::TransactionPrefix()
{
	return TEXT("Blueprint Agent Bridge: ");
}

void FBABDispatcher::Register(const FString& Name, const FString& Description, FBABHandler Handler, EBABOpKind Kind)
{
	FBABOperation Op;
	Op.Name = Name;
	Op.Description = Description;
	Op.Handler = MoveTemp(Handler);
	Op.Kind = Kind;
	Operations.Add(Name, MoveTemp(Op));
}

void FBABDispatcher::SetKind(const FString& Name, EBABOpKind Kind)
{
	if (FBABOperation* Op = Operations.Find(Name))
	{
		Op->Kind = Kind;
	}
}

TArray<FString> FBABDispatcher::GetOperationNames() const
{
	TArray<FString> Names;
	Operations.GetKeys(Names);
	Names.Sort();
	return Names;
}

TSharedRef<FJsonObject> FBABDispatcher::DescribeOperations() const
{
	TSharedRef<FJsonObject> Root = BAB::NewObject();
	TArray<TSharedPtr<FJsonValue>> Ops;
	for (const FString& Name : GetOperationNames())
	{
		const FBABOperation& Op = Operations[Name];
		TSharedRef<FJsonObject> Entry = BAB::NewObject();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("description"), Op.Description);
		Entry->SetStringField(TEXT("kind"), Op.Kind == EBABOpKind::ReadOnly ? TEXT("read") : (Op.Kind == EBABOpKind::Transactional ? TEXT("edit") : TEXT("other")));
		Ops.Add(BAB::ObjectValue(Entry));
	}
	Root->SetArrayField(TEXT("operations"), Ops);
	return Root;
}

FBABResult FBABDispatcher::Dispatch(const FString& Op, const FJsonObject& Params)
{
	const FBABOperation* Operation = Operations.Find(Op);
	if (!Operation)
	{
		return FBABResult::Fail(FString::Printf(TEXT("Unknown operation \"%s\". Known operations: %s"), *Op, *FString::Join(GetOperationNames(), TEXT(", "))));
	}

	const bool bInteractiveEditor = !IsRunningCommandlet() && GEditor != nullptr;

	FBABResult Result;
	if (Operation->Kind == EBABOpKind::Transactional && TransactionDepth > 0)
	{
		// Called from inside another operation: UE does not nest transactions, so this one shares the
		// outer transaction. A failure here makes the outer operation fail and undo everything.
		Result = Operation->Handler(Params);
		if (!Result.bOk)
		{
			bNestedFailure = true;
		}
	}
	else if (Operation->Kind == EBABOpKind::Transactional && GEditor && GEditor->Trans && GEditor->CanTransact())
	{
		// Every edit is one transaction (one undo step in a running editor). A failed edit is undone:
		// FScopedTransaction::Cancel only drops the record and keeps the changes already made, so the
		// transaction is closed and then undone without a redo entry. The headless editor has the
		// same transaction buffer, so this also keeps a failed edit from leaving a half-built graph.
		const FText Title = FText::Format(LOCTEXT("OpTransaction", "{0}{1}"), FText::FromString(TransactionPrefix()), FText::FromString(Op));
		const FGuid PreviousId = GEditor->Trans->GetUndoContext(false).TransactionId;
		{
			FScopedTransaction Transaction(Title);
			TGuardValue<int32> Depth(TransactionDepth, TransactionDepth + 1);
			bNestedFailure = false;
			Result = Operation->Handler(Params);
			if (bNestedFailure && Result.bOk)
			{
				Result.Error(TEXT("An operation run inside this one failed, so the whole operation was undone."));
			}
			bNestedFailure = false;
		}
		if (!Result.bOk)
		{
			const FTransactionContext Last = GEditor->Trans->GetUndoContext(false);
			if (Last.TransactionId != PreviousId && Last.Title.EqualTo(Title))
			{
				if (!GEditor->UndoTransaction(/*bCanRedo*/ false))
				{
					Result.Warn(TEXT("The operation failed and its partial changes could not be undone: read the graph before continuing."));
				}
			}
		}
	}
	else
	{
		Result = Operation->Handler(Params);
		if (!Result.bOk && Operation->Kind == EBABOpKind::Transactional)
		{
			Result.Warn(TEXT("No transaction buffer is available here, so partial changes of this failed operation were not undone: read the graph before continuing."));
		}
	}

	if (Operation->Kind != EBABOpKind::ReadOnly && bInteractiveEditor && GEditor->PlayWorld != nullptr)
	{
		Result.Warn(TEXT("Play In Editor is running: the change applies to the editor copy of the asset, stop PIE before compiling or saving."));
	}
	return Result;
}

TSharedRef<FJsonObject> FBABDispatcher::HandleSingle(const FJsonObject& Request)
{
	const FString Op = BAB::GetString(Request, TEXT("op"));
	const TSharedPtr<FJsonObject> Params = BAB::GetObject(Request, TEXT("params"));
	const FJsonObject EmptyParams;

	const double Start = FPlatformTime::Seconds();
	FBABResult Result;
	if (Op.IsEmpty())
	{
		Result = FBABResult::Fail(TEXT("Request is missing the \"op\" field"));
	}
	else
	{
		Result = Dispatch(Op, Params.IsValid() ? *Params : EmptyParams);
	}
	const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	TSharedRef<FJsonObject> Response = Result.ToJson();
	Response->SetStringField(TEXT("op"), Op);
	Response->SetNumberField(TEXT("elapsed_ms"), FMath::RoundToDouble(ElapsedMs * 10.0) / 10.0);
	if (const TSharedPtr<FJsonValue>* Id = Request.Values.Find(TEXT("id")))
	{
		Response->SetField(TEXT("id"), *Id);
	}

	if (!Result.bOk)
	{
		UE_LOG(LogBlueprintAgentBridge, Warning, TEXT("%s failed: %s"), *Op, *FString::Join(Result.Errors, TEXT(" | ")));
	}
	else
	{
		UE_LOG(LogBlueprintAgentBridge, Verbose, TEXT("%s ok (%.1f ms)"), *Op, ElapsedMs);
	}
	return Response;
}

TSharedRef<FJsonObject> FBABDispatcher::HandleRequest(const TSharedRef<FJsonObject>& Request)
{
	const TArray<TSharedPtr<FJsonValue>>* Batch = nullptr;
	if (Request->TryGetArrayField(TEXT("batch"), Batch) && Batch)
	{
		const bool bStopOnError = BAB::GetBool(*Request, TEXT("stop_on_error"), true);
		TSharedRef<FJsonObject> Response = BAB::NewObject();
		TArray<TSharedPtr<FJsonValue>> Results;
		bool bAllOk = true;
		for (const TSharedPtr<FJsonValue>& Item : *Batch)
		{
			const TSharedPtr<FJsonObject>* ItemObject = nullptr;
			if (!Item.IsValid() || !Item->TryGetObject(ItemObject) || !ItemObject || !ItemObject->IsValid())
			{
				FBABResult Bad = FBABResult::Fail(TEXT("Batch items must be objects with \"op\" and \"params\""));
				Results.Add(BAB::ObjectValue(Bad.ToJson()));
				bAllOk = false;
				if (bStopOnError) break;
				continue;
			}
			TSharedRef<FJsonObject> ItemResponse = HandleSingle(**ItemObject);
			const bool bItemOk = ItemResponse->GetBoolField(TEXT("ok"));
			bAllOk = bAllOk && bItemOk;
			Results.Add(BAB::ObjectValue(ItemResponse));
			if (!bItemOk && bStopOnError)
			{
				break;
			}
		}
		Response->SetBoolField(TEXT("ok"), bAllOk);
		Response->SetArrayField(TEXT("results"), Results);
		Response->SetNumberField(TEXT("completed"), Results.Num());
		Response->SetNumberField(TEXT("requested"), Batch->Num());
		return Response;
	}

	return HandleSingle(*Request);
}

FString FBABDispatcher::HandleRequestText(const FString& RequestText, bool bPretty)
{
	FString Error;
	TSharedPtr<FJsonObject> Request = BAB::JsonFromString(RequestText, Error);
	if (!Request.IsValid())
	{
		FBABResult Bad = FBABResult::Fail(Error);
		return BAB::JsonToString(Bad.ToJson(), bPretty);
	}
	return BAB::JsonToString(HandleRequest(Request.ToSharedRef()), bPretty);
}

#undef LOCTEXT_NAMESPACE
