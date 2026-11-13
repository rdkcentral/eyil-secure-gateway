<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# shared-memory specification

## Purpose

Define Gateway allocation ownership, trusted-host-UID quotas, execution-time access leases, validation and deferred physical cleanup.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC1.9, AC1.10, AC1.11, AC1.12. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: SM-01 Allocation identity and trusted application cooperation

Gateway SHALL retain connection-owned allocation identity and references while applications follow the selected cooperation contract.

The detailed [contract](#contract-sm-01-allocation-identity-and-trusted-application-cooperation) is normative for this requirement.

#### Scenario: Allocation identity and trusted application cooperation outcome

- **WHEN** an application exits after allocating a buffer
- **THEN** Gateway cleans its own object through retained directory/object references after backend access ends, without rebuilding the cleanup path from a dead PID or granting another connection access.

### Requirement: SM-02 Shared trusted-host-UID quota

SharedBufferManager SHALL account allocations across every connection with the same trusted host UID, returning charges only after physical cleanup.

The detailed [contract](#contract-sm-02-shared-trusted-host-uid-quota) is normative for this requirement.

#### Scenario: Shared trusted-host-UID quota outcome

- **WHEN** two connections of one UID hold two buffers each and a new connection of that UID requests a fifth
- **THEN** allocation returns RPC_STATUS_OK with CRYPTO_ERROR_OPERATION_FAILED; reconnect does not reset accounting and pending/failed cleanup still consumes its slot.

### Requirement: SM-03 Region presence, overlap and output validation

Gateway and Crypto SHALL apply the operation-specific presence, range and overlap rules below, including the required-capacity exception.

The detailed [contract](#contract-sm-03-region-presence-overlap-and-output-validation) is normative for this requirement.

#### Scenario: Region presence, overlap and output validation outcome

- **WHEN** Crypto returns BUFFER_OVERFLOW with a required region beyond the current allocation
- **THEN** Gateway forwards the defined required offset/length unchanged as capacity information, not readable output; an out-of-bounds or overlapping successful output instead produces INTERNAL and ends definitively completed access.

### Requirement: SM-04 Exclusive allocation and backing-space reservation

Gateway SHALL prepare cleanup ownership, reserve quota and fully preallocate each object before publishing it to the application.

The detailed [contract](#contract-sm-04-exclusive-allocation-and-backing-space-reservation) is normative for this requirement.

#### Scenario: Exclusive allocation and backing-space reservation outcome

- **WHEN** fallocate fails with ENOSPC, ENOMEM or unsupported preallocation after file creation
- **THEN** Gateway logs the stage and returns RPC_STATUS_OK with OPERATION_FAILED without publishing an ID; it rolls back or retains failed-cleanup state and quota, never falls back to ftruncate-only allocation.

### Requirement: SM-05 Execution-time access and definitive completion

Gateway SHALL claim buffer access only when work executes and end the lease exactly once on definitive completion, preserving uncertain remote access.

The detailed [contract](#contract-sm-05-execution-time-access-and-definitive-completion) is normative for this requirement.

#### Scenario: Execution-time access and definitive completion outcome

- **WHEN** a direct in-process call returns an ordinary failed hresult or unwinds an exception
- **THEN** Gateway reports INTERNAL and ends its synchronous lease; a COM_ERROR or proxy exception retains an unresolved access obligation until completion evidence exists.

### Requirement: SM-06 Release acceptance and execution-time conflicts

Gateway SHALL atomically accept the first valid release, close the ID to new use and defer physical destruction under the state/race rules below.

The detailed [contract](#contract-sm-06-release-acceptance-and-execution-time-conflicts) is normative for this requirement.

#### Scenario: Release acceptance and execution-time conflicts outcome

- **WHEN** Release executes after Encrypt has claimed the buffer
- **THEN** release immediately acknowledges acceptance with RPC_STATUS_OK and CRYPTO_ERROR_NONE, while Encrypt retains its lease and quota remains charged until eligible cleanup; no pending status or later notification is added.

### Requirement: SM-07 Deferred finalization and cleanup failures

Deferred cleanup SHALL retain ownership through failures and coordinate once-only finalization with disconnect/drain notification ordering.

The detailed [contract](#contract-sm-07-deferred-finalization-and-cleanup-failures) is normative for this requirement.

#### Scenario: Deferred finalization and cleanup failures outcome

- **WHEN** filesystem cleanup fails after release was accepted
- **THEN** the accepted release stays successful, the ID remains unavailable and private state/quota are retained with ERROR diagnostics; cleanup does not send a second response or release unresolved backend access.

### Requirement: SM-08 Application release and abandoned allocations

Applications and Gateway SHALL follow the release and reclamation responsibilities below after completion, error or response loss.

The detailed [contract](#contract-sm-08-application-release-and-abandoned-allocations) is normative for this requirement.

#### Scenario: Application release and abandoned allocations outcome

- **WHEN** the application loses the release response or never receives an allocation ID
- **THEN** it cannot infer rejection or safely resume use; connection cleanup remains responsible for unpublished-to-the-app allocations and accepted releases without automatic replay.

### Requirement: SM-09 Buffer service error mapping

Recognized buffer operations SHALL return the following CryptoError service outcomes rather than collapsing them into infrastructure errors.

The detailed [contract](#contract-sm-09-buffer-service-error-mapping) is normative for this requirement.

#### Scenario: Buffer service error mapping outcome

- **WHEN** a request has an overlapping input region, a busy buffer or an exhausted shared UID quota
- **THEN** Gateway returns RPC_STATUS_OK with the operation-matching CryptoResponse and respectively INVALID_PARAMETERS, INVALID_SESSION_STATE or OPERATION_FAILED; no rejected use reaches Crypto.

### Requirement: SM-10 Scoped private-worker filesystem identity

Gateway SHALL use the trusted mapped host UID/GID for buffer creation and unlinking, with the complete credential scope defined below.

The detailed [contract](#contract-sm-10-scoped-private-worker-filesystem-identity) is normative for this requirement.

#### Scenario: Container-owned tmpfs

- **WHEN** a container's tmpfs rejects creation by unmapped host root
- **THEN** a private Gateway worker creates/preallocates the object using the authenticated peer's mapped filesystem identity and restores its original credentials before publication or ICrypto.

#### Scenario: Failure and deferred cleanup

- **WHEN** preparation fails or a pending release becomes eligible for cleanup on another private worker
- **THEN** each filesystem scope restores that worker's saved identity and capabilities; retained allocation UID/GID and directory references support cleanup without reopening procfs.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: SM-01 Allocation identity and trusted application cooperation

Source: [GW-022](../../../doc/EyilGateway.md#610-shared-memory-identity-records-and-invariants), [GW-023](../../../doc/EyilGateway.md#application-quota-shared-across-connections).

**Agreed application contract:** authenticated applications are trusted to
coordinate their buffer users and follow the allocation/use/release rules.
The app keeps the object name and size unchanged, does not unlink it, and does
not overwrite participating regions while Crypto is using them. Authentication
identifies the peer; this cooperation is an application responsibility, not a
protection enforced by EyilClient. Gateway still validates connection ownership,
ranges, states and quotas for every operation.

| Identifier | Example | Meaning |
| --- | --- | --- |
| buffer_id | Opaque generated token | Gateway lookup key, scoped to one connection generation |
| quota_key | Trusted host UID from ZAP peer credentials | Key of shared allocation accounting across all connections/processes with that UID; not an access token or Crypto appId |
| Client name | `/eyil.<random-token>` | POSIX name opened with shm_open() inside the app container |
| Backend path | `/proc/<host-peer-pid>/root/dev/shm/eyil.<random-token>` | Proposed host-visible path passed as SharedBuffer.name |
| Gateway FD | Process-local descriptor | Retains the created object; never sent as a bare integer to Crypto |
| Shared-memory directory FD | Gateway-owned reference to the original peer shared-memory directory | Supports allocation cleanup after peer exit without resolving /proc/PID/root again |
| BufferRegion | `{ offset, length }` | Byte range in the same allocation, not a pointer |

Each buffer record retains the owner token/generation, peer evidence, opaque ID,
client name, backend path, actual capacity, object FD, retained directory and
AppQuotaRecord references, active-use count, releasePending flag and
finalization/cleanup state. The active-use count is initially zero or one.
A pending release may coexist with active or unresolved access. Directory
references may be shared by a connection's allocations. Jobs retain their
context and buffer ID but reserve no buffer use before execution.

Use exclusive creation, generated names, close-on-exec descriptors and deliberate
ownership/mode settings. The shared-memory proposal sets the peer UID/GID and
owner read/write access. Verify user-namespace mappings and backend permissions
on the target platform; Gateway running in Thunder is not itself proof of the
required procfs access.

Retain the original shared-memory directory FD and generated relative filename
for Gateway cleanup, using unlinkat() against that directory after backend
access ends. Close the directory reference after its last allocation is cleaned
up. Do not rebuild cleanup paths using a dead or reused PID. Directory-relative
operations keep a stable directory reference; see
[openat](https://man7.org/linux/man-pages/man2/open.2.html) and
[unlinkat](https://man7.org/linux/man-pages/man2/unlink.2.html).

Required invariants:

- Only the owner connection may resolve a buffer_id; same UID/PID is insufficient.
- A connection may have multiple allocations. Allow at most one executing
  operation per allocation. A worker atomically checks ownership/state and
  takes the active-use lease when execution begins. A competing use fails with
  RPC_STATUS_OK with CRYPTO_ERROR_INVALID_SESSION_STATE and an ERROR log;
  it is not queued behind the active
  operation. A valid release is accepted and closes admission to that buffer;
  its destruction waits for the active use to end. Other allocations and
  connections remain independent.
- Check `offset <= size && length <= size - offset`; avoid unchecked
  `offset + length` arithmetic. Enforce the region presence and overlap rules
  below.
- Keep the active-use lease through the ICrypto invocation. A completed
  synchronous invocation, including a defined CryptoError, ends that access
  under the Crypto contract. Every definitive lease-ending path checks for a
  pending release and triggers eligible cleanup, including failure before a
  backend invocation. Release/disconnect must not invalidate Gateway's own live
  records. Operations that have not claimed a buffer have no lease: release may
  overtake them, and their execution-time lookup then fails without calling Crypto.
- The app must not resize, unlink or modify participating regions while an
  operation is outstanding. This is the selected cooperation contract; hard
  enforcement against a violating app is outside this version's scope.
- Gateway owns unlink/cleanup. An app unmaps/closes its own view; it does not
  call shm_unlink(). Unlink/close by Gateway also does not revoke an existing
  app mapping.
- A local client timeout is not a backend completion event. Gateway does not
  receive that timer event or prolong the client's wait; it lets the invocation
  finish and handles the result normally. An uncertain COM-RPC failure remains
  different from a completed invocation returning a defined service error.

The selected backend pathname remains /proc/<host-peer-pid>/root/dev/shm/eyil.<random-token>. The Gateway-FD pathname alternative is not selected. The retained path/PID-reuse rationale remains in the source guide; capture does not prove that a PID was not reused before capture.

### Contract: SM-02 Shared trusted-host-UID quota

Source: [GW-023](../../../doc/EyilGateway.md#application-quota-shared-across-connections).

For this limit, the application accounting group is **all connections with the
same trusted host UID from ZAP**. Child apps sharing their parent's UID share
the same four allocations. Use the host credential observed by Gateway, not
container-local UID 0, GID, PID, User-Id, sessionId, a caller-supplied value or
the generated per-connection `eyil-interim:` appId. This quota key does not
require an AppManager lookup and does not replace Crypto's ApplicationContext
or establish API/key authorization.

SharedBufferManager owns `AppQuotaMap[host_uid]`. A connection retains its quota
key; allocation reservations and records retain the associated AppQuotaRecord,
containing charged allocation count and actual capacity bytes. Obtain/create
that shared record under the manager's synchronization, so concurrent first
allocations from two connections cannot create independent allowances.

- Validate `0 < capacity <= 1,572,864` before admission. If allocation size is
  rounded, validate and charge the final logical capacity, not only the request.
- Atomically reserve one of the four slots and its capacity before filesystem
  creation. Include unpublished reservations, live allocations, release-pending
  allocations and failed-cleanup records in the charged count. Account bytes
  alongside the count; the two limits imply at most 6,291,456 bytes per UID.
- Complete rollback returns its reservation once. If an object or owned cleanup
  resource remains, retain the record and charge until physical cleanup finishes.
  A lost allocation reply does not refund a live allocation.
- Release acceptance, client timeout or disconnect alone does not return a slot.
  Only completed physical cleanup of that allocation returns its charge, once.
  Closing one connection cleans its own allocations without resetting the UID's
  shared counters or touching buffers belonging to another connection.
- A new connection with the same UID joins existing accounting, including any
  cleanup retained from older connections. UID reuse must not reset outstanding
  charges or grant access to old buffers. Retire a quota-map entry only when its
  charges and all allocation-attempt references are gone, synchronized with
  concurrent lookup/reservation; never leave a charged record outside the map.

Sharing quota does **not** share buffer access: only the original owner
connection/generation may use or release its buffer_id. Each connection retains
its independent Crypto session and disconnect lifecycle.

Quota rejection uses RPC_STATUS_OK with CRYPTO_ERROR_OPERATION_FAILED and an
ERROR log containing host UID, connection generation, requested capacity,
charged count/bytes and limits. A per-buffer size excess instead uses
CRYPTO_ERROR_OVERSIZED_INPUT_DATA; zero size uses
CRYPTO_ERROR_INVALID_PARAMETERS, as specified in [section 4.2](../../../doc/EyilSecureGateway.md#42-status-and-error-layers).

### Contract: SM-03 Region presence, overlap and output validation

Source: [GW-024](../../../doc/EyilGateway.md#region-presence-and-overlap-rules), [GW-028](../../../doc/EyilGateway.md#612-shared-memory-operations-common-backend-boundary).

These rules implement the shared-memory proposal's non-overlapping inputs
(section 7) and plugin-selected output placement (section 9). In-place
processing is not supported in this version.

| Operation | Required input regions | Inputs that must not overlap each other | Output region | Output must not overlap |
| --- | --- | --- | --- | --- |
| Encrypt | init_data, input_data | init_data and input_data | encrypted_data | init_data, input_data |
| Decrypt | init_data, encrypted_data | init_data and encrypted_data | output_data | init_data, encrypted_data |
| Sign | input_data | (single input) | signature | input_data |
| Verify | input_data, expected_signature | input_data and expected_signature | none | (no output region) |

1. **Presence:** every required region field must be present in the request.
   A missing required region is CRYPTO_ERROR_INVALID_PARAMETERS.
2. **Bounds:** every present region satisfies `offset <= size && length <=
   size - offset`, including a region of length 0.
3. **Zero length:** Gateway accepts a present region of length 0 and passes it
   to Crypto. Whether the algorithm accepts it is Crypto's decision; for example
   AES-CBC rejects empty init_data with CRYPTO_ERROR_INVALID_INIT_DATA. A region
   of length 0 never overlaps another region.
4. **Overlap test:** two non-empty regions `[a, a + la)` and `[b, b + lb)`
   overlap when `a < b + lb && b < a + la`. Compute the sums in 64-bit
   arithmetic. Adjacent regions (`a + la == b`) do not overlap.
5. **Gateway input check:** in TryUse, after bounds and presence, reject
   overlapping inputs with CRYPTO_ERROR_INVALID_PARAMETERS and an ERROR log
   naming both regions. Rejection takes no active-use lease and makes no Crypto
   call.
6. **Gateway output check:** after a completed invocation with
   CRYPTO_ERROR_NONE, a non-empty output region must be inside the allocation
   and must not overlap any non-empty input region. A violation is an invalid
   successful output region: ERROR log and RPC_STATUS_INTERNAL, with the region
   not exposed. The lease still ends normally.
7. **BUFFER_OVERFLOW:** the returned required region describes capacity, not
   written data. It is exempt from the bounds and overlap checks and is
   forwarded unchanged.
8. **Crypto:** independently applies the same bounds and overlap rules to the
   actual opened object, chooses an output that overlaps no input, and writes
   only inside the output region it returns. Input regions remain unchanged.
9. **Client:** declare every region holding data that must survive the
   operation. Bytes outside the declared input regions may be overwritten by
   the output. After success, read only the returned output region; the input
   regions can be read back unchanged.

The topic/crypto ICrypto contract selects output locations in Crypto. Gateway
forwards the returned region only after validating the invocation and service
result. On success, output must be inside the allocation. A BUFFER_OVERFLOW
result may describe required capacity rather than an accessible output range.
Preserve CRYPTO_ERROR_BUFFER_OVERFLOW and the defined required offset/length
in the operation response even when that region exceeds the current capacity;
do not apply the successful-output bounds rejection to it. The app must not
read it as produced data. Compute required capacity with checked/wider
arithmetic; the app can explicitly allocate a sufficiently large new buffer,
rewrite inputs and submit a new operation, subject to the configured limits.
The library/Gateway does not replay automatically or resize a mapped buffer.

An invalid successful output region, including one that overlaps an input
region, is a Gateway processing failure: log ERROR
and return RPC_STATUS_INTERNAL without exposing that region as usable output.
Definitive backend return still ends the lease and can complete pending release;
response validation or serialization must not leave completed access active.

| Operation | App writes before the request | ICrypto call | Output returned | BUFFER_OVERFLOW possible | App reads on success |
| --- | --- | --- | --- | --- | --- |
| Encrypt | init_data, plaintext input_data | Encrypt(context, buffer, algorithm, keyId, initData, inputData) | encrypted_data region | Yes | Ciphertext region |
| Decrypt | init_data, encrypted_data | Decrypt(context, buffer, algorithm, keyId, initData, encryptedData) | output_data region | Yes | Plaintext region |
| Sign | input_data | Sign(context, buffer, algorithm, keyId, digestAlgorithm, inputData) | signature region | Yes | Signature region |
| Verify | input_data, expected_signature | Verify(context, buffer, algorithm, keyId, digestAlgorithm, inputData, expectedSignature) | verification_status | No | verification_status only, no binary output region |

### Contract: SM-04 Exclusive allocation and backing-space reservation

Source: [GW-027](../../../doc/EyilGateway.md#611-allocatesharedbuffer-and-client-mapping).

Prepare a retained allocation/cleanup record before creating the filesystem
object. Failed creation rolls back resources and quota; if physical rollback
itself fails, keep that private record and the unreclaimed resource charges for
connection cleanup, log ERROR and return RPC_STATUS_OK with
CRYPTO_ERROR_OPERATION_FAILED without publishing a buffer ID. The charge belongs
to the UID's shared quota record even if this connection has already closed.
Failure reporting must not lose the only reference to a partially created object.
This service allocation failure is distinct from failure to construct/send
its response, which follows the infrastructure fallback in [section 4.2](../../../doc/EyilSecureGateway.md#42-status-and-error-layers).

After exclusive creation, set the logical size with ftruncate(), then call
`fallocate(fd, 0, 0, capacity)` for the complete allocation before publishing
the buffer ID/name or handing the object to the app. Create using the mapped
peer filesystem UID/GID and mode 0600 as specified in SM-10; keep the random name
unpublished during preparation. Restore Gateway credentials, verify the agreed
peer ownership/permissions and final size, then publish. The peer UID owns the
file from creation, so initialization relies on the existing trusted-app
cooperation contract rather than claiming enforced Gateway-only access.
ftruncate alone does not reserve
tmpfs space; this step detects insufficient backing capacity during allocation
instead of leaving Crypto to discover it while accessing a mapped buffer.
See [fallocate](https://man7.org/linux/man-pages/man2/fallocate.2.html).

Treat ENOSPC, ENOMEM, unsupported preallocation and other terminal fallocate
errors as allocation failure: log ERROR with stage, errno and requested size,
roll back the unpublished object and quota, and return RPC_STATUS_OK with
CRYPTO_ERROR_OPERATION_FAILED and the allocation response oneof. Keep the private
cleanup record and charges if rollback cannot finish. Never silently fall back
to ftruncate-only allocation or publish a partly preallocated object. A
recoverable interruption may be retried on the same unpublished object, with
stop and connection state rechecked before publication.

The application implements its buffer mapping helper outside this repository
and the EyilClient transport API. It also handles allocation success followed
by mapping failure: request release when possible, otherwise correlated
connection cleanup reclaims the record.

If the connection closes while creation is in progress, recheck its generation
before publishing the result and clean up an allocation that cannot be admitted.
If an allocation reply is lost, the Gateway may own a buffer unknown to the app;
quota accounting and eventual connection cleanup must still cover it.

### Contract: SM-05 Execution-time access and definitive completion

Source: [GW-028](../../../doc/EyilGateway.md#612-shared-memory-operations-common-backend-boundary).

For all four operations, the app writes inputs before sending the request.
When a worker executes the request, SharedBufferManager atomically resolves
buffer_id, validates owner/ranges/state and claims the idle allocation. Missing,
unowned, released, release-pending or busy buffers return RPC_STATUS_OK with
CRYPTO_ERROR_INVALID_SESSION_STATE and an ERROR log before any ICrypto call.
Invalid input regions use CRYPTO_ERROR_INVALID_PARAMETERS. Operation-specific
limits belong to the Crypto adapter/backend, not SharedBufferManager. Preserve
CRYPTO_ERROR_OVERSIZED_INPUT_DATA when an input exceeds the operation maximum,
using the same RPC_STATUS_OK service envelope. Any adapter rejection after
claiming a buffer must finish its unused lease. Do not reserve buffers for
jobs before execution, wait for a conflict to clear or replay failed work. After a
successful claim, Gateway passes the trusted backend path and regions to the
Crypto adapter.

An accepted release can retire the ID while that invocation runs; it preserves
the existing active-use lease and backing object until access ends.

FinishUse ends that lease exactly once and checks releasePending. It triggers
eligible cleanup outside manager locks, or records readiness for connection
cleanup if disconnect/deactivation owns finalization. Invoke it on every
definitive path, including null lookup and failure before calling Crypto, a
returned service error and later response-processing failure. It does not wait
for reply delivery. [Section 6.13](../../../doc/EyilGateway.md#613-releasesharedbuffer-and-conflicting-buffer-operations) defines the finalization and error contract.

For Thunder R4, an ordinary failed Core::hresult is a completed method return:
report RPC_STATUS_INTERNAL and end the lease. COM-RPC transport failures carry
the COM_ERROR flag and conservatively retain the access obligation. A C++
exception unwinding a direct in-process call also ends its synchronous access;
an exception from a Thunder proxy does not establish remote completion. Identify
the proxy locally without another interface lookup or backend call. These
distinctions preserve the [backend access contract](../crypto-integration/spec.md#requirement-cy-06-backend-shared-buffer-access-contract) and the unresolved remote
completion boundary in [section 7.1](../../../doc/EyilSecureGateway.md#71-abnormal-backend-completion-lifecycle-integration-boundary).

A verification result of false and a failed invocation are different outcomes.
For every operation, a transport failure after remote dispatch may leave
completion uncertain. Do not take the normal FinishUse arrow in that case;
retain the access obligation and any accepted pending release until backend
completion is established. A definitive error result ends access and permits
deferred cleanup just as a successful operation does.

### Contract: SM-06 Release acceptance and execution-time conflicts

Source: [GW-030](../../../doc/EyilGateway.md#release-acceptance-and-deferred-destruction), [GW-031](../../../doc/EyilGateway.md#atomic-state-and-execution-time-conflicts).

**Agreed release contract:** validate the buffer ID and owning connection, then
atomically set releasePending. From that instant, no new operation may acquire
the allocation. If no active access remains, Gateway cleans up immediately,
subject to the connection-teardown ordering below. Otherwise, it retains the
object, directory reference, record and quota until the last active use ends.
The completion path then performs cleanup; no worker waits for another operation,
no polling timer runs and no application retry is required.

Return RPC_STATUS_OK with CRYPTO_ERROR_NONE and the empty
ReleaseSharedBufferResponse once release has been accepted. Do not wait for an
active operation to finish before producing this response. Success means **the
ID is retired from further use and Gateway has accepted cleanup responsibility**.
It does not certify that the object has already been unlinked, that backend
execution ended or that kernel memory has been reclaimed. No pending status,
additional response, notification or Protobuf/API extension is required.

The deferred period has no fixed deadline. The 10-second worker-wait budget
applies before the release job starts; expiry there rejects an unexecuted
request and creates no pending release. The 15-second client wait does not
cancel an accepted release or authorize destruction. Once accepted, the release
remains pending until its cleanup prerequisites are satisfied, independently of
whether the response reaches the client.

SharedBufferManager protects lookup, ownership, active-use count,
releasePending and finalization claims with short locks. It holds no such lock
across ICrypto, filesystem operations or response delivery. Store these fields
in the retained allocation record: recording releasePending and ending a use
must not depend on allocating a separate pending-release job.

| State / event | Atomic transition and outcome |
| --- | --- |
| Valid use of an idle allocation with releasePending false | Claim the sole active-use lease; activeOperations becomes 1 |
| Competing use while activeOperations is 1 | RPC_STATUS_OK with CRYPTO_ERROR_INVALID_SESSION_STATE and ERROR log; no wait and no Crypto call |
| First valid release, with activeOperations either 0 or 1 | Set releasePending, reject all later uses and accept release; retain any active lease |
| New use after releasePending is set | RPC_STATUS_OK with CRYPTO_ERROR_INVALID_SESSION_STATE and ERROR log, even if the original operation has since ended |
| Unknown/unowned ID or another release after release was already accepted | RPC_STATUS_OK with CRYPTO_ERROR_INVALID_SESSION_STATE and ERROR log; leave the original release and active use unchanged |
| Definitive end of active access | Decrement activeOperations once; if releasePending is false the allocation becomes idle, otherwise trigger eligible finalization |
| releasePending, activeOperations zero and lifecycle ordering permits cleanup | One worker claims finalization under the lock, then cleans up outside it; other paths cannot claim it again concurrently |
| Physical cleanup succeeds | Remove the private cleanup record, return only this allocation's count/byte charge to its shared host-UID quota record, and drop retained references exactly once |
| Physical cleanup fails | Keep the ID unavailable, retain unresolved cleanup/accounting and log ERROR; never restore the allocation to use |

Uncertain backend access retains its active-use obligation until completion is
established; returning an invocation error or releasing ICrypto is not itself
that evidence. It may coexist with releasePending. [Section 7.1](../../../doc/EyilSecureGateway.md#71-abnormal-backend-completion-lifecycle-integration-boundary) describes this
exceptional lifecycle boundary.

**Operations awaiting execution have no buffer reservation.** Check the buffer when a job
executes. Gateway does not promise request-arrival ordering, delay release to
preserve a not-yet-started use or replay a rejected operation.

| Race at the buffer manager | Result |
| --- | --- |
| Release is accepted before an admitted Encrypt claims the buffer | The ID becomes unavailable; Encrypt returns RPC_STATUS_OK with CRYPTO_ERROR_INVALID_SESSION_STATE without calling Crypto |
| Encrypt claims the buffer before Release executes | Encrypt retains its lease; Release is accepted immediately, with destruction deferred until that use ends |
| Two operations try to claim the same idle buffer | One claims it; the other returns RPC_STATUS_OK with CRYPTO_ERROR_INVALID_SESSION_STATE if it observes active use |
| A later operation executes after the previous use ends, without an accepted release | It may claim the still-live buffer and proceed normally |
| Release races the last active-use completion | Either path may find the object ready for cleanup, but exactly one claims finalization; no lost release or double cleanup |
| The app removes the object outside Gateway before Crypto opens it | Crypto handles an open/access failure if detected; Gateway logs and forwards the returned CryptoError and ends the lease after definitive completion |

The application submits dependent operations sequentially when it needs a
particular order. Different buffers and connections can execute concurrently.
Deferred destruction protects an existing active use; it does not add a buffer
dependency queue or promise that an admitted operation will succeed.

### Contract: SM-07 Deferred finalization and cleanup failures

Source: [GW-033](../../../doc/EyilGateway.md#completion-cleanup-failures-and-response-ownership), [GW-034](../../../doc/EyilGateway.md#coordination-with-disconnect-and-plugin-drain).

Finish the active-use obligation on every path that establishes no backend
access remains: failure before invocation, null ICrypto lookup, or definitive
synchronous return, including a CryptoError or verification result of false.
A later output-validation, serialization or response-allocation failure must
not strand that lease. Use retained/scoped ownership so it is ended once even
on error paths. If releasePending is set, run eligible finalization on the
current worker outside manager locks. Otherwise return the allocation to idle.
No request worker waits for another worker to free the allocation.

An ambiguous remote invocation failure keeps the access obligation and pending
release intact. When backend/platform lifecycle handling establishes completion,
the same finalization path becomes eligible. Neither a client timeout, connection
loss nor the passage of a fixed interval provides that evidence.

Unlink through the retained directory reference and generated relative name;
then release Gateway's remaining object/reference ownership. A missing name
(ENOENT) is already unlinked and does not by itself prevent closing retained
references and completing accounting. A different filesystem cleanup failure
keeps private state describing exactly what remains, and emits an ERROR log
with the buffer ID, owning connection and failed step. Do not double-close a
released descriptor, remove quota twice or make the ID usable again. The normal
connection cleanup path can revisit retained failed cleanup; there is no
periodic cleanup poller or client-driven replay requirement.

| Failure / outcome | Response and resource handling |
| --- | --- |
| Release rejected for unknown/unowned/already-released ID | RPC_STATUS_OK with CRYPTO_ERROR_INVALID_SESSION_STATE and ERROR log; no new releasePending transition |
| Release rejected before dispatch because of worker-wait timeout or infrastructure failure | RPC_STATUS_INTERNAL with empty payload and ERROR log; no releasePending transition for this request |
| Valid release accepted while idle, busy or awaiting definitive backend completion | Successful release response acknowledges acceptance; keep resources and quota until eligible physical cleanup completes |
| Filesystem cleanup fails after acceptance, even during the immediate idle-buffer path | Log ERROR and retain private cleanup/accounting; the accepted-release result stays successful, with no second response |
| Release response allocation/serialization fails after acceptance | Preserve pending cleanup; report RPC_STATUS_INTERNAL if a correlated fallback can be built before any send attempt, otherwise log/drop; no rollback or automatic replay |
| Release response send fails, or client has already timed out/disconnected | Log/drop under the single-attempt rule; cleanup continues without an ACK from the client |
| Original Crypto operation returns a defined error while release is pending | Forward its CryptoError through its own request; end active use and finalize when eligible |
| Original Crypto invocation leaves remote access uncertain | RPC_STATUS_INTERNAL for that operation when deliverable; retain its access obligation and any pending release until completion is established |

Cleanup failure does not overwrite an earlier accepted release or the original
Crypto result. Success does not revoke an app mapping or certify that the kernel
freed pages still referenced by another process. Quota is released only when
Gateway's allocation cleanup completes, rather than on response creation or
send acceptance.

Connection teardown retains the agreed ordering: settle accepted work and
active access, notify Crypto with DISCONNECTED, then finalize the connection's
remaining allocations. At the admission cutoff, SharedBufferManager records
lifecycle ownership atomically against finalization claims. A finalizer already
claimed by an earlier explicit release finishes as part of that earlier work.
Still-unclaimed records, including accepted pending releases, are finalized by
connection cleanup after the notification stage. Last-use completion records
readiness and wakes that cleanup path without blocking a worker.

Closing admission alone does not mark every buffer released: previously
accepted operations that have not started may still use allocations without releasePending,
subject to their execution-time checks and original dispatch budget. An accepted
explicit release still rejects later uses of its ID. Release jobs admitted before
the cutoff may record acceptance during drain without waiting for physical
cleanup. After accepted work settles, connection cleanup marks all remaining
allocations for release. App exit, a lost allocation reply or an omitted release
RPC therefore leaves cleanup owned by Gateway, not by the vanished client.

### Contract: SM-08 Application release and abandoned allocations

Source: [GW-025](../../../doc/EyilGateway.md#application-cleanup-after-completion-or-error).

When an allocation is no longer needed, including when abandoning it after an
error, the app-side Crypto layer stops further use, unmaps/closes its local view
and sends ReleaseSharedBufferRequest(buffer_id) through EyilClient. The response
must be checked for release acceptance. Success retires the ID from further
use; it does not mean that active backend access or physical cleanup has already
finished. Freeing a message with client_free_response() does not release a shared
allocation. For an async release call, CLIENT_OK confirms only libzmq send
acceptance; inspect the eventual RPC/service response for Gateway acceptance.

This applies when the app knows the buffer ID and still has a usable connection.
An error or the 15-second client timeout does not force an active Crypto call to
finish. Gateway accepts a valid release while access is active or unresolved,
marks releasePending and retains the object until access is known to have ended.
The app does not need to resubmit merely because the buffer was busy. Accepted
release does not cancel the original operation, authorize further reads/writes
of that allocation or certify readiness to freeze a process with uncertain work.
The app must not consume a later operation result's regions from a buffer it has
abandoned. Ordinary completion, including a returned CryptoError, triggers
eligible deferred cleanup without waiting for the app to receive the result.

If the release response is lost or its client wait times out, the app cannot
infer that release was rejected; it may already be pending or complete. Do not
resume buffer use or automatically replay the release. Closing the connection
transfers any remaining reclamation to Gateway's drain/cleanup path. A successful
release reply also does not promise immediate quota availability for another
allocation: pending resources still count against the limits.

If allocation fails before an ID is published, Gateway rolls back its partial
allocation. If the allocation succeeded but its response was lost, the app has
no ID to release; connection teardown reclaims that allocation as well. After
unexpected connection loss, cleanup needs no release RPC from the app. A fresh
connection cannot release or reuse the old connection's IDs.

### Contract: SM-09 Buffer service error mapping

Source: [OV-019](../../../doc/EyilSecureGateway.md#gateway-faults-buffer-service-outcomes-and-explicit-error-logging).

The following buffer/service outcomes follow the shared-memory proposal's
error model. Every row returns **RPC_STATUS_OK plus CryptoResponse.error**,
not an empty RPC error payload:

| Condition | CryptoError | Required handling/log detail |
| --- | --- | --- |
| Zero allocation size | CRYPTO_ERROR_INVALID_PARAMETERS | Log the invalid size; create no object |
| Requested allocation exceeds the per-buffer maximum | CRYPTO_ERROR_OVERSIZED_INPUT_DATA | Log requested size and maximum; create no object |
| Allocation, object creation/sizing/preallocation or shared host-UID quota failure | CRYPTO_ERROR_OPERATION_FAILED | Log stage/errno or host UID, connection, charged count/bytes and limit, including fallocate failure; roll back partial allocation and retain any unresolved cleanup record and quota charge |
| Missing, released or wrong-session buffer ID | CRYPTO_ERROR_INVALID_SESSION_STATE | Log failed lookup/ownership/state; do not call Crypto |
| Invalid caller-supplied input region: missing, out of bounds or overlapping another input | CRYPTO_ERROR_INVALID_PARAMETERS | Log the failed presence, bounds or overlap check; do not expose or access that region |
| Input exceeds the operation's maximum | CRYPTO_ERROR_OVERSIZED_INPUT_DATA | Log input length and operation limit |
| Crypto-selected output does not fit | CRYPTO_ERROR_BUFFER_OVERFLOW | Preserve the required output region when known; it is not readable output |
| Completed generic Crypto service failure | CRYPTO_ERROR_OPERATION_FAILED | Preserve this result when returned by Crypto; retain any more specific returned CryptoError unchanged |
| Competing use of a busy buffer, or new use after releasePending | CRYPTO_ERROR_INVALID_SESSION_STATE | Log active/release state; no wait, new lease or Crypto call |
| Repeated release after a release was already accepted | CRYPTO_ERROR_INVALID_SESSION_STATE | Preserve the existing release/active use; do not create another finalizer |

The proposal does not assign separate values for busy or repeated-release
cases. This spec uses the existing invalid-session-state value for those
unavailable buffer states; no new busy enum is added. A **first
valid release succeeds even while the buffer is in use**. It sets releasePending
and defers physical destruction as specified in [section 6.13](../../../doc/EyilGateway.md#613-releasesharedbuffer-and-conflicting-buffer-operations).

For a service result, select the response oneof matching the recognized
operation. On an allocation error, return no usable buffer ID/name/capacity;
on another error, operation outputs are not usable except the required-capacity
region explicitly allowed for BUFFER_OVERFLOW. Invalid transport context still
uses RPC_STATUS_INVALID_CONNECTION; it is distinct from a buffer ownership/state
failure inside an otherwise valid connection. A service error is a delivered
RPC response: EyilClient returns CLIENT_OK or invokes the response callback,
and Firebolt inspects both RpcStatus and CryptoError.

Rejected buffer work must not invoke Crypto, overwrite another request's state
or invalidate active leases. Roll back partial reservations. If constructing
a service-error response itself fails, report RPC_STATUS_INTERNAL only if a
correlated fallback can be prepared before any send attempt; otherwise log/drop.
Neither fallback nor response loss undoes side effects or releases active memory.

### Contract: SM-10 Scoped private-worker filesystem identity

Implementation: [SharedBuffers.cpp](../../../src/gateway/SharedBuffers.cpp).
This namespace compatibility fix refines the allocation and cleanup flows in
[section 6.11](../../../doc/EyilGateway.md#611-allocatesharedbuffer-and-client-mapping)
and [section 6.13](../../../doc/EyilGateway.md#613-releasesharedbuffer-and-conflicting-buffer-operations).
Original AC1.9–AC1.12 and the public API remain unchanged.

- Filesystem credential changes run only on Gateway-owned private-pool workers.
  ROUTER, ZAP, monitor and Thunder framework/global-pool threads do not enter
  these scopes. Do not change effective/real/saved process UID/GID or join a
  container user namespace in a Thunder thread.
- Resolve/open the peer's `/proc/<pid>/root/dev/shm` directory before switching
  filesystem identity. Retain its FD. Use the trusted host UID/GID from ZAP,
  never a caller-provided ID or the container's internal UID 0.
- Save the executing thread's filesystem UID/GID and capability sets. Apply
  `setfsgid()` followed by `setfsuid()` and explicitly query both to detect
  silent rejection. Run exclusive creation, truncate and full preallocation
  inside the scope through the retained directory FD.
- Restore the saved filesystem UID before GID, restore the exact saved
  capability state, and verify restoration on every exit, including failed
  entry and preparation errors. Preserve the operation errno. Restore before
  ownership verification, publication, response processing, ICrypto or worker
  reuse. Perform no unrelated application/framework calls inside a normal scope.
- A detected entry failure creates no object and uses the existing
  `RPC_STATUS_OK` / `CRYPTO_ERROR_OPERATION_FAILED` allocation path. Rollback
  retains its existing accounting/ownership rules. Recheck restoration once on
  failure; if it still cannot be verified, log an unrecoverable error and stop
  the process rather than return a worker with an unknown identity to its pool.
- Retain both UID and GID in each allocation record. Explicit release,
  allocation rollback, deferred finalization and disconnect cleanup use that
  identity only around `unlinkat()` on the retained directory FD. Restore before
  closing descriptors, updating accounting or logging an ordinary cleanup error.
  Failed cleanup keeps the allocation charge and record; no forced destruction
  or new retry policy is introduced.
- Gateway retains allocation lifecycle ownership and the descriptor. Filesystem
  ownership remains the app UID/GID with mode 0600. Input/output cooperation,
  per-host-UID quotas, connection ownership and backend access rules remain in
  force; a credential scope does not cancel Crypto or revoke app mappings.

## Related contracts and open decisions

[CY-06 Backend shared-buffer access contract](../crypto-integration/spec.md#requirement-cy-06-backend-shared-buffer-access-contract), [CY-07 On-demand lookup failures and recovery boundary](../crypto-integration/spec.md#requirement-cy-07-on-demand-lookup-failures-and-recovery-boundary), [GL-04 Correlated disconnect and backend notification order](../gateway-lifecycle/spec.md#requirement-gl-04-correlated-disconnect-and-backend-notification-order), [GL-05 Admission cutoff and graceful drain](../gateway-lifecycle/spec.md#requirement-gl-05-admission-cutoff-and-graceful-drain), [DG-01 Severity, context and confidentiality](../diagnostics/spec.md#requirement-dg-01-severity-context-and-confidentiality).

- [SHM-COMPLETION](../../../doc/EyilSecureGateway.md#71-abnormal-backend-completion-lifecycle-integration-boundary): unresolved; retain the safeguards above without inventing the missing integration contract.
