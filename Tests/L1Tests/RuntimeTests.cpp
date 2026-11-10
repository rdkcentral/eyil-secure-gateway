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
#include "CryptoServiceAdapter.h"
#include <eyilclient/EyilClient.h>
#include <future>

namespace
{
    std::atomic<unsigned> lines { 0 };

    void Count(const char *) { ++lines; }

    std::shared_ptr<Eyil::Connection> Owner(const char *id)
    {
        auto owner = std::make_shared<Eyil::Connection>();
        owner->userId = id;
        owner->appId = "eyil-interim:test";
        owner->sessionId = id;
        owner->peer.uid = getuid();
        owner->peer.gid = getgid();
        owner->peer.pid = getpid();
        return owner;
    }

    void Buffers()
    {
        Eyil::Diagnostics log("L1", Count);
        Eyil::SharedBuffers buffers(log);
        const auto a = Owner("a"), b = Owner("b");
        CHECK(buffers.Allocate(a, 0).error == CRYPTO_ERROR_INVALID_PARAMETERS);
        CHECK(buffers.Allocate(a, Eyil::SharedBuffers::MaximumSize + 1).error == CRYPTO_ERROR_OVERSIZED_INPUT_DATA);
        std::vector<std::string> ids;
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto result = buffers.Allocate(i % 2 ? a : b, 1024);
            CHECK(result.error == CRYPTO_ERROR_NONE);
            CHECK(result.size == 1024);
            ids.push_back(result.id);
        }
        CHECK(buffers.Charged(getuid()) == 4);
        CHECK(buffers.Allocate(a, 1024).error == CRYPTO_ERROR_OPERATION_FAILED);
        CHECK(buffers.Release(a, ids[0]) == CRYPTO_ERROR_INVALID_SESSION_STATE);
        Eyil::SharedBuffers::Region good { 0, 32 }, bad { UINT32_MAX, 32 }, overlap[] = { { 0, 20 }, { 10, 20 } };
        Eyil::SharedBuffers::Lease lease;
        CHECK(buffers.TryUse(b, ids[0], &bad, 1, lease) == CRYPTO_ERROR_INVALID_PARAMETERS);
        CHECK(buffers.TryUse(b, ids[0], overlap, 2, lease) == CRYPTO_ERROR_INVALID_PARAMETERS);
        CHECK(buffers.TryUse(b, ids[0], &good, 1, lease) == CRYPTO_ERROR_NONE);
        Eyil::SharedBuffers::Lease another;
        CHECK(buffers.TryUse(b, ids[0], &good, 1, another) == CRYPTO_ERROR_INVALID_SESSION_STATE);
        CHECK(buffers.Release(b, ids[0]) == CRYPTO_ERROR_NONE);
        CHECK(buffers.Charged(getuid()) == 4);
        CHECK(!buffers.Idle(b->userId));
        CHECK(buffers.TryUse(b, ids[0], &good, 1, another) == CRYPTO_ERROR_INVALID_SESSION_STATE);
        lease = {};
        CHECK(buffers.Charged(getuid()) == 3);
        CHECK(buffers.Cleanup(a->userId));
        CHECK(buffers.Cleanup(b->userId));
        CHECK(buffers.Charged(getuid()) == 0);
        auto c = Owner("concurrent");
        std::vector<std::future<Eyil::SharedBuffers::Allocation>> results;
        for (int i = 0; i < 16; ++i)
            results.push_back(std::async(std::launch::async, [&] { return buffers.Allocate(c, 4096); }));
        unsigned accepted = 0;
        for (auto &r : results)
            if (r.get().error == CRYPTO_ERROR_NONE)
                ++accepted;
        CHECK(accepted == 4);
        CHECK(buffers.Cleanup(c->userId));
        auto maximum = buffers.Allocate(a, Eyil::SharedBuffers::MaximumSize);
        CHECK(maximum.error == CRYPTO_ERROR_NONE);
        buffers.Gate(a->userId);
        CHECK(buffers.Release(a, maximum.id) == CRYPTO_ERROR_NONE);
        CHECK(buffers.Charged(getuid()) == 1);
        CHECK(buffers.Cleanup(a->userId));
        CHECK(buffers.Charged(getuid()) == 0);
    }

    void Adapter()
    {
        Eyil::Diagnostics log("L1", Count);
        Eyil::SharedBuffers buffers(log);
        EyilTest::MockCrypto crypto;
        Eyil::CryptoServiceAdapter adapter([&] { return crypto.Acquire(); }, buffers, log);
        const auto owner = Owner("adapter");
        CHECK(adapter.Notify(*owner, true));
        CHECK(crypto.references == 1);
        auto execute = [&](const CryptoRequest &request)
        {
            auto result = adapter.Execute(owner, request.SerializeAsString());
            CHECK(result.status == RPC_STATUS_OK);
            CryptoResponse response;
            CHECK(response.ParseFromString(result.payload));
            return response;
        };
        CryptoRequest request;
        request.set_operation(CRYPTO_OPERATION_GENERATE_RANDOM);
        request.mutable_generate_random()->set_number_of_bytes(16);
        auto response = execute(request);
        CHECK(response.error() == CRYPTO_ERROR_NONE);
        CHECK(response.generate_random().random_value() == std::string(16, 0x5a));
        crypto.serviceError = EyilTest::MockCrypto::CRYPTO_ERROR_INVALID_KEY_ID;
        CHECK(execute(request).error() == CRYPTO_ERROR_INVALID_KEY_ID);
        crypto.serviceError = EyilTest::MockCrypto::CRYPTO_ERROR_NONE;
        crypto.invocation = WPEFramework::Core::ERROR_GENERAL;
        CHECK(adapter.Execute(owner, request.SerializeAsString()).status == RPC_STATUS_INTERNAL);
        crypto.invocation = WPEFramework::Core::ERROR_NONE;
        crypto.available = false;
        CHECK(adapter.Execute(owner, request.SerializeAsString()).status == RPC_STATUS_INTERNAL);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER);
        request.mutable_allocate_shared_buffer()->set_requested_size(4096);
        response = execute(request);
        CHECK(response.error() == CRYPTO_ERROR_NONE);
        const auto id = response.allocate_shared_buffer().buffer_id();
        crypto.available = true;
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_SIGN);
        auto *sign = request.mutable_sign();
        sign->set_buffer_id(id);
        sign->set_key_id(71);
        sign->set_algorithm(SIGN_ALGORITHM_HMAC);
        sign->set_digest_algorithm(DIGEST_ALGORITHM_SHA256);
        sign->mutable_input_data()->set_length(16);
        response = execute(request);
        CHECK(response.error() == CRYPTO_ERROR_NONE);
        CHECK(response.sign().signature().offset() == 16);
        sign->mutable_input_data()->set_length(4090);
        response = execute(request);
        CHECK(response.error() == CRYPTO_ERROR_BUFFER_OVERFLOW);
        CHECK(response.sign().signature().offset() == 4090);
        CHECK(response.sign().signature().length() == 32);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_IMPORT_KEY);
        auto *import = request.mutable_import_key();
        import->set_key_type(KEY_TYPE_SYMMETRIC);
        import->set_key_blob("abc");
        CHECK(execute(request).error() == CRYPTO_ERROR_NONE);
        CHECK(!crypto.lastOptionalSet);
        import->set_import_export_type(IMPORT_EXPORT_TYPE_CLEAR);
        CHECK(execute(request).error() == CRYPTO_ERROR_NONE);
        CHECK(crypto.lastOptionalSet);
        request.set_operation(CRYPTO_OPERATION_LOAD_KEY);
        CHECK(adapter.Execute(owner, request.SerializeAsString()).status == RPC_STATUS_INTERNAL);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_LOAD_KEY);
        request.mutable_load_key()->set_locator("test-key");
        request.mutable_load_key()->set_provision_type(PROVISION_TYPE_PRE_PROVISIONED_GLOBAL);
        CHECK(execute(request).load_key().key_id() == 74);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_KEY_EXISTS);
        request.mutable_key_exists()->set_locator("test-key");
        CHECK(execute(request).key_exists().exists());
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_GENERATE_KEY);
        request.mutable_generate_key()->set_algorithm(GENERATE_KEY_ALGORITHM_AES128);
        request.mutable_generate_key()->add_key_usage(KEY_USAGE_ENCRYPT);
        CHECK(execute(request).generate_key().key_id() == 71);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_GENERATE_KEY_PAIR);
        auto *pair = request.mutable_generate_key_pair();
        pair->set_algorithm(KEY_PAIR_ALGORITHM_AUTH_DH);
        pair->mutable_auth_dh()->set_prime_p("prime");
        pair->mutable_auth_dh()->set_generator_g(2);
        CHECK(execute(request).generate_key_pair().private_key_id() == 72);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_DERIVE_SESSION_KEYS);
        request.mutable_derive_session_keys()->set_algorithm(KEY_PAIR_ALGORITHM_AUTH_DH);
        CHECK(execute(request).derive_session_keys().wrapping_key_id() == 78);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_GET_KEY_INFO);
        request.mutable_get_key_info()->set_key_id(71);
        CHECK(execute(request).get_key_info().key_algorithm() == KEY_ALGORITHM_SHA256);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_EXPORT_KEY);
        request.mutable_export_key()->set_key_id(71);
        CHECK(execute(request).export_key().key_blob().size() == 4);
        CHECK(!crypto.lastOptionalSet);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_UNLOAD_KEY);
        request.mutable_unload_key()->set_key_id(71);
        CHECK(execute(request).has_unload_key());
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_FLUSH_KEYS);
        request.mutable_flush_keys();
        CHECK(execute(request).has_flush_keys());
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_ENCRYPT);
        auto *encrypt = request.mutable_encrypt();
        encrypt->set_buffer_id(id);
        encrypt->set_algorithm(CIPHER_ALGORITHM_AES_CBC);
        encrypt->mutable_init_data()->set_length(16);
        encrypt->mutable_input_data()->set_offset(16);
        encrypt->mutable_input_data()->set_length(32);
        CHECK(execute(request).encrypt().encrypted_data().offset() == 48);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_DECRYPT);
        auto *decrypt = request.mutable_decrypt();
        decrypt->set_buffer_id(id);
        decrypt->set_algorithm(CIPHER_ALGORITHM_AES_CBC);
        decrypt->mutable_init_data()->set_length(16);
        decrypt->mutable_encrypted_data()->set_offset(16);
        decrypt->mutable_encrypted_data()->set_length(64);
        CHECK(execute(request).decrypt().output_data().offset() == 80);
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_VERIFY);
        auto *verify = request.mutable_verify();
        verify->set_buffer_id(id);
        verify->set_algorithm(SIGN_ALGORITHM_HMAC);
        verify->set_digest_algorithm(DIGEST_ALGORITHM_SHA256);
        verify->mutable_input_data()->set_length(16);
        verify->mutable_expected_signature()->set_offset(32);
        verify->mutable_expected_signature()->set_length(32);
        CHECK(execute(request).verify().verification_status());
        request.Clear();
        request.set_operation(CRYPTO_OPERATION_RELEASE_SHARED_BUFFER);
        request.mutable_release_shared_buffer()->set_buffer_id(id);
        CHECK(execute(request).error() == CRYPTO_ERROR_NONE);
        CHECK(adapter.Notify(*owner, false));
        CHECK(crypto.SessionCount() == 0);
        CHECK(crypto.references == 1);
        CHECK(crypto.releases == crypto.queries - 1);
        CHECK(buffers.Cleanup(owner->userId));
    }

    void ClientArguments()
    {
        CHECK(client_connect(nullptr, nullptr) == CLIENT_INVALID_ARGUMENT);
        uint8_t *output = reinterpret_cast<uint8_t *>(1);
        size_t size = 99;
        CHECK(client_send_request(nullptr, nullptr, 0, &output, &size) == CLIENT_INVALID_ARGUMENT);
        CHECK(!output && size == 0);
        char *text = reinterpret_cast<char *>(1);
        CHECK(client_send_request_string(nullptr, "hello", &text) == CLIENT_INVALID_ARGUMENT);
        CHECK(!text);
        CHECK(client_register_notifications(nullptr, nullptr) == CLIENT_NOT_IMPLEMENTED);
        client_unregister_notifications(nullptr);
        client_free_response(nullptr);
        client_disconnect(nullptr);
    }
}

int main(int argc, char **argv)
{
    return EyilTest::RunCases(argc, argv,
        { { "buffer-concurrency", Buffers }, { "operations", Adapter }, { "client-arguments", ClientArguments },
            { "log-burst", []
                {
                    Eyil::Diagnostics log("suppression", Count);
                    const auto before = lines.load();
                    for (int i = 0; i < 1000; ++i)
                        log.Error(Eyil::Reason::BackendCall, "test_error");
                    CHECK(lines == before + 1);
                    log.Summary();
                    CHECK(lines == before + 2);
                } } });
}
