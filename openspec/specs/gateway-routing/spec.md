<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# gateway-routing specification

## Purpose

Define Gateway request admission, private-pool dispatch, worker-start deadlines, response handoff and single-attempt delivery.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC1.1, AC1.6, AC1.7, AC1.8. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: GR-01 Socket owners and private request workers

Gateway SHALL keep transport owners separate from its private request pool and use the execution boundaries below.

The detailed [contract](#contract-gr-01-socket-owners-and-private-request-workers) is normative for this requirement.

#### Scenario: Socket owners and private request workers outcome

- **WHEN** a private worker completes a backend operation
- **THEN** it publishes owned result state and wakes the ROUTER owner; it never sends on ROUTER or uses Thunder's global request pool.

### Requirement: GR-02 Current-request admission and worker-start deadline

Gateway SHALL keep only one admitted request awaiting Dispatch and resolve worker start versus deadline atomically without a request queue.

The detailed [contract](#contract-gr-02-current-request-admission-and-worker-start-deadline) is normative for this requirement.

#### Scenario: Current-request admission and worker-start deadline outcome

- **WHEN** all private workers remain occupied for the current request's original ten-second wait
- **THEN** the request becomes EXPIRED, reports INTERNAL when deliverable and never executes later; other completions/control continue and an already RUNNING request is not timed out.

### Requirement: GR-03 Resource and transport policy

Client and Gateway SHALL apply the resource policy below while retaining safe representation and allocation checks.

The detailed [contract](#contract-gr-03-resource-and-transport-policy) is normative for this requirement.

#### Scenario: Resource and transport policy outcome

- **WHEN** a complete request or response exceeds an arbitrary application-policy byte threshold but remains representable
- **THEN** no extra message-size cap rejects it; allocation may still fail, libzmq HWM remains its default and shared-buffer capacity/count limits still apply independently.

### Requirement: GR-04 Ingress validation and accepted ownership

The ROUTER owner SHALL validate ingress, serialize admission with drain and retain accepted work under the following rules.

The detailed [contract](#contract-gr-04-ingress-validation-and-accepted-ownership) is normative for this requirement.

#### Scenario: Ingress validation and accepted ownership outcome

- **WHEN** a structurally valid string service-0 request is received
- **THEN** the owner rejects it with correlated UNSUPPORTED_SERVICE before worker admission; invalid-context/schema/duplicate rules still take precedence and unread traffic remains subject to receive pause.

### Requirement: GR-05 Completion handoff

Private workers SHALL transfer owned completions to ROUTER without waiting for delivery or introducing response-storage admission budgets.

The detailed [contract](#contract-gr-05-completion-handoff) is normative for this requirement.

#### Scenario: Completion handoff outcome

- **WHEN** response serialization fails after a backend operation or accepted release
- **THEN** Gateway prepares a correlated INTERNAL fallback only if possible, otherwise logs/drops; it never repeats the operation, undoes release or ends unresolved access to recover storage.

### Requirement: GR-06 Single nonblocking response attempt

Gateway SHALL attempt each complete response at most once and handle every send outcome under the following ownership policy.

The detailed [contract](#contract-gr-06-single-nonblocking-response-attempt) is normative for this requirement.

#### Scenario: Single nonblocking response attempt outcome

- **WHEN** the routing-frame send returns EAGAIN, EHOSTUNREACH or EINTR
- **THEN** Gateway logs/drops the response without sending its body or retrying; it continues other work, preserves backend leases and sends no second INTERNAL response to report delivery failure.

### Requirement: GR-07 Unexpected frozen or slow receiver

Gateway SHALL retain safe ownership and continue other work when a client stops receiving, using the same single-attempt policy.

The detailed [contract](#contract-gr-07-unexpected-frozen-or-slow-receiver) is normative for this requirement.

#### Scenario: Unexpected frozen or slow receiver outcome

- **WHEN** a client freezes unexpectedly while a backend operation still uses its buffer
- **THEN** the lease remains until actual completion; response backpressure neither blocks a worker nor proves disconnection, and a thawed synchronous caller can time out while async responses have no timer.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: GR-01 Socket owners and private request workers

Source: [GW-005](../../../doc/EyilGateway.md#gateway-component-and-api-call-overview), [GW-006](../../../doc/EyilGateway.md#62-thread-model-and-private-thunder-pool), [OV-021](../../../doc/EyilSecureGateway.md#44-zeromq-endpoints-and-message-framing).

The three transport owners are separate Gateway threads. RequestDispatcher
runs on the ROUTER owner; request and lifecycle jobs run on the private pool.
The monitor collector posts events to the I/O owner, which correlates them and
serializes the registry's disconnect transition.
The adapter, registry, buffer manager and completion handoff are shared runtime
components, not additional threads. The private pool has four workers, in
addition to the transport owners and libzmq's internal threads. A worker wait
pauses only further request reception; responses and control handling continue.

| Execution context | Owns / performs | Must not do |
| --- | --- | --- |
| Thunder framework thread | Initialize/Deinitialize and framework notifications | Run application requests synchronously |
| Gateway I/O owner | ROUTER, the current request, worker-capacity/deadline handling and completion draining | Block while waiting for Crypto, a worker or a slow client |
| ZAP worker | REP and authentication exchange | Use ROUTER or modify another thread's socket |
| Monitor worker | PAIR events, normalized disconnect records | Guess a session from a non-unique PID |
| Private Thunder workers | RequestJob::Dispatch, buffer operations and scoped ICrypto calls | Send a ROUTER reply or own transport state |
| libzmq internal threads | Transport machinery | Replace Gateway request/session coordination |

Create a Gateway-owned Core::ThreadPool with **four workers** and the
Thunder/platform default stack size. Application request work must not use
Core::IWorkerPool::Assign(), Core::IWorkerPool::Instance().Submit(), or helper
jobs that implicitly submit to Thunder's global pool. The wrapper's framework
deactivation job may still use the global pool.

Each DEALER and ROUTER has one socket-owning thread. Client API callers hand
work to their owner through one synchronized submission slot and a wakeup.
Gateway workers hand owned results to ROUTER through its completion handoff.
Neither application callers nor Gateway workers send or receive on the owner's
socket. This follows the restrictions for these socket types in the
[ZeroMQ socket API](https://libzmq.readthedocs.io/en/latest/zmq_socket.html).
Socket options, connection, monitoring and closure follow the same ownership.

### Contract: GR-02 Current-request admission and worker-start deadline

Source: [GW-007](../../../doc/EyilGateway.md#current-request-receive-pause-and-worker-timeout).

Gateway has **no request waiting queue**. The ROUTER owner retains at most one
current admitted request that has not started Dispatch. This is the current
ingress operation, with its immutable input, connection generation, request ID,
completion bookkeeping and absolute deadline; it is not a growing container.
Already running jobs are independent of that current-request record.

1. When there is no current request awaiting dispatch, receive and validate one
   complete RPC. Invalid or unsupported requests use the normal owner-side
   rejection path. For a supported admitted request, establish an absolute
   monotonic deadline at admission plus **10,000 ms**.
2. Try to reserve an available private worker and hand over this distinct job
   without blocking. Use one synchronized capacity account for all jobs on that
   pool, including lifecycle jobs; a request must not assume a worker remains
   available merely because an earlier count was nonzero.
3. If a worker cannot start the current request yet, **pause further ROUTER
   request reception**. Disable its receive-readiness interest while paused so
   unread traffic cannot cause a busy loop. Continue draining worker completions,
   transport/control events and the current request's timer. ZAP and monitor
   owners also continue; never sleep or block the I/O owner for ten seconds.
4. Worker capacity/start/completion transitions signal the existing pollable
   wakeup. Attempt handoff when capacity changes, retaining the original
   deadline. Before service execution, Dispatch atomically claims
   WAITING -> RUNNING only if the deadline has not expired. That claim disarms
   the worker-wait timer and lets the I/O owner resume receiving the next RPC.
5. If the deadline wins, atomically claim WAITING -> EXPIRED, log ERROR
   `worker_pool_exhausted`, and produce **RPC_STATUS_INTERNAL with empty payload**
   through the normal single-attempt response path. The expired request must
   never invoke Crypto or perform a buffer operation. Resume reception after
   the current record's handoff ownership is settled.

Keep the record alive across timer/handoff races. If a job has been handed to
Thunder but has not started, the worker must observe EXPIRED and return without
service execution or another response. Do not reuse its worker reservation or
lose its retained references before that handoff is reclaimed. In particular,
do not turn Thunder's internal dispatch queue into another application backlog:
submit only against reserved available execution capacity, and keep the current
ingress record until Dispatch claims it or its expired handoff is reclaimed.
There is no second Gateway queue or queue-full error.

Transport owners have no periodic idle-poll interval. ROUTER polls its socket
and eventfd indefinitely without a current deadline, or until the current
request's absolute worker-start deadline. Pending completions continue draining
without a blocking poll. Worker start/completion and registry changes signal the
owner. ZAP and monitor each poll their socket and a separate stop eventfd, so
shutdown wakes both even with no traffic. After transport failure, cleanup waits
for a state-revision change under the registry mutex to avoid missed wakeups;
unresolved cleanup does not introduce a retry timer.

The timer starts when Gateway admits this current request, **not** when the
client submits it or when libzmq first buffers it. Unread messages can remain
in libzmq/kernel buffers while reception is paused. They have no Gateway job or
Gateway worker-wait timer yet. Transport high-water marks and the client's
existing deadlines apply there. Removing the Gateway queue does not remove
libzmq's transport buffers or promise an end-to-end ten-second response time.

The ten-second timer is not a backend execution timeout. A request that already
claimed RUNNING completes through the usual execution/lease rules. Disconnect
or deactivation preserves accepted-work ownership; the one current request
keeps its original worker-start deadline, while running work drains normally.
Lifecycle control uses retained context state and its own wakeups; it must not
depend on receiving another application request or obtaining an ingress slot.

Use a version-appropriate nonblocking handoff with explicit ownership and
worker-start acknowledgement. Not every Thunder Submit overload reports enqueue
success; validate the adapter against the selected release. Never block ROUTER,
spin on a full pool, lose an unacknowledged job, or submit additional waiting
jobs behind the current one. Reference:
[Thunder pool implementation](https://github.com/rdkcentral/Thunder/blob/2c0fcc5529e7da734be558ca6efa05d934dcce31/Source/core/ThreadPool.h).

Detectable request/job/completion allocation failure returns RPC_STATUS_INTERNAL
when a correlated response can be constructed and sent. Release partial state
without leaving a request that can execute later. After execution begins, a
failure does not undo backend effects or end an unresolved buffer lease.
Client-local allocation failures remain CLIENT_OUT_OF_MEMORY.

### Contract: GR-03 Resource and transport policy

Source: [GW-008](../../../doc/EyilGateway.md#initial-resource-policy).

This table states the implementation policy. **Unlimited** means no additional
EyilClient/Gateway count or byte admission cap for that resource. Memory
allocation can still fail; representation limits and overflow-safe arithmetic
in the C API, Protobuf, libzmq and service interfaces still apply.

| Resource | Initial value / policy |
| --- | --- |
| Maximum complete request / response | Unlimited; bulk Crypto data uses shared memory |
| Reserved/completed response slots | Unlimited |
| Reserved/completed response data | Unlimited |
| ZeroMQ send/receive high-water marks | Keep libzmq defaults: ZMQ_SNDHWM = 1000 and ZMQ_RCVHWM = 1000 messages per peer in libzmq 4.3.4, on ROUTER and DEALER |
| Private request workers | 4, configured independently of Thunder's global pool |
| Worker stack size | Keep the Thunder/platform default |
| Shared memory per buffer | At most 1.5 MiB = 1,572,864 bytes for the entire allocation |
| Shared buffers per application UID | At most 4 allocations across all connections with the same trusted host UID, including child apps sharing it; at most 6 MiB = 6,291,456 bytes combined |
| Global shared memory | Unlimited; no additional Gateway-wide shared-memory quota |
| Kernel socket buffers | Keep OS defaults: ZMQ_SNDBUF = -1 and ZMQ_RCVBUF = -1 |

There are no Gateway waiting-request-count or queued-request-byte settings.
[Section 6.2](../../../doc/EyilGateway.md#62-thread-model-and-private-thunder-pool)'s single-current-request pause and ten-second worker-start deadline
provide overload handling instead. HWM is a transport message-count threshold
per peer, not a byte budget or a private-worker count. It does not create a
Gateway request queue. The libzmq defaults are specified in
[ZeroMQ 4.3.4 socket options](https://github.com/zeromq/libzmq/blob/v4.3.4/doc/zmq_setsockopt.txt).

Apply no configurable maximum RPC/message/text size; set ZMQ_MAXMSGSIZE=-1 on
DEALER and ROUTER. Preserve required framing, schema, enum, numeric-conversion
and service-parameter validation. Reject an input that cannot be represented
by the actual parsing/transport API safely; do not truncate lengths or bypass
integer-overflow checks. Invalid client input uses CLIENT_INVALID_ARGUMENT;
detected client allocation failure uses CLIENT_OUT_OF_MEMORY. Gateway failures
use the existing infrastructure/service error layers in [section 4.2](../../../doc/EyilSecureGateway.md#42-status-and-error-layers).

Response bookkeeping/storage grows as needed without slot or byte reservations
against policy limits. Publishing a completed job must not wait for client
receipt. On allocation failure, log the stage and use the existing error/drop
path; never retry the backend operation to recover response storage.

Shared-memory limits count the entire allocation, including input, output and
alignment space, and remain charged through deferred cleanup. The limit is
1.5 MiB (1,572,864 bytes) per buffer and four buffers per application UID across
all its connections. The resulting maximum is 6 MiB (6,291,456 bytes), not an
additional independent byte-budget setting. [Section 6.10](../../../doc/EyilGateway.md#610-shared-memory-identity-records-and-invariants) defines shared quota
accounting separately from connection ownership.

### Contract: GR-04 Ingress validation and accepted ownership

Source: [GW-012](../../../doc/EyilGateway.md#66-request-ingress-dispatch-and-response-delivery).

The ROUTER owner serializes admission with deactivation. Accepted work remains
owned during drain. Validate framing, safe message-length representation,
trusted metadata, live context, schema 1, service, request ID, service structure,
operation/oneof agreement, enum/range validity and allocation ownership. There
is no additional request/response byte cap. Preserve the original completion
when a request ID is duplicated. Independent operations may finish out of order.

Prepare owned job/completion bookkeeping before execution, without reserving
capacity against count/byte budgets. Complete buffer validation before committing
release; a rejection before that point creates no releasePending transition.
If a job acquired an active-use lease but fails before calling Crypto, FinishUse
ends that unused lease and may enable deferred cleanup. Response preparation or
delivery failure after release acceptance cannot undo the accepted release.

String calls carry service=RPC_SERVICE_UNSPECIFIED, schema 1, a nonzero ID and
raw text payload. Once received and structurally validated, Gateway logs ERROR
unsupported_service and returns RPC_STATUS_UNSUPPORTED_SERVICE with echoed
ID/service/schema and empty payload. No worker, Crypto call or buffer operation
is needed. A receive pause can delay reading this request too; Gateway cannot
classify an unread message. Common invalid-context/schema/duplicate-ID rules
take precedence. Do not sniff formats or translate text into Crypto operations.

All admission errors use normal response allocation, original-transport
validation and one send attempt. If allocation or delivery fails, log/drop;
the sync caller may time out and an accepted async call may receive no callback.

### Contract: GR-05 Completion handoff

Source: [GW-013](../../../doc/EyilGateway.md#response-ownership-and-completion-handoff).

Private workers publish completed owned records to the ROUTER owner through
a synchronized handoff and existing wakeup. This holds results awaiting their
single send attempt; it is not an application request waiting queue or a reply
retry queue. **Response record count and response bytes are unlimited by policy.**
There are no per-connection/global slot credits, response-byte budgets or
configured serialized-response maximum.

Allocate completion bookkeeping with the request so publishing a completed
record does not need another queue-node allocation. Grow response storage as
needed, with checked size arithmetic and the underlying API representation
limits. Allocation/serialization failure uses RPC_STATUS_INTERNAL if a valid
correlated error can be prepared; otherwise log the local failure and retire
the response. Never rerun the operation, undo an accepted release or free an
unresolved backend lease to recover response storage.

Workers do not wait for clients or ROUTER sends. The owner processes finite
batches of ingress/completions between checks of control events and timers,
including while receiving is paused for worker capacity. This scheduling rule
adds no request-count or storage admission limit. Release each result once on send acceptance, failed send
or stale-route disposal. libzmq's default HWM applies to its separate transport
queues; it is not a completion-storage budget.

### Contract: GR-06 Single nonblocking response attempt

Source: [GW-014](../../../doc/EyilGateway.md#one-response-send-attempt).

**Gateway attempts each response once, nonblocking. On failure it records a
local delivery error and drops the response.** There are no retained failed
replies, per-connection retry queues, send-retry timers or second attempts.

Configure ROUTER_MANDATORY=1 and use ZMQ_DONTWAIT for the multipart response.
A full route reports EAGAIN and an unreachable route reports EHOSTUNREACH;
neither is permission to replay the operation.
[ROUTER send behavior](https://libzmq.readthedocs.io/en/latest/zmq_setsockopt.html#zmq-router-mandatory-accept-only-routable-messages)

| Outcome | Gateway action |
| --- | --- |
| Original context/route is already retired or replaced | Discard the stale completion without sending it to another connection |
| Complete response accepted by libzmq | Release Gateway completion storage/accounting; do not wait for client receipt |
| EAGAIN | Record output-backpressure failure, drop this response and continue other work; do not infer process death or disconnect solely for this result |
| EHOSTUNREACH | Record unreachable-route failure and drop; connection cleanup uses the authenticated lifecycle path |
| EINTR or another send error | Record the failed attempt, drop without a retry and apply any required socket/runtime error handling |
| Completion preparation/publication fails | Record local failure and release owned state without blocking a worker or starting another backend operation |

"One attempt" applies to the complete response message, whose ROUTER framing is
[routing_id][RpcResponse bytes], not to a single frame. Prepare both frames
before starting, use SNDMORE on the route frame, and never interleave another
response. If routing-frame submission fails, do not submit its payload. If a
later frame fails, preserve or terminate the socket's multipart state safely
under the verified libzmq contract before sending unrelated traffic; never
restart that response. The target transport tests must cover these frame-level
failure paths. Success confirms local libzmq acceptance, not peer receipt.
[Send and multipart contract](https://libzmq.readthedocs.io/en/latest/zmq_send.html)

Apply this policy to successful results and all Gateway error replies, including
ten-second worker-capacity timeout errors. Do not attempt to send a
second RPC_STATUS_INTERNAL to report the failure of the first response.
Record the request ID, connection generation and local send error in bounded
diagnostics/counters, without logging Crypto payloads. Drop releases response
storage, not an allocation still leased to an active backend operation.

The client cannot observe this local send failure through a response that was
not delivered. If the transport stays open, a synchronous call reaches its
15-second CLIENT_TIMEOUT; an accepted async request has no response timeout and
may retain its pending record until local teardown or connection loss, without
a response callback. A monitor reports connection events, not individual
dropped replies. This policy does not introduce an async error callback or
unsolicited notification.

libzmq's default output HWM applies to accepted transport messages separately
from the unlimited-by-policy completion handoff. Gateway does not reserve a
response slot/byte allowance or wait for a frozen peer to read a reply.

### Contract: GR-07 Unexpected frozen or slow receiver

Source: [GW-045](../../../doc/EyilGateway.md#gateway-behavior-if-a-client-stops-receiving).

For an idle freeze with the connection preserved, Gateway has no operation to
complete and no response to deliver to that client. It retains the context and
idle resource allocations while other clients continue. Retained buffers are
expected state, not evidence that disconnect cleanup failed; apply
the per-host-UID quota across all connections of each application group. Size
platform memory/tmpfs for the expected UID groups; no Gateway-wide shared-memory
cap is selected.

The design must still contain failures when an app freezes unexpectedly or
violates its preparation contract:

- Attempt each completed response once through the ROUTER owner with DONTWAIT.
  If accepted, it may remain in libzmq's bounded output queue. If rejected,
  record the local error and release the response; Gateway retains no retry.
- EAGAIN is output pressure, not proof of death or a reason to expire a frozen
  connection. Drop only the failed response and continue processing other work.
  The client may receive nothing and no connection monitor event is implied.
- A frozen client must not occupy workers waiting to deliver replies. Workers
  publish owned completion records without count/byte admission caps; ROUTER
  makes one nonblocking send attempt. libzmq retains its default HWM. The
  ten-second worker-wait timer does not expire completed responses or messages
  still buffered in libzmq. Client submission uses one slot; its pending-response
  map remains dynamic.
- If an operation is still executing, retain its context and buffer leases
  until its actual completion. Freezing, an undelivered reply or a local timeout
  must not invalidate backend memory access. On thaw, a synchronous request whose
  deadline elapsed can report CLIENT_TIMEOUT even if backend
  execution finished during the pause. Async response waiting has no deadline.
- A real peer exit or socket failure still requires the normal correlated
  disconnect cleanup. If Gateway fails while the app is frozen, the app learns
  about the transport failure after it resumes; preservation is not guaranteed
  across that independent failure.

The response policy is defined in [section 6.6](../../../doc/EyilGateway.md#66-request-ingress-dispatch-and-response-delivery): owned completion handoff without
count/byte caps, one send attempt, local error and drop. No freeze
timeout or forced disconnect for EAGAIN is introduced.

## Related contracts and open decisions

[RP-02 Status layers and correlated Gateway failures](../rpc-protocol/spec.md#requirement-rp-02-status-layers-and-correlated-gateway-failures), [RP-05 Unsupported raw-text wire contract](../rpc-protocol/spec.md#requirement-rp-05-unsupported-raw-text-wire-contract), [CI-03 FD and transport-lifetime binding](../connection-identity/spec.md#requirement-ci-03-fd-and-transport-lifetime-binding), [CI-04 Required libzmq extension and guarded replies](../connection-identity/spec.md#requirement-ci-04-required-libzmq-extension-and-guarded-replies), [SM-02 Shared trusted-host-UID quota](../shared-memory/spec.md#requirement-sm-02-shared-trusted-host-uid-quota), [GL-05 Admission cutoff and graceful drain](../gateway-lifecycle/spec.md#requirement-gl-05-admission-cutoff-and-graceful-drain), [DG-02 Worker capacity and retained-cleanup diagnostics](../diagnostics/spec.md#requirement-dg-02-worker-capacity-and-retained-cleanup-diagnostics).

- [API-AUTHORIZATION](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
- [FUTURE-TEXT-SERVICE](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
