# ---------------------------------------------------------------------------
# src/infrastructure/CMakeLists.txt
#
# Infrastructure adapters: shared utilities (HTTP, subprocess, system), persistence (SQLite),
# Telegram gateway, downloaders, uploaders, media/archive processors, RSS poller.
#
# Each sub-directory builds a focused static library. The parent cmlb_infrastructure is an INTERFACE
# umbrella so the application layer can link a single target if it wishes to depend on everything.
# ---------------------------------------------------------------------------

add_subdirectory(http)
add_subdirectory(system)
add_subdirectory(persistence)
add_subdirectory(telegram)
add_subdirectory(download)
add_subdirectory(upload)
add_subdirectory(media)
add_subdirectory(rss)

add_library(cmlb_infrastructure INTERFACE)
add_library(cmlb::infrastructure ALIAS cmlb_infrastructure)

target_include_directories(
    cmlb_infrastructure INTERFACE $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                                  $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_infrastructure
    INTERFACE cmlb_core
              cmlb_domain
              cmlb_http
              cmlb_system
              cmlb_persistence
              cmlb_telegram
              cmlb_download
              cmlb_upload
              cmlb_media
              cmlb_rss
)
