#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationDescriptors.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	using namespace UE::UAI::Contract;

	namespace
	{
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationDescriptorSpec.inl"

		FString PointerKey(FString Key)
		{
			Key.ReplaceInline(TEXT("~"), TEXT("~0"));
			Key.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Key;
		}

		TSharedPtr<FJsonValue> RegistrationValue(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value) return MakeShared<FJsonValueString>(TEXT("<missing>"));
			if (Value->Type == EJson::Object) return MakeShared<FJsonValueString>(TEXT("object"));
			if (Value->Type == EJson::Array) return MakeShared<FJsonValueString>(TEXT("array"));
			return Value;
		}

		TSharedPtr<FJsonValue> RegistrationChoices(const TArray<FString>& Names)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Name : Names) Values.Add(MakeShared<FJsonValueString>(Name));
			return MakeShared<FJsonValueArray>(MoveTemp(Values));
		}

		FDiagnostic RegistrationFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("registration"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> ExpandSchemaValue(const TSharedPtr<FJsonValue>& Value,
			const TSharedPtr<FJsonObject>& Components, TSet<FString>& ExpansionStack,
			FDiagnostic& OutError, const FString& Path)
		{
			if (!Value.IsValid())
			{
				OutError = RegistrationFact(TEXT("timeline_animation_schema_value_invalid"),
					TEXT("Descriptor schema value is missing."), Path, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return nullptr;
			}
			switch (Value->Type)
			{
			case EJson::Null: return MakeShared<FJsonValueNull>();
			case EJson::String: return MakeShared<FJsonValueString>(Value->AsString());
			case EJson::Number: return MakeShared<FJsonValueNumber>(Value->AsNumber());
			case EJson::Boolean: return MakeShared<FJsonValueBoolean>(Value->AsBool());
			case EJson::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Expanded;
				for (const auto& Item : Value->AsArray())
				{
					auto ExpandedItem = ExpandSchemaValue(Item, Components, ExpansionStack, OutError, Path + TEXT("/") + FString::FromInt(Expanded.Num()));
					if (!ExpandedItem.IsValid()) return nullptr;
					Expanded.Add(MoveTemp(ExpandedItem));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Expanded));
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				if (!Object.IsValid())
				{
					OutError = RegistrationFact(TEXT("timeline_animation_schema_object_invalid"),
						TEXT("Descriptor schema object is missing."), Path, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return nullptr;
				}
				FString ComponentName;
				if (Object->TryGetStringField(TEXT("component"), ComponentName))
				{
					if (Object->Values.Num() != 1)
					{
						OutError = RegistrationFact(TEXT("timeline_animation_component_reference_invalid"),
							TEXT("Schema component reference cannot contain additional fields."), Path,
							MakeShared<FJsonValueNumber>(Object->Values.Num()), MakeShared<FJsonValueNumber>(1));
						return nullptr;
					}
					if (ComponentName.IsEmpty())
					{
						OutError = RegistrationFact(TEXT("timeline_animation_component_reference_invalid"),
							TEXT("Schema component name cannot be empty."), Path + TEXT("/component"),
							MakeShared<FJsonValueString>(ComponentName), MakeShared<FJsonValueString>(TEXT("non-empty component name")));
						return nullptr;
					}
					const TSharedPtr<FJsonValue>* Definition = Components->Values.Find(ComponentName);
					if (!Definition || !Definition->IsValid())
					{
						OutError = RegistrationFact(TEXT("timeline_animation_component_missing"),
							TEXT("Referenced schema component has no definition."), Path + TEXT("/component"),
							MakeShared<FJsonValueString>(ComponentName), MakeShared<FJsonValueString>(TEXT("defined schema component")));
						return nullptr;
					}
					if (ExpansionStack.Contains(ComponentName))
					{
						OutError = RegistrationFact(TEXT("timeline_animation_component_cycle"),
							TEXT("Schema component reference creates a recursive expansion cycle."), Path + TEXT("/component"),
							MakeShared<FJsonValueString>(ComponentName), MakeShared<FJsonValueString>(TEXT("acyclic component references")));
						return nullptr;
					}
					ExpansionStack.Add(ComponentName);
					auto Expanded = ExpandSchemaValue(*Definition, Components, ExpansionStack, OutError, TEXT("/components/") + PointerKey(ComponentName));
					ExpansionStack.Remove(ComponentName);
					return Expanded;
				}
				const auto Expanded = MakeShared<FJsonObject>();
				for (const auto& Pair : Object->Values)
				{
					auto ExpandedField = ExpandSchemaValue(Pair.Value, Components, ExpansionStack, OutError, Path + TEXT("/") + PointerKey(Pair.Key));
					if (!ExpandedField.IsValid()) return nullptr;
					Expanded->SetField(Pair.Key, MoveTemp(ExpandedField));
				}
				return MakeShared<FJsonValueObject>(Expanded);
			}
			default:
				OutError = RegistrationFact(TEXT("timeline_animation_schema_json_kind_invalid"),
					TEXT("Descriptor schema contains an unsupported JSON kind."), Path,
					MakeShared<FJsonValueNumber>(static_cast<int32>(Value->Type)), MakeShared<FJsonValueString>(TEXT("null, string, number, boolean, array or object")));
				return nullptr;
			}
		}

		bool TryBuildSchema(const TArray<TSharedPtr<FJsonValue>>& Fields,
			const TSharedPtr<FJsonObject>& Components, FString& OutCanonical, FDiagnostic& OutError, const FString& FieldsPath)
		{
			const auto Properties = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Required;
			for (int32 Index = 0; Index < Fields.Num(); ++Index)
			{
				const auto& FieldValue = Fields[Index];
				const TSharedPtr<FJsonObject> Field = FieldValue.IsValid() ? FieldValue->AsObject() : nullptr;
				const FString FieldPath = FieldsPath + TEXT("/") + FString::FromInt(Index);
				if (!Field.IsValid())
				{
					OutError = RegistrationFact(TEXT("timeline_animation_schema_field_invalid"), TEXT("Descriptor field must be an object."),
						FieldPath, RegistrationValue(FieldValue), MakeShared<FJsonValueString>(TEXT("field object")));
					return false;
				}
				FString Name;
				bool bRequired = false;
				if (!Field->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
				{
					OutError = RegistrationFact(TEXT("timeline_animation_schema_field_invalid"), TEXT("Descriptor field requires a non-empty name."),
						FieldPath + TEXT("/name"), RegistrationValue(Field->TryGetField(TEXT("name"))), MakeShared<FJsonValueString>(TEXT("non-empty string")));
					return false;
				}
				if (!Field->TryGetBoolField(TEXT("required"), bRequired))
				{
					OutError = RegistrationFact(TEXT("timeline_animation_schema_field_invalid"), TEXT("Descriptor field required flag must be boolean."),
						FieldPath + TEXT("/required"), RegistrationValue(Field->TryGetField(TEXT("required"))), MakeShared<FJsonValueString>(TEXT("boolean")));
					return false;
				}
				if (Properties->HasField(Name))
				{
					OutError = RegistrationFact(TEXT("timeline_animation_schema_field_invalid"), TEXT("Descriptor schema repeats a field name."),
						FieldPath + TEXT("/name"), MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("unique field name")));
					return false;
				}
				TSharedPtr<FJsonValue> Template;
				FString TemplatePath = FieldPath;
				FString ComponentName;
				if (Field->TryGetStringField(TEXT("component"), ComponentName))
				{
					const auto Reference = MakeShared<FJsonObject>();
					Reference->SetStringField(TEXT("component"), ComponentName);
					Template = MakeShared<FJsonValueObject>(Reference);
				}
				else
				{
					const TSharedPtr<FJsonObject>* Schema = nullptr;
					if (!Field->TryGetObjectField(TEXT("schema"), Schema) || !Schema || !Schema->IsValid())
					{
						OutError = RegistrationFact(TEXT("timeline_animation_schema_field_missing"), TEXT("Descriptor field requires a schema object or component reference."),
							FieldPath + TEXT("/schema"), RegistrationValue(Field->TryGetField(TEXT("schema"))), MakeShared<FJsonValueString>(TEXT("schema object")));
						return false;
					}
					Template = MakeShared<FJsonValueObject>(*Schema);
					TemplatePath += TEXT("/schema");
				}
				TSet<FString> Stack;
				auto Expanded = ExpandSchemaValue(Template, Components, Stack, OutError, TemplatePath);
				if (!Expanded.IsValid()) return false;
				Properties->SetField(Name, MoveTemp(Expanded));
				if (bRequired) Required.Add(MakeShared<FJsonValueString>(Name));
			}
			const auto Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), TEXT("object"));
			Root->SetObjectField(TEXT("properties"), Properties);
			if (!Required.IsEmpty()) Root->SetArrayField(TEXT("required"), MoveTemp(Required));
			Root->SetBoolField(TEXT("additionalProperties"), false);
			const bool bCanonical = Json::TryCanonicalize(Root, OutCanonical, OutError);
			if (!bCanonical)
			{
				OutError.SourceKind = TEXT("native_result");
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = FieldsPath;
			}
			return bCanonical;
		}

		bool TryFacet(
			const FString& Text,
			EUeAgentExecutionContextFacet& OutFacet)
		{
			static const TMap<FString, EUeAgentExecutionContextFacet> Values = {
				{TEXT("world"), EUeAgentExecutionContextFacet::World},
				{TEXT("level"), EUeAgentExecutionContextFacet::Level},
				{TEXT("data_layer"), EUeAgentExecutionContextFacet::DataLayer},
				{TEXT("world_partition"), EUeAgentExecutionContextFacet::WorldPartition},
				{TEXT("pie_instance"), EUeAgentExecutionContextFacet::PieInstance},
				{TEXT("net_mode"), EUeAgentExecutionContextFacet::NetMode},
				{TEXT("net_role"), EUeAgentExecutionContextFacet::NetRole},
				{TEXT("asset_editor"), EUeAgentExecutionContextFacet::AssetEditor},
				{TEXT("viewport"), EUeAgentExecutionContextFacet::Viewport},
				{TEXT("selection"), EUeAgentExecutionContextFacet::Selection},
			};
			const EUeAgentExecutionContextFacet* Found = Values.Find(Text);
			if (!Found)
			{
				return false;
			}
			OutFacet = *Found;
			return true;
		}

		bool TryQueryKind(const FString& Text, EUeAgentQueryKind& OutKind)
		{
			if (Text == TEXT("catalog")) { OutKind = EUeAgentQueryKind::Catalog; return true; }
			if (Text == TEXT("state")) { OutKind = EUeAgentQueryKind::State; return true; }
			if (Text == TEXT("runtime_probe")) { OutKind = EUeAgentQueryKind::RuntimeProbe; return true; }
			if (Text == TEXT("capability")) { OutKind = EUeAgentQueryKind::Capability; return true; }
			if (Text == TEXT("reference")) { OutKind = EUeAgentQueryKind::Reference; return true; }
			return false;
		}

		TSharedPtr<FJsonObject> ParseCanonicalObject(
			const FString& Canonical,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> Object;
			if (!Json::TryParseObject(Canonical, Object, OutError))
			{
				OutError.SourceKind = TEXT("native_result");
				return nullptr;
			}
			return Object;
		}

		TSharedPtr<FJsonObject> BuildDescriptorObject(
			const TSharedPtr<FJsonObject>& Profile,
			const FString& InputSchema,
			const FString& ResultSchema,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedPtr<FJsonObject> Input =
				ParseCanonicalObject(InputSchema, OutError);
			if (!Input.IsValid()) return nullptr;
			const TSharedPtr<FJsonObject> Result = ParseCanonicalObject(ResultSchema, OutError);
			if (!Result.IsValid()) return nullptr;

			const TSharedPtr<FJsonObject> Policy = MakeShared<FJsonObject>();
			Policy->SetArrayField(
				TEXT("allowed_state_planes"),
				Profile->GetArrayField(TEXT("state_planes")));
			Policy->SetArrayField(
				TEXT("required_facets"),
				Profile->GetArrayField(TEXT("required_facets")));
			Policy->SetStringField(
				TEXT("resolution_policy"),
				TEXT("explicit_only"));
			Policy->SetStringField(
				TEXT("transient_access_policy"),
				Profile->GetStringField(TEXT("operation_kind")) == TEXT("query")
					? TEXT("read_only")
					: TEXT("temporary_mutation_allowed"));

			const TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
			Descriptor->SetStringField(
				TEXT("capability_id"),
				Profile->GetStringField(TEXT("capability_id")));
			Descriptor->SetStringField(
				TEXT("descriptor_id"),
				Profile->GetStringField(TEXT("descriptor_id")));
			Descriptor->SetStringField(TEXT("domain_id"), TEXT("timeline_animation"));
			Descriptor->SetObjectField(TEXT("execution_context_policy"), Policy);
			Descriptor->SetObjectField(TEXT("input_schema"), Input);
			const FString Kind = Profile->GetStringField(TEXT("operation_kind"));
			Descriptor->SetStringField(TEXT("operation_kind"), Kind);
			if (Kind == TEXT("query"))
			{
				const TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
				Query->SetStringField(TEXT("materialization_policy"), TEXT("runtime_observe"));
				Query->SetStringField(
					TEXT("provider_binding"),
					Profile->GetStringField(TEXT("provider_binding")));
				Query->SetStringField(
					TEXT("query_kind"),
					Profile->GetStringField(TEXT("query_kind")));
				Query->SetStringField(TEXT("side_effect_policy"), TEXT("no_persistent_mutation"));
				Descriptor->SetObjectField(TEXT("query"), Query);
			}
			Descriptor->SetObjectField(TEXT("result_schema"), Result);
			Descriptor->SetField(
				TEXT("version"), MakeShared<FJsonValueNumberString>(TEXT("1")));
			return Descriptor;
		}
	}

	bool TryCreateTimelineAnimationDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Root;
		FString SpecText;
		SpecText.Reserve(32 * 1024);
		SpecText.Append(UTF8_TO_TCHAR(GTimelineAnimationDescriptorSpecJson0));
		SpecText.Append(UTF8_TO_TCHAR(GTimelineAnimationDescriptorSpecJson1));
		SpecText.Append(UTF8_TO_TCHAR(GTimelineAnimationDescriptorSpecJson2));
		static_assert(GTimelineAnimationDescriptorSpecChunkCount == 3);
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(SpecText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = RegistrationFact(TEXT("timeline_animation_descriptor_spec_invalid"),
				TEXT("Timeline-animation descriptor specification must be object JSON. ") + Reader->GetErrorMessage(), TEXT("/"),
				MakeShared<FJsonValueString>(TEXT("invalid object JSON")), MakeShared<FJsonValueString>(TEXT("object JSON")));
			return false;
		}
		const TSharedPtr<FJsonObject> Components =
			Root->GetObjectField(TEXT("components"));
		const TArray<TSharedPtr<FJsonValue>>& Profiles =
			Root->GetArrayField(TEXT("descriptors"));
		if (!Components.IsValid())
		{
			OutError = RegistrationFact(TEXT("timeline_animation_descriptor_spec_incomplete"),
				TEXT("Descriptor specification requires a components object."), TEXT("/components"),
				RegistrationValue(Root->TryGetField(TEXT("components"))), MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}

		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(Profiles.Num());
		for (int32 ProfileIndex = 0; ProfileIndex < Profiles.Num(); ++ProfileIndex)
		{
			const auto& ProfileValue = Profiles[ProfileIndex];
			const FString ProfilePath = TEXT("/descriptors/") + FString::FromInt(ProfileIndex);
			const TSharedPtr<FJsonObject> Profile =
				ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
			if (!Profile.IsValid())
			{
				OutError = RegistrationFact(TEXT("timeline_animation_descriptor_profile_invalid"),
					TEXT("Descriptor profile must be an object."), ProfilePath,
					RegistrationValue(ProfileValue), MakeShared<FJsonValueString>(TEXT("descriptor profile object")));
				return false;
			}

			FString InputSchema;
			FString ResultSchema;
			if (!TryBuildSchema(
					Profile->GetArrayField(TEXT("input_fields")),
					Components, InputSchema, OutError, ProfilePath + TEXT("/input_fields"))
				|| !TryBuildSchema(
					Profile->GetArrayField(TEXT("result_fields")),
					Components, ResultSchema, OutError, ProfilePath + TEXT("/result_fields")))
			{
				return false;
			}

			TArray<EUeAgentStatePlane> StatePlanes;
			for (const TSharedPtr<FJsonValue>& Value
				: Profile->GetArrayField(TEXT("state_planes")))
			{
				EUeAgentStatePlane Plane = EUeAgentStatePlane::Invalid;
				if (!TryParseStatePlane(Value->AsString(), Plane))
				{
					OutError = RegistrationFact(TEXT("timeline_animation_state_plane_invalid"),
						TEXT("Descriptor state plane is not recognized."), ProfilePath + TEXT("/state_planes/") + FString::FromInt(StatePlanes.Num()),
						RegistrationValue(Value), RegistrationChoices({TEXT("authoring_memory"), TEXT("persisted"), TEXT("compiled_generated"),
							TEXT("runtime_instance"), TEXT("rendered_observation"), TEXT("registry_index")}));
					return false;
				}
				StatePlanes.Add(Plane);
			}
			TArray<EUeAgentExecutionContextFacet> Facets;
			for (const TSharedPtr<FJsonValue>& Value
				: Profile->GetArrayField(TEXT("required_facets")))
			{
				EUeAgentExecutionContextFacet Facet = EUeAgentExecutionContextFacet::Invalid;
				if (!TryFacet(Value->AsString(), Facet))
				{
					OutError = RegistrationFact(TEXT("timeline_animation_context_facet_invalid"),
						TEXT("Descriptor execution-context facet is not recognized."), ProfilePath + TEXT("/required_facets/") + FString::FromInt(Facets.Num()),
						RegistrationValue(Value), RegistrationChoices({TEXT("world"), TEXT("level"), TEXT("data_layer"), TEXT("world_partition"),
							TEXT("pie_instance"), TEXT("net_mode"), TEXT("net_role"), TEXT("asset_editor"), TEXT("viewport"), TEXT("selection")}));
					return false;
				}
				Facets.Add(Facet);
			}
			const FString OperationKindText =
				Profile->GetStringField(TEXT("operation_kind"));
			const FString Access =
				OperationKindText == TEXT("query")
					? TEXT("read_only")
					: TEXT("temporary_mutation_allowed");
			const FUeAgentExecutionContextPolicy Policy(
				MoveTemp(StatePlanes),
				MoveTemp(Facets),
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				Access == TEXT("read_only")
					? EUeAgentTransientContextPolicy::ReadOnly
					: EUeAgentTransientContextPolicy::TemporaryMutationAllowed);

			const EUeAgentDescriptorOperationKind OperationKind =
				OperationKindText == TEXT("query")
					? EUeAgentDescriptorOperationKind::Query
					: EUeAgentDescriptorOperationKind::Operation;
			TOptional<FUeAgentQueryBinding> QueryBinding;
			if (OperationKind == EUeAgentDescriptorOperationKind::Query)
			{
				EUeAgentQueryKind QueryKind = EUeAgentQueryKind::Invalid;
				if (!TryQueryKind(
						Profile->GetStringField(TEXT("query_kind")),
						QueryKind))
				{
					OutError = RegistrationFact(TEXT("timeline_animation_query_kind_invalid"),
						TEXT("Descriptor query kind is not recognized."), ProfilePath + TEXT("/query_kind"),
						RegistrationValue(Profile->TryGetField(TEXT("query_kind"))),
						RegistrationChoices({TEXT("catalog"), TEXT("state"), TEXT("runtime_probe"), TEXT("capability"), TEXT("reference")}));
					return false;
				}
				QueryBinding.Emplace(
					Profile->GetStringField(TEXT("provider_binding")),
					QueryKind,
					EUeAgentQueryMaterializationPolicy::RuntimeObserve,
					EUeAgentQuerySideEffectPolicy::NoPersistentMutation);
			}

			const TSharedPtr<FJsonObject> DescriptorObject =
				BuildDescriptorObject(
					Profile, InputSchema, ResultSchema, OutError);
			FString CanonicalDescriptor;
			if (!DescriptorObject.IsValid()
				|| !Json::TryCanonicalize(
					DescriptorObject, CanonicalDescriptor, OutError))
			{
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			Descriptors.Emplace(
				FUeAgentDescriptorIdentity(
					Profile->GetStringField(TEXT("descriptor_id")),
					1,
					Json::Sha256Utf8(CanonicalDescriptor)),
				Profile->GetStringField(TEXT("capability_id")),
				TEXT("timeline_animation"),
				OperationKind,
				Policy,
				FUeAgentContractValue(InputSchema),
				FUeAgentContractValue(ResultSchema),
				MoveTemp(QueryBinding));
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors), OutRegistry, OutError);
	}
}
