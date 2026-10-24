# T48 UMG command authority

The active public UMG methods are `client.umg.asset_create(...)`,
`client.umg.widget_candidates(...)` and
`client.umg.widget_events_discover(...)`. The formal command manifest remains
the current-set authority. For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). T15 owns the
target-bound candidate-to-HJSON contract and its explicitly incomplete
Designer Palette coverage. No task-specific generated projection remains
active.

The UMG command owner delegates Widget Blueprint creation to the common durable
operation lifecycle and native UMG mechanics; widget candidate and event
discovery stay read-only queries. The SDK does not reimplement candidate
enumeration, class resolution, widget/event reflection, persistence, readback,
or cleanup.

Create success requires terminal native state and independent persisted
readback. Discovery retains its actual state layer. Prewrite failures leave no
residue; unknown state is reconciled. Cleanup is bounded to the owned fixture
and verified independently.
