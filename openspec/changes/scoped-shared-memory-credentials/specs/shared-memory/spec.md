<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Shared-memory credential scope

## MODIFIED Requirements

### Requirement: SM-04 Exclusive allocation and backing-space reservation

Gateway SHALL prepare cleanup ownership, reserve quota and fully preallocate each object before publishing it to the application.

The detailed [contract](../../../../specs/shared-memory/spec.md#contract-sm-04-exclusive-allocation-and-backing-space-reservation) is normative for this requirement.

#### Scenario: Exclusive allocation and backing-space reservation outcome

- **WHEN** fallocate fails with ENOSPC, ENOMEM or unsupported preallocation after file creation
- **THEN** Gateway logs the stage and returns RPC_STATUS_OK with OPERATION_FAILED without publishing an ID; it rolls back or retains failed-cleanup state and quota, never falls back to ftruncate-only allocation.

## ADDED Requirements

### Requirement: SM-10 Scoped private-worker filesystem identity

Gateway SHALL use the trusted mapped host UID/GID for buffer creation and unlinking, with the complete credential scope defined below.

The detailed [contract](../../../../specs/shared-memory/spec.md#contract-sm-10-scoped-private-worker-filesystem-identity) is normative for this requirement.

#### Scenario: Container-owned tmpfs

- **WHEN** a container's tmpfs rejects creation by unmapped host root
- **THEN** a private Gateway worker creates/preallocates the object using the authenticated peer's mapped filesystem identity and restores its original credentials before publication or ICrypto.

#### Scenario: Failure and deferred cleanup

- **WHEN** preparation fails or a pending release becomes eligible for cleanup on another private worker
- **THEN** each filesystem scope restores that worker's saved identity and capabilities; retained allocation UID/GID and directory references support cleanup without reopening procfs.
