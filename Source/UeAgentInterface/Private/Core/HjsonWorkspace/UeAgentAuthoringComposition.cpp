#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentHjsonValidatedHeader.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPlanBytes.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceExport.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceDelta.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentSemanticPublicReferences.h"
#include "Core/HjsonWorkspace/UeAgentSemanticUnitComparison.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Utilities/UeAgentReadableName.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringContentLayout.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringExamples.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Containers/StringConv.h"
#include "String/BytesToHex.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "Async/ParallelFor.h"
#include "CoreGlobals.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/StringBuilder.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include <cstring>
#include <type_traits>

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;
	static TSharedPtr<const FDefinitionProjectionHints> GetCompositionDefinitionProjectionHints(
		const FString& TargetKind, const FString& ProfileId);

bool IsCompositionDependencyPathField(const FString& Key)
	{
		return Key != TEXT("asset_path") && Key.EndsWith(TEXT("_path"));
	}

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

		// Equality for the complete, read-only JSON input to DefinitionIdentity.
		// Unlike CompareEqual alone, never coerce different scalar JSON kinds.
		bool IdenticalDefinitionJson(const FJsonValue& Left, const FJsonValue& Right)
		{
			if (Left.Type != Right.Type) return false;
			if (Left.Type == EJson::None) return false;
			if (Left.Type == EJson::String) return Left.AsString().Equals(Right.AsString(), ESearchCase::CaseSensitive);
			if (Left.Type == EJson::Number) {
				// DefinitionIdentity's writer preserves number-string lexemes.
				// For ordinary doubles retain even the sign bit of zero.
				if (Left.PreferStringRepresentation() != Right.PreferStringRepresentation()) return false;
				if (Left.PreferStringRepresentation()) return Left.AsString().Equals(Right.AsString(), ESearchCase::CaseSensitive);
				const double A = Left.AsNumber(), B = Right.AsNumber();
				return FMemory::Memcmp(&A, &B, sizeof(double)) == 0;
			}
			if (Left.Type == EJson::Object) {
				const auto AObject = Left.AsObject(), BObject = Right.AsObject();
				if (!AObject || !BObject) return false;
				const auto& A = AObject->Values;
				const auto& B = BObject->Values;
				if (A.Num() != B.Num()) return false;
				for (const auto& Pair : A) {
					const auto Id = B.FindId(Pair.Key);
					if (!Id.IsValidId()) return false;
					const auto& Other = B.Get(Id);
					if (!Pair.Key.Equals(Other.Key, ESearchCase::CaseSensitive)) return false;
					if (Pair.Value == Other.Value) continue;
					if (!Pair.Value || !Other.Value || !IdenticalDefinitionJson(*Pair.Value, *Other.Value)) return false;
				}
				return true;
			}
			if (Left.Type == EJson::Array) {
				const auto& A = Left.AsArray(); const auto& B = Right.AsArray();
				if (A.Num() != B.Num()) return false;
				for (int32 I = 0; I < A.Num(); ++I) {
					if (A[I] == B[I]) continue;
					if (!A[I] || !B[I] || !IdenticalDefinitionJson(*A[I], *B[I])) return false;
				}
				return true;
			}
			return FJsonValue::CompareEqual(Left, Right);
		}

		bool IdenticalDefinitionInput(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			return Left == Right || (Left && Right && IdenticalDefinitionJson(*Left, *Right));
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
			// Presentation belongs to the captured baseline too. It is deliberately
			// outside the historical row/model hashes, but source-model export must
			// not replace an unchanged property's category with General.
			TArray<TSharedPtr<FJsonValue>> Categories;
			for (const FString& Category : Unit.GetCategoryPath())
				Categories.Add(MakeShared<FJsonValueString>(Category));
			Envelope->SetArrayField(TEXT("category_path"), MoveTemp(Categories));
			Inventory->SetObjectField(TEXT("envelope"), Envelope);
		}

		bool TryBuildCompactRow(
			const FSemanticUnit& Unit,
			TSharedPtr<FJsonObject>& OutRow, FWorkspaceError& OutError,
			const FWorkspacePlacementView* PublicRefs = nullptr, const FString& SourceFile = FString())
		{
			OutRow.Reset();
			return PublicRefs ? TryBuildSemanticModelUnitRow(Unit, OutRow, OutError, *PublicRefs, SourceFile)
				: TryBuildSemanticModelUnitRow(Unit, OutRow, OutError, nullptr, SourceFile);
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

		template <typename TUnit> FString ReadablePropertyKey(const TUnit& Unit)
		{
			FString Key = Unit.GetFieldSetRef();
			if (Unit.GetSemanticTypeRef() == TEXT("uai.semantic.blueprint.pin_default") && Key.StartsWith(TEXT("pin_default.")))
				Key.RightChopInline(12);
			return Key;
		}

		template <typename TUnit> FString ReadablePropertyDefinition(const TUnit& Unit, const FAuthoringModel& Model)
		{
			if (const FString* Name = Model.FindReadablePropertyDefinitionName(Unit.GetUnitId())) return *Name;
			for (const FString& Dependency : Unit.GetDependencyRefs())
				if (const FString* Name = Model.FindReadablePropertyDefinitionName(Dependency)) return *Name;
			return FString();
		}

		FString ReadableTypeFromDefinition(const FSemanticUnit& Definition)
		{
				const FUeAgentCanonicalValueRef* Type =
					Definition.FindValue(TEXT("type"));
				if (Type == nullptr
					|| (*Type)->GetKind() != EUeAgentValueKind::Object)
				{
					return FString();
				}
				// A type describes the value, not the property's edit/candidate contract.
				return ReadablePropertyTypeName(Type->Get());
		}
		FString ReadablePropertyType(const FSemanticUnit& Unit, const FAuthoringModel& Model)
		{
			if (const FString OwnType = ReadableTypeFromDefinition(Unit); !OwnType.IsEmpty())
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
				if (const FString DependencyType = ReadableTypeFromDefinition(*Definition);
					!DependencyType.IsEmpty())
				{
					return DependencyType;
				}
			}
			return ShortReadableTypeName(Unit.GetSemanticTypeRef());
		}
		FString ReadablePropertyType(const FSemanticUnit::FDeclarationView& Unit, const FAuthoringModel& Model)
		{
			if (Unit.HasOwnTypeDescriptor()) {
				const auto* Definition = Model.FindUnit(Unit.GetUnitId());
				if (Definition) return ReadablePropertyType(*Definition, Model);
			}
			for (const FString& Dependency : Unit.GetDependencyRefs()) if (const auto* Definition = Model.FindUnit(Dependency))
				if (const FString Type = ReadableTypeFromDefinition(*Definition); !Type.IsEmpty()) return Type;
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
				const auto* Type = Definition ? Definition->FindValue(TEXT("type")) : nullptr;
				bool bReflectedType = false;
				if (Type && Type->Get().GetKind() == EUeAgentValueKind::Object)
					for (const auto& Field : Type->Get().GetObjectItems())
						bReflectedType |= Field.Key == TEXT("kind");
				// Native field descriptors reference the same centralized definitions
				// as root properties. Contract-defined business leaves retain their shape.
				if (!bReflectedType) return Value;
				const auto* Nested = Definition->FindValue(TEXT("value_definition"));
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
						const auto* Type = Unit ? Unit->FindValue(TEXT("type")) : nullptr;
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
						const auto* Type = Unit ? Unit->FindValue(TEXT("type")) : nullptr;
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
					if (const auto* Type = Definition->FindValue(TEXT("type"))) ValueType = &Type->Get();
					if (const auto* Value = Definition->FindValue(TEXT("value_definition")))
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
			int32 EncounterOrdinal = INDEX_NONE;
			FString GraphOwnerRef;
			FString UnitId;
			ESemanticUnitKind UnitKind =
				ESemanticUnitKind::PropertySet;
			FString OwnerRef;
			FString SemanticTypeRef;
			FString Role;
			FString FieldSetRef;
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			TOptional<FUeAgentCanonicalValueRef> SinglePropertyValue;
			FSemanticUnit::FSinglePropertyValue::EField SinglePropertyField = FSemanticUnit::FSinglePropertyValue::EField::Value;
			bool bDirectPropertyValue = false;
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

		bool TryFreezeDraftUnit(FUnitDraft& Draft, TOptional<FSemanticUnit>& Out,
			FWorkspaceError& OutError, int32& DirectPropertyUnits)
		{
			Draft.ItemRefs.Sort();
			Draft.RelationRefs.Sort();
			Draft.DependencyRefs.Sort();
			const auto EmplaceValues = [&](auto&& Values) {
			Out.Emplace(
				MoveTemp(Draft.UnitId),
				Draft.UnitKind,
				MoveTemp(Draft.OwnerRef),
				MoveTemp(Draft.SemanticTypeRef),
				MoveTemp(Draft.Role),
				MoveTemp(Draft.FieldSetRef),
				Forward<decltype(Values)>(Values),
				MoveTemp(Draft.ItemRefs),
				MoveTemp(Draft.RelationRefs),
				MoveTemp(Draft.OrderingRef),
				MoveTemp(Draft.LayoutRef),
				MoveTemp(Draft.DependencyRefs),
				MoveTemp(Draft.ProfileVisibility),
				MoveTemp(Draft.SourceProvenance),
				MoveTemp(Draft.TargetIdentityKey),
				MoveTemp(Draft.CategoryPath));
			};
			if (Draft.bDirectPropertyValue) {
				if (!Draft.SinglePropertyValue || !Draft.Values.IsEmpty()) {
					OutError.SetSourceFailure(TEXT("composition_single_property_handoff_invalid"),
						TEXT("The selected property producer must supply exactly its captured value, not an absent fact or a second value map."), Draft.UnitId);
					return false;
				}
				EmplaceValues(FSemanticUnit::FSinglePropertyValue{Draft.SinglePropertyValue.GetValue(), Draft.SinglePropertyField}); ++DirectPropertyUnits;
			} else EmplaceValues(MoveTemp(Draft.Values));
			return true;
		}

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
			// A capture view is synchronous; the sealed entry owner outlives composition.
			const UeAgentAuthoringIdentity::FCanonicalIdentity* CapturedIdentity = nullptr;

			const FString& GetCanonicalKey() const { return CapturedIdentity ? CapturedIdentity->CanonicalKey : CanonicalKey; }
			const FString& GetSelector(const FString& Name) const
			{
				const auto& Source = CapturedIdentity ? CapturedIdentity->Selectors : Selectors;
				const FString* Found = Source.Find(Name);
				static const FString Empty;
				return Found ? *Found : Empty;
			}
		};

		struct FCompositionEntryView
		{
			FString SemanticId;
			FCompositionIdentityView TargetIdentity;
			TSharedPtr<FJsonObject> Type;
			TSharedPtr<FJsonObject> FieldDeclaration;
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
			const UeAgentAuthoringState::FStateEntry* CapturedEntry = nullptr;

			void BindCapturedMetadata(const UeAgentAuthoringState::FStateEntry& Entry)
			{
				CapturedEntry = &Entry;
				TargetIdentity.CapturedIdentity = &Entry.TargetIdentity;
			}
			const FString& GetSemanticId() const { return CapturedEntry ? CapturedEntry->SemanticId : SemanticId; }
			const TMap<FString, FString>& GetConstraints() const { return CapturedEntry ? CapturedEntry->GetConstraints() : Constraints; }
			const TArray<FString>& GetCategoryPath() const { return CapturedEntry ? CapturedEntry->GetCategoryPath() : CategoryPath; }
			const FString& GetProvenance() const { return CapturedEntry ? CapturedEntry->Provenance : Provenance; }
		};

		struct FCompositionInput
		{
			FString TargetKind;
			FString ProfileId;
			FString BaseRevision;
			FString SourceProvenance;
			TArray<FCompositionEntryView> Entries;
			bool bEntriesOrdered = false;
			bool bDirectPropertyValues = false;
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
			const auto& Name = FSemanticUnit::FSinglePropertyValue::GetFieldName(Draft.SinglePropertyField);
			if (Draft.bDirectPropertyValue && Draft.SinglePropertyValue && FStringView(Field).Equals(FStringView(Name), ESearchCase::IgnoreCase))
				return &Draft.SinglePropertyValue.GetValue();
			return Draft.Values.Find(Field);
		}
		bool VisitDraftValues(const FUnitDraft& Draft, TFunctionRef<bool(const FString&, const FUeAgentCanonicalValueRef&)> Visitor) {
			if (Draft.bDirectPropertyValue && Draft.SinglePropertyValue) {
				const auto& Key = FSemanticUnit::FSinglePropertyValue::GetFieldName(Draft.SinglePropertyField);
				return Visitor(Key, Draft.SinglePropertyValue.GetValue());
			}
			for (const auto& Pair : Draft.Values) if (!Visitor(Pair.Key, Pair.Value)) return false;
			return true;
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
			const bool bHasGraph = Drafts.ContainsByPredicate([](const auto& Draft) { return IsGraphStructureDraft(Draft); });
			if (!bHasGraph) {
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_graph_scope drafts=%d graphs_present=false property_scan_rows=0 layout_scan_rows=0 update_value_produced=false"), Drafts.Num());
				return true;
			}
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

		bool TryAllocateKeyedMemberNames(int32 Count, TFunctionRef<const FString&(int32)> GetId,
			TFunctionRef<const FString&(int32)> GetName, const TMap<FString, FString>& BoundAliases,
			TSet<FString>& UsedAliases, TSet<FString>& ReservedAliases, FWorkspaceError& Error,
			TFunctionRef<void(int32, FString)> Store)
		{
			for (int32 Index = 0; Index < Count; ++Index) ReservedAliases.Add(GetName(Index).TrimStartAndEnd().ToLower());
			TMap<FString, int32> Occurrences;
			for (int32 Index = 0; Index < Count; ++Index) {
				FString Alias;
				if (const auto* Bound = BoundAliases.Find(GetId(Index))) Alias = *Bound;
				else if (!TryAllocateReadableKeyedItemAlias(GetName(Index), Occurrences, UsedAliases, Alias, Error, &ReservedAliases)) return false;
				Store(Index, MoveTemp(Alias));
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
			const bool bItemsOwnedByParent = false,
			const FAuthoringModel* AcceptedBindings = nullptr, const FCompositionUnitLookup* KeyedBaseline = nullptr,
			const FKeyedMemberNameLookup* KeyedNames = nullptr)
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
				TSet<FString> UsedAliases;
				TSet<FString> ReservedAliases;
				TMap<FString, FString> NamesByItem;
				TMap<FString, FString> BoundAliases;
				TMap<FString, FString> CapturedAliases;
				const auto AddNames = [&](const FString& Alias, const FString& UnitId, const FString& ActorGuid,
					const FString& ManagedId, const FString& ComponentName) {
					if (!UnitId.IsEmpty()) BoundAliases.Add(UnitId, Alias);
					if (!ActorGuid.IsEmpty()) CapturedAliases.Add(TEXT("actor_guid|") + ActorGuid, Alias);
					if (!ManagedId.IsEmpty()) CapturedAliases.Add(TEXT("managed_id|") + ManagedId, Alias);
					if (!ComponentName.IsEmpty()) CapturedAliases.Add(TEXT("component_name|") + ComponentName, Alias);
					UsedAliases.Add(Alias.TrimStartAndEnd().ToLower()); ReservedAliases.Add(Alias.TrimStartAndEnd().ToLower());
				};
				if (KeyedNames && *KeyedNames) {
					if (!(*KeyedNames)(Root->UnitId, RootField, AddNames, OutError)) return false;
				} else if (AcceptedBindings || (KeyedBaseline && *KeyedBaseline)) {
					const auto* AcceptedRoot = KeyedBaseline && *KeyedBaseline ? (*KeyedBaseline)(Root->UnitId)
						: AcceptedBindings->FindUnit(Root->UnitId);
					const auto* AcceptedRows = AcceptedRoot ? AcceptedRoot->FindValue(RootField) : nullptr;
					if (AcceptedRows && (*AcceptedRows)->GetKind() == EUeAgentValueKind::Object)
						for (const auto& Row : (*AcceptedRows)->GetObjectItems()) {
							if (Row.Value->GetKind() != EUeAgentValueKind::Object) continue;
							const auto* Id = Row.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("unit_id"); });
							if (Id && Id->Value->GetKind() == EUeAgentValueKind::String && !Id->Value->GetText().IsEmpty())
								BoundAliases.Add(Id->Value->GetText(), Row.Key);
							for (const TCHAR* Key : {TEXT("actor_guid"), TEXT("managed_id"), TEXT("component_name")}) {
								const auto* Value = Row.Value->GetObjectItems().FindByPredicate([&](const auto& Field) { return Field.Key == Key; });
								if (Value && Value->Value->GetKind() == EUeAgentValueKind::String && !Value->Value->GetText().IsEmpty())
									CapturedAliases.Add(FString(Key) + TEXT("|") + Value->Value->GetText(), Row.Key);
							}
							// Namespace occupancy is metadata. Unobserved item values are
							// never copied into this current observation.
							UsedAliases.Add(Row.Key.TrimStartAndEnd().ToLower());
							ReservedAliases.Add(Row.Key.TrimStartAndEnd().ToLower());
						}
				}
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
					if (!BoundAliases.Contains(Item->UnitId))
						for (const TCHAR* Key : {TEXT("actor_guid"), TEXT("managed_id"), TEXT("component_name")}) {
							const FString Value = ReadItemOrChildString(Key);
							if (const auto* Alias = Value.IsEmpty() ? nullptr : CapturedAliases.Find(FString(Key) + TEXT("|") + Value)) {
								BoundAliases.Add(Item->UnitId, *Alias); break;
							}
						}
				}
				if (!TryAllocateKeyedMemberNames(Items.Num(), [&](int32 Index) -> const FString& { return Items[Index]->UnitId; },
					[&](int32 Index) -> const FString& { return NamesByItem.FindChecked(Items[Index]->UnitId); },
					BoundAliases, UsedAliases, ReservedAliases, OutError,
					[&](int32 Index, FString Alias) { NamesByItem.FindChecked(Items[Index]->UnitId) = MoveTemp(Alias); })) return false;
				for (const FUnitDraft* Item : Items)
				{
					const FString& Alias = NamesByItem.FindChecked(Item->UnitId);
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
						VisitDraftValues(*Child, [&](const FString& Key, const auto& Value) { Fields.Emplace(Key, Value); return true; });
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

		void Sha256Digest(TConstArrayView<uint8> Bytes, uint8 (&Digest)[SHA256_DIGEST_LENGTH])
		{
			const uint8 Empty = 0;
			SHA256(
				Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest);
		}

		FString Sha256(TConstArrayView<uint8> Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			Sha256Digest(Bytes, Digest);
			TCHAR Hex[SHA256_DIGEST_LENGTH * 2];
			UE::String::BytesToHexLower(MakeArrayView(Digest), Hex);
			FString Result; Result.Reserve(7 + UE_ARRAY_COUNT(Hex));
			Result += TEXT("sha256:"); Result.AppendChars(Hex, UE_ARRAY_COUNT(Hex));
			return Result;
		}

		FString Sha256Text(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			return Sha256(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
		}

		FString StableId(
			const FString& Prefix,
			const FString& Identity)
		{
			const FTCHARToUTF8 Utf8(*Identity);
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			Sha256Digest(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Digest);
			TCHAR Suffix[24];
			UE::String::BytesToHexLower(MakeArrayView(Digest + SHA256_DIGEST_LENGTH - 12, 12), Suffix);
			FString Result; Result.Reserve(Prefix.Len() + 1 + UE_ARRAY_COUNT(Suffix));
			Result += Prefix; Result.AppendChar(TEXT(':')); Result.AppendChars(Suffix, UE_ARRAY_COUNT(Suffix));
			return Result;
		}

		FString SurfaceUnitId(const FString& TargetKind, const FString& SurfaceId)
		{
			return StableId(TEXT("surface"), TargetKind + TEXT("|") + SurfaceId);
		}

#if WITH_DEV_AUTOMATION_TESTS
		IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentCompositionDigestTest,
			"UeAgentInterface.Core.HjsonWorkspace.CompositionDigest",
			EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
		bool FUeAgentCompositionDigestTest::RunTest(const FString&) {
			const TPair<FString, FString> Vectors[] = {
				{TEXT(""), TEXT("sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
				{TEXT("abc"), TEXT("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")},
				{TEXT("中文"), TEXT("sha256:72726d8818f693066ceb69afa364218b692e62ea92b385782363780f47529c21")}};
			for (const auto& Vector : Vectors) {
				TestEqual(TEXT("original full text hash grammar"), Sha256Text(Vector.Key), Vector.Value);
				TestEqual(TEXT("stable ID is exactly original digest suffix"), StableId(TEXT("property"), Vector.Key), TEXT("property:") + Vector.Value.Right(24));
			}
			const FString Embedded(11, TEXT("abc\0ignored"));
			TestEqual(TEXT("oracle actually contains the original explicit-length input"), Embedded.Len(), 11);
			TestEqual(TEXT("oracle CString boundary is actually at the embedded NUL"), FCString::Strlen(*Embedded), 3);
			TestEqual(TEXT("original CString conversion boundary remains unchanged"), Sha256Text(Embedded), Vectors[1].Value);
			TArray<uint8> Bytes; for (int32 I = 0; I < 256; ++I) Bytes.Add(static_cast<uint8>(I));
			TestEqual(TEXT("all binary byte values keep original full digest"), Sha256(Bytes),
				FString(TEXT("sha256:40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880")));
			return true;
		}
#endif
		FString CompositionEntryUnitId(bool bGraph, const FString& TargetKind, const FString& SemanticId, const FString& NativeKey)
		{
			return StableId(bGraph ? TEXT("structure") : TEXT("item"), TargetKind + TEXT("|") + SemanticId + TEXT("|") + NativeKey);
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
			// The caller owns the unchanged JSON graph through this synchronous conversion.
			// One request-local conversion record borrows discovery, first provenance
			// and the built value. It replaces Expanded/Paths/Built, not the native
			// observation or the public codec's JSON representation.
			struct FNode
			{
				const FJsonValue* Value = nullptr;
				FUeAgentCanonicalValuePtr Built;
				int32 ParentLink = INDEX_NONE;
				int32 FirstChild = 0, NumChildren = 0;
			};
			struct FChild
			{
				const FString* Key = nullptr;
				int32 Parent = INDEX_NONE, ArrayIndex = INDEX_NONE, Node = INDEX_NONE;
			};
			struct FVisit
			{
				const FJsonValue* Value = nullptr;
				int32 Link = INDEX_NONE, ExpandedNode = INDEX_NONE;
			};
			TArray<FNode, TInlineAllocator<8>> Nodes;
			TArray<FChild, TInlineAllocator<8>> Children;
			TArray<FVisit, TInlineAllocator<8>> Stack;
			TArray<int32, TInlineAllocator<8>> Order;
			TMap<const FJsonValue*, int32> Discovered;
			const auto PathForLink = [&](int32 Link)
			{
				TArray<int32, TInlineAllocator<8>> Parts;
				for (; Link != INDEX_NONE; Link = Nodes[Children[Link].Parent].ParentLink) Parts.Add(Link);
				FString Path;
				for (int32 Part = Parts.Num() - 1; Part >= 0; --Part) {
					const auto& Child = Children[Parts[Part]];
					Path += TEXT("/");
					Path += Child.Key ? EscapeJsonPointerSegment(*Child.Key) : FString::FromInt(Child.ArrayIndex);
				}
				return Path;
			};
			Stack.Add({Root.Get()});
			while (!Stack.IsEmpty())
			{
				FVisit Visit = Stack.Pop(EAllowShrinking::No);
				if (Visit.ExpandedNode != INDEX_NONE) {
					Order.Add(Visit.ExpandedNode);
					continue;
				}
				if (!Visit.Value) {
					OutError.SetSourceFailure(TEXT("composition_json_value_invalid"), TEXT("The composition contains a missing child value."), FString(), PathForLink(Visit.Link));
					return false;
				}
				if (const auto* Existing = Discovered.Find(Visit.Value)) {
					if (Visit.Link != INDEX_NONE) Children[Visit.Link].Node = *Existing;
					continue;
				}
				const int32 NodeIndex = Nodes.Add({Visit.Value, nullptr, Visit.Link, Children.Num(), 0});
				Discovered.Add(Visit.Value, NodeIndex);
				if (Visit.Link != INDEX_NONE) Children[Visit.Link].Node = NodeIndex;
				Stack.Add({nullptr, INDEX_NONE, NodeIndex});
				const int32 FirstChild = Children.Num();
				if (Visit.Value->Type == EJson::Array) {
					const auto& Items = Visit.Value->AsArray();
					for (int32 Index = 0; Index < Items.Num(); ++Index)
						Children.Add({nullptr, NodeIndex, Index});
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
						Stack.Add({Items[Index].Get(), FirstChild + Index});
				} else if (Visit.Value->Type == EJson::Object) {
					const auto& Object = Visit.Value->AsObject();
					if (!Object.IsValid()) {
						OutError.SetSourceFailure(TEXT("composition_json_object_invalid"), TEXT("A composition value declares object type but has no object storage."), FString(), PathForLink(Visit.Link));
						return false;
					}
					TArray<const TPair<FString, TSharedPtr<FJsonValue>>*, TInlineAllocator<8>> Fields;
					for (const auto& Field : Object->Values) Fields.Add(&Field);
					Fields.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
					for (const auto* Field : Fields) Children.Add({&Field->Key, NodeIndex});
					for (int32 Index = Fields.Num() - 1; Index >= 0; --Index)
						Stack.Add({Fields[Index]->Value.Get(), FirstChild + Index});
				}
				Nodes[NodeIndex].NumChildren = Children.Num() - FirstChild;
			}
			// Preserve the original two-phase error priority: all missing children
			// and object storage are checked before scalar/canonical construction.
			for (int32 NodeIndex : Order)
			{
				auto& Node = Nodes[NodeIndex];
				const auto& Value = Node.Value;
				FUeAgentCanonicalValuePtr Canonical;
				FUeAgentStateSemanticError SemanticError;
				bool bBuilt = false;
				switch (Value->Type)
				{
				case EJson::Null:
				case EJson::None:
					bBuilt = FUeAgentCanonicalValue::TryNull(Resources, Canonical, SemanticError);
					break;
				case EJson::Boolean:
					bBuilt = FUeAgentCanonicalValue::TryBoolean(Value->AsBool(), Resources, Canonical, SemanticError);
					break;
				case EJson::String:
					bBuilt = FUeAgentCanonicalValue::TryString(Value->AsString(), Resources, Canonical, SemanticError);
					break;
				case EJson::Number:
				{
					Contract::FDiagnostic NumericError;
					if (!CanonicalJson::TryFromJsonScalarValue(*Value, Resources, Canonical, NumericError)) {
						OutError.SetFailure(NumericError, FString(), PathForLink(Node.ParentLink));
						return false;
					}
					bBuilt = true;
					break;
				}
				case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (int32 Index = 0; Index < Node.NumChildren; ++Index) {
						const auto& Child = Children[Node.FirstChild + Index];
						const auto& Built = Nodes[Child.Node].Built;
						if (!Built) {
							OutError.SetSourceFailure(TEXT("composition_json_child_missing"), TEXT("Array child was not converted before its parent; the input may contain a cyclic reference."),
								FString(), PathForLink(Node.ParentLink) + TEXT("/") + FString::FromInt(Index));
							return false;
						}
						Items.Add(Built.ToSharedRef());
					}
					bBuilt = FUeAgentCanonicalValue::TryArray(Items, Resources, Canonical, SemanticError);
					break;
				}
				case EJson::Object:
				{
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
					for (int32 Index = 0; Index < Node.NumChildren; ++Index) {
						const auto& Child = Children[Node.FirstChild + Index];
						const auto& Built = Nodes[Child.Node].Built;
						if (!Built) {
							OutError.SetSourceFailure(TEXT("composition_json_child_missing"), TEXT("Object child was not converted before its parent; the input may contain a cyclic reference."),
								FString(), PathForLink(Node.ParentLink) + TEXT("/") + EscapeJsonPointerSegment(*Child.Key));
							return false;
						}
						Items.Emplace(*Child.Key, Built.ToSharedRef());
					}
					bBuilt = FUeAgentCanonicalValue::TryObject(Items, Resources, Canonical, SemanticError);
					break;
				}
				default:
					break;
				}
				if (!bBuilt || !Canonical) {
					OutError.SetFailure(SemanticError.Code.IsEmpty()
						? Contract::FDiagnostic::Failure(TEXT("composition_canonical_value_failed"), TEXT("The composition value could not be represented as a canonical value."), TEXT("authoring_composition"))
						: SemanticError, FString(), PathForLink(Node.ParentLink));
					return false;
				}
				Node.Built = MoveTemp(Canonical);
			}
			if (Nodes.IsEmpty() || !Nodes[0].Built) {
				OutError.SetSourceFailure(TEXT("composition_canonical_root_missing"), TEXT("Canonical conversion completed without producing its root value."));
				return false;
			}
			OutValue = MoveTemp(Nodes[0].Built);
			return true;
		}

		bool TryProjectCanonicalAuthoringDescriptor(const FUeAgentStateEntry& Entry,
			FCompositionEntryView& View, FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
		{
			if (!Entry.GetAuthoringDescriptor().IsValid()) return true;
			TSharedPtr<FJsonValue> Json, NativeValue; UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryToAuthoringDeclarationJson(*Entry.GetAuthoringDescriptor(), Json, Error)
				|| !Json.IsValid() || Json->Type != EJson::Object)
			{ OutError.SetFailure(Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_descriptor_invalid"), TEXT("The authoring descriptor must convert to a JSON object."), TEXT("authoring_composition")) : Error, Entry.GetIdentity().GetCanonicalKey()); return false; }
			const auto Descriptor = Json->AsObject(); const TSharedPtr<FJsonObject>* Type = nullptr;
			if (!Descriptor->TryGetObjectField(TEXT("type"), Type) || !Type || !Type->IsValid())
			{ OutError.SetSourceFailure(TEXT("authoring_descriptor_type_missing"), TEXT("The authoring descriptor requires an object type definition."), Entry.GetIdentity().GetCanonicalKey(), TEXT("/type")); return false; }
			View.Type = *Type;
			View.CandidatesHeader = Descriptor->TryGetField(TEXT("candidates_header"));
			View.Candidates = Descriptor->TryGetField(TEXT("candidates"));
			View.ValueDefinition = Descriptor->TryGetField(TEXT("value_definition"));
			if (!CanonicalJson::TryToAuthoringCodecJson(Entry.GetValue().Get(), NativeValue, Error))
			{ OutError.SetFailure(Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_descriptor_value_invalid"), TEXT("The captured property value could not be represented as JSON."), TEXT("authoring_composition")) : Error, Entry.GetIdentity().GetCanonicalKey()); return false; }
			TSharedPtr<FJsonValue> Projected;
			View.bAuthoringVisible &= UeAgentPropertyValueCodec::ProjectAuthoringValue(
				View.Type, NativeValue, View.ValueDefinition, Projected, View.ValueDefinition);
			return !Projected.IsValid() || Projected == NativeValue
				|| TryCanonicalizeJson(Projected, Resources, View.Value, OutError);
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

		void CollectDirectDependencyPath(const FString& Key, const FUeAgentCanonicalValue& Value, TArray<FString>& Paths)
		{
			if (IsCompositionDependencyPathField(Key)
				&& Value.GetKind() == EUeAgentValueKind::String && Value.GetText().StartsWith(TEXT("/")))
				Paths.AddUnique(Value.GetText());
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

		const FString& Selector(
			const FCompositionEntryView& Entry,
			const TCHAR* Name)
		{
			return Entry.TargetIdentity.GetSelector(Name);
		}
		const FString& Selector(const FCompositionEntryView& Entry, const FString& Name)
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

		struct FItemSelectorParts {
			const FString* Scope = nullptr;
			const FString* Name = nullptr;
			const FString* First = nullptr;
			const FString* Component = nullptr;
			const FString* GraphName = nullptr;
			static bool SameText(const FString* A, const FString* B) {
				return A == B || (A && B && A->Len() == B->Len()
					&& FMemory::Memcmp(**A, **B, A->Len() * sizeof(TCHAR)) == 0);
			}
			bool Matches(const FItemSelectorParts& Other) const {
				return SameText(Scope, Other.Scope) && SameText(Name, Other.Name)
					&& SameText(First, Other.First) && SameText(Component, Other.Component)
					&& SameText(GraphName, Other.GraphName);
			}
			FString Build() const {
				if (!Scope) return *First;
				FString Key; Key.Reserve(Scope->Len() + 2 + Name->Len() + First->Len()
					+ (Component ? 16 + Component->Len() : 0) + (GraphName ? 24 + GraphName->Len() : 0));
				Key += *Scope;
				if (GraphName) {
					Key += TEXT("|graph_name["); Key.AppendInt(GraphName->Len());
					Key += TEXT("]="); Key += *GraphName;
				}
				Key.AppendChar(TEXT('|')); Key += *Name; Key.AppendChar(TEXT('=')); Key += *First;
				if (Component) { Key += TEXT("|component_name="); Key += *Component; }
				return Key;
			}
		};

		FItemSelectorParts DescribeItemSelectorKey(const FCompositionEntryView& Entry)
		{
			// These are the original fixed lookup names, not remembered results.
			struct FNames {
				const FString Asset = TEXT("asset_path"), World = TEXT("world_path"),
					Actor = TEXT("owner_actor_guid"), Managed = TEXT("owner_managed_id"), Component = TEXT("component_name"),
					NodeId = TEXT("node_id"), GraphName = TEXT("graph_name");
				const TArray<FString> Ordered{TEXT("dynamic_input_guid"), TEXT("input_name"), TEXT("node_guid"), NodeId,
					TEXT("expression_guid"), TEXT("component_guid"), TEXT("component_name"), TEXT("widget_name"), TEXT("member_guid"),
					TEXT("member_name"), TEXT("key_name"), TEXT("renderer_name"), TEXT("script_usage_id"), TEXT("module_guid"),
					TEXT("usage_id"), TEXT("emitter_handle_id"), TEXT("emitter_name"), TEXT("actor_guid"), TEXT("managed_id"), TEXT("managed_actor_id")};
			};
			static const FNames Names;
			const auto& Asset = Selector(Entry, Names.Asset);
			const FString& Scope = Asset.IsEmpty() ? Selector(Entry, Names.World) : Asset;
			const FString& OwnerActorGuid = Selector(Entry, Names.Actor);
			const FString& OwnerManagedId = Selector(Entry, Names.Managed);
			const FString& ComponentName = Selector(Entry, Names.Component);
			if (!ComponentName.IsEmpty()
				&& (!OwnerActorGuid.IsEmpty() || !OwnerManagedId.IsEmpty()))
			{
				const FString& Owner = !OwnerActorGuid.IsEmpty() ? OwnerActorGuid : OwnerManagedId;
				return {&Scope, !OwnerActorGuid.IsEmpty() ? &Names.Actor : &Names.Managed, &Owner, &ComponentName};
			}
			for (const FString& Name : Names.Ordered)
			{
				const FString& Value = Selector(Entry, Name);
				if (!Value.IsEmpty())
				{
					if (Name == Names.NodeId) {
						const FString& Graph = Selector(Entry, Names.GraphName);
						if (!Graph.IsEmpty()) return {&Scope, &Name, &Value, nullptr, &Graph};
					}
					return {&Scope, &Name, &Value, nullptr};
				}
			}
			return {nullptr, nullptr, &Entry.TargetIdentity.GetCanonicalKey(), nullptr};
		}
		FString ItemSelectorKey(const FCompositionEntryView& Entry) { return DescribeItemSelectorKey(Entry).Build(); }

		bool IsTargetSemantic(const FString& SemanticId)
		{
			return SemanticId.Contains(TEXT(".export.target"))
					|| SemanticId.Contains(TEXT(".export.function_target"));
		}

		const FCompositionSemanticRouteContract* SelectDeclaredSemanticRoute(
			const FTargetCompositionContract& ContractValue, const FString& SemanticId)
		{
			const FCompositionSemanticRouteContract* Selected = nullptr;
			for (const auto& Candidate : ContractValue.SemanticRoutes) {
				bool bMatches = false;
				switch (Candidate.Match) {
				case ECompositionSemanticRouteMatch::Exact: bMatches = SemanticId == Candidate.Pattern; break;
				case ECompositionSemanticRouteMatch::Suffix: bMatches = SemanticId.EndsWith(Candidate.Pattern); break;
				case ECompositionSemanticRouteMatch::Contains: bMatches = SemanticId.Contains(Candidate.Pattern); break;
				case ECompositionSemanticRouteMatch::Default: bMatches = true; break;
				default: break;
				}
				if (bMatches && (!Selected || Candidate.Priority > Selected->Priority)) Selected = &Candidate;
			}
			return Selected;
		}

		bool TryResolveDeclaredSemanticRoute(
			const FString& TargetKind,
			const FString& SemanticId,
			FEntryRoute& OutRoute, const FTargetCompositionContract* BoundContract)
		{
			FCompositionSemanticRouteContract Resolved;
			const auto* Selected = BoundContract ? SelectDeclaredSemanticRoute(*BoundContract, SemanticId) : nullptr;
			if (!BoundContract) {
				if (!TryResolveTargetCompositionSemanticRoute(TargetKind, SemanticId, Resolved)) return false;
				Selected = &Resolved;
			}
			if (!Selected) return false;
			OutRoute = {};
			OutRoute.SurfaceId = Selected->SurfaceId;
			OutRoute.UnitKind = Selected->UnitKind;
			switch (Selected->Role)
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
			const FString& PreviewTarget = FString(), const FTargetCompositionContract* BoundContract = nullptr)
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
					Route, BoundContract))
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

	bool FTargetCompositionRegistry::ResolveSemanticRoute(const FString& TargetKind,
		const FString& SemanticId, FCompositionSemanticRouteContract& OutRoute) const
	{
		OutRoute = {};
		FReadScopeLock Lock(RegistryLock);
		const auto* ContractValue = Contracts.Find(Normalize(TargetKind));
		const auto* Selected = ContractValue ? SelectDeclaredSemanticRoute(*ContractValue, SemanticId) : nullptr;
		if (!Selected) return false;
		OutRoute = *Selected;
		return true;
	}

	bool TryResolveTargetCompositionSemanticRoute(
		const FString& TargetKind, const FString& SemanticId, FCompositionSemanticRouteContract& OutRoute)
	{
		return FTargetCompositionRegistry::Get().ResolveSemanticRoute(TargetKind, SemanticId, OutRoute);
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

	bool IsTargetCompositionIdentitySemantic(const FString& TargetKind, const FString& SemanticId)
	{
		return RouteEntry(TargetKind, SemanticId).bTargetValues;
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

	bool IsCompositionAssetDependencies(const FSemanticUnit& Unit, const FString& OwnerId)
	{
		return Unit.GetUnitKind() == ESemanticUnitKind::DependencyState
			&& Unit.GetSemanticTypeRef() == TEXT("uai.authoring.dependencies/1") && Unit.GetRole() == TEXT("asset_dependencies")
			&& Unit.GetOwnerRef() == OwnerId && Unit.GetUnitId() == StableId(TEXT("dependencies"), OwnerId)
			&& Unit.GetValueCount() == 1 && Unit.HasValue(TEXT("asset_paths"));
	}

	bool TryCollectCompositionDependencyPaths(const FSemanticUnit& Target,
		TConstArrayView<FSemanticUnit> Units, TArray<FString>& OutPaths)
	{
		OutPaths.Reset();
		const bool bLevel = Target.GetSemanticTypeRef() == TEXT("ue.level.asset");
		const bool bBlueprint = Target.GetSemanticTypeRef() == TEXT("ue.blueprint.asset");
		if (Target.GetUnitKind() != ESemanticUnitKind::TargetState || (!bLevel && !bBlueprint)) return false;
		for (const auto& Unit : Units) {
			const bool bTarget = Unit.GetUnitId() == Target.GetUnitId();
			const bool bItem = Unit.GetUnitKind() == ESemanticUnitKind::ItemState && (bBlueprint
				|| Unit.GetSemanticTypeRef().EndsWith(TEXT(".actor.target"))
				|| Unit.GetSemanticTypeRef().EndsWith(TEXT(".component.target")));
			// Ordinary Blueprint property and item composition retains the original
			// top-level codec fields (including struct_path). Graph/edge envelopes
			// carry nested graph/from/to objects, never direct dependency paths.
			if (!bTarget && !bItem && !(bBlueprint && Unit.GetUnitKind() == ESemanticUnitKind::PropertySet)) continue;
			for (const auto& Field : Unit.GetValues()) CollectDirectDependencyPath(Field.Key, Field.Value.Get(), OutPaths);
		}
		OutPaths.Sort();
		return true;
	}

	static bool TryComposeEntryViews(
		FCompositionInput&& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError,
		TMap<FString, FDocumentValueBinding>* DocumentBindings = nullptr,
		FCaptureCharges* OutPreparationCharges = nullptr,
		const FDefinitionProjectionHints* ProjectionHints = nullptr,
			FCompositionUnitSet* OutUnits = nullptr,
			const FAuthoringModel* AcceptedBindings = nullptr, const FCompositionUnitLookup* KeyedBaseline = nullptr,
			const FKeyedMemberNameLookup* KeyedNames = nullptr, bool bBuildLegacyFingerprint = true)
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
			MoveTemp(Request.Entries);
		int32 SharedTypeCount = 0, UniqueTypeCount = 0;
		TSet<FString> PreviewSurfaces;
		for (const auto& Entry : Entries)
		{
			if (Entry.bAvailable && Entry.bAuthoringVisible && Entry.Type)
				Entry.FieldDeclaration ? ++SharedTypeCount : ++UniqueTypeCount;
			if (!AssetPreview::IsPreviewSemantic(Entry.GetSemanticId())) continue;
			const FString Surface = RouteEntry(Request.TargetKind, Entry.GetSemanticId(), Selector(Entry, TEXT("preview_target")), &CompositionContract).SurfaceId;
			if (PreviewSurfaces.Contains(Surface)) continue;
			PreviewSurfaces.Add(Surface);
		}
		if (!Request.bEntriesOrdered) Entries.Sort(
			[](const FCompositionEntryView& Left,
				const FCompositionEntryView& Right)
			{
				if (Left.GetSemanticId() != Right.GetSemanticId())
				{
					return Left.GetSemanticId() < Right.GetSemanticId();
				}
				return Left.TargetIdentity.GetCanonicalKey()
					< Right.TargetIdentity.GetCanonicalKey();
			});

		// Entries are ordered by semantic/identity. A semantic route belongs to
		// that declaration and preview surface, not to every instance value or
		// every later traversal. Borrow the contract already owned by this call.
		FString RouteSemantic, RoutePreview;
		FEntryRoute GroupRoute; bool bHaveGroupRoute = false;
		int32 RouteSelections = 0;
		const auto ResolveEntryRoute = [&](const FCompositionEntryView& Entry) -> const FEntryRoute& {
			const auto& Semantic = Entry.GetSemanticId();
			const auto& Preview = Selector(Entry, TEXT("preview_target"));
			if (!bHaveGroupRoute || RouteSemantic != Semantic || RoutePreview != Preview) {
				GroupRoute = RouteEntry(Request.TargetKind, Semantic, Preview, &CompositionContract);
				RouteSemantic = Semantic; RoutePreview = Preview; bHaveGroupRoute = true; ++RouteSelections;
			}
			return GroupRoute;
		};
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
		struct FFrozenPropertyUnit {
			int32 Ordinal = INDEX_NONE; TOptional<FSemanticUnit> Unit;
			TSharedPtr<FWorkspaceError> Error;
		};
		TArray<FFrozenPropertyUnit> FrozenProperties;
		int32 NextDraftOrdinal = 0, DirectPropertyUnits = 0;
		auto AddDraft =
			[&Drafts, &DraftIndex, &NextDraftOrdinal](FUnitDraft Draft) -> FUnitDraft&
			{
				Draft.EncounterOrdinal = NextDraftOrdinal++;
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
			Root.UnitId = SurfaceUnitId(Request.TargetKind, SurfaceContract.SurfaceId);
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
				return Entry.TargetIdentity.GetCanonicalKey();
			};

		TFunction<bool(const TSharedPtr<FJsonValue>&, TSharedPtr<FJsonValue>&, TArray<FString>&)> InternValueDefinition;
		FString ValueDefinitionSourceUnit, ValueDefinitionPointer;
		const auto InternDefinitionAt = [&](const TSharedPtr<FJsonValue>& Input,
			TSharedPtr<FJsonValue>& Output, TArray<FString>& Dependencies,
			const FString& SourceUnit, const FString& Pointer) {
			const FString PreviousSource = ValueDefinitionSourceUnit;
			const FString PreviousPointer = ValueDefinitionPointer;
			ValueDefinitionSourceUnit = SourceUnit;
			ValueDefinitionPointer = Pointer;
			const bool bResult = InternValueDefinition(Input, Output, Dependencies);
			ValueDefinitionSourceUnit = PreviousSource;
			ValueDefinitionPointer = PreviousPointer;
			return bResult;
		};
		const auto InternDefinitionChild = [&](const TSharedPtr<FJsonValue>& Input,
			TSharedPtr<FJsonValue>& Output, TArray<FString>& Dependencies, const FString& Suffix) {
			if (ValueDefinitionPointer.IsEmpty()) return InternValueDefinition(Input, Output, Dependencies);
			return InternDefinitionAt(Input, Output, Dependencies, ValueDefinitionSourceUnit,
				ValueDefinitionPointer + Suffix);
		};
		const auto FailValueDefinition = [&](const FString& Message, const FString& Field,
			const TSharedPtr<FJsonValue>& Input) {
			const FString Pointer = ValueDefinitionPointer + Field;
			if (Request.TargetKind != TEXT("ue.level_sequence.asset") || ValueDefinitionPointer.IsEmpty()) {
				OutError.SetSourceFailure(TEXT("composition_value_definition_invalid"), Message,
					ValueDefinitionSourceUnit, Pointer);
				return false;
			}
			// Describe only this failing native definition's shape. Preserve its
			// binding/track/property address without dumping the captured object.
			auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("composition_value_definition_invalid"),
				Message, TEXT("workspace_resolve"));
			Diagnostic.SourceKind = TEXT("native_result");
			const auto Shape = MakeShared<FJsonObject>();
			Shape->SetBoolField(TEXT("is_object"), Input && Input->Type == EJson::Object);
			if (Input && Input->Type == EJson::Object) {
				TArray<FString> Names; Input->AsObject()->Values.GetKeys(Names); Names.Sort();
				TArray<TSharedPtr<FJsonValue>> Fields;
				for (const auto& Name : Names) Fields.Add(MakeShared<FJsonValueString>(Name));
				Shape->SetArrayField(TEXT("field_names"), MoveTemp(Fields));
				if (const auto Type = Input->AsObject()->TryGetField(TEXT("type"))) Shape->SetField(TEXT("type"), Type);
			}
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Shape);
			Diagnostic.ActualValuePresent = true;
			OutError.SetFailure(MoveTemp(Diagnostic), ValueDefinitionSourceUnit, Pointer);
			return false;
		};
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
		struct FDefinitionInputProof
		{
			TSharedPtr<FJsonObject> FieldDeclaration;
			TSharedPtr<FJsonValue> ValueDefinition;
			bool bEditable = false;
			FString DefinitionId;
		};
		TMap<uint32, TArray<FDefinitionInputProof>> DefinitionInputs;
		int32 DefinitionInputCount = 0, ReusedDefinitionInputs = 0, UnsharedDefinitionInputs = 0;
		TFunction<bool(const FCompositionEntryView&, FString&)> EnsurePropertyDefinition;
		InternValueDefinition = [&](const TSharedPtr<FJsonValue>& Input,
			TSharedPtr<FJsonValue>& Output, TArray<FString>& Dependencies) -> bool
		{
			if (!Input.IsValid() || Input->Type != EJson::Object)
			{
				return FailValueDefinition(TEXT("A value definition must be an object."), FString(), Input);
			}
			const auto Object = Input->AsObject();
			const TSharedPtr<FJsonObject>* NativeType = nullptr;
			if (Object->TryGetObjectField(TEXT("type"), NativeType))
			{
				FCompositionEntryView FieldDescriptor;
				FieldDescriptor.Type = *NativeType;
				// This original nested descriptor is immutable throughout composition,
				// just like a reflected declaration. Its instance contribution remains
				// an independent input below; do not manufacture another declaration.
				FieldDescriptor.FieldDeclaration = Object;
				FieldDescriptor.bEditable = true;
				// Re-interning a complete nested definition must preserve its contract.
				// Codec-only descriptors may omit these fields; their defaults stay unchanged.
				if (Object->HasField(TEXT("editable"))
					&& !Object->TryGetBoolField(TEXT("editable"), FieldDescriptor.bEditable))
				{ return FailValueDefinition(TEXT("Value-definition editable must be a boolean."), TEXT("/editable"), Input); }
				const TSharedPtr<FJsonObject>* Constraints = nullptr;
				if (Object->HasField(TEXT("constraints")))
				{
					if (!Object->TryGetObjectField(TEXT("constraints"), Constraints))
					{ return FailValueDefinition(TEXT("Value-definition constraints must be an object."), TEXT("/constraints"), Input); }
					for (const auto& Constraint : (*Constraints)->Values)
					{
						FString Value;
						if (!Constraint.Value || !Constraint.Value->TryGetString(Value))
						{ return FailValueDefinition(FString::Printf(TEXT("Constraint '%s' must be a string."), *Constraint.Key), TEXT("/constraints/") + EscapeJsonPointerSegment(Constraint.Key), Input); }
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
				for (int32 Index = 0; Index < Items->Num(); ++Index)
				{
					const auto& Item = (*Items)[Index];
					if (Item.IsValid() && Item->Type == EJson::Null) { ProjectedItems.Add(Item); continue; }
					TSharedPtr<FJsonValue> Projected;
					if (!InternDefinitionChild(Item, Projected, Dependencies, TEXT("/items/") + FString::FromInt(Index))) return false;
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
					if (!InternDefinitionChild(Element, Projected, Dependencies, TEXT("/element_definition"))) return false;
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
					if (!InternDefinitionChild(Field.Value, Projected, Dependencies, TEXT("/fields/") + EscapeJsonPointerSegment(Field.Key))) return false;
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
				return FailValueDefinition(TypeName.IsEmpty()
					? TEXT("Value definition requires a nonempty string type.") : TEXT("Value definition requires an object contract."), TypeName.IsEmpty() ? TEXT("/type") : TEXT("/contract"), Input);
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
			// The reflection owner already produced the immutable declaration once.
			// Instance value definitions remain a separate comparison input.
			TMap<FString, FString> Constraints;
			if (!Entry.FieldDeclaration) {
				Constraints = Entry.GetConstraints();
				NormalizePropertyConstraints(Constraints);
			}
			const uint32 InputHash = Entry.FieldDeclaration
				? HashCombine(PointerHash(Entry.FieldDeclaration.Get()), GetTypeHash(Entry.bEditable)) : 0;
			if (const auto* Proofs = Entry.FieldDeclaration ? DefinitionInputs.Find(InputHash) : nullptr)
				for (const auto& Proof : *Proofs)
					if (Proof.bEditable == Entry.bEditable && Proof.FieldDeclaration == Entry.FieldDeclaration
						&& IdenticalDefinitionInput(Proof.ValueDefinition, Entry.ValueDefinition)) {
						OutDefinitionId = Proof.DefinitionId; ++ReusedDefinitionInputs; return true;
					}
			if (Entry.FieldDeclaration) {
				Constraints = Entry.GetConstraints();
				NormalizePropertyConstraints(Constraints);
			}
			const auto RememberDefinitionInput = [&]() {
				// A freshly produced per-entry TypeJSON has no later pointer-sharing
				// consumer. Do not copy it, constraints or candidates into an unused
				// proof. DefinitionUnits still interns its real normalized signature.
				if (!Entry.FieldDeclaration) { ++UnsharedDefinitionInputs; return; }
				if (DefinitionInputCount >= 4096) return;
				auto& Proofs = DefinitionInputs.FindOrAdd(InputHash);
				if (Proofs.Num() >= 32) return;
				// A successful result has already interned its entire recursive
				// definition closure in this call. Repeated inputs create no new
				// canonical values/charges, and the input objects are never mutated.
				Proofs.Add({Entry.FieldDeclaration, Entry.ValueDefinition, Entry.bEditable, OutDefinitionId});
				++DefinitionInputCount;
			};
			TSharedPtr<FJsonValue> ProjectedValueDefinition;
			TArray<FString> ValueDefinitionDependencies;
			if (Entry.ValueDefinition.IsValid()) {
				const bool bInterned = ValueDefinitionPointer.IsEmpty() && Request.TargetKind == TEXT("ue.level_sequence.asset")
					? InternDefinitionAt(Entry.ValueDefinition, ProjectedValueDefinition, ValueDefinitionDependencies,
						Entry.GetSemanticId(), TEXT("/value_definition"))
					: InternDefinitionChild(Entry.ValueDefinition, ProjectedValueDefinition, ValueDefinitionDependencies, TEXT("/value_definition"));
				if (!bInterned) return false;
			}
			const auto Identity = MakeShared<FJsonObject>();
			Identity->SetObjectField(TEXT("type"), Entry.Type);
			Identity->SetBoolField(TEXT("editable"), Entry.bEditable);
			const auto IdentityConstraints = MakeShared<FJsonObject>();
			TArray<FString> Keys;
			Constraints.GetKeys(Keys); Keys.Sort();
			for (const FString& Key : Keys)
			{
				IdentityConstraints->SetStringField(Key, Constraints.FindChecked(Key));
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
				OutDefinitionId = *Existing; RememberDefinitionInput(); return true;
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
			if (!Constraints.IsEmpty())
			{
				TSharedRef<FJsonObject> ConstraintObject = MakeShared<FJsonObject>();
				for (const FString& Key : Keys)
				{
					ConstraintObject->SetStringField(Key, Constraints.FindChecked(Key));
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
			RememberDefinitionInput();
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
				DocumentSourceKey(Entry.GetSemanticId(), Entry.TargetIdentity.GetCanonicalKey()), Address, bWholeValue});
			return true;
		};

		const auto GetDocumentCollectionContract = [](
			const FString& TargetKind,
			const FCompositionEntryView& Entry,
			const FDocumentVisit& Visit) -> const FKeyedItemProjectionContract*
		{
			if (TargetKind == TEXT("ue.control_rig.asset")
				&& Entry.GetSemanticId() == TEXT("uai.semantic.rigging.control_rig.document"))
			{
				const FString RootField = Visit.Path == TEXT("hierarchy/elements")
					? TEXT("rig_elements")
					: Visit.Path == TEXT("graphs/graphs")
						? TEXT("rig_graphs") : FString();
				for (const auto& Contract : GetKeyedItemProjectionContracts())
					if (Contract.RootField == RootField) return &Contract;
			}
			if (TargetKind == TEXT("ue.ik_rig.asset")
				&& Entry.GetSemanticId() == TEXT("uai.semantic.rigging.ik_rig.document")
				&& (Visit.Path == TEXT("goals") || Visit.Path == TEXT("solvers") || Visit.Path == TEXT("chains")))
			{
				for (const auto& Contract : GetKeyedItemProjectionContracts())
					if (Contract.RootField == TEXT("ik_") + Visit.Path) return &Contract;
			}
			if (TargetKind == TEXT("ue.ik_retargeter.asset")
				&& Entry.GetSemanticId() == TEXT("uai.semantic.rigging.ik_retargeter.document")
				&& (Visit.Path == TEXT("ops") || Visit.Path == TEXT("mappings") || Visit.Path == TEXT("poses")))
			{
				for (const auto& Contract : GetKeyedItemProjectionContracts())
					if (Contract.RootField == TEXT("ik_retarget_") + Visit.Path) return &Contract;
			}
			return FindLevelSequenceCollectionContract(TargetKind, Entry.GetSemanticId(), Visit.SurfaceId, Visit.Path);
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
						+ Entry.GetSemanticId() + TEXT("|")
						+ Entry.TargetIdentity.GetCanonicalKey() + TEXT("|")
						+ Path);
				Property.UnitKind = ESemanticUnitKind::PropertySet;
				Property.OwnerRef = OwnerId;
				Property.SemanticTypeRef =
					Entry.GetSemanticId() + TEXT("#") + Path;
				Property.Role = SurfaceId;
				Property.FieldSetRef = Path;
			Property.TargetIdentityKey =
					BindingForOwner(OwnerId, Entry);
			Property.SourceProvenance =
					Entry.GetProvenance().IsEmpty()
						? Provenance
						: Entry.GetProvenance();
			if (!DefinitionId.IsEmpty()) Property.DependencyRefs.Add(DefinitionId);
			if (!Value.IsValid())
				{
					OutError.SetSourceFailure(TEXT("composition_document_value_invalid"), TEXT("The document property has no captured value."), Property.UnitId, DocumentAddressPointer(Address));
					return false;
				}
				Property.bDirectPropertyValue = Request.bDirectPropertyValues;
				if (Property.bDirectPropertyValue) Property.SinglePropertyValue.Emplace(Value.ToSharedRef());
				else Property.Values.Add(TEXT("value"), Value.ToSharedRef());
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
						const auto* ValueFields = CompositionContract.DocumentValueFields.Find(Entry.GetSemanticId());
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
									if (const auto* Target = FindLevelSequenceCollectionContract(Request.TargetKind, Entry.GetSemanticId(), Visit.SurfaceId, Root.Key))
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
								Request.TargetKind + TEXT("|") + Entry.GetSemanticId()
									+ TEXT("|") + Entry.TargetIdentity.GetCanonicalKey()
									+ TEXT("|") + Visit.Path);
							Collection.UnitKind = ESemanticUnitKind::StructureState;
							Collection.OwnerRef = Visit.OwnerId;
							Collection.SemanticTypeRef = Entry.GetSemanticId() + TEXT("#") + Visit.Path;
							Collection.Role = Visit.SurfaceId;
							Collection.TargetIdentityKey = BindingForOwner(Visit.OwnerId, Entry);
							Collection.SourceProvenance = Entry.GetProvenance().IsEmpty()
								? Provenance : Entry.GetProvenance();
							Collection.Values.Add(Contract.RootField, VisibleKeys.ToSharedRef());
							// One template/definition owner serves root tracks and tracks nested
							// under captured bindings. The binding alias is supplied by keyed projection.
							auto InternRecordProperties = [&](const FUeAgentCanonicalValue& Record, const FString& Field,
								FUnitDraft& DefinitionCollection, TSharedPtr<FJsonObject>& Out, const FString& RecordPointer) {
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
                                const FString Pointer = RecordPointer + (Field.IsEmpty() ? FString() : TEXT("/") + EscapeJsonPointerSegment(Field))
                                    + TEXT("/") + EscapeJsonPointerSegment(Definition.Key);
                                if (!InternDefinitionAt(Json, Reference, DefinitionCollection.DependencyRefs, Entry.GetSemanticId(), Pointer)) return false;
									Out->SetField(Definition.Key, Reference);
								}
								return true;
							};
							auto BuildTrackDefinitions = [&](const FKeyedItemProjectionContract& TrackContract,
								const FUeAgentCanonicalValue& TrackValue, FUnitDraft& DefinitionCollection, const FString& TracksPointer) -> bool
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
									const FString TrackPointer = TracksPointer + TEXT("/") + EscapeJsonPointerSegment(Source.Key);
									TSharedPtr<FJsonObject> TrackPropertiesDefinition, DefaultPropertiesDefinition;
									if (!InternRecordProperties(Source.Value.Get(), TEXT("property_definitions"), DefinitionCollection, TrackPropertiesDefinition, TrackPointer)
										|| !InternRecordProperties(Source.Value.Get(), TEXT("default_property_definitions"), DefinitionCollection, DefaultPropertiesDefinition, TrackPointer)) return false;
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
											if (!InternRecordProperties(Record.Value.Get(), TEXT("property_definitions"), DefinitionCollection, Definitions,
												TrackPointer + TEXT("/sections/") + EscapeJsonPointerSegment(Record.Key))) return false;
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
										const FString ChannelPointer = TrackPointer + TEXT("/default_channels/") + EscapeJsonPointerSegment(ChannelSource.Key);
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
												|| !InternDefinitionAt(NativeDefinition, PublicDefinition, DefinitionCollection.DependencyRefs,
													Entry.GetSemanticId(), ChannelPointer + TEXT("/key_properties_definition"))) { if (!ProjectionError.IsEmpty()) OutError.SetFailure(ProjectionError, Visit.Path); return false; }
											KeyContract->SetField(TEXT("properties_definition"), PublicDefinition);
										}
										TSharedPtr<FJsonValue> PublicValueDefinition;
										const auto* ValueDefinition = ChannelSource.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("key_value_definition"); });
										if (ValueDefinition)
										{
											TSharedPtr<FJsonValue> NativeDefinition;
											if (!CanonicalJson::TryToJsonValue(ValueDefinition->Value.Get(), NativeDefinition, ProjectionError)
												|| !InternDefinitionAt(NativeDefinition, PublicValueDefinition, DefinitionCollection.DependencyRefs,
													Entry.GetSemanticId(), ChannelPointer + TEXT("/key_value_definition"))) { if (!ProjectionError.IsEmpty()) OutError.SetFailure(ProjectionError, Visit.Path); return false; }
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
											if (!InternDefinitionAt(NativeChannelDefinition, PublicDefinition, DefinitionCollection.DependencyRefs,
												Entry.GetSemanticId(), ChannelPointer + TEXT("/properties_definition"))) return false;
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
								if (!BuildTrackDefinitions(Contract, *Visit.Value, Collection, TEXT("/") + Visit.Path)) return false;
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
									const FString BindingPointer = TEXT("/") + Visit.Path + TEXT("/") + EscapeJsonPointerSegment(SourceBinding.Key);
									TSharedPtr<FJsonObject> BindingProperties;
									if (!InternRecordProperties(SourceBinding.Value.Get(), TEXT("property_definitions"), Collection, BindingProperties, BindingPointer)) return false;
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
											if (!InternRecordProperties(Template.Value.Get(), FString(), Collection, TemplateProperties,
												BindingPointer + TEXT("/template_property_definitions/") + EscapeJsonPointerSegment(Template.Key))) return false;
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
										TrackDefinitions, BindingPointer + TEXT("/tracks"))) return false;
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
								+ Entry.GetSemanticId() + TEXT("|")
								+ Entry.TargetIdentity.GetCanonicalKey() + TEXT("|")
								+ Visit.Path);
						FUnitDraft Collection;
						Collection.UnitId = CollectionId;
						Collection.UnitKind =
							ESemanticUnitKind::CollectionState;
						Collection.OwnerRef = Visit.OwnerId;
						Collection.SemanticTypeRef =
							Entry.GetSemanticId() + TEXT("#")
								+ Visit.Path;
						Collection.Role = Visit.SurfaceId;
						Collection.TargetIdentityKey =
							BindingForOwner(Visit.OwnerId, Entry);
						Collection.SourceProvenance =
							Entry.GetProvenance().IsEmpty()
								? Provenance
								: Entry.GetProvenance();
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
										+ Entry.GetSemanticId() + TEXT("|")
										+ Entry.TargetIdentity.GetCanonicalKey() + TEXT("|")
										+ ItemPath);
								Item.UnitKind =
									ESemanticUnitKind::ItemState;
								Item.OwnerRef = CollectionId;
								Item.SemanticTypeRef =
									Entry.GetSemanticId() + TEXT("#")
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
							+ Entry.GetSemanticId() + TEXT("|")
							+ Entry.TargetIdentity.GetCanonicalKey() + TEXT("|")
							+ Visit.Path);
					Unit.UnitKind = bRelation
						? ESemanticUnitKind::RelationSet
						: ESemanticUnitKind::ItemState;
					Unit.OwnerRef = Visit.OwnerId;
					Unit.SemanticTypeRef =
						Entry.GetSemanticId() + TEXT("#") + Visit.Path;
					Unit.Role = Visit.SurfaceId;
					Unit.TargetIdentityKey =
						BindingForOwner(Visit.OwnerId, Entry);
					Unit.SourceProvenance =
						Entry.GetProvenance().IsEmpty()
							? Provenance
							: Entry.GetProvenance();
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
							Entry.GetProvenance().IsEmpty()
								? Provenance
								: Entry.GetProvenance();
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
			const FEntryRoute& Route = ResolveEntryRoute(Entry);
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
			const FEntryRoute& Route = ResolveEntryRoute(Entry);
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
						Entry.TargetIdentity.GetCanonicalKey();
				}
				else if (TargetDraft.TargetIdentityKey
					!= Entry.TargetIdentity.GetCanonicalKey())
				{
					OutError.SetSourceFailure(TEXT("composition_target_identity_conflict"), TEXT("Target-level entries resolve to different target identities."),
						Entry.GetSemanticId());
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
			Draft.UnitId = CompositionEntryUnitId(Route.bGraph, Request.TargetKind, Entry.GetSemanticId(), Entry.TargetIdentity.GetCanonicalKey());
			Draft.UnitKind = Route.UnitKind;
			Draft.OwnerRef = Route.bGraph
				? *SurfaceUnit
				: FindGraphOwner(
					Entry,
					GraphUnits,
					*SurfaceUnit);
			Draft.SemanticTypeRef = Entry.GetSemanticId();
			Draft.TargetIdentityKey =
				Entry.TargetIdentity.GetCanonicalKey();
			Draft.Role = Route.SurfaceId;
			Draft.SourceProvenance =
				Entry.GetProvenance().IsEmpty()
					? Provenance
					: Entry.GetProvenance();
			Draft.CategoryPath = Entry.GetCategoryPath();
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
				if (!Entry.GetSemanticId().EndsWith(TEXT(".pin")))
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
			const FEntryRoute& Route = ResolveEntryRoute(Entry);
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
				else if (Entry.GetSemanticId().EndsWith(TEXT(".pin")))
				{
					Draft.OwnerRef = FindItemOwner(
						Entry,
						ItemUnits,
						Draft.OwnerRef);
				}
				else if (Entry.GetSemanticId().EndsWith(
					TEXT(".dynamic_input")))
				{
					Draft.OwnerRef = FindItemOwnerBySelector(
						Entry,
						ItemUnits,
						TEXT("module_guid"),
						Draft.OwnerRef);
				}
				else if (Entry.GetSemanticId().EndsWith(
						TEXT(".renderer"))
					|| Entry.GetSemanticId().EndsWith(
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
				else if (Entry.GetSemanticId().EndsWith(
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

		const double PropertyAssemblyStarted = FPlatformTime::Seconds();
		int32 PropertyStableIds = 0;
		// ItemUnits is complete before this loop. Group adjacent equal owner
		// inputs, borrowing its stable result; this is not another owner index.
		TOptional<FItemSelectorParts> PropertyOwnerParts;
		const FString* LastPropertyOwner = nullptr;
		int32 PropertyOwnerKeyBuilds = 0, PropertyOwnerGroupHits = 0;
		const auto FindPropertyOwner = [&](const FCompositionEntryView& Entry, const FString& Surface) -> const FString& {
			const auto Parts = DescribeItemSelectorKey(Entry);
			if (PropertyOwnerParts.IsSet() && PropertyOwnerParts->Matches(Parts)) ++PropertyOwnerGroupHits;
			else {
				LastPropertyOwner = ItemUnits.Find(Parts.Build()); PropertyOwnerParts = Parts; ++PropertyOwnerKeyBuilds;
			}
			return LastPropertyOwner ? *LastPropertyOwner : Surface;
		};
		for (const FCompositionEntryView& Entry : Entries)
		{
			if (!Entry.bAvailable || !Entry.Value.IsValid())
			{
				continue;
			}
			const FEntryRoute& Route = ResolveEntryRoute(Entry);
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
			const FString RelationOwner = Route.bProperty ? FString() : FindGraphOwner(Entry, GraphUnits, *SurfaceUnit);
			const FString& OwnerRef = !DeclaredOwner.IsEmpty()
				? DeclaredOwner
				: Route.bProperty
					? FindPropertyOwner(Entry, *SurfaceUnit)
					: RelationOwner;
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
					Aggregate.Groups.FindOrAdd(Entry.GetSemanticId());
				if (Group.Contains(Entry.TargetIdentity.GetCanonicalKey()))
				{
					OutError.SetSourceFailure(TEXT("composition_relation_identity_conflict"), TEXT("Two captured relations have the same identity within one relation group."),
						Entry.TargetIdentity.GetCanonicalKey());
					return false;
				}
				Group.Add(
					Entry.TargetIdentity.GetCanonicalKey(),
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
			++PropertyStableIds;
			Draft.UnitId = StableId(
				TEXT("property"),
				Request.TargetKind + TEXT("|")
					+ Entry.GetSemanticId() + TEXT("|")
					+ Entry.TargetIdentity.GetCanonicalKey());
			Draft.UnitKind = Route.UnitKind;
			Draft.OwnerRef = OwnerRef;
			Draft.SemanticTypeRef = Entry.GetSemanticId();
			Draft.TargetIdentityKey =
				Entry.TargetIdentity.GetCanonicalKey();
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
					Entry.GetSemanticId().ParseIntoArray(
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
							NormalizeRoleToken(Entry.GetSemanticId());
					}
					if (Draft.FieldSetRef.IsEmpty())
					{
						Draft.FieldSetRef =
							NormalizeRoleToken(Route.SurfaceId);
					}
				}
			}
			Draft.SourceProvenance =
				Entry.GetProvenance().IsEmpty()
					? Provenance
					: Entry.GetProvenance();
			Draft.CategoryPath = Entry.GetCategoryPath();
			if (!DefinitionId.IsEmpty()) Draft.DependencyRefs.Add(DefinitionId);
			// Select from the actual captured field shape before constructing it.
			// This is the same ordinary property producer as TryBuildValues below;
			// no failure retries through another value representation.
			if (Request.bDirectPropertyValues && Route.bProperty && Entry.Value) {
				if (Entry.Value->GetKind() != EUeAgentValueKind::Object) {
					Draft.SinglePropertyValue.Emplace(Entry.Value.ToSharedRef()); Draft.bDirectPropertyValue = true;
				} else if (Entry.Value->GetObjectItems().Num() == 1) {
					const auto& Field = Entry.Value->GetObjectItems()[0];
					if (Field.Key.Equals(TEXT("literal"), ESearchCase::CaseSensitive)) {
						Draft.SinglePropertyField = FSemanticUnit::FSinglePropertyValue::EField::Literal; Draft.bDirectPropertyValue = true;
					} else if (Field.Key.Equals(TEXT("selected"), ESearchCase::CaseSensitive)) {
						Draft.SinglePropertyField = FSemanticUnit::FSinglePropertyValue::EField::Selected; Draft.bDirectPropertyValue = true;
					} else Draft.bDirectPropertyValue = Field.Key.Equals(TEXT("value"), ESearchCase::CaseSensitive);
					if (Draft.bDirectPropertyValue) Draft.SinglePropertyValue.Emplace(Field.Value);
				}
			}
			if (!Draft.bDirectPropertyValue && !TryBuildValues(
				Entry.Value,
				{},
				Resources,
				Draft.Values,
				OutError))
			{
				return false;
			}
			const int32* PropertyOwner = Request.bDirectPropertyValues && Route.bProperty
				&& Draft.UnitKind == ESemanticUnitKind::PropertySet && Draft.SemanticTypeRef.EndsWith(TEXT(".property"))
				? DraftIndex.Find(Draft.OwnerRef) : nullptr;
			const bool bOrdinaryNativeOwner = PropertyOwner && Drafts[*PropertyOwner].UnitKind == ESemanticUnitKind::ItemState
				&& (Drafts[*PropertyOwner].SemanticTypeRef.EndsWith(TEXT(".actor.target"))
					|| Drafts[*PropertyOwner].SemanticTypeRef.EndsWith(TEXT(".component.target")));
			if (bOrdinaryNativeOwner) {
				auto& Frozen = FrozenProperties.AddDefaulted_GetRef(); Frozen.Ordinal = NextDraftOrdinal++;
				FWorkspaceError Failure;
				if (!TryFreezeDraftUnit(Draft, Frozen.Unit, Failure, DirectPropertyUnits))
					Frozen.Error = MakeShared<FWorkspaceError>(MoveTemp(Failure));
			} else AddDraft(MoveTemp(Draft));
		}

		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_property_assembly entries=%d stable_property_ids=%d old_per_byte_printf_calls_retired=%lld total_s=%.6f hash_input=original_utf8 suffix=original_digest_last12 owner_key_builds=%d owner_group_hits=%d"),
			Entries.Num(), PropertyStableIds, static_cast<int64>(PropertyStableIds) * SHA256_DIGEST_LENGTH,
			FPlatformTime::Seconds() - PropertyAssemblyStarted, PropertyOwnerKeyBuilds, PropertyOwnerGroupHits);
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

		// A generated texture can keep a persistent reference to its prior
		// source. Full and scoped observations both have its public property
		// value. The private generated-state verification remains separate.
		const UeAgentReflectionAuthoring::FProfile* SourceProfile = nullptr;
		FString SourcePackagePath, SourceObjectPath;
		if (Request.TargetKind == TEXT("ue.imported_texture.asset"))
		{
			const auto& TargetValues = Drafts[DraftIndex.FindChecked(TargetId)].Values;
			const auto* Kind = TargetValues.Find(TEXT("asset_kind"));
			const auto* Class = TargetValues.Find(TEXT("class_path"));
			const auto* Path = TargetValues.Find(TEXT("asset_path"));
			if (Kind && Class && Path && (*Kind)->GetKind() == EUeAgentValueKind::String
				&& (*Class)->GetKind() == EUeAgentValueKind::String && (*Path)->GetKind() == EUeAgentValueKind::String)
			{
				const auto* Registered = UeAgentReflectionAuthoring::FindProfileByTypeId((*Kind)->GetText());
				if (Registered && !Registered->GenerationDependencyProperty.IsEmpty() && Registered->AssetClassPath == (*Class)->GetText())
				{
					SourceProfile = Registered;
					SourcePackagePath = FPackageName::ObjectPathToPackageName((*Path)->GetText());
					SourceObjectPath = SourcePackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(SourcePackagePath);
				}
			}
		}
		TArray<FString> DependencyPaths;
		for (const FCompositionEntryView& Entry : Entries)
		{
			if (SourceProfile && Entry.GetSemanticId() == SourceProfile->GeneratedStateSemanticId) continue;
			if (SourceProfile && Entry.bAvailable && Entry.bAuthoringVisible && Entry.GetSemanticId() == TEXT("core.property")
				&& Entry.TargetIdentity.GetSelector(TEXT("property_path")) == SourceProfile->GenerationDependencyProperty)
			{
				const auto& Owner = Entry.TargetIdentity.GetSelector(TEXT("owner_path"));
				if (Owner == SourcePackagePath || Owner == SourceObjectPath)
				{
					const auto* Path = Entry.Value ? FindObjectField(Entry.Value.ToSharedRef(), TEXT("path")) : nullptr;
					if (!Path || ((*Path)->GetKind() != EUeAgentValueKind::Null && ((*Path)->GetKind() != EUeAgentValueKind::String
						|| !(*Path)->GetText().StartsWith(TEXT("/")))))
					{
						OutError.SetSourceFailure(TEXT("composition_source_dependency_ref_invalid"),
							TEXT("The registered generation dependency requires a captured object reference or null."),
							Entry.TargetIdentity.GetCanonicalKey(), SourceProfile->GenerationDependencyProperty);
						return false;
					}
					if ((*Path)->GetKind() == EUeAgentValueKind::String) DependencyPaths.AddUnique((*Path)->GetText());
				}
			}
			if (!Entry.Value.IsValid()
				|| Entry.Value->GetKind() != EUeAgentValueKind::Object)
			{
				continue;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Entry.Value->GetObjectItems())
			{
				CollectDirectDependencyPath(Field.Key, Field.Value.Get(), DependencyPaths);
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
			OutError, false, AcceptedBindings, KeyedBaseline, KeyedNames))
		{
			return false;
		}
		if (Request.TargetKind == TEXT("ue.level.asset")
			&& !TryAddKeyedItemDesiredStateProjection(Drafts, TEXT("level_components"),
				TEXT(".component.target"), TEXT("components"), ComponentItems.NamingPolicy,
			Resources, OutError, true, AcceptedBindings, KeyedBaseline, KeyedNames)) return false;

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
		Units.Reserve(Drafts.Num() + FrozenProperties.Num());
		int32 FrozenIndex = 0;
		const auto AppendFrozen = [&]() {
			auto& Frozen = FrozenProperties[FrozenIndex++];
			if (Frozen.Error) { OutError = *Frozen.Error; return false; }
			Units.Add(MoveTemp(Frozen.Unit.GetValue())); return true;
		};
		for (FUnitDraft& Draft : Drafts) {
			while (FrozenIndex < FrozenProperties.Num() && FrozenProperties[FrozenIndex].Ordinal < Draft.EncounterOrdinal)
				if (!AppendFrozen()) return false;
			TOptional<FSemanticUnit> Unit;
			if (!TryFreezeDraftUnit(Draft, Unit, OutError, DirectPropertyUnits)) return false;
			Units.Add(MoveTemp(Unit.GetValue()));
		}
		while (FrozenIndex < FrozenProperties.Num()) if (!AppendFrozen()) return false;
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_frozen_properties frozen_units=%d remaining_drafts=%d draft_index_entries=%d ordering=original_encounter no_property_structure_pass=true"),
			FrozenProperties.Num(), Drafts.Num(), DraftIndex.Num());
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_property_storage units=%d direct_value_units=%d draft_value_maps_produced=0_for_direct same_unit_validator=true"), Units.Num(), DirectPropertyUnits);
		if (OutPreparationCharges) *OutPreparationCharges = CaptureChargesBetween(PreparationBefore, Resources.Snapshot());
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_definition_inputs entries=%d declaration_type_rows=%d independent_type_rows=%d reused_definitions=%d shared_declaration_bindings=%d unshared_proof_production_skipped=%d type_deep_intern=false"),
			Entries.Num(), SharedTypeCount, UniqueTypeCount, ReusedDefinitionInputs, DefinitionInputCount, UnsharedDefinitionInputs);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_route_inputs entries=%d group_selections=%d complete_contract_copies=1 producer=original_registry_contract same_rule=true retained_route_tables=0"),
			Entries.Num(), RouteSelections);
		if (OutUnits) {
			TSet<FString> Identities;
			for (auto& Unit : Units) {
				if (!Unit.IsValid()) { OutError.SetFailure(Unit.GetValidationDiagnostic(), Unit.GetUnitId()); return false; }
				if (Identities.Contains(Unit.GetUnitId())) {
					OutError.SetSourceFailure(TEXT("duplicate_semantic_unit"), TEXT("More than one semantic unit has the same unit identity."), Unit.GetUnitId());
					return false;
				}
				Identities.Add(Unit.GetUnitId());
				if (!TryNormalizeAuthoringUnit(Unit, Resources, OutError)) return false;
			}
			OutUnits->ContractId = CompositionSchema; OutUnits->ContractVersion = 1;
			OutUnits->ProfileId = Request.ProfileId; OutUnits->BaseRevision = Request.BaseRevision;
			OutUnits->Units = MoveTemp(Units); OutUnits->TargetUnitIds = {TargetId};
			return true;
		}
		return TryCompileAuthoringModel(
			CompositionSchema,
			1,
			Request.ProfileId,
			Request.BaseRevision,
			MoveTemp(Units),
			{TargetId},
			Resources,
			OutModel,
			OutError, ProjectionHints, nullptr, nullptr, bBuildLegacyFingerprint);
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
	};
	static thread_local TSharedPtr<const FCompletedCanonicalCapture> PreviousCanonicalCapture;
	static bool ModelHonorsAcceptedDefinitionHints(const FAuthoringModel& Model, FString& Mismatch)
	{
		const auto Hints = GetAcceptedDefinitionProjectionHints();
		if (!Hints || !Hints->ContractId.Equals(Model.GetContractId(), ESearchCase::CaseSensitive)
			|| Hints->ContractVersion != Model.GetContractVersion()
			|| !Hints->ProfileId.Equals(Model.GetProfileId(), ESearchCase::CaseSensitive)
			|| Hints->TargetUnitIds != Model.GetTargetUnitIds()) {
			// Fresh export has a fresh naming policy. An unchanged native
			// snapshot does not authorize returning another workspace's names.
			Mismatch = TEXT("workspace_context");
			return Model.GetPreferredDefinitionBindings().IsEmpty();
		}
		for (const auto& Preferred : Model.GetPreferredDefinitionBindings())
			if (!Hints->Definitions.Contains(Preferred.Key)) { Mismatch = TEXT("preferred_binding_not_in_accepted_view"); return false; }
		for (const auto& Pair : Hints->Definitions) {
			const auto* Preferred = Model.GetPreferredDefinitionBindings().Find(Pair.Key);
			const auto& Hint = Pair.Value;
			if (Preferred && Preferred->ContentKey.Equals(Hint.ContentKey, ESearchCase::CaseSensitive)
				&& Preferred->DefinitionName.Equals(Hint.DefinitionName, ESearchCase::CaseSensitive)
				&& Preferred->ReadableAlias.Equals(Hint.ReadableAlias, ESearchCase::CaseSensitive)
				&& Preferred->PublicRef.Equals(Hint.PublicRef, ESearchCase::CaseSensitive)) continue;
			const auto* Unit = Model.FindUnit(Pair.Key);
			FString CurrentContent; FWorkspaceError Error;
			if (Unit && Unit->GetUnitKind() == ESemanticUnitKind::CollectionState
				&& Unit->GetRole() == TEXT("property_definition")
				&& TryHashSemanticUnitRow(*Unit, CurrentContent, Error)
				&& CurrentContent.Equals(Hint.ContentKey, ESearchCase::CaseSensitive)) {
				Mismatch = !Preferred ? TEXT("eligible_binding_missing")
					: !Preferred->ContentKey.Equals(Hint.ContentKey, ESearchCase::CaseSensitive) ? TEXT("binding_content_key")
					: !Preferred->DefinitionName.Equals(Hint.DefinitionName, ESearchCase::CaseSensitive) ? TEXT("definition_name")
					: !Preferred->ReadableAlias.Equals(Hint.ReadableAlias, ESearchCase::CaseSensitive) ? TEXT("readable_alias") : TEXT("public_ref");
				return false;
			}
		}
		return true;
	}
	static bool TryBindAcceptedDefinitionProjection(FAuthoringModelPtr& Model, FWorkspaceError& Error)
	{
		FString Mismatch;
		if (ModelHonorsAcceptedDefinitionHints(*Model, Mismatch)) return true;
		const double Started = FPlatformTime::Seconds();
		const auto Hints = GetAcceptedDefinitionProjectionHints();
		FAuthoringModelPtr Rebound;
		if (!Model->TryRebindDefinitionProjection(Hints.Get(), Rebound, Error)) return false;
		Model = MoveTemp(Rebound);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=definition_projection_rebind reason=%s units=%d native_rebuilt=false semantic_recompiled=false total_s=%.6f"),
			*Mismatch, Model->GetUnits().Num(), FPlatformTime::Seconds() - Started);
		return true;
	}
	void ResetCanonicalCaptureReuse()
	{
		PreviousCanonicalCapture.Reset();
	}

	static int64 CanonicalProofBytes(const FCanonicalEntryObservation& Proof)
	{
		// Canonical values/descriptors belong to the one current snapshot. Count
		// the extra proof backing, rather than rejecting an arbitrary entry count.
		return sizeof(FCanonicalEntryObservation) + Proof.RevisionValueKey.GetAllocatedSize()
			+ Proof.Value.Charges.GetAllocatedSize() + Proof.DescriptorCharges.GetAllocatedSize();
	}
	static constexpr int64 CanonicalProofBudget = 256ll * 1024 * 1024;
	static void RetainBoundedCanonicalRows()
	{
		if (!PreviousCanonicalCapture) return;
		int64 Total = 0;
		for (const auto& Pair : PreviousCanonicalCapture->Entries)
			Total += CanonicalProofBytes(Pair.Value) + sizeof(FString) + Pair.Key.GetAllocatedSize() + 32;
		if (Total <= CanonicalProofBudget) {
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=canonical_proof_admission mode=rows retained=%d proof_bytes=%lld partial=false"), PreviousCanonicalCapture->Entries.Num(), Total);
			return;
		}
		auto Bounded = MakeShared<FCompletedCanonicalCapture>();
		Bounded->TargetKind = PreviousCanonicalCapture->TargetKind; Bounded->ProfileId = PreviousCanonicalCapture->ProfileId;
		Bounded->Snapshot = PreviousCanonicalCapture->Snapshot; Bounded->Model = PreviousCanonicalCapture->Model;
		Bounded->Charges = PreviousCanonicalCapture->Charges;
		int64 Bytes = 0;
		for (const auto& Pair : PreviousCanonicalCapture->Entries) {
			const int64 Charge = CanonicalProofBytes(Pair.Value) + sizeof(FString) + Pair.Key.GetAllocatedSize() + 32;
			if (Charge > CanonicalProofBudget - Bytes) continue;
			Bytes += Charge; Bounded->Entries.Add(Pair.Key, Pair.Value);
		}
		PreviousCanonicalCapture = MoveTemp(Bounded);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=canonical_proof_admission mode=rows retained=%d proof_bytes=%lld partial=true"), PreviousCanonicalCapture->Entries.Num(), Bytes);
	}

	static bool TryComposeAuthoringModelFromCapture(
		const FCompositionRequest& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError,
		const TMap<const FJsonValue*, FObservedCanonicalValue>* ObservedValues,
		const FDefinitionProjectionHints* ProjectionHints = nullptr, bool bBuildLegacyFingerprint = true,
		const UeAgentAuthoringState::FStateSnapshot* BorrowedSnapshot = nullptr)
	{
		const double ComposeStarted = FPlatformTime::Seconds();
		FCompositionInput Input;
		Input.TargetKind = Request.TargetKind;
		Input.ProfileId = Request.ProfileId;
		Input.BaseRevision = Request.BaseRevision;
		Input.SourceProvenance = Request.SourceProvenance;
		const int32 BorrowedCount = BorrowedSnapshot ? BorrowedSnapshot->Num() : 0;
		const int32 SourceCount = BorrowedCount + Request.Entries.Num();
		Input.Entries.Reserve(SourceCount);
		Input.bEntriesOrdered = BorrowedSnapshot && BorrowedSnapshot->HasOrderedAuthoringEntries()
			&& Request.Entries.IsEmpty();
		Input.bDirectPropertyValues = BorrowedSnapshot && Request.TargetKind == TEXT("ue.level.asset");
		int32 ReusedCanonicalValues = 0, DirectCanonicalValues = 0, JsonValueConversions = 0;
		int32 ProjectionValuesUnchanged = 0, ProjectionDefinitionsUnchanged = 0;
		double JsonValueProjectionSeconds = 0, JsonValueConversionSeconds = 0;
		for (int32 SourceIndex = 0; SourceIndex < SourceCount; ++SourceIndex)
		{
			const auto& Entry = SourceIndex < BorrowedCount ? BorrowedSnapshot->ReadAuthoringEntry(SourceIndex)
				: Request.Entries[SourceIndex - BorrowedCount];
			FCompositionEntryView View;
			View.BindCapturedMetadata(Entry);
			View.FieldDeclaration = Entry.FieldDeclaration;
			if (Entry.FieldDeclaration) {
				const TSharedPtr<FJsonObject>* Type = nullptr;
				if (!Entry.FieldDeclaration->TryGetObjectField(TEXT("type"), Type) || !Type || !Type->IsValid()) {
					OutError.SetSourceFailure(TEXT("composition_field_declaration_invalid"),
						TEXT("The capture declaration handoff requires its original type object; no descriptor reconstruction was selected."), Entry.TargetIdentity.CanonicalKey, TEXT("/type"));
					return false;
				}
				View.Type = *Type;
			} else View.Type = Entry.GetType().ToJsonObject();
			View.bAvailable = Entry.bAvailable;
			View.bEditable = Entry.bEditable;
			View.bAuthoringVisible = Entry.bAuthoringVisible;
			View.CandidatesHeader = Entry.CandidatesHeader;
			View.Candidates = Entry.Candidates;
			View.ValueDefinition = Entry.ValueDefinition;
			if (Entry.DirectAuthoringValue) {
				View.Value = Entry.DirectAuthoringValue;
				++DirectCanonicalValues;
			} else {
			TSharedPtr<FJsonValue> ProjectedValue;
			const double ProjectionStarted = FPlatformTime::Seconds();
			View.bAuthoringVisible &= UeAgentPropertyValueCodec::ProjectAuthoringValue(
				View.Type, Entry.Value, Entry.ValueDefinition, ProjectedValue, View.ValueDefinition);
			JsonValueProjectionSeconds += FPlatformTime::Seconds() - ProjectionStarted;
				ProjectionValuesUnchanged += ProjectedValue == Entry.Value;
				ProjectionDefinitionsUnchanged += View.ValueDefinition == Entry.ValueDefinition;
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
			else if (ProjectedValue.IsValid()) {
				++JsonValueConversions; const double ConversionStarted = FPlatformTime::Seconds();
				const bool bConverted = TryCanonicalizeJson(ProjectedValue, Resources, View.Value, OutError);
				JsonValueConversionSeconds += FPlatformTime::Seconds() - ConversionStarted;
				if (!bConverted) return false;
			}
			}
			Input.Entries.Add(MoveTemp(View));
		}
		const double ViewsFinished = FPlatformTime::Seconds();
		const bool bComposed = TryComposeEntryViews(
			MoveTemp(Input),
			Resources,
			OutModel,
			OutError, nullptr, nullptr, ProjectionHints, nullptr, nullptr, nullptr, nullptr, bBuildLegacyFingerprint);
		const double ComposeFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=model_compose kind=%s entries=%d reused_canonical_values=%d entry_views_s=%.6f compose_views_s=%.6f total_s=%.6f success=%s metadata_borrowed_entries=%d native_identity_map_copies=0 direct_canonical_values=%d json_value_conversions=%d json_projection_s=%.6f json_conversion_s=%.6f projection_values_unchanged=%d projection_definitions_unchanged=%d"),
			*Request.TargetKind, SourceCount, ReusedCanonicalValues, ViewsFinished - ComposeStarted,
			ComposeFinished - ViewsFinished, ComposeFinished - ComposeStarted,
			bComposed ? TEXT("true") : TEXT("false"), SourceCount, DirectCanonicalValues, JsonValueConversions, JsonValueProjectionSeconds, JsonValueConversionSeconds, ProjectionValuesUnchanged, ProjectionDefinitionsUnchanged);
		return bComposed;
	}

	bool TryComposeAuthoringModel(const FCompositionRequest& Request, FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		return TryComposeAuthoringModelFromCapture(Request, Resources, OutModel, OutError, nullptr);
	}

	static bool ComposeCanonicalAuthoringProjection(
		const FCanonicalCompositionRequest& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, FCompositionUnitSet* OutUnits, const FAuthoringModel* AcceptedBindings = nullptr,
		const FCompositionUnitLookup* KeyedBaseline = nullptr, const FKeyedMemberNameLookup* KeyedNames = nullptr)
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
		return TryComposeEntryViews(MoveTemp(Input), Resources, OutModel, OutError, nullptr, nullptr, nullptr, OutUnits, AcceptedBindings, KeyedBaseline, KeyedNames);
	}

	bool TryComposeCanonicalAuthoringModel(const FCanonicalCompositionRequest& Request,
		FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		return ComposeCanonicalAuthoringProjection(Request, Resources, OutModel, OutError, nullptr);
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
					if (const auto* Value = Unit->FindValue(TEXT("type"))) Type = &Value->Get();
					if (const auto* Value = Unit->FindValue(TEXT("value_definition"))) Definition = &Value->Get();
					break;
				}
		if (!Edited.VisitValues([&](const FString& Key, const auto& Value) {
			const auto* Previous = Before ? Before->FindValue(Key) : nullptr;
			if (!Previous) { OutFields.Add(Key); return true; }
			if (Model && (Type || Definition))
			{
				TSharedPtr<FJsonValue> A, B; UE::UAI::Contract::FDiagnostic Error;
				if (!CanonicalJson::TryToJsonValue(**Previous, A, Error)
					|| !CanonicalJson::TryToJsonValue(*Value, B, Error))
				{ OutError.SetFailure(Error, Edited.GetUnitId()); return false; }
				if (!(Edited.GetValueCount() == 1 && Key == TEXT("value")))
				{
					const auto Left = MakeShared<FJsonObject>(), Right = MakeShared<FJsonObject>();
					Left->SetField(Key, A); Right->SetField(Key, B);
					A = MakeShared<FJsonValueObject>(Left); B = MakeShared<FJsonValueObject>(Right);
				}
				CanonicalJson::FValueComparison Compared;
				if (!TryCompareWorkspaceValues(*Model, Type, Definition, A, B, Compared, OutError)) return false;
				if (!Compared.bEquivalent) OutFields.Add(Key);
				return true;
			}
			FUeAgentSemanticComparison Comparison;
			FUeAgentStateSemanticError Error;
			if (!TryCompareValues(*Previous, Value, TEXT("/"), Resources, Comparison, Error))
			{ OutError.SetFailure(Error, Edited.GetUnitId()); return false; }
			if (!Comparison.IsEquivalent()) OutFields.Add(Key);
			return true;
		})) return false;
		if (Before)
			Before->VisitValues([&](const FString& Key, const auto&) { if (!Edited.HasValue(Key)) OutFields.Add(Key); return true; });
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
		TSet<FString> CheckedSemantics, DocumentSemantics;
		for (const FString& Semantic : Snapshot->GetSemanticIds())
		{
			if (CheckedSemantics.Contains(Semantic)) continue;
			CheckedSemantics.Add(Semantic);
			if (RouteEntry(Input.TargetKind, Semantic).bDocument) DocumentSemantics.Add(Semantic.ToLower());
		}
		if (DocumentSemantics.IsEmpty()) return true;
		const TSet<FString>& VisibleDocumentSemantics = CurrentModel->GetSemanticTypePrefixes();
		// Document reconstruction has no consumer when the complete current
		// capture contains no visible document route. Decide this from the same
		// routing/visibility inputs before projecting every property descriptor.
		// When a document is present retain the full composition context below.
		bool bHasVisibleDocument = false;
		for (const FString& Semantic : DocumentSemantics)
		{
			if (VisibleDocumentSemantics.Contains(Semantic))
			{ bHasVisibleDocument = true; break; }
		}
		if (!bHasVisibleDocument) return true;
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
		if (!TryComposeEntryViews(MoveTemp(Input), Resources, RebuiltModel, OutError, &Bindings)) return false;
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
			if (Edited->GetValueCount() != Before->GetValueCount())
			{ return RejectEdit(TEXT("composition_document_structure_edit_unsupported"), FString::Printf(TEXT("The document unit must retain its %d value fields; %d were supplied."), Before->GetValueCount(), Edited->GetValueCount())); }
			TSet<FString> ChangedFields;
			// Keyed documents depend on definitions for nested native properties.
			// Those types do not describe the readable collection itself. Detect
			// authored changes here; its keyed contract resolves them below.
			if (!TryCollectChangedUnitValueFields(Before, *Edited, Resources, ChangedFields, OutError,
				KeyedContract ? nullptr : &EditedModel.Get())) return false;
			for (const FString& Field : ChangedFields)
			{
				if (!Edited->HasValue(Field))
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
				const auto* Keys = Edited->FindValue(KeyedContract->RootField);
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
					const auto* Desired = SiblingUnit ? SiblingUnit->FindValue(Binding.KeyedContract->RootField) : nullptr;
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
				const auto* Whole = Edited->FindValue(TEXT("value"));
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

	// Reuse the full composer's keyed projection owner for an existing Level
	// metadata value change. Membership, item identities and naming are already
	// fixed; replay the affected old roots first to prove this exact recipe.
	static bool TryCaptureCanonicalEntry(const UeAgentAuthoringState::FStateEntry& Entry,
		const FCanonicalEntryObservation* Old, const FString& BaseRevision,
		const FUeAgentStateRevision& Revision, FUeAgentResourceTracker& Resources,
		FCanonicalEntryObservation& Proof, TOptional<FUeAgentStateEntry>& OutEntry,
		TMap<const FJsonValue*, FObservedCanonicalValue>* ObservedValues,
		int32& ReusedEntries, FWorkspaceError& OutError,
		int32* ExactValueReuses = nullptr, int32* ExactDescriptorReuses = nullptr, bool bIncludeAuthoringMetadata = true,
		bool bRecordReuseMaterial = true)
	{
		const bool bSameObservation = Old && Entry.Observation && Old->Observation == Entry.Observation;
		const bool bSameCurrentValue = !bSameObservation && Old && Old->Value.Value
			&& CanonicalJson::MatchesCanonicalProjection(Entry.Value, *Old->Value.Value);
		FUeAgentCanonicalValuePtr Value;
		const auto ForwardCaptureFailure = [&](Contract::FDiagnostic Failure)
		{
			if (Failure.CandidateName.IsEmpty()) Failure.CandidateName = Entry.DisplayName;
			if (Failure.TargetPath.IsEmpty())
				if (const auto* AssetPath = Entry.TargetIdentity.Selectors.Find(TEXT("asset_path"))) Failure.TargetPath = *AssetPath;
			OutError.SetFailure(MoveTemp(Failure), Entry.TargetIdentity.CanonicalKey);
			return false;
		};
		TOptional<decltype(Resources.Snapshot())> ValueUsageBefore;
		if (bRecordReuseMaterial) ValueUsageBefore.Emplace(Resources.Snapshot());
		if (bSameObservation || bSameCurrentValue) {
			FUeAgentResourceChargeError ResourceError;
			if (!Resources.TryChargeMany(Old->Value.Charges, TEXT("composition:actual_unchanged_value"), ResourceError))
			{ OutError.SetFailure(ResourceError); return false; }
			Value = Old->Value.Value; ++ReusedEntries;
			if (bSameCurrentValue && ExactValueReuses) ++*ExactValueReuses;
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
		if (ValueUsageBefore.IsSet()) {
			Proof.Value.Value = Value;
			Proof.Value.Charges = CaptureChargesBetween(ValueUsageBefore.GetValue(), Resources.Snapshot());
		}
		if (ObservedValues) ObservedValues->Add(Entry.Value.Get(), Proof.Value);
		const FString TypeId =
			!Entry.GetType().EnumPath.IsEmpty()		 ? Entry.GetType().EnumPath
			: !Entry.GetType().StructPath.IsEmpty()		 ? Entry.GetType().StructPath
			: !Entry.GetType().AllowedClassPath.IsEmpty() ? Entry.GetType().AllowedClassPath
			: Entry.GetType().Kind.IsEmpty()			 ? FString(TEXT("uai.authoring.value"))
										 : Entry.GetType().Kind;
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
		FUeAgentStateDetails Details(DisplayName, bIncludeAuthoringMetadata ? Entry.Tooltip : FString(),
			bIncludeAuthoringMetadata ? Entry.GetCategoryPath() : TArray<FString>(),
			Entry.bEditable,
			Entry.bAvailable				 ? FString(TEXT("available"))
			: Entry.AvailabilityReason.IsEmpty() ? FString(TEXT("unavailable"))
									 : Entry.AvailabilityReason,
			bIncludeAuthoringMetadata ? Entry.GetConstraints() : TMap<FString, FString>());
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
		FUeAgentCanonicalValuePtr AuthoringDescriptor;
		TOptional<decltype(Resources.Snapshot())> DescriptorBefore;
		if (bRecordReuseMaterial) DescriptorBefore.Emplace(Resources.Snapshot());
		if (bIncludeAuthoringMetadata) {
			const auto Metadata = MakeShared<FJsonObject>();
			Metadata->SetObjectField(TEXT("type"), Entry.GetType().ToJsonObject());
			if (Entry.ValueDefinition) Metadata->SetField(TEXT("value_definition"), Entry.ValueDefinition);
			if (Entry.CandidatesHeader) Metadata->SetField(TEXT("candidates_header"), Entry.CandidatesHeader);
			if (Entry.Candidates) Metadata->SetField(TEXT("candidates"), Entry.Candidates);
			const auto CurrentDescriptor = MakeShared<FJsonValueObject>(Metadata);
			const bool bSameCurrentDescriptor = !bSameObservation && Old && Old->Descriptor
				&& CanonicalJson::MatchesCanonicalProjection(CurrentDescriptor, *Old->Descriptor);
			if (bSameObservation || bSameCurrentDescriptor) {
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(Old->DescriptorCharges, TEXT("composition:actual_unchanged_descriptor"), ResourceError))
				{ OutError.SetFailure(ResourceError); return false; }
				AuthoringDescriptor = Old->Descriptor;
				if (bSameCurrentDescriptor && ExactDescriptorReuses) ++*ExactDescriptorReuses;
			} else {
				if (!TryCanonicalizeJson(CurrentDescriptor, Resources, AuthoringDescriptor, OutError)) return false;
				FUeAgentCanonicalValuePtr Normalized;
				FUeAgentStateSemanticError DefinitionError;
				if (!TryNormalizePropertyDefinition(AuthoringDescriptor.ToSharedRef(), Resources, Normalized, DefinitionError))
				{ OutError.SetFailure(MoveTemp(DefinitionError)); return false; }
				AuthoringDescriptor = MoveTemp(Normalized);
			}
		}
		if (DescriptorBefore.IsSet()) {
			Proof.Descriptor = AuthoringDescriptor;
			Proof.DescriptorCharges = CaptureChargesBetween(DescriptorBefore.GetValue(), Resources.Snapshot());
		}
		FUeAgentStateEntry CanonicalEntry(MoveTemp(Identity), MoveTemp(Type),
			Value.ToSharedRef(), MoveTemp(Default), MoveTemp(Details), Revision,
			EUeAgentAuthoredPresence::Present, EUeAgentOverrideState::None, MoveTemp(AuthoringDescriptor));
		if (!CanonicalEntry.IsValid())
		{
			return ForwardCaptureFailure(CanonicalEntry.GetValidationDiagnostic());
		}
		OutEntry.Emplace(MoveTemp(CanonicalEntry));
		return true;
	}

	static bool TryBuildCanonicalAuthoringCaptureFull(const FString& TargetKind, const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const TArray<UeAgentAuthoringState::FStateEntry>& AdditionalEntries, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, const bool bCompleteCapture = true, const bool bComposeModel = true,
		bool bIncludeAuthoringMetadata = true, bool bBuildLegacyFingerprint = true)
	{
		const double CaptureStarted = FPlatformTime::Seconds();
		const auto CaptureUsageBefore = Resources.Snapshot();
		OutSnapshot.Reset();
		OutModel.Reset();
		OutError.Reset();
		const auto NativeEntries = Snapshot.ReadAuthoringEntries();
		const int32 SourceCount = NativeEntries.Num() + AdditionalEntries.Num();
		const auto EntryAt = [&](int32 Index) -> const UeAgentAuthoringState::FStateEntry& {
			return Index < NativeEntries.Num() ? NativeEntries[Index] : AdditionalEntries[Index - NativeEntries.Num()];
		};
		if (TargetKind.IsEmpty() || SourceCount == 0)
		{
			OutError.SetSourceFailure(TEXT("authoring_capture_contract_invalid"), TargetKind.IsEmpty() ? TEXT("Authoring capture requires a target kind.") : TEXT("Authoring capture returned no state entries."));
			return false;
		}

		const double SourcePrepared = FPlatformTime::Seconds();
		const FString EffectiveProfile = ProfileId.IsEmpty() ? TEXT("llm_authoring") : ProfileId;
		const auto Previous = bCompleteCapture && bComposeModel ? PreviousCanonicalCapture : nullptr;
		const bool bSameCapture = Previous && Previous->TargetKind == TargetKind
			&& Previous->ProfileId == EffectiveProfile;
		TSharedPtr<FCompletedCanonicalCapture> Next;
		if (bComposeModel) { Next = MakeShared<FCompletedCanonicalCapture>(); Next->TargetKind = TargetKind; Next->ProfileId = EffectiveProfile; }
		bool bAllUnchanged = bSameCapture && Previous->Entries.Num() == SourceCount;
		bool bSameEntrySet = bAllUnchanged;
		TArray<int32> CanonicalOrder;
		if (bSameCapture) CanonicalOrder.Init(INDEX_NONE, SourceCount);
		int32 ReusedEntries = 0;
		int32 ExactValueReuses = 0, ExactDescriptorReuses = 0;
		FString RevisionMaterial = TargetKind;
		for (int32 EntryIndex = 0; EntryIndex < SourceCount; ++EntryIndex)
		{
			const auto& Entry = EntryAt(EntryIndex);
			const FString Key = CaptureEntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey);
			const auto* Old = bSameCapture ? Previous->Entries.Find(Key) : nullptr;
			const bool bSameObservation = Old && Entry.Observation && Old->Observation == Entry.Observation;
			bAllUnchanged &= bSameObservation;
			const bool bKnownIndex = Old && CanonicalOrder.IsValidIndex(Old->CanonicalIndex)
				&& CanonicalOrder[Old->CanonicalIndex] == INDEX_NONE;
			bSameEntrySet &= bKnownIndex;
			if (bKnownIndex) CanonicalOrder[Old->CanonicalIndex] = EntryIndex;
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
			if (Next) Next->Entries.Add(Key, MoveTemp(Proof));
		}
		bSameEntrySet &= Next && Next->Entries.Num() == SourceCount;
		const FString BaseRevision = TEXT("capture:") + Sha256Text(RevisionMaterial).Right(24);
		const FUeAgentStateRevision Revision(bCompleteCapture ? TEXT("authoring.capture") : TEXT("authoring.capture.scoped"), BaseRevision, 0);
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
			if (!TryBindAcceptedDefinitionProjection(OutModel, OutError)) return false;
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=canonical_capture kind=%s entries=%d reused_entries=%d source_prepare_s=%.6f revision_s=%.6f canonical_s=0 compose_s=0 total_s=%.6f success=true"),
				*TargetKind, SourceCount, SourceCount, SourcePrepared - CaptureStarted,
				RevisionFinished - SourcePrepared, FPlatformTime::Seconds() - CaptureStarted);
			return true;
		}

		TArray<FUeAgentStateEntry> CanonicalEntries;
		CanonicalEntries.Reserve(SourceCount);
		TMap<const FJsonValue*, FObservedCanonicalValue> ObservedValues;
		for (int32 OrderedIndex = 0; OrderedIndex < SourceCount; ++OrderedIndex)
		{
			const auto& Entry = EntryAt(bSameEntrySet ? CanonicalOrder[OrderedIndex] : OrderedIndex);
			const FString Key = CaptureEntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey);
			FCanonicalEntryObservation LocalProof;
			auto& Proof = Next ? Next->Entries.FindChecked(Key) : LocalProof;
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
				FUeAgentStateEntry Captured = Before.WithIdentityAndDefault(MoveTemp(Identity), MoveTemp(Default), Revision);
				if (!Captured.IsValid()) { OutError.SetFailure(Captured.GetValidationDiagnostic(), Entry.TargetIdentity.CanonicalKey); return false; }
				Proof.Value = Old->Value; Proof.Descriptor = Old->Descriptor; Proof.DescriptorCharges = Old->DescriptorCharges;
				if (ObservedValues.Num() < 200000) ObservedValues.Add(Entry.Value.Get(), Proof.Value);
				CanonicalEntries.Add(MoveTemp(Captured)); ++ReusedEntries;
				continue;
			}
			TOptional<FUeAgentStateEntry> CapturedEntry;
			if (!TryCaptureCanonicalEntry(Entry, Old, BaseRevision, Revision, Resources, Proof,
				CapturedEntry, bComposeModel && ObservedValues.Num() < 200000 ? &ObservedValues : nullptr, ReusedEntries, OutError,
				&ExactValueReuses, &ExactDescriptorReuses, bIncludeAuthoringMetadata, bComposeModel)) return false;
			CanonicalEntries.Add(MoveTemp(CapturedEntry.GetValue()));
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
			MoveTemp(CanonicalEntries),
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

		if (!bComposeModel) {
			UE_LOG(LogTemp, Display, TEXT("UAI read timing phase=canonical_observation entries=%d authoring_descriptors=%s complete_observation=%s model=false entry_copies=0 total_s=%.6f"),
				SourceCount, bIncludeAuthoringMetadata ? TEXT("true") : TEXT("false"), bCompleteCapture ? TEXT("true") : TEXT("false"), FPlatformTime::Seconds() - CaptureStarted);
			return true;
		}
		const double CanonicalFinished = FPlatformTime::Seconds();
		const auto& Captured = OutSnapshot->GetEntries();
		for (int32 Index = 0; Index < Captured.Num(); ++Index) {
			const auto& Identity = Captured[Index].GetIdentity();
			if (auto* Proof = Next->Entries.Find(CaptureEntryKey(Identity.GetSemanticId(),
				Identity.GetSelector(CanonicalAuthoringTargetIdentitySelector)))) Proof->CanonicalIndex = Index;
		}
		FCompositionRequest Request;
		Request.TargetKind = TargetKind;
		Request.ProfileId = ProfileId.IsEmpty()
			? TEXT("llm_authoring") : ProfileId;
		Request.BaseRevision = BaseRevision;
		Request.SourceProvenance = TEXT("authoring_state.capture");
		// Only the real Model consumer needs owned entry descriptors. Pure
		// readback above borrows the sealed native snapshot through conversion.
		Request.Entries.Reserve(SourceCount);
		for (int32 Index = 0; Index < SourceCount; ++Index) Request.Entries.Add(EntryAt(Index));
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!RegisterGraphTreeTargetCompositionContracts(ContractError)
			|| !RegisterOtherTargetCompositionContracts(ContractError))
		{
			OutError.SetFailure(ContractError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_composition_contract_registration_failed"), TEXT("Target composition contract registration failed without returning a diagnostic.")) : ContractError, FString());
			return false;
		}
		const double CompositionPrepared = FPlatformTime::Seconds();
		const auto ProjectionHints = GetCompositionDefinitionProjectionHints(Request.TargetKind, Request.ProfileId);
		// This caller explicitly requested a complete model. It has one
		// complete composer; an optional value-only attempt no longer decides
		// whether to run a second implementation.
		const bool bComposed = TryComposeAuthoringModelFromCapture(Request, Resources, OutModel,
			OutError, &ObservedValues, ProjectionHints.Get(), bBuildLegacyFingerprint);
		const double CompositionFinished = FPlatformTime::Seconds();
		if (bComposed)
		{
			Next->Snapshot = OutSnapshot; Next->Model = OutModel;
			Next->Charges = CaptureChargesBetween(CaptureUsageBefore, Resources.Snapshot());
			if (bCompleteCapture) PreviousCanonicalCapture = MoveTemp(Next);
		}
		const double CaptureFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=canonical_current_material_reuse value_matches=%d descriptor_matches=%d current_entries=%d observation_reuses=%d"),
			ExactValueReuses, ExactDescriptorReuses, Request.Entries.Num(), ReusedEntries - ExactValueReuses);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=canonical_capture kind=%s entries=%d reused_entries=%d source_prepare_s=%.6f revision_s=%.6f canonical_s=%.6f entry_s=%.6f snapshot_s=%.6f compose_s=%.6f compose_prepare_s=%.6f compose_model_s=%.6f compose_finalize_s=%.6f total_s=%.6f success=%s"),
			*TargetKind, Request.Entries.Num(), ReusedEntries, SourcePrepared - CaptureStarted,
			RevisionFinished - SourcePrepared, CanonicalFinished - RevisionFinished,
			EntriesFinished - RevisionFinished, CanonicalFinished - EntriesFinished,
			CaptureFinished - CanonicalFinished, CompositionPrepared - CanonicalFinished,
			CompositionFinished - CompositionPrepared, CaptureFinished - CompositionFinished, CaptureFinished - CaptureStarted,
			bComposed ? TEXT("true") : TEXT("false"));
		return bComposed;
	}

	bool TryBuildCanonicalAuthoringCapture(const FString& TargetKind, const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot, FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		return TryBuildCanonicalAuthoringCapture(TargetKind, ProfileId, Snapshot, Resources,
			OutSnapshot, OutModel, OutError, true);
	}

	bool TryBuildCanonicalAuthoringCapture(const FString& TargetKind, const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot, FAuthoringModelPtr& OutModel, FWorkspaceError& OutError,
		bool bBuildLegacyFingerprint)
	{
		const double Started = FPlatformTime::Seconds();
		OutSnapshot.Reset(); OutModel.Reset(); OutError.Reset();
		TArray<UeAgentAuthoringState::FStateEntry> Additional;
		UE::UAI::Contract::FDiagnostic PreviewError;
		if (!AssetPreview::CaptureAdditionalState(TargetKind, Snapshot.ReadAuthoringEntries(), Additional, PreviewError))
		{
			OutError.SetFailure(PreviewError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_preview_capture_failed"), TEXT("Asset preview capture failed without returning a diagnostic.")) : PreviewError, TargetKind);
			return false;
		}
		// This public entry owns an explicitly complete capture. Select that
		// algorithm from its input contract, never after a block-cache failure.
		const bool bCaptured = TryBuildCanonicalAuthoringCaptureFull(TargetKind, ProfileId, Snapshot, Additional, Resources, OutSnapshot, OutModel, OutError,
			true, true, true, bBuildLegacyFingerprint);
		if (bCaptured) RetainBoundedCanonicalRows();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=canonical_capture_dispatch path=explicit_complete total_s=%.6f success=%s"),
			FPlatformTime::Seconds() - Started, bCaptured ? TEXT("true") : TEXT("false"));
		return bCaptured;
	}

	bool TryBuildDirectAuthoringCapture(const FString& TargetKind, const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot, FUeAgentResourceTracker& Resources,
		TOptional<FCompositionSnapshotBinding>& OutBinding, FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		const double Started = FPlatformTime::Seconds();
		OutBinding.Reset(); OutModel.Reset(); OutError.Reset();
		FCompositionRequest Request;
		Request.TargetKind = TargetKind; Request.ProfileId = ProfileId.IsEmpty() ? TEXT("llm_authoring") : ProfileId;
		Request.SourceProvenance = TEXT("authoring_state.capture");
		Contract::FDiagnostic Error;
		if (!AssetPreview::CaptureAdditionalState(TargetKind, Snapshot.ReadAuthoringEntries(), Request.Entries, Error)
			|| !RegisterGraphTreeTargetCompositionContracts(Error) || !RegisterOtherTargetCompositionContracts(Error)) {
			OutError.SetFailure(MoveTemp(Error), TargetKind); return false;
		}
		auto Binding = BuildNativeAuthoringCaptureBinding(TargetKind, Snapshot);
		if (!Request.Entries.IsEmpty()) {
			// Additional preview observations are part of the same original full
			// revision grammar, never an alternate capture or execution algorithm.
			FString Material = TargetKind;
			for (const auto& Entry : Snapshot.ReadAuthoringEntries()) {
				Material += TEXT("|") + Entry.SemanticId + TEXT("|") + Entry.TargetIdentity.CanonicalKey + TEXT("|");
				UeAgentAuthoringState::VisitStableCanonicalValueMaterialView(Entry,
					[&](FStringView Text) { Material.AppendChars(Text.GetData(), Text.Len()); });
			}
			for (const auto& Entry : Request.Entries) {
				Material += TEXT("|") + Entry.SemanticId + TEXT("|") + Entry.TargetIdentity.CanonicalKey
					+ TEXT("|") + UeAgentAuthoringState::MakeStableCanonicalValueKey(Entry.Value);
			}
			const FString Generation = TEXT("capture:") + Sha256Text(Material).Right(24);
			const FString Id = TEXT("authoring.capture:") + Sha256Text(TargetKind + TEXT("|") + Generation).Right(24);
			Binding = {Id, FUeAgentStateRevision(TEXT("authoring.capture"), Generation, 0), TEXT("independent-readback:") + Id};
		}
		Request.BaseRevision = Binding.Revision.GetGeneration();
		const double Bound = FPlatformTime::Seconds();
		const auto Hints = GetCompositionDefinitionProjectionHints(TargetKind, Request.ProfileId);
		const bool bComposed = TryComposeAuthoringModelFromCapture(Request, Resources, OutModel, OutError,
			nullptr, Hints.Get(), false, &Snapshot);
		if (bComposed) OutBinding.Emplace(MoveTemp(Binding));
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=direct_capture_model entries=%d canonical_snapshot_entries=0 native_entry_copies=0 binding_s=%.6f model_s=%.6f total_s=%.6f success=%s"),
			Snapshot.Num() + Request.Entries.Num(), Bound - Started, FPlatformTime::Seconds() - Bound,
			FPlatformTime::Seconds() - Started, bComposed ? TEXT("true") : TEXT("false"));
		return bComposed;
	}

	static bool BindScopedOrdering(FAuthoringModelPtr& OutModel, FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError, const FAuthoringModel* AcceptedBindingBaseline);

	bool TryBuildCanonicalAuthoringSnapshot(const FString& TargetKind, const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot, FWorkspaceError& OutError, bool bIncludeAuthoringMetadata, bool bCompleteObservation)
	{
		FAuthoringModelPtr Unused;
		return TryBuildCanonicalAuthoringCaptureFull(TargetKind, ProfileId, Snapshot, {}, Resources, OutSnapshot, Unused, OutError, bCompleteObservation, false, bIncludeAuthoringMetadata);
	}
	bool TryBuildVisibleAuthoringProjection(const FString& TargetKind, const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot, FWorkspaceError& OutError)
	{
		const auto Entries = Snapshot.ReadAuthoringEntries();
		int32 FirstHidden = 0;
		while (FirstHidden < Entries.Num() && Entries[FirstHidden].bAuthoringVisible) ++FirstHidden;
		if (FirstHidden == Entries.Num())
			return TryBuildCanonicalAuthoringSnapshot(TargetKind, ProfileId, Snapshot, Resources, OutSnapshot, OutError);

		TArray<int32> Visible;
		Visible.Reserve(Entries.Num() - 1);
		for (int32 Index = 0; Index < FirstHidden; ++Index) Visible.Add(Index);
		for (int32 Index = FirstHidden + 1; Index < Entries.Num(); ++Index)
			if (Entries[Index].bAuthoringVisible) Visible.Add(Index);
		UeAgentAuthoringState::FStateSnapshot Selected;
		if (!Snapshot.TrySelectEntries(Visible, Selected))
		{
			OutError.SetSourceFailure(TEXT("authoring_projection_selection_invalid"), TEXT("Visible entries could not be bound to this observation."));
			return false;
		}
		return TryBuildCanonicalAuthoringSnapshot(TargetKind, ProfileId, Selected, Resources, OutSnapshot, OutError);
	}
	bool TryBuildVisibleAuthoringProjection(UeAgentAuthoringState::FStateEntry Entry,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot, FWorkspaceError& OutError)
	{
		TArray<UeAgentAuthoringState::FStateEntry> Entries;
		if (!Entry.bAuthoringVisible) {
			OutError.SetSourceFailure(TEXT("authoring_projection_entry_not_visible"), TEXT("A standalone public authoring observation must be visible."));
			return false;
		}
		Entries.Add(MoveTemp(Entry));
		const UeAgentAuthoringState::FStateSnapshot Empty;
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		FAuthoringModelPtr Unused;
		return TryBuildCanonicalAuthoringCaptureFull(Entries[0].TargetIdentity.TargetKind, TEXT("llm_authoring"),
			Empty, Entries, Resources, OutSnapshot, Unused, OutError, false, false);
	}
	bool TryVisitSnapshotKeyedMemberNames(const FString& TargetKind, const FUeAgentCanonicalSnapshot& Snapshot,
		const FString& ItemSemanticId, const FString& RootField, FKeyedMemberNameVisitor Visitor, FWorkspaceError& OutError)
	{
		FKeyedItemProjectionContract Contract;
		if (!TryGetKeyedItemProjectionContract(RootField, Contract)) {
			OutError.SetSourceFailure(TEXT("keyed_item_projection_contract_missing"), TEXT("The observed namespace has no keyed naming contract."), RootField); return false;
		}
		struct FName { FString Id; const FUeAgentStateEntry* Entry; const FString* Name; };
		TArray<FName> Items;
		for (const auto& Entry : Snapshot.GetEntries()) {
			if (Entry.GetIdentity().GetSemanticId() != ItemSemanticId) continue;
			const FString* Name = &Contract.NamingPolicy.FallbackName;
			if (Entry.GetValue()->GetKind() == EUeAgentValueKind::Object) for (const auto& Field : Contract.NamingPolicy.Fields) {
				const auto* Value = Entry.GetValue()->GetObjectItems().FindByPredicate([&](const auto& Item) { return Item.Key == Field; });
				if (Value && Value->Value->GetKind() == EUeAgentValueKind::String && !Value->Value->GetText().IsEmpty()) {
					Name = &Value->Value->GetText(); break;
				}
			}
			Items.Add({CompositionEntryUnitId(false, TargetKind, ItemSemanticId, CanonicalAuthoringTargetKey(Entry)), &Entry, Name});
		}
		Items.Sort([](const FName& A, const FName& B) { return A.Id < B.Id; });
		UE_LOG(LogTemp, Display, TEXT("UAI read timing phase=keyed_observed_names root=%s members=%d full_model=false"), *RootField, Items.Num());
		const TMap<FString, FString> BoundAliases;
		TSet<FString> UsedAliases, ReservedAliases;
		return TryAllocateKeyedMemberNames(Items.Num(), [&](int32 Index) -> const FString& { return Items[Index].Id; },
			[&](int32 Index) -> const FString& { return *Items[Index].Name; }, BoundAliases, UsedAliases, ReservedAliases, OutError,
			[&](int32 Index, FString Alias) {
				const auto& Item = Items[Index]; const auto& Identity = Item.Entry->GetIdentity();
				Visitor(Alias, Item.Id, Identity.GetSelector(TEXT("actor_guid")), Identity.GetSelector(TEXT("managed_id")), Identity.GetSelector(TEXT("component_name")));
			});
	}

	bool TryBuildScopedCanonicalAuthoringCapture(const FString& TargetKind, const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot, FAuthoringModelPtr& OutModel, FWorkspaceError& OutError,
		const FAuthoringModel* AcceptedBindingBaseline, const FCompositionUnitLookup& KeyedBaseline, const FKeyedMemberNameLookup& KeyedNames)
	{
		FCompositionUnitSet Observed;
		if (!TryBuildCanonicalAuthoringSnapshot(TargetKind, ProfileId, Snapshot, Resources, OutSnapshot, OutError)
			|| !TryComposeScopedAuthoringUnits(TargetKind, ProfileId, OutSnapshot, Resources, Observed, OutError, AcceptedBindingBaseline, KeyedBaseline, KeyedNames)) return false;
		// Import needs indexed observed facts, not an exported complete graph or
		// its whole-model fingerprint. No unobserved source values enter this view.
		OutModel = FAuthoringModel::MakePropertySourceView(MoveTemp(Observed.ContractId), Observed.ContractVersion,
			MoveTemp(Observed.ProfileId), MoveTemp(Observed.BaseRevision), MoveTemp(Observed.Units),
			MoveTemp(Observed.TargetUnitIds), OutSnapshot->GetSnapshotId());
		return true;
	}

	static bool BindScopedSnapshot(FUeAgentCanonicalSnapshotPtr& Snapshot, FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError, const FAuthoringModel* AcceptedBindingBaseline)
	{
		if (AcceptedBindingBaseline) {
			// Generation compatibility needs declaration identity only. It must
			// not decode every value sharing an observed semantic type.
			TMap<FString, TArray<FSemanticUnit::FDeclarationView>> Bindings;
			for (const auto& Entry : Snapshot->GetEntries())
				if (Entry.GetIdentity().GetSelector(CanonicalAuthoringTargetIdentitySelector).IsEmpty())
					Bindings.FindOrAdd(Entry.GetIdentity().GetSemanticId());
			int32 DeclarationRows = 0;
			if (!Bindings.IsEmpty()) AcceptedBindingBaseline->VisitDeclarations([&](const auto& Declaration, int32) {
				++DeclarationRows;
				if (auto* Rows = Bindings.Find(Declaration.GetSemanticTypeRef()); Rows && !Declaration.GetTargetIdentityKey().IsEmpty())
					Rows->Add(Declaration);
				return true;
			});
			TArray<FUeAgentStateEntry> Entries;
			bool bChanged = false;
			int32 Rebound = 0;
			for (int32 Index = 0; Index < Snapshot->NumEntries(); ++Index) {
				const auto& Entry = Snapshot->GetEntries()[Index];
				if (!Entry.GetIdentity().GetSelector(CanonicalAuthoringTargetIdentitySelector).IsEmpty()) continue;
				TOptional<FUeAgentCanonicalIdentity> Identity;
				bool bAmbiguous = false;
				for (const auto& Declaration : Bindings.FindChecked(Entry.GetIdentity().GetSemanticId())) {
					const auto Bound = Entry.GetIdentity().WithCapturedBinding(Declaration.GetTargetIdentityKey());
					if (!Bound.IsSet()) continue;
					if (Identity.IsSet() && Identity->GetCanonicalKey() != Bound->GetCanonicalKey()) { bAmbiguous = true; break; }
					Identity = Bound;
				}
				if (Identity.IsSet() && !bAmbiguous && Identity->GetCanonicalKey() != Entry.GetIdentity().GetCanonicalKey()) {
					if (!bChanged) Entries = Snapshot->GetEntries();
					bChanged = true; ++Rebound;
					Entries[Index] = Entry.WithIdentityAndDefault(Identity.GetValue(), Entry.GetDefault(), Entry.GetObservedRevision());
				}
			}
			if (bChanged) {
				FUeAgentCanonicalSnapshotPtr Bound;
				StateSemantic::FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalSnapshot::TryCreate(Snapshot->GetSnapshotId(), Snapshot->GetStatePlane(),
					Snapshot->GetRevision(), Snapshot->GetCapturedAt(), Snapshot->GetProvenance(),
					MoveTemp(Entries), Resources, Bound, Error)) {
					OutError.SetFailure(MoveTemp(Error)); return false;
				}
				Snapshot = MoveTemp(Bound);
			}
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=scoped_binding_directory declarations=%d observed_entries=%d rebound=%d typed_value_rows=0 copied_entries=%d"),
				DeclarationRows, Snapshot->NumEntries(), Rebound, bChanged ? Snapshot->NumEntries() : 0);
		}
		return true;
	}

	static bool ComposeScopedSnapshot(const FString& TargetKind, const FString& ProfileId,
		FUeAgentCanonicalSnapshotPtr& Snapshot, FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel, FCompositionUnitSet* OutUnits, FWorkspaceError& OutError,
		const FAuthoringModel* AcceptedBindingBaseline, const FCompositionUnitLookup* KeyedBaseline = nullptr,
		const FKeyedMemberNameLookup* KeyedNames = nullptr)
	{
		if (!BindScopedSnapshot(Snapshot, Resources, OutError, AcceptedBindingBaseline)) return false;
		FCanonicalCompositionRequest Request;
		Request.TargetKind = TargetKind; Request.ProfileId = ProfileId;
		Request.BaseRevision = Snapshot->GetRevision().GetGeneration();
		// The native capture producer uses a stable source owner in semantic
		// rows. Its per-observation evidence belongs to Snapshot/Binding, not
		// every declaration's source string. Keep both output forms identical.
		const auto& Provenance = Snapshot->GetProvenance();
		Request.SourceProvenance = Provenance.GetSourceKind() == TEXT("independent_readback")
			&& Provenance.GetSourceId() == TargetKind ? TEXT("authoring_state.capture")
			: Provenance.GetSourceId() + TEXT(":") + Provenance.GetEvidenceId();
		Request.Entries = Snapshot->GetEntries();
		Contract::FDiagnostic Registration;
		if (!RegisterGraphTreeTargetCompositionContracts(Registration) || !RegisterOtherTargetCompositionContracts(Registration)) {
			OutError.SetFailure(MoveTemp(Registration)); return false;
		}
		return ComposeCanonicalAuthoringProjection(Request, Resources, OutModel, OutError, OutUnits, AcceptedBindingBaseline, KeyedBaseline, KeyedNames);
	}

bool IsStableItemOrdering(const FSemanticUnit& Ordering, const FString& OwnerId)
{
	return Ordering.GetUnitKind() == ESemanticUnitKind::OrderingState
		&& Ordering.GetRole() == TEXT("stable_item_order") && Ordering.GetSemanticTypeRef() == TEXT("uai.authoring.ordering/1")
		&& Ordering.GetOwnerRef() == OwnerId && Ordering.GetUnitId() == StableId(TEXT("ordering"), OwnerId);
}

static bool BindScopedUnitOrdering(TArray<FSemanticUnit>& Units, FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError, const FAuthoringModel* AcceptedBindingBaseline)
	{
		if (!AcceptedBindingBaseline) return true;
		TArray<FSemanticUnit> EmptyOrderings;
		TSet<FString> PropertySurfaces;
		for (const auto& Unit : Units) if (Unit.GetUnitKind() == ESemanticUnitKind::TargetState && Unit.GetRole() == TEXT("authoring_target")) {
			FTargetCompositionContract Contract;
			if (FTargetCompositionRegistry::Get().FindContract(Unit.GetSemanticTypeRef(), Contract))
				for (const auto& Surface : Contract.Surfaces) if (Surface.SurfaceKind == EAuthoringSurfaceKind::Properties)
					PropertySurfaces.Add(TEXT("uai.authoring.surface.") + Surface.SurfaceId);
			break;
		}
		for (auto& Unit : Units) {
			if (!Unit.GetItemRefs().IsEmpty() || !Unit.GetOrderingRef().IsEmpty()) continue;
			// These original producers emit field values/schema or asset-path
			// dependencies, never owned item sequences. There is no old ordering
			// to inherit, even when the current observation contains no values.
			if ((Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
					&& (Unit.GetSemanticTypeRef().EndsWith(TEXT(".property"))
						|| PropertySurfaces.Contains(Unit.GetSemanticTypeRef())))
				|| (Unit.GetUnitKind() == ESemanticUnitKind::OrderingState
					&& Unit.GetRole() == TEXT("stable_item_order")
					&& Unit.GetSemanticTypeRef() == TEXT("uai.authoring.ordering/1"))
				|| (Unit.GetUnitKind() == ESemanticUnitKind::DependencyState
					&& Unit.GetSemanticTypeRef() == TEXT("uai.authoring.dependencies/1"))
				|| (Unit.GetUnitKind() == ESemanticUnitKind::CollectionState
					&& Unit.GetRole() == TEXT("property_definition")
					&& Unit.GetSemanticTypeRef() == TEXT("uai.authoring.property_definition/1"))) continue;
			const auto Accepted = AcceptedBindingBaseline->FindDeclaration(Unit.GetUnitId());
			if (!Accepted || Accepted->GetOrderingRef().IsEmpty()) continue;
			const auto* Ordering = AcceptedBindingBaseline->FindUnit(Accepted->GetOrderingRef());
			if (!Ordering || !IsStableItemOrdering(*Ordering, Unit.GetUnitId())) continue;
			// An empty observation subset is not removal of the accepted binding.
			// Keep its established ordering identity with *zero observed members*;
			// committed membership merge supplies untouched baseline members later.
			FUeAgentCanonicalValuePtr Empty;
			if (!TryStringArrayValue({}, Resources, Empty, OutError, Ordering->GetUnitId(), TEXT("item_refs"))) return false;
			EmptyOrderings.Add(Ordering->WithValues({{TEXT("item_refs"), Empty.ToSharedRef()}}));
			Unit = FSemanticUnit(Unit.GetUnitId(), Unit.GetUnitKind(), Unit.GetOwnerRef(), Unit.GetSemanticTypeRef(),
				Unit.GetRole(), Unit.GetFieldSetRef(), Unit.GetValues(), Unit.GetItemRefs(), Unit.GetRelationRefs(), Ordering->GetUnitId(),
				Unit.GetLayoutRef(), Unit.GetDependencyRefs(), Unit.GetProfileVisibility(), Unit.GetSourceProvenance(),
				Unit.GetTargetIdentityKey(), Unit.GetCategoryPath());
		}
		if (EmptyOrderings.IsEmpty()) return true;
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=scoped_empty_ordering bindings=%d observed_members=0"), EmptyOrderings.Num());
		Units.Append(MoveTemp(EmptyOrderings));
		return true;
	}

	bool TryComposeScopedAuthoringUnits(const FString& TargetKind, const FString& ProfileId,
		FUeAgentCanonicalSnapshotPtr& Snapshot, FUeAgentResourceTracker& Resources,
		FCompositionUnitSet& OutUnits, FWorkspaceError& OutError, const FAuthoringModel* AcceptedBindingBaseline,
		const FCompositionUnitLookup& KeyedBaseline, const FKeyedMemberNameLookup& KeyedNames)
	{
		FAuthoringModelPtr Unused;
		return ComposeScopedSnapshot(TargetKind, ProfileId, Snapshot, Resources, Unused, &OutUnits, OutError, AcceptedBindingBaseline, &KeyedBaseline, &KeyedNames)
			&& BindScopedUnitOrdering(OutUnits.Units, Resources, OutError, AcceptedBindingBaseline);
	}

	bool TryBuildScopedCanonicalAuthoringCapture(const FString& TargetKind, const FString& ProfileId,
		FUeAgentCanonicalSnapshotPtr& Snapshot, FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel, FWorkspaceError& OutError, const FAuthoringModel* AcceptedBindingBaseline)
	{
		return ComposeScopedSnapshot(TargetKind, ProfileId, Snapshot, Resources, OutModel, nullptr, OutError, AcceptedBindingBaseline)
			&& BindScopedOrdering(OutModel, Resources, OutError, AcceptedBindingBaseline);
	}

	static bool BindScopedOrdering(FAuthoringModelPtr& OutModel, FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError, const FAuthoringModel* AcceptedBindingBaseline)
	{
		if (!AcceptedBindingBaseline) return true;
		TArray<FSemanticUnit> Units = OutModel->GetUnits();
		const int32 OriginalCount = Units.Num();
		if (!BindScopedUnitOrdering(Units, Resources, OutError, AcceptedBindingBaseline)) return false;
		if (Units.Num() == OriginalCount) return true;
		const auto Current = OutModel;
		return TryCompileAuthoringModel(Current->GetContractId(), Current->GetContractVersion(), Current->GetProfileId(),
			Current->GetBaseRevision(), MoveTemp(Units), Current->GetTargetUnitIds(), Resources, OutModel, OutError,
			nullptr, Current.Get(), nullptr);
	}

	FCompositionSnapshotBinding BuildNativeAuthoringCaptureBinding(const FString& TargetKind,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot)
	{
		// The snapshot owner has already observed current values/references and
		// preserves these ordered tokens only for unchanged immutable authoring
		// content. Reuse the result of that exact input, not a native-read promise.
		struct FBindingBlockProof {
			int32 FirstIndex = 0, NumEntries = 0;
			TWeakPtr<const uint8> Observation;
		};
		struct FBindingResultProof {
			explicit FBindingResultProof(FCompositionSnapshotBinding InResult) : Result(MoveTemp(InResult)) {}
			FString TargetKind;
			int32 NumEntries = 0;
			TWeakPtr<const uint8> Membership;
			TArray<FBindingBlockProof> Blocks;
			FCompositionSnapshotBinding Result;
		};
		// One candidate, weak proof identities only: no snapshot/JSON/native
		// values or UObject roots are kept alive by this result memo.
		static thread_local TOptional<FBindingResultProof> Previous;
		const auto& Membership = Snapshot.GetObservationMembership();
		const auto& Blocks = Snapshot.GetObservationBlocks();
		bool bReusable = Previous.IsSet() && Membership
			&& Previous->Membership.Pin() == Membership
			&& Previous->TargetKind.Equals(TargetKind, ESearchCase::CaseSensitive)
			&& Previous->NumEntries == Snapshot.Num() && Previous->Blocks.Num() == Blocks.Num();
		int32 Covered = 0;
		for (int32 Index = 0; bReusable && Index < Blocks.Num(); ++Index) {
			const auto& Block = Blocks[Index]; const auto& Prior = Previous->Blocks[Index];
			bReusable = Block.FirstIndex == Covered && Block.NumEntries > 0
				&& Block.NumEntries <= Snapshot.Num() - Covered && Block.Observation
				&& Prior.FirstIndex == Block.FirstIndex && Prior.NumEntries == Block.NumEntries
				&& Prior.Observation.Pin() == Block.Observation;
			if (bReusable) Covered += Block.NumEntries;
		}
		if (bReusable && Covered == Snapshot.Num()) {
			UE_LOG(LogTemp, Display, TEXT("UAI capture binding entries=%d blocks=%d reused=true"), Snapshot.Num(), Blocks.Num());
			return Previous->Result;
		}
		Previous.Reset();
		SHA256_CTX Context;
		SHA256_Init(&Context);
		const auto Append = [&](FStringView Text) {
			int32 Length = 0;
			while (Length < Text.Len() && Text[Length] != TCHAR(0)) ++Length;
			const FTCHARToUTF8 Bytes(Text.GetData(), Length);
			SHA256_Update(&Context, reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
			return Length == Text.Len();
		};
		Append(FStringView(TargetKind));
		for (const auto& Entry : Snapshot.ReadAuthoringEntries()) {
			// The former single entry FString used terminated UTF-8 conversion.
			// Keep its first-NUL boundary across pieces, without changing the
			// stable value grammar or truncating the following entry.
			bool bTerminated = false;
			const auto AppendEntry = [&](FStringView Text) {
				if (!bTerminated) bTerminated = !Append(Text);
			};
			const auto Separator = [&]() {
				if (!bTerminated) { static constexpr uint8 Byte = '|'; SHA256_Update(&Context, &Byte, 1); }
			};
			Separator(); AppendEntry(FStringView(Entry.SemanticId)); Separator();
			AppendEntry(FStringView(Entry.TargetIdentity.CanonicalKey)); Separator();
			UeAgentAuthoringState::VisitStableCanonicalValueMaterialView(Entry, AppendEntry);
		}
		uint8 Digest[SHA256_DIGEST_LENGTH]; SHA256_Final(Digest, &Context);
		FString Hex;
		for (const uint8 Byte : Digest) Hex += FString::Printf(TEXT("%02x"), Byte);
		const FString Generation = TEXT("capture:") + Hex.Right(24);
		const FString SnapshotId = TEXT("authoring.capture:") + Sha256Text(TargetKind + TEXT("|") + Generation).Right(24);
		const FCompositionSnapshotBinding Result{SnapshotId, FUeAgentStateRevision(TEXT("authoring.capture"), Generation, 0), TEXT("independent-readback:") + SnapshotId};
		// Include weak control blocks that this memo may outlive and the fixed
		// digest-derived result strings, not just the array's inline elements.
		constexpr uint64 ProofBudget = 256ull * 1024ull;
		const uint64 EstimatedBytes = sizeof(FBindingResultProof) + TargetKind.GetAllocatedSize()
			+ static_cast<uint64>(Blocks.Num()) * (sizeof(FBindingBlockProof) + 128) + 1152;
		bool bRetained = false;
		if (Membership && EstimatedBytes <= ProofBudget) {
			FBindingResultProof Candidate(Result);
			Candidate.TargetKind = TargetKind; Candidate.NumEntries = Snapshot.Num(); Candidate.Membership = Membership;
			Candidate.Blocks.Reserve(Blocks.Num());
			Covered = 0;
			for (const auto& Block : Blocks) {
				if (Block.FirstIndex != Covered || Block.NumEntries <= 0
					|| Block.NumEntries > Snapshot.Num() - Covered || !Block.Observation) break;
				Candidate.Blocks.Add({Block.FirstIndex, Block.NumEntries, Block.Observation});
				Covered += Block.NumEntries;
			}
			const uint64 ActualBytes = sizeof(Candidate) + Candidate.TargetKind.GetAllocatedSize()
				+ Candidate.Blocks.GetAllocatedSize() + static_cast<uint64>(Candidate.Blocks.Num()) * 128 + 1152;
			if (Covered == Snapshot.Num() && Candidate.Blocks.Num() == Blocks.Num() && ActualBytes <= ProofBudget) {
				Previous = MoveTemp(Candidate); bRetained = true;
			}
		}
		UE_LOG(LogTemp, Display, TEXT("UAI capture binding entries=%d blocks=%d reused=false retained=%s"),
			Snapshot.Num(), Blocks.Num(), bRetained ? TEXT("true") : TEXT("false"));
		return Result;
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
		struct FUnitContentProof
		{
			FString Key;
			FSemanticUnit::FStorageIdentity StorageIdentity;
		};
		struct FPlanUnitProofBlock
		{
			int32 First = 0;
			TArray<FSemanticUnit::FStorageIdentity> Units;
			TArray<TPair<FString, FSemanticUnit::FStorageIdentity>> Dependencies;
			int64 Charge() const {
				int64 Bytes = sizeof(FPlanUnitProofBlock) + Units.GetAllocatedSize() + Dependencies.GetAllocatedSize()
					+ 32 + int64(Units.Num() + Dependencies.Num()) * 32; // weak control-block ownership, conservatively charged
				for (const auto& Dependency : Dependencies) Bytes += Dependency.Key.GetAllocatedSize();
				return Bytes;
			}
		};
		class FDocumentBuckets
		{
			struct FEntry { TSharedRef<const FString> Path; FString Bucket; };
			TMap<FWorkspacePlacementStorage::FKey, FEntry> Entries;
		public:
			int32 Num() const { return Entries.Num(); }
			FString* Find(const FString& Path) { auto* Entry = Entries.Find({&Path}); return Entry ? &Entry->Bucket : nullptr; }
			const FString* Find(const FString& Path) const { const auto* Entry = Entries.Find({&Path}); return Entry ? &Entry->Bucket : nullptr; }
			const FString& FindChecked(const FString& Path) const { const auto* Bucket = Find(Path); check(Bucket); return *Bucket; }
			FString& Add(TSharedRef<const FString> Path, FString Bucket) {
				const auto Key = FWorkspacePlacementStorage::FKey{&Path.Get()};
				return Entries.Add(Key, {MoveTemp(Path), MoveTemp(Bucket)}).Bucket;
			}
			int64 Charge() const {
				int64 Bytes = sizeof(FDocumentBuckets) + Entries.GetAllocatedSize();
				// Path storage is the layout document's real immutable input, shared
				// even when rows change. This index owns only its entry/bucket text.
				for (const auto& Pair : Entries) Bytes += Pair.Value.Bucket.GetAllocatedSize();
				return Bytes;
			}
		};
		class FMachineUnitIndices
		{
			TSharedRef<TArray<int32>> Indices = MakeShared<TArray<int32>>();
		public:
			FMachineUnitIndices() = default;
			FMachineUnitIndices(TArray<int32> Values) : Indices(MakeShared<TArray<int32>>(MoveTemp(Values))) {}
			void Add(int32 Index) { Indices->Add(Index); }
			int32 Num() const { return Indices->Num(); }
			bool IsEmpty() const { return Indices->IsEmpty(); }
			bool IsValidIndex(int32 Index) const { return Indices->IsValidIndex(Index); }
			int32 operator[](int32 Index) const { return (*Indices)[Index]; }
			int64 GetAllocatedSize() const { return sizeof(TArray<int32>) + 32 + Indices->GetAllocatedSize(); }
			TSharedRef<const TArray<int32>> GetSharedIndices() const { return Indices; }
			auto begin() const { return Indices->begin(); }
			auto end() const { return Indices->end(); }
		};
		struct FMachineBucketMembership
		{
			FDocumentBuckets DocumentBuckets;
			TMap<FString, FMachineUnitIndices> UnitIndices;
			int64 Charge() const {
				int64 Bytes = sizeof(FMachineBucketMembership) + 32 + DocumentBuckets.Charge() + UnitIndices.GetAllocatedSize();
				for (const auto& Pair : UnitIndices) Bytes += Pair.Key.GetAllocatedSize() + Pair.Value.GetAllocatedSize();
				return Bytes;
			}
		};
		FString MachineEnvelopePath(const FString& Document, bool bScopedOwners)
		{
			if (!bScopedOwners)
				return TEXT(".uai/baselines/") + Sha256Text(Document).Right(64).Left(2) + TEXT(".hjson");
			// The editable layout already gives every nested item a stable owner
			// directory. Keep that owner's machine rows together, rather than
			// scattering its documents through unrelated global hash buckets.
			const int32 Items = Document.Find(TEXT("/items/"), ESearchCase::CaseSensitive);
			const int32 End = Items == INDEX_NONE ? INDEX_NONE
				: Document.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Items + 7);
			const FString Scope = End == INDEX_NONE ? Document : Document.Left(End);
			return TEXT(".uai/baselines/") + Sha256Text(Scope).Right(64) + TEXT(".hjson");
		}
		// Charge immutable canonical backing, not serialized output size. Shared
		// nodes within a retained group are charged once. Across groups we may
		// conservatively charge twice; no graph is retained without an owner charge.
		int64 AddCanonicalBackingCharge(const FUeAgentCanonicalValueRef& Root, const int64 Limit,
			TSet<const FUeAgentCanonicalValue*>& Seen)
		{
			TArray<const FUeAgentCanonicalValue*> Pending{&Root.Get()};
			int64 Bytes = 0;
			while (!Pending.IsEmpty()) {
				const auto* Value = Pending.Pop(EAllowShrinking::No);
				if (Seen.Contains(Value)) continue;
				Seen.Add(Value);
				if (Value->GetTemporal() || Value->GetSpatial()) return Limit + 1;
				Bytes += sizeof(FUeAgentCanonicalValue) + 64 + Value->GetText().GetAllocatedSize()
					+ Value->GetArrayItems().GetAllocatedSize() + Value->GetObjectItems().GetAllocatedSize();
				for (const auto& Item : Value->GetArrayItems()) Pending.Add(&Item.Get());
				for (const auto& Item : Value->GetObjectItems()) {
					Bytes += Item.Key.GetAllocatedSize(); Pending.Add(&Item.Value.Get());
				}
				if (Bytes > Limit) return Limit + 1;
			}
			return Bytes;
		}
		int64 CanonicalBackingCharge(const FUeAgentCanonicalValueRef& Root, const int64 Limit) {
			TSet<const FUeAgentCanonicalValue*> Seen;
			return AddCanonicalBackingCharge(Root, Limit, Seen);
		}

		// Successful export row containers share independent immutable buckets.
		// There is no parent-map chain: writing/removing a row detaches only its
		// bucket, and dropping the last candidate releases its backing normally.
		template <typename ValueType> bool ExportRowValueEqual(const ValueType& A, const ValueType& B) { return A == B; }
		bool ExportRowValueEqual(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		template <typename ValueType> class TExportRows
		{
			using FBucket = TMap<FString, ValueType>;
			TSharedPtr<FBucket> Buckets[256];
			int32 Count = 0, ReservePerBucket = 0;
			FBucket& Write(uint32 Index) {
				auto& Bucket = Buckets[Index];
				if (!Bucket) { Bucket = MakeShared<FBucket>(); Bucket->Reserve(ReservePerBucket); }
				else if (!Bucket.IsUnique()) Bucket = MakeShared<FBucket>(*Bucket);
				return *Bucket;
			}
			template <typename InputType> void Put(const FString& Key, InputType&& Value) {
				const uint32 Hash = GetTypeHash(Key), Index = Hash >> 24;
				const auto& Bucket = Buckets[Index];
				if (const auto* Previous = Bucket ? Bucket->FindByHash(Hash, Key) : nullptr) {
					if (ExportRowValueEqual(*Previous, Value)) return;
				} else ++Count;
				Write(Index).AddByHash(Hash, Key, Forward<InputType>(Value));
			}
		public:
			int32 Num() const { return Count; }
			bool IsEmpty() const { return Count == 0; }
			void Reserve(int32 Expected) { ReservePerBucket = FMath::DivideAndRoundUp(Expected, 256); }
			void Reset() { for (auto& Bucket : Buckets) Bucket.Reset(); Count = 0; }
			int64 StringStorageBytes() const {
				int64 Bytes = sizeof(*this);
				for (const auto& Bucket : Buckets) if (Bucket) {
					Bytes += sizeof(FBucket) + 32 + Bucket->GetAllocatedSize();
					for (const auto& Pair : *Bucket) Bytes += Pair.Key.GetAllocatedSize() + Pair.Value.GetAllocatedSize();
				}
				return Bytes;
			}
			const ValueType* Find(const FString& Key) const {
				const uint32 Hash = GetTypeHash(Key);
				const auto& Bucket = Buckets[Hash >> 24]; return Bucket ? Bucket->FindByHash(Hash, Key) : nullptr;
			}
			const ValueType& FindChecked(const FString& Key) const { const auto* Value = Find(Key); check(Value); return *Value; }
			ValueType FindRef(const FString& Key) const { const auto* Value = Find(Key); return Value ? *Value : ValueType(); }
			bool Contains(const FString& Key) const { return Find(Key) != nullptr; }
			void Add(const FString& Key, const ValueType& Value) { Put(Key, Value); }
			void Add(const FString& Key, ValueType&& Value) { Put(Key, MoveTemp(Value)); }
			void Remove(const FString& Key) {
				const uint32 Hash = GetTypeHash(Key), Index = Hash >> 24;
				if (const auto& Bucket = Buckets[Index]; Bucket && Bucket->FindByHash(Hash, Key)) {
					Write(Index).RemoveByHash(Hash, Key); --Count;
					}
				}
			void RemoveIf(TFunctionRef<bool(const FString&)> Predicate) {
				for (uint32 Index = 0; Index < 256; ++Index) if (Buckets[Index]) {
					TArray<FString> Removed;
					for (const auto& Pair : *Buckets[Index]) if (Predicate(Pair.Key)) Removed.Add(Pair.Key);
					for (const FString& Key : Removed) Remove(Key);
				}
			}
			void CompactRetiredBuckets() {
				for (auto& Bucket : Buckets) if (Bucket) {
					if (Bucket->IsEmpty()) Bucket.Reset();
					else if (Bucket.IsUnique()) { Bucket->Compact(); Bucket->Shrink(); }
				}
			}
		};

		// Published declarations and formatter bytes are independent of native
		// observations. Value decoding is owned by the synchronous Source scope.
		class FGeneratedPublicationRows final : public FResolvedWorkspaceUnitBindings
		{
		public:
			FString Root;
			TSharedRef<const TArray<FSemanticUnit::FDeclarationView>> Declarations;
			TSharedRef<const FCompiledAuthoringStructure> UnitIndex;
			FWorkspacePlacementView PhysicalPaths;
			bool bScopedBuckets;
			TSharedRef<const TArray<TPair<FIntPoint, FIntPoint>>> Spans;
			TExportRows<FString> RowHashes;
			TExportRows<FUeAgentCanonicalValueRef> Inventory;
			TArray<FExportFilePlan> Files;
			TMap<FString, FString> BaselineFileHashes;
			FGeneratedPublicationRows(const FAuthoringModel& Model, const FWorkspaceLayoutPlan& Layout,
				bool bInScopedBuckets, TSharedRef<const TArray<TPair<FIntPoint, FIntPoint>>> InSpans)
				: Declarations(Model.GetSharedDeclarations()), UnitIndex(Model.GetSharedUnitIndex()),
				  PhysicalPaths(Layout.GetPhysicalPlacements()), bScopedBuckets(bInScopedBuckets), Spans(MoveTemp(InSpans)) {}
			const FSourceProvenance* FindSource(const FString&) const override { return nullptr; }
			FString ReadBaselineHash(const FString& Id) const override {
				const auto* Hash = RowHashes.Find(Id);
				if (Hash && !Hash->IsEmpty()) return *Hash;
				// A noncanonical/legacy inventory digest is not a reusable SHA.
				// Restore only its corresponding accepted envelope and let the
				// original row owner produce the exact canonical representation.
				const auto* Unit = FindBaseline(Id);
				if (!Unit) return FString();
				FString RestoredHash; FWorkspaceError Error;
				if (!TryHashSemanticUnitRow(*Unit, RestoredHash, Error)) {
					RecordScopedCapturedSemanticReadFailure(*this, Error); return FString();
				}
				return RestoredHash;
			}
			bool VisitBaselineMemberNames(const FString& Id, const FString& RootField, FKeyedMemberNameVisitor Visitor,
				FUeAgentResourceTracker& Resources, FWorkspaceError& Error) const override {
				const int32 Position = FindCompiledUnitIndex(UnitIndex.Get(), Id);
				if (!Declarations->IsValidIndex(Position)) return true;
				if (const auto* Row = Inventory.Find(Id)) {
					const auto* Envelope = (*Row)->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("envelope"); });
					const auto* Values = Envelope ? Envelope->Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("captured_values"); }) : nullptr;
					const auto* Members = Values ? Values->Value->GetObjectItems().FindByPredicate([&](const auto& Field) { return Field.Key == RootField; }) : nullptr;
					if (Members) {
						VisitCapturedKeyedMemberNames(*Members->Value, Visitor);
						UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=keyed_baseline_names root=%s members=%d typed_value_rows=0 backing=canonical"),
							*RootField, Members->Value->GetKind() == EUeAgentValueKind::Object ? Members->Value->GetObjectItems().Num() : 0);
						return true;
					}
				}
				const auto* Physical = PhysicalPaths.Find(Id);
				if (!Physical) return true;
				const FString Path = MachineEnvelopePath(*Physical, bScopedBuckets);
				return ReadScopedCapturedMemberNames(*this, Id, RootField, Resources, Visitor,
					[&](FHjsonNodePtr& Out, FWorkspaceError& Failure) {
						const auto* File = Files.FindByPredicate([&](const FExportFilePlan& Item) { return Item.GetRelativePath() == Path; });
						if (File) {
							if (!Spans->IsValidIndex(Position)) { Failure.SetSourceFailure(TEXT("workspace_publication_row_span_missing"), TEXT("The sealed captured names have no formatter row span."), Id); return false; }
							const auto& Span = (*Spans)[Position].Value;
							const FSourceProvenance Origin(Path, TEXT("/") + EscapeJsonPointerSegment(Id), 0, 0, Span.X, Span.Y, Id);
							if (Span.X < 0 || Span.Y <= Span.X) { Failure.SetSourceFailure(TEXT("workspace_publication_row_span_missing"), TEXT("The sealed captured names have an invalid formatter row span."), Id); return false; }
							return TryReadCapturedMemberNameNodes(File->GetCanonicalUtf8(), Origin, RootField, Resources, Out, Failure);
						}
						const auto* Expected = BaselineFileHashes.Find(Path);
						if (!Expected) { Failure.SetSourceFailure(TEXT("workspace_captured_file_unavailable"),
							TEXT("The required captured names have no published file binding."), Path); return false; }
						return TryReadPublishedCapturedMemberNames(Root, Path, *Expected, Id, RootField, Resources, Out, Failure);
					}, Error);
			}
			bool TryReadBaselineMembers(const FString& Id, const FString& RootField, const TSet<FString>& Keys,
				FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error) const override {
				Out.Reset();
				const int32 Position = FindCompiledUnitIndex(UnitIndex.Get(), Id);
				if (!Declarations->IsValidIndex(Position)) return true;
				const auto& Declaration = (*Declarations)[Position];
				if (const auto* Row = Inventory.Find(Id)) {
					const auto* Envelope = (*Row)->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("envelope"); });
					if (Envelope) return TryRestoreCapturedSemanticMembers(*Envelope->Value, Declaration, RootField, Keys, Resources, Out, Error);
				}
				const auto* Physical = PhysicalPaths.Find(Id);
				if (!Physical) return true;
				const FString Path = MachineEnvelopePath(*Physical, bScopedBuckets);
				const auto* File = Files.FindByPredicate([&](const FExportFilePlan& Item) { return Item.GetRelativePath() == Path; });
				if (File) {
					if (!Spans->IsValidIndex(Position)) { Error.SetSourceFailure(TEXT("workspace_publication_row_span_missing"), TEXT("The sealed captured members have no formatter row span."), Id); return false; }
					const auto& Span = (*Spans)[Position].Value;
					const FSourceProvenance Origin(Path, TEXT("/") + EscapeJsonPointerSegment(Id), 0, 0, Span.X, Span.Y, Id);
					if (Span.X < 0 || Span.Y <= Span.X) { Error.SetSourceFailure(TEXT("workspace_publication_row_span_missing"), TEXT("The sealed captured members have an invalid formatter row span."), Id); return false; }
					return TryReadCapturedSemanticMembers(File->GetCanonicalUtf8(), Origin, Declaration, RootField, Keys, Resources, Out, Error);
				}
				const auto* Expected = BaselineFileHashes.Find(Path);
				if (!Expected) { Error.SetSourceFailure(TEXT("workspace_captured_file_unavailable"),
					TEXT("The required captured members have no published file binding."), Path); return false; }
				return TryReadPublishedCapturedSemanticMembers(Root, Path, *Expected, Declaration, RootField, Keys, Resources, Out, Error);
			}
			const FWorkspaceError* GetReadFailure() const override {
				if (const auto* Error = FindScopedCapturedSemanticReadFailure(*this)) return Error;
				if (!HasSemanticWorkspaceReadScope()) {
					static const FWorkspaceError MissingScope = [] {
						FWorkspaceError Error;
						Error.SetSourceFailure(TEXT("workspace_read_scope_unavailable"),
							TEXT("Captured values require their synchronous workspace read scope."), FString());
						return Error;
					}();
					return &MissingScope;
				}
				return nullptr;
			}
			const FSemanticUnit* FindBaseline(const FString& Id) const override {
				const int32 Position = FindCompiledUnitIndex(UnitIndex.Get(), Id);
				if (!Declarations->IsValidIndex(Position)) return nullptr;
				FWorkspaceError ReadError;
				return ReadScopedCapturedSemanticUnit(*this, Id, [&](FUeAgentResourceTracker& Resources,
					TOptional<FSemanticUnit>& Out, FWorkspaceError& Error) {
					const auto& Declaration = (*Declarations)[Position];
					if (const auto* Row = Inventory.Find(Id)) {
						const auto* Envelope = (*Row)->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("envelope"); });
						if (Envelope) return TryRestoreCapturedSemanticUnit(*Envelope->Value, Declaration, Out, Error);
					}
					const auto* Physical = PhysicalPaths.Find(Id);
					if (!Physical) return false;
					const FString Path = MachineEnvelopePath(*Physical, bScopedBuckets);
					const auto* File = Files.FindByPredicate([&](const FExportFilePlan& Item) { return Item.GetRelativePath() == Path; });
					if (File) {
						if (!Spans->IsValidIndex(Position)) { Error.SetSourceFailure(TEXT("workspace_publication_row_span_missing"), TEXT("The sealed captured row has no formatter row span."), Id); return false; }
						const auto& Span = (*Spans)[Position].Value;
						FHjsonDocumentPtr Document;
						const FSourceProvenance Origin(Path, TEXT("/") + EscapeJsonPointerSegment(Id), 0, 0, Span.X, Span.Y, Id);
						if (Span.X < 0 || Span.Y <= Span.X) { Error.SetSourceFailure(TEXT("workspace_publication_row_span_missing"), TEXT("The sealed captured row has an invalid formatter row span."), Id); return false; }
						if (!TryParseGeneratedHjsonSpan(File->GetCanonicalUtf8(), Origin, Resources, Document, Error)) return false;
						return TryRestoreCapturedSemanticUnit(*Document->GetRoot(), Declaration, Resources, Out, Error);
					}
					const auto* Expected = BaselineFileHashes.Find(Path);
					if (!Expected) {
						Error.SetSourceFailure(TEXT("workspace_captured_file_unavailable"),
							TEXT("The required captured row has no published file binding."), Path); return false;
					}
					return TryReadPublishedCapturedSemanticUnit(Root, Path, *Expected, Declaration, Resources, Out, Error);
				}, ReadError);
			}
			bool TryBaselineContainsAnyString(const FString& Id, TFunctionRef<bool(const FString&)> Matches,
				TOptional<bool>& Out, FWorkspaceError& Error) const override {
				Out.Reset();
				const int32 Position = FindCompiledUnitIndex(UnitIndex.Get(), Id);
				if (!Declarations->IsValidIndex(Position)) return true;
				bool Match = false;
				if (const auto* Row = Inventory.Find(Id)) {
					const auto* Envelope = (*Row)->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("envelope"); });
					if (Envelope) {
						if (!TryCapturedCanonicalContainsAnyString(*Envelope->Value, Matches, Match, Error)) return false;
						Out = Match; return true;
					}
				}
				const auto* Physical = PhysicalPaths.Find(Id); if (!Physical) return true;
				const FString Path = MachineEnvelopePath(*Physical, bScopedBuckets);
				auto Resources = MakeSemanticWorkspaceResourceTracker();
				if (const auto* File = Files.FindByPredicate([&](const FExportFilePlan& Item) { return Item.GetRelativePath() == Path; })) {
					if (!Spans->IsValidIndex(Position) || (*Spans)[Position].Value.X < 0 || (*Spans)[Position].Value.Y <= (*Spans)[Position].Value.X) {
						Error.SetSourceFailure(TEXT("workspace_publication_row_span_missing"), TEXT("The sealed reference-query file has no corresponding formatter row span."), Id); return false;
					}
					const auto Span = (*Spans)[Position].Value;
					const FSourceProvenance Origin(Path, TEXT("/") + EscapeJsonPointerSegment(Id), 0, 0, Span.X, Span.Y, Id);
					if (!TryCapturedSpanContainsAnyString(File->GetCanonicalUtf8(), Origin, Matches, Resources, Match, Error)) return false;
				} else {
					const auto* Expected = BaselineFileHashes.Find(Path);
					if (!Expected) { Error.SetSourceFailure(TEXT("workspace_captured_file_unavailable"), TEXT("The reference-query row has no published file binding."), Id); return false; }
					if (!TryPublishedCapturedContainsAnyString(Root, Path, *Expected, Id, Matches, Resources, Match, Error)) return false;
				}
				Out = Match; return true;
			}
			FHjsonDocumentPtr FindSourceDocument(const FString&) const override { return nullptr; }
			void CopySources(TMap<FString, FSourceProvenance>&) const override {}
			void CopyBaselines(TMap<FString, FSemanticUnit>& Out) const override {
				for (const auto& Declaration : *Declarations)
					if (const auto* Unit = FindBaseline(Declaration.GetUnitId())) Out.Add(Declaration.GetUnitId(), *Unit);
			}
		};

		struct FCompositionExportBaseline;
		class FGeneratedInventoryView final : public FAcceptedWorkspaceInventory
		{
			const FCompositionExportBaseline& Owner;
			struct FRowBinding {
				const FString* Pointer = nullptr; const FString* EditHash = nullptr;
				const TPair<FIntPoint, FIntPoint>* Spans = nullptr;
			};
			bool ReadRowBinding(int32 Index, FRowBinding& Binding) const;
			bool VisitDocumentIndices(const FString& Path,
				TFunctionRef<bool(int32, const FString&, int32)> Consumer) const;
			bool Emit(int32 Index, const FString& Envelope, int32 Ordinal,
				TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor, bool bLastInBucket = true) const;
		public:
			explicit FGeneratedInventoryView(const FCompositionExportBaseline& InOwner) : Owner(InOwner) {}
			int32 Num() const override;
			bool PrepareDocumentMetadata(const FString& Path) const override;
			TOptional<bool> GetScopedMachineBuckets() const override;
			bool Visit(TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override;
			void VisitDocuments(TFunctionRef<void(const FString&)> Visitor) const override;
			const FWorkspaceLayoutPlan* GetGeneratedLayout() const override;
			bool VisitDocument(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override;
			bool VisitUnit(const FString& Id, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override;
			bool VisitBucket(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor, bool bRequireEnvelopeSpan = true) const override;
			void VisitHashes(TFunctionRef<void(const FString&, const FString&)> Visitor) const override;
			void VisitPaths(TFunctionRef<void(const FString&, const FString&, const FString&, const TArray<FString>&)> Visitor) const override;
			void VisitExpandedDocuments(TFunctionRef<void(const FString&)> Visitor) const override;
			bool HasContentFileDirectory() const override { return true; }
			void VisitContentFiles(TFunctionRef<void(const FString&)> Visitor) const override;
			FString GetInventoryPath(const FString& Document) const override;
			TSharedPtr<const FDefinitionProjectionHints> GetProjectionHints() const override;
		};
		struct FCompositionExportBaseline final : FResolvedWorkspaceUnitBindings, FSemanticProjectionBindings
		{
			FWorkspacePublication Publication;
			FString ContractId;
			int32 ContractVersion = 0;
			FString ProfileId;
			TArray<FString> TargetUnitIds;
			int32 LayoutVersion = 0;
			TSharedPtr<const FWorkspaceLayoutPlan::FProjection> Projection;
			FWorkspacePlacementView PublicRefMapping;
			// Optional composition memo only: the next successful plan takes
			// ownership after this generation's last comparison consumer. Source
			// bindings read SourceLayout/rows, never these memo tables.
			mutable TMap<FString, FUnitContentProof> UnitContentKeys;
			TArray<TSharedPtr<const FPlanUnitProofBlock>> UnitProofBlocks;
			mutable TMap<FString, FString> ItemValueInvariantKeys;
			TMap<FString, FString> PropertyDocumentSchemaKeys;
			TSharedPtr<const FDefinitionProjectionHints> DefinitionProjectionHints;
			TArray<FExportFilePlan> Files;
			FString TargetKind;
			TMap<FString, TSharedRef<FJsonObject>> PackedDocuments;
			TMap<FString, FUeAgentCanonicalValueRef> PackedExamples;
			TMap<FString, int64> PackedGroupCharges;
			TMap<FString, int64> MachineGroupCharges;
			TMap<FString, int64> DocumentIndexCharges;
			TSharedPtr<const FAuthoringExamplePublicationProof> ExamplePublication;
			TExportRows<FUeAgentCanonicalValueRef> Inventory;
			TExportRows<FUeAgentCanonicalValueRef> ManifestInventory;
			TSharedPtr<const FMachineBucketMembership> MachineBuckets;
			bool bScopedMachineBuckets = false;
			TExportRows<FString> UnitPointers;
			TExportRows<FString> UnitEditHashes;
			TWeakPtr<const uint8> ProjectionIdentity;
			TMap<FString, TArray<FString>> UnitContentFiles;
			FWorkspaceLayoutPlanPtr SourceLayout;
			// This plan is the actual output which consumed SourceLayout. Weak
			// ownership does not retain another generation or any output bytes.
			TWeakPtr<const FExportWorkspacePlan> ProducedPlan;
			// A real generated layout already owns this map. The separate field
			// remains only for the disk-recovered compatibility representation.
			const FWorkspacePlacementView& GetPublicRefs() const {
				return SourceLayout ? SourceLayout->GetMachineManifest().PublicRefMapping : PublicRefMapping;
			}
			TSharedRef<const TArray<TPair<FIntPoint, FIntPoint>>> GeneratedMachineSpans = MakeShared<const TArray<TPair<FIntPoint, FIntPoint>>>();
			TSharedPtr<FGeneratedPublicationRows> PublishedRows;
			struct FSourceInventoryRange { FString Envelope; TSharedRef<const TArray<int32>> Indices; };
			TArray<FSourceInventoryRange> SourceInventoryRanges;
			FGeneratedInventoryView GeneratedInventory{*this};
			mutable TMap<FString, FSourceProvenance> Locations;
			const FSemanticProjectionBindings* GetGeneratedProjection() const override { return SourceLayout ? this : nullptr; }
			const FAcceptedWorkspaceInventory* GetGeneratedInventory() const override { return SourceLayout && !SourceInventoryRanges.IsEmpty() ? &GeneratedInventory : nullptr; }
			const FAuthoringModel& GetModel() const override { return SourceLayout->GetModel().Get(); }
			bool Visit(TFunctionRef<bool(const FSemanticUnit&, const FString&, const FString&, const FString&)> Visitor) const override {
				for (const auto& Unit : GetModel().GetUnits()) {
					const FString& Id = Unit.GetUnitId();
					if (!Visitor(Unit, SourceLayout->GetMachineManifest().PublicRefMapping.FindChecked(Id),
						SourceLayout->GetLogicalPlacements().FindChecked(Id), SourceLayout->GetPhysicalPlacements().FindChecked(Id))) return false;
				}
				return true;
			}
			const FSourceProvenance* FindSource(const FString& Id) const override {
				if (!SourceLayout) return nullptr;
				if (const auto* Source = Locations.Find(Id)) return Source;
				const auto* Path = SourceLayout->GetPhysicalPlacements().Find(Id);
				const auto* Pointer = UnitPointers.Find(Id);
				if (!Path || !Pointer) return nullptr;
				// The formatter supplies the exact document/pointer. Unedited values
				// have no parser line/column; authored documents retain their real AST.
				return &Locations.Add(Id, FSourceProvenance(*Path, *Pointer, 0, 0, -1, -1, FString()));
			}
			const FSemanticUnit* FindBaseline(const FString& Id) const override { return SourceLayout ? SourceLayout->GetModel()->FindUnit(Id) : nullptr; }
			bool TryBaselineContainsAnyString(const FString& Id, TFunctionRef<bool(const FString&)> Matches,
				TOptional<bool>& Out, FWorkspaceError& Error) const override {
				return PublishedRows ? PublishedRows->TryBaselineContainsAnyString(Id, Matches, Out, Error)
					: FResolvedWorkspaceUnitBindings::TryBaselineContainsAnyString(Id, Matches, Out, Error);
			}
			FString ReadBaselineHash(const FString& Id) const override {
				if (PublishedRows) return PublishedRows->ReadBaselineHash(Id);
				const int32 Index = SourceLayout ? SourceLayout->GetModel()->FindUnitIndex(Id) : INDEX_NONE;
				return Index != INDEX_NONE ? SourceLayout->GetModel()->GetRowHashAt(Index) : FString();
			}
			bool VisitBaselineMemberNames(const FString& Id, const FString& RootField, FKeyedMemberNameVisitor Visitor,
				FUeAgentResourceTracker& Resources, FWorkspaceError& Error) const override {
				return PublishedRows ? PublishedRows->VisitBaselineMemberNames(Id, RootField, Visitor, Resources, Error)
					: FResolvedWorkspaceUnitBindings::VisitBaselineMemberNames(Id, RootField, Visitor, Resources, Error);
			}
			bool TryReadBaselineMembers(const FString& Id, const FString& RootField, const TSet<FString>& Keys,
				FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error) const override {
				return PublishedRows ? PublishedRows->TryReadBaselineMembers(Id, RootField, Keys, Resources, Out, Error)
					: FResolvedWorkspaceUnitBindings::TryReadBaselineMembers(Id, RootField, Keys, Resources, Out, Error);
			}
			bool FindEnvelopeSpan(const FString& Id, FAcceptedWorkspaceByteSpan& Out) const {
				if (!SourceLayout) return false;
				const auto& Model = SourceLayout->GetModel();
				const int32 Index = Model->FindUnitIndex(Id);
				if (Index == INDEX_NONE) return false;
				if (!GeneratedMachineSpans->IsValidIndex(Index)) return false;
				const auto Span = (*GeneratedMachineSpans)[Index].Value;
				Out = {Span.X, Span.Y}; return Out.IsValid();
			}
			FHjsonDocumentPtr FindSourceDocument(const FString&) const override { return nullptr; }
			FUeAgentCanonicalValuePtr FindGeneratedDocument(const FString& Path) const override {
				const auto* Value = PackedExamples.Find(Path); return Value ? *Value : FUeAgentCanonicalValuePtr();
			}
			void CopySources(TMap<FString, FSourceProvenance>& Out) const override {
				if (SourceLayout) for (const auto& Unit : SourceLayout->GetModel()->GetDeclarations())
					if (const auto* Source = FindSource(Unit.GetUnitId())) Out.Add(Unit.GetUnitId(), *Source);
			}
			void CopyBaselines(TMap<FString, FSemanticUnit>& Out) const override {
				if (SourceLayout) for (const auto& Unit : SourceLayout->GetModel()->GetUnits()) Out.Add(Unit.GetUnitId(), Unit);
			}
		};
		int32 FGeneratedInventoryView::Num() const { return Owner.SourceLayout->GetModel()->GetUnitCount(); }
		const FWorkspaceLayoutPlan* FGeneratedInventoryView::GetGeneratedLayout() const { return Owner.SourceLayout.Get(); }
		TOptional<bool> FGeneratedInventoryView::GetScopedMachineBuckets() const { return Owner.bScopedMachineBuckets; }
		void FGeneratedInventoryView::VisitDocuments(TFunctionRef<void(const FString&)> Visitor) const {
			for (const auto& File : Owner.SourceLayout->GetPhysicalFiles()) if (!File.Value.IsEmpty()) Visitor(File.Key);
		}
		bool FGeneratedInventoryView::ReadRowBinding(int32 Index, FRowBinding& Binding) const
		{
			const auto& Unit = Owner.SourceLayout->GetModel()->GetDeclarations()[Index];
			Binding.Pointer = Owner.UnitPointers.Find(Unit.GetUnitId());
			Binding.EditHash = Owner.UnitEditHashes.Find(Unit.GetUnitId());
			if (!Binding.Pointer || !Binding.EditHash || !Owner.GeneratedMachineSpans->IsValidIndex(Index)) return false;
			Binding.Spans = &(*Owner.GeneratedMachineSpans)[Index];
			return !Binding.Pointer->IsEmpty() && !Binding.EditHash->IsEmpty()
				&& Binding.Spans->Key.X >= 0 && Binding.Spans->Key.Y > Binding.Spans->Key.X
				&& Binding.Spans->Value.X >= 0 && Binding.Spans->Value.Y > Binding.Spans->Value.X;
		}
		bool FGeneratedInventoryView::PrepareDocumentMetadata(const FString& Path) const
		{
			return VisitDocumentIndices(Path, [&](int32 Index, const FString&, int32) {
				FRowBinding Binding; return ReadRowBinding(Index, Binding);
			});
		}
		bool FGeneratedInventoryView::Emit(int32 Index, const FString& Envelope, int32 Ordinal,
		TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor, bool bLastInBucket) const
		{
			const auto& Layout = *Owner.SourceLayout;
			const auto& Unit = Layout.GetModel()->GetDeclarations()[Index];
			FRowBinding Binding; if (!ReadRowBinding(Index, Binding)) return false;
			const auto* Pointer = Binding.Pointer; const auto* EditHash = Binding.EditHash; const auto& Spans = *Binding.Spans;
			const auto& Hash = Layout.GetMachineManifest().UnitInventory[Index].ContentHash;
			const auto* Content = Owner.UnitContentFiles.Find(Unit.GetUnitId());
			const TArray<FString> Empty;
			return Visitor({Unit.GetUnitId(), Unit.GetUnitKind(), Layout.GetMachineManifest().PublicRefMapping.FindChecked(Unit.GetUnitId()),
				Layout.GetLogicalPlacements().FindChecked(Unit.GetUnitId()), Layout.GetPhysicalPlacements().FindChecked(Unit.GetUnitId()),
				*Pointer, Envelope, Content ? *Content : Empty, [&]() { return *EditHash; },
				[&](const FString& Text) { return Hash.Equals(Text, ESearchCase::CaseSensitive); }, [&]() { return Hash; },
				{Spans.Key.X, Spans.Key.Y}, {Spans.Value.X, Spans.Value.Y}, true, Unit.GetSharedUnitId(), Ordinal, Owner.Publication.bDirectDataflow,
				{}, FAcceptedWorkspaceContentHashView::FromCanonicalText(Hash), bLastInBucket});
		}
		bool FGeneratedInventoryView::Visit(TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const
		{
			for (const auto& Range : Owner.SourceInventoryRanges)
				for (int32 Ordinal = 0; Ordinal < Range.Indices->Num(); ++Ordinal)
					if (!Emit((*Range.Indices)[Ordinal], Range.Envelope, Ordinal, Visitor, Ordinal + 1 == Range.Indices->Num())) return false;
			return true;
		}
		bool FGeneratedInventoryView::VisitDocumentIndices(const FString& Path,
			TFunctionRef<bool(int32, const FString&, int32)> Consumer) const
		{
			const auto* Members = Owner.SourceLayout->GetPhysicalFiles().Find(Path);
			if (!Members) return false;
			const FString Envelope = MachineEnvelopePath(Path, Owner.bScopedMachineBuckets);
			const auto* Range = Owner.SourceInventoryRanges.FindByPredicate([&](const auto& Item) { return Item.Envelope == Envelope; });
			if (!Range) return false;
			const auto& Model = Owner.SourceLayout->GetModel();
			for (const FString& Id : *Members) {
				const int32 Index = Model->FindUnitIndex(Id); if (Index == INDEX_NONE) return false;
				const int32 Ordinal = Range->Indices->Find(Index);
				if (Ordinal == INDEX_NONE || !Consumer(Index, Envelope, Ordinal)) return false;
			}
			return true;
		}
		bool FGeneratedInventoryView::VisitDocument(const FString& Path,
			TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const
		{
			return VisitDocumentIndices(Path, [&](int32 Index, const FString& Envelope, int32 Ordinal) {
				return Emit(Index, Envelope, Ordinal, Visitor);
			});
		}
		bool FGeneratedInventoryView::VisitUnit(const FString& Id, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const
		{
			const auto& Model = Owner.SourceLayout->GetModel(); const int32 Index = Model->FindUnitIndex(Id);
			const auto* Document = Owner.SourceLayout->GetPhysicalPlacements().Find(Id);
			if (Index == INDEX_NONE || !Document) return false;
			const FString Envelope = MachineEnvelopePath(*Document, Owner.bScopedMachineBuckets);
			const auto* Range = Owner.SourceInventoryRanges.FindByPredicate([&](const auto& Item) { return Item.Envelope == Envelope; });
			if (!Range) return false;
			const int32 Ordinal = Range->Indices->Find(Index);
			return Ordinal != INDEX_NONE && Emit(Index, Envelope, Ordinal, Visitor);
		}
		bool FGeneratedInventoryView::VisitBucket(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor, bool bRequireEnvelopeSpan) const
		{
			const auto* Range = Owner.SourceInventoryRanges.FindByPredicate([&](const auto& Item) { return Item.Envelope == Path; });
			if (!Range) return false;
			for (int32 Ordinal = 0; Ordinal < Range->Indices->Num(); ++Ordinal)
				if (!Emit((*Range->Indices)[Ordinal], Path, Ordinal, Visitor)) return false;
			return true;
		}
		void FGeneratedInventoryView::VisitHashes(TFunctionRef<void(const FString&, const FString&)> Visitor) const
		{
			const auto& Inventory = Owner.SourceLayout->GetMachineManifest().UnitInventory;
			for (const auto& Range : Owner.SourceInventoryRanges)
				for (int32 Index : *Range.Indices) Visitor(Inventory[Index].UnitId, Inventory[Index].ContentHash);
		}
		void FGeneratedInventoryView::VisitPaths(TFunctionRef<void(const FString&, const FString&, const FString&, const TArray<FString>&)> Visitor) const
		{
			const auto& Layout = *Owner.SourceLayout;
			const auto& Units = Layout.GetModel()->GetDeclarations();
			const TArray<FString> Empty;
			for (const auto& Range : Owner.SourceInventoryRanges) for (int32 Index : *Range.Indices) {
				const auto& Id = Units[Index].GetUnitId();
				const auto* Content = Owner.UnitContentFiles.IsEmpty() ? nullptr : Owner.UnitContentFiles.Find(Id);
				Visitor(Id, Layout.GetPhysicalPlacements().FindChecked(Id), Range.Envelope, Content ? *Content : Empty);
			}
		}
		void FGeneratedInventoryView::VisitExpandedDocuments(TFunctionRef<void(const FString&)> Visitor) const
		{
			for (const auto& Pair : Owner.UnitContentFiles)
				if (!Pair.Value.IsEmpty()) Visitor(Owner.SourceLayout->GetPhysicalPlacements().FindChecked(Pair.Key));
		}
		void FGeneratedInventoryView::VisitContentFiles(TFunctionRef<void(const FString&)> Visitor) const
		{
			for (const auto& Pair : Owner.UnitContentFiles)
				for (const auto& Path : Pair.Value) Visitor(Path);
		}
		FString FGeneratedInventoryView::GetInventoryPath(const FString& Document) const
		{
			return MachineEnvelopePath(Document, Owner.bScopedMachineBuckets).Replace(TEXT("/baselines/"), TEXT("/inventory/"));
		}
		TSharedPtr<const FDefinitionProjectionHints> FGeneratedInventoryView::GetProjectionHints() const { return Owner.DefinitionProjectionHints; }
		thread_local TSharedPtr<const FCompositionExportBaseline> PreviousCompositionExport;
		FString CompositionPublisherOwner(const FString& TargetKind, const FPreparedWorkspaceExport* Settings)
		{
			return Settings && !Settings->SourcePublisherOwnerId.IsEmpty() ? Settings->SourcePublisherOwnerId
				: TEXT("uai.authoring.composition.") + TargetKind.Replace(TEXT("."), TEXT("_"));
		}
		bool ExportDataIdentical(const FString& A, const FString& B)
		{
			return A.Equals(B, ESearchCase::CaseSensitive);
		}
		bool IsClosedLevelProjectionValueChange(const FSemanticUnit& Unit, const FSemanticUnit& Before, FWorkspaceError& Error)
		{
			if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState
				|| (Unit.GetRole() != TEXT("actor_component_hierarchy") && Unit.GetRole() != TEXT("level_components"))
				|| Unit.GetValueCount() != Before.GetValueCount()
				|| !Unit.GetSourceProvenance().Equals(Before.GetSourceProvenance(), ESearchCase::CaseSensitive)
				|| Unit.GetCategoryPath().Num() != Before.GetCategoryPath().Num()) return false;
			for (int32 I = 0; I < Unit.GetCategoryPath().Num(); ++I)
				if (!Unit.GetCategoryPath()[I].Equals(Before.GetCategoryPath()[I], ESearchCase::CaseSensitive)) return false;
			for (const auto& Field : Unit.GetValues()) {
				const auto* Old = Before.FindValue(Field.Key);
				if (!Old || !HaveOnlyNumericOrBooleanValueChanges(Old->Get(), Field.Value.Get())) return false;
			}
			// The same content owner proves every non-value declaration field.
			// Shape/string equality above preserves members, aliases and bindings.
			FString Invariant, Prior;
			return TryHashSemanticUnitContent(Unit.WithValues(Before.GetValues()), Invariant, Error)
				&& TryHashSemanticUnitContent(Before, Prior, Error)
				&& Invariant.Equals(Prior, ESearchCase::CaseSensitive);
		}
		bool ExportDataIdentical(const FWorkspaceFileMembers& A, const FWorkspaceFileMembers& B) { return A == B; }
		template <typename ItemType>
		bool ExportDataIdentical(const TArray<ItemType>& A, const TArray<ItemType>& B)
		{
			if (A.Num() != B.Num()) return false;
			for (int32 I = 0; I < A.Num(); ++I)
				if (!ExportDataIdentical(A[I], B[I])) return false;
			return true;
		}

		bool ExportMapsIdentical(const FWorkspacePlacementView& A, const FWorkspacePlacementView& B)
		{ return A.OrderIndependentCompareEqual(B); }
		template <typename FirstMap, typename SecondMap>
		bool ExportMapsIdentical(const FirstMap& A, const SecondMap& B)
		{
			if (A.Num() != B.Num()) return false;
			TArray<FString> LeftKeys, RightKeys;
			LeftKeys.Reserve(A.Num()); RightKeys.Reserve(B.Num());
			for (const auto& Pair : A) LeftKeys.Add(Pair.Key);
			for (const auto& Pair : B) RightKeys.Add(Pair.Key);
			LeftKeys.Sort(); RightKeys.Sort();
			if (!ExportDataIdentical(LeftKeys, RightKeys)) return false;
			for (const auto& Pair : A)
			{
				const auto* Other = B.Find(Pair.Key);
				if (!Other || !ExportDataIdentical(Pair.Value, *Other)) return false;
			}
			return true;
		}

		FString GraphInterfaceDocumentPath(const FString& GraphDocument)
		{
			return FPaths::Combine(FPaths::GetPath(GraphDocument),
				FPaths::GetBaseFilename(GraphDocument) + TEXT(".interfaces.hjson"));
		}

		bool IsGraphStructureUnit(const FSemanticUnit& Unit)
		{
			return Unit.GetUnitKind() == ESemanticUnitKind::StructureState
				&& IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef(),
					Unit.HasValue(TEXT("nodes")) && Unit.HasValue(TEXT("edges")));
		}
		bool IsGraphStructureUnit(const FSemanticUnit::FDeclarationView& Unit)
		{
			return Unit.GetUnitKind() == ESemanticUnitKind::StructureState
				&& IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef(), Unit.HasGraphBody());
		}

		FString KeyedItemRootField(const FSemanticUnit& Unit)
		{
			if (Unit.GetUnitKind() == ESemanticUnitKind::StructureState)
				for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
					if (Unit.HasValue(Contract.RootField)) return Contract.RootField;
			return FString();
		}
		FString KeyedItemRootField(const FSemanticUnit::FDeclarationView& Unit)
		{
			if (Unit.GetUnitKind() == ESemanticUnitKind::StructureState)
				for (const auto& Contract : GetKeyedItemProjectionContracts())
					if (Unit.HasKeyedRoot(Contract.RootField)) return Contract.RootField;
			return FString();
		}

		bool IsKeyedItemStructureUnit(const FSemanticUnit& Unit)
		{
			// Graph metadata may contain names such as interfaces. Those fields
			// belong to the graph document, not a standalone keyed collection.
			return !IsGraphStructureUnit(Unit) && !KeyedItemRootField(Unit).IsEmpty();
		}
		bool IsKeyedItemStructureUnit(const FSemanticUnit::FDeclarationView& Unit)
		{
			return !IsGraphStructureUnit(Unit) && !KeyedItemRootField(Unit).IsEmpty();
		}

	using FDocumentCollectionContracts = TArray<const FKeyedItemProjectionContract*, TInlineAllocator<4>>;

	FDocumentCollectionContracts DocumentCollectionContracts(const FSemanticUnit::FDeclarationView& Unit, const FString& TargetKind)
	{
		FString SemanticId, DocumentPath;
		Unit.GetSemanticTypeRef().Split(TEXT("#"), &SemanticId, &DocumentPath);
		const auto* Specific = FindLevelSequenceCollectionContract(TargetKind, SemanticId, Unit.GetRole(), DocumentPath);
		FDocumentCollectionContracts Contracts;
		for (const auto& Contract : GetKeyedItemProjectionContracts())
			if (Unit.HasKeyedRoot(Contract.RootField))
				Contracts.Add(Specific && Specific->RootField == Contract.RootField ? Specific : &Contract);
		return Contracts;
	}

	bool CollectionGuidesNeedContent(const FDocumentCollectionContracts& Contracts)
	{
		return Contracts.ContainsByPredicate([](const auto* Contract) {
			return !Contract->CollectionFieldContracts.IsEmpty() || !Contract->RecordSchemaField.IsEmpty();
		});
	}

// A keyed row can contain an independently produced record schema. Its
// human-readable aliases are values in that schema, not semantic dependency
// IDs of the table row. The complete guide resolves them in its target catalog.
static bool TryCollectScopedGuideDefinitions(const FAuthoringModel& Model, const FString& TargetKind,
	const TSet<FString>& Written, TSet<FString>& GuideDefinitions, FWorkspaceError& Error)
{
	TMap<FString, TSet<FString>> AliasesByTarget;
	TSet<FString> Seen = Written;
	TArray<FString> Dependencies;
	const auto Collect = [&](const FSemanticUnit::FDeclarationView& Declaration) {
		const bool bDefinition = Declaration.GetUnitKind() == ESemanticUnitKind::CollectionState && Declaration.GetRole() == TEXT("property_definition");
		if (bDefinition) GuideDefinitions.Add(Declaration.GetUnitId());
		if (bDefinition || Declaration.GetUnitKind() == ESemanticUnitKind::PropertySet)
			for (const FString& Id : Declaration.GetDependencyRefs())
				if (!Seen.Contains(Id)) { Seen.Add(Id); Dependencies.Add(Id); }
	};
	for (const FString& Id : Written) {
		const auto Declaration = Model.FindDeclaration(Id);
		if (!Declaration) continue;
		Collect(*Declaration);
		if (!IsKeyedItemStructureUnit(*Declaration)) continue;
		TSet<FString> Aliases;
		for (const auto* Contract : DocumentCollectionContracts(*Declaration, TargetKind)) {
			if (Contract->RecordSchemaField.IsEmpty()) continue;
			const auto* Unit = Model.FindUnit(Id);
			const auto* Schema = Unit ? Unit->FindValue(Contract->RecordSchemaField) : nullptr;
			if (!Schema) {
				if (!Model.CheckValueReadResult(Error)) return false;
				Error.SetSourceFailure(TEXT("keyed_record_schema_missing"),
					TEXT("The keyed collection guide requires its current record schema."), Id, TEXT("/") + Contract->RecordSchemaField);
				return false;
			}
			FAuthoringExampleCatalog::CollectPropertyGuideReferences(*Schema, Aliases);
		}
		if (Aliases.IsEmpty()) continue;
		auto Owner = Declaration;
		TSet<FString> Ancestors;
		while (Owner && !Model.GetTargetUnitIds().Contains(Owner->GetUnitId())) {
			if (Ancestors.Contains(Owner->GetUnitId())) {
				Error.SetSourceFailure(TEXT("semantic_owner_cycle"), TEXT("The record schema's catalog owner chain is cyclic."), Id);
				return false;
			}
			Ancestors.Add(Owner->GetUnitId());
			Owner = Model.FindDeclaration(Owner->GetOwnerRef());
		}
		if (!Owner) {
			if (!Model.CheckValueReadResult(Error)) return false;
			Error.SetSourceFailure(TEXT("unresolved_semantic_unit_reference"),
				TEXT("The record schema has no current target for its definition catalog."), Id);
			return false;
		}
		AliasesByTarget.FindOrAdd(Owner->GetUnitId()).Append(Aliases);
	}
	if (!AliasesByTarget.IsEmpty() && !Model.VisitDefinitionDeclarations([&](const auto& Definition) {
		const auto* Aliases = AliasesByTarget.Find(Definition.GetOwnerRef());
		if (Aliases) {
			const auto* Alias = Model.FindReadablePropertyDefinitionName(Definition.GetUnitId());
			if (Alias && Aliases->Contains(*Alias) && !Seen.Contains(Definition.GetUnitId())) {
				Seen.Add(Definition.GetUnitId()); Collect(Definition);
			}
		}
		return true;
	})) return false;
	for (int32 Index = 0; Index < Dependencies.Num(); ++Index)
		if (const auto Declaration = Model.FindDeclaration(Dependencies[Index])) Collect(*Declaration);
	return Model.CheckValueReadResult(Error);
}

	bool BuildLeafCollectionGuideDocument(const FDocumentCollectionContracts& Contracts, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& Metadata, FWorkspaceError& Error, const FString& Path)
	{
		FUeAgentCanonicalValuePtr Empty;
		FUeAgentStateSemanticError Failure;
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
		if (!FUeAgentCanonicalValue::TryObject({}, Resources, Empty, Failure)) { Error.SetFailure(MoveTemp(Failure), Path); return false; }
		for (const auto* Contract : Contracts) Fields.Emplace(Contract->RootField, Empty.ToSharedRef());
		if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Metadata, Failure)) { Error.SetFailure(MoveTemp(Failure), Path); return false; }
		return true;
	}

		template <typename TUnit> bool IsActorComponentHierarchyItemUnit(const TUnit& Unit)
		{
			return Unit.GetUnitKind() == ESemanticUnitKind::ItemState
				&& Unit.GetRole() == TEXT("actor_component_hierarchy")
				&& (Unit.GetSemanticTypeRef().EndsWith(TEXT(".actor.target"))
					|| Unit.GetSemanticTypeRef().EndsWith(TEXT(".component.target")));
		}

	}

	FString GetPublishedMachineEnvelopePath(const FString& Document, bool bScopedOwners)
	{
		return MachineEnvelopePath(Document, bScopedOwners);
	}

	static TSharedPtr<const FDefinitionProjectionHints> GetCompositionDefinitionProjectionHints(
		const FString& TargetKind, const FString& ProfileId)
	{
		const auto Accepted = GetAcceptedDefinitionProjectionHints(TargetKind);
		if (Accepted && Accepted->ProfileId.Equals(ProfileId, ESearchCase::CaseSensitive)) return Accepted;
		// A new workspace has no accepted names. Another workspace's optional
		// retained plan must not decide its public identity allocation policy.
		return nullptr;
	}

	static bool TryBuildCommittedPublicationHeaderImpl(const FCompositionSnapshotBinding& Binding,
		const FString& TargetKind, const FPreparedWorkspaceExport& Settings, FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan, FWorkspaceError& Error,
		TFunctionRef<void(const TSharedRef<FJsonObject>&)>* UpdateManifest,
		TMap<FString, FString>* AcceptedFileHashes = nullptr)
	{
		// Net-zero actual output or a zero-record state migration has no new
		// public body to generate. The latter retains current pending author
		// bytes and the existing actual baseline; it never consumes that intent.
		const FString Root = Settings.AcceptedWorkspaceRoot.IsEmpty()
			? GetAcceptedSemanticWorkspaceRoot() : Settings.AcceptedWorkspaceRoot;
		FHjsonDocumentPtr Document;
		if (!TryReadAcceptedSemanticSourceDocument(Root, ManifestPath, Resources, Document, Error)) return false;
		FUeAgentCanonicalValuePtr Canonical;
		TSharedPtr<FJsonValue> Json;
		Contract::FDiagnostic Diagnostic;
		if (!TryProjectHjsonCanonicalValue(Document->GetRoot().Get(), Resources, Canonical, Error, false)) return false;
		if (!CanonicalJson::TryToJsonValue(*Canonical, Json, Diagnostic) || !Json || !Json->AsObject()) {
			Error.SetFailure(MoveTemp(Diagnostic), ManifestPath); return false;
		}
		const auto Manifest = Json->AsObject().ToSharedRef();
		Manifest->SetStringField(TEXT("workspace_schema"), GetDirectWorkspaceSchemaId());
		Manifest->SetStringField(TEXT("publication_generation"), Settings.Publication.Generation);
		Manifest->SetStringField(TEXT("publication_state"), TEXT("committed"));
		Manifest->RemoveField(TEXT("semantic_fingerprint"));
		if (const auto Inventory = GetAcceptedWorkspaceInventory(Root)) {
			Manifest->SetField(TEXT("published_unit_count"), MakeShared<FJsonValueNumberString>(FString::FromInt(Inventory->Num())));
			if (const auto Scoped = Inventory->GetScopedMachineBuckets(); Scoped.IsSet())
				Manifest->SetStringField(TEXT("machine_bucket_organization"), Scoped.GetValue() ? TEXT("owner_scope_v1") : TEXT("document_prefix_v1"));
			if (Inventory->HasContentFileDirectory()) {
				TSet<FString> UniqueExpanded;
				Inventory->VisitExpandedDocuments([&](const FString& Path) { UniqueExpanded.Add(Path); });
				TArray<FString> Expanded = UniqueExpanded.Array(); Expanded.Sort();
				Manifest->SetArrayField(TEXT("expanded_author_documents"), StringArray(Expanded));
			}
		}
		if (Settings.ActualChanges) {
			Manifest->SetStringField(TEXT("base_revision"), Settings.ActualChanges->AfterRevision);
			Manifest->SetArrayField(TEXT("target_unit_ids"), StringArray(Settings.ActualChanges->AfterTargets));
			Manifest->SetArrayField(TEXT("structural_edits"), TArray<TSharedPtr<FJsonValue>>{});
		}
		const auto Revision = MakeShared<FJsonObject>();
		Revision->SetStringField(TEXT("source_id"), Binding.Revision.GetSourceId());
		Revision->SetStringField(TEXT("generation"), Binding.Revision.GetGeneration());
		Revision->SetField(TEXT("sequence"), MakeShared<FJsonValueNumberString>(::LexToString(Binding.Revision.GetSequence())));
		Manifest->SetObjectField(TEXT("base_state_revision"), Revision);
		if (UpdateManifest) (*UpdateManifest)(Manifest);
		TArray<uint8> Bytes;
		if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Manifest), Resources, Canonical, Error)
			|| !TrySerializeSemanticHjsonObject(*Canonical, Bytes, Error, ManifestPath)) return false;
		TMap<FString, FString> Retained;
		ArtifactPublisher::FArtifactPublishError PublishError;
		if (AcceptedFileHashes) Retained = MoveTemp(*AcceptedFileHashes);
		else if (!TryReadPublishedWorkspaceFileHashes(Root,
			CompositionPublisherOwner(TargetKind, &Settings), Retained, PublishError, true)) {
			Error.SetFailure(MoveTemp(PublishError)); return false;
		}
		Retained.Remove(ManifestPath);
		// Current physical encoding belongs to the actual accepted read set,
		// not every path in the destination's published-hash directory. Leave
		// unrelated files untouched; pending author consumption remains separate.
		if (!TryUpdateAcceptedRetainedFileHashes(Root, Retained, Error)) return false;
		const FString Hash = Sha256(Bytes);
		RememberFormattedHjsonRootHeader(Hash, *Canonical);
		TArray<FExportFilePlan> Files;
		Files.Emplace(ManifestPath, MoveTemp(Bytes), Hash);
		ArtifactPublisher::RememberVerifiedPlanBytes(Files.Last());
		TArray<FString> Paths; Retained.GetKeys(Paths); Paths.Sort();
		FString Material = Settings.SourcePublication.Generation + TEXT("|") + Binding.EvidenceId + TEXT("|") + Hash;
		for (const auto& Path : Paths) Material += TEXT("|") + Path + TEXT("|") + Retained.FindChecked(Path);
		OutPlan = MakeShared<FExportWorkspacePlan>(EWorkspaceKind::Directory, ManifestPath,
			Binding.SnapshotId, Binding.Revision, Binding.EvidenceId, MoveTemp(Files), Sha256Text(Material), 0, MoveTemp(Retained));
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=committed_publication_header retained_files=%d produced_files=1 semantic_rows_read=0 layout_materialized=false model_materialized=false"), Paths.Num());
		return true;
	}

	bool TryBuildCommittedPublicationHeader(const FCompositionSnapshotBinding& Binding,
		const FString& TargetKind, const FPreparedWorkspaceExport& Settings, FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan, FWorkspaceError& Error)
	{
		return TryBuildCommittedPublicationHeaderImpl(Binding, TargetKind, Settings, Resources, OutPlan, Error, nullptr);
	}

	static bool TryReplacePublishedObject(const FUeAgentCanonicalValueRef& Root, const FString& Pointer,
		const FUeAgentCanonicalValueRef& Value, FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out,
		FWorkspaceError& Error, const FString& Document, bool bCreateParents = false)
	{
		TArray<FString> Segments; Pointer.ParseIntoArray(Segments, TEXT("/"), true);
		for (auto& Segment : Segments) { Segment.ReplaceInline(TEXT("~1"), TEXT("/")); Segment.ReplaceInline(TEXT("~0"), TEXT("~")); }
		TFunction<bool(const FUeAgentCanonicalValueRef&, int32, FUeAgentCanonicalValuePtr&)> Replace;
		Replace = [&](const FUeAgentCanonicalValueRef& Node, int32 Depth, FUeAgentCanonicalValuePtr& Result) {
			if (Depth == Segments.Num()) { Result = Value; return true; }
			if (Node->GetKind() != EUeAgentValueKind::Object) return false;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields; bool bFound = false;
			for (const auto& Field : Node->GetObjectItems()) {
				if (Field.Key != Segments[Depth]) { Fields.Add(Field); continue; }
				FUeAgentCanonicalValuePtr Child;
				if (!Replace(Field.Value, Depth + 1, Child)) return false;
				Fields.Emplace(Field.Key, Child.ToSharedRef()); bFound = true;
			}
			if (!bFound) {
				if (Depth + 1 == Segments.Num()) Fields.Emplace(Segments[Depth], Value);
				else {
					if (!bCreateParents) return false;
					FUeAgentStateSemanticError Failure; FUeAgentCanonicalValuePtr Empty, Child;
					if (!FUeAgentCanonicalValue::TryObject({}, Resources, Empty, Failure)) { Error.SetFailure(MoveTemp(Failure), Document); return false; }
					if (!Replace(Empty.ToSharedRef(), Depth + 1, Child)) return false;
					Fields.Emplace(Segments[Depth], Child.ToSharedRef());
				}
			}
			FUeAgentStateSemanticError Failure;
			if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Result, Failure)) { Error.SetFailure(MoveTemp(Failure), Document); return false; }
			return true;
		};
		return Replace(Root, 0, Out);
	}

	static bool TryBuildPublishedPropertyRow(const FSemanticUnit& Unit, const FAuthoringModel& Model,
		const FReadablePropertyAddress& Address, const TSharedPtr<FJsonObject>& Compact, TSharedRef<FJsonObject> Visible, FWorkspaceError& Error)
	{
		Visible->SetStringField(TEXT("type"), Address.Type);
		if (!Address.Definition.IsEmpty()) Visible->SetStringField(TEXT("definition"), Address.Definition);
		Contract::FDiagnostic Failure;
		auto Value = DirectPropertyValue(Compact, Unit, Model, Failure);
		if (!Value) { Error.SetFailure(Failure.IsEmpty() ? Contract::FDiagnostic::Failure(TEXT("composition_property_value_invalid"),
			TEXT("The property could not be projected.")) : Failure, Unit.GetUnitId()); return false; }
		Visible->SetField(TEXT("value"), MoveTemp(Value)); return true;
	}

	bool TryBuildPublishedInventoryMetadata(const FAcceptedWorkspaceInventoryRow& Row,
		FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out, FWorkspaceError& Error)
	{
		const FString Hash = Row.CanonicalContentHash();
		if (Hash.IsEmpty()) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.inventory_metadata"), Row.UnitId,
			Row.EnvelopePath, TEXT("original canonical row digest unavailable"), TEXT("accepted metadata row"), TEXT("original inventory parser/row writer"));
		const auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("unit_id"), Row.UnitId); Json->SetStringField(TEXT("unit_kind"), LexToString(Row.UnitKind));
		Json->SetStringField(TEXT("public_ref"), Row.PublicRef); Json->SetStringField(TEXT("logical_path"), Row.LogicalPath);
		Json->SetStringField(TEXT("document_path"), Row.DocumentPath); Json->SetStringField(TEXT("unit_pointer"), Row.UnitPointer);
		Json->SetStringField(TEXT("envelope_ref"), Row.EnvelopePath); Json->SetStringField(TEXT("content_hash"), Hash);
		Json->SetStringField(TEXT("edit_hash"), Row.ReadEditHash());
		if (!Row.ContentFiles.IsEmpty()) Json->SetArrayField(TEXT("content_files"), StringArray(Row.ContentFiles));
		return TryCanonicalizeJson(MakeShared<FJsonValueObject>(Json), Resources, Out, Error);
	}

	bool TrySerializePublishedInventoryWithEnvelopeSpans(const TArray<FUeAgentCanonicalValueRef>& Rows,
		const TArray<FIntPoint>& EnvelopeSpans, FUeAgentResourceTracker& Resources, TArray<uint8>& OutBytes,
		TArray<FIntPoint>& OutSpans, FWorkspaceError& Error, const FString& Path)
	{
		if (Rows.Num() != EnvelopeSpans.Num()) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.inventory_ranges"),
			TEXT("one actual baseline range per metadata row"), Path, TEXT("row/range count differs"),
			TEXT("same-write baseline formatter output"), TEXT("original machine writer"));
		TArray<TArray<uint8>> Encoded; Encoded.Reserve(Rows.Num());
		TArray<FUeAgentStructuredHjsonObjectFragment> Fragments; Fragments.Reserve(Rows.Num());
		FUeAgentStateSemanticError Failure;
		for (int32 Index = 0; Index < Rows.Num(); ++Index) {
			const auto Span = EnvelopeSpans[Index];
			if (Span.X < 0 || Span.Y <= Span.X) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.inventory_ranges"),
				FString::FromInt(Index), Path, TEXT("actual baseline range missing"), TEXT("valid original byte begin/end"), TEXT("original machine writer"));
			FUeAgentCanonicalValuePtr Begin, End, Range, Changed;
			if (!FUeAgentCanonicalValue::TryInteger(FString::FromInt(Span.X), Resources, Begin, Failure)
				|| !FUeAgentCanonicalValue::TryInteger(FString::FromInt(Span.Y), Resources, End, Failure)
				|| !FUeAgentCanonicalValue::TryArray({Begin.ToSharedRef(), End.ToSharedRef()}, Resources, Range, Failure)) {
				Error.SetFailure(MoveTemp(Failure), Path); return false;
				}
			auto Fields = Rows[Index]->GetObjectItems();
			Fields.RemoveAll([](const auto& Field) { return Field.Key == TEXT("envelope_span"); });
			Fields.Emplace(TEXT("envelope_span"), Range.ToSharedRef());
			if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Changed, Failure)) { Error.SetFailure(MoveTemp(Failure), Path); return false; }
			auto& Bytes = Encoded.AddDefaulted_GetRef();
			if (!TrySerializeSemanticHjsonObject(*Changed, Bytes, Error, Path)) return false;
			Fragments.Add({FString(), MakeArrayView(Bytes.GetData(), Bytes.Num() - 1), 0});
			}
		Contract::FDiagnostic WriteFailure;
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObjectFragments(Fragments, TEXT("unit_inventory"), OutBytes, WriteFailure, &OutSpans)) {
			Error.SetFailure(MoveTemp(WriteFailure), Path); return false;
			}
		return true;
	}

	static bool TryBuildPublishedMachineRow(const FSemanticUnit& Unit, const FAuthoringModel& Model,
		const FString& PublicRef, const FString& LogicalPath, const FString& DocumentPath, const FString& UnitPointer,
		const TArray<FString>& ContentFiles, const FString& ContentHash, const FString& EditHash,
		FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& OutComplete, FWorkspaceError& Error)
	{
		const auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("unit_id"), Unit.GetUnitId()); Json->SetStringField(TEXT("public_ref"), PublicRef);
		Json->SetStringField(TEXT("unit_kind"), LexToString(Unit.GetUnitKind())); Json->SetStringField(TEXT("logical_path"), LogicalPath);
		Json->SetStringField(TEXT("document_path"), DocumentPath); Json->SetStringField(TEXT("unit_pointer"), UnitPointer);
		Json->SetStringField(TEXT("content_hash"), ContentHash); Json->SetStringField(TEXT("edit_hash"), EditHash);
		if (!ContentFiles.IsEmpty()) Json->SetArrayField(TEXT("content_files"), StringArray(ContentFiles));
		AddSemanticEnvelope(Unit, Json);
		TSharedPtr<FJsonObject> Compact; const TSharedPtr<FJsonObject>* Values = nullptr;
		if (!TryBuildCompactRow(Unit, Compact, Error, nullptr, DocumentPath) || !Compact
			|| !Compact->TryGetObjectField(TEXT("values"), Values) || !Values || !Values->IsValid()) {
			if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("composition_unit_envelope_failed"),
				TEXT("The semantic row has no captured values."), Unit.GetUnitId()); return false;
		}
		const auto Envelope = Json->GetObjectField(TEXT("envelope"));
		Envelope->SetObjectField(TEXT("captured_values"), Values->ToSharedRef());
		if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet) {
			Envelope->SetStringField(TEXT("captured_type"), ReadablePropertyType(Unit, Model));
			Envelope->SetStringField(TEXT("captured_definition"), ReadablePropertyDefinition(Unit, Model));
		}
		return TryCanonicalizeJson(MakeShared<FJsonValueObject>(Json), Resources, OutComplete, Error);
	}

	static TConstArrayView<uint8> PublishedMachineSpanView(const TArray<uint8>& Bytes, const FAcceptedWorkspaceByteSpan& Span, int32 Depth)
	{
		if (!Span.IsValid() || Span.End > Bytes.Num() || Span.Begin < Depth * 2
			|| Bytes[Span.Begin] != '{' || Bytes[Span.End - 1] != '}') return {};
		int64 ClosingLine = Span.End - 1;
		while (ClosingLine > Span.Begin && Bytes[ClosingLine - 1] != '\n') --ClosingLine;
		if (Span.End - 1 - ClosingLine != Depth * 2) return {};
		for (int64 Index = ClosingLine; Index < Span.End - 1; ++Index) if (Bytes[Index] != ' ') return {};
		return MakeArrayView(Bytes.GetData() + Span.Begin, static_cast<int32>(Span.End - Span.Begin));
	}

	template <typename TUnit>
	static TOptional<FSemanticUnit::FDeclarationView> FindSemanticFieldSetDeclaration(const TUnit& Unit, const FAuthoringModel& Model)
	{
		if (!HasSemanticFieldSetReference(Unit.GetUnitKind(), Unit.GetSemanticTypeRef(), Unit.GetFieldSetRef())) return {};
		return Model.FindDeclaration(Unit.GetFieldSetRef());
	}

static bool TryGetAggregateMemberFieldSet(const FAuthoringModel& Model, const FUeAgentCanonicalValueRef& Row,
	FString& FieldSet, FWorkspaceError& Error)
{
	FieldSet.Reset();
	const auto* Binding = FindObjectField(Row, TEXT("unit_id"));
	if (!Binding || (*Binding)->GetKind() != EUeAgentValueKind::String) return true;
	const FString& MemberId = (*Binding)->GetText();
	const auto Member = Model.FindDeclaration(MemberId);
	if (!Member) {
		if (!Model.CheckValueReadResult(Error)) return false;
		Error.SetSourceFailure(TEXT("unresolved_semantic_unit_reference"),
			TEXT("The captured collection row's bound member declaration is unavailable."), MemberId);
		return false;
	}
	if (HasSemanticFieldSetReference(Member->GetUnitKind(), Member->GetSemanticTypeRef(), Member->GetFieldSetRef()))
		FieldSet = Member->GetFieldSetRef();
	return true;
}

	static bool TrySelectPropertyPublicationInput(FAuthoringModelRef Model, const FPreparedWorkspaceExport& Settings,
		TMap<FString, const FSemanticUnit*>& Units, bool& bPropertyInput, FWorkspaceError& Error,
		const TSet<FString>* RewriteRows = nullptr)
	{
		bPropertyInput = false; Units.Reset();
		if (!Settings.ActualChanges || !Settings.SourcePublication.bDirectDataflow || !Settings.Publication.bDirectDataflow)
			return FailWorkspaceDataflowHandoff(Error, TEXT("composition.publication_input"), TEXT("actual changes and publication"),
				Settings.AcceptedWorkspaceRoot, Settings.ActualExportMode, TEXT("same-request actual and destination"), TEXT("committed export selection"));
		for (const auto& Change : Settings.ActualChanges->Changes) {
			// Real structural/type/content input chooses its own writer before
			// either writer produces values. This is not a failed property retry.
			if (!Change.Before || !Change.After || Change.After->GetUnitKind() != ESemanticUnitKind::PropertySet
				|| !Change.After->MatchesCompiledDeclaration(Change.Before->GetDeclaration().GetDeclarationProof())
				|| ReadablePropertyType(Change.Before.GetValue(), Model.Get()) != ReadablePropertyType(Change.After.GetValue(), Model.Get())
				|| ReadablePropertyDefinition(Change.Before.GetValue(), Model.Get()) != ReadablePropertyDefinition(Change.After.GetValue(), Model.Get())) return true;
			Units.Add(Change.UnitId, &Change.After.GetValue());
		}
		const FString Root = Settings.AcceptedWorkspaceRoot.IsEmpty() ? GetAcceptedSemanticWorkspaceRoot() : Settings.AcceptedWorkspaceRoot;
		const auto Source = GetAcceptedSemanticWorkspace(Root);
		if (Source && Source->GetEditedUnitIds().IsSet()) for (const auto& Id : *Source->GetEditedUnitIds()) {
			if (Units.Contains(Id) || Settings.PendingAuthorUnits.Contains(Id) || Settings.PendingKeyedAuthorRows.Contains(Id)) continue;
			const auto* Actual = Model->FindUnit(Id);
			if (!Actual) { Error.SetSourceFailure(TEXT("workspace_publication_normalization_actual_missing"),
				TEXT("Accepted author normalization has no actual unit in the sealed output scope. No alternate writer was selected."), Id); return false; }
			if (Actual->GetUnitKind() != ESemanticUnitKind::PropertySet) return true;
			Units.Add(Id, Actual);
		}
		if (RewriteRows) for (const auto& Id : *RewriteRows) {
			const auto* Actual = Model->FindUnit(Id);
			if (!Actual) { Error.SetSourceFailure(TEXT("workspace_publication_normalization_actual_missing"),
				TEXT("The full document normalization has no current unit. No alternate writer was selected."), Id); return false; }
			if (Actual->GetUnitKind() != ESemanticUnitKind::PropertySet) return true;
			Units.Add(Id, Actual);
		}
		if (Units.IsEmpty()) return true;
		const auto Inventory = GetAcceptedWorkspaceInventory(Root);
		if (!Inventory) { Error.SetSourceFailure(TEXT("workspace_publication_directory_unavailable"), TEXT("The accepted publication has no row directory."), Root); return false; }
		for (const auto& Pair : Units) {
			if (HasAuthoringPropertyContent(Pair.Value->GetSemanticTypeRef(), Pair.Value->GetRole())) return true;
			bool bContent = false;
			if (!Inventory->VisitUnit(Pair.Key, [&](const FAcceptedWorkspaceInventoryRow& Location) { bContent |= !Location.ContentFiles.IsEmpty(); return true; })) {
				Error.SetSourceFailure(TEXT("workspace_publication_property_location_missing"), TEXT("The actual property has no accepted physical location. No alternate writer was selected."), Pair.Key); return false;
			}
			if (bContent) return true;
		}
		bPropertyInput = true; return true;
	}

	static bool TryBuildChangedPropertyPublication(FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& Binding, const FString& TargetKind, const FPreparedWorkspaceExport& Settings,
		const TMap<FString, const FSemanticUnit*>& Units, FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan, FWorkspaceError& Error, TMap<FString, TPair<FString, FString>>* PendingLocations,
		const TSet<FString>* FullRebuildDocuments = nullptr)
	{
		const FString Root = Settings.AcceptedWorkspaceRoot.IsEmpty() ? GetAcceptedSemanticWorkspaceRoot() : Settings.AcceptedWorkspaceRoot;
		const auto Inventory = GetAcceptedWorkspaceInventory(Root);
		if (!Inventory) { Error.SetSourceFailure(TEXT("workspace_publication_directory_unavailable"), TEXT("The accepted publication has no row directory."), Root); return false; }
		TSet<FString> PendingDocuments;
		TMap<FString, TPair<FString, FString>> PendingRows;
		const auto RememberPending = [&](const FString& Id) {
			return Inventory->VisitUnit(Id, [&](const FAcceptedWorkspaceInventoryRow& Row) {
				PendingRows.Add(Id, {Row.DocumentPath, Row.UnitPointer}); PendingDocuments.Add(Row.DocumentPath);
				if (PendingLocations) PendingLocations->Add(Id, {Row.DocumentPath, Row.UnitPointer}); return true;
			});
		};
		const auto MissingPending = [&](const FString& Id) -> bool {
			Error.SetSourceFailure(TEXT("workspace_committed_author_location_missing"),
				TEXT("The known pending author input has no accepted public address. Author bytes and the whole commit's consumption position remain unchanged."), Id);
			return false;
		};
		for (const auto& Pending : Settings.PendingAuthorUnits) if (!RememberPending(Pending.Key)) return MissingPending(Pending.Key);
		for (const auto& Pending : Settings.PendingKeyedAuthorRows) if (!RememberPending(Pending.Key)) return MissingPending(Pending.Key);
		struct FRow {
			FString Id, Document, Pointer, Envelope;
			bool bMachineChanged = false;
			FUeAgentCanonicalValuePtr Visible, Thin, Captured;
			TArray<uint8> CapturedBytes;
		};
		TArray<FRow> Rows; TMap<FString, int32> RowIndices;
		for (const auto& Pair : Units) {
			const auto& Unit = *Pair.Value;
			if (!Inventory->VisitUnit(Pair.Key, [&](const FAcceptedWorkspaceInventoryRow& Location) {
				FRow Row; Row.Id = Pair.Key; Row.Document = Location.DocumentPath; Row.Pointer = Location.UnitPointer; Row.Envelope = Location.EnvelopePath;
				TSharedPtr<FJsonObject> Compact; const auto Visible = MakeShared<FJsonObject>();
				const auto Address = DescribeReadableProperty(Unit, *Model);
				if (!TryBuildCompactRow(Unit, Compact, Error, nullptr, Row.Document)
					|| !TryBuildPublishedPropertyRow(Unit, *Model, Address, Compact, Visible, Error)
					|| !TryCanonicalizeJson(MakeShared<FJsonValueObject>(Visible), Resources, Row.Visible, Error)) return false;
				TArray<uint8> VisibleBytes; FString ContentHash;
				if (!TrySerializeSemanticHjsonObject(*Row.Visible, VisibleBytes, Error, Row.Document)
					|| !TryHashSemanticUnitRow(Unit, ContentHash, Error, Row.Document)) return false;
				const FString EditHash = Sha256(VisibleBytes);
				Row.bMachineChanged = !Location.ContentHashEquals(ContentHash) || Location.ReadEditHash() != EditHash;
				FUeAgentCanonicalValuePtr Complete;
				if (Row.bMachineChanged) {
					if (!TryBuildPublishedMachineRow(Unit, *Model, Location.PublicRef, Location.LogicalPath, Location.DocumentPath,
						Location.UnitPointer, Location.ContentFiles, ContentHash, EditHash, Resources, Complete, Error)) return false;
					auto Fields = Complete->GetObjectItems();
					const auto* Captured = Fields.FindByPredicate([](const auto& Field) { return Field.Key == TEXT("envelope"); });
					if (!Captured) return false;
					Row.Captured = Captured->Value;
					Fields.RemoveAll([](const auto& Field) { return Field.Key == TEXT("envelope"); });
					FUeAgentCanonicalValuePtr Reference; FUeAgentStateSemanticError Failure;
					if (!FUeAgentCanonicalValue::TryString(Row.Envelope, Resources, Reference, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
					Fields.Emplace(TEXT("envelope_ref"), Reference.ToSharedRef());
					if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Row.Thin, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
				if (!TrySerializeSemanticHjsonObject(*Row.Captured, Row.CapturedBytes, Error, Row.Envelope)) return false;
					RowIndices.Add(Row.Id, Rows.Num());
				}
				// The machine baseline is actual A. The author row may already be
				// newer B on that same field; write its existing typed merge once.
				if (const auto* Pending = Settings.PendingAuthorUnits.Find(Row.Id)) {
					TSharedPtr<FJsonObject> PendingCompact; Contract::FDiagnostic Failure;
					if (!TryBuildCompactRow(*Pending, PendingCompact, Error, nullptr, Row.Document)) return false;
					const auto Value = DirectPropertyValue(PendingCompact, *Pending, *Model, Failure);
					FUeAgentCanonicalValuePtr Canonical, Replaced;
					if (!Value || !TryCanonicalizeJson(Value, Resources, Canonical, Error)) { if (!Error.HasErrors()) Error.SetFailure(MoveTemp(Failure), Row.Document); return false; }
					if (!TryReplacePublishedObject(Row.Visible.ToSharedRef(), TEXT("/value"), Canonical.ToSharedRef(), Resources, Replaced, Error, Row.Document)) return false;
					Row.Visible = MoveTemp(Replaced);
				}
				Rows.Add(MoveTemp(Row)); return true;
			})) {
				if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("workspace_captured_unit_unavailable"), TEXT("The required property has no accepted publication location."), Pair.Key);
				return false;
			}
		}
		FExportWorkspacePlanPtr Header;
		if (!TryBuildCommittedPublicationHeader(Binding, TargetKind, Settings, Resources, Header, Error)) return false;
		TArray<FExportFilePlan> Files = Header->GetFiles(); TMap<FString, FString> Retained = Header->GetRetainedFileHashes();
		TMap<FString, FUeAgentCanonicalValuePtr> Documents;
		TSet<FString> Buckets;
		for (const auto& Row : Rows) {
			if (Row.bMachineChanged) Buckets.Add(Row.Envelope);
			bool bCoveredByNewerParent = false;
			for (const auto& Pending : PendingRows) if (Pending.Key != Row.Id && Pending.Value.Key == Row.Document)
				bCoveredByNewerParent |= Pending.Value.Value == TEXT("/") || Pending.Value.Value == Row.Pointer
					|| Row.Pointer.StartsWith(Pending.Value.Value + TEXT("/"), ESearchCase::CaseSensitive);
			if (bCoveredByNewerParent) continue; // Current author parent stays intact; actual A is in its machine baseline.
			const bool bFullDocument = FullRebuildDocuments && FullRebuildDocuments->Contains(Row.Document);
			auto* Existing = Documents.Find(Row.Document);
			if (!Existing) {
				FHjsonDocumentPtr Parsed; FUeAgentCanonicalValuePtr Value;
				if (bFullDocument) {
					FUeAgentStateSemanticError Failure;
					if (!FUeAgentCanonicalValue::TryObject({}, Resources, Value, Failure)) { Error.SetFailure(MoveTemp(Failure), Row.Document); return false; }
				} else if (!TryReadAcceptedSemanticSourceDocument(Root, Row.Document, Resources, Parsed, Error)
					|| !TryProjectHjsonCanonicalValue(*Parsed->GetRoot(), Resources, Value, Error, false)) return false;
				Existing = &Documents.Add(Row.Document, MoveTemp(Value));
			}
			FUeAgentCanonicalValuePtr Changed;
			if (!TryReplacePublishedObject(Existing->ToSharedRef(), Row.Pointer, Row.Visible.ToSharedRef(), Resources, Changed, Error, Row.Document, bFullDocument)) {
				if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("workspace_committed_author_location_missing"), TEXT("The required public parent no longer contains this property location."), Row.Document, Row.Pointer);
				return false;
			}
			*Existing = MoveTemp(Changed);
			if (PendingLocations) PendingLocations->Add(Row.Id, {Row.Document, Row.Pointer});
		}
		for (const auto& Document : Documents) {
			TArray<uint8> Bytes; if (!TrySerializeSemanticHjsonObject(*Document.Value, Bytes, Error, Document.Key)) return false;
			const FString Hash = Sha256(Bytes); Files.Emplace(Document.Key, MoveTemp(Bytes), Hash); Retained.Remove(Document.Key);
			Files.Last().SetSemanticSourceDocument(true);
		}
		int32 PreservedRows = 0;
		for (const auto& Bucket : Buckets) {
			const FString Shard = Bucket.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
			TArray<uint8> OldCaptured; FString CapturedHash, InventoryHash;
			if (!TryReadAcceptedSemanticSourceBytes(Root, Bucket, OldCaptured, CapturedHash, Error)) return false;
			TArray<FUeAgentStructuredHjsonObjectFragment> Captured;
			TArray<FUeAgentCanonicalValueRef> Metadata;
			auto Identities = MakeShared<FExportFilePlan::FFragmentIdentities>();
			if (!Inventory->VisitBucket(Bucket, [&](const FAcceptedWorkspaceInventoryRow& Location) {
				if (!Location.SharedUnitIdentity) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.machine_identity"),
					Location.UnitId, Bucket, TEXT("missing immutable row identity"), TEXT("same-row inventory identity"), TEXT("original row formatter"));
				Identities->Add(Location.SharedUnitIdentity.ToSharedRef());
				if (const auto* Changed = RowIndices.Find(Location.UnitId)) {
					const auto& Row = Rows[*Changed];
					Captured.Add({Row.Id, MakeArrayView(Row.CapturedBytes.GetData(), Row.CapturedBytes.Num() - 1), 0});
					Metadata.Add(Row.Thin.ToSharedRef()); return true;
				}
				const auto CapturedView = PublishedMachineSpanView(OldCaptured, Location.EnvelopeSpan, 1);
				if (CapturedView.IsEmpty()) {
					Error.SetSourceFailure(TEXT("workspace_publication_row_span_missing"),
						TEXT("The same-publication inventory/baseline producer did not provide a valid retained row span. No whole-bucket parser was selected; the native result and author bytes remain pending."), Location.UnitId);
					return false;
				}
				FUeAgentCanonicalValuePtr Thin;
				if (!TryBuildPublishedInventoryMetadata(Location, Resources, Thin, Error)) return false;
				Captured.Add({Location.UnitId, CapturedView, 1}); Metadata.Add(Thin.ToSharedRef());
				++PreservedRows; return true;
			})) return false;
			TArray<uint8> CapturedBytes, ThinBytes; TArray<FIntPoint> CapturedSpans, ThinSpans; Contract::FDiagnostic Failure;
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObjectFragments(Captured, FString(), CapturedBytes, Failure, &CapturedSpans)) { Error.SetFailure(MoveTemp(Failure), Bucket); return false; }
			if (!TrySerializePublishedInventoryWithEnvelopeSpans(Metadata, CapturedSpans, Resources, ThinBytes, ThinSpans, Error, Shard)) return false;
			CapturedHash = Sha256(CapturedBytes); InventoryHash = Sha256(ThinBytes);
			Files.Emplace(Bucket, MoveTemp(CapturedBytes), CapturedHash, MoveTemp(CapturedSpans), Identities);
			Files.Emplace(Shard, MoveTemp(ThinBytes), InventoryHash, MoveTemp(ThinSpans), Identities);
			Retained.Remove(Bucket); Retained.Remove(Shard);
		}
		FString Material = Header->GetPlanFingerprint();
		for (const auto& File : Files) {
			ArtifactPublisher::RememberVerifiedPlanBytes(File);
			Material += TEXT("|") + File.GetRelativePath() + TEXT("|") + File.GetContentHash();
		}
		OutPlan = MakeShared<FExportWorkspacePlan>(EWorkspaceKind::Directory, ManifestPath, Binding.SnapshotId, Binding.Revision,
			Binding.EvidenceId, MoveTemp(Files), Sha256Text(Material), Header->GetObservedTokens(), MoveTemp(Retained), TOptional<TSet<FString>>(MoveTemp(PendingDocuments)));
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=property_publication_rows actual_rows=%d public_documents=%d machine_buckets=%d preserved_machine_rows=%d complete_layout=false complete_inventory_material=false"),
			Rows.Num(), Documents.Num(), Buckets.Num(), PreservedRows);
		return true;
	}

	template <typename TDeclaration>
	static FAuthoringExampleCatalog::FPropertyInstruction DescribePropertyGuide(
		const TDeclaration& Declaration, const FAuthoringModel& Model, const FString* CurrentPointer = nullptr)
	{
		FString Category = ReadablePropertyCategory(Declaration.GetCategoryPath());
		FString Key = ReadablePropertyKey(Declaration);
		FString Pointer = CurrentPointer ? *CurrentPointer
			: TEXT("/categories/") + EscapeJsonPointerSegment(Category) + TEXT("/") + EscapeJsonPointerSegment(Key);
		return {MoveTemp(Pointer), ReadablePropertyType(Declaration, Model), ReadablePropertyDefinition(Declaration, Model),
			MoveTemp(Category), MoveTemp(Key)};
	}

	static bool TryParsePropertyGuideMetadata(const FString& Path, const TArray<uint8>& Bytes,
		FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Metadata, FWorkspaceError& Error)
	{
		const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		FHjsonNodePtr Root;
		if (!TryParseHjsonSelectedNodes(FString(Decoded.Length(), Decoded.Get()), Path, Resources, Root, Error,
			[](const FString& Pointer, const FString& Key) {
				TArray<FString> Parts; Pointer.ParseIntoArray(Parts, TEXT("/"), true);
				const int32 ValueAt = Parts.Num() > 0 && Parts[0] == TEXT("value") ? 0
					: Parts.Num() > 3 && Parts[0] == TEXT("categories") && Parts[3] == TEXT("value") ? 3 : INDEX_NONE;
				return ValueAt == INDEX_NONE || Parts.Num() <= ValueAt + 1
					|| (Parts.Num() == ValueAt + 2 && Key == TEXT("content_ref"));
			})) return false;
		return TryProjectHjsonCanonicalValue(*Root, Resources, Metadata, Error);
	}

	struct FCompositionOutputScope
	{
		bool bPublishGuides = true;
		TSet<FString> Rows;
		TSet<FString> GuideDefinitions;
		FWorkspaceLayoutPlanPtr Layout;
		TMap<FString, FUeAgentCanonicalValueRef> Documents;
		TMap<FString, FUeAgentCanonicalValueRef> MachineRows;
		TMap<FString, FString> Pointers;
		TMap<FString, TArray<FString>> ContentFiles;
		TArray<FExportFilePlan> Files;
		TSharedPtr<FJsonObject> PreviousExampleIndex;
		TSet<FString> ReplacedDocuments;
		TMap<FString, FUeAgentCanonicalValueRef> PreviousGuides, ProducedGuides;
		TMap<FString, TArray<FString>> ReplacedPointers;
		const TMap<FString, FString>* PublishedFileHashes = nullptr;
		TSet<FString> ReplacedContentPaths;
		int32 RenderedDefinitionRows = 0;
	};

	static bool TryReadPublishedCanonicalDocument(const FString& Root, const FString& Path,
		const FString& PublishedHash, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& Value, FWorkspaceError& Error)
	{
		TArray<uint8> Bytes;
		if (!TryReadPublishedWorkspaceBytes(Root, Path, PublishedHash, Bytes, Error)) return false;
		const FSourceProvenance Origin(Path, TEXT("/"), 1, 1, 0, Bytes.Num(), FString());
		FHjsonDocumentPtr Parsed;
		return TryParseGeneratedHjsonSpan(Bytes, Origin, Resources, Parsed, Error)
			&& TryProjectHjsonCanonicalValue(*Parsed->GetRoot(), Resources, Value, Error, false);
	}

	static bool TryBuildCompositionExportPlanInternal(
		FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& SnapshotBinding,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError, const FString& TargetKind, const FPreparedWorkspaceExport* ExportSettings,
		TMap<FString, TPair<FString, FString>>* OutPendingAuthorLocations,
		FCompositionOutputScope* OutputScope)
	{
		const double PlanStarted = FPlatformTime::Seconds();
		OutPlan.Reset();
		OutError.Reset();
		if (!SnapshotBinding.IsValid())
		{
			OutError.SetSourceFailure(TEXT("composition_snapshot_binding_invalid"), SnapshotBinding.SnapshotId.TrimStartAndEnd().IsEmpty() ? TEXT("The captured snapshot binding has no snapshot identifier.") : !SnapshotBinding.Revision.IsValid() ? TEXT("The captured snapshot binding has an invalid revision.") : TEXT("The captured snapshot binding has no readback evidence identifier."));
			return false;
		}
		const FString ProjectionBindingRoot = ExportSettings && !ExportSettings->AcceptedWorkspaceRoot.IsEmpty()
			? ExportSettings->AcceptedWorkspaceRoot : GetAcceptedSemanticWorkspaceRoot();
		const auto AcceptedSource = GetAcceptedSemanticWorkspace(ProjectionBindingRoot);
		const bool bNoAuthoredBodyWriteback = AcceptedSource && AcceptedSource->GetEditedUnitIds().IsSet()
			&& AcceptedSource->GetEditedUnitIds()->IsEmpty();
		const auto AcceptedInventory = bNoAuthoredBodyWriteback ? GetAcceptedWorkspaceInventory(ProjectionBindingRoot) : nullptr;
		if (!OutputScope && ExportSettings && ExportSettings->ActualChanges && ExportSettings->ActualChanges->Changes.IsEmpty()
			&& ExportSettings->SourcePublication.bDirectDataflow && ExportSettings->Publication.bDirectDataflow
			&& bNoAuthoredBodyWriteback && AcceptedInventory)
			return TryBuildCommittedPublicationHeader(SnapshotBinding, TargetKind, *ExportSettings, Resources, OutPlan, OutError);
		// A cumulative difference belongs to its destination publication. A
		// last Model pointer or the survival of an intermediate merge is not
		// part of that identity. An optional output memo from another publication
		// cannot be used as this origin.
		// The accepted source owns the destination's generated material. The
		// last composition may belong to a different workspace and is merely a
		// memo, not the origin of this destination's cumulative actual changes.
		auto AcceptedBaseline = OutputScope ? nullptr : PreviousCompositionExport;
		if (!OutputScope && ExportSettings && ExportSettings->ActualChanges) {
			return FailWorkspaceDataflowHandoff(OutError, TEXT("composition.complete_planner"),
				TEXT("changed-row output scope"), ProjectionBindingRoot, TEXT("actual changes reached complete planner"),
				TEXT("property or scoped-structure publication"), TEXT("TryBuildCompositionExportPlan changed-row dispatch"));
		}
		const auto ProjectionBindings = GetAcceptedSemanticProjectionBindings(ProjectionBindingRoot, Model,
			ExportSettings ? ExportSettings->ActualChanges.Get() : nullptr,
			ExportSettings && (ExportSettings->ActualExportMode == TEXT("full") || ExportSettings->ActualExportMode == TEXT("incremental")));
		const bool bAcceptedOrigin = ExportSettings && ExportSettings->ActualChanges && AcceptedBaseline
			&& AcceptedBaseline->SourceLayout && ExportSettings->SourcePublication.bGenerationPublication
			&& AcceptedBaseline->Publication.bGenerationPublication
			&& ExportSettings->SourcePublication.State == TEXT("committed")
			&& ExportSettings->SourcePublication.Generation.Equals(AcceptedBaseline->Publication.Generation, ESearchCase::CaseSensitive);
		const auto* ActualChanges = ExportSettings ? ExportSettings->ActualChanges.Get() : nullptr;
		// A memo can be borrowed only for its own publication. The actual
		// difference itself remains valid even when that optional memo missed.
		const auto* MemoActualChanges = bAcceptedOrigin ? ActualChanges : nullptr;
		FWorkspaceLayoutPlanPtr Layout;
		if (!TryPlanWorkspaceLayout(
			Model,
			2,
			EWorkspaceSourceMode::Directory,
			MoveTemp(Telemetry),
			Resources,
			Layout,
			OutError, ProjectionBindings.Get(), MemoActualChanges,
			bAcceptedOrigin ? AcceptedBaseline->SourceLayout.Get() : nullptr)
			|| !Layout.IsValid())
		{
			return false;
		}

		const double LayoutFinished = FPlatformTime::Seconds();
		// Direct publication compares this complete current input with the actual
		// accepted publication below. The optional previous formatter memo is not
		// another authority for that comparison, nor an input to K-row output.
		const bool bDirectPublication = ExportSettings && ExportSettings->Publication.bDirectDataflow;
		TSharedPtr<const FCompositionExportBaseline> Baseline = OutputScope || bDirectPublication
			? nullptr : bAcceptedOrigin ? AcceptedBaseline : PreviousCompositionExport;
		bool bReusableBaseline = Baseline.IsValid() && ExportDataIdentical(Baseline->TargetKind, TargetKind)
			&& ExportDataIdentical(Baseline->ContractId, Model->GetContractId())
			&& Baseline->ContractVersion == Model->GetContractVersion()
			&& ExportDataIdentical(Baseline->ProfileId, Model->GetProfileId())
			&& ExportDataIdentical(Baseline->TargetUnitIds, Model->GetTargetUnitIds())
			&& Baseline->LayoutVersion == Layout->GetLayoutVersion();
		bool bIdenticalLayout = bReusableBaseline
			&& ((Baseline->ProjectionIdentity.IsValid()
				&& Baseline->ProjectionIdentity.Pin() == Layout->GetProjectionIdentity().Pin())
			|| (ExportMapsIdentical(Baseline->Projection->LogicalPlacements, Layout->GetLogicalPlacements())
			&& ExportMapsIdentical(Baseline->Projection->PhysicalPlacements, Layout->GetPhysicalPlacements())
			&& ExportMapsIdentical(Baseline->Projection->PhysicalFiles, Layout->GetPhysicalFiles())
			&& ExportMapsIdentical(Baseline->GetPublicRefs(), Layout->GetMachineManifest().PublicRefMapping)));
		bool bRetainedDefinitionBindings = bReusableBaseline && Baseline->DefinitionProjectionHints.IsValid();
		if (bRetainedDefinitionBindings)
		{
			const auto& PreviousBindings = Baseline->DefinitionProjectionHints->Definitions;
			const auto& CurrentBindings = Layout->GetDefinitionProjectionHints().Definitions;
			for (const auto& Pair : PreviousBindings)
			{
				const auto* Previous = CurrentBindings.Find(Pair.Key);
				if (!Previous || !ExportDataIdentical(Previous->ContentKey, Pair.Value.ContentKey)
					|| !ExportDataIdentical(Previous->DefinitionName, Pair.Value.DefinitionName)
					|| !ExportDataIdentical(Previous->ReadableAlias, Pair.Value.ReadableAlias)
					|| !ExportDataIdentical(Previous->PublicRef, Pair.Value.PublicRef))
				{
					bRetainedDefinitionBindings = false;
					break;
				}
			}
		}
		const bool bAllDefinitionBindings = bRetainedDefinitionBindings
			&& Baseline->DefinitionProjectionHints->Definitions.Num() == Layout->GetDefinitionProjectionHints().Definitions.Num();
		bool bIdenticalDefinitionBindings = bAllDefinitionBindings;
		// Keep prior keys at their actual owner through all old/new comparisons.
		// Only newly computed keys need request-local storage; the final consumer
		// below transfers the existing tables, rather than deep-copying every row.
		const auto ContentKeyBaseline = bReusableBaseline ? Baseline : nullptr;
		TMap<FString, FUnitContentProof> UnitContentKeys;
		if (!ContentKeyBaseline && !bDirectPublication) UnitContentKeys.Reserve(Model->GetUnitCount());
		const auto FindCurrentContentKey = [&](const FString& Id) -> const FUnitContentProof* {
			if (const auto* Current = UnitContentKeys.Find(Id)) return Current;
			return ContentKeyBaseline ? ContentKeyBaseline->UnitContentKeys.Find(Id) : nullptr;
		};
		int32 ReusedUnitContentProofs = 0;
		TMap<FString, FString> ItemValueInvariantKeys;
		TSet<FString> RecomputedInvariantUnits;
		TSet<FString> ChangedUnitIds;
		int32 ChangedPropertyUnits = 0, ChangedValueItems = 0, ChangedOtherUnits = 0, ChangedProjectionValueUnits = 0;
		const FString ProjectionValueRoot = ExportSettings && !ExportSettings->AcceptedWorkspaceRoot.IsEmpty()
			? ExportSettings->AcceptedWorkspaceRoot : GetAcceptedSemanticWorkspaceRoot();
		const auto ProjectionValueSource = GetAcceptedSemanticWorkspace(ProjectionValueRoot);
		// Global mapping changes are only a reason to locate changed keys. They
		// do not invalidate unrelated blocks or become another manifest hash gate.
		TSet<FString> ChangedProjectionUnits;
		if (bReusableBaseline && !bIdenticalLayout) {
			auto ObserveChanges = [&](const auto& Current, const auto& Before) {
				for (const auto& Pair : Current) {
					const auto* Old = Before.Find(Pair.Key);
					if (!Old || !ExportDataIdentical(Pair.Value, *Old)) ChangedProjectionUnits.Add(Pair.Key);
				}
				for (const auto& Pair : Before) if (!Current.Contains(Pair.Key)) ChangedProjectionUnits.Add(Pair.Key);
			};
			// The placement producer already retained an exact immutable document
			// only when its complete ordered members and both paths were equal.
			// Consume that result instead of re-reading its rows through two maps.
			for (const auto& File : Layout->GetPhysicalFiles()) {
				const auto* Old = Baseline->Projection->PhysicalFiles.Find(File.Key);
				if (Old && Old->GetDocument() == File.Value.GetDocument()) continue;
				for (const FString& Id : File.Value) {
					const auto* OldLogical = Baseline->Projection->LogicalPlacements.Find(Id);
					const auto* OldPhysical = Baseline->Projection->PhysicalPlacements.Find(Id);
					if (!OldLogical || !OldPhysical
						|| !ExportDataIdentical(*OldLogical, Layout->GetLogicalPlacements().FindChecked(Id))
						|| !ExportDataIdentical(*OldPhysical, Layout->GetPhysicalPlacements().FindChecked(Id))) ChangedProjectionUnits.Add(Id);
				}
			}
			for (const auto& File : Baseline->Projection->PhysicalFiles) {
				const auto* Current = Layout->GetPhysicalFiles().Find(File.Key);
				if (Current && Current->GetDocument() == File.Value.GetDocument()) continue;
				for (const FString& Id : File.Value)
					if (!Layout->GetPhysicalPlacements().Contains(Id)) ChangedProjectionUnits.Add(Id);
			}
			const auto& Names = Layout->GetSharedProjection()->ReferenceNamespace;
			const auto PreviousNames = Names ? Names->Previous.Pin() : nullptr;
			if (PreviousNames && PreviousNames == Baseline->Projection->ReferenceNamespace) {
				// The allocator identifies every non-inherited name, including
				// removed members. Compare actual results only for those inputs.
				for (const auto& Id : Names->ChangedNames) {
					const auto* Current = Layout->GetMachineManifest().PublicRefMapping.Find(Id.Get());
					const auto* Before = Baseline->GetPublicRefs().Find(Id.Get());
					if ((!Current) != (!Before) || (Current && !ExportDataIdentical(*Current, *Before))) ChangedProjectionUnits.Add(Id.Get());
				}
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=reference_name_delta logical_names=%d compared_inputs=%d producer=original_allocator"),
					Layout->GetMachineManifest().PublicRefMapping.Num(), Names->ChangedNames.Num());
			} else ObserveChanges(Layout->GetMachineManifest().PublicRefMapping, Baseline->GetPublicRefs());
		}
		if (bReusableBaseline && !bIdenticalDefinitionBindings) {
			const auto& Current = Layout->GetDefinitionProjectionHints().Definitions;
			if (Baseline->DefinitionProjectionHints) {
				const auto& Before = Baseline->DefinitionProjectionHints->Definitions;
				for (const auto& Pair : Current) {
					const auto* Old = Before.Find(Pair.Key);
					if (!Old || !ExportDataIdentical(Pair.Value.ContentKey, Old->ContentKey)
						|| !ExportDataIdentical(Pair.Value.DefinitionName, Old->DefinitionName)
						|| !ExportDataIdentical(Pair.Value.ReadableAlias, Old->ReadableAlias)
						|| !ExportDataIdentical(Pair.Value.PublicRef, Old->PublicRef)) ChangedProjectionUnits.Add(Pair.Key);
				}
				for (const auto& Pair : Before) if (!Current.Contains(Pair.Key)) ChangedProjectionUnits.Add(Pair.Key);
			} else for (const auto& Pair : Current) ChangedProjectionUnits.Add(Pair.Key);
		}
		TArray<TSharedPtr<const FPlanUnitProofBlock>> UnitProofBlocks;
		TArray<uint8> ReusableUnitBlocks;
		int32 ReusedPlanBlocks = 0;
		int64 PlanBlockBytes = 0;
		const auto& ModelUnits = Model->GetDeclarations();
		TSet<FString> ActualOutputChanges;
		if (MemoActualChanges) {
			for (const auto& Change : ActualChanges->Changes) ActualOutputChanges.Add(Change.UnitId);
			ActualOutputChanges.Append(ChangedProjectionUnits);
			// Projection dependencies live in the declaration directory. Looking
			// for affected readers does not require serializing or hashing every
			// value again, nor a fixed-position storage proof for unchanged rows.
			bool bAddedReader;
			do {
				bAddedReader = false;
				Model->VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Unit, int32) {
					if (ActualOutputChanges.Contains(Unit.GetUnitId())) return true;
					bool bAffected = ActualOutputChanges.Contains(Unit.GetFieldSetRef()) || ChangedProjectionUnits.Contains(Unit.GetOwnerRef());
					for (const auto& Id : Unit.GetDependencyRefs()) if (ActualOutputChanges.Contains(Id)) { bAffected = true; break; }
					if (bAffected) { ActualOutputChanges.Add(Unit.GetUnitId()); bAddedReader = true; }
					return true;
				});
			} while (bAddedReader);
			ReusableUnitBlocks.Init(0, FMath::DivideAndRoundUp(ModelUnits.Num(), 256));
		} else if (bDirectPublication) {
			ReusableUnitBlocks.Init(0, FMath::DivideAndRoundUp(ModelUnits.Num(), 256));
		} else for (int32 First = 0; First < ModelUnits.Num(); First += 256) {
			const int32 Count = FMath::Min(256, ModelUnits.Num() - First);
			const int32 BlockIndex = First / 256;
			const auto Prior = bReusableBaseline && Baseline->UnitProofBlocks.IsValidIndex(BlockIndex)
				? Baseline->UnitProofBlocks[BlockIndex] : nullptr;
			bool bSame = Prior && Prior->First == First && Prior->Units.Num() == Count;
			for (int32 Offset = 0; bSame && Offset < Count; ++Offset) {
				const auto& Unit = *Model->FindUnit(ModelUnits[First + Offset].GetUnitId());
				bSame = Unit.SharesStorageIdentity(Prior->Units[Offset])
					&& (ChangedProjectionUnits.IsEmpty() || !ChangedProjectionUnits.Contains(Unit.GetUnitId()));
			}
			if (bSame) for (const auto& Dependency : Prior->Dependencies) {
				const auto* Unit = Model->FindUnit(Dependency.Key);
				if (!Unit || !Unit->SharesStorageIdentity(Dependency.Value) || ChangedProjectionUnits.Contains(Dependency.Key)) { bSame = false; break; }
			}
			ReusableUnitBlocks.Add(bSame ? 1 : 0);
			if (bSame) {
				const int64 Charge = Prior->Charge();
				if (PlanBlockBytes + Charge <= 16 * 1024 * 1024) { UnitProofBlocks.Add(Prior); PlanBlockBytes += Charge; }
				else UnitProofBlocks.Add(nullptr);
				ReusedUnitContentProofs += Count; ++ReusedPlanBlocks; continue;
			}
			if (UnitProofBlocks.Num() >= 8192 || PlanBlockBytes >= 16 * 1024 * 1024) { UnitProofBlocks.Add(nullptr); continue; }
			auto Next = MakeShared<FPlanUnitProofBlock>(); Next->First = First;
			TSet<FString> Dependencies;
			for (int32 Offset = 0; Offset < Count; ++Offset) {
				const auto& Unit = *Model->FindUnit(ModelUnits[First + Offset].GetUnitId());
				Next->Units.Add(Unit.GetStorageIdentity());
				for (const auto& Id : Unit.GetDependencyRefs()) Dependencies.Add(Id);
				if (FindSemanticFieldSetDeclaration(Unit, *Model)) Dependencies.Add(Unit.GetFieldSetRef());
			}
			bool bKnown = true;
			for (const auto& Id : Dependencies) {
				const auto* Unit = Model->FindUnit(Id);
				if (!Unit) { bKnown = false; break; }
				Next->Dependencies.Emplace(Id, Unit->GetStorageIdentity());
			}
			const int64 Charge = Next->Charge();
			if (!bKnown || PlanBlockBytes + Charge > 16 * 1024 * 1024) UnitProofBlocks.Add(nullptr);
			else { PlanBlockBytes += Charge; UnitProofBlocks.Add(MoveTemp(Next)); }
		}
		const auto& PlannedInventory = Layout->GetMachineManifest().UnitInventory;
		int32 ReusedPlannedHashes = 0, RebuiltContentHashes = 0;
		auto AppendProof = [](FString& Key, const FString& Value)
		{
			Key += FString::Printf(TEXT("|%d:"), Value.Len()) + Value;
		};
		for (int32 UnitIndex = 0; UnitIndex < ModelUnits.Num(); ++UnitIndex)
		{
			if (ReusableUnitBlocks[UnitIndex / 256]) continue;
			const auto& Declaration = ModelUnits[UnitIndex];
			// Only the property-document definition closure reads these framed
			// keys in direct output. All other current/accepted rows are compared
			// by their real row/declaration/placement facts, not this old memo.
			if (bDirectPublication && (Declaration.GetUnitKind() != ESemanticUnitKind::CollectionState
				|| Declaration.GetRole() != TEXT("property_definition"))) continue;
			if (MemoActualChanges && !ActualOutputChanges.Contains(Declaration.GetUnitId())) {
				++ReusedUnitContentProofs;
				continue;
			}
			const auto* UnitValue = Model->FindUnit(Declaration.GetUnitId());
			if (!UnitValue) { Model->CheckValueReadResult(OutError); return false; }
			const FSemanticUnit& Unit = *UnitValue;
			// The current model and layout have still been built and validated.
			// Reuse only this immutable unit plus every dependency read by its
			// projection, under identical placements and definition bindings.
			const FUnitContentProof* Previous = bReusableBaseline && !ChangedProjectionUnits.Contains(Unit.GetUnitId())
				? Baseline->UnitContentKeys.Find(Unit.GetUnitId()) : nullptr;
			bool bSameProjectionInputs = Previous && !Previous->Key.IsEmpty()
				&& Unit.SharesStorageIdentity(Previous->StorageIdentity);
			const auto SameDependency = [&](const FString& Id) {
				const auto* Dependency = Model->FindUnit(Id);
				const auto* Before = Baseline->UnitContentKeys.Find(Id);
				return Dependency && Before && !ChangedProjectionUnits.Contains(Id)
					&& Dependency->SharesStorageIdentity(Before->StorageIdentity);
			};
			if (bSameProjectionInputs)
				for (const FString& DependencyId : Unit.GetDependencyRefs())
				{
					if (!SameDependency(DependencyId))
					{
						bSameProjectionInputs = false;
						break;
					}
				}
			if (bSameProjectionInputs && FindSemanticFieldSetDeclaration(Unit, *Model))
				bSameProjectionInputs = SameDependency(Unit.GetFieldSetRef());
			if (bSameProjectionInputs)
			{
				// These exact records were already inherited above. The same
				// immutable unit/dependency/projection proof as a reusable block is
				// valid by stable identity when insertion shifts its old position.
				++ReusedUnitContentProofs;
				continue;
			}
			FString Key;
			// Layout just hashed this immutable unit's exact canonical output row.
			// Numeric spellings that the public writer projects identically are
			// the same publication input. Keep every export-specific proof below;
			// this does not substitute a row hash for the layout or model proof.
			// Layout emits inventory in this model's order. Verify the current
			// identity at the actual consumer; do not build another complete
			// long-key map merely to find the few unshared units' hashes.
			if (PlannedInventory.IsValidIndex(UnitIndex)
				&& PlannedInventory[UnitIndex].UnitId.Equals(Unit.GetUnitId(), ESearchCase::CaseSensitive)) {
				Key = PlannedInventory[UnitIndex].ContentHash; ++ReusedPlannedHashes;
			} else {
				++RebuiltContentHashes;
				if (!TryHashSemanticUnitRow(Unit, Key, OutError)) return false;
			}
			// Semantic hashing deliberately excludes presentation provenance and
			// category. Include both in the export proof, with unambiguous framing.
			Key += FString::Printf(TEXT("|%d:"), Unit.GetSourceProvenance().Len()) + Unit.GetSourceProvenance();
			for (const FString& Category : Unit.GetCategoryPath())
				Key += FString::Printf(TEXT("|%d:"), Category.Len()) + Category;
			// A semantic row can stay equal while a new definition changes its
			// readable alias, or layout moves its public/document binding.
			AppendProof(Key, Layout->GetLogicalPlacements().FindRef(Unit.GetUnitId()));
			AppendProof(Key, Layout->GetPhysicalPlacements().FindRef(Unit.GetUnitId()));
			AppendProof(Key, Layout->GetMachineManifest().PublicRefMapping.FindRef(Unit.GetUnitId()));
			const bool bProperty = Unit.GetUnitKind() == ESemanticUnitKind::PropertySet;
			FReadablePropertyAddress Address;
			if (bProperty) Address = DescribeReadableProperty(Unit, *Model);
			// Preserve the exact existing framed material, including both uses
			// of Definition, while observing that descriptor only once.
			AppendProof(Key, bProperty ? Address.Definition : ReadablePropertyDefinition(Unit, Model.Get()));
			if (bProperty)
			{
				AppendProof(Key, Address.Key);
				AppendProof(Key, Address.Category);
				AppendProof(Key, Address.Type);
				AppendProof(Key, Address.Definition);
			}
			for (const FString& Dependency : Unit.GetDependencyRefs())
			{
				AppendProof(Key, Layout->GetMachineManifest().PublicRefMapping.FindRef(Dependency));
				const FString* Name = Model->FindReadablePropertyDefinitionName(Dependency);
				AppendProof(Key, Name ? *Name : FString());
			}
			UnitContentKeys.Add(Unit.GetUnitId(), {Sha256Text(Key), Unit.GetStorageIdentity()});
			RecomputedInvariantUnits.Add(Unit.GetUnitId());
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState && Unit.HasValue(TEXT("value")))
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
			if (bReusableBaseline) {
				const auto* Before = Baseline->UnitContentKeys.Find(Unit.GetUnitId());
				if (!Before || !ExportDataIdentical(UnitContentKeys.FindChecked(Unit.GetUnitId()).Key, Before->Key)) {
					ChangedUnitIds.Add(Unit.GetUnitId());
					if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet) ++ChangedPropertyUnits;
					else if (const auto* Invariant = ItemValueInvariantKeys.Find(Unit.GetUnitId()); Invariant
						&& Baseline->ItemValueInvariantKeys.Contains(Unit.GetUnitId())
						&& ExportDataIdentical(*Invariant, Baseline->ItemValueInvariantKeys.FindChecked(Unit.GetUnitId()))) ++ChangedValueItems;
					else {
						++ChangedOtherUnits;
						const auto* Captured = ProjectionValueSource ? ProjectionValueSource->FindBaselineUnit(Unit.GetUnitId()) : nullptr;
						if (TargetKind == TEXT("ue.level.asset") && Captured && IsClosedLevelProjectionValueChange(Unit, *Captured, OutError))
							++ChangedProjectionValueUnits;
						if (OutError.HasErrors()) return false;
					}
				}
			}
		}
		const int32 ComputedUnitContentKeyRows = UnitContentKeys.Num();
		auto IsIdenticalUnit = [&](const FString& Id) { return bReusableBaseline && !ChangedUnitIds.Contains(Id); };
		// Prove only the recursive definitions actually read by this document.
		// Unknown non-definition dependencies still take the full projection.
		// All memo entries belong to this one immutable model/plan. They are
		// discarded on return, and never stand in for another model's proof.
		struct FDefinitionReadDependencies { TArray<FString> Refs; bool bKnown = true; };
		TMap<FString, FDefinitionReadDependencies> DefinitionReadDependencies;
		TMap<FString, FString> SchemaKeysByRoots;
		TSet<FString> AcceptedSchemaRoots;
		int64 RetainedSchemaRootChars = 0;
		int32 ReusedSchemaClosures = 0, ReusedDefinitionReads = 0;
		auto TryPropertyDocumentSchemaKey = [&](const FWorkspaceFileMembers& Members, FString& OutKey, const TSet<FString>* UnprovenAcceptedUnits = nullptr)
		{
			TArray<FString> Pending;
			TSet<FString> Visited;
			for (const FString& Id : Members) {
				const auto Property = Model->FindDeclaration(Id);
				if (!Property || Property->GetUnitKind() != ESemanticUnitKind::PropertySet) return false;
				// Ordinary FieldSetRef is a property path, not a schema identity.
				if (FindSemanticFieldSetDeclaration(Property.GetValue(), *Model)) Pending.Add(Property->GetFieldSetRef());
				Pending.Append(Property->GetDependencyRefs());
			}
			Pending.Sort();
			FString RootKey;
			for (int32 I = 0; I < Pending.Num(); ++I)
				if (I == 0 || Pending[I] != Pending[I - 1]) AppendProof(RootKey, Pending[I]);
			if (const FString* Cached = SchemaKeysByRoots.Find(RootKey); Cached
				&& (!UnprovenAcceptedUnits || AcceptedSchemaRoots.Contains(RootKey))) {
				OutKey = *Cached; ++ReusedSchemaClosures; return true;
			}
			while (!Pending.IsEmpty()) {
				const FString Id = Pending.Pop(EAllowShrinking::No);
				if (Visited.Contains(Id)) continue;
				if (UnprovenAcceptedUnits && UnprovenAcceptedUnits->Contains(Id)) return false;
				const FSemanticUnit* Definition = Model->FindUnit(Id);
				if (!Definition || Definition->GetUnitKind() != ESemanticUnitKind::CollectionState
					|| Definition->GetRole() != TEXT("property_definition")) return false;
				Visited.Add(Id);
				FDefinitionReadDependencies Observed;
				const FDefinitionReadDependencies* Dependencies = DefinitionReadDependencies.Find(Id);
				if (Dependencies) ++ReusedDefinitionReads;
				else {
					Observed.Refs = Definition->GetDependencyRefs();
					TFunction<void(const FUeAgentCanonicalValue&)> Observe;
					Observe = [&](const FUeAgentCanonicalValue& Value) {
						if (Value.GetKind() == EUeAgentValueKind::String) {
							if (const FSemanticUnit* Ref = Model->FindUnit(Value.GetText())) {
								if (Ref->GetUnitKind() != ESemanticUnitKind::CollectionState
									|| Ref->GetRole() != TEXT("property_definition")) Observed.bKnown = false;
								else Observed.Refs.Add(Ref->GetUnitId());
							}
						} else if (Value.GetKind() == EUeAgentValueKind::Object) {
							for (const auto& Field : Value.GetObjectItems()) Observe(Field.Value.Get());
						} else if (Value.GetKind() == EUeAgentValueKind::Array) {
							for (const auto& Item : Value.GetArrayItems()) Observe(Item.Get());
						}
					};
					for (const TCHAR* Field : {TEXT("type"), TEXT("value_definition")})
						if (const auto* Value = Definition->FindValue(Field)) Observe(Value->Get());
					Dependencies = DefinitionReadDependencies.Num() < 4096
						? &DefinitionReadDependencies.Add(Id, MoveTemp(Observed)) : &Observed;
				}
				if (!Dependencies->bKnown) return false;
				Pending.Append(Dependencies->Refs);
			}
			TArray<FString> Ids = Visited.Array(); Ids.Sort();
			FString Material;
			for (const FString& Id : Ids) {
				AppendProof(Material, Id);
				const auto* Content = FindCurrentContentKey(Id);
				if (!Content) return false;
				AppendProof(Material, Content->Key);
			}
			OutKey = Sha256Text(Material);
			if (SchemaKeysByRoots.Num() < 256 && RetainedSchemaRootChars + RootKey.Len() <= 2 * 1024 * 1024) {
				RetainedSchemaRootChars += RootKey.Len();
				if (UnprovenAcceptedUnits) AcceptedSchemaRoots.Add(RootKey);
				SchemaKeysByRoots.Add(MoveTemp(RootKey), OutKey);
			}
			return true;
		};
		// A new process, or a prior export with a different complete layout/binding,
		// has no sufficient in-memory projection. The resolver has just accepted
		// the persisted baseline and its complete inventory. Restore
		// only output groups proven against this current model/layout. This never
		// restores a native observation, compiler token or old file lease.
		int32 RecoveredDiskFiles = 0, RecoveredDiskRows = 0, ValidatedDiskRows = 0;
		bool bAcceptedRowsAllCurrent = false;
		int32 DiskPropertyPointerChecks = 0, DiskProjectionGroups = 0, DiskUnchangedUnsharedUnits = 0, DiskOriginalRowHashReuses = 0;
		FString DiskRejectedUnit, DiskRejectedPath;
		const TCHAR* DiskRejectedField = TEXT("none");
		const TCHAR* DiskRecoveryReason = TEXT("warm_candidate");
		TSet<FString> AcceptedProvenFiles;
		struct FAcceptedMachineRowSpans {
			FAcceptedWorkspaceByteSpan Inventory, Envelope;
			bool bCandidate = false;
		};
		TArray<FAcceptedMachineRowSpans> AcceptedMachineRows;
		TArray<TPair<FIntPoint, FIntPoint>> GeneratedMachineSpans;
		GeneratedMachineSpans.Init(TPair<FIntPoint, FIntPoint>(FIntPoint::ZeroValue, FIntPoint::ZeroValue), ModelUnits.Num());
		TMap<FString, FString> AcceptedMachineFileHashes;
		const FString AcceptedRoot = ExportSettings && !ExportSettings->AcceptedWorkspaceRoot.IsEmpty()
			? ExportSettings->AcceptedWorkspaceRoot : GetAcceptedSemanticWorkspaceRoot();
		const auto* PublishedFileHashes = bDirectPublication ? GetAcceptedPublishedFileHashes(AcceptedRoot) : &AcceptedMachineFileHashes;
		if (bDirectPublication && !AcceptedRoot.IsEmpty() && !PublishedFileHashes) {
			OutError.SetSourceFailure(TEXT("workspace_source_handoff_missing"),
				TEXT("composition.published_bytes: accepted Source did not supply its publication byte table; source.publication_directory must provide it for this read scope."), AcceptedRoot);
			return false;
		}
		const double DiskRecoveryStarted = FPlatformTime::Seconds();
		double DiskInventorySeconds = 0, DiskReadSeconds = 0, DiskProjectionSeconds = 0;
		// Consume the same real producer whose metadata the source just accepted.
		// Model count/layout changes do not invalidate surviving stable-ID rows.
		// The publisher's ownership hashes and later source fence remain separate.
		const auto AcceptedGeneratedBindings = GetAcceptedGeneratedWorkspaceBindings(AcceptedRoot);
		FArtifactPublishError GeneratedOwnerError;
		const bool bAcceptedGeneratedBaseline = bReusableBaseline && Baseline->SourceLayout
			&& (bAcceptedOrigin || AcceptedGeneratedBindings.Get() == static_cast<const FResolvedWorkspaceUnitBindings*>(Baseline.Get()))
			&& (bDirectPublication ? PublishedFileHashes != nullptr
				: TryReadPublishedWorkspaceFileHashes(AcceptedRoot,
					CompositionPublisherOwner(TargetKind, ExportSettings), AcceptedMachineFileHashes, GeneratedOwnerError));
		if (!GeneratedOwnerError.IsEmpty()) { OutError.SetFailure(GeneratedOwnerError); return false; }
		if (bAcceptedGeneratedBaseline) {
			DiskRecoveryReason = TEXT("accepted_generated_owner");
			FUeAgentResourceChargeError OptionalCharge;
			if (Resources.TryChargeMany({{EUeAgentResourceDimension::EstimatedMemoryBytes,
				int64(ModelUnits.Num()) * static_cast<int64>(sizeof(FAcceptedMachineRowSpans))}}, TEXT("composition:accepted_machine_spans"), OptionalCharge)) {
				AcceptedMachineRows.SetNum(ModelUnits.Num());
				const auto& OldModel = Baseline->SourceLayout->GetModel();
				for (int32 Index = 0; Index < ModelUnits.Num(); ++Index) {
					const FString& Id = ModelUnits[Index].GetUnitId();
					if (!IsIdenticalUnit(Id)) continue;
					const int32 OldIndex = OldModel->FindUnitIndex(Id);
					if (!Baseline->GeneratedMachineSpans->IsValidIndex(OldIndex)) continue;
					const auto& Spans = (*Baseline->GeneratedMachineSpans)[OldIndex];
					AcceptedMachineRows[Index] = {{Spans.Key.X, Spans.Key.Y}, {Spans.Value.X, Spans.Value.Y}, true};
				}
			}
		}
		if (!OutputScope && !bAcceptedGeneratedBaseline && (!bReusableBaseline || !bIdenticalLayout || !bIdenticalDefinitionBindings)) {
			DiskRecoveryReason = TEXT("accepted_scope_or_binding");
			const auto Source = GetAcceptedSemanticWorkspace(AcceptedRoot);
			const auto Inventory = GetAcceptedWorkspaceInventory(AcceptedRoot);
			bool bAcceptedScopedBuckets = false;
			if (Inventory) Inventory->Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
				bAcceptedScopedBuckets = Row.EnvelopePath.Equals(MachineEnvelopePath(Row.DocumentPath, true), ESearchCase::CaseSensitive);
				return false; // Inspect one actual row; every row is checked below.
			});
			const auto Hints = GetAcceptedDefinitionProjectionHints(TargetKind);
			const bool bPublicationInputs = HasAcceptedPublicationInputs(AcceptedRoot);
			const FAuthoringModel* AcceptedScope = Source ? &Source->GetModel().Get() : nullptr;
			// Scope validity and whole-projection identity are different contracts.
			// A validated complete after-model can add/remove members without
			// invalidating the accepted rows for every surviving stable identity.
			const TCHAR* ScopeFailure = !AcceptedScope ? TEXT("accepted_source") : !Inventory ? TEXT("accepted_inventory")
				: !AcceptedScope->GetContractId().Equals(Model->GetContractId(), ESearchCase::CaseSensitive) ? TEXT("contract_id")
				: AcceptedScope->GetContractVersion() != Model->GetContractVersion() ? TEXT("contract_version")
				: !AcceptedScope->GetProfileId().Equals(Model->GetProfileId(), ESearchCase::CaseSensitive) ? TEXT("profile")
				: !ExportDataIdentical(AcceptedScope->GetTargetUnitIds(), Model->GetTargetUnitIds()) ? TEXT("target_scope") : nullptr;
			const bool bCommittedAfter = ExportSettings && ExportSettings->ActualExportMode == TEXT("committed");
			const bool bStructuralEdits = Source && !Source->GetStructuralEdits().IsEmpty();
			bool bComplete = !ScopeFailure && (!bStructuralEdits || bCommittedAfter);
			const TCHAR* CompleteFailure = ScopeFailure ? ScopeFailure : bStructuralEdits ? TEXT("structural_edits")
				: Inventory->Num() != ModelUnits.Num() ? TEXT("unit_count") : nullptr;
			bool bWholeProjection = !CompleteFailure;
			TSet<FString> ChangedDefinitionBindings;
			if (!ScopeFailure) for (const auto& Pair : Layout->GetDefinitionProjectionHints().Definitions) {
				const auto* Old = Hints ? Hints->Definitions.Find(Pair.Key) : nullptr;
				const bool bBindingChanged = !Old || !ExportDataIdentical(Old->DefinitionName, Pair.Value.DefinitionName)
					|| !ExportDataIdentical(Old->ReadableAlias, Pair.Value.ReadableAlias)
					|| !ExportDataIdentical(Old->PublicRef, Pair.Value.PublicRef);
				if (bBindingChanged) {
					bWholeProjection = false;
					if (!CompleteFailure) CompleteFailure = TEXT("definition_binding");
				}
				// Exact current/accepted row SHA and complete membership are checked
				// below, including definition content and additions/removals. A second
				// definition-count/content-key prerequisite proves no extra output.
				// Changed names/aliases/refs still invalidate their real consumers.
				if (bBindingChanged) ChangedDefinitionBindings.Add(Pair.Key);
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=accepted_projection_scope first_condition=%s intersection_scope=%s old_units=%d new_units=%d structural_edits=%d committed_after=%s"),
				CompleteFailure ? CompleteFailure : TEXT("none"), bComplete ? TEXT("valid") : ScopeFailure ? ScopeFailure : TEXT("uncommitted_structural_edits"),
				Inventory ? Inventory->Num() : 0, ModelUnits.Num(), Source ? Source->GetStructuralEdits().Num() : 0, bCommittedAfter ? TEXT("true") : TEXT("false"));
			auto Recovered = MakeShared<FCompositionExportBaseline>();
			auto Membership = MakeShared<FMachineBucketMembership>();
			struct FDiskDocumentProjection {
				const FString* Path = nullptr;
				const FWorkspaceFileMembers* Members = nullptr;
				FString EnvelopePath;
				int32 Position = 0;
				int32 BucketIndex = INDEX_NONE;
			};
			TMap<FString, FDiskDocumentProjection> DiskDocuments;
			TMap<FString, int32> DiskBucketSlots;
			TArray<FString> DiskBucketPaths;
			TArray<TArray<int32>> DiskBucketUnits;
			TArray<int32> DiskBucketOrder;
			TSet<FString> DiskChangedUnits, DiskChangedPaths;
			TSet<FString> DiskChangedBuckets;
			TArray<int32> DiskBucketPositions;
			int32 CommonDiskRows = 0, RemovedDiskRows = 0;
			const double DiskInventoryStarted = FPlatformTime::Seconds();
			if (bComplete) {
				FUeAgentResourceChargeError OptionalCharge;
				if (Resources.TryChargeMany({{EUeAgentResourceDimension::EstimatedMemoryBytes,
					int64(ModelUnits.Num()) * static_cast<int64>(sizeof(FAcceptedMachineRowSpans))}}, TEXT("composition:accepted_machine_spans"), OptionalCharge))
					AcceptedMachineRows.SetNum(ModelUnits.Num());
				DiskDocuments.Reserve(Layout->GetPhysicalFiles().Num());
				for (const auto& File : Layout->GetPhysicalFiles()) {
					if (File.Value.IsEmpty()) continue;
					auto& Group = DiskDocuments.Add(File.Key);
					Group.Path = &File.Key; Group.Members = &File.Value;
					Group.EnvelopePath = MachineEnvelopePath(File.Key, bAcceptedScopedBuckets);
					const int32* Slot = DiskBucketSlots.Find(Group.EnvelopePath);
					if (Slot) Group.BucketIndex = *Slot;
					else {
						Group.BucketIndex = DiskBucketPaths.Add(Group.EnvelopePath);
						DiskBucketSlots.Add(Group.EnvelopePath, Group.BucketIndex);
						DiskBucketUnits.AddDefaulted();
					}
					Membership->DocumentBuckets.Add(File.Value.GetDocument()->PhysicalPath, Group.EnvelopePath);
				}
				DiskProjectionGroups = DiskDocuments.Num();
				DiskBucketPositions.Init(0, DiskBucketPaths.Num());
				for (int32 Index = 0; Index < ModelUnits.Num(); ++Index) {
					const auto& Unit = ModelUnits[Index];
					DiskChangedUnits.Add(Unit.GetUnitId());
					const auto& Group = DiskDocuments.FindChecked(Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId()));
					auto& Indices = DiskBucketUnits[Group.BucketIndex];
					if (Indices.IsEmpty()) DiskBucketOrder.Add(Group.BucketIndex);
					Indices.Add(Index);
				}
			}
			if (bComplete) { DiskRecoveryReason = TEXT("inventory_projection"); bComplete = Inventory->Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
				const auto Reject = [&](const TCHAR* Field) {
					if (DiskRejectedUnit.IsEmpty()) { DiskRejectedUnit = Row.UnitId; DiskRejectedPath = Row.DocumentPath; DiskRejectedField = Field; }
					bWholeProjection = false;
					DiskChangedPaths.Add(Row.DocumentPath); DiskChangedBuckets.Add(Row.EnvelopePath);
					return true; // Reject this row's proof, not unrelated accepted rows.
				};
				const auto* Unit = Model->FindUnit(Row.UnitId);
				const auto* Captured = Source && !bPublicationInputs ? Source->FindBaselineUnit(Row.UnitId) : nullptr;
				auto* Group = DiskDocuments.Find(Row.DocumentPath);
				const auto* Logical = Layout->GetLogicalPlacements().Find(Row.UnitId);
				const auto* Public = Layout->GetMachineManifest().PublicRefMapping.Find(Row.UnitId);
				// Compare the complete old and current ordered memberships. A local
				// capture's absence is never used here: Model is the validated full
				// committed overlay (or full capture), Inventory the accepted baseline.
				if (const int32* Slot = DiskBucketSlots.Find(Row.EnvelopePath)) {
					const int32 Position = DiskBucketPositions[*Slot]++;
					const auto& Expected = DiskBucketUnits[*Slot];
					if (!Expected.IsValidIndex(Position) || !ExportDataIdentical(ModelUnits[Expected[Position]].GetUnitId(), Row.UnitId))
						DiskChangedBuckets.Add(Row.EnvelopePath);
				} else DiskChangedBuckets.Add(Row.EnvelopePath);
				if (Group) {
					const int32 Position = Group->Position++;
					if (Position < 0 || Position >= Group->Members->Num() || !ExportDataIdentical((*Group->Members)[Position], Row.UnitId)) {
						DiskChangedPaths.Add(Row.DocumentPath); bWholeProjection = false;
					}
				}
				if (!Unit) { ++RemovedDiskRows; return Reject(TEXT("current_unit")); }
				if (!Captured && !bPublicationInputs) return Reject(TEXT("baseline_unit"));
				++CommonDiskRows;
				if (!ExportDataIdentical(Unit->GetUnitId(), Row.UnitId)) return Reject(TEXT("unit_id"));
				if (Unit->GetUnitKind() != Row.UnitKind) return Reject(TEXT("unit_kind"));
				if (!Group) return Reject(TEXT("physical_group"));
				if (!Logical) return Reject(TEXT("logical_placement"));
				if (!Public) return Reject(TEXT("public_binding"));
				if (!ExportDataIdentical(*Group->Path, Row.DocumentPath)) return Reject(TEXT("document_path"));
				if (!ExportDataIdentical(*Logical, Row.LogicalPath)) return Reject(TEXT("logical_path"));
				if (!ExportDataIdentical(*Public, Row.PublicRef)) return Reject(TEXT("public_ref"));
				if (!Row.ContentFiles.IsEmpty()) return Reject(TEXT("content_files"));
				// This immutable layout group already binds each ordered member to
				// its physical path. Do not look up that same placement, member list,
				// file cursor and machine bucket independently for every property.
				if (!ExportDataIdentical(Group->EnvelopePath, Row.EnvelopePath)) return Reject(TEXT("envelope_path"));
				const int32 UnitIndex = Model->FindUnitIndex(Row.UnitId);
				bool bUnchangedRow = true;
				if (!Captured || !Unit->SharesStorageWith(*Captured)) {
					// Storage identity is only a shortcut. A full current capture can
					// rebuild an unchanged non-property unit in different storage.
					// Keep every declaration and current projection check before
					// accepting its exact immutable semantic-content proof.
					if (Captured) {
						if (!ExportDataIdentical(Unit->GetOwnerRef(), Captured->GetOwnerRef())) return Reject(TEXT("owner_ref"));
						if (!ExportDataIdentical(Unit->GetSemanticTypeRef(), Captured->GetSemanticTypeRef())) return Reject(TEXT("semantic_type"));
						if (!ExportDataIdentical(Unit->GetTargetIdentityKey(), Captured->GetTargetIdentityKey())) return Reject(TEXT("target_identity"));
						if (!ExportDataIdentical(Unit->GetItemRefs(), Captured->GetItemRefs())) return Reject(TEXT("item_refs"));
						if (!ExportDataIdentical(Unit->GetRelationRefs(), Captured->GetRelationRefs())) return Reject(TEXT("relation_refs"));
						if (!ExportDataIdentical(Unit->GetOrderingRef(), Captured->GetOrderingRef())) return Reject(TEXT("ordering_ref"));
						if (!ExportDataIdentical(Unit->GetLayoutRef(), Captured->GetLayoutRef())) return Reject(TEXT("layout_ref"));
						if (!ExportDataIdentical(Unit->GetDependencyRefs(), Captured->GetDependencyRefs())) return Reject(TEXT("dependency_refs"));
						if (!ExportDataIdentical(Unit->GetRole(), Captured->GetRole())) return Reject(TEXT("role"));
						if (!ExportDataIdentical(Unit->GetFieldSetRef(), Captured->GetFieldSetRef())) return Reject(TEXT("field_set"));
						if (!ExportDataIdentical(Unit->GetSourceProvenance(), Captured->GetSourceProvenance())) return Reject(TEXT("source_provenance"));
						if (!ExportDataIdentical(Unit->GetCategoryPath(), Captured->GetCategoryPath())) return Reject(TEXT("category_path"));
						if (!ExportDataIdentical(Unit->GetProfileVisibility(), Captured->GetProfileVisibility())) return Reject(TEXT("profile_visibility"));
						if (!Unit->MatchesCompiledDeclaration(Captured->GetDeclarationProof())) return Reject(TEXT("compiled_declaration"));
					}
					// Compare in the exact public row grammar, which includes all
					// declarations and values. Publication-only input can prove an
					// unchanged row this way without constructing an old value model.
					FString CurrentRowHash;
					if (PlannedInventory.IsValidIndex(UnitIndex)
						&& PlannedInventory[UnitIndex].UnitId.Equals(Unit->GetUnitId(), ESearchCase::CaseSensitive))
						CurrentRowHash = PlannedInventory[UnitIndex].ContentHash;
					else if (!TryHashSemanticUnitRow(*Unit, CurrentRowHash, OutError)) return Reject(TEXT("row_content_proof_error"));
					const bool bUnchangedContent = Row.ContentHashEquals(CurrentRowHash);
					if (bUnchangedContent) ++DiskOriginalRowHashReuses;
					if (bUnchangedContent) {
						++DiskUnchangedUnsharedUnits;
					} else {
						bUnchangedRow = false;
						// Publication-only input owns no old values. A changed row is
						// generated from the real full capture, never inferred from an
						// empty/fake baseline or discarded as an observation mismatch.
						if (!Captured) return Reject(TEXT("changed_publication_row"));
						if (Unit->GetUnitKind() != ESemanticUnitKind::PropertySet
							&& !(TargetKind == TEXT("ue.level.asset") && IsClosedLevelProjectionValueChange(*Unit, *Captured, OutError)))
							return Reject(TEXT("non_property_content"));
						DiskChangedUnits.Add(Row.UnitId); DiskChangedPaths.Add(*Group->Path);
					}
				}
				if (Unit->GetUnitKind() == ESemanticUnitKind::PropertySet
					&& (!Row.bGeneratedProjection || !Captured || !Unit->SharesStorageWith(*Captured))) {
					// A pointer consumes only location. Type/definition were already
					// observed by the current content/binding proof above.
					const FString Pointer = TEXT("/categories/") + EscapeJsonPointerSegment(ReadablePropertyCategory(Unit->GetCategoryPath()))
						+ TEXT("/") + EscapeJsonPointerSegment(ReadablePropertyKey(*Unit));
					++DiskPropertyPointerChecks;
					if (!ExportDataIdentical(Pointer, Row.UnitPointer)) return Reject(TEXT("unit_pointer"));
				}
				Recovered->UnitPointers.Add(Row.UnitId, Row.UnitPointer);
				Recovered->UnitEditHashes.Add(Row.UnitId, Row.ReadEditHash());
				if (bUnchangedRow && AcceptedMachineRows.IsValidIndex(UnitIndex)
					&& Row.InventorySpan.IsValid() && Row.EnvelopeSpan.IsValid())
					AcceptedMachineRows[UnitIndex] = {Row.InventorySpan, Row.EnvelopeSpan, true};
				if (bUnchangedRow && !ChangedDefinitionBindings.Contains(Row.UnitId)) DiskChangedUnits.Remove(Row.UnitId);
				++ValidatedDiskRows;
				return true;
			}); }
			if (OutError.HasErrors()) return false;
			if (bComplete) {
				// Content/declaration proof alone is insufficient when a consumer's
				// schema or declared dependency changed. Propagate only those reads;
				// unrelated newly introduced definitions do not poison every row.
				if (!DiskChangedUnits.IsEmpty()) {
				TMap<FString, TArray<FString>> Consumers;
				for (const auto& Unit : ModelUnits) {
					for (const auto& Id : Unit.GetDependencyRefs()) Consumers.FindOrAdd(Id).Add(Unit.GetUnitId());
					if (FindSemanticFieldSetDeclaration(Unit, *Model)) Consumers.FindOrAdd(Unit.GetFieldSetRef()).Add(Unit.GetUnitId());
				}
				TArray<FString> Pending = DiskChangedUnits.Array();
				while (!Pending.IsEmpty()) {
					const FString Id = Pending.Pop(EAllowShrinking::No);
					if (const auto* Reads = Consumers.Find(Id)) for (const FString& Consumer : *Reads)
						if (!DiskChangedUnits.Contains(Consumer)) { DiskChangedUnits.Add(Consumer); Pending.Add(Consumer); }
				}
				}
				for (const FString& Id : DiskChangedUnits) {
					const FString& Path = Layout->GetPhysicalPlacements().FindChecked(Id);
					DiskChangedPaths.Add(Path); DiskChangedBuckets.Add(Membership->DocumentBuckets.FindChecked(Path));
				}
				for (const auto& Group : DiskDocuments)
					if (Group.Value.Position != Group.Value.Members->Num()) { DiskChangedPaths.Add(Group.Key); bWholeProjection = false; }
				for (int32 Slot = 0; Slot < DiskBucketPaths.Num(); ++Slot)
					if (DiskBucketPositions[Slot] != DiskBucketUnits[Slot].Num()) DiskChangedBuckets.Add(DiskBucketPaths[Slot]);
			}
			if (bComplete) for (int32 Index : DiskBucketOrder)
				Membership->UnitIndices.Add(DiskBucketPaths[Index], MoveTemp(DiskBucketUnits[Index]));
			DiskInventorySeconds = FPlatformTime::Seconds() - DiskInventoryStarted;
			TMap<FString, FString> PublishedHashes;
			FArtifactPublishError OptionalOwnerError;
			if (bComplete) { DiskRecoveryReason = TEXT("published_owner"); bComplete = bDirectPublication ? PublishedFileHashes != nullptr
				: TryReadPublishedWorkspaceFileHashes(AcceptedRoot,
					CompositionPublisherOwner(TargetKind, ExportSettings), PublishedHashes, OptionalOwnerError); }
			if (!OptionalOwnerError.IsEmpty()) { OutError.SetFailure(OptionalOwnerError); return false; }
			TMap<FString, const FExportFilePlan*> PriorPublishedFiles;
			if (Baseline) for (const auto& File : Baseline->Files) PriorPublishedFiles.Add(File.GetRelativePath(), &File);
			auto ReadPublished = [&](const FString& Path) {
				const double ReadStarted = FPlatformTime::Seconds();
				ON_SCOPE_EXIT { DiskReadSeconds += FPlatformTime::Seconds() - ReadStarted; };
				const auto* Published = bDirectPublication ? PublishedFileHashes->Find(Path) : PublishedHashes.Find(Path);
				const auto* Accepted = FindAcceptedSemanticSourceHash(AcceptedRoot, Path);
				if (!Published || !Accepted || !ExportDataIdentical(*Published, *Accepted)) return false;
				if (const auto* Prior = PriorPublishedFiles.Find(Path); Prior && (*Prior)->GetContentHash() == *Published) {
					// Source already observed these exact bytes. Consume the same
					// immutable producer backing, leaving the later source fence intact.
					Recovered->Files.Add(**Prior); ++RecoveredDiskFiles; return true;
				}
				TArray<uint8> Bytes; FString Hash;
				if (!TryReadAcceptedSemanticSourceBytes(AcceptedRoot, Path, Bytes, Hash, OutError)) return false;
				if (!ExportDataIdentical(Hash, *Published)) {
					OutError.SetSourceFailure(TEXT("workspace_source_changed"),
						TEXT("The accepted publication bytes changed during their selected read; output planning stopped without re-rendering them."), Path);
					return false;
				}
				const auto* Prior = PriorPublishedFiles.Find(Path);
				const FExportFilePlan* PriorFile = Prior && (*Prior)->GetContentHash() == Hash ? *Prior : nullptr;
				if (PriorFile) Recovered->Files.Add(*PriorFile);
				else Recovered->Files.Emplace(Path, MoveTemp(Bytes), MoveTemp(Hash));
				// The accepted-byte owner just computed and compared this exact
				// digest. Hand its sealed backing to the existing publisher consumer.
				ArtifactPublisher::RememberVerifiedPlanBytes(Recovered->Files.Last());
				++RecoveredDiskFiles; return true;
			};
			if (bComplete) {
				DiskRecoveryReason = bWholeProjection ? TEXT("complete_projection") : TEXT("intersection_projection");
				for (const auto& File : Layout->GetPhysicalFiles()) {
					if (DiskChangedPaths.Contains(File.Key) || File.Value.IsEmpty()) continue;
					const auto* Unit = Model->FindUnit(File.Value[0]);
					const bool bPacked = Unit->GetUnitKind() == ESemanticUnitKind::PropertySet
						|| (Unit->GetUnitKind() == ESemanticUnitKind::CollectionState && Unit->GetRole() == TEXT("property_definition"));
					const auto Document = bPacked ? FindAcceptedSemanticSourceDocument(AcceptedRoot, File.Key) : nullptr;
					const auto GeneratedCanonical = bPacked && !Document ? FindAcceptedSemanticSourceCanonical(AcceptedRoot, File.Key) : nullptr;
					FString ProvenSchema;
					if (Unit->GetUnitKind() == ESemanticUnitKind::PropertySet
						&& !TryPropertyDocumentSchemaKey(File.Value, ProvenSchema, &DiskChangedUnits)) continue;
					// Definition templates have additional readers. Preserve their
					// existing whole-projection requirement; ordinary properties and
					// standalone documents consume the per-group intersection proof.
					if (!bWholeProjection && Unit->GetUnitKind() == ESemanticUnitKind::CollectionState
						&& Unit->GetRole() == TEXT("property_definition")) continue;
					if (!ReadPublished(File.Key)) { if (OutError.HasErrors()) return false; continue; }
					AcceptedProvenFiles.Add(File.Key);
					if (bPacked && !Document && !GeneratedCanonical) {
						// Current rows, membership and actual accepted bytes already
						// prove this body. Recipe input is not a byte-reuse qualifier.
						if (Unit->GetUnitKind() == ESemanticUnitKind::PropertySet)
							Recovered->PropertyDocumentSchemaKeys.Add(File.Key, MoveTemp(ProvenSchema));
						continue;
					}
					if (bPacked) {
						const double ProjectionStarted = FPlatformTime::Seconds();
						ON_SCOPE_EXIT { DiskProjectionSeconds += FPlatformTime::Seconds() - ProjectionStarted; };
						FUeAgentCanonicalValuePtr Canonical = GeneratedCanonical; TSharedPtr<FJsonValue> Json;
						Contract::FDiagnostic Error;
						if (!Canonical && !TryProjectHjsonCanonicalValue(Document->GetRoot().Get(), Resources, Canonical, OutError, false)) return false;
						if (Unit->GetUnitKind() == ESemanticUnitKind::PropertySet) {
							Recovered->PropertyDocumentSchemaKeys.Add(File.Key, MoveTemp(ProvenSchema));
							Recovered->PackedExamples.Add(File.Key, Canonical.ToSharedRef());
						}
						else {
							// Definition catalogs still have a JSON consumer. Ordinary
							// unchanged properties only need canonical recipe input and
							// their already-verified file bytes, never a second JSON tree.
							if (!CanonicalJson::TryToJsonValue(*Canonical, Json, Error) || !Json || !Json->AsObject()) {
								OutError.SetFailure(Error, File.Key); return false;
							}
							Recovered->PackedDocuments.Add(File.Key, Json->AsObject().ToSharedRef());
						}
					}
				}
				for (const auto& Bucket : Membership->UnitIndices) {
					bool bChanged = DiskChangedBuckets.Contains(Bucket.Key);
					for (int32 Index : Bucket.Value) if (DiskChangedUnits.Contains(ModelUnits[Index].GetUnitId())) { bChanged = true; break; }
					if (bChanged) continue;
					const FString InventoryPath = Bucket.Key.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
					if (ReadPublished(Bucket.Key) && ReadPublished(InventoryPath)) RecoveredDiskRows += Bucket.Value.Num();
					if (OutError.HasErrors()) return false;
				}
				Recovered->ContractId = Model->GetContractId(); Recovered->ContractVersion = Model->GetContractVersion();
				Recovered->ProfileId = Model->GetProfileId(); Recovered->TargetUnitIds = Model->GetTargetUnitIds();
				Recovered->TargetKind = TargetKind; Recovered->LayoutVersion = Layout->GetLayoutVersion();
				Recovered->Projection = Layout->GetSharedProjection(); Recovered->PublicRefMapping = Layout->GetMachineManifest().PublicRefMapping;
				Recovered->DefinitionProjectionHints = MakeShared<const FDefinitionProjectionHints>(Layout->GetDefinitionProjectionHints());
				// This compatibility reader really needs a complete independently
				// invalidatable table. Materialize it here, not in the ordinary path.
				if (ContentKeyBaseline) Recovered->UnitContentKeys = ContentKeyBaseline->UnitContentKeys;
				for (const auto& Pair : UnitContentKeys) Recovered->UnitContentKeys.Add(Pair.Key, Pair.Value);
				for (auto It = Recovered->UnitContentKeys.CreateIterator(); It; ++It)
					if (!Model->FindDeclaration(It.Key())) It.RemoveCurrent();
				for (const auto& Id : DiskChangedUnits)
					if (auto* Key = Recovered->UnitContentKeys.Find(Id)) Key->Key.Reset();
				Recovered->MachineBuckets = MoveTemp(Membership);
				Recovered->bScopedMachineBuckets = bAcceptedScopedBuckets;
				if (Baseline && Baseline->ContractId == Recovered->ContractId && Baseline->ContractVersion == Recovered->ContractVersion
					&& Baseline->ProfileId == Recovered->ProfileId && Baseline->TargetUnitIds == Recovered->TargetUnitIds)
					Recovered->ExamplePublication = Baseline->ExamplePublication;
				Baseline = MoveTemp(Recovered);
				bReusableBaseline = true;
				bIdenticalLayout = bIdenticalDefinitionBindings = bWholeProjection;
				bRetainedDefinitionBindings = bWholeProjection;
				if (!bDirectPublication) AcceptedMachineFileHashes = MoveTemp(PublishedHashes);
				ChangedUnitIds = MoveTemp(DiskChangedUnits);
				bAcceptedRowsAllCurrent = bPublicationInputs && bWholeProjection && ChangedUnitIds.IsEmpty()
					&& ValidatedDiskRows == ModelUnits.Num();
				ChangedPropertyUnits = ChangedOtherUnits = ChangedProjectionValueUnits = 0;
				for (const FString& Id : ChangedUnitIds) {
					if (Model->FindDeclaration(Id)->GetUnitKind() == ESemanticUnitKind::PropertySet) ++ChangedPropertyUnits;
					else { ++ChangedOtherUnits; ++ChangedProjectionValueUnits; }
				}
				// These observations belonged to the replaced in-memory baseline.
				// The accepted disk comparison above proved complete same bindings
				// and closed property/keyed-projection value changes; do not retain the other asset's
				// invalidation set and disable the recovered container consumers.
				ChangedValueItems = 0;
				ChangedProjectionUnits.Reset();
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=accepted_projection_intersection common_rows=%d proven_rows=%d added_rows=%d removed_rows=%d changed_or_unproven_rows=%d proven_files=%d invalidated_buckets=%d whole_projection=%s"),
					CommonDiskRows, ModelUnits.Num() - ChangedUnitIds.Num(), ModelUnits.Num() - CommonDiskRows, RemovedDiskRows,
					ChangedUnitIds.Num(), AcceptedProvenFiles.Num(), DiskChangedBuckets.Num(), bWholeProjection ? TEXT("true") : TEXT("false"));
			}
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=accepted_disk_projection files=%d machine_rows=%d validated_rows=%d reason=%s total_s=%.6f"),
			RecoveredDiskFiles, RecoveredDiskRows, ValidatedDiskRows, DiskRecoveryReason, FPlatformTime::Seconds() - DiskRecoveryStarted);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=accepted_disk_projection_parts inventory_s=%.6f read_s=%.6f projection_s=%.6f property_pointer_checks=%d pointer_descriptor_queries=0 physical_projection_groups=%d unchanged_unshared_units=%d original_row_hash_reuses=%d"),
			DiskInventorySeconds, DiskReadSeconds, DiskProjectionSeconds, DiskPropertyPointerChecks, DiskProjectionGroups, DiskUnchangedUnsharedUnits, DiskOriginalRowHashReuses);
		if (!DiskRejectedUnit.IsEmpty())
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=accepted_disk_projection_rejected row=%d unit=%s path=%s field=%s"),
				ValidatedDiskRows, *DiskRejectedUnit, *DiskRejectedPath, DiskRejectedField);
		TMap<FString, const FExportFilePlan*> BaselineFiles;
		if (bReusableBaseline)
			for (const FExportFilePlan& File : Baseline->Files) BaselineFiles.Add(File.GetRelativePath(), &File);
		TArray<TSharedRef<FExportFilePlan>> AcceptedFileBacking;
		int32 AcceptedFileReads = 0;
		int64 AcceptedFileReadBytes = 0;
		int32 AcceptedGeneratedByteReuses = 0;
		auto FindPublishedFile = [&](const FString& Path) -> const FExportFilePlan* {
			if (const auto* File = BaselineFiles.Find(Path)) return *File;
			// A legacy recovered baseline has current integer memberships but may
			// have rejected an old bucket after deletion. Its hash alone cannot
			// prove that bucket's current members. Keep its original fallback;
			// only explicitly proven public groups or the real generated owner
			// may recover an evicted byte backing here.
			if (!bAcceptedGeneratedBaseline && !AcceptedProvenFiles.Contains(Path)) return nullptr;
			const auto* Published = PublishedFileHashes ? PublishedFileHashes->Find(Path) : nullptr;
			const auto* Accepted = FindAcceptedSemanticSourceHash(AcceptedRoot, Path);
			if (!Published || !Accepted || !ExportDataIdentical(*Published, *Accepted)) return nullptr;
			TArray<uint8> Bytes; FString Hash;
			FWorkspaceError OptionalReadError;
			if (const auto GeneratedBytes = GetAcceptedGeneratedInventoryBytes(AcceptedRoot, Path)) {
				Bytes = *GeneratedBytes; Hash = *Published; ++AcceptedGeneratedByteReuses;
			} else {
				if (!TryReadAcceptedSemanticSourceBytes(AcceptedRoot, Path, Bytes, Hash, OptionalReadError)) {
					OutError = MoveTemp(OptionalReadError); return nullptr;
				}
				if (!ExportDataIdentical(Hash, *Published)) {
					OutError.SetSourceFailure(TEXT("workspace_source_changed"),
						TEXT("The accepted file bytes no longer match the published file; the selected source read stopped without re-rendering them."), Path);
					return nullptr;
				}
				++AcceptedFileReads; AcceptedFileReadBytes += Bytes.Num();
			}
			// Both paths supply the corresponding accepted producer bytes. An
			// optional memory miss does not require formatting the same file.
			const auto File = MakeShared<FExportFilePlan>(Path, MoveTemp(Bytes), MoveTemp(Hash));
			ArtifactPublisher::RememberVerifiedPlanBytes(*File);
			AcceptedFileBacking.Add(File); BaselineFiles.Add(Path, &File.Get());
			return &File.Get();
		};
		const double ComparisonFinished = FPlatformTime::Seconds();
		int32 ReusedDocuments = 0;
		int32 ReusedInventoryRows = 0;
		TArray<FExportFilePlan> Files;
		FAuthoringExampleCatalog Examples;
		RegisterDomainAuthoringExampleRules(Examples);
		TExportRows<FUeAgentCanonicalValueRef> InventoryByUnit;
		TExportRows<FString> UnitPointers;
		TExportRows<FString> UnitEditHashes;
		// The retained projection token is an inexpensive proof, not a required
		// identity. bIdenticalLayout also proves every logical/physical placement,
		// ordered file membership and public reference when a new token was built.
		const bool bSharedRowContainers = bIdenticalLayout && bIdenticalDefinitionBindings
			&& ChangedOtherUnits == ChangedProjectionValueUnits;
		const bool bInheritedRowContainers = bReusableBaseline;
		int64 RetirementRowChecks = 0;
		if (bInheritedRowContainers) {
			InventoryByUnit = Baseline->Inventory; UnitPointers = Baseline->UnitPointers; UnitEditHashes = Baseline->UnitEditHashes;
			// The accepted full comparison just consumed every current row's exact
			// content, name and membership. With no changed/retired row, scanning
			// these same tables three more times proves no additional result.
			if (!bAcceptedRowsAllCurrent) {
				auto Retired = [&](const FString& Id) { ++RetirementRowChecks; return !Model->FindDeclaration(Id) || !IsIdenticalUnit(Id); };
				InventoryByUnit.RemoveIf(Retired); UnitPointers.RemoveIf(Retired); UnitEditHashes.RemoveIf(Retired);
				}
		}
		// This set comes from complete current semantic/projection comparison,
		// never the caller's edited IDs. The retained projection proves every
		// file's complete ordered membership, including the absence of moves.
		TSet<FString> ChangedPhysicalPaths;
		// A native setter/notification may normalize an accepted author input
		// back to its published value. That is no actual delta, but its author
		// node must still receive the observed value. These paths control only
		// body production; they never enter ChangedUnitIds or fabricate actuals.
		if (ActualChanges && AcceptedSource && AcceptedSource->GetEditedUnitIds().IsSet())
			for (const auto& Id : *AcceptedSource->GetEditedUnitIds())
				if (const FString* Path = Layout->GetPhysicalPlacements().Find(Id)) ChangedPhysicalPaths.Add(*Path);
		if (bInheritedRowContainers) {
			for (const FString& UnitId : ChangedUnitIds)
				if (const FString* Path = Layout->GetPhysicalPlacements().Find(UnitId)) ChangedPhysicalPaths.Add(*Path);
			for (const FString& Id : ChangedProjectionUnits)
				if (const FString* Path = Layout->GetPhysicalPlacements().Find(Id)) ChangedPhysicalPaths.Add(*Path);
			for (const auto& Before : Baseline->Projection->PhysicalPlacements) {
				const FString* Now = Layout->GetPhysicalPlacements().Find(Before.Key);
				if (!Now || !ExportDataIdentical(*Now, Before.Value)) {
					ChangedPhysicalPaths.Add(Before.Value);
					if (Now) ChangedPhysicalPaths.Add(*Now);
				}
			}
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_row_containers shared=%s inherited=%s retained_rows=%d changed_units=%d retirement_row_checks=%lld accepted_all_current=%s"),
			bSharedRowContainers ? TEXT("true") : TEXT("false"), bInheritedRowContainers ? TEXT("true") : TEXT("false"), InventoryByUnit.Num(), ChangedUnitIds.Num(),
			RetirementRowChecks, bAcceptedRowsAllCurrent ? TEXT("true") : TEXT("false"));
		TMap<FString, TArray<FString>> UnitContentFiles;
		auto ReuseSingleUnitDocument = [&](const FString& Path, const FWorkspaceFileMembers& Members) {
			// These callers read only the complete unit and the projection mapping.
			// Keyed collections/graphs have additional reads and keep their own path.
			const auto* PreviousMembers = bReusableBaseline ? Baseline->Projection->PhysicalFiles.Find(Path) : nullptr;
			const bool bSameMembers = PreviousMembers && ExportDataIdentical(*PreviousMembers, Members);
			if ((!bIdenticalLayout && !AcceptedProvenFiles.Contains(Path) && !bSameMembers) || Members.Num() != 1 || !IsIdenticalUnit(Members[0])
				|| Baseline->UnitContentFiles.Contains(Members[0])) return false;
			const auto* File = FindPublishedFile(Path);
			if (OutError.HasErrors()) return false;
			const auto* Pointer = Baseline->UnitPointers.Find(Members[0]);
			const auto* EditHash = Baseline->UnitEditHashes.Find(Members[0]);
			if (!File || !Pointer || !EditHash) return false;
			Files.Add(*File);
			if (!bInheritedRowContainers) { UnitPointers.Add(Members[0], *Pointer); UnitEditHashes.Add(Members[0], *EditHash); }
			// The same complete document proof already established the pointer,
			// edit hash and absence of external content for this inventory row.
			if (!bInheritedRowContainers) if (const auto* Row = Baseline->Inventory.Find(Members[0])) InventoryByUnit.Add(Members[0], *Row);
			++ReusedDocuments;
			return true;
		};
		TSet<FString> ContentPaths;
		if (bInheritedRowContainers) for (const auto& Content : Baseline->UnitContentFiles) {
			const auto* Path = Layout->GetPhysicalPlacements().Find(Content.Key);
			if (!Path || ChangedPhysicalPaths.Contains(*Path)) continue;
			UnitContentFiles.Add(Content.Key, Content.Value);
			for (const auto& FilePath : Content.Value) ContentPaths.Add(FilePath.ToLower());
		}
		TMap<FString, FString> ContentDirectories;
		bool bContentDirectoriesBuilt = false;
		auto OrganizeContent = [&](const FString& UnitId, const FString& DocumentPath,
			FUeAgentCanonicalValuePtr& Row) -> bool
		{
			if (DocumentPath.StartsWith(TEXT(".uai/"))) return true;
			const FSemanticUnit* Unit = Model->FindUnit(UnitId);
			if (!Unit) return true;
			return TryOrganizeAuthoringContent(Unit->GetSemanticTypeRef(), Unit->GetRole(), Row,
				[&](const FUeAgentCanonicalValuePtr& Value, const FString& RelativeStem,
					FUeAgentCanonicalValuePtr& OutReference) -> bool
				{
					// Layout already validates the full owner/directory relation.
					// Ordinary scalar rows allocate no external content, so they do
					// not need a second complete directory map in composition.
					if (!bContentDirectoriesBuilt) {
						if (!TryBuildAuthoringContentDirectories(*Model, Layout->GetPhysicalPlacements(), ContentDirectories, OutError,
							&Layout->GetPhysicalPlacements())) return false;
						bContentDirectoriesBuilt = true;
					}
					const FString& Base = ContentDirectories.FindChecked(UnitId);
					const FString Stem = Base + TEXT("/") + RelativeStem;
					FString Path = AllocateReadableName(Stem, [&](const FString& Candidate) {
						const FString File = Candidate + TEXT(".hjson");
						if (ContentPaths.Contains(File.ToLower())) return true;
						if (OutputScope && OutputScope->PublishedFileHashes) for (const auto& Existing : *OutputScope->PublishedFileHashes)
							if (Existing.Key.Equals(File, ESearchCase::IgnoreCase) && !OutputScope->ReplacedContentPaths.Contains(Existing.Key)) return true;
						return false;
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
		TArray<const FString*> PackedDocumentOrder;
		TSet<FString> ReusedPackedDocuments;
		int32 ProvenFileMemberRows = 0;
		int32 ScopedDefinitionFileReuses = 0;
		TMap<FString, FString> PropertyDocumentSchemaKeys;
		TMap<FString, FString> PropertyPathsByItemRef;
		auto AddPropertyGuideMetadata = [&](const FString& Path, const FWorkspaceFileMembers& Members) {
			TArray<FAuthoringExampleCatalog::FPropertyInstruction> Properties;
			for (const FString& Id : Members) {
				const auto Declaration = Model->FindDeclaration(Id); if (!Declaration) return false;
				Properties.Add(DescribePropertyGuide(*Declaration, *Model));
			}
			Examples.AddPropertyDocument(Path, MoveTemp(Properties));
			return Model->CheckValueReadResult(OutError);
		};
		auto AddPropertyContentGuideMetadata = [&](const FExportFilePlan& File) {
			FUeAgentCanonicalValuePtr Metadata;
			if (!TryParsePropertyGuideMetadata(File.GetRelativePath(), File.GetCanonicalUtf8(), Resources, Metadata, OutError)) return false;
			Examples.AddDocument(File.GetRelativePath(), Metadata.ToSharedRef());
			return true;
		};
		bool bPropertyPathsBuilt = false;
		auto BuildPropertyPaths = [&]() {
		if (bPropertyPathsBuilt) return;
		bPropertyPathsBuilt = true;
		for (const auto& Property : Model->GetDeclarations())
		{
			if (Property.GetUnitKind() != ESemanticUnitKind::PropertySet) continue;
			// Layout-slot properties belong to the widget's placement, not its body.
			if (Property.GetSemanticTypeRef() == TEXT("uai.semantic.umg.slot.property")) continue;
			const auto Owner = Model->FindDeclaration(Property.GetOwnerRef());
			const FString* Path = Layout->GetPhysicalPlacements().Find(Property.GetUnitId());
			if (!Owner || Owner->GetUnitKind() != ESemanticUnitKind::ItemState
				|| !Path || !Path->EndsWith(TEXT("/properties.hjson"))) continue;
			PropertyPathsByItemRef.Add(Owner->GetUnitId(), *Path);
			if (const FString* PublicRef = Layout->GetMachineManifest().PublicRefMapping.Find(Owner->GetUnitId()))
				PropertyPathsByItemRef.Add(*PublicRef, *Path);
		}
		};
		for (const auto& PhysicalFile
			: Layout->GetPhysicalFiles())
		{
			if (OutputScope) {
				bool bWritten = false;
				for (const auto& Id : PhysicalFile.Value) bWritten |= OutputScope->Rows.Contains(Id) || OutputScope->GuideDefinitions.Contains(Id);
				if (!bWritten) continue;
			}
			if (PhysicalFile.Key == ManifestPath
				|| PhysicalFile.Value.IsEmpty())
			{
				continue;
			}
			const auto FirstDeclaration = Model->FindDeclaration(PhysicalFile.Value[0]);
			const bool bPackedPropertyContainer = FirstDeclaration
				&& FirstDeclaration->GetUnitKind() == ESemanticUnitKind::PropertySet;
			const bool bPackedDefinitionContainer = FirstDeclaration
				&& FirstDeclaration->GetUnitKind() == ESemanticUnitKind::CollectionState
				&& FirstDeclaration->GetRole() == TEXT("property_definition");
			const bool bGraphStructureDocument =
				PhysicalFile.Value.Num() == 1
				&& FirstDeclaration && IsGraphStructureUnit(*FirstDeclaration);
			// Definition templates may read units outside this physical document.
			// Keep their previous conservative fallback for other-unit changes;
			// ordinary property documents depend on their complete member rows.
			const FWorkspaceFileMembers* PreviousMembers = bReusableBaseline
				? Baseline->Projection->PhysicalFiles.Find(PhysicalFile.Key) : nullptr;
			const bool bSameDocumentMembers = PreviousMembers && ExportDataIdentical(*PreviousMembers, PhysicalFile.Value);
			// The accepted actual/declaration closure above already includes every
			// changed definition binding and its readers. Retiring an unrelated
			// definition must not invalidate this document's unchanged producer bytes.
			// Without that same-publication actual, retain the legacy catalog proof.
			const bool bProvenUnchangedFile = bInheritedRowContainers && bSameDocumentMembers
				&& !ChangedPhysicalPaths.Contains(PhysicalFile.Key)
				&& (MemoActualChanges != nullptr || bRetainedDefinitionBindings);
			const FString* OldDocumentSchemaKey = bReusableBaseline
				? Baseline->PropertyDocumentSchemaKeys.Find(PhysicalFile.Key) : nullptr;
			if (bPackedPropertyContainer && bProvenUnchangedFile) {
				const auto* File = FindPublishedFile(PhysicalFile.Key);
				if (OutError.HasErrors()) return false;
				if (File) {
					bool bHasContent = false;
					for (const auto& Id : PhysicalFile.Value) bHasContent |= UnitContentFiles.Contains(Id);
					if (bHasContent ? !AddPropertyContentGuideMetadata(*File) : !AddPropertyGuideMetadata(PhysicalFile.Key, PhysicalFile.Value)) return false;
					if (bHasContent) for (const auto& Id : PhysicalFile.Value) if (const auto* Content = UnitContentFiles.Find(Id))
						for (const auto& ContentPath : *Content) {
							const auto* ContentFile = FindPublishedFile(ContentPath);
							if (!ContentFile || !AddPropertyContentGuideMetadata(*ContentFile)) return false;
							Files.Add(*ContentFile);
						}
					if (OldDocumentSchemaKey) PropertyDocumentSchemaKeys.Add(PhysicalFile.Key, *OldDocumentSchemaKey);
					Files.Add(*File); ReusedPackedDocuments.Add(PhysicalFile.Key);
					if (!bRetainedDefinitionBindings) ++ScopedDefinitionFileReuses;
					++ReusedDocuments;
					continue;
				}
			}
			FString DocumentSchemaKey;
			const bool bKnownDocumentSchema = bPackedPropertyContainer
				&& ((bProvenUnchangedFile && OldDocumentSchemaKey)
					? (DocumentSchemaKey = *OldDocumentSchemaKey, true)
					: TryPropertyDocumentSchemaKey(PhysicalFile.Value, DocumentSchemaKey));
			if (bKnownDocumentSchema) PropertyDocumentSchemaKeys.Add(PhysicalFile.Key, DocumentSchemaKey);
			const bool bReusableDocumentSchema = bKnownDocumentSchema && OldDocumentSchemaKey
				&& ExportDataIdentical(DocumentSchemaKey, *OldDocumentSchemaKey);
			const bool bReusablePackedKind = (bPackedPropertyContainer && bReusableDocumentSchema)
				|| (bPackedDefinitionContainer && bIdenticalLayout && ChangedOtherUnits == ChangedProjectionValueUnits);
			if (bReusableBaseline && bReusablePackedKind && PreviousMembers
				&& bSameDocumentMembers
				&& (Baseline->PackedDocuments.Contains(PhysicalFile.Key)
					|| (bPackedPropertyContainer && Baseline->PackedExamples.Contains(PhysicalFile.Key))))
			{
				bool bAllMembersIdentical = true;
				for (const FString& UnitId : PhysicalFile.Value)
				{
					// External content allocation also contributes examples and can
					// depend on preceding path collisions. Keep that path authoritative.
					if ((!bProvenUnchangedFile && !IsIdenticalUnit(UnitId)) || Baseline->UnitContentFiles.Contains(UnitId)
						|| !Baseline->UnitPointers.Contains(UnitId) || !Baseline->UnitEditHashes.Contains(UnitId))
					{ bAllMembersIdentical = false; break; }
				}
				const auto* IdenticalFile = bAllMembersIdentical ? FindPublishedFile(PhysicalFile.Key) : nullptr;
				if (OutError.HasErrors()) return false;
				if (IdenticalFile)
				{
					if (bProvenUnchangedFile) ProvenFileMemberRows += PhysicalFile.Value.Num();
					if (const auto* Document = Baseline->PackedDocuments.Find(PhysicalFile.Key))
						PackedDocuments.Add(PhysicalFile.Key, *Document);
					else PackedExamples.Add(PhysicalFile.Key, Baseline->PackedExamples.FindChecked(PhysicalFile.Key));
					PackedDocumentOrder.Add(&PhysicalFile.Key);
					ReusedPackedDocuments.Add(PhysicalFile.Key);
					if (!bInheritedRowContainers) for (const FString& UnitId : PhysicalFile.Value)
					{
						UnitPointers.Add(UnitId, Baseline->UnitPointers.FindChecked(UnitId));
						UnitEditHashes.Add(UnitId, Baseline->UnitEditHashes.FindChecked(UnitId));
						if (const auto* Row = Baseline->Inventory.Find(UnitId)) InventoryByUnit.Add(UnitId, *Row);
					}
					++ReusedDocuments;
					continue;
				}
			}
			const bool bKeyedItemDocument =
				PhysicalFile.Value.Num() == 1
				&& FirstDeclaration && IsKeyedItemStructureUnit(*FirstDeclaration);
			if (bKeyedItemDocument)
			{
				// A leaf collection guide consumes its declared contract, not its
				// members. Nested/schema-bearing collections retain that real reader.
				const auto Contracts = DocumentCollectionContracts(*FirstDeclaration, TargetKind);
				if (!Contracts.IsEmpty() && !CollectionGuidesNeedContent(Contracts)
					&& ReuseSingleUnitDocument(PhysicalFile.Key, PhysicalFile.Value)) {
					FUeAgentCanonicalValuePtr Metadata;
					if (!BuildLeafCollectionGuideDocument(Contracts, Resources, Metadata, OutError, PhysicalFile.Key)) return false;
					Examples.AddDocument(PhysicalFile.Key, Metadata.ToSharedRef(), FirstDeclaration->GetSemanticTypeRef(), FirstDeclaration->GetRole());
					for (const auto* Contract : Contracts) Examples.AddCollection(PhysicalFile.Key, Metadata.ToSharedRef(), *Contract);
					continue;
				}
				if (OutError.HasErrors()) return false;
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
					const auto* CapturedItems = Unit->FindValue(Contract.RootField);
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
						TMap<FString, FUeAgentCanonicalValueRef> CapturedByKey;
						for (const auto& CapturedItem : (*CapturedItems)->GetObjectItems())
							CapturedByKey.Add(CapturedItem.Key, CapturedItem.Value);
						for (const auto& Item : VisibleItems->GetObjectItems())
						{
							TArray<TPair<FString, FUeAgentCanonicalValueRef>> RowFields = Item.Value->GetObjectItems();
							if (const auto* Captured = CapturedByKey.Find(Item.Key))
							{
								FString FieldSet;
								if (!TryGetAggregateMemberFieldSet(*Model, *Captured, FieldSet, OutError)) return false;
								if (!FieldSet.IsEmpty())
								{
									const FString* Ref = Layout->GetMachineManifest().PublicRefMapping.Find(FieldSet);
									if (!Ref) {
										OutError.SetSourceFailure(TEXT("unresolved_semantic_unit_reference"),
											TEXT("The bound collection member's declared field set has no public name."), FieldSet);
										return false;
									}
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
				Unit->VisitValues([&](const FString& Key, const FUeAgentCanonicalValueRef& Value) {
					if (ProjectedRoots.Contains(Key)) return true;
					for (const auto& Contract : GetKeyedItemProjectionContracts())
						if (ProjectedRoots.Contains(Contract.RootField)
							&& (Contract.IsReadOnlyRootField(Key) || Contract.IsEditableRootField(Key)
								|| (!Contract.RecordSchemaField.IsEmpty() && Key.Equals(Contract.RecordSchemaField, ESearchCase::CaseSensitive))))
						{
								Fields.Emplace(Key, Value);
							break;
						}
					return true;
				});
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
				const FString EditHash = Sha256(FileBytes);
				UnitEditHashes.Add(UnitId, EditHash);
				const FUeAgentCanonicalValue* UnorganizedRow = VisibleRow.Get();
				if (!OrganizeContent(UnitId, PhysicalFile.Key, VisibleRow)) return false;
				// Canonical values are immutable. Content organization replaces the
				// root only when it externalizes content; otherwise these are already
				// the exact final bytes as well as the edit-hash input.
				if (VisibleRow.Get() != UnorganizedRow
					&& !TrySerializeSemanticHjsonObject(*VisibleRow, FileBytes, OutError, PhysicalFile.Key))
				{ return false; }
				Examples.AddDocument(PhysicalFile.Key, VisibleRow.ToSharedRef(), Unit->GetSemanticTypeRef(), Unit->GetRole());
				if (OutputScope) OutputScope->Documents.Add(PhysicalFile.Key, VisibleRow.ToSharedRef());
				for (const auto* Contract : Contracts) Examples.AddCollection(PhysicalFile.Key, VisibleRow.ToSharedRef(), *Contract);
				Files.Emplace(PhysicalFile.Key, FileBytes, VisibleRow.Get() == UnorganizedRow ? EditHash : Sha256(FileBytes));
				UnitPointers.Add(UnitId, TEXT("/"));
			}
			else if (PhysicalFile.Value.Num() == 1
				&& FirstDeclaration && IsActorComponentHierarchyItemUnit(*FirstDeclaration))
			{
				if (ReuseSingleUnitDocument(PhysicalFile.Key, PhysicalFile.Value)) continue;
				if (OutError.HasErrors()) return false;
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
				const FString FileHash = Sha256(FileBytes);
				Files.Emplace(PhysicalFile.Key, FileBytes, FileHash);
				UnitPointers.Add(UnitId, TEXT("/"));
				UnitEditHashes.Add(UnitId, FileHash);
				if (OutputScope) {
					FUeAgentCanonicalValuePtr Canonical;
					if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(VisibleRow), Resources, Canonical, OutError)) return false;
					OutputScope->Documents.Add(PhysicalFile.Key, Canonical.ToSharedRef());
				}
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
					BuildPropertyPaths();
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
					const FString InterfacePath = GraphInterfaceDocumentPath(PhysicalFile.Key);
					VisibleRow->SetStringField(TEXT("interface_ref"), FPaths::GetCleanFilename(InterfacePath));
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
				if (OutputScope) OutputScope->Documents.Add(PhysicalFile.Key, ExampleDocument.ToSharedRef());
				const FString FileHash = Sha256(FileBytes);
				Files.Emplace(PhysicalFile.Key, FileBytes, FileHash);
				UnitPointers.Add(UnitId, TEXT("/"));
				UnitEditHashes.Add(UnitId, FileHash);
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
					if (OutputScope && !OutputScope->Rows.Contains(UnitId) && !OutputScope->GuideDefinitions.Contains(UnitId)) continue;
					if (OutputScope && bPackedDefinitionContainer) ++OutputScope->RenderedDefinitionRows;
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
					// This carrier is not an output document. Its typed projection
					// below and the final file's original formatter validate the actual
					// emitted fields; do not render and discard the intermediate row.
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
						if (!TryBuildPublishedPropertyRow(*Unit, *Model, Address, CompactRow, VisibleRow, OutError)) return false;
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
											if (const auto* Captured = Unit->FindValue(TEXT("value_definition")))
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
				PackedDocumentOrder.Add(&PhysicalFile.Key);
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
				if (ReuseSingleUnitDocument(PhysicalFile.Key, PhysicalFile.Value)) continue;
				if (OutError.HasErrors()) return false;
				TSharedPtr<FJsonObject> Row;
				if (!TryBuildSemanticModelUnitRow(*Unit, Row, OutError, Layout->GetMachineManifest().PublicRefMapping, PhysicalFile.Key)) return false;
				const auto Wrapper = MakeShared<FJsonObject>(); Wrapper->SetObjectField(TEXT("unit"), Row);
				TArray<uint8> FileBytes;
				if (!TrySerializeSemanticHjsonObject(Wrapper, FileBytes, OutError, PhysicalFile.Key))
				{
					return false;
				}
				Files.Emplace(
					PhysicalFile.Key,
					FileBytes,
					Sha256(FileBytes));
				UnitPointers.Add(UnitId, TEXT("/unit"));
				TArray<uint8> RowBytes;
				if (!TrySerializeSemanticHjsonObject(Row.ToSharedRef(), RowBytes, OutError, PhysicalFile.Key))
				{
					
					return false;
				}
				UnitEditHashes.Add(UnitId, Sha256(RowBytes));
				if (OutputScope) {
					FUeAgentCanonicalValuePtr Canonical;
					if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Wrapper), Resources, Canonical, OutError)) return false;
					OutputScope->Documents.Add(PhysicalFile.Key, Canonical.ToSharedRef());
				}
			}
		}
		for (const FString* PackedPathPtr : PackedDocumentOrder)
		{
			const FString& PackedPath = *PackedPathPtr;
			const auto* PackedJson = PackedDocuments.Find(PackedPath);
			if (!PackedJson) {
				// Complete unchanged property groups use their canonical recipe
				// input directly, retaining the original physical-document order.
				Examples.AddDocument(PackedPath, PackedExamples.FindChecked(PackedPath));
				Files.Add(*BaselineFiles.FindChecked(PackedPath));
				continue;
			}
			const struct { const FString& Key; const TSharedRef<FJsonObject>& Value; } Packed{PackedPath, *PackedJson};
			if (OutputScope) {
				FUeAgentCanonicalValuePtr Canonical;
				if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Packed.Value), Resources, Canonical, OutError)) return false;
				OutputScope->Documents.Add(Packed.Key, Canonical.ToSharedRef());
			}
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
			if (OutputScope) {
				bool bWritten = false;
				for (const auto& Id : Layout->GetPhysicalFiles().FindChecked(Packed.Key)) bWritten |= OutputScope->Rows.Contains(Id);
				if (!bWritten) continue; // Definition input for K guides only.
				if (OutputScope->PublishedFileHashes && OutputScope->PublishedFileHashes->Contains(Packed.Key))
					continue; // The canonical K rows go directly into the old document.
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
		if (OutputScope) {
			// The original formatter has produced the actual K rows and their
			// canonical documents. Do not build a partial full-workspace header,
			// N membership/index, machine groups or a complete-layout cache.
			for (const auto& Id : OutputScope->Rows) {
				const auto* Unit = Model->FindUnit(Id);
				const auto* Pointer = UnitPointers.Find(Id);
				const auto* EditHash = UnitEditHashes.Find(Id);
				if (!Unit || !Pointer || !EditHash) { Model->CheckValueReadResult(OutError); return false; }
				FString RowHash;
				if (!TryHashSemanticUnitRow(*Unit, RowHash, OutError)) return false;
				FUeAgentCanonicalValuePtr Row; const TArray<FString> EmptyContent;
				const auto* Content = UnitContentFiles.Find(Id);
				if (!TryBuildPublishedMachineRow(*Unit, *Model, Layout->GetMachineManifest().PublicRefMapping.FindChecked(Id),
					Layout->GetLogicalPlacements().FindChecked(Id), Layout->GetPhysicalPlacements().FindChecked(Id),
					*Pointer, Content ? *Content : EmptyContent, RowHash, *EditHash, Resources, Row, OutError)) return false;
				OutputScope->MachineRows.Add(Id, Row.ToSharedRef()); OutputScope->Pointers.Add(Id, *Pointer);
				const FString& Document = Layout->GetPhysicalPlacements().FindChecked(Id);
				OutputScope->ReplacedDocuments.Add(Document);
				OutputScope->ReplacedPointers.FindOrAdd(Document).AddUnique(*Pointer);
				if (Content) OutputScope->ContentFiles.Add(Id, *Content);
			}
			// A newly added row can share a destination with untouched published rows.
			// Read that guide once the formatter has determined both old and new addresses.
			if (OutputScope->PreviousExampleIndex) for (const auto& Path : OutputScope->ReplacedDocuments) {
				const FString GuidePath = TEXT("examples/files/") + Path;
				const auto* PublishedHash = OutputScope->PublishedFileHashes->Find(GuidePath);
				if (!PublishedHash) continue;
				FUeAgentCanonicalValuePtr Guide;
				if (!TryReadPublishedCanonicalDocument(ProjectionBindingRoot, GuidePath, *PublishedHash, Resources, Guide, OutError)) return false;
				OutputScope->PreviousGuides.Add(Path, Guide.ToSharedRef());
			}
			if (OutputScope->bPublishGuides && !Examples.Publish(Files, OutError, [](const TArray<uint8>& Bytes) { return Sha256(Bytes); },
				nullptr, nullptr, nullptr, OutputScope->PreviousExampleIndex, &OutputScope->ReplacedDocuments,
				&OutputScope->PreviousGuides, &OutputScope->ReplacedPointers, &OutputScope->ProducedGuides)) return false;
			OutputScope->Layout = MoveTemp(Layout); OutputScope->Files = MoveTemp(Files);
			return true;
		}
		const int32 DocumentProvenInventoryRows = InventoryByUnit.Num();
		// These are real successful public-document consumers, not merely a
		// cache candidate. Their complete member/projection proof kept the old
		// pointers, edit hashes and absence of external content in this plan.
		auto NeedsInventoryHashRepair = [&](const FString& Id) {
			const auto* Hash = Baseline && Baseline->PublishedRows ? Baseline->PublishedRows->RowHashes.Find(Id) : nullptr;
			// An explicit empty slot means the accepted inventory had no
			// canonical digest. A missing optional cache entry is different.
			return Hash && Hash->IsEmpty();
		};
		auto HasCurrentDocumentRowProof = [&](int32 Index) {
			// The successful document consumer proved complete members, schema,
			// pointers, edit hashes and absence of external content. Raw-row spans
			// belong to splicing, not to this complete machine-file result.
			return bAcceptedGeneratedBaseline && bInheritedRowContainers
				&& !NeedsInventoryHashRepair(ModelUnits[Index].GetUnitId())
				&& ReusedPackedDocuments.Contains(Layout->GetPhysicalPlacements().FindChecked(ModelUnits[Index].GetUnitId()));
		};
		int32 ConsumedDocumentRowProofs = 0;
		// A complete unchanged machine file is the stronger reusable result.
		// Establish its membership before rebuilding optional per-row graphs.
		bool bSameModelOrder = bReusableBaseline;
		if (bSameModelOrder && Baseline->SourceLayout) {
			const auto& Before = Baseline->SourceLayout->GetModel()->GetDeclarations();
			bSameModelOrder = Before.Num() == ModelUnits.Num();
			for (int32 Index = 0; bSameModelOrder && Index < ModelUnits.Num(); ++Index)
				bSameModelOrder = ExportDataIdentical(Before[Index].GetUnitId(), ModelUnits[Index].GetUnitId());
		}
		// A legacy workspace keeps its organization during ordinary edits. A
		// requested full export can reorganize private files once, without
		// changing the schema, author files or semantic fingerprint.
		const bool bScopedMachineBuckets = !bReusableBaseline || Baseline->bScopedMachineBuckets
			|| (ExportSettings && ExportSettings->ActualExportMode == TEXT("full"));
		const bool bSameMachineOrganization = bReusableBaseline
			&& Baseline->bScopedMachineBuckets == bScopedMachineBuckets;
		const bool bSameBucketMembership = bSameMachineOrganization && Baseline->MachineBuckets && bSameModelOrder
			&& (bIdenticalLayout || ExportMapsIdentical(Baseline->Projection->PhysicalPlacements, Layout->GetPhysicalPlacements()));
		TSharedPtr<const FMachineBucketMembership> MachineBuckets;
		if (bSameBucketMembership) MachineBuckets = Baseline->MachineBuckets;
		else {
			auto Membership = MakeShared<FMachineBucketMembership>();
			for (int32 Index = 0; Index < ModelUnits.Num(); ++Index) {
				const FString& Document = Layout->GetPhysicalPlacements().FindChecked(ModelUnits[Index].GetUnitId());
				FString* Path = Membership->DocumentBuckets.Find(Document);
				if (!Path) Path = &Membership->DocumentBuckets.Add(Layout->GetPhysicalFiles().FindChecked(Document).GetDocument()->PhysicalPath,
					MachineEnvelopePath(Document, bScopedMachineBuckets));
				Membership->UnitIndices.FindOrAdd(*Path).Add(Index);
			}
			MachineBuckets = MoveTemp(Membership);
		}
		TSet<FString> ChangedBucketMembership;
		if (bReusableBaseline && !bSameBucketMembership && Baseline->SourceLayout) {
			// The optional retained membership index can be absent at its original
			// budget. The actual old model still gives the ordered stable IDs;
			// compare them directly with the current bucket's consumers once.
			TMap<FString, FString> OldDocumentBuckets;
			TMap<FString, int32> OldPositions;
			for (const auto& Before : Baseline->SourceLayout->GetModel()->GetDeclarations()) {
				const FString& Path = Baseline->Projection->PhysicalPlacements.FindChecked(Before.GetUnitId());
				const FString* Bucket = bSameMachineOrganization ? MachineBuckets->DocumentBuckets.Find(Path) : nullptr;
				if (!Bucket) {
					Bucket = OldDocumentBuckets.Find(Path);
					if (!Bucket) Bucket = &OldDocumentBuckets.Add(Path, MachineEnvelopePath(Path, Baseline->bScopedMachineBuckets));
				}
				const int32 Position = OldPositions.FindOrAdd(*Bucket)++;
				const auto* Current = MachineBuckets->UnitIndices.Find(*Bucket);
				if (!Current || !Current->IsValidIndex(Position)
					|| !ExportDataIdentical(ModelUnits[(*Current)[Position]].GetUnitId(), Before.GetUnitId())) ChangedBucketMembership.Add(*Bucket);
			}
			for (const auto& Group : MachineBuckets->UnitIndices) {
				const auto* PreviousCount = OldPositions.Find(Group.Key);
				if (!PreviousCount || *PreviousCount != Group.Value.Num()) ChangedBucketMembership.Add(Group.Key);
			}
		}
		TBitArray<> FileProvenInventoryIndices(false, ModelUnits.Num());
		int32 FileProvenInventoryRows = 0;
		if (bReusableBaseline && Baseline->MachineBuckets) {
			TSet<FString> AffectedBuckets = ChangedBucketMembership;
			if (bSharedRowContainers && bSameBucketMembership) {
				for (const FString& Path : ChangedPhysicalPaths)
					AffectedBuckets.Add(MachineBuckets->DocumentBuckets.FindChecked(Path));
			} else {
				// Old integer positions are not identities after structural edits.
				// Prove each surviving row with the existing complete semantic and
				// projection key, then current pointer/hash/content-file evidence.
				for (const auto& Unit : ModelUnits) {
					if (HasCurrentDocumentRowProof(static_cast<int32>(&Unit - ModelUnits.GetData()))) continue;
					const FString& Id = Unit.GetUnitId();
					const auto* Pointer = UnitPointers.Find(Id);
					const auto* Hash = UnitEditHashes.Find(Id);
					const auto* OldPointer = Baseline->UnitPointers.Find(Id);
					const auto* OldHash = Baseline->UnitEditHashes.Find(Id);
					const auto* Content = UnitContentFiles.Find(Id);
					const auto* OldContent = Baseline->UnitContentFiles.Find(Id);
					const bool bSameContent = (!Content && !OldContent)
						|| (Content && OldContent && ExportDataIdentical(*Content, *OldContent));
					if (!IsIdenticalUnit(Id) || !Pointer || !Hash || !OldPointer || !OldHash || !bSameContent
						|| !ExportDataIdentical(*Pointer, *OldPointer) || !ExportDataIdentical(*Hash, *OldHash))
						AffectedBuckets.Add(MachineBuckets->DocumentBuckets.FindChecked(Layout->GetPhysicalPlacements().FindChecked(Id)));
				}
				for (const auto& Before : Baseline->Projection->PhysicalPlacements) {
					const auto* Now = Layout->GetPhysicalPlacements().Find(Before.Key);
					if (!Now || !ExportDataIdentical(*Now, Before.Value))
						AffectedBuckets.Add(Baseline->MachineBuckets->DocumentBuckets.FindChecked(Before.Value));
				}
			}
			for (const auto& Group : MachineBuckets->UnitIndices) {
				for (const int32 Index : Group.Value) if (NeedsInventoryHashRepair(ModelUnits[Index].GetUnitId())) {
					AffectedBuckets.Add(Group.Key); break;
				}
				if (AffectedBuckets.Contains(Group.Key) || !BaselineFiles.Contains(Group.Key)
					|| !BaselineFiles.Contains(Group.Key.Replace(TEXT("/baselines/"), TEXT("/inventory/")))) continue;
				for (const int32 Index : Group.Value) FileProvenInventoryIndices[Index] = true;
				FileProvenInventoryRows += Group.Value.Num();
			}
		}
		const double InventoryMembershipFinished = FPlatformTime::Seconds();
		auto InheritMachineSpans = [&](int32 Index) {
			if (AcceptedMachineRows.IsValidIndex(Index) && AcceptedMachineRows[Index].Inventory.IsValid()
				&& AcceptedMachineRows[Index].Envelope.IsValid()) {
				const auto& Old = AcceptedMachineRows[Index];
				GeneratedMachineSpans[Index] = {FIntPoint(static_cast<int32>(Old.Inventory.Begin), static_cast<int32>(Old.Inventory.End)),
					FIntPoint(static_cast<int32>(Old.Envelope.Begin), static_cast<int32>(Old.Envelope.End))};
			} else if (Baseline && Baseline->SourceLayout) {
				const int32 OldIndex = Baseline->SourceLayout->GetModel()->FindUnitIndex(ModelUnits[Index].GetUnitId());
				if (OldIndex != INDEX_NONE) {
					if (Baseline->GeneratedMachineSpans->IsValidIndex(OldIndex)) GeneratedMachineSpans[Index] = (*Baseline->GeneratedMachineSpans)[OldIndex];
				}
			}
		};
		TArray<int32> ChangedInventoryIndices;
		int32 RepairedInventoryHashes = 0;
		auto BuildInventoryRow = [&](int32 UnitIndex) -> bool {
			const auto& Declaration = ModelUnits[UnitIndex];
			if (InventoryByUnit.Contains(Declaration.GetUnitId())) return true;
			const auto* Value = Model->FindUnit(Declaration.GetUnitId());
			if (!Value) { Model->CheckValueReadResult(OutError); return false; }
			const FSemanticUnit& Unit = *Value;
			FString RowHash;
			if (PlannedInventory.IsValidIndex(UnitIndex)
				&& PlannedInventory[UnitIndex].UnitId.Equals(Unit.GetUnitId(), ESearchCase::CaseSensitive))
				RowHash = PlannedInventory[UnitIndex].ContentHash;
			else if (!TryHashSemanticUnitRow(Unit, RowHash, OutError,
				Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId()))) return false;
			const FString* UnitPointer = UnitPointers.Find(Unit.GetUnitId());
			const FString* UnitEditHash = UnitEditHashes.Find(Unit.GetUnitId());
			if (!UnitPointer || !UnitEditHash)
			{
				OutError.SetSourceFailure(TEXT("composition_unit_document_missing"), !UnitPointer ? TEXT("The exported unit has no document location.") : TEXT("The exported unit has no content fingerprint."),
					Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId()));
				return false;
			}
			FUeAgentCanonicalValuePtr CanonicalInventory;
			const auto* ContentFiles = UnitContentFiles.Find(Unit.GetUnitId()); const TArray<FString> EmptyContent;
			if (!TryBuildPublishedMachineRow(Unit, *Model, Layout->GetMachineManifest().PublicRefMapping.FindChecked(Unit.GetUnitId()),
				Layout->GetLogicalPlacements().FindChecked(Unit.GetUnitId()), Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId()),
				*UnitPointer, ContentFiles ? *ContentFiles : EmptyContent, RowHash, *UnitEditHash, Resources, CanonicalInventory, OutError)) return false;
			InventoryByUnit.Add(Unit.GetUnitId(), CanonicalInventory.ToSharedRef());
			if (NeedsInventoryHashRepair(Unit.GetUnitId())) ++RepairedInventoryHashes;
			ChangedInventoryIndices.Add(UnitIndex);
			return true;
		};
		for (int32 UnitIndex = 0; UnitIndex < ModelUnits.Num(); ++UnitIndex)
		{
			const bool bRepairHash = NeedsInventoryHashRepair(ModelUnits[UnitIndex].GetUnitId());
			if (bRepairHash) InventoryByUnit.Remove(ModelUnits[UnitIndex].GetUnitId());
			if (FileProvenInventoryIndices[UnitIndex]) { ++ReusedInventoryRows; continue; }
			const auto& Unit = ModelUnits[UnitIndex];
			if (InventoryByUnit.Contains(Unit.GetUnitId())) { ++ReusedInventoryRows; continue; }
			if (HasCurrentDocumentRowProof(UnitIndex)) { ++ConsumedDocumentRowProofs; continue; }
			bool bRawCandidate = false;
			if (!bRepairHash && IsIdenticalUnit(Unit.GetUnitId()))
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
				if (Pointer && EditHash && PreviousPointer && PreviousHash && bSameContentPaths
					&& ExportDataIdentical(*Pointer, *PreviousPointer) && ExportDataIdentical(*EditHash, *PreviousHash)) {
					if (PreviousInventory) {
						InventoryByUnit.Add(Unit.GetUnitId(), *PreviousInventory); ++ReusedInventoryRows; continue;
					}
					bRawCandidate = AcceptedMachineRows.IsValidIndex(UnitIndex) && AcceptedMachineRows[UnitIndex].bCandidate
						&& PublishedFileHashes && !PublishedFileHashes->IsEmpty();
				}
			}
			if (AcceptedMachineRows.IsValidIndex(UnitIndex)) AcceptedMachineRows[UnitIndex].bCandidate = bRawCandidate;
			if (!bRawCandidate && !BuildInventoryRow(UnitIndex)) return false;
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=inventory_digest_repair rows=%d native_replayed=false source=accepted_envelope original_row_writer=true"), RepairedInventoryHashes);
		const double InventoryRowsFinished = FPlatformTime::Seconds();
		// Keep the complete captured baseline in the same schema. Nested item
		// documents share their existing top-level owner; global documents remain
		// independent. Legacy global buckets stay readable and ordinary edits do
		// not silently migrate them.
		TExportRows<FUeAgentCanonicalValueRef> ManifestInventoryByUnit;
		const bool bReusableMachineRows = bReusableBaseline;
		if (bReusableMachineRows)
		{
			ManifestInventoryByUnit = Baseline->ManifestInventory;
			if (!bSameBucketMembership)
				ManifestInventoryByUnit.RemoveIf([&](const FString& Key) { return !Model->FindDeclaration(Key); });
		}
		TSet<FString> ChangedBaselineGroups = MoveTemp(ChangedBucketMembership);
		for (const int32 Index : ChangedInventoryIndices)
			ChangedBaselineGroups.Add(MachineBuckets->DocumentBuckets.FindChecked(
				Layout->GetPhysicalPlacements().FindChecked(ModelUnits[Index].GetUnitId())));
		if (bReusableBaseline && !bSameBucketMembership && !Baseline->SourceLayout)
		{
			// Removed or moved rows must also invalidate their old bucket, even
			// when every row remaining in that bucket can be reused unchanged.
			for (const auto& PreviousPlacement : Baseline->Projection->PhysicalPlacements)
			{
				const FString* Current = Layout->GetPhysicalPlacements().Find(PreviousPlacement.Key);
				if (!Current || !ExportDataIdentical(*Current, PreviousPlacement.Value))
					ChangedBaselineGroups.Add(MachineEnvelopePath(PreviousPlacement.Value, Baseline->bScopedMachineBuckets));
			}
		}
		int32 ReusedBaselineFiles = 0, ReusedInventoryFiles = 0, GroupedInventoryRows = 0;
		int32 SplicedMachineRows = 0, SplicedMachineGroups = 0;
		int64 SplicedMachineBytes = 0;
		double MachineFragmentReadSeconds = 0, MachineFragmentWriteSeconds = 0;
		TArray<TSharedPtr<const TArray<uint8>>> AcceptedMachineByteBacking;
		auto ReadAcceptedMachineFile = [&](const FString& Path, TArray<uint8>& Bytes) -> const TArray<uint8>* {
			const double Started = FPlatformTime::Seconds();
			ON_SCOPE_EXIT { MachineFragmentReadSeconds += FPlatformTime::Seconds() - Started; };
			const auto* Published = PublishedFileHashes ? PublishedFileHashes->Find(Path) : nullptr;
			const auto* Accepted = FindAcceptedSemanticSourceHash(AcceptedRoot, Path);
			if (!Published || !Accepted || !ExportDataIdentical(*Published, *Accepted)) return nullptr;
			if (const auto* File = BaselineFiles.Find(Path); File && (*File)->GetContentHash() == *Published)
				return &(*File)->GetCanonicalUtf8();
			if (const auto GeneratedBytes = GetAcceptedGeneratedInventoryBytes(AcceptedRoot, Path)) {
				AcceptedMachineByteBacking.Add(GeneratedBytes); ++AcceptedGeneratedByteReuses;
				return GeneratedBytes.Get();
			}
			FString Hash;
			return TryReadAcceptedSemanticSourceBytes(AcceptedRoot, Path, Bytes, Hash, OutError)
				&& ExportDataIdentical(Hash, *Published) ? &Bytes : nullptr;
		};
		const auto SpanView = &PublishedMachineSpanView;
		TArray<FString> InventoryPaths;
		for (const auto& Group : MachineBuckets->UnitIndices)
		{
			const FString InventoryPath = Group.Key.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
			InventoryPaths.Add(InventoryPath);
			const bool bReuseGroup = bReusableMachineRows && !ChangedBaselineGroups.Contains(Group.Key)
				&& FindPublishedFile(Group.Key) && FindPublishedFile(InventoryPath);
			if (OutError.HasErrors()) return false;
			if (bReuseGroup)
			{
				Files.Add(*BaselineFiles.FindChecked(Group.Key));
				Files.Add(*BaselineFiles.FindChecked(InventoryPath));
				for (int32 Index : Group.Value) InheritMachineSpans(Index);
				++ReusedBaselineFiles; ++ReusedInventoryFiles;
				continue;
			}
			ChangedBaselineGroups.Add(Group.Key);
			bool bHasFragments = false;
			for (int32 Index : Group.Value)
				if (AcceptedMachineRows.IsValidIndex(Index) && AcceptedMachineRows[Index].bCandidate
					&& IsIdenticalUnit(ModelUnits[Index].GetUnitId())) { bHasFragments = true; break; }
			TArray<uint8> PreviousEnvelopes, PreviousRows;
			const auto* AcceptedEnvelopeBytes = bHasFragments ? ReadAcceptedMachineFile(Group.Key, PreviousEnvelopes) : nullptr;
			const auto* AcceptedRowBytes = AcceptedEnvelopeBytes && !bDirectPublication ? ReadAcceptedMachineFile(InventoryPath, PreviousRows) : nullptr;
			bHasFragments = AcceptedEnvelopeBytes && (bDirectPublication || AcceptedRowBytes);
			if (OutError.HasErrors()) return false;
			if (bHasFragments && bDirectPublication) {
				const auto Metadata = GetAcceptedWorkspaceInventory(AcceptedRoot);
				if (!Metadata || !Metadata->VisitBucket(Group.Key, [&](const FAcceptedWorkspaceInventoryRow& Row) {
					const int32 Index = Model->FindUnitIndex(Row.UnitId);
					if (Index == INDEX_NONE || !AcceptedMachineRows.IsValidIndex(Index)
						|| !AcceptedMachineRows[Index].bCandidate || !IsIdenticalUnit(Row.UnitId)) return true;
					FUeAgentCanonicalValuePtr Thin;
					if (!TryBuildPublishedInventoryMetadata(Row, Resources, Thin, OutError)) return false;
					ManifestInventoryByUnit.Add(Row.UnitId, Thin.ToSharedRef()); return true;
				}, false)) return false;
				}
			{
				const double Started = FPlatformTime::Seconds();
				ON_SCOPE_EXIT { MachineFragmentWriteSeconds += FPlatformTime::Seconds() - Started; };
				TArray<FUeAgentStructuredHjsonObjectFragment> Envelopes, Rows;
				TArray<FUeAgentCanonicalValueRef> MetadataRows;
				auto Identities = MakeShared<FExportFilePlan::FFragmentIdentities>();
				TArray<TArray<uint8>> Generated;
				Envelopes.Reserve(Group.Value.Num()); Rows.Reserve(Group.Value.Num());
				for (int32 Index : Group.Value) {
					++GroupedInventoryRows;
					const FString& Id = ModelUnits[Index].GetUnitId();
					Identities->Add(ModelUnits[Index].GetSharedUnitId());
					TConstArrayView<uint8> EnvelopeView, RowView;
					if (bHasFragments && AcceptedMachineRows.IsValidIndex(Index) && AcceptedMachineRows[Index].bCandidate && IsIdenticalUnit(Id)) {
						EnvelopeView = SpanView(*AcceptedEnvelopeBytes, AcceptedMachineRows[Index].Envelope, 1);
						if (!bDirectPublication) RowView = SpanView(*AcceptedRowBytes, AcceptedMachineRows[Index].Inventory, 2);
					}
					if (!EnvelopeView.IsEmpty() && (bDirectPublication || !RowView.IsEmpty())) {
						Envelopes.Add({Id, EnvelopeView, 1});
						if (bDirectPublication) {
							const auto* Thin = ManifestInventoryByUnit.Find(Id);
							if (!Thin) return FailWorkspaceDataflowHandoff(OutError, TEXT("publication.inventory_metadata"), Id,
								InventoryPath, TEXT("accepted row projection absent"), TEXT("metadata for actual retained row"), TEXT("accepted inventory"));
							MetadataRows.Add(*Thin);
							} else Rows.Add({FString(), RowView, 2});
						++SplicedMachineRows; ++ReusedInventoryRows;
						SplicedMachineBytes += EnvelopeView.Num() + RowView.Num();
						continue;
					}
					if (!BuildInventoryRow(Index)) return false;
					const auto& Complete = InventoryByUnit.FindChecked(Id);
					const auto* Envelope = Complete->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("envelope"); });
					if (!Envelope) { OutError.SetSourceFailure(TEXT("composition_unit_envelope_failed"), TEXT("A captured baseline envelope is missing."), Id); return false; }
					auto Fields = Complete->GetObjectItems();
					Fields.RemoveAll([](const auto& Field) { return Field.Key == TEXT("envelope"); });
					FUeAgentCanonicalValuePtr Reference, Thin;
					FUeAgentStateSemanticError Error;
					if (!FUeAgentCanonicalValue::TryString(Group.Key, Resources, Reference, Error)) { OutError.SetFailure(Error); return false; }
					Fields.Emplace(TEXT("envelope_ref"), Reference.ToSharedRef());
					if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Thin, Error)) { OutError.SetFailure(Error); return false; }
					ManifestInventoryByUnit.Add(Id, Thin.ToSharedRef());
					auto& EnvelopeBytes = Generated.AddDefaulted_GetRef();
					if (!TrySerializeSemanticHjsonObject(*Envelope->Value, EnvelopeBytes, OutError, Group.Key)) return false;
					Envelopes.Add({Id, MakeArrayView(EnvelopeBytes.GetData(), EnvelopeBytes.Num() - 1), 0});
					if (bDirectPublication) MetadataRows.Add(Thin.ToSharedRef());
					else {
						auto& RowBytes = Generated.AddDefaulted_GetRef();
						if (!TrySerializeSemanticHjsonObject(*Thin, RowBytes, OutError, InventoryPath)) return false;
						Rows.Add({FString(), MakeArrayView(RowBytes.GetData(), RowBytes.Num() - 1), 0});
						}
				}
				bool bBlockShape = true;
				for (const auto* Parts : {&Envelopes, &Rows}) for (const auto& Part : *Parts)
					if (!std::memchr(Part.Utf8.GetData(), '\n', Part.Utf8.Num())) bBlockShape = false;
				if (!bBlockShape) {
					OutError.SetSourceFailure(TEXT("workspace_machine_fragment_shape_invalid"),
						TEXT("The machine-row formatter did not produce block fragments. No alternate whole-bucket builder was selected."), Group.Key);
					return false;
				}
				{
					TArray<uint8> EnvelopeBytes, RowBytes;
					TArray<FIntPoint> EnvelopeSpans, RowSpans;
					Contract::FDiagnostic Error;
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObjectFragments(Envelopes, FString(), EnvelopeBytes, Error, &EnvelopeSpans)) {
						OutError.SetFailure(Error, Group.Key); return false;
					}
					if (bDirectPublication) {
						if (!TrySerializePublishedInventoryWithEnvelopeSpans(MetadataRows, EnvelopeSpans, Resources, RowBytes, RowSpans, OutError, InventoryPath)) return false;
						} else if (!FUeAgentStructuredHjsonFormatter::TryWriteObjectFragments(Rows, TEXT("unit_inventory"), RowBytes, Error, &RowSpans)) {
						OutError.SetFailure(Error, Group.Key); return false;
						}
					const FString EnvelopeHash = Sha256(EnvelopeBytes), RowHash = Sha256(RowBytes);
					for (int32 I = 0; I < Group.Value.Num(); ++I)
						GeneratedMachineSpans[Group.Value[I]] = {RowSpans[I], EnvelopeSpans[I]};
					Files.Emplace(Group.Key, MoveTemp(EnvelopeBytes), EnvelopeHash, MoveTemp(EnvelopeSpans), Identities);
					Files.Emplace(InventoryPath, MoveTemp(RowBytes), RowHash, MoveTemp(RowSpans), Identities);
					++SplicedMachineGroups;
					continue;
				}
			}
		}
		const double InventoryGroupsFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=baseline_documents files=%d reused_files=%d"),
			MachineBuckets->UnitIndices.Num(), ReusedBaselineFiles);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=machine_row_fragments groups=%d reused_rows=%d reused_bytes=%lld generated_rows=%d read_s=%.6f write_s=%.6f proof=accepted_current_row_and_published_bytes"),
			SplicedMachineGroups, SplicedMachineRows, SplicedMachineBytes, ChangedInventoryIndices.Num(), MachineFragmentReadSeconds, MachineFragmentWriteSeconds);
		const double BaselinesFinished = FPlatformTime::Seconds();
		// The inventory is machine-owned just like the complete baselines. Keep
		// its stable buckets separate from the changing revision header so the
		// publisher/resolver can validate actual unchanged files with the same
		// parser proof, rather than reparsing every row for three changed values.
		InventoryPaths.Sort();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=inventory_documents files=%d reused_files=%d membership_s=%.6f rows_s=%.6f grouping_s=%.6f baseline_write_s=%.6f inventory_write_s=%.6f"),
			InventoryPaths.Num(), ReusedInventoryFiles, InventoryMembershipFinished - DocumentsFinished, InventoryRowsFinished - InventoryMembershipFinished,
			InventoryGroupsFinished - InventoryRowsFinished, BaselinesFinished - InventoryGroupsFinished,
			FPlatformTime::Seconds() - BaselinesFinished);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=machine_bucket_reuse membership_reused=%s document_proven_rows=%d file_proven_rows=%d grouped_rows=%d membership_bytes=%lld"),
			bSameBucketMembership ? TEXT("true") : TEXT("false"), DocumentProvenInventoryRows, FileProvenInventoryRows, GroupedInventoryRows, MachineBuckets->Charge());
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=machine_output_organization scoped_owners=%s legacy_migration=%s groups=%d documents=%d"),
			bScopedMachineBuckets ? TEXT("true") : TEXT("false"), bReusableBaseline && !bSameMachineOrganization ? TEXT("true") : TEXT("false"),
			MachineBuckets->UnitIndices.Num(), MachineBuckets->DocumentBuckets.Num());

		const double InventoryFinished = FPlatformTime::Seconds();
		const auto Targets = MakeShared<FJsonObject>();
		TMap<FString, FString> PreviewAssetPaths;
		for (const auto& Unit : Model->GetDeclarations())
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
				const auto* Constraints = Definition->FindValue(TEXT("constraints"));
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
		TSharedPtr<const FAuthoringExamplePublicationProof> ExamplePublication;
		if (!Examples.Publish(Files, OutError, [](const TArray<uint8>& Bytes) { return Sha256(Bytes); },
			bReusableBaseline ? Baseline->ExamplePublication : nullptr, &ExamplePublication,
			bReusableBaseline ? &BaselineFiles : nullptr)) return false;
		const double ExamplesFinished = FPlatformTime::Seconds();

		const FUeAgentStateRevision& Revision =
			SnapshotBinding.Revision;
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(
			TEXT("workspace_schema"),
			ExportSettings && ExportSettings->Publication.bDirectDataflow ? GetDirectWorkspaceSchemaId()
				: ExportSettings && ExportSettings->Publication.bGenerationPublication
					? GetGenerationWorkspaceSchemaId() : GetSemanticWorkspaceSchemaId());
		if (ExportSettings && ExportSettings->Publication.bGenerationPublication) {
			Manifest->SetStringField(TEXT("publication_generation"), ExportSettings->Publication.Generation);
			Manifest->SetStringField(TEXT("publication_state"), TEXT("committed"));
		}
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
		if (!(ExportSettings && ExportSettings->Publication.bDirectDataflow))
			Manifest->SetStringField(TEXT("semantic_fingerprint"), Model->GetSemanticFingerprint());
		Manifest->SetArrayField(
			TEXT("target_unit_ids"),
			StringArray(Model->GetTargetUnitIds()));
		Manifest->SetArrayField(
			TEXT("structural_edits"),
			TArray<TSharedPtr<FJsonValue>>{});
		Manifest->SetArrayField(TEXT("unit_inventory_files"), StringArray(InventoryPaths));
		// The actual writer, not optional retention of its complete layout,
		// owns these facts for the newly committed publication generation.
		Manifest->SetStringField(TEXT("machine_bucket_organization"), bScopedMachineBuckets ? TEXT("owner_scope_v1") : TEXT("document_prefix_v1"));
		Manifest->SetField(TEXT("published_unit_count"), MakeShared<FJsonValueNumberString>(FString::FromInt(Model->GetUnitCount())));
		TSet<FString> ExpandedAuthorDocuments;
		for (const auto& Content : UnitContentFiles) if (!Content.Value.IsEmpty())
			ExpandedAuthorDocuments.Add(Layout->GetPhysicalPlacements().FindChecked(Content.Key));
		TArray<FString> ExpandedDocumentPaths = ExpandedAuthorDocuments.Array(); ExpandedDocumentPaths.Sort();
		Manifest->SetArrayField(TEXT("expanded_author_documents"), StringArray(ExpandedDocumentPaths));

		TArray<uint8> ManifestBytes;
		FUeAgentCanonicalValuePtr CanonicalManifest;
		if (!TryCanonicalizeJson(MakeShared<FJsonValueObject>(Manifest), Resources, CanonicalManifest, OutError)) return false;
		if (!TrySerializeSemanticHjsonObject(*CanonicalManifest, ManifestBytes, OutError, ManifestPath))
		{
			
			return false;
		}
		const FString ManifestHash = Sha256(ManifestBytes);
		RememberFormattedHjsonRootHeader(ManifestHash, *CanonicalManifest);
		Files.Emplace(
			ManifestPath,
			MoveTemp(ManifestBytes),
			ManifestHash);
		const double ManifestFinished = FPlatformTime::Seconds();
		if (ExportSettings && OutPendingAuthorLocations) for (const auto& Pending : ExportSettings->PendingAuthorUnits) {
			const auto* Path = Layout->GetPhysicalPlacements().Find(Pending.Key);
			const auto* Pointer = UnitPointers.Find(Pending.Key);
			if (Path && Pointer) OutPendingAuthorLocations->Add(Pending.Key, {*Path, *Pointer});
		}
		if (ExportSettings && OutPendingAuthorLocations) for (const auto& Pending : ExportSettings->PendingKeyedAuthorRows) {
			const auto* Path = Layout->GetPhysicalPlacements().Find(Pending.Key);
			const auto* Pointer = UnitPointers.Find(Pending.Key);
			if (Path && Pointer) OutPendingAuthorLocations->Add(Pending.Key, TPair<FString, FString>(*Path, *Pointer));
		}
		Files.Sort(
			[](const FExportFilePlan& Left,
				const FExportFilePlan& Right)
			{
				return Left.GetRelativePath()
					< Right.GetRelativePath();
			});
		// The explicit old-record upgrade may change inventory bytes and hashes.
		// Bind its real ranges before accounting or sealing this same plan.
		if (!PrepareCompleteWorkspacePublicationDirectory(ProjectionBindingRoot, Model.Get(), *Layout, bScopedMachineBuckets, OutError)
			|| !PrepareExplicitWorkspaceFileVersionInputs(ProjectionBindingRoot, Files, Resources, OutError,
				bDirectPublication, &Model.Get(), &GeneratedMachineSpans)) return false;

		int64 OutputBytes = 0;
		TSet<FWorkspacePlacementStorage::FKey> SourceContentPaths;
		for (const auto& Content : UnitContentFiles)
			for (const auto& Path : Content.Value) SourceContentPaths.Add({&Path});
		FString PlanMaterial =
			TEXT("directory|") + ManifestPath + TEXT("|")
			+ Model->GetSemanticFingerprint()
			+ TEXT("|") + SnapshotBinding.SnapshotId
			+ TEXT("|") + Revision.GetSourceId()
			+ TEXT("|") + Revision.GetGeneration()
			+ TEXT("|")
			+ FString::Printf(TEXT("%lld"), Revision.GetSequence())
			+ TEXT("|") + SnapshotBinding.EvidenceId;
		for (FExportFilePlan& File : Files)
		{
			ArtifactPublisher::RememberVerifiedPlanBytes(File);
			File.SetSemanticSourceDocument(Layout->GetPhysicalFiles().Contains(File.GetRelativePath())
				|| SourceContentPaths.Contains(FWorkspacePlacementStorage::FKey{&File.GetRelativePath()}));
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
					Files.Num() + Model->GetUnitCount()},
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
		// A single candidate, with independently admitted optional backing. An
		// expanded unit count must not discard every proven file and row at once.
		PreviousCompositionExport.Reset();
		{
			auto Next = MakeShared<FCompositionExportBaseline>();
			Next->ProducedPlan = OutPlan;
			if (ExportSettings) Next->Publication = ExportSettings->Publication;
			Next->bScopedMachineBuckets = bScopedMachineBuckets;
			Next->ContractId = Model->GetContractId();
			Next->ContractVersion = Model->GetContractVersion();
			Next->ProfileId = Model->GetProfileId();
			Next->TargetUnitIds = Model->GetTargetUnitIds();
			Next->LayoutVersion = Layout->GetLayoutVersion();
			Next->Projection = Layout->GetSharedProjection();
			const int32 UpdatedContentKeys = UnitContentKeys.Num();
			if (ContentKeyBaseline) {
				// All old-key/schema consumers above have finished. No source,
				// publication or native proof depends on retaining this optional
				// memo at the retired owner. Early failures never consume it.
				Next->UnitContentKeys = MoveTemp(ContentKeyBaseline->UnitContentKeys);
				Next->ItemValueInvariantKeys = MoveTemp(ContentKeyBaseline->ItemValueInvariantKeys);
				const auto* Applied = ContentKeyBaseline == AcceptedBaseline ? MemoActualChanges : nullptr;
				if (Applied) {
					for (const auto& Change : Applied->Changes) if (!Change.After) {
						Next->UnitContentKeys.Remove(Change.UnitId); Next->ItemValueInvariantKeys.Remove(Change.UnitId);
					}
				} else {
					for (auto It = Next->UnitContentKeys.CreateIterator(); It; ++It) if (!Model->FindDeclaration(It.Key())) It.RemoveCurrent();
					for (auto It = Next->ItemValueInvariantKeys.CreateIterator(); It; ++It) if (!Model->FindDeclaration(It.Key())) It.RemoveCurrent();
				}
				for (auto& Pair : UnitContentKeys) Next->UnitContentKeys.Add(Pair.Key, MoveTemp(Pair.Value));
				for (const auto& Id : RecomputedInvariantUnits) Next->ItemValueInvariantKeys.Remove(Id);
				for (auto& Pair : ItemValueInvariantKeys) Next->ItemValueInvariantKeys.Add(Pair.Key, MoveTemp(Pair.Value));
			} else {
				Next->UnitContentKeys = MoveTemp(UnitContentKeys);
				Next->ItemValueInvariantKeys = MoveTemp(ItemValueInvariantKeys);
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_key_handoff complete_rows=%d updated_rows=%d inherited_owner=%s old_consumers_finished=true"),
				Next->UnitContentKeys.Num(), UpdatedContentKeys, ContentKeyBaseline ? TEXT("true") : TEXT("false"));
			Next->UnitProofBlocks = MoveTemp(UnitProofBlocks);
			Next->PropertyDocumentSchemaKeys = MoveTemp(PropertyDocumentSchemaKeys);
			Next->DefinitionProjectionHints = MakeShared<const FDefinitionProjectionHints>(Layout->GetDefinitionProjectionHints());
			// The writer already established this exact inventory-file order.
			// Source/hash consumers share its actual group arrays; no second
			// all-row syntax graph or copied membership array is required.
			int64 SourceInventoryBytes = 0;
			for (const FString& Path : InventoryPaths) {
				const FString Envelope = Path.Replace(TEXT("/inventory/"), TEXT("/baselines/"));
				const auto Indices = MachineBuckets->UnitIndices.FindChecked(Envelope).GetSharedIndices();
				Next->SourceInventoryRanges.Add({Envelope, Indices});
				SourceInventoryBytes += sizeof(FCompositionExportBaseline::FSourceInventoryRange)
					+ Envelope.GetAllocatedSize() + sizeof(TArray<int32>) + 32 + Indices->GetAllocatedSize();
			}
			int64 RetainedFileBytes = 0;
			// Public files have many independent next consumers; inventory bytes
			// already belong to the generated source shards. Do not let path sort
			// put large private files ahead of thousands of public byte consumers.
			for (int32 Priority = 0; Priority < 3; ++Priority) for (const FExportFilePlan& File : OutPlan->GetFiles()) {
				const FString& Path = File.GetRelativePath();
				const int32 FilePriority = !Path.StartsWith(TEXT(".uai/"), ESearchCase::CaseSensitive) ? 0
					: Path.StartsWith(TEXT(".uai/baselines/"), ESearchCase::CaseSensitive) ? 1 : 2;
				if (FilePriority != Priority || Path == ManifestPath || File.GetCanonicalUtf8().Num() > 512ll * 1024 * 1024 - RetainedFileBytes) continue;
				RetainedFileBytes += File.GetCanonicalUtf8().Num(); Next->Files.Add(File);
			}
			Next->TargetKind = TargetKind;
			constexpr int64 ProjectionBudget = 512ll * 1024 * 1024;
			TExportRows<FString> PublicationRowHashes;
			if (bAcceptedOrigin && AcceptedBaseline->PublishedRows) {
				PublicationRowHashes = AcceptedBaseline->PublishedRows->RowHashes;
				for (const auto& Change : ActualChanges->Changes) if (!Change.After) PublicationRowHashes.Remove(Change.UnitId);
				for (const auto& Id : ActualOutputChanges) if (const int32 Index = Model->FindUnitIndex(Id); Index != INDEX_NONE)
					PublicationRowHashes.Add(Id, Model->GetRowHashAt(Index));
				for (const int32 Index : ChangedInventoryIndices)
					PublicationRowHashes.Add(ModelUnits[Index].GetUnitId(), Model->GetRowHashAt(Index));
			} else for (int32 Index = 0; Index < Model->GetUnitCount(); ++Index)
				PublicationRowHashes.Add(ModelUnits[Index].GetUnitId(), Model->GetRowHashAt(Index));
			const int64 PublicationDirectoryBytes = PublicationRowHashes.StringStorageBytes()
				+ Model->GetDeclarations().GetAllocatedSize() + GeneratedMachineSpans.GetAllocatedSize();
			const bool bRetainPublicationDirectory = PublicationDirectoryBytes <= ProjectionBudget;
			int64 RetainedProjectionBytes = SourceInventoryBytes + PublicationDirectoryBytes;
			if (RetainedProjectionBytes > ProjectionBudget) { Next->SourceInventoryRanges.Reset(); RetainedProjectionBytes = PublicationDirectoryBytes; }
			int32 RejectedPackedGroups = 0, RejectedMachineGroups = 0, RejectedIndexGroups = 0;
			int32 IncompleteMachineGroups = 0, PartialMachineGroups = 0, PartialMachineRows = 0;
			// Pointer/edit-hash rows own strings too. Retain them as a complete
			// public-document group, before admitting its optional packed graph.
			// A rejected index group cannot leave a packed proof whose consumer
			// assumes these rows exist. Inventory graphs remain independent.
			for (const auto& Group : Layout->GetPhysicalFiles()) {
				const auto* BeforeMembers = bReusableBaseline ? Baseline->Projection->PhysicalFiles.Find(Group.Key) : nullptr;
				const int64* OldCharge = bInheritedRowContainers && !ChangedPhysicalPaths.Contains(Group.Key)
					&& BeforeMembers && ExportDataIdentical(*BeforeMembers, Group.Value)
					? Baseline->DocumentIndexCharges.Find(Group.Key) : nullptr;
				int64 Charge = OldCharge ? *OldCharge : Group.Key.GetAllocatedSize() + 128;
				if (!OldCharge) for (const FString& Id : Group.Value) {
					const FString* Pointer = UnitPointers.Find(Id);
					const FString* EditHash = UnitEditHashes.Find(Id);
					if (!Pointer || !EditHash) { Charge = ProjectionBudget + 1; break; }
					Charge += Id.GetAllocatedSize() * 2 + Pointer->GetAllocatedSize()
						// Includes map entries and copy-on-write bucket allocation overhead.
						+ EditHash->GetAllocatedSize() + 256;
					if (Charge > ProjectionBudget) break;
				}
				if (Charge <= ProjectionBudget - RetainedProjectionBytes) {
					RetainedProjectionBytes += Charge; Next->DocumentIndexCharges.Add(Group.Key, Charge);
				} else {
					++RejectedIndexGroups;
					for (const FString& Id : Group.Value) { UnitPointers.Remove(Id); UnitEditHashes.Remove(Id); }
				}
			}
			// Unchanged machine files can bypass row graphs entirely. Prefer the
			// packed public representation that their document consumer still reads,
			// then spend the same remaining budget on complete machine row groups.
			for (const auto& Pair : PackedDocuments) {
				// Property JSON finished its last consumer during formatting above.
				// The next publication/source/guide consumes canonical plus bytes;
				// only definition catalogs still have a real JSON reader.
				if (!Pair.Value->HasTypedField<EJson::Object>(TEXT("definitions"))) continue;
				if (!Next->DocumentIndexCharges.Contains(Pair.Key)) { ++RejectedPackedGroups; continue; }
				const auto* Example = PackedExamples.Find(Pair.Key);
				const auto* PreviousExample = bReusableBaseline ? Baseline->PackedExamples.Find(Pair.Key) : nullptr;
				const bool bSameExampleBacking = (!Example && !PreviousExample)
					|| (Example && PreviousExample && *Example == *PreviousExample);
				const int64* OldCharge = bReusableBaseline && ReusedPackedDocuments.Contains(Pair.Key) && bSameExampleBacking
					? Baseline->PackedGroupCharges.Find(Pair.Key) : nullptr;
				const int64 Charge = OldCharge ? *OldCharge : Pair.Value->GetMemoryFootprint()
					+ Pair.Key.GetAllocatedSize() * 3 + 384
					+ (Example ? CanonicalBackingCharge(*Example, ProjectionBudget) : 0);
				if (Charge > ProjectionBudget - RetainedProjectionBytes) { ++RejectedPackedGroups; continue; }
				RetainedProjectionBytes += Charge;
				Next->PackedDocuments.Add(Pair.Key, Pair.Value);
				if (Example) Next->PackedExamples.Add(Pair.Key, *Example);
				Next->PackedGroupCharges.Add(Pair.Key, Charge);
			}
			for (const auto& Pair : PackedExamples) if (!Next->PackedDocuments.Contains(Pair.Key)) {
				if (!Next->DocumentIndexCharges.Contains(Pair.Key)) { ++RejectedPackedGroups; continue; }
				const auto* Prior = bReusableBaseline ? Baseline->PackedExamples.Find(Pair.Key) : nullptr;
				const int64* OldCharge = Prior && *Prior == Pair.Value && !Baseline->PackedDocuments.Contains(Pair.Key)
					? Baseline->PackedGroupCharges.Find(Pair.Key) : nullptr;
				const int64 Charge = OldCharge ? *OldCharge : Pair.Key.GetAllocatedSize() * 2 + 256 + CanonicalBackingCharge(Pair.Value, ProjectionBudget);
				if (Charge > ProjectionBudget - RetainedProjectionBytes) { ++RejectedPackedGroups; continue; }
				RetainedProjectionBytes += Charge; Next->PackedExamples.Add(Pair.Key, Pair.Value);
				Next->PackedGroupCharges.Add(Pair.Key, Charge);
			}
			// Published file bytes are complete independently of this optional
			// per-unit row cache. Prefer a complete group's shared charge, but a
			// file-proven missing row or a large group must not evict every other
			// exact row. Partial groups never receive a whole-group charge shortcut.
			for (const auto& Group : MachineBuckets->UnitIndices) {
				const int64* PreviousCharge = bReusableBaseline && bSameBucketMembership && !ChangedBaselineGroups.Contains(Group.Key)
					? Baseline->MachineGroupCharges.Find(Group.Key) : nullptr;
				int64 Charge = PreviousCharge ? *PreviousCharge : Group.Key.GetAllocatedSize() + 128;
				if (!PreviousCharge) {
					TSet<const FUeAgentCanonicalValue*> Seen;
					for (const int32 Index : Group.Value) {
						const FString& Id = ModelUnits[Index].GetUnitId();
						const auto* Complete = InventoryByUnit.Find(Id);
						const auto* Thin = ManifestInventoryByUnit.Find(Id);
						if (!Complete || !Thin) { ++IncompleteMachineGroups; Charge = ProjectionBudget + 1; break; }
						Charge += Id.GetAllocatedSize() * 2 + 256
							+ AddCanonicalBackingCharge(*Complete, ProjectionBudget, Seen)
							+ AddCanonicalBackingCharge(*Thin, ProjectionBudget, Seen);
						if (Charge > ProjectionBudget) break;
					}
				}
				if (Charge <= ProjectionBudget - RetainedProjectionBytes) {
					RetainedProjectionBytes += Charge; Next->MachineGroupCharges.Add(Group.Key, Charge);
				} else {
					++RejectedMachineGroups;
					int32 RetainedRows = 0;
					for (const int32 Index : Group.Value) {
						const FString& Id = ModelUnits[Index].GetUnitId();
						const auto* Complete = InventoryByUnit.Find(Id);
						const auto* Thin = ManifestInventoryByUnit.Find(Id);
						if (Complete && Thin) {
							// Charge each admitted pair independently. Shared nodes across
							// pairs may be overcounted, never retained without a charge.
							TSet<const FUeAgentCanonicalValue*> Seen;
							const int64 RowCharge = Id.GetAllocatedSize() * 2 + 256
								+ AddCanonicalBackingCharge(*Complete, ProjectionBudget, Seen)
								+ AddCanonicalBackingCharge(*Thin, ProjectionBudget, Seen);
							if (RowCharge <= ProjectionBudget - RetainedProjectionBytes) {
								RetainedProjectionBytes += RowCharge; ++RetainedRows; continue;
							}
						}
						InventoryByUnit.Remove(Id); ManifestInventoryByUnit.Remove(Id);
					}
					if (RetainedRows) { ++PartialMachineGroups; PartialMachineRows += RetainedRows; }
				}
			}
			if (RejectedMachineGroups) { InventoryByUnit.CompactRetiredBuckets(); ManifestInventoryByUnit.CompactRetiredBuckets(); }
			if (RejectedIndexGroups) { UnitPointers.CompactRetiredBuckets(); UnitEditHashes.CompactRetiredBuckets(); }
			Next->Inventory = MoveTemp(InventoryByUnit);
			Next->ManifestInventory = MoveTemp(ManifestInventoryByUnit);
			// This owner already admits complete guide groups against its own 32MiB
			// measured backing budget; total output size is unrelated to that proof.
			Next->ExamplePublication = MoveTemp(ExamplePublication);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_retention bytes=%lld files_bytes=%lld packed=%d rejected_packed=%d rejected_machine=%d rejected_index=%d inventory=%d manifest_inventory=%d index_rows=%d"),
				RetainedProjectionBytes, RetainedFileBytes, Next->PackedDocuments.Num(), RejectedPackedGroups,
				RejectedMachineGroups, RejectedIndexGroups, Next->Inventory.Num(), Next->ManifestInventory.Num(), UnitPointers.Num());
			const int64 MachineIndexBytes = MachineBuckets->Charge();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_row_retention incomplete_groups=%d partial_groups=%d partial_rows=%d machine_index_bytes=%lld machine_index_retained=%s"),
				IncompleteMachineGroups, PartialMachineGroups, PartialMachineRows, MachineIndexBytes,
				MachineIndexBytes <= 4ll * 1024 * 1024 ? TEXT("true") : TEXT("false"));
			if (MachineIndexBytes <= 4ll * 1024 * 1024) Next->MachineBuckets = MoveTemp(MachineBuckets);
			Next->UnitPointers = MoveTemp(UnitPointers);
			Next->UnitEditHashes = MoveTemp(UnitEditHashes);
			Next->ProjectionIdentity = Layout->GetProjectionIdentity();
			Next->UnitContentFiles = MoveTemp(UnitContentFiles);
			if (bRetainPublicationDirectory) {
				Next->GeneratedMachineSpans = MakeShared<const TArray<TPair<FIntPoint, FIntPoint>>>(MoveTemp(GeneratedMachineSpans));
				auto PublishedRows = MakeShared<FGeneratedPublicationRows>(*Model, *Layout, bScopedMachineBuckets, Next->GeneratedMachineSpans);
				PublishedRows->Inventory = Next->Inventory;
				PublishedRows->Files = Next->Files;
				PublishedRows->RowHashes = MoveTemp(PublicationRowHashes);
				for (const auto& File : OutPlan->GetFiles())
					if (File.GetRelativePath().StartsWith(TEXT(".uai/baselines/"), ESearchCase::CaseSensitive))
						PublishedRows->BaselineFileHashes.Add(File.GetRelativePath(), File.GetContentHash());
				const auto Directory = FAuthoringModel::MakePublicationDirectory(*Model, PublishedRows);
				Next->SourceLayout = Layout->WithPublicationDirectory(Directory);
				Next->PublishedRows = MoveTemp(PublishedRows);
			} else Layout->RetireOptionalRetention();
			PreviousCompositionExport = MoveTemp(Next);
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=incremental_composition eligible=%s complete_layout=%s changed_properties=%d changed_value_items=%d changed_other=%d reused_documents=%d reused_inventory=%d compare_s=%.6f reused_unit_proofs=%d reused_schema_closures=%d reused_definition_reads=%d"),
			bReusableBaseline ? TEXT("true") : TEXT("false"), bIdenticalLayout ? TEXT("true") : TEXT("false"),
			ChangedPropertyUnits, ChangedValueItems, ChangedOtherUnits,
			ReusedDocuments, ReusedInventoryRows, ComparisonFinished - LayoutFinished,
			ReusedUnitContentProofs, ReusedSchemaClosures, ReusedDefinitionReads);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_proof_blocks reused=%d total=%d projection_changed_units=%d retained_bytes=%lld"),
			ReusedPlanBlocks, ReusableUnitBlocks.Num(), ChangedProjectionUnits.Num(), PlanBlockBytes);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_proof_consumer direct_publication=%s computed_key_rows=%d fixed_block_bytes=%lld definition_closure_only=%s"),
			bDirectPublication ? TEXT("true") : TEXT("false"), ComputedUnitContentKeyRows, PlanBlockBytes,
			bDirectPublication ? TEXT("true") : TEXT("false"));
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=composition_file_consumers proven_member_rows=%d content_directory_map=%s graph_property_map=%s planned_hash_reuses=%d rebuilt_content_hashes=%d scoped_definition_file_reuses=%d"),
			ProvenFileMemberRows, bContentDirectoriesBuilt ? TEXT("built") : TEXT("not_needed"), bPropertyPathsBuilt ? TEXT("built") : TEXT("not_needed"), ReusedPlannedHashes, RebuiltContentHashes, ScopedDefinitionFileReuses);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=inventory_document_handoff reused_rows=%d original_formatter_and_machine_spans=true"), ConsumedDocumentRowProofs);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=accepted_file_backing files=%d bytes=%lld generated_inventory_reuses=%d whole_file_formatter_not_used=true"),
			AcceptedFileReads, AcceptedFileReadBytes, AcceptedGeneratedByteReuses);
		const double PlanFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=export_plan kind=%s units=%d files=%d bytes=%lld layout_s=%.6f documents_s=%.6f inventory_s=%.6f navigation_s=%.6f examples_s=%.6f manifest_s=%.6f finalize_s=%.6f total_s=%.6f success=true"),
			*TargetKind, Model->GetUnitCount(), OutPlan->GetFiles().Num(), OutputBytes,
			LayoutFinished - PlanStarted, DocumentsFinished - LayoutFinished,
			InventoryFinished - DocumentsFinished, ExamplesStarted - InventoryFinished,
			ExamplesFinished - ExamplesStarted, ManifestFinished - ExamplesFinished,
			PlanFinished - ManifestFinished, PlanFinished - PlanStarted);
		return Model->CheckValueReadResult(OutError);
	}

	// The full allocator groups each kind of owner document within its real
	// content directory, including documents owned by transparent surfaces.
	static bool GatherOwnerDocumentPlacementContext(const FAuthoringModel& Previous, const FAuthoringModel& Current,
		const FCommittedWorkspaceDelta& Changes, TSet<FString>& Context, TSet<FString>& Written, FWorkspaceError& Error)
	{
		using FDeclaration = FSemanticUnit::FDeclarationView;
		const auto InheritsDirectory = [](const FDeclaration& Unit) {
			return GetAuthoringContentDirectoryRole(Unit.GetUnitKind(), Unit.GetSemanticTypeRef()) == EAuthoringContentDirectory::Inherited;
		};
		TMap<ESemanticUnitKind, TSet<FString>> Directories;
		const auto AddDirectory = [&](const FAuthoringModel& Model, const FDeclaration& Document) {
			const ESemanticUnitKind Kind = Document.GetUnitKind();
			if (Kind != ESemanticUnitKind::OrderingState && Kind != ESemanticUnitKind::RelationSet) return true;
			auto Owner = Model.FindDeclaration(Document.GetOwnerRef());
			FString Directory;
			while (Owner) {
				if (IsGraphStructureUnit(*Owner)) return true;
				if (Directory.IsEmpty() && (!InheritsDirectory(*Owner) || Owner->GetOwnerRef().IsEmpty())) Directory = Owner->GetUnitId();
				if (Owner->GetOwnerRef().IsEmpty()) break;
				Owner = Model.FindDeclaration(Owner->GetOwnerRef());
			}
			if (!Owner || Directory.IsEmpty()) {
				if (Model.CheckValueReadResult(Error)) {
					if (Kind == ESemanticUnitKind::OrderingState) Error.SetSourceFailure(TEXT("composition_ordering_owner_missing"),
						TEXT("The observed ordering cannot resolve its content directory owner."), Document.GetUnitId());
					else Error.SetSourceFailure(TEXT("composition_relation_owner_missing"),
						TEXT("The observed relation cannot resolve its content directory owner."), Document.GetUnitId());
				}
				return false;
			}
			Directories.FindOrAdd(Kind).Add(Directory);
			return true;
		};
		for (const auto& Change : Changes.Changes) {
			if (Change.Before && !AddDirectory(Previous, Change.Before->GetDeclaration())) return false;
		}
		for (const FString& Id : Written) {
			const auto Unit = Current.FindDeclaration(Id);
			if (Unit && !AddDirectory(Current, *Unit)) return false;
		}
		const auto Collect = [&](const FAuthoringModel& Model, const FString& Directory,
			ESemanticUnitKind Kind, TSet<FString>& Out) {
			const auto Anchor = Model.FindDeclaration(Directory);
			if (!Anchor) return Model.CheckValueReadResult(Error);
			const auto Include = [&](const FDeclaration& Owner) {
				if (Kind == ESemanticUnitKind::RelationSet) {
					for (const FString& Id : Owner.GetRelationRefs()) Out.Add(Id);
				} else if (!Owner.GetOrderingRef().IsEmpty() && !IsInternalAuthoringStructureSurface(
					Owner.GetUnitKind(), Owner.GetSemanticTypeRef(), Owner.GetValueCount()))
					Out.Add(Owner.GetOrderingRef());
			};
			Include(*Anchor);
			const auto IncludeSurface = [&](const FString& Id) {
				const auto Surface = Model.FindDeclaration(Id);
				if (Surface && Surface->GetOwnerRef() == Directory && InheritsDirectory(*Surface)) Include(*Surface);
			};
			if (Anchor->GetUnitKind() == ESemanticUnitKind::TargetState) {
				FTargetCompositionContract Contract;
				if (!FTargetCompositionRegistry::Get().FindContract(Anchor->GetSemanticTypeRef(), Contract)) {
					if (Kind == ESemanticUnitKind::OrderingState) Error.SetSourceFailure(TEXT("composition_ordering_surface_contract_missing"),
						TEXT("The current ordering directory requires its registered target surfaces."), Directory);
					else Error.SetSourceFailure(TEXT("composition_relation_surface_contract_missing"),
						TEXT("The current relation directory requires its registered target surfaces."), Directory);
					return false;
				}
				for (const auto& Surface : Contract.Surfaces)
					IncludeSurface(SurfaceUnitId(Contract.TargetKind, Surface.SurfaceId));
			} else {
				for (const FString& Id : Anchor->GetItemRefs()) IncludeSurface(Id);
			}
			return Model.CheckValueReadResult(Error);
		};
		for (const auto& Group : Directories) for (const FString& Directory : Group.Value) {
			TSet<FString> Before, After;
			if (!Collect(Previous, Directory, Group.Key, Before) || !Collect(Current, Directory, Group.Key, After)) return false;
			Context.Append(After);
			// Only existing peers crossing the single/shared boundary change
			// their own address. Newly added/removed records are already actuals.
			if ((Before.Num() > 1) != (After.Num() > 1))
				for (const FString& Id : After) if (Before.Contains(Id)) Written.Add(Id);
		}
		return Previous.CheckValueReadResult(Error) && Current.CheckValueReadResult(Error);
	}

// Changing a surviving item's physical segment or its actual owner invalidates
// the same descendant placements. Only the segment depends on the graph's aliases.
static void GatherChangedGraphNodeSegments(const FSemanticUnit& Previous, const FSemanticUnit& Current,
	TSet<FString>& ChangedNativeSegments)
{
	const auto* OldNodes = Previous.FindValue(TEXT("nodes"));
	const auto* NewNodes = Current.FindValue(TEXT("nodes"));
	if (!OldNodes || !NewNodes || &OldNodes->Get() == &NewNodes->Get()
		|| (*OldNodes)->GetKind() != EUeAgentValueKind::Object || (*NewNodes)->GetKind() != EUeAgentValueKind::Object) return;
	const auto NativeId = [](const FUeAgentCanonicalValueRef& Node) -> const FString* {
		for (const TCHAR* Key : {TEXT("id"), TEXT("builtin_id")}) {
			const auto* Id = FindObjectField(Node, Key);
			if (Id && (*Id)->GetKind() == EUeAgentValueKind::String && !(*Id)->GetText().IsEmpty())
				return &(*Id)->GetText();
		}
		return nullptr;
	};
	TMap<FString, FString> OldNames;
	for (const auto& Node : (*OldNodes)->GetObjectItems())
		if (const auto* Id = NativeId(Node.Value)) OldNames.Add(*Id, Node.Key);
	for (const auto& Node : (*NewNodes)->GetObjectItems()) {
		const auto* Id = NativeId(Node.Value);
		const auto* Old = Id ? OldNames.Find(*Id) : nullptr;
		if (Old && !MakeAuthoringContentPathSegment(*Old).Equals(
			MakeAuthoringContentPathSegment(Node.Key), ESearchCase::CaseSensitive))
			ChangedNativeSegments.Add(*Id);
	}
}

static bool TryBuildScopedCommittedPublication(FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& Binding, TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources, FExportWorkspacePlanPtr& OutPlan, FWorkspaceError& Error,
		const FString& TargetKind, const FPreparedWorkspaceExport& Settings,
		TMap<FString, TPair<FString, FString>>* PendingLocations, const TSet<FString>* RewriteRows = nullptr,
		const TSet<FString>* FullRebuildDocuments = nullptr, bool bPublishGuides = true)
	{
		const double Started = FPlatformTime::Seconds();
		const FString Root = Settings.AcceptedWorkspaceRoot.IsEmpty() ? GetAcceptedSemanticWorkspaceRoot() : Settings.AcceptedWorkspaceRoot;
		const auto Inventory = GetAcceptedWorkspaceInventory(Root);
		const auto Source = GetAcceptedSemanticWorkspace(Root);
		if (!Inventory || !Source) {
			Error.SetSourceFailure(TEXT("workspace_publication_directory_unavailable"), TEXT("The accepted publication's required row directory is unavailable."), Root); return false;
		}
		FCompositionOutputScope Scope;
		Scope.bPublishGuides = bPublishGuides;
		if (RewriteRows) Scope.Rows.Append(*RewriteRows);
		struct FLocation { FString Document, Pointer, Envelope; TArray<FString> Content; };
		TMap<FString, FLocation> Before;
		TSet<FString> ChangedIds, DeletedIds, AddedIds;
		TSet<FString> ChangedDefinitions;
		TSet<FString> PreviousGraphInterfaces;
		// The folded delta compares this destination's original and final declaration.
		// These identities already have their owner facts; no graph value read is needed.
		TSet<FString> ChangedItemOwnerIds;
		TSet<FString> ChangedNamedDirectories;
		for (const auto& Change : Settings.ActualChanges->Changes) {
			ChangedIds.Add(Change.UnitId);
			if (!Change.Before && Change.After) AddedIds.Add(Change.UnitId);
			if (Change.After) Scope.Rows.Add(Change.UnitId); else DeletedIds.Add(Change.UnitId);
			if (Change.Before && Change.After && Change.After->GetUnitKind() == ESemanticUnitKind::ItemState
				&& Change.Before->GetOwnerRef() != Change.After->GetOwnerRef())
				ChangedItemOwnerIds.Add(Change.UnitId);
			// The stored order contains canonical IDs. Its public text also
			// depends on the current handles of its surviving members.
			if (Change.Before && Change.After && (Change.Before->GetOwnerRef() != Change.After->GetOwnerRef()
				|| !HaveSameSemanticProjectionIdentity(*Change.After, *Change.Before))) {
				if (GetAuthoringContentDirectoryRole(Change.Before->GetUnitKind(), Change.Before->GetSemanticTypeRef()) == EAuthoringContentDirectory::NamedChild)
					ChangedNamedDirectories.Add(Change.UnitId);
				const auto Owner = Model->FindDeclaration(Change.After->GetOwnerRef());
				if (Owner && Owner->GetItemRefs().Contains(Change.UnitId) && !Owner->GetOrderingRef().IsEmpty()) {
					const auto Ordering = Model->FindDeclaration(Owner->GetOrderingRef());
					if (Ordering && Ordering->GetOwnerRef() == Owner->GetUnitId()
						&& Ordering->GetUnitKind() == ESemanticUnitKind::OrderingState
						&& Ordering->GetRole() == TEXT("stable_item_order")
						&& Ordering->GetSemanticTypeRef() == TEXT("uai.authoring.ordering/1"))
						Scope.Rows.Add(Ordering->GetUnitId());
				}
			}
			const auto* Unit = Change.After ? &Change.After.GetValue() : Change.Before ? &Change.Before.GetValue() : nullptr;
			if (Unit && Unit->GetUnitKind() == ESemanticUnitKind::CollectionState && Unit->GetRole() == TEXT("property_definition")) ChangedDefinitions.Add(Change.UnitId);
			if (Change.Before && !Inventory->VisitUnit(Change.UnitId, [&](const FAcceptedWorkspaceInventoryRow& Row) {
				Before.Add(Change.UnitId, {Row.DocumentPath, Row.UnitPointer, Row.EnvelopePath, Row.ContentFiles});
				if (IsGraphStructureUnit(*Change.Before)) PreviousGraphInterfaces.Add(GraphInterfaceDocumentPath(Row.DocumentPath));
				Scope.ReplacedPointers.FindOrAdd(Row.DocumentPath).AddUnique(Row.UnitPointer);
				Scope.ReplacedDocuments.Add(Row.DocumentPath); return true;
			})) { Model->CheckValueReadResult(Error); if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("workspace_captured_unit_unavailable"), TEXT("The changed published row has no accepted address."), Change.UnitId); return false; }
		}
		if (Source->GetEditedUnitIds().IsSet()) for (const auto& Id : *Source->GetEditedUnitIds()) {
			if (DeletedIds.Contains(Id) || Settings.PendingAuthorUnits.Contains(Id) || Settings.PendingKeyedAuthorRows.Contains(Id)) continue;
			if (Model->FindDeclaration(Id)) Scope.Rows.Add(Id);
		}
		TSet<FString> OwnerDocumentContext;
		if (!GatherOwnerDocumentPlacementContext(Source->GetModel().Get(), Model.Get(), *Settings.ActualChanges,
			OwnerDocumentContext, Scope.Rows, Error)) return false;
		const auto RememberPending = [&](const FString& Id) {
			return Inventory->VisitUnit(Id, [&](const FAcceptedWorkspaceInventoryRow& Row) {
				if (PendingLocations) PendingLocations->Add(Id, {Row.DocumentPath, Row.UnitPointer}); return true;
			});
		};
		for (const auto& Pending : Settings.PendingAuthorUnits) if (!RememberPending(Pending.Key)) return false;
		for (const auto& Pending : Settings.PendingKeyedAuthorRows) if (!RememberPending(Pending.Key)) return false;
		if (Scope.Rows.IsEmpty() && Settings.ActualChanges->Changes.IsEmpty())
			return TryBuildCommittedPublicationHeader(Binding, TargetKind, Settings, Resources, OutPlan, Error);
		TMap<FString, FString> PublishedFiles; ArtifactPublisher::FArtifactPublishError PublishError;
		if (!TryReadPublishedWorkspaceFileHashes(Root, CompositionPublisherOwner(TargetKind, &Settings), PublishedFiles, PublishError, true)) {
			Error.SetFailure(MoveTemp(PublishError)); return false;
		}
		// A surviving property's value can stay unchanged while its named owner's directory moves.
		// Select its accepted document rows. The ordinary scoped writer computes their final addresses.
		const TCHAR* PropertyFiles[] = {PropertyDocumentName::Properties, PropertyDocumentName::Inputs, PropertyDocumentName::SlotProperties};
		const FAuthoringModel& PreviousModel = Source->GetModel().Get();
		for (const FString& Id : ChangedNamedDirectories) {
			FString Directory;
			if (!TryGetAcceptedNamedChildDirectory(PreviousModel, *Inventory, Id, Directory, Error)) return false;
			if (Directory.IsEmpty()) continue;
			for (const TCHAR* Name : PropertyFiles) {
				const FString Path = Directory / Name;
				if (!PublishedFiles.Contains(Path)) continue;
				if (!Inventory->VisitDocument(Path, [&](const FAcceptedWorkspaceInventoryRow& Row) {
					if (Row.UnitKind != ESemanticUnitKind::PropertySet || DeletedIds.Contains(Row.UnitId)) return true;
					auto Owner = PreviousModel.FindDeclaration(Row.UnitId);
					while (Owner && GetAuthoringContentDirectoryRole(Owner->GetUnitKind(), Owner->GetSemanticTypeRef()) == EAuthoringContentDirectory::Inherited)
						Owner = PreviousModel.FindDeclaration(Owner->GetOwnerRef());
					if (!Owner) return false;
					if (Owner->GetUnitId() == Id) Scope.Rows.Add(Row.UnitId);
					return true;
				})) {
					if (!PreviousModel.CheckValueReadResult(Error)) return false;
					Error.SetSourceFailure(TEXT("workspace_captured_unit_unavailable"), TEXT("The moved directory's accepted property membership is unavailable."), Path);
					return false;
				}
			}
		}
		for (const auto& Id : Scope.Rows) if (!AddedIds.Contains(Id) && !Before.Contains(Id)) {
			if (!Inventory->VisitUnit(Id, [&](const FAcceptedWorkspaceInventoryRow& Row) {
				Before.Add(Id, {Row.DocumentPath, Row.UnitPointer, Row.EnvelopePath, Row.ContentFiles});
				if (Row.UnitKind == ESemanticUnitKind::StructureState) {
					const auto Previous = Source->GetModel()->FindDeclaration(Id);
					if (Previous && IsGraphStructureUnit(*Previous)) PreviousGraphInterfaces.Add(GraphInterfaceDocumentPath(Row.DocumentPath));
				}
				Scope.ReplacedPointers.FindOrAdd(Row.DocumentPath).AddUnique(Row.UnitPointer);
				Scope.ReplacedDocuments.Add(Row.DocumentPath); return true;
			})) { Model->CheckValueReadResult(Error); return false; }
		}
		for (const auto& Row : Before) for (const auto& Path : Row.Value.Content) Scope.ReplacedContentPaths.Add(Path);
		// Only the written property/type rows contribute guide recipes. Owner
		// declarations also carry catalog references for naming/serialization;
		// those references are not a request to render the unchanged catalog.
		if (bPublishGuides && !TryCollectScopedGuideDefinitions(*Model, TargetKind, Scope.Rows, Scope.GuideDefinitions, Error)) return false;
		TArray<FString> ContextIds = Scope.Rows.Array(); TSet<FString> ContextSet(Scope.Rows);
		const auto AddContext = [&](const FString& Id) {
			if (!Id.IsEmpty() && !ContextSet.Contains(Id)) { ContextSet.Add(Id); ContextIds.Add(Id); }
		};
		for (const FString& Id : Scope.GuideDefinitions) AddContext(Id);
		for (const FString& Id : OwnerDocumentContext) AddContext(Id);
	// Rewriting an aggregate rewrites every visible member's definition link.
	// The binding and field-set name are declaration inputs, not member value writes.
	for (const FString& Id : Scope.Rows) {
		const auto Declaration = Model->FindDeclaration(Id);
		if (!Declaration || !IsKeyedItemStructureUnit(*Declaration)) continue;
		for (const auto* Contract : DocumentCollectionContracts(*Declaration, TargetKind)) {
			if (Contract->AggregatedItemSemanticSuffix.IsEmpty()) continue;
			const auto* Unit = Model->FindUnit(Id);
			if (!Unit) { if (!Model->CheckValueReadResult(Error)) return false; continue; }
			const auto* Items = Unit->FindValue(Contract->RootField);
			if (!Items || (*Items)->GetKind() != EUeAgentValueKind::Object) continue;
			for (const auto& Row : (*Items)->GetObjectItems()) {
				FString FieldSet;
				if (!TryGetAggregateMemberFieldSet(*Model, Row.Value, FieldSet, Error)) return false;
				AddContext(FieldSet);
			}
		}
	}
		// A graph document rewrites the visible node map as a unit. The current
		// graph declaration owns membership; an old file can only supply placement
		// metadata for properties whose current owner is a surviving graph item.
		TSet<FString> GraphMembers;
		TSet<FString> MovedGraphMembers;
		TArray<FString> GraphDirectories;
		for (const auto& Id : Scope.Rows) {
			const auto Graph = Model->FindDeclaration(Id);
			if (!Graph || !IsGraphStructureUnit(*Graph)) continue;
			TSet<FString> ChangedNativeSegments;
			if (!AddedIds.Contains(Id)) {
				const auto* Old = Source->FindBaselineUnit(Id);
				const auto* Current = Model->FindUnit(Id);
				if (!Old || !Current) {
					if (!Model->CheckValueReadResult(Error)) return false;
					Error.SetSourceFailure(TEXT("composition_output_unit_unavailable"),
						TEXT("The graph navigation requires its previous and current node bindings."), Id);
					return false;
				}
				GatherChangedGraphNodeSegments(*Old, *Current, ChangedNativeSegments);
			}
		for (const FString& Member : Graph->GetItemRefs()) AddContext(Member);
		TArray<FString> Parents{Id};
		for (int32 Index = 0; Index < Parents.Num(); ++Index) {
			const FString ParentId = Parents[Index];
			const auto Parent = Model->FindDeclaration(ParentId);
			for (const FString& MemberId : Parent->GetItemRefs()) {
				const auto Member = Model->FindDeclaration(MemberId);
				if (!Member) {
					if (!Model->CheckValueReadResult(Error)) return false;
					Error.SetSourceFailure(TEXT("composition_output_unit_unavailable"),
						TEXT("The current graph item's declared child has no accepted actual/publication fact."), MemberId);
					return false;
				}
				if (Member->GetUnitKind() != ESemanticUnitKind::ItemState || Member->GetOwnerRef() != ParentId) continue;
				if (ResolveGraphModelItemOwner(Model.Get(), Member->GetSemanticTypeRef(), ParentId) == Id) {
					GraphMembers.Add(MemberId);
					AddContext(MemberId);
					bool bPlacementChanged = ChangedItemOwnerIds.Contains(MemberId);
					if (!bPlacementChanged && !ChangedNativeSegments.IsEmpty()) {
						const auto* Node = Model->FindUnit(MemberId);
						if (!Node) { Model->CheckValueReadResult(Error); return false; }
						bPlacementChanged = ChangedNativeSegments.Contains(ReadGraphNodeNativeIdentity(Node->GetValues()));
					}
					if (bPlacementChanged) MovedGraphMembers.Add(MemberId);
				}
				if (!Member->GetItemRefs().IsEmpty()) Parents.Add(MemberId);
			}
		}
			if (const auto* Location = Before.Find(Id))
				GraphDirectories.AddUnique(FPaths::GetBaseFilename(Location->Document, false) + TEXT("/"));
		}
		if (!GraphMembers.IsEmpty() && !GraphDirectories.IsEmpty()) {
			int32 PropertyDocuments = 0;
			for (const auto& File : PublishedFiles) {
				if (!File.Key.EndsWith(TEXT("/properties.hjson"), ESearchCase::CaseSensitive)
					|| !GraphDirectories.ContainsByPredicate([&](const FString& Directory) {
						return File.Key.StartsWith(Directory, ESearchCase::CaseSensitive);
					})) continue;
				TSet<FString> PlacedOwners;
				if (!Inventory->VisitDocument(File.Key, [&](const FAcceptedWorkspaceInventoryRow& Row) {
					if (Row.UnitKind != ESemanticUnitKind::PropertySet || DeletedIds.Contains(Row.UnitId)) return true;
					const auto Property = Model->FindDeclaration(Row.UnitId);
					if (!Property) {
						if (!Model->CheckValueReadResult(Error)) return false;
						Error.SetSourceFailure(TEXT("composition_graph_property_location_missing"),
							TEXT("The published graph property has no current declaration or observed deletion."), Row.UnitId);
						return false;
					}
					const FString& Owner = Property->GetOwnerRef();
					if (!MovedGraphMembers.IsEmpty()) {
						auto Ancestor = Model->FindDeclaration(Owner);
						while (Ancestor && Ancestor->GetUnitKind() == ESemanticUnitKind::ItemState) {
							if (MovedGraphMembers.Contains(Ancestor->GetUnitId())) {
								if (!Before.Contains(Row.UnitId)) {
									Before.Add(Row.UnitId, {Row.DocumentPath, Row.UnitPointer, Row.EnvelopePath, Row.ContentFiles});
									Scope.ReplacedPointers.FindOrAdd(Row.DocumentPath).AddUnique(Row.UnitPointer);
									Scope.ReplacedDocuments.Add(Row.DocumentPath);
									for (const FString& Path : Row.ContentFiles) Scope.ReplacedContentPaths.Add(Path);
								}
								Scope.Rows.Add(Row.UnitId);
								AddContext(Row.UnitId);
								break;
							}
							Ancestor = Model->FindDeclaration(Ancestor->GetOwnerRef());
						}
					}
					if (!GraphMembers.Contains(Owner) || Property->GetSemanticTypeRef() == TEXT("uai.semantic.umg.slot.property")) return true;
					// All rows in this body share their placement, but each native
					// naming field is a real input to the existing alias allocator.
					if (!PlacedOwners.Contains(Owner) || IsGraphNodeNativeNameProperty(Property->GetFieldSetRef())) AddContext(Row.UnitId);
					PlacedOwners.Add(Owner);
					return true;
				})) {
					if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("composition_graph_property_location_missing"),
						TEXT("The current graph formatter could not read its accepted property document membership."), File.Key);
					return false;
				}
				++PropertyDocuments;
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=scoped_graph_navigation members=%d property_documents=%d naming_context=%d"),
				GraphMembers.Num(), PropertyDocuments, ContextIds.Num());
		}
		for (int32 Index = 0; Index < ContextIds.Num(); ++Index) {
			const auto Declaration = Model->FindDeclaration(ContextIds[Index]); if (!Declaration) continue;
			const auto Add = [&](const FString& Id) {
				if (!Id.IsEmpty() && !ContextSet.Contains(Id) && Model->FindDeclaration(Id)) { ContextSet.Add(Id); ContextIds.Add(Id); }
			};
			Add(Declaration->GetOwnerRef());
			for (const auto& Dependency : Declaration->GetDependencyRefs()) Add(Dependency);
			if (const auto FieldSet = FindSemanticFieldSetDeclaration(*Declaration, *Model)) Add(FieldSet->GetUnitId());
		}
		FAuthoringModelPtr ScopedModel;
		if (!FAuthoringModel::TryMakeOutputScope(Model, ContextIds, ScopedModel, Error)) return false;
		Scope.PublishedFileHashes = &PublishedFiles; // Synchronous formatter call below.
		const auto ReadCanonical = [&](const FString& Path, FUeAgentCanonicalValuePtr& Value) {
			FHjsonDocumentPtr Parsed;
			if (Path.StartsWith(TEXT("examples/"), ESearchCase::CaseSensitive)) {
				const auto* Expected = PublishedFiles.Find(Path);
			return Expected && TryReadPublishedCanonicalDocument(Root, Path, *Expected, Resources, Value, Error);
		}
		if (!TryReadAcceptedSemanticSourceDocument(Root, Path, Resources, Parsed, Error)) return false;
			return TryProjectHjsonCanonicalValue(*Parsed->GetRoot(), Resources, Value, Error, false);
		};
		if (bPublishGuides && PublishedFiles.Contains(TEXT("examples/index.hjson"))) {
			FUeAgentCanonicalValuePtr Canonical; TSharedPtr<FJsonValue> Json; Contract::FDiagnostic Diagnostic;
			if (!ReadCanonical(TEXT("examples/index.hjson"), Canonical)
				|| !CanonicalJson::TryToJsonValue(*Canonical, Json, Diagnostic) || !Json || !Json->AsObject()) {
				if (!Error.HasErrors()) Error.SetFailure(MoveTemp(Diagnostic), TEXT("examples/index.hjson")); return false;
			}
			Scope.PreviousExampleIndex = Json->AsObject();
			// External content belongs to one formatted row and is replaced as a whole.
			// Retire its old guide mapping and recipes with that row's physical files.
			for (const auto& Path : Scope.ReplacedContentPaths) {
				if (!PublishedFiles.Contains(TEXT("examples/files/") + Path)) continue;
				Scope.ReplacedDocuments.Add(Path);
				Scope.ReplacedPointers.FindOrAdd(Path).AddUnique(TEXT("/"));
			}
		}
		FExportWorkspacePlanPtr UnusedPlan;
		if (!TryBuildCompositionExportPlanInternal(ScopedModel.ToSharedRef(), Binding, MoveTemp(Telemetry), Resources,
			UnusedPlan, Error, TargetKind, &Settings, PendingLocations, &Scope)) return false;
		const auto& Layout = *Scope.Layout;
		TMap<FString, FUeAgentCanonicalValuePtr> Documents;
		TSet<FString> RemovedFiles, Buckets;
		const auto GetDocument = [&](const FString& Path) -> FUeAgentCanonicalValuePtr* {
			if (auto* Found = Documents.Find(Path)) return Found;
			FUeAgentCanonicalValuePtr Value;
			if (PublishedFiles.Contains(Path)) { if (!ReadCanonical(Path, Value)) return nullptr; }
			else { FUeAgentStateSemanticError Failure; if (!FUeAgentCanonicalValue::TryObject({}, Resources, Value, Failure)) { Error.SetFailure(MoveTemp(Failure), Path); return nullptr; } }
			return &Documents.Add(Path, MoveTemp(Value));
		};
		const auto AtPointer = [](const FUeAgentCanonicalValueRef& RootValue, const FString& Pointer) -> FUeAgentCanonicalValuePtr {
			FUeAgentCanonicalValuePtr Value = RootValue; TArray<FString> Segments; Pointer.ParseIntoArray(Segments, TEXT("/"), true);
			for (auto Segment : Segments) {
				Segment.ReplaceInline(TEXT("~1"), TEXT("/")); Segment.ReplaceInline(TEXT("~0"), TEXT("~"));
				if (!Value || Value->GetKind() != EUeAgentValueKind::Object) return nullptr;
				const auto* Found = Value->GetObjectItems().FindByPredicate([&](const auto& Field) { return Field.Key == Segment; });
				if (!Found) return nullptr; Value = Found->Value;
			} return Value;
		};
		const auto RemoveAt = [&](FUeAgentCanonicalValuePtr& Value, const FString& Pointer, const FString& Path) {
			TArray<FString> Segments; Pointer.ParseIntoArray(Segments, TEXT("/"), true);
			for (auto& Segment : Segments) { Segment.ReplaceInline(TEXT("~1"), TEXT("/")); Segment.ReplaceInline(TEXT("~0"), TEXT("~")); }
			TFunction<bool(const FUeAgentCanonicalValueRef&, int32, FUeAgentCanonicalValuePtr&)> Remove;
			Remove = [&](const FUeAgentCanonicalValueRef& Node, int32 Depth, FUeAgentCanonicalValuePtr& Result) {
				if (Depth >= Segments.Num() || Node->GetKind() != EUeAgentValueKind::Object) return false;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
				for (const auto& Field : Node->GetObjectItems()) {
					if (Field.Key != Segments[Depth]) { Fields.Add(Field); continue; }
					if (Depth + 1 == Segments.Num()) continue;
					FUeAgentCanonicalValuePtr Child; if (!Remove(Field.Value, Depth + 1, Child)) return false;
					if (Child->GetKind() != EUeAgentValueKind::Object || !Child->GetObjectItems().IsEmpty()) Fields.Emplace(Field.Key, Child.ToSharedRef());
				}
				FUeAgentStateSemanticError Failure;
				if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Result, Failure)) { Error.SetFailure(MoveTemp(Failure), Path); return false; }
				return true;
			};
			FUeAgentCanonicalValuePtr Changed;
			if (!Remove(Value.ToSharedRef(), 0, Changed)) return false;
			Value = MoveTemp(Changed); return true;
		};
		for (const auto& Old : Before) {
			Buckets.Add(Old.Value.Envelope);
			const auto* NewPath = Layout.GetPhysicalPlacements().Find(Old.Key);
			const auto* NewPointer = Scope.Pointers.Find(Old.Key);
			if (NewPath && NewPointer && *NewPath == Old.Value.Document && *NewPointer == Old.Value.Pointer) continue;
			if (FullRebuildDocuments && FullRebuildDocuments->Contains(Old.Value.Document)) {
				RemovedFiles.Add(Old.Value.Document); continue; // Complete current members replace this document below.
			}
			if (Old.Value.Pointer == TEXT("/") || Old.Value.Pointer.IsEmpty()) { RemovedFiles.Add(Old.Value.Document); continue; }
			auto* Document = GetDocument(Old.Value.Document);
			if (!Document || !RemoveAt(*Document, Old.Value.Pointer, Old.Value.Document)) return false;
		}
		for (const auto& Row : Scope.MachineRows) {
			const FString& Path = Layout.GetPhysicalPlacements().FindChecked(Row.Key);
			const FString& Pointer = Scope.Pointers.FindChecked(Row.Key);
			Scope.ReplacedDocuments.Add(Path); RemovedFiles.Remove(Path);
			Buckets.Add(MachineEnvelopePath(Path, Inventory->GetScopedMachineBuckets().Get(false)));
			const auto* Produced = Scope.Documents.Find(Path);
			if (!Produced) { Error.SetSourceFailure(TEXT("composition_unit_document_missing"), TEXT("The formatter did not produce the changed row's public document."), Row.Key); return false; }
			const auto Visible = AtPointer(*Produced, Pointer);
			if (!Visible) { Error.SetSourceFailure(TEXT("composition_unit_document_missing"), TEXT("The formatter did not produce the changed row's public address."), Path, Pointer); return false; }
			if (!PublishedFiles.Contains(Path) || Pointer == TEXT("/") || (FullRebuildDocuments && FullRebuildDocuments->Contains(Path))) Documents.Add(Path, *Produced);
			else {
				auto* Document = GetDocument(Path); if (!Document) return false;
				FUeAgentCanonicalValuePtr Changed;
				if (!TryReplacePublishedObject(Document->ToSharedRef(), Pointer, Visible.ToSharedRef(), Resources, Changed, Error, Path, true)) return false;
				*Document = MoveTemp(Changed);
			}
			if (PendingLocations) PendingLocations->Add(Row.Key, {Path, Pointer});
		}
		// The public enum catalog is keyed by the same definition name. Patch
		// exactly that companion entry beside its changed definition, retaining
		// all other published rows/enum candidates without rendering them.
		for (const auto& Id : ChangedDefinitions) {
			const auto EnumPointer = [](const FString& Pointer) { return TEXT("/enums/") + Pointer.Mid(13); };
			if (const auto* Old = Before.Find(Id); Old && Old->Pointer.StartsWith(TEXT("/definitions/"), ESearchCase::CaseSensitive)) {
				auto* Document = GetDocument(Old->Document);
				if (!Document || !RemoveAt(*Document, EnumPointer(Old->Pointer), Old->Document)) return false;
			}
			const auto* Path = Layout.GetPhysicalPlacements().Find(Id);
			const auto* Pointer = Scope.Pointers.Find(Id);
			if (!Path || !Pointer || !Pointer->StartsWith(TEXT("/definitions/"), ESearchCase::CaseSensitive)) continue;
			const auto* Produced = Scope.Documents.Find(*Path); if (!Produced) continue;
			const FString Address = EnumPointer(*Pointer);
			const auto Entry = AtPointer(*Produced, Address);
			if (Entry) {
				auto* Document = GetDocument(*Path); if (!Document) return false;
				FUeAgentCanonicalValuePtr Changed;
				if (!TryReplacePublishedObject(Document->ToSharedRef(), Address, Entry.ToSharedRef(), Resources, Changed, Error, *Path, true)) return false;
				*Document = MoveTemp(Changed);
			}
		}
		for (auto& Document : Documents) if (Document.Key.EndsWith(TEXT("/definitions.hjson"), ESearchCase::CaseSensitive)
			&& !AtPointer(Document.Value.ToSharedRef(), TEXT("/enums"))) {
			FUeAgentCanonicalValuePtr Empty, Changed; FUeAgentStateSemanticError Failure;
			if (!FUeAgentCanonicalValue::TryObject({}, Resources, Empty, Failure)) { Error.SetFailure(MoveTemp(Failure), Document.Key); return false; }
			if (!TryReplacePublishedObject(Document.Value.ToSharedRef(), TEXT("/enums"), Empty.ToSharedRef(), Resources, Changed, Error, Document.Key, true)) return false;
			Document.Value = MoveTemp(Changed);
		}
		// Interfaces have no editable inventory row. Their publication follows
		// their graph document, including when its last interface disappears.
		for (const auto& File : Scope.Files) PreviousGraphInterfaces.Remove(File.GetRelativePath());
		for (const auto& Path : PreviousGraphInterfaces)
			if (PublishedFiles.Contains(Path)) RemovedFiles.Add(Path);
		for (const auto& Old : Before) for (const auto& Path : Old.Value.Content) {
			bool bStillUsed = false;
			for (const auto& Content : Scope.ContentFiles) bStillUsed |= Content.Value.Contains(Path);
			if (!bStillUsed) RemovedFiles.Add(Path);
		}
		for (const auto& Old : Before) {
			if (!Documents.Contains(Old.Value.Document) || RemovedFiles.Contains(Old.Value.Document)) continue;
			int32 Remaining = 0;
			if (!Inventory->VisitDocument(Old.Value.Document, [&](const FAcceptedWorkspaceInventoryRow& Row) {
				if (!ChangedIds.Contains(Row.UnitId) && !Scope.MachineRows.Contains(Row.UnitId)) ++Remaining; return true;
			})) return false;
			for (const auto& Row : Scope.MachineRows) if (Layout.GetPhysicalPlacements().FindChecked(Row.Key) == Old.Value.Document) ++Remaining;
			if (!Remaining) RemovedFiles.Add(Old.Value.Document);
		}
		// Shared recipe retirement is a real negative-reference consumer, not a
		// reason to reconstruct native/model values. Start with recipes from K
		// replaced guides, then stop as soon as remaining guides prove them used.
		TSet<FString> RetiredRecipeCandidates;
		TFunction<void(const FUeAgentCanonicalValueRef&, bool)> VisitRecipeRefs;
		VisitRecipeRefs = [&](const FUeAgentCanonicalValueRef& Value, bool bCollect) {
			if (Value->GetKind() == EUeAgentValueKind::String && Value->GetText().StartsWith(TEXT("examples/types/"), ESearchCase::CaseSensitive)) {
				if (bCollect) RetiredRecipeCandidates.Add(Value->GetText()); else RetiredRecipeCandidates.Remove(Value->GetText());
			} else if (Value->GetKind() == EUeAgentValueKind::Object) {
				for (const auto& Field : Value->GetObjectItems()) VisitRecipeRefs(Field.Value, bCollect);
			} else if (Value->GetKind() == EUeAgentValueKind::Array) for (const auto& Item : Value->GetArrayItems()) VisitRecipeRefs(Item, bCollect);
		};
		for (const auto& Guide : Scope.PreviousGuides) VisitRecipeRefs(Guide.Value, true);
		for (const auto& Guide : Scope.ProducedGuides) VisitRecipeRefs(Guide.Value, false);
		int32 UnchangedGuideReads = 0;
		if (Scope.PreviousExampleIndex) {
			const TSharedPtr<FJsonObject>* GuideFiles = nullptr;
			if (Scope.PreviousExampleIndex->TryGetObjectField(TEXT("files"), GuideFiles)) for (const auto& File : (*GuideFiles)->Values) {
				if (RetiredRecipeCandidates.IsEmpty()) break;
				if (Scope.ReplacedDocuments.Contains(File.Key)) continue;
				const FString Path = File.Value->AsString(); if (!PublishedFiles.Contains(Path)) continue;
				FUeAgentCanonicalValuePtr Guide; if (!ReadCanonical(Path, Guide)) return false;
				++UnchangedGuideReads; VisitRecipeRefs(Guide.ToSharedRef(), false);
			}
		}
		for (const auto& Path : RetiredRecipeCandidates) if (PublishedFiles.Contains(Path)) RemovedFiles.Add(Path);
		for (const auto& Path : Scope.ReplacedDocuments)
			if (!Scope.ProducedGuides.Contains(Path)) RemovedFiles.Add(TEXT("examples/files/") + Path);
		TArray<FExportFilePlan> Files;
		const auto AddCanonical = [&](const FString& Path, const FUeAgentCanonicalValueRef& Value) {
			TArray<uint8> Bytes; if (!TrySerializeSemanticHjsonObject(*Value, Bytes, Error, Path)) return false;
			const FString Hash = Sha256(Bytes); Files.Emplace(Path, MoveTemp(Bytes), Hash); return true;
		};
		for (const auto& Document : Documents) if (!RemovedFiles.Contains(Document.Key)) {
			const auto* Produced = Scope.Documents.Find(Document.Key);
			const auto* File = Produced && &Produced->Get() == Document.Value.Get()
				? Scope.Files.FindByPredicate([&](const auto& Candidate) { return Candidate.GetRelativePath() == Document.Key; }) : nullptr;
			if (File) Files.Add(*File);
			else if (!AddCanonical(Document.Key, Document.Value.ToSharedRef())) return false;
			Files.Last().SetSemanticSourceDocument(true);
		}
		// Only these physical buckets need complete membership. Unchanged rows
		// retain their original spans/bytes; they are not decoded into values.
		TSet<FString> RemovedShards, AddedShards;
		int32 PreservedRows = 0;
		for (const auto& Bucket : Buckets) {
			const FString Shard = Bucket.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
			TArray<uint8> OldCaptured;
			const bool bExisting = PublishedFiles.Contains(Bucket);
			if (bExisting && !TryReadPublishedWorkspaceBytes(Root, Bucket, PublishedFiles.FindChecked(Bucket), OldCaptured, Error)) return false;
			TArray<FUeAgentStructuredHjsonObjectFragment> Captured;
			TArray<FUeAgentCanonicalValueRef> Metadata;
			TArray<TSharedRef<TArray<uint8>>> OwnedFragments;
			auto Identities = MakeShared<FExportFilePlan::FFragmentIdentities>();
			if (bExisting && !Inventory->VisitBucket(Bucket, [&](const FAcceptedWorkspaceInventoryRow& Row) {
				if (ChangedIds.Contains(Row.UnitId) || Scope.MachineRows.Contains(Row.UnitId)) return true;
				const auto A = PublishedMachineSpanView(OldCaptured, Row.EnvelopeSpan, 1);
				if (A.IsEmpty()) {
					Error.SetSourceFailure(TEXT("workspace_publication_row_span_missing"),
						TEXT("The same-publication formatter did not hand off the retained member's spans. No whole-bucket parser or second writer was selected."), Row.UnitId);
					return false;
				}
				if (!Row.SharedUnitIdentity) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.machine_identity"),
					Row.UnitId, Bucket, TEXT("missing immutable row identity"), TEXT("same-row inventory identity"), TEXT("original row formatter"));
				Identities->Add(Row.SharedUnitIdentity.ToSharedRef());
				FUeAgentCanonicalValuePtr Thin;
				if (!TryBuildPublishedInventoryMetadata(Row, Resources, Thin, Error)) return false;
				Captured.Add({Row.UnitId, A, 1}); Metadata.Add(Thin.ToSharedRef()); ++PreservedRows; return true;
			})) return false;
			for (const auto& Row : Scope.MachineRows) {
				const FString& Path = Layout.GetPhysicalPlacements().FindChecked(Row.Key);
				if (MachineEnvelopePath(Path, Inventory->GetScopedMachineBuckets().Get(false)) != Bucket) continue;
				const int32 DeclarationIndex = Layout.GetModel()->FindUnitIndex(Row.Key);
				if (DeclarationIndex == INDEX_NONE) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.machine_identity"),
					Row.Key, Bucket, TEXT("missing formatter declaration"), TEXT("same-row identity"), TEXT("scoped formatter"));
				Identities->Add(Layout.GetModel()->GetDeclarations()[DeclarationIndex].GetSharedUnitId());
				auto Fields = Row.Value->GetObjectItems();
				const auto* Envelope = Fields.FindByPredicate([](const auto& Field) { return Field.Key == TEXT("envelope"); });
				if (!Envelope) return false; const auto CapturedValue = Envelope->Value;
				Fields.RemoveAll([](const auto& Field) { return Field.Key == TEXT("envelope"); });
				FUeAgentStateSemanticError Failure; FUeAgentCanonicalValuePtr Ref, ThinValue;
				if (!FUeAgentCanonicalValue::TryString(Bucket, Resources, Ref, Failure)) { Error.SetFailure(MoveTemp(Failure), Shard); return false; }
				Fields.Emplace(TEXT("envelope_ref"), Ref.ToSharedRef());
				if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, ThinValue, Failure)) { Error.SetFailure(MoveTemp(Failure), Shard); return false; }
				Metadata.Add(ThinValue.ToSharedRef());
				auto Bytes = MakeShared<TArray<uint8>>();
				if (!TrySerializeSemanticHjsonObject(*CapturedValue, *Bytes, Error, Bucket)) return false;
				Captured.Add({Row.Key, MakeArrayView(Bytes->GetData(), Bytes->Num() - 1), 0}); OwnedFragments.Add(Bytes);
			}
			if (Captured.IsEmpty()) { RemovedFiles.Add(Bucket); RemovedFiles.Add(Shard); RemovedShards.Add(Shard); continue; }
			TArray<uint8> A, B; TArray<FIntPoint> CapturedSpans, ThinSpans; Contract::FDiagnostic Failure;
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObjectFragments(Captured, FString(), A, Failure, &CapturedSpans)) { Error.SetFailure(MoveTemp(Failure), Bucket); return false; }
			if (!TrySerializePublishedInventoryWithEnvelopeSpans(Metadata, CapturedSpans, Resources, B, ThinSpans, Error, Shard)) return false;
			const FString HashA = Sha256(A), HashB = Sha256(B);
			Files.Emplace(Bucket, MoveTemp(A), HashA, MoveTemp(CapturedSpans), Identities);
			Files.Emplace(Shard, MoveTemp(B), HashB, MoveTemp(ThinSpans), Identities); AddedShards.Add(Shard);
		}
		const auto UpdateManifest = [&](const TSharedRef<FJsonObject>& Manifest) {
			TSet<FString> Paths; const TArray<TSharedPtr<FJsonValue>>* Old = nullptr;
			if (Manifest->TryGetArrayField(TEXT("unit_inventory_files"), Old)) for (const auto& Path : *Old) if (!RemovedShards.Contains(Path->AsString())) Paths.Add(Path->AsString());
			Paths.Append(AddedShards); TArray<FString> Ordered = Paths.Array(); Ordered.Sort(); Manifest->SetArrayField(TEXT("unit_inventory_files"), StringArray(Ordered));
			Manifest->SetField(TEXT("published_unit_count"), MakeShared<FJsonValueNumberString>(FString::FromInt(Model->GetUnitCount())));
			TSet<FString> Expanded; const TArray<TSharedPtr<FJsonValue>>* OldExpanded = nullptr;
			if (Manifest->TryGetArrayField(TEXT("expanded_author_documents"), OldExpanded))
				for (const auto& Path : *OldExpanded) if (!RemovedFiles.Contains(Path->AsString())) Expanded.Add(Path->AsString());
			for (const auto& Content : Scope.ContentFiles) if (!Content.Value.IsEmpty()) Expanded.Add(Layout.GetPhysicalPlacements().FindChecked(Content.Key));
			TArray<FString> ExpandedPaths = Expanded.Array(); ExpandedPaths.Sort(); Manifest->SetArrayField(TEXT("expanded_author_documents"), StringArray(ExpandedPaths));
		};
		TFunctionRef<void(const TSharedRef<FJsonObject>&)> UpdateManifestRef = UpdateManifest;
		FExportWorkspacePlanPtr Header;
		if (!TryBuildCommittedPublicationHeaderImpl(Binding, TargetKind, Settings, Resources, Header, Error, &UpdateManifestRef, &PublishedFiles)) return false;
		TMap<FString, FString> Retained = Header->GetRetainedFileHashes();
		for (const auto& Path : RemovedFiles) { Retained.Remove(Path); Retained.Remove(TEXT("examples/files/") + Path); }
		for (const auto& File : Header->GetFiles()) Files.Add(File);
		TSet<FWorkspacePlacementStorage::FKey> ContentPaths;
		for (const auto& Content : Scope.ContentFiles)
			for (const auto& Path : Content.Value) ContentPaths.Add({&Path});
		for (const auto& File : Scope.Files) if (!Scope.Documents.Contains(File.GetRelativePath()) && !RemovedFiles.Contains(File.GetRelativePath())) {
			Files.Add(File);
			Files.Last().SetSemanticSourceDocument(Layout.GetPhysicalFiles().Contains(File.GetRelativePath())
				|| ContentPaths.Contains(FWorkspacePlacementStorage::FKey{&File.GetRelativePath()}));
		}
		for (const auto& File : Files) Retained.Remove(File.GetRelativePath());
		FString Material = Header->GetPlanFingerprint();
		for (const auto& File : Files) {
			ArtifactPublisher::RememberVerifiedPlanBytes(File);
			Material += TEXT("|") + File.GetRelativePath() + TEXT("|") + File.GetContentHash();
		}
		OutPlan = MakeShared<FExportWorkspacePlan>(EWorkspaceKind::Directory, ManifestPath, Binding.SnapshotId, Binding.Revision,
			Binding.EvidenceId, MoveTemp(Files), Sha256Text(Material), Header->GetObservedTokens(), MoveTemp(Retained));
	if (!PrepareChangedWorkspacePublicationDirectory(Root, *Settings.ActualChanges, &Layout, Error,
		Scope.Rows.Difference(ChangedIds))) return false;
		PreviousCompositionExport.Reset(); RetireGeneratedSemanticWorkspace(Root);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=scoped_structure_publication actual_rows=%d formatter_rows=%d naming_rows=%d changed_public_documents=%d changed_machine_buckets=%d preserved_machine_rows=%d complete_layout=false second_inventory_material=false total_s=%.6f"),
			Settings.ActualChanges->Changes.Num(), Scope.Rows.Num(), ContextIds.Num(), Documents.Num(), Buckets.Num(), PreservedRows, FPlatformTime::Seconds() - Started);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=scoped_definition_consumers changed_definitions=%d guide_definitions=%d rendered_definition_rows=%d full_catalog_rows_added=0"),
			ChangedDefinitions.Num(), Scope.GuideDefinitions.Num(), Scope.RenderedDefinitionRows);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=scoped_guide_retirement changed_guides=%d unchanged_guide_reads=%d retired_recipe_candidates=%d native_values_read=0"),
			Scope.PreviousGuides.Num(), UnchangedGuideReads, RetiredRecipeCandidates.Num());
		return Model->CheckValueReadResult(Error);
	}

	// Full export promises the current guide generator too. Unchanged semantic
	// rows do not prove its code-generated recipe bodies. Feed that same catalog
	// current declarations and the final K/retained documents, without rebuilding
	// an N layout, machine rows or property value documents for this consumer.
static bool TryBuildObservedGuides(FAuthoringModelRef Model, const FString& Root, const FString& TargetKind,
		const TMap<FString, TPair<FString, FString>>& Locations, const TArray<uint8>& Membership,
		FUeAgentResourceTracker& Resources, FExportWorkspacePlanPtr& Plan, FWorkspaceError& Error)
	{
		const double Started = FPlatformTime::Seconds();
		const auto Inventory = GetAcceptedWorkspaceInventory(Root);
		if (!Inventory || !Plan) return FailWorkspaceDataflowHandoff(Error, TEXT("composition.full_guides"),
			TEXT("accepted inventory and final publication plan"), Root, TEXT("missing"),
			TEXT("same publication output"), TEXT("observed publication writer"));
		struct FDocument {
			const FSemanticUnit* First = nullptr;
			TArray<FAuthoringExampleCatalog::FPropertyInstruction> Properties;
			bool bContent = false;
		};
		TMap<FString, FDocument> Documents;
		int32 PropertyRows = 0, MetadataDocuments = 0, MetadataFiles = 0, PointerHandoffs = 0, FreshPointers = 0; int64 MetadataBytes = 0;
		const auto Add = [&](const FSemanticUnit& Unit, int32 Index, const FString& PreviousPath, const FString* PreviousPointer, bool bContent) {
			const auto* Actual = Locations.Find(Unit.GetUnitId());
			const FString& Path = Actual ? Actual->Key : PreviousPath;
			const bool bProperty = Unit.GetUnitKind() == ESemanticUnitKind::PropertySet;
			const bool bDefinition = Unit.GetUnitKind() == ESemanticUnitKind::CollectionState && Unit.GetRole() == TEXT("property_definition");
			if (!bProperty && !bDefinition && !IsKeyedItemStructureUnit(Unit) && !IsGraphStructureUnit(Unit)) return true;
			if (Path.IsEmpty()) { Error.SetSourceFailure(TEXT("workspace_observed_guide_location_missing"),
				TEXT("The current guide input has no actual public location. No complete layout was selected."), Unit.GetUnitId()); return false; }
			auto& Document = Documents.FindOrAdd(Path);
			if (!Document.First) Document.First = &Unit;
			if (bProperty) {
				// Bit 4 is this very comparison's exact current pointer, not a cache
				// qualification. Changed/new rows generate their own current pointer.
				// Bit 8 preserves the original FString/NUL-equivalent text edge.
				const bool bBorrowPointer = (Membership[Index] & 4) != 0;
				if ((bBorrowPointer && !PreviousPointer)
					|| (!bBorrowPointer && (Membership[Index] & 1) && !(Membership[Index] & (2 | 8))))
					return FailWorkspaceDataflowHandoff(Error, TEXT("composition.full_guide_pointer"), Unit.GetUnitId(), Path,
						TEXT("missing current property-pointer comparison"), TEXT("same-observation pointer or explicit changed/new row"),
						TEXT("observed row comparison"));
				Document.Properties.Add(DescribePropertyGuide(Unit, *Model, bBorrowPointer ? PreviousPointer : nullptr)); ++PropertyRows;
				if (bBorrowPointer) ++PointerHandoffs; else ++FreshPointers;
			}
			Document.bContent |= bContent || (bProperty && HasAuthoringPropertyContent(Unit.GetSemanticTypeRef(), Unit.GetRole()));
			return true;
		};
		const auto& Units = Model->GetUnits();
		if (!Inventory->Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
			const int32 Index = Model->FindUnitIndex(Row.UnitId);
			return Index == INDEX_NONE || Add(Units[Index], Index, Row.DocumentPath, &Row.UnitPointer, !Row.ContentFiles.IsEmpty());
		})) return false;
		for (int32 Index = 0; Index < Units.Num(); ++Index)
			if (!(Membership[Index] & 1) && !Add(Units[Index], Index, FString(), nullptr, false)) return false;
		// These pointers borrow only the immutable K plan throughout this call.
		// No pointer survives rebuilding that plan below.
		TMap<FString, const FExportFilePlan*> Produced;
		for (const auto& File : Plan->GetFiles()) Produced.Add(File.GetRelativePath(), &File);
		const auto Read = [&](const FString& Path, bool bProperty, FUeAgentCanonicalValuePtr& Value) {
			const auto Parse = [&](const TArray<uint8>& Bytes) {
				++MetadataFiles; MetadataBytes += Bytes.Num();
				if (bProperty) return TryParsePropertyGuideMetadata(Path, Bytes, Resources, Value, Error);
				FHjsonDocumentPtr Parsed;
				const FSourceProvenance Origin(Path, TEXT("/"), 1, 1, 0, Bytes.Num(), FString());
				return TryParseGeneratedHjsonSpan(Bytes, Origin, Resources, Parsed, Error)
					&& TryProjectHjsonCanonicalValue(*Parsed->GetRoot(), Resources, Value, Error, false);
			};
			if (const auto* File = Produced.Find(Path)) return Parse((*File)->GetCanonicalUtf8());
			const auto* Hash = Plan->GetRetainedFileHashes().Find(Path); TArray<uint8> Bytes;
			if (!Hash) return FailWorkspaceDataflowHandoff(Error, TEXT("composition.full_guides"), Path, Root,
				TEXT("absent from final write/retained set"), TEXT("actual public document"), TEXT("publication body writer"));
			return TryReadPublishedWorkspaceBytes(Root, Path, *Hash, Bytes, Error) && Parse(Bytes);
		};
		FAuthoringExampleCatalog Examples; RegisterDomainAuthoringExampleRules(Examples);
		TSet<FString> ContentDocuments;
		TFunction<bool(const FUeAgentCanonicalValueRef&)> AddContent;
		AddContent = [&](const FUeAgentCanonicalValueRef& Value) {
			if (Value->GetKind() != EUeAgentValueKind::Object) return true;
			if (const auto* Ref = Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("content_ref"); });
				Ref && Ref->Value->GetKind() == EUeAgentValueKind::String) {
				const FString& Path = Ref->Value->GetText();
				if (!ContentDocuments.Contains(Path)) {
					ContentDocuments.Add(Path); FUeAgentCanonicalValuePtr Metadata;
					if (!Read(Path, true, Metadata)) return false;
					Examples.AddDocument(Path, Metadata.ToSharedRef());
					if (!AddContent(Metadata.ToSharedRef())) return false;
				}
			}
			for (const auto& Field : Value->GetObjectItems()) if (!AddContent(Field.Value)) return false;
			return true;
		};
		for (auto& Pair : Documents) {
			const auto& Unit = *Pair.Value.First;
			if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet) {
				if (!Pair.Value.bContent) { Examples.AddPropertyDocument(Pair.Key, MoveTemp(Pair.Value.Properties)); continue; }
				FUeAgentCanonicalValuePtr Metadata;
				if (!Read(Pair.Key, true, Metadata) || !AddContent(Metadata.ToSharedRef())) return false;
				Examples.AddDocument(Pair.Key, Metadata.ToSharedRef()); continue;
			}
			++MetadataDocuments;
			if (Unit.GetRole() == TEXT("property_definition")) {
				FUeAgentCanonicalValuePtr Metadata; TSharedPtr<FJsonValue> Json; Contract::FDiagnostic Failure;
				if (!Read(Pair.Key, false, Metadata) || !CanonicalJson::TryToJsonValue(*Metadata, Json, Failure)) {
					if (!Error.HasErrors()) Error.SetFailure(MoveTemp(Failure), Pair.Key); return false;
				}
				const TSharedPtr<FJsonObject>* Definitions = nullptr; const TSharedPtr<FJsonObject>* Enums = nullptr;
				if (!Json || Json->Type != EJson::Object || !Json->AsObject()->TryGetObjectField(TEXT("definitions"), Definitions)) {
					Error.SetSourceFailure(TEXT("workspace_observed_guide_definitions_missing"), TEXT("The final definition document has no definitions object."), Pair.Key); return false;
				}
				Json->AsObject()->TryGetObjectField(TEXT("enums"), Enums);
				Examples.AddDefinitions(Pair.Key, Definitions->ToSharedRef(), Enums ? *Enums : nullptr); continue;
			}
			FUeAgentCanonicalValuePtr Metadata;
			if (IsKeyedItemStructureUnit(Unit)) {
				const auto Contracts = DocumentCollectionContracts(Unit.GetDeclaration(), TargetKind);
				if (Contracts.IsEmpty()) {
					Error.SetSourceFailure(TEXT("workspace_observed_guide_contract_missing"), TEXT("The current collection has no declared guide contract."), Unit.GetUnitId()); return false;
				}
				if (CollectionGuidesNeedContent(Contracts)) {
					if (!Read(Pair.Key, false, Metadata)) return false;
				} else if (!BuildLeafCollectionGuideDocument(Contracts, Resources, Metadata, Error, Pair.Key)) return false;
				for (const auto* Contract : Contracts) Examples.AddCollection(Pair.Key, Metadata.ToSharedRef(), *Contract);
			} else if (!Read(Pair.Key, false, Metadata)) return false;
			Examples.AddDocument(Pair.Key, Metadata.ToSharedRef(), Unit.GetSemanticTypeRef(), Unit.GetRole());
			if (!AddContent(Metadata.ToSharedRef())) return false;
		}
		TArray<FExportFilePlan> Guides;
		if (!Examples.Publish(Guides, Error, [](const TArray<uint8>& Bytes) { return Sha256(Bytes); })) return false;
		TArray<FExportFilePlan> Files; TMap<FString, FString> Retained = Plan->GetRetainedFileHashes();
		for (const auto& File : Plan->GetFiles()) if (!File.GetRelativePath().StartsWith(TEXT("examples/"), ESearchCase::CaseSensitive)) Files.Add(File);
		for (auto It = Retained.CreateIterator(); It; ++It) if (It.Key().StartsWith(TEXT("examples/"), ESearchCase::CaseSensitive)) It.RemoveCurrent();
		FString Material = Plan->GetPlanFingerprint();
		for (auto& Guide : Guides) {
			// This formal caller supplied Sha256 to the original byte producer.
			// Carry that exact immutable result, rather than hashing it again in
			// generic preflight. Generic Publish/Hash callbacks retain validation.
			ArtifactPublisher::RememberVerifiedPlanBytes(Guide);
			Material += TEXT("|") + Guide.GetRelativePath() + TEXT("|") + Guide.GetContentHash(); Files.Add(MoveTemp(Guide));
		}
		Plan = MakeShared<FExportWorkspacePlan>(Plan->GetWorkspaceKind(), Plan->GetEntryFile(), Plan->GetSnapshotId(), Plan->GetSnapshotRevision(),
			Plan->GetSnapshotEvidenceId(), MoveTemp(Files), Sha256Text(Material), Plan->GetObservedTokens(), MoveTemp(Retained), Plan->GetPendingAuthorDocuments());
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=complete_observation_guides property_instructions=%d metadata_documents=%d parsed_files=%d parsed_bytes=%lld total_s=%.6f complete_layout=false property_value_documents=false pointer_handoffs=%d fresh_pointers=%d"),
			PropertyRows, MetadataDocuments, MetadataFiles, MetadataBytes, FPlatformTime::Seconds() - Started, PointerHandoffs, FreshPointers);
		return Model->CheckValueReadResult(Error);
	}

	// A complete current observation is not a request to materialize a complete
	// output layout. This is the same publication difference consumed by the
	// existing writers, never a fabricated native commit or durable record.
	static bool TryBuildObservedPublication(FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& Binding, TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources, FExportWorkspacePlanPtr& OutPlan, FWorkspaceError& Error,
		const FString& TargetKind, const FPreparedWorkspaceExport& Settings,
		TMap<FString, TPair<FString, FString>>* PendingLocations)
	{
		const double Started = FPlatformTime::Seconds();
		const FString& Root = Settings.AcceptedWorkspaceRoot;
		const auto Source = GetAcceptedSemanticWorkspace(Root);
		const auto Inventory = GetAcceptedWorkspaceInventory(Root);
		const auto* Publication = FindAcceptedWorkspacePublication(Root);
		if (!Source || !Inventory || !Publication || !Publication->bDirectDataflow
			|| !HasAcceptedPublicationInputs(Root))
			return FailWorkspaceDataflowHandoff(Error, TEXT("composition.observed_publication"),
				TEXT("accepted direct publication and unconsumed-author classification"), Root, TEXT("missing"),
				TEXT("formal same-workspace Source preparation"), TEXT("explicit full/incremental Source"));
		const auto& Origin = Source->GetModel();
		if (Origin->GetContractId() != Model->GetContractId() || Origin->GetContractVersion() != Model->GetContractVersion()
			|| Origin->GetProfileId() != Model->GetProfileId() || Origin->GetTargetUnitIds() != Model->GetTargetUnitIds())
			return FailWorkspaceDataflowHandoff(Error, TEXT("composition.observed_publication"),
				TEXT("current contract/profile/targets"), Root, TEXT("scope mismatch"),
				TEXT("same accepted target publication"), TEXT("complete observation binding"));
		const auto& Units = Model->GetUnits();
		auto Difference = MakeShared<FCommittedWorkspaceDelta>();
		Difference->ContractId = Model->GetContractId(); Difference->ContractVersion = Model->GetContractVersion();
		Difference->ProfileId = Model->GetProfileId(); Difference->AfterRevision = Model->GetBaseRevision();
		Difference->BeforeTargets = Origin->GetTargetUnitIds(); Difference->AfterTargets = Model->GetTargetUnitIds();
		// The original current model already owns the unique identity lookup.
		// These bits record this comparison's membership/selection, not an index.
		TArray<uint8> Flags; Flags.Init(0, Units.Num());
		TSet<FString> RewriteRows, FullRebuildDocuments, ChangedPathOwners, ChangedRelationOwners, ChangedDefinitionAddresses;
		struct FBucketOrder { TSharedPtr<const FString, ESPMode::ThreadSafe> Shared; FString Owned; bool bRewrite = false; };
		TMap<FString, FBucketOrder> BucketOrder;
		int32 Compared = 0, Matched = 0, HashRequests = 0, NewHashes = 0, PointerChecks = 0, BeforeReads = 0;
		int32 InlineValueRows = 0, ExpandedInlineValueRows = 0;
		int32 ObservedDocuments = 0, NormalizedDocuments = 0;
		double HashSeconds = 0;
		const auto AddChange = [&](const FString& Id, int32 Index, bool bPublished) {
			if (Index != INDEX_NONE && (Flags[Index] & 2)) return true;
			FCommittedUnitChange Change; Change.UnitId = Id;
			if (bPublished) {
				const auto* Before = Source->FindBaselineUnit(Id); ++BeforeReads;
				if (!Before) {
					Model->CheckValueReadResult(Error);
					if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("workspace_observed_before_missing"),
						TEXT("The changed destination row has no accepted typed before. No complete Source or alternate writer was selected."), Id);
					return false;
				}
				Change.Before.Emplace(*Before);
			}
			if (Index != INDEX_NONE) {
				Change.After.Emplace(Units[Index]); Flags[Index] |= 2;
				if (Change.Before && (Change.Before->GetUnitKind() != Change.After->GetUnitKind()
					|| !HaveSameSemanticProjectionIdentity(*Change.After, *Change.Before)
					|| Change.Before->GetOwnerRef() != Change.After->GetOwnerRef()
					|| Change.Before->GetSemanticTypeRef() != Change.After->GetSemanticTypeRef()
					|| Change.Before->GetRole() != Change.After->GetRole()
					|| Change.Before->GetFieldSetRef() != Change.After->GetFieldSetRef())) ChangedPathOwners.Add(Id);
			}
			if (!Change.Before || !Change.After) {
				const auto& Unit = Change.After ? *Change.After : *Change.Before;
				if (Unit.GetUnitKind() == ESemanticUnitKind::RelationSet) ChangedRelationOwners.Add(Unit.GetOwnerRef());
			}
			Difference->Changes.Add(MoveTemp(Change)); return true;
		};
		// Only pure row formatting/digesting runs in the task graph. The accepted
		// inventory and current model remain alive through every join. Preserve
		// its exact row order for comparisons, Before reads and diagnostics.
		struct FObservedRowWork {
			const FString& UnitId; ESemanticUnitKind UnitKind;
			const FString& PublicRef; const FString& UnitPointer; const FString& EnvelopePath; const FString& DocumentPath;
			TSharedPtr<const FString, ESPMode::ThreadSafe> SharedUnitIdentity;
			TSharedPtr<const FInventoryRowSyntax> SourceSyntax;
			FAcceptedWorkspaceContentHashView Content;
			int32 Index; bool bHashReady = false; double HashWorkSeconds = 0;
			TSharedPtr<FWorkspaceError> HashError;
			FObservedRowWork(const FAcceptedWorkspaceInventoryRow& Row, int32 InIndex)
				: UnitId(Row.UnitId), UnitKind(Row.UnitKind), PublicRef(Row.PublicRef), UnitPointer(Row.UnitPointer), EnvelopePath(Row.EnvelopePath), DocumentPath(Row.DocumentPath),
				SharedUnitIdentity(Row.SharedUnitIdentity), SourceSyntax(Row.SourceSyntax), Content(Row.ContentHashView), Index(InIndex) {}
			bool ContentHashEquals(const FString& Hash) const { return Content.Matches(Hash); }
			FString CanonicalContentHash() const { return Content.Canonical(); }
		};
		TArray<FObservedRowWork> HashBatch;
		int32 HashBatches = 0, MaxHashBatch = 0; double HashWorkerElapsed = 0;
		const auto ConsumeObservedRow = [&](const FObservedRowWork& Row) {
			++Compared;
			const int32 Index = Row.Index;
			if (Index == INDEX_NONE) return AddChange(Row.UnitId, INDEX_NONE, true);
			Flags[Index] |= 1;
			const auto& Unit = Units[Index];
			Error.Reset(); // Same boundary as the original owning hash API.
			if (!Row.bHashReady) { Error = *Row.HashError; return false; }
			const FString& Hash = Unit.GetPublicRowHash();
			if (Unit.HasInlinePropertyValue()) { ++InlineValueRows; if (Unit.HasExpandedInlineValueMap()) ++ExpandedInlineValueRows; }
			const bool bSameContent = Row.ContentHashEquals(Hash);
			// Keep the compact comparison on every matching row. Only an actual
			// mismatch needs to distinguish old uncomparable spelling from change.
			if (!bSameContent && Row.CanonicalContentHash().IsEmpty())
				return FailWorkspaceDataflowHandoff(Error, TEXT("composition.observed_row"),
					Row.UnitId, Row.DocumentPath, TEXT("noncanonical or absent row digest"),
					TEXT("original canonical row SHA"), TEXT("publication inventory writer"));
			bool bSame = Unit.GetUnitKind() == Row.UnitKind && bSameContent;
			if (bSame && Unit.GetUnitKind() == ESemanticUnitKind::PropertySet) {
				++PointerChecks;
				TStringBuilder<256> Pointer;
				Pointer.Append(TEXT("/categories/"));
				Pointer.Append(EscapeJsonPointerSegment(ReadablePropertyCategory(Unit.GetCategoryPath())));
				Pointer.AppendChar(TEXT('/')); Pointer.Append(EscapeJsonPointerSegment(ReadablePropertyKey(Unit)));
				const FStringView PointerText = Pointer.ToView();
				bSame = PointerText.Equals(FStringView(Row.UnitPointer), ESearchCase::CaseSensitive);
				if (bSame) {
					// FString equality deliberately ignores a suffix after matching
					// embedded NULs. Reusing output text requires all actual code units.
					const bool bExactText = PointerText.Len() == Row.UnitPointer.Len()
						&& FMemory::Memcmp(PointerText.GetData(), *Row.UnitPointer, PointerText.Len() * sizeof(TCHAR)) == 0;
					Flags[Index] |= bExactText ? 4 : 8;
				}
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::CollectionState && Unit.GetRole() == TEXT("property_definition")) {
				const FString Pointer = TEXT("/definitions/") + EscapeJsonPointerSegment(ReadablePropertyDefinition(Unit, Model.Get()));
				const auto* Name = Model->GetPreferredDefinitionBindings().Find(Unit.GetUnitId());
				if (!Pointer.Equals(Row.UnitPointer, ESearchCase::CaseSensitive)
					|| (Name && !Name->PublicRef.IsEmpty() && !Name->PublicRef.Equals(Row.PublicRef, ESearchCase::CaseSensitive))) {
					bSame = false; ChangedDefinitionAddresses.Add(Unit.GetUnitId());
				}
			}
			// The original full writer emits each physical bucket in current
			// model identity order. Preserve that result independently of SHA.
			auto& Order = BucketOrder.FindOrAdd(Row.EnvelopePath);
			const FString& Last = Order.Shared ? *Order.Shared : Order.Owned;
			if (!Order.bRewrite && !Last.IsEmpty() && !(Last < Row.UnitId)) { RewriteRows.Add(Row.UnitId); Order.bRewrite = true; }
			Order.Shared = Row.SharedUnitIdentity;
			if (!Order.Shared) Order.Owned = Row.UnitId;
			if (bSame) { ++Matched; return true; }
			return AddChange(Row.UnitId, Index, true);
		};
		const auto FlushHashes = [&]() {
			if (HashBatch.IsEmpty()) return true;
			++HashBatches; MaxHashBatch = FMath::Max(MaxHashBatch, HashBatch.Num());
			const double StartedHashing = FPlatformTime::Seconds();
			ParallelFor(HashBatch.Num(), [&](int32 WorkIndex) {
				auto& Row = HashBatch[WorkIndex];
				if (Row.Index == INDEX_NONE) return;
				FWorkspaceError RowError; const double StartedWork = FPlatformTime::Seconds();
				Row.bHashReady = TryPrepareSemanticUnitRowHash(Units[Row.Index], RowError);
				Row.HashWorkSeconds = FPlatformTime::Seconds() - StartedWork;
				if (!Row.bHashReady) Row.HashError = MakeShared<FWorkspaceError>(MoveTemp(RowError));
			});
			HashSeconds += FPlatformTime::Seconds() - StartedHashing;
			for (const auto& Row : HashBatch) {
				HashWorkerElapsed += Row.HashWorkSeconds;
				if (!ConsumeObservedRow(Row)) return false;
			}
			HashBatch.Reset();
			return true;
		};
		const bool bVisited = Inventory->Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
			if (!Row.ContentHashView.IsBound()) {
				if (!FlushHashes()) return false;
				return FailWorkspaceDataflowHandoff(Error, TEXT("composition.observed_digest"), Row.UnitId, Row.DocumentPath,
					TEXT("missing owned digest backing"), TEXT("inventory-owned digest/string view"), TEXT("accepted inventory producer"));
			}
			const int32 Index = Model->FindUnitIndex(Row.UnitId);
			if (Index != INDEX_NONE) { ++HashRequests; if (Units[Index].GetPublicRowHash().IsEmpty()) ++NewHashes; }
			HashBatch.Emplace(Row, Index);
			return !Row.bEndReadChunk || FlushHashes();
		});
		if (!bVisited || !FlushHashes()) { Model->CheckValueReadResult(Error); return false; }
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=observed_row_tasks batches=%d max_batch_rows=%d row_jobs=%d task_wall_s=%.6f worker_elapsed_sum_s=%.6f owned_hash_text_copies=0 metadata_consumption=original_inventory_order workers=pure_const_values"),
			HashBatches, MaxHashBatch, HashRequests, HashSeconds, HashWorkerElapsed);
		for (int32 Index = 0; Index < Units.Num(); ++Index)
			if (!(Flags[Index] & 1) && !AddChange(Units[Index].GetUnitId(), Index, false)) return false;
		// Semantic equality does not promise byte equality. Full output used to
		// re-render an accepted document when equivalent spelling/whitespace
		// differed from its publication. Keep that result, using the Source's
		// existing observed hashes, not another filesystem discovery or hash pass.
		if (!TryVisitAcceptedPublicDocumentByteChanges(Root, [&](const FString& Path) {
			++ObservedDocuments; ++NormalizedDocuments;
			FullRebuildDocuments.Add(Path);
			if (Inventory->VisitDocument(Path, [&](const FAcceptedWorkspaceInventoryRow& Row) {
				if (Model->FindUnitIndex(Row.UnitId) != INDEX_NONE) RewriteRows.Add(Row.UnitId); return true;
			})) return true;
			if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("workspace_observed_document_rows_missing"),
				TEXT("The accepted document has no complete member handoff. No complete layout or alternate reader was selected."), Path);
			return false;
		}, Error)) return false;

		// Names are an independent output input, not a row-SHA qualification.
		// Consume the original Source producer's already established bindings.
		const auto Names = GetAcceptedDefinitionProjectionHints(TargetKind);
		if (!Names && !Model->GetPreferredDefinitionBindings().IsEmpty())
			return FailWorkspaceDataflowHandoff(Error, TEXT("composition.observed_definition_names"),
				TEXT("accepted definition names"), Root, TEXT("missing"), TEXT("original publication definition bindings"),
				TEXT("Source ReadPublishedDefinitionBindings"));
		TSet<FString> ChangedNames = MoveTemp(ChangedDefinitionAddresses);
		if (Names) for (const auto& Name : Model->GetPreferredDefinitionBindings()) {
			const auto* Before = Names->Definitions.Find(Name.Key);
			if (Before && (Before->DefinitionName != Name.Value.DefinitionName || Before->ReadableAlias != Name.Value.ReadableAlias
				|| Before->PublicRef != Name.Value.PublicRef)) ChangedNames.Add(Name.Key);
		}
		if (!ChangedNames.IsEmpty()) {
			// Only the real definition graph participates in this name closure.
			bool bAdded = true;
			while (bAdded) {
				bAdded = false;
				if (!Model->VisitDefinitionDeclarations([&](const auto& Declaration) {
					if (ChangedNames.Contains(Declaration.GetUnitId())) return true;
					for (const auto& Ref : Declaration.GetDependencyRefs()) if (ChangedNames.Contains(Ref)) {
						ChangedNames.Add(Declaration.GetUnitId()); bAdded = true; break;
					}
					return true;
				})) {
					Model->CheckValueReadResult(Error);
					if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("workspace_observed_definition_directory_unavailable"),
						TEXT("The current definition dependency enumeration did not complete. No alternate planner was selected."), Root);
					return false;
				}
			}
		}
		for (int32 Index = 0; Index < Units.Num(); ++Index) {
			const auto& Unit = Units[Index]; bool bWrite = ChangedNames.Contains(Unit.GetUnitId());
			if (!ChangedNames.IsEmpty()) {
				bWrite |= ChangedNames.Contains(Unit.GetFieldSetRef());
				for (const auto& Ref : Unit.GetDependencyRefs()) if (ChangedNames.Contains(Ref)) { bWrite = true; break; }
			}
			bWrite |= Unit.GetUnitKind() == ESemanticUnitKind::RelationSet && ChangedRelationOwners.Contains(Unit.GetOwnerRef());
			if (!ChangedPathOwners.IsEmpty()) {
				FString Owner = Unit.GetOwnerRef();
				while (!Owner.IsEmpty()) {
					if (ChangedPathOwners.Contains(Owner)) { bWrite = true; break; }
					const auto Parent = Model->FindDeclaration(Owner); if (!Parent) break; // Full current reference validation already ran.
					Owner = Parent->GetOwnerRef();
				}
			}
			if (bWrite && !AddChange(Unit.GetUnitId(), Index, (Flags[Index] & 1) != 0)) return false;
		}
		Difference->Changes.Sort([](const auto& A, const auto& B) { return A.UnitId < B.UnitId; });
		if (!PrepareObservedWorkspacePublicationOrigin(Root, TargetKind, *Model, *Difference, Error)) return false;
		const double ComparedAt = FPlatformTime::Seconds();
		FPreparedWorkspaceExport Output = Settings;
		Output.SourcePublication = *Publication; Output.ActualChanges = Difference;
		TMap<FString, TPair<FString, FString>> LocalLocations;
		auto* Locations = PendingLocations ? PendingLocations : &LocalLocations;
		// Output is request-local. Do not assign this difference to Settings or
		// durable history, and retain the caller's explicit export mode/watermark.
		bool bBuilt;
		if (Difference->Changes.IsEmpty() && RewriteRows.IsEmpty())
			bBuilt = TryBuildCommittedPublicationHeader(Binding, TargetKind, Output, Resources, OutPlan, Error);
		else {
			TMap<FString, const FSemanticUnit*> Properties; bool bProperty = false;
			if (!TrySelectPropertyPublicationInput(Model, Output, Properties, bProperty, Error, &RewriteRows)) return false;
			bool bReorderedBucket = false;
			for (const auto& Bucket : BucketOrder) bReorderedBucket |= Bucket.Value.bRewrite;
			if (bProperty && !bReorderedBucket) {
				bBuilt = TryBuildChangedPropertyPublication(Model, Binding, TargetKind, Output, Properties, Resources, OutPlan, Error, Locations, &FullRebuildDocuments);
				if (bBuilt) bBuilt = PrepareChangedWorkspacePublicationDirectory(Root, *Difference, nullptr, Error);
			} else bBuilt = TryBuildScopedCommittedPublication(Model, Binding, MoveTemp(Telemetry), Resources, OutPlan, Error, TargetKind, Output, Locations, &RewriteRows, &FullRebuildDocuments, false);
		}
		if (bBuilt && Difference->Changes.IsEmpty() && RewriteRows.IsEmpty())
			bBuilt = PrepareChangedWorkspacePublicationDirectory(Root, *Difference, nullptr, Error);
		if (bBuilt) bBuilt = TryBuildObservedGuides(Model, Root, TargetKind, *Locations, Flags, Resources, OutPlan, Error);
		PreviousCompositionExport.Reset(); RetireGeneratedSemanticWorkspace(Root);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=complete_observation_publication units=%d accepted_rows=%d unchanged_rows=%d output_rows=%d before_value_reads=%d row_hash_requests=%d newly_hashed_rows=%d row_writer_utf8_digest_s=%.6f property_pointer_checks=%d physical_rewrite_rows=%d observed_document_hashes=%d normalized_documents=%d comparison_s=%.6f selected_writer_s=%.6f complete_business_layout=false complete_machine_plan=false native_commit_synthesized=false success=%s inline_value_rows=%d expanded_inline_maps_at_row_consumer=%d"),
			Units.Num(), Compared, Matched, Difference->Changes.Num(), BeforeReads, HashRequests, NewHashes, HashSeconds,
			PointerChecks, RewriteRows.Num(), ObservedDocuments, NormalizedDocuments, ComparedAt - Started, FPlatformTime::Seconds() - ComparedAt,
			bBuilt ? TEXT("true") : TEXT("false"), InlineValueRows, ExpandedInlineValueRows);
		return bBuilt && Model->CheckValueReadResult(Error);
	}

	bool TryBuildCompositionExportPlan(FAuthoringModelRef Model, const FCompositionSnapshotBinding& Binding,
		TMap<FString, int64> Telemetry, FUeAgentResourceTracker& Resources, FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& Error, const FString& TargetKind, const FPreparedWorkspaceExport* Settings,
		TMap<FString, TPair<FString, FString>>* PendingLocations)
	{
		if (Settings && (Settings->bCommittedRequested || Settings->ActualExportMode == TEXT("committed") || Settings->ActualChanges)) {
			if (!Settings->ActualChanges || !Settings->SourcePublication.bDirectDataflow || !Settings->Publication.bDirectDataflow)
				return FailWorkspaceDataflowHandoff(Error, TEXT("composition.committed_dispatch"),
					!Settings->ActualChanges ? TEXT("actual_changes") : TEXT("source/destination /4 publication"),
					Settings->AcceptedWorkspaceRoot, Settings->ActualExportMode,
					TEXT("same-request actual delta and direct publication"), TEXT("committed export selection"));
		}
		if (Settings && Settings->ActualChanges && Settings->SourcePublication.bDirectDataflow && Settings->Publication.bDirectDataflow) {
			TMap<FString, const FSemanticUnit*> PropertyUnits; bool bPropertyInput = false;
			if (!TrySelectPropertyPublicationInput(Model, *Settings, PropertyUnits, bPropertyInput, Error)) return false;
			if (bPropertyInput) {
				const bool bBuilt = TryBuildChangedPropertyPublication(Model, Binding, TargetKind, *Settings, PropertyUnits, Resources, OutPlan, Error, PendingLocations);
				if (!bBuilt || !PrepareChangedWorkspacePublicationDirectory(Settings->AcceptedWorkspaceRoot, *Settings->ActualChanges, nullptr, Error)) return false;
				PreviousCompositionExport.Reset(); RetireGeneratedSemanticWorkspace(Settings->AcceptedWorkspaceRoot);
				return true;
			}
			const bool bBuilt = TryBuildScopedCommittedPublication(Model, Binding, MoveTemp(Telemetry), Resources, OutPlan, Error, TargetKind, *Settings, PendingLocations);
			if (!bBuilt && !Error.HasErrors()) Error.SetSourceFailure(TEXT("workspace_committed_projection_incomplete"),
				TEXT("A required changed-row projection or physical-file fragment could not be produced. Retain the request's custody and continue its refresh; do not replay native changes."), Settings->AcceptedWorkspaceRoot);
			return bBuilt;
		}
		if (Settings && Settings->ActualExportMode != TEXT("full") && Settings->ActualExportMode != TEXT("incremental"))
			return FailWorkspaceDataflowHandoff(Error, TEXT("composition.export_dispatch"),
				TEXT("explicit complete export mode"), Settings->AcceptedWorkspaceRoot, Settings->ActualExportMode,
				TEXT("full, initialization or requested ordinary incremental"), TEXT("public export request dispatch"));
		// Domain/mode is selected before work, not after a failed local writer.
		// New roots and an explicitly changed profile require initial complete
		// output. Existing same-profile Level publications use one difference
		// algorithm for empty, value, create, delete and naming changes.
		if (Settings && Settings->Publication.bDirectDataflow && TargetKind == TEXT("ue.level.asset") && !Settings->bInitializePublication
			&& !Settings->bIncludeResultFileDetails) {
			if (Settings->AcceptedWorkspaceRoot.IsEmpty()) return FailWorkspaceDataflowHandoff(Error,
				TEXT("composition.observed_dispatch"), TEXT("AcceptedWorkspaceRoot"), FString(), TEXT("missing"),
				TEXT("existing destination from formal Source"), TEXT("full/incremental publication entry"));
			const auto Source = GetAcceptedSemanticWorkspace(Settings->AcceptedWorkspaceRoot);
			const bool bRequestedNewProfile = Source && Source->GetModel()->GetProfileId() != Model->GetProfileId()
				&& Settings->ActualExportMode == TEXT("full");
			if (!bRequestedNewProfile)
				return TryBuildObservedPublication(Model, Binding, MoveTemp(Telemetry), Resources, OutPlan, Error, TargetKind, *Settings, PendingLocations);
		}
		return TryBuildCompositionExportPlanInternal(Model, Binding, MoveTemp(Telemetry), Resources, OutPlan, Error, TargetKind, Settings, PendingLocations, nullptr);
	}

	bool TryPreservePendingAuthoringValues(FPreparedWorkspaceExport& Prepared,
		const FAuthoringModelRef& ActualModel, FUeAgentResourceTracker& Resources, FWorkspaceError& Error,
		TSet<FString>& OutPendingDocuments)
	{
		OutPendingDocuments.Reset();
		if (Prepared.Plan && Prepared.Plan->GetPendingAuthorDocuments().IsSet()) {
			OutPendingDocuments = Prepared.Plan->GetPendingAuthorDocuments().GetValue(); return true;
		}
		if (Prepared.PendingAuthorUnits.IsEmpty() && Prepared.PendingKeyedAuthorRows.IsEmpty()) return true;
		if (!Prepared.Plan) return false;
		const auto Source = GetAcceptedSemanticWorkspace(Prepared.AcceptedWorkspaceRoot);
		struct FPatch { FString Pointer; FUeAgentCanonicalValueRef Value; bool bKeyed = false; };
		TMap<FString, TArray<FPatch>> Patches;
		for (const auto& Unit : Prepared.PendingKeyedAuthorRows) {
			const auto* Location = Prepared.PendingAuthorLocations.Find(Unit.Key);
			if (!Location) {
				Error.SetSourceFailure(TEXT("workspace_committed_author_location_missing"), TEXT("The pending keyed parent has no actual formatter location."), Unit.Key);
				return false;
			}
			for (const auto& Root : Unit.Value) for (const auto& Row : Root.Value)
				Patches.FindOrAdd(Location->Key).Add({(Location->Value == TEXT("/") ? FString() : Location->Value)
					+ (Root.Key.IsEmpty() ? FString() : TEXT("/") + EscapeJsonPointerSegment(Root.Key))
					+ TEXT("/") + EscapeJsonPointerSegment(Row.Key), Row.Value, !Root.Key.IsEmpty()});
		}
		for (const auto& Pending : Prepared.PendingAuthorUnits) {
			TPair<FString, FString> Location;
			if (const auto* Produced = Prepared.PendingAuthorLocations.Find(Pending.Key)) Location = *Produced;
			else if (Source) {
				// The zero-actual plan can retain the entire public file. Its
				// original parser child still supplies this field's exact address.
				Pending.Value.VisitValues([&](const FString& Key, const FUeAgentCanonicalValueRef&) {
					if (const auto* Node = Source->FindValueNode(Pending.Key, TEXT("/") + EscapeJsonPointerSegment(Key))) {
						Location.Key = Node->GetProvenance().GetSourceUnit();
						if (!FPaths::IsRelative(Location.Key)) FPaths::MakePathRelativeTo(Location.Key, *(Prepared.AcceptedWorkspaceRoot + TEXT("/")));
						FPaths::NormalizeFilename(Location.Key);
						Location.Value = Node->GetProvenance().GetJsonPointer();
						int32 Slash;
						if (Location.Value.FindLastChar(TEXT('/'), Slash)) Location.Value = Location.Value.Left(Slash);
						return false;
					}
					return true;
				});
			}
			if (Location.Key.IsEmpty()) {
				Error.SetSourceFailure(TEXT("workspace_committed_author_location_missing"),
					TEXT("The pending field has no producer-bound public location; author bytes and consumption were retained."), Pending.Key);
				return false;
			}
			TSharedPtr<FJsonObject> Compact;
			if (!TryBuildCompactRow(Pending.Value, Compact, Error, nullptr, Location.Key) || !Compact) return false;
			if (Pending.Value.GetUnitKind() == ESemanticUnitKind::PropertySet && !IsActorComponentHierarchyItemUnit(Pending.Value)) {
				Contract::FDiagnostic ProjectionError;
				const auto Value = DirectPropertyValue(Compact, Pending.Value, *ActualModel, ProjectionError);
				FUeAgentCanonicalValuePtr Canonical;
				if (!Value || !TryCanonicalizeJson(Value, Resources, Canonical, Error)) {
					if (!Error.HasErrors()) Error.SetFailure(MoveTemp(ProjectionError), Pending.Key);
					return false;
				}
				Patches.FindOrAdd(Location.Key).Add({(Location.Value == TEXT("/") ? FString() : Location.Value) + TEXT("/value"), Canonical.ToSharedRef()});
			} else {
				const TSharedPtr<FJsonObject>* Values = nullptr;
				TSharedRef<FJsonObject> Visible = MakeShared<FJsonObject>();
				if (!Compact->TryGetObjectField(TEXT("values"), Values)
					|| !TryProjectItemVisibleObject(Pending.Key, Values->ToSharedRef(), GetActorComponentHierarchyItemVisibleFieldPolicy(), Visible, Error)) return false;
				if (Visible->Values.IsEmpty()) {
					Error.SetSourceFailure(TEXT("workspace_committed_author_projection_missing"),
						TEXT("This pending author value has no public projection. Its current bytes and the commit's consumption position were retained."), Pending.Key);
					return false;
				}
				for (const auto& Field : Visible->Values) {
					FUeAgentCanonicalValuePtr Canonical;
					if (!TryCanonicalizeJson(Field.Value, Resources, Canonical, Error)) return false;
					Patches.FindOrAdd(Location.Key).Add({(Location.Value == TEXT("/") ? FString() : Location.Value)
						+ TEXT("/") + EscapeJsonPointerSegment(Field.Key), Canonical.ToSharedRef()});
				}
			}
		}
		if (Patches.IsEmpty()) {
			Error.SetSourceFailure(TEXT("workspace_committed_author_projection_missing"),
				TEXT("The known pending author input produced no public writeback or retained document. Publication was not advanced."), Prepared.AcceptedWorkspaceRoot);
			return false;
		}
		TArray<FExportFilePlan> Files = Prepared.Plan->GetFiles();
		TMap<FString, FString> Retained = Prepared.Plan->GetRetainedFileHashes();
		int32 Rewritten = 0, Kept = 0;
		for (const auto& Document : Patches) {
			const int32 FileIndex = Files.IndexOfByPredicate([&](const auto& File) { return File.GetRelativePath() == Document.Key; });
			if (FileIndex == INDEX_NONE) {
				const auto* CurrentHash = FindAcceptedSemanticSourceHash(Prepared.AcceptedWorkspaceRoot, Document.Key);
				if (!Retained.Contains(Document.Key) || !CurrentHash) {
					Error.SetSourceFailure(TEXT("workspace_committed_author_location_missing"), TEXT("The pending public document is not in this publication's actual file set."), Document.Key);
					return false;
				}
				FString CurrentByteHash;
				if (!TryReadAcceptedSemanticSourceByteHash(Prepared.AcceptedWorkspaceRoot, Document.Key, CurrentByteHash, Error)) return false;
				Retained.Add(Document.Key, MoveTemp(CurrentByteHash)); ++Kept; continue;
			}
			const auto& Bytes = Files[FileIndex].GetCanonicalUtf8();
			FHjsonDocumentPtr Parsed;
			const FSourceProvenance Span(Document.Key, FString(), 0, 0, 0, Bytes.Num(), FString());
			FUeAgentCanonicalValuePtr Root;
			if (!TryParseGeneratedHjsonSpan(Bytes, Span, Resources, Parsed, Error)
				|| !TryProjectHjsonCanonicalValue(Parsed->GetRoot().Get(), Resources, Root, Error, false)) return false;
			for (const auto& Patch : Document.Value) {
				if (Patch.bKeyed) {
					const auto* Existing = Parsed->Find(Patch.Pointer);
					const auto* Operation = Patch.Value->GetKind() == EUeAgentValueKind::Object
						? Patch.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("operation"); }) : nullptr;
					const FString Kind = Operation && Operation->Value->GetKind() == EUeAgentValueKind::String ? Operation->Value->GetText() : TEXT("update");
					if ((Kind == TEXT("create") && Existing) || (Kind != TEXT("create") && !Existing)) {
						Error.SetSourceFailure(TEXT("workspace_committed_author_structure_conflict"),
							TEXT("The pending keyed action conflicts with actual member presence. Author bytes and the whole commit's consumption position remain unchanged."), Document.Key, Patch.Pointer);
						return false;
					}
				}
				FUeAgentCanonicalValuePtr Changed;
				if (!TryReplacePublishedObject(Root.ToSharedRef(), Patch.Pointer, Patch.Value, Resources, Changed, Error, Document.Key)) {
					if (!Error.HasErrors()) Error.SetSourceFailure(TEXT("workspace_committed_author_location_missing"), TEXT("The formatter's public parent no longer contains the pending field address."), Document.Key, Patch.Pointer);
					return false;
				}
				Root = MoveTemp(Changed);
			}
			TArray<uint8> Updated;
			if (!TrySerializeSemanticHjsonObject(*Root, Updated, Error, Document.Key)) return false;
			const FString Hash = Sha256(Updated);
			Files[FileIndex] = FExportFilePlan(Document.Key, MoveTemp(Updated), Hash); ++Rewritten;
			Files[FileIndex].SetSemanticSourceDocument(true);
		}
		FString Material = Prepared.Plan->GetPlanFingerprint();
		for (const auto& File : Files) Material += TEXT("|") + File.GetRelativePath() + TEXT("|") + File.GetContentHash();
		TArray<FString> Paths; Retained.GenerateKeyArray(Paths); Paths.Sort();
		for (const auto& Path : Paths) Material += TEXT("|") + Path + TEXT("|") + Retained.FindChecked(Path);
		Prepared.Plan = MakeShared<FExportWorkspacePlan>(Prepared.Plan->GetWorkspaceKind(), Prepared.Plan->GetEntryFile(),
			Prepared.Plan->GetSnapshotId(), Prepared.Plan->GetSnapshotRevision(), Prepared.Plan->GetSnapshotEvidenceId(),
			MoveTemp(Files), Sha256Text(Material), Prepared.Plan->GetObservedTokens(), MoveTemp(Retained));
		for (const auto& Document : Patches) OutPendingDocuments.Add(Document.Key);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=pending_author_writeback units=%d rewritten_documents=%d retained_documents=%d actual_baseline_unchanged=true"),
			Prepared.PendingAuthorUnits.Num() + Prepared.PendingKeyedAuthorRows.Num(), Rewritten, Kept);
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
		if (PreparedExport) { PreparedExport->Plan.Reset(); PreparedExport->PublisherOwnerId.Reset(); }
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
			WorkspaceError, !PreparedExport || !PreparedExport->Publication.bDirectDataflow)
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

	bool TryPrepareCompositionExportState(const FString& TargetKind, const FAuthoringModel& Model,
		const FCompositionSnapshotBinding& SnapshotBinding, FUeAgentResourceTracker& Resources,
		FPreparedWorkspaceExport* PreparedExport, FWorkspaceError& Error)
	{
		if (!PreparedExport) return true;
		if (PreparedExport->SourcePublisherOwnerId.IsEmpty()) {
			const FString Root = PreparedExport->AcceptedWorkspaceRoot.IsEmpty()
				? GetAcceptedSemanticWorkspaceRoot() : PreparedExport->AcceptedWorkspaceRoot;
			if (!Root.IsEmpty() && IFileManager::Get().DirectoryExists(*Root)) {
				if (PreparedExport->Publication.bDirectDataflow) {
					PreparedExport->SourcePublisherOwnerId = GetAcceptedSemanticPublisherOwner(Root);
					if (PreparedExport->SourcePublisherOwnerId.IsEmpty()) {
						Error.SetSourceFailure(TEXT("workspace_source_handoff_missing"),
							TEXT("composition.publication_owner: accepted Source did not supply its publisher identity; source.publication_directory must provide it before output planning."), Root);
						return false;
					}
				} else {
				FString State;
				FArtifactPublishError PublishError;
				if (!TryReadWorkspaceConsumerState(Root, FString(), State, PreparedExport->SourcePublisherOwnerId, PublishError)) {
					Error.SetFailure(MoveTemp(PublishError)); return false;
				}
				}
			}
		}
		if (!PreparedExport->CommittedCaptureFence) return true;
		const bool bSupported = HasCommittedExportConsumer(TargetKind);
		if (PreparedExport->bCommittedRequested && !bSupported) {
			Error.SetFailure(Contract::FDiagnostic::Failure(TEXT("workspace_committed_domain_unsupported"),
				TEXT("This domain has no committed-change capture. Choose incremental or full; the existing workspace was not changed."), TEXT("workspace_export")));
			return false;
		}
		if (bSupported) {
			if (!TryMakeCommittedExportState(TargetKind, PreparedExport->CommittedTarget, Model, SnapshotBinding,
				*PreparedExport->CommittedCaptureFence, PreparedExport->ConsumerState, Error, &PreparedExport->Publication)) return false;
		} else {
			// Ordinary producers have no committed stream or watermark. Their
			// valid target shapes must not inherit the Level consumer contract.
			PreparedExport->ConsumerState.Reset();
		}
		if (PreparedExport->bCommittedRequested) {
			PreparedExport->ActualExportMode = TEXT("committed");
		}
		return true;
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
		if (PreparedExport) { PreparedExport->Plan.Reset(); PreparedExport->PublisherOwnerId.Reset(); }
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutResult.Status = TEXT("failed");
		FWorkspaceError WorkspaceError;
		FExportWorkspacePlanPtr Plan;
		if (!TryPrepareCompositionExportState(TargetKind, *Model, SnapshotBinding, Resources, PreparedExport, WorkspaceError)) {
			OutResult.Diagnostic = WorkspaceError.GetDiagnostic(); return false;
		}
		if (!TryBuildCompositionExportPlan(
			Model,
			SnapshotBinding,
			{},
			Resources,
			Plan,
			WorkspaceError, TargetKind, PreparedExport, PreparedExport ? &PreparedExport->PendingAuthorLocations : nullptr)
			|| !Plan.IsValid())
		{
			OutResult.Diagnostic = WorkspaceError.GetDiagnostic();
			return false;
		}
		const double PlanFinished = FPlatformTime::Seconds();
		const FString SourceRoot = PreparedExport && !PreparedExport->AcceptedWorkspaceRoot.IsEmpty()
			? PreparedExport->AcceptedWorkspaceRoot : WorkspaceRoot;
		if (PreviousCompositionExport && PreviousCompositionExport->SourceLayout
			&& PreviousCompositionExport->ProducedPlan.Pin() == Plan) {
			const auto Rows = PreviousCompositionExport.ToSharedRef();
			const auto& Layout = Rows->SourceLayout;
			const auto& ProducedModel = Layout->GetModel();
			const auto& Units = ProducedModel->GetDeclarations();
			const auto& Inventory = Layout->GetMachineManifest().UnitInventory;
			if (Rows->PublishedRows) Rows->PublishedRows->Root = SourceRoot;
			RememberGeneratedSemanticWorkspace(SourceRoot, ProducedModel, Rows, *Plan,
				[Rows](const FString& Id, FAcceptedWorkspaceByteSpan& Span) { return Rows->FindEnvelopeSpan(Id, Span); },
				[&](TFunctionRef<void(const FAcceptedWorkspaceInventoryRow&)> Emit) {
					const FString Missing;
					const TPair<FIntPoint, FIntPoint> MissingSpans{FIntPoint::ZeroValue, FIntPoint::ZeroValue};
					for (int32 I = 0; I < Units.Num(); ++I) {
						const auto& Unit = Units[I];
						const auto* Pointer = Rows->UnitPointers.Find(Unit.GetUnitId());
						const auto* EditHash = Rows->UnitEditHashes.Find(Unit.GetUnitId());
						const auto& Entry = Inventory[I];
						const auto& Spans = Rows->GeneratedMachineSpans->IsValidIndex(I) ? (*Rows->GeneratedMachineSpans)[I] : MissingSpans;
						const FString& Path = Layout->GetPhysicalPlacements().FindChecked(Unit.GetUnitId());
					const FString Envelope = MachineEnvelopePath(Path, Rows->bScopedMachineBuckets);
						const auto* Content = Rows->UnitContentFiles.Find(Unit.GetUnitId());
						const TArray<FString> EmptyContent;
						Emit({Unit.GetUnitId(), Unit.GetUnitKind(), Layout->GetMachineManifest().PublicRefMapping.FindChecked(Unit.GetUnitId()),
							Layout->GetLogicalPlacements().FindChecked(Unit.GetUnitId()), Path, Pointer ? *Pointer : Missing, Envelope, Content ? *Content : EmptyContent,
							[&]() { return EditHash ? *EditHash : Missing; }, [&](const FString& Hash) { return Entry.ContentHash.Equals(Hash, ESearchCase::CaseSensitive); },
							[&]() { return Entry.ContentHash; }, {Spans.Key.X, Spans.Key.Y}, {Spans.Value.X, Spans.Value.Y}, false, Unit.GetSharedUnitId()});
					}
				}, Rows->Publication);
		}
		else if (PreviousCompositionExport && PreviousCompositionExport->ProducedPlan.Pin() == Plan)
			RetireGeneratedSemanticWorkspace(SourceRoot);
		FArtifactPublishReceipt Receipt;
		FArtifactPublishError PublishError;
		const FString OwnerId = CompositionPublisherOwner(TargetKind, PreparedExport);
		if (PreparedExport && PreparedExport->bPrepareOnly)
		{
			PreparedExport->Plan = Plan;
			PreparedExport->PublisherOwnerId = OwnerId;
			OutResult.Status = TEXT("prepared");
			OutResult.ProfileId = ProfileId;
			OutResult.WorkspaceRoot = WorkspaceRoot;
			OutResult.ExpectedRevision = SnapshotBinding.Revision.GetGeneration();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=workspace_export_prepare target_kind=%s plan_s=%.6f total_s=%.6f"),
				*TargetKind, PlanFinished - ExportStarted, FPlatformTime::Seconds() - ExportStarted);
			return true;
		}
		FArtifactPublishRequest PublishRequest(Plan.ToSharedRef(), WorkspaceRoot, OwnerId);
		ConfigureWorkspacePublication(PreparedExport, PublishRequest);
		if (!TryPublishExportPlan(
			PublishRequest,
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
		const bool bResultFileDetails = !PreparedExport || PreparedExport->bIncludeResultFileDetails;
		if (bResultFileDetails) for (const FExportFilePlan& File : Plan->GetFiles())
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
		if (bResultFileDetails) OutResult.WrittenRelativeFiles = Receipt.GetIncludedFiles();
		if (bResultFileDetails) for (const FString& Relative : Receipt.GetIncludedFiles())
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
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=workspace_export kind=%s profile=%s plan_s=%.6f publish_s=%.6f result_s=%.6f total_s=%.6f success=true result_file_details=%s"),
			*TargetKind, *ProfileId, PlanFinished - ExportStarted,
			PublishFinished - PlanFinished, ExportFinished - PublishFinished,
			ExportFinished - ExportStarted, bResultFileDetails ? TEXT("true") : TEXT("false"));
		return true;
	}

	FReadablePropertyAddress DescribeReadableProperty(const FSemanticUnit& Unit, const FAuthoringModel& Model)
	{
		return {ReadablePropertyCategory(Unit.GetCategoryPath()), ReadablePropertyKey(Unit), ReadablePropertyType(Unit, Model), ReadablePropertyDefinition(Unit, Model)};
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
				UeAgentAuthoringEncodedTypes::FScope::Record(Value, NativeEntry.GetType(),
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
