# ---------------------------------------------------------------------------
# src/presentation/CMakeLists.txt
#
# Presentation layer — the user-facing surface that parses commands, routes
# them to application use cases, formats HTML replies, and manages live
# status messages.
# ---------------------------------------------------------------------------

add_library(
    cmlb_presentation
    STATIC
        command_parser.cpp
        command_dispatcher.cpp
        callback_dispatcher.cpp
        html_renderer.cpp
        progress_renderer.cpp
)

add_library(cmlb::presentation ALIAS cmlb_presentation)

target_include_directories(
    cmlb_presentation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_presentation
    PUBLIC
        # Each PUBLIC edge is justified by an `#include` in
        # include/cmlb/presentation/*.hpp. Promoting any of these to PRIVATE
        # would break consumers (main.cpp, tests, downstream binaries) that
        # include the renderer / dispatcher headers without re-resolving the
        # transitive infrastructure includes themselves.
        cmlb_core
        cmlb_domain
        cmlb_application
        cmlb_telegram
        cmlb_system
        cmlb_persistence
        cmlb_download
        # Boost stays PUBLIC because progress_renderer.hpp and the
        # dispatcher headers expose `boost::asio::awaitable<>` /
        # `boost::asio::strand<>` in their signatures.
        Boost::system
        Boost::headers
    PRIVATE
        # fmt is consumed only inside .cpp files (message body composition).
        fmt::fmt
        cmlb_warnings
)

set_target_properties(
    cmlb_presentation
    PROPERTIES
        CXX_STANDARD          23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
        OUTPUT_NAME           cmlb_presentation
)
