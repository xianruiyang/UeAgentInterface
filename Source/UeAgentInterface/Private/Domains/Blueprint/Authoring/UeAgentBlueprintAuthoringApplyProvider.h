// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

class FJsonObject;
class FJsonValue;
class UObject;

namespace UeAgentAuthoringWorkspaceTransaction
{
	struct FOperationFailure;
}

namespace UeAgentAuthoringOperationApply
{
	class IApplyProvider;
	class IDurablePackageStager;
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
		bool bCompactValues = false;
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
		int32 UnitsChecked = 0;
		int32 MutationsApplied = 0;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;
		FMutationCounters Domain;

		bool IsApplied() const { return Status == TEXT("applied"); }
	};

	UEAGENTINTERFACE_API bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> MakeApplyProvider(bool bCompactValues = false);
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
		const FString& RequestId = FString(),
		TFunction<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)> FinalReadback = {});
}
