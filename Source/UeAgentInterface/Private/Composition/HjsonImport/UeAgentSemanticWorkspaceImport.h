#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/UeAgentGraphReferenceBinding.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Templates/SharedPointer.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class FJsonValue;
namespace UeAgentNativeNormalizationIntent { class FScope; }

namespace UE::UAI::Composition::HjsonImport
{
	struct FScopedCurrentCoverage;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentCallerContext;
	using UE::UAI::HjsonWorkspace::FAuthoringModelRef;
	using UE::UAI::HjsonWorkspace::FAuthoringModelPtr;
	using UE::UAI::HjsonWorkspace::FResolvedSemanticWorkspaceRef;
	using UE::UAI::HjsonWorkspace::FStructuralEdit;
	using UE::UAI::PlanningPreflight::FChangeSetDraft;
	using UE::UAI::PlanningPreflight::FPlanningRequest;
	using UE::UAI::PlanningPreflight::FPlanningResult;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotRef;

	using FSemanticWorkspaceImportError = UE::UAI::Contract::FDiagnostic;

	/** Restore compact edits using the same typed overlay as native property import. */
	UEAGENTINTERFACE_API bool TryRestoreSemanticPropertyValue(
		const HjsonWorkspace::FSemanticUnit& Unit, const HjsonWorkspace::FAuthoringModel& Model,
		const StateSemantic::FUeAgentCanonicalValueRef& Captured,
		const StateSemantic::FUeAgentCanonicalValueRef& Proposed, FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutValue, StateSemantic::FUeAgentStateSemanticError& OutError);

	class FPreparedAuthoringImport;
	using FPreparedAuthoringImportPtr =
		TSharedPtr<const FPreparedAuthoringImport>;

	struct UEAGENTINTERFACE_API FSemanticWorkspaceChangeProjection final
	{
		bool HasChanges() const;
		int32 GetPropertyChangeCount() const;

		TArray<TSharedPtr<FJsonValue>> RootObjectProperties;
		TMap<FString, TSharedPtr<FJsonValue>> PropertyChangesByUnitId;
		TArray<FString> ChangedUnitIds;
		TArray<FStructuralEdit> StructuralEdits;
		TArray<HjsonWorkspace::FDocumentValueSourceBinding> DocumentSourceBindings;
	};

	/** Carry the exact parsed property source through the generated apply descriptor. */
	UEAGENTINTERFACE_API void AttachSemanticPropertySource(
		const FResolvedSemanticWorkspaceRef& Workspace, const FString& TargetKey, FJsonObject& Descriptor,
		const FString& TargetPath = FString());

	/** Verify the original, pre-domain field delta against a fresh native capture.
	 * Unedited native side effects are context, not an expected before-state replay. */
	UEAGENTINTERFACE_API bool TryVerifySemanticWorkspaceValueIntent(
		const FPreparedAuthoringImport& Prepared,
		const UeAgentAuthoringState::FStateSnapshot& NativeAfter,
		HjsonWorkspace::FWorkspaceError& OutError,
		const TArray<HjsonWorkspace::FDocumentValueSourceBinding>& DocumentSources = {},
		const UeAgentNativeNormalizationIntent::FScope* NativeNormalization = nullptr,
		const TArray<UeAgentAuthoringState::FReadRequest>* CurrentReadRequests = nullptr,
		const TArray<HjsonWorkspace::FGraphNodeReferenceBinding>& GraphReferenceBindings = {},
		HjsonWorkspace::FProjectObservedGraphNodeFields ProjectObservedGraphNodeFields = nullptr);

	class UEAGENTINTERFACE_API FPreparedAuthoringImport final
	{
	public:
		FPreparedAuthoringImport(
			FResolvedSemanticWorkspaceRef InWorkspace,
			FChangeSetDraft InChangeSetDraft,
			FPlanningRequest InPlanningRequest,
			FPlanningResult InPreflightResult,
			FAuthoringModelPtr InInputBaseline = nullptr,
			TMap<FString, StateSemantic::FUeAgentCanonicalValueRef> InPropertyBaselines = {});
		// Ordinary native consumers need the validated draft, not a materialized
		// execution Plan. A real Plan consumer requests its view explicitly.
		FPreparedAuthoringImport(FResolvedSemanticWorkspaceRef InWorkspace,
			TSharedRef<const FChangeSetDraft> InChangeSetDraft, FPlanningRequest InPlanningRequest,
			FAuthoringModelPtr InInputBaseline = nullptr,
			TMap<FString, StateSemantic::FUeAgentCanonicalValueRef> InPropertyBaselines = {});

		const FResolvedSemanticWorkspaceRef& GetWorkspace() const;
		const FChangeSetDraft& GetChangeSetDraft() const;
		const FAuthoringModelPtr& GetInputBaseline() const { return InputBaseline; }
		const StateSemantic::FUeAgentCanonicalValueRef* FindPropertyBaseline(const FString& Key) const;
		const PlanningPreflight::FChangeDraft* FindChangeForTargetIdentity(const FString& Identity) const;
		const FPlanningRequest& GetPlanningRequest() const;
		const FPlanningResult& GetPreflightResult() const;
		bool TryGetExecutionPlan(FUeAgentResourceTracker& Resources,
			PlanningPreflight::FPlanningPreflightPlanPtr& OutPlan, Contract::FDiagnostic& OutError) const;
		const TArray<FStructuralEdit>& GetStructuralEdits() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }
		// Select a consumer's changes after the complete request has been validated.
		// Workspace and preflight evidence continue to describe the complete request.
		FPreparedAuthoringImportPtr SelectChanges(
			TFunctionRef<bool(const PlanningPreflight::FChangeDraft&)> Include) const;

	private:
		FResolvedSemanticWorkspaceRef Workspace;
		// Frozen input bindings identify effective edits. Final native readback never recomposes an export.
		FAuthoringModelPtr InputBaseline;
		mutable TMap<FString, StateSemantic::FUeAgentCanonicalValueRef> PropertyBaselines;
		TSharedRef<const FChangeSetDraft> ChangeSetDraft;
		TMap<FString, int32> FirstChangeByTargetIdentity;
		FPlanningRequest PlanningRequest;
		mutable FPlanningResult PreflightResult;
		mutable bool bPreflightAttempted = true;
		bool bLazyPropertyBaselines = false;
		mutable Contract::FDiagnostic ValidationDiagnostic;
		void InitializeValidation(bool bRequireProvidedPlan);
	};

	UEAGENTINTERFACE_API bool TryBuildSemanticChangeSetDraft(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FSemanticWorkspaceImportError& OutError,
		const FAuthoringModelPtr& CurrentModel = nullptr,
		TArray<HjsonWorkspace::FDocumentValueSourceBinding>* OutDocumentSourceBindings = nullptr,
		const TSet<FString>* NativeStructureSemanticIds = nullptr);

	UEAGENTINTERFACE_API bool TryProjectRootObjectPropertyChanges(
		const FPreparedAuthoringImport& Prepared,
		const FString& ExpectedRootOwnerPath,
		TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges,
		FSemanticWorkspaceImportError& OutError);

	/** Pure type/value projection; the caller supplies a snapshot scoped to the selected native owner. */
	UEAGENTINTERFACE_API bool TryProjectReflectedPropertyValues(
		const UeAgentAuthoringState::FStateSnapshot& NativeDefaults,
		const HjsonWorkspace::FHjsonNode& Categories,
		FUeAgentResourceTracker& Resources,
		TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges,
		FSemanticWorkspaceImportError& OutError);

	/** Explicit categories against an unpublished native default template, not an existing target before-state. */
	UEAGENTINTERFACE_API bool TryProjectInitialRootObjectProperties(
		const UeAgentAuthoringState::FStateSnapshot& NativeDefaults,
		const FString& ExpectedRootOwnerPath,
		const HjsonWorkspace::FHjsonNode& Categories,
		FUeAgentResourceTracker& Resources,
		TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges,
		FSemanticWorkspaceImportError& OutError);

	UEAGENTINTERFACE_API bool TryPrepareRootObjectPropertyChanges(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		const FUeAgentCallerContext& CallerContext,
		const FString& ExpectedRootOwnerPath,
		FUeAgentResourceTracker& Resources,
		TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges,
		FSemanticWorkspaceImportError& OutError);

	UEAGENTINTERFACE_API bool TryPrepareSemanticWorkspaceChanges(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		const FUeAgentCallerContext& CallerContext,
		const FString& ExpectedRootOwnerPath,
		FUeAgentResourceTracker& Resources,
		FSemanticWorkspaceChangeProjection& OutProjection,
		FPreparedAuthoringImportPtr& OutPrepared,
		FSemanticWorkspaceImportError& OutError,
		const TSet<FString>* NativeStructureSemanticIds = nullptr);

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
		FSemanticWorkspaceImportError& OutError,
		const FScopedCurrentCoverage* Coverage = nullptr);

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
		FSemanticWorkspaceImportError& OutError,
		const FScopedCurrentCoverage* Coverage = nullptr);
}
