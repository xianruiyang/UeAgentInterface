# T44 Capture/media operation authority

The only current capture entries are `capture.image.execute`,
`capture.media.execute`, and `capture.render.execute`. They are discovered with
`client.query.command_search(...)` and `client.query.command_usage([...])` using
exact command IDs; the same public command contract is used
by `client.capture.image_execute(...)`, `client.capture.media_execute(...)`,
`client.capture.render_execute(...)`, and `ueagentinterfacecmd exec`. Historical screenshot
IDs are non-executable mechanics identities, not aliases or alternate routes.

Every request has the same envelope:

- `subject_ref` is the exact viewport, asset, actor, media source, or render
  resource identity.
- `policy_id` chooses one catalogued behavior. It is never inferred from the
  subject, old command name, or current Editor state.
- `parameters` carries the selected policy's documented fields.
  `output_file`, when supported, is the requested final
  artifact path; native capture first writes to operation-owned staging and the
  artifact publisher refuses to overwrite a file not owned by the same owner.

For `capture.image.execute`, use
`ueagentinterfacecmd describe capture.image.execute --parameter policy_id`
to inspect the current public policy enum. The public `parameters` descriptor is an
object without a nested schema (`schema_available: false`); the internal
`x-uai-policy-contracts` metadata is not exposed by that CLI/SDK descriptor.
Use the policy contract below and, for native asset preview, the exported `preview/`
fields. SDK invocation and operation examples live in the
[CMD guide](../../../../UeAgentInterfaceCMD/docs/active/t44_capture_media_operation.md).

Level viewport policies (`viewport`, `pcg-viewport`, `viewport-buffer`,
`viewport-sequence`, and `transform-candidates`) prepare the selected viewport's
existing native window and layout before framing or reading pixels. A minimized
or hidden window may be restored or shown. Callers do not need a separate window
preparation command. Preparation preserves the viewport selection and does not
invent capture dimensions; a viewport that remains non-positive reports its
actual size. Once the window and viewport are ready, later sequence samples do
not repeat window activation or layout warmup. Asset-editor policies retain
their own documented window-mutation permissions.

## Transform-candidate visual preview

`policy:image-capture:transform-candidates/v1` is a current non-historical
policy. Its subject must be
`capture-subject:actor:<exact-actor-object-path>`, and `parameters.candidates`
contains one or more labeled transforms. Each transform may supply location, rotator,
scale, and `relative_to_original`; omitted transform components retain the
corresponding original component.

The policy captures the original plus every temporary candidate from the active
Level viewport and publishes one operation-owned frame sheet. It neither scores nor selects candidates and never
persists the temporary transforms. The mechanics restores Actor transform,
package dirty state, Actor selection, and any temporary viewport realtime
override on success or failure; success fails closed unless each restoration is
observed. After visual comparison, edit the Actor's declared transform fields in
its Level workspace and persist the selected value with `workspace.import_changes()`.
The managed workspace owns revision and identity; see [T26](t26_level_world_domain.md).

## Asset preview workspace capture

`policy:image-capture:asset-preview/v1` captures the selected asset's actual native
preview. Use `capture-subject:asset:<object-path>` and `parameters={"mode":"single"}`
or `{"mode":"sequence"}`; `target` optionally selects a named target from the
exported `preview/index.hjson`. Camera, environment and capture settings belong to
the asset workspace and are applied with `workspace.import_changes()`. Capture
does not import pending local edits or accept a second copy of these settings.

The current native adapter determines available groups and actions. Three-dimensional
previews use their own Editor viewport; Texture previews use their own image viewport.
Source-texture extraction and graph screenshots remain different observations.
Capture receipts distinguish requested positions from actual sampled/evaluated positions.
Both single and sequence consume the applied sampling plan. Native frame units use the
preview's own frame rate; Continuous preserves normal playback intervals from the native
range start, while Timeline directly evaluates a position and does not imply event history.
Niagara uses Realtime or Continuous. Actual frame deviation and simulation age are reported;
single-image `capture.sample` preserves exposure-time state separately from restored `end_preview_state`.
Capture-owned play/pause transitions preserve the configured playback rate. Continuous
restoration completes native play/pause before restoring the prior rate; an independent
rate change made during capture is retained. Explicit user playback actions keep UE's
native semantics.
`capture.RenderWidth/RenderHeight` temporarily set the same native scene viewport's
render extent. Both zero retain native sizing; one zero preserves its current aspect
ratio. Capture restores the original extent and fixed-size flag without resizing the
Editor window. Native GPU dimension limits still apply, so use `native_width/height`
in the frame receipt as the actual sample size. These fields are not exported for
Widget Designer previews; those use their native `widget` size settings.
`capture.Width/Height` only resize the output image. Frame receipts distinguish
`requested_render_width/height`, `native_width/height`, `configured_width/height`
and final `width/height`; output interpolation is not additional rendered detail.
Realtime readback distinguishes the configured preference, sustained effective
state and native override reason. The existing `realtime_before_capture` also
includes one-off requested redraws; it is not the stored preference.
Native viewport sampling temporarily keeps the target eligible for Editor ticks
and requests short realtime frame windows, including when the Editor is in the
background. It restores its visibility delegate and realtime overrides without
changing global performance settings. `realtime_state_restored` compares the
configured and sustained states; outstanding one-off frames expire naturally.
Materialize receipt artifacts before releasing the operation.

## Image policy catalog

Image size, frame count, frame interval, warmup, padding and label scale follow
the caller's positive/nonnegative values. A requested `max_size` scales the
output to that caller-selected size. Pixel
buffers must remain representable by the native array and image types; format
quality remains a percentage. Defaults are choices for omitted parameters,
not maximum permitted values.

| Current policy on `capture.image.execute` | Observation and parameter route |
| --- | --- |
| `policy:image-capture:asset-preview/v1` | native asset preview, including animation and Niagara; `single`/`sequence` trigger with configuration in workspace `preview/` |
| `policy:image-capture:anim-blueprint/v1` | AnimBlueprint graph/window/viewport fields |
| `policy:image-capture:behavior-tree/v1` | Behavior Tree offscreen widget fields |
| `policy:image-capture:blueprint/v1` | Blueprint graph/window/viewport and restore fields |
| `policy:image-capture:control-rig/v1` | Control Rig capture target fields |
| `policy:image-capture:metasound/v1` | MetaSound graph image fields |
| `policy:image-capture:pcg-viewport/v1` | Actor framing and viewport image fields |
| `policy:image-capture:viewport/v1` | active Level viewport; view-mode, visualization, warmup, restore, format and output fields |
| `policy:image-capture:viewport-buffer/v1` | viewport buffer/depth/invert/format/output fields |
| `policy:image-capture:viewport-sequence/v1` | viewport frame-sheet sampling fields |
| `policy:image-capture:level-sequence/v1` | Level Sequence time/frame/sequence-sheet fields |
| `policy:image-capture:sound-cue/v1` | SoundCue graph image fields |
| `policy:image-capture:state-tree/v1` | StateTree offscreen widget fields |
| `policy:image-capture:texture/v1` | source texture mip/slice/format/statistics fields; distinct from native preview |
| `policy:image-capture:transform-candidates/v1` | temporary Actor transform comparison described above |
| `policy:image-capture:umg/v1` | WidgetBlueprint offscreen size/crop/region fields; optional `use_project_dpi_scale` (default false) applies the project viewport DPI rule at the requested capture size |

Every image policy publishes the available public capture metadata in
`latest_signal.payload.capture_receipt`. Fields retain their policy-specific meaning;
their presence does not imply that all policies expose the same rendering state.

Level Sequence capture evaluates the requested time and renders an independent final-color
camera surface. An active sequence camera supplies its native camera view, including projection
and post-processing. Viewport fallback supplies camera geometry; it does not copy the Editor
backbuffer, show flags or exposure history. The surface keeps a native view state for eye
adaptation, with a camera cut at each explicit sequence-time jump. This is not a higher-resolution
version of `viewport/v1`, and cross-policy images are not a controlled exposure comparison.
The receipt reports the render path, dimensions, frame times, camera sources, projection,
post-process blend and exposure-history scope. A frame with `uses_primary_artifact: true`
uses the primary image; otherwise `raw_artifact_index`, when present, indexes
`additional_artifact_refs`. Sheet-only samples have frame metadata without individual artifacts.
Internal output file paths are not part of the public receipt.

UMG capture renders design-time widget state and does not execute runtime Construct or input. It reuses a current generated class; only a missing class or a dirty/error Blueprint needs compilation. Repeated capture of an unchanged asset does not recompile its class or replace live PIE widgets. Its receipt records `dpi_scale`, `use_project_dpi_scale`, `widget_execution_mode`, canvas and crop separately from output resizing. Validate runtime interaction in PIE.

Design-time is the native Designer display contract, not a runtime screenshot:
in UE 5.6 the Designer's visibility setting controls visibility, so a widget whose
runtime `Visibility` is `Hidden` or `Collapsed` can still appear. This policy does not
execute PreConstruct or Construct. Use the PIE output surface for runtime visibility,
event-driven state and final interactive layout; keep Designer captures for the authored
design view. A difference between those two views alone does not show that the saved
WidgetBlueprint lost its property values.

Read a registered policy's exact request contract with
`ueagentinterfacecmd describe capture.image.execute --variant policy:image-capture:umg/v1`.
Add `--parameter parameters` to read only its parameter object, or `--pointer` to select a schema fragment.
The base command description lists available variant IDs; these contracts come from the plugin registry through the generated manifest and SDK.

For UMG, `capture_width` and `capture_height` are positive canvas dimensions in pixels
(defaults 1920 and 1080). Use `crop_mode: "full_canvas"` and omit `max_size` to retain
that complete canvas in the artifact. The default `auto_bounds` crops transparent margins;
`bounds_padding` adds nonnegative pixels and `bounds_alpha_threshold` selects the alpha cutoff (0–255).
For a pixel region use `crop_mode: "manual_region"` with
`region: {"x": 0, "y": 0, "width": 640, "height": 360}`. Coordinates originate at the
canvas's top left; the rectangle is clipped to the canvas and must retain positive area.
Supplying a region with the default crop mode selects manual cropping. `crop` is an accepted
alias for `region`; coordinate aliases are listed in the generated schema.
`format` is png (default), jpg/jpeg or webp; `quality` defaults to 90 (1–100).
`max_size`, when supplied, resizes the cropped output; it does not set the layout canvas.
`use_project_dpi_scale` defaults to false; true applies the project DPI rule at the requested canvas size.
Inspect the receipt's `canvas`, `crop`, `raw_width/raw_height` and final `width/height`
separately. The `asset-preview/v1` policy captures the configured native editor preview;
it is not the UMG offscreen canvas or a runtime interaction proof.

Slate widget rendering and pixel readback share `Core/EditorCapture/UeAgentSlateWidgetCapture`:
the render target stays strongly referenced through readback, and sRGB encoding occurs once.
UMG design-time, PIE output and Blueprint editor captures use this owner; the existing
Animation editor window helper also delegates to it. This does not turn the animation
sequence viewport policy into an editor-window capture. Alpha remains an output policy:
opaque scene/editor output uses the common image encoder's opaque-alpha option, while
UMG transparent output keeps its alpha.

The asset and actor subject forms are `capture-subject:asset:<object-path>` and
`capture-subject:actor:<actor-name-or-label>`. Supplying `asset_path` or `actor`
inside `parameters` is allowed only when it identifies the same subject. Actor
framing aliases (`actor`, `actor_name`, `actor_label`, `id`) are resolved in the
current Editor world before framing; conflicting aliases fail without moving the
camera. Duplicate names/labels require an exact object path or scoped discovery
name. This rule also applies to transform-candidate probes.

All image policies require an Editor process that can render. When
`FApp::CanEverRender()` is false, including `-NullRHI`, the operation fails
closed with `capture_image_rhi_unavailable:<policy_id>` before parameter
projection, screenshot mechanics, or asset-editor opening. There is no
non-rendering fallback and no image artifact. Use a rendering-capable owned
worker for image-product verification; NullRHI is valid only for the explicit
failure contract and for capture mechanics that do not require rendering.

## Media producers

`policy:media-recording:submix-wav/v1` accepts `media-source:audio-submix:master`
or an exact SoundSubmix object path after that prefix. An unresolved explicit
target fails; it does not select the master mix. `expected_duration` is in seconds.
Each operation owns a native submix buffer listener, including silent samples;
concurrent captures do not reset UE's shared recorder or each other. Cancellation
unregisters only that listener and exports samples already recorded. No native
samples reports `capture_media_recording_empty`, rather than publishing an empty WAV.
Normal completion uses the actual audio sample rate and frame count for the requested
duration; Editor ticker delays do not shorten the recording or pad missing audio.
The current Editor world selects the audio device at native start; this command does
not select a PIE world. Editor capture/render and their execution context resolve the
current Editor world for each new request, including after a map change.

`policy:media-recording:sound-preview-wav/v1` accepts `media-source:sound:<SoundWave-path>`
and exports imported PCM, without starting or stopping live recording. Both policies
use an operation-owned native WAV writer. Publication waits for its completed write,
including failure; file appearance alone is not completion. The common receipt and
release flow below applies to both.

## Lifecycle and evidence

The three commands share the T32 process-owned session registry while each
public request creates a new plan, authorization, service, and invocation
context. A successful start returns a handle in `start_result.operation`; it is not an artifact
terminal result. Advance a finite capture with `handle.resume()` until terminal;
`handle.status()` only observes. Read the receipt and materialize its artifacts before
`handle.release()`. Cancel and recovery details belong to the [T32 SDK flow](../../../../UeAgentInterfaceCMD/docs/active/t32_long_running.md).
After release, a cached receipt or an already materialized file remains usable, but
the released handle cannot query the server or advance the operation. Unknown state
does not authorize replay of the initiating command.
Cancellation remains pending until native preview restoration completes on the game
thread. Staging cleanup failures are reported separately from capture/publication
failures and do not erase already published artifacts; a failed release is not a
successful resource release.
