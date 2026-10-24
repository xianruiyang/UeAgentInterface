#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticPublicReferences.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/UeAgentReadableName.h"

#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringContentLayout.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Misc/Paths.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString WorkspaceSchema =
			TEXT("uai.authoring.workspace/2");
		const FString ManifestPath =
			TEXT(".uai/workspace.hjson");

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString UnitKindName(ESemanticUnitKind Kind)
		{
			switch (Kind)
			{
			case ESemanticUnitKind::WorkspaceIndex:
				return TEXT("workspace_index");
			case ESemanticUnitKind::TargetState:
				return TEXT("target_state");
			case ESemanticUnitKind::EntityState:
				return TEXT("entity_state");
			case ESemanticUnitKind::PropertySet:
				return TEXT("property_set");
			case ESemanticUnitKind::StructureState:
				return TEXT("structure_state");
			case ESemanticUnitKind::ItemState:
				return TEXT("item_state");
			case ESemanticUnitKind::RelationSet:
				return TEXT("relation_set");
			case ESemanticUnitKind::OrderingState:
				return TEXT("ordering_state");
			case ESemanticUnitKind::LayoutState:
				return TEXT("layout_state");
			case ESemanticUnitKind::CollectionState:
				return TEXT("collection_state");
			case ESemanticUnitKind::DependencyState:
				return TEXT("dependency_state");
			case ESemanticUnitKind::ExternalPayloadState:
				return TEXT("external_payload_state");
			case ESemanticUnitKind::OpaqueExtension:
				return TEXT("opaque_extension");
			default:
				return FString();
			}
		}

		bool TrySemanticLeafProjection(const FUeAgentCanonicalValue& Value,
			TFunctionRef<bool(int64)> ChargeMemory, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue.Reset(); OutError.Reset();
			const auto Kind = Value.GetKind();
			if (Kind == EUeAgentValueKind::Decimal)
			{
				const FString& Lexeme = Value.GetLexeme();
				if (Lexeme.Contains(TEXT(".")) || Lexeme.Contains(TEXT("e")) || Lexeme.Contains(TEXT("E"))) return true;
				if (!ChargeMemory(2048 + 4ll * (static_cast<int64>(Lexeme.Len()) + 1) * sizeof(TCHAR))) return false;
				// Keep the decimal kind without introducing a transport object into
				// readable property JSON, which is also consumed directly by codecs.
				OutValue = MakeShared<FJsonValueNumberString>(Lexeme + TEXT(".0"));
				return true;
			}
			if (Kind != EUeAgentValueKind::Temporal && Kind != EUeAgentValueKind::Spatial) return true;
			if (!ChargeMemory(1024) || !CanonicalJson::TryProjectTypedLeaf(Value, ChargeMemory, OutValue, OutError)) return false;
			OutValue->AsObject()->SetStringField(TEXT("$uai"), Kind == EUeAgentValueKind::Temporal ? TEXT("temporal") : TEXT("spatial"));
			return true;
		}

		TSharedPtr<FJsonValue> JsonFromCanonical(
			const FUeAgentCanonicalValue& Root, Contract::FDiagnostic& OutError, const FString& RootPath)
		{
			OutError.Reset();
			struct FVisit
			{
				const FUeAgentCanonicalValue* Value = nullptr;
				bool bExpanded = false;
				FString Path;
			};
			TArray<FVisit> Stack;
			TMap<
				const FUeAgentCanonicalValue*,
				TSharedPtr<FJsonValue>> Converted;
			Stack.Add({&Root, false, RootPath});
			while (!Stack.IsEmpty())
			{
				const FVisit Visit =
					Stack.Pop(EAllowShrinking::No);
				if (Converted.Contains(Visit.Value))
				{
					continue;
				}
				if (!Visit.bExpanded)
				{
					switch (Visit.Value->GetKind())
					{
					case EUeAgentValueKind::Null:
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueNull>());
						break;
					case EUeAgentValueKind::Boolean:
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueBoolean>(
								Visit.Value->GetBoolean()));
						break;
					case EUeAgentValueKind::String:
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueString>(
								Visit.Value->GetText()));
						break;
					case EUeAgentValueKind::Integer:
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueNumberString>(
								Visit.Value->GetLexeme()));
						break;
					case EUeAgentValueKind::Decimal:
					{
						TSharedPtr<FJsonValue> Leaf;
						UE::UAI::Contract::FDiagnostic Error;
						if (!TrySemanticLeafProjection(*Visit.Value, [](int64) { return true; }, Leaf, Error))
						{
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Visit.Path);
							OutError = MoveTemp(Error);
							return nullptr;
						}
						if (!Leaf.IsValid()) Leaf = MakeShared<FJsonValueNumberString>(Visit.Value->GetLexeme());
						Converted.Add(Visit.Value, Leaf);
						break;
					}
					case EUeAgentValueKind::Array:
					case EUeAgentValueKind::Object:
						Stack.Add({Visit.Value, true, Visit.Path});
						if (Visit.Value->GetKind()
							== EUeAgentValueKind::Array)
						{
							const TArray<FUeAgentCanonicalValueRef>&
								Items =
									Visit.Value->GetArrayItems();
							for (int32 Index = Items.Num() - 1;
								Index >= 0;
								--Index)
							{
								Stack.Add(
									{&Items[Index].Get(), false, Visit.Path + TEXT("/") + FString::FromInt(Index)});
							}
						}
						else
						{
							const TArray<TPair<
								FString,
								FUeAgentCanonicalValueRef>>& Items =
									Visit.Value->GetObjectItems();
							for (int32 Index = Items.Num() - 1;
								Index >= 0;
								--Index)
							{
								Stack.Add(
									{&Items[Index].Value.Get(), false, Visit.Path + TEXT("/") + EscapeJsonPointerSegment(Items[Index].Key)});
							}
						}
						break;
					case EUeAgentValueKind::Temporal:
					case EUeAgentValueKind::Spatial:
					{
						TSharedPtr<FJsonValue> Leaf;
						UE::UAI::Contract::FDiagnostic Error;
						if (!TrySemanticLeafProjection(*Visit.Value, [](int64) { return true; }, Leaf, Error))
						{
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Visit.Path);
							OutError = MoveTemp(Error);
							return nullptr;
						}
						Converted.Add(Visit.Value, Leaf);
						break;
					}
					default:
						OutError = Contract::FDiagnostic::Failure(TEXT("semantic_value_kind_unsupported"),
							TEXT("This canonical value kind cannot be projected into a semantic HJSON row."), TEXT("semantic_projection"));
						OutError.Source.JsonPath = Visit.Path;
						OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Visit.Value->GetKind()));
						return nullptr;
					}
					continue;
				}
				if (Visit.Value->GetKind()
					== EUeAgentValueKind::Array)
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const FUeAgentCanonicalValueRef& Item :
						Visit.Value->GetArrayItems())
					{
						Items.Add(Converted.FindChecked(&Item.Get()));
					}
					Converted.Add(
						Visit.Value,
						MakeShared<FJsonValueArray>(
							MoveTemp(Items)));
				}
				else
				{
					TSharedRef<FJsonObject> Object =
						MakeShared<FJsonObject>();
					for (const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Item :
						Visit.Value->GetObjectItems())
					{
						if (Object->Values.Contains(Item.Key))
						{
							OutError = Contract::FDiagnostic::Failure(TEXT("semantic_object_key_duplicate"),
								TEXT("A canonical object contains the same field name more than once."), TEXT("semantic_projection"));
							OutError.Source.JsonPath = Visit.Path + TEXT("/") + EscapeJsonPointerSegment(Item.Key);
							OutError.ActualValue = MakeShared<FJsonValueString>(Item.Key);
							return nullptr;
						}
						Object->SetField(
							Item.Key,
							Converted.FindChecked(
								&Item.Value.Get()));
					}
					Converted.Add(
						Visit.Value,
						MakeShared<FJsonValueObject>(Object));
				}
			}
			return Converted.FindChecked(&Root);
		}

		TArray<TSharedPtr<FJsonValue>> JsonStringArray(
			const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> PublicRefArray(
			const TArray<FString>& Values,
			const TMap<FString, FString>* PublicRefs)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				const FString* Public = PublicRefs == nullptr
					? nullptr : PublicRefs->Find(Value);
				Result.Add(MakeShared<FJsonValueString>(
					Public == nullptr ? Value : *Public));
			}
			return Result;
		}

		void ProjectPublicRefs(
			TSharedPtr<FJsonValue>& Value,
			const TMap<FString, FString>& PublicRefs)
		{
			if (!Value.IsValid())
			{
				return;
			}
			if (Value->Type == EJson::String)
			{
				if (const FString* Public = PublicRefs.Find(Value->AsString()))
				{
					Value = MakeShared<FJsonValueString>(*Public);
				}
				return;
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items = Value->AsArray();
				for (TSharedPtr<FJsonValue>& Item : Items)
				{
					ProjectPublicRefs(Item, PublicRefs);
				}
				Value = MakeShared<FJsonValueArray>(MoveTemp(Items));
				return;
			}
			if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Source = Value->AsObject();
				const TSharedRef<FJsonObject> Projected = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
					: Source->Values)
				{
					TSharedPtr<FJsonValue> ProjectedValue = Field.Value;
					ProjectPublicRefs(ProjectedValue, PublicRefs);
					const FString* PublicKey = PublicRefs.Find(Field.Key);
					Projected->SetField(
						PublicKey == nullptr ? Field.Key : *PublicKey,
						MoveTemp(ProjectedValue));
				}
				Value = MakeShared<FJsonValueObject>(Projected);
			}
		}

		TSharedPtr<FJsonObject> SemanticUnitJson(
			const FSemanticUnit& Unit, FWorkspaceError& OutError)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("unit_id"), Unit.GetUnitId());
			Row->SetStringField(
				TEXT("unit_kind"),
				UnitKindName(Unit.GetUnitKind()));
			Row->SetStringField(TEXT("owner_ref"), Unit.GetOwnerRef());
			Row->SetStringField(
				TEXT("semantic_type_ref"),
				Unit.GetSemanticTypeRef());
			Row->SetStringField(TEXT("role"), Unit.GetRole());
			Row->SetStringField(
				TEXT("field_set_ref"),
				Unit.GetFieldSetRef());
			TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
			TArray<FString> ValueKeys;
			Unit.GetValues().GetKeys(ValueKeys);
			ValueKeys.Sort();
			for (const FString& Key : ValueKeys)
			{
				Contract::FDiagnostic Error;
				TSharedPtr<FJsonValue> Value = JsonFromCanonical(
					Unit.GetValues().FindChecked(Key).Get(), Error, TEXT("/values/") + EscapeJsonPointerSegment(Key));
				if (!Value.IsValid())
				{
					OutError.SetFailure(MoveTemp(Error), Unit.GetUnitId());
					return nullptr;
				}
				Values->SetField(Key, MoveTemp(Value));
			}
			Row->SetObjectField(TEXT("values"), Values);
			if (!Unit.GetItemRefs().IsEmpty())
			{
				Row->SetArrayField(
					TEXT("item_refs"),
					JsonStringArray(Unit.GetItemRefs()));
			}
			if (!Unit.GetRelationRefs().IsEmpty())
			{
				Row->SetArrayField(
					TEXT("relation_refs"),
					JsonStringArray(Unit.GetRelationRefs()));
			}
			if (!Unit.GetOrderingRef().IsEmpty())
			{
				Row->SetStringField(
					TEXT("ordering_ref"),
					Unit.GetOrderingRef());
			}
			if (!Unit.GetLayoutRef().IsEmpty())
			{
				Row->SetStringField(
					TEXT("layout_ref"),
					Unit.GetLayoutRef());
			}
			if (!Unit.GetDependencyRefs().IsEmpty())
			{
				Row->SetArrayField(
					TEXT("dependency_refs"),
					JsonStringArray(Unit.GetDependencyRefs()));
			}
			Row->SetArrayField(
				TEXT("profile_visibility"),
				JsonStringArray(Unit.GetProfileVisibility()));
			if (!Unit.GetTargetIdentityKey().IsEmpty())
			{
				Row->SetStringField(
					TEXT("target_identity_key"),
					Unit.GetTargetIdentityKey());
			}
			return Row;
		}

		TSharedPtr<FJsonObject> SemanticModelUnitJson(
			const FSemanticUnit& Unit, FWorkspaceError& OutError,
			const TMap<FString, FString>* PublicRefs = nullptr)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
			TArray<FString> ValueKeys;
			Unit.GetValues().GetKeys(ValueKeys);
			ValueKeys.Sort();
			for (const FString& Key : ValueKeys)
			{
				Contract::FDiagnostic Error;
				TSharedPtr<FJsonValue> Value = JsonFromCanonical(
					Unit.GetValues().FindChecked(Key).Get(), Error, TEXT("/values/") + EscapeJsonPointerSegment(Key));
				if (!Value.IsValid())
				{
					OutError.SetFailure(MoveTemp(Error), Unit.GetUnitId());
					return nullptr;
				}
				if (PublicRefs != nullptr)
				{
					ProjectPublicRefs(Value, *PublicRefs);
				}
				Values->SetField(Key, MoveTemp(Value));
			}
			Row->SetObjectField(TEXT("values"), Values);
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState && !Unit.GetFieldSetRef().IsEmpty())
			{
				const FString* PublicRef = PublicRefs ? PublicRefs->Find(Unit.GetFieldSetRef()) : nullptr;
				Row->SetStringField(TEXT("definition"), PublicRef ? *PublicRef : Unit.GetFieldSetRef());
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& !Unit.GetFieldSetRef().IsEmpty())
			{
				Row->SetStringField(TEXT("field_set_ref"), Unit.GetFieldSetRef());
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& !Unit.GetDependencyRefs().IsEmpty())
			{
				Row->SetArrayField(
					TEXT("dependency_refs"),
					PublicRefArray(Unit.GetDependencyRefs(), PublicRefs));
			}
			return Row;
		}

		bool NormalizeSet(TArray<FString>& Values, Contract::FDiagnostic& Error,
			const FString& Code, const FString& Field, bool bPublicValues = false)
		{
			TSet<FString> Seen;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				FString& Value = Values[Index];
				Value = Normalize(MoveTemp(Value));
				if (Value.IsEmpty() || Seen.Contains(Value)) {
					Error = Contract::FDiagnostic::Failure(Code, Value.IsEmpty()
						? TEXT("The reference list contains an empty item.") : TEXT("The reference list contains a duplicate item."), TEXT("semantic_model"));
					Error.SemanticPointer = TEXT("/") + Field + TEXT("/") + FString::FromInt(Index);
					if (bPublicValues || Value.IsEmpty()) Error.ActualValue = MakeShared<FJsonValueString>(Value);
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty unique reference"));
					return false;
				}
				Seen.Add(Value);
			}
			Values.Sort();
			return true;
		}

		void AppendToken(
			FString& Material,
			const TCHAR* Prefix,
			const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value, Value.Len());
			Material.Append(Prefix);
			Material.AppendInt(Utf8.Length());
			Material.AppendChar(TEXT(':'));
			Material.Append(Value);
			Material.AppendChar(TEXT(';'));
		}

		void AppendStringArray(
			FString& Material,
			const TArray<FString>& Values)
		{
			Material.AppendChar(TEXT('a'));
			Material.AppendInt(Values.Num());
			Material.AppendChar(TEXT('['));
			for (const FString& Value : Values)
			{
				AppendToken(Material, TEXT("s"), Value);
			}
			Material.AppendChar(TEXT(']'));
		}

		struct FValueEvent
		{
			const FUeAgentCanonicalValue* Value = nullptr;
			FString Literal;

			static FValueEvent ForValue(
				const FUeAgentCanonicalValue& InValue)
			{
				FValueEvent Event;
				Event.Value = &InValue;
				return Event;
			}

			static FValueEvent ForLiteral(FString InLiteral)
			{
				FValueEvent Event;
				Event.Literal = MoveTemp(InLiteral);
				return Event;
			}
		};

		FString StringToken(const FString& Value)
		{
			FString Result;
			AppendToken(Result, TEXT("s"), Value);
			return Result;
		}

		void AppendTemporalMaterial(
			FString& Material,
			const FUeAgentTemporalPoint& Value)
		{
			Material.Append(TEXT("temporal{"));
			AppendToken(Material, TEXT("s"), Value.GetClockId());
			AppendToken(Material, TEXT("s"), Value.GetTick());
			AppendToken(Material, TEXT("s"), Value.GetRateNumerator());
			AppendToken(Material, TEXT("s"), Value.GetRateDenominator());
			AppendToken(Material, TEXT("s"), Value.GetState());
			Material.AppendChar(TEXT('}'));
		}

		void AppendSpatialMaterial(
			FString& Material,
			const FUeAgentSpatialValue& Value)
		{
			Material.Append(TEXT("spatial{"));
			AppendToken(Material, TEXT("s"), Value.GetSubject());
			Material.AppendInt(
				static_cast<int32>(Value.GetQuantityKind()));
			const FUeAgentSpatialFrame& Frame = Value.GetFrame();
			AppendToken(Material, TEXT("s"), Frame.GetFrameId());
			AppendToken(Material, TEXT("s"), Frame.GetBasisFrameId());
			Material.AppendInt(Frame.GetDimension());
			Material.Append(TEXT("basis["));
			for (const TArray<FString>& Row : Frame.GetBasis())
			{
				AppendStringArray(Material, Row);
			}
			Material.AppendChar(TEXT(']'));
			AppendStringArray(Material, Frame.GetOrigin());
			AppendToken(Material, TEXT("s"), Frame.GetUnit());
			AppendToken(Material, TEXT("s"), Frame.GetBasisEvidence());
			AppendTemporalMaterial(Material, Frame.GetObservedAt());
			AppendStringArray(Material, Value.GetComponents());
			AppendTemporalMaterial(Material, Value.GetObservedAt());
			AppendStringArray(
				Material,
				Value.GetConversionEvidence());
			Material.AppendChar(TEXT('}'));
		}

		void AppendCanonicalValueMaterial(
			FString& Material,
			const FUeAgentCanonicalValue& Root)
		{
			TArray<FValueEvent> Stack;
			Stack.Reserve(32);
			Stack.Add(FValueEvent::ForValue(Root));
			while (!Stack.IsEmpty())
			{
				FValueEvent Event = Stack.Pop(EAllowShrinking::No);
				if (Event.Value == nullptr)
				{
					Material.Append(Event.Literal);
					continue;
				}
				const FUeAgentCanonicalValue& Value = *Event.Value;
				switch (Value.GetKind())
				{
				case EUeAgentValueKind::Null:
					Material.Append(TEXT("n;"));
					break;
				case EUeAgentValueKind::Boolean:
					Material.Append(
						Value.GetBoolean()
							? TEXT("b1;")
							: TEXT("b0;"));
					break;
				case EUeAgentValueKind::String:
					AppendToken(
						Material,
						TEXT("s"),
						Value.GetText());
					break;
				case EUeAgentValueKind::Integer:
					AppendToken(
						Material,
						TEXT("i"),
						Value.GetLexeme());
					break;
				case EUeAgentValueKind::Decimal:
					AppendToken(
						Material,
						TEXT("d"),
						Value.GetLexeme());
					break;
				case EUeAgentValueKind::Array:
				{
					const TArray<FUeAgentCanonicalValueRef>& Items =
						Value.GetArrayItems();
					Material.AppendChar(TEXT('a'));
					Material.AppendInt(Items.Num());
					Material.AppendChar(TEXT('['));
					Stack.Add(FValueEvent::ForLiteral(TEXT("]")));
					for (int32 Index = Items.Num() - 1;
						Index >= 0;
						--Index)
					{
						Stack.Add(
							FValueEvent::ForValue(Items[Index].Get()));
						if (Index > 0)
						{
							Stack.Add(
								FValueEvent::ForLiteral(TEXT(",")));
						}
					}
					break;
				}
				case EUeAgentValueKind::Object:
				{
					TArray<const TPair<
						FString,
						FUeAgentCanonicalValueRef>*> Items;
					for (const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Item
						: Value.GetObjectItems())
					{
						Items.Add(&Item);
					}
					Items.Sort(
						[](const auto& Left, const auto& Right)
						{
							return Left.Key < Right.Key;
						});
					Material.AppendChar(TEXT('o'));
					Material.AppendInt(Items.Num());
					Material.AppendChar(TEXT('{'));
					Stack.Add(FValueEvent::ForLiteral(TEXT("}")));
					for (int32 Index = Items.Num() - 1;
						Index >= 0;
						--Index)
					{
						Stack.Add(FValueEvent::ForValue(
							Items[Index]->Value.Get()));
						Stack.Add(FValueEvent::ForLiteral(
							StringToken(Items[Index]->Key)));
						if (Index > 0)
						{
							Stack.Add(
								FValueEvent::ForLiteral(TEXT(",")));
						}
					}
					break;
				}
				case EUeAgentValueKind::Temporal:
					AppendTemporalMaterial(
						Material,
						*Value.GetTemporal());
					break;
				case EUeAgentValueKind::Spatial:
					AppendSpatialMaterial(
						Material,
						*Value.GetSpatial());
					break;
				default:
					Material.Append(TEXT("invalid;"));
					break;
				}
			}
		}

		FString HashBytes(const uint8* Data, int32 Size)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			SHA256(Data, Size, Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (uint8 Byte : Digest)
			{
				Hex.Appendf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}
		FString HashText(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value, Value.Len());
			return HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		}

		FString UnitMaterial(const FSemanticUnit& Unit)
		{
			FString Material;
			AppendToken(Material, TEXT("s"), Unit.GetUnitId());
			AppendToken(
				Material,
				TEXT("s"),
				UnitKindName(Unit.GetUnitKind()));
			AppendToken(Material, TEXT("s"), Unit.GetOwnerRef());
			AppendToken(
				Material,
				TEXT("s"),
				Unit.GetSemanticTypeRef());
			AppendToken(Material, TEXT("s"), Unit.GetRole());
			AppendToken(Material, TEXT("s"), Unit.GetFieldSetRef());
			TArray<FString> Keys;
			Unit.GetValues().GetKeys(Keys);
			Keys.Sort();
			Material.Append(TEXT("values{"));
			for (const FString& Key : Keys)
			{
				AppendToken(Material, TEXT("s"), Key);
				AppendCanonicalValueMaterial(
					Material,
					Unit.GetValues().FindChecked(Key).Get());
			}
			Material.AppendChar(TEXT('}'));
			AppendStringArray(Material, Unit.GetItemRefs());
			AppendStringArray(Material, Unit.GetRelationRefs());
			AppendToken(Material, TEXT("s"), Unit.GetOrderingRef());
			AppendToken(Material, TEXT("s"), Unit.GetLayoutRef());
			AppendStringArray(Material, Unit.GetDependencyRefs());
			AppendStringArray(
				Material,
				Unit.GetProfileVisibility());
			AppendToken(
				Material,
				TEXT("s"),
				Unit.GetTargetIdentityKey());
			return Material;
		}

		bool Fail(
			FWorkspaceError& OutError,
			const FString& Code,
			const FString& Reason,
			const FString& SourceUnit = FString(),
			TArray<FString> CyclePath = {})
		{
			OutError.SetSourceFailure(
				Code, Reason,
				SourceUnit,
				TEXT("/"),
				0,
				false,
				MoveTemp(CyclePath));
			return false;
		}

		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceChargeError& Error)
		{
			OutError.SetFailure(Error);
			return false;
		}

		TArray<FString> UnitReferences(const FSemanticUnit& Unit)
		{
			TArray<FString> Result;
			if (!Unit.GetOwnerRef().IsEmpty())
			{
				Result.Add(Unit.GetOwnerRef());
			}
			Result.Append(Unit.GetItemRefs());
			Result.Append(Unit.GetRelationRefs());
			if (!Unit.GetOrderingRef().IsEmpty())
			{
				Result.Add(Unit.GetOrderingRef());
			}
			if (!Unit.GetLayoutRef().IsEmpty())
			{
				Result.Add(Unit.GetLayoutRef());
			}
			Result.Append(Unit.GetDependencyRefs());
			return Result;
		}

		FString SafeSegment(const FString& Value)
		{
			const FString Lower = Value.ToLower();
			FString Result;
			bool bLastWasDash = false;
			for (TCHAR Character : Lower)
			{
				const bool bAllowed =
				FChar::IsAlnum(Character)
					|| Character == TEXT('.')
					|| Character == TEXT('_')
					|| Character == TEXT('-');
				if (bAllowed)
				{
					Result.AppendChar(Character);
					bLastWasDash = Character == TEXT('-');
				}
				else if (!bLastWasDash)
				{
					Result.AppendChar(TEXT('-'));
					bLastWasDash = true;
				}
			}
			Result.TrimStartAndEndInline();
			Result.TrimCharInline(TEXT('.'), nullptr);
			Result.TrimCharInline(TEXT('-'), nullptr);
			Result.TrimCharInline(TEXT('_'), nullptr);
			return Result.IsEmpty() ? Result : MakeAuthoringContentPathSegment(Result);
		}

		bool IsExplicitGuidName(const FString& Candidate)
		{
			FGuid Guid;
			return (FGuid::ParseExact(Candidate, EGuidFormats::Digits, Guid)
				|| FGuid::ParseExact(Candidate, EGuidFormats::DigitsWithHyphens, Guid)) && Guid.IsValid();
		}

		FString ReadObjectString(
			const FUeAgentCanonicalValue& Value,
			const TCHAR* Field)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object)
			{
				return FString();
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Value.GetObjectItems())
			{
				if (Item.Key == Field
					&& Item.Value->GetKind() == EUeAgentValueKind::String)
				{
					return Item.Value->GetText().TrimStartAndEnd();
				}
			}
			return FString();
		}

		FString ReadSemanticString(
			const FSemanticUnit& Unit,
			const TCHAR* Field)
		{
			if (const FUeAgentCanonicalValueRef* Direct =
				Unit.GetValues().Find(Field))
			{
				if ((*Direct)->GetKind() == EUeAgentValueKind::String)
				{
					const FString Name = (*Direct)->GetText().TrimStartAndEnd();
					if (!Name.IsEmpty()) return Name;
				}
			}

			TArray<FString> RootFields;
			Unit.GetValues().GetKeys(RootFields);
			RootFields.Sort();
			for (const FString& RootField : RootFields)
			{
				const FUeAgentCanonicalValueRef& Value =
					Unit.GetValues().FindChecked(RootField);
				FString Result = ReadObjectString(Value.Get(), Field);
				if (!Result.IsEmpty())
				{
					return Result;
				}
			}
			return FString();
		}

		FString ReadPathLeaf(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ReplaceInline(TEXT("\\"), TEXT("/"));
			int32 Separator = INDEX_NONE;
			if (Value.FindLastChar(TEXT('/'), Separator))
			{
				Value = Value.Mid(Separator + 1);
			}
			int32 ObjectSeparator = INDEX_NONE;
			int32 SubobjectSeparator = INDEX_NONE;
			if (Value.FindChar(TEXT(':'), SubobjectSeparator))
			{
				// A subobject path ends in its object name, not the owning asset.
				Value = Value.Mid(SubobjectSeparator + 1);
				if (Value.FindLastChar(TEXT('.'), ObjectSeparator))
					Value = Value.Mid(ObjectSeparator + 1);
				return Value.TrimStartAndEnd();
			}
			if (Value.FindChar(TEXT('.'), ObjectSeparator)
				&& ObjectSeparator > 0)
			{
				Value = Value.Left(ObjectSeparator);
			}
			return Value.TrimStartAndEnd();
		}

		FString SemanticBusinessName(const FSemanticUnit& Unit)
		{
			// Aggregated item files and their semantic references use one naming owner.
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState)
			{
				for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
				{
					if (Contract.AggregatedItemSemanticSuffix.IsEmpty()
						|| !Unit.GetSemanticTypeRef().EndsWith(Contract.AggregatedItemSemanticSuffix)) continue;
					for (const FString& Field : Contract.NamingPolicy.Fields)
					{
						const FString Candidate = MakeReadableSemanticAlias(ReadSemanticString(Unit, *Field));
						if (!Candidate.IsEmpty()) return Candidate;
					}
					break;
				}
			}
			for (const TCHAR* Field : {
				TEXT("name"), TEXT("label"), TEXT("display_name"),
				TEXT("asset_name"), TEXT("component_name"),
				TEXT("event_name"), TEXT("function_name"),
				TEXT("variable_name"), TEXT("node_name"),
				TEXT("candidate_name"), TEXT("renderer_name"), TEXT("title")})
			{
				const FString Candidate = MakeReadableSemanticAlias(
					ReadSemanticString(Unit, Field));
				if (!Candidate.IsEmpty())
				{
					return Candidate;
				}
			}
			return FString();
		}

		FString SemanticAliasBase(const FSemanticUnit& Unit)
		{
			const FString BusinessName = SemanticBusinessName(Unit);
			if (!BusinessName.IsEmpty()) return BusinessName;
			if (Unit.GetRole() == TEXT("property_definition"))
			{
				// Definitions share their captured type name across every consumer;
				// the interned identity remains solely in the machine ref mapping.
				for (const TCHAR* Field : {TEXT("struct_path"), TEXT("enum_path"),
					TEXT("class_path"), TEXT("type_id"), TEXT("kind")})
				{
					const FString TypeName = ReadPathLeaf(ReadSemanticString(Unit, Field));
					if (!TypeName.IsEmpty() && !IsOpaqueSemanticIdentity(TypeName))
						return MakeReadableSemanticAlias(TypeName);
				}
			}
			for (const TCHAR* Field : {
				TEXT("asset_path"), TEXT("object_path"), TEXT("target_ref")})
			{
				const FString Source = ReadPathLeaf(ReadSemanticString(Unit, Field));
				if (IsOpaqueSemanticIdentity(Source)) continue;
				const FString Candidate = MakeReadableSemanticAlias(Source);
				if (!Candidate.IsEmpty())
				{
					return Candidate;
				}
			}

			for (const FString& Fallback : {
				Unit.GetFieldSetRef(), Unit.GetRole(),
				Unit.GetSemanticTypeRef(), UnitKindName(Unit.GetUnitKind())})
			{
				if (IsOpaqueSemanticIdentity(Fallback)) continue;
				const FString Candidate = MakeReadableSemanticAlias(Fallback);
				if (!Candidate.IsEmpty())
				{
					return Candidate;
				}
			}
			return TEXT("unit");
		}

		FString SemanticAliasScope(const FSemanticUnit& Unit)
		{
			if (Unit.GetUnitKind() == ESemanticUnitKind::StructureState
				&& IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef()))
			{
				return TEXT("graphs");
			}
			switch (Unit.GetUnitKind())
			{
			case ESemanticUnitKind::TargetState:
				return TEXT("targets");
			case ESemanticUnitKind::EntityState:
				return TEXT("entities");
			case ESemanticUnitKind::StructureState:
				return TEXT("structures");
			case ESemanticUnitKind::ExternalPayloadState:
				return TEXT("external");
			case ESemanticUnitKind::OpaqueExtension:
				return TEXT("extensions");
			default:
				return UnitKindName(Unit.GetUnitKind())
					+ TEXT("/") + Unit.GetOwnerRef();
			}
		}

		bool TryBuildReadableAliases(
			const FAuthoringModel& Model,
			TMap<FString, FString>& OutAliases,
			FWorkspaceError& OutError)
		{
			struct FAliasInput
			{
				const FSemanticUnit* Unit = nullptr;
				FString Scope;
				FString Base;
			};
			TArray<FAliasInput> Inputs;
			TMap<FString, TSet<FString>> ReservedByScope;
			for (const FSemanticUnit& Unit : Model.GetUnits())
			{
				const FString Scope = SemanticAliasScope(Unit);
				const FString Base = SemanticAliasBase(Unit);
				Inputs.Add({&Unit, Scope, Base});
				ReservedByScope.FindOrAdd(Scope).Add(Base.ToLower());
			}
			Inputs.Sort([](const FAliasInput& Left, const FAliasInput& Right)
			{
				if (Left.Scope != Right.Scope)
				{
					return Left.Scope < Right.Scope;
				}
				const FString LeftBase = Left.Base.ToLower();
				const FString RightBase = Right.Base.ToLower();
				return LeftBase == RightBase
					? Left.Unit->GetUnitId() < Right.Unit->GetUnitId()
					: LeftBase < RightBase;
			});

			OutAliases.Reset();
			TMap<FString, int32> Occurrences;
			TMap<FString, TSet<FString>> UsedByScope;
			for (const FAliasInput& Input : Inputs)
			{
				const FString GroupKey =
					Input.Scope + TEXT("\n") + Input.Base.ToLower();
				int32& Occurrence = Occurrences.FindOrAdd(GroupKey);
				TSet<FString>& Used = UsedByScope.FindOrAdd(Input.Scope);
				FString Alias = AllocateReadableName(Input.Base, [&](const FString& Candidate) {
					return Used.Contains(Candidate.ToLower())
						|| (!Candidate.Equals(Input.Base, ESearchCase::IgnoreCase)
							&& ReservedByScope.FindChecked(Input.Scope).Contains(Candidate.ToLower()));
				}, ++Occurrence);
				if (Alias.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("semantic_alias_missing"), TEXT("A readable alias could not be allocated for the semantic unit within its scope."),
						Input.Unit->GetUnitId());
				}
				Used.Add(Alias.ToLower());
				OutAliases.Add(Input.Unit->GetUnitId(), MoveTemp(Alias));
			}
			return true;
		}

		FString OwnerBase(
			const FSemanticUnit& Unit,
			const FAuthoringModel& Model,
			const TMap<FString, FString>& Aliases,
			FWorkspaceError& OutError)
		{
			if (Unit.GetOwnerRef().IsEmpty())
			{
				return TEXT("units/")
					+ Aliases.FindChecked(Unit.GetUnitId());
			}
			const FSemanticUnit* Owner =
				Model.FindUnit(Unit.GetOwnerRef());
			check(Owner != nullptr);
			const FString OwnerHandle =
				Aliases.FindChecked(Owner->GetUnitId());
			const FSemanticUnit* Anchor = Owner;
			TArray<const FSemanticUnit*> OwnerChain;
			TSet<FString> Seen;
			while (Anchor->GetUnitKind()
				!= ESemanticUnitKind::TargetState
				&& Anchor->GetUnitKind()
					!= ESemanticUnitKind::EntityState
				&& Anchor->GetUnitKind()
					!= ESemanticUnitKind::StructureState)
			{
				if (Seen.Contains(Anchor->GetUnitId()))
				{
					Fail(
						OutError,
						TEXT("semantic_owner_cycle"), TEXT("The semantic unit owner references contain a dependency cycle."),
						Unit.GetUnitId());
					return FString();
				}
				Seen.Add(Anchor->GetUnitId());
				OwnerChain.Add(Anchor);
				if (Anchor->GetOwnerRef().IsEmpty())
				{
					return TEXT("items/") + OwnerHandle;
				}
				Anchor = Model.FindUnit(Anchor->GetOwnerRef());
				check(Anchor != nullptr);
			}

			const FString AnchorHandle =
				Aliases.FindChecked(Anchor->GetUnitId());
			FString Base;
			if (Anchor->GetUnitKind()
				== ESemanticUnitKind::TargetState)
			{
				Base = TEXT("targets/") + AnchorHandle;
			}
			else if (Anchor->GetUnitKind()
				== ESemanticUnitKind::EntityState)
			{
				Base = TEXT("entities/") + AnchorHandle;
			}
			else
			{
				const bool bGraphAnchor = IsGraphAuthoringStructureType(Anchor->GetSemanticTypeRef());
				Base = bGraphAnchor
					? TEXT("graphs/") + AnchorHandle
					: TEXT("structures/") + AnchorHandle;
			}
			if (Owner->GetUnitId() != Anchor->GetUnitId())
			{
				for (int32 Index = OwnerChain.Num() - 1; Index >= 0; --Index)
				{
					Base += TEXT("/items/")
						+ Aliases.FindChecked(OwnerChain[Index]->GetUnitId());
				}
			}
			return Base;
		}

		bool HasGraphStructureAncestor(
			const FSemanticUnit& Unit,
			const FAuthoringModel& Model)
		{
			const FSemanticUnit* Current = &Unit;
			TSet<FString> Seen;
			while (!Current->GetOwnerRef().IsEmpty())
			{
				if (Seen.Contains(Current->GetUnitId()))
				{
					return false;
				}
				Seen.Add(Current->GetUnitId());
				Current = Model.FindUnit(Current->GetOwnerRef());
				if (Current == nullptr)
				{
					return false;
				}
				if (Current->GetUnitKind() == ESemanticUnitKind::StructureState
					&& IsGraphAuthoringStructureType(Current->GetSemanticTypeRef()))
				{
					return true;
				}
			}
			return false;
		}

		bool IsGraphStructureUnit(const FSemanticUnit& Unit)
		{
			return Unit.GetUnitKind() == ESemanticUnitKind::StructureState
				&& IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef());
		}

		TMap<FString, FString> BuildGraphPhysicalPaths(
			const FAuthoringModel& Model,
			const TMap<FString, FString>& Aliases)
		{
			TMap<FString, FString> Result;
			for (const FSemanticUnit& Unit : Model.GetUnits())
			{
				if (IsGraphStructureUnit(Unit))
				{
					Result.Add(
						Unit.GetUnitId(),
						TEXT("graphs/")
							+ Aliases.FindChecked(Unit.GetUnitId())
							+ TEXT(".hjson"));
				}
			}
			return Result;
		}

		FString LogicalPath(
			const FSemanticUnit& Unit,
			const FAuthoringModel& Model,
			const TMap<FString, FString>& Aliases,
			FWorkspaceError& OutError)
		{
			const FString Handle =
				Aliases.FindChecked(Unit.GetUnitId());
			switch (Unit.GetUnitKind())
			{
			case ESemanticUnitKind::WorkspaceIndex:
				return TEXT("index.hjson");
			case ESemanticUnitKind::TargetState:
				return TEXT("targets/") + Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::EntityState:
				return TEXT("entities/") + Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::StructureState:
				return IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef())
						? TEXT("graphs/") + Handle + TEXT(".hjson")
						: TEXT("structures/") + Handle + TEXT("/structure.hjson");
			case ESemanticUnitKind::ItemState:
				return OwnerBase(Unit, Model, Aliases, OutError)
					+ TEXT("/items/")
					+ Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::PropertySet:
				if (Unit.GetRole().StartsWith(TEXT("preview/")))
					return Unit.GetRole() + TEXT("/") + SafeSegment(Unit.GetFieldSetRef()) + TEXT(".hjson");
				if (Unit.GetRole() == TEXT("authored_inputs"))
				{
					return OwnerBase(Unit, Model, Aliases, OutError)
						+ TEXT("/inputs.hjson");
				}
				return OwnerBase(Unit, Model, Aliases, OutError)
					+ (Unit.GetSemanticTypeRef() == TEXT("uai.semantic.umg.slot.property")
						? TEXT("/slot/properties/")
						: TEXT("/properties/"))
					+ SafeSegment(
						Unit.GetFieldSetRef().IsEmpty()
							? Unit.GetRole()
							: Unit.GetFieldSetRef())
					+ TEXT(".hjson");
			case ESemanticUnitKind::RelationSet:
			{
				int32 SiblingRelationCount = 0;
				for (const FSemanticUnit& Candidate : Model.GetUnits())
				{
					SiblingRelationCount += Candidate.GetUnitKind()
						== ESemanticUnitKind::RelationSet
						&& Candidate.GetOwnerRef() == Unit.GetOwnerRef();
				}
				const FString Base = OwnerBase(Unit, Model, Aliases, OutError);
				return SiblingRelationCount > 1
					? Base + TEXT("/relations/") + Handle + TEXT(".hjson")
					: Base + TEXT("/relations.hjson");
			}
			case ESemanticUnitKind::OrderingState:
				return OwnerBase(Unit, Model, Aliases, OutError)
					+ TEXT("/ordering.hjson");
			case ESemanticUnitKind::LayoutState:
				return OwnerBase(Unit, Model, Aliases, OutError)
					+ TEXT("/layout.hjson");
			case ESemanticUnitKind::CollectionState:
				if (Unit.GetRole() == TEXT("property_definition"))
				{
					return OwnerBase(Unit, Model, Aliases, OutError)
						+ TEXT("/collections/") + Handle + TEXT(".hjson");
				}
				return OwnerBase(Unit, Model, Aliases, OutError)
					+ TEXT("/collections/")
					+ Handle
					+ TEXT(".hjson");
			case ESemanticUnitKind::DependencyState:
				return OwnerBase(Unit, Model, Aliases, OutError)
					+ TEXT("/dependencies/")
					+ Handle
					+ TEXT(".hjson");
			case ESemanticUnitKind::ExternalPayloadState:
				return TEXT("external/") + Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::OpaqueExtension:
			default:
				return TEXT("extensions/") + Handle
					+ TEXT("/state.hjson");
			}
		}
	}

	FSemanticUnit::FSemanticUnit(
		FString InUnitId,
		ESemanticUnitKind InUnitKind,
		FString InOwnerRef,
		FString InSemanticTypeRef,
		FString InRole,
		FString InFieldSetRef,
		TMap<FString, FUeAgentCanonicalValueRef> InValues,
		TArray<FString> InItemRefs,
		TArray<FString> InRelationRefs,
		FString InOrderingRef,
		FString InLayoutRef,
		TArray<FString> InDependencyRefs,
		TArray<FString> InProfileVisibility,
		FString InSourceProvenance,
		FString InTargetIdentityKey,
		TArray<FString> InCategoryPath)
		: UnitId(Normalize(MoveTemp(InUnitId)))
		, UnitKind(InUnitKind)
		, OwnerRef(Normalize(MoveTemp(InOwnerRef)))
		, SemanticTypeRef(Normalize(MoveTemp(InSemanticTypeRef)))
		, Role(Normalize(MoveTemp(InRole)))
		, FieldSetRef(Normalize(MoveTemp(InFieldSetRef)))
		, Values(MoveTemp(InValues))
		, ItemRefs(MoveTemp(InItemRefs))
		, RelationRefs(MoveTemp(InRelationRefs))
		, OrderingRef(Normalize(MoveTemp(InOrderingRef)))
		, LayoutRef(Normalize(MoveTemp(InLayoutRef)))
		, DependencyRefs(MoveTemp(InDependencyRefs))
		, ProfileVisibility(MoveTemp(InProfileVisibility))
		, SourceProvenance(Normalize(MoveTemp(InSourceProvenance)))
		, TargetIdentityKey(Normalize(MoveTemp(InTargetIdentityKey)))
		, CategoryPath(MoveTemp(InCategoryPath))
	{
		for (FString& Segment : CategoryPath)
		{
			Segment = Normalize(MoveTemp(Segment));
		}
		CategoryPath.RemoveAll([](const FString& Segment)
		{
			return Segment.IsEmpty();
		});
		auto InvalidField = [&](const TCHAR* Field, const TCHAR* Reason) {
			ValidationDiagnostic = Contract::FDiagnostic::Failure(TEXT("invalid_semantic_unit"), Reason, TEXT("semantic_model"));
			ValidationDiagnostic.SemanticPointer = TEXT("/") + FString(Field);
			ValidationDiagnostic.Source.SourceUnit = UnitId;
		};
		if (UnitId.IsEmpty()) { InvalidField(TEXT("unit_id"), TEXT("The semantic unit requires a nonempty unit identity.")); return; }
		if (SemanticTypeRef.IsEmpty()) { InvalidField(TEXT("semantic_type_ref"), TEXT("The semantic unit requires a semantic type reference.")); return; }
		if (Role.IsEmpty()) { InvalidField(TEXT("role"), TEXT("The semantic unit requires a nonempty role.")); return; }
		TArray<FString> ValueKeys;
		Values.GetKeys(ValueKeys);
		ValueKeys.Sort();
		for (const FString& Key : ValueKeys) {
			if (!Normalize(Key).IsEmpty()) continue;
			InvalidField(TEXT("values"), TEXT("A semantic value field has an empty name after trimming whitespace."));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(Key);
			return;
		}
		if (!NormalizeSet(ItemRefs, ValidationDiagnostic, TEXT("invalid_semantic_unit"), TEXT("item_refs"))
			|| !NormalizeSet(RelationRefs, ValidationDiagnostic, TEXT("invalid_semantic_unit"), TEXT("relation_refs"))
			|| !NormalizeSet(DependencyRefs, ValidationDiagnostic, TEXT("invalid_semantic_unit"), TEXT("dependency_refs"))
			|| !NormalizeSet(ProfileVisibility, ValidationDiagnostic, TEXT("invalid_semantic_unit"), TEXT("profile_visibility"), true))
			ValidationDiagnostic.Source.SourceUnit = UnitId;

	}

	const FString& FSemanticUnit::GetUnitId() const { return UnitId; }
	ESemanticUnitKind FSemanticUnit::GetUnitKind() const { return UnitKind; }
	const FString& FSemanticUnit::GetOwnerRef() const { return OwnerRef; }
	const FString& FSemanticUnit::GetSemanticTypeRef() const
	{
		return SemanticTypeRef;
	}
	const FString& FSemanticUnit::GetRole() const { return Role; }
	const FString& FSemanticUnit::GetFieldSetRef() const
	{
		return FieldSetRef;
	}
	const TMap<FString, FUeAgentCanonicalValueRef>&
	FSemanticUnit::GetValues() const
	{
		return Values;
	}
	const TArray<FString>& FSemanticUnit::GetItemRefs() const
	{
		return ItemRefs;
	}
	const TArray<FString>& FSemanticUnit::GetRelationRefs() const
	{
		return RelationRefs;
	}
	const FString& FSemanticUnit::GetOrderingRef() const
	{
		return OrderingRef;
	}
	const FString& FSemanticUnit::GetLayoutRef() const { return LayoutRef; }
	const TArray<FString>& FSemanticUnit::GetDependencyRefs() const
	{
		return DependencyRefs;
	}
	const TArray<FString>& FSemanticUnit::GetProfileVisibility() const
	{
		return ProfileVisibility;
	}
	const FString& FSemanticUnit::GetSourceProvenance() const
	{
		return SourceProvenance;
	}
	const FString& FSemanticUnit::GetTargetIdentityKey() const
	{
		return TargetIdentityKey;
	}
	const TArray<FString>& FSemanticUnit::GetCategoryPath() const
	{
		return CategoryPath;
	}
	bool FSemanticUnit::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

	FAuthoringModel::FAuthoringModel(
		FString InContractId,
		int32 InContractVersion,
		FString InProfileId,
		FString InBaseRevision,
		TArray<FSemanticUnit> InUnits,
		TArray<FString> InTargetUnitIds,
		FString InSemanticFingerprint)
		: ContractId(MoveTemp(InContractId))
		, ContractVersion(InContractVersion)
		, ProfileId(MoveTemp(InProfileId))
		, BaseRevision(MoveTemp(InBaseRevision))
		, Units(MoveTemp(InUnits))
		, TargetUnitIds(MoveTemp(InTargetUnitIds))
		, SemanticFingerprint(MoveTemp(InSemanticFingerprint))
	{
		for (int32 Index = 0; Index < Units.Num(); ++Index)
		{
			UnitIndex.Add(Units[Index].GetUnitId(), Index);
		}
		// Definition aliases are local to their owning catalog. They are not type names.
		TMap<FString, TMap<FString, TArray<FString>>> DefinitionsByOwner;
		for (const FSemanticUnit& Unit : Units)
		{
			if (Unit.GetRole() != TEXT("property_definition")) continue;
			const auto* Type = Unit.GetValues().Find(TEXT("type"));
			if (Type && (*Type)->GetKind() == EUeAgentValueKind::Object)
				DefinitionsByOwner.FindOrAdd(Unit.GetOwnerRef())
					.FindOrAdd(ReadablePropertyTypeName(Type->Get())).Add(Unit.GetUnitId());
		}
		for (auto& Owner : DefinitionsByOwner)
		{
			TArray<FString> Names;
			Owner.Value.GetKeys(Names); Names.Sort();
			TSet<FString> Reserved;
			for (const FString& Name : Names) Reserved.Add(Name.ToLower());
			TSet<FString> Used;
			for (const FString& Name : Names)
			{
				auto& Definitions = Owner.Value.FindChecked(Name);
				Definitions.Sort();
				for (const FString& Definition : Definitions)
				{
					const FString Alias = AllocateReadableName(Name, [&](const FString& Candidate) {
						return Used.Contains(Candidate.ToLower())
							|| (!Candidate.Equals(Name, ESearchCase::IgnoreCase) && Reserved.Contains(Candidate.ToLower()));
					});
					Used.Add(Alias.ToLower());
					ReadablePropertyDefinitionNames.Add(Definition, Alias);
				}
			}
		}
	}

	const FString* FAuthoringModel::FindReadablePropertyDefinitionName(const FString& UnitId) const
	{
		return ReadablePropertyDefinitionNames.Find(UnitId);
	}

	const FString& FAuthoringModel::GetContractId() const
	{
		return ContractId;
	}
	int32 FAuthoringModel::GetContractVersion() const
	{
		return ContractVersion;
	}
	const FString& FAuthoringModel::GetProfileId() const
	{
		return ProfileId;
	}
	const FString& FAuthoringModel::GetBaseRevision() const
	{
		return BaseRevision;
	}
	const TArray<FSemanticUnit>& FAuthoringModel::GetUnits() const
	{
		return Units;
	}
	const TArray<FString>& FAuthoringModel::GetTargetUnitIds() const
	{
		return TargetUnitIds;
	}
	const FSemanticUnit* FAuthoringModel::FindUnit(
		const FString& UnitId) const
	{
		const int32* Index = UnitIndex.Find(UnitId);
		return Index == nullptr ? nullptr : &Units[*Index];
	}
	const FString& FAuthoringModel::GetSemanticFingerprint() const
	{
		return SemanticFingerprint;
	}

	FWorkspaceLayoutPlan::FWorkspaceLayoutPlan(
		FAuthoringModelRef InModel,
		int32 InLayoutVersion,
			EWorkspaceSourceMode InSourceMode,
			TMap<FString, FString> InLogicalPlacements,
			TMap<FString, FString> InPhysicalPlacements,
			TMap<FString, TArray<FString>> InPhysicalFiles,
		FLlmWorkspaceIndex InLlmIndex,
		FMachineWorkspaceManifest InMachineManifest,
		TMap<FString, int64> InTelemetry)
		: Model(MoveTemp(InModel))
		, LayoutVersion(InLayoutVersion)
		, SourceMode(InSourceMode)
		, LogicalPlacements(MoveTemp(InLogicalPlacements))
		, PhysicalPlacements(MoveTemp(InPhysicalPlacements))
		, PhysicalFiles(MoveTemp(InPhysicalFiles))
		, LlmIndex(MoveTemp(InLlmIndex))
		, MachineManifest(MoveTemp(InMachineManifest))
		, Telemetry(MoveTemp(InTelemetry))
	{
	}

	const FAuthoringModelRef& FWorkspaceLayoutPlan::GetModel() const
	{
		return Model;
	}
	int32 FWorkspaceLayoutPlan::GetLayoutVersion() const
	{
		return LayoutVersion;
	}
	EWorkspaceSourceMode FWorkspaceLayoutPlan::GetSourceMode() const
	{
		return SourceMode;
	}
	const FString& FWorkspaceLayoutPlan::GetSemanticFingerprint() const
	{
		return Model->GetSemanticFingerprint();
	}
	const TMap<FString, FString>&
	FWorkspaceLayoutPlan::GetLogicalPlacements() const
	{
		return LogicalPlacements;
	}
	const TMap<FString, FString>&
	FWorkspaceLayoutPlan::GetPhysicalPlacements() const
	{
		return PhysicalPlacements;
	}
	const TMap<FString, TArray<FString>>&
	FWorkspaceLayoutPlan::GetPhysicalFiles() const
	{
		return PhysicalFiles;
	}
	const FLlmWorkspaceIndex& FWorkspaceLayoutPlan::GetLlmIndex() const
	{
		return LlmIndex;
	}
	const FMachineWorkspaceManifest&
	FWorkspaceLayoutPlan::GetMachineManifest() const
	{
		return MachineManifest;
	}
	const TMap<FString, int64>&
	FWorkspaceLayoutPlan::GetTelemetry() const
	{
		return Telemetry;
	}

	FString GetSemanticWorkspaceSchemaId()
	{
		return WorkspaceSchema;
	}

	FString LexToString(ESemanticUnitKind UnitKind)
	{
		return UnitKindName(UnitKind);
	}

	bool IsOpaqueSemanticIdentity(const FString& Value)
	{
		int32 PrefixEnd = INDEX_NONE;
		if (Value.FindLastChar(TEXT(':'), PrefixEnd) && PrefixEnd > 0)
		{
			const FString Suffix = Value.Mid(PrefixEnd + 1);
			bool bStableId = Suffix.Len() == 24;
			for (TCHAR Character : Suffix) bStableId &= FChar::IsHexDigit(Character);
			if (bStableId) return true;
		}
		const FString Candidate = SafeSegment(Value);
		// Parse also accepts arbitrary 22-character base64 names as Short GUIDs,
		// including UserConstructionScript. UUID detection must use explicit forms;
		// complete digest detection below applies only to fallback identity fields.
		if (IsExplicitGuidName(Candidate))
		{
			return true;
		}
		FString Digest = Value.TrimStartAndEnd();
		Digest.RemoveFromStart(TEXT("sha256:"), ESearchCase::IgnoreCase);
		Digest.RemoveFromStart(TEXT("sha1:"), ESearchCase::IgnoreCase);
		if (Digest.Len() != 40 && Digest.Len() != 64) return false;
		for (TCHAR Character : Digest) if (!FChar::IsHexDigit(Character)) return false;
		return true;
	}

	FString MakeReadableSemanticAlias(const FString& Value)
	{
		const FString Candidate = SafeSegment(Value);
		if (IsExplicitGuidName(Candidate))
			return FString();
		return Candidate;
	}

	bool TryBuildAuthoringContentDirectories(const FAuthoringModel& Model,
		const TMap<FString, FString>& Placements, TMap<FString, FString>& OutDirectories, FWorkspaceError& OutError)
	{
		OutDirectories.Reset();
		TSet<FString> Visiting;
		TSet<FString> UsedDirectories;
		TFunction<bool(const FSemanticUnit&)> Resolve = [&](const FSemanticUnit& Unit)
		{
			if (OutDirectories.Contains(Unit.GetUnitId())) return true;
			if (Visiting.Contains(Unit.GetUnitId())) return Fail(OutError, TEXT("semantic_owner_cycle"), TEXT("The semantic unit owner references contain a dependency cycle."), Unit.GetUnitId());
			Visiting.Add(Unit.GetUnitId());
			const FSemanticUnit* Owner = Model.FindUnit(Unit.GetOwnerRef());
			FString Directory;
			if (Unit.GetUnitKind() == ESemanticUnitKind::TargetState || Unit.GetUnitKind() == ESemanticUnitKind::EntityState || !Owner)
			{
				const FString* Placement = Placements.Find(Unit.GetUnitId());
				Directory = Placement ? FPaths::GetPath(*Placement) : FString();
			}
			else
			{
				if (!Resolve(*Owner)) return false;
				Directory = OutDirectories.FindChecked(Owner->GetUnitId());
				const bool bContainer = Unit.GetUnitKind() == ESemanticUnitKind::ItemState
					|| Unit.GetUnitKind() == ESemanticUnitKind::StructureState;
				FString OwnedPath;
				if (bContainer && !Unit.GetSemanticTypeRef().StartsWith(TEXT("uai.authoring.surface.")))
				{
					const FString Emitter = ReadSemanticString(Unit, TEXT("emitter_name"));
					const FString Usage = ReadSemanticString(Unit, TEXT("usage"));
					if (Unit.GetRole() == TEXT("stack_collection") && !Usage.IsEmpty())
					{
						// Stack capture carries the emitter relation even when its semantic owner is a surface.
						if (!Emitter.IsEmpty() && !Directory.EndsWith(TEXT("/emitters/") + MakeAuthoringContentPathSegment(Emitter)))
							OwnedPath = TEXT("emitters/") + MakeAuthoringContentPathSegment(Emitter) + TEXT("/");
						OwnedPath += TEXT("stacks/") + MakeAuthoringContentPathSegment(Usage);
					}
					else if (Unit.GetSemanticTypeRef().EndsWith(TEXT(".emitter_owner")) && !Emitter.IsEmpty())
						OwnedPath = TEXT("emitters/") + MakeAuthoringContentPathSegment(Emitter);
					else
					{
						// Every owned container needs its own directory, even when no
						// display name was captured. Reuse the public alias fallback;
						// inheriting the parent's directory merges unrelated properties.
						const FString Name = SemanticAliasBase(Unit);
						OwnedPath = (IsGraphStructureUnit(Unit) ? TEXT("graphs/") : TEXT("items/"))
							+ MakeAuthoringContentPathSegment(Name);
					}
				}
				if (!OwnedPath.IsEmpty())
				{
					const FString Base = Directory / OwnedPath;
					Directory = AllocateReadableName(Base, [&](const FString& Candidate) {
						return UsedDirectories.Contains(Candidate.ToLower());
					});
					UsedDirectories.Add(Directory.ToLower());
				}
			}
			OutDirectories.Add(Unit.GetUnitId(), MoveTemp(Directory));
			Visiting.Remove(Unit.GetUnitId());
			return true;
		};
		for (const FSemanticUnit& Unit : Model.GetUnits()) if (!Resolve(Unit)) return false;
		return true;
	}

	bool TryHashSemanticUnitContent(const FSemanticUnit& Unit, FString& OutHash, FWorkspaceError& OutError)
	{
		OutHash.Reset();
		OutError.Reset();
		if (!Unit.IsValid())
		{
			OutError.SetFailure(Unit.GetValidationDiagnostic(), Unit.GetUnitId());
			return false;
		}
		OutHash = HashText(UnitMaterial(Unit));
		return true;
	}

	bool TrySerializeSemanticHjsonObject(
		const FUeAgentCanonicalValue& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit)
	{
		OutCanonicalUtf8.Reset();
		OutError.Reset();
		if (Object.GetKind() != EUeAgentValueKind::Object) {
			auto Failure = Contract::FDiagnostic::Failure(TEXT("semantic_hjson_root_not_object"),
				TEXT("A semantic HJSON document must have an object root."), TEXT("hjson_write"));
			Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
			OutError.SetFailure(MoveTemp(Failure), SourceUnit);
			return false;
		}
		FString Text;
		Contract::FDiagnostic Error;
		FUeAgentStructuredHjsonWriteOptions Options;
		Options.CanonicalLeafProjection = &TrySemanticLeafProjection;
		if (!FUeAgentStructuredHjsonFormatter::TryWriteValue(Object, Options, Text, Error)) {
			OutError.SetFailure(MoveTemp(Error), SourceUnit); return false;
		}
		Text.AppendChar(TEXT('\n'));
		const FTCHARToUTF8 Utf8(*Text);
		OutCanonicalUtf8.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return true;
	}

	bool TrySerializeSemanticHjsonObject(
		const TSharedRef<FJsonObject>& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit)
	{
		OutCanonicalUtf8.Reset();
		OutError.Reset();
		FString Text;
		Contract::FDiagnostic Error;
		const TSharedRef<FJsonValue> RootValue = MakeShared<FJsonValueObject>(Object);
		if (!FUeAgentStructuredHjsonFormatter::TryWriteValue(RootValue, Text, Error)) {
			OutError.SetFailure(MoveTemp(Error), SourceUnit); return false;
		}
		Text.AppendChar(TEXT('\n'));
		const FTCHARToUTF8 Utf8(*Text);
		OutCanonicalUtf8.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return true;
	}

	bool TrySerializeSemanticUnitRow(const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		OutError.Reset();
		if (!Unit.IsValid())
		{
			OutError.SetFailure(Unit.GetValidationDiagnostic(), SourceFile);
			return false;
		}
		const TSharedPtr<FJsonObject> Row = SemanticUnitJson(Unit, OutError);
		if (!Row.IsValid()) { OutError.SetFailure(OutError.GetDiagnostic(), SourceFile); return false; }
		return TrySerializeSemanticHjsonObject(Row.ToSharedRef(), OutCanonicalUtf8, OutError, SourceFile);
	}

	bool TryHashSemanticUnitRow(const FSemanticUnit& Unit, FString& OutHash, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutHash.Reset();
		TArray<uint8> Bytes;
		if (!TrySerializeSemanticUnitRow(Unit, Bytes, OutError, SourceFile)) return false;
		OutHash = HashBytes(Bytes.GetData(), Bytes.Num());
		return true;
	}

	bool TryBuildSemanticModelUnitRow(const FSemanticUnit& Unit,
		TSharedPtr<FJsonObject>& OutRow, FWorkspaceError& OutError, const TMap<FString, FString>* PublicRefs,
		const FString& SourceFile)
	{
		OutRow.Reset();
		OutError.Reset();
		if (!Unit.IsValid())
		{
			OutError.SetFailure(Unit.GetValidationDiagnostic(), SourceFile);
			return false;
		}
		OutRow = SemanticModelUnitJson(Unit, OutError, PublicRefs);
		if (!OutRow.IsValid()) OutError.SetFailure(OutError.GetDiagnostic(), SourceFile);
		return OutRow.IsValid();
	}

	bool TrySerializeSemanticModelUnitRow(const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		return TryBuildSemanticModelUnitRow(Unit, Row, OutError, nullptr, SourceFile)
			&& TrySerializeSemanticHjsonObject(Row.ToSharedRef(), OutCanonicalUtf8, OutError, SourceFile);
	}

	bool TrySerializeSemanticModelUnitRow(const FSemanticUnit& Unit,
		const TMap<FString, FString>& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		return TryBuildSemanticModelUnitRow(Unit, Row, OutError, &PublicRefs, SourceFile)
			&& TrySerializeSemanticHjsonObject(Row.ToSharedRef(), OutCanonicalUtf8, OutError, SourceFile);
	}

	bool TrySerializeSemanticUnitDocument(const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		if (!TryBuildSemanticModelUnitRow(Unit, Row, OutError, nullptr, SourceFile)) return false;
		const TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetObjectField(TEXT("unit"), Row);
		return TrySerializeSemanticHjsonObject(Wrapper, OutCanonicalUtf8, OutError, SourceFile);
	}

	bool TrySerializeSemanticUnitDocument(const FSemanticUnit& Unit,
		const TMap<FString, FString>& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		if (!TryBuildSemanticModelUnitRow(Unit, Row, OutError, &PublicRefs, SourceFile)) return false;
		const TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetObjectField(TEXT("unit"), Row);
		return TrySerializeSemanticHjsonObject(Wrapper, OutCanonicalUtf8, OutError, SourceFile);
	}

	bool TryCompileAuthoringModel(
		FString ContractId,
		int32 ContractVersion,
		FString ProfileId,
		FString BaseRevision,
		TArray<FSemanticUnit> Units,
		TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		OutModel.Reset();
		OutError.Reset();
		ContractId = Normalize(MoveTemp(ContractId));
		ProfileId = Normalize(MoveTemp(ProfileId));
		BaseRevision = Normalize(MoveTemp(BaseRevision));
		if (ContractId.IsEmpty()
			|| ContractVersion < 1
			|| ProfileId.IsEmpty()
			|| BaseRevision.IsEmpty()
			|| TargetUnitIds.IsEmpty())
		{
			const FString Field = ContractId.IsEmpty() ? TEXT("contract_id") : ContractVersion < 1 ? TEXT("contract_version")
				: ProfileId.IsEmpty() ? TEXT("profile_id") : BaseRevision.IsEmpty() ? TEXT("base_revision") : TEXT("target_unit_ids");
			auto Failure = Contract::FDiagnostic::Failure(TEXT("invalid_authoring_model"),
				Field == TEXT("contract_version") ? TEXT("The authoring model contract version must be positive.") : TEXT("The authoring model is missing a required field."), TEXT("semantic_model"));
			Failure.SemanticPointer = TEXT("/") + Field;
			if (Field == TEXT("contract_version")) Failure.ActualValue = MakeShared<FJsonValueNumber>(ContractVersion);
			else Failure.ActualValuePresent = false;
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		}
		Contract::FDiagnostic TargetError;
		if (!NormalizeSet(TargetUnitIds, TargetError, TEXT("invalid_target_unit_ids"), TEXT("target_unit_ids")))
		{
			OutError.SetFailure(MoveTemp(TargetError));
			return false;
		}
		Units.Sort(
			[](const FSemanticUnit& Left, const FSemanticUnit& Right)
			{
				return Left.GetUnitId() < Right.GetUnitId();
			});
		FWorkspaceError Diagnostics;
		TMap<FString, int32> UnitIndex;
		for (int32 Index = 0; Index < Units.Num(); ++Index)
		{
			const FSemanticUnit& Unit = Units[Index];
			if (!Unit.IsValid())
			{
				OutError.SetFailure(Unit.GetValidationDiagnostic(), Unit.GetUnitId());
				Diagnostics.Append(OutError);
				continue;
			}
			if (UnitIndex.Contains(Unit.GetUnitId()))
			{
				Fail(OutError, TEXT("duplicate_semantic_unit"), TEXT("More than one semantic unit has the same unit identity."), Unit.GetUnitId());
				Diagnostics.Append(OutError);
				continue;
			}
			UnitIndex.Add(Unit.GetUnitId(), Index);
		}
		if (Diagnostics.HasErrors())
		{
			Diagnostics.MarkUnchecked(TEXT("model_references"), FString(), TEXT("invalid_unit_identities"));
			OutError = MoveTemp(Diagnostics);
			return false;
		}
		for (const FString& TargetId : TargetUnitIds)
		{
			const int32* Index = UnitIndex.Find(TargetId);
			if (Index == nullptr)
			{
				Fail(OutError, TEXT("missing_target_unit"), TEXT("A requested target unit is absent from the authoring model."), TargetId);
				Diagnostics.Append(OutError);
				continue;
			}
			if (Units[*Index].GetUnitKind()
				!= ESemanticUnitKind::TargetState)
			{
				Fail(OutError, TEXT("invalid_target_unit_kind"), TEXT("A requested target unit must have the TargetState kind."), TargetId);
				Diagnostics.Append(OutError);
				continue;
			}
		}

		int64 ReferenceCount = 0;
		for (const FSemanticUnit& Unit : Units)
		{
			const TArray<FString> References = UnitReferences(Unit);
			ReferenceCount += References.Num();
			for (const FString& Reference : References)
			{
				if (!UnitIndex.Contains(Reference))
				{
					Fail(OutError, TEXT("unresolved_semantic_unit_reference"), TEXT("The semantic unit refers to a unit that is absent from the authoring model."), Unit.GetUnitId());
				Diagnostics.Append(OutError);
				continue;
				}
			}
		}

		TSet<FString> CompleteOwners;
		for (const FSemanticUnit& Start : Units)
		{
			if (CompleteOwners.Contains(Start.GetUnitId()))
			{
				continue;
			}
			TArray<FString> Path;
			TMap<FString, int32> Positions;
			const FSemanticUnit* Current = &Start;
			while (!Current->GetOwnerRef().IsEmpty())
			{
				if (CompleteOwners.Contains(Current->GetUnitId()))
				{
					break;
				}
				if (const int32* Position =
					Positions.Find(Current->GetUnitId()))
				{
					TArray<FString> Cycle;
					for (int32 Index = *Position;
						Index < Path.Num();
						++Index)
					{
						Cycle.Add(Path[Index]);
					}
					Cycle.Add(Current->GetUnitId());
					Fail(OutError, TEXT("semantic_owner_cycle"), TEXT("The semantic unit owner references contain a dependency cycle."), Start.GetUnitId(), MoveTemp(Cycle));
					Diagnostics.Append(OutError);
					break;
				}
				Positions.Add(Current->GetUnitId(), Path.Num());
				Path.Add(Current->GetUnitId());
				const int32* OwnerIndex = UnitIndex.Find(Current->GetOwnerRef());
				if (!OwnerIndex) break; // Unresolved reference already reported above.
				Current = &Units[*OwnerIndex];
			}
			CompleteOwners.Append(Path);
		}

		if (Diagnostics.HasErrors())
		{
			Diagnostics.MarkUnchecked(TEXT("domain_preflight"), FString(), TEXT("model_references_invalid"));
			OutError = MoveTemp(Diagnostics);
			return false;
		}

		FString Material;
		AppendToken(Material, TEXT("s"), ContractId);
		Material.AppendInt(ContractVersion);
		AppendToken(Material, TEXT("s"), ProfileId);
		AppendToken(Material, TEXT("s"), BaseRevision);
		AppendStringArray(Material, TargetUnitIds);
		for (const FSemanticUnit& Unit : Units)
		{
			Material.Append(UnitMaterial(Unit));
		}
		const FTCHARToUTF8 Utf8(*Material, Material.Len());
		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::InputBytes,
					Utf8.Length()},
				{EUeAgentResourceDimension::StringBytes,
					Utf8.Length()},
				{EUeAgentResourceDimension::Nodes, Units.Num()},
				{EUeAgentResourceDimension::Items,
					[&Units]()
					{
						int64 Count = 0;
						for (const FSemanticUnit& Unit : Units)
						{
							Count += Unit.GetValues().Num();
						}
						return Count;
					}()},
				{EUeAgentResourceDimension::ReferenceEdges,
					ReferenceCount},
				{EUeAgentResourceDimension::WorkUnits,
					Units.Num() + ReferenceCount},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					static_cast<int64>(Utf8.Length()) * 2},
			},
			TEXT("semantic_layout.compile"),
			ResourceError))
		{
			return FailResource(OutError, ResourceError);
		}
		OutModel = MakeShareable(new FAuthoringModel(
			MoveTemp(ContractId),
			ContractVersion,
			MoveTemp(ProfileId),
			MoveTemp(BaseRevision),
			MoveTemp(Units),
			MoveTemp(TargetUnitIds),
			HashText(Material)));
		return true;
	}

	bool TryBuildSemanticReferenceProjection(
		const FAuthoringModel& Model, FSemanticReferenceProjection& OutProjection,
		FWorkspaceError& OutError)
	{
		OutProjection = {};
		OutError.Reset();
		if (!TryBuildReadableAliases(Model, OutProjection.ReadableAliases, OutError)) return false;
		TSet<FString> UsedHandles;
		for (const FSemanticUnit& Unit : Model.GetUnits())
		{
			const FString Base = OutProjection.ReadableAliases.FindChecked(Unit.GetUnitId());
			FString Handle = Base;
			int32 Ordinal = 1;
			while (UsedHandles.Contains(Handle.ToLower()))
				Handle = Base + FString::Printf(TEXT("-%d"), ++Ordinal);
			UsedHandles.Add(Handle.ToLower());
			OutProjection.Handles.Add(Unit.GetUnitId(), MoveTemp(Handle));
		}
		for (const FSemanticUnit& Unit : Model.GetUnits())
		{
			const FString Kind = UnitKindName(Unit.GetUnitKind()).Replace(TEXT("_state"), TEXT(""));
			OutProjection.PublicRefs.Add(Unit.GetUnitId(), Kind + TEXT(":")
				+ OutProjection.Handles.FindChecked(Unit.GetUnitId()));
		}
		return true;
	}

	bool TryPlanWorkspaceLayout(
		FAuthoringModelRef Model,
		int32 LayoutVersion,
		EWorkspaceSourceMode SourceMode,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FWorkspaceLayoutPlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		OutPlan.Reset();
		OutError.Reset();
		if (LayoutVersion < 1)
		{
			auto Failure = Contract::FDiagnostic::Failure(TEXT("invalid_layout_version"), TEXT("The workspace layout version must be positive."), TEXT("workspace_layout"));
			Failure.PropertyPath = TEXT("layout_version");
			Failure.ActualValue = MakeShared<FJsonValueNumber>(LayoutVersion);
			Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("integer >= 1"));
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		}
		TMap<FString, int64> NormalizedTelemetry;
		for (const TPair<FString, int64>& Entry : Telemetry)
		{
			const FString Key = Normalize(Entry.Key);
			if (Key.IsEmpty()
				|| Entry.Value < 0
				|| NormalizedTelemetry.Contains(Key))
			{
				auto Failure = Contract::FDiagnostic::Failure(TEXT("invalid_layout_telemetry"), Key.IsEmpty()
					? TEXT("The layout telemetry field name is empty.") : Entry.Value < 0 ? TEXT("The layout telemetry count must be nonnegative.")
					: TEXT("Two layout telemetry field names are equal after normalization."), TEXT("workspace_layout"));
				Failure.CandidateName = Entry.Key;
				Failure.ActualValue = MakeShared<FJsonValueNumber>(Entry.Value);
				Failure.PropertyPath = TEXT("telemetry");
				OutError.SetFailure(MoveTemp(Failure));
				return false;
			}
			NormalizedTelemetry.Add(Key, Entry.Value);
		}
		Telemetry = MoveTemp(NormalizedTelemetry);

		FSemanticReferenceProjection References;
		if (!TryBuildSemanticReferenceProjection(Model.Get(), References, OutError)) return false;
		TMap<FString, FString> ReadableAliases = MoveTemp(References.ReadableAliases);
		TMap<FString, FString> Handles = MoveTemp(References.Handles);
		TMap<FString, FString> PublicRefs = MoveTemp(References.PublicRefs);

		TMap<FString, FString> Placements;
		TMap<FString, FString> PhysicalPlacements;
		// Public identities have a global namespace; physical navigation has owner
		// and directory scopes. Hidden state must not rename an unrelated graph or
		// asset folder merely because its readable base happens to match.
		const TMap<FString, FString> GraphPhysicalPaths =
			BuildGraphPhysicalPaths(Model.Get(), ReadableAliases);
		TMap<FString, FString> PhysicalHandles = ReadableAliases;
		for (const FSemanticUnit& Graph : Model->GetUnits())
		{
			if (!IsGraphStructureUnit(Graph)) continue;
			FGraphAuthoringAliasProjection NodeAliases;
			if (!TryBuildGraphModelNodeAliases(Model.Get(), Graph.GetUnitId(), NodeAliases, OutError)) return false;
			for (const auto& Alias : NodeAliases.AliasBySourceId)
				PhysicalHandles.Add(Alias.Key, MakeAuthoringContentPathSegment(Alias.Value));
		}
		TSet<FString> UnitPaths;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			FString Path =
				LogicalPath(Unit, Model.Get(), Handles, OutError);
			if (Path.IsEmpty())
			{
				return false;
			}
			if (UnitPaths.Contains(Path))
			{
				return Fail(OutError, TEXT("logical_path_collision"), TEXT("More than one semantic unit maps to the same workspace file path."), Path);
			}
			UnitPaths.Add(Path);
			Placements.Add(Unit.GetUnitId(), Path);
			FString PhysicalPath = LogicalPath(Unit, Model.Get(), PhysicalHandles, OutError);
			if (PhysicalPath.IsEmpty()) return false;
			const FSemanticUnit* SurfaceOwner = Unit.GetUnitKind() == ESemanticUnitKind::OrderingState
				? Model->FindUnit(Unit.GetOwnerRef()) : &Unit;
			const bool bInternalStructureSurface = SurfaceOwner
				&& SurfaceOwner->GetUnitKind() == ESemanticUnitKind::StructureState
				&& SurfaceOwner->GetSemanticTypeRef().StartsWith(TEXT("uai.authoring.surface."))
				&& SurfaceOwner->GetValues().IsEmpty();
			bool bAggregatedItem = false;
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState)
			{
				const FSemanticUnit* Parent = Model->FindUnit(Unit.GetOwnerRef());
				if (Parent && Parent->GetUnitKind() == ESemanticUnitKind::StructureState)
					for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
						if (!Contract.AggregatedItemSemanticSuffix.IsEmpty()
							&& Unit.GetSemanticTypeRef().EndsWith(Contract.AggregatedItemSemanticSuffix)
							&& Parent->GetValues().Contains(Contract.RootField))
							bAggregatedItem = true;
			}
			if (bInternalStructureSurface)
			{
				PhysicalPath = TEXT(".uai/surface-state/")
					+ Handles.FindChecked(Unit.GetUnitId()) + TEXT(".hjson");
			}
			else if (bAggregatedItem)
			{
				PhysicalPath = TEXT(".uai/keyed-state/")
					+ Handles.FindChecked(Unit.GetUnitId()) + TEXT(".hjson");
			}
			else if (const FString* GraphPath = GraphPhysicalPaths.Find(Unit.GetUnitId()))
			{
				PhysicalPath = *GraphPath;
			}
			else if (HasGraphStructureAncestor(Unit, Model.Get())
				&& (Unit.GetUnitKind() == ESemanticUnitKind::ItemState
					|| Unit.GetUnitKind() == ESemanticUnitKind::RelationSet
					|| Unit.GetUnitKind() == ESemanticUnitKind::OrderingState
					|| Unit.GetUnitKind() == ESemanticUnitKind::LayoutState))
			{
				PhysicalPath = TEXT(".uai/graph-state/")
					+ Handles.FindChecked(Unit.GetUnitId())
					+ TEXT(".hjson");
			}
			else if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& Unit.GetRole() != TEXT("authored_inputs"))
			{
				const int32 PropertiesMarker = PhysicalPath.Find(
					TEXT("/properties/"), ESearchCase::CaseSensitive,
					ESearchDir::FromEnd);
				if (PropertiesMarker != INDEX_NONE)
				{
					PhysicalPath = PhysicalPath.Left(PropertiesMarker)
						+ TEXT("/properties.hjson");
				}
			}
			else if (Unit.GetUnitKind() == ESemanticUnitKind::CollectionState
				&& Unit.GetRole() == TEXT("property_definition"))
			{
				const int32 CollectionsMarker = PhysicalPath.Find(
					TEXT("/collections/"), ESearchCase::CaseSensitive,
					ESearchDir::FromEnd);
				if (CollectionsMarker != INDEX_NONE)
				{
					PhysicalPath = PhysicalPath.Left(CollectionsMarker)
						+ TEXT("/definitions.hjson");
				}
			}
			PhysicalPlacements.Add(Unit.GetUnitId(), MoveTemp(PhysicalPath));
		}

		TMap<FString, FString> ContentDirectories;
		if (!TryBuildAuthoringContentDirectories(Model.Get(), PhysicalPlacements, ContentDirectories, OutError)) return false;
		TSet<FString> UsedPhysicalPaths;
		TSet<FString> RelocatedOwnerDocuments;
		for (const auto& Placement : PhysicalPlacements) UsedPhysicalPaths.Add(Placement.Value.ToLower());
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			if (PhysicalPlacements.FindChecked(Unit.GetUnitId()).StartsWith(TEXT(".uai/"))
				|| IsGraphStructureUnit(Unit) || HasGraphStructureAncestor(Unit, Model.Get())) continue;
			if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& Unit.GetRole().StartsWith(TEXT("preview/")))
			{
				const FString Path = Unit.GetRole() + TEXT(".hjson");
				PhysicalPlacements.Add(Unit.GetUnitId(), Path);
				UsedPhysicalPaths.Add(Path.ToLower());
				RelocatedOwnerDocuments.Add(Unit.GetUnitId());
				continue;
			}
			FString OwnerFile;
			switch (Unit.GetUnitKind())
			{
			case ESemanticUnitKind::ItemState: OwnerFile = TEXT("state.hjson"); break;
			case ESemanticUnitKind::PropertySet:
				OwnerFile = Unit.GetRole() == TEXT("authored_inputs")
					? TEXT("inputs.hjson")
					: Unit.GetSemanticTypeRef() == TEXT("uai.semantic.umg.slot.property")
						? TEXT("slot/properties.hjson")
						: TEXT("properties.hjson");
				break;
			case ESemanticUnitKind::OrderingState: OwnerFile = TEXT("ordering.hjson"); break;
			case ESemanticUnitKind::RelationSet: OwnerFile = TEXT("relations.hjson"); break;
			case ESemanticUnitKind::LayoutState: OwnerFile = TEXT("layout.hjson"); break;
			case ESemanticUnitKind::CollectionState:
				if (Unit.GetRole() == TEXT("property_definition")) OwnerFile = TEXT("definitions.hjson");
				break;
			default: break;
			}
			if (!OwnerFile.IsEmpty())
			{
				// All documents of an owner share its physical directory, including the
				// ordering and properties beside its content fragments. Logical identity is unchanged.
				const FString Path = ContentDirectories.FindChecked(Unit.GetUnitId()) / OwnerFile;
				UsedPhysicalPaths.Add(Path.ToLower());
				PhysicalPlacements.Add(Unit.GetUnitId(), Path);
				RelocatedOwnerDocuments.Add(Unit.GetUnitId());
				continue;
			}
			if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState || IsGraphStructureUnit(Unit)
				|| PhysicalPlacements.FindChecked(Unit.GetUnitId()).StartsWith(TEXT(".uai/"))) continue;
			bool bKeyedContent = Unit.GetRole() == TEXT("timeline_collection");
			for (const auto& Contract : GetKeyedItemProjectionContracts())
				bKeyedContent |= Unit.GetValues().Contains(Contract.RootField);
			if (!bKeyedContent) continue;
			FString Name = Unit.GetRole();
			Name.RemoveFromEnd(TEXT("_collection"));
			const FString Stem = ContentDirectories.FindChecked(Unit.GetUnitId()) / MakeAuthoringContentPathSegment(Name);
			FString Path = AllocateReadableName(Stem, [&](const FString& Candidate) {
				return UsedPhysicalPaths.Contains((Candidate + TEXT(".hjson")).ToLower());
			}) + TEXT(".hjson");
			UsedPhysicalPaths.Add(Path.ToLower());
			PhysicalPlacements.Add(Unit.GetUnitId(), MoveTemp(Path));
		}

		// Several logical surfaces can share a real owner. Only property/definition
		// containers support multiple units in one document; other documents remain separate.
		TMap<FString, TArray<const FSemanticUnit*>> OwnerDocuments;
		UsedPhysicalPaths.Reset();
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			const FString Path = PhysicalPlacements.FindChecked(Unit.GetUnitId()).ToLower();
			OwnerDocuments.FindOrAdd(Path).Add(&Unit);
			UsedPhysicalPaths.Add(Path);
		}
		for (const auto& Document : OwnerDocuments)
		{
			if (Document.Value.Num() < 2) continue;
			const ESemanticUnitKind Kind = Document.Value[0]->GetUnitKind();
			bool bSharedContainer = Kind == ESemanticUnitKind::PropertySet
				|| (Kind == ESemanticUnitKind::CollectionState && Document.Value[0]->GetRole() == TEXT("property_definition"));
			for (const FSemanticUnit* Unit : Document.Value)
				bSharedContainer &= Unit->GetUnitKind() == Kind
					&& (Kind != ESemanticUnitKind::CollectionState || Unit->GetRole() == TEXT("property_definition"));
			if (bSharedContainer) continue;
			for (const FSemanticUnit* Unit : Document.Value)
			{
				if (!RelocatedOwnerDocuments.Contains(Unit->GetUnitId())) continue;
				const FString Original = PhysicalPlacements.FindChecked(Unit->GetUnitId());
				const FString Name = Unit->GetFieldSetRef().IsEmpty() ? Unit->GetRole() : Unit->GetFieldSetRef();
				const FString Stem = FPaths::GetPath(Original) / (MakeAuthoringContentPathSegment(Name)
					+ TEXT("-") + FPaths::GetBaseFilename(Original));
				FString Path = AllocateReadableName(Stem, [&](const FString& Candidate) {
					return UsedPhysicalPaths.Contains((Candidate + TEXT(".hjson")).ToLower());
				}) + TEXT(".hjson");
				UsedPhysicalPaths.Add(Path.ToLower());
				PhysicalPlacements.Add(Unit->GetUnitId(), MoveTemp(Path));
			}
		}

		TMap<FString, TArray<FString>> PhysicalFiles;
		if (SourceMode == EWorkspaceSourceMode::Directory)
		{
			for (const FSemanticUnit& Unit : Model->GetUnits())
			{
				PhysicalFiles.FindOrAdd(
					PhysicalPlacements.FindChecked(Unit.GetUnitId()))
					.Add(Unit.GetUnitId());
			}
			PhysicalFiles.FindOrAdd(TEXT("index.hjson"));
			PhysicalFiles.FindOrAdd(ManifestPath);
		}
		else
		{
			TArray<FString> UnitIds;
			for (const FSemanticUnit& Unit : Model->GetUnits())
			{
				UnitIds.Add(Unit.GetUnitId());
			}
			PhysicalFiles.Add(
				SourceMode == EWorkspaceSourceMode::SingleFile
					? TEXT("workspace.hjson")
					: TEXT("inline"),
				MoveTemp(UnitIds));
		}

		FLlmWorkspaceIndex Index;
		Index.ContractId = Model->GetContractId();
		Index.ContractVersion = Model->GetContractVersion();
		Index.ProfileId = Model->GetProfileId();
		Index.TargetUnitIds = Model->GetTargetUnitIds();

		FMachineWorkspaceManifest Manifest;
		Manifest.WorkspaceSchema = WorkspaceSchema;
		Manifest.ContractId = Model->GetContractId();
		Manifest.ContractVersion = Model->GetContractVersion();
		Manifest.LayoutVersion = LayoutVersion;
		Manifest.ProfileId = Model->GetProfileId();
		Manifest.SemanticFingerprint =
			Model->GetSemanticFingerprint();
		Manifest.BaseRevision = Model->GetBaseRevision();
		Manifest.LocalHandleMapping = Handles;
		Manifest.PublicRefMapping = PublicRefs;
		Manifest.PhysicalFiles = PhysicalFiles;

		FString OutputMaterial;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			Index.UnitKindCounts.FindOrAdd(Unit.GetUnitKind())++;
			if (Unit.GetUnitKind()
					!= ESemanticUnitKind::WorkspaceIndex
				&& Unit.GetUnitKind()
					!= ESemanticUnitKind::TargetState)
			{
				Index.SurfaceUnitIds.Add(Unit.GetUnitId());
			}
			FWorkspaceUnitInventoryEntry Inventory;
			Inventory.UnitId = Unit.GetUnitId();
			Inventory.UnitKind = Unit.GetUnitKind();
			Inventory.OwnerRef = Unit.GetOwnerRef();
			Inventory.SemanticTypeRef =
				Unit.GetSemanticTypeRef();
			Inventory.Role = Unit.GetRole();
			Inventory.LocalHandle =
				Handles.FindChecked(Unit.GetUnitId());
			Inventory.LogicalPath =
				Placements.FindChecked(Unit.GetUnitId());
			if (!TryHashSemanticUnitContent(Unit, Inventory.ContentHash, OutError)) return false;
			Manifest.UnitInventory.Add(MoveTemp(Inventory));
			AppendToken(
				OutputMaterial,
				TEXT("s"),
				Placements.FindChecked(Unit.GetUnitId()));
		}
		const FTCHARToUTF8 OutputUtf8(
			*OutputMaterial,
			OutputMaterial.Len());
		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::OutputBytes,
					OutputUtf8.Length()},
				{EUeAgentResourceDimension::StringBytes,
					OutputUtf8.Length()},
				{EUeAgentResourceDimension::Files,
					PhysicalFiles.Num()},
				{EUeAgentResourceDimension::WorkUnits,
					Model->GetUnits().Num()
						+ PhysicalFiles.Num()},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					static_cast<int64>(OutputUtf8.Length()) * 2},
			},
			TEXT("semantic_layout.plan"),
			ResourceError))
		{
			return FailResource(OutError, ResourceError);
		}
		OutPlan = MakeShareable(new FWorkspaceLayoutPlan(
			MoveTemp(Model),
			LayoutVersion,
			SourceMode,
			MoveTemp(Placements),
			MoveTemp(PhysicalPlacements),
			MoveTemp(PhysicalFiles),
			MoveTemp(Index),
			MoveTemp(Manifest),
			MoveTemp(Telemetry)));
		return true;
	}

	bool IsModelVisibleSemanticWorkspacePath(const FString& RelativePath)
	{
		return !RelativePath.StartsWith(
			TEXT(".uai/"), ESearchCase::IgnoreCase);
	}
}
