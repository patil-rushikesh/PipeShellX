find_package(Threads REQUIRED)

# L0 — OS primitive backends (the only place platform headers may appear)
if(WIN32)
    message(FATAL_ERROR
        "PipeShellX does not yet implement a Win32 controller or native node (see ROADMAP.md)")
else()
    set(PIPESHELLX_STREAM_SOURCES
        stream/bounded_buffer.cpp
        stream/credit_window.cpp
        stream/line_framer.cpp
        stream/stream.cpp
    )
    set(PIPESHELLX_INVENTORY_SOURCES
        inventory/inventory.cpp
    )
    set(PIPESHELLX_PIPELINE_SOURCES
        pipeline/planner.cpp
        pipeline/parser.cpp
        pipeline/pipeline_yaml.cpp
        pipeline/local_runner.cpp
        pipeline/dag_runner.cpp
    )
    set(PIPESHELLX_POLICY_SOURCES
        policy/policy.cpp
    )
    set(PIPESHELLX_SINK_SOURCES
        sink/group_sink.cpp
        sink/json_sink.cpp
        sink/consensus.cpp
        sink/consensus_sink.cpp
        sink/ordered_sink.cpp
        sink/stream_sink.cpp
        audit/audit_log.cpp
        audit/sha256.cpp
    )
    set(PIPESHELLX_OS_SOURCES
        os/posix/child_exit.cpp
        os/posix/console.cpp
        os/posix/error.cpp
        os/posix/handle.cpp
        os/posix/io.cpp
        os/posix/paths.cpp
        os/posix/pipe.cpp
        os/posix/socket.cpp
        os/posix/poller.cpp
        os/posix/poller_poll.cpp
        os/posix/process.cpp
        os/posix/signal.cpp
        os/posix/signal_source.cpp
        os/posix/system.cpp
    )
    if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
        list(APPEND PIPESHELLX_OS_SOURCES
            os/posix/child_exit_kqueue.cpp
            os/posix/poller_kqueue.cpp
            os/posix/signal_source_kqueue.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND PIPESHELLX_OS_SOURCES
            os/posix/child_exit_pidfd.cpp
            os/posix/poller_epoll.cpp
            os/posix/signal_source_signalfd.cpp)
    endif()
endif()

# Core library
set(PIPESHELLX_TLS_SOURCES "")
if(PIPESHELLX_NATIVE_TRANSPORT)
    set(PIPESHELLX_TLS_SOURCES os/tls.cpp transport/tls_stream.cpp transport/native_transport.cpp
        ca/certificate_authority.cpp
        transport/node_server.cpp
        cli/ca_command.cpp
        cli/node_command.cpp
        transport/native_controller.cpp
        transport/canary_controller.cpp
        pipeline/distributed_runner.cpp
        pipeline/segmented_pipeline.cpp
        pipeline/fanin_pipeline.cpp)
endif()

add_library(pipeshellx_lib
    ${PIPESHELLX_OS_SOURCES}
    ${PIPESHELLX_STREAM_SOURCES}
    ${PIPESHELLX_SINK_SOURCES}
    ${PIPESHELLX_INVENTORY_SOURCES}
    ${PIPESHELLX_PIPELINE_SOURCES}
    ${PIPESHELLX_POLICY_SOURCES}
    runtime/reactor.cpp
    runtime/cluster_snapshot.cpp
    json/reader.cpp
    sync/ricart_agrawala.cpp
    election/bully.cpp
    sim/simulation.cpp
    transport/frame_codec.cpp
    transport/open_request.cpp
    transport/control_payloads.cpp
    transport/session.cpp
    transport/node_stage_runner.cpp
    ${PIPESHELLX_TLS_SOURCES}
    cli/hosts_command.cpp
    cli/ping_command.cpp
    cli/selection.cpp
    cli/diff_command.cpp
    cli/pipe_command.cpp
    cli/run_command.cpp
    cli/snapshot_command.cpp
    cli/sim_command.cpp
    cli/audit_command.cpp
    cli_options.cpp
    client_config.cpp
    client_manager.cpp
    command_executor.cpp
    logger.cpp
    process_manager.cpp
    ssh_auth.cpp
    ssh_transport.cpp
)
add_library(pipeshellx::lib ALIAS pipeshellx_lib)

target_compile_features(pipeshellx_lib PUBLIC cxx_std_20)
target_include_directories(pipeshellx_lib PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(pipeshellx_lib
    PUBLIC Threads::Threads
)
get_target_property(_pipeshellx_warning_options pipeshellx_warnings INTERFACE_COMPILE_OPTIONS)
target_compile_options(pipeshellx_lib PRIVATE ${_pipeshellx_warning_options})
target_compile_definitions(pipeshellx_lib PRIVATE
    PIPESHELLX_VERSION="${PROJECT_VERSION}"
)
if(PIPESHELLX_NATIVE_TRANSPORT)
    target_link_libraries(pipeshellx_lib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(pipeshellx_lib PUBLIC PIPESHELLX_HAVE_TLS=1)
endif()
set_target_properties(pipeshellx_lib PROPERTIES
    OUTPUT_NAME pipeshellx
    EXPORT_NAME lib
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# Interactive executable
add_executable(pipeshellx
    main.cpp
    terminal_client.cpp
)
target_link_libraries(pipeshellx PRIVATE pipeshellx_lib pipeshellx_warnings)

# Install rules
install(TARGETS pipeshellx
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Runtime)
install(TARGETS pipeshellx_lib
    EXPORT pipeshellxTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime NAMELINK_COMPONENT Development
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development)
install(EXPORT pipeshellxTargets
    FILE pipeshellxTargets.cmake
    NAMESPACE pipeshellx::
    DESTINATION ${PIPESHELLX_INSTALL_CMAKEDIR}
    COMPONENT Development
)
