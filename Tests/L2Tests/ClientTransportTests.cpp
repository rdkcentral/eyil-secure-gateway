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

#include "../support/Test.h"
#include "../../src/common/RuntimeSupport.h"
#include <eyilclient/EyilClient.h>
#include <rpc.pb.h>
#include <atomic>
#include <future>
#include <vector>
#include <cstdlib>

// Injection affects only allocations on the calling test thread, never a
// transport/worker thread. It verifies C-boundary rollback before publication.
thread_local bool failAllocation = false;

void *operator new(std::size_t size)
{
    if (failAllocation)
        throw std::bad_alloc();
    if (auto *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void operator delete[](void *p) noexcept { std::free(p); }

void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

void operator delete(void *p) noexcept { std::free(p); }

void operator delete(void *p, std::size_t) noexcept { std::free(p); }

// Keep nothrow allocation in the same test allocator domain. libzmq uses
// nothrow new; mixing ASan's default implementation with our delete/free
// overrides would report an allocator mismatch unrelated to the code tested.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return ::operator new(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return ::operator new[](size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }

void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }

namespace
{
    class Server
    {
    public:
        std::atomic<bool> stop { false };
        std::atomic<int> mode { 0 };
        std::atomic<unsigned> requests { 0 }, replies { 0 };
        std::thread thread;

        Server()
        {
            auto startup = std::make_shared<std::promise<void>>();
            auto ready = startup->get_future();
            thread = std::thread(
                [this, startup]
                {
                    try
                    {
                        Eyil::Context context;
                        Eyil::Socket router(context.Get(), ZMQ_ROUTER), zap(context.Get(), ZMQ_REP);
                        router.ConfigureTransport();
                        router.Set(ZMQ_PLAIN_SERVER, 1);
                        zap.Bind("inproc://zeromq.zap.01");
                        router.Bind(Eyil::Endpoint);
                        startup->set_value();
                        while (!stop)
                        {
                            zmq_pollitem_t items[]
                                = { { router.Get(), 0, ZMQ_POLLIN, 0 }, { zap.Get(), 0, ZMQ_POLLIN, 0 } };
                            zmq_poll(items, 2, 20);
                            if (items[1].revents & ZMQ_POLLIN)
                            {
                                std::vector<std::string> frames;
                                for (;;)
                                {
                                    Eyil::Message message;
                                    CHECK(message.Receive(zap.Get()));
                                    frames.push_back(message.String());
                                    if (!message.More())
                                        break;
                                }
                                CHECK(frames.size() == 8);
                                std::string parts[] = { "1.0", frames[1], "200", "OK", "test-user", "" };
                                for (unsigned i = 0; i < 6; ++i)
                                    CHECK(zmq_send(zap.Get(), parts[i].data(), parts[i].size(), i < 5 ? ZMQ_SNDMORE : 0)
                                        >= 0);
                            }
                            if (items[0].revents & ZMQ_POLLIN)
                            {
                                Eyil::Message route, body;
                                CHECK(route.Receive(router.Get()) && route.More());
                                CHECK(body.Receive(router.Get()) && !body.More());
                                RpcRequest request;
                                CHECK(request.ParseFromArray(body.Data(), body.Size()));
                                ++requests;
                                const int selected = mode.load();
                                if (selected == 3)
                                    continue; // connected server deliberately drops a reply
                                RpcResponse response;
                                response.set_request_id(request.request_id());
                                response.set_schema_version(1);
                                response.set_service(selected == 1 ? static_cast<RpcService>(99) : request.service());
                                response.set_status(RPC_STATUS_INTERNAL);
                                const auto bytes
                                    = selected == 2 ? std::string("\x80", 1) : response.SerializeAsString();
                                CHECK(zmq_send(router.Get(), route.Data(), route.Size(), ZMQ_SNDMORE) >= 0);
                                CHECK(zmq_send(router.Get(), bytes.data(), bytes.size(), 0) >= 0);
                                ++replies;
                            }
                        }
                    }
                    catch (...)
                    {
                        try
                        {
                            startup->set_exception(std::current_exception());
                        }
                        catch (...)
                        {
                        }
                    }
                });
            try
            {
                ready.get();
            }
            catch (...)
            {
                stop = true;
                thread.join();
                throw;
            }
        }

        ~Server()
        {
            stop = true;
            thread.join();
        }
    };

    std::atomic<unsigned> callbacks { 0 }, losses { 0 };

    void Response(Client *, uint8_t *data, size_t)
    {
        ++callbacks;
        client_free_response(data);
    }

    void Event(Client *, ClientConnectionEvent event)
    {
        if (event == CLIENT_CONNECTION_LOST)
            ++losses;
    }

    struct Handle
    {
        Client *value = nullptr;

        ~Handle() { client_disconnect(value); }
    };

    ClientResult Sync(Client *client, const std::string &wire)
    {
        uint8_t *result = reinterpret_cast<uint8_t *>(1);
        size_t size = 123;
        auto status
            = client_send_request(client, reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), &result, &size);
        if (status == CLIENT_OK)
        {
            RpcResponse response;
            CHECK(response.ParseFromArray(result, size));
            CHECK(response.status() == RPC_STATUS_INTERNAL);
            client_free_response(result);
        }
        else
            CHECK(!result && !size);
        return status;
    }
}

int main()
{
    try
    {
        RequireUnusedEndpoint();
        Client *empty = nullptr;
        failAllocation = true;
        const auto allocation = client_connect(&empty, nullptr);
        failAllocation = false;
        CHECK(allocation == CLIENT_OUT_OF_MEMORY && !empty);
        Server server;
        Handle handle;
        CHECK(client_connect(&handle.value, Event) == CLIENT_OK);
        RpcRequest request;
        request.set_service(RPC_SERVICE_CRYPTO);
        request.set_payload("opaque bytes");
        const auto wire = request.SerializeAsString();
        CHECK(Sync(handle.value, wire) == CLIENT_OK);
        CHECK(losses == 0);
        const auto count = server.requests.load();
        uint8_t *bytes = reinterpret_cast<uint8_t *>(1);
        size_t size = 55;
        failAllocation = true;
        auto status = client_send_request(
            handle.value, reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), &bytes, &size);
        failAllocation = false;
        CHECK(status == CLIENT_OUT_OF_MEMORY && !bytes && !size);
        failAllocation = true;
        status = client_send_request_async(
            handle.value, reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), Response);
        failAllocation = false;
        CHECK(status == CLIENT_OUT_OF_MEMORY);
        char *text = reinterpret_cast<char *>(1);
        failAllocation = true;
        status = client_send_request_string(handle.value, "text", &text);
        failAllocation = false;
        CHECK(status == CLIENT_OUT_OF_MEMORY && !text);
        failAllocation = true;
        status = client_send_request_string_async(handle.value, "text",
            [](Client *, char *value)
            {
                ++callbacks;
                client_free_response(value);
            });
        failAllocation = false;
        CHECK(status == CLIENT_OUT_OF_MEMORY);
        CHECK(server.requests == count && callbacks == 0);
        server.mode = 1;
        CHECK(Sync(handle.value, wire) == CLIENT_COMMUNICATION_ERROR);
        CHECK(losses == 0);
        CHECK(client_send_request_async(
                  handle.value, reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), Response)
            == CLIENT_OK);
        Eventually([&] { return server.replies >= count + 2; });
        // A following correlated request establishes that the malformed async reply was processed.
        server.mode = 0;
        CHECK(Sync(handle.value, wire) == CLIENT_OK);
        CHECK(callbacks == 0);
        server.mode = 3;
        const auto after = server.requests.load();
        CHECK(client_send_request_async(
                  handle.value, reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), Response)
            == CLIENT_OK);
        Eventually([&] { return server.requests > after; });
        server.mode = 2;
        CHECK(Sync(handle.value, wire) == CLIENT_COMMUNICATION_ERROR);
        Eventually([&] { return losses == 1; });
        CHECK(callbacks == 0);
        CHECK(Sync(handle.value, wire) == CLIENT_NOT_CONNECTED);
        std::cout << "L2 client allocation rollback, error responses, malformed frames and async retirement passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        failAllocation = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
