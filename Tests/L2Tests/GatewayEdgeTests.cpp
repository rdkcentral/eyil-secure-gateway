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
#include "../support/MockCrypto.h"
#include "GatewayRuntime.h"
#include <future>
#include <memory>
using namespace EyilTest;

namespace ReplyFault
{
    std::atomic<int> error { 0 };
    std::atomic<bool> body { false };
    std::atomic<unsigned> failedAttempts { 0 };
    std::atomic<unsigned> sendAttempts { 0 };
    std::atomic<bool> countSends { false };
}

namespace ResponseFault
{
    std::atomic<uint64_t> request { 0 };
    std::atomic<uint64_t> parsed { 0 };
    std::atomic<unsigned> parsedCount { 0 };
    std::atomic<uint64_t> admissionAllocation { 0 };
    std::atomic<unsigned> attempts { 0 };
    std::atomic<int> failures { 0 };
    std::atomic<bool> returnFalse { false };
}

namespace PollObservation
{
    std::atomic<unsigned> paused { 0 };
    std::array<std::atomic<unsigned>, 3> calls {};
    std::array<std::atomic<long>, 3> timeouts {};
    std::atomic<int> routerWake { -1 };

    void Advance(long long milliseconds)
    {
        clockAdvanceMs = milliseconds;
        const uint64_t one = 1;
        CHECK(write(routerWake.load(), &one, sizeof(one)) == sizeof(one));
    }
}

extern "C"
{
    int __real_zmq_poll(zmq_pollitem_t *, int, long);

    int __wrap_zmq_poll(zmq_pollitem_t *items, int count, long timeout)
    {
        if (count == 2 && items[0].socket)
        {
            int type = -1;
            size_t size = sizeof(type);
            if (!zmq_getsockopt(items[0].socket, ZMQ_TYPE, &type, &size))
            {
                const int index = type == ZMQ_ROUTER ? 0 : type == ZMQ_REP ? 1 : type == ZMQ_PAIR ? 2 : -1;
                if (index >= 0)
                {
                    PollObservation::timeouts[index] = timeout;
                    ++PollObservation::calls[index];
                }
                if (type == ZMQ_ROUTER)
                {
                    PollObservation::routerWake = items[1].fd;
                    if (!(items[0].events & ZMQ_POLLIN))
                        ++PollObservation::paused;
                }
            }
        }
        return __real_zmq_poll(items, count, timeout);
    }

    int __real_zmq_send(void *, const void *, size_t, int);

    int __wrap_zmq_send(void *socket, const void *data, size_t size, int flags)
    {
        int type = -1;
        size_t n = sizeof(type);
        zmq_getsockopt(socket, ZMQ_TYPE, &type, &n);
        if (type == ZMQ_ROUTER && ReplyFault::countSends)
            ++ReplyFault::sendAttempts;
        if (type == ZMQ_ROUTER && ReplyFault::error.load()
            && static_cast<bool>(flags & ZMQ_SNDMORE) != ReplyFault::body.load())
        {
            errno = ReplyFault::error.exchange(0);
            ++ReplyFault::failedAttempts;
            return -1;
        }
        return __real_zmq_send(socket, data, size, flags);
    }

    // GNU ld wrappers affect the runtime's calls into Protobuf, not its public API.
    bool
    __real__ZNK6google8protobuf11MessageLite17SerializeToStringEPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(
        const google::protobuf::MessageLite *, std::string *);

    bool
    __wrap__ZNK6google8protobuf11MessageLite17SerializeToStringEPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(
        const google::protobuf::MessageLite *message, std::string *output)
    {
        const auto *response = dynamic_cast<const RpcResponse *>(message);
        if (response && ResponseFault::request && response->request_id() == ResponseFault::request)
        {
            ++ResponseFault::attempts;
            if (ResponseFault::failures > 0)
            {
                --ResponseFault::failures;
                // A failed serializer may already have modified its destination.
                output->assign("partial");
                if (ResponseFault::returnFalse)
                    return false;
                throw std::bad_alloc();
            }
        }
        return __real__ZNK6google8protobuf11MessageLite17SerializeToStringEPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(
            message, output);
    }

    bool __real__ZN6google8protobuf11MessageLite14ParseFromArrayEPKvi(
        google::protobuf::MessageLite *, const void *, int);

    bool __wrap__ZN6google8protobuf11MessageLite14ParseFromArrayEPKvi(
        google::protobuf::MessageLite *message, const void *data, int size)
    {
        const bool result = __real__ZN6google8protobuf11MessageLite14ParseFromArrayEPKvi(message, data, size);
        const auto *request = dynamic_cast<const RpcRequest *>(message);
        if (result && request)
        {
            ++ResponseFault::parsedCount;
            auto target = ResponseFault::admissionAllocation.load();
            if (target && request->request_id() == target
                && ResponseFault::admissionAllocation.compare_exchange_strong(target, 0))
                EyilTest::allocationCountdown = 0; // First request-ID set insertion on the I/O owner.
            ResponseFault::parsed = request->request_id();
        }
        return result;
    }
}

namespace
{
    std::mutex logMutex;
    std::vector<std::string> logs;

    void Capture(const char *text)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        logs.emplace_back(text);
    }

    std::string FindLog(const std::string &text)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        for (const auto &line : logs)
            if (line.find(text) != std::string::npos)
                return line;
        return {};
    }

    struct Fixture
    {
        MockCrypto crypto;
        Eyil::GatewayRuntime gateway;
        Eyil::Context context;

        explicit Fixture(unsigned workers = 4)
            : gateway([this] { return crypto.Acquire(); }, workers, Eyil::Endpoint, Capture)
        {
            {
                std::lock_guard<std::mutex> lock(logMutex);
                logs.clear();
            }
            PollObservation::paused = 0;
            for (size_t i = 0; i < 3; ++i)
            {
                PollObservation::calls[i] = 0;
                PollObservation::timeouts[i] = 0;
            }
            ReplyFault::error = 0;
            ReplyFault::countSends = false;
            ResponseFault::request = 0;
            ResponseFault::parsed = 0;
            ResponseFault::parsedCount = 0;
            ResponseFault::admissionAllocation = 0;
            ResponseFault::attempts = 0;
            ResponseFault::failures = 0;
            ResponseFault::returnFalse = false;
            clockAdvanceMs = 0;
            RequireUnusedEndpoint();
            CHECK(gateway.Start());
        }

        ~Fixture()
        {
            crypto.Unblock();
            gateway.StopAndWait();
            clockAdvanceMs = 0;
        }
    };

    struct Peer
    {
        Eyil::Socket socket;

        explicit Peer(Fixture &f, const char *id = "")
            : socket(f.context.Get(), ZMQ_DEALER)
        {
            socket.ConfigureTransport();
            socket.SetString(ZMQ_PLAIN_USERNAME, "dummy");
            socket.SetString(ZMQ_PLAIN_PASSWORD, "dummy");
            socket.Set(ZMQ_RCVTIMEO, 3000);
            socket.Set(ZMQ_SNDTIMEO, 3000);
            socket.Set(ZMQ_RECONNECT_IVL, -1);
            if (*id)
                socket.SetString(ZMQ_ROUTING_ID, id);
            socket.Connect(Eyil::Endpoint);
        }

        void Send(const std::string &wire) { CHECK(zmq_send(socket.Get(), wire.data(), wire.size(), 0) >= 0); }

        RpcResponse Read()
        {
            Eyil::Message m;
            CHECK(m.Receive(socket.Get(), 0) && !m.More());
            RpcResponse result;
            CHECK(result.ParseFromArray(m.Data(), m.Size()));
            return result;
        }
    };

    RpcRequest Request(uint64_t id = 1)
    {
        CryptoRequest c;
        c.set_operation(CRYPTO_OPERATION_GENERATE_RANDOM);
        c.mutable_generate_random()->set_number_of_bytes(16);
        RpcRequest r;
        r.set_request_id(id);
        r.set_schema_version(1);
        r.set_service(RPC_SERVICE_CRYPTO);
        r.set_payload(c.SerializeAsString());
        return r;
    }

    void Envelopes()
    {
        Fixture f;
        Peer p(f);
        uint64_t id = 1;
        for (uint32_t schema : { 0u, 2u, UINT32_MAX })
        {
            auto r = Request(id++);
            r.set_schema_version(schema);
            p.Send(r.SerializeAsString());
            auto response = p.Read();
            CHECK(response.request_id() == r.request_id() && response.status() == RPC_STATUS_UNSUPPORTED_SCHEMA);
        }
        for (int service : { 0, 2, 99, -1 })
        {
            auto r = Request(id++);
            r.set_service(static_cast<RpcService>(service));
            p.Send(r.SerializeAsString());
            auto response = p.Read();
            CHECK(response.request_id() == r.request_id() && response.status() == RPC_STATUS_UNSUPPORTED_SERVICE);
        }
        auto r = Request(id++);
        r.set_payload("bad proto");
        p.Send(r.SerializeAsString());
        CHECK(p.Read().status() == RPC_STATUS_INTERNAL);
        CHECK(f.crypto.entered == 0);
        r = Request(id);
        p.Send(r.SerializeAsString());
        CHECK(p.Read().status() == RPC_STATUS_OK);
        CHECK(f.crypto.entered == 1);
    }

    void Framing()
    {
        Fixture f;
        Peer p(f);
        auto valid = Request();
        for (int i = 0; i < 20; ++i)
        {
            p.Send(std::string("\x80", 1));
            auto zero = Request(0);
            p.Send(zero.SerializeAsString());
            auto bytes = valid.SerializeAsString();
            CHECK(zmq_send(p.socket.Get(), bytes.data(), bytes.size(), ZMQ_SNDMORE) >= 0);
            CHECK(zmq_send(p.socket.Get(), "", 0, ZMQ_SNDMORE) >= 0);
            CHECK(zmq_send(p.socket.Get(), "extra", 5, 0) >= 0);
        }
        p.Send(valid.SerializeAsString());
        auto response = p.Read();
        CHECK(response.request_id() == 1 && response.status() == RPC_STATUS_OK);
        CHECK(f.crypto.entered == 1 && f.gateway.Log().Total(Eyil::Reason::Protocol) >= 60);
    }

    void DuplicateInFlight()
    {
        Fixture f;
        Peer p(f);
        f.crypto.block = true;

        struct Unblock
        {
            MockCrypto &c;

            ~Unblock() { c.Unblock(); }
        } unblock { f.crypto };

        const auto wire = Request(77).SerializeAsString();
        p.Send(wire);
        Eventually([&] { return f.crypto.entered == 1; });
        for (unsigned variant = 0; variant < 3; ++variant)
        {
            auto duplicate = Request(77);
            if (variant == 1)
                duplicate.set_schema_version(99);
            if (variant == 2)
                duplicate.set_service(RPC_SERVICE_UNSPECIFIED);
            const auto rejected = f.gateway.Log().Total(Eyil::Reason::Protocol);
            p.Send(duplicate.SerializeAsString());
            Eventually([&] { return f.gateway.Log().Total(Eyil::Reason::Protocol) > rejected; });
            zmq_pollitem_t item { p.socket.Get(), 0, ZMQ_POLLIN, 0 };
            CHECK(zmq_poll(&item, 1, 0) == 0);
            CHECK(f.crypto.entered == 1);
        }

        // The same ID on another authenticated connection remains independent.
        Peer other(f);
        other.Send(wire);
        Eventually([&] { return f.crypto.entered == 2; });
        f.crypto.Unblock();
        auto original = p.Read();
        CHECK(original.request_id() == 77 && original.status() == RPC_STATUS_OK);
        auto independent = other.Read();
        CHECK(independent.request_id() == 77 && independent.status() == RPC_STATUS_OK && f.crypto.entered == 2);
        p.Send(Request(78).SerializeAsString());
        CHECK(p.Read().request_id() == 78);
        zmq_pollitem_t item { p.socket.Get(), 0, ZMQ_POLLIN, 0 };
        CHECK(zmq_poll(&item, 1, 50) == 0);
    }

    void DisconnectBeforeRequest()
    {
        Fixture f;
        auto a = std::make_unique<Peer>(f), b = std::make_unique<Peer>(f);
        Eventually([&] { return f.crypto.SessionCount() == 2; });
        a.reset();
        Eventually([&] { return f.crypto.SessionCount() == 1; });
        b->Send(Request().SerializeAsString());
        CHECK(b->Read().status() == RPC_STATUS_OK);
        b.reset();
        Eventually([&] { return f.crypto.SessionCount() == 0; });
        CHECK(f.crypto.references == 1);
    }

    void DrainReplies(bool waiting, bool expire)
    {
        Fixture f(waiting ? 1 : 4);
        Peer p(f);
        f.crypto.block = true;
        const unsigned running = waiting ? 1 : 4;
        for (unsigned id = 1; id <= running; ++id)
            p.Send(Request(id).SerializeAsString());
        Eventually([&] { return f.crypto.entered == running; });
        if (waiting)
        {
            const auto paused = PollObservation::paused.load();
            p.Send(Request(2).SerializeAsString());
            Eventually([&] { return ResponseFault::parsed == 2 && PollObservation::paused > paused; });
        }

        auto stopped = std::async(std::launch::async, [&] { f.gateway.StopAndWait(); });

        struct Finish
        {
            Fixture &fixture;
            std::future<void> &stopped;

            ~Finish()
            {
                fixture.crypto.Unblock();
                if (stopped.valid())
                    stopped.wait();
            }
        } finish { f, stopped };

        CHECK(stopped.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
        Eventually([&] { return !FindLog("reason=cleanup_wait waiting_on=accepted_work").empty(); });
        const auto warning = FindLog("reason=cleanup_wait waiting_on=accepted_work");
        CHECK(warning.find(" WARNING ") != std::string::npos);
        CHECK(warning.find("connection=") != std::string::npos && warning.find("stopping=1") != std::string::npos);
        CHECK(warning.find("active_requests=") != std::string::npos);
        // Unrelated wakeups must neither repeat the warning nor create a timer.
        for (unsigned n = 0; n < 3; ++n)
        {
            const auto polls = PollObservation::calls[0].load();
            PollObservation::Advance(0);
            Eventually([&] { return PollObservation::calls[0] > polls; });
        }
        CHECK(f.gateway.Log().Total(Eyil::Reason::CleanupWaiting) == 1);
        if (expire)
        {
            PollObservation::Advance(10001);
            const auto response = p.Read();
            CHECK(response.request_id() == 2 && response.status() == RPC_STATUS_INTERNAL);
            CHECK(f.crypto.entered == 1);
        }
        f.crypto.Unblock();
        std::set<uint64_t> replies;
        const unsigned expected = running + (waiting && !expire ? 1 : 0);
        for (unsigned n = 0; n < expected; ++n)
        {
            const auto response = p.Read();
            CHECK(response.status() == RPC_STATUS_OK && replies.insert(response.request_id()).second);
        }
        CHECK(replies.count(1) && (!waiting || expire || replies.count(2)));
        CHECK(stopped.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        stopped.get();
        CHECK(f.crypto.SessionCount() == 0 && f.crypto.references == 1 && f.crypto.entered == expected);
    }

    void IdlePoll()
    {
        Fixture f;
        Peer p(f);
        p.Send(Request(1).SerializeAsString());
        CHECK(p.Read().status() == RPC_STATUS_OK);
        Eventually(
            []
            {
                std::array<unsigned, 3> before;
                for (size_t i = 0; i < 3; ++i)
                {
                    if (PollObservation::timeouts[i] != -1 || PollObservation::calls[i] == 0)
                        return false;
                    before[i] = PollObservation::calls[i];
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                for (size_t i = 0; i < 3; ++i)
                    if (PollObservation::calls[i] != before[i])
                        return false;
                return true;
            });
        p.Send(Request(2).SerializeAsString());
        CHECK(p.Read().status() == RPC_STATUS_OK);
        // The fixture stops while all three owners may be in indefinite poll.
    }

    void WorkerLogs()
    {
        Fixture f(1);
        Peer p(f);
        f.crypto.block = true;

        struct Unblock
        {
            MockCrypto &crypto;

            ~Unblock() { crypto.Unblock(); }
        } unblock { f.crypto };

        p.Send(Request(41).SerializeAsString());
        Eventually([&] { return f.crypto.entered == 1; });
        p.Send(Request(42).SerializeAsString());
        Eventually([&] { return f.gateway.Log().Total(Eyil::Reason::WorkerWaiting) == 1; });
        const auto warning = FindLog("reason=worker_pool_wait");
        CHECK(warning.find(" WARNING ") != std::string::npos);
        CHECK(warning.find("workers=1 running=1 reserved=0 lifecycle_running=0") != std::string::npos);
        CHECK(warning.find("request_id=42 service=1") != std::string::npos);
        CHECK(warning.find("connection=") != std::string::npos);
        PollObservation::Advance(25000);
        const auto response = p.Read();
        CHECK(response.request_id() == 42 && response.status() == RPC_STATUS_INTERNAL);
        const auto error = FindLog("reason=worker_pool_exhausted");
        CHECK(error.find(" ERROR ") != std::string::npos);
        CHECK(error.find("running=1 reserved=0") != std::string::npos);
        CHECK(error.find("request_id=42 service=1") != std::string::npos);
        const auto wait = error.find("wait_ms=");
        CHECK(wait != std::string::npos && std::stoll(error.substr(wait + 8)) >= 25000);
        CHECK(f.gateway.Log().Total(Eyil::Reason::WorkerWaiting) == 1);
        f.crypto.Unblock();
        CHECK(p.Read().request_id() == 41);
    }

    void WorkerBurstSaturation()
    {
        constexpr unsigned workers = 4;
        constexpr unsigned peerCount = 4;
        constexpr unsigned burstPerPeer = 250;
        constexpr unsigned timedOut = 16;
        Fixture f(workers);
        std::array<std::unique_ptr<Peer>, peerCount> peers;
        std::array<std::set<uint64_t>, peerCount> received;
        for (auto &peer : peers)
            peer = std::make_unique<Peer>(f);
        // Finish authentication before occupying all lifecycle/request capacity.
        Eventually([&] { return f.crypto.SessionCount() == peerCount; });
        for (auto &peer : peers)
        {
            auto ready = Request(burstPerPeer + 3);
            ready.set_service(RPC_SERVICE_UNSPECIFIED);
            peer->Send(ready.SerializeAsString());
            CHECK(peer->Read().status() == RPC_STATUS_UNSUPPORTED_SERVICE);
        }
        const auto initiallyParsed = ResponseFault::parsedCount.load();

        f.crypto.block = true;
        for (auto &peer : peers)
            peer->Send(Request(1).SerializeAsString());
        Eventually([&] { return f.crypto.entered == workers; });

        // Keep each route below its default HWM. This stresses worker capacity,
        // not dropped replies caused by intentionally saturated output queues.
        for (uint64_t id = 2; id <= burstPerPeer + 1; ++id)
            for (auto &peer : peers)
                peer->Send(Request(id).SerializeAsString());

        std::array<zmq_pollitem_t, peerCount> items {};
        for (unsigned i = 0; i < peerCount; ++i)
            items[i] = { peers[i]->socket.Get(), 0, ZMQ_POLLIN, 0 };
        const auto readResponse = [&](RpcStatus expected)
        {
            CHECK(zmq_poll(items.data(), items.size(), 3000) > 0);
            for (unsigned i = 0; i < peerCount; ++i)
            {
                if (!(items[i].revents & ZMQ_POLLIN))
                    continue;
                const auto response = peers[i]->Read();
                CHECK(response.schema_version() == 1 && response.service() == RPC_SERVICE_CRYPTO);
                CHECK(response.status() == expected);
                CHECK(response.request_id() >= 1 && response.request_id() <= burstPerPeer + 1);
                CHECK(received[i].insert(response.request_id()).second);
                if (expected == RPC_STATUS_INTERNAL)
                {
                    CHECK(response.request_id() != 1); // Running calls must not expire.
                    CHECK(response.payload().empty());
                }
                else
                {
                    CryptoResponse crypto;
                    CHECK(crypto.ParseFromString(response.payload()));
                    CHECK(crypto.error() == CRYPTO_ERROR_NONE && crypto.has_generate_random());
                    CHECK(crypto.generate_random().random_value() == std::string(16, '\x5a'));
                }
                return;
            }
            CHECK(false); // Poll indicated an event but no complete response was readable.
        };

        for (unsigned expired = 0; expired < timedOut; ++expired)
        {
            Eventually([&] { return f.gateway.Log().Total(Eyil::Reason::WorkerWaiting) == expired + 1; });
            CHECK(f.crypto.entered == workers);
            // Exactly one request beyond the running calls has been parsed.
            // The rest remain in libzmq while ROUTER reception is paused.
            CHECK(ResponseFault::parsedCount == initiallyParsed + workers + expired + 1);
            PollObservation::Advance(clockAdvanceMs.load() + 10001);
            readResponse(RPC_STATUS_INTERNAL);
            CHECK(f.gateway.Log().Total(Eyil::Reason::WorkerExhausted) == expired + 1);
            CHECK(f.crypto.entered == workers); // Expired work never reaches Crypto.
        }

        Eventually([&] { return f.gateway.Log().Total(Eyil::Reason::WorkerWaiting) == timedOut + 1; });
        CHECK(ResponseFault::parsedCount == initiallyParsed + workers + timedOut + 1);
        CHECK(PollObservation::paused > 0);
        CHECK(FindLog("reason=worker_pool_wait").find("workers=4 running=4 reserved=0") != std::string::npos);
        CHECK(FindLog("reason=worker_pool_exhausted").find(" ERROR ") != std::string::npos);

        // Do not advance the clock again. Unread requests get their own admission
        // deadline when received, rather than inheriting the first request's age.
        f.crypto.Unblock();
        constexpr unsigned submitted = workers + peerCount * burstPerPeer;
        for (unsigned remaining = 0; remaining < submitted - timedOut; ++remaining)
            readResponse(RPC_STATUS_OK);
        for (const auto &ids : received)
            CHECK(ids.size() == burstPerPeer + 1);
        CHECK(f.crypto.entered == submitted - timedOut);
        CHECK(f.gateway.Log().Total(Eyil::Reason::WorkerExhausted) == timedOut);

        // A fresh operation on every connection proves recovery after the burst.
        for (unsigned i = 0; i < peerCount; ++i)
        {
            peers[i]->Send(Request(burstPerPeer + 2).SerializeAsString());
            const auto reply = peers[i]->Read();
            CHECK(reply.request_id() == burstPerPeer + 2 && reply.status() == RPC_STATUS_OK);
        }
        CHECK(f.crypto.entered == submitted - timedOut + peerCount);
        CHECK(zmq_poll(items.data(), items.size(), 30) == 0); // No late/duplicate completion.
        std::cout << "SATURATION workers=" << workers << " peers=" << peerCount << " burst=" << peerCount * burstPerPeer
                  << " timed_out=" << timedOut << " recovery_requests=" << peerCount << '\n';
    }

    void DropReply(int error, bool body, bool admission = false)
    {
        Fixture f;
        Peer p(f);
        ReplyFault::error = error;
        ReplyFault::body = body;
        ReplyFault::failedAttempts = 0;
        ReplyFault::sendAttempts = 0;
        ReplyFault::countSends = true;
        auto request = Request(1);
        if (admission)
            request.set_service(RPC_SERVICE_UNSPECIFIED);
        p.Send(request.SerializeAsString());
        Eventually([&] { return f.gateway.Log().Total(Eyil::Reason::DroppedReply) == 1; });
        CHECK(ReplyFault::failedAttempts == 1 && f.crypto.entered == (admission ? 0u : 1u));
        if (body)
        {
            // A partial multipart failure retires the unusable ROUTER; it must
            // neither append another response to it nor leak the app session.
            Eventually(
                [&] { return f.gateway.Log().Total(Eyil::Reason::Transport) > 0 && f.crypto.SessionCount() == 0; });
            f.gateway.StopAndWait();
            CHECK(ReplyFault::sendAttempts == 2); // Route and body; no fallback send after a partial reply.
        }
        else
        {
            // No resend of request 1: the same live peer can continue and gets
            // only response 2, even though the first operation already executed.
            p.Send(Request(2).SerializeAsString());
            const auto response = p.Read();
            CHECK(response.request_id() == 2 && response.status() == RPC_STATUS_OK);
            CHECK(f.crypto.entered == 2 && ReplyFault::failedAttempts == 1);
            zmq_pollitem_t item { p.socket.Get(), 0, ZMQ_POLLIN, 0 };
            CHECK(zmq_poll(&item, 1, 50) == 0);
        }
    }

    enum class ResponsePath
    {
        Admission,
        AdmissionFailure,
        Worker,
        WorkerTimeout
    };

    void ResponsePreparationFailure(ResponsePath path, unsigned failures, bool serializationReturnsFalse = false)
    {
        Fixture f;
        Peer p(f);
        Peer other(f);
        Eventually([&] { return f.crypto.SessionCount() == 2; });

        if (path == ResponsePath::WorkerTimeout)
        {
            f.crypto.block = true;
            for (uint64_t id = 1; id <= 4; ++id)
                p.Send(Request(id).SerializeAsString());
            Eventually([&] { return f.crypto.entered == 4; });
        }

        constexpr uint64_t target = 77;
        auto request = Request(target);
        if (path == ResponsePath::Admission)
            request.set_service(RPC_SERVICE_UNSPECIFIED);
        if (path == ResponsePath::AdmissionFailure)
            ResponseFault::admissionAllocation = target;
        ResponseFault::request = target;
        ResponseFault::failures = failures;
        ResponseFault::returnFalse = serializationReturnsFalse;
        p.Send(request.SerializeAsString());

        if (path == ResponsePath::WorkerTimeout)
        {
            Eventually([&] { return ResponseFault::parsed == target; });
            PollObservation::Advance(10001);
        }

        const bool alreadyInternal = path == ResponsePath::AdmissionFailure || path == ResponsePath::WorkerTimeout;
        const bool dropped = alreadyInternal || failures > 1;
        if (dropped)
        {
            Eventually([&] { return f.gateway.Log().Total(Eyil::Reason::DroppedReply) == 1; });
            zmq_pollitem_t item { p.socket.Get(), 0, ZMQ_POLLIN, 0 };
            CHECK(zmq_poll(&item, 1, 30) == 0); // No partial serialized data reaches the peer.
        }
        else
        {
            const auto response = p.Read();
            CHECK(response.request_id() == target && response.service() == request.service());
            CHECK(response.schema_version() == 1 && response.status() == RPC_STATUS_INTERNAL);
            CHECK(response.payload().empty());
        }
        CHECK(ResponseFault::attempts == (alreadyInternal ? 1u : 2u));
        CHECK(ResponseFault::failures == 0);
        CHECK(f.gateway.Log().Total(Eyil::Reason::Transport) == 0);
        CHECK(f.gateway.Log().Total(serializationReturnsFalse ? Eyil::Reason::Protocol : Eyil::Reason::Allocation) > 0);

        const auto completedCalls = path == ResponsePath::Worker ? 1u : (path == ResponsePath::WorkerTimeout ? 4u : 0u);
        CHECK(f.crypto.entered == completedCalls); // Never replay a backend operation to prepare a reply.
        if (path == ResponsePath::WorkerTimeout)
        {
            CHECK(f.gateway.Log().Total(Eyil::Reason::WorkerExhausted) == 1);
            f.crypto.Unblock();
            std::set<uint64_t> completed;
            for (unsigned i = 0; i < 4; ++i)
            {
                const auto response = p.Read();
                CHECK(response.status() == RPC_STATUS_OK);
                completed.insert(response.request_id());
            }
            CHECK(completed == std::set<uint64_t>({ 1, 2, 3, 4 }));
        }

        // Reusing the retired ID tests accounting, not a client replay policy.
        p.Send(Request(target).SerializeAsString());
        auto response = p.Read();
        CHECK(response.request_id() == target && response.status() == RPC_STATUS_OK);
        other.Send(Request(91).SerializeAsString());
        response = other.Read();
        CHECK(response.request_id() == 91 && response.status() == RPC_STATUS_OK);
        CHECK(f.crypto.entered == completedCalls + 2 && f.gateway.Log().Total(Eyil::Reason::Transport) == 0);
        CHECK(f.crypto.references == 1 && f.crypto.SessionCount() == 2);
    }

    void RouteReuse()
    {
        Fixture f;
        auto a = std::make_unique<Peer>(f, "reused-route");
        f.crypto.block = true;

        struct Unblock
        {
            MockCrypto &c;

            ~Unblock() { c.Unblock(); }
        } unblock { f.crypto };

        a->Send(Request(55).SerializeAsString());
        Eventually([&] { return f.crypto.entered == 1; });
        a.reset();
        // Transport termination is observed separately from backend completion.
        // Bind the same ROUTER identity to another independently authenticated peer.
        auto b = std::make_unique<Peer>(f, "reused-route");
        Eventually([&] { return f.crypto.SessionCount() == 2; });
        f.crypto.Unblock();
        b->Send(Request(99).SerializeAsString());
        auto response = b->Read();
        CHECK(response.request_id() == 99 && response.status() == RPC_STATUS_OK);
        Eventually([&] { return f.crypto.SessionCount() == 1; });
        zmq_pollitem_t item { b->socket.Get(), 0, ZMQ_POLLIN, 0 };
        CHECK(zmq_poll(&item, 1, 50) == 0);
    }
}

int main(int argc, char **argv)
{
    return RunCases(argc, argv,
        { { "idle-poll", IdlePoll }, { "worker-logs", WorkerLogs },
            { "worker-burst-saturation", WorkerBurstSaturation }, { "envelopes", Envelopes },
            { "framing-drain", Framing }, { "duplicate-id", DuplicateInFlight },
            { "disconnect-before-request", DisconnectBeforeRequest }, { "route-reuse", RouteReuse },
            { "drain-replies", [] { DrainReplies(false, false); } },
            { "drain-waiting", [] { DrainReplies(true, false); } },
            { "drain-waiting-timeout", [] { DrainReplies(true, true); } },
            { "reply-eagain", [] { DropReply(EAGAIN, false); } },
            { "reply-unreachable", [] { DropReply(EHOSTUNREACH, false); } },
            { "reply-body-failure", [] { DropReply(EPIPE, true); } },
            { "admission-body-failure", [] { DropReply(EPIPE, true, true); } },
            { "response-admission-fallback", [] { ResponsePreparationFailure(ResponsePath::Admission, 1); } },
            { "response-admission-drop", [] { ResponsePreparationFailure(ResponsePath::Admission, 2); } },
            { "response-catch-drop", [] { ResponsePreparationFailure(ResponsePath::AdmissionFailure, 1); } },
            { "response-worker-fallback", [] { ResponsePreparationFailure(ResponsePath::Worker, 1); } },
            { "response-worker-drop", [] { ResponsePreparationFailure(ResponsePath::Worker, 2); } },
            { "response-timeout-drop", [] { ResponsePreparationFailure(ResponsePath::WorkerTimeout, 1); } },
            { "response-serialization-fallback", [] { ResponsePreparationFailure(ResponsePath::Admission, 1, true); } },
            { "response-serialization-drop", [] { ResponsePreparationFailure(ResponsePath::Admission, 2, true); } } });
}
