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

#include "CryptoServiceAdapter.h"
#include <com/com.h>
#include <climits>
#include <optional>

namespace Eyil
{
    namespace
    {
        using C = CryptoServiceAdapter::Crypto;

        std::optional<C::GenerateKeyAlgorithm> To(GenerateKeyAlgorithm value)
        {
            switch (value)
            {
            case GENERATE_KEY_ALGORITHM_AES128:
                return C::GENERATE_KEY_ALGORITHM_AES128;
            case GENERATE_KEY_ALGORITHM_AES256:
                return C::GENERATE_KEY_ALGORITHM_AES256;
            case GENERATE_KEY_ALGORITHM_HMAC128:
                return C::GENERATE_KEY_ALGORITHM_HMAC128;
            case GENERATE_KEY_ALGORITHM_HMAC160:
                return C::GENERATE_KEY_ALGORITHM_HMAC160;
            case GENERATE_KEY_ALGORITHM_HMAC256:
                return C::GENERATE_KEY_ALGORITHM_HMAC256;
            default:
                return std::nullopt;
            }
        }

        std::optional<C::KeyUsage> To(KeyUsage value)
        {
            switch (value)
            {
            case KEY_USAGE_ENCRYPT:
                return C::KEY_USAGE_ENCRYPT;
            case KEY_USAGE_DECRYPT:
                return C::KEY_USAGE_DECRYPT;
            case KEY_USAGE_SIGN:
                return C::KEY_USAGE_SIGN;
            case KEY_USAGE_VERIFY:
                return C::KEY_USAGE_VERIFY;
            case KEY_USAGE_DERIVE:
                return C::KEY_USAGE_DERIVE;
            case KEY_USAGE_WRAP:
                return C::KEY_USAGE_WRAP;
            case KEY_USAGE_UNWRAP:
                return C::KEY_USAGE_UNWRAP;
            default:
                return std::nullopt;
            }
        }

        std::optional<KeyUsage> From(C::KeyUsage value)
        {
            switch (value)
            {
            case C::KEY_USAGE_ENCRYPT:
                return KEY_USAGE_ENCRYPT;
            case C::KEY_USAGE_DECRYPT:
                return KEY_USAGE_DECRYPT;
            case C::KEY_USAGE_SIGN:
                return KEY_USAGE_SIGN;
            case C::KEY_USAGE_VERIFY:
                return KEY_USAGE_VERIFY;
            case C::KEY_USAGE_DERIVE:
                return KEY_USAGE_DERIVE;
            case C::KEY_USAGE_WRAP:
                return KEY_USAGE_WRAP;
            case C::KEY_USAGE_UNWRAP:
                return KEY_USAGE_UNWRAP;
            default:
                return std::nullopt;
            }
        }

        std::optional<C::KeyPairAlgorithm> To(KeyPairAlgorithm value)
        {
            switch (value)
            {
            case KEY_PAIR_ALGORITHM_AUTH_DH:
                return C::KEY_PAIR_ALGORITHM_AUTH_DH;
            default:
                return std::nullopt;
            }
        }

        std::optional<C::ProvisionType> To(ProvisionType value)
        {
            switch (value)
            {
            case PROVISION_TYPE_PRE_PROVISIONED_GLOBAL:
                return C::PROVISION_TYPE_PRE_PROVISIONED_GLOBAL;
            default:
                return std::nullopt;
            }
        }

        std::optional<C::KeyType> To(KeyType value)
        {
            switch (value)
            {
            case KEY_TYPE_PUBLIC:
                return C::KEY_TYPE_PUBLIC;
            case KEY_TYPE_PRIVATE:
                return C::KEY_TYPE_PRIVATE;
            case KEY_TYPE_SYMMETRIC:
                return C::KEY_TYPE_SYMMETRIC;
            default:
                return std::nullopt;
            }
        }

        std::optional<KeyType> From(C::KeyType value)
        {
            switch (value)
            {
            case C::KEY_TYPE_PUBLIC:
                return KEY_TYPE_PUBLIC;
            case C::KEY_TYPE_PRIVATE:
                return KEY_TYPE_PRIVATE;
            case C::KEY_TYPE_SYMMETRIC:
                return KEY_TYPE_SYMMETRIC;
            default:
                return std::nullopt;
            }
        }

        std::optional<KeyAlgorithm> From(C::KeyAlgorithm value)
        {
            switch (value)
            {
            case C::KEY_ALGORITHM_AES_CBC:
                return KEY_ALGORITHM_AES_CBC;
            case C::KEY_ALGORITHM_AES_CTR:
                return KEY_ALGORITHM_AES_CTR;
            case C::KEY_ALGORITHM_SHA256:
                return KEY_ALGORITHM_SHA256;
            case C::KEY_ALGORITHM_SHA384:
                return KEY_ALGORITHM_SHA384;
            case C::KEY_ALGORITHM_SHA512:
                return KEY_ALGORITHM_SHA512;
            case C::KEY_ALGORITHM_NFLXDH:
                return KEY_ALGORITHM_NFLXDH;
            case C::KEY_ALGORITHM_DH:
                return KEY_ALGORITHM_DH;
            default:
                return std::nullopt;
            }
        }

        std::optional<C::ImportExportType> To(ImportExportType value)
        {
            switch (value)
            {
            case IMPORT_EXPORT_TYPE_PROTECTED:
                return C::IMPORT_EXPORT_TYPE_PROTECTED;
            case IMPORT_EXPORT_TYPE_CLEAR:
                return C::IMPORT_EXPORT_TYPE_CLEAR;
            default:
                return std::nullopt;
            }
        }

        std::optional<C::SignatureAlgorithm> To(SignatureAlgorithm value)
        {
            switch (value)
            {
            case SIGN_ALGORITHM_HMAC:
                return C::SIGN_ALGORITHM_HMAC;
            default:
                return std::nullopt;
            }
        }

        std::optional<C::DigestAlgorithm> To(DigestAlgorithm value)
        {
            switch (value)
            {
            case DIGEST_ALGORITHM_SHA256:
                return C::DIGEST_ALGORITHM_SHA256;
            default:
                return std::nullopt;
            }
        }

        std::optional<C::CipherAlgorithm> To(CipherAlgorithm value)
        {
            switch (value)
            {
            case CIPHER_ALGORITHM_AES_CBC:
                return C::CIPHER_ALGORITHM_AES_CBC;
            default:
                return std::nullopt;
            }
        }

        std::optional<CryptoError> From(C::CryptoError value)
        {
            switch (value)
            {
            case C::CRYPTO_ERROR_NONE:
                return CRYPTO_ERROR_NONE;
            case C::CRYPTO_ERROR_AUTHENTICATION_FAILED:
                return CRYPTO_ERROR_AUTHENTICATION_FAILED;
            case C::CRYPTO_ERROR_INVALID_PARAMETERS:
                return CRYPTO_ERROR_INVALID_PARAMETERS;
            case C::CRYPTO_ERROR_OPERATION_FAILED:
                return CRYPTO_ERROR_OPERATION_FAILED;
            case C::CRYPTO_ERROR_LOCATOR_NOT_AVAILABLE:
                return CRYPTO_ERROR_LOCATOR_NOT_AVAILABLE;
            case C::CRYPTO_ERROR_INVALID_KEY_NAME:
                return CRYPTO_ERROR_INVALID_KEY_NAME;
            case C::CRYPTO_ERROR_INVALID_KEY_TYPE:
                return CRYPTO_ERROR_INVALID_KEY_TYPE;
            case C::CRYPTO_ERROR_INVALID_KEY_ID:
                return CRYPTO_ERROR_INVALID_KEY_ID;
            case C::CRYPTO_ERROR_INVALID_SESSION_STATE:
                return CRYPTO_ERROR_INVALID_SESSION_STATE;
            case C::CRYPTO_ERROR_INVALID_INIT_DATA:
                return CRYPTO_ERROR_INVALID_INIT_DATA;
            case C::CRYPTO_ERROR_OVERSIZED_INPUT_DATA:
                return CRYPTO_ERROR_OVERSIZED_INPUT_DATA;
            case C::CRYPTO_ERROR_BUFFER_OVERFLOW:
                return CRYPTO_ERROR_BUFFER_OVERFLOW;
            case C::CRYPTO_ERROR_KEY_PROVISION_ERROR:
                return CRYPTO_ERROR_KEY_PROVISION_ERROR;
            case C::CRYPTO_ERROR_SEALED_KEY_EXPORT_ERROR:
                return CRYPTO_ERROR_SEALED_KEY_EXPORT_ERROR;
            default:
                return std::nullopt;
            }
        }

        C::BufferRegion To(const BufferRegion &r) { return { r.offset(), r.length() }; }

        SharedBuffers::Region Region(const BufferRegion &r) { return { r.offset(), r.length() }; }

        void Copy(BufferRegion *out, C::BufferRegion r)
        {
            out->set_offset(r.offset);
            out->set_length(r.length);
        }

        std::vector<uint8_t> Bytes(const std::string &value) { return { value.begin(), value.end() }; }

        bool Output(CryptoError error, const SharedBuffers::Lease &lease, C::BufferRegion out,
            const SharedBuffers::Region *inputs, size_t count)
        {
            if (error != CRYPTO_ERROR_NONE)
                return true; // BUFFER_OVERFLOW is required capacity, never produced data.
            const SharedBuffers::Region region { out.offset, out.length };
            if (!SharedBuffers::Inside(region, lease.Capacity()))
                return false;
            for (size_t i = 0; i < count; ++i)
                if (SharedBuffers::Overlap(region, inputs[i]))
                    return false;
            return true;
        }
    }

    template <class F> bool CryptoServiceAdapter::Invoke(CryptoError &error, F &&call, SharedBuffers::Lease *lease)
    {
        Crypto *api = nullptr;
        try
        {
            api = resolver();
        }
        catch (...)
        {
            log.Error(Reason::BackendLookup, "ICrypto lookup raised an exception");
            return false;
        }
        if (!api)
        {
            log.Error(Reason::BackendLookup, "ICrypto lookup failed");
            return false;
        }

        Crypto::CryptoError backendError = Crypto::CRYPTO_ERROR_OPERATION_FAILED;
        WPEFramework::Core::hresult result;
        try
        {
            result = call(*api, backendError);
        }
        catch (...)
        {
            // Local stack unwinding ends the synchronous call. A proxy exception
            // can interrupt response handling without proving remote completion.
            const bool remote = dynamic_cast<WPEFramework::ProxyStub::UnknownProxyType<Crypto> *>(api) != nullptr;
            api->Release();
            if (lease && remote)
                lease->Uncertain();
            log.Error(Reason::BackendCall, "ICrypto invocation raised an exception");
            return false;
        }
        api->Release();
        // Mapping, serialization and egress run after releasing the interface.
        if (result != WPEFramework::Core::ERROR_NONE)
        {
            // R4 proxies mark transport failures with COM_ERROR. An ordinary
            // method HRESULT is a completed invocation, including error returns.
            if (lease && (result & COM_ERROR))
                lease->Uncertain();
            log.Error(Reason::BackendCall, "ICrypto invocation failed code=%u", result);
            return false;
        }
        const auto mapped = From(backendError);
        if (!mapped)
        {
            log.Error(Reason::BackendCall, "invalid backend CryptoError=%u", static_cast<unsigned>(backendError));
            return false;
        }
        error = *mapped;
        return true;
    }

    bool CryptoServiceAdapter::Notify(const Connection &owner, bool connected)
    {
        try
        {
            const Crypto::ApplicationContext context { owner.appId, owner.sessionId };
            CryptoError error = CRYPTO_ERROR_OPERATION_FAILED;
            if (!Invoke(error,
                    [&](Crypto &api, Crypto::CryptoError &error)
                    {
                        return api.NotifyAppState(context,
                            connected ? Crypto::APP_CONNECTION_STATE_CONNECTED
                                      : Crypto::APP_CONNECTION_STATE_DISCONNECTED,
                            error);
                    }))
                return false;
            if (error != CRYPTO_ERROR_NONE)
                log.Error(Reason::BackendCall, "NotifyAppState failed service_error=%d", error);
            return error == CRYPTO_ERROR_NONE;
        }
        catch (...)
        {
            log.Error(Reason::BackendCall, "NotifyAppState could not complete");
            return false;
        }
    }

    ServiceResult CryptoServiceAdapter::Execute(const std::shared_ptr<Connection> &owner, const std::string &payload)
    {
        LogScope scope;
        scope.Context().connection = owner->peer.lifetime;
        scope.Context().uid = owner->peer.uid;
        scope.Context().service = RPC_SERVICE_CRYPTO;
        const auto invalid = [&](const char *reason)
        {
            log.Error(Reason::Protocol, "Crypto request rejected: %s", reason);
            return ServiceResult {};
        };
        // Protobuf/STL storage can throw; validation and backend results use returns.
        try
        {
            CryptoRequest request;
            if (!(payload.size() <= INT_MAX
                    && request.ParseFromArray(payload.data(), static_cast<int>(payload.size()))))
                return invalid("invalid Crypto payload");
            scope.Context().operation = request.operation();
            if (!(request.operation() > CRYPTO_OPERATION_UNSPECIFIED
                    && request.operation() <= CRYPTO_OPERATION_RELEASE_SHARED_BUFFER))
                return invalid("invalid Crypto payload");
            if (static_cast<int>(request.payload_case()) != static_cast<int>(request.operation()) + 1)
                return invalid("invalid Crypto payload");
            CryptoResponse response;
            const Crypto::ApplicationContext context { owner->appId, owner->sessionId };
            CryptoError error = CRYPTO_ERROR_NONE;
            switch (request.operation())
            {
            case CRYPTO_OPERATION_LOAD_KEY:
            {
                const auto &r = request.load_key();
                auto *out = response.mutable_load_key();
                if (r.locator().empty())
                    return invalid("invalid Crypto payload");
                const auto provision = To(r.provision_type());
                if (!provision)
                    return invalid("unknown Crypto enum");
                uint32_t key = 0;
                if (!Invoke(error, [&](C &api, C::CryptoError &e)
                        { return api.LoadKey(context, r.locator(), *provision, key, e); }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                    out->set_key_id(key);
                break;
            }
            case CRYPTO_OPERATION_KEY_EXISTS:
            {
                const auto &r = request.key_exists();
                auto *out = response.mutable_key_exists();
                if (r.locator().empty())
                    return invalid("invalid Crypto payload");
                bool exists = false;
                uint32_t key = 0;
                if (!Invoke(error,
                        [&](C &api, C::CryptoError &e) { return api.KeyExists(context, r.locator(), exists, key, e); }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                {
                    out->set_exists(exists);
                    if (exists)
                        out->set_key_id(key);
                }
                break;
            }
            case CRYPTO_OPERATION_FLUSH_KEYS:
            {
                response.mutable_flush_keys();
                if (!Invoke(error, [&](C &api, C::CryptoError &e) { return api.FlushKeys(context, e); }))
                    return {};
                break;
            }
            case CRYPTO_OPERATION_GENERATE_KEY_PAIR:
            {
                const auto &r = request.generate_key_pair();
                auto *out = response.mutable_generate_key_pair();
                const auto algorithm = To(r.algorithm());
                if (!algorithm)
                    return invalid("unknown Crypto enum");
                if (!(r.has_auth_dh() && !r.auth_dh().prime_p().empty() && r.auth_dh().generator_g() > 1))
                    return invalid("invalid Crypto payload");
                C::KeyPairParameters parameters { { Bytes(r.auth_dh().prime_p()), r.auth_dh().generator_g() } };
                uint32_t priv = 0, pub = 0;
                if (!Invoke(error, [&](C &api, C::CryptoError &e)
                        { return api.GenerateKeyPair(context, *algorithm, parameters, priv, pub, e); }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                {
                    out->set_private_key_id(priv);
                    out->set_public_key_id(pub);
                }
                break;
            }
            case CRYPTO_OPERATION_DERIVE_SESSION_KEYS:
            {
                const auto &r = request.derive_session_keys();
                auto *out = response.mutable_derive_session_keys();
                const auto algorithm = To(r.algorithm());
                if (!algorithm)
                    return invalid("unknown Crypto enum");
                uint32_t enc = 0, hmac = 0, wrap = 0;
                if (!Invoke(error,
                        [&](C &api, C::CryptoError &e)
                        {
                            return api.DeriveSessionKeys(context, *algorithm, r.private_key_id(), r.public_key_id(),
                                r.derivation_key_id(), enc, hmac, wrap, e);
                        }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                {
                    out->set_encryption_key_id(enc);
                    out->set_hmac_key_id(hmac);
                    out->set_wrapping_key_id(wrap);
                }
                break;
            }
            case CRYPTO_OPERATION_GET_KEY_INFO:
            {
                const auto &r = request.get_key_info();
                auto *out = response.mutable_get_key_info();
                C::KeyType type = C::KEY_TYPE_UNSPECIFIED;
                C::KeyAlgorithm algorithm = C::KEY_ALGORITHM_UNSPECIFIED;
                std::vector<C::KeyUsage> usage;
                if (!Invoke(error, [&](C &api, C::CryptoError &e)
                        { return api.GetKeyInfo(context, r.key_id(), type, usage, algorithm, e); }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                {
                    const auto mappedType = From(type);
                    const auto mappedAlgorithm = From(algorithm);
                    if (!mappedType || !mappedAlgorithm)
                        return invalid("unknown backend key metadata");
                    out->set_key_type(*mappedType);
                    out->set_key_algorithm(*mappedAlgorithm);
                    for (auto u : usage)
                    {
                        const auto mapped = From(u);
                        if (!mapped)
                            return invalid("unknown backend key usage");
                        out->add_key_usage(*mapped);
                    }
                }
                break;
            }
            case CRYPTO_OPERATION_GENERATE_KEY:
            {
                const auto &r = request.generate_key();
                auto *out = response.mutable_generate_key();
                const auto algorithm = To(r.algorithm());
                if (!algorithm)
                    return invalid("unknown Crypto enum");
                if (r.key_usage_size() <= 0)
                    return invalid("invalid Crypto payload");
                std::vector<C::KeyUsage> usage;
                unsigned seen = 0;
                for (auto u : r.key_usage())
                {
                    const auto mapped = To(static_cast<KeyUsage>(u));
                    if (!mapped)
                        return invalid("unknown Crypto enum");
                    const auto bit = 1u << static_cast<unsigned>(*mapped);
                    if (seen & bit)
                        return invalid("invalid Crypto payload");
                    seen |= bit;
                    usage.push_back(*mapped);
                }
                uint32_t key = 0;
                if (!Invoke(error,
                        [&](C &api, C::CryptoError &e) { return api.GenerateKey(context, *algorithm, usage, key, e); }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                    out->set_key_id(key);
                break;
            }
            case CRYPTO_OPERATION_UNLOAD_KEY:
            {
                response.mutable_unload_key();
                if (!Invoke(error, [&](C &api, C::CryptoError &e)
                        { return api.UnloadKey(context, request.unload_key().key_id(), e); }))
                    return {};
                break;
            }
            case CRYPTO_OPERATION_IMPORT_KEY:
            {
                const auto &r = request.import_key();
                auto *out = response.mutable_import_key();
                const auto type = To(r.key_type());
                if (!type)
                    return invalid("unknown Crypto enum");
                WPEFramework::Core::OptionalType<C::ImportExportType> format;
                if (r.has_import_export_type())
                {
                    const auto mapped = To(r.import_export_type());
                    if (!mapped)
                        return invalid("unknown import/export type");
                    format = *mapped;
                }
                const auto blob = Bytes(r.key_blob());
                if (blob.empty())
                    return invalid("invalid Crypto payload");
                uint32_t key = 0;
                if (!Invoke(error,
                        [&](C &api, C::CryptoError &e) { return api.ImportKey(context, *type, format, blob, key, e); }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                    out->set_key_id(key);
                break;
            }
            case CRYPTO_OPERATION_EXPORT_KEY:
            {
                const auto &r = request.export_key();
                auto *out = response.mutable_export_key();
                WPEFramework::Core::OptionalType<C::ImportExportType> format;
                if (r.has_import_export_type())
                {
                    const auto mapped = To(r.import_export_type());
                    if (!mapped)
                        return invalid("unknown import/export type");
                    format = *mapped;
                }
                std::vector<uint8_t> blob;
                if (!Invoke(error,
                        [&](C &api, C::CryptoError &e) { return api.ExportKey(context, r.key_id(), format, blob, e); }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                    out->set_key_blob(blob.data(), blob.size());
                break;
            }
            case CRYPTO_OPERATION_GENERATE_RANDOM:
            {
                const auto &r = request.generate_random();
                auto *out = response.mutable_generate_random();
                if (!r.number_of_bytes())
                    return invalid("invalid Crypto payload");
                std::vector<uint8_t> random;
                if (!Invoke(error, [&](C &api, C::CryptoError &e)
                        { return api.GenerateRandom(context, r.number_of_bytes(), random, e); }))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                {
                    if (random.size() != r.number_of_bytes())
                        return invalid("invalid Crypto payload");
                    out->set_random_value(random.data(), random.size());
                }
                break;
            }
            case CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER:
            {
                auto *out = response.mutable_allocate_shared_buffer();
                const auto allocation = buffers.Allocate(owner, request.allocate_shared_buffer().requested_size());
                error = allocation.error;
                if (error == CRYPTO_ERROR_NONE)
                {
                    out->set_buffer_id(allocation.id);
                    out->set_name(allocation.name);
                    out->set_size(allocation.size);
                }
                break;
            }
            case CRYPTO_OPERATION_RELEASE_SHARED_BUFFER:
            {
                response.mutable_release_shared_buffer();
                error = buffers.Release(owner, request.release_shared_buffer().buffer_id());
                break;
            }
            case CRYPTO_OPERATION_ENCRYPT:
            {
                const auto &r = request.encrypt();
                auto *out = response.mutable_encrypt();
                const auto algorithm = To(r.algorithm());
                if (!algorithm)
                    return invalid("unknown Crypto enum");
                if (!r.has_input_data() || !r.has_init_data())
                {
                    error = CRYPTO_ERROR_INVALID_PARAMETERS;
                    break;
                }
                const SharedBuffers::Region regions[] = { Region(r.input_data()), Region(r.init_data()) };
                SharedBuffers::Lease lease;
                error = buffers.TryUse(owner, r.buffer_id(), regions, std::size(regions), lease);
                if (error != CRYPTO_ERROR_NONE)
                    break;
                C::SharedBuffer buffer { lease.Path() };
                C::BufferRegion result {};
                if (!Invoke(
                        error,
                        [&](C &api, C::CryptoError &e) {
                            return api.Encrypt(context, buffer, *algorithm, r.key_id(), To(r.init_data()),
                                To(r.input_data()), result, e);
                        },
                        &lease))
                    return {};
                if (!Output(error, lease, result, regions, std::size(regions)))
                    return invalid("invalid backend output region");
                if (error == CRYPTO_ERROR_NONE || error == CRYPTO_ERROR_BUFFER_OVERFLOW)
                    Copy(out->mutable_encrypted_data(), result);
                break;
            }
            case CRYPTO_OPERATION_DECRYPT:
            {
                const auto &r = request.decrypt();
                auto *out = response.mutable_decrypt();
                const auto algorithm = To(r.algorithm());
                if (!algorithm)
                    return invalid("unknown Crypto enum");
                if (!r.has_encrypted_data() || !r.has_init_data())
                {
                    error = CRYPTO_ERROR_INVALID_PARAMETERS;
                    break;
                }
                const SharedBuffers::Region regions[] = { Region(r.encrypted_data()), Region(r.init_data()) };
                SharedBuffers::Lease lease;
                error = buffers.TryUse(owner, r.buffer_id(), regions, std::size(regions), lease);
                if (error != CRYPTO_ERROR_NONE)
                    break;
                C::SharedBuffer buffer { lease.Path() };
                C::BufferRegion result {};
                if (!Invoke(
                        error,
                        [&](C &api, C::CryptoError &e) {
                            return api.Decrypt(context, buffer, *algorithm, r.key_id(), To(r.init_data()),
                                To(r.encrypted_data()), result, e);
                        },
                        &lease))
                    return {};
                if (!Output(error, lease, result, regions, std::size(regions)))
                    return invalid("invalid backend output region");
                if (error == CRYPTO_ERROR_NONE || error == CRYPTO_ERROR_BUFFER_OVERFLOW)
                    Copy(out->mutable_output_data(), result);
                break;
            }
            case CRYPTO_OPERATION_SIGN:
            {
                const auto &r = request.sign();
                auto *out = response.mutable_sign();
                const auto algorithm = To(r.algorithm());
                if (!algorithm)
                    return invalid("unknown Crypto enum");
                const auto digest = To(r.digest_algorithm());
                if (!digest)
                    return invalid("unknown Crypto enum");
                if (!r.has_input_data())
                {
                    error = CRYPTO_ERROR_INVALID_PARAMETERS;
                    break;
                }
                const SharedBuffers::Region regions[] = { Region(r.input_data()) };
                SharedBuffers::Lease lease;
                error = buffers.TryUse(owner, r.buffer_id(), regions, std::size(regions), lease);
                if (error != CRYPTO_ERROR_NONE)
                    break;
                C::SharedBuffer buffer { lease.Path() };
                C::BufferRegion result {};
                if (!Invoke(
                        error,
                        [&](C &api, C::CryptoError &e) {
                            return api.Sign(
                                context, buffer, *algorithm, r.key_id(), *digest, To(r.input_data()), result, e);
                        },
                        &lease))
                    return {};
                if (!Output(error, lease, result, regions, std::size(regions)))
                    return invalid("invalid backend output region");
                if (error == CRYPTO_ERROR_NONE || error == CRYPTO_ERROR_BUFFER_OVERFLOW)
                    Copy(out->mutable_signature(), result);
                break;
            }
            case CRYPTO_OPERATION_VERIFY:
            {
                const auto &r = request.verify();
                auto *out = response.mutable_verify();
                const auto algorithm = To(r.algorithm());
                if (!algorithm)
                    return invalid("unknown Crypto enum");
                const auto digest = To(r.digest_algorithm());
                if (!digest)
                    return invalid("unknown Crypto enum");
                if (!r.has_input_data() || !r.has_expected_signature())
                {
                    error = CRYPTO_ERROR_INVALID_PARAMETERS;
                    break;
                }
                const SharedBuffers::Region regions[] = { Region(r.input_data()), Region(r.expected_signature()) };
                SharedBuffers::Lease lease;
                error = buffers.TryUse(owner, r.buffer_id(), regions, std::size(regions), lease);
                if (error != CRYPTO_ERROR_NONE)
                    break;
                C::SharedBuffer buffer { lease.Path() };
                bool result = false;
                if (!Invoke(
                        error,
                        [&](C &api, C::CryptoError &e)
                        {
                            return api.Verify(context, buffer, *algorithm, r.key_id(), *digest, To(r.input_data()),
                                To(r.expected_signature()), result, e);
                        },
                        &lease))
                    return {};
                if (error == CRYPTO_ERROR_NONE)
                    out->set_verification_status(result);
                break;
            }
            default:
                return invalid("unsupported Crypto operation");
            }
            response.set_error(error);
            if (error != CRYPTO_ERROR_NONE)
                log.Error(Reason::BackendCall, "Crypto operation=%d service_error=%d", request.operation(), error);
            ServiceResult result { RPC_STATUS_OK, {} };
            if (!response.SerializeToString(&result.payload))
                return invalid("Crypto response serialization failed");
            return result;
        }
        catch (const std::exception &e)
        {
            log.Error(Reason::Protocol, "Crypto request failed: %s", e.what());
            return {};
        }
        catch (...)
        {
            log.Error(Reason::BackendCall, "Crypto request failed");
            return {};
        }
    }
}
