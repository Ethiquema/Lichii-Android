// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <boost/container/static_vector.hpp>

#include "dynarmic/src/dynarmic/common/spin_lock.h"

namespace Dynarmic {

using VAddr = std::uint64_t;
using Vector = std::array<std::uint64_t, 2>;

class ExclusiveMonitor {
public:
    explicit ExclusiveMonitor() noexcept {
        std::fill(exclusive_addresses.begin(), exclusive_addresses.end(), INVALID_EXCLUSIVE_ADDRESS);
    }

    /// Marks a region containing [address, address+size) to be exclusive to
    /// processor index.
    template<typename T, typename F>
    [[nodiscard]] inline T ReadAndMark(std::size_t index, VAddr address, F f) {
        static_assert(std::is_trivially_copyable_v<T>);
        const VAddr masked_address = address & RESERVATION_GRANULE_MASK;
        lock.Lock();
        exclusive_addresses[index] = masked_address;
        T const value = f();
        std::memcpy(exclusive_values[index].data(), std::addressof(value), sizeof(T));
        lock.Unlock();
        return value;
    }

    [[nodiscard]] inline bool CheckAndClear(std::size_t index, VAddr address) {
        const VAddr masked_address = address & RESERVATION_GRANULE_MASK;
        if (exclusive_addresses[index] != masked_address)
            return false;
        for (VAddr& other_address : exclusive_addresses)
            if (other_address == masked_address)
                other_address = INVALID_EXCLUSIVE_ADDRESS;
        return true;
    }

    /// Checks to see if processor index has exclusive access to the
    /// specified region. If it does, executes the operation then clears
    /// the exclusive state for processors if their exclusive region(s)
    /// contain [address, address+size).
    template<typename T, typename F>
    [[nodiscard]] inline bool DoExclusiveOperation(std::size_t index, VAddr address, F&& f) {
        static_assert(std::is_trivially_copyable_v<T>);
        bool result = false;
        lock.Lock();
        if (CheckAndClear(index, address)) {
            T saved_value{};
            std::memcpy(std::addressof(saved_value), exclusive_values[index].data(), sizeof(T));
            result = f(saved_value);
        }
        lock.Unlock();
        return result;
    }

    /// Unmark everything.
    inline void Clear() {
        lock.Lock();
        std::fill(exclusive_addresses.begin(), exclusive_addresses.end(), INVALID_EXCLUSIVE_ADDRESS);
        lock.Unlock();
    }

    /// Unmark processor id
    inline void ClearProcessor(size_t index) {
        lock.Lock();
        exclusive_addresses[index] = INVALID_EXCLUSIVE_ADDRESS;
        lock.Unlock();
    }

    static constexpr VAddr RESERVATION_GRANULE_MASK = 0xFFFF'FFFF'FFFF'FFFFull;
    static constexpr VAddr INVALID_EXCLUSIVE_ADDRESS = 0xDEAD'DEAD'DEAD'DEADull;
    static constexpr size_t MAX_NUM_CPU_CORES = 4; // Sync with src/core/hardware_properties
    std::array<VAddr, MAX_NUM_CPU_CORES> exclusive_addresses;
    std::array<Vector, MAX_NUM_CPU_CORES> exclusive_values;
    SpinLock lock;
};

}  // namespace Dynarmic
