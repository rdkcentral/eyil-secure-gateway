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
            if (service == nullptr || _service != nullptr)
            {
                return "Invalid EyilGateway initialization";
            }

            _service = service;
            _service->AddRef();
            _service->Register(&_notification);

            _implementation = _service->Root<Exchange::IConfiguration>(
                _connectionId, ImplementationTimeoutMs, _T("EyilGatewayImplementation"));
            if (_implementation == nullptr)
            {
                return "EyilGateway implementation could not be created";
            }

            // The initial deployment uses synchronous destruction in this process.
            if (_connectionId != 0)
            {
                return "EyilGateway requires in-process mode";
            }
            const Core::hresult result = _implementation->Configure(_service);
            if (result != Core::ERROR_NONE)
            {
                SYSLOG(Logging::Error, (_T("EyilGateway implementation configuration failed: %u"), result));
                return "EyilGateway implementation could not be configured";
            }

            SYSLOG(Logging::Notification, (_T("EyilGateway initialized, connectionId=%u"), _connectionId));
            return {};
        }

        void EyilGateway::Deinitialize(PluginHost::IShell *service)
        {
            if (_service == nullptr || _service != service)
            {
                return;
            }

            _service->Unregister(&_notification);
            RPC::IRemoteConnection *connection
                = _connectionId != 0 ? _service->RemoteConnection(_connectionId) : nullptr;

            if (_implementation != nullptr)
            {
                // The wrapper owns the implementation's sole interface reference.
                // Its destructor drains the runtime before this final Release returns.
                const uint32_t released = _implementation->Release();
                ASSERT(released == Core::ERROR_DESTRUCTION_SUCCEEDED);
                (void)released;
                _implementation = nullptr;
            }

            if (connection != nullptr)
            {
                connection->Terminate();
                connection->Release();
            }

            _connectionId = 0;
            _service->Release();
            _service = nullptr;
            SYSLOG(Logging::Notification, (_T("EyilGateway deinitialized")));
        }

        string EyilGateway::Information() const
        {
            return "EyilGateway: authenticated IPC routing and connection-owned shared memory";
        }

        void EyilGateway::Deactivated(RPC::IRemoteConnection *connection)
        {
            if (connection != nullptr && _connectionId != 0 && connection->Id() == _connectionId && _service != nullptr)
            {
                Core::IWorkerPool::Instance().Submit(PluginHost::IShell::Job::Create(
                    _service, PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
            }
        }
    }
}
