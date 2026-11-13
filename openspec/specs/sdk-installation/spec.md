<!-- Copyright 2026 RDK Management -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# sdk-installation specification

## Purpose

Define the public client/protocol SDK and installation/dependency boundaries; retain build and usage procedures in doc/.

This specification records existing agreed behavior. Original acceptance criteria
remain unchanged in [the architecture guide](../../../doc/EyilSecureGateway.md#2-acceptance-criteria-and-scope).
Traceability: AC2-Client.1, AC2-Firebolt.2. See the [source map](../../../doc/SpecificationMap.md)
for detailed provenance. Diagrams, examples and rationale remain in doc/.
Explicitly labelled proposals, recommendations and future integration questions
retain that status; internal design names do not mandate classes or public APIs.

## Requirements

### Requirement: SDK-01 Public interfaces and build targets

The SDK SHALL expose the public client and shared protocol targets while keeping implementation/build machinery outside the interface tree.

The detailed [contract](#contract-sdk-01-public-interfaces-and-build-targets) is normative for this requirement.

#### Scenario: Public interfaces and build targets outcome

- **WHEN** a C-only consumer imports EyilClient from an installed SDK
- **THEN** it can include eyilclient/EyilClient.h and link the C library without Protobuf headers; a C++ service consumer imports the separate shared protocol target.

### Requirement: SDK-02 Compatible build and runtime dependencies

Builds SHALL supply the dependencies below and distinguish stock-client builds from patched Gateway runtime builds.

The detailed [contract](#contract-sdk-02-compatible-build-and-runtime-dependencies) is normative for this requirement.

#### Scenario: Compatible build and runtime dependencies outcome

- **WHEN** Gateway is enabled but the ICrypto SDK headers are absent
- **THEN** configuration reports the missing dependency instead of compiling out the adapter; an absent runtime backend remains a supported configuration, and client-only builds require no Thunder or ICrypto.

### Requirement: SDK-03 Installed artifacts and shared binding ownership

Installation and consumers SHALL follow the artifact, descriptor-ownership and project-derived version contracts below.

The detailed [contract](#contract-sdk-03-installed-artifacts-and-shared-binding-ownership) is normative for this requirement.

#### Scenario: Installed artifacts and shared binding ownership outcome

- **WHEN** Firebolt and EyilClient use Crypto messages in the same C++ process
- **THEN** both use the SDK shared EyilProtocol bindings and compatible Protobuf; no duplicate generated .pb.cc definitions are registered, and consumers of earlier development SONAMEs are rebuilt rather than given compatibility symlinks.

### Requirement: SDK-04 Optional tests and development tools

Builds SHALL keep tests and diagnostic artifacts optional with the selected defaults and normal package ownership below.

The detailed [contract](#contract-sdk-04-optional-tests-and-development-tools) is normative for this requirement.

#### Scenario: Optional tests and development tools outcome

- **WHEN** BUILD_EYIL_DEV_TOOLS is disabled in a normal product build
- **THEN** neither CryptoTestClient nor CryptoTestBackend is built or installed, and no private dependency/loader bundle or launcher is introduced; enabled tools use normal runtime packages and container-provided dependencies.

### Requirement: SDK-05 In-process plugin and optional startup integration

Gateway packaging and startup SHALL retain in-process mode and the optional service integration below.

The detailed [contract](#contract-sdk-05-in-process-plugin-and-optional-startup-integration) is normative for this requirement.

#### Scenario: In-process plugin and optional startup integration outcome

- **WHEN** RuntimeManager or the deployed Crypto plugin is unavailable
- **THEN** EyilGateway remains independently startable; it needs no ai2managers marker, its empty production backend default is valid, and backend RPC failures do not prevent local buffer operations.

## Detailed contracts

These clauses and tables are normative parts of the linked requirements.
Explicit proposal/future labels retain their original non-final status.

### Contract: SDK-01 Public interfaces and build targets

Source: [BV-004](../../../doc/BuildAndValidation.md#repository-layout).

The interface tree contains only public headers and schemas. Build rules stay
outside it; generated Protobuf sources/headers are written to the build directory.
The client keeps the public include path `eyilclient/EyilClient.h`. The build
targets `EyilClient::EyilClient` and `EyilProtocol::EyilProtocol` are available
both inside this build and through the installed `EyilClient` and `EyilProtocol`
CMake packages. The C client also provides `eyilclient.pc`. A C-only consumer of
EyilClient does not need Protobuf headers; service consumers use the separate
public C++ protocol package from the same SDK.

### Contract: SDK-02 Compatible build and runtime dependencies

Source: [BV-005](../../../doc/BuildAndValidation.md#build-install-and-validation), [GW-015](../../../doc/EyilGateway.md#67-crypto-adapter-per-call-icrypto-references-and-per-connection-sessions).

Required tools: CMake 3.21+, Ninja, a C/C++ compiler, Protobuf 3.15+ development
files and a host-runnable protoc, ZeroMQ development files, and Python 3.9+.
Building the plugin additionally requires Thunder Core/COM/Plugins, ThunderTools and the
matching IConfiguration and ICrypto headers in the SDK. Obtain ICrypto from
entservices-apis branch topic/crypto as specified in [section 6.7](../../../doc/EyilGateway.md#67-crypto-adapter-per-call-icrypto-references-and-per-connection-sessions). Out-of-process
use requires compatible proxy/stubs for every crossed interface, including
ICrypto when Crypto is remote. Gateway currently supports in-process mode Off:
its final IConfiguration release invokes the implementation destructor, which
drains the runtime before releasing IShell. Future Local mode requires a verified
remote destruction/unload contract for IConfiguration. No additional lifecycle
interface is introduced. Do not mix headers and bindings from incompatible releases.

For Gateway runtime builds and transport L2 tests, use **libzmq 4.3.4 with the
included [connection-lifetime patch](../../../doc/zmq_connection_lifetime.patch)**,
ENABLE_DRAFTS=ON and matching consumer headers
with ZMQ_BUILD_DRAFT_API. [Section 6.14](../../../doc/EyilGateway.md#614-disconnect-detection-pid-observations-and-hibernation) defines the required metadata, monitor and
guarded-send contract. Finding an arbitrary ZeroMQ 4.3+ package is insufficient
to establish Gateway compatibility. Client/protocol-only builds can use a
supported stock ZeroMQ 4.3+ package; those builds do not validate the Gateway
extension. Product builds must link and load the same compatible dependency.

With PLUGIN_EYILGATEWAY enabled, ICrypto headers are a required build input:
configuration must report a missing SDK/API dependency instead of silently
building an adapter without Crypto support. The platform image supplies the
Crypto plugin and required bindings. Client/protocol-only builds do not require
ICrypto or the Crypto plugin.

Use the ICrypto API from
[entservices-apis branch topic/crypto](https://github.com/rdkcentral/entservices-apis/tree/topic/crypto),
specifically [apis/Crypto/ICrypto.h](https://github.com/rdkcentral/entservices-apis/blob/topic/crypto/apis/Crypto/ICrypto.h).
The reviewed branch revision is 6926f6a72fbfff18b691d09b3b7376fbd541b9d3.
The platform build supplies compatible headers and any required COM-RPC
proxy/stubs from that branch; pin a compatible revision in the dependency
recipe for reproducible builds. Crypto operations use the deployed backend
through its configured callsign. Gateway startup and transport admission also
work when no backend is configured or installed.

### Contract: SDK-03 Installed artifacts and shared binding ownership

Source: [BV-005](../../../doc/BuildAndValidation.md#build-install-and-validation), [OV-016](../../../doc/EyilSecureGateway.md#41-the-two-protobuf-files).

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
package requires the version used to build the generated bindings.
Both shared libraries and their CMake package versions use `PROJECT_VERSION`;
their SONAMEs use `PROJECT_VERSION_MAJOR`. With project version 1.0.0, this
produces `libeyilclient.so.1` and `libeyilprotocol.so.1`. Consumers of the earlier
development SONAMEs (`.so.0` and `.so.2`) must be rebuilt and relinked together
with this update; do not add compatibility symlinks. Envelope schema 1 remains
independent of library versioning.

Schemas install under `${datadir}/eyil/proto` for SDK tooling and consumers in
other languages. Installing them is not an instruction to regenerate duplicate
C++ bindings alongside `libeyilprotocol.so`. Yocto puts schemas, headers and
CMake metadata in the development package/SDK, while shared libraries belong
to runtime packages. Documentation, patches and test executables are not
installed. License collection for images belongs to the packaging system.

Yocto places the headers and CMake/pkg-config files in `eyil-secure-gateway-dev`;
they are available to consumer builds through the recipe sysroot/SDK and are not
required in the runtime image.

Client and Gateway must use matching generated rpc.proto and crypto.proto
definitions for the agreed schema. C++ consumers such as Firebolt use the public
`EyilProtocol::EyilProtocol` shared library and its installed generated headers.
Do not compile a second copy of those bindings into the same process: both RPC
and Crypto descriptors must have one shared implementation. EyilClient remains
the transport API; the protocol library only supplies message types and codecs.
The original ACs require the schemas and EyilClient, not independent C++ code
generation by every consumer. See the [SDK contract](../../../doc/BuildAndValidation.md).

### Contract: SDK-04 Optional tests and development tools

Source: [BV-005](../../../doc/BuildAndValidation.md#build-install-and-validation), [DC-003](../../../doc/DevelopmentClient.md#build-and-install).

`BUILD_TESTING`, `RDK_SERVICES_L1_TEST` and `RDK_SERVICE_L2_TEST` default to OFF.
The `dev` preset enables all three and the diagnostic tools; CI selects it
explicitly. A normal product build therefore compiles no test executables.
For a custom build directory, use `ctest --test-dir <dir> -L '^L1$'` or `'^L2$'`
with `--output-on-failure --no-tests=error`. Add `--output-junit <file>` when a
JUnit artifact is needed. Presets remain compatible with CMake 3.21.

For a development image, set `BUILD_EYIL_DEV_TOOLS=ON`. CMake defaults to OFF.
The current Yocto integration recipe deliberately enables `dev-tools` until
integration is complete; remove that PACKAGECONFIG feature for production.
The install target installs `/usr/bin/CryptoTestClient`. It uses the system ELF loader
and libraries in their normal runtime locations. No private library bundle,
loader copy or launcher is installed. Sources live in `tools/`, separately from
production runtime code in `src/`.
When Gateway is enabled, the same option also builds and installs
`libWPEFrameworkCryptoTestBackend.so` in the Thunder plugin directory and
`CryptoTestBackend.json` in its configuration directory. Both development artifacts
belong to the Yocto `eyil-secure-gateway-tools` package. With development tools
disabled, neither the CLI nor CryptoTestBackend is built or installed. A standalone
client build with Gateway disabled continues to require no Thunder SDK and
does not build CryptoTestBackend.
Yocto's shared-library dependency analysis supplies the required runtime packages,
including EyilClient, EyilProtocol, Protobuf, ZeroMQ and their dependencies.
Each package owns its libraries; normal stripping/debug splitting applies.
The CLI requires those compatible libraries inside the container as well as on
the host. A container with a separate rootfs must install or expose them through
its platform configuration. Host installation alone does not satisfy that case.

### Contract: SDK-05 In-process plugin and optional startup integration

Source: [BV-005](../../../doc/BuildAndValidation.md#build-install-and-validation), [DC-004](../../../doc/DevelopmentClient.md#build-integration-outside-the-component-repositories).

Gateway's generated configuration always uses in-process mode `Off`; there is
no implementation-mode CMake option. The wrapper rejects a manually supplied
remote mode. The implementation destructor drains the runtime on final Release;
remote destruction and library lifetime need separate validation. See [development and container validation](../../../doc/DevelopmentClient.md) for
the CLI, container runtime dependencies, startup-service selection and expected no-Crypto result.

The startup-services recipe installs an explicit optional service list. Keep
Eyil in that list so the common target requests it through `Wants`, not `Requires`:

EyilGateway starts independently through Thunder's optional service target.
It does not require RuntimeManager or the `/tmp/ai2managers` marker; RuntimeManager
does not require EyilGateway. Create the application container after the socket
appears when that application needs Gateway access.
Gateway binds and accepts valid clients even if Crypto is missing; backend RPCs
report `RPC_STATUS_INTERNAL`. The production default Crypto callsign is empty.
When development tools and Gateway are enabled, an empty CMake callsign produces
`org.rdk.CryptoTestBackend` in `configuration.backends`; an explicitly configured callsign
is preserved. The CMake cache value stays empty, so switching development tools
off restores the production default. Set the `crypto` entry in
`configuration.backends` when a real backend is installed. Do not
substitute the existing `org.rdk.Cryptography` plugin for `org.rdk.Crypto`.
Crypto startup ordering is the product's responsibility when real Crypto is
included. The component changes do not modify any Yocto layer or source revision.

## Related contracts and open decisions

[CI-04 Required libzmq extension and guarded replies](../connection-identity/spec.md#requirement-ci-04-required-libzmq-extension-and-guarded-replies), [CI-05 Startup capability verification before admission](../connection-identity/spec.md#requirement-ci-05-startup-capability-verification-before-admission), [CY-01 Configured backend list and availability](../crypto-integration/spec.md#requirement-cy-01-configured-backend-list-and-availability), [GL-01 Wrapper and implementation ownership](../gateway-lifecycle/spec.md#requirement-gl-01-wrapper-and-implementation-ownership).

