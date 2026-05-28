# ---------------------------------------------------------------------------
# cmake/version.cmake
#
# Splits CMLB_VERSION into MAJOR/MINOR/PATCH and renders the canonical header `cmlb/version.hpp`
# from its `.in` template. The generated header lives in the build tree; consumers pick it up via
# the include directory exported by cmlb_core.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

if(NOT DEFINED CMLB_VERSION)
    message(FATAL_ERROR "version.cmake included before CMLB_VERSION was read.")
endif()

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _cmlb_v "${CMLB_VERSION}")
if(NOT _cmlb_v)
    message(FATAL_ERROR "CMLB_VERSION='${CMLB_VERSION}' is not a SemVer triple.")
endif()

set(CMLB_VERSION_MAJOR ${CMAKE_MATCH_1})
set(CMLB_VERSION_MINOR ${CMAKE_MATCH_2})
set(CMLB_VERSION_PATCH ${CMAKE_MATCH_3})

set(CMLB_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${CMLB_GENERATED_INCLUDE_DIR}/cmlb")

configure_file(
    "${CMAKE_SOURCE_DIR}/include/cmlb/version.hpp.in"
    "${CMLB_GENERATED_INCLUDE_DIR}/cmlb/version.hpp" @ONLY
)
