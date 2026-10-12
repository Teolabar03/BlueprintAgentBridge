// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"

TSharedRef<FJsonObject> FBABResult::ToJson() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), bOk);
	Root->SetObjectField(TEXT("result"), Data);
	BAB::SetStringArray(*Root, TEXT("errors"), Errors);
	BAB::SetStringArray(*Root, TEXT("warnings"), Warnings);
	return Root;
}

namespace BAB
{
	FString JsonToString(const TSharedRef<FJsonObject>& Object, bool bPretty)
	{
		FString Out;
		if (bPretty)
		{
			TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
		}
		else
		{
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> JsonFromString(const FString& Text, FString& OutError)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid JSON: %s"), *Reader->GetErrorMessage());
			return nullptr;
		}
		return Object;
	}

	bool HasField(const FJsonObject& Params, const TCHAR* Field)
	{
		const TSharedPtr<FJsonValue>* Value = Params.Values.Find(Field);
		return Value && Value->IsValid() && (*Value)->Type != EJson::Null;
	}

	FString GetString(const FJsonObject& Params, const TCHAR* Field, const FString& Default)
	{
		const TSharedPtr<FJsonValue>* Value = Params.Values.Find(Field);
		if (!Value || !Value->IsValid())
		{
			return Default;
		}
		switch ((*Value)->Type)
		{
		case EJson::String: return (*Value)->AsString();
		case EJson::Number:
		{
			const double D = (*Value)->AsNumber();
			return FMath::IsNearlyEqual(D, FMath::RoundToDouble(D)) ? FString::Printf(TEXT("%lld"), (int64)D) : FString::SanitizeFloat(D);
		}
		case EJson::Boolean: return (*Value)->AsBool() ? TEXT("true") : TEXT("false");
		default: return Default;
		}
	}

	bool GetBool(const FJsonObject& Params, const TCHAR* Field, bool Default)
	{
		const TSharedPtr<FJsonValue>* Value = Params.Values.Find(Field);
		if (!Value || !Value->IsValid())
		{
			return Default;
		}
		switch ((*Value)->Type)
		{
		case EJson::Boolean: return (*Value)->AsBool();
		case EJson::Number: return (*Value)->AsNumber() != 0.0;
		case EJson::String:
		{
			const FString S = (*Value)->AsString().ToLower();
			if (S == TEXT("true") || S == TEXT("1") || S == TEXT("yes")) return true;
			if (S == TEXT("false") || S == TEXT("0") || S == TEXT("no")) return false;
			return Default;
		}
		default: return Default;
		}
	}

	int32 GetInt(const FJsonObject& Params, const TCHAR* Field, int32 Default)
	{
		return (int32)GetNumber(Params, Field, (double)Default);
	}

	double GetNumber(const FJsonObject& Params, const TCHAR* Field, double Default)
	{
		const TSharedPtr<FJsonValue>* Value = Params.Values.Find(Field);
		if (!Value || !Value->IsValid())
		{
			return Default;
		}
		switch ((*Value)->Type)
		{
		case EJson::Number: return (*Value)->AsNumber();
		case EJson::Boolean: return (*Value)->AsBool() ? 1.0 : 0.0;
		case EJson::String:
		{
			const FString S = (*Value)->AsString();
			return S.IsNumeric() ? FCString::Atod(*S) : Default;
		}
		default: return Default;
		}
	}

	TSharedPtr<FJsonObject> GetObject(const FJsonObject& Params, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Params.TryGetObjectField(FString(Field), Object) && Object)
		{
			return *Object;
		}
		return nullptr;
	}

	bool GetStringArray(const FJsonObject& Params, const TCHAR* Field, TArray<FString>& Out)
	{
		Out.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Params.TryGetArrayField(FString(Field), Array) && Array)
		{
			for (const TSharedPtr<FJsonValue>& V : *Array)
			{
				if (V.IsValid() && V->Type == EJson::String)
				{
					Out.Add(V->AsString());
				}
				else if (V.IsValid() && V->Type == EJson::Number)
				{
					Out.Add(FString::SanitizeFloat(V->AsNumber()));
				}
			}
			return true;
		}
		// Accept a single string as a one-element array.
		FString Single;
		if (Params.TryGetStringField(FString(Field), Single))
		{
			Out.Add(Single);
			return true;
		}
		return false;
	}

	bool GetVector2D(const FJsonObject& Params, const TCHAR* Field, FVector2D& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Params.TryGetArrayField(FString(Field), Array) && Array && Array->Num() >= 2)
		{
			Out.X = (*Array)[0]->AsNumber();
			Out.Y = (*Array)[1]->AsNumber();
			return true;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Params.TryGetObjectField(FString(Field), Object) && Object && Object->IsValid())
		{
			Out.X = GetNumber(**Object, TEXT("x"), 0.0);
			Out.Y = GetNumber(**Object, TEXT("y"), 0.0);
			return true;
		}
		return false;
	}

	void SetStringArray(FJsonObject& Object, const TCHAR* Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& V : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(V));
		}
		Object.SetArrayField(FString(Field), Array);
	}

	TSharedRef<FJsonObject> NewObject()
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonValue> ObjectValue(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedPtr<FJsonValue> StringValue(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}
}
