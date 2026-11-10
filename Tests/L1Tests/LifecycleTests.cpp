/**
 * Copyright 2026 RDK Management
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../support/Cases.h"
#include "../support/MockShell.h"
#include "EyilGatewayImplementation.h"
#include "EyilGateway.h"
#include <eyilclient/EyilClient.h>
#include <future>
#include <filesystem>
#include <dlfcn.h>
using namespace EyilTest;

namespace RuntimeFault
{
    std::atomic<bool> poll { false };
    std::atomic<int> wakeFd { -1 };
}

// Interpose at the shared-library boundary too: Configure owns a runtime inside
// the implementation DSO, so GNU ld --wrap on the test executable is insufficient.
extern "C" int zmq_poll(zmq_pollitem_t *items, int count, long timeout)
{
    using Poll = int (*)(zmq_pollitem_t *, int, long);
    static const auto original = reinterpret_cast<Poll>(dlsym(RTLD_NEXT, "zmq_poll"));
    bool router = false;
    if (count == 2 && items[0].socket)
    {
        int type = -1;
        size_t size = sizeof(type);
        router = !zmq_getsockopt(items[0].socket, ZMQ_TYPE, &type, &size) && type == ZMQ_ROUTER;
        if (router)
            RuntimeFault::wakeFd = items[1].fd;
    }
    const auto result = original(items, count, timeout);
    if (router && RuntimeFault::poll.exchange(false))
    {
        errno = EIO;
        return -1;
    }
    return result;
}

namespace
{
    class FrameworkPool : public Core::IWorkerPool
    {
    public:
        FrameworkPool() { Assign(this); }

        ~FrameworkPool() override { Assign(nullptr); }

        void Submit(const Core::ProxyType<Core::IDispatch> &job) override
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending = job;
            ++submissions;
        }

        Core::ProxyType<Core::IDispatch> Take()
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto job = pending;
            pending.Release();
            return job;
        }

        ::ThreadId Id(uint8_t) const override { return {}; }

        void Schedule(const Core::Time &, const Core::ProxyType<Core::IDispatch> &) override { CHECK(false); }

        bool Reschedule(const Core::Time &, const Core::ProxyType<Core::IDispatch> &) override { return false; }

        uint32_t Revoke(const Core::ProxyType<Core::IDispatch> &, uint32_t) override { return Core::ERROR_NONE; }

        void Join() override { }

        const Metadata &Snapshot() const override { return metadata; }

        std::atomic<unsigned> submissions { 0 };

    private:
        std::mutex mutex;
        Core::ProxyType<Core::IDispatch> pending;
        Metadata metadata {};
    };

    struct Implementation
    {
        WPEFramework::Exchange::IConfiguration *value = Core::Service<
            WPEFramework::Plugin::EyilGatewayImplementation>::Create<WPEFramework::Exchange::IConfiguration>();

        ~Implementation()
        {
            if (value)
                value->Release();
        }

        uint32_t Destroy()
        {
            auto *p = value;
            value = nullptr;
            return p->Release();
        }
    };

    struct ClientHandle
    {
        Client *value = nullptr;

        ~ClientHandle() { client_disconnect(value); }
    };

    void Reply(Client *, uint8_t *p, size_t) { client_free_response(p); }

    std::string Wire(const CryptoRequest &request)
    {
        RpcRequest rpc;
        rpc.set_service(RPC_SERVICE_CRYPTO);
        rpc.set_payload(request.SerializeAsString());
        return rpc.SerializeAsString();
    }

    std::string Allocate(Client *client)
    {
        CryptoRequest request;
        request.set_operation(CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER);
        request.mutable_allocate_shared_buffer()->set_requested_size(4096);
        auto bytes = Wire(request);
        uint8_t *data = nullptr;
        size_t size = 0;
        CHECK(client_send_request(client, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), &data, &size)
            == CLIENT_OK);
        std::unique_ptr<uint8_t, decltype(&client_free_response)> owned(data, client_free_response);
        RpcResponse rpc;
        CHECK(rpc.ParseFromArray(data, size) && rpc.status() == RPC_STATUS_OK);
        CryptoResponse response;
        CHECK(response.ParseFromString(rpc.payload()) && response.error() == CRYPTO_ERROR_NONE);
        return "/dev/shm" + response.allocate_shared_buffer().name();
    }

    void InvalidConfig()
    {
        MockShell shell;
        {
            Implementation i;
            CHECK(i.value);
            CHECK(i.value->Configure(nullptr) == Core::ERROR_BAD_REQUEST);
        }
        for (auto config : { "{", "null", "{}{}", "{ } trailing", R"({"workers":0})", R"({"workers":256})",
                 R"({"workers":null})", R"({"backends":null})", R"({"backends":[null]})", R"({"backends":[{}]})",
                 R"({"backends":[{"callsign":"test.crypto"}]})",
                 R"({"backends":[{"service":"","callsign":"test.crypto"}]})",
                 R"({"backends":[{"service":null,"callsign":"test.crypto"}]})",
                 R"({"backends":[{"service":"crypto","callsign":null}]})",
                 R"({"backends":[{"service":"crypto"},{"service":"crypto","callsign":"test.crypto"}]})" })
        {
            Implementation i;
            shell.config = config;
            if (i.value->Configure(&shell) != Core::ERROR_BAD_REQUEST)
                throw std::runtime_error(std::string("Invalid configuration accepted: ") + config);
            CHECK(shell.references == 1 && shell.crypto.queries == 0);
            CHECK(i.Destroy() == Core::ERROR_DESTRUCTION_SUCCEEDED);
        }
    }

    void NormalDestroy(bool withWork)
    {
        RequireUnusedEndpoint();
        MockShell shell;
        Implementation i;
        CHECK(i.value->Configure(&shell) == Core::ERROR_NONE && shell.references == 2);
        CHECK(i.value->Configure(&shell) == Core::ERROR_BAD_REQUEST && shell.references == 2);
        ClientHandle client;
        CHECK(client_connect(&client.value, nullptr) == CLIENT_OK);
        auto name = Allocate(client.value);
        CHECK(access(name.c_str(), F_OK) == 0);

        struct Unblock
        {
            MockCrypto &c;

            ~Unblock() { c.Unblock(); }
        } unblock { shell.crypto };

        if (withWork)
        {
            shell.crypto.block = true;
            CryptoRequest request;
            request.set_operation(CRYPTO_OPERATION_GENERATE_RANDOM);
            request.mutable_generate_random()->set_number_of_bytes(10);
            auto bytes = Wire(request);
            CHECK(client_send_request_async(
                      client.value, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), Reply)
                == CLIENT_OK);
            Eventually([&] { return shell.crypto.entered.load() > 0; });
            auto destroying = std::async(std::launch::async, [&] { return i.Destroy(); });
            auto waiting = destroying.wait_for(std::chrono::milliseconds(100));
            const bool retained
                = shell.references == 2 && shell.crypto.references == 2 && shell.crypto.SessionCount() == 1;
            shell.crypto.Unblock();
            CHECK(destroying.get() == Core::ERROR_DESTRUCTION_SUCCEEDED);
            CHECK(waiting == std::future_status::timeout && retained);
        }
        else
            CHECK(i.Destroy() == Core::ERROR_DESTRUCTION_SUCCEEDED);
        CHECK(shell.references == 1 && shell.crypto.references == 1 && shell.crypto.SessionCount() == 0);
        CHECK(!shell.prematureRelease && access(name.c_str(), F_OK) != 0);
        client_disconnect(client.value);
        client.value = nullptr;
        // A new instance must be able to own the same endpoint after destruction.
        Implementation replacement;
        CHECK(replacement.value->Configure(&shell) == Core::ERROR_NONE);
        CHECK(replacement.Destroy() == Core::ERROR_DESTRUCTION_SUCCEEDED && shell.references == 1);
    }

    void PartialStartup()
    {
        RequireUnusedEndpoint();
        const char *path = "/tmp/app-eyil-gateway.ipc";
        // Only remove our inactive test endpoint after RequireUnusedEndpoint.
        unlink(path);
        CHECK(mkdir(path, 0700) == 0);

        struct Directory
        {
            const char *p;

            ~Directory() { rmdir(p); }
        } directory { path };

        MockShell shell;
        Implementation i;
        CHECK(i.value->Configure(&shell) == Core::ERROR_GENERAL);
        CHECK(shell.references == 1 && shell.crypto.queries == 0);
        CHECK(rmdir(path) == 0);
        CHECK(i.value->Configure(&shell) == Core::ERROR_NONE);
        CHECK(i.Destroy() == Core::ERROR_DESTRUCTION_SUCCEEDED && shell.references == 1);
    }

    void MissingBackend(const char *config = nullptr)
    {
        RequireUnusedEndpoint();
        MockShell shell;
        if (config)
            shell.config = config;
        shell.crypto.available = false;
        Implementation i;
        CHECK(i.value->Configure(&shell) == Core::ERROR_NONE);
        CHECK(shell.lookups == 0); // Startup never queries any backend.
        ClientHandle client;
        CHECK(client_connect(&client.value, nullptr) == CLIENT_OK);
        CHECK(client.value && shell.crypto.SessionCount() == 0);
        const auto name = Allocate(client.value);
        CHECK(access(name.c_str(), F_OK) == 0);

        CryptoRequest request;
        request.set_operation(CRYPTO_OPERATION_GENERATE_RANDOM);
        request.mutable_generate_random()->set_number_of_bytes(8);
        const auto bytes = Wire(request);
        uint8_t *data = nullptr;
        size_t size = 0;
        CHECK(client_send_request(
                  client.value, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), &data, &size)
            == CLIENT_OK);
        std::unique_ptr<uint8_t, decltype(&client_free_response)> owned(data, client_free_response);
        RpcResponse response;
        CHECK(response.ParseFromArray(data, size));
        CHECK(response.status() == RPC_STATUS_INTERNAL && response.payload().empty());
        CHECK(shell.crypto.entered == 0);
        if (config)
            CHECK(shell.lookups == 0); // Unconfigured never means "query current plugin".

        CHECK(i.Destroy() == Core::ERROR_DESTRUCTION_SUCCEEDED && shell.references == 1);
        CHECK(access(name.c_str(), F_OK) != 0);
    }

    void WrapperFailure(bool missingImplementation)
    {
        RequireUnusedEndpoint();
        MockShell shell;
        shell.config = missingImplementation ? R"({"root":{"mode":"Off","locator":"missing-eyil-implementation.so"}})"
                                             : R"({"workers":0})";
        auto *plugin = Core::Service<WPEFramework::Plugin::EyilGateway>::Create<WPEFramework::PluginHost::IPlugin>();
        CHECK(plugin);

        struct ReleasePlugin
        {
            WPEFramework::PluginHost::IPlugin *plugin;
            MockShell &shell;

            ~ReleasePlugin()
            {
                plugin->Deinitialize(&shell);
                plugin->Release();
            }
        } cleanup { plugin, shell };

        CHECK(!plugin->Initialize(&shell).empty());
        // Initialize reports an error and leaves wrapper teardown to Thunder.
        CHECK(shell.references == 2 && shell.lookups == 0);
        plugin->Deinitialize(&shell);
        CHECK(shell.references == 1);
        plugin->Deinitialize(&shell);
        CHECK(shell.references == 1);

        shell.config = R"({"workers":1})";
        CHECK(plugin->Initialize(&shell).empty());
        CHECK(shell.references == 3);
        plugin->Deinitialize(&shell);
        CHECK(shell.references == 1);
    }

    void VolatileDirectory()
    {
        RequireUnusedEndpoint();
        const auto directory = std::filesystem::temp_directory_path() / ("eyil-volatile-test-" + Eyil::Token());

        struct Cleanup
        {
            std::filesystem::path directory;

            ~Cleanup() { std::filesystem::remove_all(directory); }
        } cleanup { directory };

        MockShell shell;
        shell.volatilePath = directory.string();
        Implementation i;
        CHECK(i.value->Configure(&shell) == Core::ERROR_NONE);
        CHECK(std::filesystem::is_directory(directory) && std::filesystem::is_empty(directory));
        CHECK(i.Destroy() == Core::ERROR_DESTRUCTION_SUCCEEDED && shell.references == 1);
        // A regular file cannot serve as the volatile directory. Fail cleanly, then allow reconfiguration.
        std::filesystem::remove(directory);
        Eyil::Fd file(open(directory.c_str(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0600));
        CHECK(file.Get() >= 0);
        Implementation failed;
        CHECK(failed.value->Configure(&shell) == Core::ERROR_GENERAL && shell.references == 1);
        CHECK(unlink(directory.c_str()) == 0);
        CHECK(failed.value->Configure(&shell) == Core::ERROR_NONE);
        CHECK(failed.Destroy() == Core::ERROR_DESTRUCTION_SUCCEEDED && shell.references == 1);
    }

    void BackendList()
    {
        RequireUnusedEndpoint();
        MockShell shell;
        shell.config = R"({"workers":2,"backends":[{"service":"future-service","callsign":"other.plugin"},
            {"service":"crypto","callsign":"test.crypto"}]})";
        Implementation i;
        CHECK(i.value->Configure(&shell) == Core::ERROR_NONE && shell.lookups == 0);
        ClientHandle client;
        CHECK(client_connect(&client.value, nullptr) == CLIENT_OK);
        CHECK(shell.lookups == 1 && shell.crypto.SessionCount() == 1);
        CHECK(i.Destroy() == Core::ERROR_DESTRUCTION_SUCCEEDED);
        CHECK(shell.lookups == 2 && shell.crypto.SessionCount() == 0);
    }

    void RuntimeDeactivation()
    {
        RequireUnusedEndpoint();
        FrameworkPool pool;
        MockShell shell;
        auto *plugin = Core::Service<WPEFramework::Plugin::EyilGateway>::Create<WPEFramework::PluginHost::IPlugin>();
        CHECK(plugin);

        struct Cleanup
        {
            WPEFramework::PluginHost::IPlugin *plugin;
            MockShell &shell;

            ~Cleanup()
            {
                RuntimeFault::poll = false;
                shell.onDeactivate = {};
                plugin->Deinitialize(&shell);
                plugin->Release();
            }
        } cleanup { plugin, shell };

        CHECK(plugin->Initialize(&shell).empty() && pool.submissions == 0);
        ClientHandle client;
        CHECK(client_connect(&client.value, nullptr) == CLIENT_OK);
        const auto name = Allocate(client.value);
        shell.onDeactivate = [&](WPEFramework::PluginHost::IShell::reason why)
        {
            CHECK(why == WPEFramework::PluginHost::IShell::FAILURE);
            plugin->Deinitialize(&shell);
        };

        RuntimeFault::poll = true;
        const uint64_t one = 1;
        CHECK(write(RuntimeFault::wakeFd.load(), &one, sizeof(one)) == sizeof(one));
        Eventually([&] { return pool.submissions == 1; });
        CHECK(shell.deactivations == 0); // Never deactivate inline on the failing owner.
        auto job = pool.Take();
        CHECK(job.IsValid());
        job->Dispatch(); // Framework thread calls the wrapper, whose destructor joins I/O.
        job.Release();
        CHECK(shell.deactivations == 1 && shell.State() == WPEFramework::PluginHost::IShell::DEACTIVATED);
        CHECK(pool.submissions == 1 && shell.references == 1 && shell.crypto.references == 1);
        CHECK(shell.crypto.SessionCount() == 0 && access(name.c_str(), F_OK) != 0);
        CHECK(!shell.prematureRelease);
    }
}

int main(int argc, char **argv)
{
    return RunCases(argc, argv,
        { { "invalid-config", InvalidConfig }, { "partial-startup", PartialStartup },
            { "destructor-idle", [] { NormalDestroy(false); } }, { "destructor-inflight", [] { NormalDestroy(true); } },
            { "missing-backend", [] { MissingBackend(); } },
            { "default-backends",
                []
                {
                    for (const auto *config : { "", " \t\n", "{}", " { \n } \t" })
                        MissingBackend(config);
                } },
            { "empty-backends", [] { MissingBackend(R"({"backends":[]})"); } },
            { "empty-callsign", [] { MissingBackend(R"({"backends":[{"service":"crypto","callsign":""}]})"); } },
            { "omitted-callsign", [] { MissingBackend(R"({"backends":[{"service":"crypto"}]})"); } },
            { "backend-list", BackendList }, { "wrapper-failed-config", [] { WrapperFailure(false); } },
            { "runtime-deactivation", RuntimeDeactivation },
            { "wrapper-missing-implementation", [] { WrapperFailure(true); } },
            { "volatile-directory", VolatileDirectory } });
}
