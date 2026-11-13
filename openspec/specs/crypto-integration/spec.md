<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# crypto-integration specification

## Purpose

Define the Gateway boundary to the external ICrypto backend without taking ownership of Crypto key policy or internal execution.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC1.8, AC1.12, AC2-Firebolt.1. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: CY-01 Configured backend list and availability

Gateway SHALL select the Crypto backend from trusted configuration and remain available for transport and local buffers when that backend is absent.

The detailed [contract](#contract-cy-01-configured-backend-list-and-availability) is normative for this requirement.

#### Scenario: Configured backend list and availability outcome

- **WHEN** backends is omitted, has no crypto entry or has an empty Crypto callsign
- **THEN** Gateway starts and accepts valid peers without querying an empty callsign; backend operations report INTERNAL, local buffer operations remain available and future service entries do not register adapters.

### Requirement: CY-02 Scoped Thunder interface references

Each Crypto operation or notification SHALL acquire and explicitly release its own ICrypto reference immediately around the invocation.

The detailed [contract](#contract-cy-02-scoped-thunder-interface-references) is normative for this requirement.

#### Scenario: Scoped Thunder interface references outcome

- **WHEN** ICrypto returns an invocation error or raises an external exception
- **THEN** Gateway releases the raw interface exactly once, does not consume undefined outputs and holds no cached interface while preparing or delivering the response.

### Requirement: CY-03 Authentication-time CONNECTED and session bookkeeping

Gateway SHALL settle its per-connection CONNECTED attempt during authentication and keep backend lifecycle separate from transport admission.

The detailed [contract](#contract-cy-03-authentication-time-connected-and-session-bookkeeping) is normative for this requirement.

#### Scenario: Authentication-time CONNECTED and session bookkeeping outcome

- **WHEN** Crypto is missing during authentication and appears later
- **THEN** transport and local-buffer use remain possible, but Gateway neither silently repeats CONNECTED nor replays operations; later calls preserve backend session errors and a fresh explicit connection performs a new notification attempt.

### Requirement: CY-04 Crypto operation destinations and conversions

Gateway SHALL route each recognized Crypto operation and validate conversions/presence according to the mapping below.

The detailed [contract](#contract-cy-04-crypto-operation-destinations-and-conversions) is normative for this requirement.

#### Scenario: Crypto operation destinations and conversions outcome

- **WHEN** a valid ALLOCATE_SHARED_BUFFER or RELEASE_SHARED_BUFFER request is dispatched
- **THEN** it terminates locally in SharedBufferManager; the other fifteen recognized operations use their mapped ICrypto methods and optional fields retain presence.

### Requirement: CY-05 Service consumer and backend policy boundary

Crypto consumers and Gateway SHALL retain the byte-envelope and backend policy responsibilities below.

The detailed [contract](#contract-cy-05-service-consumer-and-backend-policy-boundary) is normative for this requirement.

#### Scenario: Service consumer and backend policy boundary outcome

- **WHEN** a key-generation response is lost or a buffer allocation succeeds
- **THEN** the consumer does not automatically replay key generation; allocation results contain ID/name/size in response bytes, not an EyilClient mapping pointer. Crypto owns key ordering and authorization.

### Requirement: CY-06 Backend shared-buffer access contract

The Crypto backend SHALL independently open, validate and access each shared buffer only for the synchronous call, with no retained buffer state after definitive return.

The detailed [contract](#contract-cy-06-backend-shared-buffer-access-contract) is normative for this requirement.

#### Scenario: Backend shared-buffer access contract outcome

- **WHEN** the backend receives a zero-sized object or an invalid actual-object region
- **THEN** it rejects the access through its service contract and closes acquired resources; Gateway forwards defined service errors, while a process-terminating fault cannot be assumed to produce a response.

### Requirement: CY-07 On-demand lookup failures and recovery boundary

Gateway SHALL preserve invocation/session failures without backend health monitoring, implicit session recreation or operation replay.

The detailed [contract](#contract-cy-07-on-demand-lookup-failures-and-recovery-boundary) is normative for this requirement.

#### Scenario: On-demand lookup failures and recovery boundary outcome

- **WHEN** a later invocation reports INVALID_SESSION_STATE or INVALID_KEY_ID
- **THEN** Gateway preserves that result and does not reconstruct backend state; releasing the interface does not prove unresolved remote access has stopped.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: CY-01 Configured backend list and availability

Source: [GW-015](../../../doc/EyilGateway.md#67-crypto-adapter-per-call-icrypto-references-and-per-connection-sessions), [GW-016](../../../doc/EyilGateway.md#callsign-configuration-bitbake-to-cmake-to-runtime).

The interface supplies ApplicationContext { appId, sessionId } and
SharedBuffer { name }. Construct these from trusted internal records. A raw
FD integer is not an interprocess shared-memory identity.

The production default callsign is empty: an image can run Gateway without a Crypto backend.
With `BUILD_EYIL_DEV_TOOLS=ON`, an empty configured value selects the installed
development-only `org.rdk.CryptoTestBackend` plugin. An explicit callsign is preserved;
disabling development tools restores the empty default. See
[development tools](../../../doc/DevelopmentClient.md#development-cryptotestbackend).
Products select the installed plugin explicitly through CMake/BitBake:

Configure(IShell*) reads ConfigLine() and selects the `crypto` entry by service
name, independently of list order. Missing/empty `backends`, an omitted `crypto`
entry or an empty/omitted callsign means no configured Crypto backend. Do not
query Thunder with an empty callsign or substitute another plugin. Products
can set it to `org.rdk.Crypto` or their installed callsign. Malformed JSON,
null entries/fields, empty service names, duplicate service entries or a worker
count outside 1–255 fail configuration. Entries for future services are accepted
as configuration, but do not register an adapter or make those RPC services
supported. This list replaces the single `cryptocallsign` setting.

No ICrypto lookup is needed during Gateway startup. The listener and client
handshake remain available when Crypto is unconfigured or absent; backend RPCs
return RPC_STATUS_INTERNAL for an unavailable interface. Gateway-local buffer
operations do not require Crypto. Every configured invocation resolves the
selected callsign afresh. Configuration comes from Thunder, never app payloads,
and changes require reconfiguration/restart. Gateway's own callsign remains
org.rdk.EyilGateway.

### Contract: CY-02 Scoped Thunder interface references

Source: [GW-017](../../../doc/EyilGateway.md#interface-ownership).

Acquire ICrypto on the private worker immediately before the method invocation.
Hold the returned reference in a local raw ICrypto pointer and call Release
explicitly as soon as the invocation returns, including the catch path for an
exception raised by the backend. Do not put Thunder interfaces in C++ smart
pointers. The API returns value outputs into
caller-owned storage; check Core::hresult before consuming them, then map and
serialize them after releasing the interface. Release exactly once on every
path, including errors and unwinding; later response-allocation failure must
not leave an interface retained. Do not hold ICrypto while waiting in the
worker-wait phase, waiting for a session-initialization barrier or delivering a response.

No ICrypto pointer is cached in GatewayRuntime, CryptoServiceAdapter, a
connection context or a job waiting for execution. Concurrent invocations own independent
references, even if Thunder returns the same underlying object. Crypto owns
its internal synchronization under the ICrypto contract; reference counting
does not serialize calls. Use the same query/invoke/release scope for
NotifyAppState as for all operation methods.

This is a short-lived interface reference. Thunder owns the plugin and its
COM-RPC transport; Release is not an instruction to destroy Crypto, close a
physical transport connection or end the application's Crypto session. The
Gateway retains its IShell reference until workers and lifecycle jobs finish,
so they can safely perform their own lookups.

### Contract: CY-03 Authentication-time CONNECTED and session bookkeeping

Source: [GW-011](../../../doc/EyilGateway.md#65-zap-interim-identity-and-future-appmanager-lookup), [GW-018](../../../doc/EyilGateway.md#application-context-is-separate-from-the-interface-reference).

Gateway attempts CONNECTED during authentication for every accepted connection,
including one that never sends an RPC. The private lifecycle job settles before
ZAP success; its failure is logged but does not reject an otherwise valid peer.
An unconfigured callsign performs no Thunder lookup. A configured but absent
interface produces the same unavailable-backend result. ZAP waits without
registry locks and wakes on stop/transport closure. A late CONNECTED result
cannot revive a closed context.

Keep transport authentication separate from backend-session initialization.
Missing Crypto does not prevent a usable Client handle or local buffer RPCs;
backend calls use their normal per-call lookup and error mapping. If Crypto
appears later, Gateway does not silently repeat CONNECTED or replay operations:
Crypto validates ApplicationContext and can return its invalid-session error.
A fresh explicit connection performs a new CONNECTED attempt. Successful
initialization is recorded only for matching DISCONNECTED cleanup, not as a
cached health flag. Retain any uncertain notification cleanup obligation; an
in-progress CONNECTED job remains included in plugin shutdown accounting.

Keep appId and sessionId stable for the client connection. The Crypto plugin
owns the corresponding session and transient keys; Gateway keeps only trusted
identity and lifecycle bookkeeping. There is no global Crypto session shared
by all apps and no new app session for each interface lookup.

In the current design, the NotifyAppState(CONNECTED) attempt settles during
authentication before ZAP success and RPC admission; backend failure does not
reject the transport. Matching DISCONNECTED is sent if CONNECTED succeeded.
Keep that connection's ApplicationContext
stable across CONNECTED, operations and DISCONNECTED. Each detected disconnect
starts teardown of its corresponding context, even if the app remains alive or
has another connection. After accepted work finishes, send DISCONNECTED once
before final connection-resource release, without a grace timer or last-client
condition. This also covers authenticated connections that never send a request.
[Section 6.14](../../../doc/EyilGateway.md#614-disconnect-detection-pid-observations-and-hibernation) distinguishes this execution ordering from the architect's
clarification of the disconnect trigger.

Crypto owns session creation, key lifetime, authorization, internal concurrency
and backend recovery. Gateway forwards ApplicationContext and arguments, checks
results and releases each interface reference. It does not implement those
Crypto policies. Every notification uses the same per-call ownership as an
operation.

Gateway does not poll Crypto, subscribe to its plugin-state changes, cache a
backend-health flag or automatically reactivate/restart it. Lookup and call
results are the observations available to Gateway. A new lookup does not prove
that earlier keys or sessions survived a backend restart. [Section 6.15](../../../doc/EyilGateway.md#615-on-demand-crypto-lookup-and-invocation-failures) defines
the Gateway error boundary; Crypto owns its internal recovery. Framework
notifications for the Gateway implementation are unrelated and remain valid.

Crypto owns key authorization. Gateway checks API entitlements when that
platform integration is enabled, without duplicating Crypto's key-usage,
export-protection or cross-app-sharing decisions.

### Contract: CY-04 Crypto operation destinations and conversions

Source: [GW-019](../../../doc/EyilGateway.md#68-crypto-operation-routing-map).

All 17 Protobuf operations have an explicit destination. Only the two buffer
management operations terminate locally; the remaining 15 invoke the Crypto
backend. Structure/limit checks apply even in the interim accept-all phase.

| CryptoRequest operation | Destination / method | Main inputs → successful outputs |
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
| RELEASE_SHARED_BUFFER | SharedBufferManager::RequestRelease | buffer_id → accepted release, immediate cleanup or destruction deferred until active access ends |

Preserve optional presence for import_export_type rather than replacing an
absent field with the enum's zero value. Validate repeated usages, algorithm
parameter variants, sizes and numeric conversions before constructing backend
types. Use explicit mappings rather than assuming enum ordinals always match.

### Contract: CY-05 Service consumer and backend policy boundary

Source: [GW-020](../../../doc/EyilGateway.md#client-and-gateway-boundaries-in-the-crypto-sequences), [GW-021](../../../doc/EyilGateway.md#69-non-shared-memory-operations).

The Crypto sequences in [sections 6.9](../../../doc/EyilGateway.md#69-non-shared-memory-operations)–6.13 use the **byte request APIs**.
The consumer constructs the operation-specific message inside CryptoRequest,
serializes it into RpcRequest.payload, selects RPC_SERVICE_CRYPTO and supplies
that serialized outer request to client_send_request or
client_send_request_async. EyilClient replaces request_id/schema_version and
handles the outer response only. The consumer checks RpcStatus, then decodes
CryptoResponse and checks CryptoError before using operation outputs.

An operation name in a diagram identifies its payload; it is not another public
EyilClient function. Shared-buffer allocation returns response bytes containing
buffer_id, name and size, never a mapped pointer from EyilClient. String APIs
receive Gateway UNSUPPORTED_SERVICE and are not usable for these Crypto operations in this version.

Key generation/import/derivation may change backend state. Loss of a response
does not justify retrying automatically. Ordering UnloadKey or FlushKeys
is handled by Crypto under its ICrypto contract. Gateway does not implement
key-policy synchronization. Competing buffer uses fail at execution-time checks;
a valid release is accepted with destruction deferred until active access ends,
as specified in [sections 6.10](../../../doc/EyilGateway.md#610-shared-memory-identity-records-and-invariants)–6.13.

### Contract: CY-06 Backend shared-buffer access contract

Source: [GW-028](../../../doc/EyilGateway.md#612-shared-memory-operations-common-backend-boundary), [GW-026](../../../doc/EyilGateway.md#applicationcontainer-access-and-failure-limits).

**Required Crypto backend behavior**, following section 11 of the shared-memory
proposal, independently of Gateway's TryUse checks in [section 6.10](../../../doc/EyilGateway.md#610-shared-memory-identity-records-and-invariants):

- Open `SharedBuffer::name` with `open()` and
  `O_RDWR | O_CLOEXEC | O_NOFOLLOW`.
- Obtain the actual opened-object size with `fstat()` and reject zero,
  unsupported or oversized objects before mapping. Validate against that size,
  rather than trusting a size supplied by the caller.
- Independently validate every input and every output region actually accessed
  using `offset <= actualSize && length <= actualSize - offset`. Use checked or
  wider arithmetic for size conversions, output placement and capacity
  calculations; apply the overlap rules in [section 6.10](../../../doc/EyilGateway.md#610-shared-memory-identity-records-and-invariants) and algorithm-specific
  constraints before accessing the regions.
- Map only for the synchronous operation. Unmap and close all acquired resources
  on every return path, including a returned CryptoError. Retain no buffer name,
  descriptor, mapping or allocation record after the call, and perform no
  background access to that buffer after returning.

Crypto owns validation and safe access inside its ICrypto implementation.
Gateway forwards a returned CryptoError in the service response with
RPC_STATUS_OK when the invocation and Gateway handling succeeded. It does not
retry the operation. A failed ICrypto invocation uses RPC_STATUS_INTERNAL and
an ERROR log. A fault that terminates the backend, or Thunder when Crypto shares
its process, cannot be assumed to yield a normal response. These limits are
accepted with the trusted-app cooperation contract; they are not an outstanding
requirement to add memory protection to EyilClient.

If a synchronous call has already returned CLIENT_TIMEOUT after 15 seconds,
its pending ID is retired. Gateway may subsequently send Crypto's success or
error result, but EyilClient discards that late reply. There is no second return
or notification callback. An accepted async request can receive the error
through its response callback while its connection and pending ID remain live.
Both follow the single response-send attempt and delivery limits in [section 6.6](../../../doc/EyilGateway.md#66-request-ingress-dispatch-and-response-delivery).

### Contract: CY-07 On-demand lookup failures and recovery boundary

Source: [GW-047](../../../doc/EyilGateway.md#615-on-demand-crypto-lookup-and-invocation-failures).

There is no Crypto health-monitoring component or plugin-availability
subscription. A request or lifecycle job resolves the configured interface
when it needs to call it. A failed lookup is an invocation-time condition;
it does not stop Gateway transport, reject new client connections or prevent
Gateway-local buffer operations. A failed CONNECTED attempt is logged and
recorded separately from transport admission as specified in [section 6.5](../../../doc/EyilGateway.md#65-zap-interim-identity-and-future-appmanager-lookup).

Successful lookup does not guarantee that the call will succeed or that a
previous session survived. The selected API includes
CRYPTO_ERROR_INVALID_SESSION_STATE and CRYPTO_ERROR_INVALID_KEY_ID; preserve
those service results instead of silently recreating a session and retrying the
operation. They are not evidence, by themselves, of a particular restart cause.
Crypto owns session/key recovery; Gateway reports results without reconstructing
Crypto state or replaying operations.

Gateway cannot promise immediate detection of an idle Crypto restart without a
notification or explicit generation contract. This design accepts detection
through subsequent lookups/calls; it does not add health subscriptions to close
that gap. Session identity, valid key ownership and any backend generation check
are enforced by Crypto's contract. Gateway reports the result and does not
treat an error as permission to reuse an old context or buffer.

A scoped Release ends Gateway's interface ownership; it does not cancel a
remote operation or prove that shared-memory access stopped. Preserve unresolved
leases and any accepted pending release until backend/platform lifecycle
handling establishes completion; then signal the same finalization path used
by normal FinishUse. A null lookup before dispatch and a definitive synchronous
return, including CryptoError, can end this job's lease immediately and trigger
eligible cleanup. Later response-processing failure does not keep a completed
use active or cancel release. A blocked in-process call retains its reference
until the call actually returns; it
cannot be safely cancelled by releasing that reference from another thread.
Gateway retains its own buffer-lifetime obligations without implementing
Crypto recovery or adding a Crypto health-monitoring worker.

## Related contracts and open decisions

[SM-03 Region presence, overlap and output validation](../shared-memory/spec.md#requirement-sm-03-region-presence-overlap-and-output-validation), [SM-05 Execution-time access and definitive completion](../shared-memory/spec.md#requirement-sm-05-execution-time-access-and-definitive-completion), [SM-09 Buffer service error mapping](../shared-memory/spec.md#requirement-sm-09-buffer-service-error-mapping), [GL-04 Correlated disconnect and backend notification order](../gateway-lifecycle/spec.md#requirement-gl-04-correlated-disconnect-and-backend-notification-order), [SDK-02 Compatible build and runtime dependencies](../sdk-installation/spec.md#requirement-sdk-02-compatible-build-and-runtime-dependencies).

- [SHM-COMPLETION](../../../doc/EyilSecureGateway.md#71-abnormal-backend-completion-lifecycle-integration-boundary): unresolved; retain the safeguards above without inventing the missing integration contract.
- [AUTH-APPMANAGER](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
- [MULTI-CONNECTION-POLICY](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
- [AUTHENTICATION-PROGRESS](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
- [API-AUTHORIZATION](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
