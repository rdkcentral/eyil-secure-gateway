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

#pragma once

#include "Module.h"

#include <interfaces/IConfiguration.h>

namespace WPEFramework
{
    namespace Plugin
    {
        class EyilGateway : public PluginHost::IPlugin
        {
        public:
            EyilGateway();
            ~EyilGateway() override = default;
            EyilGateway(const EyilGateway &) = delete;
            EyilGateway &operator=(const EyilGateway &) = delete;

            BEGIN_INTERFACE_MAP(EyilGateway)
            INTERFACE_ENTRY(PluginHost::IPlugin)
            END_INTERFACE_MAP

            const string Initialize(PluginHost::IShell *service) override;
            void Deinitialize(PluginHost::IShell *service) override;
            string Information() const override;

        private:
            void Deactivated(RPC::IRemoteConnection *connection);

            class Notification : public RPC::IRemoteConnection::INotification
            {
            public:
                explicit Notification(EyilGateway *parent)
                    : _parent(*parent)
                {
                }

                BEGIN_INTERFACE_MAP(Notification)
                INTERFACE_ENTRY(RPC::IRemoteConnection::INotification)
                END_INTERFACE_MAP

                void Activated(RPC::IRemoteConnection *) override { }

                void Deactivated(RPC::IRemoteConnection *connection) override { _parent.Deactivated(connection); }

            private:
                EyilGateway &_parent;
            };

            PluginHost::IShell *_service = nullptr;
            Exchange::IConfiguration *_implementation = nullptr;
            uint32_t _connectionId = 0;
            Core::Sink<Notification> _notification;
        };
    }
}
