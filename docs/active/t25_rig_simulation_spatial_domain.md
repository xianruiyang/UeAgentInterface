# T25 Rig Simulation/Spatial HJSON Domain

`uai.rig_simulation_spatial.edit/1` is a runtime-only schema consumed through
the shared `client.content_graph.import_workspace(...)` ingress. Use
`client.content_graph.structure(...)` for the exact live subject, runtime
revision, spatial frame, evaluation clock, skeletal binding, bones, and
independent readback. Use `client.query.command_search(...)` and
`client.query.command_detail(...)` to inspect live contracts; `uai-cli` exposes the
same discovery path. No task-specific helper or tutorial-owned registry is
authoritative. The state layer is explicitly `runtime_non_persistent`.

The root is an existing `/Script/Engine.PoseableMeshComponent`. A production
subject must first be created, saved, reloaded, and read back through T26; T25
binds to that exact component object path. It does not create an Actor,
Component, skeletal asset, Rig Graph, or persistent package.

The provider is a thin adapter over the shared HJSON lifecycle. T13 is the sole
common recursive initialization authority; T25 registers only the runtime
pose-subject root initialization contract and consumes that seam. It does not
duplicate parsing, recursion, defaulting, reference binding, pose mechanics,
rollback, runtime readback, or cleanup.

The only supported operation is `update_runtime_pose`. Its exact evaluation
point identifies the editor-frame clock, authored tick, rational rate, and
authored state. Its pose frame identifies the component-local basis, dimension
3, centimeter unit, and basis evidence. Each item names one real bone and a
ten-component translation/quaternion/scale transform. Conversion evidence and
complete preflight are mandatory.

After preflight, mutation delegates to the T58 `rig-spatial-command-owner`
through `animation.subject.pose.update`; T25 then independently captures the
same real component. It accepts only a clean subject package, restores the dirty
flag after synchronous mutation, never saves or reloads the package, and
reports `saved=false`, `reloaded=false`, and `package_dirty=false`. Delegation
or readback failure restores affected bones and the original clean state.
Export is explicitly rejected because runtime pose has no persistent workspace.
