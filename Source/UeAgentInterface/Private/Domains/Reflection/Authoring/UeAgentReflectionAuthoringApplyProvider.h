// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

class FJsonObject;
class FJsonValue;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentReflectionAuthoring
{
	struct FProfile;

	struct FAssetApplyRequest
	{
		FString TypeId;
		UeAgentApplyInputContract::EOperation Operation = UeAgentApplyInputContract::EOperation::Update;
		FString AssetPath;
		FString ClassPath;
		FString SourceAssetPath;
		TArray<TSharedPtr<FJsonValue>> Properties;
		TSharedPtr<FJsonObject> ProviderExtension;
		bool bSave = false;
	};

	struct FAssetApplyResult
	{
		FString Status;
		FString Phase;
		FString ErrorCode;
		FString ErrorDetail;
		FString ProfileId;
		FString AssetPath;
		FString ResolvedOperation;
		int32 UnitsChecked = 0;
		int32 MutationsApplied = 0;
		bool bCreated = false;
		bool bRemoved = false;
		bool bNoOp = false;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct FWorkspaceInspectResult
	{
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		FString ProfileId;
		FString AssetPath;
		FString RequestedOperation;
		int32 UnitsChecked = 0;
		bool bAssetExists = false;
		bool bWouldCreate = false;
		bool bNoOp = false;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool RegisterApplyContracts(FString& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeApplyProvider();
	UEAGENTINTERFACE_API bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyAssetSource(
		const FProfile& Profile,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		TOptional<bool> SaveOverride = TOptional<bool>(),
		const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool InspectAssetSource(
		const FProfile& Profile,
		const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
		const FString& ExpectedAssetPath,
		FWorkspaceInspectResult& OutResult);
}
