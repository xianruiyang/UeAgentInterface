#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Domains/SmartObject/Authoring/UeAgentSmartObjectNativeCapture.h"
#include "Annotations/SmartObjectSlotLinkAnnotation.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"

namespace UeAgentSmartObjectAuthoring
{
	namespace
	{
		constexpr const TCHAR* AccessorId = TEXT("smart_object.structure.state");
		constexpr const TCHAR* DefinitionKind = TEXT("smart_object.definition");
		constexpr const TCHAR* ItemKind = TEXT("smart_object.item");
		constexpr const TCHAR* PropertyKind = TEXT("smart_object.item_property");
		FString IdentityModel(const FString& Kind) { return Kind + TEXT(".identity"); }
		FString Id(const FGuid& Value) { return Value.ToString(EGuidFormats::DigitsWithHyphensLower); }
		FString ItemPointer(const FString& Id) { return TEXT("/items/") + Id.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); }

		UE::UAI::Contract::FDiagnostic DocumentFailure(const TCHAR* Code, const TCHAR* Reason, const USmartObjectDefinition& Definition,
			const FString& InternalPointer, const FJsonObject* Input = nullptr, const FString& Field = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_apply"));
			Error.TargetPath = Definition.GetPathName();
			Error.Source.InternalJsonPath = InternalPointer;
			Error.PropertyPath = Field;
			if (Input)
			{
				Input->TryGetStringField(TEXT("label"), Error.CandidateName);
				if (!Field.IsEmpty()) Error.ActualValuePresent = Input->HasField(Field);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, Field);
			}
			return Error;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using EOutcome = UeAgentAuthoringState::EReadOutcome;
			OutValue = {}; OutError.Reset();
			const auto FailRead = [&](const TCHAR* Code, const FString& Reason, const FString& ExpectedType = FString())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("state_capture"));
				OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
				OutError.CandidateName = Request.Context.FindRef(TEXT("label"));
				OutError.PropertyPath = Request.PropertyPath;
				OutError.SubjectId = Request.TargetIdentity.GetSelector(TEXT("node_id"));
				OutError.SemanticId = Request.SemanticId;
				OutError.SourceKind = TEXT("native_result");
				if (!ExpectedType.IsEmpty())
				{
					OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedType);
					OutError.ActualValue = MakeShared<FJsonValueString>(Request.RootObject ? Request.RootObject->GetClass()->GetPathName()
						: Request.RootStruct ? Request.RootStruct->GetPathName() : TEXT("no native data"));
				}
				return EOutcome::Failed;
			};
			if (Request.SemanticId == ItemPropertySemanticId)
			{
				UeAgentAuthoringState::FStateAccessor Accessor;
				if (!UeAgentAuthoringState::FStateAccessorRegistry::Get().CopyAccessor(TEXT("core.property"), Accessor))
					return FailRead(TEXT("smart_object_property_accessor_missing"), TEXT("Smart Object property capture requires the core property accessor, which is not registered."));
				const EOutcome Outcome = Accessor(Request, OutValue, OutError);
				// This relationship is edited by the keyed item's linked_slot field.
				// Keep its full native property in snapshots/checkpoints, not as a second write surface.
				if (Outcome == EOutcome::Resolved && Request.RootStruct
					&& Request.RootStruct->IsChildOf(FSmartObjectSlotLinkAnnotation::StaticStruct())
					&& Request.PropertyPath == TEXT("LinkedSlot")) OutValue.bAuthoringVisible = false;
				return Outcome;
			}
			if (Request.SemanticId != DefinitionSemanticId && Request.SemanticId != ItemSemanticId)
				return FailRead(TEXT("smart_object_semantic_unsupported"), TEXT("The Smart Object accessor supports definition, item and item-property capture; the requested semantic category is unsupported."));
			const auto Value = MakeShared<FJsonObject>();
			if (Request.SemanticId == DefinitionSemanticId)
			{
				const auto* Definition = Cast<USmartObjectDefinition>(Request.RootObject);
				FDefinitionCapture Capture;
				if (!Definition) return FailRead(TEXT("smart_object_definition_missing"),
					TEXT("Smart Object capture requires a native definition object; the supplied object has a different type or is missing."), USmartObjectDefinition::StaticClass()->GetPathName());
				if (!CaptureDefinition(*Definition, Capture, OutError)) return EOutcome::Failed;
				TArray<TSharedPtr<FJsonValue>> Descriptors;
				UeAgentPropertyValueCodec::FOptions Options;
				for (const auto& Descriptor : Capture.BindableStructs)
					Descriptors.Add(UeAgentPropertyValueCodec::EncodeStructValue(
						FSmartObjectDefinitionBindableStructDescriptor::StaticStruct(), &Descriptor, nullptr, Options));
				Value->SetArrayField(TEXT("bindable_structs"), Descriptors);
			}
			if (Request.SemanticId == ItemSemanticId)
			{
				for (const auto& Field : Request.Context) Value->SetStringField(Field.Key, Field.Value);
				Value->SetStringField(TEXT("id"), Request.TargetIdentity.GetSelector(TEXT("node_id")));
				Value->SetNumberField(TEXT("index"), FCString::Atoi(*Request.Context.FindRef(TEXT("index"))));
				if (Request.Context.FindRef(TEXT("kind")) == TEXT("binding"))
				{
					const auto& Binding = *static_cast<const FPropertyBindingBinding*>(Request.RootData);
					UeAgentPropertyValueCodec::FOptions Options;
					Value->SetField(TEXT("source_path_state"), UeAgentPropertyValueCodec::EncodeStructValue(
						FPropertyBindingPath::StaticStruct(), &Binding.GetSourcePath(), nullptr, Options));
					Value->SetField(TEXT("target_path_state"), UeAgentPropertyValueCodec::EncodeStructValue(
						FPropertyBindingPath::StaticStruct(), &Binding.GetTargetPath(), nullptr, Options));
					const auto& NativeBinding = static_cast<const FSmartObjectDefinitionPropertyBinding&>(Binding);
					Value->SetField(TEXT("binding_state"), UeAgentPropertyValueCodec::EncodeStructValue(
						FSmartObjectDefinitionPropertyBinding::StaticStruct(), &NativeBinding, nullptr, Options));
					OutValue.bAuthoringVisible = NativeBinding.TargetPathRetargetingStatus != ESmartObjectPropertyPathRetargetingStatus::RetargetedPath;
				}
			}
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object"); OutValue.Provenance = AccessorId;
			OutValue.Value = MakeShared<FJsonValueObject>(Value); OutValue.bAvailable = true;
			return EOutcome::Resolved;
		}
	}

	bool RegisterDefinitionState(UE::UAI::Contract::FDiagnostic& OutError)
	{
		struct FContract { const TCHAR* Semantic; const TCHAR* Kind; TArray<FString> Keys; };
		const TArray<FContract> Contracts{
			{DefinitionSemanticId, DefinitionKind, {TEXT("asset_path"), TEXT("graph_name")}},
			{ItemSemanticId, ItemKind, {TEXT("asset_path"), TEXT("graph_name"), TEXT("node_id")}},
			{ItemPropertySemanticId, PropertyKind, {TEXT("asset_path"), TEXT("graph_name"), TEXT("node_id"), TEXT("property_path")}}
		};
		for (const auto& Contract : Contracts)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Identity;
			Identity.ModelId = IdentityModel(Contract.Kind); Identity.TargetKind = Contract.Kind;
			int32 Priority = 100;
			for (const auto& Key : Contract.Keys) Identity.Selectors.Add({Key, Priority++, false});
			Identity.KeyPatterns = {{TEXT("scoped"), 100, Contract.Keys}};
			UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
			if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Identity, IdentityResult, OutError)) return false;
			const bool bProperty = FString(Contract.Kind) == PropertyKind;
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = Contract.Semantic; Semantic.DomainId = UeAgentReflectionAuthoring::DomainId;
			Semantic.TargetKind = Contract.Kind; Semantic.ValueType = bProperty ? TEXT("*") : TEXT("object");
			Semantic.OwnerModel = TEXT("smart_object_definition"); Semantic.IdentityModel = Identity.ModelId;
			Semantic.StateAccessor = AccessorId; Semantic.AvailabilitySource = AccessorId;
			Semantic.WriteCapability = bProperty ? UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment
				: UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation;
			Semantic.ConstraintSource = TEXT("smart_object.definition"); Semantic.Comparator = TEXT("uai.compare.canonical_value");
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(Semantic, Result, OutError)) return false;
		}
		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = AccessorId; Accessor.TargetKinds = {DefinitionKind, ItemKind, TEXT("property")};
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(Accessor, ReadState, Result, OutError);
	}

	bool AppendStructuralReadRequests(const USmartObjectDefinition& Definition, const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FDefinitionCapture Capture;
		if (!CaptureDefinition(Definition, Capture, OutError)) return false;
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		auto Add = [&](const TCHAR* Semantic, const TCHAR* Kind, const FString& ItemId,
			const TMap<FString, FString>& Context, const void* Data = nullptr) {
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = Semantic;
			TMap<FString, FString> Selectors{{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), TEXT("main")}};
			if (!ItemId.IsEmpty()) Selectors.Add(TEXT("node_id"), ItemId);
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel(Kind), Kind, Selectors, Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId; Request.TargetQuery.TargetKind = Kind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = &Definition; Request.RootData = Data; Request.Context = Context;
			Requests.Add(MoveTemp(Request)); return true;
		};
		auto Options = [&](const FString& ItemId, const FString& Part) {
			UeAgentAuthoringState::FEditablePropertyReadOptions Result;
			Result.SemanticId = ItemPropertySemanticId; Result.Limits.bShowDefaultsOnly = true;
			Result.BuildSnapshotIdentity = [AssetPath, ItemId, Part](const FString& Path,
				UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, UE::UAI::Contract::FDiagnostic& Error) {
				return UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel(PropertyKind), PropertyKind,
					{{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), TEXT("main")}, {TEXT("node_id"), ItemId},
						{TEXT("property_path"), Part + TEXT(".") + Path}}, Identity, Error);
			};
			return Result;
		};
		if (!Add(DefinitionSemanticId, DefinitionKind, {}, {})) return false;
		if (!Add(ItemSemanticId, ItemKind, TEXT("root"), {{TEXT("kind"), TEXT("settings")},
			{TEXT("label"), TEXT("Object Settings")}, {TEXT("native_id"), Id(Capture.RootId)},
			{TEXT("class_path"), Definition.GetClass()->GetPathName()}})) return false;
		auto RootOptions = Options(TEXT("root"), TEXT("object"));
		RootOptions.ExcludedProperties = {TEXT("Slots"), TEXT("DefaultBehaviorDefinitions"), TEXT("DefinitionData"),
			TEXT("Parameters"), TEXT("BindingCollection")};
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&Definition, RootOptions, Requests, OutError)) return false;
		if (!Add(ItemSemanticId, ItemKind, TEXT("parameters"), {{TEXT("kind"), TEXT("parameters")},
			{TEXT("label"), TEXT("Parameters")}, {TEXT("native_id"), Id(Capture.ParametersId)},
			{TEXT("class_path"), FInstancedPropertyBag::StaticStruct()->GetPathName()}})) return false;
		auto ParameterOptions = Options(TEXT("parameters"), TEXT("parameters"));
		ParameterOptions.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) { return Property->GetFName() == TEXT("Parameters"); };
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&Definition, ParameterOptions, Requests, OutError)) return false;
		auto AddBehaviors = [&](TConstArrayView<TObjectPtr<USmartObjectBehaviorDefinition>> Behaviors, const FString& Parent) {
			for (int32 Index = 0; Index < Behaviors.Num(); ++Index)
			{
				const auto* Behavior = Behaviors[Index].Get();
				const FString Key = TEXT("behavior:") + Parent + TEXT(":") + FString::FromInt(Index);
				if (!Add(ItemSemanticId, ItemKind, Key, {{TEXT("kind"), TEXT("behavior")}, {TEXT("parent"), Parent},
					{TEXT("index"), FString::FromInt(Index)}, {TEXT("label"), Behavior ? Behavior->GetClass()->GetDisplayNameText().ToString() : TEXT("Empty Behavior")},
					{TEXT("object_name"), Behavior ? Behavior->GetName() : FString()},
					{TEXT("class_path"), Behavior ? Behavior->GetClass()->GetPathName() : FString()}})) return false;
				if (Behavior && !UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
					Behavior, Options(Key, TEXT("behavior")), Requests, OutError)) return false;
			}
			return true;
		};
		auto AddData = [&](TConstArrayView<FSmartObjectDefinitionDataProxy> Values, const FString& Parent) {
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const auto& Proxy = Values[Index]; const FString Key = Id(Proxy.ID);
				const auto* Type = Proxy.Data.GetScriptStruct();
				TMap<FString, FString> Context{{TEXT("kind"), TEXT("definition_data")}, {TEXT("parent"), Parent},
					{TEXT("index"), FString::FromInt(Index)}, {TEXT("label"), Type ? Type->GetDisplayNameText().ToString() : TEXT("Empty Definition Data")},
					{TEXT("class_path"), Type ? Type->GetPathName() : FString()}};
				if (Type && Type->IsChildOf(FSmartObjectSlotLinkAnnotation::StaticStruct())) {
					const FGuid& LinkedId = Proxy.Data.Get<FSmartObjectSlotLinkAnnotation>().LinkedSlot.GetSlotID();
					Context.Add(TEXT("linked_slot"), LinkedId.IsValid() ? Id(LinkedId) : FString());
				}
				if (!Add(ItemSemanticId, ItemKind, Key, Context)) return false;
				if (Type && !UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(const_cast<UScriptStruct*>(Type),
					Proxy.Data.GetMemory(), AssetPath + TEXT(".data:") + Key, &Definition,
					Options(Key, TEXT("data")), Requests, OutError)) return false;
			}
			return true;
		};
		if (!AddBehaviors(Capture.DefaultBehaviors, TEXT("root")) || !AddData(Capture.DefinitionData, TEXT("root"))) return false;
		for (int32 Index = 0; Index < Capture.Slots.Num(); ++Index)
		{
			const auto& Slot = Capture.Slots[Index]; const FString Key = Id(Slot.ID);
			if (!Add(ItemSemanticId, ItemKind, Key, {{TEXT("kind"), TEXT("slot")}, {TEXT("label"), Slot.Name.ToString()},
				{TEXT("parent"), TEXT("root")}, {TEXT("index"), FString::FromInt(Index)},
				{TEXT("class_path"), FSmartObjectSlotDefinition::StaticStruct()->GetPathName()}})) return false;
			auto SlotOptions = Options(Key, TEXT("slot"));
			SlotOptions.ExcludedProperties = {TEXT("Name"), TEXT("ID"), TEXT("BehaviorDefinitions"), TEXT("DefinitionData")};
			if (!UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(FSmartObjectSlotDefinition::StaticStruct(),
				&Slot, AssetPath + TEXT(".slot:") + Key, &Definition, SlotOptions, Requests, OutError)
				|| !AddBehaviors(Slot.BehaviorDefinitions, Key) || !AddData(Slot.DefinitionData, Key)) return false;
		}
		auto BindingItemId = [&](const FGuid& Guid) {
			return Guid == Capture.RootId ? FString(TEXT("root")) : Guid == Capture.ParametersId ? FString(TEXT("parameters")) : Id(Guid);
		};
		bool bBindings = true; int32 BindingIndex = 0;
		Capture.Bindings->ForEachBinding([&](const FPropertyBindingBinding& Binding) {
			if (!bBindings) return;
			const FString Source = BindingItemId(Binding.GetSourcePath().GetStructID());
			const FString Target = BindingItemId(Binding.GetTargetPath().GetStructID());
			bBindings = Add(ItemSemanticId, ItemKind, TEXT("binding:") + Target + TEXT(":") + Binding.GetTargetPath().ToString(),
				{{TEXT("kind"), TEXT("binding")}, {TEXT("label"), TEXT("Binding")}, {TEXT("index"), FString::FromInt(BindingIndex++)},
				{TEXT("source"), Source}, {TEXT("target"), Target}, {TEXT("source_path"), Binding.GetSourcePath().ToString()},
				{TEXT("target_path"), Binding.GetTargetPath().ToString()}, {TEXT("class_path"), FSmartObjectDefinitionPropertyBinding::StaticStruct()->GetPathName()}}, &Binding);
		});
		if (!bBindings) return false;
		OutRequests.Append(MoveTemp(Requests)); return true;
	}

	bool ReadDefinitionDocument(const USmartObjectDefinition& Definition,
		TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValue.Reset(); OutError.Reset();
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!AppendStructuralReadRequests(Definition, Definition.GetPathName(), Requests, OutError)) return false;
		// Internal reconstruction retains nested native identities omitted by authoring.
		for (auto& Request : Requests) Request.Limits.StructFieldFilter = nullptr;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{ OutError = Capture.Diagnostic; return false; }
		const auto Entries = Capture.Snapshot.CopyEntries();
		const auto Document = MakeShared<FJsonObject>();
		const auto Items = MakeShared<FJsonObject>();
		TMap<FString, TArray<UeAgentAuthoringState::FStateEntry>> Properties;
		for (const auto& Entry : Entries)
		{
			const FString Owner = Entry.TargetIdentity.GetSelector(TEXT("node_id"));
			if (Entry.SemanticId == ItemSemanticId)
				Items->SetField(Owner, UeAgentAuthoringState::CloneCanonicalValue(Entry.Value));
			else if (Entry.SemanticId == ItemPropertySemanticId)
				Properties.FindOrAdd(Owner).Add(Entry);
			else if (Entry.SemanticId == DefinitionSemanticId)
				Document->SetField(TEXT("bindable_structs"), UeAgentAuthoringState::CloneCanonicalValue(
					Entry.Value->AsObject()->TryGetField(TEXT("bindable_structs"))));
		}
		for (const auto& Pair : Properties)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Items->TryGetObjectField(Pair.Key, Item))
			{
				OutError = DocumentFailure(TEXT("smart_object_property_owner_missing"),
					TEXT("Captured properties have no corresponding structural item in this Smart Object definition."), Definition, ItemPointer(Pair.Key) + TEXT("/properties"));
				OutError.Stage = TEXT("state_capture");
				OutError.SourceKind = TEXT("native_result");
				OutError.SubjectId = Pair.Key;
				if (!Pair.Value.IsEmpty()) OutError.PropertyPath = Pair.Value[0].TargetIdentity.GetSelector(TEXT("property_path"));
				return false;
			}
			TSharedPtr<FJsonObject> PropertyDocument;
			if (!UeAgentDetailsAuthoringService::BuildPropertiesDocument(Pair.Value, {}, PropertyDocument, OutError)) return false;
			(*Item)->SetObjectField(TEXT("properties"), PropertyDocument);
		}
		Document->SetObjectField(TEXT("items"), Items);
		OutValue = MakeShared<FJsonValueObject>(Document);
		return true;
	}

	static bool ApplyDefinitionBindingStateImpl(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, const bool bApplyBindableStructs, const bool bForward)
	{
		OutError.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Descriptors = nullptr;
		const TSharedPtr<FJsonObject>* Items = nullptr;
		if (!Document.IsValid() || Document->Type != EJson::Object)
		{
			OutError = DocumentFailure(TEXT("smart_object_binding_document_invalid"), TEXT("The Smart Object binding document must be an object."), Definition, TEXT("/"));
			return false;
		}
		if (!Document->AsObject()->TryGetArrayField(TEXT("bindable_structs"), Descriptors))
		{
			OutError = DocumentFailure(TEXT("smart_object_binding_document_invalid"), TEXT("Smart Object binding document field 'bindable_structs' must be an array."), Definition, TEXT("/bindable_structs"), Document->AsObject().Get(), TEXT("bindable_structs"));
			return false;
		}
		if (!Document->AsObject()->TryGetObjectField(TEXT("items"), Items))
		{
			OutError = DocumentFailure(TEXT("smart_object_binding_document_invalid"), TEXT("Smart Object binding document field 'items' must be an object."), Definition, TEXT("/items"), Document->AsObject().Get(), TEXT("items"));
			return false;
		}
		TArray<FSmartObjectDefinitionBindableStructDescriptor> DesiredDescriptors;
		for (int32 Index = 0; Index < Descriptors->Num(); ++Index)
		{
			auto& Descriptor = DesiredDescriptors.AddDefaulted_GetRef();
			if (!UeAgentPropertyValueCodec::DecodeStructValue(
				FSmartObjectDefinitionBindableStructDescriptor::StaticStruct(), (*Descriptors)[Index], &Descriptor, OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/bindable_structs/") + LexToString(Index));
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document->AsObject(), TEXT("bindable_structs"));
				return false;
			}
		}
		TMap<int32, FSmartObjectDefinitionPropertyBinding> IndexedBindings;
		for (const auto& Pair : (*Items)->Values)
		{
			FString Kind;
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
			{
				OutError = DocumentFailure(TEXT("smart_object_document_item_invalid"), TEXT("Each Smart Object binding document item must be an object."), Definition, ItemPointer(Pair.Key), Items->Get(), Pair.Key);
				OutError.PropertyPath = TEXT("items");
				OutError.ActualEvidence = Pair.Value;
				return false;
			}
			if (!Pair.Value->AsObject()->TryGetStringField(TEXT("kind"), Kind))
			{
				OutError = DocumentFailure(TEXT("smart_object_document_item_invalid"), TEXT("Smart Object item field 'kind' must be a string."), Definition, ItemPointer(Pair.Key) + TEXT("/kind"), Pair.Value->AsObject().Get(), TEXT("kind"));
				OutError.ActualValue = Pair.Value->AsObject()->TryGetField(TEXT("kind"));
				return false;
			}
			if (Kind != TEXT("binding")) continue;
			const auto Item = Pair.Value->AsObject();
			double Index = 0;
			const bool bHasIndex = Item->TryGetNumberField(TEXT("index"), Index);
			if (!bHasIndex || !FMath::IsFinite(Index)
				|| Index < 0 || Index > MAX_int32 || Index != FMath::FloorToDouble(Index))
			{
				OutError = DocumentFailure(TEXT("smart_object_binding_order_invalid"), TEXT("A binding index must be a finite integer between 0 and 2147483647."), Definition, ItemPointer(Pair.Key) + TEXT("/index"), Item.Get(), TEXT("index"));
				// Non-finite native numbers are not JSON values; describe them at the source.
				OutError.ActualValue = bHasIndex && !FMath::IsFinite(Index)
					? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(LexToString(Index))) : Item->TryGetField(TEXT("index"));
				return false;
			}
			if (IndexedBindings.Contains(static_cast<int32>(Index)))
			{
				OutError = DocumentFailure(TEXT("smart_object_binding_order_invalid"), TEXT("Each binding must have a distinct index; this index is used more than once."), Definition, ItemPointer(Pair.Key) + TEXT("/index"), Item.Get(), TEXT("index"));
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Index);
				return false;
			}
			FSmartObjectDefinitionPropertyBinding Binding;
			if (!UeAgentPropertyValueCodec::DecodeStructValue(FSmartObjectDefinitionPropertyBinding::StaticStruct(),
				Item->TryGetField(TEXT("binding_state")), &Binding, OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/binding_state"));
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Item, TEXT("binding_state"));
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, ItemPointer(Pair.Key));
				return false;
			}
			IndexedBindings.Add(static_cast<int32>(Index), MoveTemp(Binding));
		}
		TArray<FSmartObjectDefinitionPropertyBinding> DesiredBindings;
		for (int32 Index = 0; Index < IndexedBindings.Num(); ++Index)
		{
			const auto* Binding = IndexedBindings.Find(Index);
			if (!Binding)
			{
				OutError = DocumentFailure(TEXT("smart_object_binding_order_invalid"), TEXT("Binding indices must form a continuous sequence starting at zero; a required index is missing."), Definition, TEXT("/items"), Document->AsObject().Get(), TEXT("items"));
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Index);
				TArray<int32> Indices;
				IndexedBindings.GetKeys(Indices);
				Indices.Sort();
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Indices[Index]);
				return false;
			}
			DesiredBindings.Add(*Binding);
		}
		FDefinitionCapture Native;
		if (!CaptureDefinition(Definition, Native, OutError)) return false;
		const auto* DescriptorsProperty = FindFProperty<FArrayProperty>(FSmartObjectBindingCollection::StaticStruct(), TEXT("BindableStructs"));
		const auto* BindingsProperty = FindFProperty<FArrayProperty>(FSmartObjectBindingCollection::StaticStruct(), TEXT("PropertyBindings"));
		const auto* BindingInner = BindingsProperty ? CastField<FStructProperty>(BindingsProperty->Inner) : nullptr;
		if (!DescriptorsProperty || !BindingInner || BindingInner->Struct != FSmartObjectDefinitionPropertyBinding::StaticStruct())
		{
			OutError = DocumentFailure(TEXT("smart_object_native_layout_invalid"),
				!DescriptorsProperty ? TEXT("The native Smart Object binding collection has no BindableStructs array.")
				: !BindingInner ? TEXT("The native PropertyBindings field is missing or is not an array of structures.")
				: TEXT("The native PropertyBindings array uses an unexpected binding structure type."), Definition, TEXT("/"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = !DescriptorsProperty ? TEXT("BindableStructs") : TEXT("PropertyBindings");
			if (BindingInner)
			{
				OutError.ExpectedValue = MakeShared<FJsonValueString>(FSmartObjectDefinitionPropertyBinding::StaticStruct()->GetPathName());
				OutError.ActualValue = MakeShared<FJsonValueString>(BindingInner->Struct ? BindingInner->Struct->GetPathName() : TEXT("no struct type"));
			}
			return false;
		}
		// Keep the collection's transient owner and observers. Assign only the two
		// persistent arrays after all values have decoded; native forward settle is separate.
		auto* Collection = const_cast<FSmartObjectBindingCollection*>(Native.Bindings);
		if (bApplyBindableStructs)
			*DescriptorsProperty->ContainerPtrToValuePtr<TArray<FSmartObjectDefinitionBindableStructDescriptor>>(Collection) = MoveTemp(DesiredDescriptors);
		if (bForward) {
			TArray<FPropertyBindingPath> Targets;
			Collection->ForEachBinding([&](const FPropertyBindingBinding& Binding) { Targets.Add(Binding.GetTargetPath()); });
			for (const auto& Target : Targets) Collection->RemoveBindings(Target);
			for (auto& Binding : DesiredBindings) Collection->AddSmartObjectBinding(MoveTemp(Binding));
		} else *BindingsProperty->ContainerPtrToValuePtr<TArray<FSmartObjectDefinitionPropertyBinding>>(Collection) = MoveTemp(DesiredBindings);
		return true;
	}
	bool ApplyDefinitionBindingState(USmartObjectDefinition& Definition, const TSharedPtr<FJsonValue>& Document,
		UE::UAI::Contract::FDiagnostic& OutError, bool bApplyBindableStructs)
	{ return ApplyDefinitionBindingStateImpl(Definition, Document, OutError, bApplyBindableStructs, false); }
	bool ApplyDefinitionBindingsForAuthoring(USmartObjectDefinition& Definition, const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError)
	{ return ApplyDefinitionBindingStateImpl(Definition, Document, OutError, false, true); }

	namespace
	{
	bool ApplyDefinitionDocumentPropertiesImpl(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage,
		const bool bEmitEditorNotifications)
	{
		OutError.Reset();
		const TSharedPtr<FJsonObject>* Items = nullptr;
		if (!Document.IsValid() || Document->Type != EJson::Object)
		{
			OutError = DocumentFailure(TEXT("smart_object_document_items_invalid"), TEXT("The Smart Object document must be an object containing an 'items' object."), Definition, TEXT("/"));
			return false;
		}
		if (!Document->AsObject()->TryGetObjectField(TEXT("items"), Items))
		{
			OutError = DocumentFailure(TEXT("smart_object_document_items_invalid"), TEXT("Smart Object document field 'items' must be an object."), Definition, TEXT("/items"), Document->AsObject().Get(), TEXT("items"));
			return false;
		}
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!AppendStructuralReadRequests(Definition, Definition.GetPathName(), Requests, OutError)) return false;
		TMap<FString, TSharedPtr<FJsonObject>> PropertiesByOwner;
		for (const auto& Item : (*Items)->Values)
		{
			if (!Item.Value.IsValid() || Item.Value->Type != EJson::Object)
			{
				OutError = DocumentFailure(TEXT("smart_object_document_item_invalid"), TEXT("Each Smart Object document item must be an object."), Definition, ItemPointer(Item.Key), Items->Get(), Item.Key);
				OutError.PropertyPath = TEXT("items");
				OutError.ActualEvidence = Item.Value;
				return false;
			}
			TSharedPtr<FJsonObject> Properties;
			if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Item.Value->AsObject(), TEXT("properties"), Properties, OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, ItemPointer(Item.Key));
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Item.Value->AsObject(), TEXT("document_apply"), Definition.GetPathName(), TEXT("properties"));
				return false;
			}
			if (Properties)
			{
				if (bEmitEditorNotifications && Item.Value->AsObject()->HasField(TEXT("linked_slot"))) {
					// Forward slot relations are assigned once from linked_slot. Replaying
					// their old checkpoint property here would restore stale SlotID/Index
					// before a native Slots notification (notably after target deletion).
					const auto ForwardProperties = MakeShared<FJsonObject>();
					ForwardProperties->Values = Properties->Values;
					auto Descriptors = Properties->GetArrayField(TEXT("properties"));
					Descriptors.RemoveAll([](const auto& Descriptor) {
						return Descriptor->AsObject()->GetStringField(TEXT("property_path")) == TEXT("data.LinkedSlot");
					});
					ForwardProperties->SetArrayField(TEXT("properties"), Descriptors);
					PropertiesByOwner.Add(Item.Key, ForwardProperties);
				} else PropertiesByOwner.Add(Item.Key, Properties);
			}
		}
		TArray<FString> OwnerOrder;
		if (bEmitEditorNotifications)
		{
			PropertiesByOwner.GetKeys(OwnerOrder);
			OwnerOrder.Sort([](const FString& A, const FString& B) {
				auto Rank = [](const FString& Owner) { return Owner == TEXT("parameters") ? 2 : Owner == TEXT("root") ? 1 : 0; };
				return Rank(A) != Rank(B) ? Rank(A) < Rank(B) : A < B;
			});
		}
		// Native root schema and parameter notifications run after all dependent
		// item values. Otherwise a later snapshot replay would undo their results.
		return UeAgentDetailsAuthoringService::ApplyScopedProperties(PropertiesByOwner, Requests,
			ItemPropertySemanticId, TEXT("node_id"), TEXT("smart_object"), [bEmitEditorNotifications, Items](const auto& Request) {
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bAllowReadOnly = true;
				Options.bShowDefaultsOnly = true;
				Options.bAllowDomainAuthorizedOwners = true;
				Options.bCompactValues = true;
				// Independent slot/data storage has no UObject-relative locator chain.
				// Notify its actual containing member through the shared apply owner.
				Options.bEmitEditorNotifications = bEmitEditorNotifications;
				if (bEmitEditorNotifications && Request.RootStruct && Request.SnapshotIdentityOverride.IsSet())
				{
					const auto Item = (*Items)->GetObjectField(Request.SnapshotIdentityOverride->GetSelector(TEXT("node_id")));
					const bool bRootData = Item->GetStringField(TEXT("kind")) == TEXT("definition_data")
						&& Item->GetStringField(TEXT("parent")) == TEXT("root");
					Options.StructNotificationOwnerPropertyPath = bRootData ? TEXT("DefinitionData") : TEXT("Slots");
				}
				return Options;
			}, OutError, TextIdentityPackage, OwnerOrder);
	}
	}

	bool ApplyDefinitionDocumentProperties(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage)
	{
		return ApplyDefinitionDocumentPropertiesImpl(
			Definition, Document, OutError, TextIdentityPackage, false);
	}

	bool ApplyDefinitionDocumentPropertiesForAuthoring(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage)
	{
		return ApplyDefinitionDocumentPropertiesImpl(
			Definition, Document, OutError, TextIdentityPackage, true);
	}
}
