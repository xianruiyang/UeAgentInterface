#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
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

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
            return false;
        }


		FString PayloadPointerPart(const FString& Text)
		{
			return Text.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		TSharedPtr<FJsonValue> PayloadKind(const TSharedPtr<FJsonValue>& Value)
		{
			const TCHAR* Kind = !Value.IsValid() ? TEXT("missing")
				: Value->Type == EJson::Null ? TEXT("null")
				: Value->Type == EJson::String ? TEXT("string")
				: Value->Type == EJson::Number ? TEXT("number")
				: Value->Type == EJson::Boolean ? TEXT("boolean")
				: Value->Type == EJson::Array ? TEXT("array")
				: Value->Type == EJson::Object ? TEXT("object") : TEXT("none");
			return MakeShared<FJsonValueString>(Kind);
		}

		FExecutionMutationError CapsValueFailure(const FString& Code, const FString& Reason,
			const FString& Path, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, const FString& SourceKind = TEXT("command_input"))
		{
			auto Error = FExecutionMutationError::Failure(Code, Reason, TEXT("execution"));
			Error.SourceKind = SourceKind;
			Error.PropertyPath = Path;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

		FExecutionMutationError CapsCondition(const FString& Code, const FString& Reason,
			const FString& Path, bool Actual = false, bool Expected = true,
			const FString& SourceKind = TEXT("command_input"))
		{
			return CapsValueFailure(Code, Reason, Path, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected), SourceKind);
		}

		const TCHAR* PayloadExpectedKind(EFieldValueKind Kind)
		{
			switch (Kind)
			{
			case EFieldValueKind::String: return TEXT("string");
			case EFieldValueKind::Boolean: return TEXT("boolean");
			case EFieldValueKind::Integer: return TEXT("integer number");
			case EFieldValueKind::Number: return TEXT("number");
			case EFieldValueKind::Array: return TEXT("array");
			case EFieldValueKind::Object: return TEXT("object");
			}
			return TEXT("supported field kind");
		}

		FString PayloadTargetKind(const FString& Profile)
		{
			if (Profile == TEXT("config_section")) return TEXT("configuration section");
			if (Profile == TEXT("device_profile")) return TEXT("device profile");
			if (Profile == TEXT("localization_target")) return TEXT("localization target");
			if (Profile == TEXT("platform_config")) return TEXT("platform configuration");
			if (Profile == TEXT("scalability_group")) return TEXT("scalability group");
			if (Profile == TEXT("project_settings")) return TEXT("project settings");
			if (Profile == TEXT("string_table")) return TEXT("string table");
			return TEXT("unrecognized target kind");
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
				UE::UAI::Contract::FDiagnostic Error;
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
			UE::UAI::Contract::FDiagnostic ParseError;
			if (!UE::UAI::Contract::Json::TryParseObject(
				Request.GetPayload().GetJsonValue(), OutPayload, ParseError))
			{
				auto Error = FExecutionMutationError::WrapFailure(TEXT("editor_project_payload_parse_failed"), ParseError);
				if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("command_input");
				return Fail(OutError, MoveTemp(Error));
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
			UE::UAI::Contract::FDiagnostic Error;
			return CanonicalJson::TryFromJsonValue(JsonValue, Resources, OutValue, Error)
				? true : Fail(OutError, Error);
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
				{
					FString Lexeme;
					return CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
						&& IsIntegralNumberLexeme(Lexeme);
				}
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

		bool ValidateConfigEntryMap(const TSharedPtr<FJsonValue>& Value,
			const FString& Path, FExecutionMutationError& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
				return Fail(OutError, CapsValueFailure(TEXT("editor_project_config_entry_invalid"),
					TEXT("Configuration entries must be an object of single-line string values."), Path,
					PayloadKind(Value), MakeShared<FJsonValueString>(TEXT("object"))));
			for (const auto& Pair : Value->AsObject()->Values)
			{
				const FString EntryPath = Path + TEXT("/") + PayloadPointerPart(Pair.Key);
				if (!IsSafeText(Pair.Key))
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_config_entry_invalid"),
						TEXT("Configuration keys must be non-empty, trimmed and contain no line breaks."), EntryPath,
						MakeShared<FJsonValueString>(Pair.Key), MakeShared<FJsonValueString>(TEXT("non-empty trimmed single-line key"))));
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_config_entry_invalid"),
						TEXT("Each configuration entry value must be a string."), EntryPath,
						PayloadKind(Pair.Value), MakeShared<FJsonValueString>(TEXT("string"))));
				const FString Text = Pair.Value->AsString();
				if (Text.Contains(TEXT("\r")) || Text.Contains(TEXT("\n")))
					return Fail(OutError, CapsCondition(TEXT("editor_project_config_entry_invalid"),
						TEXT("Configuration entry values must not contain line breaks."), EntryPath, true, false));
			}
			return true;
		}

		bool ValidateSettingsChanges(const TSharedPtr<FJsonValue>& Value,
			const FUeAgentCanonicalSnapshotRef& Snapshot, const FString& Path, FExecutionMutationError& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Array)
				return Fail(OutError, CapsValueFailure(TEXT("editor_project_settings_changes_invalid"),
					TEXT("Settings changes must be an array."), Path, PayloadKind(Value), MakeShared<FJsonValueString>(TEXT("array"))));
			TSet<FString> SeenFields;
			for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
			{
				const auto& Item = Value->AsArray()[Index];
				const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
				const auto Change = Item.IsValid() && Item->Type == EJson::Object ? Item->AsObject() : nullptr;
				FString FieldRef, Presence, TypeId;
				if (!Change.IsValid())
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_settings_change_invalid"),
						TEXT("Each settings change must be an object."), ItemPath, PayloadKind(Item), MakeShared<FJsonValueString>(TEXT("object"))));
				for (const auto& Field : {TPair<const TCHAR*, FString*>(TEXT("field_ref"), &FieldRef),
					TPair<const TCHAR*, FString*>(TEXT("presence"), &Presence), TPair<const TCHAR*, FString*>(TEXT("type_id"), &TypeId)})
				{
					if (!Change->TryGetStringField(Field.Key, *Field.Value))
						return Fail(OutError, CapsValueFailure(TEXT("editor_project_settings_change_invalid"),
							TEXT("A settings change is missing a required string field."), ItemPath + TEXT("/") + Field.Key,
							PayloadKind(Change->TryGetField(Field.Key)), MakeShared<FJsonValueString>(TEXT("string"))));
				}
				if (!IsSafeText(FieldRef) || !IsSafeText(TypeId))
				{
					const bool bFieldRef = !IsSafeText(FieldRef);
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_settings_change_invalid"),
						TEXT("Settings field and type references must be non-empty, trimmed and contain no line breaks."),
						ItemPath + (bFieldRef ? TEXT("/field_ref") : TEXT("/type_id")),
						MakeShared<FJsonValueString>(bFieldRef ? FieldRef : TypeId),
						MakeShared<FJsonValueString>(TEXT("non-empty trimmed single-line reference"))));
				}
				if (!TSet<FString>({TEXT("set"), TEXT("null"), TEXT("clear")}).Contains(Presence))
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_settings_change_invalid"),
						TEXT("The settings change presence is not supported."), ItemPath + TEXT("/presence"),
						MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set, null or clear"))));
				if (SeenFields.Contains(FieldRef))
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_settings_field_duplicate"),
						TEXT("A settings field appears more than once in this change list."), ItemPath + TEXT("/field_ref"),
						MakeShared<FJsonValueString>(FieldRef), MakeShared<FJsonValueString>(TEXT("unique field reference"))));
				SeenFields.Add(FieldRef);
				const auto* FieldEntry = FindEntry(Snapshot, FieldRef);
				if (!FieldEntry)
				{
					auto Error = CapsCondition(TEXT("editor_project_settings_field_unresolved"),
						TEXT("The current snapshot has no observation for the requested settings field."),
						ItemPath + TEXT("/field_ref"), false, true, TEXT("native_result"));
					Error.CandidateName = FieldRef;
					return Fail(OutError, MoveTemp(Error));
				}
				FString CurrentType;
				const bool bTypeCaptured = TryGetObjectString(FieldEntry->GetValue(), TEXT("type_id"), CurrentType);
				if (!bTypeCaptured || CurrentType != TypeId)
				{
					auto Error = CapsValueFailure(TEXT("editor_project_settings_field_type_mismatch"),
						bTypeCaptured ? TEXT("The requested settings field type differs from its captured type.")
							: TEXT("The captured settings field has no string type identifier."),
						ItemPath + TEXT("/type_id"), MakeShared<FJsonValueString>(bTypeCaptured ? TypeId : TEXT("missing or non-string captured type")),
						MakeShared<FJsonValueString>(bTypeCaptured ? CurrentType : TEXT("captured string type identifier")),
						bTypeCaptured ? TEXT("command_input") : TEXT("native_result"));
					Error.CandidateName = FieldRef;
					return Fail(OutError, MoveTemp(Error));
				}
				const auto PropertyValue = Change->TryGetField(TEXT("value"));
				if ((Presence == TEXT("set")) != PropertyValue.IsValid())
					return Fail(OutError, CapsCondition(TEXT("editor_project_settings_value_presence_mismatch"),
						Presence == TEXT("set") ? TEXT("A set settings change requires a value.")
							: TEXT("A null or clear settings change must omit value."),
						ItemPath + TEXT("/value"), PropertyValue.IsValid(), Presence == TEXT("set")));
				if (Presence == TEXT("set")
					&& ((TypeId == TEXT("uai.value.boolean.v1") && PropertyValue->Type != EJson::Boolean)
						|| (TypeId == TEXT("uai.value.string.v1") && PropertyValue->Type != EJson::String)
						|| (TypeId == TEXT("uai.value.integer.v1") && PropertyValue->Type != EJson::Number)))
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_settings_value_type_mismatch"),
						TEXT("The settings value JSON kind does not match its declared field type."), ItemPath + TEXT("/value"),
						PayloadKind(PropertyValue), MakeShared<FJsonValueString>(TypeId == TEXT("uai.value.boolean.v1")
							? TEXT("boolean") : TypeId == TEXT("uai.value.string.v1") ? TEXT("string") : TEXT("number"))));
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

		bool ValidateCultures(const TSharedPtr<FJsonValue>& Value,
			const FString& Path, FExecutionMutationError& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Array)
				return Fail(OutError, CapsValueFailure(TEXT("editor_project_culture_set_invalid"),
					TEXT("The culture set must be an array."), Path, PayloadKind(Value), MakeShared<FJsonValueString>(TEXT("array"))));
			TSet<FString> Cultures;
			for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
			{
				const auto& Item = Value->AsArray()[Index];
				const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
				if (!Item.IsValid() || Item->Type != EJson::String)
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_culture_set_invalid"),
						TEXT("Each culture must be a string culture name."), ItemPath, PayloadKind(Item), MakeShared<FJsonValueString>(TEXT("string"))));
				const FString Culture = Item->AsString();
				if (!ValidateCultureName(Culture))
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_culture_set_invalid"),
						TEXT("The culture name is not recognized or is not valid single-line text."), ItemPath,
						MakeShared<FJsonValueString>(Culture), MakeShared<FJsonValueString>(TEXT("recognized culture name"))));
				if (Cultures.Contains(Culture))
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_culture_set_invalid"),
						TEXT("The culture set contains a duplicate culture."), ItemPath,
						MakeShared<FJsonValueString>(Culture), MakeShared<FJsonValueString>(TEXT("unique culture name"))));
				Cultures.Add(Culture);
			}
			return true;
		}

		bool ValidateStringTableEntries(const TSharedPtr<FJsonValue>& Value,
			const FString& Path, FExecutionMutationError& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Array)
				return Fail(OutError, CapsValueFailure(TEXT("editor_project_string_table_entries_invalid"),
					TEXT("String table entries must be an array."), Path, PayloadKind(Value), MakeShared<FJsonValueString>(TEXT("array"))));
			TSet<FString> Keys;
			for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
			{
				const auto& Item = Value->AsArray()[Index];
				const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
				const auto Row = Item.IsValid() && Item->Type == EJson::Object ? Item->AsObject() : nullptr;
				FString Key, Source;
				const TSharedPtr<FJsonObject>* Metadata = nullptr;
				if (!Row.IsValid())
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_string_table_entry_invalid"),
						TEXT("Each string table entry must be an object."), ItemPath, PayloadKind(Item), MakeShared<FJsonValueString>(TEXT("object"))));
				if (!Row->TryGetStringField(TEXT("key"), Key) || !Row->TryGetStringField(TEXT("source"), Source))
				{
					const auto KeyValue = Row->TryGetField(TEXT("key"));
					const bool bKey = KeyValue.IsValid() && KeyValue->Type == EJson::String;
					auto Error = CapsValueFailure(Keys.Contains(Key) ? TEXT("editor_project_string_table_key_duplicate")
						: TEXT("editor_project_string_table_entry_invalid"),
						Keys.Contains(Key) ? TEXT("The entry repeats an existing key and has no string source.")
							: TEXT("String table entries require string key and source fields."),
						ItemPath + (bKey ? TEXT("/source") : TEXT("/key")),
						PayloadKind(bKey ? Row->TryGetField(TEXT("source")) : KeyValue), MakeShared<FJsonValueString>(TEXT("string")));
					Error.CandidateName = Key;
					return Fail(OutError, MoveTemp(Error));
				}
				if (!IsSafeText(Key))
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_string_table_entry_invalid"),
						TEXT("String table keys must be non-empty, trimmed and contain no line breaks."), ItemPath + TEXT("/key"),
						MakeShared<FJsonValueString>(Key), MakeShared<FJsonValueString>(TEXT("non-empty trimmed single-line key"))));
				if (Keys.Contains(Key))
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_string_table_key_duplicate"),
						TEXT("The string table entries contain a duplicate key."), ItemPath + TEXT("/key"),
						MakeShared<FJsonValueString>(Key), MakeShared<FJsonValueString>(TEXT("unique key"))));
				if (Row->TryGetObjectField(TEXT("metadata"), Metadata) && (!Metadata || !Metadata->IsValid()))
					return Fail(OutError, CapsCondition(TEXT("editor_project_string_table_entry_invalid"),
						TEXT("The string table metadata object is unavailable."), ItemPath + TEXT("/metadata")));
				Keys.Add(Key);
				if (Metadata && Metadata->IsValid())
				{
					for (const auto& Pair : (*Metadata)->Values)
					{
						const FString MetadataPath = ItemPath + TEXT("/metadata/") + PayloadPointerPart(Pair.Key);
						if (!IsSafeText(Pair.Key))
							return Fail(OutError, CapsValueFailure(TEXT("editor_project_string_table_metadata_invalid"),
								TEXT("String table metadata names must be non-empty, trimmed and contain no line breaks."), MetadataPath,
								MakeShared<FJsonValueString>(Pair.Key), MakeShared<FJsonValueString>(TEXT("non-empty trimmed single-line name"))));
						if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
							return Fail(OutError, CapsValueFailure(TEXT("editor_project_string_table_metadata_invalid"),
								TEXT("String table metadata values must be strings."), MetadataPath,
								PayloadKind(Pair.Value), MakeShared<FJsonValueString>(TEXT("string"))));
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	bool TryGetEditorProjectSimpleEffectProductionProfiles(
		TArray<FUeAgentEditorProjectSimpleEffectProductionProfile>&
			OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
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
				OutError = CapsCondition(TEXT("editor_project_production_descriptor_missing"),
					TEXT("A required editor/project operation descriptor is missing from the registry."),
					TEXT("/registry/descriptor/registered"), false, true, TEXT("native_result"));
				OutError.Stage = TEXT("registry");
				OutError.CandidateName = Spec.CapabilityId;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Spec.DescriptorId);
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
				const TCHAR* Field = Profile.DescriptorId.IsEmpty() ? TEXT("descriptor")
					: Profile.DescriptorVersion <= 0 ? TEXT("descriptor_version")
					: Profile.DescriptorHash.IsEmpty() ? TEXT("descriptor_fingerprint")
					: Profile.CapabilityId.IsEmpty() ? TEXT("capability")
					: Profile.ProfileId.IsEmpty() ? TEXT("target_kind")
					: Profile.NativeOperationType.IsEmpty() ? TEXT("native_operation")
					: Profile.NativeBindingId.IsEmpty() ? TEXT("native_binding")
					: Profile.NativeMechanicId.IsEmpty() ? TEXT("native_mechanic") : TEXT("registered_fields");
				OutError = CapsCondition(TEXT("editor_project_production_profile_invalid"),
					FString::Printf(TEXT("The editor/project production profile has invalid or missing required %s data."), Field),
					TEXT("/profile/") + FString(Field) + TEXT("/valid"), false, true, TEXT("native_result"));
				OutError.Stage = TEXT("registry");
				OutError.CandidateName = Profile.CapabilityId;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Spec.DescriptorId);
				OutProfiles.Reset();
				return false;
			}
			OutProfiles.Add(MoveTemp(Profile));
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
			if (!Request.IsValid())
				return Fail(OutError, FExecutionMutationError::WrapFailure(
					TEXT("editor_project_draft_request_invalid"), Request.GetValidationDiagnostic()));
			const TCHAR* Field = Context.GetProjectInstanceId().IsEmpty() ? TEXT("/context/project_identity/present")
				: Context.GetEditorInstanceId().IsEmpty() ? TEXT("/context/editor_identity/present")
				: Context.GetContextRevision().IsEmpty() ? TEXT("/context/revision/present") : TEXT("/snapshot/state_plane");
			const TCHAR* Reason = Context.GetProjectInstanceId().IsEmpty() ? TEXT("The operation context has no project identity.")
				: Context.GetEditorInstanceId().IsEmpty() ? TEXT("The operation context has no editor identity.")
				: Context.GetContextRevision().IsEmpty() ? TEXT("The operation context has no revision.")
				: TEXT("Draft creation requires an authoring memory snapshot.");
			if (CurrentSnapshot->GetStatePlane() == EUeAgentStatePlane::AuthoringMemory
				|| Context.GetProjectInstanceId().IsEmpty() || Context.GetEditorInstanceId().IsEmpty() || Context.GetContextRevision().IsEmpty())
				return Fail(OutError, CapsCondition(TEXT("editor_project_draft_request_invalid"), Reason, Field, false, true, TEXT("native_result")));
			auto Plane = MakeShared<FJsonObject>();
			Plane->SetStringField(TEXT("enum_type"), TEXT("EUeAgentStatePlane"));
			Plane->SetNumberField(TEXT("value"), static_cast<int32>(CurrentSnapshot->GetStatePlane()));
			return Fail(OutError, CapsValueFailure(TEXT("editor_project_draft_request_invalid"), Reason, Field,
				MakeShared<FJsonValueObject>(Plane), MakeShared<FJsonValueString>(TEXT("AuthoringMemory")), TEXT("native_result")));
		}

		const FDescriptorSpec* Spec =
			FindSpec(Request.GetDescriptorIdentity());
		if (!Spec)
		{
			OutError = CapsCondition(TEXT("editor_project_descriptor_not_exact"),
				TEXT("No exact editor/project production descriptor is registered for this operation request."),
				TEXT("/descriptor/exact_match"), false, true, TEXT("native_result"));
			auto Identity = MakeShared<FJsonObject>();
			Identity->SetStringField(TEXT("descriptor_id"), Request.GetDescriptorIdentity().GetDescriptorId());
			Identity->SetNumberField(TEXT("version"), Request.GetDescriptorIdentity().GetVersion());
			Identity->SetStringField(TEXT("hash"), Request.GetDescriptorIdentity().GetDescriptorHash());
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Identity);
			return false;
		}

		TSharedPtr<FJsonObject> Payload;
		if (!TryParsePayload(Request, Payload, OutError))
		{
			return false;
		}
		FString TargetRef;
		if (!Payload->TryGetStringField(
				TEXT("target_ref"),
				TargetRef)
			|| !IsSafeText(TargetRef))
		{
			const auto Value = Payload->TryGetField(TEXT("target_ref"));
			return Fail(OutError, CapsValueFailure(TEXT("editor_project_target_ref_invalid"),
				TEXT("target_ref must be a non-empty, trimmed resource reference without line breaks."), TEXT("/target_ref"),
				Value.IsValid() && Value->Type == EJson::String ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TargetRef)) : PayloadKind(Value),
				MakeShared<FJsonValueString>(TEXT("non-empty trimmed single-line resource reference"))));
		}

		ON_SCOPE_EXIT
		{
			if (OutError.IsValid() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
		};

		const FUeAgentStateEntry* TargetEntry =
			FindEntry(CurrentSnapshot, TargetRef);
		if (!TargetEntry)
		{
			return Fail(OutError, CapsCondition(TEXT("editor_project_target_snapshot_missing"),
				TEXT("The current snapshot has no observation for the requested target."), TEXT("/target/observation/available"),
				false, true, TEXT("native_result")));
		}
		FString CurrentRevision;
		FString CurrentProfile;
		if (!TryGetObjectString(
			TargetEntry->GetValue(),
			TEXT("target_revision"),
			CurrentRevision)
			|| CurrentRevision.IsEmpty())
		{
			return Fail(OutError, CapsCondition(TEXT("editor_project_target_revision_stale"),
				TEXT("The target observation has no usable string revision."), TEXT("/target/revision/available"), false, true, TEXT("native_result")));
		}
		if (!TryGetObjectString(
				TargetEntry->GetValue(),
				TEXT("profile_id"),
				CurrentProfile)
			|| CurrentProfile != Spec->ProfileId)
		{
			OutError = CapsValueFailure(TEXT("editor_project_target_profile_mismatch"),
				TEXT("The target kind does not match the operation's required target kind."), TEXT("/target/kind"),
				MakeShared<FJsonValueString>(PayloadTargetKind(CurrentProfile)), MakeShared<FJsonValueString>(PayloadTargetKind(Spec->ProfileId)),
				TEXT("native_result"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentProfile);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Spec->ProfileId);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		if (!Payload->TryGetArrayField(TEXT("changes"), Changes)
			|| !Changes
			|| Changes->IsEmpty())
		{
			return Fail(OutError, CapsValueFailure(TEXT("editor_project_changes_missing"),
				TEXT("The operation requires a non-empty changes array."), TEXT("/changes"),
				Changes ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Changes->Num())) : PayloadKind(Payload->TryGetField(TEXT("changes"))),
				MakeShared<FJsonValueString>(TEXT("non-empty array"))));
		}

		const TSet<FString> RegisteredFields(
			SplitValues(Spec->FieldIds));
		TSet<FString> SeenFields;
		TArray<TSharedPtr<FJsonValue>> TypedChanges;
		bool bHasNativeCulture = false;
		FString RequestedNativeCulture;
		FString NativeCultureValuePath;
		TSet<FString> RequestedCultures;
		for (int32 ChangeIndex = 0; ChangeIndex < Changes->Num(); ++ChangeIndex)
		{
			const auto& ChangeValue = (*Changes)[ChangeIndex];
			const FString ChangePath = TEXT("/changes/") + FString::FromInt(ChangeIndex);
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
				const auto FieldValue = Change.IsValid() ? Change->TryGetField(TEXT("field_id")) : nullptr;
				return Fail(OutError, CapsValueFailure(TEXT("editor_project_change_field_unregistered"),
					!Change.IsValid() ? TEXT("Each change must be an object.")
						: FieldValue.IsValid() && FieldValue->Type == EJson::String
							? TEXT("The requested field is not registered for this operation.") : TEXT("Each change requires a string field_id."),
					!Change.IsValid() ? ChangePath : ChangePath + TEXT("/field_id"),
					!Change.IsValid() ? PayloadKind(ChangeValue) : FieldValue.IsValid() && FieldValue->Type == EJson::String
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FieldId)) : PayloadKind(FieldValue),
					MakeShared<FJsonValueString>(!Change.IsValid() ? FString(TEXT("object")) : FString(Spec->FieldIds).Replace(TEXT("|"), TEXT(", ")))));
			}
			if (SeenFields.Contains(FieldId))
			{
				return Fail(OutError, CapsValueFailure(TEXT("editor_project_change_field_duplicate"),
					TEXT("A field appears more than once in the operation change list."), ChangePath + TEXT("/field_id"),
					MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(TEXT("unique field"))));
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
				if (!Field)
				{
					OutError = CapsCondition(TEXT("editor_project_field_contract_mismatch"),
						TEXT("No native field contract is registered for the requested operation field."),
						ChangePath + TEXT("/field_id"), false, true, TEXT("native_result"));
					OutError.CandidateName = FieldId;
					return false;
				}
				const auto Value = Change->TryGetField(TEXT("presence"));
				return Fail(OutError, CapsValueFailure(TEXT("editor_project_field_contract_mismatch"),
					TEXT("The field presence is missing or is not allowed by this field contract."), ChangePath + TEXT("/presence"),
					Value.IsValid() && Value->Type == EJson::String ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Presence)) : PayloadKind(Value),
					MakeShared<FJsonValueString>(FString(Field->AllowedPresences).Replace(TEXT("|"), TEXT(", ")))));
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
				return Fail(OutError, CapsCondition(TEXT("editor_project_value_presence_mismatch"),
					PresenceRequiresValue(Presence) ? TEXT("This field presence requires a typed value object.")
						: TEXT("This field presence must omit the typed value object."),
					ChangePath + TEXT("/value"), bHasValue, PresenceRequiresValue(Presence)));
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
			const FString ValuePath = ChangePath + TEXT("/value/canonical_value");
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
					const auto TypeValue = (*TypedValue)->TryGetField(TEXT("type_id"));
					const bool bTypeMatches = TypeValue.IsValid() && TypeValue->Type == EJson::String && TypeId == Field->TypeId;
					return Fail(OutError, CapsValueFailure(TEXT("editor_project_field_value_type_mismatch"),
						bTypeMatches ? TEXT("The canonical value does not match the field's required value kind.")
							: TEXT("The typed value identifier does not match the field contract."),
						ChangePath + (bTypeMatches ? TEXT("/value/canonical_value") : TEXT("/value/type_id")),
						bTypeMatches ? Field->ValueKind == EFieldValueKind::Integer && CanonicalValue.IsValid() && CanonicalValue->Type == EJson::Number
							? CanonicalValue : PayloadKind(CanonicalValue)
							: TypeValue.IsValid() && TypeValue->Type == EJson::String ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TypeId)) : PayloadKind(TypeValue),
						MakeShared<FJsonValueString>(bTypeMatches ? PayloadExpectedKind(Field->ValueKind) : Field->TypeId)));
				}

				if ((FieldId == TEXT("entries")
						|| FieldId == TEXT("cvars"))
					&& FString(Spec->ProfileId)
						!= TEXT("string_table")
					&& !ValidateConfigEntryMap(
						CanonicalValue, ValuePath,
						OutError))
				{
					return false;
				}
				if (FString(Spec->ProfileId)
						== TEXT("project_settings")
					&& !ValidateSettingsChanges(
						CanonicalValue,
						CurrentSnapshot, ValuePath,
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
						return Fail(OutError, CapsValueFailure(TEXT("editor_project_device_profile_cycle"),
							TEXT("A device profile cannot name itself as its base profile."), ValuePath,
							MakeShared<FJsonValueString>(BaseRef), MakeShared<FJsonValueString>(TEXT("different device profile reference"))));
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
						OutError = CapsValueFailure(TEXT("editor_project_device_profile_base_unresolved"),
							!BaseEntry ? TEXT("The selected base device profile has no observation in the current snapshot.")
								: TEXT("The selected base resource is not a device profile."), ValuePath,
							MakeShared<FJsonValueString>(BaseEntry ? PayloadTargetKind(BaseProfile) : TEXT("missing target observation")),
							MakeShared<FJsonValueString>(TEXT("device profile")), TEXT("native_result"));
						OutError.CandidateName = BaseRef;
						OutError.ActualEvidence = MakeShared<FJsonValueString>(BaseProfile);
						return false;
					}
				}
				if (FString(Spec->ProfileId)
						== TEXT("localization_target")
					&& FieldId == TEXT("native_culture"))
				{
					RequestedNativeCulture =
						CanonicalValue->AsString();
					NativeCultureValuePath = ValuePath;
					if (!ValidateCultureName(
							RequestedNativeCulture))
					{
						return Fail(OutError, CapsValueFailure(TEXT("editor_project_native_culture_invalid"),
							TEXT("The requested native culture name is not recognized or is not valid single-line text."), ValuePath,
							MakeShared<FJsonValueString>(RequestedNativeCulture), MakeShared<FJsonValueString>(TEXT("recognized culture name"))));
					}
					bHasNativeCulture = true;
				}
				if (FString(Spec->ProfileId)
						== TEXT("localization_target")
					&& FieldId == TEXT("cultures"))
				{
					if (!ValidateCultures(
							CanonicalValue, ValuePath,
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
						CanonicalValue, ValuePath,
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
			OutError = CapsCondition(TEXT("editor_project_native_culture_missing"),
				TEXT("A localization target update requires a native_culture change."), TEXT("/changes"));
			OutError.CandidateName = TEXT("native_culture");
			return false;
		}
		if (bHasNativeCulture
			&& !RequestedCultures.IsEmpty()
			&& !RequestedCultures.Contains(
				RequestedNativeCulture))
		{
			TArray<TSharedPtr<FJsonValue>> Cultures;
			TArray<FString> Names = RequestedCultures.Array();
			Names.Sort();
			for (const FString& Name : Names) Cultures.Add(MakeShared<FJsonValueString>(Name));
			return Fail(OutError, CapsValueFailure(TEXT("editor_project_native_culture_not_generated"),
				TEXT("The native culture is not included in the requested generation cultures."), NativeCultureValuePath,
				MakeShared<FJsonValueString>(RequestedNativeCulture), MakeShared<FJsonValueArray>(Cultures)));
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
			CurrentRevision);
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
			return Fail(OutError, FExecutionMutationError::WrapFailure(
				TEXT("editor_project_change_draft_invalid"), Change.GetValidationDiagnostic()));
		}

		FChangeSetDraft Draft(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			CurrentSnapshot,
			{MoveTemp(Change)});
		if (!Draft.IsValid())
		{
			return Fail(OutError, FExecutionMutationError::WrapFailure(
				TEXT("editor_project_change_set_draft_invalid"), Draft.GetValidationDiagnostic()));
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}

	bool TryResolveEditorProjectSimpleEffectNativeBinding(
		const FString& NativeBindingId,
		FString& OutNativeOperationType,
		FString& OutProfileId,
		FString& OutNativeMechanicId,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNativeOperationType.Reset();
		OutProfileId.Reset();
		OutNativeMechanicId.Reset();
		OutError.Reset();
		const FDescriptorSpec* Spec =
			FindSpecByBinding(NativeBindingId);
		if (!Spec)
		{
			OutError = CapsCondition(TEXT("editor_project_native_binding_not_registered"),
				TEXT("No editor/project execution contract is registered for the requested native binding."),
				TEXT("/native_binding/registered"), false, true, TEXT("native_result"));
			OutError.Stage = TEXT("registry");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(NativeBindingId);
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = CapsCondition(TEXT("editor_project_field_contract_not_registered"),
				!Spec ? TEXT("The requested native binding has no registered editor/project contract.")
					: !Field ? TEXT("The requested field has no contract for this target kind.")
					: TEXT("The requested field is not exposed by this operation."),
				!Spec ? FString(TEXT("/native_binding/registered")) : TEXT("/fields/") + PayloadPointerPart(FieldId) + TEXT("/registered"),
				false, true, TEXT("native_result"));
			OutError.Stage = TEXT("registry");
			OutError.CandidateName = FieldId;
			OutError.ActualEvidence = MakeShared<FJsonValueString>(NativeBindingId);
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
