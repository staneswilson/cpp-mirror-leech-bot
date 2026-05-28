# ---------------------------------------------------------------------------
# tests/integration/CMakeLists.txt
#
# Integration tests that exercise multiple components together — they may
# spin up local servers, touch the filesystem, or invoke real subprocesses.
# Each test binary is GLOB-discovered from *_test.cpp files in this dir.
# ---------------------------------------------------------------------------

file(GLOB INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS *_test.cpp)

# Tests skipped on builds that use the stub http/aria2/subprocess gateways.
# The skip-list is empty when the real implementations are restored.
set(_cmlb_skip_integration_tests
    beast_http_client_test    # spins up a real Beast server; conflicts with stub cmlb_http
)

foreach(test_file ${INTEGRATION_TEST_SOURCES})
    get_filename_component(test_name "${test_file}" NAME_WE)
    if(test_name IN_LIST _cmlb_skip_integration_tests)
        message(STATUS "Skipping integration test '${test_name}' (incompatible with current build stubs)")
        continue()
    endif()
    add_executable(integration_${test_name} ${test_file})
    target_link_libraries(
        integration_${test_name}
        PRIVATE
            cmlb_core
            cmlb_domain
            cmlb_http
            cmlb_system
            cmlb_media
            cmlb_download
            cmlb_persistence
            cmlb_test_support
            Boost::system
            Boost::headers
            OpenSSL::SSL
            OpenSSL::Crypto
            Catch2::Catch2WithMain
    )
    set_target_properties(
        integration_${test_name}
        PROPERTIES
            CXX_STANDARD          23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS        OFF
    )
    catch_discover_tests(integration_${test_name} DISCOVERY_MODE PRE_TEST)
endforeach()
