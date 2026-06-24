# ---------------------------------------------------------------------------
# CMLB — C++23 Telegram Mirror/Leech Bot
#
# Top-level build configuration. All knobs are documented inline. The build graph is decomposed by
# architectural layer:
#
# cmlb_core           (static)     — error, logger, config, runtime cmlb_domain         (static) —
# domain types (authority, task, ...) cmlb_application    (interface)  — use cases (phase 6+)
# cmlb_infrastructure (interface)  — adapters (phase 3+) cmlb_presentation   (interface)  —
# CLI/Telegram surfaces (phase 5+) cmlb                (exe)        — main entry point
#
# Helper modules live in cmake/ and are included once each, in dependency order, below.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

# ---- Read version --------------------------------------------------------
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" CMLB_VERSION LIMIT_COUNT 1)
string(STRIP "${CMLB_VERSION}" CMLB_VERSION)

if(NOT CMLB_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION file must contain a single SemVer 'MAJOR.MINOR.PATCH' line, "
                        "got '${CMLB_VERSION}'."
    )
endif()

project(
    cmlb
    VERSION ${CMLB_VERSION}
    LANGUAGES CXX
    DESCRIPTION "CMLB — a modern C++23 Telegram Mirror/Leech bot"
    HOMEPAGE_URL "https://github.com/staneswilson/cpp-mirror-leech-bot"
)

# ---- In-source guard -----------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not permitted. Configure from a build/ "
                        "directory: cmake --preset debug"
    )
endif()

# ---- Language standard ---------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMLB does not use C++20 modules (we use plain headers throughout). Modules scanning under GCC 15.x
# + Boost.Asio has known interaction bugs that silently fail the dyndep step. Disable globally —
# re-enable per-target if a future module is introduced.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(UNIX)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ---- Output layout -------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---- Module path & helper includes --------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---- Build options -------------------------------------------------------
option(CMLB_BUILD_TESTS "Build unit/property/integration tests" ON)
option(CMLB_BUILD_BENCHMARKS "Build microbenchmarks" OFF)
option(CMLB_BUILD_DOCS "Build Doxygen documentation" OFF)
option(CMLB_ENABLE_COVERAGE "Enable code-coverage instrumentation" OFF)
option(CMLB_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CMLB_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CMLB_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

include(warnings)
include(sanitizers)
include(coverage)
include(version)
include(cmlb_helpers)

# ---- Feature probes ------------------------------------------------------
# Bail out early on toolchains that lack the C++23 headers we depend on.
include(CheckIncludeFileCXX)
include(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX23_STANDARD_COMPILE_OPTION}")
foreach(_hdr expected print format source_location)
    string(MAKE_C_IDENTIFIER "CMLB_HAVE_${_hdr}" _have_var)
    string(TOUPPER "${_have_var}" _have_var)
    check_include_file_cxx("${_hdr}" ${_have_var})
    if(NOT ${_have_var})
        message(
            FATAL_ERROR
                "Required C++23 header <${_hdr}> not found. CMLB needs a "
                "fully C++23-conformant standard library "
                "(GCC >= 14, Clang >= 18, MSVC >= 19.40)."
        )
    endif()
endforeach()
unset(CMAKE_REQUIRED_FLAGS)

# ---- Third-party dependencies -------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json 3.11.3 CONFIG REQUIRED)
find_package(spdlog 1.13 CONFIG REQUIRED)
find_package(fmt 10 CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS system headers json process)
# Boost.Asio and Boost.Beast are header-only in modern Boost; pulled in via Boost::headers. We still
# declare them via find_package above for clarity.

# TDLib is the only dependency that may legitimately be absent on developer machines, AND its API
# surface changes across versions (the gateway's td_api::sendMessage signature is tied to the TDLib
# release used at compile time). When the real gateway isn't viable the project falls back to a
# build-only stub for `telegram_gateway.cpp` — every other layer compiles, unit tests run, and the
# bot binary links but errors at startup with "telegram gateway stub: rebuild with the 'telegram'
# vcpkg feature".
#
# Behaviour: * `-DCMLB_WITH_TELEGRAM=ON`: require TDLib; build real gateway. *
# `-DCMLB_WITH_TELEGRAM=OFF`: skip TDLib entirely; build stub. * default (`AUTO`): probe for TDLib
# *and* confirm version compatibility. If not found or unusable, fall back to stub.
set(CMLB_WITH_TELEGRAM "AUTO"
    CACHE STRING "Telegram backend mode: ON|OFF|AUTO. AUTO probes for a usable TDLib and \
falls back to a build-only stub if absent or version-incompatible."
)
set_property(CACHE CMLB_WITH_TELEGRAM PROPERTY STRINGS AUTO ON OFF)

if(CMLB_WITH_TELEGRAM STREQUAL "OFF")
    set(CMLB_WITH_TELEGRAM OFF)
    message(STATUS "TDLib: disabled by request — using build-only stub gateway.")
elseif(CMLB_WITH_TELEGRAM STREQUAL "ON")
    find_package(Td CONFIG REQUIRED)
    set(CMLB_WITH_TELEGRAM ON)
    message(STATUS "TDLib: enabled by request (real gateway)")
else()
    # AUTO mode: prefer real gateway only if the user has explicitly built it against the matching
    # TDLib. The stub is the safe default since TDLib's td_api signatures differ across versions and
    # MinGW builds.
    find_package(Td CONFIG QUIET)
    if(Td_FOUND AND DEFINED ENV{CMLB_PREFER_REAL_TDLIB})
        set(CMLB_WITH_TELEGRAM ON)
        message(STATUS "TDLib: AUTO — enabled (CMLB_PREFER_REAL_TDLIB=1 env set)")
    else()
        set(CMLB_WITH_TELEGRAM OFF)
        message(STATUS "TDLib: AUTO — using build-only stub gateway "
                       "(set env CMLB_PREFER_REAL_TDLIB=1 or "
                       "-DCMLB_WITH_TELEGRAM=ON to opt into the real backend)."
        )
    endif()
endif()

if(CMLB_WITH_TELEGRAM)
    unset(CMLB_TDLIB_HAS_RICH_MESSAGE CACHE)
    unset(CMLB_TDLIB_HAS_RICH_MESSAGE)
    set(CMAKE_REQUIRED_LIBRARIES Td::tdapi)
    check_cxx_source_compiles(
        [[
#include <string>
#include <utility>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

int main() {
    namespace td_api = td::td_api;
    auto source = td_api::make_object<td_api::richMessageSourceHtml>(std::string{"<b>ok</b>"});
    auto message = td_api::make_object<td_api::inputRichMessage>(
        td_api::move_object_as<td_api::RichMessageSource>(std::move(source)), false, true);
    auto content = td_api::make_object<td_api::inputMessageRichMessage>(std::move(message), false);
    (void)content;
    return 0;
}
]]
        CMLB_TDLIB_HAS_RICH_MESSAGE
    )
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(CMLB_TDLIB_HAS_RICH_MESSAGE)
        message(STATUS "TDLib: rich message API available - enabling inputMessageRichMessage.")
    else()
        message(STATUS "TDLib: rich message API unavailable - using formattedText fallback.")
    endif()
else()
    set(CMLB_TDLIB_HAS_RICH_MESSAGE OFF)
endif()

# ---- Build the project tree ---------------------------------------------
add_subdirectory(src)

if(CMLB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CMLB_BUILD_BENCHMARKS)
    # benchmarks/ is added when real benchmark sources land. The directory is not scaffolded yet —
    # re-enable this block after dropping in a nanobench-based suite under benchmarks/.
    message(WARNING "CMLB_BUILD_BENCHMARKS=ON but benchmarks/ is not yet populated. "
                    "Skipping benchmark target generation."
    )
endif()

if(CMLB_BUILD_DOCS)
    add_subdirectory(docs)
endif()

# ---- Install rules -------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS cmlb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES "${CMAKE_SOURCE_DIR}/config.example.json"
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/cmlb
)

install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---- Packaging (CPack) ---------------------------------------------------
include(packaging)

# ---- Configuration summary ----------------------------------------------
message(STATUS "")
message(STATUS "CMLB ${CMLB_VERSION} configured")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests             : ${CMLB_BUILD_TESTS}")
message(STATUS "  Benchmarks        : ${CMLB_BUILD_BENCHMARKS}")
message(STATUS "  Docs              : ${CMLB_BUILD_DOCS}")
message(STATUS "  Coverage          : ${CMLB_ENABLE_COVERAGE}")
message(STATUS "  ASan/UBSan/TSan   : ${CMLB_ENABLE_ASAN}/${CMLB_ENABLE_UBSAN}/${CMLB_ENABLE_TSAN}")
message(STATUS "")
