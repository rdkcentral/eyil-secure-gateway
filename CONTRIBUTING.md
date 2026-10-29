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

# Contributing

Submit changes through a GitHub pull request. Contributors must sign the RDK
Contributor License Agreement before code can be accepted.

Keep the EyilGateway wrapper thin and place implementation work in separate
files. Keep EyilClient independently buildable. Define runtime behavior only
when its contract is agreed; unresolved lifecycle requirements stay TODO.

Follow .clang-format. Source, script, build and workflow files
use the Apache-2.0 licensing header. JSON must remain valid JSON. README.md is
intentionally empty; architecture and build instructions belong in
[doc/EyilSecureGateway.md](doc/EyilSecureGateway.md).

Run the native and standalone client builds, installation checks, formatting
checks and relevant L1/L2 suites. Empty suites must be reported as zero tests;
do not add dummy tests or claim coverage for unexecuted code.

Include a version: major, version: minor or version: patch line in the pull
request description. Update CHANGELOG.md and version metadata when releasing.
