# T27 DataTable/CurveTable HJSON domain

The only public HJSON write ingress is
`client.content_graph.import_workspace(...)`; readback uses
`client.content_graph.structure(...)`. The verified workspace export uses
`client.content_graph.export_workspace(...)` and publishes a semantically
rebuilt `table.hjson` through the common artifact publisher. Current fields and
hashes come from `command_search` and `command_detail`, and `uai-cli` exposes
the same contracts. There is no task-local binding authority.

T13 owns initialization, recursive traversal, defaults, binding, failure,
rollback, and cleanup. T27 consumes T13's normalized `source.kind=resolved`
document set and entry identity, registers the supported table roots and owned
row/key entries, and implements no private recursive or initialization path.
T37 remains the native mechanics authority for table mutation, source update,
persistence, reload, and independent readback. Invalid or unregistered
creation is rejected before mutation.
