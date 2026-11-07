# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2023 yuzu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(yuzu-android SHARED
    android_env_file.cpp
    emu_window/emu_window.cpp
    emu_window/emu_window.h
    native.cpp
    native.h
    native_config.cpp
    native_freedreno.cpp
    android_settings.cpp
    game_metadata.cpp
    native_log.cpp
    android_config.cpp
    android_config.h
    native_input.cpp
    native_post_processing.cpp
    nx2_alloc_profile.cpp
)

# Release APKs ship the tested defaults only and don't read nx2_env.txt.
if (EDEN_NX2_NO_ENV_FILE)
    target_compile_definitions(yuzu-android PRIVATE EDEN_NX2_NO_ENV_FILE)
endif()

set_property(TARGET yuzu-android PROPERTY IMPORTED_LOCATION ${FFmpeg_LIBRARY_DIR})

target_link_libraries(yuzu-android PRIVATE audio_core common core input_common frontend_common video_core)
target_link_libraries(yuzu-android PRIVATE android camera2ndk EGL glad jnigraphics log GLESv2)
if (ARCHITECTURE_arm64)
    target_link_libraries(yuzu-android PRIVATE adrenotools)
endif()

target_link_libraries(yuzu-android PRIVATE ${FFmpeg_LIBRARIES} OpenSSL::SSL cpp-jwt::cpp-jwt)
if (ENABLE_UPDATE_CHECKER)
    target_compile_definitions(yuzu-android PUBLIC ENABLE_UPDATE_CHECKER)
endif()

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} yuzu-android)

target_link_options(yuzu-android PRIVATE "-Wl,-Bsymbolic")
