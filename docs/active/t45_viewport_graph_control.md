# T45 Viewport/graph control authority

T45 owns the current viewport and transient graph-view commands listed below.
They are contributed by the typed
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

Viewport calls retain an explicit frozen context with a readable `viewport`;
methods declaring a target use that same viewport ref. Catalog-only methods
such as view-mode and warning queries have no positional target. Graph view operations use the readable `UEdGraph`
path as both target and `asset_editor`. Spatial values carry frame,
dimension, unit, basis evidence, observation time, and conversion evidence.
Camera framing accepts exactly one selector: `subject_refs[]`, `folder_path`,
`use_current_selection=true`, `world_point`, or `world_bounds`. Subject refs
may identify an Actor or a bounded primitive component. Points and the
`world_bounds.min/max` pair are spatial values in `unreal.world`. The framing
policy explicitly carries padding, minimum/maximum distance, preserve rotation,
collision-aware, look-at, and auto-fallback behavior.

When `viewport.state.update` includes `camera_transform`, it also includes
`camera_transform_mode`. `absolute` accepts only an `unreal.world` transform.
`delta` accepts an `unreal.world` delta or a `viewport.camera.local` delta;
local translation follows the current camera axes and local rotation composes
after the current rotation. A viewport camera has no scale, so every supplied
transform must carry unit scale. `viewport.state.restore` consumes the business
state returned directly in `viewport.state.read.payload` and applies its camera state as an
absolute restore. For restore, the WorldSpatial owner captures the live viewport revision
internally during preflight and rechecks it before the first mutation; callers
never copy or submit that machine revision.

Update and restore resolve camera transforms, view modes, show flags and
interaction modes before applying any viewport setter. An invalid later field
therefore fails with confirmed no mutation instead of leaving earlier fields
changed. `show_flags` replaces the enabled flags within the supported subset
`Lighting` and `StaticMeshes`; flags outside that subset are unchanged. Native
read/apply and the public schema enum consume the same supported list.

View-mode reads return a replayable canonical mode ID from the same owner as
the catalog and resolver. Aliases are verified against the native mode, not
their spelling. Family IDs retain the selected visualization mode; an explicit
`family_none` preserves an unset native selector rather than selecting that
family's default. Buffer's unset selector is `buffer_overview`. A simple mode
request changes only the native mode and retains an existing family selector.

`viewport.instance.list` returns each exact `viewport_ref`, its camera state,
and a `viewport_type` of `perspective`, `orthographic_xy`, `orthographic_xz`,
`orthographic_yz`, `orthographic_freelook`, or `unknown`. Projection type is a
behavioral input, not interchangeable viewport metadata. A consumer whose
framing or image oracle assumes perspective FOV must first select and retain a
same-world `perspective` viewport, then use that exact identity for state read,
frame, capture, restore, and final readback. Current real-content product
evidence covers that perspective path only; the expected behavior, rejection
boundary, or support policy for non-perspective `viewport.camera.frame` remains
unconfirmed and must not be inferred from the perspective result.

`graph.view.read` exposes the readable graph location and zoom needed for an
update or restore. `viewport.state.read` likewise exposes camera, show-flag and
interaction business state. Its public record contains `viewport_ref`, nested
`camera` and `interaction`, realtime fields, `game_view` and `show_flags`; it
can be passed unchanged as `viewport.state.restore`'s `input.snapshot`.
State read/update/restore, warning and view-mode queries use the live viewport
client even when its render surface has zero size (for example while minimized).
Pixel projection, ray-based picking/selection and camera framing still require
a valid scene and nonzero render dimensions.
Sibling canonical entries are assembled by the common query projector, while
native observation and restore retain one WorldSpatial owner. `graph.view.update` and `viewport.state.restore`
resolve their exact graph or viewport and freeze the live revision inside the
same server operation, then recheck it before mutation. Callers do not read,
store or return revisions. A target that changes after preflight, ambiguous
selectors, unsupported frames, or missing required hits fail before mutation.

Interaction `widget_mode` is read from the live viewport client. Both state
update and restore use the same dispatch: level viewports write their mode
manager; Blueprint Components uses its viewport client's virtual setter because
that client owns a separate mode value. Reapplying the currently observed mode
is a no-op; a change still requires no active mode-tools tracking interaction.
Availability of a transform widget in an active editing mode is a UI capability,
not a prerequisite for restoring level viewport state. Applying state never
activates an editing mode or removes its overrides; independent readback reports
any remaining difference.

Blueprint Components reports the effective mode as `none` when it has no
eligible component selection or otherwise hides the widget. UE does not expose
that client's hidden configured mode through its public getter. Restoring an
observed `none` to an already hidden widget therefore preserves its configuration;
it does not reinterpret invisibility as a request to clear the hidden mode.
An explicit mode update uses the native setter, but a still-hidden widget is
reported as a partial mismatch with the requested and observed modes. That
snapshot does not claim to capture or restore unexposed native preferences.

Realtime update/restore changes the underlying preference and preserves other
Editor systems' overrides; it removes the legacy named UAI override rather than
competing for override priority. Readback exposes `realtime_configured` (the
preference), `realtime` (sustained effective state, excluding temporary redraw
frames), and `realtime_override_reason` (empty when no override is active).
Verification and revision tracking use the same sustained state reader. If an
override prevents the requested state, verification reports partial/mismatch
with `world_spatial_viewport_realtime_overridden`, not verified/complete; the
configured preference remains applied. The warning carries the active override
reason, `property_path`, `expected_value` and `actual_value` directly from the
comparison. Other requested viewport fields that differ retain their own
diagnostics; a realtime override does not replace those differences. A sole
difference is returned directly, while multiple differences use `diagnostics`.
The compact verification projection remains a summary; `warnings` carries the
complete diagnostic without repeating it in that projection.
Restore accepts the captured `realtime_configured` value in its saved state;
when present it takes precedence over the effective `realtime` value, so saving
a snapshot under an external override does not overwrite the user's preference.
Older saved states containing only `realtime` remain accepted.
UE re-applies Background Process=false every tick while unfocused and background
CPU throttling is enabled. Focusing Editor removes that override; UAI does not
silently change global performance settings. Active PIE flows and viewport/asset
capture jobs temporarily share UE's native CPU-throttling exemption and release
it through their existing terminal/restore paths. This controls Editor tick rate;
it does not persist a realtime preference or remove another system's rendering
override. PIE, remote desktop and editing-mode overrides retain their native
viewport behavior. Graph view, camera, selection, view mode, realtime, Game View, and
interaction changes are restored and independently queried in the public
product test; they are never represented as persisted asset state.

`viewport.ray.deproject` and `viewport.ray.trace` accept optional
`input.cold_start_render_size={width,height}` (integer pixels, 1..4096). When the
bound Editor viewport has zero extent, this explicitly defines the screen-pixel
basis and temporarily initializes the same scene viewport used by offscreen
capture. Positive existing dimensions take precedence. The query restores the
fixed-size policy on every return path and never restores, activates or resizes
the native window. Omission preserves the existing zero-extent diagnostic. Use
the capture receipt's dimensions when tracing a previously captured image; this
does not prove a collider is the visible surface when their geometry differs.

The frozen historical identities map as follows:

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

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).
