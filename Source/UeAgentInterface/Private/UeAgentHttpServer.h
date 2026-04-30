// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Delegates/Delegate.h"
#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpRouteHandle.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"

class IHttpRouter;

struct FUeAgentRequestContext
{
	FString RequestId;
	FString Command;
	TSharedPtr<FJsonObject> Params;
};

class FUeAgentHttpServer final
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
	bool ParseBatchCommands(const FUeAgentRequestContext& BatchCtx, TArray<FUeAgentRequestContext>& OutCommands, bool& bOutStopOnError, FString& OutError) const;

public:
	bool ExecuteBatchCommands(const FUeAgentRequestContext& BatchCtx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const;
	bool ExecuteCommandOnGameThread(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const;

	// Command categories (declarations split by domain).
#define UEAGENT_HTTP_SERVER_MEMBER_DECL 1
#include "UeAgentHttpServer_LevelMembers.h"
#include "UeAgentHttpServer_AssetsMembers.h"
#include "UeAgentHttpServer_LandscapeMembers.h"
#include "UeAgentHttpServer_StaticMeshMembers.h"
#include "UeAgentHttpServer_BlueprintMembers.h"
#include "UeAgentHttpServer_AnimBlueprintMembers.h"
#include "UeAgentHttpServer_MontageMembers.h"
#include "UeAgentHttpServer_UMGMembers.h"
#include "UeAgentHttpServer_EnhancedInputMembers.h"
#include "UeAgentHttpServer_MaterialMembers.h"
#include "UeAgentHttpServer_SequenceMembers.h"
#include "UeAgentHttpServer_AnimationAssetsMembers.h"
#include "UeAgentHttpServer_SkeletalMeshMembers.h"
#include "UeAgentHttpServer_IKAssetsMembers.h"
#include "UeAgentHttpServer_ControlRigMembers.h"
#include "UeAgentHttpServer_DeformerMembers.h"
#include "UeAgentHttpServer_NiagaraMembers.h"
#include "UeAgentHttpServer_ModelingMembers.h"
#include "UeAgentHttpServer_AIMembers.h"
#undef UEAGENT_HTTP_SERVER_MEMBER_DECL

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

	static class UWorld* GetEditorWorld(FString& OutError);
	static class AActor* FindActorByNameOrLabel(UWorld* World, const FString& NameOrLabel);

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
	FDelegateHandle RequestLogPreprocessorHandle;
	bool bRunning = false;

	mutable FCriticalSection TransactionMutex;
	int32 ActiveTransactionIndex = INDEX_NONE;
};
