# ---------------------------------------------------------------------------
# src/core/CMakeLists.txt
#
# Architectural foundation: error handling, structured logging, configuration, the executor
# abstraction, cancellation tokens and the canonical formatting facade.
#
# Source files in this list are produced by a follow-up agent. Until they exist the build will fail
# at link time — that is intentional and tracked separately. Do not stub them out here.
# ---------------------------------------------------------------------------

add_library(
    cmlb_core STATIC error.cpp logger.cpp configuration.cpp executor.cpp cancellation.cpp
                     formatting.cpp
)

add_library(cmlb::core ALIAS cmlb_core)

target_include_directories(
    cmlb_core PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                     $<BUILD_INTERFACE:${CMLB_GENERATED_INCLUDE_DIR}> $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_core PUBLIC fmt::fmt spdlog::spdlog nlohmann_json::nlohmann_json Boost::system
                     Boost::headers Threads::Threads PRIVATE Boost::json cmlb_warnings
)

set_target_properties(
    cmlb_core PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME
                                                                                     cmlb_core
)
