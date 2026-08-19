#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"

namespace
{
	using namespace UE::UAI::Contract;

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
		Schema->SetBoolField(
			TEXT("additionalProperties"),
			false);
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
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		for (const FString& Value : Values)
		{
			EnumValues.Add(
				MakeShared<FJsonValueString>(Value));
		}
		Schema->SetArrayField(TEXT("enum"), MoveTemp(EnumValues));
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
		Properties->SetObjectField(
			TEXT("origin"),
			NumberArraySchema(3, false));
		Properties->SetObjectField(
			TEXT("axis_u"),
			NumberArraySchema(3, false));
		Properties->SetObjectField(
			TEXT("axis_v"),
			NumberArraySchema(3, false));
		Properties->SetObjectField(
			TEXT("axis_w"),
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
		FAutomationTestBase* Test,
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
			AddRequired(
				TEXT("operand_revision"),
				StringSchema());
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
			AddRequired(
				TEXT("edge_pairs"),
				EdgePairsSchema());
		}

		for (const TSharedPtr<FJsonValue>& Value
			: Row->GetArrayField(TEXT("parameters")))
		{
			const TSharedPtr<FJsonObject> Spec =
				Value->AsObject();
			const FString Name =
				Spec->GetStringField(TEXT("name"));
			const TSharedPtr<FJsonObject> Schema =
				ParameterSchema(Spec);
			if (!Test->TestTrue(
					*(Name + TEXT(" has a supported schema kind")),
					Schema.IsValid()))
			{
				continue;
			}
			Properties->SetObjectField(Name, Schema);
			bool bRequired = true;
			if (!Spec->TryGetBoolField(
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

		const TSharedPtr<FJsonObject> StringItems =
			StringSchema();
		const TSharedPtr<FJsonObject> UniqueStrings =
			MakeShared<FJsonObject>();
		UniqueStrings->SetStringField(TEXT("type"), TEXT("array"));
		UniqueStrings->SetObjectField(
			TEXT("items"),
			StringItems);
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

	TArray<TSharedPtr<FJsonValue>> NumberValues(
		const TArray<double>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const double Value : Values)
		{
			Result.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> ValidPlacement()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("semantic_frame_id"),
			TEXT("ue.world"));
		Value->SetStringField(
			TEXT("write_frame_id"),
			TEXT("ue.world"));
		Value->SetStringField(
			TEXT("read_frame_id"),
			TEXT("ue.world"));
		Value->SetStringField(
			TEXT("verify_frame_id"),
			TEXT("ue.world"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetArrayField(
			TEXT("translation"),
			NumberValues({100.0, 200.0, 300.0}));
		Value->SetArrayField(
			TEXT("rotation_quaternion_xyzw"),
			NumberValues({0.0, 0.0, 0.0, 1.0}));
		Value->SetArrayField(
			TEXT("scale"),
			NumberValues({1.0, 2.0, 3.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("explicit normalized UE world placement"));
		Value->SetStringField(
			TEXT("conversion_chain"),
			TEXT(
				"semantic world -> C4 normalized UE world -> "
				"actor transform -> independent UE world readback"));
		return Value;
	}

	TSharedPtr<FJsonObject> ValidSpaceContract()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("semantic_frame_id"),
			TEXT("agent.mesh.intent"));
		Value->SetStringField(
			TEXT("write_frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(
			TEXT("read_frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(
			TEXT("verify_frame_id"),
			TEXT("agent.mesh.intent"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact carrier transform and geometry revision"));
		Value->SetStringField(
			TEXT("conversion_chain"),
			TEXT(
				"semantic -> C4 normalized target local -> "
				"working mesh -> carrier -> target local readback"));
		return Value;
	}

	TSharedPtr<FJsonObject> ValidTransform()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("from_frame_id"),
			TEXT("ue.mesh.operand_local"));
		Value->SetStringField(
			TEXT("to_frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetArrayField(
			TEXT("translation"),
			NumberValues({25.0, 0.0, 0.0}));
		Value->SetArrayField(
			TEXT("rotation_quaternion_xyzw"),
			NumberValues({0.0, 0.0, 0.0, 1.0}));
		Value->SetArrayField(
			TEXT("scale"),
			NumberValues({1.0, 1.0, 1.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact target and operand carrier transforms"));
		Value->SetStringField(
			TEXT("conversion_chain"),
			TEXT("operand local -> target local"));
		return Value;
	}

	TSharedPtr<FJsonObject> ValidSelection(
		const FString& Kind)
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("element_kind"), Kind);
		Value->SetArrayField(
			TEXT("element_ids"),
			NumberValues({0.0}));
		Value->SetStringField(
			TEXT("mesh_revision"),
			TEXT("rev:fixture:1"));
		return Value;
	}

	TSharedPtr<FJsonObject> ValidPlane()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetArrayField(
			TEXT("origin"),
			NumberValues({0.0, 0.0, 0.0}));
		Value->SetArrayField(
			TEXT("normal"),
			NumberValues({1.0, 0.0, 0.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("target-local plane from C4"));
		return Value;
	}

	TSharedPtr<FJsonObject> ValidDirection()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetArrayField(
			TEXT("vector"),
			NumberValues({0.0, 0.0, 1.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("target-local direction from C4"));
		return Value;
	}

	TSharedPtr<FJsonObject> ValidProjectionFrame()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("frame_id"),
			TEXT("ue.mesh.target_local"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetArrayField(
			TEXT("origin"),
			NumberValues({0.0, 0.0, 0.0}));
		Value->SetArrayField(
			TEXT("axis_u"),
			NumberValues({1.0, 0.0, 0.0}));
		Value->SetArrayField(
			TEXT("axis_v"),
			NumberValues({0.0, 1.0, 0.0}));
		Value->SetArrayField(
			TEXT("axis_w"),
			NumberValues({0.0, 0.0, 1.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("orthonormal target-local projection basis"));
		return Value;
	}

	TSharedPtr<FJsonObject> BuildValidInput(
		const TSharedPtr<FJsonObject>& Row)
	{
		const TSharedPtr<FJsonObject> Input =
			MakeShared<FJsonObject>();
		Input->SetStringField(
			TEXT("target_ref"),
			TEXT("uai://mesh/fixture-target"));
		bool bValue = false;
		if (Row->TryGetBoolField(
				TEXT("existing_target"),
				bValue)
			&& bValue)
		{
			Input->SetStringField(
				TEXT("target_revision"),
				TEXT("rev:fixture:1"));
		}
		if (Row->TryGetBoolField(TEXT("placement"), bValue)
			&& bValue)
		{
			Input->SetObjectField(
				TEXT("placement"),
				ValidPlacement());
		}
		if (Row->TryGetBoolField(
				TEXT("space_contract"),
				bValue)
			&& bValue)
		{
			Input->SetObjectField(
				TEXT("space_contract"),
				ValidSpaceContract());
		}
		FString SelectionKind;
		if (Row->TryGetStringField(
				TEXT("selection_kind"),
				SelectionKind))
		{
			Input->SetObjectField(
				TEXT("selection"),
				ValidSelection(SelectionKind));
		}
		if (Row->TryGetBoolField(TEXT("operand"), bValue)
			&& bValue)
		{
			Input->SetStringField(
				TEXT("operand_ref"),
				TEXT("uai://mesh/fixture-operand"));
			Input->SetStringField(
				TEXT("operand_revision"),
				TEXT("rev:operand:1"));
			Input->SetObjectField(
				TEXT("operand_to_target"),
				ValidTransform());
		}
		if (Row->TryGetBoolField(TEXT("plane"), bValue)
			&& bValue)
		{
			Input->SetObjectField(TEXT("plane"), ValidPlane());
		}
		if (Row->TryGetBoolField(TEXT("direction"), bValue)
			&& bValue)
		{
			Input->SetObjectField(
				TEXT("direction"),
				ValidDirection());
		}
		if (Row->TryGetBoolField(
				TEXT("projection_frame"),
				bValue)
			&& bValue)
		{
			Input->SetObjectField(
				TEXT("projection_frame"),
				ValidProjectionFrame());
		}
		if (Row->TryGetBoolField(TEXT("edge_pairs"), bValue)
			&& bValue)
		{
			TArray<TSharedPtr<FJsonValue>> Pair;
			Pair.Add(MakeShared<FJsonValueNumber>(0));
			Pair.Add(MakeShared<FJsonValueNumber>(1));
			TArray<TSharedPtr<FJsonValue>> Pairs;
			Pairs.Add(
				MakeShared<FJsonValueArray>(MoveTemp(Pair)));
			Input->SetArrayField(
				TEXT("edge_pairs"),
				MoveTemp(Pairs));
		}

		for (const TSharedPtr<FJsonValue>& Value
			: Row->GetArrayField(TEXT("parameters")))
		{
			const TSharedPtr<FJsonObject> Spec =
				Value->AsObject();
			const FString Name =
				Spec->GetStringField(TEXT("name"));
			const FString Kind =
				Spec->GetStringField(TEXT("kind"));
			if (Kind == TEXT("string"))
			{
				Input->SetStringField(Name, TEXT("fixture"));
			}
			else if (Kind == TEXT("boolean"))
			{
				Input->SetBoolField(Name, true);
			}
			else if (Kind == TEXT("enum"))
			{
				Input->SetStringField(
					Name,
					Spec->GetArrayField(TEXT("values"))[0]
						->AsString());
			}
			else if (Kind.EndsWith(TEXT("_array")))
			{
				const int32 Count = static_cast<int32>(
					Spec->GetNumberField(TEXT("count")));
				TArray<TSharedPtr<FJsonValue>> Items;
				double Minimum = 1.0;
				Spec->TryGetNumberField(
					TEXT("minimum"),
					Minimum);
				for (int32 Index = 0; Index < Count; ++Index)
				{
					Items.Add(
						MakeShared<FJsonValueNumber>(
							FMath::Max(1.0, Minimum)));
				}
				Input->SetArrayField(Name, MoveTemp(Items));
			}
			else
			{
				double Minimum = 1.0;
				Spec->TryGetNumberField(
					TEXT("minimum"),
					Minimum);
				Input->SetNumberField(
					Name,
					FMath::Max(1.0, Minimum));
			}
		}
		return Input;
	}

	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectPluginsDir(),
				TEXT(
					"UeAgentInterface/Source/"
					"UeAgentInterfaceTests/Private/"
					"DomainAdapters/ModelingGeometryEffect/"
					"Fixtures/"
					"modeling_geometry_effect_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load ModelingGeometryEffect fixture"),
				FFileHelper::LoadFileToString(
					Text,
					*FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse ModelingGeometryEffect fixture"),
				FJsonSerializer::Deserialize(
					Reader,
					Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	FUeAgentContractValue ContractValue(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Value,
		const FString& Label)
	{
		FString Canonical;
		FString Error;
		Test->TestTrue(
			*Label,
			Json::TryCanonicalize(
				Value,
				Canonical,
				Error));
		if (Canonical.IsEmpty())
		{
			Test->AddError(Label + TEXT(":") + Error);
		}
		return FUeAgentContractValue(MoveTemp(Canonical));
	}

	bool TryCompileSchema(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& SchemaObject,
		const FString& Label,
		TSharedPtr<const FUeAgentContractSchema>& OutSchema)
	{
		OutSchema.Reset();
		FUeAgentContractSchemaError Error;
		const bool bCompiled =
			FUeAgentContractSchema::TryCompile(
				ContractValue(
					Test,
					SchemaObject,
					Label + TEXT(" canonicalizes")),
				OutSchema,
				Error);
		Test->TestTrue(
			*(Label + TEXT(" compiles")),
			bCompiled);
		if (!bCompiled)
		{
			Test->AddError(
				Label + TEXT(":") + Error.Code
				+ TEXT(":") + Error.Detail);
		}
		return bCompiled && OutSchema.IsValid();
	}

	FString ComputeDescriptorHash(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Row,
		const TSharedPtr<FJsonObject>& InputSchema,
		const TSharedPtr<FJsonObject>& ResultSchema)
	{
		const TSharedPtr<FJsonObject> Policy =
			MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> StatePlanes;
		StatePlanes.Add(
			MakeShared<FJsonValueString>(
				TEXT("authoring_memory")));
		Policy->SetArrayField(
			TEXT("allowed_state_planes"),
			MoveTemp(StatePlanes));
		TArray<TSharedPtr<FJsonValue>> Facets;
		Facets.Add(
			MakeShared<FJsonValueString>(TEXT("level")));
		Facets.Add(
			MakeShared<FJsonValueString>(TEXT("world")));
		Policy->SetArrayField(
			TEXT("required_facets"),
			MoveTemp(Facets));
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
			Row->GetStringField(TEXT("capability_id")));
		Descriptor->SetStringField(
			TEXT("descriptor_id"),
			Row->GetStringField(TEXT("descriptor_id")));
		Descriptor->SetStringField(
			TEXT("domain_id"),
			TEXT("mesh"));
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
		FString Error;
		Test->TestTrue(
			TEXT("descriptor canonicalizes"),
			Json::TryCanonicalize(
				Descriptor,
				Canonical,
				Error));
		return Json::Sha256Utf8(Canonical);
	}

	bool ContainsForbiddenField(
		const TSharedPtr<FJsonObject>& Schema,
		const TSet<FString>& Forbidden,
		FString& OutField)
	{
		TArray<TSharedPtr<FJsonObject>> Pending;
		Pending.Add(Schema);
		while (!Pending.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Current =
				Pending.Pop(EAllowShrinking::No);
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Current->TryGetObjectField(
					TEXT("properties"),
					Properties)
				&& Properties
				&& Properties->IsValid())
			{
				for (const TPair<
					FString,
					TSharedPtr<FJsonValue>>& Pair
						: (*Properties)->Values)
				{
					if (Forbidden.Contains(Pair.Key))
					{
						OutField = Pair.Key;
						return true;
					}
					if (Pair.Value.IsValid()
						&& Pair.Value->Type == EJson::Object)
					{
						Pending.Add(Pair.Value->AsObject());
					}
				}
			}
			const TSharedPtr<FJsonObject>* Items = nullptr;
			if (Current->TryGetObjectField(
					TEXT("items"),
					Items)
				&& Items
				&& Items->IsValid())
			{
				Pending.Add(*Items);
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryEffectCapabilityContractTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E0."
		"CapabilityContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryEffectCapabilityContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.modeling_geometry_effect."
			"test_fixture.v1")));
	const int32 DescriptorCount = static_cast<int32>(
		Fixture->GetNumberField(TEXT("descriptor_count")));
	TestEqual(TEXT("31 formal descriptors"), DescriptorCount, 31);

	const TArray<TSharedPtr<FJsonValue>>& Rows =
		Fixture->GetArrayField(TEXT("descriptors"));
	const TArray<TSharedPtr<FJsonValue>>& Scenarios =
		Fixture->GetArrayField(TEXT("e2_scenarios"));
	const TArray<TSharedPtr<FJsonValue>>& LegacySources =
		Fixture->GetArrayField(TEXT("legacy_sources"));
	TestEqual(
		TEXT("descriptor rows are exact"),
		Rows.Num(),
		DescriptorCount);
	TestEqual(
		TEXT("every descriptor has an E2 scenario"),
		Scenarios.Num(),
		DescriptorCount);
	TestEqual(
		TEXT("every legacy command maps once"),
		LegacySources.Num(),
		DescriptorCount);

	TSet<FString> AllowedTypes;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(
			TEXT("native_operation_types")))
	{
		AllowedTypes.Add(Value->AsString());
	}
	TestEqual(
		TEXT("four typed operation classes"),
		AllowedTypes.Num(),
		4);

	TSet<FString> Forbidden;
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(
			TEXT("forbidden_input_fields")))
	{
		Forbidden.Add(Value->AsString());
	}

	const TSharedPtr<FJsonObject> ResultSchema =
		BuildResultSchema();
	TSharedPtr<const FUeAgentContractSchema> CompiledResult;
	TryCompileSchema(
		this,
		ResultSchema,
		TEXT("shared result schema"),
		CompiledResult);

	TMap<FString, FString> HashesByDescriptor;
	TMap<FString, FString> CapabilitiesByDescriptor;
	TMap<FString, FString> TypeByDescriptor;
	TSet<FString> CapabilityIds;
	TMap<FString, int32> CountsByType;
	int32 StaticCarrierCases = 0;
	int32 DynamicCarrierCases = 0;
	for (const TSharedPtr<FJsonValue>& Value : Rows)
	{
		const TSharedPtr<FJsonObject> Row =
			Value->AsObject();
		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FString CapabilityId =
			Row->GetStringField(TEXT("capability_id"));
		const FString NativeType =
			Row->GetStringField(
				TEXT("native_operation_type"));

		TestTrue(
			*(DescriptorId + TEXT(" uses the mesh namespace")),
			DescriptorId.StartsWith(
				TEXT("uai.operation.mesh.")));
		TestTrue(
			*(DescriptorId + TEXT(" maps to one typed class")),
			AllowedTypes.Contains(NativeType));
		TestFalse(
			*(DescriptorId + TEXT(" has no legacy identity")),
			Row->HasField(TEXT("legacy_command_id"))
				|| Row->HasField(TEXT("command"))
				|| Row->HasField(TEXT("route")));
		TestFalse(
			*(DescriptorId + TEXT(" is unique")),
			HashesByDescriptor.Contains(DescriptorId));
		TestFalse(
			*(CapabilityId + TEXT(" is unique")),
			CapabilityIds.Contains(CapabilityId));
		FString TestCarrierKind;
		if (Row->TryGetStringField(
				TEXT("test_carrier_kind"),
				TestCarrierKind))
		{
			TestEqual(
				*(DescriptorId
					+ TEXT(" uses the supported static carrier")),
				TestCarrierKind,
				FString(TEXT("static_mesh_asset")));
			++StaticCarrierCases;
		}
		else
		{
			++DynamicCarrierCases;
		}

		const TSharedPtr<FJsonObject> InputSchema =
			BuildInputSchema(this, Row);
		TSharedPtr<const FUeAgentContractSchema> CompiledInput;
		if (!TryCompileSchema(
				this,
				InputSchema,
				DescriptorId + TEXT(" input schema"),
				CompiledInput))
		{
			continue;
		}

		FString ForbiddenField;
		TestFalse(
			*(DescriptorId
				+ TEXT(" has no cross-owner field")),
			ContainsForbiddenField(
				InputSchema,
				Forbidden,
				ForbiddenField));
		if (!ForbiddenField.IsEmpty())
		{
			AddError(
				TEXT("modeling_geometry_forbidden_field:")
				+ DescriptorId + TEXT(":")
				+ ForbiddenField);
		}

		const TSharedPtr<FJsonObject> ValidInput =
			BuildValidInput(Row);
		TOptional<FUeAgentContractValue> Canonical;
		FUeAgentContractSchemaError ValidationError;
		const bool bAcceptedValidInput = TestTrue(
			*(DescriptorId + TEXT(" accepts valid input")),
			CompiledInput->TryValidateAndCanonicalize(
				ContractValue(
					this,
					ValidInput,
					DescriptorId
						+ TEXT(" valid input canonicalizes")),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				Canonical,
				ValidationError));
		if (!bAcceptedValidInput)
		{
			AddError(
				TEXT("modeling_geometry_valid_fixture_rejected:")
					+ DescriptorId + TEXT(":")
					+ ValidationError.Code + TEXT(":")
					+ ValidationError.InstancePath + TEXT(":")
					+ ValidationError.SchemaPath + TEXT(":")
					+ ValidationError.Detail);
		}
		TestTrue(
			*(DescriptorId
				+ TEXT(" produces canonical input")),
			Canonical.IsSet() && Canonical->IsValid());

		const TSharedPtr<FJsonObject> InvalidInput =
			MakeShared<FJsonObject>();
		InvalidInput->Values = ValidInput->Values;
		InvalidInput->RemoveField(TEXT("target_ref"));
		Canonical.Reset();
		ValidationError = {};
		TestFalse(
			*(DescriptorId
				+ TEXT(" rejects missing target_ref")),
			CompiledInput->TryValidateAndCanonicalize(
				ContractValue(
					this,
					InvalidInput,
					DescriptorId
						+ TEXT(" invalid input canonicalizes")),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				Canonical,
				ValidationError));
		TestTrue(
			*(DescriptorId
				+ TEXT(" returns typed schema error")),
			ValidationError.IsValid());

		HashesByDescriptor.Add(
			DescriptorId,
			ComputeDescriptorHash(
				this,
				Row,
				InputSchema,
				ResultSchema));
		CapabilitiesByDescriptor.Add(
			DescriptorId,
			CapabilityId);
		TypeByDescriptor.Add(DescriptorId, NativeType);
		CapabilityIds.Add(CapabilityId);
		CountsByType.FindOrAdd(NativeType) += 1;
	}
	TestEqual(
		TEXT("primitive count"),
		CountsByType.FindRef(
			TEXT("geometry.primitive.create")),
		7);
	TestEqual(
		TEXT("topology count"),
		CountsByType.FindRef(
			TEXT("geometry.topology.edit")),
		17);
	TestEqual(
		TEXT("attribute count"),
		CountsByType.FindRef(
			TEXT("geometry.attribute.edit")),
		5);
	TestEqual(
		TEXT("collision count"),
		CountsByType.FindRef(
			TEXT("geometry.collision.edit")),
		2);
	TestEqual(
		TEXT("31 exact descriptor hashes"),
		HashesByDescriptor.Num(),
		DescriptorCount);
	TestEqual(
		TEXT("four static-mesh carrier cases"),
		StaticCarrierCases,
		4);
	TestEqual(
		TEXT("remaining dynamic-mesh actor cases"),
		DynamicCarrierCases,
		27);

	TSet<FString> ScenarioDescriptors;
	for (const TSharedPtr<FJsonValue>& Value : Scenarios)
	{
		const TSharedPtr<FJsonObject> Scenario =
			Value->AsObject();
		const FString DescriptorId =
			Scenario->GetStringField(TEXT("descriptor_id"));
		TestTrue(
			*(DescriptorId + TEXT(" scenario is unique")),
			!ScenarioDescriptors.Contains(DescriptorId));
		ScenarioDescriptors.Add(DescriptorId);
		TestTrue(
			*(DescriptorId + TEXT(" scenario is formal")),
			HashesByDescriptor.Contains(DescriptorId));
		TestFalse(
			*(DescriptorId + TEXT(" names a direct API family")),
			Scenario->GetStringField(
				TEXT("direct_api_family")).IsEmpty());
		TestFalse(
			*(DescriptorId
				+ TEXT(" has an independent oracle")),
			Scenario->GetStringField(
				TEXT("oracle_kind")).IsEmpty());
	}
	TestEqual(
		TEXT("31 unique E2 scenarios"),
		ScenarioDescriptors.Num(),
		DescriptorCount);

	TSet<FString> LegacyNames;
	TSet<FString> LegacyDescriptors;
	for (const TSharedPtr<FJsonValue>& Value : LegacySources)
	{
		const TSharedPtr<FJsonObject> Legacy =
			Value->AsObject();
		const FString Source =
			Legacy->GetStringField(TEXT("source"));
		const FString DescriptorId =
			Legacy->GetStringField(TEXT("descriptor_id"));
		TestTrue(
			*(Source + TEXT(" is unique")),
			!LegacyNames.Contains(Source));
		TestTrue(
			*(DescriptorId + TEXT(" has one legacy source")),
			!LegacyDescriptors.Contains(DescriptorId));
		LegacyNames.Add(Source);
		LegacyDescriptors.Add(DescriptorId);
		TestTrue(
			*(Source + TEXT(" is only migration evidence")),
			Source.StartsWith(TEXT("modeling_"))
				&& HashesByDescriptor.Contains(DescriptorId)
				&& Legacy->GetStringField(TEXT("terminal"))
					== TEXT("REPLACE"));
	}
	TestEqual(
		TEXT("31 legacy names map one-to-one"),
		LegacyNames.Num(),
		DescriptorCount);

	const TArray<TSharedPtr<FJsonValue>>& NegativeCases =
		Fixture->GetArrayField(TEXT("e1_negative_cases"));
	TSet<FString> NegativeIds;
	for (const TSharedPtr<FJsonValue>& Value : NegativeCases)
	{
		const TSharedPtr<FJsonObject> Case =
			Value->AsObject();
		const FString CaseId =
			Case->GetStringField(TEXT("case_id"));
		TestTrue(
			*(CaseId + TEXT(" is unique")),
			!NegativeIds.Contains(CaseId));
		NegativeIds.Add(CaseId);
		TestTrue(
			*(CaseId + TEXT(" targets a formal descriptor")),
			HashesByDescriptor.Contains(
				Case->GetStringField(TEXT("descriptor_id"))));
		TestEqual(
			*(CaseId + TEXT(" fails before mutation")),
			static_cast<int32>(
				Case->GetNumberField(
					TEXT("mutation_count"))),
			0);
		TestFalse(
			*(CaseId + TEXT(" has a stable error")),
			Case->GetStringField(
				TEXT("expected_error")).IsEmpty());
	}
	TestEqual(
		TEXT("14 semantic negative cases"),
		NegativeIds.Num(),
		14);

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	const bool bCreated =
		UE::UAI::DomainAdapters::ModelingGeometryEffect::
			TryCreateModelingGeometryEffectDescriptorRegistry(
				Registry,
				RegistryError);
	TestTrue(
		TEXT("create production ModelingGeometryEffect registry"),
		bCreated);
	if (!bCreated)
	{
		AddError(
			TEXT("modeling_geometry_registry_failed:")
			+ RegistryError);
	}
	if (Registry.IsValid())
	{
		TestEqual(
			TEXT("production registry has 31 descriptors"),
			Registry->GetDescriptors().Num(),
			DescriptorCount);
		for (const TPair<FString, FString>& Pair
			: HashesByDescriptor)
		{
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(Pair.Key);
			TestTrue(
				*(Pair.Key
					+ TEXT(" is an exact production operation")),
				Descriptor
					&& Descriptor->GetOperationKind()
						== EUeAgentDescriptorOperationKind::
							Operation
					&& !Descriptor->GetQueryBinding().IsSet()
					&& Descriptor->GetCapabilityId()
						== CapabilitiesByDescriptor[Pair.Key]
					&& Descriptor->GetDomainId()
						== TEXT("mesh")
					&& Descriptor->GetIdentity()
							.GetDescriptorHash()
						== Pair.Value);
		}
	}

	return true;
}
