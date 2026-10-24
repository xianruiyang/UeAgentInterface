#include "Core/HjsonWorkspace/UeAgentEditBundleSelection.h"

#include "Containers/StringConv.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	namespace
	{
		const FString BundleSchema =
			TEXT("uai.authoring.edit_bundle/1");
		const FString BundleProfile = TEXT("llm_edit_bundle");

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool NormalizeSet(TArray<FString>& Values)
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

		bool NormalizeMap(TMap<FString, TArray<FString>>& Values)
		{
			TMap<FString, TArray<FString>> Normalized;
			for (TPair<FString, TArray<FString>>& Pair : Values)
			{
				FString Key = Normalize(Pair.Key);
				if (
					Key.IsEmpty()
					|| Normalized.Contains(Key)
					|| !NormalizeSet(Pair.Value))
				{
					return false;
				}
				Normalized.Add(MoveTemp(Key), MoveTemp(Pair.Value));
			}
			Values = MoveTemp(Normalized);
			return true;
		}

		void AppendToken(
			FString& Material,
			const TCHAR* Prefix,
			const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value, Value.Len());
			Material.Append(Prefix);
			Material.AppendInt(Utf8.Length());
			Material.AppendChar(TEXT(':'));
			Material.Append(Value);
			Material.AppendChar(TEXT(';'));
		}

		FString Sha256(const FString& Material)
		{
			const FTCHARToUTF8 Utf8(*Material, Material.Len());
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Utf8.Get()),
				static_cast<size_t>(Utf8.Length()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		bool IsSha256(const FString& Value)
		{
			if (
				Value.Len() != 71
				|| !Value.StartsWith(TEXT("sha256:")))
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

		FString PolicyMaterial(
			const FString& ContractId,
			int32 ContractVersion,
			const FString& PolicyId,
			const TMap<FString, TArray<FString>>& Controlling,
			const TMap<FString, TArray<FString>>& Constraints,
			const TArray<FString>& Roles,
			const TArray<ESemanticUnitKind>& Placements)
		{
			FString Material;
			AppendToken(Material, TEXT("c"), ContractId);
			Material.AppendInt(ContractVersion);
			AppendToken(Material, TEXT("p"), PolicyId);
			TArray<FString> Keys;
			Controlling.GetKeys(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				AppendToken(Material, TEXT("f"), Key);
				for (const FString& Value : Controlling.FindChecked(Key))
				{
					AppendToken(Material, TEXT("v"), Value);
				}
			}
			Keys.Reset();
			Constraints.GetKeys(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				AppendToken(Material, TEXT("d"), Key);
				for (const FString& Value : Constraints.FindChecked(Key))
				{
					AppendToken(Material, TEXT("v"), Value);
				}
			}
			for (const FString& Role : Roles)
			{
				AppendToken(Material, TEXT("r"), Role);
			}
			for (ESemanticUnitKind Kind : Placements)
			{
				Material.AppendInt(static_cast<int32>(Kind));
				Material.AppendChar(TEXT(';'));
			}
			return Material;
		}

		void SetSelectionError(
			FEditBundleSelectionError& OutError,
			const TCHAR* Code,
			FString Subject = FString(),
			FString Detail = FString())
		{
			OutError.Code = Code;
			OutError.SubjectId = MoveTemp(Subject);
			OutError.InternalContext = MoveTemp(Detail);
		}

		void AppendValues(
			FString& Material,
			const TCHAR* Prefix,
			const TArray<FString>& Values)
		{
			for (const FString& Value : Values)
			{
				AppendToken(Material, Prefix, Value);
			}
		}

		bool ContainsKind(
			const TArray<ESemanticUnitKind>& Values,
			const ESemanticUnitKind Kind)
		{
			return Values.Contains(Kind);
		}
	}

	FString LexToString(EWorkspaceSelectionKind Kind)
	{
		switch (Kind)
		{
		case EWorkspaceSelectionKind::Property:
			return TEXT("property");
		case EWorkspaceSelectionKind::GraphItem:
			return TEXT("graph_item");
		case EWorkspaceSelectionKind::Connection:
			return TEXT("connection");
		case EWorkspaceSelectionKind::StructuralAdd:
			return TEXT("structural_add");
		case EWorkspaceSelectionKind::TreeItem:
			return TEXT("tree_item");
		case EWorkspaceSelectionKind::TimelineItem:
			return TEXT("timeline_item");
		case EWorkspaceSelectionKind::SpatialLayout:
			return TEXT("spatial_layout");
		default:
			return FString();
		}
	}

	FWorkspaceSelection::FWorkspaceSelection(
		FString InSelectionId,
		EWorkspaceSelectionKind InSelectionKind,
		TArray<FString> InSelectedUnitIds,
		TArray<FString> InSelectedFieldIds,
		TArray<FString> InSelectedRelationIds,
		FString InCandidateBindingId)
		: SelectionId(Normalize(MoveTemp(InSelectionId)))
		, SelectionKind(InSelectionKind)
		, SelectedUnitIds(MoveTemp(InSelectedUnitIds))
		, SelectedFieldIds(MoveTemp(InSelectedFieldIds))
		, SelectedRelationIds(MoveTemp(InSelectedRelationIds))
		, CandidateBindingId(
			Normalize(MoveTemp(InCandidateBindingId)))
	{
		bValid =
			!SelectionId.IsEmpty()
			&& !LexToString(SelectionKind).IsEmpty()
			&& NormalizeSet(SelectedUnitIds)
			&& NormalizeSet(SelectedFieldIds)
			&& NormalizeSet(SelectedRelationIds)
			&& (
				SelectedUnitIds.Num() > 0
				|| SelectedFieldIds.Num() > 0
				|| SelectedRelationIds.Num() > 0);
		if (
			SelectionKind == EWorkspaceSelectionKind::Property
			&& SelectedFieldIds.IsEmpty())
		{
			bValid = false;
		}
		if (
			SelectionKind == EWorkspaceSelectionKind::Connection
			&& SelectedRelationIds.IsEmpty())
		{
			bValid = false;
		}
		if (
			SelectionKind == EWorkspaceSelectionKind::StructuralAdd
			&& (
				SelectedUnitIds.IsEmpty()
				|| CandidateBindingId.IsEmpty()))
		{
			bValid = false;
		}
	}

	const FString& FWorkspaceSelection::GetSelectionId() const
	{
		return SelectionId;
	}

	EWorkspaceSelectionKind
	FWorkspaceSelection::GetSelectionKind() const
	{
		return SelectionKind;
	}

	const TArray<FString>&
	FWorkspaceSelection::GetSelectedUnitIds() const
	{
		return SelectedUnitIds;
	}

	const TArray<FString>&
	FWorkspaceSelection::GetSelectedFieldIds() const
	{
		return SelectedFieldIds;
	}

	const TArray<FString>&
	FWorkspaceSelection::GetSelectedRelationIds() const
	{
		return SelectedRelationIds;
	}

	const FString&
	FWorkspaceSelection::GetCandidateBindingId() const
	{
		return CandidateBindingId;
	}

	bool FWorkspaceSelection::IsValid() const
	{
		return bValid;
	}

	FSelectionPolicy::FSelectionPolicy(
		FString InContractId,
		int32 InContractVersion,
		FString InPolicyId,
		TMap<FString, TArray<FString>> InControllingFieldIds,
		TMap<FString, TArray<FString>> InDynamicConstraintUnitIds,
		TArray<FString> InInterfaceRoles,
		TArray<ESemanticUnitKind> InPlacementUnitKinds)
		: ContractId(Normalize(MoveTemp(InContractId)))
		, ContractVersion(InContractVersion)
		, PolicyId(Normalize(MoveTemp(InPolicyId)))
		, ControllingFieldIds(MoveTemp(InControllingFieldIds))
		, DynamicConstraintUnitIds(
			MoveTemp(InDynamicConstraintUnitIds))
		, InterfaceRoles(MoveTemp(InInterfaceRoles))
		, PlacementUnitKinds(MoveTemp(InPlacementUnitKinds))
	{
		bValid =
			!ContractId.IsEmpty()
			&& ContractVersion > 0
			&& !PolicyId.IsEmpty()
			&& NormalizeMap(ControllingFieldIds)
			&& NormalizeMap(DynamicConstraintUnitIds)
			&& NormalizeSet(InterfaceRoles);
		PlacementUnitKinds.Sort(
			[](ESemanticUnitKind Left, ESemanticUnitKind Right)
			{
				return static_cast<uint8>(Left)
					< static_cast<uint8>(Right);
			});
		for (int32 Index = 1;
			Index < PlacementUnitKinds.Num();
			++Index)
		{
			if (
				PlacementUnitKinds[Index - 1]
				== PlacementUnitKinds[Index])
			{
				bValid = false;
			}
		}
		if (bValid)
		{
			PolicyFingerprint = Sha256(
				PolicyMaterial(
					ContractId,
					ContractVersion,
					PolicyId,
					ControllingFieldIds,
					DynamicConstraintUnitIds,
					InterfaceRoles,
					PlacementUnitKinds));
		}
	}

	const FString& FSelectionPolicy::GetContractId() const
	{
		return ContractId;
	}

	int32 FSelectionPolicy::GetContractVersion() const
	{
		return ContractVersion;
	}

	const FString& FSelectionPolicy::GetPolicyId() const
	{
		return PolicyId;
	}

	const TMap<FString, TArray<FString>>&
	FSelectionPolicy::GetControllingFieldIds() const
	{
		return ControllingFieldIds;
	}

	const TMap<FString, TArray<FString>>&
	FSelectionPolicy::GetDynamicConstraintUnitIds() const
	{
		return DynamicConstraintUnitIds;
	}

	const TArray<FString>& FSelectionPolicy::GetInterfaceRoles() const
	{
		return InterfaceRoles;
	}

	const TArray<ESemanticUnitKind>&
	FSelectionPolicy::GetPlacementUnitKinds() const
	{
		return PlacementUnitKinds;
	}

	const FString& FSelectionPolicy::GetPolicyFingerprint() const
	{
		return PolicyFingerprint;
	}

	bool FSelectionPolicy::IsValid() const
	{
		return bValid;
	}

	FSelectionClosure::FSelectionClosure(
		FWorkspaceSelection InSelection,
		TArray<FString> InUnitIds,
		TArray<FString> InFieldIds,
		TArray<FString> InRelationIds,
		FString InSelectionFingerprint,
		FString InClosureFingerprint)
		: Selection(MoveTemp(InSelection))
		, UnitIds(MoveTemp(InUnitIds))
		, FieldIds(MoveTemp(InFieldIds))
		, RelationIds(MoveTemp(InRelationIds))
		, SelectionFingerprint(
			Normalize(MoveTemp(InSelectionFingerprint)))
		, ClosureFingerprint(
			Normalize(MoveTemp(InClosureFingerprint)))
	{
		bValid =
			Selection.IsValid()
			&& NormalizeSet(UnitIds)
			&& NormalizeSet(FieldIds)
			&& NormalizeSet(RelationIds)
			&& UnitIds.Num() > 0
			&& IsSha256(SelectionFingerprint)
			&& IsSha256(ClosureFingerprint);
	}

	const FWorkspaceSelection&
	FSelectionClosure::GetSelection() const
	{
		return Selection;
	}

	const TArray<FString>& FSelectionClosure::GetUnitIds() const
	{
		return UnitIds;
	}

	const TArray<FString>& FSelectionClosure::GetFieldIds() const
	{
		return FieldIds;
	}

	const TArray<FString>& FSelectionClosure::GetRelationIds() const
	{
		return RelationIds;
	}

	const FString&
	FSelectionClosure::GetSelectionFingerprint() const
	{
		return SelectionFingerprint;
	}

	const FString& FSelectionClosure::GetClosureFingerprint() const
	{
		return ClosureFingerprint;
	}

	bool FSelectionClosure::IsValid() const
	{
		return bValid;
	}

	FScopedEditBundle::FScopedEditBundle(
		FString InWorkspaceSchema,
		FString InProfileId,
		FString InContractId,
		int32 InContractVersion,
		FString InBaseRevision,
		FString InBaseSemanticFingerprint,
		FString InSelectionFingerprint,
		FString InClosureFingerprint,
		FString InSourceSnapshotId,
		TMap<FString, FSemanticUnit> InUnitsById,
		TArray<FUeAgentStateEntry> InFieldStates,
		TMap<FString, FExportFieldDescription>
			InFieldDescriptionsById,
		TMap<FString, FString> InLogicalPlacements,
		TOptional<FUeAgentCandidateBinding> InCandidateBinding,
		FString InBundleFingerprint)
		: WorkspaceSchema(Normalize(MoveTemp(InWorkspaceSchema)))
		, ProfileId(Normalize(MoveTemp(InProfileId)))
		, ContractId(Normalize(MoveTemp(InContractId)))
		, ContractVersion(InContractVersion)
		, BaseRevision(Normalize(MoveTemp(InBaseRevision)))
		, BaseSemanticFingerprint(
			Normalize(MoveTemp(InBaseSemanticFingerprint)))
		, SelectionFingerprint(
			Normalize(MoveTemp(InSelectionFingerprint)))
		, ClosureFingerprint(
			Normalize(MoveTemp(InClosureFingerprint)))
		, SourceSnapshotId(
			Normalize(MoveTemp(InSourceSnapshotId)))
		, UnitsById(MoveTemp(InUnitsById))
		, FieldStates(MoveTemp(InFieldStates))
		, FieldDescriptionsById(
			MoveTemp(InFieldDescriptionsById))
		, LogicalPlacements(MoveTemp(InLogicalPlacements))
		, CandidateBinding(MoveTemp(InCandidateBinding))
		, BundleFingerprint(
			Normalize(MoveTemp(InBundleFingerprint)))
	{
		bValid =
			WorkspaceSchema == BundleSchema
			&& ProfileId == BundleProfile
			&& !ContractId.IsEmpty()
			&& ContractVersion > 0
			&& !BaseRevision.IsEmpty()
			&& IsSha256(BaseSemanticFingerprint)
			&& IsSha256(SelectionFingerprint)
			&& IsSha256(ClosureFingerprint)
			&& !SourceSnapshotId.IsEmpty()
			&& IsSha256(BundleFingerprint)
			&& (
				!CandidateBinding.IsSet()
				|| CandidateBinding->IsValid());
		for (const TPair<FString, FSemanticUnit>& Pair : UnitsById)
		{
			if (
				Normalize(Pair.Key) != Pair.Key
				|| Pair.Key != Pair.Value.GetUnitId()
				|| !Pair.Value.IsValid())
			{
				bValid = false;
			}
		}
		for (const TPair<FString, FString>& Pair : LogicalPlacements)
		{
			if (
				Normalize(Pair.Key).IsEmpty()
				|| Normalize(Pair.Value).IsEmpty())
			{
				bValid = false;
			}
		}
	}

	const FString& FScopedEditBundle::GetWorkspaceSchema() const
	{
		return WorkspaceSchema;
	}

	const FString& FScopedEditBundle::GetProfileId() const
	{
		return ProfileId;
	}

	const FString& FScopedEditBundle::GetContractId() const
	{
		return ContractId;
	}

	int32 FScopedEditBundle::GetContractVersion() const
	{
		return ContractVersion;
	}

	const FString& FScopedEditBundle::GetBaseRevision() const
	{
		return BaseRevision;
	}

	const FString&
	FScopedEditBundle::GetBaseSemanticFingerprint() const
	{
		return BaseSemanticFingerprint;
	}

	const FString&
	FScopedEditBundle::GetSelectionFingerprint() const
	{
		return SelectionFingerprint;
	}

	const FString& FScopedEditBundle::GetClosureFingerprint() const
	{
		return ClosureFingerprint;
	}

	const FString& FScopedEditBundle::GetSourceSnapshotId() const
	{
		return SourceSnapshotId;
	}

	const TMap<FString, FSemanticUnit>&
	FScopedEditBundle::GetUnitsById() const
	{
		return UnitsById;
	}

	const TArray<FUeAgentStateEntry>&
	FScopedEditBundle::GetFieldStates() const
	{
		return FieldStates;
	}

	const TMap<FString, FExportFieldDescription>&
	FScopedEditBundle::GetFieldDescriptionsById() const
	{
		return FieldDescriptionsById;
	}

	const TMap<FString, FString>&
	FScopedEditBundle::GetLogicalPlacements() const
	{
		return LogicalPlacements;
	}

	const TOptional<FUeAgentCandidateBinding>&
	FScopedEditBundle::GetCandidateBinding() const
	{
		return CandidateBinding;
	}

	const FString& FScopedEditBundle::GetBundleFingerprint() const
	{
		return BundleFingerprint;
	}

	bool FScopedEditBundle::IsValid() const
	{
		return bValid;
	}

	bool TryResolveSelectionClosure(
		const FAuthoringModelRef& Model,
		const FWorkspaceSelection& Selection,
		const FSelectionPolicy& Policy,
		TOptional<FSelectionClosure>& OutClosure,
		FEditBundleSelectionError& OutError)
	{
		OutClosure.Reset();
		OutError.Reset();
		if (!Selection.IsValid()
			|| !Policy.IsValid())
		{
			SetSelectionError(
				OutError,
				TEXT("edit_bundle_selection_invalid"));
			return false;
		}
		if (Policy.GetContractId() != Model->GetContractId()
			|| Policy.GetContractVersion()
				!= Model->GetContractVersion())
		{
			SetSelectionError(
				OutError,
				TEXT("edit_bundle_selection_contract_mismatch"),
				Policy.GetContractId());
			return false;
		}

		TMap<FString, const FSemanticUnit*> UnitsById;
		TMultiMap<FString, const FSemanticUnit*> ChildrenByOwner;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			UnitsById.Add(Unit.GetUnitId(), &Unit);
			if (!Unit.GetOwnerRef().IsEmpty())
			{
				ChildrenByOwner.Add(Unit.GetOwnerRef(), &Unit);
			}
		}

		TSet<FString> UnitIds;
		TSet<FString> FieldIds;
		TSet<FString> RelationIds;
		TArray<FString> PendingUnits;
		auto QueueUnit =
			[&](
				const FString& UnitId,
				const TCHAR* MissingCode) -> bool
			{
				const FSemanticUnit* const* Found =
					UnitsById.Find(UnitId);
				if (!Found)
				{
					SetSelectionError(
						OutError,
						MissingCode,
						UnitId);
					return false;
				}
				if (!UnitIds.Contains(UnitId))
				{
					UnitIds.Add(UnitId);
					PendingUnits.Add(UnitId);
				}
				return true;
			};

		for (const FString& UnitId :
			Selection.GetSelectedUnitIds())
		{
			if (!QueueUnit(
				UnitId,
				TEXT("edit_bundle_selected_unit_missing")))
			{
				return false;
			}
		}
		for (const FString& RelationId :
			Selection.GetSelectedRelationIds())
		{
			if (!QueueUnit(
				RelationId,
				TEXT("edit_bundle_selected_relation_missing")))
			{
				return false;
			}
			const FSemanticUnit& Relation =
				*UnitsById.FindChecked(RelationId);
			if (Relation.GetUnitKind()
				!= ESemanticUnitKind::RelationSet)
			{
				SetSelectionError(
					OutError,
					TEXT("edit_bundle_selected_relation_invalid"),
					RelationId);
				return false;
			}
			RelationIds.Add(RelationId);
		}

		auto FindFieldUnits =
			[&UnitsById](
				const FString& FieldId,
				const TSet<FString>& PreferredUnitIds)
			{
				TArray<const FSemanticUnit*> Matches;
				for (const TPair<FString, const FSemanticUnit*>& Pair
					: UnitsById)
				{
					const FSemanticUnit& Unit = *Pair.Value;
					if (Unit.GetUnitKind()
							!= ESemanticUnitKind::PropertySet
						|| (Unit.GetFieldSetRef() != FieldId
							&& !Unit.GetValues().Contains(FieldId)))
					{
						continue;
					}
					if (PreferredUnitIds.Contains(Unit.GetUnitId()))
					{
						Matches.Insert(&Unit, 0);
					}
					else
					{
						Matches.Add(&Unit);
					}
				}
				return Matches;
			};

		for (const FString& FieldId :
			Selection.GetSelectedFieldIds())
		{
			TArray<const FSemanticUnit*> Matches =
				FindFieldUnits(FieldId, UnitIds);
			if (Matches.IsEmpty())
			{
				SetSelectionError(
					OutError,
					TEXT("edit_bundle_selected_field_missing"),
					FieldId);
				return false;
			}
			if (Matches.Num() > 1
				&& !UnitIds.Contains(Matches[0]->GetUnitId()))
			{
				SetSelectionError(
					OutError,
					TEXT("edit_bundle_selected_field_ambiguous"),
					FieldId);
				return false;
			}
			FieldIds.Add(FieldId);
			if (!QueueUnit(
				Matches[0]->GetUnitId(),
				TEXT("edit_bundle_selected_field_unit_missing")))
			{
				return false;
			}
		}

		TSet<FString> ConstraintKeySet;
		for (const FString& Key :
			Selection.GetSelectedFieldIds())
		{
			ConstraintKeySet.Add(Key);
		}
		for (const FString& Key :
			Selection.GetSelectedUnitIds())
		{
			ConstraintKeySet.Add(Key);
		}
		TArray<FString> ConstraintKeys = ConstraintKeySet.Array();
		ConstraintKeys.Sort();
		for (const FString& Key : ConstraintKeys)
		{
			if (const TArray<FString>* Constraints =
				Policy.GetDynamicConstraintUnitIds().Find(Key))
			{
				for (const FString& UnitId : *Constraints)
				{
					if (!QueueUnit(
						UnitId,
						TEXT(
							"edit_bundle_dynamic_constraint_missing")))
					{
						return false;
					}
				}
			}
		}

		for (const FString& SelectedField :
			Selection.GetSelectedFieldIds())
		{
			const TArray<FString>* Controls =
				Policy.GetControllingFieldIds().Find(SelectedField);
			if (!Controls)
			{
				continue;
			}
			for (const FString& ControlField : *Controls)
			{
				TArray<const FSemanticUnit*> Matches =
					FindFieldUnits(ControlField, UnitIds);
				if (Matches.Num() != 1)
				{
					SetSelectionError(
						OutError,
						Matches.IsEmpty()
							? TEXT(
								"edit_bundle_controlling_field_missing")
							: TEXT(
								"edit_bundle_controlling_field_ambiguous"),
						ControlField);
					return false;
				}
				FieldIds.Add(ControlField);
				if (!QueueUnit(
					Matches[0]->GetUnitId(),
					TEXT(
						"edit_bundle_controlling_field_unit_missing")))
				{
					return false;
				}
			}
		}

		for (int32 Index = 0; Index < PendingUnits.Num(); ++Index)
		{
			const FSemanticUnit& Unit =
				*UnitsById.FindChecked(PendingUnits[Index]);
			if (!Unit.GetOwnerRef().IsEmpty()
				&& !QueueUnit(
					Unit.GetOwnerRef(),
					TEXT("edit_bundle_owner_missing")))
			{
				return false;
			}
			for (const FString& ItemRef : Unit.GetItemRefs())
			{
				if ((Unit.GetUnitKind()
						== ESemanticUnitKind::RelationSet
					|| Selection.GetSelectionKind()
						== EWorkspaceSelectionKind::Connection)
					&& !QueueUnit(
						ItemRef,
						TEXT(
							"edit_bundle_relation_endpoint_missing")))
				{
					return false;
				}
			}

			TArray<const FSemanticUnit*> Children;
			ChildrenByOwner.MultiFind(Unit.GetUnitId(), Children);
			for (const FSemanticUnit* Child : Children)
			{
				const bool bPlacement =
					ContainsKind(
						Policy.GetPlacementUnitKinds(),
						Child->GetUnitKind());
				const bool bInterface =
					Policy.GetInterfaceRoles().Contains(
						Child->GetRole());
				if ((bPlacement || bInterface)
					&& !QueueUnit(
						Child->GetUnitId(),
						TEXT(
							"edit_bundle_closure_unit_missing")))
				{
					return false;
				}
			}
		}

		TArray<FString> SortedUnitIds = UnitIds.Array();
		TArray<FString> SortedFieldIds = FieldIds.Array();
		TArray<FString> SortedRelationIds = RelationIds.Array();
		SortedUnitIds.Sort();
		SortedFieldIds.Sort();
		SortedRelationIds.Sort();
		if (SortedUnitIds.IsEmpty())
		{
			SetSelectionError(
				OutError,
				TEXT("edit_bundle_selection_empty"));
			return false;
		}

		FString SelectionMaterial =
			Model->GetSemanticFingerprint();
		AppendToken(
			SelectionMaterial,
			TEXT("selection"),
			Selection.GetSelectionId());
		AppendToken(
			SelectionMaterial,
			TEXT("kind"),
			LexToString(Selection.GetSelectionKind()));
		AppendValues(
			SelectionMaterial,
			TEXT("unit"),
			Selection.GetSelectedUnitIds());
		AppendValues(
			SelectionMaterial,
			TEXT("field"),
			Selection.GetSelectedFieldIds());
		AppendValues(
			SelectionMaterial,
			TEXT("relation"),
			Selection.GetSelectedRelationIds());
		AppendToken(
			SelectionMaterial,
			TEXT("candidate"),
			Selection.GetCandidateBindingId());
		const FString SelectionFingerprint =
			Sha256(SelectionMaterial);

		FString ClosureMaterial =
			SelectionFingerprint
			+ Policy.GetPolicyFingerprint();
		AppendValues(
			ClosureMaterial,
			TEXT("unit"),
			SortedUnitIds);
		AppendValues(
			ClosureMaterial,
			TEXT("field"),
			SortedFieldIds);
		AppendValues(
			ClosureMaterial,
			TEXT("relation"),
			SortedRelationIds);
		FSelectionClosure Closure(
			Selection,
			MoveTemp(SortedUnitIds),
			MoveTemp(SortedFieldIds),
			MoveTemp(SortedRelationIds),
			SelectionFingerprint,
			Sha256(ClosureMaterial));
		if (!Closure.IsValid())
		{
			SetSelectionError(
				OutError,
				TEXT("edit_bundle_closure_invalid"));
			return false;
		}
		OutClosure.Emplace(MoveTemp(Closure));
		return true;
	}

	bool TryBuildScopedEditBundle(
		const FAuthoringModelRef& Model,
		const FWorkspaceLayoutPlan& Layout,
		const FSelectionClosure& Closure,
		const FString& SourceSnapshotId,
		const TArray<FUeAgentStateEntry>& FieldStates,
		const TMap<FString, FExportFieldDescription>&
			FieldDescriptionsById,
		TOptional<FUeAgentCandidateBinding> CandidateBinding,
		FScopedEditBundlePtr& OutBundle,
		FEditBundleSelectionError& OutError)
	{
		OutBundle.Reset();
		OutError.Reset();
		const FString SnapshotId = Normalize(SourceSnapshotId);
		if (Layout.GetSemanticFingerprint()
				!= Model->GetSemanticFingerprint()
			|| !Closure.IsValid()
			|| SnapshotId.IsEmpty())
		{
			SetSelectionError(
				OutError,
				TEXT("edit_bundle_materialization_invalid"));
			return false;
		}
		if (Closure.GetSelection().GetSelectionKind()
				== EWorkspaceSelectionKind::StructuralAdd
			&& (!CandidateBinding.IsSet()
				|| !CandidateBinding->IsValid()
				|| CandidateBinding->GetBindingId()
					!= Closure.GetSelection()
						.GetCandidateBindingId()))
		{
			SetSelectionError(
				OutError,
				TEXT("edit_bundle_candidate_binding_mismatch"),
				Closure.GetSelection().GetCandidateBindingId());
			return false;
		}

		TMap<FString, const FSemanticUnit*> ModelUnits;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			ModelUnits.Add(Unit.GetUnitId(), &Unit);
		}
		TMap<FString, FSemanticUnit> BundleUnits;
		TMap<FString, FString> BundlePlacements;
		for (const FString& UnitId : Closure.GetUnitIds())
		{
			const FSemanticUnit* const* Found =
				ModelUnits.Find(UnitId);
			const FString* Placement =
				Layout.GetLogicalPlacements().Find(UnitId);
			if (!Found || !Placement)
			{
				SetSelectionError(
					OutError,
					!Found
						? TEXT("edit_bundle_closure_unit_missing")
						: TEXT(
							"edit_bundle_closure_placement_missing"),
					UnitId);
				return false;
			}
			BundleUnits.Add(UnitId, **Found);
			BundlePlacements.Add(UnitId, *Placement);
		}

		TSet<FString> ClosureFields;
		for (const FString& FieldId : Closure.GetFieldIds())
		{
			ClosureFields.Add(FieldId);
		}
		TArray<FUeAgentStateEntry> BundleFieldStates;
		for (const FUeAgentStateEntry& State : FieldStates)
		{
			if (ClosureFields.Contains(
				State.GetIdentity().GetSemanticId()))
			{
				BundleFieldStates.Add(State);
			}
		}
		TMap<FString, FExportFieldDescription> BundleDescriptions;
		for (const FString& FieldId : Closure.GetFieldIds())
		{
			if (const FExportFieldDescription* Description =
				FieldDescriptionsById.Find(FieldId))
			{
				BundleDescriptions.Add(FieldId, *Description);
			}
		}

		FString BundleMaterial =
			Model->GetSemanticFingerprint()
			+ Closure.GetSelectionFingerprint()
			+ Closure.GetClosureFingerprint();
		AppendToken(
			BundleMaterial,
			TEXT("snapshot"),
			SnapshotId);
		AppendValues(
			BundleMaterial,
			TEXT("unit"),
			Closure.GetUnitIds());
		for (const FString& UnitId : Closure.GetUnitIds())
		{
			AppendToken(
				BundleMaterial,
				TEXT("placement"),
				BundlePlacements.FindChecked(UnitId));
		}
		if (CandidateBinding.IsSet())
		{
			AppendToken(
				BundleMaterial,
				TEXT("candidate"),
				CandidateBinding->GetBindingId());
		}
		const FScopedEditBundleRef Bundle =
			MakeShared<FScopedEditBundle>(
				GetEditBundleWorkspaceSchemaId(),
				GetEditBundleProfileId(),
				Model->GetContractId(),
				Model->GetContractVersion(),
				Model->GetBaseRevision(),
				Model->GetSemanticFingerprint(),
				Closure.GetSelectionFingerprint(),
				Closure.GetClosureFingerprint(),
				SnapshotId,
				MoveTemp(BundleUnits),
				MoveTemp(BundleFieldStates),
				MoveTemp(BundleDescriptions),
				MoveTemp(BundlePlacements),
				MoveTemp(CandidateBinding),
				Sha256(BundleMaterial));
		if (!Bundle->IsValid())
		{
			SetSelectionError(
				OutError,
				TEXT("edit_bundle_materialization_failed"));
			return false;
		}
		OutBundle = Bundle;
		return true;
	}

	FString GetEditBundleWorkspaceSchemaId()
	{
		return BundleSchema;
	}

	FString GetEditBundleProfileId()
	{
		return BundleProfile;
	}
}
