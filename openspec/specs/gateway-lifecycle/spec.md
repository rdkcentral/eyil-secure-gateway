<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# gateway-lifecycle specification

## Purpose

Define wrapper and implementation ownership through activation, partial failure, orderly drain and fresh startup.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC1.1, AC1.3, AC1.7, AC1.12. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: GL-01 Wrapper and implementation ownership

The in-process wrapper SHALL own the sole IConfiguration reference and use final Release/destruction to drain implementation state before releasing its shell.

The detailed [contract](#contract-gl-01-wrapper-and-implementation-ownership) is normative for this requirement.

#### Scenario: Wrapper and implementation ownership outcome

- **WHEN** Thunder deactivates the implementation while a matching notification callback overlaps
- **THEN** shell/connection state is synchronized, retained shell work is submitted outside the lock, and final implementation destruction occurs on the framework lifecycle thread rather than a worker it must join.

### Requirement: GL-02 Runtime records and synchronization

Gateway SHALL retain the identity, admission, resource and lifecycle records below and serialize their ownership transitions.

The detailed [contract](#contract-gl-02-runtime-records-and-synchronization) is normative for this requirement.

#### Scenario: Runtime records and synchronization outcome

- **WHEN** new admission closes while accepted work still retains a connection
- **THEN** accepted jobs continue against the original context/generation and execution-time buffer state; removing public lookup does not select a replacement context or cancel already accepted work.

### Requirement: GL-03 Startup, configuration failures and rollback

Gateway SHALL publish RUNNING only after required startup ownership and checks succeed, and roll back partial startup through the same destruction discipline.

The detailed [contract](#contract-gl-03-startup-configuration-failures-and-rollback) is normative for this requirement.

#### Scenario: Startup, configuration failures and rollback outcome

- **WHEN** production bind or the required capability self-test fails
- **THEN** activation reports an error after owned partial resources have been closed/joined; backend absence alone is not a Configure precondition and no separate endpoint listener is introduced.

### Requirement: GL-04 Correlated disconnect and backend notification order

Gateway SHALL begin teardown immediately on correlated disconnect, drain accepted access and perform matching backend notification before remaining allocation cleanup.

The detailed [contract](#contract-gl-04-correlated-disconnect-and-backend-notification-order) is normative for this requirement.

#### Scenario: Correlated disconnect and backend notification order outcome

- **WHEN** an authenticated connection crashes without ever sending an RPC or release request
- **THEN** Gateway closes only its admission, settles any CONNECTED/accepted-work obligation, issues matching DISCONNECTED when initialized and reclaims its remaining allocations without a client cleanup RPC.

### Requirement: GL-05 Admission cutoff and graceful drain

Deactivation SHALL block new admission while preserving accepted work, routes and resource ownership until the drain rules below are satisfied.

The detailed [contract](#contract-gl-05-admission-cutoff-and-graceful-drain) is normative for this requirement.

#### Scenario: Admission cutoff and graceful drain outcome

- **WHEN** deactivation starts while a routable accepted response and one waiting request remain
- **THEN** the response receives its normal single attempt, the current request retains its original worker-start budget, and transport/pool control stays alive through accepted work and cleanup.

### Requirement: GL-06 Socket and context teardown

Gateway SHALL close sockets on their owners, join threads and terminate its private context in the required order without unsafe forced completion.

The detailed [contract](#contract-gl-06-socket-and-context-teardown) is normative for this requirement.

#### Scenario: Socket and context teardown outcome

- **WHEN** a Crypto call never returns during graceful stop
- **THEN** Gateway does not kill its worker, unload executing code or free its leased buffer; bounded destruction is not promised for that backend failure.

### Requirement: GL-07 Reactivation and fresh startup after crash

Each reactivation or crash restart SHALL create fresh Gateway state and reject reuse of earlier generations or allocations.

The detailed [contract](#contract-gl-07-reactivation-and-fresh-startup-after-crash) is normative for this requirement.

#### Scenario: Reactivation and fresh startup after crash outcome

- **WHEN** Gateway starts after an earlier runtime crashed
- **THEN** it creates fresh state and buffer names without reconstructing sessions or blindly unlinking old allocations; any explicit client reconnect creates a fresh session without replay.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: GL-01 Wrapper and implementation ownership

Source: [GW-004](../../../doc/EyilGateway.md#61-plugin-and-implementation-organization), [GW-049](../../../doc/EyilGateway.md#states-and-explicit-stop-ownership), [GW-052](../../../doc/EyilGateway.md#router-and-context-teardown), [GW-016](../../../doc/EyilGateway.md#callsign-configuration-bitbake-to-cmake-to-runtime), [GW-005](../../../doc/EyilGateway.md#gateway-component-and-api-call-overview).

EyilGateway runs **in process** with Thunder. The thin
`EyilGateway` wrapper implements IPlugin and acquires
`EyilGatewayImplementation` using
`Root<Exchange::IConfiguration>()`, then calls `Configure(IShell*)`.
The default `configuration.root.mode` is `Off`. A future `Local` mode can use
the same split with matching framework proxy/stubs.

**There is no app-facing Gateway COM-RPC or JSON-RPC API.** IConfiguration is
the internal Thunder bootstrap contract. The wrapper owns the sole implementation
interface reference. Its final Release invokes the implementation destructor,
which drains and destroys the runtime before releasing its retained IShell.
No additional lifecycle interface is introduced. ICrypto calls form a separate
trusted backend boundary. "No public Gateway COM-RPC API" does not mean there
are no framework/backend interfaces internally.

The wrapper owns the sole IConfiguration reference to the implementation. During
Deinitialize it releases that reference on the framework lifecycle thread. The
final Release synchronously invokes EyilGatewayImplementation's destructor.
That destructor destroys the runtime first: it closes admission, drains accepted
work, performs session/buffer cleanup and joins every owned thread. Only then
may the implementation release its retained IShell and finish destruction.
The wrapper releases its own IShell after the implementation's Release returns.

Jobs retain runtime/context/resource state, not IConfiguration references; they
must not postpone the last interface Release or trigger destruction on a worker.
The framework serializes activation/deactivation. Do not release the final
implementation reference from a worker/socket thread that destruction must join.
Partial-start rollback destroys the same owned runtime and only initialized
resources. There is no additional lifecycle-control interface. Future remote
implementation support must separately validate final-release completion and
library lifetime before remote Terminate can be used.
Runtime failures request framework deactivation through the wrapper. Suppress
new lifecycle callbacks and quiesce registered ones without self-joining the
callback that initiated teardown.
The runtime reports its first running-state failure through an internal C++
callback. The implementation queues an IShell::Job(DEACTIVATED, FAILURE) on the
framework pool, which invokes wrapper teardown on a framework thread. Startup
rollback and an already requested stop do not schedule another deactivation.

The wrapper serializes access to its shell pointer and remote-connection ID with
`Core::CriticalSection`. Deinitialize detaches its owned references and clears
callback-visible state under that lock, then unregisters and drains outside it.
A matching deactivation callback retains an `IShell` reference under the lock
before submitting a framework job outside it. Creation/submission failure releases
the retained reference and logs an error. Framework lifecycle calls themselves
remain serialized by Thunder; this protects their overlap with remote notifications.

The wrapper returns its initialization error without calling Deinitialize.
Thunder must invoke Deinitialize after a failed Initialize; on R4 this requires
legacyinitialize=false. A later activation creates a fresh implementation.
Returning an error does not itself schedule automatic reactivation.

These are recommended responsibilities, not a requirement for one source file
per row. Keep explicit module/config/version files, a thin wrapper and separate
implementation sources. Use internal interfaces at unfinished integration points
to isolate AppManager integration. Use topic/crypto ICrypto types only within the
Crypto adapter; keep them out of EyilClient, transport and buffer ownership.
Gateway does not poll Crypto health or subscribe to Crypto availability events.
The wrapper notifications for Gateway's own implementation lifetime remain
separate from Crypto and are still required.

### Contract: GL-02 Runtime records and synchronization

Source: [GW-009](../../../doc/EyilGateway.md#63-runtime-records-and-synchronization).

Each connection context contains:

| Field group | Required information |
| --- | --- |
| Identity | Unique User-Id, activation and connection generations, interim appId and sessionId |
| Transport binding | Gateway-side accepted FD plus immutable transport lifetime, bound to User-Id before ZAP success; FD is metadata owned by libzmq |
| Peer | Trusted host UID/GID/PID; process-instance evidence if available |
| Routing | Binary ROUTER routing ID plus a proven binding to this connection generation |
| Admission | AUTHENTICATING, ACTIVE, DRAINING, CLOSING or RETIRED; admission policy and current/handoff/in-flight bookkeeping |
| Crypto context | Per-connection initialization/drain state and last observed call outcome; no ICrypto pointer or background plugin-health state |
| Resources | Owned buffer IDs, quota key equal to trusted host UID, execution-time active-use leases, pending releases and cleanup accounting; jobs awaiting execution do not reserve buffers |
| Lifecycle | Correlated closing reason and exactly-once cleanup/notification state |

Preserve runtime app-instance identity separately when a future platform API
supplies it. The interim per-connection sessionId is not evidence of a real
launcher-provided app instance. Never derive uniqueness from UID, PID or a
reusable numeric FD alone.

Each RequestJob retains an immutable request plus `User-Id`, connection
generation, request_id, expected service and the required resource references.
A completion carries the same identity tuple and activation generation; workers
do not look up "whatever connection now has this routing ID."

Required synchronization rules:

- The Gateway ROUTER I/O owner serializes admission, route publication and
  terminal connection transitions. ZAP/monitor workers submit normalized control
  events.
- ZAP must publish its context before returning acceptance; an acknowledgement
  or a protected registry insertion is needed so the first request cannot race
  ahead of its record. No backend call runs under the registry lock.
- SharedBufferManager protects lookup, ownership, active-use state and the
  shared per-host-UID quota map with short locks. Reserve allocation count and
  bytes atomically across all connections of that UID before creating an object;
  connection teardown cannot reset another connection's charges. A worker checks
  and acquires an idle buffer atomically when it executes; retaining a request
  before execution reserves no buffer dependency.
  Release atomically sets releasePending on a valid owned allocation, including
  one in use, and prevents subsequent use. Competing uses of a busy buffer and
  uses of an unavailable/release-pending buffer fail under [section 4.2](../../../doc/EyilSecureGateway.md#42-status-and-error-layers).
  Last-use completion and release compete for one finalization claim; neither
  can destroy the object while an active or unresolved access remains.
  [Section 6.13](../../../doc/EyilGateway.md#613-releasesharedbuffer-and-conflicting-buffer-operations) also orders finalization against connection teardown. No lock is
  held across ICrypto, filesystem or transport calls.
- CONNECTED runs once during authentication before a context becomes ACTIVE
  and ZAP returns success. RPC workers never perform lazy session creation.
  Keep notification bookkeeping, not internal Crypto key state or health data.
- Closing new admission does not cancel accepted jobs. During deactivation or
  a correlated peer disconnect, the current admitted request starts within its
  original 10-second worker-wait deadline
  or expires with an error; started jobs complete before resources are released.
- Accepted jobs use their retained context and original generation during drain;
  removal from new-request lookup must not make them resolve a replacement
  context or fail solely because admission is now closed. Buffer dependencies
  and validation still apply. A disconnected route cannot receive a completion.
- A session removed from public lookup may remain privately retained by accepted
  jobs and lifecycle tasks. Purge entitlement/admission visibility immediately;
  retain only what those jobs and cleanup need.

These rules require synchronized records, not an ordering guarantee between
accepted operations. [Section 6.13](../../../doc/EyilGateway.md#613-releasesharedbuffer-and-conflicting-buffer-operations) defines the outcome when release overtakes use.
The client's DEALER I/O owner cannot enforce Gateway buffer ownership or stop
an application from writing through its own mapping.

### Contract: GL-03 Startup, configuration failures and rollback

Source: [GW-010](../../../doc/EyilGateway.md#64-startup-and-partial-start-failure), [GW-016](../../../doc/EyilGateway.md#callsign-configuration-bitbake-to-cmake-to-runtime).

When Crypto is included, platform/Thunder lifecycle coordination should make it
available before client use and retain it through Gateway's final DISCONNECTED
calls. Gateway startup and transport admission also work without Crypto; the
platform's preferred ordering is not a Configure precondition. Configure stores
the backend list; each notification and operation acquires its configured
interface on demand. There is no cached
ICrypto pointer, availability observer or startup polling. AppManager/RuntimeManager
lookup is not called in this phase.

Run the isolated capability self-test in [section 6.14](../../../doc/EyilGateway.md#614-disconnect-detection-pid-observations-and-hibernation) before creating the
production listener. It uses its own context and minimal test authenticator,
never Crypto, AppManager or the production registry. Its private directory is
created under IShell::VolatilePath(). Then start the private
pool, ZAP and monitor collector before binding ROUTER. Use standard zmq_bind
at the unchanged AC endpoint under the trusted
[deployment assumptions](../../../doc/EyilSecureGateway.md#74-settled-deployment-and-scope-assumptions).
Thunder owns single-instance activation and container policy supplies the correct
endpoint mount. Publish
RUNNING only once all owners are ready; a racing handshake cannot create an
ACTIVE context while startup admission is closed. Each activation gets fresh
registries, counters, generation, wakeups, sockets and its own ZMQ context.

Before binding ROUTER, explicitly set ZMQ_HEARTBEAT_IVL, ZMQ_HEARTBEAT_TIMEOUT
and ZMQ_HEARTBEAT_TTL to 0, matching client setup in [section 5.5](../../../doc/EyilClient.md#55-client_connect). Use the PAIR
monitor for transport events and event-driven cleanup. There is no application
heartbeat worker or inactivity watchdog.

Partial rollback uses the same runtime-destruction discipline as [section 6.16](../../../doc/EyilGateway.md#616-gateway-deactivation-reactivation-and-fresh-startup-after-a-crash). Create
stop state and wakeups before starting threads; close/join only resources
actually created/started. Failed Configure returns only after rollback completes.

Bind failure logs ERROR and fails activation through the same rollback path.
Standard libzmq listener ownership covers bind/close; no separate native listener
or endpoint lock is introduced. Container policy exposes the current socket
across endpoint recreation, as assumed in sections 3.3 and 7.4.

Gateway code does not explicitly throw exceptions. Validation, syscall and
transport failures return checked errors; Configure maps invalid settings to
ERROR_BAD_REQUEST and startup failures to ERROR_GENERAL after rollback.
Catch exceptions from external operations, including Protobuf/STL allocations,
thread construction and ICrypto invocation, and convert them to the appropriate
method result. A null backend lookup is a per-invocation error.

### Contract: GL-04 Correlated disconnect and backend notification order

Source: [GW-043](../../../doc/EyilGateway.md#event-driven-connection-cleanup).

On correlated disconnect, mark that context CLOSING immediately, close its
admission and remove public authentication/routing visibility. Retain resources
for accepted work. The one current admitted request starts within its original
worker deadline or expires; handed-off records retain ownership until reclaimed,
and running operations finish before destructive connection cleanup.
There is no reconnect grace period or idle/process-responsiveness timeout.

At that cutoff, establish SharedBufferManager's lifecycle finalization gate as
specified in [section 6.13](../../../doc/EyilGateway.md#613-releasesharedbuffer-and-conflicting-buffer-operations). It covers still-unclaimed pending releases and all
remaining connection allocations; it does not invalidate unreleased buffers
needed by accepted jobs. Finish any already-claimed explicit-release finalizer
as part of draining that work. Last-use completion records cleanup readiness;
remaining destruction follows the DISCONNECTED notification stage, without
occupying a worker while waiting for another job.

**App crash or unexpected disconnect:** the app need not send any release
requests. When its connection closes, the monitor's FD/lifetime binding selects
the original context and Gateway performs the same cleanup as for an explicit
client disconnect. Settle accepted work, notify Crypto with DISCONNECTED and
release the connection's remaining allocations after active access ends. This
includes allocations whose response never reached the app. Release through
retained directory/object references rather than reopening the exited process's
/proc/PID/root path. The application trust assumption does not remove this
Gateway responsibility. Actual target behavior is verified by the crash and
disconnect scenarios in [section 6.17](../../../doc/EyilGateway.md#617-implementation-order-and-verification-targets).

Discard completed responses waiting for the closed route and release their
response storage. Later completions from retained old jobs are discarded on
the same generation check. An explicit client reconnect authenticates a new
context; never transfer the old current request, response storage or buffer ownership to
it. Old resources remain retained only as needed for safe backend completion
and cleanup, independently of the new context.

CONNECTED is attempted during authentication, so a successfully initialized
backend context still receives matching cleanup if the client sends no RPC.
Local resources are also cleaned when initialization failed. If closure races CONNECTED, await
its outcome and retire the provisional context without admitting requests.
Do not infer that a failed notification cancelled an outstanding backend call.
Public admission/route removal is immediate; physical destruction waits for
retained jobs and leases, followed by the notification stage. Pending explicit
releases remain owned throughout this path, even if their responses were lost;
they do not require another app request or generate a later notification.
A returned invocation error does not, by itself,
establish that a remote operation stopped. Keeping private cleanup state for
that uncertainty does not re-enable the old connection or introduce Crypto
health polling.

### Contract: GL-05 Admission cutoff and graceful drain

Source: [GW-048](../../../doc/EyilGateway.md#616-gateway-deactivation-reactivation-and-fresh-startup-after-a-crash), [GW-050](../../../doc/EyilGateway.md#admission-and-drain-contract).

**Block new admissions, finish accepted work, close client connections, stop
ZeroMQ and owned threads, then destroy the runtime.** This requirement is
part of Thunder plugin ownership and needs no app lifecycle subscription.

- Serialize RUNNING to DRAINING with request acceptance. An accepted request has
  its owned job/context/accounting published before the cutoff. Work still in
  a client's submission slot or in libzmq without Gateway admission is not in that set.
- Block new RPC and authentication admission. Reply to a trustworthy rejected
  RPC with RPC_STATUS_INVALID_CONNECTION when its route permits; otherwise
  discard it safely. Do not invent a disconnect message or new status.
- Continue running jobs and retain the one current admitted request. Its
  original ten-second worker-start deadline still applies; expiry logs
  worker_pool_exhausted, produces RPC_STATUS_INTERNAL when deliverable and
  prevents later execution. Reclaim any expired handoff before releasing its
  ownership. This deadline and deactivation do not cancel running jobs.
- At the admission cutoff, establish each context's buffer-finalization gate.
  Previously claimed explicit-release cleanup finishes with its job; other
  pending releases retain their records and quota for cleanup after DISCONNECTED.
  Do not wait for this gated destruction before scheduling DISCONNECTED: first
  wait for accepted jobs and active-use obligations to settle, then notify and
  finalize, as in [section 6.13](../../../doc/EyilGateway.md#613-releasesharedbuffer-and-conflicting-buffer-operations).
- Keep ROUTER, ZAP, monitor processing, dispatcher and private pool running while
  draining. Do not unbind/close ROUTER or shut down its context at drain start.
  An empty current-request record is insufficient: account for handed-off/active jobs, authentication
  already in progress, lifecycle jobs, completions, buffer leases and claimed
  finalizers. A release response being sent is not proof of completed cleanup.
- Preserve routes for existing responses. If a peer disconnects, retire its
  route but finish its accepted work and cleanup obligations. A late ZAP result
  cannot publish a new ACTIVE context after the cutoff.
- Wait without registry/dispatcher/completion locks. Schedule lifecycle cleanup only when its
  prerequisites finish, rather than occupying every worker waiting for work
  still awaiting those workers.
- Apply the normal one-attempt response rule during drain. Give each routable
  completion its single nonblocking attempt, then release it on acceptance or
  local failure. Completion means execution/ownership settled, not app receipt.
  Never block a worker on delivery or retry a failed response. Retire any
  remaining unsent storage when entering STOPPING; no receipt ACK is required.
- After accepted work ends, send DISCONNECTED once per initialized context and
  mark all remaining allocations releasePending. Finalize those whose backend
  access has ended, including earlier accepted releases and failed-cleanup
  records. Include connections that authenticated but never sent an RPC.
  Release quota once physical cleanup completes. Unresolved access or cleanup
  errors retain private ownership and must not be reported as completed teardown.

### Contract: GL-06 Socket and context teardown

Source: [GW-052](../../../doc/EyilGateway.md#router-and-context-teardown).

Set ZMQ_LINGER=0 on all application-owned Gateway sockets when created.
Execution drains before closure; linger is not an operation or delivery ACK.
Zero linger prevents unsent traffic to a frozen client from prolonging transport
teardown. Close is asynchronous.
[Socket close contract](https://libzmq.readthedocs.io/en/latest/zmq_close.html)

Only the ROUTER owner disables monitoring and closes ROUTER. Never close
registry peer FDs directly, close ROUTER from Deinitialize, or use
zmq_disconnect on a bound listener. Enumerate all registered contexts directly
during stop; local ROUTER closure need not produce one DISCONNECTED event per
client. The monitor worker must not await MONITOR_STOPPED indefinitely.

After ROUTER closes and no response/authentication exchange remains necessary,
zmq_ctx_shutdown interrupts blocked socket operations with ETERM. Owners treat
ETERM during stop as expected and still close their own sockets exactly once.
They also have an existing stop flag and pollable wakeup; shutdown must not
depend on allocating a queue entry or receiving one last monitor event.
[Context shutdown](https://libzmq.readthedocs.io/en/latest/zmq_ctx_shutdown.html)

Join socket owners before zmq_ctx_term. Termination waits for socket closure and
linger; retry EINTR. Do not share the Gateway shutdown context with other
Thunder plugins. Once shut down, a context cannot be restarted; reactivation
creates a fresh one.
[Context termination](https://libzmq.readthedocs.io/en/latest/zmq_ctx_term.html)

The standard libzmq IPC listener owns its native socket and pathname cleanup.
Gateway closes the ZMQ socket on its owner and completes context teardown;
it does not manage a second adopted listener FD or endpoint lifecycle lock.
Thunder and container policy supply the deployment guarantees in [section 7.4](../../../doc/EyilSecureGateway.md#74-settled-deployment-and-scope-assumptions).

Partial-start rollback applies the same order to only the resources that exist.
No close/join assumes that startup reached the next stage. Successful teardown
leaves no application worker, ZMQ socket, libzmq thread or callback using plugin
code.

A graceful stop waits for actual backend completion. The client timeout and
Gateway worker-wait deadline cannot terminate a running ICrypto call. If it
never returns, do not force-kill its thread, unload code or free leased memory.
Crypto/platform failure escalation is outside Gateway policy; bounded
destruction is not promised for a non-returning backend.

### Contract: GL-07 Reactivation and fresh startup after crash

Source: [GW-053](../../../doc/EyilGateway.md#reactivation-and-fresh-startup-after-a-crash).

Reactivate only after normal stop reaches STOPPED. Reset per-activation flags
and recreate the current-request state, completion handoff, registry, identities,
pool, sockets and context. Old events
or completions must not affect the new generation even if FD numbers or routing
IDs repeat. Rebinding can create a new socket inode; container mounts must
expose the new endpoint as required in section 3.3.

After a crash, use exactly the normal startup path with empty Gateway state.
Do not reconstruct old connections, requests, buffers or sessions, and do not
wait for old clients. Thunder/platform owns crash detection and relaunch. An
in-process crash may also terminate Thunder; its restart precedes activation.

Actual UDS closure is detected by the client monitor without heartbeats. A
frozen client processes it after resuming. Pending sync/send waiters fail;
accepted async records retire without synthetic response callbacks. The app may
close the old Client from an application thread and call client_connect again.
Automatic reconnect is disabled. Close with zero linger, discard old pending
requests and undelivered responses, and establish a fresh session as specified
in [section 5.14](../../../doc/EyilClient.md#514-deadlines-callbacks-reconnect-and-library-unloading). Old-session operations must not be replayed.

Use fresh buffer names after restart. Do not adopt old objects or blindly
unlink allocations from a previous runtime. Persistent leftover storage and
Crypto's internal recovery belong to the platform/backend; Gateway does not
implement restoration of old sessions.

## Related contracts and open decisions

[CC-07 Terminal loss and explicit fresh connection](../client-connection/spec.md#requirement-cc-07-terminal-loss-and-explicit-fresh-connection), [CI-05 Startup capability verification before admission](../connection-identity/spec.md#requirement-ci-05-startup-capability-verification-before-admission), [CY-03 Authentication-time CONNECTED and session bookkeeping](../crypto-integration/spec.md#requirement-cy-03-authentication-time-connected-and-session-bookkeeping), [SM-05 Execution-time access and definitive completion](../shared-memory/spec.md#requirement-sm-05-execution-time-access-and-definitive-completion), [SM-07 Deferred finalization and cleanup failures](../shared-memory/spec.md#requirement-sm-07-deferred-finalization-and-cleanup-failures), [DG-02 Worker capacity and retained-cleanup diagnostics](../diagnostics/spec.md#requirement-dg-02-worker-capacity-and-retained-cleanup-diagnostics).

- [SHM-COMPLETION](../../../doc/EyilSecureGateway.md#71-abnormal-backend-completion-lifecycle-integration-boundary): unresolved; retain the safeguards above without inventing the missing integration contract.
- [AUTHENTICATION-PROGRESS](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
