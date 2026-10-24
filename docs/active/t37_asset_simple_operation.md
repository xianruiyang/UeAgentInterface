# T37 AssetSimple Operation Authority

The active `asset-simple-operation` contribution exposes the common
`asset.object.create` command and the typed update/replace operations whose
inputs genuinely differ. Constructor profiles remain internal mechanics; a
constructor already reachable through `asset.object.create` does not also
publish a typed create command.

The exact handler requires `uai:operate`, grants only the AssetData mechanics
scope declared by this adapter, and forwards one request to the
process-owned SimpleAuthoring Operation application. That lazy host creates at
most one runtime after the Editor world exists and survives transport or
CommandService reconstruction. The existing AssetData simple-effect code is an
internal UE-mechanics dependency of the current owner only; it is not a public
route, registry, handler, contract, test, or documentation authority. T44 removed
the compiled legacy registry/HTTP handler and verified that this retained
mechanics dependency cannot be reached through an old public identity.

This scope mapping is not destructive authorization and cannot authorize a
delete, move, cleanup or any command outside the canonical AssetSimple inventory.

Success and failure remain command-specific. The common application performs
schema validation, planning, ticket binding, native execution, settle,
persistence, independent projection/readback, rollback or `state_unknown` as
required by the selected descriptor. A prewrite rejection has no residue;
`state_unknown` is not safe for automatic replay. The public SDK exposes the
verified business status and persisted/current-state readback; operation custody,
verification evidence identities and protocol metadata remain machine-only.

## Public SDK and CLI

The final manifest projects the active AssetSimple identities into the default
`asset` namespace. Callers import only `uai`, use `uai.connect_project()` for the local project, search `asset.factory.candidates`, and call
`client.asset.object_create(...)`. Generic `client.execute(...)` and `ueagentinterfacecmd exec`
reuse the same generated catalog, Client, Protocol, and Transport. No T37-specific Python module, command/hash
table, parameter mapper, retry, persistence, rollback, or readback path remains
authoritative.

```python
candidate_result = client.asset.factory_candidates(
    "asset.object.create", query="Material"
)
assert candidate_result.ok, candidate_result.error
client.asset.object_create(
    {"name": candidate_result.payload["names"][0]},
    "/Game/StrictV2/T37/M_Default.M_Default",
)
```

Use `client.query.command_search("asset object create")` and then
`client.query.command_usage(commands)` to obtain the minimal named SDK form.
For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).
Domain behavior and failures are described in this document. Search matches only positive
tokens from command names, registered keywords and registered synonyms.

## Generic native asset creation

Call `asset.factory.candidates` with
`create_command=asset.object.create` to read the live Unreal Engine New Asset
factory catalog plus explicitly supported noninteractive typed factories. The
Curve Vector typed factory preconfigures the native Curve factory's class choice;
its hidden menu status avoids a duplicate menu entry. Class permissions and native
creation eligibility still apply. Discovery and name resolution use the same
currently executable create records; unavailable records cannot introduce a
hidden ambiguity. The result contains readable creation names. Pass the
chosen name as a string shorthand or `factory: {name: ...}`. If creation returns
`candidate_ambiguous`, retain that name and retry with the minimal readable
`select` supplied by `error.choose`. An invalid or stale selection returns the
current choices before writing. Constructor and native-factory records remain
distinct; selecting a record does not relax its initialization or interactive
restrictions. Use the returned semantic fields needed to disambiguate. Do not copy a class path, reference,
status row or machine identity. The target is the intended new asset path:
`/Game/Folder/Asset` and `/Game/Folder/Asset.Asset` name the same top-level asset.
The common create/update resolver uses its resolved object path for identity and
readback; it rejects mismatched object suffixes and preserves target-existence
checks. Input rejection identifies `/target_ref` in `error.diagnostic.json_pointer`
and explains the expected reference, without changing the existing error code.

The selected creation method must still pass its initialization and interactive
eligibility checks. Methods requiring configuration or domain-specific input
reject when they cannot execute generically. The generic command never opens a
configuration window or guesses missing input.

File Media Source assets use the ordinary Reflection workspace for `FilePath`
and `PrecacheFile`. Edit their exported string and boolean property values, then
import and read the refreshed values. `FilePath` is the native editable path field,
not the dormant typed command's source URI: property notifications invalidate
the resolved-path cache but do not invoke `SetFilePath` normalization. Absolute
paths and native relative paths remain authored values. This edits asset settings;
it does not open or validate playback of external media. The common Reflection
bridge uses the same profile class-membership check for export and import, while
capture/readback retain the concrete asset type.

Media Texture uses the same ordinary Reflection workspace for its exported
editable texture settings and `MediaPlayer` reference. Change the external
player reference and settings such as `AutoClear`, `ClearColor`, and `AddressX`
in one import. Native editor notifications update the current player and
texture resource; the native media clock updates the sample queue, external
texture registration, and clear pixels. Save/reload and checkpoint restoration
use the common transaction and native resource lifecycle. This is asset
configuration, not playback, decoding, capture, or preservation of a transient
video frame. The dormant `asset.media_texture.update` contract is not the normal
authoring path; use the actual exported property names and value shapes.
New assets enable `NewStyleOutput`: they use a regular texture resource, so
absence of a player-GUID external registry entry is not an error in that mode.
Changing the exported setting to false selects the native external-texture
path. `ClearColor` uses the exported four-number RGBA array, not named fields
or the codec's internal component envelope.

Paper Flipbook uses ordinary `FramesPerSecond` and `KeyFrames` property rows.
Each keyframe has an external `Sprite` reference and a positive `FrameRun`
count. Copy the exported array element template to add a frame even when the
array is empty; reorder existing entries, change counts, or use an empty array
to clear them. FPS and frames can change in the same import. The reference
does not create or edit the sprite asset. The dormant typed `frames` input is
not an alternative workspace shape.

The native flipbook asset currently computes frame counts and time indices
from the keyframes directly. Its editor notification re-registers flipbook
components to refresh their cached frame and rendering state. Proposed edits
avoid that global notification; reflected numeric constraints reject invalid
values before mutation. Actual edits and persisted/checkpoint rebind use the
native notification with the common transaction. Sprite and TileSet have their
own ordinary workspace surfaces described below; TileMap authoring is deferred.

Paper Sprite assets use the ordinary root workspace
for `SourceTexture`, `SourceUV`, `SourceDimension`, `PivotMode`,
`CustomPivotPoint`, and the other exported editable settings. Source region and
pivot coordinates are source-texture pixels; `PixelsPerUnrealUnit` converts the
pixel geometry to Unreal units. Use the exported vector/enum shapes. Native
notifications normalize pixel coordinates and rebuild render/collision data;
package replacement refreshes actual sprite consumers through the common
transaction. `BodySetup` is generated data, not a second authoring surface.
Source replacement records the new texture-size basis through native sprite
initialization, preserving explicit pixel regions and pivots across reload.
The retained typed `asset.paper_sprite.update` implementation is dormant.

Set `AtlasGroup` to a compatible Atlas reference, or `null` to detach; the old and
new Atlas memberships are updated together through the common transaction and
save/reload lifecycle. Create the standalone `paper_sprite_atlas` root with the
`Paper Sprite Atlas` factory; edit exported description, dimensions, padding,
mip, compression and filter fields in its ordinary workspace. Generated textures,
slots and packing state remain native derived data. UE 5.6 requires
`EnginePatches/UE5.6/paper-sprite-atlas-notification.patch` for generation notifications,
reload and previous-membership cache restoration. Automated optional repacking
keeps the current layout without a modal prompt; ordinary native generation still
runs. Explicit creation does not change the experimental menu preference.

Paper TileSet uses the ordinary root workspace for `TileSheet`, `TileSize`,
`BorderMargin`, `PerTileSpacing`, `DrawingOffset`, `PerTileData`, and `Terrains`.
Create it with the public `Paper Tile Set` factory candidate. Keep the exported
`IntPoint` shape, for example `TileSize.value: { fields: { X: 16, Y: 16 } }`.
Layout dimensions are source-texture pixels, with the top-left origin and X to
the right, Y down. Tile indices are row-major. Copy the exported terrain element
template and supply `TerrainName` and a valid zero-based `CenterTileIndex`;
reorder entries or use `[]` to clear them. These two terrain fields are the
profile's explicit native declaration, not a general read-only-property bypass.
`TerrainMembership` is not exposed and UE's terrain-membership solver remains
unimplemented. `PerTileData` describes the resulting grid: keep one entry per
tile and use the exported struct fields/templates for metadata. When omitted,
native layout changes preserve overlapping metadata by grid coordinate.
Native isolated preview uses Interactive notification to normalize layout and
allocate metadata without re-registering real TileMap components. Actual writes
and package rebind use normal native notification within the common transaction.
The shared property owner writes the complete layout group before notifying;
then it prepares metadata against that final native grid. This preserves
overlapping coordinates and omitted native fields during simultaneous texture
and tile-size changes, including changes in opposite axes. A failed actual
stage restores the first-write checkpoint rather than attempting to reverse
destructive grid allocation by individual property notifications.
The typed `asset.paper_tileset.update` is retained dormant; TileMap authoring is
not part of this workflow.

Existing Input Action assets support ordinary root-property HJSON editing.
Create the asset through the factory name/selection flow, then use
`content_graph.export_workspace`, edit the exported root property rows, and
import that workspace. Reflection owns native property validation, persistence,
independent readback and checkpoint recovery. For the direct
`PlayerMappableKeySettings` property, query `asset.owned_candidates` with that
property path and use the returned name in the ordinary property's
`value.owned_object.name`. Nested `properties` use the exported readable category
and property-row format. Candidate selection, native picker eligibility and
creation share the owned-object owner with array elements. Input Action also
declares the standard `Triggers` and `Modifiers` array contexts. Query the
specific property, append a returned candidate as an `owned_object` item, and
retain exported existing items to preserve their identity and order. Native
Input Action property notifications apply on the actual asset. Candidate
eligibility does not imply that every combination passes native data validation
or that runtime input behavior has been tested.

Input Mapping Context uses the same ordinary Reflection workspace. Its `Mappings`
array contains structured entries with a local `instance` key and `fields`. Keep
each exported key when editing or moving an existing entry; omit it for a new
entry. Array order is the desired mapping order; removing an entry removes that
mapping, and an empty array clears it. Nested owned trigger/modifier identities
remain bound to their original objects when the outer entry moves, including
omitted unchanged fields. Unknown or duplicate entry keys reject before writing.
For an existing mapping, query `asset.owned_candidates` with a zero-based path
such as `Mappings[0].Triggers` or `Mappings[0].Modifiers`, then use a returned name
for a new owned item. Keep external Action references in the exported value shape.
Import requests the native control-mapping rebuild notification and persists the
asset through the common lifecycle. This does not establish gameplay input behavior.

Sound Attenuation and Sound Concurrency assets use configured Reflection roots as
well. Edit the ordinary `Attenuation.fields` or `Concurrency.fields` values inside
the exported property row. Native reflected types and edit conditions remain in
force; the common lifecycle validates, saves and independently reads back the
settings. For example, `bAttenuate` is boolean and `MaxCount` is numeric. Unedited
settings remain unchanged. This settings workflow does not test live audio playback
or the behavior of external audio plugins.

Sound Class ordinary workspaces combine the reflected root settings with a
`child_classes` array of asset references. This array is the complete desired
child list: retain references to keep children, add references to attach them,
remove references to detach them, or clear the array. Root settings such as
`Properties.fields.Volume` can change in the same import. The domain prepares
both sides of each relationship, including a child's former parent, before the
common transaction checkpoints, saves and independently reloads affected packages.
Cycles reject before mutation. Derived editor graph links refresh without changing
unrelated packages' dirty state or rebuilding their existing node layout.

Sound Submix uses the same Reflection workspace and shared relationship plan.
Edit `child_submixes` together with root settings such as `AutoDisableTime` and
the ordered `SubmixEffectChain` array of existing Submix Effect Preset references.
An empty child array clears the children; cycles and incompatible effect references
reject before writing. The common transaction owns affected packages and independent
readback. Proposed objects are isolated and do not notify audio devices; actual
application and recovery retain native notifications. This route does not edit
preset settings or establish playback behavior. Ordinary `asset.delete` owns the
native registration lifecycle and recovery limits documented in T31.

Sound Mix uses the configured Reflection root route for `SoundClassEffects` and
ordinary settings such as `InitialDelay`. Its adjusters are normal struct values
with external Sound Class references, not owned objects or relationship mutations.
The common property chain supplies native member-property notifications, followed
by the existing transaction and independent readback. The old typed update remains
dormant. The shared definition catalog supplies `element_definition` and a
copyable native-default `element_template` for empty and nonempty Struct arrays;
these describe creation without becoming captured entries or instance identities.

Existing Specular Profile assets use the same configured Reflection workspace
provider. `Settings.ViewColor` and `LightColor` expose RuntimeColor curves with
four `r/g/b/a` channels. Each channel owns its keys, default and pre/post infinity
extrapolation; the centralized definition supplies rich-key enums/defaults and
embedded/external-reference templates. Conflicting root and channel extrapolation
is rejected; a root setting without a channel setting retains its uniform input
meaning. External `UCurveLinearColor` selection preserves inactive embedded data.
The curve coordinate `time` is not seconds here: LUT Format selects NoV/NoL or
VoH/NoH sampling. Texture-null profiles use these procedural curves. This route
edits existing assets; creation remains the generic factory flow.

Standalone Curve Float, Curve Vector and Curve Linear Color assets use that same Reflection provider. Create them
through the returned native factory name, export an ordinary workspace, and edit
the `curve_float_profile_state.value`, `curve_vector_profile_state.value` or
`curve_linear_color_profile_state.value` row on the curve surface alongside root
settings. Vector `x/y/z` and Color `r/g/b/a` channels own their
keys, defaults and pre/post infinity extrapolation independently. Rich-key enums
and defaults come from the centralized definition. `time` is the input coordinate;
its unit belongs to the consuming content and is not imposed as seconds. This
asset surface does not use a timeline or an external-reference switch. Root
properties and curve state share Reflection's validation, persistence, independent
readback and checkpoint recovery. Float uses the `value` channel and root-level
pre/post infinity extrapolation. Linear Color's seven editable `Adjust*` root
properties can be changed in the same import as its RGBA curve. Float's native
event flag is preserved and is not exposed as an editable property.

PhysicalMaterial uses the configured Reflection root provider, common property codec,
ObjectApply, semantic transaction and persisted readback. The root capture domain
also resolves `/Script/PhysicsCore.PhysicalMaterial`; a profile alone is not sufficient
for import/readback. This surface edits asset settings, not live physics simulation.

DataObject handles concrete, non-deprecated DataAsset subclasses through ordinary
Reflection. Dedicated domain providers take precedence; the generic DataObject
provider is a fallback, not a second owner for PCG, StateTree or other native domains.
Root and owned fields use native editability and the shared recursive codec.
Ordinary CPF_Edit fields do not require registration metadata.
Metadata can additionally authorize supported non-editable fields, while unsafe,
transient, deprecated and EditConst exclusions remain enforced.
The ordinary workspace uses exported native property names and shared value/reference
representations. It does not interpret dormant `registered_fields` metadata IDs as
property-path aliases. Dormant typed operation identities and their original contract
remain available only through the existing compatibility policy.

TextureRenderTarget2D uses the configured `render_target` Reflection provider and
root capture domain. Shared ObjectApply groups size, format, clear color and mip
settings before sending their native property-chain notifications. Reflection's
isolated prepare first checks the complete intent using the shared codec without
notifying or allocating a render resource; invalid dimensions reject before
actual writes. Valid input then runs native notifications on the isolated preview,
and the shared property plan captures native-normalized descriptors for actual
application and strict readback. UE owns format/gamma normalization, resource recreation and scene
capture notifications. The common transaction owns persistence and recovery.
In particular, UE's constructor initializes SRGB=true even for the default
RGBA16f format; the first native post-edit corrects it using IsSRGB(). Verification
distinguishes this exact derived change from unrelated property preservation.
Native resource initialization queues clearing for the deferred rendering phase;
pixel verification consumes that phase rather than repairing the target.

Dimensions above 2048 are supported without a UAI confirmation threshold.
Dimensions below 1 remain invalid. On UE 5.6, native TextureRenderTarget2D
notifications align dimensions to the pixel format and clamp them to 16384,
then update gamma, resources and scene captures. UAI uses that native callback;
there is no separate render-target notification adapter. Actual rendering-device constraints still apply. Workspace verification
covers a 2049-wide target, independent reload and failure restoration.

RuntimeVirtualTexture uses its existing Reflection profile through the ordinary
root workspace and ContentGraphRead domain. TileCount/TileSize remain the native
stored exponents; the common codec does not translate former typed pixel-size
inputs. MaterialType, compression and other eligible fields retain native
property-chain notifications. UE notifies referencing RVT and primitive components,
and their render-state lifecycle owns producer/cache allocation. Producer dimensions
also consume the component transform and device-profile biases. Asset getters alone
are not proof of a live allocation or rendered pages. Persistence and checkpoint
recovery remain in the shared Reflection transaction.

Resident RVTs use the shared package-replacement policy so live component references
are redirected to a genuinely reloaded object. Reflection brackets that replacement
and checkpoint restoration with native component reregistration for registered
components referencing this RVT. It drains the old proxy's queued release before
the old asset can be collected, then restores registration against the redirected
reference. Transient RVT previews likewise finish their queued native initialization
before retirement. This does not clear user references or introduce a separate VT
resource builder or persistence path.

SparseVolumeTexture settings use one shared Reflection profile for streamable
static and animated assets. Internal frame objects are not separate authoring
roots. Ordinary workspace fields keep the native AddressX/Y/Z and streaming
parameter names; enum/value conversion and transaction handling remain shared.
Property-chain notifications preserve UE's address-specific material notification
and frame recache, and its streaming-parameter manager refresh. Resident material
references use native package replacement for durable readback. Frame source data
continues to belong to the native importer and is not replaced by editing settings.

SubUVAnimation uses the shared Reflection root workspace and ContentGraphRead
domain. Edit the exported `SubUVTexture`, `SubImages_Horizontal/Vertical`,
`BoundingMode`, `OpacitySourceMode` and `AlphaThreshold` properties. Texture
references and null values use the common property codec. Native property-chain
notifications own the grid clamp, particle component reregistration, cutout cache
and bounding-geometry buffer rebuild; HJSON does not generate geometry itself.
The source texture is a referenced input, not part of the SubUV save mutation.
Persistence, final readback and checkpoint recovery use the shared transaction.

TextureCollection uses the same Reflection root and ContentGraphRead workspace.
Edit the exported `Textures` object-reference array; order, duplicate references,
null entries, removal and an empty array are preserved. Native property notifications
rebuild the resource collection and notify referencing materials. Null entries use
UE's default texture in the native resource while remaining null in the asset.
Resource collections require native bindless support; HJSON does not enable it or
replace UE's resource builder. Resident material references use the existing native
package-replacement readback policy. Referenced texture source packages are not edited.

Texture2DArray exposes `SourceTextures` and native texture settings in its ordinary
Reflection workspace. The shared array/reference codec and NativeStages apply source
generation before explicitly changed format properties. Native compatibility checks
reject null slots or textures without source data before notification; an empty list
is valid and invalidates the old source and resource. UE owns size/format/gamma
conversion, ordered slice generation and texture resource rebuilding. Source edits
can derive native defaults for settings that were not explicitly changed; inspect
the final values in the refreshed workspace. Source texture packages stay unchanged. Shared durable replacement
preserves resident material references and the common transaction handles failures.

TextureCubeArray uses that same workspace, validation preparation, NativeStages and
durable transaction. Each `SourceTextures` entry refers to one TextureCube. Native
compatibility requires matching source dimensions, formats and face counts; sources
may consistently use six faces or single-image long/lat cubemaps. Unlike Texture2DArray,
incompatible cube sources are rejected instead of resized or format-converted.
Source slice counts differ from RHI cube counts: six-face sources contribute six
source slices per entry, while long/lat sources contribute one and UE builds the
six output faces. An empty list invalidates both generated source and resource.

TextureCube exposes a separate `source_input` object in its ordinary workspace.
Fill all six fields `positive_x`, `negative_x`, `positive_y`, `negative_y`,
`positive_z`, `negative_z` with existing Texture2D asset references. Sources must
have valid single-slice, square source data with matching dimensions and formats.
The shared reference codec resolves the values; native texture mechanics freeze
and assemble the ordered faces. Explicit texture settings in the same edit remain
authoritative. Source assets are not modified.

These references are generation inputs, not a persistent source-reference array.
Re-export returns an empty input template; leaving every field empty makes no
generation request. A partially filled template or null face is rejected before
target mutation. Generated Source state participates in the shared transaction's
preview, comparison, durable readback and failure recovery; internal source digests
are not editable HJSON fields.

VolumeTexture uses the same generation-input, reference decoding and durable
transaction path. Its `source_input.value` contains an ordered `slices` array of
Texture2D references. Nonempty submissions accept 1–512 compatible single-slice
sources with matching dimensions and formats; unlike Cube faces, slices need not
be square. An empty exported array means no generation request, not source removal.
Native assembly writes the 3D Source and clears the previous `Source2DTexture` and
`Source2DTileSizeX/Y`. Independent readback and shared recovery cover both generated
bytes and that native source metadata. Source assets remain unchanged; slice refs
are not persisted as a reference array and re-export returns an empty template.
