<!--
If not stated otherwise in this file or this component's LICENSE
file the following copyright and licenses apply:

Copyright 2026 RDK Management

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# EyilClient implementation guide

This guide complements the [canonical capability specs](SpecificationMap.md#capability-index).
Read their full detailed contracts with the diagrams, rationale and procedures here.
Original ACs and open decisions remain in [EyilSecureGateway.md](EyilSecureGateway.md).
Section numbers and source anchors are preserved for navigation; the
[source map](SpecificationMap.md) covers every original section.

- [5.1 Responsibilities and limits](#51-responsibilities-and-limits)
- [5.2 Component organization and ownership](#52-component-organization-and-ownership)
- [5.3 General routing: concurrent requests on one connection](#53-general-routing-concurrent-requests-on-one-connection)
- [5.4 Public API index](#54-public-api-index)
- [5.5 client_connect()](#55-client_connect)
- [5.6 client_send_request()](#56-client_send_request)
- [5.7 client_send_request_async()](#57-client_send_request_async)
- [5.8 client_send_request_string() — Gateway returns unsupported service](#58-client_send_request_string--gateway-returns-unsupported-service)
- [5.9 client_send_request_string_async() — Gateway returns unsupported service](#59-client_send_request_string_async--gateway-returns-unsupported-service)
- [5.10 client_free_response()](#510-client_free_response)
- [5.11 client_register_notifications()](#511-client_register_notifications)
- [5.12 client_unregister_notifications()](#512-client_unregister_notifications)
- [5.13 client_disconnect()](#513-client_disconnect)
- [5.14 Deadlines, callbacks, reconnect and library unloading](#514-deadlines-callbacks-reconnect-and-library-unloading)
- [5.15 Call hierarchy](#515-call-hierarchy)

## 5. EyilClient design and API flows

### 5.1 Responsibilities and limits

Contract requirements: [CR-01 Public methods and transport scope](../openspec/specs/client-requests/spec.md#requirement-cr-01-public-methods-and-transport-scope),
[CC-01 Connection ownership and application lifetime](../openspec/specs/client-connection/spec.md#requirement-cc-01-connection-ownership-and-application-lifetime).

### 5.2 Component organization and ownership

Contract requirements: [CR-03 Single-slot ownership and publication](../openspec/specs/client-requests/spec.md#requirement-cr-03-single-slot-ownership-and-publication),
[CC-01 Connection ownership and application lifetime](../openspec/specs/client-connection/spec.md#requirement-cc-01-connection-ownership-and-application-lifetime).

```mermaid
flowchart TB
    A["Application / Firebolt Crypto<br/>Application threads"]
    subgraph LIB["EyilClient / libeyilclient.so"]
        subgraph API["Public C API"]
            LIFE["client_connect / client_disconnect"]
            REQ["client_send_request / client_send_request_async<br/>client_send_request_string / client_send_request_string_async"]
            OTHER["client_free_response<br/>client_register_notifications / client_unregister_notifications"]
        end
        RT["ClientRuntime<br/>Admission, generation, live calls and stop flag"]
        SLOT["One submission slot per Client<br/>Mutex + slotAvailable condition<br/>EMPTY / PUBLISHED / CLAIMED"]
        WAKE["Pollable wakeup<br/>Submission, cancellation and stop"]
        IO["One I/O owner<br/>DEALER + monitor PAIR + wakeup"]
        P["Pending map by request_id<br/>Many sent requests can await responses"]
        RESULT["Request completion state<br/>Send result / synchronous response waiter"]
        CB["Async response or connection-loss handler<br/>Invoked by I/O owner without locks"]
        MON["PAIR monitor receiver<br/>Existing I/O owner polls and decodes events<br/>Agreed monitor implementation"]
        LIFE --> RT
        LIFE -- "Create empty slot, start / stop / join" --> IO
        REQ -- "F1 Retain call, validate and prepare" --> RT
        RT -- "F2 WaitAndPublish, wait releases mutex" --> SLOT
        SLOT -- "F3 Wake owner after publication" --> WAKE
        RT -- "Cancellation / stop, independent of occupied slot" --> WAKE
        WAKE --> IO
        IO -- "F3 ClaimSubmission under mutex" --> SLOT
        IO -- "F4 Install response target before send" --> P
        IO -- "F5 CompleteSend, clear matching slot and notify slotAvailable" --> SLOT
        IO -- "F6 Publish send result or final sync error" --> RESULT
        P -- "F7/F8 Correlate and complete sync response" --> RESULT
        P -- "F8 Claim and deliver async response" --> CB
        IO -- "Established connection loss" --> CB
        IO -- "Enable monitor, poll and decode" --> MON
    end
    G["Gateway ROUTER<br/>ipc:///tmp/app-eyil-gateway.ipc"]
    A -- "Lifecycle calls" --> LIFE
    A -- "Request calls" --> REQ
    A -- "Free response or reserved notification call" --> OTHER
    OTHER -- "Free locally / register NOT_IMPLEMENTED / unregister no effect" --> A
    RESULT -- "Return on the calling application thread" --> A
    CB -- "Invoke application callback" --> A
    IO -- "F5 zmq_send, one binary RpcRequest frame" --> G
    G -- "F7 zmq_msg_recv, RpcResponse" --> IO
```

F1–F8 are the common flow points defined in section 5.3 and reused in every
request sequence. WaitAndPublish, ClaimSubmission and CompleteSend are internal
design method names on the runtime/owner, not additional public APIs. The slot
is read explicitly by ClaimSubmission; a wakeup alone does not transfer or send
the request. CompleteSend frees the slot after acceptance/failure, independently
of later response delivery. Connection and response callbacks share the owner
thread and must return promptly. Notification APIs create no callback state.

#### Monitor responsibilities

Contract requirements: [CC-01 Connection ownership and application lifetime](../openspec/specs/client-connection/spec.md#requirement-cc-01-connection-ownership-and-application-lifetime).

#### Single-slot publication, claiming and completion

Contract requirements: [CR-03 Single-slot ownership and publication](../openspec/specs/client-requests/spec.md#requirement-cr-03-single-slot-ownership-and-publication).

#### Shared submission diagram (F1–F6)

This diagram is the common submission path for all four request APIs. The API
diagrams in sections 5.6–5.9 draw it as one "F2-F5 Submit" step and show only
their own differences. Early exits use `break`: once a `break` block runs, the
call ends there.

```mermaid
sequenceDiagram
    autonumber
    participant A as Calling application thread
    participant API as Any request C API
    participant S as ClientRuntime / single slot
    participant IO as Client I/O owner
    participant P as Pending map
    participant Z as Client DEALER
    A->>API: Request call
    API->>API: Validate required pointers before accessing the Client
    API->>S: F1 RetainCall, check thread and READY, start absolute deadline
    API->>API: F1 PrepareRequest with owned input and result state
    break Invalid argument, I/O-owner thread, not READY or preparation allocation failure
        API-->>A: CLIENT_INVALID_ARGUMENT, CLIENT_NOT_CONNECTED or<br/>CLIENT_OUT_OF_MEMORY, nothing sent, no callback
    end
    API->>S: F2 WaitAndPublish(request, deadline)
    opt Slot occupied
        S->>S: Wait on slotAvailable, mutex released while waiting
    end
    break Deadline, close or loss wins before publication
        S-->>API: CLIENT_TIMEOUT, CLIENT_NOT_CONNECTED or<br/>CLIENT_COMMUNICATION_ERROR
        API-->>A: Exact local result, nothing sent, no callback
    end
    S->>S: Store retained Request, mark PUBLISHED, unlock
    API->>IO: F3 Signal pollable wakeup
    API->>S: F6 Wait for this request's result, mutex released
    IO->>S: F3 ClaimSubmission: lock, read, retain, mark CLAIMED, unlock
    IO->>IO: F4 Recheck eligibility, assign ID and schema 1, serialize outside lock
    IO->>P: F4 Insert expected response target before the first send
    break Allocation failure, cancellation, deadline, close or loss before sending
        IO->>P: Retire unsent entry if installed
        IO->>S: CompleteSend(request, CLIENT_OUT_OF_MEMORY or winning error),<br/>clear matching slot, notify all waiters
        S-->>API: F6 Committed send failure, no callback
    end
    loop Until accepted or terminal send outcome
        IO->>S: F5 TryBeginSend: recheck cancellation, generation and deadline
        break Request no longer eligible
            IO->>P: Retire unsent entry, it is never sent later
            IO->>S: CompleteSend(request, exact error), clear matching slot, notify
        end
        IO->>Z: F5 zmq_send(one frame, DONTWAIT)
        alt EAGAIN or EINTR
            IO->>S: End SEND_IN_PROGRESS, keep CLAIMED slot
            IO->>IO: Poll replies, monitor events and writable readiness, then retry
        else Memory failure or other terminal send error
            IO->>P: Retire unsent entry
            IO->>S: CompleteSend(request, CLIENT_OUT_OF_MEMORY or<br/>CLIENT_COMMUNICATION_ERROR), clear matching slot, notify
        else Accepted by libzmq
            IO->>S: F5 CompleteSend(request, CLIENT_OK), clear matching slot, notify
            Note over S,P: Slot is reusable, response record stays pending in P
        end
    end
    S-->>API: F6 Send result
    API-->>A: Async: return send result. Sync: continue with the response wait in 5.6
```

### 5.3 General routing: concurrent requests on one connection

Contract requirements: [CR-03 Single-slot ownership and publication](../openspec/specs/client-requests/spec.md#requirement-cr-03-single-slot-ownership-and-publication),
[CR-07 Shared nonblocking send and receive pipeline](../openspec/specs/client-requests/spec.md#requirement-cr-07-shared-nonblocking-send-and-receive-pipeline).

Coverage: **AC2-Client.1, AC2-Client.2–AC2-Client.3, AC2-Client.5,
AC1.5–AC1.7, AC2-Firebolt.1–AC2-Firebolt.2**.

Use these flow points in the API diagrams and implementation:

| Point | Calling component / internal method | Work and next boundary |
| --- | --- | --- |
| F1 | C API: RetainCall / PrepareRequest | Validate handle, calling thread and arguments; check READY, start the deadline, own input and initialize request/result state |
| F2 | Calling thread: WaitAndPublish | Wait for the single slot, releasing the mutex; publish only while eligible; timeout/close/loss can end the wait |
| F3 | Caller: WakeIo; owner: ClaimSubmission | Wake the poller; owner explicitly locks, reads and retains the published request, marks CLAIMED and unlocks |
| F4 | Owner: PrepareWireRequest / TrackRequest | Recheck deadline/generation, assign ID and schema 1, serialize and install the pending entry before sending |
| F5 | Owner: TryBeginSend / CompleteSend | Attempt zmq_send(DONTWAIT); retain the slot on temporary backpressure; atomically record the send result and release only its matching slot, then wake callers |
| F6 | Calling thread: WaitForSend or WaitForResponse | Async returns send result; sync waits for its own final response within the same total deadline, without occupying the slot after send |
| F7 | Owner: ReceiveResponse / MatchRequest | Receive a complete response, validate framing/envelope, match the original ID/service/generation and require recorded send acceptance |
| F8 | Owner: CompleteResponse | Prepare final bytes/status text, claim completion against timeout/close, retire once, wake the sync caller or invoke the async callback without locks |

These internal names make the call boundaries explicit; they can be methods
and state in one ClientRuntime plus Request records. The common send/error
rules are in section 5.6; F1–F8 do not add public API methods. F6 is the caller's
wait/return: it overlaps owner steps F3–F5 and, for sync, F7–F8. Waiting on that
result condition does not prevent the owner from claiming or sending work.

```mermaid
sequenceDiagram
    autonumber
    participant A as App thread A / async
    participant B as App thread B / sync
    participant S as ClientRuntime / single slot
    participant IO as Client I/O owner
    participant P as Pending map / result records
    participant Z as DEALER
    participant G as Gateway / private workers
    A->>A: client_send_request_async, F1 prepare A
    A->>S: F2 WaitAndPublish(A)
    S-->>A: Slot contains A
    A->>IO: F3 WakeIo
    A->>P: F6 WaitForSend(A), no state lock held
    B->>B: client_send_request, F1 prepare B
    B->>S: F2 WaitAndPublish(B)
    Note over B,S: B waits for slotAvailable, mutex is released
    IO->>S: F3 ClaimSubmission, read and retain A, slot stays occupied
    S-->>IO: Request A
    IO->>P: F4 Assign ID A, serialize, insert target A
    IO->>Z: F5 zmq_send(A, DONTWAIT)
    Z-->>IO: Accepted into libzmq
    IO->>S: CompleteSend(A, CLIENT_OK), record result and disarm deadline,<br/>clear matching slot under lock, then notify
    P-->>A: CLIENT_OK, API returns without response wait
    Z->>G: Transport request A
    S->>S: B acquires mutex, publishes B, unlocks
    S-->>B: Published
    B->>IO: F3 WakeIo
    B->>P: F6 WaitForResponse(B) within original deadline
    IO->>S: F3 ClaimSubmission, read and retain B
    S-->>IO: Request B
    IO->>P: F4 Assign ID B, serialize, insert target B
    IO->>Z: F5 zmq_send(B, DONTWAIT)
    Z-->>IO: Accepted
    IO->>S: CompleteSend(B, CLIENT_OK), record acceptance, clear matching<br/>slot and notify, response result stays pending
    Z->>G: Transport B while A can still execute
    Note over S,G: Both IDs are pending, slot is free for another caller
    G-->>Z: Response B before A
    IO->>Z: F7 zmq_msg_recv(DONTWAIT)
    Z-->>IO: RpcResponse B
    IO->>P: F7/F8 Validate, match accepted B, prepare bytes, retire B
    P-->>B: CLIENT_OK and owned response, synchronous API returns
    G-->>Z: Response A, no async response deadline
    IO->>Z: F7 zmq_msg_recv(DONTWAIT)
    Z-->>IO: RpcResponse A
    IO->>P: F7/F8 Validate, match accepted A, prepare bytes, claim callback,<br/>retire A
    IO-->>A: callback A(Client, owned response, size), on I/O thread
```

### 5.4 Public API index

Contract requirements: [CR-01 Public methods and transport scope](../openspec/specs/client-requests/spec.md#requirement-cr-01-public-methods-and-transport-scope).

#### Allocation failures across the public API

Contract requirements: [CR-02 Local result and allocation-failure contract](../openspec/specs/client-requests/spec.md#requirement-cr-02-local-result-and-allocation-failure-contract).

#### Required asynchronous request behavior

Contract requirements: [CR-04 Asynchronous send acceptance and response-only delivery](../openspec/specs/client-requests/spec.md#requirement-cr-04-asynchronous-send-acceptance-and-response-only-delivery).

#### Async signatures and response ownership

Contract requirements: [CR-04 Asynchronous send acceptance and response-only delivery](../openspec/specs/client-requests/spec.md#requirement-cr-04-asynchronous-send-acceptance-and-response-only-delivery).

The asynchronous request API uses these signatures:

```c
typedef void (*ClientResponseHandler)(
    Client *client,
    uint8_t *response_data,
    size_t response_size);

typedef void (*ClientStringResponseHandler)(
    Client *client,
    char *response_data);

ClientResult client_send_request_async(
    Client *client,
    const uint8_t *request_data,
    size_t request_size,
    ClientResponseHandler handler);

ClientResult client_send_request_string_async(
    Client *client,
    const char *request_data,
    ClientStringResponseHandler handler);
```

#### Send-result coordination and cancellation

Contract requirements: [CR-04 Asynchronous send acceptance and response-only delivery](../openspec/specs/client-requests/spec.md#requirement-cr-04-asynchronous-send-acceptance-and-response-only-delivery).

#### Request-callback execution model

Contract requirements: [CR-05 Response callback execution](../openspec/specs/client-requests/spec.md#requirement-cr-05-response-callback-execution).

### 5.5 client_connect()

Coverage: **AC2-Client.1–AC2-Client.4; AC1.2, AC1.4–AC1.6**.

#### Handle ownership and CLIENT_ALREADY_CONNECTED

Contract requirements: [CC-02 Connect handle and readiness](../openspec/specs/client-connection/spec.md#requirement-cc-02-connect-handle-and-readiness).

#### Readiness contract

Contract requirements: [CC-02 Connect handle and readiness](../openspec/specs/client-connection/spec.md#requirement-cc-02-connect-handle-and-readiness).

#### Connection callback contract

Contract requirements: [CC-03 Connection event callback timing](../openspec/specs/client-connection/spec.md#requirement-cc-03-connection-event-callback-timing).

#### Socket settings for a fresh connection

Contract requirements: [CC-04 Socket configuration and disabled heartbeats](../openspec/specs/client-connection/spec.md#requirement-cc-04-socket-configuration-and-disabled-heartbeats).

#### Disabled ZeroMQ heartbeats

Contract requirements: [CC-04 Socket configuration and disabled heartbeats](../openspec/specs/client-connection/spec.md#requirement-cc-04-socket-configuration-and-disabled-heartbeats).

Freezing an application also stops its libzmq threads. Disabling transport
heartbeats avoids disconnecting that preserved idle connection merely because
the frozen client cannot exchange heartbeat traffic. The agreed hibernation
preserves descriptors and starts only after the app finishes its operations.
An independent Thunder/Gateway failure uses terminal-loss cleanup; automatic
recovery of that platform failure is outside scope.

#### Connection sequence

The diagram's reference to the outcome table resolves to
[CC-05 Initial connection outcomes and rollback](../openspec/specs/client-connection/spec.md#requirement-cc-05-initial-connection-outcomes-and-rollback).

Contract requirements: [CC-02 Connect handle and readiness](../openspec/specs/client-connection/spec.md#requirement-cc-02-connect-handle-and-readiness).

```mermaid
sequenceDiagram
    autonumber
    participant A as Application
    participant API as client_connect
    participant C as Startup coordinator / result
    participant IO as Client I/O owner and PAIR receiver
    participant Z as Client libzmq / DEALER
    participant GW as Gateway libzmq / ROUTER
    participant G as Gateway ZAP handler
    Note over A,API: Caller initializes handle to NULL and serializes access to its<br/>slot
    Note over API,IO: Startup success and failure use the return value, no initial<br/>connection callback
    A->>API: client_connect(Client handle slot, connectionHandler)
    API->>API: Validate slot, allocate only if valid and empty
    alt Output pointer is NULL
        API-->>A: CLIENT_INVALID_ARGUMENT
    else Slot already holds a valid Client handle
        API-->>A: CLIENT_ALREADY_CONNECTED, existing handle unchanged
        Note over A,API: No startup, callback or cleanup for the rejected call
    else Initial memory allocation fails
        API->>API: Release partial allocations, keep out Client NULL
        API-->>A: CLIENT_OUT_OF_MEMORY, out Client NULL
    else Local runtime allocated
        API->>C: Retain startup result, empty slot, conditions, stop flag and<br/>wakeup, set PENDING with deadline at attempt start plus 15<br/>seconds
        API->>IO: Attempt to start owned serving thread
        alt Serving-thread allocation fails
            API->>C: Publish FAILED with CLIENT_OUT_OF_MEMORY using retained startup<br/>state
        else Other thread-creation failure
            API->>C: Publish FAILED with CLIENT_COMMUNICATION_ERROR
        else Serving thread started
            API->>C: Wait until startup result is terminal or deadline expires
            Note over API,IO: Caller waits, I/O owner continues running
            IO->>Z: Create context and DEALER
            IO->>Z: Set PLAIN credentials, default HWM, MAXMSGSIZE=-1,<br/>RECONNECT_IVL=-1, IMMEDIATE=1 and LINGER=0
            IO->>Z: Set ZMQ_HEARTBEAT_IVL, TIMEOUT and TTL to 0 before connect
            Note over IO,GW: Transport heartbeats disabled on DEALER and ROUTER
            IO->>Z: Enable DEALER monitoring at the unique inproc endpoint
            IO->>Z: Create PAIR receiver with LINGER=0 and connect to that monitor<br/>endpoint
            opt Local setup succeeded
                IO->>Z: zmq_connect(ipc:///tmp/app-eyil-gateway.ipc)
            end
            alt Local setup or synchronous zmq_connect call fails
                IO->>C: Publish FAILED with CLIENT_OUT_OF_MEMORY or<br/>CLIENT_COMMUNICATION_ERROR
                C->>C: Wake connect waiter immediately, no application callback
            else Setup succeeds and connection attempt starts
                opt Gateway reachable and handshake progresses
                    Z->>GW: UDS connection and PLAIN handshake
                    GW->>G: ZAP request with kernel-derived peer credentials
                    G->>G: Bind connection, attempt scoped ICrypto CONNECTED<br/>Backend failure is logged, transport may still succeed
                    G-->>GW: Status 200 and User-Id after setup, or failure status
                    GW-->>Z: Handshake result
                end
                Note over IO,Z: CONNECTED and CONNECT_DELAYED do not establish readiness, no<br/>automatic reconnect
                alt ZMQ_EVENT_HANDSHAKE_SUCCEEDED before deadline
                    Z-->>IO: Handshake success through monitor PAIR
                    IO->>C: Publish READY with CLIENT_OK and wake connect waiter
                else ZMQ_EVENT_HANDSHAKE_FAILED_AUTH with ZAP status 400
                    Z-->>IO: Explicit authentication rejection
                    IO->>C: Publish FAILED with CLIENT_AUTH_FAILED and wake connect waiter
                else Startup monitor or event-processing allocation fails
                    IO->>C: Publish FAILED with CLIENT_OUT_OF_MEMORY and wake connect waiter
                else Terminal protocol, transport or authentication-service failure
                    Z-->>IO: Failure event, exact mapping in table below
                    IO->>C: Publish FAILED with CLIENT_COMMUNICATION_ERROR and wake connect<br/>waiter
                else Connect deadline expires while still PENDING
                    C->>C: Publish TIMED_OUT with CLIENT_TIMEOUT
                    C->>IO: Wake and abort pending connection attempt
                end
            end
        end
        C-->>API: One terminal ClientResult
        alt Result is CLIENT_OK
            API->>C: Publish initialized Client handle and enable later loss-callback<br/>delivery
            API-->>A: CLIENT_OK, valid Client, no initial callback
        else Result is an error
            opt Serving thread was started
                API->>IO: Set existing stop flag and signal wakeup, owner closes created<br/>sockets
                IO-->>API: Serving loop exits
            end
            API->>API: Join started threads, terminate context, free runtime without<br/>publishing a control request
            API-->>A: Exact ClientResult, out Client NULL, no callback
        end
    end
```

#### Exact initial-connect outcomes

Contract requirements: [CC-05 Initial connection outcomes and rollback](../openspec/specs/client-connection/spec.md#requirement-cc-05-initial-connection-outcomes-and-rollback).

### 5.6 client_send_request()

Contract requirements: [RP-01 Envelope and request identity](../openspec/specs/rpc-protocol/spec.md#requirement-rp-01-envelope-and-request-identity),
[CR-06 Binary request validation and synchronous completion](../openspec/specs/client-requests/spec.md#requirement-cr-06-binary-request-validation-and-synchronous-completion).

Coverage: **AC2-Client.1, AC2-Client.2–AC2-Client.3, AC2-Client.5**; Crypto use also supports **AC2-Firebolt.1–AC2-Firebolt.2**.

#### Submission and synchronous wait

Contract requirements: [CR-06 Binary request validation and synchronous completion](../openspec/specs/client-requests/spec.md#requirement-cr-06-binary-request-validation-and-synchronous-completion).

The shared submission (F2–F5) is drawn in section 5.2 and the shared receive
pipeline (F7–F8) below. This diagram shows only what is specific to the
synchronous byte API.

```mermaid
sequenceDiagram
    autonumber
    participant A as Application
    participant API as client_send_request
    participant S as ClientRuntime / single slot
    participant W as Retained request / result condition
    participant IO as Client I/O owner
    participant G as Gateway I/O owner (ROUTER)
    A->>API: Client, serialized RpcRequest and size, out response and size
    API->>API: Clear every supplied output, validate required pointers
    API->>S: F1 RetainCall, check caller thread and READY, start original deadline
    API->>W: F1 Parse and own RpcRequest, require nonzero service and<br/>safe length representation
    break Invalid argument, I/O-owner thread, not READY or preparation allocation failure
        API->>API: Release partial state and any retained call guard
        API-->>A: CLIENT_INVALID_ARGUMENT, CLIENT_NOT_CONNECTED or<br/>CLIENT_OUT_OF_MEMORY, NULL/0 outputs
    end
    API->>S: F2-F5 Submit through the single slot (section 5.2)
    break Send not accepted
        S-->>API: CLIENT_TIMEOUT, CLIENT_NOT_CONNECTED,<br/>CLIENT_COMMUNICATION_ERROR or CLIENT_OUT_OF_MEMORY
        API->>S: Free unpublished state, release live-call guard
        API-->>A: Same result, NULL/0 outputs
    end
    Note over S,IO: Send accepted, slot is free for other callers during the response wait
    API->>W: F6 WaitForResponse within the original deadline, no runtime mutex held
    alt Valid response claimed before the deadline
        G-->>IO: RpcResponse, shared receive pipeline F7-F8
        IO->>W: CLIENT_OK and owned serialized RpcResponse and size
    else Matched response invalid
        G-->>IO: RpcResponse with wrong service, schema or status
        IO->>W: TryComplete(CLIENT_COMMUNICATION_ERROR)
    else Response allocation fails
        G-->>IO: RpcResponse
        IO->>W: TryComplete(CLIENT_OUT_OF_MEMORY)
    else Deadline, connection loss or explicit close wins first
        IO->>W: TryComplete(CLIENT_TIMEOUT, CLIENT_COMMUNICATION_ERROR<br/>or CLIENT_NOT_CONNECTED)
        Note over IO,G: A later reply for this ID is discarded
    end
    W-->>API: F6 One final result and response outputs
    API->>S: Release live-call guard
    API-->>A: ClientResult and response outputs, NULL/0 on every error
```

#### Shared nonblocking send and receive pipeline

Contract requirements: [CR-07 Shared nonblocking send and receive pipeline](../openspec/specs/client-requests/spec.md#requirement-cr-07-shared-nonblocking-send-and-receive-pipeline).

```mermaid
sequenceDiagram
    autonumber
    participant G as Gateway I/O owner (ROUTER)
    participant Z as Client DEALER
    participant IO as Client I/O owner
    participant S as ClientRuntime / single slot
    participant P as Pending map
    participant W as Sync result or unaccepted send waiter
    participant H as Async response handler
    G-->>Z: RpcResponse
    Z-->>IO: Readable
    IO->>Z: F7 zmq_msg_recv(DONTWAIT), consume the complete message
    IO->>IO: F7 Check framing and parse envelope
    break Receive/decode allocation fails before trustworthy correlation
        IO->>S: Latch CLIENT_OUT_OF_MEMORY, close admission, notify slotAvailable
        IO->>P: Retire affected records, clear occupied slot safely
        IO-->>W: Resolve pending sync and unaccepted send waiters with<br/>CLIENT_OUT_OF_MEMORY
        Note over IO,H: Connection-state path reports LOST, no async error callback
    end
    break Unparseable response, invalid framing or zero request_id
        IO->>S: Latch CLIENT_COMMUNICATION_ERROR, close admission, notify slotAvailable
        IO->>P: Retire affected records, clear occupied slot safely
        IO-->>W: Resolve pending sync and unaccepted send waiters with<br/>CLIENT_COMMUNICATION_ERROR
        Note over IO,H: Connection-state path reports LOST, no async error callback
    end
    IO->>P: F7 Match request_id and generation
    break Unknown or retired nonzero ID
        IO->>IO: Discard, no other request's handler is invoked
    end
    break Entry exists but its send is not yet accepted
        IO->>IO: Log and discard premature response, request and slot unchanged
    end
    IO->>IO: F7 Validate service, schema and RpcStatus
    IO->>IO: F8 Prepare owned bytes, or the status-name string for string APIs
    break Metadata invalid or response preparation fails
        IO->>P: Retire record once, free temporary storage
        IO-->>W: Sync: TryComplete(CLIENT_COMMUNICATION_ERROR or CLIENT_OUT_OF_MEMORY)
        Note over IO,H: Async: diagnostics only, no callback, send result stays CLIENT_OK
    end
    IO->>P: F8 Claim final delivery against close and any sync deadline
    break Timeout or close already won
        IO->>P: Retire record once
        IO->>IO: Free uncommitted response, keep winning result, no callback
    end
    IO->>P: Retire record and accounting once
    alt Synchronous request
        IO-->>W: CLIENT_OK with owned byte or string response
    else Async request
        IO->>IO: Release internal locks
        IO-->>H: handler(Client, owned response[, size]) on the Client I/O owner
    end
```

#### Synchronous timeout, disconnect and response races

Contract requirements: [CR-06 Binary request validation and synchronous completion](../openspec/specs/client-requests/spec.md#requirement-cr-06-binary-request-validation-and-synchronous-completion).

```mermaid
sequenceDiagram
    autonumber
    participant A as Synchronous caller / waiter
    participant R as Runtime lock / retained Request
    participant IO as Client I/O owner / pending map
    participant G as Gateway I/O owner (ROUTER)
    alt Response is validated and wins terminal transition before<br/>deadline
        G-->>IO: RpcResponse
        IO->>R: TryComplete(CLIENT_OK, owned response)
        R-->>A: Wake with successful local delivery
    else Detected response allocation failure wins terminal transition
        IO->>R: TryComplete(CLIENT_OUT_OF_MEMORY, NULL, 0)
        R-->>A: Wake with CLIENT_OUT_OF_MEMORY
        Note over A,G: Local failure does not undo execution or allow replay
    else Request deadline expires first
        A->>R: CancelOrExpireSync under the send-eligibility lock
        alt Request never published
            R->>R: Commit CLIENT_TIMEOUT, caller owns unpublished storage
        else Published or claimed, no send accepted or in progress
            R->>R: Atomically disable future sends and commit CLIENT_TIMEOUT
        else SEND_IN_PROGRESS
            R->>R: Latch expiry, prevent any subsequent attempt
            A->>R: Wait for active attempt outcome without holding the lock
            IO->>R: CompleteSend with actual outcome, commit sync CLIENT_TIMEOUT
            Note over R,G: Accepted send may execute remotely, async acceptance rules do<br/>not turn a sync timeout into success
        else Send already accepted
            R->>R: Commit CLIENT_TIMEOUT for response wait only
        end
        A->>IO: Wake for owner-side retirement
        R-->>A: CLIENT_TIMEOUT, NULL response and size 0
    else Connection is lost first
        IO->>R: TryComplete(CLIENT_COMMUNICATION_ERROR, NULL, 0)
        R-->>A: Wake with CLIENT_COMMUNICATION_ERROR
    else Explicit client_disconnect wins first
        IO->>R: TryComplete(CLIENT_NOT_CONNECTED, NULL, 0)
        R-->>A: Wake with CLIENT_NOT_CONNECTED and release live-call guard on<br/>return
    end
    Note over R,IO: Loss/close also prevent future sends and resolve any active<br/>attempt before completing affected calls
    IO->>IO: Retire this request once, clear only its matching occupied slot
    opt Late or duplicate response arrives
        G-->>IO: RpcResponse for retired request ID
        IO->>IO: Discard, no second completion, ID reuse or change to another<br/>request's slot
    end
```

### 5.7 client_send_request_async()

Contract requirements: [CR-04 Asynchronous send acceptance and response-only delivery](../openspec/specs/client-requests/spec.md#requirement-cr-04-asynchronous-send-acceptance-and-response-only-delivery),
[CR-05 Response callback execution](../openspec/specs/client-requests/spec.md#requirement-cr-05-response-callback-execution),
[CR-11 Deadlines and dynamic response state](../openspec/specs/client-requests/spec.md#requirement-cr-11-deadlines-and-dynamic-response-state),
[CR-08 Asynchronous byte API outcomes](../openspec/specs/client-requests/spec.md#requirement-cr-08-asynchronous-byte-api-outcomes).

Coverage: **AC2-Client.1, AC2-Client.2–AC2-Client.3, AC2-Client.5**.

```mermaid
sequenceDiagram
    autonumber
    participant A as Application
    participant API as client_send_request_async
    participant S as ClientRuntime / single slot
    participant IO as Client I/O owner
    participant G as Gateway I/O owner (ROUTER)
    participant H as Response handler
    A->>API: Client, serialized RpcRequest and size, response handler
    API->>API: Validate required pointers and handler
    API->>S: F1 RetainCall, check caller thread and READY, start original deadline
    API->>API: F1 Parse and own RpcRequest, require nonzero service and<br/>safe length representation
    break Invalid argument or handler, I/O-owner thread, not READY or preparation allocation failure
        API->>API: Release partial state and any retained call guard
        API-->>A: CLIENT_INVALID_ARGUMENT, CLIENT_NOT_CONNECTED or<br/>CLIENT_OUT_OF_MEMORY, no callback
    end
    API->>S: F2-F5 Submit through the single slot (section 5.2)
    S-->>API: F6 Send result
    API->>S: Release live-call guard
    break Send not accepted
        API-->>A: CLIENT_TIMEOUT, CLIENT_NOT_CONNECTED,<br/>CLIENT_COMMUNICATION_ERROR or CLIENT_OUT_OF_MEMORY, no callback
    end
    API-->>A: CLIENT_OK, libzmq accepted the message, no response wait
    Note over S,IO: Slot is free, the pending response has no deadline
    alt Valid response claimed
        G-->>IO: RpcResponse, shared receive pipeline F7-F8
        IO->>IO: Retire record, release internal locks
        IO-->>H: handler(Client, owned serialized RpcResponse, size)<br/>on the Client I/O owner
    else Matched response invalid or its allocation fails
        G-->>IO: RpcResponse
        IO->>IO: Retire record, log diagnostics, no callback, CLIENT_OK stays
    else Connection loss or explicit close
        IO->>IO: Retire record and uncommitted storage, no callback, CLIENT_OK stays
    end
    Note over A,H: A fast response can invoke the handler before the API call returns
```

### 5.8 client_send_request_string() — Gateway returns unsupported service

Contract requirements: [CR-09 Raw-string API ownership and local outcomes](../openspec/specs/client-requests/spec.md#requirement-cr-09-raw-string-api-ownership-and-local-outcomes).

Coverage: **AC2-Client.1, AC2-Client.5**. Actual string-service execution is a
delivery limitation against AC2-Client.5; the original AC remains unchanged.

#### Defined wire request and error response

Contract requirements: [RP-05 Unsupported raw-text wire contract](../openspec/specs/rpc-protocol/spec.md#requirement-rp-05-unsupported-raw-text-wire-contract).

```mermaid
sequenceDiagram
    autonumber
    participant A as Application
    participant API as client_send_request_string
    participant S as ClientRuntime / single slot
    participant W as Retained request / result condition
    participant IO as Client I/O owner
    participant G as Gateway I/O owner (ROUTER)
    A->>API: Client, NUL-terminated text, out response
    API->>API: Clear the supplied output, validate required pointers
    API->>S: F1 RetainCall, check caller thread and READY, start original deadline
    API->>W: F1 Copy text through the first NUL, check size arithmetic,<br/>build envelope with service UNSPECIFIED (0)
    break Invalid argument, I/O-owner thread, not READY or preparation allocation failure
        API->>API: Release partial state and any retained call guard
        API-->>A: CLIENT_INVALID_ARGUMENT, CLIENT_NOT_CONNECTED or<br/>CLIENT_OUT_OF_MEMORY, NULL output
    end
    API->>S: F2-F5 Submit through the single slot (section 5.2)
    break Send not accepted
        S-->>API: CLIENT_TIMEOUT, CLIENT_NOT_CONNECTED,<br/>CLIENT_COMMUNICATION_ERROR or CLIENT_OUT_OF_MEMORY
        API->>S: Free unpublished state, release live-call guard
        API-->>A: Same result, NULL output
    end
    API->>W: F6 WaitForResponse within the original deadline, no runtime mutex held
    alt Gateway reply arrives before the deadline
        G->>G: Validate context, envelope and ID, service 0 has no handler
        G->>G: Log ERROR unsupported_service, no worker or Crypto call
        G-->>IO: RpcResponse: same ID, service 0, schema 1,<br/>UNSUPPORTED_SERVICE, empty payload
        IO->>IO: F7-F8 Shared receive pipeline, expect a known non-OK status<br/>and empty payload
        alt Valid and status-name allocation succeeds
            IO->>W: CLIENT_OK and owned text RPC_STATUS_UNSUPPORTED_SERVICE
        else Unexpected OK, unknown status or nonempty payload
            IO->>W: TryComplete(CLIENT_COMMUNICATION_ERROR)
        else Status-name allocation fails
            IO->>W: TryComplete(CLIENT_OUT_OF_MEMORY)
        end
    else Deadline, connection loss or explicit close wins first
        IO->>W: TryComplete(CLIENT_TIMEOUT, CLIENT_COMMUNICATION_ERROR<br/>or CLIENT_NOT_CONNECTED)
        Note over IO,G: A later reply for this ID is discarded
    end
    W-->>API: F6 One final result
    API->>S: Release live-call guard
    API-->>A: ClientResult and owned status text, NULL on every error
```

#### Plain-string ownership and local failures

Contract requirements: [CR-09 Raw-string API ownership and local outcomes](../openspec/specs/client-requests/spec.md#requirement-cr-09-raw-string-api-ownership-and-local-outcomes).

### 5.9 client_send_request_string_async() — Gateway returns unsupported service

Contract requirements: [CR-09 Raw-string API ownership and local outcomes](../openspec/specs/client-requests/spec.md#requirement-cr-09-raw-string-api-ownership-and-local-outcomes).

Coverage: **AC2-Client.1, AC2-Client.5**, with the same string-service delivery
limitation as section 5.8. The async transport path is required: the method
copies raw text, waits for its single-slot handoff and libzmq acceptance, then
returns the send result. It does not wait for Gateway's unsupported response
and does not return CLIENT_NOT_IMPLEMENTED locally.

```mermaid
sequenceDiagram
    autonumber
    participant A as Application
    participant API as client_send_request_string_async
    participant S as ClientRuntime / single slot
    participant IO as Client I/O owner
    participant G as Gateway I/O owner (ROUTER)
    participant H as String response handler
    A->>API: Client, NUL-terminated text, response handler
    API->>API: Validate required pointers and handler
    API->>S: F1 RetainCall, check caller thread and READY, start original deadline
    API->>API: F1 Copy text through the first NUL, check size arithmetic,<br/>build envelope with service UNSPECIFIED (0)
    break Invalid argument or handler, I/O-owner thread, not READY or preparation allocation failure
        API->>API: Release partial state and any retained call guard
        API-->>A: CLIENT_INVALID_ARGUMENT, CLIENT_NOT_CONNECTED or<br/>CLIENT_OUT_OF_MEMORY, no callback
    end
    API->>S: F2-F5 Submit through the single slot (section 5.2)
    S-->>API: F6 Send result
    API->>S: Release live-call guard
    break Send not accepted
        API-->>A: CLIENT_TIMEOUT, CLIENT_NOT_CONNECTED,<br/>CLIENT_COMMUNICATION_ERROR or CLIENT_OUT_OF_MEMORY, no callback
    end
    API-->>A: CLIENT_OK, libzmq accepted the message, no response wait
    Note over S,IO: Slot is free, the pending response has no deadline
    alt Gateway reply arrives and is valid
        G->>G: Validate context, envelope and ID, service 0 has no handler,<br/>log ERROR unsupported_service, no worker
        G-->>IO: RpcResponse: same ID, service 0, schema 1,<br/>UNSUPPORTED_SERVICE, empty payload
        IO->>IO: F7-F8 Shared receive pipeline, copy status name plus NUL,<br/>retire record, release internal locks
        IO-->>H: handler(Client, owned text RPC_STATUS_UNSUPPORTED_SERVICE)<br/>on the Client I/O owner
    else Unexpected OK, unknown status, nonempty payload or status-name allocation fails
        G-->>IO: RpcResponse
        IO->>IO: Retire record, log diagnostics, no callback, CLIENT_OK stays
    else Connection loss or explicit close
        IO->>IO: Retire record and uncommitted storage, no callback, CLIENT_OK stays
    end
    Note over A,H: A fast response can invoke the handler before the API call returns
```

### 5.10 client_free_response()

Contract requirements: [CR-10 Response freeing and reserved notifications](../openspec/specs/client-requests/spec.md#requirement-cr-10-response-freeing-and-reserved-notifications).

Coverage: **AC2-Client.1**.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_free_response
    participant M as Library-compatible allocator
    A->>API: Response allocation or NULL
    Note over API,M: No new allocation or slot publication, no ClientResult return
    API->>M: free(pointer)
    M-->>API: Storage released, NULL is harmless
    API-->>A: Return
    Note over A,M: Local action only, no ZMQ IPC and no Gateway buffer release
```

### 5.11 client_register_notifications()

Contract requirements: [CR-10 Response freeing and reserved notifications](../openspec/specs/client-requests/spec.md#requirement-cr-10-response-freeing-and-reserved-notifications).

Coverage: **AC2-Client.1**. This API is reserved for future notification support.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_register_notifications
    A->>API: Client and notification handler
    API-->>A: CLIENT_NOT_IMPLEMENTED
    Note over A,API: No allocation, handler retention or transport activity, no<br/>CLIENT_OUT_OF_MEMORY path
```

### 5.12 client_unregister_notifications()

Contract requirements: [CR-10 Response freeing and reserved notifications](../openspec/specs/client-requests/spec.md#requirement-cr-10-response-freeing-and-reserved-notifications).

Coverage: **AC2-Client.1**. This API is reserved for future notification support.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_unregister_notifications
    A->>API: Client
    API-->>A: Return without changing state
    Note over A,API: No allocation, notification state or ZMQ activity, no<br/>ClientResult return
```

### 5.13 client_disconnect()

Contract requirements: [CC-06 Explicit disconnect barrier and raw-handle coordination](../openspec/specs/client-connection/spec.md#requirement-cc-06-explicit-disconnect-barrier-and-raw-handle-coordination).

Coverage: **AC2-Client.1–AC2-Client.4**, leading to **AC1.12** when the Gateway detects closure.

#### Results and callback behavior during explicit close

Contract requirements: [CC-06 Explicit disconnect barrier and raw-handle coordination](../openspec/specs/client-connection/spec.md#requirement-cc-06-explicit-disconnect-barrier-and-raw-handle-coordination).

```mermaid
sequenceDiagram
    autonumber
    participant A as Application lifecycle thread
    participant API as client_disconnect
    participant C as ClientRuntime / single slot
    participant W as In-progress calls, slot and result waiters
    participant IO as Client I/O owner and callbacks
    participant Z as libzmq / DEALER and monitor
    participant G as Gateway monitor
    A->>A: Stop new API entry and coordinate ownership of the handle
    A->>API: Client
    API->>C: Mark CLOSING, close request/callback admission, set existing<br/>stop flag
    Note over API,C: Latch CLIENT_NOT_CONNECTED only for a READY runtime, preserve an<br/>earlier LOST reason and per-call results
    Note over API,IO: Stop does not allocate or wait for the submission slot
    C->>W: notify_all(slotAvailable), unpublished calls use the latched<br/>reason
    C->>IO: Signal existing wakeup
    opt Callback or nonblocking send already claimed before cutoff
        IO->>IO: Finish callback or resolve send result, preserve any accepted<br/>send
    end
    IO->>W: Complete unresolved results with the latched reason, preserve<br/>accepted async CLIENT_OK and earlier completions
    IO->>C: Cancel unsent submission, clear matching slot after any active<br/>send resolves, notify
    IO->>IO: Retire pending IDs and discard uncommitted responses
    Note over C,IO: No synthetic response callback or intentional-close connection<br/>event
    IO->>Z: Disable monitoring, close owned sockets with LINGER=0
    Z-->>G: Peer closure eventually becomes monitor event
    IO-->>C: Serving loop exits
    W->>C: Return mapped or already-committed results, release live-call<br/>guards
    API->>IO: Join the I/O owner, including its monitor processing
    API->>C: Wait for remaining live calls and claimed callbacks without<br/>holding their locks
    API->>Z: Terminate owned context after all its sockets close
    API->>C: Clear handler, verify empty slot, release records and destroy<br/>handle
    API-->>A: Void return, handle is invalid, no ClientResult
    A->>A: Reset caller's handle slot to NULL before reuse
    Note over A,IO: No old Client callback or state access survives the cleanup<br/>barrier
    Note over API,G: Gateway drains accepted work and retains buffers until backend<br/>access ends
```

#### Concurrent callers and handle lifetime

Contract requirements: [CC-06 Explicit disconnect barrier and raw-handle coordination](../openspec/specs/client-connection/spec.md#requirement-cc-06-explicit-disconnect-barrier-and-raw-handle-coordination).

### 5.14 Deadlines, callbacks, reconnect and library unloading

#### Initial timeout and submission defaults

Contract requirements: [CR-11 Deadlines and dynamic response state](../openspec/specs/client-requests/spec.md#requirement-cr-11-deadlines-and-dynamic-response-state),
[GR-02 Current-request admission and worker-start deadline](../openspec/specs/gateway-routing/spec.md#requirement-gr-02-current-request-admission-and-worker-start-deadline),
[CR-10 Response freeing and reserved notifications](../openspec/specs/client-requests/spec.md#requirement-cr-10-response-freeing-and-reserved-notifications).

#### Terminal connection loss and explicit reconnect

Contract requirements: [CC-07 Terminal loss and explicit fresh connection](../openspec/specs/client-connection/spec.md#requirement-cc-07-terminal-loss-and-explicit-fresh-connection).

The explicit API sequence is:

```c
client_disconnect(client);
client = NULL;
ClientResult result = client_connect(&client, connection_handler);
```

```mermaid
sequenceDiagram
    autonumber
    participant A as Application thread
    participant IO as Old Client I/O owner
    participant H as Connection handler on old I/O thread
    participant W as Pending waiters and records
    participant Z as Old DEALER and monitor
    participant API as Client lifecycle API
    participant N as New client runtime
    participant G as Gateway
    Z-->>IO: Established connection disconnects
    IO->>IO: READY to LOST, close admission for old generation
    IO->>W: Wake slot waiters with terminal reason, retire occupied<br/>submission and pending records
    IO->>IO: Free uncommitted response storage, preserve transferred results
    IO->>Z: Disable monitor and close owned sockets with LINGER=0
    IO->>H: Invoke handler with CLIENT_CONNECTION_LOST
    H-->>A: Schedule recovery on an application thread
    H-->>IO: Return promptly
    IO->>IO: Exit serving loop, no automatic reconnect
    A->>API: client_disconnect(old Client), then clear handle slot
    API->>API: Quiesce callers/callbacks, join threads, terminate old context<br/>and free handle
    A->>API: client_connect(empty handle slot, connection handler)
    API->>N: Create fresh runtime, empty submission slot and pending map
    N->>G: New UDS connection and authentication
    G->>G: New connection/session identity, scoped ICrypto CONNECTED
    G-->>N: Successful handshake
    N-->>API: READY for new generation
    API-->>A: CLIENT_OK and new handle, no connection callback
    Note over A,G: No old requests, replies or session resources move to the new<br/>connection
```

#### Freeze and runtime behavior

Contract requirements: [CC-08 Prepared process freeze and library unload](../openspec/specs/client-connection/spec.md#requirement-cc-08-prepared-process-freeze-and-library-unload).

#### Concurrency, callbacks and reconnect

Contract requirements: [CR-03 Single-slot ownership and publication](../openspec/specs/client-requests/spec.md#requirement-cr-03-single-slot-ownership-and-publication),
[CR-04 Asynchronous send acceptance and response-only delivery](../openspec/specs/client-requests/spec.md#requirement-cr-04-asynchronous-send-acceptance-and-response-only-delivery),
[CR-05 Response callback execution](../openspec/specs/client-requests/spec.md#requirement-cr-05-response-callback-execution),
[CR-11 Deadlines and dynamic response state](../openspec/specs/client-requests/spec.md#requirement-cr-11-deadlines-and-dynamic-response-state),
[CC-04 Socket configuration and disabled heartbeats](../openspec/specs/client-connection/spec.md#requirement-cc-04-socket-configuration-and-disabled-heartbeats),
[CC-06 Explicit disconnect barrier and raw-handle coordination](../openspec/specs/client-connection/spec.md#requirement-cc-06-explicit-disconnect-barrier-and-raw-handle-coordination),
[CC-07 Terminal loss and explicit fresh connection](../openspec/specs/client-connection/spec.md#requirement-cc-07-terminal-loss-and-explicit-fresh-connection).

- If arbitrary blocking callbacks must be supported, add a separate bounded
  completion executor with its own shutdown contract. Do not silently create
  an unbounded thread per callback.
- An application-owned event-loop mode would need additional public integration
  APIs. It is an optional future extension, not a requirement for this version.

#### Library unloading

Contract requirements: [CC-08 Prepared process freeze and library unload](../openspec/specs/client-connection/spec.md#requirement-cc-08-prepared-process-freeze-and-library-unload).

### 5.15 Call hierarchy

```text
Application / Firebolt Crypto
  client_connect
    ClientRuntime::Start
      Allocate runtime, EMPTY slot, slotAvailable, result conditions, stop flag, wakeup
      Start I/O owner, create DEALER and set options before connecting
        heartbeat IVL/TIMEOUT/TTL=0, RECONNECT_IVL=-1, IMMEDIATE=1, LINGER=0
      Enable monitoring, connect PAIR receiver, then connect DEALER and await handshake
      HANDSHAKE_SUCCEEDED -> publish handle, CLIENT_OK, no initial callback
      Failure -> retained error, stop/join/close and NULL output

  client_send_request / client_send_request_async
    F1 RetainCall / PrepareRequest
      Check READY, start original deadline, own and validate binary RpcRequest
  client_send_request_string / client_send_request_string_async
    F1 RetainCall / PrepareRequest
      Check READY, start original deadline, scan/copy raw text to first NUL
      Build service=UNSPECIFIED envelope, payload=text bytes, no terminator

  Shared request path for all four methods
    F2 ClientRuntime::WaitAndPublish
      Wait until EMPTY or deadline/close/failure, release mutex during wait
      Publish retained Request, mark PUBLISHED, unlock
    F3 WakeIo -> I/O poller
      ClientRuntime::ClaimSubmission -> lock/read/retain/CLAIMED/unlock
    F4 I/O owner: PrepareWireRequest / TrackRequest
      Recheck eligibility -> assign ID/schema -> serialize -> install pending entry
      Allocation failure -> CLIENT_OUT_OF_MEMORY unless another terminal path won
                            retire unsent entry, free matching slot, notify
    F5 TryBeginSend -> zmq_send(DEALER, DONTWAIT)
      EAGAIN/EINTR -> occupied slot retained, process replies/events, retry if eligible
      Deadline/close/loss -> disable later sends, resolve any active attempt first
      CompleteSend(request, outcome) -> commit send result and free matching slot once
        Accepted async -> CLIENT_OK, even if cancellation raced the admitted attempt
        Sync response result remains separate; expiry/close does not become success
        Unlock -> notify result and slot waiters
    F6 Calling thread
      Async: return send result, no response timeout after acceptance
      Sync: wait for final response within original deadline, slot already free
    F7 I/O owner: zmq_msg_recv -> framing/parse -> match ID/service/schema/status
      Uncorrelatable allocation failure -> terminal CLIENT_OUT_OF_MEMORY, wake slot waiters
      Uncorrelatable invalid response -> terminal CLIENT_COMMUNICATION_ERROR, wake waiters
      Unknown/retired ID -> discard
      Installed but unaccepted ID -> log/discard response, preserve request and slot
    F8 CompleteResponse
      Byte: allocate serialized response bytes, preserve RpcStatus/service payload
      String: validate non-OK status/empty payload -> allocate RpcStatus name + NUL
        Gateway normally returns UNSUPPORTED_SERVICE, service=0, no Crypto invocation
      Claim result against deadline/close -> retire pending entry once
        Never clear another request's occupied submission slot
      Sync: CLIENT_OK + response, or exact local error with NULL/0
      Async: response callback without locks, or diagnostics/retirement without callback

  Monitor processing on I/O owner
    Established loss -> READY to LOST, latch reason once, notify slotAvailable
    Resolve occupied send, fail unresolved results, retire async records
    Close sockets with LINGER=0, emit CLIENT_CONNECTION_LOST if callback claim allowed
    Exit owner, no automatic reconnect or RESTORED/TERMINATED event
    App explicitly disconnects old handle and connects a fresh runtime/session

  client_register_notifications -> CLIENT_NOT_IMPLEMENTED, no retained handler
  client_unregister_notifications -> no effect
  client_free_response -> free, no slot access or allocation

  client_disconnect
    App coordinates handle lifetime and stops new call entry
    Close admission, set existing stop flag, notify slotAvailable, wake I/O owner
    No wait for EMPTY and no control request published into the slot
    Resolve active send, preserve accepted async CLIENT_OK
    Unresolved calls -> latched reason (CLIENT_NOT_CONNECTED for close from READY)
    Preserve earlier LOST reason, clear matching occupied slot and pending IDs
    Owner closes sockets -> join -> quiesce entered calls/claimed callbacks
    Terminate context, destroy runtime, no callback after return
```

The numbered boundaries match sections 5.2–5.9. They belong to one runtime and
its Request records; no additional client coordinator thread or request pool
is implied. Gateway request scheduling remains independent.

