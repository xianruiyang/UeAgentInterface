#include "Domains/StateTree/Authoring/UeAgentStateTreeDocument.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeNativeCapture.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeNodeInitialization.h"
#include "Blueprint/StateTreeNodeBlueprintBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorTypes.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "UObject/UnrealType.h"

namespace UeAgentStateTreeAuthoring
{
	namespace
	{
		bool DocumentFailure(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Reason,
			const FString& Pointer, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FJsonObject* Input)
		{
			Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("state_tree_document"));
			Error.PropertyPath = Pointer; Error.Source.InternalJsonPath = Pointer;
			Error.ActualValuePresent = Actual.IsValid(); Error.ActualValue = MoveTemp(Actual); Error.ExpectedValue = MoveTemp(Expected);
			if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input);
			return false;
		}
		void LocateItemFailure(UE::UAI::Contract::FDiagnostic& Error, const FJsonObject& Item, const FString& Key, const FString& Field)
		{
			if (Error.Source.InternalJsonPath.IsEmpty() && Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty())
				Error.Source.InternalJsonPath = TEXT("/") + Field;
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Item, Field);
			if (Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty())
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, TEXT("/items/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Key));
			if (Error.SubjectId.IsEmpty()) Error.SubjectId = Key;
		}
		bool ItemFailure(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Reason,
			const FJsonObject& Item, const FString& Key, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& DetailPointer = FString())
		{
			DocumentFailure(Error, Code, Reason, Field.IsEmpty() ? TEXT("/") : TEXT("/") + Field + DetailPointer,
				MoveTemp(Actual), MoveTemp(Expected), nullptr);
			Error.PropertyPath = Field + DetailPointer;
			LocateItemFailure(Error, Item, Key, Field);
			return false;
		}
		bool NativeFailure(UE::UAI::Contract::FDiagnostic& Error, const UObject& Owner, const TCHAR* Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			DocumentFailure(Error, Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), nullptr);
			Error.SourceKind = TEXT("native_result"); Error.TargetPath = Owner.GetPathName(); return false;
		}
		TSharedPtr<FJsonValue> ItemState(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value || Value->Type != EJson::Object || !Value->AsObject()->HasField(TEXT("authoring_source"))) return Value;
			const auto Result = UeAgentAuthoringState::CloneCanonicalValue(Value);
			Result->AsObject()->RemoveField(TEXT("authoring_source")); return Result;
		}
		FString Text(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
		{ FString Result; Object->TryGetStringField(Key, Result); return Result; }
		FGuid Guid(const FString& TextValue) { FGuid Result; FGuid::Parse(TextValue, Result); return Result; }
		FString Id(const FGuid& Value) { return Value.ToString(EGuidFormats::DigitsWithHyphensLower); }
		bool ReparentObject(UObject& Object, UObject& Outer, UE::UAI::Contract::FDiagnostic& Error, const TCHAR* FailureCode)
		{
			if (Object.GetOuter() == &Outer) return true;
			FName Name = Object.GetFName();
			if (FindObjectFast<UObject>(&Outer, Name))
				Name = MakeUniqueObjectName(&Outer, Object.GetClass(), Name);
			Object.Modify();
			if (Object.Rename(*Name.ToString(), &Outer, REN_DontCreateRedirectors)) return true;
			DocumentFailure(Error, FailureCode, TEXT("Unreal refused to move the native object to its required owner."),
				TEXT("/parent"), MakeShared<FJsonValueString>(GetPathNameSafe(Object.GetOuter())),
				MakeShared<FJsonValueString>(Outer.GetPathName()), nullptr);
			Error.TargetPath = Object.GetPathName(); return false;
		}
		bool ReadPath(const TSharedPtr<FJsonObject>& Item, const TCHAR* Field, FPropertyBindingPath& Path, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (UeAgentPropertyValueCodec::DecodeStructValue(FPropertyBindingPath::StaticStruct(), Item->TryGetField(Field), &Path, Error)) return true;
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/") + FString(Field));
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Item, Field);
			return false;
		}
	}

	bool InitializeTreeAuthoringPreview(UStateTree& Source, UStateTree& Preview, UE::UAI::Contract::FDiagnostic& Error)
	{
		if (&Source == &Preview) return NativeFailure(Error, Source, TEXT("state_tree_preview_is_source"),
			TEXT("Authoring preview must use a different StateTree object from the source."),
			TEXT("preview"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
		TSharedPtr<FJsonValue> Before;
		if (!ReadTreeDocument(Source, Before, Error)) return false;
		if (const auto* SourceData = Cast<UStateTreeEditorData>(Source.EditorData))
		{
			TArray<FStateCapture> States;
			if (!CaptureHierarchy(*SourceData, States, Error)) return false;
			// Native PostDuplicate changes state IDs, but preserves UObject paths and
			// parameter bags. Match those objects before exact replay can replace them.
			for (const auto& Entry : States)
			{
				auto* State = FindObject<UStateTreeState>(&Preview, *Entry.State->GetPathName(&Source));
				if (!State || State->GetClass() != Entry.State->GetClass())
					return NativeFailure(Error, Source, TEXT("state_tree_preview_state_missing"),
						!State ? TEXT("The duplicated preview is missing a source state.") : TEXT("The duplicated preview state has a different class."),
						Entry.State->GetPathName(&Source), MakeShared<FJsonValueString>(State ? State->GetClass()->GetPathName() : TEXT("missing state")),
						MakeShared<FJsonValueString>(Entry.State->GetClass()->GetPathName()));
				State->ID = Entry.State->ID;
			}
		}
		return ApplyTreeDocument(Preview, Before, Error);
	}

	bool NormalizeTreeRemovalContext(const TSharedPtr<FJsonObject>& BeforeItems,
		const TSharedPtr<FJsonObject>& DesiredItems, UE::UAI::Contract::FDiagnostic& Error)
	{
		// Native state removal removes its owned subtree, tasks and transitions;
		// RemoveInvalidBindings drops bindings whose source/target was removed.
		// State links are different: UE retains a dangling GUID for diagnostics.
		bool bRemoved = true;
		while (bRemoved)
		{
			bRemoved = false;
			TArray<FString> RemovedOwners, RemovedItems;
			TArray<UE::UAI::HjsonWorkspace::FOwnedItemRemovalCandidate> Candidates;
			for (const auto& Row : BeforeItems->Values)
			{
				if (!DesiredItems->HasField(Row.Key)) { RemovedOwners.Add(Row.Key); continue; }
				const FString Parent = Text(Row.Value->AsObject(), TEXT("parent"));
				if (!Parent.IsEmpty()) Candidates.Add({Row.Key, Parent});
			}
			if (!UE::UAI::HjsonWorkspace::TryCollectOwnedItemRemovals(Candidates, RemovedOwners,
				[&](const FString& Id, bool& bUnchanged) {
					bUnchanged = UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(
						ItemState(BeforeItems->TryGetField(Id)), ItemState(DesiredItems->TryGetField(Id)));
					return true;
				}, RemovedItems)) return false;
			for (const FString& Id : RemovedItems) DesiredItems->RemoveField(Id);
			bRemoved = !RemovedItems.IsEmpty();
			for (auto It = DesiredItems->Values.CreateIterator(); It; ++It)
			{
				if (!It.Value().IsValid() || It.Value()->Type != EJson::Object) continue;
				const auto Item = It.Value()->AsObject();
				const FString Parent = Text(Item, TEXT("parent"));
				const bool bOwnerRemoved = !Parent.IsEmpty() && BeforeItems->HasField(Parent) && !DesiredItems->HasField(Parent);
				const bool bBindingRemoved = Text(Item, TEXT("kind")) == TEXT("binding")
					&& ((!DesiredItems->HasField(Text(Item, TEXT("source"))) && BeforeItems->HasField(Text(Item, TEXT("source"))))
						|| (!DesiredItems->HasField(Text(Item, TEXT("target"))) && BeforeItems->HasField(Text(Item, TEXT("target")))));
				bool bFunctionBindingRemoved = false;
				if (Text(Item, TEXT("placement")) == TEXT("property_function"))
				{
					const auto HasBinding = [&](const TSharedPtr<FJsonObject>& Items) {
						for (const auto& Row : Items->Values)
							if (Row.Value.IsValid() && Row.Value->Type == EJson::Object
								&& Text(Row.Value->AsObject(), TEXT("kind")) == TEXT("binding")
								&& Text(Row.Value->AsObject(), TEXT("source")) == It.Key()) return true;
						return false;
					};
					bFunctionBindingRemoved = HasBinding(BeforeItems) && !HasBinding(DesiredItems);
				}
				if (!bOwnerRemoved && !bBindingRemoved && !bFunctionBindingRemoved) continue;
				const auto Before = BeforeItems->TryGetField(It.Key());
				if (!Before.IsValid() || UeAgentAuthoringState::MakeStableCanonicalValueKey(ItemState(Before))
					!= UeAgentAuthoringState::MakeStableCanonicalValueKey(ItemState(It.Value())))
				{
					Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("state_tree_edit_conflicts_with_removed_owner_or_endpoint"),
						TEXT("This item was edited while its owner or a required binding endpoint was removed."), TEXT("state_tree_document"));
					Error.SubjectId = It.Key();
					Error.PropertyPath = bOwnerRemoved ? TEXT("parent") : TEXT("binding endpoint");
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("retained owner and binding endpoints, or unchanged item removed with its owner"));
					if (bOwnerRemoved) Error.ActualValue = Item->TryGetField(TEXT("parent"));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Item);
					return false;
				}
				It.RemoveCurrent(); bRemoved = true;
			}
		}
		for (const auto& Row : DesiredItems->Values)
		{
			if (!Row.Value.IsValid() || Row.Value->Type != EJson::Object) continue;
			const auto Item = Row.Value->AsObject();
			const FString Kind = Text(Item, TEXT("kind"));
			for (const TCHAR* Field : {TEXT("linked_state"), TEXT("color_ref"), TEXT("target")})
			{
				if ((Kind == TEXT("state") && FCString::Strcmp(Field, TEXT("target")) == 0)
					|| (Kind == TEXT("transition") && FCString::Strcmp(Field, TEXT("target")) != 0)
					|| (Kind != TEXT("state") && Kind != TEXT("transition"))) continue;
				const FString Target = Text(Item, Field);
				if (Target.IsEmpty() || DesiredItems->HasField(Target)) continue;
				const auto Before = BeforeItems->TryGetField(Row.Key);
				if (!Before.IsValid() || Before->Type != EJson::Object || Text(Before->AsObject(), Field) != Target)
				{
					Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("state_tree_new_link_target_missing"),
						TEXT("/") + FString(Field), MakeShared<FJsonValueString>(TEXT("an existing item key")), MakeShared<FJsonValueString>(Target), TEXT("state_tree_document"));
					Error.PublicMessage = TEXT("The new link refers to an item that is absent from the desired tree.");
					Error.SubjectId = Row.Key;
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Item, Field);
					return false;
				}
			}
		}
		return true;
	}

	bool ApplyTreeDocumentForAuthoring(UStateTree& Tree, const TSharedPtr<FJsonValue>& OriginalValue, UE::UAI::Contract::FDiagnostic& Error,
		UPackage* TextIdentityPackage)
	{
		const auto Value = UeAgentAuthoringState::CloneCanonicalValue(OriginalValue);
		TSharedPtr<FJsonValue> BeforeDocument;
		if (!ReadTreeDocument(Tree, BeforeDocument, Error)) return false;
		const TSharedPtr<FJsonObject>* Items = nullptr;
		if (!Value.IsValid() || Value->Type != EJson::Object)
			return DocumentFailure(Error, TEXT("state_tree_document_invalid"), TEXT("The authoring document must be an object."),
				TEXT("/"), Value, MakeShared<FJsonValueString>(TEXT("object")), nullptr);
		if (!Value->AsObject()->TryGetObjectField(TEXT("items"), Items))
			return DocumentFailure(Error, TEXT("state_tree_document_invalid"), TEXT("The authoring document requires an items object."),
				TEXT("/items"), Value->AsObject()->TryGetField(TEXT("items")), MakeShared<FJsonValueString>(TEXT("object")), Value->AsObject().Get());
		const auto BeforeItems = BeforeDocument->AsObject()->GetObjectField(TEXT("items"));
		if (!NormalizeTreeRemovalContext(BeforeItems, *Items, Error)) return false;
		auto Colors = [](const TSharedPtr<FJsonObject>& Source) {
			auto Result = MakeShared<FJsonObject>();
			for (const auto& Row : Source->Values)
				if (Row.Value.IsValid() && Row.Value->Type == EJson::Object && Text(Row.Value->AsObject(), TEXT("kind")) == TEXT("color"))
					Result->SetField(Row.Key, ItemState(Row.Value));
			return MakeShared<FJsonValueObject>(Result);
		};
		TArray<FName> ChangedMembers;
		if (!UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(Colors(BeforeItems), Colors(*Items)))
			ChangedMembers.Add(GET_MEMBER_NAME_CHECKED(UStateTreeEditorData, Colors));
		UeAgentAuthoringObjectApply::FFailure NotificationFailure;
		if (auto* Data = Cast<UStateTreeEditorData>(Tree.EditorData); Data && !ChangedMembers.IsEmpty()
			&& !UeAgentAuthoringObjectApply::NotifyObjectMembers(*Data, ChangedMembers, true, NotificationFailure))
			{ Error = MoveTemp(NotificationFailure.Diagnostic); return false; }
		if (!ApplyTreeDocument(Tree, Value, Error, TextIdentityPackage)) return false;
		auto* Data = Cast<UStateTreeEditorData>(Tree.EditorData);
		if (!Data) return true;
		// These transient intents are produced by semantic import, never by capture.
		const TSharedPtr<FJsonObject>* Resets = nullptr;
		const TSharedPtr<FJsonObject>* Edits = nullptr;
		Value->AsObject()->TryGetObjectField(TEXT("authoring_parameter_resets"), Resets);
		Value->AsObject()->TryGetObjectField(TEXT("authoring_parameter_edits"), Edits);
		if (Resets)
			for (const auto& Reset : (*Resets)->Values)
				if (!Data->GetStateByID(Guid(Reset.Key)) || !Reset.Value.IsValid() || Reset.Value->Type != EJson::Array)
					return DocumentFailure(Error, TEXT("state_tree_parameter_reset_invalid"),
						!Data->GetStateByID(Guid(Reset.Key)) ? TEXT("The parameter reset owner is not present in the StateTree.")
							: TEXT("Parameter reset names must be supplied as an array."),
						TEXT("/authoring_parameter_resets/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Reset.Key),
						Reset.Value, MakeShared<FJsonValueString>(TEXT("retained state with an array of parameter names")), Value->AsObject().Get());
		TArray<FStateCapture> States;
		if (!CaptureHierarchy(*Data, States, Error)) return false;
		for (const auto& Entry : States)
		{
			auto* State = const_cast<UStateTreeState*>(Entry.State);
			if (State->Type != EStateTreeStateType::Linked && State->Type != EStateTreeStateType::LinkedAsset) continue;
			const TSharedPtr<FJsonObject>* Edit = nullptr;
			if (Edits) (*Edits)->TryGetObjectField(Id(State->ID), Edit);
			if (Edit && (*Edit)->GetBoolField(TEXT("schema")))
				return ItemFailure(Error, TEXT("state_tree_linked_parameter_schema_fixed"),
					TEXT("A linked state's parameter schema is controlled by its linked StateTree and cannot be edited here."),
					*(*Items)->GetObjectField(Id(State->Parameters.ID)), Id(State->Parameters.ID), TEXT("properties"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
			// Keep already decoded requested values for the native comparison below.
			FInstancedPropertyBag Requested;
			if (Edit) Requested = State->Parameters.Parameters;
			const auto Reset = Resets ? (*Resets)->TryGetField(Id(State->ID)) : nullptr;
			if (Reset)
				for (const auto& Name : Reset->AsArray())
				{
					if (!Name.IsValid() || Name->Type != EJson::String)
						return DocumentFailure(Error, TEXT("state_tree_parameter_reset_invalid"),
							TEXT("Each parameter reset entry must be a string naming a parameter."),
							TEXT("/authoring_parameter_resets/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id(State->ID)),
							Name, MakeShared<FJsonValueString>(TEXT("parameter name string")), Value->AsObject().Get());
					if (const auto* Field = State->Parameters.Parameters.FindPropertyDescByName(FName(*Name->AsString())))
						State->SetParametersPropertyOverridden(Field->ID, false);
				}
			// Match native compilation's linked default/schema propagation before the
			// common expected capture. Exact replay and checkpoint restore do not do this.
			State->UpdateParametersFromLinkedSubtree();
			// UE 5.6 RemoveUnusedOverrides only visits a non-null bag. Complete the
			// native empty-set case without changing the fixed-layout flag.
			if (!State->Parameters.Parameters.GetPropertyBagStruct())
				State->Parameters.ResetParametersAndOverrides();
			if (!Edit) continue;
			for (const auto& Name : (*Edit)->GetArrayField(TEXT("fields")))
			{
				const FName FieldName(*Name->AsString());
				const auto* Before = Requested.FindPropertyDescByName(FieldName);
				const auto* After = State->Parameters.Parameters.FindPropertyDescByName(FieldName);
				if (!Before || !After || !Before->CachedProperty || !After->CachedProperty
					|| !Before->CachedProperty->SameType(After->CachedProperty)
					|| !Before->CachedProperty->Identical(
						Before->CachedProperty->ContainerPtrToValuePtr<uint8>(Requested.GetValue().GetMemory()),
						After->CachedProperty->ContainerPtrToValuePtr<uint8>(State->Parameters.Parameters.GetValue().GetMemory())))
				{
					Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("state_tree_parameter_edit_rejected_by_native_authoring"),
						!Before ? TEXT("The requested parameter is absent from the saved requested schema.")
							: !After ? TEXT("Native linked-state refresh removed the edited parameter.")
							: !Before->CachedProperty || !After->CachedProperty ? TEXT("A parameter has no native property descriptor after linked-state refresh.")
							: !Before->CachedProperty->SameType(After->CachedProperty) ? TEXT("Native linked-state refresh changed the edited parameter type.")
							: TEXT("Native linked-state refresh did not retain the requested parameter value."),
						TEXT("state_tree_document"));
					Error.TargetPath = Tree.GetPathName(); Error.PropertyPath = TEXT("parameters.Parameters.") + FieldName.ToString();
					if (Before && After && Before->CachedProperty && After->CachedProperty && Before->CachedProperty->SameType(After->CachedProperty)) {
						const void* Expected = Before->CachedProperty->ContainerPtrToValuePtr<uint8>(Requested.GetValue().GetMemory());
						const void* Actual = After->CachedProperty->ContainerPtrToValuePtr<uint8>(State->Parameters.Parameters.GetValue().GetMemory());
						// Codec comparison/encoding inspect this borrowed native descriptor without changing it.
						auto* ExpectedProperty = const_cast<FProperty*>(Before->CachedProperty);
						auto* ActualProperty = const_cast<FProperty*>(After->CachedProperty);
						const auto Difference = UeAgentPropertyValueCodec::ComparePropertyValues(ExpectedProperty, Expected, Actual, State, {});
						if (!Difference.bEquivalent) {
							Difference.WithPrefix(TEXT("/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(FieldName.ToString())).ApplyTo(Error);
							Error.PropertyPath = TEXT("parameters.Parameters") + Error.PropertyPath;
						} else {
							// Keep native Identical as the acceptance rule, even when public value semantics agree.
							Error.ExpectedValue = UeAgentPropertyValueCodec::Encode(ExpectedProperty, Expected, State, {});
							Error.ActualValue = UeAgentPropertyValueCodec::Encode(ActualProperty, Actual, State, {});
							Error.ActualValuePresent = Error.ActualValue.IsValid();
							const auto Facts = MakeShared<FJsonObject>();
							Facts->SetBoolField(TEXT("native_identical"), false); Facts->SetBoolField(TEXT("public_values_equivalent"), true);
							Error.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
						}
					} else {
						Error.ExpectedValue = MakeShared<FJsonValueString>(Before && Before->CachedProperty ? Before->CachedProperty->GetCPPType() : TEXT("missing requested property"));
						Error.ActualValue = MakeShared<FJsonValueString>(After && After->CachedProperty ? After->CachedProperty->GetCPPType() : TEXT("missing refreshed property"));
						Error.ActualValuePresent = true;
					}
					const FString ParameterId = Id(State->Parameters.ID);
					const auto ParameterItem = (*Items)->GetObjectField(ParameterId);
					const TSharedPtr<FJsonObject>* Properties = nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Descriptors = nullptr;
					if (ParameterItem->TryGetObjectField(TEXT("properties"), Properties) && (*Properties)->TryGetArrayField(TEXT("properties"), Descriptors))
						for (const auto& Descriptor : *Descriptors)
							if (Descriptor->AsObject()->GetStringField(TEXT("property_path")) == TEXT("parameters.Parameters")) {
								UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Descriptor->AsObject(), FieldName.ToString()); break;
							}
					LocateItemFailure(Error, *ParameterItem, ParameterId, TEXT("properties"));
					return false;
				}
			}
		}
		// Native editor notifications propagate linked schemas before compilation
		// removes invalid bindings. Expected capture must observe that same endpoint.
		if (!ChangedMembers.IsEmpty()
			&& !UeAgentAuthoringObjectApply::NotifyObjectMembers(*Data, ChangedMembers, false, NotificationFailure))
			{ Error = MoveTemp(NotificationFailure.Diagnostic); return false; }
		TMap<FGuid, const FPropertyBindingDataView> BindingValues;
		Data->GetAllStructValues(BindingValues);
		Data->GetPropertyEditorBindings()->RemoveInvalidBindings(BindingValues);
		return true;
	}

	bool ValidateTreeDocument(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
	{
		Error.Reset();
		const TSharedPtr<FJsonObject>* ItemsPtr = nullptr;
		bool HasData = false;
		if (!Value.IsValid() || Value->Type != EJson::Object)
			return DocumentFailure(Error, TEXT("state_tree_document_invalid"), TEXT("The StateTree document must be an object."),
				TEXT("/"), Value, MakeShared<FJsonValueString>(TEXT("object")), nullptr);
		if (!Value->AsObject()->TryGetBoolField(TEXT("has_editor_data"), HasData))
			return DocumentFailure(Error, TEXT("state_tree_document_invalid"), TEXT("The has_editor_data field must be a boolean."),
				TEXT("/has_editor_data"), Value->AsObject()->TryGetField(TEXT("has_editor_data")), MakeShared<FJsonValueString>(TEXT("boolean")), Value->AsObject().Get());
		if (!Value->AsObject()->TryGetObjectField(TEXT("items"), ItemsPtr))
			return DocumentFailure(Error, TEXT("state_tree_document_invalid"), TEXT("The StateTree document requires an items object."),
				TEXT("/items"), Value->AsObject()->TryGetField(TEXT("items")), MakeShared<FJsonValueString>(TEXT("object")), Value->AsObject().Get());
		const auto Items = *ItemsPtr;
		FString SchemaPath;
		if (!Value->AsObject()->TryGetStringField(TEXT("schema_class"), SchemaPath))
			return DocumentFailure(Error, TEXT("state_tree_document_schema_missing"), TEXT("The schema_class field must be a string; an empty string selects no schema."),
				TEXT("/schema_class"), Value->AsObject()->TryGetField(TEXT("schema_class")), MakeShared<FJsonValueString>(TEXT("schema class path or empty string")), Value->AsObject().Get());
		if (!SchemaPath.IsEmpty())
		{
			const UClass* SchemaType = LoadObject<UClass>(nullptr, *SchemaPath);
			if (!SchemaType || !SchemaType->IsChildOf(UStateTreeSchema::StaticClass()) || SchemaType->HasAnyClassFlags(CLASS_Abstract))
				return DocumentFailure(Error, TEXT("state_tree_document_schema_invalid"), !SchemaType ? TEXT("The StateTree schema class could not be loaded.")
					: !SchemaType->IsChildOf(UStateTreeSchema::StaticClass()) ? TEXT("The selected schema class does not derive from StateTreeSchema.")
					: TEXT("The selected StateTree schema class is abstract."),
					TEXT("/schema_class"), MakeShared<FJsonValueString>(SchemaPath),
					MakeShared<FJsonValueString>(TEXT("concrete subclass of ") + UStateTreeSchema::StaticClass()->GetPathName()), Value->AsObject().Get());
		}
		if (!HasData) return Items->Values.IsEmpty() || DocumentFailure(Error, TEXT("state_tree_document_data_missing"),
			TEXT("A document without editor data cannot contain editor items."), TEXT("/items"),
			MakeShared<FJsonValueNumber>(Items->Values.Num()), MakeShared<FJsonValueNumber>(0), Value->AsObject().Get());
		TMap<FString, TSharedPtr<FJsonObject>> Specs;
		for (const auto& Pair : Items->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				return DocumentFailure(Error, TEXT("state_tree_document_item_invalid"), TEXT("Each StateTree item must be an object."),
					TEXT("/items/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key), Pair.Value,
					MakeShared<FJsonValueString>(TEXT("object")), Value->AsObject().Get());
			const auto Item = Pair.Value->AsObject();
			const auto Reject = [&](const TCHAR* Code, const FString& Reason, const FString& Field, TSharedPtr<FJsonValue> Expected) {
				return ItemFailure(Error, Code, Reason, *Item, Pair.Key, Field, Item->TryGetField(Field), MoveTemp(Expected));
			};
			const FString Kind = Text(Item, TEXT("kind"));
			if (Text(Item, TEXT("id")) != Pair.Key)
				return Reject(TEXT("state_tree_document_identity_mismatch"), TEXT("The item id differs from its containing document key."),
					TEXT("id"), MakeShared<FJsonValueString>(TEXT("identity of the containing item")));
			if (Kind != TEXT("state") && Kind != TEXT("node") && Kind != TEXT("transition") && Kind != TEXT("parameters")
				&& Kind != TEXT("binding") && Kind != TEXT("binding_source") && Kind != TEXT("settings") && Kind != TEXT("color"))
				return Reject(TEXT("state_tree_document_kind_invalid"), TEXT("The StateTree item kind is unsupported."), TEXT("kind"),
					MakeShared<FJsonValueString>(TEXT("state, node, transition, parameters, binding, binding_source, settings, or color")));
			if (Kind == TEXT("color"))
			{
				FGuid ColorGuid;
				if (!Pair.Key.StartsWith(TEXT("color:")) || !FGuid::Parse(Pair.Key.Mid(6), ColorGuid))
					return Reject(TEXT("state_tree_document_color_identity_invalid"), TEXT("A color item identity must use the color: prefix followed by a GUID."),
						TEXT("id"), MakeShared<FJsonValueString>(TEXT("color:<GUID>")));
			}
			else if (Kind != TEXT("binding") && Kind != TEXT("settings") && !Guid(Pair.Key).IsValid())
				return Reject(TEXT("state_tree_document_identity_invalid"), TEXT("This StateTree item kind requires a valid GUID identity."),
					TEXT("id"), MakeShared<FJsonValueString>(TEXT("valid GUID")));
			int32 Index = 0;
			if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Item, TEXT("index"), Index, Error)) {
				LocateItemFailure(Error, *Item, Pair.Key, TEXT("index")); return false;
			}
			if (!Item->HasField(TEXT("index")) || Index < 0)
				return Reject(TEXT("state_tree_document_order_invalid"), TEXT("The item index must be a present, non-negative integer."),
					TEXT("index"), MakeShared<FJsonValueString>(TEXT("non-negative int32")));
			Specs.Add(Pair.Key, Item);
		}
		auto IsKind = [&](const FString& Key, const TCHAR* Kind) { const auto* Spec = Specs.Find(Key); return Spec && Text(*Spec, TEXT("kind")) == Kind; };
		TSet<FString> ParameterOwners;
		TSet<FString> SingleTaskOwners;
		TArray<FPropertyBindingPath> BindingTargets;
		TSet<FString> BoundFunctions;
		for (const auto& Pair : Specs)
		{
			const auto Item = Pair.Value;
			const FString Kind = Text(Item, TEXT("kind")), Parent = Text(Item, TEXT("parent")), Placement = Text(Item, TEXT("placement"));
			const auto Reject = [&](const TCHAR* Code, const FString& Reason, const FString& Field, TSharedPtr<FJsonValue> Expected) {
				return ItemFailure(Error, Code, Reason, *Item, Pair.Key, Field, Item->TryGetField(Field), MoveTemp(Expected));
			};
			if (Kind == TEXT("state"))
			{
				FString Label, LinkType, Linked;
				int64 LinkTypeValue = 0;
				const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
				for (const TCHAR* Field : {TEXT("label"), TEXT("linked_state"), TEXT("linked_state_type")}) {
					FString* Result = FCString::Strcmp(Field, TEXT("label")) == 0 ? &Label : FCString::Strcmp(Field, TEXT("linked_state")) == 0 ? &Linked : &LinkType;
					if (!Item->TryGetStringField(Field, *Result))
						return Reject(TEXT("state_tree_document_state_fields_invalid"), TEXT("The state field must be a string."),
							Field, MakeShared<FJsonValueString>(TEXT("string")));
				}
				if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(StaticEnum<EStateTreeTransitionType>(), LinkType, LinkTypeValue))
					return Reject(TEXT("state_tree_document_state_fields_invalid"), TEXT("The linked-state type is not a declared transition-type enum member."),
						TEXT("linked_state_type"), MakeShared<FJsonValueString>(StaticEnum<EStateTreeTransitionType>()->GetPathName()));
				if (!Item->TryGetArrayField(TEXT("parameter_overrides"), Overrides))
					return Reject(TEXT("state_tree_document_state_fields_invalid"), TEXT("State parameter overrides must be an array of names."),
						TEXT("parameter_overrides"), MakeShared<FJsonValueString>(TEXT("array of unique non-empty parameter names")));
				TSet<FString> OverrideNames; int32 OverrideIndex = 0;
				for (const auto& Override : *Overrides)
				{
					FString Name;
					if (!Override.IsValid() || !Override->TryGetString(Name) || Name.IsEmpty() || OverrideNames.Contains(Name))
						return ItemFailure(Error, TEXT("state_tree_document_overrides_invalid"),
							OverrideNames.Contains(Name) ? TEXT("The parameter override name occurs more than once.")
								: TEXT("Each parameter override must be a non-empty string."),
							*Item, Pair.Key, TEXT("parameter_overrides"), Override,
							MakeShared<FJsonValueString>(TEXT("unique non-empty parameter name")), TEXT("/") + FString::FromInt(OverrideIndex));
					OverrideNames.Add(Name); ++OverrideIndex;
				}
				TSet<FString> Ancestors{Pair.Key};
				FString Current = Parent;
				while (!Current.IsEmpty())
				{
					if (!IsKind(Current, TEXT("state")) || Ancestors.Contains(Current))
						return Reject(TEXT("state_tree_document_hierarchy_invalid"), !IsKind(Current, TEXT("state"))
							? TEXT("The parent chain contains an absent item or an item that is not a state.") : TEXT("The state parent chain contains a cycle."),
							TEXT("parent"), MakeShared<FJsonValueString>(TEXT("acyclic chain of retained states")));
					Ancestors.Add(Current); Current = Text(Specs.FindChecked(Current), TEXT("parent"));
				}
				if (!Linked.IsEmpty() && (!Guid(Linked).IsValid() || (Specs.Contains(Linked) && !IsKind(Linked, TEXT("state")))))
					return Reject(TEXT("state_tree_document_link_invalid"), !Guid(Linked).IsValid()
						? TEXT("The linked-state identity is not a valid GUID.") : TEXT("The retained linked target is not a state."),
						TEXT("linked_state"), MakeShared<FJsonValueString>(TEXT("state GUID or empty string")));
				FString Color;
				if (!Item->TryGetStringField(TEXT("color_ref"), Color) || (!Color.IsEmpty()
					&& (!Color.StartsWith(TEXT("color:")) || !Guid(Color.Mid(6)).IsValid()
						|| (Specs.Contains(Color) && !IsKind(Color, TEXT("color"))))))
					return Reject(TEXT("state_tree_document_color_reference_invalid"), TEXT("The color reference must be empty or use color:<GUID>, and any retained target must be a color."),
						TEXT("color_ref"), MakeShared<FJsonValueString>(TEXT("color:<GUID> or empty string")));
			}
			else if (Kind == TEXT("color"))
			{
				FString Label;
				if (!Parent.IsEmpty() || !Item->TryGetStringField(TEXT("label"), Label))
					return Reject(TEXT("state_tree_document_color_fields_invalid"), !Parent.IsEmpty()
						? TEXT("Palette colors cannot have a parent.") : TEXT("A color label must be a string."),
						!Parent.IsEmpty() ? TEXT("parent") : TEXT("label"), MakeShared<FJsonValueString>(!Parent.IsEmpty() ? TEXT("empty parent") : TEXT("string")));
			}
			else if (Kind == TEXT("parameters"))
			{
				if ((!Parent.IsEmpty() && !IsKind(Parent, TEXT("state"))) || ParameterOwners.Contains(Parent))
					return Reject(TEXT("state_tree_document_parameters_invalid"), !Parent.IsEmpty() && !IsKind(Parent, TEXT("state"))
						? TEXT("The parameter owner must be a retained state or the editor root.") : TEXT("An owner cannot have more than one parameters item."),
						TEXT("parent"), MakeShared<FJsonValueString>(TEXT("root or retained state with one parameters item")));
				ParameterOwners.Add(Parent);
			}
			else if (Kind == TEXT("transition"))
			{
				FString TransitionType;
				int64 TransitionTypeValue = 0;
				if (!Item->TryGetStringField(TEXT("transition_type"), TransitionType)
					|| !UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(StaticEnum<EStateTreeTransitionType>(), TransitionType, TransitionTypeValue))
					return Reject(TEXT("state_tree_document_transition_type_invalid"), TEXT("The transition type must name a declared transition-type enum member."),
						TEXT("transition_type"), MakeShared<FJsonValueString>(StaticEnum<EStateTreeTransitionType>()->GetPathName()));
				const FString Target = Text(Item, TEXT("target"));
				if (!IsKind(Parent, TEXT("state")) || (!Target.IsEmpty() && (!Guid(Target).IsValid()
					|| (Specs.Contains(Target) && !IsKind(Target, TEXT("state"))))))
					return Reject(TEXT("state_tree_document_transition_invalid"), !IsKind(Parent, TEXT("state"))
						? TEXT("A transition must belong to a retained state.") : !Guid(Target).IsValid()
						? TEXT("The transition target is not a valid state GUID.") : TEXT("The retained transition target is not a state."),
						!IsKind(Parent, TEXT("state")) ? TEXT("parent") : TEXT("target"),
						MakeShared<FJsonValueString>(!IsKind(Parent, TEXT("state")) ? TEXT("retained state") : TEXT("state GUID or empty target")));
			}
			else if (Kind == TEXT("node"))
			{
				const bool Global = Placement == TEXT("evaluator") || Placement == TEXT("global_task");
				const bool StateNode = Placement == TEXT("task") || Placement == TEXT("single_task") || Placement == TEXT("enter_condition") || Placement == TEXT("consideration");
				if (!((Global && Parent.IsEmpty()) || (StateNode && IsKind(Parent, TEXT("state")))
					|| (Placement == TEXT("transition_condition") && IsKind(Parent, TEXT("transition")))
					|| (Placement == TEXT("property_function") && Specs.Contains(Parent))))
					return Reject(TEXT("state_tree_document_placement_invalid"), TEXT("The node placement and parent do not form a supported owner relationship."),
						TEXT("placement"), MakeShared<FJsonValueString>(TEXT("global node without parent, state node under state, condition under transition, or property function under retained target")));
				if (Placement == TEXT("single_task"))
				{
					if (SingleTaskOwners.Contains(Parent))
						return Reject(TEXT("state_tree_document_single_task_duplicate"), TEXT("A state can contain only one single_task node."),
							TEXT("placement"), MakeShared<FJsonValueString>(TEXT("one single_task per state")));
					SingleTaskOwners.Add(Parent);
				}
				const auto* Type = LoadObject<UScriptStruct>(nullptr, *Text(Item, TEXT("node_type")));
				if (!Type || !Type->IsChildOf(FStateTreeNodeBase::StaticStruct()))
					return Reject(TEXT("state_tree_document_node_type_invalid"), !Type ? TEXT("The node structure could not be loaded.")
						: TEXT("The node structure does not derive from StateTreeNodeBase."),
						TEXT("node_type"), MakeShared<FJsonValueString>(FStateTreeNodeBase::StaticStruct()->GetPathName()));
				FString InstancePath, ObjectClassPath;
				FString NodeName;
				if (!Item->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.Len() >= NAME_SIZE)
					return Reject(TEXT("state_tree_document_node_name_invalid"), TEXT("The node name must be a string shorter than the native FName limit."),
						TEXT("node_name"), MakeShared<FJsonValueString>(FString::Printf(TEXT("string shorter than %d characters"), NAME_SIZE)));
				if (!Item->TryGetStringField(TEXT("instance_type"), InstancePath))
					return Reject(TEXT("state_tree_document_instance_type_missing"), TEXT("The node instance_type field must be a string."),
						TEXT("instance_type"), MakeShared<FJsonValueString>(TEXT("structure path or empty string")));
				if (!Item->TryGetStringField(TEXT("instance_object_class"), ObjectClassPath))
					return Reject(TEXT("state_tree_document_instance_type_missing"), TEXT("The node instance_object_class field must be a string."),
						TEXT("instance_object_class"), MakeShared<FJsonValueString>(TEXT("class path or empty string")));
				if (!InstancePath.IsEmpty() && !LoadObject<UScriptStruct>(nullptr, *InstancePath))
					return Reject(TEXT("state_tree_document_instance_type_invalid"), TEXT("The node instance structure could not be loaded."),
						TEXT("instance_type"), MakeShared<FJsonValueString>(TEXT("loadable script structure")));
				if (!ObjectClassPath.IsEmpty())
				{
					const auto* Class = LoadObject<UClass>(nullptr, *ObjectClassPath);
					if (!Class || Class->HasAnyClassFlags(CLASS_Abstract))
						return Reject(TEXT("state_tree_document_instance_class_invalid"), !Class ? TEXT("The node instance class could not be loaded.")
							: TEXT("The node instance class is abstract."), TEXT("instance_object_class"), MakeShared<FJsonValueString>(TEXT("loadable non-abstract class")));
				}
			}
			else if (Kind == TEXT("binding"))
			{
				FPropertyBindingPath Source, Target;
				if (!ReadPath(Item, TEXT("source_path_state"), Source, Error) || !ReadPath(Item, TEXT("target_path_state"), Target, Error)) {
					LocateItemFailure(Error, *Item, Pair.Key, FString()); return false;
				}
				if (BindingTargets.Contains(Target))
					return Reject(TEXT("state_tree_document_binding_target_duplicate"), TEXT("More than one binding writes to the same target property."),
						TEXT("target_path"), MakeShared<FJsonValueString>(TEXT("one binding per target property")));
				BindingTargets.Add(Target);
				if (!Specs.Contains(Id(Source.GetStructID())))
					return Reject(TEXT("state_tree_document_binding_endpoint_invalid"), TEXT("The decoded source path refers to an item absent from the desired document."),
						TEXT("source"), MakeShared<FJsonValueString>(TEXT("retained source item")));
				if (!Specs.Contains(Id(Target.GetStructID())))
					return Reject(TEXT("state_tree_document_binding_endpoint_invalid"), TEXT("The decoded target path refers to an item absent from the desired document."),
						TEXT("target"), MakeShared<FJsonValueString>(TEXT("retained target item")));
				if (Text(Item, TEXT("source")) != Id(Source.GetStructID()))
					return Reject(TEXT("state_tree_document_binding_endpoint_invalid"), TEXT("The source endpoint differs from the owner encoded in source_path_state."),
						TEXT("source"), MakeShared<FJsonValueString>(TEXT("same source owner as the decoded path")));
				if (Text(Item, TEXT("target")) != Id(Target.GetStructID()))
					return Reject(TEXT("state_tree_document_binding_endpoint_invalid"), TEXT("The target endpoint differs from the owner encoded in target_path_state."),
						TEXT("target"), MakeShared<FJsonValueString>(TEXT("same target owner as the decoded path")));
				const FString SourceId = Id(Source.GetStructID());
				const auto SourceItem = Specs.FindChecked(SourceId);
				if (Text(SourceItem, TEXT("placement")) == TEXT("property_function"))
				{
					if (BoundFunctions.Contains(SourceId) || Text(SourceItem, TEXT("parent")) != Id(Target.GetStructID()))
						return Reject(TEXT("state_tree_function_binding_owner_invalid"), BoundFunctions.Contains(SourceId)
							? TEXT("A property function cannot be used as the source of multiple bindings.")
							: TEXT("The property-function owner differs from the binding target owner."),
							TEXT("source"), MakeShared<FJsonValueString>(TEXT("one binding using the function's target owner")));
					BoundFunctions.Add(SourceId);
				}
			}
			else if (Kind == TEXT("settings") && Pair.Key != TEXT("editor_settings") && Pair.Key != TEXT("schema_settings"))
				return Reject(TEXT("state_tree_document_settings_invalid"), TEXT("A settings item must use an editor_settings or schema_settings identity."),
					TEXT("id"), MakeShared<FJsonValueString>(TEXT("editor_settings or schema_settings")));
		}
		if (!ParameterOwners.Contains(FString()))
			return DocumentFailure(Error, TEXT("state_tree_document_root_parameters_missing"), TEXT("The editor root requires one parameters item."),
				TEXT("/items"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Value->AsObject().Get());
		for (const auto& Pair : Specs)
		{
			if (Text(Pair.Value, TEXT("placement")) != TEXT("property_function")) continue;
			if (!BoundFunctions.Contains(Pair.Key))
				return ItemFailure(Error, TEXT("state_tree_function_binding_missing"), TEXT("The property-function node is not used by a binding."),
					*Pair.Value, Pair.Key, TEXT("placement"), Pair.Value->TryGetField(TEXT("placement")), MakeShared<FJsonValueString>(TEXT("property function used by one binding")));
			TSet<FString> Ancestors;
			FString Current = Pair.Key;
			while (Text(Specs.FindChecked(Current), TEXT("placement")) == TEXT("property_function"))
			{
				if (Ancestors.Contains(Current))
					return ItemFailure(Error, TEXT("state_tree_function_binding_cycle"), TEXT("The property-function binding owner chain contains a cycle."),
						*Pair.Value, Pair.Key, TEXT("parent"), Pair.Value->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("acyclic property-function chain")));
				Ancestors.Add(Current);
				Current = Text(Specs.FindChecked(Current), TEXT("parent"));
			}
		}
		for (const auto& Pair : Specs)
			if (Text(Pair.Value, TEXT("kind")) == TEXT("state") && !ParameterOwners.Contains(Pair.Key))
				return ItemFailure(Error, TEXT("state_tree_document_state_parameters_missing"), TEXT("The state requires one parameters item."),
					*Pair.Value, Pair.Key, TEXT("kind"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		return true;
	}

	bool ApplyTreeDocument(UStateTree& Tree, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error, UPackage* TextIdentityPackage)
	{
		if (!ValidateTreeDocument(Value, Error)) {
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Tree.GetPathName();
			return false;
		}
		const auto Document = Value->AsObject();
		Tree.Modify();
		if (!Document->GetBoolField(TEXT("has_editor_data"))) { Tree.EditorData = nullptr; return true; }
		auto* Data = Cast<UStateTreeEditorData>(Tree.EditorData);
		if (!Data) { Data = NewObject<UStateTreeEditorData>(&Tree, NAME_None, RF_Transactional); Tree.EditorData = Data; }
		Data->Modify();
		const FString SchemaClass = Text(Document, TEXT("schema_class"));
		if (!SchemaClass.IsEmpty() && (!Data->Schema || Data->Schema->GetClass()->GetPathName() != SchemaClass))
		{
			UClass* Class = LoadObject<UClass>(nullptr, *SchemaClass);
			if (!Class || !Class->IsChildOf(UStateTreeSchema::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
				return DocumentFailure(Error, TEXT("state_tree_document_schema_invalid"), !Class ? TEXT("The schema class could not be loaded.")
					: !Class->IsChildOf(UStateTreeSchema::StaticClass()) ? TEXT("The schema class does not derive from StateTreeSchema.")
					: TEXT("The schema class is abstract."), TEXT("/schema_class"), Document->TryGetField(TEXT("schema_class")),
					MakeShared<FJsonValueString>(TEXT("concrete StateTreeSchema subclass")), Document.Get());
			Data->Schema = NewObject<UStateTreeSchema>(Data, Class, NAME_None, RF_Transactional);
		}
		else if (SchemaClass.IsEmpty()) Data->Schema = nullptr;
		FTreeCapture Before;
		if (!CaptureTree(*Data, Before, Error)) return false;
		TMap<FString, UStateTreeState*> States;
		TMap<FString, FStateTreeEditorNode> Nodes;
		TMap<FString, FStateTreeTransition> Transitions;
		for (const auto& Entry : Before.States) States.Add(Id(Entry.State->ID), const_cast<UStateTreeState*>(Entry.State));
		for (const auto& Entry : Before.Nodes) Nodes.Add(Id(Entry.Node->ID), *Entry.Node);
		for (const auto& Entry : Before.Transitions) Transitions.Add(Id(Entry.Transition->ID), *Entry.Transition);
		const auto Items = Document->GetObjectField(TEXT("items"));
		TArray<FString> Keys;
		Items->Values.GetKeys(Keys);
		Keys.Sort([&](const FString& A, const FString& B) {
			const int32 Left = Items->GetObjectField(A)->GetIntegerField(TEXT("index")), Right = Items->GetObjectField(B)->GetIntegerField(TEXT("index"));
			return Left == Right ? A < B : Left < Right;
		});
		// Resolve parents first, following the same Outer ownership as native AddChildState.
		TFunction<UStateTreeState*(const FString&)> ResolveState = [&](const FString& Key) -> UStateTreeState* {
			if (Key.IsEmpty()) return nullptr;
			const auto Spec = Items->GetObjectField(Key);
			UStateTreeState* Parent = ResolveState(Text(Spec, TEXT("parent")));
			UObject* Outer = Parent ? static_cast<UObject*>(Parent) : Data;
			UStateTreeState* State = States.FindRef(Key);
			if (!State) { State = NewObject<UStateTreeState>(Outer, NAME_None, RF_Transactional); State->ID = Guid(Key); States.Add(Key, State); }
			State->Modify();
			if (!ReparentObject(*State, *Outer, Error, TEXT("state_tree_document_state_create_failed"))) {
				LocateItemFailure(Error, *Spec, Key, TEXT("parent"));
				if (!Error.Source.SourceFile.IsEmpty() || !Error.Source.SourceUnit.IsEmpty()) Error.SourceKind = TEXT("related_input");
				return nullptr;
			}
			State->Parent = Parent;
			State->Name = FName(*Text(Spec, TEXT("label")));
			return State;
		};
		for (const auto& Key : Keys)
			if (Text(Items->GetObjectField(Key), TEXT("kind")) == TEXT("state") && !ResolveState(Key)) return false;
		Data->SubTrees.Reset(); Data->Evaluators.Reset(); Data->GlobalTasks.Reset();
		Data->Colors.Reset();
		for (auto& Pair : States)
		{
			Pair.Value->Children.Reset(); Pair.Value->Tasks.Reset(); Pair.Value->SingleTask = {};
			Pair.Value->EnterConditions.Reset(); Pair.Value->Considerations.Reset(); Pair.Value->Transitions.Reset();
		}
		for (const auto& Key : Keys)
		{
			const auto Spec = Items->GetObjectField(Key);
			const FString Kind = Text(Spec, TEXT("kind")), ParentId = Text(Spec, TEXT("parent"));
			if (Kind == TEXT("state"))
			{
				auto* State = States.FindChecked(Key);
				if (auto* Parent = States.FindRef(ParentId)) Parent->Children.Add(State); else Data->SubTrees.Add(State);
			}
			else if (Kind == TEXT("color"))
			{
				FStateTreeEditorColor Color(FStateTreeEditorColorRef(Guid(Key.Mid(6))));
				Color.DisplayName = Text(Spec, TEXT("label"));
				Data->Colors.Add(Color);
			}
			else if (Kind == TEXT("parameters"))
			{
				if (auto* State = States.FindRef(ParentId)) State->Parameters.ID = Guid(Key);
				else
				{
					const auto* Property = FindFProperty<FStructProperty>(Data->GetClass(), TEXT("RootParametersGuid"));
					if (!Property) return NativeFailure(Error, Tree, TEXT("state_tree_document_root_identity_missing"),
						TEXT("The native editor data has no RootParametersGuid structure property."), TEXT("RootParametersGuid"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("FGuid structure property")));
					*Property->ContainerPtrToValuePtr<FGuid>(Data) = Guid(Key);
				}
			}
			else if (Kind == TEXT("transition"))
			{
				auto& Transition = Transitions.FindOrAdd(Key); Transition.ID = Guid(Key); Transition.Conditions.Reset();
			}
			else if (Kind == TEXT("node"))
			{
				const auto* NodeType = LoadObject<UScriptStruct>(nullptr, *Text(Spec, TEXT("node_type")));
				const FString InstancePath = Text(Spec, TEXT("instance_type")), ObjectPath = Text(Spec, TEXT("instance_object_class"));
				if (const auto* Existing = Nodes.Find(Key))
				{
					if (Existing->Node.GetScriptStruct() != NodeType)
						return ItemFailure(Error, TEXT("state_tree_document_existing_node_type_changed"), TEXT("An existing node cannot change its native node structure."),
							*Spec, Key, TEXT("node_type"), Spec->TryGetField(TEXT("node_type")), MakeShared<FJsonValueString>(GetPathNameSafe(Existing->Node.GetScriptStruct())));
					if ((Existing->Instance.IsValid() ? Existing->Instance.GetScriptStruct()->GetPathName() : FString()) != InstancePath)
						return ItemFailure(Error, TEXT("state_tree_document_existing_node_type_changed"), TEXT("An existing node cannot change its instance structure."),
							*Spec, Key, TEXT("instance_type"), Spec->TryGetField(TEXT("instance_type")),
							MakeShared<FJsonValueString>(Existing->Instance.IsValid() ? Existing->Instance.GetScriptStruct()->GetPathName() : FString()));
					if ((Existing->InstanceObject ? Existing->InstanceObject->GetClass()->GetPathName() : FString()) != ObjectPath)
						return ItemFailure(Error, TEXT("state_tree_document_existing_node_type_changed"), TEXT("An existing node cannot change its instance object class."),
							*Spec, Key, TEXT("instance_object_class"), Spec->TryGetField(TEXT("instance_object_class")),
							MakeShared<FJsonValueString>(Existing->InstanceObject ? Existing->InstanceObject->GetClass()->GetPathName() : FString()));
					continue;
				}
				auto& Node = Nodes.Add(Key);
				UObject* NodeOwner = States.FindRef(ParentId);
				if (Text(Spec, TEXT("placement")) == TEXT("transition_condition"))
					NodeOwner = States.FindRef(Text(Items->GetObjectField(ParentId), TEXT("parent")));
				UClass* InstanceClass = ObjectPath.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *ObjectPath);
				const bool BlueprintInstance = InstanceClass && InstanceClass->IsChildOf(UStateTreeNodeBlueprintBase::StaticClass());
				if (!InitializeNode(Node, *(NodeOwner ? NodeOwner : Data), BlueprintInstance ? static_cast<const UStruct*>(InstanceClass) : NodeType, Error)) {
					const FString Field = BlueprintInstance ? TEXT("instance_object_class") : TEXT("node_type");
					Error.Source.InternalJsonPath = TEXT("/") + Field;
					LocateItemFailure(Error, *Spec, Key, Field);
					if (Error.TargetPath.IsEmpty()) Error.TargetPath = Tree.GetPathName(); return false;
				}
				Node.ID = Guid(Key);
				if (!InstancePath.IsEmpty()) Node.Instance.InitializeAs(LoadObject<UScriptStruct>(nullptr, *InstancePath));
				if (!ObjectPath.IsEmpty() && !Node.InstanceObject)
				{
					Node.InstanceObject = NewObject<UObject>(NodeOwner ? NodeOwner : Data,
						InstanceClass, NAME_None, RF_Transactional);
				}
			}
		}
		for (const auto& Key : Keys)
		{
			const auto Spec = Items->GetObjectField(Key);
			if (Text(Spec, TEXT("kind")) != TEXT("node")) continue;
			const FString Placement = Text(Spec, TEXT("placement")), Parent = Text(Spec, TEXT("parent"));
			auto& Node = Nodes.FindChecked(Key);
			// A node can move without its former state. Preserve its instance identity
			// while giving it the same ownership as a freshly initialized native node.
			UObject* NodeOwner = States.FindRef(Parent);
			if (Placement == TEXT("transition_condition"))
				NodeOwner = States.FindRef(Text(Items->GetObjectField(Parent), TEXT("parent")));
			if (Node.InstanceObject && !ReparentObject(*Node.InstanceObject, *(NodeOwner ? NodeOwner : Data), Error, TEXT("state_tree_document_node_reparent_failed"))) {
				LocateItemFailure(Error, *Spec, Key, TEXT("parent"));
				if (!Error.Source.SourceFile.IsEmpty() || !Error.Source.SourceUnit.IsEmpty()) Error.SourceKind = TEXT("related_input");
				return false;
			}
			Node.Node.GetMutable<FStateTreeNodeBase>().Name = FName(*Text(Spec, TEXT("node_name")));
			if (Placement == TEXT("evaluator")) Data->Evaluators.Add(Node);
			else if (Placement == TEXT("global_task")) Data->GlobalTasks.Add(Node);
			else if (Placement == TEXT("transition_condition")) Transitions.FindChecked(Parent).Conditions.Add(Node);
			else if (Placement != TEXT("property_function"))
			{
				auto* State = States.FindChecked(Parent);
				if (Placement == TEXT("task")) State->Tasks.Add(Node);
				else if (Placement == TEXT("single_task")) State->SingleTask = Node;
				else if (Placement == TEXT("enter_condition")) State->EnterConditions.Add(Node);
				else if (Placement == TEXT("consideration")) State->Considerations.Add(Node);
			}
		}
		for (const auto& Key : Keys)
		{
			const auto Spec = Items->GetObjectField(Key);
			if (Text(Spec, TEXT("kind")) == TEXT("transition")) States.FindChecked(Text(Spec, TEXT("parent")))->Transitions.Add(Transitions.FindChecked(Key));
		}
		Data->EditorBindings = {};
		for (const auto& Key : Keys)
		{
			const auto Spec = Items->GetObjectField(Key);
			if (Text(Spec, TEXT("kind")) != TEXT("binding")) continue;
			FPropertyBindingPath Source, Target;
			if (!ReadPath(Spec, TEXT("source_path_state"), Source, Error) || !ReadPath(Spec, TEXT("target_path_state"), Target, Error)) {
				LocateItemFailure(Error, *Spec, Key, FString()); return false;
			}
			const FString SourceId = Id(Source.GetStructID());
			const auto SourceSpec = Items->GetObjectField(SourceId);
			if (Text(SourceSpec, TEXT("placement")) == TEXT("property_function"))
				Data->EditorBindings.AddStateTreeBinding(FStateTreePropertyPathBinding(FConstStructView::Make(Nodes.FindChecked(SourceId)), Source, Target));
			else Data->EditorBindings.AddBinding(Source, Target);
		}
		if (!ApplyTreeDocumentProperties(Tree, Value, Error, TextIdentityPackage)) return false;
		auto SetLink = [&](FStateTreeStateLink& Link, const TSharedPtr<FJsonObject>& Spec, const FString& Key,
			const TCHAR* TargetField, const TCHAR* TypeField, const TCHAR* FailureCode) {
			const FString TargetId = Text(Spec, TargetField);
			const FGuid TargetGuid = Guid(TargetId);
			if (States.Contains(TargetId)) Link.Name = States.FindChecked(TargetId)->Name;
			else if (Link.ID != TargetGuid || !TargetGuid.IsValid()) Link.Name = NAME_None;
			Link.ID = TargetGuid;
			int64 Type = 0;
			if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(StaticEnum<EStateTreeTransitionType>(), Text(Spec, TypeField), Type))
				return ItemFailure(Error, FailureCode, TEXT("The link type must name a declared transition-type enum member."),
					*Spec, Key, TypeField, Spec->TryGetField(TypeField), MakeShared<FJsonValueString>(StaticEnum<EStateTreeTransitionType>()->GetPathName()));
			Link.LinkType = static_cast<EStateTreeTransitionType>(Type); return true;
		};
		for (const auto& Key : Keys)
		{
			const auto Spec = Items->GetObjectField(Key);
			if (Text(Spec, TEXT("kind")) != TEXT("state")) continue;
			auto* State = States.FindChecked(Key);
			State->ColorRef = FStateTreeEditorColorRef(Guid(Text(Spec, TEXT("color_ref")).Mid(6)));
			if (!SetLink(State->LinkedSubtree, Spec, Key, TEXT("linked_state"), TEXT("linked_state_type"), TEXT("state_tree_document_link_type_invalid"))) return false;
			State->Parameters.PropertyOverrides.Reset();
			for (const auto& Override : Spec->GetArrayField(TEXT("parameter_overrides")))
			{
				const auto* Field = State->Parameters.Parameters.FindPropertyDescByName(FName(*Override->AsString()));
				if (!Field) return ItemFailure(Error, TEXT("state_tree_document_parameter_override_missing"),
					TEXT("The parameter override names a field absent from this state's parameter bag."),
					*Spec, Key, TEXT("parameter_overrides"), Override, MakeShared<FJsonValueString>(TEXT("existing parameter name")));
				State->Parameters.PropertyOverrides.Add(Field->ID);
			}
			for (auto& Transition : State->Transitions)
				if (!SetLink(Transition.State, Items->GetObjectField(Id(Transition.ID)), Id(Transition.ID), TEXT("target"), TEXT("transition_type"),
					TEXT("state_tree_document_transition_type_invalid"))) return false;
		}
		return true;
	}
}
