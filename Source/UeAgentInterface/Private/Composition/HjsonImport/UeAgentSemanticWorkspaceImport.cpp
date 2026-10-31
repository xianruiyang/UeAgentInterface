#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"

#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/UeAgentCurveOriginalIntent.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceRebase.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "JsonUtils/JsonPointer.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentExactStringKeys.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::Composition::HjsonImport
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		TArray<const FSemanticUnit*> FindIdentityUnits(const FAuthoringModelRef& Model,
			const FString& IdentityKey, const FString& AuthoringTargetIdentityKey)
		{
			auto Units = Model->FindUnitsForTargetIdentity(IdentityKey);
			if (!AuthoringTargetIdentityKey.IsEmpty() && AuthoringTargetIdentityKey != IdentityKey)
				Units.Append(Model->FindUnitsForTargetIdentity(AuthoringTargetIdentityKey));
			Units.Sort([](const FSemanticUnit& Left, const FSemanticUnit& Right) { return &Left < &Right; });
			return Units;
		}

		UE::UAI::Contract::FDiagnostic ImportFailure(FString Code, const UE::UAI::Contract::FDiagnostic& Cause, const FString& PublicReason);
		const FSemanticUnit* FindPropertyUnitForIdentity(const FAuthoringModelRef& Model,
			const FString& IdentityKey, const FString& AuthoringTargetIdentityKey);
		bool ProjectPropertyEditValue(const FChangeDraft& Change, const FSemanticUnit& Unit,
			const FAuthoringModel& Model, FJsonObject& Descriptor, FSemanticWorkspaceImportError& OutError);
	}

	void AttachSemanticPropertySource(
		const FResolvedSemanticWorkspaceRef& Workspace, const FString& TargetKey, FJsonObject& Descriptor, const FString& TargetPath)
	{
		if (TargetKey.IsEmpty()) return;
		const FSourceProvenance* Match = nullptr;
		FString SourceUnit;
		for (const auto* Unit : Workspace->GetModel()->FindUnitsForTargetIdentity(TargetKey))
		{
			const auto* Source = Workspace->GetUnitSources().Find(Unit->GetUnitId());
			if (!Source) continue;
			if (Match) return;
			Match = Source;
			SourceUnit = Unit->GetUnitId();
		}
		if (!Match) return;
		// Property values have their own source tree, including native wrapper
		// aliases. Keep that binding so a codec leaf can resolve to the authored value.
		if (const auto* ValueSource = Workspace->FindValueSource(SourceUnit, TEXT("/value"))) Match = ValueSource;
		const auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("source_file"), Match->GetSourceUnit());
		Source->SetStringField(TEXT("json_pointer"), Match->GetJsonPointer());
		Source->SetNumberField(TEXT("line"), Match->GetLine());
		Source->SetNumberField(TEXT("column"), Match->GetColumn());
		Source->SetStringField(TEXT("source_unit"), SourceUnit);
		Source->SetStringField(TEXT("value_internal_json_path"), FString());
		if (!TargetPath.IsEmpty()) Source->SetStringField(TEXT("target"), TargetPath);
		Descriptor.SetObjectField(TEXT("authoring_source"), Source);
	}

	bool TryVerifySemanticWorkspaceValueIntent(
		const FPreparedAuthoringImport& Prepared,
		const UeAgentAuthoringState::FStateSnapshot& NativeAfter,
		FWorkspaceError& OutError,
		const TArray<FDocumentValueSourceBinding>& DocumentSources,
		const UeAgentNativeNormalizationIntent::FScope* NativeNormalization,
		const TArray<UeAgentAuthoringState::FReadRequest>* CurrentReadRequests,
		const TArray<HjsonWorkspace::FGraphNodeReferenceBinding>& GraphReferenceBindings)
	{
		OutError.Reset();
		auto IntentDocumentSources = DocumentSources;
		TMap<const UeAgentAuthoringObjectApply::FPropertyIntent*, UeAgentAuthoringObjectApply::FApplyResult> VerifiedIntents;
		if (NativeNormalization)
			for (const auto& Proof : NativeNormalization->GetCreatedProperties())
			{
				UeAgentAuthoringState::FStateEntry Actual, Expected = *Proof.Entry;
				UE::UAI::Contract::FDiagnostic Detail;
				UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
				UeAgentAuthoringSemanticCompare::FCompareOptions Options;
				Options.bCompareAvailability = false;
				const bool Found = NativeAfter.FindEntry(Expected.SemanticId, Expected.TargetIdentity.CanonicalKey, Actual);
				bool Compared = Found;
				if (Compared && Actual.NativeValue)
					Compared = UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(
						Actual, Proof.Entry->Value, nullptr, Expected, Detail);
				if (Compared) Compared = UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, Options, Comparison);
				if (Compared && Comparison.bEquivalent) continue;
				const FString Code = !Found ? TEXT("hjson_import_intent_target_missing")
					: Compared ? TEXT("hjson_import_intent_not_retained") : TEXT("hjson_import_intent_check_failed");
				FWorkspaceError Failure;
				auto Diagnostic = Found
					? Contract::FDiagnostic::WrapFailure(Code, Detail.IsEmpty() ? Comparison.Diagnostic : Detail, TEXT("final_readback"))
					: Contract::FDiagnostic::Failure(Code, TEXT("The created property is missing from the final native readback."), TEXT("final_readback"));
				if (!Found) { Diagnostic.ExpectedValue = Proof.Entry->Value; Diagnostic.ActualValuePresent = false; }
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Diagnostic, Proof.SourcePath);
				if (Proof.SourceDescriptor) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Proof.SourceDescriptor);
				Failure.SetFailure(MoveTemp(Diagnostic), FString(), Proof.SourcePath);
				for (const auto& Unit : Prepared.GetWorkspace()->GetModel()->GetUnits())
					if (const auto* Source = Prepared.GetWorkspace()->FindValueSource(Unit.GetUnitId(), Proof.SourcePath)) { Failure.Locate(*Source); break; }
				OutError.Append(Failure);
			}
		const auto& BeforeModel = Prepared.GetInputBaseline();
		auto FindGraph = [](const FAuthoringModel& Model, const FString& Key) -> const FSemanticUnit*
		{
			for (const auto& Unit : Model.GetUnits())
				if (Unit.GetTargetIdentityKey() == Key && Unit.GetValues().Contains(TEXT("nodes"))
					&& Unit.GetValues().Contains(TEXT("edges"))) return &Unit;
			return nullptr;
		};
		auto UnitValue = [](const FSemanticUnit& Unit, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
		{
			const auto Object = MakeShared<FJsonObject>();
			for (const auto& Field : Unit.GetValues())
			{
				TSharedPtr<FJsonValue> Json;
				if (!CanonicalJson::TryToJsonValue(*Field.Value, Json, Error)) return false;
				Object->SetField(Field.Key, Json);
			}
			Value = MakeShared<FJsonValueObject>(Object);
			return true;
		};
		// Input units already carry native bindings. Read those bindings directly;
		// never allocate new display names or reconstruct an authoring model here.
		auto ReadItem = [&](const FSemanticUnit& Unit, bool MergeProperties,
			UeAgentAuthoringState::FStateEntry& Entry)
		{
			if (!NativeAfter.FindEntry(Unit.GetSemanticTypeRef(), Unit.GetTargetIdentityKey(), Entry)
				|| !Entry.bAvailable || !Entry.Value || Entry.Value->Type != EJson::Object) return false;
			Entry.Value = UeAgentAuthoringState::CloneCanonicalValue(Entry.Value);
			if (BeforeModel) for (const auto& Child : BeforeModel->GetUnits())
			{
				if (Child.GetOwnerRef() != Unit.GetUnitId()) continue;
				const bool Layout = Child.GetUnitKind() == ESemanticUnitKind::LayoutState;
				if (!Layout && (!MergeProperties || Child.GetUnitKind() != ESemanticUnitKind::PropertySet
					|| Child.GetSemanticTypeRef().EndsWith(TEXT(".property")))) continue;
				UeAgentAuthoringState::FStateEntry Part;
				if (!NativeAfter.FindEntry(Child.GetSemanticTypeRef(), Child.GetTargetIdentityKey(), Part)
					|| !Part.bAvailable || !Part.Value) continue;
				if (Layout) Entry.Value->AsObject()->SetField(TEXT("pos"), Part.Value);
				else if (Part.Value->Type == EJson::Object)
					for (const auto& Field : Part.Value->AsObject()->Values) Entry.Value->AsObject()->SetField(Field.Key, Field.Value);
			}
			return true;
		};
		// Keep native side effects in unedited fields. Expected leaves always come
		// from the immutable change draft, before the domain's DTO projection.
		const auto OverlayDelta = UeAgentAuthoringSemanticCompare::OverlayValueDelta;
		auto TypeDescriptor = [&](const UeAgentAuthoringState::FStateEntry& Entry) {
			const auto Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("type"), Entry.Type.ToJsonObject());
			auto Definition = Entry.ValueDefinition;
			if (Entry.ComparisonValueDefinition)
				Definition = OverlayDelta(nullptr, Entry.ComparisonValueDefinition, Definition, nullptr);
			if (Definition) Result->SetField(TEXT("value_definition"), Definition);
			return Result;
		};
		using UeAgentAuthoringSemanticCompare::BindComparisonObjectRows;
		const auto NativeEntries = NativeAfter.CopyEntries();
		for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
		{
			if (AssetPreview::IsPreviewSemantic(Change.GetIdentity().GetSemanticId())) continue;
			// New owned items are bound and verified by their native mutation owner.
			// This check covers the stable entries supplied by the export capture;
			// it must not mistake a synthetic keyed collection for a native identity.
			if (!Change.GetExpectedBefore().IsValid() || !Change.GetProposedAfter().IsValid()) continue;
			const FString NativeKey = Change.GetIdentity().GetSelector(CanonicalAuthoringTargetIdentitySelector);
			if (NativeKey.IsEmpty()) continue;
			UeAgentAuthoringState::FStateEntry Actual;
			const bool bFound = NativeAfter.FindEntry(Change.GetIdentity().GetSemanticId(), NativeKey, Actual);
			UE::UAI::Contract::FDiagnostic ValueError, FailureDetail;
			FString DifferencePath, IntentGraphUnitId;
			TMap<FString, FString> IntentSourcePaths;
			TSharedPtr<FJsonValue> Before, Desired;
			TArray<UeAgentNativeNormalizationIntent::FValueProof> ValueProofs;
			auto AppendValueProofs = [&](const UeAgentAuthoringState::FStateEntry& Entry, const FString& Prefix) {
				if (!NativeNormalization) return;
				if (const auto* Proofs = NativeNormalization->FindValues(Entry.TargetIdentity, Entry.SemanticId))
					for (const auto& Proof : *Proofs) {
						auto Projected = Proof;
						Projected.Path = Prefix + Proof.Path;
						ValueProofs.Add(MoveTemp(Projected));
					}
			};
			if (bFound) AppendValueProofs(Actual, FString());
			if (!StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetExpectedBefore(), Before, ValueError)
				|| !StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetProposedAfter(), Desired, ValueError))
				ValueError = ImportFailure(TEXT("hjson_import_intent_value_invalid"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The before or proposed value could not be converted for final readback verification."));
			else if (!bFound) ValueError = ImportFailure(TEXT("hjson_import_intent_target_missing"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The edited target is missing from the final native readback."));
			else if (!Actual.bAvailable) ValueError = ImportFailure(TEXT("hjson_import_intent_check_failed"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The final readback value could not be compared with the original edit."));
			else
			{
				if (const auto* InputBefore = Prepared.FindPropertyBaseline(Change.GetIdentity().GetCanonicalKey()))
					if (!CanonicalJson::TryToJsonValue(**InputBefore, Before, FailureDetail))
						ValueError = ImportFailure(TEXT("hjson_import_intent_input_invalid"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The original authored property value could not be read for intent verification."));
				// ObjectApply has already consumed binding/creation syntax and frozen
				// the decoded values, including nested owned edits. Check that it
				// received the original delta, then verify that intent on the fresh
				// read root. Re-decoding `from` or `owned_object` here is not a value check.
				if (ValueError.IsEmpty() && Actual.NativeValue && NativeNormalization && CurrentReadRequests
					&& !NativeNormalization->Find(Actual))
				{
					const auto* Request = CurrentReadRequests->FindByPredicate([&](const auto& Item) {
						return Item.SemanticId == Actual.SemanticId
							&& ((Item.SnapshotIdentityOverride.IsSet()
								&& Item.SnapshotIdentityOverride->CanonicalKey == Actual.TargetIdentity.CanonicalKey)
								|| Item.TargetIdentity.CanonicalKey == Actual.TargetIdentity.CanonicalKey
								|| (Actual.NativeTargetIdentity.IsValid()
									&& Item.TargetIdentity.CanonicalKey == Actual.NativeTargetIdentity.CanonicalKey));
					});
					const auto* Proof = Request ? NativeNormalization->FindDecodedProperty(*Request) : nullptr;
					if (Proof)
					{
						// Compare the frozen decoder input with the same formal edit
						// projection used before dispatch. Native capture definitions
						// describe owned-property arrays, while compact inputs use
						// objects; ordinary structs also omit unchanged members.
						// Neither a native declaration nor a full-value overlay is
						// the contract of this sparse input. Typed intent verification
						// below still checks the independently captured native result.
						const auto* PropertyUnit = FindPropertyUnitForIdentity(Prepared.GetWorkspace()->GetModel(),
							Change.GetIdentity().GetCanonicalKey(), NativeKey);
						const auto SourceDescriptor = MakeShared<FJsonObject>();
						SourceDescriptor->SetField(TEXT("value"), Desired);
						if (!PropertyUnit)
						{
							FailureDetail = Contract::FDiagnostic::Failure(TEXT("hjson_import_property_type_missing"),
								TEXT("The prepared property change has no matching property unit and type definition."), TEXT("semantic_compare"));
						}
						else if (ProjectPropertyEditValue(Change, *PropertyUnit,
							*Prepared.GetWorkspace()->GetModel(), *SourceDescriptor, FailureDetail))
						{
							const auto SourceComparison = CanonicalJson::CompareJsonValues(
								SourceDescriptor->TryGetField(TEXT("value")), Proof->AuthoredSource);
							if (!SourceComparison.bEquivalent)
							{
								FailureDetail = Contract::FDiagnostic::Failure(TEXT("authored_value_mismatch"),
									TEXT("The property input consumed by the decoder differs from the prepared edit."), TEXT("semantic_compare"));
								SourceComparison.ApplyTo(FailureDetail);
								ValueError = ImportFailure(TEXT("hjson_import_intent_conversion_not_retained"), FailureDetail,
									TEXT("The property input consumed by the decoder differs from the prepared edit."));
							}
						}
						if (!FailureDetail.IsEmpty() && ValueError.IsEmpty())
							ValueError = ImportFailure(TEXT("hjson_import_intent_check_failed"), FailureDetail,
								TEXT("The original property edit could not be projected for input verification."));
						if (ValueError.IsEmpty())
						{
							auto* Verified = VerifiedIntents.Find(Proof->Intent.Get());
							if (!Verified)
							{
								UeAgentAuthoringObjectApply::FApplyResult Result;
								if (Request->RootStruct)
									Proof->Intent->VerifyStruct(Request->RootStruct, const_cast<void*>(Request->RootData),
										const_cast<UObject*>(Request->NotificationOwner), Result);
								else Proof->Intent->VerifyObject(const_cast<UObject*>(Request->RootObject), Result);
								Verified = &VerifiedIntents.Add(Proof->Intent.Get(), MoveTemp(Result));
							}
							if (Verified->IsApplied()) continue;
							FailureDetail = Verified->Diagnostic;
							ValueError = ImportFailure(TEXT("hjson_import_intent_not_retained"), FailureDetail, TEXT("The final native value does not retain the original edit."));
						}
						if (ValueError.PropertyPath.IsEmpty()) ValueError.PropertyPath = Change.GetIdentity().GetSelector(TEXT("property_path"));
						if (ValueError.TargetPath.IsEmpty()) ValueError.TargetPath = Change.GetIdentity().GetSelector(TEXT("asset_path"));
						DifferencePath = FailureDetail.Source.JsonPath;
					}
				}
				// Complete native graph documents already own their node values and
				// type descriptors. Only decomposed captures need ItemState assembly;
				// do not require fictitious per-node bindings for a document owner.
				const bool bNativeGraphDocument = Actual.Value && Actual.Value->Type == EJson::Object
					&& Actual.Value->AsObject()->HasTypedField<EJson::Object>(TEXT("nodes"))
					&& Actual.Value->AsObject()->HasTypedField<EJson::Object>(TEXT("edges"));
				if (const auto* EditedGraph = FindGraph(*Prepared.GetWorkspace()->GetModel(), NativeKey))
				{
					IntentGraphUnitId = EditedGraph->GetUnitId();
					const auto* BaselineGraph = BeforeModel ? FindGraph(*BeforeModel, NativeKey) : nullptr;
					if (!BaselineGraph || !UnitValue(*BaselineGraph, Before, FailureDetail)
						|| !UnitValue(*EditedGraph, Desired, FailureDetail))
					{ ValueError = ImportFailure(TEXT("hjson_import_intent_input_binding_missing"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The captured input binding needed to verify this edited value is missing.")); }
					else
					{
						const auto BaseNodes = Before->AsObject()->GetObjectField(TEXT("nodes"));
						const auto InputNodes = Desired->AsObject()->GetObjectField(TEXT("nodes"));
						const auto BeforeNodes = MakeShared<FJsonObject>();
						const auto DesiredNodes = MakeShared<FJsonObject>();
						const auto ActualNodes = MakeShared<FJsonObject>();
						const auto NodeTypes = MakeShared<FJsonObject>();
						for (const auto& Node : InputNodes->Values)
						{
							const auto Row = Node.Value->AsObject();
							FString Id, Operation;
							Row->TryGetStringField(TEXT("id"), Id);
							Row->TryGetStringField(TEXT("operation"), Operation);
							// New node creation has its native mutation receipt/owner. Stable
							// nodes retain the original native binding, even when renamed.
							if (Operation == TEXT("create")) continue;
							TSharedPtr<FJsonValue> OldValue;
							for (const auto& Pair : BaseNodes->Values) {
								FString OldId; if (Pair.Value->AsObject()->TryGetStringField(TEXT("id"), OldId) && OldId == Id) { OldValue = Pair.Value; break; }
							}
							if (!OldValue) { ValueError = ImportFailure(TEXT("hjson_import_intent_input_binding_missing"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The captured input binding needed to verify this edited value is missing.")); break; }
							const FSemanticUnit* NativeUnit = nullptr;
							for (const auto& Unit : BeforeModel->GetUnits())
							{
								if (Unit.GetUnitKind() != ESemanticUnitKind::ItemState) continue;
								const auto* UnitId = Unit.GetValues().Find(TEXT("id"));
								if (!UnitId || (*UnitId)->GetKind() != EUeAgentValueKind::String || (*UnitId)->GetText() != Id) continue;
								const FSemanticUnit* Owner = &Unit;
								while (Owner && Owner->GetUnitId() != BaselineGraph->GetUnitId()) Owner = BeforeModel->FindUnit(Owner->GetOwnerRef());
								if (Owner) { NativeUnit = &Unit; break; }
							}
							if (!bNativeGraphDocument && !NativeUnit) { ValueError = ImportFailure(TEXT("hjson_import_intent_input_binding_missing"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The captured input binding needed to verify this edited value is missing.")); break; }
							const auto OldRow = UeAgentAuthoringState::CloneCanonicalValue(OldValue)->AsObject();
							OldRow->RemoveField(TEXT("operation"));
							BeforeNodes->SetObjectField(Node.Key, OldRow);
							if (Operation != TEXT("remove")) {
								const auto NewRow = UeAgentAuthoringState::CloneCanonicalValue(Node.Value)->AsObject();
								for (const auto& Binding : GraphReferenceBindings)
								{
									if (Binding.GraphUnitId != EditedGraph->GetUnitId() || Binding.NativeNodeId != Id) continue;
									FString AuthoredReference;
									if (NewRow->TryGetStringField(Binding.FieldId, AuthoredReference)
										&& AuthoredReference == Binding.AuthoredReference)
										NewRow->SetStringField(Binding.FieldId, Binding.NativeReference);
								}
								NewRow->RemoveField(TEXT("operation")); DesiredNodes->SetObjectField(Node.Key, NewRow);
							}
							UeAgentAuthoringState::FStateEntry NativeNode;
							if (NativeUnit && ReadItem(*NativeUnit, false, NativeNode)) {
								ActualNodes->SetField(Node.Key, NativeNode.Value);
								NodeTypes->SetObjectField(Node.Key, TypeDescriptor(NativeNode));
							} else if (!NativeUnit && bNativeGraphDocument) {
								// Match the input's immutable native id, not a regenerated alias.
								for (const auto& Captured : Actual.Value->AsObject()->GetObjectField(TEXT("nodes"))->Values) {
									const auto RowValue = Captured.Value && Captured.Value->Type == EJson::Object ? Captured.Value->AsObject() : nullptr;
									FString CapturedId;
									if (RowValue && RowValue->TryGetStringField(TEXT("id"), CapturedId) && CapturedId == Id) {
										ActualNodes->SetField(Node.Key, UeAgentAuthoringState::CloneCanonicalValue(Captured.Value));
										break;
									}
								}
							}
						}
						Before->AsObject()->RemoveField(TEXT("edges"));
						Desired->AsObject()->RemoveField(TEXT("edges"));
						Before->AsObject()->SetObjectField(TEXT("nodes"), BeforeNodes);
						Desired->AsObject()->SetObjectField(TEXT("nodes"), DesiredNodes);
						Actual.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
						Actual.Value->AsObject()->RemoveField(TEXT("edges"));
						Actual.Value->AsObject()->SetObjectField(TEXT("nodes"), ActualNodes);
						if (!NodeTypes->Values.IsEmpty()) BindComparisonObjectRows(Actual, TEXT("nodes"), NodeTypes);
					}
				}
				if (ValueError.IsEmpty() && !Actual.NativeValue && Before && Desired && Actual.Value
					&& Before->Type == EJson::Object && Desired->Type == EJson::Object && Actual.Value->Type == EJson::Object)
				{
					// TSet-backed JSON maps may relocate even on replacement. Hold the
					// original fields while normalizing their values into the live map.
					const auto IntentFields = Desired->AsObject()->Values;
					for (const auto& Field : IntentFields)
					{
						FKeyedItemProjectionContract Contract;
						if (!TryGetKeyedItemProjectionContract(Field.Key, Contract) || Contract.AggregatedItemSemanticSuffix.IsEmpty()) continue;
						const FSemanticUnit* EditedRoot = nullptr;
						for (const auto& Unit : Prepared.GetWorkspace()->GetModel()->GetUnits())
							if (Unit.GetUnitKind() == ESemanticUnitKind::StructureState && Unit.GetSemanticTypeRef() == Actual.SemanticId
								&& (Unit.GetTargetIdentityKey() == NativeKey || Unit.GetTargetIdentityKey() == Change.GetIdentity().GetCanonicalKey())
								&& Unit.GetValues().Contains(Field.Key)) { EditedRoot = &Unit; break; }
						if (!EditedRoot) continue;
						const FSemanticUnit* BaselineRoot = BeforeModel ? BeforeModel->FindUnit(EditedRoot->GetUnitId()) : nullptr;
						TSharedPtr<FJsonValue> BeforeRows, DesiredRows = Field.Value, ActualRows;
						TMap<FString, FString> NativeAliases;
						const auto CapturedRows = MakeShared<FJsonObject>();
						TMap<FString, UeAgentAuthoringState::FStateEntry> CapturedEntries;
						const auto* Creation = NativeNormalization ? NativeNormalization->FindKeyedArguments(Field.Key, DesiredRows, NativeKey) : nullptr;
						if (!BaselineRoot || !CanonicalJson::TryToJsonValue(*BaselineRoot->GetValues().FindChecked(Field.Key), BeforeRows, FailureDetail))
						{ ValueError = ImportFailure(TEXT("hjson_import_intent_input_binding_missing"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The captured input binding needed to verify this edited value is missing.")); DifferencePath = TEXT("/") + EscapeJsonPointerSegment(Field.Key); break; }
						for (auto Captured : NativeEntries)
						{
							if (!Captured.SemanticId.EndsWith(Contract.AggregatedItemSemanticSuffix)
								|| !Captured.bAvailable || !Captured.Value || Captured.Value->Type != EJson::Object) continue;
							const FSemanticUnit* Bound = nullptr;
							for (const auto& Unit : BeforeModel->GetUnits())
								if (Unit.GetTargetIdentityKey() == Captured.TargetIdentity.CanonicalKey
									&& Unit.GetSemanticTypeRef() == Captured.SemanticId) { Bound = &Unit; break; }
							if (Bound && Bound->GetOwnerRef() != BaselineRoot->GetUnitId()) continue;
							// New identities are constrained by the native owner's selector
							// scope, not by a generated visible title or ordinal alias.
							bool SameScope = true;
							for (const auto& Selector : Actual.TargetIdentity.Selectors)
								if (const auto* Value = Captured.TargetIdentity.Selectors.Find(Selector.Key); Value && *Value != Selector.Value) { SameScope = false; break; }
							if (!Bound && !SameScope) continue;
							if (Bound) ReadItem(*Bound, Contract.bMergeChildPropertyValues, Captured);
							else Captured.Value = UeAgentAuthoringState::CloneCanonicalValue(Captured.Value);
							if (Bound) Captured.Value->AsObject()->SetStringField(TEXT("unit_id"), Bound->GetUnitId());
							// Some native accessors carry identity in the capture envelope
							// rather than the business value. Keep that provenance available
							// to the common creation binding, without guessing from a title.
							if (!Captured.Value->AsObject()->HasField(TEXT("id")))
								Captured.Value->AsObject()->SetStringField(TEXT("id"), Captured.TargetIdentity.CanonicalKey);
							CapturedRows->SetField(Captured.TargetIdentity.CanonicalKey, Captured.Value);
							CapturedEntries.Add(Captured.TargetIdentity.CanonicalKey, Captured);
						}
						ActualRows = MakeShared<FJsonValueObject>(CapturedRows);
						if (!TryProjectKeyedItemOriginalIntent(Contract, BeforeRows, DesiredRows, ActualRows, IntentSourcePaths, FailureDetail, Creation ? *Creation : nullptr, &NativeAliases))
						{ ValueError = ImportFailure(TEXT("hjson_import_intent_normalization_failed"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The original edit could not be reconciled with the native normalized value.")); DifferencePath = TEXT("/") + EscapeJsonPointerSegment(Field.Key); break; }
						// The collection is composed from native child captures, not from
						// the parent's accessor. Preserve their declared types through the
						// same identity/alias projection as the values, without exporting
						// comparison metadata or deriving expectations from a domain DTO.
						const auto RowTypes = MakeShared<FJsonObject>();
						for (const auto& Row : CapturedRows->Values)
						{
							FString UnitId;
							Row.Value->AsObject()->TryGetStringField(TEXT("unit_id"), UnitId);
							const auto* Alias = NativeAliases.Find(Row.Key);
							const auto* CapturedEntry = CapturedEntries.Find(Row.Key);
							if (!Alias || !CapturedEntry) continue;
							const auto& Captured = *CapturedEntry;
							// Native child proofs follow the same immutable identity/alias
							// correspondence as values and types. Collection aggregation must
							// not discard a child's accepted input normalization.
							AppendValueProofs(Captured, TEXT("/") + EscapeJsonPointerSegment(Field.Key)
								+ TEXT("/") + EscapeJsonPointerSegment(*Alias));
							const auto Descriptor = TypeDescriptor(Captured);
							auto RowDefinition = Descriptor->TryGetField(TEXT("value_definition"));
							if (Contract.bMergeChildPropertyValues)
								for (const auto& Child : BeforeModel->GetUnits())
								{
									if (Child.GetOwnerRef() != UnitId || Child.GetUnitKind() != ESemanticUnitKind::PropertySet
										|| Child.GetSemanticTypeRef().EndsWith(TEXT(".property"))) continue;
									UeAgentAuthoringState::FStateEntry ChildCapture;
									if (!NativeAfter.FindEntry(Child.GetSemanticTypeRef(), Child.GetTargetIdentityKey(), ChildCapture)) continue;
									if (ChildCapture.ValueDefinition) RowDefinition = OverlayDelta(nullptr, ChildCapture.ValueDefinition, RowDefinition, nullptr);
									if (ChildCapture.ComparisonValueDefinition) RowDefinition = OverlayDelta(nullptr, ChildCapture.ComparisonValueDefinition, RowDefinition, nullptr);
								}
							if (RowDefinition) Descriptor->SetField(TEXT("value_definition"), RowDefinition);
							RowTypes->SetObjectField(*Alias, Descriptor);
						}
						BindComparisonObjectRows(Actual, Field.Key, RowTypes);
						// Aggregated rows have their own original parser tree even when
						// they were synthesized after ordinary document bindings.
						FDocumentValueSourceBinding Binding;
						Binding.DocumentIdentity = Change.GetIdentity().GetCanonicalKey();
						Binding.UnitId = EditedRoot->GetUnitId();
						Binding.DocumentPointer = TEXT("/") + EscapeJsonPointerSegment(Field.Key);
						Binding.UnitValuePointer = Binding.DocumentPointer;
						IntentDocumentSources.Add(MoveTemp(Binding));
						Before->AsObject()->SetField(Field.Key, BeforeRows);
						Desired->AsObject()->SetField(Field.Key, DesiredRows);
						Actual.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
						Actual.Value->AsObject()->SetField(Field.Key, ActualRows);
					}
				}
				UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
				if (ValueError.IsEmpty() && (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().FindSemantic(Actual.SemanticId, Semantic)
					|| !UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().ProjectOriginalIntent(
						Semantic.Comparator, Actual, Before, Desired, Actual.Value, IntentSourcePaths, FailureDetail)))
					ValueError = ImportFailure(TEXT("hjson_import_intent_normalization_failed"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The original edit could not be reconciled with the native normalized value."));
				if (ValueError.IsEmpty() && !UeAgentCurveJson::ProjectOriginalCurveKeyOrder(Desired, IntentSourcePaths, FailureDetail))
					ValueError = ImportFailure(TEXT("hjson_import_intent_normalization_failed"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The original edit could not be reconciled with the native normalized value."));
				auto Expected = Actual;
				Expected.NativeValue.Reset();
				Expected.Value = OverlayDelta(Before, Desired, Actual.Value, &Actual);
				if (ValueError.IsEmpty() && Actual.NativeValue && NativeNormalization)
					if (const auto* Proof = NativeNormalization->Find(Actual))
					{
						// A native-normalized expectation is valid only when its frozen
						// decoded source retains the original, pre-domain edit. Checking
						// this on the isolated source also preserves unedited side effects.
						UeAgentAuthoringState::FStateEntry Source;
						UeAgentAuthoringState::FStateEntry SourceExpected;
						UeAgentAuthoringSemanticCompare::FEntryComparison SourceComparison;
						UeAgentAuthoringSemanticCompare::FCompareOptions SourceOptions;
						SourceOptions.bCompareAvailability = false;
						// Keep canonical evidence across unload/GC. Rebind both source
						// and input to the fresh schema; no preview UObject survives here.
						const bool SourceReady = UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(
							Actual, Proof->SourceValue, nullptr, Source, FailureDetail)
							&& UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(
								Actual, OverlayDelta(Before, Desired, Source.Value, &Actual), nullptr, SourceExpected, FailureDetail);
						const bool SourceCompared = SourceReady && UeAgentAuthoringSemanticCompare::CompareEntries(
							SourceExpected, Source, SourceOptions, SourceComparison);
						if (!SourceCompared || !SourceComparison.bEquivalent)
						{
							if (!SourceComparison.Diagnostic.IsEmpty()) FailureDetail = SourceComparison.Diagnostic;
							ValueError = ImportFailure(SourceCompared ? TEXT("hjson_import_intent_conversion_not_retained") : TEXT("hjson_import_intent_check_failed"), FailureDetail, TEXT("The decoded property value does not retain the original edit, or could not be compared with it."));
							DifferencePath = SourceComparison.Diagnostic.Source.JsonPath;
						}
						else Expected.Value = OverlayDelta(Proof->AuthoredSource, Proof->NormalizedValue, Expected.Value, &Actual);
					}
				if (ValueError.IsEmpty())
						for (const auto& Proof : ValueProofs)
						{
							TSharedPtr<FJsonValue> BeforeValue, DesiredValue, ExpectedValue;
							DesiredValue = UeAgentAuthoringSemanticCompare::FindDeclaredValuePath(Actual, Desired, Proof.Path);
							if (!DesiredValue) continue;
							BeforeValue = UeAgentAuthoringSemanticCompare::FindDeclaredValuePath(Actual, Before, Proof.Path);
							auto Source = Actual;
							Source.NativeValue.Reset();
							Source.Value = Proof.SourceValue;
							if (!Proof.Type.Kind.IsEmpty())
							{
								Source.Type = Proof.Type;
								Source.ValueDefinition.Reset();
								Source.ComparisonValueDefinition = Proof.ValueDefinition;
							}
							auto SourceExpected = Source;
							SourceExpected.Value = OverlayDelta(BeforeValue, DesiredValue, Source.Value, &Source);
							UeAgentAuthoringSemanticCompare::FEntryComparison SourceComparison;
							UeAgentAuthoringSemanticCompare::FCompareOptions SourceOptions;
							SourceOptions.bCompareAvailability = false;
							const bool SourceCompared = UeAgentAuthoringSemanticCompare::CompareEntries(SourceExpected, Source, SourceOptions, SourceComparison);
							if (!SourceCompared || !SourceComparison.bEquivalent)
							{
								FailureDetail = SourceComparison.Diagnostic;
								ValueError = ImportFailure(SourceCompared ? TEXT("hjson_import_intent_conversion_not_retained") : TEXT("hjson_import_intent_check_failed"), FailureDetail, TEXT("The decoded property value does not retain the original edit, or could not be compared with it."));
								DifferencePath = Proof.Path + SourceComparison.Diagnostic.Source.JsonPath;
								break;
							}
							TSharedPtr<FJsonValue> Parent;
							FString ResolvedExpectedPath;
							ExpectedValue = UeAgentAuthoringSemanticCompare::FindDeclaredValuePath(Actual, Expected.Value, Proof.Path, &ResolvedExpectedPath);
							const int32 LastSeparator = ResolvedExpectedPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
							Parent = UeAgentAuthoringSemanticCompare::FindDeclaredValuePath(Actual, Expected.Value, ResolvedExpectedPath.Left(LastSeparator));
							if (!ExpectedValue || !Parent)
							{ ValueError = ImportFailure(TEXT("hjson_import_intent_normalization_failed"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The original edit could not be reconciled with the native normalized value.")); ValueError.Source.JsonPath = Proof.Path; break; }
							// The decoded argument has independently retained the original
							// request. Bind this owned subpath to the completed native proposal:
							// array sorting and generated identities are not scalar overlays.
							const auto Normalized = UeAgentAuthoringState::CloneCanonicalValue(Proof.NormalizedValue);
							const FString Last = UE::Json::FJsonPointer::UnescapePart(ResolvedExpectedPath.Mid(LastSeparator + 1));
							if (Parent->Type == EJson::Object) Parent->AsObject()->SetField(Last, Normalized);
							else
							{
								int32 Index;
								if (Parent->Type != EJson::Array || !LexTryParseString(Index, *Last)
									|| !Parent->AsArray().IsValidIndex(Index))
								{ ValueError = ImportFailure(TEXT("hjson_import_intent_normalization_failed"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The original edit could not be reconciled with the native normalized value.")); ValueError.Source.JsonPath = Proof.Path; break; }
								const_cast<TArray<TSharedPtr<FJsonValue>>&>(Parent->AsArray())[Index] = Normalized;
							}
						}
				// Reflected values normalize through the existing codec, with the
				// fresh native schema. Do not compare compact authoring syntax to a
				// codec envelope, or exact decimal text to native float storage.
				if (ValueError.IsEmpty() && Actual.NativeValue.IsValid())
				{
					if (!UeAgentAuthoringSemanticCompare::BuildExpectedPropertyEntry(
						Actual, Expected.Value, nullptr, Expected, ValueError))
					{
						FailureDetail = ValueError;
						ValueError = ImportFailure(TEXT("hjson_import_intent_normalization_failed"), ValueError.IsValid() ? ValueError : FailureDetail, TEXT("The original edit could not be reconciled with the native normalized value."));
					}
				}
				UeAgentAuthoringSemanticCompare::FEntryComparison Comparison;
				UeAgentAuthoringSemanticCompare::FCompareOptions CompareOptions;
				// Intent verifies values, not the editability of an expected-value
				// storage object (which is intentionally read-only).
				CompareOptions.bCompareAvailability = false;
				if (ValueError.IsEmpty())
				{
					const bool Compared = UeAgentAuthoringSemanticCompare::CompareEntries(Expected, Actual, CompareOptions, Comparison);
					if (!Compared || !Comparison.bEquivalent)
					{
						FailureDetail = Comparison.Diagnostic;
						ValueError = ImportFailure(Compared ? TEXT("hjson_import_intent_not_retained") : TEXT("hjson_import_intent_check_failed"), FailureDetail, TEXT("The final native value does not retain the original edit."));
						DifferencePath = Comparison.Diagnostic.Source.JsonPath;
					}
				}
			}
			if (ValueError.IsEmpty()) continue;
			if (bFound && Desired && !DifferencePath.IsEmpty())
				UeAgentAuthoringSemanticCompare::FindDeclaredValuePath(Actual, Desired, DifferencePath, &DifferencePath);
			else if (DifferencePath.IsEmpty())
				DifferencePath = ValueError.Source.JsonPath;
			const TPair<FString, FString>* SourcePath = nullptr;
			for (const auto& Path : IntentSourcePaths)
				if ((DifferencePath == Path.Key || DifferencePath.StartsWith(Path.Key + TEXT("/")))
					&& (!SourcePath || Path.Key.Len() > SourcePath->Key.Len())) SourcePath = &Path;
			if (SourcePath) DifferencePath = SourcePath->Value + DifferencePath.Mid(SourcePath->Key.Len());
			FWorkspaceError Failure;
			auto Diagnostic = ValueError;
			Diagnostic.Stage = TEXT("final_readback");
			if (!DifferencePath.IsEmpty()) Diagnostic.Source.JsonPath = DifferencePath;
			Diagnostic.bRetrySafe = false;
			const auto& Workspace = Prepared.GetWorkspace();
			Workspace->LocateDocumentDiagnostic({Change.GetIdentity().GetCanonicalKey()}, IntentDocumentSources, Diagnostic, IntentGraphUnitId);
			Failure.SetFailure(MoveTemp(Diagnostic), FString(), DifferencePath);
			if (!Failure.GetSourceUnit().IsEmpty())
			{
				OutError.Append(Failure);
				continue;
			}
			for (const auto& Unit : Workspace->GetModel()->GetUnits())
			{
				if (Unit.GetUnitKind() == ESemanticUnitKind::TargetState) continue;
				if (Unit.GetTargetIdentityKey() != NativeKey
					&& Unit.GetTargetIdentityKey() != Change.GetIdentity().GetCanonicalKey()) continue;
				const auto* Source = Workspace->FindValueSource(Unit.GetUnitId(), TEXT("/values") + DifferencePath);
				if (!Source) Source = Workspace->GetUnitSources().Find(Unit.GetUnitId());
				if (Source) Failure.Locate(*Source);
				break;
			}
			OutError.Append(Failure);
		}
		return !OutError.HasErrors();
	}

	namespace
	{
		UE::UAI::Contract::FDiagnostic ImportFailure(
			FString Code, const UE::UAI::Contract::FDiagnostic& Cause, const FString& PublicReason)
		{
			return Cause.IsEmpty() ? Contract::FDiagnostic::Failure(MoveTemp(Code), PublicReason, TEXT("hjson_import"))
				: Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause, TEXT("hjson_import"));
		}




		bool TryValidateWorkspaceBindingForImport(
			const FResolvedSemanticWorkspaceRef& Workspace,
			const FAuthoringModelRef& CurrentModel,
			const FUeAgentStateRevision& CurrentRevision,
			const FString& ExpectedSelectionFingerprint,
			const FString& ExpectedClosureFingerprint,
			FSemanticWorkspaceImportError& OutError)
		{
			FWorkspaceError WorkspaceError;
			if (TryValidateSemanticWorkspaceBinding(
				Workspace,
				Workspace->GetMode(),
				CurrentModel,
				CurrentRevision,
				ExpectedSelectionFingerprint,
				ExpectedClosureFingerprint,
				WorkspaceError, false))
			{
				return true;
			}

			OutError = WorkspaceError.GetDiagnostic();
			if (WorkspaceError.GetCode()
				== TEXT("semantic_workspace_base_revision_mismatch"))
			{
				OutError.Code = TEXT("hjson_import_stale_base_revision");
			}
			else if (WorkspaceError.GetCode()
				== TEXT("semantic_workspace_state_revision_mismatch"))
			{
				OutError.Code = TEXT("hjson_import_stale_state_revision");
			}
			else if (WorkspaceError.GetCode()
				== TEXT("semantic_workspace_closure_mismatch"))
			{
				OutError.Code = TEXT("hjson_import_closure_mismatch");
			}
			else
			{
				OutError.Code = WorkspaceError.GetCode();
			}
			if (OutError.Code != WorkspaceError.GetCode())
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
					OutError.Code, WorkspaceError.GetDiagnostic(), TEXT("hjson_import"));
			}
			return false;
		}

		const FUeAgentCanonicalValueRef* FindObjectValue(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Key)
		{
			if (Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Row
				: Object->GetObjectItems())
			{
				if (Row.Key == Key)
				{
					return &Row.Value;
				}
			}
			return nullptr;
		}

		void LocateSemanticPropertyDiagnostic(const FResolvedSemanticWorkspaceRef& Workspace,
			const FString& UnitId, Contract::FDiagnostic& Diagnostic)
		{
			TArray<Contract::FDiagnostic*> Pending{&Diagnostic};
			while (!Pending.IsEmpty()) {
				auto& Issue = *Pending.Pop(EAllowShrinking::No);
				if (Issue.Source.SourceFile.IsEmpty() && Issue.SourceKind != TEXT("native_result")
					&& Issue.SourceKind != TEXT("command_input")) {
					if (Issue.Source.SourceUnit.IsEmpty()) Issue.Source.SourceUnit = UnitId;
					if (Issue.Source.InternalJsonPath.IsEmpty())
						Issue.Source.InternalJsonPath = Issue.Source.JsonPath.IsEmpty() ? TEXT("/") : Issue.Source.JsonPath;
					Workspace->LocateDocumentDiagnostic({}, {}, Issue);
					if (Issue.Source.SourceFile.IsEmpty()) {
						FWorkspaceError Located;
						Located.SetFailure(Issue);
						Located.LocateUnits(Workspace->GetUnitSources());
						Issue.Source = Located.GetDiagnostic().Source;
						if (!Issue.Source.SourceFile.IsEmpty()) Issue.SourceKind = TEXT("related_input");
					}
				}
				for (auto& Child : Issue.Diagnostics) Pending.Add(&Child);
				if (Issue.PublicCause) {
					auto Cause = MakeShared<Contract::FDiagnostic>(*Issue.PublicCause);
					Issue.PublicCause = Cause;
					Pending.Add(&Cause.Get());
				}
			}
		}

		Contract::FDiagnostic InvalidPropertyCategories(const FHjsonNode& Categories, FUeAgentResourceTracker& Resources)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_initial_categories_invalid"),
				TEXT("Property categories must be an object keyed by category names."), TEXT("hjson_import"));
			FWorkspaceError ProjectionError;
			if (!TryProjectHjsonNodeToJsonValue(Categories, Resources, Diagnostic.ActualValue, ProjectionError))
				return ProjectionError.GetDiagnostic();
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
			FWorkspaceError Located;
			Located.SetFailure(MoveTemp(Diagnostic));
			Located.Locate(Categories.GetProvenance());
			return Located.GetDiagnostic();
		}

		bool TryResolvePropertyValue(
			const FString& FieldId,
			const FString& UnitId,
			const FUeAgentCanonicalValueRef& RawValue,
			bool& bOutSkip,
			FUeAgentCanonicalValuePtr& OutValue,
			FSemanticWorkspaceImportError& OutError)
		{
			bOutSkip = false;
			OutValue = RawValue;
			const FUeAgentCanonicalValueRef* Presence =
				FindObjectValue(RawValue, TEXT("presence"));
			if (!Presence)
			{
				return true;
			}
			if ((*Presence)->GetKind() != EUeAgentValueKind::String)
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_property_presence_invalid"),
					TEXT("Property presence must be a string."), TEXT("hjson_import"), UnitId + TEXT(":") + FieldId);
				FUeAgentStateSemanticError ConversionError;
				if (!CanonicalJson::TryToJsonValue(**Presence, OutError.ActualValue, ConversionError)) OutError = MoveTemp(ConversionError);
				else OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("absent, inherited, default, or explicit"));
				OutError.Source.JsonPath = TEXT("/presence");
				OutError.Source.SourceUnit = UnitId;
				return false;
			}
			FString PresenceText = (*Presence)->GetText();
			PresenceText.TrimStartAndEndInline();
			PresenceText.ToLowerInline();
			if (PresenceText.IsEmpty()
				|| PresenceText == TEXT("absent")
				|| PresenceText == TEXT("inherited")
				|| PresenceText == TEXT("default"))
			{
				bOutSkip = true;
				OutValue.Reset();
				return true;
			}
			const FUeAgentCanonicalValueRef* ExplicitValue =
				FindObjectValue(RawValue, TEXT("value"));
			if (PresenceText != TEXT("explicit") || !ExplicitValue)
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_property_presence_invalid"),
					PresenceText != TEXT("explicit") ? TEXT("The property presence mode is not supported.")
						: TEXT("Explicit property presence requires a value."), TEXT("hjson_import"), UnitId + TEXT(":") + FieldId);
				if (PresenceText != TEXT("explicit")) {
					OutError.ActualValue = MakeShared<FJsonValueString>((*Presence)->GetText());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("absent, inherited, default, or explicit"));
				} else OutError.ActualValuePresent = false;
				OutError.Source.JsonPath = PresenceText != TEXT("explicit") ? TEXT("/presence") : TEXT("/value");
				OutError.Source.SourceUnit = UnitId;
				return false;
			}
			OutValue = *ExplicitValue;
			return true;
		}

		const FUeAgentCanonicalValue* DefinitionField(const FUeAgentCanonicalValue* Definition, const FString& Name)
		{
			if (Definition && Definition->GetKind() == EUeAgentValueKind::Object)
				for (const auto& Field : Definition->GetObjectItems())
					if (Field.Key == Name) return &Field.Value.Get();
			return nullptr;
		}

		const FUeAgentCanonicalValue* PropertyValueDefinition(const FSemanticUnit& Unit, const FAuthoringModel& Model,
			const FUeAgentCanonicalValue** OutType = nullptr)
		{
			for (const auto& Dependency : Unit.GetDependencyRefs())
				if (const auto* Definition = Model.FindUnit(Dependency); Definition && Definition->GetRole() == TEXT("property_definition"))
				{
					if (OutType) if (const auto* Type = Definition->GetValues().Find(TEXT("type"))) *OutType = &Type->Get();
					if (const auto* Value = Definition->GetValues().Find(TEXT("value_definition"))) return &Value->Get();
				}
			return nullptr;
		}

		// Compact authoring values are patch-shaped: fields omitted by the
		// model preserve the independently captured value. Rehydrate that
		// shape before comparison while still coercing whole-number lexemes
		// back to the captured decimal kind.
		enum class ECompactEnvelopeScope { None, UnitValues, Contents };

		bool TryOverlayCompactValue(const FUeAgentCanonicalValueRef& Current,
			const FUeAgentCanonicalValueRef& Proposed, const FAuthoringModel& Model, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue, FUeAgentStateSemanticError& OutError,
			const FUeAgentCanonicalValue* ValueDefinition = nullptr, const bool bFieldsContainer = false,
			const FUeAgentCanonicalValue* ValueType = nullptr, const bool bRequireOwnedBindings = false,
			const ECompactEnvelopeScope EnvelopeScope = ECompactEnvelopeScope::None)
		{
			OutValue = Proposed;
			if (const auto* InlineType = DefinitionField(ValueDefinition, TEXT("type"));
				DefinitionField(InlineType, TEXT("kind")))
			{
				ValueType = InlineType;
				ValueDefinition = DefinitionField(ValueDefinition, TEXT("value_definition"));
			}
			if (ValueDefinition && ValueDefinition->GetKind() == EUeAgentValueKind::String)
			{
				const auto* Definition = Model.FindUnit(ValueDefinition->GetText());
				if (const auto* Type = Definition ? Definition->GetValues().Find(TEXT("type")) : nullptr) ValueType = &Type->Get();
				const auto* Nested = Definition ? Definition->GetValues().Find(TEXT("value_definition")) : nullptr;
				ValueDefinition = Nested ? &Nested->Get() : nullptr;
			}
			const auto* NativeKind = DefinitionField(ValueType, TEXT("kind"));
			const FString CodecWrapper = NativeKind && NativeKind->GetKind() == EUeAgentValueKind::String
				? UeAgentAuthoringTypeDescriptors::NativeCodecValueWrapper(NativeKind->GetText()) : FString();
			const auto IsEnvelopeField = [&](const FString& Field)
			{
				return !bFieldsContainer && EnvelopeScope != ECompactEnvelopeScope::Contents
					&& ((EnvelopeScope == ECompactEnvelopeScope::UnitValues && Field == TEXT("value"))
						|| (!CodecWrapper.IsEmpty() && Field == CodecWrapper));
			};
			const auto* CollectionSemantics = DefinitionField(ValueDefinition, TEXT("collection_semantics"));
			if (CollectionSemantics && CollectionSemantics->GetKind() == EUeAgentValueKind::String
				&& CollectionSemantics->GetText() == TEXT("keyed_desired_state"))
			{
				// This boundary is a complete membership intention. Its native owner
				// resolves retained records with the shared keyed-item contract;
				// sparse object overlay must not resurrect omitted members first.
				return true;
			}
			const auto* ExistingInlineOnly = DefinitionField(ValueDefinition, TEXT("existing_inline_only"));
			const bool bExistingInlineOnly = ExistingInlineOnly
				&& ExistingInlineOnly->GetKind() == EUeAgentValueKind::Boolean && ExistingInlineOnly->GetBoolean();
			if (!bFieldsContainer && NativeKind && NativeKind->GetKind() == EUeAgentValueKind::String
				&& NativeKind->GetText() == TEXT("optional"))
			{
				const auto* Presence = DefinitionField(&Proposed.Get(), TEXT("is_set"));
				const auto* CurrentPresence = DefinitionField(&Current.Get(), TEXT("is_set"));
				// Presence changes do not inherit a value from the other branch.
				// Explicit contradictory input is retained for the native validator.
				if ((Presence && Presence->GetKind() == EUeAgentValueKind::Boolean && !Presence->GetBoolean())
					|| (CurrentPresence && CurrentPresence->GetKind() == EUeAgentValueKind::Boolean && !CurrentPresence->GetBoolean())) return true;
			}
			if (!bFieldsContainer && (bExistingInlineOnly
				|| (NativeKind && NativeKind->GetKind() == EUeAgentValueKind::String
					&& (NativeKind->GetText() == TEXT("map") || NativeKind->GetText() == TEXT("set")
						|| NativeKind->GetText() == TEXT("instanced_struct")))))
			{
				// Export owns compact formatting. Native ObjectApply owns matching
				// native collection identity and dynamic type completion; neither an
				// old position nor a different dynamic type supplies omitted fields.
				// Existing-inline-only descendants can belong to a new native class
				// after the root notification. Preserve their readable patch for the
				// actual ObjectApply context; it still validates every field and type.
				FUeAgentCanonicalValuePtr Ordinary;
				FWorkspaceError ProjectionError;
				if (!TryProjectDirectPropertyValue(Current, Model, ValueDefinition, Resources, Ordinary, ProjectionError, false, ValueType))
				{ OutError = ProjectionError.GetDiagnostic(); return false; }
				FUeAgentSemanticComparison Comparison;
				if (!TryCompareValues(Ordinary.ToSharedRef(), Proposed, TEXT("/native_value"), Resources, Comparison, OutError)) return false;
				if (Comparison.IsEquivalent()) OutValue = Current;
				else if (bExistingInlineOnly && !TryProjectDirectPropertyValue(
					Proposed, Model, ValueDefinition, Resources, OutValue, ProjectionError, true, ValueType))
				{ OutError = ProjectionError.GetDiagnostic(); return false; }
				return true;
			}
			const auto* Builtin = DefinitionField(ValueDefinition, TEXT("builtin"));
			if (Builtin && Builtin->GetKind() == EUeAgentValueKind::String && Builtin->GetText() == TEXT("text"))
			{
				FUeAgentCanonicalValueRef SourceEdit = Proposed;
				if (const auto* Literal = FindObjectValue(Proposed, TEXT("literal"))) SourceEdit = *Literal;
				if (SourceEdit->GetKind() != EUeAgentValueKind::String) return true; // Explicit identity is authoritative.
				const auto* CurrentSource = DefinitionField(&Current.Get(), TEXT("literal"));
				if (const auto* Localized = DefinitionField(&Current.Get(), TEXT("localized")))
					CurrentSource = DefinitionField(Localized, TEXT("source"));
				if (CurrentSource && CurrentSource->GetKind() == EUeAgentValueKind::String && CurrentSource->GetText() == SourceEdit->GetText())
				{
					OutValue = Current;
					return true;
				}
				// A changed ordinary source remains an edit intention; do not reuse the old identity as explicit input.
				return FUeAgentCanonicalValue::TryObject({{TEXT("literal"), SourceEdit}}, Resources, OutValue, OutError);
			}
			if (Current->GetKind() == EUeAgentValueKind::Decimal &&
				Proposed->GetKind() == EUeAgentValueKind::Integer)
			{
				return FUeAgentCanonicalValue::TryDecimal(
					Proposed->GetLexeme(), Resources, OutValue, OutError);
			}
			if (Current->GetKind() == EUeAgentValueKind::Object
				&& Current->GetObjectItems().Num() == 1)
			{
				const TPair<FString, FUeAgentCanonicalValueRef>& Only =
					Current->GetObjectItems()[0];
				// An explicit owned creation is a native object intention, not a
				// compact reference path. ObjectApply owns its eligibility and fields.
				if (Only.Key == TEXT("path") && NativeKind && NativeKind->GetKind() == EUeAgentValueKind::String
					&& NativeKind->GetText() == TEXT("object_ref") && FindObjectValue(Proposed, TEXT("owned_object"))) return true;
				if (IsEnvelopeField(Only.Key)
					&& FindObjectValue(Proposed, Only.Key) == nullptr)
				{
					FUeAgentCanonicalValuePtr RestoredInner;
					if (!TryOverlayCompactValue(Only.Value, Proposed, Model, Resources,
							RestoredInner, OutError, ValueDefinition, false, ValueType, bRequireOwnedBindings,
							Only.Key == TEXT("value") ? ECompactEnvelopeScope::None : ECompactEnvelopeScope::Contents))
					{
						return false;
					}
					return FUeAgentCanonicalValue::TryObject(
						{{Only.Key, RestoredInner.ToSharedRef()}},
						Resources, OutValue, OutError);
				}
			}
			const auto* RowKind = DefinitionField(ValueDefinition, TEXT("element_kind"));
			if (RowKind && RowKind->GetKind() == EUeAgentValueKind::String && RowKind->GetText() == TEXT("reflected_property"))
			{
				const auto* Definitions = DefinitionField(ValueDefinition, TEXT("items"));
				if (Current->GetKind() != EUeAgentValueKind::Array || !Definitions || Definitions->GetKind() != EUeAgentValueKind::Array
					|| Definitions->GetArrayItems().Num() != Current->GetArrayItems().Num())
				{
					OutError = Contract::FDiagnostic::Failure(TEXT("hjson_owned_property_schema_missing"),
						TEXT("The captured property collection and its item definitions must be arrays with matching lengths."), TEXT("hjson_import"));
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				TArray<FReadablePropertySchema> Schema;
				TArray<const FUeAgentCanonicalValue*> ValueDefinitions;
				for (const auto& Definition : Definitions->GetArrayItems())
				{
					const auto* ValueSchema = DefinitionField(DefinitionField(&Definition.Get(), TEXT("fields")), TEXT("value"));
					const auto* Unit = ValueSchema && ValueSchema->GetKind() == EUeAgentValueKind::String ? Model.FindUnit(ValueSchema->GetText()) : nullptr;
					const auto* Type = Unit ? Unit->GetValues().Find(TEXT("type")) : nullptr;
					FReadablePropertySchema Item;
					if (!Type || !TryDescribeReadableProperty(Definition.Get(), Type->Get(), Item))
				{
					OutError = Contract::FDiagnostic::Failure(TEXT("hjson_owned_property_schema_missing"),
						!Type ? TEXT("The captured property item has no resolvable type definition.")
							: TEXT("The captured property item definition does not describe a readable property."), TEXT("hjson_import"));
					OutError.SourceKind = TEXT("native_result");
					OutError.InternalContext = ValueSchema && ValueSchema->GetKind() == EUeAgentValueKind::String ? ValueSchema->GetText() : FString();
					return false;
				}
					const auto Address = DescribeReadableProperty(*Unit, Model);
					Item.Type = Address.Type;
					Item.Definition = Address.Definition;
					Schema.Add(MoveTemp(Item)); ValueDefinitions.Add(ValueSchema);
				}
				TArray<FResolvedReadableProperty> Rows;
				if (!TryReadReadablePropertyRows(Proposed.Get(), Schema, Rows, OutError))
				{
					return false;
				}
				auto Items = Current->GetArrayItems();
				for (const auto& Row : Rows)
				{
					const auto* Old = FindObjectValue(Items[Row.SchemaIndex], TEXT("value"));
					FUeAgentCanonicalValuePtr Resolved, Descriptor;
					if (!Old) {
						OutError = Contract::FDiagnostic::Failure(TEXT("hjson_owned_property_value_missing"),
							TEXT("The captured property row has no value to preserve while applying this edit."), TEXT("hjson_import"));
						OutError.SourceKind = TEXT("native_result");
						return false;
					}
					if (!TryOverlayCompactValue(*Old, Row.Value.ToSharedRef(), Model, Resources, Resolved, OutError, ValueDefinitions[Row.SchemaIndex], false, nullptr, bRequireOwnedBindings)) {
						const auto& Address = Schema[Row.SchemaIndex];
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
							TEXT("/") + Address.Category.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))
							+ TEXT("/") + Address.Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")) + TEXT("/value"));
						return false;
					}
					auto Fields = Items[Row.SchemaIndex]->GetObjectItems();
					for (auto& Field : Fields) if (Field.Key == TEXT("value")) Field.Value = Resolved.ToSharedRef();
					if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Descriptor, OutError)) return false;
					Items[Row.SchemaIndex] = Descriptor.ToSharedRef();
				}
				return FUeAgentCanonicalValue::TryArray(Items, Resources, OutValue, OutError);
			}
			if (Current->GetKind() != Proposed->GetKind())
			{
				return true;
			}
			if (Current->GetKind() == EUeAgentValueKind::Array)
			{
				const TArray<FUeAgentCanonicalValueRef>& CurrentItems = Current->GetArrayItems();
				const TArray<FUeAgentCanonicalValueRef>& ProposedItems = Proposed->GetArrayItems();
				const auto* Definitions = DefinitionField(ValueDefinition, TEXT("items"));
				const auto* ElementKind = DefinitionField(ValueDefinition, TEXT("element_kind"));
				const bool bOrderedOwned = Definitions && Definitions->GetKind() == EUeAgentValueKind::Array
					&& ElementKind && ElementKind->GetKind() == EUeAgentValueKind::String && ElementKind->GetText() == TEXT("owned_object");
				const bool bOrderedStructured = Definitions && Definitions->GetKind() == EUeAgentValueKind::Array
					&& ElementKind && ElementKind->GetKind() == EUeAgentValueKind::String && ElementKind->GetText() == TEXT("structured_owned_item");
				if (bOrderedOwned)
				{
					TArray<FOrderedOwnedItemBinding> Bindings; FWorkspaceError BindingError;
					if (!TryResolveOrderedOwnedItems(Proposed.Get(), *Definitions, Resources, Bindings, BindingError))
					{ OutError = BindingError.GetDiagnostic(); return false; }
					TArray<FUeAgentCanonicalValueRef> BusinessItems, ApplyItems;
					bool bSameOrder = Bindings.Num() == CurrentItems.Num();
					for (int32 Index = 0; Index < Bindings.Num(); ++Index)
					{
						const auto& Binding = Bindings[Index];
						const bool bNullItem = Binding.Value->GetKind() == EUeAgentValueKind::Null
							|| (DefinitionField(Binding.Value.Get(), TEXT("path"))
								&& DefinitionField(Binding.Value.Get(), TEXT("path"))->GetKind() == EUeAgentValueKind::Null);
						bSameOrder &= Binding.CapturedIndex == Index || bNullItem;
						FUeAgentCanonicalValuePtr Desired = Binding.Value;
						if (Binding.CapturedIndex != INDEX_NONE)
						{
							if (!CurrentItems.IsValidIndex(Binding.CapturedIndex) || !Definitions->GetArrayItems().IsValidIndex(Binding.CapturedIndex)) {
								OutError = Contract::FDiagnostic::Failure(TEXT("hjson_owned_item_binding_invalid"),
									TEXT("The resolved owned item binding does not identify a captured item and its definition."), TEXT("hjson_import"));
								OutError.SourceKind = TEXT("native_result");
								OutError.ActualValue = MakeShared<FJsonValueNumber>(Binding.CapturedIndex);
								return false;
							}
							if (!TryOverlayCompactValue(CurrentItems[Binding.CapturedIndex], Binding.Value.ToSharedRef(),
								Model, Resources, Desired, OutError, &Definitions->GetArrayItems()[Binding.CapturedIndex].Get())) {
								UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/") + FString::FromInt(Index));
								return false;
							}
						}
						BusinessItems.Add(Desired.ToSharedRef());
						const auto* Owned = FindObjectValue(Desired.ToSharedRef(), TEXT("owned_object"));
						if (!Owned) { ApplyItems.Add(Desired.ToSharedRef()); continue; } // Explicit nullable slot.
						if ((*Owned)->GetKind() != EUeAgentValueKind::Object) {
							OutError = Contract::FDiagnostic::Failure(TEXT("hjson_owned_item_value_invalid"),
								TEXT("The owned_object value must be an object."), TEXT("hjson_import"));
							FUeAgentStateSemanticError ConversionError;
							if (!CanonicalJson::TryToJsonValue(**Owned, OutError.ActualValue, ConversionError)) OutError = MoveTemp(ConversionError);
							else OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/") + FString::FromInt(Index) + TEXT("/owned_object"));
							return false;
						}
						auto Fields = (*Owned)->GetObjectItems();
						FUeAgentCanonicalValuePtr Selector, ApplyOwned, ApplyItem;
						if (!FUeAgentCanonicalValue::TryString(Binding.RelativePath, Resources, Selector, OutError)) return false;
						Fields.Emplace(TEXT("owner_relative_path"), Selector.ToSharedRef());
						if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, ApplyOwned, OutError)
							|| !FUeAgentCanonicalValue::TryObject({{TEXT("owned_object"), ApplyOwned.ToSharedRef()}}, Resources, ApplyItem, OutError)) return false;
						ApplyItems.Add(ApplyItem.ToSharedRef());
					}
					FUeAgentCanonicalValuePtr Business;
					if (!FUeAgentCanonicalValue::TryArray(BusinessItems, Resources, Business, OutError)) return false;
					FUeAgentSemanticComparison Comparison;
					if (!TryCompareValues(Current, Business.ToSharedRef(), TEXT("/items"), Resources, Comparison, OutError)) return false;
					if (!bRequireOwnedBindings && bSameOrder && Comparison.IsEquivalent()) { OutValue = Current; return true; }
					return FUeAgentCanonicalValue::TryArray(ApplyItems, Resources, OutValue, OutError);
				}
				if (bOrderedStructured)
				{
					if (Definitions->GetArrayItems().Num() != CurrentItems.Num())
					{
						OutError = Contract::FDiagnostic::Failure(TEXT("hjson_structured_item_schema_missing"),
							TEXT("The structured collection has a different number of captured items and item definitions."), TEXT("hjson_import"));
						OutError.SourceKind = TEXT("native_result");
						OutError.ExpectedValue = MakeShared<FJsonValueNumber>(CurrentItems.Num());
						OutError.ActualValue = MakeShared<FJsonValueNumber>(Definitions->GetArrayItems().Num());
						return false;
					}
					TArray<FOrderedStructuredItemBinding> Bindings; FWorkspaceError BindingError;
					if (!TryResolveOrderedStructuredItems(Proposed.Get(), *Definitions, Resources, Bindings, BindingError))
					{ OutError = BindingError.GetDiagnostic(); return false; }
					TArray<FUeAgentCanonicalValueRef> RestoredItems;
					RestoredItems.Reserve(Bindings.Num());
					for (int32 Index = 0; Index < Bindings.Num(); ++Index)
					{
						const auto& Binding = Bindings[Index];
						FUeAgentCanonicalValuePtr Restored = Binding.Value;
						if (Binding.CapturedIndex != INDEX_NONE)
						{
							if (!CurrentItems.IsValidIndex(Binding.CapturedIndex)) {
								OutError = Contract::FDiagnostic::Failure(TEXT("hjson_structured_item_binding_invalid"),
									TEXT("The resolved structured item binding does not identify a captured item."), TEXT("hjson_import"));
								OutError.SourceKind = TEXT("native_result");
								OutError.ActualValue = MakeShared<FJsonValueNumber>(Binding.CapturedIndex);
								return false;
							}
							if (!TryOverlayCompactValue(CurrentItems[Binding.CapturedIndex], Binding.Value.ToSharedRef(),
									Model, Resources, Restored, OutError, &Definitions->GetArrayItems()[Binding.CapturedIndex].Get(),
									false, DefinitionField(ValueType, TEXT("element_type")),
									bRequireOwnedBindings || Binding.CapturedIndex != Index))
							{
								UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/") + FString::FromInt(Index));
								return false;
							}
						}
						if (Binding.CapturedIndex != Index)
						{
							// Carry the existing public Entry binding into native completion.
							// This source selector is apply metadata, not a struct member.
							FUeAgentCanonicalValuePtr Source;
							if (Binding.CapturedIndex == INDEX_NONE)
							{
								if (!FUeAgentCanonicalValue::TryNull(Resources, Source, OutError)) return false;
							}
							else if (!FUeAgentCanonicalValue::TryInteger(FString::FromInt(Binding.CapturedIndex), Resources, Source, OutError)) return false;
							auto Fields = Restored->GetObjectItems();
							Fields.Emplace(TEXT("from"), Source.ToSharedRef());
							if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Restored, OutError)) return false;
						}
						RestoredItems.Add(Restored.ToSharedRef());
					}
					return FUeAgentCanonicalValue::TryArray(RestoredItems, Resources, OutValue, OutError);
				}
				// An ordinary sequence is the authored inventory, not an index patch.
				// Equal length does not establish the identity of its records. Keep
				// omitted members absent for the native owner to resolve against its
				// real context; explicit owned/structured bindings were handled above.
				return true;
			}
			if (Current->GetKind() == EUeAgentValueKind::Object)
			{
				const auto* DeclaredFields = DefinitionField(ValueDefinition, TEXT("fields"));
				const auto* FieldsComplete = DefinitionField(ValueDefinition, TEXT("fields_complete"));
				const bool bCompleteFields = !FieldsComplete || FieldsComplete->GetKind() != EUeAgentValueKind::Boolean
					|| FieldsComplete->GetBoolean();
				const bool bWrappedStruct = !bFieldsContainer && NativeKind
					&& NativeKind->GetKind() == EUeAgentValueKind::String && NativeKind->GetText() == TEXT("struct");
				const bool bDynamicBag = Builtin && Builtin->GetKind() == EUeAgentValueKind::String
					&& Builtin->GetText() == TEXT("property_bag");
				// Declared structures are closed by their current formal definition;
				// map membership, property-bag schemas and native struct envelopes
				// keep their existing owners. No asset-specific field list is needed.
				if (bCompleteFields && !bWrappedStruct && !bDynamicBag && DeclaredFields
					&& DeclaredFields->GetKind() == EUeAgentValueKind::Object)
				{
					for (const auto& Field : Proposed->GetObjectItems())
					{
						if (DefinitionField(DeclaredFields, Field.Key)) continue;
						OutError = Contract::FDiagnostic::Failure(TEXT("hjson_declared_value_unknown_field"),
							TEXT("The authored field is not declared for this value."), TEXT("hjson_import"));
						OutError.Source.JsonPath = TEXT("/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						OutError.ActualValue = MakeShared<FJsonValueString>(Field.Key);
						TArray<TSharedPtr<FJsonValue>> AllowedFields;
						for (const auto& Allowed : DeclaredFields->GetObjectItems()) AllowedFields.Add(MakeShared<FJsonValueString>(Allowed.Key));
						OutError.ExpectedValue = MakeShared<FJsonValueArray>(AllowedFields);
						return false;
					}
				}
				auto CurrentItems = Current->GetObjectItems();
				const auto& ProposedItems = Proposed->GetObjectItems();
				const auto* ProposedSchema = FindObjectValue(Proposed, TEXT("schema"));
				const auto* CurrentSchema = FindObjectValue(Current, TEXT("schema"));
				if (!bFieldsContainer && Builtin && Builtin->GetKind() == EUeAgentValueKind::String
					&& Builtin->GetText() == TEXT("property_bag") && ProposedSchema && CurrentSchema
					&& (*ProposedSchema)->GetKind() == EUeAgentValueKind::Array
					&& (*CurrentSchema)->GetKind() == EUeAgentValueKind::Array)
				{
					// Schema is a desired inventory, not an index-based patch. Only
					// unchanged descriptors may supply omitted values from the capture.
					TSet<FString> RetainedFields;
					for (const auto& DesiredDesc : (*ProposedSchema)->GetArrayItems())
					{
						const auto* Name = DefinitionField(&DesiredDesc.Get(), TEXT("name"));
						if (!Name || Name->GetKind() != EUeAgentValueKind::String) continue;
						for (const auto& CapturedDesc : (*CurrentSchema)->GetArrayItems())
						{
							const auto* CapturedName = DefinitionField(&CapturedDesc.Get(), TEXT("name"));
							if (!CapturedName || CapturedName->GetKind() != EUeAgentValueKind::String
								|| CapturedName->GetText() != Name->GetText()) continue;
							FUeAgentSemanticComparison Comparison;
							if (!TryCompareValues(CapturedDesc, DesiredDesc, TEXT("/schema"), Resources, Comparison, OutError)) return false;
							if (Comparison.IsEquivalent()) RetainedFields.Add(Name->GetText());
						}
					}
					for (auto& Item : CurrentItems)
					{
						if (Item.Key == TEXT("schema")) Item.Value = *ProposedSchema;
						if (Item.Key != TEXT("fields") || Item.Value->GetKind() != EUeAgentValueKind::Object) continue;
						auto Fields = Item.Value->GetObjectItems();
						Fields.RemoveAll([&RetainedFields](const auto& Field) { return !RetainedFields.Contains(Field.Key); });
						FUeAgentCanonicalValuePtr Retained;
						if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Retained, OutError)) return false;
						Item.Value = Retained.ToSharedRef();
					}
				}
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> RestoredItems;
				RestoredItems.Reserve(FMath::Max(CurrentItems.Num(), ProposedItems.Num()));
				TSet<FString> RestoredKeys;
				bool bChanged = CurrentItems.Num() != ProposedItems.Num();
				FUeAgentCanonicalValuePtr ProjectedCapture;
				for (const TPair<FString, FUeAgentCanonicalValueRef>& CurrentItem : CurrentItems)
				{
					const FUeAgentCanonicalValueRef* ProposedValue = nullptr;
					for (const TPair<FString, FUeAgentCanonicalValueRef>& ProposedItem :
						ProposedItems)
					{
						if (ProposedItem.Key == CurrentItem.Key)
						{
							ProposedValue = &ProposedItem.Value;
							break;
						}
					}
					RestoredKeys.Add(CurrentItem.Key);
					if (!ProposedValue && bRequireOwnedBindings)
					{
						// An omitted descendant is still part of a moved struct item.
						// Re-project its captured identity so the ordinary recursive
						// resolver restores bindings instead of cloning by destination.
						if (!ProjectedCapture)
						{
							FUeAgentCanonicalValuePtr ProjectionSource = Current;
							if (bFieldsContainer && !FUeAgentCanonicalValue::TryObject(
								{{TEXT("fields"), Current}}, Resources, ProjectionSource, OutError)) return false;
							FWorkspaceError ProjectionError;
							if (!TryProjectDirectPropertyValue(ProjectionSource.ToSharedRef(), Model, ValueDefinition,
								Resources, ProjectedCapture, ProjectionError, false, ValueType))
							{ OutError = ProjectionError.GetDiagnostic(); return false; }
						}
						const auto* Members = bFieldsContainer ? FindObjectValue(ProjectedCapture.ToSharedRef(), TEXT("fields")) : nullptr;
						ProposedValue = FindObjectValue(Members ? *Members : ProjectedCapture.ToSharedRef(), CurrentItem.Key);
					}
					if (!ProposedValue)
					{
						bChanged = true;
						RestoredItems.Add(CurrentItem);
						continue;
					}
					FUeAgentCanonicalValuePtr Restored;
					if (!TryOverlayCompactValue(
							CurrentItem.Value, *ProposedValue, Model, Resources, Restored, OutError,
							(CurrentItem.Key == TEXT("fields") || (CurrentItems.Num() == 1
								&& IsEnvelopeField(CurrentItem.Key)))
								? ValueDefinition : DefinitionField(DefinitionField(ValueDefinition, TEXT("fields")), CurrentItem.Key),
							CurrentItem.Key == TEXT("fields"),
							(CurrentItem.Key == TEXT("fields") || IsEnvelopeField(CurrentItem.Key)) ? ValueType
								: CurrentItem.Key == TEXT("value") && NativeKind && NativeKind->GetKind() == EUeAgentValueKind::String
									&& NativeKind->GetText() == TEXT("optional") ? DefinitionField(ValueType, TEXT("inner_type")) : nullptr,
							bRequireOwnedBindings,
							CurrentItems.Num() == 1 && IsEnvelopeField(CurrentItem.Key) && CurrentItem.Key != TEXT("value")
								? ECompactEnvelopeScope::Contents : ECompactEnvelopeScope::None))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
							TEXT("/") + CurrentItem.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
						return false;
					}
					bChanged |= Restored.Get() != &ProposedValue->Get();
					RestoredItems.Emplace(CurrentItem.Key, Restored.ToSharedRef());
				}
				for (const TPair<FString, FUeAgentCanonicalValueRef>& ProposedItem : ProposedItems)
				{
					if (!RestoredKeys.Contains(ProposedItem.Key))
					{
						bChanged = true;
						RestoredItems.Add(ProposedItem);
					}
				}
				return !bChanged || FUeAgentCanonicalValue::TryObject(
										RestoredItems, Resources, OutValue, OutError);
			}
			return true;
		}

		TSet<FString> PropertyOwnerCandidates(
			const FSemanticUnit& Unit, const FAuthoringModelRef& Model)
		{
			TSet<FString> Result;
			const FSemanticUnit* Current = &Unit;
			TSet<FString> Seen;
			while (Current && !Seen.Contains(Current->GetUnitId()))
			{
				Seen.Add(Current->GetUnitId());
				Result.Add(Current->GetUnitId());
				if (!Current->GetTargetIdentityKey().IsEmpty())
				{
					Result.Add(Current->GetTargetIdentityKey());
				}
				if (Current->GetOwnerRef().IsEmpty())
				{
					break;
				}
				Result.Add(Current->GetOwnerRef());
				Current = Model->FindUnit(Current->GetOwnerRef());
			}
			return Result;
		}

		Contract::FDiagnostic TargetResolutionFailure(
			const FSemanticUnit& Unit, const FString& Field, bool bAmbiguous,
			const FString& Reason, const TSharedRef<FJsonObject>& Facts)
		{
			auto Error = Contract::FDiagnostic::Failure(
				bAmbiguous ? TEXT("hjson_import_property_target_ambiguous") : TEXT("hjson_import_property_target_missing"),
				Reason, TEXT("hjson_import"), Unit.GetTargetIdentityKey());
			Error.SourceKind = TEXT("related_input");
			Error.Source.SourceUnit = Unit.GetUnitId();
			Error.PropertyPath = Field;
			Error.ActualValue = MakeShared<FJsonValueObject>(Facts);
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("one uniquely resolved current target"));
			Error.ActualValuePresent = true;
			Error.ExpectedValuePresent = true;
			return Error;
		}

		const FUeAgentStateEntry* MatchPropertyEntry(
			const TArray<const FUeAgentStateEntry*>& Entries, const FSemanticUnit& Unit,
			const FString& FieldId, const FAuthoringModelRef& Model, Contract::FDiagnostic& OutError)
		{
			if (Entries.IsEmpty())
			{
				auto Facts = MakeShared<FJsonObject>();
				Facts->SetNumberField(TEXT("candidate_pool_count"), 0);
				OutError = TargetResolutionFailure(Unit, FieldId, false,
					TEXT("No current state entry exists for this property"), Facts);
				return nullptr;
			}
			const TSet<FString> Owners = PropertyOwnerCandidates(Unit, Model);
			TSet<FString> Selectors;
			Selectors.Add(FieldId);
			if (!Unit.GetFieldSetRef().IsEmpty()) Selectors.Add(Unit.GetFieldSetRef());
			int32 ObservedMatches[3] = {};
			const FUeAgentStateEntry* ObservedEntries[3][2] = {};
			auto UniqueMatch = [&Entries, &ObservedMatches, &ObservedEntries](
				int32 Phase, TFunctionRef<bool(const FUeAgentStateEntry&)> Predicate) -> const FUeAgentStateEntry*
			{
				const FUeAgentStateEntry* Result = nullptr;
				for (const FUeAgentStateEntry* Entry : Entries)
				{
					if (!Entry || !Predicate(*Entry)) continue;
					ObservedEntries[Phase][ObservedMatches[Phase]++] = Entry;
					if (Result) return nullptr;
					Result = Entry;
				}
				return Result;
			};
			if (const FUeAgentStateEntry* Exact = UniqueMatch(0,
				[&Owners, &Selectors](const FUeAgentStateEntry& Entry)
				{
					return Owners.Contains(Entry.GetIdentity().GetOwnerId()) &&
						Selectors.Contains(Entry.GetIdentity().GetSelector());
				}))
			{
				return Exact;
			}
			if (const FUeAgentStateEntry* Owner = UniqueMatch(1,
				[&Owners](const FUeAgentStateEntry& Entry)
					{ return Owners.Contains(Entry.GetIdentity().GetOwnerId()); }))
			{
				return Owner;
			}
			if (const FUeAgentStateEntry* Selector = UniqueMatch(2,
				[&Selectors](const FUeAgentStateEntry& Entry)
					{ return Selectors.Contains(Entry.GetIdentity().GetSelector()); }))
			{
				return Selector;
			}
			if (Entries.Num() == 1 && Entries[0]) return Entries[0];
			const TCHAR* Phases[] = {TEXT("owner_and_selector"), TEXT("owner"), TEXT("selector")};
			auto Facts = MakeShared<FJsonObject>();
			auto Evidence = MakeShared<FJsonObject>();
			Facts->SetNumberField(TEXT("candidate_pool_count"), Entries.Num());
			for (int32 Phase = 0; Phase < 3; ++Phase)
			{
				// Matching stops at the second hit; report a lower bound, not a total.
				Facts->SetNumberField(FString(Phases[Phase]) +
					(ObservedMatches[Phase] == 2 ? TEXT("_matches_at_least") : TEXT("_matches")),
					ObservedMatches[Phase]);
				TArray<TSharedPtr<FJsonValue>> Identities;
				for (int32 Index = 0; Index < ObservedMatches[Phase]; ++Index)
					Identities.Add(MakeShared<FJsonValueString>(ObservedEntries[Phase][Index]->GetIdentity().GetCanonicalKey()));
				Evidence->SetArrayField(Phases[Phase], Identities);
			}
			OutError = TargetResolutionFailure(Unit, FieldId, Entries.Num() != 1,
				Entries.Num() == 1 ? TEXT("The only current property candidate is unavailable")
					: TEXT("The property owner and selector do not identify a unique current target"), Facts);
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Evidence);
			return nullptr;
		}

		const FUeAgentStateEntry* MatchSemanticTargetEntry(
			const TArray<const FUeAgentStateEntry*>& Entries,
			const FSemanticUnit& Unit, Contract::FDiagnostic& OutError,
			bool bAllowMissing, int32 SemanticCandidatePoolCount)
		{
			const FString& TargetIdentityKey = Unit.GetTargetIdentityKey();
			const FUeAgentStateEntry* Result = nullptr;
			for (const FUeAgentStateEntry* Entry : Entries)
			{
				if (!Entry
					|| (Entry->GetIdentity().GetCanonicalKey() != TargetIdentityKey
						&& Entry->GetIdentity().GetSelector(
							CanonicalAuthoringTargetIdentitySelector) != TargetIdentityKey)) continue;
				if (Result)
				{
					auto Facts = MakeShared<FJsonObject>();
					Facts->SetNumberField(TEXT("matches_at_least"), 2);
					OutError = TargetResolutionFailure(Unit, TEXT("target_identity_key"), true,
						TEXT("The semantic target identity matches more than one current state entry"), Facts);
					OutError.SemanticId = Unit.GetSemanticTypeRef();
					OutError.ActualEvidence = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueString>(Result->GetIdentity().GetCanonicalKey()),
						MakeShared<FJsonValueString>(Entry->GetIdentity().GetCanonicalKey())});
					return nullptr;
				}
				Result = Entry;
			}
			if (!Result && !bAllowMissing)
			{
				auto Facts = MakeShared<FJsonObject>();
				Facts->SetNumberField(TEXT("matching_target_count"), 0);
				Facts->SetNumberField(TEXT("semantic_candidate_pool_count"), SemanticCandidatePoolCount);
				OutError = TargetResolutionFailure(Unit, TEXT("target_identity_key"), false,
					TEXT("No current state entry matches the semantic target identity"), Facts);
				OutError.SemanticId = Unit.GetSemanticTypeRef();
			}
			return Result;
		}

		bool TryStringValue(const FString& Text, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue, FSemanticWorkspaceImportError& OutError)
		{
			OutValue.Reset();
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(Text, Resources, OutValue, Error))
			{
				OutError = (Error.IsValid() ? Error : UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_import_value_invalid"), TEXT("The canonical string builder failed without reporting a diagnostic."), TEXT("hjson_import")));
				return false;
			}
			return true;
		}

		bool TryStructuralEditValue(const FStructuralEdit& Edit, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue, FSemanticWorkspaceImportError& OutError)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
			auto AddString = [&Rows, &Resources, &OutError](
								 const FString& Key, const FString& Value) -> bool
			{
				if (Value.IsEmpty())
				{
					return true;
				}
				FUeAgentCanonicalValuePtr Canonical;
				if (!TryStringValue(Value, Resources, Canonical, OutError))
				{
					return false;
				}
				Rows.Emplace(Key, Canonical.ToSharedRef());
				return true;
			};
			if (!AddString(TEXT("edit_id"), Edit.GetEditId())
				|| !AddString(TEXT("kind"), LexToString(Edit.GetKind()))
				|| !AddString(
					TEXT("owner_unit_id"),
					Edit.GetOwnerUnitId())
				|| !AddString(
					TEXT("subject_unit_id"),
					Edit.GetSubjectUnitId())
				|| !AddString(
					TEXT("replacement_unit_id"),
					Edit.GetReplacementUnitId())
				|| !AddString(
					TEXT("relation_id"),
					Edit.GetRelationId()))
			{
				return false;
			}
			if (!Edit.GetOrderedUnitIds().IsEmpty())
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const FString& UnitId : Edit.GetOrderedUnitIds())
				{
					FUeAgentCanonicalValuePtr Item;
					if (!TryStringValue(
						UnitId,
						Resources,
						Item,
						OutError))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr Ordered;
				FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					Ordered,
					Error))
				{
					OutError = (Error.IsValid() ? Error : UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_import_value_invalid"), TEXT("The canonical array builder failed without reporting a diagnostic."), TEXT("hjson_import")));
					return false;
				}
				Rows.Emplace(
					TEXT("ordered_unit_ids"),
					Ordered.ToSharedRef());
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
				Rows,
				Resources,
				OutValue,
				Error))
			{
				OutError = (Error.IsValid() ? Error : UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_import_value_invalid"), TEXT("The canonical object builder failed without reporting a diagnostic."), TEXT("hjson_import")));
				return false;
			}
			return true;
		}

		bool TryAppendPropertyTypeAssertions(
			const FSemanticUnit& PropertyUnit,
			const FAuthoringModelRef& Model,
			const TSharedRef<FJsonObject>& Descriptor,
			FSemanticWorkspaceImportError& OutError,
			const FResolvedSemanticWorkspacePtr& Workspace = nullptr)
		{
			auto FailType = [&](Contract::FDiagnostic Diagnostic, const FSemanticUnit* TypeUnit,
				const FString& Pointer)
			{
				Diagnostic.SubjectId = PropertyUnit.GetUnitId();
				Diagnostic.PropertyPath = Descriptor->GetStringField(TEXT("property_path"));
				Diagnostic.SemanticPointer = Pointer;
				if (!Workspace.IsValid())
				{
					// Initial creation has a template model and a direct HJSON row;
					// its caller binds the row's provenance without inventing a workspace.
					OutError = MoveTemp(Diagnostic);
					OutError.SourceKind = TEXT("related_input");
					return false;
				}
				const auto IsPublicSource = [](const FSourceProvenance* Source)
				{
					return Source && !Source->GetSourceUnit().Contains(TEXT(".uai/"))
						&& !Source->GetSourceUnit().Contains(TEXT(".uai\\"));
				};
				const FSourceProvenance* Source = TypeUnit ? Workspace->FindValueSource(TypeUnit->GetUnitId(), Pointer) : nullptr;
				bool bExact = IsPublicSource(Source);
				if (!bExact)
				{
					// The type definition may live in the hidden envelope. In that case
					// the authored value is related context, not an erroneous type leaf.
					Source = Workspace->FindValueSource(PropertyUnit.GetUnitId(), TEXT("/value"));
					if (!IsPublicSource(Source)) Source = Workspace->GetUnitSources().Find(PropertyUnit.GetUnitId());
				}
				FWorkspaceError Located;
				Located.SetFailure(MoveTemp(Diagnostic));
				if (IsPublicSource(Source)) Located.Locate(*Source);
				OutError = Located.GetDiagnostic();
				OutError.PropertyPath = Descriptor->GetStringField(TEXT("property_path"));
				if (IsPublicSource(Source)) OutError.SourceKind = bExact ? TEXT("authored_value") : TEXT("related_input");
				return false;
			};
			const FSemanticUnit* Definition = nullptr;
			for (const FString& DependencyRef : PropertyUnit.GetDependencyRefs())
			{
				const FSemanticUnit* Candidate = Model->FindUnit(DependencyRef);
				if (Candidate == nullptr || Candidate->GetRole() != TEXT("property_definition"))
				{
					continue;
				}
				if (Definition != nullptr)
				{
					return FailType(Contract::FDiagnostic::Failure(TEXT("hjson_import_property_type_ambiguous"),
						TEXT("The property is bound to more than one type definition."), TEXT("preflight")), nullptr, TEXT("/type"));
				}
				Definition = Candidate;
			}
			const FUeAgentCanonicalValueRef* Type = Definition == nullptr
				? nullptr : Definition->GetValues().Find(TEXT("type"));
			if (Type == nullptr || (*Type)->GetKind() != EUeAgentValueKind::Object)
			{
				return FailType(Contract::FDiagnostic::Failure(TEXT("hjson_import_property_type_missing"),
					TEXT("The property has no usable object type definition."), TEXT("preflight")), Definition, TEXT("/type"));
			}
			TSharedPtr<FJsonValue> TypeJson;
			UE::UAI::Contract::FDiagnostic ValueError;
			if (!CanonicalJson::TryToJsonValue(**Type, TypeJson, ValueError)
				|| !TypeJson.IsValid() || TypeJson->Type != EJson::Object)
			{
				return FailType(ImportFailure(TEXT("hjson_import_property_type_invalid"), ValueError, TEXT("The captured property type must convert to an object descriptor.")), Definition, TEXT("/type"));
			}
			static const TSet<FString> AssertionFields{
				TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"),
				TEXT("element_type"), TEXT("key_type"), TEXT("mapped_type"),
				TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class"), TEXT("hash_source")};
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : TypeJson->AsObject()->Values)
			{
				// Exported explanations are owned by the native type descriptor.
				// Keep them in the workspace, without promoting them to assertions.
				if (UeAgentAuthoringTypeDescriptors::IsReadOnlyTypeMetadataField(Field.Key)) continue;
				if (!AssertionFields.Contains(Field.Key))
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_import_property_type_invalid"),
						TEXT("The type definition contains an unsupported assertion field."), TEXT("preflight"));
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Field.Key);
					return FailType(MoveTemp(Diagnostic), Definition,
						TEXT("/type/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
				}
				Descriptor->SetField(Field.Key, Field.Value);
			}
			if (!Descriptor->HasField(TEXT("kind")))
			{
				return FailType(Contract::FDiagnostic::Failure(TEXT("hjson_import_property_type_invalid"),
					TEXT("The type definition is missing its required kind."), TEXT("preflight")), Definition, TEXT("/type/kind"));
			}
			return true;
		}

		const FSemanticUnit* FindPropertyUnitForIdentity(
			const FAuthoringModelRef& Model, const FString& IdentityKey,
			const FString& AuthoringTargetIdentityKey)
		{
			const FSemanticUnit* Result = nullptr;
			for (const auto* Unit : FindIdentityUnits(Model, IdentityKey, AuthoringTargetIdentityKey))
			{
				if (Unit->GetUnitKind() != ESemanticUnitKind::PropertySet)
				{
					continue;
				}
				if (Result != nullptr)
				{
					return nullptr;
				}
				Result = Unit;
			}
			return Result;
		}

		// Workspace updates describe edits against a capture, not assignments of
		// every exported struct member. Keep containers and native atomic values
		// intact; only declared ordinary structs have member-patch semantics.
		TSharedPtr<FJsonValue> ProjectEditedStructMembers(
			const TSharedPtr<FJsonValue>& Before, const TSharedPtr<FJsonValue>& After,
			const FAuthoringModel& Model, const FUeAgentCanonicalValue* Definition,
			const FUeAgentCanonicalValue* Type, FWorkspaceError& OutError)
		{
			if (Definition && Definition->GetKind() == EUeAgentValueKind::String)
			{
				const auto* Unit = Model.FindUnit(Definition->GetText());
				const auto* NestedType = Unit ? Unit->GetValues().Find(TEXT("type")) : nullptr;
				const auto* NestedDefinition = Unit ? Unit->GetValues().Find(TEXT("value_definition")) : nullptr;
				Type = NestedType ? &NestedType->Get() : nullptr;
				Definition = NestedDefinition ? &NestedDefinition->Get() : nullptr;
			}
			else if (const auto* InlineType = DefinitionField(Definition, TEXT("type")))
			{
				Type = InlineType;
				Definition = DefinitionField(Definition, TEXT("value_definition"));
			}
			const auto* Kind = DefinitionField(Type, TEXT("kind"));
			if (!Kind || Kind->GetKind() != EUeAgentValueKind::String || Kind->GetText() != TEXT("struct")
				|| !Before || !After || Before->Type != EJson::Object || After->Type != EJson::Object)
				return After;
			const TSharedPtr<FJsonObject>* OldFields = nullptr;
			const TSharedPtr<FJsonObject>* NewFields = nullptr;
			if (!Before->AsObject()->TryGetObjectField(TEXT("fields"), OldFields)
				|| !After->AsObject()->TryGetObjectField(TEXT("fields"), NewFields)) return After;
			const auto Fields = MakeShared<FJsonObject>();
			for (const auto& Field : (*NewFields)->Values)
			{
				const auto Old = (*OldFields)->TryGetField(Field.Key);
				const auto* Member = DefinitionField(DefinitionField(Definition, TEXT("fields")), Field.Key);
				CanonicalJson::FValueComparison Compared;
				if (Old && !TryCompareWorkspaceValues(Model, nullptr, Member, Old, Field.Value, Compared, OutError)) return nullptr;
				if (Old && Compared.bEquivalent) continue;
				auto Edited = ProjectEditedStructMembers(Old, Field.Value, Model, Member, nullptr, OutError);
				if (!Edited) return nullptr;
				Fields->SetField(Field.Key, Edited);
			}
			const auto Value = MakeShared<FJsonObject>();
			Value->Values = After->AsObject()->Values;
			Value->SetObjectField(TEXT("fields"), Fields);
			return MakeShared<FJsonValueObject>(Value);
		}

		bool ProjectPropertyEditValue(const FChangeDraft& Change, const FSemanticUnit& Unit,
			const FAuthoringModel& Model, FJsonObject& Descriptor, FSemanticWorkspaceImportError& OutError)
		{
			if (Change.GetIntent() != EChangeIntent::Update || !Change.GetExpectedBefore()) return true;
			TSharedPtr<FJsonValue> Before;
			UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryToJsonValue(*Change.GetExpectedBefore(), Before, Error))
			{
				OutError = ImportFailure(TEXT("hjson_import_property_baseline_invalid"), Error, TEXT("The captured property baseline could not be projected to an authoring value."));
				return false;
			}
			const FUeAgentCanonicalValue* Type = nullptr;
			const auto* Definition = PropertyValueDefinition(Unit, Model, &Type);
			FWorkspaceError ProjectionError;
			const auto Edited = ProjectEditedStructMembers(
				Before, Descriptor.TryGetField(TEXT("value")), Model, Definition, Type, ProjectionError);
			if (!Edited)
			{
				OutError = ProjectionError.GetDiagnostic();
				return false;
			}
			Descriptor.SetField(TEXT("value"), Edited);
			return true;
		}

		bool TryProjectRootObjectPropertyChangesInternal(
			const FPreparedAuthoringImport& Prepared,
			const FString& ExpectedRootOwnerPath,
			const bool bAllowStructuralChanges,
			const bool bRequirePropertyChanges,
			TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges,
			FSemanticWorkspaceImportError& OutError)
		{
			OutPropertyChanges.Reset();
			OutError.Reset();
			FString RootOwnerPath = ExpectedRootOwnerPath;
			RootOwnerPath.TrimStartAndEndInline();
			if (!Prepared.IsValid())
			{
				OutError = Prepared.GetValidationDiagnostic();
				return false;
			}
			if (RootOwnerPath.IsEmpty())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_root_property_request_invalid"),
					TEXT("The root property import requires a native owner path."), TEXT("hjson_import"));
				OutError.PropertyPath = TEXT("/root_owner_path");
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			if (!bAllowStructuralChanges && !Prepared.GetStructuralEdits().IsEmpty())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_root_property_structural_edits_unsupported"),
					TEXT("This property import accepts property updates but does not support structural edits."), TEXT("hjson_import"));
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Prepared.GetStructuralEdits().Num());
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Projected;
			for (const FChangeDraft& Change : Prepared.GetChangeSetDraft().GetChanges())
			{
				const FUeAgentCanonicalIdentity& Identity = Change.GetIdentity();
				const TMap<FString, FString>& Selectors = Identity.GetSelectors();
				const FString* PropertyPath = Selectors.Find(TEXT("property_path"));
				if (!PropertyPath || PropertyPath->IsEmpty())
				{
					if (bAllowStructuralChanges)
					{
						continue;
					}
					OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_root_property_path_missing"),
						TEXT("The prepared property change has no property path."), TEXT("hjson_import"), Identity.GetCanonicalKey());
					OutError.TargetPath = Identity.GetSelector(TEXT("asset_path"));
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				if (Change.GetIntent() != EChangeIntent::Update)
				{
					OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_root_property_intent_unsupported"),
						TEXT("A root property change must use the update intent."), TEXT("hjson_import"), Identity.GetCanonicalKey());
					OutError.PropertyPath = *PropertyPath;
					OutError.TargetPath = Identity.GetSelector(TEXT("asset_path"));
					OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Change.GetIntent()));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("update"));
					return false;
				}
				if (Selectors.FindRef(TEXT("owner_path")) != RootOwnerPath)
				{
					if (bAllowStructuralChanges)
					{
						continue;
					}
					OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_root_property_owner_mismatch"),
						TEXT("The property change belongs to a different native owner."), TEXT("hjson_import"), Identity.GetCanonicalKey());
					OutError.PropertyPath = *PropertyPath;
					OutError.TargetPath = Identity.GetSelector(TEXT("asset_path"));
					OutError.InternalContext += TEXT(" expected=") + RootOwnerPath + TEXT(" actual=") + Selectors.FindRef(TEXT("owner_path"));
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				const FString NotificationOwnerPath =
					Selectors.FindRef(TEXT("notification_owner_path"));
				if (!NotificationOwnerPath.IsEmpty() && NotificationOwnerPath != RootOwnerPath)
				{
					OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_root_property_notification_owner_mismatch"),
						TEXT("The property change's notification owner differs from its native root owner."), TEXT("hjson_import"), Identity.GetCanonicalKey());
					OutError.PropertyPath = *PropertyPath;
					OutError.TargetPath = Identity.GetSelector(TEXT("asset_path"));
					OutError.InternalContext += TEXT(" expected=") + RootOwnerPath + TEXT(" actual=") + NotificationOwnerPath;
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				TSharedPtr<FJsonValue> JsonValue;
				UE::UAI::Contract::FDiagnostic ValueError;
				if (!Change.GetProposedAfter().IsValid()
					|| !CanonicalJson::TryToJsonValue(
						*Change.GetProposedAfter(), JsonValue, ValueError)
					|| !JsonValue.IsValid())
				{
					OutError = ImportFailure(TEXT("hjson_import_root_property_value_invalid"), ValueError, TEXT("The root property change requires a valid proposed value."));
				OutError.SubjectId = Identity.GetCanonicalKey() ;
					return false;
				}
				TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
				Descriptor->SetStringField(TEXT("property_path"), *PropertyPath);
				Descriptor->SetField(TEXT("value"), MoveTemp(JsonValue));
				const FSemanticUnit* PropertyUnit = FindPropertyUnitForIdentity(
					Prepared.GetWorkspace()->GetModel(), Identity.GetCanonicalKey(),
					Identity.GetSelector(CanonicalAuthoringTargetIdentitySelector));
				if (PropertyUnit)
					AttachSemanticPropertySource(Prepared.GetWorkspace(), PropertyUnit->GetTargetIdentityKey(), *Descriptor,
						Identity.GetSelector(TEXT("asset_path")));
				if (PropertyUnit == nullptr
					|| !TryAppendPropertyTypeAssertions(*PropertyUnit,
						Prepared.GetWorkspace()->GetModel(), Descriptor, OutError, Prepared.GetWorkspace()))
				{
					if (!OutError.IsValid())
					{
						OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_property_type_missing"),
							TEXT("The prepared property change has no matching property unit and type definition."), TEXT("hjson_import"), Identity.GetCanonicalKey());
						OutError.PropertyPath = *PropertyPath;
						OutError.TargetPath = Identity.GetSelector(TEXT("asset_path"));
						OutError.SourceKind = TEXT("native_result");
					}
					return false;
				}
				if (!ProjectPropertyEditValue(Change, *PropertyUnit,
					*Prepared.GetWorkspace()->GetModel(), *Descriptor, OutError)) return false;
				Projected.Add(MakeShared<FJsonValueObject>(Descriptor));
			}
			if (bRequirePropertyChanges && Projected.IsEmpty())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_root_property_changes_empty"),
					TEXT("This import requires at least one explicit root property update."), TEXT("hjson_import"));
				return false;
			}
			OutPropertyChanges = MoveTemp(Projected);
			return true;
		}
	}



	bool FSemanticWorkspaceChangeProjection::HasChanges() const
	{
		return !RootObjectProperties.IsEmpty()
			|| !PropertyChangesByUnitId.IsEmpty()
			|| !ChangedUnitIds.IsEmpty()
			|| !StructuralEdits.IsEmpty();
	}

	int32 FSemanticWorkspaceChangeProjection::GetPropertyChangeCount() const
	{
		return RootObjectProperties.Num() + PropertyChangesByUnitId.Num();
	}



	bool TryRestoreSemanticPropertyValue(const FSemanticUnit& Unit, const FAuthoringModel& Model,
		const FUeAgentCanonicalValueRef& Captured, const FUeAgentCanonicalValueRef& Proposed,
		FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& OutValue, FUeAgentStateSemanticError& OutError)
	{
		const FUeAgentCanonicalValue* ValueType = nullptr;
		const auto* ValueDefinition = PropertyValueDefinition(Unit, Model, &ValueType);
		return TryOverlayCompactValue(Captured, Proposed, Model, Resources, OutValue, OutError,
			ValueDefinition, false, ValueType, false, ECompactEnvelopeScope::UnitValues);
	}

	FPreparedAuthoringImport::FPreparedAuthoringImport(
		FResolvedSemanticWorkspaceRef InWorkspace,
		FChangeSetDraft InChangeSetDraft,
		FPlanningRequest InPlanningRequest,
		FPlanningResult InPreflightResult,
		FAuthoringModelPtr InInputBaseline,
		TMap<FString, FUeAgentCanonicalValueRef> InPropertyBaselines)
		: Workspace(MoveTemp(InWorkspace))
		, InputBaseline(MoveTemp(InInputBaseline))
		, PropertyBaselines(MoveTemp(InPropertyBaselines))
		, ChangeSetDraft(MoveTemp(InChangeSetDraft))
		, PlanningRequest(MoveTemp(InPlanningRequest))
		, PreflightResult(MoveTemp(InPreflightResult))
	{
		const auto& Changes = ChangeSetDraft.GetChanges();
		for (int32 Index = 0; Index < Changes.Num(); ++Index)
		{
			const auto& Identity = Changes[Index].GetIdentity();
			for (const FString& Key : {Identity.GetCanonicalKey(),
				Identity.GetSelector(CanonicalAuthoringTargetIdentitySelector)})
				if (!FirstChangeByTargetIdentity.Contains(Key)) FirstChangeByTargetIdentity.Add(Key, Index);
		}
		if (!Workspace->IsValid()) ValidationDiagnostic = Workspace->GetValidationDiagnostic();
		else if (!ChangeSetDraft.IsValid()) ValidationDiagnostic = ChangeSetDraft.GetValidationDiagnostic();
		else if (!PlanningRequest.IsValid()) ValidationDiagnostic = PlanningRequest.GetValidationDiagnostic();
		else if (!PreflightResult.GetPlan().IsValid())
		{
			ValidationDiagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_import_execution_plan_missing"),
				TEXT("The prepared import requires an execution plan from preflight."), TEXT("hjson_import"));
			ValidationDiagnostic.PropertyPath = TEXT("/execution_plan");
			ValidationDiagnostic.SourceKind = TEXT("native_result");
			ValidationDiagnostic.ActualValuePresent = false;
		}
	}

	const FResolvedSemanticWorkspaceRef&
	FPreparedAuthoringImport::GetWorkspace() const
	{
		return Workspace;
	}

	const FChangeSetDraft&
	FPreparedAuthoringImport::GetChangeSetDraft() const
	{
		return ChangeSetDraft;
	}

	const PlanningPreflight::FChangeDraft* FPreparedAuthoringImport::FindChangeForTargetIdentity(const FString& Identity) const
	{
		const int32* Index = FirstChangeByTargetIdentity.Find(Identity);
		return Index ? &ChangeSetDraft.GetChanges()[*Index] : nullptr;
	}

	const FPlanningRequest&
	FPreparedAuthoringImport::GetPlanningRequest() const
	{
		return PlanningRequest;
	}

	const FPlanningResult&
	FPreparedAuthoringImport::GetPreflightResult() const
	{
		return PreflightResult;
	}

	const TArray<FStructuralEdit>&
	FPreparedAuthoringImport::GetStructuralEdits() const
	{
		return Workspace->GetStructuralEdits();
	}

	bool FPreparedAuthoringImport::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	FPreparedAuthoringImportPtr FPreparedAuthoringImport::SelectChanges(
		TFunctionRef<bool(const PlanningPreflight::FChangeDraft&)> Include) const
	{
		TArray<PlanningPreflight::FChangeDraft> Selected;
		for (const auto& Change : ChangeSetDraft.GetChanges())
			if (Include(Change)) Selected.Add(Change);
		FChangeSetDraft Draft(ChangeSetDraft.GetDescriptorIdentity(), ChangeSetDraft.GetRequestId(),
			ChangeSetDraft.GetIdempotencyKey(), ChangeSetDraft.GetSnapshot(), MoveTemp(Selected));
		return MakeShared<FPreparedAuthoringImport>(Workspace, MoveTemp(Draft), PlanningRequest, PreflightResult,
			InputBaseline, PropertyBaselines);
	}

	bool TryProjectInitialRootObjectProperties(const UeAgentAuthoringState::FStateSnapshot& NativeDefaults,
		const FString& ExpectedRootOwnerPath,
		const FHjsonNode& Categories, FUeAgentResourceTracker& Resources,
		TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges, FSemanticWorkspaceImportError& OutError)
	{
		OutPropertyChanges.Reset();
		OutError.Reset();
		if (Categories.GetKind() != EHjsonNodeKind::Object)
		{ OutError = InvalidPropertyCategories(Categories, Resources); return false; }
		if (ExpectedRootOwnerPath.IsEmpty())
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("hjson_initial_property_owner_missing"),
				TEXT("Initial property projection requires a native root owner."), TEXT("hjson_import"));
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		for (const auto& Entry : NativeDefaults.CopyEntries())
		{
			const auto& Selectors = Entry.TargetIdentity.Selectors;
			const FString Owner = Selectors.FindRef(TEXT("owner_path"));
			const FString NotificationOwner = Selectors.FindRef(TEXT("notification_owner_path"));
			if (!Owner.Equals(ExpectedRootOwnerPath, ESearchCase::CaseSensitive)
				|| (!NotificationOwner.IsEmpty() && !NotificationOwner.Equals(ExpectedRootOwnerPath, ESearchCase::CaseSensitive)))
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("hjson_initial_property_owner_mismatch"),
					Owner != ExpectedRootOwnerPath ? TEXT("The captured property belongs to a different native root owner.")
						: TEXT("The captured property's notification owner differs from its native root owner."), TEXT("hjson_import"),
					TEXT("expected=") + ExpectedRootOwnerPath + TEXT(" owner=") + Owner + TEXT(" notification_owner=") + NotificationOwner);
				OutError.PropertyPath = Selectors.FindRef(TEXT("property_path"));
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
		}
		return TryProjectReflectedPropertyValues(NativeDefaults, Categories, Resources, OutPropertyChanges, OutError);
	}

	bool TryProjectReflectedPropertyValues(const UeAgentAuthoringState::FStateSnapshot& NativeDefaults,
		const FHjsonNode& Categories, FUeAgentResourceTracker& Resources,
		TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges, FSemanticWorkspaceImportError& OutError)
	{
		OutPropertyChanges.Reset(); OutError.Reset();
		if (Categories.GetKind() != EHjsonNodeKind::Object)
		{ OutError = InvalidPropertyCategories(Categories, Resources); return false; }
		const FHjsonDocument SourceDocument(Categories.GetProvenance().GetSourceUnit(), MakeShared<FHjsonNode>(Categories));
		FAuthoringModelPtr Template;
		FWorkspaceError WorkspaceError;
		if (!TryComposeReflectedPropertyTemplate(NativeDefaults, Resources, Template, WorkspaceError))
		{ OutError = WorkspaceError.GetDiagnostic(); return false; }
		TArray<const FSemanticUnit*> Units;
		TArray<FReadablePropertySchema> Schema;
		for (const auto& Unit : Template->GetUnits())
		{
			if (Unit.GetUnitKind() != ESemanticUnitKind::PropertySet || Unit.GetFieldSetRef().IsEmpty()
				|| Unit.GetSemanticTypeRef().StartsWith(TEXT("uai.authoring.surface."))) continue;
			const auto Address = DescribeReadableProperty(Unit, *Template);
			Schema.Add({Address.Key, Address.Category, Address.Type}); Units.Add(&Unit);
		}
		FUeAgentCanonicalValuePtr CanonicalCategories;
		if (!TryProjectHjsonCanonicalValue(Categories, Resources, CanonicalCategories, WorkspaceError, false))
		{ OutError = WorkspaceError.GetDiagnostic(); return false; }
		TArray<FResolvedReadableProperty> ResolvedRows;
		FUeAgentStateSemanticError RowError;
		FWorkspaceError Diagnostics;
		if (!TryReadReadablePropertyRows(*CanonicalCategories, Schema, ResolvedRows, RowError))
		{
			auto RowsFailure = RowError;
			const FHjsonDocument Document(Categories.GetProvenance().GetSourceUnit(), MakeShared<FHjsonNode>(Categories));
			for (const auto& Item : RowsFailure.Diagnostics)
			{
				FString Pointer = Item.Source.JsonPath;
				const FHjsonNode* Node = Document.Find(Pointer);
				// A missing member uses the nearest existing authored parent.
				while (!Node && !Pointer.IsEmpty())
				{
					int32 Slash;
					if (!Pointer.FindLastChar(TEXT('/'), Slash)) break;
					Pointer.LeftInline(Slash); Node = Document.Find(Pointer);
				}
				const auto& Provenance = Node ? Node->GetProvenance() : Categories.GetProvenance();
				FWorkspaceError Located;
				Located.SetFailure(Item);
				Located.Locate(Provenance);
				Located.MarkUnchecked(TEXT("property_value_restore"), Provenance.GetSourceUnit(), TEXT("property_row_schema_invalid"), Provenance.GetLine());
				Diagnostics.Append(Located);
			}
		}
		TArray<TSharedPtr<FJsonValue>> Projected;
		for (const auto& Row : ResolvedRows)
		{
			const auto& Unit = *Units[Row.SchemaIndex];
			const auto& Address = Schema[Row.SchemaIndex];
			const auto* FieldValue = Categories.Get(Address.Category)->Get(Address.Name);
			OutError.Reset();
			FUeAgentStateSemanticError ValueError;
			FHjsonNodePtr ValueSources;
			auto ProjectRow = [&]() -> bool
			{
				TArray<FString> TemplateFields;
				Unit.GetValues().GetKeys(TemplateFields);
				TMap<FString, FUeAgentCanonicalValueRef> Values;
				if (!TryResolveKeyedPropertyValues(*FieldValue, Address.Type,
					TemplateFields, Resources, Values, WorkspaceError, nullptr, &ValueSources))
				{ OutError = WorkspaceError.GetDiagnostic(); return false; }
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows, DefaultRows;
				for (const auto& Value : Values) Rows.Add(Value);
				for (const auto& Value : Unit.GetValues()) DefaultRows.Add(Value);
				FUeAgentCanonicalValuePtr Raw, Defaults, Resolved, Overlay;
				bool bSkip = false;
				if (!FUeAgentCanonicalValue::TryObject(Rows, Resources, Raw, ValueError)
					|| !FUeAgentCanonicalValue::TryObject(DefaultRows, Resources, Defaults, ValueError))
				{ OutError = ValueError; return false; }
				if (!TryResolvePropertyValue(Unit.GetFieldSetRef(), Unit.GetUnitId(), Raw.ToSharedRef(), bSkip, Resolved, OutError)) return false;
				if (bSkip) return true;
				if (!TryRestoreSemanticPropertyValue(Unit, *Template, Defaults.ToSharedRef(), Resolved.ToSharedRef(), Resources, Overlay, ValueError))
				{ OutError = ValueError; return false; }
				TSharedPtr<FJsonValue> Json;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!CanonicalJson::TryToJsonValue(*Overlay, Json, JsonError))
				{ OutError = ImportFailure(TEXT("hjson_initial_property_value_invalid"), JsonError, TEXT("The initial property value could not be converted for native application.")); return false; }
				const auto Descriptor = MakeShared<FJsonObject>();
				Descriptor->SetStringField(TEXT("property_path"), Unit.GetFieldSetRef());
				Descriptor->SetField(TEXT("value"), Json);
				if (!TryAppendPropertyTypeAssertions(Unit, Template.ToSharedRef(), Descriptor, OutError)) return false;
				AttachInlinePropertySources(SourceDocument, TEXT("/") + EscapeJsonPointerSegment(Address.Category)
					+ TEXT("/") + EscapeJsonPointerSegment(Address.Name), *Descriptor, TEXT("value"));
				Projected.Add(MakeShared<FJsonValueObject>(Descriptor));
				return true;
			};
			if (!ProjectRow())
			{
				FWorkspaceError Error;
				FSourceProvenance Source = FieldValue->GetProvenance();
				auto Diagnostic = OutError;
				if (!ValueError.Source.JsonPath.IsEmpty())
				{
					// Keep the semantic path separately from the authored file path.
					// Native envelopes need not match the public projection; use a leaf
					// location only when the existing document resolver finds that node.
					const FHjsonDocument ValueDocument(Source.GetSourceUnit(),
						ValueSources.IsValid() ? ValueSources.ToSharedRef() : MakeShared<FHjsonNode>(*FieldValue));
					if (const FHjsonNode* FailedValue = ValueDocument.Find(ValueError.Source.JsonPath))
						Source = FailedValue->GetProvenance();
					Diagnostic.SemanticPointer = ValueError.Source.JsonPath;
				}
				Error.SetFailure(MoveTemp(Diagnostic), Source.GetSourceUnit());
				Error.Locate(Source);
				Error.MarkUnchecked(TEXT("property_value"), FieldValue->GetProvenance().GetSourceUnit(),
					TEXT("property_conversion_failed"));
				Diagnostics.Append(Error);
			}
		}
		if (Diagnostics.HasErrors())
		{
			OutError = Diagnostics.GetDiagnostic();
			return false;
		}
		OutPropertyChanges = MoveTemp(Projected);
		return true;
	}

	bool TryProjectRootObjectPropertyChanges(
		const FPreparedAuthoringImport& Prepared,
		const FString& ExpectedRootOwnerPath,
		TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges,
		FSemanticWorkspaceImportError& OutError)
	{
		return TryProjectRootObjectPropertyChangesInternal(
			Prepared, ExpectedRootOwnerPath, false, true,
			OutPropertyChanges, OutError);
	}

	bool TryPrepareRootObjectPropertyChanges(
		const FResolvedSemanticWorkspaceRef& SourceWorkspace,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		const FUeAgentCallerContext& CallerContext,
		const FString& ExpectedRootOwnerPath,
		FUeAgentResourceTracker& Resources,
		TArray<TSharedPtr<FJsonValue>>& OutPropertyChanges,
		FSemanticWorkspaceImportError& OutError)
	{
		OutPropertyChanges.Reset();
		OutError.Reset();
		FResolvedSemanticWorkspaceRef Workspace = SourceWorkspace;
		if (!TryRebaseSemanticWorkspace(SourceWorkspace, CurrentModel,
			CurrentSnapshot->GetRevision(), Resources, Workspace, OutError)) return false;
		if (!Workspace->GetStructuralEdits().IsEmpty())
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_root_property_structural_edits_unsupported"),
				TEXT("This property import accepts property updates but does not support structural edits."), TEXT("hjson_import"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Workspace->GetStructuralEdits().Num());
			return false;
		}
		if (!TryValidateWorkspaceBindingForImport(
				Workspace, CurrentModel, CurrentSnapshot->GetRevision(),
				Workspace->GetSelectionFingerprint(),
				Workspace->GetClosureFingerprint(), OutError))
		{
			return false;
		}
		TOptional<FChangeSetDraft> Draft;
		if (!TryBuildSemanticChangeSetDraft(Workspace, CurrentSnapshot,
				MoveTemp(DescriptorIdentity), MoveTemp(RequestId), MoveTemp(IdempotencyKey),
				Resources, Draft, OutError, CurrentModel)
			|| !Draft.IsSet())
		{
			return false;
		}

		FPreparedAuthoringImportPtr Prepared;
		if (!TryPrepareResolvedAuthoringImport(Workspace, CurrentModel, CurrentSnapshot,
				Draft.GetValue(), CallerContext, false, Workspace->GetSelectionFingerprint(),
				Workspace->GetClosureFingerprint(), Resources, Prepared, OutError)
			|| !Prepared.IsValid())
		{
			return false;
		}
		return TryProjectRootObjectPropertyChanges(
			*Prepared, ExpectedRootOwnerPath, OutPropertyChanges, OutError);
	}

	bool TryPrepareSemanticWorkspaceChanges(
		const FResolvedSemanticWorkspaceRef& SourceWorkspace,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		const FUeAgentCallerContext& CallerContext,
		const FString& ExpectedRootOwnerPath,
		FUeAgentResourceTracker& Resources,
		FSemanticWorkspaceChangeProjection& OutProjection,
		FPreparedAuthoringImportPtr& OutPrepared,
		FSemanticWorkspaceImportError& OutError)
	{
		OutProjection = FSemanticWorkspaceChangeProjection();
		const double PrepareStart = FPlatformTime::Seconds();
		OutPrepared.Reset();
		OutError.Reset();
		FResolvedSemanticWorkspaceRef Workspace = SourceWorkspace;
		if (!TryRebaseSemanticWorkspace(SourceWorkspace, CurrentModel,
			CurrentSnapshot->GetRevision(), Resources, Workspace, OutError)) return false;
		if (!TryValidateWorkspaceBindingForImport(
				Workspace, CurrentModel, CurrentSnapshot->GetRevision(),
				Workspace->GetSelectionFingerprint(),
				Workspace->GetClosureFingerprint(), OutError))
		{
			return false;
		}
		TOptional<FChangeSetDraft> Draft;
		if (!TryBuildSemanticChangeSetDraft(
				Workspace, CurrentSnapshot, MoveTemp(DescriptorIdentity),
				MoveTemp(RequestId), MoveTemp(IdempotencyKey), Resources,
				Draft, OutError, CurrentModel, &OutProjection.DocumentSourceBindings)
			|| !Draft.IsSet())
		{
			return false;
		}

		FPreparedAuthoringImportPtr Prepared;
		const double DraftFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=change_set units=%d changes=%d total_s=%.6f"),
			Workspace->GetModel()->GetUnits().Num(), Draft->GetChanges().Num(), DraftFinished - PrepareStart);
		if (!TryPrepareResolvedAuthoringImport(
				Workspace, CurrentModel, CurrentSnapshot, Draft.GetValue(),
				CallerContext, false, Workspace->GetSelectionFingerprint(),
				Workspace->GetClosureFingerprint(), Resources,
				Prepared, OutError)
			|| !Prepared.IsValid())
		{
			return false;
		}
		if (!TryProjectRootObjectPropertyChangesInternal(
				*Prepared, ExpectedRootOwnerPath, true, false,
				OutProjection.RootObjectProperties, OutError))
		{
			return false;
		}

		TSet<FString> ChangedUnitIds;
		const double PlanningFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=planning changes=%d total_s=%.6f"),
			Prepared->GetChangeSetDraft().GetChanges().Num(), PlanningFinished - DraftFinished);
		// Edited files select input; only prepared semantic and structural changes
		// describe mutations. Equivalent numeric spellings must not reach consumers
		// as property changes without a corresponding prepared descriptor.
		for (const FChangeDraft& Change : Prepared->GetChangeSetDraft().GetChanges())
		{
			const FString IdentityKey = Change.GetIdentity().GetCanonicalKey();
			const FString AuthoringTargetIdentityKey =
				Change.GetIdentity().GetSelector(
					CanonicalAuthoringTargetIdentitySelector);
			auto MatchingUnits = FindIdentityUnits(Workspace->GetModel(), IdentityKey, AuthoringTargetIdentityKey);
			// Targetless keyed collections bind their document unit directly.
			if (Change.GetIdentity().GetTargetKind() == TEXT("authoring_keyed_item_desired_state"))
				if (const auto* Owner = Workspace->GetModel()->FindUnit(Change.GetIdentity().GetOwnerId()))
					MatchingUnits.AddUnique(Owner);
			MatchingUnits.Sort([](const FSemanticUnit& Left, const FSemanticUnit& Right) { return &Left < &Right; });
			for (const FSemanticUnit* MatchingUnit : MatchingUnits)
			{
				const FSemanticUnit& Unit = *MatchingUnit;
				ChangedUnitIds.Add(Unit.GetUnitId());
				const FString PropertyPath = Change.GetIdentity().GetSelector(TEXT("property_path"));
				if (!PropertyPath.IsEmpty() && Unit.GetUnitKind() == ESemanticUnitKind::PropertySet)
				{
					TSharedPtr<FJsonValue> JsonValue;
					UE::UAI::Contract::FDiagnostic ValueError;
					if (!Change.GetProposedAfter().IsValid()
						|| !CanonicalJson::TryToJsonValue(*Change.GetProposedAfter(), JsonValue, ValueError)
						|| !JsonValue.IsValid())
					{
						OutError = ImportFailure(TEXT("hjson_import_property_value_invalid"), ValueError, TEXT("The property change requires a valid proposed value."));
						OutError.SubjectId = Unit.GetUnitId();
						return false;
					}
					TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
					Descriptor->SetStringField(TEXT("property_path"), PropertyPath);
					Descriptor->SetField(TEXT("value"), MoveTemp(JsonValue));
					AttachSemanticPropertySource(Workspace, Unit.GetTargetIdentityKey(), *Descriptor,
						Change.GetIdentity().GetSelector(TEXT("asset_path")));
					if (!TryAppendPropertyTypeAssertions(Unit, Workspace->GetModel(), Descriptor, OutError, Workspace)) return false;
					if (!ProjectPropertyEditValue(Change, Unit, *Workspace->GetModel(), *Descriptor, OutError)) return false;
					OutProjection.PropertyChangesByUnitId.Add(Unit.GetUnitId(), MakeShared<FJsonValueObject>(Descriptor));
				}
			}
		}
		OutProjection.StructuralEdits = Prepared->GetStructuralEdits();
		for (const FStructuralEdit& Edit : OutProjection.StructuralEdits)
		{
			for (const FString& UnitId : {
				Edit.GetOwnerUnitId(), Edit.GetSubjectUnitId(),
				Edit.GetReplacementUnitId(), Edit.GetRelationId()})
			{
				if (!UnitId.IsEmpty() && Workspace->GetModel()->FindUnit(UnitId))
				{
					ChangedUnitIds.Add(UnitId);
				}
			}
		}
		OutProjection.ChangedUnitIds.Reserve(ChangedUnitIds.Num());
		for (const FString& UnitId : ChangedUnitIds)
		{
			OutProjection.ChangedUnitIds.Add(UnitId);
		}
		OutProjection.ChangedUnitIds.Sort();
		if (!OutProjection.HasChanges())
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_no_explicit_changes"),
				TEXT("The workspace contains no explicit property or structural change; omitted and unchanged values preserve the current state."), TEXT("hjson_import"));
			return false;
		}
		OutPrepared = MoveTemp(Prepared);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=change_projection units=%d total_s=%.6f"),
			OutProjection.ChangedUnitIds.Num(), FPlatformTime::Seconds() - PlanningFinished);
		return true;
	}

	bool TryBuildSemanticChangeSetDraft(
		const FResolvedSemanticWorkspaceRef& SourceWorkspace,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FSemanticWorkspaceImportError& OutError,
		const FAuthoringModelPtr& CurrentModel,
		TArray<FDocumentValueSourceBinding>* OutDocumentSourceBindings)
	{
		const double DraftStart = FPlatformTime::Seconds();
		OutDraft.Reset();
		OutError.Reset();
		FResolvedSemanticWorkspaceRef Workspace = SourceWorkspace;
		if (CurrentModel && !TryRebaseSemanticWorkspace(SourceWorkspace, CurrentModel.ToSharedRef(),
			CurrentSnapshot->GetRevision(), Resources, Workspace, OutError)) return false;
		if (OutDocumentSourceBindings) OutDocumentSourceBindings->Reset();
		RequestId.TrimStartAndEndInline();
		IdempotencyKey.TrimStartAndEndInline();
		if (!Workspace->IsValid()) { OutError = Workspace->GetValidationDiagnostic(); return false; }
		if (!CurrentSnapshot->GetRevision().IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_import_change_set_request_invalid"),
				TEXT("The change-set request requires a valid persisted snapshot revision."), TEXT("hjson_import"));
			OutError.PropertyPath = TEXT("/snapshot/revision");
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		if (!DescriptorIdentity.IsValid())
		{
			OutError = DescriptorIdentity.GetValidationDiagnostic();
			OutError.PropertyPath = TEXT("/descriptor") + OutError.PropertyPath;
			return false;
		}
		if (RequestId.IsEmpty() || IdempotencyKey.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_import_change_set_request_invalid"),
				RequestId.IsEmpty() ? TEXT("The change-set request requires a non-empty request identifier.")
					: TEXT("The change-set request requires a non-empty idempotency key."), TEXT("hjson_import"));
			OutError.PropertyPath = RequestId.IsEmpty() ? TEXT("/request_id") : TEXT("/idempotency_key");
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		if (Workspace->GetBaseStateRevision() != CurrentSnapshot->GetRevision())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_import_stale_state_revision"), TEXT("workspace base revision differs from the "
					 "independent persisted snapshot"), TEXT("hjson_import"), FString());
			return false;
		}

		TMap<FString, TArray<const FUeAgentStateEntry*>> EntriesBySemanticId;
		TMap<FString, TMap<uint32, TArray<const FUeAgentStateEntry*>>> EntriesBySemanticTarget;
		for (const FUeAgentStateEntry& Entry : CurrentSnapshot->GetEntries())
		{
			EntriesBySemanticId.FindOrAdd(Entry.GetIdentity().GetSemanticId()).Add(&Entry);
			auto& Targets = EntriesBySemanticTarget.FindOrAdd(Entry.GetIdentity().GetSemanticId());
			const uint32 IdentityHash = GetTypeHash(Entry.GetIdentity().GetCanonicalKey());
			Targets.FindOrAdd(IdentityHash).Add(&Entry);
			if (const auto* AuthoringIdentity = Entry.GetIdentity().GetSelectors().Find(CanonicalAuthoringTargetIdentitySelector))
			{
				const uint32 AuthoringHash = GetTypeHash(*AuthoringIdentity);
				// An entry belongs to each distinct bucket once. The matcher still
				// compares full identities, including any hash collisions.
				if (AuthoringHash != IdentityHash) Targets.FindOrAdd(AuthoringHash).Add(&Entry);
			}
		}
		int64 SemanticTargetLookups = 0;
		int64 SemanticTargetCandidates = 0;
		auto MatchTarget = [&](const FSemanticUnit& Unit, Contract::FDiagnostic& Error, bool bAllowMissing = false)
		{
			const auto* SemanticEntries = EntriesBySemanticId.Find(Unit.GetSemanticTypeRef());
			const auto* TargetIndex = EntriesBySemanticTarget.Find(Unit.GetSemanticTypeRef());
			const auto* Candidates = TargetIndex ? TargetIndex->Find(GetTypeHash(Unit.GetTargetIdentityKey())) : nullptr;
			const TArray<const FUeAgentStateEntry*> EmptyCandidates;
			++SemanticTargetLookups;
			SemanticTargetCandidates += Candidates ? Candidates->Num() : 0;
			return MatchSemanticTargetEntry(Candidates ? *Candidates : EmptyCandidates, Unit, Error,
				bAllowMissing, SemanticEntries ? SemanticEntries->Num() : 0);
		};

		TArray<FChangeDraft> Changes;
		const FAuthoringModelRef& Model = Workspace->GetModel();
		auto BindPropertySource = [&](const FUeAgentStateEntry& Entry,
			const FSemanticUnit& Unit, const FString& ValuePointer)
		{
			if (!OutDocumentSourceBindings) return;
			FDocumentValueSourceBinding Binding;
			Binding.DocumentIdentity = Entry.GetIdentity().GetCanonicalKey();
			Binding.UnitId = Unit.GetUnitId();
			Binding.UnitValuePointer = ValuePointer;
			OutDocumentSourceBindings->Add(MoveTemp(Binding));
		};
		TSet<FString> DocumentUnitIds;
		if (CurrentModel.IsValid())
		{
			TMap<FString, FUeAgentCanonicalValuePtr> Documents;
			FWorkspaceError ProjectionError;
			if (!TryProjectDocumentValueChanges(CurrentSnapshot, CurrentModel.ToSharedRef(), Model,
					Workspace->GetEditedUnitIds(), Resources, Documents, DocumentUnitIds, ProjectionError,
					&Workspace->GetResolvedKeyedOperationRoots(), OutDocumentSourceBindings,
					[&Workspace](const FString& UnitId, const FString& Pointer) {
						return Workspace->FindValueNode(UnitId, Pointer);
					}))
			{
				ProjectionError.MarkUnchecked(TEXT("document_value_projection"), FString(), TEXT("remaining_document_bindings"));
				ProjectionError.MarkUnchecked(TEXT("change_planning"), FString(), TEXT("invalid_document_values"));
				OutError = ProjectionError.GetDiagnostic();
				return false;
			}
			for (const FUeAgentStateEntry& Entry : CurrentSnapshot->GetEntries())
				if (const auto* Proposed = Documents.Find(Entry.GetIdentity().GetCanonicalKey()))
					Changes.Emplace(Entry.GetIdentity(), EChangeIntent::Update, Entry.GetValue(), *Proposed,
						TArray<FString>({TEXT("authoring.write")}),
						CurrentSnapshot->GetProvenance().GetEvidenceId(), TArray<FUeAgentCanonicalIdentity>());
		}
		TSet<FString> ExplicitEditedUnitIds;
		FWorkspaceError ValueDiagnostics;
		if (Workspace->GetEditedUnitIds().IsSet())
		{
			for (const FString& UnitId : Workspace->GetEditedUnitIds().GetValue())
			{
				ExplicitEditedUnitIds.Add(UnitId);
			}
		}
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			if (DocumentUnitIds.Contains(Unit.GetUnitId())) continue;
			if (Workspace->GetEditedUnitIds().IsSet() &&
				!ExplicitEditedUnitIds.Contains(Unit.GetUnitId()))
			{
				continue;
			}
			if (Unit.GetUnitKind() != ESemanticUnitKind::PropertySet)
			{
				if (Unit.GetTargetIdentityKey().IsEmpty())
				{
					TArray<FString> RootFields;
					for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
					{
						if (Unit.GetValues().Contains(Contract.RootField)) RootFields.Add(Contract.RootField);
					}
					if (ExplicitEditedUnitIds.Contains(Unit.GetUnitId())
						&& Unit.GetUnitKind() == ESemanticUnitKind::StructureState
						&& !RootFields.IsEmpty())
					{
						TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
						for (const FString& RootField : RootFields)
						{
							Rows.Emplace(
								RootField, Unit.GetValues().FindChecked(RootField));
						}
						FUeAgentCanonicalValuePtr Proposed;
						FUeAgentStateSemanticError SemanticError;
						if (!FUeAgentCanonicalValue::TryObject(
							Rows, Resources, Proposed, SemanticError))
						{
							OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(SemanticError.Code.IsEmpty()
									? TEXT("hjson_import_keyed_item_value_invalid")
									: SemanticError.Code, SemanticError, TEXT("hjson_import"));
                    if (OutError.Source.SourceUnit.IsEmpty()) OutError.Source.SourceUnit = Unit.GetUnitId();
							return false;
						}
						Changes.Emplace(
							FUeAgentCanonicalIdentity(
								TEXT("keyed-item-desired-state:") + Unit.GetUnitId(),
								TEXT("authoring_keyed_item_desired_state"),
								Unit.GetUnitId(), Unit.GetRole(),
								TEXT("authoring_structure"),
								CurrentSnapshot->GetRevision().GetGeneration()),
							EChangeIntent::Create, FUeAgentCanonicalValuePtr(), Proposed,
							TArray<FString>({TEXT("authoring.structure")}),
							CurrentSnapshot->GetProvenance().GetEvidenceId(),
							TArray<FUeAgentCanonicalIdentity>());
					}
					continue;
				}
				Contract::FDiagnostic MatchError;
				const FUeAgentStateEntry* Current = MatchTarget(Unit, MatchError, true);
				if (!MatchError.IsEmpty())
				{
					FWorkspaceError Located;
					Located.SetFailure(Contract::FDiagnostic::WrapFailure(
						TEXT("hjson_import_structure_target_ambiguous"), MatchError, TEXT("hjson_import")), Unit.GetUnitId());
					Located.LocateUnits(Workspace->GetUnitSources());
					OutError = Located.GetDiagnostic();
					return false;
				}
				if (!Current)
				{
					continue;
				}
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
				TArray<FString> ValueKeys;
				Unit.GetValues().GetKeys(ValueKeys);
				ValueKeys.Sort();
				for (const FString& Key : ValueKeys)
				{
					Rows.Emplace(Key, Unit.GetValues().FindChecked(Key));
				}
				FUeAgentCanonicalValuePtr Proposed;
				FUeAgentStateSemanticError SemanticError;
				if (!FUeAgentCanonicalValue::TryObject(
						Rows, Resources, Proposed, SemanticError)
					|| !Proposed.IsValid())
				{
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(SemanticError.Code.IsEmpty()
							? TEXT("hjson_import_structure_value_invalid")
							: SemanticError.Code, SemanticError, TEXT("hjson_import"));
                    if (OutError.Source.SourceUnit.IsEmpty()) OutError.Source.SourceUnit = Unit.GetUnitId();
					return false;
				}
				const FUeAgentCanonicalValue* Type = nullptr;
				const auto* Definition = PropertyValueDefinition(Unit, *Model, &Type);
				CanonicalJson::FValueComparison Compared;
				FWorkspaceError CompareError;
				if (!TryCompareWorkspaceValues(*Model, Type, Definition, Current->GetValue(), Proposed.ToSharedRef(), Compared, CompareError))
				{
					OutError = CompareError.GetDiagnostic();
					return false;
				}
				if (Compared.bEquivalent) continue;
				FUeAgentSemanticComparison Comparison;
				if (!TryCompareValues(
						Current->GetValue(), Proposed.ToSharedRef(),
						TEXT("/") + Unit.GetUnitId(), Resources,
						Comparison, SemanticError))
				{
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(SemanticError.Code.IsEmpty()
							? TEXT("hjson_import_structure_compare_failed")
							: SemanticError.Code, SemanticError, TEXT("hjson_import"));
                    if (OutError.Source.SourceUnit.IsEmpty()) OutError.Source.SourceUnit = Unit.GetUnitId();
					return false;
				}
				if (!Comparison.IsEquivalent())
				{
					if (!Current->GetDetails().IsEditable())
					{
						auto Failure = UE::UAI::StateSemantic::MakeComparisonDiagnostic(Comparison,
							TEXT("hjson_import_structure_not_editable"),
							TEXT("The changed structure is read-only; its authored differences cannot be applied."),
							TEXT("hjson_import"), TEXT("uai.authoring.diagnostic/1"));
						for (auto& Difference : Failure.Diagnostics)
						{
							const FString UnitRoot = TEXT("/") + Unit.GetUnitId();
							const FString Relative = Difference.Source.InternalJsonPath.StartsWith(UnitRoot)
								? Difference.Source.InternalJsonPath.Mid(UnitRoot.Len()) : Difference.Source.InternalJsonPath;
							const auto* Source = Workspace->FindValueSource(Unit.GetUnitId(), TEXT("/values") + Relative);
							if (!Source) Source = Workspace->FindValueSource(Unit.GetUnitId(), Relative);
							if (!Source) Source = Workspace->GetUnitSources().Find(Unit.GetUnitId());
							FWorkspaceError Error;
							Difference.Source.InternalJsonPath = TEXT("/values") + Relative;
							Error.SetFailure(MoveTemp(Difference), Unit.GetUnitId());
							if (Source) Error.Locate(*Source);
							Difference = Error.GetDiagnostic();
						}
						FWorkspaceError Error;
						Error.SetFailure(MoveTemp(Failure), Unit.GetUnitId());
						ValueDiagnostics.Append(Error);
						continue;
					}
					Changes.Emplace(
						Current->GetIdentity(), EChangeIntent::Update,
						Current->GetValue(), Proposed,
						TArray<FString>({TEXT("authoring.structure")}),
						CurrentSnapshot->GetProvenance().GetEvidenceId(),
						TArray<FUeAgentCanonicalIdentity>());
				}
				continue;
			}
			if (Unit.GetTargetIdentityKey().IsEmpty())
			{
				TArray<FString> FieldIds;
				Unit.GetValues().GetKeys(FieldIds);
				FieldIds.Sort();
				for (const FString& FieldId : FieldIds)
				{
					const FUeAgentCanonicalValueRef& RawValue =
						Unit.GetValues().FindChecked(FieldId);
					bool bSkip = false;
					FUeAgentCanonicalValuePtr Proposed;
					if (!TryResolvePropertyValue(
							FieldId, Unit.GetUnitId(), RawValue, bSkip, Proposed, OutError))
					{
						return false;
					}
					if (bSkip)
					{
						continue;
					}
					const TArray<const FUeAgentStateEntry*>* Candidates = EntriesBySemanticId.Find(FieldId);
					const TArray<const FUeAgentStateEntry*> EmptyCandidates;
					const FUeAgentStateEntry* Current = MatchPropertyEntry(
						Candidates ? *Candidates : EmptyCandidates, Unit, FieldId, Model, OutError);
					if (!Current)
					{
						FWorkspaceError Located;
						Located.SetFailure(OutError, Unit.GetUnitId());
						const FString Pointer = TEXT("/") + FieldId.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						if (const auto* Source = Workspace->FindValueSource(Unit.GetUnitId(), Pointer)) Located.Locate(*Source);
						else Located.LocateUnits(Workspace->GetUnitSources());
						OutError = Located.GetDiagnostic();
						return false;
					}
					FUeAgentSemanticComparison Comparison;
					FUeAgentStateSemanticError SemanticError;
					if (!TryCompareValues(Current->GetValue(), Proposed.ToSharedRef(),
							TEXT("/") + FieldId, Resources, Comparison, SemanticError))
					{
						OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(SemanticError.Code.IsEmpty() ? TEXT("hjson_import_value_compare_failed")
														 : SemanticError.Code, SemanticError, TEXT("hjson_import"));
                    if (OutError.Source.SourceUnit.IsEmpty()) OutError.Source.SourceUnit = Unit.GetUnitId();
						return false;
					}
					if (!Comparison.IsEquivalent())
					{
						BindPropertySource(*Current, Unit, TEXT("/")
							+ FieldId.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
						Changes.Emplace(Current->GetIdentity(), EChangeIntent::Update,
							Current->GetValue(), Proposed,
							TArray<FString>({TEXT("authoring.write")}),
							CurrentSnapshot->GetProvenance().GetEvidenceId(),
							TArray<FUeAgentCanonicalIdentity>());
					}
				}
				continue;
			}
			const FUeAgentStateEntry* Current = MatchTarget(Unit, OutError);
			if (!Current)
			{
				FWorkspaceError Located;
				Located.SetFailure(OutError, Unit.GetUnitId());
				// target_identity_key belongs to the unit row, not its values subtree.
				Located.LocateUnits(Workspace->GetUnitSources());
				OutError = Located.GetDiagnostic();
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
			TArray<FString> ValueKeys;
			Unit.GetValues().GetKeys(ValueKeys);
			ValueKeys.Sort();
			for (const FString& Key : ValueKeys)
			{
				Rows.Emplace(Key, Unit.GetValues().FindChecked(Key));
			}
			FUeAgentCanonicalValuePtr RawObject;
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryObject(Rows, Resources, RawObject, SemanticError) ||
				!RawObject.IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(SemanticError.Code.IsEmpty() ? TEXT("hjson_import_property_value_invalid")
												 : SemanticError.Code, SemanticError, TEXT("hjson_import"));
                    if (OutError.Source.SourceUnit.IsEmpty()) OutError.Source.SourceUnit = Unit.GetUnitId();
				return false;
			}
			bool bSkip = false;
			FUeAgentCanonicalValuePtr Proposed;
			// Composition stores non-object semantic values under the sole
			// "value" field. Restore that shape before property-presence handling;
			// object values (including native property envelopes) keep their fields.
			if (Current->GetValue()->GetKind() != EUeAgentValueKind::Object
				&& Unit.GetValues().Num() == 1)
			{
				if (const auto* Value = Unit.GetValues().Find(TEXT("value"))) RawObject = *Value;
			}
			if (!TryResolvePropertyValue(Current->GetIdentity().GetSemanticId(),
					Unit.GetUnitId(), RawObject.ToSharedRef(), bSkip, Proposed, OutError))
			{
				LocateSemanticPropertyDiagnostic(Workspace, Unit.GetUnitId(), OutError);
				return false;
			}
			if (bSkip)
			{
				continue;
			}
			FUeAgentCanonicalValuePtr RestoredProposed;
			FUeAgentCanonicalValuePtr AuthoringCurrent;
			FWorkspaceError CaptureProjectionError;
			// Restore omitted edits from the same editable capture as export, not
			// from hidden native members. ObjectApply preserves those members in
			// their native storage; the full snapshot still supplies ExpectedBefore.
			if (!TryProjectAuthoringCaptureValue(*Current, Resources, AuthoringCurrent, CaptureProjectionError))
			{
				CaptureProjectionError.MarkUnchecked(TEXT("property_value_restore"), FString(), TEXT("property_capture_projection_failed"));
				CaptureProjectionError.MarkUnchecked(TEXT("change_planning"), FString(), TEXT("invalid_property_capture"));
				OutError = CaptureProjectionError.GetDiagnostic();
				return false;
			}
			if (!TryRestoreSemanticPropertyValue(Unit, *Model, AuthoringCurrent.ToSharedRef(), Proposed.ToSharedRef(),
					Resources, RestoredProposed, SemanticError))
			{
				FWorkspaceError Error;
				if (SemanticError.IsEmpty()) SemanticError = Contract::FDiagnostic::Failure(TEXT("hjson_import_value_restore_failed"),
					TEXT("Property value restoration failed without reporting a diagnostic."), TEXT("hjson_import"));
				LocateSemanticPropertyDiagnostic(Workspace, Unit.GetUnitId(), SemanticError);
				Error.SetFailure(MoveTemp(SemanticError));
				Error.MarkUnchecked(TEXT("property_value"), Error.GetDiagnostic().Source.SourceFile,
					TEXT("property_conversion_failed"));
				OutError = Error.GetDiagnostic();
				ValueDiagnostics.Append(Error);
				continue;
			}
			Proposed = RestoredProposed;
			const FUeAgentCanonicalValue* Type = nullptr;
			const auto* Definition = PropertyValueDefinition(Unit, *Model, &Type);
			CanonicalJson::FValueComparison Compared;
			FWorkspaceError CompareError;
			if (!TryCompareWorkspaceValues(*Model, Type, Definition, AuthoringCurrent.ToSharedRef(),
				Proposed.ToSharedRef(), Compared, CompareError))
			{
				OutError = CompareError.GetDiagnostic();
				return false;
			}
			if (!Compared.bEquivalent)
			{
				if (!Current->GetDetails().IsEditable())
				{
					OutError = Contract::FDiagnostic::Failure(TEXT("hjson_import_property_not_editable"),
						TEXT("The authored edit changes a read-only property."), TEXT("hjson_import"), Unit.GetUnitId());
					Compared.ApplyTo(OutError);
					OutError.PropertyPath = Current->GetIdentity().GetSelector(TEXT("property_path"));
					OutError.TargetPath = Current->GetIdentity().GetSelector(TEXT("asset_path"));
					LocateSemanticPropertyDiagnostic(Workspace, Unit.GetUnitId(), OutError);
					return false;
				}
				// Property-routed documents use the same source-binding owner as
				// decomposed documents. Native selector keys are not capture identities.
				BindPropertySource(*Current, Unit,
					Current->GetValue()->GetKind() != EUeAgentValueKind::Object ? TEXT("/value") : FString());
				Changes.Emplace(Current->GetIdentity(), EChangeIntent::Update,
					Current->GetValue(), Proposed, TArray<FString>({TEXT("authoring.write")}),
					CurrentSnapshot->GetProvenance().GetEvidenceId(),
					TArray<FUeAgentCanonicalIdentity>());
			}
		}

		if (ValueDiagnostics.HasErrors())
		{
			ValueDiagnostics.MarkUnchecked(TEXT("change_planning"), FString(), TEXT("invalid_authoring_values"));
			OutError = ValueDiagnostics.GetDiagnostic();
			return false;
		}

		for (const FStructuralEdit& Edit : Workspace->GetStructuralEdits())
		{
			FUeAgentCanonicalValuePtr Proposed;
			if (!TryStructuralEditValue(Edit, Resources, Proposed, OutError))
			{
				return false;
			}
			Changes.Emplace(
				FUeAgentCanonicalIdentity(
					TEXT("structural-edit:") + Edit.GetEditId(),
					TEXT("authoring_structural_edit"),
					Edit.GetOwnerUnitId(),
					LexToString(Edit.GetKind()),
					TEXT("authoring_structure"),
					CurrentSnapshot->GetRevision().GetGeneration()),
				EChangeIntent::Create,
				FUeAgentCanonicalValuePtr(),
				Proposed,
				TArray<FString>({TEXT("authoring.structure")}),
				CurrentSnapshot->GetProvenance().GetEvidenceId(),
				TArray<FUeAgentCanonicalIdentity>());
		}
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=semantic_draft units=%d changes=%d target_lookups=%lld target_candidates=%lld total_s=%.6f"),
			Model->GetUnits().Num(), Changes.Num(), SemanticTargetLookups, SemanticTargetCandidates, FPlatformTime::Seconds() - DraftStart);
		if (Changes.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_import_no_explicit_changes"), TEXT(
					"workspace omission and unchanged values are "
					"not mutations"), TEXT("hjson_import"), FString());
			return false;
		}
		Changes.Sort(
			[](const FChangeDraft& Left, const FChangeDraft& Right)
			{
				return Left.GetIdentity().GetCanonicalKey()
					< Right.GetIdentity().GetCanonicalKey();
			});
		OutDraft.Emplace(
			MoveTemp(DescriptorIdentity),
			MoveTemp(RequestId),
			MoveTemp(IdempotencyKey),
			CurrentSnapshot,
			MoveTemp(Changes));
		if (!OutDraft->IsValid())
		{
			OutError = OutDraft->GetValidationDiagnostic();
			OutDraft.Reset();
			return false;
		}
		return true;
	}

	bool TryPrepareResolvedAuthoringImport(
		const FResolvedSemanticWorkspaceRef& SourceWorkspace,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		const FChangeSetDraft& ChangeSetDraft,
		const FUeAgentCallerContext& CallerContext,
		bool bDryRun,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FUeAgentResourceTracker& Resources,
		FPreparedAuthoringImportPtr& OutPrepared,
		FSemanticWorkspaceImportError& OutError)
	{
		OutPrepared.Reset();
		OutError.Reset();
		FResolvedSemanticWorkspaceRef Workspace = SourceWorkspace;
		if (!TryRebaseSemanticWorkspace(SourceWorkspace, CurrentModel,
			CurrentSnapshot->GetRevision(), Resources, Workspace, OutError)) return false;
		if (!ChangeSetDraft.IsValid())
		{
			OutError = ChangeSetDraft.GetValidationDiagnostic();
			return false;
		}
		if (!(ChangeSetDraft.GetSnapshot()
				->GetRevision()
				== CurrentSnapshot->GetRevision()))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_import_change_set_snapshot_mismatch"), TEXT("The draft must bind the current persisted snapshot."), TEXT("hjson_import"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("/snapshot/revision");
			const auto RevisionEvidence = [](const FUeAgentStateRevision& Revision)
			{
				const auto Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("source_id"), Revision.GetSourceId());
				Value->SetStringField(TEXT("generation"), Revision.GetGeneration());
				Value->SetStringField(TEXT("sequence"), FString::Printf(TEXT("%lld"), Revision.GetSequence()));
				return MakeShared<FJsonValueObject>(Value);
			};
			OutError.ActualEvidence = RevisionEvidence(ChangeSetDraft.GetSnapshot()->GetRevision());
			OutError.ExpectedEvidence = RevisionEvidence(CurrentSnapshot->GetRevision());
			return false;
		}

		if (!TryValidateWorkspaceBindingForImport(
				Workspace, CurrentModel, CurrentSnapshot->GetRevision(),
				ExpectedSelectionFingerprint, ExpectedClosureFingerprint,
				OutError))
		{
			return false;
		}

		FPlanningRequest PlanningRequest(
			ChangeSetDraft,
			CallerContext,
			bDryRun);
		FPlanningResult Result;
		FPlanningPreflightError PlanningError;
		if (!TryPlanChanges(
			PlanningRequest,
			Resources,
			Result,
			PlanningError))
		{
			OutError = PlanningError;
			return false;
		}
		TMap<FString, FUeAgentCanonicalValueRef> PropertyBaselines;
		TMap<uint32, TArray<const FUeAgentStateEntry*>> EntriesByIdentityHash;
		for (const auto& Entry : CurrentSnapshot->GetEntries())
			EntriesByIdentityHash.FindOrAdd(GetTypeHash(Entry.GetIdentity().GetCanonicalKey())).Add(&Entry);
		for (const auto& Change : ChangeSetDraft.GetChanges())
		{
			if (Change.GetIdentity().GetSelector(TEXT("property_path")).IsEmpty()) continue;
			const FString& IdentityKey = Change.GetIdentity().GetCanonicalKey();
			const FUeAgentStateEntry* Entry = nullptr;
			if (const auto* Candidates = EntriesByIdentityHash.Find(GetTypeHash(IdentityKey)))
				for (const auto* Candidate : *Candidates)
					if (Candidate->GetIdentity().GetCanonicalKey() == IdentityKey) { Entry = Candidate; break; }
			if (!Entry) continue;
			FUeAgentCanonicalValuePtr Value;
			FWorkspaceError ProjectionError;
			if (!TryProjectAuthoringCaptureValue(*Entry, Resources, Value, ProjectionError) || !Value)
			{
				OutError = ImportFailure(TEXT("hjson_import_property_baseline_invalid"), ProjectionError.GetDiagnostic(), TEXT("The captured property baseline could not be projected to an authoring value."));
				return false;
			}
			PropertyBaselines.Add(Entry->GetIdentity().GetCanonicalKey(), Value.ToSharedRef());
		}
		OutPrepared = MakeShared<FPreparedAuthoringImport>(
			Workspace,
			ChangeSetDraft,
			MoveTemp(PlanningRequest),
			MoveTemp(Result), CurrentModel, MoveTemp(PropertyBaselines));
		if (!OutPrepared->IsValid())
		{
			OutError = OutPrepared->GetValidationDiagnostic();
			OutPrepared.Reset();
			return false;
		}
		return true;
	}

	bool TryPrepareSemanticWorkspaceImport(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		FUeAgentDescriptorIdentity DescriptorIdentity,
		FString RequestId,
		FString IdempotencyKey,
		const FUeAgentCallerContext& CallerContext,
		bool bDryRun,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FUeAgentResourceTracker& Resources,
		FPreparedAuthoringImportPtr& OutPrepared,
		FSemanticWorkspaceImportError& OutError)
	{
		OutPrepared.Reset();
		OutError.Reset();
		FResolvedSemanticWorkspacePtr Workspace;
		FWorkspaceError WorkspaceError;
		if (!TryResolveEditedSemanticWorkspace(
			Source,
			ExpectedMode,
			Resources,
			Workspace,
			WorkspaceError)
			|| !Workspace.IsValid())
		{
			OutError = ImportFailure(WorkspaceError.GetCode().IsEmpty()
					? TEXT("hjson_import_workspace_resolve_failed")
					: WorkspaceError.GetCode(), WorkspaceError.GetDiagnostic(), TEXT("Workspace resolution did not return a valid workspace or a diagnostic."));
			return false;
		}
		FResolvedSemanticWorkspaceRef Effective = Workspace.ToSharedRef();
		if (!TryRebaseSemanticWorkspace(Effective, CurrentModel, CurrentSnapshot->GetRevision(),
			Resources, Effective, OutError)) return false;
		Workspace = Effective;
		if (!TryValidateWorkspaceBindingForImport(
				Workspace.ToSharedRef(), CurrentModel,
				CurrentSnapshot->GetRevision(), ExpectedSelectionFingerprint,
				ExpectedClosureFingerprint, OutError))
		{
			return false;
		}
		TOptional<FChangeSetDraft> Draft;
		if (!TryBuildSemanticChangeSetDraft(
			Workspace.ToSharedRef(),
			CurrentSnapshot,
			MoveTemp(DescriptorIdentity),
			MoveTemp(RequestId),
			MoveTemp(IdempotencyKey),
			Resources,
			Draft,
			OutError, CurrentModel)
			|| !Draft.IsSet())
		{
			return false;
		}
		return TryPrepareResolvedAuthoringImport(
			Workspace.ToSharedRef(),
			CurrentModel,
			CurrentSnapshot,
			Draft.GetValue(),
			CallerContext,
			bDryRun,
			ExpectedSelectionFingerprint,
			ExpectedClosureFingerprint,
			Resources,
			OutPrepared,
			OutError);
	}
}
