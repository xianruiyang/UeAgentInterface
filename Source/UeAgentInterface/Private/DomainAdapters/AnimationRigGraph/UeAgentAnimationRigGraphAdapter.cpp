#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphAdapter.h"

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	bool TryValidateAnimationRigGraphAssetIdentity(
		const FAnimationRigGraphAssetIdentity& Identity,
		FAnimationRigGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Identity.GetAssetKind().IsEmpty()
			|| Identity.GetAssetPath().IsEmpty()
			|| Identity.GetNativeClass().IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_asset_identity_invalid"));
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
		for (const FAnimationRigGraphItemIdentity& Item : Topology.GetItems())
		{
			if (Item.GetItemKey().IsEmpty() || ItemKeys.Contains(Item.GetItemKey()))
			{
				OutError.Set(TEXT("animation_rig_graph_item_identity_duplicate"), Item.GetItemKey());
				return false;
			}
			if (!AllowedIdentitySources.Contains(Item.GetIdentitySource()))
			{
				OutError.Set(TEXT("animation_rig_graph_identity_source_missing"), Item.GetItemKey());
				return false;
			}
			const bool bHasRequiredIdentity =
				Item.GetIdentitySource() == TEXT("stable_builtin_role")
					? !Item.GetStableBuiltinRole().IsEmpty()
					: !Item.GetNativeIdentity().IsEmpty();
			if (!bHasRequiredIdentity)
			{
				OutError.Set(TEXT("animation_rig_graph_identity_source_missing"), Item.GetItemKey());
				return false;
			}
			if (Item.GetNativeCarrierTypeIdentity().IsEmpty()
				|| Item.GetNativeTypeIdentity().IsEmpty()
				|| Item.GetSemanticRole().IsEmpty())
			{
				OutError.Set(TEXT("animation_rig_graph_type_identity_missing"), Item.GetItemKey());
				return false;
			}
			ItemKeys.Add(Item.GetItemKey());
		}

		TMap<FString, EAnimationRigGraphPinDirection> PinDirections;
		for (const FAnimationRigGraphPinIdentity& Pin : Topology.GetPins())
		{
			if (Pin.GetPinKey().IsEmpty() || PinDirections.Contains(Pin.GetPinKey())
				|| !ItemKeys.Contains(Pin.GetOwnerItemKey())
				|| Pin.GetNativeLabel().IsEmpty()
				|| Pin.GetNativeTypeIdentity().IsEmpty())
			{
				OutError.Set(TEXT("animation_rig_graph_relation_endpoint_unresolved"), Pin.GetPinKey());
				return false;
			}
			if (Pin.GetDirection() != EAnimationRigGraphPinDirection::Input
				&& Pin.GetDirection() != EAnimationRigGraphPinDirection::Output)
			{
				OutError.Set(TEXT("animation_rig_graph_pin_direction_invalid"), Pin.GetPinKey());
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
		for (const FAnimationRigGraphRelationIdentity& Relation : Topology.GetRelations())
		{
			if (Relation.GetRelationKey().IsEmpty()
				|| Relation.GetRelationKind().IsEmpty()
				|| RelationKeys.Contains(Relation.GetRelationKey()))
			{
				OutError.Set(TEXT("animation_rig_graph_item_identity_duplicate"), Relation.GetRelationKey());
				return false;
			}
			RelationKeys.Add(Relation.GetRelationKey());

			const bool bFromPin = PinDirections.Contains(Relation.GetFromIdentity());
			const bool bToPin = PinDirections.Contains(Relation.GetToIdentity());
			const bool bFromItem = ItemKeys.Contains(Relation.GetFromIdentity());
			const bool bToItem = ItemKeys.Contains(Relation.GetToIdentity());
			if (!(bFromPin && bToPin) && !(bFromItem && bToItem))
			{
				OutError.Set(TEXT("animation_rig_graph_relation_endpoint_unresolved"), Relation.GetRelationKey());
				return false;
			}
			if (bFromPin && (PinDirections.FindChecked(Relation.GetFromIdentity())
					!= EAnimationRigGraphPinDirection::Output
				|| PinDirections.FindChecked(Relation.GetToIdentity())
					!= EAnimationRigGraphPinDirection::Input))
			{
				OutError.Set(TEXT("animation_rig_graph_pin_direction_invalid"), Relation.GetRelationKey());
				return false;
			}
			if (Relation.GetOrderIndex() >= 0)
			{
				const FString Slot = FString::Printf(TEXT("%s|%s|%d"),
					*Relation.GetFromIdentity(), *Relation.GetRelationKind(), Relation.GetOrderIndex());
				if (OrderedSlots.Contains(Slot))
				{
					OutError.Set(TEXT("animation_rig_graph_relation_order_duplicate"), Relation.GetRelationKey());
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
			OutError.Set(TEXT("animation_rig_graph_hierarchy_cycle"));
			return false;
		}
		return true;
	}

	bool TryValidateAnimationRigGraphLifecycleEvidence(
		const FAnimationRigGraphLifecycleEvidence& Evidence,
		FAnimationRigGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (!Evidence.IsBarrierComplete() || Evidence.GetBarrierErrorCount() != 0)
		{
			switch (Evidence.GetBarrierKind())
			{
			case EAnimationRigGraphBarrierKind::Compile:
				OutError.Set(TEXT("animation_rig_graph_compile_incomplete"));
				break;
			case EAnimationRigGraphBarrierKind::Refresh:
				OutError.Set(TEXT("animation_rig_graph_refresh_incomplete"));
				break;
			case EAnimationRigGraphBarrierKind::Validate:
				OutError.Set(TEXT("animation_rig_graph_validate_incomplete"));
				break;
			}
			return false;
		}
		if (!Evidence.IsSettleComplete())
		{
			OutError.Set(TEXT("animation_rig_graph_settle_incomplete"));
			return false;
		}
		if (Evidence.GetReadbackEvidence().IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_capture_failed"));
			return false;
		}
		const TSet<FString> EvidenceIds = {
			Evidence.GetApplyEvidence(), Evidence.GetBarrierEvidence(),
			Evidence.GetSettleEvidence(), Evidence.GetReadbackEvidence()};
		if (Evidence.GetBeforeRevision().IsEmpty()
			|| Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision()
			|| EvidenceIds.Num() != 4
			|| EvidenceIds.Contains(FString()))
		{
			OutError.Set(TEXT("animation_rig_graph_readback_not_independent"));
			return false;
		}
		return true;
	}
}
