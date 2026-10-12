// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABDispatcher.h"
#include "BlueprintAgentBridgeModule.h"
#include "HAL/PlatformTime.h"

FBABDispatcher& FBABDispatcher::Get()
{
	static FBABDispatcher Instance;
	return Instance;
}

void FBABDispatcher::Register(const FString& Name, const FString& Description, FBABHandler Handler)
{
	FBABOperation Op;
	Op.Name = Name;
	Op.Description = Description;
	Op.Handler = MoveTemp(Handler);
	Operations.Add(Name, MoveTemp(Op));
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
		TSharedRef<FJsonObject> Entry = BAB::NewObject();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("description"), Operations[Name].Description);
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
	return Operation->Handler(Params);
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
