# T12 Blueprint Component authority

`blueprint.component.candidates` is the sole public candidate authority for the
T12 Blueprint Simple Construction Script creation surface. It accepts one exact
Blueprint object identity, one registered Component class, and one proposed
Component variable name. A returned `candidate.name` is copied unchanged into
the `uai.blueprint.component.edit/1` HJSON document together with its exact
`component_class`, `binding_revision`, and the returned `scs_revision`.

The public SDK ingress is
`client.blueprint.component_candidates(target=..., component_class=...,
requested_name=...)`. If the method is not already known, locate it with
`client.command_search(...)`, inspect its live contract with
`client.command_detail(...)`, and use the equivalent discovery flow in
`uai-cli`. Tutorials do not carry a separate task helper or command catalog.

The T12 HJSON root uses `/Script/Engine.Blueprint` and a non-empty
`components` array. Each entry has a unique logical `id`, an exact `candidate`,
an optional `parent` logical id (or `null`), a non-negative sibling `order`, and
registered properties. The bounded production set is
`/Script/Engine.SceneComponent` with `relative_location`, and
`/Script/Engine.PointLightComponent` with `relative_location` and `intensity`.
Every owned entry resolves the common
`uai.init.blueprint.scs_component/1` recursion contract before mutation. T13 is
the sole source-carrier and common recursive initialization authority; T12
consumes T13's normalized `source.kind=resolved` document set, registers only
the Blueprint Component domain contract, and consumes that common seam.

The domain provider matches only `content_graph.import_workspace` with the T12
schema. T11 remains the independent Blueprint node authority and is not
reimplemented here. Unsupported classes, unknown properties, stale revisions,
changed candidate bindings, duplicate names, missing parents, and parent cycles
fail before the SCS is changed. Successful imports use the shared native SCS
hook, compile and settle the Blueprint, save, unload, reload, and return the
reloaded SCS readback. A later candidate query exposes current
`existing_components` as an independent persisted readback.

The retired public command ids `blueprint_add_component`,
`blueprint_remove_component`, and `blueprint_set_component_property` are not
registered or discoverable. No legacy Component selector, runner, or Python UE
business owner is part of this surface. The default generated binding only
forwards the exact registered command and contract hash.
