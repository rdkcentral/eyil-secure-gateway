###
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
###

find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/reports")

# Register separate scenarios so failures name the violated contract.
function(eyil_test_cases target label)
    foreach(scenario IN LISTS ARGN)
        add_test(NAME ${target}.${scenario} COMMAND ${target} ${scenario})
        set_tests_properties(${target}.${scenario} PROPERTIES
            LABELS ${label} TIMEOUT 60 RESOURCE_LOCK eyil-ipc)
    endforeach()
endfunction()

# Test executables interpose failures at system boundaries; shipped libraries
# contain neither injection switches nor additional test/control interfaces.
function(eyil_test_faults target)
    target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/Tests/support/Faults.cpp)
    foreach(symbol open openat ftruncate fallocate fchown fchmod fstat unlinkat
            _ZNSt6chrono3_V212steady_clock3nowEv)
        target_link_options(${target} PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
endfunction()

if(RDK_SERVICES_L1_TEST)
    add_subdirectory(L1Tests)
endif()

if(RDK_SERVICE_L2_TEST)
    add_subdirectory(L2Tests)
endif()

add_custom_target(coverage
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/coverage.py
        --build-dir ${PROJECT_BINARY_DIR}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Run registered tests and report coverage availability"
    VERBATIM
)
