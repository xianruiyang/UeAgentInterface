# Immutable command contract authority

`FUeAgentImmutableCommandRegistry` is the only new-architecture owner of an
executable command contract. It freezes complete C++ declarations before
composition and owns the exact command identity, Python binding, input and
result fields, declared errors, contract version and hash, public owner,
availability, side effect, state layers, and purpose contract.

`FUeAgentExactCommandDispatch` composes exactly one handler for every enabled
contract. Both the command identity and public owner must match. Calls with a
prefix, an unregistered synonym, a stale contract hash, a missing handler, or
a second owner fail without entering business code. Discovery keywords and
synonyms remain non-executable purpose metadata.

The registry projection is tagged `cpp_immutable_command_registry`. SDK
bindings and command reference pages consume only that projection. A projection
with an incomplete contract, duplicate command identity, duplicate Python
binding, or mismatched hash is rejected; Markdown, tests, and old server routes
are not command-definition inputs.

The legacy server registry and earlier partial binding registry remain outside
the active module startup path established by T01. They are not adapters for
this registry and will be physically retired after all consumers have migrated.
