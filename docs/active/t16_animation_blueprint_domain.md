# T16 Animation Blueprint Domain

Ordinary Blueprint-family workspaces always include a `graph_declarations` map.
Add a readable graph name with `{ graph_kind: "function_graph" }` in that existing
collection document, then call the workspace handle's `import_changes()` and
export a fresh workspace. Query `blueprint.node_candidates` for the now-existing
graph before adding a returned node name to its ordinary `nodes` map in a second
import. Do not create extra graph files or edit the hidden manifest. The server
captures native graph/node/pin identity and owns creation replay.

The declaration owns graph name and kind; graph-body headers are read-only
references, while the body owns nodes, pins, edges and layout. Existing names and
kinds identify the captured graph. Omit a function or macro declaration to remove
it, leaving its unedited body files in place. Deletion combined with body edits
is rejected before writing. A changed map key means remove plus create; preserving
identity through rename or kind conversion is unsupported. Animation-specific
schemas retain their domain owner. This does not add animation-graph or
state-machine creation through the generic declaration map.


Use `client.content_graph.import_inline(target, document)` for missing-target
initialization and an exported workspace handle's `import_changes()` for existing
assets. Both use the common content-graph HJSON ingress. The
Animation Blueprint provider contributes `uai.animation_blueprint.edit/1` to
that shared owner and does not declare a domain apply command. Use the
read-only `animation.state_machine.candidates` command before creating a State
Machine or State, then pass only the selected `candidate.name` or
readable `candidate.name` (and optional short semantic `select`) into HJSON. For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).
The candidate method is `client.animation.state_machine_candidates(...)`. No task-specific helper or tutorial-owned
registry is authoritative.

The accepted schema is `uai.animation_blueprint.edit/1` with exact root type
`/Script/Engine.AnimBlueprint`. This revision supports a fully preflighted
`create`; the server owns the missing-target expectation. Its provider extension contains
class settings, optional common Blueprint surfaces, animation graphs, and state
machines with states, entry state, and transitions.

Candidate discovery binds one future missing AnimBlueprint identity to the same
`class_settings` used by create. It creates a transient AnimBlueprint through
the same `UAnimBlueprintFactory`, opens no editor window, and exposes two exact
native surfaces:

- `surface="animation_graph"` enumerates the context-sensitive AnimGraph
  Blueprint Action Menu and invokes the same Blueprint node spawner. The exact
  `UAnimGraphNode_StateMachine` action is writable.
- `surface="state_machine_graph"` first creates a transient State Machine
  through that parent spawner, then enumerates
  `UAnimationStateMachineSchema::GetGraphContextActions`. Plain State is
  writable; Alias, Conduit, Comment and other actions without an executable
  binding remain available to `describe` with their specific support reason.

Default discovery returns all matching base names, including search-only actions.
Creation support is independent of catalog completeness; actual source-enumeration
gaps still return `candidate_catalog_incomplete`. Same-name actions retain distinct native identities and
resolve through readable `select` fields. Use `describe` for creation support
and its reason. The old pagination guidance and coverage booleans are retired.
For a character AnimBlueprint, create it with a Skeleton binding. The same
`class_settings` object is used by candidate discovery and creation:

| Field | Creation meaning |
| --- | --- |
| `parent_class` | An existing creatable `UAnimInstance` subclass. |
| `target_skeleton` | An existing Skeleton object path. |
| `preview_skeletal_mesh` | An existing SkeletalMesh object path; its Skeleton is used when `target_skeleton` is omitted. |
| `template` | `true` creates a Skeleton-independent template and cannot be combined with either Skeleton or preview mesh. Use `false` for a bound character AnimBlueprint. |

Choose actual object paths from the current assets. A Skeleton-independent
template is not the ordinary character creation example. Existing assets expose
their domain class settings through the shared workspace settings document;
CDO properties are a separate surface. Existing `target_skeleton` and `template`
retain the current read-only identity semantics; mutable settings use the
animation owner's native validation and application.

The complete missing-target document wraps `provider_extension` in
`root.document`. Pass its HJSON text as `document`; the SDK does not add the
schema, operation, or target. This example uses native candidate names observed
in UE 5.6; use the names returned by discovery in the current environment.

```hjson
schema: "uai.animation_blueprint.edit/1"
root: {
  type: "/Script/Engine.AnimBlueprint"
  document: {
    operation: create
    target: { asset_path: "/Game/Example/ABP_Example" }
    provider_extension: {
      class_settings: {
        template: false
        parent_class: "/Script/Engine.AnimInstance"
        preview_skeletal_mesh: "/Game/Example/SK_Example.SK_Example"
      }
      state_machines: [{
        name: Locomotion
        animation_graph: AnimGraph
        candidate: { name: "Anim Graph Node State Machine" }
        pos_x: 320
        pos_y: 64
        states: [{
          name: Idle
          candidate: { name: "State" }
          pos_x: 240
          pos_y: 0
        }]
        entry_state: Idle
        transitions: []
      }]
    }
  }
}
```

Candidate identifies only the native action. Instance `name`, owning graph,
position, entry selection and transitions remain outside it. A known `class`
may be supplied only as an assertion; it cannot replace the readable name. Transitions
are relations, not right-click candidates: T16 creates them through the native
State Machine schema connection path. The service re-enumerates and resolves
the candidate during transient preflight and again in the real graph before
the first persistent mutation.

State-machine names are scoped to their owning animation graph; state names are
scoped to their state machine. Duplicate named declarations in one scope are
reported as identity conflicts instead of silently skipping their fields.
Each transition item creates a separate native transition, including multiple
items with the same endpoints. Requested names, owners, positions, entry states
and transition settings are checked after compilation and persisted reload.

Directory, `single_file`, and `inline` sources all pass through the shared
workspace loader, including manifest completeness, path/hash boundaries, and
exact entry-file resolution, before this provider executes. The provider is a
thin adapter over that shared HJSON lifecycle. T16 reuses the
verified T14 Blueprint members, components, graphs, nodes, pins, and edges. T13
is the sole common recursive initialization authority; T16 registers only the
Animation Blueprint-specific root, graph, state-machine, state, transition, and
class-setting contracts and consumes that seam. It does not duplicate parsing,
recursion, defaulting, reference binding, rollback, persistence, readback, or
cleanup.

Success is terminal only after normal Animation Blueprint compile and save,
package unload, reload, and independent readback containing class settings,
common Blueprint state, animation graphs, state machines, states, transitions,
generated class, and non-error compile status. Animation graph/state-machine
topology changes through the dedicated extension, conduits, aliases, and layer
interfaces fail before persistent writes. The native catalog still
searches unsupported actions truthfully; their presence does not authorize
HJSON creation. The non-HJSON
`animation.anim_blueprint.*` commands remain with
`animation-blueprint-command-owner` and are not compatibility fallbacks.

Existing Animation Blueprints use one semantic workspace coordinator for root
properties, Blueprint members and class defaults, and ordinary graph node
creation/deletion, properties, pin defaults, pose connections, layout, and
transition-rule graph nodes. The shared Blueprint projector and prepared mutation
use the graph's native K2-derived schema. Query `blueprint.node_candidates` for
the captured graph, including nested State and transition-rule graphs.
Use the exported graph header's `display_name` and `parent_graph` to identify
the native relationship. When `query_name` is present, use it as the node query's
graph selector; it distinguishes same-named graphs using the existing native
object-path lookup. The workspace import keeps captured graph identity internally.
Animation graph/state-machine topology stays with its domain owner; no second
animation node format is required. Generic structural edits require compile/save and reject before
writing when requested without that lifecycle. The common checkpoint covers
post-mutation early failures as well as final readback rejection, restoring the
pre-import dirty memory and original package bytes independently. Use an
independent export after import to inspect persistence. Existing inline updates
remain invalid; use the exported directory and its server-owned mapping.

A single workspace import may add member variables and their Get/Set nodes in
existing graphs. An action missing from the old asset catalog is not
rejected there when the same batch declares new variables: the shared prepared
mutation first applies the members on its scratch Blueprint, compiles and
refreshes native actions, then resolves the original name and any `select` selectors. Unresolved
or ambiguous actions still fail preflight before persistent mutation. This does
not require an intermediate member import or an Editor restart.

Existing AnimGraph node reflected properties also use this shared property
path after the graph and node identities have been matched to the current native
objects. New nodes and connections use the ordinary graph contract and its
context-sensitive candidates, rather than a reflected-property edit.
For PoseDriver, `Node.PoseTargets[*].CustomCurve` is an
ordinary RichCurve with centralized interpolation, extrapolation, tangent,
tangent-weight and key-default definitions. Its key `time` coordinate is the
activation-weight input to the RBF custom mapping, and `value` is the mapped
weight; neither is seconds. No fixed 0–1 validation range is imposed.
