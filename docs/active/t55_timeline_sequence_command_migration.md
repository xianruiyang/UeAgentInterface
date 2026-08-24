# Timeline and Sequence commands through the default SDK

Discover the current contract through
`client.query.command_search(...)` and
`client.query.command_detail(...)`, then call one generated binding:

```python
client.timeline.sequence_create(...)
client.timeline.sequence_read(...)
client.timeline.sequence_settings_update(...)
client.timeline.sequence_structure_replace(...)
client.timeline.widget_animation_read(...)
```

`client.timeline.sequence_settings_update(...)` is the non-destructive
Level Sequence settings operation. Its runtime detail owns the exact optional
settings and native revision input. The current contract can change display rate
and playback range while preserving tick resolution, bindings, tracks, sections,
channels, and keys. Tick-resolution mutation is not exposed until a
non-interactive retiming contract can preserve authored time without opening an
Editor dialog.

`client.timeline.sequence_structure_replace(...)` is an explicit full
structure replacement and is not a sparse settings patch. Sequence creation and
both update paths use the common durable operation lifecycle: validate and lock,
mutate the resolved asset, compile/settle, save, unload/reload, and independently
read back the persisted result. A stale revision or invalid asset, binding,
track, section, channel, timebase, or frame fails before publication.

HJSON callers do not choose the native path manually. They first call
`content_graph.authoring_schema`, edit the returned template, and submit it
through `content_graph.import_workspace`; the Sequence provider delegates a
sparse field edit to the settings operation and a full replacement to the
structure operation. Content-graph and native Timeline revisions are distinct
contracts and must be passed back only to the command that returned them.

The exact CLI uses `uai-cli exec <command-id>` with the current command hash
and one JSON parameter object. No task-specific generated projection, transport
retry, persistence helper, or Python Timeline business logic remains active.

