// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABNodeSearch.h"
#include "BABContext.h"
#include "BABTypes.h"
#include "BABGraphSerializer.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintDelegateNodeSpawner.h"
#include "BlueprintBoundEventNodeSpawner.h"
#include "BlueprintComponentNodeSpawner.h"
#include "K2Node.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Variable.h"

namespace BAB
{
	struct FCandidate
	{
		UBlueprintNodeSpawner* Spawner = nullptr;
		UObject* Owner = nullptr;
		FString Kind;
		FString Primary;
		int32 Score = 0;
	};

	static FString FriendlyNodeClassName(const UClass* NodeClass)
	{
		if (!NodeClass) return FString();
		FString Name = NodeClass->GetName();
		Name.RemoveFromStart(TEXT("K2Node_"));
		static const TMap<FString, FString> Aliases = {
			{ TEXT("IfThenElse"), TEXT("Branch") },
			{ TEXT("ExecutionSequence"), TEXT("Sequence") },
			{ TEXT("DynamicCast"), TEXT("Cast To") },
			{ TEXT("ClassDynamicCast"), TEXT("Cast To Class") },
			{ TEXT("MakeStruct"), TEXT("Make Struct") },
			{ TEXT("BreakStruct"), TEXT("Break Struct") },
			{ TEXT("SetFieldsInStruct"), TEXT("Set Members In Struct") },
			{ TEXT("SwitchEnum"), TEXT("Switch On Enum") },
			{ TEXT("SwitchInteger"), TEXT("Switch On Int") },
			{ TEXT("SwitchString"), TEXT("Switch On String") },
			{ TEXT("SwitchName"), TEXT("Switch On Name") },
			{ TEXT("Knot"), TEXT("Reroute") },
			{ TEXT("Self"), TEXT("Get a reference to self") },
			{ TEXT("MakeArray"), TEXT("Make Array") },
			{ TEXT("MakeSet"), TEXT("Make Set") },
			{ TEXT("MakeMap"), TEXT("Make Map") },
			{ TEXT("SpawnActorFromClass"), TEXT("Spawn Actor from Class") },
			{ TEXT("Timeline"), TEXT("Add Timeline") },
			{ TEXT("CustomEvent"), TEXT("Add Custom Event") },
			{ TEXT("FormatText"), TEXT("Format Text") },
			{ TEXT("GetClassDefaults"), TEXT("Get Class Defaults") },
			{ TEXT("MultiGate"), TEXT("MultiGate") },
			{ TEXT("DoOnceMultiInput"), TEXT("Do Once MultiInput") },
			{ TEXT("Select"), TEXT("Select") },
			{ TEXT("MacroInstance"), TEXT("Macro") },
			{ TEXT("GetArrayItem"), TEXT("Get (array item)") },
			{ TEXT("EnumLiteral"), TEXT("Enum Literal") },
			{ TEXT("InputKey"), TEXT("Input Key Event") },
			{ TEXT("InputAction"), TEXT("Input Action Event") },
			{ TEXT("CreateDelegate"), TEXT("Create Event") },
			{ TEXT("LoadAsset"), TEXT("Async Load Asset") },
		};
		if (const FString* Alias = Aliases.Find(Name)) return *Alias;
		return Name;
	}

	static void BuildCheapText(UBlueprintNodeSpawner* Spawner, UObject* Owner, FString& OutKind, FString& OutPrimary, FString& OutSearchText)
	{
		OutKind = TEXT("node");
		if (UBlueprintFunctionNodeSpawner* FunctionSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
		{
			if (const UFunction* Function = FunctionSpawner->GetFunction())
			{
				OutKind = TEXT("call_function");
				OutPrimary = Function->GetName();
				OutSearchText = FString::Printf(TEXT("%s %s %s %s %s"), *Function->GetName(), *Function->GetDisplayNameText().ToString(), *ClassDisplayName(Function->GetOwnerClass()), *Function->GetMetaData(TEXT("Keywords")), *Function->GetMetaData(TEXT("Category")));
				return;
			}
		}
		if (UBlueprintVariableNodeSpawner* VariableSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
		{
			OutKind = Spawner->NodeClass && Spawner->NodeClass->IsChildOf(UK2Node_VariableSet::StaticClass()) ? TEXT("variable_set") : TEXT("variable_get");
			if (const FProperty* Property = VariableSpawner->GetVarProperty())
			{
				OutPrimary = Property->GetName();
				OutSearchText = FString::Printf(TEXT("%s %s %s %s"), OutKind == TEXT("variable_set") ? TEXT("set") : TEXT("get"), *Property->GetName(), *Property->GetDisplayNameText().ToString(), Property->GetOwnerClass() ? *ClassDisplayName(Property->GetOwnerClass()) : TEXT(""));
			}
			else
			{
				OutPrimary = Spawner->DefaultMenuSignature.MenuName.ToString();
				OutSearchText = OutPrimary;
			}
			return;
		}
		if (UBlueprintEventNodeSpawner* EventSpawner = Cast<UBlueprintEventNodeSpawner>(Spawner))
		{
			if (const UFunction* EventFunction = EventSpawner->GetEventFunction())
			{
				OutKind = TEXT("event");
				OutPrimary = EventFunction->GetName();
				FString Short = EventFunction->GetName();
				Short.RemoveFromStart(TEXT("Receive"));
				OutSearchText = FString::Printf(TEXT("event %s %s %s %s"), *EventFunction->GetName(), *Short, *EventFunction->GetDisplayNameText().ToString(), *ClassDisplayName(EventFunction->GetOwnerClass()));
			}
			else
			{
				OutKind = TEXT("custom_event");
				OutPrimary = TEXT("Custom Event");
				OutSearchText = TEXT("custom event add custom event");
			}
			return;
		}
		if (UBlueprintDelegateNodeSpawner* DelegateSpawner = Cast<UBlueprintDelegateNodeSpawner>(Spawner))
		{
			OutKind = TEXT("delegate");
			const FMulticastDelegateProperty* Delegate = DelegateSpawner->GetDelegateProperty();
			OutPrimary = Delegate ? Delegate->GetName() : FriendlyNodeClassName(Spawner->NodeClass);
			OutSearchText = FString::Printf(TEXT("%s %s delegate event dispatcher %s"), *OutPrimary, *FriendlyNodeClassName(Spawner->NodeClass), Delegate && Delegate->GetOwnerClass() ? *ClassDisplayName(Delegate->GetOwnerClass()) : TEXT(""));
			return;
		}
		if (Spawner->IsA<UBlueprintBoundEventNodeSpawner>())
		{
			OutKind = TEXT("bound_event");
		}
		else if (Spawner->IsA<UBlueprintComponentNodeSpawner>())
		{
			OutKind = TEXT("component");
		}
		OutPrimary = Spawner->DefaultMenuSignature.MenuName.ToString();
		if (OutPrimary.IsEmpty())
		{
			OutPrimary = FriendlyNodeClassName(Spawner->NodeClass);
		}
		FString Signature = Spawner->GetSpawnerSignature().ToString();
		Signature.ReplaceInline(TEXT("/"), TEXT(" "));
		Signature.ReplaceInline(TEXT("."), TEXT(" "));
		Signature.ReplaceInline(TEXT("="), TEXT(" "));
		Signature.ReplaceInline(TEXT(";"), TEXT(" "));
		Signature.ReplaceInline(TEXT("\""), TEXT(" "));
		OutSearchText = FString::Printf(TEXT("%s %s %s %s"), *OutPrimary, *FriendlyNodeClassName(Spawner->NodeClass), Spawner->NodeClass ? *Spawner->NodeClass->GetName() : TEXT(""), *Signature);
	}

	static int32 ScoreMatch(const TArray<FString>& Words, const FString& QueryLower, const FString& Primary, const FString& SearchText)
	{
		const FString TextLower = SearchText.ToLower();
		for (const FString& Word : Words)
		{
			if (!TextLower.Contains(Word)) return 0;
		}
		int32 Score = 10;
		const FString PrimaryCompact = Primary.ToLower().Replace(TEXT(" "), TEXT(""));
		const FString QueryCompact = QueryLower.Replace(TEXT(" "), TEXT(""));
		if (PrimaryCompact == QueryCompact) Score += 120;
		else if (PrimaryCompact.StartsWith(QueryCompact)) Score += 70;
		else if (PrimaryCompact.Contains(QueryCompact)) Score += 45;
		else
		{
			for (const FString& Word : Words)
			{
				if (PrimaryCompact.Contains(Word)) Score += 12;
			}
		}
		Score -= FMath::Min(Primary.Len() / 6, 12);
		return Score;
	}

	static TSharedRef<FJsonObject> BuildAddNodeSpec(const FCandidate& Candidate, UEdGraphNode* TemplateNode)
	{
		TSharedRef<FJsonObject> Spec = NewObject();
		if (Candidate.Kind == TEXT("call_function"))
		{
			const UFunction* Function = CastChecked<UBlueprintFunctionNodeSpawner>(Candidate.Spawner)->GetFunction();
			Spec->SetStringField(TEXT("type"), TEXT("call_function"));
			Spec->SetStringField(TEXT("function"), FunctionDisplay(Function));
			return Spec;
		}
		if (Candidate.Kind == TEXT("variable_get") || Candidate.Kind == TEXT("variable_set"))
		{
			Spec->SetStringField(TEXT("type"), Candidate.Kind);
			Spec->SetStringField(TEXT("variable"), Candidate.Primary);
			if (const FProperty* Property = CastChecked<UBlueprintVariableNodeSpawner>(Candidate.Spawner)->GetVarProperty())
			{
				if (UClass* OwnerClass = Property->GetOwnerClass())
				{
					if (!UBlueprint::GetBlueprintFromClass(OwnerClass)) Spec->SetStringField(TEXT("class"), ClassDisplayName(OwnerClass));
				}
			}
			return Spec;
		}
		if (Candidate.Kind == TEXT("event"))
		{
			const UFunction* Function = CastChecked<UBlueprintEventNodeSpawner>(Candidate.Spawner)->GetEventFunction();
			Spec->SetStringField(TEXT("type"), TEXT("event"));
			Spec->SetStringField(TEXT("name"), Function->GetName());
			Spec->SetStringField(TEXT("class"), ClassDisplayName(Function->GetOwnerClass()));
			return Spec;
		}
		if (Candidate.Kind == TEXT("custom_event"))
		{
			Spec->SetStringField(TEXT("type"), TEXT("custom_event"));
			Spec->SetStringField(TEXT("name"), TEXT("MyEvent"));
			return Spec;
		}
		// Generic node: prefer a friendly spec when the template tells us enough.
		if (TemplateNode)
		{
			if (UK2Node_DynamicCast* Cast_ = Cast<UK2Node_DynamicCast>(TemplateNode))
			{
				Spec->SetStringField(TEXT("type"), TEXT("cast"));
				Spec->SetStringField(TEXT("class"), ClassDisplayName(Cast_->TargetType));
				return Spec;
			}
			if (UK2Node_MakeStruct* Make = Cast<UK2Node_MakeStruct>(TemplateNode))
			{
				Spec->SetStringField(TEXT("type"), TEXT("make_struct"));
				Spec->SetStringField(TEXT("struct"), Make->StructType ? Make->StructType->GetName() : TEXT(""));
				return Spec;
			}
			if (UK2Node_BreakStruct* Break = Cast<UK2Node_BreakStruct>(TemplateNode))
			{
				Spec->SetStringField(TEXT("type"), TEXT("break_struct"));
				Spec->SetStringField(TEXT("struct"), Break->StructType ? Break->StructType->GetName() : TEXT(""));
				return Spec;
			}
			if (UK2Node_SwitchEnum* Switch = Cast<UK2Node_SwitchEnum>(TemplateNode))
			{
				Spec->SetStringField(TEXT("type"), TEXT("switch_enum"));
				Spec->SetStringField(TEXT("enum"), Switch->Enum ? Switch->Enum->GetName() : TEXT(""));
				return Spec;
			}
			if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(TemplateNode))
			{
				Spec->SetStringField(TEXT("type"), TEXT("macro"));
				if (UEdGraph* MacroGraph = Macro->GetMacroGraph())
				{
					Spec->SetStringField(TEXT("macro"), MacroGraph->GetName());
					Spec->SetStringField(TEXT("library"), MacroGraph->GetOutermost()->GetName());
				}
				return Spec;
			}
			if (TemplateNode->IsA<UK2Node_IfThenElse>())
			{
				Spec->SetStringField(TEXT("type"), TEXT("branch"));
				return Spec;
			}
			if (TemplateNode->GetClass() == UK2Node_ExecutionSequence::StaticClass())
			{
				Spec->SetStringField(TEXT("type"), TEXT("sequence"));
				return Spec;
			}
		}
		Spec->SetStringField(TEXT("type"), TEXT("spawner"));
		Spec->SetStringField(TEXT("signature"), Candidate.Spawner->GetSpawnerSignature().ToString());
		return Spec;
	}

	TArray<TSharedPtr<FJsonValue>> SearchNodes(const FSearchOptions& Options, int32& OutTotalMatches)
	{
		TArray<TSharedPtr<FJsonValue>> Results;
		OutTotalMatches = 0;

		const FString QueryLower = Options.Query.TrimStartAndEnd().ToLower();
		TArray<FString> Words;
		QueryLower.ParseIntoArrayWS(Words);
		if (Words.Num() == 0)
		{
			return Results;
		}

		FBlueprintActionDatabase& Database = FBlueprintActionDatabase::Get();
		const FBlueprintActionDatabase::FActionRegistry& Registry = Database.GetAllActions();

		TArray<FCandidate> Candidates;
		for (const auto& Pair : Registry)
		{
			UObject* Owner = Pair.Key.ResolveObjectPtr();
			for (UBlueprintNodeSpawner* Spawner : Pair.Value)
			{
				if (!Spawner) continue;
				FCandidate Candidate;
				Candidate.Spawner = Spawner;
				Candidate.Owner = Owner;
				FString SearchText;
				BuildCheapText(Spawner, Owner, Candidate.Kind, Candidate.Primary, SearchText);
				if (!Options.KindFilter.IsEmpty() && !Candidate.Kind.Equals(Options.KindFilter, ESearchCase::IgnoreCase)) continue;
				Candidate.Score = ScoreMatch(Words, QueryLower, Candidate.Primary, SearchText);
				if (Candidate.Score > 0)
				{
					Candidates.Add(Candidate);
				}
			}
		}
		OutTotalMatches = Candidates.Num();
		Candidates.Sort([](const FCandidate& A, const FCandidate& B)
		{
			if (A.Score != B.Score) return A.Score > B.Score;
			return A.Primary < B.Primary;
		});

		FBlueprintActionFilter Filter;
		const bool bUseFilter = Options.bContextSensitive && Options.Blueprint != nullptr;
		if (Options.Blueprint) Filter.Context.Blueprints.Add(Options.Blueprint);
		if (Options.Graph) Filter.Context.Graphs.Add(Options.Graph);
		if (Options.ContextPin) Filter.Context.Pins.Add(Options.ContextPin);

		int32 Examined = 0;
		const int32 MaxExamined = FMath::Max(Options.Limit * 20, 400);
		for (FCandidate& Candidate : Candidates)
		{
			if (Results.Num() >= Options.Limit || Examined >= MaxExamined) break;
			Examined++;

			if (bUseFilter)
			{
				FBlueprintActionInfo ActionInfo(Candidate.Owner, Candidate.Spawner);
				if (Filter.IsFiltered(ActionInfo)) continue;
			}

			FBlueprintActionUiSpec UiSpec = Candidate.Spawner->GetUiSpec(Filter.Context, IBlueprintNodeBinder::FBindingSet());
			UEdGraphNode* TemplateNode = Candidate.Spawner->GetTemplateNode(Options.Graph);

			TSharedRef<FJsonObject> Result = NewObject();
			Result->SetStringField(TEXT("title"), UiSpec.MenuName.ToString());
			Result->SetStringField(TEXT("category"), UiSpec.Category.ToString());
			Result->SetStringField(TEXT("tooltip"), UiSpec.Tooltip.ToString().Left(400));
			if (!UiSpec.Keywords.IsEmpty()) Result->SetStringField(TEXT("keywords"), UiSpec.Keywords.ToString());
			Result->SetStringField(TEXT("kind"), Candidate.Kind);
			if (Candidate.Spawner->NodeClass) Result->SetStringField(TEXT("node_class"), Candidate.Spawner->NodeClass->GetName());
			if (Candidate.Kind == TEXT("call_function"))
			{
				const UFunction* Function = CastChecked<UBlueprintFunctionNodeSpawner>(Candidate.Spawner)->GetFunction();
				Result->SetStringField(TEXT("function"), FunctionDisplay(Function));
				if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure)) Result->SetBoolField(TEXT("pure"), true);
				if (Function->HasMetaData(TEXT("Latent"))) Result->SetBoolField(TEXT("latent"), true);
				if (Function->HasAnyFunctionFlags(FUNC_Static)) Result->SetBoolField(TEXT("static"), true);
			}
			Result->SetObjectField(TEXT("add_node_spec"), BuildAddNodeSpec(Candidate, TemplateNode));
			Result->SetStringField(TEXT("signature"), Candidate.Spawner->GetSpawnerSignature().ToString());

			if (Options.bIncludePins && TemplateNode)
			{
				TArray<TSharedPtr<FJsonValue>> Pins;
				for (UEdGraphPin* Pin : TemplateNode->Pins)
				{
					if (!Pin || Pin->bHidden) continue;
					TSharedRef<FJsonObject> PinJson = NewObject();
					PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
					PinJson->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
					PinJson->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
					const FString Default = PinDefaultToString(Pin);
					if (!Default.IsEmpty() && Pin->Direction == EGPD_Input) PinJson->SetStringField(TEXT("default"), Default);
					Pins.Add(ObjectValue(PinJson));
				}
				Result->SetArrayField(TEXT("pins"), Pins);
			}
			Results.Add(ObjectValue(Result));
		}
		return Results;
	}
}
