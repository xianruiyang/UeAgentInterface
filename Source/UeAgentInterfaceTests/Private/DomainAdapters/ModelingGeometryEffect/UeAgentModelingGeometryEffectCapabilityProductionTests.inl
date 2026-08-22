using namespace UE::UAI::ContextResource;
using namespace UE::UAI::Contract;
using namespace UE::UAI::Discovery;
using namespace UE::UAI::DomainAdapters::GenericEffect;
using namespace UE::UAI::DomainAdapters::ModelingGeometryEffect;
using namespace UE::UAI::ExecutionMutation;
using namespace UE::UAI::PlanningPreflight;
using namespace UE::UAI::StateSemantic;

TArray<TSharedPtr<FJsonValue>> ModelingNumberValues(
	const std::initializer_list<double> Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const double Value : Values)
	{
		Result.Add(MakeShared<FJsonValueNumber>(Value));
	}
	return Result;
}

TSharedPtr<FJsonObject> ModelingRevision(
	const FString& SourceId,
	const int64 Sequence = 1)
{
	const TSharedPtr<FJsonObject> Revision = MakeShared<FJsonObject>();
	Revision->SetStringField(TEXT("source_id"), SourceId);
	Revision->SetStringField(
		TEXT("generation"),
		TEXT("fixture-generation"));
	Revision->SetNumberField(
		TEXT("sequence"),
		static_cast<double>(Sequence));
	return Revision;
}

TSharedPtr<FJsonObject> ModelingPlacement()
{
	const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(
		TEXT("semantic_frame_id"),
		TEXT("ue.world"));
	Value->SetStringField(TEXT("write_frame_id"), TEXT("ue.world"));
	Value->SetStringField(TEXT("read_frame_id"), TEXT("ue.world"));
	Value->SetStringField(TEXT("verify_frame_id"), TEXT("ue.world"));
	Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
	Value->SetArrayField(
		TEXT("translation"),
		ModelingNumberValues({100.0, 200.0, 300.0}));
	Value->SetArrayField(
		TEXT("rotation_quaternion_xyzw"),
		ModelingNumberValues({0.0, 0.0, 0.0, 1.0}));
	Value->SetArrayField(
		TEXT("scale"),
		ModelingNumberValues({1.0, 2.0, 3.0}));
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

TSharedPtr<FJsonObject> ModelingSpaceContract()
{
	const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
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
			"semantic -> C4 normalized target local -> working mesh -> "
			"carrier -> independent target-local readback"));
	return Value;
}

TSharedPtr<FJsonObject> ModelingOperandTransform()
{
	const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(
		TEXT("from_frame_id"),
		TEXT("ue.mesh.operand_local"));
	Value->SetStringField(
		TEXT("to_frame_id"),
		TEXT("ue.mesh.target_local"));
	Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
	Value->SetArrayField(
		TEXT("translation"),
		ModelingNumberValues({25.0, 0.0, 0.0}));
	Value->SetArrayField(
		TEXT("rotation_quaternion_xyzw"),
		ModelingNumberValues({0.0, 0.0, 0.0, 1.0}));
	Value->SetArrayField(
		TEXT("scale"),
		ModelingNumberValues({1.0, 1.0, 1.0}));
	Value->SetStringField(
		TEXT("basis_evidence"),
		TEXT("exact target and operand carrier transforms"));
	Value->SetStringField(
		TEXT("conversion_chain"),
		TEXT("operand local -> target local"));
	return Value;
}

TSharedPtr<FJsonObject> ModelingSelection(const FString& Kind)
{
	const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(TEXT("element_kind"), Kind);
	Value->SetArrayField(
		TEXT("element_ids"),
		ModelingNumberValues({0.0}));
	return Value;
}

TSharedPtr<FJsonObject> ModelingPlane()
{
	const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(
		TEXT("frame_id"),
		TEXT("ue.mesh.target_local"));
	Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
	Value->SetArrayField(
		TEXT("origin"),
		ModelingNumberValues({0.0, 0.0, 0.0}));
	Value->SetArrayField(
		TEXT("normal"),
		ModelingNumberValues({1.0, 0.0, 0.0}));
	Value->SetStringField(
		TEXT("basis_evidence"),
		TEXT("target-local plane from C4"));
	return Value;
}

TSharedPtr<FJsonObject> ModelingDirection()
{
	const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(
		TEXT("frame_id"),
		TEXT("ue.mesh.target_local"));
	Value->SetArrayField(
		TEXT("vector"),
		ModelingNumberValues({0.0, 0.0, 1.0}));
	Value->SetStringField(
		TEXT("basis_evidence"),
		TEXT("target-local direction from C4"));
	return Value;
}

TSharedPtr<FJsonObject> ModelingProjectionFrame()
{
	const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(
		TEXT("frame_id"),
		TEXT("ue.mesh.target_local"));
	Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
	Value->SetArrayField(
		TEXT("origin"),
		ModelingNumberValues({0.0, 0.0, 0.0}));
	Value->SetArrayField(
		TEXT("axis_u"),
		ModelingNumberValues({1.0, 0.0, 0.0}));
	Value->SetArrayField(
		TEXT("axis_v"),
		ModelingNumberValues({0.0, 1.0, 0.0}));
	Value->SetArrayField(
		TEXT("axis_w"),
		ModelingNumberValues({0.0, 0.0, 1.0}));
	Value->SetStringField(
		TEXT("basis_evidence"),
		TEXT("orthonormal target-local projection basis"));
	return Value;
}

TSharedPtr<FJsonObject> BuildModelingValidInput(
	const TSharedPtr<FJsonObject>& Row)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(
		TEXT("target_ref"),
		TEXT("uai://mesh/fixture-target"));

	bool bValue = false;
	if (Row->TryGetBoolField(TEXT("existing_target"), bValue) && bValue)
	{
		Input->SetObjectField(
			TEXT("target_revision"),
			ModelingRevision(TEXT("uai://mesh/fixture-target")));
	}
	if (Row->TryGetBoolField(TEXT("placement"), bValue) && bValue)
	{
		Input->SetObjectField(TEXT("placement"), ModelingPlacement());
	}
	if (Row->TryGetBoolField(TEXT("space_contract"), bValue) && bValue)
	{
		Input->SetObjectField(
			TEXT("space_contract"),
			ModelingSpaceContract());
	}
	FString SelectionKind;
	if (Row->TryGetStringField(TEXT("selection_kind"), SelectionKind))
	{
		Input->SetObjectField(
			TEXT("selection"),
			ModelingSelection(SelectionKind));
	}
	if (Row->TryGetBoolField(TEXT("operand"), bValue) && bValue)
	{
		Input->SetStringField(
			TEXT("operand_ref"),
			TEXT("uai://mesh/fixture-operand"));
		Input->SetObjectField(
			TEXT("operand_revision"),
			ModelingRevision(TEXT("uai://mesh/fixture-operand")));
		Input->SetObjectField(
			TEXT("operand_to_target"),
			ModelingOperandTransform());
	}
	if (Row->TryGetBoolField(TEXT("plane"), bValue) && bValue)
	{
		Input->SetObjectField(TEXT("plane"), ModelingPlane());
	}
	if (Row->TryGetBoolField(TEXT("direction"), bValue) && bValue)
	{
		Input->SetObjectField(
			TEXT("direction"),
			ModelingDirection());
	}
	if (Row->TryGetBoolField(TEXT("projection_frame"), bValue) && bValue)
	{
		Input->SetObjectField(
			TEXT("projection_frame"),
			ModelingProjectionFrame());
	}
	if (Row->TryGetBoolField(TEXT("edge_pairs"), bValue) && bValue)
	{
		TArray<TSharedPtr<FJsonValue>> Pair;
		Pair.Add(MakeShared<FJsonValueNumber>(0));
		Pair.Add(MakeShared<FJsonValueNumber>(1));
		TArray<TSharedPtr<FJsonValue>> Pairs;
		Pairs.Add(MakeShared<FJsonValueArray>(MoveTemp(Pair)));
		Input->SetArrayField(TEXT("edge_pairs"), MoveTemp(Pairs));
	}

	for (const TSharedPtr<FJsonValue>& Value
		: Row->GetArrayField(TEXT("parameters")))
	{
		const TSharedPtr<FJsonObject> Spec = Value->AsObject();
		const FString Name = Spec->GetStringField(TEXT("name"));
		const FString Kind = Spec->GetStringField(TEXT("kind"));
		if (Kind == TEXT("string"))
		{
			const FString StringValue =
				Name == TEXT("material_ref")
						? TEXT("uai://material/default")
						: TEXT("fixture");
			Input->SetStringField(Name, StringValue);
		}
		else if (Kind == TEXT("boolean"))
		{
			Input->SetBoolField(Name, true);
		}
		else if (Kind == TEXT("enum"))
		{
			Input->SetStringField(
				Name,
				Spec->GetArrayField(TEXT("values"))[0]->AsString());
		}
		else if (Kind.EndsWith(TEXT("_array")))
		{
			const int32 Count = static_cast<int32>(
				Spec->GetNumberField(TEXT("count")));
			double Minimum = 1.0;
			Spec->TryGetNumberField(TEXT("minimum"), Minimum);
			TArray<TSharedPtr<FJsonValue>> Items;
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
			Spec->TryGetNumberField(TEXT("minimum"), Minimum);
			double Number = FMath::Max(1.0, Minimum);
			if (Name == TEXT("slot_index"))
			{
				Number = 1.0;
			}
			Input->SetNumberField(Name, Number);
		}
	}
	return Input;
}

void PatchModelingHighSignalInput(
	const TSharedPtr<FJsonObject>& Row,
	const TSharedPtr<FJsonObject>& Input)
{
	const FString OperationKey =
		Row->GetStringField(TEXT("operation_key"));
	if (OperationKey == TEXT("box"))
	{
		Input->SetArrayField(
			TEXT("size_cm"),
			ModelingNumberValues({120.0, 80.0, 40.0}));
		Input->SetArrayField(
			TEXT("subdivisions"),
			ModelingNumberValues({2.0, 3.0, 4.0}));
		Input->SetStringField(TEXT("anchor"), TEXT("base_center"));
	}
	else if (OperationKey == TEXT("cylinder"))
	{
		Input->SetNumberField(TEXT("radius_cm"), 35.0);
		Input->SetNumberField(TEXT("height_cm"), 90.0);
		Input->SetNumberField(TEXT("radial_segments"), 12.0);
		Input->SetNumberField(TEXT("height_segments"), 3.0);
		Input->SetBoolField(TEXT("cap_ends"), true);
		Input->SetStringField(TEXT("anchor"), TEXT("base_center"));
	}
	else if (OperationKey == TEXT("plane"))
	{
		Input->SetArrayField(
			TEXT("size_cm"),
			ModelingNumberValues({120.0, 80.0}));
		Input->SetArrayField(
			TEXT("subdivisions"),
			ModelingNumberValues({3.0, 2.0}));
		Input->SetBoolField(TEXT("double_sided"), true);
	}
	else if (OperationKey == TEXT("ramp"))
	{
		Input->SetArrayField(
			TEXT("size_cm"),
			ModelingNumberValues({120.0, 80.0, 40.0}));
		Input->SetStringField(TEXT("slope_axis"), TEXT("x"));
		Input->SetStringField(TEXT("rise_direction"), TEXT("negative"));
	}
	else if (OperationKey == TEXT("ramp_corner"))
	{
		Input->SetArrayField(
			TEXT("size_cm"),
			ModelingNumberValues({120.0, 80.0, 40.0}));
		Input->SetStringField(
			TEXT("high_corner"),
			TEXT("negative_x_negative_y"));
	}
	else if (OperationKey == TEXT("sphere"))
	{
		Input->SetNumberField(TEXT("radius_cm"), 50.0);
		Input->SetNumberField(TEXT("longitude_segments"), 12.0);
		Input->SetNumberField(TEXT("latitude_segments"), 8.0);
	}
	else if (OperationKey == TEXT("stairs"))
	{
		Input->SetNumberField(TEXT("step_count"), 4.0);
		Input->SetNumberField(TEXT("step_width_cm"), 100.0);
		Input->SetNumberField(TEXT("step_depth_cm"), 30.0);
		Input->SetNumberField(TEXT("step_height_cm"), 20.0);
		Input->SetStringField(TEXT("rise_axis"), TEXT("x"));
		Input->SetStringField(TEXT("rise_direction"), TEXT("negative"));
	}
	else if (OperationKey == TEXT("bevel_edges"))
	{
		Input->SetNumberField(TEXT("distance_cm"), 7.0);
		Input->SetNumberField(TEXT("segments"), 2.0);
		Input->SetNumberField(TEXT("roundness"), 0.5);
	}
	else if (OperationKey == TEXT("duplicate_faces"))
	{
		Input->SetArrayField(
			TEXT("translation_cm"),
			ModelingNumberValues({0.0, 0.0, 125.0}));
	}
	else if (OperationKey == TEXT("extrude_faces"))
	{
		Input->SetNumberField(TEXT("distance_cm"), 25.0);
		Input->SetStringField(
			TEXT("direction_mode"),
			TEXT("fixed"));
	}
	else if (OperationKey == TEXT("inset_faces"))
	{
		Input->SetNumberField(TEXT("distance_cm"), 15.0);
	}
	else if (OperationKey == TEXT("offset_faces"))
	{
		Input->SetNumberField(TEXT("distance_cm"), 20.0);
		Input->SetStringField(
			TEXT("offset_mode"),
			TEXT("face_normals"));
	}
	else if (OperationKey == TEXT("push_pull_faces"))
	{
		Input->SetNumberField(TEXT("distance_cm"), 30.0);
		Input->SetStringField(
			TEXT("direction_mode"),
			TEXT("fixed"));
	}
	else if (OperationKey == TEXT("plane_cut"))
	{
		Input->SetStringField(TEXT("keep_side"), TEXT("both"));
		Input->SetBoolField(TEXT("fill_hole"), true);
	}
	else if (OperationKey == TEXT("remesh"))
	{
		Input->SetNumberField(
			TEXT("target_edge_length_cm"),
			25.0);
		Input->SetNumberField(TEXT("iterations"), 5.0);
	}
	else if (OperationKey == TEXT("simplify"))
	{
		Input->SetStringField(
			TEXT("target_mode"),
			TEXT("triangle_ratio"));
		Input->SetNumberField(TEXT("target_value"), 0.25);
	}
	else if (OperationKey == TEXT("subdivide"))
	{
		Input->SetStringField(TEXT("scheme"), TEXT("uniform"));
		Input->SetNumberField(TEXT("levels"), 2.0);
	}
	else if (OperationKey == TEXT("voxel_boolean"))
	{
		Input->SetStringField(TEXT("operation"), TEXT("union"));
		Input->SetNumberField(TEXT("voxel_size_cm"), 10.0);
		Input->SetNumberField(TEXT("adaptivity"), 0.25);
	}
	else if (OperationKey == TEXT("weld_edges"))
	{
		Input->SetNumberField(TEXT("tolerance_cm"), 0.1);
	}
	else if (OperationKey == TEXT("add_material_slot"))
	{
		Input->SetStringField(
			TEXT("slot_name"),
			TEXT("SA003I_Slot"));
	}
	else if (OperationKey == TEXT("auto_uv"))
	{
		Input->SetNumberField(TEXT("uv_channel"), 2.0);
		Input->SetBoolField(TEXT("pack"), true);
		Input->SetNumberField(
			TEXT("texture_resolution"),
			512.0);
	}
	else if (OperationKey == TEXT("project_uv"))
	{
		Input->SetNumberField(TEXT("uv_channel"), 2.0);
		Input->SetArrayField(
			TEXT("dimensions_cm"),
			ModelingNumberValues({200.0, 100.0, 50.0}));
	}
	else if (OperationKey == TEXT("recompute_normals"))
	{
		Input->SetStringField(
			TEXT("mode"),
			TEXT("area_angle_weighted"));
		Input->SetBoolField(
			TEXT("preserve_existing_hard_edges"),
			false);
	}
	else if (OperationKey == TEXT("assign_material_slot"))
	{
		Input->SetNumberField(TEXT("slot_index"), 1.0);
	}
	else if (OperationKey == TEXT("generate_convex_collision"))
	{
		Input->SetStringField(
			TEXT("mode"),
			TEXT("decomposition"));
		Input->SetNumberField(TEXT("max_hulls"), 4.0);
		Input->SetBoolField(TEXT("replace_existing"), true);
	}
	else if (OperationKey == TEXT("generate_simple_collision"))
	{
		Input->SetStringField(
			TEXT("method"),
			TEXT("aligned_boxes"));
		Input->SetNumberField(TEXT("max_shapes"), 4.0);
		Input->SetBoolField(TEXT("replace_existing"), true);
	}
}

bool SerializeModelingInput(
	FAutomationTestBase* Test,
	const TSharedPtr<FJsonObject>& Input,
	FString& OutJson)
{
	OutJson.Reset();
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&OutJson);
	return Test->TestTrue(
		TEXT("serialize ModelingGeometry canonical input"),
		FJsonSerializer::Serialize(Input.ToSharedRef(), Writer));
}

TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
MakeModelingResources()
{
	return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
		FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1048576},
			{EUeAgentResourceDimension::OutputBytes, 1048576},
			{EUeAgentResourceDimension::StringBytes, 1048576},
			{EUeAgentResourceDimension::Nodes, 32768},
			{EUeAgentResourceDimension::Items, 32768},
			{EUeAgentResourceDimension::Files, 64},
			{EUeAgentResourceDimension::ReferenceEdges, 32768},
			{EUeAgentResourceDimension::WorkUnits, 1048576},
			{
				EUeAgentResourceDimension::EstimatedMemoryBytes,
				134217728,
			},
		}));
}

bool TryModelingCanonicalValue(
	const TSharedPtr<FJsonValue>& JsonValue,
	FUeAgentResourceTracker& Resources,
	FUeAgentCanonicalValuePtr& OutValue,
	FUeAgentStateSemanticError& OutError)
{
	OutValue.Reset();
	OutError = {};
	if (!JsonValue.IsValid())
	{
		return false;
	}
	switch (JsonValue->Type)
	{
	case EJson::Null:
		return FUeAgentCanonicalValue::TryNull(
			Resources,
			OutValue,
			OutError);
	case EJson::Boolean:
		return FUeAgentCanonicalValue::TryBoolean(
			JsonValue->AsBool(),
			Resources,
			OutValue,
			OutError);
	case EJson::String:
		return FUeAgentCanonicalValue::TryString(
			JsonValue->AsString(),
			Resources,
			OutValue,
			OutError);
	case EJson::Number:
		return FUeAgentCanonicalValue::TryDecimal(
			FString::SanitizeFloat(JsonValue->AsNumber()),
			Resources,
			OutValue,
			OutError);
	case EJson::Array:
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const TSharedPtr<FJsonValue>& Item
				: JsonValue->AsArray())
			{
				FUeAgentCanonicalValuePtr CanonicalItem;
				if (!TryModelingCanonicalValue(
						Item,
						Resources,
						CanonicalItem,
						OutError))
				{
					return false;
				}
				Items.Add(CanonicalItem.ToSharedRef());
			}
			return FUeAgentCanonicalValue::TryArray(
				Items,
				Resources,
				OutValue,
				OutError);
		}
	case EJson::Object:
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: JsonValue->AsObject()->Values)
			{
				FUeAgentCanonicalValuePtr CanonicalItem;
				if (!TryModelingCanonicalValue(
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
			return FUeAgentCanonicalValue::TryObject(
				Items,
				Resources,
				OutValue,
				OutError);
		}
	default:
		return false;
	}
}

void SetIntegerArray(
	const TSharedPtr<FJsonObject>& Object,
	const FString& Name,
	const TArray<int32>& Values)
{
	TArray<TSharedPtr<FJsonValue>> JsonValues;
	for (const int32 Value : Values)
	{
		JsonValues.Add(MakeShared<FJsonValueNumber>(Value));
	}
	Object->SetArrayField(Name, MoveTemp(JsonValues));
}

TSharedPtr<FJsonObject> MakeModelingMeshState(
	UDynamicMesh& DynamicMesh,
	const FString& TargetRef,
	const TSharedPtr<FJsonObject>& Revision,
	const FString& CarrierKind,
	const bool bDeclareBoundaryEdges)
{
	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("target_ref"), TargetRef);
	State->SetObjectField(TEXT("target_revision"), Revision);
	State->SetStringField(TEXT("carrier_kind"), CarrierKind);

	TArray<int32> VertexIds;
	TArray<int32> EdgeIds;
	TArray<int32> BoundaryEdgeIds;
	TArray<int32> TriangleIds;
	FString Fingerprint;
	DynamicMesh.ProcessMesh(
		[&] (const FDynamicMesh3& Mesh)
		{
			int64 Checksum = 0;
			for (const int32 VertexId : Mesh.VertexIndicesItr())
			{
				VertexIds.Add(VertexId);
				const FVector3d Position = Mesh.GetVertex(VertexId);
				Checksum +=
					FMath::RoundToInt64(Position.X * 100.0)
					+ FMath::RoundToInt64(Position.Y * 1000.0)
					+ FMath::RoundToInt64(Position.Z * 10000.0);
			}
			for (const int32 EdgeId : Mesh.EdgeIndicesItr())
			{
				EdgeIds.Add(EdgeId);
				if (Mesh.IsBoundaryEdge(EdgeId))
				{
					BoundaryEdgeIds.Add(EdgeId);
				}
			}
			for (const int32 TriangleId : Mesh.TriangleIndicesItr())
			{
				TriangleIds.Add(TriangleId);
			}
			Fingerprint = FString::Printf(
				TEXT("mesh:v=%d:t=%d:e=%d:c=%lld"),
				Mesh.VertexCount(),
				Mesh.TriangleCount(),
				Mesh.EdgeCount(),
				Checksum);
		});
	if (bDeclareBoundaryEdges && BoundaryEdgeIds.Num() < 2)
	{
		BoundaryEdgeIds = {0, 1};
	}
	State->SetStringField(
		TEXT("geometry_fingerprint"),
		Fingerprint);
	SetIntegerArray(State, TEXT("vertex_ids"), VertexIds);
	SetIntegerArray(State, TEXT("edge_ids"), EdgeIds);
	SetIntegerArray(
		State,
		TEXT("boundary_edge_ids"),
		BoundaryEdgeIds);
	SetIntegerArray(State, TEXT("triangle_ids"), TriangleIds);
	State->SetNumberField(TEXT("material_slot_count"), 2);
	return State;
}

struct FModelingSnapshotOptions
{
	bool bIncludeTarget = true;
	bool bIncludeOperand = false;
	bool bDeclareBoundaryEdges = true;
	FString CarrierKind = TEXT("dynamic_mesh_actor");
};

FUeAgentCanonicalSnapshotPtr MakeModelingSnapshot(
	FAutomationTestBase* Test,
	const FUeAgentOperationDescriptor& Descriptor,
	FOwnedDynamicMeshWorld& Owner,
	ADynamicMeshActor* Operand,
	const FModelingSnapshotOptions& Options,
	FUeAgentResourceTracker& Resources)
{
	const FUeAgentStateRevision SnapshotRevision(
		TEXT("modeling-geometry-e1"),
		TEXT("owned-worker:r1"),
		1);
	TArray<FUeAgentStateEntry> Entries;

	auto AppendState =
		[&](
			ADynamicMeshActor& Actor,
			const FString& Ref,
			const int64 RevisionSequence,
			const FString& CarrierKind,
			const bool bDeclareBoundaryEdges)
		{
			UDynamicMeshComponent* Component =
				Actor.GetDynamicMeshComponent();
			UDynamicMesh* DynamicMesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			if (!DynamicMesh)
			{
				return false;
			}
			const TSharedPtr<FJsonObject> StateObject =
				MakeModelingMeshState(
					*DynamicMesh,
					Ref,
					ModelingRevision(Ref, RevisionSequence),
					CarrierKind,
					bDeclareBoundaryEdges);
			FUeAgentCanonicalValuePtr Current;
			FUeAgentStateSemanticError ValueError;
			if (!TryModelingCanonicalValue(
					MakeShared<FJsonValueObject>(StateObject),
					Resources,
					Current,
					ValueError))
			{
				return false;
			}
			FUeAgentCanonicalValuePtr Default;
			if (!FUeAgentCanonicalValue::TryObject(
					{},
					Resources,
					Default,
					ValueError))
			{
				return false;
			}
			const FString NativeTargetKind =
				Descriptor.GetCapabilityId().Contains(
					TEXT(".primitive."))
					? TEXT("geometry.primitive.create")
					: Descriptor.GetCapabilityId().Contains(
							TEXT(".topology."))
						? TEXT("geometry.topology.edit")
						: Descriptor.GetCapabilityId().Contains(
								TEXT(".attribute."))
							? TEXT("geometry.attribute.edit")
							: TEXT("geometry.collision.edit");
			Entries.Emplace(
				FUeAgentCanonicalIdentity(
					TEXT("uai.modeling.geometry:") + Ref,
					NativeTargetKind,
					Owner.World->GetPathName(),
					Ref,
					TEXT("modeling_geometry"),
					FString::Printf(
						TEXT("owned-world:%u"),
						Owner.World->GetUniqueID())),
				FUeAgentCanonicalType(
					TEXT("modeling_geometry_current_state"),
					EUeAgentValueKind::Object,
					false,
					TEXT("object")),
				Current.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::Schema,
					TEXT("modeling-geometry:e1:empty-default"),
					TEXT(
						"test-only default; current state is "
						"independent carrier readback"),
					Default.ToSharedRef()),
				FUeAgentStateDetails(
					Ref,
					TEXT(""),
					{TEXT("ModelingGeometryEffect")},
					true,
					TEXT("available"),
					{}),
				SnapshotRevision);
			return true;
		};

	if (Options.bIncludeTarget
		&& !AppendState(
			*Owner.Actor,
			TEXT("uai://mesh/fixture-target"),
			1,
			Options.CarrierKind,
			Options.bDeclareBoundaryEdges))
	{
		Test->AddError(TEXT("modeling_target_snapshot_failed"));
		return nullptr;
	}
	if (Options.bIncludeOperand
		&& (!Operand
			|| !AppendState(
				*Operand,
				TEXT("uai://mesh/fixture-operand"),
				1,
				TEXT("dynamic_mesh_actor"),
				false)))
	{
		Test->AddError(TEXT("modeling_operand_snapshot_failed"));
		return nullptr;
	}

	FUeAgentCanonicalSnapshotPtr Snapshot;
	FUeAgentStateSemanticError Error;
	if (!Test->TestTrue(
			TEXT("create independent ModelingGeometry snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				TEXT("snapshot:modeling-geometry:")
					+ Descriptor.GetCapabilityId(),
				EUeAgentStatePlane::AuthoringMemory,
				SnapshotRevision,
				FUeAgentTemporalPoint(
					TEXT("owned-worker-clock"),
					TEXT("1")),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					TEXT("owned-dynamic-mesh-native-api"),
					TEXT(
						"modeling-geometry:prestate-readback")),
				Entries,
				Resources,
				Snapshot,
				Error)))
	{
		Test->AddError(
			TEXT("modeling_snapshot_failed:") + Error.Code);
		return nullptr;
	}
	return Snapshot;
}

FUeAgentExecutionContextRefOwner MakeModelingExecutionContext(
	const FUeAgentDescriptorIdentity& Descriptor,
	UWorld& World)
{
	FUeAgentExecutionContextInput Input;
	Input.ProjectInstanceId = TEXT("project:modeling-geometry");
	Input.EditorInstanceId = TEXT("editor:owned-worker");
	Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
	Input.WorldType = EUeAgentExecutionWorldType::Preview;
	Input.WorldIdentity = World.GetPathName();
	Input.LevelIdentity = World.PersistentLevel->GetPathName();
	Input.ContextRevision = FString::Printf(
		TEXT("owned-world:%u"),
		World.GetUniqueID());
	Input.bResolvedFromCurrent = false;

	FUeAgentExecutionContextRefPtr Context;
	FUeAgentExecutionContextError Error;
	check(FUeAgentExecutionContextRef::TryCreate(
		Input,
		Descriptor,
		EUeAgentRequestKind::Operation,
		FUeAgentExecutionContextPolicy(
			{EUeAgentStatePlane::AuthoringMemory},
			{
				EUeAgentExecutionContextFacet::Level,
				EUeAgentExecutionContextFacet::World,
			},
			EUeAgentContextResolutionPolicy::ExplicitOnly,
			EUeAgentTransientContextPolicy::TemporaryMutationAllowed),
		Context,
		Error));
	return Context.ToSharedRef();
}

bool BuildModelingDraft(
	FAutomationTestBase* Test,
	const FUeAgentOperationDescriptor& Descriptor,
	UWorld& World,
	const TSharedPtr<FJsonObject>& InputObject,
	const FUeAgentCanonicalSnapshotRef& Snapshot,
	FUeAgentResourceTracker& Resources,
	TOptional<FChangeSetDraft>& OutDraft,
	FExecutionMutationError& OutError)
{
	FString InputJson;
	if (!SerializeModelingInput(Test, InputObject, InputJson))
	{
		return false;
	}
	TSharedPtr<const FUeAgentContractSchema> Compiled;
	FUeAgentContractSchemaError SchemaError;
	if (!Test->TestTrue(
			*(Descriptor.GetIdentity().GetDescriptorId()
				+ TEXT(" input schema compiles")),
			FUeAgentContractSchema::TryCompile(
				Descriptor.GetInputSchema(),
				Compiled,
				SchemaError))
		|| !Compiled.IsValid())
	{
		return false;
	}
	TOptional<FUeAgentContractValue> CanonicalPayload;
	const bool bCanonicalized =
		Compiled->TryValidateAndCanonicalize(
			FUeAgentContractValue(InputJson),
			[](int64,
				const FString&,
				FUeAgentContractSchemaError&)
			{
				return true;
			},
			CanonicalPayload,
			SchemaError);
	if (!Test->TestTrue(
			*(Descriptor.GetIdentity().GetDescriptorId()
				+ TEXT(" input canonicalizes once in C0")),
			bCanonicalized)
		|| !CanonicalPayload.IsSet())
	{
		if (!bCanonicalized)
		{
			Test->AddError(
				Descriptor.GetIdentity().GetDescriptorId()
				+ TEXT(":")
				+ SchemaError.Code + TEXT(":")
				+ SchemaError.InstancePath + TEXT(":")
				+ SchemaError.SchemaPath + TEXT(":")
				+ SchemaError.Keyword + TEXT(":")
				+ SchemaError.Detail);
		}
		return false;
	}
	const FUeAgentExecutionContextRefOwner Context =
		MakeModelingExecutionContext(
			Descriptor.GetIdentity(),
			World);
	return TryBuildModelingGeometryEffectChangeSetDraft(
		FUeAgentOperationRequest(
			Descriptor.GetIdentity(),
			TEXT("request:modeling-geometry"),
			TEXT("idempotency:modeling-geometry"),
			CanonicalPayload.GetValue()),
		*Context,
		Snapshot,
		Resources,
		OutDraft,
		OutError);
}

bool HasModelingCanonicalField(
	const FUeAgentCanonicalValuePtr& Value,
	const FString& Field)
{
	if (!Value.IsValid()
		|| Value->GetKind() != EUeAgentValueKind::Object)
	{
		return false;
	}
	for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
		: Value->GetObjectItems())
	{
		if (Item.Key == Field)
		{
			return true;
		}
	}
	return false;
}

const TSharedPtr<FJsonObject> FindModelingDescriptorRow(
	const TSharedPtr<FJsonObject>& Fixture,
	const FString& DescriptorId)
{
	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("descriptors")))
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (Row->GetStringField(TEXT("descriptor_id"))
			== DescriptorId)
		{
			return Row;
		}
	}
	return nullptr;
}

bool RunModelingProductionPlanningTests(
	FAutomationTestBase* Test,
	const TSharedPtr<FJsonObject>& Fixture)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!Test->TestTrue(
			TEXT("create ModelingGeometry production registry"),
			TryCreateModelingGeometryEffectDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		return false;
	}

	FOwnedDynamicMeshWorld Owner;
	if (!Test->TestTrue(
			TEXT("planning owns a valid mesh carrier"),
			Owner.IsValid()))
	{
		return false;
	}
	ADynamicMeshActor* Operand = Owner.SpawnCubeActor(
		TEXT("ModelingGeometryOwnedOperand"),
		FVector(1025.0, 2000.0, 300.0));
	if (!Test->TestNotNull(
			TEXT("planning owns an operand carrier"),
			Operand))
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("descriptors")))
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(DescriptorId);
		if (!Test->TestNotNull(
				*(DescriptorId + TEXT(" production descriptor exists")),
				Descriptor))
		{
			continue;
		}
		bool bPrimitive = false;
		Row->TryGetBoolField(TEXT("primitive_output"), bPrimitive);
		bool bOperand = false;
		Row->TryGetBoolField(TEXT("operand"), bOperand);
		FModelingSnapshotOptions Options;
		Options.bIncludeTarget = !bPrimitive;
		Options.bIncludeOperand = bOperand;
		FString TestCarrierKind;
		if (Row->TryGetStringField(
				TEXT("test_carrier_kind"),
				TestCarrierKind))
		{
			Options.CarrierKind = TestCarrierKind;
		}
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources =
				MakeModelingResources();
		const FUeAgentCanonicalSnapshotPtr Snapshot =
			MakeModelingSnapshot(
				Test,
				*Descriptor,
				Owner,
				Operand,
				Options,
				*Resources);
		if (!Snapshot.IsValid())
		{
			continue;
		}

		TOptional<FChangeSetDraft> Draft;
		FExecutionMutationError Error;
		const bool bBuilt = BuildModelingDraft(
			Test,
			*Descriptor,
			*Owner.World,
			BuildModelingValidInput(Row),
			Snapshot.ToSharedRef(),
			*Resources,
			Draft,
			Error);
		Test->TestTrue(
			*(DescriptorId + TEXT(" builds one typed draft")),
			bBuilt);
		if (!bBuilt || !Draft.IsSet() || !Draft->IsValid())
		{
			Test->AddError(
				DescriptorId + TEXT(":") + Error.GetCode());
			continue;
		}
		Test->TestEqual(
			*(DescriptorId + TEXT(" produces one atomic change")),
			Draft->GetChanges().Num(),
			1);
		const FChangeDraft& Change = Draft->GetChanges()[0];
		Test->TestEqual(
			*(DescriptorId + TEXT(" freezes typed geometry kind")),
			Change.GetIdentity().GetTargetKind(),
			Row->GetStringField(TEXT("native_operation_type")));
		Test->TestEqual(
			*(DescriptorId + TEXT(" preserves mutation intent")),
			Change.GetIntent(),
			bPrimitive
				? EChangeIntent::Create
				: EChangeIntent::Update);
		Test->TestEqual(
			*(DescriptorId + TEXT(" before-state presence is exact")),
			Change.GetExpectedBefore().IsValid(),
			!bPrimitive);
		for (const FString& NativeField
			: TArray<FString>{
				TEXT("operation_key"),
				TEXT("target_ref"),
				TEXT("carrier_kind"),
				TEXT("expected_before_fingerprint"),
				TEXT("typed_parameters"),
			})
		{
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" freezes native field ")
					+ NativeField),
				HasModelingCanonicalField(
					Change.GetProposedAfter(),
					NativeField));
		}
		for (const FString& PublicField
			: TArray<FString>{
				TEXT("accept"),
				TEXT("active_tool"),
				TEXT("command_id"),
				TEXT("current_selection"),
				TEXT("http"),
				TEXT("save"),
				TEXT("tool_properties"),
				TEXT("transaction"),
				TEXT("verify"),
			})
		{
			Test->TestFalse(
				*(DescriptorId
					+ TEXT(" excludes cross-owner field ")
					+ PublicField),
				HasModelingCanonicalField(
					Change.GetProposedAfter(),
					PublicField));
		}
		if (Row->HasField(TEXT("selection_kind"))
			|| Row->HasField(TEXT("edge_pairs")))
		{
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" freezes revision-bound element ids")),
				HasModelingCanonicalField(
					Change.GetProposedAfter(),
					TEXT("selection_ids")));
		}
		if (bOperand)
		{
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" freezes operand fingerprint")),
				HasModelingCanonicalField(
					Change.GetProposedAfter(),
					TEXT("operand_before_fingerprint")));
		}
	}

	for (const TSharedPtr<FJsonValue>& Value
		: Fixture->GetArrayField(TEXT("e1_negative_cases")))
	{
		const TSharedPtr<FJsonObject> Case = Value->AsObject();
		const FString CaseId =
			Case->GetStringField(TEXT("case_id"));
		const FString DescriptorId =
			Case->GetStringField(TEXT("descriptor_id"));
		const TSharedPtr<FJsonObject> Row =
			FindModelingDescriptorRow(Fixture, DescriptorId);
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(DescriptorId);
		if (!Row.IsValid()
			|| !Test->TestNotNull(
				*(CaseId + TEXT(" descriptor exists")),
				Descriptor))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Input =
			BuildModelingValidInput(Row);
		FModelingSnapshotOptions Options;
		bool bPrimitive = false;
		Row->TryGetBoolField(TEXT("primitive_output"), bPrimitive);
		bool bOperand = false;
		Row->TryGetBoolField(TEXT("operand"), bOperand);
		Options.bIncludeTarget = !bPrimitive;
		Options.bIncludeOperand = bOperand;

		if (CaseId == TEXT("primitive-output-key-already-exists"))
		{
			Options.bIncludeTarget = true;
		}
		else if (CaseId == TEXT("target-unresolved"))
		{
			Options.bIncludeTarget = false;
		}
		else if (CaseId == TEXT("target-revision-stale"))
		{
			Input->SetObjectField(
				TEXT("target_revision"),
				ModelingRevision(
					TEXT("uai://mesh/fixture-target"),
					2));
		}
		else if (CaseId == TEXT("selection-element-invalid"))
		{
			Input->GetObjectField(TEXT("selection"))
				->SetArrayField(
					TEXT("element_ids"),
					ModelingNumberValues({99999.0}));
		}
		else if (CaseId
			== TEXT("boundary-edge-no-longer-boundary"))
		{
			Options.bDeclareBoundaryEdges = false;
		}
		else if (CaseId == TEXT("operand-unresolved"))
		{
			Options.bIncludeOperand = false;
		}
		else if (CaseId == TEXT("operand-revision-stale"))
		{
			Input->SetObjectField(
				TEXT("operand_revision"),
				ModelingRevision(
					TEXT("uai://mesh/fixture-operand"),
					2));
		}
		else if (CaseId == TEXT("operand-transform-singular"))
		{
			Input->GetObjectField(TEXT("operand_to_target"))
				->SetArrayField(
					TEXT("rotation_quaternion_xyzw"),
					ModelingNumberValues({0.0, 0.0, 0.0, 0.0}));
		}
		else if (CaseId == TEXT("plane-normal-zero"))
		{
			Input->GetObjectField(TEXT("plane"))
				->SetArrayField(
					TEXT("normal"),
					ModelingNumberValues({0.0, 0.0, 0.0}));
		}
		else if (CaseId == TEXT("fixed-direction-missing"))
		{
			Input->SetStringField(
				TEXT("direction_mode"),
				TEXT("fixed"));
			Input->RemoveField(TEXT("direction"));
		}
		else if (CaseId == TEXT("unsupported-carrier"))
		{
			Options.CarrierKind = TEXT("skeletal_mesh_asset");
		}
		else if (CaseId
			== TEXT("simplify-ratio-out-of-range"))
		{
			Input->SetStringField(
				TEXT("target_mode"),
				TEXT("triangle_ratio"));
			Input->SetNumberField(TEXT("target_value"), 2.0);
		}
		else if (CaseId == TEXT("weld-edge-pair-duplicate"))
		{
			TArray<TSharedPtr<FJsonValue>> First;
			First.Add(MakeShared<FJsonValueNumber>(0));
			First.Add(MakeShared<FJsonValueNumber>(1));
			TArray<TSharedPtr<FJsonValue>> Second;
			Second.Add(MakeShared<FJsonValueNumber>(1));
			Second.Add(MakeShared<FJsonValueNumber>(0));
			TArray<TSharedPtr<FJsonValue>> Pairs;
			Pairs.Add(
				MakeShared<FJsonValueArray>(MoveTemp(First)));
			Pairs.Add(
				MakeShared<FJsonValueArray>(MoveTemp(Second)));
			Input->SetArrayField(
				TEXT("edge_pairs"),
				MoveTemp(Pairs));
		}

		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources =
				MakeModelingResources();
		const FUeAgentCanonicalSnapshotPtr Snapshot =
			MakeModelingSnapshot(
				Test,
				*Descriptor,
				Owner,
				Operand,
				Options,
				*Resources);
		if (!Snapshot.IsValid())
		{
			continue;
		}
		TOptional<FChangeSetDraft> Draft;
		FExecutionMutationError Error;
		Test->TestFalse(
			*(CaseId + TEXT(" fails before C6 and mutation")),
			BuildModelingDraft(
				Test,
				*Descriptor,
				*Owner.World,
				Input,
				Snapshot.ToSharedRef(),
				*Resources,
				Draft,
				Error));
		Test->TestEqual(
			*(CaseId + TEXT(" returns its stable domain error")),
			Error.GetCode(),
			Case->GetStringField(TEXT("expected_error")));
		Test->TestFalse(
			*(CaseId + TEXT(" produces no draft")),
			Draft.IsSet());
	}
	return !Test->HasAnyErrors();
}

struct FModelingCarrierReadback
{
	FMeshReadback Mesh;
	int32 EdgeCount = 0;
	int32 BoundaryEdgeCount = 0;
	int32 UvLayerCount = 0;
	int32 UvElementCount = 0;
	FVector2f UvMin = FVector2f::ZeroVector;
	FVector2f UvMax = FVector2f::ZeroVector;
	bool bHasUv = false;
	int32 NormalElementCount = 0;
	int64 NormalChecksum = 0;
	int64 MaterialIdChecksum = 0;
	TMap<int32, int32> MaterialIdByTriangle;
	int32 MaterialSlotCount = 0;
	TArray<FName> MaterialSlotNames;
	TArray<FString> MaterialRefs;
	int32 CollisionShapeCount = 0;
	int32 CollisionBoxCount = 0;
	int32 CollisionSphereCount = 0;
	int32 CollisionCapsuleCount = 0;
	int32 CollisionConvexCount = 0;

	bool operator==(const FModelingCarrierReadback& Other) const
	{
		return Mesh == Other.Mesh
			&& EdgeCount == Other.EdgeCount
			&& BoundaryEdgeCount == Other.BoundaryEdgeCount
			&& UvLayerCount == Other.UvLayerCount
			&& UvElementCount == Other.UvElementCount
			&& UvMin.Equals(Other.UvMin)
			&& UvMax.Equals(Other.UvMax)
			&& bHasUv == Other.bHasUv
			&& NormalElementCount == Other.NormalElementCount
			&& NormalChecksum == Other.NormalChecksum
			&& MaterialIdChecksum == Other.MaterialIdChecksum
			&& MaterialIdByTriangle.OrderIndependentCompareEqual(
				Other.MaterialIdByTriangle)
			&& MaterialSlotCount == Other.MaterialSlotCount
			&& MaterialSlotNames == Other.MaterialSlotNames
			&& MaterialRefs == Other.MaterialRefs
			&& CollisionShapeCount == Other.CollisionShapeCount
			&& CollisionBoxCount == Other.CollisionBoxCount
			&& CollisionSphereCount == Other.CollisionSphereCount
			&& CollisionCapsuleCount
				== Other.CollisionCapsuleCount
			&& CollisionConvexCount == Other.CollisionConvexCount;
	}
};

bool ReadModelingCarrier(
	UDynamicMeshComponent& Component,
	FModelingCarrierReadback& Out)
{
	Out = {};
	UDynamicMesh* DynamicMesh = Component.GetDynamicMesh();
	if (!DynamicMesh || !ReadMesh(*DynamicMesh, Out.Mesh))
	{
		return false;
	}
	DynamicMesh->ProcessMesh(
		[&Out](const FDynamicMesh3& Mesh)
		{
			Out.EdgeCount = Mesh.EdgeCount();
			for (const int32 EdgeId : Mesh.EdgeIndicesItr())
			{
				if (Mesh.IsBoundaryEdge(EdgeId))
				{
					++Out.BoundaryEdgeCount;
				}
			}
			if (Mesh.HasAttributes())
			{
				const FDynamicMeshAttributeSet* Attributes =
					Mesh.Attributes();
				Out.UvLayerCount = Attributes->NumUVLayers();
				for (int32 LayerIndex = 0;
					LayerIndex < Attributes->NumUVLayers();
					++LayerIndex)
				{
					const FDynamicMeshUVOverlay* Uv =
						Attributes->GetUVLayer(LayerIndex);
					Out.UvElementCount += Uv->ElementCount();
					for (const int32 ElementId
						: Uv->ElementIndicesItr())
					{
						const FVector2f Value =
							Uv->GetElement(ElementId);
						if (!Out.bHasUv)
						{
							Out.UvMin = Value;
							Out.UvMax = Value;
							Out.bHasUv = true;
						}
						else
						{
							Out.UvMin.X =
								FMath::Min(Out.UvMin.X, Value.X);
							Out.UvMin.Y =
								FMath::Min(Out.UvMin.Y, Value.Y);
							Out.UvMax.X =
								FMath::Max(Out.UvMax.X, Value.X);
							Out.UvMax.Y =
								FMath::Max(Out.UvMax.Y, Value.Y);
						}
					}
				}
				if (const FDynamicMeshNormalOverlay* Normals =
					Attributes->PrimaryNormals())
				{
					Out.NormalElementCount =
						Normals->ElementCount();
					for (const int32 ElementId
						: Normals->ElementIndicesItr())
					{
						const FVector3f Normal =
							Normals->GetElement(ElementId);
						Out.NormalChecksum +=
							static_cast<int64>(ElementId + 1)
							* (
								FMath::RoundToInt64(
									Normal.X * 1000.0)
								+ FMath::RoundToInt64(
									Normal.Y * 10000.0)
								+ FMath::RoundToInt64(
									Normal.Z * 100000.0));
					}
				}
				if (const FDynamicMeshMaterialAttribute* Materials =
					Attributes->GetMaterialID())
				{
					for (const int32 TriangleId
						: Mesh.TriangleIndicesItr())
					{
						const int32 MaterialId =
							Materials->GetValue(TriangleId);
						Out.MaterialIdChecksum += MaterialId;
						Out.MaterialIdByTriangle.Add(
							TriangleId,
							MaterialId);
					}
				}
			}
		});
	Out.MaterialSlotCount = Component.GetNumMaterials();
	Out.MaterialSlotNames = Component.GetMaterialSlotNames();
	for (int32 Index = 0;
		Index < Component.GetNumMaterials();
		++Index)
	{
		UMaterialInterface* Material = Component.GetMaterial(Index);
		Out.MaterialRefs.Add(
			Material ? Material->GetPathName() : FString());
	}
	if (const UBodySetup* BodySetup = Component.GetBodySetup())
	{
		const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
		Out.CollisionBoxCount = AggGeom.BoxElems.Num();
		Out.CollisionSphereCount = AggGeom.SphereElems.Num();
		Out.CollisionCapsuleCount =
			AggGeom.SphylElems.Num()
			+ AggGeom.TaperedCapsuleElems.Num();
		Out.CollisionConvexCount = AggGeom.ConvexElems.Num();
		Out.CollisionShapeCount =
			Out.CollisionBoxCount
			+ Out.CollisionSphereCount
			+ Out.CollisionCapsuleCount
			+ Out.CollisionConvexCount;
	}
	return true;
}

UStaticMesh* CreateModelingStaticMeshCarrier(
	FOwnedDynamicMeshWorld& Owner)
{
	if (!Owner.Package || !Owner.DynamicMesh)
	{
		return nullptr;
	}
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(
		Owner.Package,
		*FString::Printf(
			TEXT("SM_ModelingGeometry_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)),
		RF_Transactional);
	if (!StaticMesh)
	{
		return nullptr;
	}
	StaticMesh->SetNumSourceModels(1);
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	Owner.DynamicMesh->ProcessMesh(
		[&Description](const FDynamicMesh3& Mesh)
		{
			FDynamicMeshToMeshDescription Converter;
			Converter.Convert(&Mesh, Description);
		});
	StaticMesh->CreateMeshDescription(0, Description);
	StaticMesh->CommitMeshDescription(0);
	StaticMesh->GetStaticMaterials().Add(
		FStaticMaterial(
			UMaterial::GetDefaultMaterial(MD_Surface)));
	StaticMesh->GetStaticMaterials().Add(
		FStaticMaterial(
			UMaterial::GetDefaultMaterial(MD_Surface)));
	if (!StaticMesh->GetBodySetup())
	{
		StaticMesh->CreateBodySetup();
	}
	return StaticMesh;
}

bool ReadModelingStaticCarrier(
	UStaticMesh& StaticMesh,
	FModelingCarrierReadback& Out)
{
	Out = {};
	const FMeshDescription* Description =
		StaticMesh.GetMeshDescription(0);
	if (!Description)
	{
		return false;
	}
	FDynamicMesh3 Mesh;
	FMeshDescriptionToDynamicMesh Converter;
	Converter.Convert(Description, Mesh);
	Out.Mesh.VertexCount = Mesh.VertexCount();
	Out.Mesh.TriangleCount = Mesh.TriangleCount();
	for (const int32 VertexId : Mesh.VertexIndicesItr())
	{
		const FVector3d Position = Mesh.GetVertex(VertexId);
		Out.Mesh.Bounds += FVector(Position);
		Out.Mesh.VertexChecksum +=
			FMath::RoundToInt64(Position.X * 100.0)
			+ FMath::RoundToInt64(Position.Y * 1000.0)
			+ FMath::RoundToInt64(Position.Z * 10000.0);
	}
	for (const int32 EdgeId : Mesh.EdgeIndicesItr())
	{
		if (Mesh.IsBoundaryEdge(EdgeId))
		{
			++Out.BoundaryEdgeCount;
		}
	}
	Out.EdgeCount = Mesh.EdgeCount();
	if (Mesh.HasAttributes())
	{
		const FDynamicMeshAttributeSet* Attributes =
			Mesh.Attributes();
		Out.UvLayerCount = Attributes->NumUVLayers();
		for (int32 LayerIndex = 0;
			LayerIndex < Attributes->NumUVLayers();
			++LayerIndex)
		{
			const FDynamicMeshUVOverlay* Uv =
				Attributes->GetUVLayer(LayerIndex);
			Out.UvElementCount += Uv->ElementCount();
			for (const int32 ElementId : Uv->ElementIndicesItr())
			{
				const FVector2f Value =
					Uv->GetElement(ElementId);
				if (!Out.bHasUv)
				{
					Out.UvMin = Value;
					Out.UvMax = Value;
					Out.bHasUv = true;
				}
				else
				{
					Out.UvMin.X =
						FMath::Min(Out.UvMin.X, Value.X);
					Out.UvMin.Y =
						FMath::Min(Out.UvMin.Y, Value.Y);
					Out.UvMax.X =
						FMath::Max(Out.UvMax.X, Value.X);
					Out.UvMax.Y =
						FMath::Max(Out.UvMax.Y, Value.Y);
				}
			}
		}
		if (const FDynamicMeshNormalOverlay* Normals =
			Attributes->PrimaryNormals())
		{
			Out.NormalElementCount = Normals->ElementCount();
			for (const int32 ElementId
				: Normals->ElementIndicesItr())
			{
				const FVector3f Normal =
					Normals->GetElement(ElementId);
				Out.NormalChecksum +=
					static_cast<int64>(ElementId + 1)
					* (
						FMath::RoundToInt64(
							Normal.X * 1000.0)
						+ FMath::RoundToInt64(
							Normal.Y * 10000.0)
						+ FMath::RoundToInt64(
							Normal.Z * 100000.0));
			}
		}
		if (const FDynamicMeshMaterialAttribute* Materials =
			Attributes->GetMaterialID())
		{
			for (const int32 TriangleId
				: Mesh.TriangleIndicesItr())
			{
				const int32 MaterialId =
					Materials->GetValue(TriangleId);
				Out.MaterialIdChecksum += MaterialId;
				Out.MaterialIdByTriangle.Add(
					TriangleId,
					MaterialId);
			}
		}
	}
	Out.MaterialSlotCount =
		StaticMesh.GetStaticMaterials().Num();
	for (const FStaticMaterial& Material
		: StaticMesh.GetStaticMaterials())
	{
		Out.MaterialSlotNames.Add(Material.MaterialSlotName);
		Out.MaterialRefs.Add(
			Material.MaterialInterface
				? Material.MaterialInterface->GetPathName()
				: FString());
	}
	if (const UBodySetup* BodySetup = StaticMesh.GetBodySetup())
	{
		const FKAggregateGeom& AggGeom =
			BodySetup->AggGeom;
		Out.CollisionBoxCount = AggGeom.BoxElems.Num();
		Out.CollisionSphereCount = AggGeom.SphereElems.Num();
		Out.CollisionCapsuleCount =
			AggGeom.SphylElems.Num()
			+ AggGeom.TaperedCapsuleElems.Num();
		Out.CollisionConvexCount = AggGeom.ConvexElems.Num();
		Out.CollisionShapeCount =
			Out.CollisionBoxCount
			+ Out.CollisionSphereCount
			+ Out.CollisionCapsuleCount
			+ Out.CollisionConvexCount;
	}
	return true;
}

ADynamicMeshActor* FindExactDynamicMeshActor(
	UWorld& World,
	const FString& Label)
{
	ADynamicMeshActor* Match = nullptr;
	for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
	{
		if (It->GetActorLabel() != Label)
		{
			continue;
		}
		if (Match)
		{
			return nullptr;
		}
		Match = *It;
	}
	return Match;
}

bool ReadHighestVertexLocal(
	UDynamicMeshComponent& Component,
	FVector3d& OutVertex)
{
	OutVertex = FVector3d::Zero();
	UDynamicMesh* DynamicMesh = Component.GetDynamicMesh();
	if (!DynamicMesh)
	{
		return false;
	}
	bool bFound = false;
	DynamicMesh->ProcessMesh(
		[&](const FDynamicMesh3& Mesh)
		{
			double HighestZ = -TNumericLimits<double>::Max();
			TArray<FVector3d> Highest;
			for (const int32 VertexId : Mesh.VertexIndicesItr())
			{
				const FVector3d Vertex =
					Mesh.GetVertex(VertexId);
				if (Vertex.Z > HighestZ + UE_DOUBLE_SMALL_NUMBER)
				{
					HighestZ = Vertex.Z;
					Highest.Reset();
					Highest.Add(Vertex);
				}
				else if (FMath::IsNearlyEqual(
					Vertex.Z,
					HighestZ,
					UE_DOUBLE_SMALL_NUMBER))
				{
					Highest.Add(Vertex);
				}
			}
			if (Highest.IsEmpty())
			{
				return;
			}
			for (const FVector3d& Vertex : Highest)
			{
				OutVertex += Vertex;
			}
			OutVertex /= static_cast<double>(Highest.Num());
			bFound = true;
		});
	return bFound;
}

bool VerifyPrimitiveSemanticOracle(
	FAutomationTestBase* Test,
	const FString& DescriptorId,
	const FString& OperationKey,
	ADynamicMeshActor& Actor,
	const FModelingCarrierReadback& After)
{
	UDynamicMeshComponent* Component =
		Actor.GetDynamicMeshComponent();
	if (!Test->TestNotNull(
			*(DescriptorId + TEXT(" created carrier exists")),
			Component))
	{
		return false;
	}
	Test->TestTrue(
		*(DescriptorId + TEXT(" placement translation")),
		Actor.GetActorLocation().Equals(
			FVector(100.0, 200.0, 300.0),
			0.01));
	Test->TestTrue(
		*(DescriptorId + TEXT(" placement rotation")),
		Actor.GetActorQuat().Equals(FQuat::Identity, 0.0001));
	Test->TestTrue(
		*(DescriptorId + TEXT(" placement non-uniform scale")),
		Actor.GetActorScale3D().Equals(
			FVector(1.0, 2.0, 3.0),
			0.0001));

	const FVector Size = After.Mesh.Bounds.GetSize();
	if (OperationKey == TEXT("box"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" exact local bounds")),
			Size.Equals(FVector(120.0, 80.0, 40.0), 0.01)
				&& FMath::IsNearlyZero(
					After.Mesh.Bounds.Min.Z,
					0.01));
		Test->TestTrue(
			*(DescriptorId + TEXT(" subdivisions affect topology")),
			After.Mesh.VertexCount > 8
				&& After.Mesh.TriangleCount > 12);
	}
	else if (OperationKey == TEXT("cylinder"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" exact radius height anchor")),
			FMath::IsNearlyEqual(Size.X, 70.0, 0.5)
				&& FMath::IsNearlyEqual(Size.Y, 70.0, 0.5)
				&& FMath::IsNearlyEqual(Size.Z, 90.0, 0.01)
				&& FMath::IsNearlyZero(
					After.Mesh.Bounds.Min.Z,
					0.01));
		Test->TestTrue(
			*(DescriptorId + TEXT(" height segments affect topology")),
			After.Mesh.VertexCount > 26
				&& After.Mesh.TriangleCount > 48);
	}
	else if (OperationKey == TEXT("plane"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" exact plane bounds")),
			FMath::IsNearlyEqual(Size.X, 120.0, 0.01)
				&& FMath::IsNearlyEqual(Size.Y, 80.0, 0.01)
				&& FMath::IsNearlyZero(Size.Z, 0.01));
		Test->TestEqual(
			*(DescriptorId
				+ TEXT(" subdivisions and double-sided topology")),
			After.Mesh.TriangleCount,
			24);
	}
	else if (OperationKey == TEXT("sphere"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" exact sphere bounds")),
			Size.Equals(FVector(100.0), 0.5));
		Test->TestTrue(
			*(DescriptorId + TEXT(" sphere segments affect topology")),
			After.Mesh.VertexCount >= 74
				&& After.Mesh.TriangleCount >= 144);
	}
	else
	{
		FVector3d Highest;
		Test->TestTrue(
			*(DescriptorId + TEXT(" highest vertex readback")),
			ReadHighestVertexLocal(*Component, Highest));
		if (OperationKey == TEXT("ramp"))
		{
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" negative X rise direction")),
				Highest.X < -50.0
					&& FMath::Abs(Highest.Y) < 1.0);
		}
		else if (OperationKey == TEXT("ramp_corner"))
		{
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" requested high corner")),
				Highest.X < -50.0 && Highest.Y < -30.0);
		}
		else if (OperationKey == TEXT("stairs"))
		{
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" negative X stairs bounds")),
				FMath::IsNearlyEqual(Size.X, 120.0, 0.01)
					&& FMath::IsNearlyEqual(Size.Y, 100.0, 0.01)
					&& FMath::IsNearlyEqual(Size.Z, 80.0, 0.01)
					&& After.Mesh.Bounds.Max.X <= 0.01
					&& FMath::IsNearlyZero(
						After.Mesh.Bounds.Min.Z,
						0.01));
		}
	}
	return !Test->HasAnyErrors();
}

bool VerifyTopologySemanticOracle(
	FAutomationTestBase* Test,
	const FString& DescriptorId,
	const FString& OperationKey,
	const FModelingCarrierReadback& Before,
	const FModelingCarrierReadback& After)
{
	const int32 BeforeTriangles = Before.Mesh.TriangleCount;
	const int32 AfterTriangles = After.Mesh.TriangleCount;
	if (OperationKey == TEXT("bevel_edges"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" bevel adds topology")),
			AfterTriangles > BeforeTriangles
				&& After.Mesh.VertexCount > Before.Mesh.VertexCount);
	}
	else if (OperationKey == TEXT("boolean"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" union is closed and expanded")),
			After.BoundaryEdgeCount == 0
				&& After.Mesh.Bounds.Max.X
					>= Before.Mesh.Bounds.Max.X + 20.0);
	}
	else if (OperationKey == TEXT("duplicate_faces"))
	{
		Test->TestEqual(
			*(DescriptorId + TEXT(" duplicates full selection")),
			AfterTriangles,
			BeforeTriangles + 2);
		Test->TestTrue(
			*(DescriptorId + TEXT(" uses requested translation")),
			After.Mesh.Bounds.Max.Z
				>= Before.Mesh.Bounds.Max.Z + 120.0);
	}
	else if (OperationKey == TEXT("extrude_faces"))
	{
		Test->TestTrue(
			*(DescriptorId
				+ TEXT(" extrudes selection with side topology")),
			AfterTriangles >= BeforeTriangles + 8
				&& After.Mesh.Bounds.Max.Z
					>= Before.Mesh.Bounds.Max.Z + 20.0);
	}
	else if (OperationKey == TEXT("fill_holes"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" closes selected holes")),
			After.BoundaryEdgeCount
				< Before.BoundaryEdgeCount);
	}
	else if (OperationKey == TEXT("inset_faces"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" creates inset topology")),
			AfterTriangles > BeforeTriangles
				&& After.Mesh.VertexCount > Before.Mesh.VertexCount);
	}
	else if (OperationKey == TEXT("mesh_cut"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" filled cut has no raw triangle seam")),
			After.BoundaryEdgeCount == 0
				&& AfterTriangles > BeforeTriangles);
	}
	else if (OperationKey == TEXT("mirror"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" keep-original mirrors full mesh")),
			AfterTriangles >= BeforeTriangles * 2
				&& After.BoundaryEdgeCount == 0);
	}
	else if (OperationKey == TEXT("offset_faces"))
	{
		Test->TestTrue(
			*FString::Printf(
				TEXT(
					"%s honors 20cm face-normal offset "
					"(max Z %.3f -> %.3f)"),
				*DescriptorId,
				Before.Mesh.Bounds.Max.Z,
				After.Mesh.Bounds.Max.Z),
			After.Mesh.Bounds.Max.Z
				>= Before.Mesh.Bounds.Max.Z + 15.0);
	}
	else if (OperationKey == TEXT("plane_cut"))
	{
		Test->TestTrue(
			*FString::Printf(
				TEXT(
					"%s keep-both filled plane cut "
					"(triangles %d -> %d, boundary edges %d -> %d)"),
				*DescriptorId,
				BeforeTriangles,
				AfterTriangles,
				Before.BoundaryEdgeCount,
				After.BoundaryEdgeCount),
			AfterTriangles > BeforeTriangles
				&& After.BoundaryEdgeCount == 0);
	}
	else if (OperationKey == TEXT("push_pull_faces"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" push-pull creates side topology")),
			AfterTriangles >= BeforeTriangles + 8
				&& After.Mesh.Bounds.Max.Z
					>= Before.Mesh.Bounds.Max.Z + 25.0);
	}
	else if (OperationKey == TEXT("remesh"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" target edge length changes density")),
			AfterTriangles >= BeforeTriangles * 3);
	}
	else if (OperationKey == TEXT("simplify"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" triangle-ratio target")),
			AfterTriangles <= FMath::CeilToInt(
				BeforeTriangles * 0.25) + 1);
	}
	else if (OperationKey == TEXT("subdivide"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" two uniform levels")),
			AfterTriangles >= BeforeTriangles * 4);
	}
	else if (OperationKey == TEXT("trim"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" trim returns closed target")),
			After.BoundaryEdgeCount == 0
				&& AfterTriangles > 0);
	}
	else if (OperationKey == TEXT("voxel_boolean"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" voxel union returns closed mesh")),
			After.BoundaryEdgeCount == 0
				&& AfterTriangles > BeforeTriangles);
	}
	else if (OperationKey == TEXT("weld_edges"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" weld reduces boundary edges")),
			After.BoundaryEdgeCount
				< Before.BoundaryEdgeCount);
	}
	return !Test->HasAnyErrors();
}

bool VerifyAttributeSemanticOracle(
	FAutomationTestBase* Test,
	const FString& DescriptorId,
	const FString& OperationKey,
	const TSharedPtr<FJsonObject>& Input,
	const FModelingCarrierReadback& Before,
	const FModelingCarrierReadback& After)
{
	if (OperationKey == TEXT("add_material_slot"))
	{
		Test->TestEqual(
			*(DescriptorId + TEXT(" adds exactly one slot")),
			After.MaterialSlotCount,
			Before.MaterialSlotCount + 1);
		Test->TestTrue(
			*(DescriptorId + TEXT(" preserves requested slot name")),
			After.MaterialSlotNames.Contains(
				FName(TEXT("SA003I_Slot"))));
		Test->TestTrue(
			*(DescriptorId + TEXT(" binds requested material")),
			After.MaterialRefs.Contains(
				UMaterial::GetDefaultMaterial(MD_Surface)
					->GetPathName()));
	}
	else if (OperationKey == TEXT("auto_uv")
		|| OperationKey == TEXT("project_uv"))
	{
		Test->TestTrue(
			*(DescriptorId + TEXT(" writes requested UV channel")),
			After.UvLayerCount >= 3
				&& After.UvElementCount > 0);
		Test->TestTrue(
			*(DescriptorId + TEXT(" produces normalized UVs")),
			After.bHasUv
				&& After.UvMin.X >= -0.001f
				&& After.UvMin.Y >= -0.001f
				&& After.UvMax.X <= 1.001f
				&& After.UvMax.Y <= 1.001f);
	}
	else if (OperationKey == TEXT("recompute_normals"))
	{
		Test->TestTrue(
			*(DescriptorId
				+ TEXT(" area-angle normals share overlay elements")),
			After.NormalElementCount > 0
				&& After.NormalElementCount
					< After.Mesh.TriangleCount * 3);
	}
	else if (OperationKey == TEXT("assign_material_slot"))
	{
		const TSharedPtr<FJsonObject> Selection =
			Input->GetObjectField(TEXT("selection"));
		for (const TSharedPtr<FJsonValue>& Value
			: Selection->GetArrayField(TEXT("element_ids")))
		{
			const int32 TriangleId =
				static_cast<int32>(Value->AsNumber());
			const int32* MaterialId =
				After.MaterialIdByTriangle.Find(TriangleId);
			Test->TestTrue(
				*FString::Printf(
					TEXT("%s assigns selected triangle %d"),
					*DescriptorId,
					TriangleId),
				MaterialId && *MaterialId == 1);
		}
	}
	return !Test->HasAnyErrors();
}

bool VerifyCollisionSemanticOracle(
	FAutomationTestBase* Test,
	const FString& DescriptorId,
	const FString& OperationKey,
	const FModelingCarrierReadback& After)
{
	if (OperationKey == TEXT("generate_convex_collision"))
	{
		Test->TestTrue(
			*(DescriptorId
				+ TEXT(" decomposition creates per-component hulls")),
			After.CollisionConvexCount >= 2
				&& After.CollisionBoxCount == 0
				&& After.CollisionSphereCount == 0
				&& After.CollisionCapsuleCount == 0);
	}
	else
	{
		Test->TestTrue(
			*(DescriptorId
				+ TEXT(" aligned boxes follow disconnected components")),
			After.CollisionBoxCount == 2
				&& After.CollisionConvexCount == 0
				&& After.CollisionSphereCount == 0
				&& After.CollisionCapsuleCount == 0);
	}
	return !Test->HasAnyErrors();
}

struct FModelingNativeRequest
{
	FModelingNativeRequest(
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			InResources,
		FPlanningPreflightPlanRef InPlan,
		FUeAgentExecutionContextRefOwner InContext)
		: Resources(MoveTemp(InResources))
		, Cancellation([this]() { return Now; })
		, Plan(MoveTemp(InPlan))
		, Context(MoveTemp(InContext))
	{
	}

	FExecutionMutationRequest MakeRequest()
	{
		return FExecutionMutationRequest(
			TEXT("operation:modeling-geometry"),
			TEXT("owner:modeling-geometry"),
			Plan,
			Context,
			Resources,
			FUeAgentOperationDeadline::FromTimeout(
				60.0,
				[this]() { return Now; }),
			Cancellation.GetToken(),
			EExecutionThreadAffinity::GameThread,
			EExecutionReentrancyPolicy::RejectSameTarget);
	}

	double Now = FPlatformTime::Seconds();
	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources;
	FUeAgentCancellationSource Cancellation;
	FPlanningPreflightPlanRef Plan;
	FUeAgentExecutionContextRefOwner Context;
};

FUeAgentGenericEffectBinding MakeModelingBinding(
	const FUeAgentOperationDescriptor& Descriptor,
	const FUeAgentCanonicalIdentity& Identity)
{
	return FUeAgentGenericEffectBinding(
		FUeAgentCandidateBinding(
			TEXT("binding:") + Descriptor.GetCapabilityId(),
			FUeAgentCandidateSummary(
				Identity,
				Descriptor.GetIdentity(),
				TEXT("modeling_geometry"),
				Descriptor.GetCapabilityId(),
				TEXT("owned mesh geometry target"),
				{TEXT("Mesh"), TEXT("Geometry")}),
			FUeAgentCatalogRevision(
				TEXT("modeling-geometry-e2"),
				TEXT("owned-worker:r1"),
				1),
			TEXT("uai://mesh/")
				+ Descriptor.GetCapabilityId(),
			TEXT("modeling-geometry-read"),
			{
				EUeAgentCandidateOperation::Create,
				EUeAgentCandidateOperation::Update,
			},
			TEXT("fixture:modeling-geometry-e2:binding")),
		TEXT("UAI.ModelingGeometry:CanonicalObject"),
		FUeAgentCanonicalType(
			TEXT("modeling_geometry_canonical_object"),
			EUeAgentValueKind::Object,
			false,
			TEXT("object")),
		TEXT("fixture:modeling-geometry-e2:type-binding"));
}

struct FAppliedModelingCapability
{
	TSharedPtr<IUeAgentModelingGeometryEffectNativeHook> Hook;
	TUniquePtr<FModelingNativeRequest> Fixture;
	TOptional<FExecutionMutationRequest> Request;
	FUeAgentGenericEffectApplyResult Result;
};

bool ApplyModelingCapability(
	FAutomationTestBase* Test,
	const TSharedRef<FOwnedReferenceResolver, ESPMode::ThreadSafe>&
		Resolver,
	const TSharedRef<const FUeAgentDescriptorRegistry>& Registry,
	FOwnedDynamicMeshWorld& Owner,
	const TSharedPtr<FJsonObject>& Row,
	const TSharedPtr<FJsonObject>& Input,
	ADynamicMeshActor* Operand,
	const FUeAgentCanonicalSnapshotRef& Snapshot,
	FAppliedModelingCapability& OutApplied)
{
	const FString DescriptorId =
		Row->GetStringField(TEXT("descriptor_id"));
	const FUeAgentOperationDescriptor* Descriptor =
		Registry->FindByDescriptorId(DescriptorId);
	if (!Test->TestNotNull(
			*(DescriptorId + TEXT(" descriptor exists")),
			Descriptor))
	{
		return false;
	}
	const TSharedRef<
		FUeAgentResourceTracker,
		ESPMode::ThreadSafe> Resources =
			MakeModelingResources();
	TOptional<FChangeSetDraft> Draft;
	FExecutionMutationError Error;
	if (!BuildModelingDraft(
			Test,
			*Descriptor,
			*Owner.World,
			Input,
			Snapshot,
			*Resources,
			Draft,
			Error)
		|| !Draft.IsSet()
		|| !Draft->IsValid())
	{
		Test->AddError(
			DescriptorId + TEXT(":") + Error.GetCode());
		return false;
	}

	FPlanningResult PlanningResult;
	FPlanningPreflightError PlanningError;
	if (!Test->TestTrue(
			*(DescriptorId + TEXT(" freezes through C6")),
			TryPlanChanges(
				FPlanningRequest(
					Draft.GetValue(),
					FUeAgentCallerContext(
						TEXT("agent:modeling-geometry-e2"),
						{TEXT("uai:modeling-geometry:write")},
						TEXT("correlation:modeling-geometry-e2")),
					false),
				*Resources,
				PlanningResult,
				PlanningError))
		|| !PlanningResult.IsReady()
		|| !PlanningResult.GetPlan().IsValid())
	{
		return false;
	}

	TSharedPtr<IUeAgentModelingGeometryEffectNativeHook> Hook;
	if (!Test->TestTrue(
			*(DescriptorId + TEXT(" native hook creates")),
			FUeAgentModelingGeometryEffectNativeHookFactory::TryCreate(
				Resolver,
				Hook,
				Error))
		|| !Hook.IsValid())
	{
		return false;
	}
	const FUeAgentExecutionContextRefOwner Context =
		MakeModelingExecutionContext(
			Descriptor->GetIdentity(),
			*Owner.World);
	OutApplied.Fixture = MakeUnique<FModelingNativeRequest>(
		Resources,
		PlanningResult.GetPlan().ToSharedRef(),
		Context);
	OutApplied.Request.Emplace(
		OutApplied.Fixture->MakeRequest());
	OutApplied.Hook = MoveTemp(Hook);

	TArray<FUeAgentGenericEffectBinding> Bindings;
	for (const FPlannedChange& Change
		: OutApplied.Fixture->Plan->GetChangeSet().GetChanges())
	{
		Bindings.Add(
			MakeModelingBinding(
				*Descriptor,
				Change.GetIdentity()));
	}
	const bool bApplied = OutApplied.Hook->TryApply(
		*Owner.World,
		OutApplied.Request.GetValue(),
		FLockFenceLease(
			TEXT("owner:modeling-geometry-e2"),
			{Owner.World->GetPathName()},
			1),
		Bindings,
		*OutApplied.Fixture->Resources,
		OutApplied.Result,
		Error);
	Test->TestTrue(
		*(DescriptorId + TEXT(" applies through direct native hook")),
		bApplied);
	Test->TestTrue(
		*(DescriptorId + TEXT(" reports mutation start")),
		OutApplied.Result.DidMutationStart());
	if (!bApplied)
	{
		Test->AddError(
			DescriptorId + TEXT(":") + Error.GetCode());
	}
	return bApplied;
}

void RollbackModelingCapability(
	FAutomationTestBase* Test,
	FOwnedDynamicMeshWorld& Owner,
	const FString& DescriptorId,
	FAppliedModelingCapability& Applied)
{
	bool bConfirmed = false;
	FExecutionMutationError Error;
	Test->TestTrue(
		*(DescriptorId + TEXT(" rollback executes")),
		Applied.Hook->TryRollback(
			*Owner.World,
			Applied.Request.GetValue(),
			Applied.Result,
			*Applied.Fixture->Resources,
			bConfirmed,
			Error));
	Test->TestTrue(
		*(DescriptorId + TEXT(" rollback is confirmed")),
		bConfirmed);
}

void PrepareModelingScenarioMesh(
	const FString& FixtureKind,
	UDynamicMesh& DynamicMesh)
{
	if (FixtureKind == TEXT("open_cube"))
	{
		DynamicMesh.EditMesh(
			[](FDynamicMesh3& Mesh)
			{
				if (Mesh.IsTriangle(0))
				{
					Mesh.RemoveTriangle(0, false, false);
				}
				if (Mesh.IsTriangle(2))
				{
					Mesh.RemoveTriangle(2, false, false);
				}
			});
	}
	else if (FixtureKind == TEXT("collision_cube"))
	{
		DynamicMesh.EditMesh(
			[](FDynamicMesh3& Mesh)
			{
				TMap<int32, int32> VertexMap;
				TArray<int32> SourceVertices;
				TArray<int32> SourceTriangles;
				for (const int32 VertexId
					: Mesh.VertexIndicesItr())
				{
					SourceVertices.Add(VertexId);
				}
				for (const int32 TriangleId
					: Mesh.TriangleIndicesItr())
				{
					SourceTriangles.Add(TriangleId);
				}
				for (const int32 VertexId : SourceVertices)
				{
					VertexMap.Add(
						VertexId,
						Mesh.AppendVertex(
							Mesh.GetVertex(VertexId)
								+ FVector3d(250.0, 0.0, 0.0)));
				}
				for (const int32 TriangleId : SourceTriangles)
				{
					const FIndex3i Triangle =
						Mesh.GetTriangle(TriangleId);
					Mesh.AppendTriangle(
						VertexMap[Triangle.A],
						VertexMap[Triangle.B],
						VertexMap[Triangle.C]);
				}
			});
	}
	else if (FixtureKind == TEXT("weld_seam"))
	{
		DynamicMesh.EditMesh(
			[](FDynamicMesh3& Mesh)
			{
				Mesh.Clear();
				Mesh.EnableTriangleGroups();
				const int32 A0 = Mesh.AppendVertex(
					FVector3d(0.0, 0.0, 0.0));
				const int32 A1 = Mesh.AppendVertex(
					FVector3d(100.0, 0.0, 0.0));
				const int32 A2 = Mesh.AppendVertex(
					FVector3d(0.0, 100.0, 0.0));
				const int32 B0 = Mesh.AppendVertex(
					FVector3d(100.0, 0.0, 0.0));
				const int32 B1 = Mesh.AppendVertex(
					FVector3d(100.0, 100.0, 0.0));
				const int32 B2 = Mesh.AppendVertex(
					FVector3d(0.0, 100.0, 0.0));
				Mesh.AppendTriangle(A0, A1, A2);
				Mesh.AppendTriangle(B0, B1, B2);
				Mesh.EnableAttributes();
			});
	}
	else if (FixtureKind == TEXT("attribute_cube"))
	{
		DynamicMesh.EditMesh(
			[](FDynamicMesh3& Mesh)
			{
				if (!Mesh.HasAttributes())
				{
					Mesh.EnableAttributes();
				}
				FDynamicMeshNormalOverlay* Normals =
					Mesh.Attributes()->PrimaryNormals();
				Normals->ClearElements();
				for (const int32 TriangleId
					: Mesh.TriangleIndicesItr())
				{
					const FIndex3i Triangle =
						Mesh.GetTriangle(TriangleId);
					FIndex3i Elements;
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						const int32 ElementId =
							Normals->AppendElement(
								FVector3f(1.0f, 0.0f, 0.0f));
						Normals->SetParentVertex(
							ElementId,
							Triangle[Corner]);
						Elements[Corner] = ElementId;
					}
					Normals->SetTriangle(
						TriangleId,
						Elements);
				}
			});
	}
}

void PatchModelingSelectionForLiveMesh(
	const TSharedPtr<FJsonObject>& Row,
	const TSharedPtr<FJsonObject>& Input,
	UDynamicMesh& DynamicMesh)
{
	FString SelectionKind;
	if (Row->TryGetStringField(TEXT("selection_kind"), SelectionKind))
	{
		TArray<int32> ElementIds;
		DynamicMesh.ProcessMesh(
			[&ElementIds, &SelectionKind](
				const FDynamicMesh3& Mesh)
			{
				if (SelectionKind == TEXT("triangle"))
				{
					double HighestCentroidZ =
						-TNumericLimits<double>::Max();
					TArray<int32> HighestTriangles;
					for (const int32 TriangleId
						: Mesh.TriangleIndicesItr())
					{
						const FVector3d Centroid =
							Mesh.GetTriCentroid(TriangleId);
						if (Centroid.Z
							> HighestCentroidZ
								+ UE_DOUBLE_SMALL_NUMBER)
						{
							HighestCentroidZ = Centroid.Z;
							HighestTriangles.Reset();
							HighestTriangles.Add(TriangleId);
						}
						else if (FMath::IsNearlyEqual(
							Centroid.Z,
							HighestCentroidZ,
							UE_DOUBLE_SMALL_NUMBER))
						{
							HighestTriangles.Add(TriangleId);
						}
					}
					for (const int32 TriangleId : HighestTriangles)
					{
						ElementIds.Add(TriangleId);
						if (ElementIds.Num() == 2)
						{
							break;
						}
					}
				}
				else if (SelectionKind == TEXT("vertex"))
				{
					for (const int32 VertexId
						: Mesh.VertexIndicesItr())
					{
						ElementIds.Add(VertexId);
						if (ElementIds.Num() == 2)
						{
							break;
						}
					}
				}
				else
				{
					for (const int32 EdgeId
						: Mesh.EdgeIndicesItr())
					{
						if (SelectionKind
								== TEXT("boundary_edge")
							&& !Mesh.IsBoundaryEdge(EdgeId))
						{
							continue;
						}
						ElementIds.Add(EdgeId);
						if (ElementIds.Num() == 2)
						{
							break;
						}
					}
				}
			});
		if (!ElementIds.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> JsonIds;
			for (const int32 ElementId : ElementIds)
			{
				JsonIds.Add(
					MakeShared<FJsonValueNumber>(ElementId));
			}
			Input->GetObjectField(TEXT("selection"))
				->SetArrayField(
					TEXT("element_ids"),
					MoveTemp(JsonIds));
		}
	}
	bool bEdgePairs = false;
	if (Row->TryGetBoolField(TEXT("edge_pairs"), bEdgePairs)
		&& bEdgePairs)
	{
		UE::Geometry::FIndex2i WeldPair =
			UE::Geometry::FIndex2i::Invalid();
		DynamicMesh.ProcessMesh(
			[&WeldPair](const FDynamicMesh3& Mesh)
			{
				TArray<int32> BoundaryEdges;
				for (const int32 EdgeId : Mesh.EdgeIndicesItr())
				{
					if (Mesh.IsBoundaryEdge(EdgeId))
					{
						BoundaryEdges.Add(EdgeId);
					}
				}
				for (int32 A = 0;
					A < BoundaryEdges.Num()
						&& WeldPair == UE::Geometry::FIndex2i::Invalid();
					++A)
				{
					const UE::Geometry::FIndex2i EdgeA =
						Mesh.GetEdgeV(BoundaryEdges[A]);
					for (int32 B = A + 1;
						B < BoundaryEdges.Num();
						++B)
					{
						const UE::Geometry::FIndex2i EdgeB =
							Mesh.GetEdgeV(BoundaryEdges[B]);
						const FVector3d A0 =
							Mesh.GetVertex(EdgeA.A);
						const FVector3d A1 =
							Mesh.GetVertex(EdgeA.B);
						const FVector3d B0 =
							Mesh.GetVertex(EdgeB.A);
						const FVector3d B1 =
							Mesh.GetVertex(EdgeB.B);
						if ((A0.Equals(B0, 0.001)
								&& A1.Equals(B1, 0.001))
							|| (A0.Equals(B1, 0.001)
								&& A1.Equals(B0, 0.001)))
						{
							WeldPair = UE::Geometry::FIndex2i(
								BoundaryEdges[A],
								BoundaryEdges[B]);
							break;
						}
					}
				}
			});
		if (WeldPair != UE::Geometry::FIndex2i::Invalid())
		{
			TArray<TSharedPtr<FJsonValue>> Pair;
			Pair.Add(
				MakeShared<FJsonValueNumber>(WeldPair.A));
			Pair.Add(
				MakeShared<FJsonValueNumber>(WeldPair.B));
			TArray<TSharedPtr<FJsonValue>> Pairs;
			Pairs.Add(
				MakeShared<FJsonValueArray>(MoveTemp(Pair)));
			Input->SetArrayField(
				TEXT("edge_pairs"),
				MoveTemp(Pairs));
		}
	}
}

bool VerifyModelingProductionSourceBoundary(
	FAutomationTestBase* Test)
{
	const FString SourceRoot = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT(
			"UeAgentInterface/Source/UeAgentInterface/Private/"
			"DomainAdapters/ModelingGeometryEffect"));
	const TArray<FString> SourceFiles = {
		FPaths::Combine(
			SourceRoot,
			TEXT("UeAgentModelingGeometryEffectCapabilities.cpp")),
		FPaths::Combine(
			SourceRoot,
			TEXT("UeAgentModelingGeometryEffectNative.cpp")),
	};
	FString Combined;
	for (const FString& Path : SourceFiles)
	{
		FString Text;
		if (!Test->TestTrue(
				*(Path + TEXT(" exists for boundary audit")),
				FFileHelper::LoadFileToString(Text, *Path)))
		{
			return false;
		}
		Combined += Text;
	}
	for (const FString& Forbidden
		: TArray<FString>{
			TEXT("ModelingToolsEditorMode"),
				TEXT("InteractiveToolManager"),
				TEXT("UInteractiveTool"),
				TEXT("GLevelEditorModeTools"),
				TEXT("GetSelectedActors"),
				TEXT("GetSelectedComponents"),
				TEXT("tool_properties"),
				TEXT("modeling_create_"),
				TEXT("modeling_bevel_"),
			})
	{
		Test->TestFalse(
			*(Forbidden
				+ TEXT(" is absent from production owner")),
			Combined.Contains(Forbidden));
	}
	return !Test->HasAnyErrors();
}

bool RunModelingProductionOwnedCarrierTests(
	FAutomationTestBase* Test,
	const TSharedPtr<FJsonObject>& Fixture,
	const FString& NativeOperationFilter = FString(),
	const bool bVerifySemanticOracle = false)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!Test->TestTrue(
			TEXT("create ModelingGeometry production registry"),
			TryCreateModelingGeometryEffectDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid()
		|| !VerifyModelingProductionSourceBoundary(Test))
	{
		return false;
	}

	int32 ExecutedProfileCount = 0;
	for (const TSharedPtr<FJsonValue>& ScenarioValue
		: Fixture->GetArrayField(TEXT("e2_scenarios")))
	{
		const TSharedPtr<FJsonObject> Scenario =
			ScenarioValue->AsObject();
		const FString DescriptorId =
			Scenario->GetStringField(TEXT("descriptor_id"));
		const TSharedPtr<FJsonObject> Row =
			FindModelingDescriptorRow(Fixture, DescriptorId);
		if (!Row.IsValid())
		{
			Test->AddError(
				DescriptorId + TEXT(":fixture_row_missing"));
			continue;
		}
		if (!NativeOperationFilter.IsEmpty()
			&& Row->GetStringField(
				TEXT("native_operation_type"))
				!= NativeOperationFilter)
		{
			continue;
		}
		++ExecutedProfileCount;
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(DescriptorId);
		if (!Test->TestNotNull(
				*(DescriptorId + TEXT(" descriptor exists")),
				Descriptor))
		{
			continue;
		}

		FOwnedDynamicMeshWorld Owner;
		if (!Test->TestTrue(
			*(DescriptorId + TEXT(" owns a fresh carrier")),
			Owner.IsValid()))
		{
			continue;
		}
		PrepareModelingScenarioMesh(
			Scenario->GetStringField(TEXT("fixture_kind")),
			*Owner.DynamicMesh);
		UDynamicMeshComponent* TargetComponent =
			Owner.Actor->GetDynamicMeshComponent();
		if (!TargetComponent)
		{
			Test->AddError(
				DescriptorId + TEXT(":target_component_missing"));
			continue;
		}
		TargetComponent->SetMaterial(
			0,
			UMaterial::GetDefaultMaterial(MD_Surface));
		TargetComponent->SetMaterial(
			1,
			UMaterial::GetDefaultMaterial(MD_Surface));
		FString TestCarrierKind;
		const bool bStaticCarrier =
			Row->TryGetStringField(
				TEXT("test_carrier_kind"),
				TestCarrierKind)
			&& TestCarrierKind == TEXT("static_mesh_asset");
		UStaticMesh* StaticTarget = bStaticCarrier
			? CreateModelingStaticMeshCarrier(Owner)
			: nullptr;
		if (bStaticCarrier
			&& !Test->TestNotNull(
				*(DescriptorId
					+ TEXT(" owns a static-mesh carrier")),
				StaticTarget))
		{
			continue;
		}
		Owner.Package->SetDirtyFlag(false);

		bool bUsesOperand = false;
		Row->TryGetBoolField(TEXT("operand"), bUsesOperand);
		ADynamicMeshActor* Operand = bUsesOperand
			? Owner.SpawnCubeActor(
				TEXT("ModelingGeometryOwnedOperand"),
				FVector(1025.0, 2000.0, 300.0))
			: nullptr;

		const TSharedRef<
			FOwnedReferenceResolver,
			ESPMode::ThreadSafe> Resolver =
				MakeShared<
					FOwnedReferenceResolver,
					ESPMode::ThreadSafe>();
		Resolver->Objects.Add(
			TEXT("uai://mesh/fixture-target"),
			bStaticCarrier
				? static_cast<UObject*>(StaticTarget)
				: static_cast<UObject*>(Owner.Actor));
		if (Operand)
		{
			Resolver->Objects.Add(
				TEXT("uai://mesh/fixture-operand"),
				Operand);
		}
		Resolver->Objects.Add(
			TEXT("uai://material/default"),
			UMaterial::GetDefaultMaterial(MD_Surface));

		bool bPrimitive = false;
		Row->TryGetBoolField(TEXT("primitive_output"), bPrimitive);
		FModelingSnapshotOptions Options;
		Options.bIncludeTarget = !bPrimitive;
		Options.bIncludeOperand = bUsesOperand;
		Options.bDeclareBoundaryEdges = true;
		Options.CarrierKind = bStaticCarrier
			? TEXT("static_mesh_asset")
			: TEXT("dynamic_mesh_actor");
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> SnapshotResources =
				MakeModelingResources();
		const FUeAgentCanonicalSnapshotPtr Snapshot =
			MakeModelingSnapshot(
				Test,
				*Descriptor,
				Owner,
				Operand,
				Options,
				*SnapshotResources);
		if (!Snapshot.IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Input =
			BuildModelingValidInput(Row);
		if (bVerifySemanticOracle)
		{
			PatchModelingHighSignalInput(Row, Input);
		}
		PatchModelingSelectionForLiveMesh(
			Row,
			Input,
			*Owner.DynamicMesh);
		FModelingCarrierReadback Before;
		if (!Test->TestTrue(
			*(DescriptorId
				+ TEXT(" reads target prestate independently")),
			bStaticCarrier
				? ReadModelingStaticCarrier(*StaticTarget, Before)
				: ReadModelingCarrier(*TargetComponent, Before)))
		{
			continue;
		}
		FModelingCarrierReadback OperandBefore;
		if (Operand)
		{
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" reads operand prestate independently")),
				ReadModelingCarrier(
					*Operand->GetDynamicMeshComponent(),
					OperandBefore));
		}
		const bool bDirtyBefore = Owner.Package->IsDirty();
		const int32 ActorCountBefore =
			Owner.CountDynamicMeshActors();

		FAppliedModelingCapability Applied;
		if (!ApplyModelingCapability(
				Test,
				Resolver,
				Registry.ToSharedRef(),
				Owner,
				Row,
				Input,
				Operand,
				Snapshot.ToSharedRef(),
				Applied))
		{
			continue;
		}

		if (bPrimitive)
		{
			Test->TestEqual(
				*(DescriptorId + TEXT(" creates one mesh actor")),
				Owner.CountDynamicMeshActors(),
				ActorCountBefore + 1);
			if (bVerifySemanticOracle)
			{
				ADynamicMeshActor* Created =
					FindExactDynamicMeshActor(
						*Owner.World,
						TEXT("uai://mesh/fixture-target"));
				FModelingCarrierReadback CreatedState;
				if (Test->TestNotNull(
						*(DescriptorId
							+ TEXT(
								" resolves created logical identity")),
						Created)
					&& Test->TestTrue(
						*(DescriptorId
							+ TEXT(
								" reads created carrier independently")),
						Created
							&& ReadModelingCarrier(
								*Created
									->GetDynamicMeshComponent(),
								CreatedState)))
				{
					VerifyPrimitiveSemanticOracle(
						Test,
						DescriptorId,
						Row->GetStringField(
							TEXT("operation_key")),
						*Created,
						CreatedState);
				}
			}
		}
		else
		{
			FModelingCarrierReadback After;
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" reads committed carrier independently")),
				bStaticCarrier
					? ReadModelingStaticCarrier(
						*StaticTarget,
						After)
					: ReadModelingCarrier(
						*TargetComponent,
						After));
			Test->TestFalse(
				*(DescriptorId
					+ TEXT(" changes its declared oracle state")),
				After == Before);
			if (bVerifySemanticOracle)
			{
				const FString OperationKey =
					Row->GetStringField(TEXT("operation_key"));
				const FString NativeOperation =
					Row->GetStringField(
						TEXT("native_operation_type"));
				if (NativeOperation
					== TEXT("geometry.topology.edit"))
				{
					VerifyTopologySemanticOracle(
						Test,
						DescriptorId,
						OperationKey,
						Before,
						After);
				}
				else if (NativeOperation
					== TEXT("geometry.attribute.edit"))
				{
					VerifyAttributeSemanticOracle(
						Test,
						DescriptorId,
						OperationKey,
						Input,
						Before,
						After);
				}
				else if (NativeOperation
					== TEXT("geometry.collision.edit"))
				{
					VerifyCollisionSemanticOracle(
						Test,
						DescriptorId,
						OperationKey,
						After);
				}
			}
		}
		if (Operand)
		{
			FModelingCarrierReadback OperandAfter;
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" rereads operand independently")),
				ReadModelingCarrier(
					*Operand->GetDynamicMeshComponent(),
					OperandAfter));
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" preserves operand carrier")),
				OperandAfter == OperandBefore);
		}

		RollbackModelingCapability(
			Test,
			Owner,
			DescriptorId,
			Applied);
		if (bPrimitive)
		{
			Test->TestEqual(
				*(DescriptorId
					+ TEXT(" rollback removes created actor")),
				Owner.CountDynamicMeshActors(),
				ActorCountBefore);
		}
		else
		{
			FModelingCarrierReadback RolledBack;
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" rereads rollback state independently")),
				bStaticCarrier
					? ReadModelingStaticCarrier(
						*StaticTarget,
						RolledBack)
					: ReadModelingCarrier(
						*TargetComponent,
						RolledBack));
			const bool bRollbackMatches = RolledBack == Before;
			Test->TestTrue(
				*(DescriptorId
					+ TEXT(" rollback restores full carrier")),
				bRollbackMatches);
			if (!bRollbackMatches)
			{
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" rollback material slot count")),
					RolledBack.MaterialSlotCount,
					Before.MaterialSlotCount);
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" rollback material id checksum")),
					RolledBack.MaterialIdChecksum,
					Before.MaterialIdChecksum);
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" rollback collision shape count")),
					RolledBack.CollisionShapeCount,
					Before.CollisionShapeCount);
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" rollback uv layer count")),
					RolledBack.UvLayerCount,
					Before.UvLayerCount);
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" rollback normal element count")),
					RolledBack.NormalElementCount,
					Before.NormalElementCount);
				Test->TestEqual(
					*(DescriptorId
						+ TEXT(" rollback vertex checksum")),
					RolledBack.Mesh.VertexChecksum,
					Before.Mesh.VertexChecksum);
			}
		}
		Test->TestEqual(
			*(DescriptorId
				+ TEXT(" rollback restores package dirty state")),
			Owner.Package->IsDirty(),
			bDirtyBefore);
	}
	if (!NativeOperationFilter.IsEmpty())
	{
		const int32 ExpectedProfileCount =
			NativeOperationFilter
					== TEXT("geometry.primitive.create")
				? 7
				: NativeOperationFilter
						== TEXT("geometry.topology.edit")
					? 17
					: NativeOperationFilter
							== TEXT("geometry.attribute.edit")
						? 5
						: NativeOperationFilter
								== TEXT("geometry.collision.edit")
							? 2
							: 0;
		Test->TestEqual(
			*(NativeOperationFilter
				+ TEXT(" profile coverage")),
			ExecutedProfileCount,
			ExpectedProfileCount);
	}
	return !Test->HasAnyErrors();
}
