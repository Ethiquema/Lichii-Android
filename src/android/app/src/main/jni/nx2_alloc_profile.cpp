// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Sampling heap profiler for operator new, for finding what grows host memory on phones where
// heapprofd can't stay attached. With EDEN_NX2_ALLOC_PROFILE set, about one allocation per 8 MiB
// is sampled with its call stack; every 10 s the stacks holding the most live sampled bytes are
// appended to files/alloc_profile.txt (or the path the variable names) as offsets into this
// library (symbolize with llvm-addr2line on the unstripped .so). The dump writes with plain
// write(): it runs inside operator new and must not allocate or take the logger's locks.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>

#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <unwind.h>

namespace {

constexpr std::size_t SampleInterval = 8 << 20;
constexpr std::size_t MaxFrames = 24;
constexpr std::size_t NumStacks = 4096;
constexpr std::size_t NumPointers = 1 << 18;
constexpr std::size_t ProbeLimit = 16;

struct Stack {
    std::uint64_t hash;
    std::uint32_t depth;
    std::array<std::uintptr_t, MaxFrames> pcs;
    std::int64_t live;
    std::int64_t total;
};

struct Pointer {
    std::atomic<void*> ptr;
    std::uint32_t stack;
    std::int64_t weight;
};

std::mutex g_mutex;
Stack g_stacks[NumStacks];
Pointer g_pointers[NumPointers];
std::atomic<std::size_t> g_pointer_count{0};
thread_local std::int64_t t_countdown = SampleInterval;
thread_local bool t_busy = false;

struct UnwindState {
    std::uintptr_t* pcs;
    std::uint32_t depth;
};

_Unwind_Reason_Code UnwindFrame(_Unwind_Context* context, void* arg) {
    auto* state = static_cast<UnwindState*>(arg);
    const std::uintptr_t pc = _Unwind_GetIP(context);
    if (pc == 0 || state->depth >= MaxFrames) {
        return _URC_END_OF_STACK;
    }
    state->pcs[state->depth++] = pc;
    return _URC_NO_REASON;
}

std::size_t PointerSlot(void* ptr) {
    return (reinterpret_cast<std::uintptr_t>(ptr) >> 4) * 0x9E3779B97F4A7C15ULL >> 46;
}

std::uintptr_t LibraryBase() {
    static const std::uintptr_t base = [] {
        Dl_info info{};
        dladdr(reinterpret_cast<void*>(&LibraryBase), &info);
        return reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    }();
    return base;
}

void Dump() {
    struct Top {
        std::int64_t live;
        std::int64_t total;
        std::uint32_t depth;
        std::array<std::uintptr_t, MaxFrames> pcs;
    };
    std::array<Top, 16> top{};
    std::int64_t live_sum = 0;
    {
        std::scoped_lock lock{g_mutex};
        for (const Stack& stack : g_stacks) {
            if (stack.depth == 0) {
                continue;
            }
            live_sum += stack.live;
            std::size_t i = top.size();
            while (i > 0 && top[i - 1].live < stack.live) {
                --i;
            }
            if (i == top.size()) {
                continue;
            }
            for (std::size_t j = top.size() - 1; j > i; --j) {
                top[j] = top[j - 1];
            }
            top[i] = {stack.live, stack.total, stack.depth, stack.pcs};
        }
    }
    const char* path = std::getenv("EDEN_NX2_ALLOC_PROFILE");
    if (path == nullptr || path[0] != '/') {
        path = "/sdcard/Android/data/dev.eden.nx2.relWithDebInfo/files/alloc_profile.txt";
    }
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }
    const std::uintptr_t base = LibraryBase();
    char line[1024];
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    int length = std::snprintf(line, sizeof(line), "dump %lld s: %lld MiB live\n",
                               static_cast<long long>(now), static_cast<long long>(live_sum >> 20));
    write(fd, line, length);
    for (const Top& entry : top) {
        if (entry.depth == 0) {
            break;
        }
        length = std::snprintf(line, sizeof(line), "%lld MiB live, %lld MiB allocated:",
                               static_cast<long long>(entry.live >> 20),
                               static_cast<long long>(entry.total >> 20));
        for (std::uint32_t i = 2; i < entry.depth && length < 900; ++i) {
            length += std::snprintf(line + length, sizeof(line) - length, " %#lx",
                                    static_cast<unsigned long>(entry.pcs[i] - base));
        }
        line[length++] = '\n';
        write(fd, line, length);
    }
    close(fd);
}

void Sample(void* ptr, std::size_t size) {
    t_countdown = SampleInterval;
    if (std::getenv("EDEN_NX2_ALLOC_PROFILE") == nullptr) {
        return;
    }
    t_busy = true;
    const std::int64_t weight =
        static_cast<std::int64_t>(size >= SampleInterval ? size : SampleInterval);
    std::array<std::uintptr_t, MaxFrames> pcs{};
    UnwindState state{pcs.data(), 0};
    _Unwind_Backtrace(UnwindFrame, &state);
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::uint32_t i = 0; i < state.depth; ++i) {
        hash = (hash ^ pcs[i]) * 0x100000001b3ULL;
    }
    bool dump = false;
    {
        std::scoped_lock lock{g_mutex};
        std::size_t index = hash % NumStacks;
        for (std::size_t probe = 0; probe < NumStacks; ++probe, index = (index + 1) % NumStacks) {
            Stack& stack = g_stacks[index];
            if (stack.depth == 0) {
                stack.hash = hash;
                stack.depth = state.depth;
                stack.pcs = pcs;
            } else if (stack.hash != hash) {
                continue;
            }
            stack.live += weight;
            stack.total += weight;
            break;
        }
        std::size_t slot = PointerSlot(ptr);
        for (std::size_t probe = 0; probe < ProbeLimit; ++probe, slot = (slot + 1) % NumPointers) {
            Pointer& entry = g_pointers[slot];
            if (entry.ptr.load(std::memory_order_relaxed) == nullptr) {
                entry.stack = static_cast<std::uint32_t>(index);
                entry.weight = weight;
                entry.ptr.store(ptr, std::memory_order_release);
                g_pointer_count.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        static auto next_dump = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        if (const auto now = std::chrono::steady_clock::now(); now >= next_dump) {
            next_dump = now + std::chrono::seconds{10};
            dump = true;
        }
    }
    if (dump) {
        Dump();
    }
    t_busy = false;
}

void Forget(void* ptr) {
    if (ptr == nullptr || g_pointer_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::size_t slot = PointerSlot(ptr);
    for (std::size_t probe = 0; probe < ProbeLimit; ++probe, slot = (slot + 1) % NumPointers) {
        Pointer& entry = g_pointers[slot];
        if (entry.ptr.load(std::memory_order_acquire) != ptr) {
            continue;
        }
        std::scoped_lock lock{g_mutex};
        if (entry.ptr.load(std::memory_order_relaxed) == ptr) {
            g_stacks[entry.stack].live -= entry.weight;
            entry.ptr.store(nullptr, std::memory_order_relaxed);
            g_pointer_count.fetch_sub(1, std::memory_order_relaxed);
        }
        return;
    }
}

void* Allocate(std::size_t size) noexcept {
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr != nullptr && !t_busy) {
        t_countdown -= static_cast<std::int64_t>(size);
        if (t_countdown <= 0) [[unlikely]] {
            Sample(ptr, size);
        }
    }
    return ptr;
}

void Deallocate(void* ptr) noexcept {
    Forget(ptr);
    std::free(ptr);
}

} // namespace

void* operator new(std::size_t size) {
    void* ptr = Allocate(size);
    if (ptr == nullptr) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return Allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return Allocate(size);
}

void operator delete(void* ptr) noexcept {
    Deallocate(ptr);
}

void operator delete[](void* ptr) noexcept {
    Deallocate(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    Deallocate(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    Deallocate(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    Deallocate(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    Deallocate(ptr);
}
