<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Validation

## Completed checks

- Native Gateway, client, diagnostic tools and test targets build successfully.
- Full CTest run: 154/154 passed, comprising 120 L1 and 34 L2 cases.
- Seven new allocator cases cover mapped create/unlink identities, silent entry
  rejection and preparation failures, restoration retry, retained failed cleanup,
  concurrent identities, non-default saved filesystem IDs and deferred cleanup
  using the allocation's captured GID. Existing allocation-memory, rollback,
  quota, ownership and lifetime regressions also pass.
- L2 includes the real Thunder wrapper/implementation and diagnostic-backend
  smoke test, local/shared-memory operations, missing backend and lifecycle cases.
- Repository formatting/licensing/JSON/YAML/source checks pass.
- Strict OpenSpec validation passes for all ten main specs and this change.
  The informational ADDED-already-exists message reflects SM-10 having already
  been applied to the main spec; archive must assess synchronization first.
- `git diff --check` passes. Original AC text, client/proto/ICrypto APIs and
  README.md are unchanged. No commit, deployment or archive was performed.

The existing native build cache initially selected a stock ZeroMQ library for
the lifecycle tests. Reconfiguring it to the provided patched libzmq 4.3.4
resolved that dependency failure; the complete suite then passed. This required
no build-rule or dependency-recipe source change.

## Device boundary

The earlier diagnostic reproduced rejected host-root creation and successful
creation using the mapped app identity. That established the cause; it was not
a run of the newly built Gateway. Foreign IDs in the new native unit tests are
simulated by linker-only, thread-local wrappers. Native unprivileged user/mount
namespace setup is unavailable in this environment (EPERM).

Validate the rebuilt image inside an application container: buffer allocation,
Crypto read/write, explicit release, busy deferred release and disconnect
cleanup, with unchanged identities on other threads and successful subsequent
requests from other app UIDs. Bypass the separately identified DobbyInit exec
cleanup defect, or fix it before using DobbyTool for a finite diagnostic command.
