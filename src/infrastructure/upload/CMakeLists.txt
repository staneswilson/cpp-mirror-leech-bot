# ---------------------------------------------------------------------------
# src/infrastructure/upload/CMakeLists.txt
#
# Upload adapters: Telegram (TDLib via Messenger), Google Drive (service account JWT + REST), rclone
# (subprocess driver). The interface itself (`uploader_interface.hpp`) is header-only.
# ---------------------------------------------------------------------------

add_library(cmlb_upload STATIC telegram_uploader.cpp google_drive_uploader.cpp rclone_uploader.cpp)

add_library(cmlb::upload ALIAS cmlb_upload)

target_include_directories(
    cmlb_upload PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_upload
    PUBLIC cmlb_core
           cmlb_domain
           cmlb_http
           cmlb_telegram
           cmlb_system
           Boost::headers
           Boost::system
           OpenSSL::SSL
           OpenSSL::Crypto
           nlohmann_json::nlohmann_json
           fmt::fmt
    PRIVATE cmlb_warnings
)

set_target_properties(
    cmlb_upload PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME
                                                                                       cmlb_upload
)
