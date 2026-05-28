# ---------------------------------------------------------------------------
# tests/unit/application/CMakeLists.txt
#
# Unit tests for the application (use cases) layer. Each *_test.cpp file is built into its own
# catch_discover_tests executable.
# ---------------------------------------------------------------------------

file(GLOB APPLICATION_TEST_SOURCES CONFIGURE_DEPENDS *_test.cpp)
foreach(test_file ${APPLICATION_TEST_SOURCES})
    get_filename_component(test_name "${test_file}" NAME_WE)
    add_executable(application_${test_name} ${test_file})
    target_link_libraries(
        application_${test_name} PRIVATE cmlb_application cmlb_presentation cmlb_test_support
                                         cmlb_warnings Catch2::Catch2WithMain
    )
    # RUN_SERIAL TRUE: these tests stand up their own asio io_context + run coroutines that drive
    # the executor pool. Under `ctest -j N` with N>1 the OS scheduler can starve the
    # coroutine-completion path of one test exe and the test SEGFAULTs at suspend-point teardown.
    # All work is in-memory stubs, so serial execution costs ~1 s total — well worth it.
    catch_discover_tests(
        application_${test_name} DISCOVERY_MODE PRE_TEST PROPERTIES RUN_SERIAL TRUE
    )
endforeach()
