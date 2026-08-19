# Transport and command-service composition

Status: strict_v2 T01 integration contract. This document does not claim that
the command registry or any Unreal domain is production-ready.

`FUeAgentInterfaceModule` is the only active composition root for the current
listener and its concrete transport adapter. The adapter maps one authenticated
request/reply message to `IUeAgentTransportRequestHandler`; inner command,
query, operation, session, registry, and domain responsibilities do not receive
route, header, method, or status-code types.

`FUeAgentTransportService` owns one `IUeAgentNetworkServerControl` instance. It
binds declared delivery requirements before starting, starts and stops the
owned instance at most once, preserves the transport diagnostic on failure, and
stops the instance during destruction when the caller has not already done so.
An unsupported delivery capability is rejected before listener activation;
upper layers do not retry or replay a request.

`FUeAgentCommandService` retains a stable transport-neutral ingress and a
request-context invalidator. An Editor map event advances only the context
generation and invalidates the observed World identity; it does not rebuild the
ingress, listener, handlers, or session owners.

The former module-owned `FUeAgentHttpServer`, World-bound query composition,
and startup Authoring registrations are no longer reachable from module startup.
Their source remains legacy material pending the task-owned migrations and the
final zero-reference retirement task; it is not an active public authority.

Until T02 activates the audited immutable command contracts, the composition
root installs an explicit `command_service_unavailable` protocol response. This
keeps the transport boundary testable without presenting an echo, fallback, or
legacy dispatch as business success.
