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


add_custom_target(check-l1
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/run-tests.py
        --build-dir ${PROJECT_BINARY_DIR}
        --label L1
    VERBATIM
)

if(TARGET ${NAMESPACE}CryptoTestBackend)
    add_executable(eyil-crypto-test-backend-l1 CryptoTestBackendTests.cpp)
    target_link_libraries(eyil-crypto-test-backend-l1 PRIVATE ${NAMESPACE}CryptoTestBackend)
    eyil_test_cases(eyil-crypto-test-backend-l1 L1 sessions fixed-replies round-trip signature regions files)
endif()

if(TARGET EyilGatewayRuntime AND TARGET eyilclient)
    add_executable(eyil-l1 RuntimeTests.cpp)
    target_link_libraries(eyil-l1 PRIVATE EyilGatewayRuntime EyilClient::EyilClient)
    eyil_test_cases(eyil-l1 L1 buffer-concurrency operations client-arguments log-burst)
endif()

if(TARGET eyilclient)
    add_executable(eyil-c-api-l1 ${PROJECT_SOURCE_DIR}/scripts/consumer/main.c)
    target_link_libraries(eyil-c-api-l1 PRIVATE EyilClient::EyilClient)
    add_test(NAME eyil-c-api-l1 COMMAND eyil-c-api-l1)
    set_tests_properties(eyil-c-api-l1 PROPERTIES LABELS L1 TIMEOUT 10)
endif()

if(TARGET EyilGatewayRuntime)
    add_executable(eyil-buffer-l1 SharedBufferTests.cpp)
    target_link_libraries(eyil-buffer-l1 PRIVATE EyilGatewayRuntime)
    eyil_test_faults(eyil-buffer-l1)
    eyil_test_cases(eyil-buffer-l1 L1 log-correlation sizes range-arithmetic uid-isolation
        allocation-memory release-failure lease-lifetime release-race open-denied
        exclusive-create-failed truncate-failed allocate-enospc chown-failed
        chmod-failed stat-failed allocate-eintr)
    add_executable(eyil-adapter-l1 AdapterTests.cpp)
    target_link_libraries(eyil-adapter-l1 PRIVATE EyilGatewayRuntime)
    eyil_test_faults(eyil-adapter-l1)
    eyil_test_cases(eyil-adapter-l1 L1 arguments enum-mappings optional-presence
        invalid-enums oneof-parsing missing-parameters service-errors invocation-errors
        bad-backend-outputs output-regions uncertain-access local-without-backend
        lookup-all-operations invocation-all-operations exception-all-operations buffer-rejections
        completed-error-release completed-exception-release)
endif()

if(TARGET EyilGatewayRuntime AND TARGET eyilclient)
    add_executable(eyil-lifecycle-l1 LifecycleTests.cpp)
    target_link_libraries(eyil-lifecycle-l1 PRIVATE ${NAMESPACE}EyilGateway ${NAMESPACE}EyilGatewayImplementation
        EyilGatewayRuntime EyilClient::EyilClient)
    set(lifecycle_cases invalid-config partial-startup destructor-idle
        destructor-inflight missing-backend default-backends empty-backends empty-callsign
        omitted-callsign backend-list wrapper-failed-config wrapper-missing-implementation volatile-directory
        runtime-deactivation)
    eyil_test_cases(eyil-lifecycle-l1 L1 ${lifecycle_cases})
    foreach(scenario IN LISTS lifecycle_cases)
        # Root() loads Thunder Plugins and its indirect SDK dependencies.
        set_tests_properties(eyil-lifecycle-l1.${scenario} PROPERTIES ENVIRONMENT
            "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:ZeroMQ::ZeroMQ>:$<TARGET_FILE_DIR:protobuf::libprotobuf>:$<TARGET_FILE_DIR:${NAMESPACE}Plugins::${NAMESPACE}Plugins>:$ENV{LD_LIBRARY_PATH}")
    endforeach()
endif()

if(TARGET EyilGatewayRuntime)
    add_executable(eyil-common-l1 CommonTests.cpp)
    target_link_libraries(eyil-common-l1 PRIVATE EyilGatewayRuntime)
    eyil_test_faults(eyil-common-l1)
    target_link_options(eyil-common-l1 PRIVATE "-Wl,--wrap=zmq_msg_gets")
    eyil_enable_coverage(eyil-common-l1)
    eyil_test_cases(eyil-common-l1 L1 descriptors wakeup sockets tokens log-suppression poll-deadlines log-context
        identity-decimal credentials monitor-framing)
endif()

if(TARGET eyilclient)
    # Compile the shipped client source into this test executable so GNU ld can
    # interpose libzmq/allocation failures at its external-call boundaries.
    add_executable(eyil-client-l1 ClientTests.cpp ${PROJECT_SOURCE_DIR}/src/clientlib/EyilClient.cpp)
    target_include_directories(eyil-client-l1 PRIVATE ${PROJECT_SOURCE_DIR}/apis)
    target_link_libraries(eyil-client-l1 PRIVATE EyilProtocol::EyilProtocol ZeroMQ::ZeroMQ Threads::Threads)
    eyil_test_faults(eyil-client-l1)
    foreach(symbol zmq_send zmq_msg_recv malloc zmq_socket zmq_socket_monitor zmq_connect zmq_poll)
        target_link_options(eyil-client-l1 PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
    eyil_enable_coverage(eyil-client-l1)
    eyil_test_cases(eyil-client-l1 L1 arguments raw-text wrong-service wrong-schema
        unset-status unknown-status zero-id malformed multipart async-retirement duplicate-unknown-id
        send-memory send-error send-eagain send-timeout send-eintr send-acceptance-race
        sync-response-memory async-response-memory sync-decode-memory async-decode-memory
        input-memory explicit-close transport-loss callback-rules callback-quiescence
        sync-timeout-late-reply auth-denied auth-temporary auth-internal socket-memory socket-error
        monitor-memory monitor-error connect-memory connect-error
        owner-memory close-before-send idle-poll async-no-deadline connect-allocation monitor-auth-300)
endif()
