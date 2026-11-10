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
    std::atomic<uint64_t> admissionAllocation { 0 };
    std::atomic<unsigned> attempts { 0 };
    std::atomic<int> failures { 0 };
    std::atomic<bool> returnFalse { false };
}

extern "C"
{
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
    void Ignore(const char *) { }

    struct Fixture
    {
        MockCrypto crypto;
        Eyil::GatewayRuntime gateway { [this] { return crypto.Acquire(); }, 4, Eyil::Endpoint, Ignore };
        Eyil::Context context;

        Fixture()
        {
            ReplyFault::error = 0;
            ReplyFault::countSends = false;
            ResponseFault::request = 0;
            ResponseFault::parsed = 0;
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
        p.Send(wire);
        auto duplicate = p.Read();
        CHECK(duplicate.request_id() == 77 && duplicate.status() == RPC_STATUS_INTERNAL && f.crypto.entered == 1);
        f.crypto.Unblock();
        auto original = p.Read();
        CHECK(original.request_id() == 77 && original.status() == RPC_STATUS_OK && f.crypto.entered == 1);
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
            clockAdvanceMs = 10001;
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
        { { "envelopes", Envelopes }, { "framing-drain", Framing }, { "duplicate-id", DuplicateInFlight },
            { "disconnect-before-request", DisconnectBeforeRequest }, { "route-reuse", RouteReuse },
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
