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
#include <stdio.h>

static unsigned int callback_count = 0;

static void on_connection_event(Client *client, ClientConnectionEvent event)
{
    (void)client;
    (void)event;
    ++callback_count;
}

static void on_response(Client *client, uint8_t *data, size_t size)
{
    (void)client;
    (void)size;
    ++callback_count;
    client_free_response(data);
}

static void on_string_response(Client *client, char *data)
{
    (void)client;
    ++callback_count;
    client_free_response(data);
}

int main(void)
{
    Client *client = NULL;
    if (client_connect(&client, on_connection_event) != CLIENT_NOT_IMPLEMENTED || client != NULL)
    {
        return 1;
    }

    uint8_t request[] = { 0x08, 0x01 };
    uint8_t *response = request;
    size_t response_size = sizeof(request);
    if (client_send_request(client, request, sizeof(request), &response, &response_size) != CLIENT_NOT_IMPLEMENTED
        || response != NULL || response_size != 0)
    {
        return 1;
    }

    char string_request[] = "{}";
    char *string_response = string_request;
    if (client_send_request_string(client, string_request, &string_response) != CLIENT_NOT_IMPLEMENTED
        || string_response != NULL)
    {
        return 1;
    }

    if (client_send_request_async(client, request, sizeof(request), on_response) != CLIENT_NOT_IMPLEMENTED
        || client_send_request_string_async(client, string_request, on_string_response) != CLIENT_NOT_IMPLEMENTED
        || client_register_notifications(client, on_response) != CLIENT_NOT_IMPLEMENTED)
    {
        return 1;
    }

    if (client_connect(NULL, on_connection_event) != CLIENT_INVALID_ARGUMENT
        || client_send_request_string(client, string_request, NULL) != CLIENT_INVALID_ARGUMENT)
    {
        return 1;
    }
    response_size = sizeof(request);
    if (client_send_request(client, request, sizeof(request), NULL, &response_size) != CLIENT_INVALID_ARGUMENT
        || response_size != 0)
    {
        return 1;
    }
    response = request;
    if (client_send_request(client, request, sizeof(request), &response, NULL) != CLIENT_INVALID_ARGUMENT
        || response != NULL)
    {
        return 1;
    }

    client_free_response(NULL);
    client_unregister_notifications(client);
    client_disconnect(client);
    if (callback_count != 0)
    {
        return 1;
    }
    puts("Client API placeholders verified.");
    return 0;
}
