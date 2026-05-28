# ---------------------------------------------------------------------------
# cmake/sanitizers.cmake
#
# Wires AddressSanitizer / UndefinedBehaviorSanitizer / ThreadSanitizer
# into the build via the cmlb_warnings INTERFACE target. Only one
# sanitizer may be enabled at a time — the toolchains do not compose
# them safely (notably ASan ⊕ TSan, which share TLS slots).
#
# This module assumes warnings.cmake has already been included so that
# `cmlb_warnings` exists.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

if(NOT TARGET cmlb_warnings)
    message(FATAL_ERROR
        "sanitizers.cmake requires warnings.cmake to be included first.")
endif()

# ---- Mutual exclusion ---------------------------------------------------
set(_cmlb_active_sanitizers "")
foreach(_san IN ITEMS ASAN UBSAN TSAN)
    if(CMLB_ENABLE_${_san})
        list(APPEND _cmlb_active_sanitizers ${_san})
    endif()
endforeach()

list(LENGTH _cmlb_active_sanitizers _cmlb_n_sanitizers)
if(_cmlb_n_sanitizers GREATER 1)
    string(REPLACE ";" ", " _csv "${_cmlb_active_sanitizers}")
    message(FATAL_ERROR
        "Sanitizers are mutually exclusive; got: ${_csv}. "
        "Enable at most one of CMLB_ENABLE_ASAN / _UBSAN / _TSAN.")
endif()

if(_cmlb_n_sanitizers EQUAL 0)
    return()
endif()

# ---- MSVC bail-out -------------------------------------------------------
if(MSVC)
    message(WARNING
        "Sanitizers are not enabled on MSVC in v1; build the request "
        "(${_cmlb_active_sanitizers}) under Clang/GCC instead. "
        "Continuing with sanitizer flags disabled.")
    return()
endif()

# ---- Flag selection ------------------------------------------------------
set(_cmlb_san_flags -fno-omit-frame-pointer -g)
set(_cmlb_san_link_flags "")

if(CMLB_ENABLE_ASAN)
    list(APPEND _cmlb_san_flags
        -fsanitize=address
        -fsanitize-address-use-after-scope
    )
    list(APPEND _cmlb_san_link_flags -fsanitize=address)
endif()

if(CMLB_ENABLE_UBSAN)
    list(APPEND _cmlb_san_flags
        -fsanitize=undefined
        -fno-sanitize-recover=undefined
    )
    list(APPEND _cmlb_san_link_flags -fsanitize=undefined)
endif()

if(CMLB_ENABLE_TSAN)
    list(APPEND _cmlb_san_flags -fsanitize=thread)
    list(APPEND _cmlb_san_link_flags -fsanitize=thread)
endif()

target_compile_options(
    cmlb_warnings
    INTERFACE
        $<BUILD_INTERFACE:${_cmlb_san_flags}>
)

target_link_options(
    cmlb_warnings
    INTERFACE
        $<BUILD_INTERFACE:${_cmlb_san_link_flags}>
)

message(STATUS "Sanitizers active: ${_cmlb_active_sanitizers}")
