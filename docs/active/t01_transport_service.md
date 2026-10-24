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

The former module-owned `FUeAgentHttpServer`, its compiled registry, World-bound
query composition, and startup Authoring registrations are physically absent
from the active composition tree. UE mechanics still required by current
EditorProject, ModelingGeometry, and CaptureMedia consumers live behind the
private `FUeAgentLegacyMechanics` implementation boundary. That boundary owns no
listener, route, authentication, registry, or command dispatch and cannot be
constructed by module startup.

The composition root installs the immutable command registry behind the shared
CommandService ingress. Every public command uses the same authenticated HTTP
route and transport-neutral dispatch; domains do not own separate listeners or
request queues.

## Concurrent HTTP admission and command scheduling

`FUeAgentSocketHttpRouteHost` owns the loopback listener and its network thread.
Accepting connections, reading HTTP framing, detecting disconnected clients,
and writing replies continue while a command occupies the GameThread. The host
replaces the Engine HTTPServer route host and does not change global Engine HTTP
listener settings. HTTP payloads remain opaque bytes; command decoding and
business dispatch stay in their existing owners.

Complete command requests enter one FIFO queue and execute on the GameThread,
one request per ticker dispatch with a reentrancy guard. The pending queue holds
at most 128 requests. Full admission returns HTTP 503 with
`transport_backpressured` and an explicit message that this request did not
execute; increasing the TCP backlog is not the scheduling mechanism. The ping
route executes on the network thread and observes only synchronized transport
state. A successful ping proves reachability, not command completion or
authentication.

An exchange owns its reply until delivery or disconnection. A disconnected
request still waiting for execution is removed immediately, releasing both its
queue slot and retained request without waiting for another GameThread tick.
Admission, removal, dequeue and stop use one synchronized FIFO; capacity is
derived from that queue, with no separate counter or disconnected tombstones.
Disconnect and reply completion serialize their terminal-state updates so a
late completion cannot revive a disconnected exchange. Disconnection after execution
starts does not cancel or replay the command, and its business outcome may be
unknown to the caller. Long-running commands return their existing operation
snapshot through an ordinary response; their operation owner controls later
execution and polling. The unused transport-level `Deferred` reply fields and
callback path have been removed. There is one transport reply path.

Unbinding invalidates the owned route. Stopping atomically closes queue admission
and detaches pending work, removes the GameThread ticker, and rejects pending
unexecuted requests with `transport_stopping`. The network thread closes the
listening socket as soon as it observes stop, before draining existing replies.
Stopping joins that thread and releases owned sockets before handler destruction.
The host accepts Content-Length framed HTTP requests and closes each connection
after one response; unsupported framing receives an explicit transport error.
Transport failure code and message are exposed in diagnostic headers, while the
response body remains opaque to the client transport adapter. Neither the host
nor the client automatically retries or replays commands.
