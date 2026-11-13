<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# client-requests specification

## Purpose

Define the nine public C API contracts, concurrent request coordination, local outcomes and response ownership.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC2-Client.1, AC2-Client.5, AC2-Firebolt.1, AC2-Firebolt.2. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: CR-01 Public methods and transport scope

EyilClient SHALL provide the nine declared C entry points with the scope and behavior below.

The detailed [contract](#contract-cr-01-public-methods-and-transport-scope) is normative for this requirement.

#### Scenario: Public methods and transport scope outcome

- **WHEN** an application requests a Crypto operation
- **THEN** it uses the byte API with serialized service payloads; EyilClient handles transport/envelope ownership without invoking Thunder or implicitly mapping shared memory.

### Requirement: CR-02 Local result and allocation-failure contract

C entry points and the I/O owner SHALL return or retire local failures according to the following tables, preserving already won results.

The detailed [contract](#contract-cr-02-local-result-and-allocation-failure-contract) is normative for this requirement.

#### Scenario: Local result and allocation-failure contract outcome

- **WHEN** request preparation or pending-map insertion fails before send acceptance
- **THEN** the API reports CLIENT_OUT_OF_MEMORY with cleared synchronous outputs or no async response callback, retains cleanup ownership and never sends untracked work.

### Requirement: CR-03 Single-slot ownership and publication

Each Client SHALL coordinate callers through one pre-created submission slot and independently retained request records.

The detailed [contract](#contract-cr-03-single-slot-ownership-and-publication) is normative for this requirement.

#### Scenario: Single-slot ownership and publication outcome

- **WHEN** request A has been accepted and awaits its response while request B occupies the slot
- **THEN** retiring A never clears B's slot; the owner services responses and events during B's backpressure, and no submission queue or FIFO guarantee is introduced.

### Requirement: CR-04 Asynchronous send acceptance and response-only delivery

Both asynchronous methods SHALL return their send result after libzmq acceptance or a terminal submission failure, with the response contract below.

The detailed [contract](#contract-cr-04-asynchronous-send-acceptance-and-response-only-delivery) is normative for this requirement.

#### Scenario: Asynchronous send acceptance and response-only delivery outcome

- **WHEN** an accepted async request receives a valid response later than fifteen seconds
- **THEN** the response may invoke its handler once; no response timeout expires it. If close races an already accepted send, the submission remains CLIENT_OK even when no callback can be delivered.

### Requirement: CR-05 Response callback execution

Response callbacks SHALL execute on their Client I/O owner under the lifetime and reentrancy rules below.

The detailed [contract](#contract-cr-05-response-callback-execution) is normative for this requirement.

#### Scenario: Response callback execution outcome

- **WHEN** a response callback calls a request API on its own Client
- **THEN** the request call returns CLIENT_INVALID_ARGUMENT before admission; freeing the received response is allowed and callback invocation holds no internal locks.

### Requirement: CR-06 Binary request validation and synchronous completion

Binary request APIs SHALL validate caller input and retain result storage according to the following rules.

The detailed [contract](#contract-cr-06-binary-request-validation-and-synchronous-completion) is normative for this requirement.

#### Scenario: Binary request validation and synchronous completion outcome

- **WHEN** response storage is being prepared when the synchronous deadline wins
- **THEN** uncommitted storage is freed and the caller receives CLIENT_TIMEOUT with NULL/0; the timeout does not cancel remote work or free a remote buffer.

### Requirement: CR-07 Shared nonblocking send and receive pipeline

The I/O owner SHALL arbitrate every send, receive, result claim and retirement through the shared pipeline below.

The detailed [contract](#contract-cr-07-shared-nonblocking-send-and-receive-pipeline) is normative for this requirement.

#### Scenario: Shared nonblocking send and receive pipeline outcome

- **WHEN** a response for an installed but unaccepted request arrives
- **THEN** the owner logs and discards it without retiring the request, changing its send result or clearing its slot; uncorrelatable decode failure instead follows the specified connection-wide failure path.

### Requirement: CR-08 Asynchronous byte API outcomes

client_send_request_async SHALL preserve the following submission errors and later response behavior.

The detailed [contract](#contract-cr-08-asynchronous-byte-api-outcomes) is normative for this requirement.

#### Scenario: Asynchronous byte API outcomes outcome

- **WHEN** a matched response cannot be allocated after a successful async send
- **THEN** the record is retired with diagnostics and no callback; the accepted CLIENT_OK result is not revised.

### Requirement: CR-09 Raw-string API ownership and local outcomes

String request methods SHALL accept caller text and expose validated Gateway status text using the ownership and error rules below.

The detailed [contract](#contract-cr-09-raw-string-api-ownership-and-local-outcomes) is normative for this requirement.

#### Scenario: Raw-string API ownership and local outcomes outcome

- **WHEN** the caller passes a NUL-terminated string containing abc followed by NUL and more bytes
- **THEN** only abc becomes payload; the synchronous remote rejection returns CLIENT_OK and owned non-NULL status text, while local errors clear the output. The async method waits only for send acceptance.

### Requirement: CR-10 Response freeing and reserved notifications

Response freeing and the two reserved notification APIs SHALL remain local operations with the following ownership and no-allocation behavior.

The detailed [contract](#contract-cr-10-response-freeing-and-reserved-notifications) is normative for this requirement.

#### Scenario: Response freeing and reserved notifications outcome

- **WHEN** the caller frees NULL or calls either notification API
- **THEN** freeing NULL is harmless; register returns CLIENT_NOT_IMPLEMENTED without retaining a handler, and unregister changes nothing or allocates nothing.

### Requirement: CR-11 Deadlines and dynamic response state

Timed calls SHALL use the original monotonic deadlines and submission/response storage policy below.

The detailed [contract](#contract-cr-11-deadlines-and-dynamic-response-state) is normative for this requirement.

#### Scenario: Deadlines and dynamic response state outcome

- **WHEN** a caller spends part of its fifteen-second budget waiting for the occupied slot
- **THEN** preparation, send and synchronous response processing share only the remaining budget; slot publication does not restart the clock, and an accepted async response record has no deadline.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: CR-01 Public methods and transport scope

Source: [CL-004](../../../doc/EyilClient.md#51-responsibilities-and-limits), [CL-010](../../../doc/EyilClient.md#54-public-api-index).

EyilClient provides transport coordination, not Crypto semantics. A caller can
submit concurrent synchronous and asynchronous requests on the same `Client`.
A synchronous caller waits for its response; an async caller waits only for
libzmq send acceptance. Neither may hold a lock needed by the I/O owner or
another caller while waiting.

The library does not:

- Call AppManager, RuntimeManager, Thunder or ICrypto.
- Decide key authorization, generate trusted app identity or choose host paths.
- Allocate, map or release Gateway-owned buffers implicitly through the generic
  transport API. The app-side Crypto layer requests and maps them explicitly.
- Promise that reconnect preserves keys, buffers or a Gateway session.
- Treat a timeout, a successful send or a connection callback as proof that a
  remote Crypto operation finished.

The public API is declared in
[EyilClient.h](../../../apis/eyilclient/EyilClient.h). All nine methods
have individual sequences in the [client guide](../../../doc/EyilClient.md#5-eyilclient-design-and-api-flows). Both asynchronous request methods are required
public functionality under AC2-Client.5, alongside their synchronous counterparts.
The selected string path delivers Gateway's unsupported-service response;
successful text execution is outside this delivery as recorded in [section 5.8](../../../doc/EyilClient.md#58-client_send_request_string--gateway-returns-unsupported-service).

| Method | Behavior | AC coverage |
| --- | --- | --- |
| client_connect | Return a handle after successful handshake; reject an occupied handle slot with CLIENT_ALREADY_CONNECTED; report exact startup/authentication/timeout errors | AC2-Client.1–AC2-Client.4; AC1.2, AC1.4–AC1.6 |
| client_send_request | Wait/publish one submission slot, then await the correlated binary response without holding that slot | AC2-Client.1, AC2-Client.2–AC2-Client.3, AC2-Client.5 |
| client_send_request_async | Wait for the slot and libzmq send acceptance within 15 seconds; deliver a later binary response with no response timeout | AC2-Client.1, AC2-Client.2–AC2-Client.3, AC2-Client.5 |
| client_send_request_string | Send copied raw text with service=UNSPECIFIED through the slot; return CLIENT_OK plus received RPC_STATUS_UNSUPPORTED_SERVICE status text | AC2-Client.1, AC2-Client.5 |
| client_send_request_string_async | Same slot/send contract; Gateway unsupported-service status is delivered as owned text through the response callback | AC2-Client.1, AC2-Client.5 |
| client_free_response | Release a library-allocated result | AC2-Client.1 |
| client_register_notifications | Return CLIENT_NOT_IMPLEMENTED; reserved for future support | AC2-Client.1 and the public header |
| client_unregister_notifications | Return without changing state; reserved for future support | AC2-Client.1 and the public header |
| client_disconnect | Stop admission/I/O, settle pending work and destroy the handle | AC2-Client.1–AC2-Client.4; AC1.12 on the Gateway side |

### Contract: CR-02 Local result and allocation-failure contract

Source: [OV-018](../../../doc/EyilSecureGateway.md#agreed-local-error-behavior), [CL-011](../../../doc/EyilClient.md#allocation-failures-across-the-public-api).

The following client behavior is agreed. It applies independently of AppManager authentication and the outcome of
a Crypto invocation.
Apply ClientResult through the connect, synchronous request or async submission
return value. Async response callbacks carry response data only; they do not
carry local ClientResult errors.

| Situation | ClientResult | Response handling |
| --- | --- | --- |
| Invalid arguments, lengths that the actual API cannot safely represent or invalid caller-supplied binary request envelope | CLIENT_INVALID_ARGUMENT | No response |
| client_register_notifications is called | CLIENT_NOT_IMPLEMENTED | Reserved for future support; no handler is stored or invoked |
| client_connect receives a valid, already allocated Client handle in the caller's slot | CLIENT_ALREADY_CONNECTED | Preserve the existing handle and connection state; create no new connection and invoke no callback |
| Submission without a usable connection | CLIENT_NOT_CONNECTED | No response |
| Explicit client_disconnect wins while a synchronous result or an unaccepted async send is pending | CLIENT_NOT_CONNECTED | Clear synchronous outputs, cancel unsent work and retire pending IDs; no async response callback |
| Authentication explicitly rejected | CLIENT_AUTH_FAILED | No response |
| Detected allocation failure during connect, sync processing or async submission before libzmq acceptance, including request preparation and pending-map insertion | CLIENT_OUT_OF_MEMORY | Roll back local state; sync outputs remain NULL/0 and async submission invokes no response callback |
| Connect, synchronous request or async send-submission deadline expires | CLIENT_TIMEOUT | No handle on connect failure; no synchronous response on timeout; async send failure invokes no response callback. A synchronous timeout may leave remote execution in progress |
| Connection lost during a synchronous request or before async send acceptance, malformed synchronous response or terminal send failure | CLIENT_COMMUNICATION_ERROR | No response |
| Valid correlated synchronous RPC response delivered, or async message accepted by zmq_send | CLIENT_OK | Sync returns the response; async returns without waiting for one. The caller checks RpcStatus and service status when a response is delivered |

These mappings apply when that terminal path wins. An admitted nonblocking send
must finish before its acceptance is known: timeout or close during the attempt
cannot turn an accepted async send into a failed submission. A synchronous
call can still time out or close while awaiting its response. [Section 5.2](../../../doc/EyilClient.md#52-component-organization-and-ownership)
defines result arbitration and preserves the first terminal runtime reason.

For synchronous byte responses, every local error produces `response_data == NULL` and
`response_size == 0`. For string responses, it produces `response_data == NULL`.
A valid Gateway error response, including RPC_STATUS_INTERNAL, is still a
received RPC response: byte APIs return CLIENT_OK with its bytes and preserve
RpcStatus.
Do not turn a local failure into a fabricated Gateway RpcResponse or CryptoError.

Catch detectable allocation failures at the C API boundary and inside the
I/O owner, including `std::bad_alloc`, failed allocator returns and explicit
ENOMEM from local setup/transport. No C++ allocation exception may escape a
C entry point or terminate the serving loop. Use retained result state to
publish an error; failure reporting and cleanup must not require another heap
allocation. Diagnostics are best effort and must not prevent retirement.

| API | Allocation failure handling |
| --- | --- |
| client_connect | CLIENT_OUT_OF_MEMORY for handle/runtime/setup allocation failure; roll back only this attempt, join any started threads, leave the output NULL |
| client_send_request | CLIENT_OUT_OF_MEMORY for input/envelope/request/waiter/pending-map/send storage or correlated response allocation failure; NULL output and size 0 |
| client_send_request_async | Before send acceptance, return CLIENT_OUT_OF_MEMORY with no response callback; after acceptance, retire a matched response that cannot be allocated with diagnostics and no callback |
| client_send_request_string | Same synchronous rules, including copying input text, constructing the wire request and allocating the response string; return CLIENT_OUT_OF_MEMORY and NULL, releasing temporary storage |
| client_send_request_string_async | Same async rules, including input copying and wire-request preparation before acceptance, and response-string allocation afterward |
| client_free_response | Void, releases storage without allocating; NULL is accepted; no ClientResult is returned |
| client_register_notifications | Always CLIENT_NOT_IMPLEMENTED in this version; no state is allocated or handler retained |
| client_unregister_notifications | Void, no effect or allocation in this version |
| client_disconnect | Void, uses an existing stop flag and wakeup to initiate cleanup without waiting for a free slot or allocating a control request; frees retained state, no ClientResult is returned |

For all four request methods, pre-send failure cancels the unsent request and
releases partial state. A call that times out before publication cannot leave
work for the owner. A published request remains retained until owner retirement;
cancellation prevents it being sent after the API reports failure. A failure
after sending does not undo remote execution or
make a shared buffer safe to reuse. Never replay the operation to recover a
local allocation failure.

The common receive pipeline in [section 5.6](../../../doc/EyilClient.md#56-client_send_request) also defines failure before a
response can be correlated. An accepted async send retains CLIENT_OK even if
response processing fails before the submitting thread resumes; its send result
cannot be revised or turned into an error callback.

### Contract: CR-03 Single-slot ownership and publication

Source: [CL-005](../../../doc/EyilClient.md#52-component-organization-and-ownership), [CL-007](../../../doc/EyilClient.md#single-slot-publication-claiming-and-completion).

Use one ClientRuntime per Client, with one I/O owner and one retained request
record per entered request call. The rows below describe responsibilities and
method boundaries, not a requirement for a class or thread per row.

| Component / state | Responsibility | Owner / synchronization |
| --- | --- | --- |
| Public C API | Retain a live call, validate arguments, prepare owned input and wait for submission/result | Calling application thread; never touches DEALER |
| ClientRuntime | Connection generation, admission, slot, callback claims and shutdown | Per-Client state mutex and retained live-call references |
| Single submission slot | One retained Request reference, EMPTY/PUBLISHED/CLAIMED state and slot-available condition variable | Pre-created during connect; no std::queue, list of submissions or queue-node allocation |
| Request record | Owned envelope/text, mode, ID, generation, original deadline, send result and response waiter or handler | Retained independently by the caller, occupied slot, I/O owner and pending map as needed |
| I/O owner | Claim the slot, serialize/send, receive/correlate, process deadlines and PAIR events | One library-owned thread; sole DEALER owner |
| Pending map | request_id to expected response metadata and completion target | I/O owner; entry installed before first send and retired once |
| Per-call result conditions | Wake the async caller on send outcome or sync caller on final result | Waits release the state mutex; send acceptance and response completion remain separate states in the Request |
| Monitor collector | Decode PAIR events and drive connection-state handling | Existing I/O owner; agreed consolidation, no additional client monitor thread |
| Response storage | Owned binary response or NUL-terminated status text | Transferred to the application and freed with client_free_response |

The slot is shared only within one Client. Callers waiting for it remain on
their application threads, retaining their own request/call state; the library
does not put them in another submission container. The slot remains occupied
while its request is being prepared or waiting for libzmq acceptance. After a
terminal send outcome, the owner clears it and wakes waiting callers. Sent
requests remain independently tracked in the pending map, with no fixed count
cap. One occupied slot does not mean one outstanding remote operation.

Every request entry point obtains a live-call guard before input preparation.
The guard covers slot waiting, publication, send/result waiting and response
preparation. Disconnect wakes these calls before waiting for their guards to
drain. The application must still coordinate raw-handle lifetime as specified
in [section 5.13](../../../doc/EyilClient.md#513-client_disconnect). Socket ownership is never transferred to an application caller.

Create the slot, state mutex, slotAvailable condition and pollable wakeup during
connect. No submission queue exists. The API prepares an owned Request before
publication, including its callback/waiter, generation and original deadline.
Preparation can fail with CLIENT_OUT_OF_MEMORY; publishing a retained reference
into an empty slot must not allocate.

WaitAndPublish locks the per-Client state, checks admission and waits until the
slot is EMPTY or a terminal condition occurs. A condition-variable wait releases
the mutex. Use one absolute 15-second deadline starting when the request call
is admitted, before input preparation and slot waiting; no stage restarts it.
After publication, unlock and signal the existing pollable wakeup. A condition
variable alone cannot wake the I/O owner's socket poll.

The owner has no fixed idle-poll interval. Poll DEALER, monitor PAIR and the
existing eventfd indefinitely when no deadline applies, otherwise use the time
remaining until the earliest connect, synchronous-request or unaccepted-send
deadline. Accepted asynchronous response records do not contribute a deadline.
Publication, cancellation and stop signal eventfd; replies and writable
readiness wake the socket poll directly. Retire expired records before computing
the next wait, so an already expired deadline cannot cause a busy loop.

On that wakeup, ClaimSubmission locks state, reads the published Request,
retains it, marks the slot CLAIMED and unlocks. Reading the slot does not clear
it. The owner prepares serialization and the pending-map entry outside the
state lock, then attempts a nonblocking send. There is only one submitted,
not-yet-accepted request at a time. EAGAIN leaves that request in the slot while
the owner polls replies, PAIR events, writable readiness and active deadlines.
It must not block in zmq_send, spin, or wait on an application condition variable.

CompleteSend(request, outcome) applies to the retained Request that owns the
attempt. Under the state lock, it ends SEND_IN_PROGRESS, records the send
outcome once, applies any pending cancellation, disarms an accepted async send's
deadline and clears the slot only if that slot still belongs to this Request.
Unlock before notifying slotAvailable and the request's result condition.
These updates form one coordinated transition. The owner removes
an unsent pending entry before servicing another submission or response.

After acceptance, the pending map retains the response target but not the slot.
A synchronous caller can keep waiting for its response while another caller
publishes and sends a new request. Response retirement for request A must never
clear a slot now occupied by request B. Repeated retirement/completion is
harmless and cannot change an already committed result or release another
request's storage. No async response deadline starts.

The owner allocates increasing nonzero request IDs within its connection
generation. It never reuses a retired ID, including after timeout or failed
submission, so a late response cannot match another request. Do not wrap the
counter: exhaustion ends the generation through the terminal
CLIENT_COMMUNICATION_ERROR path. A fresh Client may restart its ID sequence;
its transport and Gateway context are independent.

| State / event | Required action |
| --- | --- |
| Another request occupies the slot | Wait on slotAvailable within the caller's original deadline; no queue-full result and no FIFO guarantee among competing callers |
| Deadline expires before publication | Return CLIENT_TIMEOUT; no request was transferred or sent |
| Allocation fails during input/request preparation | Return CLIENT_OUT_OF_MEMORY; release partial state, no publication or callback |
| Serialization or pending-map insertion fails after claiming | Owner retires unsent work, frees only its matching slot and completes with CLIENT_OUT_OF_MEMORY unless a terminal result/cancellation already won |
| Published/claimed request is cancelled before a send attempt | Mark cancelled, wake the owner and prevent any later send; retain the record until owner retirement and slot release |
| Nonblocking send attempt is in progress | Resolve that attempt before returning a failure claiming no acceptance; successful acceptance fixes the async result at CLIENT_OK |
| Explicit disconnect closes READY admission | Latch CLIENT_NOT_CONNECTED, notify slotAvailable immediately and wake the owner; preserve earlier results and resolve any send in progress before completing affected calls |
| Unexpected connection loss | Notify slotAvailable, retire unsent/sent state and complete unresolved calls with CLIENT_COMMUNICATION_ERROR; accepted async results remain CLIENT_OK |

EMPTY/PUBLISHED/CLAIMED describe the slot. SEND_IN_PROGRESS describes an
attempt on its retained Request while the slot remains CLAIMED. Only one such
attempt can execute on the owner. The state lock protects the final eligibility
check and transition into SEND_IN_PROGRESS against cancellation/close. Release it for the nonblocking
socket call, then publish the actual outcome under the same synchronization.
A waiter may request cancellation during an attempt but cannot declare that an
accepted message was unsent. After EAGAIN/EINTR, end SEND_IN_PROGRESS under the
lock and recheck cancellation, generation and the original deadline. If no
longer eligible, complete with the winning error; otherwise keep the CLAIMED
slot, poll other work and admit another attempt only through TryBeginSend.
Only the I/O owner clears an occupied slot; an expired caller cannot destroy
storage still retained by it.

Latch the first terminal runtime reason when READY admission closes. Callers
still preparing input or waiting for the slot are not in the pending map; they
read this retained reason before publication or completion. Explicit close
selects CLIENT_NOT_CONNECTED, transport loss CLIENT_COMMUNICATION_ERROR, and
connection-wide decode allocation failure CLIENT_OUT_OF_MEMORY under [section
5.6](../../../doc/EyilClient.md#56-client_send_request). A later disconnect of an already LOST handle must not replace its reason.
Already committed per-call results remain unchanged. New calls on an already
lost/closing handle get CLIENT_NOT_CONNECTED. notify_all requires no heap
allocation.
Local stop uses its existing flag and wakeup, never waits for a free slot and
never publishes a stop request into that slot.

Every result arrow in the [client UMLs](../../../doc/EyilClient.md#5-eyilclient-design-and-api-flows) uses this terminal-result arbitration.
An error discovered during preparation cannot overwrite a timeout or close
already committed while that preparation ran. A send-acceptance arrow records
the actual attempt outcome; for sync it is distinct from the final response
result. Only an accepted async send fixes the public result at CLIENT_OK.

### Contract: CR-04 Asynchronous send acceptance and response-only delivery

Source: [CL-012](../../../doc/EyilClient.md#required-asynchronous-request-behavior), [CL-013](../../../doc/EyilClient.md#async-signatures-and-response-ownership), [CL-014](../../../doc/EyilClient.md#send-result-coordination-and-cancellation).

Implement client_send_request_async() and client_send_request_string_async()
using the same connection, request correlation and coordinator as the
synchronous methods. The submitting thread waits for its send result, **not for
the remote response**.

- Validate and copy the request, initialize its retained send-result state,
  then WaitAndPublish into the single slot. Waiting releases the state mutex.
- Wake the I/O owner, which explicitly claims the slot and installs the pending
  record before attempting send. Return CLIENT_OK only after zmq_send accepts
  that request. Slot publication alone is insufficient.
- EAGAIN retains the occupied slot within the original 15-second budget while
  the owner services other replies and events. Acceptance or terminal failure
  clears the slot and wakes waiting callers.
- Return validation, allocation, connection or send-submission timeout errors
  directly from the API. A failed submission invokes no response callback.
- After send acceptance, retain the request for a response with **no response
  timeout**. A valid response invokes its registered callback at most once.
- Connection loss or explicit shutdown retires outstanding async records without
  a response callback. Transport loss is reported by the connection-event handler.
  Local failure to validate or allocate a matched response also retires that
  record with diagnostics and no response callback.
- A successful send therefore does not guarantee a later callback. Never
  fabricate a Gateway response or deliver NULL as a synthetic error response.

**Selected send guarantee:** zmq_send success means acceptance into libzmq's
queue. It does not prove physical UDS transmission, Gateway receipt or backend
execution, even with a blocking send. No receipt ACK or Protobuf extension is
required by this contract. See the
[ZeroMQ send contract](https://libzmq.readthedocs.io/en/latest/zmq_send.html).

The string method accepts the same NUL-terminated text as its synchronous
counterpart, without caller encoding. Both asynchronous request methods are
required. Byte-method callers continue to supply and interpret serialized
Protobuf; the plain-text contract applies to the string methods.

The pending map associates the library-assigned request_id with the handler
supplied for that request. Callback arguments carry the actual response only.
For byte responses, the app inspects RpcStatus and service status. String
responses carry the symbolic name of the received Gateway error, normally
RPC_STATUS_UNSUPPORTED_SERVICE, under the defined contract in [section 5.8](../../../doc/EyilClient.md#58-client_send_request_string--gateway-returns-unsupported-service).
The caller releases either result through
client_free_response(). A valid Gateway error response follows the same response
delivery path. ClientNotificationHandler is reserved for future support and is
not used to deliver request results or local errors.

Copy input before returning so the caller may reuse its request storage.
Keep pending bookkeeping and callback code valid until response delivery or
connection teardown. Without a response deadline, a request that never receives
a reply can retain its pending-map entry and memory until teardown. Local
validation/allocation failure retires the affected entry but cannot report a
per-request error through these response-only signatures.

Each async request has two distinct stages: a bounded submission wait and an
unbounded response wait. Keep separate send-result and response-completion
states in the same retained Request, rather than requiring separate allocated
coordinator objects. CompleteSend commits the send outcome and matching-slot
release together under the state lock, then notifies the request and slot
waiters. The submitting thread returns that result; response waiting never
occupies the slot.

Serialize the nonblocking send attempt and its acceptance publication against
send-deadline cancellation. Check the request's state and deadline before every
attempt. With no attempt in progress, cancellation atomically makes the request
ineligible for any later send. During SEND_IN_PROGRESS, latch the cancellation
reason and resolve that attempt before publishing its terminal send result.
If libzmq accepts it, the async result is CLIENT_OK even if the deadline or
close occurred during that attempt. If it does not accept it, apply the latched
reason and do not retry. Without cancellation, EAGAIN/EINTR leaves the request
eligible only within its original deadline; a terminal send error completes it.

After requesting cancellation, a waiter awaiting the active attempt uses its
result condition; it must not spin on an already expired deadline. The deadline
prevents a new send attempt, but resolving an admitted attempt and safely
retiring its state can finish later. This is not a hard real-time return bound.
[Section 5.6](../../../doc/EyilClient.md#56-client_send_request) applies the same send eligibility rules to synchronous calls while
keeping their final response result separate from send acceptance.

After acceptance, disarm the async send deadline. Response arrival and
connection cleanup compete only for ownership of the pending record; retire it
once and discard late/duplicate replies. No async response-expiry task runs.
A local retirement or successful send does not prove backend completion or make
a shared buffer safe to reuse.

### Contract: CR-05 Response callback execution

Source: [CL-015](../../../doc/EyilClient.md#request-callback-execution-model).

- Invoke response callbacks on the owning Client's serving/I/O thread, without
  internal state/slot locks held. Callbacks are serialized within a Client;
  different Clients can invoke callbacks concurrently.
- Retire the pending record before calling application code, and keep the Client
  and response storage alive for the invocation.
- Never call the handler inline on the submission call stack. A fast response
  can invoke it on the I/O owner before the submitting thread resumes from its
  send-result wait; prepare application state before making the API call.
- A response callback may call client_free_response. Schedule new requests and
  other work on an application thread.
- All request APIs, including async submission, can wait for the I/O owner.
  Calling them from that owner's callback would deadlock. Detect this case and
  return CLIENT_INVALID_ARGUMENT before admission. Do not call client_connect
  or client_disconnect from the callback either.
- Callbacks must return promptly so other sends, replies, monitor events and
  active deadlines can progress.

These rules do not depend on AppManager or the Crypto backend. Explicit
disconnect closes callback admission, resolves unresolved sync/send-result
waiters with CLIENT_NOT_CONNECTED, retires async response records and waits for
already-claimed callbacks and live calls before destroying the handle. [Section
5.13](../../../doc/EyilClient.md#513-client_disconnect) defines the complete ordering. [Section 5.5](../../../doc/EyilClient.md#55-client_connect) defines the connection-handler
contract: startup uses the synchronous result, later unexpected loss emits
CLIENT_CONNECTION_LOST and explicit close emits no connection callback.

### Contract: CR-06 Binary request validation and synchronous completion

Source: [CL-025](../../../doc/EyilClient.md#submission-and-synchronous-wait), [CL-027](../../../doc/EyilClient.md#synchronous-timeout-disconnect-and-response-races).

The single slot is released at F5, so the synchronous response wait does not
serialize other requests behind the remote operation. Every local-error return
releases the call guard and prepared storage, with NULL/0 outputs. Races between
the response, the deadline and close are drawn in the [synchronous-race sequence](../../../doc/EyilClient.md#synchronous-timeout-disconnect-and-response-races).

Before dereferencing outputs, validate their addresses; clear every supplied
output even if another required argument is missing. Require a non-NULL live
Client, non-NULL input with nonzero representable length, and both output pointers.
An empty/default envelope or RPC_SERVICE_UNSPECIFIED is invalid. Other numeric
service values can reach Gateway, which determines support. Validate Protobuf
parsing and safe length conversions without imposing a message-size policy cap
or interpreting the service payload.
Caller-supplied request_id and schema_version are ignored and overwritten.

The request record owns its parsed data, completion state and connection
generation. Application input need remain valid only for the API call. Allocate
the owned request and completion bookkeeping before acceptance. For async
calls the same preparation completes before returning CLIENT_OK. Neither API
may leave a published pointer into temporary caller storage.

The waiter and owner share a retained Request, not a pointer to the caller's
stack. TryComplete claims its final result and response ownership once against
all other terminal paths. Before send acceptance, a synchronous timeout must
also cancel send eligibility under the same state lock used by TryBeginSend;
changing only the response waiter is insufficient. During an admitted attempt,
latch expiry and wait for its actual outcome. An accepted send is recorded as
such, but a synchronous call whose response deadline expired still returns
CLIENT_TIMEOUT. It may have executed remotely. A nonaccepted attempt after
expiry is retired without another send.

After acceptance, timeout ends only the local response wait. Check the absolute
deadline when claiming the final response, including after byte/string storage
preparation. If another result wins during preparation, free the uncommitted
storage. The owner alone removes pending entries and clears a slot only when
it still contains this Request; an older response timeout cannot affect a newer
submission. Published storage survives until owner retirement, even if its
caller has returned. Unpublished storage remains the caller's responsibility.
Async submission uses the separate send-result rules in [section 5.4](../../../doc/EyilClient.md#54-public-api-index) and has
no response deadline after acceptance.

On every local failure, return NULL response and size zero. A timeout ends the
local wait; it does not cancel remote work, release remote buffers or authorize
replay. A valid response containing a non-OK RpcStatus still returns CLIENT_OK;
the caller inspects that status and eventually calls client_free_response.

The 15-second synchronous deadline includes preparation and waiting for the
single slot. The pending-response map grows dynamically; detected allocation
failures use CLIENT_OUT_OF_MEMORY. A busy slot waits or times out, with no
local queue-full result. [Section 5.14](../../../doc/EyilClient.md#514-deadlines-callbacks-reconnect-and-library-unloading) defines terminal loss and explicit reconnect.
[Section 5.13](../../../doc/EyilClient.md#513-client_disconnect) defines explicit-close results and callback quiescence. The
[resource policy in section 6.2](../../../doc/EyilGateway.md#initial-resource-policy) applies
no additional message-size cap and retains transport defaults; safe
representation and arithmetic checks remain mandatory.

### Contract: CR-07 Shared nonblocking send and receive pipeline

Source: [CL-026](../../../doc/EyilClient.md#shared-nonblocking-send-and-receive-pipeline).

This pipeline serves all four request APIs. Socket operations, pending-map
mutation and monitor processing belong to the I/O owner. A record identifies
whether its caller waits for a synchronous result or only for async send
acceptance. Sync requests retain their total deadline after send; async requests
disarm their submission deadline once libzmq accepts the message.

The send half (F3–F5) is the shared submission diagram in [section 5.2](../../../doc/EyilClient.md#52-component-organization-and-ownership). After
EAGAIN/EINTR, resume at TryBeginSend on the same tracked Request; do not assign
another ID, reinsert the pending entry or claim another submission. The [guide diagram](../../../doc/EyilClient.md#shared-nonblocking-send-and-receive-pipeline) shows the shared receive half (F7–F8). Receive processing can run on any
owner iteration, including while the slot waits for writable readiness.

CompleteSend updates the retained Request's send result and releases only its
matching slot in the single transition defined in [section 5.2](../../../doc/EyilClient.md#52-component-organization-and-ownership). Arrows to result
waiters in that sequence notify/read that committed state; they do not perform another
unconditional result assignment. All synchronous error completions use
TryComplete and preserve an earlier terminal result. In particular, retirement
of an accepted response record never touches another request's submission slot.

Explicit local close uses CLIENT_NOT_CONNECTED for still-pending API results;
unexpected transport loss uses CLIENT_COMMUNICATION_ERROR. Preserve any result
that already won completion. No callback or send attempt can be newly admitted
after the local-close cutoff; [section 5.13](../../../doc/EyilClient.md#513-client_disconnect) covers an attempt already in progress.

Implementation rules:

- Install the response target before sending, so even an immediate reply can
  be correlated. Serialization, message-storage and pending-map allocation
  failures retire only the affected unsent request, publish
  CLIENT_OUT_OF_MEMORY unless another terminal path already won, and leave
  other requests progressing. Clear only that Request's matching slot and notify
  waiting callers on its terminal send result. Use one DEALER body frame without
  a REQ-style delimiter.
- A successful zmq_send is the async return boundary: libzmq queued the message.
  Do not claim physical delivery or submit it again. EAGAIN permits retry only
  for work that has not been accepted, within its original active deadline.
  EINTR permits retry while the request remains eligible. Other terminal send
  failures return CLIENT_COMMUNICATION_ERROR; explicit memory failures return
  CLIENT_OUT_OF_MEMORY. See
  [zmq_send](https://libzmq.readthedocs.io/en/latest/zmq_send.html).
- Receive with zmq_msg_recv and check ZMQ_RCVMORE. EAGAIN means nothing to read;
  EINTR permits retry. Drain malformed multipart input without treating its
  remaining frames as separate responses. See
  [zmq_msg_recv](https://libzmq.readthedocs.io/en/latest/zmq_msg_recv.html).
- A detected receive/decode allocation failure before trustworthy request-ID
  correlation cannot safely be attributed to one request. Mark the connection
  unusable and close it on its owner. Resolve still-pending synchronous results
  and unaccepted async send waiters with CLIENT_OUT_OF_MEMORY, including callers
  waiting for the slot through the retained runtime failure reason; retire accepted
  async records and report CLIENT_CONNECTION_LOST through the connection-state
  path. Do not use a partly parsed ID to fail an unrelated request. Complete
  cleanup with retained state, without allocating another control action.
- Uncorrelatable malformed input makes the connection unusable under the
  terminal-loss policy. Resolve pending synchronous and not-yet-accepted send
  waiters with CLIENT_COMMUNICATION_ERROR. Retire accepted async response
  records and report connection loss through the connection-state path.
- A parsed nonzero ID absent from the pending map is discarded. It may be a
  late synchronous reply or a duplicate; it is not a notification.
- A pending entry alone does not authorize response delivery: its send must
  already be recorded as accepted. Log and discard a premature response for an
  installed but unaccepted request without retiring it, changing its send result
  or clearing its slot. The sole owner records acceptance before processing
  another received message, so a valid fast response satisfies this check.
- A matched response must have the expected service, schema version 1 and a
  known nonzero RpcStatus. A valid Gateway error response is still delivered
  normally: byte sync returns CLIENT_OK with bytes and byte async invokes its
  response handler. String records expect service=0 and a non-OK status with
  empty payload; return its symbolic name as defined in [section 5.8](../../../doc/EyilClient.md#58-client_send_request_string--gateway-returns-unsupported-service). Only the
  service-specific byte consumer interprets CryptoResponse.
- Matched metadata failure returns CLIENT_COMMUNICATION_ERROR to a synchronous
  waiter; matched response allocation failure returns CLIENT_OUT_OF_MEMORY.
  Claim either error through TryComplete; preserve a timeout/close that already
  won and free any uncommitted storage.
  For an async record whose send succeeded, retire it with diagnostics and no
  response callback. Its send result remains CLIENT_OK, even if its submitting
  thread has not resumed yet. These signatures have no post-send error channel.
- Returned response storage must be compatible with client_free_response.
  String methods copy the validated non-OK RpcStatus name into owned
  NUL-terminated storage; they do not return the binary envelope as a C string.
  If response-text preparation fails, apply the same sync-error/async-retirement
  distinction. Never truncate binary data at an embedded NUL to report success.
- Prepare the final byte or string result before claiming synchronous success
  or an async callback. Serialize that claim with explicit close and any active
  synchronous deadline. If another terminal path wins during preparation, free
  the uncommitted result; a received envelope alone does not commit public success.
- Retire request accounting once and free temporary Protobuf/ZMQ storage on
  every terminal path. Late replies cannot revive a retired request.

The connection-wide handling of an uncorrelatable malformed response or decode
allocation failure follows the terminal cleanup path in [section 5.14](../../../doc/EyilClient.md#514-deadlines-callbacks-reconnect-and-library-unloading), preserving
the error mappings above. Recovery requires explicit close and a fresh connect.
An allocation failure after trustworthy correlation affects only that request.
No async response deadline or synthetic local-error RpcResponse is introduced.
Failure before correlation wakes all entered slot waiters as well as pending
requests. The runtime latches its first terminal reason before notifying
slotAvailable; later explicit teardown cannot overwrite that reason.

### Contract: CR-08 Asynchronous byte API outcomes

Source: [CL-028](../../../doc/EyilClient.md#57-client_send_request_async).

There is no response timeout after acceptance. All pre-acceptance failures
return through ClientResult and invoke no response callback. Invalid arguments,
envelope, handler or request calls from the I/O-owner callback return
CLIENT_INVALID_ARGUMENT. A non-ready handle returns CLIENT_NOT_CONNECTED.
Preparation, serialization or pending-map allocation failure returns
CLIENT_OUT_OF_MEMORY. Slot/send deadline expiry returns CLIENT_TIMEOUT.
Explicit close returns CLIENT_NOT_CONNECTED; unexpected loss or terminal send
failure returns CLIENT_COMMUNICATION_ERROR. Connection-wide local failures
retain the mappings in the common pipeline.

After CLIENT_OK, a validated correlated response, including a Gateway error,
invokes ClientResponseHandler at most once. Connection cleanup or local response
processing failure can retire it without a callback. The pending map remains
dynamic; an unanswered async request can stay there until teardown. Callbacks
run on the I/O owner without locks and may free their response, but must schedule
new request calls on an application thread.

### Contract: CR-09 Raw-string API ownership and local outcomes

Source: [CL-029](../../../doc/EyilClient.md#58-client_send_request_string--gateway-returns-unsupported-service), [CL-031](../../../doc/EyilClient.md#plain-string-ownership-and-local-failures), [CL-032](../../../doc/EyilClient.md#59-client_send_request_string_async--gateway-returns-unsupported-service).

Accept an ordinary NUL-terminated request string, without a size argument,
Base64, ProtoJSON or a caller-built envelope. The library copies bytes up to
the first NUL and uses the same slot, owner, pending map and total synchronous
deadline as the byte API. **Gateway rejects the string request with the existing
RPC_STATUS_UNSUPPORTED_SERVICE (4).** The public method does not return a local
CLIENT_NOT_IMPLEMENTED without contacting Gateway.

The caller supplies a valid NUL-terminated string and keeps it unchanged until
the API returns. Scan to the first NUL without an additional text-size cap, own
the bytes before publication and use checked lengths during Protobuf
serialization and ZMQ transmission. The first NUL ends input: "abc\0def"
supplies "abc" only. Empty input still reaches the
same unsupported-service response. Never measure a serialized envelope using
strlen(), decode caller text as Protobuf, or infer a Crypto operation from it.

For a valid remote error, allocate the symbolic status name plus its final NUL
before claiming completion. The application checks that text and calls
client_free_response. CLIENT_OK means the remote error response was delivered;
it does not mean the text operation succeeded. A successful string result is
never NULL and is never binary Protobuf disguised as a C string.

Invalid arguments or lengths that the actual API cannot safely represent return
CLIENT_INVALID_ARGUMENT; there is no additional size-policy threshold. Allocation
failure in copying, request creation, serialization, pending-map insertion or
final status-string preparation returns CLIENT_OUT_OF_MEMORY. Slot waiting,
send attempts and final response preparation share the original 15-second
deadline. Timeout, explicit close and unexpected loss return CLIENT_TIMEOUT,
CLIENT_NOT_CONNECTED and CLIENT_COMMUNICATION_ERROR respectively, with NULL
output. Free all partial storage and release the live-call guard on every exit.
The general race/uncorrelatable-response rules in [section 5.6](../../../doc/EyilClient.md#56-client_send_request) also apply.

This keeps the existing C signatures and raw-string input boundary. Consumers
use the byte APIs with serialized CryptoRequest payloads for actual Crypto
operations. Neither client nor Gateway translates arbitrary text into ICrypto.

The wire envelope and exact status-text representation are defined in [section
5.8](../../../doc/EyilClient.md#58-client_send_request_string--gateway-returns-unsupported-service). A delivered RPC_STATUS_UNSUPPORTED_SERVICE response invokes
ClientStringResponseHandler(Client, owned_status_text); the caller frees it
through client_free_response. Gateway errors are real responses and therefore
use the existing response-only callback, with no new ClientResult callback
argument or user_data parameter.

Input validation, copying, slot waiting and send failures follow [section 5.7](../../../doc/EyilClient.md#57-client_send_request_async).
A failed submission has no response callback. After acceptance there is no
response timeout: loss, explicit close, invalid response or inability to allocate
the status text can retire the record without a callback. Preserve the already
published CLIENT_OK send result. A fast response can invoke the handler on the
I/O owner before the submitting thread resumes. No internal lock is held during
the callback, and request/disconnect calls must be scheduled on an application
thread.

### Contract: CR-10 Response freeing and reserved notifications

Source: [CL-033](../../../doc/EyilClient.md#510-client_free_response), [CL-034](../../../doc/EyilClient.md#511-client_register_notifications), [CL-035](../../../doc/EyilClient.md#512-client_unregister_notifications).

This releases a message-result allocation, not a shared-memory object.
Do not pass a mapped buffer, stack memory or a Gateway buffer ID. Freeing a
response requires no request record or new allocation and remains available
after another API reports CLIENT_OUT_OF_MEMORY.

**Return CLIENT_NOT_IMPLEMENTED.** The function ignores its arguments and does
not store or invoke the handler, allocate notification state, subscribe to events
or send a message to Gateway.

Notification delivery is outside this version's scope. A future feature must
define the events, Protobuf payloads, wire identification and callback lifecycle
before enabling this API. It is not a pending decision for the current delivery.

Connection-state events go to ClientConnectionEventHandler registered through
client_connect(). Async responses go to the response handler supplied with each
request. A late or unmatched RPC response follows the validation rules in
[section 5.6](../../../doc/EyilClient.md#56-client_send_request) and must not be reclassified as a notification.

This void function returns without changing state. Notification registration
is unsupported, so there is no handler to remove or notification delivery to drain.

Calling this function is not required before client_disconnect in this version.
It does not affect the connection-event handler or pending request responses.
Neither notification function allocates, so neither has an out-of-memory path.

As shown in the original free-response sequence, client_free_response SHALL accept NULL, return void and perform no ZMQ operation. A delivered library response is freed with this function, including after its Client has been disconnected; it is not a Gateway shared-buffer release.

### Contract: CR-11 Deadlines and dynamic response state

Source: [CL-040](../../../doc/EyilClient.md#initial-timeout-and-submission-defaults).

| Setting | Initial value | Meaning |
| --- | --- | --- |
| Common timed-wait duration | 15,000 ms | Used for connect, sync requests and async send submission; no public timeout setter |
| Connect deadline | Attempt start plus 15 seconds | Includes startup/handshake waiting; rollback must finish safely |
| Synchronous request deadline | Request-call admission plus 15 seconds | One budget covering input preparation, slot availability, serialization, send, receive/validation and final result preparation |
| Async send deadline | Request-call admission plus 15 seconds | Covers preparation, waiting/publishing the slot and send attempts until libzmq acceptance |
| Async response deadline | None | Send acceptance disarms its send timer; pending response remains until delivery or retirement/teardown |
| Client submission storage | One pre-created slot | At most one published/claimed unsent request; other callers wait on slotAvailable, with no queue container or queue-full result |
| Pending-response map | Dynamic, no fixed request-count cap | Install before first send; sent requests awaiting replies do not occupy the slot |

Do not restart the deadline after acquiring the slot or after sending. Use a
monotonic clock. A call waiting for the slot gets CLIENT_TIMEOUT when its
original budget expires. Callers wait without holding the state mutex; the I/O
owner can process the occupied slot, earlier responses and monitor events.
None of these timers expires an idle connection.

Cancellation before publication leaves nothing for the owner. Cancellation of
a published request marks its retained state and wakes the owner; that request
cannot be sent later. If a nonblocking send is already in progress, resolve
that attempt before reporting failure claiming no acceptance. Acceptance keeps
the async CLIENT_OK result even if a deadline/close raced it. The slot remains
occupied until its owner finishes that send/cancellation outcome, then becomes
available independently of response waiting.

Slot publication moves a retained reference and does not allocate. Input,
request state, serialization, pending-map growth and response preparation still
can fail with CLIENT_OUT_OF_MEMORY under [sections 5.2](../../../doc/EyilClient.md#52-component-organization-and-ownership) and 5.4. Unpublished callers
retain their own prepared requests while blocked, so removal of the submission
container is not a total client memory bound. The pending map can retain
unanswered async requests indefinitely because there is no response timeout.
No FIFO guarantee is made among callers competing for the slot.

There is no additional message/text-size cap. Use ZMQ_MAXMSGSIZE=-1 and default
libzmq HWM (1000 send/receive messages per peer in 4.3.4), with safe length
conversion/arithmetic and normal allocation-failure handling. EAGAIN retains
the occupied client slot while the I/O owner continues polling.

## Related contracts and open decisions

[RP-01 Envelope and request identity](../rpc-protocol/spec.md#requirement-rp-01-envelope-and-request-identity), [RP-02 Status layers and correlated Gateway failures](../rpc-protocol/spec.md#requirement-rp-02-status-layers-and-correlated-gateway-failures), [RP-05 Unsupported raw-text wire contract](../rpc-protocol/spec.md#requirement-rp-05-unsupported-raw-text-wire-contract), [CC-03 Connection event callback timing](../client-connection/spec.md#requirement-cc-03-connection-event-callback-timing), [CC-06 Explicit disconnect barrier and raw-handle coordination](../client-connection/spec.md#requirement-cc-06-explicit-disconnect-barrier-and-raw-handle-coordination), [CC-07 Terminal loss and explicit fresh connection](../client-connection/spec.md#requirement-cc-07-terminal-loss-and-explicit-fresh-connection), [GR-03 Resource and transport policy](../gateway-routing/spec.md#requirement-gr-03-resource-and-transport-policy).

- [FUTURE-TEXT-SERVICE](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
