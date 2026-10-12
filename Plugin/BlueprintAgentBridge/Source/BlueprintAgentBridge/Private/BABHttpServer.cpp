// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABHttpServer.h"
#include "BABDispatcher.h"
#include "BABTypes.h"
#include "BlueprintAgentBridgeModule.h"

#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpPath.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpServerConstants.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/CoreMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"

FBABHttpServer& FBABHttpServer::Get()
{
	static FBABHttpServer Instance;
	return Instance;
}

static TUniquePtr<FHttpServerResponse> JsonResponse(const FString& Json, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
{
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Json, TEXT("application/json"));
	Response->Code = Code;
	return Response;
}

static FString BodyToString(const FHttpServerRequest& Request)
{
	if (Request.Body.Num() == 0)
	{
		return FString();
	}
	FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	return FString(Converter.Length(), Converter.Get());
}

bool FBABHttpServer::Start(uint32 InPort, FString& OutError)
{
	if (bRunning)
	{
		if (Port == InPort)
		{
			return true;
		}
		Stop();
	}

	// Never expose the editor on the network: listen on the loopback interface only.
	if (GConfig)
	{
		GConfig->SetString(TEXT("HTTPServer.Listeners"), TEXT("DefaultBindAddress"), TEXT("localhost"), GEngineIni);
	}

	FHttpServerModule& Module = FHttpServerModule::Get();
	Router = Module.GetHttpRouter(InPort, /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not bind HTTP port %u (already in use?)."), InPort);
		return false;
	}

	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/rpc")), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler::CreateLambda(
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			LastRequestTime = FPlatformTime::Seconds();
			RequestCount++;
			const FString Body = BodyToString(Request);
			const FString Json = FBABDispatcher::Get().HandleRequestText(Body, false);
			OnComplete(JsonResponse(Json));
			return true;
		})));

	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/health")), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateLambda(
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			TSharedRef<FJsonObject> Json = BAB::NewObject();
			Json->SetBoolField(TEXT("ok"), true);
			Json->SetStringField(TEXT("service"), TEXT("BlueprintAgentBridge"));
			Json->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
			Json->SetStringField(TEXT("project"), FApp::GetProjectName());
			Json->SetStringField(TEXT("mode"), IsRunningCommandlet() ? TEXT("commandlet") : TEXT("editor"));
			Json->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
			Json->SetNumberField(TEXT("uptime_s"), FMath::RoundToDouble(FPlatformTime::Seconds() - StartTime));
			Json->SetNumberField(TEXT("requests"), RequestCount);
			OnComplete(JsonResponse(BAB::JsonToString(Json)));
			return true;
		})));

	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/ops")), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateLambda(
		[](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			OnComplete(JsonResponse(BAB::JsonToString(FBABDispatcher::Get().DescribeOperations())));
			return true;
		})));

	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/shutdown")), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler::CreateLambda(
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			bShutdownRequested = true;
			TSharedRef<FJsonObject> Json = BAB::NewObject();
			Json->SetBoolField(TEXT("ok"), true);
			Json->SetBoolField(TEXT("stopping"), true);
			Json->SetStringField(TEXT("mode"), IsRunningCommandlet() ? TEXT("commandlet") : TEXT("editor"));
			OnComplete(JsonResponse(BAB::JsonToString(Json)));
			if (!IsRunningCommandlet())
			{
				// In the editor only the server stops; the editor keeps running.
				Stop();
			}
			return true;
		})));

	Module.StartAllListeners();
	Port = InPort;
	bRunning = true;
	bShutdownRequested = false;
	StartTime = FPlatformTime::Seconds();
	LastRequestTime = StartTime;
	UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Blueprint Agent Bridge listening on http://127.0.0.1:%u (POST /rpc, GET /health, GET /ops, POST /shutdown)"), InPort);
	return true;
}

void FBABHttpServer::Stop()
{
	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& Handle : Routes)
		{
			if (Handle.IsValid())
			{
				Router->UnbindRoute(Handle);
			}
		}
	}
	Routes.Empty();
	Router.Reset();
	if (bRunning)
	{
		UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Blueprint Agent Bridge server stopped (port %u)."), Port);
	}
	bRunning = false;
}
