#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#define UAI_GRAPH_COMPOUND_CANONICAL_V1 1

namespace UE::UAI::Composition::GraphCompound
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::PlanningPreflight::EChangeIntent;
	using UE::UAI::PlanningPreflight::FChangeSetDraft;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotRef;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr;

	UEAGENTINTERFACE_API FString GetGraphCompoundCanonicalSchemaId();

	enum class EGraphCompoundElementKind : uint8
	{
		Target,
		Structure,
		Item,
		Pin,
		Relation,
		Property,
		Input,
		Hierarchy,
		Order,
		Binding,
		Dependency,
	};

	UEAGENTINTERFACE_API FString LexToString(EGraphCompoundElementKind Kind);

	using FGraphCompoundCanonicalError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FGraphCompoundCanonicalEdit final
	{
	public:
		FGraphCompoundCanonicalEdit(
			FUeAgentCanonicalIdentity InIdentity,
			EGraphCompoundElementKind InElementKind,
			EChangeIntent InIntent,
			FUeAgentCanonicalValuePtr InProposedAfter,
			TArray<FUeAgentCanonicalIdentity> InDependencies,
			TArray<FString> InRequiredAuthorityScopes,
			FString InImpactEvidenceId,
			TArray<FUeAgentCanonicalIdentity> InImpactTargets);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		EGraphCompoundElementKind GetElementKind() const;
		EChangeIntent GetIntent() const;
		const FUeAgentCanonicalValuePtr& GetProposedAfter() const;
		const TArray<FUeAgentCanonicalIdentity>& GetDependencies() const;
		const TArray<FString>& GetRequiredAuthorityScopes() const;
		const FString& GetImpactEvidenceId() const;
		const TArray<FUeAgentCanonicalIdentity>& GetImpactTargets() const;
		bool IsValid() const;
		const FGraphCompoundCanonicalError& GetValidationDiagnostic() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		EGraphCompoundElementKind ElementKind =
			EGraphCompoundElementKind::Item;
		EChangeIntent Intent = EChangeIntent::Update;
		FUeAgentCanonicalValuePtr ProposedAfter;
		TArray<FUeAgentCanonicalIdentity> Dependencies;
		TArray<FString> RequiredAuthorityScopes;
		FString ImpactEvidenceId;
		TArray<FUeAgentCanonicalIdentity> ImpactTargets;
		FGraphCompoundCanonicalError ValidationDiagnostic;
	};

	// Public diagnostic facts for an authored value: scalars retain their value;
	// containers report shape/count without copying their complete contents.
	UEAGENTINTERFACE_API TSharedPtr<FJsonValue> CanonicalInputFact(const StateSemantic::FUeAgentCanonicalValue* Value);

	// Bind a converted input value's relative paths at the boundary that owns its input location.
	UEAGENTINTERFACE_API void BindGraphCompoundInputValueDiagnostic(
		Contract::FDiagnostic& Diagnostic, const FString& InputPointer, const FString& ValueRoot = TEXT("/"));
	// Only for diagnostics produced from generated/native data, never authored input.
	UEAGENTINTERFACE_API void MarkGraphCompoundNativeDiagnostic(Contract::FDiagnostic& Diagnostic);

	class UEAGENTINTERFACE_API FGraphCompoundCanonicalPayload final
	{
	public:
		FGraphCompoundCanonicalPayload(
			FString InSchemaId,
			TArray<FGraphCompoundCanonicalEdit> InEdits,
			TMap<FString, FString> InInputPointers = {});

		const FString& GetSchemaId() const;
		const TArray<FGraphCompoundCanonicalEdit>& GetEdits() const;
		// Transient input provenance; never part of canonical state or fingerprints.
		void LocateDiagnostic(Contract::FDiagnostic& Diagnostic, const FString& SemanticId = FString()) const;
		const FString* FindInputPointer(const FString& SemanticId) const { return InputPointers.Find(SemanticId); }

	private:
		FString SchemaId;
		TArray<FGraphCompoundCanonicalEdit> Edits;
		TMap<FString, FString> InputPointers;
	};

	UEAGENTINTERFACE_API bool TryValidateGraphCompoundCanonicalPayload(
		const FGraphCompoundCanonicalPayload& Payload,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		FUeAgentResourceTracker& Resources,
		FGraphCompoundCanonicalError& OutError);

	UEAGENTINTERFACE_API bool TryBuildGraphCompoundChangeSetDraft(
		const FGraphCompoundCanonicalPayload& Payload,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FString& RequestId,
		const FString& IdempotencyKey,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FGraphCompoundCanonicalError& OutError);

	enum class EGraphCompoundExecutionLane : uint8
	{
		Query,
		Operation,
	};

	UEAGENTINTERFACE_API FString LexToString(EGraphCompoundExecutionLane Lane);

	class UEAGENTINTERFACE_API FGraphCompoundExecutionContract final
	{
	public:
		FGraphCompoundExecutionContract(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			EGraphCompoundExecutionLane InLane,
			FString InFamilyId,
			FString InBindingId,
			FString InTypedRequestId,
			FString InTypedResultId,
			FString InC6ChangeKind,
			FString InNativeActionId,
			FString InReadbackId,
			FString InSettleBarrier,
			FString InOracleKind,
			bool bInRequiresNativeMutation,
			bool bInReadsBackAfterMutation);

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		EGraphCompoundExecutionLane GetLane() const;
		const FString& GetFamilyId() const;
		const FString& GetBindingId() const;
		const FString& GetTypedRequestId() const;
		const FString& GetTypedResultId() const;
		const FString& GetC6ChangeKind() const;
		const FString& GetNativeActionId() const;
		const FString& GetReadbackId() const;
		const FString& GetSettleBarrier() const;
		const FString& GetOracleKind() const;
		bool RequiresNativeMutation() const;
		bool ReadsBackAfterMutation() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		EGraphCompoundExecutionLane Lane = EGraphCompoundExecutionLane::Query;
		FString FamilyId;
		FString BindingId;
		FString TypedRequestId;
		FString TypedResultId;
		FString C6ChangeKind;
		FString NativeActionId;
		FString ReadbackId;
		FString SettleBarrier;
		FString OracleKind;
		bool bRequiresNativeMutation = false;
		bool bReadsBackAfterMutation = false;
		Contract::FDiagnostic ValidationDiagnostic;
	};
}
