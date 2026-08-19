# T23 Animation/Curve HJSON domain

The sole Animation/Curve authoring ingress is
`client.content_graph.import_workspace(...)`; independent readback uses
`client.content_graph.structure(...)`. The shared SDK also exposes
`client.content_graph.export_workspace(...)`, but T23 does not claim a
lossless Animation/Curve export projection and returns
`animation_curve_domain_export_not_registered` instead of a false-success read
payload. `command_search`, `command_detail`, and `uai-cli` expose the same
current contracts. There is no task-local generated authority.

T13 owns initialization, recursive traversal, default resolution, binding,
failure, rollback, and cleanup. The T23 provider consumes that common contract
and registers the supported Animation/Curve root and owned-entry creation
contracts; it has no private recursive or initialization path.

Native skeleton references, curves, channels, keys, notifies, compression,
refresh, save/reload, and independent readback remain delegated to T56.
Unregistered creation and invalid revisions fail before mutation.
