#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphAdapter.h"

namespace UE::UAI::DomainAdapters::PcgAiNodeGraph
{
	bool TryValidatePcgAiNodeGraphAssetIdentity(
		const FPcgAiNodeGraphAssetIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Identity.GetAssetKind().IsEmpty()
			|| Identity.GetAssetPath().IsEmpty()
			|| Identity.GetNativeClass().IsEmpty())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_asset_identity_invalid"));
			return false;
		}
		return true;
	}

	bool TryValidatePcgAiNodeGraphBlackboardEntryIdentity(
		const FPcgAiNodeGraphBlackboardEntryIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Identity.GetOwnerIdentity().IsEmpty()
			|| Identity.GetEntryName().IsEmpty()
			|| Identity.GetKeyTypeIdentity().IsEmpty())
		{
			OutError.Set(
				TEXT("pcg_ai_node_graph_blackboard_entry_identity_invalid"));
			return false;
		}
		return true;
	}

	bool TryValidatePcgAiNodeGraphParameterIdentity(
		const FPcgAiNodeGraphParameterIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Identity.GetOwnerIdentity().IsEmpty()
			|| !Identity.GetPropertyGuid().IsValid()
			|| Identity.GetPropertyName().IsNone()
			|| Identity.GetPropertyType().IsEmpty())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_parameter_identity_invalid"));
			return false;
		}
		return true;
	}

	bool TryValidatePcgAiNodeGraphComponentBindingIdentity(
		const FPcgAiNodeGraphComponentBindingIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Identity.GetActorIdentity().IsEmpty()
			|| Identity.GetComponentIdentity().IsEmpty()
			|| Identity.GetGraphIdentity().IsEmpty()
			|| Identity.GetGenerationPolicy().IsEmpty())
		{
			OutError.Set(
				TEXT("pcg_ai_node_graph_component_binding_identity_invalid"));
			return false;
		}
		return true;
	}

	bool TryValidatePcgAiNodeGraphDataAssetIdentity(
		const FPcgAiNodeGraphDataAssetIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Identity.GetDataAssetIdentity().IsEmpty()
			|| Identity.GetDataTypeIdentity().IsEmpty()
			|| Identity.GetSourceIdentity().IsEmpty()
			|| Identity.GetContentRevision().IsEmpty())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_data_asset_identity_invalid"));
			return false;
		}
		return true;
	}

	bool TryValidatePcgAiNodeGraphTopology(
		const FPcgAiNodeGraphTopology& Topology,
		FPcgAiNodeGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Topology.GetTargetKind().IsEmpty()
			|| Topology.GetTargetIdentity().IsEmpty()
			|| Topology.GetStructureIdentity().IsEmpty())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_target_identity_missing"));
			return false;
		}

		TSet<FString> ItemKeys;
		for (const FPcgAiNodeGraphItemIdentity& Item : Topology.GetItems())
		{
			if (Item.GetItemKey().IsEmpty()
				|| (Item.GetNativeIdentity().IsEmpty()
					&& Item.GetStableBuiltinRole().IsEmpty()))
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_item_identity_missing"),
					Item.GetItemKey());
				return false;
			}
			if (Item.GetNativeCarrierTypeIdentity().IsEmpty()
				|| Item.GetNativeTypeIdentity().IsEmpty())
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_type_identity_missing"),
					Item.GetItemKey());
				return false;
			}
			if (ItemKeys.Contains(Item.GetItemKey()))
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_item_identity_duplicate"),
					Item.GetItemKey());
				return false;
			}
			ItemKeys.Add(Item.GetItemKey());
		}

		TMap<FString, EPcgAiNodeGraphPinDirection> PinDirections;
		for (const FPcgAiNodeGraphPinIdentity& Pin : Topology.GetPins())
		{
			if (Pin.GetPinKey().IsEmpty()
				|| !ItemKeys.Contains(Pin.GetOwnerItemKey())
				|| Pin.GetNativeLabel().IsEmpty()
				|| Pin.GetNativeTypeIdentity().IsEmpty())
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_pin_identity_invalid"),
					Pin.GetPinKey());
				return false;
			}
			if (PinDirections.Contains(Pin.GetPinKey()))
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_pin_identity_duplicate"),
					Pin.GetPinKey());
				return false;
			}
			PinDirections.Add(Pin.GetPinKey(), Pin.GetDirection());
		}

		TSet<FString> RelationKeys;
		TMap<FString, TArray<FString>> HierarchyChildren;
		TMap<FString, int32> HierarchyInDegree;
		for (const FString& ItemKey : ItemKeys)
		{
			HierarchyInDegree.Add(ItemKey, 0);
		}

		for (const FPcgAiNodeGraphRelationIdentity& Relation
			: Topology.GetRelations())
		{
			if (Relation.GetRelationKey().IsEmpty()
				|| Relation.GetRelationKind().IsEmpty()
				|| RelationKeys.Contains(Relation.GetRelationKey()))
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_relation_identity_duplicate"),
					Relation.GetRelationKey());
				return false;
			}
			RelationKeys.Add(Relation.GetRelationKey());

			if (Relation.GetRelationKind() == TEXT("graph_link"))
			{
				const EPcgAiNodeGraphPinDirection* FromDirection =
					PinDirections.Find(Relation.GetFromIdentity());
				const EPcgAiNodeGraphPinDirection* ToDirection =
					PinDirections.Find(Relation.GetToIdentity());
				if (!FromDirection || !ToDirection)
				{
					OutError.Set(
						TEXT("pcg_ai_node_graph_relation_endpoint_unresolved"),
						Relation.GetRelationKey());
					return false;
				}
				if (*FromDirection != EPcgAiNodeGraphPinDirection::Output
					|| *ToDirection != EPcgAiNodeGraphPinDirection::Input)
				{
					OutError.Set(
						TEXT("pcg_ai_node_graph_link_direction_invalid"),
						Relation.GetRelationKey());
					return false;
				}
				continue;
			}

			if (!ItemKeys.Contains(Relation.GetFromIdentity())
				|| !ItemKeys.Contains(Relation.GetToIdentity()))
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_relation_endpoint_unresolved"),
					Relation.GetRelationKey());
				return false;
			}
			if ((Relation.GetRelationKind() == TEXT("hierarchy_child")
					|| Relation.GetRelationKind() == TEXT("ordered_attachment"))
				&& Relation.GetOrderIndex() < 0)
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_relation_order_missing"),
					Relation.GetRelationKey());
				return false;
			}
			if (Relation.GetRelationKind() == TEXT("state_transition")
				&& Relation.GetNativeRelationIdentity().IsEmpty())
			{
				OutError.Set(
					TEXT("pcg_ai_node_graph_relation_native_identity_missing"),
					Relation.GetRelationKey());
				return false;
			}
			if (Relation.GetRelationKind() == TEXT("hierarchy_child"))
			{
				HierarchyChildren.FindOrAdd(
					Relation.GetFromIdentity()).Add(Relation.GetToIdentity());
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
			const TArray<FString>* Children = HierarchyChildren.Find(Current);
			if (!Children)
			{
				continue;
			}
			for (const FString& Child : *Children)
			{
				int32& Degree = HierarchyInDegree.FindChecked(Child);
				--Degree;
				if (Degree == 0)
				{
					Ready.Add(Child);
				}
			}
		}
		if (Visited != HierarchyInDegree.Num())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_hierarchy_cycle"));
			return false;
		}
		return true;
	}

	bool TryValidatePcgAiNodeGraphLifecycleEvidence(
		const FPcgAiNodeGraphLifecycleEvidence& Evidence,
		FPcgAiNodeGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (!Evidence.IsCaptureComplete())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_capture_failed"));
			return false;
		}
		if (!Evidence.IsBarrierComplete()
			|| Evidence.GetBarrierErrorCount() != 0)
		{
			switch (Evidence.GetBarrierKind())
			{
			case EPcgAiNodeGraphBarrierKind::Compile:
				OutError.Set(TEXT("pcg_ai_node_graph_compile_incomplete"));
				break;
			case EPcgAiNodeGraphBarrierKind::Refresh:
				OutError.Set(TEXT("pcg_ai_node_graph_refresh_incomplete"));
				break;
			case EPcgAiNodeGraphBarrierKind::Validate:
				OutError.Set(TEXT("pcg_ai_node_graph_validate_incomplete"));
				break;
			}
			return false;
		}
		if (!Evidence.IsSettleComplete())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_settle_incomplete"));
			return false;
		}
		if (Evidence.GetBeforeRevision().IsEmpty()
			|| Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision())
		{
			OutError.Set(
				TEXT("pcg_ai_node_graph_readback_revision_not_advanced"));
			return false;
		}
		if (!Evidence.IsReadbackIndependent()
			|| Evidence.GetApplyEvidence().IsEmpty()
			|| Evidence.GetSettleEvidence().IsEmpty()
			|| Evidence.GetReadbackEvidence().IsEmpty()
			|| Evidence.GetApplyEvidence() == Evidence.GetSettleEvidence()
			|| Evidence.GetApplyEvidence() == Evidence.GetReadbackEvidence()
			|| Evidence.GetSettleEvidence() == Evidence.GetReadbackEvidence())
		{
			OutError.Set(TEXT("pcg_ai_node_graph_readback_not_independent"));
			return false;
		}
		return true;
	}
}
