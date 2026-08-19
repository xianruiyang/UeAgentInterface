// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Server/UeAgentHttpServerTypes.h"

#include "Delegates/Delegate.h"
#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpRouteHandle.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"

class FProperty;
class IHttpRouter;

class UEAGENTINTERFACE_API FUeAgentHttpServer final
{
public:
	FUeAgentHttpServer();
	~FUeAgentHttpServer();

	bool Start();
	void Stop();
	bool IsRunning() const;

	int32 GetPort() const;
	FString GetListenAddress() const;
	FString GetAuthTokenMasked() const;
	FString GetAuthToken() const;
	FString GetServerInstanceId() const;

	static bool ParseResponseOptions(const TSharedPtr<FJsonObject>& Root, FUeAgentResponseOptions& OutOptions, FString& OutError);
	static FString FormatResponseJson(const TSharedRef<FJsonObject>& Root, const FUeAgentResponseOptions& Options = FUeAgentResponseOptions());

private:
	bool BindRoutes();
	void UnbindRoutes();

	bool HandlePing(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleExec(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	bool IsAuthorized(const FHttpServerRequest& Request, FString& OutReason) const;

	static TUniquePtr<FHttpServerResponse> MakeJsonResponse(int32 Code, const FString& JsonText);
	static TUniquePtr<FHttpServerResponse> MakeErrorResponse(int32 Code, const FString& ErrorCode, const FString& ErrorMessage);

	bool ParseExecBody(const FHttpServerRequest& Request, FUeAgentRequestContext& OutCtx, FString& OutError) const;
public:
	bool ExecuteCommandOnGameThread(const FUeAgentRequestContext& Ctx, FUeAgentCommandOutput& OutOutput, FString& OutError) const;
	bool ExecuteCommandOnGameThread(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const;

	// Command member declarations are generated from domain-owned member headers.
#define UEAGENT_HTTP_SERVER_COMMAND_MEMBER_DECL 1
#include "Server/Generated/UeAgentHttpServerCommandMembers.h"
#undef UEAGENT_HTTP_SERVER_COMMAND_MEMBER_DECL

private:
	static bool JsonTryGetObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj);
	static bool JsonTryGetString(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue);
	static bool JsonTryGetNumber(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue);
	static bool JsonTryGetBool(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue);
	static bool JsonTryGetVector(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue);
	static bool JsonTryGetRotator(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue);

	static TSharedPtr<FJsonObject> VecToJson(const FVector& V);
	static TSharedPtr<FJsonObject> RotToJson(const FRotator& R);
	static void SetPropertyImportResultFields(
		const TSharedPtr<FJsonObject>& Obj,
		FProperty* Property,
		const FString& RequestedValueText,
		const FString& AppliedValueText,
		const FString& ImportStatus,
		const FString& ImportError = FString());
	static TSharedPtr<FJsonObject> MakePropertyImportResultJson(
		const FString& PropertyName,
		FProperty* Property,
		const FString& RequestedValueText,
		const FString& AppliedValueText,
		const FString& ImportStatus,
		const FString& ImportError = FString());

	// Shared editor-world resolution used by command-domain adapters.
public:
	static class UWorld* GetEditorWorld(FString& OutError);
	static class AActor* FindActorByNameOrLabel(UWorld* World, const FString& NameOrLabel);

private:
	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
	FDelegateHandle RequestLogPreprocessorHandle;
	bool bRunning;
	mutable bool bPythonConsoleCommandExecuted;
	FString ServerInstanceId;

};
