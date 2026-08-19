// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

class FJsonObject;
class FJsonValue;

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
}

namespace UeAgentMaterialAuthoring
{
	struct UEAGENTINTERFACE_API FAssetApplyRequest
	{
		UeAgentApplyInputContract::EOperation Operation = UeAgentApplyInputContract::EOperation::Update;
		FString AssetPath;
		FString AssetKind;
		FString SourceAssetPath;
		TArray<TSharedPtr<FJsonValue>> Properties;
		TSharedPtr<FJsonObject> Graph;
		TSharedPtr<FJsonObject> FunctionSettings;
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
		FString AssetKind;
		UObject* Asset = nullptr;
		int32 ExpressionsCreated = 0;
		int32 EdgesCreated = 0;
		int32 RootInputsConnected = 0;
		int32 UnitsChecked = 0;
		int32 MutationsApplied = 0;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		bool IsApplied() const { return Status == TEXT("applied"); }
	};

	UEAGENTINTERFACE_API bool RegisterApplyContracts(FString& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeApplyProvider();
	UEAGENTINTERFACE_API bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyAssetWorkspace(
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetKind,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId = FString());
}
