# T12 Blueprint Add Component candidate authority

`blueprint.component.candidates` is the sole public candidate query for adding
an existing Component type to a Blueprint Simple Construction Script. It reads
the same `FComponentTypeRegistry` used by the Editor's Add Component menu,
applies the same default Class Viewer filter, and preserves behaviorally
distinct entries such as Cube, Sphere, and a plain Static Mesh Component.

The public call is `client.blueprint.component_candidates(...)`. Discover it
with `client.query.command_search(...)` and obtain the minimal Python form with
`client.query.command_usage(...)`.

```python
result = client.blueprint.component_candidates(
    target="/Game/Blueprints/BP_Example.BP_Example",
    parent="DefaultSceneRoot",
    query="Point Light",
)
name = result.payload["names"][0]
```

The response contains names and one query scope:

```json
{"names":["Point Light"],"scope":{"target":"/Game/Blueprints/BP_Example","parent":"DefaultSceneRoot"}}
```

The owner merges behaviorally equivalent rows and groups display names once.
Distinct same-name actions remain separate internally; an actual ambiguity returns
short semantic `select` choices such as class, source or asset. Machine identity,
category, provider and paging metadata do not enter each candidate.
`scope` identifies the target and supplied parent once; it is response metadata,
not a component selector. Re-query needed names when that context changes.

Use only one minimal selector in the exported ordinary component-tree row. The
stable map key owns the Component instance name; put the selected readable
`name` directly at row top level, with optional short `select`. Copy optional
`parent` from the current exported component tree: it can name an SCS node or an
inherited scene component, such as a Character's `RootComponent`. Query and import
resolve these names through the same component operations. Omitting `parent`
uses the actor root. An explicit `parent: ""` on an owned scene component requests
native root promotion; it can replace DefaultSceneRoot. DefaultSceneRoot cannot
be deleted directly: when replacing it, declare the new root with `parent: ""`
in the same desired tree. Native promotion retires the old default root.

```hjson
components: {
  DoorTrigger: {
    name: "Point Light"
    parent: DefaultSceneRoot
  }
}
```

Do not write the retired `provider_extension` carrier, an explicit
`operation: create`, or a nested `candidate` object. The keyed desired-state
projection constructs those machine fields internally. A new row may also carry
known writable initial `properties` in the shared descriptor format, for example
`properties: [{property_path: "Intensity", value: 1234.0}]` for Point Light.
Creation and those properties are applied in the same import. When the selected
native fields are unknown, import the minimal row and read its refreshed properties.
After creation, the refreshed component row contains its class/parent and its
ordinary property file owns subsequent edits; the inline creation payload is not retained.

Before mutation, the Blueprint Surfaces provider re-enumerates the native
registry and resolves the same entry by top-level readable `name` and optional
`select`. Creation uses
`USubobjectDataSubsystem::AddNewSubobject`, applies the registry's asset
override and post-create callback, and renames the new SCS node to the stable-map
instance name. Initial properties use the common recursive property apply and
original-intent verification; subsequent edits use the refreshed native definitions. Unknown,
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

The component tree includes inherited native and parent Blueprint SCS templates.
Inherited property edits use the native editable object/override template in the
child Blueprint; they do not write the ancestor template. An owned component can
attach to an inherited scene component. Parent Blueprint SCS components retain
their inherited name, class and parent; structural edits report their HJSON source.
Native C++ component class overrides follow UE's enabled native override operation
and accept the compatible derived classes it permits. They do not replace the
inherited component with a new SCS node. Native rename/reparent/delete restrictions
remain separate from editable properties and class overrides.
