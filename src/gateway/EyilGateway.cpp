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
        SERVICE_REGISTRATION(EyilGateway, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

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

            _implementation
                = _service->Root<Exchange::IConfiguration>(_connectionId, 5000, _T("EyilGatewayImplementation"));
            if (_implementation == nullptr)
            {
                Deinitialize(service);
                return "EyilGateway implementation could not be created";
            }

            const Core::hresult result = _implementation->Configure(_service);
            if (result != Core::ERROR_NONE)
            {
                SYSLOG(Logging::Error, (_T("EyilGateway implementation configuration failed: %u"), result));
                Deinitialize(service);
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
                _implementation->Release();
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
            return "EyilGateway skeleton; request processing is not implemented";
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
