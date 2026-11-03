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

#include "EyilGatewayImplementation.h"
#include "Version.h"

#include <ProtocolVersion.h>
#include <rpc.pb.h>
#include <zmq.h>

namespace WPEFramework
{
    namespace Plugin
    {
        SERVICE_REGISTRATION(
            EyilGatewayImplementation, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

        Core::hresult EyilGatewayImplementation::Configure(PluginHost::IShell *service)
        {
            if (service == nullptr || _configured)
            {
                return Core::ERROR_BAD_REQUEST;
            }

            int major = 0;
            int minor = 0;
            int patch = 0;
            zmq_version(&major, &minor, &patch);
            SYSLOG(Logging::Notification,
                (_T("EyilGateway implementation configured for %s: ZeroMQ %d.%d.%d, schema %u, status %s"),
                    service->Callsign().c_str(), major, minor, patch, EYIL_RPC_SCHEMA_VERSION,
                    RpcStatus_Name(RPC_STATUS_OK).c_str()));
            _configured = true;
            // TODO(DISPATCH): submit a distinct Thunder worker-pool job for each request.
            return Core::ERROR_NONE;
        }

        EyilGatewayImplementation::~EyilGatewayImplementation()
        {
            if (_configured)
            {
                SYSLOG(Logging::Notification, (_T("EyilGateway implementation released")));
            }
        }
    }
}
