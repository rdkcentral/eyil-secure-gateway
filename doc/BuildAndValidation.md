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

Public interfaces live in `apis/`; production implementation lives in `src/`.
The optional diagnostic client and CryptoTestBackend live in `tools/`.

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
  EyilClientConfig.cmake.in   Installed client CMake package template
  EyilProtocolConfig.cmake.in Installed shared C++ protocol package template
  eyilclient.pc.in            Installed client pkg-config template
  Coverage.cmake             Coverage configuration
  FindZeroMQ.cmake            ZeroMQ discovery
CMakeLists.txt                Top-level build
Tests/                       Test suites and fault-injection mocks
tools/                       Optional diagnostic CLI and CryptoTestBackend
CMakePresets.json             Native development and L1/L2 presets
scripts/                     Build and validation utilities
doc/                         Architecture and supporting material
```

The interface tree contains only public headers and schemas. Build rules stay
outside it; generated Protobuf sources/headers are written to the build directory.
The client keeps the public include path `eyilclient/EyilClient.h`. The build
targets `EyilClient::EyilClient` and `EyilProtocol::EyilProtocol` are available
both inside this build and through the installed `EyilClient` and `EyilProtocol`
CMake packages. The C client also provides `eyilclient.pc`. A C-only consumer of
EyilClient does not need Protobuf headers; service consumers use the separate
public C++ protocol package from the same SDK.

<details>
<summary>Build, installation, coverage and GitHub workflow details</summary>

#### Build, install and validation

Required tools: CMake 3.21+, Ninja, a C/C++ compiler, Protobuf 3.15+ development
files and a host-runnable protoc, ZeroMQ development files, and Python 3.9+.
Building the plugin additionally requires Thunder Core/COM/Plugins, ThunderTools and the
matching IConfiguration and ICrypto headers in the SDK. Obtain ICrypto from
entservices-apis branch topic/crypto as specified in section 6.7. Out-of-process
use requires compatible proxy/stubs for every crossed interface, including
ICrypto when Crypto is remote. Gateway currently supports in-process mode Off:
its final IConfiguration release invokes the implementation destructor, which
drains the runtime before releasing IShell. Future Local mode requires a verified
remote destruction/unload contract for IConfiguration. No additional lifecycle
interface is introduced. Do not mix headers and bindings from incompatible releases.

For Gateway runtime builds and transport L2 tests, use **libzmq 4.3.4 with the
included [connection-lifetime patch](zmq_connection_lifetime.patch)**,
ENABLE_DRAFTS=ON and matching consumer headers
with ZMQ_BUILD_DRAFT_API. Section 6.14 defines the required metadata, monitor and
guarded-send contract. Finding an arbitrary ZeroMQ 4.3+ package is insufficient
to establish Gateway compatibility. Client/protocol-only builds can use a
supported stock ZeroMQ 4.3+ package; those builds do not validate the Gateway
extension. Product builds must link and load the same compatible dependency.

Gateway startup must probe downstream option 10001 and pass the bounded,
isolated IPC self-test in
[section 6.14](EyilGateway.md#startup-capability-check-before-admission)
before production bind/admission. Stock draft-enabled libzmq can provide monitor
version 2 without the connection-lifetime patch. Test both rejection of an
incompatible loaded library and the actual target rootfs under its container
permissions/namespaces; a native development-container result is not device
validation. The startup test supplements the full patch regression suite and uses
a private directory under IShell::VolatilePath(). With the loaded dependency pinned
and the behavioral checks retained in L2/CI, a later change can remove the startup
exchange while keeping the inexpensive option-10001 and monitor-v2 capability checks.

With PLUGIN_EYILGATEWAY enabled, ICrypto headers are a required build input:
configuration must report a missing SDK/API dependency instead of silently
building an adapter without Crypto support. The platform image supplies the
Crypto plugin and required bindings. Client/protocol-only builds do not require
ICrypto or the Crypto plugin.

`BUILD_TESTING`, `RDK_SERVICES_L1_TEST` and `RDK_SERVICE_L2_TEST` default to OFF.
The `dev` preset enables all three and the diagnostic tools; CI selects it
explicitly. A normal product build therefore compiles no test executables.
For a custom build directory, use `ctest --test-dir <dir> -L '^L1$'` or `'^L2$'`
with `--output-on-failure --no-tests=error`. Add `--output-junit <file>` when a
JUnit artifact is needed. Presets remain compatible with CMake 3.21.

For a native SDK build, install the host development prerequisites, then run:

```sh
bash build_dependencies.sh
bash build.sh --preset dev
source scripts/environment.sh
ctest --preset l1
ctest --preset l2
bash scripts/check-install.sh build rootfs/all
```

build_dependencies.sh fetches pinned Thunder R4.4, ThunderTools and
ThunderInterfaces sources into the ignored _deps directory. It also supplies
the selected topic/crypto interface headers and builds libzmq 4.3.4 with the
connection-lifetime patch. Native tests use in-process Crypto; remote Crypto
still requires matching COM-RPC proxy/stubs from the product SDK. The bootstrap does not install system packages. To use an
existing SDK, set EYIL_THUNDER_PREFIX; optional
EYIL_DEPS_DIR selects local dependency storage. EYIL_CONFIGURATION_INCLUDE_DIR
can select the SDK include directory containing `interfaces/IConfiguration.h`
when it is not found automatically.

Gateway's generated configuration always uses in-process mode `Off`; there is
no implementation-mode CMake option. The wrapper rejects a manually supplied
remote mode. The implementation destructor drains the runtime on final Release;
remote destruction and library lifetime need separate validation. See [development and container validation](DevelopmentClient.md) for
the CLI, dev-rootfs bundle, startup-service selection and expected no-Crypto result.

Select the Crypto callsign through
`-DPLUGIN_EYILGATEWAY_CRYPTO_CALLSIGN=org.rdk.Crypto` or the product-specific
value. Its default is empty, allowing transport and local buffer operations
without a backend. Section 6.7 defines how BitBake forwards that setting to the
`crypto` entry in configuration.backends. It is independent of Gateway's own
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

Installation contains the shared libraries, public client/protocol headers,
CMake/pkg-config metadata, Protobuf schemas, enabled development executables
and generated Thunder plugin configuration. Gateway and CryptoTestBackend
plugins install under `lib/${STORAGE_DIRECTORY}/plugins`, matching Thunder R4's
systempath even when ordinary libraries use `CMAKE_INSTALL_LIBDIR=lib64`.
The configuration uses org.rdk.EyilGateway, with autostart false by default.
Consumers include `eyilclient/EyilClient.h` and link `EyilClient::EyilClient`,
or use `pkg-config --cflags --libs eyilclient`.

The public `libeyilprotocol.so` owns the generated `rpc.proto` and `crypto.proto`
bindings. Its SDK includes `rpc.pb.h`, `crypto.pb.h` and `ProtocolVersion.h` under
`${includedir}/eyilprotocol`, plus `EyilProtocol::EyilProtocol` from
`find_package(EyilProtocol CONFIG REQUIRED)`. C++ consumers, including Firebolt,
link this shared target and must not compile another copy of these `.pb.cc`
files into the same process. Use the same SDK's Protobuf version; the exported
package requires the version used to build the generated bindings. Library
SONAMEs remain unchanged: client ABI 0 and protocol ABI 2 are independent of
project version 1.0.0 and envelope schema 1.

Schemas install under `${datadir}/eyil/proto` for SDK tooling and consumers in
other languages. Installing them is not an instruction to regenerate duplicate
C++ bindings alongside `libeyilprotocol.so`. Yocto puts schemas, headers and
CMake metadata in the development package/SDK, while shared libraries belong
to runtime packages. Documentation, patches and test executables are not
installed. License collection for images belongs to the packaging system.

`check-install.sh` builds a consumer using only installed CMake packages and
headers, with no includes from this repository's source or build tree. It also
links a separate shared consumer library with hidden symbols and verifies that
both consumers resolve the same RPC/Crypto descriptors and exchange a payload.

An application with the installation prefix in its CMake search path needs only:

```cmake
find_package(EyilClient CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE EyilClient::EyilClient)
```

The imported target supplies the client include directory and library location.

For a C++ service consumer such as Firebolt Crypto, also use the shared protocol
bindings:

```cmake
find_package(EyilProtocol CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE EyilProtocol::EyilProtocol)
```

This target supplies the include path for `rpc.pb.h`, `crypto.pb.h` and
`ProtocolVersion.h`, along with its Protobuf dependency.

Yocto places the headers and CMake/pkg-config files in `eyil-secure-gateway-dev`;
they are available to consumer builds through the recipe sysroot/SDK and are not
required in the runtime image.

L1 scenarios are registered individually in CTest. The suites cover:

| Suite | Regression coverage |
| --- | --- |
| Shared buffers | Exact size boundaries, host-UID quotas across connections, region overflow/overlap, busy and deferred release, concurrent release/cleanup, allocation rollback, ENOSPC and open/create/truncate/chown/chmod/stat/unlink failures |
| Crypto adapter | All 17 operations, exact backend arguments and trusted context, enum/optional/oneof validation, every defined CryptoError, null lookup, failed/throwing invocation, output allocation failure after interface release, output-region validation and BUFFER_OVERFLOW propagation |
| Client | Public argument/output contracts, raw strings, request correlation, malformed/duplicate/unknown replies, EAGAIN/EINTR, send acceptance versus deadline, input/owner/response allocation failures, authentication/setup errors, callback restrictions/quiescence and cleanup with pending calls |
| Implementation lifecycle | Rejected configuration, partial-start rollback, repeated configuration rejection and final IConfiguration Release invoking destruction with idle resources or an active ICrypto call; IShell remains retained until completion |
| Common runtime | Descriptor ownership, wakeups, transport settings, token format, credential numeric bounds, monitor framing and bounded diagnostic suppression |
| L2 | Real DEALER/ROUTER IPC with the patched libzmq, concurrent callers/out-of-order results, real timeout budgets, missing Crypto, multiple connections, duplicate request IDs, disconnect before the first request, route reuse, stale reply prevention and single-attempt reply delivery under EAGAIN/EHOSTUNREACH/partial-send failures; response allocation/serialization failures during admission, admission-error handling, worker execution and worker timeout, including INTERNAL fallback, dropped-reply cleanup and continued service |
| Thunder L2 | Actual wrapper/implementation activation and destruction with the test-only ICrypto mock, with the backend absent, and with the development CryptoTestBackend plugin plus CLI shared-memory checks when development tools are enabled |
| Wrapper lifecycle | Failed Initialize retains state for framework-driven Deinitialize; missing implementation, invalid configuration, repeated cleanup, fresh activation and configured volatile-directory failure/recovery |

Fault injection uses test-only linker wrappers and allocation overrides. No test
hooks or additional Thunder interfaces are installed. Client fault tests compile
the production client source directly so the wrappers can intercept its external
calls. Other L2 tests exercise the shared library that is installed. Simulated
clock advancement isolates request races; L2 separately checks the real 10-second
worker wait and 15-second synchronous deadline, plus later async responses.
The test ICrypto performs deterministic fake operations, not cryptography.
Container mounts, device credentials and remote Crypto access still require the
separate device validation described in the architecture.

L1 and L2 use CTest directly; the development presets and check-l1/check-l2
targets select their labels. CI and the check targets also write JUnit reports.
Install-consumer checks cover compilation, linking and the Protobuf status
contract. Runtime tests validate the client and Gateway flows described above.

```sh
EYIL_BUILD_DIR=build-coverage bash build.sh --preset dev -DENABLE_COVERAGE=ON
source scripts/environment.sh
python3 scripts/coverage.py --build-dir build-coverage
```

Coverage instruments handwritten plugin/client code and shared runtime helpers, excluding generated
Protobuf and dependency sources. Registered L1/L2 tests exercise the implementation;
the runner produces XML, HTML and text reports. An empty selected suite produces
an explicit coverage-unavailable summary instead of an artificial percentage.
Use `--label L1` or `--label L2` to run coverage for one suite; the default runs
both suites.

#### Optional memory and undefined-behavior checks

Use a separate native build for sanitizers. The SDK's Thunder executable is not
sanitized, so this build omits the development executable and the real-host
Thunder test; run that test in the ordinary native build above.

```sh
EYIL_BUILD_DIR=build-sanitize bash build.sh --preset dev -DENABLE_COVERAGE=OFF \
  -DBUILD_EYIL_DEV_TOOLS=OFF \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=$PWD/Tests/support/ThunderR44.ubsan.supp" \
  ctest --test-dir build-sanitize --output-on-failure
```

The pinned Thunder R4.4 `ProxyObject::operator delete` calls `__Destructed()`
after destruction, which triggers a UBSan vptr diagnostic. The supplied
[suppression](../Tests/support/ThunderR44.ubsan.supp) applies only to vptr checks
in that dependency header; it does not suppress Eyil diagnostics or leak checks.
Reassess and remove it when updating the SDK. Test allocation overrides cover
both throwing and nothrow forms to preserve allocator pairing under ASan.

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

