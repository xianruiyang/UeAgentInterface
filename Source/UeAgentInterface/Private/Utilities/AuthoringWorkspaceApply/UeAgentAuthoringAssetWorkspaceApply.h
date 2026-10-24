// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentAuthoringAssetWorkspaceApply
{
	struct UEAGENTINTERFACE_API FFailure
	{
		FString Phase;
		UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;

		bool IsSet() const { return !Diagnostic.IsEmpty(); }
	};

	/**
	 * Runs the shared single-asset workspace graph, request and transaction
	 * lifecycle. Domain providers retain mutation preparation and result fields.
	 */
	UEAGENTINTERFACE_API bool Execute(
		const UeAgentApplyWorkspaceContract::FContract& WorkspaceContract,
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
		const UeAgentAuthoringOperationApply::FProviderRegistry& ProviderRegistry,
		const FString& RequestId,
		bool bSaveWhenCallerControlled,
		const UeAgentAuthoringOperationApply::FBackendOptions& BackendOptions,
		UeAgentAuthoringOperationApply::FApplyWorkspaceResult& OutResult,
		FFailure& OutFailure);
}
