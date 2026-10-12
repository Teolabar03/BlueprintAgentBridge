// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Result of a bridge operation: ok flag, JSON payload, errors and warnings.
 * Every operation returns one of these; nothing throws.
 */
struct FBABResult
{
	bool bOk = true;
	TSharedRef<FJsonObject> Data;
	TArray<FString> Errors;
	TArray<FString> Warnings;

	FBABResult() : Data(MakeShared<FJsonObject>()) {}

	static FBABResult Ok() { return FBABResult(); }
	static FBABResult Ok(const TSharedRef<FJsonObject>& InData) { FBABResult R; R.Data = InData; return R; }
	static FBABResult Fail(const FString& Message) { FBABResult R; R.bOk = false; R.Errors.Add(Message); return R; }

	FBABResult& Warn(const FString& Message) { Warnings.Add(Message); return *this; }
	FBABResult& Error(const FString& Message) { bOk = false; Errors.Add(Message); return *this; }

	/** Full response object: { ok, result, errors, warnings } */
	TSharedRef<FJsonObject> ToJson() const;
};

namespace BAB
{
	FString JsonToString(const TSharedRef<FJsonObject>& Object, bool bPretty = false);
	TSharedPtr<FJsonObject> JsonFromString(const FString& Text, FString& OutError);

	// Tolerant parameter accessors (numbers given as strings are accepted, and vice versa).
	bool HasField(const FJsonObject& Params, const TCHAR* Field);
	FString GetString(const FJsonObject& Params, const TCHAR* Field, const FString& Default = FString());
	bool GetBool(const FJsonObject& Params, const TCHAR* Field, bool Default);
	int32 GetInt(const FJsonObject& Params, const TCHAR* Field, int32 Default);
	double GetNumber(const FJsonObject& Params, const TCHAR* Field, double Default);
	TSharedPtr<FJsonObject> GetObject(const FJsonObject& Params, const TCHAR* Field);
	bool GetStringArray(const FJsonObject& Params, const TCHAR* Field, TArray<FString>& Out);
	/** Reads [x, y] or {"x":..,"y":..}. Returns false when absent or malformed. */
	bool GetVector2D(const FJsonObject& Params, const TCHAR* Field, FVector2D& Out);

	void SetStringArray(FJsonObject& Object, const TCHAR* Field, const TArray<FString>& Values);
	TSharedRef<FJsonObject> NewObject();
	TSharedPtr<FJsonValue> ObjectValue(const TSharedRef<FJsonObject>& Object);
	TSharedPtr<FJsonValue> StringValue(const FString& Value);
}
