// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

class FJsonObject;

namespace UeAgentAuthoringWorkspaceDependencyGraph
{
	enum class EDependencySource : uint8
	{
		Contract,
		Reference,
	};

	enum class EOrderConstraint : uint8
	{
		TargetBeforeSource,
		SourceBeforeTarget,
	};

	struct UEAGENTINTERFACE_API FDependencyDeclaration
	{
		FString SourceUnitKey;
		FString TargetReference;
		FString SourceJsonPointer = TEXT("/");
		EOrderConstraint Order = EOrderConstraint::TargetBeforeSource;
		bool bRequired = true;
	};

	struct UEAGENTINTERFACE_API FDependencyInputs
	{
		TArray<FDependencyDeclaration> ContractDependencies;
		TArray<FDependencyDeclaration> ReferenceDependencies;
	};

	struct UEAGENTINTERFACE_API FResolvedEdge
	{
		int32 BeforeUnitIndex = INDEX_NONE;
		int32 AfterUnitIndex = INDEX_NONE;
		int32 DeclarationUnitIndex = INDEX_NONE;
		EDependencySource Source = EDependencySource::Contract;
		FString SourceJsonPointer;
	};

	struct UEAGENTINTERFACE_API FGraphFailure
	{
		FString ErrorCode;
		FString ErrorDetail;
		UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
	};

	struct UEAGENTINTERFACE_API FDependencyGraph
	{
		FString ContractId;
		TArray<int32> OrderedUnitIndices;
		TArray<FResolvedEdge> Edges;
		TArray<FString> CycleUnitKeys;

		bool IsReadyFor(const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace) const;
		TSharedRef<FJsonObject> ToJsonObject(
			const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace) const;
	};

	UEAGENTINTERFACE_API FString DependencySourceToString(EDependencySource Source);

	UEAGENTINTERFACE_API bool BuildDependencyGraph(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
		const FDependencyInputs& Inputs,
		FDependencyGraph& OutGraph,
		FGraphFailure& OutFailure);
}
