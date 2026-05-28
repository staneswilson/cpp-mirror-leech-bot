# ---------------------------------------------------------------------------
# src/infrastructure/persistence/CMakeLists.txt
#
# SQLite-backed persistence layer: connection pool, schema migrator, and repository implementations
# for tasks, user settings, bot settings, and RSS feeds. Public headers live under
# include/cmlb/infrastructure/persistence/ and are exposed via cmlb_core's include-directory chain.
# ---------------------------------------------------------------------------

find_package(unofficial-sqlite-modern-cpp CONFIG QUIET)

add_library(
    cmlb_persistence STATIC
    sqlite_connection_pool.cpp schema_migrator.cpp sqlite_task_repository.cpp
    sqlite_user_settings_repository.cpp sqlite_bot_settings_repository.cpp
    sqlite_rss_feed_repository.cpp
)

add_library(cmlb::persistence ALIAS cmlb_persistence)

target_include_directories(
    cmlb_persistence PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                            $<INSTALL_INTERFACE:include> PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    cmlb_persistence
    PUBLIC cmlb_core
           cmlb_domain
           SQLite::SQLite3
           Boost::system
           Boost::headers
           nlohmann_json::nlohmann_json
           fmt::fmt
    PRIVATE cmlb_warnings
)

# sqlite-modern-cpp ships as a header-only library. The vcpkg port exports the target
# `unofficial::sqlite-modern-cpp`; on systems where the find package call above succeeds we link it
# for its include directory. When the port is unavailable (e.g. system-installed under /usr/include)
# the header still resolves via the default include search path.
if(TARGET unofficial::sqlite-modern-cpp)
    target_link_libraries(cmlb_persistence PUBLIC unofficial::sqlite-modern-cpp)
endif()

target_compile_features(cmlb_persistence PUBLIC cxx_std_23)

set_target_properties(
    cmlb_persistence PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                                OUTPUT_NAME cmlb_persistence
)
