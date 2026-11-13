<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Design

## Ownership and scope

SharedBuffers mutations are reached through GatewayRuntime::Execute on the
Gateway-owned private Thunder pool. Transport, ZAP/monitor, wrapper/framework
and global-pool threads do not run filesystem identity scopes. Keep procfs
opening outside the scope; captured directory FDs avoid a second traversal
under peer credentials. Creation, truncate/preallocation and unlink are narrow,
synchronous scopes without a backend/application callback.

## Credential guard

Capture filesystem UID/GID and Linux capability version-3 data for the current
thread. Set filesystem GID before UID using the trusted ZAP host values. Query
both IDs to detect the setters' silent rejection. Restore UID before GID,
restore the exact captured capability sets and verify every state. Preserve
errno through destruction. Capability capture failure changes no credentials.
Partial entry and every normal/error return restore before further work.
Retry restoration once on verification failure. If still invalid, log and
terminate: continuing with an unknown worker identity is unsafe. This is an
unrecoverable invariant failure, not an ordinary allocation/service failure.
Do not introduce process-wide setuid/setgid calls or enter the user namespace.

## Allocation and cleanup

Capture both peer UID and GID in the allocation record. Create mode 0600 and
preallocate before publishing. The object is owned by the mapped peer UID from
creation; there is no claim of enforced Gateway-only access during preparation.
The existing cooperating-app rule covers the unpublished random object.
Restore credentials before fchown/fchmod verification, final fstat, publication,
response handling or ICrypto. Gateway keeps descriptor/lifecycle ownership.

Finalize uses the retained directory and allocation UID/GID around unlinkat,
including rollback, busy deferred release and disconnect cleanup. Restore
before closing FDs or updating quota. Failed scope entry or unlink retains
record/quota ownership under the existing cleanup policy.

## Verification

Use linker-only test wrappers to simulate mapped credentials and silent setter
rejection without granting native tests root. Observe credentials at procfs
open, create, ownership finalization and unlink. Exercise failure rollback,
restoration retries, non-default saved filesystem IDs, deferred cleanup and
concurrent workers. Run native L1/L2 against patched libzmq and optional tools.
A rebuilt-image container run remains distinct from native/fault-injection tests.

## Detailed contract amendments

The main spec detailed clauses were updated during apply. They must remain in
place when merging the requirement deltas; archive does not import standalone
detailed sections. The exact canonical allocation and credential-scope clauses
are linked from the deltas.

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
