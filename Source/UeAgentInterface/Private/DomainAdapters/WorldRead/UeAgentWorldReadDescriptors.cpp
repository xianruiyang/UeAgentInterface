#include "DomainAdapters/WorldRead/UeAgentWorldReadDescriptors.h"

#include "Core/Contract/UeAgentDiagnostic.h"

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
			UE::UAI::Contract::FDiagnostic Error;
			return Json::TryCanonicalize(
					Object,
					Canonical,
					Error)
				? Canonical
				: FString(Schema);
		}

		void AddWithError(
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			const TCHAR* DescriptorId,
			const TCHAR* CapabilityId,
			const TCHAR* ProviderBinding,
			EUeAgentQueryKind QueryKind,
			TArray<EUeAgentStatePlane> StatePlanes,
			TArray<EUeAgentExecutionContextFacet> Facets,
			const TCHAR* InputSchema,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!OutError.IsEmpty())
			{
				return;
			}
			auto BuildDescriptor = [=](const FString& DescriptorHash)
			{
				return FUeAgentOperationDescriptor(
					FUeAgentDescriptorIdentity(
						DescriptorId,
						1,
						DescriptorHash),
					CapabilityId,
				TEXT("world"),
				EUeAgentDescriptorOperationKind::Query,
				FUeAgentExecutionContextPolicy(
					StatePlanes,
					Facets,
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
			};
			const FUeAgentOperationDescriptor Draft =
				BuildDescriptor(FString());
			FString DescriptorHash;
			if (!TryComputeOperationDescriptorHash(
					Draft,
					DescriptorHash,
					OutError))
			{
				return;
			}
			OutDescriptors.Add(BuildDescriptor(DescriptorHash));
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
			TArray<FUeAgentOperationDescriptor>& Descriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			AddWithError(Descriptors, TEXT("uai.query.world.scene_context"),
				TEXT("world.scene_context.read"), TEXT("world.read.scene_context"),
				EUeAgentQueryKind::State, Authoring(), WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"workspace_root":{"type":"string"},"analysis_ref":{"type":"string"},"targets":{"type":"array","items":{"type":"string"}},"regions":{"type":"array","items":{"type":"object","properties":{"min_cm":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"max_cm":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["min_cm","max_cm"],"additionalProperties":false}},"margin_cm":{"type":"number","minimum":0},"near_distance_cm":{"type":"number","minimum":0,"maximum":10},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":200},"object_ref":{"type":"string"}},"additionalProperties":false})json"), OutError);
			AddWithError(Descriptors, TEXT("uai.query.world.geometry_overlap"),
				TEXT("world.geometry_overlap.read"), TEXT("world.read.geometry_overlap"),
				EUeAgentQueryKind::State, Authoring(), WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"workspace_root":{"type":"string"},"analysis_ref":{"type":"string"},"targets":{"type":"array","items":{"type":"string"}},"regions":{"type":"array","items":{"type":"object","properties":{"min_cm":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"max_cm":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["min_cm","max_cm"],"additionalProperties":false}},"margin_cm":{"type":"number","minimum":0},"near_distance_cm":{"type":"number","minimum":0,"maximum":10},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":200},"object_ref":{"type":"string"}},"additionalProperties":false})json"), OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.summary"),
				TEXT("world.summary.read"),
				TEXT("world.read.summary"),
				EUeAgentQueryKind::State,
				Authoring(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.actors"),
				TEXT("world.actors.read"),
				TEXT("world.read.actors"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"class_paths":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"folder_path_prefix":{"type":"string"},"tags":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.actor_components"),
				TEXT("world.actor_components.read"),
				TEXT("world.read.actor_components"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.selection"),
				TEXT("world.selection.read"),
				TEXT("world.read.selection"),
				EUeAgentQueryKind::State,
				Authoring(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.property"),
				TEXT("world.property.read"),
				TEXT("world.read.property"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"property_path":{"type":"string","minLength":1}},"required":["property_path"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.content"),
				TEXT("world.content.read"),
				TEXT("world.read.content"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"scope":{"type":"string","enum":["world","selection","target"]},"include_bounds":{"type":"boolean"},"include_components":{"type":"boolean"},"sections":{"type":"array","items":{"type":"string","enum":["actors","attachments","components","folders","world_settings"]},"uniqueItems":true}},"additionalProperties":false})json"),
				OutError);
		}

		void AddSpatialDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.ray_trace"),
				TEXT("world.ray_trace.read"),
				TEXT("world.read.ray_trace"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"start":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"direction":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","components"],"additionalProperties":false},"distance_cm":{"type":"number","exclusiveMinimum":0},"trace_channel":{"type":"string","minLength":1},"trace_complex":{"type":"boolean"}},"required":["start","direction","distance_cm"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.overlap"),
				TEXT("world.overlap.read"),
				TEXT("world.read.overlap"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"shape":{"type":"string","enum":["box","sphere","capsule"]},"center":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"half_extents_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3},"radius_cm":{"type":"number","minimum":0},"half_height_cm":{"type":"number","minimum":0},"trace_channel":{"type":"string","minLength":1},"include_targets":{"type":"boolean"}},"required":["shape","center"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.capsule_sweep"),
				TEXT("world.capsule_sweep.read"),
				TEXT("world.read.capsule_sweep"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"start":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"end":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"radius_cm":{"type":"number","exclusiveMinimum":0},"half_height_cm":{"type":"number","exclusiveMinimum":0},"trace_channel":{"type":"string","minLength":1},"trace_complex":{"type":"boolean"},"find_initial_overlaps":{"type":"boolean"},"include_hits":{"type":"boolean"}},"required":["start","end","radius_cm","half_height_cm"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.capsule_path"),
				TEXT("world.capsule_path.read"),
				TEXT("world.read.capsule_path"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"points":{"type":"array","items":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"minItems":2},"radius_cm":{"type":"number","exclusiveMinimum":0},"half_height_cm":{"type":"number","exclusiveMinimum":0},"step_cm":{"type":"number","exclusiveMinimum":0},"trace_channel":{"type":"string","minLength":1},"include_first_hit":{"type":"boolean"},"trace_complex":{"type":"boolean"}},"required":["points","radius_cm","half_height_cm","step_cm"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.nearby_shapes"),
				TEXT("world.nearby_shapes.read"),
				TEXT("world.read.nearby_shapes"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"center":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"half_extents_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3},"radius_cm":{"type":"number","minimum":0},"folder_path_prefix":{"type":"string"},"ignore_folder_path_prefix":{"type":"string"},"include_shapes":{"type":"boolean"}},"required":["center"],"additionalProperties":false})json"),
				OutError);
		}

		void AddMeshNavigationDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.mesh_closest_vertex"),
				TEXT("world.mesh_closest_vertex.read"),
				TEXT("world.read.mesh_closest_vertex"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"world_point":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false}},"required":["world_point"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.mesh_vertex_position"),
				TEXT("world.mesh_vertex_position.read"),
				TEXT("world.read.mesh_vertex_position"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"vertex_index":{"type":"integer","minimum":0}},"required":["vertex_index"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.nav_project_point"),
				TEXT("world.nav_project_point.read"),
				TEXT("world.read.nav_project_point"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"point":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"query_extent_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3}},"required":["point"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.nav_find_path"),
				TEXT("world.nav_find_path.read"),
				TEXT("world.read.nav_find_path"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"start":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"end":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"project_to_nav":{"type":"boolean"},"allow_projection_failure":{"type":"boolean"},"allow_partial":{"type":"boolean"},"include_path_points":{"type":"boolean"},"query_extent_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3}},"required":["start","end"],"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.nav_connectivity"),
				TEXT("world.nav_connectivity.read"),
				TEXT("world.read.nav_connectivity"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"points":{"type":"array","items":{"type":"object","properties":{"frame_id":{"type":"string","const":"ue.world"},"unit":{"type":"string","const":"centimeter"},"components":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["frame_id","unit","components"],"additionalProperties":false},"minItems":2},"pairs":{"type":"array","items":{"type":"array","items":{"type":"integer","minimum":0},"minItems":2,"maxItems":2}},"project_to_nav":{"type":"boolean"},"allow_projection_failure":{"type":"boolean"},"allow_partial":{"type":"boolean"},"include_path_points":{"type":"boolean"},"query_extent_cm":{"type":"array","items":{"type":"number","minimum":0},"minItems":3,"maxItems":3}},"required":["points"],"additionalProperties":false})json"),
				OutError);
		}

		void AddTopologyPhysicsDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.data_layers"),
				TEXT("world.data_layers.read"),
				TEXT("world.read.data_layers"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"scope":{"type":"string","enum":["world","selection"]},"include_actor_membership":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.hlod_assignments"),
				TEXT("world.hlod_assignments.read"),
				TEXT("world.read.hlod_assignments"),
				EUeAgentQueryKind::State,
				Authoring(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"folder_path_prefix":{"type":"string"},"managed_tag":{"type":"string"}},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.streaming_levels"),
				TEXT("world.streaming_levels.read"),
				TEXT("world.read.streaming_levels"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"level_packages":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.partition"),
				TEXT("world.partition.read"),
				TEXT("world.read.partition"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"include_loaded_regions":{"type":"boolean"},"include_actor_descriptors":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.partition_health"),
				TEXT("world.partition_health.read"),
				TEXT("world.read.partition_health"),
				EUeAgentQueryKind::RuntimeProbe,
				Authoring(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"enable_streaming":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.physics_component"),
				TEXT("world.physics_component.read"),
				TEXT("world.read.physics_component"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"include_collision_responses":{"type":"boolean"},"include_material":{"type":"boolean"},"include_runtime_state":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			AddWithError(
				Descriptors,
				TEXT("uai.query.world.physics_constraint"),
				TEXT("world.physics_constraint.read"),
				TEXT("world.read.physics_constraint"),
				EUeAgentQueryKind::State,
				AuthoringAndRuntime(),
				WorldFacet(),
				TEXT(R"json({"type":"object","properties":{"include_endpoints":{"type":"boolean"},"include_limits":{"type":"boolean"},"include_runtime_state":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
		}

	}

	bool TryCreateWorldReadDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<FUeAgentOperationDescriptor> Descriptors;
		AddObjectDescriptors(Descriptors, OutError);
		AddSpatialDescriptors(Descriptors, OutError);
		AddMeshNavigationDescriptors(Descriptors, OutError);
		AddTopologyPhysicsDescriptors(Descriptors, OutError);
		if (!OutError.IsEmpty())
		{
			OutRegistry.Reset();
			return false;
		}
		for (const FUeAgentOperationDescriptor& Descriptor
			: Descriptors)
		{
			TSharedPtr<const FUeAgentDescriptorRegistry>
				SingleRegistry;
			UE::UAI::Contract::FDiagnostic SingleError;
			if (!FUeAgentDescriptorRegistry::TryCreate(
					{Descriptor},
					SingleRegistry,
					SingleError))
			{
				OutRegistry.Reset();
				OutError = MoveTemp(SingleError);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}
}
