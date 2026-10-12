// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABOpsCommon.h"
#include "BABNodeFactory.h"
#include "BABNodeSearch.h"
#include "BABAutoLayout.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "EdGraphNode_Comment.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_CustomEvent.h"
#include "UObject/Package.h"

namespace BAB
{
	static const TCHAR* ConnectionResponseName(ECanCreateConnectionResponse Response)
	{
		switch (Response)
		{
		case CONNECT_RESPONSE_MAKE: return TEXT("MAKE");
		case CONNECT_RESPONSE_DISALLOW: return TEXT("DISALLOW");
		case CONNECT_RESPONSE_BREAK_OTHERS_A: return TEXT("MAKE_REPLACING_EXISTING_LINK_ON_FROM");
		case CONNECT_RESPONSE_BREAK_OTHERS_B: return TEXT("MAKE_REPLACING_EXISTING_LINK_ON_TO");
		case CONNECT_RESPONSE_BREAK_OTHERS_AB: return TEXT("MAKE_REPLACING_EXISTING_LINKS");
		case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE: return TEXT("MAKE_WITH_CONVERSION_NODE");
		case CONNECT_RESPONSE_MAKE_WITH_PROMOTION: return TEXT("MAKE_WITH_PROMOTION");
		default: return TEXT("UNKNOWN");
		}
	}

	static TSharedRef<FJsonObject> PinRefJson(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Json = NewObject();
		Json->SetStringField(TEXT("node"), NodeId(Pin->GetOwningNode()));
		Json->SetStringField(TEXT("pin"), Pin->PinName.ToString());
		Json->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
		Json->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
		return Json;
	}

	static FString PinDescription(const UEdGraphPin* Pin)
	{
		return FString::Printf(TEXT("%s.%s [%s %s]"), *NodeId(Pin->GetOwningNode()), *Pin->PinName.ToString(), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"), *PinTypeToString(Pin->PinType));
	}

	/**
	 * Reads a pin from params. Accepts either a reference field (object {node,pin} or "ID.Pin")
	 * or the pair of fields NodeField + PinField.
	 */
	/** Resolves a pin reference, trying the preferred direction first (so "from" favours outputs and "to" favours inputs). */
	static UEdGraphPin* ResolvePinRefPreferring(const FOpContext& Ctx, const TSharedPtr<FJsonValue>& Ref, const FString& PreferredDirection, FString& OutError)
	{
		if (!PreferredDirection.IsEmpty() && Ref.IsValid())
		{
			TSharedPtr<FJsonValue> Preferred;
			if (Ref->Type == EJson::String && !Ref->AsString().Contains(TEXT(":")))
			{
				Preferred = MakeShared<FJsonValueString>(Ref->AsString() + TEXT(":") + PreferredDirection);
			}
			else if (Ref->Type == EJson::Object && !HasField(*Ref->AsObject(), TEXT("direction")) && !HasField(*Ref->AsObject(), TEXT("dir")))
			{
				TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
				Copy->Values = Ref->AsObject()->Values;
				Copy->SetStringField(TEXT("direction"), PreferredDirection);
				Preferred = MakeShared<FJsonValueObject>(Copy);
			}
			if (Preferred.IsValid())
			{
				FString Ignored;
				if (UEdGraphPin* Pin = ResolvePinRef(Ctx.Blueprint, Ctx.Graph, Preferred, Ignored))
				{
					return Pin;
				}
			}
		}
		return ResolvePinRef(Ctx.Blueprint, Ctx.Graph, Ref, OutError);
	}

	static UEdGraphPin* PinFromParams(const FOpContext& Ctx, const FJsonObject& Params, const TCHAR* RefField, const TCHAR* NodeField, const TCHAR* PinField, const FString& DefaultDirection, FString& OutError)
	{
		const FString NodeRef = GetString(Params, NodeField);
		const TSharedPtr<FJsonValue>* Ref = Params.Values.Find(RefField);
		if (Ref && Ref->IsValid() && (*Ref)->Type == EJson::Object)
		{
			return ResolvePinRefPreferring(Ctx, *Ref, DefaultDirection, OutError);
		}
		if (Ref && Ref->IsValid() && (*Ref)->Type == EJson::String && NodeRef.IsEmpty())
		{
			return ResolvePinRefPreferring(Ctx, *Ref, DefaultDirection, OutError);
		}
		const FString PinName = GetString(Params, PinField);
		if (NodeRef.IsEmpty() || PinName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing pin reference: pass \"%s\" as {\"node\":\"ID\",\"pin\":\"Name\"} or \"ID.Name\", or the fields \"%s\" and \"%s\"."), RefField, NodeField, PinField);
			return nullptr;
		}
		UEdGraphNode* Node = FindNode(Ctx.Blueprint, Ctx.Graph, NodeRef, OutError);
		if (!Node) return nullptr;
		return FindPin(Node, PinName, GetString(Params, TEXT("direction"), DefaultDirection), OutError);
	}

	static UEdGraphNode* NodeFromParams(const FOpContext& Ctx, const FJsonObject& Params, FString& OutError)
	{
		const FString NodeRef = GetString(Params, TEXT("node"), GetString(Params, TEXT("id")));
		if (NodeRef.IsEmpty())
		{
			OutError = TEXT("Missing \"node\" (node id from bp_read_graph).");
			return nullptr;
		}
		return FindNode(Ctx.Blueprint, Ctx.Graph, NodeRef, OutError);
	}

	static FVector2D DefaultPositionForNewNode(UEdGraph* Graph, const UEdGraphPin* AutowireFrom)
	{
		if (AutowireFrom && AutowireFrom->GetOwningNodeUnchecked())
		{
			const UEdGraphNode* Source = AutowireFrom->GetOwningNode();
			const float Direction = AutowireFrom->Direction == EGPD_Output ? 1.0f : -1.0f;
			return FVector2D(Source->NodePosX + Direction * 420.0f, Source->NodePosY);
		}
		int32 MaxY = -200;
		int32 MinX = 0;
		bool bAny = false;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			MaxY = FMath::Max(MaxY, Node->NodePosY);
			MinX = bAny ? FMath::Min(MinX, Node->NodePosX) : Node->NodePosX;
			bAny = true;
		}
		return FVector2D(bAny ? MinX : 0, bAny ? MaxY + 200 : 0);
	}

	static FString SetPinDefaultChecked(const UEdGraphSchema_K2* Schema, UEdGraphPin* Pin, const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutWarnings)
	{
		const FName Category = Pin->PinType.PinCategory;
		const bool bObjectLike = Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_Interface;
		const bool bSoft = Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass;

		if (bObjectLike && !Pin->PinType.IsContainer())
		{
			FString Path = Value.IsValid() && Value->Type == EJson::String ? Value->AsString().TrimStartAndEnd() : FString();
			UObject* Object = nullptr;
			if (!Path.IsEmpty() && !Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				FString Error;
				if (Category == UEdGraphSchema_K2::PC_Class)
				{
					Object = ResolveClass(Path, Error);
				}
				else
				{
					Object = FindObject<UObject>(nullptr, *Path);
					if (!Object) Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path, nullptr, LOAD_NoWarn, nullptr);
					if (!Object) Object = ResolveClass(Path, Error);
				}
				if (!Object)
				{
					return FString::Printf(TEXT("Object or class not found: %s%s"), *Path, Error.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Error));
				}
			}
			const FString Validation = Schema->IsPinDefaultValid(Pin, FString(), Object, FText::GetEmpty());
			if (!Validation.IsEmpty()) return Validation;
			Schema->TrySetDefaultObject(*Pin, Object);
			return FString();
		}

		if (Category == UEdGraphSchema_K2::PC_Text && !Pin->PinType.IsContainer())
		{
			const FString Text = Value.IsValid() ? JsonValueToPinDefault(Pin, Value) : FString();
			Schema->TrySetDefaultText(*Pin, FText::FromString(Text));
			return FString();
		}

		FString Text = JsonValueToPinDefault(Pin, Value);

		// Enum pins: accept display names ("Visibility") as well as entry names ("ECC_Visibility").
		if (UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
		{
			if (Enum->GetIndexByNameString(Text) == INDEX_NONE)
			{
				const FString Compact = Text.Replace(TEXT(" "), TEXT(""));
				for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
				{
					const FString EntryName = Enum->GetNameStringByIndex(Index);
					const FString Display = Enum->GetDisplayNameTextByIndex(Index).ToString().Replace(TEXT(" "), TEXT(""));
					if (EntryName.Equals(Compact, ESearchCase::IgnoreCase) || Display.Equals(Compact, ESearchCase::IgnoreCase) || (Enum->GetNameByIndex(Index).ToString().EndsWith(TEXT("::") + Compact)))
					{
						Text = EntryName;
						break;
					}
				}
			}
		}
		if (bSoft && !Text.IsEmpty())
		{
			FString Error;
			if (UObject* Object = FindObject<UObject>(nullptr, *Text))
			{
				Text = Object->GetPathName();
			}
			else if (UClass* Class = ResolveClass(Text, Error))
			{
				if (Category == UEdGraphSchema_K2::PC_SoftClass) Text = Class->GetPathName();
			}
		}

		FString UseValue;
		TObjectPtr<UObject> UseObject = nullptr;
		FText UseText;
		Schema->GetPinDefaultValuesFromString(Pin->PinType, Pin->GetOwningNodeUnchecked(), Text, UseValue, UseObject, UseText, false);
		const FString Validation = Schema->IsPinDefaultValid(Pin, UseValue, UseObject, UseText);
		if (!Validation.IsEmpty())
		{
			return FString::Printf(TEXT("Invalid default \"%s\" for pin %s (%s): %s"), *Text, *Pin->PinName.ToString(), *PinTypeToString(Pin->PinType), *Validation);
		}
		Schema->TrySetDefaultValue(*Pin, Text);
		if (Pin->LinkedTo.Num() > 0)
		{
			OutWarnings.Add(FString::Printf(TEXT("Pin %s is connected; its default is ignored while a link exists."), *Pin->PinName.ToString()));
		}
		return FString();
	}
}

void BABRegisterOperations_Graph()
{
	using namespace BAB;
	FBABDispatcher& D = FBABDispatcher::Get();

	D.Register(TEXT("bp_read_graph"), TEXT("Reads a graph as compact text (default) or JSON. graph defaults to the event graph."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, true, Ctx, Failure)) return Failure;
		const FString Format = GetString(Params, TEXT("format"), TEXT("text")).ToLower();
		const FReadOptions Options = ReadOptionsFromParams(Params);
		FBABResult Result;
		if (Format == TEXT("json") || Format == TEXT("both"))
		{
			Result.Data = GraphToJson(Ctx.Blueprint, Ctx.Graph, Options);
		}
		if (Format == TEXT("text") || Format == TEXT("both"))
		{
			Result.Data->SetStringField(TEXT("text"), GraphToText(Ctx.Blueprint, Ctx.Graph, Options));
		}
		Result.Data->SetStringField(TEXT("blueprint"), GetObjectPath(Ctx.Blueprint));
		Result.Data->SetStringField(TEXT("graph_name"), Ctx.Graph->GetName());
		Result.Data->SetNumberField(TEXT("node_count"), Ctx.Graph->Nodes.Num());
		return Result;
	});

	D.Register(TEXT("bp_get_node"), TEXT("Returns one node (JSON and text) by id."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphNode* Node = NodeFromParams(Ctx, Params, Error);
		if (!Node) return FBABResult::Fail(Error);
		const FReadOptions Options = ReadOptionsFromParams(Params);
		FBABResult Result;
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Node, Options));
		Result.Data->SetStringField(TEXT("text"), NodeToText(Node, Options));
		Result.Data->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
		return Result;
	});

	D.Register(TEXT("bp_search_nodes"), TEXT("Searches placeable nodes (functions, events, variables, macros...) and returns ready-to-use specs for bp_add_node."), [](const FJsonObject& Params) -> FBABResult
	{
		FSearchOptions Options;
		Options.Query = GetString(Params, TEXT("query"));
		if (Options.Query.TrimStartAndEnd().IsEmpty())
		{
			return FBABResult::Fail(TEXT("Missing \"query\" (e.g. \"print string\", \"set actor location\", \"for each loop\")."));
		}
		Options.Limit = FMath::Clamp(GetInt(Params, TEXT("limit"), 25), 1, 200);
		Options.bIncludePins = GetBool(Params, TEXT("include_pins"), false);
		Options.KindFilter = GetString(Params, TEXT("kind"));
		Options.bContextSensitive = GetBool(Params, TEXT("context_sensitive"), true);

		FBABResult Result;
		if (HasField(Params, TEXT("blueprint")))
		{
			FOpContext Ctx;
			FBABResult Failure;
			if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
			Options.Blueprint = Ctx.Blueprint;
			Options.Graph = Ctx.Graph ? Ctx.Graph : FBlueprintEditorUtils::FindEventGraph(Ctx.Blueprint);
			if (const TSharedPtr<FJsonValue>* ContextPin = Params.Values.Find(TEXT("context_pin")))
			{
				FString Error;
				Options.ContextPin = ResolvePinRef(Ctx.Blueprint, Ctx.Graph, *ContextPin, Error);
				if (!Options.ContextPin) return FBABResult::Fail(Error);
			}
		}
		int32 TotalMatches = 0;
		TArray<TSharedPtr<FJsonValue>> Results = SearchNodes(Options, TotalMatches);
		Result.Data->SetStringField(TEXT("query"), Options.Query);
		Result.Data->SetArrayField(TEXT("results"), Results);
		Result.Data->SetNumberField(TEXT("count"), Results.Num());
		Result.Data->SetNumberField(TEXT("total_matches"), TotalMatches);
		if (Results.Num() == 0)
		{
			Result.Warn(TEXT("No node matched. Try fewer or different words, the function name without spaces, or kind=call_function."));
		}
		return Result;
	});

	D.Register(TEXT("bp_add_node"), TEXT("Adds a node to a graph from a spec ({type:call_function, function:...}, {type:event, name:BeginPlay}, ...)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, true, Ctx, Failure)) return Failure;

		const TSharedPtr<FJsonObject> SpecObject = GetObject(Params, TEXT("node"));
		const FJsonObject& Spec = SpecObject.IsValid() ? *SpecObject : Params;

		FBABResult Result;
		UEdGraphPin* AutowireFrom = nullptr;
		const TSharedPtr<FJsonValue>* AutowireValue = Params.Values.Find(TEXT("autowire_from"));
		if (!AutowireValue) AutowireValue = Params.Values.Find(TEXT("connect_from"));
		if (AutowireValue && AutowireValue->IsValid() && (*AutowireValue)->Type != EJson::Null)
		{
			FString Error;
			AutowireFrom = ResolvePinRef(Ctx.Blueprint, nullptr, *AutowireValue, Error);
			if (!AutowireFrom) return FBABResult::Fail(Error);
		}

		FVector2D Position;
		bool bHasPosition = GetVector2D(Params, TEXT("pos"), Position) || GetVector2D(Params, TEXT("position"), Position) || GetVector2D(Spec, TEXT("pos"), Position);
		if (!bHasPosition)
		{
			Position = DefaultPositionForNewNode(Ctx.Graph, AutowireFrom);
		}

		FString Error;
		TArray<FString> Warnings;
		UEdGraphNode* Node = CreateNodeFromSpec(Ctx.Blueprint, Ctx.Graph, Spec, Position, Error, Warnings);
		for (const FString& W : Warnings) Result.Warn(W);
		if (!Node)
		{
			Result.Error(Error);
			return Result;
		}

		bool bAutowired = false;
		if (AutowireFrom)
		{
			Node->AutowireNewNode(AutowireFrom);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->LinkedTo.Contains(AutowireFrom)) { bAutowired = true; break; }
			}
			if (!bAutowired)
			{
				Result.Warn(FString::Printf(TEXT("Could not autowire from %s: no compatible pin. Use bp_connect_pins."), *PinDescription(AutowireFrom)));
			}
			else if (EnableGhostNode(AutowireFrom->GetOwningNode()))
			{
				Result.Warn(FString::Printf(TEXT("Enabled the template node %s because it is now connected."), *NodeId(AutowireFrom->GetOwningNode())));
			}
		}
		MarkModified(Ctx.Blueprint, Node);

		const FReadOptions Options;
		Result.Data->SetStringField(TEXT("id"), NodeId(Node));
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Node, Options));
		Result.Data->SetStringField(TEXT("text"), NodeToText(Node, Options));
		Result.Data->SetStringField(TEXT("graph"), Ctx.Graph->GetName());
		Result.Data->SetBoolField(TEXT("autowired"), bAutowired);
		WarnIfNotWritable(Ctx.Blueprint, Result);
		return Result;
	});

	D.Register(TEXT("bp_remove_node"), TEXT("Removes a node and reports the links that were broken."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphNode* Node = NodeFromParams(Ctx, Params, Error);
		if (!Node) return FBABResult::Fail(Error);
		if (!Node->CanUserDeleteNode())
		{
			return FBABResult::Fail(FString::Printf(TEXT("Node %s (%s) cannot be deleted (entry/result nodes and some events are mandatory)."), *NodeId(Node), *NodeTitle(Node)));
		}
		TArray<TSharedPtr<FJsonValue>> Broken;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked || !Linked->GetOwningNodeUnchecked()) continue;
				TSharedRef<FJsonObject> Link = NewObject();
				Link->SetStringField(TEXT("pin"), Pin->PinName.ToString());
				Link->SetStringField(TEXT("other_node"), NodeId(Linked->GetOwningNode()));
				Link->SetStringField(TEXT("other_pin"), Linked->PinName.ToString());
				Broken.Add(ObjectValue(Link));
			}
		}
		FBABResult Result;
		Result.Data->SetStringField(TEXT("removed"), NodeId(Node));
		Result.Data->SetStringField(TEXT("title"), NodeTitle(Node));
		Result.Data->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
		Result.Data->SetArrayField(TEXT("broken_links"), Broken);
		FBlueprintEditorUtils::RemoveNode(Ctx.Blueprint, Node);
		return Result;
	});

	D.Register(TEXT("bp_update_node"), TEXT("Moves a node, sets its comment, enables/disables it, or renames a custom event / comment box."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphNode* Node = NodeFromParams(Ctx, Params, Error);
		if (!Node) return FBABResult::Fail(Error);

		FBABResult Result;
		Node->Modify();
		FVector2D Position;
		if (GetVector2D(Params, TEXT("pos"), Position) || GetVector2D(Params, TEXT("position"), Position))
		{
			Node->NodePosX = FMath::RoundToInt(Position.X);
			Node->NodePosY = FMath::RoundToInt(Position.Y);
		}
		if (HasField(Params, TEXT("comment")))
		{
			const FString Comment = GetString(Params, TEXT("comment"));
			if (UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node))
			{
				CommentNode->NodeComment = Comment;
			}
			else
			{
				Node->NodeComment = Comment;
				Node->bCommentBubbleVisible = !Comment.IsEmpty();
			}
		}
		if (HasField(Params, TEXT("enabled")))
		{
			Node->SetEnabledState(GetBool(Params, TEXT("enabled"), true) ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled, true);
		}
		const FString NewName = GetString(Params, TEXT("name"), GetString(Params, TEXT("title")));
		if (!NewName.IsEmpty())
		{
			if (Node->IsA<UK2Node_CustomEvent>() || Node->IsA<UEdGraphNode_Comment>())
			{
				Node->OnRenameNode(NewName);
			}
			else
			{
				Result.Warn(TEXT("Only custom events and comment boxes can be renamed."));
			}
		}
		if (HasField(Params, TEXT("width")) || HasField(Params, TEXT("height")))
		{
			Node->NodeWidth = GetInt(Params, TEXT("width"), Node->NodeWidth);
			Node->NodeHeight = GetInt(Params, TEXT("height"), Node->NodeHeight);
		}
		MarkModified(Ctx.Blueprint, Node);
		const FReadOptions Options;
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Node, Options));
		Result.Data->SetStringField(TEXT("text"), NodeToText(Node, Options));
		return Result;
	});

	D.Register(TEXT("bp_connect_pins"), TEXT("Connects two pins (from: output, to: input). Reports conversion nodes and replaced links."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphPin* From = PinFromParams(Ctx, Params, TEXT("from"), TEXT("from_node"), TEXT("from_pin"), TEXT("out"), Error);
		if (!From) return FBABResult::Fail(Error);
		UEdGraphPin* To = PinFromParams(Ctx, Params, TEXT("to"), TEXT("to_node"), TEXT("to_pin"), TEXT("in"), Error);
		if (!To) return FBABResult::Fail(Error);
		if (From->Direction == EGPD_Input && To->Direction == EGPD_Output)
		{
			Swap(From, To);
		}
		if (From->Direction == To->Direction)
		{
			return FBABResult::Fail(FString::Printf(TEXT("Both pins are %s pins: %s and %s. Connect an output to an input."), From->Direction == EGPD_Input ? TEXT("input") : TEXT("output"), *PinDescription(From), *PinDescription(To)));
		}
		if (From->LinkedTo.Contains(To))
		{
			FBABResult Already;
			Already.Data->SetBoolField(TEXT("connected"), true);
			Already.Data->SetStringField(TEXT("response"), TEXT("ALREADY_CONNECTED"));
			Already.Data->SetObjectField(TEXT("from"), PinRefJson(From));
			Already.Data->SetObjectField(TEXT("to"), PinRefJson(To));
			return Already;
		}

		const UEdGraphSchema* Schema = From->GetSchema();
		const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			return FBABResult::Fail(FString::Printf(TEXT("Cannot connect %s to %s: %s"), *PinDescription(From), *PinDescription(To), *Response.Message.ToString()));
		}

		UEdGraph* Graph = From->GetOwningNode()->GetGraph();
		TSet<UEdGraphNode*> NodesBefore;
		for (UEdGraphNode* Node : Graph->Nodes) NodesBefore.Add(Node);
		TArray<UEdGraphPin*> FromLinksBefore = From->LinkedTo;
		TArray<UEdGraphPin*> ToLinksBefore = To->LinkedTo;

		const bool bConnected = Schema->TryCreateConnection(From, To);
		if (!bConnected)
		{
			return FBABResult::Fail(FString::Printf(TEXT("The schema refused the connection %s -> %s: %s"), *PinDescription(From), *PinDescription(To), *Response.Message.ToString()));
		}

		FBABResult Result;
		Result.Data->SetBoolField(TEXT("connected"), true);
		Result.Data->SetStringField(TEXT("response"), ConnectionResponseName(Response.Response));
		if (!Response.Message.IsEmpty()) Result.Data->SetStringField(TEXT("message"), Response.Message.ToString());
		Result.Data->SetObjectField(TEXT("from"), PinRefJson(From));
		Result.Data->SetObjectField(TEXT("to"), PinRefJson(To));

		TArray<TSharedPtr<FJsonValue>> NewNodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !NodesBefore.Contains(Node))
			{
				NewNodes.Add(ObjectValue(NodeToJson(Node, FReadOptions())));
			}
		}
		Result.Data->SetArrayField(TEXT("new_nodes"), NewNodes);
		if (NewNodes.Num() > 0)
		{
			Result.Warn(FString::Printf(TEXT("The schema inserted %d conversion node(s) between the pins."), NewNodes.Num()));
		}
		TArray<TSharedPtr<FJsonValue>> Replaced;
		for (UEdGraphPin* Old : FromLinksBefore)
		{
			if (Old && !From->LinkedTo.Contains(Old)) Replaced.Add(ObjectValue(PinRefJson(Old)));
		}
		for (UEdGraphPin* Old : ToLinksBefore)
		{
			if (Old && !To->LinkedTo.Contains(Old)) Replaced.Add(ObjectValue(PinRefJson(Old)));
		}
		Result.Data->SetArrayField(TEXT("replaced_links"), Replaced);
		if (Replaced.Num() > 0)
		{
			Result.Warn(FString::Printf(TEXT("%d existing link(s) were replaced by this connection."), Replaced.Num()));
		}
		// Connecting to a disabled template event (BeginPlay, Tick...) enables it, as the editor does.
		for (UEdGraphNode* Node : { From->GetOwningNode(), To->GetOwningNode() })
		{
			if (EnableGhostNode(Node))
			{
				Result.Warn(FString::Printf(TEXT("Enabled the template node %s (%s) because it is now connected."), *NodeId(Node), *NodeTitle(Node)));
			}
		}
		MarkModified(Ctx.Blueprint);
		return Result;
	});

	D.Register(TEXT("bp_disconnect_pins"), TEXT("Breaks one link (from + to) or every link of a pin (pin)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		FBABResult Result;
		TArray<TSharedPtr<FJsonValue>> Broken;
		if (HasField(Params, TEXT("from")) && HasField(Params, TEXT("to")))
		{
			UEdGraphPin* From = PinFromParams(Ctx, Params, TEXT("from"), TEXT("from_node"), TEXT("from_pin"), TEXT(""), Error);
			if (!From) return FBABResult::Fail(Error);
			UEdGraphPin* To = PinFromParams(Ctx, Params, TEXT("to"), TEXT("to_node"), TEXT("to_pin"), TEXT(""), Error);
			if (!To) return FBABResult::Fail(Error);
			if (!From->LinkedTo.Contains(To))
			{
				return FBABResult::Fail(FString::Printf(TEXT("%s and %s are not connected."), *PinDescription(From), *PinDescription(To)));
			}
			From->GetSchema()->BreakSinglePinLink(From, To);
			TSharedRef<FJsonObject> Link = NewObject();
			Link->SetObjectField(TEXT("from"), PinRefJson(From));
			Link->SetObjectField(TEXT("to"), PinRefJson(To));
			Broken.Add(ObjectValue(Link));
		}
		else
		{
			UEdGraphPin* Pin = PinFromParams(Ctx, Params, TEXT("pin"), TEXT("node"), TEXT("pin"), TEXT(""), Error);
			if (!Pin) return FBABResult::Fail(Error);
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked) continue;
				TSharedRef<FJsonObject> Link = NewObject();
				Link->SetObjectField(TEXT("from"), PinRefJson(Pin));
				Link->SetObjectField(TEXT("to"), PinRefJson(Linked));
				Broken.Add(ObjectValue(Link));
			}
			Pin->GetSchema()->BreakPinLinks(*Pin, true);
		}
		Result.Data->SetArrayField(TEXT("broken"), Broken);
		Result.Data->SetNumberField(TEXT("count"), Broken.Num());
		MarkModified(Ctx.Blueprint);
		return Result;
	});

	D.Register(TEXT("bp_set_pin_default"), TEXT("Sets (or resets with reset=true) the default value of an input pin. Accepts numbers, booleans, strings, [x,y,z] and objects."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphPin* Pin = PinFromParams(Ctx, Params, TEXT("pin"), TEXT("node"), TEXT("pin"), TEXT("in"), Error);
		if (!Pin) return FBABResult::Fail(Error);
		if (Pin->Direction != EGPD_Input)
		{
			return FBABResult::Fail(FString::Printf(TEXT("%s is an output pin; only input pins have default values."), *PinDescription(Pin)));
		}
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return FBABResult::Fail(TEXT("Execution pins have no default value."));
		}
		const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Pin->GetSchema());
		if (!Schema) Schema = GetDefault<UEdGraphSchema_K2>();

		FBABResult Result;
		Pin->GetOwningNode()->Modify();
		const TSharedPtr<FJsonValue>* Value = Params.Values.Find(TEXT("value"));
		if (GetBool(Params, TEXT("reset"), false) || !Value || !Value->IsValid() || (*Value)->Type == EJson::Null)
		{
			Schema->ResetPinToAutogeneratedDefaultValue(Pin, true);
			Result.Data->SetBoolField(TEXT("reset"), true);
		}
		else
		{
			TArray<FString> Warnings;
			const FString SetError = SetPinDefaultChecked(Schema, Pin, *Value, Warnings);
			for (const FString& W : Warnings) Result.Warn(W);
			if (!SetError.IsEmpty())
			{
				Result.Error(SetError);
				return Result;
			}
		}
		MarkModified(Ctx.Blueprint, Pin->GetOwningNode());
		Result.Data->SetObjectField(TEXT("pin"), PinToJson(Pin, FReadOptions()));
		Result.Data->SetStringField(TEXT("node"), NodeId(Pin->GetOwningNode()));
		Result.Data->SetStringField(TEXT("default"), PinDefaultToString(Pin));
		return Result;
	});

	D.Register(TEXT("bp_add_pin"), TEXT("Adds an input pin to nodes that support it (Sequence, MakeArray, Select, Switch, MultiGate, math operators)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphNode* Node = NodeFromParams(Ctx, Params, Error);
		if (!Node) return FBABResult::Fail(Error);
		const int32 PinsBefore = Node->Pins.Num();
		Node->Modify();
		if (IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(Node))
		{
			if (!AddPin->CanAddPin()) return FBABResult::Fail(TEXT("This node cannot take more pins."));
			AddPin->AddInputPin();
		}
		else if (UK2Node_Switch* Switch = Cast<UK2Node_Switch>(Node))
		{
			Switch->AddPinToSwitchNode();
		}
		else
		{
			return FBABResult::Fail(FString::Printf(TEXT("Node %s (%s) does not support adding pins."), *NodeId(Node), *Node->GetClass()->GetName()));
		}
		MarkModified(Ctx.Blueprint, Node);
		FBABResult Result;
		TArray<TSharedPtr<FJsonValue>> NewPins;
		for (int32 Index = PinsBefore; Index < Node->Pins.Num(); ++Index)
		{
			NewPins.Add(ObjectValue(PinToJson(Node->Pins[Index], FReadOptions())));
		}
		Result.Data->SetArrayField(TEXT("new_pins"), NewPins);
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Node, FReadOptions()));
		return Result;
	});

	D.Register(TEXT("bp_remove_pin"), TEXT("Removes a dynamic input pin added with bp_add_pin. Connected pins need force=true; broken links are reported."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphPin* Pin = PinFromParams(Ctx, Params, TEXT("pin"), TEXT("node"), TEXT("pin"), TEXT("in"), Error);
		if (!Pin) return FBABResult::Fail(Error);
		UEdGraphNode* Node = Pin->GetOwningNode();
		TArray<TSharedPtr<FJsonValue>> Broken;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked && Linked->GetOwningNodeUnchecked()) Broken.Add(ObjectValue(PinRefJson(Linked)));
		}
		FBABResult Result;
		Result.Data->SetArrayField(TEXT("broken_links"), Broken);
		if (Broken.Num() > 0 && !GetBool(Params, TEXT("force"), false))
		{
			Result.Error(FString::Printf(TEXT("Pin %s is connected to %d pin(s) (see broken_links). Pass force=true to remove it anyway."), *PinDescription(Pin), Broken.Num()));
			return Result;
		}
		Node->Modify();
		if (IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(Node))
		{
			if (!AddPin->CanRemovePin(Pin)) return FBABResult::Fail(TEXT("This pin cannot be removed."));
			AddPin->RemoveInputPin(Pin);
		}
		else if (UK2Node_Switch* Switch = Cast<UK2Node_Switch>(Node))
		{
			if (!Switch->CanRemoveExecutionPin(Pin)) return FBABResult::Fail(TEXT("This pin cannot be removed."));
			Switch->RemovePinFromSwitchNode(Pin);
		}
		else
		{
			return FBABResult::Fail(FString::Printf(TEXT("Node %s (%s) does not support removing pins."), *NodeId(Node), *Node->GetClass()->GetName()));
		}
		MarkModified(Ctx.Blueprint, Node);
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Node, FReadOptions()));
		return Result;
	});

	D.Register(TEXT("bp_split_pin"), TEXT("Splits a struct pin into its members (e.g. Vector into X, Y, Z)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphPin* Pin = PinFromParams(Ctx, Params, TEXT("pin"), TEXT("node"), TEXT("pin"), TEXT(""), Error);
		if (!Pin) return FBABResult::Fail(Error);
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (Pin->SubPins.Num() > 0) return FBABResult::Fail(TEXT("The pin is already split."));
		if (!Schema->CanSplitStructPin(*Pin)) return FBABResult::Fail(FString::Printf(TEXT("%s cannot be split (not a struct pin, or connected)."), *PinDescription(Pin)));
		Pin->GetOwningNode()->Modify();
		Schema->SplitPin(Pin, true);
		MarkModified(Ctx.Blueprint, Pin->GetOwningNode());
		FBABResult Result;
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Pin->GetOwningNode(), FReadOptions()));
		return Result;
	});

	D.Register(TEXT("bp_recombine_pin"), TEXT("Recombines a split struct pin (pass the parent pin or any sub pin)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphPin* Pin = PinFromParams(Ctx, Params, TEXT("pin"), TEXT("node"), TEXT("pin"), TEXT(""), Error);
		if (!Pin) return FBABResult::Fail(Error);
		UEdGraphPin* Target = Pin;
		if (Pin->SubPins.Num() > 0) Target = Pin->SubPins[0];
		if (!Target->ParentPin) return FBABResult::Fail(FString::Printf(TEXT("%s is not a split pin."), *PinDescription(Pin)));
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		Target->GetOwningNode()->Modify();
		Schema->RecombinePin(Target);
		MarkModified(Ctx.Blueprint, Pin->GetOwningNode());
		FBABResult Result;
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Pin->GetOwningNode(), FReadOptions()));
		return Result;
	});

	D.Register(TEXT("bp_set_node_property"), TEXT("Sets a reflected property on a node (advanced; e.g. TargetType on a cast) and reconstructs it."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphNode* Node = NodeFromParams(Ctx, Params, Error);
		if (!Node) return FBABResult::Fail(Error);
		const FString PropertyName = GetString(Params, TEXT("property"));
		const TSharedPtr<FJsonValue>* Value = Params.Values.Find(TEXT("value"));
		if (PropertyName.IsEmpty() || !Value)
		{
			return FBABResult::Fail(TEXT("Pass \"property\" and \"value\"."));
		}
		if (!ApplyNodeProperty(Node, PropertyName, *Value, Error))
		{
			return FBABResult::Fail(Error);
		}
		if (GetBool(Params, TEXT("reconstruct"), true))
		{
			Node->ReconstructNode();
		}
		MarkModified(Ctx.Blueprint, Node);
		FBABResult Result;
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Node, FReadOptions()));
		Result.Data->SetStringField(TEXT("text"), NodeToText(Node, FReadOptions()));
		return Result;
	});

	D.Register(TEXT("bp_reconstruct_node"), TEXT("Rebuilds the pins of a node (after changing its properties or when it shows stale pins)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UEdGraphNode* Node = NodeFromParams(Ctx, Params, Error);
		if (!Node) return FBABResult::Fail(Error);
		Node->Modify();
		Node->ReconstructNode();
		MarkModified(Ctx.Blueprint, Node);
		FBABResult Result;
		Result.Data->SetObjectField(TEXT("node"), NodeToJson(Node, FReadOptions()));
		Result.Data->SetStringField(TEXT("text"), NodeToText(Node, FReadOptions()));
		return Result;
	});

	D.Register(TEXT("bp_export_text"), TEXT("Exports nodes (all, or the given ids) in the editor copy/paste text format."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, true, Ctx, Failure)) return Failure;
		TArray<FString> Ids;
		GetStringArray(Params, TEXT("nodes"), Ids);
		TSet<UObject*> Objects;
		if (Ids.Num() == 0)
		{
			for (UEdGraphNode* Node : Ctx.Graph->Nodes) if (Node) Objects.Add(Node);
		}
		else
		{
			for (const FString& Id : Ids)
			{
				FString Error;
				UEdGraphNode* Node = FindNode(Ctx.Blueprint, Ctx.Graph, Id, Error);
				if (!Node) return FBABResult::Fail(Error);
				Objects.Add(Node);
			}
		}
		FString Text;
		FEdGraphUtilities::ExportNodesToText(Objects, Text);
		FBABResult Result;
		Result.Data->SetStringField(TEXT("text"), Text);
		Result.Data->SetNumberField(TEXT("node_count"), Objects.Num());
		return Result;
	});

	D.Register(TEXT("bp_import_text"), TEXT("Pastes nodes from the editor copy/paste text format into a graph."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, true, Ctx, Failure)) return Failure;
		const FString Text = GetString(Params, TEXT("text"));
		if (Text.IsEmpty()) return FBABResult::Fail(TEXT("Missing \"text\" (content copied from a Blueprint graph)."));
		if (!FEdGraphUtilities::CanImportNodesFromText(Ctx.Graph, Text))
		{
			return FBABResult::Fail(TEXT("The text does not contain nodes that can be pasted into this graph."));
		}
		Ctx.Graph->Modify();
		TSet<UEdGraphNode*> Imported;
		FEdGraphUtilities::ImportNodesFromText(Ctx.Graph, Text, Imported);
		if (Imported.Num() == 0) return FBABResult::Fail(TEXT("No node was imported."));

		FVector2D Average(0, 0);
		for (UEdGraphNode* Node : Imported) Average += FVector2D(Node->NodePosX, Node->NodePosY);
		Average /= Imported.Num();
		FVector2D Target;
		const bool bHasTarget = GetVector2D(Params, TEXT("pos"), Target) || GetVector2D(Params, TEXT("position"), Target);
		if (!bHasTarget) Target = DefaultPositionForNewNode(Ctx.Graph, nullptr);
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UEdGraphNode* Node : Imported)
		{
			Node->NodePosX = FMath::RoundToInt(Node->NodePosX - Average.X + Target.X);
			Node->NodePosY = FMath::RoundToInt(Node->NodePosY - Average.Y + Target.Y);
			Node->SnapToGrid(16);
			Node->CreateNewGuid();
			Nodes.Add(ObjectValue(NodeToJson(Node, FReadOptions())));
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);
		FBABResult Result;
		Result.Data->SetArrayField(TEXT("nodes"), Nodes);
		Result.Data->SetNumberField(TEXT("count"), Nodes.Num());
		return Result;
	});

	D.Register(TEXT("bp_auto_layout"), TEXT("Re-arranges the nodes of a graph (or the given ids) left-to-right along their connections."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, true, Ctx, Failure)) return Failure;
		FLayoutOptions Options;
		GetVector2D(Params, TEXT("origin"), Options.Origin);
		Options.ColumnGap = (float)GetNumber(Params, TEXT("column_gap"), Options.ColumnGap);
		Options.RowGap = (float)GetNumber(Params, TEXT("row_gap"), Options.RowGap);
		TArray<FString> Ids;
		GetStringArray(Params, TEXT("nodes"), Ids);
		for (const FString& Id : Ids)
		{
			FString Error;
			UEdGraphNode* Node = FindNode(Ctx.Blueprint, Ctx.Graph, Id, Error);
			if (!Node) return FBABResult::Fail(Error);
			Options.Subset.Add(Node);
		}
		const int32 Moved = AutoLayoutGraph(Ctx.Graph, Options);
		if (Moved > 0) MarkModified(Ctx.Blueprint);
		FBABResult Result;
		Result.Data->SetNumberField(TEXT("moved"), Moved);
		TArray<TSharedPtr<FJsonValue>> Positions;
		for (UEdGraphNode* Node : Ctx.Graph->Nodes)
		{
			if (!Node || (Options.Subset.Num() > 0 && !Options.Subset.Contains(Node))) continue;
			TSharedRef<FJsonObject> Entry = NewObject();
			Entry->SetStringField(TEXT("id"), NodeId(Node));
			Entry->SetStringField(TEXT("title"), NodeTitle(Node));
			TArray<TSharedPtr<FJsonValue>> Pos;
			Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
			Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
			Entry->SetArrayField(TEXT("pos"), Pos);
			Positions.Add(ObjectValue(Entry));
		}
		Result.Data->SetArrayField(TEXT("positions"), Positions);
		return Result;
	});
}
