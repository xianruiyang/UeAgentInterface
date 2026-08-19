# T54 Audio command authority

Current default methods:

```python
client.audio_media.asset_create(...)
client.metasound.graph_compile_settle(...)
client.metasound.graph_default_mutate(...)
client.metasound.graph_interface_mutate(...)
client.metasound.graph_link_mutate(...)
client.metasound.graph_node_mutate(...)
client.sound_cue.graph_compile_settle(...)
client.sound_cue.graph_node_mutate(...)
client.sound_cue.graph_relation_mutate(...)
client.sound_cue.graph_root_mutate(...)
```

They are discovered by command_search and command_detail and mirrored by
uai-cli exact execution. The Audio command owner delegates asset creation,
MetaSound/SoundCue graph mutation, compile/settle, persistence, readback, and
cleanup to the common operation service and native UE mechanics; the SDK
contains no parallel graph or audio business.

Terminal success requires applicable native settle and independent persisted
readback. Prewrite failure leaves no residue; unknown state is reconciled.
Cleanup is bounded to owned fixtures and independently verified.

