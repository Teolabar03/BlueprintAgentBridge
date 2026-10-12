// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABOpsCommon.h"
#include "BABHttpServer.h"
#include "BlueprintAgentBridgeModule.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphToken.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/UObjectToken.h"
#include "Logging/TokenizedMessage.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

namespace BAB
{
	static const TCHAR* SeverityToString(EMessageSeverity::Type Severity)
	{
		switch (Severity)
		{
		case EMessageSeverity::Error: return TEXT("error");
		case EMessageSeverity::PerformanceWarning: return TEXT("performance_warning");
		case EMessageSeverity::Warning: return TEXT("warning");
		case EMessageSeverity::Info: return TEXT("info");
		default: return TEXT("note");
		}
	}

	void CompileBlueprintInto(UBlueprint* BP, FJsonObject& Data)
	{
		FCompilerResultsLog Results;
		Results.bAnnotateMentionedNodes = true;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

		TArray<TSharedPtr<FJsonValue>> Messages;
		TSet<FString> Seen;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			TSharedRef<FJsonObject> Entry = NewObject();
			Entry->SetStringField(TEXT("severity"), SeverityToString(Message->GetSeverity()));
			const FString Text = Message->ToText().ToString();
			Entry->SetStringField(TEXT("message"), Text);
			for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
			{
				const UEdGraphNode* Node = nullptr;
				if (Token->GetType() == EMessageToken::Object)
				{
					const FUObjectToken& ObjectToken = static_cast<const FUObjectToken&>(Token.Get());
					Node = Cast<UEdGraphNode>(ObjectToken.GetObject().Get());
				}
				else if (Token->GetType() == EMessageToken::EdGraph)
				{
					const FEdGraphToken& GraphToken = static_cast<const FEdGraphToken&>(Token.Get());
					if (const UEdGraphPin* Pin = GraphToken.GetPin())
					{
						Entry->SetStringField(TEXT("pin"), Pin->PinName.ToString());
						Node = Pin->GetOwningNodeUnchecked();
					}
					else
					{
						Node = Cast<UEdGraphNode>(GraphToken.GetGraphObject());
					}
				}
				if (Node && !Entry->HasField(TEXT("node")))
				{
					Entry->SetStringField(TEXT("node"), NodeId(Node));
					Entry->SetStringField(TEXT("node_title"), NodeTitle(Node));
					if (UEdGraph* Graph = Node->GetGraph()) Entry->SetStringField(TEXT("graph"), Graph->GetName());
				}
			}
			const FString Key = Entry->HasField(TEXT("node")) ? Entry->GetStringField(TEXT("node")) + Text : Text;
			if (!Seen.Contains(Key))
			{
				Seen.Add(Key);
				Messages.Add(ObjectValue(Entry));
			}
		}
		// Nodes annotated by the compiler that did not surface through the log tokens.
		TArray<UEdGraph*> Graphs;
		GetAllGraphs(BP, Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->bHasCompilerMessage && Node->ErrorType <= EMessageSeverity::Warning)
				{
					const FString Key = NodeId(Node) + Node->ErrorMsg;
					if (Seen.Contains(Key)) continue;
					Seen.Add(Key);
					TSharedRef<FJsonObject> Entry = NewObject();
					Entry->SetStringField(TEXT("severity"), Node->ErrorType <= EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
					Entry->SetStringField(TEXT("message"), Node->ErrorMsg);
					Entry->SetStringField(TEXT("node"), NodeId(Node));
					Entry->SetStringField(TEXT("node_title"), NodeTitle(Node));
					Entry->SetStringField(TEXT("graph"), Graph->GetName());
					Messages.Add(ObjectValue(Entry));
				}
			}
		}

		Data.SetStringField(TEXT("status"), StatusToString(BP));
		Data.SetBoolField(TEXT("success"), BP->Status != BS_Error && Results.NumErrors == 0);
		Data.SetNumberField(TEXT("num_errors"), Results.NumErrors);
		Data.SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
		Data.SetArrayField(TEXT("messages"), Messages);
	}

	FBABResult SaveBlueprintPackage(UBlueprint* BP)
	{
		FString Reason;
		if (!CheckPackageWritable(BP, Reason))
		{
			return FBABResult::Fail(Reason);
		}
		const FString Filename = GetPackageFilename(BP);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		if (IFileManager::Get().FileExists(*Filename))
		{
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			IFileHandle* Handle = PlatformFile.OpenWrite(*Filename, /*bAppend*/ true, /*bAllowRead*/ true);
			if (!Handle)
			{
				return FBABResult::Fail(FString::Printf(TEXT("%s is locked by another process (another editor instance?). Close it and retry."), *Filename));
			}
			delete Handle;
		}

		UPackage* Package = BP->GetOutermost();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_None;
		SaveArgs.Error = GError;
		const bool bSaved = UPackage::SavePackage(Package, BP, *Filename, SaveArgs);
		if (!bSaved)
		{
			return FBABResult::Fail(FString::Printf(TEXT("SavePackage failed for %s (see the engine log)."), *Filename));
		}
		FBABResult Result;
		Result.Data->SetBoolField(TEXT("saved"), true);
		Result.Data->SetStringField(TEXT("file"), FPaths::ConvertRelativePathToFull(Filename));
		Result.Data->SetStringField(TEXT("blueprint"), GetObjectPath(BP));
		return Result;
	}

	static FString ShortClassFromTag(const FString& TagValue)
	{
		FString Path = FPackageName::ExportTextPathToObjectPath(TagValue);
		FString Name = FPackageName::ObjectPathToObjectName(Path);
		return Name.IsEmpty() ? TagValue : Name;
	}
}

void BABRegisterOperations_Assets()
{
	using namespace BAB;
	FBABDispatcher& D = FBABDispatcher::Get();

	D.Register(TEXT("bp_status"), TEXT("Engine, project and server information plus the list of operations."), [](const FJsonObject&) -> FBABResult
	{
		FBABResult Result;
		Result.Data->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Result.Data->SetStringField(TEXT("project"), FApp::GetProjectName());
		Result.Data->SetStringField(TEXT("project_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Result.Data->SetStringField(TEXT("mode"), IsRunningCommandlet() ? TEXT("commandlet") : TEXT("editor"));
		Result.Data->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("BlueprintAgentBridge")))
		{
			Result.Data->SetStringField(TEXT("plugin_version"), Plugin->GetDescriptor().VersionName);
		}
		Result.Data->SetBoolField(TEXT("server_running"), FBABHttpServer::Get().IsRunning());
		Result.Data->SetNumberField(TEXT("server_port"), FBABHttpServer::Get().GetPort());
		SetStringArray(*Result.Data, TEXT("operations"), FBABDispatcher::Get().GetOperationNames());
		return Result;
	});

	D.Register(TEXT("bp_list_operations"), TEXT("Lists every operation with its description."), [](const FJsonObject&) -> FBABResult
	{
		return FBABResult::Ok(FBABDispatcher::Get().DescribeOperations());
	});

	D.Register(TEXT("bp_list_assets"), TEXT("Lists Blueprint assets under a content path (default /Game)."), [](const FJsonObject& Params) -> FBABResult
	{
		FString Path = GetString(Params, TEXT("path"), TEXT("/Game"));
		if (!Path.StartsWith(TEXT("/"))) Path = TEXT("/Game/") + Path;
		Path.RemoveFromEnd(TEXT("/"));
		const bool bRecursive = GetBool(Params, TEXT("recursive"), true);
		const int32 Limit = FMath::Max(1, GetInt(Params, TEXT("limit"), 200));
		const FString NameFilter = GetString(Params, TEXT("name_filter"));
		const FString ParentFilter = GetString(Params, TEXT("parent_class"));

		EnsureAssetRegistryScanned(Path);
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Path));
		Filter.bRecursivePaths = bRecursive;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetObjectPathString() < B.GetObjectPathString(); });

		TArray<TSharedPtr<FJsonValue>> List;
		int32 Total = 0;
		for (const FAssetData& Asset : Assets)
		{
			if (!NameFilter.IsEmpty() && !Asset.AssetName.ToString().Contains(NameFilter)) continue;
			FString ParentClass;
			FString ParentTag;
			if (Asset.GetTagValue(FName(TEXT("ParentClass")), ParentTag)) ParentClass = ShortClassFromTag(ParentTag);
			if (!ParentFilter.IsEmpty() && !ParentClass.Equals(ParentFilter, ESearchCase::IgnoreCase)) continue;
			Total++;
			if (List.Num() >= Limit) continue;
			TSharedRef<FJsonObject> Entry = NewObject();
			Entry->SetStringField(TEXT("path"), Asset.PackageName.ToString());
			Entry->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
			Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			Entry->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
			if (!ParentClass.IsEmpty()) Entry->SetStringField(TEXT("parent_class"), ParentClass);
			FString TypeTag;
			if (Asset.GetTagValue(FName(TEXT("BlueprintType")), TypeTag))
			{
				TypeTag.RemoveFromStart(TEXT("BPTYPE_"));
				Entry->SetStringField(TEXT("blueprint_type"), TypeTag);
			}
			List.Add(ObjectValue(Entry));
		}
		FBABResult Result;
		Result.Data->SetStringField(TEXT("path"), Path);
		Result.Data->SetArrayField(TEXT("assets"), List);
		Result.Data->SetNumberField(TEXT("count"), List.Num());
		Result.Data->SetNumberField(TEXT("total"), Total);
		Result.Data->SetBoolField(TEXT("truncated"), Total > List.Num());
		return Result;
	});

	D.Register(TEXT("bp_create_blueprint"), TEXT("Creates a new Blueprint asset (in memory; pass save=true to write it)."), [](const FJsonObject& Params) -> FBABResult
	{
		FString Path = GetString(Params, TEXT("path"), TEXT("/Game"));
		FString Name = GetString(Params, TEXT("name")).TrimStartAndEnd();
		Name.ReplaceInline(TEXT(" "), TEXT("_"));
		if (Name.IsEmpty())
		{
			return FBABResult::Fail(TEXT("Missing \"name\" for the new Blueprint (e.g. BP_Door)."));
		}
		if (!Path.StartsWith(TEXT("/"))) Path = TEXT("/Game/") + Path;
		Path.RemoveFromEnd(TEXT("/"));
		const FString PackageName = Path + TEXT("/") + Name;
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
		{
			return FBABResult::Fail(FString::Printf(TEXT("Invalid asset path %s: %s"), *PackageName, *Reason.ToString()));
		}
		if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
		{
			return FBABResult::Fail(FString::Printf(TEXT("An asset already exists at %s. Choose another name or edit the existing Blueprint."), *PackageName));
		}

		const FString TypeString = GetString(Params, TEXT("blueprint_type"), TEXT("normal")).ToLower();
		EBlueprintType BlueprintType = BPTYPE_Normal;
		UClass* ParentClass = nullptr;
		FString Error;
		if (TypeString == TEXT("function_library") || TypeString == TEXT("functionlibrary"))
		{
			BlueprintType = BPTYPE_FunctionLibrary;
			ParentClass = UBlueprintFunctionLibrary::StaticClass();
		}
		else if (TypeString == TEXT("interface"))
		{
			BlueprintType = BPTYPE_Interface;
			ParentClass = UInterface::StaticClass();
		}
		else
		{
			if (TypeString == TEXT("macro_library") || TypeString == TEXT("macrolibrary")) BlueprintType = BPTYPE_MacroLibrary;
			else if (TypeString == TEXT("const")) BlueprintType = BPTYPE_Const;
			ParentClass = ResolveClass(GetString(Params, TEXT("parent_class"), TEXT("Actor")), Error);
			if (!ParentClass) return FBABResult::Fail(Error);
		}
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			return FBABResult::Fail(FString::Printf(TEXT("Cannot create a Blueprint from %s (not Blueprintable or deprecated)."), *ClassDisplayName(ParentClass)));
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FBABResult::Fail(FString::Printf(TEXT("Could not create package %s."), *PackageName));
		}
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, FName(*Name), BlueprintType, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("BlueprintAgentBridge")));
		if (!BP)
		{
			return FBABResult::Fail(TEXT("CreateBlueprint returned null."));
		}
		FAssetRegistryModule::AssetCreated(BP);
		Package->MarkPackageDirty();
		EnsureAssetRegistryScanned(TEXT("/Game"));

		FBABResult Result;
		Result.Data = BlueprintToJson(BP);
		Result.Data->SetStringField(TEXT("text"), BlueprintToText(BP));
		Result.Data->SetBoolField(TEXT("saved"), false);
		if (GetBool(Params, TEXT("save"), false))
		{
			FBABResult Saved = SaveBlueprintPackage(BP);
			if (!Saved.bOk)
			{
				// The asset exists in memory, but the caller asked for a file: report the failure as an error.
				for (const FString& E : Saved.Errors) Result.Error(FString::Printf(TEXT("Created in memory but not saved: %s"), *E));
			}
			else
			{
				Result.Data->SetBoolField(TEXT("saved"), true);
				Result.Data->SetStringField(TEXT("file"), Saved.Data->GetStringField(TEXT("file")));
			}
		}
		return Result;
	});

	D.Register(TEXT("bp_describe_blueprint"), TEXT("Summary of a Blueprint: parent, graphs, variables, components, functions, events."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		const FString Format = GetString(Params, TEXT("format"), TEXT("text")).ToLower();
		FBABResult Result;
		if (Format == TEXT("json") || Format == TEXT("both"))
		{
			Result.Data = BlueprintToJson(Ctx.Blueprint);
		}
		if (Format == TEXT("text") || Format == TEXT("both"))
		{
			Result.Data->SetStringField(TEXT("text"), BlueprintToText(Ctx.Blueprint));
			Result.Data->SetStringField(TEXT("path"), GetObjectPath(Ctx.Blueprint));
		}
		return Result;
	});

	D.Register(TEXT("bp_compile"), TEXT("Compiles the Blueprint and returns errors and warnings with node ids."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FBABResult Result;
		CompileBlueprintInto(Ctx.Blueprint, *Result.Data);
		if (GetBool(Params, TEXT("save"), false))
		{
			if (Ctx.Blueprint->Status == BS_Error)
			{
				Result.Warn(TEXT("Not saved: the Blueprint has compile errors."));
			}
			else
			{
				FBABResult Saved = SaveBlueprintPackage(Ctx.Blueprint);
				if (Saved.bOk) Result.Data->SetStringField(TEXT("file"), Saved.Data->GetStringField(TEXT("file")));
				else for (const FString& E : Saved.Errors) Result.Warn(E);
				Result.Data->SetBoolField(TEXT("saved"), Saved.bOk);
			}
		}
		return Result;
	});

	D.Register(TEXT("bp_save"), TEXT("Compiles (unless compile=false) and saves the Blueprint asset to disk."), [](const FJsonObject& Params) -> FBABResult
	{
		FOpContext Ctx;
		FBABResult Failure;
		if (!ResolveContext(Params, false, Ctx, Failure)) return Failure;
		FBABResult Result;
		if (GetBool(Params, TEXT("compile"), true))
		{
			TSharedRef<FJsonObject> Compile = NewObject();
			CompileBlueprintInto(Ctx.Blueprint, *Compile);
			Result.Data->SetObjectField(TEXT("compile"), Compile);
			if (Ctx.Blueprint->Status == BS_Error && !GetBool(Params, TEXT("force"), false))
			{
				Result.Error(TEXT("The Blueprint has compile errors and was not saved. Fix them or pass force=true."));
				return Result;
			}
		}
		FBABResult Saved = SaveBlueprintPackage(Ctx.Blueprint);
		if (!Saved.bOk)
		{
			for (const FString& E : Saved.Errors) Result.Error(E);
			return Result;
		}
		for (const auto& Pair : Saved.Data->Values) Result.Data->SetField(Pair.Key, Pair.Value);
		return Result;
	});
}
