// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABGraphSerializer.h"
#include "BABContext.h"
#include "BABTypes.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/TimelineTemplate.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MultiGate.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Self.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Select.h"
#include "K2Node_Switch.h"
#include "K2Node_Timeline.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_InputKey.h"
#include "Logging/TokenizedMessage.h"

namespace BAB
{
	bool ShouldShowPin(const UEdGraphPin* Pin, const FReadOptions& Options)
	{
		if (!Pin) return false;
		if (Pin->bHidden && !Options.bIncludeHiddenPins) return false;
		return true;
	}

	/** "then (Completed)" when the editor shows a label that differs from the pin name beyond spacing. */
	static FString PinLabel(const UEdGraphPin* Pin)
	{
		const FString Name = Pin->PinName.ToString();
		const FString Display = Pin->GetDisplayName().ToString();
		if (Display.IsEmpty())
		{
			return Name;
		}
		const FString CompactDisplay = Display.Replace(TEXT(" "), TEXT(""));
		FString CompactName = Name.Replace(TEXT(" "), TEXT(""));
		// bPrintToScreen is shown as "Print to Screen": same thing, no label needed.
		if (CompactName.Len() > 1 && CompactName[0] == TEXT('b') && FChar::IsUpper(CompactName[1]))
		{
			CompactName.RemoveAt(0);
		}
		if (!CompactDisplay.Equals(CompactName, ESearchCase::IgnoreCase))
		{
			return FString::Printf(TEXT("%s (%s)"), *Name, *Display);
		}
		return Name;
	}

	static FString QuoteIfText(const UEdGraphPin* Pin, const FString& Default)
	{
		const FName Category = Pin->PinType.PinCategory;
		if (Category == UEdGraphSchema_K2::PC_String || Category == UEdGraphSchema_K2::PC_Text || Category == UEdGraphSchema_K2::PC_Name)
		{
			return FString::Printf(TEXT("\"%s\""), *Default.ReplaceCharWithEscapedChar());
		}
		return Default;
	}

	FString DescribeNode(UEdGraphNode* Node, FJsonObject& Details, FString& DetailText)
	{
		DetailText.Reset();
		if (!Node) return TEXT("node");

		if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			Details.SetStringField(TEXT("name"), CustomEvent->CustomFunctionName.ToString());
			DetailText = FString::Printf(TEXT("custom_event %s"), *CustomEvent->CustomFunctionName.ToString());
			return TEXT("custom_event");
		}
		if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
		{
			const FString Owner = ClassDisplayName(Event->EventReference.GetMemberParentClass());
			const FString Signature = FString::Printf(TEXT("%s.%s"), *Owner, *Event->EventReference.GetMemberName().ToString());
			Details.SetStringField(TEXT("event"), Signature);
			DetailText = FString::Printf(TEXT("event %s"), *Signature);
			return TEXT("event");
		}
		if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			const UFunction* Function = Call->GetTargetFunction();
			FString Signature;
			if (Function)
			{
				Signature = FunctionDisplay(Function);
			}
			else
			{
				Signature = FString::Printf(TEXT("%s.%s"), *ClassDisplayName(Call->FunctionReference.GetMemberParentClass()), *Call->FunctionReference.GetMemberName().ToString());
			}
			Details.SetStringField(TEXT("function"), Signature);
			if (Call->IsNodePure()) Details.SetBoolField(TEXT("pure"), true);
			const bool bLatent = Function && Function->HasMetaData(TEXT("Latent"));
			if (bLatent) Details.SetBoolField(TEXT("latent"), true);
			if (!Function) Details.SetBoolField(TEXT("unresolved"), true);
			const TCHAR* Kind = Node->IsA<UK2Node_CallParentFunction>() ? TEXT("call_parent_function") : TEXT("call_function");
			DetailText = FString::Printf(TEXT("%s %s%s%s%s"), Node->IsA<UK2Node_CallParentFunction>() ? TEXT("parent") : TEXT("call"), *Signature, Call->IsNodePure() ? TEXT(" (pure)") : TEXT(""), bLatent ? TEXT(" (latent)") : TEXT(""), Function ? TEXT("") : TEXT(" (unresolved)"));
			return Kind;
		}
		if (UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			const FString VarName = Variable->GetVarNameString();
			Details.SetStringField(TEXT("variable"), VarName);
			const bool bSet = Node->IsA<UK2Node_VariableSet>();
			FString Scope;
			UClass* SourceClass = Variable->GetVariableSourceClass();
			UClass* SelfClass = Variable->HasValidBlueprint() ? GetBlueprintSelfClass(Variable->GetBlueprint()) : nullptr;
			const bool bSelfContext = !SourceClass || (SelfClass && SelfClass->IsChildOf(SourceClass));
			if (!bSelfContext)
			{
				Scope = ClassDisplayName(SourceClass);
				Details.SetStringField(TEXT("class"), Scope);
			}
			if (FProperty* Property = Variable->GetPropertyForVariable())
			{
				if (Property->GetOwner<UFunction>())
				{
					Details.SetBoolField(TEXT("local"), true);
				}
			}
			DetailText = FString::Printf(TEXT("%s %s%s"), bSet ? TEXT("set") : TEXT("get"), *VarName, Scope.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" on %s"), *Scope));
			return bSet ? TEXT("variable_set") : TEXT("variable_get");
		}
		if (Node->IsA<UK2Node_IfThenElse>())
		{
			DetailText = TEXT("branch");
			return TEXT("branch");
		}
		if (Node->IsA<UK2Node_MultiGate>())
		{
			DetailText = TEXT("multi_gate");
			return TEXT("multi_gate");
		}
		if (Node->IsA<UK2Node_ExecutionSequence>())
		{
			DetailText = TEXT("sequence");
			return TEXT("sequence");
		}
		if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			UEdGraph* MacroGraph = Macro->GetMacroGraph();
			const FString MacroName = MacroGraph ? MacroGraph->GetName() : TEXT("<missing>");
			Details.SetStringField(TEXT("macro"), MacroName);
			if (MacroGraph) Details.SetStringField(TEXT("library"), MacroGraph->GetOutermost()->GetName());
			DetailText = FString::Printf(TEXT("macro %s"), *MacroName);
			return TEXT("macro");
		}
		if (UK2Node_DynamicCast* Cast_ = Cast<UK2Node_DynamicCast>(Node))
		{
			const FString Target = ClassDisplayName(Cast_->TargetType);
			Details.SetStringField(TEXT("class"), Target);
			if (Cast_->IsNodePure()) Details.SetBoolField(TEXT("pure"), true);
			DetailText = FString::Printf(TEXT("cast %s%s"), *Target, Cast_->IsNodePure() ? TEXT(" (pure)") : TEXT(""));
			return TEXT("cast");
		}
		if (Node->IsA<UK2Node_SpawnActorFromClass>())
		{
			DetailText = TEXT("spawn_actor");
			return TEXT("spawn_actor");
		}
		if (Node->IsA<UK2Node_Knot>())
		{
			DetailText = TEXT("reroute");
			return TEXT("knot");
		}
		if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			Details.SetStringField(TEXT("text"), Comment->NodeComment);
			Details.SetNumberField(TEXT("width"), Comment->NodeWidth);
			Details.SetNumberField(TEXT("height"), Comment->NodeHeight);
			DetailText = FString::Printf(TEXT("comment %dx%d"), Comment->NodeWidth, Comment->NodeHeight);
			return TEXT("comment");
		}
		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			DetailText = TEXT("function_entry");
			return TEXT("function_entry");
		}
		if (Node->IsA<UK2Node_FunctionResult>())
		{
			DetailText = TEXT("function_result");
			return TEXT("function_result");
		}
		if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			const FString Bound = Composite->BoundGraph ? Composite->BoundGraph->GetName() : TEXT("<none>");
			Details.SetStringField(TEXT("graph"), Bound);
			DetailText = FString::Printf(TEXT("collapsed_graph %s"), *Bound);
			return TEXT("collapsed_graph");
		}
		if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
		{
			const TCHAR* Kind = Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs ? TEXT("tunnel_entry") : (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs ? TEXT("tunnel_exit") : TEXT("tunnel"));
			DetailText = Kind;
			return Kind;
		}
		if (Node->IsA<UK2Node_Self>())
		{
			DetailText = TEXT("self");
			return TEXT("self");
		}
		if (Node->IsA<UK2Node_MakeArray>())
		{
			DetailText = TEXT("make_array");
			return TEXT("make_array");
		}
		if (UK2Node_MakeStruct* MakeStruct = Cast<UK2Node_MakeStruct>(Node))
		{
			const FString StructName = MakeStruct->StructType ? MakeStruct->StructType->GetName() : TEXT("<none>");
			Details.SetStringField(TEXT("struct"), StructName);
			DetailText = FString::Printf(TEXT("make_struct %s"), *StructName);
			return TEXT("make_struct");
		}
		if (UK2Node_BreakStruct* BreakStruct = Cast<UK2Node_BreakStruct>(Node))
		{
			const FString StructName = BreakStruct->StructType ? BreakStruct->StructType->GetName() : TEXT("<none>");
			Details.SetStringField(TEXT("struct"), StructName);
			DetailText = FString::Printf(TEXT("break_struct %s"), *StructName);
			return TEXT("break_struct");
		}
		if (Node->IsA<UK2Node_Select>())
		{
			DetailText = TEXT("select");
			return TEXT("select");
		}
		if (Node->IsA<UK2Node_Switch>())
		{
			DetailText = FString::Printf(TEXT("switch %s"), *Node->GetClass()->GetName().Replace(TEXT("K2Node_Switch"), TEXT("")));
			return TEXT("switch");
		}
		if (UK2Node_Timeline* Timeline = Cast<UK2Node_Timeline>(Node))
		{
			Details.SetStringField(TEXT("name"), Timeline->TimelineName.ToString());
			DetailText = FString::Printf(TEXT("timeline %s"), *Timeline->TimelineName.ToString());
			return TEXT("timeline");
		}
		if (Node->IsA<UK2Node_CreateDelegate>())
		{
			DetailText = TEXT("create_delegate");
			return TEXT("create_delegate");
		}
		if (UK2Node_InputKey* InputKey = Cast<UK2Node_InputKey>(Node))
		{
			Details.SetStringField(TEXT("key"), InputKey->InputKey.ToString());
			DetailText = FString::Printf(TEXT("input_key %s"), *InputKey->InputKey.ToString());
			return TEXT("input_key");
		}
		DetailText = Node->GetClass()->GetName();
		return TEXT("node");
	}

	TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin, const FReadOptions& Options)
	{
		TSharedRef<FJsonObject> Json = NewObject();
		Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
		const FString Display = Pin->GetDisplayName().ToString();
		if (!Display.IsEmpty() && Display != Pin->PinName.ToString())
		{
			Json->SetStringField(TEXT("display"), Display);
		}
		Json->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
		if (Options.bIncludePinTypes)
		{
			Json->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
		}
		if (Pin->Direction == EGPD_Input)
		{
			const FString Default = PinDefaultToString(Pin);
			if (!Default.IsEmpty())
			{
				Json->SetStringField(TEXT("default"), Default);
			}
		}
		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked && Linked->GetOwningNodeUnchecked())
			{
				TSharedRef<FJsonObject> Link = NewObject();
				Link->SetStringField(TEXT("node"), NodeId(Linked->GetOwningNode()));
				Link->SetStringField(TEXT("pin"), Linked->PinName.ToString());
				Links.Add(ObjectValue(Link));
			}
		}
		Json->SetArrayField(TEXT("links"), Links);
		if (Pin->bHidden) Json->SetBoolField(TEXT("hidden"), true);
		if (Pin->bAdvancedView) Json->SetBoolField(TEXT("advanced"), true);
		if (Pin->bOrphanedPin) Json->SetBoolField(TEXT("orphaned"), true);
		if (Pin->bNotConnectable) Json->SetBoolField(TEXT("connectable"), false);
		if (Pin->ParentPin) Json->SetStringField(TEXT("parent"), Pin->ParentPin->PinName.ToString());
		if (Pin->SubPins.Num() > 0) Json->SetBoolField(TEXT("split"), true);
		if (GetDefault<UEdGraphSchema_K2>()->IsSelfPin(*Pin)) Json->SetBoolField(TEXT("is_self"), true);
		return Json;
	}

	static void CollectPins(UEdGraphNode* Node, const FReadOptions& Options, TArray<UEdGraphPin*>& Inputs, TArray<UEdGraphPin*>& Outputs)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!ShouldShowPin(Pin, Options)) continue;
			(Pin->Direction == EGPD_Input ? Inputs : Outputs).Add(Pin);
		}
	}

	static const TCHAR* SeverityName(int32 ErrorType)
	{
		switch (ErrorType)
		{
		case EMessageSeverity::Error: return TEXT("error");
		case EMessageSeverity::PerformanceWarning: return TEXT("performance_warning");
		case EMessageSeverity::Warning: return TEXT("warning");
		case EMessageSeverity::Info: return TEXT("info");
		default: return TEXT("note");
		}
	}

	TSharedRef<FJsonObject> NodeToJson(UEdGraphNode* Node, const FReadOptions& Options)
	{
		TSharedRef<FJsonObject> Json = NewObject();
		Json->SetStringField(TEXT("id"), NodeId(Node));
		if (Options.bIncludeGuids)
		{
			Json->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
		Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());

		FString DetailText;
		TSharedRef<FJsonObject> Details = NewObject();
		const FString Kind = DescribeNode(Node, *Details, DetailText);
		Json->SetStringField(TEXT("kind"), Kind);
		Json->SetStringField(TEXT("title"), NodeTitle(Node));
		for (const auto& Pair : Details->Values)
		{
			Json->SetField(Pair.Key, Pair.Value);
		}
		if (Options.bIncludePositions)
		{
			TArray<TSharedPtr<FJsonValue>> Pos;
			Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
			Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
			Json->SetArrayField(TEXT("pos"), Pos);
		}
		if (!Node->NodeComment.IsEmpty() && !Node->IsA<UEdGraphNode_Comment>())
		{
			Json->SetStringField(TEXT("comment"), Node->NodeComment);
		}
		if (!Node->IsNodeEnabled())
		{
			Json->SetBoolField(TEXT("enabled"), false);
		}
		if (Node->bHasCompilerMessage)
		{
			TSharedRef<FJsonObject> Error = NewObject();
			Error->SetStringField(TEXT("severity"), SeverityName(Node->ErrorType));
			Error->SetStringField(TEXT("message"), Node->ErrorMsg);
			Json->SetObjectField(TEXT("compiler_message"), Error);
		}

		TArray<UEdGraphPin*> Inputs, Outputs;
		CollectPins(Node, Options, Inputs, Outputs);
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* Pin : Inputs) Pins.Add(ObjectValue(PinToJson(Pin, Options)));
		for (UEdGraphPin* Pin : Outputs) Pins.Add(ObjectValue(PinToJson(Pin, Options)));
		Json->SetArrayField(TEXT("pins"), Pins);
		return Json;
	}

	TSharedRef<FJsonObject> GraphToJson(UBlueprint* BP, UEdGraph* Graph, const FReadOptions& Options)
	{
		TSharedRef<FJsonObject> Json = NewObject();
		Json->SetStringField(TEXT("blueprint"), GetObjectPath(BP));
		TSharedRef<FJsonObject> GraphInfo = NewObject();
		GraphInfo->SetStringField(TEXT("name"), Graph->GetName());
		GraphInfo->SetStringField(TEXT("kind"), GraphKind(BP, Graph));
		GraphInfo->SetStringField(TEXT("guid"), Graph->GraphGuid.ToString(EGuidFormats::Digits).Left(8));
		Json->SetObjectField(TEXT("graph"), GraphInfo);
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(ObjectValue(NodeToJson(Node, Options)));
			}
		}
		Json->SetNumberField(TEXT("node_count"), Nodes.Num());
		Json->SetArrayField(TEXT("nodes"), Nodes);
		return Json;
	}

	static FString PinLinksText(const UEdGraphPin* Pin)
	{
		TArray<FString> Targets;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked && Linked->GetOwningNodeUnchecked())
			{
				Targets.Add(FString::Printf(TEXT("%s.%s"), *NodeId(Linked->GetOwningNode()), *Linked->PinName.ToString()));
			}
		}
		return FString::Join(Targets, TEXT(", "));
	}

	static FString PinFlagsText(const UEdGraphPin* Pin)
	{
		TArray<FString> Flags;
		if (GetDefault<UEdGraphSchema_K2>()->IsSelfPin(*Pin) && Pin->LinkedTo.Num() == 0) Flags.Add(TEXT("self"));
		if (Pin->bAdvancedView) Flags.Add(TEXT("adv"));
		if (Pin->bOrphanedPin) Flags.Add(TEXT("orphaned"));
		if (Pin->bHidden) Flags.Add(TEXT("hidden"));
		if (Pin->bNotConnectable) Flags.Add(TEXT("not-connectable"));
		if (Pin->SubPins.Num() > 0) Flags.Add(TEXT("split"));
		return Flags.Num() ? FString::Printf(TEXT(" (%s)"), *FString::Join(Flags, TEXT(", "))) : FString();
	}

	FString NodeToText(UEdGraphNode* Node, const FReadOptions& Options)
	{
		FString DetailText;
		TSharedRef<FJsonObject> Details = NewObject();
		DescribeNode(Node, *Details, DetailText);

		FString Out = FString::Printf(TEXT("[%s] %s | %s"), *NodeId(Node), *NodeTitle(Node), *DetailText);
		if (Options.bIncludePositions)
		{
			Out += FString::Printf(TEXT(" @ %d,%d"), Node->NodePosX, Node->NodePosY);
		}
		if (!Node->IsNodeEnabled())
		{
			Out += TEXT(" [disabled]");
		}
		if (!Node->NodeComment.IsEmpty() && !Node->IsA<UEdGraphNode_Comment>())
		{
			Out += FString::Printf(TEXT("  // %s"), *Node->NodeComment.Replace(TEXT("\n"), TEXT(" ")));
		}
		Out += TEXT("\n");
		if (Node->bHasCompilerMessage)
		{
			Out += FString::Printf(TEXT("  ! %s: %s\n"), SeverityName(Node->ErrorType), *Node->ErrorMsg.Replace(TEXT("\n"), TEXT(" ")));
		}

		TArray<UEdGraphPin*> Inputs, Outputs;
		CollectPins(Node, Options, Inputs, Outputs);
		for (UEdGraphPin* Pin : Inputs)
		{
			FString Line = FString::Printf(TEXT("  < %s"), *PinLabel(Pin));
			if (Options.bIncludePinTypes) Line += FString::Printf(TEXT(" : %s"), *PinTypeToString(Pin->PinType));
			if (Pin->LinkedTo.Num() > 0)
			{
				Line += FString::Printf(TEXT(" <- %s"), *PinLinksText(Pin));
			}
			else
			{
				const FString Default = PinDefaultToString(Pin);
				const bool bStringLike = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text;
				if (!Default.IsEmpty() || (bStringLike && !Pin->PinType.IsContainer()))
				{
					Line += FString::Printf(TEXT(" = %s"), *QuoteIfText(Pin, Default));
				}
			}
			Line += PinFlagsText(Pin);
			Out += Line + TEXT("\n");
		}
		for (UEdGraphPin* Pin : Outputs)
		{
			FString Line = FString::Printf(TEXT("  > %s"), *PinLabel(Pin));
			if (Options.bIncludePinTypes) Line += FString::Printf(TEXT(" : %s"), *PinTypeToString(Pin->PinType));
			if (Pin->LinkedTo.Num() > 0)
			{
				Line += FString::Printf(TEXT(" -> %s"), *PinLinksText(Pin));
			}
			Line += PinFlagsText(Pin);
			Out += Line + TEXT("\n");
		}
		return Out;
	}

	FString GraphToText(UBlueprint* BP, UEdGraph* Graph, const FReadOptions& Options)
	{
		FString Out;
		Out += FString::Printf(TEXT("Blueprint %s | parent: %s | status: %s\n"), *GetObjectPath(BP), *ClassDisplayName(BP->ParentClass), *StatusToString(BP));
		Out += FString::Printf(TEXT("Graph %s (%s) | %d nodes\n"), *Graph->GetName(), *GraphKind(BP, Graph), Graph->Nodes.Num());
		Out += TEXT("# [id] title | kind detail @ x,y ; \"<\" input pin, \">\" output pin, \"=\" default value, \"<-\"/\"->\" links to id.pin\n\n");
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Out += NodeToText(Node, Options);
			}
		}
		return Out;
	}

	// ---------------------------------------------------------------------------------------
	// Blueprint summary

	static void AppendSignatureFromGraph(UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& Inputs, TArray<TSharedPtr<FJsonValue>>& Outputs, TArray<FString>& InputText, TArray<FString>& OutputText)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
			UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node);
			if (!Entry && !Result) continue;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->bHidden || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Entry && Pin->Direction != EGPD_Output) continue;
				if (Result && Pin->Direction != EGPD_Input) continue;
				TSharedRef<FJsonObject> Param = NewObject();
				Param->SetStringField(TEXT("name"), Pin->PinName.ToString());
				Param->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
				const FString Line = FString::Printf(TEXT("%s: %s"), *Pin->PinName.ToString(), *PinTypeToString(Pin->PinType));
				if (Entry) { Inputs.Add(ObjectValue(Param)); InputText.Add(Line); }
				else { Outputs.Add(ObjectValue(Param)); OutputText.Add(Line); }
			}
		}
	}

	TSharedRef<FJsonObject> BlueprintToJson(UBlueprint* BP)
	{
		TSharedRef<FJsonObject> Json = NewObject();
		Json->SetStringField(TEXT("path"), GetObjectPath(BP));
		Json->SetStringField(TEXT("name"), BP->GetName());
		Json->SetStringField(TEXT("parent_class"), ClassDisplayName(BP->ParentClass));
		if (BP->ParentClass) Json->SetStringField(TEXT("parent_class_path"), BP->ParentClass->GetPathName());
		Json->SetStringField(TEXT("blueprint_type"), BlueprintTypeToString(BP));
		Json->SetStringField(TEXT("status"), StatusToString(BP));
		Json->SetBoolField(TEXT("dirty"), BP->GetOutermost()->IsDirty());
		if (BP->GeneratedClass) Json->SetStringField(TEXT("generated_class"), BP->GeneratedClass->GetName());
		if (!BP->BlueprintDescription.IsEmpty()) Json->SetStringField(TEXT("description"), BP->BlueprintDescription);

		// Graphs
		TArray<UEdGraph*> Graphs;
		GetAllGraphs(BP, Graphs);
		TArray<TSharedPtr<FJsonValue>> GraphList;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			TSharedRef<FJsonObject> G = NewObject();
			G->SetStringField(TEXT("name"), Graph->GetName());
			G->SetStringField(TEXT("kind"), GraphKind(BP, Graph));
			G->SetNumberField(TEXT("nodes"), Graph->Nodes.Num());
			GraphList.Add(ObjectValue(G));
		}
		Json->SetArrayField(TEXT("graphs"), GraphList);

		// Variables
		TArray<TSharedPtr<FJsonValue>> Variables;
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			TSharedRef<FJsonObject> V = NewObject();
			V->SetStringField(TEXT("name"), Var.VarName.ToString());
			V->SetStringField(TEXT("type"), PinTypeToString(Var.VarType));
			if (!Var.DefaultValue.IsEmpty()) V->SetStringField(TEXT("default"), Var.DefaultValue);
			const FString Category = Var.Category.ToString();
			if (!Category.IsEmpty() && Category != UEdGraphSchema_K2::VR_DefaultCategory.ToString()) V->SetStringField(TEXT("category"), Category);
			V->SetBoolField(TEXT("instance_editable"), (Var.PropertyFlags & CPF_Edit) && !(Var.PropertyFlags & CPF_DisableEditOnInstance));
			if (Var.PropertyFlags & CPF_BlueprintReadOnly) V->SetBoolField(TEXT("read_only"), true);
			if (Var.PropertyFlags & CPF_Net) V->SetBoolField(TEXT("replicated"), true);
			if (Var.HasMetaData(TEXT("ExposeOnSpawn")) && Var.GetMetaData(TEXT("ExposeOnSpawn")) == TEXT("true")) V->SetBoolField(TEXT("expose_on_spawn"), true);
			if (Var.HasMetaData(TEXT("BlueprintPrivate")) && Var.GetMetaData(TEXT("BlueprintPrivate")) == TEXT("true")) V->SetBoolField(TEXT("private"), true);
			if (Var.HasMetaData(TEXT("tooltip"))) V->SetStringField(TEXT("tooltip"), Var.GetMetaData(TEXT("tooltip")));
			Variables.Add(ObjectValue(V));
		}
		Json->SetArrayField(TEXT("variables"), Variables);

		// Components (own SCS only)
		TArray<TSharedPtr<FJsonValue>> Components;
		if (BP->SimpleConstructionScript)
		{
			TFunction<void(USCS_Node*, const FString&)> Visit = [&](USCS_Node* SCSNode, const FString& ParentName)
			{
				if (!SCSNode) return;
				TSharedRef<FJsonObject> C = NewObject();
				C->SetStringField(TEXT("name"), SCSNode->GetVariableName().ToString());
				C->SetStringField(TEXT("class"), ClassDisplayName(SCSNode->ComponentClass));
				if (!ParentName.IsEmpty()) C->SetStringField(TEXT("parent"), ParentName);
				Components.Add(ObjectValue(C));
				for (USCS_Node* Child : SCSNode->GetChildNodes())
				{
					Visit(Child, SCSNode->GetVariableName().ToString());
				}
			};
			for (USCS_Node* Root : BP->SimpleConstructionScript->GetRootNodes())
			{
				Visit(Root, FString());
			}
		}
		Json->SetArrayField(TEXT("components"), Components);

		// Functions
		TArray<TSharedPtr<FJsonValue>> Functions;
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (!Graph) continue;
			TSharedRef<FJsonObject> F = NewObject();
			F->SetStringField(TEXT("name"), Graph->GetName());
			TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
			TArray<FString> InText, OutText;
			AppendSignatureFromGraph(Graph, Inputs, Outputs, InText, OutText);
			F->SetArrayField(TEXT("inputs"), Inputs);
			F->SetArrayField(TEXT("outputs"), Outputs);
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					const int32 Flags = Entry->GetFunctionFlags();
					if (Flags & FUNC_BlueprintPure) F->SetBoolField(TEXT("pure"), true);
					if (Flags & FUNC_Const) F->SetBoolField(TEXT("const"), true);
					if (Flags & FUNC_Static) F->SetBoolField(TEXT("static"), true);
					if (Flags & FUNC_Protected) F->SetStringField(TEXT("access"), TEXT("protected"));
					else if (Flags & FUNC_Private) F->SetStringField(TEXT("access"), TEXT("private"));
					else F->SetStringField(TEXT("access"), TEXT("public"));
					if (!Entry->MetaData.Category.IsEmpty()) F->SetStringField(TEXT("category"), Entry->MetaData.Category.ToString());
					if (!Entry->MetaData.ToolTip.IsEmpty()) F->SetStringField(TEXT("tooltip"), Entry->MetaData.ToolTip.ToString());
					break;
				}
			}
			Functions.Add(ObjectValue(F));
		}
		Json->SetArrayField(TEXT("functions"), Functions);

		// Macros
		TArray<FString> MacroNames;
		for (UEdGraph* Graph : BP->MacroGraphs) if (Graph) MacroNames.Add(Graph->GetName());
		SetStringArray(*Json, TEXT("macros"), MacroNames);

		// Event dispatchers
		TArray<TSharedPtr<FJsonValue>> Dispatchers;
		for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
		{
			if (!Graph) continue;
			TSharedRef<FJsonObject> D = NewObject();
			D->SetStringField(TEXT("name"), Graph->GetName());
			TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
			TArray<FString> InText, OutText;
			AppendSignatureFromGraph(Graph, Inputs, Outputs, InText, OutText);
			D->SetArrayField(TEXT("params"), Inputs);
			Dispatchers.Add(ObjectValue(D));
		}
		Json->SetArrayField(TEXT("event_dispatchers"), Dispatchers);

		// Events present in the ubergraphs
		TArray<TSharedPtr<FJsonValue>> Events;
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
				{
					TSharedRef<FJsonObject> E = NewObject();
					E->SetStringField(TEXT("name"), Event->GetFunctionName().ToString());
					E->SetStringField(TEXT("node"), NodeId(Event));
					E->SetStringField(TEXT("graph"), Graph->GetName());
					E->SetBoolField(TEXT("custom"), Event->IsA<UK2Node_CustomEvent>());
					Events.Add(ObjectValue(E));
				}
			}
		}
		Json->SetArrayField(TEXT("events"), Events);

		// Interfaces
		TArray<FString> Interfaces;
		for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
		{
			if (Interface.Interface) Interfaces.Add(ClassDisplayName(Interface.Interface));
		}
		SetStringArray(*Json, TEXT("interfaces"), Interfaces);

		// Timelines
		TArray<FString> Timelines;
		for (UTimelineTemplate* Timeline : BP->Timelines)
		{
			if (Timeline) Timelines.Add(Timeline->GetVariableName().ToString());
		}
		SetStringArray(*Json, TEXT("timelines"), Timelines);

		return Json;
	}

	FString BlueprintToText(UBlueprint* BP)
	{
		FString Out;
		Out += FString::Printf(TEXT("Blueprint %s | parent: %s | type: %s | status: %s%s\n"), *GetObjectPath(BP), *ClassDisplayName(BP->ParentClass), *BlueprintTypeToString(BP), *StatusToString(BP), BP->GetOutermost()->IsDirty() ? TEXT(" | unsaved changes") : TEXT(""));

		TArray<UEdGraph*> Graphs;
		GetAllGraphs(BP, Graphs);
		Out += TEXT("Graphs:\n");
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph) Out += FString::Printf(TEXT("  %s (%s, %d nodes)\n"), *Graph->GetName(), *GraphKind(BP, Graph), Graph->Nodes.Num());
		}

		Out += FString::Printf(TEXT("Variables (%d):\n"), BP->NewVariables.Num());
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			TArray<FString> Flags;
			if ((Var.PropertyFlags & CPF_Edit) && !(Var.PropertyFlags & CPF_DisableEditOnInstance)) Flags.Add(TEXT("instance editable"));
			if (Var.PropertyFlags & CPF_BlueprintReadOnly) Flags.Add(TEXT("read-only"));
			if (Var.PropertyFlags & CPF_Net) Flags.Add(TEXT("replicated"));
			if (Var.HasMetaData(TEXT("ExposeOnSpawn")) && Var.GetMetaData(TEXT("ExposeOnSpawn")) == TEXT("true")) Flags.Add(TEXT("expose on spawn"));
			const FString Category = Var.Category.ToString();
			if (!Category.IsEmpty() && Category != UEdGraphSchema_K2::VR_DefaultCategory.ToString()) Flags.Add(FString::Printf(TEXT("category %s"), *Category));
			Out += FString::Printf(TEXT("  %s : %s%s%s\n"), *Var.VarName.ToString(), *PinTypeToString(Var.VarType), Var.DefaultValue.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" = %s"), *Var.DefaultValue), Flags.Num() ? *FString::Printf(TEXT(" (%s)"), *FString::Join(Flags, TEXT(", "))) : TEXT(""));
		}

		if (BP->SimpleConstructionScript && BP->SimpleConstructionScript->GetAllNodes().Num() > 0)
		{
			Out += TEXT("Components:\n");
			for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (SCSNode) Out += FString::Printf(TEXT("  %s : %s\n"), *SCSNode->GetVariableName().ToString(), *ClassDisplayName(SCSNode->ComponentClass));
			}
		}

		if (BP->FunctionGraphs.Num() > 0)
		{
			Out += TEXT("Functions:\n");
			for (UEdGraph* Graph : BP->FunctionGraphs)
			{
				if (!Graph) continue;
				TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
				TArray<FString> InText, OutText;
				AppendSignatureFromGraph(Graph, Inputs, Outputs, InText, OutText);
				Out += FString::Printf(TEXT("  %s(%s)%s\n"), *Graph->GetName(), *FString::Join(InText, TEXT(", ")), OutText.Num() ? *FString::Printf(TEXT(" -> (%s)"), *FString::Join(OutText, TEXT(", "))) : TEXT(""));
			}
		}
		if (BP->MacroGraphs.Num() > 0)
		{
			TArray<FString> Names;
			for (UEdGraph* Graph : BP->MacroGraphs) if (Graph) Names.Add(Graph->GetName());
			Out += FString::Printf(TEXT("Macros: %s\n"), *FString::Join(Names, TEXT(", ")));
		}
		if (BP->DelegateSignatureGraphs.Num() > 0)
		{
			TArray<FString> Names;
			for (UEdGraph* Graph : BP->DelegateSignatureGraphs) if (Graph) Names.Add(Graph->GetName());
			Out += FString::Printf(TEXT("Event dispatchers: %s\n"), *FString::Join(Names, TEXT(", ")));
		}
		TArray<FString> EventLines;
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
				{
					EventLines.Add(FString::Printf(TEXT("%s [%s]"), *Event->GetFunctionName().ToString(), *NodeId(Event)));
				}
			}
		}
		if (EventLines.Num() > 0)
		{
			Out += FString::Printf(TEXT("Events: %s\n"), *FString::Join(EventLines, TEXT(", ")));
		}
		if (BP->ImplementedInterfaces.Num() > 0)
		{
			TArray<FString> Names;
			for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces) if (Interface.Interface) Names.Add(ClassDisplayName(Interface.Interface));
			Out += FString::Printf(TEXT("Interfaces: %s\n"), *FString::Join(Names, TEXT(", ")));
		}
		if (BP->Timelines.Num() > 0)
		{
			TArray<FString> Names;
			for (UTimelineTemplate* Timeline : BP->Timelines) if (Timeline) Names.Add(Timeline->GetVariableName().ToString());
			Out += FString::Printf(TEXT("Timelines: %s\n"), *FString::Join(Names, TEXT(", ")));
		}
		return Out;
	}
}
