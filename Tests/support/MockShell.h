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
#include <plugins/plugins.h>
#include <functional>

namespace EyilTest
{
    class MockShell : public WPEFramework::PluginHost::IShell
    {
    public:
        MockCrypto crypto;
        mutable std::atomic<unsigned> references { 1 };
        mutable std::atomic<bool> prematureRelease { false };
        std::atomic<unsigned> lookups { 0 };
        std::atomic<unsigned> deactivations { 0 };
        std::function<void(reason)> onDeactivate;
        std::string volatilePath;
        std::string config = R"({"workers":2,"backends":[{"service":"crypto","callsign":"test.crypto"}]})";

        uint32_t AddRef() const override { return ++references; }

        uint32_t Release() const override
        {
            if (crypto.references != 1)
                prematureRelease = true;
            return --references;
        }

        void *QueryInterface(uint32_t id) override
        {
            if (id == ID || id == Core::IUnknown::ID)
            {
                AddRef();
                return static_cast<IShell *>(this);
            }
            return nullptr;
        }

        void *QueryInterfaceByCallsign(uint32_t id, const string &name) override
        {
            ++lookups;
            if (references < 2)
                prematureRelease = true;
            return id == WPEFramework::Exchange::ICrypto::ID && name == "test.crypto" ? crypto.Acquire() : nullptr;
        }

        string ConfigLine() const override { return config; }

        Core::hresult ConfigLine(const string &s) override
        {
            config = s;
            return Core::ERROR_NONE;
        }

        void EnableWebServer(const string &, const string &) override { }

        void DisableWebServer() override { }

        string Model() const override { return {}; }

        string Accessor() const override { return {}; }

        string WebPrefix() const override { return {}; }

        string Locator() const override { return {}; }

        string ClassName() const override { return {}; }

        string Versions() const override { return {}; }

        string Callsign() const override { return {}; }

        string PersistentPath() const override { return {}; }

        string VolatilePath() const override { return volatilePath; }

        string DataPath() const override { return {}; }

        string ProxyStubPath() const override { return {}; }

        string SystemPath() const override { return {}; }

        string PluginPath() const override { return {}; }

        string SystemRootPath() const override { return {}; }

        string HashKey() const override { return {}; }

        Core::hresult SystemRootPath(const string &) override { return Core::ERROR_NONE; }

        bool Background() const override { return false; }

        startup Startup() const override { return startup::ACTIVATED; }

        Core::hresult Startup(startup) override { return Core::ERROR_NONE; }

        string Substitute(const string &s) const override { return s; }

        bool Resumed() const override { return true; }

        Core::hresult Resumed(bool) override { return Core::ERROR_NONE; }

        Core::hresult Metadata(string &) const override { return Core::ERROR_NONE; }

        bool IsSupported(uint8_t) const override { return true; }

        WPEFramework::PluginHost::ISubSystem *SubSystems() override { return nullptr; }

        void Notify(const string &) override { }

        void Register(WPEFramework::PluginHost::IPlugin::INotification *) override { }

        void Unregister(WPEFramework::PluginHost::IPlugin::INotification *) override { }

        state State() const override { return deactivations ? DEACTIVATED : ACTIVATED; }

        Core::hresult Activate(reason) override { return Core::ERROR_NONE; }

        Core::hresult Deactivate(reason why) override
        {
            if (onDeactivate)
                onDeactivate(why);
            ++deactivations;
            return Core::ERROR_NONE;
        }

        Core::hresult Unavailable(reason) override { return Core::ERROR_NONE; }

        Core::hresult Hibernate(uint32_t) override { return Core::ERROR_NONE; }

        reason Reason() const override { return REQUESTED; }

        uint32_t Submit(uint32_t, const Core::ProxyType<Core::JSON::IElement> &) override { return Core::ERROR_NONE; }

        ICOMLink *COMLink() override { return nullptr; }
    };
}
