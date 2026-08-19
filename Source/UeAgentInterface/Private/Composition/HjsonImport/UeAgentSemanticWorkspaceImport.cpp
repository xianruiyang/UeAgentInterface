#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"

#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"

namespace UE::UAI::Composition::HjsonImport
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		void SetImportError(
			FSemanticWorkspaceImportError& OutError,
			const FString& Code,
			const FString& Detail = FString())
		{
			OutError.Code = Code;
			OutError.Detail = Detail.IsEmpty() ? Code : Detail;
		}

		const FUeAgentCanonicalValueRef* FindObjectValue(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Key)
		{
			if (Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Row
				: Object->GetObjectItems())
			{
				if (Row.Key == Key)
				{
					return &Row.Value;
				}
			}
			return nullptr;
		}

		bool TryResolvePropertyValue(
			const FString& FieldId,
			const FString& UnitId,
			const FUeAgentCanonicalValueRef& RawValue,
			bool& bOutSkip,
			FUeAgentCanonicalValuePtr& OutValue,
			FSemanticWorkspaceImportError& OutError)
		{
			bOutSkip = false;
			OutValue = RawValue;
			const FUeAgentCanonicalValueRef* Presence =
				FindObjectValue(RawValue, TEXT("presence"));
			if (!Presence)
			{
				return true;
			}
			if ((*Presence)->GetKind() != EUeAgentValueKind::String)
			{
				SetImportError(
					OutError,
					TEXT("hjson_import_property_presence_invalid"),
					UnitId + TEXT(":") + FieldId);
				return false;
			}
			FString PresenceText = (*Presence)->GetText();
			PresenceText.TrimStartAndEndInline();
			PresenceText.ToLowerInline();
			if (PresenceText.IsEmpty()
				|| PresenceText == TEXT("absent")
				|| PresenceText == TEXT("inherited")
				|| PresenceText == TEXT("default"))
			{
				bOutSkip = true;
				OutValue.Reset();
				return true;
			}
			const FUeAgentCanonicalValueRef* ExplicitValue =
				FindObjectValue(RawValue, TEXT("value"));
			if (PresenceText != TEXT("explicit") || !ExplicitValue)
			{
				SetImportError(
					OutError,
					TEXT("hjson_import_property_presence_invalid"),
					UnitId + TEXT(":") + FieldId);
				return false;
			}
			OutValue = *ExplicitValue;
			return true;
		}

		TSet<FString> PropertyOwnerCandidates(
			const FSemanticUnit& Unit,
			const FAuthoringModelRef& Model)
		{
			TSet<FString> Result;
			const FSemanticUnit* Current = &Unit;
			TSet<FString> Seen;
			while (Current && !Seen.Contains(Current->GetUnitId()))
			{
				Seen.Add(Current->GetUnitId());
				Result.Add(Current->GetUnitId());
				if (!Current->GetTargetIdentityKey().IsEmpty())
				{
					Result.Add(Current->GetTargetIdentityKey());
				}
				if (Current->GetOwnerRef().IsEmpty())
				{
					break;
				}
				Result.Add(Current->GetOwnerRef());
				Current = Model->FindUnit(Current->GetOwnerRef());
			}
			return Result;
		}

		const FUeAgentStateEntry* MatchPropertyEntry(
			const TArray<const FUeAgentStateEntry*>& Entries,
			const FSemanticUnit& Unit,
			const FString& FieldId,
			const FAuthoringModelRef& Model,
			bool& bOutAmbiguous)
		{
			bOutAmbiguous = false;
			if (Entries.IsEmpty())
			{
				return nullptr;
			}
			const TSet<FString> Owners =
				PropertyOwnerCandidates(Unit, Model);
			TSet<FString> Selectors;
			Selectors.Add(FieldId);
			if (!Unit.GetFieldSetRef().IsEmpty())
			{
				Selectors.Add(Unit.GetFieldSetRef());
			}
			auto UniqueMatch =
				[&Entries](
					TFunctionRef<bool(const FUeAgentStateEntry&)> Predicate)
					-> const FUeAgentStateEntry*
				{
					const FUeAgentStateEntry* Result = nullptr;
					for (const FUeAgentStateEntry* Entry : Entries)
					{
						if (!Entry || !Predicate(*Entry))
						{
							continue;
						}
						if (Result)
						{
							return nullptr;
						}
						Result = Entry;
					}
					return Result;
				};
			if (const FUeAgentStateEntry* Exact = UniqueMatch(
				[&Owners, &Selectors](const FUeAgentStateEntry& Entry)
				{
					return Owners.Contains(
							Entry.GetIdentity().GetOwnerId())
						&& Selectors.Contains(
							Entry.GetIdentity().GetSelector());
				}))
			{
				return Exact;
			}
			if (const FUeAgentStateEntry* Owner = UniqueMatch(
				[&Owners](const FUeAgentStateEntry& Entry)
				{
					return Owners.Contains(
						Entry.GetIdentity().GetOwnerId());
				}))
			{
				return Owner;
			}
			if (const FUeAgentStateEntry* Selector = UniqueMatch(
				[&Selectors](const FUeAgentStateEntry& Entry)
				{
					return Selectors.Contains(
						Entry.GetIdentity().GetSelector());
				}))
			{
				return Selector;
			}
			if (Entries.Num() == 1)
			{
				return Entries[0];
			}
			bOutAmbiguous = true;
			return nullptr;
		}

		bool TryStringValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FSemanticWorkspaceImportError& OutError)
		{
			OutValue.Reset();
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
				Text,
				Resources,
				OutValue,
				Error))
			{
				SetImportError(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("hjson_import_value_invalid")
						: Error.Code,
					Error.Detail);
				return false;
			}
			return true;
		}

		bool TryStructuralEditValue(
			const FStructuralEdit& Edit,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FSemanticWorkspaceImportError& OutError)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
			auto AddString =
				[&Rows, &Resources, &OutError](
					const FString& Key,
					const FString& Value) -> bool
				{
					if (Value.IsEmpty())
					{
						return true;
					}
					FUeAgentCanonicalValuePtr Canonical;
					if (!TryStringValue(
						Value,
						Resources,
						Canonical,
						OutError))
					{
						return false;
					}
					Rows.Emplace(Key, Canonical.ToSharedRef());
					return true;
				};
			if (!AddString(TEXT("edit_id"), Edit.GetEditId())
				|| !AddString(TEXT("kind"), LexToString(Edit.GetKind()))
				|| !AddString(
					TEXT("owner_unit_id"),
					Edit.GetOwnerUnitId())
				|| !AddString(
					TEXT("subject_unit_id"),
					Edit.GetSubjectUnitId())
				|| !AddString(
					TEXT("replacement_unit_id"),
					Edit.GetReplacementUnitId())
				|| !AddString(
					TEXT("relation_id"),
					Edit.GetRelationId()))
			{
				return false;
			}
			if (!Edit.GetOrderedUnitIds().IsEmpty())
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const FString& UnitId : Edit.GetOrderedUnitIds())
				{
					FUeAgentCanonicalValuePtr Item;
					if (!TryStringValue(
						UnitId,
						Resources,
						Item,
						OutError))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr Ordered;
				FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					Ordered,
					Error))
				{
					SetImportError(
						OutError,
						Error.Code.IsEmpty()
							? TEXT("hjson_import_value_invalid")
							: Error.Code,
						Error.Detail);
					return false;
				}
				Rows.Emplace(
					TEXT("ordered_unit_ids"),
					Ordered.ToSharedRef());
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
				Rows,
				Resources,
				OutValue,
				Error))
			{
				SetImportError(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("hjson_import_value_invalid")
						: Error.Code,
					Error.Detail);
				return false;
			}
			return true;
		}
	}

	bool FSemanticWorkspaceImportError::IsPresent() const
	{
		return !Code.IsEmpty();
	}

	void FSemanticWorkspaceImportError::Reset()
	{
		Code.Reset();
		Detail.Reset();
	}

	FPreparedAuthoringImport::FPreparedAuthoringImport(
		FResolvedSemanticWorkspaceRef InWorkspace,
		FChangeSetDraft InChangeSetDraft,
		FPlanningRequest InPlanningRequest,
		FPlanningResult InPreflightResult)
		: Workspace(MoveTemp(InWorkspace))
		, ChangeSetDraft(MoveTemp(InChangeSetDraft))
		, PlanningRequest(MoveTemp(InPlanningRequest))
		, PreflightResult(MoveTemp(InPreflightResult))
	{
		bValid = Workspace->IsValid()
			&& ChangeSetDraft.IsValid()
			&& PlanningRequest.IsValid()
			&& PreflightResult.GetPlan().IsValid();
	}

	const FResolvedSemanticWorkspaceRef&
	FPreparedAuthoringImport::GetWorkspace() const
	{
		return Workspace;
	}

	const FChangeSetDraft&
	FPreparedAuthoringImport::GetChangeSetDraft() const
	{
		return ChangeSetDraft;
	}

	const FPlanningRequest&
	FPreparedAuthoringImport::GetPlanningRequest() const
	{
		return PlanningRequest;
	}

	const FPlanningResult&
	FPreparedAuthoringImport::GetPreflightResult() const
	{
		return PreflightResult;
	}

	const TArray<FStructuralEdit>&
	FPreparedAuthoringImport::GetStructuralEdits() const
	{
		return Workspace->GetStructuralEdits();
	}

	bool FPreparedAuthoringImport::IsValid() const
	{
		return bValid;
	}

	bool TryBuildSemanticChangeSetDraft(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FSemanticWorkspaceImportError& OutError)
	{
		OutDraft.Reset();
		OutError.Reset();
		RequestId.TrimStartAndEndInline();
		IdempotencyKey.TrimStartAndEndInline();
		if (!Workspace->IsValid()
			|| !CurrentSnapshot->GetRevision().IsValid()
			|| !DescriptorIdentity.IsValid()
			|| RequestId.IsEmpty()
			|| IdempotencyKey.IsEmpty())
		{
			SetImportError(
				OutError,
				TEXT("hjson_import_change_set_request_invalid"));
			return false;
		}
		if (Workspace->GetBaseStateRevision()
			!= CurrentSnapshot->GetRevision())
		{
			SetImportError(
				OutError,
				TEXT("hjson_import_stale_state_revision"),
				TEXT(
					"workspace base revision differs from the "
					"independent persisted snapshot"));
			return false;
		}

		TMap<FString, TArray<const FUeAgentStateEntry*>>
			EntriesBySemanticId;
		for (const FUeAgentStateEntry& Entry
			: CurrentSnapshot->GetEntries())
		{
			EntriesBySemanticId.FindOrAdd(
				Entry.GetIdentity().GetSemanticId()).Add(&Entry);
		}

		TArray<FChangeDraft> Changes;
		const FAuthoringModelRef& Model = Workspace->GetModel();
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			if (Unit.GetUnitKind()
				!= ESemanticUnitKind::PropertySet)
			{
				continue;
			}
			TArray<FString> FieldIds;
			Unit.GetValues().GetKeys(FieldIds);
			FieldIds.Sort();
			for (const FString& FieldId : FieldIds)
			{
				const FUeAgentCanonicalValueRef& RawValue =
					Unit.GetValues().FindChecked(FieldId);
				bool bSkip = false;
				FUeAgentCanonicalValuePtr Proposed;
				if (!TryResolvePropertyValue(
					FieldId,
					Unit.GetUnitId(),
					RawValue,
					bSkip,
					Proposed,
					OutError))
				{
					return false;
				}
				if (bSkip)
				{
					continue;
				}
				const TArray<const FUeAgentStateEntry*>* Candidates =
					EntriesBySemanticId.Find(FieldId);
				bool bAmbiguous = false;
				const FUeAgentStateEntry* Current = Candidates
					? MatchPropertyEntry(
						*Candidates,
						Unit,
						FieldId,
						Model,
						bAmbiguous)
					: nullptr;
				if (!Current)
				{
					SetImportError(
						OutError,
						bAmbiguous
							? TEXT(
								"hjson_import_property_target_"
								"ambiguous")
							: TEXT(
								"hjson_import_property_target_"
								"missing"),
						Unit.GetUnitId() + TEXT(":") + FieldId);
					return false;
				}
				FUeAgentSemanticComparison Comparison;
				FUeAgentStateSemanticError SemanticError;
				if (!TryCompareValues(
					Current->GetValue(),
					Proposed.ToSharedRef(),
					TEXT("/") + FieldId,
					Resources,
					Comparison,
					SemanticError))
				{
					SetImportError(
						OutError,
						SemanticError.Code.IsEmpty()
							? TEXT("hjson_import_value_compare_failed")
							: SemanticError.Code,
						SemanticError.Detail);
					return false;
				}
				if (Comparison.IsEquivalent())
				{
					continue;
				}
				Changes.Emplace(
					Current->GetIdentity(),
					EChangeIntent::Update,
					Current->GetValue(),
					Proposed,
					TArray<FString>({TEXT("authoring.write")}),
					CurrentSnapshot->GetProvenance().GetEvidenceId(),
					TArray<FUeAgentCanonicalIdentity>());
			}
		}

		for (const FStructuralEdit& Edit
			: Workspace->GetStructuralEdits())
		{
			FUeAgentCanonicalValuePtr Proposed;
			if (!TryStructuralEditValue(
				Edit,
				Resources,
				Proposed,
				OutError))
			{
				return false;
			}
			Changes.Emplace(
				FUeAgentCanonicalIdentity(
					TEXT("structural-edit:") + Edit.GetEditId(),
					TEXT("authoring_structural_edit"),
					Edit.GetOwnerUnitId(),
					LexToString(Edit.GetKind()),
					TEXT("authoring_structure"),
					CurrentSnapshot->GetRevision().GetGeneration()),
				EChangeIntent::Create,
				FUeAgentCanonicalValuePtr(),
				Proposed,
				TArray<FString>({TEXT("authoring.structure")}),
				CurrentSnapshot->GetProvenance().GetEvidenceId(),
				TArray<FUeAgentCanonicalIdentity>());
		}
		if (Changes.IsEmpty())
		{
			SetImportError(
				OutError,
				TEXT("hjson_import_no_explicit_changes"),
				TEXT(
					"workspace omission and unchanged values are "
					"not mutations"));
			return false;
		}
		Changes.Sort(
			[](const FChangeDraft& Left, const FChangeDraft& Right)
			{
				return Left.GetIdentity().GetCanonicalKey()
					< Right.GetIdentity().GetCanonicalKey();
			});
		OutDraft.Emplace(
			MoveTemp(DescriptorIdentity),
			MoveTemp(RequestId),
			MoveTemp(IdempotencyKey),
			CurrentSnapshot,
			MoveTemp(Changes));
		if (!OutDraft->IsValid())
		{
			OutDraft.Reset();
			SetImportError(
				OutError,
				TEXT("hjson_import_change_set_invalid"));
			return false;
		}
		return true;
	}

	bool TryPrepareResolvedAuthoringImport(
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
		FSemanticWorkspaceImportError& OutError)
	{
		OutPrepared.Reset();
		OutError.Reset();
		if (!ChangeSetDraft.IsValid()
			|| !(ChangeSetDraft.GetSnapshot()
				->GetRevision()
				== CurrentSnapshot->GetRevision()))
		{
			OutError.Code =
				TEXT("hjson_import_change_set_snapshot_mismatch");
			OutError.Detail =
				TEXT("draft must bind the current persisted snapshot");
			return false;
		}

		FWorkspaceError WorkspaceError;
		if (!TryValidateSemanticWorkspaceBinding(
			Workspace,
			Workspace->GetMode(),
			CurrentModel,
			CurrentSnapshot->GetRevision(),
			ExpectedSelectionFingerprint,
			ExpectedClosureFingerprint,
			WorkspaceError))
		{
			if (WorkspaceError.GetCode()
				== TEXT(
					"semantic_workspace_base_revision_mismatch"))
			{
				OutError.Code =
					TEXT("hjson_import_stale_base_revision");
			}
			else if (WorkspaceError.GetCode()
				== TEXT(
					"semantic_workspace_state_revision_mismatch"))
			{
				OutError.Code =
					TEXT("hjson_import_stale_state_revision");
			}
			else if (WorkspaceError.GetCode()
				== TEXT("semantic_workspace_closure_mismatch"))
			{
				OutError.Code =
					TEXT("hjson_import_closure_mismatch");
			}
			else
			{
				OutError.Code = WorkspaceError.GetCode();
			}
			OutError.Detail =
				TEXT("semantic workspace binding is not current");
			return false;
		}

		FPlanningRequest PlanningRequest(
			ChangeSetDraft,
			CallerContext,
			bDryRun);
		FPlanningResult Result;
		FPlanningPreflightError PlanningError;
		if (!TryPlanChanges(
			PlanningRequest,
			Resources,
			Result,
			PlanningError))
		{
			OutError.Code = PlanningError.GetCode();
			OutError.Detail = PlanningError.GetDetail();
			return false;
		}
		OutPrepared = MakeShared<FPreparedAuthoringImport>(
			Workspace,
			ChangeSetDraft,
			MoveTemp(PlanningRequest),
			MoveTemp(Result));
		if (!OutPrepared->IsValid())
		{
			OutPrepared.Reset();
			OutError.Code =
				TEXT("hjson_import_prepared_contract_invalid");
			return false;
		}
		return true;
	}

	bool TryPrepareSemanticWorkspaceImport(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		const FUeAgentCallerContext& CallerContext,
		bool bDryRun,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FUeAgentResourceTracker& Resources,
		FPreparedAuthoringImportPtr& OutPrepared,
		FSemanticWorkspaceImportError& OutError)
	{
		OutPrepared.Reset();
		OutError.Reset();
		FResolvedSemanticWorkspacePtr Workspace;
		FWorkspaceError WorkspaceError;
		if (!TryResolveSemanticWorkspace(
			Source,
			ExpectedMode,
			Resources,
			Workspace,
			WorkspaceError)
			|| !Workspace.IsValid())
		{
			SetImportError(
				OutError,
				WorkspaceError.GetCode().IsEmpty()
					? TEXT("hjson_import_workspace_resolve_failed")
					: WorkspaceError.GetCode(),
				WorkspaceError.GetSourceUnit());
			return false;
		}
		TOptional<FChangeSetDraft> Draft;
		if (!TryBuildSemanticChangeSetDraft(
			Workspace.ToSharedRef(),
			CurrentSnapshot,
			MoveTemp(DescriptorIdentity),
			MoveTemp(RequestId),
			MoveTemp(IdempotencyKey),
			Resources,
			Draft,
			OutError)
			|| !Draft.IsSet())
		{
			return false;
		}
		return TryPrepareResolvedAuthoringImport(
			Workspace.ToSharedRef(),
			CurrentModel,
			CurrentSnapshot,
			Draft.GetValue(),
			CallerContext,
			bDryRun,
			ExpectedSelectionFingerprint,
			ExpectedClosureFingerprint,
			Resources,
			OutPrepared,
			OutError);
	}
}
