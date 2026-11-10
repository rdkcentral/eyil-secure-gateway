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

# Development build and container smoke test

The implementation lives in `src/clientlib` and `src/gateway`; public headers
and Protobuf stay in `apis`. No acceptance-criteria text is changed.

## Build and install

`build_dependencies.sh` prepares the pinned native Thunder SDK, the selected
`topic/crypto` headers and patched libzmq 4.3.4. The dependency build enables
PLAIN and draft monitors; CURVE is not needed by Eyil. Gateway probes option
10001 and performs a private IPC self-test before opening its production listener.
A stock libzmq library causes activation to fail explicitly.

```sh
bash build_dependencies.sh
bash build.sh -DBUILD_EYIL_DEV_TOOLS=ON
python3 scripts/run-tests.py --build-dir build --label L1
python3 scripts/run-tests.py --build-dir build --label L2
DESTDIR="$PWD/rootfs/dev" cmake --install build
```

For an existing SDK, set `EYIL_THUNDER_PREFIX`. Cross builds use the target
CMake toolchain and target dependencies, including the same libzmq patch and
compatible ICrypto headers/bindings. Set `EYIL_CRYPTO_INCLUDE_DIR` to the SDK
directory containing `interfaces/ICrypto.h` and matching `IConfiguration.h`,
`Ids.h`, `Module.h` and `entservices_errorcodes.h`.

For a development image, set `BUILD_EYIL_DEV_TOOLS=ON`. Production defaults to
OFF. The install target installs `/usr/bin/eyil-client` and a self-contained
runtime bundle in `/usr/libexec/eyil-dev`. The bundle includes a launcher,
the client executable, its ELF interpreter and required libraries.
`scripts/stage-dev-client.py` uses readelf and never executes target
binaries. Cross installs need `CMAKE_SYSROOT` or `EYIL_DEV_SYSROOT` containing
the target interpreter and runtime libraries. Keep the original dependency
package licenses in the image; the bundle does not change their licenses.

## Build integration outside the component repositories

The product recipe must build/install EyilGateway and EyilClient, apply
`doc/zmq_connection_lifetime.patch` to the image's libzmq, and supply the selected
ICrypto headers. Native bootstrap artifacts are not target-image libraries.

The startup-services recipe installs an explicit optional service list. Keep
Eyil in that list so the common target requests it through `Wants`, not `Requires`:

```bitbake
THUNDER_OPTIONAL_STARTUP_SERVICES ?= "wpeframework-eyilgateway.service"
```

RuntimeManager requests EyilGateway activation through `Wants=` and orders it
first with `After=`; the Gateway unit also declares `Before=` RuntimeManager.
A missing or failed Gateway service does not prevent RuntimeManager activation.
Gateway binds and accepts valid clients even if Crypto is missing; backend RPCs
report `RPC_STATUS_INTERNAL`. The default Crypto callsign is empty. Set the `crypto`
entry in `configuration.backends` when a backend is installed. Do not
substitute the existing `org.rdk.Cryptography` plugin for `org.rdk.Crypto`.
Crypto startup ordering is the product's responsibility when real Crypto is
included. The component changes do not modify any Yocto layer or source revision.

## Container mount

RALF's `RalfOCIConfigGenerator` and legacy `DobbySpecGenerator::createMounts`
add a bind mount for `/tmp/app-eyil-gateway.ipc` only when `stat()` succeeds and
the source is a Unix socket. The socket is absent from RALF's static
`resources/oci-base-spec.json`; the generator adds it conditionally. A missing
socket or another file type at that path does not add a mount or fail generation.
The container can start without Gateway access.

When present, the socket is mounted at the same path with
`ro,nosuid,nodev,noexec`. Read-only applies to the mounted filesystem object;
it does not prohibit Unix-domain socket communication. No host `/tmp` mount is
needed. If Gateway starts later, recreate the container and regenerate its
configuration to add the mount. Existing single-file binds also retain the old
inode after listener recreation, so affected containers must be recreated.
The existence check runs during configuration generation; removal between that
check and Dobby's bind mount can still fail container startup.

For development AppManagers builds (`RDK_APPMANAGERS_DEBUG`), both generation
paths additionally bind `/usr/libexec/eyil-dev` read-only when that directory
exists. It is executable, unlike the socket mount. This is why a host-rootfs
binary is actually reachable inside the app container. Enable development tools
only in development images and create the container after the bundle is installed.

## Run with no Crypto implementation

After the new image boots and an application container is running:

```sh
DobbyTool list
DobbyTool exec <container-id> '/usr/libexec/eyil-dev/run --expect-no-crypto'
```

The command syntax follows [Dobby's command documentation](https://github.com/rdkcentral/Dobby/blob/e534e3d461251c9f7cadcd6184cdf5b95c431061/Troubleshooting.md).

Expected output:

```text
connect=CLIENT_OK
response id=<id> rpc=RPC_STATUS_INTERNAL
string_response=RPC_STATUS_UNSUPPORTED_SERVICE
response id=<id> rpc=RPC_STATUS_OK crypto=CRYPTO_ERROR_NONE
PASS: 16 binary async responses, string sync/async, mixed sync, two connections, shared UID quota and release.
```

The output above illustrates the result types; request IDs and response order
vary. The CLI runs the same sync/async, string and buffer cases in both modes.
Without ICrypto, `CONNECTED` logs a backend failure but the client handshake
succeeds. Binary Crypto operations return `CLIENT_OK` with an actual
`RPC_STATUS_INTERNAL` response, including through async callbacks. Local buffer
allocation/release still succeeds; the fifth allocation produces the documented
quota error. No connection callback is emitted for a backend lookup failure.
`CLIENT_TIMEOUT` indicates an unavailable/stalled transport, not missing Crypto.
`CLIENT_AUTH_FAILED` means an explicit ZAP denial.

## Run with a backend

```sh
DobbyTool exec <container-id> '/usr/libexec/eyil-dev/run --exercise'
```

This exercises two connections, 16 binary async sends from application threads,
concurrent synchronous requests, both plain-string APIs, notification placeholders,
shared-UID buffer quotas and explicit release. Output contains request IDs and
RPC/Crypto status names, never cryptographic data. String transport returns
`CLIENT_OK` and `RPC_STATUS_UNSUPPORTED_SERVICE`. Allocate/release succeed;
a fifth allocation across the two connections returns `RPC_STATUS_OK` with
`CRYPTO_ERROR_OPERATION_FAILED`. The client checks these distinct error layers.

The CLI has its own 30-second response-wait budget so diagnostics can terminate.
This does not add an async timeout to EyilClient. Failure or timeout does not
prove remote completion or authorize buffer reuse.

## L1/L2 implementation evidence

L1 exercises the host-UID quota shared across connections, ownership rejection,
overflow-safe region checks, concurrent allocation, deferred release and scoped
adapter references. A separate C consumer checks the installed public API.

L2 runs real IPC with the patched libzmq, the private Thunder pool and a mock
implementing the actual ICrypto interface. It checks missing Crypto, independent
connections, out-of-order responses, more than 64 async requests, response delivery
after 15 seconds, the 10-second worker-start timeout, callback reentrancy, explicit
close and unexpected loss. Repeated errors are suppressed and summarized;
production Gateway diagnostics use Thunder logging. EyilClient diagnostics use
stderr without a Thunder dependency.

When a native Thunder executable and development CLI are available, an additional
L2 test loads the real Gateway wrapper/implementation and `libEyilCryptoMock.so`
in Thunder, runs the CLI with and without Crypto, and verifies orderly shutdown.
The mock produces deterministic test bytes, not cryptography. It is built only
for tests and is never installed into the image.

The initial implementation supports `configuration.root.mode=Off`. The wrapper's
final IConfiguration Release invokes the implementation destructor, which drains
accepted work and joins owned threads before releasing its retained IShell.
No additional lifecycle interface exists. Local mode remains unsupported until
remote destruction and library lifetime have been validated.
An unreturning backend call or uncertain remote buffer access cannot be forcibly
cancelled; the backend-completion and authentication-progress boundaries in the
architecture remain applicable. Container/Dobby validation follows on the new
image; native L2 is not a substitute for that test.
