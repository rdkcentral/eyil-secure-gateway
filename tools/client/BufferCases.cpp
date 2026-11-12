// Copyright 2026 RDK Management
// SPDX-License-Identifier: Apache-2.0
#include "TestClient.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace EyilDev
{
    void AllocationCases(Runner &runner, Connection &a, Connection &b)
    {
        for (const auto delivery : { Delivery::Sync, Delivery::Async })
            for (const uint32_t size : { 0u, 1u, MaximumBufferSize, MaximumBufferSize + 1 })
                runner.Run("allocation/" + std::to_string(size) + "/" + Name(delivery),
                    size == 0                      ? "INVALID_PARAMETERS"
                        : size > MaximumBufferSize ? "OVERSIZED_INPUT_DATA"
                                                   : "allocation, exact size, mapping, release",
                    [&]
                    {
                        if (size == 0 || size > MaximumBufferSize)
                            Expect(runner.Send(a, AllocateRequest(size), delivery),
                                CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER,
                                size == 0 ? CRYPTO_ERROR_INVALID_PARAMETERS : CRYPTO_ERROR_OVERSIZED_INPUT_DATA);
                        else
                        {
                            Buffer buffer(runner, a, size, delivery);
                            buffer.Map();
                            // Touch both ends, without writing outside the actual allocation.
                            buffer.Data()[0] = 0x3a;
                            buffer.Data()[size - 1] = 0x7c;
                            Check(buffer.Data()[size - 1] == 0x7c, "last allocation byte is not accessible");
                            buffer.Release(delivery);
                        }
                    });
        runner.Run("shared-UID-quota-and-reuse", "four buffers across A/B; fifth fails; release restores one slot",
            [&]
            {
                std::vector<std::unique_ptr<Buffer>> buffers;
                for (unsigned i = 0; i < 4; ++i)
                    buffers.push_back(std::make_unique<Buffer>(runner, i % 2 ? b : a));
                for (auto *connection : { &a, &b })
                    Expect(runner.Send(*connection, AllocateRequest(4096), Delivery::Async),
                        CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER, CRYPTO_ERROR_OPERATION_FAILED);
                buffers.front()->Release();
                Buffer replacement(runner, b, 4096, Delivery::Async);
                for (auto &buffer : buffers)
                    if (!buffer->Id().empty())
                        buffer->Release();
                replacement.Release(Delivery::Async);
            });
    }

    void OwnershipCases(Runner &runner, Connection &a, Connection &b)
    {
        for (const auto delivery : { Delivery::Sync, Delivery::Async })
            runner.Run(std::string("buffer-ownership/") + Name(delivery),
                "foreign use/release rejected, original owner retains its buffer",
                [&]
                {
                    Buffer buffer(runner, a, 128);
                    buffer.Map();
                    std::memset(buffer.Data(), 0x22, buffer.Size());
                    auto use = Request(CRYPTO_OPERATION_SIGN, buffer.Id());
                    Expect(runner.Send(b, use, delivery), CRYPTO_OPERATION_SIGN, CRYPTO_ERROR_INVALID_SESSION_STATE);
                    Expect(runner.Send(b, ReleaseRequest(buffer.Id()), delivery),
                        CRYPTO_OPERATION_RELEASE_SHARED_BUFFER, CRYPTO_ERROR_INVALID_SESSION_STATE);
                    if (runner.GetMode() != Mode::Exercise)
                    {
                        const auto reply = runner.Send(a, use, delivery);
                        if (runner.GetMode() == Mode::MissingBackend)
                            ExpectRpc(reply, RPC_STATUS_INTERNAL);
                        else
                            Expect(reply, CRYPTO_OPERATION_SIGN);
                    }
                    const auto oldId = buffer.Id();
                    buffer.Release(delivery);
                    Expect(runner.Send(a, ReleaseRequest(oldId), delivery), CRYPTO_OPERATION_RELEASE_SHARED_BUFFER,
                        CRYPTO_ERROR_INVALID_SESSION_STATE);
                    Expect(runner.Send(a, use, delivery), CRYPTO_OPERATION_SIGN, CRYPTO_ERROR_INVALID_SESSION_STATE);
                });
        runner.Run("unknown-buffer", "unknown ID rejected for use and release",
            [&]
            {
                const std::string unknown = "eyil-diagnostic-nonexistent-buffer";
                Expect(runner.Send(a, Request(CRYPTO_OPERATION_SIGN, unknown), Delivery::Async), CRYPTO_OPERATION_SIGN,
                    CRYPTO_ERROR_INVALID_SESSION_STATE);
                Expect(runner.Send(a, ReleaseRequest(unknown), Delivery::Sync), CRYPTO_OPERATION_RELEASE_SHARED_BUFFER,
                    CRYPTO_ERROR_INVALID_SESSION_STATE);
            });
    }

    void RegionCases(Runner &runner, Connection &a)
    {
        struct InvalidRegion
        {
            const char *name;
            uint32_t offset;
            uint32_t length;
        };

        for (const auto region : { InvalidRegion { "end-plus-one", 63, 2 }, { "offset-past-end", 65, 0 },
                 { "offset-overflow", std::numeric_limits<uint32_t>::max(), 2 },
                 { "length-overflow", 1, std::numeric_limits<uint32_t>::max() } })
            runner.Run(std::string("region/") + region.name, "INVALID_PARAMETERS; no buffer mutation",
                [&]
                {
                    Buffer buffer(runner, a, 64);
                    buffer.Map();
                    std::memset(buffer.Data(), 0xcc, 64);
                    auto request = Request(CRYPTO_OPERATION_SIGN, buffer.Id());
                    *request.mutable_sign()->mutable_input_data() = Region(region.offset, region.length);
                    Expect(runner.Send(a, request, Delivery::Async), CRYPTO_OPERATION_SIGN,
                        CRYPTO_ERROR_INVALID_PARAMETERS);
                    Check(std::all_of(buffer.Data(), buffer.Data() + 64, [](uint8_t value) { return value == 0xcc; }),
                        "rejected region changed shared memory");
                });
        for (const auto operation :
            { CRYPTO_OPERATION_ENCRYPT, CRYPTO_OPERATION_DECRYPT, CRYPTO_OPERATION_SIGN, CRYPTO_OPERATION_VERIFY })
            runner.Run("missing-region/" + CryptoOperation_Name(operation), "INVALID_PARAMETERS",
                [&]
                {
                    Buffer buffer(runner, a, 128);
                    auto request = Request(operation, buffer.Id());
                    switch (operation)
                    {
                    case CRYPTO_OPERATION_ENCRYPT:
                        request.mutable_encrypt()->clear_input_data();
                        break;
                    case CRYPTO_OPERATION_DECRYPT:
                        request.mutable_decrypt()->clear_encrypted_data();
                        break;
                    case CRYPTO_OPERATION_SIGN:
                        request.mutable_sign()->clear_input_data();
                        break;
                    case CRYPTO_OPERATION_VERIFY:
                        request.mutable_verify()->clear_expected_signature();
                        break;
                    default:
                        break;
                    }
                    Expect(runner.Send(a, request, Delivery::Sync), operation, CRYPTO_ERROR_INVALID_PARAMETERS);
                });
        for (const auto operation : { CRYPTO_OPERATION_ENCRYPT, CRYPTO_OPERATION_DECRYPT, CRYPTO_OPERATION_VERIFY })
            runner.Run("overlap/" + CryptoOperation_Name(operation), "INVALID_PARAMETERS",
                [&]
                {
                    Buffer buffer(runner, a, 128);
                    auto request = Request(operation, buffer.Id());
                    if (operation == CRYPTO_OPERATION_ENCRYPT)
                        *request.mutable_encrypt()->mutable_input_data() = Region(8, 16);
                    else if (operation == CRYPTO_OPERATION_DECRYPT)
                        *request.mutable_decrypt()->mutable_encrypted_data() = Region(8, 16);
                    else
                        *request.mutable_verify()->mutable_expected_signature() = Region(40, 32);
                    Expect(runner.Send(a, request, Delivery::Async), operation, CRYPTO_ERROR_INVALID_PARAMETERS);
                });
        if (runner.GetMode() != Mode::TestBackend)
        {
            runner.Skip("output-placement-and-invalid-key", "requires deterministic backend outputs");
            return;
        }
        for (const auto &input : { Region(0, 32), Region(3, 20), Region(0, 0) })
            runner.Run("signature-placement/" + std::to_string(input.offset()) + "/" + std::to_string(input.length()),
                "16-byte alignment; output within capacity; untouched bytes preserved",
                [&]
                {
                    Buffer buffer(runner, a, 64);
                    buffer.Map();
                    std::vector<uint8_t> expected(64, 0x33);
                    std::memcpy(buffer.Data(), expected.data(), expected.size());
                    auto request = Request(CRYPTO_OPERATION_SIGN, buffer.Id());
                    *request.mutable_sign()->mutable_input_data() = input;
                    const auto response = Expect(runner.Send(a, request, Delivery::Async), CRYPTO_OPERATION_SIGN);
                    const auto &output = response.sign().signature();
                    const auto offset = (input.offset() + input.length() + 15) & ~15u;
                    Check(output.offset() == offset && output.length() == 32, "unexpected aligned output region");
                    std::vector<uint8_t> checksum(32, 0x5a);
                    for (uint32_t i = 0; i < input.length(); ++i)
                        checksum[i % 32] ^= 0x33;
                    std::copy(checksum.begin(), checksum.end(), expected.begin() + offset);
                    Check(std::memcmp(buffer.Data(), expected.data(), expected.size()) == 0,
                        "incorrect checksum or extra writes");
                });
        runner.Run("output-overflow", "required region forwarded; no readable output or buffer mutation",
            [&]
            {
                Buffer buffer(runner, a, 64);
                buffer.Map();
                std::memset(buffer.Data(), 0x33, 64);
                auto request = Request(CRYPTO_OPERATION_SIGN, buffer.Id());
                *request.mutable_sign()->mutable_input_data() = Region(0, 33);
                const auto response = Expect(
                    runner.Send(a, request, Delivery::Async), CRYPTO_OPERATION_SIGN, CRYPTO_ERROR_BUFFER_OVERFLOW);
                Check(response.sign().signature().offset() == 48 && response.sign().signature().length() == 32,
                    "incorrect required output capacity");
                Check(std::all_of(buffer.Data(), buffer.Data() + 64, [](uint8_t value) { return value == 0x33; }),
                    "overflow wrote partial output");
                // The returned region describes capacity; never dereference it here.
            });
        for (const auto operation : { CRYPTO_OPERATION_GET_KEY_INFO, CRYPTO_OPERATION_UNLOAD_KEY,
                 CRYPTO_OPERATION_EXPORT_KEY, CRYPTO_OPERATION_SIGN })
            runner.Run("invalid-key/" + CryptoOperation_Name(operation), "CLIENT_OK / RPC_OK / INVALID_KEY_ID",
                [&]
                {
                    Buffer buffer(runner, a, 128);
                    auto request = Request(operation, buffer.Id());
                    switch (operation)
                    {
                    case CRYPTO_OPERATION_GET_KEY_INFO:
                        request.mutable_get_key_info()->set_key_id(999);
                        break;
                    case CRYPTO_OPERATION_UNLOAD_KEY:
                        request.mutable_unload_key()->set_key_id(999);
                        break;
                    case CRYPTO_OPERATION_EXPORT_KEY:
                        request.mutable_export_key()->set_key_id(999);
                        break;
                    case CRYPTO_OPERATION_SIGN:
                        request.mutable_sign()->set_key_id(999);
                        break;
                    default:
                        break;
                    }
                    Expect(runner.Send(a, request, Delivery::Async), operation, CRYPTO_ERROR_INVALID_KEY_ID);
                });
    }
}
