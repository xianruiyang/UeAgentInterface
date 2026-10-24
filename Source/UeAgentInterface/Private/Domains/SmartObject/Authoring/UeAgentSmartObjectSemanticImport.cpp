#include "Domains/SmartObject/Authoring/UeAgentSmartObjectSemanticImport.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectDocument.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "UObject/StrongObjectPtr.h"
#include "SmartObjectDefinition.h"
#include "SmartObjectAnnotation.h"
#include "Annotations/SmartObjectSlotLinkAnnotation.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UeAgentSmartObjectAuthoring
{
	bool BuildDefinitionItemDefaults(USmartObjectDefinition& Context, const FString& Label,
		const FString& Kind, UStruct* Type, TSharedPtr<FJsonObject>& OutItem, UE::UAI::Contract::FDiagnostic& OutError,
		const TSharedPtr<FJsonObject>& InitialCategories, const UE::UAI::HjsonWorkspace::FHjsonNode* InitialCategorySource)
	{
		using namespace UE::UAI;
		auto Fail = [&](UE::UAI::Contract::FDiagnostic Diagnostic) { OutError = MoveTemp(Diagnostic); return false; };
		auto Reject = [&](const FString& Code, const FString& Reason, const FString& Field = FString(),
			const TSharedPtr<FJsonValue>& Actual = nullptr, const TSharedPtr<FJsonValue>& Expected = nullptr) {
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("smart_object_import"));
			Diagnostic.ActualValue = Actual; Diagnostic.ExpectedValue = Expected;
			if (Actual) Diagnostic.ActualValuePresent = true;
			Diagnostic.TargetPath = Context.GetPathName();
			Diagnostic.Source.InternalJsonPath = TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Field);
			return Fail(MoveTemp(Diagnostic));
		};
		if (Kind == TEXT("binding"))
		{
			if (InitialCategories || Type != FSmartObjectDefinitionPropertyBinding::StaticStruct())
				return Reject(TEXT("smart_object_binding_candidate_invalid"), InitialCategories
					? TEXT("A binding candidate does not accept initial property categories.") : TEXT("The selected type is not a SmartObject property binding."),
					InitialCategories ? TEXT("categories") : TEXT("candidate"), MakeShared<FJsonValueString>(GetPathNameSafe(Type)), MakeShared<FJsonValueString>(FSmartObjectDefinitionPropertyBinding::StaticStruct()->GetPathName()));
			OutItem = MakeShared<FJsonObject>();
			OutItem->SetStringField(TEXT("id"), TEXT("new_binding:") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
			OutItem->SetStringField(TEXT("kind"), Kind); OutItem->SetStringField(TEXT("label"), TEXT("Binding"));
			OutItem->SetStringField(TEXT("class_path"), Type->GetPathName());
			for (const TCHAR* Field : {TEXT("source"), TEXT("target"), TEXT("source_path"), TEXT("target_path")}) OutItem->SetStringField(Field, FString());
			return true;
		}
		TStrongObjectPtr<USmartObjectDefinition> Template(NewObject<USmartObjectDefinition>(GetTransientPackage()));
		Template->DebugAddSlot();
		auto& Slot = Template->GetMutableSlot(0);
		Slot.ID = FGuid::NewGuid(); Slot.Name = FName(*Label);
		Slot.SelectionPreconditions.SetSchemaClass(Context.GetWorldConditionSchemaClass());
		if (!InitializeConditionQuery(Slot.SelectionPreconditions, *Template,
			TEXT("slot.SelectionPreconditions"), OutError)) return false;
		FString Id = Slot.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
		if (Kind == TEXT("behavior"))
		{
			auto* Class = Cast<UClass>(Type);
			if (!Class || !Class->IsChildOf(USmartObjectBehaviorDefinition::StaticClass())
				|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				return Reject(TEXT("smart_object_behavior_candidate_invalid"),
					!Class || !Class->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()) ? TEXT("The selected type is not a SmartObject behavior class.")
						: Class->HasAnyClassFlags(CLASS_Abstract) ? TEXT("The selected behavior class is abstract.")
						: Class->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("The selected behavior class is deprecated.")
						: TEXT("The selected behavior class has been superseded by a newer version."),
					TEXT("candidate"), MakeShared<FJsonValueString>(GetPathNameSafe(Type)), MakeShared<FJsonValueString>(TEXT("concrete SmartObjectBehaviorDefinition class")));
			const FName ObjectName(*(TEXT("Behavior_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			Slot.BehaviorDefinitions.Add(NewObject<USmartObjectBehaviorDefinition>(Template.Get(), Class, ObjectName, RF_Transactional));
			Id = TEXT("behavior:") + Id + TEXT(":0");
		}
		else if (Kind == TEXT("definition_data"))
		{
			auto* Struct = Cast<UScriptStruct>(Type);
			if (!Struct || Struct == FSmartObjectDefinitionData::StaticStruct()
				|| !Struct->IsChildOf(FSmartObjectDefinitionData::StaticStruct()) || Struct->HasMetaData(TEXT("Hidden")))
				return Reject(TEXT("smart_object_data_candidate_invalid"),
					!Struct || !Struct->IsChildOf(FSmartObjectDefinitionData::StaticStruct()) ? TEXT("The selected type is not a SmartObject definition-data struct.")
						: Struct == FSmartObjectDefinitionData::StaticStruct() ? TEXT("The definition-data base struct cannot be instantiated as an item.")
						: TEXT("The selected definition-data struct is hidden from authoring."),
					TEXT("candidate"), MakeShared<FJsonValueString>(GetPathNameSafe(Type)), MakeShared<FJsonValueString>(TEXT("visible SmartObjectDefinitionData derived struct")));
			auto& Data = Slot.DefinitionData.AddDefaulted_GetRef(); Data.ID = FGuid::NewGuid();
			Data.Data.InitializeAsScriptStruct(Struct); Id = Data.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
		}
		else if (Kind != TEXT("slot") || Type != FSmartObjectSlotDefinition::StaticStruct())
			return Reject(TEXT("smart_object_candidate_kind_unsupported"), TEXT("The selected candidate kind and native type do not describe a SmartObject slot."), TEXT("candidate"), MakeShared<FJsonValueString>(Kind + TEXT(": ") + GetPathNameSafe(Type)), MakeShared<FJsonValueString>(FSmartObjectSlotDefinition::StaticStruct()->GetPathName()));
		TSharedPtr<FJsonValue> Document;
		if (!ReadDefinitionDocument(*Template, Document, OutError)) return false;
		OutItem = Document->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(Id);
		if (Kind == TEXT("slot")) OutItem->SetStringField(TEXT("label"), Label);
		if (!InitialCategories) return true;
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!AppendStructuralReadRequests(*Template, Template->GetPathName(), Requests, OutError)) return false;
		Requests.RemoveAll([&](const auto& Request) { return Request.SemanticId != ItemPropertySemanticId
			|| !Request.SnapshotIdentityOverride.IsSet() || Request.SnapshotIdentityOverride->GetSelector(TEXT("node_id")) != Id; });
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) return Fail(Capture.Diagnostic);
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		FString Text; HjsonWorkspace::FHjsonDocumentPtr Parsed; HjsonWorkspace::FWorkspaceError Error;
		if (!InitialCategorySource && (!CommandContract::Json::TryCanonicalize(InitialCategories, Text, OutError)
			|| !HjsonWorkspace::TryParseHjsonDocument(Text, Label, Resources, Parsed, Error))) return Fail(OutError.IsValid() ? OutError : Error.GetDiagnostic());
		TArray<TSharedPtr<FJsonValue>> Changes; Composition::HjsonImport::FSemanticWorkspaceImportError Failure;
		if (!Composition::HjsonImport::TryProjectReflectedPropertyValues(Capture.Snapshot,
			InitialCategorySource ? *InitialCategorySource : *Parsed->GetRoot(), Resources, Changes, Failure))
			return Fail(Failure);
		for (const auto& Change : Changes)
		{
			const FString Path = Change->AsObject()->GetStringField(TEXT("property_path"));
			const auto* Descriptor = OutItem->GetObjectField(TEXT("properties"))->GetArrayField(TEXT("properties")).FindByPredicate(
				[&](const auto& Value) { return Value->AsObject()->GetStringField(TEXT("property_path")) == Path; });
			if (!Descriptor) return Fail(UE::UAI::Contract::FDiagnostic::Failure(TEXT("smart_object_initial_property_missing"), TEXT("Initial property was not found: ") + Path, TEXT("smart_object_import")));
			(*Descriptor)->AsObject()->SetField(TEXT("value"), UeAgentAuthoringState::CloneCanonicalValue(Change->AsObject()->TryGetField(TEXT("value"))));
			if (const auto Source = Change->AsObject()->TryGetField(TEXT("authoring_source")))
				(*Descriptor)->AsObject()->SetField(TEXT("authoring_source"), UeAgentAuthoringState::CloneCanonicalValue(Source));
		}
		return true;
	}

	bool BuildDefinitionDocumentFromSemanticModel(USmartObjectDefinition& Definition,
		const TSharedRef<const UE::UAI::HjsonWorkspace::FAuthoringModel>& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::HjsonWorkspace::FWorkspaceError& Error)
	{
		using namespace UE::UAI;
		OutDocument.Reset(); Error.Reset();
		FString SourceUnitId, SourcePointer;
		auto Fail = [&](Contract::FDiagnostic Diagnostic, const FString& Field = FString()) {
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Definition.GetPathName();
			if (!Field.IsEmpty()) Diagnostic.PropertyPath = Field;
			if (Diagnostic.SourceKind == TEXT("native_result")) { Error.SetFailure(MoveTemp(Diagnostic), FString(), FString()); return false; }
			if (Diagnostic.Source.SourceFile.IsEmpty() && !Model->FindUnit(Diagnostic.Source.SourceUnit))
				Diagnostic.Source.SourceUnit = SourceUnitId;
			if (!Field.IsEmpty()) Diagnostic.SemanticPointer = SourcePointer + TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Field);
			else if (Diagnostic.SemanticPointer.IsEmpty() && Diagnostic.Source.JsonPath.IsEmpty()
				&& (Diagnostic.Source.InternalJsonPath.IsEmpty() || Diagnostic.Source.InternalJsonPath == TEXT("/")))
			{
				Diagnostic.SemanticPointer = SourcePointer;
				Diagnostic.Source.InternalJsonPath = SourcePointer;
				Diagnostic.SourceKind = TEXT("related_input");
			}
			Error.SetFailure(MoveTemp(Diagnostic), SourceUnitId, SourcePointer); return false;
		};

		auto Reject = [&](const FString& Code, const FString& Reason, const FString& Field = FString(),
			const TSharedPtr<FJsonValue>& Actual = nullptr, const TSharedPtr<FJsonValue>& Expected = nullptr) {
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("smart_object_import"));
			Diagnostic.ActualValue = Actual; Diagnostic.ExpectedValue = Expected;
			if (Actual) Diagnostic.ActualValuePresent = true;
			Diagnostic.TargetPath = Definition.GetPathName();
			if (!Field.IsEmpty()) Diagnostic.PropertyPath = Field;
			Diagnostic.SemanticPointer = Field.IsEmpty() ? SourcePointer
				: SourcePointer + TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Field);
			Diagnostic.Source.InternalJsonPath = Diagnostic.SemanticPointer;
			return Fail(MoveTemp(Diagnostic), Field);
		};
		UE::UAI::Contract::FDiagnostic Detail;
		TSharedPtr<FJsonValue> Captured;
		if (!ReadDefinitionDocument(Definition, Captured, Detail)) { Error.SetFailure(Detail); return false; }
		const auto Desired = UeAgentAuthoringState::CloneCanonicalValue(Captured)->AsObject();
		const auto Before = Captured->AsObject()->GetObjectField(TEXT("items"));
		const HjsonWorkspace::FSemanticUnit* Root = nullptr;
		for (const auto& Unit : Model->GetUnits())
			if (Unit.GetSemanticTypeRef() == DefinitionSemanticId)
			{
				if (Root) return Reject(TEXT("smart_object_semantic_root_ambiguous"), TEXT("The semantic model contains more than one SmartObject definition document."));
				Root = &Unit;
			}
		if (!Root) return Reject(TEXT("smart_object_semantic_root_missing"), TEXT("The semantic model does not contain a SmartObject definition document."));
		SourceUnitId = Root->GetUnitId(); SourcePointer = TEXT("/smart_object_items");
		const auto* CanonicalRows = Root->GetValues().Find(TEXT("smart_object_items"));
		TSharedPtr<FJsonValue> Rows;
		if (CanonicalRows && !StateSemantic::CanonicalJson::TryToJsonValue(CanonicalRows->Get(), Rows, Detail)) return Fail(Detail);
		if (!Rows.IsValid() || Rows->Type != EJson::Object) return Reject(TEXT("smart_object_semantic_items_invalid"), TEXT("SmartObject items must be an object."), FString(), Rows, MakeShared<FJsonValueString>(TEXT("object")));
		const auto Items = MakeShared<FJsonObject>();
		TMap<FString, FString> NewIds;
		TMap<FString, FString> SourceKeyByNativeId;
		auto SelectItemSource = [&](const FString& Id)
		{
			SourceUnitId = Root->GetUnitId();
			SourcePointer = SourceKeyByNativeId.Contains(Id)
				? TEXT("/smart_object_items/") + HjsonWorkspace::EscapeJsonPointerSegment(SourceKeyByNativeId.FindChecked(Id))
				: TEXT("/smart_object_items");
		};
		TSet<FString> NewItems;
		for (const auto& Row : Rows->AsObject()->Values)
		{
			SourcePointer = TEXT("/smart_object_items/") + HjsonWorkspace::EscapeJsonPointerSegment(Row.Key);
			if (!Row.Value.IsValid() || Row.Value->Type != EJson::Object) return Reject(TEXT("smart_object_semantic_item_invalid"), TEXT("Each SmartObject item must be an object."), FString(), Row.Value, MakeShared<FJsonValueString>(TEXT("object")));
			const auto Source = Row.Value->AsObject();
			FString Operation, Id; Source->TryGetStringField(TEXT("operation"), Operation); Source->TryGetStringField(TEXT("id"), Id);
			if (Operation == TEXT("remove"))
			{
				if (Id == TEXT("root") || Id == TEXT("parameters")) return Reject(TEXT("smart_object_root_item_required"), TEXT("The SmartObject root settings and parameters items must both be retained."));
				continue;
			}
			const TSharedPtr<FJsonObject>* Previous = nullptr; TSharedPtr<FJsonObject> Item;
			if (Operation == TEXT("create"))
			{
				const TSharedPtr<FJsonObject>* CandidateSpec = nullptr; HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
				Source->TryGetObjectField(TEXT("candidate"), CandidateSpec);
				if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(CandidateSpec ? *CandidateSpec : TSharedPtr<FJsonObject>(),
						SourcePointer + TEXT("/candidate"), Selector, Error)) return Fail(Error.GetDiagnostic());
				QueryOwners::AiBehavior::FAiBehaviorCandidate Candidate; QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError Failure;
				if (!QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(Definition, Selector.Name, {}, Candidate, Failure, Selector.Select))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Failure, SourcePointer + TEXT("/candidate"));
					return Fail(MoveTemp(Failure));
				}
				FString Kind = Candidate.Kind; Kind.RemoveFromStart(TEXT("smart_object_"));
				const TSharedPtr<FJsonObject>* Categories = nullptr;
				if (Source->HasField(TEXT("categories")) && !Source->TryGetObjectField(TEXT("categories"), Categories)) return Reject(TEXT("hjson_initial_categories_invalid"), TEXT("Initial property categories must be an object."), TEXT("categories"), Source->TryGetField(TEXT("categories")), MakeShared<FJsonValueString>(TEXT("object")));
				if (!Candidate.bSupported)
				{
					auto SupportFailure = Contract::FDiagnostic::WrapFailure(TEXT("smart_object_candidate_unsupported"), Candidate.SupportDiagnostic);
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(SupportFailure, SourcePointer + TEXT("/candidate"));
					return Fail(MoveTemp(SupportFailure));
				}
				if (!BuildDefinitionItemDefaults(Definition, Row.Key, Kind,
					LoadObject<UStruct>(nullptr, *Candidate.PayloadType), Item, Detail, Categories ? *Categories : nullptr,
					Prepared.GetWorkspace()->FindValueNode(Root->GetUnitId(), TEXT("/smart_object_items/")
						+ HjsonWorkspace::EscapeJsonPointerSegment(Row.Key) + TEXT("/categories")))) return Fail(Detail);
				Id = Kind == TEXT("behavior") ? TEXT("new_behavior:") + FGuid::NewGuid().ToString(EGuidFormats::Digits) : Item->GetStringField(TEXT("id"));
				Item->SetStringField(TEXT("id"), Id); if (Kind != TEXT("binding")) Item->SetStringField(TEXT("parent"), TEXT("root")); Item->SetNumberField(TEXT("index"), -1);
				if (Kind == TEXT("binding") && (!Source->HasField(TEXT("source")) || !Source->HasField(TEXT("target"))))
					return Reject(TEXT("smart_object_binding_endpoint_required"), TEXT("A SmartObject binding requires both source and target endpoints."), Source->HasField(TEXT("source")) ? TEXT("target") : TEXT("source"), nullptr, MakeShared<FJsonValueString>(TEXT("binding endpoint")));
				NewIds.Add(Row.Key, Id); NewItems.Add(Id);
			}
			else
			{
				if (Source->HasField(TEXT("candidate")) || Source->HasField(TEXT("categories"))) return Reject(TEXT("smart_object_retype_unsupported"), TEXT("Candidate selection and initial categories may only be supplied when creating a SmartObject item."), Source->HasField(TEXT("candidate")) ? TEXT("candidate") : TEXT("categories"));
				if (!Before->TryGetObjectField(Id, Previous)) return Reject(TEXT("smart_object_semantic_existing_item_missing"), TEXT("The edited SmartObject item is no longer present in the native definition."), FString(), MakeShared<FJsonValueString>(Row.Key));
				Item = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(*Previous))->AsObject();
			}
			if (Items->HasField(Id)) return Reject(TEXT("smart_object_semantic_identity_duplicate"), TEXT("The same native SmartObject item is addressed by more than one authored row."), FString(), MakeShared<FJsonValueString>(Row.Key));
			SourceKeyByNativeId.Add(Id, Row.Key);
			if (Source->HasField(TEXT("kind")) && Source->GetStringField(TEXT("kind")) != Item->GetStringField(TEXT("kind")))
				return Reject(TEXT("smart_object_kind_requires_candidate"), TEXT("An existing SmartObject item's kind cannot be changed directly; creation requires a candidate."), TEXT("kind"), Source->TryGetField(TEXT("kind")), Item->TryGetField(TEXT("kind")));
			if (Id == TEXT("root") || Id == TEXT("parameters"))
			{
				// These fixed owners have neither a movable parent nor array order.
				for (const TCHAR* Field : {TEXT("parent"), TEXT("index")})
					if (const auto Value = Source->TryGetField(Field))
						if (UeAgentAuthoringState::MakeStableCanonicalValueKey(Value)
							!= UeAgentAuthoringState::MakeStableCanonicalValueKey(Item->TryGetField(Field)))
							return Reject(TEXT("smart_object_fixed_owner_structure_read_only"), TEXT("The root settings and parameters items have a fixed parent and index."), Field, Value, Item->TryGetField(Field));
			}
			if (Item->GetStringField(TEXT("kind")) != TEXT("slot") && Source->HasField(TEXT("label"))
				&& Source->GetStringField(TEXT("label")) != Item->GetStringField(TEXT("label")))
				return Reject(TEXT("smart_object_item_label_read_only"), TEXT("Only SmartObject slot labels can be changed directly."), TEXT("label"), Source->TryGetField(TEXT("label")), Item->TryGetField(TEXT("label")));
			if (Item->GetStringField(TEXT("kind")) == TEXT("binding"))
				if (const auto Parent = Source->TryGetField(TEXT("parent")))
					if (UeAgentAuthoringState::MakeStableCanonicalValueKey(Parent)
						!= UeAgentAuthoringState::MakeStableCanonicalValueKey(Item->TryGetField(TEXT("parent"))))
						return Reject(TEXT("smart_object_binding_parent_unsupported"), TEXT("A SmartObject binding does not support changing its parent."), TEXT("parent"));
			for (const TCHAR* Field : {TEXT("source"), TEXT("target"), TEXT("source_path"), TEXT("target_path")})
				if (const auto Value = Source->TryGetField(Field))
				{
					if (Item->GetStringField(TEXT("kind")) != TEXT("binding")) return Reject(TEXT("smart_object_binding_field_owner_invalid"), TEXT("Binding endpoint and path fields are only valid on binding items."), Field, MakeShared<FJsonValueString>(Item->GetStringField(TEXT("kind"))), MakeShared<FJsonValueString>(TEXT("binding")));
					Item->SetField(Field, UeAgentAuthoringState::CloneCanonicalValue(Value));
				}
			for (const TCHAR* Field : {TEXT("label"), TEXT("parent"), TEXT("index")})
				if (const auto Value = Source->TryGetField(Field)) Item->SetField(Field, UeAgentAuthoringState::CloneCanonicalValue(Value));
			if (const auto LinkedSlot = Source->TryGetField(TEXT("linked_slot"))) {
				if (Item->GetStringField(TEXT("kind")) != TEXT("definition_data")) return Reject(TEXT("smart_object_slot_link_owner_invalid"), TEXT("The linked_slot field requires a SmartObject slot-link annotation."), TEXT("linked_slot"), MakeShared<FJsonValueString>(Item->GetStringField(TEXT("class_path"))), MakeShared<FJsonValueString>(FSmartObjectSlotLinkAnnotation::StaticStruct()->GetPathName()));
				const auto* Type = LoadObject<UScriptStruct>(nullptr, *Item->GetStringField(TEXT("class_path")));
				if (!Type || !Type->IsChildOf(FSmartObjectSlotLinkAnnotation::StaticStruct())) return Reject(TEXT("smart_object_slot_link_owner_invalid"), TEXT("The linked_slot field requires a SmartObject slot-link annotation."), TEXT("linked_slot"), MakeShared<FJsonValueString>(Item->GetStringField(TEXT("class_path"))), MakeShared<FJsonValueString>(FSmartObjectSlotLinkAnnotation::StaticStruct()->GetPathName()));
				Item->SetField(TEXT("linked_slot"), UeAgentAuthoringState::CloneCanonicalValue(LinkedSlot));
			}
			// Preserve parser provenance through canonical projection, cloning, and native apply.
			Prepared.GetWorkspace()->AttachInlinePropertySources(Root->GetUnitId(), SourcePointer, *Item, FString());
			Items->SetObjectField(Id, Item);
		}
		for (const auto& Pair : Items->Values)
			for (const TCHAR* Field : {TEXT("parent"), TEXT("source"), TEXT("target"), TEXT("linked_slot")})
			if (const auto Parent = Pair.Value->AsObject()->TryGetField(Field); Parent && Parent->Type == EJson::Object)
			{
				SelectItemSource(Pair.Key);
				FString Name;
				if (!Parent->AsObject()->TryGetStringField(TEXT("new_item"), Name) || !NewIds.Contains(Name)) return Reject(TEXT("smart_object_new_parent_unresolved"), TEXT("The new_item reference does not identify an item created by this import."), Field, Parent->AsObject()->TryGetField(TEXT("new_item")), MakeShared<FJsonValueString>(TEXT("created item key")));
				Pair.Value->AsObject()->SetStringField(Field, NewIds.FindChecked(Name));
			}
		for (const auto& Pair : Items->Values)
			if (const auto LinkedSlot = Pair.Value->AsObject()->TryGetField(TEXT("linked_slot")); LinkedSlot && LinkedSlot->IsNull())
				Pair.Value->AsObject()->SetStringField(TEXT("linked_slot"), FString());
		for (const auto& Pair : Projection.PropertyChangesByUnitId)
		{
			SourceUnitId = Pair.Key; SourcePointer = TEXT("/value");
			const auto* Unit = Model->GetUnits().FindByPredicate(
				[&](const auto& Candidate) { return Candidate.GetUnitId() == Pair.Key; });
			if (!Unit) return Reject(TEXT("smart_object_property_unit_missing"), TEXT("The projected property change has no matching semantic property unit."));
			const auto* Change = Prepared.GetChangeSetDraft().GetChanges().FindByPredicate(
				[&](const auto& Candidate) {
					return Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetCanonicalKey()
						|| Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetSelector(
							HjsonWorkspace::CanonicalAuthoringTargetIdentitySelector);
				});
			if (!Change || !Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				return Reject(TEXT("smart_object_property_change_invalid"), TEXT("The property unit must have a matching prepared change and object value."));
			const FString Owner = Change->GetIdentity().GetSelector(TEXT("node_id"));
			const FString Path = Change->GetIdentity().GetSelector(TEXT("property_path"));
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Items->TryGetObjectField(Owner, Item)) return Reject(TEXT("smart_object_property_owner_missing"), TEXT("The property's native owner is not present in the desired SmartObject definition."), FString(), MakeShared<FJsonValueString>(Path));
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!(*Item)->TryGetObjectField(TEXT("properties"), Properties)) return Reject(TEXT("smart_object_properties_missing"), TEXT("The selected SmartObject item has no native properties collection."), FString(), MakeShared<FJsonValueString>(Path));
			bool bFound = false;
			for (const auto& Descriptor : (*Properties)->GetArrayField(TEXT("properties")))
			{
				if (Descriptor->AsObject()->GetStringField(TEXT("property_path")) != Path) continue;
				const auto Value = Pair.Value->AsObject()->TryGetField(TEXT("value"));
				if (!Value.IsValid()) return Reject(TEXT("smart_object_property_value_missing"), TEXT("The projected SmartObject property edit has no value."), FString(), nullptr, MakeShared<FJsonValueString>(TEXT("property value for ") + Path));
				Descriptor->AsObject()->SetField(TEXT("value"), UeAgentAuthoringState::CloneCanonicalValue(Value));
				if (const auto Source = Pair.Value->AsObject()->TryGetField(TEXT("authoring_source")))
					Descriptor->AsObject()->SetField(TEXT("authoring_source"), UeAgentAuthoringState::CloneCanonicalValue(Source));
				bFound = true; break;
			}
			if (!bFound) return Reject(TEXT("smart_object_property_descriptor_missing"), TEXT("The requested property path does not exist in the selected SmartObject item."), FString(), MakeShared<FJsonValueString>(Path));
		}
		SourceUnitId = Root->GetUnitId(); SourcePointer = TEXT("/smart_object_items");
		if (!Items->HasField(TEXT("root")) || !Items->HasField(TEXT("parameters"))) return Reject(TEXT("smart_object_root_item_required"), TEXT("The SmartObject root settings and parameters items must both be retained."));
		if (!ResolveDefinitionRemovalContext(Before, Items, Detail)) return Fail(Detail);
		// Capture decoded creation arguments before ordinal/derived-ID compaction.
		if (!HjsonWorkspace::TryRecordKeyedCreationArguments(TEXT("smart_object_items"), Rows, Items, NewIds, Detail)) return Fail(Detail);
		TMap<FString, TArray<TSharedPtr<FJsonObject>>> Orders;
		for (const auto& Pair : Items->Values)
		{
			SelectItemSource(Pair.Key);
			const auto Item = Pair.Value->AsObject(); const FString Kind = Item->GetStringField(TEXT("kind"));
			if (Kind == TEXT("settings") || Kind == TEXT("parameters")) continue;
			FString Parent; Item->TryGetStringField(TEXT("parent"), Parent);
			if (Kind != TEXT("binding") && Parent != TEXT("root")
				&& (!Items->HasField(Parent) || Items->GetObjectField(Parent)->GetStringField(TEXT("kind")) != TEXT("slot")))
				return Reject(TEXT("smart_object_parent_invalid"), TEXT("The item's parent must identify the root settings or an existing slot."), TEXT("parent"), nullptr, MakeShared<FJsonValueString>(TEXT("root settings or an existing slot")));
			if (Kind == TEXT("slot") && Parent != TEXT("root")) return Reject(TEXT("smart_object_slot_parent_invalid"), TEXT("SmartObject slots must belong directly to the root settings."), TEXT("parent"), nullptr, MakeShared<FJsonValueString>(TEXT("root settings")));
			if (Kind == TEXT("definition_data") && Parent == TEXT("root")
				&& (NewItems.Contains(Pair.Key) || !Before->HasField(Pair.Key)
					|| Before->GetObjectField(Pair.Key)->GetStringField(TEXT("parent")) != Parent))
			{
				const auto* Type = LoadObject<UScriptStruct>(nullptr, *Item->GetStringField(TEXT("class_path")));
				if (Type && Type->IsChildOf(FSmartObjectSlotAnnotation::StaticStruct()))
					return Reject(TEXT("smart_object_slot_annotation_requires_slot_parent"), TEXT("A slot annotation must belong to a slot."), TEXT("parent"), nullptr, MakeShared<FJsonValueString>(TEXT("slot")));
			}
			Orders.FindOrAdd(Kind + TEXT(":") + Parent).Add(Item);
		}
		for (auto& Group : Orders)
		{
			int32 NextIndex = 0; TSet<int32> Occupied;
			for (const auto& Item : Group.Value)
			{
				SelectItemSource(Item->GetStringField(TEXT("id")));
				const FString ItemSourcePointer = SourcePointer;
				SourcePointer += TEXT("/index");
				int32 Index = 0;
				if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Item, TEXT("index"), Index, Detail))
				{ UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Detail, ItemSourcePointer); return Fail(Detail); }
				if (!Item->HasField(TEXT("index")) || Index >= MAX_int32
					|| (Index < 0 && (Index != -1 || !NewItems.Contains(Item->GetStringField(TEXT("id"))))))
					return Reject(TEXT("smart_object_item_index_invalid"), TEXT("An existing item index must be non-negative and below the maximum int32 value; only a new item may use -1."),
						FString(), Item->TryGetField(TEXT("index")), MakeShared<FJsonValueString>(
							NewItems.Contains(Item->GetStringField(TEXT("id"))) ? TEXT("integer from -1 to 2147483646") : TEXT("integer from 0 to 2147483646")));
				if (Index >= 0)
				{
					if (Occupied.Contains(static_cast<int32>(Index))) return Reject(TEXT("smart_object_item_index_duplicate"), TEXT("Two items of the same kind under the same parent use the same index."), FString(), MakeShared<FJsonValueNumber>(Index), MakeShared<FJsonValueString>(TEXT("unused sibling index")));
					Occupied.Add(static_cast<int32>(Index)); NextIndex = FMath::Max(NextIndex, static_cast<int32>(Index) + 1);
				}
			}
			for (const auto& Item : Group.Value) if (Item->GetIntegerField(TEXT("index")) < 0) Item->SetNumberField(TEXT("index"), NextIndex++);
			Group.Value.Sort([](const auto& A, const auto& B) { return A->GetIntegerField(TEXT("index")) < B->GetIntegerField(TEXT("index")); });
			for (int32 Index = 0; Index < Group.Value.Num(); ++Index) Group.Value[Index]->SetNumberField(TEXT("index"), Index);
		}
		const auto FinalItems = MakeShared<FJsonObject>();
		for (const auto& Pair : Items->Values)
		{
			SelectItemSource(Pair.Key);
			const auto Item = Pair.Value->AsObject(); FString Id = Pair.Key;
			if (Item->GetStringField(TEXT("kind")) == TEXT("behavior"))
				Id = TEXT("behavior:") + Item->GetStringField(TEXT("parent")) + TEXT(":") + FString::FromInt(Item->GetIntegerField(TEXT("index")));
			if (Item->GetStringField(TEXT("kind")) == TEXT("binding"))
			{
				bool bChanged = !Before->HasField(Pair.Key);
				if (!bChanged) for (const TCHAR* Field : {TEXT("source"), TEXT("target"), TEXT("source_path"), TEXT("target_path")})
					bChanged |= UeAgentAuthoringState::MakeStableCanonicalValueKey(Item->TryGetField(Field))
						!= UeAgentAuthoringState::MakeStableCanonicalValueKey(Before->GetObjectField(Pair.Key)->TryGetField(Field));
				if (!bChanged)
				{
					// Native PostEdit resolves or removes this old relationship after the
					// new storage is installed; missing old endpoints are not new requests.
					FinalItems->SetObjectField(Id, Item);
					continue;
				}
				FPropertyBindingPath SourcePath, TargetPath;
				auto Resolve = [&](const TCHAR* Endpoint, const TCHAR* PathField, FPropertyBindingPath& Path) {
					FString OwnerId, PathText;
					if (!Item->TryGetStringField(Endpoint, OwnerId) || !Items->HasField(OwnerId))
						return Reject(TEXT("smart_object_binding_endpoint_invalid"), TEXT("The binding endpoint does not identify an item in the desired definition."), Endpoint);
					if (!Item->TryGetStringField(PathField, PathText) || PathText.IsEmpty())
						return Reject(TEXT("smart_object_binding_path_invalid"), TEXT("The binding property path must be a non-empty string."), PathField, Item->TryGetField(PathField), MakeShared<FJsonValueString>(TEXT("non-empty property path")));
					const auto OwnerItem = Items->GetObjectField(OwnerId); FGuid NativeId;
					if (!FGuid::Parse(OwnerId == TEXT("root") || OwnerId == TEXT("parameters")
						? OwnerItem->GetStringField(TEXT("native_id")) : OwnerId, NativeId)) {
						auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("smart_object_binding_identity_invalid"),
							TEXT("The resolved binding owner has no valid native identity."), TEXT("smart_object_import"));
						Diagnostic.SubjectId = OwnerId; Diagnostic.SourceKind = TEXT("native_result");
						return Fail(MoveTemp(Diagnostic));
					}
					Path.SetStructID(NativeId);
					if (!Path.FromString(PathText)) return Reject(TEXT("smart_object_binding_path_invalid"),
						TEXT("The binding property path has invalid path syntax."), PathField, MakeShared<FJsonValueString>(PathText));
					return true;
				};
				if (!Resolve(TEXT("source"), TEXT("source_path"), SourcePath)
					|| !Resolve(TEXT("target"), TEXT("target_path"), TargetPath)) return false;
				{
					FSmartObjectDefinitionPropertyBinding Binding(SourcePath, TargetPath);
					Item->SetField(TEXT("binding_state"), UeAgentPropertyValueCodec::EncodeStructValue(FSmartObjectDefinitionPropertyBinding::StaticStruct(), &Binding, nullptr, {}));
					Item->SetField(TEXT("source_path_state"), UeAgentPropertyValueCodec::EncodeStructValue(FPropertyBindingPath::StaticStruct(), &SourcePath, nullptr, {}));
					Item->SetField(TEXT("target_path_state"), UeAgentPropertyValueCodec::EncodeStructValue(FPropertyBindingPath::StaticStruct(), &TargetPath, nullptr, {}));
				}
				Id = TEXT("binding:") + Item->GetStringField(TEXT("target")) + TEXT(":") + TargetPath.ToString();
				if (FinalItems->HasField(Id)) return Reject(TEXT("smart_object_binding_target_duplicate"), TEXT("More than one binding writes to the same target property."), TEXT("target_path"), MakeShared<FJsonValueString>(TargetPath.ToString()), MakeShared<FJsonValueString>(TEXT("a property with at most one binding")));
			}
			Item->SetStringField(TEXT("id"), Id); FinalItems->SetObjectField(Id, Item);
		}
		Desired->SetObjectField(TEXT("items"), FinalItems);
		SourcePointer = TEXT("/smart_object_items");
		if (!ValidateDefinitionBindingsForAuthoring(Definition, MakeShared<FJsonValueObject>(Desired), Detail)) return Fail(Detail);
		OutDocument = Desired;
		return true;
	}
}
