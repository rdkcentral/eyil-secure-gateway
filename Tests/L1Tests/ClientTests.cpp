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
#include "../support/Faults.h"
#include "../../src/common/RuntimeSupport.h"
#include <eyilclient/EyilClient.h>
#include <rpc.pb.h>
#include <future>
#include <vector>
#include <climits>
using namespace EyilTest;

namespace Injection
{
    std::atomic<int> sendError { 0 }, sendFailures { 0 }, setupError { 0 }, setupPoint { 0 };
    std::atomic<unsigned> attempts { 0 };
    std::atomic<int> ownerAllocation { -1 }, monitorStatus { 0 };
    std::atomic<bool> decodeMemory { false }, responseMemory { false }, advanceAfterSend { false };
    thread_local bool responseMalloc = false;

    int Type(void *s)
    {
        int t = -1;
        size_t n = sizeof(t);
        zmq_getsockopt(s, ZMQ_TYPE, &t, &n);
        return t;
    }

    bool Setup(int point)
    {
        if (setupPoint == point)
        {
            errno = setupError;
            return true;
        }
        return false;
    }
}

extern "C"
{
    int __real_zmq_send(void *, const void *, size_t, int);

    int __wrap_zmq_send(void *s, const void *b, size_t n, int f)
    {
        if (Injection::Type(s) == ZMQ_DEALER)
        {
            ++Injection::attempts;
            int count = Injection::sendFailures.load();
            if (count && (count < 0 || Injection::sendFailures.fetch_sub(1) > 0))
            {
                errno = Injection::sendError;
                return -1;
            }
            int result = __real_zmq_send(s, b, n, f);
            if (result >= 0 && Injection::advanceAfterSend.exchange(false))
                clockAdvanceMs = 16000;
            return result;
        }
        return __real_zmq_send(s, b, n, f);
    }

    int __real_zmq_msg_recv(zmq_msg_t *, void *, int);

    int __wrap_zmq_msg_recv(zmq_msg_t *m, void *s, int f)
    {
        int result = __real_zmq_msg_recv(m, s, f);
        if (result == 6 && Injection::Type(s) == ZMQ_PAIR && Injection::monitorStatus.load())
        {
            // Exercise client mapping for a status actually exposed by a monitor;
            // a real server-side ZAP status need not propagate to the client.
            uint16_t event = ZMQ_EVENT_HANDSHAKE_FAILED_AUTH;
            int32_t status = Injection::monitorStatus.exchange(0);
            memcpy(zmq_msg_data(m), &event, 2);
            memcpy(static_cast<char *>(zmq_msg_data(m)) + 2, &status, 4);
        }
        if (result >= 0 && Injection::Type(s) == ZMQ_DEALER)
        {
            if (Injection::decodeMemory.exchange(false))
                allocationCountdown = 0;
            if (Injection::responseMemory.exchange(false))
                Injection::responseMalloc = true;
        }
        return result;
    }

    void *__real_malloc(size_t);

    void *__wrap_malloc(size_t size)
    {
        if (Injection::responseMalloc)
        {
            Injection::responseMalloc = false;
            errno = ENOMEM;
            return nullptr;
        }
        return __real_malloc(size);
    }

    int __real_zmq_poll(zmq_pollitem_t *, int, long);

    int __wrap_zmq_poll(zmq_pollitem_t *items, int count, long timeout)
    {
        int result = __real_zmq_poll(items, count, timeout);
        if (count == 3 && Injection::Type(items[0].socket) == ZMQ_DEALER)
        {
            const int index = Injection::ownerAllocation.exchange(-1);
            if (index >= 0)
                allocationCountdown = index;
        }
        return result;
    }

    void *__real_zmq_socket(void *, int);

    void *__wrap_zmq_socket(void *c, int t)
    {
        if (t == ZMQ_DEALER && Injection::Setup(1))
            return nullptr;
        return __real_zmq_socket(c, t);
    }

    int __real_zmq_socket_monitor(void *, const char *, int);

    int __wrap_zmq_socket_monitor(void *s, const char *e, int events)
    {
        if (e && Injection::Setup(2))
            return -1;
        return __real_zmq_socket_monitor(s, e, events);
    }

    int __real_zmq_connect(void *, const char *);

    int __wrap_zmq_connect(void *s, const char *e)
    {
        if (Injection::Type(s) == ZMQ_DEALER && Injection::Setup(3))
            return -1;
        return __real_zmq_connect(s, e);
    }
}

namespace
{
    enum class Mode
    {
        Normal,
        WrongService,
        WrongSchema,
        UnsetStatus,
        UnknownStatus,
        ZeroId,
        Malformed,
        Multipart,
        Duplicates,
        Drop,
        TextOk,
        TextPayload
    };

    class Server
    {
        std::thread thread;
        std::exception_ptr failure;
        std::mutex mutex;
        std::vector<RpcRequest> recorded;

    public:
        std::atomic<bool> stop { false }, hold { false };
        std::atomic<Mode> mode { Mode::Normal };
        std::atomic<unsigned> requests { 0 }, replies { 0 }, handshakes { 0 };
        int authStatus;

        explicit Server(int auth = 200)
            : authStatus(auth)
        {
            RequireUnusedEndpoint();
            std::promise<void> startup;
            auto ready = startup.get_future();
            thread = std::thread(
                [this, &startup]
                {
                    bool started = false;
                    try
                    {
                        Eyil::Context context;
                        Eyil::Socket router(context.Get(), ZMQ_ROUTER), zap(context.Get(), ZMQ_REP);
                        router.ConfigureTransport();
                        router.Set(ZMQ_PLAIN_SERVER, 1);
                        zap.Bind("inproc://zeromq.zap.01");
                        router.Bind(Eyil::Endpoint);
                        started = true;
                        startup.set_value();
                        std::vector<std::pair<std::string, RpcRequest>> waiting;
                        while (!stop)
                        {
                            zmq_pollitem_t items[]
                                = { { router.Get(), 0, ZMQ_POLLIN, 0 }, { zap.Get(), 0, ZMQ_POLLIN, 0 } };
                            zmq_poll(items, 2, 10);
                            if (items[1].revents & ZMQ_POLLIN)
                            {
                                std::vector<std::string> frames;
                                do
                                {
                                    Eyil::Message m;
                                    CHECK(m.Receive(zap.Get()));
                                    frames.push_back(m.String());
                                    if (!m.More())
                                        break;
                                } while (true);
                                CHECK(frames.size() == 8);
                                ++handshakes;
                                std::string parts[] = { "1.0", frames[1], std::to_string(authStatus), "unit",
                                    authStatus == 200 ? "user" : "", "" };
                                for (int i = 0; i < 6; ++i)
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
                                {
                                    std::lock_guard<std::mutex> lock(mutex);
                                    recorded.push_back(request);
                                }
                                ++requests;
                                if (mode != Mode::Drop)
                                    waiting.emplace_back(route.String(), std::move(request));
                            }
                            if (hold)
                                continue;
                            for (auto &item : waiting)
                            {
                                const Mode selected = mode;
                                RpcResponse response;
                                response.set_request_id(selected == Mode::ZeroId ? 0 : item.second.request_id());
                                response.set_schema_version(selected == Mode::WrongSchema ? 2 : 1);
                                response.set_service(selected == Mode::WrongService ? static_cast<RpcService>(99)
                                                                                    : item.second.service());
                                response.set_status(selected == Mode::UnsetStatus ? RPC_STATUS_UNSPECIFIED
                                        : selected == Mode::UnknownStatus         ? static_cast<RpcStatus>(99)
                                        : selected == Mode::TextOk                ? RPC_STATUS_OK
                                        : item.second.service() == RPC_SERVICE_UNSPECIFIED
                                        ? RPC_STATUS_UNSUPPORTED_SERVICE
                                        : RPC_STATUS_INTERNAL);
                                if (item.second.service() != RPC_SERVICE_UNSPECIFIED || selected == Mode::TextPayload)
                                    response.set_payload(std::string(64, 'x'));
                                auto send = [&](const std::string &bytes, bool multi = false)
                                {
                                    CHECK(
                                        zmq_send(router.Get(), item.first.data(), item.first.size(), ZMQ_SNDMORE) >= 0);
                                    CHECK(zmq_send(router.Get(), bytes.data(), bytes.size(), multi ? ZMQ_SNDMORE : 0)
                                        >= 0);
                                    if (multi)
                                        CHECK(zmq_send(router.Get(), "extra", 5, 0) >= 0);
                                    ++replies;
                                };
                                if (selected == Mode::Duplicates)
                                {
                                    auto unknown = response;
                                    unknown.set_request_id(UINT64_MAX);
                                    send(unknown.SerializeAsString());
                                }
                                send(
                                    selected == Mode::Malformed ? std::string("\x80", 1) : response.SerializeAsString(),
                                    selected == Mode::Multipart);
                                if (selected == Mode::Duplicates)
                                    send(response.SerializeAsString());
                            }
                            waiting.clear();
                        }
                    }
                    catch (...)
                    {
                        if (!started)
                            startup.set_exception(std::current_exception());
                        else
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            failure = std::current_exception();
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

        ~Server() { Stop(); }

        void Stop()
        {
            stop = true;
            if (thread.joinable())
                thread.join();
        }

        void Check()
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (failure)
                std::rethrow_exception(failure);
        }

        RpcRequest Last()
        {
            std::lock_guard<std::mutex> lock(mutex);
            CHECK(!recorded.empty());
            return recorded.back();
        }
    };

    std::atomic<unsigned> callbacks { 0 }, lost { 0 }, callbackErrors { 0 };
    std::atomic<bool> callbackEntered { false }, blockCallback { false };
    std::mutex callbackMutex;
    std::condition_variable callbackChanged;

    std::string Wire()
    {
        RpcRequest r;
        r.set_request_id(999);
        r.set_schema_version(99);
        r.set_service(RPC_SERVICE_CRYPTO);
        r.set_payload("opaque");
        return r.SerializeAsString();
    }

    void Response(Client *, uint8_t *data, size_t size)
    {
        RpcResponse response;
        if (!data || !response.ParseFromArray(data, size) || response.status() != RPC_STATUS_INTERNAL)
            ++callbackErrors;
        client_free_response(data);
        ++callbacks;
    }

    void TextResponse(Client *, char *data)
    {
        if (!data || std::string(data) != "RPC_STATUS_UNSUPPORTED_SERVICE")
            ++callbackErrors;
        client_free_response(data);
        ++callbacks;
    }

    void Event(Client *, ClientConnectionEvent event)
    {
        if (event != CLIENT_CONNECTION_LOST)
            ++callbackErrors;
        ++lost;
    }

    void Blocking(Client *c, uint8_t *p, size_t n)
    {
        callbackEntered = true;
        std::unique_lock<std::mutex> lock(callbackMutex);
        callbackChanged.wait(lock, [] { return !blockCallback.load(); });
        Response(c, p, n);
    }

    void Throwing(Client *c, uint8_t *p, size_t n)
    {
        Response(c, p, n);
        throw std::runtime_error("application callback");
    }

    void Reentrant(Client *c, uint8_t *p, size_t n)
    {
        auto wire = Wire();
        auto bytes = reinterpret_cast<const uint8_t *>(wire.data());
        uint8_t *out = nullptr;
        size_t size = 0;
        char *text = nullptr;
        if (client_send_request(c, bytes, wire.size(), &out, &size) != CLIENT_INVALID_ARGUMENT || out || size)
            ++callbackErrors;
        if (client_send_request_async(c, bytes, wire.size(), Response) != CLIENT_INVALID_ARGUMENT)
            ++callbackErrors;
        if (client_send_request_string(c, "x", &text) != CLIENT_INVALID_ARGUMENT || text)
            ++callbackErrors;
        if (client_send_request_string_async(c, "x", TextResponse) != CLIENT_INVALID_ARGUMENT)
            ++callbackErrors;
        client_disconnect(c); // rejected on owner; handle must remain usable
        Response(c, p, n);
    }

    struct Fixture
    {
        Server server;
        Client *client = nullptr;

        Fixture()
        {
            callbacks = lost = callbackErrors = 0;
            CHECK(client_connect(&client, Event) == CLIENT_OK);
            CHECK(lost == 0);
        }

        ~Fixture()
        {
            Injection::sendFailures = 0;
            Injection::setupPoint = 0;
            clockAdvanceMs = 0;
            blockCallback = false;
            callbackChanged.notify_all();
            client_disconnect(client);
        }

        ClientResult Call(bool async = false, bool text = false, ClientResponseHandler handler = Response)
        {
            auto wire = Wire();
            if (text)
            {
                if (async)
                    return client_send_request_string_async(client, "plain text", TextResponse);
                char *output = reinterpret_cast<char *>(1);
                auto result = client_send_request_string(client, "plain text", &output);
                if (result == CLIENT_OK)
                {
                    CHECK(output && std::string(output) == "RPC_STATUS_UNSUPPORTED_SERVICE");
                    client_free_response(output);
                }
                else
                    CHECK(!output);
                return result;
            }
            auto bytes = reinterpret_cast<const uint8_t *>(wire.data());
            if (async)
                return client_send_request_async(client, bytes, wire.size(), handler);
            uint8_t *output = reinterpret_cast<uint8_t *>(1);
            size_t size = 123;
            auto result = client_send_request(client, bytes, wire.size(), &output, &size);
            if (result == CLIENT_OK)
            {
                RpcResponse response;
                CHECK(output && response.ParseFromArray(output, size) && response.status() == RPC_STATUS_INTERNAL);
                client_free_response(output);
            }
            else
                CHECK(!output && size == 0);
            return result;
        }
    };

    void Arguments()
    {
        Fixture f;
        auto wire = Wire();
        auto bytes = reinterpret_cast<const uint8_t *>(wire.data());
        for (auto client : { static_cast<Client *>(nullptr), reinterpret_cast<Client *>(1) })
        {
            uint8_t *p = reinterpret_cast<uint8_t *>(1);
            size_t n = 1;
            CHECK(client_send_request(client, bytes, wire.size(), &p, &n) == CLIENT_INVALID_ARGUMENT);
            CHECK(!p && !n);
        }
        for (auto input : { std::string(), std::string("\x80", 1), RpcRequest().SerializeAsString() })
        {
            uint8_t *p = nullptr;
            size_t n = 0;
            CHECK(client_send_request(f.client, reinterpret_cast<const uint8_t *>(input.data()), input.size(), &p, &n)
                == CLIENT_INVALID_ARGUMENT);
        }
        uint8_t *p = reinterpret_cast<uint8_t *>(1);
        size_t n = 1;
        CHECK(client_send_request(f.client, bytes, wire.size(), &p, nullptr) == CLIENT_INVALID_ARGUMENT && !p);
        CHECK(client_send_request(f.client, bytes, wire.size(), nullptr, &n) == CLIENT_INVALID_ARGUMENT && !n);
        CHECK(client_send_request(f.client, bytes, size_t(INT_MAX) + 1, &p, &n) == CLIENT_INVALID_ARGUMENT && !p && !n);
        CHECK(client_send_request(f.client, nullptr, 1, &p, &n) == CLIENT_INVALID_ARGUMENT);
        CHECK(client_send_request_async(f.client, bytes, wire.size(), nullptr) == CLIENT_INVALID_ARGUMENT);
        CHECK(client_send_request_string_async(f.client, "x", nullptr) == CLIENT_INVALID_ARGUMENT);
        char *s = reinterpret_cast<char *>(1);
        CHECK(client_send_request_string(f.client, nullptr, &s) == CLIENT_INVALID_ARGUMENT && !s);
        CHECK(client_send_request_string(f.client, "x", nullptr) == CLIENT_INVALID_ARGUMENT);
        auto original = f.client;
        CHECK(client_connect(&f.client, Event) == CLIENT_ALREADY_CONNECTED && f.client == original);
        CHECK(f.Call() == CLIENT_OK);
        auto observed = f.server.Last();
        CHECK(observed.request_id() != 999 && observed.request_id() != 0 && observed.schema_version() == 1
            && observed.payload() == "opaque");
        CHECK(f.server.requests == 1 && callbacks == 0 && lost == 0);
        CHECK(client_register_notifications(f.client, nullptr) == CLIENT_NOT_IMPLEMENTED);
        client_unregister_notifications(f.client);
    }

    void Text()
    {
        Fixture f;
        for (auto input : { "", "hello", "a\0ignored", "zażółć" })
        {
            char *p = nullptr;
            CHECK(client_send_request_string(f.client, input, &p) == CLIENT_OK);
            CHECK(std::string(p) == "RPC_STATUS_UNSUPPORTED_SERVICE");
            client_free_response(p);
            CHECK(f.server.Last().payload() == input);
        }
        CHECK(f.Call(true, true) == CLIENT_OK);
        Eventually([] { return callbacks == 1; });
        CHECK(callbackErrors == 0);
        for (auto mode : { Mode::TextOk, Mode::TextPayload })
        {
            f.server.mode = mode;
            CHECK(f.Call(false, true) == CLIENT_COMMUNICATION_ERROR);
        }
    }

    void BadResponse(Mode mode, bool connection)
    {
        Fixture f;
        f.server.mode = mode;
        CHECK(f.Call() == CLIENT_COMMUNICATION_ERROR);
        if (connection)
        {
            Eventually([] { return lost == 1; });
            CHECK(f.Call() == CLIENT_NOT_CONNECTED);
        }
        else
        {
            CHECK(lost == 0);
            f.server.mode = Mode::Normal;
            CHECK(f.Call() == CLIENT_OK);
        }
        f.server.Check();
    }

    void AsyncRetirement()
    {
        Fixture f;
        f.server.mode = Mode::WrongService;
        CHECK(f.Call(true) == CLIENT_OK);
        Eventually([&] { return f.server.replies == 1; });
        f.server.mode = Mode::Normal;
        CHECK(f.Call() == CLIENT_OK);
        CHECK(callbacks == 0 && lost == 0);
        CHECK(f.Call(true) == CLIENT_OK);
        Eventually([] { return callbacks == 1; });
        CHECK(callbackErrors == 0);
    }

    void Duplicates()
    {
        Fixture f;
        f.server.mode = Mode::Duplicates;
        CHECK(f.Call(true) == CLIENT_OK);
        Eventually([] { return callbacks == 1; });
        CHECK(f.Call() == CLIENT_OK);
        CHECK(callbacks == 1 && lost == 0 && callbackErrors == 0);
    }

    void SendFailure(int error, ClientResult expected)
    {
        Fixture f;
        for (bool text : { false, true })
            for (bool async : { false, true })
            {
                Injection::sendError = error;
                Injection::sendFailures = 1;
                CHECK(f.Call(async, text) == expected);
            }
        CHECK(f.server.requests == 0 && callbacks == 0);
        CHECK(f.Call() == CLIENT_OK);
        CHECK(f.server.requests == 1);
    }

    void Backpressure(bool expire)
    {
        Fixture f;
        Injection::attempts = 0;
        Injection::sendError = EAGAIN;
        Injection::sendFailures = -1;
        auto submit = std::async(std::launch::async, [&] { return f.Call(true); });
        Eventually([] { return Injection::attempts > 5; });
        CHECK(submit.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        CHECK(f.server.requests == 0 && callbacks == 0);
        if (expire)
            clockAdvanceMs = 16000;
        else
            Injection::sendFailures = 0;
        auto result = submit.get();
        clockAdvanceMs = 0;
        Injection::sendFailures = 0;
        CHECK(result == (expire ? CLIENT_TIMEOUT : CLIENT_OK));
        CHECK(f.Call() == CLIENT_OK);
        CHECK(f.server.requests == (expire ? 1u : 2u));
        CHECK(callbacks == (expire ? 0u : 1u));
    }

    void Interrupted()
    {
        Fixture f;
        Injection::sendError = EINTR;
        Injection::sendFailures = 3;
        CHECK(f.Call(true) == CLIENT_OK);
        Eventually([] { return callbacks == 1; });
        CHECK(f.server.requests == 1);
    }

    void AcceptedAtDeadline()
    {
        Fixture f;
        Injection::advanceAfterSend = true;
        CHECK(f.Call(true) == CLIENT_OK);
        clockAdvanceMs = 0;
        Eventually([] { return callbacks == 1; });
        CHECK(callbackErrors == 0);
    }

    void ResponseMemory(bool async, bool decode)
    {
        Fixture f;
        Injection::decodeMemory = decode;
        Injection::responseMemory = !decode;
        CHECK(f.Call(async) == (async ? CLIENT_OK : CLIENT_OUT_OF_MEMORY));
        if (decode)
        {
            Eventually([] { return lost == 1; });
            CHECK(f.Call() == CLIENT_NOT_CONNECTED);
        }
        else
        {
            CHECK(f.Call() == CLIENT_OK);
            CHECK(lost == 0);
        }
        CHECK(callbacks == 0);
    }

    void InputMemory()
    {
        Fixture f;
        for (bool text : { false, true })
            for (bool async : { false, true })
            {
                bool success = false;
                unsigned failed = 0;
                const auto wire = Wire();
                for (int index = 0; index < 80; ++index)
                {
                    ClientResult result;
                    uint8_t *p = nullptr;
                    char *t = nullptr;
                    size_t n = 0;
                    const auto before = f.server.requests.load();
                    const auto callbacksBefore = callbacks.load();
                    {
                        FailAllocation fault(index);
                        if (text)
                            result = async ? client_send_request_string_async(f.client, "text", TextResponse)
                                           : client_send_request_string(f.client, "text", &t);
                        else
                            result = async ? client_send_request_async(f.client,
                                                 reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), Response)
                                           : client_send_request(f.client,
                                                 reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), &p, &n);
                    }
                    if (result == CLIENT_OK)
                    {
                        client_free_response(p);
                        client_free_response(t);
                        if (async)
                            Eventually([&] { return callbacks == callbacksBefore + 1; });
                        success = true;
                        break;
                    }
                    CHECK(result == CLIENT_OUT_OF_MEMORY && !p && !t && !n);
                    ++failed;
                    CHECK(f.server.requests == before);
                }
                CHECK(success && failed > 0);
            }
        CHECK(f.Call() == CLIENT_OK);
        Eventually([] { return callbacks == 2; });
        CHECK(callbackErrors == 0);
    }

    void LocalClose()
    {
        Fixture f;
        f.server.mode = Mode::Drop;
        CHECK(f.Call(true) == CLIENT_OK);
        Eventually([&] { return f.server.requests == 1; });
        auto sync = std::async(std::launch::async, [&] { return f.Call(); });
        Eventually([&] { return f.server.requests == 2; });
        client_disconnect(f.client);
        f.client = nullptr;
        CHECK(sync.get() == CLIENT_NOT_CONNECTED);
        CHECK(callbacks == 0 && lost == 0);
    }

    void Loss()
    {
        Fixture f;
        f.server.mode = Mode::Drop;
        CHECK(f.Call(true) == CLIENT_OK);
        auto sync = std::async(std::launch::async, [&] { return f.Call(); });
        Eventually([&] { return f.server.requests == 2; });
        f.server.Stop();
        CHECK(sync.get() == CLIENT_COMMUNICATION_ERROR);
        Eventually([] { return lost == 1; });
        CHECK(callbacks == 0 && f.Call() == CLIENT_NOT_CONNECTED);
    }

    void Callbacks()
    {
        Fixture f;
        CHECK(f.Call(true, false, Reentrant) == CLIENT_OK);
        Eventually([] { return callbacks == 1; });
        CHECK(f.Call() == CLIENT_OK);
        CHECK(f.Call(true, false, Throwing) == CLIENT_OK);
        Eventually([] { return callbacks == 2; });
        CHECK(f.Call() == CLIENT_OK);
        CHECK(callbackErrors == 0 && lost == 0);
    }

    void CallbackClose()
    {
        Fixture f;
        blockCallback = true;
        callbackEntered = false;
        CHECK(f.Call(true, false, Blocking) == CLIENT_OK);
        Eventually([] { return callbackEntered.load(); });
        auto closing = std::async(std::launch::async, [&] { client_disconnect(f.client); });
        auto waiting = closing.wait_for(std::chrono::milliseconds(100));
        blockCallback = false;
        callbackChanged.notify_all();
        closing.get();
        f.client = nullptr;
        CHECK(waiting == std::future_status::timeout && callbacks == 1 && lost == 0);
    }

    void SyncTimeout()
    {
        Fixture f;
        f.server.hold = true;
        auto sync = std::async(std::launch::async, [&] { return f.Call(); });
        Eventually([&] { return f.server.requests == 1; });
        clockAdvanceMs = 16000;
        auto result = sync.get();
        clockAdvanceMs = 0;
        CHECK(result == CLIENT_TIMEOUT);
        f.server.hold = false;
        CHECK(f.Call() == CLIENT_OK);
        CHECK(f.server.requests == 2 && callbacks == 0 && lost == 0);
    }

    void OwnerMemory()
    {
        // A fresh pending map is required for each fault position: a recovered
        // map may retain buckets and need fewer allocations on the next call.
        for (int index = 0; index < 3; ++index)
        {
            Fixture f;
            RpcRequest request;
            request.set_service(RPC_SERVICE_CRYPTO);
            request.set_payload(std::string(1024, 'x'));
            const auto wire = request.SerializeAsString();
            Injection::ownerAllocation = index;
            CHECK(client_send_request_async(
                      f.client, reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), Response)
                == CLIENT_OUT_OF_MEMORY);
            CHECK(f.server.requests == 0 && callbacks == 0);
            CHECK(f.Call() == CLIENT_OK && f.server.requests == 1);
        }
    }

    void CloseBeforeSend()
    {
        Fixture f;
        Injection::attempts = 0;
        Injection::sendError = EAGAIN;
        Injection::sendFailures = -1;
        auto first = std::async(std::launch::async, [&] { return f.Call(true); });
        Eventually([] { return Injection::attempts > 5; });
        // The second caller waits for the single handoff slot, not a queue.
        std::promise<void> begun;
        auto started = begun.get_future();
        auto second = std::async(std::launch::async,
            [&]
            {
                begun.set_value();
                return f.Call(true);
            });
        started.get();
        CHECK(second.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
        client_disconnect(f.client);
        const auto a = first.get(), b = second.get();
        f.client = nullptr;
        Injection::sendFailures = 0;
        CHECK(a == CLIENT_NOT_CONNECTED && b == CLIENT_NOT_CONNECTED);
        CHECK(f.server.requests == 0 && callbacks == 0 && lost == 0);
    }

    void AsyncNoDeadline()
    {
        Fixture f;
        f.server.hold = true;
        // Accepted async work survives arbitrary passage of its former send deadline.
        CHECK(f.Call(true) == CLIENT_OK);
        Eventually([&] { return f.server.requests == 1; });
        clockAdvanceMs = 60000;
        f.server.hold = false;
        Eventually([] { return callbacks == 1; });
        clockAdvanceMs = 0;
        CHECK(callbackErrors == 0 && lost == 0);
    }

    void ConnectMemory()
    {
        Server server;
        unsigned failed = 0;
        bool success = false;
        for (int index = 0; index < 20; ++index)
        {
            Client *client = nullptr;
            ClientResult result;
            {
                FailAllocation failure(index);
                result = client_connect(&client, Event);
            }
            if (result == CLIENT_OK)
            {
                client_disconnect(client);
                success = true;
                break;
            }
            CHECK(result == CLIENT_OUT_OF_MEMORY && !client);
            ++failed;
        }
        CHECK(failed >= 3 && success);
    }

    void TemporaryAuth()
    {
        lost = 0;
        Server server(300);
        Client *client = nullptr;
        const auto result = client_connect(&client, Event);
        // ZAP 300 is a server-side failure. Depending on libzmq, the client
        // observes disconnect, or its readiness deadline expires with no event.
        CHECK(result == CLIENT_COMMUNICATION_ERROR || result == CLIENT_TIMEOUT);
        CHECK(!client && lost == 0);
        server.Check();
    }

    void Auth(int status, ClientResult expected)
    {
        lost = 0;
        Server server(status);
        Client *client = nullptr;
        CHECK(client_connect(&client, Event) == expected);
        CHECK(!client && lost == 0);
        server.Check();
    }

    void SetupFailure(int point, int error)
    {
        Injection::setupPoint = point;
        Injection::setupError = error;
        Client *client = nullptr;
        lost = 0;
        auto result = client_connect(&client, Event);
        Injection::setupPoint = 0;
        CHECK(result == (error == ENOMEM ? CLIENT_OUT_OF_MEMORY : CLIENT_COMMUNICATION_ERROR));
        CHECK(!client && lost == 0);
    }
}

int main(int argc, char **argv)
{
    return RunCases(argc, argv,
        { { "arguments", Arguments }, { "raw-text", Text },
            { "wrong-service", [] { BadResponse(Mode::WrongService, false); } },
            { "wrong-schema", [] { BadResponse(Mode::WrongSchema, false); } },
            { "unset-status", [] { BadResponse(Mode::UnsetStatus, false); } },
            { "unknown-status", [] { BadResponse(Mode::UnknownStatus, false); } },
            { "zero-id", [] { BadResponse(Mode::ZeroId, true); } },
            { "malformed", [] { BadResponse(Mode::Malformed, true); } },
            { "multipart", [] { BadResponse(Mode::Multipart, true); } }, { "async-retirement", AsyncRetirement },
            { "duplicate-unknown-id", Duplicates },
            { "send-memory", [] { SendFailure(ENOMEM, CLIENT_OUT_OF_MEMORY); } },
            { "send-error", [] { SendFailure(EPIPE, CLIENT_COMMUNICATION_ERROR); } },
            { "send-eagain", [] { Backpressure(false); } }, { "send-timeout", [] { Backpressure(true); } },
            { "send-eintr", Interrupted }, { "send-acceptance-race", AcceptedAtDeadline },
            { "sync-response-memory", [] { ResponseMemory(false, false); } },
            { "async-response-memory", [] { ResponseMemory(true, false); } },
            { "sync-decode-memory", [] { ResponseMemory(false, true); } },
            { "async-decode-memory", [] { ResponseMemory(true, true); } }, { "input-memory", InputMemory },
            { "explicit-close", LocalClose }, { "transport-loss", Loss }, { "callback-rules", Callbacks },
            { "callback-quiescence", CallbackClose }, { "sync-timeout-late-reply", SyncTimeout },
            { "auth-denied", [] { Auth(400, CLIENT_AUTH_FAILED); } }, { "auth-temporary", TemporaryAuth },
            { "auth-internal", [] { Auth(500, CLIENT_COMMUNICATION_ERROR); } },
            { "socket-memory", [] { SetupFailure(1, ENOMEM); } }, { "socket-error", [] { SetupFailure(1, EMFILE); } },
            { "monitor-memory", [] { SetupFailure(2, ENOMEM); } }, { "monitor-error", [] { SetupFailure(2, EINVAL); } },
            { "connect-memory", [] { SetupFailure(3, ENOMEM); } }, { "connect-error", [] { SetupFailure(3, EINVAL); } },
            { "owner-memory", OwnerMemory }, { "close-before-send", CloseBeforeSend },
            { "async-no-deadline", AsyncNoDeadline }, { "connect-allocation", ConnectMemory },
            { "monitor-auth-300", []
                {
                    Injection::monitorStatus = 300;
                    Auth(200, CLIENT_COMMUNICATION_ERROR);
                } } });
}
