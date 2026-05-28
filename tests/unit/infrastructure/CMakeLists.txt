# ---------------------------------------------------------------------------
# tests/unit/infrastructure/CMakeLists.txt
#
# Unit tests for the infrastructure layer. Each subsystem gets its own subdirectory mirroring
# src/infrastructure/<subsystem>/.
# ---------------------------------------------------------------------------

add_subdirectory(system)
add_subdirectory(media)
add_subdirectory(rss)
add_subdirectory(telegram)
