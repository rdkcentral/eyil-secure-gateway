/**
 * If not stated otherwise in this file or this component's LICENSE
 * file the following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "EyilGateway.h"
#include "Version.h"

namespace WPEFramework
{
    namespace
    {
        constexpr uint32_t ImplementationTimeoutMs = 5000;

        static Plugin::Metadata<Plugin::EyilGateway> metadata(
            // Version (Major, Minor, Patch)
            API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH,
            // Preconditions
            {},
            // Terminations
            {},
            // Controls
            {});
    }

    namespace Plugin
    {
        EyilGateway::EyilGateway()
            : _notification(this)
        {
        }

        const string EyilGateway::Initialize(PluginHost::IShell *service)
        {
            {
                Core::SafeSyncType<Core::CriticalSection> lock(_adminLock);
                if (service == nullptr || _service != nullptr)
                {
                    return "Invalid EyilGateway initialization";
                }
                _service = service;
                _service->AddRef();
            }

            service->Register(&_notification);

            uint32_t connectionId = 0;
            auto *implementation = service->Root<Exchange::IConfiguration>(
                connectionId, ImplementationTimeoutMs, _T("EyilGatewayImplementation"));
            {
                Core::SafeSyncType<Core::CriticalSection> lock(_adminLock);
                _implementation = implementation;
                _connectionId = connectionId;
            }
            if (implementation == nullptr)
            {
                return "EyilGateway implementation could not be created";
            }

            // The initial deployment uses synchronous destruction in this process.
            if (connectionId != 0)
            {
                return "EyilGateway requires in-process mode";
            }
            const Core::hresult result = implementation->Configure(service);
            if (result != Core::ERROR_NONE)
            {
                SYSLOG(Logging::Error, (_T("EyilGateway implementation configuration failed: %u"), result));
                return "EyilGateway implementation could not be configured";
            }

            SYSLOG(Logging::Notification, (_T("EyilGateway initialized, connectionId=%u"), connectionId));
            return {};
        }

        void EyilGateway::Deinitialize(PluginHost::IShell *service)
        {
            PluginHost::IShell *retainedService = nullptr;
            Exchange::IConfiguration *implementation = nullptr;
            uint32_t connectionId = 0;
            {
                Core::SafeSyncType<Core::CriticalSection> lock(_adminLock);
                if (_service == nullptr || _service != service)
                {
                    return;
                }
                // Transfer owned references and stop admitting notifications before draining.
                retainedService = _service;
                implementation = _implementation;
                connectionId = _connectionId;
                _service = nullptr;
                _implementation = nullptr;
                _connectionId = 0;
            }

            retainedService->Unregister(&_notification);
            RPC::IRemoteConnection *connection
                = connectionId != 0 ? retainedService->RemoteConnection(connectionId) : nullptr;

            if (implementation != nullptr)
            {
                // The wrapper owns the implementation's sole interface reference.
                // Its destructor drains the runtime before this final Release returns.
                const uint32_t released = implementation->Release();
                implementation = nullptr;
                ASSERT(released == Core::ERROR_DESTRUCTION_SUCCEEDED);
                (void)released;
            }

            if (connection != nullptr)
            {
                connection->Terminate();
                connection->Release();
                connection = nullptr;
            }

            retainedService->Release();
            retainedService = nullptr;
            SYSLOG(Logging::Notification, (_T("EyilGateway deinitialized")));
        }

        string EyilGateway::Information() const
        {
            return "EyilGateway: authenticated IPC routing and connection-owned shared memory";
        }

        void EyilGateway::Deactivated(RPC::IRemoteConnection *connection)
        {
            if (connection == nullptr)
                return;

            const uint32_t connectionId = connection->Id();
            PluginHost::IShell *service = nullptr;
            {
                Core::SafeSyncType<Core::CriticalSection> lock(_adminLock);
                if (_connectionId != 0 && connectionId == _connectionId && _service != nullptr)
                {
                    service = _service;
                    service->AddRef();
                }
            }
            if (service != nullptr)
            {
                // Job creation/submission can allocate in Thunder. Keep the shell alive
                // independently of Deinitialize, without holding our lock in framework code.
                try
                {
                    Core::IWorkerPool::Instance().Submit(PluginHost::IShell::Job::Create(
                        service, PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
                }
                catch (const std::exception &error)
                {
                    SYSLOG(Logging::Error, (_T("EyilGateway deactivation scheduling failed: %s"), error.what()));
                }
                catch (...)
                {
                    SYSLOG(Logging::Error, (_T("EyilGateway deactivation scheduling failed")));
                }
                service->Release();
                service = nullptr;
            }
        }
    }
}
