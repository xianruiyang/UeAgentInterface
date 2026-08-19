#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::HjsonImport
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentCallerContext;
	using UE::UAI::HjsonWorkspace::FAuthoringModelRef;
	using UE::UAI::HjsonWorkspace::FResolvedSemanticWorkspaceRef;
	using UE::UAI::HjsonWorkspace::FStructuralEdit;
	using UE::UAI::PlanningPreflight::FChangeSetDraft;
	using UE::UAI::PlanningPreflight::FPlanningRequest;
	using UE::UAI::PlanningPreflight::FPlanningResult;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotRef;

	struct UEAGENTINTERFACE_API FSemanticWorkspaceImportError
	{
		FString Code;
		FString Detail;

		bool IsPresent() const;
		void Reset();
	};

	class FPreparedAuthoringImport;
	using FPreparedAuthoringImportPtr =
		TSharedPtr<const FPreparedAuthoringImport>;

	class UEAGENTINTERFACE_API FPreparedAuthoringImport final
	{
	public:
		FPreparedAuthoringImport(
			FResolvedSemanticWorkspaceRef InWorkspace,
			FChangeSetDraft InChangeSetDraft,
			FPlanningRequest InPlanningRequest,
			FPlanningResult InPreflightResult);

		const FResolvedSemanticWorkspaceRef& GetWorkspace() const;
		const FChangeSetDraft& GetChangeSetDraft() const;
		const FPlanningRequest& GetPlanningRequest() const;
		const FPlanningResult& GetPreflightResult() const;
		const TArray<FStructuralEdit>& GetStructuralEdits() const;
		bool IsValid() const;

	private:
		FResolvedSemanticWorkspaceRef Workspace;
		FChangeSetDraft ChangeSetDraft;
		FPlanningRequest PlanningRequest;
		FPlanningResult PreflightResult;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API bool TryBuildSemanticChangeSetDraft(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FSemanticWorkspaceImportError& OutError);

	UEAGENTINTERFACE_API bool TryPrepareResolvedAuthoringImport(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		const FChangeSetDraft& ChangeSetDraft,
		const FUeAgentCallerContext& CallerContext,
		bool bDryRun,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FUeAgentResourceTracker& Resources,
		FPreparedAuthoringImportPtr& OutPrepared,
		FSemanticWorkspaceImportError& OutError);

	UEAGENTINTERFACE_API bool TryPrepareSemanticWorkspaceImport(
		const HjsonWorkspace::FWorkspaceSource& Source,
		HjsonWorkspace::ESemanticWorkspaceMode ExpectedMode,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		const FUeAgentCallerContext& CallerContext,
		bool bDryRun,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FUeAgentResourceTracker& Resources,
		FPreparedAuthoringImportPtr& OutPrepared,
		FSemanticWorkspaceImportError& OutError);
}
