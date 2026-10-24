#include "Composition/GraphCompound/UeAgentGraphCompoundCanonical.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::GraphCompound
{
	namespace
	{
		using namespace UE::UAI::ContextResource;
		using namespace UE::UAI::PlanningPreflight;
		using namespace UE::UAI::StateSemantic;

		FGraphCompoundCanonicalError CanonicalFailure(const FString& Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString())
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
			Error.PropertyPath = Field;
			Error.TargetPath = Target;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			MarkGraphCompoundNativeDiagnostic(Error);
			return Error;
		}

		FGraphCompoundCanonicalError IdentityFailureAt(const FUeAgentCanonicalIdentity& Identity, const FString& Field)
		{
			auto Error = Identity.GetValidationDiagnostic();
			// Identity validation reports paths relative to its own /identity root.
			FString Relative = Error.SemanticPointer;
			Relative.RemoveFromStart(TEXT("/identity"));
			Error.SemanticPointer = Field + Relative;
			Error.PropertyPath = Error.SemanticPointer;
			MarkGraphCompoundNativeDiagnostic(Error);
			return Error;
		}

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
			FUeAgentResourceChargeError Error;
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
			OutError = Error;
			MarkGraphCompoundNativeDiagnostic(OutError);
			return false;
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
		if (!Identity.IsValid())
		{
			ValidationDiagnostic = IdentityFailureAt(Identity, TEXT("/identity"));
			return;
		}
		auto Reject = [this](const FString& Reason, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			ValidationDiagnostic = CanonicalFailure(TEXT("graph_compound_edit_invalid"), Reason,
				Field, MoveTemp(Actual), MoveTemp(Expected), Identity.GetOwnerId());
			ValidationDiagnostic.SubjectId = Identity.GetSemanticId();
		};
		if (!IsElementKindValid(ElementKind))
		{
			Reject(TEXT("The canonical edit element kind is not supported."), TEXT("/element_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(ElementKind)),
				MakeShared<FJsonValueString>(TEXT("target, structure, item, pin, relation, property, input, hierarchy, order, binding or dependency")));
			return;
		}
		const bool bRequiresAfter = Intent != EChangeIntent::Delete;
		if (ProposedAfter.IsValid() != bRequiresAfter)
		{
			Reject(bRequiresAfter ? TEXT("The canonical edit requires a proposed value.")
				: TEXT("A delete edit must not contain a proposed value."), TEXT("/proposed_after.present"),
				MakeShared<FJsonValueBoolean>(ProposedAfter.IsValid()), MakeShared<FJsonValueBoolean>(bRequiresAfter));
			return;
		}
		for (int32 Index = 0; Index < Dependencies.Num(); ++Index)
		{
			if (!Dependencies[Index].IsValid())
			{
				ValidationDiagnostic = IdentityFailureAt(Dependencies[Index], FString::Printf(TEXT("/dependencies/%d"), Index));
				ValidationDiagnostic.TargetPath = Identity.GetOwnerId();
				return;
			}
		}
		for (int32 Index = 0; Index < ImpactTargets.Num(); ++Index)
		{
			if (!ImpactTargets[Index].IsValid())
			{
				ValidationDiagnostic = IdentityFailureAt(ImpactTargets[Index], FString::Printf(TEXT("/impact_targets/%d"), Index));
				ValidationDiagnostic.TargetPath = Identity.GetOwnerId();
				return;
			}
		}
		if (ImpactEvidenceId.IsEmpty())
			Reject(TEXT("The canonical edit requires a non-empty impact evidence identifier."),
				TEXT("/impact_evidence_id.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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

	bool FGraphCompoundCanonicalEdit::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FGraphCompoundCanonicalError& FGraphCompoundCanonicalEdit::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FGraphCompoundCanonicalPayload::FGraphCompoundCanonicalPayload(
		FString InSchemaId,
		TArray<FGraphCompoundCanonicalEdit> InEdits,
		TMap<FString, FString> InInputPointers)
		: SchemaId(MoveTemp(InSchemaId))
		, Edits(MoveTemp(InEdits))
		, InputPointers(MoveTemp(InInputPointers))
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

	TSharedPtr<FJsonValue> CanonicalInputFact(const StateSemantic::FUeAgentCanonicalValue* Value)
	{
		using StateSemantic::EUeAgentValueKind;
		if (!Value) return nullptr;
		switch (Value->GetKind())
		{
		case EUeAgentValueKind::Null: return MakeShared<FJsonValueNull>();
		case EUeAgentValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value->GetBoolean());
		case EUeAgentValueKind::String: return MakeShared<FJsonValueString>(Value->GetText());
		default: break;
		}
		const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
		switch (Value->GetKind())
		{
		case EUeAgentValueKind::Integer:
		case EUeAgentValueKind::Decimal:
			Fact->SetStringField(TEXT("kind"), Value->GetKind() == EUeAgentValueKind::Integer
				? TEXT("integer") : TEXT("decimal"));
			Fact->SetStringField(TEXT("value"), Value->GetLexeme());
			break;
		case EUeAgentValueKind::Array:
			Fact->SetStringField(TEXT("kind"), TEXT("array"));
			Fact->SetNumberField(TEXT("count"), Value->GetArrayItems().Num());
			break;
		case EUeAgentValueKind::Object:
			Fact->SetStringField(TEXT("kind"), TEXT("object"));
			Fact->SetNumberField(TEXT("count"), Value->GetObjectItems().Num());
			break;
		default:
			Fact->SetNumberField(TEXT("canonical_kind"), static_cast<int32>(Value->GetKind()));
			break;
		}
		return MakeShared<FJsonValueObject>(Fact);
	}

	void BindGraphCompoundInputValueDiagnostic(Contract::FDiagnostic& Diagnostic, const FString& InputPointer, const FString& ValueRoot)
	{
		if (!Diagnostic.IsValid()) return;
		for (auto& Child : Diagnostic.Diagnostics) BindGraphCompoundInputValueDiagnostic(Child, InputPointer, ValueRoot);
		if (Diagnostic.PublicCause)
		{
			auto Cause = MakeShared<Contract::FDiagnostic>(*Diagnostic.PublicCause);
			BindGraphCompoundInputValueDiagnostic(*Cause, InputPointer, ValueRoot);
			Diagnostic.PublicCause = MoveTemp(Cause);
		}
		if (Diagnostic.SourceKind == TEXT("native_result") || Diagnostic.SourceKind == TEXT("command_input")
			|| !Diagnostic.Source.SourceFile.IsEmpty()) return;
		auto Bind = [&](FString& Path)
		{
			if (ValueRoot != TEXT("/"))
			{
				if (Path == ValueRoot) Path.Reset();
				else if (Path.StartsWith(ValueRoot + TEXT("/"))) Path.RightChopInline(ValueRoot.Len());
			}
			Path = (InputPointer == TEXT("/") ? FString() : InputPointer)
				+ (Path == TEXT("/") ? FString() : Path);
			if (Path.IsEmpty()) Path = TEXT("/");
		};
		Bind(Diagnostic.Source.JsonPath);
		if (!Diagnostic.Source.InternalJsonPath.IsEmpty()) Bind(Diagnostic.Source.InternalJsonPath);
		if (!Diagnostic.SemanticPointer.IsEmpty()) Bind(Diagnostic.SemanticPointer);
		else Diagnostic.SemanticPointer = Diagnostic.Source.JsonPath;
		Diagnostic.SourceKind = TEXT("command_input");
	}

	void MarkGraphCompoundNativeDiagnostic(Contract::FDiagnostic& Diagnostic)
	{
		if (!Diagnostic.IsValid()) return;
		Diagnostic.SourceKind = TEXT("native_result");
		for (auto& Child : Diagnostic.Diagnostics) MarkGraphCompoundNativeDiagnostic(Child);
		if (Diagnostic.PublicCause)
		{
			auto Cause = MakeShared<Contract::FDiagnostic>(*Diagnostic.PublicCause);
			MarkGraphCompoundNativeDiagnostic(*Cause);
			Diagnostic.PublicCause = MoveTemp(Cause);
		}
	}

	void FGraphCompoundCanonicalPayload::LocateDiagnostic(Contract::FDiagnostic& Diagnostic, const FString& SemanticId) const
	{
		if (!Diagnostic.IsValid()) return;
		for (auto& Child : Diagnostic.Diagnostics) LocateDiagnostic(Child, SemanticId);
		if (Diagnostic.PublicCause)
		{
			auto Cause = MakeShared<Contract::FDiagnostic>(*Diagnostic.PublicCause);
			LocateDiagnostic(*Cause, SemanticId); Diagnostic.PublicCause = MoveTemp(Cause);
		}
		if (Diagnostic.SourceKind == TEXT("native_result") || Diagnostic.SourceKind == TEXT("command_input")
			|| !Diagnostic.Source.SourceFile.IsEmpty() || !Diagnostic.SemanticPointer.IsEmpty() || !Diagnostic.Source.JsonPath.IsEmpty()) return;
		const FString* Pointer = InputPointers.Find(SemanticId.IsEmpty() ? Diagnostic.SubjectId : SemanticId);
		// A whole-operation failure is not evidence against an arbitrary first edit.
		if (!Pointer) return;
		Diagnostic.Source.JsonPath = *Pointer;
		Diagnostic.SourceKind = TEXT("related_input");
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
			OutError = CanonicalFailure(TEXT("graph_compound_schema_mismatch"),
				TEXT("The canonical graph payload schema does not match the supported schema."), TEXT("/schema_id"),
				MakeShared<FJsonValueString>(Payload.GetSchemaId()), MakeShared<FJsonValueString>(GetGraphCompoundCanonicalSchemaId()));
			return false;
		}
		if (Payload.GetEdits().IsEmpty())
		{
			OutError = CanonicalFailure(TEXT("graph_compound_edits_empty"),
				TEXT("At least one explicit per-identity edit is required."), TEXT("/edits.count"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one edit")));
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
				OutError = Edit.GetValidationDiagnostic();
				OutError.OperationIndex = Index;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Edit.GetIdentity().GetSemanticId();
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Edit.GetIdentity().GetOwnerId();
				return false;
			}
			if (EditIndexByKey.Contains(Key))
			{
				OutError = CanonicalFailure(TEXT("graph_compound_duplicate_identity"),
					TEXT("This canonical edit repeats the identity of an earlier edit."), TEXT("/edits/duplicate_of_index"),
					MakeShared<FJsonValueNumber>(EditIndexByKey.FindChecked(Key)), MakeShared<FJsonValueString>(TEXT("unique identity per edit")),
					Edit.GetIdentity().GetOwnerId());
				OutError.OperationIndex = Index;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Key);
				return false;
			}
			EditIndexByKey.Add(Key, Index);

			const bool bExists = SnapshotByKey.Contains(Key);
			if (Edit.GetIntent() == EChangeIntent::Create && bExists)
			{
				OutError = CanonicalFailure(TEXT("graph_compound_create_target_exists"),
					TEXT("The create edit's target is already present in the independent snapshot."), TEXT("/target.present"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Edit.GetIdentity().GetOwnerId());
				OutError.OperationIndex = Index;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Key);
				return false;
			}
			if (Edit.GetIntent() != EChangeIntent::Create && !bExists)
			{
				OutError = CanonicalFailure(TEXT("graph_compound_change_target_missing"),
					TEXT("The edit requires a target with an independently read current value."), TEXT("/target.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Edit.GetIdentity().GetOwnerId());
				OutError.OperationIndex = Index;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Key);
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
			const auto& Edit = Payload.GetEdits()[Index];
			for (int32 DependencyIndexInEdit = 0; DependencyIndexInEdit < Edit.GetDependencies().Num(); ++DependencyIndexInEdit)
			{
				const auto& Dependency = Edit.GetDependencies()[DependencyIndexInEdit];
				const FString& DependencyKey = Dependency.GetCanonicalKey();
				if (SeenDependencies.Contains(DependencyKey))
				{
					OutError = CanonicalFailure(TEXT("graph_compound_duplicate_dependency"),
						TEXT("The canonical edit lists the same dependency more than once."),
						FString::Printf(TEXT("/dependencies/%d"), DependencyIndexInEdit),
						MakeShared<FJsonValueString>(Dependency.GetOwnerId()), MakeShared<FJsonValueString>(TEXT("one reference per dependency")),
						Edit.GetIdentity().GetOwnerId());
					OutError.OperationIndex = Index;
					OutError.ActualEvidence = MakeShared<FJsonValueString>(DependencyKey);
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
					OutError = CanonicalFailure(TEXT("graph_compound_dependency_missing"),
						TEXT("The referenced dependency is absent from both the payload edits and the independent snapshot."),
						FString::Printf(TEXT("/dependencies/%d"), DependencyIndexInEdit),
						MakeShared<FJsonValueString>(Dependency.GetOwnerId()), MakeShared<FJsonValueString>(TEXT("dependency present in edits or snapshot")),
						Edit.GetIdentity().GetOwnerId());
					OutError.OperationIndex = Index;
					OutError.ActualEvidence = MakeShared<FJsonValueString>(DependencyKey);
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
			TArray<TSharedPtr<FJsonValue>> BlockedIndices;
			for (int32 Index = 0; Index < InDegree.Num(); ++Index)
				if (InDegree[Index] > 0) BlockedIndices.Add(MakeShared<FJsonValueNumber>(Index));
			OutError = CanonicalFailure(TEXT("graph_compound_dependency_cycle"),
				TEXT("A dependency cycle prevents these edits from being ordered; listed edits may also depend on the cycle."),
				TEXT("/dependencies/blocked_edit_indices"), MakeShared<FJsonValueArray>(MoveTemp(BlockedIndices)),
				MakeShared<FJsonValueString>(TEXT("acyclic dependency graph")));
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
		if (!DescriptorIdentity.IsValid())
		{
			OutError = DescriptorIdentity.GetValidationDiagnostic();
			MarkGraphCompoundNativeDiagnostic(OutError);
			return false;
		}
		if (RequestId.TrimStartAndEnd().IsEmpty() || IdempotencyKey.TrimStartAndEnd().IsEmpty())
		{
			const bool bRequestMissing = RequestId.TrimStartAndEnd().IsEmpty();
			OutError = CanonicalFailure(TEXT("graph_compound_draft_identity_invalid"),
				bRequestMissing ? TEXT("The graph change set requires a non-empty request identifier.")
					: TEXT("The graph change set requires a non-empty idempotency key."),
				bRequestMissing ? TEXT("/request_id.present") : TEXT("/idempotency_key.present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
				OutError = Changes.Last().GetValidationDiagnostic();
				MarkGraphCompoundNativeDiagnostic(OutError);
				OutError.OperationIndex = Changes.Num() - 1;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Edit.GetIdentity().GetSemanticId();
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Edit.GetIdentity().GetOwnerId();
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
			OutError = Draft.GetValidationDiagnostic();
			MarkGraphCompoundNativeDiagnostic(OutError);
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
		auto Reject = [this](const TCHAR* Field, const FString& Reason,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			ValidationDiagnostic = Contract::FDiagnostic::Failure(
				TEXT("graph_compound_execution_contract_invalid"), Reason,
				TEXT("graph_compound"), FString(), BindingId);
			ValidationDiagnostic.SourceKind = TEXT("native_result");
			ValidationDiagnostic.PropertyPath = Field;
			ValidationDiagnostic.ActualValue = MoveTemp(Actual);
			ValidationDiagnostic.ExpectedValue = MoveTemp(Expected);
		};
		if (!DescriptorIdentity.IsValid())
		{
			ValidationDiagnostic = Contract::FDiagnostic::WrapFailure(
				TEXT("graph_compound_execution_contract_invalid"),
				DescriptorIdentity.GetValidationDiagnostic(), TEXT("graph_compound"),
				TEXT("The graph execution contract has an invalid descriptor identity."));
			ValidationDiagnostic.SourceKind = TEXT("native_result");
			ValidationDiagnostic.PropertyPath = TEXT("descriptor_identity");
			return;
		}
		if (FamilyId.IsEmpty())
		{
			Reject(TEXT("family_id"), TEXT("The graph execution contract family_id is empty after trimming."),
				MakeShared<FJsonValueString>(FamilyId), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return;
		}
		if (BindingId.IsEmpty())
		{
			Reject(TEXT("binding_id"), TEXT("The graph execution contract binding_id is empty after trimming."),
				MakeShared<FJsonValueString>(BindingId), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return;
		}
		if (TypedRequestId.IsEmpty())
		{
			Reject(TEXT("typed_request_id"), TEXT("The graph execution contract typed_request_id is empty after trimming."),
				MakeShared<FJsonValueString>(TypedRequestId), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return;
		}
		if (TypedResultId.IsEmpty())
		{
			Reject(TEXT("typed_result_id"), TEXT("The graph execution contract typed_result_id is empty after trimming."),
				MakeShared<FJsonValueString>(TypedResultId), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return;
		}
		if (ReadbackId.IsEmpty())
		{
			Reject(TEXT("readback_id"), TEXT("The graph execution contract readback_id is empty after trimming."),
				MakeShared<FJsonValueString>(ReadbackId), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return;
		}
		if (OracleKind.IsEmpty())
		{
			Reject(TEXT("oracle_kind"), TEXT("The graph execution contract oracle_kind is empty after trimming."),
				MakeShared<FJsonValueString>(OracleKind), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return;
		}
		if (Lane != EGraphCompoundExecutionLane::Query && Lane != EGraphCompoundExecutionLane::Operation)
		{
			Reject(TEXT("lane"), TEXT("The graph execution contract lane is unsupported."),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Lane)),
				MakeShared<FJsonValueString>(TEXT("Query or Operation")));
			return;
		}
		const bool bOperation = Lane == EGraphCompoundExecutionLane::Operation;
		if (bRequiresNativeMutation != bOperation)
		{
			Reject(TEXT("requires_native_mutation"), TEXT("requires_native_mutation must be false for Query and true for Operation."),
				MakeShared<FJsonValueBoolean>(bRequiresNativeMutation), MakeShared<FJsonValueBoolean>(bOperation));
			return;
		}
		if (bReadsBackAfterMutation != bOperation)
		{
			Reject(TEXT("reads_back_after_mutation"), TEXT("reads_back_after_mutation must be false for Query and true for Operation."),
				MakeShared<FJsonValueBoolean>(bReadsBackAfterMutation), MakeShared<FJsonValueBoolean>(bOperation));
			return;
		}
		if (C6ChangeKind.IsEmpty() == bOperation)
		{
			Reject(TEXT("c6_change_kind"), TEXT("c6_change_kind must be empty for Query and non-empty for Operation."),
				MakeShared<FJsonValueString>(C6ChangeKind),
				MakeShared<FJsonValueString>(bOperation ? TEXT("non-empty string") : TEXT("empty string")));
			return;
		}
		if (NativeActionId.IsEmpty() == bOperation)
		{
			Reject(TEXT("native_action_id"), TEXT("native_action_id must be empty for Query and non-empty for Operation."),
				MakeShared<FJsonValueString>(NativeActionId),
				MakeShared<FJsonValueString>(bOperation ? TEXT("non-empty string") : TEXT("empty string")));
			return;
		}
		if (SettleBarrier.IsEmpty() == bOperation)
		{
			Reject(TEXT("settle_barrier"), TEXT("settle_barrier must be empty for Query and non-empty for Operation."),
				MakeShared<FJsonValueString>(SettleBarrier),
				MakeShared<FJsonValueString>(bOperation ? TEXT("non-empty string") : TEXT("empty string")));
			return;
		}
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
	bool FGraphCompoundExecutionContract::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const Contract::FDiagnostic& FGraphCompoundExecutionContract::GetValidationDiagnostic() const { return ValidationDiagnostic; }
}
