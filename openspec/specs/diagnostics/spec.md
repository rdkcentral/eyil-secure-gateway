<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# diagnostics specification

## Purpose

Define actionable diagnostics and confidentiality without a new metrics API, periodic polling or full request tracing.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC1.7, AC1.12, AC2-Client.5. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: DG-01 Severity, context and confidentiality

Client and Gateway diagnostics SHALL use the existing logging facilities with the following severities, bounded context and failure-safe suppression rules.

The detailed [contract](#contract-dg-01-severity-context-and-confidentiality) is normative for this requirement.

#### Scenario: Severity, context and confidentiality outcome

- **WHEN** repeated WARNING and ERROR events occur in the same category
- **THEN** each severity has its own five-second suppression window and suppressed counts; WARNING does not hide ERROR, and logging never exposes payloads or changes cleanup/result ownership.

### Requirement: DG-02 Worker capacity and retained-cleanup diagnostics

Gateway SHALL distinguish worker saturation and unresolved cleanup with actionable event-driven diagnostics.

The detailed [contract](#contract-dg-02-worker-capacity-and-retained-cleanup-diagnostics) is normative for this requirement.

#### Scenario: Worker capacity and retained-cleanup diagnostics outcome

- **WHEN** a request first waits for capacity and later expires before Dispatch
- **THEN** one worker_pool_wait WARNING precedes an ERROR worker_pool_exhausted with actual elapsed wait and running/reserved counts; retained teardown produces cleanup_wait once per cause without a polling timer.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: DG-01 Severity, context and confidentiality

Source: [OV-022](../../../doc/EyilSecureGateway.md#45-diagnostics-and-logging), [OV-019](../../../doc/EyilSecureGateway.md#gateway-faults-buffer-service-outcomes-and-explicit-error-logging).

The initial implementation emphasizes actionable warning/error diagnostics.
Use the platform's existing logging facilities and a consistent reason code;
no new metrics service, app API or mandatory request-tracing system is required.

Connection teardown emits `WARNING reason=cleanup_wait` once per connection and
waiting cause: authentication, accepted work, unresolved buffer access, worker
capacity or failed final cleanup. Include connection/UID, active-request count,
worker running/reserved counts and whether Gateway is stopping. Repeated I/O
wakeups do not repeat the same warning. Existing category suppression still
limits bursts across connections. This is event-driven diagnostics; it adds no
periodic poll, cleanup replay or permission to free an unresolved lease.

| Severity | Events and required context |
| --- | --- |
| ERROR | worker_pool_exhausted after the ten-second worker-start wait, with total/running/reserved counts and request identity; Gateway faults and buffer/service failures from [section 4.2](../../../doc/EyilSecureGateway.md#42-status-and-error-layers); startup/configuration or required-libzmq-capability failure; ICrypto lookup/invocation failure; response allocation/send failure; shared-memory creation or deferred-cleanup failure. Record the failing stage and result/errno when available. |
| WARNING | First transition of a request into waiting for worker capacity; client deadline expiry, unexpected connection loss, an accepted async record retired without a deliverable response, and unresolved backend-access/cleanup obligations. Include the reason and number of affected records; normal idle connections and prepared process freeze are not warnings. |
| INFO | One activation/readiness summary with relevant configured limits and libzmq version/capability-check outcome; one orderly stop/drain summary. Routine successful requests, buffer operations and explicit client disconnects need no per-operation INFO log. |
| DEBUG / TRACE | Optional, disabled-by-default investigation of sequencing or individual successful operations. Never enable full request-flow tracing by default to compensate for missing error context. |

Include the component, activation/connection generation, request_id, service,
operation and opaque buffer ID only when known and relevant. Use bounded,
sanitized fields. Never log payloads, strings supplied by applications, key
material, authentication tokens or shared-memory contents. Logging a rejection
does not imply its error response was delivered; log a failed delivery as a
separate outcome under the same correlation identifiers.

Keep the ERROR classification required by [section 4.2](../../../doc/EyilSecureGateway.md#42-status-and-error-layers). Rate-limit diagnostics
within each fixed category and severity to one emitted event per five-second
window, reporting suppressed-event counts at that same severity. WARNING must
not suppress ERROR. Stop-time summaries retain the last suppressed connection
and request identifiers; aggregate counts can cover multiple requests.
Synchronous adapter calls inherit a bounded thread-local correlation scope;
deferred buffer cleanup retains its own context, including the explicit release
request when one exists. Buffer IDs are truncated to 64 characters and sanitized;
payloads and credentials are never included. Small counters may summarize
rejections, timeouts, dropped replies, outstanding async records and deferred
cleanup at shutdown or on a diagnostic request. Do not add periodic polling,
per-request INFO traces or a new externally exposed metrics contract.

Diagnostics must not hold registry/dispatcher/completion locks, block a socket
owner or require another allocation to finish an error/cleanup path. Failure to log cannot
change a ClientResult/RpcStatus, cancel backend access or lose cleanup ownership.

The Gateway logs each specific failure at **ERROR** where it is detected,
independently of whether its response can be sent. Include connection
token/generation, request_id, service/operation and opaque buffer ID when known;
record a bounded reason such as invalid_service_payload, buffer_quota_exceeded,
buffer_unavailable or buffer_busy. [Section 4.5](../../../doc/EyilSecureGateway.md#45-diagnostics-and-logging) defines suppression and content
restrictions. Do not log payloads, key material or shared-memory contents.

### Contract: DG-02 Worker capacity and retained-cleanup diagnostics

Source: [GW-007](../../../doc/EyilGateway.md#current-request-receive-pause-and-worker-timeout), [GW-051](../../../doc/EyilGateway.md#deactivation-sequence).

The timeout log must make saturation explicit: reason `worker_pool_exhausted`,
configured worker count, running/reserved counts, wait duration, request_id,
service and connection generation. It means the pool could not start the
request within ten seconds under the current load. Include lifecycle-worker
usage where relevant; slow or stuck backend calls can occupy the pool too.
Do not hide the condition behind a generic allocation error. Log any failure to
deliver the error separately; no second reply or service replay is attempted.

Emit `worker_pool_wait` at WARNING once when a request first cannot obtain
capacity. Both diagnostics distinguish running jobs from reserved handoffs that
have not started; running lifecycle jobs are included and also reported
separately. Compute `wait_ms` from the monotonic admission timestamp, rather than
printing the configured timeout as the observed duration.

A retained connection that prevents teardown is diagnosed with the
`cleanup_wait` warning defined in [section 4.5](../../../doc/EyilSecureGateway.md#45-diagnostics-and-logging). It identifies the wait cause and
active work counts once per cause, without an idle timer or automatic cleanup
retry. `cleanup_incomplete` remains an ERROR when finalization fails. Diagnostic
output does not alter the requirement to retain unresolved backend access.

## Related contracts and open decisions

[RP-02 Status layers and correlated Gateway failures](../rpc-protocol/spec.md#requirement-rp-02-status-layers-and-correlated-gateway-failures), [GR-02 Current-request admission and worker-start deadline](../gateway-routing/spec.md#requirement-gr-02-current-request-admission-and-worker-start-deadline), [GR-06 Single nonblocking response attempt](../gateway-routing/spec.md#requirement-gr-06-single-nonblocking-response-attempt), [SM-09 Buffer service error mapping](../shared-memory/spec.md#requirement-sm-09-buffer-service-error-mapping), [GL-06 Socket and context teardown](../gateway-lifecycle/spec.md#requirement-gl-06-socket-and-context-teardown).

