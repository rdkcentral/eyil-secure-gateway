<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Proposal

## Why

OpenSpec is configured, but its capability inventory is empty while the existing
Eyil requirements are distributed across implementation guides. Establish a
traceable baseline so future changes can reference those contracts without
maintaining competing normative copies.

## What Changes

- Extract agreed requirements into ten capability specifications under
  `openspec/specs/`, using named requirements and observable scenarios.
- Account for every source section in a coverage map, including material that
  remains in `doc/`. Record exact requirement links when extraction is complete.
- Preserve original AC sections 2.1–2.3 verbatim. Preserve unresolved decisions,
  selected scope assumptions and the distinction between requirements, proposed
  mechanisms, implementation evidence and external integration work.
- Keep diagrams, architectural reasoning, build/use instructions and the libzmq
  patch in `doc/`. Replace migrated normative prose/tables with links only after
  their destinations exist and their complete contracts have been reviewed.
- Update OpenSpec context and documentation navigation to identify the canonical
  requirement location. Keep references valid after synchronization and archive.

No runtime behavior, C ABI, Protobuf schema, library version, configuration,
resource policy, production code, test code or build/packaging behavior changes.
An identified discrepancy is recorded, not silently fixed during migration.

## Capabilities

### New Capabilities

None. This change records existing contracts; it does not introduce system
behavior. The proposed capability destinations are listed in `design.md`.

### Modified Capabilities

None. `.openspec.yaml` sets `skip_specs: true` for this documentation-only change.
No artificial delta requirements are added to satisfy validation. During apply,
the existing contracts will be converted directly into main capability specs;
archive will preserve that baseline without re-applying runtime deltas.

## Impact

- All seven current `doc/` files are inventoried; original ACs, diagrams and the
  patch have explicit preservation checks.
- `openspec/specs/`, `openspec/config.yaml` and relevant documentation links change.
- Coverage and review records stay with this change; a stable source map will
  remain available from `doc/` after migration.
- Only documentation/OpenSpec validation is required. Existing test suites are
  referenced as evidence; this migration does not claim new runtime test results.
- External proposals, API repositories, meta-layers and deployment configuration
  remain outside the edit scope. No commits or pushes are authorized.
