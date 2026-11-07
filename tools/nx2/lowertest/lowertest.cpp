// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs Shader::Ampere::LowerToMaxwell over native programs dumped with EDEN_NX2_SHADER_DUMP
// (raw 16-byte instructions, no header) and reports which fail and where.
//
// Build: clang++ -std=c++20 -O1 -I src tools/nx2/lowertest/lowertest.cpp -o build/lowertest
// Usage: build/lowertest [-q] [-a] <dir or .bin>...
//   -q: failures and summary only; -a: list each program's attribute accesses (ALD/AST/IPA)

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace fs = std::filesystem;
using Shader::Ampere::Instruction;

int main(int argc, char** argv) {
    bool quiet = false;
    bool attributes = false;
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        if (arg == "-q") {
            quiet = true;
        } else if (arg == "-a") {
            attributes = true;
        } else if (fs::is_directory(arg)) {
            for (const auto& entry : fs::directory_iterator(arg)) {
                if (entry.path().extension() == ".bin") files.push_back(entry.path());
            }
        } else {
            files.emplace_back(arg);
        }
    }
    std::sort(files.begin(), files.end());
    // (error, opcode) -> files
    std::map<std::pair<std::string, u32>, std::vector<std::string>> failures;
    size_t passed = 0;
    for (const auto& path : files) {
        std::ifstream file(path, std::ios::binary);
        std::vector<Instruction> code(fs::file_size(path) / sizeof(Instruction));
        file.read(reinterpret_cast<char*>(code.data()), code.size() * sizeof(Instruction));
        const auto name = path.filename().string();
        if (attributes) {
            for (size_t i = 0; i < code.size(); ++i) {
                const auto& insn = code[i];
                const u32 opcode = insn.Bits(0, 12);
                if (opcode != 0x321 && opcode != 0x322 && opcode != 0x326 && opcode != 0x526) continue;
                const bool ipa = opcode == 0x326 || opcode == 0x526;
                std::printf("  %s +%#06zx %s a[%#05x] x%u vertex R%u [%08x %08x %08x %08x]\n", name.c_str(),
                            i * sizeof(Instruction), ipa ? "IPA" : opcode == 0x321 ? "ALD" : "AST",
                            insn.Bits(ipa ? 38 : 40, 10), ipa ? 1 : insn.Bits(74, 2) + 1,
                            opcode == 0x321 ? insn.Bits(32, 8) : opcode == 0x322 ? insn.Bits(64, 8) : 255,
                            insn.words[0], insn.words[1], insn.words[2], insn.words[3]);
            }
        }
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        if (lowered) {
            ++passed;
            if (!quiet) std::printf("OK   %s (%zu -> %zu)\n", name.c_str(), code.size(), lowered.instructions.size());
            continue;
        }
        const size_t index = lowered.error_offset / sizeof(Instruction);
        const auto words = index < code.size() ? code[index].words : std::array<u32, 4>{};
        const u32 opcode = words[0] & 0xfff;
        std::printf("FAIL %s +%#x: %s [op %03x %08x %08x %08x %08x]\n", name.c_str(), lowered.error_offset,
                    std::string{lowered.error}.c_str(), opcode, words[0], words[1], words[2], words[3]);
        failures[{std::string{lowered.error}, opcode}].push_back(name);
    }
    std::printf("\n%zu of %zu lower\n", passed, files.size());
    for (const auto& [key, names] : failures) {
        std::printf("%4zu  op %03x  %s\n", names.size(), key.second, key.first.c_str());
    }
    return failures.empty() ? 0 : 1;
}
