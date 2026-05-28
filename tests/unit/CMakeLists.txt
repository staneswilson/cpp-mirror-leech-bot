# ---------------------------------------------------------------------------
# tests/unit/CMakeLists.txt
#
# Unit-test tree. Each architectural layer gets its own subdirectory so test additions stay local to
# the code they exercise.
# ---------------------------------------------------------------------------

add_subdirectory(core)
add_subdirectory(domain)
add_subdirectory(infrastructure)
add_subdirectory(application)
add_subdirectory(presentation)
