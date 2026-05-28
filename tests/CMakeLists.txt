# ---------------------------------------------------------------------------
# tests/CMakeLists.txt
#
# Test root. Pulls in Catch2 v3 and CTest, defines the shared
# `cmlb_test_support` header-only library used by every test binary,
# then descends into the per-layer test trees.
# ---------------------------------------------------------------------------

find_package(Catch2 3 REQUIRED)

include(CTest)
include(Catch)

# ---- Shared test scaffolding --------------------------------------------
add_library(cmlb_test_support INTERFACE)
add_library(cmlb::test_support ALIAS cmlb_test_support)

target_include_directories(
    cmlb_test_support
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

target_link_libraries(
    cmlb_test_support
    INTERFACE
        cmlb_core
        cmlb_domain
        cmlb_persistence
        cmlb_download
        cmlb_upload
        cmlb_telegram
        Catch2::Catch2WithMain
)

target_compile_features(cmlb_test_support INTERFACE cxx_std_23)

# ---- Sub-trees -----------------------------------------------------------
add_subdirectory(unit)
add_subdirectory(integration)
