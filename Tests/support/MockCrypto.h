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

#pragma once
// Deterministic test double only: deliberately does not implement cryptography.
#include <interfaces/ICrypto.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace EyilTest
{
    namespace Core = WPEFramework::Core;
    using std::string;

    class MockCrypto : public WPEFramework::Exchange::ICrypto
    {
    public:
        mutable std::atomic<unsigned> references { 1 };
        mutable std::atomic<unsigned> queries { 0 };
        mutable std::atomic<unsigned> releases { 0 };
        mutable std::atomic<unsigned> calls { 0 };
        mutable std::atomic<unsigned> entered { 0 };

        std::atomic<bool> available { true };
        std::atomic<bool> block { false };
        std::atomic<unsigned> delayMs { 0 };
        std::atomic<unsigned> slowSize { 0 };
        std::atomic<Core::hresult> invocation { Core::ERROR_NONE };
        std::atomic<CryptoError> serviceError { CRYPTO_ERROR_NONE };
        mutable std::mutex mutex;
        mutable std::condition_variable changed;
        std::set<string> sessions;
        mutable bool lastOptionalSet = false;

        uint32_t AddRef() const override { return ++references; }

        uint32_t Release() const override
        {
            ++releases;
            return --references;
        }

        void *QueryInterface(uint32_t id) override
        {
            if (id == ID || id == Core::IUnknown::ID)
            {
                AddRef();
                return static_cast<WPEFramework::Exchange::ICrypto *>(this);
            }
            return nullptr;
        }

        WPEFramework::Exchange::ICrypto *Acquire()
        {
            ++queries;
            if (!available)
                return nullptr;
            AddRef();
            return this;
        }

        void Unblock()
        {
            block = false;
            changed.notify_all();
        }

        size_t SessionCount()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return sessions.size();
        }

        Core::hresult Result(const ApplicationContext &context, CryptoError &error) const
        {
            ++calls;
            ++entered;
            changed.notify_all();
            if (delayMs)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return !block.load(); });
            error = sessions.count(context.sessionId) ? serviceError.load() : CRYPTO_ERROR_INVALID_SESSION_STATE;
            return invocation.load();
        }

        Core::hresult GenerateKey(const ApplicationContext &context, const GenerateKeyAlgorithm algorithm,
            const std::vector<KeyUsage> &usage, uint32_t &keyId, CryptoError &error) override
        {
            keyId = 71;
            return Result(context, error);
        }

        Core::hresult GenerateKeyPair(const ApplicationContext &context, const KeyPairAlgorithm algorithm,
            const KeyPairParameters &parameters, uint32_t &privateKeyId, uint32_t &publicKeyId,
            CryptoError &error) override
        {
            privateKeyId = 72;
            publicKeyId = 73;
            return Result(context, error);
        }

        Core::hresult LoadKey(const ApplicationContext &context, const string &locator,
            const ProvisionType provisionType, uint32_t &keyId, CryptoError &error) override
        {
            keyId = 74;
            return Result(context, error);
        }

        Core::hresult UnloadKey(const ApplicationContext &context, const uint32_t keyId, CryptoError &error) override
        {
            return Result(context, error);
        }

        Core::hresult KeyExists(const ApplicationContext &context, const string &locator, bool &exists, uint32_t &keyId,
            CryptoError &error) const override
        {
            exists = true;
            keyId = 74;
            return Result(context, error);
        }

        Core::hresult GetKeyInfo(const ApplicationContext &context, const uint32_t keyId, KeyType &keyType,
            std::vector<KeyUsage> &keyUsage, KeyAlgorithm &keyAlgorithm, CryptoError &error) const override
        {
            keyType = KEY_TYPE_SYMMETRIC;
            keyUsage = { KEY_USAGE_SIGN };
            keyAlgorithm = KEY_ALGORITHM_SHA256;
            return Result(context, error);
        }

        Core::hresult FlushKeys(const ApplicationContext &context, CryptoError &error) override
        {
            return Result(context, error);
        }

        Core::hresult ImportKey(const ApplicationContext &context, const KeyType keyType,
            const Core::OptionalType<ImportExportType> &importExportType, const std::vector<uint8_t> &keyBlob,
            uint32_t &keyId, CryptoError &error) override
        {
            lastOptionalSet = importExportType.IsSet();
            keyId = 75;
            return Result(context, error);
        }

        Core::hresult ExportKey(const ApplicationContext &context, const uint32_t keyId,
            const Core::OptionalType<ImportExportType> &importExportType, std::vector<uint8_t> &keyBlob,
            CryptoError &error) const override
        {
            lastOptionalSet = importExportType.IsSet();
            keyBlob = { 1, 2, 3, 4 };
            return Result(context, error);
        }

        Core::hresult Sign(const ApplicationContext &context, const SharedBuffer &buffer,
            const SignatureAlgorithm algorithm, const uint32_t keyId, const DigestAlgorithm digestAlgorithm,
            const BufferRegion &inputData, BufferRegion &signature, CryptoError &error) override
        {
            auto result = Result(context, error);
            if (result == Core::ERROR_NONE && error == CRYPTO_ERROR_NONE)
                error = Write(buffer, inputData, signature, 32);
            return result;
        }

        Core::hresult Verify(const ApplicationContext &context, const SharedBuffer &buffer,
            const SignatureAlgorithm algorithm, const uint32_t keyId, const DigestAlgorithm digestAlgorithm,
            const BufferRegion &inputData, const BufferRegion &expectedSignature, bool &verificationStatus,
            CryptoError &error) override
        {
            verificationStatus = true;
            return Result(context, error);
        }

        Core::hresult Encrypt(const ApplicationContext &context, const SharedBuffer &buffer,
            const CipherAlgorithm algorithm, const uint32_t keyId, const BufferRegion &initData,
            const BufferRegion &inputData, BufferRegion &encryptedData, CryptoError &error) override
        {
            auto result = Result(context, error);
            if (result == Core::ERROR_NONE && error == CRYPTO_ERROR_NONE)
                error = Write(buffer,
                    { std::max(initData.offset + initData.length, inputData.offset + inputData.length), 0 },
                    encryptedData, inputData.length + 16);
            return result;
        }

        Core::hresult Decrypt(const ApplicationContext &context, const SharedBuffer &buffer,
            const CipherAlgorithm algorithm, const uint32_t keyId, const BufferRegion &initData,
            const BufferRegion &encryptedData, BufferRegion &outputData, CryptoError &error) override
        {
            auto result = Result(context, error);
            if (result == Core::ERROR_NONE && error == CRYPTO_ERROR_NONE)
                error = Write(buffer,
                    { std::max(initData.offset + initData.length, encryptedData.offset + encryptedData.length), 0 },
                    outputData, encryptedData.length);
            return result;
        }

        Core::hresult DeriveSessionKeys(const ApplicationContext &context, const KeyPairAlgorithm algorithm,
            const uint32_t privateKeyId, const uint32_t publicKeyId, const uint32_t derivationKeyId,
            uint32_t &encryptionKeyId, uint32_t &hmacKeyId, uint32_t &wrappingKeyId, CryptoError &error) override
        {
            encryptionKeyId = 76;
            hmacKeyId = 77;
            wrappingKeyId = 78;
            return Result(context, error);
        }

        Core::hresult GenerateRandom(const ApplicationContext &context, const uint32_t numberOfBytes,
            std::vector<uint8_t> &randomValue, CryptoError &error) override
        {
            if (slowSize && numberOfBytes == slowSize)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto result = Result(context, error);
            if (result == Core::ERROR_NONE && error == CRYPTO_ERROR_NONE)
                randomValue.assign(numberOfBytes, 0x5a);
            return result;
        }

        Core::hresult NotifyAppState(
            const ApplicationContext &context, const AppConnectionState state, CryptoError &error) override
        {
            ++calls;
            std::lock_guard<std::mutex> lock(mutex);
            error = serviceError.load();
            if (invocation == Core::ERROR_NONE && error == CRYPTO_ERROR_NONE)
            {
                if (state == APP_CONNECTION_STATE_CONNECTED)
                    sessions.insert(context.sessionId);
                else
                    sessions.erase(context.sessionId);
            }
            return invocation.load();
        }

    private:
        static CryptoError Write(const SharedBuffer &buffer, BufferRegion input, BufferRegion &output, uint32_t bytes)
        {
            output = { input.offset + input.length, bytes };
            const int fd = open(buffer.name.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (fd < 0)
                return CRYPTO_ERROR_OPERATION_FAILED;

            struct stat status
            {
            };

            CryptoError result = CRYPTO_ERROR_NONE;
            if (fstat(fd, &status) || !S_ISREG(status.st_mode) || status.st_size <= 0)
                result = CRYPTO_ERROR_OPERATION_FAILED;
            else if (uint64_t(output.offset) + output.length > static_cast<uint64_t>(status.st_size))
                result = CRYPTO_ERROR_BUFFER_OVERFLOW;
            else
            {
                std::vector<uint8_t> data(bytes, 0xa5);
                if (pwrite(fd, data.data(), data.size(), output.offset) != static_cast<ssize_t>(data.size()))
                    result = CRYPTO_ERROR_OPERATION_FAILED;
            }
            close(fd);
            return result;
        }
    };
}
