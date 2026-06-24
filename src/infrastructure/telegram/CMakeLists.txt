# ---------------------------------------------------------------------------
# src/infrastructure/telegram/CMakeLists.txt
#
# `cmlb_telegram` — the only static library in CMLB that links against TDLib. All other modules talk
# to Telegram through the public headers in `include/cmlb/infrastructure/telegram/*.hpp` (which are
# PIMPL-isolated and therefore do not transitively include any `<td/...>` header).
# ---------------------------------------------------------------------------

# Pick the real gateway implementation or the build-only stub depending on whether TDLib was located
# by the top-level `find_package(Td CONFIG)`. The stub is for unit-test builds on machines without
# TDLib; the bot binary still links but exits at startup with a clear error.
if(CMLB_WITH_TELEGRAM)
    set(_cmlb_telegram_gateway_src telegram_gateway.cpp)
else()
    set(_cmlb_telegram_gateway_src telegram_gateway_stub.cpp)
endif()

add_library(
    cmlb_telegram STATIC ${_cmlb_telegram_gateway_src} messenger.cpp update_router.cpp
                         authentication_flow.cpp
)

add_library(cmlb::telegram ALIAS cmlb_telegram)

target_include_directories(
    cmlb_telegram PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                         $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_telegram PUBLIC cmlb_core cmlb_domain Boost::headers Boost::system PRIVATE cmlb_warnings
                                                                                    fmt::fmt
)

if(CMLB_WITH_TELEGRAM)
    target_link_libraries(cmlb_telegram PUBLIC Td::tdclient Td::tdapi)
    if(CMLB_TDLIB_HAS_RICH_MESSAGE)
        target_compile_definitions(cmlb_telegram PRIVATE CMLB_TDLIB_HAS_RICH_MESSAGE=1)
    endif()
endif()

set_target_properties(
    cmlb_telegram PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                             OUTPUT_NAME cmlb_telegram
)
