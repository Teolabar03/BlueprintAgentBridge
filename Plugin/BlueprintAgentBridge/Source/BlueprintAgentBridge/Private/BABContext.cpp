// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABContext.h"
#include "BABTypes.h"
#include "BlueprintAgentBridgeModule.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"

namespace BAB
{
	static IAssetRegistry& GetAssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	void EnsureAssetRegistryScanned(const FString& Path)
	{
		IAssetRegistry& AssetRegistry = GetAssetRegistry();
		static TSet<FString> ScannedPaths;
		if (!ScannedPaths.Contains(Path))
		{
			TArray<FString> Paths;
			Paths.Add(Path);
			AssetRegistry.ScanPathsSynchronous(Paths, /*bForceRescan*/ false);
			ScannedPaths.Add(Path);
		}
		if (AssetRegistry.IsLoadingAssets())
		{
			AssetRegistry.WaitForCompletion();
		}
	}

	UBlueprint* FindBlueprint(const FString& InPath, FString& OutError)
	{
		FString Path = InPath.TrimStartAndEnd();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Path.IsEmpty())
		{
			OutError = TEXT("Missing \"blueprint\" parameter (asset path such as /Game/Blueprints/BP_Player).");
			return nullptr;
		}

		// Windows/absolute file path -> long package name
		if (Path.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase) || Path.Contains(TEXT(":/")))
		{
			FString PackageName;
			if (FPackageName::TryConvertFilenameToLongPackageName(Path, PackageName))
			{
				Path = PackageName;
			}
			else
			{
				OutError = FString::Printf(TEXT("Cannot map file %s to a content path. Is it inside the project Content folder?"), *Path);
				return nullptr;
			}
		}

		// Bare asset name -> search the asset registry
		if (!Path.StartsWith(TEXT("/")))
		{
			EnsureAssetRegistryScanned(TEXT("/Game"));
			FARFilter Filter;
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			Filter.PackagePaths.Add(FName(TEXT("/Game")));
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			GetAssetRegistry().GetAssets(Filter, Assets);

			TArray<FString> Matches;
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetName.ToString().Equals(Path, ESearchCase::IgnoreCase))
				{
					Matches.Add(Asset.GetObjectPathString());
				}
			}
			// Also consider Blueprints that only exist in memory (just created, never saved).
			if (Matches.Num() == 0)
			{
				for (TObjectIterator<UBlueprint> It; It; ++It)
				{
					if (It->GetName().Equals(Path, ESearchCase::IgnoreCase) && !It->GetOutermost()->HasAnyFlags(RF_Transient))
					{
						Matches.AddUnique(It->GetPathName());
					}
				}
			}
			if (Matches.Num() == 0)
			{
				OutError = FString::Printf(TEXT("No Blueprint named \"%s\" found under /Game. Use a full path such as /Game/Folder/%s, or call bp_list_assets."), *Path, *Path);
				return nullptr;
			}
			if (Matches.Num() > 1)
			{
				OutError = FString::Printf(TEXT("Blueprint name \"%s\" is ambiguous: %s"), *Path, *FString::Join(Matches, TEXT(", ")));
				return nullptr;
			}
			Path = Matches[0];
		}

		// Normalize to an object path: /Game/Folder/Name.Name
		FString PackageName = Path;
		FString ObjectName;
		int32 DotIndex = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), DotIndex))
		{
			PackageName = Path.Left(DotIndex);
			ObjectName = Path.Mid(DotIndex + 1);
		}
		else
		{
			ObjectName = FPackageName::GetShortName(Path);
		}
		const FString ObjectPath = PackageName + TEXT(".") + ObjectName;

		UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Object)
		{
			if (!FPackageName::IsValidLongPackageName(PackageName))
			{
				OutError = FString::Printf(TEXT("\"%s\" is not a valid content path. Expected something like /Game/Folder/BP_Name."), *PackageName);
				return nullptr;
			}
			if (!FPackageName::DoesPackageExist(PackageName))
			{
				OutError = FString::Printf(TEXT("Blueprint asset not found on disk: %s"), *PackageName);
				return nullptr;
			}
			Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_None, nullptr);
			if (!Object)
			{
				OutError = FString::Printf(TEXT("Failed to load %s (see the log for details)."), *ObjectPath);
				return nullptr;
			}
		}

		UBlueprint* Blueprint = Cast<UBlueprint>(Object);
		if (!Blueprint)
		{
			if (UClass* AsClass = Cast<UClass>(Object))
			{
				Blueprint = UBlueprint::GetBlueprintFromClass(AsClass);
			}
		}
		if (!Blueprint)
		{
			OutError = FString::Printf(TEXT("%s is a %s, not a Blueprint."), *ObjectPath, *Object->GetClass()->GetName());
		}
		return Blueprint;
	}

	FString GetObjectPath(const UBlueprint* BP)
	{
		return BP ? BP->GetPathName() : FString();
	}

	FString GetPackageFilename(const UBlueprint* BP)
	{
		if (!BP)
		{
			return FString();
		}
		const UPackage* Package = BP->GetOutermost();
		FString Filename;
		if (FPackageName::DoesPackageExist(Package->GetName(), &Filename))
		{
			return Filename;
		}
		if (FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
		{
			return Filename;
		}
		return FString();
	}

	bool CheckPackageWritable(const UBlueprint* BP, FString& OutReason)
	{
		const FString Filename = GetPackageFilename(BP);
		if (Filename.IsEmpty())
		{
			OutReason = FString::Printf(TEXT("Cannot resolve a file path for package %s."), *BP->GetOutermost()->GetName());
			return false;
		}
		IFileManager& FileManager = IFileManager::Get();
		if (FileManager.FileExists(*Filename) && FileManager.IsReadOnly(*Filename))
		{
			OutReason = FString::Printf(TEXT("%s is read-only (checked in to source control?). Check it out or clear the read-only flag before saving."), *Filename);
			return false;
		}
		return true;
	}

	void GetAllGraphs(const UBlueprint* BP, TArray<UEdGraph*>& OutGraphs)
	{
		OutGraphs.Reset();
		if (BP)
		{
			BP->GetAllGraphs(OutGraphs);
			for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
			{
				OutGraphs.AddUnique(Graph);
			}
		}
	}

	UEdGraph* FindGraph(UBlueprint* BP, const FString& InGraphName, FString& OutError)
	{
		const FString GraphName = InGraphName.TrimStartAndEnd();
		TArray<UEdGraph*> Graphs;
		GetAllGraphs(BP, Graphs);

		if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
		{
			if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP))
			{
				return EventGraph;
			}
			if (BP->UbergraphPages.Num() > 0)
			{
				return BP->UbergraphPages[0];
			}
			if (GraphName.IsEmpty())
			{
				TArray<FString> Names;
				for (UEdGraph* G : Graphs) Names.Add(G->GetName());
				OutError = FString::Printf(TEXT("This Blueprint has no event graph; specify \"graph\". Available graphs: %s"), *FString::Join(Names, TEXT(", ")));
				return nullptr;
			}
		}

		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && (Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase) || Graph->GraphGuid.ToString(EGuidFormats::Digits).StartsWith(GraphName.Replace(TEXT("-"), TEXT("")), ESearchCase::IgnoreCase)))
			{
				return Graph;
			}
		}

		TArray<FString> Names;
		for (UEdGraph* G : Graphs) if (G) Names.Add(G->GetName());
		OutError = FString::Printf(TEXT("Graph \"%s\" not found in %s. Available graphs: %s"), *GraphName, *GetObjectPath(BP), *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	FString NodeId(const UEdGraphNode* Node)
	{
		return Node ? Node->NodeGuid.ToString(EGuidFormats::Digits).Left(8) : FString();
	}

	UEdGraphNode* FindNode(UBlueprint* BP, UEdGraph* Graph, const FString& InNodeId, FString& OutError)
	{
		FString Id = InNodeId.TrimStartAndEnd();
		Id.ReplaceInline(TEXT("-"), TEXT(""));
		Id.ReplaceInline(TEXT("{"), TEXT(""));
		Id.ReplaceInline(TEXT("}"), TEXT(""));
		Id.ReplaceInline(TEXT("["), TEXT(""));
		Id.ReplaceInline(TEXT("]"), TEXT(""));
		if (Id.IsEmpty())
		{
			OutError = TEXT("Missing node id.");
			return nullptr;
		}

		TArray<UEdGraph*> Graphs;
		if (Graph)
		{
			Graphs.Add(Graph);
		}
		else
		{
			GetAllGraphs(BP, Graphs);
		}

		bool bLooksLikeGuid = true;
		for (TCHAR C : Id)
		{
			if (!FChar::IsHexDigit(C)) { bLooksLikeGuid = false; break; }
		}

		TArray<UEdGraphNode*> Matches;
		if (bLooksLikeGuid)
		{
			for (UEdGraph* G : Graphs)
			{
				if (!G) continue;
				for (UEdGraphNode* Node : G->Nodes)
				{
					if (Node && Node->NodeGuid.ToString(EGuidFormats::Digits).StartsWith(Id, ESearchCase::IgnoreCase))
					{
						Matches.Add(Node);
					}
				}
			}
		}
		if (Matches.Num() == 0)
		{
			// Fall back to a unique title match ("Event BeginPlay").
			for (UEdGraph* G : Graphs)
			{
				if (!G) continue;
				for (UEdGraphNode* Node : G->Nodes)
				{
					if (Node && NodeTitle(Node).Equals(InNodeId.TrimStartAndEnd(), ESearchCase::IgnoreCase))
					{
						Matches.Add(Node);
					}
				}
			}
		}

		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		if (Matches.Num() > 1)
		{
			TArray<FString> Ids;
			for (UEdGraphNode* N : Matches) Ids.Add(FString::Printf(TEXT("%s (%s)"), *NodeId(N), *NodeTitle(N)));
			OutError = FString::Printf(TEXT("Node id \"%s\" is ambiguous: %s"), *InNodeId, *FString::Join(Ids, TEXT(", ")));
			return nullptr;
		}
		OutError = FString::Printf(TEXT("Node \"%s\" not found in %s%s. Use bp_read_graph to list node ids."), *InNodeId, *GetObjectPath(BP), Graph ? *FString::Printf(TEXT(" graph %s"), *Graph->GetName()) : TEXT(""));
		return nullptr;
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& InPinName, const FString& InDirection, FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("Pin lookup on a null node.");
			return nullptr;
		}
		FString PinName = InPinName.TrimStartAndEnd();
		FString Direction = InDirection.ToLower();
		int32 ColonIndex = INDEX_NONE;
		if (PinName.FindLastChar(TEXT(':'), ColonIndex))
		{
			const FString Suffix = PinName.Mid(ColonIndex + 1).ToLower();
			if (Suffix == TEXT("in") || Suffix == TEXT("out") || Suffix == TEXT("input") || Suffix == TEXT("output"))
			{
				Direction = Suffix;
				PinName = PinName.Left(ColonIndex);
			}
		}
		EEdGraphPinDirection WantedDirection = EGPD_MAX;
		if (Direction == TEXT("in") || Direction == TEXT("input")) WantedDirection = EGPD_Input;
		else if (Direction == TEXT("out") || Direction == TEXT("output")) WantedDirection = EGPD_Output;

		if (PinName.IsEmpty())
		{
			OutError = TEXT("Missing pin name.");
			return nullptr;
		}

		auto DirectionOk = [&](const UEdGraphPin* Pin) { return WantedDirection == EGPD_MAX || Pin->Direction == WantedDirection; };
		FString Compact = PinName.Replace(TEXT(" "), TEXT(""));

		TArray<UEdGraphPin*> Matches;
		// Pass 1: exact internal name
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && DirectionOk(Pin) && Pin->PinName.ToString() == PinName) Matches.Add(Pin);
		}
		// Pass 2: case-insensitive internal name
		if (Matches.Num() == 0)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && DirectionOk(Pin) && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase)) Matches.Add(Pin);
			}
		}
		// Pass 3: display name, ignoring spaces and case ("In String" == "InString")
		if (Matches.Num() == 0)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || !DirectionOk(Pin)) continue;
				const FString Display = Pin->GetDisplayName().ToString().Replace(TEXT(" "), TEXT(""));
				if (Display.Equals(Compact, ESearchCase::IgnoreCase) || Pin->PinName.ToString().Replace(TEXT(" "), TEXT("")).Equals(Compact, ESearchCase::IgnoreCase))
				{
					Matches.Add(Pin);
				}
			}
		}
		// Pass 4: common aliases for the execution pins
		if (Matches.Num() == 0)
		{
			const FString Lower = PinName.ToLower();
			FName Alias = NAME_None;
			if (Lower == TEXT("exec") || Lower == TEXT("in") || Lower == TEXT("execute")) Alias = UEdGraphSchema_K2::PN_Execute;
			else if (Lower == TEXT("out") || Lower == TEXT("then")) Alias = UEdGraphSchema_K2::PN_Then;
			if (!Alias.IsNone())
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && DirectionOk(Pin) && Pin->PinName == Alias) Matches.Add(Pin);
				}
			}
		}
		// Pass 5: the output of a variable Set node is called Output_Get; accept the variable name with direction out.
		if (Matches.Num() == 0 && WantedDirection == EGPD_Output)
		{
			if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
			{
				if (SetNode->GetVarNameString().Equals(PinName, ESearchCase::IgnoreCase) || SetNode->GetVarNameString().Replace(TEXT(" "), TEXT("")).Equals(Compact, ESearchCase::IgnoreCase))
				{
					// UK2Node_VariableSet::GetVariableOutputPinName() is not exported; the name is fixed in the engine.
					if (UEdGraphPin* OutputPin = SetNode->FindPin(FName(TEXT("Output_Get")), EGPD_Output))
					{
						Matches.Add(OutputPin);
					}
				}
			}
		}

		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		if (Matches.Num() > 1)
		{
			// Same name on both directions: ask for a direction.
			TArray<FString> Descriptions;
			for (UEdGraphPin* Pin : Matches) Descriptions.Add(FString::Printf(TEXT("%s:%s"), *Pin->PinName.ToString(), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out")));
			OutError = FString::Printf(TEXT("Pin \"%s\" on node %s is ambiguous (%s). Add a direction, e.g. \"%s:out\"."), *PinName, *NodeId(Node), *FString::Join(Descriptions, TEXT(", ")), *PinName);
			return nullptr;
		}

		TArray<FString> Available;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden)
			{
				Available.Add(FString::Printf(TEXT("%s (%s)"), *Pin->PinName.ToString(), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out")));
			}
		}
		OutError = FString::Printf(TEXT("Pin \"%s\" not found on node %s \"%s\". Available pins: %s"), *PinName, *NodeId(Node), *NodeTitle(Node), *FString::Join(Available, TEXT(", ")));
		return nullptr;
	}

	UEdGraphPin* ResolvePinRef(UBlueprint* BP, UEdGraph* Graph, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			OutError = TEXT("Missing pin reference. Use {\"node\":\"ID\",\"pin\":\"Name\"} or the string \"ID.Name\".");
			return nullptr;
		}
		FString NodeRef, PinName, Direction;
		if (Value->Type == EJson::String)
		{
			const FString Text = Value->AsString();
			int32 DotIndex = INDEX_NONE;
			if (!Text.FindChar(TEXT('.'), DotIndex))
			{
				OutError = FString::Printf(TEXT("Pin reference \"%s\" must look like \"NODEID.PinName\"."), *Text);
				return nullptr;
			}
			NodeRef = Text.Left(DotIndex);
			PinName = Text.Mid(DotIndex + 1);
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			NodeRef = GetString(*Object, TEXT("node"));
			PinName = GetString(*Object, TEXT("pin"));
			Direction = GetString(*Object, TEXT("direction"), GetString(*Object, TEXT("dir")));
		}
		else
		{
			OutError = TEXT("Pin reference must be an object or a string.");
			return nullptr;
		}

		UEdGraphNode* Node = FindNode(BP, Graph, NodeRef, OutError);
		if (!Node)
		{
			return nullptr;
		}
		return FindPin(Node, PinName, Direction, OutError);
	}

	FString GraphKind(const UBlueprint* BP, const UEdGraph* Graph)
	{
		if (!BP || !Graph) return TEXT("graph");
		auto ContainsGraph = [Graph](const TArray<TObjectPtr<UEdGraph>>& List)
		{
			for (const TObjectPtr<UEdGraph>& Entry : List)
			{
				if (Entry.Get() == Graph) return true;
			}
			return false;
		};
		if (ContainsGraph(BP->UbergraphPages)) return TEXT("ubergraph");
		if (ContainsGraph(BP->FunctionGraphs)) return TEXT("function");
		if (ContainsGraph(BP->MacroGraphs)) return TEXT("macro");
		if (ContainsGraph(BP->DelegateSignatureGraphs)) return TEXT("delegate_signature");
		if (Graph->GetOuter() && Graph->GetOuter()->IsA<UEdGraph>()) return TEXT("subgraph");
		if (const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema()))
		{
			switch (Schema->GetGraphType(Graph))
			{
			case GT_Function: return TEXT("function");
			case GT_Ubergraph: return TEXT("ubergraph");
			case GT_Macro: return TEXT("macro");
			default: break;
			}
		}
		return TEXT("graph");
	}

	FString NodeTitle(const UEdGraphNode* Node)
	{
		if (!Node) return FString();
		FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Title.ReplaceInline(TEXT("\r"), TEXT(""));
		Title.ReplaceInline(TEXT("\n"), TEXT(" | "));
		return Title.TrimStartAndEnd();
	}

	FString StatusToString(const UBlueprint* BP)
	{
		if (!BP) return TEXT("Unknown");
		switch (BP->Status)
		{
		case BS_Dirty: return TEXT("Dirty");
		case BS_Error: return TEXT("Error");
		case BS_UpToDate: return TEXT("UpToDate");
		case BS_BeingCreated: return TEXT("BeingCreated");
		case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
		default: return TEXT("Unknown");
		}
	}

	FString BlueprintTypeToString(const UBlueprint* BP)
	{
		if (!BP) return TEXT("Unknown");
		switch (BP->BlueprintType)
		{
		case BPTYPE_Normal: return TEXT("Normal");
		case BPTYPE_Const: return TEXT("Const");
		case BPTYPE_MacroLibrary: return TEXT("MacroLibrary");
		case BPTYPE_Interface: return TEXT("Interface");
		case BPTYPE_LevelScript: return TEXT("LevelScript");
		case BPTYPE_FunctionLibrary: return TEXT("FunctionLibrary");
		default: return TEXT("Unknown");
		}
	}

	// ---------------------------------------------------------------------------------------
	// Reflection lookups

	UClass* GetBlueprintSelfClass(const UBlueprint* BP)
	{
		if (!BP) return nullptr;
		if (BP->SkeletonGeneratedClass) return BP->SkeletonGeneratedClass;
		if (BP->GeneratedClass) return BP->GeneratedClass;
		return BP->ParentClass;
	}

	FString ClassDisplayName(const UClass* Class)
	{
		if (!Class) return TEXT("None");
		FString Name = Class->GetName();
		Name.RemoveFromStart(TEXT("SKEL_"));
		return Name;
	}

	FString FunctionDisplay(const UFunction* Function)
	{
		if (!Function) return TEXT("None");
		return FString::Printf(TEXT("%s.%s"), *ClassDisplayName(Function->GetOwnerClass()), *Function->GetName());
	}

	/** Short name or path lookup without the "short type name" warning that TryFindTypeSlow logs. */
	template <typename T>
	static T* FindTypeQuiet(const FString& Name)
	{
		if (Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")))
		{
			if (T* Found = FindObject<T>(nullptr, *Name))
			{
				return Found;
			}
			return LoadObject<T>(nullptr, *Name, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return FindFirstObject<T>(*Name, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
	}

	UClass* ResolveClass(const FString& InName, FString& OutError)
	{
		FString Name = InName.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			OutError = TEXT("Missing class name.");
			return nullptr;
		}
		// Strip the UE text-export decoration: Class'/Script/Engine.Actor'
		if (Name.EndsWith(TEXT("'")))
		{
			int32 Quote = INDEX_NONE;
			if (Name.FindChar(TEXT('\''), Quote))
			{
				Name = Name.Mid(Quote + 1).LeftChop(1);
			}
		}

		// Content path: a Blueprint asset
		if (Name.StartsWith(TEXT("/")) && !Name.StartsWith(TEXT("/Script/")))
		{
			FString Error;
			FString AssetPath = Name;
			AssetPath.RemoveFromEnd(TEXT("_C"));
			if (UBlueprint* BP = FindBlueprint(AssetPath, Error))
			{
				if (BP->GeneratedClass) return BP->GeneratedClass;
			}
			OutError = FString::Printf(TEXT("No Blueprint class at %s (%s)"), *Name, *Error);
			return nullptr;
		}

		if (UClass* Found = FindTypeQuiet<UClass>(Name))
		{
			return Found;
		}
		// Native prefixes: AActor, UStaticMeshComponent
		if (Name.Len() > 2 && (Name[0] == TEXT('A') || Name[0] == TEXT('U')) && FChar::IsUpper(Name[1]))
		{
			if (UClass* Found = FindTypeQuiet<UClass>(Name.Mid(1)))
			{
				return Found;
			}
		}
		// Blueprint asset name, with or without the _C suffix
		{
			FString AssetName = Name;
			AssetName.RemoveFromEnd(TEXT("_C"));
			FString Error;
			if (UBlueprint* BP = FindBlueprint(AssetName, Error))
			{
				if (BP->GeneratedClass) return BP->GeneratedClass;
			}
		}
		OutError = FString::Printf(TEXT("Class not found: \"%s\". Use the reflected name without prefix (Actor, StaticMeshComponent), a Blueprint asset path (/Game/BP_Foo) or a script path (/Script/Engine.Actor)."), *Name);
		return nullptr;
	}

	UScriptStruct* ResolveStruct(const FString& InName, FString& OutError)
	{
		FString Name = InName.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			OutError = TEXT("Missing struct name.");
			return nullptr;
		}
		if (UScriptStruct* Found = FindTypeQuiet<UScriptStruct>(Name))
		{
			return Found;
		}
		if (Name.Len() > 2 && Name[0] == TEXT('F') && FChar::IsUpper(Name[1]))
		{
			if (UScriptStruct* Found = FindTypeQuiet<UScriptStruct>(Name.Mid(1)))
			{
				return Found;
			}
		}
		OutError = FString::Printf(TEXT("Struct not found: \"%s\" (use names like Vector, Rotator, LinearColor, Transform or a user struct asset path)."), *Name);
		return nullptr;
	}

	UEnum* ResolveEnum(const FString& InName, FString& OutError)
	{
		FString Name = InName.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			OutError = TEXT("Missing enum name.");
			return nullptr;
		}
		if (UEnum* Found = FindTypeQuiet<UEnum>(Name))
		{
			return Found;
		}
		if (!Name.StartsWith(TEXT("E")))
		{
			if (UEnum* Found = FindTypeQuiet<UEnum>(TEXT("E") + Name))
			{
				return Found;
			}
		}
		OutError = FString::Printf(TEXT("Enum not found: \"%s\"."), *Name);
		return nullptr;
	}

	bool FunctionNameMatches(const UFunction* Function, const FString& Query)
	{
		if (!Function) return false;
		const FString Name = Function->GetName();
		if (Name.Equals(Query, ESearchCase::IgnoreCase)) return true;
		if (Name.Equals(TEXT("K2_") + Query, ESearchCase::IgnoreCase)) return true;
		if (Name.Equals(TEXT("Receive") + Query, ESearchCase::IgnoreCase)) return true;
		if (Name.Equals(TEXT("BP_") + Query, ESearchCase::IgnoreCase)) return true;
		const FString QueryCompact = Query.Replace(TEXT(" "), TEXT(""));
		FString Display = Function->GetDisplayNameText().ToString();
		Display.ReplaceInline(TEXT(" "), TEXT(""));
		return Display.Equals(QueryCompact, ESearchCase::IgnoreCase);
	}

	static bool IsStaleClass(const UClass* Class)
	{
		if (!Class) return true;
		const FString Name = Class->GetName();
		return Name.StartsWith(TEXT("SKEL_")) || Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASHCLASS_")) || Name.StartsWith(TEXT("HOTRELOADED_")) || Class->HasAnyClassFlags(CLASS_NewerVersionExists);
	}

	UFunction* ResolveFunction(UBlueprint* BP, const FString& InSpec, const FString& ClassHint, FString& OutError)
	{
		FString Spec = InSpec.TrimStartAndEnd();
		FString ClassName = ClassHint.TrimStartAndEnd();
		FString FuncName = Spec;

		int32 SeparatorIndex = INDEX_NONE;
		if (Spec.FindLastChar(TEXT(':'), SeparatorIndex))
		{
			ClassName = Spec.Left(SeparatorIndex);
			FuncName = Spec.Mid(SeparatorIndex + 1);
		}
		else if (!Spec.StartsWith(TEXT("/")) && Spec.FindLastChar(TEXT('.'), SeparatorIndex))
		{
			ClassName = Spec.Left(SeparatorIndex);
			FuncName = Spec.Mid(SeparatorIndex + 1);
		}
		FuncName = FuncName.TrimStartAndEnd();
		if (FuncName.IsEmpty())
		{
			OutError = TEXT("Missing function name.");
			return nullptr;
		}

		UClass* SelfClass = GetBlueprintSelfClass(BP);

		if (!ClassName.IsEmpty())
		{
			UClass* Class = nullptr;
			if (ClassName.Equals(TEXT("self"), ESearchCase::IgnoreCase) || ClassName.Equals(TEXT("this"), ESearchCase::IgnoreCase))
			{
				Class = SelfClass;
				if (!Class)
				{
					OutError = TEXT("\"self\" requires a Blueprint context.");
					return nullptr;
				}
			}
			else
			{
				Class = ResolveClass(ClassName, OutError);
				if (!Class) return nullptr;
			}
			if (UFunction* Exact = Class->FindFunctionByName(FName(*FuncName)))
			{
				return Exact;
			}
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (FunctionNameMatches(*It, FuncName))
				{
					return *It;
				}
			}
			OutError = FString::Printf(TEXT("Function \"%s\" not found on class %s."), *FuncName, *ClassDisplayName(Class));
			return nullptr;
		}

		// 1) Functions callable on self (own, inherited, interfaces)
		if (SelfClass)
		{
			if (UFunction* Exact = SelfClass->FindFunctionByName(FName(*FuncName)))
			{
				return Exact;
			}
			for (TFieldIterator<UFunction> It(SelfClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (FunctionNameMatches(*It, FuncName))
				{
					return *It;
				}
			}
		}

		// 2) Global scan of Blueprint-callable functions
		TArray<UFunction*> Candidates;
		TArray<UFunction*> DeprecatedCandidates;
		for (TObjectIterator<UFunction> It; It; ++It)
		{
			UFunction* Function = *It;
			if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;
			UClass* Owner = Function->GetOwnerClass();
			if (IsStaleClass(Owner)) continue;
			if (!FunctionNameMatches(Function, FuncName)) continue;
			if (Function->HasMetaData(TEXT("DeprecatedFunction")) || Owner->HasAnyClassFlags(CLASS_Deprecated))
			{
				DeprecatedCandidates.Add(Function);
			}
			else
			{
				Candidates.Add(Function);
			}
		}
		if (Candidates.Num() == 0)
		{
			Candidates = DeprecatedCandidates;
		}
		if (Candidates.Num() == 1)
		{
			return Candidates[0];
		}
		if (Candidates.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Function \"%s\" not found. Use bp_search_nodes to find the exact name and owning class, then pass \"Class.Function\"."), *FuncName);
			return nullptr;
		}
		// Prefer an exact (case-sensitive) name match if it is unique.
		TArray<UFunction*> ExactMatches;
		for (UFunction* F : Candidates) if (F->GetName() == FuncName) ExactMatches.Add(F);
		if (ExactMatches.Num() == 1)
		{
			return ExactMatches[0];
		}
		TArray<FString> Names;
		for (UFunction* F : Candidates) Names.Add(FunctionDisplay(F));
		Names.Sort();
		OutError = FString::Printf(TEXT("Function name \"%s\" is ambiguous. Specify the class, one of: %s"), *FuncName, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	// ---------------------------------------------------------------------------------------
	// Pin types

	static FString TerminalTypeToString(FName Category, FName SubCategory, UObject* SubObject)
	{
		auto ObjName = [SubObject, SubCategory](const TCHAR* Prefix) -> FString
		{
			if (SubCategory == UEdGraphSchema_K2::PSC_Self) return FString::Printf(TEXT("%s:self"), Prefix);
			if (const UClass* AsClass = Cast<UClass>(SubObject)) return FString::Printf(TEXT("%s:%s"), Prefix, *ClassDisplayName(AsClass));
			if (SubObject) return FString::Printf(TEXT("%s:%s"), Prefix, *SubObject->GetName());
			return FString(Prefix);
		};

		if (Category == UEdGraphSchema_K2::PC_Exec) return TEXT("exec");
		if (Category == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
		if (Category == UEdGraphSchema_K2::PC_Byte)
		{
			if (UEnum* Enum = Cast<UEnum>(SubObject)) return FString::Printf(TEXT("enum:%s"), *Enum->GetName());
			return TEXT("byte");
		}
		if (Category == UEdGraphSchema_K2::PC_Int) return TEXT("int");
		if (Category == UEdGraphSchema_K2::PC_Int64) return TEXT("int64");
		if (Category == UEdGraphSchema_K2::PC_Real || Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double) return TEXT("float");
		if (Category == UEdGraphSchema_K2::PC_Name) return TEXT("name");
		if (Category == UEdGraphSchema_K2::PC_String) return TEXT("string");
		if (Category == UEdGraphSchema_K2::PC_Text) return TEXT("text");
		if (Category == UEdGraphSchema_K2::PC_Object) return ObjName(TEXT("object"));
		if (Category == UEdGraphSchema_K2::PC_Class) return ObjName(TEXT("class"));
		if (Category == UEdGraphSchema_K2::PC_SoftObject) return ObjName(TEXT("softobject"));
		if (Category == UEdGraphSchema_K2::PC_SoftClass) return ObjName(TEXT("softclass"));
		if (Category == UEdGraphSchema_K2::PC_Interface) return ObjName(TEXT("interface"));
		if (Category == UEdGraphSchema_K2::PC_Struct) return SubObject ? FString::Printf(TEXT("struct:%s"), *SubObject->GetName()) : TEXT("struct");
		if (Category == UEdGraphSchema_K2::PC_Enum) return SubObject ? FString::Printf(TEXT("enum:%s"), *SubObject->GetName()) : TEXT("enum");
		if (Category == UEdGraphSchema_K2::PC_Delegate) return TEXT("delegate");
		if (Category == UEdGraphSchema_K2::PC_MCDelegate) return TEXT("mcdelegate");
		if (Category == UEdGraphSchema_K2::PC_Wildcard) return TEXT("wildcard");
		if (Category == UEdGraphSchema_K2::PC_FieldPath) return TEXT("fieldpath");
		return Category.ToString();
	}

	FString PinTypeToString(const FEdGraphPinType& Type)
	{
		const FString Inner = TerminalTypeToString(Type.PinCategory, Type.PinSubCategory, Type.PinSubCategoryObject.Get());
		FString Result;
		switch (Type.ContainerType)
		{
		case EPinContainerType::Array: Result = FString::Printf(TEXT("array<%s>"), *Inner); break;
		case EPinContainerType::Set: Result = FString::Printf(TEXT("set<%s>"), *Inner); break;
		case EPinContainerType::Map:
			Result = FString::Printf(TEXT("map<%s,%s>"), *Inner, *TerminalTypeToString(Type.PinValueType.TerminalCategory, Type.PinValueType.TerminalSubCategory, Type.PinValueType.TerminalSubCategoryObject.Get()));
			break;
		default: Result = Inner; break;
		}
		if (Type.bIsReference)
		{
			Result += TEXT("&");
		}
		return Result;
	}

	static bool ParseTerminalType(const FString& InText, FName& OutCategory, FName& OutSubCategory, UObject*& OutSubObject, FString& OutError)
	{
		FString Text = InText.TrimStartAndEnd();
		OutCategory = NAME_None;
		OutSubCategory = NAME_None;
		OutSubObject = nullptr;

		FString Kind = Text;
		FString Arg;
		int32 Sep = INDEX_NONE;
		if (Text.FindChar(TEXT(':'), Sep))
		{
			Kind = Text.Left(Sep);
			Arg = Text.Mid(Sep + 1);
		}
		else if (Text.FindChar(TEXT('<'), Sep) && Text.EndsWith(TEXT(">")))
		{
			Kind = Text.Left(Sep);
			Arg = Text.Mid(Sep + 1).LeftChop(1);
		}
		Kind = Kind.TrimStartAndEnd().ToLower();
		Arg = Arg.TrimStartAndEnd();

		if (Kind == TEXT("exec")) { OutCategory = UEdGraphSchema_K2::PC_Exec; return true; }
		if (Kind == TEXT("bool") || Kind == TEXT("boolean")) { OutCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
		if (Kind == TEXT("byte") || Kind == TEXT("uint8")) { OutCategory = UEdGraphSchema_K2::PC_Byte; return true; }
		if (Kind == TEXT("int") || Kind == TEXT("int32") || Kind == TEXT("integer")) { OutCategory = UEdGraphSchema_K2::PC_Int; return true; }
		if (Kind == TEXT("int64")) { OutCategory = UEdGraphSchema_K2::PC_Int64; return true; }
		if (Kind == TEXT("float") || Kind == TEXT("double") || Kind == TEXT("real")) { OutCategory = UEdGraphSchema_K2::PC_Real; OutSubCategory = UEdGraphSchema_K2::PC_Double; return true; }
		if (Kind == TEXT("float32") || Kind == TEXT("single")) { OutCategory = UEdGraphSchema_K2::PC_Real; OutSubCategory = UEdGraphSchema_K2::PC_Float; return true; }
		if (Kind == TEXT("name")) { OutCategory = UEdGraphSchema_K2::PC_Name; return true; }
		if (Kind == TEXT("string")) { OutCategory = UEdGraphSchema_K2::PC_String; return true; }
		if (Kind == TEXT("text")) { OutCategory = UEdGraphSchema_K2::PC_Text; return true; }
		if (Kind == TEXT("wildcard")) { OutCategory = UEdGraphSchema_K2::PC_Wildcard; return true; }
		if (Kind == TEXT("delegate")) { OutCategory = UEdGraphSchema_K2::PC_Delegate; return true; }
		if (Kind == TEXT("mcdelegate") || Kind == TEXT("multicastdelegate") || Kind == TEXT("eventdispatcher")) { OutCategory = UEdGraphSchema_K2::PC_MCDelegate; return true; }
		if (Kind == TEXT("fieldpath")) { OutCategory = UEdGraphSchema_K2::PC_FieldPath; return true; }

		if (Kind == TEXT("object") || Kind == TEXT("class") || Kind == TEXT("softobject") || Kind == TEXT("softclass") || Kind == TEXT("interface"))
		{
			UClass* Class = nullptr;
			if (Arg.IsEmpty() || Arg.Equals(TEXT("self"), ESearchCase::IgnoreCase))
			{
				if (Arg.IsEmpty()) Class = UObject::StaticClass();
			}
			else
			{
				Class = ResolveClass(Arg, OutError);
				if (!Class) return false;
			}
			if (Kind == TEXT("object")) OutCategory = UEdGraphSchema_K2::PC_Object;
			else if (Kind == TEXT("class")) OutCategory = UEdGraphSchema_K2::PC_Class;
			else if (Kind == TEXT("softobject")) OutCategory = UEdGraphSchema_K2::PC_SoftObject;
			else if (Kind == TEXT("softclass")) OutCategory = UEdGraphSchema_K2::PC_SoftClass;
			else OutCategory = UEdGraphSchema_K2::PC_Interface;
			if (Class) OutSubObject = Class; else OutSubCategory = UEdGraphSchema_K2::PSC_Self;
			return true;
		}
		if (Kind == TEXT("struct"))
		{
			UScriptStruct* Struct = ResolveStruct(Arg, OutError);
			if (!Struct) return false;
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			OutSubObject = Struct;
			return true;
		}
		if (Kind == TEXT("enum"))
		{
			UEnum* Enum = ResolveEnum(Arg, OutError);
			if (!Enum) return false;
			OutCategory = UEdGraphSchema_K2::PC_Byte;
			OutSubObject = Enum;
			return true;
		}

		// Bare type name: struct, enum or class
		FString Ignored;
		if (UScriptStruct* Struct = ResolveStruct(Text, Ignored))
		{
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			OutSubObject = Struct;
			return true;
		}
		if (UEnum* Enum = ResolveEnum(Text, Ignored))
		{
			OutCategory = UEdGraphSchema_K2::PC_Byte;
			OutSubObject = Enum;
			return true;
		}
		if (UClass* Class = ResolveClass(Text, Ignored))
		{
			OutCategory = UEdGraphSchema_K2::PC_Object;
			OutSubObject = Class;
			return true;
		}
		OutError = FString::Printf(TEXT("Unknown type \"%s\". Expected bool, byte, int, int64, float, name, string, text, object:Class, class:Class, softobject:Class, struct:Name, enum:Name, or array<T> / set<T> / map<K,V>."), *InText);
		return false;
	}

	bool ParsePinType(const FString& InText, FEdGraphPinType& Out, FString& OutError)
	{
		FString Text = InText.TrimStartAndEnd();
		Out = FEdGraphPinType();
		if (Text.IsEmpty())
		{
			OutError = TEXT("Missing type.");
			return false;
		}
		if (Text.EndsWith(TEXT("&")))
		{
			Out.bIsReference = true;
			Text = Text.LeftChop(1).TrimStartAndEnd();
		}
		else if (Text.EndsWith(TEXT(" ref"), ESearchCase::IgnoreCase))
		{
			Out.bIsReference = true;
			Text = Text.LeftChop(4).TrimStartAndEnd();
		}

		FString Inner = Text;
		FString ValueText;
		const FString Lower = Text.ToLower();
		if (Lower.StartsWith(TEXT("array<")) && Text.EndsWith(TEXT(">")))
		{
			Out.ContainerType = EPinContainerType::Array;
			Inner = Text.Mid(6).LeftChop(1);
		}
		else if (Text.EndsWith(TEXT("[]")))
		{
			Out.ContainerType = EPinContainerType::Array;
			Inner = Text.LeftChop(2);
		}
		else if (Lower.StartsWith(TEXT("set<")) && Text.EndsWith(TEXT(">")))
		{
			Out.ContainerType = EPinContainerType::Set;
			Inner = Text.Mid(4).LeftChop(1);
		}
		else if (Lower.StartsWith(TEXT("map<")) && Text.EndsWith(TEXT(">")))
		{
			Out.ContainerType = EPinContainerType::Map;
			const FString Body = Text.Mid(4).LeftChop(1);
			// split on the top-level comma
			int32 Depth = 0;
			int32 SplitIndex = INDEX_NONE;
			for (int32 i = 0; i < Body.Len(); ++i)
			{
				if (Body[i] == TEXT('<')) Depth++;
				else if (Body[i] == TEXT('>')) Depth--;
				else if (Body[i] == TEXT(',') && Depth == 0) { SplitIndex = i; break; }
			}
			if (SplitIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Map type \"%s\" needs a key and a value: map<string,int>."), *InText);
				return false;
			}
			Inner = Body.Left(SplitIndex);
			ValueText = Body.Mid(SplitIndex + 1);
		}

		UObject* SubObject = nullptr;
		if (!ParseTerminalType(Inner, Out.PinCategory, Out.PinSubCategory, SubObject, OutError))
		{
			return false;
		}
		Out.PinSubCategoryObject = SubObject;

		if (Out.ContainerType == EPinContainerType::Map)
		{
			FName ValueCategory, ValueSubCategory;
			UObject* ValueObject = nullptr;
			if (!ParseTerminalType(ValueText, ValueCategory, ValueSubCategory, ValueObject, OutError))
			{
				return false;
			}
			Out.PinValueType.TerminalCategory = ValueCategory;
			Out.PinValueType.TerminalSubCategory = ValueSubCategory;
			Out.PinValueType.TerminalSubCategoryObject = ValueObject;
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------
	// Pin defaults

	FString PinDefaultToString(const UEdGraphPin* Pin)
	{
		if (!Pin) return FString();
		if (!Pin->DefaultTextValue.IsEmpty())
		{
			return Pin->DefaultTextValue.ToString();
		}
		return Pin->GetDefaultAsString();
	}

	static bool IsCommaSeparatedStruct(const UScriptStruct* Struct)
	{
		if (!Struct) return false;
		static const TSet<FName> Names = {
			TEXT("Vector"), TEXT("Vector2D"), TEXT("Vector4"), TEXT("Rotator"),
			TEXT("Vector3f"), TEXT("Vector2f"), TEXT("Vector4f"), TEXT("Rotator3f"),
			TEXT("Vector3d"), TEXT("Vector2d"), TEXT("Vector4d"), TEXT("Rotator3d")
		};
		return Names.Contains(Struct->GetFName());
	}

	static FString QuoteForImportText(const FString& Value)
	{
		FString Escaped = Value.ReplaceCharWithEscapedChar();
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static FString NumberToString(double Value, bool bIntegral)
	{
		if (bIntegral || FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
		{
			return bIntegral ? FString::Printf(TEXT("%lld"), (int64)FMath::RoundToDouble(Value)) : FString::Printf(TEXT("%lld.0"), (int64)FMath::RoundToDouble(Value));
		}
		return FString::SanitizeFloat(Value);
	}

	static FString JsonValueToUEStringInternal(const TSharedPtr<FJsonValue>& Value, const UStruct* StructHint, bool bNested)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::Boolean: return Value->AsBool() ? TEXT("True") : TEXT("False");
		case EJson::Number: return NumberToString(Value->AsNumber(), false);
		case EJson::String: return bNested ? QuoteForImportText(Value->AsString()) : Value->AsString();
		case EJson::Array:
		{
			TArray<FString> Items;
			if (StructHint)
			{
				// Positional struct literal: [1, 2, 3] -> (X=1,Y=2,Z=3)
				int32 Index = 0;
				const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
				for (TFieldIterator<FProperty> It(StructHint); It && Index < Array.Num(); ++It, ++Index)
				{
					const UStruct* Inner = nullptr;
					if (const FStructProperty* StructProp = CastField<FStructProperty>(*It)) Inner = StructProp->Struct;
					Items.Add(FString::Printf(TEXT("%s=%s"), *It->GetName(), *JsonValueToUEStringInternal(Array[Index], Inner, true)));
				}
				return FString::Printf(TEXT("(%s)"), *FString::Join(Items, TEXT(",")));
			}
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Items.Add(JsonValueToUEStringInternal(Item, nullptr, true));
			}
			return FString::Printf(TEXT("(%s)"), *FString::Join(Items, TEXT(",")));
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			TArray<FString> Items;
			TSet<FString> Consumed;
			if (StructHint)
			{
				for (TFieldIterator<FProperty> It(StructHint); It; ++It)
				{
					const FProperty* Property = *It;
					const FString PropName = Property->GetName();
					const FString DisplayName = Property->GetDisplayNameText().ToString().Replace(TEXT(" "), TEXT(""));
					for (const auto& Pair : Object->Values)
					{
						if (Pair.Key.Equals(PropName, ESearchCase::IgnoreCase) || Pair.Key.Equals(DisplayName, ESearchCase::IgnoreCase))
						{
							const UStruct* Inner = nullptr;
							if (const FStructProperty* StructProp = CastField<FStructProperty>(Property)) Inner = StructProp->Struct;
							Items.Add(FString::Printf(TEXT("%s=%s"), *PropName, *JsonValueToUEStringInternal(Pair.Value, Inner, true)));
							Consumed.Add(Pair.Key);
							break;
						}
					}
				}
			}
			for (const auto& Pair : Object->Values)
			{
				if (!Consumed.Contains(Pair.Key))
				{
					Items.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *JsonValueToUEStringInternal(Pair.Value, nullptr, true)));
				}
			}
			return FString::Printf(TEXT("(%s)"), *FString::Join(Items, TEXT(",")));
		}
		default:
			return FString();
		}
	}

	FString JsonValueToUEString(const TSharedPtr<FJsonValue>& Value, const UStruct* StructHint)
	{
		return JsonValueToUEStringInternal(Value, StructHint, false);
	}

	FString JsonValueToPinDefault(const UEdGraphPin* Pin, const TSharedPtr<FJsonValue>& Value)
	{
		if (!Pin || !Value.IsValid() || Value->Type == EJson::Null)
		{
			return FString();
		}
		const FEdGraphPinType& Type = Pin->PinType;
		const bool bIntegral = Type.PinCategory == UEdGraphSchema_K2::PC_Int || Type.PinCategory == UEdGraphSchema_K2::PC_Int64 || Type.PinCategory == UEdGraphSchema_K2::PC_Byte;
		const UScriptStruct* Struct = Type.PinCategory == UEdGraphSchema_K2::PC_Struct ? Cast<UScriptStruct>(Type.PinSubCategoryObject.Get()) : nullptr;

		if (Value->Type == EJson::Number)
		{
			return NumberToString(Value->AsNumber(), bIntegral);
		}
		if (Value->Type == EJson::Boolean)
		{
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		}
		if (Value->Type == EJson::String)
		{
			return Value->AsString();
		}
		if (Struct && IsCommaSeparatedStruct(Struct))
		{
			TArray<FString> Parts;
			if (Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					Parts.Add(Item.IsValid() && Item->Type == EJson::Number ? FString::SanitizeFloat(Item->AsNumber()) : (Item.IsValid() ? Item->AsString() : TEXT("0")));
				}
			}
			else if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>& Object = Value->AsObject();
				for (TFieldIterator<FProperty> It(Struct); It; ++It)
				{
					double Component = 0.0;
					const FString PropName = It->GetName();
					for (const auto& Pair : Object->Values)
					{
						if (Pair.Key.Equals(PropName, ESearchCase::IgnoreCase) && Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
						{
							Component = Pair.Value->AsNumber();
						}
					}
					Parts.Add(FString::SanitizeFloat(Component));
				}
			}
			return FString::Join(Parts, TEXT(", "));
		}
		return JsonValueToUEString(Value, Struct);
	}

	// ---------------------------------------------------------------------------------------

	bool EnableGhostNode(UEdGraphNode* Node)
	{
		if (!Node || !Node->IsAutomaticallyPlacedGhostNode())
		{
			return false;
		}
		Node->Modify();
		Node->NodeComment.Empty();
		Node->bCommentBubbleVisible = false;
		Node->SetEnabledState(ENodeEnabledState::Enabled, true);
		return true;
	}

	void MarkModified(UBlueprint* BP, UEdGraphNode* Node)
	{
		if (!BP) return;
		bool bStructural = false;
		if (const UK2Node* K2Node = Cast<UK2Node>(Node))
		{
			bStructural = K2Node->NodeCausesStructuralBlueprintChange();
		}
		if (bStructural)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		}
	}
}
