# Timeline and Sequence commands through the default SDK

Discover the current command with positive keywords through
`client.query.command_search(...)`, then read its exact usage with
`client.query.command_usage(...)` before calling the generated binding:

```python
client.timeline.sequence_create(...)
client.timeline.track_candidates(...)
```

For HJSON editing, follow the [Sequence workspace contract](t22_sequence_domain.md).
Obtain `workspace = client.content_graph.export_workspace(...)`, edit its ordinary
HJSON files, then call `workspace.import_changes()`. Machine
identity and concurrency data stay in hidden files; callers do not maintain
revisions or command hashes.

The former Sequence read/structure-replace and WidgetAnimation read commands
are compatibility identities, not the normal named SDK editing flow.
Use workspace export/import for Sequence and WidgetAnimation authoring.

Sequence HJSON initialization, saved root-property and float-key value/time/collection updates use the common
transaction, save/reload and failure recovery lifecycle. The linked workspace contract
states the verified scope; this does not establish equivalent recovery for
every native timeline operation. Use current discovery and usage for the
thin CLI invocation and the exact native operation contract.
