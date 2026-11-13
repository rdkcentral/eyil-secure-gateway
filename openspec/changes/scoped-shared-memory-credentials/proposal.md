<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Scoped shared-memory filesystem credentials

## Why

A private tmpfs mounted in an app user namespace can reject host-root creation
with EOVERFLOW because host UID 0 has no mapping there. The device diagnostic
confirmed creation succeeds with the authenticated app's mapped host identity.
Gateway must support this without changing credentials on framework threads.

## What Changes

- Scope filesystem UID/GID changes to creation/preallocation and unlinking on
  Gateway's private workers; retain the original directory and object FDs.
- Restore and verify each worker's saved filesystem IDs and capability state.
- Preserve quota, deferred-release and error contracts; retain UID and GID for
  cleanup. Unrecoverable restoration failure cannot return a dirty worker.
- Clarify that the object is app-owned from creation and its random name remains
  unpublished until preparation completes under the existing cooperation model.
- Add credential rejection, restoration, concurrent-worker and cleanup regressions.

## Capabilities

### Modified Capabilities

- `shared-memory`: add SM-10 and refine SM-04 initialization semantics.

## Impact

AC1.9–AC1.12 keep their original wording. No client/proto/ICrypto API, buffer
quota or namespace path changes. No process-wide UID/GID change or new library
dependency. The independent DobbyInit exec-wrapper failure is outside this fix.
