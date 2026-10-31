#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Utilities/UeAgentReadableName.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringContentLayout.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringExamples.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Containers/StringConv.h"
#include "CoreGlobals.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	bool FCompositionSnapshotBinding::IsValid() const
	{
		return !SnapshotId.TrimStartAndEnd().IsEmpty()
			&& Revision.IsValid()
			&& !EvidenceId.TrimStartAndEnd().IsEmpty();
	}

	namespace
	{
		const FString CompositionSchema =
			TEXT("uai.authoring.target_composition/1");
		const FString ManifestPath =
			TEXT(".uai/workspace.hjson");

		TSharedPtr<FJsonValue> OrderedDefinitionIdentity(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value) return MakeShared<FJsonValueNull>();
			if (Value->Type == EJson::Object)
			{
				const auto Result = MakeShared<FJsonObject>();
				TArray<FString> Keys;
				Value->AsObject()->Values.GetKeys(Keys); Keys.Sort();
				for (const FString& Key : Keys) Result->SetField(Key, OrderedDefinitionIdentity(Value->AsObject()->Values.FindChecked(Key)));
				return MakeShared<FJsonValueObject>(Result);
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const auto& Item : Value->AsArray()) Items.Add(OrderedDefinitionIdentity(Item));
				return MakeShared<FJsonValueArray>(Items);
			}
			return Value;
		}

		FString DefinitionIdentity(const TSharedRef<FJsonObject>& Definition)
		{
			FString Result;
			FJsonSerializer::Serialize(OrderedDefinitionIdentity(MakeShared<FJsonValueObject>(Definition))->AsObject().ToSharedRef(),
				TJsonWriterFactory<>::Create(&Result));
			return Result;
		}

		bool TryReadHjsonObject(
			const TArray<uint8>& Bytes,
			TSharedPtr<FJsonObject>& OutObject)
		{
			OutObject.Reset();
			const FUTF8ToTCHAR Conversion(
				reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
				Bytes.Num());
			FString Text(Conversion.Length(), Conversion.Get());
			UE::UAI::Contract::FDiagnostic Error;
			return FUeAgentStructuredHjsonParser::ParseObject(
				Text,
				TEXT("semantic-unit-row.hjson"),
				OutObject,
				Error);
		}

		void AddSemanticEnvelope(
			const FSemanticUnit& Unit,
			const TSharedRef<FJsonObject>& Inventory)
		{
			TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
			Envelope->SetStringField(TEXT("owner_ref"), Unit.GetOwnerRef());
			Envelope->SetStringField(
				TEXT("semantic_type_ref"), Unit.GetSemanticTypeRef());
			Envelope->SetStringField(TEXT("role"), Unit.GetRole());
			Envelope->SetStringField(
				TEXT("field_set_ref"), Unit.GetFieldSetRef());
			if (!Unit.GetItemRefs().IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FString& Value : Unit.GetItemRefs())
				{
					Values.Add(MakeShared<FJsonValueString>(Value));
				}
				Envelope->SetArrayField(TEXT("item_refs"), MoveTemp(Values));
			}
			if (!Unit.GetRelationRefs().IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FString& Value : Unit.GetRelationRefs())
				{
					Values.Add(MakeShared<FJsonValueString>(Value));
				}
				Envelope->SetArrayField(TEXT("relation_refs"), MoveTemp(Values));
			}
			if (!Unit.GetOrderingRef().IsEmpty())
			{
				Envelope->SetStringField(
					TEXT("ordering_ref"), Unit.GetOrderingRef());
			}
			if (!Unit.GetLayoutRef().IsEmpty())
			{
				Envelope->SetStringField(
					TEXT("layout_ref"), Unit.GetLayoutRef());
			}
			if (!Unit.GetDependencyRefs().IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FString& Value : Unit.GetDependencyRefs())
				{
					Values.Add(MakeShared<FJsonValueString>(Value));
				}
				Envelope->SetArrayField(
					TEXT("dependency_refs"), MoveTemp(Values));
			}
			if (!Unit.GetProfileVisibility().IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FString& Value : Unit.GetProfileVisibility())
				{
					Values.Add(MakeShared<FJsonValueString>(Value));
				}
				Envelope->SetArrayField(
					TEXT("profile_visibility"), MoveTemp(Values));
			}
			if (!Unit.GetTargetIdentityKey().IsEmpty())
			{
				Envelope->SetStringField(
					TEXT("target_identity_key"),
					Unit.GetTargetIdentityKey());
			}
			if (!Unit.GetSourceProvenance().IsEmpty())
			{
				Envelope->SetStringField(
					TEXT("source_provenance"),
					Unit.GetSourceProvenance());
			}
			Inventory->SetObjectField(TEXT("envelope"), Envelope);
		}

		bool TryBuildCompactRow(
			const FSemanticUnit& Unit,
			TSharedPtr<FJsonObject>& OutRow, FWorkspaceError& OutError,
			const TMap<FString, FString>* PublicRefs = nullptr, const FString& SourceFile = FString())
		{
			OutRow.Reset();
			return TryBuildSemanticModelUnitRow(Unit, OutRow, OutError, PublicRefs, SourceFile);
		}

		// The model already owns definition interning and cycle validation. Keep
		// expansion shared by nested-record export and workspace edit comparison.
		TSharedPtr<FJsonValue> ExpandPropertyDefinition(const FAuthoringModel& Model,
			const TSharedPtr<FJsonValue>& Value, FWorkspaceError& OutError,
			const FString& SourceUnit = FString(), const FString& Pointer = FString())
		{
			if (!Value)
			{
				auto Error = Contract::FDiagnostic::Failure(TEXT("property_definition_value_missing"),
					TEXT("The property definition contains a missing value."), TEXT("definition_expansion"));
				Error.ActualValuePresent = false;
				OutError.SetFailure(MoveTemp(Error), SourceUnit, Pointer);
				return nullptr;
			}
			if (Value->Type == EJson::String)
			{
				const auto* Definition = Model.FindUnit(Value->AsString());
				if (!Definition || Definition->GetRole() != TEXT("property_definition")) return Value;
				TSharedPtr<FJsonObject> Row;
				if (!TryBuildCompactRow(*Definition, Row, OutError)) return nullptr;
				// Row construction owns the values object; it cannot be absent on success.
				return ExpandPropertyDefinition(Model, Row->TryGetField(TEXT("values")), OutError,
					Definition->GetUnitId(), TEXT("/values"));
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
				{
					auto Expanded = ExpandPropertyDefinition(Model, Value->AsArray()[Index], OutError,
						SourceUnit, Pointer + TEXT("/") + FString::FromInt(Index));
					if (!Expanded) return nullptr;
					Items.Add(MoveTemp(Expanded));
				}
				return MakeShared<FJsonValueArray>(Items);
			}
			if (Value->Type != EJson::Object) return Value;
			const auto Object = MakeShared<FJsonObject>();
			for (const auto& Field : Value->AsObject()->Values)
			{
				auto Expanded = ExpandPropertyDefinition(Model, Field.Value, OutError,
					SourceUnit, Pointer + TEXT("/") + EscapeJsonPointerSegment(Field.Key));
				if (!Expanded) return nullptr;
				Object->SetField(Field.Key, MoveTemp(Expanded));
			}
			return MakeShared<FJsonValueObject>(Object);
		}

		FString ReadablePropertyDefinition(const FSemanticUnit& Unit, const FAuthoringModel& Model)
		{
			if (const FString* Name = Model.FindReadablePropertyDefinitionName(Unit.GetUnitId())) return *Name;
			for (const FString& Dependency : Unit.GetDependencyRefs())
				if (const FString* Name = Model.FindReadablePropertyDefinitionName(Dependency)) return *Name;
			return FString();
		}

		FString ReadablePropertyType(
			const FSemanticUnit& Unit,
			const FAuthoringModel& Model)
		{
			auto FromDefinition = [](const FSemanticUnit& Definition) -> FString
			{
				const FUeAgentCanonicalValueRef* Type =
					Definition.GetValues().Find(TEXT("type"));
				if (Type == nullptr
					|| (*Type)->GetKind() != EUeAgentValueKind::Object)
				{
					return FString();
				}
				// A type describes the value, not the property's edit/candidate contract.
				return ReadablePropertyTypeName(Type->Get());
			};
			if (const FString OwnType = FromDefinition(Unit); !OwnType.IsEmpty())
			{
				return OwnType;
			}
			for (const FString& Dependency : Unit.GetDependencyRefs())
			{
				const FSemanticUnit* Definition = Model.FindUnit(Dependency);
				if (Definition == nullptr)
				{
					continue;
				}
				if (const FString DependencyType = FromDefinition(*Definition);
					!DependencyType.IsEmpty())
				{
					return DependencyType;
				}
			}
			return ShortReadableTypeName(Unit.GetSemanticTypeRef());
		}

		const FUeAgentCanonicalValue* ProjectionTypeField(const FUeAgentCanonicalValue* Type, const TCHAR* Name)
		{
			if (!Type || Type->GetKind() != EUeAgentValueKind::Object) return nullptr;
			const auto* Field = Type->GetObjectItems().FindByPredicate([Name](const auto& Item) { return Item.Key == Name; });
			return Field ? &Field->Value.Get() : nullptr;
		}

		TSharedPtr<FJsonValue> ProjectDirectPropertyValue(
			const TSharedPtr<FJsonValue>& Value,
			const FAuthoringModel& Model, UE::UAI::Contract::FDiagnostic& OutError,
			const FUeAgentCanonicalValue* ValueDefinition = nullptr, bool bOrderedItem = false, int32 Occurrence = 0,
			bool bNativeScope = false, const FUeAgentCanonicalValue* ValueType = nullptr,
			bool bEnvelopeContents = false)
		{
			if (!Value.IsValid()) return nullptr;
            // Captured templates and native struct members may still carry an
            // inline descriptor; interned members use a definition reference below.
            if (const auto* InlineType = ProjectionTypeField(ValueDefinition, TEXT("type"));
                ProjectionTypeField(InlineType, TEXT("kind")))
            {
                ValueType = InlineType;
                ValueDefinition = ProjectionTypeField(ValueDefinition, TEXT("value_definition"));
            }
            const auto FailProjection = [&](const FString& Reason) -> TSharedPtr<FJsonValue>
            {
                if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
                    TEXT("composition_property_value_invalid"), Reason, TEXT("property_projection"));
                return nullptr;
            };
			if (ValueDefinition && ValueDefinition->GetKind() == EUeAgentValueKind::String)
			{
				const auto* Definition = Model.FindUnit(ValueDefinition->GetText());
				const auto* Type = Definition ? Definition->GetValues().Find(TEXT("type")) : nullptr;
				bool bReflectedType = false;
				if (Type && Type->Get().GetKind() == EUeAgentValueKind::Object)
					for (const auto& Field : Type->Get().GetObjectItems())
						bReflectedType |= Field.Key == TEXT("kind");
				// Native field descriptors reference the same centralized definitions
				// as root properties. Contract-defined business leaves retain their shape.
				if (!bReflectedType) return Value;
				const auto* Nested = Definition->GetValues().Find(TEXT("value_definition"));
				const auto Projected = ProjectDirectPropertyValue(Value, Model, OutError, Nested ? &Nested->Get() : nullptr,
					bOrderedItem, Occurrence, bNativeScope, &Type->Get(), bEnvelopeContents);
				if (Projected && UeAgentAuthoringEncodedTypes::FScope::IsActive())
				{
					TSharedPtr<FJsonValue> NativeType, NativeDefinition;
					if (!CanonicalJson::TryToJsonValue(Type->Get(), NativeType, OutError)) return nullptr;
					if (Nested && !CanonicalJson::TryToJsonValue(Nested->Get(), NativeDefinition, OutError)) return nullptr;
					const auto Descriptor = NativeDefinition && NativeDefinition->Type == EJson::Object ? NativeDefinition->AsObject() : MakeShared<FJsonObject>();
					Descriptor->SetField(TEXT("type"), NativeType);
					UeAgentAuthoringTypeDescriptors::FDescriptor Declared;
					NativeType->AsObject()->TryGetStringField(TEXT("kind"), Declared.Kind);
					UeAgentAuthoringEncodedTypes::FScope::Record(Projected, Declared, Descriptor);
				}
				return Projected;
			}
			// A deferred inline patch already contains readable rows. Validate its
			// catalog aliases before rebinding those rows to the live native schema.
			if (bNativeScope && Value->Type == EJson::Object && ValueDefinition
				&& ValueDefinition->GetKind() == EUeAgentValueKind::Object)
			{
				bool bPropertyRows = false;
				const FUeAgentCanonicalValue* Items = nullptr;
				for (const auto& Field : ValueDefinition->GetObjectItems())
				{
					if (Field.Key == TEXT("element_kind") && Field.Value->GetKind() == EUeAgentValueKind::String)
						bPropertyRows = Field.Value->GetText() == TEXT("reflected_property");
					if (Field.Key == TEXT("items")) Items = &Field.Value.Get();
				}
				if (bPropertyRows)
				{
					if (!Items || Items->GetKind() != EUeAgentValueKind::Array) return FailProjection(TEXT("The reflected property schema must contain an array of items."));
					TArray<FReadablePropertySchema> Schema;
					TArray<const FSemanticUnit*> Units;
					TArray<const FUeAgentCanonicalValue*> Definitions;
					for (const auto& Item : Items->GetArrayItems())
					{
						const FUeAgentCanonicalValue* Definition = nullptr;
						if (Item->GetKind() != EUeAgentValueKind::Object) return FailProjection(TEXT("A reflected property schema item must be an object."));
						for (const auto& Field : Item->GetObjectItems())
							if (Field.Key == TEXT("fields") && Field.Value->GetKind() == EUeAgentValueKind::Object)
								for (const auto& Member : Field.Value->GetObjectItems())
									if (Member.Key == TEXT("value")) Definition = &Member.Value.Get();
						const auto* Unit = Definition && Definition->GetKind() == EUeAgentValueKind::String ? Model.FindUnit(Definition->GetText()) : nullptr;
						const auto* Type = Unit ? Unit->GetValues().Find(TEXT("type")) : nullptr;
						FReadablePropertySchema Row;
						if (!Type || !TryDescribeReadableProperty(Item.Get(), Type->Get(), Row)) return FailProjection(TEXT("The reflected property row has no readable type descriptor."));
						Row.Type = ReadablePropertyType(*Unit, Model);
						Row.Definition = ReadablePropertyDefinition(*Unit, Model);
						Schema.Add(MoveTemp(Row)); Units.Add(Unit); Definitions.Add(Definition);
					}
					// A preceding root edit can change the inline class. Only rebind
					// aliases proven by the captured schema; leave new native rows for
					// ObjectApply to validate against that actual class.
					for (const auto& Category : Value->AsObject()->Values)
					{
						if (!Category.Value || Category.Value->Type != EJson::Object) return FailProjection(TEXT("A reflected property category must be an object."));
						for (const auto& Field : Category.Value->AsObject()->Values)
						{
							FString Type;
							if (!Field.Value || Field.Value->Type != EJson::Object || !Field.Value->AsObject()->TryGetStringField(TEXT("type"), Type)) return FailProjection(TEXT("A reflected property row must declare its type."));
							const int32 Index = Schema.IndexOfByPredicate([&](const FReadablePropertySchema& Row) { return Row.Category == Category.Key && Row.Name == Field.Key; });
							if (Index == INDEX_NONE)
							{
								Schema.Add({Field.Key, Category.Key, Type}); Units.Add(nullptr); Definitions.Add(nullptr);
							}
							else if (Schema[Index].Type != Type)
							{
								Schema[Index].Type = Type; Units[Index] = nullptr; Definitions[Index] = nullptr;
							}
						}
					}
					auto Resources = MakeSemanticWorkspaceResourceTracker();
					FUeAgentCanonicalValuePtr Canonical;
					
					TArray<FResolvedReadableProperty> Rows;
					FUeAgentStateSemanticError RowError;
					if (!CanonicalJson::TryFromJsonValue(Value, Resources, Canonical, OutError)) return nullptr;
                    if (!TryReadReadablePropertyRows(*Canonical, Schema, Rows, RowError))
                    { OutError = MoveTemp(RowError); return nullptr; }
					const auto Categories = MakeShared<FJsonObject>();
					for (const auto& Row : Rows)
					{
						auto NativeSchema = Schema[Row.SchemaIndex];
						if (Units[Row.SchemaIndex]) NativeSchema.Type = ReadablePropertyType(*Units[Row.SchemaIndex], Model);
						NativeSchema.Definition.Reset();
						TSharedPtr<FJsonValue> Inner;
						if (!CanonicalJson::TryToJsonValue(*Row.Value, Inner, OutError)) return nullptr;
						const auto NativeValue = Units[Row.SchemaIndex]
							? ProjectDirectPropertyValue(Inner, Model, OutError, Definitions[Row.SchemaIndex], false, 0, true) : Inner;
						if (!NativeValue || !TryAddReadablePropertyRow(Categories, NativeSchema, NativeValue)) return FailProjection(TEXT("The projected property row could not be added to its category."));
					}
					return MakeShared<FJsonValueObject>(Categories);
				}
			}
			if (ValueDefinition && ValueDefinition->GetKind() == EUeAgentValueKind::Object)
				for (const auto& DefinitionField : ValueDefinition->GetObjectItems())
					if (DefinitionField.Key == TEXT("builtin") && DefinitionField.Value->GetKind() == EUeAgentValueKind::String
						&& DefinitionField.Value->GetText() == TEXT("text") && Value->Type == EJson::Object)
					{
						const auto TextObject = Value->AsObject();
						const TSharedPtr<FJsonObject>* Localized = nullptr;
						FString Source;
						if (TextObject->TryGetObjectField(TEXT("localized"), Localized)
							&& (*Localized)->TryGetStringField(TEXT("source"), Source))
							return MakeShared<FJsonValueString>(Source);
					}
			const auto* Kind = ProjectionTypeField(ValueType, TEXT("kind"));
			const FString NativeKind = Kind && Kind->GetKind() == EUeAgentValueKind::String ? Kind->GetText() : FString();
			const FString CodecWrapper = UeAgentAuthoringTypeDescriptors::NativeCodecValueWrapper(NativeKind);
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				const FUeAgentCanonicalValue* ItemDefinitions = nullptr;
				bool bOrderedOwnedItems = false;
				bool bOrderedStructuredItems = false;
				bool bPropertyRows = false;
				if (ValueDefinition && ValueDefinition->GetKind() == EUeAgentValueKind::Object)
					for (const auto& Field : ValueDefinition->GetObjectItems())
					{
						if (Field.Key == TEXT("element_kind") && Field.Value->GetKind() == EUeAgentValueKind::String)
						{
							bOrderedOwnedItems = Field.Value->GetText() == TEXT("owned_object");
							bOrderedStructuredItems = Field.Value->GetText() == TEXT("structured_owned_item");
							bPropertyRows = Field.Value->GetText() == TEXT("reflected_property");
						}
						if (Field.Key == TEXT("items") && Field.Value->GetKind() == EUeAgentValueKind::Array)
							ItemDefinitions = &Field.Value.Get();
					}
				if (bPropertyRows)
				{
					if (!ItemDefinitions || ItemDefinitions->GetArrayItems().Num() != Value->AsArray().Num()) return FailProjection(TEXT("The property value count does not match its captured schema."));
					const auto Categories = MakeShared<FJsonObject>();
					for (int32 RowIndex = 0; RowIndex < Value->AsArray().Num(); ++RowIndex)
					{
						const auto& Definition = ItemDefinitions->GetArrayItems()[RowIndex].Get();
						const FUeAgentCanonicalValue* ValueSchema = nullptr;
						if (Definition.GetKind() != EUeAgentValueKind::Object) return FailProjection(TEXT("A reflected property definition must be an object."));
						for (const auto& Field : Definition.GetObjectItems())
							if (Field.Key == TEXT("fields") && Field.Value->GetKind() == EUeAgentValueKind::Object)
								for (const auto& Member : Field.Value->GetObjectItems())
									if (Member.Key == TEXT("value")) ValueSchema = &Member.Value.Get();
						const auto* Unit = ValueSchema && ValueSchema->GetKind() == EUeAgentValueKind::String ? Model.FindUnit(ValueSchema->GetText()) : nullptr;
						const auto* Type = Unit ? Unit->GetValues().Find(TEXT("type")) : nullptr;
						FReadablePropertySchema Schema;
						if (!Type || !TryDescribeReadableProperty(Definition, Type->Get(), Schema)) return FailProjection(TEXT("The reflected property definition has no readable type descriptor."));
						Schema.Type = ReadablePropertyType(*Unit, Model);
						if (!bNativeScope) Schema.Definition = ReadablePropertyDefinition(*Unit, Model);
						const auto& Item = Value->AsArray()[RowIndex];
						if (!Item || Item->Type != EJson::Object) return FailProjection(TEXT("A reflected property value must be an object."));
						const auto Inner = Item->AsObject()->TryGetField(TEXT("value"));
						if (!TryAddReadablePropertyRow(Categories, Schema, ProjectDirectPropertyValue(Inner, Model, OutError, ValueSchema, false, 0, bNativeScope))) return FailProjection(TEXT("The projected property row could not be added to its category."));
					}
					return MakeShared<FJsonValueObject>(Categories);
				}
				int32 Index = 0;
				TMap<FString, int32> Occurrences;
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					const FUeAgentCanonicalValue* ItemDefinition = ItemDefinitions && ItemDefinitions->GetArrayItems().IsValidIndex(Index)
						? &ItemDefinitions->GetArrayItems()[Index].Get() : nullptr;
					int32 ItemOccurrence = 0;
					if (ItemDefinition && ItemDefinition->GetKind() == EUeAgentValueKind::Object)
						for (const auto& Field : ItemDefinition->GetObjectItems())
							if (Field.Key == TEXT("instance_name") && Field.Value->GetKind() == EUeAgentValueKind::String)
								ItemOccurrence = Occurrences.FindOrAdd(Field.Value->GetText().TrimStartAndEnd().ToLower())++;
					const auto* ItemType = NativeKind == TEXT("map") ? ValueType : ProjectionTypeField(ValueType, TEXT("element_type"));
					auto ProjectedItem = ProjectDirectPropertyValue(Item, Model, OutError, ItemDefinition,
						bOrderedOwnedItems, ItemOccurrence, bNativeScope, ItemType);
					if (!OutError.IsEmpty()) return nullptr;
					if (bOrderedStructuredItems)
					{
						if (!ProjectedItem.IsValid() || ProjectedItem->Type != EJson::Object) return FailProjection(TEXT("An ordered structured item must project to an object."));
						FString Key;
						FWorkspaceError KeyError;
						if (!TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Index, Key, KeyError)) { OutError = KeyError.GetDiagnostic(); return nullptr; }
						auto Structured = MakeShared<FJsonObject>();
						Structured->Values = ProjectedItem->AsObject()->Values;
						Structured->SetStringField(TEXT("instance"), Key);
						ProjectedItem = MakeShared<FJsonValueObject>(Structured);
					}
					Items.Add(ProjectedItem);
					++Index;
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			if (Value->Type != EJson::Object) return Value;
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid()) return FailProjection(TEXT("The property object is invalid."));
			if (Object->Values.Num() == 1)
			{
				const TPair<FString, TSharedPtr<FJsonValue>>& Only =
					*Object->Values.CreateConstIterator();
				if (!bEnvelopeContents && !CodecWrapper.IsEmpty() && Only.Key == CodecWrapper)
				{
					return ProjectDirectPropertyValue(Only.Value, Model, OutError, ValueDefinition, false, 0, bNativeScope, ValueType, true);
				}
			}
			TSharedRef<FJsonObject> Projected = MakeShared<FJsonObject>();
			const FUeAgentCanonicalValue* FieldDefinitions = nullptr;
			bool bHashSource = false;
			if (ValueDefinition && ValueDefinition->GetKind() == EUeAgentValueKind::Object)
				for (const auto& Field : ValueDefinition->GetObjectItems())
				{
					bHashSource |= Field.Key == TEXT("hash_source");
					if (Field.Key == TEXT("fields") && Field.Value->GetKind() == EUeAgentValueKind::Object)
						FieldDefinitions = &Field.Value.Get();
				}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
			{
				// Matching facts keep the complete canonical codec representation;
				// ordinary source/text shorthand is only for the editable value.
				if (bHashSource && Field.Key == TEXT("from")) { Projected->SetField(Field.Key, Field.Value); continue; }
				if (Field.Key.Equals(TEXT("fields"), ESearchCase::CaseSensitive)
					&& Field.Value.IsValid() && Field.Value->Type == EJson::Object)
				{
					// Codec fields are a member map, not a value envelope. A native
					// member named Value (or another wrapper name) keeps its key.
					const auto Members = MakeShared<FJsonObject>();
					for (const auto& Member : Field.Value->AsObject()->Values)
					{
						const FUeAgentCanonicalValue* MemberDefinition = nullptr;
						if (FieldDefinitions)
							for (const auto& Candidate : FieldDefinitions->GetObjectItems())
								if (Candidate.Key.Equals(Member.Key, ESearchCase::CaseSensitive)) { MemberDefinition = &Candidate.Value.Get(); break; }
						Members->SetField(Member.Key, ProjectDirectPropertyValue(Member.Value, Model, OutError, MemberDefinition, false, 0, bNativeScope));
                        if (!OutError.IsEmpty()) return nullptr;
					}
					Projected->SetObjectField(Field.Key, Members);
					continue;
				}
				const FUeAgentCanonicalValue* ChildDefinition = nullptr;
				if (FieldDefinitions)
				{
					if (Field.Key == TEXT("fields")) ChildDefinition = ValueDefinition;
					else for (const auto& Candidate : FieldDefinitions->GetObjectItems())
						if (Candidate.Key.Equals(Field.Key, ESearchCase::CaseSensitive)) { ChildDefinition = &Candidate.Value.Get(); break; }
				}
				const FUeAgentCanonicalValue* ChildType = nullptr;
				if (NativeKind == TEXT("map"))
				{
					if (Field.Key == TEXT("entries")) ChildType = ValueType;
					else if (Field.Key == TEXT("key")) ChildType = ProjectionTypeField(ValueType, TEXT("key_type"));
					else if (Field.Key == TEXT("value")) ChildType = ProjectionTypeField(ValueType, TEXT("mapped_type"));
				}
				else if (NativeKind == TEXT("optional") && Field.Key == TEXT("value"))
					ChildType = ProjectionTypeField(ValueType, TEXT("inner_type"));
				Projected->SetField(
					Field.Key, ProjectDirectPropertyValue(Field.Value, Model, OutError, ChildDefinition, false, 0, bNativeScope, ChildType));
                if (!OutError.IsEmpty()) return nullptr;
			}
			if (bOrderedItem && Projected->HasField(TEXT("owned_object")) && ValueDefinition && ValueDefinition->GetKind() == EUeAgentValueKind::Object)
				for (const auto& Field : ValueDefinition->GetObjectItems())
					if (Field.Key == TEXT("builtin") && Field.Value->GetKind() == EUeAgentValueKind::String
						&& Field.Value->GetText() == TEXT("owned_object"))
					{
						FString Key, RelativePath; FWorkspaceError Error;
						if (!TryGetOwnedItemBinding(*ValueDefinition, Key, RelativePath, Error, Occurrence)) { OutError = Error.GetDiagnostic(); return nullptr; }
						// The captured instance binding owns the exact class. Ordinary
						// edits do not repeat or reselect it.
						Projected->GetObjectField(TEXT("owned_object"))->RemoveField(TEXT("class_path"));
						Projected->GetObjectField(TEXT("owned_object"))->SetStringField(TEXT("instance"), Key);
					}
			return MakeShared<FJsonValueObject>(Projected);
		}

		bool ProjectElementTemplates(const TSharedPtr<FJsonValue>& PublicDefinition,
			const FUeAgentCanonicalValue& CapturedDefinition, const FAuthoringModel& Model, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!PublicDefinition) return true;
			if (PublicDefinition->Type == EJson::Array && CapturedDefinition.GetKind() == EUeAgentValueKind::Array)
			{
				const auto& Items = PublicDefinition->AsArray(); const auto& Captured = CapturedDefinition.GetArrayItems();
				for (int32 Index = 0; Index < FMath::Min(Items.Num(), Captured.Num()); ++Index)
					if (!ProjectElementTemplates(Items[Index], Captured[Index].Get(), Model, OutError)) return false;
				return true;
			}
			if (PublicDefinition->Type != EJson::Object || CapturedDefinition.GetKind() != EUeAgentValueKind::Object) return true;
			const auto Object = PublicDefinition->AsObject();
			const auto* Element = CapturedDefinition.GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("element_definition"); });
			if (const auto Template = Object->TryGetField(TEXT("element_template")); Template && Element)
			{
				// Project defaults through the same codec-value projection as a real
				// property, but never request an existing ordered-item identity.
				Object->SetField(TEXT("element_template"), ProjectDirectPropertyValue(Template, Model, OutError, &Element->Value.Get()));
                if (!OutError.IsEmpty()) return false;
			}
			for (const auto& Field : CapturedDefinition.GetObjectItems())
				if (Field.Key != TEXT("element_template"))
					if (!ProjectElementTemplates(Object->TryGetField(Field.Key), Field.Value.Get(), Model, OutError)) return false;
		    return true;
		}

		TSharedPtr<FJsonValue> DirectPropertyValue(
			const TSharedPtr<FJsonObject>& CompactRow,
			const FSemanticUnit& Unit, const FAuthoringModel& Model, UE::UAI::Contract::FDiagnostic& OutError, bool bNativeScope = false)
		{
			const TSharedPtr<FJsonObject>* Values = nullptr;
			if (!CompactRow.IsValid()
				|| !CompactRow->TryGetObjectField(TEXT("values"), Values)
				|| Values == nullptr || !Values->IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("composition_property_values_missing"), TEXT("The property row has no values object."), TEXT("property_projection"));
                return nullptr;
			}
			const FUeAgentCanonicalValue* ValueDefinition = nullptr;
			const FUeAgentCanonicalValue* ValueType = nullptr;
			for (const FString& Dependency : Unit.GetDependencyRefs())
			{
				const auto* Definition = Model.FindUnit(Dependency);
				if (Definition && Definition->GetRole() == TEXT("property_definition"))
				{
					if (const auto* Type = Definition->GetValues().Find(TEXT("type"))) ValueType = &Type->Get();
					if (const auto* Value = Definition->GetValues().Find(TEXT("value_definition")))
						ValueDefinition = &Value->Get();
					break;
				}
			}
			// The unit's value slot is a semantic envelope. It is consumed exactly once;
			// nested business maps named value/items/path are never inferred envelopes.
			if ((*Values)->Values.Num() == 1 && (*Values)->HasField(TEXT("value")))
				return ProjectDirectPropertyValue((*Values)->TryGetField(TEXT("value")), Model, OutError,
					ValueDefinition, false, 0, bNativeScope, ValueType);
			return ProjectDirectPropertyValue(
				MakeShared<FJsonValueObject>((*Values).ToSharedRef()), Model, OutError, ValueDefinition, false, 0, bNativeScope, ValueType);
		}

		struct FUnitDraft
		{
			FString GraphOwnerRef;
			FString UnitId;
			ESemanticUnitKind UnitKind =
				ESemanticUnitKind::PropertySet;
			FString OwnerRef;
			FString SemanticTypeRef;
			FString Role;
			FString FieldSetRef;
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			TArray<FString> ItemRefs;
			TArray<FString> RelationRefs;
			FString OrderingRef;
			FString LayoutRef;
			TArray<FString> DependencyRefs;
			TArray<FString> ProfileVisibility{
				TEXT("state"),
				TEXT("seed"),
				TEXT("edit_bundle")};
			FString SourceProvenance;
			FString TargetIdentityKey;
			TArray<FString> CategoryPath;
		};

		struct FEntryRoute
		{
			FString SurfaceId;
			ESemanticUnitKind UnitKind =
				ESemanticUnitKind::PropertySet;
			bool bTargetValues = false;
			bool bGraph = false;
			bool bItem = false;
			bool bGraphItem = false;
			bool bProperty = false;
			bool bRelation = false;
			bool bDocument = false;
		};

		struct FCompositionIdentityView
		{
			FString CanonicalKey;
			TMap<FString, FString> Selectors;

			FString GetSelector(const FString& Name) const
			{
				const FString* Found = Selectors.Find(Name);
				return Found ? *Found : FString();
			}
		};

		struct FCompositionEntryView
		{
			FString SemanticId;
			FCompositionIdentityView TargetIdentity;
			TSharedPtr<FJsonObject> Type;
			FUeAgentCanonicalValuePtr Value;
			bool bAvailable = true;
			bool bEditable = false;
			bool bAuthoringVisible = true;
			TMap<FString, FString> Constraints;
			TSharedPtr<FJsonValue> CandidatesHeader;
			TSharedPtr<FJsonValue> Candidates;
			TSharedPtr<FJsonValue> ValueDefinition;
			TArray<FString> CategoryPath;
			FString Provenance;
		};

		struct FCompositionInput
		{
			FString TargetKind;
			FString ProfileId;
			FString BaseRevision;
			FString SourceProvenance;
			TArray<FCompositionEntryView> Entries;
		};

		struct FDocumentAddressStep
		{
			FString Key;
			int32 Index = INDEX_NONE;
		};
		using FDocumentAddress = TArray<FDocumentAddressStep>;
		FString DocumentAddressPointer(const FDocumentAddress& Address)
		{
			FString Pointer;
			for (const auto& Step : Address)
				Pointer += TEXT("/") + (Step.Index != INDEX_NONE ? FString::FromInt(Step.Index)
					: Step.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
			return Pointer.IsEmpty() ? FString(TEXT("/")) : Pointer;
		}
		FDocumentAddress ChildAddress(FDocumentAddress Parent, const FString& Key, int32 Index = INDEX_NONE)
		{
			Parent.Add({Key, Index});
			return Parent;
		}
		const FKeyedItemProjectionContract* FindLevelSequenceCollectionContract(
			const FString& TargetKind, const FString& SemanticId, const FString& SurfaceId, const FString& Path)
		{
			if (!(TargetKind == TEXT("ue.level_sequence.asset")
				&& SemanticId == TEXT("uai.semantic.reflection.sequence.profile_state")
				&& SurfaceId == TEXT("timeline_collection"))) return nullptr;
			if (Path == TEXT("bindings") || Path.EndsWith(TEXT("/bindings"))) return &GetLevelSequenceBindingProjectionContract();
			if (Path == TEXT("folders") || Path.EndsWith(TEXT("/folders"))) return &GetLevelSequenceFolderProjectionContract();
			if (Path == TEXT("root_tracks") || Path.EndsWith(TEXT("/root_tracks"))) return &GetLevelSequenceRootScalarTrackProjectionContract();
			if (Path == TEXT("tracks") || Path.EndsWith(TEXT("/tracks"))) return &GetLevelSequenceBindingScalarTrackProjectionContract();
			return nullptr;
		}
		struct FDocumentValueBinding
		{
			FString SourceKey;
			FDocumentAddress Address;
			bool bWholeValue = false;
			const FKeyedItemProjectionContract* KeyedContract = nullptr;
			FUeAgentCanonicalValuePtr RecordDefaults;
			FUeAgentCanonicalValuePtr NestedRecordDefaults;
			FUeAgentCanonicalValuePtr CapturedItems;
		};

		FString PointerToken(const FString& Key)
		{
			return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		void BindKeyedDocumentSources(const FUeAgentCanonicalValue& Visible,
			const FUeAgentCanonicalValue& Resolved, const FKeyedItemProjectionContract& Contract,
			const FDocumentValueSourceBinding& Parent, TArray<FDocumentValueSourceBinding>& Sources)
		{
			// The keyed resolver emits array records in visible-key order. Keep
			// that binding while both representations still exist, before a native
			// owner can reorder items or replace private creation identities.
			if (!Contract.bIdentityFromMapKey || Visible.GetKind() != EUeAgentValueKind::Object
				|| Resolved.GetKind() != EUeAgentValueKind::Array || Contract.bEmitOperationRecords) return;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (const auto& Item : Visible.GetObjectItems())
			{
				const auto* Operation = Item.Value->GetObjectItems().FindByPredicate([](const auto& Field) {
					return Field.Key == TEXT("operation"); });
				if (Operation && Operation->Value->GetKind() == EUeAgentValueKind::String
					&& Operation->Value->GetText() == TEXT("remove")) continue;
				Items.Add(Item);
			}
			Items.Sort([](const auto& A, const auto& B) { return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0; });
			if (Items.Num() != Resolved.GetArrayItems().Num()) return;
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				FDocumentValueSourceBinding Binding = Parent;
				Binding.DocumentPointer += TEXT("/") + FString::FromInt(Index);
				Binding.UnitValuePointer += TEXT("/") + PointerToken(Items[Index].Key);
				Sources.Add(Binding);
				for (const auto& Child : Contract.CollectionFieldContracts)
				{
					const auto* VisibleChild = Items[Index].Value->GetObjectItems().FindByPredicate([&](const auto& Field) {
						return Field.Key == Child.Key; });
					const auto* ResolvedChild = Resolved.GetArrayItems()[Index]->GetObjectItems().FindByPredicate([&](const auto& Field) {
						return Field.Key == Child.Key; });
					if (!VisibleChild || !ResolvedChild) continue;
					FDocumentValueSourceBinding Nested = Binding;
					Nested.DocumentPointer += TEXT("/") + PointerToken(Child.Key);
					Nested.UnitValuePointer += TEXT("/") + PointerToken(Child.Key);
					BindKeyedDocumentSources(*VisibleChild->Value, *ResolvedChild->Value, *Child.Value, Nested, Sources);
				}
			}
		}
		FString DocumentSourceKey(const FString& SemanticId, const FString& TargetKey)
		{
			return FString::FromInt(SemanticId.Len()) + TEXT(":") + SemanticId + TargetKey;
		}
		struct FDocumentVisit
		{
			FUeAgentCanonicalValuePtr Value;
			FString Path;
			FString RoleHint;
			FString OwnerId;
			FString SurfaceId;
			int32 SourceIndex = INDEX_NONE;
			FDocumentAddress Address;
		};

		struct FRelationAggregate
		{
			FString OwnerRef;
			FString SurfaceId;
			TMap<
				FString,
				TMap<FString, FUeAgentCanonicalValueRef>> Groups;
		};

		const FUeAgentCanonicalValueRef* FindDraftValue(
			const FUnitDraft& Draft,
			const TCHAR* Field)
		{
			return Draft.Values.Find(Field);
		}

		FString ReadDraftString(
			const FUnitDraft& Draft,
			const TCHAR* Field)
		{
			const FUeAgentCanonicalValueRef* Value = FindDraftValue(Draft, Field);
			return Value != nullptr
				&& (*Value)->GetKind() == EUeAgentValueKind::String
					? (*Value)->GetText()
					: FString();
		}

		bool IsGraphStructureDraft(const FUnitDraft& Draft)
		{
			return Draft.UnitKind == ESemanticUnitKind::StructureState
				&& (Draft.SemanticTypeRef.EndsWith(TEXT(".graph"))
					|| Draft.SemanticTypeRef.Contains(TEXT(".graph."))
					|| (Draft.Values.Contains(TEXT("nodes"))
						&& Draft.Values.Contains(TEXT("edges"))));
		}

		bool TryAddGraphDesiredStateProjection(
			TArray<FUnitDraft>& Drafts,
			FUeAgentResourceTracker& Resources,
			FWorkspaceError& OutError)
		{
			FUeAgentCanonicalValuePtr UpdateValue;
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryString(
				TEXT("update"), Resources, UpdateValue, SemanticError))
			{
				OutError.SetFailure(SemanticError, FString());
				return false;
			}
			TMap<FString, FUnitDraft*> LayoutByOwner;
			TSet<FString> AmbiguousLayoutOwners;
			TMap<FString, TMap<FString, FUeAgentCanonicalValueRef>> PropertiesByOwner;
			for (const FUnitDraft& Draft : Drafts)
			{
				if (Draft.UnitKind != ESemanticUnitKind::PropertySet || Draft.FieldSetRef.IsEmpty()) continue;
				if (const auto* Value = FindGraphNodeNativeNameProperty(Draft.FieldSetRef, Draft.Values))
					PropertiesByOwner.FindOrAdd(Draft.OwnerRef).Add(Draft.FieldSetRef, *Value);
			}
			for (FUnitDraft& Draft : Drafts)
				if (Draft.UnitKind == ESemanticUnitKind::LayoutState)
				{
					if (LayoutByOwner.Contains(Draft.OwnerRef))
						AmbiguousLayoutOwners.Add(Draft.OwnerRef);
					LayoutByOwner.Add(Draft.OwnerRef, &Draft);
				}

			FWorkspaceError ProjectionErrors;
			for (FUnitDraft& Graph : Drafts)
			{
				if (!IsGraphStructureDraft(Graph))
				{
					continue;
				}

				TArray<const FUnitDraft*> Nodes;
				for (const FUnitDraft& Candidate : Drafts)
				{
					if ((Candidate.OwnerRef == Graph.UnitId || Candidate.GraphOwnerRef == Graph.UnitId)
						&& Candidate.UnitKind == ESemanticUnitKind::ItemState)
					{
						Nodes.Add(&Candidate);
					}
				}
				TArray<FGraphAuthoringNodeAliasInput> AliasInputs;
				for (const FUnitDraft* Node : Nodes)
				{
					const FString SemanticName = ReadGraphNodeSemanticName(
						Node->Values, PropertiesByOwner.Find(Node->UnitId));
					AliasInputs.Add({Node->UnitId, SemanticName,
						ReadGraphNodeNativeIdentity(Node->Values)});
				}

				FGraphAuthoringAliasProjection Aliases;
				if (!TryBuildGraphAuthoringAliasProjection(
					AliasInputs, Aliases, OutError))
				{
					ProjectionErrors.Append(OutError);
					ProjectionErrors.MarkUnchecked(TEXT("graph_projection"), Graph.UnitId, TEXT("alias_projection_failed"));
					OutError.Reset();
					continue;
				}

				TArray<TPair<FString, FUeAgentCanonicalValueRef>> NodeRows;
				for (const FUnitDraft* Node : Nodes)
				{
					const auto ProjectNode = [&]() -> bool {
					const FString Alias =
						Aliases.AliasBySourceId.FindChecked(Node->UnitId);
					TMap<FString, FUeAgentCanonicalValueRef> FieldMap;
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Field : Node->Values)
					{
						FieldMap.Add(Field.Key, Field.Value);
					}
					if (!Node->GraphOwnerRef.IsEmpty() && Node->OwnerRef != Graph.UnitId)
					{
						const FUnitDraft* const* Parent = Nodes.FindByPredicate(
							[&](const FUnitDraft* Candidate) { return Candidate->UnitId == Node->OwnerRef; });
						FUeAgentCanonicalValuePtr ParentId;
						if (!Parent) { OutError.SetSourceFailure(TEXT("composition_graph_parent_invalid"),
							FString::Printf(TEXT("Parent node for '%s' is absent from the captured graph."), *Alias), Node->UnitId, TEXT("/parent")); return false; }
						if (!FUeAgentCanonicalValue::TryString(ReadDraftString(**Parent, TEXT("id")),
							Resources, ParentId, SemanticError))
						{ OutError.SetFailure(SemanticError, Node->UnitId, TEXT("/parent")); return false; }
						FieldMap.Add(TEXT("parent"), ParentId.ToSharedRef());
					}
					if (!TryNormalizeGraphAuthoringNodePosition(FieldMap, Alias, Resources, OutError)) return false;
					if (FUnitDraft* const* Layout = LayoutByOwner.Find(Node->UnitId))
					{
						if (AmbiguousLayoutOwners.Contains(Node->UnitId))
						{ OutError.SetSourceFailure(TEXT("composition_graph_layout_ambiguous"), FString::Printf(TEXT("Node '%s' has more than one captured layout."), *Alias), Node->UnitId, TEXT("/pos")); return false; }
						FUeAgentCanonicalValuePtr Position;
						if ((*Layout)->Values.Num() == 1 && (*Layout)->Values.Contains(TEXT("value")))
							Position = (*Layout)->Values.FindChecked(TEXT("value"));
						else
						{
							TArray<TPair<FString, FUeAgentCanonicalValueRef>> Coordinates;
							for (const auto& Field : (*Layout)->Values) Coordinates.Add(Field);
							if (!FUeAgentCanonicalValue::TryObject(Coordinates, Resources, Position, SemanticError))
							{ OutError.SetFailure(SemanticError, Node->UnitId); return false; }
						}
						TMap<FString, FUeAgentCanonicalValueRef> LayoutPosition;
						LayoutPosition.Add(TEXT("pos"), Position.ToSharedRef());
						if (!TryNormalizeGraphAuthoringNodePosition(LayoutPosition, Alias, Resources, OutError)) return false;
						Position = LayoutPosition.FindChecked(TEXT("pos"));
						(*Layout)->Values.Reset();
						for (const auto& Coordinate : Position->GetObjectItems())
							(*Layout)->Values.Add(Coordinate.Key, Coordinate.Value);
						if (const auto* Inline = FieldMap.Find(TEXT("pos")))
						{
							FUeAgentSemanticComparison Comparison;
							if (!TryCompareValues(*Inline, Position.ToSharedRef(), TEXT("/pos"), Resources, Comparison, SemanticError))
							{ OutError.SetFailure(SemanticError, Node->UnitId, TEXT("/pos")); return false; }
							if (!Comparison.IsEquivalent())
							{ OutError.SetFailure(MakeComparisonDiagnostic(Comparison, TEXT("composition_graph_layout_conflict"),
								FString::Printf(TEXT("Inline and layout positions disagree for node '%s'."), *Alias), TEXT("authoring_composition")), Node->UnitId, TEXT("/pos")); return false; }
						}
						FieldMap.Add(TEXT("pos"), Position.ToSharedRef());
					}
					if (!TryNormalizeGraphAuthoringNodePosition(
							FieldMap, Alias, Resources, OutError))
					{
						return false;
					}
					FieldMap.Add(TEXT("operation"), UpdateValue.ToSharedRef());
					TArray<FString> FieldKeys;
					FieldMap.GetKeys(FieldKeys);
					FieldKeys.Sort();
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
					for (const FString& FieldKey : FieldKeys)
					{
						Fields.Emplace(FieldKey, FieldMap.FindChecked(FieldKey));
					}
					FUeAgentCanonicalValuePtr NodeValue;
					if (!FUeAgentCanonicalValue::TryObject(
						Fields, Resources, NodeValue, SemanticError))
					{
						OutError.SetFailure(SemanticError, FString());
						return false;
					}
					NodeRows.Emplace(Alias, NodeValue.ToSharedRef());
					return true;
					};
					if (!ProjectNode())
					{
						ProjectionErrors.Append(OutError);
						ProjectionErrors.MarkUnchecked(TEXT("node_projection"), Node->UnitId, TEXT("node_fields_invalid"));
						OutError.Reset();
					}
				}
				if (NodeRows.Num() != Nodes.Num()) continue;
				FUeAgentCanonicalValuePtr NodeMap;
				if (!FUeAgentCanonicalValue::TryObject(
					NodeRows, Resources, NodeMap, SemanticError))
				{
					OutError.SetFailure(SemanticError, FString());
					return false;
				}

				TArray<TPair<FString, FUeAgentCanonicalValueRef>> EdgeRows;
				bool bHasRelationSet = false;
				for (const FUnitDraft& RelationSet : Drafts)
				{
					if (RelationSet.OwnerRef != Graph.UnitId
						|| RelationSet.UnitKind != ESemanticUnitKind::RelationSet)
					{
						continue;
					}
					bHasRelationSet = true;
					const FUeAgentCanonicalValueRef* Relations =
						FindDraftValue(RelationSet, TEXT("relations"));
					if (Relations == nullptr
						|| (*Relations)->GetKind() != EUeAgentValueKind::Object)
					{
						continue;
					}
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Group
						: (*Relations)->GetObjectItems())
					{
						if (Group.Value->GetKind() != EUeAgentValueKind::Object)
						{
							continue;
						}
						for (const TPair<FString, FUeAgentCanonicalValueRef>& Record
							: Group.Value->GetObjectItems())
						{
							if (Record.Value->GetKind() != EUeAgentValueKind::Object)
							{
								continue;
							}
							bool bHasFrom = false;
							bool bHasTo = false;
							for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
								: Record.Value->GetObjectItems())
							{
								bHasFrom = bHasFrom || Field.Key == TEXT("from");
								bHasTo = bHasTo || Field.Key == TEXT("to");
							}
							if (!bHasFrom || !bHasTo)
							{
								continue;
							}
							TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields =
								Record.Value->GetObjectItems();
							Fields.RemoveAll([](const auto& Field) { return Field.Key == TEXT("operation"); });
							Fields.Emplace(TEXT("operation"), UpdateValue.ToSharedRef());
							FUeAgentCanonicalValuePtr EdgeValue;
							if (!FUeAgentCanonicalValue::TryObject(
								Fields, Resources, EdgeValue, SemanticError))
							{
								OutError.SetFailure(SemanticError, FString());
								return false;
							}
							EdgeRows.Emplace(Record.Key, EdgeValue.ToSharedRef());
						}
					}
				}
				FUeAgentCanonicalValuePtr EdgeMap;
				if (!FUeAgentCanonicalValue::TryObject(
					EdgeRows, Resources, EdgeMap, SemanticError))
				{
					OutError.SetFailure(SemanticError, FString());
					return false;
				}

				const FUeAgentCanonicalValueRef* InlineNodes =
					FindDraftValue(Graph, TEXT("nodes"));
				if (!Nodes.IsEmpty() || InlineNodes == nullptr
					|| (*InlineNodes)->GetKind() != EUeAgentValueKind::Object)
				{
					Graph.Values.Add(TEXT("nodes"), NodeMap.ToSharedRef());
				}
				const FUeAgentCanonicalValueRef* InlineEdges =
					FindDraftValue(Graph, TEXT("edges"));
				if (bHasRelationSet || InlineEdges == nullptr
					|| (*InlineEdges)->GetKind() != EUeAgentValueKind::Object)
				{
					Graph.Values.Add(TEXT("edges"), EdgeMap.ToSharedRef());
				}
			}
			if (ProjectionErrors.HasErrors())
			{
				OutError = MoveTemp(ProjectionErrors);
				return false;
			}
			return true;
		}

		bool TryAddKeyedItemDesiredStateProjection(
			TArray<FUnitDraft>& Drafts,
			const FString& SurfaceRole,
			const FString& ItemSemanticSuffix,
			const FString& RootField,
			const FKeyedItemNamingPolicy& NamingPolicy,
			FUeAgentResourceTracker& Resources,
			FWorkspaceError& OutError,
			const bool bItemsOwnedByParent = false)
		{
			FKeyedItemProjectionContract Contract;
			TryGetKeyedItemProjectionContract(RootField, Contract);
			// These passes only change root Values, never Drafts membership or owner
			// identities. Retain source order while limiting each root/item to its
			// actual children instead of rescanning every reflected property.
			TMap<FString, TArray<const FUnitDraft*>> ItemsByOwner;
			TMap<FString, TArray<const FUnitDraft*>> PropertiesByOwner;
			for (const FUnitDraft& Draft : Drafts)
			{
				if (Draft.UnitKind == ESemanticUnitKind::ItemState
					&& (Draft.SemanticTypeRef.EndsWith(ItemSemanticSuffix)
						|| Draft.SemanticTypeRef.Contains(ItemSemanticSuffix + TEXT("#"))))
					ItemsByOwner.FindOrAdd(Draft.OwnerRef).Add(&Draft);
				if (Contract.bMergeChildPropertyValues && Draft.UnitKind == ESemanticUnitKind::PropertySet
					&& !Draft.SemanticTypeRef.EndsWith(TEXT(".property")))
					PropertiesByOwner.FindOrAdd(Draft.OwnerRef).Add(&Draft);
			}
			for (FUnitDraft& RootDraft : Drafts)
			{
				FUnitDraft* Root = &RootDraft;
				if (Root->UnitKind != ESemanticUnitKind::StructureState || Root->Role != SurfaceRole)
					continue;
				if (!Contract.AggregatedParentSemanticSuffix.IsEmpty()
					&& !Root->SemanticTypeRef.EndsWith(Contract.AggregatedParentSemanticSuffix))
					continue;
				TArray<const FUnitDraft*> Items;
				if (const auto* Owned = ItemsByOwner.Find(bItemsOwnedByParent ? Root->OwnerRef : Root->UnitId))
					Items = *Owned;
				Items.Sort([](const FUnitDraft& Left, const FUnitDraft& Right)
				{
					return Left.UnitId < Right.UnitId;
				});
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
				TMap<FString, int32> Occurrences;
				TSet<FString> UsedAliases;
				TSet<FString> ReservedAliases;
				TMap<FString, FString> NamesByItem;
				for (const FUnitDraft* Item : Items)
				{
					auto ReadItemOrChildString = [&PropertiesByOwner, Item](const TCHAR* Field)
					{
						FString Value = ReadDraftString(*Item, Field);
						if (!Value.IsEmpty())
						{
							return Value;
						}
						if (const auto* Children = PropertiesByOwner.Find(Item->UnitId))
						for (const FUnitDraft* Child : *Children)
						{
							Value = ReadDraftString(*Child, Field);
							if (!Value.IsEmpty())
							{
								return Value;
							}
						}
						return FString();
					};
					FString SemanticName;
					for (const FString& Field : NamingPolicy.Fields)
					{
						SemanticName = ReadItemOrChildString(*Field);
						if (!SemanticName.IsEmpty()) break;
					}
					if (SemanticName.IsEmpty())
					{
						SemanticName = NamingPolicy.FallbackName;
					}
					NamesByItem.Add(Item->UnitId, SemanticName);
					ReservedAliases.Add(SemanticName.TrimStartAndEnd().ToLower());
				}
				for (const FUnitDraft* Item : Items)
				{
					const FString& SemanticName = NamesByItem.FindChecked(Item->UnitId);
					FString Alias;
					if (!TryAllocateReadableKeyedItemAlias(
							SemanticName, Occurrences, UsedAliases, Alias, OutError, &ReservedAliases))
					{
						return false;
					}
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
					for (const auto& Field : Item->Values)
					{
						if ((Field.Key == TEXT("name") || Field.Key == TEXT("label"))
							&& !Contract.ImplicitCapturedFields.Contains(Field.Key)
							&& Field.Value->GetKind() == EUeAgentValueKind::String
							&& Field.Value->GetText() == Alias)
						{
							continue;
						}
						Fields.Emplace(Field.Key, Field.Value);
					}
					if (bItemsOwnedByParent)
					{
						// The component's machine selectors stay captured; its public
						// type is context, while new rows use the native candidate name.
						if (const auto* Class = Item->Values.Find(TEXT("component_class")))
							Fields.Emplace(TEXT("class"), *Class);
					}
					if (const auto* Children = PropertiesByOwner.Find(Item->UnitId))
					for (const FUnitDraft* Child : *Children)
					{
						for (const auto& Field : Child->Values)
						{
							Fields.Emplace(Field.Key, Field.Value);
						}
					}
					FUeAgentCanonicalValuePtr Operation;
					FUeAgentCanonicalValuePtr Row;
					FUeAgentStateSemanticError Error;
					if (!FUeAgentCanonicalValue::TryString(
							TEXT("update"), Resources, Operation, Error))
					{
						OutError.SetFailure(Error, FString());
						return false;
					}
					Fields.RemoveAll([](const auto& Field) { return Field.Key == TEXT("operation"); });
					Fields.Emplace(TEXT("operation"), Operation.ToSharedRef());
					if (!Contract.AggregatedItemSemanticSuffix.IsEmpty())
					{
						FUeAgentCanonicalValuePtr Binding;
						if (!FUeAgentCanonicalValue::TryString(Item->UnitId, Resources, Binding, Error))
						{
							OutError.SetFailure(Error, FString());
							return false;
						}
						Fields.Emplace(TEXT("unit_id"), Binding.ToSharedRef());
					}
					if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Row, Error))
					{
						OutError.SetFailure(Error, FString());
						return false;
					}
					Rows.Emplace(Alias, Row.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr ItemMap;
				FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalValue::TryObject(Rows, Resources, ItemMap, Error))
				{
					OutError.SetFailure(Error, FString());
					return false;
				}
				Root->Values.Add(RootField, ItemMap.ToSharedRef());
			}
			return true;
		}

		bool TryAddBlueprintGraphDeclarations(TArray<FUnitDraft>& Drafts,
			FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
		{
			FUnitDraft* Root = Drafts.FindByPredicate([](const FUnitDraft& Draft) {
				return Draft.SemanticTypeRef == TEXT("uai.authoring.surface.graph_collection"); });
			if (!Root) { OutError.SetSourceFailure(TEXT("composition_graph_collection_missing"), TEXT("The captured Blueprint has no graph-collection surface for graph declarations.")); return false; }
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
			TSet<FString> Names;
			for (const FUnitDraft& Draft : Drafts)
			{
				if (Draft.SemanticTypeRef != TEXT("uai.semantic.blueprint.graph")) continue;
				const auto* Header = Draft.Values.Find(TEXT("graph"));
				if (!Header || (*Header)->GetKind() != EUeAgentValueKind::Object) continue;
				const auto& Fields = (*Header)->GetObjectItems();
				const auto* Owned = Fields.FindByPredicate([](const auto& F) { return F.Key == TEXT("graph_declaration_owned"); });
				if (!Owned || Owned->Value->GetKind() != EUeAgentValueKind::Boolean || !Owned->Value->GetBoolean()) continue;
				const auto* Name = Fields.FindByPredicate([](const auto& F) { return F.Key == TEXT("name"); });
				const auto* Kind = Fields.FindByPredicate([](const auto& F) { return F.Key == TEXT("graph_kind"); });
				const auto* Id = Fields.FindByPredicate([](const auto& F) { return F.Key == TEXT("graph_guid"); });
				if (!Name || !Kind || !Id || Name->Value->GetKind() != EUeAgentValueKind::String
					|| Names.Contains(Name->Value->GetText().ToLower()))
				{ OutError.SetSourceFailure(TEXT("composition_graph_declaration_identity_invalid"),
					!Name ? TEXT("Graph declaration is missing name.") : !Kind ? TEXT("Graph declaration is missing graph_kind.")
					: !Id ? TEXT("Graph declaration has no captured graph identity.") : Name->Value->GetKind() != EUeAgentValueKind::String
					? TEXT("Graph declaration name must be a string.") : FString::Printf(TEXT("Graph declaration name '%s' is duplicated."), *Name->Value->GetText()), Draft.UnitId, TEXT("/graph")); return false; }
				Names.Add(Name->Value->GetText().ToLower());
				FUeAgentCanonicalValuePtr Row;
				FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalValue::TryObject({{TEXT("name"), Name->Value}, {TEXT("graph_kind"), Kind->Value},
					{TEXT("graph_guid"), Id->Value}}, Resources, Row, Error))
				{ OutError.SetFailure(Error, FString()); return false; }
				Rows.Emplace(Name->Value->GetText(), Row.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Collection;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(Rows, Resources, Collection, Error))
			{ OutError.SetFailure(Error, FString()); return false; }
			Root->Values.Add(TEXT("graph_declarations"), Collection.ToSharedRef());
			return true;
		}

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString Sha256Text(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return Sha256(Bytes);
		}

		FString StableId(
			const FString& Prefix,
			const FString& Identity)
		{
			return Prefix + TEXT(":") + Sha256Text(Identity).Right(24);
		}

		FString CanonicalValueKindName(const EUeAgentValueKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return TEXT("unknown");
			}
		}

		FString CanonicalAuthoringTargetKey(const FUeAgentStateEntry& Entry)
		{
			const FString NativeKey = Entry.GetIdentity().GetSelector(CanonicalAuthoringTargetIdentitySelector);
			return NativeKey.IsEmpty() ? Entry.GetIdentity().GetCanonicalKey() : NativeKey;
		}

		TSharedPtr<FJsonObject> CanonicalTypeJson(
			const FUeAgentCanonicalType& Type)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("kind"), CanonicalValueKindName(Type.GetValueKind()));
			Result->SetStringField(TEXT("type_id"), Type.GetTypeId());
			if (Type.IsNullable()) Result->SetBoolField(TEXT("nullable"), true);
			if (!Type.GetContainerKind().IsEmpty())
			{
				Result->SetStringField(TEXT("container_kind"), Type.GetContainerKind());
			}
			return Result;
		}

		bool ContractEquals(
			const FTargetCompositionContract& Left,
			const FTargetCompositionContract& Right, UE::UAI::Contract::FDiagnostic& OutError)
		{
			auto Mismatch = [&](const FString& Path, TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_composition_contract_conflict"),
					FString::Printf(TEXT("Registered and proposed composition contracts disagree at '%s'."), *Path), TEXT("composition_contract"));
				OutError.CandidateName = Right.TargetKind;
				OutError.PropertyPath = Path;
				OutError.ExpectedValue = MoveTemp(Expected);
				OutError.ActualValue = MoveTemp(Actual);
				return false;
			};
			auto String = [](const FString& Value) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueString>(Value); };
			auto Number = [](int32 Value) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueNumber>(Value); };
			auto Strings = [](const auto& Values) -> TSharedPtr<FJsonValue> {
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const auto& Value : Values) Items.Add(MakeShared<FJsonValueString>(Value));
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			};
			if (Left.SchemaId != Right.SchemaId) return Mismatch(TEXT("/schema"), String(Left.SchemaId), String(Right.SchemaId));
			if (Left.TargetKind != Right.TargetKind) return Mismatch(TEXT("/target_kind"), String(Left.TargetKind), String(Right.TargetKind));
			if (Left.Surfaces.Num() != Right.Surfaces.Num()) return Mismatch(TEXT("/surfaces/count"), Number(Left.Surfaces.Num()), Number(Right.Surfaces.Num()));
			if (Left.SemanticRoutes.Num() != Right.SemanticRoutes.Num()) return Mismatch(TEXT("/semantic_routes/count"), Number(Left.SemanticRoutes.Num()), Number(Right.SemanticRoutes.Num()));
			if (Left.DocumentValueFields.Num() != Right.DocumentValueFields.Num()) return Mismatch(TEXT("/document_value_fields/count"), Number(Left.DocumentValueFields.Num()), Number(Right.DocumentValueFields.Num()));
			for (const auto& Fields : Left.DocumentValueFields)
			{
				const auto* Other = Right.DocumentValueFields.Find(Fields.Key);
				if (!Other || *Other != Fields.Value)
					return Mismatch(TEXT("/document_value_fields/") + EscapeJsonPointerSegment(Fields.Key), Strings(Fields.Value), Other ? Strings(*Other) : nullptr);
			}
			for (int32 Index = 0; Index < Left.Surfaces.Num(); ++Index)
			{
				const auto& L = Left.Surfaces[Index]; const auto& R = Right.Surfaces[Index];
				const FString Path = FString::Printf(TEXT("/surfaces/%d"), Index);
				if (L.SurfaceId != R.SurfaceId) return Mismatch(Path + TEXT("/surface_id"), String(L.SurfaceId), String(R.SurfaceId));
				if (L.SurfaceKind != R.SurfaceKind) return Mismatch(Path + TEXT("/surface_kind"), Number(static_cast<int32>(L.SurfaceKind)), Number(static_cast<int32>(R.SurfaceKind)));
				if (L.UnitKind != R.UnitKind) return Mismatch(Path + TEXT("/unit_kind"), String(LexToString(L.UnitKind)), String(LexToString(R.UnitKind)));
			}
			for (int32 Index = 0; Index < Left.SemanticRoutes.Num(); ++Index)
			{
				const FCompositionSemanticRouteContract& LeftRoute =
					Left.SemanticRoutes[Index];
				const FCompositionSemanticRouteContract& RightRoute =
					Right.SemanticRoutes[Index];
				const FString Path = FString::Printf(TEXT("/semantic_routes/%d"), Index);
				if (LeftRoute.Match != RightRoute.Match) return Mismatch(Path + TEXT("/match"), Number(static_cast<int32>(LeftRoute.Match)), Number(static_cast<int32>(RightRoute.Match)));
				if (LeftRoute.Pattern != RightRoute.Pattern) return Mismatch(Path + TEXT("/pattern"), String(LeftRoute.Pattern), String(RightRoute.Pattern));
				if (LeftRoute.SurfaceId != RightRoute.SurfaceId) return Mismatch(Path + TEXT("/surface_id"), String(LeftRoute.SurfaceId), String(RightRoute.SurfaceId));
				if (LeftRoute.UnitKind != RightRoute.UnitKind) return Mismatch(Path + TEXT("/unit_kind"), String(LexToString(LeftRoute.UnitKind)), String(LexToString(RightRoute.UnitKind)));
				if (LeftRoute.Role != RightRoute.Role) return Mismatch(Path + TEXT("/role"), Number(static_cast<int32>(LeftRoute.Role)), Number(static_cast<int32>(RightRoute.Role)));
				if (LeftRoute.Priority != RightRoute.Priority) return Mismatch(Path + TEXT("/priority"), Number(LeftRoute.Priority), Number(RightRoute.Priority));
			}
			return true;
		}

		FCompositionSurfaceContract Surface(
			const TCHAR* Id,
			EAuthoringSurfaceKind Kind,
			ESemanticUnitKind UnitKind)
		{
			FCompositionSurfaceContract Result;
			Result.SurfaceId = Id;
			Result.SurfaceKind = Kind;
			Result.UnitKind = UnitKind;
			return Result;
		}

		FTargetCompositionContract Contract(
			const TCHAR* TargetKind,
			TArray<FCompositionSurfaceContract> Surfaces,
			TArray<FCompositionSemanticRouteContract> SemanticRoutes = {})
		{
			FTargetCompositionContract Result;
			Result.SchemaId = CompositionSchema;
			Result.TargetKind = TargetKind;
			Result.Surfaces = MoveTemp(Surfaces);
			Result.SemanticRoutes = MoveTemp(SemanticRoutes);
			return Result;
		}

		bool TryCanonicalizeJson(
			const TSharedPtr<FJsonValue>& Root,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			OutValue.Reset();
			if (!Root.IsValid())
			{
				OutError.SetSourceFailure(TEXT("composition_json_value_invalid"), TEXT("The composition value is missing and cannot be converted to canonical form."));
				return false;
			}
			struct FVisit
			{
				TSharedPtr<FJsonValue> Value;
				bool bExpanded = false;
				FString Path;
			};
			TArray<FVisit> Stack;
			TArray<TSharedPtr<FJsonValue>> Order;
			TSet<const FJsonValue*> Expanded;
			TMap<const FJsonValue*, FString> Paths;
			Stack.Add({Root, false, TEXT("")});
			while (!Stack.IsEmpty())
			{
				FVisit Visit = Stack.Pop(EAllowShrinking::No);
				if (!Visit.Value.IsValid())
				{
					OutError.SetSourceFailure(TEXT("composition_json_value_invalid"), TEXT("The composition contains a missing child value."), FString(), Visit.Path);
					return false;
				}
				const FJsonValue* Key = Visit.Value.Get();
				if (Visit.bExpanded)
				{
					Order.Add(Visit.Value);
					continue;
				}
				if (Expanded.Contains(Key))
				{
					continue;
				}
				Expanded.Add(Key);
				Paths.Add(Key, Visit.Path);
				Stack.Add({Visit.Value, true, Visit.Path});
				if (Visit.Value->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Items =
						Visit.Value->AsArray();
					for (int32 Index = Items.Num() - 1;
						Index >= 0;
						--Index)
					{
						Stack.Add({Items[Index], false, Visit.Path + TEXT("/") + FString::FromInt(Index)});
					}
				}
				else if (Visit.Value->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> Object =
						Visit.Value->AsObject();
					if (!Object.IsValid())
					{
						OutError.SetSourceFailure(TEXT("composition_json_object_invalid"), TEXT("A composition value declares object type but has no object storage."), FString(), Visit.Path);
						return false;
					}
					TArray<FString> Keys;
					Object->Values.GetKeys(Keys);
					Keys.Sort();
					for (int32 Index = Keys.Num() - 1;
						Index >= 0;
						--Index)
					{
						Stack.Add({
							Object->Values.FindChecked(Keys[Index]),
							false, Visit.Path + TEXT("/") + EscapeJsonPointerSegment(Keys[Index])});
					}
				}
			}

			TMap<const FJsonValue*, FUeAgentCanonicalValuePtr> Built;
			for (const TSharedPtr<FJsonValue>& Value : Order)
			{
				FUeAgentCanonicalValuePtr Canonical;
				FUeAgentStateSemanticError SemanticError;
				bool bBuilt = false;
				switch (Value->Type)
				{
				case EJson::Null:
				case EJson::None:
					bBuilt = FUeAgentCanonicalValue::TryNull(
						Resources,
						Canonical,
						SemanticError);
					break;
				case EJson::Boolean:
					bBuilt = FUeAgentCanonicalValue::TryBoolean(
						Value->AsBool(),
						Resources,
						Canonical,
						SemanticError);
					break;
				case EJson::String:
					bBuilt = FUeAgentCanonicalValue::TryString(
						Value->AsString(),
						Resources,
						Canonical,
						SemanticError);
					break;
				case EJson::Number:
				{
					UE::UAI::Contract::FDiagnostic NumericError;
					if (!CanonicalJson::TryFromJsonValue(Value, Resources, Canonical, NumericError))
					{
						OutError.SetFailure(NumericError, FString(), Paths.FindChecked(Value.Get()));
						return false;
					}
					bBuilt = true;
					break;
				}
				case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: Value->AsArray())
					{
						const FUeAgentCanonicalValuePtr* Found =
							Built.Find(Item.Get());
						if (!Found || !Found->IsValid())
						{
							OutError.SetSourceFailure(TEXT("composition_json_child_missing"), TEXT("Array child was not converted before its parent; the input may contain a cyclic reference."),
								FString(), Paths.FindChecked(Value.Get()) + TEXT("/") + FString::FromInt(Items.Num()));
							return false;
						}
						Items.Add(Found->ToSharedRef());
					}
					bBuilt = FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						Canonical,
						SemanticError);
					break;
				}
				case EJson::Object:
				{
					const TSharedPtr<FJsonObject> Object =
						Value->AsObject();
					TArray<FString> Keys;
					Object->Values.GetKeys(Keys);
					Keys.Sort();
					TArray<TPair<
						FString,
						FUeAgentCanonicalValueRef>> Items;
					for (const FString& ChildKey : Keys)
					{
						const TSharedPtr<FJsonValue>& Child =
							Object->Values.FindChecked(ChildKey);
						const FUeAgentCanonicalValuePtr* Found =
							Built.Find(Child.Get());
						if (!Found || !Found->IsValid())
						{
							OutError.SetSourceFailure(TEXT("composition_json_child_missing"), TEXT("Object child was not converted before its parent; the input may contain a cyclic reference."),
								FString(), Paths.FindChecked(Value.Get()) + TEXT("/") + EscapeJsonPointerSegment(ChildKey));
							return false;
						}
						Items.Emplace(
							ChildKey,
							Found->ToSharedRef());
					}
					bBuilt = FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						Canonical,
						SemanticError);
					break;
				}
				default:
					break;
				}
				if (!bBuilt || !Canonical.IsValid())
				{
					OutError.SetFailure(SemanticError.Code.IsEmpty()
							? UE::UAI::Contract::FDiagnostic::Failure(TEXT("composition_canonical_value_failed"), TEXT("The composition value could not be represented as a canonical value."), TEXT("authoring_composition"))
							: SemanticError, FString(), Paths.FindChecked(Value.Get()));
					return false;
				}
				Built.Add(Value.Get(), Canonical);
			}
			const FUeAgentCanonicalValuePtr* Result =
				Built.Find(Root.Get());
			if (!Result || !Result->IsValid())
			{
				OutError.SetSourceFailure(TEXT("composition_canonical_root_missing"), TEXT("Canonical conversion completed without producing its root value."));
				return false;
			}
			OutValue = *Result;
			return true;
		}
		bool TryProjectCanonicalAuthoringDescriptor(const FUeAgentStateEntry& Entry,
			FCompositionEntryView& View, FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
		{
			if (!Entry.GetAuthoringDescriptor().IsValid()) return true;
			TSharedPtr<FJsonValue> Json, NativeValue; UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryToJsonValue(*Entry.GetAuthoringDescriptor(), Json, Error)
				|| !Json.IsValid() || Json->Type != EJson::Object)
			{ OutError.SetFailure(Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_descriptor_invalid"), TEXT("The authoring descriptor must convert to a JSON object."), TEXT("authoring_composition")) : Error, Entry.GetIdentity().GetCanonicalKey()); return false; }
			const auto Descriptor = Json->AsObject(); const TSharedPtr<FJsonObject>* Type = nullptr;
			if (!Descriptor->TryGetObjectField(TEXT("type"), Type) || !Type || !Type->IsValid())
			{ OutError.SetSourceFailure(TEXT("authoring_descriptor_type_missing"), TEXT("The authoring descriptor requires an object type definition."), Entry.GetIdentity().GetCanonicalKey(), TEXT("/type")); return false; }
			View.Type = *Type;
			View.CandidatesHeader = Descriptor->TryGetField(TEXT("candidates_header"));
			View.Candidates = Descriptor->TryGetField(TEXT("candidates"));
			View.ValueDefinition = Descriptor->TryGetField(TEXT("value_definition"));
			if (!CanonicalJson::TryToJsonValue(Entry.GetValue().Get(), NativeValue, Error))
			{ OutError.SetFailure(Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_descriptor_value_invalid"), TEXT("The captured property value could not be represented as JSON."), TEXT("authoring_composition")) : Error, Entry.GetIdentity().GetCanonicalKey()); return false; }
			TSharedPtr<FJsonValue> Projected;
			View.bAuthoringVisible &= UeAgentPropertyValueCodec::ProjectAuthoringValue(
				View.Type, NativeValue, View.ValueDefinition, Projected, View.ValueDefinition);
			return !Projected.IsValid() || TryCanonicalizeJson(Projected, Resources, View.Value, OutError);
		}


		bool TryBuildValues(
			const TSharedPtr<FJsonValue>& Value,
			const TSet<FString>& Excluded,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FWorkspaceError& OutError)
		{
			OutValues.Reset();
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				if (!Object.IsValid())
				{
					OutError.SetSourceFailure(TEXT("composition_json_object_invalid"), TEXT("A composition value declares object type but has no object storage."));
					return false;
				}
				TArray<FString> Keys;
				Object->Values.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					if (Excluded.Contains(Key))
					{
						continue;
					}
					FUeAgentCanonicalValuePtr Canonical;
					if (!TryCanonicalizeJson(
						Object->Values.FindChecked(Key),
						Resources,
						Canonical,
						OutError))
					{
						return false;
					}
					OutValues.Add(Key, Canonical.ToSharedRef());
				}
				return true;
			}
			FUeAgentCanonicalValuePtr Canonical;
			if (!TryCanonicalizeJson(
				Value,
				Resources,
				Canonical,
				OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("value"), Canonical.ToSharedRef());
			return true;
		}

		bool TryBuildValues(
			const FUeAgentCanonicalValuePtr& Value,
			const TSet<FString>& Excluded,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FWorkspaceError& OutError)
		{
			OutValues.Reset();
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->GetKind() == EUeAgentValueKind::Object)
			{
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
					: Value->GetObjectItems())
				{
					if (!Excluded.Contains(Field.Key))
					{
						OutValues.Add(Field.Key, Field.Value);
					}
				}
				return true;
			}
			OutValues.Add(TEXT("value"), Value.ToSharedRef());
			return true;
		}

		bool TryStringArrayValue(
			const TArray<FString>& Values,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError, const FString& SourceUnit, const FString& Field)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FString& Value : Values)
			{
				FUeAgentCanonicalValuePtr Item;
				FUeAgentStateSemanticError SemanticError;
				if (!FUeAgentCanonicalValue::TryString(
					Value,
					Resources,
					Item,
					SemanticError))
				{
					if (SemanticError.PropertyPath.IsEmpty()) SemanticError.PropertyPath = Field + TEXT("/") + FString::FromInt(Items.Num());
					OutError.SetFailure(SemanticError, SourceUnit);
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryArray(
				Items,
				Resources,
				OutValue,
				SemanticError))
			{
				if (SemanticError.PropertyPath.IsEmpty()) SemanticError.PropertyPath = Field;
				OutError.SetFailure(SemanticError, SourceUnit);
				return false;
			}
			return true;
		}

		FString Selector(
			const FCompositionEntryView& Entry,
			const TCHAR* Name)
		{
			return Entry.TargetIdentity.GetSelector(Name);
		}

		FString GraphSelectorKey(
			const FCompositionEntryView& Entry)
		{
			const FString Asset = Selector(Entry, TEXT("asset_path"));
			FString Graph = Selector(Entry, TEXT("graph_guid"));
			if (Graph.IsEmpty())
			{
				Graph = Selector(Entry, TEXT("graph_name"));
			}
			if (Graph.IsEmpty())
			{
				const FString Usage =
					Selector(Entry, TEXT("usage"));
				const FString UsageId =
					Selector(Entry, TEXT("usage_id"));
				if (!Usage.IsEmpty() || !UsageId.IsEmpty())
				{
					Graph = TEXT("stage=") + Selector(Entry, TEXT("stage_owner_id"))
						+ TEXT("|") + Usage
						+ TEXT("@") + UsageId;
				}
			}
			return Asset + TEXT("|") + Graph;
		}

		FString ItemSelectorKey(
			const FCompositionEntryView& Entry)
		{
			FString Scope = Selector(Entry, TEXT("asset_path"));
			if (Scope.IsEmpty())
			{
				Scope = Selector(Entry, TEXT("world_path"));
			}
			const FString OwnerActorGuid =
				Selector(Entry, TEXT("owner_actor_guid"));
			const FString OwnerManagedId =
				Selector(Entry, TEXT("owner_managed_id"));
			const FString ComponentName =
				Selector(Entry, TEXT("component_name"));
			if (!ComponentName.IsEmpty()
				&& (!OwnerActorGuid.IsEmpty() || !OwnerManagedId.IsEmpty()))
			{
				return Scope
					+ (!OwnerActorGuid.IsEmpty()
						? TEXT("|owner_actor_guid=") + OwnerActorGuid
						: TEXT("|owner_managed_id=") + OwnerManagedId)
					+ TEXT("|component_name=") + ComponentName;
			}
			for (const TCHAR* Name : {
				TEXT("dynamic_input_guid"),
				TEXT("input_name"),
				TEXT("node_guid"),
				TEXT("node_id"),
				TEXT("expression_guid"),
				TEXT("component_guid"),
				TEXT("component_name"),
				TEXT("widget_name"),
				TEXT("member_guid"),
				TEXT("member_name"),
				TEXT("key_name"),
				TEXT("renderer_name"),
				TEXT("script_usage_id"),
				TEXT("module_guid"),
				TEXT("usage_id"),
				TEXT("emitter_handle_id"),
				TEXT("emitter_name"),
				TEXT("actor_guid"),
				TEXT("managed_id"),
				TEXT("managed_actor_id")})
			{
				const FString Value = Selector(Entry, Name);
				if (!Value.IsEmpty())
				{
					return Scope + TEXT("|") + Name
						+ TEXT("=") + Value;
				}
			}
			return Entry.TargetIdentity.CanonicalKey;
		}

		bool IsTargetSemantic(const FString& SemanticId)
		{
			return SemanticId.Contains(TEXT(".export.target"))
					|| SemanticId.Contains(TEXT(".export.function_target"));
		}

		bool TryResolveDeclaredSemanticRoute(
			const FString& TargetKind,
			const FString& SemanticId,
			FEntryRoute& OutRoute)
		{
			FCompositionSemanticRouteContract Selected;
			if (!TryResolveTargetCompositionSemanticRoute(
				TargetKind,
				SemanticId,
				Selected))
			{
				return false;
			}
			OutRoute = {};
			OutRoute.SurfaceId = Selected.SurfaceId;
			OutRoute.UnitKind = Selected.UnitKind;
			switch (Selected.Role)
			{
			case ECompositionSemanticRouteRole::TargetValues:
				OutRoute.bTargetValues = true;
				break;
			case ECompositionSemanticRouteRole::Graph:
				OutRoute.bGraph = true;
				break;
			case ECompositionSemanticRouteRole::Item:
				OutRoute.bItem = true;
				break;
			case ECompositionSemanticRouteRole::GraphItem:
				OutRoute.bItem = true;
				OutRoute.bGraphItem = true;
				break;
			case ECompositionSemanticRouteRole::Property:
				OutRoute.bProperty = true;
				break;
			case ECompositionSemanticRouteRole::Relation:
				OutRoute.bRelation = true;
				break;
			case ECompositionSemanticRouteRole::Document:
				OutRoute.bDocument = true;
				break;
			default:
				return false;
			}
			return true;
		}

		FEntryRoute RouteEntry(
			const FString& TargetKind,
			const FString& SemanticId,
			const FString& PreviewTarget = FString())
		{
			FEntryRoute Route;
			if (AssetPreview::IsPreviewSemantic(SemanticId))
			{
				Route.SurfaceId = TEXT("preview/") + (PreviewTarget.IsEmpty() ? FString(TEXT("main")) : PreviewTarget)
					+ TEXT("/") + AssetPreview::GroupFromSemantic(SemanticId);
				Route.UnitKind = ESemanticUnitKind::PropertySet;
				Route.bProperty = true;
				return Route;
			}
			if (SemanticId.StartsWith(TEXT("content_graph.summary:"))
				|| SemanticId.StartsWith(TEXT("content_graph.container:"))
				|| SemanticId.StartsWith(TEXT("content_graph.item:"))
				|| SemanticId.StartsWith(TEXT("content_graph.relation:"))
				|| SemanticId.StartsWith(TEXT("content_graph.diagnostic:")))
			{
				return Route;
			}
			if (IsTargetSemantic(SemanticId))
			{
				Route.SurfaceId = TEXT("target_settings");
				Route.UnitKind = ESemanticUnitKind::TargetState;
				Route.bTargetValues = true;
				return Route;
			}
			if (TryResolveDeclaredSemanticRoute(
					TargetKind,
					SemanticId,
					Route))
			{
				return Route;
			}

			if (SemanticId.EndsWith(TEXT(".document"))
				|| SemanticId.EndsWith(TEXT(".profile_state"))
				|| SemanticId.EndsWith(TEXT(".seed")))
			{
				Route.bDocument = true;
				return Route;
			}

			if (TargetKind.StartsWith(TEXT("ue.niagara_")))
			{
				if (SemanticId == TEXT("uai.semantic.niagara.asset")
					|| SemanticId == TEXT("uai.semantic.niagara.system")
					|| SemanticId == TEXT("uai.semantic.niagara.emitter")
					|| SemanticId == TEXT("uai.semantic.niagara.script"))
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::TargetState;
					Route.bTargetValues = true;
				}
				else if (SemanticId.EndsWith(TEXT(".stack_entry")))
				{
					Route.SurfaceId = TEXT("stack_collection");
					Route.UnitKind = ESemanticUnitKind::StructureState;
					Route.bGraph = true;
				}
				else if (SemanticId.EndsWith(TEXT(".module")))
				{
					Route.SurfaceId = TEXT("stack_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".stack_order")))
				{
					Route.SurfaceId = TEXT("stack_collection");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else if (SemanticId.EndsWith(TEXT(".user_parameter")))
				{
					Route.SurfaceId = TEXT("user_parameters");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".dynamic_input")))
				{
					Route.SurfaceId = TEXT("dynamic_input_tree");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".emitter_owner"))
					|| SemanticId.EndsWith(TEXT(".renderer"))
					|| SemanticId.EndsWith(TEXT(".event_handler")))
				{
					Route.SurfaceId = TEXT("emitter_hierarchy");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".emitter_property"))
					|| SemanticId.EndsWith(TEXT(".module_property")))
				{
					Route.SurfaceId = TEXT("module_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			if (TargetKind == TEXT("ue.level.asset"))
			{
				if (SemanticId.EndsWith(TEXT(".world.target")))
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::TargetState;
					Route.bTargetValues = true;
				}
				else if (SemanticId.EndsWith(TEXT(".world.property")))
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".actor.target"))
					|| SemanticId.EndsWith(TEXT(".component.target")))
				{
					Route.SurfaceId =
						TEXT("actor_component_hierarchy");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".actor"))
					|| SemanticId.EndsWith(TEXT(".component"))
					|| SemanticId.EndsWith(TEXT(".property")))
				{
					Route.SurfaceId =
						TEXT("actor_component_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			if (TargetKind == TEXT("ue.blueprint.asset")
				|| TargetKind == TEXT("ue.anim_blueprint.asset"))
			{
				if (SemanticId.Contains(TEXT(".class_default.property")))
				{
					Route.SurfaceId =
						TargetKind == TEXT("ue.blueprint.asset")
							? TEXT("class_defaults")
							: TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".interface")))
				{
					Route.SurfaceId = TEXT("member_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.Contains(TEXT(".member.")))
				{
					Route.SurfaceId = TEXT("member_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.Contains(TEXT(".component.property")))
				{
					Route.SurfaceId =
						TargetKind == TEXT("ue.blueprint.asset")
							? TEXT("component_tree")
							: TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".component")))
				{
					Route.SurfaceId =
						TargetKind == TEXT("ue.blueprint.asset")
							? TEXT("component_tree")
							: TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".graph")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::StructureState;
					Route.bGraph = true;
				}
				else if (SemanticId.Contains(TEXT(".node.property"))
					|| SemanticId.Contains(TEXT(".pin_default")))
				{
					Route.SurfaceId = TEXT("node_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".node")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".edge")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			if (TargetKind == TEXT("ue.widget_blueprint.asset"))
			{
				if (SemanticId.EndsWith(TEXT(".widget_tree")))
				{
					Route.SurfaceId = TEXT("widget_tree");
					Route.UnitKind = ESemanticUnitKind::StructureState;
					Route.bGraph = true;
				}
				else if (SemanticId.EndsWith(TEXT(".widget")))
				{
					Route.SurfaceId = TEXT("widget_tree");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.Contains(TEXT(".widget.property"))
					|| SemanticId.Contains(TEXT(".slot.property")))
				{
					Route.SurfaceId = TEXT("widget_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".binding")))
				{
					Route.SurfaceId = TEXT("binding_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".slot")))
				{
					Route.SurfaceId = TEXT("widget_tree");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else if (SemanticId.EndsWith(TEXT(".animation")))
				{
					Route.SurfaceId = TEXT("animation_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else
				{
					return RouteEntry(
						TEXT("ue.blueprint.asset"),
						SemanticId);
				}
				return Route;
			}

			if (TargetKind == TEXT("ue.pcg_graph.asset"))
			{
				if (SemanticId.EndsWith(TEXT(".graph")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::StructureState;
					Route.bGraph = true;
				}
				else if (SemanticId.Contains(TEXT("node_property")))
				{
					Route.SurfaceId = TEXT("node_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".node"))
					|| SemanticId.EndsWith(TEXT(".pin")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".edge")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			Route.SurfaceId = TEXT("target_settings");
			Route.UnitKind = ESemanticUnitKind::PropertySet;
			Route.bProperty = true;
			return Route;
		}

		FUeAgentCanonicalValuePtr FindLayoutValue(
			const FCompositionEntryView& Entry)
		{
			if (!Entry.Value.IsValid()
				|| Entry.Value->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Entry.Value->GetObjectItems())
			{
				if (Field.Key == TEXT("pos")
					|| Field.Key == TEXT("position")
					|| Field.Key == TEXT("editor_position"))
				{
					return FUeAgentCanonicalValuePtr(Field.Value);
				}
			}
			return nullptr;
		}

		FString FindGraphOwner(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& GraphUnits,
			const FString& Fallback)
		{
			const FString Key = GraphSelectorKey(Entry);
			if (const FString* Found = GraphUnits.Find(Key))
			{
				return *Found;
			}
			return Fallback;
		}

		FString FindItemOwner(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& ItemUnits,
			const FString& Fallback)
		{
			const FString Key = ItemSelectorKey(Entry);
			if (const FString* Found = ItemUnits.Find(Key))
			{
				return *Found;
			}
			return Fallback;
		}

		FString FindItemOwnerBySelector(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& ItemUnits,
			const TCHAR* SelectorName,
			const FString& Fallback)
		{
			const FString SelectorValue =
				Selector(Entry, SelectorName);
			if (SelectorValue.IsEmpty())
			{
				return Fallback;
			}
			FString Scope = Selector(Entry, TEXT("asset_path"));
			if (Scope.IsEmpty())
			{
				Scope = Selector(Entry, TEXT("world_path"));
			}
			const FString Key = Scope
				+ TEXT("|") + SelectorName + TEXT("=")
				+ SelectorValue;
			if (const FString* Found = ItemUnits.Find(Key))
			{
				return *Found;
			}
			return Fallback;
		}

		FString FindItemOwnerByAliasedSelector(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& ItemUnits,
			const TCHAR* SourceSelectorName,
			const TCHAR* OwnerSelectorName,
			const FString& Fallback)
		{
			const FString SelectorValue =
				Selector(Entry, SourceSelectorName);
			if (SelectorValue.IsEmpty())
			{
				return Fallback;
			}
			FString Scope = Selector(Entry, TEXT("asset_path"));
			if (Scope.IsEmpty())
			{
				Scope = Selector(Entry, TEXT("world_path"));
			}
			const FString Key = Scope + TEXT("|") + OwnerSelectorName
				+ TEXT("=") + SelectorValue;
			if (const FString* Found = ItemUnits.Find(Key))
			{
				return *Found;
			}
			return Fallback;
		}

		FString FindDeclaredItemOwner(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& SourceItemUnits)
		{
			const FString OwnerItemId =
				Selector(Entry, TEXT("owner_item_id"));
			if (OwnerItemId.IsEmpty())
			{
				return FString();
			}
			const FString Key =
				Selector(Entry, TEXT("asset_path"))
				+ TEXT("|item_id=") + OwnerItemId;
			const FString* Found = SourceItemUnits.Find(Key);
			return Found ? *Found : FString();
		}

		FString NormalizeRoleToken(FString Value)
		{
			Value.ToLowerInline();
			Value.ReplaceInline(TEXT("-"), TEXT("_"));
			Value.ReplaceInline(TEXT(" "), TEXT("_"));
			if (Value.EndsWith(TEXT("ies")))
			{
				Value.LeftChopInline(3);
				Value += TEXT("y");
			}
			else if (Value.EndsWith(TEXT("s"))
				&& !Value.EndsWith(TEXT("ss")))
			{
				Value.LeftChopInline(1);
			}
			return Value;
		}

		FString ChooseDocumentSurface(
			const FTargetCompositionContract& CompositionContract,
			const FString& RootRole,
			const bool bStructured)
		{
			const FString Role = NormalizeRoleToken(RootRole);
			const auto HasAny =
				[&Role](std::initializer_list<const TCHAR*> Tokens)
				{
					for (const TCHAR* Token : Tokens)
					{
						if (Role.Contains(Token))
						{
							return true;
						}
					}
					return false;
				};
			const EAuthoringSurfaceKind PreferredKind =
				HasAny({TEXT("movie_scene"), TEXT("timeline"),
					TEXT("track"), TEXT("section"), TEXT("key"),
					TEXT("curve")})
					? EAuthoringSurfaceKind::Timeline
				: HasAny({TEXT("hierarchy"), TEXT("element"),
					TEXT("bone"), TEXT("goal"), TEXT("chain"),
					TEXT("actor"), TEXT("component")})
					? EAuthoringSurfaceKind::Hierarchy
				: HasAny({TEXT("graph"), TEXT("node"),
					TEXT("edge"), TEXT("link")})
					? EAuthoringSurfaceKind::Structure
				: HasAny({TEXT("payload"), TEXT("source"),
					TEXT("import")})
					? EAuthoringSurfaceKind::ExternalPayload
					: EAuthoringSurfaceKind::Items;

			for (const FCompositionSurfaceContract& SurfaceContract
				: CompositionContract.Surfaces)
			{
				const FString SurfaceRole =
					NormalizeRoleToken(SurfaceContract.SurfaceId);
				if (SurfaceRole.Contains(Role)
					|| (!Role.IsEmpty()
						&& Role.Contains(SurfaceRole)))
				{
					return SurfaceContract.SurfaceId;
				}
			}
			for (const FCompositionSurfaceContract& SurfaceContract
				: CompositionContract.Surfaces)
			{
				if (SurfaceContract.SurfaceKind == PreferredKind)
				{
					return SurfaceContract.SurfaceId;
				}
			}
			if (!bStructured)
			{
				return TEXT("target_settings");
			}
			for (const FCompositionSurfaceContract& SurfaceContract
				: CompositionContract.Surfaces)
			{
				if (SurfaceContract.SurfaceKind
						== EAuthoringSurfaceKind::Properties
					&& SurfaceContract.SurfaceId
						!= TEXT("target_settings"))
				{
					return SurfaceContract.SurfaceId;
				}
			}
			for (const FCompositionSurfaceContract& SurfaceContract
				: CompositionContract.Surfaces)
			{
				if (SurfaceContract.SurfaceKind
					!= EAuthoringSurfaceKind::Properties)
				{
					return SurfaceContract.SurfaceId;
				}
			}
			return TEXT("target_settings");
		}

		bool IsRelationDocumentObject(
			const FString& RoleHint,
			const FUeAgentCanonicalValuePtr& Object)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return false;
			}
			const FString Role = NormalizeRoleToken(RoleHint);
			const bool bRelationRole =
				Role.Contains(TEXT("edge"))
				|| Role.Contains(TEXT("link"))
				|| Role.Contains(TEXT("relation"));
			bool bFromNode = false;
			bool bToNode = false;
			bool bSource = false;
			bool bTarget = false;
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Object->GetObjectItems())
			{
				bFromNode |= Field.Key == TEXT("from_node");
				bToNode |= Field.Key == TEXT("to_node");
				bSource |= Field.Key == TEXT("source");
				bTarget |= Field.Key == TEXT("target");
			}
			return bRelationRole
				|| (bFromNode && bToNode)
				|| (bSource && bTarget);
		}

		bool IsStructuredValue(const FUeAgentCanonicalValueRef& Value)
		{
			return Value->GetKind() == EUeAgentValueKind::Object
				|| Value->GetKind() == EUeAgentValueKind::Array;
		}

		bool IsScalarCollection(const FUeAgentCanonicalValueRef& Value)
		{
			if (Value->GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item : Value->GetArrayItems())
			{
				if (IsStructuredValue(Item))
				{
					return false;
				}
			}
			return true;
		}

		const FUeAgentCanonicalValueRef* FindObjectField(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Name)
		{
			if (Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Object->GetObjectItems())
			{
				if (Field.Key == Name)
				{
					return &Field.Value;
				}
			}
			return nullptr;
		}

		void AddOwnedReference(
			FUnitDraft& Owner,
			const FUnitDraft& Child)
		{
			switch (Child.UnitKind)
			{
			case ESemanticUnitKind::StructureState:
			case ESemanticUnitKind::ItemState:
			case ESemanticUnitKind::CollectionState:
			case ESemanticUnitKind::EntityState:
				Owner.ItemRefs.AddUnique(Child.UnitId);
				break;
			case ESemanticUnitKind::RelationSet:
				Owner.RelationRefs.AddUnique(Child.UnitId);
				break;
			case ESemanticUnitKind::LayoutState:
				Owner.LayoutRef = Child.UnitId;
				break;
			case ESemanticUnitKind::DependencyState:
				Owner.DependencyRefs.AddUnique(Child.UnitId);
				break;
			default:
				break;
			}
		}

		TArray<TSharedPtr<FJsonValue>> StringArray(
			const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		int64 EstimatedTokens(const TArray<FExportFilePlan>& Files)
		{
			int64 Bytes = 0;
			for (const FExportFilePlan& File : Files)
			{
				if (IsModelVisibleSemanticWorkspacePath(
						File.GetRelativePath()))
				{
					Bytes += File.GetCanonicalUtf8().Num();
				}
			}
			return (Bytes + 3) / 4;
		}

		bool RegisterOne(
			FTargetCompositionContract Value,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			return FTargetCompositionRegistry::Get().RegisterContract(
				MoveTemp(Value),
				OutError);
		}
	}

	FTargetCompositionRegistry&
	FTargetCompositionRegistry::Get()
	{
		static FTargetCompositionRegistry Registry;
		return Registry;
	}

	bool FTargetCompositionRegistry::RegisterContract(
		FTargetCompositionContract ContractValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return RegisterContracts(
			{MoveTemp(ContractValue)},
			OutError);
	}

	bool FTargetCompositionRegistry::RegisterContracts(
		TArray<FTargetCompositionContract> InContracts,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		TMap<FString, FTargetCompositionContract> Pending;
		for (FTargetCompositionContract& ContractValue : InContracts)
		{
			if (!ValidateTargetCompositionContract(
				ContractValue,
				OutError))
			{
				return false;
			}
			if (const FTargetCompositionContract* ExistingPending =
				Pending.Find(ContractValue.TargetKind))
			{
				if (!ContractEquals(*ExistingPending, ContractValue, OutError))
				{
					return false;
				}
				continue;
			}
			Pending.Add(
				ContractValue.TargetKind,
				MoveTemp(ContractValue));
		}
		FWriteScopeLock Lock(RegistryLock);
		for (const TPair<FString, FTargetCompositionContract>& Pair : Pending)
		{
			if (const FTargetCompositionContract* Existing =
				Contracts.Find(Pair.Key))
			{
				if (!ContractEquals(*Existing, Pair.Value, OutError))
				{
					return false;
				}
			}
		}
		for (TPair<FString, FTargetCompositionContract>& Pair : Pending)
		{
			if (!Contracts.Contains(Pair.Key))
			{
				Contracts.Add(Pair.Key, MoveTemp(Pair.Value));
			}
		}
		return true;
	}

	bool FTargetCompositionRegistry::FindContract(
		const FString& TargetKind,
		FTargetCompositionContract& OutContract) const
	{
		FReadScopeLock Lock(RegistryLock);
		const FTargetCompositionContract* Found =
			Contracts.Find(Normalize(TargetKind));
		if (!Found)
		{
			return false;
		}
		OutContract = *Found;
		return true;
	}

	TArray<FTargetCompositionContract>
	FTargetCompositionRegistry::ListContracts() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FTargetCompositionContract> Result;
		Contracts.GenerateValueArray(Result);
		Result.Sort(
			[](const FTargetCompositionContract& Left,
				const FTargetCompositionContract& Right)
			{
				return Left.TargetKind < Right.TargetKind;
			});
		return Result;
	}

	FString GetTargetCompositionContractSchemaId()
	{
		return CompositionSchema;
	}

	bool TryResolveTargetCompositionSemanticRoute(
		const FString& TargetKind,
		const FString& SemanticId,
		FCompositionSemanticRouteContract& OutRoute)
	{
		OutRoute = {};
		FTargetCompositionContract ContractValue;
		if (!FTargetCompositionRegistry::Get().FindContract(
				TargetKind,
				ContractValue)
			|| ContractValue.SemanticRoutes.IsEmpty())
		{
			return false;
		}
		const FCompositionSemanticRouteContract* Selected = nullptr;
		for (const FCompositionSemanticRouteContract& Candidate :
			ContractValue.SemanticRoutes)
		{
			bool bMatches = false;
			switch (Candidate.Match)
			{
			case ECompositionSemanticRouteMatch::Exact:
				bMatches = SemanticId == Candidate.Pattern;
				break;
			case ECompositionSemanticRouteMatch::Suffix:
				bMatches = SemanticId.EndsWith(Candidate.Pattern);
				break;
			case ECompositionSemanticRouteMatch::Contains:
				bMatches = SemanticId.Contains(Candidate.Pattern);
				break;
			case ECompositionSemanticRouteMatch::Default:
				bMatches = true;
				break;
			default:
				break;
			}
			if (bMatches
				&& (!Selected || Candidate.Priority > Selected->Priority))
			{
				Selected = &Candidate;
			}
		}
		if (!Selected)
		{
			return false;
		}
		OutRoute = *Selected;
		return true;
	}

	bool ValidateTargetCompositionContract(
		const FTargetCompositionContract& ContractValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		auto Reject = [&](const TCHAR* Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("composition_contract"));
			OutError.CandidateName = ContractValue.TargetKind;
			OutError.PropertyPath = Path;
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		};
		if (Normalize(ContractValue.SchemaId) != CompositionSchema
			|| Normalize(ContractValue.TargetKind).IsEmpty()
			|| ContractValue.Surfaces.IsEmpty())
		{
			return Reject(TEXT("target_composition_contract_invalid"), Normalize(ContractValue.SchemaId) != CompositionSchema
				? FString::Printf(TEXT("Composition schema must be '%s'."), *CompositionSchema)
				: Normalize(ContractValue.TargetKind).IsEmpty() ? TEXT("Composition target kind must not be empty.")
				: TEXT("Composition requires at least one surface."), Normalize(ContractValue.SchemaId) != CompositionSchema
				? TEXT("/schema") : Normalize(ContractValue.TargetKind).IsEmpty() ? TEXT("/target_kind") : TEXT("/surfaces"));
		}
		TSet<FString> SurfaceIds;
		bool bHasProperties = false;
		bool bHasCompound = false;
		int32 SurfaceIndex = -1;
		for (const FCompositionSurfaceContract& Item
			: ContractValue.Surfaces)
		{
			++SurfaceIndex;
			const FString SurfaceId = Normalize(Item.SurfaceId);
			if (SurfaceId.IsEmpty()
				|| SurfaceIds.Contains(SurfaceId)
				|| Item.UnitKind
					== ESemanticUnitKind::OpaqueExtension)
			{
				return Reject(TEXT("target_composition_surface_invalid"), SurfaceId.IsEmpty() ? TEXT("Surface identifier must not be empty.")
					: SurfaceIds.Contains(SurfaceId) ? FString::Printf(TEXT("Surface '%s' is declared more than once."), *SurfaceId)
					: FString::Printf(TEXT("Surface '%s' cannot use the opaque-extension unit kind."), *SurfaceId),
					FString::Printf(TEXT("/surfaces/%d"), SurfaceIndex), MakeShared<FJsonValueString>(SurfaceId));
			}
			SurfaceIds.Add(SurfaceId);
			bHasProperties |= Item.SurfaceKind
				== EAuthoringSurfaceKind::Properties;
			bHasCompound |= Item.SurfaceKind
				!= EAuthoringSurfaceKind::Properties;
		}
		if (!SurfaceIds.Contains(TEXT("target_settings"))
			|| !bHasProperties
			|| !bHasCompound)
		{
			return Reject(TEXT("target_composition_surface_coverage_invalid"), !SurfaceIds.Contains(TEXT("target_settings"))
				? TEXT("Composition must declare the target_settings surface.") : !bHasProperties
				? TEXT("Composition must declare a properties surface.") : TEXT("Composition must declare a non-property surface."), TEXT("/surfaces"));
		}
		TSet<int32> Priorities;
		int32 DefaultCount = 0;
		int32 DefaultPriority = MAX_int32;
		int32 LowestSpecificPriority = MAX_int32;
		int32 RouteIndex = -1;
		for (const FCompositionSemanticRouteContract& Route :
			ContractValue.SemanticRoutes)
		{
			++RouteIndex;
			const FString SurfaceId = Normalize(Route.SurfaceId);
			const FString RoutePath = FString::Printf(TEXT("/semantic_routes/%d"), RouteIndex);
			const FString Pattern = Normalize(Route.Pattern);
			if (!SurfaceIds.Contains(SurfaceId))
			{
				return Reject(TEXT("target_composition_semantic_route_surface_missing"), TEXT("The semantic route refers to an undeclared surface."), RoutePath + TEXT("/surface_id"), MakeShared<FJsonValueString>(SurfaceId));
			}
			if (Route.Role == ECompositionSemanticRouteRole::Undeclared
				|| Route.UnitKind == ESemanticUnitKind::OpaqueExtension)
			{
				return Reject(TEXT("target_composition_semantic_route_invalid"), Route.Role == ECompositionSemanticRouteRole::Undeclared
					? TEXT("A semantic route must declare its role.") : TEXT("A semantic route cannot use the opaque-extension unit kind."), RoutePath);
			}
			if (Priorities.Contains(Route.Priority))
			{
				return Reject(TEXT("target_composition_semantic_route_priority_duplicate"), TEXT("Semantic route priorities must be unique."), RoutePath + TEXT("/priority"), MakeShared<FJsonValueNumber>(Route.Priority));
			}
			Priorities.Add(Route.Priority);
			if (Route.Match == ECompositionSemanticRouteMatch::Default)
			{
				if (!Pattern.IsEmpty())
				{
					return Reject(TEXT("target_composition_semantic_route_default_pattern_invalid"), TEXT("A default semantic route must not declare a pattern."), RoutePath + TEXT("/pattern"), MakeShared<FJsonValueString>(Pattern));
				}
				++DefaultCount;
				DefaultPriority = Route.Priority;
			}
			else
			{
				if (Pattern.IsEmpty())
				{
					return Reject(TEXT("target_composition_semantic_route_pattern_missing"), TEXT("A non-default semantic route requires a nonempty pattern."), RoutePath + TEXT("/pattern"));
				}
				LowestSpecificPriority = FMath::Min(
					LowestSpecificPriority,
					Route.Priority);
			}
		}
		if (!ContractValue.SemanticRoutes.IsEmpty()
			&& (DefaultCount != 1
				|| (LowestSpecificPriority != MAX_int32
					&& DefaultPriority >= LowestSpecificPriority)))
		{
			return Reject(TEXT("target_composition_semantic_route_default_invalid"), DefaultCount != 1
				? FString::Printf(TEXT("Exactly one default semantic route is required; found %d."), DefaultCount)
				: FString::Printf(TEXT("Default route priority %d must be below every specific route priority; lowest is %d."), DefaultPriority, LowestSpecificPriority), TEXT("/semantic_routes"));
		}
		return true;
	}

	bool RegisterGraphTreeTargetCompositionContracts(
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TArray<FCompositionSurfaceContract> Blueprint{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("class_defaults"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("member_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("component_tree"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("timeline_collection"),
				EAuthoringSurfaceKind::Timeline,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("node_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Widget{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("class_defaults"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("member_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("widget_tree"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("binding_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("animation_collection"),
				EAuthoringSurfaceKind::Timeline,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("widget_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("node_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> AnimBlueprint{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("member_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("node_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Material{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("parameter_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("expression_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Pcg{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("node_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> TextureGraph{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("parameter_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("node_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> DeformerGraph = TextureGraph;
		const TArray<FCompositionSemanticRouteContract> MaterialRoutes{
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".graph"), TEXT("graph_collection"),
				ESemanticUnitKind::StructureState,
				ECompositionSemanticRouteRole::Graph, 600},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".expression"), TEXT("graph_collection"),
				ESemanticUnitKind::ItemState,
				ECompositionSemanticRouteRole::Item, 500},
			{ECompositionSemanticRouteMatch::Contains,
				TEXT("expression_property"), TEXT("expression_properties"),
				ESemanticUnitKind::PropertySet,
				ECompositionSemanticRouteRole::Property, 400},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".edge"), TEXT("graph_collection"),
				ESemanticUnitKind::RelationSet,
				ECompositionSemanticRouteRole::Relation, 300},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".root_input"), TEXT("graph_collection"),
				ESemanticUnitKind::RelationSet,
				ECompositionSemanticRouteRole::Relation, 250},
			{ECompositionSemanticRouteMatch::Contains,
				TEXT("parameter"), TEXT("parameter_collection"),
				ESemanticUnitKind::ItemState,
				ECompositionSemanticRouteRole::Item, 200},
			{ECompositionSemanticRouteMatch::Default,
				FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet,
				ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSemanticRouteContract> TextureGraphRoutes{
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".graph"), TEXT("graph_collection"),
				ESemanticUnitKind::StructureState,
				ECompositionSemanticRouteRole::Graph, 600},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".node"), TEXT("graph_collection"),
				ESemanticUnitKind::ItemState,
				ECompositionSemanticRouteRole::Item, 500},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".pin"), TEXT("graph_collection"),
				ESemanticUnitKind::ItemState,
				ECompositionSemanticRouteRole::Item, 400},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".node_pin"), TEXT("graph_collection"),
				ESemanticUnitKind::RelationSet,
				ECompositionSemanticRouteRole::Relation, 350},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".edge"), TEXT("graph_collection"),
				ESemanticUnitKind::RelationSet,
				ECompositionSemanticRouteRole::Relation, 300},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".parameter"), TEXT("parameter_collection"),
				ESemanticUnitKind::ItemState,
				ECompositionSemanticRouteRole::Item, 250},
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".pin_value"), TEXT("node_properties"),
				ESemanticUnitKind::PropertySet,
				ECompositionSemanticRouteRole::Property, 200},
			{ECompositionSemanticRouteMatch::Default,
				FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet,
				ECompositionSemanticRouteRole::Property, 0},
		};
		return RegisterOne(
				Contract(TEXT("ue.blueprint.asset"), Blueprint),
				OutError)
			&& RegisterOne(
				Contract(TEXT("ue.widget_blueprint.asset"), Widget),
				OutError)
			&& RegisterOne(
				Contract(
					TEXT("ue.anim_blueprint.asset"),
					AnimBlueprint),
				OutError)
			&& RegisterOne(
				Contract(TEXT("ue.material.asset"), Material, MaterialRoutes),
				OutError)
			&& RegisterOne(
				Contract(
					TEXT("ue.material_function.asset"),
					Material,
					MaterialRoutes),
				OutError)
			&& RegisterOne(
				Contract(
					TEXT("ue.material_instance.asset"),
					Material,
					MaterialRoutes),
				OutError)
			&& RegisterOne(
				Contract(TEXT("ue.pcg_graph.asset"), Pcg),
				OutError)
			&& RegisterOne(
				Contract(TEXT("ue.texture_graph.asset"), TextureGraph,
					TextureGraphRoutes),
			OutError)
			&& RegisterOne(
				Contract(TEXT("ue.deformer_graph.asset"), DeformerGraph,
					TextureGraphRoutes),
				OutError);
	}

	bool RegisterOtherTargetCompositionContracts(
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TArray<FCompositionSurfaceContract> Generic{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("owned_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
		};
		const TArray<FCompositionSurfaceContract> Niagara{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("emitter_hierarchy"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("user_parameters"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("stack_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("dynamic_input_tree"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("module_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Timeline{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("timeline_collection"),
				EAuthoringSurfaceKind::Timeline,
				ESemanticUnitKind::CollectionState),
		};
		const TArray<FCompositionSurfaceContract> Level{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("actor_component_hierarchy"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("actor_component_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Rig{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("rig_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("rig_hierarchy"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("rigvm_graph"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("hierarchy_item_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Behavior{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("behavior_tree"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("item_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Audio{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("audio_graph"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("item_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSemanticRouteContract> AudioGraphRoutes{
			{ECompositionSemanticRouteMatch::Suffix,
				TEXT(".document"), TEXT("audio_graph"),
				ESemanticUnitKind::StructureState,
				ECompositionSemanticRouteRole::Graph, 600},
			{ECompositionSemanticRouteMatch::Default,
				FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet,
				ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSemanticRouteContract> BehaviorRoutes{
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".graph"), TEXT("behavior_tree"),
				ESemanticUnitKind::StructureState, ECompositionSemanticRouteRole::Graph, 630},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".node"), TEXT("behavior_tree"),
				ESemanticUnitKind::ItemState, ECompositionSemanticRouteRole::GraphItem, 620},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".edge"), TEXT("behavior_tree"),
				ESemanticUnitKind::RelationSet, ECompositionSemanticRouteRole::Relation, 610},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".node_property"), TEXT("item_properties"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 600},
			{ECompositionSemanticRouteMatch::Default, FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSurfaceContract> Eqs{
			Surface(TEXT("target_settings"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
			Surface(TEXT("eqs"), EAuthoringSurfaceKind::Structure, ESemanticUnitKind::StructureState),
			Surface(TEXT("item_properties"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSemanticRouteContract> EqsRoutes{
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".graph"), TEXT("eqs"),
				ESemanticUnitKind::StructureState, ECompositionSemanticRouteRole::Graph, 630},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".node"), TEXT("eqs"),
				ESemanticUnitKind::ItemState, ECompositionSemanticRouteRole::GraphItem, 620},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".edge"), TEXT("eqs"),
				ESemanticUnitKind::RelationSet, ECompositionSemanticRouteRole::Relation, 610},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".node_property"), TEXT("item_properties"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 600},
			{ECompositionSemanticRouteMatch::Default, FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSurfaceContract> StateTree{
			Surface(TEXT("target_settings"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
			Surface(TEXT("state_tree"), EAuthoringSurfaceKind::Hierarchy, ESemanticUnitKind::StructureState),
			Surface(TEXT("item_properties"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Blackboard{
			Surface(TEXT("target_settings"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
			Surface(TEXT("blackboard"), EAuthoringSurfaceKind::Collection, ESemanticUnitKind::StructureState),
			Surface(TEXT("item_properties"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSemanticRouteContract> BlackboardRoutes{
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".blackboard.document"), TEXT("blackboard"),
				ESemanticUnitKind::StructureState, ECompositionSemanticRouteRole::Graph, 630},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".blackboard.key"), TEXT("blackboard"),
				ESemanticUnitKind::ItemState, ECompositionSemanticRouteRole::Item, 620},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".blackboard.key_property"), TEXT("item_properties"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 600},
			{ECompositionSemanticRouteMatch::Default, FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSurfaceContract> SmartObject{
			Surface(TEXT("target_settings"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
			Surface(TEXT("smart_object"), EAuthoringSurfaceKind::Collection, ESemanticUnitKind::StructureState),
			Surface(TEXT("item_properties"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSemanticRouteContract> SmartObjectRoutes{
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".smart_object.definition"), TEXT("smart_object"),
				ESemanticUnitKind::StructureState, ECompositionSemanticRouteRole::Graph, 630},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".smart_object.item"), TEXT("smart_object"),
				ESemanticUnitKind::ItemState, ECompositionSemanticRouteRole::Item, 620},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".smart_object.item_property"), TEXT("item_properties"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 600},
			{ECompositionSemanticRouteMatch::Default, FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSemanticRouteContract> StateTreeRoutes{
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".state_tree.tree"), TEXT("state_tree"),
				ESemanticUnitKind::StructureState, ECompositionSemanticRouteRole::Graph, 630},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".state_tree.item"), TEXT("state_tree"),
				ESemanticUnitKind::ItemState, ECompositionSemanticRouteRole::Item, 620},
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".state_tree.item_property"), TEXT("item_properties"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 600},
			{ECompositionSemanticRouteMatch::Default, FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSurfaceContract> Curve{
			Surface(TEXT("target_settings"), EAuthoringSurfaceKind::Properties, ESemanticUnitKind::PropertySet),
			Surface(TEXT("curve"), EAuthoringSurfaceKind::Structure, ESemanticUnitKind::StructureState),
		};
		const TArray<FCompositionSemanticRouteContract> CurveRoutes{
			{ECompositionSemanticRouteMatch::Suffix, TEXT(".profile_state"), TEXT("curve"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 600},
			{ECompositionSemanticRouteMatch::Default, FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSemanticRouteContract> AssetRelationRoutes{
			{ECompositionSemanticRouteMatch::Default, FString(), TEXT("target_settings"),
				ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0},
		};
		const TArray<FCompositionSurfaceContract> Table{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("table_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
		};
		const TArray<FCompositionSurfaceContract> External{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("source_payload"),
				EAuthoringSurfaceKind::ExternalPayload,
				ESemanticUnitKind::ExternalPayloadState),
		};
		if (!RegisterOne(
				Contract(TEXT("ue.metasound.asset"), Audio, AudioGraphRoutes),
				OutError)
			|| !RegisterOne(Contract(TEXT("ue.sound_cue.asset"), Audio, AudioGraphRoutes), OutError)
			|| !RegisterOne(Contract(TEXT("ue.behavior_tree.asset"), Behavior, BehaviorRoutes), OutError)
			|| !RegisterOne(Contract(TEXT("ue.eqs.asset"), Eqs, EqsRoutes), OutError)
			|| !RegisterOne(Contract(TEXT("ue.curve.asset"), Curve, CurveRoutes), OutError)
			|| !RegisterOne(Contract(TEXT("ue.sound_class.asset"), Generic, AssetRelationRoutes), OutError)
			|| !RegisterOne(Contract(TEXT("ue.sound_submix.asset"), Generic, AssetRelationRoutes), OutError)
				|| !RegisterOne(Contract(TEXT("ue.state_tree.asset"), StateTree, StateTreeRoutes), OutError)
				|| !RegisterOne(Contract(TEXT("ue.blackboard.asset"), Blackboard, BlackboardRoutes), OutError)
				|| !RegisterOne(Contract(TEXT("ue.smart_object.asset"), SmartObject, SmartObjectRoutes), OutError))
		{
			return false;
		}
		const TArray<TPair<FString, const TArray<FCompositionSurfaceContract>*>>
			Registrations{
				{TEXT("ue.generic.object"), &Generic},
				{TEXT("ue.project_settings.config"), &Generic},
				{TEXT("ue.physics_asset.asset"), &Generic},
				{TEXT("ue.niagara_system.asset"), &Niagara},
				{TEXT("ue.niagara_emitter.asset"), &Niagara},
				{TEXT("ue.niagara_script.asset"), &Niagara},
				{TEXT("ue.level_sequence.asset"), &Timeline},
				{TEXT("ue.animation_sequence.asset"), &Timeline},
				{TEXT("ue.level.asset"), &Level},
				{TEXT("ue.control_rig.asset"), &Rig},
				{TEXT("ue.ik_rig.asset"), &Rig},
				{TEXT("ue.ik_retargeter.asset"), &Rig},
				{TEXT("ue.skeleton.asset"), &Rig},
				{TEXT("ue.data_table.asset"), &Table},
				{TEXT("ue.curve_table.asset"), &Table},
				{TEXT("ue.string_table.asset"), &Table},
				{TEXT("ue.localization_settings.config"), &Table},
				{TEXT("ue.imported_texture.asset"), &External},
				{TEXT("ue.imported_mesh.asset"), &External},
				{TEXT("ue.imported_audio.asset"), &External},
				{TEXT("ue.imported_csv.asset"), &External},
			};
		for (const TPair<
			FString,
			const TArray<FCompositionSurfaceContract>*>& Registration
			: Registrations)
		{
			TArray<FCompositionSemanticRouteContract> Routes;
			if (Registration.Key == TEXT("ue.generic.object") || Registration.Key == TEXT("ue.project_settings.config"))
			{
				int32 Priority = 600;
				for (const TCHAR* Semantic : {
					TEXT("uai.semantic.animation.anim_sequence.document"),
					TEXT("uai.semantic.animation.anim_montage.document"),
					TEXT("uai.semantic.animation.pose_asset.document"),
					TEXT("uai.semantic.reflection.pcg_data_asset.profile_state"),
					TEXT("uai.semantic.reflection.ml_deformer.profile_state"),
					TEXT("uai.semantic.reflection.material_parameter_collection.profile_state"),
					TEXT("uai.semantic.reflection.material_function_instance.profile_state"),
					TEXT("uai.semantic.reflection.material_layer_instance.profile_state"),
					TEXT("uai.semantic.reflection.material_layer_blend_instance.profile_state")})
					Routes.Add({ECompositionSemanticRouteMatch::Exact, Semantic,
						TEXT("target_settings"), ESemanticUnitKind::PropertySet,
						ECompositionSemanticRouteRole::Property, Priority++});
				int32 DocumentPriority = 500;
				for (const TCHAR* Suffix : {TEXT(".document"), TEXT(".profile_state"), TEXT(".seed")})
					Routes.Add({ECompositionSemanticRouteMatch::Suffix, Suffix,
						TEXT("target_settings"), ESemanticUnitKind::PropertySet,
						ECompositionSemanticRouteRole::Document, DocumentPriority--});
				Routes.Add({ECompositionSemanticRouteMatch::Default, FString(),
					TEXT("target_settings"), ESemanticUnitKind::PropertySet,
					ECompositionSemanticRouteRole::Property, 0});
			}
			if (Registration.Key == TEXT("ue.physics_asset.asset"))
			{
				Routes.Add({ECompositionSemanticRouteMatch::Exact, TEXT("uai.semantic.reflection.physics_asset.profile_state"),
					TEXT("target_settings"), ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 600});
				Routes.Add({ECompositionSemanticRouteMatch::Default, FString(),
					TEXT("target_settings"), ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0});
			}
			if (Registration.Key == TEXT("ue.skeleton.asset"))
			{
				Routes.Add({ECompositionSemanticRouteMatch::Exact, TEXT("uai.semantic.animation.skeleton.document"),
					TEXT("target_settings"), ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 600});
				Routes.Add({ECompositionSemanticRouteMatch::Default, FString(),
					TEXT("target_settings"), ESemanticUnitKind::PropertySet, ECompositionSemanticRouteRole::Property, 0});
			}
			if (Registration.Key == TEXT("ue.imported_texture.asset") || Registration.Key == TEXT("ue.imported_mesh.asset"))
			{
				// Whole input values use the shared property projection, but retain a
				// distinct semantic identity from the generated asset state.
				if (Registration.Key == TEXT("ue.imported_texture.asset"))
				Routes.Add({ECompositionSemanticRouteMatch::Suffix, TEXT(".generation_input"),
					TEXT("source_payload"), ESemanticUnitKind::PropertySet,
					ECompositionSemanticRouteRole::Property, 600});
				if (Registration.Key == TEXT("ue.imported_mesh.asset"))
				{
					int32 MeshPropertyPriority = 600;
					for (const TCHAR* Semantic : {TEXT("uai.semantic.reflection.static_mesh.profile_state"), TEXT("uai.semantic.reflection.skeletal_mesh.profile_state")})
						Routes.Add({ECompositionSemanticRouteMatch::Exact, Semantic,
							TEXT("target_settings"), ESemanticUnitKind::PropertySet,
							ECompositionSemanticRouteRole::Property, MeshPropertyPriority++});
				}
				// Explicit routes replace RouteEntry's fallback. Preserve its document
				// decomposition for all existing texture profiles before the default.
				int32 DocumentPriority = 500;
				for (const TCHAR* Suffix : {TEXT(".document"), TEXT(".profile_state"), TEXT(".seed")})
				{
					Routes.Add({ECompositionSemanticRouteMatch::Suffix, Suffix,
						TEXT("target_settings"), ESemanticUnitKind::PropertySet,
						ECompositionSemanticRouteRole::Document, DocumentPriority--});
				}
				Routes.Add({ECompositionSemanticRouteMatch::Default, FString(),
					TEXT("target_settings"), ESemanticUnitKind::PropertySet,
					ECompositionSemanticRouteRole::Property, 0});
			}
			if (Registration.Key == TEXT("ue.data_table.asset") || Registration.Key == TEXT("ue.curve_table.asset") || Registration.Key == TEXT("ue.string_table.asset")
				|| Registration.Key == TEXT("ue.localization_settings.config"))
			{
				Routes.Add({ECompositionSemanticRouteMatch::Exact,
					Registration.Key == TEXT("ue.data_table.asset") ? TEXT("data_table.rows")
						: Registration.Key == TEXT("ue.curve_table.asset") ? TEXT("curve_table.rows")
						: Registration.Key == TEXT("ue.string_table.asset") ? TEXT("string_table.entries") : TEXT("localization_settings.targets"),
					TEXT("table_collection"), ESemanticUnitKind::StructureState,
					ECompositionSemanticRouteRole::Graph, 600});
				Routes.Add({ECompositionSemanticRouteMatch::Default, FString(),
					TEXT("target_settings"), ESemanticUnitKind::PropertySet,
					ECompositionSemanticRouteRole::Property, 0});
			}
			auto TargetContract = Contract(*Registration.Key, *Registration.Value, Routes);
			if (Registration.Key == TEXT("ue.level_sequence.asset"))
				TargetContract.DocumentValueFields.Add(TEXT("uai.semantic.reflection.sequence.profile_state"),
					{TEXT("display_rate"), TEXT("tick_resolution"), TEXT("playback_range")});
			if (!RegisterOne(MoveTemp(TargetContract), OutError))
			{
				return false;
			}
		}
		return true;
	}

	FString ResolveTargetCompositionKind(
		const FString& SourceTargetKind,
		const FString& SourceTypeId)
	{
		const FString Target = SourceTargetKind.ToLower();
		const FString Type = SourceTypeId.ToLower();
		if (Target.StartsWith(TEXT("ue.")))
		{
			return Target;
		}
		if (Target.StartsWith(TEXT("niagara"))
			|| Type.StartsWith(TEXT("niagara")))
		{
			const FString Kind = Type.IsEmpty() ? Target : Type;
			if (Kind.Contains(TEXT("emitter")))
			{
				return TEXT("ue.niagara_emitter.asset");
			}
			if (Kind.Contains(TEXT("script")))
			{
				return TEXT("ue.niagara_script.asset");
			}
			return TEXT("ue.niagara_system.asset");
		}
		if (Target == TEXT("animation.skeleton")
			|| Type == TEXT("skeleton"))
		{
			return TEXT("ue.skeleton.asset");
		}
		if (Type == TEXT("physics_asset")) return TEXT("ue.physics_asset.asset");
		if (Target == TEXT("animation.skeletal_mesh")
			|| Type == TEXT("skeletal_mesh")
			|| Type == TEXT("static_mesh"))
		{
			return TEXT("ue.imported_mesh.asset");
		}
		if (Target == TEXT("rigging.control_rig")
			|| Type == TEXT("control_rig"))
		{
			return TEXT("ue.control_rig.asset");
		}
		if (Target == TEXT("rigging.ik_rig")
			|| Type == TEXT("ik_rig"))
		{
			return TEXT("ue.ik_rig.asset");
		}
		if (Target == TEXT("rigging.ik_retargeter")
			|| Type == TEXT("ik_retargeter"))
		{
			return TEXT("ue.ik_retargeter.asset");
		}
		if (Target == TEXT("audio.metasound")
			|| Type == TEXT("metasound"))
		{
			return TEXT("ue.metasound.asset");
		}
		if (Target == TEXT("audio.sound_cue")
			|| Type == TEXT("sound_cue"))
		{
			return TEXT("ue.sound_cue.asset");
		}
		if (Target == TEXT("audio.sound_submix")
			|| Type == TEXT("sound_submix"))
		{
			return TEXT("ue.sound_submix.asset");
		}
		if (Type == TEXT("sound_class"))
		{
			return TEXT("ue.sound_class.asset");
		}
		if (Type == TEXT("sequence"))
		{
			return TEXT("ue.level_sequence.asset");
		}
		if (Type == TEXT("behavior_tree"))
		{
			return TEXT("ue.behavior_tree.asset");
		}
		if (Type == TEXT("blackboard"))
		{
			return TEXT("ue.blackboard.asset");
		}
		if (Type == TEXT("eqs"))
		{
			return TEXT("ue.eqs.asset");
		}
		if (Type == TEXT("state_tree"))
		{
			return TEXT("ue.state_tree.asset");
		}
		if (Type == TEXT("smart_object"))
		{
			return TEXT("ue.smart_object.asset");
		}
		if (Type == TEXT("curve_table"))
		{
			return TEXT("ue.curve_table.asset");
		}
		if (Type.Contains(TEXT("curve")))
		{
			return TEXT("ue.curve.asset");
		}
		if (Type == TEXT("data_table"))
		{
			return TEXT("ue.data_table.asset");
		}
		if (Type == TEXT("string_table"))
		{
			return TEXT("ue.string_table.asset");
		}
		if (Type == TEXT("project_settings")) return TEXT("ue.project_settings.config");
		if (Type == TEXT("localization_settings"))
		{
			return TEXT("ue.localization_settings.config");
		}
		if (Type.Contains(TEXT("texture"))
			|| Type == TEXT("paper_sprite")
			|| Type == TEXT("paper_flipbook")
			|| Type == TEXT("paper_tilemap")
			|| Type == TEXT("paper_tileset"))
		{
			return TEXT("ue.imported_texture.asset");
		}
		if (Type == TEXT("material_instance"))
		{
			return TEXT("ue.material_instance.asset");
		}
		return TEXT("ue.generic.object");
	}

	using FCaptureCharges = TMap<EUeAgentResourceDimension, int64>;
	template <typename UsageType>
	static FCaptureCharges CaptureChargesBetween(const UsageType& Before, const UsageType& After)
	{
		FCaptureCharges Charges;
		for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D) {
			const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
			const int64 Delta = After.Consumed(Dimension) - Before.Consumed(Dimension);
			if (Delta > 0) Charges.Add(Dimension, Delta);
		}
		return Charges;
	}
	static FString CaptureEntryKey(const FString& Semantic, const FString& Identity)
	{ return FString::FromInt(Semantic.Len()) + TEXT(":") + Semantic + Identity; }

	static bool TryComposeEntryViews(
		const FCompositionInput& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError,
		TMap<FString, FDocumentValueBinding>* DocumentBindings = nullptr,
		FCaptureCharges* OutPreparationCharges = nullptr)
	{
		const auto PreparationBefore = Resources.Snapshot();
		OutModel.Reset();
		OutError.Reset();
		FTargetCompositionContract CompositionContract;
		if (!FTargetCompositionRegistry::Get().FindContract(
			Request.TargetKind,
			CompositionContract))
		{
			OutError.SetSourceFailure(TEXT("target_composition_contract_missing"),
				FString::Printf(TEXT("No composition contract is registered for target kind '%s'."), *Request.TargetKind));
			return false;
		}
		if (Normalize(Request.ProfileId).IsEmpty()
			|| Normalize(Request.BaseRevision).IsEmpty())
		{
			OutError.SetSourceFailure(TEXT("composition_request_invalid"), Normalize(Request.ProfileId).IsEmpty()
				? TEXT("Composition requires a nonempty profile identifier.") : TEXT("Composition requires a captured base revision."));
			return false;
		}

		TArray<FCompositionEntryView> Entries =
			Request.Entries;
		TSet<FString> PreviewSurfaces;
		for (const auto& Entry : Entries)
		{
			if (!AssetPreview::IsPreviewSemantic(Entry.SemanticId)) continue;
			const FString Surface = RouteEntry(Request.TargetKind, Entry.SemanticId, Selector(Entry, TEXT("preview_target"))).SurfaceId;
			if (PreviewSurfaces.Contains(Surface)) continue;
			PreviewSurfaces.Add(Surface);
		}
		Entries.Sort(
			[](const FCompositionEntryView& Left,
				const FCompositionEntryView& Right)
			{
				if (Left.SemanticId != Right.SemanticId)
				{
					return Left.SemanticId < Right.SemanticId;
				}
				return Left.TargetIdentity.CanonicalKey
					< Right.TargetIdentity.CanonicalKey;
			});

		FString AssetPath;
		TMap<FString, FRelationAggregate> RelationAggregates;
		for (const FCompositionEntryView& Entry : Entries)
		{
			AssetPath = Selector(Entry, TEXT("asset_path"));
			if (!AssetPath.IsEmpty())
			{
				break;
			}
		}
		if (AssetPath.IsEmpty())
		{
			AssetPath = Request.TargetKind;
		}
		const FString TargetId = StableId(
			TEXT("target"),
			Request.TargetKind + TEXT("|") + AssetPath);
		const FString Provenance =
			Normalize(Request.SourceProvenance).IsEmpty()
				? TEXT("authoring_state.capture")
				: Normalize(Request.SourceProvenance);

		TArray<FUnitDraft> Drafts;
		TMap<FString, int32> DraftIndex;
		auto AddDraft =
			[&Drafts, &DraftIndex](FUnitDraft Draft) -> FUnitDraft&
			{
				const int32 Index = Drafts.Add(MoveTemp(Draft));
				DraftIndex.Add(Drafts[Index].UnitId, Index);
				return Drafts[Index];
			};
		FUnitDraft Target;
		Target.UnitId = TargetId;
		Target.UnitKind = ESemanticUnitKind::TargetState;
		Target.SemanticTypeRef = Request.TargetKind;
		Target.Role = TEXT("authoring_target");
		Target.SourceProvenance = Provenance;
		AddDraft(MoveTemp(Target));

		TMap<FString, FString> SurfaceUnits;
		for (const FCompositionSurfaceContract& SurfaceContract
			: CompositionContract.Surfaces)
		{
			FUnitDraft Root;
			Root.UnitId = StableId(
				TEXT("surface"),
				Request.TargetKind + TEXT("|")
					+ SurfaceContract.SurfaceId);
			Root.UnitKind = SurfaceContract.UnitKind;
			Root.OwnerRef = TargetId;
			Root.SemanticTypeRef =
				TEXT("uai.authoring.surface.")
				+ SurfaceContract.SurfaceId;
			Root.Role = SurfaceContract.SurfaceId;
			Root.FieldSetRef =
				SurfaceContract.SurfaceKind
						== EAuthoringSurfaceKind::Properties
					? SurfaceContract.SurfaceId
					: FString();
			Root.SourceProvenance = Provenance;
			SurfaceUnits.Add(
				SurfaceContract.SurfaceId,
				Root.UnitId);
			AddDraft(MoveTemp(Root));
		}
		// Preview entries choose their surface explicitly in RouteEntry. Keep
		// those physical views out of the asset document's fallback routing:
		// otherwise an unmatched collection can become preview properties.
		for (const FString& Surface : PreviewSurfaces)
		{
			SurfaceUnits.Add(Surface, TargetId);
		}

		TMap<FString, FString> GraphUnits;
		TMap<FString, FString> ItemUnits;
		TMap<FString, FString> SourceItemUnits;
		TMap<FString, FString> DefinitionUnits;
		TMap<FString, TArray<FString>> ExplicitOrder;
		TMap<FString, const FCompositionEntryView*>
			EntryByUnit;

		auto BindingForOwner =
			[&Drafts, &DraftIndex](
				const FString& OwnerId,
				const FCompositionEntryView& Entry)
				-> FString
			{
				FString CurrentId = OwnerId;
				TSet<FString> Seen;
				while (!CurrentId.IsEmpty()
					&& !Seen.Contains(CurrentId))
				{
					Seen.Add(CurrentId);
					const int32* Index = DraftIndex.Find(CurrentId);
					if (!Index)
					{
						break;
					}
					const FUnitDraft& Owner = Drafts[*Index];
					if (!Owner.TargetIdentityKey.IsEmpty())
					{
						return FString();
					}
					CurrentId = Owner.OwnerRef;
				}
				return Entry.TargetIdentity.CanonicalKey;
			};

		TFunction<bool(const TSharedPtr<FJsonValue>&, TSharedPtr<FJsonValue>&, TArray<FString>&)> InternValueDefinition;
		auto DescribeObjectBranches = [](const FKeyedItemProjectionContract& Contract) {
			const auto ObjectBranches = MakeShared<FJsonObject>();
			for (const auto& ObjectPolicy : Contract.ObjectFieldPolicies)
			{
				if (ObjectPolicy.Value.DiscriminatorField.IsEmpty()) continue;
				const auto BranchContract = MakeShared<FJsonObject>();
				BranchContract->SetStringField(TEXT("discriminator"), ObjectPolicy.Value.DiscriminatorField);
				const auto Variants = MakeShared<FJsonObject>();
				const auto RequiredVariants = MakeShared<FJsonObject>();
				for (const auto& Branch : ObjectPolicy.Value.BranchFields)
				{
					TArray<FString> Names = Branch.Value.Array();
					Names.Sort();
					TArray<TSharedPtr<FJsonValue>> Members;
					for (const FString& Name : Names) Members.Add(MakeShared<FJsonValueString>(Name));
					Variants->SetArrayField(Branch.Key, Members);
					TArray<TSharedPtr<FJsonValue>> RequiredMembers;
					for (const auto& Name : Names)
						if (!ObjectPolicy.Value.ExclusiveBranchFields.ContainsByPredicate([&](const auto& Group) { return Group.Contains(Name); })) RequiredMembers.Add(MakeShared<FJsonValueString>(Name));
					RequiredVariants->SetArrayField(Branch.Key, RequiredMembers);
				}
				BranchContract->SetObjectField(ObjectPolicy.Value.ExclusiveBranchFields.IsEmpty() ? TEXT("required_and_allowed_members") : TEXT("allowed_members"), Variants);
				if (!ObjectPolicy.Value.ExclusiveBranchFields.IsEmpty())
				{
					BranchContract->SetObjectField(TEXT("required_members"), RequiredVariants);
					TArray<TSharedPtr<FJsonValue>> Groups;
					for (const auto& Group : ObjectPolicy.Value.ExclusiveBranchFields)
					{
						TArray<FString> Names = Group.Array(); Names.Sort(); TArray<TSharedPtr<FJsonValue>> Members;
						for (const auto& Name : Names) Members.Add(MakeShared<FJsonValueString>(Name));
						Groups.Add(MakeShared<FJsonValueArray>(Members));
					}
					BranchContract->SetArrayField(TEXT("exactly_one_when_allowed"), Groups);
				}
				ObjectBranches->SetObjectField(ObjectPolicy.Key, BranchContract);
			}
			return ObjectBranches;
		};
		TFunction<bool(const FCompositionEntryView&, FString&)> EnsurePropertyDefinition;
		InternValueDefinition = [&](const TSharedPtr<FJsonValue>& Input,
			TSharedPtr<FJsonValue>& Output, TArray<FString>& Dependencies) -> bool
		{
			if (!Input.IsValid() || Input->Type != EJson::Object)
			{
				OutError.SetSourceFailure(TEXT("composition_value_definition_invalid"), TEXT("A value definition must be an object.")); return false;
			}
			const auto Object = Input->AsObject();
			const TSharedPtr<FJsonObject>* NativeType = nullptr;
			if (Object->TryGetObjectField(TEXT("type"), NativeType))
			{
				FCompositionEntryView FieldDescriptor;
				FieldDescriptor.Type = *NativeType;
				FieldDescriptor.bEditable = true;
				// Re-interning a complete nested definition must preserve its contract.
				// Codec-only descriptors may omit these fields; their defaults stay unchanged.
				if (Object->HasField(TEXT("editable"))
					&& !Object->TryGetBoolField(TEXT("editable"), FieldDescriptor.bEditable))
				{ OutError.SetSourceFailure(TEXT("composition_value_definition_invalid"), TEXT("Value-definition editable must be a boolean."), FString(), TEXT("/editable")); return false; }
				const TSharedPtr<FJsonObject>* Constraints = nullptr;
				if (Object->HasField(TEXT("constraints")))
				{
					if (!Object->TryGetObjectField(TEXT("constraints"), Constraints))
					{ OutError.SetSourceFailure(TEXT("composition_value_definition_invalid"), TEXT("Value-definition constraints must be an object."), FString(), TEXT("/constraints")); return false; }
					for (const auto& Constraint : (*Constraints)->Values)
					{
						FString Value;
						if (!Constraint.Value || !Constraint.Value->TryGetString(Value))
						{ OutError.SetSourceFailure(TEXT("composition_value_definition_invalid"), FString::Printf(TEXT("Constraint '%s' must be a string."), *Constraint.Key), FString(), TEXT("/constraints/") + EscapeJsonPointerSegment(Constraint.Key)); return false; }
						FieldDescriptor.Constraints.Add(Constraint.Key, MoveTemp(Value));
					}
				}
				FieldDescriptor.ValueDefinition = Object->TryGetField(TEXT("value_definition"));
				FieldDescriptor.CandidatesHeader = Object->TryGetField(TEXT("candidates_header"));
				FieldDescriptor.Candidates = Object->TryGetField(TEXT("candidates"));
				FString DefinitionId;
				if (!EnsurePropertyDefinition(FieldDescriptor, DefinitionId)) return false;
				Dependencies.AddUnique(DefinitionId);
				Output = MakeShared<FJsonValueString>(DefinitionId);
				return true;
			}
			FString Builtin;
			if (Object->Values.Num() == 1 && Object->HasField(TEXT("hash_source")))
			{
				Output = Input;
				return true;
			}
			if (Object->Values.Num() == 1 && Object->TryGetStringField(TEXT("builtin"), Builtin)
				&& (Builtin == TEXT("text") || Builtin == TEXT("owned_object") || Builtin == TEXT("property_bag")))
			{
				Output = Input;
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (Object->TryGetArrayField(TEXT("items"), Items))
			{
				TArray<TSharedPtr<FJsonValue>> ProjectedItems;
				for (const auto& Item : *Items)
				{
					if (Item.IsValid() && Item->Type == EJson::Null) { ProjectedItems.Add(Item); continue; }
					TSharedPtr<FJsonValue> Projected;
					if (!InternValueDefinition(Item, Projected, Dependencies)) return false;
					ProjectedItems.Add(Projected);
				}
				const auto Result = MakeShared<FJsonObject>();
				// Intern only structural child definitions. Preserve the remaining
				// contract metadata rather than maintaining another partial key list.
				Result->Values = Object->Values;
				Result->SetArrayField(TEXT("items"), ProjectedItems);
				if (const auto Element = Object->TryGetField(TEXT("element_definition")))
				{
					TSharedPtr<FJsonValue> Projected;
					if (!InternValueDefinition(Element, Projected, Dependencies)) return false;
					Result->SetField(TEXT("element_definition"), Projected);
				}
				Output = MakeShared<FJsonValueObject>(Result);
				return true;
			}
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			if (Object->TryGetObjectField(TEXT("fields"), Fields))
			{
				const auto ProjectedFields = MakeShared<FJsonObject>();
				for (const auto& Field : (*Fields)->Values)
				{
					TSharedPtr<FJsonValue> Projected;
					if (!InternValueDefinition(Field.Value, Projected, Dependencies)) return false;
					ProjectedFields->SetField(Field.Key, Projected);
				}
				const auto Result = MakeShared<FJsonObject>();
				Result->Values = Object->Values;
				Result->SetObjectField(TEXT("fields"), ProjectedFields);
				Output = MakeShared<FJsonValueObject>(Result);
				return true;
			}
			FString TypeName;
			const TSharedPtr<FJsonObject>* Contract = nullptr;
			if (!Object->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty()
				|| !Object->TryGetObjectField(TEXT("contract"), Contract))
			{
				OutError.SetSourceFailure(TEXT("composition_value_definition_invalid"), TypeName.IsEmpty()
					? TEXT("Value definition requires a nonempty string type.") : TEXT("Value definition requires an object contract."), FString(), TypeName.IsEmpty() ? TEXT("/type") : TEXT("/contract")); return false;
			}
			const FString Signature = DefinitionIdentity(Object.ToSharedRef());
			const FString Key = TEXT("value-definition:") + Signature;
			FString DefinitionId;
			if (const FString* Existing = DefinitionUnits.Find(Key)) DefinitionId = *Existing;
			else
			{
				DefinitionId = StableId(TEXT("property_definition"), Key);
				FUnitDraft Definition;
				Definition.UnitId = DefinitionId;
				Definition.UnitKind = ESemanticUnitKind::CollectionState;
				Definition.OwnerRef = TargetId;
				Definition.SemanticTypeRef = TEXT("uai.authoring.property_definition/1");
				Definition.Role = TEXT("property_definition");
				Definition.FieldSetRef = DefinitionId;
				Definition.SourceProvenance = Provenance;
				const auto Type = MakeShared<FJsonObject>();
				Type->SetStringField(TEXT("type_id"), TypeName);
				for (const auto& Field : TArray<TPair<FString, TSharedPtr<FJsonValue>>>{
					{TEXT("type"), MakeShared<FJsonValueObject>(Type)},
					{TEXT("name"), MakeShared<FJsonValueString>(TypeName)},
					{TEXT("value_definition"), MakeShared<FJsonValueObject>(*Contract)}})
				{
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalizeJson(Field.Value, Resources, Value, OutError)) return false;
					Definition.Values.Add(Field.Key, Value.ToSharedRef());
				}
				DefinitionUnits.Add(Key, DefinitionId);
				AddDraft(MoveTemp(Definition));
			}
			Dependencies.AddUnique(DefinitionId);
			Output = MakeShared<FJsonValueString>(DefinitionId);
			return true;
		};

		EnsurePropertyDefinition =
			[&](const FCompositionEntryView& Entry, FString& OutDefinitionId) -> bool
		{
			OutDefinitionId.Reset();
			if (!Entry.Type.IsValid()) return true;
			TSharedPtr<FJsonValue> ProjectedValueDefinition;
			TArray<FString> ValueDefinitionDependencies;
			if (Entry.ValueDefinition.IsValid()
				&& !InternValueDefinition(Entry.ValueDefinition, ProjectedValueDefinition, ValueDefinitionDependencies)) return false;
			const auto Identity = MakeShared<FJsonObject>();
			Identity->SetObjectField(TEXT("type"), Entry.Type);
			Identity->SetBoolField(TEXT("editable"), Entry.bEditable);
			const auto IdentityConstraints = MakeShared<FJsonObject>();
			TArray<FString> Keys;
			Entry.Constraints.GetKeys(Keys); Keys.Sort();
			for (const FString& Key : Keys)
			{
				FString Value = Entry.Constraints.FindChecked(Key);
				// Only the native numeric bound contracts have numeric equivalence.
				// Scope, expressions, class paths and arbitrary metadata retain their exact identity.
				if (Key == TEXT("clamp_min") || Key == TEXT("clamp_max") || Key == TEXT("ui_min") || Key == TEXT("ui_max"))
				{
					FString Normalized;
					if (TryNormalizeNumberLexeme(Value, Normalized)) Value = MoveTemp(Normalized);
				}
				IdentityConstraints->SetStringField(Key, Value);
			}
			Identity->SetObjectField(TEXT("constraints"), IdentityConstraints);
			Identity->SetField(TEXT("candidates_header"), OrderedDefinitionIdentity(Entry.CandidatesHeader));
			Identity->SetField(TEXT("candidates"), OrderedDefinitionIdentity(Entry.Candidates));
			// Child definitions have already been interned by their complete normalized
			// contracts, so equivalent nested bounds cannot split the parent definition.
			Identity->SetField(TEXT("value_definition"), OrderedDefinitionIdentity(ProjectedValueDefinition));
			const FString Signature = DefinitionIdentity(Identity);
			const FString DefinitionKey = TEXT("definition:") + Signature;
			if (const FString* Existing = DefinitionUnits.Find(DefinitionKey))
			{
				OutDefinitionId = *Existing; return true;
			}
			OutDefinitionId = StableId(TEXT("property_definition"), Signature);
			FUnitDraft Definition;
			Definition.UnitId = OutDefinitionId;
			Definition.UnitKind = ESemanticUnitKind::CollectionState;
			Definition.OwnerRef = TargetId;
			Definition.SemanticTypeRef = TEXT("uai.authoring.property_definition/1");
			Definition.Role = TEXT("property_definition");
			Definition.FieldSetRef = OutDefinitionId;
			Definition.SourceProvenance = Provenance;
			if (ProjectedValueDefinition.IsValid())
			{
				FUeAgentCanonicalValuePtr Value;
				if (!TryCanonicalizeJson(ProjectedValueDefinition, Resources, Value, OutError)) return false;
				Definition.Values.Add(TEXT("value_definition"), Value.ToSharedRef());
				Definition.DependencyRefs = MoveTemp(ValueDefinitionDependencies);
			}
			FUeAgentCanonicalValuePtr TypeValue;
			FUeAgentStateSemanticError SemanticError;
			if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Entry.Type.ToSharedRef()), Resources, TypeValue, OutError)) return false;
			Definition.Values.Add(TEXT("type"), TypeValue.ToSharedRef());
			FUeAgentCanonicalValuePtr EditableValue;
			if (!FUeAgentCanonicalValue::TryBoolean(Entry.bEditable, Resources, EditableValue, SemanticError))
			{
				OutError.SetFailure(SemanticError, FString()); return false;
			}
			Definition.Values.Add(TEXT("editable"), EditableValue.ToSharedRef());
			if (!Entry.Constraints.IsEmpty())
			{
				TSharedRef<FJsonObject> ConstraintObject = MakeShared<FJsonObject>();
				for (const FString& Key : Keys)
				{
					ConstraintObject->SetStringField(Key, Entry.Constraints.FindChecked(Key));
				}
				FUeAgentCanonicalValuePtr ConstraintValue;
				if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(ConstraintObject), Resources, ConstraintValue, OutError)) return false;
				Definition.Values.Add(TEXT("constraints"), ConstraintValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr CandidateHeaderValue;
			if (Entry.CandidatesHeader.IsValid())
			{
				if (!TryCanonicalizeJson(Entry.CandidatesHeader, Resources, CandidateHeaderValue, OutError)) return false;
				Definition.Values.Add(TEXT("candidates_header"), CandidateHeaderValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr CandidateValue;
			if (Entry.Candidates.IsValid())
			{
				if (!TryCanonicalizeJson(Entry.Candidates, Resources, CandidateValue, OutError)) return false;
				Definition.Values.Add(TEXT("candidates"), CandidateValue.ToSharedRef());
			}
			DefinitionUnits.Add(DefinitionKey, OutDefinitionId);
			AddDraft(MoveTemp(Definition));
			return true;
		};

		auto BindDocumentValue = [&](const FString& UnitId, const FCompositionEntryView& Entry,
			const FDocumentAddress& Address, bool bWholeValue)
		{
			if (!DocumentBindings) return true;
			if (DocumentBindings->Contains(UnitId))
			{
				OutError.SetSourceFailure(TEXT("composition_document_address_ambiguous"), TEXT("A composed unit is bound to more than one document address."), UnitId, DocumentAddressPointer(Address));
				return false;
			}
			DocumentBindings->Add(UnitId, {
				DocumentSourceKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey), Address, bWholeValue});
			return true;
		};

		const auto GetDocumentCollectionContract = [](
			const FString& TargetKind,
			const FCompositionEntryView& Entry,
			const FDocumentVisit& Visit) -> const FKeyedItemProjectionContract*
		{
			if (TargetKind == TEXT("ue.control_rig.asset")
				&& Entry.SemanticId == TEXT("uai.semantic.rigging.control_rig.document"))
			{
				const FString RootField = Visit.Path == TEXT("hierarchy/elements")
					? TEXT("rig_elements")
					: Visit.Path == TEXT("graphs/graphs")
						? TEXT("rig_graphs") : FString();
				for (const auto& Contract : GetKeyedItemProjectionContracts())
					if (Contract.RootField == RootField) return &Contract;
			}
			if (TargetKind == TEXT("ue.ik_rig.asset")
				&& Entry.SemanticId == TEXT("uai.semantic.rigging.ik_rig.document")
				&& (Visit.Path == TEXT("goals") || Visit.Path == TEXT("solvers") || Visit.Path == TEXT("chains")))
			{
				for (const auto& Contract : GetKeyedItemProjectionContracts())
					if (Contract.RootField == TEXT("ik_") + Visit.Path) return &Contract;
			}
			if (TargetKind == TEXT("ue.ik_retargeter.asset")
				&& Entry.SemanticId == TEXT("uai.semantic.rigging.ik_retargeter.document")
				&& (Visit.Path == TEXT("ops") || Visit.Path == TEXT("mappings") || Visit.Path == TEXT("poses")))
			{
				for (const auto& Contract : GetKeyedItemProjectionContracts())
					if (Contract.RootField == TEXT("ik_retarget_") + Visit.Path) return &Contract;
			}
			return FindLevelSequenceCollectionContract(TargetKind, Entry.SemanticId, Visit.SurfaceId, Visit.Path);
		};
		auto AddDocumentProperty =
			[&](
				const FCompositionEntryView& Entry,
				const FString& OwnerId,
				const FString& SurfaceId,
				const FString& Path,
				const FUeAgentCanonicalValuePtr& Value,
				const FDocumentAddress& Address) -> bool
		{
			FString DefinitionId;
			if (!EnsurePropertyDefinition(Entry, DefinitionId)) return false;
			FUnitDraft Property;
				Property.UnitId = StableId(
					TEXT("property"),
					Request.TargetKind + TEXT("|")
						+ Entry.SemanticId + TEXT("|")
						+ Entry.TargetIdentity.CanonicalKey + TEXT("|")
						+ Path);
				Property.UnitKind = ESemanticUnitKind::PropertySet;
				Property.OwnerRef = OwnerId;
				Property.SemanticTypeRef =
					Entry.SemanticId + TEXT("#") + Path;
				Property.Role = SurfaceId;
				Property.FieldSetRef = Path;
			Property.TargetIdentityKey =
					BindingForOwner(OwnerId, Entry);
			Property.SourceProvenance =
					Entry.Provenance.IsEmpty()
						? Provenance
						: Entry.Provenance;
			if (!DefinitionId.IsEmpty()) Property.DependencyRefs.Add(DefinitionId);
			if (!Value.IsValid())
				{
					OutError.SetSourceFailure(TEXT("composition_document_value_invalid"), TEXT("The document property has no captured value."), Property.UnitId, DocumentAddressPointer(Address));
					return false;
				}
				Property.Values.Add(TEXT("value"), Value.ToSharedRef());
				if (!BindDocumentValue(Property.UnitId, Entry, Address, true)) return false;
				AddDraft(MoveTemp(Property));
				return true;
			};

		auto DecomposeDocument =
			[&](const FCompositionEntryView& Entry) -> bool
			{
				if (!Entry.Value.IsValid())
				{
					return true;
				}
				TArray<FDocumentVisit> Visits;
				if (Entry.Value->GetKind() == EUeAgentValueKind::Object)
				{
					const auto& Fields = Entry.Value->GetObjectItems();
					for (int32 Index = Fields.Num() - 1;
						Index >= 0;
						--Index)
					{
						const FString& Key = Fields[Index].Key;
						const FUeAgentCanonicalValuePtr Value(
							Fields[Index].Value);
						const bool bStructured =
							IsStructuredValue(Fields[Index].Value);
						const auto* ValueFields = CompositionContract.DocumentValueFields.Find(Entry.SemanticId);
						const bool bNamedValue = ValueFields && ValueFields->Contains(Key);
						const FString SurfaceId =
							ChooseDocumentSurface(
								CompositionContract,
								Key,
								bStructured && !bNamedValue);
						const FString* SurfaceOwner =
							SurfaceUnits.Find(SurfaceId);
						if (!SurfaceOwner)
						{
							continue;
						}
						if (!bStructured || bNamedValue)
						{
							if (!AddDocumentProperty(
								Entry,
								*SurfaceOwner,
								SurfaceId,
								Key,
									Value, ChildAddress({}, Key)))
							{
								return false;
							}
							continue;
						}
						Visits.Add({
							Value,
							Key,
							Key,
							*SurfaceOwner,
							SurfaceId,
							INDEX_NONE, ChildAddress({}, Key)});
					}
				}
				else
				{
					const FString SurfaceId =
						ChooseDocumentSurface(
							CompositionContract,
							TEXT("document"),
							Entry.Value->GetKind()
								== EUeAgentValueKind::Array);
					const FString* SurfaceOwner =
						SurfaceUnits.Find(SurfaceId);
					if (!SurfaceOwner)
					{
						return true;
					}
					if (Entry.Value->GetKind()
						!= EUeAgentValueKind::Array)
					{
						return AddDocumentProperty(
							Entry,
							*SurfaceOwner,
							SurfaceId,
							TEXT("document"),
							Entry.Value, {});
					}
					Visits.Add({
						Entry.Value,
						TEXT("document"),
						TEXT("document"),
						*SurfaceOwner,
						SurfaceId,
						INDEX_NONE, {}});
				}

				while (!Visits.IsEmpty())
				{
					const FDocumentVisit Visit =
						Visits.Pop(EAllowShrinking::No);
					if (!Visit.Value.IsValid())
					{
						OutError.SetSourceFailure(TEXT("composition_document_value_invalid"), TEXT("The document property has no captured value."), Visit.OwnerId, DocumentAddressPointer(Visit.Address));
						return false;
					}
					const FKeyedItemProjectionContract* KeyedContract =
						GetDocumentCollectionContract(Request.TargetKind, Entry, Visit);
					// Scalar arrays are values, including at the document root. Splitting
					// them into fixed item refs loses editable membership/order (e.g. solver_order).
					if (!KeyedContract && IsScalarCollection(Visit.Value.ToSharedRef()))
					{
						if (!AddDocumentProperty(Entry, Visit.OwnerId, Visit.SurfaceId,
							Visit.Path, Visit.Value, Visit.Address)) return false;
						continue;
					}
					if (Visit.Value->GetKind() == EUeAgentValueKind::Array || KeyedContract)
					{
						if (KeyedContract)
						{
							const FKeyedItemProjectionContract& Contract = *KeyedContract;
							FUeAgentCanonicalValuePtr VisibleKeys;
							FKeyedItemReferenceContext ReferenceContext;
							if (Entry.Value.IsValid() && Entry.Value->GetKind() == EUeAgentValueKind::Object)
								for (const auto& Root : Entry.Value->GetObjectItems())
									if (const auto* Target = FindLevelSequenceCollectionContract(Request.TargetKind, Entry.SemanticId, Visit.SurfaceId, Root.Key))
										ReferenceContext.Add(Root.Key, {Target, &Root.Value.Get(), nullptr, false});
							if (!TryProjectKeyedItemVisibleValue(
									Visit.Path, *Visit.Value, Contract, Resources,
									VisibleKeys, OutError, EKeyedItemProjectionInputKind::CapturedItems, &ReferenceContext))
							{
								return false;
							}
							FUnitDraft Collection;
							Collection.UnitId = StableId(
								TEXT("keyed_collection"),
								Request.TargetKind + TEXT("|") + Entry.SemanticId
									+ TEXT("|") + Entry.TargetIdentity.CanonicalKey
									+ TEXT("|") + Visit.Path);
							Collection.UnitKind = ESemanticUnitKind::StructureState;
							Collection.OwnerRef = Visit.OwnerId;
							Collection.SemanticTypeRef = Entry.SemanticId + TEXT("#") + Visit.Path;
							Collection.Role = Visit.SurfaceId;
							Collection.TargetIdentityKey = BindingForOwner(Visit.OwnerId, Entry);
							Collection.SourceProvenance = Entry.Provenance.IsEmpty()
								? Provenance : Entry.Provenance;
							Collection.Values.Add(Contract.RootField, VisibleKeys.ToSharedRef());
							// One template/definition owner serves root tracks and tracks nested
							// under captured bindings. The binding alias is supplied by keyed projection.
							auto InternRecordProperties = [&](const FUeAgentCanonicalValue& Record, const FString& Field,
								FUnitDraft& DefinitionCollection, TSharedPtr<FJsonObject>& Out) {
								Out = MakeShared<FJsonObject>();
								const auto* Definitions = Record.GetObjectItems().FindByPredicate([&](const auto& Item) { return Item.Key == Field; });
								if (!Field.IsEmpty() && !Definitions) return true;
								UE::UAI::Contract::FDiagnostic ProjectionError;
								const auto& Entries = Field.IsEmpty() ? Record.GetObjectItems() : Definitions->Value->GetObjectItems();
								for (const auto& Definition : Entries)
								{
									TSharedPtr<FJsonValue> Json, Reference;
									if (!CanonicalJson::TryToJsonValue(Definition.Value.Get(), Json, ProjectionError))
                                { OutError.SetFailure(ProjectionError, Visit.Path); return false; }
                                if (!InternValueDefinition(Json, Reference, DefinitionCollection.DependencyRefs)) return false;
									Out->SetField(Definition.Key, Reference);
								}
								return true;
							};
							auto BuildTrackDefinitions = [&](const FKeyedItemProjectionContract& TrackContract,
								const FUeAgentCanonicalValue& TrackValue, FUnitDraft& DefinitionCollection) -> bool
							{
								FUeAgentStateSemanticError KeyError;
								FUeAgentCanonicalValuePtr Template;
								FKeyedItemProjectionContract TemplateContract;
								FUeAgentCanonicalValuePtr DefaultMap;
								TSharedPtr<FJsonValue> TemplateJson, DefinitionRef;
								UE::UAI::Contract::FDiagnostic ProjectionError;
								FUeAgentCanonicalValuePtr CanonicalDefinitionRef;
								// Each track supplies native defaults. Reuse its exact readable alias policy,
								// then intern projected templates by content (Float/Double may share one).
								FKeyedItemProjectionContract DefaultSourceContract = TrackContract;
								DefaultSourceContract.VisibleFieldPolicy = {{TEXT("default_range"), TEXT("default_channels"), TEXT("default_native_channels"), TEXT("default_properties"), TEXT("default_property_definitions"), TEXT("property_definitions"), TEXT("sections")}};
								DefaultSourceContract.CollectionFieldContracts.Reset();
								FUeAgentCanonicalValuePtr DefaultSources;
								if (!TryProjectKeyedItemVisibleValue(Visit.Path, TrackValue, DefaultSourceContract,
									Resources, DefaultSources, OutError)) return false;
								TArray<TPair<FString, FUeAgentCanonicalValueRef>> SectionDefinitions, KeyDefinitions, ChannelDefinitions;
								const auto ObjectPropertyDefinitions = MakeShared<FJsonObject>();
								TemplateContract = GetLevelSequenceScalarSectionProjectionContract();
								TemplateContract.CapturedRecordIdentityField.Reset();
								for (const auto& Source : DefaultSources->GetObjectItems())
								{
									TSharedPtr<FJsonObject> TrackPropertiesDefinition, DefaultPropertiesDefinition;
									if (!InternRecordProperties(Source.Value.Get(), TEXT("property_definitions"), DefinitionCollection, TrackPropertiesDefinition)
										|| !InternRecordProperties(Source.Value.Get(), TEXT("default_property_definitions"), DefinitionCollection, DefaultPropertiesDefinition)) return false;
									const auto OwnerDefinitions = MakeShared<FJsonObject>();
									OwnerDefinitions->SetObjectField(TEXT("track"), TrackPropertiesDefinition);
									OwnerDefinitions->SetObjectField(TEXT("section_template"), DefaultPropertiesDefinition);
									const auto* CapturedSections = Source.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("sections"); });
									if (CapturedSections)
									{
										auto DefinitionContract = GetLevelSequenceScalarSectionProjectionContract();
										DefinitionContract.VisibleFieldPolicy = {{TEXT("property_definitions")}};
										FUeAgentCanonicalValuePtr Records;
										if (!TryProjectKeyedItemVisibleValue(Visit.Path, CapturedSections->Value.Get(), DefinitionContract, Resources, Records, OutError)) return false;
										const auto Sections = MakeShared<FJsonObject>();
										for (const auto& Record : Records->GetObjectItems())
										{
											TSharedPtr<FJsonObject> Definitions;
											if (!InternRecordProperties(Record.Value.Get(), TEXT("property_definitions"), DefinitionCollection, Definitions)) return false;
											Sections->SetObjectField(Record.Key, Definitions);
										}
										OwnerDefinitions->SetObjectField(TEXT("sections"), Sections);
									}
									ObjectPropertyDefinitions->SetObjectField(Source.Key, OwnerDefinitions);
									const auto* Channels = Source.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("default_channels"); });
									const auto* Range = Source.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("default_range"); });
									const auto* ObjectProperties = Source.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("default_properties"); });
									if (!Channels || !Range) { OutError.SetSourceFailure(TEXT("composition_native_section_defaults_missing"), FString::Printf(TEXT("Track '%s' has no captured %s required to create sections."), *Source.Key, !Channels ? TEXT("default_channels") : TEXT("default_range")), Visit.Path); return false; }
									FKeyedItemProjectionContract ChannelSourceContract = GetLevelSequenceScalarChannelProjectionContract();
									ChannelSourceContract.VisibleFieldPolicy = {{TEXT("default_key"), TEXT("key_properties_definition"), TEXT("key_value_definition"), TEXT("default"), TEXT("properties"), TEXT("properties_definition"), TEXT("keys")}};
									FUeAgentCanonicalValuePtr ChannelSources;
									if (!TryProjectKeyedItemVisibleValue(Visit.Path, Channels->Value.Get(), ChannelSourceContract, Resources, ChannelSources, OutError)) return false;
									TArray<TPair<FString, FUeAgentCanonicalValueRef>> ChannelKeyDefinitions, ChannelPropertyDefinitions;
									for (const auto& ChannelSource : ChannelSources->GetObjectItems())
									{
										const auto* Properties = ChannelSource.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("default_key"); });
										const auto* PropertyDefinition = ChannelSource.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("key_properties_definition"); });
										if (!Properties) { OutError.SetSourceFailure(TEXT("composition_native_key_defaults_missing"), FString::Printf(TEXT("Channel '%s' on track '%s' has no captured default_key required to create keys."), *ChannelSource.Key, *Source.Key), Visit.Path); return false; }
										const auto& DefaultFields = Properties->Value->GetObjectItems();
										FUeAgentCanonicalValuePtr KeyDefaults, KeyTemplate;
										FKeyedItemProjectionContract KeyTemplateContract = GetLevelSequenceScalarKeyProjectionContract();
										KeyTemplateContract.CapturedRecordIdentityField.Reset();
										if (!FUeAgentCanonicalValue::TryObject(DefaultFields, Resources, KeyDefaults, KeyError)
											|| !FUeAgentCanonicalValue::TryObject({{TEXT("new_key"), KeyDefaults.ToSharedRef()}}, Resources, DefaultMap, KeyError)
											|| !TryProjectKeyedItemVisibleValue(Visit.Path, *DefaultMap, KeyTemplateContract, Resources, KeyTemplate, OutError)
											|| !CanonicalJson::TryToJsonValue(KeyTemplate->GetObjectItems()[0].Value.Get(), TemplateJson, ProjectionError)) { if (!KeyError.IsEmpty()) OutError.SetFailure(KeyError, Visit.Path); else if (!ProjectionError.IsEmpty()) OutError.SetFailure(ProjectionError, Visit.Path); return false; }
										const auto KeyContract = MakeShared<FJsonObject>(); KeyContract->SetField(TEXT("template"), TemplateJson);
										KeyContract->SetStringField(TEXT("time_unit"), TEXT("sequence_tick"));
										if (PropertyDefinition)
										{
											TSharedPtr<FJsonValue> NativeDefinition, PublicDefinition;
											if (!CanonicalJson::TryToJsonValue(PropertyDefinition->Value.Get(), NativeDefinition, ProjectionError)
												|| !InternValueDefinition(NativeDefinition, PublicDefinition, DefinitionCollection.DependencyRefs)) { if (!ProjectionError.IsEmpty()) OutError.SetFailure(ProjectionError, Visit.Path); return false; }
											KeyContract->SetField(TEXT("properties_definition"), PublicDefinition);
										}
										TSharedPtr<FJsonValue> PublicValueDefinition;
										const auto* ValueDefinition = ChannelSource.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("key_value_definition"); });
										if (ValueDefinition)
										{
											TSharedPtr<FJsonValue> NativeDefinition;
											if (!CanonicalJson::TryToJsonValue(ValueDefinition->Value.Get(), NativeDefinition, ProjectionError)
												|| !InternValueDefinition(NativeDefinition, PublicValueDefinition, DefinitionCollection.DependencyRefs)) { if (!ProjectionError.IsEmpty()) OutError.SetFailure(ProjectionError, Visit.Path); return false; }
											const bool bLexeme = TemplateJson->AsObject()->HasField(TEXT("value_lexeme"));
											KeyContract->SetField(bLexeme ? TEXT("value_lexeme_definition") : TEXT("value_definition"), PublicValueDefinition);
											if (bLexeme) KeyContract->SetBoolField(TEXT("value_lexeme_requires_selection"), TemplateJson->AsObject()->GetStringField(TEXT("value_lexeme")).IsEmpty());
										}
										const auto KeyDefinition = MakeShared<FJsonObject>(); KeyDefinition->SetStringField(TEXT("type"), TEXT("LevelSequenceScalarKey"));
										KeyDefinition->SetObjectField(TEXT("contract"), KeyContract);
										if (!InternValueDefinition(MakeShared<FJsonValueObject>(KeyDefinition), DefinitionRef, DefinitionCollection.DependencyRefs)
											|| !TryCanonicalizeJson(DefinitionRef, Resources, CanonicalDefinitionRef, OutError)) return false;
										ChannelKeyDefinitions.Emplace(ChannelSource.Key, CanonicalDefinitionRef.ToSharedRef());
										const auto ChannelContract = MakeShared<FJsonObject>();
										TSharedPtr<FJsonValue> ChannelJson;
										if (!CanonicalJson::TryToJsonValue(ChannelSource.Value.Get(), ChannelJson, ProjectionError)) { if (!ProjectionError.IsEmpty()) OutError.SetFailure(ProjectionError, Visit.Path); return false; }
										const auto ChannelTemplate = ChannelJson->AsObject();
										const auto NativeChannelDefinition = ChannelTemplate->TryGetField(TEXT("properties_definition"));
										ChannelTemplate->RemoveField(TEXT("default_key"));
										ChannelTemplate->RemoveField(TEXT("key_properties_definition"));
										ChannelTemplate->RemoveField(TEXT("properties_definition"));
										ChannelTemplate->RemoveField(TEXT("key_value_definition"));
										ChannelContract->SetObjectField(TEXT("template"), ChannelTemplate);
										if (PublicValueDefinition) {
											const bool bLexeme = TemplateJson->AsObject()->HasField(TEXT("value_lexeme"));
											ChannelContract->SetField(bLexeme ? TEXT("value_lexeme_definition") : TEXT("value_definition"), PublicValueDefinition);
											if (bLexeme) ChannelContract->SetStringField(TEXT("unnamed_value_policy"), TEXT("An empty existing name preserves its captured native value; new values require an enum selection."));
										}
										ChannelContract->SetObjectField(TEXT("object_branches"), DescribeObjectBranches(GetLevelSequenceScalarChannelProjectionContract()));
										if (NativeChannelDefinition.IsValid())
										{
											TSharedPtr<FJsonValue> PublicDefinition;
											if (!InternValueDefinition(NativeChannelDefinition, PublicDefinition, DefinitionCollection.DependencyRefs)) return false;
											ChannelContract->SetField(TEXT("properties_definition"), PublicDefinition);
										}
										const auto ChannelDefinition = MakeShared<FJsonObject>();
										ChannelDefinition->SetStringField(TEXT("type"), TEXT("LevelSequenceScalarChannel"));
										ChannelDefinition->SetObjectField(TEXT("contract"), ChannelContract);
										if (!InternValueDefinition(MakeShared<FJsonValueObject>(ChannelDefinition), DefinitionRef, DefinitionCollection.DependencyRefs)
											|| !TryCanonicalizeJson(DefinitionRef, Resources, CanonicalDefinitionRef, OutError)) return false;
										ChannelPropertyDefinitions.Emplace(ChannelSource.Key, CanonicalDefinitionRef.ToSharedRef());
									}
									FUeAgentCanonicalValuePtr ChannelDefinitionMap;
									if (!FUeAgentCanonicalValue::TryObject(ChannelKeyDefinitions, Resources, ChannelDefinitionMap, KeyError)) { if (!KeyError.IsEmpty()) OutError.SetFailure(KeyError, Visit.Path); return false; }
									KeyDefinitions.Emplace(Source.Key, ChannelDefinitionMap.ToSharedRef());
									if (!FUeAgentCanonicalValue::TryObject(ChannelPropertyDefinitions, Resources, ChannelDefinitionMap, KeyError)) { if (!KeyError.IsEmpty()) OutError.SetFailure(KeyError, Visit.Path); return false; }
									ChannelDefinitions.Emplace(Source.Key, ChannelDefinitionMap.ToSharedRef());
									FUeAgentCanonicalValuePtr NativeDefaults;
									auto NativeDefaultFields = TArray<TPair<FString, FUeAgentCanonicalValueRef>>{{TEXT("range"), Range->Value}, {TEXT("channels"), Channels->Value}};
									if (ObjectProperties) NativeDefaultFields.Emplace(TEXT("properties"), ObjectProperties->Value);
									if (const auto* NativeContext = Source.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("default_native_channels"); }))
										NativeDefaultFields.Emplace(TEXT("native_channels"), NativeContext->Value);
									if (!FUeAgentCanonicalValue::TryObject(NativeDefaultFields, Resources, NativeDefaults, KeyError)
										|| !FUeAgentCanonicalValue::TryObject({{TEXT("new_section"), NativeDefaults.ToSharedRef()}}, Resources, DefaultMap, KeyError)
										|| !TryProjectKeyedItemVisibleValue(Visit.Path, *DefaultMap, TemplateContract, Resources, Template, OutError)
										|| !CanonicalJson::TryToJsonValue(Template->GetObjectItems()[0].Value.Get(), TemplateJson, ProjectionError)) { if (!KeyError.IsEmpty()) OutError.SetFailure(KeyError, Visit.Path); else if (!ProjectionError.IsEmpty()) OutError.SetFailure(ProjectionError, Visit.Path); return false; }
									const auto SectionDefinitionContract = MakeShared<FJsonObject>();
									SectionDefinitionContract->SetField(TEXT("template"), TemplateJson);
									SectionDefinitionContract->SetStringField(TEXT("time_unit"), TEXT("sequence_tick"));
									SectionDefinitionContract->SetObjectField(TEXT("object_branches"), DescribeObjectBranches(TemplateContract));
									const auto SectionDefinition = MakeShared<FJsonObject>();
									SectionDefinition->SetStringField(TEXT("type"), TEXT("LevelSequenceScalarSection"));
									SectionDefinition->SetObjectField(TEXT("contract"), SectionDefinitionContract);
									if (!InternValueDefinition(MakeShared<FJsonValueObject>(SectionDefinition), DefinitionRef, DefinitionCollection.DependencyRefs)
										|| !TryCanonicalizeJson(DefinitionRef, Resources, CanonicalDefinitionRef, OutError)) return false;
									SectionDefinitions.Emplace(Source.Key, CanonicalDefinitionRef.ToSharedRef());
								}
								FUeAgentCanonicalValuePtr ContextDefinitions;
								if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(ObjectPropertyDefinitions), Resources, ContextDefinitions, OutError)) return false;
								DefinitionCollection.Values.Add(TEXT("object_property_definitions"), ContextDefinitions.ToSharedRef());
								if (!FUeAgentCanonicalValue::TryObject(SectionDefinitions, Resources, ContextDefinitions, KeyError))
								{ OutError.SetFailure(KeyError, Visit.Path); return false; }
								DefinitionCollection.Values.Add(TEXT("section_definitions"), ContextDefinitions.ToSharedRef());
								if (!FUeAgentCanonicalValue::TryObject(KeyDefinitions, Resources, ContextDefinitions, KeyError)) { if (!KeyError.IsEmpty()) OutError.SetFailure(KeyError, Visit.Path); return false; }
								DefinitionCollection.Values.Add(TEXT("key_definitions"), ContextDefinitions.ToSharedRef());
								if (!FUeAgentCanonicalValue::TryObject(ChannelDefinitions, Resources, ContextDefinitions, KeyError)) { if (!KeyError.IsEmpty()) OutError.SetFailure(KeyError, Visit.Path); return false; }
								DefinitionCollection.Values.Add(TEXT("channel_definitions"), ContextDefinitions.ToSharedRef());
								return true;
							};
							if (Contract.RootField == TEXT("root_tracks") || Contract.RootField == TEXT("tracks"))
							{
								if (!BuildTrackDefinitions(Contract, *Visit.Value, Collection)) return false;
							}
							else if (Contract.RootField == TEXT("bindings"))
							{
								auto BindingSourcesContract = Contract;
								BindingSourcesContract.VisibleFieldPolicy = {{TEXT("tracks"), TEXT("property_definitions"), TEXT("template_property_definitions")}};
								BindingSourcesContract.CollectionFieldContracts.Reset();
								FUeAgentCanonicalValuePtr BindingSources;
								if (!TryProjectKeyedItemVisibleValue(Visit.Path, *Visit.Value, BindingSourcesContract,
									Resources, BindingSources, OutError)) return false;
								TMap<FString, TArray<TPair<FString, FUeAgentCanonicalValueRef>>> GroupedDefinitions;
								for (const auto& SourceBinding : BindingSources->GetObjectItems())
								{
									TSharedPtr<FJsonObject> BindingProperties;
									if (!InternRecordProperties(SourceBinding.Value.Get(), TEXT("property_definitions"), Collection, BindingProperties)) return false;
									FUeAgentCanonicalValuePtr BindingPropertyDefinitions;
									if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(BindingProperties), Resources, BindingPropertyDefinitions, OutError)) return false;
									GroupedDefinitions.FindOrAdd(TEXT("binding_property_definitions")).Emplace(SourceBinding.Key, BindingPropertyDefinitions.ToSharedRef());
									const auto* Templates = SourceBinding.Value->GetObjectItems().FindByPredicate([](const auto& Field) {
										return Field.Key == TEXT("template_property_definitions"); });
									if (Templates)
									{
										const auto Definitions = MakeShared<FJsonObject>();
										for (const auto& Template : Templates->Value->GetObjectItems())
										{
											TSharedPtr<FJsonObject> TemplateProperties;
											if (!InternRecordProperties(Template.Value.Get(), FString(), Collection, TemplateProperties)) return false;
											Definitions->SetObjectField(Template.Key, TemplateProperties);
										}
										FUeAgentCanonicalValuePtr Value;
										if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Definitions), Resources, Value, OutError)) return false;
										GroupedDefinitions.FindOrAdd(TEXT("template_property_definitions")).Emplace(SourceBinding.Key, Value.ToSharedRef());
									}
									const auto* Tracks = SourceBinding.Value->GetObjectItems().FindByPredicate([](const auto& Field) {
										return Field.Key == TEXT("tracks"); });
									if (!Tracks) { OutError.SetSourceFailure(TEXT("composition_binding_tracks_missing"), FString::Printf(TEXT("Binding '%s' has no captured tracks collection."), *SourceBinding.Key), Visit.Path); return false; }
									FUnitDraft TrackDefinitions;
									if (!BuildTrackDefinitions(GetLevelSequenceBindingScalarTrackProjectionContract(), Tracks->Value.Get(),
										TrackDefinitions)) return false;
									for (const auto& Dependency : TrackDefinitions.DependencyRefs) Collection.DependencyRefs.AddUnique(Dependency);
									for (const auto& Definition : TrackDefinitions.Values)
										GroupedDefinitions.FindOrAdd(Definition.Key).Emplace(SourceBinding.Key, Definition.Value);
								}
								FUeAgentStateSemanticError DefinitionError;
								for (const auto& Group : GroupedDefinitions)
								{
									FUeAgentCanonicalValuePtr Value;
									if (!FUeAgentCanonicalValue::TryObject(Group.Value, Resources, Value, DefinitionError))
								{ OutError.SetFailure(DefinitionError, Collection.UnitId); return false; }
									Collection.Values.Add(Group.Key, Value.ToSharedRef());
								}
							}
							const FString CollectionId = Collection.UnitId;
							if (!BindDocumentValue(CollectionId, Entry, Visit.Address, true)) return false;
							if (DocumentBindings)
							{
								auto& Binding = DocumentBindings->FindChecked(CollectionId);
							Binding.KeyedContract = &Contract;
							Binding.CapturedItems = Visit.Value;
							}
							AddDraft(MoveTemp(Collection));
							ExplicitOrder.FindOrAdd(Visit.OwnerId).Add(CollectionId);
							continue;
						}
						const FString CollectionId = StableId(
							TEXT("collection"),
							Request.TargetKind + TEXT("|")
								+ Entry.SemanticId + TEXT("|")
								+ Entry.TargetIdentity.CanonicalKey + TEXT("|")
								+ Visit.Path);
						FUnitDraft Collection;
						Collection.UnitId = CollectionId;
						Collection.UnitKind =
							ESemanticUnitKind::CollectionState;
						Collection.OwnerRef = Visit.OwnerId;
						Collection.SemanticTypeRef =
							Entry.SemanticId + TEXT("#")
								+ Visit.Path;
						Collection.Role = Visit.SurfaceId;
						Collection.TargetIdentityKey =
							BindingForOwner(Visit.OwnerId, Entry);
						Collection.SourceProvenance =
							Entry.Provenance.IsEmpty()
								? Provenance
								: Entry.Provenance;
						const FString CollectionProvenance =
							Collection.SourceProvenance;
						AddDraft(MoveTemp(Collection));
						ExplicitOrder.FindOrAdd(
							Visit.OwnerId).Add(CollectionId);
						const TArray<FUeAgentCanonicalValueRef>& Items =
							Visit.Value->GetArrayItems();
						for (int32 Index = Items.Num() - 1;
							Index >= 0;
							--Index)
						{
							const FString ItemPath =
								FString::Printf(
									TEXT("%s/%08d"),
									*Visit.Path,
									Index);
							if (IsStructuredValue(Items[Index]))
							{
								Visits.Add({
									FUeAgentCanonicalValuePtr(Items[Index]),
									ItemPath,
									Visit.RoleHint,
									CollectionId,
									Visit.SurfaceId,
									Index, ChildAddress(Visit.Address, FString(), Index)});
							}
							else
							{
								FUnitDraft Item;
								Item.UnitId = StableId(
									TEXT("item"),
									Request.TargetKind + TEXT("|")
										+ Entry.SemanticId + TEXT("|")
										+ Entry.TargetIdentity.CanonicalKey + TEXT("|")
										+ ItemPath);
								Item.UnitKind =
									ESemanticUnitKind::ItemState;
								Item.OwnerRef = CollectionId;
								Item.SemanticTypeRef =
									Entry.SemanticId + TEXT("#")
										+ ItemPath;
								Item.Role = Visit.SurfaceId;
								Item.TargetIdentityKey =
									BindingForOwner(CollectionId, Entry);
								Item.SourceProvenance =
									CollectionProvenance;
								if (!TryBuildValues(
									FUeAgentCanonicalValuePtr(Items[Index]),
									{},
									Resources,
									Item.Values,
									OutError))
								{
									return false;
								}
								const FString ItemId = Item.UnitId;
								if (!BindDocumentValue(ItemId, Entry,
									ChildAddress(Visit.Address, FString(), Index), true)) return false;
								AddDraft(MoveTemp(Item));
								ExplicitOrder.FindOrAdd(
									CollectionId).Insert(ItemId, 0);
							}
						}
						continue;
					}

					if (Visit.Value->GetKind()
						!= EUeAgentValueKind::Object)
					{
						if (!AddDocumentProperty(
							Entry,
							Visit.OwnerId,
							Visit.SurfaceId,
							Visit.Path,
								Visit.Value, Visit.Address))
						{
							return false;
						}
						continue;
					}
					const bool bRelation =
						IsRelationDocumentObject(
							Visit.RoleHint,
							Visit.Value);
					FUnitDraft Unit;
					Unit.UnitId = StableId(
						bRelation ? TEXT("relation") : TEXT("item"),
						Request.TargetKind + TEXT("|")
							+ Entry.SemanticId + TEXT("|")
							+ Entry.TargetIdentity.CanonicalKey + TEXT("|")
							+ Visit.Path);
					Unit.UnitKind = bRelation
						? ESemanticUnitKind::RelationSet
						: ESemanticUnitKind::ItemState;
					Unit.OwnerRef = Visit.OwnerId;
					Unit.SemanticTypeRef =
						Entry.SemanticId + TEXT("#") + Visit.Path;
					Unit.Role = Visit.SurfaceId;
					Unit.TargetIdentityKey =
						BindingForOwner(Visit.OwnerId, Entry);
					Unit.SourceProvenance =
						Entry.Provenance.IsEmpty()
							? Provenance
							: Entry.Provenance;
					FUeAgentCanonicalValuePtr LayoutValue;
					FString LayoutFieldKey;
					const auto& Fields = Visit.Value->GetObjectItems();
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
						: Fields)
					{
						if (Field.Key == TEXT("pos")
							|| Field.Key == TEXT("position")
							|| Field.Key == TEXT("editor_position"))
						{
							LayoutValue =
								FUeAgentCanonicalValuePtr(Field.Value);
							LayoutFieldKey = Field.Key;
							continue;
						}
						const FDocumentVisit ChildVisit{
							FUeAgentCanonicalValuePtr(Field.Value), Visit.Path + TEXT("/") + Field.Key,
							Field.Key, Unit.UnitId, Visit.SurfaceId, INDEX_NONE,
							ChildAddress(Visit.Address, Field.Key)};
						const bool bDocumentKeyedCollection =
							GetDocumentCollectionContract(Request.TargetKind, Entry, ChildVisit) != nullptr;
						if (!IsStructuredValue(Field.Value)
							|| (IsScalarCollection(Field.Value) && !bDocumentKeyedCollection))
						{
							Unit.Values.Add(Field.Key, Field.Value);
						}
					}
					const FString UnitId = Unit.UnitId;
					if (!BindDocumentValue(UnitId, Entry, Visit.Address, false)) return false;
					AddDraft(MoveTemp(Unit));
					if (!bRelation)
					{
						ExplicitOrder.FindOrAdd(
							Visit.OwnerId).Add(UnitId);
					}
					if (LayoutValue.IsValid())
					{
						FUnitDraft Layout;
						Layout.UnitId = StableId(
							TEXT("layout"),
							UnitId);
						Layout.UnitKind =
							ESemanticUnitKind::LayoutState;
						Layout.OwnerRef = UnitId;
						Layout.SemanticTypeRef =
							TEXT("uai.authoring.layout/1");
						Layout.Role = TEXT("editor_layout");
						Layout.SourceProvenance =
							Entry.Provenance.IsEmpty()
								? Provenance
								: Entry.Provenance;
						if (!TryBuildValues(
							LayoutValue,
							{},
							Resources,
							Layout.Values,
							OutError))
						{
							return false;
						}
						const FString LayoutId = Layout.UnitId;
						if (!BindDocumentValue(
							LayoutId, Entry,
							ChildAddress(Visit.Address, LayoutFieldKey), false))
						{
							return false;
						}
						AddDraft(MoveTemp(Layout));
					}
					for (int32 Index = Fields.Num() - 1;
						Index >= 0;
						--Index)
					{
						const FString& Key = Fields[Index].Key;
						if (Key == TEXT("pos")
							|| Key == TEXT("position")
							|| Key == TEXT("editor_position"))
						{
							continue;
						}
						const FUeAgentCanonicalValueRef& Child =
							Fields[Index].Value;
						if (!IsStructuredValue(Child))
						{
							continue;
						}
						const FDocumentVisit ChildVisit{
							FUeAgentCanonicalValuePtr(Child), Visit.Path + TEXT("/") + Key,
							Key, UnitId, Visit.SurfaceId, INDEX_NONE,
							ChildAddress(Visit.Address, Key)};
						if (IsScalarCollection(Child)
							&& GetDocumentCollectionContract(Request.TargetKind, Entry, ChildVisit) == nullptr)
						{
							continue;
						}
						Visits.Add(ChildVisit);
					}
				}
				return true;
			};

		for (const FCompositionEntryView& Entry : Entries)
		{
			if (!Entry.bAuthoringVisible)
			{
				continue;
			}
			const FEntryRoute Route =
				RouteEntry(Request.TargetKind, Entry.SemanticId, Selector(Entry, TEXT("preview_target")));
			if (Entry.bAvailable
				&& Route.bDocument
				&& !DecomposeDocument(Entry))
			{
				return false;
			}
		}

		for (const FCompositionEntryView& Entry : Entries)
		{
			if (!Entry.bAvailable || !Entry.Value.IsValid() || !Entry.bAuthoringVisible)
			{
				continue;
			}
			const FEntryRoute Route =
				RouteEntry(Request.TargetKind, Entry.SemanticId, Selector(Entry, TEXT("preview_target")));
			if (Route.bDocument)
			{
				continue;
			}
			if (Route.bTargetValues)
			{
				FUnitDraft& TargetDraft =
					Drafts[DraftIndex.FindChecked(TargetId)];
				if (TargetDraft.TargetIdentityKey.IsEmpty())
				{
					TargetDraft.TargetIdentityKey =
						Entry.TargetIdentity.CanonicalKey;
				}
				else if (TargetDraft.TargetIdentityKey
					!= Entry.TargetIdentity.CanonicalKey)
				{
					OutError.SetSourceFailure(TEXT("composition_target_identity_conflict"), TEXT("Target-level entries resolve to different target identities."),
						Entry.SemanticId);
					return false;
				}
				if (!TryBuildValues(
					Entry.Value,
					{},
					Resources,
					TargetDraft.Values,
					OutError))
				{
					return false;
				}
				continue;
			}
			if (!Route.bGraph && !Route.bItem)
			{
				continue;
			}
			const FString* SurfaceUnit =
				SurfaceUnits.Find(Route.SurfaceId);
			if (!SurfaceUnit)
			{
				continue;
			}
			FUnitDraft Draft;
			Draft.UnitId = StableId(
				Route.bGraph ? TEXT("structure") : TEXT("item"),
				Request.TargetKind + TEXT("|")
					+ Entry.SemanticId + TEXT("|")
					+ Entry.TargetIdentity.CanonicalKey);
			Draft.UnitKind = Route.UnitKind;
			Draft.OwnerRef = Route.bGraph
				? *SurfaceUnit
				: FindGraphOwner(
					Entry,
					GraphUnits,
					*SurfaceUnit);
			Draft.SemanticTypeRef = Entry.SemanticId;
			Draft.TargetIdentityKey =
				Entry.TargetIdentity.CanonicalKey;
			Draft.Role = Route.SurfaceId;
			Draft.SourceProvenance =
				Entry.Provenance.IsEmpty()
					? Provenance
					: Entry.Provenance;
			Draft.CategoryPath = Entry.CategoryPath;
			// An item's captured field schema belongs to the shared definition
			// catalog, not to its editable business values or identity.
			if (Route.bItem && Entry.ValueDefinition.IsValid())
			{
				if (!EnsurePropertyDefinition(Entry, Draft.FieldSetRef)) return false;
				if (!Draft.FieldSetRef.IsEmpty()) Draft.DependencyRefs.Add(Draft.FieldSetRef);
			}
			if (!TryBuildValues(
				Entry.Value,
				{
					TEXT("pos"),
					TEXT("position"),
					TEXT("editor_position")},
				Resources,
				Draft.Values,
				OutError))
			{
				return false;
			}
			const FString DraftId = Draft.UnitId;
			AddDraft(MoveTemp(Draft));
			EntryByUnit.Add(DraftId, &Entry);
			if (Route.bGraph)
			{
				GraphUnits.Add(GraphSelectorKey(Entry), DraftId);
			}
			else
			{
				// Pins are owned leaves. Their node selector intentionally matches
				// the parent node, so indexing a Pin as an owner would replace that
				// node and can assign sibling Pins (or the Pin itself) as owners.
				if (!Entry.SemanticId.EndsWith(TEXT(".pin")))
				{
					ItemUnits.Add(ItemSelectorKey(Entry), DraftId);
				}
				const FString SourceItemId =
					Selector(Entry, TEXT("item_id"));
				if (!SourceItemId.IsEmpty())
				{
					SourceItemUnits.Add(
						Selector(Entry, TEXT("asset_path"))
							+ TEXT("|item_id=") + SourceItemId,
						DraftId);
				}
			}
		}

		for (const TPair<FString, const FCompositionEntryView*>& Pair
			: EntryByUnit)
		{
			FUnitDraft& Draft =
				Drafts[DraftIndex.FindChecked(Pair.Key)];
			const FCompositionEntryView& Entry = *Pair.Value;
			const FEntryRoute Route =
				RouteEntry(Request.TargetKind, Entry.SemanticId, Selector(Entry, TEXT("preview_target")));
			if (Route.bGraph)
			{
				const FString DeclaredOwner = FindDeclaredItemOwner(Entry, SourceItemUnits);
				if (!Selector(Entry, TEXT("owner_item_id")).IsEmpty() && DeclaredOwner.IsEmpty())
				{
					OutError.SetSourceFailure(TEXT("composition_graph_owner_unresolved"), TEXT("The declared graph owner does not match any captured item."), Draft.UnitId);
					return false;
				}
				if (!DeclaredOwner.IsEmpty())
				{
					Draft.OwnerRef = DeclaredOwner;
					FUeAgentCanonicalValuePtr OwnerValue;
					FUeAgentStateSemanticError Error;
					if (!FUeAgentCanonicalValue::TryString(DeclaredOwner, Resources, OwnerValue, Error))
					{
						OutError.SetFailure(Error, FString());
						return false;
					}
					Draft.Values.Add(TEXT("owner"), OwnerValue.ToSharedRef());
				}
			}
			if (Route.bItem)
			{
				Draft.OwnerRef = FindGraphOwner(
					Entry,
					GraphUnits,
					Draft.OwnerRef);
				if (Route.bGraphItem) Draft.GraphOwnerRef = Draft.OwnerRef;
				const FString DeclaredOwner =
					FindDeclaredItemOwner(Entry, SourceItemUnits);
				if (!DeclaredOwner.IsEmpty())
				{
					Draft.OwnerRef = DeclaredOwner;
				}
				else if (Entry.SemanticId.EndsWith(TEXT(".pin")))
				{
					Draft.OwnerRef = FindItemOwner(
						Entry,
						ItemUnits,
						Draft.OwnerRef);
				}
				else if (Entry.SemanticId.EndsWith(
					TEXT(".dynamic_input")))
				{
					Draft.OwnerRef = FindItemOwnerBySelector(
						Entry,
						ItemUnits,
						TEXT("module_guid"),
						Draft.OwnerRef);
				}
				else if (Entry.SemanticId.EndsWith(
						TEXT(".renderer"))
					|| Entry.SemanticId.EndsWith(
						TEXT(".event_handler")))
				{
					Draft.OwnerRef = FindItemOwnerBySelector(
						Entry,
						ItemUnits,
						TEXT("emitter_handle_id"),
						Draft.OwnerRef);
					Draft.OwnerRef = FindItemOwnerBySelector(
						Entry,
						ItemUnits,
						TEXT("emitter_name"),
						Draft.OwnerRef);
				}
				else if (Entry.SemanticId.EndsWith(
					TEXT(".component.target")))
				{
					Draft.OwnerRef = FindItemOwnerByAliasedSelector(
						Entry,
						ItemUnits,
						TEXT("owner_actor_guid"),
						TEXT("actor_guid"),
						Draft.OwnerRef);
					Draft.OwnerRef = FindItemOwnerByAliasedSelector(
						Entry,
						ItemUnits,
						TEXT("owner_managed_id"),
						TEXT("managed_id"),
						Draft.OwnerRef);
				}
			}
			const FUeAgentCanonicalValuePtr Layout =
				FindLayoutValue(Entry);
			if (Layout.IsValid())
			{
				FUnitDraft LayoutDraft;
				LayoutDraft.UnitId = StableId(
					TEXT("layout"),
					Draft.UnitId);
				LayoutDraft.UnitKind =
					ESemanticUnitKind::LayoutState;
				LayoutDraft.OwnerRef = Draft.UnitId;
				LayoutDraft.SemanticTypeRef =
					TEXT("uai.authoring.layout/1");
				LayoutDraft.Role = TEXT("editor_layout");
				LayoutDraft.SourceProvenance =
					Draft.SourceProvenance;
				if (!TryBuildValues(
					Layout,
					{},
					Resources,
					LayoutDraft.Values,
					OutError))
				{
					return false;
				}
				AddDraft(MoveTemp(LayoutDraft));
			}
		}

		for (const FCompositionEntryView& Entry : Entries)
		{
			if (!Entry.bAvailable || !Entry.Value.IsValid())
			{
				continue;
			}
			const FEntryRoute Route =
				RouteEntry(Request.TargetKind, Entry.SemanticId, Selector(Entry, TEXT("preview_target")));
			if (!Route.bProperty && !Route.bRelation)
			{
				continue;
			}
			if (!Entry.bAuthoringVisible)
			{
				continue;
			}
			const FString* SurfaceUnit =
				SurfaceUnits.Find(Route.SurfaceId);
			if (!SurfaceUnit)
			{
				continue;
			}
			const FString DeclaredOwner = Route.bProperty
				? FindDeclaredItemOwner(Entry, SourceItemUnits)
				: FString();
			const FString OwnerRef = !DeclaredOwner.IsEmpty()
				? DeclaredOwner
				: Route.bProperty
					? FindItemOwner(Entry, ItemUnits, *SurfaceUnit)
					: FindGraphOwner(Entry, GraphUnits, *SurfaceUnit);
			FString DefinitionId;
			if (Route.bProperty
				&& !EnsurePropertyDefinition(Entry, DefinitionId))
			{
				return false;
			}
			if (Route.bRelation)
			{
				const FString AggregateKey =
					OwnerRef + TEXT("|") + Route.SurfaceId;
				FRelationAggregate& Aggregate =
					RelationAggregates.FindOrAdd(AggregateKey);
				Aggregate.OwnerRef = OwnerRef;
				Aggregate.SurfaceId = Route.SurfaceId;
				TMap<FString, FUeAgentCanonicalValueRef>& Group =
					Aggregate.Groups.FindOrAdd(Entry.SemanticId);
				if (Group.Contains(Entry.TargetIdentity.CanonicalKey))
				{
					OutError.SetSourceFailure(TEXT("composition_relation_identity_conflict"), TEXT("Two captured relations have the same identity within one relation group."),
						Entry.TargetIdentity.CanonicalKey);
					return false;
				}
				Group.Add(
					Entry.TargetIdentity.CanonicalKey,
					Entry.Value.ToSharedRef());

				if (Entry.Value->GetKind() == EUeAgentValueKind::Object)
				{
					for (const TCHAR* Field : {
						TEXT("from_node"),
						TEXT("to_node")})
					{
						const FUeAgentCanonicalValueRef* NodeValue =
							FindObjectField(
								Entry.Value.ToSharedRef(),
								Field);
						if (!NodeValue
							|| (*NodeValue)->GetKind()
								!= EUeAgentValueKind::String
							|| !(*NodeValue)->GetText().StartsWith(
								TEXT("builtin:")))
						{
							continue;
						}
						const FString& NodeRef = (*NodeValue)->GetText();
						const FString BuiltinId = StableId(
							TEXT("item"),
							Request.TargetKind + TEXT("|")
								+ OwnerRef + TEXT("|") + NodeRef);
						if (DraftIndex.Contains(BuiltinId))
						{
							continue;
						}
						FUnitDraft Builtin;
						Builtin.UnitId = BuiltinId;
						Builtin.UnitKind =
							ESemanticUnitKind::ItemState;
						Builtin.OwnerRef = OwnerRef;
						Builtin.SemanticTypeRef =
							TEXT("ue.blueprint.node.builtin");
						Builtin.Role = TEXT("graph_collection");
						Builtin.SourceProvenance = Provenance;
						FUeAgentCanonicalValuePtr BuiltinValue;
						FUeAgentStateSemanticError SemanticError;
						if (!FUeAgentCanonicalValue::TryString(
							NodeRef,
							Resources,
							BuiltinValue,
							SemanticError))
						{
							OutError.SetFailure(SemanticError, FString());
							return false;
						}
						Builtin.Values.Add(
							TEXT("builtin_id"),
							BuiltinValue.ToSharedRef());
						AddDraft(MoveTemp(Builtin));
					}
				}
				continue;
			}
			FUnitDraft Draft;
			Draft.UnitId = StableId(
				TEXT("property"),
				Request.TargetKind + TEXT("|")
					+ Entry.SemanticId + TEXT("|")
					+ Entry.TargetIdentity.CanonicalKey);
			Draft.UnitKind = Route.UnitKind;
			Draft.OwnerRef = OwnerRef;
			Draft.SemanticTypeRef = Entry.SemanticId;
			Draft.TargetIdentityKey =
				Entry.TargetIdentity.CanonicalKey;
			Draft.Role = Route.SurfaceId;
			Draft.FieldSetRef = Route.bProperty
				? Normalize(Selector(Entry, TEXT("property_path")))
				: FString();
			if (Draft.FieldSetRef.IsEmpty() && Route.bProperty)
			{
				Draft.FieldSetRef =
					Normalize(Selector(Entry, TEXT("selector")));
				if (Draft.FieldSetRef.IsEmpty())
				{
					TArray<FString> SemanticSegments;
					Entry.SemanticId.ParseIntoArray(
						SemanticSegments, TEXT("."), true);
					if (SemanticSegments.Num() >= 2)
					{
						Draft.FieldSetRef = NormalizeRoleToken(
							SemanticSegments[SemanticSegments.Num() - 2])
							+ TEXT("_")
							+ NormalizeRoleToken(SemanticSegments.Last());
					}
					else
					{
						Draft.FieldSetRef =
							NormalizeRoleToken(Entry.SemanticId);
					}
					if (Draft.FieldSetRef.IsEmpty())
					{
						Draft.FieldSetRef =
							NormalizeRoleToken(Route.SurfaceId);
					}
				}
			}
			Draft.SourceProvenance =
				Entry.Provenance.IsEmpty()
					? Provenance
					: Entry.Provenance;
			Draft.CategoryPath = Entry.CategoryPath;
			if (!DefinitionId.IsEmpty()) Draft.DependencyRefs.Add(DefinitionId);
			if (!TryBuildValues(
				Entry.Value,
				{},
				Resources,
				Draft.Values,
				OutError))
			{
				return false;
			}
			AddDraft(MoveTemp(Draft));
		}

		TArray<FString> RelationAggregateKeys;
		RelationAggregates.GetKeys(RelationAggregateKeys);
		RelationAggregateKeys.Sort();
		for (const FString& AggregateKey : RelationAggregateKeys)
		{
			const FRelationAggregate& Aggregate =
				RelationAggregates.FindChecked(AggregateKey);
			TArray<FString> SemanticIds;
			Aggregate.Groups.GetKeys(SemanticIds);
			SemanticIds.Sort();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> GroupValues;
			for (const FString& SemanticId : SemanticIds)
			{
				const TMap<FString, FUeAgentCanonicalValueRef>& Records =
					Aggregate.Groups.FindChecked(SemanticId);
				TArray<FString> RecordIds;
				Records.GetKeys(RecordIds);
				RecordIds.Sort();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> RecordValues;
				for (const FString& RecordId : RecordIds)
				{
					RecordValues.Emplace(
						RecordId,
						Records.FindChecked(RecordId));
				}
				FUeAgentCanonicalValuePtr GroupValue;
				FUeAgentStateSemanticError SemanticError;
				if (!FUeAgentCanonicalValue::TryObject(
					RecordValues,
					Resources,
					GroupValue,
					SemanticError))
				{
					OutError.SetFailure(SemanticError, FString());
					return false;
				}
				GroupValues.Emplace(
					SemanticId,
					GroupValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr RelationsValue;
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryObject(
				GroupValues,
				Resources,
				RelationsValue,
				SemanticError))
			{
				OutError.SetFailure(SemanticError, FString());
				return false;
			}
			FUnitDraft RelationSet;
			RelationSet.UnitId = StableId(
				TEXT("relation"),
				Request.TargetKind + TEXT("|") + AggregateKey);
			RelationSet.UnitKind = ESemanticUnitKind::RelationSet;
			RelationSet.OwnerRef = Aggregate.OwnerRef;
			RelationSet.SemanticTypeRef =
				TEXT("uai.authoring.relations/1");
			RelationSet.Role = Aggregate.SurfaceId;
			RelationSet.SourceProvenance = Provenance;
			RelationSet.Values.Add(
				TEXT("relations"),
				RelationsValue.ToSharedRef());
			AddDraft(MoveTemp(RelationSet));
		}

		TArray<FString> DependencyPaths;
		for (const FCompositionEntryView& Entry : Entries)
		{
			if (!Entry.Value.IsValid()
				|| Entry.Value->GetKind() != EUeAgentValueKind::Object)
			{
				continue;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Entry.Value->GetObjectItems())
			{
				if (Field.Key == TEXT("asset_path")
					|| !Field.Key.EndsWith(TEXT("_path"))
					|| Field.Value->GetKind()
						!= EUeAgentValueKind::String)
				{
					continue;
				}
				const FString& Path = Field.Value->GetText();
				if (Path.StartsWith(TEXT("/")))
				{
					DependencyPaths.AddUnique(Path);
				}
			}
		}
		DependencyPaths.Sort();
		if (!DependencyPaths.IsEmpty())
		{
			FUnitDraft Dependencies;
			Dependencies.UnitId = StableId(
				TEXT("dependencies"),
				TargetId);
			Dependencies.UnitKind =
				ESemanticUnitKind::DependencyState;
			Dependencies.OwnerRef = TargetId;
			Dependencies.SemanticTypeRef =
				TEXT("uai.authoring.dependencies/1");
			Dependencies.Role = TEXT("asset_dependencies");
			Dependencies.SourceProvenance = Provenance;
			FUeAgentCanonicalValuePtr PathsValue;
			if (!TryStringArrayValue(
				DependencyPaths,
				Resources,
				PathsValue,
				OutError, Dependencies.UnitId, TEXT("asset_paths")))
			{
				return false;
			}
			Dependencies.Values.Add(
				TEXT("asset_paths"),
				PathsValue.ToSharedRef());
			AddDraft(MoveTemp(Dependencies));
		}

		if (Request.TargetKind == TEXT("ue.level.asset"))
		{
			const int32 Count = Drafts.Num();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FUnitDraft& Actor = Drafts[Index];
				if (Actor.UnitKind != ESemanticUnitKind::ItemState
					|| !Actor.SemanticTypeRef.EndsWith(TEXT(".actor.target"))) continue;
				FUnitDraft Components;
				Components.UnitId = StableId(TEXT("structure"), Actor.UnitId + TEXT("|level_components"));
				Components.UnitKind = ESemanticUnitKind::StructureState;
				Components.OwnerRef = Actor.UnitId;
				Components.SemanticTypeRef = TEXT("uai.authoring.surface.level_components");
				Components.Role = TEXT("level_components");
				Components.SourceProvenance = Actor.SourceProvenance;
				AddDraft(MoveTemp(Components));
			}
		}

		for (const FUnitDraft& Child : Drafts)
		{
			if (Child.OwnerRef.IsEmpty())
			{
				continue;
			}
			if (int32* OwnerIndex = DraftIndex.Find(Child.OwnerRef))
			{
				AddOwnedReference(Drafts[*OwnerIndex], Child);
			}
		}

		TSet<FString> DefinitionUnitIds;
		for (const FUnitDraft& Draft : Drafts)
		{
			if (Draft.Role == TEXT("property_definition"))
			{
				DefinitionUnitIds.Add(Draft.UnitId);
			}
		}
		const int32 ContainerCount = Drafts.Num();
		for (int32 Index = 0; Index < ContainerCount; ++Index)
		{
			FUnitDraft& Container = Drafts[Index];
			if (Container.ItemRefs.IsEmpty())
			{
				continue;
			}
			// Navigation reflects the native sequence owner; it does not author a second order.
			if (!ExplicitOrder.Contains(Container.UnitId))
			{
				for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
				{
					if (Contract.NativeOrderField.IsEmpty() || Contract.AggregatedParentSemanticSuffix.IsEmpty()
						|| !Container.SemanticTypeRef.EndsWith(Contract.AggregatedParentSemanticSuffix)) continue;
					TArray<TPair<FString, FString>> NativeOrder;
					for (const FString& ItemId : Container.ItemRefs)
					{
						const FUnitDraft& Item = Drafts[DraftIndex.FindChecked(ItemId)];
						if (!Item.SemanticTypeRef.EndsWith(Contract.AggregatedItemSemanticSuffix)) continue;
						const FUeAgentCanonicalValueRef* Value = Item.Values.Find(Contract.NativeOrderField);
						if (Value && ((*Value)->GetKind() == EUeAgentValueKind::Integer
							|| (*Value)->GetKind() == EUeAgentValueKind::Decimal))
							NativeOrder.Emplace((*Value)->GetLexeme(), ItemId);
					}
					NativeOrder.Sort([](const auto& Left, const auto& Right) {
						int32 Comparison = 0;
						return TryCompareNumberLexemes(Left.Key, Right.Key, Comparison) && Comparison != 0
							? Comparison < 0 : Left.Value < Right.Value;
					});
					TArray<FString>& Ordered = ExplicitOrder.FindOrAdd(Container.UnitId);
					for (const auto& Item : NativeOrder) Ordered.Add(Item.Value);
					break;
				}
			}
			if (const TArray<FString>* Ordered =
				ExplicitOrder.Find(Container.UnitId))
			{
				TArray<FString> StableOrder;
				for (const FString& ItemId : *Ordered)
				{
					if (Container.ItemRefs.Contains(ItemId))
					{
						StableOrder.AddUnique(ItemId);
					}
				}
				TArray<FString> Remaining = Container.ItemRefs;
				Remaining.RemoveAll(
					[&StableOrder](const FString& ItemId)
					{
						return StableOrder.Contains(ItemId);
					});
				Remaining.Sort();
				StableOrder.Append(Remaining);
				Container.ItemRefs = MoveTemp(StableOrder);
			}
			else
			{
				Container.ItemRefs.Sort();
			}
			FUnitDraft Ordering;
			Ordering.UnitId = StableId(
				TEXT("ordering"),
				Container.UnitId);
			Ordering.UnitKind =
				ESemanticUnitKind::OrderingState;
			Ordering.OwnerRef = Container.UnitId;
			Ordering.SemanticTypeRef =
				TEXT("uai.authoring.ordering/1");
			Ordering.Role = TEXT("stable_item_order");
			Ordering.SourceProvenance = Provenance;
			TArray<FString> ModelVisibleOrder = Container.ItemRefs;
			ModelVisibleOrder.RemoveAll(
				[&DefinitionUnitIds](const FString& ItemId)
				{
					return DefinitionUnitIds.Contains(ItemId);
				});
			FUeAgentCanonicalValuePtr OrderValue;
			if (!TryStringArrayValue(
				ModelVisibleOrder,
				Resources,
				OrderValue,
				OutError, Ordering.UnitId, TEXT("item_refs")))
			{
				return false;
			}
			Ordering.Values.Add(
				TEXT("item_refs"),
				OrderValue.ToSharedRef());
			Container.OrderingRef = Ordering.UnitId;
			AddDraft(MoveTemp(Ordering));
		}

		FKeyedItemProjectionContract ActorItems;
		FKeyedItemProjectionContract ComponentItems;
		FKeyedItemProjectionContract ModuleItems;
		FKeyedItemProjectionContract EmitterItems;
		FKeyedItemProjectionContract UserParameterItems;
		FKeyedItemProjectionContract WidgetItems;
		FKeyedItemProjectionContract BindingItems;
		FKeyedItemProjectionContract AnimationItems;
		FKeyedItemProjectionContract TreeItems;
		FKeyedItemProjectionContract BlackboardItems;
		FKeyedItemProjectionContract SmartObjectItems;
		TryGetKeyedItemProjectionContract(TEXT("actors"), ActorItems);
		TryGetKeyedItemProjectionContract(TEXT("components"), ComponentItems);
		TryGetKeyedItemProjectionContract(TEXT("modules"), ModuleItems);
		TryGetKeyedItemProjectionContract(TEXT("emitters"), EmitterItems);
		TryGetKeyedItemProjectionContract(TEXT("user_parameters"), UserParameterItems);
		TryGetKeyedItemProjectionContract(TEXT("widgets"), WidgetItems);
		TryGetKeyedItemProjectionContract(TEXT("bindings"), BindingItems);
		TryGetKeyedItemProjectionContract(TEXT("animations"), AnimationItems);
		TryGetKeyedItemProjectionContract(TEXT("tree_items"), TreeItems);
		TryGetKeyedItemProjectionContract(TEXT("blackboard_keys"), BlackboardItems);
		TryGetKeyedItemProjectionContract(TEXT("smart_object_items"), SmartObjectItems);
		if (Request.TargetKind == TEXT("ue.smart_object.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("smart_object"),
				SmartObjectItems.AggregatedItemSemanticSuffix, SmartObjectItems.RootField,
				SmartObjectItems.NamingPolicy, Resources, OutError)) return false;
		if (Request.TargetKind == TEXT("ue.state_tree.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("state_tree"),
				TreeItems.AggregatedItemSemanticSuffix, TreeItems.RootField,
				TreeItems.NamingPolicy, Resources, OutError)) return false;
		if (Request.TargetKind == TEXT("ue.blackboard.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("blackboard"),
				BlackboardItems.AggregatedItemSemanticSuffix, BlackboardItems.RootField,
				BlackboardItems.NamingPolicy, Resources, OutError)) return false;
		if (Request.TargetKind == TEXT("ue.widget_blueprint.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("widget_tree"),
				WidgetItems.AggregatedItemSemanticSuffix, WidgetItems.RootField,
				WidgetItems.NamingPolicy, Resources, OutError))
		{
			return false;
		}
		if (Request.TargetKind == TEXT("ue.widget_blueprint.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("binding_collection"),
				BindingItems.AggregatedItemSemanticSuffix, BindingItems.RootField,
				BindingItems.NamingPolicy, Resources, OutError)) return false;
		if (Request.TargetKind == TEXT("ue.widget_blueprint.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("animation_collection"),
				AnimationItems.AggregatedItemSemanticSuffix, AnimationItems.RootField,
				AnimationItems.NamingPolicy, Resources, OutError)) return false;
		if (Request.TargetKind.StartsWith(TEXT("ue.niagara_"))
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("stack_collection"),
				ModuleItems.AggregatedItemSemanticSuffix, ModuleItems.RootField,
				ModuleItems.NamingPolicy, Resources, OutError))
		{
			return false;
		}
		if (Request.TargetKind == TEXT("ue.niagara_system.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("emitter_hierarchy"),
				EmitterItems.AggregatedItemSemanticSuffix, EmitterItems.RootField,
				EmitterItems.NamingPolicy, Resources, OutError)) return false;
		if (Request.TargetKind == TEXT("ue.niagara_system.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("user_parameters"),
				UserParameterItems.AggregatedItemSemanticSuffix, UserParameterItems.RootField,
				UserParameterItems.NamingPolicy, Resources, OutError)) return false;
		if (Request.TargetKind == TEXT("ue.level.asset")
			&& !TryAddKeyedItemDesiredStateProjection(
				Drafts,
				TEXT("actor_component_hierarchy"),
				TEXT(".actor.target"),
				TEXT("actors"),
				ActorItems.NamingPolicy,
				Resources,
				OutError))
		{
			return false;
		}
		if (Request.TargetKind == TEXT("ue.level.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("level_components"),
				TEXT(".component.target"), TEXT("components"), ComponentItems.NamingPolicy,
				Resources, OutError, true)) return false;

		if (Request.TargetKind == TEXT("ue.blueprint.asset")
			&& !TryAddKeyedItemDesiredStateProjection(
				Drafts,
				TEXT("component_tree"),
				TEXT(".component"),
				TEXT("components"),
				ComponentItems.NamingPolicy,
				Resources,
				OutError))
		{
			return false;
		}

		if ((Request.TargetKind == TEXT("ue.blueprint.asset") || Request.TargetKind == TEXT("ue.widget_blueprint.asset") || Request.TargetKind == TEXT("ue.anim_blueprint.asset"))
			&& (!TryAddKeyedItemDesiredStateProjection(
					Drafts,
					TEXT("member_collection"),
					TEXT(".interface"),
					TEXT("interfaces"),
					ComponentItems.NamingPolicy,
					Resources,
					OutError)
				|| !TryAddKeyedItemDesiredStateProjection(
					Drafts,
					TEXT("member_collection"),
					TEXT(".member.variable"),
					TEXT("variables"),
					ComponentItems.NamingPolicy,
					Resources,
					OutError)
				|| !TryAddKeyedItemDesiredStateProjection(
					Drafts,
					TEXT("member_collection"),
					TEXT(".member.delegate"),
					TEXT("delegates"),
					ComponentItems.NamingPolicy,
					Resources,
					OutError)))
		{
			return false;
		}

		if (!TryAddGraphDesiredStateProjection(Drafts, Resources, OutError))
		{
			return false;
		}
		if ((Request.TargetKind == TEXT("ue.blueprint.asset") || Request.TargetKind == TEXT("ue.widget_blueprint.asset")
			|| Request.TargetKind == TEXT("ue.anim_blueprint.asset"))
			&& !TryAddBlueprintGraphDeclarations(Drafts, Resources, OutError)) return false;

		TArray<FSemanticUnit> Units;
		Units.Reserve(Drafts.Num());
		for (FUnitDraft& Draft : Drafts)
		{
			Draft.ItemRefs.Sort();
			Draft.RelationRefs.Sort();
			Draft.DependencyRefs.Sort();
			Units.Emplace(
				MoveTemp(Draft.UnitId),
				Draft.UnitKind,
				MoveTemp(Draft.OwnerRef),
				MoveTemp(Draft.SemanticTypeRef),
				MoveTemp(Draft.Role),
				MoveTemp(Draft.FieldSetRef),
				MoveTemp(Draft.Values),
				MoveTemp(Draft.ItemRefs),
				MoveTemp(Draft.RelationRefs),
				MoveTemp(Draft.OrderingRef),
				MoveTemp(Draft.LayoutRef),
				MoveTemp(Draft.DependencyRefs),
				MoveTemp(Draft.ProfileVisibility),
				MoveTemp(Draft.SourceProvenance),
				MoveTemp(Draft.TargetIdentityKey),
				MoveTemp(Draft.CategoryPath));
		}
		if (OutPreparationCharges) *OutPreparationCharges = CaptureChargesBetween(PreparationBefore, Resources.Snapshot());
		return TryCompileAuthoringModel(
			CompositionSchema,
			1,
			Request.ProfileId,
			Request.BaseRevision,
			MoveTemp(Units),
			{TargetId},
			Resources,
			OutModel,
			OutError);
	}

	struct FObservedCanonicalValue
	{
		FUeAgentCanonicalValuePtr Value;
		TMap<EUeAgentResourceDimension, int64> Charges;
	};
	struct FCanonicalEntryObservation
	{
		TSharedPtr<const uint8> Observation;
		FObservedCanonicalValue Value;
		FUeAgentCanonicalValuePtr Descriptor;
		FCaptureCharges DescriptorCharges;
		FString RevisionValueKey;
		bool bAuthoringVisible = true;
		int32 CanonicalIndex = INDEX_NONE;
	};
	struct FCompletedCanonicalCapture
	{
		FString TargetKind;
		FString ProfileId;
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FAuthoringModelPtr Model;
		TMap<FString, FCanonicalEntryObservation> Entries;
		FCaptureCharges Charges;
		FCaptureCharges CompositionPreparationCharges;
		TMap<FString, FCaptureCharges> ProjectedValueCharges;
	};
	static thread_local TSharedPtr<const FCompletedCanonicalCapture> PreviousCanonicalCapture;

	static bool TryComposeAuthoringModelFromCapture(
		const FCompositionRequest& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError,
		const TMap<const FJsonValue*, FObservedCanonicalValue>* ObservedValues,
		FCaptureCharges* OutPreparationCharges = nullptr,
		TMap<FString, FCaptureCharges>* OutProjectedValueCharges = nullptr)
	{
		const double ComposeStarted = FPlatformTime::Seconds();
		const auto ViewsBefore = Resources.Snapshot();
		FCompositionInput Input;
		Input.TargetKind = Request.TargetKind;
		Input.ProfileId = Request.ProfileId;
		Input.BaseRevision = Request.BaseRevision;
		Input.SourceProvenance = Request.SourceProvenance;
		Input.Entries.Reserve(Request.Entries.Num());
		int32 ReusedCanonicalValues = 0;
		for (const UeAgentAuthoringState::FStateEntry& Entry
			: Request.Entries)
		{
			FCompositionEntryView View;
			View.SemanticId = Entry.SemanticId;
			View.TargetIdentity.CanonicalKey =
				Entry.TargetIdentity.CanonicalKey;
			View.TargetIdentity.Selectors =
				Entry.TargetIdentity.Selectors;
			View.Type = Entry.Type.ToJsonObject();
			View.bAvailable = Entry.bAvailable;
			View.bEditable = Entry.bEditable;
			View.bAuthoringVisible = Entry.bAuthoringVisible;
			View.Constraints = Entry.Constraints;
			View.CandidatesHeader = Entry.CandidatesHeader;
			View.Candidates = Entry.Candidates;
			View.ValueDefinition = Entry.ValueDefinition;
			View.CategoryPath = Entry.CategoryPath;
			View.Provenance = Entry.Provenance;
			TSharedPtr<FJsonValue> ProjectedValue;
			const auto ValueBefore = Resources.Snapshot();
			View.bAuthoringVisible &= UeAgentPropertyValueCodec::ProjectAuthoringValue(
				View.Type, Entry.Value, Entry.ValueDefinition, ProjectedValue, View.ValueDefinition);
			const auto* Observed = ObservedValues && ProjectedValue.Get() == Entry.Value.Get()
				? ObservedValues->Find(Entry.Value.Get()) : nullptr;
			if (Observed)
			{
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(Observed->Charges, TEXT("composition:captured_value"), ResourceError))
				{ OutError.SetFailure(ResourceError); return false; }
				View.Value = Observed->Value;
				++ReusedCanonicalValues;
			}
			else if (ProjectedValue.IsValid()
				&& !TryCanonicalizeJson(
					ProjectedValue,
					Resources,
					View.Value,
					OutError))
			{
				return false;
			}
			if (!Observed && OutProjectedValueCharges)
				OutProjectedValueCharges->Add(CaptureEntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey),
					CaptureChargesBetween(ValueBefore, Resources.Snapshot()));
			Input.Entries.Add(MoveTemp(View));
		}
		const double ViewsFinished = FPlatformTime::Seconds();
		const auto ViewCharges = CaptureChargesBetween(ViewsBefore, Resources.Snapshot());
		const bool bComposed = TryComposeEntryViews(
			Input,
			Resources,
			OutModel,
			OutError, nullptr, OutPreparationCharges);
		if (bComposed && OutPreparationCharges)
			for (const auto& Charge : ViewCharges) OutPreparationCharges->FindOrAdd(Charge.Key) += Charge.Value;
		const double ComposeFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=model_compose kind=%s entries=%d reused_canonical_values=%d entry_views_s=%.6f compose_views_s=%.6f total_s=%.6f success=%s"),
			*Request.TargetKind, Request.Entries.Num(), ReusedCanonicalValues, ViewsFinished - ComposeStarted,
			ComposeFinished - ViewsFinished, ComposeFinished - ComposeStarted,
			bComposed ? TEXT("true") : TEXT("false"));
		return bComposed;
	}

	bool TryComposeAuthoringModel(const FCompositionRequest& Request, FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		return TryComposeAuthoringModelFromCapture(Request, Resources, OutModel, OutError, nullptr);
	}

	bool TryComposeCanonicalAuthoringModel(
		const FCanonicalCompositionRequest& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		FCompositionInput Input;
		Input.TargetKind = Request.TargetKind;
		Input.ProfileId = Request.ProfileId;
		Input.BaseRevision = Request.BaseRevision;
		Input.SourceProvenance = Request.SourceProvenance;
		Input.Entries.Reserve(Request.Entries.Num());
		for (const FUeAgentStateEntry& Entry : Request.Entries)
		{
			FCompositionEntryView View;
			View.SemanticId = Entry.GetIdentity().GetSemanticId();
			View.TargetIdentity.CanonicalKey = CanonicalAuthoringTargetKey(Entry);
			View.TargetIdentity.Selectors =
				Entry.GetIdentity().GetSelectors();
			View.Type = CanonicalTypeJson(Entry.GetType());
			View.bEditable = Entry.GetDetails().IsEditable();
			View.Constraints = Entry.GetDetails().GetConstraints();
			View.CategoryPath = Entry.GetDetails().GetCategoryPath();
			if (View.TargetIdentity.Selectors.IsEmpty()
				&& !Entry.GetIdentity().GetSelector().IsEmpty())
			{
				View.TargetIdentity.Selectors.Add(
					TEXT("selector"), Entry.GetIdentity().GetSelector());
			}
			View.Value = FUeAgentCanonicalValuePtr(Entry.GetValue());
			View.bAvailable = Entry.GetDetails().GetAvailability() != TEXT("unavailable");
			View.Provenance = Request.SourceProvenance;
			if (!TryProjectCanonicalAuthoringDescriptor(Entry, View, Resources, OutError)) return false;
			Input.Entries.Add(MoveTemp(View));
		}
		return TryComposeEntryViews(Input, Resources, OutModel, OutError);
	}

	bool TryCompareWorkspaceValues(const FAuthoringModel& Model, const FUeAgentCanonicalValue* Type,
		const FUeAgentCanonicalValue* Definition, const TSharedPtr<FJsonValue>& Before,
		const TSharedPtr<FJsonValue>& After, CanonicalJson::FValueComparison& OutComparison, FWorkspaceError& OutError)
	{
		TSharedPtr<FJsonValue> JsonType, JsonDefinition;
		UE::UAI::Contract::FDiagnostic Error;
		if ((Type && !CanonicalJson::TryToJsonValue(*Type, JsonType, Error))
			|| (Definition && !CanonicalJson::TryToJsonValue(*Definition, JsonDefinition, Error)))
		{ OutError.SetFailure(Error, FString()); return false; }
		if (JsonDefinition)
		{
			JsonDefinition = ExpandPropertyDefinition(Model, JsonDefinition, OutError);
			if (!JsonDefinition) return false;
		}
		// A nested catalog reference expands to a complete descriptor.
		if (JsonDefinition && JsonDefinition->Type == EJson::Object)
		{
			const auto Descriptor = JsonDefinition->AsObject();
			if (const auto CapturedType = Descriptor->TryGetField(TEXT("type")))
			{
				JsonType = CapturedType;
				JsonDefinition = Descriptor->TryGetField(TEXT("value_definition"));
			}
		}
		if (!JsonType && JsonDefinition && JsonDefinition->Type == EJson::Object
			&& JsonDefinition->AsObject()->HasTypedField<EJson::Object>(TEXT("fields")))
		{
			const auto RecordType = MakeShared<FJsonObject>();
			RecordType->SetStringField(TEXT("kind"), TEXT("object"));
			JsonType = MakeShared<FJsonValueObject>(RecordType);
		}
		OutComparison = UeAgentAuthoringSemanticCompare::CompareDeclaredValues(
			JsonType && JsonType->Type == EJson::Object ? JsonType->AsObject() : nullptr,
			JsonDefinition && JsonDefinition->Type == EJson::Object ? JsonDefinition->AsObject() : nullptr,
			Before, After);
		if (!OutComparison.IsComparable()) {
			OutComparison.ApplyTo(Error);
			OutError.SetFailure(Error, FString());
			return false;
		}
		return true;
	}

	bool TryCompareWorkspaceValues(const FAuthoringModel& Model, const FUeAgentCanonicalValue* Type,
		const FUeAgentCanonicalValue* Definition, const FUeAgentCanonicalValueRef& Before,
		const FUeAgentCanonicalValueRef& After, CanonicalJson::FValueComparison& OutComparison, FWorkspaceError& OutError)
	{
		TSharedPtr<FJsonValue> A, B; UE::UAI::Contract::FDiagnostic Error;
		if (!CanonicalJson::TryToJsonValue(*Before, A, Error) || !CanonicalJson::TryToJsonValue(*After, B, Error))
		{ OutError.SetFailure(Error, FString()); return false; }
		return TryCompareWorkspaceValues(Model, Type, Definition, A, B, OutComparison, OutError);
	}

	bool TryCollectChangedUnitValueFields(
		const FSemanticUnit* Before, const FSemanticUnit& Edited,
		FUeAgentResourceTracker& Resources, TSet<FString>& OutFields,
		FWorkspaceError& OutError, const FAuthoringModel* Model)
	{
		OutFields.Reset();
		const FUeAgentCanonicalValue* Type = nullptr;
		const FUeAgentCanonicalValue* Definition = nullptr;
		if (Model)
			for (const auto& Dependency : Edited.GetDependencyRefs())
				if (const auto* Unit = Model->FindUnit(Dependency); Unit && Unit->GetRole() == TEXT("property_definition"))
				{
					if (const auto* Value = Unit->GetValues().Find(TEXT("type"))) Type = &Value->Get();
					if (const auto* Value = Unit->GetValues().Find(TEXT("value_definition"))) Definition = &Value->Get();
					break;
				}
		for (const auto& Field : Edited.GetValues())
		{
			const auto* Previous = Before ? Before->GetValues().Find(Field.Key) : nullptr;
			if (!Previous) { OutFields.Add(Field.Key); continue; }
			if (Model && (Type || Definition))
			{
				TSharedPtr<FJsonValue> A, B; UE::UAI::Contract::FDiagnostic Error;
				if (!CanonicalJson::TryToJsonValue(**Previous, A, Error)
					|| !CanonicalJson::TryToJsonValue(*Field.Value, B, Error))
				{ OutError.SetFailure(Error, Edited.GetUnitId()); return false; }
				if (!(Edited.GetValues().Num() == 1 && Field.Key == TEXT("value")))
				{
					const auto Left = MakeShared<FJsonObject>(), Right = MakeShared<FJsonObject>();
					Left->SetField(Field.Key, A); Right->SetField(Field.Key, B);
					A = MakeShared<FJsonValueObject>(Left); B = MakeShared<FJsonValueObject>(Right);
				}
				CanonicalJson::FValueComparison Compared;
				if (!TryCompareWorkspaceValues(*Model, Type, Definition, A, B, Compared, OutError)) return false;
				if (!Compared.bEquivalent) OutFields.Add(Field.Key);
				continue;
			}
			FUeAgentSemanticComparison Comparison;
			FUeAgentStateSemanticError Error;
			if (!TryCompareValues(*Previous, Field.Value, TEXT("/"), Resources, Comparison, Error))
			{ OutError.SetFailure(Error, Edited.GetUnitId()); return false; }
			if (!Comparison.IsEquivalent()) OutFields.Add(Field.Key);
		}
		if (Before)
			for (const auto& Field : Before->GetValues())
				if (!Edited.GetValues().Contains(Field.Key)) OutFields.Add(Field.Key);
		return true;
	}

	bool TryProjectDocumentValueChanges(
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FAuthoringModelRef& CurrentModel, const FAuthoringModelRef& EditedModel,
		const TOptional<TArray<FString>>& EditedUnitIds, FUeAgentResourceTracker& Resources,
		TMap<FString, FUeAgentCanonicalValuePtr>& OutDocuments,
		TSet<FString>& OutDocumentUnitIds, FWorkspaceError& OutError,
		const TMap<FString, TSet<FString>>* ResolvedKeyedOperationRoots,
		TArray<FDocumentValueSourceBinding>* OutSourceBindings,
		const FAuthoringValueSourceResolver& FindValueSource)
	{
		OutDocuments.Reset();
		OutDocumentUnitIds.Reset();
		if (OutSourceBindings) OutSourceBindings->Reset();
		FCompositionInput Input;
		Input.TargetKind = Snapshot->GetProvenance().GetSourceId();
		Input.ProfileId = CurrentModel->GetProfileId();
		Input.BaseRevision = CurrentModel->GetBaseRevision();
		Input.SourceProvenance = TEXT("authoring_state.capture");
		TMap<FString, const FUeAgentStateEntry*> Sources;
		TSet<FString> VisibleDocumentSemantics;
		for (const FSemanticUnit& Unit : CurrentModel->GetUnits())
		{
			const FString& Type = Unit.GetSemanticTypeRef();
			for (int32 Position = Type.Find(TEXT("#")); Position != INDEX_NONE;
				Position = Type.Find(TEXT("#"), ESearchCase::CaseSensitive,
					ESearchDir::FromStart, Position + 1))
				VisibleDocumentSemantics.Add(Type.Left(Position).ToLower());
		}
		for (const FUeAgentStateEntry& Entry : Snapshot->GetEntries())
		{
			FCompositionEntryView View;
			View.SemanticId = Entry.GetIdentity().GetSemanticId();
			View.TargetIdentity.CanonicalKey = CanonicalAuthoringTargetKey(Entry);
			View.TargetIdentity.Selectors = Entry.GetIdentity().GetSelectors();
			View.Type = CanonicalTypeJson(Entry.GetType());
			View.Value = Entry.GetValue();
			View.bEditable = Entry.GetDetails().IsEditable();
			View.bAvailable = Entry.GetDetails().GetAvailability() != TEXT("unavailable");
			View.Constraints = Entry.GetDetails().GetConstraints();
			View.CategoryPath = Entry.GetDetails().GetCategoryPath();
			View.Provenance = Input.SourceProvenance;
			if (!TryProjectCanonicalAuthoringDescriptor(Entry, View, Resources, OutError)) return false;
			if (RouteEntry(Input.TargetKind, View.SemanticId).bDocument)
			{
				// The supplied current model is the authoritative visible capture.
				View.bAuthoringVisible = VisibleDocumentSemantics.Contains(View.SemanticId.ToLower());
				if (View.bAuthoringVisible)
				{
					const FString SourceKey = DocumentSourceKey(View.SemanticId, View.TargetIdentity.CanonicalKey);
					if (Sources.Contains(SourceKey))
					{ OutError.SetSourceFailure(TEXT("composition_document_source_ambiguous"), TEXT("More than one captured document has the same semantic binding.")); return false; }
					Sources.Add(SourceKey, &Entry);
				}
			}
			Input.Entries.Add(MoveTemp(View));
		}
		if (Sources.IsEmpty()) return true;
		TMap<FString, FDocumentValueBinding> Bindings;
		FAuthoringModelPtr RebuiltModel;
		if (!TryComposeEntryViews(Input, Resources, RebuiltModel, OutError, &Bindings)) return false;
		TMap<FString, FUeAgentCanonicalValuePtr> ProposedDocuments;
		TMap<FString, TArray<FDocumentAddress>> WrittenAddresses;
		for (const auto& Pair : Bindings)
		{
			const auto RejectEdit = [&](const FString& Code, const FString& Reason, const FString& Pointer = TEXT("/"))
			{
				OutError.SetSourceFailure(Code, Reason, Pair.Key, Pointer);
				if (FindValueSource)
					if (const auto* Node = FindValueSource(Pair.Key, Pointer)) OutError.Locate(Node->GetProvenance());
				return false;
			};
			const FSemanticUnit* Before = CurrentModel->FindUnit(Pair.Key);
			const FSemanticUnit* Generated = RebuiltModel->FindUnit(Pair.Key);
			if (!Before || !Generated) continue;
			if (Before->GetOwnerRef() != Generated->GetOwnerRef()
				|| Before->GetTargetIdentityKey() != Generated->GetTargetIdentityKey()
				|| Before->GetSemanticTypeRef() != Generated->GetSemanticTypeRef())
			{ return RejectEdit(TEXT("composition_document_binding_mismatch"),
				Before->GetOwnerRef() != Generated->GetOwnerRef() ? TEXT("The captured document owner differs from the workspace binding.")
				: Before->GetTargetIdentityKey() != Generated->GetTargetIdentityKey() ? TEXT("The captured document target differs from the workspace binding.")
				: TEXT("The captured document semantic type differs from the workspace binding.")); }
			OutDocumentUnitIds.Add(Pair.Key);
			if (OutSourceBindings)
			{
				const FUeAgentStateEntry* Source = Sources.FindRef(Pair.Value.SourceKey);
				if (Source)
				{
					FDocumentValueSourceBinding Binding;
					Binding.DocumentIdentity = Source->GetIdentity().GetCanonicalKey();
					Binding.UnitId = Pair.Key;
					for (const FDocumentAddressStep& Step : Pair.Value.Address)
					{
						const FString Token = Step.Index != INDEX_NONE ? FString::FromInt(Step.Index)
							: Step.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						Binding.DocumentPointer += TEXT("/") + Token;
					}
					Binding.UnitValuePointer = Pair.Value.KeyedContract
						? TEXT("/") + Pair.Value.KeyedContract->RootField
						: Pair.Value.bWholeValue ? TEXT("/value") : FString();
					OutSourceBindings->Add(MoveTemp(Binding));
				}
			}
			if (EditedUnitIds.IsSet() && !EditedUnitIds.GetValue().Contains(Pair.Key)) continue;
			const FSemanticUnit* Edited = EditedModel->FindUnit(Pair.Key);
			if (!Edited || Edited->GetUnitKind() != Before->GetUnitKind()
				|| Edited->GetOwnerRef() != Before->GetOwnerRef()
				|| Edited->GetItemRefs() != Before->GetItemRefs()
				|| Edited->GetRelationRefs() != Before->GetRelationRefs())
			{ return RejectEdit(TEXT("composition_document_structure_edit_unsupported"),
				!Edited ? TEXT("This document unit cannot be removed.")
				: Edited->GetUnitKind() != Before->GetUnitKind() ? TEXT("The document unit kind cannot be changed.")
				: Edited->GetOwnerRef() != Before->GetOwnerRef() ? TEXT("The document unit owner cannot be changed.")
				: Edited->GetItemRefs() != Before->GetItemRefs() ? TEXT("The document unit item bindings cannot be changed directly.")
				: TEXT("The document unit relation bindings cannot be changed directly.")); }
			const FKeyedItemProjectionContract* KeyedContract = Pair.Value.KeyedContract;
			if (Edited->GetValues().Num() != Before->GetValues().Num())
			{ return RejectEdit(TEXT("composition_document_structure_edit_unsupported"), FString::Printf(TEXT("The document unit must retain its %d value fields; %d were supplied."), Before->GetValues().Num(), Edited->GetValues().Num())); }
			TSet<FString> ChangedFields;
			// Keyed documents depend on definitions for nested native properties.
			// Those types do not describe the readable collection itself. Detect
			// authored changes here; its keyed contract resolves them below.
			if (!TryCollectChangedUnitValueFields(Before, *Edited, Resources, ChangedFields, OutError,
				KeyedContract ? nullptr : &EditedModel.Get())) return false;
			for (const FString& Field : ChangedFields)
			{
				if (!Edited->GetValues().Contains(Field))
				{ return RejectEdit(TEXT("composition_document_field_missing"), FString::Printf(TEXT("Required document field '%s' was removed."), *Field), TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))); }
				if (KeyedContract && Field != KeyedContract->RootField)
				{ return RejectEdit(TEXT("composition_document_definition_read_only"), FString::Printf(TEXT("Document definition field '%s' is read-only; edit collection '%s' instead."), *Field, *KeyedContract->RootField), TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))); }
			}
			if (ChangedFields.IsEmpty()) continue;
			const FUeAgentStateEntry* Source = Sources.FindRef(Pair.Value.SourceKey);
			if (!Source || !Source->GetDetails().IsEditable())
			{ return RejectEdit(TEXT("composition_document_not_editable"), !Source ? TEXT("The edited document has no captured source binding.") : TEXT("The captured source document is read-only.")); }
			const FString SourceId = Source->GetIdentity().GetCanonicalKey();
			TArray<FDocumentAddress> Writes;
			if (Pair.Value.bWholeValue) Writes.Add(Pair.Value.Address);
			else for (const FString& Field : ChangedFields) Writes.Add(ChildAddress(Pair.Value.Address, Field));
			for (const auto& Write : Writes)
			{
				for (const auto& Existing : WrittenAddresses.FindOrAdd(SourceId))
				{
					bool bOverlaps = true;
					for (int32 Index = 0; Index < FMath::Min(Write.Num(), Existing.Num()); ++Index)
						bOverlaps &= Write[Index].Index == Existing[Index].Index
							&& Write[Index].Key.Equals(Existing[Index].Key, ESearchCase::CaseSensitive);
					if (bOverlaps) { return RejectEdit(TEXT("composition_document_overlapping_edits"), FString::Printf(TEXT("Document edits at '%s' and '%s' overlap."), *DocumentAddressPointer(Write), *DocumentAddressPointer(Existing))); }
				}
				WrittenAddresses.FindOrAdd(SourceId).Add(Write);
			}
			FUeAgentCanonicalValuePtr Document = ProposedDocuments.FindRef(SourceId);
			if (!Document.IsValid()) Document = Source->GetValue();
			TArray<FUeAgentCanonicalValueRef> Parents;
			FUeAgentCanonicalValuePtr Value = Document;
			for (const FDocumentAddressStep& Step : Pair.Value.Address)
			{
				Parents.Add(Value.ToSharedRef());
				if (Step.Index != INDEX_NONE && Value->GetKind() == EUeAgentValueKind::Array
					&& Value->GetArrayItems().IsValidIndex(Step.Index))
					Value = Value->GetArrayItems()[Step.Index];
				else if (Step.Index == INDEX_NONE && Value->GetKind() == EUeAgentValueKind::Object)
				{
					const auto* Field = Value->GetObjectItems().FindByPredicate([&](const auto& Item) {
						return Item.Key.Equals(Step.Key, ESearchCase::CaseSensitive); });
					if (!Field) { return RejectEdit(TEXT("composition_document_address_missing"), FString::Printf(TEXT("Field '%s' is absent from the captured document at '%s'."), *Step.Key, *DocumentAddressPointer(Pair.Value.Address))); }
					Value = Field->Value;
				}
				else { return RejectEdit(TEXT("composition_document_address_invalid"),
					Step.Index != INDEX_NONE ? FString::Printf(TEXT("Document path '%s' requires array index %d, but the captured value is not an array or the index is out of range."), *DocumentAddressPointer(Pair.Value.Address), Step.Index)
					: FString::Printf(TEXT("Document path '%s' requires an object at field '%s'."), *DocumentAddressPointer(Pair.Value.Address), *Step.Key)); }
			}
			FUeAgentStateSemanticError Error;
			if (KeyedContract)
			{
				const auto* Keys = Edited->GetValues().Find(KeyedContract->RootField);
				bool bSemanticChanged = false;
				FUeAgentCanonicalValuePtr Resolved;
				// Trust only collections actually normalized by the workspace resolver.
				// A registered name or a caller-supplied operation is not provenance.
				const TSet<FString>* ResolvedRoots = ResolvedKeyedOperationRoots
					? ResolvedKeyedOperationRoots->Find(Pair.Key) : nullptr;
				const bool bOperationRecords = ResolvedRoots && ResolvedRoots->Contains(KeyedContract->RootField);
				FKeyedItemReferenceContext ReferenceContext;
				for (const auto& Sibling : Bindings)
				{
					const auto& Binding = Sibling.Value;
					if (Binding.SourceKey != Pair.Value.SourceKey || !Binding.KeyedContract || !Binding.CapturedItems) continue;
					const auto* SiblingUnit = EditedModel->FindUnit(Sibling.Key);
					const auto* Desired = SiblingUnit ? SiblingUnit->GetValues().Find(Binding.KeyedContract->RootField) : nullptr;
					const auto* SiblingResolved = ResolvedKeyedOperationRoots ? ResolvedKeyedOperationRoots->Find(Sibling.Key) : nullptr;
					ReferenceContext.Add(Binding.KeyedContract->RootField, {Binding.KeyedContract, Binding.CapturedItems.Get(),
						Desired ? &Desired->Get() : nullptr, SiblingResolved && SiblingResolved->Contains(Binding.KeyedContract->RootField)});
				}
				if (!Keys) return RejectEdit(TEXT("composition_document_field_missing"), FString::Printf(TEXT("Required collection '%s' was removed."), *KeyedContract->RootField), TEXT("/") + KeyedContract->RootField);
				if (!TryResolveKeyedItemDesiredState(Keys->Get(), *Value, Pair.Key,
					*KeyedContract, Pair.Value.RecordDefaults.Get(), Resources, Resolved, bSemanticChanged, OutError,
					Pair.Value.NestedRecordDefaults.Get(), bOperationRecords
						? EKeyedItemInputKind::ResolvedOperationRecords : EKeyedItemInputKind::VisibleDesiredState, &ReferenceContext,
					FindValueSource ? FindValueSource(Pair.Key, TEXT("/") + KeyedContract->RootField) : nullptr)) return false;
				if (OutSourceBindings)
				{
					// Copy: adding nested bindings may reallocate the source array.
					const auto* Root = OutSourceBindings->FindByPredicate([&](const auto& Binding) {
						return Binding.DocumentIdentity == SourceId && Binding.UnitId == Pair.Key; });
					if (Root)
					{
						const auto RootBinding = *Root;
						BindKeyedDocumentSources(Keys->Get(), *Resolved, *KeyedContract, RootBinding, *OutSourceBindings);
					}
				}
				Value = Resolved;
			}
			else if (Pair.Value.bWholeValue)
			{
				const auto* Whole = Edited->GetValues().Find(TEXT("value"));
				if (!Whole) return RejectEdit(TEXT("composition_document_value_missing"), TEXT("The edited document is missing its required value field."), TEXT("/value"));
				Value = *Whole;
			}
			else
			{
				if (Value->GetKind() != EUeAgentValueKind::Object)
				{ return RejectEdit(TEXT("composition_document_object_required"), TEXT("Field edits require the captured document value to be an object.")); }
				auto Fields = Value->GetObjectItems();
				for (auto& Field : Fields)
					if (ChangedFields.Contains(Field.Key)) Field.Value = Edited->GetValues().FindChecked(Field.Key);
				if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Value, Error))
				{ OutError.SetFailure(Error, Pair.Key); return false; }
			}
			for (int32 Index = Parents.Num() - 1; Index >= 0; --Index)
			{
				const auto& Parent = Parents[Index];
				const auto& Step = Pair.Value.Address[Index];
				if (Step.Index != INDEX_NONE)
				{
					auto Items = Parent->GetArrayItems();
					Items[Step.Index] = Value.ToSharedRef();
					if (!FUeAgentCanonicalValue::TryArray(Items, Resources, Value, Error))
					{ OutError.SetFailure(Error, Pair.Key); return false; }
				}
				else
				{
					auto Fields = Parent->GetObjectItems();
					for (auto& Field : Fields)
						if (Field.Key.Equals(Step.Key, ESearchCase::CaseSensitive)) Field.Value = Value.ToSharedRef();
					if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Value, Error))
					{ OutError.SetFailure(Error, Pair.Key); return false; }
				}
			}
			ProposedDocuments.Add(SourceId, Value);
		}
		OutDocuments = MoveTemp(ProposedDocuments);
		return true;
	}

	static bool SameCaptureDescriptor(const FUeAgentCanonicalValuePtr& A, const FUeAgentCanonicalValuePtr& B)
	{
		if (A == B) return true;
		if (!A || !B) return false;
		TSharedPtr<FJsonValue> Left, Right; Contract::FDiagnostic Error;
		return StateSemantic::CanonicalJson::TryToJsonValue(*A, Left, Error)
			&& StateSemantic::CanonicalJson::TryToJsonValue(*B, Right, Error)
			&& StateSemantic::CanonicalJson::AreJsonValuesEquivalent(Left, Right);
	}

	// The common capture owns proof and reuse. This first dependency closure is
	// deliberately the existing Level *.property route: its numeric values are
	// excluded from keyed Actor/Component child-property merging. Other shapes
	// retain the full composer, including strings/references and metadata edits.
	static TOptional<bool> TryComposeValueOnlyCapture(const FCompositionRequest& Request,
		const TArray<FUeAgentStateEntry>& CurrentEntries, const FCompletedCanonicalCapture& Previous,
		FCompletedCanonicalCapture& Next, FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		if (Request.TargetKind != TEXT("ue.level.asset") || Previous.Entries.Num() != Request.Entries.Num()) return {};
		const double Started = FPlatformTime::Seconds();
		TMap<FString, TMap<FString, FUeAgentCanonicalValueRef>> ChangedValues;
		FCaptureCharges PreparationCharges = Previous.CompositionPreparationCharges;
		TMap<FString, FCaptureCharges> ProjectedCharges = Previous.ProjectedValueCharges;
		for (int32 I = 0; I < Request.Entries.Num(); ++I) {
			const auto& Source = Request.Entries[I];
			const FString Key = CaptureEntryKey(Source.SemanticId, Source.TargetIdentity.CanonicalKey);
			const auto* Old = Previous.Entries.Find(Key);
			if (!Old || Old->CanonicalIndex == INDEX_NONE) return {};
			if (Source.Observation && Old->Observation == Source.Observation) continue;
			if (!Source.bAvailable || !Source.bAuthoringVisible || !Source.SemanticId.EndsWith(TEXT(".property"))
				|| Old->bAuthoringVisible != Source.bAuthoringVisible) return {};
			const auto& Before = Previous.Snapshot->GetEntries()[Old->CanonicalIndex];
			const auto& Current = CurrentEntries[I];
			if (!(Before.GetDetails() == Current.GetDetails())
				|| Before.GetType().GetTypeId() != Current.GetType().GetTypeId()
				|| Before.GetType().GetContainerKind() != Current.GetType().GetContainerKind()
				|| Before.GetDefault().GetProvenance() != Current.GetDefault().GetProvenance()
				|| !SameCaptureDescriptor(Before.GetAuthoringDescriptor(), Current.GetAuthoringDescriptor())
				|| !HaveOnlyNumericOrBooleanValueChanges(Before.GetValue().Get(), Current.GetValue().Get())) return {};
			const auto& BeforeSelectors = Before.GetIdentity().GetSelectors();
			const auto& CurrentSelectors = Current.GetIdentity().GetSelectors();
			if (BeforeSelectors.Num() != CurrentSelectors.Num()) return {};
			for (const auto& Pair : CurrentSelectors) {
				const FString* Value = BeforeSelectors.Find(Pair.Key);
				if (!Value || !Value->Equals(Pair.Value, ESearchCase::CaseSensitive)) return {};
			}
			const FString UnitId = StableId(TEXT("property"), Request.TargetKind + TEXT("|")
				+ Source.SemanticId + TEXT("|") + Source.TargetIdentity.CanonicalKey);
			const auto* Unit = Previous.Model->FindUnit(UnitId);
			if (!Unit || Unit->GetUnitKind() != ESemanticUnitKind::PropertySet
				|| Unit->GetSemanticTypeRef() != Source.SemanticId) return {};
			TSharedPtr<FJsonValue> Projected, Definition;
			if (!UeAgentPropertyValueCodec::ProjectAuthoringValue(Source.Type.ToJsonObject(),
				Source.Value, Source.ValueDefinition, Projected, Definition)) return {};
			auto ValueResources = MakeSemanticWorkspaceResourceTracker();
			const auto ValueBefore = ValueResources.Snapshot();
			FUeAgentCanonicalValuePtr Value;
			if (Projected == Source.Value) {
				const auto& Proof = Next.Entries.FindChecked(Key).Value;
				FUeAgentResourceChargeError ResourceError;
				if (!ValueResources.TryChargeMany(Proof.Charges, TEXT("composition:captured_value"), ResourceError))
				{ OutError.SetFailure(ResourceError); return false; }
				Value = Proof.Value;
			} else if (!TryCanonicalizeJson(Projected, ValueResources, Value, OutError)) return false;
			const auto NewCharges = CaptureChargesBetween(ValueBefore, ValueResources.Snapshot());
			const auto* OldProjection = Previous.ProjectedValueCharges.Find(Key);
			const auto& OldCharges = OldProjection ? *OldProjection : Old->Value.Charges;
			for (const auto& Charge : OldCharges) {
				int64& Total = PreparationCharges.FindOrAdd(Charge.Key);
				if (Total < Charge.Value) return {};
				Total -= Charge.Value;
			}
			for (const auto& Charge : NewCharges) PreparationCharges.FindOrAdd(Charge.Key) += Charge.Value;
			if (Projected == Source.Value) ProjectedCharges.Remove(Key); else ProjectedCharges.Add(Key, NewCharges);
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			if (!TryBuildValues(Value, {}, ValueResources, Values, OutError)) return false;
			ChangedValues.Add(UnitId, MoveTemp(Values));
		}
		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(PreparationCharges, TEXT("composition:unchanged_relationships"), ResourceError))
		{ OutError.SetFailure(ResourceError); return false; }
		TArray<FSemanticUnit> Units = Previous.Model->GetUnits();
		for (auto& Unit : Units)
			if (auto* Values = ChangedValues.Find(Unit.GetUnitId())) Unit = Unit.WithValues(MoveTemp(*Values));
		const bool bCompiled = TryCompileAuthoringModel(Previous.Model->GetContractId(), Previous.Model->GetContractVersion(),
			Request.ProfileId, Request.BaseRevision, MoveTemp(Units), Previous.Model->GetTargetUnitIds(), Resources, OutModel, OutError);
		if (bCompiled) {
			Next.CompositionPreparationCharges = MoveTemp(PreparationCharges);
			Next.ProjectedValueCharges = MoveTemp(ProjectedCharges);
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=model_value_delta kind=%s total_units=%d changed_properties=%d total_s=%.6f success=%s"),
			*Request.TargetKind, Previous.Model->GetUnits().Num(), ChangedValues.Num(),
			FPlatformTime::Seconds() - Started, bCompiled ? TEXT("true") : TEXT("false"));
		return bCompiled;
	}

	bool TryBuildCanonicalAuthoringCapture(const FString& TargetKind, const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		const double CaptureStarted = FPlatformTime::Seconds();
		const auto CaptureUsageBefore = Resources.Snapshot();
		OutSnapshot.Reset();
		OutModel.Reset();
		OutError.Reset();
		// Preview appends to this separate array; projection reads existing JSON
		// and constructs changed subtrees. Keep the snapshot's payloads read-only.
		TArray<UeAgentAuthoringState::FStateEntry> SourceEntries = Snapshot.GetEntries();
		UE::UAI::Contract::FDiagnostic PreviewError;
		if (!AssetPreview::AppendState(TargetKind, SourceEntries, PreviewError))
		{
			OutError.SetFailure(PreviewError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_preview_capture_failed"), TEXT("Asset preview capture failed without returning a diagnostic.")) : PreviewError, TargetKind);
			return false;
		}
		if (TargetKind.IsEmpty() || SourceEntries.IsEmpty())
		{
			OutError.SetSourceFailure(TEXT("authoring_capture_contract_invalid"), TargetKind.IsEmpty() ? TEXT("Authoring capture requires a target kind.") : TEXT("Authoring capture returned no state entries."));
			return false;
		}

		const double SourcePrepared = FPlatformTime::Seconds();
		const FString EffectiveProfile = ProfileId.IsEmpty() ? TEXT("llm_authoring") : ProfileId;
		const auto Previous = PreviousCanonicalCapture;
		const bool bSameCapture = Previous && Previous->TargetKind == TargetKind
			&& Previous->ProfileId == EffectiveProfile && Previous->Entries.Num() == SourceEntries.Num();
		auto Next = MakeShared<FCompletedCanonicalCapture>();
		Next->TargetKind = TargetKind; Next->ProfileId = EffectiveProfile;
		bool bAllUnchanged = bSameCapture;
		int32 ReusedEntries = 0;
		FString RevisionMaterial = TargetKind;
		for (const UeAgentAuthoringState::FStateEntry& Entry : SourceEntries)
		{
			const FString Key = CaptureEntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey);
			const auto* Old = bSameCapture ? Previous->Entries.Find(Key) : nullptr;
			const bool bSameObservation = Old && Entry.Observation && Old->Observation == Entry.Observation;
			bAllUnchanged &= bSameObservation;
			FCanonicalEntryObservation Proof;
			Proof.Observation = Entry.Observation;
			Proof.bAuthoringVisible = Entry.bAuthoringVisible;
			Proof.RevisionValueKey = bSameObservation ? Old->RevisionValueKey
				: UeAgentAuthoringState::MakeStableCanonicalValueKey(Entry.Value);
			RevisionMaterial +=
				TEXT("|") + Entry.SemanticId
				+ TEXT("|") + Entry.TargetIdentity.CanonicalKey
				+ TEXT("|")
				+ Proof.RevisionValueKey;
			Next->Entries.Add(Key, MoveTemp(Proof));
		}
		const FString BaseRevision = TEXT("capture:") + Sha256Text(RevisionMaterial).Right(24);
		const FUeAgentStateRevision Revision(TEXT("authoring.capture"), BaseRevision, 0);
		const FString SnapshotId = TEXT("authoring.capture:") +
								   Sha256Text(TargetKind + TEXT("|") + BaseRevision).Right(24);
		const FString EvidenceId = TEXT("independent-readback:") + SnapshotId;
		const double RevisionFinished = FPlatformTime::Seconds();
		if (bAllUnchanged && Previous->Model->GetBaseRevision() == BaseRevision)
		{
			FUeAgentResourceChargeError ResourceError;
			if (!Resources.TryChargeMany(Previous->Charges, TEXT("composition:actual_unchanged_capture"), ResourceError))
			{ OutError.SetFailure(ResourceError); return false; }
			OutSnapshot = Previous->Snapshot; OutModel = Previous->Model;
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=canonical_capture kind=%s entries=%d reused_entries=%d source_prepare_s=%.6f revision_s=%.6f canonical_s=0 compose_s=0 total_s=%.6f success=true"),
				*TargetKind, SourceEntries.Num(), SourceEntries.Num(), SourcePrepared - CaptureStarted,
				RevisionFinished - SourcePrepared, FPlatformTime::Seconds() - CaptureStarted);
			return true;
		}

		TArray<FUeAgentStateEntry> CanonicalEntries;
		CanonicalEntries.Reserve(SourceEntries.Num());
		TMap<const FJsonValue*, FObservedCanonicalValue> ObservedValues;
		for (const UeAgentAuthoringState::FStateEntry& Entry : SourceEntries)
		{
			const FString Key = CaptureEntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey);
			auto& Proof = Next->Entries.FindChecked(Key);
			const auto* Old = bSameCapture ? Previous->Entries.Find(Key) : nullptr;
			const bool bSameObservation = Old && Entry.Observation && Old->Observation == Entry.Observation;
			if (bSameObservation && Old->CanonicalIndex != INDEX_NONE)
			{
				// Reuse the already validated type/details/descriptor and captured
				// value. Capture generation is part of canonical identity, so both
				// identity and its post-create default source must still be rebound.
				const auto& Before = Previous->Snapshot->GetEntries()[Old->CanonicalIndex];
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(Old->Value.Charges, TEXT("composition:actual_unchanged_value"), ResourceError)
					|| !Resources.TryChargeMany(Old->DescriptorCharges, TEXT("composition:actual_unchanged_descriptor"), ResourceError))
				{ OutError.SetFailure(ResourceError); return false; }
				FUeAgentCanonicalIdentity Identity = Before.GetIdentity().WithGeneration(BaseRevision);
				FUeAgentDefaultValue Default(EUeAgentDefaultSourceKind::PostCreateReadback,
					Identity.GetCanonicalKey(), Before.GetDefault().GetProvenance(), Before.GetValue());
				FUeAgentStateEntry Captured(MoveTemp(Identity), Before.GetType(), Before.GetValue(),
					MoveTemp(Default), Before.GetDetails(), Revision, EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::None, Before.GetAuthoringDescriptor());
				if (!Captured.IsValid()) { OutError.SetFailure(Captured.GetValidationDiagnostic(), Entry.TargetIdentity.CanonicalKey); return false; }
				Proof.Value = Old->Value; Proof.Descriptor = Old->Descriptor; Proof.DescriptorCharges = Old->DescriptorCharges;
				if (SourceEntries.Num() <= 200000) ObservedValues.Add(Entry.Value.Get(), Proof.Value);
				CanonicalEntries.Add(MoveTemp(Captured)); ++ReusedEntries;
				continue;
			}
			FUeAgentCanonicalValuePtr Value;
			const auto ForwardCaptureFailure = [&](Contract::FDiagnostic Failure)
			{
				if (Failure.CandidateName.IsEmpty()) Failure.CandidateName = Entry.DisplayName;
				if (Failure.TargetPath.IsEmpty())
					if (const auto* AssetPath = Entry.TargetIdentity.Selectors.Find(TEXT("asset_path"))) Failure.TargetPath = *AssetPath;
				OutError.SetFailure(MoveTemp(Failure), Entry.TargetIdentity.CanonicalKey);
				return false;
			};
			const auto ValueUsageBefore = Resources.Snapshot();
			if (bSameObservation) {
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(Old->Value.Charges, TEXT("composition:actual_unchanged_value"), ResourceError))
				{ OutError.SetFailure(ResourceError); return false; }
				Value = Old->Value.Value; ++ReusedEntries;
			}
			else if (!Entry.Value.IsValid() ||
				!TryCanonicalizeJson(Entry.Value, Resources, Value, OutError) || !Value.IsValid())
			{
				if (OutError.GetCode().IsEmpty())
				{
					OutError.SetSourceFailure(TEXT("authoring_capture_value_invalid"), TEXT("The captured entry has no value to convert."), Entry.TargetIdentity.CanonicalKey);
				}
				return false;
			}
			Proof.Value.Value = Value;
			Proof.Value.Charges = CaptureChargesBetween(ValueUsageBefore, Resources.Snapshot());
			if (SourceEntries.Num() <= 200000) ObservedValues.Add(Entry.Value.Get(), Proof.Value);
			const FString TypeId =
				!Entry.Type.EnumPath.IsEmpty()			 ? Entry.Type.EnumPath
				: !Entry.Type.StructPath.IsEmpty()		 ? Entry.Type.StructPath
				: !Entry.Type.AllowedClassPath.IsEmpty() ? Entry.Type.AllowedClassPath
				: Entry.Type.Kind.IsEmpty()				 ? FString(TEXT("uai.authoring.value"))
														 : Entry.Type.Kind;
			const FString ContainerKind =
				Value->GetKind() == EUeAgentValueKind::Array	? FString(TEXT("array"))
				: Value->GetKind() == EUeAgentValueKind::Object ? FString(TEXT("object"))
																: FString(TEXT("scalar"));
			TMap<FString, FString> CanonicalSelectors =
				Entry.TargetIdentity.Selectors;
			CanonicalSelectors.Add(
				CanonicalAuthoringTargetIdentitySelector,
				Entry.TargetIdentity.CanonicalKey);
			FUeAgentCanonicalIdentity Identity(Entry.SemanticId, Entry.TargetIdentity.TargetKind,
				Entry.TargetIdentity.ModelId, MoveTemp(CanonicalSelectors),
				Entry.TargetIdentity.KeyPattern.IsEmpty() ? Entry.TargetIdentity.TargetKind
														  : Entry.TargetIdentity.KeyPattern,
				BaseRevision);
			FUeAgentCanonicalType Type(TypeId, Value->GetKind(),
				Value->GetKind() == EUeAgentValueKind::Null, ContainerKind);
			const FString Provenance = Entry.Provenance.IsEmpty()
										   ? FString(TEXT("authoring_state.capture"))
										   : Entry.Provenance;
			FUeAgentDefaultValue Default(EUeAgentDefaultSourceKind::PostCreateReadback,
				Identity.GetCanonicalKey(), Provenance, Value.ToSharedRef());
			const FString DisplayName = !Entry.DisplayName.IsEmpty() ? Entry.DisplayName
										: !Entry.SemanticId.IsEmpty()
											? Entry.SemanticId
											: Entry.TargetIdentity.TargetKind;
			FUeAgentStateDetails Details(DisplayName, Entry.Tooltip, Entry.CategoryPath,
				Entry.bEditable,
				Entry.bAvailable					 ? FString(TEXT("available"))
				: Entry.AvailabilityReason.IsEmpty() ? FString(TEXT("unavailable"))
													 : Entry.AvailabilityReason,
				Entry.Constraints);
			if (!Identity.IsValid())
			{
				return ForwardCaptureFailure(Identity.GetValidationDiagnostic());
			}
			if (!Type.IsValid())
			{
				return ForwardCaptureFailure(Type.GetValidationDiagnostic());
			}
			if (!Default.IsValid())
			{
				return ForwardCaptureFailure(Default.GetValidationDiagnostic());
			}
			if (!Details.IsValid())
			{
				return ForwardCaptureFailure(Details.GetValidationDiagnostic());
			}
			// Preserve captured metadata at the state/canonical boundary. The
			// shared export/restore/intent projection cannot reconstruct editable
			// struct membership from a bare canonical type ID.
			const auto Metadata = MakeShared<FJsonObject>();
			Metadata->SetObjectField(TEXT("type"), Entry.Type.ToJsonObject());
			if (Entry.ValueDefinition) Metadata->SetField(TEXT("value_definition"), Entry.ValueDefinition);
			if (Entry.CandidatesHeader) Metadata->SetField(TEXT("candidates_header"), Entry.CandidatesHeader);
			if (Entry.Candidates) Metadata->SetField(TEXT("candidates"), Entry.Candidates);
			FUeAgentCanonicalValuePtr AuthoringDescriptor;
			const auto DescriptorBefore = Resources.Snapshot();
			if (bSameObservation) {
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(Old->DescriptorCharges, TEXT("composition:actual_unchanged_descriptor"), ResourceError))
				{ OutError.SetFailure(ResourceError); return false; }
				AuthoringDescriptor = Old->Descriptor;
			} else if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Metadata), Resources, AuthoringDescriptor, OutError)) return false;
			Proof.Descriptor = AuthoringDescriptor;
			Proof.DescriptorCharges = CaptureChargesBetween(DescriptorBefore, Resources.Snapshot());
			FUeAgentStateEntry CanonicalEntry(MoveTemp(Identity), MoveTemp(Type),
				Value.ToSharedRef(), MoveTemp(Default), MoveTemp(Details), Revision,
				EUeAgentAuthoredPresence::Present, EUeAgentOverrideState::None, MoveTemp(AuthoringDescriptor));
			if (!CanonicalEntry.IsValid())
			{
				return ForwardCaptureFailure(CanonicalEntry.GetValidationDiagnostic());
			}
			CanonicalEntries.Add(MoveTemp(CanonicalEntry));
		}

		const double EntriesFinished = FPlatformTime::Seconds();
		FUeAgentStateSemanticError SemanticError;
		if (!FUeAgentCanonicalSnapshot::TryCreate(
			SnapshotId,
			Contract::EUeAgentStatePlane::AuthoringMemory,
			Revision,
			FUeAgentTemporalPoint(
				TEXT("authoring.capture"), TEXT("0")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TargetKind,
				EvidenceId),
			CanonicalEntries,
			Resources,
			OutSnapshot,
			SemanticError)
			|| !OutSnapshot.IsValid())
		{
			OutError.SetFailure(SemanticError.Code.IsEmpty()
					? UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_capture_snapshot_invalid"), TEXT("Canonical authoring capture did not produce a valid snapshot."))
					: SemanticError, FString(), SemanticError.Source.JsonPath);
			return false;
		}

		const double CanonicalFinished = FPlatformTime::Seconds();
		FCompositionRequest Request;
		Request.TargetKind = TargetKind;
		Request.ProfileId = ProfileId.IsEmpty()
			? TEXT("llm_authoring") : ProfileId;
		Request.BaseRevision = BaseRevision;
		Request.SourceProvenance = TEXT("authoring_state.capture");
		Request.Entries = MoveTemp(SourceEntries);
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!RegisterGraphTreeTargetCompositionContracts(ContractError)
			|| !RegisterOtherTargetCompositionContracts(ContractError))
		{
			OutError.SetFailure(ContractError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_composition_contract_registration_failed"), TEXT("Target composition contract registration failed without returning a diagnostic.")) : ContractError, FString());
			return false;
		}
		const double CompositionPrepared = FPlatformTime::Seconds();
		const TOptional<bool> IncrementalComposition = bSameCapture
			? TryComposeValueOnlyCapture(Request, CanonicalEntries, *Previous, *Next, Resources, OutModel, OutError)
			: TOptional<bool>();
		const bool bComposed = IncrementalComposition.IsSet() ? IncrementalComposition.GetValue() : TryComposeAuthoringModelFromCapture(
			Request,
			Resources,
			OutModel,
			OutError, &ObservedValues, &Next->CompositionPreparationCharges, &Next->ProjectedValueCharges);
		const double CompositionFinished = FPlatformTime::Seconds();
		if (bComposed && Request.Entries.Num() <= 200000)
		{
			Next->Snapshot = OutSnapshot; Next->Model = OutModel;
			Next->Charges = CaptureChargesBetween(CaptureUsageBefore, Resources.Snapshot());
			const auto& Captured = OutSnapshot->GetEntries();
			for (int32 Index = 0; Index < Captured.Num(); ++Index) {
				const auto& Identity = Captured[Index].GetIdentity();
				if (auto* Proof = Next->Entries.Find(CaptureEntryKey(Identity.GetSemanticId(),
					Identity.GetSelector(CanonicalAuthoringTargetIdentitySelector)))) Proof->CanonicalIndex = Index;
			}
			PreviousCanonicalCapture = MoveTemp(Next);
		}
		const double CaptureFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=canonical_capture kind=%s entries=%d reused_entries=%d source_prepare_s=%.6f revision_s=%.6f canonical_s=%.6f entry_s=%.6f snapshot_s=%.6f compose_s=%.6f compose_prepare_s=%.6f compose_model_s=%.6f compose_finalize_s=%.6f total_s=%.6f success=%s"),
			*TargetKind, Request.Entries.Num(), ReusedEntries, SourcePrepared - CaptureStarted,
			RevisionFinished - SourcePrepared, CanonicalFinished - RevisionFinished,
			EntriesFinished - RevisionFinished, CanonicalFinished - EntriesFinished,
			CaptureFinished - CanonicalFinished, CompositionPrepared - CanonicalFinished,
			CompositionFinished - CompositionPrepared, CaptureFinished - CompositionFinished, CaptureFinished - CaptureStarted,
			bComposed ? TEXT("true") : TEXT("false"));
		return bComposed;
	}

	bool TryBuildCompositionExportPlan(
		FAuthoringModelRef Model,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		const FUeAgentStateRevision Revision(
			TEXT("authoring.composition"),
			Model->GetBaseRevision(),
			0);
		const FString EvidenceId =
			TEXT("composition:") + Model->GetSemanticFingerprint();
		return TryBuildCompositionExportPlan(
			Model,
			{
				EvidenceId,
				Revision,
				EvidenceId,
			},
			MoveTemp(Telemetry),
			Resources,
			OutPlan,
			OutError);
	}

	namespace
	{
		// Retain only the last successful composition. Every reuse is justified by
		// the newly captured complete model and its freshly planned projection;
		// this is never a substitute for native capture or revision validation.
		struct FCompositionExportBaseline
		{
			FString ContractId;
			int32 ContractVersion = 0;
			FString ProfileId;
			TArray<FString> TargetUnitIds;
			int32 LayoutVersion = 0;
			TMap<FString, FString> LogicalPlacements;
			TMap<FString, FString> PhysicalPlacements;
			TMap<FString, TArray<FString>> PhysicalFiles;
			TMap<FString, FString> PublicRefMapping;
			TMap<FString, FString> UnitContentKeys;
			TMap<FString, FString> ItemValueInvariantKeys;
			TArray<FExportFilePlan> Files;
			FString TargetKind;
			TMap<FString, TSharedRef<FJsonObject>> PackedDocuments;
			TMap<FString, FUeAgentCanonicalValueRef> PackedExamples;
			TMap<FString, FUeAgentCanonicalValueRef> Inventory;
			TMap<FString, FUeAgentCanonicalValueRef> ManifestInventory;
			TMap<FString, FString> UnitPointers;
			TMap<FString, FString> UnitEditHashes;
			TMap<FString, TArray<FString>> UnitContentFiles;
		};
		thread_local TSharedPtr<const FCompositionExportBaseline> PreviousCompositionExport;
		bool ExportDataIdentical(const FString& A, const FString& B)
		{
			return A.Equals(B, ESearchCase::CaseSensitive);
		}
		template <typename ItemType>
		bool ExportDataIdentical(const TArray<ItemType>& A, const TArray<ItemType>& B)
		{
			if (A.Num() != B.Num()) return false;
			for (int32 I = 0; I < A.Num(); ++I)
				if (!ExportDataIdentical(A[I], B[I])) return false;
			return true;
		}

		template <typename ValueType>
		bool ExportMapsIdentical(const TMap<FString, ValueType>& A, const TMap<FString, ValueType>& B)
		{
			if (A.Num() != B.Num()) return false;
			TArray<FString> LeftKeys, RightKeys;
			A.GetKeys(LeftKeys); B.GetKeys(RightKeys);
			LeftKeys.Sort(); RightKeys.Sort();
			if (!ExportDataIdentical(LeftKeys, RightKeys)) return false;
			for (const auto& Pair : A)
			{
				const auto* Other = B.Find(Pair.Key);
				if (!Other || !ExportDataIdentical(Pair.Value, *Other)) return false;
			}
			return true;
		}

		bool IsGraphStructureUnit(const FSemanticUnit& Unit)
		{
			return Unit.GetUnitKind() == ESemanticUnitKind::StructureState
				&& IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef(),
					Unit.GetValues().Contains(TEXT("nodes")) && Unit.GetValues().Contains(TEXT("edges")));
		}

		FString KeyedItemRootField(const FSemanticUnit& Unit)
		{
			if (Unit.GetUnitKind() == ESemanticUnitKind::StructureState)
				for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
					if (Unit.GetValues().Contains(Contract.RootField)) return Contract.RootField;
			return FString();
		}

		bool IsKeyedItemStructureUnit(const FSemanticUnit& Unit)
		{
			// Graph metadata may contain names such as interfaces. Those fields
			// belong to the graph document, not a standalone keyed collection.
			return !IsGraphStructureUnit(Unit) && !KeyedItemRootField(Unit).IsEmpty();
		}

		bool IsActorComponentHierarchyItemUnit(const FSemanticUnit& Unit)
		{
			return Unit.GetUnitKind() == ESemanticUnitKind::ItemState
				&& Unit.GetRole() == TEXT("actor_component_hierarchy")
				&& (Unit.GetSemanticTypeRef().EndsWith(TEXT(".actor.target"))
					|| Unit.GetSemanticTypeRef().EndsWith(TEXT(".component.target")));
		}

	}

	bool TryBuildCompositionExportPlan(
		FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& SnapshotBinding,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError, const FString& TargetKind)
	{
		const double PlanStarted = FPlatformTime::Seconds();
		double CompactValidationSeconds = 0.0;
		int32 CompactValidationCount = 0;
		OutPlan.Reset();
		OutError.Reset();
		if (!SnapshotBinding.IsValid())
		{
			OutError.SetSourceFailure(TEXT("composition_snapshot_binding_invalid"), SnapshotBinding.SnapshotId.TrimStartAndEnd().IsEmpty() ? TEXT("The captured snapshot binding has no snapshot identifier.") : !SnapshotBinding.Revision.IsValid() ? TEXT("The captured snapshot binding has an invalid revision.") : TEXT("The captured snapshot binding has no readback evidence identifier."));
			return false;
		}
		FWorkspaceLayoutPlanPtr Layout;
		if (!TryPlanWorkspaceLayout(
			Model,
			2,
			EWorkspaceSourceMode::Directory,
			MoveTemp(Telemetry),
			Resources,
			Layout,
			OutError)
			|| !Layout.IsValid())
		{
			return false;
		}

		const double LayoutFinished = FPlatformTime::Seconds();
		const TSharedPtr<const FCompositionExportBaseline> Baseline = PreviousCompositionExport;
		bool bReusableProjection = Baseline.IsValid() && ExportDataIdentical(Baseline->TargetKind, TargetKind)
			&& ExportDataIdentical(Baseline->ContractId, Model->GetContractId())
			&& Baseline->ContractVersion == Model->GetContractVersion()
			&& ExportDataIdentical(Baseline->ProfileId, Model->GetProfileId())
			&& ExportDataIdentical(Baseline->TargetUnitIds, Model->GetTargetUnitIds())
			&& Baseline->UnitContentKeys.Num() == Model->GetUnits().Num()
			&& Baseline->LayoutVersion == Layout->GetLayoutVersion()
			&& ExportMapsIdentical(Baseline->LogicalPlacements, Layout->GetLogicalPlacements())
			&& ExportMapsIdentical(Baseline->PhysicalPlacements, Layout->GetPhysicalPlacements())
			&& ExportMapsIdentical(Baseline->PhysicalFiles, Layout->GetPhysicalFiles())
			&& ExportMapsIdentical(Baseline->PublicRefMapping, Layout->GetMachineManifest().PublicRefMapping);
		TMap<FString, FString> UnitContentKeys;
		TMap<FString, FString> ItemValueInvariantKeys;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			FString Key;
			if (!TryHashSemanticUnitContent(Unit, Key, OutError)) return false;
			// Semantic hashing deliberately excludes presentation provenance and
			// category. Include both in the export proof, with unambiguous framing.
			Key += FString::Printf(TEXT("|%d:"), Unit.GetSourceProvenance().Len()) + Unit.GetSourceProvenance();
			for (const FString& Category : Unit.GetCategoryPath())
				Key += FString::Printf(TEXT("|%d:"), Category.Len()) + Category;
			UnitContentKeys.Add(Unit.GetUnitId(), Sha256Text(Key));
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState && Unit.GetValues().Contains(TEXT("value")))
			{
				// A standalone value/override can change without changing the item's
				// identity, definition, owner, or any other projected state field.
				auto Invariants = Unit.GetValues();
				Invariants.Remove(TEXT("value"));
				Invariants.Remove(TEXT("authored_override"));
				FString InvariantKey;
				if (!TryHashSemanticUnitContent(Unit.WithValues(MoveTemp(Invariants)), InvariantKey, OutError)) return false;
				InvariantKey += FString::Printf(TEXT("|%d:"), Unit.GetSourceProvenance().Len()) + Unit.GetSourceProvenance();
				for (const FString& Category : Unit.GetCategoryPath())
					InvariantKey += FString::Printf(TEXT("|%d:"), Category.Len()) + Category;
				ItemValueInvariantKeys.Add(Unit.GetUnitId(), Sha256Text(InvariantKey));
			}
		}
		TSet<FString> IdenticalUnits;
		int32 ChangedPropertyUnits = 0;
		int32 ChangedValueItems = 0;
		int32 ChangedOtherUnits = 0;
		if (bReusableProjection)
		{
			for (const FSemanticUnit& Unit : Model->GetUnits())
			{
				const FString* Previous = Baseline->UnitContentKeys.Find(Unit.GetUnitId());
				if (Previous && ExportDataIdentical(UnitContentKeys.FindChecked(Unit.GetUnitId()), *Previous)) IdenticalUnits.Add(Unit.GetUnitId());
				else if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet) ++ChangedPropertyUnits;
				else if (const FString* Invariant = ItemValueInvariantKeys.Find(Unit.GetUnitId()); Invariant
					&& Baseline->ItemValueInvariantKeys.Contains(Unit.GetUnitId())
					&& ExportDataIdentical(*Invariant, Baseline->ItemValueInvariantKeys.FindChecked(Unit.GetUnitId()))) ++ChangedValueItems;
				else ++ChangedOtherUnits;
			}
			// Only value changes with unchanged item invariants and the exact
			// complete projection qualify. Structure and unknown shapes regenerate.
			bReusableProjection = ChangedOtherUnits == 0;
		}
		TMap<FString, const FExportFilePlan*> BaselineFiles;
		if (bReusableProjection)
			for (const FExportFilePlan& File : Baseline->Files) BaselineFiles.Add(File.GetRelativePath(), &File);
		const double ComparisonFinished = FPlatformTime::Seconds();
		int32 ReusedDocuments = 0;
		int32 ReusedInventoryRows = 0;
		TArray<FExportFilePlan> Files;
		FAuthoringExampleCatalog Examples;
		RegisterDomainAuthoringExampleRules(Examples);
		TMap<FString, FUeAgentCanonicalValueRef> InventoryByUnit;
		TMap<FString, FString> UnitPointers;
		TMap<FString, FString> UnitEditHashes;
		TMap<FString, TArray<FString>> UnitContentFiles;
		TSet<FString> ContentPaths;
		TMap<FString, FString> ContentDirectories;
		if (!TryBuildAuthoringContentDirectories(*Model, Layout->GetPhysicalPlacements(), ContentDirectories, OutError)) return false;
		auto OrganizeContent = [&](const FString& UnitId, const FString& DocumentPath,
			FUeAgentCanonicalValuePtr& Row) -> bool
		{
			if (DocumentPath.StartsWith(TEXT(".uai/"))) return true;
			const FSemanticUnit* Unit = Model->FindUnit(UnitId);
			if (!Unit) return true;
			const FString Base = ContentDirectories.FindChecked(UnitId);
			return TryOrganizeAuthoringContent(Unit->GetSemanticTypeRef(), Unit->GetRole(), Row,
				[&](const FUeAgentCanonicalValuePtr& Value, const FString& RelativeStem,
					FUeAgentCanonicalValuePtr& OutReference) -> bool
				{
					const FString Stem = Base + TEXT("/") + RelativeStem;
					FString Path = AllocateReadableName(Stem, [&](const FString& Candidate) {
						return ContentPaths.Contains((Candidate + TEXT(".hjson")).ToLower());
					}) + TEXT(".hjson");
					FUeAgentCanonicalValuePtr Content;
					FUeAgentStateSemanticError Error;
					if (!FUeAgentCanonicalValue::TryObject({{TEXT("value"), Value.ToSharedRef()}}, Resources, Content, Error))
					{ OutError.SetFailure(Error, Path); return false; }
					Examples.AddDocument(Path, Content.ToSharedRef(), Unit->GetSemanticTypeRef(), Unit->GetRole(), RelativeStem);
					TArray<uint8> Bytes;
					if (!TrySerializeSemanticHjsonObject(*Content, Bytes, OutError, Path))
					{
						 return false;
					}
					const FString ContentHash = Sha256(Bytes);
					Files.Emplace(Path, MoveTemp(Bytes), ContentHash);
					ContentPaths.Add(Path.ToLower());
					UnitContentFiles.FindOrAdd(UnitId).Add(Path);
					FUeAgentCanonicalValuePtr ReferencePath;
					if (!FUeAgentCanonicalValue::TryString(Path, Resources, ReferencePath, Error)
						|| !FUeAgentCanonicalValue::TryObject({{TEXT("content_ref"), ReferencePath.ToSharedRef()}}, Resources, OutReference, Error))
					{ OutError.SetFailure(Error, Path); return false; }
					return true;
				}, Resources, OutError);
		};
		TMap<FString, TSharedRef<FJsonObject>> PackedDocuments;
		TMap<FString, FUeAgentCanonicalValueRef> PackedExamples;
		TSet<FString> ReusedPackedDocuments;
		TMap<FString, FString> PropertyPathsByItemRef;
		for (const FSemanticUnit& Property : Model->GetUnits())
		{
			if (Property.GetUnitKind() != ESemanticUnitKind::PropertySet) continue;
			// Layout-slot properties belong to the widget's placement, not its body.
			if (Property.GetSemanticTypeRef() == TEXT("uai.semantic.umg.slot.property")) continue;
			const FSemanticUnit* Owner = Model->FindUnit(Property.GetOwnerRef());
			const FString* Path = Layout->GetPhysicalPlacements().Find(Property.GetUnitId());
			if (!Owner || Owner->GetUnitKind() != ESemanticUnitKind::ItemState
				|| !Path || !Path->EndsWith(TEXT("/properties.hjson"))) continue;
			PropertyPathsByItemRef.Add(Owner->GetUnitId(), *Path);
			if (const FString* PublicRef = Layout->GetMachineManifest().PublicRefMapping.Find(Owner->GetUnitId()))
				PropertyPathsByItemRef.Add(*PublicRef, *Path);
		}
		for (const TPair<FString, TArray<FString>>& PhysicalFile
			: Layout->GetPhysicalFiles())
		{
			if (PhysicalFile.Key == ManifestPath
				|| PhysicalFile.Value.IsEmpty())
			{
				continue;
			}
			const bool bPackedPropertyContainer =
				!PhysicalFile.Value.IsEmpty()
				&& Model->FindUnit(PhysicalFile.Value[0]) != nullptr
				&& Model->FindUnit(PhysicalFile.Value[0])->GetUnitKind()
					== ESemanticUnitKind::PropertySet;
			const bool bPackedDefinitionContainer =
				!PhysicalFile.Value.IsEmpty()
				&& Model->FindUnit(PhysicalFile.Value[0]) != nullptr
				&& Model->FindUnit(PhysicalFile.Value[0])->GetUnitKind()
					== ESemanticUnitKind::CollectionState
				&& Model->FindUnit(PhysicalFile.Value[0])->GetRole()
					== TEXT("property_definition");
			const bool bGraphStructureDocument =
				PhysicalFile.Value.Num() == 1
				&& Model->FindUnit(PhysicalFile.Value[0]) != nullptr
				&& IsGraphStructureUnit(*Model->FindUnit(PhysicalFile.Value[0]));
			if (bReusableProjection && (bPackedPropertyContainer || bPackedDefinitionContainer)
				&& Baseline->PackedDocuments.Contains(PhysicalFile.Key) && BaselineFiles.Contains(PhysicalFile.Key))
			{
				bool bAllMembersIdentical = true;
				for (const FString& UnitId : PhysicalFile.Value)
				{
					// External content allocation also contributes examples and can
					// depend on preceding path collisions. Keep that path authoritative.
					if (!IdenticalUnits.Contains(UnitId) || Baseline->UnitContentFiles.Contains(UnitId))
					{ bAllMembersIdentical = false; break; }
				}
				if (bAllMembersIdentical)
				{
					PackedDocuments.Add(PhysicalFile.Key, Baseline->PackedDocuments.FindChecked(PhysicalFile.Key));
					ReusedPackedDocuments.Add(PhysicalFile.Key);
					for (const FString& UnitId : PhysicalFile.Value)
					{
						UnitPointers.Add(UnitId, Baseline->UnitPointers.FindChecked(UnitId));
						UnitEditHashes.Add(UnitId, Baseline->UnitEditHashes.FindChecked(UnitId));
					}
					++ReusedDocuments;
					continue;
				}
			}
			const bool bKeyedItemDocument =
				PhysicalFile.Value.Num() == 1
				&& Model->FindUnit(PhysicalFile.Value[0]) != nullptr
				&& IsKeyedItemStructureUnit(
					*Model->FindUnit(PhysicalFile.Value[0]));
			if (bKeyedItemDocument)
			{
				const FString& UnitId = PhysicalFile.Value[0];
				const FSemanticUnit* Unit = Model->FindUnit(UnitId);
				check(Unit != nullptr);
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
				TSet<FString> ProjectedRoots;
				FString SemanticId, DocumentPath;
				Unit->GetSemanticTypeRef().Split(TEXT("#"), &SemanticId, &DocumentPath);
				const auto* DocumentContract = FindLevelSequenceCollectionContract(TargetKind, SemanticId, Unit->GetRole(), DocumentPath);
				for (const auto& Contract : GetKeyedItemProjectionContracts())
				{
					const auto* CapturedItems = Unit->GetValues().Find(Contract.RootField);
					if (!CapturedItems) continue;
					FUeAgentCanonicalValuePtr VisibleItems;
					const auto& ProjectionContract = DocumentContract && DocumentContract->RootField == Contract.RootField ? *DocumentContract : Contract;
					if (!TryProjectKeyedItemVisibleValue(UnitId, CapturedItems->Get(), ProjectionContract, Resources, VisibleItems, OutError,
						DocumentContract ? EKeyedItemProjectionInputKind::ProjectedMap : EKeyedItemProjectionInputKind::CapturedItems))
					{
						if (OutError.GetCode().IsEmpty()) OutError.SetSourceFailure(TEXT("composition_keyed_item_projection_invalid"), FString::Printf(TEXT("The captured collection '%s' could not be projected to editable records."), *Contract.RootField), UnitId);
						return false;
					}
					ProjectedRoots.Add(Contract.RootField);
					// Folded items retain their schema navigation without putting metadata
					// in captured business values or downstream operation payloads.
					if (!ProjectionContract.AggregatedItemSemanticSuffix.IsEmpty())
					{
						TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
						FUeAgentStateSemanticError Error;
						TMap<FString, const FUeAgentCanonicalValue*> CapturedByKey;
						for (const auto& CapturedItem : (*CapturedItems)->GetObjectItems())
							CapturedByKey.Add(CapturedItem.Key, &CapturedItem.Value.Get());
						for (const auto& Item : VisibleItems->GetObjectItems())
						{
							TArray<TPair<FString, FUeAgentCanonicalValueRef>> RowFields = Item.Value->GetObjectItems();
							if (const auto* Captured = CapturedByKey.Find(Item.Key))
							{
								for (const auto& Field : (*Captured)->GetObjectItems())
								{
									if (Field.Key != TEXT("unit_id") || Field.Value->GetKind() != EUeAgentValueKind::String) continue;
									const FSemanticUnit* Bound = Model->FindUnit(Field.Value->GetText());
									const FString* Ref = Bound ? Layout->GetMachineManifest().PublicRefMapping.Find(Bound->GetFieldSetRef()) : nullptr;
									if (!Ref) continue;
									FUeAgentCanonicalValuePtr Definition;
									if (!FUeAgentCanonicalValue::TryString(*Ref, Resources, Definition, Error))
									{ OutError.SetFailure(Error, UnitId); return false; }
									RowFields.Emplace(TEXT("definition"), Definition.ToSharedRef());
								}
							}
							FUeAgentCanonicalValuePtr Row;
							if (!FUeAgentCanonicalValue::TryObject(RowFields, Resources, Row, Error))
							{ OutError.SetFailure(Error, UnitId); return false; }
							Rows.Emplace(Item.Key, Row.ToSharedRef());
						}
						if (!FUeAgentCanonicalValue::TryObject(Rows, Resources, VisibleItems, Error))
						{ OutError.SetFailure(Error, UnitId); return false; }
					}
					Fields.Emplace(Contract.RootField, VisibleItems.ToSharedRef());
				}
				for (const auto& Field : Unit->GetValues())
				{
					if (ProjectedRoots.Contains(Field.Key)) continue;
					for (const auto& Contract : GetKeyedItemProjectionContracts())
						if (ProjectedRoots.Contains(Contract.RootField)
							&& (Contract.IsReadOnlyRootField(Field.Key) || Contract.IsEditableRootField(Field.Key)
								|| (!Contract.RecordSchemaField.IsEmpty() && Field.Key.Equals(Contract.RecordSchemaField, ESearchCase::CaseSensitive))))
						{
							Fields.Add(Field);
							break;
						}
				}
				FUeAgentCanonicalValuePtr VisibleRow;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, VisibleRow, StateError))
				{
					OutError.SetFailure(StateError, UnitId); return false;
				}
				TArray<uint8> FileBytes;
				if (!TrySerializeSemanticHjsonObject(*VisibleRow, FileBytes, OutError, PhysicalFile.Key))
				{
					 return false;
				}
				UnitEditHashes.Add(UnitId, Sha256(FileBytes));
				if (!OrganizeContent(UnitId, PhysicalFile.Key, VisibleRow)) return false;
                if (!TrySerializeSemanticHjsonObject(*VisibleRow, FileBytes, OutError, PhysicalFile.Key))
                {  return false; }
				Examples.AddDocument(PhysicalFile.Key, VisibleRow.ToSharedRef(), Unit->GetSemanticTypeRef(), Unit->GetRole());
				for (const auto& Contract : GetKeyedItemProjectionContracts())
					if (ProjectedRoots.Contains(Contract.RootField))
						Examples.AddCollection(PhysicalFile.Key, VisibleRow.ToSharedRef(),
							DocumentContract && DocumentContract->RootField == Contract.RootField ? *DocumentContract : Contract);
				Files.Emplace(PhysicalFile.Key, FileBytes, Sha256(FileBytes));
				UnitPointers.Add(UnitId, TEXT("/"));
			}
			else if (PhysicalFile.Value.Num() == 1
				&& Model->FindUnit(PhysicalFile.Value[0]) != nullptr
				&& IsActorComponentHierarchyItemUnit(
					*Model->FindUnit(PhysicalFile.Value[0])))
			{
				const FString& UnitId = PhysicalFile.Value[0];
				const FSemanticUnit* Unit = Model->FindUnit(UnitId);
				check(Unit != nullptr);
				TSharedPtr<FJsonObject> CompactRow;
				const TSharedPtr<FJsonObject>* CapturedValues = nullptr;
				TSharedRef<FJsonObject> VisibleRow = MakeShared<FJsonObject>();
				if (!TryBuildCompactRow(*Unit, CompactRow, OutError, nullptr, PhysicalFile.Key)
					|| !CompactRow.IsValid()
					|| !CompactRow->TryGetObjectField(TEXT("values"), CapturedValues)
					|| CapturedValues == nullptr || !CapturedValues->IsValid()
					|| !TryProjectItemVisibleObject(UnitId,
						(*CapturedValues).ToSharedRef(),
						GetActorComponentHierarchyItemVisibleFieldPolicy(),
						VisibleRow, OutError))
				{
					if (OutError.GetCode().IsEmpty())
					{
						OutError.SetSourceFailure(TEXT("composition_item_projection_invalid"), TEXT("The item projection did not provide the required values object."), UnitId, TEXT("/values"));
					}
					return false;
				}
				TArray<uint8> FileBytes;
				if (!TrySerializeSemanticHjsonObject(VisibleRow, FileBytes, OutError, PhysicalFile.Key))
				{
					
					return false;
				}
				Files.Emplace(PhysicalFile.Key, FileBytes, Sha256(FileBytes));
				UnitPointers.Add(UnitId, TEXT("/"));
				UnitEditHashes.Add(UnitId, Sha256(FileBytes));
			}
			else if (bGraphStructureDocument)
			{
				const FString& UnitId = PhysicalFile.Value[0];
				const FSemanticUnit* Unit = Model->FindUnit(UnitId);
				check(Unit != nullptr);
				TSharedPtr<FJsonObject> CompactRow;
				TSharedRef<FJsonObject> VisibleRow = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> InterfaceRow;
				if (!TryBuildCompactRow(*Unit, CompactRow, OutError, &Layout->GetMachineManifest().PublicRefMapping, PhysicalFile.Key)
					|| !CompactRow.IsValid()
					|| !TryProjectGraphAuthoringVisibleRow(
						UnitId, CompactRow.ToSharedRef(), VisibleRow,
						InterfaceRow, OutError))
				{
					if (OutError.GetCode().IsEmpty())
					{
						OutError.SetSourceFailure(TEXT("composition_graph_projection_invalid"),
							TEXT("The graph could not be projected into its authoring document."), UnitId);
					}
					return false;
				}
				const TSharedPtr<FJsonObject>* VisibleNodes = nullptr;
				if (VisibleRow->TryGetObjectField(TEXT("nodes"), VisibleNodes))
				{
					FGraphAuthoringAliasProjection Aliases;
					if (!TryBuildGraphModelNodeAliases(*Model, UnitId, Aliases, OutError)) return false;
					for (const auto& Alias : Aliases.AliasBySourceId)
					{
						const TSharedPtr<FJsonObject>* VisibleNode = nullptr;
						if (!(*VisibleNodes)->TryGetObjectField(Alias.Value, VisibleNode)) continue;
						if (const FString* PropertyPath = PropertyPathsByItemRef.Find(Alias.Key))
						{
							FString RelativePath = *PropertyPath;
							const FString GraphDirectory = FPaths::GetPath(PhysicalFile.Key) + TEXT("/");
							FPaths::MakePathRelativeTo(RelativePath, *GraphDirectory);
							(*VisibleNode)->SetStringField(TEXT("properties_ref"), RelativePath);
						}
					}
				}
				if (InterfaceRow.IsValid())
				{
					const FString InterfaceFileName =
						FPaths::GetBaseFilename(PhysicalFile.Key)
						+ TEXT(".interfaces.hjson");
					const FString InterfacePath = FPaths::Combine(
						FPaths::GetPath(PhysicalFile.Key), InterfaceFileName);
					VisibleRow->SetStringField(
						TEXT("interface_ref"), InterfaceFileName);
					TArray<uint8> InterfaceBytes;
					if (!TrySerializeSemanticHjsonObject(
						InterfaceRow.ToSharedRef(), InterfaceBytes, OutError, InterfacePath))
					{
						
						return false;
					}
					Files.Emplace(
						InterfacePath, InterfaceBytes, Sha256(InterfaceBytes));
				}
				TArray<uint8> FileBytes;
				if (!TrySerializeSemanticHjsonObject(VisibleRow, FileBytes, OutError, PhysicalFile.Key))
				{
					
					return false;
				}
				FUeAgentCanonicalValuePtr ExampleDocument;
				if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(VisibleRow), Resources, ExampleDocument, OutError)) return false;
				Examples.AddDocument(PhysicalFile.Key, ExampleDocument.ToSharedRef(), Unit->GetSemanticTypeRef(), Unit->GetRole());
				Files.Emplace(PhysicalFile.Key, FileBytes, Sha256(FileBytes));
				UnitPointers.Add(UnitId, TEXT("/"));
				UnitEditHashes.Add(UnitId, Sha256(FileBytes));
			}
			else if (bPackedPropertyContainer || bPackedDefinitionContainer)
			{
				TSharedRef<FJsonObject> Container =
					MakeShared<FJsonObject>();
				TSharedRef<FJsonObject> KeyedRows = MakeShared<FJsonObject>();
				TMap<FString, TSharedRef<FJsonObject>> CategoryRows;
				TSet<FString> PropertyRowKeys;
				TSharedRef<FJsonObject> EnumCatalog = MakeShared<FJsonObject>();
				TMap<FString, TSet<FString>> EnumCandidates;
				for (int32 Index = 0; Index < PhysicalFile.Value.Num(); ++Index)
				{
					const FString& UnitId = PhysicalFile.Value[Index];
					const FSemanticUnit* Unit = Model->FindUnit(UnitId);
					if (Unit == nullptr)
					{
						OutError.SetSourceFailure(TEXT("composition_unit_missing"), TEXT("A unit assigned to this exported document is absent from the authoring model."), UnitId);
						return false;
					}
					TSharedPtr<FJsonObject> CompactRow;
					if (!TryBuildCompactRow(*Unit, CompactRow, OutError,
						bPackedDefinitionContainer ? &Layout->GetMachineManifest().PublicRefMapping : nullptr, PhysicalFile.Key)
						|| !CompactRow.IsValid())
					{
						return false;
					}
					{
						// Keep the original formatter checks, including render failures.
						// This row is not emitted, and its edit hash was overwritten below.
						// Avoid the unused UTF-8 copy and SHA-256 without weakening validation.
						const double ValidationStarted = FPlatformTime::Seconds();
						FString ValidationText;
						Contract::FDiagnostic ValidationError;
						OutError.Reset();
						if (!FUeAgentStructuredHjsonFormatter::TryWriteValue(
							MakeShared<FJsonValueObject>(CompactRow.ToSharedRef()), ValidationText, ValidationError))
						{
							OutError.SetFailure(MoveTemp(ValidationError), PhysicalFile.Key);
							return false;
						}
						CompactValidationSeconds += FPlatformTime::Seconds() - ValidationStarted;
						++CompactValidationCount;
					}
					FString RowKey;
					TSharedRef<FJsonObject> VisibleRow = MakeShared<FJsonObject>();
					if (bPackedPropertyContainer)
					{
						const auto Address = DescribeReadableProperty(*Unit, *Model);
						RowKey = Address.Key;
						const FString CategoryKey = Address.Category;
						CategoryRows.FindOrAdd(
							CategoryKey,
							MakeShared<FJsonObject>());
						if (RowKey.IsEmpty() || PropertyRowKeys.Contains(RowKey))
						{
							UE_LOG(LogTemp, Warning, TEXT("UAI property composition conflict: file=%s property=%s category=%s semantic=%s owner=%s target=%s"),
								*PhysicalFile.Key, *RowKey, *CategoryKey, *Unit->GetSemanticTypeRef(), *Unit->GetOwnerRef(), *Unit->GetTargetIdentityKey());
							OutError.SetSourceFailure(TEXT("composition_property_key_invalid"), RowKey.IsEmpty() ? TEXT("The exported property has no readable name.") : FString::Printf(TEXT("More than one property would be exported under the name '%s'."), *RowKey), PhysicalFile.Key,
								TEXT("/categories/") + CategoryKey + TEXT("/") + RowKey);
							return false;
						}
						PropertyRowKeys.Add(RowKey);
						VisibleRow->SetStringField(
							TEXT("type"), Address.Type);
						if (!Address.Definition.IsEmpty()) VisibleRow->SetStringField(TEXT("definition"), Address.Definition);
						UE::UAI::Contract::FDiagnostic ProjectionError;
                        TSharedPtr<FJsonValue> DirectValue = DirectPropertyValue(CompactRow, *Unit, *Model, ProjectionError);
						if (!DirectValue.IsValid())
						{
							OutError.SetFailure(ProjectionError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("composition_property_value_invalid"), TEXT("The property could not be projected.")) : ProjectionError, UnitId);
							return false;
						}
						VisibleRow->SetField(TEXT("value"), MoveTemp(DirectValue));
					}
					else
					{
						const FString TypeName = ReadablePropertyType(*Unit, Model.Get());
						RowKey = ReadablePropertyDefinition(*Unit, Model.Get());
						if (RowKey.IsEmpty()) RowKey = TEXT("definition");
						const FString BaseKey = RowKey;
						RowKey = AllocateReadableName(BaseKey, [&](const FString& Candidate) {
							return KeyedRows->HasField(Candidate);
						});
						const TSharedPtr<FJsonObject>* Values = nullptr;
						if (CompactRow->TryGetObjectField(TEXT("values"), Values)
							&& Values != nullptr && Values->IsValid())
						{
							for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
								: (*Values)->Values)
							{
								if (Field.Key != TEXT("candidates")
									&& Field.Key != TEXT("candidates_header"))
								{
									if (Field.Key == TEXT("value_definition"))
									{
										if (auto PublicDefinition = ProjectPublicOwnedItemDefinition(Field.Value))
										{
											if (const auto* Captured = Unit->GetValues().Find(TEXT("value_definition")))
												{
                                                UE::UAI::Contract::FDiagnostic TemplateError;
                                                if (!ProjectElementTemplates(PublicDefinition, Captured->Get(), Model.Get(), TemplateError))
                                                { OutError.SetFailure(TemplateError, UnitId); return false; }
                                            }
											VisibleRow->SetField(Field.Key, PublicDefinition);
										}
									}
									else VisibleRow->SetField(Field.Key, Field.Value);
								}
							}
							// Native field definitions are referenced by other definitions too.
							// Their binding must be resolvable from the ordinary catalog itself.
							VisibleRow->SetStringField(TEXT("ref"), Layout->GetMachineManifest().PublicRefMapping.FindChecked(UnitId));
							const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
							if (!TypeName.IsEmpty()
								&& (*Values)->TryGetArrayField(TEXT("candidates"), Candidates)
								&& Candidates != nullptr)
							{
								for (const TSharedPtr<FJsonValue>& Candidate : *Candidates)
								{
									FString Name;
									if (Candidate.IsValid() && Candidate->Type == EJson::Object)
									{
										const TSharedPtr<FJsonObject> CandidateObject = Candidate->AsObject();
										if (CandidateObject.IsValid())
										{
											CandidateObject->TryGetStringField(TEXT("name"), Name);
										}
									}
									else if (Candidate.IsValid() && Candidate->Type == EJson::Array)
									{
										const TArray<TSharedPtr<FJsonValue>>& Cells = Candidate->AsArray();
										if (!Cells.IsEmpty() && Cells[0].IsValid()
											&& Cells[0]->Type == EJson::String)
										{
											Name = Cells[0]->AsString();
										}
									}
									else if (Candidate.IsValid() && Candidate->Type == EJson::String)
									{
										Name = Candidate->AsString();
									}
									Name.TrimStartAndEndInline();
									if (!Name.IsEmpty())
									{
									EnumCandidates.FindOrAdd(RowKey).Add(Name);
									}
								}
							}
						}
					}
					TArray<uint8> VisibleBytes;
					if (!TrySerializeSemanticHjsonObject(VisibleRow, VisibleBytes, OutError, PhysicalFile.Key))
					{
						
						return false;
					}
					UnitEditHashes.Add(UnitId, Sha256(VisibleBytes));
					if (bPackedPropertyContainer)
					{
						FUeAgentCanonicalValuePtr Content;
						if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(VisibleRow), Resources, Content, OutError)
							|| !OrganizeContent(UnitId, PhysicalFile.Key, Content)) return false;
						TSharedPtr<FJsonValue> Json;
						UE::UAI::Contract::FDiagnostic Error;
						if (!CanonicalJson::TryToJsonValue(*Content, Json, Error))
						{ OutError.SetFailure(Error, UnitId); return false; }
						VisibleRow->Values = Json->AsObject()->Values;
					}
					if (bPackedPropertyContainer)
					{
						CategoryRows.FindChecked(
							ReadablePropertyCategory(Unit->GetCategoryPath()))
							->SetObjectField(RowKey, VisibleRow);
					}
					else
					{
						KeyedRows->SetObjectField(RowKey, VisibleRow);
					}
					UnitPointers.Add(
						UnitId,
						bPackedPropertyContainer
							? TEXT("/categories/")
								+ EscapeJsonPointerSegment(
									ReadablePropertyCategory(Unit->GetCategoryPath()))
								+ TEXT("/") + EscapeJsonPointerSegment(RowKey)
							: TEXT("/definitions/")
								+ EscapeJsonPointerSegment(RowKey));
				}
				if (bPackedPropertyContainer)
				{
					TSharedRef<FJsonObject> Categories = MakeShared<FJsonObject>();
					TArray<FString> CategoryKeys;
					CategoryRows.GetKeys(CategoryKeys);
					CategoryKeys.Sort();
					for (const FString& CategoryKey : CategoryKeys)
					{
						Categories->SetObjectField(
							CategoryKey, CategoryRows.FindChecked(CategoryKey));
					}
					Container->SetObjectField(TEXT("categories"), Categories);
				}
				else
				{
					Container->SetObjectField(TEXT("definitions"), KeyedRows);
				}
				if (bPackedDefinitionContainer)
				{
					for (TPair<FString, TSet<FString>>& Catalog : EnumCandidates)
					{
						TArray<FString> Names = Catalog.Value.Array();
						Names.Sort();
						TArray<TSharedPtr<FJsonValue>> Values;
						for (const FString& Name : Names)
						{
							Values.Add(MakeShared<FJsonValueString>(Name));
						}
						EnumCatalog->SetArrayField(Catalog.Key, MoveTemp(Values));
					}
					Container->SetObjectField(TEXT("enums"), EnumCatalog);
				}
				PackedDocuments.Add(PhysicalFile.Key, Container);
			}
			else
			{
				const FString& UnitId = PhysicalFile.Value[0];
				const FSemanticUnit* Unit = Model->FindUnit(UnitId);
				if (Unit == nullptr)
				{
					OutError.SetSourceFailure(TEXT("composition_unit_missing"), TEXT("A unit assigned to this exported document is absent from the authoring model."), UnitId);
					return false;
				}
				TArray<uint8> FileBytes;
				if (!TrySerializeSemanticUnitDocument(
						*Unit,
						Layout->GetMachineManifest().PublicRefMapping,
						FileBytes, OutError, PhysicalFile.Key))
				{
					return false;
				}
				Files.Emplace(
					PhysicalFile.Key,
					FileBytes,
					Sha256(FileBytes));
				UnitPointers.Add(UnitId, TEXT("/unit"));
				TArray<uint8> RowBytes;
				if (!TrySerializeSemanticModelUnitRow(
						*Unit,
						Layout->GetMachineManifest().PublicRefMapping,
						RowBytes, OutError, PhysicalFile.Key))
				{
					
					return false;
				}
				UnitEditHashes.Add(UnitId, Sha256(RowBytes));
			}
		}
		for (const TPair<FString, TSharedRef<FJsonObject>>& Packed
			: PackedDocuments)
		{
			if (Packed.Value->HasTypedField<EJson::Object>(TEXT("definitions")))
				Examples.AddDefinitions(Packed.Key, Packed.Value->GetObjectField(TEXT("definitions")).ToSharedRef(), Packed.Value->GetObjectField(TEXT("enums")));
			else
			{
				FUeAgentCanonicalValuePtr ExampleDocument;
				const auto* PreviousExample = ReusedPackedDocuments.Contains(Packed.Key) ? Baseline->PackedExamples.Find(Packed.Key) : nullptr;
				if (PreviousExample) ExampleDocument = *PreviousExample;
				else if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Packed.Value), Resources, ExampleDocument, OutError)) return false;
				PackedExamples.Add(Packed.Key, ExampleDocument.ToSharedRef());
				Examples.AddDocument(Packed.Key, ExampleDocument.ToSharedRef());
			}
			if (ReusedPackedDocuments.Contains(Packed.Key))
			{
				Files.Add(*BaselineFiles.FindChecked(Packed.Key));
				continue;
			}
			TArray<uint8> FileBytes;
			if (!TrySerializeSemanticHjsonObject(Packed.Value, FileBytes, OutError, Packed.Key))
			{
				
				return false;
			}
			const FString FileHash = Sha256(FileBytes);
			Files.Emplace(Packed.Key, MoveTemp(FileBytes), FileHash);
		}
		const double DocumentsFinished = FPlatformTime::Seconds();
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			if (bReusableProjection && IdenticalUnits.Contains(Unit.GetUnitId()))
			{
				const auto* PreviousInventory = Baseline->Inventory.Find(Unit.GetUnitId());
				const auto* Pointer = UnitPointers.Find(Unit.GetUnitId());
				const auto* EditHash = UnitEditHashes.Find(Unit.GetUnitId());
				const auto* PreviousPointer = Baseline->UnitPointers.Find(Unit.GetUnitId());
				const auto* PreviousHash = Baseline->UnitEditHashes.Find(Unit.GetUnitId());
				const auto* Content = UnitContentFiles.Find(Unit.GetUnitId());
				const auto* PreviousContent = Baseline->UnitContentFiles.Find(Unit.GetUnitId());
				const bool bSameContentPaths = (!Content && !PreviousContent)
					|| (Content && PreviousContent && ExportDataIdentical(*Content, *PreviousContent));
				if (PreviousInventory && Pointer && EditHash && PreviousPointer && PreviousHash
					&& bSameContentPaths && ExportDataIdentical(*Pointer, *PreviousPointer) && ExportDataIdentical(*EditHash, *PreviousHash))
				{
					InventoryByUnit.Add(Unit.GetUnitId(), *PreviousInventory);
					++ReusedInventoryRows;
					continue;
				}
			}
			TArray<uint8> RowBytes;
			if (!TrySerializeSemanticUnitRow(Unit, RowBytes, OutError,
				Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId())))
			{
				return false;
			}
			TSharedRef<FJsonObject> Inventory =
				MakeShared<FJsonObject>();
			Inventory->SetStringField(
				TEXT("unit_id"),
				Unit.GetUnitId());
			Inventory->SetStringField(
				TEXT("public_ref"),
				Layout->GetMachineManifest().PublicRefMapping.FindChecked(
					Unit.GetUnitId()));
			Inventory->SetStringField(
				TEXT("unit_kind"),
				LexToString(Unit.GetUnitKind()));
			Inventory->SetStringField(
				TEXT("logical_path"),
				Layout->GetLogicalPlacements().FindChecked(
					Unit.GetUnitId()));
			Inventory->SetStringField(
				TEXT("document_path"),
				Layout->GetPhysicalPlacements().FindChecked(
					Unit.GetUnitId()));
			const FString* UnitPointer = UnitPointers.Find(Unit.GetUnitId());
			const FString* UnitEditHash = UnitEditHashes.Find(Unit.GetUnitId());
			if (!UnitPointer || !UnitEditHash)
			{
				OutError.SetSourceFailure(TEXT("composition_unit_document_missing"), !UnitPointer ? TEXT("The exported unit has no document location.") : TEXT("The exported unit has no content fingerprint."),
					Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId()));
				return false;
			}
			Inventory->SetStringField(TEXT("unit_pointer"), *UnitPointer);
			Inventory->SetStringField(
				TEXT("content_hash"),
				Sha256(RowBytes));
			Inventory->SetStringField(
				TEXT("edit_hash"),
				*UnitEditHash);
			if (const auto* ContentFiles = UnitContentFiles.Find(Unit.GetUnitId()))
				Inventory->SetArrayField(TEXT("content_files"), StringArray(*ContentFiles));
			AddSemanticEnvelope(Unit, Inventory);
			// The existing machine envelope is the baseline for every unit;
			// these values do not enter model-facing editable files.
			{
				TSharedPtr<FJsonObject> CompactRow;
				const TSharedPtr<FJsonObject>* Values = nullptr;
				if (!TryBuildCompactRow(Unit, CompactRow, OutError, nullptr,
					Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId()))
					|| !CompactRow.IsValid()
					|| !CompactRow->TryGetObjectField(TEXT("values"), Values)
					|| Values == nullptr || !Values->IsValid())
				{
					if (!OutError.HasErrors()) OutError.SetSourceFailure(TEXT("composition_unit_envelope_failed"), TEXT("The semantic row did not provide the values required for its captured baseline."), Unit.GetUnitId(), TEXT("/values"));
					return false;
				}
				Inventory->GetObjectField(TEXT("envelope"))->SetObjectField(
					TEXT("captured_values"), (*Values).ToSharedRef());
				if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet)
				{
					Inventory->GetObjectField(TEXT("envelope"))->SetStringField(
						TEXT("captured_type"), ReadablePropertyType(Unit, Model.Get()));
					Inventory->GetObjectField(TEXT("envelope"))->SetStringField(
						TEXT("captured_definition"), ReadablePropertyDefinition(Unit, Model.Get()));
				}
			}
			FUeAgentCanonicalValuePtr CanonicalInventory;
			if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Inventory), Resources, CanonicalInventory, OutError)) return false;
			InventoryByUnit.Add(Unit.GetUnitId(), CanonicalInventory.ToSharedRef());
		}
		// Keep the complete captured baseline, but partition its machine-only
		// envelopes by the existing physical document owner. Coalesce those owners
		// into a bounded set of stable buckets: one file per logical document added
		// measured filesystem overhead in the Large consumer. Identity keys
		// remain complete and unique; bucket sharing only widens invalidation.
		TMap<FString, FUeAgentCanonicalValueRef> ManifestInventoryByUnit;
		TMap<FString, TArray<TPair<FString, FUeAgentCanonicalValueRef>>> BaselineGroups;
		TMap<FString, TArray<FUeAgentCanonicalValueRef>> InventoryGroups;
		TMap<FString, FString> BaselinePaths;
		TSet<FString> ChangedBaselineGroups;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			const FString& DocumentPath = Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId());
			FString* GroupPath = BaselinePaths.Find(DocumentPath);
			if (!GroupPath) GroupPath = &BaselinePaths.Add(DocumentPath,
				TEXT(".uai/baselines/") + Sha256Text(DocumentPath).Right(64).Left(2) + TEXT(".hjson"));
			const FString& Path = *GroupPath;
			const auto& Complete = InventoryByUnit.FindChecked(Unit.GetUnitId());
			const auto* Envelope = Complete->GetObjectItems().FindByPredicate(
				[](const auto& Field) { return Field.Key == TEXT("envelope"); });
			if (!Envelope) { OutError.SetSourceFailure(TEXT("composition_unit_envelope_failed"),
				TEXT("A captured baseline envelope is missing."), Unit.GetUnitId()); return false; }
			BaselineGroups.FindOrAdd(Path).Emplace(Unit.GetUnitId(), Envelope->Value);
			const auto* PreviousComplete = bReusableProjection ? Baseline->Inventory.Find(Unit.GetUnitId()) : nullptr;
			const auto* Previous = PreviousComplete && &PreviousComplete->Get() == &Complete.Get()
				? Baseline->ManifestInventory.Find(Unit.GetUnitId()) : nullptr;
			if (Previous)
			{
				InventoryGroups.FindOrAdd(Path.Replace(TEXT("/baselines/"), TEXT("/inventory/"))).Add(*Previous);
				ManifestInventoryByUnit.Add(Unit.GetUnitId(), *Previous);
				continue;
			}
			ChangedBaselineGroups.Add(Path);
			auto Fields = Complete->GetObjectItems();
			Fields.RemoveAll([](const auto& Field) { return Field.Key == TEXT("envelope"); });
			FUeAgentCanonicalValuePtr Reference, Thin;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(Path, Resources, Reference, Error))
			{ OutError.SetFailure(Error); return false; }
			Fields.Emplace(TEXT("envelope_ref"), Reference.ToSharedRef());
			if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Thin, Error))
			{ OutError.SetFailure(Error); return false; }
			InventoryGroups.FindOrAdd(Path.Replace(TEXT("/baselines/"), TEXT("/inventory/"))).Add(Thin.ToSharedRef());
			ManifestInventoryByUnit.Add(Unit.GetUnitId(), Thin.ToSharedRef());
		}
		int32 ReusedBaselineFiles = 0;
		for (auto& Group : BaselineGroups)
		{
			if (!ChangedBaselineGroups.Contains(Group.Key) && BaselineFiles.Contains(Group.Key))
			{ Files.Add(*BaselineFiles.FindChecked(Group.Key)); ++ReusedBaselineFiles; continue; }
			FUeAgentCanonicalValuePtr Envelopes;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Group.Value), Resources, Envelopes, Error))
			{ OutError.SetFailure(Error); return false; }
			TArray<uint8> Bytes;
			if (!TrySerializeSemanticHjsonObject(*Envelopes, Bytes, OutError, Group.Key)) return false;
			const FString Hash = Sha256(Bytes);
			Files.Emplace(Group.Key, MoveTemp(Bytes), Hash);
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=baseline_documents files=%d reused_files=%d"),
			BaselineGroups.Num(), ReusedBaselineFiles);
		// The inventory is machine-owned just like the complete baselines. Keep
		// its stable buckets separate from the changing revision header so the
		// publisher/resolver can validate actual unchanged files with the same
		// parser proof, rather than reparsing every row for three changed values.
		TArray<FString> InventoryPaths;
		InventoryGroups.GetKeys(InventoryPaths); InventoryPaths.Sort();
		int32 ReusedInventoryFiles = 0;
		for (const FString& Path : InventoryPaths)
		{
			const FString BaselinePath = Path.Replace(TEXT("/inventory/"), TEXT("/baselines/"));
			if (!ChangedBaselineGroups.Contains(BaselinePath) && BaselineFiles.Contains(Path))
			{ Files.Add(*BaselineFiles.FindChecked(Path)); ++ReusedInventoryFiles; continue; }
			FUeAgentCanonicalValuePtr Rows, Document;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryArray(MoveTemp(InventoryGroups.FindChecked(Path)), Resources, Rows, Error)
				|| !FUeAgentCanonicalValue::TryObject({{TEXT("unit_inventory"), Rows.ToSharedRef()}}, Resources, Document, Error))
			{ OutError.SetFailure(Error, Path); return false; }
			TArray<uint8> Bytes;
			if (!TrySerializeSemanticHjsonObject(*Document, Bytes, OutError, Path)) return false;
			const FString Hash = Sha256(Bytes); Files.Emplace(Path, MoveTemp(Bytes), Hash);
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=inventory_documents files=%d reused_files=%d"),
			InventoryPaths.Num(), ReusedInventoryFiles);

		const double InventoryFinished = FPlatformTime::Seconds();
		const auto Targets = MakeShared<FJsonObject>();
		TMap<FString, FString> PreviewAssetPaths;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			if (!AssetPreview::IsPreviewSemantic(Unit.GetSemanticTypeRef())) continue;
			const FString Group = AssetPreview::GroupFromSemantic(Unit.GetSemanticTypeRef());
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			UE::UAI::Contract::FDiagnostic IdentityError;
			if (!UeAgentAuthoringIdentity::ParseCanonicalKey(Unit.GetTargetIdentityKey(), Identity, IdentityError)) continue;
			FString Target, GroupSuffix;
			if (!Unit.GetRole().Mid(FCString::Strlen(TEXT("preview/"))).Split(TEXT("/"), &Target, &GroupSuffix)) continue;
			auto TargetInfo = Targets->HasField(Target) ? Targets->GetObjectField(Target) : MakeShared<FJsonObject>();
			auto PreviewGroups = TargetInfo->HasField(TEXT("groups")) ? TargetInfo->GetObjectField(TEXT("groups")) : MakeShared<FJsonObject>();
			const FString OwnerPath = Identity.GetSelector(TEXT("owner_path"));
			FString PreviewAssetPath, LocalSuffix;
			if (OwnerPath.Split(TEXT(":preview:"), &PreviewAssetPath, &LocalSuffix))
				PreviewAssetPaths.Add(Target, PreviewAssetPath);
			FString Scope = OwnerPath.Contains(TEXT(":preview:"))
				? (Group == TEXT("capture") ? TEXT("capture") : TEXT("viewport")) : TEXT("shared_profile");
			FString NativeProfile;
			for (const FString& Dependency : Unit.GetDependencyRefs())
			{
				const auto* Definition = Model->FindUnit(Dependency);
				if (!Definition) continue;
				const auto* Constraints = Definition->GetValues().Find(TEXT("constraints"));
				if (!Constraints) continue;
				if (const auto* Value = FindObjectField(*Constraints, TEXT("scope"))) Scope = (*Value)->GetText();
				if (const auto* Value = FindObjectField(*Constraints, TEXT("profile"))) NativeProfile = (*Value)->GetText();
			}
			auto Description = PreviewGroups->HasField(Group) ? PreviewGroups->GetObjectField(Group) : MakeShared<FJsonObject>();
			Description->SetStringField(TEXT("file"), Target + TEXT("/") + Group + TEXT(".hjson"));
			auto Owners = Description->HasField(TEXT("owners")) ? Description->GetObjectField(TEXT("owners")) : MakeShared<FJsonObject>();
			auto Owner = Owners->HasField(Scope) ? Owners->GetObjectField(Scope) : MakeShared<FJsonObject>();
			Owner->SetStringField(TEXT("source"), Unit.GetSourceProvenance().RightChop(FCString::Strlen(TEXT("asset_preview."))));
			if (Scope == TEXT("shared_profile"))
			{
				if (!NativeProfile.IsEmpty()) Owner->SetStringField(TEXT("profile"), NativeProfile);
			}
			TArray<TSharedPtr<FJsonValue>> Fields;
			if (Owner->HasField(TEXT("fields"))) Fields = Owner->GetArrayField(TEXT("fields"));
			Fields.Add(MakeShared<FJsonValueString>(Unit.GetFieldSetRef()));
			Owner->SetArrayField(TEXT("fields"), MoveTemp(Fields));
			Owners->SetObjectField(Scope, Owner);
			Description->SetObjectField(TEXT("owners"), Owners);
			PreviewGroups->SetObjectField(Group, Description);
			TargetInfo->SetObjectField(TEXT("groups"), PreviewGroups);
			Targets->SetObjectField(Target, TargetInfo);
		}
		if (!Targets->Values.IsEmpty())
		{
			TArray<FString> TargetNames;
			Targets->Values.GetKeys(TargetNames);
			TargetNames.Sort();
			for (const FString& TargetName : TargetNames)
			{
			const auto Main = Targets->GetObjectField(TargetName);
			const auto PreviewGroups = Main->GetObjectField(TEXT("groups"));
			for (auto& Pair : PreviewGroups->Values)
			{
				auto Owners = Pair.Value->AsObject()->GetObjectField(TEXT("owners"));
				if (Owners->Values.Num() == 1)
					for (auto& Owner : Owners->Values) Owner.Value->AsObject()->RemoveField(TEXT("fields"));
			}
			if (!PreviewGroups->HasField(TEXT("view")))
				Main->SetStringField(TEXT("view_state"), TEXT("requires_open_preview"));
			Main->SetArrayField(TEXT("operations"), StringArray({TEXT("configure"), TEXT("capture_single"), TEXT("capture_sequence")}));
			if (const auto* AssetPath = PreviewAssetPaths.Find(TargetName))
				Main->SetArrayField(TEXT("actions"), StringArray(AssetPreview::GetActions(
					FindObject<UObject>(nullptr, **AssetPath), TargetName)));
			}
			const auto Navigation = MakeShared<FJsonObject>();
			Navigation->SetStringField(TEXT("default_target"), TargetNames.Contains(TEXT("main")) ? TEXT("main") : TargetNames[0]);
			Navigation->SetObjectField(TEXT("targets"), Targets);
			TArray<uint8> NavigationBytes;
			if (!TrySerializeSemanticHjsonObject(Navigation, NavigationBytes, OutError, TEXT("preview/index.hjson")))
			{
				
				return false;
			}
			// Generated navigation shares the publisher lifecycle, but is not an
			// editable semantic unit or a second source of property values.
			Files.Emplace(TEXT("preview/index.hjson"), NavigationBytes, Sha256(NavigationBytes));
		}

		const double ExamplesStarted = FPlatformTime::Seconds();
		if (!Examples.Publish(Files, OutError, [](const TArray<uint8>& Bytes) { return Sha256(Bytes); })) return false;
		const double ExamplesFinished = FPlatformTime::Seconds();

		const FUeAgentStateRevision& Revision =
			SnapshotBinding.Revision;
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(
			TEXT("workspace_schema"),
			GetSemanticWorkspaceSchemaId());
		Manifest->SetStringField(TEXT("import_mode"), TEXT("state"));
		Manifest->SetStringField(
			TEXT("contract_id"),
			Model->GetContractId());
		Manifest->SetField(
			TEXT("contract_version"),
			MakeShared<FJsonValueNumberString>(FString::FromInt(Model->GetContractVersion())));
		Manifest->SetField(
			TEXT("layout_version"),
			MakeShared<FJsonValueNumberString>(FString::FromInt(Layout->GetLayoutVersion())));
		Manifest->SetStringField(
			TEXT("profile_id"),
			Model->GetProfileId());
		Manifest->SetStringField(
			TEXT("base_revision"),
			Model->GetBaseRevision());
		TSharedRef<FJsonObject> RevisionObject =
			MakeShared<FJsonObject>();
		RevisionObject->SetStringField(
			TEXT("source_id"),
			Revision.GetSourceId());
		RevisionObject->SetStringField(
			TEXT("generation"),
			Revision.GetGeneration());
		RevisionObject->SetField(
			TEXT("sequence"),
			MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), Revision.GetSequence())));
		Manifest->SetObjectField(
			TEXT("base_state_revision"),
			RevisionObject);
		Manifest->SetStringField(
			TEXT("semantic_fingerprint"),
			Model->GetSemanticFingerprint());
		Manifest->SetArrayField(
			TEXT("target_unit_ids"),
			StringArray(Model->GetTargetUnitIds()));
		Manifest->SetArrayField(
			TEXT("structural_edits"),
			TArray<TSharedPtr<FJsonValue>>{});
		Manifest->SetArrayField(TEXT("unit_inventory_files"), StringArray(InventoryPaths));

		TArray<uint8> ManifestBytes;
		FUeAgentCanonicalValuePtr CanonicalManifest;
		if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Manifest), Resources, CanonicalManifest, OutError)) return false;
		if (!TrySerializeSemanticHjsonObject(*CanonicalManifest, ManifestBytes, OutError, ManifestPath))
		{
			
			return false;
		}
		const FString ManifestHash = Sha256(ManifestBytes);
		Files.Emplace(
			ManifestPath,
			MoveTemp(ManifestBytes),
			ManifestHash);
		const double ManifestFinished = FPlatformTime::Seconds();
		Files.Sort(
			[](const FExportFilePlan& Left,
				const FExportFilePlan& Right)
			{
				return Left.GetRelativePath()
					< Right.GetRelativePath();
			});

		int64 OutputBytes = 0;
		FString PlanMaterial =
			TEXT("directory|") + ManifestPath + TEXT("|")
			+ Model->GetSemanticFingerprint()
			+ TEXT("|") + SnapshotBinding.SnapshotId
			+ TEXT("|") + Revision.GetSourceId()
			+ TEXT("|") + Revision.GetGeneration()
			+ TEXT("|")
			+ FString::Printf(TEXT("%lld"), Revision.GetSequence())
			+ TEXT("|") + SnapshotBinding.EvidenceId;
		for (const FExportFilePlan& File : Files)
		{
			OutputBytes += File.GetCanonicalUtf8().Num();
			PlanMaterial += TEXT("|") + File.GetRelativePath()
				+ TEXT("|") + File.GetContentHash();
		}
		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::OutputBytes, OutputBytes},
				{EUeAgentResourceDimension::Files, Files.Num()},
				{EUeAgentResourceDimension::WorkUnits,
					Files.Num() + Model->GetUnits().Num()},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					OutputBytes * 2},
			},
			TEXT("authoring_composition.materialize"),
			ResourceError))
		{
			OutError.SetFailure(ResourceError, FString());
			return false;
		}
		const FString Fingerprint = Sha256Text(PlanMaterial);
		const int64 ObservedTokens = EstimatedTokens(Files);
		OutPlan = MakeShared<FExportWorkspacePlan>(
			EWorkspaceKind::Directory,
			ManifestPath,
			SnapshotBinding.SnapshotId,
			Revision,
			SnapshotBinding.EvidenceId,
			MoveTemp(Files),
			Fingerprint,
			ObservedTokens);
		// A single bounded candidate, never a registry of live assets. Larger
		// exports simply retain the existing uncached path.
		PreviousCompositionExport.Reset();
		if (OutputBytes <= 512ll * 1024 * 1024 && Model->GetUnits().Num() <= 200000)
		{
			auto Next = MakeShared<FCompositionExportBaseline>();
			Next->ContractId = Model->GetContractId();
			Next->ContractVersion = Model->GetContractVersion();
			Next->ProfileId = Model->GetProfileId();
			Next->TargetUnitIds = Model->GetTargetUnitIds();
			Next->LayoutVersion = Layout->GetLayoutVersion();
			Next->LogicalPlacements = Layout->GetLogicalPlacements();
			Next->PhysicalPlacements = Layout->GetPhysicalPlacements();
			Next->PhysicalFiles = Layout->GetPhysicalFiles();
			Next->PublicRefMapping = Layout->GetMachineManifest().PublicRefMapping;
			Next->UnitContentKeys = MoveTemp(UnitContentKeys);
			Next->ItemValueInvariantKeys = MoveTemp(ItemValueInvariantKeys);
			for (const FExportFilePlan& File : OutPlan->GetFiles())
				if (File.GetRelativePath() != ManifestPath) Next->Files.Add(File);
			Next->TargetKind = TargetKind;
			Next->PackedDocuments = MoveTemp(PackedDocuments);
			Next->PackedExamples = MoveTemp(PackedExamples);
			Next->Inventory = MoveTemp(InventoryByUnit);
			Next->ManifestInventory = MoveTemp(ManifestInventoryByUnit);
			Next->UnitPointers = MoveTemp(UnitPointers);
			Next->UnitEditHashes = MoveTemp(UnitEditHashes);
			Next->UnitContentFiles = MoveTemp(UnitContentFiles);
			PreviousCompositionExport = MoveTemp(Next);
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=incremental_composition eligible=%s changed_properties=%d changed_value_items=%d changed_other=%d reused_documents=%d reused_inventory=%d compare_s=%.6f"),
			bReusableProjection ? TEXT("true") : TEXT("false"), ChangedPropertyUnits, ChangedValueItems, ChangedOtherUnits,
			ReusedDocuments, ReusedInventoryRows, ComparisonFinished - LayoutFinished);
		const double PlanFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=export_plan kind=%s units=%d files=%d bytes=%lld layout_s=%.6f documents_s=%.6f inventory_s=%.6f navigation_s=%.6f examples_s=%.6f manifest_s=%.6f finalize_s=%.6f total_s=%.6f compact_validation_s=%.6f compact_rows=%d success=true"),
			*TargetKind, Model->GetUnits().Num(), OutPlan->GetFiles().Num(), OutputBytes,
			LayoutFinished - PlanStarted, DocumentsFinished - LayoutFinished,
			InventoryFinished - DocumentsFinished, ExamplesStarted - InventoryFinished,
			ExamplesFinished - ExamplesStarted, ManifestFinished - ExamplesFinished,
			PlanFinished - ManifestFinished, PlanFinished - PlanStarted,
			CompactValidationSeconds, CompactValidationCount);
		return true;
	}

	bool TryExportCompositionWorkspace(
		const FString& TargetKind,
		const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const FString& WorkspaceRoot,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FPreparedWorkspaceExport* PreparedExport)
	{
		if (PreparedExport) *PreparedExport = {};
		OutResult =
			UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutResult.Status = TEXT("failed");
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!RegisterGraphTreeTargetCompositionContracts(ContractError)
			|| !RegisterOtherTargetCompositionContracts(ContractError))
		{
			OutResult.Diagnostic = ContractError;
			return false;
		}
		FUeAgentResourceTracker Resources =
			MakeSemanticWorkspaceResourceTracker();
		FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
		FAuthoringModelPtr Model;
		FWorkspaceError WorkspaceError;
		if (!TryBuildCanonicalAuthoringCapture(
			TargetKind,
			ProfileId,
			Snapshot,
			Resources,
			CanonicalSnapshot,
			Model,
			WorkspaceError)
			|| !CanonicalSnapshot.IsValid()
			|| !Model.IsValid())
		{
			OutResult.Diagnostic = WorkspaceError.GetDiagnostic();
			return false;
		}
		return TryExportCapturedCompositionWorkspace(TargetKind, ProfileId, Model.ToSharedRef(),
			{CanonicalSnapshot->GetSnapshotId(), CanonicalSnapshot->GetRevision(),
				CanonicalSnapshot->GetProvenance().GetEvidenceId()},
			WorkspaceRoot, Resources, OutResult, PreparedExport);
	}

	bool TryExportCapturedCompositionWorkspace(
		const FString& TargetKind,
		const FString& ProfileId,
		FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& SnapshotBinding,
		const FString& WorkspaceRoot,
		FUeAgentResourceTracker& Resources,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FPreparedWorkspaceExport* PreparedExport)
	{
		const double ExportStarted = FPlatformTime::Seconds();
		if (PreparedExport) *PreparedExport = {};
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutResult.Status = TEXT("failed");
		FWorkspaceError WorkspaceError;
		FExportWorkspacePlanPtr Plan;
		if (!TryBuildCompositionExportPlan(
			Model,
			SnapshotBinding,
			{},
			Resources,
			Plan,
			WorkspaceError, TargetKind)
			|| !Plan.IsValid())
		{
			OutResult.Diagnostic = WorkspaceError.GetDiagnostic();
			return false;
		}
		const double PlanFinished = FPlatformTime::Seconds();
		FArtifactPublishReceipt Receipt;
		FArtifactPublishError PublishError;
		const FString OwnerId =
			TEXT("uai.authoring.composition.")
			+ TargetKind.Replace(TEXT("."), TEXT("_"));
		if (PreparedExport)
		{
			PreparedExport->Plan = Plan;
			PreparedExport->PublisherOwnerId = OwnerId;
			OutResult.Status = TEXT("prepared");
			OutResult.ProfileId = ProfileId;
			OutResult.WorkspaceRoot = WorkspaceRoot;
			OutResult.ExpectedRevision = Model->GetBaseRevision();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=workspace_export_prepare target_kind=%s plan_s=%.6f total_s=%.6f"),
				*TargetKind, PlanFinished - ExportStarted, FPlatformTime::Seconds() - ExportStarted);
			return true;
		}
		if (!TryPublishExportPlan(
			FArtifactPublishRequest(
				Plan.ToSharedRef(),
				WorkspaceRoot,
				OwnerId),
			Resources,
			Receipt,
			PublishError))
		{
			OutResult.Diagnostic = PublishError;
			return false;
		}

		const double PublishFinished = FPlatformTime::Seconds();
		OutResult.Status = TEXT("written");
		OutResult.ContractId = Model->GetContractId();
		OutResult.ContractVersion =
			FString::FromInt(Model->GetContractVersion());
		OutResult.ProfileId = ProfileId;
		OutResult.ExpectedRevision = Model->GetBaseRevision();
		OutResult.WorkspaceRoot = Receipt.GetFinalPath();
		OutResult.EntryFile = Receipt.GetEntryFile();
		for (const FExportFilePlan& File : Plan->GetFiles())
		{
			UeAgentExportWorkspaceWriter::FPlannedFile Planned;
			Planned.FileId = File.GetRelativePath();
			Planned.RelativePath = File.GetRelativePath();
			Planned.ResolvedPath = FPaths::Combine(
				Receipt.GetFinalPath(),
				File.GetRelativePath());
			Planned.Role = File.GetRelativePath() == ManifestPath
				? TEXT("machine_manifest")
				: IsModelVisibleSemanticWorkspacePath(File.GetRelativePath())
					? TEXT("semantic_unit")
					: TEXT("machine_state");
			Planned.Metrics =
				UeAgentExportWorkspaceWriter::MeasureUtf8Text(
					File.GetCanonicalUtf8());
			OutResult.Metrics.Add(Planned.Metrics);
			if (Planned.Role == TEXT("semantic_unit"))
			{
				OutResult.ModelMetrics.Add(Planned.Metrics);
			}
			OutResult.Files.Add(MoveTemp(Planned));
		}
		OutResult.WrittenRelativeFiles =
			Receipt.GetIncludedFiles();
		for (const FString& Relative : Receipt.GetIncludedFiles())
		{
			OutResult.WrittenFiles.Add(
				FPaths::Combine(
					Receipt.GetFinalPath(),
					Relative));
		}
		TSharedRef<FJsonObject> ReceiptJson =
			MakeShared<FJsonObject>();
		ReceiptJson->SetStringField(
			TEXT("publisher_owner_id"),
			Receipt.GetPublisherOwnerId());
		ReceiptJson->SetStringField(
			TEXT("transaction_id"),
			Receipt.GetTransactionId());
		ReceiptJson->SetStringField(
			TEXT("plan_fingerprint"),
			Receipt.GetPlanFingerprint());
		ReceiptJson->SetStringField(
			TEXT("inventory_hash"),
			Receipt.GetInventoryHash());
		OutResult.PublisherReceipt = ReceiptJson;
		if (!Receipt.GetCleanupWarnings().IsEmpty()) {
			TArray<TSharedPtr<FJsonValue>> Warnings;
			for (const auto& Warning : Receipt.GetCleanupWarnings())
				Warnings.Add(MakeShared<FJsonValueObject>(Warning.ToPublicJson()));
			ReceiptJson->SetArrayField(TEXT("warnings"), MoveTemp(Warnings));
		}
		const double ExportFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=workspace_export kind=%s profile=%s plan_s=%.6f publish_s=%.6f result_s=%.6f total_s=%.6f success=true"),
			*TargetKind, *ProfileId, PlanFinished - ExportStarted,
			PublishFinished - PlanFinished, ExportFinished - PublishFinished,
			ExportFinished - ExportStarted);
		return true;
	}

	FReadablePropertyAddress DescribeReadableProperty(const FSemanticUnit& Unit, const FAuthoringModel& Model)
	{
		FReadablePropertyAddress Result{ReadablePropertyCategory(Unit.GetCategoryPath()), Unit.GetFieldSetRef(), ReadablePropertyType(Unit, Model), ReadablePropertyDefinition(Unit, Model)};
		if (Unit.GetSemanticTypeRef() == TEXT("uai.semantic.blueprint.pin_default") && Result.Key.StartsWith(TEXT("pin_default.")))
			Result.Key.RightChopInline(12);
		return Result;
	}

	bool TryComposeReflectedPropertyTemplate(const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		ContextResource::FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!RegisterGraphTreeTargetCompositionContracts(ContractError) || !RegisterOtherTargetCompositionContracts(ContractError))
		{
			OutError.SetFailure(ContractError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_composition_contract_registration_failed"), TEXT("Target composition contract registration failed without returning a diagnostic.")) : ContractError, FString()); return false;
		}
		FCompositionRequest Request;
		Request.TargetKind = TEXT("ue.generic.object");
		Request.ProfileId = TEXT("llm_authoring");
		Request.BaseRevision = TEXT("native-default-template");
		Request.SourceProvenance = TEXT("authoring_state.capture");
		Request.Entries = Snapshot.CopyEntries();
		return TryComposeAuthoringModel(Request, Resources, OutModel, OutError);
	}

	bool TryProjectAuthoringCaptureValue(const FUeAgentStateEntry& Entry,
		FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError)
	{
		FCompositionEntryView View;
		View.Value = Entry.GetValue();
		if (!TryProjectCanonicalAuthoringDescriptor(Entry, View, Resources, OutError)) return false;
		OutValue = View.Value;
		return true;
	}

	bool TryProjectDirectPropertyValue(const FUeAgentCanonicalValueRef& Value, const FAuthoringModel& Model,
		const FUeAgentCanonicalValue* ValueDefinition, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError, bool bNativeScope,
		const FUeAgentCanonicalValue* ValueType)
	{
		UE::UAI::Contract::FDiagnostic Error;
		TSharedPtr<FJsonValue> JsonValue;
		if (!CanonicalJson::TryToJsonValue(Value.Get(), JsonValue, Error))
		{
			OutError.SetFailure(Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("direct_property_projection_failed"), TEXT("The property value could not be converted to its editable representation.")) : Error, FString());
			return false;
		}
		const auto Projected = ProjectDirectPropertyValue(JsonValue, Model, Error, ValueDefinition, false, 0, bNativeScope, ValueType);
		if (!Projected || !CanonicalJson::TryFromJsonValue(Projected, Resources, OutValue, Error))
		{
			OutError.SetFailure(Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("direct_property_projection_failed"), TEXT("The property value could not be converted to its editable representation.")) : Error, FString());
			return false;
		}
		return true;
	}

	bool TryProjectReflectedRecord(
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		TSharedPtr<FJsonObject>& OutProperties,
		TSharedPtr<FJsonObject>& OutDefinitions,
		FWorkspaceError& OutError, bool bGroupByCategory, bool bNativeScope)
	{
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!RegisterGraphTreeTargetCompositionContracts(ContractError)
			|| !RegisterOtherTargetCompositionContracts(ContractError))
		{
			OutError.SetFailure(ContractError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_composition_contract_registration_failed"), TEXT("Target composition contract registration failed without returning a diagnostic.")) : ContractError, FString());
			return false;
		}
		if (Snapshot.IsEmpty())
		{
			OutProperties = MakeShared<FJsonObject>();
			OutDefinitions = MakeShared<FJsonObject>();
			return true;
		}
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		FAuthoringModelPtr Model;
		if (!TryComposeReflectedPropertyTemplate(Snapshot, Resources, Model, OutError)) return false;
		OutProperties = MakeShared<FJsonObject>();
		OutDefinitions = MakeShared<FJsonObject>();
		// The nested-record contract returns definitions independent of this temporary
		// model. Expand its private references before the caller interns them in its own catalog.
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			if (Unit.GetUnitKind() != ESemanticUnitKind::PropertySet
				|| Unit.GetFieldSetRef().IsEmpty()
				|| Unit.GetSemanticTypeRef().StartsWith(TEXT("uai.authoring.surface."))) continue;
			TSharedPtr<FJsonObject> Row;
			if (!TryBuildCompactRow(Unit, Row, OutError)) return false;
			UE::UAI::Contract::FDiagnostic ProjectionError;
            const TSharedPtr<FJsonValue> Value = DirectPropertyValue(Row, Unit, *Model, ProjectionError, bNativeScope);
			if (!Value.IsValid())
			{
				OutError.SetFailure(ProjectionError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("composition_property_value_invalid"), TEXT("The property could not be projected.")) : ProjectionError, Unit.GetFieldSetRef());
				return false;
			}
			const FString Type = ReadablePropertyType(Unit, *Model);
			UeAgentAuthoringState::FStateEntry NativeEntry;
			if (UeAgentAuthoringEncodedTypes::FScope::IsActive()
				&& Snapshot.FindEntry(Unit.GetSemanticTypeRef(), Unit.GetTargetIdentityKey(), NativeEntry))
			{
				const auto Definition = NativeEntry.ComparisonValueDefinition ? NativeEntry.ComparisonValueDefinition : NativeEntry.ValueDefinition;
				UeAgentAuthoringEncodedTypes::FScope::Record(Value, NativeEntry.Type,
					Definition && Definition->Type == EJson::Object ? Definition->AsObject() : nullptr);
			}
			const FString DefinitionName = bNativeScope ? FString() : ReadablePropertyDefinition(Unit, *Model);
			const TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), Type);
			if (!DefinitionName.IsEmpty()) Property->SetStringField(TEXT("definition"), DefinitionName);
			Property->SetField(TEXT("value"), Value);
			if (bGroupByCategory)
			{
				auto Address = DescribeReadableProperty(Unit, *Model);
				Address.Type = Type;
				if (!TryAddReadablePropertyRow(OutProperties.ToSharedRef(), {Address.Key, Address.Category, Address.Type, DefinitionName}, Value))
				{ OutError.SetSourceFailure(TEXT("composition_property_address_ambiguous"), FString::Printf(TEXT("More than one property has name '%s' in category '%s'."), *Address.Key, *Address.Category), Address.Key); return false; }
			}
			else OutProperties->SetObjectField(Unit.GetFieldSetRef(), Property);
			if (bNativeScope) continue;
			for (const FString& Dependency : Unit.GetDependencyRefs())
			{
				const FSemanticUnit* Definition = Model->FindUnit(Dependency);
				if (!Definition || Definition->GetRole() != TEXT("property_definition")) continue;
				TSharedPtr<FJsonObject> DefinitionRow;
				if (!TryBuildCompactRow(*Definition, DefinitionRow, OutError)) return false;
				const auto Expanded = ExpandPropertyDefinition(*Model, DefinitionRow->TryGetField(TEXT("values")),
					OutError, Definition->GetUnitId(), TEXT("/values"));
				if (!Expanded) return false;
				OutDefinitions->SetObjectField(DefinitionName, Expanded->AsObject());
			}
		}
		return true;
	}
}
