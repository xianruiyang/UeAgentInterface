#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"

namespace UE::UAI::HjsonWorkspace
{
	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool NormalizeUniqueSet(TArray<FString>& Values)
		{
			for (FString& Value : Values)
			{
				Value = Normalize(MoveTemp(Value));
				if (Value.IsEmpty())
				{
					return false;
				}
			}
			Values.Sort();
			for (int32 Index = 1; Index < Values.Num(); ++Index)
			{
				if (Values[Index - 1] == Values[Index])
				{
					return false;
				}
			}
			return true;
		}

		bool NormalizeUniqueSequence(TArray<FString>& Values)
		{
			TSet<FString> Seen;
			for (FString& Value : Values)
			{
				Value = Normalize(MoveTemp(Value));
				if (Value.IsEmpty() || Seen.Contains(Value))
				{
					return false;
				}
				Seen.Add(Value);
			}
			return true;
		}

		bool SameContract(
			const FAuthoringModel& Left,
			const FAuthoringModel& Right)
		{
			return Left.GetContractId() == Right.GetContractId()
				&& Left.GetContractVersion()
					== Right.GetContractVersion()
				&& Left.GetProfileId() == Right.GetProfileId()
				&& Left.GetBaseRevision()
					== Right.GetBaseRevision();
		}
	}

	FString LexToString(ESemanticWorkspaceMode Mode)
	{
		switch (Mode)
		{
		case ESemanticWorkspaceMode::State:
			return TEXT("state");
		case ESemanticWorkspaceMode::Seed:
			return TEXT("seed");
		case ESemanticWorkspaceMode::EditBundle:
			return TEXT("edit_bundle");
		default:
			return FString();
		}
	}

	FString LexToString(EStructuralEditKind Kind)
	{
		switch (Kind)
		{
		case EStructuralEditKind::Add:
			return TEXT("add");
		case EStructuralEditKind::Remove:
			return TEXT("remove");
		case EStructuralEditKind::Replace:
			return TEXT("replace");
		case EStructuralEditKind::Rename:
			return TEXT("rename");
		case EStructuralEditKind::AddRelation:
			return TEXT("add_relation");
		case EStructuralEditKind::RemoveRelation:
			return TEXT("remove_relation");
		case EStructuralEditKind::SetOrder:
			return TEXT("set_order");
		default:
			return FString();
		}
	}

	FStructuralEdit::FStructuralEdit(
		FString InEditId,
		EStructuralEditKind InKind,
		FString InOwnerUnitId,
		FString InSubjectUnitId,
		FString InReplacementUnitId,
		FString InRelationId,
		TArray<FString> InOrderedUnitIds)
		: EditId(Normalize(MoveTemp(InEditId)))
		, Kind(InKind)
		, OwnerUnitId(Normalize(MoveTemp(InOwnerUnitId)))
		, SubjectUnitId(Normalize(MoveTemp(InSubjectUnitId)))
		, ReplacementUnitId(
			Normalize(MoveTemp(InReplacementUnitId)))
		, RelationId(Normalize(MoveTemp(InRelationId)))
		, OrderedUnitIds(MoveTemp(InOrderedUnitIds))
	{
		const bool bOrderIsValid =
			NormalizeUniqueSequence(OrderedUnitIds);
		const bool bNeedsSubject =
			Kind == EStructuralEditKind::Add
			|| Kind == EStructuralEditKind::Remove
			|| Kind == EStructuralEditKind::Rename;
		const bool bNeedsRelation =
			Kind == EStructuralEditKind::AddRelation
			|| Kind == EStructuralEditKind::RemoveRelation;
		bValid = !EditId.IsEmpty()
			&& !OwnerUnitId.IsEmpty()
			&& !LexToString(Kind).IsEmpty()
			&& (!bNeedsSubject || !SubjectUnitId.IsEmpty())
			&& (Kind != EStructuralEditKind::Replace
				|| (!SubjectUnitId.IsEmpty()
					&& !ReplacementUnitId.IsEmpty()))
			&& (!bNeedsRelation || !RelationId.IsEmpty())
			&& (Kind != EStructuralEditKind::SetOrder
				|| (!OrderedUnitIds.IsEmpty() && bOrderIsValid))
			&& (Kind == EStructuralEditKind::SetOrder
				|| OrderedUnitIds.IsEmpty());
	}

	const FString& FStructuralEdit::GetEditId() const
	{
		return EditId;
	}

	EStructuralEditKind FStructuralEdit::GetKind() const
	{
		return Kind;
	}

	const FString& FStructuralEdit::GetOwnerUnitId() const
	{
		return OwnerUnitId;
	}

	const FString& FStructuralEdit::GetSubjectUnitId() const
	{
		return SubjectUnitId;
	}

	const FString& FStructuralEdit::GetReplacementUnitId() const
	{
		return ReplacementUnitId;
	}

	const FString& FStructuralEdit::GetRelationId() const
	{
		return RelationId;
	}

	const TArray<FString>& FStructuralEdit::GetOrderedUnitIds() const
	{
		return OrderedUnitIds;
	}

	bool FStructuralEdit::IsValid() const
	{
		return bValid;
	}

	FResolvedSemanticWorkspace::FResolvedSemanticWorkspace(
		EWorkspaceSourceKind InSourceKind,
		ESemanticWorkspaceMode InMode,
		FAuthoringModelRef InModel,
		FUeAgentStateRevision InBaseStateRevision,
		TArray<FStructuralEdit> InStructuralEdits,
		TArray<FString> InManagedUnitPaths,
		FString InSelectionFingerprint,
		FString InClosureFingerprint,
		TArray<FString> InClosureUnitIds,
		FString InWorkspaceFingerprint)
		: SourceKind(InSourceKind)
		, Mode(InMode)
		, Model(MoveTemp(InModel))
		, BaseStateRevision(MoveTemp(InBaseStateRevision))
		, StructuralEdits(MoveTemp(InStructuralEdits))
		, ManagedUnitPaths(MoveTemp(InManagedUnitPaths))
		, SelectionFingerprint(
			Normalize(MoveTemp(InSelectionFingerprint)))
		, ClosureFingerprint(
			Normalize(MoveTemp(InClosureFingerprint)))
		, ClosureUnitIds(MoveTemp(InClosureUnitIds))
		, WorkspaceFingerprint(
			Normalize(MoveTemp(InWorkspaceFingerprint)))
	{
		const bool bPathsAreValid =
			NormalizeUniqueSet(ManagedUnitPaths);
		const bool bClosureIsValid =
			NormalizeUniqueSet(ClosureUnitIds);
		bool bEditsAreValid = true;
		TSet<FString> EditIds;
		for (const FStructuralEdit& Edit : StructuralEdits)
		{
			if (!Edit.IsValid()
				|| EditIds.Contains(Edit.GetEditId()))
			{
				bEditsAreValid = false;
				break;
			}
			EditIds.Add(Edit.GetEditId());
		}
		const bool bBundleBindingIsValid =
			Mode != ESemanticWorkspaceMode::EditBundle
			|| (!SelectionFingerprint.IsEmpty()
				&& !ClosureFingerprint.IsEmpty()
				&& !ClosureUnitIds.IsEmpty()
				&& bClosureIsValid);
		bValid = !LexToString(Mode).IsEmpty()
			&& !Model->GetSemanticFingerprint().IsEmpty()
			&& !WorkspaceFingerprint.IsEmpty()
			&& bPathsAreValid
			&& bEditsAreValid
			&& bBundleBindingIsValid;
	}

	EWorkspaceSourceKind
	FResolvedSemanticWorkspace::GetSourceKind() const
	{
		return SourceKind;
	}

	ESemanticWorkspaceMode
	FResolvedSemanticWorkspace::GetMode() const
	{
		return Mode;
	}

	const FAuthoringModelRef&
	FResolvedSemanticWorkspace::GetModel() const
	{
		return Model;
	}

	const FUeAgentStateRevision&
	FResolvedSemanticWorkspace::GetBaseStateRevision() const
	{
		return BaseStateRevision;
	}

	const TArray<FStructuralEdit>&
	FResolvedSemanticWorkspace::GetStructuralEdits() const
	{
		return StructuralEdits;
	}

	const TArray<FString>&
	FResolvedSemanticWorkspace::GetManagedUnitPaths() const
	{
		return ManagedUnitPaths;
	}

	const FString&
	FResolvedSemanticWorkspace::GetSelectionFingerprint() const
	{
		return SelectionFingerprint;
	}

	const FString&
	FResolvedSemanticWorkspace::GetClosureFingerprint() const
	{
		return ClosureFingerprint;
	}

	const TArray<FString>&
	FResolvedSemanticWorkspace::GetClosureUnitIds() const
	{
		return ClosureUnitIds;
	}

	const FString&
	FResolvedSemanticWorkspace::GetWorkspaceFingerprint() const
	{
		return WorkspaceFingerprint;
	}

	bool FResolvedSemanticWorkspace::IsValid() const
	{
		return bValid;
	}

	bool TryValidateSemanticWorkspaceBinding(
		const FResolvedSemanticWorkspaceRef& Workspace,
		ESemanticWorkspaceMode ExpectedMode,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentStateRevision& CurrentRevision,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FWorkspaceError& OutError)
	{
		OutError.Reset();
		if (!Workspace->IsValid())
		{
			OutError.Set(
				TEXT("semantic_workspace_invalid"));
			return false;
		}
		if (Workspace->GetMode() != ExpectedMode)
		{
			OutError.Set(
				TEXT("semantic_workspace_mode_mismatch"));
			return false;
		}
		if (!SameContract(
			*Workspace->GetModel(),
			*CurrentModel))
		{
			OutError.Set(
				TEXT("semantic_workspace_base_revision_mismatch"));
			return false;
		}
		if (!(Workspace->GetBaseStateRevision() == CurrentRevision))
		{
			OutError.Set(
				TEXT("semantic_workspace_state_revision_mismatch"));
			return false;
		}
		if (ExpectedMode == ESemanticWorkspaceMode::EditBundle)
		{
			if (Workspace->GetSelectionFingerprint()
					!= Normalize(ExpectedSelectionFingerprint)
				|| Workspace->GetClosureFingerprint()
					!= Normalize(ExpectedClosureFingerprint))
			{
				OutError.Set(
					TEXT("semantic_workspace_closure_mismatch"));
				return false;
			}
		}
		return true;
	}
}
