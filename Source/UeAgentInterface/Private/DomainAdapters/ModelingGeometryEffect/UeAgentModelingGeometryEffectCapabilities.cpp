#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
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

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = TEXT(""))
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; return false; }

		TSharedPtr<FJsonValue> NumberFact(const double Value)
		{
			return FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value)));
		}

		TSharedPtr<FJsonValue> NumberArrayFact(const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const double Value : Values) Items.Add(NumberFact(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		TSharedPtr<FJsonValue> JsonFact(const TSharedPtr<FJsonValue>& Value, const bool bIncludeScalar = true)
		{
			if (!Value.IsValid()) return MakeShared<FJsonValueNull>();
			if (bIncludeScalar && Value->Type == EJson::Number) return NumberFact(Value->AsNumber());
			if (bIncludeScalar && (Value->Type == EJson::Null || Value->Type == EJson::Boolean || Value->Type == EJson::String)) return Value;
			const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
			const TCHAR* Kind = TEXT("none");
			switch (Value->Type)
			{
			case EJson::Null: Kind = TEXT("null"); break;
			case EJson::Boolean: Kind = TEXT("boolean"); break;
			case EJson::String: Kind = TEXT("string"); break;
			case EJson::Number: Kind = TEXT("number"); break;
			case EJson::Array: Kind = TEXT("array"); Shape->SetNumberField(TEXT("count"), Value->AsArray().Num()); break;
			case EJson::Object: Kind = TEXT("object"); Shape->SetNumberField(TEXT("count"), Value->AsObject()->Values.Num()); break;
			default: break;
			}
			Shape->SetStringField(TEXT("kind"), Kind);
			return MakeShared<FJsonValueObject>(Shape);
		}

		TSharedPtr<FJsonValue> CanonicalShape(const FUeAgentCanonicalValuePtr& Value)
		{
			if (!Value.IsValid()) return MakeShared<FJsonValueNull>();
			const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
			const TCHAR* Kind = TEXT("unknown");
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: Kind = TEXT("null"); break;
			case EUeAgentValueKind::Boolean: Kind = TEXT("boolean"); break;
			case EUeAgentValueKind::String: Kind = TEXT("string"); break;
			case EUeAgentValueKind::Integer: Kind = TEXT("integer"); break;
			case EUeAgentValueKind::Decimal: Kind = TEXT("decimal"); break;
			case EUeAgentValueKind::Array: Kind = TEXT("array"); Shape->SetNumberField(TEXT("count"), Value->GetArrayItems().Num()); break;
			case EUeAgentValueKind::Object: Kind = TEXT("object"); Shape->SetNumberField(TEXT("count"), Value->GetObjectItems().Num()); break;
			case EUeAgentValueKind::Temporal: Kind = TEXT("temporal"); break;
			case EUeAgentValueKind::Spatial: Kind = TEXT("spatial"); break;
			}
			Shape->SetStringField(TEXT("kind"), Kind);
			return MakeShared<FJsonValueObject>(Shape);
		}

		FDiagnostic InputFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			FDiagnostic Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, MoveTemp(Expected), MoveTemp(Actual), TEXT("execution"), Reason);
			Error.SourceKind = TEXT("authored_value"); Error.TargetPath = Target;
			return Error;
		}

		FDiagnostic NativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			FDiagnostic Error = InputFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			Error.SourceKind = TEXT("native_result"); Error.Source.JsonPath.Reset(); Error.SemanticPointer.Reset();
			return Error;
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

		TSharedPtr<FJsonObject> StateRevisionSchema()
		{
			const TSharedPtr<FJsonObject> Sequence =
				MakeShared<FJsonObject>();
			Sequence->SetStringField(TEXT("type"), TEXT("integer"));
			Sequence->SetNumberField(TEXT("minimum"), 0);
			Sequence->SetNumberField(
				TEXT("maximum"),
				static_cast<double>(
					FUeAgentStateRevisionJson::MaxExactJsonSequence));
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("source_id"), StringSchema());
			Properties->SetObjectField(TEXT("generation"), StringSchema());
			Properties->SetObjectField(TEXT("sequence"), Sequence);
			return ObjectSchema(
				Properties,
				{
					TEXT("source_id"),
					TEXT("generation"),
					TEXT("sequence"),
				});
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
			return ObjectSchema(
				Properties,
				{
					TEXT("element_kind"),
					TEXT("element_ids"),
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
				StateRevisionSchema());
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutCanonical.Reset();
			if (!Object.IsValid())
			{
				OutError = NativeFact(TEXT("modeling_geometry_schema_invalid"),
					TEXT("The generated modeling geometry schema object is missing."), TEXT("/schema_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Stage = TEXT("registry");
				return false;
			}
			if (!Json::TryCanonicalize(Object, OutCanonical, OutError))
			{
				if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutSpecs.Reset(); OutCanonicalResult.Reset(); OutError.Reset();
			TSharedPtr<FJsonObject> Catalog;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Private::DescriptorCatalogJson);
			if (!FJsonSerializer::Deserialize(Reader, Catalog) || !Catalog.IsValid())
			{
				OutError = NativeFact(TEXT("modeling_geometry_catalog_parse_failed"),
					Reader->GetErrorMessage().IsEmpty() ? TEXT("The embedded modeling geometry catalog must be a valid JSON object.")
					: Reader->GetErrorMessage(), TEXT("/catalog"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Stage = TEXT("registry"); OutError.Source.Line = Reader->GetLineNumber(); OutError.Source.Column = Reader->GetCharacterNumber();
				return false;
			}
			FString Schema;
			const FString ExpectedSchema = TEXT("uai.domain_adapter.modeling_geometry_effect.production_catalog.v1");
			if (!Catalog->TryGetStringField(TEXT("schema"), Schema) || Schema != ExpectedSchema)
			{
				OutError = NativeFact(TEXT("modeling_geometry_catalog_parse_failed"),
					TEXT("The embedded modeling geometry catalog has a missing or unsupported schema."), TEXT("/catalog/schema_supported"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Stage = TEXT("registry"); OutError.ActualEvidence = Catalog->TryGetField(TEXT("schema"));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedSchema);
				return false;
			}
			const TSharedPtr<FJsonObject> ResultSchema = BuildResultSchema();
			if (!TryCanonicalize(ResultSchema, OutCanonicalResult, OutError)) return false;
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!Catalog->TryGetArrayField(TEXT("descriptors"), Rows) || !Rows)
			{
				OutError = NativeFact(TEXT("modeling_geometry_descriptors_missing"),
					TEXT("The embedded modeling catalog descriptors field must be an array."), TEXT("/catalog/descriptors"),
					JsonFact(Catalog->TryGetField(TEXT("descriptors"))), MakeShared<FJsonValueString>(TEXT("array")));
				OutError.Stage = TEXT("registry");
				return false;
			}
			OutSpecs.Reserve(Rows->Num());
			for (int32 Index = 0; Index < Rows->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Rows)[Index];
				const TSharedPtr<FJsonObject> Row = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				const FString RowPath = FString::Printf(TEXT("/catalog/descriptors/%d"), Index);
				if (!Row.IsValid())
				{
					OutError = NativeFact(TEXT("modeling_geometry_descriptor_invalid"),
						TEXT("The modeling catalog descriptor must be an object."), RowPath, JsonFact(Value), MakeShared<FJsonValueString>(TEXT("object")));
					OutError.Stage = TEXT("registry"); return false;
				}
				const TSharedPtr<FJsonObject> InputSchema = BuildInputSchema(Row);
				if (!InputSchema.IsValid())
				{
					OutError = NativeFact(TEXT("modeling_geometry_descriptor_invalid"),
						TEXT("No input schema could be built for the modeling catalog descriptor."), RowPath + TEXT("/input_schema_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.Stage = TEXT("registry"); return false;
				}
				FDescriptorSpec Spec;
				const auto ReadField = [&](const TCHAR* Field, FString& Result)
				{
					if (Row->TryGetStringField(Field, Result)) return true;
					OutError = NativeFact(TEXT("modeling_geometry_descriptor_invalid"),
						FString::Printf(TEXT("The modeling catalog descriptor field '%s' must be a string."), Field),
						RowPath + TEXT("/") + Field, JsonFact(Row->TryGetField(Field)), MakeShared<FJsonValueString>(TEXT("string")));
					OutError.Stage = TEXT("registry"); return false;
				};
				if (!ReadField(TEXT("descriptor_id"), Spec.DescriptorId) || !ReadField(TEXT("capability_id"), Spec.CapabilityId)
					|| !ReadField(TEXT("native_operation_type"), Spec.NativeTargetKind) || !ReadField(TEXT("operation_key"), Spec.OperationKey)) return false;
				if (!TryCanonicalize(InputSchema, Spec.CanonicalInputSchema, OutError)) return false;
				Row->TryGetBoolField(TEXT("primitive_output"), Spec.bPrimitiveOutput);
				const TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
				Descriptor->SetStringField(TEXT("capability_id"), Spec.CapabilityId);
				Descriptor->SetStringField(TEXT("descriptor_id"), Spec.DescriptorId);
				Descriptor->SetStringField(TEXT("domain_id"), TEXT("mesh"));
				Descriptor->SetObjectField(TEXT("execution_context_policy"), ExecutionPolicyObject());
				Descriptor->SetObjectField(TEXT("input_schema"), InputSchema);
				Descriptor->SetStringField(TEXT("operation_kind"), TEXT("operation"));
				Descriptor->SetObjectField(TEXT("result_schema"), ResultSchema);
				Descriptor->SetNumberField(TEXT("version"), 1);
				FString CanonicalDescriptor;
				if (!TryCanonicalize(Descriptor, CanonicalDescriptor, OutError)) return false;
				Spec.DescriptorHash = Json::Sha256Utf8(CanonicalDescriptor);
				Spec.Row = Row; OutSpecs.Add(MoveTemp(Spec));
			}
			return true;
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError,
			const FString& Path = TEXT("/draft_after"))
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
				return Fail(OutError, NativeFact(TEXT("modeling_geometry_internal_value_missing"),
					TEXT("The generated modeling change contains a missing value."), Path,
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			FUeAgentStateSemanticError Error;
			bool bConverted = false;
			switch (JsonValue->Type)
			{
			case EJson::Null: bConverted = FUeAgentCanonicalValue::TryNull(Resources, OutValue, Error); break;
			case EJson::Boolean: bConverted = FUeAgentCanonicalValue::TryBoolean(JsonValue->AsBool(), Resources, OutValue, Error); break;
			case EJson::String: bConverted = FUeAgentCanonicalValue::TryString(JsonValue->AsString(), Resources, OutValue, Error); break;
			case EJson::Number:
				bConverted = FUeAgentCanonicalValue::TryDecimal(FString::SanitizeFloat(JsonValue->AsNumber()), Resources, OutValue, Error);
				break;
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					const TArray<TSharedPtr<FJsonValue>>& JsonItems = JsonValue->AsArray();
					for (int32 Index = 0; Index < JsonItems.Num(); ++Index)
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalValue(JsonItems[Index], Resources, Canonical, OutError,
							Path + FString::Printf(TEXT("/%d"), Index))) return false;
						Items.Add(Canonical.ToSharedRef());
					}
					bConverted = FUeAgentCanonicalValue::TryArray(Items, Resources, OutValue, Error);
					break;
				}
			case EJson::Object:
				{
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr Canonical;
						const FString Key = Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						if (!TryCanonicalValue(Pair.Value, Resources, Canonical, OutError, Path + TEXT("/") + Key)) return false;
						Items.Emplace(Pair.Key, Canonical.ToSharedRef());
					}
					bConverted = FUeAgentCanonicalValue::TryObject(Items, Resources, OutValue, Error);
					break;
				}
			default:
				return Fail(OutError, NativeFact(TEXT("modeling_geometry_internal_value_failed"),
					TEXT("The generated modeling change contains an unsupported JSON value kind."), Path,
					JsonFact(JsonValue, false), MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array, or object"))));
			}
			if (!bConverted)
			{
				OutError = MoveTemp(Error);
				if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = Path + OutError.PropertyPath;
				if (OutError.SourceKind == TEXT("native_result"))
				{
					OutError.Source.JsonPath.Reset(); OutError.SemanticPointer.Reset();
				}
			}
			return bConverted;
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
			const FUeAgentCanonicalValuePtr& Object, const FString& Field, TSet<int32>& OutValues,
			FExecutionMutationError& OutError, const FString& TargetRef)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Value = FindCanonicalField(Object, Field);
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::Array)
				return Fail(OutError, NativeFact(TEXT("modeling_geometry_selection_element_invalid"),
					TEXT("The target snapshot is missing an array of selectable element identifiers."),
					TEXT("/snapshot/") + Field, CanonicalShape(Value), MakeShared<FJsonValueString>(TEXT("array of integer or decimal identifiers")), TargetRef));
			const TArray<FUeAgentCanonicalValueRef>& Items = Value->GetArrayItems();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Item = Items[Index];
				if (Item->GetKind() != EUeAgentValueKind::Integer && Item->GetKind() != EUeAgentValueKind::Decimal)
					return Fail(OutError, NativeFact(TEXT("modeling_geometry_selection_element_invalid"),
						TEXT("The target snapshot contains a non-numeric element identifier."),
						TEXT("/snapshot/") + Field + FString::Printf(TEXT("/%d"), Index), CanonicalShape(FUeAgentCanonicalValuePtr(Item)),
						MakeShared<FJsonValueString>(TEXT("integer or decimal identifier")), TargetRef));
				OutValues.Add(FCString::Atoi(*Item->GetLexeme()));
			}
			return true;
		}

		const FUeAgentStateEntry* FindSnapshotTarget(
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			const FString& TargetRef, int32* OutMatchCount = nullptr)
		{
			if (OutMatchCount) *OutMatchCount = 0;
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
				if (OutMatchCount) ++*OutMatchCount;
				if (Match)
				{
					return nullptr;
				}
				Match = &Entry;
			}
			return Match;
		}

		bool ReadJsonNumberArray(
			const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const int32 Count,
			TArray<double>& OutValues, FExecutionMutationError& OutError,
			const FString& ParentPath, const FString& Code, const FString& TargetRef)
		{
			OutValues.Reset();
			const FString Path = ParentPath + TEXT("/") + Field;
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(Field, Items) || !Items || Items->Num() != Count)
				return Fail(OutError, InputFact(Code, FString::Printf(TEXT("The field '%s' must contain exactly %d finite numbers."), Field, Count),
					Path, JsonFact(Object.IsValid() ? Object->TryGetField(Field) : nullptr),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("array of %d finite numbers"), Count)), TargetRef));
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Item = (*Items)[Index];
				double Number = 0.0;
				if (!Item.IsValid() || !Item->TryGetNumber(Number) || !FMath::IsFinite(Number))
					return Fail(OutError, InputFact(Code, TEXT("This array component must be a finite number."),
						Path + FString::Printf(TEXT("/%d"), Index), JsonFact(Item), MakeShared<FJsonValueString>(TEXT("finite number")), TargetRef));
				OutValues.Add(Number);
			}
			return true;
		}

		bool ValidateSelection(
			const TSharedPtr<FJsonObject>& Payload, const FString& SelectionKind,
			const FUeAgentCanonicalValuePtr& Current, FExecutionMutationError& OutError, const FString& TargetRef)
		{
			const TSharedPtr<FJsonObject>* Selection = nullptr;
			if (!Payload->TryGetObjectField(TEXT("selection"), Selection) || !Selection || !Selection->IsValid())
				return Fail(OutError, InputFact(TEXT("modeling_geometry_selection_element_invalid"),
					TEXT("The modeling selection must be an object."), TEXT("/selection"), JsonFact(Payload->TryGetField(TEXT("selection"))),
					MakeShared<FJsonValueString>(TEXT("object with element_ids")), TargetRef));
			const TCHAR* SnapshotField = SelectionKind == TEXT("vertex") ? TEXT("vertex_ids")
				: SelectionKind == TEXT("triangle") ? TEXT("triangle_ids") : SelectionKind == TEXT("boundary_edge") ? TEXT("boundary_edge_ids") : TEXT("edge_ids");
			TSet<int32> Allowed;
			if (!ReadCanonicalIntegerSet(Current, SnapshotField, Allowed, OutError, TargetRef)) return false;
			const TArray<TSharedPtr<FJsonValue>>* ElementIds = nullptr;
			if (!(*Selection)->TryGetArrayField(TEXT("element_ids"), ElementIds) || !ElementIds)
				return Fail(OutError, InputFact(TEXT("modeling_geometry_selection_element_invalid"),
					TEXT("The selection element_ids field must be an array."), TEXT("/selection/element_ids"),
					JsonFact((*Selection)->TryGetField(TEXT("element_ids"))), MakeShared<FJsonValueString>(TEXT("array")), TargetRef));
			for (int32 Index = 0; Index < ElementIds->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Item = (*ElementIds)[Index];
				double Number = -1.0;
				const bool bNumber = Item.IsValid() && Item->TryGetNumber(Number);
				const bool bRepresentable = bNumber && FMath::IsFinite(Number) && Number >= MIN_int32 && Number <= MAX_int32;
				if (!bRepresentable || !Allowed.Contains(static_cast<int32>(Number)))
				{
					const TSharedRef<FJsonObject> Expected = MakeShared<FJsonObject>();
					Expected->SetStringField(TEXT("selection_kind"), SelectionKind);
					Expected->SetNumberField(TEXT("allowed_count"), Allowed.Num());
					Expected->SetStringField(TEXT("constraint"), TEXT("numeric int32-representable identifier contained in the target snapshot"));
					return Fail(OutError, InputFact(SelectionKind == TEXT("boundary_edge")
						? TEXT("modeling_geometry_boundary_edge_invalid") : TEXT("modeling_geometry_selection_element_invalid"), bRepresentable
						? TEXT("The selected element is not present in the target's current selectable identifiers.")
						: TEXT("The selected element identifier is not a finite int32-representable number."),
						FString::Printf(TEXT("/selection/element_ids/%d"), Index), JsonFact(Item), MakeShared<FJsonValueObject>(Expected), TargetRef));
				}
			}
			return true;
		}

		bool ValidateOperandTransform(const TSharedPtr<FJsonObject>& Payload, FExecutionMutationError& OutError, const FString& TargetRef)
		{
			const FString Code = TEXT("modeling_geometry_operand_transform_invalid");
			const TSharedPtr<FJsonObject>* Transform = nullptr;
			if (!Payload->TryGetObjectField(TEXT("operand_to_target"), Transform) || !Transform)
				return Fail(OutError, InputFact(Code, TEXT("The operand_to_target transform must be an object."), TEXT("/operand_to_target"),
					JsonFact(Payload->TryGetField(TEXT("operand_to_target"))), MakeShared<FJsonValueString>(TEXT("object")), TargetRef));
			TArray<double> Rotation;
			TArray<double> Scale;
			if (!ReadJsonNumberArray(*Transform, TEXT("rotation_quaternion_xyzw"), 4, Rotation, OutError, TEXT("/operand_to_target"), Code, TargetRef)
				|| !ReadJsonNumberArray(*Transform, TEXT("scale"), 3, Scale, OutError, TEXT("/operand_to_target"), Code, TargetRef)) return false;
			double QuaternionNormSquared = 0.0;
			for (const double Component : Rotation) QuaternionNormSquared += Component * Component;
			if (QuaternionNormSquared <= UE_DOUBLE_SMALL_NUMBER)
				return Fail(OutError, InputFact(Code, TEXT("The operand rotation quaternion has a near-zero norm."),
					TEXT("/operand_to_target/rotation_quaternion_xyzw"), NumberArrayFact(Rotation),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("quaternion norm squared greater than %.17g"), static_cast<double>(UE_DOUBLE_SMALL_NUMBER))), TargetRef));
			for (int32 Index = 0; Index < Scale.Num(); ++Index)
			{
				if (FMath::Abs(Scale[Index]) <= UE_DOUBLE_SMALL_NUMBER)
					return Fail(OutError, InputFact(Code, TEXT("The operand transform contains a near-zero scale component."),
						FString::Printf(TEXT("/operand_to_target/scale/%d"), Index), NumberFact(Scale[Index]),
						MakeShared<FJsonValueString>(FString::Printf(TEXT("absolute scale greater than %.17g"), static_cast<double>(UE_DOUBLE_SMALL_NUMBER))), TargetRef));
			}
			return true;
		}

		bool ValidatePlane(const TSharedPtr<FJsonObject>& Payload, FExecutionMutationError& OutError, const FString& TargetRef)
		{
			const FString Code = TEXT("modeling_geometry_plane_invalid");
			const TSharedPtr<FJsonObject>* Plane = nullptr;
			if (!Payload->TryGetObjectField(TEXT("plane"), Plane) || !Plane)
				return Fail(OutError, InputFact(Code, TEXT("The modeling plane must be an object."), TEXT("/plane"),
					JsonFact(Payload->TryGetField(TEXT("plane"))), MakeShared<FJsonValueString>(TEXT("object with a normal")), TargetRef));
			TArray<double> Normal;
			if (!ReadJsonNumberArray(*Plane, TEXT("normal"), 3, Normal, OutError, TEXT("/plane"), Code, TargetRef)) return false;
			if (FVector3d(Normal[0], Normal[1], Normal[2]).IsNearlyZero())
				return Fail(OutError, InputFact(Code, TEXT("The plane normal is nearly zero."), TEXT("/plane/normal"),
					NumberArrayFact(Normal), MakeShared<FJsonValueString>(TEXT("normal outside the FVector3d nearly-zero tolerance")), TargetRef));
			return true;
		}

		bool ValidateEdgePairs(const TSharedPtr<FJsonObject>& Payload, FExecutionMutationError& OutError, const FString& TargetRef)
		{
			const FString Code = TEXT("modeling_geometry_edge_pair_invalid");
			const TArray<TSharedPtr<FJsonValue>>* Pairs = nullptr;
			if (!Payload->TryGetArrayField(TEXT("edge_pairs"), Pairs) || !Pairs)
				return Fail(OutError, InputFact(Code, TEXT("The edge_pairs field must be an array."), TEXT("/edge_pairs"),
					JsonFact(Payload->TryGetField(TEXT("edge_pairs"))), MakeShared<FJsonValueString>(TEXT("array of two-endpoint pairs")), TargetRef));
			TSet<FString> CanonicalPairs;
			for (int32 Index = 0; Index < Pairs->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& PairValue = (*Pairs)[Index];
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				if (!PairValue.IsValid() || !PairValue->TryGetArray(Pair) || !Pair || Pair->Num() != 2)
					return Fail(OutError, InputFact(Code, TEXT("Each edge pair must contain exactly two endpoint identifiers."),
						FString::Printf(TEXT("/edge_pairs/%d"), Index), JsonFact(PairValue), MakeShared<FJsonValueString>(TEXT("array of length 2")), TargetRef));
				int32 Endpoints[2];
				for (int32 Endpoint = 0; Endpoint < 2; ++Endpoint)
				{
					const TSharedPtr<FJsonValue>& Item = (*Pair)[Endpoint];
					double Value = 0.0;
					if (!Item.IsValid() || !Item->TryGetNumber(Value) || !FMath::IsFinite(Value) || Value < MIN_int32 || Value > MAX_int32)
						return Fail(OutError, InputFact(Code, TEXT("The edge endpoint must be a finite int32-representable number."),
							FString::Printf(TEXT("/edge_pairs/%d/%d"), Index, Endpoint), JsonFact(Item),
							MakeShared<FJsonValueString>(TEXT("finite int32-representable identifier")), TargetRef));
					Endpoints[Endpoint] = static_cast<int32>(Value);
				}
				const int32 A = Endpoints[0];
				const int32 B = Endpoints[1];
				const FString Key = FString::Printf(TEXT("%d:%d"), FMath::Min(A, B), FMath::Max(A, B));
				if (A == B || CanonicalPairs.Contains(Key))
					return Fail(OutError, InputFact(Code, A == B ? TEXT("The edge pair repeats the same endpoint.")
						: TEXT("This unordered edge pair is already listed."), FString::Printf(TEXT("/edge_pairs/%d"), Index),
						NumberArrayFact(TArray<double>{ static_cast<double>(A), static_cast<double>(B) }),
						MakeShared<FJsonValueString>(TEXT("two distinct endpoints forming an unlisted unordered pair")), TargetRef));
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
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
			if (!OutChanges.Last().IsValid())
			{
				OutError = FDiagnostic::WrapFailure(TEXT("modeling_geometry_change_draft_invalid"),
					OutChanges.Last().GetValidationDiagnostic(), TEXT("execution"), TEXT("The modeling geometry change draft is invalid."));
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				return false;
			}
			return true;
		}
	}

	TArray<FString> GetModelingGeometryEffectAuthorityScopes()
	{
		return {TEXT("uai:modeling-geometry:write")};
	}

	bool TryCreateModelingGeometryEffectDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
				const FString Field = Profile.DescriptorId.IsEmpty() ? TEXT("descriptor_id")
					: Profile.DescriptorVersion <= 0 ? TEXT("descriptor_version") : Profile.DescriptorHash.IsEmpty() ? TEXT("descriptor_hash")
					: Profile.CapabilityId.IsEmpty() ? TEXT("capability_id") : Profile.NativeTargetKind.IsEmpty() ? TEXT("native_operation_type") : TEXT("operation_key");
				OutError = NativeFact(TEXT("modeling_geometry_production_profile_invalid"),
					FString::Printf(TEXT("The modeling geometry production profile has an invalid required field '%s'."), *Field),
					TEXT("/profile/") + Field + TEXT("_valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Stage = TEXT("registry"); OutError.CandidateName = Profile.DescriptorId;
				return false;
			}
			OutProfiles.Add(MoveTemp(Profile));
		}
		return true;
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
		if (!Request.IsValid())
			return Fail(OutError, FDiagnostic::WrapFailure(TEXT("modeling_geometry_draft_request_invalid"),
				Request.GetValidationDiagnostic(), TEXT("execution"), TEXT("The modeling geometry operation request is invalid.")));
		if (Context.GetWorldIdentity().IsEmpty() || Context.GetLevelIdentity().IsEmpty()
			|| Context.GetContextRevision().IsEmpty() || CurrentSnapshot->GetStatePlane() != EUeAgentStatePlane::AuthoringMemory)
		{
			const FString Field = Context.GetWorldIdentity().IsEmpty() ? TEXT("/execution_context/world_available")
				: Context.GetLevelIdentity().IsEmpty() ? TEXT("/execution_context/level_available")
				: Context.GetContextRevision().IsEmpty() ? TEXT("/execution_context/revision_available") : TEXT("/snapshot/authoring_memory");
			const FString Reason = Context.GetWorldIdentity().IsEmpty() ? TEXT("The modeling geometry execution context has no world identity.")
				: Context.GetLevelIdentity().IsEmpty() ? TEXT("The modeling geometry execution context has no level identity.")
				: Context.GetContextRevision().IsEmpty() ? TEXT("The modeling geometry execution context has no revision.")
				: TEXT("Modeling geometry drafts require an authoring-memory snapshot.");
			OutError = NativeFact(TEXT("modeling_geometry_draft_request_invalid"), Reason, Field,
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueNumber>(static_cast<int32>(CurrentSnapshot->GetStatePlane()));
			return false;
		}

		TArray<FDescriptorSpec> Specs;
		FString CanonicalResult;
		UE::UAI::Contract::FDiagnostic CatalogError;
		if (!TryLoadSpecs(Specs, CanonicalResult, CatalogError))
		{
			return Fail(OutError, CatalogError);
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
			const FUeAgentDescriptorIdentity& Identity = Request.GetDescriptorIdentity();
			OutError = NativeFact(TEXT("modeling_geometry_descriptor_not_exact"),
				TEXT("The requested modeling geometry descriptor does not match a registered descriptor definition."),
				TEXT("/descriptor_identity_matches"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = Identity.GetDescriptorId();
			const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
			Actual->SetStringField(TEXT("descriptor_id"), Identity.GetDescriptorId());
			Actual->SetNumberField(TEXT("version"), Identity.GetVersion());
			Actual->SetStringField(TEXT("hash"), Identity.GetDescriptorHash());
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Actual);
			return false;
		}

		TSharedPtr<FJsonObject> Payload;
		UE::UAI::Contract::FDiagnostic ParseError;
		if (!Contract::Json::TryParseObject(Request.GetPayload().GetJsonValue(), Payload, ParseError))
		{
			return Fail(OutError, FDiagnostic::WrapFailure(TEXT("modeling_geometry_payload_parse_failed"), ParseError,
				TEXT("execution"), TEXT("The modeling geometry request payload could not be parsed as an object.")));
		}
		FString TargetRef;
		if (!Payload->TryGetStringField(
				TEXT("target_ref"),
				TargetRef)
			|| TargetRef.IsEmpty())
		{
			return Fail(OutError, InputFact(TEXT("modeling_geometry_reference_unresolved"),
				TEXT("A non-empty modeling target_ref string is required."), TEXT("/target_ref"),
				JsonFact(Payload->TryGetField(TEXT("target_ref"))), MakeShared<FJsonValueString>(TEXT("non-empty target reference"))));
		}

		int32 TargetMatchCount = 0;
		const FUeAgentStateEntry* Target = FindSnapshotTarget(CurrentSnapshot, TargetRef, &TargetMatchCount);
		FUeAgentCanonicalValuePtr Current;
		FString CarrierKind = TEXT("dynamic_mesh_actor");
		FString Fingerprint = TEXT("absent");
		if (Spec->bPrimitiveOutput)
		{
			if (Target)
			{
				return Fail(OutError, InputFact(TEXT("modeling_geometry_output_already_exists"),
					TEXT("The primitive output target already exists in the current snapshot."), TEXT("/target_ref"),
					MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("target absent from the current snapshot")), TargetRef));
			}
		}
		else
		{
			if (!Target)
			{
				OutError = NativeFact(TEXT("modeling_geometry_reference_unresolved"), TargetMatchCount == 0
					? TEXT("No target in the current modeling snapshot matches target_ref.")
					: TEXT("Multiple targets in the current modeling snapshot match target_ref."),
					TEXT("/snapshot/target_match_count"), TargetMatchCount < 2
					? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(TargetMatchCount)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("at least 2"))),
					MakeShared<FJsonValueNumber>(1), TargetRef);
				OutError.SourceKind = TEXT("related_input"); OutError.Source.JsonPath = TEXT("/target_ref");
				return false;
			}
			Current = FUeAgentCanonicalValuePtr(Target->GetValue());
			TOptional<FUeAgentStateRevision> CurrentRevision;
			if (!FUeAgentStateRevisionJson::TryReadCanonical(
					FindCanonicalField(
						Current,
						TEXT("target_revision")),
					CurrentRevision, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				OutError.SourceKind = TEXT("native_result");
				const FString Suffix = OutError.PropertyPath == TEXT("target_revision") ? FString() : OutError.PropertyPath;
				OutError.PropertyPath = TEXT("snapshot/target_revision")
					+ (Suffix.IsEmpty() ? FString() : Suffix.StartsWith(TEXT("/")) ? Suffix : TEXT("/") + Suffix);
				return false;
			}
			if (!ReadCanonicalString(
					Current,
					TEXT("carrier_kind"),
					CarrierKind)
				|| (CarrierKind != TEXT("dynamic_mesh_actor")
					&& CarrierKind
						!= TEXT("static_mesh_asset")))
			{
				const FUeAgentCanonicalValuePtr Carrier = FindCanonicalField(Current, TEXT("carrier_kind"));
				return Fail(OutError, NativeFact(TEXT("modeling_geometry_carrier_unsupported"),
					TEXT("The modeling target snapshot must describe a dynamic mesh actor or static mesh asset."), TEXT("/snapshot/carrier_kind"),
					Carrier.IsValid() && Carrier->GetKind() == EUeAgentValueKind::String
					? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Carrier->GetText())) : CanonicalShape(Carrier),
					MakeShared<FJsonValueString>(TEXT("dynamic_mesh_actor or static_mesh_asset")), TargetRef));
			}
			if (!ReadCanonicalString(
					Current,
					TEXT("geometry_fingerprint"),
					Fingerprint))
			{
				OutError = NativeFact(TEXT("modeling_geometry_target_state_unavailable"),
					TEXT("The target snapshot has no readable non-empty geometry fingerprint."), TEXT("/snapshot/geometry_fingerprint_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TargetRef);
				OutError.ActualEvidence = CanonicalShape(FindCanonicalField(Current, TEXT("geometry_fingerprint")));
				return false;
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
				OutError, TargetRef))
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
			if (!Payload->TryGetStringField(TEXT("operand_ref"), OperandRef) || OperandRef.IsEmpty())
				return Fail(OutError, InputFact(TEXT("modeling_geometry_operand_unresolved"),
					TEXT("A non-empty operand_ref string is required for this operation."), TEXT("/operand_ref"),
					JsonFact(Payload->TryGetField(TEXT("operand_ref"))), MakeShared<FJsonValueString>(TEXT("non-empty operand reference")), TargetRef));
			int32 OperandMatchCount = 0;
			const FUeAgentStateEntry* Operand = FindSnapshotTarget(CurrentSnapshot, OperandRef, &OperandMatchCount);
			if (!Operand)
			{
				OutError = NativeFact(TEXT("modeling_geometry_operand_unresolved"), OperandMatchCount == 0
					? TEXT("No target in the current modeling snapshot matches operand_ref.")
					: TEXT("Multiple targets in the current modeling snapshot match operand_ref."),
					TEXT("/snapshot/operand_match_count"), OperandMatchCount < 2
					? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(OperandMatchCount)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("at least 2"))),
					MakeShared<FJsonValueNumber>(1), OperandRef);
				OutError.SourceKind = TEXT("related_input"); OutError.Source.JsonPath = TEXT("/operand_ref");
				return false;
			}
			const FUeAgentCanonicalValuePtr OperandCurrent(
				Operand->GetValue());
			TOptional<FUeAgentStateRevision> OperandCurrentRevision;
			if (!FUeAgentStateRevisionJson::TryReadCanonical(
					FindCanonicalField(
						OperandCurrent,
						TEXT("target_revision")),
					OperandCurrentRevision, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = OperandRef;
				OutError.SourceKind = TEXT("native_result");
				const FString Suffix = OutError.PropertyPath == TEXT("target_revision") ? FString() : OutError.PropertyPath;
				OutError.PropertyPath = TEXT("snapshot/target_revision")
					+ (Suffix.IsEmpty() ? FString() : Suffix.StartsWith(TEXT("/")) ? Suffix : TEXT("/") + Suffix);
				return false;
			}
			if (!ReadCanonicalString(OperandCurrent, TEXT("geometry_fingerprint"), OperandFingerprint))
			{
				OutError = NativeFact(TEXT("modeling_geometry_operand_state_unavailable"),
					TEXT("The operand snapshot has no readable non-empty geometry fingerprint."), TEXT("/snapshot/geometry_fingerprint_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OperandRef);
				OutError.ActualEvidence = CanonicalShape(FindCanonicalField(OperandCurrent, TEXT("geometry_fingerprint")));
				return false;
			}
			if (!ValidateOperandTransform(Payload, OutError, TargetRef)) return false;
		}

		bool bPlane = false;
		if (Spec->Row->TryGetBoolField(TEXT("plane"), bPlane)
			&& bPlane
			&& !ValidatePlane(Payload, OutError, TargetRef))
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
			return Fail(OutError, InputFact(TEXT("modeling_geometry_direction_required"),
				TEXT("A direction must be supplied when direction_mode is fixed."), TEXT("/direction"),
				JsonFact(Payload->TryGetField(TEXT("direction"))), MakeShared<FJsonValueString>(TEXT("direction value for fixed mode")), TargetRef));
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
				return Fail(OutError, InputFact(TEXT("modeling_geometry_simplify_target_invalid"),
					TEXT("A triangle_ratio simplify target must not exceed 1."), TEXT("/target_value"), NumberFact(TargetValue),
					MakeShared<FJsonValueString>(TEXT("value at most 1 when target_mode is triangle_ratio")), TargetRef));
			}
		}
		bool bEdgePairs = false;
		if (Spec->Row->TryGetBoolField(
				TEXT("edge_pairs"),
				bEdgePairs)
			&& bEdgePairs
			&& !ValidateEdgePairs(Payload, OutError, TargetRef))
		{
			return false;
		}

		const TSharedPtr<FJsonObject> TypedParameters =
			MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
			: Payload->Values)
		{
			if (Pair.Key != TEXT("target_ref")
				&& Pair.Key != TEXT("operand_ref"))
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
			OutError = FDiagnostic::WrapFailure(TEXT("modeling_geometry_change_set_draft_invalid"), Draft.GetValidationDiagnostic(),
				TEXT("execution"), TEXT("The modeling geometry change-set draft is invalid."));
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
			return false;
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}
}
