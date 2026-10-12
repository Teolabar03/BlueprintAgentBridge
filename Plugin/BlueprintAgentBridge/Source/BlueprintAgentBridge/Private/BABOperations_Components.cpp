// Copyright (c) 2026 Matteo La Barbera. MIT License.
//
// Components (SimpleConstructionScript), class defaults, interfaces, event dispatchers and
// timeline tracks: everything an agent needs to build a real actor, not only its graphs.

#include "BABOpsCommon.h"
#include "BABNodeFactory.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/TimelineTemplate.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Timeline.h"
#include "K2Node_Variable.h"
#include "UObject/Interface.h"
#include "UObject/UnrealType.h"

namespace BAB
{
	// ---------------------------------------------------------------------------------------
	// Shared helpers

	/** Editable properties of Object whose value differs from Reference (or every editable property with bAll). */
	static TSharedRef<FJsonObject> CollectProperties(UObject* Object, UObject* Reference, bool bAll, const FString& Filter)
	{
		TSharedRef<FJsonObject> Out = NewObject();
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient)) continue;
			if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;
			const FString Name = Property->GetName();
			if (!Filter.IsEmpty() && !Name.Contains(Filter) && !Property->GetDisplayNameText().ToString().Contains(Filter)) continue;
			if (!bAll && Filter.IsEmpty() && Reference)
			{
				// Only properties that Reference also has can be compared; the rest are always listed.
				const bool bShared = Reference->GetClass()->IsChildOf(Property->GetOwnerClass());
				if (bShared && Property->Identical_InContainer(Object, Reference, 0, PPF_None)) continue;
			}
			FString Value;
			if (FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(Object), Value, Object))
			{
				Out->SetStringField(Name, Value);
			}
		}
		return Out;
	}

	static FBABResult ApplyProperties(UObject* Object, const FJsonObject& Params, UBlueprint* BP, bool bStructural)
	{
		FBABResult Result;
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Pending;
		if (HasField(Params, TEXT("property")))
		{
			const TSharedPtr<FJsonValue>* Value = Params.Values.Find(TEXT("value"));
			Pending.Add(TPair<FString, TSharedPtr<FJsonValue>>(GetString(Params, TEXT("property")), Value ? *Value : nullptr));
		}
		if (TSharedPtr<FJsonObject> Properties = GetObject(Params, TEXT("properties")))
		{
			for (const auto& Pair : Properties->Values) Pending.Add(TPair<FString, TSharedPtr<FJsonValue>>(Pair.Key, Pair.Value));
		}
		if (Pending.Num() == 0)
		{
			return FBABResult::Fail(TEXT("Pass \"property\" + \"value\", or a \"properties\" object."));
		}
		TArray<FString> Applied;
		for (const auto& Pair : Pending)
		{
			FString Error;
			FProperty* Property = nullptr;
			if (!ApplyObjectProperty(Object, Pair.Key, Pair.Value, Error, &Property))
			{
				Result.Error(Error);
				continue;
			}
			FPropertyChangedEvent Event(Property);
			Object->PostEditChangeProperty(Event);
			Applied.Add(Pair.Key);
		}
		SetStringArray(*Result.Data, TEXT("applied"), Applied);
		if (Applied.Num() > 0)
		{
			if (bStructural) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			else FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		}
		return Result;
	}

	static USCS_Node* FindComponentNode(UBlueprint* BP, const FString& Name, FString& OutError)
	{
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!SCS)
		{
			OutError = FString::Printf(TEXT("%s has no component list (only Actor-based Blueprints have components)."), *GetObjectPath(BP));
			return nullptr;
		}
		if (USCS_Node* Exact = SCS->FindSCSNode(FName(*Name)))
		{
			return Exact;
		}
		TArray<FString> Names;
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node) continue;
			if (Node->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase)) return Node;
			Names.Add(Node->GetVariableName().ToString());
		}
		OutError = FString::Printf(TEXT("Component \"%s\" not found. Components: %s"), *Name, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	static TSharedRef<FJsonObject> ComponentToJson(UBlueprint* BP, USCS_Node* Node, bool bAllProperties, const FString& Filter)
	{
		TSharedRef<FJsonObject> Json = NewObject();
		Json->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
		Json->SetStringField(TEXT("class"), ClassDisplayName(Node->ComponentClass));
		USCS_Node* Parent = nullptr;
		for (USCS_Node* Other : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Other && Other->GetChildNodes().Contains(Node)) { Parent = Other; break; }
		}
		if (Parent) Json->SetStringField(TEXT("parent"), Parent->GetVariableName().ToString());
		else if (!Node->ParentComponentOrVariableName.IsNone()) Json->SetStringField(TEXT("parent"), Node->ParentComponentOrVariableName.ToString());
		TArray<FString> Children;
		for (USCS_Node* Child : Node->GetChildNodes()) if (Child) Children.Add(Child->GetVariableName().ToString());
		SetStringArray(*Json, TEXT("children"), Children);
		if (UActorComponent* Template = Node->ComponentTemplate)
		{
			Json->SetObjectField(TEXT("properties"), CollectProperties(Template, Template->GetClass()->GetDefaultObject(), bAllProperties, Filter));
		}
		return Json;
	}

	// ---------------------------------------------------------------------------------------
	// Timelines

	static const TCHAR* InterpName(ERichCurveInterpMode Mode)
	{
		switch (Mode)
		{
		case RCIM_Constant: return TEXT("constant");
		case RCIM_Cubic: return TEXT("cubic");
		default: return TEXT("linear");
		}
	}

	static TSharedPtr<FJsonValue> CurveKeysToJson(const FRichCurve& Curve)
	{
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (auto It = Curve.GetKeyIterator(); It; ++It)
		{
			TArray<TSharedPtr<FJsonValue>> Pair;
			Pair.Add(MakeShared<FJsonValueNumber>(It->Time));
			Pair.Add(MakeShared<FJsonValueNumber>(It->Value));
			Keys.Add(MakeShared<FJsonValueArray>(Pair));
		}
		return MakeShared<FJsonValueArray>(Keys);
	}

	static TSharedRef<FJsonObject> TimelineToJson(UTimelineTemplate* Template)
	{
		TSharedRef<FJsonObject> Json = NewObject();
		Json->SetStringField(TEXT("name"), Template->GetVariableName().ToString());
		Json->SetNumberField(TEXT("length"), Template->TimelineLength);
		Json->SetStringField(TEXT("length_mode"), Template->LengthMode == TL_LastKeyFrame ? TEXT("last_keyframe") : TEXT("timeline_length"));
		Json->SetBoolField(TEXT("auto_play"), Template->bAutoPlay);
		Json->SetBoolField(TEXT("loop"), Template->bLoop);
		Json->SetBoolField(TEXT("replicated"), Template->bReplicated);
		Json->SetBoolField(TEXT("ignore_time_dilation"), Template->bIgnoreTimeDilation);
		TArray<TSharedPtr<FJsonValue>> Tracks;
		auto AddTrack = [&](const FName& Name, const TCHAR* Type, const FRichCurve* Curves, int32 CurveCount)
		{
			TSharedRef<FJsonObject> Track = NewObject();
			Track->SetStringField(TEXT("name"), Name.ToString());
			Track->SetStringField(TEXT("type"), Type);
			if (CurveCount == 1)
			{
				Track->SetField(TEXT("keys"), CurveKeysToJson(Curves[0]));
				if (Curves[0].GetNumKeys() > 0) Track->SetStringField(TEXT("interp"), InterpName(Curves[0].GetFirstKey().InterpMode));
			}
			else
			{
				// Merge component curves on shared key times: [time, [c0, c1, ...]]
				TSet<float> Times;
				for (int32 Index = 0; Index < CurveCount; ++Index)
				{
					for (auto It = Curves[Index].GetKeyIterator(); It; ++It) Times.Add(It->Time);
				}
				TArray<float> Sorted = Times.Array();
				Sorted.Sort();
				TArray<TSharedPtr<FJsonValue>> Keys;
				for (float Time : Sorted)
				{
					TArray<TSharedPtr<FJsonValue>> Components;
					for (int32 Index = 0; Index < CurveCount; ++Index) Components.Add(MakeShared<FJsonValueNumber>(Curves[Index].Eval(Time)));
					TArray<TSharedPtr<FJsonValue>> Pair;
					Pair.Add(MakeShared<FJsonValueNumber>(Time));
					Pair.Add(MakeShared<FJsonValueArray>(Components));
					Keys.Add(MakeShared<FJsonValueArray>(Pair));
				}
				Track->SetArrayField(TEXT("keys"), Keys);
			}
			Tracks.Add(ObjectValue(Track));
		};
		for (const FTTFloatTrack& Track : Template->FloatTracks) if (Track.CurveFloat) AddTrack(Track.GetTrackName(), TEXT("float"), &Track.CurveFloat->FloatCurve, 1);
		for (const FTTVectorTrack& Track : Template->VectorTracks) if (Track.CurveVector) AddTrack(Track.GetTrackName(), TEXT("vector"), Track.CurveVector->FloatCurves, 3);
		for (const FTTLinearColorTrack& Track : Template->LinearColorTracks) if (Track.CurveLinearColor) AddTrack(Track.GetTrackName(), TEXT("linear_color"), Track.CurveLinearColor->FloatCurves, 4);
		for (const FTTEventTrack& Track : Template->EventTracks) if (Track.CurveKeys) AddTrack(Track.GetTrackName(), TEXT("event"), &Track.CurveKeys->FloatCurve, 1);
		Json->SetArrayField(TEXT("tracks"), Tracks);
		return Json;
	}

	static UTimelineTemplate* FindTimeline(UBlueprint* BP, const FString& Name, FString& OutError)
	{
		if (UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(FName(*Name)))
		{
			return Template;
		}
		TArray<FString> Names;
		for (UTimelineTemplate* Template : BP->Timelines)
		{
			if (!Template) continue;
			if (Template->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase)) return Template;
			Names.Add(Template->GetVariableName().ToString());
		}
		OutError = FString::Printf(TEXT("Timeline \"%s\" not found. Timelines: %s. Add one with bp_add_node {type:\"timeline\", name:\"...\"}."), *Name, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	static ERichCurveInterpMode ParseInterp(const FString& Text)
	{
		const FString Lower = Text.ToLower();
		if (Lower == TEXT("cubic") || Lower == TEXT("auto") || Lower == TEXT("smooth")) return RCIM_Cubic;
		if (Lower == TEXT("constant") || Lower == TEXT("step")) return RCIM_Constant;
		return RCIM_Linear;
	}

	static void FillCurve(FRichCurve& Curve, const TArray<TPair<float, float>>& Keys, ERichCurveInterpMode Interp)
	{
		Curve.Reset();
		for (const TPair<float, float>& Key : Keys)
		{
			const FKeyHandle Handle = Curve.AddKey(Key.Key, Key.Value);
			Curve.SetKeyInterpMode(Handle, Interp);
			if (Interp == RCIM_Cubic) Curve.SetKeyTangentMode(Handle, RCTM_Auto);
		}
		Curve.AutoSetTangents();
	}

	/** Parses "keys": [[time, value], [time, [c0, c1, c2]], ...] into per-component key lists. */
	static bool ParseTrackKeys(const FJsonObject& Track, int32 ComponentCount, TArray<TArray<TPair<float, float>>>& OutPerComponent, FString& OutError)
	{
		OutPerComponent.SetNum(ComponentCount);
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!Track.TryGetArrayField(TEXT("keys"), Keys) || !Keys)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
			float Time = 0.0f;
			TArray<float> Components;
			if (KeyValue.IsValid() && KeyValue->Type == EJson::Number)
			{
				Time = (float)KeyValue->AsNumber();
			}
			else if (KeyValue.IsValid() && KeyValue->TryGetArray(Pair) && Pair && Pair->Num() >= 1)
			{
				Time = (float)(*Pair)[0]->AsNumber();
				if (Pair->Num() >= 2)
				{
					const TSharedPtr<FJsonValue>& ValuePart = (*Pair)[1];
					if (ValuePart->Type == EJson::Number) Components.Add((float)ValuePart->AsNumber());
					else if (ValuePart->Type == EJson::Array) for (const TSharedPtr<FJsonValue>& C : ValuePart->AsArray()) Components.Add((float)C->AsNumber());
				}
			}
			else
			{
				OutError = TEXT("Each key must be [time, value], [time, [c0, c1, ...]] or a bare time for event tracks.");
				return false;
			}
			for (int32 Index = 0; Index < ComponentCount; ++Index)
			{
				const float Component = Components.IsValidIndex(Index) ? Components[Index] : (Components.Num() == 1 ? Components[0] : 0.0f);
				OutPerComponent[Index].Add(TPair<float, float>(Time, Component));
			}
		}
		return true;
	}
}

void BABRegisterOperations_Components()
{
	using namespace BAB;
	FBABDispatcher& D = FBABDispatcher::Get();

	// ---------------------------------------------------------------------------------------
	// Components

	D.Register(TEXT("bp_add_component"), TEXT("Adds a component to an Actor Blueprint (class, name, parent, properties). Scene components attach to the root unless parent is given."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		UBlueprint* BP = Ctx.Blueprint;
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!SCS)
		{
			return FBABResult::Fail(FString::Printf(TEXT("%s has no component list (only Actor-based Blueprints have components)."), *GetObjectPath(BP)));
		}
		FString Error;
		UClass* ComponentClass = ResolveClass(GetString(Params, TEXT("class")), Error);
		if (!ComponentClass) return FBABResult::Fail(Error);
		if (!ComponentClass->IsChildOf(UActorComponent::StaticClass()) || ComponentClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return FBABResult::Fail(FString::Printf(TEXT("%s is not a concrete component class."), *ClassDisplayName(ComponentClass)));
		}
		if (!FKismetEditorUtilities::IsClassABlueprintSpawnableComponent(ComponentClass))
		{
			return FBABResult::Fail(FString::Printf(TEXT("%s cannot be added from Blueprints (missing BlueprintSpawnableComponent)."), *ClassDisplayName(ComponentClass)));
		}

		FString Name = GetString(Params, TEXT("name")).TrimStartAndEnd();
		Name.ReplaceInline(TEXT(" "), TEXT(""));
		FName VariableName = Name.IsEmpty() ? SCS->GenerateNewComponentName(ComponentClass) : FName(*Name);
		TSet<FName> ExistingVariables;
		FBlueprintEditorUtils::GetClassVariableList(BP, ExistingVariables, true);
		if (!Name.IsEmpty() && (SCS->FindSCSNode(VariableName) || ExistingVariables.Contains(VariableName)))
		{
			if (GetBool(Params, TEXT("unique"), false))
			{
				VariableName = SCS->GenerateNewComponentName(ComponentClass, VariableName);
			}
			else
			{
				return FBABResult::Fail(FString::Printf(TEXT("A component or variable named \"%s\" already exists. Choose another name or pass unique=true."), *Name));
			}
		}

		USCS_Node* ParentNode = nullptr;
		const FString ParentName = GetString(Params, TEXT("parent"));
		if (!ParentName.IsEmpty())
		{
			ParentNode = FindComponentNode(BP, ParentName, Error);
			if (!ParentNode) return FBABResult::Fail(Error);
		}

		BP->Modify();
		SCS->Modify();
		USCS_Node* NewNode = SCS->CreateNode(ComponentClass, VariableName);
		if (!NewNode)
		{
			return FBABResult::Fail(TEXT("CreateNode failed."));
		}
		const bool bScene = ComponentClass->IsChildOf(USceneComponent::StaticClass());
		if (ParentNode)
		{
			ParentNode->AddChildNode(NewNode);
		}
		else if (bScene)
		{
			USCS_Node* RootNode = nullptr;
			SCS->GetSceneRootComponentTemplate(true, &RootNode);
			if (RootNode && RootNode != NewNode)
			{
				RootNode->AddChildNode(NewNode);
			}
			else
			{
				SCS->AddNode(NewNode);
			}
		}
		else
		{
			SCS->AddNode(NewNode);
		}
		SCS->ValidateSceneRootNodes();

		FBABResult Result;
		if (TSharedPtr<FJsonObject> Properties = GetObject(Params, TEXT("properties")))
		{
			FJsonObject PropertyParams;
			PropertyParams.SetObjectField(TEXT("properties"), Properties);
			FBABResult Applied = ApplyProperties(NewNode->ComponentTemplate, PropertyParams, BP, false);
			for (const FString& E : Applied.Errors) Result.Warn(E);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		Result.Data = ComponentToJson(BP, NewNode, false, FString());
		WarnIfNotWritable(BP, Result);
		return Result;
	});

	D.Register(TEXT("bp_get_component"), TEXT("Reads a component: class, parent, children and its properties that differ from the class defaults (all=true for every editable property, filter for a substring)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		USCS_Node* Node = FindComponentNode(Ctx.Blueprint, GetString(Params, TEXT("component"), GetString(Params, TEXT("name"))), Error);
		if (!Node) return FBABResult::Fail(Error);
		FBABResult Result;
		Result.Data = ComponentToJson(Ctx.Blueprint, Node, GetBool(Params, TEXT("all"), false), GetString(Params, TEXT("filter")));
		return Result;
	});

	D.Register(TEXT("bp_set_component_property"), TEXT("Sets properties on a component template (property + value, or a properties object): RelativeLocation, StaticMesh, BoxExtent, bGenerateOverlapEvents..."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		USCS_Node* Node = FindComponentNode(Ctx.Blueprint, GetString(Params, TEXT("component"), GetString(Params, TEXT("name"))), Error);
		if (!Node) return FBABResult::Fail(Error);
		if (!Node->ComponentTemplate) return FBABResult::Fail(TEXT("The component has no template object."));
		Node->Modify();
		FBABResult Result = ApplyProperties(Node->ComponentTemplate, Params, Ctx.Blueprint, false);
		Result.Data->SetObjectField(TEXT("component"), ComponentToJson(Ctx.Blueprint, Node, false, FString()));
		return Result;
	});

	D.Register(TEXT("bp_remove_component"), TEXT("Removes a component (children are re-parented). Fails if graph nodes use it unless force=true."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		UBlueprint* BP = Ctx.Blueprint;
		FString Error;
		USCS_Node* Node = FindComponentNode(BP, GetString(Params, TEXT("component"), GetString(Params, TEXT("name"))), Error);
		if (!Node) return FBABResult::Fail(Error);
		const FName VariableName = Node->GetVariableName();
		TArray<TSharedPtr<FJsonValue>> Usages;
		TArray<UEdGraph*> Graphs;
		GetAllGraphs(BP, Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(GraphNode);
				const bool bUses = (VarNode && VarNode->GetVarName() == VariableName) || (GraphNode && GraphNode->GetClass()->GetName() == TEXT("K2Node_ComponentBoundEvent") && NodeTitle(GraphNode).Contains(VariableName.ToString()));
				if (bUses)
				{
					TSharedRef<FJsonObject> Usage = NewObject();
					Usage->SetStringField(TEXT("node"), NodeId(GraphNode));
					Usage->SetStringField(TEXT("title"), NodeTitle(GraphNode));
					Usage->SetStringField(TEXT("graph"), Graph->GetName());
					Usages.Add(ObjectValue(Usage));
				}
			}
		}
		FBABResult Result;
		Result.Data->SetArrayField(TEXT("usages"), Usages);
		if (Usages.Num() > 0 && !GetBool(Params, TEXT("force"), false))
		{
			Result.Error(FString::Printf(TEXT("Component \"%s\" is used by %d node(s) (see usages). Pass force=true to remove the component and those nodes."), *VariableName.ToString(), Usages.Num()));
			return Result;
		}
		if (Usages.Num() > 0)
		{
			FBlueprintEditorUtils::RemoveVariableNodes(BP, VariableName);
		}
		BP->Modify();
		BP->SimpleConstructionScript->Modify();
		BP->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		Result.Data->SetStringField(TEXT("removed"), VariableName.ToString());
		return Result;
	});

	// ---------------------------------------------------------------------------------------
	// Class defaults

	D.Register(TEXT("bp_get_class_defaults"), TEXT("Class default values: own variables plus inherited properties that differ from the parent class (all=true lists every editable property; filter narrows by name)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		UBlueprint* BP = Ctx.Blueprint;
		if (!BP->GeneratedClass) return FBABResult::Fail(TEXT("The Blueprint has no generated class yet; compile it first."));
		UObject* DefaultObject = BP->GeneratedClass->GetDefaultObject();
		UObject* ParentDefault = BP->ParentClass ? BP->ParentClass->GetDefaultObject() : nullptr;
		FBABResult Result;
		Result.Data->SetStringField(TEXT("class"), ClassDisplayName(BP->GeneratedClass));
		Result.Data->SetObjectField(TEXT("defaults"), CollectProperties(DefaultObject, ParentDefault, GetBool(Params, TEXT("all"), false), GetString(Params, TEXT("filter"))));
		return Result;
	});

	D.Register(TEXT("bp_set_class_default"), TEXT("Sets class default values on the Blueprint (property + value, or a properties object), e.g. bReplicates, InitialLifeSpan, inherited variables."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		UBlueprint* BP = Ctx.Blueprint;
		if (!BP->GeneratedClass) return FBABResult::Fail(TEXT("The Blueprint has no generated class yet; compile it first."));
		UObject* DefaultObject = BP->GeneratedClass->GetDefaultObject();
		FBABResult Result = ApplyProperties(DefaultObject, Params, BP, false);
		Result.Data->SetObjectField(TEXT("defaults"), CollectProperties(DefaultObject, BP->ParentClass ? BP->ParentClass->GetDefaultObject() : nullptr, false, FString()));
		return Result;
	});

	// ---------------------------------------------------------------------------------------
	// Interfaces

	D.Register(TEXT("bp_add_interface"), TEXT("Implements a Blueprint or native interface. Functions with outputs become graphs, the others become events you can add with bp_add_node."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UClass* InterfaceClass = ResolveClass(GetString(Params, TEXT("interface")), Error);
		if (!InterfaceClass) return FBABResult::Fail(Error);
		if (!InterfaceClass->IsChildOf(UInterface::StaticClass()) || !FKismetEditorUtilities::IsClassABlueprintInterface(InterfaceClass))
		{
			return FBABResult::Fail(FString::Printf(TEXT("%s is not a Blueprint-implementable interface."), *ClassDisplayName(InterfaceClass)));
		}
		for (const FBPInterfaceDescription& Existing : Ctx.Blueprint->ImplementedInterfaces)
		{
			if (Existing.Interface == InterfaceClass) return FBABResult::Fail(FString::Printf(TEXT("%s is already implemented."), *ClassDisplayName(InterfaceClass)));
		}
		if (!FBlueprintEditorUtils::ImplementNewInterface(Ctx.Blueprint, InterfaceClass->GetClassPathName()))
		{
			return FBABResult::Fail(FString::Printf(TEXT("The editor refused to implement %s (name clash with an existing function?)."), *ClassDisplayName(InterfaceClass)));
		}
		FBABResult Result;
		Result.Data->SetStringField(TEXT("interface"), ClassDisplayName(InterfaceClass));
		TArray<TSharedPtr<FJsonValue>> Functions;
		for (TFieldIterator<UFunction> It(InterfaceClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			bool bHasOutputs = false;
			for (TFieldIterator<FProperty> ParamIt(*It); ParamIt && (ParamIt->PropertyFlags & CPF_Parm); ++ParamIt)
			{
				if (ParamIt->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm) && !ParamIt->HasAnyPropertyFlags(CPF_ReferenceParm)) bHasOutputs = true;
				if (ParamIt->HasAnyPropertyFlags(CPF_ReturnParm)) bHasOutputs = true;
			}
			TSharedRef<FJsonObject> F = NewObject();
			F->SetStringField(TEXT("name"), It->GetName());
			F->SetStringField(TEXT("implemented_as"), bHasOutputs ? TEXT("function graph") : TEXT("event (add with bp_add_node type=event)"));
			Functions.Add(ObjectValue(F));
		}
		Result.Data->SetArrayField(TEXT("functions"), Functions);
		return Result;
	});

	D.Register(TEXT("bp_remove_interface"), TEXT("Removes an implemented interface (preserve_functions=true keeps the graphs as normal functions)."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UClass* InterfaceClass = ResolveClass(GetString(Params, TEXT("interface")), Error);
		if (!InterfaceClass) return FBABResult::Fail(Error);
		bool bFound = false;
		for (const FBPInterfaceDescription& Existing : Ctx.Blueprint->ImplementedInterfaces) bFound |= (Existing.Interface == InterfaceClass);
		if (!bFound) return FBABResult::Fail(FString::Printf(TEXT("%s is not implemented by this Blueprint."), *ClassDisplayName(InterfaceClass)));
		FBlueprintEditorUtils::RemoveInterface(Ctx.Blueprint, InterfaceClass->GetClassPathName(), GetBool(Params, TEXT("preserve_functions"), false));
		FBABResult Result;
		Result.Data->SetStringField(TEXT("removed"), ClassDisplayName(InterfaceClass));
		return Result;
	});

	// ---------------------------------------------------------------------------------------
	// Event dispatchers

	D.Register(TEXT("bp_add_event_dispatcher"), TEXT("Adds an event dispatcher (multicast delegate) with optional params. Use node types call_dispatcher / bind_dispatcher / unbind_dispatcher."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		UBlueprint* BP = Ctx.Blueprint;
		FString Name = GetString(Params, TEXT("name")).TrimStartAndEnd();
		Name.ReplaceInline(TEXT(" "), TEXT(""));
		if (Name.IsEmpty()) return FBABResult::Fail(TEXT("Missing dispatcher \"name\" (e.g. OnOpened)."));
		TSet<FName> ExistingVariables;
		FBlueprintEditorUtils::GetClassVariableList(BP, ExistingVariables, true);
		if (ExistingVariables.Contains(FName(*Name)) || (GetBlueprintSelfClass(BP) && GetBlueprintSelfClass(BP)->FindFunctionByName(FName(*Name))))
		{
			return FBABResult::Fail(FString::Printf(TEXT("A variable, function or dispatcher named \"%s\" already exists."), *Name));
		}

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		FEdGraphPinType DelegateType;
		DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, FName(*Name), DelegateType))
		{
			return FBABResult::Fail(TEXT("Adding the dispatcher variable failed."));
		}
		UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!SignatureGraph)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*Name));
			return FBABResult::Fail(TEXT("Creating the dispatcher signature graph failed."));
		}
		SignatureGraph->bEditable = false;
		Schema->CreateDefaultNodesForGraph(*SignatureGraph);
		Schema->CreateFunctionGraphTerminators(*SignatureGraph, (UClass*)nullptr);
		Schema->AddExtraFunctionFlags(SignatureGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
		Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);
		BP->DelegateSignatureGraphs.Add(SignatureGraph);

		FBABResult Result;
		const TArray<TSharedPtr<FJsonValue>>* ParamList = nullptr;
		if (Params.TryGetArrayField(TEXT("params"), ParamList) && ParamList)
		{
			UK2Node_FunctionEntry* Entry = nullptr;
			for (UEdGraphNode* Node : SignatureGraph->Nodes)
			{
				if ((Entry = Cast<UK2Node_FunctionEntry>(Node)) != nullptr) break;
			}
			for (const TSharedPtr<FJsonValue>& ParamValue : *ParamList)
			{
				const TSharedPtr<FJsonObject>* ParamObject = nullptr;
				if (!Entry || !ParamValue.IsValid() || !ParamValue->TryGetObject(ParamObject) || !ParamObject) continue;
				const FString ParamName = GetString(**ParamObject, TEXT("name"));
				FEdGraphPinType PinType;
				FString TypeError;
				if (ParamName.IsEmpty() || !ParsePinType(GetString(**ParamObject, TEXT("type")), PinType, TypeError))
				{
					Result.Warn(FString::Printf(TEXT("Skipped param \"%s\": %s"), *ParamName, *TypeError));
					continue;
				}
				Entry->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output, true);
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		Result.Data->SetStringField(TEXT("dispatcher"), Name);
		Result.Data->SetStringField(TEXT("signature_graph"), SignatureGraph->GetName());
		Result.Data->SetStringField(TEXT("hint"), TEXT("Add nodes with {type:\"call_dispatcher\", dispatcher:\"...\"}, {type:\"bind_dispatcher\"...} (connect a custom event OutputDelegate pin to its Event pin) or {type:\"unbind_dispatcher\"...}."));
		return Result;
	});

	// ---------------------------------------------------------------------------------------
	// Timelines

	D.Register(TEXT("bp_get_timeline"), TEXT("Reads a timeline: length, flags and tracks with their keys."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FString Error;
		UTimelineTemplate* Template = FindTimeline(Ctx.Blueprint, GetString(Params, TEXT("timeline"), GetString(Params, TEXT("name"))), Error);
		if (!Template) return FBABResult::Fail(Error);
		FBABResult Result;
		Result.Data = TimelineToJson(Template);
		return Result;
	});

	D.Register(TEXT("bp_update_timeline"), TEXT("Edits a timeline: length, auto_play, loop, replicated, tracks [{name, type: float|vector|linear_color|event, keys: [[time, value], ...], interp: linear|cubic|constant}], remove_tracks [names]."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		UBlueprint* BP = Ctx.Blueprint;
		FString Error;
		UTimelineTemplate* Template = FindTimeline(BP, GetString(Params, TEXT("timeline"), GetString(Params, TEXT("name"))), Error);
		if (!Template) return FBABResult::Fail(Error);
		UClass* OwnerClass = BP->GeneratedClass;
		if (!OwnerClass) return FBABResult::Fail(TEXT("The Blueprint has no generated class yet; compile it first."));

		FBABResult Result;
		Template->Modify();
		if (HasField(Params, TEXT("length"))) Template->TimelineLength = (float)GetNumber(Params, TEXT("length"), Template->TimelineLength);
		if (HasField(Params, TEXT("length_mode"))) Template->LengthMode = GetString(Params, TEXT("length_mode")).ToLower() == TEXT("last_keyframe") ? TL_LastKeyFrame : TL_TimelineLength;
		if (HasField(Params, TEXT("auto_play"))) Template->bAutoPlay = GetBool(Params, TEXT("auto_play"), false);
		if (HasField(Params, TEXT("loop"))) Template->bLoop = GetBool(Params, TEXT("loop"), false);
		if (HasField(Params, TEXT("replicated"))) Template->bReplicated = GetBool(Params, TEXT("replicated"), false);
		if (HasField(Params, TEXT("ignore_time_dilation"))) Template->bIgnoreTimeDilation = GetBool(Params, TEXT("ignore_time_dilation"), false);

		TArray<FString> RemoveNames;
		GetStringArray(Params, TEXT("remove_tracks"), RemoveNames);
		for (const FString& RemoveName : RemoveNames)
		{
			// Same order as the timeline editor: drop the display entry, then the track itself.
			const FName TrackName(*RemoveName);
			bool bRemoved = false;
			for (int32 DisplayIndex = 0; DisplayIndex < Template->GetNumDisplayTracks() && !bRemoved; ++DisplayIndex)
			{
				const FTTTrackId TrackId = Template->GetDisplayTrackId(DisplayIndex);
				FName Candidate = NAME_None;
				switch (TrackId.TrackType)
				{
				case FTTTrackBase::TT_Event: if (Template->EventTracks.IsValidIndex(TrackId.TrackIndex)) Candidate = Template->EventTracks[TrackId.TrackIndex].GetTrackName(); break;
				case FTTTrackBase::TT_FloatInterp: if (Template->FloatTracks.IsValidIndex(TrackId.TrackIndex)) Candidate = Template->FloatTracks[TrackId.TrackIndex].GetTrackName(); break;
				case FTTTrackBase::TT_VectorInterp: if (Template->VectorTracks.IsValidIndex(TrackId.TrackIndex)) Candidate = Template->VectorTracks[TrackId.TrackIndex].GetTrackName(); break;
				case FTTTrackBase::TT_LinearColorInterp: if (Template->LinearColorTracks.IsValidIndex(TrackId.TrackIndex)) Candidate = Template->LinearColorTracks[TrackId.TrackIndex].GetTrackName(); break;
				default: break;
				}
				if (Candidate != TrackName) continue;
				Template->RemoveDisplayTrack(DisplayIndex);
				switch (TrackId.TrackType)
				{
				case FTTTrackBase::TT_Event: Template->EventTracks.RemoveAt(TrackId.TrackIndex); break;
				case FTTTrackBase::TT_FloatInterp: Template->FloatTracks.RemoveAt(TrackId.TrackIndex); break;
				case FTTTrackBase::TT_VectorInterp: Template->VectorTracks.RemoveAt(TrackId.TrackIndex); break;
				case FTTTrackBase::TT_LinearColorInterp: Template->LinearColorTracks.RemoveAt(TrackId.TrackIndex); break;
				default: break;
				}
				bRemoved = true;
			}
			if (!bRemoved) Result.Warn(FString::Printf(TEXT("No track named \"%s\"."), *RemoveName));
		}

		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (Params.TryGetArrayField(TEXT("tracks"), Tracks) && Tracks)
		{
			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject) continue;
				const FJsonObject& Track = **TrackObject;
				const FString TrackNameString = GetString(Track, TEXT("name")).TrimStartAndEnd();
				const FString Type = GetString(Track, TEXT("type"), TEXT("float")).ToLower();
				const ERichCurveInterpMode Interp = ParseInterp(GetString(Track, TEXT("interp"), TEXT("linear")));
				if (TrackNameString.IsEmpty())
				{
					Result.Warn(TEXT("Skipped a track without a name."));
					continue;
				}
				const FName TrackName(*TrackNameString);
				const int32 ComponentCount = Type == TEXT("vector") ? 3 : (Type == TEXT("linear_color") || Type == TEXT("color") ? 4 : 1);
				TArray<TArray<TPair<float, float>>> Keys;
				if (!ParseTrackKeys(Track, ComponentCount, Keys, Error))
				{
					Result.Warn(FString::Printf(TEXT("Track \"%s\": %s"), *TrackNameString, *Error));
					continue;
				}

				if (Type == TEXT("float"))
				{
					FTTFloatTrack* Existing = Template->FloatTracks.FindByPredicate([&](const FTTFloatTrack& T) { return T.GetTrackName() == TrackName; });
					if (!Existing)
					{
						if (!Template->IsNewTrackNameValid(TrackName)) { Result.Warn(FString::Printf(TEXT("Track name \"%s\" is already used by a track of another type."), *TrackNameString)); continue; }
						FTTFloatTrack NewTrack;
						NewTrack.SetTrackName(TrackName, Template);
						NewTrack.CurveFloat = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
						Template->FloatTracks.Add(NewTrack);
						Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, Template->FloatTracks.Num() - 1));
						Existing = &Template->FloatTracks.Last();
					}
					if (Existing->CurveFloat && Keys[0].Num() > 0) FillCurve(Existing->CurveFloat->FloatCurve, Keys[0], Interp);
				}
				else if (Type == TEXT("vector"))
				{
					FTTVectorTrack* Existing = Template->VectorTracks.FindByPredicate([&](const FTTVectorTrack& T) { return T.GetTrackName() == TrackName; });
					if (!Existing)
					{
						if (!Template->IsNewTrackNameValid(TrackName)) { Result.Warn(FString::Printf(TEXT("Track name \"%s\" is already used by a track of another type."), *TrackNameString)); continue; }
						FTTVectorTrack NewTrack;
						NewTrack.SetTrackName(TrackName, Template);
						NewTrack.CurveVector = NewObject<UCurveVector>(OwnerClass, NAME_None, RF_Public);
						Template->VectorTracks.Add(NewTrack);
						Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, Template->VectorTracks.Num() - 1));
						Existing = &Template->VectorTracks.Last();
					}
					if (Existing->CurveVector && Keys[0].Num() > 0) for (int32 Index = 0; Index < 3; ++Index) FillCurve(Existing->CurveVector->FloatCurves[Index], Keys[Index], Interp);
				}
				else if (Type == TEXT("linear_color") || Type == TEXT("color"))
				{
					FTTLinearColorTrack* Existing = Template->LinearColorTracks.FindByPredicate([&](const FTTLinearColorTrack& T) { return T.GetTrackName() == TrackName; });
					if (!Existing)
					{
						if (!Template->IsNewTrackNameValid(TrackName)) { Result.Warn(FString::Printf(TEXT("Track name \"%s\" is already used by a track of another type."), *TrackNameString)); continue; }
						FTTLinearColorTrack NewTrack;
						NewTrack.SetTrackName(TrackName, Template);
						NewTrack.CurveLinearColor = NewObject<UCurveLinearColor>(OwnerClass, NAME_None, RF_Public);
						Template->LinearColorTracks.Add(NewTrack);
						Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_LinearColorInterp, Template->LinearColorTracks.Num() - 1));
						Existing = &Template->LinearColorTracks.Last();
					}
					if (Existing->CurveLinearColor && Keys[0].Num() > 0) for (int32 Index = 0; Index < 4; ++Index) FillCurve(Existing->CurveLinearColor->FloatCurves[Index], Keys[Index], Interp);
				}
				else if (Type == TEXT("event"))
				{
					FTTEventTrack* Existing = Template->EventTracks.FindByPredicate([&](const FTTEventTrack& T) { return T.GetTrackName() == TrackName; });
					if (!Existing)
					{
						if (!Template->IsNewTrackNameValid(TrackName)) { Result.Warn(FString::Printf(TEXT("Track name \"%s\" is already used by a track of another type."), *TrackNameString)); continue; }
						FTTEventTrack NewTrack;
						NewTrack.SetTrackName(TrackName, Template);
						NewTrack.CurveKeys = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
						NewTrack.CurveKeys->bIsEventCurve = true;
						Template->EventTracks.Add(NewTrack);
						Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, Template->EventTracks.Num() - 1));
						Existing = &Template->EventTracks.Last();
					}
					if (Existing->CurveKeys && Keys[0].Num() > 0) FillCurve(Existing->CurveKeys->FloatCurve, Keys[0], RCIM_Constant);
				}
				else
				{
					Result.Warn(FString::Printf(TEXT("Unknown track type \"%s\" (float, vector, linear_color, event)."), *Type));
				}
			}
		}

		if (UK2Node_Timeline* TimelineNode = FBlueprintEditorUtils::FindNodeForTimeline(BP, Template))
		{
			TimelineNode->Modify();
			TimelineNode->ReconstructNode();
			Result.Data->SetStringField(TEXT("node"), NodeId(TimelineNode));
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		Result.Data->SetObjectField(TEXT("timeline"), TimelineToJson(Template));
		return Result;
	});
}
