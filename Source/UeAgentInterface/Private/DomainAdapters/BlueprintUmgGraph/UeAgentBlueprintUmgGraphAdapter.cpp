#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphAdapter.h"

namespace UE::UAI::DomainAdapters::BlueprintUmgGraph
{
	bool TryValidateBlueprintUmgGraphTopology(
		const FBlueprintUmgGraphTopology& Topology,
		FBlueprintUmgGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Topology.GetTargetKind().IsEmpty()
			|| Topology.GetTargetIdentity().IsEmpty()
			|| Topology.GetStructureIdentity().IsEmpty())
		{
			OutError.Set(TEXT("blueprint_umg_graph_target_identity_missing"));
			return false;
		}

		TSet<FString> ItemKeys;
		for (const FBlueprintUmgGraphItemIdentity& Item : Topology.GetItems())
		{
			if (Item.GetItemKey().IsEmpty()
				|| (Item.GetNativeGuid().IsEmpty()
					&& Item.GetStableBuiltinRole().IsEmpty()))
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_builtin_identity_missing"),
					Item.GetItemKey());
				return false;
			}
			if (ItemKeys.Contains(Item.GetItemKey()))
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_item_identity_duplicate"),
					Item.GetItemKey());
				return false;
			}
			ItemKeys.Add(Item.GetItemKey());
		}

		TMap<FString, EBlueprintUmgGraphPinDirection> PinDirections;
		for (const FBlueprintUmgGraphPinIdentity& Pin : Topology.GetPins())
		{
			if (Pin.GetPinKey().IsEmpty()
				|| !ItemKeys.Contains(Pin.GetOwnerItemKey()))
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_pin_owner_unresolved"),
					Pin.GetPinKey());
				return false;
			}
			if (PinDirections.Contains(Pin.GetPinKey()))
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_pin_identity_duplicate"),
					Pin.GetPinKey());
				return false;
			}
			PinDirections.Add(Pin.GetPinKey(), Pin.GetDirection());
		}

		TSet<FString> RelationKeys;
		for (const FBlueprintUmgGraphLinkIdentity& Link : Topology.GetLinks())
		{
			const EBlueprintUmgGraphPinDirection* FromDirection =
				PinDirections.Find(Link.GetFromPinKey());
			const EBlueprintUmgGraphPinDirection* ToDirection =
				PinDirections.Find(Link.GetToPinKey());
			if (!FromDirection || !ToDirection)
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_link_endpoint_unresolved"),
					Link.GetLinkKey());
				return false;
			}
			if (*FromDirection != EBlueprintUmgGraphPinDirection::Output
				|| *ToDirection != EBlueprintUmgGraphPinDirection::Input)
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_link_direction_invalid"),
					Link.GetLinkKey());
				return false;
			}
			if (Link.GetLinkKey().IsEmpty()
				|| RelationKeys.Contains(Link.GetLinkKey()))
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_relation_identity_duplicate"),
					Link.GetLinkKey());
				return false;
			}
			RelationKeys.Add(Link.GetLinkKey());
		}

		TMap<FString, int32> InDegree;
		TMultiMap<FString, FString> Children;
		for (const FString& ItemKey : ItemKeys)
		{
			InDegree.Add(ItemKey, 0);
		}
		for (const FBlueprintUmgGraphHierarchyRelation& Relation
			: Topology.GetHierarchy())
		{
			if (Relation.GetRelationKey().IsEmpty()
				|| RelationKeys.Contains(Relation.GetRelationKey()))
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_relation_identity_duplicate"),
					Relation.GetRelationKey());
				return false;
			}
			RelationKeys.Add(Relation.GetRelationKey());
			if (!ItemKeys.Contains(Relation.GetItemKey())
				|| !ItemKeys.Contains(Relation.GetParentKey()))
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_hierarchy_endpoint_unresolved"),
					Relation.GetRelationKey());
				return false;
			}
			++InDegree.FindChecked(Relation.GetItemKey());
			Children.Add(Relation.GetParentKey(), Relation.GetItemKey());
		}

		TArray<FString> Ready;
		for (const TPair<FString, int32>& Pair : InDegree)
		{
			if (Pair.Value == 0)
			{
				Ready.Add(Pair.Key);
			}
		}
		int32 Visited = 0;
		while (!Ready.IsEmpty())
		{
			const FString Parent = Ready.Pop(EAllowShrinking::No);
			++Visited;
			TArray<FString> DirectChildren;
			Children.MultiFind(Parent, DirectChildren);
			for (const FString& Child : DirectChildren)
			{
				int32& Degree = InDegree.FindChecked(Child);
				--Degree;
				if (Degree == 0)
				{
					Ready.Add(Child);
				}
			}
		}
		if (Visited != ItemKeys.Num())
		{
			OutError.Set(TEXT("blueprint_umg_graph_hierarchy_cycle"));
			return false;
		}

		TSet<FString> BindingKeys;
		for (const FBlueprintUmgGraphBindingIdentity& Binding
			: Topology.GetBindings())
		{
			if (Binding.GetBindingKey().IsEmpty()
				|| BindingKeys.Contains(Binding.GetBindingKey()))
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_binding_identity_duplicate"),
					Binding.GetBindingKey());
				return false;
			}
			if (!ItemKeys.Contains(Binding.GetSourceIdentity())
				|| Binding.GetSourceField().IsEmpty()
				|| Binding.GetTargetIdentity().IsEmpty()
				|| Binding.GetTargetField().IsEmpty())
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_binding_endpoint_unresolved"),
					Binding.GetBindingKey());
				return false;
			}
			BindingKeys.Add(Binding.GetBindingKey());
		}
		return true;
	}

	bool TryValidateBlueprintUmgGraphLifecycleEvidence(
		const FBlueprintUmgGraphLifecycleEvidence& Evidence,
		FBlueprintUmgGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (!Evidence.IsCompileComplete() || Evidence.GetErrorCount() != 0)
		{
			OutError.Set(TEXT("blueprint_umg_graph_compile_incomplete"));
			return false;
		}
		if (Evidence.GetBeforeRevision().IsEmpty()
			|| Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision())
		{
			OutError.Set(TEXT("blueprint_umg_graph_readback_revision_not_advanced"));
			return false;
		}
		if (Evidence.GetApplyEvidence().IsEmpty()
			|| Evidence.GetSettleEvidence().IsEmpty()
			|| Evidence.GetReadbackEvidence().IsEmpty()
			|| Evidence.GetApplyEvidence() == Evidence.GetSettleEvidence()
			|| Evidence.GetApplyEvidence() == Evidence.GetReadbackEvidence()
			|| Evidence.GetSettleEvidence() == Evidence.GetReadbackEvidence())
		{
			OutError.Set(TEXT("blueprint_umg_graph_readback_not_independent"));
			return false;
		}
		return true;
	}
}
