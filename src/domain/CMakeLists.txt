# ---------------------------------------------------------------------------
# src/domain/CMakeLists.txt
#
# Domain types. Most are header-only value objects (identifiers, byte_size, upload_destination,
# ...); only the ones with non-trivial behaviour (authority rules, task state machine) get a .cpp.
# ---------------------------------------------------------------------------

add_library(cmlb_domain STATIC authority.cpp task.cpp)

add_library(cmlb::domain ALIAS cmlb_domain)

target_include_directories(
    cmlb_domain PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
)

target_link_libraries(cmlb_domain PUBLIC cmlb_core PRIVATE cmlb_warnings)

set_target_properties(
    cmlb_domain PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME
                                                                                       cmlb_domain
)
