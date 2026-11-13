<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Tasks

These tasks perform a documentation-only migration. Checkboxes track completion. Follow
[design.md](design.md), the complete [coverage map](coverage-map.md) and its
[source snapshot](source-baseline.json). No production, API, schema, test,
build/packaging or external-repository changes are included. Keep README.md empty.

There are no change delta specs: skip_specs is declared. Author the existing
baseline directly under openspec/specs/ during apply. Original doc/ prose stays
until its replacement has been reviewed; source sections may share destinations.

## 1. Verify the migration baseline

- [x] 1.1 Compare all seven source-file hashes with source-baseline.json and record the pre-apply worktree state. Verify any intervening changes have an explicit coverage-map amendment, and preserve unrelated changes.
- [x] 1.2 Verify the inventory covers every source heading and line range, the 19 AC rows, all six open-decision IDs and the 14 patch blocks. Confirm the three protected AC hashes and all 35 diagram hashes before editing.
- [x] 1.3 Prepare a local pre-apply snapshot of files to be edited for selective rollback. Verify it includes uncommitted OpenSpec setup and does not require a Git reset, commit or external repository edit.

## 2. Extract the protocol and client contracts

- [x] 2.1 Create openspec/specs/rpc-protocol/spec.md from Overview 4.1–4.4 and the mapped client/Gateway wire rules. Verify exact fields/status values, schema 1, framing, correlation, duplicate handling and raw-string rejection against every source table; add requirement/scenario anchors to all affected coverage rows and run strict validation for this spec.
- [x] 2.2 Create openspec/specs/client-connection/spec.md from the mapped connect/monitor/socket/close/loss/freeze sections. Verify every startup result, callback timing/cutoff, handle ownership and fresh-connection rule has a sourced requirement/scenario; update coverage and run strict validation for this spec.
- [x] 2.3 Create openspec/specs/client-requests/spec.md for the four request methods, free/notification APIs, single slot, pending map, deadlines, ownership and races. Verify all local error outputs, async acceptance/callback exceptions, text behavior and allocation failures against the source; update coverage and run strict validation for this spec.
- [x] 2.4 Cross-review the three specs against every applicable ClientResult row and Client API flow. Verify request/connection callback rules are distinct, no successful text service or async response timeout was introduced, and common wire rules are linked rather than copied.

## 3. Extract connection identity and Gateway routing

- [x] 3.1 Create openspec/specs/connection-identity/spec.md from mapped ZAP, interim identity, FD/lifetime binding, patch capability checks and deployment contracts. Verify exact endpoint, metadata/event formats, option 10001 and startup self-test failure rules; link the unchanged patch and source diagrams, update coverage and strictly validate the spec.
- [x] 3.2 Create openspec/specs/gateway-routing/spec.md for socket/thread ownership, private workers, one current request, admission-to-Dispatch timeout, resource policy and response handoff. Verify no request queue/retry or new quota is introduced, the ten-second deadline cannot time out executing work, and accepted work cannot execute after expiry; update coverage and strictly validate the spec.
- [x] 3.3 Cross-review routing and identity requirements for no-first-message disconnect, multiple connections from one PID, route reuse, guarded multipart sends and a slow receiver. Verify each existing outcome maps to a source clause/scenario, including dropped responses and continued control/completion progress.

## 4. Extract Crypto and shared-memory contracts

- [x] 4.1 Create openspec/specs/crypto-integration/spec.md for configuration, all 17 operations, scoped interface references, notification attempts and invocation/service outcomes. Verify missing backend still permits Gateway admission/local buffers, optional/enum conversions remain explicit, and backend-owned behavior is distinguished from Gateway responsibilities; update coverage and strictly validate the spec.
- [x] 4.2 Create openspec/specs/shared-memory/spec.md for UID quota, connection ownership, creation/preallocation, regions and access contracts. Verify exact 1,572,864-byte/four-per-host-UID limits, no global cap, every allocation/rollback step and every operation-specific presence/overlap rule against the source; update coverage and strictly validate the spec.
- [x] 4.3 Complete shared-memory release, lease completion and cleanup requirements. Verify accepted deferred release, conflicting use, repeated release, physical quota return, disconnect gates, ordinary hresult versus uncertain remote access and all buffer CryptoError mappings with sourced scenarios; update coverage and repeat strict validation after these additions.
- [x] 4.4 Cross-review allocation/use/release/disconnect flows against the backend open flags, fstat/bounds/no-retained-state requirements and BUFFER_OVERFLOW exception. Verify no guaranteed remote completion, protected mapping, FD-path substitution or automatic replay was invented, and link SHM-COMPLETION as unresolved.

## 5. Extract lifecycle and diagnostics

- [x] 5.1 Create openspec/specs/gateway-lifecycle/spec.md for configure/rollback, wrapper ownership, destructor-driven drain, framework deactivation, teardown and fresh startup. Verify retained shell/routes/work/resources, DISCONNECTED ordering and unresolved-cleanup behavior against each source state/sequence; update coverage and strictly validate the spec.
- [x] 5.2 Create openspec/specs/diagnostics/spec.md from Overview 4.5 and related failure paths. Verify severity/context, worker wait/running/reserved values, five-second category/severity suppression, cleanup warnings and confidentiality rules; update coverage and strictly validate the spec.
- [x] 5.3 Cross-review client/Gateway wakeups, stop races and failure diagnostics. Verify no periodic idle polling or cleanup retry was introduced, failed logging cannot change results/ownership, and lifecycle requirements reference shared-memory completion rules instead of duplicating them.

## 6. Extract SDK contracts and preserve guide content

- [x] 6.1 Create openspec/specs/sdk-installation/spec.md from mapped public-artifact, consumer, dependency/version and installation boundaries. Verify public generated bindings/protos, one descriptor owner, project-derived versions, plugin paths, optional tools and test defaults against source text; update coverage and strictly validate the spec.
- [x] 6.2 Review every BuildAndValidation.md and DevelopmentClient.md section. Verify build/use commands, container examples, fake-backend behavior, startup proposal and recorded test evidence remain in doc/, with canonical requirement links only for actual SDK/production contracts; record the disposition of every source row.

## 7. Complete traceability and remove duplicate normative copies

- [x] 7.1 Resolve every source row to exact target requirement anchors or a reviewed retained/open/protected disposition. Verify all ten capabilities have source provenance and relevant original AC links; record contradictions with exact text and status rather than selecting new behavior.
- [x] 7.2 Replace migrated normative prose/tables in EyilSecureGateway.md, EyilClient.md and EyilGateway.md with specific requirement links. Verify the complete contract exists before each removal; preserve original headings, all Mermaid blocks, rationale and usable examples.
- [x] 7.3 Preserve the original AC sections verbatim and keep sections 7.1–7.2 as the open-decision register. Verify all six identifiers and their status remain visible through spec links, integration checks remain distinct, and settled scope/PID-reuse notes have not become new gaps.
- [x] 7.4 Publish the resolved map in doc/SpecificationMap.md and update documentation navigation. Verify every migrated clause is covered, no row remains pending, both source and target anchors resolve, and normal documentation links do not depend on this active change directory.
- [x] 7.5 Update openspec/config.yaml and doc/OpenSpec.md to identify main specs as canonical contracts and doc/ as guidance/original ACs/open decisions. Verify obsolete empty-baseline and duplicate-maintenance instructions are removed while workflow/install instructions and generated skills remain intact.

## 8. Verify the integrated documentation migration

- [x] 8.1 Compare the protected AC sections, all 35 diagrams and the complete patch with source-baseline.json, locating content after line movement. Verify exact preservation and record results alongside the completed coverage map.
- [x] 8.2 Review links, residual duplicated normative rules and every extraction checkpoint in design.md across all capabilities. Verify forward/reverse traceability, all error tables and all unresolved/proposed labels; include any discrepancies in the migration result.
- [x] 8.3 Run openspec validate --all --strict --no-interactive, bash scripts/check-format.sh and git diff --check. Verify these checks pass, list the ten main specs, and report any unavailable check without claiming runtime or device validation.
- [x] 8.4 Review the final worktree diff against the pre-apply snapshot. Verify only planned documentation/OpenSpec files changed, README.md is empty, no code/API/build/test change or commit occurred, and the migration summary identifies preserved material and completed coverage.

## Workflow follow-up

- Archive only when explicitly requested after review. This baseline has no delta specs to synchronize; keep main specs intact.
- When archiving, adjust historical relative source links for the archive directory depth and validate them. Stable doc/SpecificationMap.md links must remain independent of the change location.
