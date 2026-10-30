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

# Eyil Secure Gateway architecture and implementation guide

## 1. Introduction

This document describes how to implement the remaining EyilClient and
EyilGateway logic: transport, concurrent request coordination, connection
contexts, request execution and gateway-owned shared memory. It also records
the integration boundaries and unanswered questions that must not be replaced
by assumptions in code.

Eyil Secure Gateway is an open-source RDK component intended for the
`rdkcentral` ecosystem. The supported platform deployment uses the
**AppManager and RuntimeManager plugins**. Crypto is its first service; the
Gateway remains a general service router. The plugin is `EyilGateway`, its
callsign is `org.rdk.EyilGateway`, and its independent C client library is
`EyilClient`, installed as `libeyilclient.so`.

> **Current implementation:** this repository contains a buildable skeleton.
> It has no active ZMQ connections, request workers, shared-memory allocation
> or backend calls. **AppManager/RuntimeManager APIs and the Crypto backend
> through ICrypto are not used at this time.** Their future integration points
> are explicitly marked with TODO identifiers below.

Use the following distinction throughout the document:

| Label | Meaning |
| --- | --- |
| Existing | Present in this repository or supplied by the platform, as identified |
| Target | Agreed behavior to implement, including the acceptance criteria |
| Proposed | Recommended implementation detail requiring review where indicated |
| TODO | An unresolved contract or future integration; not implemented behavior |

Sequence diagrams describe the **target or proposed implementation**, unless
explicitly identified as existing. Internal component and method names in
diagrams are design names, not claims that those classes already exist.
Public `client_*` functions, Protobuf messages and Thunder bootstrap methods
refer to actual API names.

UML sequences show the intended execution flow. Current skeleton/header status
is recorded in method headings and surrounding text, rather than as TODO steps
inside an agreed flow. Genuine policy or format choices are listed separately
so implementation status does not look like an unresolved design decision.

Navigation: [ACs](#2-acceptance-criteria-and-scope),
[platform architecture](#3-high-level-architecture-and-platform-placement),
[protocol](#4-apis-protobuf-and-zeromq),
[client flows](#5-eyilclient-design-and-api-flows),
[Gateway flows](#6-eyilgateway-design-and-flows),
[gaps to answer](#7-gaps-to-answer).

## 2. Acceptance criteria and scope

The IDs below make the supplied ACs traceable to individual flows. They
describe completion of the feature, not the status of the current skeleton.
The duplicated original "AC2" for Firebolt Crypto is numbered **AC3** here.

### 2.1 AC1: EyilGateway plugin

| ID | Target acceptance criterion |
| --- | --- |
| AC1.1 | Run as the Thunder plugin `EyilGateway`, callsign `org.rdk.EyilGateway`. Initially run the implementation in process. |
| AC1.2 | Bind a `ZMQ_ROUTER` to `ipc:///tmp/app-eyil-gateway.ipc` and accept multiple application connections. |
| AC1.3 | Enable `ZMQ_PLAIN_SERVER` on the ROUTER and run a `ZMQ_REP` ZAP handler at `inproc://zeromq.zap.01` in the same ZMQ context. |
| AC1.4 | Run a monitor worker receiving events on a `ZMQ_PAIR` at `inproc://app-eyil-gateway-monitor`. |
| AC1.5 | Extract kernel-derived peer UID from the ZAP address; retain GID and host PID. For this phase, accept valid direct UDS clients without AppManager authorization and allow all gateway APIs. |
| AC1.6 | Assign a unique ZAP `User-Id` per connection. Retrieve it from every received request and resolve an independent connection context. Multiple connections from one process remain independent. |
| AC1.7 | Dispatch every accepted request as a separate job on a Gateway-owned Thunder thread pool. Keep transport and other requests progressing, subject to bounded capacity and declared resource dependencies. |
| AC1.8 | Parse the Protobuf envelope and route `RPC_SERVICE_CRYPTO` to a service adapter. Future backend integration invokes the external Crypto plugin through ICrypto. |
| AC1.9 | Gateway creates, owns and accounts for shared-memory allocations used by encrypt, decrypt, sign and verify. AllocateSharedBuffer and ReleaseSharedBuffer terminate in Gateway. Allocations belong to one connection; multiple allocations per connection are supported. |
| AC1.10 | When a disconnect is reliably associated with a connection, close its admission, notify the integrated Crypto backend through NotifyAppState(DISCONNECTED), and release its resources without invalidating outstanding backend access. Exact notification/drain ordering remains a lifecycle TODO. |

"Separate thread" in the original high-level AC is implemented as an
independent dispatch job using a fixed private worker pool. It does not mean
creating an unbounded OS thread for every request, or promising unlimited
parallelism when workers, backend capacity or shared resources are exhausted.

### 2.2 AC2: EyilClient library

| ID | Target acceptance criterion |
| --- | --- |
| AC2.1 | Expose the C API in `eyilclient/EyilClient.h` for consumers such as the Firebolt Crypto Client Library. The library itself does not depend on Thunder. |
| AC2.2 | Connect a `ZMQ_DEALER` to `ipc:///tmp/app-eyil-gateway.ipc`. Supply internal dummy PLAIN username/password strings to trigger server-side ZAP; they are not app credentials. |
| AC2.3 | Receive socket monitor events through a `ZMQ_PAIR` at `inproc://app-eyil-client-monitor-<unique-client-id>`. |
| AC2.4 | Support synchronous and asynchronous binary and string requests, including multiple outstanding requests on one connection and responses arriving out of order. Report local errors through ClientResult; accepted asynchronous requests receive exactly one completion with status and the submitted user_data. |

The accepted implementation direction is a **library-owned serving/I/O
thread**, initially one per `Client`. Applications do not supply an I/O
handler or pump the library. The supplied AC mentions a separate client monitor
thread. Handling its PAIR socket on the serving loop is a proposed consolidation;
confirmation is required before treating that as satisfying the literal
dedicated-thread wording. See TODO(CLIENT-MONITOR).

Initial client defaults are **15 seconds for connect**, **15 seconds per
request**, and **64 outstanding requests per Client**. These are local operation
limits, not idle-connection expiry. An application may freeze its process after
finishing all operations and callbacks; it owns preparation for that freeze.
Preserving the connection also depends on the ZeroMQ heartbeat policy, which
requires architect discussion. See section 5.5 for socket setup and
TODO(ZMQ-HEARTBEAT), and sections 5.14 and 6.14 for lifecycle assumptions.

The supplied ACs require socket monitoring and cleanup on disconnect. They do
not require application heartbeat RPCs, an inactivity watchdog or ZeroMQ
transport heartbeats. The target client and Gateway contain no custom heartbeat
or inactivity-watchdog logic; the transport-level setting is a separate decision.

### 2.3 AC3: Firebolt Crypto Client Library

| ID | Target acceptance criterion |
| --- | --- |
| AC3.1 | Firebolt Crypto uses EyilClient to route app requests to Gateway and ultimately the external Crypto backend. |
| AC3.2 | Firebolt Crypto serializes/deserializes service-specific Crypto payloads using the agreed Protobuf definitions. EyilClient handles the outer RPC envelope, owns request IDs and sets schema version 1; this agreed functionality remains to be implemented in the skeleton. |

Firebolt Crypto and the real Crypto plugin are separate deliverables. Their
presence in diagrams does not add their implementation to this repository.

### 2.4 Delivery boundary and current status

| Part | Existing skeleton | Remaining implementation |
| --- | --- | --- |
| Thunder wrapper | IPlugin, Root<IConfiguration>(), Configure(), release and remote-connection notification | Integrate a deterministic stop/drain lifecycle |
| Implementation library | IConfiguration, configuration guard and dependency logging | Transport, authentication seam, registry, private pool, router and buffers |
| EyilClient | Nine C functions with placeholder results and response freeing; async callbacks lack status/user_data | Connection handles, coordinator, I/O thread, pending requests and the agreed typed async completion API |
| Protocol | Generated rpc.proto and crypto.proto, schema constant 1 | Agreed RPC envelope processing, validation and service mapping; implement TODO(CLIENT-ENVELOPE) and align the client header comment |
| ZeroMQ | Located and linked by CMake | Runtime sockets, monitors and target peer-credential support |
| AppManager / RuntimeManager | No calls or authentication dependency in this code | TODO(AUTH-APPMANAGER); optional lifecycle integration remains TODO |
| Crypto / ICrypto | No ICrypto header dependency, adapter binding, real backend or stub | TODO(BACKEND-ICRYPTO), after interface and lifecycle contracts are approved |
| Tests and CI | Build/install checks, L1/L2 runners and coverage scaffolding | Runtime tests for the flows in this document; L1/L2 currently contain zero tests |

The first transport implementation should keep the two external seams explicit:
an interim identity provider accepts connections, and an unavailable service
backend reports a clear failure for backend-dependent operations. It must not
manufacture successful cryptographic results. Gateway-local allocation/release
can be implemented independently of ICrypto. These are planned boundaries,
not runtime components already implemented.

## 3. High-level architecture and platform placement

### 3.1 RDK deployment

The target is an RDK platform with AppManager and RuntimeManager. The initial
Gateway implementation shares Thunder's process, while an application and its
EyilClient instance run inside an application container. The wrapper/implementation
split preserves a path to a separate implementation process later.

The platform plugins are part of the intended deployment **even while Gateway
does not call them**. Local PC builds and isolated development tests are useful
validation environments; they do not define a second production deployment
based on a legacy application service.

```mermaid
flowchart LR
    subgraph APP["Existing application container"]
        A["Application / Firebolt app"]
        F["External Firebolt Crypto library"]
        C["NEW EyilClient"]
        MAP["App-side buffer mapping helper - TODO owner"]
        SHM["Existing private /dev/shm"]
        A --> F
        F --> C
        F --> MAP
        MAP --> SHM
    end
    subgraph TH["Existing host Thunder process - initial deployment"]
        subgraph GW["EyilGateway - org.rdk.EyilGateway"]
            WR["NEW EyilGateway IPlugin"]
            IM["NEW EyilGatewayImplementation"]
            IO["PLANNED ROUTER, ZAP and monitor"]
            REG["PLANNED connection registry"]
            POOL["PLANNED private Thunder request pool"]
            AD["PLANNED service adapter boundary"]
            BM["PLANNED shared-buffer manager"]
            WR --> IM
            IM --> IO
            IO --> REG
            IO --> POOL
            POOL --> AD
            POOL --> BM
        end
        AM["Existing AppManager"]
        RM["Existing RuntimeManager"]
        AM --> RM
    end
    CP["External Crypto plugin / ICrypto - NOT INTEGRATED"]
    SEC["External platform security libraries / TEE"]
    K["Existing Linux: UDS credentials, procfs, tmpfs"]
    D["Existing Dobby container runtime"]
    C -- "Protobuf over ZMQ over shared UDS" --> IO
    K --> IO
    RM --> D
    D -. "Launch / container configuration" .-> APP
    REG -. "TODO: UID lookup and API entitlements" .-> AM
    AD -. "TODO: trusted ApplicationContext + ICrypto" .-> CP
    CP -.-> SEC
    BM -- "Create in peer mount namespace, retain FD" --> SHM
    CP -. "Future open/map/use/close via trusted path" .-> SHM
```

The inner EyilGateway block groups components owned by the plugin. AppManager
and RuntimeManager remain separate plugins within the Thunder process. The
diagram distinguishes existing platform infrastructure from new Gateway
components and future integrations. Crypto may itself be in process or remote;
Gateway must not depend on a raw pointer or FD being meaningful in Crypto's
process.

### 3.2 Components, ownership and trust boundaries

| Component | Owner / location | Responsibility |
| --- | --- | --- |
| Application / Firebolt Crypto | Application container; external consumer | Express operations, serialize Crypto payloads and consume results |
| EyilClient | New library in the application's process | Transport coordination, RPC correlation and C API buffer ownership |
| App-side mapping helper | Placement TODO; not part of the current EyilClient transport API | Open/map the name returned by allocation; manage app mappings |
| EyilGateway wrapper | New Thunder IPlugin | Framework lifecycle and implementation acquisition |
| EyilGatewayImplementation | New implementation library | Own Gateway transport, contexts, request pool, adapters and allocations |
| AppManager / RuntimeManager | Existing RDK platform plugins | Future authoritative identity/entitlements; lifecycle information if adopted |
| Dobby / DobbyTool | Existing container runtime / diagnostic tool | Launch, container namespace/mount setup and test execution; no request routing |
| Linux kernel / libzmq | Existing platform dependencies | UDS transport and peer credentials; monitor support depends on the target libzmq |
| Crypto plugin / ICrypto | External backend, not integrated | Crypto execution, lower-level sessions, keys and key authorization |
| Sec API / secure hardware / TEE | External platform-specific implementation | Backend-dependent cryptographic operations; no Gateway dependency |

There is **one listening UDS endpoint**, shared by all intended applications.
Each accepted connection gets its own identity, request accounting and buffer
ownership. Separate connections do not require separate socket paths.

The app cannot authoritatively provide its UID, appId, sessionId or a host
shared-memory path. The transport supplies peer credentials; the Gateway creates
the connection context and resolves opaque buffer IDs. Key policy remains in
Crypto, including locators, usages, import/export restrictions and sharing.

### 3.3 Container mounts and direct access

The AC endpoint is `ipc:///tmp/app-eyil-gateway.ipc`. Deployment must expose the
same socket object to intended containers, with deliberate directory/socket
permissions. Sharing all of host `/tmp` also exposes unrelated objects; a narrow
mount is preferred when supported. A directory mount avoids binding a container
to an obsolete socket inode after the server recreates it. Final Dobby mount
configuration and permissions remain TODO(DEPLOYMENT).

Monitor and ZAP `inproc://` endpoints are local to a ZMQ context; they need no
filesystem socket or container mount.

Ordinary application containers must not acquire the Crypto interface directly
through Thunder's communicator. `ApplicationContext` is identity metadata; it
does not authenticate the COM-RPC caller. The platform direction is to restrict
general app access to Thunder. Verify the actual communicator mounts and access
policy before enabling Crypto integration.

Gateway's private pool isolates its request scheduling from Thunder's global
worker queue. It does not isolate process crashes, total CPU/memory pressure or
a blocked in-process backend. Those limits matter while Gateway is in process.

### 3.4 External integration seams

| TODO | Planned insertion point | Behavior before integration |
| --- | --- | --- |
| TODO(AUTH-APPMANAGER) | AuthenticationProvider called by ZAP; future Gateway → AppManager → RuntimeManager lookup using trusted UID | Accept valid direct UDS peers, record UID/GID/PID, create unique interim appId/sessionId per connection |
| TODO(API-AUTHORIZATION) | Admission check against entitlements cached in the connection context | Allow all structurally valid supported APIs; still enforce limits and buffer ownership |
| TODO(BACKEND-ICRYPTO) | CryptoServiceAdapter implementing an internal service-backend boundary | No ICrypto calls or generated success results; backend-dependent requests fail explicitly |
| TODO(BACKEND-SESSION) | Ordered backend session initialization and drain hooks on the connection context | Keep state model independent of external Crypto types |
| TODO(LIFECYCLE-EVENTS) | Lifecycle observer receiving platform events and submitting serialized context transitions | No AppManager/RuntimeManager subscriptions or inferred hibernate/idle policy |

The interface names in this table describe internal seams. They are not
additional app-facing Thunder interfaces and do not claim that a formal
AppManager authentication API is already available. Future API additions should
be confined to these seams, rather than spreading external plugin types through
the client, transport or buffer registry.

## 4. APIs: Protobuf and ZeroMQ

### 4.1 The two Protobuf files

| File | Purpose | Boundary |
| --- | --- | --- |
| [rpc.proto](../apis/proto/rpc.proto) | Service-neutral RpcRequest/RpcResponse, request_id, service, schema_version and RpcStatus | EyilClient ↔ Gateway routing |
| [crypto.proto](../apis/proto/crypto.proto) | CryptoRequest/CryptoResponse, operations, key metadata, buffer allocation/release and offset/length regions | Crypto consumer ↔ Gateway Crypto adapter |

`RpcRequest.payload` contains a serialized service request.
`RpcResponse.payload` contains the service response when available. Crypto
defines an operation enum and a `oneof`; they must agree. A valid Protobuf parse
alone does not validate the operation, enum values, required message presence
or allowed lengths.

The RPC envelope fields are:

| Message | Fields |
| --- | --- |
| RpcRequest | request_id: uint64; service: RpcService; schema_version: uint32; payload: bytes |
| RpcResponse | request_id: uint64; service: RpcService; schema_version: uint32; status: RpcStatus; payload: bytes |

[ProtocolVersion.h](../apis/proto/ProtocolVersion.h) fixes the current envelope
schema to **1**. EyilClient sets it for each RPC. There is no version negotiation
or fallback protocol in the current schema. Library/package ABI versions and
Thunder plugin metadata versions are separate concepts.

**Agreed functionality — not yet implemented in the skeleton:** EyilClient
serializes/deserializes the outer RPC envelope, owns request IDs, sets
schema_version to 1 and correlates responses. These are implementation tasks,
not open architecture decisions.

For the existing byte API, the caller supplies a serialized RpcRequest carrying
service and payload. EyilClient parses the envelope, assigns request_id, sets
schema_version and serializes it for transmission. On receipt, it parses and
validates RpcResponse, matches request_id to the pending call and returns the
serialized response to that caller. The service is carried in the envelope;
no separate service argument is needed. Crypto payload bytes remain opaque to
EyilClient and are interpreted by the service-specific consumer.

**TODO(CLIENT-ENVELOPE):** implement this agreed behavior and update the public
header comment accordingly. The current comment above client_send_request()
says that the API does not perform Protobuf serialization/deserialization; it
has not yet been aligned with the agreed envelope handling. Its wording should
distinguish RPC envelope processing from service-payload interpretation. The
flows below show the agreed target behavior, which is missing from the current
skeleton.

### 4.2 Status and error layers

| Layer | Meaning and handling |
| --- | --- |
| ClientResult | Local C API validation, connection, allocation, timeout or communication result |
| RpcStatus | Gateway routing/execution result; explicitly inspect it even when a response was received |
| CryptoError | Crypto service result, carried inside CryptoResponse |
| Future Core::hresult | Whether the ICrypto invocation itself succeeded; check before reading output parameters |

`CLIENT_OK` on an asynchronous submission means local acceptance, not
remote success. `RPC_STATUS_OK` means the Gateway successfully produced the
service response; `CryptoResponse.error` can still report a failed crypto
operation. Protobuf's zero default `RPC_STATUS_UNSPECIFIED` is not success.

#### Agreed local error behavior

The following client behavior is agreed and remains to be implemented in the
skeleton. It does not depend on AppManager or Crypto backend integration.
Apply the result through the synchronous return value, an immediate async
rejection or an accepted async request's completion, as appropriate.

| Situation | ClientResult | Response handling |
| --- | --- | --- |
| Invalid arguments or invalid caller-supplied request envelope | CLIENT_INVALID_ARGUMENT | No response |
| Submission without a usable connection | CLIENT_NOT_CONNECTED | No response |
| Authentication explicitly rejected | CLIENT_AUTH_FAILED | No response |
| Local memory allocation failure | CLIENT_OUT_OF_MEMORY | No response |
| The 15-second connect or request deadline expires | CLIENT_TIMEOUT | No Client handle on connect failure; no response on request timeout, and remote execution may still be in progress |
| Connection lost during a request, malformed response or terminal communication failure | CLIENT_COMMUNICATION_ERROR | No response |
| Valid correlated RPC response received and delivered successfully | CLIENT_OK | Deliver the serialized response; the caller still checks RpcStatus and the service result |

For byte responses, every local error produces `response_data == NULL` and
`response_size == 0`. For string responses, it produces `response_data == NULL`.
A valid Gateway error response, including RPC_STATUS_QUEUE_FULL, is still a
received RPC response: return CLIENT_OK with its bytes and preserve RpcStatus.
Do not turn a local failure into a fabricated Gateway RpcResponse or CryptoError.

Both connect and request timeouts are **15,000 ms** in the initial version,
using one common internal duration constant. No new public timeout-configuration
API is required for this version. An authentication error requires an explicit
rejection; an unavailable endpoint or lost connection is not evidence of denied
access.
Temporary socket backpressure is handled by bounded queue/deadline policy and
is not automatically a terminal communication failure. The local queue-full
result remains a separate admission-policy question because ClientResult has
no dedicated queue-full value; the initial count limit is 64 outstanding requests
per Client, as defined in section 5.14.

An async submission that returns an error was not accepted and must not invoke
its response callback. Once submission returns CLIENT_OK, the library owes
exactly one completion with either CLIENT_OK and a response or a local error
and no response. The typed callback extension is specified in section 5.4.
Connection-event notifications do not replace per-request completions.

A timeout or connection loss does not prove remote cancellation. Do not replay
the operation automatically or claim that its shared buffer is safe to reuse.

| RpcStatus | Intended use |
| --- | --- |
| UNSPECIFIED (0) | Missing/unset result; reject as an invalid response |
| OK (1) | Successfully routed and processed at the Gateway layer |
| INVALID_REQUEST (2) | Parsed envelope/service request violates the agreed structure |
| UNSUPPORTED_SCHEMA (3) | Unsupported envelope schema_version |
| UNSUPPORTED_SERVICE (4) | Unknown or unregistered RPC service |
| MALFORMED_REQUEST (5) | Cannot decode the expected message/framing |
| INVALID_CONNECTION (6) | Missing, stale or inadmissible authenticated connection context |
| INTERNAL (7) | Gateway failure or, provisionally, unavailable/failed backend invocation |
| QUEUE_FULL (8) | Bounded request admission rejected the request |

Exact Gateway service-validation versus CryptoError mappings, malformed-message
correlation and backend-unavailable mapping remain TODO(ERROR-MAPPING).
Without a trustworthy request ID or route, do not fabricate a correlated reply.
Transport/authentication failures can occur before any RpcResponse is possible.

### 4.3 Protocol capabilities and limitations

| Capability | Implementation consequence |
| --- | --- |
| Independent request_id | Many requests may be outstanding; match responses by ID, not arrival order |
| Explicit service enum | A new service needs a schema/adapter addition; only CRYPTO is currently defined |
| Opaque service payload | Transport does not implement Crypto key policy |
| Buffer ID plus regions | Four bulk-data operations avoid embedding their input/output in the RPC message |
| uint32 buffer fields | Validate size/ranges with overflow-safe arithmetic and stricter configured quotas |
| No app identity fields | Derive context from trusted transport metadata |
| No deadline/cancel message | A local timeout cannot cancel backend work or prove buffer reuse is safe |
| No notification envelope | Public callback registration does not define a wire notification protocol |
| No retry/deduplication contract | Do not replay key generation, import or other operations automatically |
| No text encoding definition | String API cannot carry arbitrary binary Protobuf via strlen(); its codec remains TODO |
| No lifecycle/heartbeat messages | Do not invent idle expiry, hibernation commands or heartbeat services |

The protocol permits concurrent requests; it does not define ordering for
operations sharing a key, buffer or session. Request-ID correlation also does
not provide exactly-once execution or distinguish a lost reply from an operation
that never ran.

### 4.4 ZeroMQ endpoints and message framing

| Endpoint | Socket / owner | Purpose |
| --- | --- | --- |
| `ipc:///tmp/app-eyil-gateway.ipc` | Gateway ROUTER binds; each client DEALER connects | Application RPC over filesystem UDS |
| `inproc://zeromq.zap.01` | Gateway ZAP REP binds | Internal authentication exchange |
| `inproc://app-eyil-gateway-monitor` | ROUTER monitor publishes; Gateway PAIR receives | Gateway transport events |
| `inproc://app-eyil-client-monitor-<unique-client-id>` | DEALER monitor publishes; client PAIR receives | Client transport events |

The sample's DEALER sends one frame containing the serialized RpcRequest.
ROUTER receives `[routing_id][RpcRequest bytes]` and replies with
`[routing_id][RpcResponse bytes]`; DEALER receives the response body only.
Retain this framing as the proposed baseline. There is no empty REQ delimiter.
Validate the frame count and consume/reject an entire malformed multipart
message before accepting another. Define size and frame limits explicitly.

The ROUTER I/O owner copies `zmq_msg_gets(message, "User-Id")` before closing
the received message. The routing frame is a delivery address, not authenticated
identity. ZAP's User-Id is a different identifier.

Each DEALER and ROUTER has one socket-owning thread. API callers and request
workers communicate with it through queues; they never send or receive on its
socket. This follows the restrictions for these socket types in the
[ZeroMQ socket API](https://libzmq.readthedocs.io/en/latest/zmq_socket.html).
Socket options, connection, monitoring and closure follow the same ownership.

PLAIN server mode belongs on the ROUTER. The REP handler answers ZAP exchanges;
EyilClient supplies dummy PLAIN strings and never runs a ZAP server. The trusted
UID/GID/PID representation depends on the target libzmq implementation, rather
than the client password. See the
[ZAP specification](https://rfc.zeromq.org/spec/27/).

### 4.5 Reference baseline and deliberate changes from samples

The architect-provided reference materials are the latest
`CryptoGateway/EyilGateway/` sample,
its `Gateway-Owned-Shared-Memory-Proposal.md`,
`zmq_disconnect_sending_UID_GID_PID.patch` and `zmq_server.cpp`.
The table below compares these materials with the agreed implementation
decisions for this repository. They are design references, not build dependencies.

| Reference behavior | Direction for this repository |
| --- | --- |
| Gateway-owned allocation, opaque buffer ID and trusted backend path | Retain; add concurrency/lifetime constraints below |
| Sample transport_error_code and heartbeat service | Use the checked-in RpcStatus schema; no heartbeat service |
| Sample application-level RPC heartbeat | Removed from the target client and Gateway; no heartbeat RPC, sender thread or handler |
| Sample libzmq transport heartbeats on DEALER and ROUTER | Architect discussion required: heartbeat expiry can disconnect a hibernated app; proposed disabled settings are recorded at client/Gateway socket setup |
| Sample client_watchdog removes sessions after ClientTimeout=15 seconds without a request | Replaced in the target design by correlated disconnect-event cleanup, not by another idle timer |
| Sample client_connect waits for HANDSHAKE_SUCCEEDED, but reports AUTH_FAILED on wait expiry | Retain handshake readiness; return CLIENT_TIMEOUT on deadline expiry and distinguish explicit failures |
| Sample 15-second connect wait, send timeout and response wait | Use the agreed 15-second connect deadline and one 15-second total request deadline, without restarting it for separate send/receive phases |
| Sample single pending client request and multiple socket users | One I/O owner and a request-ID pending map |
| Sample synchronous dispatch in the ROUTER loop | One private Thunder-pool dispatch job per accepted request |
| Credential monitor patch | Evaluate for events; it does not provide a complete per-connection correlation token |
| Sample hardcoded identities / detached background threads | Generate independent connection identities; own and join the required Gateway threads |

The current repository's schema and accepted decisions take precedence when a
sample differs. No runtime sample code is imported by this documentation change.
Do not mix current and legacy sample binaries solely because both advertise
schema 1: for example, the old zero transport-error value meant success, while
zero RpcStatus now means UNSPECIFIED. Use matching generated definitions at
both ends.

## 5. EyilClient design and API flows

### 5.1 Responsibilities and limits

EyilClient provides transport coordination, not Crypto semantics. A caller can
submit concurrent synchronous and asynchronous requests on the same `Client`.
Only a synchronous caller waits; it must not hold a lock needed by the I/O owner
or another caller while waiting.

The proposed first implementation has one library-owned serving thread and one
owned ZMQ context per `Client`. A shared context or shared serving loop can be
an optimization after ownership and shutdown are proven. libzmq also creates
its own internal I/O threads: those do not maintain the library's pending map,
run its callbacks or replace its serving thread.

There is no application heartbeat producer, heartbeat worker, periodic liveness
RPC or inactivity watchdog in the target client. Connection-state handling uses
the monitor events and the request coordinator. The two 15-second operation
deadlines remain. Whether libzmq itself sends transport heartbeats is separately
tracked at connection setup in section 5.5; it is not a client-library timer job.

The library does not:

- Call AppManager, RuntimeManager, Thunder or ICrypto.
- Decide key authorization, generate trusted app identity or choose host paths.
- Allocate, map or release Gateway-owned buffers implicitly through the generic
  transport API. The app-side Crypto layer requests and maps them explicitly.
- Promise that reconnect preserves keys, buffers or a Gateway session.
- Treat a timeout, a successful send or a connection callback as proof that a
  remote Crypto operation finished.

### 5.2 Component organization and ownership

| Planned component | Responsibility | Owner / synchronization |
| --- | --- | --- |
| C API facade | Validate arguments, clear outputs, parse the outer request into owned storage and hold a live call reference | Calling application thread; never touches DEALER |
| Per-Client coordinator | State, generation, admission, command queue and shutdown coordination | Short protected state transitions |
| Request record | Copied request, assigned ID, handler/user_data or waiter, state and absolute 15-second deadline | Retained until one terminal local outcome |
| Serving/I/O loop | Drain submissions, send/receive, correlate replies, monitor events | Library-owned thread; sole DEALER owner |
| Pending map | request_id → waiter/callback and expected response metadata | Serving thread; completion ownership transferred once |
| Completion target | Condition variable for sync caller or async callback with ClientResult and user_data | Exactly one terminal result; request callbacks run on the client I/O owner without internal locks held |
| Monitor collector | Read/decode PAIR events and hand them to connection-state handling | I/O owner performs these monitor-thread duties in the proposed design; a separate worker remains an AC wording decision |
| Response storage | Independently allocated byte/string result | Ownership passes to application, freed with client_free_response |

```mermaid
flowchart LR
    A["Application / Firebolt Crypto<br/>Application threads"]
    subgraph LIB["EyilClient runtime / libeyilclient.so - same application process"]
        subgraph API["Public C API - called by the application"]
            LIFE["client_connect()<br/>client_disconnect()"]
            REQ["client_send_request()<br/>client_send_request_async()<br/>client_send_request_string()<br/>client_send_request_string_async()"]
            NOTIFY["client_register_notifications()<br/>client_unregister_notifications()"]
            FREE["client_free_response()"]
        end
        CO["Per-Client coordinator<br/>Validation, admission and request state"]
        Q["Bounded submission queue + wakeup"]
        IO["Library-owned I/O owner thread<br/>Poll DEALER + monitor PAIR + wakeup<br/>Handle replies and connection events"]
        D["DEALER socket"]
        P["Pending map by request_id"]
        SYNC["Per-call synchronous waiter"]
        CB["Async completion<br/>ClientResult + response + user_data"]
        HANDLERS["Connection / notification handlers"]
        EVENTS["libzmq internal monitor sender<br/>DEALER connection events"]
        M["PAIR monitor receiver<br/>Polled and decoded by the I/O owner<br/>Same monitor-event collection duties"]
        MN["Design note - AC confirmation<br/>PAIR receiver + I/O owner cover monitor-thread duties<br/>No separate monitor worker in this proposal"]
        LIFE -- "Start / stop coordination" --> CO
        CO --> Q
        LIFE -- "Start / join serving thread" --> IO
        LIFE -- "Connection handler registration" --> HANDLERS
        REQ -- "Validate and prepare owned request" --> CO
        NOTIFY -- "Register / remove local handler" --> HANDLERS
        Q -- "Commands and requests" --> IO
        IO -- "Send requests" --> D
        D -- "Receive responses" --> IO
        IO -- "Correlate result / settle local error" --> P
        P --> SYNC
        P --> CB
        IO -- "Connection events / future notifications" --> HANDLERS
        D -. "Monitoring enabled" .-> EVENTS
        EVENTS -- "Internal inproc event channel" --> M
        M -- "Connection state events" --> IO
        MN -. "Same proposed owner thread" .-> M
        MN -. "Collect and handle monitor events" .-> IO
    end
    G["Gateway ROUTER<br/>ipc:///tmp/app-eyil-gateway.ipc"]
    A -- "Connect / disconnect" --> LIFE
    A -- "Sync / async request calls" --> REQ
    A -- "Register / unregister notifications" --> NOTIFY
    A -- "Free returned response storage" --> FREE
    SYNC -- "Return ClientResult and response" --> A
    CB -- "Invoke request completion handler" --> A
    HANDLERS -- "Invoke registered handler" --> A
    D -- "RpcRequest over UDS" --> G
    G -- "RpcResponse over UDS" --> D
```

The application explicitly calls the public functions shown in the inner API
block. Those entry points run on the calling application thread; request and
control queues transfer work to the library-owned I/O loop. Synchronous results
return to the caller, while asynchronous results use the registered per-request
handler and user_data. Section 5.4 defines request-callback execution rules.
Response freeing and notification registration are local API operations.

The PAIR monitor receiver is an internal socket plus event-decoding code.
Enabling monitoring on DEALER makes libzmq create the other PAIR endpoint, which
emits connection, handshake and disconnect events. The receiver connects through
the client monitor's inproc endpoint. It carries transport events rather than
RPC responses. See the
[ZeroMQ monitor API](https://libzmq.readthedocs.io/en/latest/zmq_socket_monitor.html).
In the proposed arrangement, the same client I/O loop reads DEALER and the
PAIR receiver; the PAIR block does not itself imply a separate thread or service.
The dedicated-monitor-thread wording remains TODO(CLIENT-MONITOR).

> **Note for the PAIR and I/O-owner blocks:** together they perform the monitor
> thread's expected event-collection and connection-state duties. The PAIR block
> is a socket and decoder executed by the I/O owner, not another thread. A
> separate worker is technically optional; accepting this arrangement against
> the AC's explicit monitor-thread wording remains to be confirmed.

Monitoring is a responsibility, not necessarily a dedicated thread. The two
proposed arrangements consume the same PAIR events:

| Thread or component | Responsibility | Current skeleton status |
| --- | --- | --- |
| libzmq's internal I/O threads | Transport/protocol work and production of socket-monitor events | Used only once a ZMQ context/socket runtime is implemented |
| EyilClient serving/I/O owner | Own DEALER, poll replies, submissions and deadlines; in the proposed consolidated model, also poll the monitor PAIR and update connection state | Not implemented |
| Optional dedicated monitor worker | Own only the monitor PAIR and post decoded events to the coordinator/I/O owner | Not implemented; needed only if the literal AC requires a distinct worker |

In the consolidated model, poll the DEALER, monitor PAIR and submission wakeup
together. Reading a monitor event must not wait for an RPC response. A dedicated
monitor worker would separate that event-reading work, but it would not replace
the I/O owner or its pending-request coordination. ZeroMQ's monitor API requires
a PAIR receiver, not a particular application-thread arrangement. The remaining
CLIENT-MONITOR question is acceptance of the AC's thread wording, rather than
missing monitoring functionality in the proposed design. Neither arrangement
is implemented in this skeleton.

Freezing the whole application stops both its serving thread and any dedicated
monitor thread, including libzmq's own threads. Adding a separate monitor thread
cannot make the frozen application process events. In either arrangement,
callbacks must remain short to avoid delaying the client's processing.

#### Monitor responsibilities for AC confirmation

Confirm the following monitoring contract independently of whether a dedicated
OS thread is required. The consolidated implementation assigns collection and
handling to the existing I/O owner. A dedicated collector would own only PAIR
and forward events to that owner; it must not also read or write DEALER or
independently mutate the pending-request map.

| Expected monitor responsibility | How PAIR plus the I/O owner cover it |
| --- | --- |
| Set up event collection before connecting | The DEALER owner enables monitoring at the unique client monitor endpoint; the collector creates/connects its PAIR receiver in the same ZMQ context |
| Receive and decode complete monitor messages | Poll PAIR alongside DEALER and the submission wakeup; validate the event frames, event value and endpoint for the configured monitor API version |
| Establish handshake readiness | On HANDSHAKE_SUCCEEDED, publish READY and wake client_connect; CONNECTED or connection-retry events alone do not establish readiness |
| Report initial connection/authentication failures | Decode failure details and wake the connect waiter with the exact ClientResult from section 5.5; retain the distinction between authentication denial and transport/protocol failure |
| Handle loss of an established connection | Mark the connection unusable, stop new admission and hand off completion of affected requests with CLIENT_COMMUNICATION_ERROR; emit the applicable connection event |
| Handle later state transitions without duplicates | Serialize state changes, ignore events belonging to a stopped runtime and issue connection callbacks only for relevant transitions; actual reconnect behavior remains CONNECTION-LIFETIME |
| Keep application callbacks and request completion coordinated | Invoke connection handlers without internal locks held and settle each affected request exactly once through the owner; a connection notification cannot replace per-request completion |
| Handle monitor failure and orderly stop | Treat unexpected collector failure as a runtime error; during requested shutdown, wake the poller, stop monitoring and close PAIR on its owner without waiting indefinitely for another event |

The selected monitor events and their framing come from the
[ZeroMQ monitor API](https://libzmq.readthedocs.io/en/latest/zmq_socket_monitor.html).
The ClientResult mapping, pending-request completion and callback rules are
EyilClient responsibilities built on those events. The 15-second connect/request
timers are coordinator/I/O-loop work; monitor events alone do not implement
those deadlines.

The loop must fairly service monitor events, replies and submissions in bounded
batches. Functional equivalence does not imply identical scheduling isolation:
a blocking callback on the shared owner delays monitor processing too. A
separate collector can keep reading events in that situation, but pending-map
updates and request completions still need the owner. The proposed design
therefore requires nonblocking I/O and short application callbacks.

**Confirmation to obtain:** does the PAIR receiver plus the I/O owner, with the
responsibilities above, satisfy AC2.3, or is an additional dedicated OS thread
itself mandatory? No heartbeat sender, idle-session watchdog or periodic PID
responsiveness probe is part of this monitor contract. The monitor also does
not perform Gateway-side ZAP authentication or decode service responses.

#### Request submission and completion

A wakeup FD or equivalent queue notification lets application threads wake the
serving loop without sharing a ZMQ socket. Copy an asynchronous request before
returning to the caller, so the caller may reuse its input memory. Queue bounds
must include copied request bytes as well as the number of requests.

Proposed request states are `QUEUED → SENT → COMPLETED`, with terminal local
failure/timeout/shutdown paths. Remove a pending entry atomically before
delivering completion. Reserve IDs before sending; do not reuse an ID while
a late response from the same connection generation could still arrive.
Validate response ID, service, schema and status before completing the caller.

### 5.3 General routing: concurrent requests on one connection

Coverage: **AC2.1, AC2.2, AC2.4, AC1.6, AC1.7, AC3.1–AC3.2**.

```mermaid
sequenceDiagram
    participant A as App thread A
    participant B as App thread B
    participant API as C API / coordinator
    participant IO as EyilClient I/O owner
    participant GW as Gateway ROUTER owner
    participant W as Gateway private pool
    A->>API: client_send_request_async(request A, callback A, user_data A)
    API->>API: Parse owned outer envelope, reserve bounded request state
    API-->>A: Local submission result
    B->>API: client_send_request(request B)
    API->>IO: Wake with queued A and B
    IO->>IO: Assign IDs and schema 1, serialize and install pending entries
    IO->>GW: zmq_send(DEALER, RpcRequest A) over UDS
    IO->>GW: zmq_send(DEALER, RpcRequest B) over same UDS connection
    GW->>W: Dispatch job A
    GW->>W: Dispatch job B
    Note over A,B: Only synchronous caller B waits
    W-->>GW: Completion B before A
    GW-->>IO: RpcResponse B
    IO->>IO: zmq_msg_recv, validate and match ID B
    IO-->>API: Complete waiter B
    API-->>B: Serialized response B
    W-->>GW: Completion A
    GW-->>IO: RpcResponse A
    IO->>IO: zmq_msg_recv, validate and match ID A
    IO-->>A: callback A with CLIENT_OK, owned response bytes and user_data A
```

This is not a strict send/receive alternation. DEALER/ROUTER supports the message
flow; request correlation and delivery to the right API caller are EyilClient
responsibilities. Slow application callbacks must not monopolize its I/O loop.

### 5.4 Public API index

The checked-in skeleton signatures are in
[EyilClient.h](../EyilClient/include/eyilclient/EyilClient.h). The agreed async
extension below is not yet reflected in that header or implementation. All nine
methods have individual sequences below. Method headings identify current
implementation status separately from those target sequences. The header
explicitly documents CLIENT_NOT_IMPLEMENTED for the two async request methods;
the other unimplemented/no-op labels below come from the current .cpp skeleton.

| Method | Target behavior | AC coverage |
| --- | --- | --- |
| client_connect | Return a handle after successful handshake; report exact startup/authentication/timeout errors | AC2.1–AC2.3; AC1.3, AC1.5–AC1.6 |
| client_send_request | Binary request with a per-call synchronous waiter | AC2.1, AC2.2, AC2.4 |
| client_send_request_async | Binary submission with one typed completion and per-request user_data | AC2.1, AC2.2, AC2.4 |
| client_send_request_string | Defined text codec plus synchronous request core | AC2.1, AC2.4 |
| client_send_request_string_async | Defined text codec plus one typed completion and per-request user_data | AC2.1, AC2.4 |
| client_free_response | Release a library-allocated result | AC2.1 |
| client_register_notifications | Register a local handler; wire notifications remain TODO | AC2.1; not a completed notification protocol |
| client_unregister_notifications | Remove the handler and define callback quiescence | AC2.1 |
| client_disconnect | Stop admission/I/O, settle pending work and destroy the handle | AC2.1–AC2.3; AC1.10 on the Gateway side |

The skeleton currently returns `CLIENT_NOT_IMPLEMENTED` for unimplemented
result-returning functions, validates required output pointers and clears
outputs. Disconnect/unregister are no-ops. Only response freeing is functional.
The following sequences are implementation targets, not tests that pass today.

#### Agreed async callback API extension

Both request callback types receive ClientResult and the same opaque user_data
pointer supplied with that request. The intended C signatures are:

```c
/* Declare ClientResult before these callback typedefs. */
typedef void (*ClientResponseHandler)(
    Client *client,
    ClientResult result,
    uint8_t *response_data,
    size_t response_size,
    void *user_data);

typedef void (*ClientStringResponseHandler)(
    Client *client,
    ClientResult result,
    char *response_data,
    void *user_data);

ClientResult client_send_request_async(
    Client *client,
    const uint8_t *request_data,
    size_t request_size,
    ClientResponseHandler handler,
    void *user_data);

ClientResult client_send_request_string_async(
    Client *client,
    const char *request_data,
    ClientStringResponseHandler handler,
    void *user_data);
```

**TODO(CLIENT-ASYNC-COMPLETION):** update the public header, implementation and
consumers to these agreed signatures. This changes the C callback/function
contract and requires callers to be updated and rebuilt. It does not change
the Protobuf schema or its version 1.

The following completion rules apply to both async APIs:

1. Validate the handle, input, callback and local admission before accepting
   the request. Copy input bytes and retain the handler/user_data in its record.
   If submission fails, return a non-OK ClientResult and invoke no callback.
2. A CLIENT_OK submission creates one completion obligation. Reserve enough
   request state to report a later error even if response allocation fails.
3. On success, invoke the handler with CLIENT_OK and an owned serialized
   response. The app must inspect RpcStatus/service status and eventually call
   client_free_response(). For strings, deliver an owned NUL-terminated result
   in the selected text representation described in section 5.8.
4. On local failure, invoke the handler with the mapped ClientResult and NULL
   response; the byte callback additionally receives size zero. The error is
   reported in ClientResult, so no response allocation is needed to report it.
5. Deliver the original user_data unchanged on success and failure. It is a
   local caller-owned pointer, never serialized or sent to Gateway. NULL is
   allowed. Initialize its referenced state before submission and keep it valid
   until the callback finishes. EyilClient does not free it.
6. Resolve competing response, timeout and connection-loss paths through one
   terminal transition. Remove/retire the pending entry once; a late or duplicate
   response must not trigger another callback or satisfy a different request.
7. A connection-loss event completes affected accepted requests individually
   with CLIENT_COMMUNICATION_ERROR. A connection callback alone is insufficient.
   No automatic request replay follows a local failure.

The user_data pointer identifies the caller's request even when there are no
response bytes from which to read a request ID. Connection and notification
callbacks retain their existing signatures; this extension applies to request
completion callbacks only.

#### Request-callback execution model

The target client implementation uses the following concrete execution rules:

- Invoke request-completion callbacks on the owning Client's serving/I/O thread.
  They are serialized within that Client; different Client instances may execute
  callbacks concurrently. Completion order follows responses/errors, not request
  submission order.
- Retire the pending record and release internal state/queue locks before
  invoking application code. Keep the Client and completion storage alive for
  the callback invocation.
- Never invoke a completion inline on the submission call stack. A callback on
  the I/O thread can nevertheless start before the submitting application thread
  has returned from its async API call. Initialize user_data before submission.
- Request-completion handlers may free their response or enqueue another async
  request. New submissions use the normal queue; they cannot invoke another
  response callback recursively before the current callback returns.
- Blocking client calls, including synchronous send and connect, and
  client_disconnect must not be called from a request-completion handler. A
  synchronous send detects its own I/O-owner thread and returns
  CLIENT_INVALID_ARGUMENT instead of waiting on itself. Schedule blocking or
  shutdown work on an application thread.
- Callbacks must return promptly so other requests, monitor events and deadlines
  can progress. Hand heavy application work to the application's own executor.

These are client implementation rules and do not depend on a future backend or
AppManager API. Connection/notification handler replacement and quiescence remain
TODO(CLIENT-CALLBACKS); their existing non-reentrant contract is separate from
the request-completion rules above. Explicit client_disconnect must settle
accepted work through typed completions; the local-close status and shutdown
ordering remain TODO(CLIENT-SHUTDOWN).

### 5.5 client_connect() — not implemented in the skeleton

Coverage: **AC2.1–AC2.3; AC1.3, AC1.5–AC1.6**.

#### Readiness contract

**Agreed readiness contract:** return CLIENT_OK and publish a valid Client
handle only after local startup has succeeded and the client monitor receives
`ZMQ_EVENT_HANDSHAKE_SUCCEEDED` for the pending connection attempt. The
application can then immediately submit RPC requests. Crypto backend availability
and operation results are handled by subsequent RPCs.

The architect-provided sample already uses this handshake event to set its
connected state and wake a condition-variable wait. It waits up to 15 seconds,
but incorrectly returns CLIENT_AUTH_FAILED when the wait expires. Retain its
handshake-readiness mechanism, use the agreed **15-second connect deadline**,
and apply the agreed CLIENT_TIMEOUT mapping. Section 5.14 defines the shared
duration constant and its separate application to each request.

#### ZeroMQ heartbeat options at connection setup — architect discussion

The sample enables ZeroMQ heartbeats inside client_connect(), after creating
DEALER and before calling zmq_connect(). These are socket options implemented
by libzmq, not the removed application-level heartbeat RPC or worker:

| Option | Sample setting | Meaning |
| --- | --- | --- |
| ZMQ_HEARTBEAT_IVL | 5,000 ms | Enable periodic transport PING commands |
| ZMQ_HEARTBEAT_TIMEOUT | 15,000 ms | Expire the connection if no traffic arrives within the timeout after PING |

Although configured here, heartbeat behavior continues after client_connect()
returns; it is not limited to the initial connection attempt.

The sample Gateway sets the same options on ROUTER before zmq_bind(). Configure
the reviewed policy when the sockets are created, before connect/bind. Do not
assume that changing options on a connected socket suspends existing heartbeat
timers; the target libzmq version and any transition protocol would need separate
verification. See the
[ZeroMQ socket-option contract](https://libzmq.readthedocs.io/en/latest/zmq_setsockopt.html).

**Hibernation problem:** freezing the whole application also stops its libzmq
threads. While Gateway keeps running, the frozen client cannot answer its PINGs.
A long enough pause can therefore cause libzmq to close the connection and emit
a real disconnect event. Gateway's event-based cleanup can then invalidate the
old session and buffers. This can happen with no pending application request;
removing an RPC heartbeat or watchdog does not prevent the transport timeout.
The sample's transport timeout is distinct from our selected 15-second local
connect/request deadlines.

**Responsibility boundary:** Gateway serves client requests and owns their
connection resources. It does not supervise application health or probe idle
apps for responsiveness. App health assessment and stop/restart decisions belong
to platform app management. With no outstanding request, Gateway expects no
application reply; silence alone is not a failure. Gateway must still protect
other clients through bounded resources, fair scheduling and nonblocking sends.

**Possible argument for transport heartbeats:** an idle, completely hung process
can retain its open socket without producing a disconnect event. If its libzmq
threads also stop making progress, heartbeats can expire that silent connection
and initiate safe resource cleanup. This would provide a connection-expiry
policy, not an application-health guarantee. Application code can hang while
libzmq continues handling PING/PONG, and a deliberate whole-process freeze can
produce the same transport silence as a complete hang. Heartbeat processing is
internal to the [libzmq transport engine](https://github.com/zeromq/libzmq/blob/v4.3.5/src/zmtp_engine.cpp),
independent of an application-level health response.

**TODO(ZMQ-HEARTBEAT):** architects must choose the policy for app-facing UDS
connections. The proposed baseline for preserving an idle frozen connection is
ZMQ_HEARTBEAT_IVL=0 on both DEALER and ROUTER, with no peer heartbeat TTL requested.
This is a proposal pending confirmation, not an approved architectural decision.
If transport heartbeats are required, the platform needs an agreed hibernation
and reconnect/resource-recovery contract; the document cannot promise that the
same live session survives the pause. A longer finite timeout alone does not
cover an unbounded freeze.

**Architect question:** does Gateway need to detect and expire the connection
of an idle, completely hung process within a bounded time to reclaim resources,
or is retaining bounded connection state until actual disconnect or an agreed
platform lifecycle event sufficient? App health monitoring is outside Gateway's
scope. If bounded transport-silence detection is required, confirm why and how
intentional hibernation is accommodated. Otherwise, confirm the proposed
heartbeat-disabled policy. Application-level heartbeat and inactivity-watchdog
logic stay removed in either case; the PAIR monitor remains active.

#### Connection sequence

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_connect
    participant C as Startup coordinator / result
    participant IO as Client I/O owner and PAIR receiver
    participant Z as Client libzmq / DEALER
    participant GW as Gateway libzmq / ROUTER
    participant G as Gateway ZAP handler
    A->>API: client_connect(out Client, connectionHandler)
    API->>API: Validate output and prepare local runtime
    alt Output pointer is NULL
        API-->>A: CLIENT_INVALID_ARGUMENT
    else Initial memory allocation fails
        API->>API: Release partial allocations, keep out Client NULL
        API-->>A: CLIENT_OUT_OF_MEMORY, out Client NULL
    else Local runtime allocated
        API->>C: Set PENDING and monotonic connect deadline at start plus 15 seconds
        API->>IO: Start owned serving thread
        API->>C: Wait until startup result is terminal or deadline expires
        Note over API,IO: Caller waits, I/O owner continues running
        IO->>Z: Create context and DEALER
        IO->>Z: Set PLAIN credentials, bounds and finite linger
        IO->>Z: Apply transport heartbeat options before connect
        Note over IO,GW: Proposed heartbeats off on both ends, architect decision ZMQ-HEARTBEAT pending
        IO->>Z: Enable DEALER monitoring at the unique inproc endpoint
        IO->>Z: Create PAIR receiver and connect to that monitor endpoint
        opt Local setup succeeded
            IO->>Z: zmq_connect(ipc:///tmp/app-eyil-gateway.ipc)
        end
        alt Local setup or synchronous zmq_connect call fails
            IO->>C: Publish FAILED with CLIENT_OUT_OF_MEMORY or CLIENT_COMMUNICATION_ERROR
            C->>C: Notify waiting caller immediately
        else Setup succeeds and connection attempt starts
            opt Gateway reachable and handshake progresses
                Z->>GW: UDS connection and PLAIN handshake
                GW->>G: ZAP request with kernel-derived peer credentials
                G-->>GW: Status 200 and User-Id, or failure status
                GW-->>Z: Handshake result
            end
            Note over IO,Z: CONNECTED, CONNECT_DELAYED and CONNECT_RETRIED do not establish readiness
            alt ZMQ_EVENT_HANDSHAKE_SUCCEEDED before deadline
                Z-->>IO: Handshake success through monitor PAIR
                IO->>C: Publish READY with CLIENT_OK and notify
            else ZMQ_EVENT_HANDSHAKE_FAILED_AUTH with ZAP status 400
                Z-->>IO: Explicit authentication rejection
                IO->>C: Publish FAILED with CLIENT_AUTH_FAILED and notify
            else Terminal protocol, transport or authentication-service failure
                Z-->>IO: Failure event, exact mapping in table below
                IO->>C: Publish FAILED with CLIENT_COMMUNICATION_ERROR and notify
            else Connect deadline expires while still PENDING
                C->>C: Publish TIMED_OUT with CLIENT_TIMEOUT
                C->>IO: Wake and abort pending connection attempt
            end
        end
        C-->>API: One terminal ClientResult
        alt Result is CLIENT_OK
            API->>API: Publish initialized Client handle
            API-->>A: CLIENT_OK, valid Client
        else Result is an error
            API->>IO: Request stop, owner closes any created sockets
            IO-->>API: Serving loop exits
            API->>API: Join started threads, terminate context, free runtime
            API-->>A: Exact ClientResult, out Client NULL
        end
    end
```

A serving-thread startup failure is handled by the caller using the same
failure/rollback path; cleanup only stops or joins threads that were actually
started.

#### Exact initial-connect outcomes

| Condition / monitor event | Startup action | client_connect result |
| --- | --- | --- |
| Output argument `Client **client` is NULL | Reject before creating runtime state | CLIENT_INVALID_ARGUMENT |
| Client/runtime allocation fails, including an explicit ENOMEM from local setup | Stop/roll back any initialized resources | CLIENT_OUT_OF_MEMORY, output NULL |
| Non-memory context/socket/monitor/thread setup failure, or synchronous zmq_connect error | Publish FAILED and stop the attempt | CLIENT_COMMUNICATION_ERROR, output NULL |
| ZMQ_EVENT_CONNECTED | Keep PENDING; transport connection alone is insufficient | No return yet |
| ZMQ_EVENT_CONNECT_DELAYED or ZMQ_EVENT_CONNECT_RETRIED | Keep PENDING within the original deadline | No return yet |
| ZMQ_EVENT_HANDSHAKE_SUCCEEDED while PENDING and before the deadline | Publish READY and wake caller | CLIENT_OK, valid output handle |
| ZMQ_EVENT_HANDSHAKE_FAILED_AUTH with ZAP status 400 | Publish FAILED and wake caller immediately | CLIENT_AUTH_FAILED, output NULL |
| ZMQ_EVENT_HANDSHAKE_FAILED_AUTH with ZAP status 300 or 500 | End this connect attempt as an authentication-service failure | CLIENT_COMMUNICATION_ERROR, output NULL |
| ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL | Publish FAILED and wake caller immediately | CLIENT_COMMUNICATION_ERROR, output NULL |
| ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL | Publish FAILED and wake caller immediately | CLIENT_COMMUNICATION_ERROR, output NULL |
| ZMQ_EVENT_DISCONNECTED before initial readiness | End this connect attempt and wake caller | CLIENT_COMMUNICATION_ERROR, output NULL |
| Monitor/receive processing terminates unexpectedly while startup is PENDING | Publish FAILED and wake caller | CLIENT_COMMUNICATION_ERROR, output NULL |
| Absolute connection deadline expires while startup is PENDING, including an unavailable endpoint with delayed/retried connects | Publish TIMED_OUT; stop and clean up the attempt | CLIENT_TIMEOUT, output NULL |

The monitor's HANDSHAKE_FAILED_AUTH event carries the ZAP status: 400 is an
authentication denial, 300 a temporary authentication-service error and 500 an
internal authentication-service error. This client contract ends the current
connect call on those explicit failures, preserving CLIENT_AUTH_FAILED for a
denial. Event and status definitions:
[ZeroMQ monitor API](https://libzmq.readthedocs.io/en/latest/zmq_socket_monitor.html),
[ZAP specification](https://rfc.zeromq.org/spec/27/).

`zmq_connect()` returning zero and receiving ZMQ_EVENT_CONNECTED are both
insufficient for CLIENT_OK. Successful handshake detection is the agreed
readiness barrier. No Crypto request or AppManager availability probe is needed
to establish it.

The startup coordinator stores a terminal result, rather than only a
`connected` boolean. The wait predicate is `startupState != PENDING` with an
absolute monotonic deadline. Success and explicit failures publish the result
and notify the waiter; the timeout path publishes CLIENT_TIMEOUT. Synchronize
these transitions so one attempt has one final outcome. A late handshake event
cannot revive a timed-out or failed attempt, and retries do not restart its
deadline. Reconnect behavior after a previously successful client_connect
remains a separate lifecycle decision.

On every failure, keep a provided output handle NULL, close created sockets on
their owner, stop/join started threads, terminate the owned context and free the
incomplete handle before returning. The deadline bounds the readiness wait;
safe rollback still has to finish. Do not expose an incomplete handle through
application callbacks on a failed attempt. Initial connection-callback timing
around successful handle publication, including whether the first handshake
emits RESTORED, remains CLIENT-CALLBACKS; it does not change the agreed readiness
or error return contract. The current skeleton still returns
CLIENT_NOT_IMPLEMENTED until this connection flow is implemented.

### 5.6 client_send_request() — not implemented in the skeleton

Coverage: **AC2.1, AC2.2, AC2.4**; Crypto use also supports **AC3.1–AC3.2**.

The caller supplies one serialized RpcRequest containing the service and its
opaque payload. The library parses that envelope, replaces request_id and
schema_version, and serializes the outgoing request. It validates the returning
RpcResponse and gives the caller its complete serialized bytes. It does not
parse CryptoRequest or CryptoResponse. No additional service argument or byte
API signature change is needed for this agreed envelope handling.

#### Submission and synchronous wait

```mermaid
sequenceDiagram
    participant A as Calling app thread
    participant API as client_send_request
    participant C as Client coordinator
    participant IO as I/O owner / pending map
    participant W as Per-request waiter
    participant G as Gateway ROUTER
    A->>API: Binary request, out response, out size
    API->>API: Clear supplied outputs, validate arguments and calling thread
    opt Arguments valid and current thread may block
        API->>API: Parse RpcRequest into owned storage, validate envelope limits
    end
    alt Invalid arguments, envelope or blocking call on own I/O thread
        API-->>A: CLIENT_INVALID_ARGUMENT, NULL response and size 0
    else Request preparation allocation fails
        API-->>A: CLIENT_OUT_OF_MEMORY, NULL response and size 0
    else Valid prepared request
        API->>C: Retain live call, check readiness and reserve one of 64 request slots
        alt No usable connection
            C->>C: Release temporary request state and call reference
            C-->>API: CLIENT_NOT_CONNECTED
            API-->>A: CLIENT_NOT_CONNECTED, NULL response and size 0
        else Admission or request-state allocation fails
            C->>C: Release temporary request state and call reference
            C-->>API: Local admission error or CLIENT_OUT_OF_MEMORY
            API-->>A: Error, NULL response and size 0
        else Accepted
            C->>W: Retain waiter, generation and admission time plus 15-second deadline
            C->>IO: Queue owned request and wake
            API->>W: Wait for terminal result, no coordinator lock held
            IO->>IO: Check generation/deadline, assign ID and schema 1
            IO->>IO: Serialize request and install pending entry before send
            IO->>G: zmq_send(DEALER, one RpcRequest frame, DONTWAIT) over UDS
            Note over IO,G: Shared I/O flow below handles backpressure and concurrent requests
            alt Valid correlated response wins completion
                G-->>IO: RpcResponse via zmq_msg_recv
                IO->>IO: Parse and validate envelope, allocate owned response bytes
                IO->>W: Publish CLIENT_OK and owned response, signal
            else Deadline, connection loss or local processing failure wins
                IO->>W: Publish exact local error and NULL response, signal
            end
            W-->>API: One terminal result
            API->>C: Release live call reference
            API-->>A: ClientResult and response outputs
        end
    end
```

Before dereferencing outputs, validate their addresses; clear every supplied
output even if another required argument is missing. Require a non-NULL live
Client, non-NULL input with nonzero bounded length, and both output pointers.
An empty/default envelope or RPC_SERVICE_UNSPECIFIED is invalid. Other numeric
service values can reach Gateway, which determines support. Validate Protobuf
parsing and common message limits without interpreting the service payload.
Caller-supplied request_id and schema_version are ignored and overwritten.

The request record owns its parsed data, completion state and connection
generation. Application input need remain valid only for the API call. Reserve
count/byte capacity and completion bookkeeping before acceptance. For async
calls the same preparation completes before returning CLIENT_OK. Neither API
may leave a queued pointer into temporary caller storage.

#### Shared nonblocking send and receive pipeline

This pipeline serves all four request APIs. Socket polling, send/receive,
pending-map mutation and monitor processing belong to the I/O owner. The
following send and receive paths run as event-loop work; they do not impose a
one-request-at-a-time send/receive cycle.

```mermaid
sequenceDiagram
    participant Q as Submission queue
    participant IO as Client I/O owner
    participant P as Pending requests
    participant G as Gateway ROUTER over UDS
    participant C as Waiter or request callback
    Q-->>IO: Owned request, completion target, generation and deadline
    alt Request already terminal, expired or generation no longer usable
        IO->>IO: Skip unsent work, settle any remaining local completion
        Note over IO,C: Expired uses CLIENT_TIMEOUT, lost generation uses CLIENT_COMMUNICATION_ERROR
    else Request can proceed
        IO->>IO: Set unique nonzero request_id and schema_version 1
        IO->>IO: Serialize RpcRequest, keep service payload opaque
        IO->>P: Register request before first send attempt
        IO->>G: zmq_send(DEALER, one frame, ZMQ_DONTWAIT)
        alt Message accepted by libzmq
            IO->>P: Mark SENT, retain completion obligation
        else EAGAIN, temporarily unable to queue
            IO->>P: Keep unsent bytes and original deadline
            Note over IO,G: Poll for writable socket while receiving replies and monitor events
            opt Still pending, connected and within deadline when writable
                IO->>G: Retry only this not-yet-accepted send
            end
        else Terminal send error
            IO->>P: Complete affected request with mapped local error
        end
    end
    Note over IO,C: Other submissions and completions continue independently
    G-->>IO: Socket readable with a response
    IO->>IO: zmq_msg_recv(DONTWAIT), check frame count and message size
    alt Wrong framing, excessive size, unparseable response or zero request_id
        IO->>P: Mark current connection unusable, fail its pending work
        P-->>C: CLIENT_COMMUNICATION_ERROR, no response
    else Parsed outer response
        IO->>P: Match request_id in the current generation
        alt Unknown, retired or duplicate nonzero ID
            IO->>IO: Discard response without completing another request
        else Matching request
            IO->>IO: Verify expected service, schema 1 and known nonzero RpcStatus
            alt Invalid response metadata
                IO->>P: Complete matching request with CLIENT_COMMUNICATION_ERROR
                P-->>C: Error and no response
            else Valid response
                IO->>IO: Allocate independent response storage and copy received bytes
                alt Allocation fails
                    IO->>P: Complete matching request with CLIENT_OUT_OF_MEMORY
                    P-->>C: Error and no response
                else Storage ready
                    IO->>P: Publish CLIENT_OK and response through one terminal transition
                    P-->>C: Owned serialized RpcResponse, including any Gateway error status
                end
            end
        end
    end
```

Implementation rules for that pipeline:

- Serialize only after assigning the library-owned ID and schema. Install the
  pending target before attempting to send, so a fast response is correlatable.
  Use one DEALER body frame without a REQ-style empty delimiter.
- A successful zmq_send means that libzmq accepted the message into its queue.
  It is not proof of delivery or execution. Do not submit that request again.
  EAGAIN means no acceptance: retain it within the original byte/count budget
  and retry on writable readiness without extending its deadline. EINTR permits
  retry of the interrupted call while the request is still pending. Other
  terminal socket failures map to CLIENT_COMMUNICATION_ERROR; an explicit
  local allocation failure maps to CLIENT_OUT_OF_MEMORY. See
  [zmq_send](https://libzmq.readthedocs.io/en/latest/zmq_send.html).
- Receive with zmq_msg_recv and check ZMQ_RCVMORE. EAGAIN means there is currently
  nothing to read; EINTR permits retry. Neither is itself a request failure.
  Drain/discard a malformed multipart message without treating its remaining
  frames as separate replies. See
  [zmq_msg_recv](https://libzmq.readthedocs.io/en/latest/zmq_msg_recv.html).
- A response with no trustworthy envelope or with request_id zero cannot be
  attributed to one waiter. The proposed client policy marks that connection
  unusable and settles its outstanding work with CLIENT_COMMUNICATION_ERROR.
  A fully parsed nonzero ID absent from the pending map is discarded; it may
  be a late reply. It is never a notification or another request's result.
- For a matched ID, require the expected service, schema_version 1 and a known
  RpcStatus other than UNSPECIFIED. Invalid metadata fails that matched call
  with CLIENT_COMMUNICATION_ERROR. Leave CryptoResponse validation to the
  service consumer. A valid RPC_STATUS_QUEUE_FULL or another Gateway error is
  delivered with CLIENT_OK, preserving the remote status and payload.
- Copy a successful binary response into storage compatible with
  client_free_response before closing the zmq message. Return the validated
  complete serialized RpcResponse, not just its payload. Release all temporary
  Protobuf/ZMQ storage and retire request accounting once ownership is settled.

The malformed-response scope above is a concrete client implementation proposal;
it does not add a wire status or claim an architect decision. Lifecycle policy
for reconnecting that unusable connection remains separate.

#### Timeout, disconnect and response races

```mermaid
sequenceDiagram
    participant A as Synchronous caller / waiter
    participant R as Retained request completion state
    participant IO as I/O owner / pending map
    participant G as Gateway
    alt Response is validated and wins terminal transition before deadline
        G-->>IO: RpcResponse
        IO->>R: TryComplete(CLIENT_OK, owned response)
        R-->>A: Wake with successful local delivery
    else Request deadline expires first
        A->>R: TryComplete(CLIENT_TIMEOUT, NULL, 0)
        A->>IO: Wake for owner-side retirement
        R-->>A: CLIENT_TIMEOUT, NULL response and size 0
    else Connection is lost first
        IO->>R: TryComplete(CLIENT_COMMUNICATION_ERROR, NULL, 0)
        R-->>A: Wake with CLIENT_COMMUNICATION_ERROR
    end
    IO->>IO: Retire pending/unsent state and accounting exactly once
    opt Late or duplicate response arrives
        G-->>IO: RpcResponse for retired request ID
        IO->>IO: Discard, no second completion and no ID reassignment
    end
```

The waiter and I/O owner share a retained completion object, not a pointer to
the caller's stack. TryComplete publishes the result and its response ownership
atomically with respect to other terminal paths. If a prepared response loses
that race, its storage is freed internally. The I/O owner alone removes pending
entries; a caller-side timeout marks completion and wakes that owner. The queued
record stays alive until owner-side retirement, even if the caller has returned.
Async deadlines use the same transition on the I/O thread and deliver a typed
callback. A timed-out request that has not yet been handed to libzmq is skipped.
Check the absolute deadline when claiming a response as well as when processing
timers, so a delayed timer cannot turn an expired request into a success.

On every local failure, return NULL response and size zero. A timeout ends the
local wait; it does not cancel remote work, release remote buffers or authorize
replay. A valid response containing a non-OK RpcStatus still returns CLIENT_OK;
the caller inspects that status and eventually calls client_free_response.

There is no unresolved envelope-processing step in this flow. The 15-second
deadline and 64-request count limit are selected. Remaining client decisions are
the local queue-full ClientResult, byte-size budgets and explicit close/reconnect
behavior. They are recorded under CLIENT-ADMISSION, RESOURCE-LIMITS,
CLIENT-SHUTDOWN and CONNECTION-LIFETIME in section 7.7. The stale public-header
description and missing implementation are implementation work, not gaps in this
byte API's intended envelope handling.

### 5.7 client_send_request_async() — CLIENT_NOT_IMPLEMENTED in the header

Coverage: **AC2.1, AC2.2, AC2.4**.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_send_request_async
    participant IO as Serving thread / pending map
    participant Z as DEALER
    participant G as Gateway ROUTER
    A->>API: Request bytes, response handler and user_data
    API->>API: Validate arguments and handler, parse owned outer RpcRequest
    API->>API: Check readiness, reserve bounded request and completion state
    alt Submission rejected
        API-->>A: Non-OK ClientResult
        Note over A,IO: No response callback for a rejected submission
    else Submission accepted
        API->>IO: Enqueue request plus handler/user_data and wake
        API-->>A: CLIENT_OK, one completion is owed
        IO->>IO: Check generation/deadline, assign ID and schema 1
        IO->>IO: Serialize RpcRequest, register pending completion before send
        IO->>Z: zmq_send(one RpcRequest frame, DONTWAIT)
        Z->>G: IPC request
        Note over IO,G: EAGAIN retains unsent work, same shared pipeline as synchronous requests
        alt Valid correlated response delivered
            G-->>Z: IPC RpcResponse, possibly out of order
            IO->>Z: zmq_msg_recv(DONTWAIT)
            IO->>IO: Check framing, parse RpcResponse and validate ID/service/schema/status
            IO->>IO: Allocate owned response, win completion and retire pending entry
            IO->>IO: Release internal locks before calling application code
            IO-->>A: handler(Client, CLIENT_OK, response, size, user_data)
            A->>API: Later client_free_response(response)
        else Local failure before successful completion
            IO->>IO: Observe deadline, connection loss or local error
            IO->>IO: Retire pending entry once, ignore later reply
            IO->>IO: Release internal locks before calling application code
            IO-->>A: handler(Client, error, NULL, 0, user_data)
        end
    end
    Note over A,IO: Handler runs on I/O owner, never inline on the submission stack
```

The header explicitly documents CLIENT_NOT_IMPLEMENTED for this method. Its
target callback carries ClientResult and per-request user_data, as specified in
section 5.4; those fields still need to be added to the header and implementation.
They are not unresolved error-behavior questions.

Immediate rejection invokes no callback: invalid arguments/envelope/handler use
CLIENT_INVALID_ARGUMENT, no usable connection uses CLIENT_NOT_CONNECTED, and
allocation failure uses CLIENT_OUT_OF_MEMORY. The bounded-queue-full return
value is the separate CLIENT-ADMISSION decision. Once accepted, deadline expiry
uses CLIENT_TIMEOUT, connection loss or invalid response uses
CLIENT_COMMUNICATION_ERROR, and allocation failure uses CLIENT_OUT_OF_MEMORY.
Each invokes the handler once with NULL, size zero and the original user_data.
Never fabricate a Gateway response for a local error.

All request callbacks for this Client run serially on its I/O owner after
internal locks are released. The handler may free its response or enqueue
another async request, but must not wait synchronously or disconnect there.
It may run before the submitting application thread returns. Section 5.4 defines
this target execution model; section 5.6 defines the shared send/receive,
correlation and terminal-race behavior.

### 5.8 client_send_request_string() — not implemented in the skeleton

Coverage: **AC2.1, AC2.4**.

**Current API status:** both string methods are declared, but valid calls return
CLIENT_NOT_IMPLEMENTED in EyilClient.cpp. The async method is also explicitly
marked that way in the header. There is no implemented text codec in this
repository. Keep string methods unsupported until a format is selected; the
diagrams describe their eventual target behavior.

The string API requires a lossless text representation of the outer RPC
envelope. Its target flow is a conversion wrapper around the same binary
request core. The diagrams below show that complete flow; selection of the text
format is a genuine contract question, described after the first diagram.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_send_request_string
    participant CODEC as String/envelope conversion
    participant B as Shared binary request core
    participant IO as Client I/O owner
    participant G as Gateway ROUTER
    A->>API: NUL-terminated request and out response
    API->>API: Clear supplied output, validate pointers and bounded string length
    opt Arguments valid
        API->>CODEC: Decode text to serialized RpcRequest with explicit byte length
    end
    alt Invalid arguments, text or envelope
        API-->>A: CLIENT_INVALID_ARGUMENT, NULL response
    else Decode/preparation allocation fails
        API-->>A: CLIENT_OUT_OF_MEMORY, NULL response
    else Request prepared
        API->>B: Submit synchronous bytes through the common request path
        B->>B: Parse/validate outer envelope, check readiness and admission
        alt Common request rejects before enqueue
            B-->>API: Exact local rejection, no response
            API->>API: Free temporary request storage
            API-->>A: Same ClientResult, NULL response
        else Request accepted
            B->>IO: Queue owned request and waiter, wake
            IO->>IO: Assign ID/schema 1, serialize and register pending request
            IO->>G: zmq_send(DEALER, one binary RpcRequest frame, DONTWAIT) over UDS
            alt Valid response completes the binary request
                G-->>IO: RpcResponse via zmq_msg_recv(DONTWAIT)
                IO->>IO: Validate framing and envelope, correlate ID, copy response
                IO-->>B: Complete waiter with CLIENT_OK and owned response bytes
                B-->>API: CLIENT_OK, serialized RpcResponse and byte length
                API->>CODEC: Encode whole response to bounded NUL-terminated text
                alt Encoding and allocation succeed
                    CODEC-->>API: Owned response string
                    API->>API: Free temporary binary response and request storage
                    API-->>A: CLIENT_OK and owned response string
                else Response-string allocation fails
                    CODEC-->>API: CLIENT_OUT_OF_MEMORY
                    API->>API: Free temporary binary response and request storage
                    API-->>A: CLIENT_OUT_OF_MEMORY, NULL response
                else Encoded response exceeds configured bound
                    CODEC-->>API: CLIENT_COMMUNICATION_ERROR
                    API->>API: Free temporary binary response and request storage
                    API-->>A: CLIENT_COMMUNICATION_ERROR, NULL response
                end
            else Deadline, transport or response-processing failure
                IO-->>B: Exact local error and no response
                B-->>API: Same local error
                API->>API: Free temporary request storage
                API-->>A: Same ClientResult, NULL response
            end
        end
    end
```

#### Why the string format still needs a decision

The architect-provided sample's client_send_request_string applies strlen() to
request_data, passes those bytes directly to the byte API, then copies the
binary response and appends a NUL. It contains no text encoder/decoder. Its
header only promises NUL-terminated strings; it does not specify their format.

Serialized Protobuf can contain zero bytes, including inside the opaque
payload. strlen() would truncate such an input, and appending a terminator to a
binary response would not make it an unambiguous C string. The target wrapper
therefore needs a format decision, not merely removal of an implementation TODO.
See the [Protobuf wire format](https://protobuf.dev/programming-guides/encoding/).

| Candidate string contract | Input/output conversion | Consequence |
| --- | --- | --- |
| Base64 of the complete serialized RpcRequest/RpcResponse | Decode to binary before the shared core; encode validated response bytes afterward | Simple lossless binary wrapper, including unknown wire fields; text is not field-readable |
| Protobuf JSON mapping of the outer RpcRequest/RpcResponse | Parse/print using the Protobuf JSON mapping; retain service payload as bytes | Field-readable outer envelope; bytes fields use base64, with ProtoJSON's compatibility/unknown-field limits |

The [ProtoJSON specification](https://protobuf.dev/programming-guides/json/)
defines the second option. Keep **both options as proposals for a later
decision**; neither is selected. Base64 is the smaller implementation around the
byte API, while ProtoJSON makes the outer fields readable. Confirm the format
with the API consumer under **TODO(STRING-CODEC)**, including text and
decoded-message limits. Do not select a format by guessing from the input. Both
alternatives keep binary Protobuf over ZMQ and keep service payload interpretation
outside EyilClient.

Conversion ownership and failures are already defined: validate and decode input
before submission, own all intermediate storage, and return a separately owned
NUL-terminated response for client_free_response. Check size arithmetic and the
configured decoded/encoded limits before allocation. Invalid/oversized input
returns CLIENT_INVALID_ARGUMENT; allocation failure returns CLIENT_OUT_OF_MEMORY.
All temporary storage is released on every success/rejection/error path. A valid
received response must fit the agreed encoded-response bound; a response that
cannot be represented within that bound is a local CLIENT_COMMUNICATION_ERROR.

CLIENT_TIMEOUT and transport errors propagate unchanged from the shared binary
core. Failure to allocate the final text can occur after remote execution and
successful receipt of its response: return CLIENT_OUT_OF_MEMORY and NULL, and
do not replay the operation. Format-independent error/ownership behavior is not
an outstanding design question. The method is currently a skeleton placeholder,
as indicated in its heading; that is separate from choosing the string format.

### 5.9 client_send_request_string_async() — CLIENT_NOT_IMPLEMENTED in the header

Coverage: **AC2.1, AC2.4**.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_send_request_string_async
    participant CODEC as String/envelope conversion
    participant IO as Shared request coordinator / I/O owner
    participant G as Gateway ROUTER
    A->>API: NUL-terminated request, string handler and user_data
    API->>API: Validate Client, input, string bound and handler
    API->>CODEC: Decode selected text format to owned RpcRequest
    CODEC-->>API: Parsed outer envelope or local error
    API->>API: Check readiness, reserve bounded request and completion state
    alt Input conversion or submission rejected
        API-->>A: Non-OK ClientResult, no callback
    else Submission accepted
        API->>IO: Enqueue request plus handler/user_data and wake
        API-->>A: CLIENT_OK, one completion is owed
        IO->>IO: Check generation/deadline, assign ID and schema 1
        IO->>IO: Serialize RpcRequest and install pending completion before send
        IO->>G: zmq_send(DEALER, one binary RpcRequest frame, DONTWAIT) over UDS
        Note over IO,G: Same nonblocking send and receive pipeline as byte requests
        alt Valid response and successful text conversion
            G-->>IO: RpcResponse via zmq_msg_recv(DONTWAIT)
            IO->>IO: Check framing, parse envelope and validate ID/service/schema/status
            IO->>CODEC: Encode complete validated response within text size bound
            CODEC-->>IO: Owned NUL-terminated string
            IO->>IO: Retire request once, free temporary storage and release locks
            IO-->>A: handler(Client, CLIENT_OK, response, user_data)
            A->>API: Later client_free_response(response)
        else Local failure before successful completion
            IO->>IO: Resolve timeout, transport, validation or text allocation error
            IO->>IO: Retire request once, free temporary storage and release locks
            IO-->>A: handler(Client, error, NULL, user_data)
        end
    end
    Note over A,IO: String callback runs on the I/O owner, never inline on submission
```

Both string APIs use the same coordinator and pending map as byte APIs.
They do not create a second DEALER, use arrival-order matching or implement
their own synchronous receive loop. Async strings use the same rejection and
exactly-once completion rules, with no size parameter in the string callback.

The header explicitly marks this method CLIENT_NOT_IMPLEMENTED. Its target
error mapping and callback execution are the same as for binary async requests:
CLIENT_INVALID_ARGUMENT for invalid input, CLIENT_NOT_CONNECTED before admission,
CLIENT_TIMEOUT on deadline expiry, CLIENT_COMMUNICATION_ERROR for communication
or invalid-response failures, and CLIENT_OUT_OF_MEMORY for allocation failures.
Pre-admission errors return directly without a callback. Post-admission errors
invoke the handler once with NULL and the original user_data, including a
failure to allocate the response string. The actual text format is the same
STRING-CODEC decision as for the synchronous string API, not a second contract.

### 5.10 client_free_response() — implemented

Coverage: **AC2.1**.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_free_response
    participant M as Library-compatible allocator
    A->>API: Response/notification allocation or NULL
    API->>M: free(pointer)
    M-->>API: Storage released, NULL is harmless
    API-->>A: Return
    Note over A,M: Local action only, no ZMQ IPC and no Gateway buffer release
```

This releases a message-result allocation, not a shared-memory object.
Do not pass a mapped buffer, stack memory or a Gateway buffer ID. The header's
notification callback prohibition on other API calls needs clarification for
this freeing function; until clarified, defer freeing outside that callback.

### 5.11 client_register_notifications() — not implemented in the skeleton

Coverage: **AC2.1**; incoming notification protocol remains a TODO.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_register_notifications
    participant C as Coordinator
    participant IO as Serving loop
    A->>API: Client and notification handler
    API->>C: Validate live state, synchronize handler replacement
    C-->>API: Registration result
    API-->>A: ClientResult
    Note over API,IO: Local registration only, no ZMQ subscribe/RPC message
    opt Future notification schema is agreed
        IO->>IO: Receive and classify a defined notification
        IO->>C: Obtain registered handler safely
        IO-->>A: handler(Client, owned notification bytes, size)
    end
```

No notification message exists in the checked-in RPC schema. Do not classify
an unmatched response or request_id zero as a notification without a protocol
decision. Replacement behavior, handler lifetime and failure reporting belong
to TODO(CLIENT-CALLBACKS) and TODO(NOTIFICATIONS).

### 5.12 client_unregister_notifications() — no-op in the skeleton

Coverage: **AC2.1**.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_unregister_notifications
    participant C as Coordinator / callback state
    participant IO as Serving loop
    A->>API: Client
    API->>C: Remove handler from future dispatch
    C->>IO: Synchronize against queued/in-progress delivery
    Note over C,IO: TODO: define drain-or-discard and quiescence guarantee
    IO-->>C: No further use of removed handler
    C-->>API: Unregistration complete
    API-->>A: Return
    Note over A,IO: No ZMQ IPC, application calls this before disconnect
```

The current header requires unregistering notifications before disconnect.
A useful target guarantee is that no notification callback uses the removed
handler after return. Its implementation and behavior when called from a
callback need agreement; self-waiting on the serving thread is invalid.

### 5.13 client_disconnect() — no-op in the skeleton

Coverage: **AC2.1–AC2.3**, leading to **AC1.10** when the Gateway detects closure.

```mermaid
sequenceDiagram
    participant A as Application
    participant API as client_disconnect
    participant C as Coordinator
    participant IO as Serving thread
    participant Z as libzmq / DEALER and monitor
    participant G as Gateway monitor
    A->>API: Client, after unregistering notifications
    API->>C: Stop new API admissions, request shutdown
    C->>IO: Wake, resolve queued/pending local completions
    Note over C,IO: Typed completion is agreed, local-close status/order is TODO
    IO->>Z: Disable monitoring, close owned sockets with finite linger
    Z-->>G: Peer closure eventually becomes monitor event
    IO-->>C: Serving loop exits
    API->>IO: Join thread, also join separate monitor worker if used
    API->>Z: Terminate owned context after all its sockets close
    API->>C: Quiesce callbacks/callers, destroy state
    API-->>A: Return, handle is invalid
    Note over API,G: No disconnect protobuf and no guarantee of remote completion
```

Do not call disconnect from its own callback thread or race destruction against
uncoordinated application calls. A proposed implementation closes admission and
waits for retained in-progress API references; the public concurrency contract
must describe what the application must serialize. Never detach a serving
thread and free its state.

### 5.14 Deadlines, callbacks, reconnect and library unloading

#### Initial timeout and admission defaults

| Setting | Initial value | Meaning |
| --- | --- | --- |
| Common operation timeout | 15,000 ms | One internal duration constant used by connect and request coordination; no public configuration API is required initially |
| Connect deadline | Connect-attempt start plus 15 seconds | Covers setup/handshake readiness waiting; retries cannot restart the deadline, and safe rollback must still finish before return |
| Request deadline | Admission time plus 15 seconds | Covers queueing, nonblocking send attempts and waiting for a response; applies to both synchronous and asynchronous accepted requests |
| Outstanding-request count | 64 per Client | Queued/unsent and sent/awaiting-response requests share this one limit; it is not 64 slots in each stage |

The request timeout is a **single total budget**, not 15 seconds for queueing,
another 15 for sending and another 15 for receiving. Serialization and response
validation occur within that budget. Complete expiry with CLIENT_TIMEOUT through
the existing synchronous or typed async path. A timeout does not close an idle
connection and does not cancel remote work. Successful connect and each later
request use separate deadlines with the same duration.

A dynamic C++ container can implement the queue, but it does not provide a
resource limit. A fast producer or slow Gateway could otherwise keep increasing
the pending map and copied payload storage. Admit up to 64 unfinished requests
per Client and reject additional submissions immediately, without enqueueing or
creating an async callback obligation. Retire an outstanding slot once, as part
of terminal completion; a request callback can then enqueue new work. This is
request-count accounting, not a fixed-size container requirement.

The exact local full-queue result remains **TODO(CLIENT-ADMISSION)**. A proposed
dedicated CLIENT_QUEUE_FULL value would make the reason explicit, but that value
is not present in the header and has not been selected. Do not fabricate the
Gateway's RPC_STATUS_QUEUE_FULL for a client-side rejection.

The count limit does not by itself bound bytes, temporary parsing allocations
or libzmq's internal queues. Select maximum message sizes and byte budgets under
RESOURCE-LIMITS. Gateway must enforce its own per-connection/global limits; it
cannot trust client-library admission, and 64 is not implicitly the Gateway's
global queue limit. A client holding already-returned response allocations owns
their lifetime separately from the pending-request count.

#### Freeze and runtime behavior

The application owns preparation for freezing its process: stop new submissions,
finish accepted operations and callbacks, and leave no uncertain remote work.
With no pending operation, no request timer is active and the 15-second duration
does not expire the idle connection. No custom heartbeat or inactivity watchdog
is part of that flow.

The intended freeze preserves the process and its descriptors. Preservation of
Gateway's live context additionally requires a compatible transport heartbeat
policy. Section 5.5 records the sample's enabled ZeroMQ heartbeat settings and
the pending architect decision; disabled transport heartbeats are the proposed
baseline for this scenario. If libzmq closes the connection during a pause,
normal disconnect handling applies. Section 6.14 describes the Gateway side
and the distinction from process destruction.

#### Concurrency, callbacks and reconnect

The following constraints apply to all request APIs:

- Bounded queues and nonblocking socket operations keep one caller from holding
  the shared connection indefinitely. Use the 64-request limit and explicit
  byte/message limits.
- Use a monotonic clock for the agreed 15-second local deadlines. The current
  API has no timeout argument; future configurability is optional and does not
  block the initial fixed defaults. There is no idle-connection expiry.
- Complete each locally accepted request exactly once with either a successful
  local delivery or a typed local error. Rejected async submissions receive no
  callback. Late replies must not trigger a second callback or satisfy a newly
  allocated request ID. This is a local completion guarantee, not exactly-once
  remote execution.
- Do not retry automatically after a timeout or reconnect. A crypto operation
  may already have changed backend state.
- A transport reconnect is not a promise of the same Gateway context. Buffer IDs
  and key handles from the old session cannot simply be reused.
- Choose libzmq automatic-reconnect and queued-send behavior explicitly. Avoid
  silently carrying old-session work onto a newly authenticated connection.
  Application-level "no retry" alone does not settle what libzmq does with its
  internal send queues. This remains part of TODO(CONNECTION-LIFETIME).
- Keep the transport heartbeat decision consistent on DEALER and ROUTER.
  Automatic reconnect after a heartbeat-driven disconnect does not restore the
  old Gateway context, buffers or key handles. This remains part of
  TODO(ZMQ-HEARTBEAT) and the reconnect contract.
- Connection and notification callbacks are specified by the header to run on
  an internal thread without calling other client APIs. Request callbacks use
  the target execution rules in section 5.4: run on the I/O owner without held
  internal locks, permit async enqueue/free, and prohibit blocking calls or
  disconnect from that handler. Callbacks must hand heavy work to the app.
- If arbitrary blocking callbacks must be supported, add a separate bounded
  completion executor with its own shutdown contract. Do not silently create
  an unbounded thread per callback.
- An application-owned event-loop mode would need additional public integration
  APIs. It is an optional future extension, not a requirement for this version.

#### Library unloading

A shared library can create threads. Runtime unloading is safe only after the
application disconnects every Client, all library-owned threads are joined,
all calls/callbacks have returned, owned ZMQ contexts are terminated and all
buffers requiring `client_free_response()` have been freed. No thread may still
execute library code after `dlclose()`. libzmq's internal threads do not remove
this obligation. See the
[dynamic loader API](https://man7.org/linux/man-pages/man3/dlopen.3.html).

### 5.15 Planned call hierarchy

```text
Application / Firebolt Crypto
  client_connect
    ClientCoordinator::Start
      IoLoop::Run
        Configure DEALER options, including pending transport heartbeat policy
        Enable monitor and connect PAIR receiver -> zmq_connect
        Poll PAIR -> handshake result -> complete startup waiter

  client_send_request[_async]
  client_send_request_string[_async]
    C API validation -> optional string decoding
      EnvelopeCodec::ParseRequest -> owned RpcRequest and opaque service payload
      ClientCoordinator::Submit -> bounded queue -> wakeup
        IoLoop::DrainSubmissions
          CheckGenerationAndDeadline
          EnvelopeCodec::SerializeRequest(schema=1, unique request_id)
          PendingRequests::Insert
          zmq_send(DEALER, DONTWAIT) -> retain unsent work on EAGAIN
        IoLoop::Receive
          zmq_msg_recv -> framing/size check -> parse RpcResponse
            match ID/generation -> validate service/schema/status -> own response
              TryCompleteOnce -> retire pending state
                synchronous waiter OR callback on I/O owner, no internal locks
                  optional string encoding -> ClientResult, response, user_data
        Deadline or connection loss
          TryCompleteOnce -> retire pending state -> typed local error, no replay

  Monitor processing on the same I/O owner
    Poll PAIR -> decode transport event -> connection-state transition
      notify startup waiter / connection handler / affected pending requests

  client_register_notifications / client_unregister_notifications
    CallbackRegistry -> synchronization; no current wire subscription

  client_free_response -> free
  client_disconnect
    CloseAdmission -> wake/settle -> owner closes sockets
      join threads -> terminate context -> release handle
```

The call hierarchy contains no application heartbeat producer, heartbeat
service dispatch or inactivity-watchdog task. Pending lifecycle/API choices are
kept in sections 5.5 and 7.7; implementation-status labels in method headings do
not change the target request flows.

## 6. EyilGateway design and flows

### 6.1 Plugin and implementation organization

The initial implementation is **in process** with Thunder. The existing thin
`EyilGateway` wrapper implements IPlugin and acquires
`EyilGatewayImplementation` using
`Root<Exchange::IConfiguration>()`, then calls `Configure(IShell*)`.
The default `configuration.root.mode` is `Off`. A future `Local` mode can use
the same split with matching framework proxy/stubs.

**There is no app-facing Gateway COM-RPC or JSON-RPC API.** IConfiguration is
the internal Thunder bootstrap contract. Future ICrypto calls form a separate
trusted backend boundary. "No public Gateway COM-RPC API" does not mean there
are no framework/backend interfaces internally.

| Planned unit | Responsibility | Separation rule |
| --- | --- | --- |
| EyilGateway wrapper | Register framework notifications, acquire/configure implementation, release it | Keep transport and service logic out of IPlugin |
| EyilGatewayImplementation | Construct and stop the runtime; own configuration/resources | Internal IConfiguration; define explicit safe teardown |
| GatewayServer / IoLoop | Own ROUTER, receive messages, route completions | No blocking backend work; no worker uses ROUTER |
| ZapAuthenticator | Own REP, parse credentials, answer ZAP | No application-supplied identity is trusted |
| AuthenticationProvider | Interim acceptance; future authoritative lookup | TODO(AUTH-APPMANAGER), separate from transport |
| ConnectionRegistry | Map User-Id to retained context; serialize admission/closing | Never key ownership solely by UID/PID/routing ID |
| RequestDispatcher | Envelope/service validation and routing | Service-neutral at outer boundary |
| RequestJob | One retained dispatch object per accepted request | Immutable input plus explicit context/resource references |
| PrivateRequestPool | Own a Core::ThreadPool and bounded admission | No replacement of Thunder's global worker singleton |
| CryptoServiceAdapter | Decode Crypto operation, validate structure and call backend seam | No Crypto key policy or direct app identity claims |
| SharedBufferManager | Create/track allocations, quotas, leases and release | Gateway-local; no dependency on ICrypto types |
| CompletionQueue | Carry result bytes and original connection generation back to I/O | Bounded; stale completions cannot acquire a new route |
| MonitorCollector | Own PAIR and post normalized transport events | Correlation is a prerequisite for targeted cleanup |
| Optional lifecycle observer | Future app lifecycle events | TODO(LIFECYCLE-EVENTS), no current platform subscription |

There is no application heartbeat service, heartbeat worker or inactivity
watchdog in the Gateway runtime. The registry has no last-request-age expiry
rule. Connection cleanup is driven by correlated disconnect events and the
normal explicit shutdown path. ROUTER's built-in ZeroMQ heartbeat settings
remain a separate architect decision, applied at startup as described below.
Gateway owns request isolation and resource accounting; application-health
supervision and app restart decisions remain outside its responsibilities.

These are recommended responsibilities, not a requirement for one source file
per row. Keep explicit module/config/version files, a thin wrapper and separate
implementation sources. Use internal interfaces at unfinished integration points
so including ICrypto or AppManager headers is unnecessary for the first transport
and buffer implementation.

### 6.2 Thread model and private Thunder pool

Coverage: **AC1.2–AC1.4, AC1.7**.

| Execution context | Owns / performs | Must not do |
| --- | --- | --- |
| Thunder framework thread | Initialize/Deinitialize and framework notifications | Run application requests synchronously |
| Gateway I/O owner | ROUTER, bounded parsing/admission, event/completion draining | Wait for Crypto, a full worker queue or a slow client |
| ZAP worker | REP and authentication exchange | Use ROUTER or modify another thread's socket |
| Monitor worker | PAIR events, normalized disconnect records | Guess a session from a non-unique PID |
| Private Thunder workers | RequestJob::Dispatch, buffer operations and future backend calls | Send a ROUTER reply or own transport state |
| libzmq internal threads | Transport machinery | Replace Gateway request/session coordination |

Create a Gateway-owned `Core::ThreadPool` instance with an explicit worker
count, stack budget and bounded queue. Submit a distinct `Core::IDispatch`
object for each admitted request. Do not call `Core::IWorkerPool::Assign()`
or use `Core::IWorkerPool::Instance().Submit()` for application request work.
Some Thunder helper job classes submit implicitly to that global instance;
select the explicit private-pool path.

The local Thunder R4.4 implementation supports independently owned thread pools.
Its submission APIs must be wrapped carefully: not every version returns an
enqueue-success result, and infinite submission waits would block the ROUTER.
Use explicit admission accounting and a version-appropriate nonblocking/bounded
submission path. Pool sizing and behavior must be validated against the target
Thunder release. Reference:
[Thunder pool implementation](https://github.com/rdkcentral/Thunder/blob/2c0fcc5529e7da734be558ca6efa05d934dcce31/Source/core/ThreadPool.h).

The wrapper's existing use of the global pool for a framework deactivation job
can remain. It is separate from the new application request pool.

### 6.3 Runtime records and synchronization

A planned connection context contains:

| Field group | Required information |
| --- | --- |
| Identity | Unique User-Id, connection token/generation, interim appId and sessionId |
| Peer | Trusted host UID/GID/PID; process-instance evidence if available |
| Routing | Binary ROUTER routing ID plus a proven binding to this connection generation |
| Admission | State, API entitlements placeholder, in-flight counts and byte budgets |
| Backend | Not integrated / not initialized / initializing / ready / failed / draining |
| Resources | Owned buffer IDs, queued/active leases, completion accounting |
| Lifecycle | Correlated closing reason and exactly-once cleanup/notification state |

Preserve runtime app-instance identity separately when a future platform API
supplies it. The interim per-connection sessionId is not evidence of a real
launcher-provided app instance. Never derive uniqueness from UID, PID or a
reusable numeric FD alone.

Each RequestJob retains an immutable request plus `User-Id`, connection
generation, request_id, expected service and the required resource references.
A completion carries the same identity tuple; workers do not look up "whatever
connection now has this routing ID."

Proposed synchronization rules:

- The I/O owner serializes admission, route publication and terminal connection
  transitions. ZAP/monitor workers submit bounded control events.
- ZAP must publish its context before returning acceptance; an acknowledgement
  or a protected registry insertion is needed so the first request cannot race
  ahead of its record. No backend call runs under the registry lock.
- Buffer records use retained leases and synchronized ownership/quota accounting.
  A release removes admission to that allocation before physical destruction.
- Backend initialization happens once per context. Other requests wait through
  an ordered continuation/barrier, not by exhausting all pool threads while an
  initialization job waits behind them.
- A session removed from public lookup may remain privately retained by active
  jobs. Purge entitlement/admission visibility immediately; retain only what
  those jobs and cleanup need.

These rules define invariants. The exact locking/owner-thread implementation
and dependency ordering remain **Gap 5**, not an assertion that arbitrary
concurrent access to these records is safe.

### 6.4 Startup and partial-start failure

Coverage: **AC1.1–AC1.4, AC1.7**.

```mermaid
sequenceDiagram
    participant T as Thunder
    participant P as EyilGateway wrapper
    participant I as EyilGatewayImplementation
    participant W as Private pool
    participant Z as ZAP worker
    participant M as Monitor worker
    participant R as ROUTER owner
    T->>P: Initialize(IShell)
    P->>T: AddRef shell, Register notification
    P->>T: Root(IConfiguration, EyilGatewayImplementation)
    T-->>P: Implementation interface
    P->>I: Configure(IShell)
    I->>I: Validate configuration, create owned runtime/context
    I->>W: Construct/start private request pool
    I->>Z: Start REP, bind inproc ZAP endpoint
    Z-->>I: Authenticator ready or startup failure
    I->>R: Create ROUTER, set PLAIN server and limits
    R->>R: Apply transport heartbeat options before bind
    Note over I,R: Proposed heartbeats off, same pending ZMQ-HEARTBEAT decision as client setup
    R->>R: Enable Gateway monitor endpoint
    I->>M: Start PAIR collector
    M-->>I: Collector ready
    I->>R: Bind ipc:///tmp/app-eyil-gateway.ipc
    R-->>I: Listener ready or bind failure
    alt All required runtime parts ready
        I-->>P: Configure succeeds
        P-->>T: Initialization succeeds
    else Any startup step fails
        I->>I: Close admission, roll back started parts
        I-->>P: Configure fails
        P->>P: Deinitialize and release framework references
        P-->>T: Initialization error
    end
```

Require ZAP readiness before accepting handshakes. Do not publish a healthy
plugin until its required runtime parts are ready. With ICrypto absent, the
transport can be ready while Crypto is unavailable; the chosen degraded-start
policy is a TODO. AppManager/RuntimeManager lookup is not called in this phase.

The sample enables ZMQ_HEARTBEAT_IVL=5000 and ZMQ_HEARTBEAT_TIMEOUT=15000
before binding ROUTER. Do not copy those settings as settled requirements:
Gateway remains running while an application hibernates, so its transport
heartbeat can disconnect that otherwise live app. The paired DEALER/ROUTER
decision is **TODO(ZMQ-HEARTBEAT)** in section 5.5. The proposed setting is off
for connection-preserving freezes, pending architect confirmation. Start no
application heartbeat or inactivity-watchdog workers.

Partial rollback must use the same ownership discipline as normal stop.
The IConfiguration-only wrapper currently has no explicit runtime stop method;
how to request and await full shutdown before release is **Gap 2**.

### 6.5 ZAP, interim identity and future AppManager lookup

Coverage: **AC1.3, AC1.5–AC1.6; AC2.2**.

```mermaid
sequenceDiagram
    participant C as Client DEALER
    participant L as Gateway libzmq
    participant Z as ZAP REP worker
    participant A as AuthenticationProvider
    participant AM as AppManager - future
    participant RM as RuntimeManager - future
    participant REG as ConnectionRegistry
    C->>L: Direct UDS connection, dummy PLAIN credentials
    L->>Z: ZAP request with kernel-derived address credentials
    Z->>Z: Validate ZAP frames/mechanism, parse UID/GID/host PID
    Z->>A: Resolve peer identity
    alt Initial phase - no platform lookup
        A->>A: Accept, generate unique interim appId/sessionId
        A-->>Z: Peer context, all supported APIs allowed
    else Future TODO(AUTH-APPMANAGER)
        A->>AM: Formal UID lookup API - not yet integrated
        AM->>RM: Resolve authoritative app identity/entitlements
        RM-->>AM: Authoritative result
        AM-->>A: Identity and API authorization
        A-->>Z: Allow with context, or deny
    end
    alt Accepted valid peer
        Z->>REG: Publish independent connection token and User-Id
        REG-->>Z: Publication complete
        Z-->>L: ZAP success with unique User-Id
        L-->>C: Authenticated transport
    else Malformed/untrusted peer or future denial
        Z-->>L: ZAP failure, no admitted session
        L-->>C: Handshake failure
    end
```

A User-Id can encode `appId#sessionId#connectionToken`, or be an opaque random
token resolved through the registry. The important property is uniqueness per
accepted connection and an unambiguous lookup; any delimiter-based encoding
needs escaping/validation. Reconnection creates a new generation. Two sockets
from the same UID/GID/PID do not share a context.

The interim "accept all" decision removes policy checks, not frame validation,
credential provenance checks, quotas or buffer isolation. Production API
entitlements are cached per connection and purged from admission on disconnect.
No app-supplied username, password, route or payload field replaces the kernel
identity or the future platform lookup.

### 6.6 Request ingress, dispatch and response delivery

Coverage: **AC1.2, AC1.6–AC1.9; AC2.4**.

```mermaid
sequenceDiagram
    participant C as Client DEALER
    participant R as ROUTER I/O owner
    participant REG as ConnectionRegistry
    participant Q as Admission / private pool
    participant J as RequestJob
    participant S as Service adapter / buffer manager
    participant OUT as Completion queue
    C->>R: Routing frame plus RpcRequest frame
    R->>R: Copy frames and User-Id metadata, check limits
    R->>REG: Resolve live context and verified route generation
    REG-->>R: Retained context or rejection
    R->>R: Validate envelope, schema, service and request ID
    R->>Q: Reserve request and response capacity
    alt Invalid request or no capacity
        R-->>C: Bounded error reply when safely correlatable
    else Admitted
        R->>Q: Submit distinct RequestJob
        Q->>J: Dispatch on private Thunder worker
        J->>J: Validate service payload and operation/oneof
        J->>S: Gateway-local operation or backend seam
        S-->>J: Result or explicit unavailable/error result
        J->>OUT: Response plus original context/generation
        OUT-->>R: Wake completion drain
        R->>REG: Confirm same live route generation
        alt Original connection still safely routable
            R-->>C: zmq_send route and response using nonblocking policy
        else Closed, replaced or ambiguous route
            R->>R: Discard stale delivery, release completion accounting
        end
    end
```

Admission should bound per-connection and global requests, serialized bytes and
expected response capacity. Expensive service work belongs in the job; bound
even envelope parsing so malformed traffic cannot monopolize I/O.

Validate at least: frame count, message size, trusted metadata, live context,
schema 1, supported service, request-ID rules, service parse, operation/oneof
agreement, enum/range validity and allocation ownership. Unknown/duplicate IDs
need an explicit policy. Independent requests can finish in any order;
dependent operations must follow the resource-ordering contract.

A job publishes completion rather than touching ROUTER. Configure deliberate
ROUTER routing/error behavior and use nonblocking sends. A full client queue
must not stall other clients. `ZMQ_ROUTER_MANDATORY` makes unreachable/full-route
conditions observable; it does not authenticate a routing ID or solve route
reuse. See [ZeroMQ socket options](https://libzmq.readthedocs.io/en/latest/zmq_setsockopt.html).
Detailed admission-to-egress accounting remains **Gap 6**.

### 6.7 Service adapter and future ICrypto session setup

Coverage: **AC1.8, AC1.10**.

**ICrypto is not included or called by the current repository.** The future
adapter is based on the reviewed
[ICrypto revision 6926f6a](https://github.com/rdkcentral/entservices-apis/blob/6926f6a72fbfff18b691d09b3b7376fbd541b9d3/apis/Crypto/ICrypto.h)
from [PR #1089](https://github.com/rdkcentral/entservices-apis/pull/1089).
Pin an approved compatible revision when integrating; this document does not
claim that the PR has been merged.

That revision supplies `ApplicationContext { appId, sessionId }` and
`SharedBuffer { name }`. The adapter creates these from trusted internal
records. A raw FD integer is not an interprocess shared-memory identity.

```mermaid
sequenceDiagram
    participant J as Private-pool request job
    participant A as CryptoServiceAdapter
    participant B as Backend boundary
    participant T as Thunder IShell - future
    participant C as ICrypto - future
    J->>A: Crypto request with retained connection context
    alt Backend not integrated / unavailable
        A->>B: Resolve configured backend capability
        B-->>A: Unavailable
        A-->>J: Explicit RPC failure, no fake Crypto success
    else Future ICrypto integration enabled
        A->>T: Acquire ICrypto by configured callsign
        T-->>A: Retained interface or unavailable
        A->>B: Ensure one initialization for this context/generation
        B->>C: NotifyAppState(context, CONNECTED)
        C-->>B: Invocation result and CryptoError
        B-->>A: Ready, or initialization failure
        A->>C: Requested method with trusted ApplicationContext
        C-->>A: Invocation result, CryptoError and outputs
        A->>A: Check invocation before consuming outputs
        A-->>J: Mapped RpcStatus and CryptoResponse
    end
```

Acquire a configured backend using the appropriate IShell interface lookup
(for example `QueryInterfaceByCallsign<Exchange::ICrypto>()`), not by
constructing the Crypto implementation inside Gateway. Retain the interface
for active calls and release it through an ordered stop/restart path.

An initialization barrier must prevent an operation from overtaking CONNECTED
or multiple concurrent requests from initializing the same context twice.
Whether CONNECTED occurs at authentication or first backend use, and how
never-used connections are cleaned up, remain TODO(BACKEND-SESSION).
No such notification is sent while the backend is absent.

Crypto owns key authorization. Gateway will eventually check API entitlements
before dispatch, but it must not duplicate Crypto's decisions about key usage,
export protection or cross-app sharing.

### 6.8 Crypto operation routing map

All 17 Protobuf operations have an explicit destination. Only the two buffer
management operations terminate locally; the remaining 15 require the future
backend. Structure/limit checks apply even in the interim accept-all phase.

| CryptoRequest operation | Destination / future method | Main inputs → successful outputs |
| --- | --- | --- |
| LOAD_KEY | ICrypto::LoadKey | locator, provision_type → key_id |
| KEY_EXISTS | ICrypto::KeyExists | locator → exists, key_id when present |
| FLUSH_KEYS | ICrypto::FlushKeys | trusted context → empty response |
| GENERATE_KEY_PAIR | ICrypto::GenerateKeyPair | algorithm, matching parameters → private_key_id, public_key_id |
| DERIVE_SESSION_KEYS | ICrypto::DeriveSessionKeys | algorithm and source key IDs → encryption/hmac/wrapping key IDs |
| GET_KEY_INFO | ICrypto::GetKeyInfo | key_id → type, usages, algorithm |
| GENERATE_KEY | ICrypto::GenerateKey | algorithm, usages → key_id |
| UNLOAD_KEY | ICrypto::UnloadKey | key_id → empty response |
| IMPORT_KEY | ICrypto::ImportKey | type, optional protection format, blob → key_id |
| EXPORT_KEY | ICrypto::ExportKey | key_id, optional protection format → blob |
| GENERATE_RANDOM | ICrypto::GenerateRandom | bounded byte count → random bytes |
| ENCRYPT | ICrypto::Encrypt | buffer_id, algorithm, key_id, init/input regions → encrypted region |
| DECRYPT | ICrypto::Decrypt | buffer_id, algorithm, key_id, init/encrypted regions → plaintext region |
| SIGN | ICrypto::Sign | buffer_id, algorithms, key_id, input region → signature region |
| VERIFY | ICrypto::Verify | buffer_id, algorithms, key_id, input/signature regions → verification_status |
| ALLOCATE_SHARED_BUFFER | SharedBufferManager::Allocate | requested_size → buffer_id, client name, size |
| RELEASE_SHARED_BUFFER | SharedBufferManager::Release | buffer_id → release result |

Preserve optional presence for import_export_type rather than replacing an
absent field with the enum's zero value. Validate repeated usages, algorithm
parameter variants, sizes and numeric conversions before constructing backend
types. Use explicit mappings rather than assuming enum ordinals always match.

### 6.9 Non-shared-memory operations

Coverage: **AC1.8; AC3.1–AC3.2**. This flow applies to the eleven key/random
operations above; allocation/release and the four bulk-data operations have
separate flows.

```mermaid
sequenceDiagram
    participant F as Firebolt Crypto / consumer
    participant L as EyilClient
    participant R as Gateway ROUTER
    participant J as Private request job / adapter
    participant C as ICrypto - future
    F->>F: Serialize operation-specific CryptoRequest
    F->>L: Send through selected sync/async C API
    L->>R: RpcRequest(service=CRYPTO, schema=1)
    R->>J: Submit retained context and request
    J->>J: Validate operation/oneof, parameters and API entitlement
    alt Backend absent
        J-->>R: Explicit unavailable failure
    else Backend integrated and context ready
        J->>C: LoadKey / GenerateKey / other mapped method
        C->>C: Enforce key policy and perform operation
        C-->>J: Core result, CryptoError and output values
        J->>J: Map both error layers, encode CryptoResponse
        J-->>R: Completion with original request ID/generation
    end
    R-->>L: RpcResponse through original connection
    L-->>F: Serialized result to matching waiter/callback
    F->>F: Inspect RpcStatus and CryptoError, decode result
```

Key generation/import/derivation may change backend state. Loss of a response
does not justify retrying automatically. Ordering UnloadKey or FlushKeys
against concurrent use of those keys is part of **Gap 5**.

### 6.10 Shared-memory identity, records and invariants

Coverage: **AC1.9–AC1.10**. Allocation is a Gateway responsibility; mapping by
the application and future access by Crypto are separate responsibilities.

| Identifier | Example | Meaning |
| --- | --- | --- |
| buffer_id | Opaque generated token | Gateway lookup key, scoped to one connection generation |
| Client name | `/eyil.<random-token>` | POSIX name opened with shm_open() inside the app container |
| Backend path | `/proc/<host-peer-pid>/root/dev/shm/eyil.<random-token>` | Proposed host-visible path passed as SharedBuffer.name |
| Gateway FD | Process-local descriptor | Retains the created object; never sent as a bare integer to Crypto |
| BufferRegion | `{ offset, length }` | Byte range in the same allocation, not a pointer |

Each buffer record retains the owner token/generation, peer evidence, opaque ID,
client name, backend path, actual capacity, FD, reserved/active use count and
release state. Per-connection count/byte quotas and a global byte budget require
atomic reservation and rollback on every failure path.

Use exclusive creation, generated names, close-on-exec descriptors and deliberate
ownership/mode settings. The shared-memory proposal sets the peer UID/GID and
owner read/write access. Verify user-namespace mappings and backend permissions
on the target platform; Gateway running in Thunder is not itself proof of the
required procfs access.

The proposal's namespace path uses the peer's mount view. Access is subject to
procfs checks and can become unavailable when the peer or its main thread exits.
See [proc_pid_root](https://man7.org/linux/man-pages/man5/proc_pid_root.5.html).
The path/FD relationship remains **Gap 4**: retaining an FD does not guarantee
that a later open of the pathname obtains the same object.

Required invariants:

- Only the owner connection may resolve a buffer_id; same UID/PID is insufficient.
- A connection may have multiple allocations. Initially allow at most one
  operation using an allocation at a time, with queued-use reservation rules
  still to be settled.
- Check `offset <= size && length <= size - offset`; avoid unchecked
  `offset + length` arithmetic. Enforce per-operation region presence and
  overlap rules.
- Keep a lease from the point at which the operation depends on the allocation
  until backend access is definitively over. Release/disconnect cannot invalidate it.
- The app must not resize, unlink or modify participating regions while an
  operation is outstanding. This cooperation rule is not protection against
  a malicious writable client; mutation/truncation defenses remain TODO.
- Gateway owns unlink/cleanup. An app unmaps/closes its own view; it does not
  call shm_unlink(). Unlink/close by Gateway also does not revoke an existing
  app mapping.
- A local client timeout or uncertain COM-RPC failure is not safe completion.

The sample's 32 MiB buffer, four buffers and 128 MiB per-session limits are
reference values, not production defaults adopted by this document. Select
configuration values together with the platform memory and concurrency budgets.

### 6.11 AllocateSharedBuffer and client mapping

Coverage: **AC1.9; AC2.4; AC3.1–AC3.2**. No ICrypto call is involved.

```mermaid
sequenceDiagram
    participant A as App-side Crypto / buffer helper
    participant L as EyilClient
    participant R as Gateway ROUTER
    participant J as Private request job
    participant B as SharedBufferManager
    participant K as Linux / peer private dev-shm
    A->>L: AllocateSharedBufferRequest(requested_size)
    L->>R: RpcRequest through DEALER IPC
    R->>J: Dispatch with retained connection context
    J->>B: Allocate(owner, requested_size)
    B->>B: Validate nonzero size, reserve count/byte quotas
    B->>K: Safely open peer shm directory, exclusive object creation
    B->>K: ftruncate, set approved ownership and permissions
    K-->>B: Retained FD and verified capacity
    B->>B: Publish buffer_id, client name, host path and owner
    B-->>J: AllocateSharedBufferResponse
    J-->>R: Completion carrying original context/generation
    R-->>L: RpcResponse
    L-->>A: buffer_id, POSIX name and size
    A->>K: shm_open(name, O_RDWR), inspect size, mmap(MAP_SHARED)
    K-->>A: Application mapping
    Note over B,K: Roll back object, FD and quota on any partial failure
```

The mapping helper is outside the current EyilClient transport API. Decide
whether Firebolt Crypto supplies it or a separate utility does. The application
must also handle allocation success followed by mapping failure: request release
when possible, otherwise let correlated connection cleanup reclaim the record.

If the connection closes while creation is in progress, recheck its generation
before publishing the result and clean up an allocation that cannot be admitted.
If an allocation reply is lost, the Gateway may own a buffer unknown to the app;
quota accounting and eventual connection cleanup must still cover it.

### 6.12 Shared-memory operations: common backend boundary

For all four operations, the app writes inputs before sending the request.
The Gateway resolves buffer_id and validates owner/ranges, then passes the
trusted backend path and regions to the future adapter. Crypto independently
opens the object, checks its actual size, maps it, performs the operation and
unmaps/closes it before a successful synchronous invocation returns.

The reviewed ICrypto contract selects output locations in Crypto. Gateway
forwards the returned region only after validating the invocation and service
result. On success, output must be inside the allocation. A BUFFER_OVERFLOW
result may describe required capacity rather than an accessible output range;
the app must not read it as produced data. Exact required-size propagation must
match the adopted interface revision.

The following diagrams expand the four operation-specific calls. All Crypto
participants are **future TODO(BACKEND-ICRYPTO)**; while absent, dispatch returns
an explicit failure without running these backend steps.

#### Encrypt

Coverage: **AC1.8–AC1.9; AC3.1–AC3.2**.

```mermaid
sequenceDiagram
    participant A as App-side Crypto / mapped buffer
    participant L as EyilClient
    participant R as Gateway ROUTER
    participant J as Private job / Crypto adapter
    participant B as SharedBufferManager
    participant C as ICrypto - future
    A->>A: Write init_data and plaintext input_data
    A->>L: EncryptRequest(buffer_id, algorithm, key_id, regions)
    L->>R: RpcRequest over DEALER IPC
    R->>J: Dispatch independent job
    J->>B: Acquire owner-checked lease, validate regions
    B-->>J: Retained object identity and capacity
    J->>C: Encrypt(context, buffer, algorithm, keyId, initData, inputData)
    C->>C: Open/check/map, enforce key policy, write ciphertext
    C->>C: Unmap and close before normal return
    C-->>J: Invocation result, CryptoError, encryptedData region
    J->>J: Validate successful output or preserve overflow result
    J->>B: End lease only when backend access is known finished
    J-->>R: Completion with EncryptResponse
    R-->>L: RpcResponse
    L-->>A: Matching completion, read ciphertext only on success
```

#### Decrypt

Coverage: **AC1.8–AC1.9; AC3.1–AC3.2**.

```mermaid
sequenceDiagram
    participant A as App-side Crypto / mapped buffer
    participant L as EyilClient
    participant R as Gateway ROUTER
    participant J as Private job / Crypto adapter
    participant B as SharedBufferManager
    participant C as ICrypto - future
    A->>A: Write init_data and encrypted_data
    A->>L: DecryptRequest(buffer_id, algorithm, key_id, regions)
    L->>R: RpcRequest over DEALER IPC
    R->>J: Dispatch independent job
    J->>B: Acquire owner-checked lease, validate input regions
    B-->>J: Trusted object identity and capacity
    J->>C: Decrypt(context, buffer, algorithm, keyId, initData, encryptedData)
    C->>C: Open/check/map, enforce key policy, write plaintext
    C->>C: Unmap and close before normal return
    C-->>J: Invocation result, CryptoError, outputData region
    J->>J: Validate result and successful output bounds
    J->>B: End lease after definitive backend completion
    J-->>R: Completion with DecryptResponse
    R-->>L: RpcResponse
    L-->>A: Matching completion, read plaintext only on success
```

#### Sign

Coverage: **AC1.8–AC1.9; AC3.1–AC3.2**.

```mermaid
sequenceDiagram
    participant A as App-side Crypto / mapped buffer
    participant L as EyilClient
    participant R as Gateway ROUTER
    participant J as Private job / Crypto adapter
    participant B as SharedBufferManager
    participant C as ICrypto - future
    A->>A: Write input_data
    A->>L: SignRequest(buffer_id, algorithm, key_id, digest, input)
    L->>R: RpcRequest over DEALER IPC
    R->>J: Dispatch independent job
    J->>B: Acquire owner-checked lease, validate input region
    B-->>J: Trusted object identity and capacity
    J->>C: Sign(context, buffer, algorithm, keyId, digestAlgorithm, inputData)
    C->>C: Open/check/map, sign, select/write signature region
    C->>C: Unmap and close before normal return
    C-->>J: Invocation result, CryptoError, signature region
    J->>J: Validate success region or overflow result
    J->>B: End lease after definitive backend completion
    J-->>R: Completion with SignResponse
    R-->>L: RpcResponse
    L-->>A: Matching completion, read signature only on success
```

#### Verify

Coverage: **AC1.8–AC1.9; AC3.1–AC3.2**.

```mermaid
sequenceDiagram
    participant A as App-side Crypto / mapped buffer
    participant L as EyilClient
    participant R as Gateway ROUTER
    participant J as Private job / Crypto adapter
    participant B as SharedBufferManager
    participant C as ICrypto - future
    A->>A: Write input_data and expected_signature
    A->>L: VerifyRequest(buffer_id, algorithms, key_id, two regions)
    L->>R: RpcRequest over DEALER IPC
    R->>J: Dispatch independent job
    J->>B: Acquire owner-checked lease, validate both input regions
    B-->>J: Trusted object identity and capacity
    J->>C: Verify(context, buffer, algorithms, keyId, input, signature)
    C->>C: Open/check/map, enforce key policy, compare signature
    C->>C: Unmap and close before normal return
    C-->>J: Invocation result, CryptoError, verificationStatus
    J->>B: End lease after definitive backend completion
    J-->>R: Completion with VerifyResponse
    R-->>L: RpcResponse
    L-->>A: Matching completion with boolean result
    Note over A,C: Verify returns no binary output region
```

A verification result of false and a failed invocation are different outcomes.
For every operation, a transport failure after remote dispatch may leave
completion uncertain. Do not take the normal lease-release arrow in that case;
retain/quarantine the resource until an agreed recovery mechanism resolves it.

### 6.13 ReleaseSharedBuffer, reuse and concurrent release

Coverage: **AC1.9–AC1.10**. No ICrypto call is needed for an idle allocation.

```mermaid
sequenceDiagram
    participant A as App-side buffer helper
    participant L as EyilClient
    participant R as Gateway ROUTER
    participant J as Private release job
    participant B as SharedBufferManager
    participant K as Linux
    A->>A: Wait for definitive completion of all uses
    A->>K: munmap and close own mapping/FD
    A->>L: ReleaseSharedBufferRequest(buffer_id)
    L->>R: RpcRequest over IPC
    R->>J: Dispatch release job with owner context
    J->>B: Release(owner, buffer_id)
    B->>B: Check owner, reject future use atomically
    alt No reserved or active operation
        B->>K: Unlink owned object and close retained FD
        B->>B: Remove record, release quota
        B-->>J: Released
    else Operation still has a lease
        B->>B: Mark release pending, retain object and quota
        Note over J,B: TODO: defer response or acknowledge pending release?
        B-->>J: Outcome according to agreed release contract
    end
    J-->>R: Completion
    R-->>L: RpcResponse
    L-->>A: Matching release result
```

When the last definitive lease ends, the manager can finish a pending release
exactly once. Unknown IDs, duplicate release and release pending acknowledgements
need precise error/idempotency rules.

Do not rely on the order requests arrived at ROUTER: a queued Encrypt can run
after a Release job unless its dependency was reserved or ordered earlier.
Similarly, `client_send_request()` timing out does not satisfy the app's
"all uses completed" condition. These are **Gaps 4 and 5**.

### 6.14 Disconnect detection, PID observations and hibernation

Coverage: **AC1.4, AC1.6, AC1.10**.

#### Event-driven connection cleanup

The target replaces the sample's inactivity watchdog with event-driven cleanup:
an actual transport disconnect is correlated to the original connection, its
admission is closed, and backend/resource cleanup follows the retained-work
rules below. Do not expire a context because no RPC arrived recently. UID alone
does not identify a connection; multiple connections from one process remain
independent.

These events establish connection state, not application health. Gateway need
not prove an idle app is responsive to retain its bounded connection state. If
an app hangs with the transport still open, cleanup requires an actual close or
an explicitly adopted lifecycle/connection-expiry policy. The question of
whether such expiry is required for resource reclamation, and whether transport
heartbeats should provide it, remains in section 5.5. Do not introduce client
health polling as part of disconnect handling.

The supplied libzmq patch extends version-2 disconnect monitor events with
UID/GID/PID in addition to the FD. `zmq_server.cpp` demonstrates consuming those
values. The draft/versioned monitor API and exact event format must match the
target patched libzmq build. A normal ZeroMQ 4.3 dependency declaration does not
guarantee that extension exists.

The patch does not enable or disable ZeroMQ heartbeats, and `zmq_server.cpp`
does not configure them. The separate EyilGateway client/Gateway samples do
enable them, as recorded in section 5.5; using those samples with patched libzmq
does not remove their heartbeat settings.

Standard monitor disconnect events do not include ZAP User-Id; their FD may
already be closed. See the
[monitor API](https://libzmq.readthedocs.io/en/latest/zmq_socket_monitor.html).
The patch's credential tuple still identifies a process, not one of its several
connections. FD reuse, handshakes that never send a request and rapid reconnects
must be covered by a stronger correlation contract.

```mermaid
sequenceDiagram
    participant P as Peer / kernel
    participant Z as libzmq monitor producer
    participant M as Gateway PAIR monitor worker
    participant REG as Registry / I/O owner
    participant J as Queued and active jobs
    participant C as Crypto adapter - future
    participant B as SharedBufferManager
    P->>Z: Socket closes or transport fails
    Z->>M: Disconnect event with versioned identity evidence
    M->>REG: Post normalized event
    REG->>REG: Correlate to exact connection generation
    alt Evidence is ambiguous
        REG->>REG: Record unresolved event, do not select every matching PID
        Note over M,REG: Gap 1 blocks reliable targeted cleanup
    else Exact original connection is known
        REG->>REG: Close admission, remove auth/routing visibility
        REG->>J: Resolve queued jobs, retain active references
        J-->>REG: Definitive completion of active backend access
        opt Backend had an initialized context
            REG->>C: Schedule NotifyAppState(DISCONNECTED) through worker path
            C-->>REG: Completion or unresolved failure
        end
        REG->>B: Finalize releasable allocations after leases end
        B-->>REG: Resources released or retained for unresolved access
        REG->>REG: Retire context when references and cleanup permit
    end
```

The proposed drain-before-destructive-notification ordering prevents
DISCONNECTED from freeing Crypto resources used by an active call. The
architectural requirement to notify on detection must be reconciled with this
ordering; it is a TODO, not a reconnect grace period. Backend work must not run
on the monitor or I/O thread.

Process-existence tracking can supplement monitor events. It needs a stable
process-instance handle or equivalent reuse protection; UID has no process
lifetime, and a PID existence check alone can match a replacement process.
A living process can close one of several connections. An inherited connection
can outlive its original creator, so process exit is not automatically socket
closure.

#### Intended process-freeze behavior and prerequisites

**An application may freeze its process after completing its operations. The
application owns that preparation; Gateway must not treat the pause as a
disconnect merely because the application is idle.** Preserving its live
connection also depends on the transport heartbeat decision in section 5.5.
This scenario assumes compatible settings, with heartbeats off as the current
proposal pending architect confirmation.

The intended operation freezes the existing process with its descriptors
preserved, for example through a process stop or container freezer. Linux's
[cgroup freezer](https://docs.kernel.org/admin-guide/cgroup-v2.html) stops the
processes until thawed; freezing is distinct from killing them.

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

```mermaid
sequenceDiagram
    participant A as Application / its callers
    participant C as EyilClient runtime
    participant P as Platform freeze mechanism
    participant K as Kernel / open UDS connection
    participant G as Gateway registry and I/O owner
    participant O as Another active application
    Note over C,G: This preservation scenario assumes transport heartbeats off, pending architect decision
    A->>A: Stop submissions and coordinate all callers
    C-->>A: Finish accepted calls and response callbacks
    A->>A: Establish no pending or uncertain remote operation
    A->>P: Application is ready for process freeze
    P->>K: Freeze process with existing descriptors preserved
    Note over A,C: All application and libzmq threads in this process stop
    Note over K,G: Freeze alone produces no socket disconnect event
    G->>G: Retain connection identity, context and idle resources
    O->>G: Requests on another connection
    G-->>O: Independent responses continue
    P->>K: Thaw the same process
    alt UDS connection and Gateway context survived
        A->>C: Submit a new request
        C->>G: RpcRequest on the existing connection
        G-->>C: RpcResponse for that request
        C-->>A: Normal request completion
    else Actual transport or Gateway failure occurred during pause
        C->>C: Process connection failure through the normal monitor/I/O path
        C-->>A: Report connection/request failure without replaying old work
        Note over A,G: Reconnect and resource recovery follow the separate lifecycle contract
    end
```

No application-level heartbeat or inactivity watchdog participates in this
flow. The agreed 15-second connect/request deadline applies only to a pending
local operation, never to an otherwise idle connection.

**ZeroMQ heartbeat risk remains TODO(ZMQ-HEARTBEAT).** If ROUTER continues sending
PINGs while all client threads are frozen, lack of traffic can expire the
transport timeout. The resulting disconnect is real even if the PID still
exists. Adding UID/GID/PID to that event does not prevent the timeout or preserve
the old socket. Once that connection has closed, normal event-driven cleanup
applies; ignoring the event would not restore it. If architects retain transport
heartbeats, they must also define the hibernation/reconnect and resource-recovery
contract. Section 5.5 records the options, proposed disabled baseline and
discussion points.

A process that is stopped can retain its PID/UID and its open UDS descriptors.
Do not classify a stopped state, absence of CPU progress or lack of messages as
disconnect evidence. PID existence provides no responsiveness guarantee, and a
dedicated client monitor thread would be frozen along with the rest of the app.
The supplied UID/GID/PID patch only enriches an actual transport-disconnect event;
it neither detects a freeze nor adds a timeout for one.

AppManager/RuntimeManager lifecycle subscriptions remain TODO(LIFECYCLE-EVENTS).
The idle process-freeze case does not require such an observer. Actual process
termination, checkpoint/restore that replaces the process or loses descriptors,
and restarting Gateway are separate cases: the same session cannot be assumed
to survive them. The specific platform hibernation mechanism still needs
verification if it does more than freeze the existing process.

#### Removed sample application-level liveness logic

| Removed from the target design | Replacement or retained responsibility |
| --- | --- |
| Client RpcHeartbeat producer, heartbeat worker and Gateway RPC_SERVICE_CONTROL heartbeat handler | Normal application requests only; PAIR monitor events supply connection-state information |
| Sample client_watchdog() thread and last_request inactivity expiry | Correlated disconnect events drive session cleanup, with safe handling of outstanding work and buffers |

The old watchdog could release buffers and notify DISCONNECTED after RPC
silence while the UDS socket remained connected. It is sample-only application
logic and is not part of the new client or Gateway architecture. There is no
replacement periodic application liveness timer. Keep socket monitors, safe
disconnect cleanup and the selected local operation deadlines. Built-in ZeroMQ
transport heartbeats are separate and remain under architect discussion.

#### Gateway behavior if a client stops receiving

For an idle freeze with the connection preserved, Gateway has no operation to
complete and no response to deliver to that client. It retains the context and
idle resource allocations while other clients continue. Retained buffers are
expected state, not evidence that disconnect cleanup failed; apply
per-connection and global quotas to keep that state bounded.

The design must still contain failures when an app freezes unexpectedly, violates
its preparation contract, or a future notification arrives during the pause:

- A response may remain queued for the frozen peer. Do not make ROUTER, the
  monitor or a private-pool worker wait for that peer to read it. Deliver through
  bounded per-connection completion queues and nonblocking ROUTER sends.
- Use explicit ROUTER_MANDATORY/error handling: a full live route must not
  silently lose an RPC response. EAGAIN is backpressure, not proof that the peer
  died. Schedule other writable routes instead of spinning or blocking on this
  one. A process-existence check cannot turn a full queue into a safe disconnect.
- A frozen client must not exhaust global completion storage or occupy all
  workers. Gateway needs independent count/byte admission and fair egress; the
  client's 64-request limit is insufficient as a server resource guarantee.
- If an operation is still executing, retain its context and buffer leases
  until its actual completion. Freezing, an undelivered reply or a local timeout
  must not invalidate backend memory access. On thaw, an overdue client request
  can report CLIENT_TIMEOUT even if backend execution finished during the pause.
- A real peer exit or socket failure still requires the normal correlated
  disconnect cleanup. If Gateway fails while the app is frozen, the app learns
  about the transport failure after it resumes; preservation is not guaranteed
  across that independent failure.

Exact egress budgets and saturation policy remain **Gap 6**. Bounded queues and
nonblocking sends protect other clients without inventing a freeze timeout.

#### Local transport evidence and remaining device validation

A local Linux probe using libzmq 4.3.5 stopped only its own DEALER test process
with SIGSTOP for 20.03 seconds, with transport heartbeats disabled at both ends.
The PID remained present, no ROUTER disconnect event occurred during the pause,
another client completed 21 round trips, and the stopped client resumed its
existing connection. A separate stress phase filled the frozen peer's route;
nonblocking mandatory ROUTER sends returned EAGAIN and the other client still
completed a request.

This verifies the local transport behavior, not the unimplemented EyilClient/
Gateway, patched ZAP/monitor identity handling, Crypto backend or Dobby platform
hibernation. Repeat the idle-freeze and backpressure scenarios on the target
device once the runtime exists, using its actual freeze mechanism and libzmq.

### 6.15 Backend unavailable, restarted or uncertain

Coverage: **AC1.8–AC1.10**, with backend integration pending.

```mermaid
sequenceDiagram
    participant T as Thunder backend availability - future
    participant A as Crypto adapter
    participant REG as Connection/backend states
    participant J as Active request job
    participant B as SharedBufferManager
    participant R as ROUTER owner
    T-->>A: Interface unavailable or remote backend lost
    A->>REG: Mark backend generation unavailable
    REG->>REG: Stop new backend-dependent admission
    J-->>A: Invocation returns failure or completion becomes uncertain
    alt Backend is known to have stopped all access
        A->>B: End affected leases according to recovery contract
    else Completion / remote process state uncertain
        A->>B: Retain resources, do not claim safe reuse
    end
    A-->>R: Failure completions where correlation remains valid
    opt Backend becomes available again
        T-->>A: New interface generation
        A->>REG: Require an explicit session/key recovery decision
        Note over A,REG: No automatic replay or reuse of old key/buffer handles
    end
```

This is the proposed failure boundary, not an implemented notification API.
Choose startup behavior, acquisition retries, interface-release ordering and
recovery semantics in **Gap 3**. While Crypto is not integrated, Gateway-local
buffer operations can remain available according to their own admission rules.

### 6.16 Gateway stop, unload and crash recovery

Coverage: **AC1.1, AC1.4, AC1.7, AC1.10**.

```mermaid
sequenceDiagram
    participant T as Thunder
    participant P as EyilGateway wrapper
    participant I as Implementation runtime
    participant R as ROUTER / ZAP / monitor owners
    participant W as Private worker pool
    participant C as Crypto adapter - future
    participant B as SharedBufferManager
    T->>P: Deinitialize
    P->>I: Request deterministic Stop - trigger is TODO
    I->>I: Close admission, mark contexts closing
    I->>R: Stop accepting handshakes/requests, keep control drain alive
    I->>W: Cancel or resolve queued work under agreed policy
    W-->>I: Active calls finish and completion references drain
    I->>C: Schedule final backend notifications on worker path
    C-->>I: Notifications complete or failure policy applies
    I->>B: Finalize buffers with no unresolved access
    I->>W: Stop and join private workers
    I->>R: Disable monitors, close sockets on their owners, exit
    I->>I: Join transport/auth/monitor threads, terminate ZMQ context
    I-->>P: Runtime quiescent
    P->>P: Release implementation and shell references
    opt Future remote implementation mode
        P->>T: Terminate/release remote connection after orderly stop
    end
    P-->>T: Deinitialize complete
```

Do not unload a library while a job, callback or socket thread can execute its
code. A finite ZMQ linger policy is necessary for bounded transport shutdown;
context termination waits for open sockets and pending linger.
[ZeroMQ context termination](https://libzmq.readthedocs.io/en/latest/zmq_ctx_term.html)

This sequence needs an actual stop trigger through the existing
IConfiguration-only split. Merely adding threads and relying on the current
Release()/Terminate() sequence is insufficient. In-process Crypto calls cannot
be safely killed by forcefully terminating their worker thread.

A crash is different from orderly stop. Named objects can survive without their
registry, and a remote backend might still be accessing one. Define an ownership
marker/epoch and safe orphan recovery; never scan and unlink arbitrary app
shared-memory files. Crash cleanup, hung calls and a bounded shutdown policy
remain **Gap 2**.

### 6.17 Implementation order and verification targets

Implement the transport and registry behind the seams described above before
adding external dependencies. The following order keeps each step reviewable:

1. Resolve the remaining notification/shutdown callback and connection-correlation
   contracts that block the first implementation. Apply the selected 15-second
   operation deadlines and 64-request client limit. Discuss ZeroMQ heartbeat and
   hibernation policy with architects; use the explicitly marked disabled proposal
   for the preservation scenario. Keep app-owned preparation and actual
   disconnect/reconnect behavior distinct.
2. Implement the agreed RPC envelope and typed transport-error/completion
   behavior; align the header comment and async signatures. Add EyilClient's
   owner loop, bounded queues, pending map and shutdown. Prove two callers can
   use one connection without touching its socket.
3. Implement Gateway startup/ZAP/interim identities, the registry and monitor
   integration. Prove multiple connections from one PID remain independent.
4. Add private-pool jobs, bounded admission and completion routing using an
   unavailable backend seam. Test errors without claiming Crypto success.
5. Add Gateway-local allocations, ownership checks and leases. Resolve pathname
   identity and release-ordering gaps before enabling real backend access.
6. Integrate AppManager/RuntimeManager authentication only at the marked TODOs,
   once their formal API exists. Add lifecycle event behavior only after a
   separate policy decision.
7. Integrate the approved ICrypto revision in the adapter, including session
   initialization, failure handling and definitive buffer completion.

| Test scope | Scenario / evidence required |
| --- | --- |
| L1 client | Request IDs and response matching; duplicate/late replies; argument/output ownership; exactly one local completion |
| L1 async errors | Immediate rejection has no callback; accepted requests complete once with status and original user_data; local failures return NULL/zero; valid Gateway error responses preserve CLIENT_OK plus RpcStatus |
| L1 client limits | 64 queued plus awaiting-response requests share one limit; further submissions are rejected without a callback; slots are retired once; a request uses one 15-second budget across queue/send/receive |
| L1 Gateway | Envelope/oneof validation; context lookup; admission accounting; enum/presence mapping; allocation rollback |
| L1 concurrency | Queued/active leases, disconnect during dispatch, independent connections and dependency barriers |
| L2 transport | Real DEALER/ROUTER IPC, ZAP metadata, reverse-order completion and simultaneous sync/async callers |
| L2 connect readiness | CONNECTED alone cannot complete connect; HANDSHAKE_SUCCEEDED returns CLIENT_OK; explicit rejection returns CLIENT_AUTH_FAILED; absent/stalled endpoint reaches CLIENT_TIMEOUT; each failure wakes the waiter and rolls back resources |
| L2 heartbeat policy | Verify the reviewed DEALER/ROUTER option settings before connect/bind; reproduce the hibernation disconnect risk with sample heartbeats enabled; validate the policy selected under ZMQ-HEARTBEAT |
| L2 disconnect | Target patched monitor, multiple connections from one PID, FD/route reuse, disconnect before first request |
| L2 buffers | Container-private mapping, cross-connection rejection, release during work, size/region failures and lost replies |
| L2 lifecycle | Partial-start rollback, stop with queued/active jobs, slow receiver fairness and no surviving library threads |
| L2 process freeze | For the proposed transport-heartbeats-disabled scenario: app drains operations/callbacks, freezes longer than 15 seconds, preserves its live context/buffers while another client continues, then resumes; no custom RPC heartbeat or inactivity watchdog exists |
| L2 unexpected frozen receiver | Outstanding response hits route backpressure without blocking other routes/workers; bounded accounting and buffer leases survive until definitive completion or actual disconnect |
| Future integration | AppManager identity mapping, ICrypto invocation/result mapping, backend restart and communicator restrictions |

Use a controlled fake adapter for concurrency/failure tests when needed, clearly
separate from production Crypto and never described as real cryptography.
No optional example app or Crypto stub is currently installed by this skeleton.
An eventual DobbyTool-based smoke test should execute the client inside a real
application container to verify mounts, credentials and private shared memory;
DobbyTool itself is not in the RPC path.

The implementation wiring should make the current external boundaries visible
in comments, following this conceptual example (not existing source code):

```text
GatewayRuntime
  authentication = InterimIdentityProvider
    // TODO(AUTH-APPMANAGER): replace acceptance with the formal UID lookup.
    // TODO(API-AUTHORIZATION): populate per-connection API entitlements.
  cryptoBackend = UnavailableBackend
    // TODO(BACKEND-ICRYPTO): bind the approved interface in one adapter.
    // TODO(BACKEND-SESSION): add ordered CONNECTED/DISCONNECTED handling.
  lifecycleObserver = none
    // TODO(LIFECYCLE-EVENTS): add agreed platform subscriptions and policy.
```

Keep this choice explicit in startup/configuration and tests. Interim admission
must not be silently mistaken for production authorization when the later
provider is unavailable.

### 6.18 Build and repository validation reference

These commands validate the existing skeleton. Runtime acceptance remains tied
to the implementation and tests described above.

<details>
<summary>Build, installation, coverage and GitHub workflow details</summary>

#### Build, install and validation

Required tools: CMake 3.21+, Ninja, a C/C++ compiler, Protobuf 3.15+ development
files and protoc, ZeroMQ 4.3+ development files, and Python 3.9+. Building the
plugin additionally requires Thunder Core/COM/Plugins, ThunderTools and the
matching `interfaces/IConfiguration.h` from ThunderInterfaces or the platform's
entservices-apis SDK. Out-of-process use also requires matching IConfiguration
proxy/stubs in Thunder's configured proxystub directory. Do not mix headers
and proxy/stubs from different interface providers or releases.

For a native SDK build, install the host development prerequisites, then run:

```sh
bash build_dependencies.sh
bash build.sh
python3 scripts/run-tests.py --build-dir build --label L1
python3 scripts/run-tests.py --build-dir build --label L2
bash scripts/check-install.sh build rootfs/all
```

build_dependencies.sh fetches pinned Thunder R4.4, ThunderTools and
ThunderInterfaces sources into the ignored _deps directory. It builds the
IConfiguration headers and proxy/stubs along with Thunder. It does not install
system packages. To use an existing SDK, set EYIL_THUNDER_PREFIX; optional
EYIL_DEPS_DIR selects local dependency storage. EYIL_CONFIGURATION_INCLUDE_DIR
can select the SDK include directory containing `interfaces/IConfiguration.h`
when it is not found automatically.

To generate a configuration for a separate implementation process, build with
`bash build.sh -DPLUGIN_EYILGATEWAY_MODE=Local`. The default is `Off`. Install both
plugin libraries and make the matching proxy/stubs available to Thunder and
WPEProcess before using Local mode.

The client and protocol can be built without Thunder:

```sh
cmake -S . -B build-client -G Ninja \
  -DPLUGIN_EYILGATEWAY=OFF -DBUILD_EYILCLIENT=ON \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build-client
bash scripts/check-install.sh build-client rootfs/client
```

Use a CMake toolchain file and target dependency prefixes when cross compiling.
Protobuf_PROTOC_EXECUTABLE must identify a host-runnable protoc. Cross-platform
rootfs validation is still pending.

Installed CMake consumers use EyilClient::EyilClient and
EyilProtocol::EyilProtocol. The C header is eyilclient/EyilClient.h; the client also
provides eyilclient.pc. Both plugin libraries install to the Thunder plugin
directory. The generated configuration uses org.rdk.EyilGateway, with autostart
false by default. No test executables are installed.

L1 and L2 are intentionally empty. Their runners invoke CTest and emit summaries
and JUnit reports with zero tests. Install-consumer checks verify compilation,
linking, client API placeholders and the Protobuf status contract separately;
they are not advertised as implemented service tests.

```sh
EYIL_BUILD_DIR=build-coverage bash build.sh -DENABLE_COVERAGE=ON
python3 scripts/coverage.py --build-dir build-coverage
```

Coverage instruments handwritten plugin/client code, excluding generated
Protobuf and dependency sources. Empty suites produce an explicit coverage
unavailable summary. Once real tests are registered with L1/L2 labels, the same
runner produces XML, HTML and text reports. No artificial percentage is reported.
Use `--label L1` or `--label L2` to run coverage for one suite; the default runs
both suites.

#### Repository checks and GitHub configuration

C/C++ uses .clang-format; Python uses Black; YAML uses yamllint. New source,
build, workflow and documentation files carry the Apache-2.0 licensing header.
JSON remains strict JSON and is covered by LICENSE/NOTICE. README.md is
intentionally empty.

Install the pinned development tools from scripts/requirements-dev.txt and run
scripts/check-format.sh plus scripts/validate-files.py. Build outputs and local
SDKs are ignored by Git.

GitHub workflows cover native build/install checks, standalone client builds,
L1/L2, coverage, formatting, JSON validation, manual CI, FOSSID,
changelog/version validation, and manual draft release creation. CLA and
CODEOWNERS are retained as repository governance.

L1 and L2 each collect coverage for their selected suite through the shared
`native-ci.yml` workflow. Manual CI uses the same native build job. Formatting
and licensing checks run together with JSON checks in `json_validator.yml`.
The scripts directory supplies the build and validation commands used locally
and in CI; GitHub Actions itself does not require that directory layout.

Jobs use GitHub-hosted `ubuntu-24.04` runners. This release supplies Protobuf
3.21, which meets the project's 3.15 minimum for proto3 optional fields; Ubuntu
22.04 supplies Protobuf 3.12. The native SDK cache includes the runner release.
No organization-specific runner label is required. Native and standalone-client
builds use CMake 3.28.3, matching the tested SDK setup and retaining compatibility
with the pinned Thunder sources' older CMake policy baseline.

FOSSID uses the public
`rdkcentral/build_tools_workflows/.github/workflows/fossid_integration_stateless_diffscan.yml@1.0.0`
on pull requests from branches in this repository. It requires the four FOSSID
secrets referenced in the workflow and skips fork pull requests. CLA uses
`rdkcentral/cmf-actions` and requires CLA_ASSISTANT. Build, test, coverage and
repository-validation jobs require no organization credentials.

TODO(CI): add Coverity when an accessible rdkcentral workflow and its credential
contract are available. No private Coverity workflow is referenced in this
repository.

The scripts directory, formatting configuration and CHANGELOG.md must be
committed with the workflows so a fresh checkout contains their dependencies.

The release workflow is manual and creates a draft only from main after
checking the requested version against CMake and CHANGELOG.md. Repository
administrators must configure branch protection and runner access separately.
Version checks also compare the CMake version and EyilGateway/Version.h so the
plugin's reported and registered versions agree.

</details>

## 7. Gaps to answer

These are the **six architectural gaps from the review**, followed by the
remaining API/platform TODOs. Proposed safeguards in earlier sections describe
the required invariants; they do not close these questions. Resolve the
blocking items before implementing the affected production path.

### 7.1 Gap 1: Connection identity for disconnects and outgoing responses

**Question:** what stable transport identity links a ZAP acceptance, each
request, its ROUTER route and a later disconnect to the same connection
generation?

The supplied patch returns FD/UID/GID/PID, but two connections from one process
share the credentials. FDs and routing IDs can be reused. A ZAP connection may
also disconnect before sending its first request, leaving no request-derived
route binding.

Answers required:

- Which immutable token or verified mapping joins ZAP User-Id, monitor events
  and ROUTER messages, including before the first request?
- Does the target libzmq need a further patch to expose the same token at all
  three points? Which version/draft build is required?
- How are reconnect, FD reuse, duplicate routing IDs and ROUTER handover handled?
- How is a late reply prevented from reaching another connection that reused
  the same ROUTER identity? An incoming User-Id check alone does not protect
  an outgoing route.
- What happens when a monitor event cannot be correlated, without disconnecting
  unrelated connections or retaining unbounded abandoned state?

**Implementation constraint:** carry a generation through every job/completion
and reject stale delivery. This is necessary but is not a complete transport
binding by itself; libzmq can change routes independently of the registry.
Do not claim that the credential patch alone solves this gap.

**Blocks:** reliable independent-session cleanup and secure response routing.
**Coverage affected:** AC1.4, AC1.6, AC1.10; AC2.3–AC2.4.
**TODO:** CONNECTION-CORRELATION.

### 7.2 Gap 2: Complete startup, shutdown and crash-recovery lifecycle

**Question:** how does Thunder activation/deactivation drive a complete,
deterministic runtime lifecycle through the current IConfiguration-only split?

Answers required:

- What must be ready before Configure succeeds, and is missing Crypto allowed
  to leave the transport in a deliberately degraded mode?
- How does the wrapper request and await stop before Release/remote Terminate?
  Is controlled destruction sufficient, or is an explicit internal stop
  contract needed?
- Which queued requests are cancelled versus drained? How are active calls,
  completion records, ZAP replies and monitor workers joined in dependency order?
- What is the policy when an in-process backend never returns or a remote call
  has an uncertain outcome? A timeout cannot make forced thread destruction safe.
- How are allocations recovered after a crash without unlinking a live client's
  or another Gateway instance's object?
- Which bounded shutdown/linger policies apply, and what state is reported if
  safe teardown cannot complete?

**Implementation constraint:** use one rollback/stop ownership model and keep
implementation code loaded while any worker can still execute it. Do not infer
that the existing skeleton's reference release is a complete runtime stop.

**Blocks:** safe plugin disable/reload, partial-start rollback and orphan cleanup.
**Coverage affected:** AC1.1, AC1.4, AC1.7, AC1.10.
**TODO:** GATEWAY-LIFECYCLE.

### 7.3 Gap 3: Crypto acquisition, availability and restart lifecycle

**Question:** how will the future adapter manage an ICrypto interface and its
per-connection Crypto sessions across startup, failure and backend replacement?

Answers required:

- Which callsign and approved ICrypto revision are used? What is the interface
  acquisition, reference ownership and availability-notification mechanism?
- Does backend absence fail Gateway startup or only backend-dependent requests?
  Which RpcStatus communicates that condition?
- Is CONNECTED sent on authentication or first backend use? How are concurrent
  first requests ordered, and what happens if initialization fails?
- What are the backend's concurrency guarantees for different requests, one
  ApplicationContext, and different keys/buffers?
- What happens to existing key handles and context state after backend restart?
  Must the client reconnect, or is a defined reinitialization sequence available?
- How do DISCONNECTED and invocation failure interact with active calls and
  uncertain remote completion?

**Implementation constraint:** retain the internal backend seam now; add ICrypto
types and calls only in the future adapter. Never replay operations or silently
reuse a prior backend generation's resources.

**Blocks:** real Crypto integration and correct backend session cleanup.
**Coverage affected:** AC1.8–AC1.10; AC3.1.
**TODOs:** BACKEND-ICRYPTO, BACKEND-SESSION, BACKEND-RECOVERY.

### 7.4 Gap 4: Stable shared-memory object identity and lifetime

**Question:** how do Gateway and Crypto guarantee they access the same backing
object, and when is that object safe to reuse or destroy?

Gateway retains an FD, but Crypto opens a pathname. A client can potentially
replace/unlink the name, resize the file or mutate mapped bytes. A retained FD
continues referring to the original object even if its name changes; a later
open by name need not. `O_NOFOLLOW` alone does not secure the entire path.
See the [Linux open API](https://man7.org/linux/man-pages/man2/open.2.html).

Answers required:

- Is the client-namespace pathname a sufficient identity under the accepted
  threat model, or is a stable Gateway-controlled path/object handle needed?
- How does Crypto verify the opened object matches the retained allocation?
  What changes, if any, are needed in the backend identity contract?
- What survives peer exit, PID reuse or loss of /proc/PID/root access? Which
  namespace references, permissions or capabilities are required?
- What prevents replacement, truncation and mutation during processing?
  Are input snapshots or a different protected mapping arrangement required?
- After a client timeout, disconnect or uncertain COM-RPC result, what proves
  backend completion? How does the app learn that reuse is safe?
- What are the release acknowledgement and BUFFER_OVERFLOW/required-capacity
  contracts?

**Implementation constraint:** retain leases and quarantine uncertain use.
Gateway ownership is allocation ownership; it is not immutability, revocation
of client mappings or proof that a reopened pathname resolves to the same file.

**Blocks:** safe real shared-memory backend access and reliable cleanup/reuse.
**Coverage affected:** AC1.8–AC1.10.
**TODOs:** SHM-IDENTITY, SHM-COMPLETION, SHM-MUTATION.

### 7.5 Gap 5: Ownership and ordering under parallel dispatch

**Question:** which component serializes mutable session/resource state, and
what ordering is guaranteed for requests that depend on the same resource?

Answers required:

- Which transitions belong to the I/O owner, and which registry/buffer/backend
  operations use locks or retained records across ZAP and worker threads?
- Does a job acquire/reserve its buffer dependency before enqueueing or when
  execution begins? How is a queued use protected from a later Release?
- Are dependent operations client-ordered, server-serialized, rejected while
  busy or represented by continuations? Which errors express rejection?
- How do UnloadKey and FlushKeys interact with pending Sign/Encrypt/Derive calls?
- Does closing a session cancel queued work, drain it, or use different rules
  for requests that have not yet entered Crypto?
- How are CONNECTED/DISCONNECTED barriers scheduled without blocking every
  worker on work that is itself queued behind them?

**Implementation constraint:** one dispatch object per request does not imply
unrestricted simultaneous access to every resource. Define resource-specific
ordering while keeping unrelated requests concurrent on the private worker pool.

**Blocks:** safe concurrent request, release and disconnect behavior.
**Coverage affected:** AC1.6–AC1.10; AC2.4.
**TODO:** PARALLEL-ORDERING.

### 7.6 Gap 6: Response-side backpressure and fairness

**Question:** how are completions bounded and fairly delivered when a client
stops receiving, freezes or produces requests faster than it consumes replies?

Answers required:

- What per-connection/global count and byte limits cover pending requests,
  completed responses and libzmq queues? The selected 64 outstanding requests
  per Client do not define or enforce these Gateway-side budgets.
- Is response capacity reserved before dispatch, including large random/export
  results and error replies? What happens if serialization exceeds that budget?
- How are nonblocking multipart sends resumed/rejected on EAGAIN, and how is
  EHOSTUNREACH distinguished from temporary pressure?
- How are clients scheduled fairly so one full route cannot starve others?
- What happens to a slow client's completions when its limit is reached?
  The intended preservation scenario retains the live context without a custom
  idle timer; transport heartbeats remain under architect discussion. Unexpected
  backlog needs bounded handling; any policy to disconnect a saturated live
  client remains a separate decision.
- How does shutdown release completion accounting without blocking workers on
  a full egress queue or losing ownership of live buffers?

**Implementation constraint:** bounding the private request queue alone is
insufficient. Do not block the ROUTER or every worker while waiting for one
client's output queue. Use explicit admission-to-completion accounting.

**Blocks:** bounded resource use and progress under slow/frozen clients.
**Coverage affected:** AC1.2, AC1.7, AC1.10; AC2.4.
**TODO:** EGRESS-BACKPRESSURE.

### 7.7 Remaining API and platform decisions

These items complement the six gaps; several are prerequisites for implementing
a public API, even though they are not new Gateway architecture gaps.

| TODO | Answer needed / integration boundary |
| --- | --- |
| AUTH-APPMANAGER | Formal UID lookup via AppManager/RuntimeManager, authoritative app/app-instance identity, UID reuse guarantees and lookup failure behavior |
| API-AUTHORIZATION | Exact entitlement format and mapping from service/operation to allowed APIs; currently all supported APIs are allowed |
| LIFECYCLE-EVENTS | Whether/how to integrate stop, destruction or platform hibernation events; application-owned preparation is agreed, while any observer needed by the selected transport/reconnect policy remains undefined |
| CONNECTION-LIFETIME | Actual connection-loss/reconnect, Gateway restart and checkpoint/restore behavior; preservation across app freeze is conditional on compatible transport settings, with no custom inactivity watchdog |
| ZMQ-HEARTBEAT | Is bounded expiry of an idle, completely hung peer's connection required for resource reclamation? App health supervision is outside Gateway scope. Proposed off to preserve frozen connections; if enabled, define transport-silence expiry and hibernation/reconnect/resource recovery; see section 5.5 |
| CLIENT-CALLBACKS | Initial connection-callback timing, connection/notification handler lifetime and replacement/quiescence; request-completion errors, user_data, I/O-thread execution and reentrancy rules are specified in section 5.4 |
| CLIENT-SHUTDOWN | Explicit local-close result code, shutdown callback ordering and concurrent-caller quiescence; accepted work must use the agreed typed completion path |
| CLIENT-ADMISSION | Local full-queue result after the selected 64 outstanding requests per Client; ClientResult lacks a queue-full value, and server RpcStatus must not be fabricated for local rejection |
| CLIENT-MONITOR | Confirm the monitor-responsibility checklist in section 5.2 and whether PAIR plus the I/O owner satisfies the AC, or a distinct OS thread is mandatory; neither arrangement is implemented yet |
| STRING-CODEC | Choose later between Base64 of the entire binary envelope and ProtoJSON, including text/binary size limits; both string APIs currently return CLIENT_NOT_IMPLEMENTED |
| NOTIFICATIONS | Wire message/service definition and notification classification; local registration alone is insufficient |
| ERROR-MAPPING | Invalid service payload, duplicate/malformed IDs, backend unavailable, busy/release-pending and quota errors |
| BUFFER-HELPER | Ownership of app-side shm_open/mmap helpers, output validation and release coordination |
| DEPLOYMENT | UDS mount/recreation, permissions, procfs/user-namespace access, private /dev/shm and blocked direct Crypto communicator access |
| RESOURCE-LIMITS | Byte/message limits beyond the selected client count of 64; Gateway private-pool workers/stacks, queue/response budgets, buffer counts/sizes and global limits |
| ZMQ-PLATFORM | Exact peer-credential encoding, patched version-2 monitor support and stable connection identity on supported devices |
| CI | Add Coverity only when the usable public workflow and credential contract are available; real L1/L2 coverage follows real tests |

**Current integration boundary remains unchanged:** Gateway does not call
AppManager/RuntimeManager or Crypto/ICrypto today. Implement the local transport
and ownership mechanisms around the marked seams, then add each external
integration after its contract is agreed. The app-owned idle process-freeze
preparation, 15-second operation deadlines and 64-request client count limit are
selected. Application heartbeat and inactivity-watchdog logic are removed from
the target client and Gateway. ZeroMQ transport heartbeats, actual reconnect,
destructive hibernation/checkpoint behavior and external lifecycle integration
remain explicit architecture questions.
