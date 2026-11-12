// Copyright 2026 RDK Management
// SPDX-License-Identifier: Apache-2.0
// Development diagnostic client. No cryptographic implementation or secret logging.
#include "client/TestClient.h"

#include <cstring>
#include <iostream>

int main(int argc, char **argv)
{
    using namespace EyilDev;
    if (argc != 2
        || (std::strcmp(argv[1], "--expect-no-crypto") && std::strcmp(argv[1], "--exercise")
            && std::strcmp(argv[1], "--crypto-test-backend")))
    {
        std::cout
            << "Usage: eyil-client --expect-no-crypto | --exercise | --crypto-test-backend\n"
               "--expect-no-crypto: all 15 backend operations must return INTERNAL; local buffer APIs still work.\n"
               "--exercise: transport, strings, mixed sync/async and Gateway-owned buffer cases.\n"
               "--crypto-test-backend: all 17 operations, deterministic outputs, async shared memory and edge cases.\n"
               "Exit: 0 all selected cases passed; 1 test/setup/fatal failure; 2 invalid arguments.\n";
        return argc == 2 && !std::strcmp(argv[1], "--help") ? 0 : 2;
    }
    const auto mode = !std::strcmp(argv[1], "--crypto-test-backend") ? Mode::TestBackend
        : !std::strcmp(argv[1], "--expect-no-crypto")                ? Mode::MissingBackend
                                                                     : Mode::Exercise;
    Runner runner(mode);
    runner.Log("START mode=" + std::string(argv[1]));
    runner.Log("NOTE buffer quota tests require the host UID's four slots to be available to this diagnostic run.");
    runner.Log("NOTE expected negative cases may produce Gateway warnings/errors; no request payloads are logged.");
    try
    {
        // These scopes guarantee callback quiescence before Runner destruction,
        // including early termination after a transport error or missing reply.
        Connection a(runner, "A");
        Connection b(runner, "B");
        ApiCases(runner, a);
        KeyCases(runner, a);
        ConcurrentCases(runner, a, b);
        BulkCases(runner, a, b);
        AllocationCases(runner, a, b);
        OwnershipCases(runner, a, b);
        RegionCases(runner, a);
        InvalidRequestCases(runner, a);
        LifecycleCases(runner, a, b);
        runner.Run("final-disconnect", "both connections closed; no connection callback",
            [&]
            {
                a.Close();
                b.Close();
            });
    }
    catch (const std::exception &error)
    {
        runner.Abort(error.what());
    }
    return runner.Summary();
}
