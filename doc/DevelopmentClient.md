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
bash build.sh --preset dev
ctest --preset l1
ctest --preset l2
DESTDIR="$PWD/rootfs/dev" cmake --install build
```

For an existing SDK, set `EYIL_THUNDER_PREFIX`. Cross builds use the target
CMake toolchain and target dependencies, including the same libzmq patch and
compatible ICrypto headers/bindings. Set `EYIL_CRYPTO_INCLUDE_DIR` to the SDK
directory containing `interfaces/ICrypto.h` and matching `IConfiguration.h`,
`Ids.h`, `Module.h` and `entservices_errorcodes.h`.

For a development image, set `BUILD_EYIL_DEV_TOOLS=ON`. CMake defaults to OFF.
The current Yocto integration recipe deliberately enables `dev-tools` until
integration is complete; remove that PACKAGECONFIG feature for production. The install target installs `/usr/bin/eyil-client` and a self-contained
runtime bundle in `/usr/lib/eyil`. The bundle includes a launcher,
the client executable, its ELF interpreter and required libraries. Sources live
in `tools/`, separately from production runtime code in `src/`. The private
library copies let the host-built CLI run in an app container with its own
rootfs. Host package dependencies alone do not make those libraries available
inside a container. The `run` launcher executes the bundled ELF interpreter with
`--library-path` pointing at this directory; it does not activate plugins.
The bundle can be removed once the app rootfs supplies compatible dependencies;
its copied libraries are private and do not replace Yocto's rootfs providers.
When Gateway is enabled, the same option also builds and installs
`libWPEFrameworkCryptoTestBackend.so` in the Thunder plugin directory and
`CryptoTestBackend.json` in its configuration directory. Both development artifacts
belong to the Yocto `eyil-secure-gateway-tools` package. With development tools
disabled, neither the CLI nor CryptoTestBackend is built or installed. A standalone
client build with Gateway disabled continues to require no Thunder SDK and
does not build CryptoTestBackend.
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
report `RPC_STATUS_INTERNAL`. The production default Crypto callsign is empty.
When development tools and Gateway are enabled, an empty CMake callsign produces
`org.rdk.CryptoTestBackend` in `configuration.backends`; an explicitly configured callsign
is preserved. The CMake cache value stays empty, so switching development tools
off restores the production default. Set the `crypto` entry in
`configuration.backends` when a real backend is installed. Do not
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
paths additionally bind `/usr/lib/eyil` read-only when that directory
exists. It is executable, unlike the socket mount. This is why a host-rootfs
binary is actually reachable inside the app container. Enable development tools
only in development images and create the container after the bundle is installed.

## Run with no Crypto implementation

After the new image boots and an application container is running:

```sh
DobbyTool list
DobbyTool exec <container-id> '/usr/lib/eyil/run --expect-no-crypto'
```

For this scenario, leave CryptoTestBackend inactive and do not activate another backend
at the configured callsign. Installing the development plugin does not activate
it: its Thunder R4 configuration has `autostart: false`.

The command syntax follows [Dobby's command documentation](https://github.com/rdkcentral/Dobby/blob/e534e3d461251c9f7cadcd6184cdf5b95c431061/Troubleshooting.md).

Example output (IDs, timings and summary counts depend on the selected mode):

```text
CONNECT connection=A local=CLIENT_OK
RUN case=CRYPTO_OPERATION_LOAD_KEY/async expected=CLIENT_OK / RPC_STATUS_INTERNAL / empty payload
CALL operation=CRYPTO_OPERATION_LOAD_KEY mode=async
SEND connection=A mode=async slot=0 local=CLIENT_OK
REPLY connection=A mode=async slot=0 id=<id> rpc=RPC_STATUS_INTERNAL
PASS case=CRYPTO_OPERATION_LOAD_KEY/async duration_ms=<time>
SUMMARY passed=<count> failed=0 skipped=<count> callbacks=<count> connection_events=0 result=PASS
```

Without ICrypto, `CONNECTED` logs a backend failure but the client handshake
succeeds. The CLI checks all 15 backend operations through both sync and async
APIs. Valid owned buffers are supplied for Encrypt/Decrypt/Sign/Verify, so these
cases exercise backend lookup rather than failing earlier buffer validation.
The expected local result is `CLIENT_OK`, with `RPC_STATUS_INTERNAL` and an empty
service payload. Gateway-local allocation/release still works. No connection
callback is expected for a backend lookup failure. `CLIENT_TIMEOUT` indicates
an unavailable/stalled transport; `CLIENT_AUTH_FAILED` means an explicit denial.

## Run with a backend

```sh
DobbyTool exec <container-id> '/usr/lib/eyil/run --exercise'
```

This mode tests transport, GenerateRandom, both string APIs, local argument
validation, notification placeholders, buffer limits/ownership, malformed service
requests and disconnect cleanup. It skips fixed key and bulk-output expectations
that require CryptoTestBackend. It does not invoke key import, unload or flush
on an arbitrary backend. String calls return `CLIENT_OK` and the text
`RPC_STATUS_UNSUPPORTED_SERVICE`.

### Test reporting and failure handling

Each selected case prints `RUN` with its expectation, then `PASS`, `FAIL` or
`FATAL`. `SEND` records the connection, delivery mode and local result; `REPLY`
records the request ID and Gateway/service statuses. `STEP` identifies the bulk
operation being checked. Payloads, keys and buffer contents are not logged.
Failed assertions include the expected and actual status or the failed invariant.
`SKIP` states why a case is not applicable or cannot run after a fatal failure.

An independent assertion failure marks the run failed and allows remaining
cases to run. Loss of connection, send/receive failure, an invalid or duplicate
callback, cleanup failure or an unresolved response stops further operations.
Connections are closed and callbacks quiesced before test state is destroyed.
The process returns 0 only when every selected case passed, 1 for any test/setup
failure, and 2 for invalid command-line arguments. The final `SUMMARY` contains
passed/failed/skipped counts and callback/connection-event counts.

The CLI has a 30-second wait budget per async batch, solely so diagnostics can
terminate. It does not add a response timeout to EyilClient. On an uncertain
outcome the helper does not reuse or explicitly release affected allocations;
connection teardown leaves Gateway responsible for its normal lease-aware
cleanup. A local timeout is never treated as remote cancellation.

Quota tests require all four slots for the diagnostic process's host UID to be
available. Run without other Gateway clients consuming that UID's allowance;
a failure caused by external quota use is not proof of a Gateway defect.
The disconnect test observes only its own names for up to five seconds before
checking reclaimed quota. This diagnostic observation is not an idle watchdog.

## Development CryptoTestBackend

CryptoTestBackend implements ICrypto for diagnostics only. It does not implement AES,
HMAC, secure random generation, key storage or key authorization, even though
requests use the normal Crypto enum selectors. It logs a development-only notice
on activation. It retains sessions by both appId and sessionId and keeps no
buffer descriptor, mapping or input/output data after an operation returns.

| Operation | Deterministic test behavior |
| --- | --- |
| GenerateRandom | Return the requested number of `0x5a` bytes |
| GenerateKey, LoadKey, ImportKey | Return fixed key ID 1 |
| GenerateKeyPair / DeriveSessionKeys | Return IDs 2/3 and 4/5/6 respectively |
| KeyExists / GetKeyInfo / ExportKey | Fixed test metadata and `45 59 49 4c` export bytes; known IDs are 1–6 |
| UnloadKey / FlushKeys | Successful placeholders; fixed test IDs remain available until session disconnect |
| Encrypt / Decrypt | XOR every input byte with `0xa5`; output has the same length, IV contents are ignored |
| Sign | Start with 32 bytes of `0x5a`; XOR input byte `i` into signature byte `i % 32` |
| Verify | Compare the supplied signature with the above checksum; a mismatch returns false with no service error |

Outputs start at the first 16-byte boundary after the highest declared input
end. The backend checks file type/size, overflow-safe ranges and input overlap.
It opens with `O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK` and uses `pread`/`pwrite`
with temporary private storage. An incomplete read/write is an operation failure.
`BUFFER_OVERFLOW` returns the required region without writing output. These are
test behaviors, not additional requirements on a real Crypto implementation.

Activate on the host before opening client connections, then execute inside the
application container:

```sh
/usr/bin/PluginActivator org.rdk.CryptoTestBackend
DobbyTool exec <container-id> '/usr/lib/eyil/run --crypto-test-backend'
```

Gateway must already be active with `crypto` routed to `org.rdk.CryptoTestBackend`.
The mode checks all 17 Protobuf operations. The following ten groups require no
artificial delay or new control API in the backend:

| Group | Checks |
| --- | --- |
| Complete operation routing | All eleven key/random methods through sync and async; fixed IDs, metadata and bytes; optional import/export formats; Unload/Flush placeholder replies |
| Async shared memory | Encrypt/Decrypt/Sign/Verify on four independent buffers across two connections; exact output, unchanged inputs/unrelated bytes, Verify true and false; also a synchronous round trip |
| Concurrent correlation | Sixteen different random lengths submitted from application threads plus a synchronous call; a distinct callback slot per request; unique IDs per connection; caller-supplied ID/schema overwritten |
| Allocation limits and quota | 0, 1, 1,572,864 and 1,572,865 bytes; shared four-buffer host-UID limit; releasing one buffer allows allocation through another connection |
| Buffer ownership | Foreign use/release rejected; original owner can still use/release; unknown, released and twice-released IDs rejected |
| Invalid key/regions and capacity | Unknown keys, missing/overlapping regions, exact boundaries, offset/length overflow, aligned output and BUFFER_OVERFLOW without reading required-but-unavailable output |
| Invalid requests and public arguments | Unknown service/operation/enums, malformed or empty Crypto payload, operation/oneof mismatch, duplicate usages, invalid optional format; NULL arguments, output clearing, occupied handle, notifications and raw strings |
| Disconnect cleanup | Leave three allocations on A, disconnect A and observe removal; B keeps its allocation; fresh A rejects old IDs; all reclaimed quota can be allocated again |
| Repeated lifecycle | Ten connect/async/allocate/release/disconnect cycles; no callback on explicit close; final explicit teardown checked before SUMMARY |
| Missing backend | Separate `--expect-no-crypto` run checks all 15 ICrypto operations fail at Gateway level while local Allocate/Release continues to work |

The callback functions only transfer owned response storage and signal the
waiting application thread. Parsing, validation and logging happen there, not
on the Client I/O thread. Request slots establish correlation without adding a
user-context argument to the public API. Output comparisons include guard bytes
outside the returned region. Concurrent tests accept any completion order; they
do not force a particular order or prove a worker was occupied for a duration.

The test backend still has no real key store: Import/Export is not a real
round trip and Unload/Flush does not revoke fixed IDs. Controlled timeout,
worker-exhaustion, release-during-execution and deliberately reversed-response
scenarios remain in the separate L1/L2 fixtures; the installed backend is not
modified to delay its operations.

### Startup services proposal — not installed by this change

For temporary automatic activation, add
`wpeframework-crypto-test-backend.service` to `thunder-startup-services`, with
`ExecStart=/usr/bin/PluginActivator org.rdk.CryptoTestBackend`, `After=wpeframework.service`
and `Before=wpeframework-eyilgateway.service`. Use the existing startup-marker
conditions and `ConditionPathExists=/etc/WPEFramework/plugins/CryptoTestBackend.json`.
Add it to the recipe's `THUNDER_OPTIONAL_STARTUP_SERVICES` for development images
and request/order it from EyilGateway through `Wants`/`After`, not `Requires`.
This lets CryptoTestBackend start before the first client CONNECTED notification while
preserving Gateway startup without a backend. Remove the temporary service and
dependency when the real Crypto backend is integrated. The service repository
and its recipe are unchanged here; manual activation above is sufficient.

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
The L2 mock produces deterministic test bytes, not cryptography. It is built only
for tests and is never installed into the image. The separate development
CryptoTestBackend plugin is installed only with the CLI; the same Thunder L2 harness
loads its generated configuration and runs `--crypto-test-backend` as a third scenario.
The harness checks required case names and summary/exit status, saves per-run CLI
logs, verifies that mismatched fixed replies produce FAIL while later independent
cases and disconnect still run, and checks fatal startup against an unavailable
Gateway. Missing-backend log suppression is checked separately from intentional
protocol/buffer validation errors. Its L1 tests cover session isolation, fixed replies, XOR round trips, signature
verification, overflow/range/overlap errors and rejected zero-sized, oversized,
symlink, FIFO and missing files.

The initial implementation supports `configuration.root.mode=Off`. The wrapper's
final IConfiguration Release invokes the implementation destructor, which drains
accepted work and joins owned threads before releasing its retained IShell.
No additional lifecycle interface exists. Local mode remains unsupported until
remote destruction and library lifetime have been validated.
An unreturning backend call or uncertain remote buffer access cannot be forcibly
cancelled; the backend-completion and authentication-progress boundaries in the
architecture remain applicable. Container/Dobby validation follows on the new
image; native L2 is not a substitute for that test.
