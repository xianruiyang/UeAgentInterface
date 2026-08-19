// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

class FJsonObject;
class FJsonValue;
class UObject;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentBlueprintAuthoring
{
	struct UEAGENTINTERFACE_API FAssetApplyRequest
	{
		UeAgentApplyInputContract::EOperation Operation = UeAgentApplyInputContract::EOperation::Update;
		FString AssetPath;
		FString AssetKind;
		FString SourceAssetPath;
		TArray<TSharedPtr<FJsonValue>> Properties;
		TSharedPtr<FJsonObject> ProviderExtension;
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
		FString AssetKind;
		UObject* Asset = nullptr;
		int32 UnitsChecked = 0;
		int32 MutationsApplied = 0;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;
		FMutationCounters Domain;

		bool IsApplied() const { return Status == TEXT("applied"); }
	};

	UEAGENTINTERFACE_API bool RegisterApplyContracts(FString& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeApplyProvider();
	UEAGENTINTERFACE_API bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyAssetWorkspace(
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyAssetInlineWorkspace(
		const TSharedPtr<const FJsonObject>& Document,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId = FString());
}
