// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDescriptors.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDocuments.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

class FJsonObject;
class FJsonValue;
class UObject;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentRiggingAuthoring
{
	struct UEAGENTINTERFACE_API FAssetApplyRequest
	{
		EAssetKind Kind = EAssetKind::ControlRig;
		UeAgentApplyInputContract::EOperation Operation =
			UeAgentApplyInputContract::EOperation::Update;
		FString AssetPath;
		FString SourceAssetPath;
		TArray<TSharedPtr<FJsonValue>> Properties;
		TSharedPtr<FJsonObject> Document;
		bool bCompile = true;
		bool bSave = false;
	};

	struct UEAGENTINTERFACE_API FAssetApplyResult
	{
		FString Status;
		FString Phase;
		FString ErrorCode;
		FString ErrorDetail;
		FString ResolvedOperation;
		FString AssetPath;
		FString ObjectPath;
		UObject* Asset = nullptr;
		FMutationStats Stats;
		int32 UnitsChecked = 0;
		int32 MutationsApplied = 0;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		bool IsApplied() const { return Status == TEXT("applied"); }
	};

	struct UEAGENTINTERFACE_API FWorkspaceInspectResult
	{
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		FString AssetPath;
		FString RequestedOperation;
		FMutationStats PlannedStats;
		int32 UnitsChecked = 0;
		bool bAssetExists = false;
		bool bWouldCreate = false;
		bool bNoOp = false;

		bool IsReady() const { return Status == TEXT("ready"); }
	};

	UEAGENTINTERFACE_API bool RegisterApplyContracts(FString& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeApplyProvider();
	UEAGENTINTERFACE_API bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyAssetWorkspace(
		const FProfile& Profile,
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool InspectAssetWorkspace(
		const FProfile& Profile,
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetPath,
		FWorkspaceInspectResult& OutResult);
}
