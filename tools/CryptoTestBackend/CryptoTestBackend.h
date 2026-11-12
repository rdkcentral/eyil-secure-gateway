/**
 * Copyright 2026 RDK Management
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifndef MODULE_NAME
#define MODULE_NAME Plugin_CryptoTestBackend
#endif

#include <plugins/plugins.h>
#include <interfaces/ICrypto.h>
#include <mutex>
#include <vector>

namespace WPEFramework::Plugin
{
    // Development backend only. The algorithms and keys below are test fixtures.
    class CryptoTestBackend : public PluginHost::IPlugin, public Exchange::ICrypto
    {
    public:
        BEGIN_INTERFACE_MAP(CryptoTestBackend)
        INTERFACE_ENTRY(PluginHost::IPlugin)
        INTERFACE_ENTRY(Exchange::ICrypto)
        END_INTERFACE_MAP

        const string Initialize(PluginHost::IShell *service) override;
        void Deinitialize(PluginHost::IShell *service) override;
        string Information() const override;

        Core::hresult NotifyAppState(
            const ApplicationContext &context, AppConnectionState state, CryptoError &error) override;
        Core::hresult GenerateRandom(const ApplicationContext &context, uint32_t numberOfBytes,
            std::vector<uint8_t> &randomValue, CryptoError &error) override;
        Core::hresult GenerateKey(const ApplicationContext &context, GenerateKeyAlgorithm algorithm,
            const std::vector<KeyUsage> &usage, uint32_t &keyId, CryptoError &error) override;
        Core::hresult GenerateKeyPair(const ApplicationContext &context, KeyPairAlgorithm algorithm,
            const KeyPairParameters &parameters, uint32_t &privateKeyId, uint32_t &publicKeyId,
            CryptoError &error) override;
        Core::hresult LoadKey(const ApplicationContext &context, const string &locator, ProvisionType provisionType,
            uint32_t &keyId, CryptoError &error) override;
        Core::hresult UnloadKey(const ApplicationContext &context, uint32_t keyId, CryptoError &error) override;
        Core::hresult KeyExists(const ApplicationContext &context, const string &locator, bool &exists, uint32_t &keyId,
            CryptoError &error) const override;
        Core::hresult GetKeyInfo(const ApplicationContext &context, uint32_t keyId, KeyType &keyType,
            std::vector<KeyUsage> &keyUsage, KeyAlgorithm &keyAlgorithm, CryptoError &error) const override;
        Core::hresult FlushKeys(const ApplicationContext &context, CryptoError &error) override;
        Core::hresult ImportKey(const ApplicationContext &context, KeyType keyType,
            const Core::OptionalType<ImportExportType> &importExportType, const std::vector<uint8_t> &keyBlob,
            uint32_t &keyId, CryptoError &error) override;
        Core::hresult ExportKey(const ApplicationContext &context, uint32_t keyId,
            const Core::OptionalType<ImportExportType> &importExportType, std::vector<uint8_t> &keyBlob,
            CryptoError &error) const override;
        Core::hresult DeriveSessionKeys(const ApplicationContext &context, KeyPairAlgorithm algorithm,
            uint32_t privateKeyId, uint32_t publicKeyId, uint32_t derivationKeyId, uint32_t &encryptionKeyId,
            uint32_t &hmacKeyId, uint32_t &wrappingKeyId, CryptoError &error) override;

        Core::hresult Encrypt(const ApplicationContext &context, const SharedBuffer &buffer, CipherAlgorithm algorithm,
            uint32_t keyId, const BufferRegion &initData, const BufferRegion &inputData, BufferRegion &encryptedData,
            CryptoError &error) override;
        Core::hresult Decrypt(const ApplicationContext &context, const SharedBuffer &buffer, CipherAlgorithm algorithm,
            uint32_t keyId, const BufferRegion &initData, const BufferRegion &encryptedData, BufferRegion &outputData,
            CryptoError &error) override;
        Core::hresult Sign(const ApplicationContext &context, const SharedBuffer &buffer, SignatureAlgorithm algorithm,
            uint32_t keyId, DigestAlgorithm digestAlgorithm, const BufferRegion &inputData, BufferRegion &signature,
            CryptoError &error) override;
        Core::hresult Verify(const ApplicationContext &context, const SharedBuffer &buffer,
            SignatureAlgorithm algorithm, uint32_t keyId, DigestAlgorithm digestAlgorithm,
            const BufferRegion &inputData, const BufferRegion &expectedSignature, bool &verificationStatus,
            CryptoError &error) override;

    private:
        bool Session(const ApplicationContext &context, CryptoError &error) const;
        bool Key(const ApplicationContext &context, uint32_t keyId, CryptoError &error) const;

        mutable std::mutex _mutex;
        std::vector<ApplicationContext> _sessions;
    };
}
