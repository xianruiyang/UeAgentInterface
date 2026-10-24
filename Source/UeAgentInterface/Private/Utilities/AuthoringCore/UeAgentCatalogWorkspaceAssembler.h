// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

class FJsonObject;

namespace UeAgentCatalogWorkspaceAssembler
{
	struct UEAGENTINTERFACE_API FBuildRequest
	{
		TSharedPtr<FJsonObject> Placement;
		TSharedPtr<FJsonObject> Seed;
		FString Operation;
		TSharedPtr<FJsonObject> Context;
	};

	struct UEAGENTINTERFACE_API FBuildResult
	{
		TSharedPtr<FJsonObject> CanonicalDocument;
		UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
		UeAgentAuthoringWorkspaceRouter::FWorkspace Workspace;
		UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph DependencyGraph;
	};

	struct UEAGENTINTERFACE_API FBuildFailure
	{
		UE::UAI::Contract::FDiagnostic Diagnostic;
		TArray<FString> RequiredContext;
	};

	UEAGENTINTERFACE_API bool BuildVirtualWorkspace(
		const FBuildRequest& Request,
		FBuildResult& OutResult,
		FBuildFailure& OutFailure);
}
