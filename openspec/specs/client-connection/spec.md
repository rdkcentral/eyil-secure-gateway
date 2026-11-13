<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# client-connection specification

## Purpose

Define Client handle ownership, readiness, transport events, explicit shutdown, fresh connections and quiescence.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC2-Client.1, AC2-Client.2, AC2-Client.3, AC2-Client.4. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: CC-01 Connection ownership and application lifetime

Each Client SHALL own its connection runtime while the application controls explicit connection lifetime.

The detailed [contract](#contract-cc-01-connection-ownership-and-application-lifetime) is normative for this requirement.

#### Scenario: Connection ownership and application lifetime outcome

- **WHEN** an application leaves a connection idle after completing work
- **THEN** no inactivity timer expires it; the Client owner continues monitor duties without an extra monitor thread or an application pump.

### Requirement: CC-02 Connect handle and readiness

client_connect SHALL validate the caller's handle slot and publish a new handle only after successful handshake readiness.

The detailed [contract](#contract-cc-02-connect-handle-and-readiness) is normative for this requirement.

#### Scenario: Connect handle and readiness outcome

- **WHEN** client_connect receives a valid existing handle, including a LOST handle
- **THEN** it returns CLIENT_ALREADY_CONNECTED and preserves the handle without another runtime or callback; a fresh empty slot publishes a handle only after HANDSHAKE_SUCCEEDED.

### Requirement: CC-03 Connection event callback timing

The connection handler SHALL report only an eligible later unexpected loss under the following publication and cutoff rules.

The detailed [contract](#contract-cc-03-connection-event-callback-timing) is normative for this requirement.

#### Scenario: Connection event callback timing outcome

- **WHEN** a successful initial handshake is followed immediately by unexpected transport loss
- **THEN** the initial result has no callback; loss is retained until handle publication and may produce CLIENT_CONNECTION_LOST once. Explicit close permits no newly claimed callback.

### Requirement: CC-04 Socket configuration and disabled heartbeats

The socket owners SHALL apply the following fresh-connection settings, including disabled heartbeats on both ends.

The detailed [contract](#contract-cc-04-socket-configuration-and-disabled-heartbeats) is normative for this requirement.

#### Scenario: Socket configuration and disabled heartbeats outcome

- **WHEN** a DEALER or ROUTER is recreated
- **THEN** heartbeat IVL/TIMEOUT/TTL remain zero; the DEALER uses RECONNECT_IVL=-1, IMMEDIATE=1 and LINGER=0, and old transport queues are discarded by owner-side close.

### Requirement: CC-05 Initial connection outcomes and rollback

client_connect SHALL return the mapped observed startup outcome exactly once and complete safe partial-start rollback.

The detailed [contract](#contract-cc-05-initial-connection-outcomes-and-rollback) is normative for this requirement.

#### Scenario: Initial connection outcomes and rollback outcome

- **WHEN** a pending handshake reaches its deadline without an explicit denial
- **THEN** the caller receives CLIENT_TIMEOUT with a NULL handle after rollback; an unavailable endpoint or missing status-300 client event is not inferred to be authentication denial.

### Requirement: CC-06 Explicit disconnect barrier and raw-handle coordination

client_disconnect SHALL close local admission, settle retained calls/callbacks and destroy the Client under the following synchronization contract.

The detailed [contract](#contract-cc-06-explicit-disconnect-barrier-and-raw-handle-coordination) is normative for this requirement.

#### Scenario: Explicit disconnect barrier and raw-handle coordination outcome

- **WHEN** explicit disconnect wins while a sync call waits and another async send is already accepted
- **THEN** the sync call receives CLIENT_NOT_CONNECTED with cleared outputs; the async send keeps CLIENT_OK. Cleanup waits for claimed callbacks/live calls, without allocating a stop request or certifying remote completion.

### Requirement: CC-07 Terminal loss and explicit fresh connection

Unexpected connection loss SHALL permanently retire that Client generation; any later connection is an explicitly created fresh runtime.

The detailed [contract](#contract-cc-07-terminal-loss-and-explicit-fresh-connection) is normative for this requirement.

#### Scenario: Terminal loss and explicit fresh connection outcome

- **WHEN** a connection is lost and the application later closes its old handle and connects again
- **THEN** old pending work, transport queues, buffer IDs and session key handles are not restored or replayed; the new runtime starts with an empty slot and pending map.

### Requirement: CC-08 Prepared process freeze and library unload

Applications SHALL meet the quiescence conditions below before freezing a live connection or unloading EyilClient.

The detailed [contract](#contract-cc-08-prepared-process-freeze-and-library-unload) is normative for this requirement.

#### Scenario: Prepared process freeze and library unload outcome

- **WHEN** an application has stopped new submissions and all operations/callbacks have definitively finished
- **THEN** freezing with descriptors preserved introduces no heartbeat/idle expiry; unloading additionally requires all Clients disconnected, threads/contexts quiescent and returned response allocations freed.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: CC-01 Connection ownership and application lifetime

Source: [CL-004](../../../doc/EyilClient.md#51-responsibilities-and-limits), [CL-005](../../../doc/EyilClient.md#52-component-organization-and-ownership), [CL-006](../../../doc/EyilClient.md#monitor-responsibilities).

The application chooses when to connect and disconnect. The recommendation is
to keep one connection for the app instance's lifetime, reusing it across
operations and idle periods; connecting explicitly on first use is also an
application choice. EyilClient does not connect automatically on a request or
expire an idle connection. Closing the connection triggers Gateway/Crypto
cleanup of its context and resources, even while the application remains alive.

Each Client has one library-owned serving thread and one owned ZMQ context.
libzmq also creates its own internal I/O threads: those do not maintain the
library's pending map,
run its callbacks or replace its serving thread.

There is no application heartbeat producer, heartbeat worker, periodic liveness
RPC or inactivity watchdog in EyilClient. Connection-state handling uses
the monitor events and the request coordinator. Connect, sync requests and async
send submission retain 15-second deadlines; async response waiting has none.
Built-in libzmq transport heartbeats are also disabled, as specified in
[section 5.5](../../../doc/EyilClient.md#55-client_connect).

The PAIR monitor receiver is an internal socket plus event-decoding code.
Enabling monitoring on DEALER makes libzmq publish connection, handshake and
disconnect events at the unique client monitor endpoint. The same Client I/O
owner connects the PAIR receiver in that context, polls it alongside DEALER and
the submission wakeup, and updates connection/request state. This is the agreed
monitor implementation; no additional client monitor thread is required.
[ZeroMQ monitor API](https://libzmq.readthedocs.io/en/latest/zmq_socket_monitor.html)

The following duties remain required under the consolidated ownership model.
Reading monitor events must not wait for an RPC response, and callbacks run
without internal locks. libzmq's internal transport threads produce the events;
the Client I/O owner consumes them and owns the pending-request map.

| Expected monitor responsibility | How PAIR plus the I/O owner cover it |
| --- | --- |
| Set up event collection before connecting | The DEALER owner enables monitoring at the unique client monitor endpoint; the same owner creates/connects its PAIR receiver in the same ZMQ context |
| Receive and decode complete monitor messages | Poll PAIR alongside DEALER and the submission wakeup; validate the event frames, event value and endpoint for the configured monitor API version |
| Establish handshake readiness | On HANDSHAKE_SUCCEEDED, publish READY and wake client_connect without an application callback; CONNECTED alone does not establish readiness |
| Report initial connection/authentication failures | Wake the connect waiter with the exact ClientResult from [section 5.5](../../../doc/EyilClient.md#55-client_connect), without a callback; distinguish authentication denial from transport/protocol failure |
| Handle loss of an established connection | Mark the connection unusable, stop admission, fail unresolved sync/send-result waiters with CLIENT_COMMUNICATION_ERROR, retire accepted async response records and report CLIENT_CONNECTION_LOST once if callback admission remains open |
| Handle later state transitions without duplicates | Serialize the terminal loss transition, retire the old generation once and ignore later events; explicit reconnect creates a new Client and session as specified in [section 5.14](../../../doc/EyilClient.md#514-deadlines-callbacks-reconnect-and-library-unloading) |
| Keep application callbacks and request retirement coordinated | Invoke connection handlers without internal locks, resolve each active API waiter once and retire affected async response records once; no synthetic async error callback |
| Handle monitor failure and orderly stop | Treat unexpected collector failure as a runtime error; during requested shutdown, suppress intentional-close callbacks, wake the poller, stop monitoring and close PAIR on its owner without waiting indefinitely for another event |

Connect, synchronous-request and async-send deadlines are handled by the same
owner/coordinator; monitor events alone do not implement those timers. Async
responses have no timer. Poll and process submissions, replies and monitor events
in finite batches so each progresses. Callbacks must return promptly because a
blocking callback also delays monitor handling.

Freezing the application stops the serving/I/O owner and libzmq's threads.
The agreed hibernation contract requires the app to finish operations and
callbacks first while preserving its descriptors. The monitor handles transport
state, not app responsiveness, Gateway-side ZAP or service-payload decoding.

### Contract: CC-02 Connect handle and readiness

Source: [CL-017](../../../doc/EyilClient.md#handle-ownership-and-client_already_connected), [CL-018](../../../doc/EyilClient.md#readiness-contract), [CL-022](../../../doc/EyilClient.md#connection-sequence).

The API treats `Client **client` as an input/output handle slot.
Initialize it before the first call, for example `Client *client = NULL`.

- If `client == NULL`, return CLIENT_INVALID_ARGUMENT without dereferencing it.
- If `*client != NULL` and contains a valid Client handle that has not been
  destroyed, return CLIENT_ALREADY_CONNECTED immediately. Preserve that handle,
  its registered handler, pending requests and connection state. Do not allocate
  a runtime, start threads, connect another socket or invoke a callback for the
  rejected call.
- If `*client == NULL`, start a new connection attempt. Publish the handle only
  on CLIENT_OK; on startup failure the slot remains NULL after rollback.

CLIENT_ALREADY_CONNECTED is local handle-ownership validation, not a Gateway
rejection or a restriction to one connection per process, UID or app. A separate
NULL-initialized slot may create another independent Client. An existing handle
still occupies its slot after transport loss; calling client_connect with it is
not a reconnect API and returns CLIENT_ALREADY_CONNECTED. To reconnect, call
client_disconnect on the old handle from an application thread, reset the slot
to NULL and call client_connect again. The new call creates a fresh runtime and
authenticated session; the old handle never returns to READY after loss.

The caller must serialize access to the same handle slot, including concurrent
connect attempts. Uninitialized, fabricated or destroyed handles are invalid
API usage. After client_disconnect destroys a handle, the caller must reset its
own slot to NULL before reusing it: client_disconnect takes Client*, so it cannot
clear the caller's variable.

**Agreed readiness contract:** return CLIENT_OK and publish a valid Client
handle only after local startup has succeeded and the client monitor receives
`ZMQ_EVENT_HANDSHAKE_SUCCEEDED` for the pending connection attempt. The
application can then immediately submit RPC requests. Gateway attempts
NotifyAppState(CONNECTED) during authentication, but a missing backend or failed
notification does not reject an otherwise valid transport. Backend RPCs report
lookup/invocation/service errors; local buffer RPCs remain available without Crypto.

The startup coordinator waits for handshake success or a terminal failure
within a **15-second connect deadline**. Deadline expiry returns CLIENT_TIMEOUT;
explicit authentication rejection returns CLIENT_AUTH_FAILED. [Section 5.14](../../../doc/EyilClient.md#514-deadlines-callbacks-reconnect-and-library-unloading)
defines the common duration constant and its application to each timed wait.

Startup state includes the empty slot, slotAvailable condition, stop flag and
wakeup needed for rollback. A detected
allocation failure during serving-thread creation or owner-side setup publishes
CLIENT_OUT_OF_MEMORY through that retained state. Other startup failures use
CLIENT_COMMUNICATION_ERROR. Cleanup only stops or joins threads that actually
started and does not allocate a shutdown action.

### Contract: CC-03 Connection event callback timing

Source: [CL-019](../../../doc/EyilClient.md#connection-callback-contract).

client_connect is synchronous. Its return value reports the initial connection
outcome; the connection handler reports only a later unexpected loss.

| Situation | API result | Connection callback |
| --- | --- | --- |
| Initial setup and authentication succeed | CLIENT_OK with a valid Client handle | None |
| Initial connection attempt fails | Exact ClientResult, including CLIENT_AUTH_FAILED for an explicit authentication denial; output remains NULL | None |
| An established connection is lost unexpectedly | Retire pending work using [section 5.14](../../../doc/EyilClient.md#514-deadlines-callbacks-reconnect-and-library-unloading); later request submissions return CLIENT_NOT_CONNECTED | CLIENT_CONNECTION_LOST once, if the registered handler can be claimed before explicit close shuts callback admission |
| Application calls client_disconnect | Synchronous local cleanup, void return | None for the intentional close; an already-claimed loss callback may finish before disconnect returns |

CLIENT_CONNECTION_RESTORED and CLIENT_CONNECTION_TERMINATED are unused in this
design. A fresh explicit client_connect reports its own success or failure
through its return value. Authentication denial occurs during that call's
handshake and returns CLIENT_AUTH_FAILED; it does not generate TERMINATED.
An established socket closing does not by itself identify an authentication
failure. Automatic reconnect and in-place reauthentication are not part of
this lifecycle. RESTORED and TERMINATED remain unused enum values; their
presence does not introduce a reconnect or reauthentication requirement.

Keep connection-callback delivery disabled while the handle is unpublished.
Serialize successful handle publication with enabling loss-callback delivery;
retain a loss detected after startup success until it can be delivered with the
published handle. Failed startup never exposes an incomplete Client through a
callback. A genuine loss immediately after readiness can race the calling
thread's resumption; it is a distinct loss event, not a startup-success callback.
[Section 5.13](../../../doc/EyilClient.md#513-client_disconnect) defines callback claims and quiescence during explicit close.

### Contract: CC-04 Socket configuration and disabled heartbeats

Source: [CL-020](../../../doc/EyilClient.md#socket-settings-for-a-fresh-connection), [CL-021](../../../doc/EyilClient.md#disabled-zeromq-heartbeats).

Set these options on each new client DEALER before zmq_connect:

| Option | Required setting | Purpose |
| --- | --- | --- |
| ZMQ_RECONNECT_IVL | -1 | Disable libzmq automatic reconnection |
| ZMQ_IMMEDIATE | 1 | Queue sends only on completed connections |
| ZMQ_LINGER | 0 | Discard unsent transport messages when closing the socket |

Also set zero linger on the client monitor socket. Retain the library's
HANDSHAKE_SUCCEEDED admission barrier and nonblocking sends; IMMEDIATE does not
replace readiness validation. Closing the old socket is required to retire its
transport queues. These options do not cancel messages already received by
Gateway or prove remote completion. See the
[ZeroMQ socket options](https://libzmq.readthedocs.io/en/latest/zmq_setsockopt.html).

The 15-second connect deadline still bounds readiness waiting for a new attempt.
It does not create an automatic retry loop. If startup fails, roll back that
attempt; another attempt requires a new explicit client_connect call. A terminal
failure returns its mapped error promptly; an attempt still pending at the
deadline returns CLIENT_TIMEOUT.

**Decision: ZeroMQ transport heartbeats are disabled on both the client's
DEALER and Gateway's ROUTER.** Apply these settings before connect/bind and
whenever either socket is recreated:

| Option | Required setting | Meaning |
| --- | --- | --- |
| ZMQ_HEARTBEAT_IVL | 0 | Do not send periodic ZMTP PING commands |
| ZMQ_HEARTBEAT_TIMEOUT | 0 | No active heartbeat timeout while the heartbeat interval is disabled |
| ZMQ_HEARTBEAT_TTL | 0 | Do not request heartbeat-based expiry at the peer |

Configure the disabled values before zmq_connect()/zmq_bind() and after socket
recreation. These settings apply when establishing the connection. See the
[ZeroMQ socket-option contract](https://libzmq.readthedocs.io/en/latest/zmq_setsockopt.html).

Keep the PAIR monitors and the 15-second connect, synchronous-request and
async-send deadlines. Async responses have no timeout. There is no application
heartbeat RPC, heartbeat worker, idle-expiry timer or inactivity watchdog.

Gateway serves requests and owns connection resources; it does not supervise
application health. An idle, completely hung process may retain its socket and
bounded connection state until actual disconnection or an explicitly agreed
platform lifecycle action. Protect other clients through quotas, fair scheduling
and nonblocking sends; do not add responsiveness probes or heartbeat expiry.

### Contract: CC-05 Initial connection outcomes and rollback

Source: [CL-023](../../../doc/EyilClient.md#exact-initial-connect-outcomes).

| Condition / monitor event | Startup action | client_connect result |
| --- | --- | --- |
| Output argument `Client **client` is NULL | Reject before creating runtime state | CLIENT_INVALID_ARGUMENT |
| `*client` already holds a valid, not-yet-destroyed Client handle, including after transport loss | Reject before allocating or starting another connection; preserve the existing handle, handler and state | CLIENT_ALREADY_CONNECTED, original handle unchanged |
| Client/runtime/serving-thread allocation fails, including an explicit ENOMEM from local setup or startup event processing | Stop/roll back any initialized resources | CLIENT_OUT_OF_MEMORY, output NULL |
| Non-memory context/socket/monitor/thread setup failure, or synchronous zmq_connect error | Publish FAILED and stop the attempt | CLIENT_COMMUNICATION_ERROR, output NULL |
| ZMQ_EVENT_CONNECTED | Keep PENDING; transport connection alone is insufficient | No return yet |
| ZMQ_EVENT_CONNECT_DELAYED | Keep PENDING within the original deadline; do not create another connection attempt | No return yet |
| ZMQ_EVENT_HANDSHAKE_SUCCEEDED while PENDING and before the deadline | Publish READY and wake caller | CLIENT_OK, valid output handle |
| ZMQ_EVENT_HANDSHAKE_FAILED_AUTH with ZAP status 400 | Publish FAILED and wake caller immediately | CLIENT_AUTH_FAILED, output NULL |
| ZMQ_EVENT_HANDSHAKE_FAILED_AUTH with ZAP status 300 or 500 | End this connect attempt as an authentication-service failure | CLIENT_COMMUNICATION_ERROR, output NULL |
| ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL | Publish FAILED and wake caller immediately | CLIENT_COMMUNICATION_ERROR, output NULL |
| ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL | Publish FAILED and wake caller immediately | CLIENT_COMMUNICATION_ERROR, output NULL |
| ZMQ_EVENT_DISCONNECTED before initial readiness | End this connect attempt and wake caller | CLIENT_COMMUNICATION_ERROR, output NULL |
| Monitor/receive processing terminates unexpectedly for a non-memory reason while startup is PENDING | Publish FAILED and wake caller | CLIENT_COMMUNICATION_ERROR, output NULL |
| Absolute connection deadline expires while startup is PENDING, including an unavailable endpoint without a terminal failure event | Publish TIMED_OUT; stop and clean up the attempt | CLIENT_TIMEOUT, output NULL |

The monitor's HANDSHAKE_FAILED_AUTH event carries the ZAP status: 400 is an
authentication denial, 300 a temporary authentication-service error and 500 an
internal authentication-service error. This client contract ends the current
connect call on those explicit failures, preserving CLIENT_AUTH_FAILED for a
denial. Event and status definitions:
[ZeroMQ monitor API](https://libzmq.readthedocs.io/en/latest/zmq_socket_monitor.html),
[ZAP specification](https://rfc.zeromq.org/spec/27/).

ZAP status is first observed on the server. In particular, status 300 can close
or stall the handshake without exposing `HANDSHAKE_FAILED_AUTH(300)` on the
client monitor. Map the event actually received: a disconnect or terminal
failure returns CLIENT_COMMUNICATION_ERROR; expiry with no terminal event
returns CLIENT_TIMEOUT. Never infer CLIENT_AUTH_FAILED from either condition.
The table's status-300 mapping applies only if that event reaches the client.

`zmq_connect()` returning zero and receiving ZMQ_EVENT_CONNECTED are both
insufficient for CLIENT_OK. Successful handshake detection is the agreed
readiness barrier. The client sends no extra Crypto request or AppManager
probe; Gateway performs CONNECTED during authentication as in [section 6.5](../../../doc/EyilGateway.md#65-zap-interim-identity-and-future-appmanager-lookup).

The startup coordinator stores a terminal result, rather than only a
`connected` boolean. The wait predicate is `startupState != PENDING` with an
absolute monotonic deadline. Success and explicit failures publish the result
and notify the waiter; the timeout path publishes CLIENT_TIMEOUT. Synchronize
these transitions so one attempt has one final outcome. A late handshake event
cannot revive a timed-out or failed attempt or restart its deadline. After a
previously successful client_connect, connection loss is terminal and recovery
uses a new explicit connection as defined in [section 5.14](../../../doc/EyilClient.md#514-deadlines-callbacks-reconnect-and-library-unloading).

On failure of a new connection attempt from an empty slot, keep the output
handle NULL, close created sockets on their owner, stop/join started threads,
terminate the owned context and free the incomplete handle before returning.
CLIENT_ALREADY_CONNECTED is an earlier rejection: preserve the existing handle
and do not run startup rollback against it. The deadline bounds the readiness
wait; safe rollback still has to finish. Return every startup outcome through
client_connect alone, without a connection callback. Handshake success does
not emit RESTORED, and authentication denial does not emit TERMINATED. Only a
separate unexpected loss after startup success can enter the loss-callback path
defined above.

### Contract: CC-06 Explicit disconnect barrier and raw-handle coordination

Source: [CL-036](../../../doc/EyilClient.md#513-client_disconnect), [CL-037](../../../doc/EyilClient.md#results-and-callback-behavior-during-explicit-close), [CL-038](../../../doc/EyilClient.md#concurrent-callers-and-handle-lifetime).

client_disconnect is a synchronous local cleanup barrier. The app may call it
while calls wait for the slot, occupy it or await responses; it need not wait for those
responses first. The function closes new admission, wakes slot waiters, cancels the unsent submission,
retires pending response IDs and closes the connection. It returns only after
the old runtime is quiescent and the handle is destroyed. It does not wait for
a Gateway acknowledgement or certify that remote operations have finished.

| State when local close wins | Outcome |
| --- | --- |
| Synchronous request still waiting | Return CLIENT_NOT_CONNECTED with NULL response and size 0 for bytes, or NULL for strings; prevent further sends and resolve any admitted send attempt before returning |
| Async submission not accepted and no send attempt in progress | Return CLIENT_NOT_CONNECTED through its send waiter; cancel the unsent action and invoke no response callback |
| Nonblocking send attempt in progress | Latch close and wait for its outcome; acceptance preserves async CLIENT_OK, while a pending sync response result becomes CLIENT_NOT_CONNECTED; nonacceptance cannot trigger another attempt |
| Async send already accepted | Preserve CLIENT_OK, remove its pending response ID and release library-owned storage without a response callback |
| Uncommitted response or unclaimed callback | Discard its storage and pending record; do not start delivery after the close cutoff |
| Result or callback already claimed | Preserve the winning result/ownership, let the call or callback finish and wait for its live reference to be released |
| Already LOST handle | Finish teardown without changing the loss result or closing an already closed socket again |

Serialize local-close admission with request publication and callback claims.
Use the same retained completion state for close, response, timeout and transport
failure: only one terminal result wins. If a nonblocking send is already in
progress, let the owner resolve it before completing its send waiter. Successful
libzmq acceptance remains CLIENT_OK for an async submission even if local close
was requested while that attempt ran. A synchronous call still waiting for its
response returns CLIENT_NOT_CONNECTED when close wins, even if its send was
accepted. That local error does not prove remote cancellation. Unsent actions
cancelled by close can never execute later.

An intentional close generates no connection callback. Suppress monitor events
caused by shutting down the socket. A connection callback already claimed for an
earlier unexpected loss may finish; no new callback is claimed after callback
admission closes.
Response and connection callbacks run without internal locks, and no callback
can execute for that Client after client_disconnect returns.

Disconnect must remain possible after allocation failure. Set a stop flag
created during connect, notify slotAvailable and signal the existing wakeup.
Shutdown must not acquire an empty slot or publish a control request. Resolve
result waiters using retained
completion objects and erase/free records without constructing a temporary
container. The function is void; it cannot report CLIENT_OUT_OF_MEMORY and
must not depend on allocating an error object to clean up.

Wake and resolve pending callers before waiting for their live-call references
to drain. Never wait while holding the state/slot or callback locks they need
to return. Joining the I/O owner also waits for its active callback. Callbacks
must return promptly; safe cleanup cannot forcefully terminate application code
running on that thread. Preserve results already committed to a caller,
including a fully prepared string owned by a completed waiter. Response-text
preparation precedes the final completion claim, as specified in [section 5.8](../../../doc/EyilClient.md#58-client_send_request_string--gateway-returns-unsupported-service).
Free only uncommitted library-owned response storage.

The same function destroys a handle whose connection already reached LOST.
Reuse completed cleanup safely: close each socket once, join started threads
and terminate the old owned ZMQ context before releasing the handle. Free only
library-owned undelivered response storage; results already transferred to the
caller remain available for client_free_response. Explicit reconnect starts
from a NULL handle slot with no old runtime state, as specified in [section 5.14](../../../doc/EyilClient.md#514-deadlines-callbacks-reconnect-and-library-unloading).

- Call disconnect from an application lifecycle thread, never from a Client
  response/connection callback or the I/O thread it must join. A callback can
  schedule cleanup on an application thread and return.
- The application serializes connect/disconnect and destruction of the same
  handle. Stop new call entry and coordinate threads that could still enter
  using a copied raw pointer. A call not yet holding an internal live reference
  must not race handle destruction; neither the internal state mutex nor a raw
  Client pointer makes that race safe.
- Calls already holding live references may remain blocked in the library while
  disconnect runs. Close wakes their waiters. An entered call that has not yet
  submitted work checks admission before publication and returns the retained
  terminal reason instead of publishing into the slot: CLIENT_NOT_CONNECTED for
  explicit close from READY, or the earlier loss reason for an already LOST
  runtime. Every return path
  releases its guard, including validation and allocation failures.
- Serialize callback admission with CLOSING. Claims made before the cutoff
  retain the Client until return; later responses/events are discarded. Keep
  the registered handler and its application dependencies valid until the
  disconnect barrier completes, then clear the registration.
- Once disconnect returns, do not use the old handle again, including a second
  disconnect. Clear the application's slot and use a new Client for reconnect.
  Already-transferred response allocations remain app-owned and may still be
  released through client_free_response.

The library never detaches a live thread and destroys its state. Explicit local
close cleans transport and request bookkeeping; Gateway's accepted jobs and
shared-buffer leases follow their existing drain and completion rules.

### Contract: CC-07 Terminal loss and explicit fresh connection

Source: [CL-041](../../../doc/EyilClient.md#terminal-connection-loss-and-explicit-reconnect).

**Decision:** loss of an established connection ends that connection generation.
The I/O owner closes admission and transitions READY to LOST once. It stops
submissions, retires pending work and closes the old transport; it does not
reconnect the socket, restore the handle to READY or replay an operation. A new
request on the still-allocated lost handle returns CLIENT_NOT_CONNECTED.

| State at detected transport loss | Required cleanup/result |
| --- | --- |
| Callers waiting for the slot | Save the terminal failure reason and notify slotAvailable; entered calls return that reason without publication |
| Published/claimed unsent request | Cancel, resolve any active send attempt, clear the slot and release its reference; never send cancelled work later |
| Synchronous result or async send-result waiter still pending | Complete once with CLIENT_COMMUNICATION_ERROR; clear response outputs and invoke no async response callback |
| Async send already accepted by libzmq | Preserve its CLIENT_OK submission result, retire its response record without a callback and release library-owned request state |
| Unread, received or prepared response not committed to a caller | Discard it and release its library-owned storage and accounting |
| Result already committed to a waiter or callback | Preserve the winning completion and ownership transfer; do not overwrite it or free the caller's response |
| Old libzmq queues | Disable monitoring and close owner-held sockets with LINGER=0; discard unread input, release received message objects and do not reuse the sockets |

Use the retained completion objects, stop flags and wakeups so cleanup remains
possible after allocation failure. A response, timeout or send acceptance that
already won its terminal transition keeps its result. For connection-wide
local allocation/validation failures, use the existing error mappings from
[section 5.6](../../../doc/EyilClient.md#56-client_send_request) with the same teardown discipline. No synthetic async error response
is introduced.

The terminal runtime reason is latched once. Explicit cleanup after loss must
not change CLIENT_COMMUNICATION_ERROR or CLIENT_OUT_OF_MEMORY to
CLIENT_NOT_CONNECTED for previously admitted callers still preparing input or
waiting for the slot. New calls after READY ended are rejected before admission
with CLIENT_NOT_CONNECTED; they do not inherit an older operation's error.

Report CLIENT_CONNECTION_LOST once for the established connection, without held
internal locks, if its callback is claimed before explicit close shuts callback
admission. Local close does not create a new loss event. The I/O owner closes
both DEALER and its monitor PAIR. The old serving loop exits and the lost handle remains valid
only for rejected requests and orderly destruction. client_disconnect joins the
started threads and terminates the old owned ZMQ context; none of that runtime
is reused. A callback already executing must return before destruction completes.
The application schedules disconnect/reconnect from an application thread, not
from the connection callback.

Neither CLIENT_CONNECTION_RESTORED nor CLIENT_CONNECTION_TERMINATED is emitted.
The new explicit connection reports its startup outcome only through the
client_connect return value, just like the first connection. It cannot restore
the old handle or session.

The application first coordinates its callers and the old handle's lifetime.
A successful new connect has a new Client runtime, owned ZMQ context, DEALER,
monitor endpoint and connection generation. Gateway authenticates a new
User-Id/session and performs CONNECTED again. Matching UID/PID or future
authoritative app identity does not restore an old connection. The submission
slot and pending-response map start empty; old buffer IDs and session key handles
cannot be used in the new session.

Client cleanup is not a remote cancellation acknowledgement. Gateway separately
closes the old context's admission and drops its undeliverable responses. Accepted
Gateway jobs follow the existing drain/deadline contract; active backend access
must finish before DISCONNECTED and buffer destruction. A new session can be
admitted while the old one retains private cleanup references, without sharing
its resources or accepting its late completions. A missing response does not
make automatic operation replay safe.

### Contract: CC-08 Prepared process freeze and library unload

Source: [CL-042](../../../doc/EyilClient.md#freeze-and-runtime-behavior), [CL-044](../../../doc/EyilClient.md#library-unloading), [GW-044](../../../doc/EyilGateway.md#intended-process-freeze-behavior-and-prerequisites).

The application owns preparation for freezing its process: stop new submissions,
finish accepted operations and callbacks, and leave no uncertain remote work.
Async CLIENT_OK confirms only send acceptance, so it does not satisfy this
preparation requirement; the operation may still be executing remotely.
With no pending operation, no request timer is active and the 15-second duration
does not expire the idle connection. No custom heartbeat or inactivity watchdog
is part of that flow.

The intended freeze preserves the process and its descriptors. Transport
heartbeats are disabled on both ends, so a lack of heartbeat traffic during the
pause does not expire that connection. Actual socket closure or Gateway failure
still uses normal disconnect handling. [Section 6.14](../../../doc/EyilGateway.md#614-disconnect-detection-pid-observations-and-hibernation) describes the Gateway side
and the distinction from process destruction.

A shared library can create threads. Runtime unloading is safe only after the
application disconnects every Client, all library-owned threads are joined,
all calls/callbacks have returned, owned ZMQ contexts are terminated and all
buffers requiring `client_free_response()` have been freed. No thread may still
execute library code after `dlclose()`. libzmq's internal threads do not remove
this obligation. See the
[dynamic loader API](https://man7.org/linux/man-pages/man3/dlopen.3.html).

Before allowing the freeze, the application must:

1. Stop new submissions from all its callers and finish any connect attempt.
2. Wait for all accepted requests and response callbacks to finish. There must
   be no unsent request, outstanding response, active callback or active backend
   operation belonging to the connection.
3. Coordinate its shared-buffer users so no Crypto operation is still reading
   or writing a region. Idle allocations and key/session state may remain owned
   by the live connection; freezing does not imply release.
4. Treat a timeout or communication error as an uncertain remote outcome, not
   proof that the operation ended. Without definitive completion, the app has
   not established the required idle state. The generic API has no remote-drain
   or cancel acknowledgement that could prove otherwise.

No new public prepare-to-freeze API or AppManager subscription is introduced by
these assumptions. The application coordinates its own activity with its
platform's freeze mechanism. This is a responsibility of the app, not a claim
that the current library can atomically certify remote quiescence.

## Related contracts and open decisions

[CR-03 Single-slot ownership and publication](../client-requests/spec.md#requirement-cr-03-single-slot-ownership-and-publication), [CR-05 Response callback execution](../client-requests/spec.md#requirement-cr-05-response-callback-execution), [CR-11 Deadlines and dynamic response state](../client-requests/spec.md#requirement-cr-11-deadlines-and-dynamic-response-state).

