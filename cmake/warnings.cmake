# ---------------------------------------------------------------------------
# cmake/warnings.cmake
#
# Defines `cmlb_warnings`, an INTERFACE target carrying the project's
# warning policy. Every CMLB target links against it (PRIVATE so the
# flags do not leak to consumers).
#
# Warnings are errors on every compiler — no asymmetry between MSVC and
# GCC/Clang.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

include(CheckCXXCompilerFlag)

add_library(cmlb_warnings INTERFACE)

# ---- GCC / Clang common warnings ----------------------------------------
set(_cmlb_gcc_clang_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wconversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wcast-align
    -Wunused
    -Wmisleading-indentation
)

# ---- GCC-only warnings (Clang silently accepts -W but warns) ------------
set(_cmlb_gcc_only_warnings
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast
)

# ---- MSVC warnings ------------------------------------------------------
set(_cmlb_msvc_warnings
    /W4
    /WX
    /permissive-
    /Zc:__cplusplus
    /utf-8
    /w14242
    /w14254
    /w14263
    /w14265
    /w14287
    /we4289
    /w14296
    /w14311
    /w14545
    /w14546
    /w14547
    /w14549
    /w14555
    /w14619
    /w14640
    /w14826
    /w14905
    /w14906
    /w14928
    /w14062
)

target_compile_options(
    cmlb_warnings
    INTERFACE
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>,$<BUILD_INTERFACE:1>>:${_cmlb_gcc_clang_warnings}>
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,GNU>,$<BUILD_INTERFACE:1>>:${_cmlb_gcc_only_warnings}>
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,MSVC>,$<BUILD_INTERFACE:1>>:${_cmlb_msvc_warnings}>
)

# ---- Clang-only: try -Wlifetime if available ----------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    check_cxx_compiler_flag("-Wlifetime" CMLB_CLANG_HAS_WLIFETIME)
    if(CMLB_CLANG_HAS_WLIFETIME)
        target_compile_options(
            cmlb_warnings
            INTERFACE
                $<BUILD_INTERFACE:-Wlifetime>
        )
    endif()
endif()

# ---- GCC >= 14: demote -Wnull-dereference from error to warning ---------
# GCC 14's interprocedural analysis fires -Wnull-dereference on inlined
# Boost.Asio internals (gcc bug #89180). The warning is still useful for
# spotting genuine null derefs in our own code, but treating these system-
# header false positives as errors would block every build that touches
# Asio. Keep the warning, drop the -Werror just for this one diagnostic.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
    target_compile_options(
        cmlb_warnings
        INTERFACE
            $<BUILD_INTERFACE:-Wno-error=null-dereference>
    )
endif()

# ---- NDEBUG / debug-iterator hardening ----------------------------------
target_compile_definitions(
    cmlb_warnings
    INTERFACE
        $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
        $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:MSVC>>:_ITERATOR_DEBUG_LEVEL=2>
)
