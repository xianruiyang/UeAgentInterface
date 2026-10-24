# Retired UAI capability catalog

This directory preserves the former `uai.capability_catalog.v2` projection and
its pre-refactor decision overlay as historical evidence only.

It was removed from the active surface because it copied the complete command
identity set, manifest hash and command count into a second maintained state
source. Its dedicated test therefore turned every legitimate manifest addition
into a catalog synchronization gate without proving runtime behavior, model
usability or a user capability.

Current authority is intentionally elsewhere:

- public command identity and contracts: the frozen runtime manifest and live
  `query.command.detail` projection;
- capability verification scope: the current root plan and task/result owners;
- dynamic task status and next action: the root task table;
- old 939-command disposition and the snapshots in this directory: historical
  audit evidence only.

Do not import, regenerate, synchronize or execute this catalog as part of the
active build, test, SDK, skill or release workflow. Read it only for a bounded
historical audit or an explicitly authorized migration.
