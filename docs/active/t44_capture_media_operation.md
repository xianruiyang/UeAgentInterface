# T44 Capture/media operation authority

The only current capture entries are `capture.image.execute`,
`capture.media.execute`, and `capture.render.execute`. They are discovered with
`command_search` and `command_detail`; the exact manifest contract is also used
by `client.capture.image_execute(...)`, `client.capture.media_execute(...)`,
`client.capture.render_execute(...)`, and `uai-cli exec`. Historical screenshot
IDs are non-executable mechanics identities, not aliases or alternate routes.

Every request has the same envelope:

- `subject_ref` is the exact viewport, asset, actor, media source, or render
  resource identity.
- `policy_id` chooses one catalogued behavior. It is never inferred from the
  subject, old command name, or current Editor state.
- `parameters` carries the fields listed for that policy by
  `command_detail`. Historical parameter names retain their meaning inside the
  selected policy. `output_file`, when supported, is the requested final
  artifact path; native capture first writes to operation-owned staging and the
  artifact publisher refuses to overwrite a file not owned by the same owner.

## Image policy catalog

| Retired historical identity | Current policy on `capture.image.execute` | Subject and parameter route |
| --- | --- | --- |
| `anim_blueprint_screenshot` | `policy:image-capture:anim-blueprint/v1` | asset identity in `subject_ref`; graph/window/viewport options in `parameters` |
| `anim_sequence_screenshot` | `policy:image-capture:anim-sequence/v1` | animation asset in `subject_ref`; time/frame/sequence-sheet fields in `parameters` |
| `behavior_tree_screenshot` | `policy:image-capture:behavior-tree/v1` | behavior tree asset in `subject_ref`; offscreen widget options in `parameters` |
| `blueprint_screenshot` | `policy:image-capture:blueprint/v1` | blueprint asset in `subject_ref`; graph/window/viewport and restore fields in `parameters` |
| `control_rig_screenshot` | `policy:image-capture:control-rig/v1` | control rig asset in `subject_ref`; capture target fields in `parameters` |
| `metasound_screenshot` | `policy:image-capture:metasound/v1` | MetaSound asset in `subject_ref`; graph image fields in `parameters` |
| `niagara_screenshot` | `policy:image-capture:niagara/v1` | Niagara asset in `subject_ref`; preview camera/tick/frame-sheet fields in `parameters` |
| `pcg_screenshot` | `policy:image-capture:pcg-viewport/v1` | actor identity in `subject_ref`; framing and viewport image fields in `parameters` |
| `screenshot_viewport` | `policy:image-capture:viewport/v1` | `capture-subject:viewport:active`; view-mode, visualization, warmup, tick, restore, format and output fields in `parameters` |
| `screenshot_viewport_buffer` | `policy:image-capture:viewport-buffer/v1` | `capture-subject:viewport:active`; buffer/depth/invert/format/output fields in `parameters` |
| `sequence_screenshot` | `policy:image-capture:level-sequence/v1` | level sequence asset in `subject_ref`; time/frame/sequence-sheet fields in `parameters` |
| `sound_cue_screenshot` | `policy:image-capture:sound-cue/v1` | SoundCue asset in `subject_ref`; graph image fields in `parameters` |
| `state_tree_screenshot` | `policy:image-capture:state-tree/v1` | StateTree asset in `subject_ref`; offscreen widget options in `parameters` |
| `texture_screenshot` | `policy:image-capture:texture/v1` | texture asset in `subject_ref`; mip/slice/format/statistics fields in `parameters` |
| `umg_screenshot` | `policy:image-capture:umg/v1` | WidgetBlueprint asset in `subject_ref`; offscreen size/crop/region fields in `parameters` |

The asset and actor subject forms are `capture-subject:asset:<object-path>` and
`capture-subject:actor:<actor-name-or-label>`. Supplying `asset_path` or `actor`
inside `parameters` is allowed only when it identifies the same subject.

## Lifecycle and evidence

The three commands share the T32 process-owned session registry while each
public request creates a new plan, authorization, service, and invocation
context. A successful start returns an `operation_ref`; it is not an artifact
terminal result. Observe or reattach with `operation.status`, cancel with
`operation.cancel`, read the terminal receipt and artifact through the same
session, then call `operation.release`. Unknown state forbids replay. Release
removes only operation-owned staging and releases the lease; the published
artifact remains independently readable until explicitly cleaned up by its
owner.

