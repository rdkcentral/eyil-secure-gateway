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
#include "../support/Owners.h"
#include "../support/InspectCrypto.h"
#include "CryptoServiceAdapter.h"
using namespace EyilTest;

namespace
{
    using C = WPEFramework::Exchange::ICrypto;
    constexpr uint32_t Key = 0xf1234567;
    const std::string Blob("a\0b\xff", 4);

    CryptoRequest Request(int operation, const std::string &id)
    {
        CryptoRequest r;
        r.set_operation(static_cast<CryptoOperation>(operation));
        switch (operation)
        {
        case CRYPTO_OPERATION_LOAD_KEY:
            r.mutable_load_key()->set_locator("test://key");
            r.mutable_load_key()->set_provision_type(PROVISION_TYPE_PRE_PROVISIONED_GLOBAL);
            break;
        case CRYPTO_OPERATION_KEY_EXISTS:
            r.mutable_key_exists()->set_locator("test://key");
            break;
        case CRYPTO_OPERATION_FLUSH_KEYS:
            r.mutable_flush_keys();
            break;
        case CRYPTO_OPERATION_SIGN:
        {
            auto *p = r.mutable_sign();
            p->set_buffer_id(id);
            p->set_key_id(Key);
            p->set_algorithm(SIGN_ALGORITHM_HMAC);
            p->set_digest_algorithm(DIGEST_ALGORITHM_SHA256);
            p->mutable_input_data()->set_offset(32);
            p->mutable_input_data()->set_length(16);
            break;
        }
        case CRYPTO_OPERATION_VERIFY:
        {
            auto *p = r.mutable_verify();
            p->set_buffer_id(id);
            p->set_key_id(Key);
            p->set_algorithm(SIGN_ALGORITHM_HMAC);
            p->set_digest_algorithm(DIGEST_ALGORITHM_SHA256);
            p->mutable_input_data()->set_offset(32);
            p->mutable_input_data()->set_length(16);
            p->mutable_expected_signature()->set_offset(64);
            p->mutable_expected_signature()->set_length(32);
            break;
        }
        case CRYPTO_OPERATION_GENERATE_KEY_PAIR:
        {
            auto *p = r.mutable_generate_key_pair();
            p->set_algorithm(KEY_PAIR_ALGORITHM_AUTH_DH);
            p->mutable_auth_dh()->set_prime_p(Blob);
            p->mutable_auth_dh()->set_generator_g(7);
            break;
        }
        case CRYPTO_OPERATION_DERIVE_SESSION_KEYS:
        {
            auto *p = r.mutable_derive_session_keys();
            p->set_algorithm(KEY_PAIR_ALGORITHM_AUTH_DH);
            p->set_private_key_id(Key);
            p->set_public_key_id(123);
            p->set_derivation_key_id(456);
            break;
        }
        case CRYPTO_OPERATION_GET_KEY_INFO:
            r.mutable_get_key_info()->set_key_id(Key);
            break;
        case CRYPTO_OPERATION_GENERATE_KEY:
            r.mutable_generate_key()->set_algorithm(GENERATE_KEY_ALGORITHM_AES256);
            r.mutable_generate_key()->add_key_usage(KEY_USAGE_ENCRYPT);
            break;
        case CRYPTO_OPERATION_UNLOAD_KEY:
            r.mutable_unload_key()->set_key_id(Key);
            break;
        case CRYPTO_OPERATION_IMPORT_KEY:
            r.mutable_import_key()->set_key_type(KEY_TYPE_PRIVATE);
            r.mutable_import_key()->set_key_blob(Blob);
            break;
        case CRYPTO_OPERATION_EXPORT_KEY:
            r.mutable_export_key()->set_key_id(Key);
            break;
        case CRYPTO_OPERATION_GENERATE_RANDOM:
            r.mutable_generate_random()->set_number_of_bytes(27);
            break;
        case CRYPTO_OPERATION_ENCRYPT:
        {
            auto *p = r.mutable_encrypt();
            p->set_buffer_id(id);
            p->set_key_id(Key);
            p->set_algorithm(CIPHER_ALGORITHM_AES_CBC);
            p->mutable_init_data()->set_length(16);
            p->mutable_input_data()->set_offset(32);
            p->mutable_input_data()->set_length(16);
            break;
        }
        case CRYPTO_OPERATION_DECRYPT:
        {
            auto *p = r.mutable_decrypt();
            p->set_buffer_id(id);
            p->set_key_id(Key);
            p->set_algorithm(CIPHER_ALGORITHM_AES_CBC);
            p->mutable_init_data()->set_length(16);
            p->mutable_encrypted_data()->set_offset(32);
            p->mutable_encrypted_data()->set_length(16);
            break;
        }
        case CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER:
            r.mutable_allocate_shared_buffer()->set_requested_size(2048);
            break;
        case CRYPTO_OPERATION_RELEASE_SHARED_BUFFER:
            r.mutable_release_shared_buffer()->set_buffer_id(id);
            break;
        }
        return r;
    }

    struct Fixture
    {
        Eyil::Diagnostics log { "adapter-unit", IgnoreLog };
        Eyil::SharedBuffers buffers { log };
        InspectCrypto crypto;
        Eyil::CryptoServiceAdapter adapter { [this] { return crypto.Acquire(); }, buffers, log };
        std::shared_ptr<Eyil::Connection> owner = Owner("adapter");
        Eyil::SharedBuffers::Allocation allocation;

        Fixture()
        {
            CHECK(adapter.Notify(*owner, true));
            allocation = buffers.Allocate(owner, 4096);
            CHECK(allocation.error == CRYPTO_ERROR_NONE);
        }

        ~Fixture()
        {
            crypto.onRelease = {};
            allocationCountdown = -1;
            adapter.Notify(*owner, false);
            buffers.Cleanup(owner->userId);
            // An injected uncertain invocation has no real backend work. Remove
            // only this test's known object after asserting it remained quarantined.
            unlink(("/dev/shm" + allocation.name).c_str());
        }

        Eyil::ServiceResult Execute(const CryptoRequest &request)
        {
            return adapter.Execute(owner, request.SerializeAsString());
        }

        CryptoResponse Service(const CryptoRequest &request)
        {
            auto result = Execute(request);
            CHECK(result.status == RPC_STATUS_OK);
            CryptoResponse response;
            CHECK(response.ParseFromString(result.payload));
            CHECK(static_cast<int>(response.payload_case()) == static_cast<int>(request.operation()) + 1);
            CHECK(crypto.references == 1);
            return response;
        }

        void Invalid(const CryptoRequest &request)
        {
            auto calls = crypto.calls.load();
            auto result = Execute(request);
            CHECK(result.status == RPC_STATUS_INTERNAL && result.payload.empty());
            CHECK(crypto.calls == calls);
        }
    };

    void Arguments()
    {
        Fixture f;
        for (int op = 1; op <= 17; ++op)
        {
            auto request = Request(op, f.allocation.id);
            const auto queries = f.crypto.queries.load();
            auto response = f.Service(request);
            CHECK(response.error() == CRYPTO_ERROR_NONE);
            if (op >= 16)
            {
                CHECK(f.crypto.queries == queries);
                continue;
            }
            const auto &call = f.crypto.last;
            CHECK(call.operation == op && call.context.appId == f.owner->appId
                && call.context.sessionId == f.owner->sessionId);
            if (op == CRYPTO_OPERATION_LOAD_KEY || op == CRYPTO_OPERATION_KEY_EXISTS)
                CHECK(call.text == "test://key");
            if (op == CRYPTO_OPERATION_LOAD_KEY)
                CHECK(call.algorithm == C::PROVISION_TYPE_PRE_PROVISIONED_GLOBAL);
            if (op == CRYPTO_OPERATION_GENERATE_KEY_PAIR)
            {
                CHECK(call.algorithm == C::KEY_PAIR_ALGORITHM_AUTH_DH);
                CHECK(call.number == 7);
                CHECK(std::string(call.bytes.begin(), call.bytes.end()) == Blob);
            }
            if (op == CRYPTO_OPERATION_DERIVE_SESSION_KEYS)
            {
                CHECK(call.key == Key && call.secondKey == 123 && call.thirdKey == 456);
                CHECK(call.algorithm == C::KEY_PAIR_ALGORITHM_AUTH_DH);
            }
            if (op == CRYPTO_OPERATION_IMPORT_KEY)
            {
                CHECK(call.algorithm == C::KEY_TYPE_PRIVATE && !call.optional);
                CHECK(std::string(call.bytes.begin(), call.bytes.end()) == Blob);
            }
            if (op == CRYPTO_OPERATION_EXPORT_KEY)
                CHECK(!call.optional && call.key == Key);
            if (op == CRYPTO_OPERATION_UNLOAD_KEY || op == CRYPTO_OPERATION_GET_KEY_INFO)
                CHECK(call.key == Key);
            if (op == CRYPTO_OPERATION_GENERATE_KEY)
            {
                CHECK(call.algorithm == C::GENERATE_KEY_ALGORITHM_AES256);
                CHECK(call.usages == std::vector<C::KeyUsage> { C::KEY_USAGE_ENCRYPT });
            }
            if (op == CRYPTO_OPERATION_GENERATE_RANDOM)
                CHECK(call.number == 27);
            if (op == 4 || op == 5 || op == 14 || op == 15)
            {
                CHECK(call.key == Key
                    && call.path == "/proc/" + std::to_string(getpid()) + "/root/dev/shm" + f.allocation.name);
                if (op == 4 || op == 5)
                {
                    CHECK(call.first.offset == 32 && call.first.length == 16);
                    CHECK(call.algorithm == C::SIGN_ALGORITHM_HMAC && call.digest == C::DIGEST_ALGORITHM_SHA256);
                }
                if (op == 5)
                    CHECK(call.second.offset == 64 && call.second.length == 32);
                if (op == 14 || op == 15)
                {
                    CHECK(call.first.offset == 0 && call.first.length == 16);
                    CHECK(call.second.offset == 32 && call.second.length == 16);
                    CHECK(call.algorithm == C::CIPHER_ALGORITHM_AES_CBC);
                }
            }
        }
    }

    void EnumMappings()
    {
        Fixture f;
        const std::pair<GenerateKeyAlgorithm, C::GenerateKeyAlgorithm> algorithms[]
            = { { GENERATE_KEY_ALGORITHM_AES128, C::GENERATE_KEY_ALGORITHM_AES128 },
                  { GENERATE_KEY_ALGORITHM_AES256, C::GENERATE_KEY_ALGORITHM_AES256 },
                  { GENERATE_KEY_ALGORITHM_HMAC128, C::GENERATE_KEY_ALGORITHM_HMAC128 },
                  { GENERATE_KEY_ALGORITHM_HMAC160, C::GENERATE_KEY_ALGORITHM_HMAC160 },
                  { GENERATE_KEY_ALGORITHM_HMAC256, C::GENERATE_KEY_ALGORITHM_HMAC256 } };
        const std::pair<KeyUsage, C::KeyUsage> usages[] = { { KEY_USAGE_ENCRYPT, C::KEY_USAGE_ENCRYPT },
            { KEY_USAGE_DECRYPT, C::KEY_USAGE_DECRYPT }, { KEY_USAGE_SIGN, C::KEY_USAGE_SIGN },
            { KEY_USAGE_VERIFY, C::KEY_USAGE_VERIFY }, { KEY_USAGE_DERIVE, C::KEY_USAGE_DERIVE },
            { KEY_USAGE_WRAP, C::KEY_USAGE_WRAP }, { KEY_USAGE_UNWRAP, C::KEY_USAGE_UNWRAP } };
        for (auto a : algorithms)
        {
            auto r = Request(9, f.allocation.id);
            r.mutable_generate_key()->set_algorithm(a.first);
            r.mutable_generate_key()->clear_key_usage();
            for (auto u : usages)
                r.mutable_generate_key()->add_key_usage(u.first);
            CHECK(f.Service(r).error() == CRYPTO_ERROR_NONE);
            CHECK(f.crypto.last.algorithm == a.second);
            for (size_t i = 0; i < std::size(usages); ++i)
                CHECK(f.crypto.last.usages[i] == usages[i].second);
        }
        for (auto type : { KEY_TYPE_PUBLIC, KEY_TYPE_PRIVATE, KEY_TYPE_SYMMETRIC })
        {
            auto r = Request(11, f.allocation.id);
            r.mutable_import_key()->set_key_type(type);
            CHECK(f.Service(r).error() == CRYPTO_ERROR_NONE);
            CHECK(f.crypto.last.algorithm == static_cast<unsigned>(type));
            f.crypto.infoType = static_cast<C::KeyType>(type);
            CHECK(f.Service(Request(8, f.allocation.id)).get_key_info().key_type() == type);
        }
        for (int algorithm = 1; algorithm <= 7; ++algorithm)
        {
            f.crypto.infoAlgorithm = static_cast<C::KeyAlgorithm>(algorithm);
            f.crypto.infoUsages.clear();
            for (auto u : usages)
                f.crypto.infoUsages.push_back(u.second);
            auto r = f.Service(Request(8, f.allocation.id));
            CHECK(r.get_key_info().key_algorithm() == algorithm);
            for (size_t i = 0; i < std::size(usages); ++i)
                CHECK(r.get_key_info().key_usage(i) == usages[i].first);
        }
    }

    void OptionalPresence()
    {
        Fixture f;
        for (int op : { 11, 12 })
            for (int mode : { -1, 0, 1, 2, 99 })
            {
                auto r = Request(op, f.allocation.id);
                if (mode >= 0)
                {
                    if (op == 11)
                        r.mutable_import_key()->set_import_export_type(static_cast<ImportExportType>(mode));
                    else
                        r.mutable_export_key()->set_import_export_type(static_cast<ImportExportType>(mode));
                }
                if (mode == 0 || mode == 99)
                {
                    f.Invalid(r);
                    continue;
                }
                CHECK(f.Service(r).error() == CRYPTO_ERROR_NONE);
                CHECK(f.crypto.last.optional == (mode >= 0));
                if (mode >= 0)
                    CHECK(f.crypto.last.format == static_cast<unsigned>(mode));
            }
    }

    void InvalidEnums()
    {
        Fixture f;
        for (int op = 1; op <= 17; ++op)
            for (int bad : { 0, 99, -1 })
            {
                auto base = Request(op, f.allocation.id);
                const auto *field = base.GetDescriptor()->FindFieldByNumber(op + 1);
                auto *payload = base.GetReflection()->MutableMessage(&base, field);
                for (int i = 0; i < payload->GetDescriptor()->field_count(); ++i)
                {
                    const auto *member = payload->GetDescriptor()->field(i);
                    if (member->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_ENUM)
                        continue;
                    auto r = base;
                    auto *p = r.GetReflection()->MutableMessage(&r, field);
                    if (member->is_repeated())
                    {
                        p->GetReflection()->ClearField(p, member);
                        p->GetReflection()->AddEnumValue(p, member, bad);
                    }
                    else
                        p->GetReflection()->SetEnumValue(p, member, bad);
                    f.Invalid(r);
                }
            }
    }

    void OneofAndParsing()
    {
        Fixture f;
        for (int body = 1; body <= 17; ++body)
            for (int op = 0; op <= 18; ++op)
            {
                if (op == body)
                    continue;
                auto r = Request(body, f.allocation.id);
                r.set_operation(static_cast<CryptoOperation>(op));
                f.Invalid(r);
            }
        for (int op = 0; op <= 18; ++op)
        {
            CryptoRequest r;
            r.set_operation(static_cast<CryptoOperation>(op));
            f.Invalid(r);
        }
        CHECK(f.adapter.Execute(f.owner, std::string("\x80", 1)).status == RPC_STATUS_INTERNAL);
        CHECK(f.adapter.Execute(f.owner, "").status == RPC_STATUS_INTERNAL);
    }

    void MissingParameters()
    {
        Fixture f;
        auto r = Request(1, f.allocation.id);
        r.mutable_load_key()->clear_locator();
        f.Invalid(r);
        r = Request(2, f.allocation.id);
        r.mutable_key_exists()->clear_locator();
        f.Invalid(r);
        r = Request(6, f.allocation.id);
        r.mutable_generate_key_pair()->clear_parameters();
        f.Invalid(r);
        r = Request(6, f.allocation.id);
        r.mutable_generate_key_pair()->mutable_auth_dh()->clear_prime_p();
        f.Invalid(r);
        for (unsigned g : { 0, 1 })
        {
            r = Request(6, f.allocation.id);
            r.mutable_generate_key_pair()->mutable_auth_dh()->set_generator_g(g);
            f.Invalid(r);
        }
        r = Request(9, f.allocation.id);
        r.mutable_generate_key()->clear_key_usage();
        f.Invalid(r);
        r = Request(9, f.allocation.id);
        r.mutable_generate_key()->add_key_usage(KEY_USAGE_ENCRYPT);
        f.Invalid(r);
        r = Request(11, f.allocation.id);
        r.mutable_import_key()->clear_key_blob();
        f.Invalid(r);
        r = Request(13, f.allocation.id);
        r.mutable_generate_random()->set_number_of_bytes(0);
        f.Invalid(r);
        for (int op : { 4, 5, 14, 15 })
        {
            auto base = Request(op, f.allocation.id);
            const auto *field = base.GetDescriptor()->FindFieldByNumber(op + 1);
            const auto &p = base.GetReflection()->GetMessage(base, field);
            for (int i = 0; i < p.GetDescriptor()->field_count(); ++i)
            {
                const auto *region = p.GetDescriptor()->field(i);
                if (region->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
                    continue;
                r = base;
                auto *payload = r.GetReflection()->MutableMessage(&r, field);
                payload->GetReflection()->ClearField(payload, region);
                CHECK(f.Service(r).error() == CRYPTO_ERROR_INVALID_PARAMETERS);
            }
        }
    }

    void ServiceErrors()
    {
        Fixture f;
        for (int error = 0; error <= 13; ++error)
            for (int op = 1; op <= 15; ++op)
            {
                f.crypto.serviceError = static_cast<C::CryptoError>(error);
                const auto queries = f.crypto.queries.load(), released = f.crypto.releases.load();
                CHECK(f.Service(Request(op, f.allocation.id)).error() == error);
                CHECK(f.crypto.queries == queries + 1 && f.crypto.releases == released + 1);
            }
        f.crypto.serviceError = static_cast<C::CryptoError>(250);
        CHECK(f.Execute(Request(13, f.allocation.id)).status == RPC_STATUS_INTERNAL);
        CHECK(f.crypto.references == 1);
    }

    void InvocationErrors()
    {
        Fixture f;
        auto r = Request(13, f.allocation.id);
        f.crypto.available = false;
        CHECK(f.Execute(r).status == RPC_STATUS_INTERNAL);
        CHECK(f.crypto.references == 1);
        f.crypto.available = true;
        f.crypto.invocation = WPEFramework::Core::ERROR_GENERAL;
        CHECK(f.Execute(r).status == RPC_STATUS_INTERNAL);
        CHECK(f.crypto.references == 1);
        f.crypto.invocation = WPEFramework::Core::ERROR_NONE;
        f.crypto.throwCall = true;
        CHECK(f.Execute(r).status == RPC_STATUS_INTERNAL);
        CHECK(f.crypto.references == 1);
        f.crypto.throwCall = false;
        const auto wire = r.SerializeAsString();
        f.crypto.onRelease = [] { allocationCountdown = 0; };
        auto result = f.adapter.Execute(f.owner, wire);
        allocationCountdown = -1;
        f.crypto.onRelease = {};
        CHECK(result.status == RPC_STATUS_INTERNAL && f.crypto.references == 1);
        CHECK(f.Service(r).error() == CRYPTO_ERROR_NONE);
    }

    void BackendFailures(int failure)
    {
        // Invoke is instantiated separately for each ICrypto method. Verify
        // reference ownership and failure handling for every dispatch path.
        for (int operation = 1; operation <= 15; ++operation)
        {
            Fixture f;
            f.crypto.available = failure != 0;
            f.crypto.invocation = failure == 1 ? WPEFramework::Core::ERROR_GENERAL : WPEFramework::Core::ERROR_NONE;
            f.crypto.throwCall = failure == 2;
            const auto released = f.crypto.releases.load();
            const auto result = f.Execute(Request(operation, f.allocation.id));
            CHECK(result.status == RPC_STATUS_INTERNAL && result.payload.empty());
            CHECK(f.crypto.references == 1);
            CHECK(f.crypto.releases == released + (failure == 0 ? 0 : 1));
            CHECK(f.buffers.Idle(f.owner->userId));
            CHECK(f.buffers.Cleanup(f.owner->userId));
            CHECK(f.buffers.Charged(getuid()) == 0);
        }
    }

    void CompletedFailureRelease(bool exception)
    {
        for (int operation : { 4, 5, 14, 15 })
        {
            Fixture f;
            for (unsigned n = 1; n < Eyil::SharedBuffers::MaximumCount; ++n)
                CHECK(f.buffers.Allocate(f.owner, 4096).error == CRYPTO_ERROR_NONE);
            CHECK(f.buffers.Charged(getuid()) == Eyil::SharedBuffers::MaximumCount);
            CHECK(f.buffers.Allocate(f.owner, 4096).error == CRYPTO_ERROR_OPERATION_FAILED);

            bool pending = false;
            f.crypto.onRelease = [&]
            {
                pending = f.buffers.Release(f.owner, f.allocation.id) == CRYPTO_ERROR_NONE
                    && access(("/dev/shm" + f.allocation.name).c_str(), F_OK) == 0
                    && f.buffers.Charged(getuid()) == Eyil::SharedBuffers::MaximumCount;
            };
            f.crypto.throwCall = exception;
            f.crypto.invocation = exception ? WPEFramework::Core::ERROR_NONE : WPEFramework::Core::ERROR_GENERAL;
            auto result = f.Execute(Request(operation, f.allocation.id));
            f.crypto.onRelease = {};
            CHECK(result.status == RPC_STATUS_INTERNAL && result.payload.empty());
            CHECK(pending && f.crypto.references == 1 && f.buffers.Idle(f.owner->userId));
            CHECK(access(("/dev/shm" + f.allocation.name).c_str(), F_OK) != 0);
            CHECK(f.buffers.Charged(getuid()) == Eyil::SharedBuffers::MaximumCount - 1);
            CHECK(f.buffers.Allocate(f.owner, 4096).error == CRYPTO_ERROR_NONE);
        }
    }

    void BufferRejections()
    {
        Fixture f;
        for (int operation : { 4, 5, 14, 15 })
        {
            const auto calls = f.crypto.calls.load();
            CHECK(f.Service(Request(operation, "unknown-buffer")).error() == CRYPTO_ERROR_INVALID_SESSION_STATE);
            Eyil::SharedBuffers::Lease lease;
            const Eyil::SharedBuffers::Region input { 0, 16 };
            CHECK(f.buffers.TryUse(f.owner, f.allocation.id, &input, 1, lease) == CRYPTO_ERROR_NONE);
            CHECK(f.Service(Request(operation, f.allocation.id)).error() == CRYPTO_ERROR_INVALID_SESSION_STATE);
            CHECK(f.crypto.calls == calls);
        }
    }

    void BadBackendOutputs()
    {
        Fixture f;
        auto r = Request(8, f.allocation.id);
        f.crypto.infoType = static_cast<C::KeyType>(99);
        CHECK(f.Execute(r).status == RPC_STATUS_INTERNAL);
        f.crypto.infoType = C::KEY_TYPE_PUBLIC;
        f.crypto.infoAlgorithm = static_cast<C::KeyAlgorithm>(99);
        CHECK(f.Execute(r).status == RPC_STATUS_INTERNAL);
        f.crypto.infoAlgorithm = C::KEY_ALGORITHM_AES_CBC;
        f.crypto.infoUsages = { static_cast<C::KeyUsage>(99) };
        CHECK(f.Execute(r).status == RPC_STATUS_INTERNAL);
        f.crypto.wrongRandomSize = true;
        CHECK(f.Execute(Request(13, f.allocation.id)).status == RPC_STATUS_INTERNAL);
        f.crypto.existsResult = false;
        auto exists = f.Service(Request(2, f.allocation.id));
        CHECK(!exists.key_exists().exists() && exists.key_exists().key_id() == 0);
        f.crypto.verificationResult = false;
        CHECK(!f.Service(Request(5, f.allocation.id)).verify().verification_status());
    }

    void OutputRegions()
    {
        Fixture f;
        f.crypto.overrideRegion = true;
        for (int op : { 4, 14, 15 })
        {
            auto r = Request(op, f.allocation.id);
            for (C::BufferRegion bad :
                { C::BufferRegion { 32, 1 }, C::BufferRegion { 4095, 2 }, C::BufferRegion { UINT32_MAX, 16 } })
            {
                f.crypto.outputRegion = bad;
                CHECK(f.Execute(r).status == RPC_STATUS_INTERNAL);
                CHECK(f.buffers.Idle(f.owner->userId));
            }
            f.crypto.outputRegion = { 4096, 0 };
            CHECK(f.Service(r).error() == CRYPTO_ERROR_NONE);
            f.crypto.outputRegion = { 4096, 16 };
            f.crypto.serviceError = C::CRYPTO_ERROR_BUFFER_OVERFLOW;
            auto response = f.Service(r);
            CHECK(response.error() == CRYPTO_ERROR_BUFFER_OVERFLOW);
            const auto &payload
                = response.GetReflection()->GetMessage(response, response.GetDescriptor()->FindFieldByNumber(op + 1));
            const auto &region = payload.GetReflection()->GetMessage(payload, payload.GetDescriptor()->field(0));
            CHECK(region.GetReflection()->GetUInt32(region, region.GetDescriptor()->FindFieldByName("offset")) == 4096);
            CHECK(region.GetReflection()->GetUInt32(region, region.GetDescriptor()->FindFieldByName("length")) == 16);
            f.crypto.serviceError = C::CRYPTO_ERROR_NONE;
        }
    }

    void Quarantine()
    {
        Fixture f;
        f.crypto.invocation = COM_ERROR | WPEFramework::Core::ERROR_TIMEDOUT;
        CHECK(f.Execute(Request(4, f.allocation.id)).status == RPC_STATUS_INTERNAL);
        CHECK(f.crypto.references == 1 && !f.buffers.Idle(f.owner->userId));
        CHECK(f.buffers.Release(f.owner, f.allocation.id) == CRYPTO_ERROR_NONE);
        CHECK(!f.buffers.Cleanup(f.owner->userId) && f.buffers.Charged(getuid()) == 1);
        CHECK(access(("/dev/shm" + f.allocation.name).c_str(), F_OK) == 0);
    }

    void LocalWithoutBackend()
    {
        Fixture f;
        f.crypto.available = false;
        const auto queries = f.crypto.queries.load();
        auto a = f.Service(Request(16, f.allocation.id));
        CHECK(a.error() == CRYPTO_ERROR_NONE);
        auto release = Request(17, a.allocate_shared_buffer().buffer_id());
        CHECK(f.Service(release).error() == CRYPTO_ERROR_NONE);
        CHECK(f.crypto.queries == queries);
        CHECK(!f.adapter.Notify(*f.owner, true));
        f.crypto.available = true;
        f.crypto.serviceError = C::CRYPTO_ERROR_OPERATION_FAILED;
        CHECK(!f.adapter.Notify(*f.owner, true));
        CHECK(f.crypto.references == 1);
    }
}

int main(int argc, char **argv)
{
    return RunCases(argc, argv,
        { { "arguments", Arguments }, { "enum-mappings", EnumMappings }, { "optional-presence", OptionalPresence },
            { "invalid-enums", InvalidEnums }, { "oneof-parsing", OneofAndParsing },
            { "missing-parameters", MissingParameters }, { "service-errors", ServiceErrors },
            { "invocation-errors", InvocationErrors }, { "bad-backend-outputs", BadBackendOutputs },
            { "lookup-all-operations", [] { BackendFailures(0); } },
            { "invocation-all-operations", [] { BackendFailures(1); } },
            { "exception-all-operations", [] { BackendFailures(2); } }, { "buffer-rejections", BufferRejections },
            { "completed-error-release", [] { CompletedFailureRelease(false); } },
            { "completed-exception-release", [] { CompletedFailureRelease(true); } },
            { "output-regions", OutputRegions }, { "uncertain-access", Quarantine },
            { "local-without-backend", LocalWithoutBackend } });
}
