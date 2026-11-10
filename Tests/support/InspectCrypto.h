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

#include "MockCrypto.h"
#include <functional>
#include <crypto.pb.h>

namespace EyilTest
{
    // Records every backend input. Unit assertions compare both payload fields
    // and the trusted context/path, rather than accepting a constant mock result.
    class InspectCrypto : public MockCrypto
    {
    public:
        struct Call
        {
            CryptoOperation operation = CRYPTO_OPERATION_UNSPECIFIED;
            ApplicationContext context;
            std::string text;
            std::string path;
            unsigned algorithm = 0;
            unsigned digest = 0;
            unsigned format = 0;
            bool optional = false;
            uint32_t key = 0;
            uint32_t secondKey = 0;
            uint32_t thirdKey = 0;
            uint32_t number = 0;
            std::vector<uint8_t> bytes;
            std::vector<KeyUsage> usages;
            BufferRegion first {};
            BufferRegion second {};
        };

        mutable Call last;
        bool throwCall = false;
        bool existsResult = true;
        bool verificationResult = true;
        bool wrongRandomSize = false;
        bool overrideRegion = false;
        BufferRegion outputRegion {};
        KeyType infoType = KEY_TYPE_SYMMETRIC;
        KeyAlgorithm infoAlgorithm = KEY_ALGORITHM_SHA256;
        std::vector<KeyUsage> infoUsages { KEY_USAGE_SIGN };
        std::function<void()> onRelease;

        uint32_t Release() const override
        {
            if (onRelease)
                onRelease();
            return MockCrypto::Release();
        }

        void Capture(CryptoOperation op, const ApplicationContext &context) const
        {
            last = {};
            last.operation = op;
            last.context = context;
            if (throwCall)
                throw std::runtime_error("test backend exception");
        }

        Core::hresult GenerateKey(const ApplicationContext &c, GenerateKeyAlgorithm a,
            const std::vector<KeyUsage> &usage, uint32_t &key, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_GENERATE_KEY, c);
            last.algorithm = a;
            last.usages = usage;
            return MockCrypto::GenerateKey(c, a, usage, key, e);
        }

        Core::hresult GenerateKeyPair(const ApplicationContext &c, KeyPairAlgorithm a, const KeyPairParameters &p,
            uint32_t &priv, uint32_t &pub, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_GENERATE_KEY_PAIR, c);
            last.algorithm = a;
            last.bytes = p.authDH.primeP;
            last.number = p.authDH.generatorG;
            return MockCrypto::GenerateKeyPair(c, a, p, priv, pub, e);
        }

        Core::hresult LoadKey(
            const ApplicationContext &c, const string &loc, ProvisionType type, uint32_t &key, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_LOAD_KEY, c);
            last.text = loc;
            last.algorithm = type;
            return MockCrypto::LoadKey(c, loc, type, key, e);
        }

        Core::hresult KeyExists(
            const ApplicationContext &c, const string &loc, bool &exists, uint32_t &key, CryptoError &e) const override
        {
            Capture(CRYPTO_OPERATION_KEY_EXISTS, c);
            last.text = loc;
            auto r = MockCrypto::KeyExists(c, loc, exists, key, e);
            exists = existsResult;
            return r;
        }

        Core::hresult FlushKeys(const ApplicationContext &c, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_FLUSH_KEYS, c);
            return MockCrypto::FlushKeys(c, e);
        }

        Core::hresult UnloadKey(const ApplicationContext &c, uint32_t key, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_UNLOAD_KEY, c);
            last.key = key;
            return MockCrypto::UnloadKey(c, key, e);
        }

        Core::hresult DeriveSessionKeys(const ApplicationContext &c, KeyPairAlgorithm a, uint32_t priv, uint32_t pub,
            uint32_t deriv, uint32_t &enc, uint32_t &hmac, uint32_t &wrap, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_DERIVE_SESSION_KEYS, c);
            last.algorithm = a;
            last.key = priv;
            last.secondKey = pub;
            last.thirdKey = deriv;
            return MockCrypto::DeriveSessionKeys(c, a, priv, pub, deriv, enc, hmac, wrap, e);
        }

        Core::hresult GetKeyInfo(const ApplicationContext &c, uint32_t key, KeyType &type, std::vector<KeyUsage> &usage,
            KeyAlgorithm &algorithm, CryptoError &e) const override
        {
            Capture(CRYPTO_OPERATION_GET_KEY_INFO, c);
            last.key = key;
            auto r = Result(c, e);
            type = infoType;
            usage = infoUsages;
            algorithm = infoAlgorithm;
            return r;
        }

        Core::hresult ImportKey(const ApplicationContext &c, KeyType type,
            const Core::OptionalType<ImportExportType> &format, const std::vector<uint8_t> &blob, uint32_t &key,
            CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_IMPORT_KEY, c);
            last.algorithm = type;
            last.optional = format.IsSet();
            if (format.IsSet())
                last.format = format.Value();
            last.bytes = blob;
            return MockCrypto::ImportKey(c, type, format, blob, key, e);
        }

        Core::hresult ExportKey(const ApplicationContext &c, uint32_t key,
            const Core::OptionalType<ImportExportType> &format, std::vector<uint8_t> &blob,
            CryptoError &e) const override
        {
            Capture(CRYPTO_OPERATION_EXPORT_KEY, c);
            last.key = key;
            last.optional = format.IsSet();
            if (format.IsSet())
                last.format = format.Value();
            return MockCrypto::ExportKey(c, key, format, blob, e);
        }

        Core::hresult GenerateRandom(
            const ApplicationContext &c, uint32_t n, std::vector<uint8_t> &bytes, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_GENERATE_RANDOM, c);
            last.number = n;
            auto r = MockCrypto::GenerateRandom(c, n, bytes, e);
            if (wrongRandomSize)
                bytes.push_back(0);
            return r;
        }

        Core::hresult Sign(const ApplicationContext &c, const SharedBuffer &b, SignatureAlgorithm a, uint32_t key,
            DigestAlgorithm digest, const BufferRegion &input, BufferRegion &out, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_SIGN, c);
            last.path = b.name;
            last.algorithm = a;
            last.key = key;
            last.digest = digest;
            last.first = input;
            if (overrideRegion)
            {
                out = outputRegion;
                return Result(c, e);
            }
            return MockCrypto::Sign(c, b, a, key, digest, input, out, e);
        }

        Core::hresult Verify(const ApplicationContext &c, const SharedBuffer &b, SignatureAlgorithm a, uint32_t key,
            DigestAlgorithm digest, const BufferRegion &input, const BufferRegion &sig, bool &ok,
            CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_VERIFY, c);
            last.path = b.name;
            last.algorithm = a;
            last.key = key;
            last.digest = digest;
            last.first = input;
            last.second = sig;
            auto r = MockCrypto::Verify(c, b, a, key, digest, input, sig, ok, e);
            ok = verificationResult;
            return r;
        }

        Core::hresult Encrypt(const ApplicationContext &c, const SharedBuffer &b, CipherAlgorithm a, uint32_t key,
            const BufferRegion &init, const BufferRegion &input, BufferRegion &out, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_ENCRYPT, c);
            last.path = b.name;
            last.algorithm = a;
            last.key = key;
            last.first = init;
            last.second = input;
            if (overrideRegion)
            {
                out = outputRegion;
                return Result(c, e);
            }
            return MockCrypto::Encrypt(c, b, a, key, init, input, out, e);
        }

        Core::hresult Decrypt(const ApplicationContext &c, const SharedBuffer &b, CipherAlgorithm a, uint32_t key,
            const BufferRegion &init, const BufferRegion &input, BufferRegion &out, CryptoError &e) override
        {
            Capture(CRYPTO_OPERATION_DECRYPT, c);
            last.path = b.name;
            last.algorithm = a;
            last.key = key;
            last.first = init;
            last.second = input;
            if (overrideRegion)
            {
                out = outputRegion;
                return Result(c, e);
            }
            return MockCrypto::Decrypt(c, b, a, key, init, input, out, e);
        }
    };
}
