# ---------------------------------------------------------------------------
# tests/unit/domain/CMakeLists.txt
#
# Auto-discovers every *_test.cpp in this directory and produces one Catch2 test binary per file via
# cmlb_add_test(). file(GLOB) is fine here — these are tests, not production sources, and
# CONFIGURE_DEPENDS keeps Ninja accurate. Sibling agents may extend this file with explicitly listed
# special-case targets.
# ---------------------------------------------------------------------------

file(GLOB CMLB_DOMAIN_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

foreach(_src IN LISTS CMLB_DOMAIN_TESTS)
    cmlb_add_test("${_src}" LIBRARIES cmlb_domain LABELS domain)
endforeach()
