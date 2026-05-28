# ---------------------------------------------------------------------------
# cmake/coverage.cmake
#
# When CMLB_ENABLE_COVERAGE is ON, attaches the appropriate instrumentation flags to the
# cmlb_warnings INTERFACE target and defines a `cmlb_coverage` custom target that produces an HTML
# report under ${CMAKE_BINARY_DIR}/coverage_html.
#
# GCC  → gcov / lcov / genhtml Clang → llvm-profdata / llvm-cov show MSVC → unsupported (warns and
# returns).
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

if(NOT CMLB_ENABLE_COVERAGE)
    return()
endif()

if(NOT TARGET cmlb_warnings)
    message(FATAL_ERROR "coverage.cmake requires warnings.cmake to be included first.")
endif()

if(MSVC)
    message(WARNING "Code coverage is not supported on MSVC in v1. "
                    "CMLB_ENABLE_COVERAGE will be ignored."
    )
    return()
endif()

# ---- Flag selection ------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(cmlb_warnings INTERFACE $<BUILD_INTERFACE:--coverage -O0 -g>)
    target_link_options(cmlb_warnings INTERFACE $<BUILD_INTERFACE:--coverage>)
    set(_cmlb_cov_kind "gcov")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(
        cmlb_warnings INTERFACE $<BUILD_INTERFACE:-fprofile-instr-generate -fcoverage-mapping -O0
                                -g>
    )
    target_link_options(
        cmlb_warnings INTERFACE $<BUILD_INTERFACE:-fprofile-instr-generate -fcoverage-mapping>
    )
    set(_cmlb_cov_kind "llvm")
else()
    message(WARNING "Unknown compiler '${CMAKE_CXX_COMPILER_ID}' for coverage; "
                    "skipping report target."
    )
    return()
endif()

# ---- Report target -------------------------------------------------------
set(_cmlb_cov_dir "${CMAKE_BINARY_DIR}/coverage_html")
file(MAKE_DIRECTORY "${_cmlb_cov_dir}")

if(_cmlb_cov_kind STREQUAL "gcov")
    find_program(LCOV_EXECUTABLE lcov)
    find_program(GENHTML_EXECUTABLE genhtml)

    if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
        add_custom_target(
            cmlb_coverage
            COMMAND ${LCOV_EXECUTABLE} --directory ${CMAKE_BINARY_DIR} --capture --output-file
                    ${CMAKE_BINARY_DIR}/coverage.info --rc lcov_branch_coverage=1
            COMMAND ${LCOV_EXECUTABLE} --remove ${CMAKE_BINARY_DIR}/coverage.info "*/tests/*"
                    "*/_deps/*" "/usr/*" --output-file ${CMAKE_BINARY_DIR}/coverage.info
            COMMAND ${GENHTML_EXECUTABLE} ${CMAKE_BINARY_DIR}/coverage.info --output-directory
                    ${_cmlb_cov_dir}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating gcov/lcov coverage report → ${_cmlb_cov_dir}"
            VERBATIM
        )
    else()
        message(WARNING "lcov/genhtml not found; cmlb_coverage target will not be "
                        "created. Install lcov to enable HTML reports."
        )
    endif()

elseif(_cmlb_cov_kind STREQUAL "llvm")
    find_program(LLVM_PROFDATA_EXECUTABLE llvm-profdata)
    find_program(LLVM_COV_EXECUTABLE llvm-cov)

    if(LLVM_PROFDATA_EXECUTABLE AND LLVM_COV_EXECUTABLE)
        add_custom_target(
            cmlb_coverage
            COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/cmlb-%p.profraw
                    ctest --output-on-failure
            COMMAND ${LLVM_PROFDATA_EXECUTABLE} merge -sparse ${CMAKE_BINARY_DIR}/cmlb-*.profraw -o
                    ${CMAKE_BINARY_DIR}/cmlb.profdata
            COMMAND
                ${LLVM_COV_EXECUTABLE} show $<TARGET_FILE:cmlb>
                -instr-profile=${CMAKE_BINARY_DIR}/cmlb.profdata -format=html
                -output-dir=${_cmlb_cov_dir}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating llvm-cov coverage report → ${_cmlb_cov_dir}"
            VERBATIM
        )
    else()
        message(WARNING "llvm-profdata / llvm-cov not found; cmlb_coverage target "
                        "will not be created."
        )
    endif()
endif()
