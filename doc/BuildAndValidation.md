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

# Eyil Secure Gateway build and validation guide

This is a required part of the [architecture and implementation specification](EyilSecureGateway.md).
Read its original ACs, common protocol/error/logging rules and gaps with this guide.
Section numbers are shared across the specification; client sections are in
[EyilClient.md](EyilClient.md), Gateway sections in [EyilGateway.md](EyilGateway.md),
and build/CI details in [BuildAndValidation.md](BuildAndValidation.md).

- [6.18 Build and repository validation reference](#618-build-and-repository-validation-reference)

### 6.18 Build and repository validation reference

These commands cover build and repository validation. Runtime acceptance also
requires the [behavior tests in section 6.17](EyilGateway.md#617-implementation-order-and-verification-targets).

#### Repository layout

Public interfaces live in `apis/`; implementation lives in `src/`.

```text
apis/
  eyilclient/EyilClient.h       Public C client API
  proto/rpc.proto             RPC envelope schema
  proto/crypto.proto          Crypto service schema
  proto/ProtocolVersion.h     Public protocol version constant
src/
  clientlib/                  Client implementation and CMakeLists.txt
  gateway/                    Plugin implementation, private headers,
                              configuration and CMakeLists.txt
cmake/
  EyilProtocol.cmake          Protocol generation, library and install rules
  EyilClientConfig.cmake.in   Installed client package template
  EyilProtocolConfig.cmake.in Installed protocol package template
  eyilclient.pc.in            pkg-config template
  Coverage.cmake             Coverage configuration
  FindZeroMQ.cmake            ZeroMQ discovery
CMakeLists.txt                Top-level build
Tests/                       Test suites
scripts/                     Build and validation utilities
doc/                         Architecture and supporting material
```

The interface tree contains only public headers and schemas. Build rules and
package templates stay outside it; generated Protobuf sources/headers and
configured package files are written to the build directory. The client keeps
the public include path `eyilclient/EyilClient.h`. Library names, CMake targets
`EyilClient::EyilClient` and `EyilProtocol::EyilProtocol`, and installed interface
locations are independent of the source layout.

<details>
<summary>Build, installation, coverage and GitHub workflow details</summary>

#### Build, install and validation

Required tools: CMake 3.21+, Ninja, a C/C++ compiler, Protobuf 3.15+ development
files and a host-runnable protoc, ZeroMQ development files, and Python 3.9+.
Building the plugin additionally requires Thunder Core/COM/Plugins, ThunderTools and the
matching IConfiguration and ICrypto headers in the SDK. Obtain ICrypto from
entservices-apis branch topic/crypto as specified in section 6.7. Out-of-process
use requires compatible proxy/stubs for every crossed interface, including
ICrypto when Crypto is remote, and IConfiguration plus the private lifecycle
control interface when Gateway implementation mode is Local. Do not mix headers
and bindings from incompatible releases.

For Gateway runtime builds and transport L2 tests, use **libzmq 4.3.4 with the
included [connection-lifetime patch](zmq_connection_lifetime.patch)**,
ENABLE_DRAFTS=ON and matching consumer headers
with ZMQ_BUILD_DRAFT_API. Section 6.14 defines the required metadata, monitor and
guarded-send contract. Finding an arbitrary ZeroMQ 4.3+ package is insufficient
to establish Gateway compatibility. Client/protocol-only builds can use a
supported stock ZeroMQ 4.3+ package; those builds do not validate the Gateway
extension. Product builds must link and load the same compatible dependency.

With PLUGIN_EYILGATEWAY enabled, ICrypto headers are a required build input:
configuration must report a missing SDK/API dependency instead of silently
building an adapter without Crypto support. The platform image supplies the
Crypto plugin and required bindings. Client/protocol-only builds do not require
ICrypto or the Crypto plugin.

For a native SDK build, install the host development prerequisites, then run:

```sh
bash build_dependencies.sh
bash build.sh
source scripts/environment.sh
python3 scripts/run-tests.py --build-dir build --label L1
python3 scripts/run-tests.py --build-dir build --label L2
bash scripts/check-install.sh build rootfs/all
```

build_dependencies.sh fetches pinned Thunder R4.4, ThunderTools and
ThunderInterfaces sources into the ignored _deps directory. It builds the
IConfiguration headers and proxy/stubs along with Thunder. Supply the selected
topic/crypto ICrypto headers and any required proxy/stubs through the SDK/API
dependency as well. The bootstrap does not install system packages. To use an
existing SDK, set EYIL_THUNDER_PREFIX; optional
EYIL_DEPS_DIR selects local dependency storage. EYIL_CONFIGURATION_INCLUDE_DIR
can select the SDK include directory containing `interfaces/IConfiguration.h`
when it is not found automatically.

To generate a configuration for a separate implementation process, build with
`bash build.sh -DPLUGIN_EYILGATEWAY_MODE=Local`. The default is `Off`. Install both
plugin libraries and make the matching proxy/stubs available to Thunder and
WPEProcess before using Local mode.

Select the Crypto callsign through
`-DPLUGIN_EYILGATEWAY_CRYPTO_CALLSIGN=org.rdk.Crypto` or the product-specific
value. Section 6.7 defines how the BitBake recipe forwards that setting and how
it reaches configuration.cryptocallsign. It is independent of Gateway's own
callsign and implementation mode.

The client and protocol can be built without Thunder:

```sh
cmake -S . -B build-client -G Ninja \
  -DPLUGIN_EYILGATEWAY=OFF -DBUILD_EYILCLIENT=ON \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build-client
bash scripts/check-install.sh build-client rootfs/client
```

Use a CMake toolchain file and target dependency prefixes when cross compiling.
Protobuf_PROTOC_EXECUTABLE must identify a host-runnable protoc. Cross-platform
rootfs validation is still pending.

Installed CMake consumers use EyilClient::EyilClient and
EyilProtocol::EyilProtocol. The C header is eyilclient/EyilClient.h; the client also
provides eyilclient.pc. Both plugin libraries install to the Thunder plugin
directory. The generated configuration uses org.rdk.EyilGateway, with autostart
false by default. No test executables are installed.

L1 and L2 runners invoke CTest and emit summaries and JUnit reports.
Install-consumer checks cover compilation, linking and the Protobuf status
contract. Runtime tests validate the client and Gateway flows described above.

```sh
EYIL_BUILD_DIR=build-coverage bash build.sh -DENABLE_COVERAGE=ON
source scripts/environment.sh
python3 scripts/coverage.py --build-dir build-coverage
```

Coverage instruments handwritten plugin/client code, excluding generated
Protobuf and dependency sources. Empty suites produce an explicit coverage
unavailable summary. Once real tests are registered with L1/L2 labels, the same
runner produces XML, HTML and text reports. No artificial percentage is reported.
Use `--label L1` or `--label L2` to run coverage for one suite; the default runs
both suites.

#### Repository checks and GitHub configuration

C/C++ uses .clang-format; Python uses Black; YAML uses yamllint. New source,
build, workflow and documentation files carry the Apache-2.0 licensing header.
JSON remains strict JSON and is covered by LICENSE/NOTICE. README.md is
intentionally empty.

Install the pinned development tools from scripts/requirements-dev.txt and run
scripts/check-format.sh plus scripts/validate-files.py. Build outputs and local
SDKs are ignored by Git.

GitHub workflows cover native build/install checks, standalone client builds,
L1/L2, coverage, formatting, JSON validation, manual CI, FOSSID,
changelog/version validation, and manual draft release creation. CLA and
CODEOWNERS are retained as repository governance.

L1 and L2 each collect coverage for their selected suite through the shared
`native-ci.yml` workflow. Manual CI uses the same native build job. Formatting
and licensing checks run together with JSON checks in `json_validator.yml`.
The scripts directory supplies the build and validation commands used locally
and in CI; GitHub Actions itself does not require that directory layout.

Gateway transport acceptance tests require the extended libzmq and selected
ICrypto SDK/bindings described above. Installing a distribution libzmq3-dev
package alone does not establish those runtime prerequisites. Record the
dependency revision/capabilities with test results and validate the installed
device binary separately under ZMQ-PLATFORM.

Jobs use GitHub-hosted `ubuntu-24.04` runners. This release supplies Protobuf
3.21, which meets the project's 3.15 minimum for proto3 optional fields.
The native SDK cache includes the runner release.
No organization-specific runner label is required. Native and standalone-client
builds use CMake 3.28.3, compatible with the pinned Thunder sources.

FOSSID uses the public
`rdkcentral/build_tools_workflows/.github/workflows/fossid_integration_stateless_diffscan.yml@1.0.0`
on pull requests from branches in this repository. It requires the four FOSSID
secrets referenced in the workflow and skips fork pull requests. CLA uses
`rdkcentral/cmf-actions` and requires CLA_ASSISTANT. Build, test, coverage and
repository-validation jobs require no organization credentials.

The scripts directory, formatting configuration and CHANGELOG.md must be
committed with the workflows so a fresh checkout contains their dependencies.

The release workflow is manual and creates a draft only from main after
checking the requested version against CMake and CHANGELOG.md. Repository
administrators must configure branch protection and runner access separately.
Version checks also compare the CMake version and src/gateway/Version.h so the
plugin's reported and registered versions agree.

</details>

