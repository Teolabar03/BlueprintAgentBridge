// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "BABTypes.h"
#include "BABContext.h"
#include "BABDispatcher.h"
#include "BABGraphSerializer.h"

class UBlueprint;
class UEdGraph;

namespace BAB
{
	struct FOpContext
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
	};

	/** Resolves "blueprint" (required) and "graph" (optional unless bRequireGraph). Returns a failed result on error. */
	inline bool ResolveContext(const FJsonObject& Params, bool bRequireGraph, FOpContext& Out, FBABResult& OutFailure)
	{
		FString Error;
		Out.Blueprint = FindBlueprint(GetString(Params, TEXT("blueprint")), Error);
		if (!Out.Blueprint)
		{
			OutFailure = FBABResult::Fail(Error);
			return false;
		}
		const FString GraphName = GetString(Params, TEXT("graph"));
		if (bRequireGraph || !GraphName.IsEmpty())
		{
			Out.Graph = FindGraph(Out.Blueprint, GraphName, Error);
			if (!Out.Graph)
			{
				OutFailure = FBABResult::Fail(Error);
				return false;
			}
		}
		return true;
	}

	inline FReadOptions ReadOptionsFromParams(const FJsonObject& Params)
	{
		FReadOptions Options;
		Options.bIncludeHiddenPins = GetBool(Params, TEXT("include_hidden_pins"), false);
		Options.bIncludePositions = GetBool(Params, TEXT("include_positions"), true);
		Options.bIncludeGuids = GetBool(Params, TEXT("include_guids"), false);
		Options.bIncludePinTypes = GetBool(Params, TEXT("include_pin_types"), true);
		return Options;
	}

	/** Adds a warning when the asset file is read-only, so the agent learns early that saving will fail. */
	inline void WarnIfNotWritable(UBlueprint* BP, FBABResult& Result)
	{
		FString Reason;
		if (BP && !CheckPackageWritable(BP, Reason))
		{
			Result.Warn(Reason);
		}
	}

	/** Compiles and fills Data with status, counts and messages. Shared by bp_compile and bp_save. */
	void CompileBlueprintInto(UBlueprint* BP, FJsonObject& Data);

	/** Saves the package to disk after the usual safety checks. */
	FBABResult SaveBlueprintPackage(UBlueprint* BP);
}
