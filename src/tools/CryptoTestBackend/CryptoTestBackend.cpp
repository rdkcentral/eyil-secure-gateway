/**
 * Copyright 2026 RDK Management
 * SPDX-License-Identifier: Apache-2.0
 */

#include "CryptoTestBackend.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace WPEFramework::Plugin
{
    SERVICE_REGISTRATION(CryptoTestBackend, 1, 0, 0);

    namespace
    {
        using Crypto = Exchange::ICrypto;
        constexpr uint64_t MaximumBufferSize = 1572864;

        bool SameSession(const Crypto::ApplicationContext &a, const Crypto::ApplicationContext &b)
        {
            return a.appId == b.appId && a.sessionId == b.sessionId;
        }

        class Buffer
        {
        public:
            explicit Buffer(const Crypto::SharedBuffer &buffer)
                : _fd(open(buffer.name.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK))
            {
                struct stat info
                {
                };

                if (_fd >= 0 && fstat(_fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0
                    && static_cast<uint64_t>(info.st_size) <= MaximumBufferSize)
                    _size = static_cast<uint64_t>(info.st_size);
            }

            ~Buffer()
            {
                if (_fd >= 0)
                    close(_fd);
            }

            Buffer(const Buffer &) = delete;
            Buffer &operator=(const Buffer &) = delete;

            bool Valid() const { return _size != 0; }

            bool Contains(const Crypto::BufferRegion &region) const
            {
                return region.offset <= _size && region.length <= _size - region.offset;
            }

            static uint64_t End(const Crypto::BufferRegion &region) { return uint64_t(region.offset) + region.length; }

            static bool Overlap(const Crypto::BufferRegion &a, const Crypto::BufferRegion &b)
            {
                return a.length && b.length && a.offset < End(b) && b.offset < End(a);
            }

            bool Read(const Crypto::BufferRegion &region, std::vector<uint8_t> &data) const
            {
                data.resize(region.length);
                size_t done = 0;
                while (done < data.size())
                {
                    const auto count = pread(_fd, data.data() + done, data.size() - done, region.offset + done);
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count <= 0)
                        return false;
                    done += static_cast<size_t>(count);
                }
                return true;
            }

            bool Write(const Crypto::BufferRegion &region, const uint8_t *data) const
            {
                size_t done = 0;
                while (done < region.length)
                {
                    const auto count = pwrite(_fd, data + done, region.length - done, region.offset + done);
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count <= 0)
                        return false;
                    done += static_cast<size_t>(count);
                }
                return true;
            }

        private:
            int _fd;
            uint64_t _size = 0;
        };

        std::array<uint8_t, 32> Signature(const std::vector<uint8_t> &input)
        {
            std::array<uint8_t, 32> signature;
            signature.fill(0x5a);
            for (size_t i = 0; i < input.size(); ++i)
                signature[i % signature.size()] ^= input[i];
            return signature;
        }

        Crypto::CryptoError Transform(const Crypto::SharedBuffer &shared, const Crypto::BufferRegion &input,
            const Crypto::BufferRegion &init, Crypto::BufferRegion &output, bool sign)
        {
            Buffer buffer(shared);
            if (!buffer.Valid())
                return Crypto::CRYPTO_ERROR_OPERATION_FAILED;
            if (!buffer.Contains(input) || !buffer.Contains(init) || Buffer::Overlap(input, init))
                return Crypto::CRYPTO_ERROR_INVALID_PARAMETERS;

            const auto end = std::max(Buffer::End(input), Buffer::End(init));
            output = { static_cast<uint32_t>((end + 15) & ~uint64_t(15)), sign ? 32 : input.length };
            if (!buffer.Contains(output))
                return Crypto::CRYPTO_ERROR_BUFFER_OVERFLOW;

            std::vector<uint8_t> data;
            if (!buffer.Read(input, data))
                return Crypto::CRYPTO_ERROR_OPERATION_FAILED;
            if (sign)
            {
                const auto signature = Signature(data);
                return buffer.Write(output, signature.data()) ? Crypto::CRYPTO_ERROR_NONE
                                                              : Crypto::CRYPTO_ERROR_OPERATION_FAILED;
            }
            for (auto &byte : data)
                byte ^= 0xa5;
            return buffer.Write(output, data.data()) ? Crypto::CRYPTO_ERROR_NONE
                                                     : Crypto::CRYPTO_ERROR_OPERATION_FAILED;
        }
    }

    const string CryptoTestBackend::Initialize(PluginHost::IShell *)
    {
        SYSLOG(Logging::Notification,
            (_T("CryptoTestBackend activated: DEVELOPMENT ONLY, deterministic data, no real cryptography")));
        return {};
    }

    void CryptoTestBackend::Deinitialize(PluginHost::IShell *)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _sessions.clear();
    }

    string CryptoTestBackend::Information() const
    {
        return "Development-only deterministic ICrypto backend; no real cryptography";
    }

    bool CryptoTestBackend::Session(const ApplicationContext &context, CryptoError &error) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const bool found = std::any_of(
            _sessions.begin(), _sessions.end(), [&](const auto &session) { return SameSession(context, session); });
        error = found ? CRYPTO_ERROR_NONE : CRYPTO_ERROR_INVALID_SESSION_STATE;
        return found;
    }

    bool CryptoTestBackend::Key(const ApplicationContext &context, uint32_t keyId, CryptoError &error) const
    {
        if (!Session(context, error))
            return false;
        if (keyId < 1 || keyId > 6)
            error = CRYPTO_ERROR_INVALID_KEY_ID;
        return error == CRYPTO_ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::NotifyAppState(
        const ApplicationContext &context, AppConnectionState state, CryptoError &error)
    {
        error = CRYPTO_ERROR_NONE;
        if (context.appId.empty() || context.sessionId.empty()
            || (state != APP_CONNECTION_STATE_CONNECTED && state != APP_CONNECTION_STATE_DISCONNECTED))
        {
            error = CRYPTO_ERROR_INVALID_PARAMETERS;
            return Core::ERROR_NONE;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = std::find_if(
            _sessions.begin(), _sessions.end(), [&](const auto &session) { return SameSession(context, session); });
        if (state == APP_CONNECTION_STATE_DISCONNECTED)
        {
            if (found != _sessions.end())
                _sessions.erase(found);
        }
        else if (found == _sessions.end())
        {
            try
            {
                _sessions.push_back(context);
            }
            catch (const std::bad_alloc &)
            {
                return Core::ERROR_GENERAL;
            }
        }
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::GenerateRandom(
        const ApplicationContext &context, uint32_t count, std::vector<uint8_t> &data, CryptoError &error)
    {
        data.clear();
        if (Session(context, error))
        {
            try
            {
                data.assign(count, 0x5a);
            }
            catch (const std::bad_alloc &)
            {
                return Core::ERROR_GENERAL;
            }
            catch (const std::length_error &)
            {
                return Core::ERROR_GENERAL;
            }
        }
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::GenerateKey(const ApplicationContext &context, GenerateKeyAlgorithm,
        const std::vector<KeyUsage> &, uint32_t &keyId, CryptoError &error)
    {
        keyId = Session(context, error) ? 1 : 0;
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::GenerateKeyPair(const ApplicationContext &context, KeyPairAlgorithm,
        const KeyPairParameters &, uint32_t &privateKeyId, uint32_t &publicKeyId, CryptoError &error)
    {
        const bool valid = Session(context, error);
        privateKeyId = valid ? 2 : 0;
        publicKeyId = valid ? 3 : 0;
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::LoadKey(
        const ApplicationContext &context, const string &, ProvisionType, uint32_t &keyId, CryptoError &error)
    {
        keyId = Session(context, error) ? 1 : 0;
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::UnloadKey(const ApplicationContext &context, uint32_t keyId, CryptoError &error)
    {
        Key(context, keyId, error);
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::KeyExists(
        const ApplicationContext &context, const string &, bool &exists, uint32_t &keyId, CryptoError &error) const
    {
        exists = Session(context, error);
        keyId = exists ? 1 : 0;
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::GetKeyInfo(const ApplicationContext &context, uint32_t keyId, KeyType &keyType,
        std::vector<KeyUsage> &usage, KeyAlgorithm &algorithm, CryptoError &error) const
    {
        keyType = KEY_TYPE_UNSPECIFIED;
        algorithm = KEY_ALGORITHM_UNSPECIFIED;
        usage.clear();
        if (Key(context, keyId, error))
        {
            keyType = KEY_TYPE_SYMMETRIC;
            algorithm = KEY_ALGORITHM_AES_CBC;
            try
            {
                usage = { KEY_USAGE_ENCRYPT, KEY_USAGE_DECRYPT, KEY_USAGE_SIGN, KEY_USAGE_VERIFY };
            }
            catch (const std::bad_alloc &)
            {
                return Core::ERROR_GENERAL;
            }
        }
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::FlushKeys(const ApplicationContext &context, CryptoError &error)
    {
        Session(context, error);
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::ImportKey(const ApplicationContext &context, KeyType,
        const Core::OptionalType<ImportExportType> &, const std::vector<uint8_t> &, uint32_t &keyId, CryptoError &error)
    {
        keyId = Session(context, error) ? 1 : 0;
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::ExportKey(const ApplicationContext &context, uint32_t keyId,
        const Core::OptionalType<ImportExportType> &, std::vector<uint8_t> &blob, CryptoError &error) const
    {
        blob.clear();
        if (Key(context, keyId, error))
        {
            try
            {
                blob = { 0x45, 0x59, 0x49, 0x4c };
            }
            catch (const std::bad_alloc &)
            {
                return Core::ERROR_GENERAL;
            }
        }
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::DeriveSessionKeys(const ApplicationContext &context, KeyPairAlgorithm, uint32_t,
        uint32_t, uint32_t, uint32_t &encryptionKeyId, uint32_t &hmacKeyId, uint32_t &wrappingKeyId, CryptoError &error)
    {
        const bool valid = Session(context, error);
        encryptionKeyId = valid ? 4 : 0;
        hmacKeyId = valid ? 5 : 0;
        wrappingKeyId = valid ? 6 : 0;
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::Encrypt(const ApplicationContext &context, const SharedBuffer &buffer,
        CipherAlgorithm algorithm, uint32_t keyId, const BufferRegion &initData, const BufferRegion &inputData,
        BufferRegion &output, CryptoError &error)
    {
        output = {};
        if (Key(context, keyId, error))
        {
            if (algorithm != CIPHER_ALGORITHM_AES_CBC || initData.length != 16)
                error = CRYPTO_ERROR_INVALID_PARAMETERS;
            else
            {
                try
                {
                    error = Transform(buffer, inputData, initData, output, false);
                }
                catch (const std::bad_alloc &)
                {
                    return Core::ERROR_GENERAL;
                }
            }
        }
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::Decrypt(const ApplicationContext &context, const SharedBuffer &buffer,
        CipherAlgorithm algorithm, uint32_t keyId, const BufferRegion &initData, const BufferRegion &encryptedData,
        BufferRegion &output, CryptoError &error)
    {
        return Encrypt(context, buffer, algorithm, keyId, initData, encryptedData, output, error);
    }

    Core::hresult CryptoTestBackend::Sign(const ApplicationContext &context, const SharedBuffer &buffer,
        SignatureAlgorithm algorithm, uint32_t keyId, DigestAlgorithm digest, const BufferRegion &input,
        BufferRegion &signature, CryptoError &error)
    {
        signature = {};
        if (Key(context, keyId, error))
        {
            if (algorithm != SIGN_ALGORITHM_HMAC || digest != DIGEST_ALGORITHM_SHA256)
                error = CRYPTO_ERROR_INVALID_PARAMETERS;
            else
            {
                try
                {
                    error = Transform(buffer, input, {}, signature, true);
                }
                catch (const std::bad_alloc &)
                {
                    return Core::ERROR_GENERAL;
                }
            }
        }
        return Core::ERROR_NONE;
    }

    Core::hresult CryptoTestBackend::Verify(const ApplicationContext &context, const SharedBuffer &shared,
        SignatureAlgorithm algorithm, uint32_t keyId, DigestAlgorithm digest, const BufferRegion &input,
        const BufferRegion &expected, bool &verified, CryptoError &error)
    {
        verified = false;
        if (!Key(context, keyId, error))
            return Core::ERROR_NONE;
        if (algorithm != SIGN_ALGORITHM_HMAC || digest != DIGEST_ALGORITHM_SHA256)
        {
            error = CRYPTO_ERROR_INVALID_PARAMETERS;
            return Core::ERROR_NONE;
        }
        Buffer buffer(shared);
        if (!buffer.Valid())
            error = CRYPTO_ERROR_OPERATION_FAILED;
        else if (!buffer.Contains(input) || !buffer.Contains(expected) || Buffer::Overlap(input, expected))
            error = CRYPTO_ERROR_INVALID_PARAMETERS;
        else
        {
            try
            {
                std::vector<uint8_t> data, supplied;
                if (!buffer.Read(input, data) || !buffer.Read(expected, supplied))
                    error = CRYPTO_ERROR_OPERATION_FAILED;
                else
                {
                    const auto signature = Signature(data);
                    verified = supplied.size() == signature.size()
                        && std::equal(signature.begin(), signature.end(), supplied.begin());
                }
            }
            catch (const std::bad_alloc &)
            {
                return Core::ERROR_GENERAL;
            }
        }
        return Core::ERROR_NONE;
    }
}
