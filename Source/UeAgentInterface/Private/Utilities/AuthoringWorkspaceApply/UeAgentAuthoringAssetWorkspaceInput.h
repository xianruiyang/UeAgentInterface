// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"

namespace UeAgentAuthoringAssetWorkspaceInput
{
	struct UEAGENTINTERFACE_API FErrorCodes
	{
		FString ContractMissing;
		FString EntryInvalid;
		FString AssetKindConflict;
		FString AssetPathConflict;
	};

	struct UEAGENTINTERFACE_API FRequest
	{
		FString WorkspaceContractId;
		FString UnitId = TEXT("asset");
		UeAgentAuthoringWorkspaceLoader::FSourceRequest Source;
		FString ExpectedAssetPath;
		FString ExpectedAssetKind;
		FErrorCodes Errors;
	};

	struct UEAGENTINTERFACE_API FTargetRequest
	{
		FString UnitId = TEXT("asset");
		FString ExpectedAssetPath;
		FString ExpectedAssetKind;
		FErrorCodes Errors;
	};

	struct UEAGENTINTERFACE_API FTargetResult
	{
		FString Phase;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		UeAgentApplyInputContract::FContract InputContract;
		UeAgentApplyInputContract::FOperationInput Input;
		FString CanonicalUnitKey;
		FString AssetPath;
		FString SourceAssetPath;
		FString AssetKind;
	};

	struct UEAGENTINTERFACE_API FResult
	{
		FString Phase;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
		UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
		UeAgentApplyInputContract::FContract InputContract;
		UeAgentApplyInputContract::FOperationInput Input;
		FString CanonicalUnitKey;
		FString AssetPath;
		FString SourceAssetPath;
		FString AssetKind;

		bool IsResolved() const { return Phase == TEXT("resolved") && Diagnostic.IsEmpty(); }
	};

	/**
	 * Resolves the shared asset workspace envelope and target assertions.
	 * Domain mutation, compile, save and readback remain provider-owned.
	 */
	UEAGENTINTERFACE_API bool Resolve(
		const FRequest& Request,
		FResult& OutResult);

	UEAGENTINTERFACE_API bool ResolveTarget(
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
		const FTargetRequest& Request,
		FTargetResult& OutResult);
}
