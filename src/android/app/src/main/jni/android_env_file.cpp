// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Android has no way to pass environment variables to an app, so the EDEN_* diagnostic switches
// are read from <external files dir>/env.txt, one KEY=VALUE per line ('#' starts a comment).
// Several switches are read by static initializers, so this runs before them.
//
// Field debugging note (AYN Thor round 3, docs/AYN_THOR_GPU_FIX.md): an edit to env.txt can
// silently not apply and look like "the switch does not work". Three causes, all handled here:
//  1. The folder depends on the applicationId of the *installed* build. CI releases are
//     dev.lichii.android, but local relWithDebInfo/debug builds and nightly builds append a
//     suffix, so a file placed next to the documented package is never read. The scanned path
//     is therefore always logged, found or not.
//  2. Editors saving UTF-8 with a BOM make the first key unmatchable (EF BB BF prefix), and a
//     copied nx2_switches.txt template can carry a second assignment further down. The BOM is
//     stripped and duplicate assignments are reported; the last one wins, as with setenv.
//  3. On Android 13+ file managers cannot write into Android/data — push the file with adb.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <android/log.h>

namespace {

std::string PackageName() {
    char name[256]{};
    if (FILE* file = std::fopen("/proc/self/cmdline", "rb")) {
        const size_t n = std::fread(name, 1, sizeof(name) - 1, file);
        name[n] = '\0';
        std::fclose(file);
    }
    // A process of the app may carry a ":service" suffix.
    if (char* colon = std::strchr(name, ':')) {
        *colon = '\0';
    }
    return name;
}

std::string Trim(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool StartsWithUtf8Bom(const std::string& s) {
    return s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
           static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF;
}

__attribute__((constructor(101))) void LoadEnvFile() {
    const std::string package = PackageName();
    if (package.empty()) {
        return;
    }
    const std::string path = "/storage/emulated/0/Android/data/" + package + "/files/env.txt";
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        // Always report the scanned path: builds other than the CI release carry an
        // applicationId suffix, so the folder from the docs may not be this build's folder.
        __android_log_print(ANDROID_LOG_WARN, "YuzuNative",
                            "env.txt: no switches file at %s (create it there — adb push <file> %s "
                            "— one KEY=VALUE per line, then restart the app)",
                            path.c_str(), path.c_str());
        return;
    }
    std::map<std::string, std::string> seen;
    char buffer[1024];
    bool first_line = true;
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
        std::string line = Trim(buffer);
        if (first_line) {
            first_line = false;
            if (StartsWithUtf8Bom(line)) {
                line = Trim(line.substr(3));
            }
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        setenv(key.c_str(), value.c_str(), 1);
        const auto [it, inserted] = seen.try_emplace(key, value);
        if (!inserted && it->second != value) {
            __android_log_print(ANDROID_LOG_WARN, "YuzuNative",
                                "env.txt: %s assigned again (%s -> %s), the last one wins",
                                key.c_str(), it->second.c_str(), value.c_str());
            it->second = value;
        }
        __android_log_print(ANDROID_LOG_WARN, "YuzuNative", "env.txt: %s=%s", key.c_str(),
                            value.c_str());
    }
    std::fclose(file);
    __android_log_print(ANDROID_LOG_WARN, "YuzuNative", "env.txt: %zu switch(es) loaded from %s",
                        seen.size(), path.c_str());
}

} // namespace
