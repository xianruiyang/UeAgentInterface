#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphAdapter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::BlueprintUmgGraph
{
	namespace
	{
		FBlueprintUmgGraphAdapterError NativeFact(
			const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_adapter"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FBlueprintUmgGraphAdapterError TopologyFact(
			const FBlueprintUmgGraphTopology& Topology,
			const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& SubjectId = FString())
		{
			auto Error = NativeFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected));
			Error.SubjectId = SubjectId;
			auto Identity = MakeShared<FJsonObject>();
			Identity->SetStringField(TEXT("target_identity"), Topology.GetTargetIdentity());
			Identity->SetStringField(TEXT("structure_identity"), Topology.GetStructureIdentity());
			Error.ActualEvidence = MakeShared<FJsonValueObject>(Identity);
			return Error;
		}

		FString DirectionName(const EBlueprintUmgGraphPinDirection Direction)
		{
			switch (Direction)
			{
			case EBlueprintUmgGraphPinDirection::Input: return TEXT("input");
			case EBlueprintUmgGraphPinDirection::Output: return TEXT("output");
			default: return FString::Printf(TEXT("invalid(%u)"), static_cast<uint32>(Direction));
			}
		}

		TSharedPtr<FJsonValue> EndpointFacts(const bool bFrom, const bool bTo,
			const FString& FromField = TEXT("from_resolved"), const FString& ToField = TEXT("to_resolved"))
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(FromField, bFrom);
			Facts->SetBoolField(ToField, bTo);
			return MakeShared<FJsonValueObject>(Facts);
		}

		TSharedPtr<FJsonValue> LifecycleFacts(const FBlueprintUmgGraphLifecycleEvidence& Evidence)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetStringField(TEXT("before_revision"), Evidence.GetBeforeRevision());
			Facts->SetStringField(TEXT("readback_revision"), Evidence.GetReadbackRevision());
			Facts->SetStringField(TEXT("apply_evidence"), Evidence.GetApplyEvidence());
			Facts->SetStringField(TEXT("settle_evidence"), Evidence.GetSettleEvidence());
			Facts->SetStringField(TEXT("readback_evidence"), Evidence.GetReadbackEvidence());
			return MakeShared<FJsonValueObject>(Facts);
		}
	}

	bool TryValidateBlueprintUmgGraphTopology(
		const FBlueprintUmgGraphTopology& Topology,
		FBlueprintUmgGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Topology.GetTargetKind().IsEmpty()
			|| Topology.GetTargetIdentity().IsEmpty()
			|| Topology.GetStructureIdentity().IsEmpty())
		{
			const FString Field = Topology.GetTargetKind().IsEmpty() ? TEXT("target_kind")
				: Topology.GetTargetIdentity().IsEmpty() ? TEXT("target_identity") : TEXT("structure_identity");
			OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_target_identity_missing"),
				FString::Printf(TEXT("The native topology has no %s."), *Field), TEXT("/") + Field + TEXT("/available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		TSet<FString> ItemKeys;
		for (int32 Index = 0; Index < Topology.GetItems().Num(); ++Index)
		{
			const FBlueprintUmgGraphItemIdentity& Item = Topology.GetItems()[Index];
			const FString Path = FString::Printf(TEXT("/items/%d"), Index);
			if (Item.GetItemKey().IsEmpty()
				|| (Item.GetNativeGuid().IsEmpty() && Item.GetStableBuiltinRole().IsEmpty()))
			{
				const bool bKeyMissing = Item.GetItemKey().IsEmpty();
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_builtin_identity_missing"),
					bKeyMissing ? TEXT("The native topology item has no identity key.")
						: TEXT("The native topology item has neither a native GUID nor a stable builtin role."),
					Path + (bKeyMissing ? TEXT("/item_key/available") : TEXT("/native_identity/available")),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Item.GetItemKey());
				return false;
			}
			if (ItemKeys.Contains(Item.GetItemKey()))
			{
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_item_identity_duplicate"),
					TEXT("This native topology item repeats an earlier item's identity key."), Path + TEXT("/item_key/duplicate"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Item.GetItemKey());
				return false;
			}
			ItemKeys.Add(Item.GetItemKey());
		}

		TMap<FString, EBlueprintUmgGraphPinDirection> PinDirections;
		for (int32 Index = 0; Index < Topology.GetPins().Num(); ++Index)
		{
			const FBlueprintUmgGraphPinIdentity& Pin = Topology.GetPins()[Index];
			const FString Path = FString::Printf(TEXT("/pins/%d"), Index);
			if (Pin.GetPinKey().IsEmpty() || !ItemKeys.Contains(Pin.GetOwnerItemKey()))
			{
				const bool bKeyMissing = Pin.GetPinKey().IsEmpty();
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_pin_owner_unresolved"),
					bKeyMissing ? TEXT("The native pin has no identity key.")
						: TEXT("The native pin refers to an owner absent from the topology's items."),
					Path + (bKeyMissing ? TEXT("/pin_key/available") : TEXT("/owner/resolved")),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Pin.GetOwnerItemKey());
				OutError.CandidateName = Pin.GetNativeName();
				OutError.InternalContext = Pin.GetPinKey();
				return false;
			}
			if (PinDirections.Contains(Pin.GetPinKey()))
			{
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_pin_identity_duplicate"),
					TEXT("This native pin repeats an earlier pin's identity key."), Path + TEXT("/pin_key/duplicate"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Pin.GetOwnerItemKey());
				OutError.CandidateName = Pin.GetNativeName();
				OutError.InternalContext = Pin.GetPinKey();
				return false;
			}
			PinDirections.Add(Pin.GetPinKey(), Pin.GetDirection());
		}

		TSet<FString> RelationKeys;
		for (int32 Index = 0; Index < Topology.GetLinks().Num(); ++Index)
		{
			const FBlueprintUmgGraphLinkIdentity& Link = Topology.GetLinks()[Index];
			const FString Path = FString::Printf(TEXT("/links/%d"), Index);
			const EBlueprintUmgGraphPinDirection* FromDirection = PinDirections.Find(Link.GetFromPinKey());
			const EBlueprintUmgGraphPinDirection* ToDirection = PinDirections.Find(Link.GetToPinKey());
			if (!FromDirection || !ToDirection)
			{
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_link_endpoint_unresolved"),
					TEXT("The native link refers to a source or destination pin absent from the topology."),
					Path + TEXT("/endpoints"), EndpointFacts(FromDirection != nullptr, ToDirection != nullptr),
					EndpointFacts(true, true), Link.GetLinkKey());
				return false;
			}
			if (*FromDirection != EBlueprintUmgGraphPinDirection::Output
				|| *ToDirection != EBlueprintUmgGraphPinDirection::Input)
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("from_direction"), DirectionName(*FromDirection));
				Actual->SetStringField(TEXT("to_direction"), DirectionName(*ToDirection));
				auto Expected = MakeShared<FJsonObject>();
				Expected->SetStringField(TEXT("from_direction"), TEXT("output"));
				Expected->SetStringField(TEXT("to_direction"), TEXT("input"));
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_link_direction_invalid"),
					TEXT("A native link must connect an output pin to an input pin."), Path + TEXT("/direction"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), Link.GetLinkKey());
				return false;
			}
			if (Link.GetLinkKey().IsEmpty() || RelationKeys.Contains(Link.GetLinkKey()))
			{
				const bool bMissing = Link.GetLinkKey().IsEmpty();
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_relation_identity_duplicate"),
					bMissing ? TEXT("The native link has no relation key.") : TEXT("The native link repeats an earlier relation key."),
					Path + (bMissing ? TEXT("/link_key/available") : TEXT("/link_key/duplicate")),
					MakeShared<FJsonValueBoolean>(!bMissing), MakeShared<FJsonValueBoolean>(bMissing), Link.GetLinkKey());
				return false;
			}
			RelationKeys.Add(Link.GetLinkKey());
		}

		TMap<FString, int32> InDegree;
		TMultiMap<FString, FString> Children;
		for (const FString& ItemKey : ItemKeys) InDegree.Add(ItemKey, 0);
		for (int32 Index = 0; Index < Topology.GetHierarchy().Num(); ++Index)
		{
			const FBlueprintUmgGraphHierarchyRelation& Relation = Topology.GetHierarchy()[Index];
			const FString Path = FString::Printf(TEXT("/hierarchy/%d"), Index);
			if (Relation.GetRelationKey().IsEmpty() || RelationKeys.Contains(Relation.GetRelationKey()))
			{
				const bool bMissing = Relation.GetRelationKey().IsEmpty();
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_relation_identity_duplicate"),
					bMissing ? TEXT("The native hierarchy relation has no key.")
						: TEXT("The native hierarchy relation repeats an earlier link or hierarchy relation key."),
					Path + (bMissing ? TEXT("/relation_key/available") : TEXT("/relation_key/duplicate")),
					MakeShared<FJsonValueBoolean>(!bMissing), MakeShared<FJsonValueBoolean>(bMissing), Relation.GetItemKey());
				OutError.InternalContext = Relation.GetRelationKey();
				return false;
			}
			RelationKeys.Add(Relation.GetRelationKey());
			if (!ItemKeys.Contains(Relation.GetItemKey()) || !ItemKeys.Contains(Relation.GetParentKey()))
			{
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_hierarchy_endpoint_unresolved"),
					TEXT("The native hierarchy relation refers to a child or parent absent from the topology."),
					Path + TEXT("/endpoints"), EndpointFacts(ItemKeys.Contains(Relation.GetParentKey()), ItemKeys.Contains(Relation.GetItemKey()), TEXT("parent_resolved"), TEXT("child_resolved")),
					EndpointFacts(true, true, TEXT("parent_resolved"), TEXT("child_resolved")), Relation.GetItemKey());
				OutError.InternalContext = Relation.GetRelationKey();
				return false;
			}
			++InDegree.FindChecked(Relation.GetItemKey());
			Children.Add(Relation.GetParentKey(), Relation.GetItemKey());
		}

		TArray<FString> Ready;
		for (const TPair<FString, int32>& Pair : InDegree)
			if (Pair.Value == 0) Ready.Add(Pair.Key);
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
				if (Degree == 0) Ready.Add(Child);
			}
		}
		if (Visited != ItemKeys.Num())
		{
			OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_hierarchy_cycle"),
				TEXT("The native hierarchy contains a cycle; not all items can be visited in parent-first order."),
				TEXT("/hierarchy/visited_item_count"), MakeShared<FJsonValueNumber>(Visited), MakeShared<FJsonValueNumber>(ItemKeys.Num()));
			return false;
		}

		TSet<FString> BindingKeys;
		for (int32 Index = 0; Index < Topology.GetBindings().Num(); ++Index)
		{
			const FBlueprintUmgGraphBindingIdentity& Binding = Topology.GetBindings()[Index];
			const FString Path = FString::Printf(TEXT("/bindings/%d"), Index);
			if (Binding.GetBindingKey().IsEmpty() || BindingKeys.Contains(Binding.GetBindingKey()))
			{
				const bool bMissing = Binding.GetBindingKey().IsEmpty();
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_binding_identity_duplicate"),
					bMissing ? TEXT("The native binding has no identity key.") : TEXT("The native binding repeats an earlier binding key."),
					Path + (bMissing ? TEXT("/binding_key/available") : TEXT("/binding_key/duplicate")),
					MakeShared<FJsonValueBoolean>(!bMissing), MakeShared<FJsonValueBoolean>(bMissing), Binding.GetSourceIdentity());
				OutError.InternalContext = Binding.GetBindingKey();
				return false;
			}
			if (!ItemKeys.Contains(Binding.GetSourceIdentity()) || Binding.GetSourceField().IsEmpty()
				|| Binding.GetTargetIdentity().IsEmpty() || Binding.GetTargetField().IsEmpty())
			{
				const bool bSourceMissing = !ItemKeys.Contains(Binding.GetSourceIdentity());
				const FString Field = bSourceMissing ? TEXT("source/resolved")
					: Binding.GetSourceField().IsEmpty() ? TEXT("source_field/available")
					: Binding.GetTargetIdentity().IsEmpty() ? TEXT("target/available") : TEXT("target_field/available");
				OutError = TopologyFact(Topology, TEXT("blueprint_umg_graph_binding_endpoint_unresolved"),
					FString::Printf(TEXT("The native binding is missing %s."), *Field), Path + TEXT("/") + Field,
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Binding.GetSourceIdentity());
				OutError.CandidateName = Binding.GetSourceField();
				OutError.InternalContext = Binding.GetBindingKey();
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
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("complete"), Evidence.IsCompileComplete());
			Actual->SetNumberField(TEXT("error_count"), Evidence.GetErrorCount());
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetBoolField(TEXT("complete"), true);
			Expected->SetNumberField(TEXT("error_count"), 0);
			OutError = NativeFact(TEXT("blueprint_umg_graph_compile_incomplete"),
				Evidence.GetErrorCount() != 0 ? FString::Printf(TEXT("The native Blueprint compile reported %d errors."), Evidence.GetErrorCount())
					: TEXT("The native Blueprint compile did not complete."),
				TEXT("/compile"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
			return false;
		}
		if (Evidence.GetBeforeRevision().IsEmpty() || Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision())
		{
			const FString Field = Evidence.GetBeforeRevision().IsEmpty() ? TEXT("/before_revision/available")
				: Evidence.GetReadbackRevision().IsEmpty() ? TEXT("/readback_revision/available") : TEXT("/readback_revision/advanced");
			OutError = NativeFact(TEXT("blueprint_umg_graph_readback_revision_not_advanced"),
				Field.EndsWith(TEXT("/advanced")) ? TEXT("Native readback returned the same revision as before the mutation.")
					: TEXT("Native lifecycle evidence is missing a before or readback revision."),
				Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = LifecycleFacts(Evidence);
			return false;
		}
		if (Evidence.GetApplyEvidence().IsEmpty() || Evidence.GetSettleEvidence().IsEmpty() || Evidence.GetReadbackEvidence().IsEmpty()
			|| Evidence.GetApplyEvidence() == Evidence.GetSettleEvidence()
			|| Evidence.GetApplyEvidence() == Evidence.GetReadbackEvidence()
			|| Evidence.GetSettleEvidence() == Evidence.GetReadbackEvidence())
		{
			const FString Field = Evidence.GetApplyEvidence().IsEmpty() ? TEXT("/apply_evidence/available")
				: Evidence.GetSettleEvidence().IsEmpty() ? TEXT("/settle_evidence/available")
				: Evidence.GetReadbackEvidence().IsEmpty() ? TEXT("/readback_evidence/available")
				: Evidence.GetApplyEvidence() == Evidence.GetSettleEvidence() ? TEXT("/apply_settle_evidence/distinct")
				: Evidence.GetApplyEvidence() == Evidence.GetReadbackEvidence() ? TEXT("/apply_readback_evidence/distinct") : TEXT("/settle_readback_evidence/distinct");
			OutError = NativeFact(TEXT("blueprint_umg_graph_readback_not_independent"),
				Field.EndsWith(TEXT("/available")) ? TEXT("Native lifecycle evidence is missing an apply, settle or readback observation.")
					: TEXT("Native lifecycle stages reused the same evidence identity instead of independent observations."),
				Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = LifecycleFacts(Evidence);
			return false;
		}
		return true;
	}
}
