# T54 Audio command authority

Current default methods:

```python
client.audio_media.asset_create(...)
client.metasound.graph_compile_settle(...)
client.sound_cue.graph_compile_settle(...)
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). The Audio command owner delegates asset creation and
explicit compile/settle to the common operation service and
native UE mechanics; the SDK contains no parallel graph or audio business.

MetaSound default/interface/link/node and SoundCue node/relation/root mutation
identities are internal Audio HJSON operation kinds. They remain implemented by
the same native mechanics, but are not separate public commands; use the shared
`content_graph` export/import workflow for graph authoring.

Terminal success requires applicable native settle and independent persisted
readback. Prewrite failure leaves no residue; unknown state is reconciled.
Cleanup is bounded to owned fixtures and independently verified.
