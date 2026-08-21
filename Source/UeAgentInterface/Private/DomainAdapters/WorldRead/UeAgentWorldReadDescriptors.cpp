#include "DomainAdapters/WorldRead/UeAgentWorldReadDescriptors.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::WorldRead
{
	using namespace UE::UAI::Contract;

	namespace
	{
		FString CanonicalSchema(const TCHAR* Schema)
		{
			TSharedPtr<FJsonObject> Object;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Schema);
			if (!FJsonSerializer::Deserialize(Reader, Object)
				|| !Object.IsValid())
			{
				return Schema;
			}
			FString Canonical;
			FString Error;
			return Json::TryCanonicalize(
					Object,
					Canonical,
					Error)
				? Canonical
				: FString(Schema);
		}

		void Add(
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			const TCHAR* DescriptorId,
			const TCHAR* DescriptorHash,
			const TCHAR* CapabilityId,
			const TCHAR* ProviderBinding,
			EUeAgentQueryKind QueryKind,
			TArray<EUeAgentStatePlane> StatePlanes,
			TArray<EUeAgentExecutionContextFacet> Facets,
			const TCHAR* InputSchema)
		{
			OutDescriptors.Emplace(
				FUeAgentDescriptorIdentity(
					DescriptorId,
					1,
					DescriptorHash),
				CapabilityId,
				TEXT("world"),
				EUeAgentDescriptorOperationKind::Query,
				FUeAgentExecutionContextPolicy(
					MoveTemp(StatePlanes),
					MoveTemp(Facets),
					EUeAgentContextResolutionPolicy::
						ResolveCurrentThenFreeze,
					EUeAgentTransientContextPolicy::ReadOnly),
				FUeAgentContractValue(
					CanonicalSchema(InputSchema)),
				FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
				FUeAgentQueryBinding(
					ProviderBinding,
					QueryKind,
					EUeAgentQueryMaterializationPolicy::
						RuntimeObserve,
					EUeAgentQuerySideEffectPolicy::
						NoPersistentMutation));
		}

		TArray<EUeAgentStatePlane> Authoring()
		{
			return {EUeAgentStatePlane::AuthoringMemory};
		}

		TArray<EUeAgentStatePlane> AuthoringAndRuntime()
		{
			return {
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::RuntimeInstance,
			};
		}

		TArray<EUeAgentExecutionContextFacet> WorldFacet()
		{
			return {EUeAgentExecutionContextFacet::World};
		}

		void AddObjectDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors)
		{
			Add(
				Descriptors,
				TEXT("uai.query.world.summary"),
				TEXT("sha256:0c8c315c7e3f6d714fd0ee10c9fe14342917408f095d6d0da55c72fd4741234f"),
				TEXT("world.summary.read"),
				TEXT("world.read.summary"),
				EUeAgentQueryKind::State,
				Authoring(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.actors"),
				TEXT("sha256:1a4c1b9fa68e2bad414de0f57869ccbf98029caa510cdd278891d63166933102"),
				TEXT("world.actors.read"),
				TEXT("world.read.actors"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"class_paths":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"folder_path_prefix":{"type":"string"},"tags":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.actor_components"),
				TEXT("sha256:72992b27afa74cc3c2d5b604d502ab2dd2e2612928c7358691922222f6fa42d0"),
				TEXT("world.actor_components.read"),
				TEXT("world.read.actor_components"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"include_non_scene":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.selection"),
				TEXT("sha256:8e498e351b0cb08a1e9e78f7596af6117dd66fce8b6257aac2e70ed114615126"),
				TEXT("world.selection.read"),
				TEXT("world.read.selection"),
				EUeAgentQueryKind::State,
				Authoring(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.property"),
				TEXT("sha256:87f836780eed9a42654d2865889c74b0212a532eafcc86d3f4615e4931e6ac6e"),
				TEXT("world.property.read"),
				TEXT("world.read.property"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"property_path":{"type":"string","minLength":1}},"required":["property_path"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.content"),
				TEXT("sha256:f31e1b14ba2556edfc9914f8c93cd2c3b56b164703637ee6f2eb8ae03dce8933"),
				TEXT("world.content.read"),
				TEXT("world.read.content"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"scope":{"type":"string","enum":["world","selection","target"]},"include_bounds":{"type":"boolean"},"include_components":{"type":"boolean"},"sections":{"type":"array","items":{"type":"string","enum":["actors","attachments","components","folders","world_settings"]},"uniqueItems":true}},"additionalProperties":false})json"));
		}

		void AddSpatialDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors)
		{
			Add(
				Descriptors,
				TEXT("uai.query.world.ray_trace"),
				TEXT("sha256:40abdbe0435cbf04cf7f2c8f856d85b7127c7a3b534bf36e3e8cb5a59f832922"),
				TEXT("world.ray_trace.read"),
				TEXT("world.read.ray_trace"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"start":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"direction":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","components"],"additionalProperties":false},"distance_cm":{"type":"number","exclusiveMinimum":0},"trace_channel":{"type":"string","minLength":1},"trace_complex":{"type":"boolean"},"ignore_target_refs":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["start","direction","distance_cm"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.overlap"),
				TEXT("sha256:63fbf20fa3085d13ac96c2dddd55e755063251099c04ef8eff872bff93f742eb"),
				TEXT("world.overlap.read"),
				TEXT("world.read.overlap"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"shape":{"type":"string","enum":["box","sphere","capsule"]},"center":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"half_extents_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3},"radius_cm":{"type":"number","minimum":0},"half_height_cm":{"type":"number","minimum":0},"trace_channel":{"type":"string","minLength":1},"trace_complex":{"type":"boolean"},"ignore_target_refs":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["shape","center"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.capsule_sweep"),
				TEXT("sha256:6cb40b6352fa8bdee487b33318b811c0fde347e6b0b16ac5e8af4939fc71a232"),
				TEXT("world.capsule_sweep.read"),
				TEXT("world.read.capsule_sweep"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"start":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"end":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"radius_cm":{"type":"number","exclusiveMinimum":0},"half_height_cm":{"type":"number","exclusiveMinimum":0},"trace_channel":{"type":"string","minLength":1},"trace_complex":{"type":"boolean"},"find_initial_overlaps":{"type":"boolean"},"return_penetration_depth":{"type":"boolean"},"ignore_target_refs":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["start","end","radius_cm","half_height_cm"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.capsule_path"),
				TEXT("sha256:23f139aae58ab8c2a4864fa429f5ec110f750e668019cf84ffdf41e1f7681fed"),
				TEXT("world.capsule_path.read"),
				TEXT("world.read.capsule_path"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"points":{"type":"array","items":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"minItems":2},"radius_cm":{"type":"number","exclusiveMinimum":0},"half_height_cm":{"type":"number","exclusiveMinimum":0},"step_cm":{"type":"number","exclusiveMinimum":0},"trace_channel":{"type":"string","minLength":1},"trace_complex":{"type":"boolean"},"require_floor":{"type":"boolean"},"snap_to_floor":{"type":"boolean"},"floor_trace_channel":{"type":"string","minLength":1},"floor_trace_up_cm":{"type":"number","minimum":0},"floor_trace_down_cm":{"type":"number","minimum":0},"floor_clearance_cm":{"type":"number","minimum":0},"max_walkable_slope_deg":{"type":"number","minimum":0,"maximum":90}},"required":["points","radius_cm","half_height_cm","step_cm"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.nearby_shapes"),
				TEXT("sha256:c00aecd11cc68be751776bcce5cb468f09aa3a10447af67ea0485723c064707b"),
				TEXT("world.nearby_shapes.read"),
				TEXT("world.read.nearby_shapes"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"center":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"half_extents_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3},"radius_cm":{"type":"number","minimum":0},"component_classes":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"folder_path_prefix":{"type":"string"},"ignore_folder_path_prefix":{"type":"string"},"include_visual_fallback":{"type":"boolean"},"detail_level":{"type":"string","enum":["summary","shape"]}},"required":["center"],"additionalProperties":false})json"));
		}

		void AddMeshNavigationDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors)
		{
			Add(
				Descriptors,
				TEXT("uai.query.world.mesh_closest_vertex"),
				TEXT("sha256:708d8ded16d9dc7430c6f4b8367950be500a1becfbbbe8fc63c7433cead7e5e5"),
				TEXT("world.mesh_closest_vertex.read"),
				TEXT("world.read.mesh_closest_vertex"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"world_point":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false}},"required":["world_point"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.mesh_vertex_position"),
				TEXT("sha256:345ba8f9c81478ee39ae4fe35249acc957e42f515c808da50e260e5abd239e11"),
				TEXT("world.mesh_vertex_position.read"),
				TEXT("world.read.mesh_vertex_position"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"vertex_index":{"type":"integer","minimum":0}},"required":["vertex_index"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.nav_project_point"),
				TEXT("sha256:15717c38f15bebf7333fd5be32b31d79bafa1cf2ed80468e9425f73b9fd72d44"),
				TEXT("world.nav_project_point.read"),
				TEXT("world.read.nav_project_point"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"point":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"query_extent_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3}},"required":["point"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.nav_find_path"),
				TEXT("sha256:a7f385ab9d197393c8ece37f40f2fc3157d9aea6f36ebbe698aa6ccc9add86da"),
				TEXT("world.nav_find_path.read"),
				TEXT("world.read.nav_find_path"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"start":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"end":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"project_to_nav":{"type":"boolean"},"allow_projection_failure":{"type":"boolean"},"allow_partial":{"type":"boolean"},"query_extent_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3}},"required":["start","end"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.nav_connectivity"),
				TEXT("sha256:24e8a35b076afb6cf2c731b1df5f5717a388552db1bd3dcaa9bdf3a7f57b7652"),
				TEXT("world.nav_connectivity.read"),
				TEXT("world.read.nav_connectivity"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"points":{"type":"array","items":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"minItems":2},"pairs":{"type":"array","items":{"type":"array","items":{"type":"integer","minimum":0},"minItems":2,"maxItems":2}},"project_to_nav":{"type":"boolean"},"allow_partial":{"type":"boolean"},"include_path_points":{"type":"boolean"},"query_extent_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3}},"required":["points"],"additionalProperties":false})json"));
		}

		void AddTopologyPhysicsDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors)
		{
			Add(
				Descriptors,
				TEXT("uai.query.world.data_layers"),
				TEXT("sha256:2846909763f242c7f5d084e77a1af3d6478246d1cc1511267aaa61a113e6982f"),
				TEXT("world.data_layers.read"),
				TEXT("world.read.data_layers"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"scope":{"type":"string","enum":["world","selection"]},"include_actor_membership":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.hlod_assignments"),
				TEXT("sha256:26cdee2d8823b369e95b10dcf8abc69dcb148113f26d99e45c86034a6cabf8d5"),
				TEXT("world.hlod_assignments.read"),
				TEXT("world.read.hlod_assignments"),
				EUeAgentQueryKind::State,
				Authoring(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"scope":{"type":"string","enum":["world","selection"]},"folder_path_prefix":{"type":"string"},"managed_tag":{"type":"string"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.streaming_levels"),
				TEXT("sha256:8805bbe7dd2243fcca8a90eceda5e0e43c36285def0a95847efa8a5f2b9df93b"),
				TEXT("world.streaming_levels.read"),
				TEXT("world.read.streaming_levels"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"level_packages":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"scope":{"type":"string","enum":["world","selection"]}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.partition"),
				TEXT("sha256:8b1a726d0d8387887d797908477a2fd164c081b817f9fc21a4460ce148dce6df"),
				TEXT("world.partition.read"),
				TEXT("world.read.partition"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"include_loaded_regions":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.partition_health"),
				TEXT("sha256:4acdb35c196d9a7237e5438a55d77aa221042ecc076fd4c172c8dcdf97064e95"),
				TEXT("world.partition_health.read"),
				TEXT("world.read.partition_health"),
				EUeAgentQueryKind::RuntimeProbe,
				Authoring(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"enable_streaming":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.physics_component"),
				TEXT("sha256:e93cf11fe378e554d8c9a61ac41e3145aa3c53365f8e60f4d100b64f979cd9b0"),
				TEXT("world.physics_component.read"),
				TEXT("world.read.physics_component"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"include_collision_responses":{"type":"boolean"},"include_material":{"type":"boolean"},"include_runtime_state":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.world.physics_constraint"),
				TEXT("sha256:181cb31c54cd3619c0df954a83414e7c8d81ffa2330628479cb0c1b20ce147bc"),
				TEXT("world.physics_constraint.read"),
				TEXT("world.read.physics_constraint"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"include_endpoints":{"type":"boolean"},"include_limits":{"type":"boolean"},"include_runtime_state":{"type":"boolean"}},"additionalProperties":false})json"));
		}
	}

	bool TryCreateWorldReadDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(23);
		AddObjectDescriptors(Descriptors);
		AddSpatialDescriptors(Descriptors);
		AddMeshNavigationDescriptors(Descriptors);
		AddTopologyPhysicsDescriptors(Descriptors);
		if (Descriptors.Num() != 23)
		{
			OutRegistry.Reset();
			OutError = TEXT("world_read_descriptor_count_invalid");
			return false;
		}
		for (const FUeAgentOperationDescriptor& Descriptor
			: Descriptors)
		{
			TSharedPtr<const FUeAgentDescriptorRegistry>
				SingleRegistry;
			FString SingleError;
			if (!FUeAgentDescriptorRegistry::TryCreate(
					{Descriptor},
					SingleRegistry,
					SingleError))
			{
				OutRegistry.Reset();
				OutError = FString::Printf(
					TEXT("%s:%s"),
					*Descriptor.GetIdentity().GetDescriptorId(),
					*SingleError);
				return false;
			}
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}
}
