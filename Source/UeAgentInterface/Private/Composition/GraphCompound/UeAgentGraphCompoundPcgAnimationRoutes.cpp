#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundLifecycleResultContract.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	namespace
	{

		FDiagnostic RouteFailure(
			const TCHAR* Code, const FString& Reason, const FString& Subject,
			const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"), FString(), Subject);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		struct FRouteSpec final
		{
			const TCHAR* CapabilityId;
			const TCHAR* DescriptorId;
			const TCHAR* DomainId;
			const TCHAR* FamilyId;
			EPcgAnimationGraphInputKind InputKind;
			const TCHAR* TargetKind;
			TArray<FString> AllowedElementKinds;
			const TCHAR* SettleBarrier;
		};

		const TArray<FRouteSpec>& RouteSpecs()
		{
			using E = EPcgAnimationGraphInputKind;
			static const TArray<FRouteSpec> Values = {
				{TEXT("ai.behavior_tree.asset.create"), TEXT("uai.operation.ai.behavior_tree.asset.create"), TEXT("pcg_ai_node_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("behavior_tree"), {}, TEXT("domain_refresh_settle")},
				{TEXT("ai.behavior_tree.graph.apply"), TEXT("uai.operation.ai.behavior_tree.graph.apply"), TEXT("pcg_ai_node_graph"), TEXT("behavior_tree_graph_mutation"), E::GraphEdits, TEXT("behavior_tree"), {TEXT("item"), TEXT("relation"), TEXT("hierarchy"), TEXT("order"), TEXT("property"), TEXT("binding")}, TEXT("domain_compile_settle")},
				{TEXT("ai.blackboard.asset.create"), TEXT("uai.operation.ai.blackboard.asset.create"), TEXT("pcg_ai_node_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("blackboard"), {}, TEXT("domain_refresh_settle")},
				{TEXT("ai.blackboard.schema.apply"), TEXT("uai.operation.ai.blackboard.schema.apply"), TEXT("pcg_ai_node_graph"), TEXT("blackboard_schema_mutation"), E::BlackboardSchema, TEXT("blackboard"), {}, TEXT("domain_refresh_settle")},
				{TEXT("ai.eqs.asset.create"), TEXT("uai.operation.ai.eqs.asset.create"), TEXT("pcg_ai_node_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("eqs"), {}, TEXT("domain_refresh_settle")},
				{TEXT("ai.eqs.graph.apply"), TEXT("uai.operation.ai.eqs.graph.apply"), TEXT("pcg_ai_node_graph"), TEXT("eqs_graph_mutation"), E::GraphEdits, TEXT("eqs"), {TEXT("item"), TEXT("relation"), TEXT("order"), TEXT("property")}, TEXT("domain_compile_settle")},
				{TEXT("ai.smart_object_definition.asset.create"), TEXT("uai.operation.ai.smart_object_definition.asset.create"), TEXT("pcg_ai_node_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("smart_object_definition"), {}, TEXT("domain_refresh_settle")},
				{TEXT("ai.smart_object_definition.apply"), TEXT("uai.operation.ai.smart_object_definition.apply"), TEXT("pcg_ai_node_graph"), TEXT("smart_object_definition_mutation"), E::SmartObjectDefinition, TEXT("smart_object_definition"), {}, TEXT("domain_validate_settle")},
				{TEXT("ai.state_tree.asset.create"), TEXT("uai.operation.ai.state_tree.asset.create"), TEXT("pcg_ai_node_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("state_tree"), {}, TEXT("domain_refresh_settle")},
				{TEXT("ai.state_tree.hierarchy.apply"), TEXT("uai.operation.ai.state_tree.hierarchy.apply"), TEXT("pcg_ai_node_graph"), TEXT("state_tree_hierarchy_mutation"), E::GraphEdits, TEXT("state_tree"), {TEXT("item"), TEXT("relation"), TEXT("hierarchy"), TEXT("order"), TEXT("property")}, TEXT("domain_compile_settle")},
				{TEXT("pcg.graph.asset.create"), TEXT("uai.operation.pcg.graph.asset.create"), TEXT("pcg_ai_node_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("pcg_graph"), {}, TEXT("domain_refresh_settle")},
				{TEXT("pcg.graph.apply"), TEXT("uai.operation.pcg.graph.apply"), TEXT("pcg_ai_node_graph"), TEXT("pcg_graph_topology_mutation"), E::GraphEdits, TEXT("pcg_graph"), {TEXT("item"), TEXT("pin"), TEXT("relation"), TEXT("property")}, TEXT("domain_compile_settle")},
				{TEXT("pcg.graph_instance.apply"), TEXT("uai.operation.pcg.graph_instance.apply"), TEXT("pcg_ai_node_graph"), TEXT("pcg_parameter_binding_mutation"), E::PcgGraphInstance, TEXT("pcg_graph_instance"), {}, TEXT("domain_refresh_settle")},
				{TEXT("pcg.graph_parameters.apply"), TEXT("uai.operation.pcg.graph_parameters.apply"), TEXT("pcg_ai_node_graph"), TEXT("pcg_parameter_binding_mutation"), E::PcgParameters, TEXT("pcg_graph"), {}, TEXT("domain_refresh_settle")},
				{TEXT("pcg.component.apply"), TEXT("uai.operation.pcg.component.apply"), TEXT("pcg_ai_node_graph"), TEXT("pcg_component_binding_mutation"), E::PcgComponentBinding, TEXT("pcg_component"), {}, TEXT("domain_refresh_settle")},
				{TEXT("pcg.data_asset.apply"), TEXT("uai.operation.pcg.data_asset.apply"), TEXT("pcg_ai_node_graph"), TEXT("pcg_data_asset_mutation"), E::PcgDataAsset, TEXT("pcg_data_asset"), {}, TEXT("domain_validate_settle")},
				{TEXT("pcg.data_asset.create_from_level"), TEXT("uai.operation.pcg.data_asset.create_from_level"), TEXT("pcg_ai_node_graph"), TEXT("pcg_data_asset_mutation"), E::PcgDataFromLevel, TEXT("pcg_data_asset"), {}, TEXT("domain_validate_settle")},
				{TEXT("pcg.component.world_binding.apply"), TEXT("uai.operation.pcg.component.world_binding.apply"), TEXT("pcg_ai_node_graph"), TEXT("pcg_component_binding_mutation"), E::PcgWorldBinding, TEXT("pcg_component"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.anim_blueprint.graph.apply"), TEXT("uai.operation.animation.anim_blueprint.graph.apply"), TEXT("animation_rig_graph"), TEXT("anim_blueprint_graph_mutation"), E::GraphEdits, TEXT("anim_blueprint"), {TEXT("structure"), TEXT("item"), TEXT("pin"), TEXT("relation"), TEXT("binding")}, TEXT("domain_compile_settle")},
				{TEXT("animation.anim_blueprint.asset.create"), TEXT("uai.operation.animation.anim_blueprint.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateAnimBlueprint, TEXT("anim_blueprint"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.anim_blueprint.layer_interface.asset.create"), TEXT("uai.operation.animation.anim_blueprint.layer_interface.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("anim_layer_interface"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.blend_space.definition.apply"), TEXT("uai.operation.animation.blend_space.definition.apply"), TEXT("animation_rig_graph"), TEXT("blend_space_definition_mutation"), E::BlendSpaceDefinition, TEXT("blend_space"), {}, TEXT("domain_validate_settle")},
				{TEXT("animation.blend_space.asset.create"), TEXT("uai.operation.animation.blend_space.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateBlendSpace, TEXT("blend_space"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.skeleton.definition.apply"), TEXT("uai.operation.animation.skeleton.definition.apply"), TEXT("animation_rig_graph"), TEXT("skeleton_definition_mutation"), E::GraphEdits, TEXT("skeleton"), {TEXT("item"), TEXT("relation"), TEXT("hierarchy"), TEXT("property")}, TEXT("domain_refresh_settle")},
				{TEXT("animation.skeleton.compatibility.apply"), TEXT("uai.operation.animation.skeleton.compatibility.apply"), TEXT("animation_rig_graph"), TEXT("skeleton_definition_mutation"), E::SkeletonCompatibility, TEXT("skeleton"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.skeleton.virtual_bone.apply"), TEXT("uai.operation.animation.skeleton.virtual_bone.apply"), TEXT("animation_rig_graph"), TEXT("skeleton_definition_mutation"), E::SkeletonVirtualBone, TEXT("skeleton"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.control_rig.hierarchy_graph.apply"), TEXT("uai.operation.animation.control_rig.hierarchy_graph.apply"), TEXT("animation_rig_graph"), TEXT("control_rig_hierarchy_graph_mutation"), E::GraphEdits, TEXT("control_rig"), {TEXT("item"), TEXT("pin"), TEXT("relation"), TEXT("hierarchy"), TEXT("order"), TEXT("property")}, TEXT("domain_compile_settle")},
				{TEXT("animation.control_rig.asset.create"), TEXT("uai.operation.animation.control_rig.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateSkeletonBound, TEXT("control_rig"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.control_rig.shape_library.apply"), TEXT("uai.operation.animation.control_rig.shape_library.apply"), TEXT("animation_rig_graph"), TEXT("control_rig_shape_library_mutation"), E::ControlRigShapeLibrary, TEXT("control_rig_shape_library"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.control_rig.shape_library.asset.create"), TEXT("uai.operation.animation.control_rig.shape_library.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("control_rig_shape_library"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.deformer_graph.topology.apply"), TEXT("uai.operation.animation.deformer_graph.topology.apply"), TEXT("animation_rig_graph"), TEXT("deformer_graph_topology_mutation"), E::GraphEdits, TEXT("deformer_graph"), {TEXT("item"), TEXT("pin"), TEXT("relation"), TEXT("order"), TEXT("property")}, TEXT("domain_compile_settle")},
				{TEXT("animation.deformer_graph.asset.create"), TEXT("uai.operation.animation.deformer_graph.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateDeformerGraph, TEXT("deformer_graph"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.deformer_source_library.bindings.apply"), TEXT("uai.operation.animation.deformer_source_library.bindings.apply"), TEXT("animation_rig_graph"), TEXT("deformer_source_binding_mutation"), E::DeformerSourceBindings, TEXT("deformer_source_library"), {}, TEXT("domain_validate_settle")},
				{TEXT("animation.deformer_source_library.asset.create"), TEXT("uai.operation.animation.deformer_source_library.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateConcreteClass, TEXT("deformer_source_library"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.mesh_deformer_collection.entries.apply"), TEXT("uai.operation.animation.mesh_deformer_collection.entries.apply"), TEXT("animation_rig_graph"), TEXT("mesh_deformer_collection_mutation"), E::MeshDeformerCollectionEntries, TEXT("mesh_deformer_collection"), {}, TEXT("domain_validate_settle")},
				{TEXT("animation.mesh_deformer_collection.asset.create"), TEXT("uai.operation.animation.mesh_deformer_collection.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateSimple, TEXT("mesh_deformer_collection"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.ml_deformer.configuration.apply"), TEXT("uai.operation.animation.ml_deformer.configuration.apply"), TEXT("animation_rig_graph"), TEXT("ml_deformer_configuration_mutation"), E::MlDeformerConfiguration, TEXT("ml_deformer"), {}, TEXT("domain_validate_settle")},
				{TEXT("animation.ml_deformer.asset.create"), TEXT("uai.operation.animation.ml_deformer.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateMlModel, TEXT("ml_deformer"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.ik_retargeter.definition.apply"), TEXT("uai.operation.animation.ik_retargeter.definition.apply"), TEXT("animation_rig_graph"), TEXT("ik_retargeter_definition_mutation"), E::IkRetargeterDefinition, TEXT("ik_retargeter"), {}, TEXT("domain_validate_settle")},
				{TEXT("animation.ik_retargeter.asset.create"), TEXT("uai.operation.animation.ik_retargeter.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateRetargeter, TEXT("ik_retargeter"), {}, TEXT("domain_refresh_settle")},
				{TEXT("animation.ik_retargeter.settings.apply"), TEXT("uai.operation.animation.ik_retargeter.settings.apply"), TEXT("animation_rig_graph"), TEXT("ik_retargeter_definition_mutation"), E::IkRetargeterSettings, TEXT("ik_retargeter"), {}, TEXT("domain_validate_settle")},
				{TEXT("animation.ik_rig.definition.auto_generate"), TEXT("uai.operation.animation.ik_rig.definition.auto_generate"), TEXT("animation_rig_graph"), TEXT("ik_rig_definition_mutation"), E::IkRigAutoGenerate, TEXT("ik_rig"), {}, TEXT("domain_validate_settle")},
				{TEXT("animation.ik_rig.definition.apply"), TEXT("uai.operation.animation.ik_rig.definition.apply"), TEXT("animation_rig_graph"), TEXT("ik_rig_definition_mutation"), E::IkRigDefinition, TEXT("ik_rig"), {}, TEXT("domain_validate_settle")},
				{TEXT("animation.ik_rig.asset.create"), TEXT("uai.operation.animation.ik_rig.asset.create"), TEXT("animation_rig_graph"), TEXT("asset_lifecycle"), E::AssetCreateIkRig, TEXT("ik_rig"), {}, TEXT("domain_refresh_settle")},
			};
			return Values;
		}

		FString CanonicalSchema(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Text)
		{
			TSharedPtr<FJsonObject> Object;
			FString Canonical;
			if (!Json::TryParseObject(Text, Object, Error)
				|| !Json::TryCanonicalize(Object, Canonical, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				return FString();
			}
			return Canonical;
		}

		TSharedRef<FJsonObject> NonEmptyStringSchema()
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("type"), TEXT("string"));
			Result->SetNumberField(TEXT("minLength"), 1);
			return Result;
		}

		FString GraphEditsSchema(const TArray<FString>& AllowedKinds, UE::UAI::Contract::FDiagnostic& Error)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), TEXT("object"));
			Root->SetBoolField(TEXT("additionalProperties"), false);
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("target_ref"), NonEmptyStringSchema());
			const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
			Edit->SetStringField(TEXT("type"), TEXT("object"));
			Edit->SetBoolField(TEXT("additionalProperties"), false);
			const TSharedRef<FJsonObject> EditProperties = MakeShared<FJsonObject>();
			EditProperties->SetObjectField(TEXT("identity"), NonEmptyStringSchema());
			const TSharedRef<FJsonObject> ElementKind = NonEmptyStringSchema();
			TArray<TSharedPtr<FJsonValue>> ElementValues;
			for (const FString& Kind : AllowedKinds)
			{
				ElementValues.Add(MakeShared<FJsonValueString>(Kind));
			}
			ElementKind->SetArrayField(TEXT("enum"), MoveTemp(ElementValues));
			EditProperties->SetObjectField(TEXT("element_kind"), ElementKind);
			const TSharedRef<FJsonObject> Intent = NonEmptyStringSchema();
			Intent->SetArrayField(TEXT("enum"), {
				MakeShared<FJsonValueString>(TEXT("create")),
				MakeShared<FJsonValueString>(TEXT("update")),
				MakeShared<FJsonValueString>(TEXT("replace")),
				MakeShared<FJsonValueString>(TEXT("delete"))});
			EditProperties->SetObjectField(TEXT("intent"), Intent);
			EditProperties->SetObjectField(TEXT("value"), MakeShared<FJsonObject>());
			const TSharedRef<FJsonObject> Dependencies = MakeShared<FJsonObject>();
			Dependencies->SetStringField(TEXT("type"), TEXT("array"));
			Dependencies->SetObjectField(TEXT("items"), NonEmptyStringSchema());
			Dependencies->SetBoolField(TEXT("uniqueItems"), true);
			EditProperties->SetObjectField(TEXT("depends_on"), Dependencies);
			Edit->SetObjectField(TEXT("properties"), EditProperties);
			Edit->SetArrayField(TEXT("required"), {
				MakeShared<FJsonValueString>(TEXT("identity")),
				MakeShared<FJsonValueString>(TEXT("element_kind")),
				MakeShared<FJsonValueString>(TEXT("intent"))});
			const TSharedRef<FJsonObject> Edits = MakeShared<FJsonObject>();
			Edits->SetStringField(TEXT("type"), TEXT("array"));
			Edits->SetNumberField(TEXT("minItems"), 1);
			Edits->SetObjectField(TEXT("items"), Edit);
			Properties->SetObjectField(TEXT("edits"), Edits);
			Root->SetObjectField(TEXT("properties"), Properties);
			Root->SetArrayField(TEXT("required"), {
				MakeShared<FJsonValueString>(TEXT("target_ref")),
				MakeShared<FJsonValueString>(TEXT("edits"))});
			FString Canonical;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				return FString();
			}
			return Canonical;
		}

		FString InputSchema(const FRouteSpec& Spec, UE::UAI::Contract::FDiagnostic& Error)
		{
			using E = EPcgAnimationGraphInputKind;
			switch (Spec.InputKind)
			{
			case E::AssetCreateSimple:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\"],\"type\":\"object\"}"));
			case E::AssetCreateAnimBlueprint:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"parent_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"skeleton_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"parent_class_ref\",\"skeleton_ref\"],\"type\":\"object\"}"));
			case E::AssetCreateBlendSpace:
			{
				FRouteSpec DefinitionSpec = Spec;
				DefinitionSpec.InputKind = E::BlendSpaceDefinition;
				TSharedPtr<FJsonObject> Root, Definition;
				FString Canonical;
				const FString RootSchema = CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"dimension\":{\"maximum\":2,\"minimum\":1,\"type\":\"integer\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"skeleton_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"dimension\",\"package_path\",\"skeleton_ref\"],\"type\":\"object\"}"));
				if (RootSchema.IsEmpty() || !Json::TryParseObject(RootSchema, Root, Error))
				{
					MarkGraphCompoundNativeDiagnostic(Error);
					return FString();
				}
				const FString DefinitionSchema = InputSchema(DefinitionSpec, Error);
				if (DefinitionSchema.IsEmpty() || !Json::TryParseObject(DefinitionSchema, Definition, Error))
				{
					MarkGraphCompoundNativeDiagnostic(Error);
					return FString();
				}
				const auto Properties = Root->GetObjectField(TEXT("properties"));
				const auto Fields = Definition->GetObjectField(TEXT("properties"));
				Properties->SetField(TEXT("axes"), Fields->TryGetField(TEXT("axes")));
				Properties->SetField(TEXT("samples"), Fields->TryGetField(TEXT("samples")));
				if (!Json::TryCanonicalize(Root, Canonical, Error))
				{
					MarkGraphCompoundNativeDiagnostic(Error);
					return FString();
				}
				return Canonical;
			}
			case E::AssetCreateSkeletonBound:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"skeleton_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"skeleton_ref\"],\"type\":\"object\"}"));
			case E::AssetCreateDeformerGraph:
			case E::AssetCreateIkRig:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"skeletal_mesh_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"skeletal_mesh_ref\"],\"type\":\"object\"}"));
			case E::AssetCreateConcreteClass:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"concrete_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"concrete_class_ref\",\"package_path\"],\"type\":\"object\"}"));
			case E::AssetCreateMlModel:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"model_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"model_class_ref\",\"package_path\"],\"type\":\"object\"}"));
			case E::AssetCreateRetargeter:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"source_ik_rig_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ik_rig_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"source_ik_rig_ref\",\"target_ik_rig_ref\"],\"type\":\"object\"}"));
			case E::GraphEdits:
				return GraphEditsSchema(Spec.AllowedElementKinds, Error);
			case E::BlackboardSchema:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"entries\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"entry_name\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"key_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"ownership\":{\"enum\":[\"local\",\"inherited\"],\"type\":\"string\"}},\"required\":[\"entry_name\",\"intent\",\"key_type_ref\",\"ownership\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"entries\",\"target_ref\"],\"type\":\"object\"}"));
			case E::SmartObjectDefinition:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"slots\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"behavior_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"slot_guid\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"behavior_type_ref\",\"data_type_ref\",\"intent\",\"slot_guid\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"slots\",\"target_ref\"],\"type\":\"object\"}"));
			case E::PcgGraphInstance:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"graph_ref\":{\"minLength\":1,\"type\":\"string\"},\"overrides\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"property_guid\":{\"minLength\":1,\"type\":\"string\"},\"property_name\":{\"minLength\":1,\"type\":\"string\"},\"property_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"property_guid\",\"property_name\",\"property_type_ref\",\"value\"],\"type\":\"object\"},\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"graph_ref\",\"overrides\",\"target_ref\"],\"type\":\"object\"}"));
			case E::PcgParameters:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"parameters\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"property_guid\":{\"minLength\":1,\"type\":\"string\"},\"property_name\":{\"minLength\":1,\"type\":\"string\"},\"property_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"property_guid\",\"property_name\",\"property_type_ref\",\"value\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"parameters\",\"target_ref\"],\"type\":\"object\"}"));
			case E::PcgComponentBinding:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"generation_policy\":{\"enum\":[\"on_demand\",\"on_load\",\"runtime\"],\"type\":\"string\"},\"graph_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"generation_policy\",\"graph_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case E::PcgDataAsset:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"source_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"data_type_ref\",\"source_ref\",\"target_ref\",\"value\"],\"type\":\"object\"}"));
			case E::PcgDataFromLevel:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"source_level_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"data_type_ref\",\"source_level_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case E::PcgWorldBinding:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"actor_ref\":{\"minLength\":1,\"type\":\"string\"},\"component_ref\":{\"minLength\":1,\"type\":\"string\"},\"generation_policy\":{\"enum\":[\"on_demand\",\"on_load\",\"runtime\"],\"type\":\"string\"},\"graph_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"actor_ref\",\"component_ref\",\"generation_policy\",\"graph_ref\"],\"type\":\"object\"}"));
			case E::BlendSpaceDefinition:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"axes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"axis_index\":{\"minimum\":0,\"type\":\"integer\"},\"axis_name\":{\"minLength\":1,\"type\":\"string\"},\"grid_divisions\":{\"minimum\":1,\"type\":\"integer\"},\"maximum\":{\"type\":\"number\"},\"minimum\":{\"type\":\"number\"}},\"required\":[\"axis_index\",\"axis_name\",\"grid_divisions\",\"maximum\",\"minimum\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"samples\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"animation_ref\":{\"minLength\":1,\"type\":\"string\"},\"coordinates\":{\"items\":{\"type\":\"number\"},\"maxItems\":3,\"minItems\":1,\"type\":\"array\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"}},\"required\":[\"animation_ref\",\"coordinates\",\"intent\"],\"type\":\"object\"},\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"axes\",\"samples\",\"target_ref\"],\"type\":\"object\"}"));
			case E::SkeletonCompatibility:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"changes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"create\",\"delete\"],\"type\":\"string\"},\"skeleton_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"intent\",\"skeleton_ref\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"changes\",\"target_ref\"],\"type\":\"object\"}"));
			case E::SkeletonVirtualBone:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"name\":{\"minLength\":1,\"type\":\"string\"},\"source_bone\":{\"minLength\":1,\"type\":\"string\"},\"target_bone\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"intent\",\"name\",\"source_bone\",\"target_bone\",\"target_ref\"],\"type\":\"object\"}"));
			case E::ControlRigShapeLibrary:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"shapes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"material_ref\":{\"minLength\":1,\"type\":\"string\"},\"mesh_ref\":{\"minLength\":1,\"type\":\"string\"},\"name\":{\"minLength\":1,\"type\":\"string\"},\"order_index\":{\"minimum\":0,\"type\":\"integer\"}},\"required\":[\"intent\",\"material_ref\",\"mesh_ref\",\"name\",\"order_index\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"shapes\",\"target_ref\"],\"type\":\"object\"}"));
			case E::DeformerSourceBindings:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"bindings\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"asset_ref\":{\"minLength\":1,\"type\":\"string\"},\"binding_key\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"property_type_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_ref\",\"binding_key\",\"intent\",\"property_type_ref\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"concrete_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"bindings\",\"concrete_class_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case E::MeshDeformerCollectionEntries:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"entries\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"entry_kind\":{\"enum\":[\"deformer\",\"child_collection\"],\"type\":\"string\"},\"entry_ref\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"order_index\":{\"minimum\":0,\"type\":\"integer\"}},\"required\":[\"entry_kind\",\"entry_ref\",\"intent\",\"order_index\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"entries\",\"target_ref\"],\"type\":\"object\"}"));
			case E::MlDeformerConfiguration:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"animation_ref\":{\"minLength\":1,\"type\":\"string\"},\"concrete_model_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"deformer_graph_ref\":{\"type\":\"string\"},\"skeletal_mesh_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_geometry_cache_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"animation_ref\",\"concrete_model_class_ref\",\"deformer_graph_ref\",\"skeletal_mesh_ref\",\"target_geometry_cache_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case E::IkRigDefinition:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"chains\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"end_bone\":{\"minLength\":1,\"type\":\"string\"},\"goal_name\":{\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"name\":{\"minLength\":1,\"type\":\"string\"},\"start_bone\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"end_bone\",\"goal_name\",\"intent\",\"name\",\"start_bone\"],\"type\":\"object\"},\"type\":\"array\"},\"goals\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"bone_name\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"bone_name\",\"intent\",\"name\"],\"type\":\"object\"},\"type\":\"array\"},\"retarget_root\":{\"minLength\":1,\"type\":\"string\"},\"solvers\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"order_index\":{\"minimum\":0,\"type\":\"integer\"},\"solver_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"solver_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"intent\",\"order_index\",\"solver_class_ref\",\"solver_ref\"],\"type\":\"object\"},\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"chains\",\"goals\",\"retarget_root\",\"solvers\",\"target_ref\"],\"type\":\"object\"}"));
			case E::IkRigAutoGenerate:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"preset\":{\"enum\":[\"humanoid\",\"quadruped\",\"custom\"],\"type\":\"string\"},\"skeletal_mesh_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"preset\",\"skeletal_mesh_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case E::IkRetargeterDefinition:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"chain_mappings\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"operation_ref\":{\"minLength\":1,\"type\":\"string\"},\"order_index\":{\"minimum\":0,\"type\":\"integer\"},\"source_chain\":{\"minLength\":1,\"type\":\"string\"},\"target_chain\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"intent\",\"operation_ref\",\"order_index\",\"source_chain\",\"target_chain\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"source_ik_rig_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ik_rig_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"chain_mappings\",\"source_ik_rig_ref\",\"target_ik_rig_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case E::IkRetargeterSettings:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"settings\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"chain_name\":{\"type\":\"string\"},\"scope\":{\"enum\":[\"global\",\"root\",\"chain\"],\"type\":\"string\"},\"setting_name\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"chain_name\",\"scope\",\"setting_name\",\"value\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"settings\",\"target_ref\"],\"type\":\"object\"}"));
			default:
				Error = RouteFailure(TEXT("graph_compound_input_schema_kind_invalid"),
					TEXT("The registered graph route has no input schema for its input kind."),
					Spec.CapabilityId, TEXT("input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.InputKind)),
					MakeShared<FJsonValueString>(TEXT("a supported input schema kind")));
				return FString();
			}
		}

		FString ResultSchema(const FRouteSpec& Spec, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (FCString::Strcmp(
					Spec.DomainId, TEXT("animation_rig_graph")) == 0)
			{
				return LifecycleResultContract::
					BuildMutationLifecycleResultSchema(false, Error);
			}
			return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"target_ref\":{\"type\":\"string\"}},\"required\":[\"target_ref\"],\"type\":\"object\"}"));
		}

		TSharedPtr<FJsonObject> DescriptorObject(
			const FRouteSpec& Spec,
			const FString& Input,
			const FString& Result,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> InputObject;
			TSharedPtr<FJsonObject> ResultObject;
			if (!Json::TryParseObject(Input, InputObject, OutError)
				|| !Json::TryParseObject(Result, ResultObject, OutError))
			{
				return nullptr;
			}
			const TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
			Policy->SetArrayField(TEXT("allowed_state_planes"), {
				MakeShared<FJsonValueString>(TEXT("persisted"))});
			Policy->SetArrayField(TEXT("required_facets"), {});
			Policy->SetStringField(TEXT("resolution_policy"), TEXT("explicit_only"));
			Policy->SetStringField(
				TEXT("transient_access_policy"),
				TEXT("temporary_mutation_allowed"));
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("capability_id"), Spec.CapabilityId);
			Object->SetStringField(TEXT("descriptor_id"), Spec.DescriptorId);
			Object->SetStringField(TEXT("domain_id"), Spec.DomainId);
			Object->SetObjectField(TEXT("execution_context_policy"), Policy);
			Object->SetObjectField(TEXT("input_schema"), InputObject);
			Object->SetStringField(TEXT("operation_kind"), TEXT("operation"));
			Object->SetObjectField(TEXT("result_schema"), ResultObject);
			Object->SetField(TEXT("version"),
				MakeShared<FJsonValueNumberString>(TEXT("1")));
			return Object;
		}

		FString StableSuffix(FString Value)
		{
			Value.ReplaceInline(TEXT("."), TEXT("_"));
			return Value;
		}

		struct FRouteBundle final
		{
			TArray<FPcgAnimationGraphRouteProfile> Profiles;
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			UE::UAI::Contract::FDiagnostic Error;
		};

		FRouteBundle BuildBundle()
		{
			FRouteBundle Bundle;
			TArray<FUeAgentOperationDescriptor> Descriptors;
			for (const FRouteSpec& Spec : RouteSpecs())
			{
				auto RejectSchema = [&](const TCHAR* Field)
				{
					MarkGraphCompoundNativeDiagnostic(Bundle.Error);
					if (Bundle.Error.TargetPath.IsEmpty()) Bundle.Error.TargetPath = Spec.DescriptorId;
					if (Bundle.Error.PropertyPath.IsEmpty()) Bundle.Error.PropertyPath = Field;
					Bundle.Profiles.Reset();
				};
				const FString Input = InputSchema(Spec, Bundle.Error);
				if (Input.IsEmpty())
				{
					RejectSchema(TEXT("input_schema"));
					return Bundle;
				}
				const FString Result = ResultSchema(Spec, Bundle.Error);
				if (Result.IsEmpty())
				{
					RejectSchema(TEXT("result_schema"));
					return Bundle;
				}
				TSharedPtr<FJsonObject> Object = DescriptorObject(Spec, Input, Result, Bundle.Error);
				FString CanonicalDescriptor;
				if (!Object.IsValid() || !Json::TryCanonicalize(Object, CanonicalDescriptor, Bundle.Error))
				{
					RejectSchema(TEXT("descriptor"));
					return Bundle;
				}
				const FUeAgentDescriptorIdentity Identity(
					Spec.DescriptorId, 1, Json::Sha256Utf8(CanonicalDescriptor));
				const FUeAgentExecutionContextPolicy Policy(
					{EUeAgentStatePlane::Persisted}, {},
					EUeAgentContextResolutionPolicy::ExplicitOnly,
					EUeAgentTransientContextPolicy::TemporaryMutationAllowed);
				Descriptors.Emplace(
					Identity, Spec.CapabilityId, Spec.DomainId,
					EUeAgentDescriptorOperationKind::Operation,
					Policy, FUeAgentContractValue(Input),
					FUeAgentContractValue(Result),
					TOptional<FUeAgentQueryBinding>());

				const FString Suffix = StableSuffix(Spec.CapabilityId);
				Bundle.Profiles.Emplace(
					Spec.CapabilityId, Spec.DescriptorId, Spec.DomainId,
					Spec.FamilyId, Spec.InputKind, Spec.TargetKind,
					FGraphCompoundExecutionContract(
						Identity,
						EGraphCompoundExecutionLane::Operation,
						Spec.FamilyId,
						FString(TEXT("graph_compound.")) + Spec.CapabilityId,
						FString(TEXT("graph_compound.")) + Suffix + TEXT(".request.v1"),
						FString(TEXT("graph_compound.")) + Suffix + TEXT(".result.v1"),
						TEXT("graph_compound_identity_change"),
						FString(TEXT("native.")) + Spec.CapabilityId,
						FString(TEXT("readback.")) + Spec.CapabilityId,
						Spec.SettleBarrier,
						TEXT("independent_post_mutation_readback"),
						true,
						true));
			}
			if (!FUeAgentDescriptorRegistry::TryCreate(
					MoveTemp(Descriptors), Bundle.Registry, Bundle.Error))
			{
				MarkGraphCompoundNativeDiagnostic(Bundle.Error);
				Bundle.Profiles.Reset();
			}
			return Bundle;
		}

		const FRouteBundle& RouteBundle()
		{
			static const FRouteBundle Bundle = BuildBundle();
			return Bundle;
		}

		const FRouteSpec* FindSpec(const FPcgAnimationGraphRouteProfile& Profile)
		{
			for (const FRouteSpec& Spec : RouteSpecs())
			{
				if (Profile.GetCapabilityId() == Spec.CapabilityId
					&& Profile.GetDescriptorId() == Spec.DescriptorId
					&& Profile.GetInputKind() == Spec.InputKind)
				{
					return &Spec;
				}
			}
			return nullptr;
		}

		void SetError(FGraphCompoundRouteError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
        }

		bool TryValidatedObject(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FUeAgentResourceTracker& Resources,
			TSharedPtr<FJsonObject>& OutObject,
			FGraphCompoundRouteError& OutError)
		{
			OutObject.Reset();
			OutError = {};
			const FRouteBundle& Bundle = RouteBundle();
			if (!Profile.IsValid())
			{
				OutError = Profile.GetValidationDiagnostic();
				return false;
			}
			if (!Bundle.Registry.IsValid())
			{
				OutError = Bundle.Error.IsValid() ? Bundle.Error : RouteFailure(
					TEXT("graph_compound_registry_missing"),
					TEXT("The graph route registry is unavailable."),
					Profile.GetCapabilityId(), TEXT("registry.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (!FindSpec(Profile))
			{
				const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("capability_id"), Profile.GetCapabilityId());
				Actual->SetStringField(TEXT("descriptor_id"), Profile.GetDescriptorId());
				Actual->SetNumberField(TEXT("input_kind"), static_cast<int32>(Profile.GetInputKind()));
				OutError = RouteFailure(TEXT("graph_compound_route_spec_missing"),
					TEXT("No registered graph route specification matches the profile."),
					Profile.GetCapabilityId(), TEXT("route_spec"), MakeShared<FJsonValueObject>(Actual),
					MakeShared<FJsonValueString>(TEXT("registered route specification")));
				return false;
			}
			const FUeAgentOperationDescriptor* Descriptor =
				Bundle.Registry->FindByDescriptorId(Profile.GetDescriptorId());
			if (!Descriptor)
			{
				OutError = RouteFailure(TEXT("graph_compound_descriptor_missing"), TEXT("The graph route descriptor is missing from its registry."),
					Profile.GetDescriptorId(), TEXT("descriptor.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			const FUeAgentContractSchema* Schema =
				Bundle.Registry->FindCompiledInputSchemaExact(Descriptor->GetIdentity());
			if (!Schema)
			{
				OutError = RouteFailure(TEXT("graph_compound_schema_missing"), TEXT("The graph route descriptor has no compiled input schema."),
					Profile.GetDescriptorId(), TEXT("input_schema.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			const FUeAgentContractSchemaCheckpoint Checkpoint =
				[&Resources](
					const int64 WorkUnits,
					const FString& InstancePath,
					FUeAgentContractSchemaError& Error)
				{
					FUeAgentResourceChargeError ChargeError;
					if (!Resources.TryCharge(
							EUeAgentResourceDimension::WorkUnits,
							WorkUnits,
							TEXT("graph_compound.gc016.schema"),
							ChargeError))
					{
						Error = ChargeError;
				Error.Source.JsonPath = InstancePath;
						return false;
					}
					return true;
				};
			TOptional<FUeAgentContractValue> Canonical;
			FUeAgentContractSchemaError SchemaError;
			const bool bValidated = Schema->TryValidateAndCanonicalize(Input, Checkpoint, Canonical, SchemaError);
			if (!bValidated || !Canonical.IsSet())
			{
				OutError = SchemaError.IsValid() ? SchemaError : RouteFailure(
					TEXT("graph_compound_input_invalid"),
					bValidated ? TEXT("Graph input validation returned without a canonical value.")
						: TEXT("Graph input validation failed without a diagnostic."),
					Profile.GetCapabilityId(), bValidated ? TEXT("canonical_input.present") : TEXT("input_validation.succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			UE::UAI::Contract::FDiagnostic ParseError;
			if (!Json::TryParseObject(
					Canonical->GetJsonValue(), OutObject, ParseError))
			{
				SetError(OutError, ParseError, Profile.GetCapabilityId());
				return false;
			}
			return true;
		}

		bool TryIntent(const FString& Text, EChangeIntent& OutIntent)
		{
			if (Text == TEXT("create")) { OutIntent = EChangeIntent::Create; return true; }
			if (Text == TEXT("update")) { OutIntent = EChangeIntent::Update; return true; }
			if (Text == TEXT("replace")) { OutIntent = EChangeIntent::Replace; return true; }
			if (Text == TEXT("delete")) { OutIntent = EChangeIntent::Delete; return true; }
			return false;
		}

		bool TryElementKind(
			const FString& Text,
			EGraphCompoundElementKind& OutKind)
		{
			static const TMap<FString, EGraphCompoundElementKind> Values = {
				{TEXT("target"), EGraphCompoundElementKind::Target},
				{TEXT("structure"), EGraphCompoundElementKind::Structure},
				{TEXT("item"), EGraphCompoundElementKind::Item},
				{TEXT("pin"), EGraphCompoundElementKind::Pin},
				{TEXT("relation"), EGraphCompoundElementKind::Relation},
				{TEXT("property"), EGraphCompoundElementKind::Property},
				{TEXT("input"), EGraphCompoundElementKind::Input},
				{TEXT("hierarchy"), EGraphCompoundElementKind::Hierarchy},
				{TEXT("order"), EGraphCompoundElementKind::Order},
				{TEXT("binding"), EGraphCompoundElementKind::Binding},
				{TEXT("dependency"), EGraphCompoundElementKind::Dependency},
			};
			const EGraphCompoundElementKind* Found = Values.Find(Text);
			if (!Found) { return false; }
			OutKind = *Found;
			return true;
		}

		FUeAgentCanonicalIdentity Identity(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Generation)
		{
			return FUeAgentCanonicalIdentity(
				SemanticId, TargetKind, OwnerId, SemanticId,
				TEXT("graph_compound"), Generation);
		}

		bool TryCanonical(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRouteError& OutError,
			const FString& Subject)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!JsonValue.IsValid()
				|| !CanonicalJson::TryFromJsonValue(
					JsonValue, Resources, OutValue, Error))
			{
				SetError(OutError, Error, Subject);
				return false;
			}
			return true;
		}

		bool IsAssetCreate(const EPcgAnimationGraphInputKind Kind)
		{
			using E = EPcgAnimationGraphInputKind;
			return Kind >= E::AssetCreateSimple && Kind <= E::AssetCreateIkRig;
		}

		FString AssetTargetRef(const FJsonObject& Object)
		{
			FString PackagePath = Object.GetStringField(TEXT("package_path"));
			while (PackagePath.EndsWith(TEXT("/")))
			{
				PackagePath.LeftChopInline(1);
			}
			return PackagePath + TEXT("/")
				+ Object.GetStringField(TEXT("asset_name"));
		}

		TSharedPtr<FJsonValue> ObjectWithout(
			const FJsonObject& Object,
			const TSet<FString>& Excluded)
		{
			const TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
			{
				if (!Excluded.Contains(Pair.Key))
				{
					Copy->SetField(Pair.Key, Pair.Value);
				}
			}
			return MakeShared<FJsonValueObject>(Copy);
		}

		bool TrySnapshotStringField(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const FString& SemanticId,
			const FString& Field,
			FString& OutValue, FGraphCompoundRouteError& OutError)
		{
			OutValue.Reset();
			OutError = {};
			FDiagnostic Missing = RouteFailure(TEXT("graph_compound_pcg_component_state_invalid"),
				TEXT("The PCG component snapshot has no matching binding entry."),
				SemanticId, TEXT("snapshot.entry_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			for (const FUeAgentStateEntry& Entry : Snapshot.GetEntries())
			{
				if (Entry.GetIdentity().GetSemanticId() != SemanticId)
					continue;
				if (Entry.GetValue()->GetKind() != EUeAgentValueKind::Object)
				{
					Missing = RouteFailure(TEXT("graph_compound_pcg_component_state_invalid"),
						TEXT("The PCG component binding snapshot entry must be an object."), SemanticId, TEXT("snapshot.value_kind"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Entry.GetValue()->GetKind())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentValueKind::Object)));
					continue;
				}
				Missing = RouteFailure(TEXT("graph_compound_pcg_component_state_invalid"),
					TEXT("The PCG component binding snapshot is missing the required string field."),
					SemanticId, Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
					: Entry.GetValue()->GetObjectItems())
				{
					if (Item.Key != Field)
						continue;
					if (Item.Value->GetKind() == EUeAgentValueKind::String)
					{
						OutValue = Item.Value->GetText();
						if (!OutValue.IsEmpty()) return true;
						OutError = RouteFailure(TEXT("graph_compound_pcg_component_state_invalid"),
							TEXT("The PCG component binding snapshot string field is empty."), SemanticId, Field,
							MakeShared<FJsonValueString>(OutValue), MakeShared<FJsonValueString>(TEXT("non-empty string")));
						return false;
					}
					Missing = RouteFailure(TEXT("graph_compound_pcg_component_state_invalid"),
						TEXT("The PCG component binding snapshot field must be a string."), SemanticId, Field,
						MakeShared<FJsonValueNumber>(static_cast<int32>(Item.Value->GetKind())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentValueKind::String)));
				}
				OutError = MoveTemp(Missing);
				return false;
			}
			OutError = MoveTemp(Missing);
			return false;
		}

		bool SnapshotContainsSemanticId(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const FString& SemanticId)
		{
			return Snapshot.GetEntries().ContainsByPredicate(
				[&SemanticId](const FUeAgentStateEntry& Entry)
				{
					return Entry.GetIdentity().GetSemanticId() == SemanticId;
				});
		}
	}

	FPcgAnimationGraphRouteProfile::FPcgAnimationGraphRouteProfile(
		FString InCapabilityId,
		FString InDescriptorId,
		FString InDomainId,
		FString InFamilyId,
		const EPcgAnimationGraphInputKind InInputKind,
		FString InTargetKind,
		FGraphCompoundExecutionContract InExecutionContract)
		: CapabilityId(MoveTemp(InCapabilityId))
		, DescriptorId(MoveTemp(InDescriptorId))
		, DomainId(MoveTemp(InDomainId))
		, FamilyId(MoveTemp(InFamilyId))
		, InputKind(InInputKind)
		, TargetKind(MoveTemp(InTargetKind))
		, ExecutionContract(MoveTemp(InExecutionContract))
	{
		for (FString* Value : {
			&CapabilityId, &DescriptorId, &DomainId, &FamilyId, &TargetKind})
		{
			Value->TrimStartAndEndInline();
		}
		if (CapabilityId.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route capability_id is empty after trimming."),
				CapabilityId, TEXT("capability_id"), MakeShared<FJsonValueString>(CapabilityId),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (DescriptorId.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route descriptor_id is empty after trimming."),
				CapabilityId, TEXT("descriptor_id"), MakeShared<FJsonValueString>(DescriptorId),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (DomainId.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route domain_id is empty after trimming."),
				CapabilityId, TEXT("domain_id"), MakeShared<FJsonValueString>(DomainId),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (FamilyId.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route family_id is empty after trimming."),
				CapabilityId, TEXT("family_id"), MakeShared<FJsonValueString>(FamilyId),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (TargetKind.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route target_kind is empty after trimming."),
				CapabilityId, TEXT("target_kind"), MakeShared<FJsonValueString>(TargetKind),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (InputKind == EPcgAnimationGraphInputKind::Invalid)
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route has an invalid input kind."),
				CapabilityId, TEXT("input_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(InputKind)),
				MakeShared<FJsonValueString>(TEXT("non-Invalid input kind")));
		}
		else if (!ExecutionContract.IsValid())
		{
			ValidationDiagnostic = FDiagnostic::WrapFailure(
				TEXT("graph_compound_binding_profile_invalid"),
				ExecutionContract.GetValidationDiagnostic(), TEXT("graph_compound"),
				TEXT("The graph route execution contract is invalid."));
		}
	}

	const FString& FPcgAnimationGraphRouteProfile::GetCapabilityId() const { return CapabilityId; }
	const FString& FPcgAnimationGraphRouteProfile::GetDescriptorId() const { return DescriptorId; }
	const FString& FPcgAnimationGraphRouteProfile::GetDomainId() const { return DomainId; }
	const FString& FPcgAnimationGraphRouteProfile::GetFamilyId() const { return FamilyId; }
	EPcgAnimationGraphInputKind FPcgAnimationGraphRouteProfile::GetInputKind() const { return InputKind; }
	const FString& FPcgAnimationGraphRouteProfile::GetTargetKind() const { return TargetKind; }
	const FGraphCompoundExecutionContract& FPcgAnimationGraphRouteProfile::GetExecutionContract() const { return ExecutionContract; }
	bool FPcgAnimationGraphRouteProfile::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FDiagnostic& FPcgAnimationGraphRouteProfile::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	bool TryGetPcgAnimationGraphRouteProfiles(
		const TArray<FPcgAnimationGraphRouteProfile>*& OutProfiles, FDiagnostic& OutError)
	{
		OutProfiles = nullptr;
		OutError = RouteBundle().Error;
		if (!OutError.IsEmpty()) return false;
		OutProfiles = &RouteBundle().Profiles;
		return true;
	}

	const FPcgAnimationGraphRouteProfile*
	FindPcgAnimationGraphRouteProfile(const FString& CapabilityId)
	{
		for (const FPcgAnimationGraphRouteProfile& Profile
			: RouteBundle().Profiles)
		{
			if (Profile.GetCapabilityId() == CapabilityId)
			{
				return &Profile;
			}
		}
		return nullptr;
	}

	bool TryCreatePcgAnimationGraphDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry = RouteBundle().Registry;
		OutError = RouteBundle().Error;
		return OutRegistry.IsValid() && OutError.IsEmpty();
	}

	bool TryDecodePcgAnimationGraphTypedOperation(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FUeAgentContractValue& Input,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		FUeAgentResourceTracker& Resources,
		FGraphCompoundCanonicalPayload& OutPayload,
		FGraphCompoundRouteError& OutError)
	{
		OutPayload = FGraphCompoundCanonicalPayload(
			GetGraphCompoundCanonicalSchemaId(), {});
		OutError = {};
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		const FString Generation = Snapshot->GetRevision().GetGeneration();
		const FString TargetRef = Profile.GetInputKind()
			== EPcgAnimationGraphInputKind::PcgWorldBinding
			? Object->GetStringField(TEXT("component_ref"))
			: (IsAssetCreate(Profile.GetInputKind())
				? AssetTargetRef(*Object)
				: Object->GetStringField(TEXT("target_ref")));
		const TArray<FString> Authority = {
			Profile.GetDomainId() + TEXT(".write")};
		const FString Evidence = TEXT("request:") + Profile.GetCapabilityId();
		TArray<FGraphCompoundCanonicalEdit> Edits;
		TMap<FString, FString> InputPointers;
		FString InputPointer;

		auto FailInput = [&](const TCHAR* Code, const TCHAR* Reason,
			const FString& Field, const FString& Pointer,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const int32 InputIndex = INDEX_NONE)
		{
			OutError = RouteFailure(Code, Reason, TargetRef, Field, MoveTemp(Actual), MoveTemp(Expected));
			OutError.SourceKind = TEXT("command_input");
			OutError.Source.JsonPath = Pointer;
			if (InputIndex != INDEX_NONE) OutError.OperationIndex = InputIndex;
			OutError.TargetPath = TargetRef;
			OutError.ActualValuePresent = OutError.ActualValue.IsValid();
			return false;
		};
		auto ReadIntent = [&](const TSharedPtr<FJsonObject>& Entry,
			const TSharedPtr<FJsonValue>& Value, const TCHAR* Code,
			const FString& Field, const int32 InputIndex, EChangeIntent& Intent)
		{
			if (!Entry.IsValid())
				return FailInput(Code, TEXT("An edit entry must be an object."),
					Field, InputPointer, Value, MakeShared<FJsonValueString>(TEXT("object")), InputIndex);
			const FString IntentText = Entry->GetStringField(TEXT("intent"));
			if (!TryIntent(IntentText, Intent))
				return FailInput(Code, TEXT("The edit intent is not supported."),
					TEXT("intent"), InputPointer + TEXT("/intent"),
					MakeShared<FJsonValueString>(IntentText),
					MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueString>(TEXT("create")), MakeShared<FJsonValueString>(TEXT("update")),
						MakeShared<FJsonValueString>(TEXT("replace")), MakeShared<FJsonValueString>(TEXT("delete"))}), InputIndex);
			return true;
		};


		auto AddJsonEdit = [&] (
			const FString& SemanticId,
			const EGraphCompoundElementKind ElementKind,
			const EChangeIntent Intent,
			const TSharedPtr<FJsonValue>& JsonValue,
			TArray<FUeAgentCanonicalIdentity> Dependencies = {}, const bool bGeneratedValue = false)
		{
			FString ValuePointer = InputPointer;
			if (ValuePointer.IsEmpty())
				for (const auto& Field : Object->Values)
					if (Field.Value == JsonValue) { ValuePointer = TEXT("/") + Field.Key; break; }
			InputPointers.Add(SemanticId, ValuePointer.IsEmpty() ? TEXT("/") : ValuePointer);
			FUeAgentCanonicalValuePtr Proposed;
			if (Intent != EChangeIntent::Delete
				&& !TryCanonical(
					JsonValue, Resources, Proposed, OutError, SemanticId))
			{
				if (bGeneratedValue) MarkGraphCompoundNativeDiagnostic(OutError);
				else BindGraphCompoundInputValueDiagnostic(OutError, ValuePointer.IsEmpty() ? TEXT("/") : ValuePointer);
				return false;
			}
			const FUeAgentCanonicalIdentity EditIdentity = Identity(
				SemanticId, Profile.GetTargetKind(), TargetRef, Generation);
			Edits.Emplace(
				EditIdentity, ElementKind, Intent, Proposed,
				MoveTemp(Dependencies), Authority, Evidence,
				TArray<FUeAgentCanonicalIdentity>({EditIdentity}));
			return true;
		};

		auto AddItemArray = [&] (
			const TCHAR* Field,
			const TCHAR* IdentityField,
			const TCHAR* Prefix,
			const EGraphCompoundElementKind Kind)
		{
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(Field).Num(); ++InputIndex)
			{
				const auto& ItemValue = Object->GetArrayField(Field)[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + Field + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!ReadIntent(Item, ItemValue, TEXT("graph_compound_specialized_item_invalid"), Field, InputIndex, Intent))
				{
					return false;
				}
				const FString IdentityValue =
					Item->GetStringField(IdentityField);
				if (!AddJsonEdit(
						FString(Prefix) + IdentityValue, Kind, Intent,
						ObjectWithout(*Item, {TEXT("intent"), IdentityField})))
				{
					return false;
				}
			}
			return true;
		};

		auto AddParameters = [&] (const TCHAR* Field)
		{
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(Field).Num(); ++InputIndex)
			{
				const auto& ParameterValue = Object->GetArrayField(Field)[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + Field + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Parameter = ParameterValue->AsObject();
				if (!Parameter.IsValid())
					return FailInput(TEXT("graph_compound_parameter_invalid"),
						TEXT("A graph parameter entry must be an object."), Field, InputPointer,
						ParameterValue, MakeShared<FJsonValueString>(TEXT("object")), InputIndex);
				const FString SemanticId = FString::Printf(
					TEXT("property:parameter:%s:%s:%s"),
					*Parameter->GetStringField(TEXT("property_guid")),
					*Parameter->GetStringField(TEXT("property_name")),
					*Parameter->GetStringField(TEXT("property_type_ref")));
				InputPointer += TEXT("/value");
				if (!AddJsonEdit(
						SemanticId, EGraphCompoundElementKind::Property,
						EChangeIntent::Update,
						Parameter->Values.FindChecked(TEXT("value"))))
				{
					return false;
				}
			}
			return true;
		};

		using E = EPcgAnimationGraphInputKind;
		if (IsAssetCreate(Profile.GetInputKind()))
		{
			const TSharedRef<FJsonObject> Definition = MakeShared<FJsonObject>();
			Definition->SetStringField(TEXT("asset_kind"), Profile.GetTargetKind());
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (Pair.Key != TEXT("asset_name")
					&& Pair.Key != TEXT("package_path"))
				{
					Definition->SetField(Pair.Key, Pair.Value);
				}
			}
			if (!AddJsonEdit(
					TEXT("target:") + TargetRef,
					EGraphCompoundElementKind::Target,
					EChangeIntent::Create,
					MakeShared<FJsonValueObject>(Definition), {}, true))
			{
				return false;
			}
		}
		else if (Profile.GetInputKind() == E::GraphEdits)
		{
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("edits")).Num(); ++InputIndex)
			{
				const auto& EditValue = Object->GetArrayField(TEXT("edits"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("edits") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Edit = EditValue->AsObject();
				EGraphCompoundElementKind ElementKind =
					EGraphCompoundElementKind::Item;
				EChangeIntent Intent = EChangeIntent::Update;
				if (!Edit.IsValid())
					return FailInput(TEXT("graph_compound_edit_kind_invalid"),
						TEXT("A graph edit must be an object."), TEXT("edits"), InputPointer,
						EditValue, MakeShared<FJsonValueString>(TEXT("object")), InputIndex);
				if (!TryElementKind(Edit->GetStringField(TEXT("element_kind")), ElementKind))
					return FailInput(TEXT("graph_compound_edit_kind_invalid"),
						TEXT("The graph edit element kind is not supported."),
						TEXT("element_kind"), InputPointer + TEXT("/element_kind"),
						MakeShared<FJsonValueString>(Edit->GetStringField(TEXT("element_kind"))),
						MakeShared<FJsonValueString>(TEXT("target, structure, item, pin, relation, property, input, hierarchy, order, binding, dependency")), InputIndex);
				if (!ReadIntent(Edit, EditValue, TEXT("graph_compound_edit_kind_invalid"), TEXT("edits"), InputIndex, Intent))
					return false;
				TArray<FUeAgentCanonicalIdentity> Dependencies;
				const TArray<TSharedPtr<FJsonValue>>* DependencyValues = nullptr;
				if (Edit->TryGetArrayField(TEXT("depends_on"), DependencyValues)
					&& DependencyValues)
				{
					for (const TSharedPtr<FJsonValue>& Dependency : *DependencyValues)
					{
						Dependencies.Add(Identity(
							Dependency->AsString(), Profile.GetTargetKind(),
							TargetRef, Generation));
					}
				}
				const TSharedPtr<FJsonValue>* Proposed =
					Edit->Values.Find(TEXT("value"));
				if (Intent != EChangeIntent::Delete && !Proposed)
					return FailInput(TEXT("graph_compound_edit_value_missing"),
						TEXT("A non-delete graph edit requires a value."), TEXT("value"),
						InputPointer + TEXT("/value"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), InputIndex);
				if (Proposed) InputPointer += TEXT("/value");
				if (!AddJsonEdit(
						Edit->GetStringField(TEXT("identity")), ElementKind, Intent,
						Proposed ? *Proposed : TSharedPtr<FJsonValue>(),
						MoveTemp(Dependencies)))
				{
					return false;
				}
			}
		}
		else
		{
			switch (Profile.GetInputKind())
			{
			case E::BlackboardSchema:
				for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("entries")).Num(); ++InputIndex)
				{
					const auto& EntryValue = Object->GetArrayField(TEXT("entries"))[InputIndex];
					TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("entries") + TEXT("/") + ::LexToString(InputIndex));
					const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
					EChangeIntent Intent = EChangeIntent::Update;
					if (!ReadIntent(Entry, EntryValue, TEXT("graph_compound_blackboard_entry_invalid"), TEXT("entries"), InputIndex, Intent))
					{
						return false;
					}
					const FString SemanticId = TEXT("item:blackboard_entry:")
						+ Entry->GetStringField(TEXT("entry_name")) + TEXT(":")
						+ Entry->GetStringField(TEXT("key_type_ref")) + TEXT(":")
						+ Entry->GetStringField(TEXT("ownership"));
					if (!AddJsonEdit(
							SemanticId, EGraphCompoundElementKind::Item, Intent,
							ObjectWithout(*Entry, {
								TEXT("intent"), TEXT("entry_name")})))
					{
						return false;
					}
				}
				break;
			case E::SmartObjectDefinition:
				if (!AddItemArray(TEXT("slots"), TEXT("slot_guid"),
					TEXT("item:slot:"), EGraphCompoundElementKind::Item))
				{
					return false;
				}
				break;
			case E::PcgGraphInstance:
				if (!AddJsonEdit(
						TEXT("binding:graph_source"), EGraphCompoundElementKind::Binding,
						EChangeIntent::Update,
						Object->Values.FindChecked(TEXT("graph_ref")))
					|| !AddParameters(TEXT("overrides")))
				{
					return false;
				}
				break;
			case E::PcgParameters:
				if (!AddParameters(TEXT("parameters")))
				{
					return false;
				}
				break;
			case E::PcgComponentBinding:
			case E::PcgWorldBinding:
			{
				FString ActorRef;
				if (Profile.GetInputKind() == E::PcgWorldBinding)
				{
					ActorRef = Object->GetStringField(TEXT("actor_ref"));
				}
				else if (!TrySnapshotStringField(
						*Snapshot, TEXT("binding:pcg_component_graph"),
						TEXT("actor_ref"), ActorRef, OutError))
				{
					OutError.TargetPath = TargetRef;
					return false;
				}
				const TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
				Binding->SetStringField(TEXT("actor_ref"), ActorRef);
				Binding->SetStringField(TEXT("component_ref"), TargetRef);
				Binding->SetStringField(TEXT("generation_policy"),
					Object->GetStringField(TEXT("generation_policy")));
				Binding->SetStringField(TEXT("graph_ref"),
					Object->GetStringField(TEXT("graph_ref")));
				if (!AddJsonEdit(
						TEXT("binding:pcg_component_graph"),
						EGraphCompoundElementKind::Binding,
						EChangeIntent::Update,
						MakeShared<FJsonValueObject>(Binding), {}, true))
				{
					return false;
				}
				break;
			}
			case E::PcgDataAsset:
				if (!AddJsonEdit(
						TEXT("property:data_content:")
							+ Object->GetStringField(TEXT("data_type_ref")),
						EGraphCompoundElementKind::Property,
						EChangeIntent::Replace,
						ObjectWithout(*Object, {TEXT("target_ref")})))
				{
					return false;
				}
				break;
			case E::PcgDataFromLevel:
				if (!AddJsonEdit(
						TEXT("target:") + TargetRef,
						EGraphCompoundElementKind::Target,
						EChangeIntent::Create,
						ObjectWithout(*Object, {TEXT("target_ref")})))
				{
					return false;
				}
				break;
			case E::BlendSpaceDefinition:
				for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("axes")).Num(); ++InputIndex)
				{
					const auto& AxisValue = Object->GetArrayField(TEXT("axes"))[InputIndex];
					TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("axes") + TEXT("/") + ::LexToString(InputIndex));
					const TSharedPtr<FJsonObject> Axis = AxisValue->AsObject();
					const FString SemanticId = FString::Printf(
						TEXT("item:blend_axis:%d"),
						static_cast<int32>(Axis->GetIntegerField(TEXT("axis_index"))));
					if (!AddJsonEdit(
							SemanticId, EGraphCompoundElementKind::Item,
							EChangeIntent::Update, AxisValue))
					{
						return false;
					}
				}
				for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("samples")).Num(); ++InputIndex)
				{
					const auto& SampleValue = Object->GetArrayField(TEXT("samples"))[InputIndex];
					TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("samples") + TEXT("/") + ::LexToString(InputIndex));
					const TSharedPtr<FJsonObject> Sample = SampleValue->AsObject();
					EChangeIntent Intent = EChangeIntent::Update;
					TryIntent(Sample->GetStringField(TEXT("intent")), Intent);
					FString CoordinateIdentity;
					const TArray<TSharedPtr<FJsonValue>>& Coordinates = Sample->GetArrayField(TEXT("coordinates"));
					for (int32 CoordinateIndex = 0; CoordinateIndex < Coordinates.Num(); ++CoordinateIndex)
					{
						const TSharedPtr<FJsonValue>& Coordinate = Coordinates[CoordinateIndex];
						double CoordinateValue = 0.0;
						const bool bNumber = Coordinate->TryGetNumber(CoordinateValue);
						if (!bNumber || !FMath::IsFinite(CoordinateValue))
						{
							TSharedPtr<FJsonValue> Actual = Coordinate;
							if (bNumber) Actual = MakeShared<FJsonValueString>(::LexToString(CoordinateValue));
							return FailInput(TEXT("graph_compound_blend_coordinate_invalid"),
								bNumber ? TEXT("A Blend Space coordinate must be finite.") : TEXT("A Blend Space coordinate must be numeric."),
								TEXT("coordinates"), InputPointer + TEXT("/coordinates/") + ::LexToString(CoordinateIndex),
								MoveTemp(Actual), MakeShared<FJsonValueString>(TEXT("finite number")), InputIndex);
						}
						CoordinateIdentity += FString::SanitizeFloat(CoordinateValue)
							+ TEXT(",");
					}
					if (!AddJsonEdit(
							TEXT("item:blend_sample:")
								+ Sample->GetStringField(TEXT("animation_ref"))
								+ TEXT("@") + CoordinateIdentity,
							EGraphCompoundElementKind::Item,
							Intent,
							ObjectWithout(*Sample, {TEXT("intent")})))
					{
						return false;
					}
				}
				break;
			case E::SkeletonCompatibility:
				if (!AddItemArray(TEXT("changes"), TEXT("skeleton_ref"),
					TEXT("binding:compatible_skeleton:"),
					EGraphCompoundElementKind::Binding))
				{
					return false;
				}
				break;
			case E::SkeletonVirtualBone:
			{
				EChangeIntent Intent = EChangeIntent::Update;
				TryIntent(Object->GetStringField(TEXT("intent")), Intent);
				if (!AddJsonEdit(
						TEXT("hierarchy:virtual_bone:")
							+ Object->GetStringField(TEXT("name")),
						EGraphCompoundElementKind::Hierarchy, Intent,
						ObjectWithout(*Object, {
							TEXT("intent"), TEXT("name"), TEXT("target_ref")})))
				{
					return false;
				}
				break;
			}
			case E::ControlRigShapeLibrary:
				if (!AddItemArray(TEXT("shapes"), TEXT("name"),
					TEXT("item:shape:"), EGraphCompoundElementKind::Item))
				{
					return false;
				}
				break;
			case E::DeformerSourceBindings:
				if (!AddJsonEdit(
						TEXT("property:concrete_source_class"),
						EGraphCompoundElementKind::Property,
						EChangeIntent::Update,
						Object->Values.FindChecked(TEXT("concrete_class_ref"))))
				{
					return false;
				}
				for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("bindings")).Num(); ++InputIndex)
				{
					const auto& BindingValue = Object->GetArrayField(TEXT("bindings"))[InputIndex];
					TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("bindings") + TEXT("/") + ::LexToString(InputIndex));
					const TSharedPtr<FJsonObject> Binding = BindingValue->AsObject();
					EChangeIntent Intent = EChangeIntent::Update;
					if (!ReadIntent(Binding, BindingValue, TEXT("graph_compound_source_binding_invalid"), TEXT("bindings"), InputIndex, Intent))
					{
						return false;
					}
					const FString SemanticId = TEXT("binding:source:")
						+ Object->GetStringField(TEXT("concrete_class_ref")) + TEXT(":")
						+ Binding->GetStringField(TEXT("binding_key")) + TEXT(":")
						+ Binding->GetStringField(TEXT("property_type_ref"));
					if (!AddJsonEdit(
							SemanticId, EGraphCompoundElementKind::Binding, Intent,
							ObjectWithout(*Binding, {
								TEXT("intent"), TEXT("binding_key")})))
					{
						return false;
					}
				}
				break;
			case E::MeshDeformerCollectionEntries:
				if (!AddItemArray(TEXT("entries"), TEXT("entry_ref"),
					TEXT("item:collection_entry:"), EGraphCompoundElementKind::Item))
				{
					return false;
				}
				break;
			case E::MlDeformerConfiguration:
				if (!AddJsonEdit(
						TEXT("property:ml_configuration"),
						EGraphCompoundElementKind::Property,
						EChangeIntent::Replace,
						ObjectWithout(*Object, {TEXT("target_ref")})))
				{
					return false;
				}
				break;
			case E::IkRigDefinition:
				if (!AddJsonEdit(
						TEXT("property:retarget_root"),
						EGraphCompoundElementKind::Property,
						EChangeIntent::Update,
						Object->Values.FindChecked(TEXT("retarget_root")))
					|| !AddItemArray(TEXT("goals"), TEXT("name"),
						TEXT("item:ik_goal:"), EGraphCompoundElementKind::Item)
					|| !AddItemArray(TEXT("solvers"), TEXT("solver_ref"),
						TEXT("order:ik_solver:"), EGraphCompoundElementKind::Order)
					|| !AddItemArray(TEXT("chains"), TEXT("name"),
						TEXT("hierarchy:ik_chain:"), EGraphCompoundElementKind::Hierarchy))
				{
					return false;
				}
				break;
			case E::IkRigAutoGenerate:
				if (!AddJsonEdit(
						TEXT("property:definition:auto_generate"),
						EGraphCompoundElementKind::Property,
						EChangeIntent::Update,
						ObjectWithout(*Object, {TEXT("target_ref")})))
				{
					return false;
				}
				break;
			case E::IkRetargeterDefinition:
				if (!AddJsonEdit(
						TEXT("binding:source_ik_rig"),
						EGraphCompoundElementKind::Binding,
						EChangeIntent::Update,
						Object->Values.FindChecked(TEXT("source_ik_rig_ref")))
					|| !AddJsonEdit(
						TEXT("binding:target_ik_rig"),
						EGraphCompoundElementKind::Binding,
						EChangeIntent::Update,
						Object->Values.FindChecked(TEXT("target_ik_rig_ref")))
					|| !AddItemArray(TEXT("chain_mappings"), TEXT("target_chain"),
						TEXT("binding:chain_mapping:"),
						EGraphCompoundElementKind::Binding))
				{
					return false;
				}
				break;
			case E::IkRetargeterSettings:
				for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("settings")).Num(); ++InputIndex)
				{
					const auto& SettingValue = Object->GetArrayField(TEXT("settings"))[InputIndex];
					TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("settings") + TEXT("/") + ::LexToString(InputIndex));
					const TSharedPtr<FJsonObject> Setting = SettingValue->AsObject();
					const FString SemanticId = TEXT("property:setting:")
						+ Setting->GetStringField(TEXT("scope")) + TEXT(":")
						+ Setting->GetStringField(TEXT("chain_name")) + TEXT(":")
						+ Setting->GetStringField(TEXT("setting_name"));
					InputPointer += TEXT("/value");
					if (!AddJsonEdit(
							SemanticId,
							EGraphCompoundElementKind::Property,
							SnapshotContainsSemanticId(Snapshot.Get(), SemanticId)
								? EChangeIntent::Update : EChangeIntent::Create,
							Setting->Values.FindChecked(TEXT("value"))))
					{
						return false;
					}
				}
				break;
			default:
				OutError = RouteFailure(TEXT("graph_compound_typed_input_kind_invalid"),
					TEXT("The PCG/animation typed operation input kind is not supported."), Profile.GetCapabilityId(), TEXT("input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetInputKind())),
					MakeShared<FJsonValueString>(TEXT("a supported PCG/animation typed operation input kind")));
				return false;
			}
		}

		OutPayload = FGraphCompoundCanonicalPayload(
			GetGraphCompoundCanonicalSchemaId(), MoveTemp(Edits), MoveTemp(InputPointers));
		if (OutPayload.GetEdits().IsEmpty())
			return FailInput(TEXT("graph_compound_typed_operation_empty"),
				TEXT("The typed operation contains no edits."), TEXT("edits"), TEXT("/"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one edit")));
		return true;
	}

	bool TryBuildPcgAnimationGraphTypedOperationDraft(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FUeAgentContractValue& Input,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& RequestId,
		const FString& IdempotencyKey,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FGraphCompoundRouteError& OutError)
	{
		OutDraft.Reset();
		OutError = {};
		if (DescriptorIdentity
			!= Profile.GetExecutionContract().GetDescriptorIdentity())
		{
			const FUeAgentDescriptorIdentity& Expected = Profile.GetExecutionContract().GetDescriptorIdentity();
			OutError = RouteFailure(TEXT("graph_compound_descriptor_identity_mismatch"),
				TEXT("The operation descriptor does not match the graph route."),
				Profile.GetDescriptorId(), TEXT("descriptor_identity"), nullptr, nullptr);
			if (DescriptorIdentity.GetDescriptorId() != Expected.GetDescriptorId())
			{
				OutError.PropertyPath = TEXT("descriptor_identity.descriptor_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(DescriptorIdentity.GetDescriptorId());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected.GetDescriptorId());
			}
			else if (DescriptorIdentity.GetVersion() != Expected.GetVersion())
			{
				OutError.PropertyPath = TEXT("descriptor_identity.version");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(DescriptorIdentity.GetVersion());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Expected.GetVersion());
			}
			else
			{
				OutError.PublicMessage = TEXT("The operation descriptor content does not match the graph route.");
				OutError.PropertyPath = TEXT("descriptor_identity.descriptor_hash");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(DescriptorIdentity.GetDescriptorHash());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetDescriptorHash());
			}
			return false;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		if (!TryDecodePcgAnimationGraphTypedOperation(
				Profile, Input, Snapshot, Resources, Payload, OutError))
		{
			return false;
		}
		FGraphCompoundCanonicalError CanonicalError;
		if (!TryBuildGraphCompoundChangeSetDraft(
				Payload, Snapshot, DescriptorIdentity,
				RequestId, IdempotencyKey, Resources,
				OutDraft, CanonicalError))
		{
			Payload.LocateDiagnostic(CanonicalError);
			SetError(OutError, CanonicalError, CanonicalError.SubjectId);
			return false;
		}
		return OutDraft.IsSet();
	}
}
