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
#include "../support/MockCrypto.h"
#include "GatewayRuntime.h"
#include <eyilclient/EyilClient.h>
#include <future>
#include <map>

namespace
{
    std::mutex resultsMutex;
    std::vector<RpcResponse> responses;
    std::atomic<unsigned> textResponses { 0 }, lost { 0 }, callbackErrors { 0 };

    std::string randomRequest(unsigned count = 16)
    {
        CryptoRequest request;
        request.set_operation(CRYPTO_OPERATION_GENERATE_RANDOM);
        request.mutable_generate_random()->set_number_of_bytes(count);
        RpcRequest rpc;
        rpc.set_service(RPC_SERVICE_CRYPTO);
        rpc.set_payload(request.SerializeAsString());
        return rpc.SerializeAsString();
    }

    void ConnectionEvent(Client *, ClientConnectionEvent event)
    {
        if (event == CLIENT_CONNECTION_LOST)
            ++lost;
        else
            ++callbackErrors;
    }

    void Response(Client *client, uint8_t *data, size_t size)
    {
        RpcResponse result;
        if (!data || !result.ParseFromArray(data, static_cast<int>(size)))
            ++callbackErrors;
        client_free_response(data);
        // Callback reentrancy must fail before trying to acquire the submission slot.
        char *text = nullptr;
        if (client_send_request_string(client, "reentrant", &text) != CLIENT_INVALID_ARGUMENT || text)
            ++callbackErrors;
        std::lock_guard<std::mutex> lock(resultsMutex);
        responses.push_back(result);
    }

    void TextResponse(Client *, char *data)
    {
        if (!data || std::string(data) != "RPC_STATUS_UNSUPPORTED_SERVICE")
            ++callbackErrors;
        client_free_response(data);
        ++textResponses;
    }

    size_t Count()
    {
        std::lock_guard<std::mutex> lock(resultsMutex);
        return responses.size();
    }

    struct ClientOwner
    {
        Client *value = nullptr;

        ~ClientOwner()
        {
            if (value)
                client_disconnect(value);
        }

        void Connect() { CHECK(client_connect(&value, ConnectionEvent) == CLIENT_OK); }
    };

    struct Fixture
    {
        EyilTest::MockCrypto crypto;
        Eyil::GatewayRuntime gateway { [this] { return crypto.Acquire(); } };

        Fixture() { CHECK(gateway.Start()); }

        ~Fixture()
        {
            crypto.Unblock();
            gateway.StopAndWait();
        }
    };

    struct Unblock
    {
        EyilTest::MockCrypto &crypto;

        ~Unblock() { crypto.Unblock(); }
    };

    void Basic()
    {
        Fixture f;
        ClientOwner a, b;
        a.Connect();
        b.Connect();
        CHECK(lost == 0);
        CHECK(f.crypto.SessionCount() == 2);
        Client *original = a.value;
        CHECK(client_connect(&a.value, nullptr) == CLIENT_ALREADY_CONNECTED);
        CHECK(a.value == original);
        auto request = randomRequest();
        const auto before = Count();
        std::vector<std::future<ClientResult>> sends;
        for (unsigned i = 0; i < 80; ++i)
            sends.push_back(std::async(std::launch::async,
                [&, i]
                {
                    return client_send_request_async(i % 2 ? a.value : b.value,
                        reinterpret_cast<const uint8_t *>(request.data()), request.size(), Response);
                }));
        for (auto &send : sends)
            CHECK(send.get() == CLIENT_OK);
        Eventually([&] { return Count() == before + 80; });
        CHECK(callbackErrors == 0);
        uint8_t *bytes = nullptr;
        size_t size = 0;
        CHECK(client_send_request(
                  a.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), &bytes, &size)
            == CLIENT_OK);
        RpcResponse response;
        CHECK(response.ParseFromArray(bytes, size));
        client_free_response(bytes);
        CHECK(response.status() == RPC_STATUS_OK);
        // A backend disappearing after authentication is an RPC error, not a
        // fabricated local client failure or a cached-interface invocation.
        f.crypto.available = false;
        CHECK(client_send_request(
                  a.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), &bytes, &size)
            == CLIENT_OK);
        CHECK(response.ParseFromArray(bytes, size));
        client_free_response(bytes);
        CHECK(response.status() == RPC_STATUS_INTERNAL && response.payload().empty());
        f.crypto.available = true;
        char *text = nullptr;
        CHECK(client_send_request_string(a.value, "any plain string", &text) == CLIENT_OK);
        CHECK(std::string(text) == "RPC_STATUS_UNSUPPORTED_SERVICE");
        client_free_response(text);
        auto textBefore = textResponses.load();
        CHECK(client_send_request_string_async(b.value, "not base64", TextResponse) == CLIENT_OK);
        Eventually([&] { return textResponses == textBefore + 1; });
        // No first RPC is necessary for monitor-driven cleanup.
        {
            ClientOwner idle;
            idle.Connect();
            CHECK(f.crypto.SessionCount() == 3);
        }
        Eventually([&] { return f.crypto.SessionCount() == 2; });
        CHECK(lost == 0);
    }

    void OutOfOrder()
    {
        Fixture f;
        ClientOwner a;
        a.Connect();
        f.crypto.slowSize = 7;
        auto slow = randomRequest(7), fast = randomRequest(8);
        const auto before = Count();
        CHECK(client_send_request_async(a.value, reinterpret_cast<const uint8_t *>(slow.data()), slow.size(), Response)
            == CLIENT_OK);
        CHECK(client_send_request_async(a.value, reinterpret_cast<const uint8_t *>(fast.data()), fast.size(), Response)
            == CLIENT_OK);
        Eventually([&] { return Count() == before + 2; });
        std::lock_guard<std::mutex> lock(resultsMutex);
        CHECK(responses[before].request_id() == 2);
        CHECK(responses[before + 1].request_id() == 1);
        CryptoResponse result;
        CHECK(result.ParseFromString(responses[before].payload()));
        CHECK(result.generate_random().random_value().size() == 8);
    }

    void SharedMemory()
    {
        Fixture f;
        ClientOwner a, b;
        a.Connect();
        b.Connect();
        auto sync = [&](Client *client, const CryptoRequest &request)
        {
            RpcRequest rpc;
            rpc.set_service(RPC_SERVICE_CRYPTO);
            rpc.set_payload(request.SerializeAsString());
            auto wire = rpc.SerializeAsString();
            uint8_t *bytes = nullptr;
            size_t size = 0;
            CHECK(
                client_send_request(client, reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), &bytes, &size)
                == CLIENT_OK);
            RpcResponse reply;
            CHECK(reply.ParseFromArray(bytes, size));
            client_free_response(bytes);
            CHECK(reply.status() == RPC_STATUS_OK);
            CryptoResponse result;
            CHECK(result.ParseFromString(reply.payload()));
            return result;
        };
        CryptoRequest allocate;
        allocate.set_operation(CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER);
        allocate.mutable_allocate_shared_buffer()->set_requested_size(4096);
        const auto result = sync(a.value, allocate);
        CHECK(result.error() == CRYPTO_ERROR_NONE);
        const auto id = result.allocate_shared_buffer().buffer_id();
        CryptoRequest release;
        release.set_operation(CRYPTO_OPERATION_RELEASE_SHARED_BUFFER);
        release.mutable_release_shared_buffer()->set_buffer_id(id);
        CHECK(sync(b.value, release).error() == CRYPTO_ERROR_INVALID_SESSION_STATE);
        CryptoRequest sign;
        sign.set_operation(CRYPTO_OPERATION_SIGN);
        auto *data = sign.mutable_sign();
        data->set_buffer_id(id);
        data->set_algorithm(SIGN_ALGORITHM_HMAC);
        data->set_digest_algorithm(DIGEST_ALGORITHM_SHA256);
        data->set_key_id(71);
        data->mutable_input_data()->set_length(32);
        RpcRequest rpc;
        rpc.set_service(RPC_SERVICE_CRYPTO);
        rpc.set_payload(sign.SerializeAsString());
        const auto wire = rpc.SerializeAsString();
        f.crypto.block = true;
        Unblock guard { f.crypto };
        const auto before = Count();
        CHECK(client_send_request_async(a.value, reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), Response)
            == CLIENT_OK);
        Eventually([&] { return f.crypto.entered == 1; });
        CHECK(sync(a.value, sign).error() == CRYPTO_ERROR_INVALID_SESSION_STATE);
        CHECK(sync(a.value, release).error() == CRYPTO_ERROR_NONE);
        CHECK(f.gateway.Buffers().Charged(getuid()) == 1);
        f.crypto.Unblock();
        Eventually([&] { return Count() == before + 1 && f.gateway.Buffers().Charged(getuid()) == 0; });
        CHECK(sync(a.value, allocate).error() == CRYPTO_ERROR_NONE);
        CHECK(sync(b.value, allocate).error() == CRYPTO_ERROR_NONE);
        client_disconnect(a.value);
        a.value = nullptr;
        Eventually([&] { return f.gateway.Buffers().Charged(getuid()) == 1 && f.crypto.SessionCount() == 1; });
    }

    void MissingCrypto()
    {
        EyilTest::MockCrypto crypto;
        crypto.available = false;
        Eyil::GatewayRuntime gateway([&] { return crypto.Acquire(); });
        CHECK(gateway.Start());
        const auto request = randomRequest();
        for (int i = 0; i < 6; ++i)
        {
            ClientOwner client;
            client.Connect();
            const auto before = Count();
            CHECK(client_send_request_async(
                      client.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), Response)
                == CLIENT_OK);
            Eventually([&] { return Count() == before + 1; });
            std::lock_guard<std::mutex> lock(resultsMutex);
            CHECK(responses.back().status() == RPC_STATUS_INTERNAL && responses.back().payload().empty());
        }
        CHECK(lost == 0 && callbackErrors == 0 && crypto.SessionCount() == 0);

        // Availability later does not silently create a session or replay CONNECTED.
        ClientOwner existing;
        existing.Connect();
        crypto.available = true;
        const auto before = Count();
        CHECK(client_send_request_async(
                  existing.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), Response)
            == CLIENT_OK);
        Eventually([&] { return Count() == before + 1; });
        {
            std::lock_guard<std::mutex> lock(resultsMutex);
            CryptoResponse result;
            CHECK(responses.back().status() == RPC_STATUS_OK && result.ParseFromString(responses.back().payload()));
            CHECK(result.error() == CRYPTO_ERROR_INVALID_SESSION_STATE);
        }
        CHECK(crypto.SessionCount() == 0);
        client_disconnect(existing.value);
        existing.value = nullptr;
        {
            ClientOwner fresh;
            fresh.Connect();
            CHECK(crypto.SessionCount() == 1);
        }
        Eventually([&] { return crypto.SessionCount() == 0; });
        CHECK(lost == 0 && callbackErrors == 0);
        gateway.StopAndWait();
    }

    void WorkerExhaustion()
    {
        Fixture f;
        ClientOwner a;
        a.Connect();
        auto request = randomRequest();
        f.crypto.block = true;
        Unblock guard { f.crypto };
        const auto before = Count();
        for (unsigned i = 0; i < 4; ++i)
            CHECK(client_send_request_async(
                      a.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), Response)
                == CLIENT_OK);
        Eventually([&] { return f.crypto.entered == 4; });
        CHECK(client_send_request_async(
                  a.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), Response)
            == CLIENT_OK);
        Eventually([&] { return Count() == before + 1; }, 12500);
        {
            std::lock_guard<std::mutex> lock(resultsMutex);
            CHECK(responses.back().status() == RPC_STATUS_INTERNAL);
        }
        CHECK(f.crypto.entered == 4);
        f.crypto.Unblock();
        Eventually([&] { return Count() == before + 5; });
    }

    void LateAsyncAndClose()
    {
        Fixture f;
        ClientOwner a;
        a.Connect();
        f.crypto.delayMs = 15500;
        auto request = randomRequest();
        auto before = Count();
        auto start = Eyil::Clock::now();
        CHECK(client_send_request_async(
                  a.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), Response)
            == CLIENT_OK);
        CHECK(Eyil::Clock::now() - start < std::chrono::seconds(2));
        auto timed = std::async(std::launch::async,
            [&]
            {
                uint8_t *result = reinterpret_cast<uint8_t *>(1);
                size_t size = 99;
                const auto status = client_send_request(
                    a.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), &result, &size);
                CHECK(!result && !size);
                return status;
            });
        CHECK(timed.get() == CLIENT_TIMEOUT);
        // The synchronous deadline must not retire the independent async call.
        Eventually([&] { return Count() == before + 1; }, 18000);
        CHECK(callbackErrors == 0);
        f.crypto.delayMs = 0;
        // Local disconnect wakes a concurrent synchronous call; remote work drains independently.
        f.crypto.block = true;
        Unblock guard { f.crypto };
        const auto entered = f.crypto.entered.load();
        auto waiting = std::async(std::launch::async,
            [&]
            {
                uint8_t *result = nullptr;
                size_t size = 0;
                auto status = client_send_request(
                    a.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), &result, &size);
                if (result || size)
                    ++callbackErrors;
                return status;
            });
        Eventually([&] { return f.crypto.entered > entered; });
        client_disconnect(a.value);
        a.value = nullptr;
        CHECK(waiting.get() == CLIENT_NOT_CONNECTED);
        CHECK(lost == 0);
        f.crypto.Unblock();
        Eventually([&] { return f.crypto.SessionCount() == 0; });
    }

    void Loss()
    {
        Fixture f;
        ClientOwner a;
        a.Connect();
        const auto before = lost.load();
        f.gateway.StopAndWait();
        Eventually([&] { return lost == before + 1; });
        auto request = randomRequest();
        CHECK(client_send_request_async(
                  a.value, reinterpret_cast<const uint8_t *>(request.data()), request.size(), Response)
            == CLIENT_NOT_CONNECTED);
        CHECK(client_connect(&a.value, nullptr) == CLIENT_ALREADY_CONNECTED);
    }
}

int main(int argc, char **argv)
{
    auto checked = [](auto run)
    {
        return [run]
        {
            RequireUnusedEndpoint();
            run();
            CHECK(callbackErrors == 0);
        };
    };
    return EyilTest::RunCases(argc, argv,
        { { "basic", checked(Basic) }, { "out-of-order", checked(OutOfOrder) },
            { "shared-memory", checked(SharedMemory) }, { "missing-crypto", checked(MissingCrypto) },
            { "worker-exhaustion", checked(WorkerExhaustion) }, { "late-async-close", checked(LateAsyncAndClose) },
            { "connection-loss", checked(Loss) }, { "patch", [] { CHECK(Eyil::VerifyTransportPatch()); } },
            { "patch-cleanup",
                []
                {
                    for (int i = 0; i < 20; ++i)
                        CHECK(Eyil::VerifyTransportPatch());
                } },
            { "unpatched", []
                {
                    const auto result = Eyil::VerifyTransportPatch();
                    CHECK(!result && result.error == ENOTSUP);
                    CHECK(std::string(result.message).find("option 10001") != std::string::npos);
                } } });
}
