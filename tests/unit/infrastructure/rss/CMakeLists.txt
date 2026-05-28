# ---------------------------------------------------------------------------
# tests/unit/infrastructure/rss/CMakeLists.txt
#
# RSS document parser unit tests. The poller has external dependencies
# (HTTP, DB) so it is exercised at the integration layer.
# ---------------------------------------------------------------------------

file(GLOB INFRA_RSS_TEST_SOURCES CONFIGURE_DEPENDS *_test.cpp)
foreach(test_file ${INFRA_RSS_TEST_SOURCES})
    get_filename_component(test_name "${test_file}" NAME_WE)
    add_executable(infrastructure_rss_${test_name} ${test_file})
    target_link_libraries(
        infrastructure_rss_${test_name}
        PRIVATE
            cmlb_rss
            cmlb_core
            cmlb_test_support
            Catch2::Catch2WithMain
    )
    catch_discover_tests(infrastructure_rss_${test_name} DISCOVERY_MODE PRE_TEST)
endforeach()
