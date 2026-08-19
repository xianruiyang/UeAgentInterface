#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialRequests.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Code,
			const FString& Subject = FString(),
			const FString& Detail = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = Subject;
			OutError.Detail = Detail;
			return false;
		}

		struct FRequestSpec
		{
			const TCHAR* CapabilityId;
			const TCHAR* TypeId;
			const TCHAR* RequestKindId;
			const TCHAR* FamilyId;
			bool bOperation;
			bool bExpectedRevision;
			bool bCreateTarget;
			TArray<FString> IdentityFields;
			TArray<FString> RequiredFields;
			TArray<FString> OptionalFields;
		};

		const TArray<FRequestSpec>& RequestSpecs()
		{
			static const TArray<FRequestSpec> Specs = {
				{TEXT("rig.skeleton.preview_mesh.bind"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.request.v1"), TEXT("skeleton.bind_preview_mesh.request.v1"), TEXT("skeletal_attachment"), true, true, false, {TEXT("skeleton_ref"), TEXT("preview_mesh_ref")}, {TEXT("skeleton_ref"), TEXT("preview_mesh_ref"), TEXT("expected_revision")}, {}},
				{TEXT("rig.skeleton.socket.update"), TEXT("rig_simulation_spatial.skeleton.update_socket.request.v1"), TEXT("skeleton.update_socket.request.v1"), TEXT("skeletal_attachment"), true, true, false, {TEXT("skeleton_ref")}, {TEXT("skeleton_ref"), TEXT("operation"), TEXT("socket_ref"), TEXT("parent_bone_ref"), TEXT("socket_transform"), TEXT("expected_revision")}, {}},
				{TEXT("rig.control.bake.animation"), TEXT("rig_simulation_spatial.control_rig.bake_animation.request.v1"), TEXT("control_rig.bake_animation.request.v1"), TEXT("rig_bake"), true, true, false, {TEXT("source_sequence_ref"), TEXT("target_animation_ref"), TEXT("binding_ref"), TEXT("rig_ref")}, {TEXT("source_sequence_ref"), TEXT("target_animation_ref"), TEXT("binding_ref"), TEXT("rig_ref"), TEXT("authored_range"), TEXT("sample_rate"), TEXT("expected_revision")}, {}},
				{TEXT("rig.control.bake.controls"), TEXT("rig_simulation_spatial.control_rig.bake_controls.request.v1"), TEXT("control_rig.bake_controls.request.v1"), TEXT("rig_bake"), true, true, false, {TEXT("target_sequence_ref"), TEXT("binding_ref"), TEXT("rig_ref")}, {TEXT("target_sequence_ref"), TEXT("binding_ref"), TEXT("rig_ref"), TEXT("authored_range"), TEXT("sample_rate"), TEXT("expected_revision")}, {}},
				{TEXT("rig.ik.definition.update"), TEXT("rig_simulation_spatial.ik.update_definition.request.v1"), TEXT("ik.update_definition.request.v1"), TEXT("ik_retarget"), true, true, false, {TEXT("ik_rig_ref")}, {TEXT("ik_rig_ref"), TEXT("definition_changes"), TEXT("expected_revision")}, {TEXT("preview_context")}},
				{TEXT("rig.retarget.definition.update"), TEXT("rig_simulation_spatial.retarget.update_definition.request.v1"), TEXT("retarget.update_definition.request.v1"), TEXT("ik_retarget"), true, true, false, {TEXT("retargeter_ref"), TEXT("source_rig_ref"), TEXT("target_rig_ref")}, {TEXT("retargeter_ref"), TEXT("source_rig_ref"), TEXT("target_rig_ref"), TEXT("chain_mappings"), TEXT("expected_revision")}, {}},
				{TEXT("rig.retarget.pose.update"), TEXT("rig_simulation_spatial.retarget.update_pose.request.v1"), TEXT("retarget.update_pose.request.v1"), TEXT("ik_retarget"), true, true, false, {TEXT("retargeter_ref")}, {TEXT("retargeter_ref"), TEXT("subject_side"), TEXT("pose"), TEXT("pose_frame"), TEXT("expected_revision")}, {}},
				{TEXT("rig.retarget.pose.auto_align"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.request.v1"), TEXT("retarget.auto_align_pose.request.v1"), TEXT("ik_retarget"), true, true, false, {TEXT("retargeter_ref")}, {TEXT("retargeter_ref"), TEXT("subject_side"), TEXT("alignment_policy"), TEXT("expected_revision")}, {TEXT("bone_refs")}},
				{TEXT("rig.retarget.assets.generate"), TEXT("rig_simulation_spatial.retarget.generate_assets.request.v1"), TEXT("retarget.generate_assets.request.v1"), TEXT("ik_retarget"), true, false, true, {TEXT("source_subject_ref"), TEXT("target_subject_ref"), TEXT("retargeter_ref"), TEXT("destination_root")}, {TEXT("source_subject_ref"), TEXT("target_subject_ref"), TEXT("retargeter_ref"), TEXT("destination_root"), TEXT("asset_requests")}, {}},
				{TEXT("physics.material.update"), TEXT("rig_simulation_spatial.physics.update_material.request.v1"), TEXT("physics.update_material.request.v1"), TEXT("simulation_definition"), true, true, false, {TEXT("physical_material_ref")}, {TEXT("physical_material_ref"), TEXT("material_state"), TEXT("expected_revision")}, {}},
				{TEXT("physics.asset.structure.replace"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.request.v1"), TEXT("physics.replace_asset_structure.request.v1"), TEXT("simulation_definition"), true, true, false, {TEXT("physics_asset_ref")}, {TEXT("physics_asset_ref"), TEXT("bodies"), TEXT("constraints"), TEXT("profiles"), TEXT("expected_revision")}, {}},
				{TEXT("physics.constraint.update"), TEXT("rig_simulation_spatial.physics.update_constraint.request.v1"), TEXT("physics.update_constraint.request.v1"), TEXT("simulation_definition"), true, true, false, {TEXT("physics_asset_ref")}, {TEXT("physics_asset_ref"), TEXT("constraint_ref"), TEXT("endpoint_a"), TEXT("endpoint_b"), TEXT("constraint_state"), TEXT("expected_revision")}, {}},
				{TEXT("physics.simulation.probe"), TEXT("rig_simulation_spatial.physics.probe_simulation.request.v1"), TEXT("physics.probe_simulation.request.v1"), TEXT("simulation_observation"), true, false, false, {TEXT("subject_refs")}, {TEXT("subject_refs"), TEXT("simulation_session"), TEXT("simulation_range")}, {TEXT("impulses"), TEXT("velocities")}},
				{TEXT("deformer.geometry.sample"), TEXT("rig_simulation_spatial.deformer.sample_geometry.request.v1"), TEXT("deformer.sample_geometry.request.v1"), TEXT("simulation_observation"), false, false, false, {TEXT("geometry_cache_ref")}, {TEXT("geometry_cache_ref"), TEXT("sample_time"), TEXT("output_frame")}, {TEXT("vertex_selector")}},
				{TEXT("deformer.training_inputs.validate"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.request.v1"), TEXT("deformer.validate_training_inputs.request.v1"), TEXT("simulation_observation"), false, false, false, {TEXT("training_input_refs")}, {TEXT("training_input_refs"), TEXT("temporal_ranges"), TEXT("frame_mappings")}, {}},
				{TEXT("deformer.evaluation.sample"), TEXT("rig_simulation_spatial.deformer.sample_evaluation.request.v1"), TEXT("deformer.sample_evaluation.request.v1"), TEXT("simulation_observation"), false, false, false, {TEXT("deformer_ref"), TEXT("subject_ref")}, {TEXT("deformer_ref"), TEXT("subject_ref"), TEXT("sample_time"), TEXT("output_frame")}, {TEXT("vertex_selector")}},
				{TEXT("pcg.generated_spatial_data.read"), TEXT("rig_simulation_spatial.pcg.read_generated_data.request.v1"), TEXT("pcg.read_generated_data.request.v1"), TEXT("simulation_observation"), false, false, false, {TEXT("generation_identity.source_ref")}, {TEXT("generation_identity"), TEXT("data_selector"), TEXT("output_frame")}, {}},
				{TEXT("pcg.partition.spatial_state.read"), TEXT("rig_simulation_spatial.pcg.read_partition_state.request.v1"), TEXT("pcg.read_partition_state.request.v1"), TEXT("simulation_observation"), false, false, false, {TEXT("generation_identity.source_ref")}, {TEXT("generation_identity"), TEXT("partition_selector"), TEXT("output_frame")}, {}},
			};
			return Specs;
		}

		const FRequestSpec* FindSpec(const FString& CapabilityId)
		{
			return RequestSpecs().FindByPredicate(
				[&CapabilityId](const FRequestSpec& Spec)
				{
					return CapabilityId == Spec.CapabilityId;
				});
		}

		bool ValidateFields(
			const TSharedPtr<FJsonObject>& Object,
			const TArray<FString>& Required,
			const TArray<FString>& Optional,
			const FString& Subject,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			if (!Object.IsValid())
			{
				return Fail(Error, TEXT("rig_simulation_spatial_object_invalid"), Subject);
			}
			for (const FString& Field : Required)
			{
				if (!Object->HasField(Field))
				{
					return Fail(Error,
						Field.EndsWith(TEXT("_ref")) || Field.EndsWith(TEXT("_refs"))
							? TEXT("rig_simulation_spatial_required_identity_missing")
							: Field == TEXT("expected_revision")
								? TEXT("rig_simulation_spatial_expected_revision_missing")
								: TEXT("rig_simulation_spatial_required_field_missing"),
						Subject, Field);
				}
			}
			TSet<FString> Allowed;
			for (const FString& Field : Required) Allowed.Add(Field);
			for (const FString& Field : Optional) Allowed.Add(Field);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (!Allowed.Contains(Pair.Key))
				{
					return Fail(Error,
						TEXT("rig_simulation_spatial_unknown_field"), Subject, Pair.Key);
				}
			}
			return true;
		}

		bool RequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			if (!Object.IsValid() || !Object->TryGetStringField(Field, Out))
			{
				return Fail(Error,
					FString(Field).EndsWith(TEXT("_ref"))
						? TEXT("rig_simulation_spatial_required_identity_missing")
						: TEXT("rig_simulation_spatial_required_string_missing"),
					Field);
			}
			Out.TrimStartAndEndInline();
			return !Out.IsEmpty() || Fail(Error,
				FString(Field).EndsWith(TEXT("_ref"))
					? TEXT("rig_simulation_spatial_required_identity_missing")
					: TEXT("rig_simulation_spatial_required_string_missing"), Field);
		}

		bool OptionalString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			if (!Object->HasField(Field)) return true;
			return RequiredString(Object, Field, Out, Error);
		}

		bool RequiredObject(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Object.IsValid() || !Object->TryGetObjectField(Field, Found)
				|| !Found || !Found->IsValid())
			{
				return Fail(Error,
					Field == FString(TEXT("expected_revision"))
						? TEXT("rig_simulation_spatial_expected_revision_missing")
						: TEXT("rig_simulation_spatial_required_object_missing"), Field);
			}
			Out = *Found;
			return true;
		}

		bool OptionalObject(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& Out,
			bool& bPresent,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			bPresent = Object.IsValid() && Object->HasField(Field);
			return !bPresent || RequiredObject(Object, Field, Out, Error);
		}

		bool StringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FString>& Out,
			bool bRequired,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(Field, Values) || !Values)
			{
				return !bRequired || Fail(Error,
					FString(Field).EndsWith(TEXT("_refs"))
						? TEXT("rig_simulation_spatial_required_identity_missing")
						: TEXT("rig_simulation_spatial_required_array_missing"), Field);
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (!Value.IsValid() || !Value->TryGetString(Text))
				{
					return Fail(Error, TEXT("rig_simulation_spatial_string_array_invalid"), Field);
				}
				Text.TrimStartAndEndInline();
				if (Text.IsEmpty()) return Fail(Error,
					TEXT("rig_simulation_spatial_string_array_invalid"), Field);
				Out.Add(MoveTemp(Text));
			}
			return (!bRequired || !Out.IsEmpty()) || Fail(Error,
				TEXT("rig_simulation_spatial_required_array_empty"), Field);
		}

		template <typename ItemType, typename ParserType>
		bool ObjectArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<ItemType>& Out,
			bool bRequired,
			ParserType Parser,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(Field, Values) || !Values)
			{
				return !bRequired || Fail(Error,
					TEXT("rig_simulation_spatial_required_array_missing"), Field);
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> ItemObject =
					Value.IsValid() ? Value->AsObject() : nullptr;
				ItemType Item;
				if (!ItemObject.IsValid() || !Parser(ItemObject, Item, Error)) return false;
				Out.Add(MoveTemp(Item));
			}
			return (!bRequired || !Out.IsEmpty()) || Fail(Error,
				TEXT("rig_simulation_spatial_required_array_empty"), Field);
		}

		bool OptionalBool(const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field, TOptional<bool>& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out.Reset();
			if (!Object->HasField(Field)) return true;
			bool Value = false;
			if (!Object->TryGetBoolField(Field, Value))
				return Fail(Error, TEXT("rig_simulation_spatial_boolean_invalid"), Field);
			Out = Value;
			return true;
		}

		bool ParseRevision(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialRevision& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			FString Sequence;
			return ValidateFields(Object,
				{TEXT("source_id"), TEXT("generation"), TEXT("sequence")}, {},
				TEXT("expected_revision"), Error)
				&& RequiredString(Object, TEXT("source_id"), Out.SourceId, Error)
				&& RequiredString(Object, TEXT("generation"), Out.Generation, Error)
				&& RequiredString(Object, TEXT("sequence"), Sequence, Error)
				&& LexTryParseString(Out.Sequence, *Sequence)
				&& Out.Sequence >= 0
				&& (Out.IsValid() || Fail(Error,
					TEXT("rig_simulation_spatial_revision_invalid")));
		}

		bool ParseRate(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialRate& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			return ValidateFields(Object,
				{TEXT("numerator"), TEXT("denominator")}, {}, TEXT("rate"), Error)
				&& RequiredString(Object, TEXT("numerator"), Out.Numerator, Error)
				&& RequiredString(Object, TEXT("denominator"), Out.Denominator, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("rig_simulation_spatial_rate_invalid")));
		}

		bool ParsePoint(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPoint& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Rate;
			return ValidateFields(Object,
				{TEXT("clock_id"), TEXT("tick"), TEXT("rate"), TEXT("state")}, {},
				TEXT("temporal_point"), Error)
				&& RequiredString(Object, TEXT("clock_id"), Out.ClockId, Error)
				&& RequiredString(Object, TEXT("tick"), Out.Tick, Error)
				&& RequiredObject(Object, TEXT("rate"), Rate, Error)
				&& ParseRate(Rate, Out.Rate, Error)
				&& RequiredString(Object, TEXT("state"), Out.State, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("rig_simulation_spatial_temporal_point_invalid")));
		}

		bool ParseRange(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialRange& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Rate;
			return ValidateFields(Object,
				{TEXT("clock_id"), TEXT("lower_tick"), TEXT("upper_tick"), TEXT("rate"), TEXT("state")}, {},
				TEXT("temporal_range"), Error)
				&& RequiredString(Object, TEXT("clock_id"), Out.ClockId, Error)
				&& RequiredString(Object, TEXT("lower_tick"), Out.LowerTick, Error)
				&& RequiredString(Object, TEXT("upper_tick"), Out.UpperTick, Error)
				&& RequiredObject(Object, TEXT("rate"), Rate, Error)
				&& ParseRate(Rate, Out.Rate, Error)
				&& RequiredString(Object, TEXT("state"), Out.State, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("rig_simulation_spatial_temporal_range_invalid")));
		}

		bool ParseFrame(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialFrame3d& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			double Dimension = 0.0;
			return ValidateFields(Object,
				{TEXT("frame_id"), TEXT("basis_frame_id"), TEXT("dimension"), TEXT("unit"), TEXT("basis_evidence_id")}, {},
				TEXT("frame_3d"), Error)
				&& RequiredString(Object, TEXT("frame_id"), Out.FrameId, Error)
				&& RequiredString(Object, TEXT("basis_frame_id"), Out.BasisFrameId, Error)
				&& Object->TryGetNumberField(TEXT("dimension"), Dimension)
				&& (Out.Dimension = static_cast<int32>(Dimension)) == 3
				&& RequiredString(Object, TEXT("unit"), Out.Unit, Error)
				&& RequiredString(Object, TEXT("basis_evidence_id"), Out.BasisEvidenceId, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("rig_simulation_spatial_frame_invalid")));
		}

		bool ParseTransform(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialTransform3d& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Frame;
			return ValidateFields(Object,
				{TEXT("subject"), TEXT("quantity"), TEXT("frame"), TEXT("components"), TEXT("conversion_evidence_ids")}, {},
				TEXT("transform_3d"), Error)
				&& RequiredString(Object, TEXT("subject"), Out.Subject, Error)
				&& RequiredString(Object, TEXT("quantity"), Out.Quantity, Error)
				&& RequiredObject(Object, TEXT("frame"), Frame, Error)
				&& ParseFrame(Frame, Out.Frame, Error)
				&& StringArray(Object, TEXT("components"), Out.Components, true, Error)
				&& StringArray(Object, TEXT("conversion_evidence_ids"),
					Out.ConversionEvidenceIds, true, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("rig_simulation_spatial_transform_invalid")));
		}

		bool ParseIkChange(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialIkDefinitionChange& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("kind"), TEXT("operation"), TEXT("primary_ref")},
				{TEXT("secondary_ref"), TEXT("tertiary_ref"), TEXT("solver_kind")},
				TEXT("ik_definition_change"), Error)
				&& RequiredString(Object, TEXT("kind"), Out.Kind, Error)
				&& RequiredString(Object, TEXT("operation"), Out.Operation, Error)
				&& RequiredString(Object, TEXT("primary_ref"), Out.PrimaryRef, Error)
				&& OptionalString(Object, TEXT("secondary_ref"), Out.SecondaryRef, Error)
				&& OptionalString(Object, TEXT("tertiary_ref"), Out.TertiaryRef, Error)
				&& OptionalString(Object, TEXT("solver_kind"), Out.SolverKind, Error);
		}

		bool ParsePoseItem(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPoseItem& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Transform;
			return ValidateFields(Object, {TEXT("bone_ref"), TEXT("transform")}, {},
				TEXT("pose_item"), Error)
				&& RequiredString(Object, TEXT("bone_ref"), Out.BoneRef, Error)
				&& RequiredObject(Object, TEXT("transform"), Transform, Error)
				&& ParseTransform(Transform, Out.Transform, Error);
		}

		bool ParseChainMapping(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialChainMapping& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("source_chain_ref"), TEXT("target_chain_ref")}, {TEXT("goal_ref")},
				TEXT("chain_mapping"), Error)
				&& RequiredString(Object, TEXT("source_chain_ref"), Out.SourceChainRef, Error)
				&& RequiredString(Object, TEXT("target_chain_ref"), Out.TargetChainRef, Error)
				&& OptionalString(Object, TEXT("goal_ref"), Out.GoalRef, Error);
		}

		bool ParseAssetRequest(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialAssetRequest& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("asset_kind"), TEXT("name"), TEXT("overwrite_policy")}, {},
				TEXT("asset_request"), Error)
				&& RequiredString(Object, TEXT("asset_kind"), Out.AssetKind, Error)
				&& RequiredString(Object, TEXT("name"), Out.Name, Error)
				&& RequiredString(Object, TEXT("overwrite_policy"), Out.OverwritePolicy, Error);
		}

		bool ParseMaterialState(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialMaterialState& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object, {},
				{TEXT("friction"), TEXT("static_friction"), TEXT("restitution"), TEXT("density"), TEXT("raise_mass_to_power"), TEXT("friction_combine_mode"), TEXT("restitution_combine_mode")},
				TEXT("physical_material_state"), Error)
				&& !Object->Values.IsEmpty()
				&& OptionalString(Object, TEXT("friction"), Out.Friction, Error)
				&& OptionalString(Object, TEXT("static_friction"), Out.StaticFriction, Error)
				&& OptionalString(Object, TEXT("restitution"), Out.Restitution, Error)
				&& OptionalString(Object, TEXT("density"), Out.Density, Error)
				&& OptionalString(Object, TEXT("raise_mass_to_power"), Out.RaiseMassToPower, Error)
				&& OptionalString(Object, TEXT("friction_combine_mode"), Out.FrictionCombineMode, Error)
				&& OptionalString(Object, TEXT("restitution_combine_mode"), Out.RestitutionCombineMode, Error);
		}

		bool ParseBodyState(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPhysicsBodyState& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object, {},
				{TEXT("collision_enabled"), TEXT("physics_type"), TEXT("mass_kg"), TEXT("consider_for_bounds")},
				TEXT("physics_body_state"), Error)
				&& !Object->Values.IsEmpty()
				&& OptionalString(Object, TEXT("collision_enabled"), Out.CollisionEnabled, Error)
				&& OptionalString(Object, TEXT("physics_type"), Out.PhysicsType, Error)
				&& OptionalString(Object, TEXT("mass_kg"), Out.MassKg, Error)
				&& OptionalBool(Object, TEXT("consider_for_bounds"), Out.ConsiderForBounds, Error);
		}

		bool ParseConstraintState(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialConstraintState& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			if (!ValidateFields(Object, {},
				{TEXT("linear_x_motion"), TEXT("linear_y_motion"), TEXT("linear_z_motion"), TEXT("linear_limit_cm"), TEXT("twist_motion"), TEXT("swing1_motion"), TEXT("swing2_motion"), TEXT("twist_limit_degrees"), TEXT("swing1_limit_degrees"), TEXT("swing2_limit_degrees"), TEXT("disable_collision")},
				TEXT("physics_constraint_state"), Error) || Object->Values.IsEmpty())
			{
				return false;
			}
			return OptionalString(Object, TEXT("linear_x_motion"), Out.LinearXMotion, Error)
				&& OptionalString(Object, TEXT("linear_y_motion"), Out.LinearYMotion, Error)
				&& OptionalString(Object, TEXT("linear_z_motion"), Out.LinearZMotion, Error)
				&& OptionalString(Object, TEXT("linear_limit_cm"), Out.LinearLimitCm, Error)
				&& OptionalString(Object, TEXT("twist_motion"), Out.TwistMotion, Error)
				&& OptionalString(Object, TEXT("swing1_motion"), Out.Swing1Motion, Error)
				&& OptionalString(Object, TEXT("swing2_motion"), Out.Swing2Motion, Error)
				&& OptionalString(Object, TEXT("twist_limit_degrees"), Out.TwistLimitDegrees, Error)
				&& OptionalString(Object, TEXT("swing1_limit_degrees"), Out.Swing1LimitDegrees, Error)
				&& OptionalString(Object, TEXT("swing2_limit_degrees"), Out.Swing2LimitDegrees, Error)
				&& OptionalBool(Object, TEXT("disable_collision"), Out.DisableCollision, Error);
		}

		bool ParseEndpoint(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialConstraintEndpoint& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Frame;
			return ValidateFields(Object, {TEXT("body_ref"), TEXT("frame")}, {},
				TEXT("constraint_endpoint"), Error)
				&& RequiredString(Object, TEXT("body_ref"), Out.BodyRef, Error)
				&& RequiredObject(Object, TEXT("frame"), Frame, Error)
				&& ParseTransform(Frame, Out.Frame, Error);
		}

		bool ParseBody(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPhysicsBody& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> State;
			return ValidateFields(Object,
				{TEXT("body_ref"), TEXT("bone_ref"), TEXT("shape_refs"), TEXT("state")}, {},
				TEXT("physics_body"), Error)
				&& RequiredString(Object, TEXT("body_ref"), Out.BodyRef, Error)
				&& RequiredString(Object, TEXT("bone_ref"), Out.BoneRef, Error)
				&& StringArray(Object, TEXT("shape_refs"), Out.ShapeRefs, true, Error)
				&& RequiredObject(Object, TEXT("state"), State, Error)
				&& ParseBodyState(State, Out.State, Error);
		}

		bool ParseConstraint(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPhysicsConstraint& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> A;
			TSharedPtr<FJsonObject> B;
			TSharedPtr<FJsonObject> State;
			return ValidateFields(Object,
				{TEXT("constraint_ref"), TEXT("endpoint_a"), TEXT("endpoint_b"), TEXT("state")}, {},
				TEXT("physics_constraint"), Error)
				&& RequiredString(Object, TEXT("constraint_ref"), Out.ConstraintRef, Error)
				&& RequiredObject(Object, TEXT("endpoint_a"), A, Error)
				&& ParseEndpoint(A, Out.EndpointA, Error)
				&& RequiredObject(Object, TEXT("endpoint_b"), B, Error)
				&& ParseEndpoint(B, Out.EndpointB, Error)
				&& RequiredObject(Object, TEXT("state"), State, Error)
				&& ParseConstraintState(State, Out.State, Error);
		}

		bool ParseProfile(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialPhysicsProfile& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("profile_name")}, {TEXT("body_refs"), TEXT("constraint_refs")},
				TEXT("physics_profile"), Error)
				&& RequiredString(Object, TEXT("profile_name"), Out.ProfileName, Error)
				&& StringArray(Object, TEXT("body_refs"), Out.BodyRefs, false, Error)
				&& StringArray(Object, TEXT("constraint_refs"), Out.ConstraintRefs, false, Error);
		}

		bool ParseSession(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialSimulationSession& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			return ValidateFields(Object,
				{TEXT("session_id"), TEXT("owner_ref"), TEXT("generation")}, {},
				TEXT("simulation_session"), Error)
				&& RequiredString(Object, TEXT("session_id"), Out.SessionId, Error)
				&& RequiredString(Object, TEXT("owner_ref"), Out.OwnerRef, Error)
				&& RequiredString(Object, TEXT("generation"), Out.Generation, Error);
		}

		bool ParseVectorSample(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialVectorSample& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Time;
			TSharedPtr<FJsonObject> Frame;
			return ValidateFields(Object,
				{TEXT("subject_ref"), TEXT("time"), TEXT("frame"), TEXT("components"), TEXT("evidence_ids")}, {},
				TEXT("vector_sample"), Error)
				&& RequiredString(Object, TEXT("subject_ref"), Out.SubjectRef, Error)
				&& RequiredObject(Object, TEXT("time"), Time, Error)
				&& ParsePoint(Time, Out.Time, Error)
				&& RequiredObject(Object, TEXT("frame"), Frame, Error)
				&& ParseFrame(Frame, Out.Frame, Error)
				&& StringArray(Object, TEXT("components"), Out.Components, true, Error)
				&& StringArray(Object, TEXT("evidence_ids"), Out.EvidenceIds, true, Error);
		}

		bool ParseSelector(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialSelector& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			if (!ValidateFields(Object, {TEXT("mode")},
				{TEXT("refs"), TEXT("start"), TEXT("count")}, TEXT("selector"), Error)
				|| !RequiredString(Object, TEXT("mode"), Out.Mode, Error)
				|| !StringArray(Object, TEXT("refs"), Out.Refs, false, Error)) return false;
			for (const TCHAR* Field : {TEXT("start"), TEXT("count")})
			{
				if (!Object->HasField(Field)) continue;
				double Value = 0.0;
				if (!Object->TryGetNumberField(Field, Value) || Value < 0.0
					|| !FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
					return Fail(Error, TEXT("rig_simulation_spatial_selector_integer_invalid"), Field);
				if (FString(Field) == TEXT("start")) Out.Start = static_cast<int32>(Value);
				else Out.Count = static_cast<int32>(Value);
			}
			return true;
		}

		bool ParseFrameMapping(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialFrameMapping& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Source;
			TSharedPtr<FJsonObject> Target;
			return ValidateFields(Object,
				{TEXT("source_frame"), TEXT("target_frame"), TEXT("conversion_evidence_ids")}, {},
				TEXT("frame_mapping"), Error)
				&& RequiredObject(Object, TEXT("source_frame"), Source, Error)
				&& ParseFrame(Source, Out.SourceFrame, Error)
				&& RequiredObject(Object, TEXT("target_frame"), Target, Error)
				&& ParseFrame(Target, Out.TargetFrame, Error)
				&& StringArray(Object, TEXT("conversion_evidence_ids"),
					Out.ConversionEvidenceIds, true, Error);
		}

		bool ParseGenerationIdentity(const TSharedPtr<FJsonObject>& Object,
			FRigSimulationSpatialGenerationIdentity& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Revision;
			return ValidateFields(Object,
				{TEXT("source_ref"), TEXT("generation"), TEXT("revision")}, {},
				TEXT("generation_identity"), Error)
				&& RequiredString(Object, TEXT("source_ref"), Out.SourceRef, Error)
				&& RequiredString(Object, TEXT("generation"), Out.Generation, Error)
				&& RequiredObject(Object, TEXT("revision"), Revision, Error)
				&& ParseRevision(Revision, Out.Revision, Error);
		}

		bool ConvertCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			OutValue.Reset();
			if (!JsonValue.IsValid()) return Fail(Error,
				TEXT("rig_simulation_spatial_intent_json_invalid"));
			FUeAgentStateSemanticError StateError;
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return FUeAgentCanonicalValue::TryNull(Resources, OutValue, StateError)
					|| Fail(Error, StateError.Code);
			case EJson::Boolean:
				return FUeAgentCanonicalValue::TryBoolean(JsonValue->AsBool(), Resources,
					OutValue, StateError) || Fail(Error, StateError.Code);
			case EJson::String:
				return FUeAgentCanonicalValue::TryString(JsonValue->AsString(), Resources,
					OutValue, StateError) || Fail(Error, StateError.Code);
			case EJson::Number:
			{
				const double Number = JsonValue->AsNumber();
				const bool bInteger = FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number));
				const FString Lexeme = bInteger
					? FString::Printf(TEXT("%.0f"), Number)
					: FString::SanitizeFloat(Number, 0);
				return (bInteger
					? FUeAgentCanonicalValue::TryInteger(Lexeme, Resources, OutValue, StateError)
					: FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, OutValue, StateError))
					|| Fail(Error, StateError.Code);
			}
			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const TSharedPtr<FJsonValue>& Item : JsonValue->AsArray())
				{
					FUeAgentCanonicalValuePtr Canonical;
					if (!ConvertCanonicalValue(Item, Resources, Canonical, Error)) return false;
					Items.Add(Canonical.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryArray(Items, Resources, OutValue, StateError)
					|| Fail(Error, StateError.Code);
			}
			case EJson::Object:
			{
				TArray<FString> Keys;
				JsonValue->AsObject()->Values.GetKeys(Keys);
				Keys.Sort();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				for (const FString& Key : Keys)
				{
					FUeAgentCanonicalValuePtr Canonical;
					if (!ConvertCanonicalValue(
							JsonValue->AsObject()->Values.FindChecked(Key), Resources,
							Canonical, Error)) return false;
					Items.Emplace(Key, Canonical.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryObject(Items, Resources, OutValue, StateError)
					|| Fail(Error, StateError.Code);
			}
			default:
				return Fail(Error, TEXT("rig_simulation_spatial_intent_json_kind_invalid"));
			}
		}

		EUeAgentRigSimulationSpatialPortFamily PortFamily(const FString& FamilyId)
		{
			if (FamilyId == TEXT("skeletal_attachment")) return EUeAgentRigSimulationSpatialPortFamily::SkeletalAttachment;
			if (FamilyId == TEXT("rig_bake")) return EUeAgentRigSimulationSpatialPortFamily::RigBake;
			if (FamilyId == TEXT("ik_retarget")) return EUeAgentRigSimulationSpatialPortFamily::IkRetarget;
			if (FamilyId == TEXT("simulation_definition")) return EUeAgentRigSimulationSpatialPortFamily::SimulationDefinition;
			return EUeAgentRigSimulationSpatialPortFamily::SimulationObservation;
		}

		struct FDecoded
		{
			FUeAgentRigSimulationSpatialRequestPayload Payload;
			FString PrimaryTargetRef;
			TOptional<FRigSimulationSpatialRevision> ExpectedRevision;
			TArray<FRigSimulationSpatialPoint> Points;
			TArray<FRigSimulationSpatialRange> Ranges;
			TArray<FRigSimulationSpatialTransform3d> Transforms;
			TArray<TPair<FString, FRigSimulationSpatialFrame3d>> FrameUses;
			TArray<FRigSimulationSpatialVectorSample> DirectionSamples;
		};

		bool ParseExpectedRevision(const TSharedPtr<FJsonObject>& Root,
			FDecoded& Out, FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Object;
			FRigSimulationSpatialRevision Revision;
			if (!RequiredObject(Root, TEXT("expected_revision"), Object, Error)
				|| !ParseRevision(Object, Revision, Error)) return false;
			Out.ExpectedRevision = MoveTemp(Revision);
			return true;
		}

		bool DecodePayload(const FRequestSpec& Spec,
			const TSharedPtr<FJsonObject>& Root, FDecoded& Out,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			if (!ValidateFields(Root, Spec.RequiredFields, Spec.OptionalFields,
				Spec.CapabilityId, Error)) return false;
			const FString Capability = Spec.CapabilityId;
			TSharedPtr<FJsonObject> Object;
			bool bPresent = false;

			if (Capability == TEXT("rig.skeleton.preview_mesh.bind"))
			{
				FRigSkeletonPreviewMeshBindInput Value;
				if (!RequiredString(Root, TEXT("skeleton_ref"), Value.SkeletonRef, Error)
					|| !RequiredString(Root, TEXT("preview_mesh_ref"), Value.PreviewMeshRef, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.SkeletonRef;
				Out.Payload.Set<FRigSkeletonPreviewMeshBindInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.skeleton.socket.update"))
			{
				FRigSkeletonSocketUpdateInput Value;
				if (!RequiredString(Root, TEXT("skeleton_ref"), Value.SkeletonRef, Error)
					|| !RequiredString(Root, TEXT("operation"), Value.Operation, Error)
					|| !RequiredString(Root, TEXT("socket_ref"), Value.SocketRef, Error)
					|| !RequiredString(Root, TEXT("parent_bone_ref"), Value.ParentBoneRef, Error)
					|| !RequiredObject(Root, TEXT("socket_transform"), Object, Error)
					|| !ParseTransform(Object, Value.SocketTransform, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.SkeletonRef;
				Out.Transforms.Add(Value.SocketTransform);
				Out.Payload.Set<FRigSkeletonSocketUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.control.bake.animation"))
			{
				FRigControlBakeAnimationInput Value;
				if (!RequiredString(Root, TEXT("source_sequence_ref"), Value.SourceSequenceRef, Error)
					|| !RequiredString(Root, TEXT("target_animation_ref"), Value.TargetAnimationRef, Error)
					|| !RequiredString(Root, TEXT("binding_ref"), Value.BindingRef, Error)
					|| !RequiredString(Root, TEXT("rig_ref"), Value.RigRef, Error)
					|| !RequiredObject(Root, TEXT("authored_range"), Object, Error)
					|| !ParseRange(Object, Value.AuthoredRange, Error)
					|| !RequiredObject(Root, TEXT("sample_rate"), Object, Error)
					|| !ParseRate(Object, Value.SampleRate, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.TargetAnimationRef;
				Out.Ranges.Add(Value.AuthoredRange);
				Out.Payload.Set<FRigControlBakeAnimationInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.control.bake.controls"))
			{
				FRigControlBakeControlsInput Value;
				if (!RequiredString(Root, TEXT("target_sequence_ref"), Value.TargetSequenceRef, Error)
					|| !RequiredString(Root, TEXT("binding_ref"), Value.BindingRef, Error)
					|| !RequiredString(Root, TEXT("rig_ref"), Value.RigRef, Error)
					|| !RequiredObject(Root, TEXT("authored_range"), Object, Error)
					|| !ParseRange(Object, Value.AuthoredRange, Error)
					|| !RequiredObject(Root, TEXT("sample_rate"), Object, Error)
					|| !ParseRate(Object, Value.SampleRate, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.TargetSequenceRef;
				Out.Ranges.Add(Value.AuthoredRange);
				Out.Payload.Set<FRigControlBakeControlsInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.ik.definition.update"))
			{
				FRigIkDefinitionUpdateInput Value;
				if (!RequiredString(Root, TEXT("ik_rig_ref"), Value.IkRigRef, Error)
					|| !ObjectArray(Root, TEXT("definition_changes"), Value.DefinitionChanges,
						true, ParseIkChange, Error)
					|| !ParseExpectedRevision(Root, Out, Error)
					|| !OptionalObject(Root, TEXT("preview_context"), Object, bPresent, Error)) return false;
				if (bPresent && (!ValidateFields(Object, {TEXT("preview_mesh_ref")}, {},
					TEXT("ik_preview_context"), Error)
					|| !RequiredString(Object, TEXT("preview_mesh_ref"), Value.PreviewMeshRef, Error))) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.IkRigRef;
				Out.Payload.Set<FRigIkDefinitionUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.retarget.definition.update"))
			{
				FRigRetargetDefinitionUpdateInput Value;
				if (!RequiredString(Root, TEXT("retargeter_ref"), Value.RetargeterRef, Error)
					|| !RequiredString(Root, TEXT("source_rig_ref"), Value.SourceRigRef, Error)
					|| !RequiredString(Root, TEXT("target_rig_ref"), Value.TargetRigRef, Error)
					|| !ObjectArray(Root, TEXT("chain_mappings"), Value.ChainMappings,
						true, ParseChainMapping, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.RetargeterRef;
				Out.Payload.Set<FRigRetargetDefinitionUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.retarget.pose.update"))
			{
				FRigRetargetPoseUpdateInput Value;
				if (!RequiredString(Root, TEXT("retargeter_ref"), Value.RetargeterRef, Error)
					|| !RequiredString(Root, TEXT("subject_side"), Value.SubjectSide, Error)
					|| !ObjectArray(Root, TEXT("pose"), Value.Pose, true, ParsePoseItem, Error)
					|| !RequiredObject(Root, TEXT("pose_frame"), Object, Error)
					|| !ParseFrame(Object, Value.PoseFrame, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.RetargeterRef;
				Out.FrameUses.Emplace(Value.RetargeterRef, Value.PoseFrame);
				for (const FRigSimulationSpatialPoseItem& Item : Value.Pose)
					Out.Transforms.Add(Item.Transform);
				Out.Payload.Set<FRigRetargetPoseUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.retarget.pose.auto_align"))
			{
				FRigRetargetPoseAutoAlignInput Value;
				if (!RequiredString(Root, TEXT("retargeter_ref"), Value.RetargeterRef, Error)
					|| !RequiredString(Root, TEXT("subject_side"), Value.SubjectSide, Error)
					|| !RequiredString(Root, TEXT("alignment_policy"), Value.AlignmentPolicy, Error)
					|| !ParseExpectedRevision(Root, Out, Error)
					|| !StringArray(Root, TEXT("bone_refs"), Value.BoneRefs, false, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.RetargeterRef;
				Out.Payload.Set<FRigRetargetPoseAutoAlignInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("rig.retarget.assets.generate"))
			{
				FRigRetargetAssetsGenerateInput Value;
				if (!RequiredString(Root, TEXT("source_subject_ref"), Value.SourceSubjectRef, Error)
					|| !RequiredString(Root, TEXT("target_subject_ref"), Value.TargetSubjectRef, Error)
					|| !RequiredString(Root, TEXT("retargeter_ref"), Value.RetargeterRef, Error)
					|| !RequiredString(Root, TEXT("destination_root"), Value.DestinationRoot, Error)
					|| !ObjectArray(Root, TEXT("asset_requests"), Value.AssetRequests,
						true, ParseAssetRequest, Error)) return false;
				Out.PrimaryTargetRef = Value.DestinationRoot;
				Out.Payload.Set<FRigRetargetAssetsGenerateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("physics.material.update"))
			{
				FPhysicsMaterialUpdateInput Value;
				if (!RequiredString(Root, TEXT("physical_material_ref"), Value.PhysicalMaterialRef, Error)
					|| !RequiredObject(Root, TEXT("material_state"), Object, Error)
					|| !ParseMaterialState(Object, Value.MaterialState, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.PhysicalMaterialRef;
				Out.Payload.Set<FPhysicsMaterialUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("physics.asset.structure.replace"))
			{
				FPhysicsAssetStructureReplaceInput Value;
				if (!RequiredString(Root, TEXT("physics_asset_ref"), Value.PhysicsAssetRef, Error)
					|| !ObjectArray(Root, TEXT("bodies"), Value.Bodies, true, ParseBody, Error)
					|| !ObjectArray(Root, TEXT("constraints"), Value.Constraints, true, ParseConstraint, Error)
					|| !ObjectArray(Root, TEXT("profiles"), Value.Profiles, true, ParseProfile, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.PhysicsAssetRef;
				for (const FRigSimulationSpatialPhysicsConstraint& Item : Value.Constraints)
				{
					Out.Transforms.Add(Item.EndpointA.Frame);
					Out.Transforms.Add(Item.EndpointB.Frame);
				}
				Out.Payload.Set<FPhysicsAssetStructureReplaceInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("physics.constraint.update"))
			{
				FPhysicsConstraintUpdateInput Value;
				if (!RequiredString(Root, TEXT("physics_asset_ref"), Value.PhysicsAssetRef, Error)
					|| !RequiredString(Root, TEXT("constraint_ref"), Value.ConstraintRef, Error)
					|| !RequiredObject(Root, TEXT("endpoint_a"), Object, Error)
					|| !ParseEndpoint(Object, Value.EndpointA, Error)
					|| !RequiredObject(Root, TEXT("endpoint_b"), Object, Error)
					|| !ParseEndpoint(Object, Value.EndpointB, Error)
					|| !RequiredObject(Root, TEXT("constraint_state"), Object, Error)
					|| !ParseConstraintState(Object, Value.ConstraintState, Error)
					|| !ParseExpectedRevision(Root, Out, Error)) return false;
				Value.ExpectedRevision = Out.ExpectedRevision.GetValue();
				Out.PrimaryTargetRef = Value.PhysicsAssetRef;
				Out.Transforms.Add(Value.EndpointA.Frame);
				Out.Transforms.Add(Value.EndpointB.Frame);
				Out.Payload.Set<FPhysicsConstraintUpdateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("physics.simulation.probe"))
			{
				FPhysicsSimulationProbeInput Value;
				if (!StringArray(Root, TEXT("subject_refs"), Value.SubjectRefs, true, Error)
					|| !RequiredObject(Root, TEXT("simulation_session"), Object, Error)
					|| !ParseSession(Object, Value.SimulationSession, Error)
					|| !RequiredObject(Root, TEXT("simulation_range"), Object, Error)
					|| !ParseRange(Object, Value.SimulationRange, Error)
					|| !ObjectArray(Root, TEXT("impulses"), Value.Impulses, false, ParseVectorSample, Error)
					|| !ObjectArray(Root, TEXT("velocities"), Value.Velocities, false, ParseVectorSample, Error)) return false;
				Out.PrimaryTargetRef = Value.SimulationSession.OwnerRef;
				Out.Ranges.Add(Value.SimulationRange);
				for (const FRigSimulationSpatialVectorSample& Item : Value.Impulses)
				{
					Out.Points.Add(Item.Time);
					Out.DirectionSamples.Add(Item);
				}
				for (const FRigSimulationSpatialVectorSample& Item : Value.Velocities)
				{
					Out.Points.Add(Item.Time);
					Out.DirectionSamples.Add(Item);
				}
				Out.Payload.Set<FPhysicsSimulationProbeInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("deformer.geometry.sample"))
			{
				FDeformerGeometrySampleInput Value;
				if (!RequiredString(Root, TEXT("geometry_cache_ref"), Value.GeometryCacheRef, Error)
					|| !RequiredObject(Root, TEXT("sample_time"), Object, Error)
					|| !ParsePoint(Object, Value.SampleTime, Error)
					|| !RequiredObject(Root, TEXT("output_frame"), Object, Error)
					|| !ParseFrame(Object, Value.OutputFrame, Error)
					|| !OptionalObject(Root, TEXT("vertex_selector"), Object, bPresent, Error)) return false;
				if (bPresent)
				{
					FRigSimulationSpatialSelector Selector;
					if (!ParseSelector(Object, Selector, Error)) return false;
					Value.VertexSelector = MoveTemp(Selector);
				}
				Out.PrimaryTargetRef = Value.GeometryCacheRef;
				Out.Points.Add(Value.SampleTime);
				Out.FrameUses.Emplace(Value.GeometryCacheRef, Value.OutputFrame);
				Out.Payload.Set<FDeformerGeometrySampleInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("deformer.training_inputs.validate"))
			{
				FDeformerTrainingInputsValidateInput Value;
				if (!StringArray(Root, TEXT("training_input_refs"), Value.TrainingInputRefs, true, Error)
					|| !ObjectArray(Root, TEXT("temporal_ranges"), Value.TemporalRanges, true, ParseRange, Error)
					|| !ObjectArray(Root, TEXT("frame_mappings"), Value.FrameMappings, true, ParseFrameMapping, Error)) return false;
				Out.PrimaryTargetRef = Value.TrainingInputRefs[0];
				Out.Ranges.Append(Value.TemporalRanges);
				for (const FRigSimulationSpatialFrameMapping& Mapping : Value.FrameMappings)
				{
					Out.FrameUses.Emplace(Out.PrimaryTargetRef, Mapping.SourceFrame);
					Out.FrameUses.Emplace(Out.PrimaryTargetRef, Mapping.TargetFrame);
				}
				Out.Payload.Set<FDeformerTrainingInputsValidateInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("deformer.evaluation.sample"))
			{
				FDeformerEvaluationSampleInput Value;
				if (!RequiredString(Root, TEXT("deformer_ref"), Value.DeformerRef, Error)
					|| !RequiredString(Root, TEXT("subject_ref"), Value.SubjectRef, Error)
					|| !RequiredObject(Root, TEXT("sample_time"), Object, Error)
					|| !ParsePoint(Object, Value.SampleTime, Error)
					|| !RequiredObject(Root, TEXT("output_frame"), Object, Error)
					|| !ParseFrame(Object, Value.OutputFrame, Error)
					|| !OptionalObject(Root, TEXT("vertex_selector"), Object, bPresent, Error)) return false;
				if (bPresent)
				{
					FRigSimulationSpatialSelector Selector;
					if (!ParseSelector(Object, Selector, Error)) return false;
					Value.VertexSelector = MoveTemp(Selector);
				}
				Out.PrimaryTargetRef = Value.DeformerRef;
				Out.Points.Add(Value.SampleTime);
				Out.FrameUses.Emplace(Value.SubjectRef, Value.OutputFrame);
				Out.Payload.Set<FDeformerEvaluationSampleInput>(MoveTemp(Value));
				return true;
			}
			if (Capability == TEXT("pcg.generated_spatial_data.read")
				|| Capability == TEXT("pcg.partition.spatial_state.read"))
			{
				FRigSimulationSpatialGenerationIdentity Identity;
				FRigSimulationSpatialSelector Selector;
				FRigSimulationSpatialFrame3d Frame;
				if (!RequiredObject(Root, TEXT("generation_identity"), Object, Error)
					|| !ParseGenerationIdentity(Object, Identity, Error)
					|| !RequiredObject(Root,
						Capability.Contains(TEXT("partition")) ? TEXT("partition_selector") : TEXT("data_selector"),
						Object, Error)
					|| !ParseSelector(Object, Selector, Error)
					|| !RequiredObject(Root, TEXT("output_frame"), Object, Error)
					|| !ParseFrame(Object, Frame, Error)) return false;
				Out.PrimaryTargetRef = Identity.SourceRef;
				Out.FrameUses.Emplace(Identity.SourceRef, Frame);
				if (Capability.Contains(TEXT("partition")))
				{
					FPcgPartitionSpatialStateReadInput Value;
					Value.GenerationIdentity = MoveTemp(Identity);
					Value.PartitionSelector = MoveTemp(Selector);
					Value.OutputFrame = MoveTemp(Frame);
					Out.Payload.Set<FPcgPartitionSpatialStateReadInput>(MoveTemp(Value));
				}
				else
				{
					FPcgGeneratedSpatialDataReadInput Value;
					Value.GenerationIdentity = MoveTemp(Identity);
					Value.DataSelector = MoveTemp(Selector);
					Value.OutputFrame = MoveTemp(Frame);
					Out.Payload.Set<FPcgGeneratedSpatialDataReadInput>(MoveTemp(Value));
				}
				return true;
			}
			return Fail(Error, TEXT("rig_simulation_spatial_request_decode_missing"), Capability);
		}

		bool BuildFormalPortRequest(
			const FRequestSpec& Spec,
			const FDecoded& Decoded,
			const IUeAgentRigSimulationSpatialRequestEnvironment& Environment,
			FUeAgentRigSimulationSpatialPortRequest& OutRequest,
			FUeAgentRigSimulationSpatialExecutionError& Error)
		{
			TArray<FUeAgentTemporalPoint> Points;
			for (const FRigSimulationSpatialPoint& Point : Decoded.Points)
			{
				if (!Environment.HasTemporalPath(Point.ClockId))
					return Fail(Error, TEXT("rig_simulation_spatial_clock_conversion_missing"),
						Spec.CapabilityId, Point.ClockId);
				Points.Add(Point.ToTemporalPoint());
			}
			TArray<FUeAgentTemporalRange> Ranges;
			for (const FRigSimulationSpatialRange& Range : Decoded.Ranges)
			{
				if (!Environment.HasTemporalPath(Range.ClockId))
					return Fail(Error, TEXT("rig_simulation_spatial_clock_conversion_missing"),
						Spec.CapabilityId, Range.ClockId);
				Ranges.Add(Range.ToTemporalRange());
			}

			TArray<FUeAgentSpatialValuePtr> Values;
			for (const FRigSimulationSpatialTransform3d& Transform : Decoded.Transforms)
			{
				if (!Environment.HasSpatialPath(Transform.Frame.FrameId))
					return Fail(Error, TEXT("rig_simulation_spatial_frame_conversion_missing"),
						Spec.CapabilityId, Transform.Frame.FrameId);
				FUeAgentSpatialFrame Frame(TEXT(""), TEXT(""), 0, {}, {}, TEXT(""),
					TEXT(""), FUeAgentTemporalPoint(TEXT(""), TEXT("")));
				if (!Environment.TryResolveFrame(Transform.Frame, Frame))
					return Fail(Error, TEXT("rig_simulation_spatial_frame_fact_missing"),
						Spec.CapabilityId, Transform.Frame.FrameId);
				const FUeAgentTemporalPoint ObservedAt = Frame.GetObservedAt();
				Values.Add(MakeShared<FUeAgentSpatialValue>(
					Transform.Subject, EUeAgentSpatialQuantityKind::Transform,
					MoveTemp(Frame), Transform.Components,
					ObservedAt, Transform.ConversionEvidenceIds));
			}
			for (const TPair<FString, FRigSimulationSpatialFrame3d>& Use : Decoded.FrameUses)
			{
				if (!Environment.HasSpatialPath(Use.Value.FrameId))
					return Fail(Error, TEXT("rig_simulation_spatial_frame_conversion_missing"),
						Spec.CapabilityId, Use.Value.FrameId);
				FUeAgentSpatialFrame Frame(TEXT(""), TEXT(""), 0, {}, {}, TEXT(""),
					TEXT(""), FUeAgentTemporalPoint(TEXT(""), TEXT("")));
				if (!Environment.TryResolveFrame(Use.Value, Frame))
					return Fail(Error, TEXT("rig_simulation_spatial_frame_fact_missing"),
						Spec.CapabilityId, Use.Value.FrameId);
				const FUeAgentTemporalPoint ObservedAt = Frame.GetObservedAt();
				Values.Add(MakeShared<FUeAgentSpatialValue>(
					Use.Key, EUeAgentSpatialQuantityKind::Point, MoveTemp(Frame),
					TArray<FString>{TEXT("0"), TEXT("0"), TEXT("0")}, ObservedAt,
					TArray<FString>{Use.Value.BasisEvidenceId}));
			}
			for (const FRigSimulationSpatialVectorSample& Sample
				: Decoded.DirectionSamples)
			{
				if (!Environment.HasSpatialPath(Sample.Frame.FrameId))
					return Fail(Error, TEXT("rig_simulation_spatial_frame_conversion_missing"),
						Spec.CapabilityId, Sample.Frame.FrameId);
				FUeAgentSpatialFrame Frame(TEXT(""), TEXT(""), 0, {}, {}, TEXT(""),
					TEXT(""), FUeAgentTemporalPoint(TEXT(""), TEXT("")));
				if (!Environment.TryResolveFrame(Sample.Frame, Frame))
					return Fail(Error, TEXT("rig_simulation_spatial_frame_fact_missing"),
						Spec.CapabilityId, Sample.Frame.FrameId);
				Values.Add(MakeShared<FUeAgentSpatialValue>(
					Sample.SubjectRef, EUeAgentSpatialQuantityKind::Direction,
					MoveTemp(Frame), Sample.Components, Sample.Time.ToTemporalPoint(),
					Sample.EvidenceIds));
			}
			TSet<FString> Facets;
			Facets.Add(TEXT("explicit_object_environment"));
			Facets.Add(TEXT("explicit_revision_environment"));
			Facets.Add(TEXT("explicit_frame_clock_environment"));
			OutRequest = FUeAgentRigSimulationSpatialPortRequest(
				PortFamily(Spec.FamilyId), MoveTemp(Points), MoveTemp(Ranges),
				MoveTemp(Values), Environment.GetSpatialConversions(), MoveTemp(Facets),
				Environment.GetTemporalConversions());
			return true;
		}

		class FRequestDecoder final : public IUeAgentRigSimulationSpatialRequestDecoder
		{
		public:
			explicit FRequestDecoder(
				TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TryDecode(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentContractValue& CanonicalInput,
				FUeAgentResourceTracker& Resources,
				FUeAgentRigSimulationSpatialTypedRequestPtr& OutRequest,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutRequest.Reset();
				OutError = {};
				const FRequestSpec* Spec = FindSpec(Contract.GetCapabilityId());
				if (!Spec || Contract.GetTypedRequestId() != Spec->TypeId
					|| Contract.GetFamilyId() != Spec->FamilyId
					|| Contract.RequiresNativeMutation() != Spec->bOperation)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_request_contract_mismatch"),
						Contract.GetCapabilityId());
				}
				TSharedPtr<FJsonObject> Root;
				FString JsonError;
				if (!Json::TryParseObject(CanonicalInput.GetJsonValue(), Root, JsonError)
					|| !Root.IsValid())
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_request_json_invalid"),
						Contract.GetCapabilityId(), JsonError);
				}
				FDecoded Decoded;
				if (!DecodePayload(*Spec, Root, Decoded, OutError)) return false;
				if (Decoded.PrimaryTargetRef.IsEmpty())
					return Fail(OutError,
						TEXT("rig_simulation_spatial_required_identity_missing"),
						Contract.GetCapabilityId());
				if (!Spec->bCreateTarget
					&& !Environment->HasExplicitObject(Decoded.PrimaryTargetRef))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_explicit_target_unavailable"),
						Contract.GetCapabilityId(), Decoded.PrimaryTargetRef);
				}
				if (Spec->bExpectedRevision)
				{
					if (!Decoded.ExpectedRevision.IsSet())
						return Fail(OutError,
							TEXT("rig_simulation_spatial_expected_revision_missing"),
							Contract.GetCapabilityId());
					FRigSimulationSpatialRevision Actual;
					if (!Environment->TryReadRevision(
							Decoded.PrimaryTargetRef, Actual, OutError)) return false;
					if (!(Actual == Decoded.ExpectedRevision.GetValue()))
					{
						const FRigSimulationSpatialRevision& Expected =
							Decoded.ExpectedRevision.GetValue();
						return Fail(OutError,
							TEXT("rig_simulation_spatial_revision_mismatch"),
							Contract.GetCapabilityId(),
							FString::Printf(
								TEXT("target=%s expected=%s|%s|%lld actual=%s|%s|%lld"),
								*Decoded.PrimaryTargetRef, *Expected.SourceId,
								*Expected.Generation, Expected.Sequence,
								*Actual.SourceId, *Actual.Generation, Actual.Sequence));
					}
				}
				FUeAgentRigSimulationSpatialPortRequest Formal(
					EUeAgentRigSimulationSpatialPortFamily::SimulationObservation,
					{}, {}, {}, {}, {});
				if (!BuildFormalPortRequest(*Spec, Decoded, *Environment, Formal, OutError))
					return false;
				FUeAgentCanonicalValuePtr Intent;
				if (!ConvertCanonicalValue(MakeShared<FJsonValueObject>(Root),
						Resources, Intent, OutError) || !Intent.IsValid()) return false;
				OutRequest = MakeShared<FUeAgentRigSimulationSpatialRequest>(
					Spec->TypeId, Spec->FamilyId, Spec->RequestKindId,
					Spec->bOperation, MoveTemp(Decoded.PrimaryTargetRef),
					MoveTemp(Decoded.ExpectedRevision), MoveTemp(Decoded.Payload),
					MoveTemp(Formal), Intent.ToSharedRef());
				return OutRequest.IsValid();
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> Environment;
		};
	}

	bool FRigSimulationSpatialRevision::IsValid() const
	{
		return !SourceId.IsEmpty() && !Generation.IsEmpty() && Sequence >= 0;
	}

	bool FRigSimulationSpatialRevision::operator==(
		const FRigSimulationSpatialRevision& Other) const
	{
		return SourceId == Other.SourceId && Generation == Other.Generation
			&& Sequence == Other.Sequence;
	}

	bool FRigSimulationSpatialRate::IsValid() const
	{
		int64 NumeratorValue = 0;
		int64 DenominatorValue = 0;
		return LexTryParseString(NumeratorValue, *Numerator)
			&& LexTryParseString(DenominatorValue, *Denominator)
			&& NumeratorValue > 0 && DenominatorValue > 0;
	}

	bool FRigSimulationSpatialPoint::IsValid() const
	{
		return !ClockId.IsEmpty() && !Tick.IsEmpty() && Rate.IsValid()
			&& (State == TEXT("authored") || State == TEXT("evaluation")
				|| State == TEXT("simulation"));
	}

	FUeAgentTemporalPoint FRigSimulationSpatialPoint::ToTemporalPoint() const
	{
		return FUeAgentTemporalPoint(ClockId, Tick, Rate.Numerator, Rate.Denominator, State);
	}

	bool FRigSimulationSpatialRange::IsValid() const
	{
		return !ClockId.IsEmpty() && !LowerTick.IsEmpty() && !UpperTick.IsEmpty()
			&& Rate.IsValid() && (State == TEXT("authored")
				|| State == TEXT("evaluation") || State == TEXT("simulation"));
	}

	FUeAgentTemporalRange FRigSimulationSpatialRange::ToTemporalRange() const
	{
		return FUeAgentTemporalRange(
			ClockId, State,
			FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
				ClockId, LowerTick, Rate.Numerator, Rate.Denominator, State)),
			FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
				ClockId, UpperTick, Rate.Numerator, Rate.Denominator, State)));
	}

	bool FRigSimulationSpatialFrame3d::IsValid() const
	{
		return !FrameId.IsEmpty() && !BasisFrameId.IsEmpty() && Dimension == 3
			&& !Unit.IsEmpty() && !BasisEvidenceId.IsEmpty();
	}

	bool FRigSimulationSpatialTransform3d::IsValid() const
	{
		return !Subject.IsEmpty() && Quantity == TEXT("transform") && Frame.IsValid()
			&& Components.Num() == 16 && !ConversionEvidenceIds.IsEmpty();
	}

	FUeAgentRigSimulationSpatialRequest::FUeAgentRigSimulationSpatialRequest(
		FString InTypeId,
		FString InFamilyId,
		FString InRequestKindId,
		const bool bInOperation,
		FString InPrimaryTargetRef,
		TOptional<FRigSimulationSpatialRevision> InExpectedRevision,
		FUeAgentRigSimulationSpatialRequestPayload InPayload,
		FUeAgentRigSimulationSpatialPortRequest InFormalPortRequest,
		FUeAgentCanonicalValueRef InIntentPayload)
		: TypeId(MoveTemp(InTypeId))
		, FamilyId(MoveTemp(InFamilyId))
		, RequestKindId(MoveTemp(InRequestKindId))
		, bOperation(bInOperation)
		, PrimaryTargetRef(MoveTemp(InPrimaryTargetRef))
		, ExpectedRevision(MoveTemp(InExpectedRevision))
		, Payload(MoveTemp(InPayload))
		, FormalPortRequest(MoveTemp(InFormalPortRequest))
		, IntentPayload(MoveTemp(InIntentPayload))
	{
	}

	const FString& FUeAgentRigSimulationSpatialRequest::GetTypeId() const { return TypeId; }
	const FString& FUeAgentRigSimulationSpatialRequest::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentRigSimulationSpatialRequest::GetRequestKindId() const { return RequestKindId; }
	const FString& FUeAgentRigSimulationSpatialRequest::GetPrimaryTargetRef() const { return PrimaryTargetRef; }
	const FUeAgentRigSimulationSpatialRequestPayload& FUeAgentRigSimulationSpatialRequest::GetPayload() const { return Payload; }
	const FUeAgentRigSimulationSpatialPortRequest& FUeAgentRigSimulationSpatialRequest::GetFormalPortRequest() const { return FormalPortRequest; }

	bool FUeAgentRigSimulationSpatialRequest::TryBuildChangeDraft(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FUeAgentOperationContext& Context,
		TOptional<FChangeDraft>& OutDraft,
		FUeAgentRigSimulationSpatialExecutionError& OutError) const
	{
		OutDraft.Reset();
		OutError = {};
		if (!bOperation)
			return Fail(OutError,
				TEXT("rig_simulation_spatial_query_has_no_change_draft"), RequestKindId);
		if (Snapshot.GetEntries().IsEmpty())
			return Fail(OutError,
				TEXT("rig_simulation_spatial_preflight_snapshot_empty"), RequestKindId);
		const FUeAgentStateEntry& Primary = Snapshot.GetEntries()[0];
		TArray<FUeAgentCanonicalIdentity> ImpactTargets;
		for (const FUeAgentStateEntry& Entry : Snapshot.GetEntries())
			ImpactTargets.Add(Entry.GetIdentity());
		OutDraft.Emplace(
			Primary.GetIdentity(), EChangeIntent::Update, Primary.GetValue(),
			IntentPayload, TArray<FString>{TEXT("rig-simulation-spatial:write")},
			TEXT("rig-simulation-spatial-typed-intent:") + RequestKindId,
			MoveTemp(ImpactTargets));
		return OutDraft->IsValid();
	}

	bool TryCreateRigSimulationSpatialRequestDecoder(
		TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> Environment,
		TSharedPtr<IUeAgentRigSimulationSpatialRequestDecoder>& OutDecoder,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutDecoder.Reset();
		OutError = {};
		if (RequestSpecs().Num() != 18)
			return Fail(OutError,
				TEXT("rig_simulation_spatial_request_spec_count_invalid"));
		TSet<FString> Capabilities;
		TSet<FString> Types;
		TSet<FString> Kinds;
		for (const FRequestSpec& Spec : RequestSpecs())
		{
			if (Capabilities.Contains(Spec.CapabilityId)
				|| Types.Contains(Spec.TypeId) || Kinds.Contains(Spec.RequestKindId))
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_request_spec_duplicate"), Spec.CapabilityId);
			}
			Capabilities.Add(Spec.CapabilityId);
			Types.Add(Spec.TypeId);
			Kinds.Add(Spec.RequestKindId);
		}
		OutDecoder = MakeShared<FRequestDecoder>(MoveTemp(Environment));
		return true;
	}

	bool TryGetRigSimulationSpatialIdentityFields(
		const FString& CapabilityId,
		TArray<FString>& OutFields,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutFields.Reset();
		OutError = {};
		const FRequestSpec* Spec = FindSpec(CapabilityId);
		if (!Spec) return Fail(OutError,
			TEXT("rig_simulation_spatial_request_spec_unknown"), CapabilityId);
		OutFields = Spec->IdentityFields;
		return true;
	}
}
