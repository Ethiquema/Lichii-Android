// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <algorithm>

#include <boost/icl/interval_set.hpp>
#include "common/common_types.h"
#include "common/container/unordered_set.h"

namespace Dynarmic::Backend {

template<typename P>
void BlockRangeInformation<P>::AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location) {
    if (boost::icl::is_empty(range)) {
        return;
    }
    const P first = boost::icl::first(range);
    const P last = boost::icl::last(range);
    ranges.emplace(first, Range{last, location});
    max_span = (std::max)(max_span, P(last - first));
}

template<typename P>
void BlockRangeInformation<P>::ClearCache() {
    ranges.clear();
    max_span = 0;
}

template<typename P>
::Common::unordered_set<IR::LocationDescriptor> BlockRangeInformation<P>::InvalidateRanges(const boost::icl::interval_set<P>& invalidated) {
    ::Common::unordered_set<IR::LocationDescriptor> erase_locations;
    for (const auto& interval : invalidated) {
        const P first = boost::icl::first(interval);
        const P last = boost::icl::last(interval);
        const P search_begin = first > max_span ? P(first - max_span) : P(0);
        auto it = ranges.lower_bound(search_begin);
        while (it != ranges.end() && it->first <= last) {
            if (it->second.last < first) {
                ++it;
                continue;
            }
            erase_locations.insert(it->second.location);
            it = ranges.erase(it);
        }
    }
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
