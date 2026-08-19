// Immutable compiled C0 source for TimelineAnimation. The E1 fixture is independent.
static constexpr ANSICHAR GTimelineAnimationDescriptorSpecJson0[] = R"UAITAD0({
  "schema": "uai.domain_adapter.timeline_animation.exact_contract_fixture.v1",
  "domain_id": "timeline_animation",
  "temporal_contract_id": "uai.temporal_contract.v1",
  "space_contract_id": "uai.space_contract.v1",
  "descriptor_count": 19,
  "query_count": 8,
  "operation_count": 11,
  "components": {
    "target_ref": {"type":"string","minLength":1},
    "ref_array": {"type":"array","items":{"component":"target_ref"},"uniqueItems":true},
    "revision": {
      "type":"object",
      "properties": {
        "source_id":{"type":"string","minLength":1},
        "generation":{"type":"string","minLength":1},
        "sequence":{"type":"string","minLength":1}
      },
      "required":["source_id","generation","sequence"],
      "additionalProperties":false
    },
    "rate": {
      "type":"object",
      "properties": {
        "numerator":{"type":"string","minLength":1},
        "denominator":{"type":"string","minLength":1}
      },
      "required":["numerator","denominator"],
      "additionalProperties":false
    },
    "temporal_point": {
      "type":"object",
      "properties": {
        "clock_id":{"type":"string","minLength":1},
        "tick":{"type":"string","minLength":1},
        "rate":{"component":"rate"},
        "state":{"type":"string","const":"authored"}
      },
      "required":["clock_id","tick","rate","state"],
      "additionalProperties":false
    },
    "temporal_bound": {
      "type":"object",
      "properties": {
        "kind":{"type":"string","enum":["open","inclusive","exclusive"]},
        "point":{"component":"temporal_point"}
      },
      "required":["kind"],
      "additionalProperties":false
    },
    "temporal_range": {
      "type":"object",
      "properties": {
        "clock_id":{"type":"string","minLength":1},
        "state":{"type":"string","const":"authored"},
        "lower":{"component":"temporal_bound"},
        "upper":{"component":"temporal_bound"}
      },
      "required":["clock_id","state","lower","upper"],
      "additionalProperties":false
    },
    "frame_3d": {
      "type":"object",
      "properties": {
        "frame_id":{"type":"string","minLength":1},
        "basis_frame_id":{"type":"string","minLength":1},
        "dimension":{"type":"integer","const":3},
        "unit":{"type":"string","minLength":1},
        "basis_evidence_id":{"type":"string","minLength":1}
      },
      "required":["frame_id","basis_frame_id","dimension","unit","basis_evidence_id"],
      "additionalProperties":false
    },
    "transform_3d": {
      "type":"object",
      "properties": {
        "subject":{"type":"string","minLength":1},
        "quantity":{"type":"string","const":"transform"},
        "frame":{"component":"frame_3d"},
        "components":{"type":"array","items":{"type":"string","minLength":1},"minItems":10,"maxItems":10},
        "conversion_evidence_ids":{"type":"array","items":{"type":"string","minLength":1},"minItems":1,"uniqueItems":true}
      },
      "required":["subject","quantity","frame","components","conversion_evidence_ids"],
      "additionalProperties":false
    },
    "verification": {
      "type":"object",
      "properties": {
        "oracle_kind":{"type":"string","minLength":1},
        "evidence_ids":{"type":"array","items":{"type":"string","minLength":1},"minItems":1,"uniqueItems":true}
      },
      "required":["oracle_kind","evidence_ids"],
      "additionalProperties":false
    },
    "asset_summary": {
      "type":"object",
      "properties": {
        "asset_ref":{"component":"target_ref"},
        "asset_kind":{"type":"string","minLength":1},
        "rate":{"component":"rate"},
        "authored_range":{"component":"temporal_range"},
        "item_refs":{"component":"ref_array"},
        "observed_revision":{"component":"revision"}
      },
      "required":["asset_ref","asset_kind","rate","authored_range","item_refs","observed_revision"],
      "additionalProperties":false
    },
    "bone_key": {
      "type":"object",
      "properties": {
        "time":{"component":"temporal_point"},
        "transform":{"component":"transform_3d"}
      },
      "required":["time","transform"],
      "additionalProperties":false
    },
    "bone_track": {
      "type":"object",
      "properties": {
        "bone_ref":{"component":"target_ref"},
        "keys":{"type":"array","items":{"component":"bone_key"},"minItems":1}
      },
      "required":["bone_ref","keys"],
      "additionalProperties":false
    },
    "curve_key": {
      "type":"object",
      "properties": {
        "time":{"component":"temporal_point"},
        "value":{"type":"number"},
        "interpolation":{"type":"string","enum":["constant","linear","cubic"]}
      },
      "required":["time","value","interpolation"],
      "additionalProperties":false
    },
    "curve_change": {
      "type":"object",
      "properties": {
        "curve_ref":{"component":"target_ref"},
        "operation":{"type":"string","enum":["upsert","remove"]},
        "keys":{"type":"array","items":{"component":"curve_key"}}
      },
      "required":["curve_ref","operation","keys"],
      "additionalProperties":false
    },
    "metadata_change": {
      "type":"object",
      "properties": {
        "metadata_ref":{"component":"target_ref"},
        "operation":{"type":"string","enum":["upsert","remove"]},
        "value_type":{"type":"string","enum":["string","integer","decimal","boolean","asset_ref"]},
        "value_lexeme":{"type":"string"}
      },
      "required":["metadata_ref","operation","value_type","value_lexeme"],
      "additionalProperties":false
    },
    "event_change": {
      "type":"object",
      "properties": {
        "event_ref":{"component":"target_ref"},
        "event_kind":{"type":"string","enum":["notify_track","notify","notify_state","sync_marker"]},
        "operation":{"type":"string","enum":["upsert","remove"]},
        "time":{"component":"temporal_point"},
        "range":{"component":"temporal_range"},
        "payload_class_ref":{"component":"target_ref"},
        "track_ref":{"component":"target_ref"}
      },
      "required":["event_ref","event_kind","operation"],
      "additionalProperties":false
    },
    "blend_parameter": {
      "type":"object",
      "properties": {
        "axis_ref":{"component":"target_ref"},
        "value":{"type":"number"}
      },
      "required":["axis_ref","value"],
      "additionalProperties":false
    },
    "timeline_key": {
      "type":"object",
      "properties": {
        "key_ref":{"component":"target_ref"},
        "time":{"component":"temporal_point"},
        "value_type":{"type":"string","enum":["boolean","integer","decimal","transform","asset_ref","string"]},
        "value_lexeme":{"type":"string"},
        "transform":{"component":"transform_3d"}
      },
      "required":["key_ref","time","value_type"],
      "additionalProperties":false
    },
    "timeline_section": {
      "type":"object",
      "properties": {
        "section_ref":{"component":"target_ref"},
        "section_kind":{"type":"string","minLength":1},
        "range":{"component":"temporal_range"},
        "source_ref":{"component":"target_ref"},
        "keys":{"type":"array","items":{"component":"timeline_key"}}
      },
      "required":["section_ref","section_kind","range","keys"],
      "additionalProperties":false
    },
    "timeline_track": {
      "type":"object",
      "properties": {
        "track_ref":{"component":"target_ref"},
        "track_kind":{"type":"string","minLength":1},
        "property_path":{"type":"string"},
        "sections":{"type":"array","items":{"component":"timeline_section"}}
      },
      "required":["track_ref","track_kind","sections"],
      "additionalProperties":false
    },
    "timeline_binding": {
      "type":"object",
      "properties": {
        "binding_ref":{"component":"target_ref"},
        "binding_kind":{"type":"string","enum":["possessable","spawnable","widget","slot"]},
        "object_ref":{"component":"target_ref"},
        "tracks":{"type":"array","items":{"component":"timeline_track"}}
      },
      "required":["binding_ref","binding_kind","tracks"],
      "additionalProperties":false
    },
    "timeline_structure": {
      "type":"object",
      "properties": {
        "display_rate":{"component":"rate"},
        "tick_resolution":{"component":"rate"},
        "playback_range":{"component":"temporal_range"},
        "bindings":{"type":"array","items":{"component":"timeline_binding"}},
        "root_tracks":{"type":"array","items":{"component":"timeline_track"}}
      },
      "required":["display_rate","tick_resolution","playback_range","bindings","root_tracks"],
      "additionalProperties":false
    },
    "pose_item": {
      "type":"object",
      "properties": {
        "bone_ref":{"component":"target_ref"},
        "transform":{"component":"transform_3d"}
      },
      "required":["bone_ref","transform"],
      "additionalProperties":false
    },
    "morph_weight": {
      "type":"object",
      "properties": {
        "morph_ref":{"component":"target_ref"},
        "weight":{"type":"number"}
      },
      "required":["morph_ref","weight"],
      "additionalProperties":false
    }
  },
  "descriptors": [
    {"capability_id":"animation.sequence.read","descriptor_id":"uai.query.animation.sequence.read","operation_kind":"query","provider_binding":"timeline_animation.sequence.read","query_kind":"state","state_planes":["persisted"],"required_facets":[],"oracle_kind":"independent_asset_or_sample_readback","input_fields":[{"name":"temporal_range","required":false,"component":"temporal_range"},{"name":"include_tracks","required":false,"schema":{"type":"boolean"}},{"name":"include_keys","required":false,"schema":{"type":"boolean"}}],"result_fields":[{"name":"sequence","required":true,"component":"asset_summary"}]},
    {"capability_id":"animation.sequence.root_motion.analyze","descriptor_id":"uai.query.animation.sequence.root_motion.analyze","operation_kind":"query","provider_binding":"timeline_animation.sequence.analyze_root_motion","query_kind":"state","state_planes":["persisted"],"required_facets":[],"oracle_kind":"independent_asset_or_sample_readback","input_fields":[{"name":"temporal_range","required":true,"component":"temporal_range"},{"name":"sample_rate","required":true,"component":"rate"},{"name":"root_bone_ref","required":false,"component":"target_ref"}],"result_fields":[{"name":"sample_range","required":true,"component":"temporal_range"},{"name":"translation_delta","required":true,"component":"transform_3d"},{"name":"sample_count","required":true,"schema":{"type":"integer","minimum":1}},{"name":"observed_revision","required":true,"component":"revision"}]},
    {"capability_id":"animation.sequence.pose.sample","descriptor_id":"uai.query.animation.sequence.pose.sample","operation_kind":"query","provider_binding":"timeline_animation.sequence.sample_pose","query_kind":"state","state_planes":["persisted"],"required_facets":[],"oracle_kind":"independent_asset_or_sample_readback","input_fields":[{"name":"time_point","required":true,"component":"temporal_point"},{"name":"bone_refs","required":false,"component":"ref_array"},{"name":"output_frame","required":false,"component":"frame_3d"}],"result_fields":[{"name":"time_point","required":true,"component":"temporal_point"},{"name":"pose","required":true,"schema":{"type":"array","items":{"component":"pose_item"}}},{"name":"observed_revision","required":true,"component":"revision"}]},
    {"capability_id":"animation.blend_space.sample","descriptor_id":"uai.query.animation.blend_space.sample","operation_kind":"query","provider_binding":"timeline_animation.blend_space.sample","query_kind":"state","state_planes":["persisted"],"required_facets":[],"oracle_kind":"independent_asset_or_sample_readback","input_fields":[{"name":"blend_parameters","required":true,"schema":{"type":"array","items":{"component":"blend_parameter"},"minItems":1}},{"name":"time_point","required")UAITAD0";
static constexpr ANSICHAR GTimelineAnimationDescriptorSpecJson1[] = R"UAITAD1(:false,"component":"temporal_point"},{"name":"bone_refs","required":false,"component":"ref_array"},{"name":"output_frame","required":false,"component":"frame_3d"}],"result_fields":[{"name":"sample_weights","required":true,"schema":{"type":"array","items":{"component":"morph_weight"}}},{"name":"pose","required":true,"schema":{"type":"array","items":{"component":"pose_item"}}},{"name":"observed_revision","required":true,"component":"revision"}]},
    {"capability_id":"animation.sequence.bone_tracks.replace","descriptor_id":"uai.operation.animation.sequence.bone_tracks.replace","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_tracks_curves_events_readback","input_fields":[{"name":"authored_range","required":true,"component":"temporal_range"},{"name":"sample_rate","required":true,"component":"rate"},{"name":"bone_tracks","required":true,"schema":{"type":"array","items":{"component":"bone_track"},"minItems":1}},{"name":"expected_revision","required":true,"component":"revision"}],"result_fields":[{"name":"sequence","required":true,"component":"asset_summary"},{"name":"changed_item_refs","required":true,"component":"ref_array"},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"animation.sequence.curves.update","descriptor_id":"uai.operation.animation.sequence.curves.update","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_tracks_curves_events_readback","input_fields":[{"name":"curve_changes","required":true,"schema":{"type":"array","items":{"component":"curve_change"},"minItems":1}},{"name":"expected_revision","required":true,"component":"revision"}],"result_fields":[{"name":"sequence","required":true,"component":"asset_summary"},{"name":"changed_item_refs","required":true,"component":"ref_array"},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"animation.sequence.metadata.update","descriptor_id":"uai.operation.animation.sequence.metadata.update","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_tracks_curves_events_readback","input_fields":[{"name":"metadata_changes","required":true,"schema":{"type":"array","items":{"component":"metadata_change"},"minItems":1}},{"name":"expected_revision","required":true,"component":"revision"}],"result_fields":[{"name":"sequence","required":true,"component":"asset_summary"},{"name":"changed_item_refs","required":true,"component":"ref_array"},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"animation.sequence.events.update","descriptor_id":"uai.operation.animation.sequence.events.update","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_tracks_curves_events_readback","input_fields":[{"name":"event_changes","required":true,"schema":{"type":"array","items":{"component":"event_change"},"minItems":1}},{"name":"expected_revision","required":true,"component":"revision"}],"result_fields":[{"name":"sequence","required":true,"component":"asset_summary"},{"name":"changed_item_refs","required":true,"component":"ref_array"},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"animation.montage.read","descriptor_id":"uai.query.animation.montage.read","operation_kind":"query","provider_binding":"timeline_animation.montage.read","query_kind":"state","state_planes":["persisted"],"required_facets":[],"oracle_kind":"independent_asset_or_sample_readback","input_fields":[{"name":"include_sections","required":false,"schema":{"type":"boolean"}},{"name":"include_slots","required":false,"schema":{"type":"boolean"}}],"result_fields":[{"name":"montage","required":true,"component":"asset_summary"},{"name":"structure","required":true,"component":"timeline_structure"}]},
    {"capability_id":"animation.montage.create","descriptor_id":"uai.operation.animation.montage.create","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_structure_identity_and_range_readback","input_fields":[{"name":"destination_ref","required":true,"component":"target_ref"},{"name":"skeleton_ref","required":true,"component":"target_ref"},{"name":"initial_structure","required":true,"component":"timeline_structure"},{"name":"source_animation_ref","required":false,"component":"target_ref"}],"result_fields":[{"name":"montage","required":true,"component":"asset_summary"},{"name":"structure","required":true,"component":"timeline_structure"},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"animation.montage.structure.replace","descriptor_id":"uai.operation.animation.montage.structure.replace","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_structure_identity_and_range_readback","input_fields":[{"name":"structure","required":true,"component":"timeline_structure"},{"name":"expected_revision","required":true,"component":"revision"}],"result_fields":[{"name":"montage","required":true,"component":"asset_summary"},{"name":"structure","required":true,"component":"timeline_structure"},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"animation.skeleton.slots.read","descriptor_id":"uai.query.animation.skeleton.slots.read","operation_kind":"query","provider_binding":"timeline_animation.skeleton.read_slots","query_kind":"state","state_planes":["persisted"],"required_facets":[],"oracle_kind":"independent_asset_or_sample_readback","input_fields":[],"result_fields":[{"name":"slot_groups","required":true,"schema":{"type":"array","items":{"component":"timeline_binding"}}},{"name":"observed_revision","required":true,"component":"revision"}]},
    {"capability_id":"timeline.sequence.read","descriptor_id":"uai.query.timeline.sequence.read","operation_kind":"query","provider_binding":"timeline_animation.level_sequence.read","query_kind":"state","state_planes":["persisted"],"required_facets":[],"oracle_kind":"independent_asset_or_sample_readback","input_fields":[{"name":"temporal_range","required":false,"component":"temporal_range"},{"name":"include_bindings","required":false,"schema":{"type":"boolean"}},{"name":"include_tracks","required":false,"schema":{"type":"boolean"}},{"name":"include_keys","required":false,"schema":{"type":"boolean"}}],"result_fields":[{"name":"sequence","required":true,"component":"asset_summary"},{"name":"structure","required":true,"component":"timeline_structure"}]},
    {"capability_id":"timeline.sequence.create","descriptor_id":"uai.operation.timeline.sequence.create","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_structure_identity_and_range_readback","input_fields":[{"name":"destination_ref","required":true,"component":"target_ref"},{"name":"initial_structure","required":true,"component":"timeline_structure"},{"name":"display_rate","required":false,"component":"rate"},{"name":"tick_resolution","required":false,"component":"rate"}],"result_fields":[{"name":"sequence","required":true,"component":"asset_summary"},{"name":"structure","required":true,"component":"timeline_structure"},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"timeline.sequence.structure.replace","descriptor_id":"uai.operation.timeline.sequence.structure.replace","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_structure_identity_and_range_readback","input_fields":[{"name":"structure","required":true,"component":"timeline_structure"},{"name":"expected_revision","required":true,"component":"revision"}],"result_fields":[{"name":"sequence","required":true,"component":"asset_summary"},{"name":"structure","required":true,"component":"timeline_structure"},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"timeline.widget_animation.read","descriptor_id":"uai.query.timeline.widget_animation.read","operation_kind":"query","provider_binding":"timeline_animation.widget_animation.read","query_kind":"state","state_planes":["persisted"],"required_facets":[],"oracle_kind":"independent_asset_or_sample_readback","input_fields":[{"name":"temporal_range","required":false,"component":"temporal_range"},{"name":"include_bindings","required":false,"schema":{"type":"boolean"}},{"name":"include_tracks","required":false,"schema":{"type":"boolean"}}],"result_fields":[{"name":"animation","required":true,"component":"asset_summary"},{"name":"structure","required":true,"component":"timeline_structure"}]},
    {"capability_id":"animation.subject.pose.update","descriptor_id":"uai.operation.animation.subject.pose.update","operation_kind":"operation","state_planes":["authoring_memory"],"required_facets":["world"],"oracle_kind":"same_component_pose_or_morph_readback","input_fields":[{"name":"evaluation_point","required":true,"component":"temporal_point"},{"name":"pose","required":true,"schema":{"type":"array","items":{"component":"pose_item"},"minItems":1}},{"name":"pose_frame","required":true,"component":"frame_3d"},{"name":"weight","required":false,"schema":{"type":"number","minimum":0,"maximum":1}}],"result_fields":[{"name":"evaluation_point","required":true,"component":"temporal_point"},{"name":"pose","required":true,"schema":{"type":"array","items":{"component":"pose_item"},"minItems":1}},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"animation.subject.morph_weights.update","descriptor_id":"uai.operation.animation.subject.morph_weights.update","operation_kind":"operation","state_planes":["authoring_memory"],"required_facets":["world"],"oracle_kind":"same_component_pose_or_morph_readback","input_fields":[{"name":"evaluation_point","required":true,"component":"temporal_point"},{"name":"morph_weights","required":true,"schema":{"type":"array","items":{"component":"morph_weight"},"minItems":1}}],"result_fields":[{"name":"evaluation_point","required":true,"component":"temporal_point"},{"name":"morph_weights","required":true,"schema":{"type":"array","items":{"component":"morph_weight"},"minItems":1}},{"name":"verification","required":true,"component":"verification"}]},
    {"capability_id":"animation.ik_probe_candidate.apply","descriptor_id":"uai.operation.animation.ik_probe_candidate.apply","operation_kind":"operation","state_planes":["authoring_memory","persisted"],"required_facets":[],"oracle_kind":"independent_sequence_and_rig_key_readback","input_fields":[{"name":"rig_ref","required":true,"component":"target_ref"},{"name":"goal_ref","required":true,"component":"target_ref"},{"name":"candidate","required":true,"component":"transform_3d"},{"name":"authored_time","required":true,"component":"temporal_point"},{"name":"expected_revision","required":true,"component":"revision"}],"result_fields":[{"name":"candidate","required":true,"component":"transform_3d"},{"name":"authored_time","required":true,"component":"temporal_point"},{"name":"observed_revision","required":true,"component":"revision"},{"name":"verification","required":true,"component":"verification"}]}
  ],
  "chains": [
    {"capability_id":"animation.sequence.read","family_id":"temporal_read","typed_operation_id":"timeline_animation.sequence.read","binding_id":"timeline_animation.sequence.read","typed_request_id":"timeline_animation.sequence.read.request.v1","typed_result_id":"timeline_animation.sequence.read.result.v1","readback_id":"timeline_animation.sequence.readback","settle_barrier":"none"},
    {"capability_id":"animation.sequence.root_motion.analyze","family_id":"temporal_read","typed_operation_id":"timeline_animation.sequence.ana)UAITAD1";
static constexpr ANSICHAR GTimelineAnimationDescriptorSpecJson2[] = R"UAITAD2(lyze_root_motion","binding_id":"timeline_animation.sequence.analyze_root_motion","typed_request_id":"timeline_animation.sequence.root_motion.request.v1","typed_result_id":"timeline_animation.sequence.root_motion.result.v1","readback_id":"timeline_animation.sequence.root_motion.readback","settle_barrier":"none"},
    {"capability_id":"animation.sequence.pose.sample","family_id":"temporal_read","typed_operation_id":"timeline_animation.sequence.sample_pose","binding_id":"timeline_animation.sequence.sample_pose","typed_request_id":"timeline_animation.sequence.pose_sample.request.v1","typed_result_id":"timeline_animation.sequence.pose_sample.result.v1","readback_id":"timeline_animation.sequence.pose_sample.readback","settle_barrier":"none"},
    {"capability_id":"animation.blend_space.sample","family_id":"temporal_read","typed_operation_id":"timeline_animation.blend_space.sample","binding_id":"timeline_animation.blend_space.sample","typed_request_id":"timeline_animation.blend_space.sample.request.v1","typed_result_id":"timeline_animation.blend_space.sample.result.v1","readback_id":"timeline_animation.blend_space.sample.readback","settle_barrier":"none"},
    {"capability_id":"animation.sequence.bone_tracks.replace","family_id":"animation_keyed_data","typed_operation_id":"timeline_animation.sequence.replace_bone_tracks","binding_id":"timeline_animation.sequence.replace_bone_tracks","typed_request_id":"timeline_animation.sequence.bone_tracks_replace.request.v1","typed_result_id":"timeline_animation.sequence.bone_tracks_replace.result.v1","readback_id":"timeline_animation.sequence.bone_tracks.readback","settle_barrier":"animation_sequence_rebuild_notify_compress_settle"},
    {"capability_id":"animation.sequence.curves.update","family_id":"animation_keyed_data","typed_operation_id":"timeline_animation.sequence.update_curves","binding_id":"timeline_animation.sequence.update_curves","typed_request_id":"timeline_animation.sequence.curves_update.request.v1","typed_result_id":"timeline_animation.sequence.curves_update.result.v1","readback_id":"timeline_animation.sequence.curves.readback","settle_barrier":"animation_sequence_rebuild_notify_compress_settle"},
    {"capability_id":"animation.sequence.metadata.update","family_id":"animation_keyed_data","typed_operation_id":"timeline_animation.sequence.update_metadata","binding_id":"timeline_animation.sequence.update_metadata","typed_request_id":"timeline_animation.sequence.metadata_update.request.v1","typed_result_id":"timeline_animation.sequence.metadata_update.result.v1","readback_id":"timeline_animation.sequence.metadata.readback","settle_barrier":"animation_sequence_rebuild_notify_compress_settle"},
    {"capability_id":"animation.sequence.events.update","family_id":"animation_keyed_data","typed_operation_id":"timeline_animation.sequence.update_events","binding_id":"timeline_animation.sequence.update_events","typed_request_id":"timeline_animation.sequence.events_update.request.v1","typed_result_id":"timeline_animation.sequence.events_update.result.v1","readback_id":"timeline_animation.sequence.events.readback","settle_barrier":"animation_sequence_rebuild_notify_compress_settle"},
    {"capability_id":"animation.montage.read","family_id":"temporal_read","typed_operation_id":"timeline_animation.montage.read","binding_id":"timeline_animation.montage.read","typed_request_id":"timeline_animation.montage.read.request.v1","typed_result_id":"timeline_animation.montage.read.result.v1","readback_id":"timeline_animation.montage.readback","settle_barrier":"none"},
    {"capability_id":"animation.montage.create","family_id":"timeline_structure","typed_operation_id":"timeline_animation.montage.create","binding_id":"timeline_animation.montage.create","typed_request_id":"timeline_animation.montage.create.request.v1","typed_result_id":"timeline_animation.montage.create.result.v1","readback_id":"timeline_animation.montage.structure.readback","settle_barrier":"montage_refresh_and_relink"},
    {"capability_id":"animation.montage.structure.replace","family_id":"timeline_structure","typed_operation_id":"timeline_animation.montage.replace_structure","binding_id":"timeline_animation.montage.replace_structure","typed_request_id":"timeline_animation.montage.structure_replace.request.v1","typed_result_id":"timeline_animation.montage.structure_replace.result.v1","readback_id":"timeline_animation.montage.structure.readback","settle_barrier":"montage_refresh_and_relink"},
    {"capability_id":"animation.skeleton.slots.read","family_id":"temporal_read","typed_operation_id":"timeline_animation.skeleton.read_slots","binding_id":"timeline_animation.skeleton.read_slots","typed_request_id":"timeline_animation.skeleton.slots_read.request.v1","typed_result_id":"timeline_animation.skeleton.slots_read.result.v1","readback_id":"timeline_animation.skeleton.slots.readback","settle_barrier":"none"},
    {"capability_id":"timeline.sequence.read","family_id":"temporal_read","typed_operation_id":"timeline_animation.level_sequence.read","binding_id":"timeline_animation.level_sequence.read","typed_request_id":"timeline_animation.level_sequence.read.request.v1","typed_result_id":"timeline_animation.level_sequence.read.result.v1","readback_id":"timeline_animation.level_sequence.readback","settle_barrier":"none"},
    {"capability_id":"timeline.sequence.create","family_id":"timeline_structure","typed_operation_id":"timeline_animation.level_sequence.create","binding_id":"timeline_animation.level_sequence.create","typed_request_id":"timeline_animation.level_sequence.create.request.v1","typed_result_id":"timeline_animation.level_sequence.create.result.v1","readback_id":"timeline_animation.level_sequence.structure.readback","settle_barrier":"movie_scene_compile_and_evaluation_template_settle"},
    {"capability_id":"timeline.sequence.structure.replace","family_id":"timeline_structure","typed_operation_id":"timeline_animation.level_sequence.replace_structure","binding_id":"timeline_animation.level_sequence.replace_structure","typed_request_id":"timeline_animation.level_sequence.structure_replace.request.v1","typed_result_id":"timeline_animation.level_sequence.structure_replace.result.v1","readback_id":"timeline_animation.level_sequence.structure.readback","settle_barrier":"movie_scene_compile_and_evaluation_template_settle"},
    {"capability_id":"timeline.widget_animation.read","family_id":"temporal_read","typed_operation_id":"timeline_animation.widget_animation.read","binding_id":"timeline_animation.widget_animation.read","typed_request_id":"timeline_animation.widget_animation.read.request.v1","typed_result_id":"timeline_animation.widget_animation.read.result.v1","readback_id":"timeline_animation.widget_animation.readback","settle_barrier":"none"},
    {"capability_id":"animation.subject.pose.update","family_id":"subject_animation_state","typed_operation_id":"timeline_animation.subject.update_pose","binding_id":"timeline_animation.subject.update_pose","typed_request_id":"timeline_animation.subject.pose_update.request.v1","typed_result_id":"timeline_animation.subject.pose_update.result.v1","readback_id":"timeline_animation.subject.pose.readback","settle_barrier":"skeletal_component_pose_refresh"},
    {"capability_id":"animation.subject.morph_weights.update","family_id":"subject_animation_state","typed_operation_id":"timeline_animation.subject.update_morph_weights","binding_id":"timeline_animation.subject.update_morph_weights","typed_request_id":"timeline_animation.subject.morph_weights_update.request.v1","typed_result_id":"timeline_animation.subject.morph_weights_update.result.v1","readback_id":"timeline_animation.subject.morph_weights.readback","settle_barrier":"skeletal_component_morph_refresh"},
    {"capability_id":"animation.ik_probe_candidate.apply","family_id":"timeline_rig_candidate","typed_operation_id":"timeline_animation.ik_probe.apply_candidate","binding_id":"timeline_animation.ik_probe.apply_candidate","typed_request_id":"timeline_animation.ik_probe.candidate_apply.request.v1","typed_result_id":"timeline_animation.ik_probe.candidate_apply.result.v1","readback_id":"timeline_animation.ik_probe.candidate.readback","settle_barrier":"control_rig_sequence_evaluation_settle"}
  ]
}
)UAITAD2";
static constexpr int32 GTimelineAnimationDescriptorSpecChunkCount = 3;
