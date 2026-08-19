#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectDescriptorCatalog.generated.inl"

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		struct FDescriptorSpec
		{
			TSharedPtr<FJsonObject> Row;
			FString DescriptorId;
			FString DescriptorHash;
			FString CapabilityId;
			FString NativeTargetKind;
			FString OperationKey;
			FString CanonicalInputSchema;
			bool bPrimitiveOutput = false;
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

		TSharedPtr<FJsonObject> ObjectSchema(
			const TSharedPtr<FJsonObject>& Properties,
			const TArray<FString>& Required)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> RequiredValues;
			for (const FString& Field : Required)
			{
				RequiredValues.Add(
					MakeShared<FJsonValueString>(Field));
			}
			Schema->SetArrayField(
				TEXT("required"),
				MoveTemp(RequiredValues));
			Schema->SetBoolField(TEXT("additionalProperties"), false);
			return Schema;
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
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FString& Value : Values)
			{
				Items.Add(MakeShared<FJsonValueString>(Value));
			}
			Schema->SetArrayField(TEXT("enum"), MoveTemp(Items));
			return Schema;
		}

		TSharedPtr<FJsonObject> BooleanSchema()
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("boolean"));
			return Schema;
		}

		TSharedPtr<FJsonObject> NumericSchema(
			const bool bInteger,
			const TSharedPtr<FJsonObject>& Spec)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(
				TEXT("type"),
				bInteger ? TEXT("integer") : TEXT("number"));
			double Value = 0.0;
			if (Spec->TryGetNumberField(TEXT("minimum"), Value))
			{
				Schema->SetNumberField(TEXT("minimum"), Value);
			}
			if (Spec->TryGetNumberField(TEXT("maximum"), Value))
			{
				Schema->SetNumberField(TEXT("maximum"), Value);
			}
			return Schema;
		}

		TSharedPtr<FJsonObject> PositiveNumberSchema()
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("number"));
			Schema->SetNumberField(TEXT("exclusiveMinimum"), 0);
			return Schema;
		}

		TSharedPtr<FJsonObject> ArraySchema(
			const TSharedPtr<FJsonObject>& ItemSchema,
			const int32 Count,
			const bool bUnique = false)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("array"));
			Schema->SetObjectField(TEXT("items"), ItemSchema);
			Schema->SetNumberField(TEXT("minItems"), Count);
			Schema->SetNumberField(TEXT("maxItems"), Count);
			if (bUnique)
			{
				Schema->SetBoolField(TEXT("uniqueItems"), true);
			}
			return Schema;
		}

		TSharedPtr<FJsonObject> NumberArraySchema(
			const int32 Count,
			const bool bPositive)
		{
			return ArraySchema(
				bPositive
					? PositiveNumberSchema()
					: NumericSchema(
						false,
						MakeShared<FJsonObject>()),
				Count);
		}

		TSharedPtr<FJsonObject> PlacementSchema()
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("semantic_frame_id"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("write_frame_id"),
				ConstStringSchema(TEXT("ue.world")));
			Properties->SetObjectField(
				TEXT("read_frame_id"),
				ConstStringSchema(TEXT("ue.world")));
			Properties->SetObjectField(
				TEXT("verify_frame_id"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("unit"),
				EnumSchema({TEXT("centimeter"), TEXT("meter")}));
			Properties->SetObjectField(
				TEXT("translation"),
				NumberArraySchema(3, false));
			Properties->SetObjectField(
				TEXT("rotation_quaternion_xyzw"),
				NumberArraySchema(4, false));
			Properties->SetObjectField(
				TEXT("scale"),
				NumberArraySchema(3, true));
			Properties->SetObjectField(
				TEXT("basis_evidence"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("conversion_chain"),
				StringSchema());
			return ObjectSchema(
				Properties,
				{
					TEXT("semantic_frame_id"),
					TEXT("write_frame_id"),
					TEXT("read_frame_id"),
					TEXT("verify_frame_id"),
					TEXT("unit"),
					TEXT("translation"),
					TEXT("rotation_quaternion_xyzw"),
					TEXT("scale"),
					TEXT("basis_evidence"),
					TEXT("conversion_chain"),
				});
		}

		TSharedPtr<FJsonObject> MeshSpaceSchema()
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("semantic_frame_id"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("write_frame_id"),
				ConstStringSchema(TEXT("ue.mesh.target_local")));
			Properties->SetObjectField(
				TEXT("read_frame_id"),
				ConstStringSchema(TEXT("ue.mesh.target_local")));
			Properties->SetObjectField(
				TEXT("verify_frame_id"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("unit"),
				EnumSchema({TEXT("centimeter"), TEXT("meter")}));
			Properties->SetObjectField(
				TEXT("basis_evidence"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("conversion_chain"),
				StringSchema());
			return ObjectSchema(
				Properties,
				{
					TEXT("semantic_frame_id"),
					TEXT("write_frame_id"),
					TEXT("read_frame_id"),
					TEXT("verify_frame_id"),
					TEXT("unit"),
					TEXT("basis_evidence"),
					TEXT("conversion_chain"),
				});
		}

		TSharedPtr<FJsonObject> TransformSchema()
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("from_frame_id"),
				ConstStringSchema(TEXT("ue.mesh.operand_local")));
			Properties->SetObjectField(
				TEXT("to_frame_id"),
				ConstStringSchema(TEXT("ue.mesh.target_local")));
			Properties->SetObjectField(
				TEXT("unit"),
				EnumSchema({TEXT("centimeter"), TEXT("meter")}));
			Properties->SetObjectField(
				TEXT("translation"),
				NumberArraySchema(3, false));
			Properties->SetObjectField(
				TEXT("rotation_quaternion_xyzw"),
				NumberArraySchema(4, false));
			Properties->SetObjectField(
				TEXT("scale"),
				NumberArraySchema(3, true));
			Properties->SetObjectField(
				TEXT("basis_evidence"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("conversion_chain"),
				StringSchema());
			return ObjectSchema(
				Properties,
				{
					TEXT("from_frame_id"),
					TEXT("to_frame_id"),
					TEXT("unit"),
					TEXT("translation"),
					TEXT("rotation_quaternion_xyzw"),
					TEXT("scale"),
					TEXT("basis_evidence"),
					TEXT("conversion_chain"),
				});
		}

		TSharedPtr<FJsonObject> SelectionSchema(
			const FString& ElementKind)
		{
			const TSharedPtr<FJsonObject> IdSchema =
				MakeShared<FJsonObject>();
			IdSchema->SetStringField(TEXT("type"), TEXT("integer"));
			IdSchema->SetNumberField(TEXT("minimum"), 0);
			const TSharedPtr<FJsonObject> Ids =
				MakeShared<FJsonObject>();
			Ids->SetStringField(TEXT("type"), TEXT("array"));
			Ids->SetObjectField(TEXT("items"), IdSchema);
			Ids->SetNumberField(TEXT("minItems"), 1);
			Ids->SetBoolField(TEXT("uniqueItems"), true);
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("element_kind"),
				ConstStringSchema(ElementKind));
			Properties->SetObjectField(TEXT("element_ids"), Ids);
			Properties->SetObjectField(
				TEXT("mesh_revision"),
				StringSchema());
			return ObjectSchema(
				Properties,
				{
					TEXT("element_kind"),
					TEXT("element_ids"),
					TEXT("mesh_revision"),
				});
		}

		TSharedPtr<FJsonObject> PlaneSchema()
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("frame_id"),
				ConstStringSchema(TEXT("ue.mesh.target_local")));
			Properties->SetObjectField(
				TEXT("unit"),
				EnumSchema({TEXT("centimeter"), TEXT("meter")}));
			Properties->SetObjectField(
				TEXT("origin"),
				NumberArraySchema(3, false));
			Properties->SetObjectField(
				TEXT("normal"),
				NumberArraySchema(3, false));
			Properties->SetObjectField(
				TEXT("basis_evidence"),
				StringSchema());
			return ObjectSchema(
				Properties,
				{
					TEXT("frame_id"),
					TEXT("unit"),
					TEXT("origin"),
					TEXT("normal"),
					TEXT("basis_evidence"),
				});
		}

		TSharedPtr<FJsonObject> DirectionSchema()
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("frame_id"),
				ConstStringSchema(TEXT("ue.mesh.target_local")));
			Properties->SetObjectField(
				TEXT("vector"),
				NumberArraySchema(3, false));
			Properties->SetObjectField(
				TEXT("basis_evidence"),
				StringSchema());
			return ObjectSchema(
				Properties,
				{
					TEXT("frame_id"),
					TEXT("vector"),
					TEXT("basis_evidence"),
				});
		}

		TSharedPtr<FJsonObject> ProjectionFrameSchema()
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("frame_id"),
				ConstStringSchema(TEXT("ue.mesh.target_local")));
			Properties->SetObjectField(
				TEXT("unit"),
				EnumSchema({TEXT("centimeter"), TEXT("meter")}));
			for (const TCHAR* Field
				: {
					TEXT("origin"),
					TEXT("axis_u"),
					TEXT("axis_v"),
					TEXT("axis_w"),
				})
			{
				Properties->SetObjectField(
					Field,
					NumberArraySchema(3, false));
			}
			Properties->SetObjectField(
				TEXT("basis_evidence"),
				StringSchema());
			return ObjectSchema(
				Properties,
				{
					TEXT("frame_id"),
					TEXT("unit"),
					TEXT("origin"),
					TEXT("axis_u"),
					TEXT("axis_v"),
					TEXT("axis_w"),
					TEXT("basis_evidence"),
				});
		}

		TSharedPtr<FJsonObject> EdgePairsSchema()
		{
			const TSharedPtr<FJsonObject> EdgeId =
				MakeShared<FJsonObject>();
			EdgeId->SetStringField(TEXT("type"), TEXT("integer"));
			EdgeId->SetNumberField(TEXT("minimum"), 0);
			const TSharedPtr<FJsonObject> Pair =
				ArraySchema(EdgeId, 2, true);
			const TSharedPtr<FJsonObject> Pairs =
				MakeShared<FJsonObject>();
			Pairs->SetStringField(TEXT("type"), TEXT("array"));
			Pairs->SetObjectField(TEXT("items"), Pair);
			Pairs->SetNumberField(TEXT("minItems"), 1);
			Pairs->SetBoolField(TEXT("uniqueItems"), true);
			return Pairs;
		}

		TSharedPtr<FJsonObject> ParameterSchema(
			const TSharedPtr<FJsonObject>& Spec)
		{
			const FString Kind =
				Spec->GetStringField(TEXT("kind"));
			if (Kind == TEXT("string"))
			{
				return StringSchema();
			}
			if (Kind == TEXT("boolean"))
			{
				return BooleanSchema();
			}
			if (Kind == TEXT("positive_number"))
			{
				return PositiveNumberSchema();
			}
			if (Kind == TEXT("number"))
			{
				return NumericSchema(false, Spec);
			}
			if (Kind == TEXT("integer"))
			{
				return NumericSchema(true, Spec);
			}
			if (Kind == TEXT("enum"))
			{
				TArray<FString> Values;
				for (const TSharedPtr<FJsonValue>& Value
					: Spec->GetArrayField(TEXT("values")))
				{
					Values.Add(Value->AsString());
				}
				return EnumSchema(Values);
			}
			const int32 Count = static_cast<int32>(
				Spec->GetNumberField(TEXT("count")));
			if (Kind == TEXT("positive_number_array"))
			{
				return NumberArraySchema(Count, true);
			}
			if (Kind == TEXT("number_array"))
			{
				return NumberArraySchema(Count, false);
			}
			if (Kind == TEXT("integer_array"))
			{
				return ArraySchema(
					NumericSchema(true, Spec),
					Count);
			}
			return nullptr;
		}

		TSharedPtr<FJsonObject> BuildInputSchema(
			const TSharedPtr<FJsonObject>& Row)
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			TArray<FString> Required;
			auto AddRequired =
				[&Properties, &Required](
					const TCHAR* Name,
					const TSharedPtr<FJsonObject>& Schema)
				{
					Properties->SetObjectField(Name, Schema);
					Required.Add(Name);
				};

			bool bValue = false;
			if (Row->TryGetBoolField(
					TEXT("primitive_output"),
					bValue)
				&& bValue)
			{
				AddRequired(TEXT("target_ref"), StringSchema());
			}
			if (Row->TryGetBoolField(
					TEXT("existing_target"),
					bValue)
				&& bValue)
			{
				AddRequired(TEXT("target_ref"), StringSchema());
				AddRequired(TEXT("target_revision"), StringSchema());
			}
			if (Row->TryGetBoolField(TEXT("placement"), bValue)
				&& bValue)
			{
				AddRequired(TEXT("placement"), PlacementSchema());
			}
			if (Row->TryGetBoolField(
					TEXT("space_contract"),
					bValue)
				&& bValue)
			{
				AddRequired(
					TEXT("space_contract"),
					MeshSpaceSchema());
			}
			FString SelectionKind;
			if (Row->TryGetStringField(
					TEXT("selection_kind"),
					SelectionKind))
			{
				AddRequired(
					TEXT("selection"),
					SelectionSchema(SelectionKind));
			}
			if (Row->TryGetBoolField(TEXT("operand"), bValue)
				&& bValue)
			{
				AddRequired(TEXT("operand_ref"), StringSchema());
				AddRequired(TEXT("operand_revision"), StringSchema());
				AddRequired(
					TEXT("operand_to_target"),
					TransformSchema());
			}
			if (Row->TryGetBoolField(TEXT("plane"), bValue)
				&& bValue)
			{
				AddRequired(TEXT("plane"), PlaneSchema());
			}
			if (Row->TryGetBoolField(TEXT("direction"), bValue)
				&& bValue)
			{
				Properties->SetObjectField(
					TEXT("direction"),
					DirectionSchema());
			}
			if (Row->TryGetBoolField(
					TEXT("projection_frame"),
					bValue)
				&& bValue)
			{
				AddRequired(
					TEXT("projection_frame"),
					ProjectionFrameSchema());
			}
			if (Row->TryGetBoolField(TEXT("edge_pairs"), bValue)
				&& bValue)
			{
				AddRequired(TEXT("edge_pairs"), EdgePairsSchema());
			}

			for (const TSharedPtr<FJsonValue>& Value
				: Row->GetArrayField(TEXT("parameters")))
			{
				const TSharedPtr<FJsonObject> Parameter =
					Value->AsObject();
				const FString Name =
					Parameter->GetStringField(TEXT("name"));
				const TSharedPtr<FJsonObject> Schema =
					ParameterSchema(Parameter);
				if (!Schema.IsValid())
				{
					return nullptr;
				}
				Properties->SetObjectField(Name, Schema);
				bool bRequired = true;
				if (!Parameter->TryGetBoolField(
						TEXT("required"),
						bRequired)
					|| bRequired)
				{
					Required.Add(Name);
				}
			}
			return ObjectSchema(Properties, Required);
		}

		TSharedPtr<FJsonObject> BuildResultSchema()
		{
			const TSharedPtr<FJsonObject> RevisionProperties =
				MakeShared<FJsonObject>();
			RevisionProperties->SetObjectField(
				TEXT("target_ref"),
				StringSchema());
			RevisionProperties->SetObjectField(
				TEXT("revision"),
				StringSchema());
			const TSharedPtr<FJsonObject> RevisionSchema =
				ObjectSchema(
					RevisionProperties,
					{TEXT("target_ref"), TEXT("revision")});
			const TSharedPtr<FJsonObject> Revisions =
				MakeShared<FJsonObject>();
			Revisions->SetStringField(TEXT("type"), TEXT("array"));
			Revisions->SetObjectField(TEXT("items"), RevisionSchema);
			Revisions->SetBoolField(TEXT("uniqueItems"), true);
			const TSharedPtr<FJsonObject> UniqueStrings =
				MakeShared<FJsonObject>();
			UniqueStrings->SetStringField(TEXT("type"), TEXT("array"));
			UniqueStrings->SetObjectField(
				TEXT("items"),
				StringSchema());
			UniqueStrings->SetBoolField(TEXT("uniqueItems"), true);
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
				TEXT("target_refs"),
				UniqueStrings);
			Properties->SetObjectField(
				TEXT("geometry_revisions"),
				Revisions);
			Properties->SetObjectField(
				TEXT("evidence_refs"),
				UniqueStrings);
			return ObjectSchema(
				Properties,
				{
					TEXT("operation_id"),
					TEXT("outcome"),
					TEXT("target_refs"),
					TEXT("geometry_revisions"),
				});
		}

		bool TryCanonicalize(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutCanonical,
			FString& OutError)
		{
			OutCanonical.Reset();
			if (!Object.IsValid()
				|| !Json::TryCanonicalize(
					Object,
					OutCanonical,
					OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError =
						TEXT("modeling_geometry_schema_invalid");
				}
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> ExecutionPolicyObject()
		{
			const TSharedPtr<FJsonObject> Policy =
				MakeShared<FJsonObject>();
			Policy->SetArrayField(
				TEXT("allowed_state_planes"),
				{MakeShared<FJsonValueString>(
					TEXT("authoring_memory"))});
			Policy->SetArrayField(
				TEXT("required_facets"),
				{
					MakeShared<FJsonValueString>(TEXT("level")),
					MakeShared<FJsonValueString>(TEXT("world")),
				});
			Policy->SetStringField(
				TEXT("resolution_policy"),
				TEXT("explicit_only"));
			Policy->SetStringField(
				TEXT("transient_access_policy"),
				TEXT("temporary_mutation_allowed"));
			return Policy;
		}

		bool TryLoadSpecs(
			TArray<FDescriptorSpec>& OutSpecs,
			FString& OutCanonicalResult,
			FString& OutError)
		{
			OutSpecs.Reset();
			OutCanonicalResult.Reset();
			OutError.Reset();
			TSharedPtr<FJsonObject> Catalog;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(
					Private::DescriptorCatalogJson);
			if (!FJsonSerializer::Deserialize(Reader, Catalog)
				|| !Catalog.IsValid()
				|| Catalog->GetStringField(TEXT("schema"))
					!= TEXT(
						"uai.domain_adapter.modeling_geometry_effect."
						"production_catalog.v1"))
			{
				OutError =
					TEXT("modeling_geometry_catalog_parse_failed");
				return false;
			}
			const TSharedPtr<FJsonObject> ResultSchema =
				BuildResultSchema();
			if (!TryCanonicalize(
					ResultSchema,
					OutCanonicalResult,
					OutError))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!Catalog->TryGetArrayField(TEXT("descriptors"), Rows)
				|| !Rows
				|| Rows->Num() != 31)
			{
				OutError =
					TEXT("modeling_geometry_descriptor_count_invalid");
				return false;
			}
			OutSpecs.Reserve(Rows->Num());
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject> Row =
					Value.IsValid() ? Value->AsObject() : nullptr;
				const TSharedPtr<FJsonObject> InputSchema =
					BuildInputSchema(Row);
				FDescriptorSpec Spec;
				if (!Row.IsValid()
					|| !InputSchema.IsValid()
					|| !Row->TryGetStringField(
						TEXT("descriptor_id"),
						Spec.DescriptorId)
					|| !Row->TryGetStringField(
						TEXT("capability_id"),
						Spec.CapabilityId)
					|| !Row->TryGetStringField(
						TEXT("native_operation_type"),
						Spec.NativeTargetKind)
					|| !Row->TryGetStringField(
						TEXT("operation_key"),
						Spec.OperationKey)
					|| !TryCanonicalize(
						InputSchema,
						Spec.CanonicalInputSchema,
						OutError))
				{
					OutError =
						TEXT("modeling_geometry_descriptor_invalid");
					return false;
				}
				Row->TryGetBoolField(
					TEXT("primitive_output"),
					Spec.bPrimitiveOutput);
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
					TEXT("mesh"));
				Descriptor->SetObjectField(
					TEXT("execution_context_policy"),
					ExecutionPolicyObject());
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
				FString CanonicalDescriptor;
				if (!TryCanonicalize(
						Descriptor,
						CanonicalDescriptor,
						OutError))
				{
					return false;
				}
				Spec.DescriptorHash =
					Json::Sha256Utf8(CanonicalDescriptor);
				Spec.Row = Row;
				OutSpecs.Add(MoveTemp(Spec));
			}
			return true;
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_internal_value_missing"));
			}
			FUeAgentStateSemanticError Error;
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
				if (FUeAgentCanonicalValue::TryDecimal(
						FString::SanitizeFloat(
							JsonValue->AsNumber()),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: JsonValue->AsArray())
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalValue(
								Item,
								Resources,
								Canonical,
								OutError))
						{
							return false;
						}
						Items.Add(Canonical.ToSharedRef());
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
						TPair<FString, FUeAgentCanonicalValueRef>>
						Items;
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Pair
							: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalValue(
								Pair.Value,
								Resources,
								Canonical,
								OutError))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							Canonical.ToSharedRef());
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
					? TEXT("modeling_geometry_internal_value_failed")
					: Error.Code,
				Error.Detail);
		}

		FUeAgentCanonicalValuePtr FindCanonicalField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
					: Object->GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return FUeAgentCanonicalValuePtr(Item.Value);
				}
			}
			return nullptr;
		}

		bool ReadCanonicalString(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			FString& OutValue)
		{
			const FUeAgentCanonicalValuePtr Value =
				FindCanonicalField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty();
		}

		bool ReadCanonicalIntegerSet(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			TSet<int32>& OutValues)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindCanonicalField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::Integer
					&& Item->GetKind() != EUeAgentValueKind::Decimal)
				{
					return false;
				}
				OutValues.Add(FCString::Atoi(*Item->GetLexeme()));
			}
			return true;
		}

		const FUeAgentStateEntry* FindSnapshotTarget(
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			const FString& TargetRef)
		{
			const FUeAgentStateEntry* Match = nullptr;
			for (const FUeAgentStateEntry& Entry
				: Snapshot->GetEntries())
			{
				FString CandidateRef;
				if (!ReadCanonicalString(
						FUeAgentCanonicalValuePtr(
							Entry.GetValue()),
						TEXT("target_ref"),
						CandidateRef)
					|| CandidateRef != TargetRef)
				{
					continue;
				}
				if (Match)
				{
					return nullptr;
				}
				Match = &Entry;
			}
			return Match;
		}

		bool ReadJsonNumberArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const int32 Count,
			TArray<double>& OutValues)
		{
			OutValues.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Items)
				|| !Items
				|| Items->Num() != Count)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				double Number = 0.0;
				if (!Item.IsValid()
					|| !Item->TryGetNumber(Number)
					|| !FMath::IsFinite(Number))
				{
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
		}

		bool ValidateSelection(
			const TSharedPtr<FJsonObject>& Payload,
			const FString& SelectionKind,
			const FUeAgentCanonicalValuePtr& Current,
			FExecutionMutationError& OutError)
		{
			const TSharedPtr<FJsonObject>* Selection = nullptr;
			if (!Payload->TryGetObjectField(
					TEXT("selection"),
					Selection)
				|| !Selection
				|| !Selection->IsValid())
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_selection_element_invalid"));
			}
			FString TargetRevision;
			FString SelectionRevision;
			if (!ReadCanonicalString(
					Current,
					TEXT("target_revision"),
					TargetRevision)
				|| !(*Selection)->TryGetStringField(
					TEXT("mesh_revision"),
					SelectionRevision)
				|| SelectionRevision != TargetRevision)
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_selection_revision_mismatch"));
			}
			const TCHAR* SnapshotField =
				SelectionKind == TEXT("vertex")
					? TEXT("vertex_ids")
					: SelectionKind == TEXT("triangle")
						? TEXT("triangle_ids")
						: SelectionKind == TEXT("boundary_edge")
							? TEXT("boundary_edge_ids")
							: TEXT("edge_ids");
			TSet<int32> Allowed;
			if (!ReadCanonicalIntegerSet(
					Current,
					SnapshotField,
					Allowed))
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_selection_element_invalid"));
			}
			const TArray<TSharedPtr<FJsonValue>>* ElementIds =
				nullptr;
			if (!(*Selection)->TryGetArrayField(
					TEXT("element_ids"),
					ElementIds)
				|| !ElementIds)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_selection_element_invalid"));
			}
			for (const TSharedPtr<FJsonValue>& Item : *ElementIds)
			{
				double Number = -1.0;
				if (!Item.IsValid()
					|| !Item->TryGetNumber(Number)
					|| !Allowed.Contains(
						static_cast<int32>(Number)))
				{
					return Fail(
						OutError,
						SelectionKind == TEXT("boundary_edge")
							? TEXT(
								"modeling_geometry_boundary_edge_invalid")
							: TEXT(
								"modeling_geometry_selection_element_invalid"));
				}
			}
			return true;
		}

		bool ValidateOperandTransform(
			const TSharedPtr<FJsonObject>& Payload,
			FExecutionMutationError& OutError)
		{
			const TSharedPtr<FJsonObject>* Transform = nullptr;
			TArray<double> Rotation;
			TArray<double> Scale;
			if (!Payload->TryGetObjectField(
					TEXT("operand_to_target"),
					Transform)
				|| !Transform
				|| !ReadJsonNumberArray(
					*Transform,
					TEXT("rotation_quaternion_xyzw"),
					4,
					Rotation)
				|| !ReadJsonNumberArray(
					*Transform,
					TEXT("scale"),
					3,
					Scale))
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_operand_transform_invalid"));
			}
			double QuaternionNormSquared = 0.0;
			for (const double Component : Rotation)
			{
				QuaternionNormSquared += Component * Component;
			}
			if (QuaternionNormSquared <= UE_DOUBLE_SMALL_NUMBER
				|| Scale.ContainsByPredicate(
					[](const double Value)
					{
						return FMath::Abs(Value)
							<= UE_DOUBLE_SMALL_NUMBER;
					}))
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_operand_transform_invalid"));
			}
			return true;
		}

		bool ValidatePlane(
			const TSharedPtr<FJsonObject>& Payload,
			FExecutionMutationError& OutError)
		{
			const TSharedPtr<FJsonObject>* Plane = nullptr;
			TArray<double> Normal;
			if (!Payload->TryGetObjectField(TEXT("plane"), Plane)
				|| !Plane
				|| !ReadJsonNumberArray(
					*Plane,
					TEXT("normal"),
					3,
					Normal)
				|| FVector3d(
						Normal[0],
						Normal[1],
						Normal[2])
					.IsNearlyZero())
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_plane_invalid"));
			}
			return true;
		}

		bool ValidateEdgePairs(
			const TSharedPtr<FJsonObject>& Payload,
			FExecutionMutationError& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* Pairs = nullptr;
			if (!Payload->TryGetArrayField(
					TEXT("edge_pairs"),
					Pairs)
				|| !Pairs)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_edge_pair_invalid"));
			}
			TSet<FString> CanonicalPairs;
			for (const TSharedPtr<FJsonValue>& PairValue : *Pairs)
			{
				const TArray<TSharedPtr<FJsonValue>>& Pair =
					PairValue->AsArray();
				if (Pair.Num() != 2)
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_edge_pair_invalid"));
				}
				const int32 A =
					static_cast<int32>(Pair[0]->AsNumber());
				const int32 B =
					static_cast<int32>(Pair[1]->AsNumber());
				const FString Key = FString::Printf(
					TEXT("%d:%d"),
					FMath::Min(A, B),
					FMath::Max(A, B));
				if (A == B || CanonicalPairs.Contains(Key))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_edge_pair_invalid"));
				}
				CanonicalPairs.Add(Key);
			}
			return true;
		}

		bool AddDraftChange(
			const FDescriptorSpec& Spec,
			const FString& TargetRef,
			const FUeAgentCanonicalValuePtr& ExpectedBefore,
			const TSharedPtr<FJsonObject>& InternalAfter,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FChangeDraft>& OutChanges,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr After;
			if (!TryCanonicalValue(
					MakeShared<FJsonValueObject>(InternalAfter),
					Resources,
					After,
					OutError))
			{
				return false;
			}
			const FUeAgentCanonicalIdentity Identity(
				TEXT("uai.modeling.geometry:") + TargetRef,
				Spec.NativeTargetKind,
				Context.GetWorldIdentity(),
				TargetRef,
				TEXT("modeling_geometry"),
				Context.GetContextRevision());
			OutChanges.Emplace(
				Identity,
				Spec.bPrimitiveOutput
					? EChangeIntent::Create
					: EChangeIntent::Update,
				ExpectedBefore,
				After,
				GetModelingGeometryEffectAuthorityScopes(),
				TEXT("modeling-geometry:canonical-typed-draft"),
				TArray<FUeAgentCanonicalIdentity>{});
			return OutChanges.Last().IsValid()
				|| Fail(
					OutError,
					TEXT("modeling_geometry_change_draft_invalid"),
					TargetRef);
		}
	}

	TArray<FString> GetModelingGeometryEffectAuthorityScopes()
	{
		return {TEXT("uai:modeling-geometry:write")};
	}

	bool TryCreateModelingGeometryEffectDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		TArray<FDescriptorSpec> Specs;
		FString CanonicalResult;
		if (!TryLoadSpecs(Specs, CanonicalResult, OutError))
		{
			return false;
		}
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(Specs.Num());
		for (const FDescriptorSpec& Spec : Specs)
		{
			Descriptors.Emplace(
				FUeAgentDescriptorIdentity(
					Spec.DescriptorId,
					1,
					Spec.DescriptorHash),
				Spec.CapabilityId,
				TEXT("mesh"),
				EUeAgentDescriptorOperationKind::Operation,
				FUeAgentExecutionContextPolicy(
					{EUeAgentStatePlane::AuthoringMemory},
					{
						EUeAgentExecutionContextFacet::Level,
						EUeAgentExecutionContextFacet::World,
					},
					EUeAgentContextResolutionPolicy::ExplicitOnly,
					EUeAgentTransientContextPolicy::
						TemporaryMutationAllowed),
				FUeAgentContractValue(Spec.CanonicalInputSchema),
				FUeAgentContractValue(CanonicalResult));
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	bool TryGetModelingGeometryEffectProductionProfiles(
		TArray<FUeAgentModelingGeometryEffectProductionProfile>&
			OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		TArray<FDescriptorSpec> Specs;
		FString CanonicalResult;
		if (!TryLoadSpecs(Specs, CanonicalResult, OutError))
		{
			return false;
		}
		OutProfiles.Reserve(Specs.Num());
		for (const FDescriptorSpec& Spec : Specs)
		{
			FUeAgentModelingGeometryEffectProductionProfile Profile;
			Profile.DescriptorId = Spec.DescriptorId;
			Profile.DescriptorVersion = 1;
			Profile.DescriptorHash = Spec.DescriptorHash;
			Profile.CapabilityId = Spec.CapabilityId;
			Profile.NativeTargetKind = Spec.NativeTargetKind;
			Profile.OperationKey = Spec.OperationKey;
			Profile.bPrimitiveOutput = Spec.bPrimitiveOutput;
			if (!Profile.IsValid())
			{
				OutProfiles.Reset();
				OutError =
					TEXT("modeling_geometry_production_profile_invalid");
				return false;
			}
			OutProfiles.Add(MoveTemp(Profile));
		}
		return OutProfiles.Num() == 31;
	}

	bool TryBuildModelingGeometryEffectChangeSetDraft(
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
			|| Context.GetWorldIdentity().IsEmpty()
			|| Context.GetLevelIdentity().IsEmpty()
			|| Context.GetContextRevision().IsEmpty()
			|| CurrentSnapshot->GetStatePlane()
				!= EUeAgentStatePlane::AuthoringMemory)
		{
			return Fail(
				OutError,
				TEXT("modeling_geometry_draft_request_invalid"));
		}

		TArray<FDescriptorSpec> Specs;
		FString CanonicalResult;
		FString CatalogError;
		if (!TryLoadSpecs(Specs, CanonicalResult, CatalogError))
		{
			return Fail(
				OutError,
				TEXT("modeling_geometry_catalog_invalid"),
				CatalogError);
		}
		const FDescriptorSpec* Spec = nullptr;
		for (const FDescriptorSpec& Candidate : Specs)
		{
			if (Request.GetDescriptorIdentity().GetDescriptorId()
					== Candidate.DescriptorId
				&& Request.GetDescriptorIdentity().GetVersion() == 1
				&& Request.GetDescriptorIdentity()
						.GetDescriptorHash()
					== Candidate.DescriptorHash)
			{
				Spec = &Candidate;
				break;
			}
		}
		if (!Spec)
		{
			return Fail(
				OutError,
				TEXT("modeling_geometry_descriptor_not_exact"));
		}

		TSharedPtr<FJsonObject> Payload;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(
				Request.GetPayload().GetJsonValue());
		if (!FJsonSerializer::Deserialize(Reader, Payload)
			|| !Payload.IsValid())
		{
			return Fail(
				OutError,
				TEXT("modeling_geometry_payload_parse_failed"));
		}
		FString TargetRef;
		if (!Payload->TryGetStringField(
				TEXT("target_ref"),
				TargetRef)
			|| TargetRef.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("modeling_geometry_reference_unresolved"));
		}

		const FUeAgentStateEntry* Target =
			FindSnapshotTarget(CurrentSnapshot, TargetRef);
		FUeAgentCanonicalValuePtr Current;
		FString CarrierKind = TEXT("dynamic_mesh_actor");
		FString Fingerprint = TEXT("absent");
		if (Spec->bPrimitiveOutput)
		{
			if (Target)
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_output_already_exists"));
			}
		}
		else
		{
			if (!Target)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_reference_unresolved"));
			}
			Current = FUeAgentCanonicalValuePtr(Target->GetValue());
			FString ExpectedRevision;
			FString ActualRevision;
			Payload->TryGetStringField(
				TEXT("target_revision"),
				ExpectedRevision);
			if (!ReadCanonicalString(
					Current,
					TEXT("target_revision"),
					ActualRevision)
				|| ExpectedRevision != ActualRevision)
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_target_revision_stale"));
			}
			if (!ReadCanonicalString(
					Current,
					TEXT("carrier_kind"),
					CarrierKind)
				|| (CarrierKind != TEXT("dynamic_mesh_actor")
					&& CarrierKind
						!= TEXT("static_mesh_asset")))
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_carrier_unsupported"));
			}
			if (!ReadCanonicalString(
					Current,
					TEXT("geometry_fingerprint"),
					Fingerprint))
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_target_revision_stale"));
			}
		}

		FString SelectionKind;
		if (Spec->Row->TryGetStringField(
				TEXT("selection_kind"),
				SelectionKind)
			&& !ValidateSelection(
				Payload,
				SelectionKind,
				Current,
				OutError))
		{
			return false;
		}

		bool bOperand = false;
		FString OperandRef;
		FString OperandFingerprint;
		if (Spec->Row->TryGetBoolField(
				TEXT("operand"),
				bOperand)
			&& bOperand)
		{
			FString OperandRevision;
			Payload->TryGetStringField(
				TEXT("operand_ref"),
				OperandRef);
			Payload->TryGetStringField(
				TEXT("operand_revision"),
				OperandRevision);
			const FUeAgentStateEntry* Operand =
				FindSnapshotTarget(CurrentSnapshot, OperandRef);
			if (!Operand)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_operand_unresolved"));
			}
			const FUeAgentCanonicalValuePtr OperandCurrent(
				Operand->GetValue());
			FString ActualRevision;
			if (!ReadCanonicalString(
					OperandCurrent,
					TEXT("target_revision"),
					ActualRevision)
				|| ActualRevision != OperandRevision)
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_operand_revision_stale"));
			}
			if (!ReadCanonicalString(
					OperandCurrent,
					TEXT("geometry_fingerprint"),
					OperandFingerprint)
				|| !ValidateOperandTransform(Payload, OutError))
			{
				return false;
			}
		}

		bool bPlane = false;
		if (Spec->Row->TryGetBoolField(TEXT("plane"), bPlane)
			&& bPlane
			&& !ValidatePlane(Payload, OutError))
		{
			return false;
		}
		bool bDirection = false;
		FString DirectionMode;
		if (Spec->Row->TryGetBoolField(
				TEXT("direction"),
				bDirection)
			&& bDirection
			&& Payload->TryGetStringField(
				TEXT("direction_mode"),
				DirectionMode)
			&& DirectionMode == TEXT("fixed")
			&& !Payload->HasField(TEXT("direction")))
		{
			return Fail(
				OutError,
				TEXT("modeling_geometry_direction_required"));
		}
		if (Spec->OperationKey == TEXT("simplify"))
		{
			FString TargetMode;
			double TargetValue = 0.0;
			if (Payload->TryGetStringField(
					TEXT("target_mode"),
					TargetMode)
				&& TargetMode == TEXT("triangle_ratio")
				&& Payload->TryGetNumberField(
					TEXT("target_value"),
					TargetValue)
				&& TargetValue > 1.0)
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_simplify_target_invalid"));
			}
		}
		bool bEdgePairs = false;
		if (Spec->Row->TryGetBoolField(
				TEXT("edge_pairs"),
				bEdgePairs)
			&& bEdgePairs
			&& !ValidateEdgePairs(Payload, OutError))
		{
			return false;
		}

		const TSharedPtr<FJsonObject> TypedParameters =
			MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
			: Payload->Values)
		{
			if (Pair.Key != TEXT("target_ref")
				&& Pair.Key != TEXT("target_revision")
				&& Pair.Key != TEXT("operand_ref")
				&& Pair.Key != TEXT("operand_revision"))
			{
				TypedParameters->SetField(Pair.Key, Pair.Value);
			}
		}
		const TSharedPtr<FJsonObject> After =
			MakeShared<FJsonObject>();
		After->SetStringField(
			TEXT("operation_key"),
			Spec->OperationKey);
		After->SetStringField(TEXT("target_ref"), TargetRef);
		After->SetStringField(TEXT("carrier_kind"), CarrierKind);
		After->SetStringField(
			TEXT("expected_before_fingerprint"),
			Fingerprint);
		After->SetObjectField(
			TEXT("typed_parameters"),
			TypedParameters);
		if (!SelectionKind.IsEmpty())
		{
			After->SetField(
				TEXT("selection_ids"),
				Payload->GetObjectField(TEXT("selection"))
					->TryGetField(TEXT("element_ids")));
		}
		else if (bEdgePairs)
		{
			After->SetField(
				TEXT("selection_ids"),
				Payload->TryGetField(TEXT("edge_pairs")));
		}
		if (bOperand)
		{
			After->SetStringField(TEXT("operand_ref"), OperandRef);
			After->SetStringField(
				TEXT("operand_before_fingerprint"),
				OperandFingerprint);
		}

		TArray<FChangeDraft> Changes;
		if (!AddDraftChange(
				*Spec,
				TargetRef,
				Current,
				After,
				Context,
				Resources,
				Changes,
				OutError))
		{
			return false;
		}
		FChangeSetDraft Draft(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			CurrentSnapshot,
			MoveTemp(Changes));
		if (!Draft.IsValid())
		{
			return Fail(
				OutError,
				TEXT("modeling_geometry_change_set_draft_invalid"));
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}
}
