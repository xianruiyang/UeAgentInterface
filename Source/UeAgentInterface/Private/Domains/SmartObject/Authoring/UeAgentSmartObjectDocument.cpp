#include "Domains/SmartObject/Authoring/UeAgentSmartObjectDocument.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectNativeCapture.h"
#include "QueryOwners/AiBehavior/UeAgentSmartObjectMenu.h"
#include "Annotations/SmartObjectSlotLinkAnnotation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "UObject/UnrealType.h"
#include "WorldConditionQuery.h"
#include "WorldConditionBase.h"
#include "WorldConditionSchema.h"
#include "PropertyBindingExtension.h"
#include "PropertyBindingDataView.h"
#include "PropertyBindingTypes.h"
#include "UObject/StrongObjectPtr.h"

namespace UeAgentSmartObjectAuthoring
{
	bool InitializeConditionQuery(FWorldConditionQueryDefinition& Query, const UObject& Owner,
		const FString& PropertyPath, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto Failure = [&](const TCHAR* Code, const FString& Field, const FString& Message,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Message, TEXT("native_compile"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Owner.GetPathName();
			OutError.PropertyPath = Field;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual); OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		};
		const auto* Property = FindFProperty<FArrayProperty>(FWorldConditionQueryDefinition::StaticStruct(), TEXT("EditableConditions"));
		const auto* Element = Property ? CastField<FStructProperty>(Property->Inner) : nullptr;
		if (!Element || Element->Struct != FWorldConditionEditable::StaticStruct())
			return Failure(TEXT("world_condition_native_layout_invalid"), PropertyPath + TEXT(".EditableConditions"),
				TEXT("The native editable-condition field must be an array of WorldConditionEditable structures."),
				MakeShared<FJsonValueString>(Property ? Property->GetCPPType(nullptr, 0) : TEXT("missing array property")),
				MakeShared<FJsonValueString>(FWorldConditionEditable::StaticStruct()->GetPathName()));
		const UWorldConditionSchema* Schema = Query.GetSchemaClass().GetDefaultObject();
		if (!Schema) return Failure(TEXT("world_condition_schema_required"), PropertyPath + TEXT(".SchemaClass"),
			TEXT("The condition query has no available schema class default object."),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		const auto& Editable = *Property->ContainerPtrToValuePtr<TArray<FWorldConditionEditable>>(&Query);
		TArray<const UScriptStruct*> RequestedTypes;
		for (int32 Index = 0; Index < Editable.Num(); ++Index) {
			const UScriptStruct* Type = Editable[Index].Condition.GetScriptStruct();
			// Empty native editor rows are placeholders, not an authored condition.
			if (!Type) continue;
			if (!Type->IsChildOf(FWorldConditionBase::StaticStruct()) || !Schema->IsStructAllowed(Type)) {
				const auto Expected = MakeShared<FJsonObject>();
				Expected->SetStringField(TEXT("base_type"), FWorldConditionBase::StaticStruct()->GetPathName());
				Expected->SetStringField(TEXT("schema_class"), Schema->GetClass()->GetPathName());
				return Failure(TEXT("world_condition_type_not_allowed"),
					PropertyPath + FString::Printf(TEXT(".EditableConditions[%d].Condition"), Index),
					!Type->IsChildOf(FWorldConditionBase::StaticStruct()) ? TEXT("The condition type does not derive from WorldConditionBase.")
					: TEXT("The selected schema does not allow this condition type."),
					MakeShared<FJsonValueString>(Type->GetPathName()), MakeShared<FJsonValueObject>(Expected));
			}
			RequestedTypes.Add(Type);
		}
		if (!Query.Initialize(&Owner))
			return Failure(TEXT("world_condition_initialize_failed"), PropertyPath, TEXT("Unreal failed to initialize the native condition query."),
				MakeShared<FJsonValueString>(Schema->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("schema with successfully initialized native conditions")));
		const FWorldConditionQuerySharedDefinition* Compiled = Query.GetSharedDefinition();
		const int32 ActualCount = Compiled ? Compiled->GetConditions().Num() : 0;
		if (ActualCount != RequestedTypes.Num())
			return Failure(TEXT("world_condition_compilation_incomplete"), PropertyPath + TEXT(".Conditions.Count"),
				TEXT("Native compilation did not retain every requested condition."),
				MakeShared<FJsonValueNumber>(ActualCount), MakeShared<FJsonValueNumber>(RequestedTypes.Num()));
		for (int32 Index = 0; Index < ActualCount; ++Index) {
			const auto* ActualType = Compiled->GetConditions()[Index].GetScriptStruct();
			if (ActualType != RequestedTypes[Index])
				return Failure(TEXT("world_condition_compilation_incomplete"), PropertyPath + FString::Printf(TEXT(".Conditions[%d]"), Index),
					TEXT("Native compilation changed the requested condition type or order."),
					MakeShared<FJsonValueString>(ActualType ? ActualType->GetPathName() : TEXT("no condition type")),
					MakeShared<FJsonValueString>(RequestedTypes[Index]->GetPathName()));
		}
		// UE Initialize() does not forward Link() failure. A genuinely empty query is valid.
		if (ActualCount > 0 && !Compiled->IsLinked())
			return Failure(TEXT("world_condition_link_failed"), PropertyPath + TEXT(".IsLinked"),
				TEXT("Native conditions could not bind to the selected schema context."),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		return true;
	}

	namespace
	{
		UE::UAI::Contract::FDiagnostic DocumentFailure(const TCHAR* Code, const FString& Reason,
			const FString& Pointer, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("smart_object_document"));
			Error.PropertyPath = Pointer;
			Error.Source.InternalJsonPath = Pointer;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ActualValue = MoveTemp(Actual); Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}
		FString ItemPointer(const FString& Key)
		{
			return TEXT("/items/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Key);
		}
		void LocateItemFailure(UE::UAI::Contract::FDiagnostic& Error, const FJsonObject& Item,
			const FString& Key, const FString& Field)
		{
			// The producer owns the field; only the existing sidecar resolves its input location.
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Item, Field);
			if (Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty())
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, ItemPointer(Key));
			if (Error.SubjectId.IsEmpty()) Error.SubjectId = Key;
		}
		void LocateConditionInput(UE::UAI::Contract::FDiagnostic& Error, const FJsonObject& Item)
		{
			if (!Error.Source.SourceFile.IsEmpty() || !Error.Source.SourceUnit.IsEmpty()) return;
			const auto PreviousSource = Error.Source;
			const FString PreviousKind = Error.SourceKind, PreviousPointer = Error.SemanticPointer, NativePath = Error.PropertyPath;
			Error.SourceKind.Reset(); Error.Source.InternalJsonPath = TEXT("/");
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Item);
			if (Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty()) {
				Error.Source = PreviousSource; Error.SourceKind = PreviousKind; Error.SemanticPointer = PreviousPointer;
			} else Error.SourceKind = TEXT("related_input");
			Error.PropertyPath = NativePath;
		}
		bool ItemFailure(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Reason,
			const FJsonObject& Item, const FString& Key, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			Error = DocumentFailure(Code, Reason, Field.IsEmpty() ? TEXT("/") : TEXT("/") + Field,
				MoveTemp(Actual), MoveTemp(Expected));
			Error.PropertyPath = Field;
			LocateItemFailure(Error, Item, Key, Field);
			return false;
		}
		bool NativeFailure(UE::UAI::Contract::FDiagnostic& Error, const USmartObjectDefinition& Definition,
			const TCHAR* Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			Error = DocumentFailure(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Definition.GetPathName();
			return false;
		}
		FString Text(const TSharedPtr<FJsonObject>& Item, const TCHAR* Field)
		{ FString Value; Item->TryGetStringField(Field, Value); return Value; }
		FGuid Guid(const FString& Value) { FGuid Result; FGuid::Parse(Value, Result); return Result; }
		FString Id(const FGuid& Value) { return Value.ToString(EGuidFormats::DigitsWithHyphensLower); }
		bool ApplyDefinitionDocumentImpl(USmartObjectDefinition& Definition,
			const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError,
			UPackage* TextIdentityPackage, bool bForwardAuthoring);
		bool ValidateForwardBindings(USmartObjectDefinition& Definition,
			const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError)
		{
			TMap<int32, TPair<FString, TSharedPtr<FJsonObject>>> BindingItems;
			for (const auto& Pair : Document->AsObject()->GetObjectField(TEXT("items"))->Values) {
				const auto Item = Pair.Value->AsObject();
				if (Text(Item, TEXT("kind")) == TEXT("binding"))
					BindingItems.Emplace(Item->GetIntegerField(TEXT("index")), TPair<FString, TSharedPtr<FJsonObject>>(Pair.Key, Item));
			}
			if (BindingItems.IsEmpty()) return true;
			// Exact replay validates proposed storage without notifying or changing the asset.
			TStrongObjectPtr<USmartObjectDefinition> Proposed(DuplicateObject<USmartObjectDefinition>(&Definition, GetTransientPackage()));
			if (!Proposed) return NativeFailure(OutError, Definition, TEXT("smart_object_binding_validation_copy_failed"),
				TEXT("Unreal could not duplicate the SmartObject definition for binding validation."),
				TEXT("definition"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (!ApplyDefinitionDocument(*Proposed, Document, OutError)) {
				if (OutError.TargetPath.IsEmpty() || OutError.TargetPath == Proposed->GetPathName())
					OutError.TargetPath = Definition.GetPathName();
				return false;
			}
			auto& Owner = static_cast<IPropertyBindingBindingCollectionOwner&>(*Proposed);
			const auto& BeforeOwner = static_cast<const IPropertyBindingBindingCollectionOwner&>(Definition);
			bool bValid = true; TSet<FString> Targets; int32 BindingIndex = 0;
			Owner.GetEditorPropertyBindings()->ForEachBinding([&](const FPropertyBindingBinding& Binding) {
				const int32 Index = BindingIndex++;
				if (!bValid) return;
				const auto& Source = Binding.GetSourcePath(); const auto& Target = Binding.GetTargetPath();
				const auto& Input = BindingItems.FindChecked(Index);
				const auto Reject = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) {
					bValid = ItemFailure(OutError, Code, Reason, *Input.Value, Input.Key, Field, MoveTemp(Actual), MoveTemp(Expected));
					OutError.TargetPath = Definition.GetPathName();
				};
				const FString TargetKey = Id(Target.GetStructID()) + TEXT(":") + Target.ToString();
				if (Targets.Contains(TargetKey)) {
					Reject(TEXT("smart_object_binding_target_duplicate"), TEXT("More than one binding writes to the same target property."),
						TEXT("target_path"), MakeShared<FJsonValueString>(Target.ToString()), MakeShared<FJsonValueString>(TEXT("one binding per target property"))); return;
				}
				Targets.Add(TargetKey);
				const auto* PreviousSource = BeforeOwner.GetEditorPropertyBindings()->GetBindingSource(Target);
				// Unedited relationships remain native context until PostEdit refreshes them.
				if (PreviousSource && *PreviousSource == Source) return;
				TArray<TInstancedStruct<FPropertyBindingBindableStructDescriptor>> Sources;
				Owner.GetBindableStructs(Target.GetStructID(), Sources);
				if (!Sources.ContainsByPredicate([&](const auto& Entry) { return Entry.Get().ID == Source.GetStructID(); })) {
					Reject(TEXT("smart_object_binding_source_unavailable"), TEXT("The selected source owner is unavailable to this binding target."),
						TEXT("source"), MakeShared<FJsonValueString>(Source.ToString()), MakeShared<FJsonValueString>(TEXT("source owner visible to the target"))); return;
				}
				FPropertyBindingDataView SourceView, TargetView;
				TArray<FPropertyBindingPathIndirection> SourceSteps, TargetSteps;
				if (!Owner.GetBindingDataViewByID(Source.GetStructID(), SourceView)) {
					Reject(TEXT("smart_object_binding_path_invalid"), TEXT("The source owner's native data view is unavailable."),
						TEXT("source"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)); return;
				}
				if (!Owner.GetBindingDataViewByID(Target.GetStructID(), TargetView)) {
					Reject(TEXT("smart_object_binding_path_invalid"), TEXT("The target owner's native data view is unavailable."),
						TEXT("target"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)); return;
				}
				FString NativeReason;
				if (!Source.ResolveIndirectionsWithValue(SourceView, SourceSteps, &NativeReason)) {
					Reject(TEXT("smart_object_binding_path_invalid"), TEXT("The source property path could not be resolved against its native value."),
						TEXT("source_path"), MakeShared<FJsonValueString>(Source.ToString()), MakeShared<FJsonValueString>(TEXT("resolvable source property path")));
					auto Cause = UE::UAI::Contract::FDiagnostic::Failure(TEXT("property_binding_path_resolve_failed"), NativeReason, TEXT("native_path_resolve"));
					Cause.PropertyPath = TEXT("source_path"); Cause.Source.InternalJsonPath = TEXT("/source_path");
					LocateItemFailure(Cause, *Input.Value, Input.Key, TEXT("source_path")); OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(Cause)); return;
				}
				if (!Target.ResolveIndirectionsWithValue(TargetView, TargetSteps, &NativeReason)) {
					Reject(TEXT("smart_object_binding_path_invalid"), TEXT("The target property path could not be resolved against its native value."),
						TEXT("target_path"), MakeShared<FJsonValueString>(Target.ToString()), MakeShared<FJsonValueString>(TEXT("resolvable target property path")));
					auto Cause = UE::UAI::Contract::FDiagnostic::Failure(TEXT("property_binding_path_resolve_failed"), NativeReason, TEXT("native_path_resolve"));
					Cause.PropertyPath = TEXT("target_path"); Cause.Source.InternalJsonPath = TEXT("/target_path");
					LocateItemFailure(Cause, *Input.Value, Input.Key, TEXT("target_path")); OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(Cause)); return;
				}
				if (TargetSteps.IsEmpty()) {
					Reject(TEXT("smart_object_binding_path_invalid"), TEXT("The binding target path does not select a property."),
						TEXT("target_path"), MakeShared<FJsonValueString>(Target.ToString()), MakeShared<FJsonValueString>(TEXT("non-empty property selection"))); return;
				}
				if (SourceSteps.IsEmpty()) {
					Reject(TEXT("smart_object_binding_context_source_unsupported"), TEXT("Binding directly from an entire context value is unsupported."),
						TEXT("source_path"), MakeShared<FJsonValueString>(Source.ToString()), MakeShared<FJsonValueString>(TEXT("source leaf property"))); return;
				}
				const FProperty* SourceProperty = SourceSteps.Last().GetProperty();
				const FProperty* TargetProperty = TargetSteps.Last().GetProperty();
				const auto PropertyFacts = [](const FProperty* Property) -> TSharedPtr<FJsonValue> {
					if (!Property) return MakeShared<FJsonValueNull>();
					const auto Facts = MakeShared<FJsonObject>();
					Facts->SetStringField(TEXT("property"), Property->GetName()); Facts->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
					return MakeShared<FJsonValueObject>(Facts);
				};
				if (!SourceProperty || !TargetProperty
					|| UE::PropertyBinding::GetPropertyCompatibility(SourceProperty, TargetProperty) == UE::PropertyBinding::EPropertyCompatibility::Incompatible) {
					Reject(TEXT("smart_object_binding_type_incompatible"), !SourceProperty || !TargetProperty
						? TEXT("A resolved binding endpoint has no native leaf property.") : TEXT("The source property type cannot be assigned or promoted to the target property type."),
						TEXT("target_path"), PropertyFacts(SourceProperty), PropertyFacts(TargetProperty)); return;
				}
				for (const auto& Step : SourceSteps)
					if (const FProperty* Property = Step.GetProperty(); Property && !UE::PropertyBinding::IsPropertyBindable(*Property)) {
						Reject(TEXT("smart_object_binding_source_not_bindable"), !Property->HasAnyPropertyFlags(CPF_Edit)
							? TEXT("The source path property is not editable.") : TEXT("The source path property is private or protected and does not permit binding access."),
							TEXT("source_path"), PropertyFacts(Property), MakeShared<FJsonValueString>(TEXT("editable property with binding access"))); return;
					}
				if (TargetProperty->HasAnyPropertyFlags(CPF_PersistentInstance | CPF_EditorOnly | CPF_Config | CPF_Deprecated)) {
					const auto Facts = MakeShared<FJsonObject>();
					Facts->SetStringField(TEXT("property"), TargetProperty->GetName());
					Facts->SetBoolField(TEXT("persistent_instance"), TargetProperty->HasAnyPropertyFlags(CPF_PersistentInstance));
					Facts->SetBoolField(TEXT("editor_only"), TargetProperty->HasAnyPropertyFlags(CPF_EditorOnly));
					Facts->SetBoolField(TEXT("config"), TargetProperty->HasAnyPropertyFlags(CPF_Config));
					Facts->SetBoolField(TEXT("deprecated"), TargetProperty->HasAnyPropertyFlags(CPF_Deprecated));
					Reject(TEXT("smart_object_binding_target_not_bindable"), TEXT("The target property's native flags prohibit binding."),
						TEXT("target_path"), MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("property without prohibited binding flags"))); return;
				}
				for (const auto& Step : TargetSteps)
					if (const FProperty* Property = Step.GetProperty(); Property && Property->HasMetaData(TEXT("NoBinding"))) {
						Reject(TEXT("smart_object_binding_target_not_bindable"), TEXT("A target path property declares NoBinding metadata."),
							TEXT("target_path"), PropertyFacts(Property), MakeShared<FJsonValueString>(TEXT("path without NoBinding properties"))); return;
					}
				// Parameters are a scoped view of the NoBinding Parameters member.
				if (Source.GetStructID() == Target.GetStructID()) {
					Reject(TEXT("smart_object_binding_target_not_bindable"), TEXT("A binding cannot write back into the same scoped owner used as its source."),
						TEXT("target"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)); return;
				}
			});
			return bValid;
		}
	}

	bool ResolveDefinitionRemovalContext(const TSharedPtr<FJsonObject>& BeforeItems,
		const TSharedPtr<FJsonObject>& DesiredItems, UE::UAI::Contract::FDiagnostic& OutError)
	{
		using namespace UE::UAI::HjsonWorkspace;
		if (!BeforeItems || !DesiredItems) {
			OutError = DocumentFailure(TEXT("smart_object_document_items_invalid"),
				!BeforeItems ? TEXT("The captured native definition has no items object.") : TEXT("The desired definition requires an items object."),
				TEXT("/items"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("items object")));
			if (!BeforeItems) OutError.SourceKind = TEXT("native_result");
			return false;
		}
		TArray<FString> RemovedOwners, RemovedItems;
		TArray<FOwnedItemRemovalCandidate> Candidates;
		for (const auto& Pair : BeforeItems->Values)
		{
			if (!DesiredItems->HasField(Pair.Key)) { RemovedOwners.Add(Pair.Key); continue; }
			const auto Item = Pair.Value->AsObject();
			const FString Kind = Text(Item, TEXT("kind"));
			// Only native containment participates in cascading deletion.
			if (Kind == TEXT("behavior") || Kind == TEXT("definition_data"))
				Candidates.Add({Pair.Key, Text(Item, TEXT("parent"))});
		}
		const auto ItemState = [](const TSharedPtr<FJsonValue>& Value) {
			if (!Value || Value->Type != EJson::Object || !Value->AsObject()->HasField(TEXT("authoring_source"))) return Value;
			const auto State = UeAgentAuthoringState::CloneCanonicalValue(Value);
			State->AsObject()->RemoveField(TEXT("authoring_source"));
			return State;
		};
		if (!TryCollectOwnedItemRemovals(Candidates, RemovedOwners,
			[&](const FString& Id, bool& bUnchanged) {
				bUnchanged = UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(
					ItemState(BeforeItems->TryGetField(Id)), ItemState(DesiredItems->TryGetField(Id)));
				return true;
			}, RemovedItems)) return false; // This callback cannot fail.
		for (const FString& Removed : RemovedItems) DesiredItems->RemoveField(Removed);
		for (const auto& Pair : DesiredItems->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) {
				OutError = DocumentFailure(TEXT("smart_object_document_item_invalid"), TEXT("Each desired definition item must be an object."),
					ItemPointer(Pair.Key), Pair.Value, MakeShared<FJsonValueString>(TEXT("object"))); return false;
			}
			const auto Item = Pair.Value->AsObject();
			FString Linked;
			if (!Item->TryGetStringField(TEXT("linked_slot"), Linked) || Linked.IsEmpty() || DesiredItems->HasField(Linked)) continue;
			const TSharedPtr<FJsonObject>* Previous = nullptr;
			if (!BeforeItems->TryGetObjectField(Pair.Key, Previous) || Text(*Previous, TEXT("linked_slot")) != Linked)
				return ItemFailure(OutError, TEXT("smart_object_slot_link_target_invalid"),
					TEXT("The edited slot link points to an item absent from the desired definition."),
					*Item, Pair.Key, TEXT("linked_slot"), Item->TryGetField(TEXT("linked_slot")), MakeShared<FJsonValueString>(TEXT("retained slot item")));
		}
		return true;
	}

	bool ValidateDefinitionDocument(const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const TSharedPtr<FJsonObject>* ItemsPtr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Bindables = nullptr;
		if (!Document.IsValid() || Document->Type != EJson::Object) {
			OutError = DocumentFailure(TEXT("smart_object_document_invalid"), TEXT("The definition document must be an object."),
				TEXT("/"), Document, MakeShared<FJsonValueString>(TEXT("object"))); return false;
		}
		if (!Document->AsObject()->TryGetObjectField(TEXT("items"), ItemsPtr)) {
			OutError = DocumentFailure(TEXT("smart_object_document_invalid"), TEXT("The definition document requires an items object."),
				TEXT("/items"), Document->AsObject()->TryGetField(TEXT("items")), MakeShared<FJsonValueString>(TEXT("object"))); return false;
		}
		if (!Document->AsObject()->TryGetArrayField(TEXT("bindable_structs"), Bindables)) {
			OutError = DocumentFailure(TEXT("smart_object_document_invalid"), TEXT("The definition document requires a bindable_structs array."),
				TEXT("/bindable_structs"), Document->AsObject()->TryGetField(TEXT("bindable_structs")), MakeShared<FJsonValueString>(TEXT("array"))); return false;
		}
		const auto Items = *ItemsPtr;
		auto IsKind = [&](const FString& Key, const TCHAR* Kind) {
			const TSharedPtr<FJsonObject>* Item = nullptr;
			return Items->TryGetObjectField(Key, Item) && Text(*Item, TEXT("kind")) == Kind;
		};
		for (const TCHAR* Root : {TEXT("root"), TEXT("parameters")})
			if (!IsKind(Root, FCString::Strcmp(Root, TEXT("root")) == 0 ? TEXT("settings") : TEXT("parameters"))) {
				OutError = DocumentFailure(TEXT("smart_object_document_root_missing"),
					FString::Printf(TEXT("The definition requires the '%s' item with its fixed kind."), Root),
					ItemPointer(Root), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueString>(FCString::Strcmp(Root, TEXT("root")) == 0 ? TEXT("settings") : TEXT("parameters"))); return false;
			}
		TMap<FString, TSet<int32>> Orders;
		TMap<FString, FString> OrderOwners, BehaviorTypes;
		for (const auto& Pair : Items->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) {
				OutError = DocumentFailure(TEXT("smart_object_document_item_invalid"), TEXT("Each definition item must be an object."),
					ItemPointer(Pair.Key), Pair.Value, MakeShared<FJsonValueString>(TEXT("object"))); return false;
			}
			const auto Item = Pair.Value->AsObject();
			const FString Kind = Text(Item, TEXT("kind")), Parent = Text(Item, TEXT("parent"));
			const auto Reject = [&](const TCHAR* Code, const FString& Reason, const FString& Field, TSharedPtr<FJsonValue> Expected) {
				return ItemFailure(OutError, Code, Reason, *Item, Pair.Key, Field, Item->TryGetField(Field), MoveTemp(Expected));
			};
			if (Text(Item, TEXT("id")) != Pair.Key)
				return Reject(TEXT("smart_object_document_identity_mismatch"), TEXT("The item id differs from its containing document key."),
					TEXT("id"), MakeShared<FJsonValueString>(TEXT("identity of the containing item")));
			int32 Index = 0;
			if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Item, TEXT("index"), Index, OutError)) {
				LocateItemFailure(OutError, *Item, Pair.Key, TEXT("index")); return false;
			}
			if (!Item->HasField(TEXT("index")) || Index < 0)
				return Reject(TEXT("smart_object_document_order_invalid"), TEXT("The item index must be a present, non-negative integer."),
					TEXT("index"), MakeShared<FJsonValueString>(TEXT("non-negative int32")));
			if (Kind == TEXT("settings") || Kind == TEXT("parameters"))
			{
				FGuid NativeId;
				if (Pair.Key != (Kind == TEXT("settings") ? TEXT("root") : TEXT("parameters")))
					return Reject(TEXT("smart_object_document_root_identity_invalid"), TEXT("Settings and parameters must retain their fixed document key."),
						TEXT("kind"), MakeShared<FJsonValueString>(Kind == TEXT("settings") ? TEXT("root item") : TEXT("parameters item")));
				if (!FGuid::Parse(Text(Item, TEXT("native_id")), NativeId))
					return Reject(TEXT("smart_object_document_root_identity_invalid"), TEXT("The fixed owner has no valid native GUID."),
						TEXT("native_id"), MakeShared<FJsonValueString>(TEXT("valid GUID")));
			}
			else if (Kind == TEXT("slot") || Kind == TEXT("definition_data") || Kind == TEXT("behavior"))
			{
				if (Parent != TEXT("root") && (Kind == TEXT("slot") || !IsKind(Parent, TEXT("slot"))))
					return Reject(TEXT("smart_object_document_parent_invalid"), TEXT("Slots require the root parent; data and behaviors require the root or a retained slot."),
						TEXT("parent"), MakeShared<FJsonValueString>(Kind == TEXT("slot") ? TEXT("root") : TEXT("root or retained slot")));
				if (Kind != TEXT("behavior"))
				{
					FGuid NativeId;
					if (!FGuid::Parse(Pair.Key, NativeId))
						return Reject(TEXT("smart_object_document_identity_invalid"), TEXT("A slot or definition-data item requires a GUID document identity."),
							TEXT("id"), MakeShared<FJsonValueString>(TEXT("valid GUID")));
				}
				const FString ClassPath = Text(Item, TEXT("class_path"));
				if (Kind == TEXT("slot") && ClassPath != FSmartObjectSlotDefinition::StaticStruct()->GetPathName())
					return Reject(TEXT("smart_object_document_slot_type_invalid"), TEXT("A slot item must use the native SmartObject slot definition type."),
						TEXT("class_path"), MakeShared<FJsonValueString>(FSmartObjectSlotDefinition::StaticStruct()->GetPathName()));
				if (Kind == TEXT("definition_data") && !ClassPath.IsEmpty())
				{
					const auto* Type = LoadObject<UScriptStruct>(nullptr, *ClassPath);
					if (!Type || !Type->IsChildOf(FSmartObjectDefinitionData::StaticStruct()))
						return Reject(TEXT("smart_object_document_data_type_invalid"), !Type ? TEXT("The definition-data structure could not be loaded.")
							: TEXT("The selected structure does not derive from SmartObjectDefinitionData."),
							TEXT("class_path"), MakeShared<FJsonValueString>(FSmartObjectDefinitionData::StaticStruct()->GetPathName()));
					if (Type->IsChildOf(FSmartObjectSlotLinkAnnotation::StaticStruct())) {
						FString LinkedSlot; FGuid LinkedId;
						if (!Item->TryGetStringField(TEXT("linked_slot"), LinkedSlot))
							return Reject(TEXT("smart_object_slot_link_target_invalid"), TEXT("A slot-link annotation requires a linked_slot string."),
								TEXT("linked_slot"), MakeShared<FJsonValueString>(TEXT("slot GUID or empty string")));
						if (!IsKind(Parent, TEXT("slot")))
							return Reject(TEXT("smart_object_slot_link_target_invalid"), TEXT("A slot-link annotation must belong to a slot."),
								TEXT("parent"), MakeShared<FJsonValueString>(TEXT("slot item")));
						if (!LinkedSlot.IsEmpty() && (!FGuid::Parse(LinkedSlot, LinkedId) || (Items->HasField(LinkedSlot) && !IsKind(LinkedSlot, TEXT("slot")))))
							return Reject(TEXT("smart_object_slot_link_target_invalid"), TEXT("The link must be a slot GUID and any retained target must be a slot."),
								TEXT("linked_slot"), MakeShared<FJsonValueString>(TEXT("slot GUID or empty string")));
					} else if (Item->HasField(TEXT("linked_slot")))
						return Reject(TEXT("smart_object_slot_link_owner_invalid"), TEXT("Only a slot-link annotation may contain linked_slot."),
							TEXT("linked_slot"), MakeShared<FJsonValueString>(FSmartObjectSlotLinkAnnotation::StaticStruct()->GetPathName()));
				}
				if (Kind == TEXT("behavior"))
				{
					const FString Name = Text(Item, TEXT("object_name"));
					if (Pair.Key != TEXT("behavior:") + Parent + TEXT(":") + FString::FromInt(Index))
						return Reject(TEXT("smart_object_document_behavior_identity_invalid"), TEXT("The behavior identity must match its parent and index."),
							TEXT("index"), MakeShared<FJsonValueString>(TEXT("index encoded in the behavior identity")));
					if (ClassPath.IsEmpty())
					{
						if (!Name.IsEmpty() || Item->HasField(TEXT("properties")))
							return Reject(TEXT("smart_object_document_empty_behavior_invalid"), TEXT("An empty behavior row cannot contain an object name or properties."),
								!Name.IsEmpty() ? TEXT("object_name") : TEXT("properties"), MakeShared<FJsonValueString>(TEXT("empty name and no properties")));
					}
					else
					{
						const auto* Type = LoadObject<UClass>(nullptr, *ClassPath);
						if (!Type || !Type->IsChildOf(USmartObjectBehaviorDefinition::StaticClass())
							|| Type->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
							return Reject(TEXT("smart_object_document_behavior_type_invalid"), !Type ? TEXT("The behavior class could not be loaded.")
								: !Type->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()) ? TEXT("The class does not derive from SmartObjectBehaviorDefinition.")
								: Type->HasAnyClassFlags(CLASS_Abstract) ? TEXT("An abstract behavior class cannot be instantiated.")
								: Type->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("A deprecated behavior class cannot be instantiated.")
								: TEXT("A newer version of the behavior class exists."),
								TEXT("class_path"), MakeShared<FJsonValueString>(TEXT("concrete current SmartObjectBehaviorDefinition class")));
						FText NameError;
						if (Name.IsEmpty() || !FName(*Name).IsValidObjectName(NameError)) {
							Reject(TEXT("smart_object_document_behavior_type_invalid"), Name.IsEmpty() ? TEXT("A non-empty behavior requires an object name.")
								: NameError.ToString(), TEXT("object_name"), MakeShared<FJsonValueString>(TEXT("valid non-empty UObject name"))); return false;
						}
						if (const auto* Existing = BehaviorTypes.Find(Name); Existing && *Existing != ClassPath)
							return Reject(TEXT("smart_object_document_behavior_object_conflict"), TEXT("The same behavior object name is assigned different classes."),
								TEXT("class_path"), MakeShared<FJsonValueString>(*Existing));
						BehaviorTypes.Add(Name, ClassPath);
					}
				}
			}
			else if (Kind != TEXT("binding"))
				return Reject(TEXT("smart_object_document_kind_invalid"), TEXT("The item kind is not supported by a SmartObject definition."),
					TEXT("kind"), MakeShared<FJsonValueString>(TEXT("settings, parameters, slot, definition_data, behavior, or binding")));
			if (Kind != TEXT("settings") && Kind != TEXT("parameters"))
			{
				const FString Group = Kind + TEXT(":") + Parent;
				auto& Order = Orders.FindOrAdd(Group);
				if (Order.Contains(Index))
					return Reject(TEXT("smart_object_document_order_duplicate"), TEXT("Sibling items of the same kind cannot use the same index."),
						TEXT("index"), MakeShared<FJsonValueString>(TEXT("unused sibling index")));
				Order.Add(Index); OrderOwners.FindOrAdd(Group, Pair.Key);
			}
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Item->TryGetObjectField(TEXT("properties"), Properties))
			{
				TArray<TSharedPtr<FJsonValue>> Descriptors;
				if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(*Properties, Descriptors, OutError)) {
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/properties"));
					LocateItemFailure(OutError, *Item, Pair.Key, FString()); return false;
				}
			}
		}
		for (const auto& Order : Orders)
			for (int32 Index = 0; Index < Order.Value.Num(); ++Index)
				if (!Order.Value.Contains(Index)) {
					const FString& Key = OrderOwners.FindChecked(Order.Key);
					return ItemFailure(OutError, TEXT("smart_object_document_order_gap"),
						FString::Printf(TEXT("Sibling indices must be contiguous from zero; index %d is missing."), Index),
						*Items->GetObjectField(Key), Key, TEXT("index"), nullptr, MakeShared<FJsonValueNumber>(Index));
				}
		return true;
	}
	bool ValidateDefinitionBindingsForAuthoring(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& Document, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ValidateDefinitionDocument(Document, OutError) && ValidateForwardBindings(Definition, Document, OutError);
	}

	namespace
	{
	bool ApplyDefinitionDocumentImpl(USmartObjectDefinition& Definition, const TSharedPtr<FJsonValue>& Document,
		UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage, const bool bForwardAuthoring)
	{
		if (!ValidateDefinitionDocument(Document, OutError)) return false;
		FDefinitionCapture Before;
		if (!CaptureDefinition(Definition, Before, OutError)) return false;
		const auto Items = Document->AsObject()->GetObjectField(TEXT("items"));
		TArray<FString> Keys;
		Items->Values.GetKeys(Keys);
		Keys.Sort([&](const FString& A, const FString& B) {
			const int32 Left = Items->GetObjectField(A)->GetIntegerField(TEXT("index"));
			const int32 Right = Items->GetObjectField(B)->GetIntegerField(TEXT("index"));
			return Left != Right ? Left < Right : A < B;
		});
		TMap<FString, USmartObjectBehaviorDefinition*> Behaviors;
		// Resolve all existing object identities before any structural mutation.
		for (const auto& Key : Keys)
		{
			const auto Item = Items->GetObjectField(Key);
			if (Text(Item, TEXT("kind")) != TEXT("behavior") || Text(Item, TEXT("class_path")).IsEmpty()) continue;
			const FString Name = Text(Item, TEXT("object_name"));
			if (auto* Existing = FindObject<UObject>(&Definition, *Name))
			{
			if (Existing->GetClass()->GetPathName() != Text(Item, TEXT("class_path")))
					return ItemFailure(OutError, TEXT("smart_object_document_existing_object_type_changed"),
						TEXT("An existing behavior object cannot be reused with a different class."),
						*Item, Key, TEXT("class_path"), Item->TryGetField(TEXT("class_path")),
						MakeShared<FJsonValueString>(Existing->GetClass()->GetPathName()));
				Behaviors.Add(Name, CastChecked<USmartObjectBehaviorDefinition>(Existing));
			}
		}
		TArray<FSmartObjectSlotDefinition> Slots;
		TMap<FString, int32> SlotIndices;
		for (const auto& Key : Keys)
		{
			const auto Item = Items->GetObjectField(Key);
			if (Text(Item, TEXT("kind")) != TEXT("slot")) continue;
			const auto* Existing = Before.Slots.FindByPredicate([&](const auto& Slot) { return Id(Slot.ID) == Key; });
			auto& Slot = Slots.Add_GetRef(Existing ? *Existing : FSmartObjectSlotDefinition());
			Slot.ID = Guid(Key); Slot.Name = FName(*Text(Item, TEXT("label")));
			Slot.BehaviorDefinitions.Reset(); Slot.DefinitionData.Reset();
			SlotIndices.Add(Key, Slots.Num() - 1);
		}
		TArray<TObjectPtr<USmartObjectBehaviorDefinition>> Defaults;
		TArray<FSmartObjectDefinitionDataProxy> Data;
		TArray<FString> NewBehaviors;
		TMap<FGuid, const FSmartObjectDefinitionDataProxy*> PreviousData;
		if (bForwardAuthoring)
		{
			for (const auto& Proxy : Before.DefinitionData) PreviousData.Add(Proxy.ID, &Proxy);
			for (const auto& Slot : Before.Slots)
				for (const auto& Proxy : Slot.DefinitionData) PreviousData.Add(Proxy.ID, &Proxy);
		}
		for (const auto& Key : Keys)
		{
			const auto Item = Items->GetObjectField(Key);
			const FString Kind = Text(Item, TEXT("kind")), Parent = Text(Item, TEXT("parent"));
			if (Kind == TEXT("behavior"))
			{
				USmartObjectBehaviorDefinition* Behavior = nullptr;
				const FString ClassPath = Text(Item, TEXT("class_path")), Name = Text(Item, TEXT("object_name"));
				if (!ClassPath.IsEmpty())
				{
					Behavior = Behaviors.FindRef(Name);
					if (!Behavior)
					{
						// Keep the new cell empty until its actual customized element handle can vet this class.
						if (bForwardAuthoring)
						{
							NewBehaviors.Add(Key);
							(Parent == TEXT("root") ? Defaults : Slots[SlotIndices.FindChecked(Parent)].BehaviorDefinitions).Add(nullptr);
							continue;
						}
						Behavior = NewObject<USmartObjectBehaviorDefinition>(&Definition,
							LoadObject<UClass>(nullptr, *ClassPath), FName(*Name), RF_Transactional);
						Behaviors.Add(Name, Behavior);
					}
				}
				(Parent == TEXT("root") ? Defaults : Slots[SlotIndices.FindChecked(Parent)].BehaviorDefinitions).Add(Behavior);
			}
			else if (Kind == TEXT("definition_data"))
			{
				FSmartObjectDefinitionDataProxy Proxy;
				Proxy.ID = Guid(Key);
				const FString ClassPath = Text(Item, TEXT("class_path"));
				if (!ClassPath.IsEmpty())
				{
					const auto* Existing = PreviousData.FindRef(Proxy.ID);
					if (Existing && Existing->Data.IsValid() && Existing->Data.GetScriptStruct()->GetPathName() == ClassPath)
						Proxy.Data = Existing->Data;
					else
						Proxy.Data.InitializeAsScriptStruct(LoadObject<UScriptStruct>(nullptr, *ClassPath));
				}
				(Parent == TEXT("root") ? Data : Slots[SlotIndices.FindChecked(Parent)].DefinitionData).Add(MoveTemp(Proxy));
			}
		}
		const auto* SlotsProperty = FindFProperty<FArrayProperty>(Definition.GetClass(), TEXT("Slots"));
		const auto* SlotInner = SlotsProperty ? CastField<FStructProperty>(SlotsProperty->Inner) : nullptr;
		if (!SlotInner || SlotInner->Struct != FSmartObjectSlotDefinition::StaticStruct())
			return NativeFailure(OutError, Definition, TEXT("smart_object_native_layout_invalid"),
				TEXT("The native Slots field must be an array of SmartObjectSlotDefinition structures."),
				TEXT("Slots"), MakeShared<FJsonValueString>(SlotsProperty ? SlotsProperty->GetCPPType(nullptr, 0) : TEXT("missing array property")),
				MakeShared<FJsonValueString>(FSmartObjectSlotDefinition::StaticStruct()->GetPathName()));
		Definition.Modify();
		*SlotsProperty->ContainerPtrToValuePtr<TArray<FSmartObjectSlotDefinition>>(&Definition) = MoveTemp(Slots);
		*FindFProperty<FArrayProperty>(Definition.GetClass(), TEXT("DefaultBehaviorDefinitions"))->ContainerPtrToValuePtr<TArray<TObjectPtr<USmartObjectBehaviorDefinition>>>(&Definition) = MoveTemp(Defaults);
		*FindFProperty<FArrayProperty>(Definition.GetClass(), TEXT("DefinitionData"))->ContainerPtrToValuePtr<TArray<FSmartObjectDefinitionDataProxy>>(&Definition) = MoveTemp(Data);
		*FindFProperty<FStructProperty>(Definition.GetClass(), TEXT("RootID"))->ContainerPtrToValuePtr<FGuid>(&Definition) = Guid(Text(Items->GetObjectField(TEXT("root")), TEXT("native_id")));
		*FindFProperty<FStructProperty>(Definition.GetClass(), TEXT("ParametersID"))->ContainerPtrToValuePtr<FGuid>(&Definition) = Guid(Text(Items->GetObjectField(TEXT("parameters")), TEXT("native_id")));
		for (const FString& Key : NewBehaviors)
		{
			const auto Item = Items->GetObjectField(Key);
			const FString Parent = Text(Item, TEXT("parent"));
			const int32 SlotIndex = Parent == TEXT("root") ? INDEX_NONE : SlotIndices.FindChecked(Parent);
			const int32 Index = Item->GetIntegerField(TEXT("index"));
			const FString ClassPath = Text(Item, TEXT("class_path"));
			auto* Class = LoadObject<UClass>(nullptr, *ClassPath);
			if (!Class || !UE::UAI::QueryOwners::AiBehavior::CanAssignSmartObjectBehavior(Definition, SlotIndex, Index, *Class))
				return ItemFailure(OutError, TEXT("smart_object_behavior_class_unavailable"),
					TEXT("The inline behavior class is unavailable on the destination element's native Details picker."),
					*Item, Key, TEXT("class_path"), Item->TryGetField(TEXT("class_path")), nullptr);
			auto& Array = SlotIndex == INDEX_NONE
				? *FindFieldChecked<FArrayProperty>(Definition.GetClass(), TEXT("DefaultBehaviorDefinitions"))->ContainerPtrToValuePtr<TArray<TObjectPtr<USmartObjectBehaviorDefinition>>>(&Definition)
				: Definition.GetMutableSlot(SlotIndex).BehaviorDefinitions;
			Array[Index] = NewObject<USmartObjectBehaviorDefinition>(&Definition, Class, FName(*Text(Item, TEXT("object_name"))), RF_Transactional);
		}
		if (bForwardAuthoring)
		{
			// Native property notifications must see the desired binding state. Do
			// not overwrite their refreshed descriptors/paths with the before-state.
			return ApplyDefinitionBindingsForAuthoring(Definition, Document, OutError)
				&& ApplyDefinitionDocumentPropertiesForAuthoring(Definition, Document, OutError, TextIdentityPackage);
		}
		return ApplyDefinitionDocumentProperties(Definition, Document, OutError, TextIdentityPackage)
			&& ApplyDefinitionBindingState(Definition, Document, OutError);
	}
	}

	bool ApplyDefinitionDocument(USmartObjectDefinition& Definition, const TSharedPtr<FJsonValue>& Document,
		UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage)
	{
		return ApplyDefinitionDocumentImpl(Definition, Document, OutError, TextIdentityPackage, false);
	}

	bool ApplyDefinitionDocumentForAuthoring(USmartObjectDefinition& Definition,
		const TSharedPtr<FJsonValue>& RequestedDocument, UE::UAI::Contract::FDiagnostic& OutError, UPackage* TextIdentityPackage)
	{
		TSharedPtr<FJsonValue> Before;
		if (!ReadDefinitionDocument(Definition, Before, OutError)) return false;
		if (!RequestedDocument || RequestedDocument->Type != EJson::Object) {
			OutError = DocumentFailure(TEXT("smart_object_document_items_invalid"), TEXT("The requested definition document must be an object."),
				TEXT("/"), RequestedDocument, MakeShared<FJsonValueString>(TEXT("object")));
			OutError.TargetPath = Definition.GetPathName(); return false;
		}
		const auto Document = UeAgentAuthoringState::CloneCanonicalValue(RequestedDocument);
		const TSharedPtr<FJsonObject>* RequestedItems = nullptr;
		if (!Document->AsObject()->TryGetObjectField(TEXT("items"), RequestedItems)) {
			OutError = DocumentFailure(TEXT("smart_object_document_items_invalid"), TEXT("The requested definition requires an items object."),
				TEXT("/items"), Document->AsObject()->TryGetField(TEXT("items")), MakeShared<FJsonValueString>(TEXT("object")));
			OutError.TargetPath = Definition.GetPathName(); return false;
		}
		if (!ResolveDefinitionRemovalContext(Before->AsObject()->GetObjectField(TEXT("items")), *RequestedItems, OutError)
			|| !ValidateDefinitionDocument(Document, OutError)) return false;
		if (!ValidateForwardBindings(Definition, Document, OutError)) return false;
		auto Structure = [](const TSharedPtr<FJsonValue>& Value, const FName Member)
		{
			const auto Items = Value->AsObject()->GetObjectField(TEXT("items"));
			const auto Result = MakeShared<FJsonObject>();
			for (const auto& Pair : Items->Values)
			{
				const auto Item = Pair.Value->AsObject();
				const FString Kind = Text(Item, TEXT("kind")), Parent = Text(Item, TEXT("parent"));
				const bool bIncluded = Member == TEXT("Slots") ? Kind == TEXT("slot")
					|| ((Kind == TEXT("behavior") || Kind == TEXT("definition_data")) && Parent != TEXT("root"))
					: Parent == TEXT("root") && Kind == (Member == TEXT("DefinitionData") ? TEXT("definition_data") : TEXT("behavior"));
				if (!bIncluded) continue;
				const auto Metadata = UeAgentAuthoringState::CloneCanonicalValue(Pair.Value)->AsObject();
				Metadata->RemoveField(TEXT("properties"));
				Metadata->RemoveField(TEXT("authoring_source"));
				Result->SetObjectField(Pair.Key, Metadata);
			}
			return UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueObject>(Result));
		};
		TArray<FName> ChangedMembers;
		for (const FName Member : {FName(TEXT("Slots")), FName(TEXT("DefaultBehaviorDefinitions")), FName(TEXT("DefinitionData"))})
			if (Structure(Before, Member) != Structure(Document, Member)) ChangedMembers.Add(Member);
		FDefinitionCapture NativeBefore;
		if (!CaptureDefinition(Definition, NativeBefore, OutError)) return false;
		TArray<FPropertyBindingPath> RemovedBindingTargets;
		TArray<TPair<FPropertyBindingPath, FPropertyBindingPath>> PreviousBindings;
		bool bHasDesiredBindings = false;
		for (const auto& Pair : Document->AsObject()->GetObjectField(TEXT("items"))->Values)
			bHasDesiredBindings |= Text(Pair.Value->AsObject(), TEXT("kind")) == TEXT("binding");
		NativeBefore.Bindings->ForEachBinding([&](const FPropertyBindingBinding& Binding) {
			RemovedBindingTargets.Add(Binding.GetTargetPath());
			PreviousBindings.Emplace(Binding.GetSourcePath(), Binding.GetTargetPath());
		});
		UeAgentAuthoringObjectApply::FFailure NotificationFailure;
		if (!UeAgentAuthoringObjectApply::NotifyObjectMembers(Definition, ChangedMembers, true, NotificationFailure))
			{ OutError = MoveTemp(NotificationFailure.Diagnostic); return false; }
		const auto PreviousSchema = Definition.GetWorldConditionSchemaClass();
		if (!ApplyDefinitionDocumentImpl(Definition, Document, OutError, TextIdentityPackage, true))
			return false;
		// Slot links use canonical item identity. The shared keyed-item projection
		// already resolved readable names/new items; native code alone derives indices.
		const auto* SlotIdProperty = FindFProperty<FStructProperty>(FSmartObjectSlotReference::StaticStruct(), TEXT("SlotID"));
		if (!SlotIdProperty) return NativeFailure(OutError, Definition, TEXT("smart_object_native_layout_invalid"),
			TEXT("The native SmartObject slot reference has no SlotID structure field."), TEXT("SmartObjectSlotReference.SlotID"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("structure property")));
		const auto Items = Document->AsObject()->GetObjectField(TEXT("items"));
		for (int32 SlotIndex = 0; SlotIndex < Definition.GetSlots().Num(); ++SlotIndex)
			for (auto& Data : Definition.GetMutableSlot(SlotIndex).DefinitionData)
				if (Data.Data.IsValid() && Data.Data.GetScriptStruct()->IsChildOf(FSmartObjectSlotLinkAnnotation::StaticStruct())) {
					const auto Item = Items->GetObjectField(Id(Data.ID));
					auto& Reference = Data.Data.GetMutable<FSmartObjectSlotLinkAnnotation>().LinkedSlot;
					*SlotIdProperty->ContainerPtrToValuePtr<FGuid>(&Reference) = Guid(Text(Item, TEXT("linked_slot")));
				}
		// linked_slot participates in the structural comparison above. The Slots
		// post-edit notification below invokes the native protected reference refresh.
		// Conditions are domain-owned compiled data. A reconstructed/new query has
		// no shared definition until explicitly initialized; this is independent
		// of UObject loading (migration, preload dependencies and load flags).
		const auto Schema = Definition.GetWorldConditionSchemaClass();
		if (!Schema) return NativeFailure(OutError, Definition, TEXT("smart_object_world_condition_schema_required"),
			TEXT("The SmartObject definition has no world-condition schema class."), TEXT("WorldConditionSchemaClass"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(UWorldConditionSchema::StaticClass()->GetPathName()));
		auto* PreconditionsProperty = FindFProperty<FStructProperty>(Definition.GetClass(), TEXT("Preconditions"));
		if (!PreconditionsProperty || PreconditionsProperty->Struct != FWorldConditionQueryDefinition::StaticStruct())
			return NativeFailure(OutError, Definition, TEXT("smart_object_native_layout_invalid"),
				TEXT("The native Preconditions field must be a WorldConditionQueryDefinition structure."),
				TEXT("Preconditions"), MakeShared<FJsonValueString>(PreconditionsProperty ? PreconditionsProperty->GetCPPType(nullptr, 0) : TEXT("missing structure property")),
				MakeShared<FJsonValueString>(FWorldConditionQueryDefinition::StaticStruct()->GetPathName()));
		auto& Preconditions = *PreconditionsProperty->ContainerPtrToValuePtr<FWorldConditionQueryDefinition>(&Definition);
		if (!Preconditions.GetSchemaClass()) Preconditions.SetSchemaClass(Schema);
		if (!InitializeConditionQuery(Preconditions, Definition, TEXT("object.Preconditions"), OutError)) {
			LocateConditionInput(OutError, *Items->GetObjectField(TEXT("root"))); return false;
		}
		for (int32 Index = 0; Index < Definition.GetSlots().Num(); ++Index)
		{
			auto& Conditions = Definition.GetMutableSlot(Index).SelectionPreconditions;
			// Root schema changes govern slot schemas in native Details editing.
			// Replay of an unchanged slot document must not undo that dependency.
			if (Schema != PreviousSchema || !Conditions.GetSchemaClass()) Conditions.SetSchemaClass(Schema);
			if (!InitializeConditionQuery(Conditions, Definition,
				FString::Printf(TEXT("Slots[%d].SelectionPreconditions"), Index), OutError)) {
				LocateConditionInput(OutError, *Items->GetObjectField(Id(Definition.GetSlots()[Index].ID))); return false;
			}
		}
		TArray<TPair<FPropertyBindingPath, FPropertyBindingPath>> ExplicitBindings;
		static_cast<IPropertyBindingBindingCollectionOwner&>(Definition).GetEditorPropertyBindings()->ForEachBinding(
			[&](const FPropertyBindingBinding& Binding) {
				if (!PreviousBindings.ContainsByPredicate([&](const auto& Previous) {
					return Previous.Key == Binding.GetSourcePath() && Previous.Value == Binding.GetTargetPath(); }))
					ExplicitBindings.Emplace(Binding.GetSourcePath(), Binding.GetTargetPath());
			});
		if (!UeAgentAuthoringObjectApply::NotifyObjectMembers(Definition, ChangedMembers, false, NotificationFailure))
			{ OutError = MoveTemp(NotificationFailure.Diagnostic); return false; }
		// Bindings were installed before PostEdit. Do not resurrect context removed
		// by native path refresh; notify only the surviving and actually removed paths.
		if (!ChangedMembers.IsEmpty() || !RemovedBindingTargets.IsEmpty() || bHasDesiredBindings)
		{
			FDefinitionCapture NativeAfter;
			if (!CaptureDefinition(Definition, NativeAfter, OutError)) return false;
			TArray<TPair<FPropertyBindingPath, FPropertyBindingPath>> Paths;
			NativeAfter.Bindings->ForEachBinding([&](const FPropertyBindingBinding& Binding)
				{ Paths.Emplace(Binding.GetSourcePath(), Binding.GetTargetPath()); });
			auto& Owner = static_cast<IPropertyBindingBindingCollectionOwner&>(Definition);
			for (const auto& Path : Paths) Owner.OnPropertyBindingChanged(Path.Key, Path.Value);
			for (const auto& Target : RemovedBindingTargets)
				if (!Owner.GetEditorPropertyBindings()->GetBindingSource(Target))
					Owner.OnPropertyBindingChanged(FPropertyBindingPath(), Target);
			for (const auto& Requested : ExplicitBindings)
			{
				const auto* Actual = Owner.GetEditorPropertyBindings()->GetBindingSource(Requested.Value);
				if (!Actual || *Actual != Requested.Key) {
					const auto Facts = MakeShared<FJsonObject>();
					Facts->SetBoolField(TEXT("binding_present"), Actual != nullptr);
					if (Actual) {
						Facts->SetStringField(TEXT("source_path"), Actual->ToString());
						Facts->SetBoolField(TEXT("same_source_owner"), Actual->GetStructID() == Requested.Key.GetStructID());
					}
					OutError = DocumentFailure(TEXT("smart_object_binding_edit_not_retained"),
						TEXT("Native post-edit refresh did not retain the explicitly requested binding source."),
						TEXT("/target_path"), MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(Requested.Key.ToString()));
					OutError.TargetPath = Definition.GetPathName(); OutError.PropertyPath = TEXT("target_path");
					const FGuid TargetId = Requested.Value.GetStructID();
					const FString TargetItem = TargetId == NativeBefore.RootId ? FString(TEXT("root"))
						: TargetId == NativeBefore.ParametersId ? FString(TEXT("parameters")) : Id(TargetId);
					const FString Key = TEXT("binding:") + TargetItem + TEXT(":") + Requested.Value.ToString();
					const TSharedPtr<FJsonObject>* Input = nullptr;
					if (Items->TryGetObjectField(Key, Input)) LocateItemFailure(OutError, **Input, Key, TEXT("target_path"));
					else OutError.SourceKind = TEXT("native_result");
					return false;
				}
			}
		}
		return true;
	}
}
