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

# EyilGateway implementation guide

This guide complements the [canonical capability specs](SpecificationMap.md#capability-index).
Read their full detailed contracts with the diagrams, rationale and procedures here.
Original ACs and open decisions remain in [EyilSecureGateway.md](EyilSecureGateway.md).
Section numbers and source anchors are preserved for navigation; the
[source map](SpecificationMap.md) covers every original section.

- [6.1 Plugin and implementation organization](#61-plugin-and-implementation-organization)
- [6.2 Thread model and private Thunder pool](#62-thread-model-and-private-thunder-pool)
- [6.3 Runtime records and synchronization](#63-runtime-records-and-synchronization)
- [6.4 Startup and partial-start failure](#64-startup-and-partial-start-failure)
- [6.5 ZAP, interim identity and future AppManager lookup](#65-zap-interim-identity-and-future-appmanager-lookup)
- [6.6 Request ingress, dispatch and response delivery](#66-request-ingress-dispatch-and-response-delivery)
- [6.7 Crypto adapter: per-call ICrypto references and per-connection sessions](#67-crypto-adapter-per-call-icrypto-references-and-per-connection-sessions)
- [6.8 Crypto operation routing map](#68-crypto-operation-routing-map)
- [6.9 Non-shared-memory operations](#69-non-shared-memory-operations)
- [6.10 Shared-memory identity, records and invariants](#610-shared-memory-identity-records-and-invariants)
- [6.11 AllocateSharedBuffer and client mapping](#611-allocatesharedbuffer-and-client-mapping)
- [6.12 Shared-memory operations: common backend boundary](#612-shared-memory-operations-common-backend-boundary)
- [6.13 ReleaseSharedBuffer and conflicting buffer operations](#613-releasesharedbuffer-and-conflicting-buffer-operations)
- [6.14 Disconnect detection, PID observations and hibernation](#614-disconnect-detection-pid-observations-and-hibernation)
- [6.15 On-demand Crypto lookup and invocation failures](#615-on-demand-crypto-lookup-and-invocation-failures)
- [6.16 Gateway deactivation, reactivation and fresh startup after a crash](#616-gateway-deactivation-reactivation-and-fresh-startup-after-a-crash)
- [6.17 Implementation order and verification targets](#617-implementation-order-and-verification-targets)

## 6. EyilGateway design and flows

### 6.1 Plugin and implementation organization

Contract requirements: [GL-01 Wrapper and implementation ownership](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-01-wrapper-and-implementation-ownership).

| Component | Responsibility | Separation rule |
| --- | --- | --- |
| EyilGateway wrapper | Register notifications, acquire/configure implementation, final Release waits for destruction | Keep transport and service logic out of IPlugin |
| EyilGatewayImplementation | Own configuration/runtime and destructor-driven shutdown | IConfiguration starts the runtime; destruction drains and stops it before releasing IShell |
| GatewayServer / IoLoop | Own ROUTER, receive messages, route completions | No blocking backend work; no worker uses ROUTER |
| ZapAuthenticator | Own REP, parse credentials, answer ZAP | No application-supplied identity is trusted |
| AuthenticationProvider | Interim acceptance; future authoritative lookup | TODO(AUTH-APPMANAGER), separate from transport |
| ConnectionRegistry | Map User-Id to retained context; serialize admission/closing | Never key ownership solely by UID/PID/routing ID |
| RequestDispatcher | Envelope/service validation, routing and nonblocking handoff to the private pool | Service-neutral at outer boundary |
| Current request / dispatcher | One retained admitted request awaiting a worker, no request queue | Pause further ROUTER reception; worker-start deadline is 10 seconds, with atomic timeout/start arbitration |
| RequestJob | One retained dispatch object per accepted request | Immutable input plus explicit context/resource references |
| PrivateRequestPool | Own a Core::ThreadPool with a configured worker count | No replacement of Thunder's global worker singleton; internal capacity must not block ROUTER |
| CryptoServiceAdapter | Decode Crypto operations and query/invoke/release ICrypto per call | No cached ICrypto pointer, global Crypto session, health observer or duplicated key policy |
| SharedBufferManager | Create/track connection-owned allocations, shared per-host-UID quota records, active-use leases and deferred release; finalize once access ends | Gateway-local; quota spans connections, buffer access remains scoped to its owner connection; no dependency on ICrypto types |
| CompletionHandoff | Transfer completed owned records to the ROUTER owner, without count/byte caps | At most one send attempt; release on success/failure, no reply retry queue or delivery to a replacement route |
| MonitorCollector | Own PAIR and post normalized transport events | Correlation is a prerequisite for targeted cleanup |

#### Gateway component and API-call overview

Contract requirements: [GR-01 Socket owners and private request workers](../openspec/specs/gateway-routing/spec.md#requirement-gr-01-socket-owners-and-private-request-workers),
[GR-04 Ingress validation and accepted ownership](../openspec/specs/gateway-routing/spec.md#requirement-gr-04-ingress-validation-and-accepted-ownership),
[GR-05 Completion handoff](../openspec/specs/gateway-routing/spec.md#requirement-gr-05-completion-handoff),
[GR-06 Single nonblocking response attempt](../openspec/specs/gateway-routing/spec.md#requirement-gr-06-single-nonblocking-response-attempt),
[CC-04 Socket configuration and disabled heartbeats](../openspec/specs/client-connection/spec.md#requirement-cc-04-socket-configuration-and-disabled-heartbeats),
[CI-03 FD and transport-lifetime binding](../openspec/specs/connection-identity/spec.md#requirement-ci-03-fd-and-transport-lifetime-binding),
[GL-01 Wrapper and implementation ownership](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-01-wrapper-and-implementation-ownership).

Coverage: **AC1.1–AC1.12; AC2-Client.2–AC2-Client.3, AC2-Client.5;
AC2-Firebolt.1–AC2-Firebolt.2**.

The application calls EyilClient's C API. Gateway receives the resulting
Protobuf RPC over its ROUTER socket and dispatches a request job. The diagram
separates that request/response path from authentication, connection cleanup
and Thunder lifecycle control. Everything inside the implementation block is
owned by that Gateway runtime; the application and Crypto plugin remain
outside it.

```mermaid
flowchart TB
    APP["Application / Firebolt Crypto"]
    CLIENT["EyilClient public C API<br/>Connect, request and disconnect<br/>Client I/O owner + DEALER"]
    TH["Thunder framework"]
    subgraph GW["EyilGateway plugin"]
        WR["EyilGateway IPlugin wrapper<br/>Initialize / Deinitialize"]
        subgraph RT["EyilGatewayImplementation"]
            subgraph TRANSPORT["Socket owners"]
                IO["Gateway I/O owner<br/>ROUTER + RequestDispatcher<br/>Validate, admit and send replies"]
                ZAP["ZapAuthenticator worker - ZAP REP<br/>AuthenticationProvider<br/>Trusted UID / GID / host PID"]
                MON["MonitorCollector worker<br/>Monitor PAIR"]
            end
            REG["ConnectionRegistry<br/>User-Id + FD / transport lifetime<br/>Context, route and admission state"]
            CURRENT["Current request - no waiting queue<br/>Pause further receives while awaiting a worker<br/>10 seconds to start, otherwise INTERNAL + ERROR"]
            POOL["Private Thunder pool<br/>RequestJob::Dispatch + lifecycle jobs<br/>4 workers"]
            AD["CryptoServiceAdapter<br/>Decode and map Crypto operations<br/>Scoped ICrypto reference per call"]
            BUF["SharedBufferManager<br/>Connection-owned buffers, quota shared by host UID<br/>Active-use leases and deferred release"]
            OUT["CompletionHandoff<br/>RpcResponse + original generation<br/>No response count/byte cap"]
        end
    end
    CRYPTO["Crypto plugin / ICrypto<br/>Configured Crypto backend callsign"]
    PLATFORM["AppManager / RuntimeManager<br/>Future identity and authorization lookup"]

    APP -- "Calls client_* functions" --> CLIENT
    CLIENT <-->|"RpcRequest / RpcResponse over UDS<br/>ipc:///tmp/app-eyil-gateway.ipc"| IO
    TH -- "Initialize / Deinitialize" --> WR
    WR -- "Acquire, Configure, final Release / destructor" --> RT

    IO -. "libzmq internal ZAP exchange" .-> ZAP
    IO -. "libzmq transport events" .-> MON
    ZAP -. "TODO: authoritative platform lookup" .-> PLATFORM
    ZAP -. "Bind transport and initialize connection" .-> REG
    MON -. "Post disconnect to I/O owner for correlation" .-> REG
    REG -. "CONNECTED / drain / DISCONNECTED<br/>Separate lifecycle control path" .-> POOL

    IO <-->|"Resolve User-Id and original route"| REG
    IO -- "Unregistered service including string calls<br/>UNSUPPORTED_SERVICE, no worker dispatch" --> CLIENT
    IO -- "Retain current supported request" --> CURRENT
    CURRENT -- "Reserve worker and hand off without blocking" --> POOL
    POOL -- "Start/capacity wakeup: resume receiving" --> IO
    CURRENT -- "10-second timeout: INTERNAL and exhaustion log" --> IO
    POOL <-->|"Crypto operations and NotifyAppState"| AD
    POOL <-->|"AllocateSharedBuffer / ReleaseSharedBuffer<br/>Deferred cleanup"| BUF
    AD <-->|"TryUse / FinishUse for bulk operations"| BUF
    AD <-->|"Query via IShell, invoke, Release"| CRYPTO
    POOL -- "RPC job result or mapped error" --> OUT
    OUT -- "Wake owner, check original route, one send attempt" --> IO
```

Solid arrows show API calls and RPC data flow. Dashed arrows show authentication,
monitor events and connection lifecycle control. The ZAP and monitor arrows are
libzmq's internal exchanges; they do not mean the ROUTER owner uses REP or PAIR.
This is a component/call map; the sequence diagrams below define ordering and
failure handling.

| Call or event entering Gateway | Internal path and result | Detailed flow |
| --- | --- | --- |
| Thunder Initialize / Deinitialize | Wrapper acquires/configures the implementation; deactivation releases its sole interface reference and awaits destructor completion | Sections 6.4 and 6.16 |
| client_connect | DEALER handshake reaches ZAP; bind a unique User-Id to the transport/context and settle the worker-side NotifyAppState(CONNECTED) attempt before ZAP success. Backend failure is logged separately and does not reject the transport | Section 6.5 |
| client_send_request / client_send_request_async carrying a Crypto RpcRequest | ROUTER owner validates/adopts the current request, pauses further receives while waiting up to 10 seconds for a worker, then dispatches or returns INTERNAL; completions use the same owner | Sections 6.6–6.9 |
| client_send_request_string / client_send_request_string_async | ROUTER rejects the unrouted service=0 envelope with RPC_STATUS_UNSUPPORTED_SERVICE before worker admission; client exposes the received status as text | Sections 5.8–5.9 and 6.6 |
| ALLOCATE_SHARED_BUFFER / RELEASE_SHARED_BUFFER | Worker calls SharedBufferManager locally. Allocation returns buffer_id, name and size; release acceptance blocks new use and defers physical cleanup while access remains active or unresolved | Sections 6.10–6.11 and 6.13 |
| ENCRYPT / DECRYPT / SIGN / VERIFY | Worker-side adapter acquires an execution-time buffer lease, calls ICrypto using trusted context/object metadata, and ends the lease when backend access is definitively complete | Section 6.12 |
| Other Crypto operations | Adapter queries the configured callsign through IShell, invokes the mapped ICrypto method and releases the reference immediately after that call | Sections 6.7–6.9 |
| Peer closure after client_disconnect or unexpected transport loss | PAIR event identifies the original connection; immediately close its admission, drain accepted work, notify DISCONNECTED with its ApplicationContext through the private pool and finalize releasable resources; no grace period or wait for another connection to close | Section 6.14 |

### 6.2 Thread model and private Thunder pool

Contract requirements: [GR-01 Socket owners and private request workers](../openspec/specs/gateway-routing/spec.md#requirement-gr-01-socket-owners-and-private-request-workers).

Coverage: **AC1.1–AC1.3, AC1.7**.

#### Current request, receive pause and worker timeout

Contract requirements: [GR-02 Current-request admission and worker-start deadline](../openspec/specs/gateway-routing/spec.md#requirement-gr-02-current-request-admission-and-worker-start-deadline),
[DG-02 Worker capacity and retained-cleanup diagnostics](../openspec/specs/diagnostics/spec.md#requirement-dg-02-worker-capacity-and-retained-cleanup-diagnostics).

For example, `reason=worker_pool_exhausted workers=4 running=4 reserved=0
wait_ms=10000`, followed by request ID, service and connection generation,
identifies a request that could not start while all four workers were occupied.

#### Initial resource policy

Contract requirements: [GR-03 Resource and transport policy](../openspec/specs/gateway-routing/spec.md#requirement-gr-03-resource-and-transport-policy).

This revised sizing decision supersedes the shared-memory proposal's section
6.3 limits and examples requiring larger allocations. The proposal's ownership,
error and deferred-release contracts still apply. A buffer's capacity includes
both input and output; it is not a plaintext-size allowance. There is no global
shared-memory cap. Backing-space preallocation must still succeed or return
CRYPTO_ERROR_OPERATION_FAILED. Request/response messages retain the Unlimited
policy; the shared-buffer limit must not become a transport message-size cap.

### 6.3 Runtime records and synchronization

Contract requirements: [GL-02 Runtime records and synchronization](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-02-runtime-records-and-synchronization).

### 6.4 Startup and partial-start failure

Contract requirements: [GL-03 Startup, configuration failures and rollback](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-03-startup-configuration-failures-and-rollback).

Coverage: **AC1.1–AC1.3, AC1.7**.

```mermaid
sequenceDiagram
    autonumber
    participant T as Thunder
    participant P as EyilGateway wrapper
    participant I as EyilGatewayImplementation
    participant W as Private pool
    participant Z as ZAP worker
    participant M as Monitor worker
    participant R as Gateway I/O owner (ROUTER)
    T->>P: Initialize(IShell)
    P->>T: AddRef shell, Register notification
    P->>T: Root(IConfiguration, EyilGatewayImplementation)
    T-->>P: Implementation interface
    P->>I: Configure(IShell) through IConfiguration
    I->>I: Validate configuration, create fresh generation/context and stop wakeups
    I->>I: Run bounded private-IPC libzmq self-test, fail startup on any mismatch
    I->>W: Construct/start private request pool
    I->>Z: Start REP, bind inproc ZAP endpoint
    Z-->>I: Authenticator ready or startup failure
    I->>R: Create ROUTER, check required libzmq capability, set PLAIN server, default HWM and MAXMSGSIZE=-1
    R->>R: Set heartbeat IVL/TIMEOUT/TTL=0, HANDOVER=0, MANDATORY=1 and LINGER=0
    Note over I,R: Transport heartbeats disabled, matching the client
    R->>R: Enable Gateway monitor endpoint
    I->>M: Start PAIR collector
    M-->>I: Collector ready
    I->>R: Bind ipc:///tmp/app-eyil-gateway.ipc
    R-->>I: Listener ready or bind failure
    alt All required runtime parts ready
        I->>I: Publish RUNNING and open authentication/request admission
        I-->>P: Configure succeeds
        P-->>T: Initialization succeeds
    else Any startup step fails
        I->>I: Destroy partial runtime, close/join only resources that started
        I-->>P: Configure fails
        P-->>T: Initialization error
        T->>P: Deinitialize after failed Initialize
        P->>P: Release implementation and framework references
    end
```

### 6.5 ZAP, interim identity and future AppManager lookup

Contract requirements: [CI-02 Independent interim connection contexts](../openspec/specs/connection-identity/spec.md#requirement-ci-02-independent-interim-connection-contexts),
[CY-03 Authentication-time CONNECTED and session bookkeeping](../openspec/specs/crypto-integration/spec.md#requirement-cy-03-authentication-time-connected-and-session-bookkeeping).

Coverage: **AC1.2, AC1.4–AC1.6; AC2-Client.2–AC2-Client.3**.

```mermaid
sequenceDiagram
    autonumber
    participant C as Client DEALER
    participant L as Gateway libzmq
    participant Z as ZAP REP worker
    participant A as AuthenticationProvider
    participant AM as AppManager - future
    participant RM as RuntimeManager - future
    participant REG as ConnectionRegistry
    participant W as Worker (private pool), lifecycle job
    participant T as Thunder IShell
    participant CP as Crypto plugin / ICrypto
    C->>L: Direct UDS connection, dummy PLAIN credentials
    L->>Z: ZAP credentials and trusted accepted-FD/transport-lifetime binding
    Z->>Z: Validate ZAP frames/mechanism, parse UID/GID/host PID
    Z->>A: Resolve peer identity
    alt Initial phase - no platform lookup
        A->>A: Generate appId eyil-interim:unique-token and separate sessionId
        A-->>Z: Peer context, all supported APIs allowed
    else Future TODO(AUTH-APPMANAGER)
        A->>AM: Formal UID lookup API - not yet integrated
        AM->>RM: Resolve authoritative app identity/entitlements
        RM-->>AM: Authoritative result
        AM-->>A: Identity and API authorization
        A-->>Z: Allow with context, or deny
    end
    alt Valid peer and runtime admission open
        Z->>REG: Bind transport lifetime to User-Id, create AUTHENTICATING context
        Z->>W: Schedule CONNECTED, no registry lock held
        opt No private worker is free yet
            Note over Z,W: ZAP waits for a worker, bound pending under<br/>AUTHENTICATION-PROGRESS, later handshakes wait behind it
        end
        W->>T: Query ICrypto using configured callsign
        T-->>W: Scoped interface reference or nullptr
        alt Lookup returns nullptr
            W-->>Z: Initialization failure, no notification dispatched
        else Interface acquired
            W->>CP: NotifyAppState(context, CONNECTED)
            CP-->>W: Invocation and service result
            W->>CP: Release reference on every completed call path
            W-->>Z: Retained initialization result
        end
        alt Notification attempt settled, same transport live and admission open
            Z->>REG: Publish ACTIVE context
            Z-->>L: ZAP success with unique User-Id
            L-->>C: Authenticated transport
        else Disconnect, deactivation or local setup failure wins
            Z->>REG: Close admission, retain required cleanup state
            Z-->>L: ZAP service error if exchange still open
            Note over REG,W: If CONNECTED succeeded, perform DISCONNECTED after work ends
        end
    else Malformed/untrusted peer or future denial
        Z-->>L: ZAP failure, no admitted session
        L-->>C: Handshake failure
    end
```

Platform activation order establishes availability, not a bound on the duration
of NotifyAppState(CONNECTED). The single ZAP REP exchange waits for its lifecycle
job before replying, so a slow call can delay subsequent authentication. The
client's 15-second connect timeout does not cancel that job. Admission waiting,
lifecycle-worker capacity and late-result cleanup are tracked under
AUTHENTICATION-PROGRESS in section 7.2; no backend execution timeout or forced
thread cancellation is introduced by the current design.

### 6.6 Request ingress, dispatch and response delivery

Contract requirements: [GR-02 Current-request admission and worker-start deadline](../openspec/specs/gateway-routing/spec.md#requirement-gr-02-current-request-admission-and-worker-start-deadline),
[RP-02 Status layers and correlated Gateway failures](../openspec/specs/rpc-protocol/spec.md#requirement-rp-02-status-layers-and-correlated-gateway-failures),
[GR-04 Ingress validation and accepted ownership](../openspec/specs/gateway-routing/spec.md#requirement-gr-04-ingress-validation-and-accepted-ownership).

Coverage: **AC1.1, AC1.5–AC1.11; AC2-Client.5**.

```mermaid
sequenceDiagram
    autonumber
    participant C as Client DEALER
    participant R as Gateway I/O owner (ROUTER)
    participant REG as ConnectionRegistry
    participant CURRENT as Current request, no queue
    participant P as Private Thunder pool
    participant J as Worker (private pool), RequestJob
    participant S as Service adapter / buffer manager
    participant OUT as Completion handoff
    Note over R,CURRENT: Receive a new RPC only when no current request awaits dispatch
    C->>R: Routing frame plus RpcRequest
    R->>REG: Resolve ACTIVE context, transport binding and runtime admission
    REG-->>R: Retained context or rejection
    R->>R: Validate framing, envelope, schema, service and request ID
    alt Duplicate ID on this connection generation
        R->>R: Log ERROR, keep original completion and discard duplicate
    else Invalid request or admission closed
        R->>R: Log exact rejection reason
        R-->>C: Mapped RpcStatus when safely correlatable and deliverable
    else Service has no registered handler, including service 0
        R->>R: Log unsupported_service, no worker or service call
        R-->>C: UNSUPPORTED_SERVICE, echoed ID/service, schema 1, empty payload
    else Supported request
        R->>CURRENT: Retain owned job/context/completion state, deadline admission plus 10 seconds
        alt Preparation allocation fails
            R->>R: Release partial state and log ERROR
            R-->>C: RPC_STATUS_INTERNAL when deliverable, no service execution
        else Current request ready
            R->>P: Try reserve available worker and hand off without blocking
            opt No worker can start yet
                R->>R: Pause further ROUTER receives, disable receive poll interest
                Note over C,R: While paused, no connection's requests are read,<br/>including requests that would need no worker
                Note over R,P: Continue replies, control/monitor events and deadline processing
                P-->>R: Capacity change signals existing wakeup
                R->>P: Retry eligible handoff within the original deadline
            end
            alt Deadline wins before Dispatch claims RUNNING
                R->>CURRENT: Atomically mark EXPIRED
                R->>R: Log worker_pool_exhausted with worker counts, wait and request identity
                R-->>C: One RPC_STATUS_INTERNAL response attempt, empty payload
                Note over CURRENT,J: A late handoff observes EXPIRED and never calls the service
                R->>CURRENT: Retire after handoff ownership settles
                R->>R: Resume receiving the next request
            else Worker claims RUNNING before deadline
                P->>J: Dispatch distinct job, disarm worker-wait timer
                J-->>R: Start acknowledgement, current ingress record can be released
                R->>R: Resume receiving another request
                J->>J: Validate service payload and operation/oneof
                alt Invalid service structure
                    J->>OUT: RPC_STATUS_INTERNAL with empty payload
                else Service request valid
                    J->>S: Gateway-local operation or scoped ICrypto invocation
                    S-->>J: Service result or mapped invocation failure
                    J->>OUT: Owned response with original context/generation
                end
                Note over J,OUT: No response slot/byte cap<br/>Allocation failure uses existing error/drop rules
                OUT-->>R: Wake completion drain
                R->>REG: Verify original authenticated transport lifetime
                alt Original route remains usable
                    R->>R: Set expected lifetime and attempt multipart response once with DONTWAIT
                    alt Complete response accepted by libzmq
                        R-->>C: Queued transport delivery, no receipt guarantee
                    else Send fails
                        R->>R: Log send error and drop, no retry or second response
                    end
                else Original route closed or replaced
                    R->>R: Discard stale response
                end
                R->>R: Release completion storage/accounting once
            end
        end
    end
```

#### Response ownership and completion handoff

Contract requirements: [GR-05 Completion handoff](../openspec/specs/gateway-routing/spec.md#requirement-gr-05-completion-handoff).

#### One response send attempt

Contract requirements: [GR-06 Single nonblocking response attempt](../openspec/specs/gateway-routing/spec.md#requirement-gr-06-single-nonblocking-response-attempt).

The 10-second Gateway deadline bounds the current admitted request's wait for
a worker, not backend execution or reply delivery. Adding a backend execution
deadline would not make a dropped error reply observable to the client, cancel an in-process
ICrypto call or prove that shared memory is no longer in use. The selected
contract introduces no such execution deadline. Bounded retention after an
undelivered async response would require a separately agreed client retirement
policy or connection teardown; it cannot be guaranteed by a Gateway timer alone.

### 6.7 Crypto adapter: per-call ICrypto references and per-connection sessions

Contract requirements: [SDK-02 Compatible build and runtime dependencies](../openspec/specs/sdk-installation/spec.md#requirement-sdk-02-compatible-build-and-runtime-dependencies),
[CY-01 Configured backend list and availability](../openspec/specs/crypto-integration/spec.md#requirement-cy-01-configured-backend-list-and-availability).

Coverage: **AC1.8, AC1.12**.

#### Callsign configuration: BitBake to CMake to runtime

Contract requirements: [CY-01 Configured backend list and availability](../openspec/specs/crypto-integration/spec.md#requirement-cy-01-configured-backend-list-and-availability),
[GL-03 Startup, configuration failures and rollback](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-03-startup-configuration-failures-and-rollback),
[GL-01 Wrapper and implementation ownership](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-01-wrapper-and-implementation-ownership).

```cmake
set(PLUGIN_EYILGATEWAY_CRYPTO_CALLSIGN ""
    CACHE STRING "Optional Crypto backend callsign"
)
```

```bitbake
EYILGATEWAY_CRYPTO_CALLSIGN ?= ""
EXTRA_OECMAKE += "-DPLUGIN_EYILGATEWAY_CRYPTO_CALLSIGN=${EYILGATEWAY_CRYPTO_CALLSIGN}"
```

The generated Thunder configuration carries a list of service/callsign entries:

```json
{
    "callsign": "org.rdk.EyilGateway",
    "configuration": {
        "backends": [
            { "service": "crypto", "callsign": "" }
        ],
        "workers": 4
    }
}
```

#### Interface ownership

Contract requirements: [CY-02 Scoped Thunder interface references](../openspec/specs/crypto-integration/spec.md#requirement-cy-02-scoped-thunder-interface-references).

```mermaid
sequenceDiagram
    participant J as Worker (private pool), operation or lifecycle job
    participant A as CryptoServiceAdapter
    participant T as Thunder IShell
    participant C as Crypto plugin / ICrypto
    J->>A: Method, trusted ApplicationContext and owned inputs
    Note over J,A: Satisfy required per-connection ordering before acquiring ICrypto
    A->>T: QueryInterfaceByCallsign(ICrypto ID, configured Crypto backend callsign)
    T-->>A: Caller-owned interface reference or nullptr
    alt Lookup returns nullptr
        A-->>J: RPC_STATUS_INTERNAL, no Crypto invocation
    else Interface acquired
        A->>A: Retain local raw interface pointer for this invocation
        A->>C: Requested method or NotifyAppState(context, state)
        C-->>A: Core result and method outputs
        A->>C: Release immediately after invocation on every path
        alt Invocation succeeded
            A->>A: Preserve CryptoError and copy valid outputs
        else Invocation failed
            A->>A: Ignore unspecified outputs, prepare RPC_STATUS_INTERNAL
        end
        A-->>J: Owned result or mapped failure, no retained ICrypto pointer
    end
```

#### Application context is separate from the interface reference

Contract requirements: [CY-03 Authentication-time CONNECTED and session bookkeeping](../openspec/specs/crypto-integration/spec.md#requirement-cy-03-authentication-time-connected-and-session-bookkeeping).

For example, GenerateKey returns a key ID, Gateway releases its ICrypto
reference, and a later Sign obtains another reference and passes the same
ApplicationContext and key ID. Crypto must preserve that session's state across
these independent interface references. Releasing an interface must not invoke
NotifyAppState(DISCONNECTED) or discard the app's keys.

### 6.8 Crypto operation routing map

Contract requirements: [CY-04 Crypto operation destinations and conversions](../openspec/specs/crypto-integration/spec.md#requirement-cy-04-crypto-operation-destinations-and-conversions).

#### Client and Gateway boundaries in the Crypto sequences

Contract requirements: [CY-05 Service consumer and backend policy boundary](../openspec/specs/crypto-integration/spec.md#requirement-cy-05-service-consumer-and-backend-policy-boundary),
[GR-02 Current-request admission and worker-start deadline](../openspec/specs/gateway-routing/spec.md#requirement-gr-02-current-request-admission-and-worker-start-deadline),
[GR-06 Single nonblocking response attempt](../openspec/specs/gateway-routing/spec.md#requirement-gr-06-single-nonblocking-response-attempt),
[SM-05 Execution-time access and definitive completion](../openspec/specs/shared-memory/spec.md#requirement-sm-05-execution-time-access-and-definitive-completion),
[RP-02 Status layers and correlated Gateway failures](../openspec/specs/rpc-protocol/spec.md#requirement-rp-02-status-layers-and-correlated-gateway-failures),
[DG-01 Severity, context and confidentiality](../openspec/specs/diagnostics/spec.md#requirement-dg-01-severity-context-and-confidentiality).

### 6.9 Non-shared-memory operations

Contract requirements: [CY-05 Service consumer and backend policy boundary](../openspec/specs/crypto-integration/spec.md#requirement-cy-05-service-consumer-and-backend-policy-boundary).

Coverage: **AC1.8; AC2-Firebolt.1–AC2-Firebolt.2**. This flow applies to the eleven key/random
operations above; allocation/release and the four bulk-data operations have
separate flows.

```mermaid
sequenceDiagram
    autonumber
    participant F as Firebolt Crypto / consumer
    participant L as EyilClient
    participant R as Gateway I/O owner (ROUTER)
    participant J as Worker (private pool), Crypto adapter
    participant T as Thunder IShell
    participant C as Crypto plugin / ICrypto
    F->>F: Serialize CryptoRequest inside RpcRequest with service CRYPTO
    F->>L: client_send_request or client_send_request_async with envelope bytes
    L->>R: RpcRequest(service=CRYPTO, schema=1)
    R->>J: Submit retained context and request
    J->>J: Validate operation, parameters and entitlement, satisfy context ordering
    J->>T: Query ICrypto using the configured Crypto backend callsign
    T-->>J: Caller-owned interface reference or nullptr
    alt Lookup returns nullptr
        J-->>R: RPC_STATUS_INTERNAL
    else Scoped reference acquired
        J->>C: LoadKey / GenerateKey / other mapped method with ApplicationContext
        C->>C: Enforce key policy and perform operation
        C-->>J: Core result, CryptoError and output values
        J->>C: Release immediately after invocation on every result path
        J->>J: Check Core result, preserve CryptoError and copy defined outputs
        J->>J: Encode owned CryptoResponse or mapped invocation failure
        J-->>R: Completion with original request ID/generation
    end
    R-->>L: RpcResponse through original connection
    L-->>F: Serialized result to matching waiter/callback
    F->>F: Inspect RpcStatus and CryptoError, decode result
```

### 6.10 Shared-memory identity, records and invariants

Contract requirements: [SM-01 Allocation identity and trusted application cooperation](../openspec/specs/shared-memory/spec.md#requirement-sm-01-allocation-identity-and-trusted-application-cooperation).

Coverage: **AC1.9–AC1.12**. Allocation is a Gateway responsibility; mapping by
the application and access by Crypto are separate responsibilities.

#### Application quota shared across connections

Contract requirements: [SM-02 Shared trusted-host-UID quota](../openspec/specs/shared-memory/spec.md#requirement-sm-02-shared-trusted-host-uid-quota),
[SM-01 Allocation identity and trusted application cooperation](../openspec/specs/shared-memory/spec.md#requirement-sm-01-allocation-identity-and-trusted-application-cooperation).

For example, connections A and B of one UID may hold two buffers each. A fifth
allocation from A, B or a newly opened connection C fails with
RPC_STATUS_OK and CRYPTO_ERROR_OPERATION_FAILED. It becomes possible after one
allocation completes cleanup. A different UID has its own four-slot allowance;
there is no Gateway-wide shared-memory cap.

The proposal's namespace path uses the peer's mount view. Access is subject to
procfs checks and can become unavailable when the peer or its main thread exits.
See [proc_pid_root](https://man7.org/linux/man-pages/man5/proc_pid_root.5.html).
The selected trust model requires the app to preserve that name/object binding.
Retaining an object FD alone does not prevent replacement of its pathname.
Keep the proposal's app-namespace backend path for this version. The discussed
`/proc/<gateway-pid>/fd/<fd>` alternative is not selected: it would require
different procfs/open rules and still would not prove that the correct peer
namespace was captured initially or prevent modification/truncation. The
existing path remains subject to backend-completion validation in section 7.1.

**PID reuse edge case:** if the peer exits and its PID has not been reused,
opening `/proc/<pid>/root/dev/shm` fails; log ERROR and return the existing
allocation-failure result. Linux can reuse a freed PID after the allocation
counter wraps at [pid_max](https://docs.kernel.org/admin-guide/sysctl/kernel.html#pid-max),
so the path could then refer to another process. Retained directory/object FDs
protect resources only after capture.

#### Region presence and overlap rules

Contract requirements: [SM-03 Region presence, overlap and output validation](../openspec/specs/shared-memory/spec.md#requirement-sm-03-region-presence-overlap-and-output-validation),
[SM-02 Shared trusted-host-UID quota](../openspec/specs/shared-memory/spec.md#requirement-sm-02-shared-trusted-host-uid-quota).

#### Application cleanup after completion or error

Contract requirements: [SM-08 Application release and abandoned allocations](../openspec/specs/shared-memory/spec.md#requirement-sm-08-application-release-and-abandoned-allocations).

#### Application/container access and failure limits

Contract requirements: [CY-06 Backend shared-buffer access contract](../openspec/specs/crypto-integration/spec.md#requirement-cy-06-backend-shared-buffer-access-contract).

The app opens its allocation with O_RDWR and maps it with MAP_SHARED. Its
mapping is writable without going through EyilClient or Gateway. A client-side
mutex or an advisory in-use flag can coordinate cooperating callers, but cannot
stop another app thread from writing, truncating or unlinking the object using
ordinary memory access or system calls. A container's private /dev/shm separates
its namespace; it does not remove the app's access to its own allocation.

| App action | Effect and error boundary |
| --- | --- |
| Close its FD or unmap its own view | Does not invalidate an FD or mapping already held by Gateway or Crypto; no Crypto failure is implied |
| Unlink before Crypto opens the object | A later open can fail; Crypto handles that failure and returns its service result |
| Unlink after Crypto has opened/mapped the object | Existing references remain usable; unlink alone does not cancel the operation |
| Replace the name | A later open may resolve a different object; failure is not guaranteed merely because Gateway still holds the original FD |
| Rewrite input through its mapping during execution | Crypto may consume changed or inconsistent data without detecting an error |
| Shrink an object already mapped by Crypto | Access beyond its new end can raise SIGBUS; this is not automatically a returned CryptoError |

These are Linux shared-memory semantics, described by
[shm_open/shm_unlink](https://man7.org/linux/man-pages/man3/shm_open.3.html),
[mmap](https://man7.org/linux/man-pages/man2/mmap.2.html) and the shared-memory
hazards in [memfd_create](https://man7.org/linux/man-pages/man2/memfd_create.2.html).
They do not require Gateway to preserve an invalid request until it succeeds.

EyilClient cannot revoke another process's mapping, and Gateway changing file
permissions does not revoke existing writable mappings. Additional containment
of a violating app would be a separate platform/backend requirement. It is not
part of the selected path/name-based ICrypto integration.

### 6.11 AllocateSharedBuffer and client mapping

Container-owned tmpfs creation and cleanup use
[SM-10 Scoped private-worker filesystem identity](../openspec/specs/shared-memory/spec.md#requirement-sm-10-scoped-private-worker-filesystem-identity).
The host's root UID may be unmapped in the container's tmpfs user namespace;
the private worker uses the trusted mapped UID/GID and restores its filesystem
credentials and capability state before returning to other work.

Contract requirements: [SM-04 Exclusive allocation and backing-space reservation](../openspec/specs/shared-memory/spec.md#requirement-sm-04-exclusive-allocation-and-backing-space-reservation).

Coverage: **AC1.9–AC1.11; AC2-Client.5; AC2-Firebolt.1–AC2-Firebolt.2**. No ICrypto call is involved.

Preallocation does not prevent later app truncation, hole punching, mutation,
or unrelated memory pressure. Keep the cooperation and lease rules; this is
not a claim that all SIGBUS/OOM causes are removed. Validate tmpfs support and
memory-cgroup charging on the product: pages allocated by Gateway need not be
charged to the app's cgroup. EyilClient does not allocate or preallocate these
objects.

```mermaid
sequenceDiagram
    autonumber
    participant A as App-side Crypto / buffer helper
    participant L as EyilClient
    participant R as Gateway I/O owner (ROUTER)
    participant J as Worker (private pool)
    participant B as SharedBufferManager
    participant Q as Shared quota record keyed by host UID
    participant K as Linux / peer private dev-shm
    A->>A: Put AllocateSharedBufferRequest in CryptoRequest and RpcRequest
    A->>L: Byte request API, service CRYPTO
    L->>R: RpcRequest through DEALER IPC
    R->>J: Dispatch with retained connection context
    J->>B: Allocate(connection owner, trusted host UID, requested_size)
    B->>B: Validate requested size
    alt Requested size is zero
        B->>B: Log ERROR, create no object
        B-->>J: CRYPTO_ERROR_INVALID_PARAMETERS
    else Capacity exceeds 1.5 MiB (1572864 bytes) per-buffer limit
        B->>B: Log ERROR with size and limit, create no object
        B-->>J: CRYPTO_ERROR_OVERSIZED_INPUT_DATA
    else Size valid
        B->>B: Prepare cleanup record and retain shared quota record
        B->>Q: Atomically reserve one of four slots and actual capacity across all connections
        Q-->>B: Reservation result
        alt UID quota exhausted or allocation bookkeeping unavailable
            B->>B: Roll back partial reservation and log ERROR
            B-->>J: CRYPTO_ERROR_OPERATION_FAILED
        else Shared UID quota reserved
            B->>K: Retain peer shm directory FD, create exclusive object relative to it
            B->>K: ftruncate then fallocate full capacity with Gateway-only access
            B->>K: Set approved peer ownership/permissions, verify final size
            alt Creation, sizing, preallocation or ownership/permission setup fails
                B->>B: Log ERROR, clean up object and FD<br/>Return UID quota charge only after cleanup completes
                B-->>J: CRYPTO_ERROR_OPERATION_FAILED, no published buffer ID
            else Connection closed before publication
                B->>B: Roll back unpublished object, retain cleanup state on failure
                B-->>J: No usable allocation, retire through connection-close handling
            else Object prepared and connection still admits publication
                K-->>B: Retained object/directory references and verified capacity
                B->>B: Publish buffer_id, client name, host path, capacity, owner and quota reference
                B-->>J: CRYPTO_ERROR_NONE and AllocateSharedBufferResponse
            end
        end
    end
    opt Service result is safely correlatable and response storage is available
        J->>J: Build CryptoResponse with allocation oneof and exact CryptoError
        J-->>R: RPC_STATUS_OK plus serialized CryptoResponse
        R-->>L: Single response attempt through original connection
        L-->>A: Complete serialized RpcResponse if still pending and delivered
        A->>A: Inspect RpcStatus and CryptoError
        alt CRYPTO_ERROR_NONE
            A->>A: Decode buffer_id, client name and allocated size
            A->>K: shm_open(name, O_RDWR), inspect size, mmap(MAP_SHARED)
            K-->>A: Application mapping
        else Service allocation failure
            A->>A: No usable allocation or buffer to map
        end
    end
    Note over J,R: Response construction failure uses section 4.2 infrastructure fallback<br/>Response loss never removes Gateway cleanup ownership
```

### 6.12 Shared-memory operations: common backend boundary

The operation table referenced by this sequence is preserved in
[SM-03 Region presence, overlap and output validation](../openspec/specs/shared-memory/spec.md#requirement-sm-03-region-presence-overlap-and-output-validation).

Contract requirements: [SM-05 Execution-time access and definitive completion](../openspec/specs/shared-memory/spec.md#requirement-sm-05-execution-time-access-and-definitive-completion),
[CY-06 Backend shared-buffer access contract](../openspec/specs/crypto-integration/spec.md#requirement-cy-06-backend-shared-buffer-access-contract),
[SM-03 Region presence, overlap and output validation](../openspec/specs/shared-memory/spec.md#requirement-sm-03-region-presence-overlap-and-output-validation).

The four shared-memory operations follow one flow, drawn once below. Each
invocation acquires its own scoped ICrypto reference. The CONNECTED attempt has
already settled during authentication; null lookup and invocation failures follow
sections 6.7 and 6.15. The table lists what differs per operation. Each
`break` block ends the job early; its error completion still reaches the client
through the same single send attempt as the final response.

Coverage: **AC1.8–AC1.11; AC2-Firebolt.1–AC2-Firebolt.2**.

```mermaid
sequenceDiagram
    autonumber
    participant A as App-side Crypto / mapped buffer
    participant L as EyilClient
    participant R as Gateway I/O owner (ROUTER)
    participant J as Worker (private pool), Crypto adapter
    participant B as SharedBufferManager
    participant T as Thunder IShell
    participant C as Crypto plugin / ICrypto
    A->>A: Write the operation's input regions (see table)
    A->>A: Wrap Op request (buffer_id, algorithm, key_id, regions)<br/>in CryptoRequest and RpcRequest
    A->>L: Byte request API with serialized RpcRequest, service CRYPTO
    L->>R: RpcRequest over DEALER IPC
    R->>J: Dispatch independent job (section 6.6)
    J->>B: TryUse(owner, buffer_id, regions) at execution time
    break Missing, unowned, released, release-pending or busy buffer
        B->>B: Log ERROR with exact cause, owner and request ID
        B-->>J: CRYPTO_ERROR_INVALID_SESSION_STATE, no active-use lease
        J-->>R: RPC_STATUS_OK with CryptoResponse error, no Crypto call or retry
    end
    break Missing, out-of-bounds or overlapping input region (section 6.10)
        B->>B: Log ERROR for the failed presence, bounds or overlap check
        B-->>J: CRYPTO_ERROR_INVALID_PARAMETERS, no active-use lease
        J-->>R: RPC_STATUS_OK with CryptoResponse error, no Crypto call
    end
    B-->>J: Idle owned allocation claimed atomically, retained object identity and capacity
    J->>J: Check operation-specific input limits
    break Input exceeds the operation maximum
        J->>B: FinishUse for the unused lease, finalize pending release if eligible
        J->>J: Log ERROR with input length and operation limit
        J-->>R: RPC_STATUS_OK with CRYPTO_ERROR_OVERSIZED_INPUT_DATA, no Crypto call
    end
    J->>T: Query ICrypto using the configured Crypto backend callsign
    T-->>J: Scoped reference or nullptr
    break Lookup returns nullptr
        J->>B: FinishUse for the unused lease, finalize pending release if eligible
        J->>J: Log ERROR for unavailable ICrypto
        J-->>R: RPC_STATUS_INTERNAL
    end
    J->>C: Op(context, buffer, ...) as listed in the table
    C->>C: Open/check/map, enforce key policy, perform the operation
    C->>C: Unmap and close before normal return
    C-->>J: Invocation result, CryptoError, output region or status
    J->>C: Release the scoped reference after the call returns
    alt Core invocation failed, backend access uncertain
        J->>B: Retain lease and any pending release
        J->>J: Log ERROR and ignore undefined outputs
        J-->>R: RPC_STATUS_INTERNAL, no automatic replay
    else Core invocation failed, access definitively ended or never started
        J->>B: FinishUse, finalize pending release if eligible
        J->>J: Log ERROR and ignore undefined outputs
        J-->>R: RPC_STATUS_INTERNAL, no automatic replay
    else Invocation completed
        J->>J: Preserve CryptoError, log a returned service failure
        J->>J: Validate a successful output region,<br/>or preserve the BUFFER_OVERFLOW required region
        J->>B: FinishUse after definitive completion, finalize pending release if eligible
        J-->>R: Completion with the operation's response
    end
    R-->>L: RpcResponse, one send attempt
    L-->>A: Matched RpcResponse bytes through waiter or callback
    A->>A: Check RpcStatus and CryptoError, decode the output,<br/>read output regions only on success
```

### 6.13 ReleaseSharedBuffer and conflicting buffer operations

Coverage: **AC1.9–AC1.12**. Release is a Gateway-local operation.

#### Release acceptance and deferred destruction

Contract requirements: [SM-06 Release acceptance and execution-time conflicts](../openspec/specs/shared-memory/spec.md#requirement-sm-06-release-acceptance-and-execution-time-conflicts).

#### Atomic state and execution-time conflicts

Contract requirements: [SM-06 Release acceptance and execution-time conflicts](../openspec/specs/shared-memory/spec.md#requirement-sm-06-release-acceptance-and-execution-time-conflicts).

#### Release request sequence

```mermaid
sequenceDiagram
    autonumber
    participant A as App-side buffer helper
    participant L as EyilClient
    participant R as Gateway I/O owner (ROUTER)
    participant J as Worker (private pool), release job
    participant B as SharedBufferManager
    participant K as Linux
    A->>A: Stop further use, unmap and close own view
    A->>A: Wrap ReleaseSharedBufferRequest in CryptoRequest and RpcRequest
    A->>L: Byte request API with serialized RpcRequest, service CRYPTO
    L->>R: RpcRequest over IPC
    R->>J: Dispatch release job within its worker-wait deadline
    J->>B: RequestRelease(owner, buffer_id)
    B->>B: Under lock, validate owner and release state
    alt Unknown, unowned or previously released ID
        B->>B: Log ERROR with exact cause, request context and buffer state
        B-->>J: CRYPTO_ERROR_INVALID_SESSION_STATE, existing use or release unchanged
    else First valid release
        B->>B: Set releasePending, exclude new uses, retain cleanup ownership
        alt No active access and lifecycle ordering permits cleanup
            B->>B: Claim finalization once, unlock
            B->>K: unlinkat retained directory and name, close owned object FD
            alt Cleanup succeeds
                B->>B: Remove cleanup record, release quota and directory reference
            else Cleanup fails
                B->>B: Log ERROR, retain unresolved cleanup/accounting, keep ID unavailable
            end
        else Active or unresolved access, or lifecycle cleanup owns finalization
            B->>B: Unlock, retain object, record, directory reference and quota
            Note over J,B: No wait for active use and no cleanup timer
        end
        B-->>J: Accepted, successful empty ReleaseSharedBufferResponse
    end
    J-->>R: RPC_STATUS_OK, CryptoResponse with exact CryptoError and release oneof
    R-->>L: Single RpcResponse attempt with original ID and generation
    L-->>A: Matched RpcResponse bytes if still pending and delivered
    A->>A: Check RpcStatus and CryptoError for acceptance, never reuse a released ID
    Note over A,B: Response loss or timeout does not undo an accepted release
```

The following flow shows release during an active operation on a live connection:

```mermaid
sequenceDiagram
    autonumber
    participant A as Application
    participant R as Gateway I/O owner (ROUTER)
    participant W as Worker (private pool), Crypto operation
    participant F as Worker (private pool), release job
    participant B as SharedBufferManager
    participant C as Crypto plugin / ICrypto
    participant K as Linux
    W->>B: TryUse(B), claim sole active lease
    B-->>W: Retained allocation
    W->>C: Scoped ICrypto invocation using B
    Note over W,C: Invocation remains in progress
    A->>R: ReleaseSharedBuffer(B), separate request ID
    R->>F: Dispatch release job
    F->>B: RequestRelease(B)
    B->>B: Set releasePending, keep active lease and quota
    B-->>F: Accepted without waiting for Crypto
    F-->>R: RPC_STATUS_OK, CRYPTO_ERROR_NONE, empty release response
    R-->>A: Single response attempt acknowledging accepted release
    Note over B,C: New uses are rejected, backing object remains valid
    C-->>W: Definitive synchronous completion, success or CryptoError
    W->>C: Release scoped interface
    W->>B: FinishUse, activeOperations becomes zero
    B->>B: Claim pending finalization once, unlock
    B->>K: Unlink owned name and close owned object reference
    alt Cleanup succeeds
        B->>B: Remove cleanup record, release quota and directory reference
    else Cleanup fails
        B->>B: Log ERROR and retain private cleanup/accounting
    end
    B-->>W: Active-use obligation settled
    W-->>R: Original operation result, independently of the release response
    R-->>A: Single original-result attempt if route and pending ID still exist
    Note over A,B: No second release response or notification, abandoned buffer regions must not be read
```

The release-before-use case is also intentionally allowed:

```mermaid
sequenceDiagram
    participant A as Application
    participant R as Gateway I/O owner (ROUTER)
    participant E as Worker (private pool), Encrypt
    participant F as Worker (private pool), release job
    participant B as SharedBufferManager
    A->>R: Encrypt(B), request ID 10
    R->>E: Dispatch Encrypt, no buffer reservation before execution
    Note over R,E: The next RPC is read only after Encrypt has started RUNNING
    A->>R: ReleaseSharedBuffer(B), request ID 11
    R->>F: Dispatch Release
    Note over E,F: Release can overtake Encrypt only between Encrypt starting<br/>and its TryUse, for example while it decodes the request
    F->>B: RequestRelease(B), mark idle allocation releasePending
    B->>B: Claim immediate cleanup once, keep ID unavailable
    B-->>F: Release accepted
    F-->>R: Successful release response, ID 11
    E->>B: TryUse(B) when Encrypt reaches its buffer check
    B->>B: Log ERROR for unavailable buffer
    B-->>E: CRYPTO_ERROR_INVALID_SESSION_STATE
    E-->>R: RPC_STATUS_OK with CryptoResponse error, ID 10, no Crypto invocation
    R-->>A: One send attempt for each response using its own request ID
```

#### Completion, cleanup failures and response ownership

Contract requirements: [SM-07 Deferred finalization and cleanup failures](../openspec/specs/shared-memory/spec.md#requirement-sm-07-deferred-finalization-and-cleanup-failures).

#### Coordination with disconnect and plugin drain

Contract requirements: [SM-07 Deferred finalization and cleanup failures](../openspec/specs/shared-memory/spec.md#requirement-sm-07-deferred-finalization-and-cleanup-failures).

### 6.14 Disconnect detection, PID observations and hibernation

Coverage: **AC1.3, AC1.5–AC1.6, AC1.12**.

#### Required connection-to-FD binding

Contract requirements: [CI-03 FD and transport-lifetime binding](../openspec/specs/connection-identity/spec.md#requirement-ci-03-fd-and-transport-lifetime-binding).

#### Rationale: authentication succeeds before the first message

Coverage: **AC1.3, AC1.5–AC1.6, AC1.12**. The source extension supplies the common
connection identity needed for cleanup under the selected authentication-time
session lifecycle. It is not needed merely to obtain an accepted FD.

Unmodified libzmq 4.3.4 already reports the accepted transport FD through
ZMQ_EVENT_ACCEPTED. Its ZAP request exposes the peer credentials, but neither
that FD nor a shared connection-lifetime identifier. A received application
message can provide User-Id and FD together, using message metadata and the
existing ZMQ_SRCFD accessor. That last association exists only if a message
actually reaches the Gateway receiver.
[Monitor events](https://libzmq.readthedocs.io/en/latest/zmq_socket_monitor.html),
[4.3.4 message metadata](https://github.com/zeromq/libzmq/blob/v4.3.4/src/stream_engine_base.cpp),
[4.3.4 ZAP request construction](https://github.com/zeromq/libzmq/blob/v4.3.4/src/zap_client.cpp).

The distinction between connection readiness and first-message delivery is
essential. The transport handshake and ZAP exchange happen during connection
setup. client_connect returns CLIENT_OK after handshake success; this does not
require the application to submit a Crypto RPC or confirm that Gateway has
received any application message. The application may immediately disconnect
one of its Clients while keeping its other connections open. A transport failure
can also occur before its first queued message reaches Gateway.

Gateway already creates an independent ApplicationContext and attempts
NotifyAppState(CONNECTED) during authentication, before ZAP success. When that
notification succeeds, it needs matching cleanup even if no RPC follows. This failure does not require a
shared-memory allocation to exist: the authenticated context and Crypto session
are enough to make targeted disconnect handling necessary.

##### Failure sequence without a shared transport identity

Clients A and B below are two DEALER sockets in the same application process,
with identical UID/GID/PID. The diagram shows B's authentication being processed
first to illustrate independent scheduling. The A/B labels describe the actual
connections for the reader; they are not extra information available to the ZAP
handler. Neither connection sends an application message in this sequence.

```mermaid
sequenceDiagram
    autonumber
    participant A as Client A
    participant B as Client B - same process
    participant T as Gateway libzmq
    participant M as Gateway monitor PAIR
    participant Z as Gateway ZAP handler
    participant R as Gateway registry
    participant C as Private lifecycle worker / ICrypto
    A->>T: Open UDS connection A
    T-->>M: ACCEPTED with FD 42
    M->>R: Record accepted FD 42
    B->>T: Open UDS connection B
    T-->>M: ACCEPTED with FD 43
    M->>R: Record accepted FD 43
    Note over T,R: Monitor and ZAP processing have no shared per-connection key
    T->>Z: ZAP for B, shared peer credentials, no FD or lifetime
    Z->>R: Create context U1 from trusted credentials
    R->>C: Scoped NotifyAppState(context for U1, CONNECTED)
    C-->>R: Completed successfully
    R-->>Z: Context published
    Z-->>T: ZAP 200 with User-Id U1
    T-->>B: Handshake succeeds, client_connect returns CLIENT_OK
    T->>Z: ZAP for A, identical peer credentials, no FD or lifetime
    Z->>R: Create context U2 from trusted credentials
    R->>C: Scoped NotifyAppState(context for U2, CONNECTED)
    C-->>R: Completed successfully
    R-->>Z: Context published
    Z-->>T: ZAP 200 with User-Id U2
    T-->>A: Handshake succeeds, client_connect returns CLIENT_OK
    Note over A,R: No received RPC or probe has associated either User-Id with an FD
    B->>T: Close only connection B before its first message arrives
    T-->>M: DISCONNECTED with FD 43
    M->>R: Resolve FD 43 to its authenticated context
    Note over R,C: U1 and U2 share credentials, so the registry cannot safely choose one
    Note over A,R: Closing both would invalidate live A<br/>Retaining both would leave B's session unreclaimed
```

Adding UID/GID/PID to the disconnect event identifies the process but still
does not distinguish U1 from U2. Pairing the first ACCEPTED event with the first
ZAP request is also insufficient: independent queues and I/O threads do not
provide that ordering contract. The FD can identify the disconnected transport
while the application still lacks its association with the authenticated session.

##### Why an immediate request or automatic probe does not close the gap

ZMQ_PROBE_ROUTER=1 can make DEALER automatically send an empty message when a
connection is established. It defaults to 0. This is a one-time probe, not a
periodic heartbeat; Gateway would have to recognize it separately from an RPC.
If received, it can help associate routing identity, User-Id and FD. It is not
enabled by the current client design.
[ZMQ_PROBE_ROUTER](https://libzmq.readthedocs.io/en/latest/zmq_setsockopt.html#zmq-probe-router-bootstrap-connections-to-router-sockets).

Neither a probe nor an application HELLO is guaranteed to reach Gateway before
the connection fails. Successful zmq_send only establishes local queue
acceptance. A successful transport handshake is not an acknowledgement that
Gateway's application loop has consumed the probe. The failure sequence above
therefore also applies when a first message was queued but never received.
[Send completion contract](https://libzmq.readthedocs.io/en/latest/zmq_send.html).

An explicit HELLO/ACK protocol could define a different session-activation
boundary and defer Crypto CONNECTED until Gateway receives HELLO. That would
require a new bootstrap contract and rules for all pre-HELLO state. It would
change the selected requirement to bind the context and notify Crypto during
ZAP. The source extension satisfies that existing requirement without depending
on a new application message. FD and outgoing-route reuse also require lifetime
checks independently of when the first message arrives.

##### Selected sequence with the connection-lifetime extension

Contract requirements: [CI-03 FD and transport-lifetime binding](../openspec/specs/connection-identity/spec.md#requirement-ci-03-fd-and-transport-lifetime-binding).

The extension assigns the lifetime before authentication and carries it through
local ZAP metadata, received messages and monitor events. The registry can bind
the FD to User-Id before acknowledging ZAP. The sequence below shows B closing
without an RPC while A remains live.

```mermaid
sequenceDiagram
    autonumber
    participant B as Client B
    participant T as Gateway libzmq
    participant M as Gateway monitor PAIR
    participant Z as Gateway ZAP handler
    participant R as Gateway registry
    participant C as Private lifecycle worker / ICrypto
    Note over R,C: Client A already has independent context U2, FD 42, lifetime 101
    B->>T: Open UDS connection B
    T->>T: Capture credentials, assign FD 43 and lifetime 102
    T-->>M: ACCEPTED with FD 43 and lifetime 102
    M->>R: Record transport lifetime 102 in the current activation
    T->>Z: ZAP credentials plus local FD 43 and lifetime 102 metadata
    Z->>R: Bind User-Id U1 to FD 43 and lifetime 102
    R->>C: Scoped NotifyAppState(context for U1, CONNECTED)
    C-->>R: Completed successfully
    R-->>Z: Context ready, binding established
    Z-->>T: ZAP 200 with User-Id U1
    T-->>B: Handshake succeeds, client_connect returns CLIENT_OK
    Note over B,R: No application RPC or probe is required to establish this binding
    B->>T: Close connection B before sending any RPC
    T-->>M: DISCONNECTED with FD 43, peer credentials and lifetime 102
    M->>R: Resolve current activation and lifetime 102, verify stored FD
    R->>R: Mark only U1 closing, finish its accepted work if any
    R->>C: Scoped NotifyAppState(context for U1, DISCONNECTED)
    C-->>R: Completed successfully
    R->>R: Retire U1 and its releasable resources
    Note over R,C: Client A and context U2 remain active
    opt A later connection reuses the numeric FD
        T->>T: Reuse FD 43 with a new lifetime 103
        T-->>M: ACCEPTED with FD 43 and lifetime 103
        M->>R: Track the new lifetime independently
        Note over M,R: Old lifetime 102 events or cleanup work cannot select lifetime 103
    end
```

#### Required libzmq extension and platform integration

Contract requirements: [CI-04 Required libzmq extension and guarded replies](../openspec/specs/connection-identity/spec.md#requirement-ci-04-required-libzmq-extension-and-guarded-replies).

#### Startup capability check before admission

Contract requirements: [CI-05 Startup capability verification before admission](../openspec/specs/connection-identity/spec.md#requirement-ci-05-startup-capability-verification-before-admission).

```cpp
uint64_t expected = UINT64_MAX; // Probe before bind; no request can be sent.
if (zmq_setsockopt(router, ZMQ_ROUTER_EXPECTED_CONNECTION_ID,
                  &expected, sizeof(expected)) != 0) {
    return FailStartup("zmq_connection_lifetime_unavailable", zmq_errno());
}
expected = 0;
if (zmq_setsockopt(router, ZMQ_ROUTER_EXPECTED_CONNECTION_ID,
                  &expected, sizeof(expected)) != 0) {
    return FailStartup("zmq_connection_lifetime_reset_failed", zmq_errno());
}
if (zmq_socket_monitor_versioned(router, monitorEndpoint, monitorEvents,
                                 2, ZMQ_PAIR) != 0) {
    return FailStartup("zmq_monitor_v2_unavailable", zmq_errno());
}
// Start the monitor receiver and complete the other startup prerequisites.
// Only then bind the production endpoint and open admission.
```

The extension regression must cover independent connections from one process,
no-request disconnects, rejected/in-progress authentication, closure before ZAP,
FD reuse, context recreation, monitor-v1 compatibility and stale reply rejection
after route reuse. It must also reject replacement of native transport identity
by ZAP metadata. Run it against the selected dependency and runtime configuration;
this specification records required scenarios, not execution results.

**TODO(ZMQ-PLATFORM):** integrate the extension into the product recipe, consume
its properties/events and guarded-send option in Gateway, and verify the
matching headers, library and behavior on the target rootfs. Gateway must still
serialize authentication/disconnect state, retain closing records for late
events and explicitly retire its contexts during local shutdown.

#### Event-driven connection cleanup

Contract requirements: [GL-04 Correlated disconnect and backend notification order](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-04-correlated-disconnect-and-backend-notification-order),
[CC-08 Prepared process freeze and library unload](../openspec/specs/client-connection/spec.md#requirement-cc-08-prepared-process-freeze-and-library-unload),
[CC-04 Socket configuration and disabled heartbeats](../openspec/specs/client-connection/spec.md#requirement-cc-04-socket-configuration-and-disabled-heartbeats),
[CI-03 FD and transport-lifetime binding](../openspec/specs/connection-identity/spec.md#requirement-ci-03-fd-and-transport-lifetime-binding).

The architect's clarification ties DISCONNECTED to detection of each client's
disconnection and its corresponding ApplicationContext. Do not wait for app
termination, the last connection from that app instance, or a reconnect window.
The application decides when to close; keeping one connection across operations
and idle periods for its lifetime is recommended.

The current implementation design starts teardown immediately and drains
accepted work before invoking DISCONNECTED, because that notification may
release Crypto resources still used by a call. Schedule it as soon as that
drain permits. This is the existing active-work safety ordering; the architect's
quoted clarification establishes the trigger and absence of a grace period
but does not explicitly specify ordering against in-flight Crypto calls.
Section 7.1 retains the abnormal-completion boundary. An idle connection has
no accepted work to drain and its notification is immediately eligible.

```mermaid
sequenceDiagram
    autonumber
    participant P as Peer / kernel
    participant Z as libzmq monitor producer
    participant M as Gateway PAIR monitor worker
    participant REG as Registry / Gateway I/O owner (ROUTER)
    participant J as Worker (private pool), accepted and lifecycle jobs
    participant C as Crypto adapter
    participant T as Thunder IShell
    participant CP as Crypto plugin / ICrypto
    participant B as SharedBufferManager
    P->>Z: App exits/crashes, closes DEALER or loses the transport
    Z->>M: DISCONNECTED with FD/lifetime and peer credentials
    M->>REG: Normalize with original activation generation
    REG->>REG: Resolve established transport binding to User-Id
    alt Already retired lifetime
        REG->>REG: Ignore duplicate/stale event without touching a reused FD
    else Original context found
        REG->>REG: Mark CLOSING, remove admission and route visibility
        Note over REG,J: Per-connection teardown starts now, no grace timer or last-connection wait
        REG->>B: Establish lifecycle finalization gate, retain pending releases and quota
        REG->>REG: Drop completed undeliverable responses and release their capacity
        REG->>J: Drain accepted jobs, active uses, claimed finalizers and CONNECTED
        J-->>REG: Jobs settled, including any uncertain buffer-access outcome
        alt Backend access is definitively over
            opt CONNECTED succeeded
                REG->>J: Schedule one DISCONNECTED job for this connection's ApplicationContext
                J->>C: NotifyAppState(context, DISCONNECTED)
                C->>T: Query ICrypto using configured callsign
                T-->>C: Scoped reference or nullptr
                alt Lookup failed
                    C-->>J: Lookup failure, nothing dispatched
                    J-->>REG: Log ERROR notification failure, no background retry
                else Core invocation failed or CryptoError returned
                    C->>CP: NotifyAppState(context, DISCONNECTED)
                    CP-->>C: Failure result
                    C->>CP: Release reference after the call returns
                    C-->>J: Failure result
                    J-->>REG: Log ERROR notification failure, no background retry
                else Notification succeeded
                    C->>CP: NotifyAppState(context, DISCONNECTED)
                    CP-->>C: Success
                    C->>CP: Release reference after the call returns
                    C-->>J: Success
                    J-->>REG: Notification complete
                end
            end
            REG->>B: Mark remaining allocations releasePending, finalize through retained references
            alt Physical cleanup succeeds
                B-->>REG: Resources reclaimed, quota and directory references released once
                REG->>REG: Retire private context and binding when cleanup references end
            else Physical cleanup fails
                B-->>REG: ERROR logged, private cleanup records and unreclaimed quota retained
                REG->>REG: Keep cleanup ownership, admission and routes remain closed
            end
        else Backend buffer access remains uncertain
            REG->>B: Retain releasePending and affected allocations, leases and quota
            REG->>REG: Retain private cleanup state, keep admission and routes closed
            Note over REG,B: Completion evidence makes deferred cleanup eligible under section 7.1
        end
    end
```

A confirmed exit of an exact process instance can identify all its associated
contexts if process-exit integration is adopted. It needs PID-reuse protection.
A socket event carrying PID/UID is not a process-exit notification: closing
one socket must not remove the process's other connections. Inherited sockets
require a separate ownership policy. Normal socket cleanup needs no process
health polling.

#### Intended process-freeze behavior and prerequisites

Contract requirements: [CC-08 Prepared process freeze and library unload](../openspec/specs/client-connection/spec.md#requirement-cc-08-prepared-process-freeze-and-library-unload),
[CC-04 Socket configuration and disabled heartbeats](../openspec/specs/client-connection/spec.md#requirement-cc-04-socket-configuration-and-disabled-heartbeats),
[CI-03 FD and transport-lifetime binding](../openspec/specs/connection-identity/spec.md#requirement-ci-03-fd-and-transport-lifetime-binding).

The intended operation freezes the existing process with its descriptors
preserved, for example through a process stop or container freezer. Linux's
[cgroup freezer](https://docs.kernel.org/admin-guide/cgroup-v2.html) stops the
processes until thawed; freezing is distinct from killing them.

```mermaid
sequenceDiagram
    participant A as Application / its callers
    participant C as EyilClient runtime
    participant P as Platform freeze mechanism
    participant K as Kernel / open UDS connection
    participant G as Gateway registry and I/O owner
    participant O as Another active application
    Note over C,G: Transport heartbeats disabled, existing process and socket descriptors preserved
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
        Note over A,G: Retire old pending state, explicitly close and connect a fresh session
    end
```

#### Gateway behavior if a client stops receiving

Contract requirements: [GR-07 Unexpected frozen or slow receiver](../openspec/specs/gateway-routing/spec.md#requirement-gr-07-unexpected-frozen-or-slow-receiver).

#### Platform validation requirements

Validate process freeze and disconnect behavior on the supported device with
its configured libzmq, monitor-event format and platform hibernation mechanism.
Tests must cover a prepared idle process frozen longer than 15 seconds,
continued operation of other clients, thaw with the same connection, transport
backpressure and cleanup after an actual socket closure. Section 6.17 lists the
runtime acceptance scenarios.

### 6.15 On-demand Crypto lookup and invocation failures

Contract requirements: [CY-07 On-demand lookup failures and recovery boundary](../openspec/specs/crypto-integration/spec.md#requirement-cy-07-on-demand-lookup-failures-and-recovery-boundary).

Coverage: **AC1.8–AC1.12**.

```mermaid
sequenceDiagram
    autonumber
    participant J as Worker (private pool), operation or lifecycle job
    participant A as Crypto adapter
    participant T as Thunder IShell
    participant C as Crypto plugin / ICrypto
    participant B as SharedBufferManager
    participant R as Gateway I/O owner (ROUTER)
    J->>A: Execute with trusted connection context
    A->>T: QueryInterfaceByCallsign using configured callsign
    T-->>A: Scoped reference or nullptr
    alt Lookup returns nullptr before invocation
        A->>B: FinishUse for unused lease if held, finalize pending release if eligible
        A-->>J: RPC_STATUS_INTERNAL, nothing dispatched to Crypto
    else Reference acquired
        A->>C: Invoke method
        alt Core invocation succeeds
            C-->>A: CryptoError and defined outputs
            A->>C: Release scoped reference immediately after invocation
            A->>A: Preserve service error or copy successful outputs
            A->>B: FinishUse after definitive return, finalize pending release if eligible
            A-->>J: Owned operation response or internal lifecycle result
        else Invocation fails
            C-->>A: Core failure, outputs not valid
            A->>C: Release scoped reference after call returns
            alt Backend access remains uncertain
                A->>B: Retain unresolved lease and any pending release
            else Access definitively ended or was never started
                A->>B: FinishUse if held, finalize pending release if eligible
            end
            A->>A: Log ERROR with invocation failure and known completion state
            A-->>J: RPC_STATUS_INTERNAL, no automatic retry
        end
    end
    alt Job originated from an app RPC
        J-->>R: Correlated failure or result through normal completion path
    else Internal lifecycle notification
        J->>J: Record notification outcome, no unsolicited app RPC response
    end
    Note over J,C: Later calls perform their own lookup, no availability observer or automatic replay
```

### 6.16 Gateway deactivation, reactivation and fresh startup after a crash

Contract requirements: [GL-05 Admission cutoff and graceful drain](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-05-admission-cutoff-and-graceful-drain).

Coverage: **AC1.1–AC1.3, AC1.5–AC1.12**.

#### States and explicit stop ownership

Contract requirements: [GL-01 Wrapper and implementation ownership](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-01-wrapper-and-implementation-ownership).

```mermaid
stateDiagram-v2
    [*] --> STOPPED
    STOPPED --> STARTING: Initialize fresh runtime
    STARTING --> RUNNING: Owners ready and ROUTER bound
    STARTING --> STOPPING: Startup failure
    RUNNING --> DRAINING: Deinitialize or runtime failure closes admission
    DRAINING --> DRAINING: Backend access or cleanup still unresolved
    DRAINING --> STOPPING: Accepted work and cleanup settled
    STOPPING --> STOPPED: All sockets closed and threads joined
    STOPPED --> STARTING: Reactivate with new generation
    note right of DRAINING
        A non-returning backend call keeps the runtime here.
        No forced timeout or thread kill.
    end note
```

#### Admission and drain contract

Contract requirements: [GL-05 Admission cutoff and graceful drain](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-05-admission-cutoff-and-graceful-drain).

#### Deactivation sequence

Contract requirements: [DG-02 Worker capacity and retained-cleanup diagnostics](../openspec/specs/diagnostics/spec.md#requirement-dg-02-worker-capacity-and-retained-cleanup-diagnostics).

```mermaid
sequenceDiagram
    autonumber
    participant T as Thunder
    participant P as EyilGateway wrapper
    participant I as Runtime / drain coordinator
    participant R as Gateway I/O owner (ROUTER)
    participant Z as ZAP worker
    participant M as Monitor worker
    participant W as Private request pool
    participant CP as Crypto adapter / ICrypto
    participant B as SharedBufferManager
    participant X as Owned ZMQ context
    T->>P: Deinitialize
    P->>P: Suppress new callbacks, retain implementation and IShell
    P->>I: Final IConfiguration Release invokes implementation destructor
    I->>I: RUNNING to DRAINING under admission lock, mark all contexts DRAINING<br/>(no new admission, routes kept for replies)
    I->>B: Establish lifecycle finalization gates, retain pending releases and quota
    I->>R: Close new admission, keep replies and control processing running
    I->>Z: Reject new authentication, resolve in-progress setup
    par Running jobs complete
        W->>CP: Scoped query, operation and Release
        CP-->>W: Definitive operation result
        W-->>R: Original-generation completion
        W->>B: FinishUse, record pending-release readiness without gated destruction
        W-->>I: Settle job, lease and any already-claimed finalizer accounting
    and Current request starts or expires
        alt Worker claims RUNNING before its original deadline
            W-->>I: Runs and settles like any other job
        else Original worker-start deadline expires
            R->>R: Atomically expire, log worker_pool_exhausted, prepare RPC_STATUS_INTERNAL
            R->>I: Settle accounting after the expired handoff is reclaimed
        end
    end
    R->>R: Attempt each reply once, release on acceptance or log and drop on failure
    I->>CP: Schedule per-context DISCONNECTED on the private pool
    Note over I,CP: Each notification acquires and releases its own ICrypto reference
    CP-->>I: Lifecycle jobs finish
    I->>B: Mark remaining buffers releasePending, finalize after access and notification
    alt Backend access unresolved or physical cleanup failed
        B-->>I: ERROR logged, private cleanup records and quota retained
        I->>I: Stay DRAINING, teardown is not reported complete
        Note over I,B: Completion evidence later makes cleanup eligible (section 7.1)
    else Cleanup complete
        B-->>I: Quota and retained references released once
        I->>I: Mark all contexts CLOSING, enter STOPPING
        I->>R: Wake owner to retire egress and close transport
        R->>R: Disable monitor, zmq_close(ROUTER) with LINGER=0
        Note over R,M: No per-client DISCONNECTED event is required for plugin stop
        R-->>I: ROUTER closed on owner
        I->>X: zmq_ctx_shutdown()
        X-->>Z: Blocked socket operations wake with ETERM
        X-->>M: Blocked socket operations wake with ETERM
        I->>Z: Signal existing stop wakeup
        I->>M: Signal existing stop wakeup
        Z->>Z: Close REP exactly once, exit
        M->>M: Close PAIR exactly once, exit
        I->>I: Join ROUTER, ZAP and monitor owners
        I->>X: zmq_ctx_term(), retry EINTR
        X-->>I: Context and libzmq internal threads terminated
        I->>W: Stop and join drained private pool
        I->>I: Release current request, completion records and registry, publish STOPPED
        I->>I: Release retained IShell, finish destructor
        I-->>P: Final Release returns after runtime shutdown
        P->>P: Clear implementation pointer, reset activation flags
        P->>T: Unregister remaining notifications and release IShell
        P-->>T: Deinitialize complete
    end
```

#### ROUTER and context teardown

Contract requirements: [GL-01 Wrapper and implementation ownership](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-01-wrapper-and-implementation-ownership),
[GL-06 Socket and context teardown](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-06-socket-and-context-teardown).

#### Reactivation and fresh startup after a crash

Contract requirements: [GL-07 Reactivation and fresh startup after crash](../openspec/specs/gateway-lifecycle/spec.md#requirement-gl-07-reactivation-and-fresh-startup-after-crash).

```mermaid
sequenceDiagram
    participant T as Thunder / platform
    participant OLD as Previous Gateway runtime
    participant C as Existing client
    participant NEW as Fresh Gateway runtime
    T->>OLD: Deinitialize: final Release invokes implementation destructor
    OLD-->>C: UDS closes, client monitor reports connection loss
    OLD-->>T: STOPPED, old runtime quiescent
    T->>NEW: Initialize and Configure fresh generation/context
    NEW->>NEW: Start owners/private pool, bind same UDS pathname
    NEW-->>T: RUNNING
    C->>C: App may close old Client and create a fresh one
    C->>NEW: New UDS connection and authentication
    NEW->>NEW: New FD/lifetime and identity, notify CONNECTED
    NEW-->>C: Handshake succeeds
    Note over OLD,NEW: No old request, route, buffer ID or session is restored
```

### 6.17 Implementation order and verification targets

Build Gateway against the selected ICrypto API and implement its Crypto adapter
as part of this delivery. The following order separates transport, operation
routing and lifecycle work without deferring the ICrypto dependency:

1. Implement the FD/lifetime binding and fresh-runtime stop/reactivation
   contract. Apply the agreed client callback and synchronous local-close
   contracts, together with the selected 15-second
   connect/sync-request/async-send deadlines, the single client submission slot and
   CLIENT_OUT_OF_MEMORY handling, with no async response timeout. Disable ZeroMQ
   heartbeats on both ends. Implement terminal client loss, zero-linger queue
   disposal and explicit creation of a fresh Client. Apply the agreed idle
   hibernation contract; automatic recovery after Thunder failure or Gateway
   reload is outside scope.
2. Implement the agreed RPC envelope, local error behavior and selected async
   completion contract. Document that contract in the public API. Add EyilClient's
   owner loop, single slot with mutex/condition/wakeup, pending map and shutdown.
   Prove callers wait/publish/claim/send independently of earlier responses.
   Implement string service=0 requests and Gateway UNSUPPORTED_SERVICE replies,
   including symbolic status text in both string methods. Preserve the plain-text
   public boundary.
3. Implement startup, ZAP/interim identities, authentication-time CONNECTED,
   the registry and monitor binding. Prove multiple connections from one PID
   remain independent, including closure before the first request.
4. Add four private workers and one current ingress record, with no request
   queue. Pause further receives while that record awaits a worker, continue
   replies/control processing, and return INTERNAL with worker_pool_exhausted
   ERROR if Dispatch has not started within ten seconds. Validate capacity/start
   wakeups, timeout races, per-call ICrypto ownership and completion routing.
5. Add Gateway-local allocations, atomic execution-time ownership/state
   checks and active-use leases. Reject competing uses and uses of retired IDs
   without reserving uses before execution or replaying an operation. Accept valid release
   immediately, defer destruction until active access ends, and retain quota
   through cleanup. Apply the trusted-app cooperation contract, retained directory
   references and exactly-once finalization across release/disconnect/stop.
   Validate the exceptional completion boundary in section 7.1 with the
   platform/Crypto implementation.
6. Integrate AppManager/RuntimeManager authentication only at the marked TODOs,
   once their formal API exists. Hibernation needs no lifecycle subscription;
   application preparation follows the selected idle-process contract.
7. Validate CONNECTED during authentication, accepted-work drain,
   DISCONNECTED, owner-thread teardown and clean reactivation. Exercise the
   ICrypto contract without adding backend policy, health monitoring or replay.

| Test scope | Scenario / evidence required |
| --- | --- |
| L1 client | Nonzero request IDs are never reused within a generation, including after failed submission/retirement; counter exhaustion cannot wrap; response matching and duplicate/late replies; argument/output ownership; each waiter resolves once and each response callback runs at most once |
| L1 async send | API cannot return CLIENT_OK before zmq_send accepts the message; EAGAIN keeps the send waiter pending; timeout/error cancels unsent work with no callback; race acceptance against deadline and disconnect |
| L1 async response | A response later than 15 seconds is delivered; no response timer exists; original callback signatures carry valid Gateway error responses; connection loss or local response-processing failure retires the record without an error callback |
| L1 callback execution | Request calls from the I/O-owner callback return CLIENT_INVALID_ARGUMENT; freeing a response is allowed; callback runs without internal locks and may precede submitting-thread resumption |
| L1 connection callbacks | Initial success and every startup failure produce no callback; later unexpected loss produces CLIENT_CONNECTION_LOST at most once; explicit close suppresses new callback claims; RESTORED and TERMINATED are never emitted, including after an explicit fresh connect |
| L1 client submission slot | At most one published/claimed unsent request; competing callers wait with the mutex released; owner explicitly claims it; acceptance/failure frees the slot, sent requests remain independently pending; no queue container or queue-full result |
| L1 slot waiting and cancellation | Occupy the slot with EAGAIN, keep receiving earlier responses/PAIR events, expire a competing caller without publication, and prove its request never sends; clear SEND_IN_PROGRESS after every nonaccepted attempt and apply latched cancellation before retry; race cancellation with acceptance and preserve accepted async CLIENT_OK |
| L1 synchronous pre-send timeout | Expire a published/claimed sync request before TryBeginSend and prove no later send occurs; expire during an admitted attempt and resolve its actual outcome before returning CLIENT_TIMEOUT, even if accepted; verify the same behavior for both sync methods |
| L1 request-specific retirement | Retire response/timeout for accepted A while B occupies the slot; B remains intact. Repeat completion/retirement for A and inject preparation failure after timeout/close; preserve each winning result and release each matching slot/reference once |
| L1 premature response | Inject a response for a pending entry whose send is not yet accepted; discard/log it without callback, retirement or slot release. A fast valid reply after recorded acceptance still completes normally |
| L1 slot-waiter teardown | Wake callers that have not entered the pending map on explicit close, transport loss and connection-wide decode allocation failure; propagate CLIENT_NOT_CONNECTED, CLIENT_COMMUNICATION_ERROR and CLIENT_OUT_OF_MEMORY respectively, without allocating or waiting for EMPTY; a later explicit close cannot overwrite the loss reason for already admitted callers |
| L1 client startup allocation failures | Fail runtime, thread-start and owner-side setup allocation; CLIENT_OUT_OF_MEMORY leaves output NULL, closes partial resources and joins only started threads |
| L1 client submission allocation failures | Inject failure during input copy, request/waiter creation, serialization and pending-map insertion; all four request APIs return CLIENT_OUT_OF_MEMORY before send acceptance, release partial state and cannot send later or invoke a callback |
| L1 client response allocation failures | Sync returns CLIENT_OUT_OF_MEMORY and NULL/0; matched async responses retire without a callback or revision of accepted CLIENT_OK, even before the submitter resumes; uncorrelatable decode failure follows connection-wide cleanup |
| L1 client cleanup under allocation failure | Free/disconnect complete without waiting for a free slot or allocating a control request/temporary cleanup container; notification register/unregister allocate nothing and retain their specified outcomes |
| L1 client deadlines | Sync uses one 15-second total budget through final byte/string result preparation and async only a 15-second send budget; slot waiting uses the original budget and cancellation cannot send work after returning failure |
| L1 terminal client loss | Retire unsent/pending work and uncommitted responses once; unresolved waiters get CLIENT_COMMUNICATION_ERROR, accepted async submissions retain CLIENT_OK without error callbacks; preserve committed/app-owned responses and reject new requests on the lost handle |
| L1 explicit client disconnect | Close with sync/async/string calls waiting for or occupying the slot; unresolved waiters return CLIENT_NOT_CONNECTED with cleared outputs, accepted async sends retain CLIENT_OK, pending IDs and undelivered responses retire once; cancelled actions never send later |
| L1 disconnect races and callbacks | Race close against send acceptance, timeout, response claim and transport loss; preserve one result, suppress intentional-close events, release every live-call guard and wait for claimed callbacks; no callback or state access after return |
| L1 plain-string APIs | Both string methods copy to the first NUL with no size argument or caller encoding; exercise empty strings, whitespace and line breaks as data, representable large inputs, caller input reuse and allocation failures; copy the received non-OK RpcStatus name with one response terminator; reject unknown/UNSPECIFIED status, unexpected OK and nonempty error payload |
| L1 string completion races | Race response-text preparation with the synchronous deadline, explicit close and transport loss; no intermediate binary success escapes, the winning result is preserved, and any losing prepared string is freed |
| L2 string rejection | Both string APIs send raw text in a service=0 RpcRequest; Gateway logs ERROR and returns correlated UNSUPPORTED_SERVICE with empty payload before worker admission; sync returns CLIENT_OK plus exact status text, async receives that text through its response callback; no ICrypto call or invented text service |
| L1 Gateway | Envelope/oneof validation; context lookup; current/handoff/in-flight ownership; enum/presence mapping; allocation rollback; the same numeric request ID on two authenticated connections is independent, while a duplicate of tracked work in one connection cannot replace its completion |
| L1 Gateway rejection logging | Undecodable service payloads, operation/oneof mismatches and infrastructure failures use RPC_STATUS_INTERNAL with empty payload. Buffer/service errors follow the CryptoError table in section 4.2 under RPC_STATUS_OK. Both log ERROR with the exact cause and known request/connection context; duplicate IDs cannot dispatch twice or replace the original completion; unsafe correlation logs/drops; logs contain no payload or key material |
| L1 buffer error mapping | Zero size/invalid regions: INVALID_PARAMETERS; size/input maximum: OVERSIZED_INPUT_DATA; allocation or quota failure: OPERATION_FAILED; missing/wrong-session/released/busy/release-pending buffer and repeated release: INVALID_SESSION_STATE; output capacity: BUFFER_OVERFLOW. Assert RPC_STATUS_OK, the matching CryptoResponse oneof, unusable success outputs on error, exact diagnostics and cleanup ownership. A first valid release while busy still succeeds |
| L1 Gateway receive pause | Occupy all four workers, admit one current request and verify further ROUTER receives pause without a request queue. Replies, monitor/control events and timers continue; worker-capacity wakeup resumes dispatch/reception without spinning |
| L1 Gateway worker timeout | After 10,000 ms from current-request admission, return INTERNAL with empty payload and worker_pool_exhausted ERROR containing total/running/reserved workers and request identity. Race expiry against Dispatch, including a handed-off job; one result, no execution after expiry, no timeout of running calls. Unread libzmq messages have no Gateway timer yet |
| L1 Gateway drain | Atomic admission cutoff; the current request starts or expires under its original deadline, handed-off ownership settles, running work finishes; stop/control handling requires no new RPC or request queue |
| L1 Gateway response storage | Response storage grows when allocations succeed, with no response-count/byte budget rejection. Inject actual bookkeeping/serialization allocation failure; preserve one completion/error and cleanup ownership without blocking a worker or replaying service work |
| L1 resource configuration | Four private workers, default stack size, default HWM 1000/1000 and kernel buffers, MAXMSGSIZE=-1; individual request/response size and completion storage remain unlimited by policy. Accept capacity 1,572,864 bytes; reject 1,572,865 with RPC_STATUS_OK/OVERSIZED_INPUT_DATA. At most four allocations and 6,291,456 logical capacity bytes per host UID. Distinct UIDs have independent allowances with no global cap. Preserve arithmetic, representation and service validation |
| L1 shared application quota | Allocate two buffers on connection A and two on B with the same host UID; reject the fifth through either connection or new C with RPC_STATUS_OK/OPERATION_FAILED and an ERROR log. Different PIDs and interim appIds, including child apps with that UID, share the allowance. Different UIDs do not. Same-UID connections still cannot use/release each other's buffer IDs |
| L1 quota reservation and cleanup | Race allocations from multiple same-UID connections for the final slot; only one reservation succeeds. Inject creation/preallocation/rollback failure and verify exact charges. Pending release, lost reply and disconnect do not refund before cleanup; closing one connection cannot reset another's charges. Reconnect and reused UID cannot bypass retained charges. Race zero-charge record removal with new allocation and preserve one shared record |
| L1 Gateway response send | At most one logical response attempt; EAGAIN/EHOSTUNREACH/EINTR cause local error and drop; no requeue, second error response or backend replay; storage released once; multipart frames never cross responses |
| L1 region rules | Per operation: missing required region, out-of-bounds region, overlapping inputs and adjacent inputs (allowed); length-0 regions pass Gateway and reach Crypto; 64-bit overlap arithmetic near UINT32_MAX; successful output overlapping an input or outside the allocation returns RPC_STATUS_INTERNAL and ends the lease; BUFFER_OVERFLOW region is forwarded unchanged even when out of bounds |
| L1 buffer concurrency | Admitted jobs reserve no buffer before TryUse; release winning before TryUse returns RPC_STATUS_OK with CRYPTO_ERROR_INVALID_SESSION_STATE and no Crypto call; use winning rejects a competing use through the same service error but permits release acceptance; race release with last-use completion and verify one finalization claim, no use after releasePending and no lost cleanup |
| L1 deferred release | Release during active access returns RPC_STATUS_OK, CRYPTO_ERROR_NONE and the empty release response before that access ends; retain object, record, directory reference and quota; definitive completion triggers cleanup once; duplicate release cannot clear state or create another finalizer |
| L1 buffer completion errors | Null lookup/pre-invocation failure, defined CryptoError and post-call validation/serialization/allocation failure end the lease exactly once when access is definitively over and trigger eligible pending cleanup; ambiguous remote failure retains access and releasePending until completion evidence arrives |
| L1 release and cleanup failures | Idle and deferred cleanup failures log ERROR and keep the ID unavailable with retained cleanup/accounting; accepted release is not retroactively failed and produces no second response; ENOENT permits remaining cleanup; partial allocation rollback failure retains a private record without publishing an ID |
| L1 release response loss and deadlines | Response preparation/send failure and a client timeout do not undo accepted release; expiry of the current release request before Dispatch creates no releasePending state. Deferred cleanup has no forced destruction deadline and does not depend on response delivery |
| L2 transport | Real DEALER/ROUTER IPC, ZAP metadata, reverse-order completion and simultaneous sync/async callers |
| L2 worker burst saturation | Four authenticated connections send 1,000 additional requests while all four workers are blocked in mock Crypto. Advance the test clock through 16 separate admission deadlines; assert INTERNAL replies, one parsed waiting request, paused ROUTER reception and no backend execution of expired requests. Unblock workers, validate every remaining response and per-connection ID exactly once, then send fresh requests on every connection. This uses simulated deadlines alongside the separate real-time ten-second exhaustion test; it does not impose FIFO/fairness or an application request queue |
| L2 backend-independent admission | Empty/default backend configuration and absent configured Crypto still allow startup and handshake. Sync/async backend RPCs deliver RPC_STATUS_INTERNAL; local allocate/release and unsupported string responses remain available. Missing backend does not emit a connection-loss callback; duplicate backend service configuration is rejected |
| L2 connect readiness | CONNECTED alone cannot complete connect; HANDSHAKE_SUCCEEDED returns CLIENT_OK; explicit rejection returns CLIENT_AUTH_FAILED; unresolved/stalled attempt reaches CLIENT_TIMEOUT; each failure wakes the waiter and rolls back resources; no startup outcome invokes a callback, and immediate subsequent loss cannot expose an unpublished handle |
| L2 explicit reconnect | Verify RECONNECT_IVL=-1, IMMEDIATE=1 and LINGER=0; after loss and Gateway return, no automatic handshake or old queued send occurs; explicit close/new connect creates an empty client runtime and new Gateway session, rejects old buffer IDs and never delivers old replies to new requests |
| L2 client disconnect barrier | Disconnect with pending calls and an active short callback; wake callers before waiting, join socket owners without lock deadlocks, discard transport queues, preserve app-owned responses and verify zero callbacks after return |
| L2 disabled heartbeats | Verify IVL/TIMEOUT/TTL are 0 on DEALER and ROUTER before connect/bind and after socket recreation; confirm monitor-driven cleanup still occurs on actual socket closure |
| L2 disconnect | Trusted FD/lifetime-to-User-Id binding; close one of multiple supported interim connections from a PID while the app and other connection remain alive; notify the closed connection's ApplicationContext once after safe drain, with no grace/last-connection wait and no unrelated cleanup. Cover delayed/duplicate events, FD/route reuse and disconnect before the first request |
| L2 guarded responses | Apply the expected transport lifetime to successful and error replies; force route reuse and verify no old response reaches a replacement connection, including before monitor cleanup catches up |
| L2 libzmq startup self-test | Stock draft-enabled libzmq passes monitor-v2 setup but fails the option-10001 check; missing/wrong metadata or event contents fail the private IPC test before production bind. A compatible loaded library passes distinct same-PID lifetimes, a no-request disconnect and wrong-guard rejection. Expiry/stop/failure closes all private sockets/context/temp entries; no Crypto call, production session or public listener is created |
| L1 interim identity | appId has reserved eyil-interim: prefix and a unique token; separate sessionId is stable through CONNECTED, operations and DISCONNECTED. Two connections from one PID differ; app payloads cannot replace either identity. Marker recognition alone is not proof of Crypto key-policy enforcement |
| L1/L2 buffer preallocation | ftruncate success followed by fallocate failure returns RPC_STATUS_OK plus CRYPTO_ERROR_OPERATION_FAILED without publishing an ID. Cover ENOSPC/ENOMEM/unsupported operation and partial rollback failure; verify object/FD/quota ownership. On an isolated test tmpfs using the product configuration, exhaust capacity and verify allocation fails before Crypto accesses the buffer; record cgroup charging and remaining truncation risk |
| L2 endpoint integration | Use the exact AC URI under the single-instance Thunder and trusted container-mount assumptions. Exercise standard bind/close, logged bind failure and partial-start rollback; container clients reach the configured Gateway endpoint |
| L2 buffers | Container-private mapping, cross-connection rejection, release before an admitted use claims the buffer and deferred release during active use; buffer remains accessible to the active backend until definitive completion, new uses fail, and quota becomes available only after cleanup; Crypto errors are forwarded; size/region failures and lost replies; a result after a synchronous timeout is discarded |
| Shared-memory platform validation | Verify app permissions, directory/object reference lifetime and namespace/process reuse under the selected cooperation contract; cleanup does not reopen an exited or reused PID's path |
| L2 app crash cleanup | Allocate buffers, then terminate the owning app without release RPCs; correlated disconnect settles accepted work, sends DISCONNECTED and reclaims all remaining objects/FDs/quotas through retained directory references; repeat with an active Crypto call, accepted pending release, lost allocation/release replies and notification failure, without early destruction or lost cleanup ownership |
| L2 lifecycle | Fail every startup stage; stop idle/busy/receive-paused runtimes; race CONNECTED, explicit release, finalization and disconnect/deactivation; drain already-claimed finalizers, gate other destruction until accepted work settles and DISCONNECTED runs; pending-release records do not block the notification that enables cleanup; unresolved access/cleanup cannot report successful teardown |
| L2 socket teardown | ROUTER closes on its owner after drain; slow/frozen reader cannot block; REP/PAIR close on stop/ETERM; no final monitor-event dependency; all threads/context retire |
| L2 reactivation | Repeated activation cycles rebind in host/container views; fresh identities/generations; stale callbacks/events cannot reach new clients; no FD/thread leaks |
| L2 crash restart | Start with empty state; client monitor observes loss; no old replay/session restoration; fresh connection performs authentication and CONNECTED |
| L2 process freeze | With transport heartbeats disabled: app drains operations/callbacks, freezes longer than 15 seconds, preserves its live context/buffers while another client continues, then resumes; no custom RPC heartbeat or inactivity watchdog exists |
| L2 unexpected frozen receiver | Force output backpressure; failed response is logged/dropped once with no retry or forced idle disconnect; other clients continue; client sync timeout and retained async pending record match the contract |
| L2 multipart response errors | Inject failure before/after routing frame; verify safe multipart finalization or controlled socket teardown, no response retry and no mixing routes/payloads |
| Crypto integration | CMake/BitBake callsign reaches runtime lookup; one query/release per invocation including notifications, errors and local output-allocation failures; no retained ICrypto or health subscriptions; invocation/CryptoError mapping; restart observed through call results; communicator restrictions |
| Future platform integration | AppManager/RuntimeManager identity mapping and API entitlements |

Use a controlled fake adapter for concurrency/failure tests when needed, clearly
separate from production Crypto and never described as real cryptography.
An eventual DobbyTool-based smoke test should execute the client inside a real
application container to verify mounts, credentials and private shared memory;
DobbyTool itself is not in the RPC path.

Runtime components and their integration boundaries are organized as follows:

```text
GatewayRuntime
  authentication = InterimIdentityProvider
    // appId = eyil-interim:<unique-token>; separate per-connection sessionId.
    // TODO(AUTH-APPMANAGER): replace acceptance with the formal UID lookup.
    // TODO(API-AUTHORIZATION): populate per-connection API entitlements.
  cryptoBackend = CryptoServiceAdapter(shell, ConfiguredCallsign("crypto"))
    // Query topic/crypto ICrypto immediately before each invocation.
    // Scoped Release after use, no cached interface or Crypto health observer.
    // CONNECTED during authentication; DISCONNECTED after accepted work ends.
    // Crypto owns internal session/key behavior and recovery.
  lifecycleObserver = none
    // Hibernation preserves the idle connection; the app finishes operations first.
```

Keep this choice explicit in startup/configuration and tests. Interim admission
must not be silently mistaken for production authorization when the later
provider is unavailable.

