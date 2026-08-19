# T45 Viewport/graph control authority

T45 owns exactly fourteen current commands. They are contributed by the typed
`viewport-graph-control` capability and enter the shared Query/Operation
composition; no task-specific route, SDK projection, compatibility alias, or
persisted graph-layout owner remains here.

The default catalog exposes the same identities as
`client.graph.view_read`, `client.graph.view_update`,
`client.viewport.actor_pick`, `client.viewport.actor_select`,
`client.viewport.camera_frame`, `client.viewport.instance_list`,
`client.viewport.pixel_scale_read`, `client.viewport.ray_deproject`,
`client.viewport.ray_trace`, `client.viewport.state_read`,
`client.viewport.state_restore`, `client.viewport.state_update`,
`client.viewport.view_mode_list`, and `client.viewport.warning_list`.

The read-only commands are `graph.view.read`, `viewport.actor.pick`,
`viewport.instance.list`, `viewport.pixel_scale.read`,
`viewport.ray.deproject`, `viewport.ray.trace`, `viewport.state.read`,
`viewport.view_mode.list`, and `viewport.warning.list`. The recoverable
Editor-transient operations are `graph.view.update`, `viewport.actor.select`,
`viewport.camera.frame`, `viewport.state.restore`, and
`viewport.state.update`. `graph.layout.apply` is a persisted asset-layout
capability owned only by T62 and is not implemented or routed by T45.

All calls retain an explicit target and frozen context. Viewport operations use
an exact `viewport_identity`; graph view operations use the exact `UEdGraph`
path as both target and `asset_editor_identity`. Spatial values carry frame,
dimension, unit, basis evidence, observation time, and conversion evidence.
Camera framing accepts exactly one selector: `subject_refs[]`, `folder_path`,
or `use_current_selection=true`. Its policy explicitly carries padding,
minimum/maximum distance, preserve rotation, collision-aware, look-at, and
auto-fallback behavior.

`graph.view.read` exposes location, zoom, and the actual CAS revision as the
snapshot roles `graph_view.revision.source_id`,
`graph_view.revision.generation`, and `graph_view.revision.sequence`.
`graph.view.update` must consume that exact revision. `viewport.state.read`
likewise exposes all camera, show-flag, interaction, and revision fields needed
to construct a complete `viewport.state.restore` snapshot. Stale revisions,
ambiguous selectors, unsupported frames, or missing required hits fail before
mutation.

Realtime updates preserve other Editor systems' overrides. UAI first updates
the underlying viewport state; when another override conflicts, it layers one
named UAI override. The next UAI update or restore removes only that named
override. Graph view, camera, selection, view mode, realtime, Game View, and
interaction changes are restored and independently queried in the public
product test; they are never represented as persisted asset state.

The nineteen frozen historical identities map as follows:

| Historical identity | Exact successor |
| --- | --- |
| `level_set_selection` | `viewport.actor.select` |
| `viewport_deproject_screen_to_world` | `viewport.ray.deproject` |
| `viewport_focus_actor`, `viewport_focus_actor_safe` | `viewport.camera.frame` |
| `viewport_frame_actor` | `viewport.camera.frame` + `viewport.state.read` |
| `viewport_frame_actors`, `viewport_frame_folder`, `viewport_frame_selection` | `viewport.camera.frame` |
| `viewport_get_camera`, `viewport_get_editor_interaction_state` | `viewport.state.read` |
| `viewport_get_warnings` | `viewport.warning.list` |
| `viewport_list_view_modes` | `viewport.view_mode.list` |
| `viewport_pick_actor_at_screen` | `viewport.actor.pick` |
| `viewport_restore_editor_interaction_state` | `viewport.instance.list` + per-viewport `viewport.state.restore` + `viewport.state.read` |
| `viewport_select_actor_at_screen` | `viewport.actor.select` |
| `viewport_set_camera`, `viewport_set_game_view`, `viewport_set_realtime` | `viewport.state.update` + `viewport.state.read` |
| `viewport_trace_screen_point` | `viewport.ray.trace` |

The authoritative row-by-row frozen contract, routing policy, semantic
coverage, product receipt requirements, and old-ID exit facts are in
`successors/t45_viewport_graph_control.json`. Discovery and use remain through
`command_search`, `command_detail`, the default named SDK methods, and
`uai-cli`; the historical IDs are absent from manifest, SDK, CLI, and alias
routing.
