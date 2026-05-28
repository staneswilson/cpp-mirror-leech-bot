file(GLOB INFRA_SYSTEM_TEST_SOURCES CONFIGURE_DEPENDS *_test.cpp)
foreach(test_file ${INFRA_SYSTEM_TEST_SOURCES})
    get_filename_component(test_name "${test_file}" NAME_WE)
    add_executable(infrastructure_system_${test_name} ${test_file})
    target_link_libraries(
        infrastructure_system_${test_name}
        PRIVATE
            cmlb_system
            cmlb_core
            cmlb_test_support
            Catch2::Catch2WithMain
    )
    catch_discover_tests(infrastructure_system_${test_name} DISCOVERY_MODE PRE_TEST)
endforeach()
