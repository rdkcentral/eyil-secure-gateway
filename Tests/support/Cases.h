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

#pragma once

#include "Test.h"
#include <initializer_list>
#include <utility>

namespace EyilTest
{
    using Case = std::pair<const char *, std::function<void()>>;

    inline int RunCases(int argc, char **argv, std::initializer_list<Case> cases)
    {
        unsigned executed = 0, failures = 0;
        for (const auto &test : cases)
        {
            if (argc > 1 && test.first != std::string(argv[1]))
                continue;
            ++executed;
            try
            {
                test.second();
                std::cout << "PASS " << test.first << '\n';
            }
            catch (const std::exception &e)
            {
                ++failures;
                std::cerr << "FAIL " << test.first << ": " << e.what() << '\n';
            }
        }
        if (!executed)
            std::cerr << "Unknown test case\n";
        return executed && !failures ? 0 : 1;
    }
}
