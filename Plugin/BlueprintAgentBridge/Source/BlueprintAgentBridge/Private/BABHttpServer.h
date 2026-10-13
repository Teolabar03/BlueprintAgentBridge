// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"

class IHttpRouter;

/**
 * Local HTTP endpoint (loopback only) in front of the dispatcher.
 *   POST /rpc       body: {"op": "...", "params": {...}}  or a batch
 *   GET  /health    liveness + engine info
 *   GET  /ops       operation list
 *   POST /shutdown  asks the serving loop (commandlet) to exit, or stops the server in the editor
 *
 * Every request must name a loopback Host and carry no Origin header (browsers always send one
 * on cross-site requests, which blocks CSRF and DNS rebinding). /rpc and /shutdown also require
 * the header "X-BAB-Token" with the random token written to GetTokenFilePath() at start.
 */
class FBABHttpServer
{
public:
	static FBABHttpServer& Get();

	bool Start(uint32 InPort, FString& OutError);
	void Stop();

	bool IsRunning() const { return bRunning; }
	uint32 GetPort() const { return Port; }
	bool WasShutdownRequested() const { return bShutdownRequested; }
	void ClearShutdownRequest() { bShutdownRequested = false; }
	double GetLastRequestTime() const { return LastRequestTime; }
	int32 GetRequestCount() const { return RequestCount; }

	/** <Project>/Saved/BlueprintAgentBridge/server-<port>.token */
	static FString GetTokenFilePath(uint32 InPort);

private:
	FString Token;
	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;
	uint32 Port = 0;
	bool bRunning = false;
	bool bShutdownRequested = false;
	double StartTime = 0.0;
	double LastRequestTime = 0.0;
	int32 RequestCount = 0;
};
