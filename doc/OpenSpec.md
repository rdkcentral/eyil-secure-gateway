<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# OpenSpec development workflow

This repository uses OpenSpec 2.0.0 with GitHub Copilot. It organizes
planned changes, requirements, implementation tasks and completed-change history.
It does not replace the compiler, L1/L2 tests, or code review. No OpenSpec files
or tools are installed into the SDK or device rootfs.

## Project files and the existing specification

| Location | Purpose |
| --- | --- |
| `openspec/config.yaml` | Project context, documentation references and workflow rules |
| `openspec/specs/` | Canonical agreed contracts, organized into ten capability specifications |
| `openspec/changes/<change-name>/` | A change's proposal, design, spec deltas and tasks |
| `openspec/changes/archive/` | Completed-change history |
| `.agents/skills/openspec-*/` | Generated workflow instructions for GitHub Copilot |
| `.agents/skills/.openspec-target` | Tool registration used by `openspec update` |
| `openspec/LICENSE.OpenSpec` | License for the generated upstream instructions |

The existing specification has been migrated to [openspec/specs/](../openspec/specs/).
The [source map](SpecificationMap.md) covers every original section and links to
exact requirements. Each spec contains concise SHALL requirements and scenarios,
followed by the complete normative contract clauses and tables. Read both parts;
a scenario does not replace an exception in its detailed contract.

[EyilSecureGateway.md](EyilSecureGateway.md) preserves the original ACs verbatim
and keeps sections 7.1–7.2 as the unresolved-decision register.
[EyilClient.md](EyilClient.md) and [EyilGateway.md](EyilGateway.md) retain diagrams,
rationale and verification guidance with links to the canonical contracts.
Build/use procedures stay in [BuildAndValidation.md](BuildAndValidation.md) and
[DevelopmentClient.md](DevelopmentClient.md). Proposed/future items retain their
status; the migration does not establish new behavior or prove runtime compliance.

Propose changes against the relevant capability specs. Update their contracts
through the change workflow and keep affected diagrams, links and the source map
consistent. Do not restore duplicate normative prose in the guides. The migration
change establishes the baseline directly and has no spec deltas to synchronize;
archiving it is a separate, explicitly requested step.

## Host setup

The standalone CLI does not need Node.js. Install it once on the development
host; CMake, BitBake and runtime installations do not depend on it. To use the
same version as the generated files in this repository:

```sh
curl -fsSL https://openspec.dev/cli/install.sh | OPENSPEC_VERSION=v2.0.0 sh
export PATH="$HOME/.local/bin:$PATH"
openspec --version
```

The official installer verifies the download against the release checksums.
For other installation methods, see the [OpenSpec installation guide](https://openspec.dev/docs/installation).
The npm variant currently requires Node.js 22.12.0 or newer.

The workflow files are versioned in this repository. After cloning, open this
repository as the workspace root in your Copilot IDE and reload the IDE/session
if necessary. There is no separate OpenSpec server or API key to configure;
the selected coding assistant keeps its own account requirements.

To regenerate the integrations deliberately, run from the repository root:

```sh
openspec init --tools github-copilot --profile core --language English --no-copilot-cloud
```

This selects the workflows without adding a GitHub Copilot cloud-agent
workflow. Local and IDE Copilot use the `.agents/skills/` files.
See [supported tools](https://openspec.dev/docs/supported-tools).

## Using the workflows

Invoke these in the assistant chat, not in the shell:

| Action | GitHub Copilot |
| --- | --- |
| Explore a problem | `/openspec-explore` |
| Prepare a proposal, design, specs and tasks | `/openspec-propose` |
| Implement a selected change | `/openspec-apply-change` |
| Revise its plan | `/openspec-update-change` |
| Synchronize capability specs | `/openspec-sync-specs` |
| Archive completed work | `/openspec-archive-change` |

For example, in GitHub Copilot:

```text
/openspec-propose Add a CryptoTestClient case for disconnect during an async request.
```

Review the resulting plan, then request implementation using
`/openspec-apply-change <change-name>`. The propose workflow produces planning
artifacts only; it does not implement the change. These commands do not authorize
Git commits or pushes. The generated core profile does not include a separate
verify skill; use the normal validation commands and relevant tests below.

Useful terminal commands, from the repository root:

```sh
openspec list
openspec list --specs
openspec status --change <change-name>
openspec validate <change-name> --strict --no-interactive
openspec validate --all --strict --no-interactive
bash scripts/check-format.sh
```

The spec listing contains the ten baseline capabilities. OpenSpec validation
checks artifact structure, not implementation correctness. Run the applicable build,
L1/L2 and installation checks from [BuildAndValidation.md](BuildAndValidation.md)
before marking implementation tasks complete. Record unavailable device checks
separately.

Authored Markdown/YAML artifacts, including generated change metadata such as
`.openspec.yaml`, need the repository copyright and Apache-2.0 notice before
repository validation. Preserve upstream OpenSpec skills byte-for-byte under
their MIT license; put project-specific rules in `config.yaml`.

## Updates

Use `openspec update --dry-run` to inspect changes before refreshing generated
instructions. Keep the CLI and generated skills at a compatible version, update
the version recorded here and in NOTICE, and preserve the upstream license.
Do not edit generated skills to change project policy; regeneration would
overwrite those edits.

Telemetry can be disabled for a shell with `export OPENSPEC_TELEMETRY=0`, or for
the user with `openspec config set telemetry.enabled false`. This setup used the
environment setting and did not change shell startup files or global preferences.
