# ---------------------------------------------------------------------------
# src/infrastructure/http/CMakeLists.txt
#
# Beast-based HTTP/HTTPS client. Header-only Boost.Beast plus OpenSSL for the TLS half. Builds a
# focused static library so the rest of the bot can depend on it without dragging in Boost.Process
# or other heavy deps.
# ---------------------------------------------------------------------------

add_library(cmlb_http STATIC beast_http_client.cpp)

add_library(cmlb::http ALIAS cmlb_http)

target_include_directories(
    cmlb_http PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_http PUBLIC cmlb_core Boost::system Boost::headers OpenSSL::SSL OpenSSL::Crypto
    PRIVATE cmlb_warnings
)

set_target_properties(
    cmlb_http PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME
                                                                                     cmlb_http
)

# Boost.Beast + Asio coroutines explode the COFF section count on MSVC well past
# the 65,535 default ceiling. `/bigobj` lifts it to ~4 billion. GCC/Clang have
# no equivalent need.
if(MSVC)
    target_compile_options(cmlb_http PRIVATE /bigobj)
endif()
