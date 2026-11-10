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

#include "EyilGatewayImplementation.h"
#include "Version.h"
#include <map>

namespace WPEFramework
{
    namespace Plugin
    {
        SERVICE_REGISTRATION(
            EyilGatewayImplementation, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

        namespace
        {
            void RuntimeLog(const char *text)
            {
                if (std::strstr(text, " WARNING "))
                    SYSLOG(Logging::Notification, (_T("%s"), text));
                else
                    SYSLOG(Logging::Error, (_T("%s"), text));
            }

            class BackendConfig : public Core::JSON::Container
            {
            public:
                BackendConfig()
                    : Callsign("")
                {
                    Add("service", &Service);
                    Add("callsign", &Callsign);
                }

                BackendConfig(const BackendConfig &other)
                    : BackendConfig()
                {
                    Service = other.Service;
                    Callsign = other.Callsign;
                }

                Core::JSON::String Service;
                Core::JSON::String Callsign;
            };

            class GatewayConfig : public Core::JSON::Container
            {
            public:
                GatewayConfig()
                    : Workers(4)
                {
                    Add("backends", &Backends);
                    Add("workers", &Workers);
                }

                Core::JSON::ArrayType<BackendConfig> Backends;
                Core::JSON::DecUInt16 Workers;
            };

            struct Settings
            {
                std::map<std::string, std::string> backends;
                unsigned workers = 4;
            };

            Core::hresult ReadSettings(const PluginHost::IShell &service, Settings &settings)
            {
                // JSON parsing and owned strings may allocate. Keep that failure
                // boundary separate from configuration validation and runtime startup.
                try
                {
                    GatewayConfig config;
                    const auto text = service.ConfigLine();
                    const auto first = text.find_first_not_of(" \t\r\n");
                    const auto last = text.find_last_not_of(" \t\r\n");
                    // R4.4's Container parser rejects an empty object. An absent
                    // ConfigLine or a strictly empty JSON object uses our defaults.
                    const bool defaultsOnly = first == std::string::npos
                        || (text[first] == '{' && text[last] == '}'
                            && text.find_first_not_of(" \t\r\n", first + 1) == last);
                    if ((!defaultsOnly && !config.FromString(text)) || config.IsNull() || config.Backends.IsNull()
                        || config.Workers.IsNull() || !config.Workers.Value() || config.Workers.Value() > 255)
                        return Core::ERROR_BAD_REQUEST;

                    // Elements() skips unset/null containers; inspect every entry.
                    for (uint32_t index = 0; index < config.Backends.Length(); ++index)
                    {
                        const auto &entry = config.Backends[index];
                        if (entry.IsNull() || entry.Service.IsNull() || entry.Service.Value().empty()
                            || entry.Callsign.IsNull()
                            || !settings.backends.emplace(entry.Service.Value(), entry.Callsign.Value()).second)
                            return Core::ERROR_BAD_REQUEST;
                    }
                    settings.workers = config.Workers.Value();
                    return Core::ERROR_NONE;
                }
                catch (const std::exception &error)
                {
                    SYSLOG(Logging::Error, (_T("EyilGateway configuration failed: %s"), error.what()));
                }
                catch (...)
                {
                    SYSLOG(Logging::Error, (_T("EyilGateway configuration failed")));
                }
                return Core::ERROR_GENERAL;
            }
        }

        Core::hresult EyilGatewayImplementation::Configure(PluginHost::IShell *service)
        {
            if (!service || _service)
                return Core::ERROR_BAD_REQUEST;

            Settings settings;
            const auto result = ReadSettings(*service, settings);
            if (result != Core::ERROR_NONE)
                return result;

            const auto crypto = settings.backends.find("crypto");
            const std::string emptyCallsign;
            const auto &callsign = crypto == settings.backends.end() ? emptyCallsign : crypto->second;
            _service = service;
            _service->AddRef();

            // Captured strings and runtime allocation may throw in the C++ library.
            // Startup validation and transport failures return an explicit result.
            bool started = false;
            try
            {
                _runtime = std::make_unique<Eyil::GatewayRuntime>(
                    [this, callsign]() -> Exchange::ICrypto *
                    {
                        // Empty must not query the current plugin or a fallback callsign.
                        return callsign.empty() ? nullptr
                                                : _service->QueryInterfaceByCallsign<Exchange::ICrypto>(callsign);
                    },
                    settings.workers, Eyil::Endpoint, RuntimeLog, service->VolatilePath(),
                    [this]
                    {
                        // Thunder invokes the wrapper's Deinitialize on its own
                        // worker; never release the implementation on an I/O owner.
                        Core::IWorkerPool::Instance().Submit(PluginHost::IShell::Job::Create(
                            _service, PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
                    });
                started = _runtime->Start();
            }
            catch (const std::exception &e)
            {
                SYSLOG(Logging::Error, (_T("EyilGateway startup failed: %s"), e.what()));
            }
            catch (...)
            {
                SYSLOG(Logging::Error, (_T("EyilGateway startup failed")));
            }

            if (started)
            {
                SYSLOG(Logging::Notification,
                    (_T("EyilGateway ready, workers=%u, Crypto=%s"), settings.workers,
                        callsign.empty() ? "unconfigured" : callsign.c_str()));
                return Core::ERROR_NONE;
            }

            // Partial startup follows the same ownership order as destruction:
            // runtime shutdown completes while its ICrypto resolver can use IShell.
            _runtime.reset();
            if (_service)
            {
                _service->Release();
                _service = nullptr;
            }
            return Core::ERROR_GENERAL;
        }

        EyilGatewayImplementation::~EyilGatewayImplementation()
        {
            // GatewayRuntime's destructor closes admission, drains accepted work,
            // cleans sessions/buffers and joins every owned thread before returning.
            _runtime.reset();
            if (_service)
            {
                _service->Release();
                _service = nullptr;
            }
        }
    }
}
