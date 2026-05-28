# ---------------------------------------------------------------------------
# src/infrastructure/system/CMakeLists.txt
#
# System-level infrastructure: subprocess runner, resource metrics, and signal-handler bridge.
# Boost.Process v2 needs the `Boost::process` import target (linked PUBLIC because headers leak out
# via the implementation TU).
# ---------------------------------------------------------------------------

add_library(cmlb_system STATIC subprocess.cpp system_metrics.cpp signal_handler.cpp)

add_library(cmlb::system ALIAS cmlb_system)

target_include_directories(
    cmlb_system PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_system PUBLIC cmlb_core Boost::system Boost::headers Boost::process PRIVATE cmlb_warnings
)

if(WIN32)
    # Psapi: required for some process/memory query APIs we may pull in via GetProcessMemoryInfo in
    # the future. Linked unconditionally for Windows so test binaries don't need to repeat the
    # dependency.
    target_link_libraries(cmlb_system PRIVATE Psapi)
endif()

set_target_properties(
    cmlb_system PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME
                                                                                       cmlb_system
)
