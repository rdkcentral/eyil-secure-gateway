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


add_custom_target(check-l2
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/run-tests.py
        --build-dir ${PROJECT_BINARY_DIR}
        --label L2
    VERBATIM
)

if(TARGET EyilGatewayRuntime AND TARGET eyilclient)
    add_executable(eyil-l2 TransportTests.cpp)
    target_link_libraries(eyil-l2 PRIVATE EyilGatewayRuntime EyilClient::EyilClient)
    eyil_test_cases(eyil-l2 L2 basic out-of-order shared-memory missing-crypto
        worker-exhaustion late-async-close connection-loss patch patch-cleanup)
endif()

if(TARGET EyilGatewayRuntime)
    add_library(EyilCryptoMock MODULE ../support/MockCryptoPlugin.cpp ${PROJECT_SOURCE_DIR}/src/gateway/Module.cpp)
    target_link_libraries(EyilCryptoMock PRIVATE EyilGatewayRuntime)
    target_compile_definitions(EyilCryptoMock PRIVATE MODULE_NAME=Plugin_EyilCryptoMock BUILD_REFERENCE="test-only")
endif()
if(TARGET EyilCryptoMock AND TARGET eyil-client)
    find_program(EYIL_THUNDER_EXECUTABLE NAMES WPEFramework Thunder
        HINTS "${${NAMESPACE}Core_DIR}/../../../bin" "$ENV{EYIL_THUNDER_PREFIX}/bin")
    if(EYIL_THUNDER_EXECUTABLE)
        set(crypto_test_backend_arguments)
        if(TARGET ${NAMESPACE}CryptoTestBackend)
            list(APPEND crypto_test_backend_arguments
                --crypto-test-backend $<TARGET_FILE:${NAMESPACE}CryptoTestBackend>
                --crypto-test-backend-configuration ${PROJECT_BINARY_DIR}/src/tools/CryptoTestBackend/config/CryptoTestBackend.json)
        endif()
        add_test(NAME eyil-thunder-l2
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-thunder-runtime.py
                --thunder ${EYIL_THUNDER_EXECUTABLE}
                --wrapper $<TARGET_FILE:${NAMESPACE}EyilGateway>
                --implementation $<TARGET_FILE:${NAMESPACE}EyilGatewayImplementation>
                --mock $<TARGET_FILE:EyilCryptoMock>
                --configuration ${PROJECT_BINARY_DIR}/src/gateway/config/EyilGateway.json
                --client $<TARGET_FILE:eyil-client>
                --reports ${PROJECT_BINARY_DIR}/reports/thunder
                ${crypto_test_backend_arguments}
                --library-dir $<TARGET_FILE_DIR:${NAMESPACE}Core::${NAMESPACE}Core>
                --library-dir $<TARGET_FILE_DIR:ZeroMQ::ZeroMQ>
                --library-dir $<TARGET_FILE_DIR:protobuf::libprotobuf>)
        set_tests_properties(eyil-thunder-l2 PROPERTIES LABELS L2 TIMEOUT 120 RESOURCE_LOCK eyil-ipc)
    else()
        message(STATUS "Thunder executable unavailable; real-process L2 test not registered")
    endif()
endif()

if(TARGET eyil-l2)
    find_library(EYIL_STOCK_ZMQ_LIBRARY NAMES zmq libzmq.so.5
        PATHS /usr/lib /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} NO_DEFAULT_PATH)
    if(EYIL_STOCK_ZMQ_LIBRARY)
        get_filename_component(_stock_zmq_dir "${EYIL_STOCK_ZMQ_LIBRARY}" DIRECTORY)
        add_test(NAME eyil-unpatched-zmq COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=${_stock_zmq_dir}:$ENV{LD_LIBRARY_PATH}" $<TARGET_FILE:eyil-l2> unpatched)
        set_tests_properties(eyil-unpatched-zmq PROPERTIES LABELS L2 TIMEOUT 10)
    endif()
endif()

if(TARGET eyilclient)
    add_executable(eyil-client-transport-l2 ClientTransportTests.cpp)
    target_link_libraries(eyil-client-transport-l2 PRIVATE EyilClient::EyilClient EyilProtocol::EyilProtocol ZeroMQ::ZeroMQ Threads::Threads)
    add_test(NAME eyil-client-transport-l2 COMMAND eyil-client-transport-l2)
    set_tests_properties(eyil-client-transport-l2 PROPERTIES LABELS L2 TIMEOUT 45 RESOURCE_LOCK eyil-ipc)
endif()

if(TARGET EyilGatewayRuntime)
    add_executable(eyil-gateway-edge-l2 GatewayEdgeTests.cpp)
    target_link_libraries(eyil-gateway-edge-l2 PRIVATE EyilGatewayRuntime)
    eyil_test_faults(eyil-gateway-edge-l2)
    target_link_options(eyil-gateway-edge-l2 PRIVATE "-Wl,--wrap=zmq_send" "-Wl,--wrap=zmq_poll"
        "-Wl,--wrap=_ZN6google8protobuf11MessageLite14ParseFromArrayEPKvi"
        "-Wl,--wrap=_ZNK6google8protobuf11MessageLite17SerializeToStringEPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
    eyil_test_cases(eyil-gateway-edge-l2 L2 idle-poll worker-logs envelopes framing-drain duplicate-id
        disconnect-before-request route-reuse reply-eagain reply-unreachable reply-body-failure
        admission-body-failure response-admission-fallback response-admission-drop response-catch-drop
        response-worker-fallback response-worker-drop response-timeout-drop
        response-serialization-fallback response-serialization-drop
        drain-replies drain-waiting drain-waiting-timeout)
endif()
