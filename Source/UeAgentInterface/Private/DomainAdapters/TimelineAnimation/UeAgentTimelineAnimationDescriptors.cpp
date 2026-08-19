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

		TSharedPtr<FJsonValue> ExpandSchemaValue(
			const TSharedPtr<FJsonValue>& Value,
			const TSharedPtr<FJsonObject>& Components,
			TSet<FString>& ExpansionStack,
			FString& OutError)
		{
			if (!Value.IsValid())
			{
				OutError = TEXT("timeline_animation_schema_value_invalid");
				return nullptr;
			}
			switch (Value->Type)
			{
			case EJson::Null:
				return MakeShared<FJsonValueNull>();
			case EJson::String:
				return MakeShared<FJsonValueString>(Value->AsString());
			case EJson::Number:
				return MakeShared<FJsonValueNumber>(Value->AsNumber());
			case EJson::Boolean:
				return MakeShared<FJsonValueBoolean>(Value->AsBool());
			case EJson::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Expanded;
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					TSharedPtr<FJsonValue> ExpandedItem = ExpandSchemaValue(
						Item, Components, ExpansionStack, OutError);
					if (!ExpandedItem.IsValid())
					{
						return nullptr;
					}
					Expanded.Add(MoveTemp(ExpandedItem));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Expanded));
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				if (!Object.IsValid())
				{
					OutError = TEXT("timeline_animation_schema_object_invalid");
					return nullptr;
				}
				FString ComponentName;
				if (Object->TryGetStringField(TEXT("component"), ComponentName))
				{
					if (Object->Values.Num() != 1 || ComponentName.IsEmpty())
					{
						OutError = TEXT("timeline_animation_component_reference_invalid");
						return nullptr;
					}
					const TSharedPtr<FJsonValue>* Definition =
						Components->Values.Find(ComponentName);
					if (!Definition || !Definition->IsValid())
					{
						OutError = TEXT("timeline_animation_component_missing:")
							+ ComponentName;
						return nullptr;
					}
					if (ExpansionStack.Contains(ComponentName))
					{
						OutError = TEXT("timeline_animation_component_cycle:")
							+ ComponentName;
						return nullptr;
					}
					ExpansionStack.Add(ComponentName);
					TSharedPtr<FJsonValue> Expanded = ExpandSchemaValue(
						*Definition, Components, ExpansionStack, OutError);
					ExpansionStack.Remove(ComponentName);
					return Expanded;
				}

				const TSharedPtr<FJsonObject> Expanded = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
					: Object->Values)
				{
					TSharedPtr<FJsonValue> ExpandedField = ExpandSchemaValue(
						Pair.Value, Components, ExpansionStack, OutError);
					if (!ExpandedField.IsValid())
					{
						return nullptr;
					}
					Expanded->SetField(Pair.Key, MoveTemp(ExpandedField));
				}
				return MakeShared<FJsonValueObject>(Expanded);
			}
			default:
				OutError = TEXT("timeline_animation_schema_json_kind_invalid");
				return nullptr;
			}
		}

		bool TryBuildSchema(
			const TArray<TSharedPtr<FJsonValue>>& Fields,
			const TSharedPtr<FJsonObject>& Components,
			FString& OutCanonical,
			FString& OutError)
		{
			const TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Required;
			for (const TSharedPtr<FJsonValue>& FieldValue : Fields)
			{
				const TSharedPtr<FJsonObject> Field =
					FieldValue.IsValid() ? FieldValue->AsObject() : nullptr;
				FString Name;
				bool bRequired = false;
				if (!Field.IsValid()
					|| !Field->TryGetStringField(TEXT("name"), Name)
					|| Name.IsEmpty()
					|| !Field->TryGetBoolField(TEXT("required"), bRequired)
					|| Properties->HasField(Name))
				{
					OutError = TEXT("timeline_animation_schema_field_invalid");
					return false;
				}

				TSharedPtr<FJsonValue> Template;
				FString ComponentName;
				if (Field->TryGetStringField(TEXT("component"), ComponentName))
				{
					const TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
					Reference->SetStringField(TEXT("component"), ComponentName);
					Template = MakeShared<FJsonValueObject>(Reference);
				}
				else
				{
					const TSharedPtr<FJsonObject>* Schema = nullptr;
					if (!Field->TryGetObjectField(TEXT("schema"), Schema)
						|| !Schema || !Schema->IsValid())
					{
						OutError = TEXT("timeline_animation_schema_field_missing");
						return false;
					}
					Template = MakeShared<FJsonValueObject>(*Schema);
				}
				TSet<FString> Stack;
				TSharedPtr<FJsonValue> Expanded = ExpandSchemaValue(
					Template, Components, Stack, OutError);
				if (!Expanded.IsValid())
				{
					return false;
				}
				Properties->SetField(Name, MoveTemp(Expanded));
				if (bRequired)
				{
					Required.Add(MakeShared<FJsonValueString>(Name));
				}
			}

			const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), TEXT("object"));
			Root->SetObjectField(TEXT("properties"), Properties);
			if (!Required.IsEmpty())
			{
				Root->SetArrayField(TEXT("required"), MoveTemp(Required));
			}
			Root->SetBoolField(TEXT("additionalProperties"), false);
			return Json::TryCanonicalize(Root, OutCanonical, OutError);
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
			FString& OutError)
		{
			TSharedPtr<FJsonObject> Object;
			if (!Json::TryParseObject(Canonical, Object, OutError))
			{
				return nullptr;
			}
			return Object;
		}

		TSharedPtr<FJsonObject> BuildDescriptorObject(
			const TSharedPtr<FJsonObject>& Profile,
			const FString& InputSchema,
			const FString& ResultSchema,
			FString& OutError)
		{
			const TSharedPtr<FJsonObject> Input =
				ParseCanonicalObject(InputSchema, OutError);
			const TSharedPtr<FJsonObject> Result =
				ParseCanonicalObject(ResultSchema, OutError);
			if (!Input.IsValid() || !Result.IsValid())
			{
				return nullptr;
			}

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
		FString& OutError)
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
			OutError = TEXT("timeline_animation_descriptor_spec_invalid");
			return false;
		}
		const TSharedPtr<FJsonObject> Components =
			Root->GetObjectField(TEXT("components"));
		const TArray<TSharedPtr<FJsonValue>>& Profiles =
			Root->GetArrayField(TEXT("descriptors"));
		if (!Components.IsValid() || Profiles.Num() != 19)
		{
			OutError = TEXT("timeline_animation_descriptor_spec_incomplete");
			return false;
		}

		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(Profiles.Num());
		for (const TSharedPtr<FJsonValue>& ProfileValue : Profiles)
		{
			const TSharedPtr<FJsonObject> Profile =
				ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
			if (!Profile.IsValid())
			{
				OutError = TEXT("timeline_animation_descriptor_profile_invalid");
				return false;
			}

			FString InputSchema;
			FString ResultSchema;
			if (!TryBuildSchema(
					Profile->GetArrayField(TEXT("input_fields")),
					Components, InputSchema, OutError)
				|| !TryBuildSchema(
					Profile->GetArrayField(TEXT("result_fields")),
					Components, ResultSchema, OutError))
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
					OutError = TEXT("timeline_animation_state_plane_invalid");
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
					OutError = TEXT("timeline_animation_context_facet_invalid");
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
					OutError = TEXT("timeline_animation_query_kind_invalid");
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

