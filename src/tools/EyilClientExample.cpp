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
// Development diagnostic client. No cryptographic implementation or secret logging.
#include <eyilclient/EyilClient.h>
#include <rpc.pb.h>
#include <crypto.pb.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace
{
    std::mutex mutex;
    std::condition_variable changed;
    unsigned callbacks = 0, failures = 0, connectionEvents = 0;
    bool expectNoCrypto = false;

    void Require(bool value, const char *description)
    {
        if (!value)
            throw std::runtime_error(description);
    }

    const char *Name(ClientResult result)
    {
        switch (result)
        {
        case CLIENT_OK:
            return "CLIENT_OK";
        case CLIENT_INVALID_ARGUMENT:
            return "CLIENT_INVALID_ARGUMENT";
        case CLIENT_NOT_CONNECTED:
            return "CLIENT_NOT_CONNECTED";
        case CLIENT_AUTH_FAILED:
            return "CLIENT_AUTH_FAILED";
        case CLIENT_TIMEOUT:
            return "CLIENT_TIMEOUT";
        case CLIENT_COMMUNICATION_ERROR:
            return "CLIENT_COMMUNICATION_ERROR";
        case CLIENT_OUT_OF_MEMORY:
            return "CLIENT_OUT_OF_MEMORY";
        case CLIENT_ALREADY_CONNECTED:
            return "CLIENT_ALREADY_CONNECTED";
        case CLIENT_NOT_IMPLEMENTED:
            return "CLIENT_NOT_IMPLEMENTED";
        }
        return "UNKNOWN";
    }

    void Event(Client *, ClientConnectionEvent event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++connectionEvents;
        ++failures;
        std::cout << "connection_event=" << event << '\n';
        changed.notify_all();
    }

    void Print(const RpcResponse &rpc)
    {
        std::cout << "response id=" << rpc.request_id() << " rpc=" << RpcStatus_Name(rpc.status());
        if (rpc.status() == RPC_STATUS_OK)
        {
            CryptoResponse crypto;
            if (crypto.ParseFromString(rpc.payload()))
                std::cout << " crypto=" << CryptoError_Name(crypto.error());
            else
                ++failures;
        }
        std::cout << '\n';
    }

    bool ExpectedBackendReply(const RpcResponse &rpc)
    {
        if (expectNoCrypto)
            return rpc.status() == RPC_STATUS_INTERNAL && rpc.payload().empty();

        CryptoResponse crypto;
        return rpc.status() == RPC_STATUS_OK && crypto.ParseFromString(rpc.payload())
            && crypto.error() == CRYPTO_ERROR_NONE && crypto.has_generate_random()
            && crypto.generate_random().random_value().size() == 32;
    }

    void Binary(Client *, uint8_t *data, size_t size)
    {
        std::lock_guard<std::mutex> lock(mutex);
        RpcResponse rpc;
        if (data && rpc.ParseFromArray(data, static_cast<int>(size)) && ExpectedBackendReply(rpc))
            Print(rpc);
        else
            ++failures;
        client_free_response(data);
        ++callbacks;
        changed.notify_all();
    }

    void Text(Client *, char *data)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!data || std::strcmp(data, "RPC_STATUS_UNSUPPORTED_SERVICE"))
            ++failures;
        std::cout << "string_response=" << (data ? data : "NULL") << '\n';
        client_free_response(data);
        ++callbacks;
        changed.notify_all();
    }

    struct Handle
    {
        Client *value = nullptr;

        ~Handle()
        {
            if (value)
                client_disconnect(value);
        }
    };

    std::string Envelope(const CryptoRequest &crypto)
    {
        RpcRequest rpc;
        rpc.set_service(RPC_SERVICE_CRYPTO);
        rpc.set_payload(crypto.SerializeAsString());
        return rpc.SerializeAsString();
    }

    RpcResponse Sync(Client *client, const CryptoRequest &crypto)
    {
        const auto request = Envelope(crypto);
        uint8_t *data = nullptr;
        size_t size = 0;
        const auto status = client_send_request(
            client, reinterpret_cast<const uint8_t *>(request.data()), request.size(), &data, &size);
        Require(status == CLIENT_OK, "synchronous request failed locally");
        RpcResponse result;
        const bool parsed = result.ParseFromArray(data, static_cast<int>(size));
        client_free_response(data);
        Require(parsed, "invalid response");
        {
            std::lock_guard<std::mutex> lock(mutex);
            Print(result);
        }
        return result;
    }

    CryptoResponse Service(const RpcResponse &rpc)
    {
        Require(rpc.status() == RPC_STATUS_OK, "Gateway rejected buffer operation");
        CryptoResponse result;
        Require(result.ParseFromString(rpc.payload()), "invalid Crypto response");
        return result;
    }
}

int main(int argc, char **argv)
{
    try
    {
        if (argc != 2 || (std::strcmp(argv[1], "--expect-no-crypto") && std::strcmp(argv[1], "--exercise")))
        {
            std::cout << "Usage: eyil-client --expect-no-crypto | --exercise\n"
                         "--expect-no-crypto: connect and exercise APIs; backend RPCs must return INTERNAL.\n"
                         "--exercise: run sync, async, plain string, multi-connection and buffer cases.\n";
            return argc == 2 && !std::strcmp(argv[1], "--help") ? 0 : 2;
        }
        Require(client_register_notifications(nullptr, nullptr) == CLIENT_NOT_IMPLEMENTED, "notification contract");
        client_unregister_notifications(nullptr);
        client_free_response(nullptr);
        Require(client_connect(nullptr, nullptr) == CLIENT_INVALID_ARGUMENT, "argument validation");
        expectNoCrypto = !std::strcmp(argv[1], "--expect-no-crypto");
        Handle a;
        auto status = client_connect(&a.value, Event);
        std::cout << "connect=" << Name(status) << '\n';
        Require(status == CLIENT_OK && a.value && connectionEvents == 0, "connect failed or initial callback emitted");
        Require(client_connect(&a.value, Event) == CLIENT_ALREADY_CONNECTED, "occupied handle contract");
        Handle b;
        Require(client_connect(&b.value, Event) == CLIENT_OK, "second connection");
        char *text = nullptr;
        Require(client_send_request_string(a.value, "plain text: hello", &text) == CLIENT_OK, "sync string");
        Require(text && !std::strcmp(text, "RPC_STATUS_UNSUPPORTED_SERVICE"), "string routing status");
        client_free_response(text);
        Require(client_send_request_string_async(a.value, "another raw string", Text) == CLIENT_OK, "async string");
        CryptoRequest random;
        random.set_operation(CRYPTO_OPERATION_GENERATE_RANDOM);
        random.mutable_generate_random()->set_number_of_bytes(32);
        const auto request = Envelope(random);
        std::vector<std::future<ClientResult>> sends;
        for (unsigned i = 0; i < 16; ++i)
            sends.push_back(std::async(std::launch::async,
                [&, i]
                {
                    return client_send_request_async(i % 2 ? a.value : b.value,
                        reinterpret_cast<const uint8_t *>(request.data()), request.size(), Binary);
                }));
        for (auto &sent : sends)
            Require(sent.get() == CLIENT_OK, "async send acceptance");
        Require(ExpectedBackendReply(Sync(a.value, random)), "unexpected backend RPC result");
        std::vector<std::string> buffers;
        CryptoRequest allocate;
        allocate.set_operation(CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER);
        allocate.mutable_allocate_shared_buffer()->set_requested_size(4096);
        for (unsigned i = 0; i < 4; ++i)
        {
            auto response = Service(Sync(i % 2 ? a.value : b.value, allocate));
            Require(response.error() == CRYPTO_ERROR_NONE, "allocate buffer");
            buffers.push_back(response.allocate_shared_buffer().buffer_id());
        }
        Require(Service(Sync(a.value, allocate)).error() == CRYPTO_ERROR_OPERATION_FAILED,
            "four buffers shared across host UID");
        for (unsigned i = 0; i < 4; ++i)
        {
            CryptoRequest release;
            release.set_operation(CRYPTO_OPERATION_RELEASE_SHARED_BUFFER);
            release.mutable_release_shared_buffer()->set_buffer_id(buffers[i]);
            Require(
                Service(Sync(i % 2 ? a.value : b.value, release)).error() == CRYPTO_ERROR_NONE, "release own buffer");
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            // This is the diagnostic application's wait budget, not a library response timeout.
            Require(changed.wait_for(lock, std::chrono::seconds(30), [] { return callbacks == 17 || failures; }),
                "diagnostic response wait expired; remote outcome is uncertain");
            Require(!failures && !connectionEvents, "response or connection failure");
        }
        std::cout << "PASS: 16 binary async responses, string sync/async, mixed sync, two connections, shared UID "
                     "quota and release.\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
