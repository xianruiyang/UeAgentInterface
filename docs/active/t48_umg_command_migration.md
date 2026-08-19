# T48 UMG command authority

The current public methods are client.umg.asset_create(...) and
client.umg.widget_events_discover(...), discovered by command_search and
command_detail and mirrored by uai-cli exact execution. No task-specific
generated projection remains active.

The UMG command owner delegates Widget Blueprint creation to the common durable
operation lifecycle and native UMG mechanics; widget-event discovery stays a
read-only query. The SDK does not reimplement class resolution, widget/event
reflection, persistence, readback, or cleanup.

Create success requires terminal native state and independent persisted
readback. Discovery retains its actual state layer. Prewrite failures leave no
residue; unknown state is reconciled. Cleanup is bounded to the owned fixture
and verified independently.

