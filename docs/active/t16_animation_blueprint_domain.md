# T16 Animation Blueprint Domain

`content_graph.import_workspace` is the only public HJSON ingress. The
Animation Blueprint provider contributes `uai.animation_blueprint.edit/1` to
that shared owner and does not declare a domain apply command. Use
`client.content_graph.import_workspace(...)` plus readback methods. Use
`client.command_search(...)` and `client.command_detail(...)` to inspect live
contracts; `uai-cli` exposes the same discovery path. No task-specific helper
or tutorial-owned registry is authoritative.

The accepted schema is `uai.animation_blueprint.edit/1` with exact root type
`/Script/Engine.AnimBlueprint`. This revision supports a fully preflighted
`create` with `expected_revision="missing"`. Its provider extension contains
class settings, optional common Blueprint surfaces, animation graphs, and state
machines with states, entry state, and transitions.

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
generated class, and non-error compile status. Existing-target updates,
deletion, conduits, aliases, transition-rule graph authoring, layer interfaces,
and workspace export fail before persistent writes. The non-HJSON
`animation.anim_blueprint.*` commands remain with
`animation-blueprint-command-owner` and are not compatibility fallbacks.
