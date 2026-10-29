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

#include <eyilclient/EyilClient.h>

#include <cstdlib>
#include <zmq.h>

static_assert(ZMQ_VERSION_MAJOR >= 4, "EyilClient requires ZeroMQ 4");

extern "C"
{

    ClientResult client_connect(Client **client, ClientConnectionEventHandler)
    {
        if (client == nullptr)
        {
            return CLIENT_INVALID_ARGUMENT;
        }
        *client = nullptr;
        return CLIENT_NOT_IMPLEMENTED;
    }

    ClientResult client_send_request(Client *, const uint8_t *, size_t, uint8_t **response_data, size_t *response_size)
    {
        if (response_data != nullptr)
        {
            *response_data = nullptr;
        }
        if (response_size != nullptr)
        {
            *response_size = 0;
        }
        if (response_data == nullptr || response_size == nullptr)
        {
            return CLIENT_INVALID_ARGUMENT;
        }
        return CLIENT_NOT_IMPLEMENTED;
    }

    ClientResult client_send_request_async(Client *, const uint8_t *, size_t, ClientResponseHandler)
    {
        return CLIENT_NOT_IMPLEMENTED;
    }

    ClientResult client_send_request_string(Client *, const char *, char **response_data)
    {
        if (response_data == nullptr)
        {
            return CLIENT_INVALID_ARGUMENT;
        }
        *response_data = nullptr;
        return CLIENT_NOT_IMPLEMENTED;
    }

    ClientResult client_send_request_string_async(Client *, const char *, ClientStringResponseHandler)
    {
        return CLIENT_NOT_IMPLEMENTED;
    }

    void client_free_response(void *response_data) { std::free(response_data); }

    ClientResult client_register_notifications(Client *, ClientNotificationHandler) { return CLIENT_NOT_IMPLEMENTED; }

    void client_unregister_notifications(Client *) { }

    void client_disconnect(Client *) { }
}
