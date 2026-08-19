# T41 Landscape operation owner

`landscape.create` and `landscape.height_delta` have one public authority:
`landscape-operation-owner`. The default SDK surface is
`client.landscape.create(...)` and `client.landscape.height_delta(...)`; the
CLI surface is `uai-cli ... exec <command-id>`. Callers use `command_search`
and `command_detail` to obtain the current fields and contract hash. No
task-specific generated projection is a supported entry.

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

For `landscape.create`, `target_ref` is the requested creation identity and
determines the actor label; it is not a promise that Unreal will use that text
as the UObject path. After creation, read the persisted Landscape and retain
the exact UObject path returned by the public read flow. That observed path is
the `target_ref` required by `landscape.height_delta` and later exact-target
operations.

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
Independently query persisted world/Landscape state after reload. Invalid
topology, sample grid, scale, target, or stale context fails before mutation.
`operation_state_unknown` is not safe to replay. Test fixtures are isolated,
unloaded, deleted and checked absent after reload.
