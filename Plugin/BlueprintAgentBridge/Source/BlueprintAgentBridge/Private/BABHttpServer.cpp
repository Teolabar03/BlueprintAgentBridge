// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABHttpServer.h"
#include "BABDispatcher.h"
#include "BABTypes.h"
#include "BABPlatform.h"
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
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Containers/Ticker.h"

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

static const TArray<FString>* HeaderValues(const FHttpServerRequest& Request, const TCHAR* Name)
{
	// The HTTP server stores header names in lower case.
	return Request.Headers.Find(FString(Name).ToLower());
}

static FString FirstHeader(const FHttpServerRequest& Request, const TCHAR* Name)
{
	const TArray<FString>* Values = HeaderValues(Request, Name);
	return Values && Values->Num() > 0 ? (*Values)[0].TrimStartAndEnd() : FString();
}

/** Exactly "localhost", "127.0.0.1" or "[::1]", optionally followed by ":<digits>". */
static bool IsLoopbackHost(const FString& HostHeader)
{
	FString Host = HostHeader;
	FString PortPart;
	if (Host.StartsWith(TEXT("[")))
	{
		int32 Close = INDEX_NONE;
		if (!Host.FindChar(TEXT(']'), Close))
		{
			return false;
		}
		PortPart = Host.Mid(Close + 1);
		Host = Host.Left(Close + 1);
	}
	else
	{
		int32 Colon = INDEX_NONE;
		if (Host.FindChar(TEXT(':'), Colon))
		{
			PortPart = Host.Mid(Colon);
			Host = Host.Left(Colon);
		}
	}
	if (!PortPart.IsEmpty())
	{
		const FString Digits = PortPart.Mid(1);
		if (!PortPart.StartsWith(TEXT(":")) || Digits.IsEmpty() || Digits.Len() > 5)
		{
			return false;
		}
		for (const TCHAR C : Digits)
		{
			if (!FChar::IsDigit(C))
			{
				return false;
			}
		}
	}
	return Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) || Host == TEXT("127.0.0.1") || Host == TEXT("[::1]");
}

static FString NewToken()
{
	// Two random GUIDs: 256 bits that a web page cannot guess.
	return FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

static TUniquePtr<FHttpServerResponse> ErrorResponse(EHttpServerResponseCodes Code, const FString& Message)
{
	return JsonResponse(BAB::JsonToString(FBABResult::Fail(Message).ToJson()), Code);
}

/**
 * Rejects requests a web page could send: any Origin header (browsers add it to cross-site
 * requests), a Host that is not loopback (DNS rebinding) and, when required, a missing token.
 */
static TUniquePtr<FHttpServerResponse> CheckRequest(const FHttpServerRequest& Request, const FString& Token, bool bRequireToken)
{
	if (!FirstHeader(Request, TEXT("origin")).IsEmpty())
	{
		return ErrorResponse(EHttpServerResponseCodes::Forbidden, TEXT("Requests from web pages (Origin header) are not accepted."));
	}
	const TArray<FString>* Hosts = HeaderValues(Request, TEXT("host"));
	if (!Hosts || Hosts->Num() != 1 || !IsLoopbackHost((*Hosts)[0].TrimStartAndEnd()))
	{
		return ErrorResponse(EHttpServerResponseCodes::Forbidden, TEXT("The Host header must be 127.0.0.1 or localhost."));
	}
	if (bRequireToken && (Token.IsEmpty() || !FirstHeader(Request, TEXT("x-bab-token")).Equals(Token, ESearchCase::CaseSensitive)))
	{
		return ErrorResponse(EHttpServerResponseCodes::Denied,
			TEXT("Missing or wrong X-BAB-Token header. The token is in <Project>/Saved/BlueprintAgentBridge/server-<port>.token (rewritten each time the server starts)."));
	}
	return nullptr;
}

FString FBABHttpServer::GetTokenFilePath(uint32 InPort)
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BlueprintAgentBridge") / FString::Printf(TEXT("server-%u.token"), InPort));
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
	// GetHttpRouter only binds (and honours bFailOnBindFailure) once listeners are enabled. In a
	// commandlet they are not yet, so without this a busy port went unnoticed: the bind failed
	// later inside StartAllListeners and the server reported itself as listening.
	Module.StartAllListeners();
	Router = Module.GetHttpRouter(InPort, /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		// Leave the token file alone: it may belong to another server that owns this port.
		OutError = FString::Printf(TEXT("Could not bind HTTP port %u (already in use?)."), InPort);
		return false;
	}

	// Written only once the port is ours (GetHttpRouter binds immediately when asked to fail on
	// bind failure). The file sits in the project's Saved folder and only the current user can
	// read it, so other accounts on the machine cannot use the token; the MCP server sends it back.
	Token = NewToken();
	const FString TokenFile = GetTokenFilePath(InPort);
	FString WriteError;
	if (!BAB::WritePrivateFile(TokenFile, Token, WriteError))
	{
		Token.Reset();
		Router.Reset();
		OutError = FString::Printf(TEXT("Could not write the access token: %s"), *WriteError);
		return false;
	}

	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/rpc")), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler::CreateLambda(
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			if (TUniquePtr<FHttpServerResponse> Rejected = CheckRequest(Request, Token, true))
			{
				OnComplete(MoveTemp(Rejected));
				return true;
			}
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
			if (TUniquePtr<FHttpServerResponse> Rejected = CheckRequest(Request, Token, false))
			{
				OnComplete(MoveTemp(Rejected));
				return true;
			}
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
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			if (TUniquePtr<FHttpServerResponse> Rejected = CheckRequest(Request, Token, false))
			{
				OnComplete(MoveTemp(Rejected));
				return true;
			}
			OnComplete(JsonResponse(BAB::JsonToString(FBABDispatcher::Get().DescribeOperations())));
			return true;
		})));

	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/shutdown")), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler::CreateLambda(
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			if (TUniquePtr<FHttpServerResponse> Rejected = CheckRequest(Request, Token, true))
			{
				OnComplete(MoveTemp(Rejected));
				return true;
			}
			bShutdownRequested = true;
			TSharedRef<FJsonObject> Json = BAB::NewObject();
			Json->SetBoolField(TEXT("ok"), true);
			Json->SetBoolField(TEXT("stopping"), true);
			Json->SetStringField(TEXT("mode"), IsRunningCommandlet() ? TEXT("commandlet") : TEXT("editor"));
			OnComplete(JsonResponse(BAB::JsonToString(Json)));
			if (!IsRunningCommandlet())
			{
				// In the editor only the server stops; the editor keeps running. Unbind the routes
				// on the next tick, not from inside the route handler that is running now.
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
				{
					Stop();
					return false;
				}));
			}
			return true;
		})));

	Module.StartAllListeners();
	Port = InPort;
	bRunning = true;
	bShutdownRequested = false;
	StartTime = FPlatformTime::Seconds();
	LastRequestTime = StartTime;
	UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Blueprint Agent Bridge listening on http://127.0.0.1:%u (POST /rpc, GET /health, GET /ops, POST /shutdown); token in %s"), InPort, *TokenFile);
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
		IFileManager::Get().Delete(*GetTokenFilePath(Port), false, false, true);
		Token.Reset();
		UE_LOG(LogBlueprintAgentBridge, Display, TEXT("Blueprint Agent Bridge server stopped (port %u)."), Port);
	}
	bRunning = false;
}
