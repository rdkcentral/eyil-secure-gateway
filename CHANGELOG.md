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

#### [1.0.0]

- Add the EyilGateway Thunder plugin and independent EyilClient library skeletons.
- Acquire a separate IConfiguration implementation through Thunder Root, with
  in-process and local-process configuration, remote notifications and cleanup.
- Share plugin version macros across metadata and both service registrations.
- Add the public EyilClient C API with explicit unimplemented-operation results.
- Keep the supplied nine-method client API without a version getter.
- Integrate generated Protobuf contracts and ZeroMQ build dependencies.
- Introduce RpcStatus and RPC envelope schema version 1.
- Add empty L1/L2 suites, coverage support, installation checks and CI workflows.
- Run coverage within L1/L2 jobs and combine JSON, formatting and licensing checks
  in one validation workflow.
- Use GitHub-hosted Ubuntu 24.04 runners and the public rdkcentral FOSSID workflow;
  remove private Coverity workflow dependencies.
- Document the planned architecture and pending runtime decisions.
