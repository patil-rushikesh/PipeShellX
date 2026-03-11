find_package(Threads REQUIRED)

# Main library target
add_library(remote_command_lib
    client_config.cpp
    client_manager.cpp
    ipc_engine.cpp
    process_manager.cpp
    ssh_auth.cpp
    command_executor.cpp
    session_manager.cpp
    logger.cpp
)

target_include_directories(remote_command_lib PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(remote_command_lib PUBLIC Threads::Threads)

# Main executable
add_executable(remote_command_app
    main.cpp
    terminal_client.cpp
)
set_target_properties(remote_command_app PROPERTIES OUTPUT_NAME PipeShellX)

target_link_libraries(remote_command_app PRIVATE remote_command_lib)

# Debug/Release flags
target_compile_options(remote_command_lib PRIVATE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)
target_compile_options(remote_command_app PRIVATE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)

# Install rules
install(TARGETS remote_command_app remote_command_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
