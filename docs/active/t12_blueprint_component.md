# T12 Blueprint Add Component candidate authority

`blueprint.component.candidates` is the sole public candidate query for adding
an existing Component type to a Blueprint Simple Construction Script. It reads
the same `FComponentTypeRegistry` used by the Editor's Add Component menu,
applies the same default Class Viewer filter, and preserves behaviorally
distinct entries such as Cube, Sphere, and a plain Static Mesh Component.

The public call is `client.blueprint.component_candidates(...)`. When its
method or hash is not already known, use `client.query.command_search(...)` followed
by `client.query.command_detail(...)`; `uai-cli` exposes the same exact contract.

```python
page = client.blueprint.component_candidates(
    target="/Game/Blueprints/BP_Example.BP_Example",
    parent="DefaultSceneRoot",
    query="Point Light",
    limit=10,
)
candidate = page.payload["candidates"][0]
```

Each row has compact creation selectors:

```json
{
  "name": "Point Light",
  "id": "candidate:...",
  "display_name": "Point Light",
  "kind": "actor_component",
  "class_path": "/Script/Engine.PointLightComponent"
}
```

`name` is sufficient only when it is unique in the current native registry.
`id` is the exact selector and preserves the native action, Component class,
and any asset override. `class_path`, `asset_path`, display name, and category
are optional diagnostics; the model does not need to copy them to create.

Use only one minimal selector in the Blueprint-root `uai.blueprint.edit/1`
document. The stable map key owns the Component instance name; `parent` remains
outer SCS context:

```hjson
"component:DoorTrigger": {
  operation: create
  candidate: { id: "candidate:..." }
  parent: DefaultSceneRoot
}
```

Before mutation, the Blueprint Surfaces provider re-enumerates the native
registry and resolves the same entry by `name` or `id`. Creation uses
`USubobjectDataSubsystem::AddNewSubobject`, applies the registry's asset
override and post-create callback, renames the new SCS node to the stable-map
instance name, and then applies any explicit HJSON properties. Unknown,
ambiguous, stale, or mismatched richer fields fail before write; there is no
class-only fallback.

The deterministic query context does not consume the user's current Content
Browser selection. Registry entries with their own asset override, including
Basic Shapes, remain covered. The menu actions that create a new C++ or
Blueprint Component class are separate source/asset-creation workflows and are
not represented as SCS instance candidates.

There is no active `uai.blueprint.component.edit/1` root and no
Component-specific public apply command. The process HJSON authority rejects
that retired schema; only the Blueprint-root `uai.blueprint.edit/1` provider
owns persistent Component creation. Successful Blueprint-root import
compiles, saves, unloads, reloads, and reads back the persisted SCS through the
shared lifecycle seam. T13 remains the sole source-carrier and common recursive
initialization authority.
