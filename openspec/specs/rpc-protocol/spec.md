<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# rpc-protocol specification

## Purpose

Define the service-neutral wire envelope, correlation, framing and status layers shared by EyilClient and EyilGateway.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC1.8, AC2-Client.5, AC2-Firebolt.2. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: RP-01 Envelope and request identity

EyilClient and Gateway SHALL use the schema-1 envelope and per-connection correlation rules below.

The detailed [contract](#contract-rp-01-envelope-and-request-identity) is normative for this requirement.

#### Scenario: Envelope and request identity outcome

- **WHEN** two connections submit requests with the same numeric request ID
- **THEN** Gateway correlates each request within its authenticated connection generation; EyilClient replaces caller IDs/schema and returns the complete validated response envelope.

### Requirement: RP-02 Status layers and correlated Gateway failures

Gateway and consumers SHALL preserve the separate local, routing, service and invocation result layers and the following correlation rules.

The detailed [contract](#contract-rp-02-status-layers-and-correlated-gateway-failures) is normative for this requirement.

#### Scenario: Status layers and correlated Gateway failures outcome

- **WHEN** a second request repeats an ID still owned by an accepted request on the same connection
- **THEN** Gateway logs and discards the duplicate without another execution or response; the original result remains eligible for delivery.

### Requirement: RP-03 Protocol scope and concurrency limits

Implementations SHALL respect the capabilities and limitations of the current wire schema.

The detailed [contract](#contract-rp-03-protocol-scope-and-concurrency-limits) is normative for this requirement.

#### Scenario: Protocol scope and concurrency limits outcome

- **WHEN** a synchronous request times out after possible remote execution
- **THEN** no protocol cancellation or replay guarantee is inferred and no automatic replay or heartbeat service is introduced.

### Requirement: RP-04 Multipart framing

DEALER and ROUTER SHALL use the exact framing below and drain complete malformed multipart messages.

The detailed [contract](#contract-rp-04-multipart-framing) is normative for this requirement.

#### Scenario: Multipart framing outcome

- **WHEN** a peer sends an unexpected multipart body
- **THEN** the receiver consumes and rejects the entire malformed message before accepting the next one; it does not reinterpret remaining frames as requests or responses.

### Requirement: RP-05 Unsupported raw-text wire contract

String APIs and Gateway SHALL use the existing service-0 rejection contract below without interpreting text as a Crypto operation.

The detailed [contract](#contract-rp-05-unsupported-raw-text-wire-contract) is normative for this requirement.

#### Scenario: Unsupported raw-text wire contract outcome

- **WHEN** a string call sends an empty string or ordinary text
- **THEN** the library sends a schema-1 service-0 envelope; a correlated UNSUPPORTED_SERVICE reply with empty payload becomes owned status text. Unexpected OK, unknown status or nonempty payload follows the defined local response-error path.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: RP-01 Envelope and request identity

Source: [OV-016](../../../doc/EyilSecureGateway.md#41-the-two-protobuf-files).

| File | Purpose | Boundary |
| --- | --- | --- |
| [rpc.proto](../../../apis/proto/rpc.proto) | Service-neutral RpcRequest/RpcResponse, request_id, service, schema_version and RpcStatus | EyilClient ↔ Gateway routing |
| [crypto.proto](../../../apis/proto/crypto.proto) | CryptoRequest/CryptoResponse, operations, key metadata, buffer allocation/release and offset/length regions | Crypto consumer ↔ Gateway Crypto adapter |

`RpcRequest.payload` carries service payload bytes. For CRYPTO these are a
serialized CryptoRequest; the unsupported string path carries text as described
below. `RpcResponse.payload` contains the service response when available and
is empty for Gateway routing errors. Crypto
defines an operation enum and a `oneof`; they must agree. A valid Protobuf parse
alone does not validate the operation, enum values, required message presence
or allowed lengths.

The RPC envelope fields are:

| Message | Fields |
| --- | --- |
| RpcRequest | request_id: uint64; service: RpcService; schema_version: uint32; payload: bytes |
| RpcResponse | request_id: uint64; service: RpcService; schema_version: uint32; status: RpcStatus; payload: bytes |

[ProtocolVersion.h](../../../apis/proto/ProtocolVersion.h) fixes the current envelope
schema to **1**. EyilClient sets it for each RPC. There is no version negotiation
or fallback protocol in the current schema. Library/package ABI versions and
Thunder plugin metadata versions are separate concepts.

**Agreed envelope handling:** EyilClient
serializes/deserializes the outer RPC envelope, owns request IDs, sets
schema_version to 1 and correlates responses.

For the byte API, the caller supplies a serialized RpcRequest carrying
service and payload. EyilClient parses the envelope, assigns request_id, sets
schema_version and serializes it for transmission. On receipt, it parses and
validates RpcResponse, matches request_id to the pending call and returns the
serialized response to that caller. The service is carried in the envelope;
no separate service argument is needed. Crypto payload bytes remain opaque to
EyilClient and are interpreted by the service-specific consumer.

Request IDs are nonzero and unique throughout one Client connection generation,
including after a request is retired. Different connections may use the same
numeric ID. Gateway therefore identifies a request by its authenticated
connection generation together with request_id, never by request_id globally.

### Contract: RP-02 Status layers and correlated Gateway failures

Source: [OV-017](../../../doc/EyilSecureGateway.md#42-status-and-error-layers), [OV-018](../../../doc/EyilSecureGateway.md#agreed-local-error-behavior), [OV-019](../../../doc/EyilSecureGateway.md#gateway-faults-buffer-service-outcomes-and-explicit-error-logging).

| Layer | Meaning and handling |
| --- | --- |
| ClientResult | Local C API validation, connection, allocation, timeout or communication result |
| RpcStatus | Gateway routing/execution result; explicitly inspect it even when a response was received |
| CryptoError | Crypto service result, carried inside CryptoResponse |
| Core::hresult | Whether the ICrypto invocation itself succeeded; check before reading output parameters |

`CLIENT_OK` on an asynchronous submission means zmq_send accepted the message
into libzmq's queue; it does not confirm physical UDS delivery or remote success.
`RPC_STATUS_OK` means the Gateway successfully produced the
service response; `CryptoResponse.error` can still report a failed crypto
operation. Protobuf's zero default `RPC_STATUS_UNSPECIFIED` is not success.

| RpcStatus | Intended use |
| --- | --- |
| UNSPECIFIED (0) | Missing/unset result; reject as an invalid response |
| OK (1) | Successfully routed and processed at the Gateway layer |
| INVALID_REQUEST (2) | Parsed outer RPC envelope violates the agreed structure; service-message failures and parsed buffer outcomes follow their separate mappings below |
| UNSUPPORTED_SCHEMA (3) | Unsupported envelope schema_version |
| UNSUPPORTED_SERVICE (4) | No registered service, including service=UNSPECIFIED used by the unsupported string path; log ERROR and return empty payload |
| MALFORMED_REQUEST (5) | Cannot decode the outer RPC envelope or its framing; service-payload decoding failures use INTERNAL as specified below |
| INVALID_CONNECTION (6) | Missing, stale or inadmissible authenticated connection context |
| INTERNAL (7) | Gateway infrastructure or service-message decoding failure: ten-second current-request worker-start timeout, request/job/response allocation failure, null ICrypto lookup or failed invocation when a correlated reply is possible. Shared-buffer service outcomes use the [buffer error contract](../shared-memory/spec.md#requirement-sm-09-buffer-service-error-mapping) |

Preserve the two remote error layers. RPC_STATUS_INTERNAL with an empty service
payload reports a Gateway infrastructure/decoding failure. A structurally valid
Crypto operation that produces a buffer/service error returns RPC_STATUS_OK
with a serialized CryptoResponse carrying the appropriate CryptoError. This
also applies to AllocateSharedBuffer and ReleaseSharedBuffer, which execute in
Gateway without ICrypto. Use the existing schema; no new error enum is required.

| Gateway failure | RPC result when safely correlatable | Required log detail |
| --- | --- | --- |
| Duplicate ID still owned by an accepted request on the same connection generation | No second response; log and discard the duplicate | Duplicate ID and original request state; preserve the original completion |
| Undecodable service message or operation/oneof mismatch | RPC_STATUS_INTERNAL, empty payload | Exact parse/structure failure; no service execution |
| Current request cannot start on a private worker within 10 seconds | RPC_STATUS_INTERNAL, empty payload | ERROR worker_pool_exhausted; configured/running/reserved workers, elapsed wait, request ID/service and connection generation; never execute it later |
| Request/job/response infrastructure allocation failure | RPC_STATUS_INTERNAL, empty payload | Failing allocation/preparation stage and result, distinct from worker saturation; preserve cleanup ownership |
| Null ICrypto lookup or failed Core::hresult invocation | RPC_STATUS_INTERNAL, empty payload | Configured callsign, operation and lookup/invocation result; do not consume undefined outputs |
| Invalid output region returned as a successful backend result | RPC_STATUS_INTERNAL, empty payload | Returned bounds and recorded capacity; expose no unusable output |

Only send an error when the request ID and original authenticated route permit
unambiguous correlation. A duplicate ID still owned by an accepted request must
not produce a second competing response under that ID: log and discard the
duplicate, leave the original job/completion intact and never dispatch a second
operation. Scope this check to the authenticated connection generation; request
1 on another connection is independent. This is not a replay/deduplication
guarantee for completed operations. Likewise, log and discard malformed input
without a trustworthy ID or route. Neither error layer permits inventing a
request ID or replying to a replacement connection.

### Contract: RP-03 Protocol scope and concurrency limits

Source: [OV-020](../../../doc/EyilSecureGateway.md#43-protocol-capabilities-and-limitations).

| Capability | Implementation consequence |
| --- | --- |
| Independent request_id | Many requests may be outstanding; match responses by ID, not arrival order |
| Explicit service enum | A new service needs a schema/adapter addition; only CRYPTO is currently defined |
| Opaque service payload | Transport does not implement Crypto key policy |
| Buffer ID plus regions | Four bulk-data operations avoid embedding their input/output in the RPC message |
| Empty ReleaseSharedBufferResponse | Successful response acknowledges accepted release; destruction may wait for active access to end, with no later completion notification |
| uint32 buffer fields | Validate size/ranges with overflow-safe arithmetic and stricter configured quotas |
| No app identity fields | Derive context from trusted transport metadata |
| No deadline/cancel message | A local timeout cannot cancel backend work or prove buffer reuse is safe |
| No notification envelope | Notification support is reserved for the future; registration returns CLIENT_NOT_IMPLEMENTED and creates no subscription |
| No retry/deduplication contract | Do not replay key generation, import or other operations automatically |
| NUL-terminated string API boundary | Copy input without caller encoding; send an unrouted service=0 envelope and expose Gateway UNSUPPORTED_SERVICE as status text; successful text-service execution is deferred |
| No lifecycle/heartbeat messages | Do not invent idle expiry, hibernation commands or heartbeat services |

The protocol permits concurrent requests; it does not define ordering for
operations sharing a key, buffer or session. Request-ID correlation also does
not provide exactly-once execution or distinguish a lost reply from an operation
that never ran.

### Contract: RP-04 Multipart framing

Source: [OV-021](../../../doc/EyilSecureGateway.md#44-zeromq-endpoints-and-message-framing).

DEALER sends one frame containing the serialized RpcRequest.
ROUTER receives `[routing_id][RpcRequest bytes]` and replies with
`[routing_id][RpcResponse bytes]`; DEALER receives the response body only.
There is no empty REQ delimiter.
Validate the frame count and consume/reject an entire malformed multipart
message before accepting another. The required frame count is explicit; no
additional message-byte cap applies. Retain representation and arithmetic checks.

String API input does not change this framing: EyilClient constructs the binary
RpcRequest internally and sends it with its serialized length. Gateway still
receives a binary envelope. The local C-string terminator is not a transport
frame delimiter and is not included in a directly carried text payload.

### Contract: RP-05 Unsupported raw-text wire contract

Source: [CL-030](../../../doc/EyilClient.md#defined-wire-request-and-error-response).

| Boundary | Selected representation |
| --- | --- |
| Caller input | NUL-terminated text, including an empty string; NULL is invalid |
| Internal request | RpcRequest with assigned nonzero request_id, schema_version=1, service=RPC_SERVICE_UNSPECIFIED (0) and payload equal to the copied text bytes, excluding the terminator |
| Gateway dispatch | After common context/envelope/ID checks, service 0 has no registered handler. Log ERROR unsupported_service and construct a correlated RPC_STATUS_UNSUPPORTED_SERVICE response; do not parse CryptoRequest or submit a service job |
| Wire response | RpcResponse echoes request_id and service=0, sets schema_version=1 and status=RPC_STATUS_UNSUPPORTED_SERVICE, with empty payload |
| Synchronous C result | CLIENT_OK with a library-allocated NUL-terminated string containing exactly RPC_STATUS_UNSUPPORTED_SERVICE |
| Async C result / response | CLIENT_OK after libzmq send acceptance; a delivered error response later invokes the supplied string callback with that same owned status string |

UNSPECIFIED records the absence of an assigned text service. It is not a new
service identifier and never selects Crypto. The internal string builder alone
creates this envelope; the public byte-input validator continues to require a
nonzero service. Common response validation matches the service expected by
the pending record, including 0 for this string path. No Protobuf field, enum
or schema-version change is needed for this unsupported behavior. A future text
service requires an explicit route and interpretation contract; do not turn
service 0 into a working text service or guess a route from input contents.

The status string is the symbolic name of the **received** non-OK RpcStatus.
For example, a correlated INVALID_CONNECTION or INTERNAL reply is exposed as
RPC_STATUS_INVALID_CONNECTION or RPC_STATUS_INTERNAL. This is a defined remote
error representation, not a fabricated local-error response. Reject an unknown
or UNSPECIFIED status, unexpected OK or nonempty payload on this unsupported
path as CLIENT_COMMUNICATION_ERROR for sync, or diagnostic retirement without
a callback for an already-accepted async request. Future successful text/service
responses are outside this version's scope.

## Related contracts and open decisions

[CR-02 Local result and allocation-failure contract](../client-requests/spec.md#requirement-cr-02-local-result-and-allocation-failure-contract), [SM-09 Buffer service error mapping](../shared-memory/spec.md#requirement-sm-09-buffer-service-error-mapping).

- [FUTURE-TEXT-SERVICE](../../../doc/EyilSecureGateway.md#72-remaining-api-and-platform-decisions): unresolved; retain the safeguards above without inventing the missing integration contract.
