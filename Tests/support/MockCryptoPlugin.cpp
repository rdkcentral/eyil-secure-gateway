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

#include "MockCrypto.h"
#include <plugins/plugins.h>

namespace WPEFramework
{
    namespace Plugin
    {
        // L2-only plugin: never installed by the production install target.
        class EyilCryptoMock : public PluginHost::IPlugin, public EyilTest::MockCrypto
        {
        public:
            BEGIN_INTERFACE_MAP(EyilCryptoMock)
            INTERFACE_ENTRY(PluginHost::IPlugin)
            INTERFACE_ENTRY(Exchange::ICrypto)

            END_INTERFACE_MAP
            const string Initialize(PluginHost::IShell *) override { return {}; }

            void Deinitialize(PluginHost::IShell *) override { }

            string Information() const override { return "L2 deterministic ICrypto mock; no real cryptography"; }
        };

        SERVICE_REGISTRATION(EyilCryptoMock, 1, 0, 0);
    }
}
