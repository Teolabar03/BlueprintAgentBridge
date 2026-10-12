// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABOpsCommon.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CallFunction.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"

namespace BAB
{
	static TSharedPtr<FJsonObject> FindVariableJson(UBlueprint* BP, const FName VarName)
	{
		TSharedRef<FJsonObject> Summary = BlueprintToJson(BP);
		const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
		if (Summary->TryGetArrayField(TEXT("variables"), Variables) && Variables)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Variables)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Value->TryGetObject(Object) && Object && (*Object)->GetStringField(TEXT("name")) == VarName.ToString())
				{
					return *Object;
				}
			}
		}
		return nullptr;
	}

	static void ApplyVariableOptions(UBlueprint* BP, const FName VarName, const FJsonObject& Params, FBABResult& Result)
	{
		if (HasField(Params, TEXT("category")))
		{
			FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, VarName, nullptr, FText::FromString(GetString(Params, TEXT("category"))), true);
		}
		if (HasField(Params, TEXT("instance_editable")))
		{
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(BP, VarName, !GetBool(Params, TEXT("instance_editable"), true));
		}
		if (HasField(Params, TEXT("read_only")))
		{
			FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(BP, VarName, GetBool(Params, TEXT("read_only"), false));
		}
		if (HasField(Params, TEXT("expose_on_spawn")))
		{
			if (GetBool(Params, TEXT("expose_on_spawn"), false))
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
			else
				FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(BP, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn);
		}
		if (HasField(Params, TEXT("private")))
		{
			if (GetBool(Params, TEXT("private"), false))
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, VarName, nullptr, FBlueprintMetadata::MD_Private, TEXT("true"));
			else
				FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(BP, VarName, nullptr, FBlueprintMetadata::MD_Private);
		}
		if (HasField(Params, TEXT("tooltip")))
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, VarName, nullptr, FName(TEXT("tooltip")), GetString(Params, TEXT("tooltip")));
		}
		if (HasField(Params, TEXT("replicated")))
		{
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName);
			if (Index != INDEX_NONE)
			{
				BP->Modify();
				if (GetBool(Params, TEXT("replicated"), false)) BP->NewVariables[Index].PropertyFlags |= CPF_Net;
				else BP->NewVariables[Index].PropertyFlags &= ~CPF_Net;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			}
		}
	}

	static FString DefaultValueFromParams(const FJsonObject& Params, const FEdGraphPinType& Type)
	{
		const TSharedPtr<FJsonValue>* Value = Params.Values.Find(TEXT("default"));
		if (!Value || !Value->IsValid() || (*Value)->Type == EJson::Null) return FString();
		if ((*Value)->Type == EJson::String) return (*Value)->AsString();
		if ((*Value)->Type == EJson::Boolean) return (*Value)->AsBool() ? TEXT("true") : TEXT("false");
		const UStruct* StructHint = Type.PinCategory == UEdGraphSchema_K2::PC_Struct ? Cast<UStruct>(Type.PinSubCategoryObject.Get()) : nullptr;
		return JsonValueToUEString(*Value, StructHint);
	}

	/** Variable nodes that read or write VarName of BP, in BP itself and in every loaded Blueprint that depends on it. */
	static void CollectVariableUsages(UBlueprint* BP, const FName VarName, TArray<TSharedPtr<FJsonValue>>& OutUsages, int32& OutLocalUsages)
	{
		OutLocalUsages = 0;
		TArray<UBlueprint*> Owners;
		Owners.Add(BP);
		TArray<UBlueprint*> Dependents;
		FBlueprintEditorUtils::GetDependentBlueprints(BP, Dependents);
		for (UBlueprint* Dependent : Dependents)
		{
			if (Dependent && Dependent != BP) Owners.AddUnique(Dependent);
		}
		for (UBlueprint* Owner : Owners)
		{
			TArray<UEdGraph*> Graphs;
			GetAllGraphs(Owner, Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph) continue;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node);
					if (!VarNode || VarNode->GetVarName() != VarName) continue;
					if (Owner != BP)
					{
						// In another Blueprint the node must target a class derived from this Blueprint.
						UClass* Source = VarNode->GetVariableSourceClass();
						const bool bTargetsThisBlueprint = Source && ((BP->GeneratedClass && Source->IsChildOf(BP->GeneratedClass)) || (BP->SkeletonGeneratedClass && Source->IsChildOf(BP->SkeletonGeneratedClass)));
						if (!bTargetsThisBlueprint) continue;
					}
					else
					{
						OutLocalUsages++;
					}
					TSharedRef<FJsonObject> Usage = NewObject();
					Usage->SetStringField(TEXT("blueprint"), GetObjectPath(Owner));
					Usage->SetStringField(TEXT("node"), NodeId(Node));
					Usage->SetStringField(TEXT("title"), NodeTitle(Node));
					Usage->SetStringField(TEXT("graph"), Graph->GetName());
					OutUsages.Add(ObjectValue(Usage));
				}
			}
		}
	}

	static bool ParseParamList(const FJsonObject& Params, const TCHAR* Field, TArray<TTuple<FString, FEdGraphPinType, FString>>& Out, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Params.TryGetArrayField(FString(Field), Array) || !Array) return true;
		for (const TSharedPtr<FJsonValue>& Item : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Item.IsValid() || !Item->TryGetObject(Object) || !Object)
			{
				OutError = FString::Printf(TEXT("Entries of \"%s\" must be objects like {\"name\":\"Amount\",\"type\":\"float\"}."), Field);
				return false;
			}
			const FString Name = GetString(**Object, TEXT("name")).TrimStartAndEnd();
			FEdGraphPinType Type;
			FString TypeError;
			if (Name.IsEmpty() || !ParsePinType(GetString(**Object, TEXT("type")), Type, TypeError))
			{
				OutError = FString::Printf(TEXT("Invalid %s parameter \"%s\": %s"), Field, *Name, *TypeError);
				return false;
			}
			Out.Add(MakeTuple(Name, Type, DefaultValueFromParams(**Object, Type)));
		}
		return true;
	}
}

void BABRegisterOperations_Members()
{
	using namespace BAB;
	FBABDispatcher& D = FBABDispatcher::Get();

	D.Register(TEXT("bp_add_variable"), TEXT("Adds a member variable (name, type, default, category, instance_editable, read_only, replicated, expose_on_spawn, tooltip)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Name = GetString(Params, TEXT("name")).TrimStartAndEnd();
		if (Name.IsEmpty()) return FBABResult::Fail(TEXT("Missing variable \"name\"."));
		FEdGraphPinType Type;
		FString Error;
		if (!ParsePinType(GetString(Params, TEXT("type"), TEXT("float")), Type, Error)) return FBABResult::Fail(Error);
		const FString Default = DefaultValueFromParams(Params, Type);
		const FName VarName(*Name);
		if (!FBlueprintEditorUtils::AddMemberVariable(Ctx.Blueprint, VarName, Type, Default))
		{
			return FBABResult::Fail(FString::Printf(TEXT("Could not add variable \"%s\": a variable with that name already exists (here or in a parent class) or the name is invalid."), *Name));
		}
		FBABResult Result;
		ApplyVariableOptions(Ctx.Blueprint, VarName, Params, Result);
		if (TSharedPtr<FJsonObject> Variable = FindVariableJson(Ctx.Blueprint, VarName)) Result.Data->SetObjectField(TEXT("variable"), Variable);
		Result.Data->SetStringField(TEXT("name"), Name);
		Result.Data->SetStringField(TEXT("type"), PinTypeToString(Type));
		return Result;
	});

	D.Register(TEXT("bp_update_variable"), TEXT("Renames, retypes or changes the default/flags of a member variable."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FName VarName(*GetString(Params, TEXT("name")).TrimStartAndEnd());
		int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Ctx.Blueprint, VarName);
		if (Index == INDEX_NONE)
		{
			return FBABResult::Fail(FString::Printf(TEXT("Variable \"%s\" is not declared in this Blueprint (inherited variables cannot be edited here)."), *VarName.ToString()));
		}
		FBABResult Result;
		const FString NewName = GetString(Params, TEXT("new_name")).TrimStartAndEnd();
		if (!NewName.IsEmpty() && NewName != VarName.ToString())
		{
			FBlueprintEditorUtils::RenameMemberVariable(Ctx.Blueprint, VarName, FName(*NewName));
			VarName = FName(*NewName);
			Index = FBlueprintEditorUtils::FindNewVariableIndex(Ctx.Blueprint, VarName);
			if (Index == INDEX_NONE) return FBABResult::Fail(TEXT("Rename failed (name already in use?)."));
		}
		if (HasField(Params, TEXT("type")))
		{
			FEdGraphPinType Type;
			FString Error;
			if (!ParsePinType(GetString(Params, TEXT("type")), Type, Error)) return FBABResult::Fail(Error);
			FBlueprintEditorUtils::ChangeMemberVariableType(Ctx.Blueprint, VarName, Type);
			Index = FBlueprintEditorUtils::FindNewVariableIndex(Ctx.Blueprint, VarName);
		}
		if (HasField(Params, TEXT("default")) && Index != INDEX_NONE)
		{
			Ctx.Blueprint->Modify();
			Ctx.Blueprint->NewVariables[Index].DefaultValue = DefaultValueFromParams(Params, Ctx.Blueprint->NewVariables[Index].VarType);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);
		}
		ApplyVariableOptions(Ctx.Blueprint, VarName, Params, Result);
		if (TSharedPtr<FJsonObject> Variable = FindVariableJson(Ctx.Blueprint, VarName)) Result.Data->SetObjectField(TEXT("variable"), Variable);
		Result.Data->SetStringField(TEXT("name"), VarName.ToString());
		return Result;
	});

	D.Register(TEXT("bp_remove_variable"), TEXT("Removes a member variable. Fails if it is used by nodes unless force=true (then the nodes are removed too)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		const FName VarName(*GetString(Params, TEXT("name")).TrimStartAndEnd());
		if (FBlueprintEditorUtils::FindNewVariableIndex(Ctx.Blueprint, VarName) == INDEX_NONE)
		{
			return FBABResult::Fail(FString::Printf(TEXT("Variable \"%s\" is not declared in this Blueprint."), *VarName.ToString()));
		}
		TArray<TSharedPtr<FJsonValue>> Usages;
		int32 LocalUsages = 0;
		CollectVariableUsages(Ctx.Blueprint, VarName, Usages, LocalUsages);
		FBABResult Result;
		Result.Data->SetArrayField(TEXT("usages"), Usages);
		if (Usages.Num() > 0 && !GetBool(Params, TEXT("force"), false))
		{
			Result.Error(FString::Printf(TEXT("Variable \"%s\" is used by %d node(s), %d of them in other Blueprints (see usages). Pass force=true to remove the variable and the nodes in this Blueprint; nodes in other Blueprints will break."), *VarName.ToString(), Usages.Num(), Usages.Num() - LocalUsages));
			return Result;
		}
		FBlueprintEditorUtils::RemoveMemberVariable(Ctx.Blueprint, VarName);
		Result.Data->SetStringField(TEXT("removed"), VarName.ToString());
		Result.Data->SetNumberField(TEXT("removed_nodes"), LocalUsages);
		if (Usages.Num() > LocalUsages)
		{
			Result.Warn(FString::Printf(TEXT("%d node(s) in other Blueprints still reference the removed variable and will fail to compile."), Usages.Num() - LocalUsages));
		}
		return Result;
	});

	D.Register(TEXT("bp_add_local_variable"), TEXT("Adds a local variable to a function graph."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, true, Ctx, Failure)) return Failure;
		if (!FBlueprintEditorUtils::DoesSupportLocalVariables(Ctx.Graph))
		{
			return FBABResult::Fail(FString::Printf(TEXT("Graph %s does not support local variables (only function graphs do)."), *Ctx.Graph->GetName()));
		}
		const FString Name = GetString(Params, TEXT("name")).TrimStartAndEnd();
		if (Name.IsEmpty()) return FBABResult::Fail(TEXT("Missing variable \"name\"."));
		FEdGraphPinType Type;
		FString Error;
		if (!ParsePinType(GetString(Params, TEXT("type"), TEXT("float")), Type, Error)) return FBABResult::Fail(Error);
		if (!FBlueprintEditorUtils::AddLocalVariable(Ctx.Blueprint, Ctx.Graph, FName(*Name), Type, DefaultValueFromParams(Params, Type)))
		{
			return FBABResult::Fail(FString::Printf(TEXT("Could not add local variable \"%s\" (name in use?)."), *Name));
		}
		FBABResult Result;
		Result.Data->SetStringField(TEXT("name"), Name);
		Result.Data->SetStringField(TEXT("type"), PinTypeToString(Type));
		Result.Data->SetStringField(TEXT("graph"), Ctx.Graph->GetName());
		return Result;
	});

	D.Register(TEXT("bp_add_function"), TEXT("Creates a function graph with inputs/outputs (name, inputs:[{name,type}], outputs:[...], pure, category, tooltip, access)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		UBlueprint* BP = Ctx.Blueprint;
		FString Name = GetString(Params, TEXT("name")).TrimStartAndEnd();
		Name.ReplaceInline(TEXT(" "), TEXT(""));
		if (Name.IsEmpty()) return FBABResult::Fail(TEXT("Missing function \"name\"."));
		if (BP->BlueprintType == BPTYPE_MacroLibrary)
		{
			return FBABResult::Fail(TEXT("Macro libraries cannot contain functions."));
		}
		TArray<UEdGraph*> Graphs;
		GetAllGraphs(BP, Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return FBABResult::Fail(FString::Printf(TEXT("A graph named \"%s\" already exists."), *Name));
			}
		}
		if (UClass* Self = GetBlueprintSelfClass(BP))
		{
			if (Self->FindFunctionByName(FName(*Name)))
			{
				return FBABResult::Fail(FString::Printf(TEXT("A function or event named \"%s\" already exists (maybe inherited). Use another name or override it with an event."), *Name));
			}
		}

		TArray<TTuple<FString, FEdGraphPinType, FString>> Inputs, Outputs;
		FString Error;
		if (!ParseParamList(Params, TEXT("inputs"), Inputs, Error)) return FBABResult::Fail(Error);
		if (!ParseParamList(Params, TEXT("outputs"), Outputs, Error)) return FBABResult::Fail(Error);

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!NewGraph) return FBABResult::Fail(TEXT("CreateNewGraph failed."));
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated*/ true, nullptr);

		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : NewGraph->Nodes)
		{
			if ((Entry = Cast<UK2Node_FunctionEntry>(Node)) != nullptr) break;
		}
		if (!Entry) return FBABResult::Fail(TEXT("The new function graph has no entry node."));

		FBABResult Result;
		for (const auto& Input : Inputs)
		{
			UEdGraphPin* Pin = Entry->CreateUserDefinedPin(FName(*Input.Get<0>()), Input.Get<1>(), EGPD_Output, true);
			if (!Pin)
			{
				Result.Warn(FString::Printf(TEXT("Input \"%s\" could not be created."), *Input.Get<0>()));
				continue;
			}
			if (!Input.Get<2>().IsEmpty())
			{
				for (TSharedPtr<FUserPinInfo>& PinInfo : Entry->UserDefinedPins)
				{
					if (PinInfo.IsValid() && PinInfo->PinName == Pin->PinName)
					{
						Entry->ModifyUserDefinedPinDefaultValue(PinInfo, Input.Get<2>());
					}
				}
			}
		}
		UK2Node_FunctionResult* ResultNode = nullptr;
		if (Outputs.Num() > 0)
		{
			ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
			for (const auto& Output : Outputs)
			{
				if (ResultNode && !ResultNode->CreateUserDefinedPin(FName(*Output.Get<0>()), Output.Get<1>(), EGPD_Input, true))
				{
					Result.Warn(FString::Printf(TEXT("Output \"%s\" could not be created."), *Output.Get<0>()));
				}
			}
		}
		if (GetBool(Params, TEXT("pure"), false)) Entry->AddExtraFlags(FUNC_BlueprintPure);
		if (GetBool(Params, TEXT("const"), false)) Entry->AddExtraFlags(FUNC_Const);
		const FString Access = GetString(Params, TEXT("access"), TEXT("public")).ToLower();
		Entry->ClearExtraFlags(FUNC_Public | FUNC_Protected | FUNC_Private);
		if (Access == TEXT("protected")) Entry->AddExtraFlags(FUNC_Protected);
		else if (Access == TEXT("private")) Entry->AddExtraFlags(FUNC_Private);
		else Entry->AddExtraFlags(FUNC_Public);
		if (HasField(Params, TEXT("category"))) Entry->MetaData.Category = FText::FromString(GetString(Params, TEXT("category")));
		if (HasField(Params, TEXT("tooltip"))) Entry->MetaData.ToolTip = FText::FromString(GetString(Params, TEXT("tooltip")));
		if (GetBool(Params, TEXT("call_in_editor"), false)) Entry->MetaData.bCallInEditor = true;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		Result.Data->SetStringField(TEXT("graph"), NewGraph->GetName());
		Result.Data->SetStringField(TEXT("entry_node"), NodeId(Entry));
		if (ResultNode) Result.Data->SetStringField(TEXT("result_node"), NodeId(ResultNode));
		Result.Data->SetStringField(TEXT("text"), GraphToText(BP, NewGraph, FReadOptions()));
		return Result;
	});

	D.Register(TEXT("bp_remove_graph"), TEXT("Removes a function, macro or event dispatcher graph. Fails when it is still called unless force=true."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, true, Ctx, Failure)) return Failure;
		UBlueprint* BP = Ctx.Blueprint;
		UEdGraph* Graph = Ctx.Graph;
		if (!Graph->bAllowDeletion || GraphKind(BP, Graph) == TEXT("ubergraph"))
		{
			return FBABResult::Fail(FString::Printf(TEXT("Graph %s cannot be removed."), *Graph->GetName()));
		}
		TArray<TSharedPtr<FJsonValue>> Usages;
		auto Scan = [&](UBlueprint* Owner)
		{
			TArray<UEdGraph*> Graphs;
			GetAllGraphs(Owner, Graphs);
			for (UEdGraph* G : Graphs)
			{
				if (!G || G == Graph) continue;
				for (UEdGraphNode* Node : G->Nodes)
				{
					bool bUses = false;
					if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
					{
						bUses = Call->FunctionReference.GetMemberName() == Graph->GetFName() && (Owner != BP || Call->FunctionReference.IsSelfContext() || Call->FunctionReference.GetMemberParentClass() == BP->GeneratedClass || Call->FunctionReference.GetMemberParentClass() == BP->SkeletonGeneratedClass);
					}
					else if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
					{
						bUses = Macro->GetMacroGraph() == Graph;
					}
					if (bUses)
					{
						TSharedRef<FJsonObject> Usage = NewObject();
						Usage->SetStringField(TEXT("blueprint"), GetObjectPath(Owner));
						Usage->SetStringField(TEXT("graph"), G->GetName());
						Usage->SetStringField(TEXT("node"), NodeId(Node));
						Usages.Add(ObjectValue(Usage));
					}
				}
			}
		};
		Scan(BP);
		TArray<UBlueprint*> Dependents;
		FBlueprintEditorUtils::GetDependentBlueprints(BP, Dependents);
		for (UBlueprint* Dependent : Dependents) Scan(Dependent);

		FBABResult Result;
		Result.Data->SetArrayField(TEXT("usages"), Usages);
		if (Usages.Num() > 0 && !GetBool(Params, TEXT("force"), false))
		{
			Result.Error(FString::Printf(TEXT("Graph \"%s\" is used by %d node(s). Pass force=true to remove it anyway (those nodes will break)."), *Graph->GetName(), Usages.Num()));
			return Result;
		}
		const FString GraphName = Graph->GetName();
		FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::Default);
		Result.Data->SetStringField(TEXT("removed"), GraphName);
		return Result;
	});
}
