#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

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

		TSharedPtr<FJsonValue> JsonFromCanonical(
			const FUeAgentCanonicalValue& Root)
		{
			struct FVisit
			{
				const FUeAgentCanonicalValue* Value = nullptr;
				bool bExpanded = false;
			};
			TArray<FVisit> Stack;
			TMap<
				const FUeAgentCanonicalValue*,
				TSharedPtr<FJsonValue>> Converted;
			Stack.Add({&Root, false});
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
						const FString& Lexeme = Visit.Value->GetLexeme();
						if (Lexeme.Contains(TEXT("."))
							|| Lexeme.Contains(TEXT("e"))
							|| Lexeme.Contains(TEXT("E")))
						{
							Converted.Add(
								Visit.Value,
								MakeShared<FJsonValueNumberString>(Lexeme));
							break;
						}
						TSharedRef<FJsonObject> TaggedDecimal =
							MakeShared<FJsonObject>();
						TaggedDecimal->SetStringField(
							TEXT("$uai"),
							TEXT("decimal"));
						TaggedDecimal->SetStringField(
							TEXT("value"),
							Lexeme);
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueObject>(TaggedDecimal));
						break;
					}
					case EUeAgentValueKind::Array:
					case EUeAgentValueKind::Object:
						Stack.Add({Visit.Value, true});
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
									{&Items[Index].Get(), false});
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
									{&Items[Index].Value.Get(), false});
							}
						}
						break;
					case EUeAgentValueKind::Temporal:
					case EUeAgentValueKind::Spatial:
					{
						TSharedPtr<FJsonValue> TypedValue;
						FString TypedError;
						if (!CanonicalJson::TryToJsonValue(
								*Visit.Value,
								TypedValue,
								TypedError)
							|| !TypedValue.IsValid()
							|| TypedValue->Type != EJson::Object)
						{
							return nullptr;
						}
						TypedValue->AsObject()->SetStringField(
							TEXT("$uai"),
							Visit.Value->GetKind()
								== EUeAgentValueKind::Temporal
									? TEXT("temporal")
									: TEXT("spatial"));
						Converted.Add(Visit.Value, TypedValue);
						break;
					}
					default:
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

		TSharedRef<FJsonObject> SemanticUnitJson(
			const FSemanticUnit& Unit)
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
				Values->SetField(
					Key,
					JsonFromCanonical(
						Unit.GetValues().FindChecked(Key).Get()));
			}
			Row->SetObjectField(TEXT("values"), Values);
			Row->SetArrayField(
				TEXT("item_refs"),
				JsonStringArray(Unit.GetItemRefs()));
			Row->SetArrayField(
				TEXT("relation_refs"),
				JsonStringArray(Unit.GetRelationRefs()));
			Row->SetStringField(
				TEXT("ordering_ref"),
				Unit.GetOrderingRef());
			Row->SetStringField(
				TEXT("layout_ref"),
				Unit.GetLayoutRef());
			Row->SetArrayField(
				TEXT("dependency_refs"),
				JsonStringArray(Unit.GetDependencyRefs()));
			Row->SetArrayField(
				TEXT("profile_visibility"),
				JsonStringArray(Unit.GetProfileVisibility()));
			Row->SetStringField(
				TEXT("source_provenance"),
				Unit.GetSourceProvenance());
			if (!Unit.GetTargetIdentityKey().IsEmpty())
			{
				Row->SetStringField(
					TEXT("target_identity_key"),
					Unit.GetTargetIdentityKey());
			}
			return Row;
		}

		bool SerializeJsonObject(
			const TSharedRef<FJsonObject>& Object,
			TArray<uint8>& OutBytes)
		{
			FString Text;
			const TSharedRef<TJsonWriter<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<
						TCHAR,
						TCondensedJsonPrintPolicy<TCHAR>>::Create(
							&Text);
			if (!FJsonSerializer::Serialize(Object, Writer))
			{
				return false;
			}
			Text.AppendChar(TEXT('\n'));
			const FTCHARToUTF8 Utf8(*Text);
			OutBytes.Reset(Utf8.Length());
			OutBytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return true;
		}

		bool NormalizeSet(TArray<FString>& Values)
		{
			for (FString& Value : Values)
			{
				Value = Normalize(MoveTemp(Value));
				if (Value.IsEmpty())
				{
					return false;
				}
			}
			Values.Sort();
			for (int32 Index = 1; Index < Values.Num(); ++Index)
			{
				if (Values[Index - 1] == Values[Index])
				{
					return false;
				}
			}
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

		FString HashText(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value, Value.Len());
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			SHA256(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length(),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (uint8 Byte : Digest)
			{
				Hex.Appendf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
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
			const FString& SourceUnit = FString(),
			TArray<FString> CyclePath = {})
		{
			OutError.Set(
				Code,
				SourceUnit,
				TEXT("/"),
				0,
				false,
				MoveTemp(CyclePath));
			return false;
		}

		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceBudgetExceeded& Error)
		{
			OutError.Set(
				Error.Code,
				Error.Path,
				TEXT("/"),
				Error.Observed,
				Error.bStoppedEarly);
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
					(Character >= TEXT('a')
						&& Character <= TEXT('z'))
					|| (Character >= TEXT('0')
						&& Character <= TEXT('9'))
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
			return Result;
		}

		FString OwnerBase(
			const FSemanticUnit& Unit,
			const FAuthoringModel& Model,
			const TMap<FString, FString>& Handles,
			FWorkspaceError& OutError)
		{
			if (Unit.GetOwnerRef().IsEmpty())
			{
				return TEXT("units/")
					+ Handles.FindChecked(Unit.GetUnitId());
			}
			const FSemanticUnit* Owner =
				Model.FindUnit(Unit.GetOwnerRef());
			check(Owner != nullptr);
			const FString OwnerHandle =
				Handles.FindChecked(Owner->GetUnitId());
			const FSemanticUnit* Anchor = Owner;
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
						TEXT("semantic_owner_cycle"),
						Unit.GetUnitId());
					return FString();
				}
				Seen.Add(Anchor->GetUnitId());
				if (Anchor->GetOwnerRef().IsEmpty())
				{
					return TEXT("items/") + OwnerHandle;
				}
				Anchor = Model.FindUnit(Anchor->GetOwnerRef());
				check(Anchor != nullptr);
			}

			const FString AnchorHandle =
				Handles.FindChecked(Anchor->GetUnitId());
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
				Base = TEXT("structures/")
					+ SafeSegment(Anchor->GetSemanticTypeRef())
					+ TEXT("/")
					+ AnchorHandle;
			}
			if (Owner->GetUnitId() != Anchor->GetUnitId())
			{
				Base += TEXT("/items/") + OwnerHandle;
			}
			return Base;
		}

		FString LogicalPath(
			const FSemanticUnit& Unit,
			const FAuthoringModel& Model,
			const TMap<FString, FString>& Handles,
			FWorkspaceError& OutError)
		{
			const FString Handle =
				Handles.FindChecked(Unit.GetUnitId());
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
				return TEXT("structures/")
					+ SafeSegment(Unit.GetSemanticTypeRef())
					+ TEXT("/")
					+ Handle
					+ TEXT("/structure.hjson");
			case ESemanticUnitKind::ItemState:
				return OwnerBase(Unit, Model, Handles, OutError)
					+ TEXT("/items/")
					+ Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::PropertySet:
				if (Unit.GetRole() == TEXT("authored_inputs"))
				{
					return OwnerBase(Unit, Model, Handles, OutError)
						+ TEXT("/inputs.hjson");
				}
				return OwnerBase(Unit, Model, Handles, OutError)
					+ TEXT("/properties/")
					+ SafeSegment(
						Unit.GetFieldSetRef().IsEmpty()
							? Unit.GetRole()
							: Unit.GetFieldSetRef())
					+ TEXT(".hjson");
			case ESemanticUnitKind::RelationSet:
				return OwnerBase(Unit, Model, Handles, OutError)
					+ TEXT("/relations.hjson");
			case ESemanticUnitKind::OrderingState:
				return OwnerBase(Unit, Model, Handles, OutError)
					+ TEXT("/ordering.hjson");
			case ESemanticUnitKind::LayoutState:
				return OwnerBase(Unit, Model, Handles, OutError)
					+ TEXT("/layout.hjson");
			case ESemanticUnitKind::CollectionState:
				return OwnerBase(Unit, Model, Handles, OutError)
					+ TEXT("/collections/")
					+ Handle
					+ TEXT(".hjson");
			case ESemanticUnitKind::DependencyState:
				return OwnerBase(Unit, Model, Handles, OutError)
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
		FString InTargetIdentityKey)
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
	{
		TArray<FString> ValueKeys;
		Values.GetKeys(ValueKeys);
		bool bValuesValid = true;
		for (const FString& Key : ValueKeys)
		{
			if (Normalize(Key).IsEmpty())
			{
				bValuesValid = false;
				break;
			}
		}
		bValid = !UnitId.IsEmpty()
			&& !SemanticTypeRef.IsEmpty()
			&& !Role.IsEmpty()
			&& bValuesValid
			&& NormalizeSet(ItemRefs)
			&& NormalizeSet(RelationRefs)
			&& NormalizeSet(DependencyRefs)
			&& NormalizeSet(ProfileVisibility);
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
	bool FSemanticUnit::IsValid() const { return bValid; }

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
		TMap<FString, TArray<FString>> InPhysicalFiles,
		FLlmWorkspaceIndex InLlmIndex,
		FMachineWorkspaceManifest InMachineManifest,
		TMap<FString, int64> InTelemetry)
		: Model(MoveTemp(InModel))
		, LayoutVersion(InLayoutVersion)
		, SourceMode(InSourceMode)
		, LogicalPlacements(MoveTemp(InLogicalPlacements))
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

	FString StableLocalHandle(
		const FString& UnitId,
		ESemanticUnitKind UnitKind)
	{
		const FString NormalizedId = Normalize(UnitId);
		FString Readable = SafeSegment(NormalizedId);
		if (Readable.IsEmpty())
		{
			Readable = SafeSegment(UnitKindName(UnitKind));
		}
		if (Readable.Len() > 48)
		{
			Readable = Readable.Right(48);
			Readable.TrimCharInline(TEXT('.'), nullptr);
			Readable.TrimCharInline(TEXT('-'), nullptr);
			Readable.TrimCharInline(TEXT('_'), nullptr);
		}
		if (Readable.IsEmpty())
		{
			Readable = TEXT("unit");
		}
		FString Identity = UnitKindName(UnitKind);
		Identity.AppendChar(TEXT('\0'));
		Identity.Append(NormalizedId);
		return Readable
			+ TEXT("-")
			+ HashText(Identity).Right(16);
	}

	FString SemanticUnitContentHash(const FSemanticUnit& Unit)
	{
		return Unit.IsValid()
			? HashText(UnitMaterial(Unit))
			: FString();
	}

	bool TrySerializeSemanticUnitRow(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8)
	{
		OutCanonicalUtf8.Reset();
		return Unit.IsValid()
			&& SerializeJsonObject(
				SemanticUnitJson(Unit),
				OutCanonicalUtf8);
	}

	bool TrySerializeSemanticUnitDocument(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8)
	{
		OutCanonicalUtf8.Reset();
		if (!Unit.IsValid())
		{
			return false;
		}
		TSharedRef<FJsonObject> Wrapper =
			MakeShared<FJsonObject>();
		Wrapper->SetObjectField(
			TEXT("unit"),
			SemanticUnitJson(Unit));
		return SerializeJsonObject(
			Wrapper,
			OutCanonicalUtf8);
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
			return Fail(OutError, TEXT("invalid_authoring_model"));
		}
		if (!NormalizeSet(TargetUnitIds))
		{
			return Fail(
				OutError,
				TEXT("invalid_target_unit_ids"));
		}
		Units.Sort(
			[](const FSemanticUnit& Left, const FSemanticUnit& Right)
			{
				return Left.GetUnitId() < Right.GetUnitId();
			});
		TMap<FString, int32> UnitIndex;
		for (int32 Index = 0; Index < Units.Num(); ++Index)
		{
			const FSemanticUnit& Unit = Units[Index];
			if (!Unit.IsValid())
			{
				return Fail(
					OutError,
					TEXT("invalid_semantic_unit"),
					Unit.GetUnitId());
			}
			if (UnitIndex.Contains(Unit.GetUnitId()))
			{
				return Fail(
					OutError,
					TEXT("duplicate_semantic_unit"),
					Unit.GetUnitId());
			}
			UnitIndex.Add(Unit.GetUnitId(), Index);
		}
		for (const FString& TargetId : TargetUnitIds)
		{
			const int32* Index = UnitIndex.Find(TargetId);
			if (Index == nullptr)
			{
				return Fail(
					OutError,
					TEXT("missing_target_unit"),
					TargetId);
			}
			if (Units[*Index].GetUnitKind()
				!= ESemanticUnitKind::TargetState)
			{
				return Fail(
					OutError,
					TEXT("invalid_target_unit_kind"),
					TargetId);
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
					return Fail(
						OutError,
						TEXT(
							"unresolved_semantic_unit_reference"),
						Unit.GetUnitId());
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
					return Fail(
						OutError,
						TEXT("semantic_owner_cycle"),
						Start.GetUnitId(),
						MoveTemp(Cycle));
				}
				Positions.Add(Current->GetUnitId(), Path.Num());
				Path.Add(Current->GetUnitId());
				Current =
					&Units[UnitIndex.FindChecked(
						Current->GetOwnerRef())];
			}
			CompleteOwners.Append(Path);
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
		FUeAgentResourceBudgetExceeded ResourceError;
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
			return Fail(OutError, TEXT("invalid_layout_version"));
		}
		TMap<FString, int64> NormalizedTelemetry;
		for (const TPair<FString, int64>& Entry : Telemetry)
		{
			const FString Key = Normalize(Entry.Key);
			if (Key.IsEmpty()
				|| Entry.Value < 0
				|| NormalizedTelemetry.Contains(Key))
			{
				return Fail(
					OutError,
					TEXT("invalid_layout_telemetry"));
			}
			NormalizedTelemetry.Add(Key, Entry.Value);
		}
		Telemetry = MoveTemp(NormalizedTelemetry);

		TMap<FString, FString> Handles;
		TSet<FString> HandleSet;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			const FString Handle =
				StableLocalHandle(
					Unit.GetUnitId(),
					Unit.GetUnitKind());
			if (HandleSet.Contains(Handle))
			{
				return Fail(
					OutError,
					TEXT("local_handle_collision"),
					Unit.GetUnitId());
			}
			HandleSet.Add(Handle);
			Handles.Add(Unit.GetUnitId(), Handle);
		}

		TMap<FString, FString> Placements;
		TSet<FString> PathSet;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			const FString Path =
				LogicalPath(Unit, Model.Get(), Handles, OutError);
			if (Path.IsEmpty())
			{
				return false;
			}
			if (PathSet.Contains(Path))
			{
				return Fail(
					OutError,
					TEXT("logical_path_collision"),
					Unit.GetUnitId());
			}
			PathSet.Add(Path);
			Placements.Add(Unit.GetUnitId(), Path);
		}

		TMap<FString, TArray<FString>> PhysicalFiles;
		if (SourceMode == EWorkspaceSourceMode::Directory)
		{
			for (const FSemanticUnit& Unit : Model->GetUnits())
			{
				PhysicalFiles.Add(
					Placements.FindChecked(Unit.GetUnitId()),
					{Unit.GetUnitId()});
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
			Inventory.ContentHash =
				SemanticUnitContentHash(Unit);
			Manifest.UnitInventory.Add(MoveTemp(Inventory));
			AppendToken(
				OutputMaterial,
				TEXT("s"),
				Placements.FindChecked(Unit.GetUnitId()));
		}
		const FTCHARToUTF8 OutputUtf8(
			*OutputMaterial,
			OutputMaterial.Len());
		FUeAgentResourceBudgetExceeded ResourceError;
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
			MoveTemp(PhysicalFiles),
			MoveTemp(Index),
			MoveTemp(Manifest),
			MoveTemp(Telemetry)));
		return true;
	}
}
