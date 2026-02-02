cmake_minimum_required(VERSION 3.28)

project(TelegramMirrorBot
    VERSION 0.2.0
    DESCRIPTION "High-performance C++23 Telegram Mirror/Leech Bot"
    LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# Compiler Conformance & Flags
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX /permissive- /Zc:__cplusplus /utf-8)
else()
    add_compile_options(
        -Wall 
        -Wextra 
        -Werror 
        -pedantic 
        -Wconversion 
        -Wshadow 
        -Wnon-virtual-dtor
    )
endif()

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json 3.11.2 REQUIRED)
find_package(td REQUIRED)
find_package(OpenSSL REQUIRED)

# -----------------------------------------------------------------------------
# Source Files
# -----------------------------------------------------------------------------
set(CORE_SOURCES
    src/main.cpp
    src/core/bot_engine.cpp
)

set(DOWNLOADER_SOURCES
    src/downloaders/aria2_client.cpp
)

set(UPLOADER_SOURCES
    src/uploaders/telegram_uploader.cpp
    src/uploaders/rclone_uploader.cpp
)

set(DB_SOURCES
    src/db/database.cpp
)

set(COMMAND_SOURCES
    src/commands/command_router.cpp
)

set(UTILS_SOURCES
    src/utils/status_manager.cpp
    src/utils/archive_handler.cpp
    src/utils/media_processor.cpp
    src/utils/rss_monitor.cpp
)

# -----------------------------------------------------------------------------
# Project Target
# -----------------------------------------------------------------------------
add_executable(cmlb_bot
    ${CORE_SOURCES}
    ${DOWNLOADER_SOURCES}
    ${UPLOADER_SOURCES}
    ${DB_SOURCES}
    ${COMMAND_SOURCES}
    ${UTILS_SOURCES}
)

target_include_directories(cmlb_bot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cmlb_bot PRIVATE
    Boost::boost
    Boost::system
    nlohmann_json::nlohmann_json
    td::td_client 
    td::td_api
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Platform-specific linking
if(WIN32)
    target_link_libraries(cmlb_bot PRIVATE ws2_32 crypt32)
endif()

# -----------------------------------------------------------------------------
# Feature Testing
# -----------------------------------------------------------------------------
include(CheckIncludeFileCXX)
check_include_file_cxx("print" HAVE_PRINT_HEADER)
check_include_file_cxx("expected" HAVE_EXPECTED_HEADER)

if(NOT HAVE_PRINT_HEADER OR NOT HAVE_EXPECTED_HEADER)
    message(FATAL_ERROR "This project requires a C++23 standard library with <print> and <expected>.")
endif()

# -----------------------------------------------------------------------------
# Output Directories
# -----------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Create data directories
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/data)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/downloads)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/logs)

# -----------------------------------------------------------------------------
# Installation
# -----------------------------------------------------------------------------
install(TARGETS cmlb_bot RUNTIME DESTINATION bin)
install(FILES config.example.json DESTINATION etc/cmlb)
install(DIRECTORY DESTINATION var/cmlb/data)
install(DIRECTORY DESTINATION var/cmlb/downloads)
install(DIRECTORY DESTINATION var/log/cmlb)
