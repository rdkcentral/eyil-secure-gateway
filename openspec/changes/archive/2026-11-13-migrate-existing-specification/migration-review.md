<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Migration review

## Result and authority

The existing agreed baseline is organized into ten main capability specs with
68 requirements and 68 scenarios. Each requirement links to its complete
normative clauses/tables in the same file. This is a documentation migration;
it neither introduces behavior nor certifies implementation compliance.

The [resolved coverage map](coverage-map.md) and stable
[reader map](../../../../doc/SpecificationMap.md) cover all 167 original ranges
in seven source files. Every detailed contract names its source range IDs.
371 prose/table blocks were extracted and 48 repeated summaries were replaced
by links. Remaining component/flow tables and test matrices are guide summaries
or verification evidence, with the canonical contracts identified separately.

Original ACs remain authoritative as originally worded, alongside the documented
implementation interpretations and limitations. Sections 7.1–7.2 of the
architecture guide remain the canonical unresolved-decision register. All six
IDs retain their status: SHM-COMPLETION, AUTH-APPMANAGER,
MULTI-CONNECTION-POLICY, AUTHENTICATION-PROGRESS, API-AUTHORIZATION and
FUTURE-TEXT-SERVICE. No new timeout, authorization rule, thread, recovery policy
or shared-memory identity mechanism was selected.

## Source interpretations preserved

| Source text / location | Treatment |
| --- | --- |
| Original AC1.7: “SHALL process each request in a separate thread”; section 2.4: “Each accepted request has an independent dispatch job on the fixed private worker pool.” | Preserve the AC verbatim and the agreed private-pool/receive-pause contract separately. Migration is not AC sign-off. |
| Original AC2-Client.4 requires a monitor thread; section 2.4 says “This consolidated arrangement is agreed; no additional client monitor thread is required.” | Preserve both original wording and the consolidated I/O-owner contract. |
| Section 2.4: “This is an explicit delivery limitation against the original string-functionality AC” | Keep the limitation visible; migrate the existing service-0 rejection/status-text contract without inventing successful text execution. |
| Section 6.2: “This revised sizing decision supersedes the shared-memory proposal's section 6.3 limits and examples requiring larger allocations.” | Preserve that rationale and the selected 1,572,864-byte/four-per-host-UID quota. Do not restore the earlier proposal's sizing. |
| Section 6.10 labels the example backend path “Proposed host-visible path”; its subsequent explanation says “Keep the proposal's app-namespace backend path for this version.” | Preserve the illustrative label and the selected pathname/cooperation contract. Do not substitute the discussed Gateway-FD path or reopen PID reuse as a design gap. |
| Section 6.7: “The reviewed branch revision is 6926f6a72fbfff18b691d09b3b7376fbd541b9d3.” | Retain this source review reference and its compatible-build dependency requirement. No dependency recipe or revision is changed by migration. |
| Section 7.1: “Potential backend/integration requirement — to confirm” | Preserve its conditional status. Ordinary completed calls and uncertain remote access remain distinct; no unconditional completion guarantee is added. |
| DevelopmentClient.md: “These are test behaviors, not additional requirements on a real Crypto implementation.” | Keep deterministic operations, alignment, private copies, CLI deadlines and test evidence in the development guide. |

## Extraction and semantic checks

The review used every checkpoint in [design.md](design.md): wire fields/statuses,
all ClientResult rows, send/response/close races, callback ownership, request IDs,
single-slot/client and single-current/Gateway admission, exact worker deadlines,
single-attempt multipart replies, FD/lifetime binding, patch probes/self-test,
backend availability/reference ownership, all 17 operations, UID quotas,
preallocation/rollback, region/overlap rules, deferred release and abnormal
completion, destructor drain, diagnostics and public SDK installation.

Extracted blocks were checked against their original snapshot text, allowing
only Markdown-link rebasing, numbered-section links, relocated table/diagram
references and removal of a stray “Library” before “Both shared libraries” in
the SDK paragraph. Repeated summaries were reviewed against their linked full
contracts. All other retained source blocks were checked for preservation;
only navigation/workflow authority text was intentionally rewritten.

The response-freeing rules shown only in the original diagram remain explicit
in CR-10. Source examples, rationale, proposal labels and future executor/event
loop discussion remain in the guides. Build/test/run commands remain in doc/.

## Preservation and validation

Compared with [source-baseline.json](source-baseline.json):

- All three original AC sections, including all 19 rows, are byte-for-byte intact.
- All 35 Mermaid blocks and all 60 fenced examples/commands are unchanged.
- The complete libzmq patch, including all 14 diff blocks, is unchanged.
- Every original source heading remains present and in its original order.
- Every extracted block is present in its canonical detailed contract, subject
  only to the navigation edits described above.
- All 167 source ranges have reviewed destinations or retained dispositions.

Final checks:

| Check | Result |
| --- | --- |
| `openspec validate --all --strict --no-interactive` | Passed: ten main specs and the migration change, zero failures. `skip_specs` correctly permits this baseline-only change without deltas. |
| `bash scripts/check-format.sh` | Passed: file/licensing/JSON validation, Black, YAML, C/C++/proto formatting and shell syntax. |
| `git diff --check` | Passed after removing two extra end-of-file blank lines. |
| Source-preservation audit | Passed: AC hashes, Mermaid hashes, all fenced examples, complete patch, original headings, extracted clauses and retained source blocks. |
| Local Markdown links and fragments | Passed: 1,470 local links across guides, main specs and change artifacts; all targets/anchors resolve. |
| Apply-scope audit | Passed: only doc/ and openspec/ changed relative to the pre-apply snapshot; all 114 other pre-existing files and HEAD are unchanged. |

## Worktree boundary

The pre-apply snapshot included uncommitted OpenSpec setup in NOTICE,
scripts/check-format.sh, scripts/validate-files.py, doc/BuildAndValidation.md,
doc/OpenSpec.md, .agents/ and openspec/. Those changes were retained.
All 114 pre-existing files outside doc/ and openspec/ match their pre-apply
hashes. README.md remains empty. HEAD remains
9046714db6f52a655025f100516258a41957eb5a.

No production source, public API, Protobuf schema, build, packaging, test or
external repository file was changed by apply. No commit, push, runtime/device
test or archive was performed. Build and L1/L2 execution were unnecessary for
this documentation-only change; repository validation is separate from runtime
acceptance. Archive remains a separately requested workflow.

## Archive follow-up — 2026-11-13

Archived on explicit user request after confirming all 28 tasks complete and
all planning artifacts complete or intentionally skipped. No delta specs exist,
so no synchronization was needed. Historical relative links were rebased for
the archive directory; the main specs and all files outside this change were
left intact. The preceding worktree and validation results describe the apply
operation before archival.
