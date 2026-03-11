enable_testing()

find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(WARNING "GTest not found; test target will not be built.")
    return()
endif()

add_executable(remote_command_tests
    test_ipc.cpp
    test_prcoess_manager.cpp
)

target_include_directories(remote_command_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(remote_command_tests PRIVATE
    remote_command_lib
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME RemoteCommandTests COMMAND remote_command_tests)
