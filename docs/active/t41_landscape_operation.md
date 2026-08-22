# T41 Landscape operation and height-read owners

`landscape.create` and `landscape.height_delta` have one mutation authority:
`landscape-operation-owner`. `landscape.height.read` is the independent
read-only authority published by `query-command-owner`; its descriptor belongs
to the Landscape domain and its `landscape.read.height` provider is contributed
to the one frozen process `DomainCapability` registry. QueryService consumes
that registry lazily and does not build a private provider table.

The default SDK surface is `client.landscape.create(...)`,
`client.landscape.height_read(...)`, and
`client.landscape.height_delta(...)`; the CLI surface is
`uai-cli ... exec <command-id>`. Callers use `command_search` and
`command_detail` to obtain current fields and contract hashes. No task-specific
generated projection is a supported entry.

The typed `landscape` contribution owns two request-scoped operation
factories. The public handler and the typed registry resolve the same stable
factory object for each exact command. That factory runs the common
`OperationService` lifecycle and delegates the domain mutation exactly once to
the process-unique SimpleAuthoring host. The host remains the sole owner of
topology validation, native Landscape mutation, persistence, reload and C9
readback; the adapter does not reproduce those mechanics.

Coordinate payloads identify the `semantic_frame_id`, unit, basis evidence,
conversion chain, observed scale and exact `target_ref`. The public SDK only
forwards one manifest command and contains no Landscape conversion or UE
business implementation.

For `landscape.create`, `target_ref` is the stable managed identity. Creation
binds it to the Landscape actor through the UAI ownership marker and
`UAI/Managed/landscape` folder. The same exact identity is accepted by
`landscape.height.read`, `landscape.height_delta`, and exact managed cleanup,
including after persistence and reload. An exact observed UObject path is also
a valid target, but an actor label, partial path, prefix, or guessed alias is
not. Unreal still owns the generated UObject name and path.

## Independent bounded height read

`landscape.height.read` requires an exact `target` and an `input` object with:

- `region.frame_id = ue.landscape.local_sample_grid`, inclusive integer
  `min_inclusive` and `max_inclusive` pairs;
- `space_contract.frame_id = ue.landscape.local_sample_grid`,
  `unit = sample`, and non-empty `basis_evidence`.

The public route accepts at most 16,384 samples per request. It resolves the
managed identity fail-closed before considering an exact UObject path, reads
height textures through UE's read-only component data interface, and proves
that every requested sample is present. It does not call the Landscape edit
interface, dirty a package, create a transaction, save, or mutate the actor.

The single projected state value contains the exact `target_ref`, observed
`actor_path`, managed identity/folder/tag evidence, full extent, requested
region, actor scale, `height_encoding = landscape_local_height`,
`height_order = row_major_x_fastest`, `sample_count`, and `heights`. The X
coordinate advances fastest, then Y. Invalid frames, out-of-bounds or sparse
regions, excessive sample counts, duplicate managed identities, wrong target
types, missing targets, and unavailable height data remain distinct typed
failures.

## Historical capability resolution

`landscape_create` resolves exactly to `landscape.create`. Its former scalar
topology and transform fields are represented by `topology`, `placement`,
`sample_region`, `initial_height` and an explicit target identity.

`landscape_raise_circle` resolves exactly to the more general
`landscape.height_delta`; it is not retained as a second public alias. Given an
explicit target, independently observed Landscape transform and extent,
`center` in UE world centimeters, `radius_cm`, `strength_cm`, and `falloff`
clamped to `[0, 1]`, derive the bounded sample region and each local-height
delta as follows:

```text
local_center = inverse(landscape_transform) * center
world_distance(x,y) = sqrt(((x-local_center.x)*scale.x)^2
                           + ((y-local_center.y)*scale.y)^2)
linear = max(0, 1 - world_distance/radius_cm)
smooth = linear^2 * (3 - 2*linear)
weight = lerp(linear, smooth, falloff)
delta_local_height = (strength_cm / scale.z) * weight
```

The region is the intersection of the Landscape extent and the per-axis
sample bounds that can fall within `radius_cm`; `row_stride` equals the bounded
region width. This preserves a circle in world space even when X and Y scales
differ. The current native product test freezes center strength, smoothstep
falloff, non-uniform XY scale, bounded region, persistence and independent
native height readback.

Success is terminal only after the common lifecycle receives the native
operation projection with `status=verified` and `effect_outcome=complete`.
Use `landscape.height.read` as the independent readback before and after a
delta, and again after reload when persistence is in scope. Invalid topology,
sample grid, scale, target, or stale context fails before mutation.
`operation_state_unknown` is not safe to replay. Test fixtures are isolated,
unloaded, deleted and checked absent after reload.
