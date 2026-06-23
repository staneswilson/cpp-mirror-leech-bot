# ---------------------------------------------------------------------------
# cmake/packaging.cmake
#
# CPack configuration. Generators are selected per-platform:
#
# - Linux:   TGZ, ZIP
# - macOS:   TGZ, ZIP
# - Windows: TGZ, ZIP, WIX (if WiX toolset is found)
#
# The DESCRIPTION file is rendered from the README if it exists.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

set(CPACK_PACKAGE_NAME "cmlb")
set(CPACK_PACKAGE_VENDOR "CMLB Contributors")
set(CPACK_PACKAGE_VERSION "${CMLB_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${CMLB_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${CMLB_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${CMLB_VERSION_PATCH}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/staneswilson/cpp-mirror-leech-bot")
set(CPACK_PACKAGE_CONTACT "https://github.com/staneswilson/cpp-mirror-leech-bot/issues")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Modern C++23 Telegram Mirror/Leech bot.")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "cmlb")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

if(EXISTS "${CMAKE_SOURCE_DIR}/README.md")
    set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")
endif()

# ---- Source-archive hygiene ---------------------------------------------
set(CPACK_SOURCE_IGNORE_FILES
    "/\\\\.git/"
    "/\\\\.github/"
    "/\\\\.vs/"
    "/\\\\.vscode/"
    "/\\\\.cache/"
    "/build/"
    "/builds/"
    "/out/"
    "/cmake-build-.*/"
    "/vcpkg_installed/"
    "/downloads/"
    "/tdlib/"
    "/third_party/"
    "\\\\.legacy\\\\."
    "\\\\.user$"
    "\\\\.swp$"
    "~$"
)

# ---- Generator selection -------------------------------------------------
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CPACK_GENERATOR TGZ ZIP)

elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND CPACK_GENERATOR TGZ ZIP)

elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND CPACK_GENERATOR TGZ ZIP)

    find_program(CMLB_WIX_CANDLE candle)
    if(CMLB_WIX_CANDLE)
        list(APPEND CPACK_GENERATOR WIX)
        # A stable upgrade GUID — bump on incompatible install layouts only.
        set(CPACK_WIX_UPGRADE_GUID "5E8C7AD3-7C29-4D87-9B91-3C2C9C5C5C7B")
        set(CPACK_WIX_PRODUCT_GUID "*")
        set(CPACK_WIX_LICENSE_RTF "")
    else()
        message(STATUS "WiX toolset not found — skipping MSI generator.")
    endif()
endif()

include(CPack)
