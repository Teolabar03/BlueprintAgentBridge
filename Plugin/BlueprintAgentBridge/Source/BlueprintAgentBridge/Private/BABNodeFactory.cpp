// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABNodeFactory.h"
#include "BABContext.h"
#include "BABTypes.h"
#include "BlueprintAgentBridgeModule.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MultiGate.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ClassDynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Select.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_Timeline.h"
#include "K2Node_GetClassDefaults.h"
#include "K2Node_FormatText.h"
#include "K2Node_InputKey.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace BAB
{
	/**
	 * Creates a node the same way the editor menu does (UBlueprintNodeSpawner::SpawnEdGraphNode):
	 * configure, allocate pins, PostPlacedNewNode, then add to the graph. Some nodes
	 * (SpawnActorFromClass, Timeline) rely on this order.
	 */
	template <typename TNode, typename TConfigure>
	static TNode* SpawnTyped(UEdGraph* Graph, const FVector2D& Position, TConfigure Configure)
	{
		TNode* Node = NewObject<TNode>(Graph, TNode::StaticClass(), NAME_None, RF_Transactional);
		Node->CreateNewGuid();
		Node->NodePosX = FMath::RoundToInt(Position.X);
		Node->NodePosY = FMath::RoundToInt(Position.Y);
		Configure(Node);
		Node->AllocateDefaultPins();
		Node->PostPlacedNewNode();
		Graph->Modify();
		Graph->AddNode(Node, /*bUserAction*/ true, /*bSelectNewNode*/ false);
		return Node;
	}

	static FString SpecString(const FJsonObject& Spec, const TCHAR* Primary, const TCHAR* Alias = nullptr, const TCHAR* Alias2 = nullptr)
	{
		FString Value = GetString(Spec, Primary);
		if (Value.IsEmpty() && Alias) Value = GetString(Spec, Alias);
		if (Value.IsEmpty() && Alias2) Value = GetString(Spec, Alias2);
		return Value;
	}

	TArray<FString> GetSupportedNodeTypes()
	{
		return {
			TEXT("call_function"), TEXT("event"), TEXT("custom_event"), TEXT("variable_get"), TEXT("variable_set"),
			TEXT("branch"), TEXT("sequence"), TEXT("multi_gate"), TEXT("macro"), TEXT("cast"), TEXT("class_cast"), TEXT("spawn_actor"),
			TEXT("knot"), TEXT("comment"), TEXT("self"), TEXT("make_array"), TEXT("make_struct"), TEXT("break_struct"), TEXT("select"),
			TEXT("switch_enum"), TEXT("switch_int"), TEXT("switch_string"), TEXT("switch_name"), TEXT("timeline"),
			TEXT("get_class_defaults"), TEXT("format_text"), TEXT("input_key"), TEXT("spawner"), TEXT("node_class")
		};
	}

	UFunction* ResolveEventFunction(UBlueprint* BP, const FString& InName, const FString& ClassName, FString& OutError)
	{
		FString Name = InName.TrimStartAndEnd();
		Name.RemoveFromStart(TEXT("Event "), ESearchCase::IgnoreCase);
		Name.ReplaceInline(TEXT(" "), TEXT(""));
		if (Name.IsEmpty())
		{
			OutError = TEXT("Missing event name (e.g. BeginPlay, Tick, ActorBeginOverlap).");
			return nullptr;
		}

		TArray<UClass*> Scopes;
		if (!ClassName.IsEmpty())
		{
			UClass* Class = ResolveClass(ClassName, OutError);
			if (!Class) return nullptr;
			Scopes.Add(Class);
		}
		else
		{
			if (BP->ParentClass) Scopes.Add(BP->ParentClass);
			for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
			{
				if (Interface.Interface) Scopes.Add(Interface.Interface);
			}
		}

		const TArray<FString> Candidates = { Name, TEXT("Receive") + Name, TEXT("K2_") + Name, TEXT("On") + Name };
		for (UClass* Scope : Scopes)
		{
			for (TFieldIterator<UFunction> It(Scope, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Function = *It;
				if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)) continue;
				for (const FString& Candidate : Candidates)
				{
					if (Function->GetName().Equals(Candidate, ESearchCase::IgnoreCase)) return Function;
				}
				FString Display = Function->GetDisplayNameText().ToString();
				Display.ReplaceInline(TEXT(" "), TEXT(""));
				Display.RemoveFromStart(TEXT("Event"));
				if (Display.Equals(Name, ESearchCase::IgnoreCase)) return Function;
			}
		}

		TArray<FString> Available;
		for (UClass* Scope : Scopes)
		{
			for (TFieldIterator<UFunction> It(Scope, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (It->HasAnyFunctionFlags(FUNC_BlueprintEvent) && Available.Num() < 60)
				{
					FString Short = It->GetName();
					Short.RemoveFromStart(TEXT("Receive"));
					Available.AddUnique(Short);
				}
			}
		}
		Available.Sort();
		OutError = FString::Printf(TEXT("Event \"%s\" not found on %s. Overridable events include: %s"), *InName, Scopes.Num() ? *ClassDisplayName(Scopes[0]) : TEXT("the parent class"), *FString::Join(Available, TEXT(", ")));
		return nullptr;
	}

	UEdGraph* FindMacroGraph(UBlueprint* BP, const FString& InMacroName, const FString& LibraryPath, FString& OutError)
	{
		const FString MacroName = InMacroName.TrimStartAndEnd();
		if (MacroName.IsEmpty())
		{
			OutError = TEXT("Missing macro name (e.g. ForEachLoop, ForLoop, DoOnce, Gate, FlipFlop, IsValid).");
			return nullptr;
		}
		TArray<UBlueprint*> Libraries;
		if (!LibraryPath.IsEmpty())
		{
			UBlueprint* Library = FindBlueprint(LibraryPath, OutError);
			if (!Library) return nullptr;
			Libraries.Add(Library);
		}
		else
		{
			if (BP) Libraries.Add(BP);
			if (UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")))
			{
				Libraries.Add(StandardMacros);
			}
		}
		const FString Compact = MacroName.Replace(TEXT(" "), TEXT(""));
		for (UBlueprint* Library : Libraries)
		{
			for (UEdGraph* Graph : Library->MacroGraphs)
			{
				if (Graph && (Graph->GetName().Equals(MacroName, ESearchCase::IgnoreCase) || Graph->GetName().Replace(TEXT(" "), TEXT("")).Equals(Compact, ESearchCase::IgnoreCase)))
				{
					return Graph;
				}
			}
		}
		TArray<FString> Names;
		for (UBlueprint* Library : Libraries)
		{
			for (UEdGraph* Graph : Library->MacroGraphs) if (Graph) Names.Add(Graph->GetName());
		}
		Names.Sort();
		OutError = FString::Printf(TEXT("Macro \"%s\" not found. Available macros: %s"), *MacroName, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	UBlueprintNodeSpawner* FindSpawnerBySignature(const FString& Signature, UObject** OutOwner)
	{
		FBlueprintActionDatabase& Database = FBlueprintActionDatabase::Get();
		const FBlueprintActionDatabase::FActionRegistry& Registry = Database.GetAllActions();
		for (const auto& Pair : Registry)
		{
			for (UBlueprintNodeSpawner* Spawner : Pair.Value)
			{
				if (Spawner && Spawner->GetSpawnerSignature().ToString() == Signature)
				{
					if (OutOwner) *OutOwner = Pair.Key.ResolveObjectPtr();
					return Spawner;
				}
			}
		}
		return nullptr;
	}

	bool ApplyNodeProperty(UEdGraphNode* Node, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("No node.");
			return false;
		}
		FProperty* Property = Node->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					Property = *It;
					break;
				}
			}
		}
		if (!Property)
		{
			TArray<FString> Names;
			for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
			{
				if (It->GetOwnerClass() && !It->GetOwnerClass()->IsChildOf(UEdGraphNode::StaticClass()) ) continue;
				if (It->GetOwnerClass() == UEdGraphNode::StaticClass()) continue;
				Names.Add(It->GetName());
			}
			OutError = FString::Printf(TEXT("Property \"%s\" not found on %s. Node properties: %s"), *PropertyName, *Node->GetClass()->GetName(), *FString::Join(Names, TEXT(", ")));
			return false;
		}

		FString Text;
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const FString AsString = Value.IsValid() && Value->Type == EJson::String ? Value->AsString() : FString();
			if (AsString.IsEmpty() || AsString.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				Text = TEXT("None");
			}
			else if (CastField<FClassProperty>(Property) || CastField<FSoftClassProperty>(Property) || (ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf(UClass::StaticClass())))
			{
				UClass* Class = ResolveClass(AsString, OutError);
				if (!Class) return false;
				Text = Class->GetPathName();
			}
			else
			{
				UObject* Object = FindObject<UObject>(nullptr, *AsString);
				if (!Object) Object = StaticLoadObject(UObject::StaticClass(), nullptr, *AsString, nullptr, LOAD_NoWarn, nullptr);
				if (!Object)
				{
					// maybe a struct or enum name used for StructType / Enum properties
					FString Ignored;
					if (ObjectProperty->PropertyClass == UScriptStruct::StaticClass()) Object = ResolveStruct(AsString, Ignored);
					else if (ObjectProperty->PropertyClass == UEnum::StaticClass()) Object = ResolveEnum(AsString, Ignored);
				}
				if (!Object)
				{
					OutError = FString::Printf(TEXT("Object not found for property %s: %s"), *PropertyName, *AsString);
					return false;
				}
				Text = Object->GetPathName();
			}
		}
		else if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			Text = JsonValueToUEString(Value, StructProperty->Struct);
		}
		else
		{
			Text = JsonValueToUEString(Value, nullptr);
		}

		Node->Modify();
		const TCHAR* Result = Property->ImportText_Direct(*Text, Property->ContainerPtrToValuePtr<void>(Node), Node, PPF_None);
		if (!Result)
		{
			OutError = FString::Printf(TEXT("Could not parse \"%s\" for property %s (%s)."), *Text, *PropertyName, *Property->GetCPPType());
			return false;
		}
		return true;
	}

	static UEdGraphNode* SpawnByClass(UEdGraph* Graph, UClass* NodeClass, const FVector2D& Position, const TSharedPtr<FJsonObject>& Properties, FString& OutError, TArray<FString>& OutWarnings)
	{
		if (!NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
		{
			OutError = FString::Printf(TEXT("%s is not a graph node class."), *NodeClass->GetName());
			return nullptr;
		}
		if (NodeClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("%s is abstract."), *NodeClass->GetName());
			return nullptr;
		}
		// Same sequence as UBlueprintNodeSpawner::SpawnEdGraphNode (UEdGraph::CreateNode is protected).
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
		Node->CreateNewGuid();
		Node->NodePosX = FMath::RoundToInt(Position.X);
		Node->NodePosY = FMath::RoundToInt(Position.Y);
		if (Properties.IsValid())
		{
			for (const auto& Pair : Properties->Values)
			{
				FString Error;
				if (!ApplyNodeProperty(Node, Pair.Key, Pair.Value, Error))
				{
					OutWarnings.Add(Error);
				}
			}
		}
		Node->AllocateDefaultPins();
		Node->PostPlacedNewNode();
		Graph->Modify();
		Graph->AddNode(Node, /*bUserAction*/ true, /*bSelectNewNode*/ false);
		return Node;
	}

	static bool RequireUbergraph(UBlueprint* BP, UEdGraph* Graph, const TCHAR* What, FString& OutError)
	{
		if (GraphKind(BP, Graph) != TEXT("ubergraph"))
		{
			OutError = FString::Printf(TEXT("%s can only be placed in an event graph, not in %s (%s)."), What, *Graph->GetName(), *GraphKind(BP, Graph));
			return false;
		}
		return true;
	}

	UEdGraphNode* CreateNodeFromSpec(UBlueprint* BP, UEdGraph* Graph, const FJsonObject& Spec, const FVector2D& Position, FString& OutError, TArray<FString>& OutWarnings)
	{
		FString Type = SpecString(Spec, TEXT("type"), TEXT("kind"), TEXT("node_type")).ToLower();
		Type.ReplaceInline(TEXT("-"), TEXT("_"));
		Type.ReplaceInline(TEXT(" "), TEXT("_"));
		if (Type.IsEmpty())
		{
			// Infer from the fields present
			if (HasField(Spec, TEXT("function"))) Type = TEXT("call_function");
			else if (HasField(Spec, TEXT("event"))) Type = TEXT("event");
			else if (HasField(Spec, TEXT("macro"))) Type = TEXT("macro");
			else if (HasField(Spec, TEXT("signature"))) Type = TEXT("spawner");
			else if (HasField(Spec, TEXT("node_class"))) Type = TEXT("node_class");
			else
			{
				OutError = FString::Printf(TEXT("Missing node \"type\". Supported: %s"), *FString::Join(GetSupportedNodeTypes(), TEXT(", ")));
				return nullptr;
			}
		}

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		if (Type == TEXT("call_function") || Type == TEXT("function") || Type == TEXT("call") || Type == TEXT("callfunction") || Type == TEXT("call_parent_function"))
		{
			const FString FunctionSpec = SpecString(Spec, TEXT("function"), TEXT("name"));
			if (FunctionSpec.IsEmpty())
			{
				OutError = TEXT("call_function needs \"function\" (e.g. \"PrintString\", \"KismetSystemLibrary.PrintString\", \"self.MyFunction\").");
				return nullptr;
			}
			UFunction* Function = ResolveFunction(BP, FunctionSpec, GetString(Spec, TEXT("class")), OutError);
			if (!Function) return nullptr;
			const bool bParent = Type == TEXT("call_parent_function") || GetBool(Spec, TEXT("parent"), false);
			if (bParent)
			{
				return SpawnTyped<UK2Node_CallParentFunction>(Graph, Position, [Function](UK2Node_CallParentFunction* Node) { Node->SetFromFunction(Function); });
			}
			if (Function->HasMetaData(TEXT("Latent")) && GraphKind(BP, Graph) != TEXT("ubergraph"))
			{
				OutWarnings.Add(FString::Printf(TEXT("%s is a latent function; it only compiles inside an event graph."), *FunctionDisplay(Function)));
			}
			return SpawnTyped<UK2Node_CallFunction>(Graph, Position, [Function](UK2Node_CallFunction* Node) { Node->SetFromFunction(Function); });
		}

		if (Type == TEXT("event"))
		{
			if (!RequireUbergraph(BP, Graph, TEXT("An event"), OutError)) return nullptr;
			const FString Name = SpecString(Spec, TEXT("name"), TEXT("event"), TEXT("function"));
			UFunction* Function = ResolveEventFunction(BP, Name, GetString(Spec, TEXT("class")), OutError);
			if (!Function) return nullptr;
			UClass* OwnerClass = Function->GetOwnerClass();
			if (UBlueprint* OwnerBP = UBlueprint::GetBlueprintFromClass(OwnerClass))
			{
				if (OwnerBP->GeneratedClass) OwnerClass = OwnerBP->GeneratedClass;
			}
			if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(BP, OwnerClass, Function->GetFName()))
			{
				// New Blueprints come with disabled template events (BeginPlay, Tick, ActorBeginOverlap).
				// Like the editor menu, reuse and enable that node instead of creating a duplicate.
				if (!Existing->IsNodeEnabled() && Existing->GetGraph() == Graph)
				{
					if (!EnableGhostNode(Existing))
					{
						Existing->Modify();
						Existing->SetEnabledState(ENodeEnabledState::Enabled, true);
					}
					if (HasField(Spec, TEXT("pos")) || (Position.X != 0.0 || Position.Y != 0.0))
					{
						Existing->NodePosX = FMath::RoundToInt(Position.X);
						Existing->NodePosY = FMath::RoundToInt(Position.Y);
					}
					OutWarnings.Add(FString::Printf(TEXT("Enabled the existing template event node %s instead of creating a duplicate."), *NodeId(Existing)));
					return Existing;
				}
				OutError = FString::Printf(TEXT("Event %s already exists in this Blueprint as node %s%s (an event can only appear once). Connect to it instead."), *Function->GetName(), *NodeId(Existing), Existing->GetGraph() != Graph ? *FString::Printf(TEXT(" in graph %s"), *Existing->GetGraph()->GetName()) : TEXT(""));
				return nullptr;
			}
			return SpawnTyped<UK2Node_Event>(Graph, Position, [Function, OwnerClass](UK2Node_Event* Node)
			{
				Node->EventReference.SetExternalMember(Function->GetFName(), OwnerClass);
				Node->bOverrideFunction = true;
			});
		}

		if (Type == TEXT("custom_event") || Type == TEXT("customevent"))
		{
			if (!RequireUbergraph(BP, Graph, TEXT("A custom event"), OutError)) return nullptr;
			FString Name = SpecString(Spec, TEXT("name"), TEXT("event")).TrimStartAndEnd();
			if (Name.IsEmpty()) Name = TEXT("CustomEvent");
			Name.ReplaceInline(TEXT(" "), TEXT(""));
			FName EventName(*Name);
			if (UClass* Self = GetBlueprintSelfClass(BP))
			{
				if (Self->FindFunctionByName(EventName))
				{
					if (GetBool(Spec, TEXT("unique"), false))
					{
						EventName = FBlueprintEditorUtils::FindUniqueKismetName(BP, Name);
						OutWarnings.Add(FString::Printf(TEXT("\"%s\" was taken; the custom event was named %s."), *Name, *EventName.ToString()));
					}
					else
					{
						OutError = FString::Printf(TEXT("A function or event named \"%s\" already exists in this Blueprint. Choose another name or pass \"unique\": true."), *Name);
						return nullptr;
					}
				}
			}
			UK2Node_CustomEvent* Node = SpawnTyped<UK2Node_CustomEvent>(Graph, Position, [EventName](UK2Node_CustomEvent* N) { N->CustomFunctionName = EventName; });
			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if (Spec.TryGetArrayField(TEXT("inputs"), Inputs) && Inputs)
			{
				for (const TSharedPtr<FJsonValue>& Input : *Inputs)
				{
					const TSharedPtr<FJsonObject>* InputObject = nullptr;
					if (!Input.IsValid() || !Input->TryGetObject(InputObject) || !InputObject) continue;
					const FString PinName = GetString(**InputObject, TEXT("name"));
					FEdGraphPinType PinType;
					FString TypeError;
					if (PinName.IsEmpty() || !ParsePinType(GetString(**InputObject, TEXT("type")), PinType, TypeError))
					{
						OutWarnings.Add(FString::Printf(TEXT("Skipped input \"%s\": %s"), *PinName, *TypeError));
						continue;
					}
					Node->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Output, true);
				}
			}
			return Node;
		}

		if (Type == TEXT("variable_get") || Type == TEXT("variable_set") || Type == TEXT("get") || Type == TEXT("set") || Type == TEXT("getter") || Type == TEXT("setter") || Type == TEXT("variableget") || Type == TEXT("variableset"))
		{
			const bool bSet = Type.Contains(TEXT("set"));
			const FString VarName = SpecString(Spec, TEXT("variable"), TEXT("name"), TEXT("var")).TrimStartAndEnd();
			if (VarName.IsEmpty())
			{
				OutError = TEXT("variable_get/variable_set need \"variable\" (the variable name).");
				return nullptr;
			}
			const FString ClassName = GetString(Spec, TEXT("class"));
			bool bSelfContext = true;
			UClass* OwnerClass = nullptr;
			if (!ClassName.IsEmpty() && !ClassName.Equals(TEXT("self"), ESearchCase::IgnoreCase))
			{
				OwnerClass = ResolveClass(ClassName, OutError);
				if (!OwnerClass) return nullptr;
				bSelfContext = false;
			}
			else
			{
				OwnerClass = GetBlueprintSelfClass(BP);
			}
			if (!OwnerClass)
			{
				OutError = TEXT("Cannot determine the class that owns the variable.");
				return nullptr;
			}
			FProperty* Property = FindFProperty<FProperty>(OwnerClass, FName(*VarName));
			if (!Property)
			{
				for (TFieldIterator<FProperty> It(OwnerClass); It; ++It)
				{
					if (It->GetName().Equals(VarName, ESearchCase::IgnoreCase) || It->GetDisplayNameText().ToString().Replace(TEXT(" "), TEXT("")).Equals(VarName.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase))
					{
						Property = *It;
						break;
					}
				}
			}
			if (!Property)
			{
				TArray<FString> Names;
				for (const FBPVariableDescription& Var : BP->NewVariables) Names.Add(Var.VarName.ToString());
				for (TFieldIterator<FProperty> It(OwnerClass); It && Names.Num() < 80; ++It)
				{
					if (It->HasAnyPropertyFlags(CPF_BlueprintVisible)) Names.AddUnique(It->GetName());
				}
				OutError = FString::Printf(TEXT("Variable \"%s\" not found on %s. Known variables: %s"), *VarName, *ClassDisplayName(OwnerClass), *FString::Join(Names, TEXT(", ")));
				return nullptr;
			}
			if (bSet)
			{
				return SpawnTyped<UK2Node_VariableSet>(Graph, Position, [Property, bSelfContext, OwnerClass](UK2Node_VariableSet* Node) { Node->SetFromProperty(Property, bSelfContext, OwnerClass); });
			}
			return SpawnTyped<UK2Node_VariableGet>(Graph, Position, [Property, bSelfContext, OwnerClass](UK2Node_VariableGet* Node) { Node->SetFromProperty(Property, bSelfContext, OwnerClass); });
		}

		if (Type == TEXT("branch") || Type == TEXT("if") || Type == TEXT("ifthenelse") || Type == TEXT("if_then_else"))
		{
			return SpawnTyped<UK2Node_IfThenElse>(Graph, Position, [](UK2Node_IfThenElse*) {});
		}
		if (Type == TEXT("sequence") || Type == TEXT("execution_sequence"))
		{
			return SpawnTyped<UK2Node_ExecutionSequence>(Graph, Position, [](UK2Node_ExecutionSequence*) {});
		}
		if (Type == TEXT("multi_gate") || Type == TEXT("multigate"))
		{
			return SpawnTyped<UK2Node_MultiGate>(Graph, Position, [](UK2Node_MultiGate*) {});
		}
		if (Type == TEXT("macro"))
		{
			UEdGraph* MacroGraph = FindMacroGraph(BP, SpecString(Spec, TEXT("macro"), TEXT("name")), GetString(Spec, TEXT("library")), OutError);
			if (!MacroGraph) return nullptr;
			return SpawnTyped<UK2Node_MacroInstance>(Graph, Position, [MacroGraph](UK2Node_MacroInstance* Node) { Node->SetMacroGraph(MacroGraph); });
		}
		if (Type == TEXT("cast") || Type == TEXT("dynamic_cast") || Type == TEXT("cast_to"))
		{
			UClass* Target = ResolveClass(SpecString(Spec, TEXT("class"), TEXT("target"), TEXT("to")), OutError);
			if (!Target) return nullptr;
			UK2Node_DynamicCast* Node = SpawnTyped<UK2Node_DynamicCast>(Graph, Position, [Target](UK2Node_DynamicCast* N) { N->TargetType = Target; });
			Node->SetPurity(GetBool(Spec, TEXT("pure"), false));
			return Node;
		}
		if (Type == TEXT("class_cast"))
		{
			UClass* Target = ResolveClass(SpecString(Spec, TEXT("class"), TEXT("target"), TEXT("to")), OutError);
			if (!Target) return nullptr;
			UK2Node_ClassDynamicCast* Node = SpawnTyped<UK2Node_ClassDynamicCast>(Graph, Position, [Target](UK2Node_ClassDynamicCast* N) { N->TargetType = Target; });
			Node->SetPurity(GetBool(Spec, TEXT("pure"), false));
			return Node;
		}
		if (Type == TEXT("spawn_actor") || Type == TEXT("spawnactor") || Type == TEXT("spawn_actor_from_class"))
		{
			UK2Node_SpawnActorFromClass* Node = SpawnTyped<UK2Node_SpawnActorFromClass>(Graph, Position, [](UK2Node_SpawnActorFromClass*) {});
			const FString ClassName = GetString(Spec, TEXT("class"));
			if (!ClassName.IsEmpty())
			{
				UClass* Class = ResolveClass(ClassName, OutError);
				if (!Class)
				{
					OutWarnings.Add(OutError);
					OutError.Reset();
				}
				else if (UEdGraphPin* ClassPin = Node->GetClassPin())
				{
					Schema->TrySetDefaultObject(*ClassPin, Class);
				}
			}
			return Node;
		}
		if (Type == TEXT("knot") || Type == TEXT("reroute"))
		{
			return SpawnTyped<UK2Node_Knot>(Graph, Position, [](UK2Node_Knot*) {});
		}
		if (Type == TEXT("comment"))
		{
			const FString Text = SpecString(Spec, TEXT("text"), TEXT("comment"), TEXT("title"));
			const int32 Width = GetInt(Spec, TEXT("width"), 400);
			const int32 Height = GetInt(Spec, TEXT("height"), 200);
			return SpawnTyped<UEdGraphNode_Comment>(Graph, Position, [&](UEdGraphNode_Comment* Node)
			{
				Node->NodeComment = Text.IsEmpty() ? TEXT("Comment") : Text;
				Node->NodeWidth = Width;
				Node->NodeHeight = Height;
			});
		}
		if (Type == TEXT("self"))
		{
			return SpawnTyped<UK2Node_Self>(Graph, Position, [](UK2Node_Self*) {});
		}
		if (Type == TEXT("make_array") || Type == TEXT("makearray"))
		{
			return SpawnTyped<UK2Node_MakeArray>(Graph, Position, [](UK2Node_MakeArray*) {});
		}
		if (Type == TEXT("make_struct") || Type == TEXT("makestruct") || Type == TEXT("break_struct") || Type == TEXT("breakstruct"))
		{
			UScriptStruct* Struct = ResolveStruct(SpecString(Spec, TEXT("struct"), TEXT("name")), OutError);
			if (!Struct) return nullptr;
			if (Type.StartsWith(TEXT("make")))
			{
				return SpawnTyped<UK2Node_MakeStruct>(Graph, Position, [Struct](UK2Node_MakeStruct* Node) { Node->StructType = Struct; });
			}
			return SpawnTyped<UK2Node_BreakStruct>(Graph, Position, [Struct](UK2Node_BreakStruct* Node) { Node->StructType = Struct; });
		}
		if (Type == TEXT("select"))
		{
			return SpawnTyped<UK2Node_Select>(Graph, Position, [](UK2Node_Select*) {});
		}
		if (Type == TEXT("switch_enum") || Type == TEXT("switch_on_enum"))
		{
			UEnum* Enum = ResolveEnum(SpecString(Spec, TEXT("enum"), TEXT("name")), OutError);
			if (!Enum) return nullptr;
			return SpawnTyped<UK2Node_SwitchEnum>(Graph, Position, [Enum](UK2Node_SwitchEnum* Node) { Node->SetEnum(Enum); });
		}
		if (Type == TEXT("switch_int") || Type == TEXT("switch_integer") || Type == TEXT("switch_on_int"))
		{
			return SpawnTyped<UK2Node_SwitchInteger>(Graph, Position, [](UK2Node_SwitchInteger*) {});
		}
		if (Type == TEXT("switch_string") || Type == TEXT("switch_on_string"))
		{
			return SpawnTyped<UK2Node_SwitchString>(Graph, Position, [](UK2Node_SwitchString*) {});
		}
		if (Type == TEXT("switch_name") || Type == TEXT("switch_on_name"))
		{
			return SpawnTyped<UK2Node_SwitchName>(Graph, Position, [](UK2Node_SwitchName*) {});
		}
		if (Type == TEXT("timeline"))
		{
			if (!FBlueprintEditorUtils::DoesSupportTimelines(BP) || !RequireUbergraph(BP, Graph, TEXT("A timeline"), OutError))
			{
				if (OutError.IsEmpty()) OutError = TEXT("This Blueprint does not support timelines.");
				return nullptr;
			}
			UK2Node_Timeline* Node = SpawnTyped<UK2Node_Timeline>(Graph, Position, [](UK2Node_Timeline*) {});
			const FString Name = GetString(Spec, TEXT("name"));
			if (!Name.IsEmpty() && Name != Node->TimelineName.ToString())
			{
				if (!FBlueprintEditorUtils::RenameTimeline(BP, Node->TimelineName, FName(*Name)))
				{
					OutWarnings.Add(FString::Printf(TEXT("Could not rename the timeline to \"%s\"; it is called %s."), *Name, *Node->TimelineName.ToString()));
				}
			}
			return Node;
		}
		if (Type == TEXT("get_class_defaults") || Type == TEXT("class_defaults"))
		{
			UK2Node_GetClassDefaults* Node = SpawnTyped<UK2Node_GetClassDefaults>(Graph, Position, [](UK2Node_GetClassDefaults*) {});
			const FString ClassName = GetString(Spec, TEXT("class"));
			if (!ClassName.IsEmpty())
			{
				UClass* Class = ResolveClass(ClassName, OutError);
				if (!Class) { OutWarnings.Add(OutError); OutError.Reset(); }
				else if (UEdGraphPin* ClassPin = Node->FindClassPin())
				{
					Schema->TrySetDefaultObject(*ClassPin, Class);
				}
			}
			return Node;
		}
		if (Type == TEXT("format_text"))
		{
			return SpawnTyped<UK2Node_FormatText>(Graph, Position, [](UK2Node_FormatText*) {});
		}
		if (Type == TEXT("input_key"))
		{
			const FString KeyName = SpecString(Spec, TEXT("key"), TEXT("name"));
			const FKey Key(*KeyName);
			if (!Key.IsValid())
			{
				OutError = FString::Printf(TEXT("Unknown input key \"%s\" (examples: SpaceBar, LeftMouseButton, E, Gamepad_FaceButton_Bottom)."), *KeyName);
				return nullptr;
			}
			if (!RequireUbergraph(BP, Graph, TEXT("An input key event"), OutError)) return nullptr;
			return SpawnTyped<UK2Node_InputKey>(Graph, Position, [Key](UK2Node_InputKey* Node) { Node->InputKey = Key; });
		}
		if (Type == TEXT("spawner") || Type == TEXT("action"))
		{
			const FString Signature = GetString(Spec, TEXT("signature"));
			if (Signature.IsEmpty())
			{
				OutError = TEXT("spawner needs the \"signature\" string returned by bp_search_nodes.");
				return nullptr;
			}
			UObject* Owner = nullptr;
			UBlueprintNodeSpawner* Spawner = FindSpawnerBySignature(Signature, &Owner);
			if (!Spawner)
			{
				OutError = FString::Printf(TEXT("No node action matches signature \"%s\". Run bp_search_nodes again; signatures change when the editor reloads."), *Signature);
				return nullptr;
			}
			UEdGraphNode* Node = Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), Position);
			if (!Node)
			{
				OutError = TEXT("The node spawner returned no node.");
			}
			return Node;
		}
		if (Type == TEXT("node_class") || Type == TEXT("raw"))
		{
			FString ClassName = SpecString(Spec, TEXT("node_class"), TEXT("class"), TEXT("name"));
			UClass* NodeClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
			if (!NodeClass && !ClassName.StartsWith(TEXT("K2Node_"))) NodeClass = FindFirstObject<UClass>(*(TEXT("K2Node_") + ClassName), EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
			if (!NodeClass && ClassName.StartsWith(TEXT("U"))) NodeClass = FindFirstObject<UClass>(*ClassName.Mid(1), EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
			if (!NodeClass)
			{
				OutError = FString::Printf(TEXT("Node class \"%s\" not found (expected something like K2Node_IfThenElse)."), *ClassName);
				return nullptr;
			}
			return SpawnByClass(Graph, NodeClass, Position, GetObject(Spec, TEXT("properties")), OutError, OutWarnings);
		}

		OutError = FString::Printf(TEXT("Unknown node type \"%s\". Supported: %s"), *Type, *FString::Join(GetSupportedNodeTypes(), TEXT(", ")));
		return nullptr;
	}
}
