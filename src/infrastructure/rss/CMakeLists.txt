# ---------------------------------------------------------------------------
# src/infrastructure/rss/CMakeLists.txt
#
# RSS document parser + long-lived feed poller. The parser is regex-based
# (full XML parsing is overkill for the feeds we see in the wild); the
# poller drives HTTP fetches through cmlb_http and persists state via
# cmlb_persistence.
# ---------------------------------------------------------------------------

add_library(
    cmlb_rss
    STATIC
        rss_document_parser.cpp
        rss_feed_poller.cpp
)

add_library(cmlb::rss ALIAS cmlb_rss)

target_include_directories(
    cmlb_rss
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_rss
    PUBLIC
        cmlb_core
        cmlb_domain
        cmlb_http
        cmlb_persistence
        Boost::headers
    PRIVATE
        cmlb_warnings
)

set_target_properties(
    cmlb_rss
    PROPERTIES
        CXX_STANDARD          23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
        OUTPUT_NAME           cmlb_rss
)
