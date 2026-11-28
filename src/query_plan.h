#pragma once

#include "store_types.h"

#include <algorithm>
#include <vector>

namespace factor_store::detail {

struct QueryDay {
    DayIndex day;
    std::size_t position;
};

struct QueryRun {
    DayIndex day;
    std::size_t position;
    std::size_t length;
};

struct QueryDays {
    void add(DayIndex day, std::size_t position);

    void sort_if_needed() {
        if (!sorted) {
            // Reorder traversal without changing any request's output position.
            std::sort(days.begin(), days.end(), [](const auto& a, const auto& b) {
                return a.day < b.day;
            });
            sorted = true;
        }
    }

    void plan_dense_reads();

    void plan_sparse_slice();

    std::vector<QueryDay> days;
    bool sorted = true;
    // Sparse reads may reorder days. The dense plan retains the original positions.
    std::vector<QueryRun> dense_runs;
    std::vector<QueryDay> dense_points;
    std::vector<DayIndex> sparse_slice;
};

}  // namespace factor_store::detail
