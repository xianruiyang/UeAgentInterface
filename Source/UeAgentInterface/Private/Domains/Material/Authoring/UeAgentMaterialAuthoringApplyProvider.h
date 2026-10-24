// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

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
		FGraphApplyContext GraphContext;
		TSharedPtr<FJsonObject> FunctionSettings;
		TArray<TSharedPtr<FJsonValue>> InstanceParameters;
		TSharedPtr<FJsonValue> InstanceLayers;
		bool bCompile = true;
		bool bSave = false;
		TFunction<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)> FinalReadback;
		TSharedPtr<UeAgentAuthoringOperationApply::IDurablePackageStager> PackageStager;
	};

	struct UEAGENTINTERFACE_API FAssetApplyResult
	{
		FString Status;
		FString Phase;
		UE::UAI::Contract::FDiagnostic Diagnostic;
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

	UEAGENTINTERFACE_API bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeApplyProvider(const FGraphApplyContext& Context = {});
	UEAGENTINTERFACE_API bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId = FString());
	UEAGENTINTERFACE_API bool ApplyAssetWorkspace(
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetKind,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId = FString());
}
