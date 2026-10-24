#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphAdapter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::PcgAiNodeGraph
{
	namespace
	{
		Contract::FDiagnostic GraphFact(const TCHAR* Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString(), const FString& PrivateContext = FString())
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_adapter"), PrivateContext);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.TargetPath = Target;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FString DirectionName(const EPcgAiNodeGraphPinDirection Direction)
		{
			switch (Direction)
			{
			case EPcgAiNodeGraphPinDirection::Input: return TEXT("Input");
			case EPcgAiNodeGraphPinDirection::Output: return TEXT("Output");
			default: return FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Direction));
			}
		}
	}

	bool TryValidatePcgAiNodeGraphAssetIdentity(
		const FPcgAiNodeGraphAssetIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Identity.GetAssetKind().IsEmpty()
			|| Identity.GetAssetPath().IsEmpty()
			|| Identity.GetNativeClass().IsEmpty())
		{
			OutError = GraphFact(TEXT("pcg_ai_node_graph_asset_identity_invalid"),
			Identity.GetAssetKind().IsEmpty() ? TEXT("The native asset identity has no asset kind.")
				: (Identity.GetAssetPath().IsEmpty() ? TEXT("The native asset identity has no asset path.") : TEXT("The native asset identity has no class type.")),
			Identity.GetAssetKind().IsEmpty() ? TEXT("/asset/asset_kind")
				: (Identity.GetAssetPath().IsEmpty() ? TEXT("/asset/asset_path") : TEXT("/asset/native_class")), MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty string")), Identity.GetAssetPath(), FString());
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
			OutError = GraphFact(TEXT("pcg_ai_node_graph_blackboard_entry_identity_invalid"),
			Identity.GetOwnerIdentity().IsEmpty() ? TEXT("The blackboard entry has no owner asset.")
				: (Identity.GetEntryName().IsEmpty() ? TEXT("The blackboard entry has no name.") : TEXT("The blackboard entry has no key type.")),
			Identity.GetOwnerIdentity().IsEmpty() ? TEXT("/blackboard_entry/owner")
				: (Identity.GetEntryName().IsEmpty() ? TEXT("/blackboard_entry/name") : TEXT("/blackboard_entry/key_type")), MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty string")), Identity.GetOwnerIdentity(), FString());
			OutError.CandidateName = Identity.GetEntryName();
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
			if (Identity.GetOwnerIdentity().IsEmpty())
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_parameter_identity_invalid"),
				TEXT("The PCG parameter has no owner graph."),
				TEXT("/parameter/owner"), MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty graph path")), FString(), FString());
			}
			else if (!Identity.GetPropertyGuid().IsValid())
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_parameter_identity_invalid"),
				TEXT("The PCG parameter property GUID is invalid."),
				TEXT("/parameter/property_guid_valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Identity.GetOwnerIdentity(), Identity.GetPropertyGuid().ToString());
			}
			else
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_parameter_identity_invalid"),
				Identity.GetPropertyName().IsNone() ? TEXT("The PCG parameter has no property name.") : TEXT("The PCG parameter has no property type."),
				Identity.GetPropertyName().IsNone() ? TEXT("/parameter/name") : TEXT("/parameter/type"), MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty string")), Identity.GetOwnerIdentity(), FString());
			}
			OutError.CandidateName = Identity.GetPropertyName().IsNone() ? FString() : Identity.GetPropertyName().ToString();
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
			OutError = GraphFact(TEXT("pcg_ai_node_graph_component_binding_identity_invalid"),
			Identity.GetActorIdentity().IsEmpty() ? TEXT("The PCG component binding has no actor.")
				: (Identity.GetComponentIdentity().IsEmpty() ? TEXT("The PCG component binding has no component.")
					: (Identity.GetGraphIdentity().IsEmpty() ? TEXT("The PCG component binding has no graph.") : TEXT("The PCG component binding has no generation policy."))),
			Identity.GetActorIdentity().IsEmpty() ? TEXT("/component_binding/actor")
				: (Identity.GetComponentIdentity().IsEmpty() ? TEXT("/component_binding/component")
					: (Identity.GetGraphIdentity().IsEmpty() ? TEXT("/component_binding/graph") : TEXT("/component_binding/generation_policy"))), MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty string")), Identity.GetComponentIdentity(), FString());
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
			if (Identity.GetContentRevision().IsEmpty() && !Identity.GetDataAssetIdentity().IsEmpty()
				&& !Identity.GetDataTypeIdentity().IsEmpty() && !Identity.GetSourceIdentity().IsEmpty())
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_data_asset_identity_invalid"),
				TEXT("The PCG data asset has no content revision."),
				TEXT("/data_asset/content_revision_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Identity.GetDataAssetIdentity(), FString());
			}
			else
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_data_asset_identity_invalid"),
				Identity.GetDataAssetIdentity().IsEmpty() ? TEXT("The PCG data asset identity has no asset path.")
					: (Identity.GetDataTypeIdentity().IsEmpty() ? TEXT("The PCG data asset identity has no data type.") : TEXT("The PCG data asset identity has no source asset reference.")),
				Identity.GetDataAssetIdentity().IsEmpty() ? TEXT("/data_asset/asset_path")
					: (Identity.GetDataTypeIdentity().IsEmpty() ? TEXT("/data_asset/data_type") : TEXT("/data_asset/source")), MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty string")), Identity.GetDataAssetIdentity(), FString());
			}
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
			OutError = GraphFact(TEXT("pcg_ai_node_graph_target_identity_missing"),
				Topology.GetTargetKind().IsEmpty() ? TEXT("The captured graph has no target kind.")
				: (Topology.GetTargetIdentity().IsEmpty() ? TEXT("The captured graph has no target asset.") : TEXT("The captured graph has no structure object.")),
				Topology.GetTargetKind().IsEmpty() ? TEXT("/topology/target_kind")
				: (Topology.GetTargetIdentity().IsEmpty() ? TEXT("/topology/target") : TEXT("/topology/structure")), MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty string")), Topology.GetTargetIdentity(), FString());
			return false;
		}

		TSet<FString> ItemKeys;
		int32 ItemIndex = 0;
		for (const FPcgAiNodeGraphItemIdentity& Item : Topology.GetItems())
		{
			const FString ItemPath = FString::Printf(TEXT("/topology/items/%d"), ItemIndex++);
			if (Item.GetItemKey().IsEmpty()
				|| (Item.GetNativeIdentity().IsEmpty()
					&& Item.GetStableBuiltinRole().IsEmpty()))
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_item_identity_missing"),
					Item.GetItemKey().IsEmpty() ? TEXT("The graph item has no item key.") : TEXT("The graph item has neither a native identity nor a stable built-in role."),
					ItemPath + (Item.GetItemKey().IsEmpty() ? TEXT("/item_key_present") : TEXT("/identity_available")), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Topology.GetTargetIdentity(), Item.GetItemKey());
				OutError.CandidateName = Item.GetStableBuiltinRole();
				return false;
			}
			if (Item.GetNativeCarrierTypeIdentity().IsEmpty()
				|| Item.GetNativeTypeIdentity().IsEmpty())
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_type_identity_missing"),
					Item.GetNativeCarrierTypeIdentity().IsEmpty() ? TEXT("The graph item has no native carrier class.") : TEXT("The graph item has no native payload type."),
					ItemPath + (Item.GetNativeCarrierTypeIdentity().IsEmpty() ? TEXT("/carrier_class") : TEXT("/payload_type")), MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty class or type")), Topology.GetTargetIdentity(), Item.GetItemKey());
				OutError.CandidateName = Item.GetStableBuiltinRole();
				return false;
			}
			if (ItemKeys.Contains(Item.GetItemKey()))
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_item_identity_duplicate"),
					TEXT("The graph contains a duplicate item identity."),
					ItemPath + TEXT("/item_key_unique"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Topology.GetTargetIdentity(), Item.GetItemKey());
				return false;
			}
			ItemKeys.Add(Item.GetItemKey());
		}

		TMap<FString, EPcgAiNodeGraphPinDirection> PinDirections;
		int32 PinIndex = 0;
		for (const FPcgAiNodeGraphPinIdentity& Pin : Topology.GetPins())
		{
			const FString PinPath = FString::Printf(TEXT("/topology/pins/%d"), PinIndex++);
			if (Pin.GetPinKey().IsEmpty()
				|| !ItemKeys.Contains(Pin.GetOwnerItemKey())
				|| Pin.GetNativeLabel().IsEmpty()
				|| Pin.GetNativeTypeIdentity().IsEmpty())
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_pin_identity_invalid"),
					Pin.GetPinKey().IsEmpty() ? TEXT("The graph pin has no pin key.")
					: (!ItemKeys.Contains(Pin.GetOwnerItemKey()) ? TEXT("The graph pin refers to an unknown owner item.")
						: (Pin.GetNativeLabel().IsEmpty() ? TEXT("The graph pin has no native label.") : TEXT("The graph pin has no native type."))),
					PinPath + (Pin.GetPinKey().IsEmpty() ? TEXT("/pin_key_present")
					: (!ItemKeys.Contains(Pin.GetOwnerItemKey()) ? TEXT("/owner_resolved")
						: (Pin.GetNativeLabel().IsEmpty() ? TEXT("/label_present") : TEXT("/type_present")))), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Topology.GetTargetIdentity(), Pin.GetPinKey());
				OutError.CandidateName = Pin.GetNativeLabel();
				return false;
			}
			if (PinDirections.Contains(Pin.GetPinKey()))
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_pin_identity_duplicate"),
					TEXT("The graph contains a duplicate pin identity."),
					PinPath + TEXT("/pin_key_unique"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Topology.GetTargetIdentity(), Pin.GetPinKey());
				OutError.CandidateName = Pin.GetNativeLabel();
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

		int32 RelationIndex = 0;
		for (const FPcgAiNodeGraphRelationIdentity& Relation
			: Topology.GetRelations())
		{
			const FString RelationPath = FString::Printf(TEXT("/topology/relations/%d"), RelationIndex++);
			if (Relation.GetRelationKey().IsEmpty()
				|| Relation.GetRelationKind().IsEmpty()
				|| RelationKeys.Contains(Relation.GetRelationKey()))
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_relation_identity_duplicate"),
					Relation.GetRelationKey().IsEmpty() ? TEXT("The graph relation has no relation key.")
					: (Relation.GetRelationKind().IsEmpty() ? TEXT("The graph relation has no kind.") : TEXT("The graph contains a duplicate relation identity.")),
					RelationPath + (Relation.GetRelationKey().IsEmpty() ? TEXT("/relation_key_present")
					: (Relation.GetRelationKind().IsEmpty() ? TEXT("/kind_present") : TEXT("/relation_key_unique"))), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Topology.GetTargetIdentity(), Relation.GetRelationKey());
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
					OutError = GraphFact(TEXT("pcg_ai_node_graph_relation_endpoint_unresolved"),
						!FromDirection ? TEXT("The graph link source pin does not resolve.") : TEXT("The graph link destination pin does not resolve."),
						RelationPath + (!FromDirection ? TEXT("/from_resolved") : TEXT("/to_resolved")), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Topology.GetTargetIdentity(), Relation.GetRelationKey());
					return false;
				}
				if (*FromDirection != EPcgAiNodeGraphPinDirection::Output
					|| *ToDirection != EPcgAiNodeGraphPinDirection::Input)
				{
					OutError = GraphFact(TEXT("pcg_ai_node_graph_link_direction_invalid"),
						*FromDirection != EPcgAiNodeGraphPinDirection::Output ? TEXT("The graph link source must be an output pin.") : TEXT("The graph link destination must be an input pin."),
						RelationPath + (*FromDirection != EPcgAiNodeGraphPinDirection::Output ? TEXT("/from_direction") : TEXT("/to_direction")), MakeShared<FJsonValueString>(*FromDirection != EPcgAiNodeGraphPinDirection::Output ? DirectionName(*FromDirection) : DirectionName(*ToDirection)), MakeShared<FJsonValueString>(*FromDirection != EPcgAiNodeGraphPinDirection::Output ? TEXT("Output") : TEXT("Input")), Topology.GetTargetIdentity(), Relation.GetRelationKey());
					return false;
				}
				continue;
			}

			if (!ItemKeys.Contains(Relation.GetFromIdentity())
				|| !ItemKeys.Contains(Relation.GetToIdentity()))
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_relation_endpoint_unresolved"),
					!ItemKeys.Contains(Relation.GetFromIdentity()) ? TEXT("The relation source item does not resolve.") : TEXT("The relation destination item does not resolve."),
					RelationPath + (!ItemKeys.Contains(Relation.GetFromIdentity()) ? TEXT("/from_resolved") : TEXT("/to_resolved")), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Topology.GetTargetIdentity(), Relation.GetRelationKey());
				return false;
			}
			if ((Relation.GetRelationKind() == TEXT("hierarchy_child")
					|| Relation.GetRelationKind() == TEXT("ordered_attachment"))
				&& Relation.GetOrderIndex() < 0)
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_relation_order_missing"),
					TEXT("The ordered graph relation requires a non-negative order index."),
					RelationPath + TEXT("/order_index"), MakeShared<FJsonValueNumber>(Relation.GetOrderIndex()), MakeShared<FJsonValueString>(TEXT("integer >= 0")), Topology.GetTargetIdentity(), Relation.GetRelationKey());
				return false;
			}
			if (Relation.GetRelationKind() == TEXT("state_transition")
				&& Relation.GetNativeRelationIdentity().IsEmpty())
			{
				OutError = GraphFact(TEXT("pcg_ai_node_graph_relation_native_identity_missing"),
					TEXT("The state transition relation has no native transition identity."),
					RelationPath + TEXT("/native_identity_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Topology.GetTargetIdentity(), Relation.GetRelationKey());
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
			OutError = GraphFact(TEXT("pcg_ai_node_graph_hierarchy_cycle"),
				TEXT("The graph hierarchy contains a cycle; topological traversal could not visit every item."),
				TEXT("/topology/hierarchy/visited_items"), MakeShared<FJsonValueNumber>(Visited), MakeShared<FJsonValueNumber>(HierarchyInDegree.Num()), Topology.GetTargetIdentity(), FString());
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
			OutError = GraphFact(TEXT("pcg_ai_node_graph_capture_failed"), TEXT("The native graph capture did not complete."), TEXT("/lifecycle/capture_complete"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (!Evidence.IsBarrierComplete()
			|| Evidence.GetBarrierErrorCount() != 0)
		{
			const TCHAR* Code = TEXT("pcg_ai_node_graph_barrier_incomplete");
			const TCHAR* Kind = TEXT("unknown");
			switch (Evidence.GetBarrierKind())
			{
			case EPcgAiNodeGraphBarrierKind::Compile:
				Code = TEXT("pcg_ai_node_graph_compile_incomplete"); Kind = TEXT("Compile"); break;
			case EPcgAiNodeGraphBarrierKind::Refresh:
				Code = TEXT("pcg_ai_node_graph_refresh_incomplete"); Kind = TEXT("Refresh"); break;
			case EPcgAiNodeGraphBarrierKind::Validate:
				Code = TEXT("pcg_ai_node_graph_validate_incomplete"); Kind = TEXT("Validate"); break;
			}
			const auto Actual = MakeShared<FJsonObject>();
			Actual->SetStringField(TEXT("barrier_kind"), Kind);
			Actual->SetBoolField(TEXT("complete"), Evidence.IsBarrierComplete());
			Actual->SetNumberField(TEXT("error_count"), Evidence.GetBarrierErrorCount());
			const auto Expected = MakeShared<FJsonObject>();
			Expected->SetStringField(TEXT("barrier_kind"), Kind);
			Expected->SetBoolField(TEXT("complete"), true);
			Expected->SetNumberField(TEXT("error_count"), 0);
			OutError = GraphFact(Code,
				!Evidence.IsBarrierComplete() ? TEXT("The native graph barrier did not complete.") : TEXT("The native graph barrier reported errors."),
				TEXT("/lifecycle/barrier"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
			if (FCString::Strcmp(Kind, TEXT("unknown")) == 0)
			{
				Actual->SetNumberField(TEXT("barrier_kind_value"), static_cast<int32>(Evidence.GetBarrierKind()));
				Expected->SetStringField(TEXT("barrier_kind"), TEXT("Compile | Refresh | Validate"));
			}
			return false;
		}
		if (!Evidence.IsSettleComplete())
		{
			OutError = GraphFact(TEXT("pcg_ai_node_graph_settle_incomplete"), TEXT("The native graph settle phase did not complete."), TEXT("/lifecycle/settle_complete"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Evidence.GetBeforeRevision().IsEmpty()
			|| Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision())
		{
			const bool bBeforeMissing = Evidence.GetBeforeRevision().IsEmpty();
			const bool bReadbackMissing = Evidence.GetReadbackRevision().IsEmpty();
			OutError = GraphFact(TEXT("pcg_ai_node_graph_readback_revision_not_advanced"),
				bBeforeMissing ? TEXT("The native graph lifecycle has no before revision.")
					: (bReadbackMissing ? TEXT("The native graph readback has no revision.") : TEXT("The native graph readback revision has not advanced.")),
				bBeforeMissing ? TEXT("/lifecycle/before_revision_present")
					: (bReadbackMissing ? TEXT("/lifecycle/readback_revision_present") : TEXT("/lifecycle/revision_advanced")),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Evidence.GetReadbackRevision());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Evidence.GetBeforeRevision());
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
			const TCHAR* Reason = TEXT("The native graph readback was not obtained independently.");
			const TCHAR* Path = TEXT("/lifecycle/readback_independent");
			if (Evidence.IsReadbackIndependent())
			{
				if (Evidence.GetApplyEvidence().IsEmpty())
				{
					Reason = TEXT("The native graph lifecycle has no apply evidence."); Path = TEXT("/lifecycle/apply_evidence_present");
				}
				else if (Evidence.GetSettleEvidence().IsEmpty())
				{
					Reason = TEXT("The native graph lifecycle has no settle evidence."); Path = TEXT("/lifecycle/settle_evidence_present");
				}
				else if (Evidence.GetReadbackEvidence().IsEmpty())
				{
					Reason = TEXT("The native graph lifecycle has no readback evidence."); Path = TEXT("/lifecycle/readback_evidence_present");
				}
				else if (Evidence.GetApplyEvidence() == Evidence.GetSettleEvidence())
				{
					Reason = TEXT("The native graph apply and settle phases reused the same evidence."); Path = TEXT("/lifecycle/apply_settle_evidence_distinct");
				}
				else if (Evidence.GetApplyEvidence() == Evidence.GetReadbackEvidence())
				{
					Reason = TEXT("The native graph apply and readback phases reused the same evidence."); Path = TEXT("/lifecycle/apply_readback_evidence_distinct");
				}
				else
				{
					Reason = TEXT("The native graph settle and readback phases reused the same evidence."); Path = TEXT("/lifecycle/settle_readback_evidence_distinct");
				}
			}
			OutError = GraphFact(TEXT("pcg_ai_node_graph_readback_not_independent"), Reason, Path,
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			const auto PrivateEvidence = MakeShared<FJsonObject>();
			PrivateEvidence->SetStringField(TEXT("apply"), Evidence.GetApplyEvidence());
			PrivateEvidence->SetStringField(TEXT("settle"), Evidence.GetSettleEvidence());
			PrivateEvidence->SetStringField(TEXT("readback"), Evidence.GetReadbackEvidence());
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(PrivateEvidence);
			return false;
		}
		return true;
	}
}
