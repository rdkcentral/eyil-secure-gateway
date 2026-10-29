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

#include <ProtocolVersion.h>
#include <eyilclient/EyilClient.h>
#include <rpc.pb.h>

#include <string>

int main()
{
    Client *client = nullptr;
    if (client_connect(&client, nullptr) != CLIENT_NOT_IMPLEMENTED || client != nullptr)
    {
        return 1;
    }
    RpcResponse response;
    if (response.status() != RPC_STATUS_UNSPECIFIED)
    {
        return 1;
    }
    response.set_request_id(42);
    response.set_service(RPC_SERVICE_CRYPTO);
    response.set_schema_version(EYIL_RPC_SCHEMA_VERSION);
    response.set_status(RPC_STATUS_OK);
    std::string wire;
    if (!response.SerializeToString(&wire))
    {
        return 1;
    }
    RpcResponse decoded;
    return decoded.ParseFromString(wire) && decoded.request_id() == 42
            && decoded.schema_version() == EYIL_RPC_SCHEMA_VERSION && decoded.status() == RPC_STATUS_OK
        ? 0
        : 1;
}
