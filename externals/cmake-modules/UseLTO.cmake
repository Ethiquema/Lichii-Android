# SPDX-FileCopyrightText: Copyright 2025 crueter
# SPDX-License-Identifier: LGPL-3.0-or-later

## UseLTO ##

# Enable Interprocedural Optimization (IPO).
# Self-explanatory.

# ThinLTO is enabled by default on Android: the NDK only ships clang + lld, so
# it is always available there and the CI/CD pipeline gets it without any flag.
# Desktop builds keep the conservative default (pass -DENABLE_LTO=ON to opt in).
# Escape hatch everywhere: -DENABLE_LTO=OFF.
if (ANDROID)
    set(_ENABLE_LTO_DEFAULT ON)
else()
    set(_ENABLE_LTO_DEFAULT OFF)
endif()
option(ENABLE_LTO "Enable Link-Time Optimization (LTO)" ${_ENABLE_LTO_DEFAULT})
unset(_ENABLE_LTO_DEFAULT)

if (ENABLE_LTO)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND ANDROID)
        # Android NDK: clang is the only compiler and lld the only linker, so
        # ThinLTO is always available and needs no IPO probe. Explicitly emit
        # -flto=thin:
        # CMAKE_INTERPROCEDURAL_OPTIMIZATION would emit full -flto here, which
        # links several times slower for an equivalent result.
        # Per-source opt-outs (e.g. -fno-lto on src/core kernel.cpp) still win,
        # file-level options come after directory-level ones.
        set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
        add_compile_options(
            $<$<COMPILE_LANG_AND_ID:C,Clang>:-flto=thin>
            $<$<COMPILE_LANG_AND_ID:CXX,Clang>:-flto=thin>)
        add_link_options(-flto=thin)
    else()
        include(CheckIPOSupported)
        check_ipo_supported(RESULT COMPILER_SUPPORTS_LTO)
        if(NOT COMPILER_SUPPORTS_LTO)
            message(FATAL_ERROR
            "Your compiler does not support interprocedural optimization"
            " (IPO). Disable ENABLE_LTO and try again.")
        endif()
        set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${COMPILER_SUPPORTS_LTO})
    endif()
endif()