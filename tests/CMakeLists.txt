# GoogleTest is always available so the suite can never be silently skipped:
# use an installed copy when there is one (works offline), otherwise fetch it.
# PIPESHELLX_SYSTEM_GTEST=ON requires the installed copy and never fetches.
if(PIPESHELLX_SYSTEM_GTEST)
    find_package(GTest REQUIRED)
else()
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            # v1.17.0, pinned to the commit rather than a movable tag. Keep this
            # in sync with THIRD_PARTY_NOTICES.md when upgrading GoogleTest.
            GIT_TAG        "${PIPESHELLX_GOOGLETEST_FALLBACK_COMMIT}"
        )
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

set(PIPESHELLX_TLS_TEST_SOURCES "")
if(PIPESHELLX_NATIVE_TRANSPORT AND OpenSSL_FOUND)
    set(PIPESHELLX_TLS_TEST_SOURCES unit/os/test_tls.cpp unit/transport/test_tls_stream.cpp
        unit/transport/test_native_transport.cpp unit/transport/test_node_policy.cpp
        unit/ca/test_certificate_authority.cpp
        unit/cli/test_ca_command.cpp
    unit/cli/test_diff_command.cpp
    unit/cli/test_pipe_command.cpp unit/cli/test_node_command.cpp)
endif()

add_executable(pipeshellx_tests
    unit/os/test_child_exit.cpp
    unit/os/test_handle.cpp
    unit/os/test_pipe.cpp
    unit/os/test_poller.cpp
    unit/os/test_process.cpp
    unit/os/test_result.cpp
    unit/os/test_signal_source.cpp
    unit/os/test_socket.cpp
    ${PIPESHELLX_TLS_TEST_SOURCES}
    unit/os/test_system.cpp
    unit/stream/test_bounded_buffer.cpp
    unit/stream/test_credit_window.cpp
    unit/stream/test_stream.cpp
    unit/sink/test_sinks.cpp
    unit/inventory/test_inventory.cpp
    unit/sink/test_consensus.cpp
    unit/pipeline/test_planner.cpp
    unit/pipeline/test_parser.cpp
    unit/pipeline/test_pipeline_yaml.cpp
    unit/pipeline/test_local_runner.cpp
    unit/pipeline/test_dag_runner.cpp
    unit/policy/test_policy.cpp
    unit/cli/test_run_command.cpp
    unit/cli/test_ping_command.cpp
    unit/cli/test_snapshot_command.cpp
    unit/stream/test_line_framer.cpp
    unit/stream/test_spool_buffer.cpp
    unit/runtime/test_reactor.cpp
    unit/runtime/test_backoff.cpp
    unit/runtime/test_lamport_clock.cpp
    unit/runtime/test_cluster_snapshot.cpp
    unit/json/test_json_reader.cpp
    unit/sync/test_ricart_agrawala.cpp
    unit/election/test_bully.cpp
    unit/sim/test_simulation.cpp
    unit/cli/test_sim_command.cpp
    unit/audit/test_audit_log.cpp
    unit/audit/test_audit_chain.cpp
    unit/transport/test_frame_codec.cpp
    unit/transport/test_wire.cpp
    unit/transport/test_open_request.cpp
    unit/transport/test_control_payloads.cpp
    unit/transport/test_session.cpp
    unit/transport/test_session_faults.cpp
    unit/transport/test_scale.cpp
    unit/transport/test_node_fencing.cpp
    test_cli_options.cpp
    test_client_config.cpp
    test_command_executor.cpp
    test_logger.cpp
    test_process_manager.cpp
    test_process_manager_golden.cpp
    test_main.cpp
    test_ssh_auth.cpp
    test_ssh_transport.cpp
)

target_include_directories(pipeshellx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pipeshellx_tests PRIVATE
    pipeshellx_lib
    pipeshellx_warnings
    GTest::gtest
)

include(GoogleTest)
gtest_discover_tests(pipeshellx_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 120
)

add_test(NAME dependency_notice_metadata
    COMMAND ${CMAKE_COMMAND}
        -DPIPESHELLX_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DPIPESHELLX_GOOGLETEST_FALLBACK_VERSION=${PIPESHELLX_GOOGLETEST_FALLBACK_VERSION}
        -DPIPESHELLX_GOOGLETEST_FALLBACK_COMMIT=${PIPESHELLX_GOOGLETEST_FALLBACK_COMMIT}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyDependencyNotices.cmake)
set_tests_properties(dependency_notice_metadata PROPERTIES LABELS packaging TIMEOUT 10)

# The portable poll backend is the behavior oracle: these suites must pass on
# it as well as on the platform-preferred backend.
add_test(NAME golden_on_poll_backend
    COMMAND pipeshellx_tests --gtest_filter=Golden*:ProcessManagerTest.*:CommandExecutorTest.*
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(golden_on_poll_backend PROPERTIES ENVIRONMENT "PIPESHELLX_POLLER=poll" TIMEOUT 300)

# Installation is a supported API: verify the lowercase executable, relocatable
# CMake export, and a fresh downstream find_package/link/run cycle.
set(_pipeshellx_smoke_bindir "${CMAKE_INSTALL_BINDIR}")
set(_pipeshellx_smoke_includedir "${CMAKE_INSTALL_INCLUDEDIR}")
set(_pipeshellx_smoke_libdir "${CMAKE_INSTALL_LIBDIR}")
set(_pipeshellx_smoke_datadir "${CMAKE_INSTALL_DATADIR}")
if(_pipeshellx_smoke_bindir STREQUAL "")
    set(_pipeshellx_smoke_bindir bin)
endif()
if(_pipeshellx_smoke_includedir STREQUAL "")
    set(_pipeshellx_smoke_includedir include)
endif()
if(_pipeshellx_smoke_libdir STREQUAL "")
    set(_pipeshellx_smoke_libdir lib)
endif()
if(_pipeshellx_smoke_datadir STREQUAL "")
    set(_pipeshellx_smoke_datadir share)
endif()
if(NOT PIPESHELLX_SANITIZE)
    add_test(NAME packaging_install_consumer_smoke
        COMMAND ${CMAKE_COMMAND}
            -DPIPESHELLX_BUILD_DIR=${PROJECT_BINARY_DIR}
            -DPIPESHELLX_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DPIPESHELLX_INSTALL_BINDIR=${_pipeshellx_smoke_bindir}
            -DPIPESHELLX_INSTALL_INCLUDEDIR=${_pipeshellx_smoke_includedir}
            -DPIPESHELLX_INSTALL_LIBDIR=${_pipeshellx_smoke_libdir}
            -DPIPESHELLX_INSTALL_DATADIR=${_pipeshellx_smoke_datadir}
            -DPIPESHELLX_GENERATOR=${CMAKE_GENERATOR}
            -DPIPESHELLX_CONFIG=$<CONFIG>
            -P ${PROJECT_SOURCE_DIR}/cmake/RunInstallSmoke.cmake)
    set_tests_properties(packaging_install_consumer_smoke PROPERTIES LABELS packaging RUN_SERIAL TRUE TIMEOUT 180)
endif()

# PIPESHELLX_NATIVE_TRANSPORT=ON is a hard contract, not an opportunistic
# feature toggle: configuration must fail clearly when OpenSSL 3 is absent.
add_test(NAME native_transport_requires_openssl3
    COMMAND ${CMAKE_COMMAND}
        -DPIPESHELLX_BUILD_DIR=${PROJECT_BINARY_DIR}
        -DPIPESHELLX_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DPIPESHELLX_GENERATOR=${CMAKE_GENERATOR}
        -P ${PROJECT_SOURCE_DIR}/cmake/RequireOpenSSL3Smoke.cmake)
set_tests_properties(native_transport_requires_openssl3 PROPERTIES LABELS packaging RUN_SERIAL TRUE TIMEOUT 120)
