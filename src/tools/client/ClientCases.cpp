// Copyright 2026 RDK Management
// SPDX-License-Identifier: Apache-2.0
#include "TestClient.h"

#include <cstring>
#include <memory>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace EyilDev
{
    void ApiCases(Runner &runner, Connection &a)
    {
        runner.Run("public-api-validation",
            "invalid calls clear outputs; occupied handle unchanged; notifications unsupported",
            [&]
            {
                Check(
                    client_connect(nullptr, Runner::Event) == CLIENT_INVALID_ARGUMENT, "NULL connect output accepted");
                auto *existing = a.Get();
                Check(client_connect(&existing, Runner::Event) == CLIENT_ALREADY_CONNECTED && existing == a.Get(),
                    "occupied handle was replaced");
                Check(client_register_notifications(a.Get(), Runner::Notification) == CLIENT_NOT_IMPLEMENTED,
                    "notification registration must remain unsupported");
                client_unregister_notifications(a.Get());
                client_free_response(nullptr);
                client_disconnect(nullptr);
                uint8_t sentinel = 0;
                uint8_t *data = &sentinel;
                size_t size = 99;
                const uint8_t invalid[] { 0xff };
                const auto result = client_send_request(a.Get(), invalid, sizeof(invalid), &data, &size);
                Check(result == CLIENT_INVALID_ARGUMENT && data == nullptr && size == 0,
                    std::string("invalid binary request: expected INVALID_ARGUMENT and NULL/0, actual ")
                        + Name(result));
                Check(client_send_request_async(a.Get(), invalid, sizeof(invalid), Runner::Notification)
                        == CLIENT_INVALID_ARGUMENT,
                    "invalid async request accepted");
                const auto valid = Envelope(RandomRequest(3)).SerializeAsString();
                Check(client_send_request_async(
                          a.Get(), reinterpret_cast<const uint8_t *>(valid.data()), valid.size(), nullptr)
                        == CLIENT_INVALID_ARGUMENT,
                    "NULL response handler accepted");
                char textSentinel = 0;
                char *text = &textSentinel;
                Check(client_send_request_string(a.Get(), nullptr, &text) == CLIENT_INVALID_ARGUMENT && text == nullptr,
                    "invalid string request did not clear output");
                Check(client_send_request_string_async(a.Get(), "text", nullptr) == CLIENT_INVALID_ARGUMENT,
                    "NULL string response handler accepted");
            });
        for (const auto delivery : { Delivery::Sync, Delivery::Async })
            for (const std::string &text : { std::string("plain text"), std::string(""), std::string("UTF-8: zażółć"),
                     std::string("prefix\0ignored", 14) })
                runner.Run(std::string("string/") + Name(delivery) + "/bytes=" + std::to_string(text.size()),
                    "CLIENT_OK / RPC_STATUS_UNSUPPORTED_SERVICE text",
                    [&]
                    {
                        const auto response = runner.Text(a, text.c_str(), delivery);
                        runner.Log("STRING_REPLY status=" + response);
                        Check(response == "RPC_STATUS_UNSUPPORTED_SERVICE", "unexpected string routing response");
                    });
    }

    void InvalidRequestCases(Runner &runner, Connection &a)
    {
        std::vector<std::pair<std::string, RpcRequest>> cases;
        RpcRequest unknown = Envelope(RandomRequest(8));
        unknown.set_service(static_cast<RpcService>(999));
        cases.push_back({ "unknown-service", unknown });
        auto malformed = Envelope(RandomRequest(8));
        malformed.set_payload(std::string(1, '\x80'));
        cases.push_back({ "unparseable-crypto", malformed });
        malformed.clear_payload();
        cases.push_back({ "empty-crypto", malformed });
        auto crypto = RandomRequest(8);
        crypto.set_operation(static_cast<CryptoOperation>(999));
        cases.push_back({ "unknown-operation", Envelope(crypto) });
        crypto.set_operation(CRYPTO_OPERATION_SIGN);
        cases.push_back({ "operation-oneof-mismatch", Envelope(crypto) });
        crypto.Clear();
        crypto.set_operation(CRYPTO_OPERATION_GENERATE_KEY);
        cases.push_back({ "missing-oneof", Envelope(crypto) });
        crypto = Request(CRYPTO_OPERATION_GENERATE_KEY);
        crypto.mutable_generate_key()->set_algorithm(static_cast<GenerateKeyAlgorithm>(999));
        cases.push_back({ "invalid-enum", Envelope(crypto) });
        crypto = Request(CRYPTO_OPERATION_GENERATE_KEY);
        crypto.mutable_generate_key()->add_key_usage(KEY_USAGE_ENCRYPT);
        cases.push_back({ "duplicate-key-usage", Envelope(crypto) });
        cases.push_back({ "random-size-zero", Envelope(RandomRequest(0)) });
        crypto = Request(CRYPTO_OPERATION_IMPORT_KEY);
        crypto.mutable_import_key()->clear_key_blob();
        cases.push_back({ "empty-import", Envelope(crypto) });
        crypto = Request(CRYPTO_OPERATION_EXPORT_KEY);
        crypto.mutable_export_key()->set_import_export_type(IMPORT_EXPORT_TYPE_UNSPECIFIED);
        cases.push_back({ "present-unspecified-format", Envelope(crypto) });
        for (const auto &entry : cases)
            for (const auto delivery : { Delivery::Sync, Delivery::Async })
                runner.Run("invalid-request/" + entry.first + "/" + Name(delivery),
                    entry.first == "unknown-service" ? "CLIENT_OK / UNSUPPORTED_SERVICE" : "CLIENT_OK / INTERNAL",
                    [&]
                    {
                        RpcResponse response;
                        if (delivery == Delivery::Sync)
                            response = runner.Sync(a, entry.second);
                        else
                        {
                            runner.Start({ { &a, entry.second } });
                            response = runner.Finish().front();
                        }
                        ExpectRpc(response,
                            entry.first == "unknown-service" ? RPC_STATUS_UNSUPPORTED_SERVICE : RPC_STATUS_INTERNAL);
                    });
    }

    namespace
    {
        void WaitUntilRemoved(Runner &runner, const std::vector<std::string> &names)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            runner.Log("WAIT disconnect cleanup: observe only this test's shared-memory names, budget_ms=5000");
            for (;;)
            {
                bool removed = true;
                for (const auto &name : names)
                {
                    const auto fd = shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
                    if (fd >= 0)
                    {
                        close(fd);
                        removed = false;
                    }
                    else
                        Check(errno == ENOENT, "cannot observe cleanup errno=" + std::to_string(errno));
                }
                if (removed)
                    return;
                if (std::chrono::steady_clock::now() >= deadline)
                    throw Failure(
                        "Gateway did not unlink disconnected client's buffers within diagnostic budget", true);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    void LifecycleCases(Runner &runner, Connection &a, Connection &b)
    {
        runner.Run("disconnect-cleanup-and-new-context",
            "A resources reclaimed, B unaffected, fresh A rejects old buffer IDs",
            [&]
            {
                Buffer surviving(runner, b, 128);
                std::vector<std::unique_ptr<Buffer>> owned;
                std::vector<std::string> names;
                for (unsigned i = 0; i < 3; ++i)
                {
                    owned.push_back(std::make_unique<Buffer>(runner, a, 128));
                    names.push_back(owned.back()->ObjectName());
                }
                const auto staleId = owned.front()->Id();
                CheckRandom(runner, runner.Send(a, RandomRequest(23), Delivery::Async), 23);
                a.Close();
                // Destruct while the old handle is NULL: Gateway, not the helper,
                // must reclaim these allocations through correlated disconnect.
                owned.clear();
                WaitUntilRemoved(runner, names);
                CheckRandom(runner, runner.Send(b, RandomRequest(29), Delivery::Sync), 29);
                a.Connect();
                Expect(runner.Send(a, ReleaseRequest(staleId), Delivery::Async), CRYPTO_OPERATION_RELEASE_SHARED_BUFFER,
                    CRYPTO_ERROR_INVALID_SESSION_STATE);
                Expect(runner.Send(a, Request(CRYPTO_OPERATION_SIGN, staleId), Delivery::Sync), CRYPTO_OPERATION_SIGN,
                    CRYPTO_ERROR_INVALID_SESSION_STATE);
                std::vector<std::unique_ptr<Buffer>> recovered;
                for (unsigned i = 0; i < 3; ++i)
                    recovered.push_back(std::make_unique<Buffer>(runner, i % 2 ? a : b, 128));
                Expect(runner.Send(a, AllocateRequest(128), Delivery::Sync), CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER,
                    CRYPTO_ERROR_OPERATION_FAILED);
                for (auto &buffer : recovered)
                    buffer->Release();
                surviving.Release();
            });
        for (unsigned cycle = 0; cycle < 10; ++cycle)
            runner.Run("connection-cycle/" + std::to_string(cycle + 1),
                "connect, async response, allocate/release, silent disconnect",
                [&]
                {
                    Connection connection(runner, "cycle-" + std::to_string(cycle + 1));
                    Buffer buffer(runner, connection, 128, Delivery::Async);
                    CheckRandom(
                        runner, runner.Send(connection, RandomRequest(11 + cycle), Delivery::Async), 11 + cycle);
                    buffer.Release(Delivery::Async);
                    connection.Close();
                });
    }
}
