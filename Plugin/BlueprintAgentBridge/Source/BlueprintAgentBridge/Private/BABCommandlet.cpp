// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABCommandlet.h"
#include "BABDispatcher.h"
#include "BABHttpServer.h"
#include "BABContext.h"
#include "BABTypes.h"
#include "BlueprintAgentBridgeModule.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CoreMisc.h"
#include "Containers/Ticker.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "TickableEditorObject.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"

UBlueprintAgentBridgeCommandlet::UBlueprintAgentBridgeCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
}

int32 UBlueprintAgentBridgeCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Values;
	ParseCommandLine(*Params, Tokens, Switches, Values);

	BABRegisterAllOperations();

	// A commandlet initializes the editor engine without its transaction buffer (UEditorEngine::Init
	// creates it, commandlets only call InitEditor). Create it so failed edits are undone and the
	// serving loop offers bp_undo / bp_redo.
	if (GEditor && !GEditor->Trans)
	{
		GEditor->Trans = GEditor->CreateTrans();
		UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Created the editor transaction buffer for this headless session."));
	}
	BAB::EnsureAssetRegistryScanned(TEXT("/Game"));

	const FString* RequestFile = Values.Find(TEXT("request"));
	const bool bServe = Switches.Contains(TEXT("serve")) || Switches.Contains(TEXT("server"));
	if (!RequestFile && !bServe)
	{
		UE_LOG(LogBlueprintAgentBridge, Error, TEXT("Usage: -run=BlueprintAgentBridge -request=<file.json> [-response=<file.json>]  |  -serve [-port=8765] [-idle=<seconds>] [-parentpid=<pid>]"));
		return 1;
	}

	int32 ExitCode = 0;
	if (RequestFile)
	{
		FString RequestText;
		if (!FFileHelper::LoadFileToString(RequestText, **RequestFile))
		{
			UE_LOG(LogBlueprintAgentBridge, Error, TEXT("Cannot read request file %s"), **RequestFile);
			return 1;
		}
		const FString ResponseText = FBABDispatcher::Get().HandleRequestText(RequestText, /*bPretty*/ true);
		FString ResponseFile = Values.FindRef(TEXT("response"));
		if (ResponseFile.IsEmpty())
		{
			ResponseFile = *RequestFile + TEXT(".response.json");
		}
		if (!FFileHelper::SaveStringToFile(ResponseText, *ResponseFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogBlueprintAgentBridge, Error, TEXT("Cannot write response file %s"), *ResponseFile);
			return 1;
		}
		UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Response written to %s"), *ResponseFile);
		FString Error;
		TSharedPtr<FJsonObject> Response = BAB::JsonFromString(ResponseText, Error);
		bool bOk = false;
		if (Response.IsValid())
		{
			Response->TryGetBoolField(TEXT("ok"), bOk);
		}
		ExitCode = bOk ? 0 : 2;
	}

	if (bServe)
	{
		int32 Port = 8765;
		if (const FString* PortValue = Values.Find(TEXT("port")))
		{
			Port = FCString::Atoi(**PortValue);
		}
		double IdleTimeout = 0.0;
		if (const FString* IdleValue = Values.Find(TEXT("idle")))
		{
			IdleTimeout = FCString::Atod(**IdleValue);
		}
		// The process that launched us (the MCP server). When it dies without stopping us,
		// for example because it was killed, we exit instead of holding the project open.
		// The handle is opened once and kept, so the pid cannot be reused by another process while we wait.
		uint32 ParentPid = 0;
		FProcHandle ParentHandle;
		if (const FString* ParentValue = Values.Find(TEXT("parentpid")))
		{
			ParentPid = (uint32)FCString::Atoi64(**ParentValue);
			ParentHandle = ParentPid != 0 ? FPlatformProcess::OpenProcess(ParentPid) : FProcHandle();
			if (!ParentHandle.IsValid())
			{
				// Opening can fail for a process with higher rights: fall back to polling the pid.
				if (ParentPid == 0 || !FPlatformProcess::IsApplicationRunning(ParentPid))
				{
					UE_LOG(LogBlueprintAgentBridge, Warning, TEXT("-parentpid=%s: no such process; the server will not follow its parent."), **ParentValue);
					ParentPid = 0;
				}
			}
		}
		FString Error;
		if (!FBABHttpServer::Get().Start((uint32)Port, Error))
		{
			UE_LOG(LogBlueprintAgentBridge, Error, TEXT("%s"), *Error);
			return 3;
		}
		UE_LOG(LogBlueprintAgentBridge, Display, TEXT("BAB_SERVER_READY port=%d pid=%u"), Port, FPlatformProcess::GetCurrentProcessId());

		const double StartTime = FPlatformTime::Seconds();
		double LastTick = StartTime;
		double LastParentCheck = StartTime;
		while (!IsEngineExitRequested() && !FBABHttpServer::Get().WasShutdownRequested())
		{
			const double Now = FPlatformTime::Seconds();
			const float DeltaTime = (float)(Now - LastTick);
			LastTick = Now;

			FTSTicker::GetCoreTicker().Tick(DeltaTime);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FTickableEditorObject::TickObjects(DeltaTime);

			if (IdleTimeout > 0.0 && (Now - FMath::Max(FBABHttpServer::Get().GetLastRequestTime(), StartTime)) > IdleTimeout)
			{
				UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Idle for %.0f seconds, shutting down."), IdleTimeout);
				break;
			}
			if (ParentPid != 0 && Now - LastParentCheck > 2.0)
			{
				LastParentCheck = Now;
				const bool bParentAlive = ParentHandle.IsValid() ? FPlatformProcess::IsProcRunning(ParentHandle) : FPlatformProcess::IsApplicationRunning(ParentPid);
				if (!bParentAlive)
				{
					UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Parent process %u is gone, shutting down."), ParentPid);
					break;
				}
			}
			FPlatformProcess::Sleep(0.004f);
		}
		if (ParentHandle.IsValid())
		{
			FPlatformProcess::CloseProc(ParentHandle);
		}
		FBABHttpServer::Get().Stop();
		UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Blueprint Agent Bridge server loop ended after %d request(s)."), FBABHttpServer::Get().GetRequestCount());
	}
	return ExitCode;
}
