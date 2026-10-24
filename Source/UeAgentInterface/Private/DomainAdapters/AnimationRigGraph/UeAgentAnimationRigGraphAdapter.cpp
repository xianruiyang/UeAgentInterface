#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphAdapter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	namespace
	{
		FAnimationRigGraphAdapterError TopologyFailure(const TCHAR* Code, const FString& Reason,
			const FString& Path, const FString& Identity = FString())
		{
			auto Error = FAnimationRigGraphAdapterError::Failure(Code, Reason, TEXT("native_graph"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.Source.InternalJsonPath = Path;
			Error.SubjectId = Identity;
			Error.ActualEvidence = MakeShared<FJsonValueString>(Identity);
			return Error;
		}
	}

	bool TryValidateAnimationRigGraphAssetIdentity(
		const FAnimationRigGraphAssetIdentity& Identity,
		FAnimationRigGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Identity.GetAssetKind().IsEmpty()
			|| Identity.GetAssetPath().IsEmpty()
			|| Identity.GetNativeClass().IsEmpty())
		{
			const TCHAR* Field = Identity.GetAssetKind().IsEmpty() ? TEXT("asset_kind")
				: Identity.GetAssetPath().IsEmpty() ? TEXT("asset_path") : TEXT("native_class");
			OutError = FAnimationRigGraphAdapterError::Failure(TEXT("animation_rig_graph_asset_identity_invalid"),
				FString::Printf(TEXT("The captured animation asset identity has an empty '%s'."), Field), TEXT("native_graph"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Identity.GetAssetPath();
			OutError.PropertyPath = TEXT("/") + FString(Field);
			OutError.ActualValue = MakeShared<FJsonValueString>(FString());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty string"));
			return false;
		}
		return true;
	}

	bool TryValidateAnimationRigGraphTopology(
		const FAnimationRigGraphTopology& Topology,
		FAnimationRigGraphAdapterError& OutError)
	{
		OutError.Reset();
		const TSet<FString> AllowedIdentitySources = {
			TEXT("native_guid"), TEXT("native_object_path"), TEXT("native_key"),
			TEXT("native_composite"), TEXT("stable_builtin_role")};
		TSet<FString> ItemKeys;
		int32 ItemIndex = 0;
		for (const FAnimationRigGraphItemIdentity& Item : Topology.GetItems())
		{
			const FString Base = FString::Printf(TEXT("/items/%d"), ItemIndex++);
			if (Item.GetItemKey().IsEmpty() || ItemKeys.Contains(Item.GetItemKey()))
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_item_identity_duplicate"),
					Item.GetItemKey().IsEmpty() ? TEXT("The captured graph item has no identity key.")
						: TEXT("Two captured graph items have the same identity key."), Base + TEXT("/item_key"), Item.GetItemKey());
				return false;
			}
			if (!AllowedIdentitySources.Contains(Item.GetIdentitySource()))
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_identity_source_missing"),
					TEXT("The captured graph item uses an unsupported identity source."), Base + TEXT("/identity_source"), Item.GetItemKey());
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Item.GetIdentitySource());
				return false;
			}
			const bool bHasRequiredIdentity =
				Item.GetIdentitySource() == TEXT("stable_builtin_role")
					? !Item.GetStableBuiltinRole().IsEmpty()
					: !Item.GetNativeIdentity().IsEmpty();
			if (!bHasRequiredIdentity)
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_identity_source_missing"),
					Item.GetIdentitySource() == TEXT("stable_builtin_role") ? TEXT("The captured graph item has no required stable built-in role.")
						: TEXT("The captured graph item has no required native identity."),
					Base + (Item.GetIdentitySource() == TEXT("stable_builtin_role") ? TEXT("/stable_builtin_role") : TEXT("/native_identity")), Item.GetItemKey());
				return false;
			}
			if (Item.GetNativeCarrierTypeIdentity().IsEmpty()
				|| Item.GetNativeTypeIdentity().IsEmpty()
				|| Item.GetSemanticRole().IsEmpty())
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_type_identity_missing"),
					Item.GetNativeCarrierTypeIdentity().IsEmpty() ? TEXT("The captured graph item has no native carrier type.")
						: Item.GetNativeTypeIdentity().IsEmpty() ? TEXT("The captured graph item has no native type.")
						: TEXT("The captured graph item has no semantic role."),
					Base + (Item.GetNativeCarrierTypeIdentity().IsEmpty() ? TEXT("/native_carrier_type")
						: Item.GetNativeTypeIdentity().IsEmpty() ? TEXT("/native_type") : TEXT("/semantic_role")), Item.GetItemKey());
				return false;
			}
			ItemKeys.Add(Item.GetItemKey());
		}

		TMap<FString, EAnimationRigGraphPinDirection> PinDirections;
		int32 PinIndex = 0;
		for (const FAnimationRigGraphPinIdentity& Pin : Topology.GetPins())
		{
			const FString Base = FString::Printf(TEXT("/pins/%d"), PinIndex++);
			if (Pin.GetPinKey().IsEmpty() || PinDirections.Contains(Pin.GetPinKey())
				|| !ItemKeys.Contains(Pin.GetOwnerItemKey())
				|| Pin.GetNativeLabel().IsEmpty()
				|| Pin.GetNativeTypeIdentity().IsEmpty())
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_relation_endpoint_unresolved"),
					Pin.GetPinKey().IsEmpty() ? TEXT("The captured pin has no identity key.")
						: PinDirections.Contains(Pin.GetPinKey()) ? TEXT("Two captured pins have the same identity key.")
						: !ItemKeys.Contains(Pin.GetOwnerItemKey()) ? TEXT("The captured pin refers to an owner item that is absent from the graph.")
						: Pin.GetNativeLabel().IsEmpty() ? TEXT("The captured pin has no native label.")
						: TEXT("The captured pin has no native type."), Base, Pin.GetPinKey());
				if (!ItemKeys.Contains(Pin.GetOwnerItemKey())) OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Pin.GetOwnerItemKey());
				return false;
			}
			if (Pin.GetDirection() != EAnimationRigGraphPinDirection::Input
				&& Pin.GetDirection() != EAnimationRigGraphPinDirection::Output)
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_pin_direction_invalid"),
					TEXT("The captured pin direction is neither input nor output."), Base + TEXT("/direction"), Pin.GetPinKey());
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Pin.GetDirection()));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("input or output"));
				return false;
			}
			PinDirections.Add(Pin.GetPinKey(), Pin.GetDirection());
		}

		TSet<FString> RelationKeys;
		TSet<FString> OrderedSlots;
		TMap<FString, TArray<FString>> HierarchyChildren;
		TMap<FString, int32> HierarchyInDegree;
		for (const FString& ItemKey : ItemKeys)
		{
			HierarchyInDegree.Add(ItemKey, 0);
		}
		int32 RelationIndex = 0;
		for (const FAnimationRigGraphRelationIdentity& Relation : Topology.GetRelations())
		{
			const FString Base = FString::Printf(TEXT("/relations/%d"), RelationIndex++);
			if (Relation.GetRelationKey().IsEmpty()
				|| Relation.GetRelationKind().IsEmpty()
				|| RelationKeys.Contains(Relation.GetRelationKey()))
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_item_identity_duplicate"),
					Relation.GetRelationKey().IsEmpty() ? TEXT("The captured relation has no identity key.")
						: Relation.GetRelationKind().IsEmpty() ? TEXT("The captured relation has no relation kind.")
						: TEXT("Two captured relations have the same identity key."), Base, Relation.GetRelationKey());
				return false;
			}
			RelationKeys.Add(Relation.GetRelationKey());

			const bool bFromPin = PinDirections.Contains(Relation.GetFromIdentity());
			const bool bToPin = PinDirections.Contains(Relation.GetToIdentity());
			const bool bFromItem = ItemKeys.Contains(Relation.GetFromIdentity());
			const bool bToItem = ItemKeys.Contains(Relation.GetToIdentity());
			if (!(bFromPin && bToPin) && !(bFromItem && bToItem))
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_relation_endpoint_unresolved"),
					!(bFromPin || bFromItem) && !(bToPin || bToItem) ? TEXT("Both captured relation endpoints are absent from the graph.")
						: !(bFromPin || bFromItem) ? TEXT("The captured relation source endpoint is absent from the graph.")
						: !(bToPin || bToItem) ? TEXT("The captured relation destination endpoint is absent from the graph.")
						: TEXT("The captured relation mixes a pin endpoint with an item endpoint."), Base, Relation.GetRelationKey());
				const auto Endpoints = MakeShared<FJsonObject>();
				Endpoints->SetStringField(TEXT("from"), Relation.GetFromIdentity());
				Endpoints->SetStringField(TEXT("to"), Relation.GetToIdentity());
				OutError.ActualEvidence = MakeShared<FJsonValueObject>(Endpoints);
				return false;
			}
			if (bFromPin && (PinDirections.FindChecked(Relation.GetFromIdentity())
					!= EAnimationRigGraphPinDirection::Output
				|| PinDirections.FindChecked(Relation.GetToIdentity())
					!= EAnimationRigGraphPinDirection::Input))
			{
				OutError = TopologyFailure(TEXT("animation_rig_graph_pin_direction_invalid"),
					TEXT("A captured pin relation must run from an output pin to an input pin."), Base, Relation.GetRelationKey());
				const auto Directions = MakeShared<FJsonObject>();
				Directions->SetNumberField(TEXT("from"), static_cast<int32>(PinDirections.FindChecked(Relation.GetFromIdentity())));
				Directions->SetNumberField(TEXT("to"), static_cast<int32>(PinDirections.FindChecked(Relation.GetToIdentity())));
				OutError.ActualValue = MakeShared<FJsonValueObject>(Directions);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("output to input"));
				return false;
			}
			if (Relation.GetOrderIndex() >= 0)
			{
				const FString Slot = FString::Printf(TEXT("%s|%s|%d"),
					*Relation.GetFromIdentity(), *Relation.GetRelationKind(), Relation.GetOrderIndex());
				if (OrderedSlots.Contains(Slot))
				{
					OutError = TopologyFailure(TEXT("animation_rig_graph_relation_order_duplicate"),
						TEXT("Two relations from the same source and of the same kind occupy the same order index."), Base + TEXT("/order_index"), Relation.GetRelationKey());
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Relation.GetOrderIndex());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unique order index within the relation group"));
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Slot);
					return false;
				}
				OrderedSlots.Add(Slot);
			}
			if (bFromItem && (Relation.GetRelationKind() == TEXT("hierarchy_child")
				|| Relation.GetRelationKind() == TEXT("collection_member")))
			{
				HierarchyChildren.FindOrAdd(Relation.GetFromIdentity()).Add(Relation.GetToIdentity());
				++HierarchyInDegree.FindChecked(Relation.GetToIdentity());
			}
		}

		TArray<FString> Ready;
		for (const TPair<FString, int32>& Pair : HierarchyInDegree)
		{
			if (Pair.Value == 0)
			{
				Ready.Add(Pair.Key);
			}
		}
		int32 Visited = 0;
		while (!Ready.IsEmpty())
		{
			const FString Current = Ready.Pop(EAllowShrinking::No);
			++Visited;
			if (const TArray<FString>* Children = HierarchyChildren.Find(Current))
			{
				for (const FString& Child : *Children)
				{
					int32& Degree = HierarchyInDegree.FindChecked(Child);
					if (--Degree == 0)
					{
						Ready.Add(Child);
					}
				}
			}
		}
		if (Visited != HierarchyInDegree.Num())
		{
			OutError = TopologyFailure(TEXT("animation_rig_graph_hierarchy_cycle"),
				TEXT("The captured hierarchy contains a cycle, so not all graph items can be ordered."), TEXT("/relations"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(HierarchyInDegree.Num() - Visited);
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(0);
			TArray<FString> Unresolved;
			for (const auto& Pair : HierarchyInDegree) if (Pair.Value > 0) Unresolved.Add(Pair.Key);
			Unresolved.Sort();
			TArray<TSharedPtr<FJsonValue>> Identities;
			for (const FString& Identity : Unresolved) Identities.Add(MakeShared<FJsonValueString>(Identity));
			OutError.ActualEvidence = MakeShared<FJsonValueArray>(MoveTemp(Identities));
			return false;
		}
		return true;
	}

	bool TryValidateAnimationRigGraphLifecycleEvidence(
		const FAnimationRigGraphLifecycleEvidence& Evidence,
		FAnimationRigGraphAdapterError& OutError)
	{
		OutError.Reset();
        const auto LifecycleFailure = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            OutError = FAnimationRigGraphAdapterError::Failure(Code, Reason, TEXT("native_graph"));
            OutError.SourceKind = TEXT("native_result");
            OutError.PropertyPath = Field;
            OutError.ActualValue = Actual;
            OutError.ExpectedValue = Expected;
            return false;
        };
        if (!Evidence.IsBarrierComplete() || Evidence.GetBarrierErrorCount() != 0)
        {
            const TCHAR* Code = TEXT("animation_rig_graph_validate_incomplete");
            FString Kind = TEXT("validation");
            switch (Evidence.GetBarrierKind())
            {
            case EAnimationRigGraphBarrierKind::Compile:
                Code = TEXT("animation_rig_graph_compile_incomplete");
                Kind = TEXT("compilation");
                break;
            case EAnimationRigGraphBarrierKind::Refresh:
                Code = TEXT("animation_rig_graph_refresh_incomplete");
                Kind = TEXT("refresh");
                break;
            case EAnimationRigGraphBarrierKind::Validate:
                break;
            }
            auto Actual = MakeShared<FJsonObject>();
            Actual->SetBoolField(TEXT("complete"), Evidence.IsBarrierComplete());
            Actual->SetNumberField(TEXT("error_count"), Evidence.GetBarrierErrorCount());
            auto Expected = MakeShared<FJsonObject>();
            Expected->SetBoolField(TEXT("complete"), true);
            Expected->SetNumberField(TEXT("error_count"), 0);
            return LifecycleFailure(Code, TEXT("Native ") + Kind + TEXT(" must complete without errors before readback."),
                TEXT("barrier"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
        }
        if (!Evidence.IsSettleComplete())
            return LifecycleFailure(TEXT("animation_rig_graph_settle_incomplete"),
                TEXT("The native asset has not completed settling."), TEXT("settle_complete"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        if (Evidence.GetReadbackEvidence().IsEmpty())
            return LifecycleFailure(TEXT("animation_rig_graph_capture_failed"),
                TEXT("Final native capture did not produce readback evidence."), TEXT("readback_evidence_available"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		const TSet<FString> EvidenceIds = {
			Evidence.GetApplyEvidence(), Evidence.GetBarrierEvidence(),
			Evidence.GetSettleEvidence(), Evidence.GetReadbackEvidence()};
		if (Evidence.GetBeforeRevision().IsEmpty()
			|| Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision()
			|| EvidenceIds.Num() != 4
			|| EvidenceIds.Contains(FString()))
		{
            if (Evidence.GetBeforeRevision().IsEmpty() || Evidence.GetReadbackRevision().IsEmpty())
                return LifecycleFailure(TEXT("animation_rig_graph_readback_not_independent"),
                    TEXT("Both the before and final readback revisions are required."),
                    Evidence.GetBeforeRevision().IsEmpty() ? TEXT("before_revision_available") : TEXT("readback_revision_available"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            if (Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision())
                return LifecycleFailure(TEXT("animation_rig_graph_readback_not_independent"),
                    TEXT("The final readback revision is unchanged from the before revision."), TEXT("revision_changed"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            if (EvidenceIds.Num() != 4)
                return LifecycleFailure(TEXT("animation_rig_graph_readback_not_independent"),
                    TEXT("Apply, barrier, settle and readback must have distinct evidence identities."), TEXT("distinct_evidence_count"),
                    MakeShared<FJsonValueNumber>(EvidenceIds.Num()), MakeShared<FJsonValueNumber>(4));
            const TCHAR* Field = Evidence.GetApplyEvidence().IsEmpty() ? TEXT("apply_evidence_available")
                : Evidence.GetBarrierEvidence().IsEmpty() ? TEXT("barrier_evidence_available") : TEXT("settle_evidence_available");
            return LifecycleFailure(TEXT("animation_rig_graph_readback_not_independent"),
                TEXT("Every lifecycle stage must produce evidence."), Field,
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}
		return true;
	}
}
