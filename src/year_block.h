#pragma once

#include "store_types.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace factor_store::detail {

struct QueryDay;
struct QueryDays;
struct QueryRun;

inline constexpr std::size_t dense_threshold = 192;

struct IndexedValue {
    DayIndex day = empty_day;
    double value = 0.0;
};

struct SparseView {
    const DayIndex* days;
    const double* values;
    std::size_t size;
};

struct SparseBlock {
    void reserve(std::size_t capacity) {
        days.reserve(capacity);
        values.reserve(capacity);
    }

    void append(DayIndex day, double value) {
        days.push_back(day);
        values.push_back(value);
    }

    [[nodiscard]] SparseView view() const noexcept {
        return {days.data(), values.data(), days.size()};
    }

    std::vector<DayIndex> days;
    std::vector<double> values;
};

inline void set_query_value(QueryResult& result, std::size_t position, double value) noexcept {
    result.values[position] = value;
    result.validity[position / 64] |= std::uint64_t{1} << (position % 64);
}

struct DenseBlock {
    std::array<double, max_days_in_year> values{};
    std::array<std::uint64_t, (max_days_in_year + 63) / 64> present{};

    void upsert(DayIndex day, double value) noexcept {
        values[day] = value;
        present[day / 64] |= std::uint64_t{1} << (day % 64);
    }

    [[nodiscard]] bool has_value(DayIndex day) const noexcept {
        return (present[day / 64] & (std::uint64_t{1} << (day % 64))) != 0;
    }

    // Return up to 64 consecutive validity bits starting at the least significant bit.
    [[nodiscard]] std::uint64_t read_mask(std::size_t day, std::size_t count) const noexcept;
};

using DenseOwner = std::unique_ptr<DenseBlock>;

// Links live in map nodes, whose addresses stay stable when the map rehashes.
struct Factor;

struct PromotionLink {
    PromotionLink() = default;
    PromotionLink(const PromotionLink&) = delete;
    PromotionLink& operator=(const PromotionLink&) = delete;

    [[nodiscard]] bool linked() const noexcept {
        return next != this;
    }

    void unlink() noexcept {
        if (linked()) {
            previous->next = next;
            next->previous = previous;
            previous = this;
            next = this;
        }
    }

    PromotionLink* previous = this;
    PromotionLink* next = this;
};

class YearBlock : private PromotionLink {
public:
    ~YearBlock() {
        // A published block is destroyed with exclusive access to the catalog.
        // A newly inserted block that fails to initialize has no queue entry.
        unlink();
    }

    void read_one(DayIndex day, std::size_t position, QueryResult& result) const {
        if (const auto* owner = std::get_if<DenseOwner>(&storage_)) {
            const DenseBlock& dense = **owner;
            if (dense.has_value(day)) {
                set_query_value(result, position, dense.values[day]);
            }
        } else if (const auto* single = std::get_if<IndexedValue>(&storage_)) {
            if (single->day == day) {
                set_query_value(result, position, single->value);
            }
        } else {
            read_sparse_day(std::get<SparseBlock>(storage_), day, position, result);
        }
    }

    void read_many(QueryDays& requested, std::size_t offset, QueryResult& result) const;

    // A new date that crosses the threshold makes this block eligible for the queue.
    bool upsert(DayIndex day, double value) {
        if (auto* single = std::get_if<IndexedValue>(&storage_)) {
            if (single->day == empty_day || single->day == day) {
                *single = {day, value};
            } else {
                expand_single(*single, {day, value});
            }
            return false;
        }
        if (auto* owner = std::get_if<DenseOwner>(&storage_)) {
            (*owner)->upsert(day, value);
            return false;
        }
        auto& sparse = std::get<SparseBlock>(storage_);
        const auto position = day > sparse.days.back()
                                  ? sparse.days.end()
                                  : std::lower_bound(sparse.days.begin(), sparse.days.end(), day);
        const auto index = position - sparse.days.begin();
        if (position != sparse.days.end() && *position == day) {
            sparse.values[index] = value;
            return false;
        }

        if (sparse.days.size() == sparse.days.capacity() ||
            sparse.values.size() == sparse.values.capacity()) {
            const auto capacity = std::min(max_days_in_year, 2 * sparse.days.size() + 1);
            // Complete both allocations before changing either array's length.
            sparse.reserve(capacity);
        }
        sparse.days.insert(sparse.days.begin() + index, day);
        sparse.values.insert(sparse.values.begin() + index, value);
        return sparse.days.size() == dense_threshold;
    }

    // Return whether the caller needs to cancel a pending promotion.
    bool merge(const std::vector<IndexedValue>& points);

private:
    friend class PromotionQueue;

    static void read_dense_run(const DenseBlock& dense, const QueryRun& run,
                               std::size_t offset, QueryResult& result) noexcept;

    static void read_dense_points(const DenseBlock& dense, const std::vector<QueryDay>& requested,
                                  std::size_t offset, QueryResult& result) noexcept;

    static void read_sparse_day(const SparseBlock& sparse, DayIndex requested,
                                std::size_t position, QueryResult& result) {
        const auto day = std::lower_bound(sparse.days.begin(), sparse.days.end(), requested);
        if (day != sparse.days.end() && *day == requested) {
            set_query_value(result, position, sparse.values[day - sparse.days.begin()]);
        }
    }

    static bool read_sparse_slice(const SparseBlock& sparse, const QueryDays& requested,
                                  std::size_t offset, QueryResult& result) noexcept;

    [[nodiscard]] bool ready_for_promotion() const noexcept {
        const auto* sparse = std::get_if<SparseBlock>(&storage_);
        return sparse && sparse->days.size() >= dense_threshold;
    }

    static void read_sparse_in_order(const SparseBlock& sparse,
                                     const std::vector<QueryDay>& requested,
                                     std::size_t offset, QueryResult& result);

    void expand_single(IndexedValue first, IndexedValue second);

    [[nodiscard]] SparseView sparse_view() const;

    void promote();

    void install_dense(SparseView old);

    // Keep the first value in the block itself, then use SoA for multi-value sparse blocks.
    std::variant<IndexedValue, SparseBlock, DenseOwner> storage_;
    // Stays set while a task is in flight so cancel() can prevent a later requeue.
    Factor* pending_owner_ = nullptr;
};

}  // namespace factor_store::detail
