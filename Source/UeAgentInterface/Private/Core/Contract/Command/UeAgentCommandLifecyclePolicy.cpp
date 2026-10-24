#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"

#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::CommandLifecycle
{
	using namespace CommandContract;

	namespace
	{
		void AppendDormantPolicies(
			TArray<FCommandLifecyclePolicy>& Policies,
			const TArray<FString>& CommandIds,
			const TArray<FString>& SuccessorCommandIds,
			const TCHAR* Reason)
		{
			for (const FString& CommandId : CommandIds)
			{
				Policies.Add({
					CommandId,
					EUeAgentCommandAvailability::Dormant,
					SuccessorCommandIds,
					Reason,
				});
			}
		}
	}

	const TArray<FCommandLifecyclePolicy>& GetPolicies()
	{
		static const TArray<FCommandLifecyclePolicy> Policies = []
		{
			TArray<FCommandLifecyclePolicy> Result = {
			{
				TEXT("world.actor.create"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("world.actor.candidates"),
				 TEXT("content_graph.import_workspace")},
				TEXT("Persisted Actor creation belongs to Level Actor HJSON after native candidate discovery."),
			},
			{
				TEXT("world.actors"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.structure"),
				 TEXT("content_graph.items")},
				TEXT("Persisted World Actor discovery belongs to World HJSON."),
			},
			{
				TEXT("world.actor_components"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.items")},
				TEXT("Persisted Actor component structure belongs to World HJSON."),
			},
			{
				TEXT("world.content"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("query.content.search"),
				 TEXT("content_graph.structure"),
				 TEXT("content_graph.items")},
				TEXT("World Actor and Component discovery belongs to content search and persisted World HJSON."),
			},
			{
				TEXT("world.property"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.properties")},
				TEXT("Registered persisted Actor and Component properties belong to World HJSON."),
			},
			{
				TEXT("world.object_transform.read"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.structure"),
				 TEXT("content_graph.items")},
				TEXT("Persisted Actor and Component transform readback belongs to World HJSON."),
			},
			{
				TEXT("world.object_transform.update"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.import_workspace")},
				TEXT("Persisted Actor and Component transform mutation belongs to World HJSON."),
			},
			{
				TEXT("asset.registry_relations"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("asset.relations")},
				TEXT("Asset relation discovery belongs to the structured asset.relations graph; this registry-only route is compatibility-only."),
			},
			{
				TEXT("animation.deformer_source_library.asset.create"),
				EUeAgentCommandAvailability::Disabled,
				{},
				TEXT("No enabled plugin provides an adjudicated concrete native owner for Deformer Source Library creation."),
			},
			{
				TEXT("animation.ml_deformer.asset.create"),
				EUeAgentCommandAvailability::Disabled,
				{},
				TEXT("No enabled plugin provides an adjudicated concrete native owner for ML Deformer creation."),
			},
			};

			AppendDormantPolicies(
				Result,
				{
					TEXT("animation.anim_blueprint.asset.create"),
				},
				{
					TEXT("content_graph.export_workspace"),
					TEXT("content_graph.import_workspace"),
				},
				TEXT("Animation Blueprint creation and desired state belong to the common HJSON authoring surface."));

			AppendDormantPolicies(
				Result,
				{
					TEXT("asset.audio_effect_preset.update"),
					TEXT("asset.curve.update"),
					TEXT("asset.data_object.update"),
					TEXT("asset.data_table.replace"),
					TEXT("asset.input_action.update"),
					TEXT("asset.input_mapping_context.update"),
					TEXT("asset.media_player.update"),
					TEXT("asset.media_source.update"),
					TEXT("asset.media_texture.update"),
					TEXT("asset.paper_flipbook.update"),
					TEXT("asset.paper_sprite.update"),
					TEXT("asset.paper_tilemap.update"),
					TEXT("asset.paper_tileset.update"),
					TEXT("asset.render_target.update"),
					TEXT("asset.runtime_virtual_texture.update"),
					TEXT("asset.skeletal_mesh.update"),
					TEXT("asset.sound_attenuation.update"),
					TEXT("asset.sound_class.update"),
					TEXT("asset.sound_concurrency.update"),
					TEXT("asset.sound_mix.update"),
					TEXT("asset.sound_submix.update"),
					TEXT("asset.sparse_volume_texture.update"),
					TEXT("asset.static_mesh.update"),
					TEXT("asset.subuv_animation.update"),
					TEXT("asset.texture_array.replace"),
					TEXT("asset.texture_collection.update"),
					TEXT("asset.texture_cube_array.replace"),
					TEXT("asset.texture_cube.replace"),
					TEXT("asset.volume_texture.replace"),
					TEXT("niagara.compound.apply"),
					TEXT("pcg.component.apply"),
					TEXT("pcg.component.world_binding.apply"),
					TEXT("pcg.data_asset.apply"),
					TEXT("pcg.graph_instance.apply"),
				},
				{
					TEXT("content_graph.export_workspace"),
					TEXT("content_graph.import_workspace"),
				},
				TEXT("Persistent asset desired state belongs to the common HJSON authoring surface."));

			AppendDormantPolicies(
				Result,
				{
					TEXT("animation.blend_space.definition.apply"),
					TEXT("animation.control_rig.shape_library.apply"),
					TEXT("animation.deformer_graph.topology.apply"),
					TEXT("animation.deformer_source_library.bindings.apply"),
					TEXT("animation.mesh_deformer_collection.entries.apply"),
					TEXT("animation.ml_deformer.configuration.apply"),
					TEXT("animation.montage.create"),
					TEXT("animation.montage.structure.replace"),
					TEXT("animation.sequence.bone_tracks.replace"),
					TEXT("animation.sequence.curves.update"),
					TEXT("animation.sequence.events.update"),
					TEXT("animation.sequence.metadata.update"),
					TEXT("physics.asset.structure.replace"),
					TEXT("physics.constraint.update"),
					TEXT("physics.material.update"),
					TEXT("rig.ik.definition.update"),
					TEXT("rig.retarget.definition.update"),
					TEXT("rig.retarget.pose.update"),
					TEXT("rig.skeleton.preview_mesh.bind"),
					TEXT("rig.skeleton.socket.update"),
					TEXT("timeline.sequence.structure.replace"),
				},
				{
					TEXT("content_graph.export_workspace"),
					TEXT("content_graph.import_workspace"),
				},
				TEXT("Persistent domain structure and properties belong to the common HJSON authoring surface."));

			AppendDormantPolicies(
				Result,
				{
					TEXT("asset.properties"),
					TEXT("asset.summary"),
					TEXT("animation.montage.read"),
					TEXT("animation.sequence.read"),
					TEXT("animation.skeleton.slots.read"),
					TEXT("timeline.sequence.read"),
					TEXT("timeline.widget_animation.read"),
				},
				{
					TEXT("content_graph.export_workspace"),
					TEXT("content_graph.summary"),
					TEXT("content_graph.structure"),
					TEXT("content_graph.items"),
					TEXT("content_graph.properties"),
				},
				TEXT("Persistent asset readback belongs to HJSON export and the common ContentGraph query family."));

			AppendDormantPolicies(
				Result,
				{
					TEXT("mesh.attribute.material_slot.add"),
					TEXT("mesh.attribute.material_slot.assign"),
					TEXT("mesh.pivot.update"),
					TEXT("modeling.actor.mesh.replace"),
					TEXT("modeling.asset.material_slot.remove"),
					TEXT("texture_graph.graph.link.mutate"),
					TEXT("texture_graph.graph.node.mutate"),
					TEXT("texture_graph.graph.parameter.mutate"),
					TEXT("world.actor_hlod_layer.update"),
					TEXT("world.actor_streaming_policy.update"),
					TEXT("world.data_layer_instance.create"),
					TEXT("world.data_layer_relation.update"),
					TEXT("world.data_layer_state.update"),
					TEXT("world.data_layer.delete"),
					TEXT("world.level_game_mode.update"),
					TEXT("world.level_streaming.move_actors"),
					TEXT("world.level_streaming.remove"),
					TEXT("world.streaming_relation.create"),
					TEXT("world.streaming_relation.state.update"),
					TEXT("world.world_partition_streaming.update"),
				},
				{
					TEXT("content_graph.export_workspace"),
					TEXT("content_graph.import_workspace"),
				},
				TEXT("Persistent asset, graph, and World desired state belongs to the common HJSON authoring surface."));

			AppendDormantPolicies(
				Result,
				{
					TEXT("editor_project.localization_target.update"),
					TEXT("editor_project.string_table.update"),
				},
				{
					TEXT("content_graph.export_workspace"),
					TEXT("content_graph.import_workspace"),
				},
				TEXT("Localization target and string table edits use their registered HJSON profiles."));

			return Result;
		}();
		return Policies;
	}

	const FCommandLifecyclePolicy* FindExact(const FString& CommandId)
	{
		return GetPolicies().FindByPredicate(
			[&CommandId](const FCommandLifecyclePolicy& Policy)
			{
				return Policy.CommandId == CommandId;
			});
	}

	bool TryApply(
		TArray<FUeAgentCommandDeclaration>& Declarations,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		TSet<FString> Applied;
		for (FUeAgentCommandDeclaration& Declaration : Declarations)
		{
			const FCommandLifecyclePolicy* Policy =
				FindExact(Declaration.CommandId);
			if (!Policy)
			{
				continue;
			}
			if (Applied.Contains(Policy->CommandId))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_lifecycle_policy_duplicate"), TEXT("The lifecycle policy was applied to more than one declaration of the same command."), TEXT("command_registration"));
				OutError.CandidateName = Policy->CommandId;
				OutError.PropertyPath = TEXT("command_id");
				return false;
			}
			if (Declaration.Availability
				!= EUeAgentCommandAvailability::Enabled)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_lifecycle_policy_source_not_enabled"), TEXT("A lifecycle policy can only be applied to an enabled command declaration."), TEXT("command_registration"));
				OutError.CandidateName = Policy->CommandId;
				OutError.PropertyPath = TEXT("availability");
				return false;
			}
			Declaration.Availability = Policy->Availability;
			Applied.Add(Policy->CommandId);
		}
		return true;
	}

	const TCHAR* LexToString(const EUeAgentCommandAvailability Availability)
	{
		switch (Availability)
		{
		case EUeAgentCommandAvailability::Enabled:
			return TEXT("active");
		case EUeAgentCommandAvailability::Dormant:
			return TEXT("dormant");
		case EUeAgentCommandAvailability::Disabled:
			return TEXT("disabled");
		default:
			return TEXT("invalid");
		}
	}

	bool IsExecutable(const EUeAgentCommandAvailability Availability)
	{
		return Availability == EUeAgentCommandAvailability::Enabled
			|| Availability == EUeAgentCommandAvailability::Dormant;
	}
}
