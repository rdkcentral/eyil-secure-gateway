<!--
If not stated otherwise in this file or this component's LICENSE
file the following copyright and licenses apply:

Copyright 2026 RDK Management

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Changelog

#### [Unreleased]

- Implement synchronous/asynchronous EyilClient transport with one socket owner,
  a single submission slot, request correlation and deterministic disconnect.
- Implement Gateway ZAP/monitor handling, guarded connection routing, a private
  four-worker Thunder pool and per-call ICrypto dispatch for all Crypto operations.
- Add shared buffers with host-UID quotas, fallocate and deferred release.
- Add mandatory libzmq patch verification and reproducible native dependencies.
- Add L1/L2 tests, an ICrypto test plugin and real Thunder lifecycle validation.
- Expand named edge-case tests for allocation/syscall failures, malformed requests,
  callback/deadline/disconnect races, buffer leases and ICrypto argument/results.
- Contain Gateway response allocation/serialization failures: send a minimal
  INTERNAL response when possible, otherwise log and retire the reply without
  stopping the runtime or replaying the operation.
- Drain Gateway runtime from implementation destruction on final IConfiguration
  release, without an additional internal Thunder control interface.
- Stop the capability-probe monitor before closing its sockets to prevent
  teardown-time endpoint-frame leaks.
- Include common runtime helpers in coverage and clarify observable ZAP 300
  outcomes on the client.
- Configure backend callsigns by service in a list, with Crypto unconfigured by
  default; allow transport admission and local buffer operations without Crypto,
  returning backend errors through ordinary sync/async RPC responses.
- Separate configuration parsing from runtime startup exception handling and
  keep consistent spacing between source definitions and member groups.
- Keep one wrapper metadata registration and let Thunder perform Deinitialize
  after failed Initialize. Use explicit ICrypto Release calls and checked error
  results instead of throwing for validation, transport and syscall failures.
- Place the transport self-test under Thunder's volatile path, align its deadline
  with the specification and test wrapper cleanup/reactivation and probe cleanup.
- Add an optional development client with a container-mountable runtime bundle.
- Keep public C signatures unchanged and document the implemented error/callback
  behavior; initial runtime support is in-process only.

#### [1.0.0]

- Add the EyilGateway Thunder plugin and independent EyilClient library skeletons.
- Acquire a separate IConfiguration implementation through Thunder Root, with
  in-process and local-process configuration, remote notifications and cleanup.
- Share plugin version macros across metadata and both service registrations.
- Add the public EyilClient C API with explicit unimplemented-operation results.
- Keep the supplied nine-method client API without a version getter.
- Integrate generated Protobuf contracts and ZeroMQ build dependencies.
- Separate public interfaces in apis/ from implementation in src/ and move
  protocol build rules and package templates into cmake/.
- Introduce RpcStatus and RPC envelope schema version 1.
- Add empty L1/L2 suites, coverage support, installation checks and CI workflows.
- Run coverage within L1/L2 jobs and combine JSON, formatting and licensing checks
  in one validation workflow.
- Use GitHub-hosted Ubuntu 24.04 runners and the public rdkcentral FOSSID workflow;
  remove private Coverity workflow dependencies.
- Document the planned architecture and pending runtime decisions.
