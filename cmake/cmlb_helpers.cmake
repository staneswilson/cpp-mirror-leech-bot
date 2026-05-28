# ---------------------------------------------------------------------------
# cmake/cmlb_helpers.cmake
#
# Reusable helpers shared across the build. Currently exposes:
#
#   cmlb_add_test(<source_file>
#       [LIBRARIES <lib> ...]
#       [LABELS    <label> ...])
#
# which compiles a single Catch2 test from <source_file>, links it
# against cmlb_test_support plus any caller-supplied LIBRARIES, and
# registers each TEST_CASE with CTest via catch_discover_tests.
#
# The target name is derived from the file stem prefixed with `tests_`,
# so `error_test.cpp` becomes the target `tests_error_test`.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

function(cmlb_add_test source_file)
    set(_options "")
    set(_one_value "")
    set(_multi_value LIBRARIES LABELS)
    cmake_parse_arguments(
        ARG "${_options}" "${_one_value}" "${_multi_value}" ${ARGN}
    )

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "cmlb_add_test: unrecognised argument(s): "
            "${ARG_UNPARSED_ARGUMENTS}")
    endif()

    get_filename_component(_stem "${source_file}" NAME_WE)
    set(_target "tests_${_stem}")

    add_executable(${_target} "${source_file}")

    target_link_libraries(
        ${_target}
        PRIVATE
            cmlb_test_support
            cmlb_warnings
            ${ARG_LIBRARIES}
    )

    set_target_properties(
        ${_target}
        PROPERTIES
            CXX_STANDARD          23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS        OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
    )

    # DISCOVERY_MODE PRE_TEST: discover Catch2 test cases at `ctest` time
    # (when CTest can set PATH to include the vcpkg-installed DLL dirs),
    # not at build time. Without this, every test binary that links against
    # any DLL dependency (fmt, spdlog, boost, sqlite, openssl, ...) fails
    # the discovery step on Windows because the bare exe cannot resolve
    # its imports until those DLLs are on PATH.
    catch_discover_tests(
        ${_target}
        TEST_PREFIX "${_stem}::"
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "unit;${ARG_LABELS}"
    )
endfunction()
