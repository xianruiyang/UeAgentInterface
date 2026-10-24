#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphAdapter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

	namespace
	{
		FMaterialNiagaraGraphAdapterError AdapterFailure(const FString& Code, const FString& Reason,
			const FString& Path, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
			const FString& InternalIdentity = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_adapter"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.SubjectId = InternalIdentity;
			return Error;
		}

		FMaterialNiagaraGraphAdapterError AdapterCondition(const FString& Code, const FString& Reason,
			const FString& Path, const FString& InternalIdentity = FString(), bool Actual = false, bool Expected = true)
		{
			return AdapterFailure(Code, Reason, Path, MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected), InternalIdentity);
		}

		TSharedPtr<FJsonValue> DirectionValue(EMaterialNiagaraGraphPinDirection Direction)
		{
			return MakeShared<FJsonValueString>(Direction == EMaterialNiagaraGraphPinDirection::Input ? TEXT("input")
				: Direction == EMaterialNiagaraGraphPinDirection::Output ? TEXT("output")
				: FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Direction)));
		}
	}

	bool TryValidateMaterialNiagaraGraphParameterIdentities(
		const TArray<FMaterialNiagaraGraphParameterIdentity>& Parameters, FMaterialNiagaraGraphAdapterError& OutError)
	{
		OutError.Reset();
		TSet<FString> Keys;
		for (int32 Index = 0; Index < Parameters.Num(); ++Index)
		{
			const auto& Parameter = Parameters[Index];
			const FString& Association = Parameter.GetParameterAssociation();
			const bool bGlobal = Association == TEXT("global");
			const bool bIndexed = Association == TEXT("layer") || Association == TEXT("blend");
			const FString Path = TEXT("/parameters/") + FString::FromInt(Index);
			if (Parameter.GetParameterName().IsEmpty())
				OutError = AdapterFailure(TEXT("material_niagara_graph_parameter_identity_invalid"),
					TEXT("The material parameter name must not be empty."), Path + TEXT("/parameter_name"),
					MakeShared<FJsonValueString>(Parameter.GetParameterName()), MakeShared<FJsonValueString>(TEXT("nonempty parameter name")));
			else if (!bGlobal && !bIndexed)
				OutError = AdapterFailure(TEXT("material_niagara_graph_parameter_identity_invalid"),
					TEXT("The material parameter association is unsupported."), Path + TEXT("/association"),
					MakeShared<FJsonValueString>(Association), MakeShared<FJsonValueString>(TEXT("global, layer or blend")));
			else if ((bGlobal && Parameter.GetParameterIndex() != INDEX_NONE) || (bIndexed && Parameter.GetParameterIndex() < 0))
				OutError = AdapterFailure(TEXT("material_niagara_graph_parameter_identity_invalid"),
					bGlobal ? TEXT("A global material parameter must use INDEX_NONE.") : TEXT("A layer or blend parameter must use a nonnegative index."),
					Path + TEXT("/parameter_index"), MakeShared<FJsonValueNumber>(Parameter.GetParameterIndex()),
					bGlobal ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(INDEX_NONE))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("integer >= 0"))));
			if (OutError.IsValid())
			{
				OutError.CandidateName = Parameter.GetParameterName();
				OutError.SubjectId = Parameter.GetParameterKey();
				return false;
			}
			const FString Key = Parameter.GetParameterKey();
			if (Keys.Contains(Key))
			{
				const auto Identity = MakeShared<FJsonObject>();
				Identity->SetStringField(TEXT("name"), Parameter.GetParameterName());
				Identity->SetStringField(TEXT("association"), Association);
				Identity->SetNumberField(TEXT("index"), Parameter.GetParameterIndex());
				Identity->SetStringField(TEXT("type"), ParameterTypeToken(Parameter.GetParameterType()));
				OutError = AdapterFailure(TEXT("material_niagara_graph_parameter_identity_duplicate"),
					TEXT("The material parameter duplicates an earlier name, type, association and index."),
					Path, MakeShared<FJsonValueObject>(Identity), MakeShared<FJsonValueString>(TEXT("unique parameter identity")), Key);
				OutError.CandidateName = Parameter.GetParameterName();
				return false;
			}
			Keys.Add(Key);
		}
		return true;
	}

	bool TryValidateMaterialNiagaraGraphTopology(const FMaterialNiagaraGraphTopology& Topology, FMaterialNiagaraGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (Topology.GetTargetKind().IsEmpty() || Topology.GetTargetIdentity().IsEmpty() || Topology.GetStructureIdentity().IsEmpty())
		{
			OutError = AdapterCondition(TEXT("material_niagara_graph_target_identity_missing"),
				Topology.GetTargetKind().IsEmpty() ? TEXT("The captured topology has no target kind.")
					: Topology.GetTargetIdentity().IsEmpty() ? TEXT("The captured topology has no native target identity.")
					: TEXT("The captured topology has no native structure identity."),
				Topology.GetTargetKind().IsEmpty() ? TEXT("/target_kind_present")
					: Topology.GetTargetIdentity().IsEmpty() ? TEXT("/target_identity_present") : TEXT("/structure_identity_present"),
				Topology.GetTargetIdentity());
			return false;
		}
		TSet<FString> ItemKeys;
		for (int32 Index = 0; Index < Topology.GetItems().Num(); ++Index)
		{
			const auto& Item = Topology.GetItems()[Index];
			const FString Path = TEXT("/items/") + FString::FromInt(Index);
			if (Item.GetItemKey().IsEmpty() || (Item.GetNativeGuid().IsEmpty() && Item.GetStableBuiltinRole().IsEmpty()))
			{
				OutError = AdapterCondition(TEXT("material_niagara_graph_item_identity_missing"),
					Item.GetItemKey().IsEmpty() ? TEXT("The captured item has no internal item key.")
						: TEXT("The captured item has neither a native identity nor a stable builtin role."),
					Path + (Item.GetItemKey().IsEmpty() ? TEXT("/item_key_present") : TEXT("/native_or_builtin_identity_present")), Item.GetItemKey());
				return false;
			}
			if (ItemKeys.Contains(Item.GetItemKey()))
			{
				OutError = AdapterCondition(TEXT("material_niagara_graph_item_identity_duplicate"),
					TEXT("The captured item duplicates an earlier internal item identity."), Path + TEXT("/item_identity_unique"), Item.GetItemKey());
				return false;
			}
			ItemKeys.Add(Item.GetItemKey());
		}
		TMap<FString, EMaterialNiagaraGraphPinDirection> PinDirections;
		for (int32 Index = 0; Index < Topology.GetPins().Num(); ++Index)
		{
			const auto& Pin = Topology.GetPins()[Index];
			const FString Path = TEXT("/pins/") + FString::FromInt(Index);
			if (Pin.GetPinKey().IsEmpty() || !ItemKeys.Contains(Pin.GetOwnerItemKey()))
			{
				OutError = AdapterCondition(TEXT("material_niagara_graph_pin_owner_unresolved"),
					Pin.GetPinKey().IsEmpty() ? TEXT("The captured pin has no internal pin key.") : TEXT("The captured pin refers to an owner item absent from the topology."),
					Path + (Pin.GetPinKey().IsEmpty() ? TEXT("/pin_key_present") : TEXT("/owner_item_present")), Pin.GetPinKey());
				OutError.CandidateName = Pin.GetNativeName();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Pin.GetOwnerItemKey());
				return false;
			}
			if (PinDirections.Contains(Pin.GetPinKey()))
			{
				OutError = AdapterCondition(TEXT("material_niagara_graph_pin_identity_duplicate"),
					TEXT("The captured pin duplicates an earlier internal pin identity."), Path + TEXT("/pin_identity_unique"), Pin.GetPinKey());
				OutError.CandidateName = Pin.GetNativeName();
				return false;
			}
			PinDirections.Add(Pin.GetPinKey(), Pin.GetDirection());
		}
		TSet<FString> RelationKeys;
		for (int32 Index = 0; Index < Topology.GetLinks().Num(); ++Index)
		{
			const auto& Link = Topology.GetLinks()[Index];
			const FString Path = TEXT("/links/") + FString::FromInt(Index);
			const auto* FromDirection = PinDirections.Find(Link.GetFromPinKey());
			const auto* ToDirection = PinDirections.Find(Link.GetToPinKey());
			if (!FromDirection || !ToDirection)
			{
				OutError = AdapterCondition(TEXT("material_niagara_graph_link_endpoint_unresolved"),
					!FromDirection ? TEXT("The captured link's source pin is absent from the topology.") : TEXT("The captured link's destination pin is absent from the topology."),
					Path + (!FromDirection ? TEXT("/source_pin_present") : TEXT("/destination_pin_present")), Link.GetLinkKey());
				OutError.ActualEvidence = MakeShared<FJsonValueString>(!FromDirection ? Link.GetFromPinKey() : Link.GetToPinKey());
				return false;
			}
			if (*FromDirection != EMaterialNiagaraGraphPinDirection::Output || *ToDirection != EMaterialNiagaraGraphPinDirection::Input)
			{
				const bool bFrom = *FromDirection != EMaterialNiagaraGraphPinDirection::Output;
				OutError = AdapterFailure(TEXT("material_niagara_graph_link_direction_invalid"),
					bFrom ? TEXT("The captured link's source pin must be an output.") : TEXT("The captured link's destination pin must be an input."),
					Path + (bFrom ? TEXT("/source_direction") : TEXT("/destination_direction")), DirectionValue(bFrom ? *FromDirection : *ToDirection),
					MakeShared<FJsonValueString>(bFrom ? TEXT("output") : TEXT("input")), Link.GetLinkKey());
				return false;
			}
			if (Link.GetLinkKey().IsEmpty() || RelationKeys.Contains(Link.GetLinkKey()))
			{
				OutError = AdapterCondition(TEXT("material_niagara_graph_relation_identity_duplicate"),
					Link.GetLinkKey().IsEmpty() ? TEXT("The captured link has no internal relation key.") : TEXT("The captured link duplicates an earlier relation identity."),
					Path + (Link.GetLinkKey().IsEmpty() ? TEXT("/relation_key_present") : TEXT("/relation_identity_unique")), Link.GetLinkKey());
				return false;
			}
			RelationKeys.Add(Link.GetLinkKey());
		}
		TSet<FString> OrderedItems;
		TMap<FString, FString> PredecessorByItem;
		for (int32 Index = 0; Index < Topology.GetOrdering().Num(); ++Index)
		{
			const auto& Ordering = Topology.GetOrdering()[Index];
			const FString Path = TEXT("/ordering/") + FString::FromInt(Index);
			if (Ordering.GetOrderingKey().IsEmpty() || RelationKeys.Contains(Ordering.GetOrderingKey()))
			{
				OutError = AdapterCondition(TEXT("material_niagara_graph_relation_identity_duplicate"),
					Ordering.GetOrderingKey().IsEmpty() ? TEXT("The captured ordering relation has no internal key.") : TEXT("The captured ordering relation duplicates an earlier relation identity."),
					Path + (Ordering.GetOrderingKey().IsEmpty() ? TEXT("/relation_key_present") : TEXT("/relation_identity_unique")), Ordering.GetOrderingKey());
				return false;
			}
			RelationKeys.Add(Ordering.GetOrderingKey());
			if (!ItemKeys.Contains(Ordering.GetItemKey()) || (!Ordering.GetPredecessorItemKey().IsEmpty() && !ItemKeys.Contains(Ordering.GetPredecessorItemKey())))
			{
				const bool bItemMissing = !ItemKeys.Contains(Ordering.GetItemKey());
				OutError = AdapterCondition(TEXT("material_niagara_graph_stack_item_identity_missing"),
					bItemMissing ? TEXT("The ordering relation refers to an item absent from the captured stack.")
						: TEXT("The ordering relation's predecessor is absent from the captured stack."),
					Path + (bItemMissing ? TEXT("/item_present") : TEXT("/predecessor_present")), Ordering.GetOrderingKey());
				OutError.ActualEvidence = MakeShared<FJsonValueString>(bItemMissing ? Ordering.GetItemKey() : Ordering.GetPredecessorItemKey());
				return false;
			}
			if (OrderedItems.Contains(Ordering.GetItemKey()))
			{
				OutError = AdapterCondition(TEXT("material_niagara_graph_stack_order_duplicate"),
					TEXT("The captured stack assigns more than one ordering relation to the same item."),
					Path + TEXT("/item_order_unique"), Ordering.GetItemKey());
				return false;
			}
			OrderedItems.Add(Ordering.GetItemKey());
			PredecessorByItem.Add(Ordering.GetItemKey(), Ordering.GetPredecessorItemKey());
		}
		for (const FString& Start : OrderedItems)
		{
			TSet<FString> Seen;
			TArray<FString> Traversal;
			FString Current = Start;
			while (!Current.IsEmpty())
			{
				if (Seen.Contains(Current))
				{
					OutError = AdapterCondition(TEXT("material_niagara_graph_stack_order_cycle"),
						TEXT("The captured stack's predecessor relations contain a cycle."), TEXT("/ordering/cyclic"), Start, true, false);
					Traversal.Add(Current);
					OutError.CyclePath = MoveTemp(Traversal);
					return false;
				}
				Seen.Add(Current);
				Traversal.Add(Current);
				const FString* Predecessor = PredecessorByItem.Find(Current);
				Current = Predecessor ? *Predecessor : FString();
			}
		}
		return true;
	}

	bool TryValidateMaterialNiagaraGraphLifecycleEvidence(const FMaterialNiagaraGraphLifecycleEvidence& Evidence, FMaterialNiagaraGraphAdapterError& OutError)
	{
		OutError.Reset();
		if (!Evidence.IsStackCaptureComplete() || !Evidence.IsCompoundCaptureComplete())
		{
			const bool bStack = !Evidence.IsStackCaptureComplete();
			OutError = AdapterCondition(bStack ? TEXT("material_niagara_graph_stack_capture_failed") : TEXT("material_niagara_graph_compound_capture_failed"),
				bStack ? TEXT("The supplied lifecycle evidence does not confirm stack capture completion.")
					: TEXT("The supplied lifecycle evidence does not confirm compound capture completion."),
				bStack ? TEXT("/stack_capture_complete") : TEXT("/compound_capture_complete"));
			return false;
		}
		if (!Evidence.IsCompilationComplete() || Evidence.GetCompileErrorCount() != 0)
		{
			OutError = !Evidence.IsCompilationComplete()
				? AdapterCondition(TEXT("material_niagara_graph_compile_incomplete"), TEXT("The supplied lifecycle evidence does not confirm compilation completion."), TEXT("/compilation_complete"))
				: AdapterFailure(TEXT("material_niagara_graph_compile_incomplete"), TEXT("The supplied lifecycle evidence reports material or Niagara compile errors."),
					TEXT("/compile_error_count"), MakeShared<FJsonValueNumber>(Evidence.GetCompileErrorCount()), MakeShared<FJsonValueNumber>(0));
			return false;
		}
		if (Evidence.GetStackIssueCount() != 0)
		{
			OutError = AdapterFailure(TEXT("material_niagara_graph_stack_issue"), TEXT("The supplied lifecycle evidence reports stack issues."),
				TEXT("/stack_issue_count"), MakeShared<FJsonValueNumber>(Evidence.GetStackIssueCount()), MakeShared<FJsonValueNumber>(0));
			return false;
		}
		if (Evidence.GetBeforeRevision().IsEmpty() || Evidence.GetReadbackRevision().IsEmpty() || Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision())
		{
			const bool bBeforeMissing = Evidence.GetBeforeRevision().IsEmpty();
			const bool bReadbackMissing = Evidence.GetReadbackRevision().IsEmpty();
			OutError = AdapterCondition(TEXT("material_niagara_graph_readback_revision_not_advanced"),
				bBeforeMissing ? TEXT("The lifecycle evidence has no before revision.")
					: bReadbackMissing ? TEXT("The lifecycle evidence has no independent readback revision.")
					: TEXT("The lifecycle readback revision has not advanced from the before revision."),
				bBeforeMissing ? TEXT("/before_revision_present") : bReadbackMissing ? TEXT("/readback_revision_present") : TEXT("/revision_advanced"));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Evidence.GetBeforeRevision());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Evidence.GetReadbackRevision());
			return false;
		}
		if (Evidence.GetApplyEvidence().IsEmpty() || Evidence.GetSettleEvidence().IsEmpty() || Evidence.GetReadbackEvidence().IsEmpty()
			|| Evidence.GetApplyEvidence() == Evidence.GetSettleEvidence() || Evidence.GetApplyEvidence() == Evidence.GetReadbackEvidence()
			|| Evidence.GetSettleEvidence() == Evidence.GetReadbackEvidence())
		{
			const bool bApplyMissing = Evidence.GetApplyEvidence().IsEmpty();
			const bool bSettleMissing = Evidence.GetSettleEvidence().IsEmpty();
			const bool bReadbackMissing = Evidence.GetReadbackEvidence().IsEmpty();
			const bool bApplySettle = Evidence.GetApplyEvidence() == Evidence.GetSettleEvidence();
			const bool bApplyReadback = Evidence.GetApplyEvidence() == Evidence.GetReadbackEvidence();
			OutError = AdapterCondition(TEXT("material_niagara_graph_readback_not_independent"),
				bApplyMissing ? TEXT("The lifecycle evidence has no apply observation.")
					: bSettleMissing ? TEXT("The lifecycle evidence has no settle observation.")
					: bReadbackMissing ? TEXT("The lifecycle evidence has no independent readback observation.")
					: bApplySettle ? TEXT("Apply and settle reused the same observation.")
					: bApplyReadback ? TEXT("Apply and readback reused the same observation.") : TEXT("Settle and readback reused the same observation."),
				bApplyMissing ? TEXT("/apply_evidence_present") : bSettleMissing ? TEXT("/settle_evidence_present")
					: bReadbackMissing ? TEXT("/readback_evidence_present") : bApplySettle ? TEXT("/apply_settle_independent")
					: bApplyReadback ? TEXT("/apply_readback_independent") : TEXT("/settle_readback_independent"));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(bApplySettle || bApplyReadback ? Evidence.GetApplyEvidence() : Evidence.GetSettleEvidence());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(bApplySettle ? Evidence.GetSettleEvidence() : Evidence.GetReadbackEvidence());
			return false;
		}
		return true;
	}
}
