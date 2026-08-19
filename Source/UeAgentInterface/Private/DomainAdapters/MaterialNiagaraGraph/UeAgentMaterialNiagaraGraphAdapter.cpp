#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphAdapter.h"

namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph
{
	namespace
	{
		const TCHAR* ParameterTypeToken(
			const EMaterialNiagaraGraphParameterType Type)
		{
			switch (Type)
			{
			case EMaterialNiagaraGraphParameterType::Scalar:
				return TEXT("scalar");
			case EMaterialNiagaraGraphParameterType::Vector:
				return TEXT("vector");
			case EMaterialNiagaraGraphParameterType::Texture:
				return TEXT("texture");
			case EMaterialNiagaraGraphParameterType::StaticSwitch:
				return TEXT("static_switch");
			case EMaterialNiagaraGraphParameterType::StaticComponentMask:
				return TEXT("static_component_mask");
			}
			return TEXT("unknown");
		}
	}

	FString FMaterialNiagaraGraphParameterIdentity::GetParameterKey() const
	{
		return FString::Printf(
			TEXT("parameter:%s:%s:%d:%s"),
			ParameterTypeToken(ParameterType),
			*ParameterAssociation,
			ParameterIndex,
			*ParameterName);
	}

	bool TryValidateMaterialNiagaraGraphParameterIdentities(
		const TArray<FMaterialNiagaraGraphParameterIdentity>& Parameters,
		FMaterialNiagaraGraphAdapterError& OutError)
	{
		OutError.Reset();
		TSet<FString> Keys;
		for (const FMaterialNiagaraGraphParameterIdentity& Parameter
			: Parameters)
		{
			const FString& Association =
				Parameter.GetParameterAssociation();
			const bool bGlobal = Association == TEXT("global");
			const bool bIndexed = Association == TEXT("layer")
				|| Association == TEXT("blend");
			if (Parameter.GetParameterName().IsEmpty()
				|| (!bGlobal && !bIndexed)
				|| (bGlobal && Parameter.GetParameterIndex() != INDEX_NONE)
				|| (bIndexed && Parameter.GetParameterIndex() < 0))
			{
				OutError.Set(
					TEXT("material_niagara_graph_parameter_identity_invalid"),
					Parameter.GetParameterKey());
				return false;
			}
			const FString Key = Parameter.GetParameterKey();
			if (Keys.Contains(Key))
			{
				OutError.Set(
					TEXT("material_niagara_graph_parameter_identity_duplicate"),
					Key);
				return false;
			}
			Keys.Add(Key);
		}
		return true;
	}

	bool TryValidateMaterialNiagaraGraphTopology(
		const FMaterialNiagaraGraphTopology& Topology,
		FMaterialNiagaraGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Topology.GetTargetKind().IsEmpty()
			|| Topology.GetTargetIdentity().IsEmpty()
			|| Topology.GetStructureIdentity().IsEmpty())
		{
			OutError.Set(TEXT("material_niagara_graph_target_identity_missing"));
			return false;
		}

		TSet<FString> ItemKeys;
		for (const FMaterialNiagaraGraphItemIdentity& Item
			: Topology.GetItems())
		{
			if (Item.GetItemKey().IsEmpty()
				|| (Item.GetNativeGuid().IsEmpty()
					&& Item.GetStableBuiltinRole().IsEmpty()))
			{
				OutError.Set(
					TEXT("material_niagara_graph_item_identity_missing"),
					Item.GetItemKey());
				return false;
			}
			if (ItemKeys.Contains(Item.GetItemKey()))
			{
				OutError.Set(
					TEXT("material_niagara_graph_item_identity_duplicate"),
					Item.GetItemKey());
				return false;
			}
			ItemKeys.Add(Item.GetItemKey());
		}

		TMap<FString, EMaterialNiagaraGraphPinDirection> PinDirections;
		for (const FMaterialNiagaraGraphPinIdentity& Pin
			: Topology.GetPins())
		{
			if (Pin.GetPinKey().IsEmpty()
				|| !ItemKeys.Contains(Pin.GetOwnerItemKey()))
			{
				OutError.Set(
					TEXT("material_niagara_graph_pin_owner_unresolved"),
					Pin.GetPinKey());
				return false;
			}
			if (PinDirections.Contains(Pin.GetPinKey()))
			{
				OutError.Set(
					TEXT("material_niagara_graph_pin_identity_duplicate"),
					Pin.GetPinKey());
				return false;
			}
			PinDirections.Add(Pin.GetPinKey(), Pin.GetDirection());
		}

		TSet<FString> RelationKeys;
		for (const FMaterialNiagaraGraphLinkIdentity& Link
			: Topology.GetLinks())
		{
			const EMaterialNiagaraGraphPinDirection* FromDirection =
				PinDirections.Find(Link.GetFromPinKey());
			const EMaterialNiagaraGraphPinDirection* ToDirection =
				PinDirections.Find(Link.GetToPinKey());
			if (!FromDirection || !ToDirection)
			{
				OutError.Set(
					TEXT("material_niagara_graph_link_endpoint_unresolved"),
					Link.GetLinkKey());
				return false;
			}
			if (*FromDirection != EMaterialNiagaraGraphPinDirection::Output
				|| *ToDirection != EMaterialNiagaraGraphPinDirection::Input)
			{
				OutError.Set(
					TEXT("material_niagara_graph_link_direction_invalid"),
					Link.GetLinkKey());
				return false;
			}
			if (Link.GetLinkKey().IsEmpty()
				|| RelationKeys.Contains(Link.GetLinkKey()))
			{
				OutError.Set(
					TEXT("material_niagara_graph_relation_identity_duplicate"),
					Link.GetLinkKey());
				return false;
			}
			RelationKeys.Add(Link.GetLinkKey());
		}

		TSet<FString> OrderedItems;
		TMap<FString, FString> PredecessorByItem;
		for (const FMaterialNiagaraGraphOrderingIdentity& Ordering
			: Topology.GetOrdering())
		{
			if (Ordering.GetOrderingKey().IsEmpty()
				|| RelationKeys.Contains(Ordering.GetOrderingKey()))
			{
				OutError.Set(
					TEXT("material_niagara_graph_relation_identity_duplicate"),
					Ordering.GetOrderingKey());
				return false;
			}
			RelationKeys.Add(Ordering.GetOrderingKey());
			if (!ItemKeys.Contains(Ordering.GetItemKey())
				|| (!Ordering.GetPredecessorItemKey().IsEmpty()
					&& !ItemKeys.Contains(
						Ordering.GetPredecessorItemKey())))
			{
				OutError.Set(
					TEXT("material_niagara_graph_stack_item_identity_missing"),
					Ordering.GetOrderingKey());
				return false;
			}
			if (OrderedItems.Contains(Ordering.GetItemKey()))
			{
				OutError.Set(
					TEXT("material_niagara_graph_stack_order_duplicate"),
					Ordering.GetItemKey());
				return false;
			}
			OrderedItems.Add(Ordering.GetItemKey());
			PredecessorByItem.Add(
				Ordering.GetItemKey(),
				Ordering.GetPredecessorItemKey());
		}

		for (const FString& Start : OrderedItems)
		{
			TSet<FString> Seen;
			FString Current = Start;
			while (!Current.IsEmpty())
			{
				if (Seen.Contains(Current))
				{
					OutError.Set(
						TEXT("material_niagara_graph_stack_order_cycle"),
						Start);
					return false;
				}
				Seen.Add(Current);
				const FString* Predecessor =
					PredecessorByItem.Find(Current);
				Current = Predecessor ? *Predecessor : FString();
			}
		}
		return true;
	}

	bool TryValidateMaterialNiagaraGraphLifecycleEvidence(
		const FMaterialNiagaraGraphLifecycleEvidence& Evidence,
		FMaterialNiagaraGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (!Evidence.IsStackCaptureComplete())
		{
			OutError.Set(TEXT("material_niagara_graph_stack_capture_failed"));
			return false;
		}
		if (!Evidence.IsCompoundCaptureComplete())
		{
			OutError.Set(TEXT("material_niagara_graph_compound_capture_failed"));
			return false;
		}
		if (!Evidence.IsCompilationComplete()
			|| Evidence.GetCompileErrorCount() != 0)
		{
			OutError.Set(TEXT("material_niagara_graph_compile_incomplete"));
			return false;
		}
		if (Evidence.GetStackIssueCount() != 0)
		{
			OutError.Set(TEXT("material_niagara_graph_stack_issue"));
			return false;
		}
		if (Evidence.GetBeforeRevision().IsEmpty()
			|| Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision())
		{
			OutError.Set(
				TEXT("material_niagara_graph_readback_revision_not_advanced"));
			return false;
		}
		if (Evidence.GetApplyEvidence().IsEmpty()
			|| Evidence.GetSettleEvidence().IsEmpty()
			|| Evidence.GetReadbackEvidence().IsEmpty()
			|| Evidence.GetApplyEvidence() == Evidence.GetSettleEvidence()
			|| Evidence.GetApplyEvidence() == Evidence.GetReadbackEvidence()
			|| Evidence.GetSettleEvidence() == Evidence.GetReadbackEvidence())
		{
			OutError.Set(
				TEXT("material_niagara_graph_readback_not_independent"));
			return false;
		}
		return true;
	}

	bool TryValidateMaterialNiagaraGraphCompoundCapture(
		const bool bCaptureSucceeded,
		const FString& DelegatedErrorCode,
		const FString& DelegatedErrorDetail,
		FMaterialNiagaraGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (bCaptureSucceeded)
		{
			return true;
		}
		OutError.Set(
			DelegatedErrorCode.IsEmpty()
				? TEXT("material_niagara_graph_compound_capture_failed")
				: DelegatedErrorCode,
			DelegatedErrorDetail);
		return false;
	}
}
