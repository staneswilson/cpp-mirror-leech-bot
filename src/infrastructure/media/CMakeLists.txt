# ---------------------------------------------------------------------------
# src/infrastructure/media/CMakeLists.txt
#
# Media (ffmpeg) and archive (7-zip) processors. Both adapters shell out
# via the cmlb_system Subprocess gateway — no direct std::system/popen
# is permitted in this layer.
# ---------------------------------------------------------------------------

add_library(
    cmlb_media
    STATIC
        ffmpeg_media_processor.cpp
        seven_zip_archive_processor.cpp
)

add_library(cmlb::media ALIAS cmlb_media)

target_include_directories(
    cmlb_media
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(
    cmlb_media
    PUBLIC
        cmlb_core
        cmlb_domain
        cmlb_system
        Boost::headers
        nlohmann_json::nlohmann_json
    PRIVATE
        cmlb_warnings
)

set_target_properties(
    cmlb_media
    PROPERTIES
        CXX_STANDARD          23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
        OUTPUT_NAME           cmlb_media
)
