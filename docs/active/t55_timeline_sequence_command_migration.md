# T55 Timeline/sequence command authority

Current methods:

```python
client.timeline.sequence_create(...)
client.timeline.sequence_read(...)
client.timeline.sequence_structure_replace(...)
client.timeline.widget_animation_read(...)
```

They are discovered through command_search and command_detail and mirrored by
uai-cli exact execution. The timeline command owner delegates creation and
structure mutation to the common operation lifecycle and native mechanics;
read commands retain their actual state layer. The SDK contains no sequence
business or implicit timebase conversion.

Terminal writes require independent persisted readback. Prewrite failure leaves
no residue; unknown state is reconciled. Cleanup is bounded to owned fixtures
and independently verified.

