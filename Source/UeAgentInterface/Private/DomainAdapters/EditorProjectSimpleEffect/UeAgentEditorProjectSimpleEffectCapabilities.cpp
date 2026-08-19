#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::EditorProjectSimpleEffect
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		enum class EFieldValueKind : uint8
		{
			String,
			Boolean,
			Integer,
			Number,
			Array,
			Object,
		};

		struct FFieldSpec
		{
			const TCHAR* ProfileId;
			const TCHAR* FieldId;
			const TCHAR* FieldBindingId;
			const TCHAR* TypeId;
			EFieldValueKind ValueKind;
			const TCHAR* AllowedPresences;
		};

		const FFieldSpec FieldSpecs[] = {
			{
				TEXT("config_section"),
				TEXT("entries"),
				TEXT("editor_project.field.config_section.entries:v1"),
				TEXT("uai.editor_project.config_entry_map.v1"),
				EFieldValueKind::Object,
				TEXT("set|remove|replace|clear"),
			},
			{
				TEXT("project_settings"),
				TEXT("properties"),
				TEXT(
					"editor_project.field.project_settings.properties:v1"),
				TEXT(
					"uai.editor_project.settings_property_changes.v1"),
				EFieldValueKind::Array,
				TEXT("set"),
			},
			{
				TEXT("device_profile"),
				TEXT("device_type"),
				TEXT(
					"editor_project.field.device_profile.device_type:v1"),
				TEXT("uai.value.string.v1"),
				EFieldValueKind::String,
				TEXT("set|clear"),
			},
			{
				TEXT("device_profile"),
				TEXT("base_profile_ref"),
				TEXT(
					"editor_project.field.device_profile.base_profile_ref:v1"),
				TEXT("uai.value.canonical_ref.v1"),
				EFieldValueKind::String,
				TEXT("set|null|clear"),
			},
			{
				TEXT("device_profile"),
				TEXT("cvars"),
				TEXT("editor_project.field.device_profile.cvars:v1"),
				TEXT("uai.editor_project.config_entry_map.v1"),
				EFieldValueKind::Object,
				TEXT("set|remove|replace|clear"),
			},
			{
				TEXT("localization_target"),
				TEXT("native_culture"),
				TEXT(
					"editor_project.field.localization_target.native_culture:v1"),
				TEXT("uai.value.culture_name.v1"),
				EFieldValueKind::String,
				TEXT("set"),
			},
			{
				TEXT("localization_target"),
				TEXT("cultures"),
				TEXT(
					"editor_project.field.localization_target.cultures:v1"),
				TEXT("uai.value.culture_name_set.v1"),
				EFieldValueKind::Array,
				TEXT("set|remove|replace|clear"),
			},
			{
				TEXT("platform_config"),
				TEXT("entries"),
				TEXT("editor_project.field.platform_config.entries:v1"),
				TEXT("uai.editor_project.config_entry_map.v1"),
				EFieldValueKind::Object,
				TEXT("set|remove|replace|clear"),
			},
			{
				TEXT("scalability_group"),
				TEXT("entries"),
				TEXT(
					"editor_project.field.scalability_group.entries:v1"),
				TEXT("uai.editor_project.config_entry_map.v1"),
				EFieldValueKind::Object,
				TEXT("set|remove|replace|clear"),
			},
			{
				TEXT("string_table"),
				TEXT("namespace"),
				TEXT("editor_project.field.string_table.namespace:v1"),
				TEXT("uai.value.string.v1"),
				EFieldValueKind::String,
				TEXT("set|clear"),
			},
			{
				TEXT("string_table"),
				TEXT("entries"),
				TEXT("editor_project.field.string_table.entries:v1"),
				TEXT("uai.editor_project.string_table_entries.v1"),
				EFieldValueKind::Array,
				TEXT("set|remove|replace|clear"),
			},
		};

		struct FDescriptorSpec
		{
			const TCHAR* DescriptorId;
			const TCHAR* CapabilityId;
			const TCHAR* ProfileId;
			const TCHAR* NativeOperationType;
			const TCHAR* NativeMechanicId;
			const TCHAR* FieldIds;
		};

		const FDescriptorSpec DescriptorSpecs[] = {
			{
				TEXT(
					"uai.operation.editor_project.config_section.update"),
				TEXT("editor_project.config_section.update"),
				TEXT("config_section"),
				TEXT("editor_project.typed_update"),
				TEXT(
					"uai.editor_project.mechanic:update_config_section:v1"),
				TEXT("entries"),
			},
			{
				TEXT(
					"uai.operation.editor_project.project_settings.update"),
				TEXT("editor_project.project_settings.update"),
				TEXT("project_settings"),
				TEXT("editor_project.typed_update"),
				TEXT(
					"uai.editor_project.mechanic:update_project_settings:v1"),
				TEXT("properties"),
			},
			{
				TEXT(
					"uai.operation.editor_project.device_profile.update"),
				TEXT("editor_project.device_profile.update"),
				TEXT("device_profile"),
				TEXT("editor_project.typed_update"),
				TEXT(
					"uai.editor_project.mechanic:update_device_profile:v1"),
				TEXT("device_type|base_profile_ref|cvars"),
			},
			{
				TEXT(
					"uai.operation.editor_project.localization_target.update"),
				TEXT("editor_project.localization_target.update"),
				TEXT("localization_target"),
				TEXT("editor_project.typed_update"),
				TEXT(
					"uai.editor_project.mechanic:update_localization_target:v1"),
				TEXT("native_culture|cultures"),
			},
			{
				TEXT(
					"uai.operation.editor_project.platform_config.update"),
				TEXT("editor_project.platform_config.update"),
				TEXT("platform_config"),
				TEXT("editor_project.typed_update"),
				TEXT(
					"uai.editor_project.mechanic:update_platform_config:v1"),
				TEXT("entries"),
			},
			{
				TEXT(
					"uai.operation.editor_project.scalability_group.update"),
				TEXT("editor_project.scalability_group.update"),
				TEXT("scalability_group"),
				TEXT("editor_project.typed_update"),
				TEXT(
					"uai.editor_project.mechanic:update_scalability_group:v1"),
				TEXT("entries"),
			},
			{
				TEXT(
					"uai.operation.editor_project.string_table.update"),
				TEXT("editor_project.string_table.update"),
				TEXT("string_table"),
				TEXT("editor_project.typed_update"),
				TEXT(
					"uai.editor_project.mechanic:update_string_table:v1"),
				TEXT("namespace|entries"),
			},
		};

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		TArray<FString> SplitValues(const TCHAR* Source)
		{
			TArray<FString> Values;
			FString(Source).ParseIntoArray(
				Values,
				TEXT("|"),
				true);
			return Values;
		}

		FString MakeNativeBindingId(const FDescriptorSpec& Spec)
		{
			return TEXT("uai.editor_project.binding:")
				+ FString(Spec.CapabilityId)
				+ TEXT(":v1");
		}

		const FFieldSpec* FindFieldSpec(
			const FString& ProfileId,
			const FString& FieldId)
		{
			for (const FFieldSpec& Field : FieldSpecs)
			{
				if (ProfileId == Field.ProfileId
					&& FieldId == Field.FieldId)
				{
					return &Field;
				}
			}
			return nullptr;
		}

		const FDescriptorSpec* FindSpecByBinding(
			const FString& NativeBindingId)
		{
			for (const FDescriptorSpec& Spec : DescriptorSpecs)
			{
				if (NativeBindingId == MakeNativeBindingId(Spec))
				{
					return &Spec;
				}
			}
			return nullptr;
		}

		TSharedPtr<FJsonObject> StringSchema()
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			Schema->SetNumberField(TEXT("minLength"), 1);
			return Schema;
		}

		TSharedPtr<FJsonObject> ConstStringSchema(
			const FString& Value)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("const"), Value);
			return Schema;
		}

		TSharedPtr<FJsonObject> EnumSchema(
			const TArray<FString>& Values)
		{
			const TSharedPtr<FJsonObject> Schema = StringSchema();
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			for (const FString& Value : Values)
			{
				EnumValues.Add(
					MakeShared<FJsonValueString>(Value));
			}
			Schema->SetArrayField(
				TEXT("enum"),
				MoveTemp(EnumValues));
			return Schema;
		}

		TSharedPtr<FJsonObject> ObjectSchema(
			const TSharedPtr<FJsonObject>& Properties,
			const TArray<FString>& Required)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), Properties);
			Schema->SetBoolField(
				TEXT("additionalProperties"),
				false);
			TArray<TSharedPtr<FJsonValue>> RequiredValues;
			for (const FString& Name : Required)
			{
				RequiredValues.Add(
					MakeShared<FJsonValueString>(Name));
			}
			Schema->SetArrayField(
				TEXT("required"),
				MoveTemp(RequiredValues));
			return Schema;
		}

		TSharedPtr<FJsonObject> ValueKindSchema(
			const EFieldValueKind Kind)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			switch (Kind)
			{
			case EFieldValueKind::String:
				Schema->SetStringField(TEXT("type"), TEXT("string"));
				Schema->SetNumberField(TEXT("minLength"), 1);
				break;
			case EFieldValueKind::Boolean:
				Schema->SetStringField(TEXT("type"), TEXT("boolean"));
				break;
			case EFieldValueKind::Integer:
				Schema->SetStringField(TEXT("type"), TEXT("integer"));
				break;
			case EFieldValueKind::Number:
				Schema->SetStringField(TEXT("type"), TEXT("number"));
				break;
			case EFieldValueKind::Array:
				Schema->SetStringField(TEXT("type"), TEXT("array"));
				break;
			case EFieldValueKind::Object:
				Schema->SetStringField(TEXT("type"), TEXT("object"));
				break;
			}
			return Schema;
		}

		bool PresenceRequiresValue(const FString& Presence)
		{
			return Presence == TEXT("set")
				|| Presence == TEXT("remove")
				|| Presence == TEXT("replace");
		}

		TSharedPtr<FJsonObject> BuildChangeUnitSchema(
			const FFieldSpec& Field)
		{
			TArray<FString> ValuePresences;
			TArray<FString> NoValuePresences;
			for (const FString& Presence
				: SplitValues(Field.AllowedPresences))
			{
				(PresenceRequiresValue(Presence)
						? ValuePresences
						: NoValuePresences)
					.Add(Presence);
			}

			TArray<TSharedPtr<FJsonValue>> Alternatives;
			if (!ValuePresences.IsEmpty())
			{
				const TSharedPtr<FJsonObject> ValueProperties =
					MakeShared<FJsonObject>();
				ValueProperties->SetObjectField(
					TEXT("type_id"),
					ConstStringSchema(Field.TypeId));
				ValueProperties->SetObjectField(
					TEXT("canonical_value"),
					ValueKindSchema(Field.ValueKind));

				const TSharedPtr<FJsonObject> UnitProperties =
					MakeShared<FJsonObject>();
				UnitProperties->SetObjectField(
					TEXT("field_id"),
					ConstStringSchema(Field.FieldId));
				UnitProperties->SetObjectField(
					TEXT("presence"),
					EnumSchema(ValuePresences));
				UnitProperties->SetObjectField(
					TEXT("value"),
					ObjectSchema(
						ValueProperties,
						{
							TEXT("type_id"),
							TEXT("canonical_value"),
						}));
				Alternatives.Add(
					MakeShared<FJsonValueObject>(
						ObjectSchema(
							UnitProperties,
							{
								TEXT("field_id"),
								TEXT("presence"),
								TEXT("value"),
							})));
			}
			if (!NoValuePresences.IsEmpty())
			{
				const TSharedPtr<FJsonObject> UnitProperties =
					MakeShared<FJsonObject>();
				UnitProperties->SetObjectField(
					TEXT("field_id"),
					ConstStringSchema(Field.FieldId));
				UnitProperties->SetObjectField(
					TEXT("presence"),
					EnumSchema(NoValuePresences));
				Alternatives.Add(
					MakeShared<FJsonValueObject>(
						ObjectSchema(
							UnitProperties,
							{TEXT("field_id"), TEXT("presence")})));
			}
			if (Alternatives.Num() == 1)
			{
				return Alternatives[0]->AsObject();
			}
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetArrayField(
				TEXT("oneOf"),
				MoveTemp(Alternatives));
			return Schema;
		}

		TSharedPtr<FJsonObject> BuildInputSchema(
			const FDescriptorSpec& Spec)
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("target_ref"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("expected_revision"),
				StringSchema());

			const TSharedPtr<FJsonObject> Changes =
				MakeShared<FJsonObject>();
			Changes->SetStringField(TEXT("type"), TEXT("array"));
			TArray<TSharedPtr<FJsonValue>> Alternatives;
			for (const FString& FieldId
				: SplitValues(Spec.FieldIds))
			{
				if (const FFieldSpec* Field =
					FindFieldSpec(Spec.ProfileId, FieldId))
				{
					Alternatives.Add(
						MakeShared<FJsonValueObject>(
							BuildChangeUnitSchema(*Field)));
				}
			}
			const TSharedPtr<FJsonObject> ItemSchema =
				MakeShared<FJsonObject>();
			ItemSchema->SetArrayField(
				TEXT("oneOf"),
				MoveTemp(Alternatives));
			Changes->SetObjectField(TEXT("items"), ItemSchema);
			Changes->SetNumberField(TEXT("minItems"), 1);
			Changes->SetBoolField(TEXT("uniqueItems"), true);
			Properties->SetObjectField(TEXT("changes"), Changes);

			return ObjectSchema(
				Properties,
				{
					TEXT("target_ref"),
					TEXT("expected_revision"),
					TEXT("changes"),
				});
		}

		TSharedPtr<FJsonObject> BuildResultSchema()
		{
			const TSharedPtr<FJsonObject> StringArraySchema =
				MakeShared<FJsonObject>();
			StringArraySchema->SetStringField(
				TEXT("type"),
				TEXT("array"));
			StringArraySchema->SetObjectField(
				TEXT("items"),
				StringSchema());
			StringArraySchema->SetBoolField(
				TEXT("uniqueItems"),
				true);

			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("operation_id"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("outcome"),
				EnumSchema({
					TEXT("complete"),
					TEXT("partial"),
					TEXT("rolled_back"),
					TEXT("state_unknown"),
				}));
			Properties->SetObjectField(
				TEXT("target_ref"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("target_revision"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("evidence_refs"),
				StringArraySchema);
			return ObjectSchema(
				Properties,
				{
					TEXT("operation_id"),
					TEXT("outcome"),
					TEXT("target_ref"),
					TEXT("target_revision"),
				});
		}

		bool TryCanonicalize(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutCanonical,
			FString& OutError)
		{
			return Object.IsValid()
				&& Json::TryCanonicalize(
					Object,
					OutCanonical,
					OutError);
		}

		bool TryBuildDescriptorHash(
			const FDescriptorSpec& Spec,
			const TSharedPtr<FJsonObject>& InputSchema,
			const TSharedPtr<FJsonObject>& ResultSchema,
			FString& OutHash,
			FString& OutError)
		{
			const TSharedPtr<FJsonObject> Policy =
				MakeShared<FJsonObject>();
			Policy->SetArrayField(
				TEXT("allowed_state_planes"),
				{
					MakeShared<FJsonValueString>(
						TEXT("authoring_memory")),
				});
			Policy->SetArrayField(TEXT("required_facets"), {});
			Policy->SetStringField(
				TEXT("resolution_policy"),
				TEXT("explicit_only"));
			Policy->SetStringField(
				TEXT("transient_access_policy"),
				TEXT("temporary_mutation_allowed"));

			const TSharedPtr<FJsonObject> Descriptor =
				MakeShared<FJsonObject>();
			Descriptor->SetStringField(
				TEXT("capability_id"),
				Spec.CapabilityId);
			Descriptor->SetStringField(
				TEXT("descriptor_id"),
				Spec.DescriptorId);
			Descriptor->SetStringField(
				TEXT("domain_id"),
				TEXT("editor_project"));
			Descriptor->SetObjectField(
				TEXT("execution_context_policy"),
				Policy);
			Descriptor->SetObjectField(
				TEXT("input_schema"),
				InputSchema);
			Descriptor->SetStringField(
				TEXT("operation_kind"),
				TEXT("operation"));
			Descriptor->SetObjectField(
				TEXT("result_schema"),
				ResultSchema);
			Descriptor->SetNumberField(TEXT("version"), 1);

			FString Canonical;
			if (!TryCanonicalize(
					Descriptor,
					Canonical,
					OutError))
			{
				return false;
			}
			OutHash = Json::Sha256Utf8(Canonical);
			return !OutHash.IsEmpty();
		}

		const FDescriptorSpec* FindSpec(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FDescriptorSpec& Spec : DescriptorSpecs)
			{
				if (Identity.GetDescriptorId() != Spec.DescriptorId
					|| Identity.GetVersion() != 1)
				{
					continue;
				}
				FString Hash;
				FString Error;
				if (TryBuildDescriptorHash(
						Spec,
						BuildInputSchema(Spec),
						BuildResultSchema(),
						Hash,
						Error)
					&& Identity.GetDescriptorHash() == Hash)
				{
					return &Spec;
				}
			}
			return nullptr;
		}

		bool TryParsePayload(
			const FUeAgentOperationRequest& Request,
			TSharedPtr<FJsonObject>& OutPayload,
			FExecutionMutationError& OutError)
		{
			OutPayload.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(
					Request.GetPayload().GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutPayload)
				|| !OutPayload.IsValid())
			{
				return Fail(
					OutError,
					TEXT("editor_project_payload_parse_failed"));
			}
			return true;
		}

		const FUeAgentStateEntry* FindEntry(
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			const FString& CanonicalRef)
		{
			for (const FUeAgentStateEntry& Entry
				: Snapshot->GetEntries())
			{
				if (Entry.GetIdentity().GetSelector()
					== CanonicalRef)
				{
					return &Entry;
				}
			}
			return nullptr;
		}

		bool TryGetObjectString(
			const FUeAgentCanonicalValueRef& Value,
			const FString& Field,
			FString& OutText)
		{
			if (Value->GetKind() != EUeAgentValueKind::Object)
			{
				return false;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Value->GetObjectItems())
			{
				if (Item.Key == Field
					&& Item.Value->GetKind()
						== EUeAgentValueKind::String)
				{
					OutText = Item.Value->GetText();
					return true;
				}
			}
			return false;
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			FUeAgentStateSemanticError Error;
			if (!JsonValue.IsValid())
			{
				return Fail(
					OutError,
					TEXT("editor_project_canonical_value_missing"));
			}
			switch (JsonValue->Type)
			{
			case EJson::Null:
				if (FUeAgentCanonicalValue::TryNull(
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Boolean:
				if (FUeAgentCanonicalValue::TryBoolean(
						JsonValue->AsBool(),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::String:
				if (FUeAgentCanonicalValue::TryString(
						JsonValue->AsString(),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Number:
				{
					const double Number = JsonValue->AsNumber();
					const bool bInteger =
						FMath::IsNearlyEqual(
							Number,
							FMath::FloorToDouble(Number))
						&& Number
							>= static_cast<double>(MIN_int64)
						&& Number
							<= static_cast<double>(MAX_int64);
					const bool bBuilt = bInteger
						? FUeAgentCanonicalValue::TryInteger(
							FString::Printf(
								TEXT("%lld"),
								static_cast<long long>(
									Number)),
							Resources,
							OutValue,
							Error)
						: FUeAgentCanonicalValue::TryDecimal(
							FString::SanitizeFloat(Number),
							Resources,
							OutValue,
							Error);
					if (bBuilt)
					{
						return true;
					}
					break;
				}
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: JsonValue->AsArray())
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Item,
								Resources,
								CanonicalItem,
								OutError))
						{
							return false;
						}
						Items.Add(CanonicalItem.ToSharedRef());
					}
					if (FUeAgentCanonicalValue::TryArray(
							Items,
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			case EJson::Object:
				{
					TArray<
						TPair<
							FString,
							FUeAgentCanonicalValueRef>> Items;
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Pair
							: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Pair.Value,
								Resources,
								CanonicalItem,
								OutError))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							CanonicalItem.ToSharedRef());
					}
					if (FUeAgentCanonicalValue::TryObject(
							Items,
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			default:
				break;
			}
			return Fail(
				OutError,
				Error.Code.IsEmpty()
					? TEXT(
						"editor_project_canonical_value_failed")
					: Error.Code,
				Error.Detail);
		}

		bool JsonValueMatchesKind(
			const TSharedPtr<FJsonValue>& Value,
			const EFieldValueKind Kind)
		{
			if (!Value.IsValid())
			{
				return false;
			}
			switch (Kind)
			{
			case EFieldValueKind::String:
				return Value->Type == EJson::String;
			case EFieldValueKind::Boolean:
				return Value->Type == EJson::Boolean;
			case EFieldValueKind::Integer:
				return Value->Type == EJson::Number
					&& FMath::IsNearlyEqual(
						Value->AsNumber(),
						FMath::FloorToDouble(Value->AsNumber()));
			case EFieldValueKind::Number:
				return Value->Type == EJson::Number;
			case EFieldValueKind::Array:
				return Value->Type == EJson::Array;
			case EFieldValueKind::Object:
				return Value->Type == EJson::Object;
			}
			return false;
		}

		bool IsSafeText(const FString& Text)
		{
			FString Trimmed = Text;
			Trimmed.TrimStartAndEndInline();
			return !Trimmed.IsEmpty()
				&& Trimmed == Text
				&& !Text.Contains(TEXT("\r"))
				&& !Text.Contains(TEXT("\n"));
		}

		bool ValidateConfigEntryMap(
			const TSharedPtr<FJsonValue>& Value,
			FExecutionMutationError& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				return Fail(
					OutError,
					TEXT("editor_project_config_entry_invalid"));
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: Value->AsObject()->Values)
			{
				if (!IsSafeText(Pair.Key)
					|| !Pair.Value.IsValid()
					|| Pair.Value->Type != EJson::String
					|| Pair.Value->AsString().Contains(TEXT("\r"))
					|| Pair.Value->AsString().Contains(TEXT("\n")))
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_config_entry_invalid"),
						Pair.Key);
				}
			}
			return true;
		}

		bool ValidateSettingsChanges(
			const TSharedPtr<FJsonValue>& Value,
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			FExecutionMutationError& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_settings_changes_invalid"));
			}
			TSet<FString> SeenFields;
			for (const TSharedPtr<FJsonValue>& Item
				: Value->AsArray())
			{
				const TSharedPtr<FJsonObject> Change =
					Item.IsValid() ? Item->AsObject() : nullptr;
				FString FieldRef;
				FString ExpectedRevision;
				FString Presence;
				FString TypeId;
				if (!Change.IsValid()
					|| !Change->TryGetStringField(
						TEXT("field_ref"),
						FieldRef)
					|| !Change->TryGetStringField(
						TEXT("expected_revision"),
						ExpectedRevision)
					|| !Change->TryGetStringField(
						TEXT("presence"),
						Presence)
					|| !Change->TryGetStringField(
						TEXT("type_id"),
						TypeId)
					|| !IsSafeText(FieldRef)
					|| !IsSafeText(ExpectedRevision)
					|| !IsSafeText(TypeId)
					|| !TSet<FString>({
						TEXT("set"),
						TEXT("null"),
						TEXT("clear"),
					}).Contains(Presence))
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_change_invalid"),
						FieldRef);
				}
				if (SeenFields.Contains(FieldRef))
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_field_duplicate"),
						FieldRef);
				}
				SeenFields.Add(FieldRef);

				const FUeAgentStateEntry* FieldEntry =
					FindEntry(Snapshot, FieldRef);
				if (!FieldEntry)
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_field_unresolved"),
						FieldRef);
				}
				FString CurrentRevision;
				FString CurrentType;
				if (!TryGetObjectString(
						FieldEntry->GetValue(),
						TEXT("target_revision"),
						CurrentRevision)
					|| CurrentRevision != ExpectedRevision)
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_field_revision_stale"),
						FieldRef);
				}
				if (!TryGetObjectString(
						FieldEntry->GetValue(),
						TEXT("type_id"),
						CurrentType)
					|| CurrentType != TypeId)
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_field_type_mismatch"),
						FieldRef);
				}
				const TSharedPtr<FJsonValue> PropertyValue =
					Change->TryGetField(TEXT("value"));
				if ((Presence == TEXT("set"))
					!= PropertyValue.IsValid())
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_value_presence_mismatch"),
						FieldRef);
				}
				if (Presence == TEXT("set")
					&& ((TypeId == TEXT("uai.value.boolean.v1")
							&& PropertyValue->Type
								!= EJson::Boolean)
						|| (TypeId == TEXT("uai.value.string.v1")
							&& PropertyValue->Type
								!= EJson::String)
						|| (TypeId == TEXT("uai.value.integer.v1")
							&& PropertyValue->Type
								!= EJson::Number)))
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_value_type_mismatch"),
						FieldRef);
				}
			}
			return true;
		}

		bool ValidateCultureName(const FString& Culture)
		{
			return IsSafeText(Culture)
				&& !Culture.Contains(TEXT(" "))
				&& !Culture.Contains(TEXT("/"))
				&& !Culture.Contains(TEXT("\\"));
		}

		bool ValidateCultures(
			const TSharedPtr<FJsonValue>& Value,
			FExecutionMutationError& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Array)
			{
				return Fail(
					OutError,
					TEXT("editor_project_culture_set_invalid"));
			}
			TSet<FString> Cultures;
			for (const TSharedPtr<FJsonValue>& Item
				: Value->AsArray())
			{
				if (!Item.IsValid()
					|| Item->Type != EJson::String
					|| !ValidateCultureName(Item->AsString())
					|| Cultures.Contains(Item->AsString()))
				{
					return Fail(
						OutError,
						TEXT("editor_project_culture_set_invalid"));
				}
				Cultures.Add(Item->AsString());
			}
			return true;
		}

		bool ValidateStringTableEntries(
			const TSharedPtr<FJsonValue>& Value,
			FExecutionMutationError& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_string_table_entries_invalid"));
			}
			TSet<FString> Keys;
			for (const TSharedPtr<FJsonValue>& Item
				: Value->AsArray())
			{
				const TSharedPtr<FJsonObject> Row =
					Item.IsValid() ? Item->AsObject() : nullptr;
				FString Key;
				FString Source;
				const TSharedPtr<FJsonObject>* Metadata = nullptr;
				if (!Row.IsValid()
					|| !Row->TryGetStringField(TEXT("key"), Key)
					|| !Row->TryGetStringField(
						TEXT("source"),
						Source)
					|| !IsSafeText(Key)
					|| Keys.Contains(Key)
					|| (Row->TryGetObjectField(
							TEXT("metadata"),
							Metadata)
						&& (!Metadata
							|| !Metadata->IsValid())))
				{
					return Fail(
						OutError,
						Keys.Contains(Key)
							? TEXT(
								"editor_project_string_table_key_duplicate")
							: TEXT(
								"editor_project_string_table_entry_invalid"),
						Key);
				}
				Keys.Add(Key);
				if (Metadata && Metadata->IsValid())
				{
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Pair
							: (*Metadata)->Values)
					{
						if (!IsSafeText(Pair.Key)
							|| !Pair.Value.IsValid()
							|| Pair.Value->Type
								!= EJson::String)
						{
							return Fail(
								OutError,
								TEXT(
									"editor_project_string_table_metadata_invalid"),
								Key);
						}
					}
				}
			}
			return true;
		}
	}

	TArray<FString> GetEditorProjectSimpleEffectAuthorityScopes(
		const FString& ProfileId)
	{
		TArray<FString> AuthorityScopes{
			TEXT("uai:editor-project:write"),
		};
		AuthorityScopes.Add(
			ProfileId == TEXT("string_table")
				? TEXT("uai:asset:write")
				: TEXT("uai:project-config:write"));
		return AuthorityScopes;
	}

	bool TryCreateEditorProjectSimpleEffectDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		const TSharedPtr<FJsonObject> ResultSchema =
			BuildResultSchema();
		FString CanonicalResult;
		if (!TryCanonicalize(
				ResultSchema,
				CanonicalResult,
				OutError))
		{
			return false;
		}

		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(UE_ARRAY_COUNT(DescriptorSpecs));
		for (const FDescriptorSpec& Spec : DescriptorSpecs)
		{
			const TSharedPtr<FJsonObject> InputSchema =
				BuildInputSchema(Spec);
			FString CanonicalInput;
			FString DescriptorHash;
			if (!TryCanonicalize(
					InputSchema,
					CanonicalInput,
					OutError)
				|| !TryBuildDescriptorHash(
					Spec,
					InputSchema,
					ResultSchema,
					DescriptorHash,
					OutError))
			{
				OutRegistry.Reset();
				return false;
			}
			Descriptors.Emplace(
				FUeAgentDescriptorIdentity(
					Spec.DescriptorId,
					1,
					MoveTemp(DescriptorHash)),
				Spec.CapabilityId,
				TEXT("editor_project"),
				EUeAgentDescriptorOperationKind::Operation,
				FUeAgentExecutionContextPolicy(
					{EUeAgentStatePlane::AuthoringMemory},
					{},
					EUeAgentContextResolutionPolicy::ExplicitOnly,
					EUeAgentTransientContextPolicy::
						TemporaryMutationAllowed),
				FUeAgentContractValue(MoveTemp(CanonicalInput)),
				FUeAgentContractValue(CanonicalResult));
		}
		if (Descriptors.Num() != 7)
		{
			OutError =
				TEXT("editor_project_descriptor_count_invalid");
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	bool TryGetEditorProjectSimpleEffectProductionProfiles(
		TArray<FUeAgentEditorProjectSimpleEffectProductionProfile>&
			OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		if (!TryCreateEditorProjectSimpleEffectDescriptorRegistry(
				Registry,
				OutError)
			|| !Registry.IsValid())
		{
			return false;
		}

		OutProfiles.Reserve(UE_ARRAY_COUNT(DescriptorSpecs));
		for (const FDescriptorSpec& Spec : DescriptorSpecs)
		{
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->GetDescriptors().FindByPredicate(
					[&Spec](
						const FUeAgentOperationDescriptor& Candidate)
					{
						return Candidate.GetIdentity().GetDescriptorId()
							== Spec.DescriptorId;
					});
			if (!Descriptor)
			{
				OutError =
					TEXT("editor_project_production_descriptor_missing:")
					+ FString(Spec.DescriptorId);
				OutProfiles.Reset();
				return false;
			}
			FUeAgentEditorProjectSimpleEffectProductionProfile Profile;
			Profile.DescriptorId = Spec.DescriptorId;
			Profile.DescriptorVersion =
				Descriptor->GetIdentity().GetVersion();
			Profile.DescriptorHash =
				Descriptor->GetIdentity().GetDescriptorHash();
			Profile.CapabilityId = Spec.CapabilityId;
			Profile.ProfileId = Spec.ProfileId;
			Profile.NativeOperationType = Spec.NativeOperationType;
			Profile.NativeBindingId = MakeNativeBindingId(Spec);
			Profile.NativeMechanicId = Spec.NativeMechanicId;
			Profile.FieldIds = SplitValues(Spec.FieldIds);
			if (!Profile.IsValid())
			{
				OutError =
					TEXT("editor_project_production_profile_invalid:")
					+ FString(Spec.DescriptorId);
				OutProfiles.Reset();
				return false;
			}
			OutProfiles.Add(MoveTemp(Profile));
		}
		if (OutProfiles.Num() != 7)
		{
			OutError =
				TEXT("editor_project_production_profile_count_mismatch");
			OutProfiles.Reset();
			return false;
		}
		return true;
	}

	bool TryBuildEditorProjectSimpleEffectChangeSetDraft(
		const FUeAgentOperationRequest& Request,
		const FUeAgentExecutionContextRef& Context,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FExecutionMutationError& OutError)
	{
		OutDraft.Reset();
		OutError = FExecutionMutationError();
		if (!Request.IsValid()
			|| Context.GetProjectInstanceId().IsEmpty()
			|| Context.GetEditorInstanceId().IsEmpty()
			|| Context.GetContextRevision().IsEmpty()
			|| CurrentSnapshot->GetStatePlane()
				!= EUeAgentStatePlane::AuthoringMemory)
		{
			return Fail(
				OutError,
				TEXT("editor_project_draft_request_invalid"));
		}

		const FDescriptorSpec* Spec =
			FindSpec(Request.GetDescriptorIdentity());
		if (!Spec)
		{
			return Fail(
				OutError,
				TEXT("editor_project_descriptor_not_exact"));
		}

		TSharedPtr<FJsonObject> Payload;
		if (!TryParsePayload(Request, Payload, OutError))
		{
			return false;
		}
		FString TargetRef;
		FString ExpectedRevision;
		if (!Payload->TryGetStringField(
				TEXT("target_ref"),
				TargetRef)
			|| !Payload->TryGetStringField(
				TEXT("expected_revision"),
				ExpectedRevision)
			|| !IsSafeText(TargetRef)
			|| !IsSafeText(ExpectedRevision))
		{
			return Fail(
				OutError,
				TEXT("editor_project_target_ref_invalid"));
		}

		const FUeAgentStateEntry* TargetEntry =
			FindEntry(CurrentSnapshot, TargetRef);
		if (!TargetEntry)
		{
			return Fail(
				OutError,
				TEXT("editor_project_target_snapshot_missing"),
				TargetRef);
		}
		FString CurrentRevision;
		FString CurrentProfile;
		if (!TryGetObjectString(
				TargetEntry->GetValue(),
				TEXT("target_revision"),
				CurrentRevision)
			|| CurrentRevision != ExpectedRevision)
		{
			return Fail(
				OutError,
				TEXT("editor_project_target_revision_stale"),
				TargetRef);
		}
		if (!TryGetObjectString(
				TargetEntry->GetValue(),
				TEXT("profile_id"),
				CurrentProfile)
			|| CurrentProfile != Spec->ProfileId)
		{
			return Fail(
				OutError,
				TEXT("editor_project_target_profile_mismatch"),
				TargetRef);
		}

		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		if (!Payload->TryGetArrayField(TEXT("changes"), Changes)
			|| !Changes
			|| Changes->IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("editor_project_changes_missing"));
		}

		const TSet<FString> RegisteredFields(
			SplitValues(Spec->FieldIds));
		TSet<FString> SeenFields;
		TArray<TSharedPtr<FJsonValue>> TypedChanges;
		bool bHasNativeCulture = false;
		FString RequestedNativeCulture;
		TSet<FString> RequestedCultures;
		for (const TSharedPtr<FJsonValue>& ChangeValue : *Changes)
		{
			const TSharedPtr<FJsonObject> Change =
				ChangeValue.IsValid()
					? ChangeValue->AsObject()
					: nullptr;
			FString FieldId;
			FString Presence;
			if (!Change.IsValid()
				|| !Change->TryGetStringField(
					TEXT("field_id"),
					FieldId)
				|| !RegisteredFields.Contains(FieldId))
			{
				return Fail(
					OutError,
					TEXT("editor_project_change_field_unregistered"),
					FieldId);
			}
			if (SeenFields.Contains(FieldId))
			{
				return Fail(
					OutError,
					TEXT("editor_project_change_field_duplicate"),
					FieldId);
			}
			SeenFields.Add(FieldId);

			const FFieldSpec* Field =
				FindFieldSpec(Spec->ProfileId, FieldId);
			if (!Field
				|| !Change->TryGetStringField(
					TEXT("presence"),
					Presence)
				|| !TSet<FString>(
					SplitValues(Field->AllowedPresences))
					.Contains(Presence))
			{
				return Fail(
					OutError,
					TEXT("editor_project_field_contract_mismatch"),
					FieldId);
			}

			const TSharedPtr<FJsonObject>* TypedValue = nullptr;
			const bool bHasValue =
				Change->TryGetObjectField(
					TEXT("value"),
					TypedValue)
				&& TypedValue
				&& TypedValue->IsValid();
			if (PresenceRequiresValue(Presence) != bHasValue)
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_value_presence_mismatch"),
					FieldId);
			}

			TSharedPtr<FJsonValue> CanonicalValue;
			const TSharedPtr<FJsonObject> InternalChange =
				MakeShared<FJsonObject>();
			InternalChange->SetStringField(
				TEXT("field_id"),
				FieldId);
			InternalChange->SetStringField(
				TEXT("field_binding_id"),
				Field->FieldBindingId);
			InternalChange->SetStringField(
				TEXT("presence"),
				Presence);
			if (bHasValue)
			{
				FString TypeId;
				CanonicalValue =
					(*TypedValue)->TryGetField(
						TEXT("canonical_value"));
				if (!(*TypedValue)->TryGetStringField(
						TEXT("type_id"),
						TypeId)
					|| TypeId != Field->TypeId
					|| !JsonValueMatchesKind(
						CanonicalValue,
						Field->ValueKind))
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_field_value_type_mismatch"),
						FieldId);
				}

				if ((FieldId == TEXT("entries")
						|| FieldId == TEXT("cvars"))
					&& FString(Spec->ProfileId)
						!= TEXT("string_table")
					&& !ValidateConfigEntryMap(
						CanonicalValue,
						OutError))
				{
					return false;
				}
				if (FString(Spec->ProfileId)
						== TEXT("project_settings")
					&& !ValidateSettingsChanges(
						CanonicalValue,
						CurrentSnapshot,
						OutError))
				{
					return false;
				}
				if (FString(Spec->ProfileId)
						== TEXT("device_profile")
					&& FieldId == TEXT("base_profile_ref"))
				{
					const FString BaseRef =
						CanonicalValue->AsString();
					if (BaseRef == TargetRef)
					{
						return Fail(
							OutError,
							TEXT(
								"editor_project_device_profile_cycle"),
							BaseRef);
					}
					const FUeAgentStateEntry* BaseEntry =
						FindEntry(CurrentSnapshot, BaseRef);
					FString BaseProfile;
					if (!BaseEntry
						|| !TryGetObjectString(
							BaseEntry->GetValue(),
							TEXT("profile_id"),
							BaseProfile)
						|| BaseProfile != TEXT("device_profile"))
					{
						return Fail(
							OutError,
							TEXT(
								"editor_project_device_profile_base_unresolved"),
							BaseRef);
					}
				}
				if (FString(Spec->ProfileId)
						== TEXT("localization_target")
					&& FieldId == TEXT("native_culture"))
				{
					RequestedNativeCulture =
						CanonicalValue->AsString();
					if (!ValidateCultureName(
							RequestedNativeCulture))
					{
						return Fail(
							OutError,
							TEXT(
								"editor_project_native_culture_invalid"));
					}
					bHasNativeCulture = true;
				}
				if (FString(Spec->ProfileId)
						== TEXT("localization_target")
					&& FieldId == TEXT("cultures"))
				{
					if (!ValidateCultures(
							CanonicalValue,
							OutError))
					{
						return false;
					}
					for (const TSharedPtr<FJsonValue>& Culture
						: CanonicalValue->AsArray())
					{
						RequestedCultures.Add(
							Culture->AsString());
					}
				}
				if (FString(Spec->ProfileId)
						== TEXT("string_table")
					&& FieldId == TEXT("entries")
					&& !ValidateStringTableEntries(
						CanonicalValue,
						OutError))
				{
					return false;
				}

				const TSharedPtr<FJsonObject> InternalValue =
					MakeShared<FJsonObject>();
				InternalValue->SetStringField(
					TEXT("type_id"),
					Field->TypeId);
				InternalValue->SetField(
					TEXT("canonical_value"),
					CanonicalValue);
				InternalChange->SetObjectField(
					TEXT("value"),
					InternalValue);
			}
			TypedChanges.Add(
				MakeShared<FJsonValueObject>(InternalChange));
		}

		if (FString(Spec->ProfileId)
				== TEXT("localization_target")
			&& !bHasNativeCulture)
		{
			return Fail(
				OutError,
				TEXT("editor_project_native_culture_missing"));
		}
		if (bHasNativeCulture
			&& !RequestedCultures.IsEmpty()
			&& !RequestedCultures.Contains(
				RequestedNativeCulture))
		{
			return Fail(
				OutError,
				TEXT(
					"editor_project_native_culture_not_generated"),
				RequestedNativeCulture);
		}

		const TSharedPtr<FJsonObject> ProposedAfter =
			MakeShared<FJsonObject>();
		ProposedAfter->SetStringField(
			TEXT("native_operation_type"),
			Spec->NativeOperationType);
		ProposedAfter->SetStringField(
			TEXT("native_binding_id"),
			MakeNativeBindingId(*Spec));
		ProposedAfter->SetStringField(
			TEXT("native_mechanic_id"),
			Spec->NativeMechanicId);
		ProposedAfter->SetStringField(
			TEXT("profile_id"),
			Spec->ProfileId);
		ProposedAfter->SetStringField(
			TEXT("target_ref"),
			TargetRef);
		ProposedAfter->SetStringField(
			TEXT("target_revision"),
			ExpectedRevision);
		ProposedAfter->SetArrayField(
			TEXT("typed_changes"),
			MoveTemp(TypedChanges));

		FUeAgentCanonicalValuePtr CanonicalAfter;
		if (!TryCanonicalValue(
				MakeShared<FJsonValueObject>(ProposedAfter),
				Resources,
				CanonicalAfter,
				OutError))
		{
			return false;
		}

		TArray<FString> AuthorityScopes =
			GetEditorProjectSimpleEffectAuthorityScopes(Spec->ProfileId);

		FChangeDraft Change(
			FUeAgentCanonicalIdentity(
				TEXT("uai.editor_project:") + TargetRef,
				Spec->NativeOperationType,
				TargetEntry->GetIdentity().GetOwnerId(),
				TargetRef,
				Spec->ProfileId,
				Context.GetContextRevision()),
			EChangeIntent::Update,
			FUeAgentCanonicalValuePtr(TargetEntry->GetValue()),
			MoveTemp(CanonicalAfter),
			MoveTemp(AuthorityScopes),
			TEXT("editor-project:typed-update"),
			{});
		if (!Change.IsValid())
		{
			return Fail(
				OutError,
				TEXT("editor_project_change_draft_invalid"));
		}

		FChangeSetDraft Draft(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			CurrentSnapshot,
			{MoveTemp(Change)});
		if (!Draft.IsValid())
		{
			return Fail(
				OutError,
				TEXT("editor_project_change_set_draft_invalid"));
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}

	bool TryResolveEditorProjectSimpleEffectNativeBinding(
		const FString& NativeBindingId,
		FString& OutNativeOperationType,
		FString& OutProfileId,
		FString& OutNativeMechanicId,
		FString& OutError)
	{
		OutNativeOperationType.Reset();
		OutProfileId.Reset();
		OutNativeMechanicId.Reset();
		OutError.Reset();
		const FDescriptorSpec* Spec =
			FindSpecByBinding(NativeBindingId);
		if (!Spec)
		{
			OutError =
				TEXT(
					"editor_project_native_binding_not_registered");
			return false;
		}
		OutNativeOperationType = Spec->NativeOperationType;
		OutProfileId = Spec->ProfileId;
		OutNativeMechanicId = Spec->NativeMechanicId;
		return true;
	}

	bool TryResolveEditorProjectSimpleEffectFieldContract(
		const FString& NativeBindingId,
		const FString& FieldId,
		FString& OutFieldBindingId,
		FString& OutTypeId,
		EUeAgentValueKind& OutValueKind,
		TArray<FString>& OutAllowedPresences,
		FString& OutError)
	{
		OutFieldBindingId.Reset();
		OutTypeId.Reset();
		OutValueKind = EUeAgentValueKind::Null;
		OutAllowedPresences.Reset();
		OutError.Reset();
		const FDescriptorSpec* Spec =
			FindSpecByBinding(NativeBindingId);
		const FFieldSpec* Field = Spec
			? FindFieldSpec(Spec->ProfileId, FieldId)
			: nullptr;
		if (!Spec || !Field
			|| !TSet<FString>(SplitValues(Spec->FieldIds))
				.Contains(FieldId))
		{
			OutError =
				TEXT(
					"editor_project_field_contract_not_registered");
			return false;
		}
		OutFieldBindingId = Field->FieldBindingId;
		OutTypeId = Field->TypeId;
		OutAllowedPresences =
			SplitValues(Field->AllowedPresences);
		switch (Field->ValueKind)
		{
		case EFieldValueKind::String:
			OutValueKind = EUeAgentValueKind::String;
			break;
		case EFieldValueKind::Boolean:
			OutValueKind = EUeAgentValueKind::Boolean;
			break;
		case EFieldValueKind::Integer:
			OutValueKind = EUeAgentValueKind::Integer;
			break;
		case EFieldValueKind::Number:
			OutValueKind = EUeAgentValueKind::Decimal;
			break;
		case EFieldValueKind::Array:
			OutValueKind = EUeAgentValueKind::Array;
			break;
		case EFieldValueKind::Object:
			OutValueKind = EUeAgentValueKind::Object;
			break;
		}
		return true;
	}
}
