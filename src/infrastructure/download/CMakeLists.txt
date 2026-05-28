# ---------------------------------------------------------------------------
# src/infrastructure/download/CMakeLists.txt
#
# Downloader adapters: shared interface + aria2 (WebSocket JSON-RPC) and qBittorrent (HTTP Web API
# v2) concrete clients.
# ---------------------------------------------------------------------------

add_library(
    cmlb_download STATIC downloader_interface.cpp aria2_downloader.cpp qbittorrent_downloader.cpp
)

add_library(cmlb::download ALIAS cmlb_download)

target_include_directories(
    cmlb_download PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                         $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_download
    PUBLIC cmlb_core
           cmlb_domain
           cmlb_http
           Boost::system
           Boost::headers
           Boost::json
           OpenSSL::SSL
           OpenSSL::Crypto
           nlohmann_json::nlohmann_json
    PRIVATE cmlb_warnings
)

set_target_properties(
    cmlb_download PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                             OUTPUT_NAME cmlb_download
)
