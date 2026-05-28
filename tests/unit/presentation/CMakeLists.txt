# ---------------------------------------------------------------------------
# tests/unit/presentation/CMakeLists.txt
#
# Auto-discovers every *_test.cpp in this directory and produces one Catch2 test binary per file via
# `cmlb_add_test()`. Mirrors the pattern used by the domain test sub-tree.
# ---------------------------------------------------------------------------

file(GLOB CMLB_PRESENTATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

foreach(_src IN LISTS CMLB_PRESENTATION_TESTS)
    cmlb_add_test("${_src}" LIBRARIES cmlb_presentation LABELS presentation)
endforeach()
