#include "Core/BindingGateway/Query/UeAgentQueryPublicProjection.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::BindingGateway::Query
{
	namespace
	{
		using FField = FQueryPublicFieldProjection;
		FField ScalarField(const TCHAR* Name, const bool bRequired = false)
		{
			return {Name, Name, bRequired};
		}
		FField ObjectField(const TCHAR* Name, TArray<FField> Children, const bool bRequired = false)
		{
			return {Name, Name, bRequired, EUeAgentQueryPublicIdentityField::Selector, MoveTemp(Children), false};
		}
		FField NullableField(FField Field)
		{
			Field.bNullable = true;
			return Field;
		}
		FField ArrayField(const TCHAR* Name, TArray<FField> Children = {}, const bool bRequired = false)
		{
			return {Name, Name, bRequired, EUeAgentQueryPublicIdentityField::Selector, MoveTemp(Children), true};
		}
		FField ScalarMapField(const TCHAR* Name)
		{
			FField Field = ObjectField(Name, {});
			Field.bScalarMap = true;
			return Field;
		}
		FField AuthoringValueField(const TCHAR* Name)
		{
			FField Field = ScalarField(Name);
			Field.bAuthoringValue = true;
			return Field;
		}

		FField InPublicContainer(const TCHAR* Name, FField Field)
		{
			Field.PublicContainerPath.Add(Name);
			return Field;
		}
		FField VectorField(const TCHAR* Name)
		{
			return ObjectField(Name, {ScalarField(TEXT("x"), true), ScalarField(TEXT("y"), true), ScalarField(TEXT("z"), true)});
		}

		FQueryPublicFieldProjection SpatialValueField(
			const TCHAR* Selector, const TCHAR* PublicName, const bool bRequired,
			const EUeAgentQueryPublicIdentityField IdentityField = EUeAgentQueryPublicIdentityField::ObjectRole)
		{
			using Field = FQueryPublicFieldProjection;
			return {Selector, PublicName, bRequired,
				IdentityField, {
					Field{TEXT("quantity"), TEXT("quantity"), false},
					Field{TEXT("frame_id"), TEXT("frame_id"), true},
					Field{TEXT("dimension"), TEXT("dimension"), false},
					Field{TEXT("unit"), TEXT("unit"), true},
					Field{TEXT("components"), TEXT("components"), true,
						EUeAgentQueryPublicIdentityField::Selector, {}, true}
				}, false};
		}

		FField SpatialField(const TCHAR* Selector, const TCHAR* PublicName, const bool bRequired = false)
		{
			return SpatialValueField(Selector, PublicName, bRequired, EUeAgentQueryPublicIdentityField::Selector);
		}
		FField SpatialArrayField(const TCHAR* Name)
		{
			return ArrayField(Name, SpatialField(Name, Name).Children);
		}
		FField AssetCollectionField()
		{
			FField Fields = ObjectField(TEXT("fields"), {});
			Fields.bScalarMap = true;
			const TArray<FField> ItemFields = {ScalarField(TEXT("field"), true),
				ScalarField(TEXT("index")), ScalarField(TEXT("key")), ScalarField(TEXT("value"), true)};
			return ObjectField(TEXT("collection"), {ScalarField(TEXT("kind"), true), ScalarField(TEXT("count"), true),
				ArrayField(TEXT("fields"), {ScalarField(TEXT("field"), true), ScalarField(TEXT("count"), true)}),
				ArrayField(TEXT("rows"), {ScalarField(TEXT("key"), true), MoveTemp(Fields)}),
				ArrayField(TEXT("items"), ItemFields), ArrayField(TEXT("entries"), ItemFields)}, true);
		}
		FField AssetSampleField()
		{
			return ObjectField(TEXT("sample"), {ScalarField(TEXT("kind"), true), ScalarField(TEXT("index")),
				VectorField(TEXT("origin_cm")), VectorField(TEXT("extent_cm")),
				ScalarField(TEXT("sphere_radius_cm")), VectorField(TEXT("position_cm")),
				ScalarField(TEXT("section_count")), ScalarField(TEXT("material_slot_count")),
				ScalarField(TEXT("material_index")), ScalarField(TEXT("material_index_valid")),
				ScalarField(TEXT("material_path")), ScalarField(TEXT("triangle_count")),
				ScalarField(TEXT("section_info_material_index")), ScalarField(TEXT("original_section_info_material_index"))}, true);
		}
		FField AssetRelationsField()
		{
			FField Field = ObjectField(TEXT("relation_graph"), {
				ScalarField(TEXT("root"), true),
				ObjectField(TEXT("coverage"), {ScalarField(TEXT("complete"), true), ScalarField(TEXT("registry_loading"), true),
					ScalarField(TEXT("scope"), true), ScalarField(TEXT("runtime_references_included"), true)}, true),
				ArrayField(TEXT("nodes"), {ScalarField(TEXT("ref"), true), ScalarField(TEXT("package_name")),
					ScalarField(TEXT("object_path")), ScalarField(TEXT("resolved"), true),
					ScalarField(TEXT("redirector")), ScalarField(TEXT("redirect_target"))}, true),
				ArrayField(TEXT("edges"), {ScalarField(TEXT("source"), true), ScalarField(TEXT("target"), true),
					ScalarField(TEXT("direction"), true), ScalarField(TEXT("depth"), true), ScalarField(TEXT("relation"), true),
					ArrayField(TEXT("properties"), {}, true), ArrayField(TEXT("path")), ScalarField(TEXT("cycle"))}, true)}, true);
			Field.PublicName = TEXT("graph");
			return Field;
		}
		FField LandscapeHeightField()
		{
			FField Field = ObjectField(TEXT("height_state"), {
				ObjectField(TEXT("extent"), {ScalarField(TEXT("frame_id"), true),
					ArrayField(TEXT("min_inclusive"), {}, true), ArrayField(TEXT("max_inclusive"), {}, true)}, true),
				VectorField(TEXT("scale_cm_per_local_unit")), ScalarField(TEXT("height_encoding"), true),
				ScalarField(TEXT("height_order"), true), ArrayField(TEXT("heights"), {}, true)}, true);
			Field.PublicName = TEXT("height_grid");
			return Field;
		}

		FQueryPublicFieldProjection EditorProjectObservation()
		{
			FQueryPublicFieldProjection Field{TEXT("observation_json"), TEXT("observation"), true};
			Field.bStructuredValue = true;
			return Field;
		}

		FQueryPublicFieldProjection SceneAnalysisField()
		{
			FQueryPublicFieldProjection Field = ScalarField(TEXT("analysis"), true);
			Field.bStructuredValue = true;
			return Field;
		}

		TArray<FQueryPublicFieldProjection> RigBakeContextFields()
		{
			const auto Rate = [](const TCHAR* Name) {
				return ObjectField(Name, {ScalarField(TEXT("numerator"), true), ScalarField(TEXT("denominator"), true)}, true);
			};
			const auto Input = [&](const TCHAR* Name, const bool bAnimation) {
				TArray<FField> Fields = {ScalarField(TEXT("binding_ref"), true), ScalarField(TEXT("rig_ref"), true),
					ObjectField(TEXT("authored_range"), {ScalarField(TEXT("clock_id"), true),
						ScalarField(TEXT("lower_tick"), true), ScalarField(TEXT("upper_tick"), true),
						Rate(TEXT("rate")), ScalarField(TEXT("state"), true)}, true), Rate(TEXT("sample_rate")),
					ObjectField(TEXT("expected_revision"), {ScalarField(TEXT("source_id"), true),
						ScalarField(TEXT("generation"), true), ScalarField(TEXT("sequence"), true)}, true)};
				Fields.Add(ScalarField(bAnimation ? TEXT("source_sequence_ref") : TEXT("target_sequence_ref"), true));
				if (bAnimation) Fields.Add(ScalarField(TEXT("target_animation_ref"), true));
				return ObjectField(Name, MoveTemp(Fields), !bAnimation);
			};
			return {ScalarField(TEXT("sequence_ref"), true), ScalarField(TEXT("binding_name"), true),
				Input(TEXT("controls_input"), false), Input(TEXT("animation_input"), true)};
		}

		TArray<FQueryPublicFieldProjection> TimelineSequenceFields()
		{
			const auto Scalar = [](const TCHAR* Selector, const TCHAR* PublicName,
				const bool bRequired = false)
			{
				return FQueryPublicFieldProjection{
					Selector, PublicName, bRequired,
					EUeAgentQueryPublicIdentityField::Selector, {}, false};
			};
			const auto Object = [](const TCHAR* Selector, const TCHAR* PublicName,
				TArray<FQueryPublicFieldProjection> Children,
				const bool bRequired = false)
			{
				return FQueryPublicFieldProjection{
					Selector, PublicName, bRequired,
					EUeAgentQueryPublicIdentityField::Selector,
					MoveTemp(Children), false};
			};
			const auto Array = [](const TCHAR* Selector, const TCHAR* PublicName,
				TArray<FQueryPublicFieldProjection> ItemChildren = {})
			{
				return FQueryPublicFieldProjection{
					Selector, PublicName, false,
					EUeAgentQueryPublicIdentityField::Selector,
					MoveTemp(ItemChildren), true};
			};
			return {
				Object(TEXT("sequence"), TEXT("sequence"), {
					Scalar(TEXT("asset_ref"), TEXT("asset_ref"), true),
					Scalar(TEXT("asset_kind"), TEXT("asset_kind"), true),
				}, true),
				Object(TEXT("structure"), TEXT("structure"), {
					Object(TEXT("display_rate"), TEXT("display_rate"), {
						Scalar(TEXT("numerator"), TEXT("numerator"), true),
						Scalar(TEXT("denominator"), TEXT("denominator"), true),
					}, true),
					Object(TEXT("tick_resolution"), TEXT("tick_resolution"), {
						Scalar(TEXT("numerator"), TEXT("numerator"), true),
						Scalar(TEXT("denominator"), TEXT("denominator"), true),
					}, true),
					Object(TEXT("playback_range"), TEXT("playback_range"), {
						Scalar(TEXT("lower"), TEXT("lower"), true),
						Scalar(TEXT("upper"), TEXT("upper"), true),
					}, true),
					Scalar(TEXT("binding_count"), TEXT("binding_count"), true),
					Scalar(TEXT("root_track_count"), TEXT("root_track_count"), true),
					Array(TEXT("bindings"), TEXT("bindings"), {
						Scalar(TEXT("binding_ref"), TEXT("binding_ref"), true),
						Scalar(TEXT("name"), TEXT("name"), true),
						Scalar(TEXT("track_count"), TEXT("track_count"), true),
						Array(TEXT("tracks"), TEXT("tracks"), {
							Scalar(TEXT("track_ref"), TEXT("track_ref"), true),
							Scalar(TEXT("track_type"), TEXT("track_type"), true),
							Scalar(TEXT("section_count"), TEXT("section_count"), true),
							Array(TEXT("sections"), TEXT("sections"), {
								Scalar(TEXT("section_ref"), TEXT("section_ref"), true),
								Scalar(TEXT("section_type"), TEXT("section_type"), true),
								Scalar(TEXT("key_count"), TEXT("key_count")),
								Array(TEXT("keys"), TEXT("keys")),
							}),
						}),
					}),
					Array(TEXT("root_tracks"), TEXT("root_tracks"), {
						Scalar(TEXT("track_ref"), TEXT("track_ref"), true),
						Scalar(TEXT("track_type"), TEXT("track_type"), true),
						Scalar(TEXT("section_count"), TEXT("section_count"), true),
						Array(TEXT("sections"), TEXT("sections"), {
							Scalar(TEXT("section_ref"), TEXT("section_ref"), true),
							Scalar(TEXT("section_type"), TEXT("section_type"), true),
							Scalar(TEXT("key_count"), TEXT("key_count")),
							Array(TEXT("keys"), TEXT("keys")),
						}),
					}),
				}, true),
			};
		}

		const TArray<FQueryPublicProjectionDescriptor> Descriptors = {
			{
				TEXT("rig_bake_context"), EUeAgentQueryPublicProjectionStrategy::SingleRecord, TEXT("record"),
				{{TEXT("bake_context"), TEXT("bake_context"), true,
					EUeAgentQueryPublicIdentityField::ObjectRole, RigBakeContextFields(), false}},
				{{TEXT("bake_context"), TEXT("object"), true, TEXT("Ready inputs for the existing Control Rig bake operations.")}},
				TEXT("Use the public SDK result.rig_bake_context handle to bake_controls or bake_animation; refresh the context after editing or restarting Editor."),
				false, {}, FString(), {}, FString(), {TEXT("rig.control.bake.context")},
			},
			{
				TEXT("graph_view_state"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					SpatialValueField(TEXT("graph_view.location"), TEXT("location"), true),
					{TEXT("graph_view.zoom"), TEXT("zoom"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
					SpatialValueField(TEXT("graph_view.document.location"), TEXT("document_location"), false),
					{TEXT("graph_view.document.zoom"), TEXT("document_zoom"), false, EUeAgentQueryPublicIdentityField::ObjectRole},
				},
				{
					{TEXT("location"), TEXT("object"), true, TEXT("Current graph-local view location.")},
					{TEXT("zoom"), TEXT("number"), true, TEXT("Current graph view zoom.")},
					{TEXT("document_location"), TEXT("object"), false, TEXT("Saved graph-local view location when explicitly requested and available.")},
					{TEXT("document_zoom"), TEXT("number"), false, TEXT("Saved graph view zoom when explicitly requested and available.")},
				},
				TEXT("Use location and zoom for the current graph view; saved document state is present only when requested, while revision identity remains internal."),
				false,
				{},
				FString(),
				{},
				FString(),
				{TEXT("graph.view.read")},
			},
			{
				TEXT("viewport_ray"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					SpatialValueField(TEXT("viewport_ray.origin"), TEXT("origin"), true),
					SpatialValueField(TEXT("viewport_ray.direction"), TEXT("direction"), true),
				},
				{
					{TEXT("origin"), TEXT("object"), true, TEXT("Ray origin in ue.world centimeters.")},
					{TEXT("direction"), TEXT("object"), true, TEXT("Unit ray direction in ue.world.")},
				},
				TEXT("Use origin and direction directly; viewport, request and evidence identity remain internal."),
				false,
				{},
				FString(),
				{},
				FString(),
				{TEXT("viewport.ray.deproject")},
			},
			{
				TEXT("viewport_ray_hit"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					SpatialValueField(TEXT("viewport_ray.origin"), TEXT("origin"), true),
					SpatialValueField(TEXT("viewport_ray.direction"), TEXT("direction"), true),
					{TEXT("viewport_hit.blocking"), TEXT("hit"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
					{TEXT("viewport_hit.actor_ref"), TEXT("actor_ref"), false, EUeAgentQueryPublicIdentityField::ObjectRole},
					SpatialValueField(TEXT("viewport_hit.location"), TEXT("location"), false),
					SpatialValueField(TEXT("viewport_hit.normal"), TEXT("normal"), false),
					{TEXT("viewport_hit.distance"), TEXT("distance"), false, EUeAgentQueryPublicIdentityField::ObjectRole},
				},
				{
					{TEXT("origin"), TEXT("object"), true, TEXT("Ray origin in ue.world centimeters.")},
					{TEXT("direction"), TEXT("object"), true, TEXT("Unit ray direction in ue.world.")},
					{TEXT("hit"), TEXT("boolean"), true, TEXT("Whether the ray produced a blocking hit.")},
					{TEXT("actor_ref"), TEXT("string"), false, TEXT("Exact readable actor reference when a blocking actor was hit.")},
					{TEXT("location"), TEXT("object"), false, TEXT("Hit location in ue.world centimeters.")},
					{TEXT("normal"), TEXT("object"), false, TEXT("Hit surface normal in ue.world.")},
					{TEXT("distance"), TEXT("number"), false, TEXT("Distance from ray origin to the hit in centimeters.")},
				},
				TEXT("Use hit first and actor_ref when present; ray and hit geometry remain available without internal trace evidence."),
				false,
				{},
				FString(),
				{},
				FString(),
				{TEXT("viewport.ray.trace")},
			},
			{
				TEXT("viewport_actor_pick"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("viewport_hit.blocking"), TEXT("hit"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
					{TEXT("viewport_hit.actor_ref"), TEXT("actor_ref"), false, EUeAgentQueryPublicIdentityField::ObjectRole},
				},
				{
					{TEXT("hit"), TEXT("boolean"), true, TEXT("Whether the screen point resolved a blocking actor hit.")},
					{TEXT("actor_ref"), TEXT("string"), false, TEXT("Exact readable actor reference when present.")},
				},
				TEXT("Use actor_ref when present; ray geometry and internal trace evidence are not repeated for actor picking."),
				false,
				{},
				FString(),
				{},
				FString(),
				{TEXT("viewport.actor.pick")},
			},
			{
				TEXT("viewport_pixel_scale"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					SpatialValueField(TEXT("viewport_pixel_scale.sample_point"), TEXT("sample_point"), true),
					{TEXT("viewport_pixel_scale.world_units_per_pixel"), TEXT("world_units_per_pixel"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
				},
				{
					{TEXT("sample_point"), TEXT("object"), true, TEXT("Sample point in ue.world centimeters.")},
					{TEXT("world_units_per_pixel"), TEXT("number"), true, TEXT("World centimeters represented by one viewport pixel at the sample point.")},
				},
				TEXT("Use the sample point and scale directly; camera and evidence identities remain internal."),
				false,
				{},
				FString(),
				{},
				FString(),
				{TEXT("viewport.pixel_scale.read")},
			},
			{
				TEXT("viewport_state"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					InPublicContainer(TEXT("camera"), SpatialValueField(TEXT("viewport_state.camera_transform"), TEXT("camera_transform"), true)),
					InPublicContainer(TEXT("camera"), {TEXT("viewport_state.fov"), TEXT("fov_degrees"), true, EUeAgentQueryPublicIdentityField::ObjectRole}),
					InPublicContainer(TEXT("camera"), {TEXT("viewport_state.view_mode"), TEXT("view_mode"), true, EUeAgentQueryPublicIdentityField::ObjectRole}),
					{TEXT("viewport_state.realtime"), TEXT("realtime"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
					{TEXT("viewport_state.realtime_configured"), TEXT("realtime_configured"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
					{TEXT("viewport_state.realtime_override_reason"), TEXT("realtime_override_reason"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
					{TEXT("viewport_state.game_view"), TEXT("game_view"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
					{TEXT("viewport_state.show_flags"), TEXT("show_flags"), true, EUeAgentQueryPublicIdentityField::ObjectRole, {}, true},
					InPublicContainer(TEXT("interaction"), {TEXT("viewport_state.interaction.mode_widgets"), TEXT("mode_widgets"), true, EUeAgentQueryPublicIdentityField::ObjectRole}),
					InPublicContainer(TEXT("interaction"), {TEXT("viewport_state.interaction.selection"), TEXT("selection"), true, EUeAgentQueryPublicIdentityField::ObjectRole}),
					InPublicContainer(TEXT("interaction"), {TEXT("viewport_state.interaction.pivot"), TEXT("pivot"), true, EUeAgentQueryPublicIdentityField::ObjectRole}),
					InPublicContainer(TEXT("interaction"), {TEXT("viewport_state.interaction.editor_primitives"), TEXT("editor_primitives"), true, EUeAgentQueryPublicIdentityField::ObjectRole}),
					InPublicContainer(TEXT("interaction"), {TEXT("viewport_state.interaction.game_primitives"), TEXT("game_primitives"), true, EUeAgentQueryPublicIdentityField::ObjectRole}),
					InPublicContainer(TEXT("interaction"), {TEXT("viewport_state.interaction.widget_mode"), TEXT("widget_mode"), true, EUeAgentQueryPublicIdentityField::ObjectRole}),
				},
				{
					{TEXT("viewport_ref"), TEXT("string"), true, TEXT("Readable identity of the observed viewport.")},
					{TEXT("camera"), TEXT("object"), true, TEXT("Camera transform, fov_degrees and view_mode, ready for viewport state restoration.")},
					{TEXT("realtime"), TEXT("boolean"), true, TEXT("Sustained effective realtime state, excluding temporary redraw frames.")},
					{TEXT("realtime_configured"), TEXT("boolean"), true, TEXT("Configured realtime preference before Editor overrides.")},
					{TEXT("realtime_override_reason"), TEXT("string"), true, TEXT("Active Editor override reason, or empty when none.")},
					{TEXT("game_view"), TEXT("boolean"), true, TEXT("Current game-view state.")},
					{TEXT("show_flags"), TEXT("array"), true, TEXT("Curated viewport show flags.")},
					{TEXT("interaction"), TEXT("object"), true, TEXT("Mode widgets, selection, pivot, editor/game primitives and widget mode.")},
				},
				TEXT("Pass this complete public payload directly as viewport.state_restore input.snapshot for the same viewport; machine revision and observation identities remain internal."),
				false,
				{},
				TEXT("viewport_ref"),
				{},
				FString(),
				{TEXT("viewport.state.read")},
			},
			{
				TEXT("viewport_warnings"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{{TEXT("viewport_warnings"), TEXT("warnings"), true, EUeAgentQueryPublicIdentityField::ObjectRole,
					{ScalarField(TEXT("code"), true), ScalarField(TEXT("message"), true), ScalarField(TEXT("severity"), true)}, true}},
				{
					{TEXT("warnings"), TEXT("array"), true, TEXT("Viewport warnings with code, message and severity only.")},
				},
				TEXT("Use warning code, message and severity; renderer probes, gates and internal evidence remain internal."),
				false,
				{},
				FString(),
				{TEXT("warnings")},
				FString(),
				{TEXT("viewport.warning.list")},
			},
			{
				TEXT("viewport_view_modes"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("viewport_modes.catalog"), TEXT("modes"), true, EUeAgentQueryPublicIdentityField::ObjectRole,
						{ScalarField(TEXT("mode_id"), true), ScalarField(TEXT("label"), true)}, true},
					{TEXT("viewport_modes.active"), TEXT("active_mode_id"), true, EUeAgentQueryPublicIdentityField::ObjectRole},
				},
				{
					{TEXT("modes"), TEXT("array"), true, TEXT("Available viewport mode ids and readable labels.")},
					{TEXT("active_mode_id"), TEXT("string"), true, TEXT("Current viewport mode id.")},
				},
				TEXT("Use mode_id for viewport updates and label for readability; internal catalog identity is not public."),
				false,
				{},
				FString(),
				{TEXT("modes")},
				FString(),
				{TEXT("viewport.view_mode.list")},
			},
			{
				TEXT("viewport_instance_records"),
				EUeAgentQueryPublicProjectionStrategy::SnapshotRecordArray,
				TEXT("instances"),
				{
					{TEXT("viewport_ref"), TEXT("viewport_ref"), true},
					{TEXT("viewport_type"), TEXT("viewport_type"), true},
				},
				{
					{TEXT("instances"), TEXT("array"), true, TEXT("Viewport instances with readable reference and type only.")},
				},
				TEXT("Use viewport_ref and viewport_type; camera, snapshot and owner identities are internal and are not public."),
				false,
				{},
				FString(),
				{},
				TEXT("viewport_instances.catalog"),
				{TEXT("viewport.instance.list")},
				EUeAgentQueryPublicIdentityField::ObjectRole,
			},
			{
				TEXT("timeline_sequence_read"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("timeline_sequence.sequence"), TEXT("sequence"), true, EUeAgentQueryPublicIdentityField::ObjectRole, TimelineSequenceFields()[0].Children, false},
					{TEXT("timeline_sequence.structure"), TEXT("structure"), true, EUeAgentQueryPublicIdentityField::ObjectRole, TimelineSequenceFields()[1].Children, false},
				},
				{
					{TEXT("sequence"), TEXT("object"), true, TEXT("Readable sequence asset reference and kind.")},
					{TEXT("structure"), TEXT("object"), true, TEXT("Display rate, tick resolution, playback range and explicitly requested structure.")},
				},
				TEXT("Use the readable sequence summary and requested structure; object paths, signatures, revisions and native observation text remain internal."),
				false,
				{},
				FString(),
				{},
				FString(),
				{TEXT("timeline.sequence.read")},
			},
			{
				TEXT("legacy_typed_envelope"),
				EUeAgentQueryPublicProjectionStrategy::LegacyTypedEnvelope,
				FString(),
				{},
				{
					{TEXT("command_id"), TEXT("string"), true, TEXT("Exact public command identity.")},
					{TEXT("mechanics_command_id"), TEXT("string"), true, TEXT("Exact server-owned mechanics route used for the read.")},
					{TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true for a successful Query.")},
					{TEXT("query_result"), TEXT("object"), true, TEXT("Typed projection with source identity, state revision and completeness.")},
				},
				TEXT("Use command_id, mechanics_command_id and state revision to interpret the result."),
			},
			{
				TEXT("content_graph_entries"),
				EUeAgentQueryPublicProjectionStrategy::SnapshotEntries,
				FString(),
				{},
				{
					{TEXT("entries"), TEXT("array"), true, TEXT("Direct business values for the selected content graph facet.")},
				},
				TEXT("Use the complete direct business entries."),
			},
			{
				TEXT("content_graph_sections"),
				EUeAgentQueryPublicProjectionStrategy::SnapshotSections,
				FString(),
				{},
				{
					{TEXT("sections"), TEXT("object"), true, TEXT("Business entries grouped once by content graph section.")},
				},
				TEXT("Use the complete section-grouped business entries."),
			},
			{
				TEXT("dirty_resources"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("resources"),
				{
					{TEXT("resource_ref"), TEXT("resource_ref"), true},
					{TEXT("resource_kind"), TEXT("resource_kind"), true},
				},
				{
					{TEXT("resources"), TEXT("array"), true, TEXT("Dirty resources grouped by canonical resource reference and resource kind.")},
				},
				TEXT("Use each resource_ref unchanged with its resource_kind for the next save or discard decision; no machine identity is public."),
			},
			{
				TEXT("editor_resources"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("resources"),
				{
					{TEXT("resource_ref"), TEXT("resource_ref"), true},
					{TEXT("resource_kind"), TEXT("resource_kind"), true},
					{TEXT("resource_dirty"), TEXT("dirty"), false},
				},
				{
					{TEXT("resources"), TEXT("array"), true, TEXT("Open Editor resources grouped by exact resource reference, kind and dirty state.")},
				},
				TEXT("Use each resource_ref unchanged for the next exact asset action; dirty is included only as current business state and Editor machine identity is never public."),
			},
			{
				TEXT("object_summaries"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("objects"),
				{
					{TEXT("path"), TEXT("path"), true},
					{TEXT("class_path"), TEXT("class_path"), true},
					{TEXT("package_name"), TEXT("package_name"), true},
					{TEXT("package_dirty"), TEXT("package_dirty"), true},
				},
				{
					{TEXT("objects"), TEXT("array"), true, TEXT("Queried objects grouped by exact UE path with class and package state.")},
				},
				TEXT("Use path unchanged as the exact UE target; class and package fields are direct business state, while snapshot and owner identity remain internal."),
			},
			{
				TEXT("owned_candidate_names"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("candidates"),
				{{TEXT("names"), TEXT("names"), true, EUeAgentQueryPublicIdentityField::Selector, {}, true}},
				{{TEXT("names"), TEXT("array"), true, TEXT("All matching native candidate names for this owned property, grouped by display name.")}},
				TEXT("Use a returned name unchanged in a new owned item. Refine query when more specific names are needed; select is only required after a genuine ambiguity."),
			},
			{
				TEXT("asset_collection"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("collections"),
				{
					AssetCollectionField(),
				},
				{
					{TEXT("collections"), TEXT("array"), true, TEXT("Normalized rows, items or map entries without snapshot or owner identity.")},
				},
				TEXT("The requested collection kind returns names or keys plus only the values explicitly requested by the input. Reuse returned field, key and index values directly; target and request controls are not repeated."),
				true,
				{},
				FString(),
				{
					TEXT("fields"),
					TEXT("items"),
					TEXT("entries"),
					TEXT("rows"),
				},
			},
			{
				TEXT("asset_sample"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("samples"),
				{
					AssetSampleField(),
				},
				{
					{TEXT("samples"), TEXT("array"), true, TEXT("Normalized mesh bounds, vertex or LOD 0 section observations.")},
				},
				TEXT("Bounds return origin_cm, extent_cm and sphere_radius_cm; vertex returns index and position_cm. Section defaults to index 0 and returns LOD 0 render material assignment, slot/section counts, triangle count and current/original section-map indices; an invalid material index remains observable. The target and request envelope are not repeated."),
				true,
			},
			{
				TEXT("asset_properties"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("properties"),
				{
					{TEXT("property_count"), TEXT("property_count"), false},
				},
				{
					{TEXT("properties"), TEXT("array"), true, TEXT("The default property count or explicitly requested asset fields without repeating the target.")},
				},
				TEXT("Use property_count only as a discovery summary; request field_ids only when the next action needs specific values. Requested property names and values appear under fields, while the target and internal owner remain implicit."),
				false,
				{
					TEXT("asset.properties:value:"),
					TEXT("asset"),
					TEXT("fields"),
				},
			},
			{
				TEXT("asset_relations"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("relations"),
				{
					AssetRelationsField(),
				},
				{
					{TEXT("relations"), TEXT("array"), true, TEXT("Normalized asset relation graphs using exact UE object or package references.")},
				},
				TEXT("Each graph preserves relation direction, depth, properties, traversal path and coverage while omitting request echo, aliases and internal identities. Reuse root, node refs and edge refs unchanged as exact UE targets."),
				true,
			},
			{
				TEXT("project_settings"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("settings"),
				{
					{TEXT("property_count"), TEXT("property_count"), false},
					ScalarMapField(TEXT("defaults")),
				},
				{
					{TEXT("settings"), TEXT("array"), true, TEXT("The default settings count or explicitly requested project setting fields without repeating the section input.")},
				},
				TEXT("Use property_count only as a discovery summary; request field_ids only when the next action needs specific settings. Requested names and values appear under fields, while the section input and internal owner remain implicit."),
				false,
				{
					TEXT("project.settings:value:"),
					TEXT("project"),
					TEXT("fields"),
				},
			},
			{
				TEXT("editor_project_observation"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{EditorProjectObservation()},
				{{TEXT("observation"), TEXT("object"), true,
					TEXT("The requested EditorProject capability observation; native business keys and values retain their declared shape.")}},
				TEXT("Explicit editor_project_capability requests return observation. Ordinary queries keep their existing compact projection."),
			},
			{
				TEXT("config_section"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("sections"),
				{
					ArrayField(TEXT("keys")),
				},
				{
					{TEXT("sections"), TEXT("array"), true, TEXT("Section key names by default, or explicitly requested saved values under fields.")},
				},
				TEXT("The default result is names-only. Pass keys only when the next action needs saved values; repeated config keys remain arrays under fields, while request scope and internal owner are not repeated."),
				false,
				{
					TEXT("config.value:"),
					TEXT("config"),
					TEXT("fields"),
				},
				FString(),
				{TEXT("keys")},
			},
			{
				TEXT("config_resolved_value"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("values"),
				{},
				{
					{TEXT("values"), TEXT("array"), true, TEXT("The explicitly requested resolved config key and value under fields.")},
				},
				TEXT("The required key is returned only under fields with its resolved value; request scope, section and internal owner are not repeated."),
				false,
				{
					TEXT("config.value:"),
					TEXT("config"),
					TEXT("fields"),
				},
			},
			{
				TEXT("asset_registry_records"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("assets"),
				{
					{TEXT("object_path"), TEXT("object_path"), true},
					{TEXT("class_path"), TEXT("class_path"), true},
					{TEXT("package_name"), TEXT("package_name"), true},
					ScalarMapField(TEXT("tags")),
					ArrayField(TEXT("chunk_ids")),
				},
				{
					{TEXT("assets"), TEXT("array"), true, TEXT("Asset Registry identity with native tags and integer chunk_ids only when requested.")},
				},
				TEXT("Use object_path unchanged as the exact asset target. Optional tags preserve native strings; chunk_ids may be empty. Internal registry identity and snapshot state are not public."),
			},
			{
				TEXT("validation_results"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("results"),
				{
					{TEXT("path"), TEXT("path"), false},
					{TEXT("is_valid"), TEXT("is_valid"), false},
					{TEXT("validation_rule_id"), TEXT("rule"), false},
					{TEXT("validation_rule_passed"), TEXT("passed"), false},
				},
				{
					{TEXT("results"), TEXT("array"), true, TEXT("Validation target, overall result and returned rule outcomes without internal rule owner identities.")},
				},
				TEXT("Use is_valid for the overall decision and each rule/passed pair for actionable validation detail; internal grouping identities are not public."),
			},
			{
				TEXT("platform_observations"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("platforms"),
				{
					{TEXT("platform_name"), TEXT("platform_name"), true},
					{TEXT("ini_platform_name"), TEXT("ini_platform_name"), false},
					{TEXT("display_name"), TEXT("display_name"), false},
					{TEXT("running_platform"), TEXT("running"), false},
					{TEXT("sdk_installed"), TEXT("sdk_installed"), false},
					{TEXT("sdk_documentation_path"), TEXT("sdk_documentation_path"), false},
				},
				{
					{TEXT("platforms"), TEXT("array"), true, TEXT("Requested platform observations grouped by stable platform name.")},
				},
				TEXT("The default result is names-only; request field_ids only when the next action needs specific platform observations. Request control fields and internal target identity are not public."),
				true,
			},
			{
				TEXT("origin_resolution"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("origins"),
				{
					{TEXT("resolved"), TEXT("resolved"), true},
					{TEXT("status"), TEXT("status"), true},
					{TEXT("target.kind"), TEXT("kind"), false},
					{TEXT("target.name"), TEXT("name"), false},
					{TEXT("target.full_name"), TEXT("full_name"), false},
					{TEXT("target.asset_path"), TEXT("asset_path"), false},
					{TEXT("target.graph_name"), TEXT("graph_name"), false},
					{TEXT("target.json_file"), TEXT("json_file"), false},
					{TEXT("target.json_pointer"), TEXT("json_pointer"), false},
					{TEXT("origin.asset_paths"), TEXT("asset_paths"), false, EUeAgentQueryPublicIdentityField::Selector, {}, true},
					{TEXT("origin.package_paths"), TEXT("package_paths"), false, EUeAgentQueryPublicIdentityField::Selector, {}, true},
					{TEXT("origin.package_files"), TEXT("package_files"), false, EUeAgentQueryPublicIdentityField::Selector, {}, true},
					{TEXT("origin.code_paths"), TEXT("code_paths"), false, EUeAgentQueryPublicIdentityField::Selector,
						{ScalarField(TEXT("path"), true), ScalarField(TEXT("line")), ScalarField(TEXT("column")),
							ScalarField(TEXT("symbol")), ScalarField(TEXT("module")), ScalarField(TEXT("status"))}, true},
					ArrayField(TEXT("warnings")),
				},
				{
					{TEXT("origins"), TEXT("array"), true, TEXT("Resolved business origins and usable source locations without candidate or node machine identity.")},
				},
				TEXT("Use resolved/status first, then exact asset, package, file or code paths for the next action; candidate identity and node machine identity are not public."),
			},
			{
				TEXT("world_summary"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("worlds"),
				{
					{TEXT("package_dirty"), TEXT("package_dirty"), true},
				},
				{
					{TEXT("worlds"), TEXT("array"), true, TEXT("Current worlds by exact object path and package dirty state.")},
				},
				TEXT("Use object_path unchanged for subsequent world reads; snapshot and machine identities are not public."),
				false,
				{},
				TEXT("object_path"),
			},
			{
				TEXT("world_selection"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("selected_objects"),
				{
					{TEXT("selected_identity"), TEXT("object_path"), true},
				},
				{
					{TEXT("selected_objects"), TEXT("array"), true, TEXT("Exact object paths selected in the current Editor context.")},
				},
				TEXT("Use each selected object path unchanged; selection context and internal owner identities are not repeated."),
				true,
			},
			{
				TEXT("world_ray_trace"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("hit"), TEXT("hit"), true},
					{TEXT("target_ref"), TEXT("target_ref"), false},
					SpatialField(TEXT("impact_point"), TEXT("impact_point")),
				},
				{
					{TEXT("hit"), TEXT("boolean"), true, TEXT("Whether the ray hit a target.")},
					{TEXT("target_ref"), TEXT("string"), false, TEXT("Exact hit component reference, present only on a hit.")},
					{TEXT("impact_point"), TEXT("object"), false, TEXT("Hit point in ue.world centimeters, present only on a hit.")},
				},
				TEXT("Use hit first. target_ref and impact_point are present only on a hit; the point is in ue.world centimeters and internal trace state is not public."),
			},
			{
				TEXT("world_overlap"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("overlap_count"), TEXT("count"), true},
					ArrayField(TEXT("targets")),
				},
				{
					{TEXT("count"), TEXT("integer"), true, TEXT("Number of unique overlapping components.")},
					{TEXT("targets"), TEXT("array"), false, TEXT("Sorted exact component references only when explicitly requested.")},
				},
				TEXT("Use count by default. Request targets only when the next action needs exact component references; request shape and internal state are not repeated."),
				false,
				{},
				FString(),
				{TEXT("targets")},
			},
			{
				TEXT("world_capsule_sweep"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("hit_count"), TEXT("hit_count"), true},
					ArrayField(TEXT("hits"), {ScalarField(TEXT("target_ref"), true), SpatialField(TEXT("impact_point"), TEXT("impact_point"), true)}),
				},
				{
					{TEXT("hit_count"), TEXT("integer"), true, TEXT("Number of capsule sweep hits.")},
					{TEXT("hits"), TEXT("array"), false, TEXT("Ordered hit targets and impact points only when explicitly requested.")},
				},
				TEXT("Use hit_count by default. Request hits only when the next action needs exact target and impact points; request geometry and internal trace state are not repeated."),
				false,
				{},
				FString(),
				{TEXT("hits")},
			},
			{
				TEXT("world_capsule_path"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("blocked_segments"), TEXT("blocked_segments"), true},
					NullableField(ObjectField(TEXT("first_blocking_hit"), {
						ScalarField(TEXT("segment_index"), true),
						NullableField(ScalarField(TEXT("target_ref"), true)),
						SpatialField(TEXT("shape_location"), TEXT("shape_location"), true),
						SpatialField(TEXT("impact_point"), TEXT("impact_point"), true),
						ScalarField(TEXT("start_penetrating"), true)})),
				},
				{
					{TEXT("blocked_segments"), TEXT("integer"), true, TEXT("Number of input path segments blocked by the capsule sweep.")},
					{TEXT("first_blocking_hit"), TEXT("object"), false, TEXT("Only when include_first_hit is true: first blocking segment_index (zero-based), native component target_ref (or null), shape_location, impact_point and start_penetrating; null when no segment is blocked."), true},
				},
				TEXT("Use blocked_segments by default. Request include_first_hit when locating the first obstruction; segment and substep order are preserved without another sweep. Positions are ue.world centimeters; initial penetration is not a precise contact surface. trace_complex selects the native query representation, not runtime traversability. Request geometry and controls are not repeated."),
			},
			{
				TEXT("world_scene_analysis"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{SceneAnalysisField()},
				{{TEXT("analysis"), TEXT("object"), true, TEXT("Versioned scene context or geometry facts with explicit pagination and coverage.")}},
				TEXT("Geometry facts do not decide design validity or prove rendered Z-fighting. Reuse analysis_ref for pages or the other analysis. Unsupported, pending and stale are never clear."),
			},
			{
				TEXT("world_nearby_shapes"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("nearby_shape_count"), TEXT("count"), true},
					ArrayField(TEXT("shapes"), {ScalarField(TEXT("target_ref"), true), SpatialField(TEXT("bounds_center"), TEXT("bounds_center"), true)}),
				},
				{
					{TEXT("count"), TEXT("integer"), true, TEXT("Number of nearby primitive components.")},
					{TEXT("shapes"), TEXT("array"), false, TEXT("Sorted collision-component references and bounds centers only when explicitly requested.")},
				},
				TEXT("Use count by default. Request shapes only when exact collision-component or center facts are needed; visual-only and transient components are excluded."),
				false,
				{},
				FString(),
				{TEXT("shapes")},
			},
			{
				TEXT("world_mesh_vertex"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("vertex_index"), TEXT("vertex_index"), true},
					SpatialField(TEXT("vertex_position"), TEXT("position"), true),
				},
				{
					{TEXT("vertex_index"), TEXT("integer"), true, TEXT("Stable index in the sorted Static Mesh vertex sequence.")},
					{TEXT("position"), TEXT("object"), true, TEXT("Vertex position in ue.world centimeters.")},
				},
				TEXT("Use vertex_index for a subsequent exact vertex read and position for world-space decisions; target, query point and transform evidence are not repeated."),
			},
			{
				TEXT("world_nav_project_point"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					SpatialField(TEXT("projected_point"), TEXT("projected_point"), true),
				},
				{
					{TEXT("projected_point"), TEXT("object"), true, TEXT("Point projected onto the current navigation data in ue.world centimeters.")},
				},
				TEXT("Use projected_point directly for the next navigation action; request point, extent and internal navigation state are not repeated."),
			},
			{
				TEXT("world_nav_find_path"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("reachable"), TEXT("reachable"), true},
					SpatialArrayField(TEXT("path_points")),
				},
				{
					{TEXT("reachable"), TEXT("boolean"), true, TEXT("Whether the requested endpoints have an accepted navigation path.")},
					{TEXT("path_points"), TEXT("array"), false, TEXT("Path points in ue.world centimeters only when explicitly requested.")},
				},
				TEXT("Use reachable by default. Request path points only when the next spatial action needs the route geometry; endpoints and internal navigation state are not repeated."),
				false,
				{},
				FString(),
			},
			{
				TEXT("world_nav_connectivity"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					ArrayField(TEXT("pairs"), {ScalarField(TEXT("from_index"), true), ScalarField(TEXT("to_index"), true),
						ScalarField(TEXT("reachable"), true), SpatialArrayField(TEXT("path_points"))}, true),
				},
				{
					{TEXT("pairs"), TEXT("array"), true, TEXT("Requested point-index pairs with reachability and optional path points.")},
				},
				TEXT("Use from_index, to_index and reachable for each pair. path_points appear only when explicitly requested; input points and dynamic selector names are not repeated."),
				false,
				{},
				FString(),
				{TEXT("pairs")},
			},
			{
				TEXT("world_data_layers"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("data_layers"),
				{
					{TEXT("data_layer_short_name"), TEXT("name"), true},
					{TEXT("data_layer_full_name"), TEXT("full_name"), false},
					{TEXT("data_layer_asset_path"), TEXT("asset_path"), false},
					{TEXT("data_layer_runtime"), TEXT("runtime"), false},
					{TEXT("data_layer_runtime_state"), TEXT("runtime_state"), false},
					{TEXT("data_layer_effective_runtime_state"), TEXT("effective_runtime_state"), false},
					{TEXT("data_layer_loaded_in_editor"), TEXT("loaded"), false},
					{TEXT("data_layer_effective_loaded_in_editor"), TEXT("effective_loaded"), false},
					{TEXT("data_layer_visible"), TEXT("visible"), false},
					{TEXT("data_layer_effective_visible"), TEXT("effective_visible"), false},
					{TEXT("data_layer_actor_membership_count"), TEXT("actor_count"), false},
					{TEXT("data_layer_actor_members"), TEXT("actors"), false, EUeAgentQueryPublicIdentityField::Selector, {}, true},
				},
				{
					{TEXT("data_layers"), TEXT("array"), true, TEXT("Data Layer records with readable names and current state.")},
				},
				TEXT("Use object_path or name for the next exact Data Layer action. actors and actor_count appear only when membership was explicitly requested; scope and internal state are not repeated."),
				false,
				{},
				TEXT("object_path"),
			},
			{
				TEXT("world_hlod_assignments"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("assignments"),
				{
					{TEXT("hlod_layer"), TEXT("hlod_layer_path"), true},
				},
				{
					{TEXT("assignments"), TEXT("array"), true, TEXT("Actors and their current HLOD Layer path.")},
				},
				TEXT("Use actor_path and hlod_layer_path directly for HLOD follow-up; folder/tag filters and internal owner identities are not repeated."),
				false,
				{},
				TEXT("actor_path"),
			},
			{
				TEXT("world_streaming_levels"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("levels"),
				{
					{TEXT("package_name"), TEXT("package_name"), true},
					{TEXT("loaded"), TEXT("loaded"), true},
					{TEXT("visible"), TEXT("visible"), true},
					{TEXT("editor_visible"), TEXT("editor_visible"), true},
					{TEXT("should_be_loaded"), TEXT("should_be_loaded"), true},
					{TEXT("should_be_visible"), TEXT("should_be_visible"), true},
					{TEXT("initially_loaded"), TEXT("initially_loaded"), false},
					{TEXT("initially_visible"), TEXT("initially_visible"), false},
				},
				{
					{TEXT("levels"), TEXT("array"), true, TEXT("Streaming Level objects and package names, optionally filtered by exact package.")},
				},
				TEXT("Use object_path or package_name directly for the next streaming-level action; requested filters and internal state are not repeated."),
				false,
				{},
				TEXT("object_path"),
			},
			{
				TEXT("world_partition"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("partition_enabled"), TEXT("enabled"), true},
					{TEXT("streaming_enabled"), TEXT("streaming_enabled"), true},
					{TEXT("user_loaded_region_count"), TEXT("loaded_region_count"), true},
					ArrayField(TEXT("loaded_regions"), {SpatialField(TEXT("center"), TEXT("center"), true), SpatialField(TEXT("extent"), TEXT("extent"), true)}),
					ArrayField(TEXT("actor_descriptors"), {ScalarField(TEXT("object_path"), true),
						ScalarField(TEXT("package"), true), ScalarField(TEXT("label"), true), ScalarField(TEXT("native_class"), true),
						ScalarField(TEXT("loaded"), true), ScalarField(TEXT("spatially_loaded"), true), ScalarField(TEXT("runtime_grid"), true),
						ScalarField(TEXT("hlod_layer"), true), ArrayField(TEXT("data_layers"), {}, true)}),
				},
				{
					{TEXT("enabled"), TEXT("boolean"), true, TEXT("Whether the current world has World Partition.")},
					{TEXT("streaming_enabled"), TEXT("boolean"), true, TEXT("Whether World Partition streaming is enabled in the Editor.")},
					{TEXT("loaded_region_count"), TEXT("integer"), true, TEXT("Number of user-loaded Editor regions.")},
					{TEXT("loaded_regions"), TEXT("array"), false, TEXT("Region center and extent in ue.world centimeters only when explicitly requested.")},
					{TEXT("actor_descriptors"), TEXT("array"), false, TEXT("Opt-in native WP descriptors, including external package and current loaded state; reading does not load Actors.")},
				},
				TEXT("Use enabled and streaming_enabled first. Request loaded_regions only when geometry is necessary; world identity, Editor name and internal spatial envelopes are not public."),
				false,
				{},
				FString(),
				{TEXT("loaded_regions"), TEXT("actor_descriptors")},
			},
			{
				TEXT("world_partition_health"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("partition_enabled"), TEXT("partition_enabled"), true},
					{TEXT("health_status"), TEXT("status"), true},
					{TEXT("error_count"), TEXT("error_count"), true},
					ArrayField(TEXT("issues"), {ScalarField(TEXT("code"), true), ScalarField(TEXT("subject"))}, true),
				},
				{
					{TEXT("partition_enabled"), TEXT("boolean"), true, TEXT("Whether the world has World Partition.")},
					{TEXT("status"), TEXT("string"), true, TEXT("not_enabled, valid, or invalid.")},
					{TEXT("error_count"), TEXT("integer"), true, TEXT("Number of reported partition issues.")},
					{TEXT("issues"), TEXT("array"), true, TEXT("Issue codes and readable subjects without GUID or internal detail strings.")},
				},
				TEXT("Use status and error_count first, then issue code/subject for actionable diagnostics; GUID detail and internal checker state are not public."),
				false,
				{},
				FString(),
				{TEXT("issues")},
			},
			{
				TEXT("world_physics_component"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("collision_enabled"), TEXT("collision_enabled"), true},
					{TEXT("material_path"), TEXT("material_path"), false},
					{TEXT("gravity_enabled"), TEXT("gravity_enabled"), false},
					{TEXT("simulating_physics"), TEXT("simulating_physics"), false},
				},
				{
					{TEXT("collision_enabled"), TEXT("string"), true, TEXT("Current UE collision-enabled enum value.")},
					{TEXT("material_path"), TEXT("string"), false, TEXT("Material slot zero path when explicitly requested.")},
					{TEXT("gravity_enabled"), TEXT("boolean"), false, TEXT("Runtime gravity state when explicitly requested.")},
					{TEXT("simulating_physics"), TEXT("boolean"), false, TEXT("Runtime simulation state when explicitly requested.")},
					{TEXT("collision_responses"), TEXT("object"), false, TEXT("Per-channel collision responses when explicitly requested.")},
				},
				TEXT("Use collision_enabled by default. Optional material, runtime state and collision_responses appear only when requested; target and internal owner identity are not repeated."),
				false,
				{
					TEXT("world.physics:response:"),
					TEXT("primitive_component"),
					TEXT("collision_responses"),
				},
			},
			{
				TEXT("world_physics_constraint"),
				EUeAgentQueryPublicProjectionStrategy::SingleRecord,
				TEXT("record"),
				{
					{TEXT("constraint_path"), TEXT("path"), true},
					{TEXT("constraint_component_1"), TEXT("component_1"), false},
					{TEXT("constraint_bone_1"), TEXT("bone_1"), false},
					{TEXT("constraint_component_2"), TEXT("component_2"), false},
					{TEXT("constraint_bone_2"), TEXT("bone_2"), false},
					{TEXT("linear_limit_cm"), TEXT("linear_limit_cm"), false},
					{TEXT("linear_x_motion"), TEXT("linear_x_motion"), false},
					{TEXT("linear_y_motion"), TEXT("linear_y_motion"), false},
					{TEXT("linear_z_motion"), TEXT("linear_z_motion"), false},
					{TEXT("angular_swing_1_limit_degrees"), TEXT("swing_1_limit_degrees"), false},
					{TEXT("angular_swing_2_limit_degrees"), TEXT("swing_2_limit_degrees"), false},
					{TEXT("angular_twist_limit_degrees"), TEXT("twist_limit_degrees"), false},
					{TEXT("constraint_broken"), TEXT("broken"), false},
					{TEXT("current_twist_degrees"), TEXT("current_twist_degrees"), false},
					{TEXT("current_swing_1_degrees"), TEXT("current_swing_1_degrees"), false},
					{TEXT("current_swing_2_degrees"), TEXT("current_swing_2_degrees"), false},
				},
				{
					{TEXT("path"), TEXT("string"), true, TEXT("Exact Physics Constraint Component path.")},
					{TEXT("component_1"), TEXT("string"), false, TEXT("First constrained component when endpoints are requested.")},
					{TEXT("bone_1"), TEXT("string"), false, TEXT("First constrained bone when endpoints are requested.")},
					{TEXT("component_2"), TEXT("string"), false, TEXT("Second constrained component when endpoints are requested.")},
					{TEXT("bone_2"), TEXT("string"), false, TEXT("Second constrained bone when endpoints are requested.")},
					{TEXT("linear_limit_cm"), TEXT("number"), false, TEXT("Linear constraint limit in centimeters.")},
					{TEXT("linear_x_motion"), TEXT("string"), false, TEXT("Linear X motion enum value.")},
					{TEXT("linear_y_motion"), TEXT("string"), false, TEXT("Linear Y motion enum value.")},
					{TEXT("linear_z_motion"), TEXT("string"), false, TEXT("Linear Z motion enum value.")},
					{TEXT("swing_1_limit_degrees"), TEXT("number"), false, TEXT("First angular swing limit in degrees.")},
					{TEXT("swing_2_limit_degrees"), TEXT("number"), false, TEXT("Second angular swing limit in degrees.")},
					{TEXT("twist_limit_degrees"), TEXT("number"), false, TEXT("Angular twist limit in degrees.")},
					{TEXT("broken"), TEXT("boolean"), false, TEXT("Runtime broken state.")},
					{TEXT("current_twist_degrees"), TEXT("number"), false, TEXT("Current runtime twist in degrees.")},
					{TEXT("current_swing_1_degrees"), TEXT("number"), false, TEXT("Current runtime first swing in degrees.")},
					{TEXT("current_swing_2_degrees"), TEXT("number"), false, TEXT("Current runtime second swing in degrees.")},
				},
				TEXT("Use path by default. Endpoint, limit and runtime fields appear only when explicitly requested; request flags and internal owner identity are not repeated."),
			},
			{
				TEXT("landscape_height_grid"),
				EUeAgentQueryPublicProjectionStrategy::GroupedRecords,
				TEXT("height_grids"),
				{
					LandscapeHeightField(),
				},
				{
					{TEXT("height_grids"), TEXT("array"), true, TEXT("Bounded Landscape local-height grids with extent, scale, ordering and values.")},
				},
				TEXT("Interpret heights over the requested inclusive region in row-major x-fastest order. Extent uses ue.landscape.local_sample_grid and scale_cm_per_local_unit maps local axes to centimeters; target, requested region and managed ownership are not repeated."),
				true,
				{},
				FString(),
			},
		};
	}

	namespace
	{
		FString FieldPath(const FString& Parent, const FString& Field)
		{
			return Parent.IsEmpty() ? Field : Parent + TEXT(".") + Field;
		}

		bool InvalidProjection(Contract::FDiagnostic& Error, const FString& Path, const TCHAR* Reason,
			const FString& Actual = FString(), bool bInternalValue = false)
		{
			Error = Contract::FDiagnostic::Failure(TEXT("query_public_projection_invalid"), Reason, TEXT("query_public_projection"));
			Error.PropertyPath = Path;
			Error.SourceKind = TEXT("native_result");
			if (bInternalValue) Error.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			else Error.ActualValue = MakeShared<FJsonValueString>(Actual);
			return false;
		}
	}

	bool FQueryPublicFieldProjection::IsValid() const
	{
		Contract::FDiagnostic Diagnostic;
		return TryValidate(Diagnostic);
	}

	bool FQueryPublicFieldProjection::TryValidate(Contract::FDiagnostic& Error, const FString& Path) const
	{
		Error.Reset();
		if (Selector.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("selector")), TEXT("A projected field requires a source selector."));
		if (PublicName.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("public_name")), TEXT("A projected field requires a public name."));
		for (int32 Index = 0; Index < PublicContainerPath.Num(); ++Index)
			if (PublicContainerPath[Index].IsEmpty()) return InvalidProjection(Error, FieldPath(Path, FString::Printf(TEXT("public_container_path[%d]"), Index)), TEXT("A public container path segment must not be empty."));
		TSet<FString> Selectors;
		TSet<FString> PublicNames;
		for (int32 Index = 0; Index < Children.Num(); ++Index)
		{
			const auto& Child = Children[Index];
			const FString ChildPath = FieldPath(Path, FString::Printf(TEXT("children[%d]"), Index));
			if (!Child.TryValidate(Error, ChildPath)) return false;
			if (!Child.PublicContainerPath.IsEmpty()) return InvalidProjection(Error, FieldPath(ChildPath, TEXT("public_container_path")), TEXT("A nested field cannot also specify a record container path."), FString::Join(Child.PublicContainerPath, TEXT(".")));
			if (Selectors.Contains(Child.Selector)) return InvalidProjection(Error, FieldPath(ChildPath, TEXT("selector")), TEXT("Two sibling fields use the same source selector."), Child.Selector, true);
			if (PublicNames.Contains(Child.PublicName)) return InvalidProjection(Error, FieldPath(ChildPath, TEXT("public_name")), TEXT("Two sibling fields use the same public name."), Child.PublicName);
			Selectors.Add(Child.Selector);
			PublicNames.Add(Child.PublicName);
		}
		return true;
	}

	bool FQueryPublicResultField::IsValid() const
	{
		Contract::FDiagnostic Diagnostic;
		return TryValidate(Diagnostic);
	}

	bool FQueryPublicResultField::TryValidate(Contract::FDiagnostic& Error, const FString& Path) const
	{
		Error.Reset();
		if (Name.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("name")), TEXT("A public result field requires a name."));
		if (WireType.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("wire_type")), TEXT("A public result field requires a wire type."));
		if (Description.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("description")), TEXT("A public result field requires a description."));
		return true;
	}

	bool FQueryPublicDynamicFieldProjection::IsEmpty() const
	{
		return SemanticIdPrefix.IsEmpty() && TargetKind.IsEmpty() && PublicContainerName.IsEmpty();
	}

	bool FQueryPublicDynamicFieldProjection::IsValid() const
	{
		Contract::FDiagnostic Diagnostic;
		return TryValidate(Diagnostic);
	}

	bool FQueryPublicDynamicFieldProjection::TryValidate(Contract::FDiagnostic& Error, const FString& Path) const
	{
		Error.Reset();
		if (SemanticIdPrefix.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("semantic_id_prefix")), TEXT("A dynamic projection requires a semantic identifier prefix."));
		if (TargetKind.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("target_kind")), TEXT("A dynamic projection requires a target kind."));
		if (PublicContainerName.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("public_container_name")), TEXT("A dynamic projection requires a public container name."));
		return true;
	}

	bool FQueryPublicProjectionDescriptor::IsValid() const
	{
		Contract::FDiagnostic Diagnostic;
		return TryValidate(Diagnostic);
	}

	bool FQueryPublicProjectionDescriptor::TryValidate(Contract::FDiagnostic& Error, const FString& Path) const
	{
		Error.Reset();
		if (Id.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("id")), TEXT("A public projection descriptor requires an identifier."));
		if (ResultFields.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("result_fields")), TEXT("A public projection must declare at least one result field."), TEXT("0 fields"));
		if (ResultGuidance.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("result_guidance")), TEXT("A public projection requires result guidance."));
		TSet<FString> ResultNames;
		for (int32 Index = 0; Index < ResultFields.Num(); ++Index)
		{
			const auto& Field = ResultFields[Index];
			const FString ItemPath = FieldPath(Path, FString::Printf(TEXT("result_fields[%d]"), Index));
			if (!Field.TryValidate(Error, ItemPath)) return false;
			if (ResultNames.Contains(Field.Name)) return InvalidProjection(Error, FieldPath(ItemPath, TEXT("name")), TEXT("The public result field name is declared more than once."), Field.Name);
			ResultNames.Add(Field.Name);
		}
		const bool bRecordProjection = Strategy == EUeAgentQueryPublicProjectionStrategy::GroupedRecords
			|| Strategy == EUeAgentQueryPublicProjectionStrategy::SingleRecord || Strategy == EUeAgentQueryPublicProjectionStrategy::SnapshotRecordArray;
		if (!bRecordProjection)
		{
			const TCHAR* UnsupportedField = !CollectionField.IsEmpty() ? TEXT("collection_field")
				: !FieldProjections.IsEmpty() ? TEXT("field_projections") : bFlattenSingleFieldRecords ? TEXT("flatten_single_field_records")
				: !DynamicFields.IsEmpty() ? TEXT("dynamic_fields") : !OwnerPublicName.IsEmpty() ? TEXT("owner_public_name")
				: !PagedArrayFields.IsEmpty() ? TEXT("paged_array_fields") : !SnapshotArraySelector.IsEmpty() ? TEXT("snapshot_array_selector")
				: !CommandIds.IsEmpty() ? TEXT("command_ids") : nullptr;
			if (!UnsupportedField) return true;
			InvalidProjection(Error, FieldPath(Path, UnsupportedField), TEXT("This record-specific option is not supported by the selected projection strategy."));
			Error.ActualValue.Reset();
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unset"));
			Error.ActualEvidence = MakeShared<FJsonValueNumber>(static_cast<int32>(Strategy));
			return false;
		}
		if (CollectionField.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("collection_field")), TEXT("A record projection requires a collection field."));
		if ((Strategy == EUeAgentQueryPublicProjectionStrategy::GroupedRecords || Strategy == EUeAgentQueryPublicProjectionStrategy::SnapshotRecordArray) && !ResultNames.Contains(CollectionField))
			return InvalidProjection(Error, FieldPath(Path, TEXT("collection_field")), TEXT("The collection field is not declared in result_fields."), CollectionField);
		if (FieldProjections.IsEmpty() && DynamicFields.IsEmpty()) return InvalidProjection(Error, FieldPath(Path, TEXT("field_projections")), TEXT("A record projection requires declared fields or a dynamic field projection."), TEXT("0 fields"));
		if (!DynamicFields.IsEmpty() && !DynamicFields.TryValidate(Error, FieldPath(Path, TEXT("dynamic_fields")))) return false;
		if (Strategy == EUeAgentQueryPublicProjectionStrategy::SnapshotRecordArray && SnapshotArraySelector.IsEmpty())
			return InvalidProjection(Error, FieldPath(Path, TEXT("snapshot_array_selector")), TEXT("A snapshot record array requires a source array selector."));
		TSet<FString> Selectors;
		TSet<FString> PublicNames;
		TArray<TArray<FString>> PublicPaths;
		for (int32 Index = 0; Index < FieldProjections.Num(); ++Index)
		{
			const auto& Field = FieldProjections[Index];
			const FString ItemPath = FieldPath(Path, FString::Printf(TEXT("field_projections[%d]"), Index));
			if (!Field.TryValidate(Error, ItemPath)) return false;
			if (Selectors.Contains(Field.Selector)) return InvalidProjection(Error, FieldPath(ItemPath, TEXT("selector")), TEXT("Two projected fields use the same source selector."), Field.Selector, true);
			if (!Field.PublicContainerPath.IsEmpty() && (Strategy == EUeAgentQueryPublicProjectionStrategy::SnapshotRecordArray || bFlattenSingleFieldRecords))
				return InvalidProjection(Error, FieldPath(ItemPath, TEXT("public_container_path")), TEXT("Record array and flattened projections cannot contain nested record paths."), FString::Join(Field.PublicContainerPath, TEXT(".")));
			TArray<FString> PublicPath = Field.PublicContainerPath;
			PublicPath.Add(Field.PublicName);
			for (int32 Previous = 0; Previous < PublicPaths.Num(); ++Previous)
			{
				const auto& ExistingPath = PublicPaths[Previous];
				int32 CommonLength = 0;
				const int32 ShorterLength = FMath::Min(PublicPath.Num(), ExistingPath.Num());
				while (CommonLength < ShorterLength && PublicPath[CommonLength] == ExistingPath[CommonLength]) ++CommonLength;
				if (CommonLength == ShorterLength)
				{
					InvalidProjection(Error, ItemPath, TEXT("Two public field paths overlap; a value cannot also be an object container."), FString::Join(PublicPath, TEXT(".")));
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("no overlap with ") + FString::Join(ExistingPath, TEXT(".")));
					return false;
				}
			}
			Selectors.Add(Field.Selector);
			PublicNames.Add(PublicPath[0]);
			PublicPaths.Add(MoveTemp(PublicPath));
		}
		const bool bHasDynamicFields = !DynamicFields.IsEmpty();
		if (bHasDynamicFields && PublicNames.Contains(DynamicFields.PublicContainerName))
			return InvalidProjection(Error, FieldPath(Path, TEXT("dynamic_fields.public_container_name")), TEXT("The dynamic container name conflicts with a projected field."), DynamicFields.PublicContainerName);
		if (!OwnerPublicName.IsEmpty() && (PublicNames.Contains(OwnerPublicName) || (bHasDynamicFields && DynamicFields.PublicContainerName == OwnerPublicName)))
			return InvalidProjection(Error, FieldPath(Path, TEXT("owner_public_name")), TEXT("The owner field name conflicts with a projected field or dynamic container."), OwnerPublicName);
		TSet<FString> PageFieldNames;
		for (int32 Index = 0; Index < PagedArrayFields.Num(); ++Index)
		{
			const auto& Field = PagedArrayFields[Index];
			if (Field.IsEmpty() || PageFieldNames.Contains(Field)) return InvalidProjection(Error, FieldPath(Path, FString::Printf(TEXT("paged_array_fields[%d]"), Index)), TEXT("Paged array field names must be non-empty and unique."), Field);
			PageFieldNames.Add(Field);
		}
		TSet<FString> CommandNames;
		for (int32 Index = 0; Index < CommandIds.Num(); ++Index)
		{
			const auto& Command = CommandIds[Index];
			if (Command.IsEmpty() || CommandNames.Contains(Command)) return InvalidProjection(Error, FieldPath(Path, FString::Printf(TEXT("command_ids[%d]"), Index)), TEXT("Projection command identifiers must be non-empty and unique."), Command);
			CommandNames.Add(Command);
		}
		if (Strategy == EUeAgentQueryPublicProjectionStrategy::SingleRecord)
		{
			for (int32 Index = 0; Index < ResultFields.Num(); ++Index)
			{
				const auto& Field = ResultFields[Index];
				if (!PublicNames.Contains(Field.Name) && Field.Name != OwnerPublicName && (!bHasDynamicFields || Field.Name != DynamicFields.PublicContainerName))
					return InvalidProjection(Error, FieldPath(Path, FString::Printf(TEXT("result_fields[%d].name"), Index)), TEXT("The declared result field has no projected field, owner field or dynamic container."), Field.Name);
			}
		}
		return true;
	}

	const FQueryPublicProjectionDescriptor* FindQueryPublicProjectionDescriptor(const FString& DescriptorId, Contract::FDiagnostic* OutDiagnostic)
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		const auto* Found = Descriptors.FindByPredicate([&DescriptorId](const auto& Descriptor) { return Descriptor.Id == DescriptorId; });
		if (!Found) return nullptr;
		Contract::FDiagnostic Diagnostic;
		if (Found->TryValidate(Diagnostic, TEXT("public_projection"))) return Found;
		Diagnostic.SubjectId = DescriptorId;
		if (OutDiagnostic) *OutDiagnostic = MoveTemp(Diagnostic);
		return nullptr;
	}

	const FQueryPublicProjectionDescriptor* FindQueryPublicProjectionDescriptorForCommand(const FString& CommandId, Contract::FDiagnostic* OutDiagnostic)
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		const auto* Found = Descriptors.FindByPredicate([&CommandId](const auto& Descriptor) { return Descriptor.CommandIds.Contains(CommandId); });
		if (!Found) return nullptr;
		Contract::FDiagnostic Diagnostic;
		if (Found->TryValidate(Diagnostic, TEXT("public_projection"))) return Found;
		Diagnostic.CandidateName = CommandId;
		if (OutDiagnostic) *OutDiagnostic = MoveTemp(Diagnostic);
		return nullptr;
	}

	Projection::EUeAgentProjectionPageUnit QueryPublicProjectionPageUnit(
		const FQueryPublicProjectionDescriptor& Descriptor)
	{
		if (Descriptor.Strategy
			== EUeAgentQueryPublicProjectionStrategy::SingleRecord)
		{
			return Projection::EUeAgentProjectionPageUnit::SingleRecord;
		}
		if (Descriptor.Strategy
				== EUeAgentQueryPublicProjectionStrategy::GroupedRecords
			|| Descriptor.Strategy
				== EUeAgentQueryPublicProjectionStrategy::SnapshotRecordArray)
		{
			return Projection::EUeAgentProjectionPageUnit::OwnerRecord;
		}
		return Projection::EUeAgentProjectionPageUnit::Entry;
	}
}
