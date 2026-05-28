# ---------------------------------------------------------------------------
# tests/unit/infrastructure/telegram/CMakeLists.txt
#
# Pure unit tests for the telegram infrastructure adapter. Anything that
# touches the live TDLib client lives in an integration test (and requires a
# real bot account), so this directory is intentionally minimal.
# ---------------------------------------------------------------------------

file(GLOB TELEGRAM_TEST_SOURCES CONFIGURE_DEPENDS *_test.cpp)
foreach(test_file ${TELEGRAM_TEST_SOURCES})
    get_filename_component(test_name "${test_file}" NAME_WE)
    add_executable(telegram_${test_name} ${test_file})
    target_link_libraries(
        telegram_${test_name}
        PRIVATE
            cmlb_telegram
            cmlb_test_support
            Catch2::Catch2WithMain
    )
    catch_discover_tests(telegram_${test_name} DISCOVERY_MODE PRE_TEST)
endforeach()
