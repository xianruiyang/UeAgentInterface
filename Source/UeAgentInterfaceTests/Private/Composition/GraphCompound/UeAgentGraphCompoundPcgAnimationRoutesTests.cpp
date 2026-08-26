#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 131072},
			{EUeAgentResourceDimension::Items, 131072},
			{EUeAgentResourceDimension::Files, 128},
			{EUeAgentResourceDimension::ReferenceEdges, 262144},
			{EUeAgentResourceDimension::WorkUnits, 1048576},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 64 * 1024 * 1024},
		}));
	}

	FUeAgentCanonicalValuePtr MakeString(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
		return Value;
	}

	FUeAgentCanonicalIdentity MakeIdentity(
		const FString& SemanticId,
		const FString& OwnerId)
	{
		return FUeAgentCanonicalIdentity(
			SemanticId,
			TEXT("graph_element"),
			OwnerId,
			SemanticId,
			TEXT("graph_compound"),
			TEXT("generation:gc016"));
	}

	FUeAgentCanonicalSnapshotRef MakeSnapshot(
		FUeAgentResourceTracker& Resources,
		const TArray<FUeAgentCanonicalIdentity>& Identities)
	{
		const FUeAgentStateRevision Revision(
			TEXT("gc016:persisted"), TEXT("generation:gc016"), 16);
		TArray<FUeAgentStateEntry> Entries;
		for (const FUeAgentCanonicalIdentity& Identity : Identities)
		{
			const bool bPcgComponentBinding =
				Identity.GetSemanticId()
					== TEXT("binding:pcg_component_graph");
			FUeAgentCanonicalValuePtr Value;
			FUeAgentCanonicalValuePtr DefaultValue;
			FUeAgentStateSemanticError ValueError;
			if (bPcgComponentBinding)
			{
				check(FUeAgentCanonicalValue::TryObject(
					{{TEXT("actor_ref"), MakeString(
						TEXT("/Game/UAI/Map.Actor"), Resources).ToSharedRef()}},
					Resources,
					Value,
					ValueError));
				check(FUeAgentCanonicalValue::TryObject(
					{}, Resources, DefaultValue, ValueError));
			}
			else
			{
				Value = MakeString(TEXT("before"), Resources);
				DefaultValue = MakeString(TEXT(""), Resources);
			}
			Entries.Emplace(
				Identity,
				FUeAgentCanonicalType(
					bPcgComponentBinding ? TEXT("object") : TEXT("string"),
					bPcgComponentBinding
						? EUeAgentValueKind::Object
						: EUeAgentValueKind::String,
					false,
					bPcgComponentBinding ? TEXT("object") : TEXT("scalar")),
				Value.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::Schema,
					bPcgComponentBinding
						? TEXT("gc016:object-default")
						: TEXT("gc016:string-default"),
					TEXT("fixture:gc016"),
					DefaultValue.ToSharedRef()),
				FUeAgentStateDetails(
					TEXT("Graph element"), TEXT(""), {TEXT("GC-016")},
					true, TEXT("available"), {}),
				Revision,
				EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::Overridden);
		}
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("snapshot:gc016"),
			EUeAgentStatePlane::Persisted,
			Revision,
			FUeAgentTemporalPoint(TEXT("fixture"), TEXT("16")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("fixture:gc016"),
				TEXT("evidence:gc016:persisted")),
			Entries,
			Resources,
			Snapshot,
			Error));
		return Snapshot.ToSharedRef();
	}

	const TArray<FString>& ExpectedCapabilities()
	{
		static const TArray<FString> Values = {
			TEXT("ai.behavior_tree.asset.create"),
			TEXT("ai.behavior_tree.graph.apply"),
			TEXT("ai.blackboard.asset.create"),
			TEXT("ai.blackboard.schema.apply"),
			TEXT("ai.eqs.asset.create"),
			TEXT("ai.eqs.graph.apply"),
			TEXT("ai.smart_object_definition.asset.create"),
			TEXT("ai.smart_object_definition.apply"),
			TEXT("ai.state_tree.asset.create"),
			TEXT("ai.state_tree.hierarchy.apply"),
			TEXT("pcg.graph.asset.create"),
			TEXT("pcg.graph.apply"),
			TEXT("pcg.graph_instance.apply"),
			TEXT("pcg.graph_parameters.apply"),
			TEXT("pcg.component.apply"),
			TEXT("pcg.data_asset.apply"),
			TEXT("pcg.data_asset.create_from_level"),
			TEXT("pcg.component.world_binding.apply"),
			TEXT("animation.anim_blueprint.graph.apply"),
			TEXT("animation.anim_blueprint.asset.create"),
			TEXT("animation.anim_blueprint.layer_interface.asset.create"),
			TEXT("animation.blend_space.definition.apply"),
			TEXT("animation.blend_space.asset.create"),
			TEXT("animation.skeleton.definition.apply"),
			TEXT("animation.skeleton.compatibility.apply"),
			TEXT("animation.skeleton.virtual_bone.apply"),
			TEXT("animation.control_rig.hierarchy_graph.apply"),
			TEXT("animation.control_rig.asset.create"),
			TEXT("animation.control_rig.shape_library.apply"),
			TEXT("animation.control_rig.shape_library.asset.create"),
			TEXT("animation.deformer_graph.topology.apply"),
			TEXT("animation.deformer_graph.asset.create"),
			TEXT("animation.deformer_source_library.bindings.apply"),
			TEXT("animation.deformer_source_library.asset.create"),
			TEXT("animation.mesh_deformer_collection.entries.apply"),
			TEXT("animation.mesh_deformer_collection.asset.create"),
			TEXT("animation.ml_deformer.configuration.apply"),
			TEXT("animation.ml_deformer.asset.create"),
			TEXT("animation.ik_retargeter.definition.apply"),
			TEXT("animation.ik_retargeter.asset.create"),
			TEXT("animation.ik_retargeter.settings.apply"),
			TEXT("animation.ik_rig.definition.auto_generate"),
			TEXT("animation.ik_rig.definition.apply"),
			TEXT("animation.ik_rig.asset.create"),
		};
		return Values;
	}

	const TMap<FString, FString>& RouteExamples()
	{
		static const TMap<FString, FString> Values = {
			{TEXT("ai.behavior_tree.asset.create"), TEXT("{\"asset_name\":\"BT_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("ai.behavior_tree.graph.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"item\",\"identity\":\"bt_node:root\",\"intent\":\"create\",\"value\":{\"carrier_type_ref\":\"/Script/AIGraph.BehaviorTreeGraphNode_Root\",\"payload_type_ref\":\"/Script/AIModule.BTComposite_Selector\"}}],\"target_ref\":\"/Game/UAI/BT_GC016\"}")},
			{TEXT("ai.blackboard.asset.create"), TEXT("{\"asset_name\":\"BB_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("ai.blackboard.schema.apply"), TEXT("{\"entries\":[{\"entry_name\":\"Target\",\"intent\":\"create\",\"key_type_ref\":\"/Script/AIModule.BlackboardKeyType_Object\",\"ownership\":\"local\"}],\"target_ref\":\"/Game/UAI/BB_GC016\"}")},
			{TEXT("ai.eqs.asset.create"), TEXT("{\"asset_name\":\"EQS_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("ai.eqs.graph.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"item\",\"identity\":\"eqs_option:root\",\"intent\":\"create\",\"value\":{\"generator_type_ref\":\"/Script/AIModule.EnvQueryGenerator_ActorsOfClass\"}}],\"target_ref\":\"/Game/UAI/EQS_GC016\"}")},
			{TEXT("ai.smart_object_definition.asset.create"), TEXT("{\"asset_name\":\"SO_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("ai.smart_object_definition.apply"), TEXT("{\"slots\":[{\"behavior_type_ref\":\"/Script/SmartObjectsModule.SmartObjectBehaviorDefinition\",\"data_type_ref\":\"/Script/SmartObjectsModule.SmartObjectSlotDefinitionData\",\"intent\":\"create\",\"slot_guid\":\"11111111-2222-3333-4444-555555555555\"}],\"target_ref\":\"/Game/UAI/SO_GC016\"}")},
			{TEXT("ai.state_tree.asset.create"), TEXT("{\"asset_name\":\"ST_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("ai.state_tree.hierarchy.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"hierarchy\",\"identity\":\"state:root\",\"intent\":\"create\",\"value\":{\"native_guid\":\"22222222-3333-4444-5555-666666666666\",\"payload_type_ref\":\"/Script/StateTreeModule.StateTreeState\"}}],\"target_ref\":\"/Game/UAI/ST_GC016\"}")},
			{TEXT("pcg.graph.asset.create"), TEXT("{\"asset_name\":\"PCG_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("pcg.graph.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"item\",\"identity\":\"pcg_node:CreateGrid\",\"intent\":\"create\",\"value\":{\"candidate\":{\"name\":\"Create Points Grid\"},\"position\":[120,80]}}],\"target_ref\":\"/Game/UAI/PCG_GC016\"}")},
			{TEXT("pcg.graph_instance.apply"), TEXT("{\"graph_ref\":\"/Game/UAI/PCG_Source\",\"overrides\":[{\"property_guid\":\"33333333-4444-5555-6666-777777777777\",\"property_name\":\"Density\",\"property_type_ref\":\"double\",\"value\":0.5}],\"target_ref\":\"/Game/UAI/PCGI_GC016\"}")},
			{TEXT("pcg.graph_parameters.apply"), TEXT("{\"parameters\":[{\"property_guid\":\"44444444-5555-6666-7777-888888888888\",\"property_name\":\"Seed\",\"property_type_ref\":\"int32\",\"value\":42}],\"target_ref\":\"/Game/UAI/PCG_GC016\"}")},
			{TEXT("pcg.component.apply"), TEXT("{\"generation_policy\":\"on_demand\",\"graph_ref\":\"/Game/UAI/PCG_GC016\",\"target_ref\":\"component:/Game/UAI/Map.Actor:PCG\"}")},
			{TEXT("pcg.data_asset.apply"), TEXT("{\"content_revision\":\"revision:16\",\"data_type_ref\":\"/Script/PCG.PCGPointData\",\"source_ref\":\"source:points\",\"target_ref\":\"/Game/UAI/PCGD_GC016\",\"value\":{\"point_count\":8}}")} ,
			{TEXT("pcg.data_asset.create_from_level"), TEXT("{\"data_type_ref\":\"/Script/PCG.PCGPointData\",\"source_level_ref\":\"/Game/Maps/WB_WhiteBox01\",\"target_ref\":\"/Game/UAI/PCGD_FromLevel\"}")},
			{TEXT("pcg.component.world_binding.apply"), TEXT("{\"actor_ref\":\"/Game/Maps/WB_WhiteBox01.Actor_GC016\",\"component_ref\":\"/Game/Maps/WB_WhiteBox01.Actor_GC016:PCG\",\"generation_policy\":\"on_demand\",\"graph_ref\":\"/Game/UAI/PCG_GC016\"}")},
			{TEXT("animation.anim_blueprint.graph.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"item\",\"identity\":\"anim_node:root\",\"intent\":\"create\",\"value\":{\"carrier_type_ref\":\"/Script/AnimGraph.AnimGraphNode_Root\",\"runtime_type_ref\":\"/Script/Engine.AnimNode_Root\"}}],\"target_ref\":\"/Game/UAI/ABP_GC016\"}")},
			{TEXT("animation.anim_blueprint.asset.create"), TEXT("{\"asset_name\":\"ABP_GC016\",\"package_path\":\"/Game/UAI\",\"parent_class_ref\":\"/Script/Engine.AnimInstance\",\"skeleton_ref\":\"/Game/UAI/SK_GC016\"}")},
			{TEXT("animation.anim_blueprint.layer_interface.asset.create"), TEXT("{\"asset_name\":\"ALI_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("animation.blend_space.definition.apply"), TEXT("{\"axes\":[{\"axis_index\":0,\"axis_name\":\"Speed\",\"grid_divisions\":4,\"maximum\":600,\"minimum\":0}],\"samples\":[{\"animation_ref\":\"/Game/UAI/A_Run\",\"coordinates\":[600,0],\"intent\":\"create\"}],\"target_ref\":\"/Game/UAI/BS_GC016\"}")},
			{TEXT("animation.blend_space.asset.create"), TEXT("{\"asset_name\":\"BS_GC016\",\"dimension\":2,\"package_path\":\"/Game/UAI\",\"skeleton_ref\":\"/Game/UAI/SK_GC016\"}")},
			{TEXT("animation.skeleton.definition.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"hierarchy\",\"identity\":\"bone:root\",\"intent\":\"update\",\"value\":{\"native_key\":\"root\"}}],\"target_ref\":\"/Game/UAI/SK_GC016\"}")},
			{TEXT("animation.skeleton.compatibility.apply"), TEXT("{\"changes\":[{\"intent\":\"create\",\"skeleton_ref\":\"/Game/UAI/SK_Compatible\"}],\"target_ref\":\"/Game/UAI/SK_GC016\"}")},
			{TEXT("animation.skeleton.virtual_bone.apply"), TEXT("{\"intent\":\"create\",\"name\":\"VB root_ik_foot_l\",\"source_bone\":\"root\",\"target_bone\":\"ik_foot_l\",\"target_ref\":\"/Game/UAI/SK_GC016\"}")},
			{TEXT("animation.control_rig.hierarchy_graph.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"item\",\"identity\":\"rig_element:Control:FootIK\",\"intent\":\"create\",\"value\":{\"control_type_ref\":\"EulerTransform\",\"native_key\":\"Control:FootIK\"}}],\"target_ref\":\"/Game/UAI/CR_GC016\"}")},
			{TEXT("animation.control_rig.asset.create"), TEXT("{\"asset_name\":\"CR_GC016\",\"package_path\":\"/Game/UAI\",\"skeleton_ref\":\"/Game/UAI/SK_GC016\"}")},
			{TEXT("animation.control_rig.shape_library.apply"), TEXT("{\"shapes\":[{\"intent\":\"create\",\"material_ref\":\"/Game/UAI/M_Shape\",\"mesh_ref\":\"/Game/UAI/SM_Shape\",\"name\":\"Cube\",\"order_index\":0}],\"target_ref\":\"/Game/UAI/CRSL_GC016\"}")},
			{TEXT("animation.control_rig.shape_library.asset.create"), TEXT("{\"asset_name\":\"CRSL_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("animation.deformer_graph.topology.apply"), TEXT("{\"edits\":[{\"depends_on\":[],\"element_kind\":\"item\",\"identity\":\"optimus_node:comment\",\"intent\":\"create\",\"value\":{\"native_object_path\":\"DG_GC016.UpdateGraph.Comment_GC016\",\"type_ref\":\"/Script/OptimusCore.OptimusNode_Comment\"}}],\"target_ref\":\"/Game/UAI/DG_GC016\"}")},
			{TEXT("animation.deformer_graph.asset.create"), TEXT("{\"asset_name\":\"DG_GC016\",\"package_path\":\"/Game/UAI\",\"skeletal_mesh_ref\":\"/Game/UAI/SKM_GC016\"}")},
			{TEXT("animation.deformer_source_library.bindings.apply"), TEXT("{\"bindings\":[{\"asset_ref\":\"/Game/UAI/SKM_GC016\",\"binding_key\":\"InputMesh\",\"intent\":\"update\",\"property_type_ref\":\"/Script/Engine.SkeletalMesh\"}],\"concrete_class_ref\":\"/Script/OptimusCore.OptimusSkeletalMeshComponentSource\",\"target_ref\":\"/Game/UAI/DSL_GC016\"}")},
			{TEXT("animation.deformer_source_library.asset.create"), TEXT("{\"asset_name\":\"DSL_GC016\",\"concrete_class_ref\":\"/Script/OptimusCore.OptimusSkeletalMeshComponentSource\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("animation.mesh_deformer_collection.entries.apply"), TEXT("{\"entries\":[{\"entry_kind\":\"deformer\",\"entry_ref\":\"/Game/UAI/DG_GC016\",\"intent\":\"create\",\"order_index\":0}],\"target_ref\":\"/Game/UAI/MDC_GC016\"}")},
			{TEXT("animation.mesh_deformer_collection.asset.create"), TEXT("{\"asset_name\":\"MDC_GC016\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("animation.ml_deformer.configuration.apply"), TEXT("{\"animation_ref\":\"/Game/UAI/A_Train\",\"concrete_model_class_ref\":\"/Script/NearestNeighborModel.NearestNeighborModel\",\"deformer_graph_ref\":\"/Game/UAI/DG_GC016\",\"skeletal_mesh_ref\":\"/Game/UAI/SKM_GC016\",\"target_geometry_cache_ref\":\"/Game/UAI/GC_Target\",\"target_ref\":\"/Game/UAI/MLD_GC016\"}")},
			{TEXT("animation.ml_deformer.asset.create"), TEXT("{\"asset_name\":\"MLD_GC016\",\"model_class_ref\":\"/Script/NearestNeighborModel.NearestNeighborModel\",\"package_path\":\"/Game/UAI\"}")},
			{TEXT("animation.ik_retargeter.definition.apply"), TEXT("{\"chain_mappings\":[{\"intent\":\"create\",\"operation_ref\":\"/Script/IKRig.IKRetargetFKChainsOp\",\"order_index\":0,\"source_chain\":\"Leg_L\",\"target_chain\":\"Leg_L\"}],\"source_ik_rig_ref\":\"/Game/UAI/IK_Source\",\"target_ik_rig_ref\":\"/Game/UAI/IK_Target\",\"target_ref\":\"/Game/UAI/RTG_GC016\"}")},
			{TEXT("animation.ik_retargeter.asset.create"), TEXT("{\"asset_name\":\"RTG_GC016\",\"package_path\":\"/Game/UAI\",\"source_ik_rig_ref\":\"/Game/UAI/IK_Source\",\"target_ik_rig_ref\":\"/Game/UAI/IK_Target\"}")},
			{TEXT("animation.ik_retargeter.settings.apply"), TEXT("{\"settings\":[{\"chain_name\":\"Leg_L\",\"scope\":\"chain\",\"setting_name\":\"RotationMode\",\"value\":\"Interpolated\"}],\"target_ref\":\"/Game/UAI/RTG_GC016\"}")},
			{TEXT("animation.ik_rig.definition.auto_generate"), TEXT("{\"preset\":\"humanoid\",\"skeletal_mesh_ref\":\"/Game/UAI/SKM_GC016\",\"target_ref\":\"/Game/UAI/IK_GC016\"}")},
			{TEXT("animation.ik_rig.definition.apply"), TEXT("{\"chains\":[{\"end_bone\":\"foot_l\",\"goal_name\":\"Foot_L\",\"intent\":\"create\",\"name\":\"Leg_L\",\"start_bone\":\"thigh_l\"}],\"goals\":[{\"bone_name\":\"foot_l\",\"intent\":\"create\",\"name\":\"Foot_L\"}],\"retarget_root\":\"pelvis\",\"solvers\":[{\"intent\":\"create\",\"order_index\":0,\"solver_class_ref\":\"/Script/IKRig.IKRigFullBodyIKSolver\",\"solver_ref\":\"solver:FBIK\"}],\"target_ref\":\"/Game/UAI/IK_GC016\"}")},
			{TEXT("animation.ik_rig.asset.create"), TEXT("{\"asset_name\":\"IK_GC016\",\"package_path\":\"/Game/UAI\",\"skeletal_mesh_ref\":\"/Game/UAI/SKM_GC016\"}")},
		};
		return Values;
	}

	TArray<FUeAgentCanonicalIdentity> ExistingFor(
		const FGraphCompoundCanonicalPayload& Payload)
	{
		TArray<FUeAgentCanonicalIdentity> Existing;
		TSet<FString> Keys;
		TSet<FString> CreatedKeys;
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			if (Edit.GetIntent() == EChangeIntent::Create)
			{
				CreatedKeys.Add(Edit.GetIdentity().GetCanonicalKey());
			}
		}
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			if (Edit.GetIntent() != EChangeIntent::Create
				&& !Keys.Contains(Edit.GetIdentity().GetCanonicalKey()))
			{
				Existing.Add(Edit.GetIdentity());
				Keys.Add(Edit.GetIdentity().GetCanonicalKey());
			}
			for (const FUeAgentCanonicalIdentity& Dependency : Edit.GetDependencies())
			{
				if (!CreatedKeys.Contains(Dependency.GetCanonicalKey())
					&& !Keys.Contains(Dependency.GetCanonicalKey()))
				{
					Existing.Add(Dependency);
					Keys.Add(Dependency.GetCanonicalKey());
				}
			}
		}
		return Existing;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPcgAnimationExactContractTest,
	"UeAgentInterface.Composition.GraphCompound.PcgAnimation.ExactContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPcgAnimationExactContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TArray<FPcgAnimationGraphRouteProfile>& Profiles =
		GetPcgAnimationGraphRouteProfiles();
	TestEqual(TEXT("exactly 44 routes are owned"), Profiles.Num(), 44);
	TSet<FString> Capabilities;
	TSet<FString> DescriptorIds;
	int32 PcgAiCount = 0;
	int32 AnimationCount = 0;
	for (const FPcgAnimationGraphRouteProfile& Profile : Profiles)
	{
		Capabilities.Add(Profile.GetCapabilityId());
		DescriptorIds.Add(Profile.GetDescriptorId());
		PcgAiCount += Profile.GetDomainId() == TEXT("pcg_ai_node_graph") ? 1 : 0;
		AnimationCount += Profile.GetDomainId() == TEXT("animation_rig_graph") ? 1 : 0;
		TestTrue(*FString::Printf(TEXT("%s profile is valid"),
			*Profile.GetCapabilityId()), Profile.IsValid());
		TestEqual(TEXT("every route enters C3O"),
			Profile.GetExecutionContract().GetLane(),
			EGraphCompoundExecutionLane::Operation);
		TestTrue(TEXT("every route requires a native mutation"),
			Profile.GetExecutionContract().RequiresNativeMutation());
	}
	TestEqual(TEXT("PCG/AI count"), PcgAiCount, 18);
	TestEqual(TEXT("Animation/Rig count"), AnimationCount, 26);
	TestEqual(TEXT("capabilities are unique"), Capabilities.Num(), 44);
	TestEqual(TEXT("descriptor ids are unique"), DescriptorIds.Num(), 44);
	for (const FString& Capability : ExpectedCapabilities())
	{
		TestTrue(*FString::Printf(TEXT("%s is present"), *Capability),
			Capabilities.Contains(Capability));
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString Error;
	TestTrue(TEXT("exact registry compiles"),
		TryCreatePcgAnimationGraphDescriptorRegistry(Registry, Error));
	if (Registry.IsValid())
	{
		TestEqual(TEXT("registry owns 44 descriptors"),
			Registry->GetDescriptors().Num(), 44);
		TSet<FString> Hashes;
		for (const FUeAgentOperationDescriptor& Descriptor
			: Registry->GetDescriptors())
		{
			Hashes.Add(Descriptor.GetIdentity().GetDescriptorHash());
			TestTrue(TEXT("schema closes additional properties"),
				Descriptor.GetInputSchema().GetJsonValue().Contains(
					TEXT("\"additionalProperties\":false")));
			const FUeAgentContractSchema* Schema =
				Registry->FindCompiledInputSchemaExact(Descriptor.GetIdentity());
			TestNotNull(TEXT("compiled schema exists"), Schema);
			if (Schema)
			{
				TOptional<FUeAgentContractValue> Canonical;
				FUeAgentContractSchemaError SchemaError;
				TestFalse(TEXT("unknown fields fail closed"),
					Schema->TryValidateAndCanonicalize(
						FUeAgentContractValue(TEXT("{\"unexpected\":true}")),
						[](int64, const FString&, FUeAgentContractSchemaError&)
						{
							return true;
						},
						Canonical, SchemaError));
			}
			const FPcgAnimationGraphRouteProfile* Profile =
				FindPcgAnimationGraphRouteProfile(Descriptor.GetCapabilityId());
			const FUeAgentContractSchema* ResultSchema =
				Registry->FindCompiledResultSchemaExact(Descriptor.GetIdentity());
			TestNotNull(TEXT("the route profile is exact"), Profile);
			TestNotNull(TEXT("the result schema compiles"), ResultSchema);
			if (Profile && ResultSchema)
			{
				TOptional<FUeAgentContractValue> Canonical;
				FUeAgentContractSchemaError SchemaError;
				const bool bAnimation =
					Profile->GetDomainId() == TEXT("animation_rig_graph");
				const FString ResultJson = bAnimation
					? FString(TEXT(
						"{\"compile_settled\":true,"
						"\"independent_readback_evidence_id\":\"readback:1\","
						"\"mutation_applied\":true,"
						"\"persistence_receipt_id\":\"receipt:1\","
						"\"reloaded\":true,\"revision\":\"sha256:1\","
						"\"saved\":true,\"target_ref\":\"/Game/T.T\","
						"\"terminal_status\":\"succeeded\"}"))
					: FString(TEXT(
						"{\"revision\":\"sha256:1\","
						"\"target_ref\":\"/Game/T.T\"}"));
				TestTrue(
					bAnimation
						? TEXT("Animation/Rig result requires lifecycle and readback evidence")
						: TEXT("PCG/AI result keeps its existing contract"),
					ResultSchema->TryValidateAndCanonicalize(
						FUeAgentContractValue(ResultJson),
						[](int64, const FString&, FUeAgentContractSchemaError&)
						{
							return true;
						},
						Canonical,
						SchemaError));
			}
		}
		TestEqual(TEXT("descriptor hashes are exact and unique"),
			Hashes.Num(), 44);

		const FPcgAnimationGraphRouteProfile* RetargetProfile =
			FindPcgAnimationGraphRouteProfile(
				TEXT("animation.ik_retargeter.definition.apply"));
		const FUeAgentOperationDescriptor* RetargetDescriptor = RetargetProfile
			? Registry->FindByDescriptorId(RetargetProfile->GetDescriptorId())
			: nullptr;
		const FUeAgentContractSchema* RetargetSchema = RetargetDescriptor
			? Registry->FindCompiledInputSchemaExact(
				RetargetDescriptor->GetIdentity())
			: nullptr;
		TestNotNull(
			TEXT("IK Retargeter definition input schema exists"),
			RetargetSchema);
		if (RetargetSchema)
		{
			TOptional<FUeAgentContractValue> Canonical;
			FUeAgentContractSchemaError SchemaError;
			TestFalse(
				TEXT("IK Retargeter definition rejects an empty mapping set"),
				RetargetSchema->TryValidateAndCanonicalize(
					FUeAgentContractValue(TEXT(
						"{\"chain_mappings\":[],"
						"\"source_ik_rig_ref\":\"/Game/UAI/IK_Source\","
						"\"target_ik_rig_ref\":\"/Game/UAI/IK_Target\","
						"\"target_ref\":\"/Game/UAI/RTG\"}")),
					[](int64, const FString&, FUeAgentContractSchemaError&)
					{
						return true;
					},
					Canonical,
					SchemaError));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPcgAnimationSpecializedMappingTest,
	"UeAgentInterface.Composition.GraphCompound.PcgAnimation.SpecializedMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPcgAnimationSpecializedMappingTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
	for (const FString& Capability : {
		TEXT("ai.blackboard.schema.apply"),
		TEXT("pcg.graph_parameters.apply"),
		TEXT("animation.blend_space.definition.apply"),
		TEXT("animation.ik_rig.definition.apply"),
		TEXT("animation.ik_retargeter.settings.apply")})
	{
		const FPcgAnimationGraphRouteProfile* Profile =
			FindPcgAnimationGraphRouteProfile(Capability);
		TestNotNull(*FString::Printf(TEXT("%s exists"), *Capability), Profile);
		if (!Profile)
		{
			continue;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		FGraphCompoundRouteError Error;
		TestTrue(*FString::Printf(TEXT("%s decodes specialized fields"),
			*Capability), TryDecodePcgAnimationGraphTypedOperation(
				*Profile,
				FUeAgentContractValue(RouteExamples().FindChecked(Capability)),
				Empty, Resources, Payload, Error));
		TestTrue(TEXT("specialized mapping produces identity edits"),
			!Payload.GetEdits().IsEmpty());
		for (const FGraphCompoundCanonicalEdit& Edit : Payload.GetEdits())
		{
			TestFalse(TEXT("identity is stable and non-empty"),
				Edit.GetIdentity().GetSemanticId().IsEmpty());
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundBehaviorTreeBindingContractTest,
	"UeAgentInterface.Composition.GraphCompound.PcgAnimation.BehaviorTreeBindingContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundBehaviorTreeBindingContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FPcgAnimationGraphRouteProfile* Profile =
		FindPcgAnimationGraphRouteProfile(TEXT("ai.behavior_tree.graph.apply"));
	if (!TestNotNull(TEXT("behavior tree graph route exists"), Profile))
	{
		return false;
	}
	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
	FGraphCompoundCanonicalPayload Payload(
		GetGraphCompoundCanonicalSchemaId(), {});
	FGraphCompoundRouteError Error;
	const FUeAgentContractValue Input(TEXT(
		"{\"edits\":[{\"depends_on\":[],\"element_kind\":\"binding\","
		"\"identity\":\"binding:blackboard_asset\",\"intent\":\"create\","
		"\"value\":\"/Game/UAI/BB_GC016\"}],"
		"\"target_ref\":\"/Game/UAI/BT_GC016\"}"));
	TestTrue(TEXT("behavior tree route accepts the Blackboard binding element"),
		TryDecodePcgAnimationGraphTypedOperation(
			*Profile, Input, Empty, Resources, Payload, Error));
	if (!HasAnyErrors())
	{
		TestEqual(TEXT("one binding edit is preserved"),
			Payload.GetEdits().Num(), 1);
		if (Payload.GetEdits().Num() == 1)
		{
			TestEqual(TEXT("binding identity remains exact"),
				Payload.GetEdits()[0].GetIdentity().GetSemanticId(),
				FString(TEXT("binding:blackboard_asset")));
			TestEqual(TEXT("binding element kind remains exact"),
				Payload.GetEdits()[0].GetElementKind(),
				EGraphCompoundElementKind::Binding);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPcgAnimationAllRoutesTest,
	"UeAgentInterface.Composition.GraphCompound.PcgAnimation.AllRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPcgAnimationAllRoutesTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("all 44 routes have an owned typed example"),
		RouteExamples().Num(), 44);
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(TEXT("registry builds"),
		TryCreatePcgAnimationGraphDescriptorRegistry(Registry, RegistryError));
	if (!Registry.IsValid())
	{
		return false;
	}

	for (const TPair<FString, FString>& Case : RouteExamples())
	{
		FUeAgentResourceTracker Resources = MakeTracker();
		TArray<FUeAgentCanonicalIdentity> RequiredState;
		if (Case.Key == TEXT("pcg.component.apply"))
		{
			RequiredState.Add(MakeIdentity(
				TEXT("binding:pcg_component_graph"),
				TEXT("component:/Game/UAI/Map.Actor:PCG")));
		}
		const FUeAgentCanonicalSnapshotRef Empty =
			MakeSnapshot(Resources, RequiredState);
		const FPcgAnimationGraphRouteProfile* Profile =
			FindPcgAnimationGraphRouteProfile(Case.Key);
		TestNotNull(*FString::Printf(TEXT("%s profile exists"), *Case.Key),
			Profile);
		if (!Profile)
		{
			continue;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		FGraphCompoundRouteError Error;
		TestTrue(*FString::Printf(TEXT("%s decodes"), *Case.Key),
			TryDecodePcgAnimationGraphTypedOperation(
				*Profile, FUeAgentContractValue(Case.Value), Empty,
				Resources, Payload, Error));
		if (Payload.GetEdits().IsEmpty())
		{
			AddError(FString::Printf(TEXT("%s produced no identity edits"),
				*Case.Key));
			continue;
		}
		const FUeAgentCanonicalSnapshotRef Before =
			MakeSnapshot(Resources, ExistingFor(Payload));
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(Profile->GetDescriptorId());
		TestNotNull(TEXT("descriptor exists"), Descriptor);
		if (!Descriptor)
		{
			continue;
		}
		TOptional<FChangeSetDraft> Draft;
		TestTrue(*FString::Printf(TEXT("%s builds a C6 draft"), *Case.Key),
			TryBuildPcgAnimationGraphTypedOperationDraft(
				*Profile, Descriptor->GetIdentity(),
				FUeAgentContractValue(Case.Value), Before,
				TEXT("request:gc016:all"),
				TEXT("idempotency:gc016:all"),
				Resources, Draft, Error));
		TestTrue(TEXT("draft is present"), Draft.IsSet());
		if (Draft.IsSet())
		{
			TestEqual(TEXT("per-identity cardinality is preserved"),
				Draft->GetChanges().Num(), Payload.GetEdits().Num());
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPcgAnimationUnboundedHierarchyTest,
	"UeAgentInterface.Composition.GraphCompound.PcgAnimation.UnboundedHierarchy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPcgAnimationUnboundedHierarchyTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FPcgAnimationGraphRouteProfile* Profile =
		FindPcgAnimationGraphRouteProfile(TEXT("ai.state_tree.hierarchy.apply"));
	TestNotNull(TEXT("StateTree hierarchy route exists"), Profile);
	if (!Profile)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Edits;
	for (int32 Index = 0; Index < 512; ++Index)
	{
		const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
		const FString Identity = FString::Printf(TEXT("state:%d"), Index);
		Edit->SetStringField(TEXT("identity"), Identity);
		Edit->SetStringField(TEXT("element_kind"), TEXT("hierarchy"));
		Edit->SetStringField(TEXT("intent"), TEXT("create"));
		TArray<TSharedPtr<FJsonValue>> Dependencies;
		if (Index > 0)
		{
			Dependencies.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("state:%d"), Index - 1)));
		}
		Edit->SetArrayField(TEXT("depends_on"), MoveTemp(Dependencies));
		const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("native_guid"),
			FString::Printf(TEXT("native-guid-%d"), Index));
		Edit->SetObjectField(TEXT("value"), Value);
		Edits.Add(MakeShared<FJsonValueObject>(Edit));
	}
	const TSharedRef<FJsonObject> InputObject = MakeShared<FJsonObject>();
	InputObject->SetStringField(TEXT("target_ref"), TEXT("/Game/UAI/ST_Deep"));
	InputObject->SetArrayField(TEXT("edits"), MoveTemp(Edits));
	FString Input;
	FString JsonError;
	TestTrue(TEXT("deep input canonicalizes"),
		Json::TryCanonicalize(InputObject, Input, JsonError));

	FUeAgentResourceTracker Resources = MakeTracker();
	const FUeAgentCanonicalSnapshotRef Empty = MakeSnapshot(Resources, {});
	FGraphCompoundCanonicalPayload Payload(
		GetGraphCompoundCanonicalSchemaId(), {});
	FGraphCompoundRouteError Error;
	TestTrue(TEXT("512-level hierarchy decodes without a fixed depth cap"),
		TryDecodePcgAnimationGraphTypedOperation(
			*Profile, FUeAgentContractValue(Input), Empty,
			Resources, Payload, Error));
	TestEqual(TEXT("all hierarchy identities are retained"),
		Payload.GetEdits().Num(), 512);

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(TEXT("registry builds"),
		TryCreatePcgAnimationGraphDescriptorRegistry(Registry, RegistryError));
	const FUeAgentOperationDescriptor* Descriptor = Registry.IsValid()
		? Registry->FindByDescriptorId(Profile->GetDescriptorId()) : nullptr;
	TestNotNull(TEXT("descriptor exists"), Descriptor);
	if (Descriptor)
	{
		TOptional<FChangeSetDraft> Draft;
		TestTrue(TEXT("512-level hierarchy builds one shared C6 draft"),
			TryBuildPcgAnimationGraphTypedOperationDraft(
				*Profile, Descriptor->GetIdentity(),
				FUeAgentContractValue(Input), Empty,
				TEXT("request:gc016:deep"),
				TEXT("idempotency:gc016:deep"),
				Resources, Draft, Error));
		TestTrue(TEXT("deep draft exists"), Draft.IsSet());
		if (Draft.IsSet())
		{
			TestEqual(TEXT("deep draft keeps per-identity changes"),
				Draft->GetChanges().Num(), 512);
		}
	}
	return !HasAnyErrors();
}
