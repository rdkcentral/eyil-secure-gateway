<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# connection-identity specification

## Purpose

Bind authenticated peer evidence, per-connection contexts, transport messages and disconnects without confusing reused routes or descriptors.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC1.1, AC1.2, AC1.3, AC1.4, AC1.5, AC1.6, AC1.12. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: CI-01 Endpoints, PLAIN and trusted request metadata

Gateway and Client SHALL use the specified sockets/endpoints and trusted transport metadata, not application credentials or routing IDs, for connection identity.

The detailed [contract](#contract-ci-01-endpoints-plain-and-trusted-request-metadata) is normative for this requirement.

#### Scenario: Endpoints, PLAIN and trusted request metadata outcome

- **WHEN** a DEALER connects using the internal dummy PLAIN strings
- **THEN** the Gateway ZAP REP authenticates trusted UDS peer evidence in its own context and assigns identity independently of the dummy credentials and delivery route.

### Requirement: CI-02 Independent interim connection contexts

Gateway SHALL maintain independent interim identities per accepted connection while preserving the explicitly unresolved authoritative multi-connection policy.

The detailed [contract](#contract-ci-02-independent-interim-connection-contexts) is normative for this requirement.

#### Scenario: Independent interim connection contexts outcome

- **WHEN** two connections originate from the same host UID/PID before authoritative integration
- **THEN** they receive independent appIds/sessionIds and User-Ids; shared UID quota does not merge their Crypto contexts or defer one connection's teardown until another closes.

### Requirement: CI-03 FD and transport-lifetime binding

Gateway SHALL bind each User-Id to trusted FD/lifetime evidence before ZAP success and retain generations through all events, jobs and replies.

The detailed [contract](#contract-ci-03-fd-and-transport-lifetime-binding) is normative for this requirement.

#### Scenario: FD and transport-lifetime binding outcome

- **WHEN** one of two connections from the same process disconnects before sending its first RPC
- **THEN** Gateway selects only that original lifetime; a delayed event or reused FD cannot retire the other connection or resurrect the closed context.

### Requirement: CI-04 Required libzmq extension and guarded replies

Gateway SHALL require and validate the downstream transport metadata, versioned monitor events and per-response route guard below.

The detailed [contract](#contract-ci-04-required-libzmq-extension-and-guarded-replies) is normative for this requirement.

#### Scenario: Required libzmq extension and guarded replies outcome

- **WHEN** a route with identical routing-ID bytes now belongs to a different transport lifetime
- **THEN** the original connection-ID guard rejects its routing frame with EHOSTUNREACH before multipart send; Gateway sends no body and never falls back to PID matching or an unguarded route.

### Requirement: CI-05 Startup capability verification before admission

Gateway SHALL fail activation if the required runtime capabilities or bounded isolated behavioral self-test fail before production admission.

The detailed [contract](#contract-ci-05-startup-capability-verification-before-admission) is normative for this requirement.

#### Scenario: Startup capability verification before admission outcome

- **WHEN** the loaded library accepts version-2 monitoring but lacks correct connection metadata or guarded-send behavior
- **THEN** the startup self-test logs an actionable error, rolls back all private resources and fails Configure before production bind; version numbers and successful monitor setup alone are insufficient.

### Requirement: CI-06 Deployment and endpoint trust boundary

Deployment and Gateway SHALL use the selected endpoint, permissions and container-access contract below.

The detailed [contract](#contract-ci-06-deployment-and-endpoint-trust-boundary) is normative for this requirement.

#### Scenario: Deployment and endpoint trust boundary outcome

- **WHEN** the socket is absent when an application container configuration is generated
- **THEN** the container may start without Gateway access; adding access later requires recreating its configuration/container. Gateway keeps the AC endpoint and does not add a server-identity protocol.

### Requirement: CI-07 Trusted identity and external seams

Gateway SHALL derive connection identity from trusted transport evidence and keep platform identity and Crypto integration behind separate adapters.

The detailed [contract](#contract-ci-07-trusted-identity-and-external-seams) is normative for this requirement.

#### Scenario: Trusted identity and external seams outcome

- **WHEN** two connections share one trusted host UID
- **THEN** their identity and allocation ownership remain independent; quota sharing does not authorize cross-connection access, and future authoritative identity/entitlements remain at the documented seams.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: CI-01 Endpoints, PLAIN and trusted request metadata

Source: [OV-021](../../../doc/EyilSecureGateway.md#44-zeromq-endpoints-and-message-framing).

| Endpoint | Socket / owner | Purpose |
| --- | --- | --- |
| `ipc:///tmp/app-eyil-gateway.ipc` | Gateway ROUTER binds; each client DEALER connects | Application RPC over filesystem UDS |
| `inproc://zeromq.zap.01` | Gateway ZAP REP binds | Internal authentication exchange |
| `inproc://app-eyil-gateway-monitor` | ROUTER monitor publishes; Gateway PAIR receives | Gateway transport events |
| `inproc://app-eyil-client-monitor-<unique-client-id>` | DEALER monitor publishes; client PAIR receives | Client transport events |

The ROUTER I/O owner copies `zmq_msg_gets(message, "User-Id")` before closing
the received message. The routing frame is a delivery address, not authenticated
identity. ZAP's User-Id is a different identifier.

PLAIN server mode belongs on the ROUTER. The REP handler answers ZAP exchanges;
EyilClient supplies dummy PLAIN strings and never runs a ZAP server. The trusted
UID/GID/PID representation depends on the target libzmq implementation, rather
than the client password. See the
[ZAP specification](https://rfc.zeromq.org/spec/27/).

### Contract: CI-02 Independent interim connection contexts

Source: [GW-011](../../../doc/EyilGateway.md#65-zap-interim-identity-and-future-appmanager-lookup).

The ZAP worker assigns one unique User-Id per accepted connection; every request
resolves its context through that authenticated metadata. With authoritative
platform identities, the architect suggests `<app_id>#<app_instance_id>` for
one connection and a unique suffix such as
`<app_id>#<app_instance_id>#<connection_count>` if multiple connections are
allowed. Escape/validate delimiter-based fields and avoid collisions with any
retained connection generation. The string alone does not replace the trusted
transport-lifetime binding.

**Multiple connections — architect decision pending:** typically one app
instance is expected to keep one connection. Allowing additional connections
with distinct User-Ids or rejecting a second connection remains under architect
review. Until that decision and authoritative identity integration, retain the
current independent interim contexts; do not reject another connection merely
because UID or PID matches. If multiple connections are allowed, their backend
ApplicationContext mapping must preserve independent teardown: a unique ZAP
User-Id alone does not isolate two connections passed to Crypto with an identical
ApplicationContext. Track this under MULTI-CONNECTION-POLICY in [section 7.2](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions).

For the interim provider, User-Id can encode `appId#sessionId#connectionToken`,
or be an opaque token resolved through the registry. Generated sessionId is not
an authoritative app-instance ID. Each accepted connection gets its own context;
reconnection creates a new generation. The shared host-UID buffer quota in
[section 6.10](../../../doc/EyilGateway.md#610-shared-memory-identity-records-and-invariants) does not merge these contexts or defer teardown until the last
connection closes.

Set interim appId to `eyil-interim:<unique-token>`, with an unpredictable token
unique to the connection, and generate a separate sessionId. Keep both stable
for that connection's notifications and operations. Reserve the `eyil-interim:`
prefix for this provider; an authoritative provider must not issue identities
in that namespace. Multiple connections from one process still get different
appIds and sessionIds. The client does not supply or override either value.

The marker lets Crypto distinguish an interim identity from an authoritative
platform appId without changing ApplicationContext or the public protocol.
It does not itself enforce key policy: Crypto's treatment of the marker,
including any refusal of device-global keys requiring real app identity, must
be agreed and implemented by Crypto under AUTH-APPMANAGER. Do not claim that
adding the prefix already restricts key access or add a Gateway key-policy check.
Interim values identify this connection only; they do not authorize provisioned-key use.
The platform allocates application UIDs; Gateway must obtain the current
UID-to-app/app-instance relationship through the agreed AppManager/RuntimeManager
integration. Do not infer appId from a numeric UID or introduce an independent
static UID map that can become stale when the platform reassigns a UID.
Until that integration exists, retain the selected interim identity behavior
and leave Crypto's key policy with Crypto.

The interim "accept all" decision removes policy checks, not frame validation,
credential provenance checks, quotas or buffer isolation. Production API
entitlements are cached per connection and purged from admission on disconnect.
No app-supplied username, password, route or payload field replaces the kernel
identity or the future platform lookup.

### Contract: CI-03 FD and transport-lifetime binding

Source: [GW-036](../../../doc/EyilGateway.md#required-connection-to-fd-binding), [GW-040](../../../doc/EyilGateway.md#selected-sequence-with-the-connection-lifetime-extension).

One monitor is attached to the Gateway ROUTER and observes all its underlying
connections. Each DISCONNECTED event concerns one of those connections. Client
disconnect closes DEALER on its owning thread; the resulting UDS closure is
detected by Gateway without a disconnect Protobuf message or heartbeat.
Close is asynchronous and does not acknowledge remote cleanup.
[Monitor API](https://libzmq.readthedocs.io/en/latest/zmq_socket_monitor.html),
[socket close API](https://libzmq.readthedocs.io/en/latest/zmq_close.html)

| Gateway input | Information to associate |
| --- | --- |
| Monitor ACCEPTED | Gateway-side accepted transport FD and its lifetime |
| ZAP authentication | Trusted peer credentials and transport FD/lifetime metadata; Gateway assigns a unique User-Id |
| ROUTER request | User-Id, transport FD/lifetime metadata and binary routing ID |
| Monitor DISCONNECTED | Transport FD/lifetime and versioned peer credentials |

**Bind the Gateway-side FD and its connection lifetime to User-Id before ZAP
success.** Keep the activation generation and a non-reused transport lifetime
identifier in that record. The client's own numeric FD is unrelated.

The platform/libzmq integration must expose the same trusted binding to ZAP and
disconnect handling. A transport-generated connection token accompanying the FD
in local ZAP metadata and versioned monitor events supplies that contract.
Credentials alone or arrival order across independent queues do not.
This is a required integration contract, not a claim that standard ZAP exposes
an accepted FD or standard monitor events contain User-Id.

An event's FD may already be closed. Its stored number can still be used for
lookup; the lifetime identifier prevents selection of a newer connection that
reused it. The registry observes the FD but never closes, reads, writes or
duplicates it. Do not rediscover credentials by calling SO_PEERCRED on a delayed
event's FD. ZMQ_FD returned by zmq_getsockopt is a notification descriptor for
the ZeroMQ socket, not an accepted client's FD.
[ZMQ_FD contract](https://libzmq.readthedocs.io/en/latest/zmq_getsockopt.html#zmq-fd-retrieve-file-descriptor-associated-with-the-socket)

Required rules:

1. Establish the binding during authentication, before any RPC is needed.
   Connections that never send a request remain identifiable for cleanup.
2. Serialize acceptance and closure of that lifetime. Retain closing state
   until in-progress authentication resolves; delayed ACCEPTED or ZAP completion
   must not resurrect a closed connection.
3. Resolve each disconnect to the original context only. Two connections from
   the same UID/GID/PID remain independent.
4. Carry activation and connection generations through every job, completion
   and control event. Retire a binding once; delayed/duplicate events cannot
   affect a replacement connection or a new Gateway activation.
5. Bind ROUTER routing identity to that same transport lifetime and keep
   ROUTER_HANDOVER disabled. At send handoff, the transport integration must
   verify the route still identifies the original connection. A stale registry
   entry or matching routing-ID string alone does not permit old response
   delivery to a replacement connection.
6. Treat missing/conflicting binding evidence as a transport-contract failure,
   never as permission to choose all connections with the same PID. Reject an
   unbound connection; if reliable runtime accounting is lost, use controlled
   teardown rather than guessing ownership.

Use the local metadata and monitor encoding below. A version-2 monitor alone
does not establish support for this extension. Validate FD reuse, rapid
reconnect, interleaved handshakes and closure before the first request on each
target.

The shown ordering is one valid execution, not a requirement that monitor
events arrive before ZAP. Both paths use the same lifetime, so the registry can
merge their evidence without guessing. If disconnect overtakes an in-progress
authentication or CONNECTED job, retain a CLOSING record for that lifetime.
After the job settles, send matching DISCONNECTED if CONNECTED succeeded;
late authentication completion must not reactivate the closed context.

The non-reused lifetime is also necessary after the initial binding: a delayed
event, retained job or cleanup action keyed only by FD could otherwise select
a replacement connection. Carry the activation/lifetime tuple through every
such record. For outgoing responses, check the original lifetime against the
currently selected ROUTER pipe before sending, as specified below. The patch
provides the transport evidence and send guard; Gateway must implement the
registry transitions and resource cleanup that consume them.

### Contract: CI-04 Required libzmq extension and guarded replies

Source: [GW-041](../../../doc/EyilGateway.md#required-libzmq-extension-and-platform-integration).

The source patch is included in this repository:
[zmq_connection_lifetime.patch](../../../doc/zmq_connection_lifetime.patch).
Apply it to the upstream libzmq 4.3.4 sources in the platform recipe. It contains
the connection metadata, version-2 monitor events, guarded ROUTER sends,
regression tests and libzmq integration notes described in the patch and the [extension guide](../../../doc/EyilGateway.md#required-libzmq-extension-and-platform-integration).

The reviewed RDK manifest selects `meta-openembedded` tag `rdk-4.0.0`, whose
[ZeroMQ recipe](https://github.com/rdkcentral/meta-openembedded/blob/4ad41baed6236d499804cbfc4f174042d84fce97/meta-oe/recipes-connectivity/zeromq/zeromq_4.3.4.bb)
uses **libzmq 4.3.4**. Build that library with the connection-lifetime source
extension and `-DENABLE_DRAFTS=ON`. Gateway uses the matching headers with
`ZMQ_BUILD_DRAFT_API` defined. Stock libzmq 4.3.4 does not supply this complete
binding; the properties and guarded-send option below are downstream additions.

| Boundary | Required extension contract |
| --- | --- |
| ZAP request body frames | `zmq_msg_gets()` exposes `Transport-Connection-Id` and `Transport-FD` as decimal uint64 strings; Peer-Address retains kernel-derived UDS credentials |
| Received ROUTER request | The same transport properties accompany User-Id on the body and routing frames; copy them before closing the messages |
| Monitor ACCEPTED, version 2 | Two values: `[FD, connection_id]` |
| Monitor DISCONNECTED, version 2 | Five values: `[FD, UID, GID, PID, connection_id]` |
| Monitor version 1 | Existing framing and single FD value remain unchanged; it cannot provide the required lifetime binding |
| Outgoing ROUTER reply | Set `ZMQ_ROUTER_EXPECTED_CONNECTION_ID` to the original request's uint64 transport identity before its routing frame; libzmq verifies the selected pipe on the ROUTER owner thread |

The stream engine allocates a nonzero identity before authentication and does
not reuse it within the loaded library instance. Pair it with Gateway's own
activation generation; the identity is not persistent across process restart
or library unloading. Capture UID/GID/PID while the accepted UDS FD is valid;
disconnect reporting uses the retained snapshot. An unavailable credential is
represented by UINT64_MAX and must not be accepted as a valid UDS identity.

ZAP keeps its standard multipart body. The added properties are local message
metadata, not extra wire frames or application-supplied credentials. The ZAP
request-ID field is not the connection lifetime. Native transport properties
take precedence over metadata from ZAP replies or remote peers. This contract
uses a direct inproc ZAP handler; rebuilding ZAP messages from their byte strings
alone would lose the local properties.

The ROUTER guard uses downstream option number 10001 and a native uint64
argument. Its value persists until changed; zero disables it. Gateway must
successfully set the original connection ID for every response, then attempt
the routing frame and body under the single-attempt policy in [section 6.6](../../../doc/EyilGateway.md#66-request-ingress-dispatch-and-response-delivery).
A missing or replaced route fails with EHOSTUNREACH before entering multipart
send state, even if the same routing-ID bytes now identify a different peer.
Do not send the body after routing-frame failure. The guard does not promise
peer receipt after local send acceptance.

Keep ROUTER_MANDATORY enabled and ROUTER_HANDOVER disabled. The guard supports
the binding ROUTER and accepted ZMTP TCP/IPC stream path; unsupported routes
fail closed. Check option results, metadata presence and monitor frame counts.
Do not silently fall back to PID matching, an unguarded send or assumed event
ordering when the installed library lacks the extension. The version number
reported by zmq_version() alone cannot identify this downstream capability.

### Contract: CI-05 Startup capability verification before admission

Source: [GW-042](../../../doc/EyilGateway.md#startup-capability-check-before-admission).

Before binding the production IPC endpoint or publishing RUNNING, the ROUTER
owner checks the required downstream option and sets up the version-2 monitor.
A successful build against patched headers is not evidence that the loaded
shared library has the same capabilities. The [guide contains an implementation outline](../../../doc/EyilGateway.md#startup-capability-check-before-admission); FailStartup means ERROR logging followed by normal startup rollback:

Use the real configured monitor endpoint/event mask and the existing socket
owner; do not create a second monitor here. A missing required draft symbol may
prevent library loading before Configure; that is also a startup failure, never
a reason to fall back to version-1 events. Report the runtime version for
diagnostics, but do not use it as the capability test. Resetting the probe does
not replace setting the original connection ID before every actual reply.

Option 10001 detects the downstream guard; version-2 monitoring is also present
in stock draft-enabled libzmq and does not identify the patch. Neither the
version number nor successful option/monitor setup proves the metadata and
event contents. In addition to the production-socket checks above, Configure
must complete this behavioral self-test before creating the production listener:

1. Create an isolated ZMQ context and a private Gateway-owned directory with
   mode 0700 containing a temporary IPC endpoint. One test owner runs the
   nonblocking ROUTER, DEALERs, monitor PAIR and a minimal ZAP REP; use only dummy
   credentials and probe bytes. The test never calls Crypto/AppManager, modifies
   production contexts, or binds the public endpoint.
2. Probe option 10001 and enable version-2 monitoring. Establish two connections
   from the same process. Verify distinct nonzero transport identities, the
   accepted FD/lifetime pair, kernel credential encoding and the ZAP metadata.
   Exchange one probe on the first connection; require the same lifetime/FD
   and assigned User-Id on its ROUTER routing/body frames.
3. Try a reply guarded by the other connection's identity: the routing-frame
   send must fail with EHOSTUNREACH and no body may be sent. Set the correct
   guard and verify one successful probe reply. Close both peers, including the
   second peer before it sends an application message; require the five-value
   disconnect events to match their original accepted/ZAP lifetimes and
   credential snapshots. Independent monitor/ZAP delivery order is permitted.
4. Use one total 15-second monotonic self-test deadline and wake on requested
   stop. Failure, missing metadata, wrong event shape or expiry logs one
   actionable ERROR and fails Configure before production bind/admission.
   Close sockets on their owner with zero linger, join any started test owner,
   terminate the private context and remove only its owned temporary entries
   on both success and failure. Safe rollback must finish before Configure
   returns; the deadline does not authorize abandoning a thread.

Log one successful startup summary, not each probe frame. The test must exercise
the library actually loaded by Gateway, not an external executable that might
resolve another libzmq. Its bounded check does not replace the patch's full
FD/route-reuse regression or testing the product's installed binary. Continue
validating native properties and event shapes at runtime; reject missing or
invalid identity evidence without accepting an unbound connection or sending
an unguarded reply. Stock or mismatched libzmq must fail activation without
leaking partially started resources.

### Contract: CI-06 Deployment and endpoint trust boundary

Source: [OV-013](../../../doc/EyilSecureGateway.md#33-container-mounts-and-direct-access), [OV-029](../../../doc/EyilSecureGateway.md#74-settled-deployment-and-scope-assumptions).

The AC endpoint is `ipc:///tmp/app-eyil-gateway.ipc`. Thunder is trusted to run
one Gateway instance. The initial Gateway implementation explicitly applies
mode `0666` to its socket after binding. This lets applications with different
UIDs connect without a shared Gateway group; it also permits local host processes
that can reach the path. This is an explicit part of interim accept-all admission,
not app authorization. Container policy supplies the correct endpoint mount,
including exposure of the current socket after endpoint recreation. A product
that narrows socket permissions must also provide the required UID/GID or ACL
access for every intended app; changing to `0600` or `0660` alone breaks those
clients. These are selected deployment assumptions, described in [section
7.4](../../../doc/EyilSecureGateway.md#74-settled-deployment-and-scope-assumptions); endpoint ownership and server identity are not open Gateway design topics.

The AppManagers Legacy and RALF generators add the socket bind mount only when
the source exists and is a Unix socket. Otherwise the container starts without
Gateway access. If Gateway becomes available later, recreate the container with
a newly generated configuration to add the mount. See
[container mount setup](../../../doc/DevelopmentClient.md#container-mount).

Monitor and ZAP `inproc://` endpoints are local to a ZMQ context; they need no
filesystem socket or container mount.

Ordinary application containers must not acquire the Crypto interface directly
through Thunder's communicator. `ApplicationContext` is identity metadata; it
does not authenticate the COM-RPC caller. The platform direction is to restrict
general app access to Thunder. Deployment validation must verify the actual
communicator mounts and access policy for the deployed Crypto integration.

Thunder is trusted to run one EyilGateway instance. Container policy mounts
the correct Gateway endpoint with the required permissions and maintains that
mapping across endpoint recreation. Gateway uses the standard libzmq bind/close
path at `ipc:///tmp/app-eyil-gateway.ipc`; startup/bind failures are logged and
rolled back. A separate native listener, endpoint ownership protocol and client
server-identity verification are outside the selected scope. The original AC
endpoint is unchanged.

### Contract: CI-07 Trusted identity and external seams

Source: [OV-009](../../../doc/EyilSecureGateway.md#25-delivery-boundary), [OV-012](../../../doc/EyilSecureGateway.md#32-components-ownership-and-trust-boundaries), [OV-014](../../../doc/EyilSecureGateway.md#34-external-integration-seams).

Keep authentication and Crypto integration behind separate adapters. The interim
identity provider accepts connections until AppManager lookup is integrated.
CryptoServiceAdapter calls the deployed Crypto plugin using trusted connection
metadata and checks both invocation and service results. Gateway-local buffer
allocation/release needs no ICrypto call.

The app cannot authoritatively provide its UID, appId, sessionId or a host
shared-memory path. The transport supplies peer credentials; the Gateway creates
the connection context and resolves opaque buffer IDs. Key policy remains in
Crypto, including locators, usages, import/export restrictions and sharing.

The interface names in the [integration-seam table](../../../doc/EyilSecureGateway.md#34-external-integration-seams) describe internal seams. They are not
additional app-facing Thunder interfaces and do not claim that a formal
AppManager authentication API is already available. Future API additions should
be confined to these seams, rather than spreading external plugin types through
the client, transport or buffer registry.

## Related contracts and open decisions

[GL-04 Correlated disconnect and backend notification order](../gateway-lifecycle/spec.md#requirement-gl-04-correlated-disconnect-and-backend-notification-order), [CY-03 Authentication-time CONNECTED and session bookkeeping](../crypto-integration/spec.md#requirement-cy-03-authentication-time-connected-and-session-bookkeeping), [SM-02 Shared trusted-host-UID quota](../shared-memory/spec.md#requirement-sm-02-shared-trusted-host-uid-quota), [SDK-02 Compatible build and runtime dependencies](../sdk-installation/spec.md#requirement-sdk-02-compatible-build-and-runtime-dependencies).

- [AUTH-APPMANAGER](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
- [MULTI-CONNECTION-POLICY](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
- [AUTHENTICATION-PROGRESS](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
- [API-AUTHORIZATION](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
