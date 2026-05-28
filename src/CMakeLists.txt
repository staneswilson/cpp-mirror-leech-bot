# ---------------------------------------------------------------------------
# src/CMakeLists.txt
#
# Aggregates every architectural layer and produces the final `cmlb` executable. Layers must be
# added bottom-up so that link dependencies are visible when each subdirectory's target is created.
# ---------------------------------------------------------------------------

add_subdirectory(core)
add_subdirectory(domain)
add_subdirectory(application)
add_subdirectory(infrastructure)
add_subdirectory(presentation)

# ---- Final executable ----------------------------------------------------
add_executable(cmlb main.cpp)

target_link_libraries(
    cmlb PRIVATE cmlb_core cmlb_domain cmlb_application cmlb_infrastructure cmlb_presentation
                 cmlb_warnings
)

set_target_properties(
    cmlb PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME cmlb
)
