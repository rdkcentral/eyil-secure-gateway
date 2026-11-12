/**
 * Copyright 2026 RDK Management
 * SPDX-License-Identifier: Apache-2.0
 */

#include "CryptoTestBackend.h"
#include "../support/Cases.h"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    using C = WPEFramework::Exchange::ICrypto;
    namespace Core = WPEFramework::Core;

    struct Fixture
    {
        Fixture()
            : api(Core::Service<WPEFramework::Plugin::CryptoTestBackend>::Create<C>())
        {
            CHECK(api);
            CHECK(api->NotifyAppState(context, C::APP_CONNECTION_STATE_CONNECTED, error) == Core::ERROR_NONE);
            CHECK(error == C::CRYPTO_ERROR_NONE);
        }

        ~Fixture() { api->Release(); }

        C *api;
        C::ApplicationContext context { "test-app", "connection-a" };
        C::CryptoError error = C::CRYPTO_ERROR_NONE;
    };

    struct File
    {
        File()
        {
            char name[] = "/tmp/eyil-crypto-test-backend-XXXXXX";
            fd = mkstemp(name);
            CHECK(fd >= 0);
            path = name;
            CHECK(ftruncate(fd, 256) == 0);
        }

        ~File()
        {
            close(fd);
            unlink(path.c_str());
        }

        C::SharedBuffer Buffer() const { return { path }; }

        int fd;
        std::string path;
    };

    void Sessions()
    {
        Fixture f;
        std::vector<uint8_t> random;
        C::ApplicationContext other { "different-app", f.context.sessionId };
        CHECK(f.api->GenerateRandom(other, 8, random, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_INVALID_SESSION_STATE && random.empty());
        CHECK(f.api->NotifyAppState(other, C::APP_CONNECTION_STATE_CONNECTED, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE);
        CHECK(f.api->NotifyAppState(f.context, C::APP_CONNECTION_STATE_DISCONNECTED, f.error) == Core::ERROR_NONE);
        CHECK(f.api->GenerateRandom(f.context, 8, random, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_INVALID_SESSION_STATE);
        CHECK(f.api->GenerateRandom(other, 8, random, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE && random == std::vector<uint8_t>(8, 0x5a));
        CHECK(f.api->NotifyAppState({}, C::APP_CONNECTION_STATE_CONNECTED, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_INVALID_PARAMETERS);
    }

    void FixedReplies()
    {
        Fixture f;
        uint32_t key = 0, first = 0, second = 0, third = 0;
        CHECK(f.api->GenerateKey(f.context, C::GENERATE_KEY_ALGORITHM_AES128, {}, key, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE && key == 1);
        CHECK(f.api->GenerateKeyPair(f.context, C::KEY_PAIR_ALGORITHM_AUTH_DH, {}, first, second, f.error)
            == Core::ERROR_NONE);
        CHECK(first == 2 && second == 3 && f.error == C::CRYPTO_ERROR_NONE);
        CHECK(f.api->DeriveSessionKeys(f.context, C::KEY_PAIR_ALGORITHM_AUTH_DH, 2, 3, 1, first, second, third, f.error)
            == Core::ERROR_NONE);
        CHECK(first == 4 && second == 5 && third == 6 && f.error == C::CRYPTO_ERROR_NONE);
        std::vector<uint8_t> data;
        CHECK(f.api->ExportKey(f.context, key, {}, data, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE && data == std::vector<uint8_t>({ 0x45, 0x59, 0x49, 0x4c }));
        CHECK(f.api->ExportKey(f.context, 7, {}, data, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_INVALID_KEY_ID && data.empty());
        CHECK(f.api->GenerateRandom(f.context, 0, data, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE && data.empty());
        CHECK(f.api->GenerateRandom(f.context, 32, data, f.error) == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE && data == std::vector<uint8_t>(32, 0x5a));
    }

    void RoundTrip()
    {
        Fixture f;
        File file;
        std::array<uint8_t, 16> input, actual;
        input.fill(0x3c);
        CHECK(pwrite(file.fd, input.data(), input.size(), 32) == 16);
        C::BufferRegion encrypted {}, plaintext {};
        CHECK(f.api->Encrypt(
                  f.context, file.Buffer(), C::CIPHER_ALGORITHM_AES_CBC, 1, { 0, 16 }, { 32, 16 }, encrypted, f.error)
            == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE && encrypted.offset == 48 && encrypted.length == 16);
        CHECK(pread(file.fd, actual.data(), actual.size(), 48) == 16);
        CHECK(std::all_of(actual.begin(), actual.end(), [](uint8_t byte) { return byte == (0x3c ^ 0xa5); }));
        CHECK(f.api->Decrypt(
                  f.context, file.Buffer(), C::CIPHER_ALGORITHM_AES_CBC, 1, { 0, 16 }, encrypted, plaintext, f.error)
            == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE && plaintext.offset == 64 && plaintext.length == 16);
        CHECK(pread(file.fd, actual.data(), actual.size(), 64) == 16 && actual == input);
        CHECK(pread(file.fd, actual.data(), actual.size(), 32) == 16 && actual == input);
    }

    void Signature()
    {
        Fixture f;
        File file;
        C::BufferRegion output {};
        CHECK(f.api->Sign(f.context, file.Buffer(), C::SIGN_ALGORITHM_HMAC, 1, C::DIGEST_ALGORITHM_SHA256, { 3, 20 },
                  output, f.error)
            == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_NONE && output.offset == 32 && output.length == 32);
        bool verified = false;
        CHECK(f.api->Verify(f.context, file.Buffer(), C::SIGN_ALGORITHM_HMAC, 1, C::DIGEST_ALGORITHM_SHA256, { 3, 20 },
                  output, verified, f.error)
            == Core::ERROR_NONE);
        CHECK(verified && f.error == C::CRYPTO_ERROR_NONE);
        const uint8_t changed = 1;
        CHECK(pwrite(file.fd, &changed, 1, 3) == 1);
        CHECK(f.api->Verify(f.context, file.Buffer(), C::SIGN_ALGORITHM_HMAC, 1, C::DIGEST_ALGORITHM_SHA256, { 3, 20 },
                  output, verified, f.error)
            == Core::ERROR_NONE);
        CHECK(!verified && f.error == C::CRYPTO_ERROR_NONE);
    }

    void Regions()
    {
        Fixture f;
        File file;
        C::BufferRegion output {};
        CHECK(f.api->Sign(f.context, file.Buffer(), C::SIGN_ALGORITHM_HMAC, 1, C::DIGEST_ALGORITHM_SHA256, { 240, 16 },
                  output, f.error)
            == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_BUFFER_OVERFLOW && output.offset == 256 && output.length == 32);
        CHECK(f.api->Sign(f.context, file.Buffer(), C::SIGN_ALGORITHM_HMAC, 1, C::DIGEST_ALGORITHM_SHA256,
                  { UINT32_MAX, 16 }, output, f.error)
            == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_INVALID_PARAMETERS);
        CHECK(f.api->Encrypt(
                  f.context, file.Buffer(), C::CIPHER_ALGORITHM_AES_CBC, 1, { 0, 16 }, { 8, 16 }, output, f.error)
            == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_INVALID_PARAMETERS);
        CHECK(ftruncate(file.fd, 16) == 0);
        CHECK(f.api->Sign(f.context, file.Buffer(), C::SIGN_ALGORITHM_HMAC, 1, C::DIGEST_ALGORITHM_SHA256, { 32, 16 },
                  output, f.error)
            == Core::ERROR_NONE);
        CHECK(f.error == C::CRYPTO_ERROR_INVALID_PARAMETERS);
    }

    void Files()
    {
        Fixture f;
        File file;
        C::BufferRegion output {};
        auto reject = [&](const std::string &path)
        {
            CHECK(f.api->Sign(f.context, { path }, C::SIGN_ALGORITHM_HMAC, 1, C::DIGEST_ALGORITHM_SHA256, { 0, 16 },
                      output, f.error)
                == Core::ERROR_NONE);
            CHECK(f.error == C::CRYPTO_ERROR_OPERATION_FAILED);
        };
        const auto link = file.path + ".link";
        CHECK(symlink(file.path.c_str(), link.c_str()) == 0);
        reject(link);
        CHECK(unlink(link.c_str()) == 0);
        const auto fifo = file.path + ".fifo";
        CHECK(mkfifo(fifo.c_str(), 0600) == 0);
        reject(fifo);
        CHECK(unlink(fifo.c_str()) == 0);
        CHECK(ftruncate(file.fd, 0) == 0);
        reject(file.path);
        CHECK(ftruncate(file.fd, 1572865) == 0);
        reject(file.path);
        reject(file.path + ".missing");
    }
}

int main(int argc, char **argv)
{
    return EyilTest::RunCases(argc, argv,
        { { "sessions", Sessions }, { "fixed-replies", FixedReplies }, { "round-trip", RoundTrip },
            { "signature", Signature }, { "regions", Regions }, { "files", Files } });
}
