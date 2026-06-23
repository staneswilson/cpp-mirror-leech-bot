# ---------------------------------------------------------------------------
# src/application/CMakeLists.txt
#
# Application layer — orchestration of infrastructure adapters by use cases. Each translation unit
# implements exactly one use case (verb-noun naming).
# ---------------------------------------------------------------------------

add_library(
    cmlb_application STATIC
    mirror_url.cpp
    leech_url.cpp
    clone_drive_resource.cpp
    count_drive_resource.cpp
    delete_drive_resource.cpp
    cancel_task.cpp
    pause_task.cpp
    resume_task.cpp
    show_status.cpp
    update_user_settings.cpp
    update_bot_settings.cpp
    rss_subscription.cpp
)

add_library(cmlb::application ALIAS cmlb_application)

target_include_directories(
    cmlb_application PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                            $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_application
    PUBLIC # PUBLIC because application use-case headers (mirror_url.hpp, leech_url.hpp, ...) take
           # refs to infrastructure interfaces in their constructors. Anything depending on
           # cmlb_application must see those headers transitively.
           cmlb_core
           cmlb_domain
           cmlb_persistence
           cmlb_telegram
           cmlb_download
           cmlb_upload
           cmlb_system
           Boost::headers
           Boost::system
    PRIVATE # fmt is consumed only inside .cpp files (status-message formatting), so it stays an
            # implementation detail.
            fmt::fmt cmlb_warnings
)

set_target_properties(
    cmlb_application PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                                OUTPUT_NAME cmlb_application
)
