<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Design

## Context

See [proposal.md — Why](proposal.md#why) for the motivation. The current
worktree has no main capability specs. Seven files in doc/ contain the existing
contracts, architectural explanations, diagrams, operational instructions and
dependency patch. The source inventory covers 8,165 lines in 167 contiguous
ranges, including all Markdown headings and all 14 patch diff blocks.

[coverage-map.md](coverage-map.md) records the planned disposition of every
range. [source-baseline.json](source-baseline.json) identifies the actual
worktree snapshot, including pre-existing uncommitted OpenSpec setup. It records
file hashes, the three original AC sections and all 35 Mermaid blocks. This is
planning evidence, not proof that requirements have already been extracted.

The migration has several authorities to preserve: unchanged original ACs,
subsequently agreed implementation contracts, unresolved integration choices,
and examples of a development backend. Their status cannot be inferred from
the presence of SHALL/MUST alone. No code change is needed to perform this work.

## Goals / Non-Goals

**Goals:**

- Give each agreed contract one canonical capability owner and preserve its
  conditions, error cases, concurrency ordering and explicit limitations.
- Make every source section traceable in both directions, including sections
  retained entirely as guidance and evidence.
- Keep the documents useful for implementation: existing diagram anchors remain,
  and requirement links lead directly to the corresponding detailed contract.
- Preserve unresolved decisions without converting proposals into guarantees or
  making already settled scope choices appear open again.

**Non-Goals:**

- Reconcile architectural questions by choosing new runtime behavior, alter ACs,
  or treat documentation coverage as proof of AC implementation compliance.
- Update an external shared-memory proposal, ICrypto repository, product layer,
  installed header, production implementation, test, build or packaging script.
- Reformat diagrams, delete rationale to reduce line count, add runtime tests,
  or claim new platform validation. README.md remains empty.

## Decisions

### 1. Establish a main-spec baseline without runtime deltas

The change declares skip_specs: true. During apply, author main specs directly
under openspec/specs/. The source requirements already exist; this change only
establishes their OpenSpec representation. There are no proposed runtime deltas
to synchronize or re-apply during archive. Main specs use the normal Purpose /
Requirements structure, named SHALL/MUST requirements and observable scenarios.

Alternative considered: introduce ten ADDED delta capabilities as though they
were new behavior. That would obscure whether the migration changes the product
and would require a second synchronization step before source links become
usable. Direct baseline authoring keeps the documentation-only scope explicit.

### 2. Use ten capability owners

Every destination below is a spec.md inside the named openspec/specs/ directory.
The coverage map supplies the complete source ranges; this table assigns
canonical ownership to prevent duplication between specs.

| Capability | Canonical contract | Principal source sections |
| --- | --- | --- |
| rpc-protocol | Envelope fields, schema/version behavior, framing, request correlation, RpcStatus definitions, safely correlated infrastructure errors and the text rejection wire contract | Overview 4.1–4.4; Client 5.6, 5.8–5.9; Gateway 6.6 |
| client-connection | Client handle and owner-thread lifetime, connect readiness/results, monitor handling, connection callbacks, socket setup, local shutdown, terminal loss and prepared idle freeze | Client 5.1–5.2, 5.5, 5.13–5.15; Overview 2.4, 7.4 |
| client-requests | Four request APIs, single submission slot, pending records, deadlines/races, async acceptance and callbacks, response ownership, reserved notification APIs | Client 5.2–5.4, 5.6–5.12, 5.14; Overview 4.2 |
| connection-identity | Fixed endpoint, ZAP peer evidence, per-connection User-Id and interim identity, FD/lifetime binding, guarded routing, libzmq extension/self-test and trusted deployment assumptions | Overview 3.2–3.4, 4.4, 7.4; Gateway 6.5, 6.14 |
| gateway-routing | ROUTER ownership, admission, four private workers, one current request/no request queue, worker-start deadline, completion ownership and single-attempt egress | Gateway 6.1–6.3, 6.6; Overview 2.4, 4.2 |
| crypto-integration | Backend configuration, per-call query/invoke/Release, ApplicationContext and notifications, operation mapping, hresult versus CryptoError and backend access contract | Gateway 6.5, 6.7–6.9, 6.12, 6.15; Overview 3.4 |
| shared-memory | UID quota and connection ownership, creation/preallocation, region validation, execution-time leases, service errors, deferred destruction and retained cleanup | Gateway 6.10–6.14; Overview 4.2, 7.1 |
| gateway-lifecycle | Wrapper/implementation ownership, configure/rollback, destructor-driven stop, admission/drain ordering, framework deactivation, teardown and fresh startup after failure | Gateway 6.1, 6.3–6.4, 6.14–6.16 |
| diagnostics | Severity, fields, suppression, worker-capacity and cleanup diagnostics, confidentiality and best-effort failure logging | Overview 4.5; Gateway 6.2, 6.10–6.16 |
| sdk-installation | Public client/protocol artifacts and consumers, shared generated bindings, version variables, plugin/tool locations, development-only packaging and dependency boundaries | Overview 2.5, 4.1; BuildAndValidation; DevelopmentClient build/install sections |

Error mapping has explicit ownership: rpc-protocol owns envelope and RpcStatus
rules; client-connection and client-requests own their local ClientResult
outcomes; shared-memory owns buffer-service CryptoError mappings;
crypto-integration owns invocation and returned backend outcomes. Other specs
link to these rules instead of reproducing tables with subtly different cases.

Alternative considered: one spec per existing document or one monolithic spec.
That preserves the current duplication between protocol, client and Gateway
descriptions. Capability ownership makes future changes narrower without
discarding any existing detail.

### 3. Extract clauses, not just headings or summaries

For each SPLIT row, review every paragraph, list, table cell and example note.
Create named requirements for the agreed behavior, retaining concrete values,
API/error identifiers, preconditions, failure handling and ownership transitions.
Scenarios illustrate observable results; they do not replace detailed normative
clauses or invent guarantees absent from the source. One requirement can cover
multiple equivalent source passages if all qualifications are retained.

Each requirement records source sections and relevant original AC IDs. Each
source row records the resulting requirement anchors, content retained locally,
review outcome and any discrepancy. Read-only comparison with headers/protos
can identify a mismatch, but neither code nor the document is silently changed
to resolve it during migration. Record the exact conflict for a later decision.

Diagram-only obligations need particular care: keep the Mermaid block unchanged
and express an agreed obligation in its owning requirement when prose does not
already capture it. An illustrative design name is not made a required class.

The following checks are mandatory extraction checkpoints, not a replacement
for the complete section review:

| Area | Details that must survive extraction |
| --- | --- |
| Protocol | Exact envelope fields and schema 1; ID/service/schema/status validation; service-specific bytes remain opaque to EyilClient; active duplicate ID is logged/dropped without a second reply; no automatic replay or fabricated correlated reply |
| Local/API errors | Every ClientResult condition and output-clearing rule; local OOM versus Gateway errors; valid error responses still return CLIENT_OK; matched versus uncorrelatable receive failures |
| Client ownership | One DEALER/PAIR owner and wakeup; deadline/event-driven waits without periodic idle polling; one submission slot and no submission queue; independently retained waiters/pending map; install tracking before send; no lock held across waits/callbacks |
| Client deadlines | One 15,000 ms duration with original monotonic deadlines; sync total budget and async send-only budget; exact send/cancellation/response races; no late send after failed submission; no async response deadline |
| Async callbacks | CLIENT_OK only after libzmq acceptance, response-only callback at most once, valid error responses, accepted result cannot be revised, no callback guarantee after a dropped response, callback-thread restrictions and owned response freeing |
| Connect/close | HANDSHAKE_SUCCEEDED barrier, exact event/error mapping, occupied handle preservation, no initial or explicit-close callback, later LOST once, unused RESTORED/TERMINATED, cleanup of pending records/transport queues, no automatic reconnect and raw-handle caller coordination |
| String/notification APIs | Raw NUL-terminated strings without Base64/ProtoJSON; empty string allowed; service 0 rejection and exact status-text validation; no successful text service; register returns NOT_IMPLEMENTED and unregister has no effect |
| Gateway dispatch | Four private workers/default stacks; no waiting-request queue; one current request pauses only receives; ten seconds through actual Dispatch start; atomic timeout/start resolution; completions/control continue; event/deadline wakeups and separate ZAP/monitor stop wakeups; no idle polling or count/byte admission caps added |
| Egress/resources | One nonblocking multipart response attempt, including on EAGAIN/EINTR; safe multipart state and route guard; no retry or synthetic second error; default HWM and kernel buffers; unlimited message/response/global shared-memory policies remain distinct from representation limits |
| Identity | Kernel host UID/GID/PID, unique independent User-Id/generation, eyil-interim: appId, no AppManager dependency yet; FD/lifetime established before ZAP success, not on first RPC; option 10001, metadata and monitor layouts, no-first-message disconnect and stale-route guard |
| libzmq integration | Matching patched 4.3.4/draft APIs, capability probe plus isolated bounded startup self-test before production admission; full failure/cleanup checks; patch source remains unchanged |
| Backend | Empty/default or unavailable backend does not prevent transport/local-buffer use; backend-list validation, configurable callsign and dev fallback; query/invoke/Release per call including notifications; no cached interface/health observer/replay; all 17 operations and conversions/presence checks |
| Quota | Exactly 1,572,864 bytes per buffer and four allocations shared by all connections with the same trusted host UID; connection ownership remains separate; reservations, release-pending and failed cleanup stay charged; no global cap |
| Allocation/access | Existing app-namespace path, retained object/directory references, exclusive create, ftruncate plus full fallocate before publication, rollback ownership; exact region presence/range/overlap rules and app cooperation limits |
| Release/completion | TryUse at execution time, one active use per allocation, conflict rejection; first release accepted immediately with deferred destruction; repeated release error; once-only cleanup outside locks and quota returned only after physical cleanup |
| Crypto buffer boundary | Required open flags, fstat/size/range validation, no retained mapping/state after completed invocation, overflow region forwarding, invalid successful output rejection; ordinary R4 hresult/direct exception versus uncertain COM-RPC/proxy completion remains explicit |
| Plugin lifecycle | Configure/rollback and framework responsibilities; final IConfiguration Release invokes implementation-destructor drain; no new stop interface; shell/reference synchronization, preserved reply routes, DISCONNECTED ordering and no unsafe forced teardown |
| Logs | ERROR/WARNING/INFO criteria, actual worker wait/running/reserved fields, correlation fields, five-second category/severity suppression, bounded identifiers, no sensitive payloads and no periodic full-flow tracing |
| SDK | Shared public EyilProtocol bindings and installed generated headers/protos, one descriptor owner per C++ process, C/CMake/pkg-config consumers, project-derived library versions, plugin path, optional tool/default-test policy, no installed docs/private dependency bundle |

The preserved source tables remain the detailed checklist until every affected
row is reviewed. Do not shorten enumerated rules to a generic “validate input”
or “clean up resources” requirement.

### 4. Preserve ACs and decision status in their existing locations

The only verbatim original AC copy remains Overview sections 2.1–2.3, with all
19 table rows, WHEN text, identifiers, duplicated AC2 naming and original
wording unchanged. Capability specs link to those sections. The interpretation
in section 2.4 stays distinguishable from the AC: private-pool scheduling,
consolidated client monitoring and the string-service limitation are not
rewritten as modifications to the original acceptance criteria.

Overview sections 7.1–7.2 remain the canonical unresolved-decision register.
Each affected spec links to the relevant TODO identifier. Repeated TODO
explanations elsewhere become links, while agreed safeguards move into specs.
The potential guarantee for abnormal remote completion remains conditional.

| Existing item | Status to preserve | Affected capabilities |
| --- | --- | --- |
| SHM-COMPLETION | Backend/platform guarantee after uncertain remote failure remains to confirm; retain resources until access is known ended | shared-memory, crypto-integration, gateway-lifecycle |
| AUTH-APPMANAGER | Formal identity lookup and interim-prefix policy remain unresolved; current trusted-UID quota does not require lookup | connection-identity, crypto-integration |
| MULTI-CONNECTION-POLICY | Final allow/reject policy and authoritative ApplicationContext mapping remain unresolved; interim connections stay independent | connection-identity, crypto-integration |
| AUTHENTICATION-PROGRESS | Authentication-time CONNECTED bound/lifecycle capacity remain unresolved; do not add a timeout or reserved worker | connection-identity, crypto-integration, gateway-lifecycle |
| API-AUTHORIZATION | Entitlement contract remains unresolved; interim supported APIs are allowed | connection-identity, gateway-routing |
| FUTURE-TEXT-SERVICE | Future successful text execution remains undefined; current service-0 rejection is already selected | rpc-protocol, client-requests, gateway-routing |

Section 7.3 remains an integration/verification checklist. Section 7.4 remains
an index of settled scope assumptions with links to their canonical contracts.
Do not reopen removed gaps about endpoint ownership, idle freeze, automatic
recovery, client monitoring, app-side helpers or PID reuse. Retain their
limitations and rationale, including the PID-reuse edge-case note.

Proposal-labelled mechanisms remain labelled as such. Development-backend
operations, diagnostic CLI timeouts and recorded test results stay examples or
evidence, not requirements on deployed Crypto or additional Client deadlines.

Alternative considered: put every TODO in a normative SHALL requirement.
That would turn unresolved choices into implementation obligations. The linked
gap register preserves visibility without making those decisions.

### 5. Replace duplicates only after their destinations exist

Keep source filenames and existing heading text/anchors. Under each affected
heading, add specific links to canonical requirements and retain the diagrams,
reasoning and examples that explain them. Remove only prose/tables whose full
contract has a reviewed destination. A heading containing only links is valid.
Keep source/API examples usable and do not change the Mermaid blocks.

Build and use commands remain in BuildAndValidation.md and DevelopmentClient.md.
Extract only their normative SDK/distribution boundaries, keeping procedure and
evidence separate. The libzmq patch stays intact in doc/ and is referenced from
connection-identity; do not treat its embedded C++ as editable migration material.

Publish the completed map as doc/SpecificationMap.md so routine navigation does
not depend on an active change directory. Retain this planning snapshot as
history. Update openspec/config.yaml and doc/OpenSpec.md to name the main specs
as canonical, the original ACs as unchanged source criteria, and doc/ as linked
guidance/open decisions. Remove the setup advice to maintain duplicate baseline
contracts. This is a workflow-context update, not an installation change.

Alternative considered: leave the original normative text beside the new specs
until a later cleanup. That would create competing sources immediately. The
per-section extract/review/link sequence allows safe deduplication in this change.

### 6. Validate preservation independently of OpenSpec syntax

OpenSpec validation checks artifact structure, not semantic completeness. Apply
must also perform the following checks:

- Check source hashes before editing. If worktree source has changed since this
  snapshot, inventory that difference and review its effect before extraction;
  do not overwrite new user changes or silently regenerate the old snapshot.
- Reconcile all 167 source rows and any newly discovered source sections with
  exact requirement anchors or explicit retained disposition. Review in reverse
  as well: every normative target clause must have source provenance.
- Compare the original AC blocks, all 35 Mermaid blocks and the patch against
  stored hashes, locating them by headings/content after line numbers move.
- Review all error, timeout, quota, status and lifecycle tables against their
  destinations. Check TODO identifiers and status, not merely keyword counts.
- Check source/target links and heading anchors, missing code references,
  duplicate normative copies and dangling links introduced by section edits.
- Run strict OpenSpec validation and repository formatting/file checks. Confirm
  that no production, public API, schema, build or test file changed. Record
  unavailable checks explicitly; do not claim compilation or L1/L2 execution.

Archive is a later workflow. Main documentation must not link to this active
change path. When archiving, rebase the historical map's relative source links
for its extra directory depth and validate them; no spec synchronization is
needed for this documentation-only baseline.

## Risks / Trade-offs

### Apply layout clarification

The baseline uses concise `Requirement` and `Scenario` blocks linked to complete
`Detailed contracts` sections in the same spec. The detailed clauses and tables
are expressly normative parts of their requirements; only existing labels such
as Proposed or future remain non-final. This keeps OpenSpec's strict requirement
length validation satisfied without summarizing away conditions or error tables.
Source references identify the original range IDs in the resolved coverage map.
Navigation-only wording, such as “the table below”, becomes an explicit link
where the table or diagram moved to another document.

No runtime behavior or planned capability boundary changes through this layout.

- **A short spec drops an exception** → Preserve full clauses and source-table
  mappings; review every source range and trace each target clause back.
- **Repeated prose disagrees** → Record the exact discrepancy and preserve its
  decision status; do not use migration as authorization to choose behavior.
- **Scenario text overpromises** → Keep preconditions, uncertainty and external
  backend/platform responsibilities explicit; avoid turning examples into ACs.
- **Line numbers or archive locations change** → Keep source headings, publish
  a stable resolved map and validate links after editing and later archive.
- **Source changes during planning/apply** → Compare worktree hashes and reconcile
  intervening changes; preserve user edits and the original snapshot.
- **Detailed specs remain substantial** → Reduce duplication through links, not
  by removing contract details needed by implementation tools.

## Migration Plan

1. Confirm the source snapshot and identify all existing unrelated changes.
2. Create the ten main capability specs, first protocol/client and then
   identity/Gateway/backend/buffers/lifecycle/diagnostics/SDK. Retain source
   prose until its destination passes clause review.
3. Complete source-to-requirement and requirement-to-source coverage, including
   original AC traceability and unresolved-decision links.
4. Replace migrated source prose with links, preserve explanatory material,
   publish the resolved map and update OpenSpec guidance/context.
5. Run preservation, link, strict OpenSpec and repository checks. Present the
   documentation-only diff and any recorded discrepancies for review.

Rollback is local and documentation-only: remove newly authored baseline specs
and restore only this migration's documentation/context edits from a pre-apply
snapshot. Preserve earlier uncommitted setup and later user edits; do not use a
whole-worktree reset. No runtime deployment, commit, push or archive is part of
this proposed migration's apply step.
