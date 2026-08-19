#include "Composition/GraphCompound/UeAgentGraphCompoundCanonical.h"

#include "Core/ContextResource/UeAgentContextResource.h"

namespace UE::UAI::Composition::GraphCompound
{
	namespace
	{
		using namespace UE::UAI::ContextResource;
		using namespace UE::UAI::PlanningPreflight;
		using namespace UE::UAI::StateSemantic;

		TArray<FString> NormalizeStrings(TArray<FString> Values)
		{
			for (FString& Value : Values)
			{
				Value.TrimStartAndEndInline();
			}
			Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1])
				{
					Values.RemoveAt(Index);
				}
			}
			return Values;
		}

		bool IsElementKindValid(const EGraphCompoundElementKind Kind)
		{
			switch (Kind)
			{
			case EGraphCompoundElementKind::Target:
			case EGraphCompoundElementKind::Structure:
			case EGraphCompoundElementKind::Item:
			case EGraphCompoundElementKind::Pin:
			case EGraphCompoundElementKind::Relation:
			case EGraphCompoundElementKind::Property:
			case EGraphCompoundElementKind::Input:
			case EGraphCompoundElementKind::Hierarchy:
			case EGraphCompoundElementKind::Order:
			case EGraphCompoundElementKind::Binding:
			case EGraphCompoundElementKind::Dependency:
				return true;
			default:
				return false;
			}
		}

		bool TryChargeValidation(
			const int64 Items,
			const int64 Edges,
			FUeAgentResourceTracker& Resources,
			FGraphCompoundCanonicalError& OutError)
		{
			FUeAgentResourceBudgetExceeded Error;
			if (Resources.TryChargeMany(
				{
					{EUeAgentResourceDimension::Nodes, Items},
					{EUeAgentResourceDimension::Items, Items},
					{EUeAgentResourceDimension::ReferenceEdges, Edges},
					{EUeAgentResourceDimension::WorkUnits, Items + Edges},
					{
						EUeAgentResourceDimension::EstimatedMemoryBytes,
						Items * 160 + Edges * 16,
					},
				},
				TEXT("/graph_compound/canonical"),
				Error))
			{
				return true;
			}
			OutError.Code = Error.Code.IsEmpty()
				? TEXT("graph_compound_resource_budget_exceeded")
				: Error.Code;
			OutError.Detail = FString::Printf(
				TEXT("resource %s observed %lld over limit %lld at %s"),
				*LexToString(Error.Dimension),
				static_cast<long long>(Error.Observed),
				static_cast<long long>(Error.Limit),
				*Error.Path);
			return false;
		}

		void SetError(
			FGraphCompoundCanonicalError& OutError,
			const FString& Code,
			const FString& IdentityKey,
			const FString& Detail)
		{
			OutError.Code = Code;
			OutError.IdentityKey = IdentityKey;
			OutError.Detail = Detail;
		}
	}

	FString GetGraphCompoundCanonicalSchemaId()
	{
		return TEXT("uai.graph_compound.canonical.v1");
	}

	FString LexToString(const EGraphCompoundElementKind Kind)
	{
		switch (Kind)
		{
		case EGraphCompoundElementKind::Target: return TEXT("target");
		case EGraphCompoundElementKind::Structure: return TEXT("structure");
		case EGraphCompoundElementKind::Item: return TEXT("item");
		case EGraphCompoundElementKind::Pin: return TEXT("pin");
		case EGraphCompoundElementKind::Relation: return TEXT("relation");
		case EGraphCompoundElementKind::Property: return TEXT("property");
		case EGraphCompoundElementKind::Input: return TEXT("input");
		case EGraphCompoundElementKind::Hierarchy: return TEXT("hierarchy");
		case EGraphCompoundElementKind::Order: return TEXT("order");
		case EGraphCompoundElementKind::Binding: return TEXT("binding");
		case EGraphCompoundElementKind::Dependency: return TEXT("dependency");
		default: return TEXT("unknown");
		}
	}

	FGraphCompoundCanonicalEdit::FGraphCompoundCanonicalEdit(
		FUeAgentCanonicalIdentity InIdentity,
		const EGraphCompoundElementKind InElementKind,
		const EChangeIntent InIntent,
		FUeAgentCanonicalValuePtr InProposedAfter,
		TArray<FUeAgentCanonicalIdentity> InDependencies,
		TArray<FString> InRequiredAuthorityScopes,
		FString InImpactEvidenceId,
		TArray<FUeAgentCanonicalIdentity> InImpactTargets)
		: Identity(MoveTemp(InIdentity))
		, ElementKind(InElementKind)
		, Intent(InIntent)
		, ProposedAfter(MoveTemp(InProposedAfter))
		, Dependencies(MoveTemp(InDependencies))
		, RequiredAuthorityScopes(NormalizeStrings(MoveTemp(InRequiredAuthorityScopes)))
		, ImpactEvidenceId(MoveTemp(InImpactEvidenceId))
		, ImpactTargets(MoveTemp(InImpactTargets))
	{
		ImpactEvidenceId.TrimStartAndEndInline();
		Dependencies.Sort(
			[](const FUeAgentCanonicalIdentity& Left,
				const FUeAgentCanonicalIdentity& Right)
			{
				return Left.GetCanonicalKey() < Right.GetCanonicalKey();
			});
		ImpactTargets.Sort(
			[](const FUeAgentCanonicalIdentity& Left,
				const FUeAgentCanonicalIdentity& Right)
			{
				return Left.GetCanonicalKey() < Right.GetCanonicalKey();
			});
		bool bReferencesValid = true;
		for (const FUeAgentCanonicalIdentity& Dependency : Dependencies)
		{
			bReferencesValid = bReferencesValid && Dependency.IsValid();
		}
		for (const FUeAgentCanonicalIdentity& Target : ImpactTargets)
		{
			bReferencesValid = bReferencesValid && Target.IsValid();
		}
		const bool bValueValid = Intent == EChangeIntent::Delete
			? !ProposedAfter.IsValid()
			: ProposedAfter.IsValid();
		bValid = Identity.IsValid()
			&& IsElementKindValid(ElementKind)
			&& bValueValid
			&& bReferencesValid
			&& !ImpactEvidenceId.IsEmpty();
	}

	const FUeAgentCanonicalIdentity& FGraphCompoundCanonicalEdit::GetIdentity() const
	{
		return Identity;
	}

	EGraphCompoundElementKind FGraphCompoundCanonicalEdit::GetElementKind() const
	{
		return ElementKind;
	}

	EChangeIntent FGraphCompoundCanonicalEdit::GetIntent() const { return Intent; }

	const FUeAgentCanonicalValuePtr& FGraphCompoundCanonicalEdit::GetProposedAfter() const
	{
		return ProposedAfter;
	}

	const TArray<FUeAgentCanonicalIdentity>& FGraphCompoundCanonicalEdit::GetDependencies() const
	{
		return Dependencies;
	}

	const TArray<FString>& FGraphCompoundCanonicalEdit::GetRequiredAuthorityScopes() const
	{
		return RequiredAuthorityScopes;
	}

	const FString& FGraphCompoundCanonicalEdit::GetImpactEvidenceId() const
	{
		return ImpactEvidenceId;
	}

	const TArray<FUeAgentCanonicalIdentity>& FGraphCompoundCanonicalEdit::GetImpactTargets() const
	{
		return ImpactTargets;
	}

	bool FGraphCompoundCanonicalEdit::IsValid() const { return bValid; }

	FGraphCompoundCanonicalPayload::FGraphCompoundCanonicalPayload(
		FString InSchemaId,
		TArray<FGraphCompoundCanonicalEdit> InEdits)
		: SchemaId(MoveTemp(InSchemaId))
		, Edits(MoveTemp(InEdits))
	{
		SchemaId.TrimStartAndEndInline();
	}

	const FString& FGraphCompoundCanonicalPayload::GetSchemaId() const
	{
		return SchemaId;
	}

	const TArray<FGraphCompoundCanonicalEdit>& FGraphCompoundCanonicalPayload::GetEdits() const
	{
		return Edits;
	}

	bool TryValidateGraphCompoundCanonicalPayload(
		const FGraphCompoundCanonicalPayload& Payload,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		FUeAgentResourceTracker& Resources,
		FGraphCompoundCanonicalError& OutError)
	{
		OutError = FGraphCompoundCanonicalError();
		if (Payload.GetSchemaId() != GetGraphCompoundCanonicalSchemaId())
		{
			SetError(
				OutError,
				TEXT("graph_compound_schema_mismatch"),
				TEXT(""),
				TEXT("canonical graph payload must use uai.graph_compound.canonical.v1"));
			return false;
		}
		if (Payload.GetEdits().IsEmpty())
		{
			SetError(
				OutError,
				TEXT("graph_compound_edits_empty"),
				TEXT(""),
				TEXT("at least one explicit per-identity edit is required"));
			return false;
		}

		int64 EdgeCount = 0;
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			EdgeCount += Edit.GetDependencies().Num();
		}
		if (!TryChargeValidation(
			Payload.GetEdits().Num(),
			EdgeCount,
			Resources,
			OutError))
		{
			return false;
		}

		TMap<FString, const FUeAgentStateEntry*> SnapshotByKey;
		for (const FUeAgentStateEntry& Entry : Snapshot->GetEntries())
		{
			SnapshotByKey.Add(Entry.GetIdentity().GetCanonicalKey(), &Entry);
		}

		TMap<FString, int32> EditIndexByKey;
		for (int32 Index = 0; Index < Payload.GetEdits().Num(); ++Index)
		{
			const FGraphCompoundCanonicalEdit& Edit = Payload.GetEdits()[Index];
			const FString& Key = Edit.GetIdentity().GetCanonicalKey();
			if (!Edit.IsValid())
			{
				SetError(
					OutError,
					TEXT("graph_compound_edit_invalid"),
					Key,
					TEXT("canonical edit identity, kind, value or impact evidence is invalid"));
				return false;
			}
			if (EditIndexByKey.Contains(Key))
			{
				SetError(
					OutError,
					TEXT("graph_compound_duplicate_identity"),
					Key,
					TEXT("one canonical payload may edit an identity only once"));
				return false;
			}
			EditIndexByKey.Add(Key, Index);

			const bool bExists = SnapshotByKey.Contains(Key);
			if (Edit.GetIntent() == EChangeIntent::Create && bExists)
			{
				SetError(
					OutError,
					TEXT("graph_compound_create_target_exists"),
					Key,
					TEXT("create requires the canonical identity to be absent from the C4 snapshot"));
				return false;
			}
			if (Edit.GetIntent() != EChangeIntent::Create && !bExists)
			{
				SetError(
					OutError,
					TEXT("graph_compound_change_target_missing"),
					Key,
					TEXT("update, replace and delete require an independently read current value"));
				return false;
			}
		}

		TArray<int32> InDegree;
		InDegree.Init(0, Payload.GetEdits().Num());
		TArray<TArray<int32>> Dependents;
		Dependents.SetNum(Payload.GetEdits().Num());
		for (int32 Index = 0; Index < Payload.GetEdits().Num(); ++Index)
		{
			TSet<FString> SeenDependencies;
			for (const FUeAgentCanonicalIdentity& Dependency
				: Payload.GetEdits()[Index].GetDependencies())
			{
				const FString& DependencyKey = Dependency.GetCanonicalKey();
				if (SeenDependencies.Contains(DependencyKey))
				{
					SetError(
						OutError,
						TEXT("graph_compound_duplicate_dependency"),
						Payload.GetEdits()[Index].GetIdentity().GetCanonicalKey(),
						DependencyKey);
					return false;
				}
				SeenDependencies.Add(DependencyKey);
				const int32* DependencyIndex = EditIndexByKey.Find(DependencyKey);
				if (DependencyIndex != nullptr)
				{
					++InDegree[Index];
					Dependents[*DependencyIndex].Add(Index);
				}
				else if (!SnapshotByKey.Contains(DependencyKey))
				{
					SetError(
						OutError,
						TEXT("graph_compound_dependency_missing"),
						Payload.GetEdits()[Index].GetIdentity().GetCanonicalKey(),
						DependencyKey);
					return false;
				}
			}
		}

		TArray<int32> Ready;
		Ready.Reserve(InDegree.Num());
		for (int32 Index = 0; Index < InDegree.Num(); ++Index)
		{
			if (InDegree[Index] == 0)
			{
				Ready.Add(Index);
			}
		}
		int32 Processed = 0;
		for (int32 ReadyIndex = 0; ReadyIndex < Ready.Num(); ++ReadyIndex)
		{
			const int32 Index = Ready[ReadyIndex];
			++Processed;
			for (const int32 Dependent : Dependents[Index])
			{
				if (--InDegree[Dependent] == 0)
				{
					Ready.Add(Dependent);
				}
			}
		}
		if (Processed != Payload.GetEdits().Num())
		{
			SetError(
				OutError,
				TEXT("graph_compound_dependency_cycle"),
				TEXT(""),
				TEXT("canonical dependency graph contains a cycle"));
			return false;
		}
		return true;
	}

	bool TryBuildGraphCompoundChangeSetDraft(
		const FGraphCompoundCanonicalPayload& Payload,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FString& RequestId,
		const FString& IdempotencyKey,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FGraphCompoundCanonicalError& OutError)
	{
		OutDraft.Reset();
		OutError = FGraphCompoundCanonicalError();
		if (!DescriptorIdentity.IsValid()
			|| RequestId.TrimStartAndEnd().IsEmpty()
			|| IdempotencyKey.TrimStartAndEnd().IsEmpty())
		{
			SetError(
				OutError,
				TEXT("graph_compound_draft_identity_invalid"),
				TEXT(""),
				TEXT("descriptor, request id and idempotency key are required"));
			return false;
		}
		if (!TryValidateGraphCompoundCanonicalPayload(
			Payload,
			Snapshot,
			Resources,
			OutError))
		{
			return false;
		}

		TMap<FString, FUeAgentCanonicalValuePtr> CurrentByKey;
		for (const FUeAgentStateEntry& Entry : Snapshot->GetEntries())
		{
			CurrentByKey.Add(
				Entry.GetIdentity().GetCanonicalKey(),
				Entry.GetValue());
		}

		TArray<FChangeDraft> Changes;
		Changes.Reserve(Payload.GetEdits().Num());
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			const FUeAgentCanonicalValuePtr* Current =
				CurrentByKey.Find(Edit.GetIdentity().GetCanonicalKey());
			Changes.Emplace(
				Edit.GetIdentity(),
				Edit.GetIntent(),
				Current == nullptr ? FUeAgentCanonicalValuePtr() : *Current,
				Edit.GetProposedAfter(),
				Edit.GetRequiredAuthorityScopes(),
				Edit.GetImpactEvidenceId(),
				Edit.GetImpactTargets());
			if (!Changes.Last().IsValid())
			{
				SetError(
					OutError,
					TEXT("graph_compound_change_draft_invalid"),
					Edit.GetIdentity().GetCanonicalKey(),
					TEXT("per-identity canonical edit could not form a C6 change draft"));
				return false;
			}
		}

		FChangeSetDraft Draft(
			DescriptorIdentity,
			RequestId,
			IdempotencyKey,
			Snapshot,
			MoveTemp(Changes));
		if (!Draft.IsValid())
		{
			SetError(
				OutError,
				TEXT("graph_compound_change_set_invalid"),
				TEXT(""),
				TEXT("canonical edits could not form one valid C6 ChangeSetDraft"));
			return false;
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}

	FString LexToString(const EGraphCompoundExecutionLane Lane)
	{
		switch (Lane)
		{
		case EGraphCompoundExecutionLane::Query: return TEXT("c3q");
		case EGraphCompoundExecutionLane::Operation: return TEXT("c3o");
		default: return TEXT("unknown");
		}
	}

	FGraphCompoundExecutionContract::FGraphCompoundExecutionContract(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		const EGraphCompoundExecutionLane InLane,
		FString InFamilyId,
		FString InBindingId,
		FString InTypedRequestId,
		FString InTypedResultId,
		FString InC6ChangeKind,
		FString InNativeActionId,
		FString InReadbackId,
		FString InSettleBarrier,
		FString InOracleKind,
		const bool bInRequiresNativeMutation,
		const bool bInReadsBackAfterMutation)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, Lane(InLane)
		, FamilyId(MoveTemp(InFamilyId))
		, BindingId(MoveTemp(InBindingId))
		, TypedRequestId(MoveTemp(InTypedRequestId))
		, TypedResultId(MoveTemp(InTypedResultId))
		, C6ChangeKind(MoveTemp(InC6ChangeKind))
		, NativeActionId(MoveTemp(InNativeActionId))
		, ReadbackId(MoveTemp(InReadbackId))
		, SettleBarrier(MoveTemp(InSettleBarrier))
		, OracleKind(MoveTemp(InOracleKind))
		, bRequiresNativeMutation(bInRequiresNativeMutation)
		, bReadsBackAfterMutation(bInReadsBackAfterMutation)
	{
		for (FString* Value : {
			&FamilyId,
			&BindingId,
			&TypedRequestId,
			&TypedResultId,
			&C6ChangeKind,
			&NativeActionId,
			&ReadbackId,
			&SettleBarrier,
			&OracleKind})
		{
			Value->TrimStartAndEndInline();
		}
		const bool bCommonValid = DescriptorIdentity.IsValid()
			&& !FamilyId.IsEmpty()
			&& !BindingId.IsEmpty()
			&& !TypedRequestId.IsEmpty()
			&& !TypedResultId.IsEmpty()
			&& !ReadbackId.IsEmpty()
			&& !OracleKind.IsEmpty();
		const bool bQueryValid = Lane == EGraphCompoundExecutionLane::Query
			&& !bRequiresNativeMutation
			&& !bReadsBackAfterMutation
			&& C6ChangeKind.IsEmpty()
			&& NativeActionId.IsEmpty()
			&& SettleBarrier.IsEmpty();
		const bool bOperationValid = Lane == EGraphCompoundExecutionLane::Operation
			&& bRequiresNativeMutation
			&& bReadsBackAfterMutation
			&& !C6ChangeKind.IsEmpty()
			&& !NativeActionId.IsEmpty()
			&& !SettleBarrier.IsEmpty();
		bValid = bCommonValid && (bQueryValid || bOperationValid);
	}

	const FUeAgentDescriptorIdentity& FGraphCompoundExecutionContract::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	EGraphCompoundExecutionLane FGraphCompoundExecutionContract::GetLane() const
	{
		return Lane;
	}

	const FString& FGraphCompoundExecutionContract::GetFamilyId() const { return FamilyId; }
	const FString& FGraphCompoundExecutionContract::GetBindingId() const { return BindingId; }
	const FString& FGraphCompoundExecutionContract::GetTypedRequestId() const { return TypedRequestId; }
	const FString& FGraphCompoundExecutionContract::GetTypedResultId() const { return TypedResultId; }
	const FString& FGraphCompoundExecutionContract::GetC6ChangeKind() const { return C6ChangeKind; }
	const FString& FGraphCompoundExecutionContract::GetNativeActionId() const { return NativeActionId; }
	const FString& FGraphCompoundExecutionContract::GetReadbackId() const { return ReadbackId; }
	const FString& FGraphCompoundExecutionContract::GetSettleBarrier() const { return SettleBarrier; }
	const FString& FGraphCompoundExecutionContract::GetOracleKind() const { return OracleKind; }
	bool FGraphCompoundExecutionContract::RequiresNativeMutation() const { return bRequiresNativeMutation; }
	bool FGraphCompoundExecutionContract::ReadsBackAfterMutation() const { return bReadsBackAfterMutation; }
	bool FGraphCompoundExecutionContract::IsValid() const { return bValid; }
}
