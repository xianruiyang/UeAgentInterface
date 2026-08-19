# T03 state/value/context contract

`FUeAgentFrozenExecutionContext` freezes one request's project identity, state plane, resolved context facets and freshness observation before planning. A required `current` facet must be resolved at the outer boundary; a domain owner receives only stable identities and cannot query a new current World, selection, viewport or editor later.

`IUeAgentStateValueContextPort` is the fixed owner-facing port for the existing canonical identity, value, default, authored-presence, override, state-plane, temporal and spatial contracts. Its production implementation delegates snapshot creation, semantic comparison and explicit conversions to `Core/StateSemantic`; it does not parse fields or branch by domain.

Request freshness and captured content time are separate values. Authoring memory, persistence, compiled/generated state, runtime state and rendered observation remain distinct state planes. Conversion APIs require an explicit temporal or spatial conversion object, so missing frame, clock, rate, unit or evidence cannot be guessed from a field name.

This module contract does not claim that every active command or domain owner already consumes the port. Consumer migration and public production evidence belong to their dependent strict-v2 tasks.
