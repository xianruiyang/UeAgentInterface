#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::PlanningPreflight
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentCallerContext;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotRef;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;

	using FPlanningPreflightError = UE::UAI::Contract::FDiagnostic;

	enum class EChangeIntent : uint8
	{
		Create,
		Update,
		Replace,
		Delete,
	};

	UEAGENTINTERFACE_API FString LexToString(EChangeIntent Intent);

	class UEAGENTINTERFACE_API FChangeDraft final
	{
	public:
		FChangeDraft(
			FUeAgentCanonicalIdentity InIdentity,
			EChangeIntent InIntent,
			FUeAgentCanonicalValuePtr InExpectedBefore,
			FUeAgentCanonicalValuePtr InProposedAfter,
			TArray<FString> InRequiredAuthorityScopes,
			FString InImpactEvidenceId,
			TArray<FUeAgentCanonicalIdentity> InImpactTargets);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		EChangeIntent GetIntent() const;
		const FUeAgentCanonicalValuePtr& GetExpectedBefore() const;
		const FUeAgentCanonicalValuePtr& GetProposedAfter() const;
		const TArray<FString>& GetRequiredAuthorityScopes() const;
		const FString& GetImpactEvidenceId() const;
		const TArray<FUeAgentCanonicalIdentity>& GetImpactTargets() const;
		bool IsValid() const;
		const FPlanningPreflightError& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FUeAgentCanonicalIdentity Identity;
		EChangeIntent Intent = EChangeIntent::Update;
		FUeAgentCanonicalValuePtr ExpectedBefore;
		FUeAgentCanonicalValuePtr ProposedAfter;
		TArray<FString> RequiredAuthorityScopes;
		FString ImpactEvidenceId;
		TArray<FUeAgentCanonicalIdentity> ImpactTargets;
		FPlanningPreflightError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FChangeSetDraft final
	{
	public:
		FChangeSetDraft(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			FString InIdempotencyKey,
			FUeAgentCanonicalSnapshotRef InSnapshot,
			TArray<FChangeDraft> InChanges);

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FString& GetRequestId() const;
		const FString& GetIdempotencyKey() const;
		const FUeAgentCanonicalSnapshotRef& GetSnapshot() const;
		const TArray<FChangeDraft>& GetChanges() const;
		bool IsValid() const;
		const FPlanningPreflightError& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		FString IdempotencyKey;
		FUeAgentCanonicalSnapshotRef Snapshot;
		TArray<FChangeDraft> Changes;
		FPlanningPreflightError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FPlanningRequest final
	{
	public:
		FPlanningRequest(
			FChangeSetDraft InDraft,
			FUeAgentCallerContext InCallerContext,
			bool bInDryRun);

		const FChangeSetDraft& GetDraft() const;
		const FUeAgentCallerContext& GetCallerContext() const;
		bool IsDryRun() const;
		bool IsValid() const;
		const FPlanningPreflightError& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FChangeSetDraft Draft;
		FUeAgentCallerContext CallerContext;
		bool bDryRun = false;
		FPlanningPreflightError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FPlannedChange final
	{
	public:
		FPlannedChange(
			FUeAgentCanonicalIdentity InIdentity,
			EChangeIntent InIntent,
			FUeAgentCanonicalValuePtr InBefore,
			FUeAgentCanonicalValuePtr InAfter,
			TArray<FString> InRequiredAuthorityScopes,
			FString InImpactEvidenceId);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		EChangeIntent GetIntent() const;
		const FUeAgentCanonicalValuePtr& GetBefore() const;
		const FUeAgentCanonicalValuePtr& GetAfter() const;
		const TArray<FString>& GetRequiredAuthorityScopes() const;
		const FString& GetImpactEvidenceId() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		EChangeIntent Intent = EChangeIntent::Update;
		FUeAgentCanonicalValuePtr Before;
		FUeAgentCanonicalValuePtr After;
		TArray<FString> RequiredAuthorityScopes;
		FString ImpactEvidenceId;
	};

	class UEAGENTINTERFACE_API FChangeSet final
	{
	public:
		explicit FChangeSet(TArray<FPlannedChange> InChanges = {});
		const TArray<FPlannedChange>& GetChanges() const;

	private:
		TArray<FPlannedChange> Changes;
	};

	class UEAGENTINTERFACE_API FImpact final
	{
	public:
		FImpact(
			FUeAgentCanonicalIdentity InIdentity,
			FUeAgentCanonicalIdentity InSourceIdentity,
			FString InKind,
			FString InEvidenceId);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		const FUeAgentCanonicalIdentity& GetSourceIdentity() const;
		const FString& GetKind() const;
		const FString& GetEvidenceId() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		FUeAgentCanonicalIdentity SourceIdentity;
		FString Kind;
		FString EvidenceId;
	};

	class UEAGENTINTERFACE_API FImpactSet final
	{
	public:
		explicit FImpactSet(TArray<FImpact> InImpacts = {});
		const TArray<FImpact>& GetImpacts() const;

	private:
		TArray<FImpact> Impacts;
	};

	class UEAGENTINTERFACE_API FPrecondition final
	{
	public:
		FPrecondition(
			FUeAgentCanonicalIdentity InIdentity,
			FUeAgentStateRevision InObservedRevision,
			FString InExistence,
			FUeAgentCanonicalValuePtr InExpectedBefore);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		const FUeAgentStateRevision& GetObservedRevision() const;
		const FString& GetExistence() const;
		const FUeAgentCanonicalValuePtr& GetExpectedBefore() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		FUeAgentStateRevision ObservedRevision;
		FString Existence;
		FUeAgentCanonicalValuePtr ExpectedBefore;
	};

	class FPlanningPreflightPlan;
	using FPlanningPreflightPlanPtr =
		TSharedPtr<const FPlanningPreflightPlan>;
	using FPlanningPreflightPlanRef =
		TSharedRef<const FPlanningPreflightPlan>;

	class UEAGENTINTERFACE_API FPlanningPreflightPlan final
	{
	public:
		FPlanningPreflightPlan(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			FString InIdempotencyKey,
			FString InCallerId,
			TArray<FString> InAuthorityScopes,
			FString InSnapshotId,
			FUeAgentStateRevision InObservedRevision,
			FString InSnapshotEvidenceId,
			FChangeSet InChangeSet,
			FImpactSet InImpactSet,
			TArray<FPrecondition> InPreconditions,
			FString InFingerprint);

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FString& GetRequestId() const;
		const FString& GetIdempotencyKey() const;
		const FString& GetCallerId() const;
		const TArray<FString>& GetAuthorityScopes() const;
		const FString& GetSnapshotId() const;
		const FUeAgentStateRevision& GetObservedRevision() const;
		const FString& GetSnapshotEvidenceId() const;
		const FChangeSet& GetChangeSet() const;
		const FImpactSet& GetImpactSet() const;
		const TArray<FPrecondition>& GetPreconditions() const;
		const FString& GetFingerprint() const;
		FPlanningPreflightPlanRef WithFingerprint(
			FString InFingerprint) const;

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		FString IdempotencyKey;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString SnapshotId;
		FUeAgentStateRevision ObservedRevision;
		FString SnapshotEvidenceId;
		FChangeSet ChangeSet;
		FImpactSet ImpactSet;
		TArray<FPrecondition> Preconditions;
		FString Fingerprint;
	};

	class UEAGENTINTERFACE_API FPlanningResult final
	{
	public:
		FPlanningResult() = default;
		FPlanningResult(
			FPlanningPreflightPlanPtr InPlan,
			bool bInDryRun,
			bool bInMutationPermitted,
			FString InStatus);

		const FPlanningPreflightPlanPtr& GetPlan() const;
		bool IsDryRun() const;
		bool IsMutationPermitted() const;
		const FString& GetStatus() const;
		bool IsReady() const;

	private:
		FPlanningPreflightPlanPtr Plan;
		bool bDryRun = false;
		bool bMutationPermitted = false;
		FString Status;
	};

	UEAGENTINTERFACE_API bool TryComputePlanFingerprint(
		const FPlanningPreflightPlanRef& Plan,
		FUeAgentResourceTracker& Resources,
		FString& OutFingerprint,
		FPlanningPreflightError& OutError);

	UEAGENTINTERFACE_API bool TryPlanChanges(
		const FPlanningRequest& Request,
		FUeAgentResourceTracker& Resources,
		FPlanningResult& OutResult,
		FPlanningPreflightError& OutError);

	UEAGENTINTERFACE_API bool TryRevalidatePlan(
		const FPlanningPreflightPlanRef& Plan,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		const FUeAgentCallerContext& CallerContext,
		FUeAgentResourceTracker& Resources,
		FPlanningPreflightError& OutError);
}
