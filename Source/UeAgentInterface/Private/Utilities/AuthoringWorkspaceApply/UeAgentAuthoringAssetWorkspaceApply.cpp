// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"

namespace UeAgentAuthoringAssetWorkspaceApply
{
	bool Execute(
		const UeAgentApplyWorkspaceContract::FContract& WorkspaceContract,
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
		const UeAgentAuthoringOperationApply::FProviderRegistry& ProviderRegistry,
		const FString& RequestId,
		const bool bSaveWhenCallerControlled,
		const UeAgentAuthoringOperationApply::FBackendOptions& BackendOptions,
		UeAgentAuthoringOperationApply::FApplyWorkspaceResult& OutResult,
		FFailure& OutFailure)
	{
		using namespace UeAgentAuthoringOperationApply;

		OutResult = FApplyWorkspaceResult();
		OutFailure = FFailure();
		UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
		UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
		if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
			WorkspaceContract,
			Workspace,
			UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
			Graph,
			GraphFailure))
		{
			OutFailure.Phase = TEXT("preflight");
			OutFailure.Diagnostic = GraphFailure.Diagnostic;
			return false;
		}

		UeAgentAuthoringWorkspaceTransaction::FExecutionOptions ExecutionOptions;
		FOperationFailure RequestFailure;
		if (!BuildApplyEntryExecutionOptions(
			RequestId,
			ExecutionOptions,
			RequestFailure))
		{
			OutFailure.Phase = TEXT("preflight");
			OutFailure.Diagnostic = RequestFailure.Diagnostic;
			return false;
		}
		ExecutionOptions.bSaveWhenCallerControlled = bSaveWhenCallerControlled;

		if (ApplyWorkspace(
			WorkspaceContract,
			Workspace,
			Graph,
			UeAgentApplyInputContract::FRegistry::Get(),
			ProviderRegistry,
			ExecutionOptions,
			BackendOptions,
			OutResult))
		{
			return true;
		}

		FApplyFailureProjection Failure;
		if (TryProjectApplyFailure(OutResult, Failure))
		{
			OutFailure.Phase = Failure.Phase;
			OutFailure.Diagnostic = Failure.Diagnostic;
		}
		else
		{
			OutFailure.Phase = TEXT("execution");
			OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(
                TEXT("asset_workspace_apply_failed"), TEXT("The asset workspace failed without reporting a diagnostic."));
		}
		return false;
	}
}
