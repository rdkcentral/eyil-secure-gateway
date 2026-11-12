// Copyright 2026 RDK Management
// SPDX-License-Identifier: Apache-2.0
#include "TestClient.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace EyilDev
{
    CryptoRequest AllocateRequest(uint32_t size)
    {
        CryptoRequest request;
        request.set_operation(CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER);
        request.mutable_allocate_shared_buffer()->set_requested_size(size);
        return request;
    }

    CryptoRequest ReleaseRequest(const std::string &id)
    {
        CryptoRequest request;
        request.set_operation(CRYPTO_OPERATION_RELEASE_SHARED_BUFFER);
        request.mutable_release_shared_buffer()->set_buffer_id(id);
        return request;
    }

    CryptoRequest RandomRequest(uint32_t size)
    {
        CryptoRequest request;
        request.set_operation(CRYPTO_OPERATION_GENERATE_RANDOM);
        request.mutable_generate_random()->set_number_of_bytes(size);
        return request;
    }

    CryptoRequest Request(CryptoOperation operation, const std::string &bufferId)
    {
        CryptoRequest request;
        request.set_operation(operation);
        switch (operation)
        {
        case CRYPTO_OPERATION_LOAD_KEY:
            request.mutable_load_key()->set_locator("eyil-development-test-key");
            request.mutable_load_key()->set_provision_type(PROVISION_TYPE_PRE_PROVISIONED_GLOBAL);
            break;
        case CRYPTO_OPERATION_KEY_EXISTS:
            request.mutable_key_exists()->set_locator("eyil-development-test-key");
            break;
        case CRYPTO_OPERATION_FLUSH_KEYS:
            request.mutable_flush_keys();
            break;
        case CRYPTO_OPERATION_GENERATE_KEY_PAIR:
            request.mutable_generate_key_pair()->set_algorithm(KEY_PAIR_ALGORITHM_AUTH_DH);
            request.mutable_generate_key_pair()->mutable_auth_dh()->set_prime_p(std::string(256, '\x5b'));
            request.mutable_generate_key_pair()->mutable_auth_dh()->set_generator_g(2);
            break;
        case CRYPTO_OPERATION_DERIVE_SESSION_KEYS:
            request.mutable_derive_session_keys()->set_algorithm(KEY_PAIR_ALGORITHM_AUTH_DH);
            request.mutable_derive_session_keys()->set_private_key_id(2);
            request.mutable_derive_session_keys()->set_public_key_id(3);
            request.mutable_derive_session_keys()->set_derivation_key_id(1);
            break;
        case CRYPTO_OPERATION_GET_KEY_INFO:
            request.mutable_get_key_info()->set_key_id(1);
            break;
        case CRYPTO_OPERATION_GENERATE_KEY:
            request.mutable_generate_key()->set_algorithm(GENERATE_KEY_ALGORITHM_AES128);
            request.mutable_generate_key()->add_key_usage(KEY_USAGE_ENCRYPT);
            request.mutable_generate_key()->add_key_usage(KEY_USAGE_DECRYPT);
            request.mutable_generate_key()->add_key_usage(KEY_USAGE_SIGN);
            request.mutable_generate_key()->add_key_usage(KEY_USAGE_VERIFY);
            break;
        case CRYPTO_OPERATION_UNLOAD_KEY:
            request.mutable_unload_key()->set_key_id(1);
            break;
        case CRYPTO_OPERATION_IMPORT_KEY:
            request.mutable_import_key()->set_key_type(KEY_TYPE_SYMMETRIC);
            request.mutable_import_key()->set_key_blob("development-only");
            break;
        case CRYPTO_OPERATION_EXPORT_KEY:
            request.mutable_export_key()->set_key_id(1);
            break;
        case CRYPTO_OPERATION_GENERATE_RANDOM:
            return RandomRequest(32);
        case CRYPTO_OPERATION_ENCRYPT:
        {
            auto *value = request.mutable_encrypt();
            value->set_buffer_id(bufferId);
            value->set_key_id(1);
            value->set_algorithm(CIPHER_ALGORITHM_AES_CBC);
            *value->mutable_init_data() = Region(0, 16);
            *value->mutable_input_data() = Region(32, 16);
            break;
        }
        case CRYPTO_OPERATION_DECRYPT:
        {
            auto *value = request.mutable_decrypt();
            value->set_buffer_id(bufferId);
            value->set_key_id(1);
            value->set_algorithm(CIPHER_ALGORITHM_AES_CBC);
            *value->mutable_init_data() = Region(0, 16);
            *value->mutable_encrypted_data() = Region(32, 16);
            break;
        }
        case CRYPTO_OPERATION_SIGN:
        {
            auto *value = request.mutable_sign();
            value->set_buffer_id(bufferId);
            value->set_key_id(1);
            value->set_algorithm(SIGN_ALGORITHM_HMAC);
            value->set_digest_algorithm(DIGEST_ALGORITHM_SHA256);
            *value->mutable_input_data() = Region(32, 16);
            break;
        }
        case CRYPTO_OPERATION_VERIFY:
        {
            auto *value = request.mutable_verify();
            value->set_buffer_id(bufferId);
            value->set_key_id(1);
            value->set_algorithm(SIGN_ALGORITHM_HMAC);
            value->set_digest_algorithm(DIGEST_ALGORITHM_SHA256);
            *value->mutable_input_data() = Region(32, 16);
            *value->mutable_expected_signature() = Region(64, 32);
            break;
        }
        default:
            throw Failure("no diagnostic request builder for " + CryptoOperation_Name(operation), true);
        }
        return request;
    }

    void CheckRandom(Runner &runner, const RpcResponse &response, uint32_t size)
    {
        if (runner.GetMode() == Mode::MissingBackend)
        {
            ExpectRpc(response, RPC_STATUS_INTERNAL);
            return;
        }
        const auto crypto = Expect(response, CRYPTO_OPERATION_GENERATE_RANDOM);
        const auto &bytes = crypto.generate_random().random_value();
        Check(bytes.size() == size, "random response length does not match its request");
        if (runner.GetMode() == Mode::TestBackend)
            Check(std::all_of(bytes.begin(), bytes.end(), [](char byte) { return byte == 0x5a; }),
                "random response differs from CryptoTestBackend fixed bytes");
    }

    namespace
    {
        void CheckFixed(const CryptoResponse &response, CryptoOperation operation)
        {
            switch (operation)
            {
            case CRYPTO_OPERATION_LOAD_KEY:
                Check(response.load_key().key_id() == 1, "expected loaded key ID 1");
                break;
            case CRYPTO_OPERATION_KEY_EXISTS:
                Check(response.key_exists().exists() && response.key_exists().key_id() == 1, "expected existing key 1");
                break;
            case CRYPTO_OPERATION_GENERATE_KEY_PAIR:
                Check(response.generate_key_pair().private_key_id() == 2
                        && response.generate_key_pair().public_key_id() == 3,
                    "expected key pair IDs 2/3");
                break;
            case CRYPTO_OPERATION_DERIVE_SESSION_KEYS:
                Check(response.derive_session_keys().encryption_key_id() == 4
                        && response.derive_session_keys().hmac_key_id() == 5
                        && response.derive_session_keys().wrapping_key_id() == 6,
                    "expected derived key IDs 4/5/6");
                break;
            case CRYPTO_OPERATION_GET_KEY_INFO:
            {
                const auto &info = response.get_key_info();
                const std::set<int> usages(info.key_usage().begin(), info.key_usage().end());
                Check(info.key_type() == KEY_TYPE_SYMMETRIC && info.key_algorithm() == KEY_ALGORITHM_AES_CBC
                        && info.key_usage_size() == 4
                        && usages
                            == std::set<int> { KEY_USAGE_ENCRYPT, KEY_USAGE_DECRYPT, KEY_USAGE_SIGN, KEY_USAGE_VERIFY },
                    "unexpected fixed key metadata");
                break;
            }
            case CRYPTO_OPERATION_GENERATE_KEY:
                Check(response.generate_key().key_id() == 1, "expected generated key ID 1");
                break;
            case CRYPTO_OPERATION_IMPORT_KEY:
                Check(response.import_key().key_id() == 1, "expected imported key ID 1");
                break;
            case CRYPTO_OPERATION_EXPORT_KEY:
                Check(response.export_key().key_blob() == "EYIL", "unexpected fixed export bytes");
                break;
            default:
                break; // Flush/Unload have empty successful response messages.
            }
        }
    }

    void KeyCases(Runner &runner, Connection &a)
    {
        if (runner.GetMode() == Mode::Exercise)
        {
            runner.Skip(
                "fixed-key-operations", "requires CryptoTestBackend; arbitrary backend key policy is not assumed");
            return;
        }
        const std::array<CryptoOperation, 11> operations { CRYPTO_OPERATION_LOAD_KEY, CRYPTO_OPERATION_KEY_EXISTS,
            CRYPTO_OPERATION_GENERATE_KEY_PAIR, CRYPTO_OPERATION_DERIVE_SESSION_KEYS, CRYPTO_OPERATION_GET_KEY_INFO,
            CRYPTO_OPERATION_GENERATE_KEY, CRYPTO_OPERATION_IMPORT_KEY, CRYPTO_OPERATION_EXPORT_KEY,
            CRYPTO_OPERATION_GENERATE_RANDOM, CRYPTO_OPERATION_UNLOAD_KEY, CRYPTO_OPERATION_FLUSH_KEYS };
        for (const auto operation : operations)
            for (const auto delivery : { Delivery::Sync, Delivery::Async })
                runner.Run(CryptoOperation_Name(operation) + "/" + Name(delivery),
                    runner.GetMode() == Mode::MissingBackend ? "CLIENT_OK / RPC_STATUS_INTERNAL / empty payload"
                                                             : "CLIENT_OK / RPC_STATUS_OK / fixed backend response",
                    [&]
                    {
                        const auto response = runner.Send(a, Request(operation), delivery);
                        if (runner.GetMode() == Mode::MissingBackend)
                            ExpectRpc(response, RPC_STATUS_INTERNAL);
                        else if (operation == CRYPTO_OPERATION_GENERATE_RANDOM)
                            CheckRandom(runner, response, 32);
                        else
                            CheckFixed(Expect(response, operation), operation);
                    });
        if (runner.GetMode() == Mode::TestBackend)
        {
            runner.Log(
                "NOTE UnloadKey/FlushKeys validate routing and placeholder responses; test keys are not deleted.");
            for (const auto operation : { CRYPTO_OPERATION_IMPORT_KEY, CRYPTO_OPERATION_EXPORT_KEY })
                for (const auto format : { IMPORT_EXPORT_TYPE_CLEAR, IMPORT_EXPORT_TYPE_PROTECTED })
                    runner.Run(CryptoOperation_Name(operation) + "/" + ImportExportType_Name(format),
                        "explicit optional format accepted",
                        [&]
                        {
                            auto request = Request(operation);
                            if (operation == CRYPTO_OPERATION_IMPORT_KEY)
                                request.mutable_import_key()->set_import_export_type(format);
                            else
                                request.mutable_export_key()->set_import_export_type(format);
                            CheckFixed(Expect(runner.Send(a, request, Delivery::Async), operation), operation);
                        });
        }
    }

    void ConcurrentCases(Runner &runner, Connection &a, Connection &b)
    {
        runner.Run("mixed-sync-async-two-connections",
            "16 distinct responses matched to their callback slots exactly once",
            [&]
            {
                std::vector<Call> calls;
                for (size_t i = 0; i < AsyncSlots; ++i)
                {
                    auto request = Envelope(RandomRequest(17 + i * 13));
                    // EyilClient must overwrite these fields independently for every call.
                    request.set_request_id(999);
                    request.set_schema_version(999);
                    calls.push_back({ i % 2 ? &a : &b, request });
                }
                runner.Start(calls, true);
                // Read all accepted async responses before assertions or buffer reuse.
                const auto sync = runner.Sync(a, Envelope(RandomRequest(313)));
                const auto responses = runner.Finish();
                CheckRandom(runner, sync, 313);
                Check(responses.size() == calls.size(), "missing async result");
                for (size_t i = 0; i < responses.size(); ++i)
                    CheckRandom(runner, responses[i], 17 + i * 13);
            });
    }

    void BulkCases(Runner &runner, Connection &a, Connection &b)
    {
        if (runner.GetMode() == Mode::Exercise)
        {
            runner.Skip("bulk-data", "fixed XOR/checksum expectations require CryptoTestBackend");
            return;
        }
        if (runner.GetMode() == Mode::MissingBackend)
        {
            for (const auto operation :
                { CRYPTO_OPERATION_ENCRYPT, CRYPTO_OPERATION_DECRYPT, CRYPTO_OPERATION_SIGN, CRYPTO_OPERATION_VERIFY })
                for (const auto delivery : { Delivery::Sync, Delivery::Async })
                    runner.Run(CryptoOperation_Name(operation) + "/missing/" + Name(delivery),
                        "valid owned buffer, backend INTERNAL, local release still succeeds",
                        [&]
                        {
                            Buffer buffer(runner, a);
                            buffer.Map();
                            std::memset(buffer.Data(), 0, buffer.Size());
                            ExpectRpc(runner.Send(a, Request(operation, buffer.Id()), delivery), RPC_STATUS_INTERNAL);
                            buffer.Release(delivery);
                        });
            return;
        }
        for (const auto delivery : { Delivery::Sync, Delivery::Async })
            runner.Run(std::string("shared-memory-round-trip/") + Name(delivery),
                "XOR round trip; checksum; verify true/false; only output region changed",
                [&]
                {
                    const size_t count = delivery == Delivery::Async ? 4 : 1;
                    std::vector<std::unique_ptr<Buffer>> buffers;
                    std::vector<std::vector<uint8_t>> expected;
                    std::vector<BufferRegion> encrypted(count), signature(count);
                    for (size_t i = 0; i < count; ++i)
                    {
                        buffers.push_back(std::make_unique<Buffer>(runner, i % 2 ? b : a, 4096, delivery));
                        buffers.back()->Map();
                        expected.emplace_back(4096, 0xcc);
                        std::fill_n(expected.back().begin(), 16, 0);
                        for (size_t j = 0; j < 16 * (i + 1); ++j)
                            expected.back()[32 + j] = static_cast<uint8_t>(j + i);
                        std::memcpy(buffers.back()->Data(), expected.back().data(), 4096);
                    }
                    for (const auto operation : { CRYPTO_OPERATION_ENCRYPT, CRYPTO_OPERATION_DECRYPT,
                             CRYPTO_OPERATION_SIGN, CRYPTO_OPERATION_VERIFY })
                    {
                        runner.Log("STEP bulk operation=" + CryptoOperation_Name(operation) + " mode=" + Name(delivery)
                            + " independent_buffers=" + std::to_string(count));
                        std::vector<Call> calls;
                        for (size_t i = 0; i < count; ++i)
                        {
                            auto request = Request(operation, buffers[i]->Id());
                            const auto input = Region(32, 16 * (i + 1));
                            switch (operation)
                            {
                            case CRYPTO_OPERATION_ENCRYPT:
                                *request.mutable_encrypt()->mutable_input_data() = input;
                                break;
                            case CRYPTO_OPERATION_DECRYPT:
                                *request.mutable_decrypt()->mutable_encrypted_data() = encrypted[i];
                                break;
                            case CRYPTO_OPERATION_SIGN:
                                *request.mutable_sign()->mutable_input_data() = input;
                                break;
                            case CRYPTO_OPERATION_VERIFY:
                                *request.mutable_verify()->mutable_input_data() = input;
                                *request.mutable_verify()->mutable_expected_signature() = signature[i];
                                break;
                            default:
                                break;
                            }
                            calls.push_back({ &buffers[i]->Owner(), Envelope(request) });
                        }
                        std::vector<RpcResponse> responses;
                        if (delivery == Delivery::Async)
                        {
                            runner.Start(calls, true);
                            responses = runner.Finish();
                        }
                        else
                            responses.push_back(runner.Sync(a, calls.front().request));
                        for (size_t i = 0; i < count; ++i)
                        {
                            const auto response = Expect(responses[i], operation);
                            const uint32_t length = 16 * (i + 1);
                            BufferRegion output;
                            uint32_t offset = 0;
                            std::vector<uint8_t> bytes;
                            if (operation == CRYPTO_OPERATION_ENCRYPT || operation == CRYPTO_OPERATION_DECRYPT)
                            {
                                output = operation == CRYPTO_OPERATION_ENCRYPT ? response.encrypt().encrypted_data()
                                                                               : response.decrypt().output_data();
                                offset = operation == CRYPTO_OPERATION_ENCRYPT ? 32 + length
                                                                               : encrypted[i].offset() + length;
                                for (uint32_t j = 0; j < length; ++j)
                                    bytes.push_back(static_cast<uint8_t>(j + i)
                                        ^ (operation == CRYPTO_OPERATION_ENCRYPT ? 0xa5 : 0));
                                if (operation == CRYPTO_OPERATION_ENCRYPT)
                                    encrypted[i] = output;
                            }
                            else if (operation == CRYPTO_OPERATION_SIGN)
                            {
                                output = response.sign().signature();
                                offset = 32 + length;
                                bytes.assign(32, 0x5a);
                                for (uint32_t j = 0; j < length; ++j)
                                    bytes[j % 32] ^= static_cast<uint8_t>(j + i);
                                signature[i] = output;
                            }
                            else
                                Check(response.verify().verification_status(), "valid signature rejected");
                            if (!bytes.empty())
                            {
                                Check(output.offset() == offset && output.length() == bytes.size(),
                                    "unexpected output region");
                                std::copy(bytes.begin(), bytes.end(), expected[i].begin() + offset);
                            }
                            Check(std::memcmp(buffers[i]->Data(), expected[i].data(), expected[i].size()) == 0,
                                "buffer " + std::to_string(i) + ": incorrect output or unrelated region modified");
                        }
                    }
                    for (size_t i = 0; i < count; ++i)
                    {
                        buffers[i]->Data()[signature[i].offset()] ^= 1;
                        auto request = Request(CRYPTO_OPERATION_VERIFY, buffers[i]->Id());
                        *request.mutable_verify()->mutable_input_data() = Region(32, 16 * (i + 1));
                        *request.mutable_verify()->mutable_expected_signature() = signature[i];
                        const auto result
                            = Expect(runner.Send(buffers[i]->Owner(), request, delivery), CRYPTO_OPERATION_VERIFY);
                        Check(!result.verify().verification_status(), "modified signature accepted");
                    }
                    for (auto &buffer : buffers)
                        buffer->Release(delivery);
                });
    }
}
