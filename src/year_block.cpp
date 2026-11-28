#include "year_block.h"
#include "query_plan.h"

#include <cstring>

namespace factor_store::detail {

namespace {

std::uint64_t low_bits(std::size_t count) noexcept {
    return ~std::uint64_t{0} >> (64 - count);
}

}  // namespace

std::uint64_t DenseBlock::read_mask(std::size_t day, std::size_t count) const noexcept {
    const auto word = day / 64;
    const auto bit = day % 64;
    auto mask = present[word] >> bit;
    if (count > 64 - bit) {
        mask |= present[word + 1] << (64 - bit);
    }
    return mask & low_bits(count);
}

void YearBlock::read_many(QueryDays& requested, std::size_t offset, QueryResult& result) const {
    if (const auto* owner = std::get_if<DenseOwner>(&storage_)) {
        if (requested.dense_runs.empty()) {
            read_dense_points(**owner, requested.days, offset, result);
        } else {
            for (const auto& run : requested.dense_runs) {
                read_dense_run(**owner, run, offset, result);
            }
            read_dense_points(**owner, requested.dense_points, offset, result);
        }
        return;
    }
    if (const auto* single = std::get_if<IndexedValue>(&storage_)) {
        for (const auto& request : requested.days) {
            if (request.day == single->day) {
                set_query_value(result, offset + request.position, single->value);
            }
        }
        return;
    }

    const auto& sparse = std::get<SparseBlock>(storage_);
    if (!requested.sparse_slice.empty() && read_sparse_slice(sparse, requested, offset, result)) {
        return;
    }
    // For many requested dates, amortize a single pass over the sparse block.
    constexpr std::size_t min_merge_dates = 24;
    if (requested.days.size() >= min_merge_dates &&
        (requested.days.size() >= sparse.days.size() / 4 ||
         (sparse.days.size() >= dense_threshold &&
          requested.days.size() >= sparse.days.size() / 8))) {
        requested.sort_if_needed();
        read_sparse_in_order(sparse, requested.days, offset, result);
        return;
    }

    for (const auto& request : requested.days) {
        read_sparse_day(sparse, request.day, offset + request.position, result);
    }
}

bool YearBlock::merge(const std::vector<IndexedValue>& points) {
    if (auto* single = std::get_if<IndexedValue>(&storage_);
        single && points.size() == 1 &&
        (single->day == empty_day || single->day == points.front().day)) {
        *single = points.front();
        return false;
    }
    auto* owner = std::get_if<DenseOwner>(&storage_);
    const bool promote_first = !owner && points.size() >= dense_threshold;
    if (promote_first) {
        promote();
        owner = std::get_if<DenseOwner>(&storage_);
    }
    if (owner) {
        for (const auto& point : points) {
            (*owner)->upsert(point.day, point.value);
        }
        return promote_first;
    }

    const auto old = sparse_view();
    SparseBlock merged;
    merged.reserve(std::min(max_days_in_year, old.size + points.size()));

    std::size_t current = 0;
    for (const auto& point : points) {
        while (current < old.size && old.days[current] < point.day) {
            merged.append(old.days[current], old.values[current]);
            ++current;
        }
        if (current < old.size && old.days[current] == point.day) {
            ++current;
        }
        merged.append(point.day, point.value);
    }
    while (current < old.size) {
        merged.append(old.days[current], old.values[current]);
        ++current;
    }
    if (merged.days.size() >= dense_threshold) {
        install_dense(merged.view());
        return true;
    }
    storage_.emplace<SparseBlock>(std::move(merged));
    return false;
}

void YearBlock::read_dense_run(const DenseBlock& dense, const QueryRun& run,
                               std::size_t offset, QueryResult& result) noexcept {
    const auto start = offset + run.position;
    std::memcpy(result.values.data() + start, dense.values.data() + run.day,
                run.length * sizeof(double));
    for (std::size_t copied = 0; copied < run.length;) {
        const auto output = start + copied;
        const auto bit = output % 64;
        const auto count = std::min(run.length - copied, 64 - bit);
        const auto mask = dense.read_mask(run.day + copied, count);
        result.validity[output / 64] |= mask << bit;
        copied += count;
    }
}

void YearBlock::read_dense_points(const DenseBlock& dense, const std::vector<QueryDay>& requested,
                                  std::size_t offset, QueryResult& result) noexcept {
    auto word = offset / 64;
    std::uint64_t mask = 0;
    for (const auto& request : requested) {
        if (dense.has_value(request.day)) {
            const auto position = offset + request.position;
            result.values[position] = dense.values[request.day];
            const auto next_word = position / 64;
            if (next_word != word) {
                result.validity[word] |= mask;
                word = next_word;
                mask = 0;
            }
            mask |= std::uint64_t{1} << (position % 64);
        }
    }
    // Other years and neighboring rows can contribute to the same word.
    if (mask != 0) {
        result.validity[word] |= mask;
    }
}

bool YearBlock::read_sparse_slice(const SparseBlock& sparse, const QueryDays& requested,
                                  std::size_t offset, QueryResult& result) noexcept {
    const auto& days = requested.sparse_slice;
    auto first = sparse.days.begin();
    if (days.front() != *first) {
        first = std::lower_bound(first, sparse.days.end(), days.front());
    }
    if (days.size() > static_cast<std::size_t>(sparse.days.end() - first) ||
        !std::equal(days.begin(), days.end(), first)) {
        return false;
    }

    auto position = offset + requested.days.front().position;
    const auto index = first - sparse.days.begin();
    std::memcpy(result.values.data() + position, sparse.values.data() + index,
                days.size() * sizeof(double));
    for (auto remaining = days.size(); remaining != 0;) {
        const auto bit = position % 64;
        const auto count = std::min(remaining, 64 - bit);
        result.validity[position / 64] |= low_bits(count) << bit;
        position += count;
        remaining -= count;
    }
    return true;
}

void YearBlock::read_sparse_in_order(const SparseBlock& sparse,
                                     const std::vector<QueryDay>& requested,
                                     std::size_t offset, QueryResult& result) {
    auto day = std::lower_bound(sparse.days.begin(), sparse.days.end(), requested.front().day);
    for (const auto& request : requested) {
        while (day != sparse.days.end() && *day < request.day) {
            ++day;
        }
        if (day == sparse.days.end()) {
            break;
        }
        if (*day == request.day) {
            set_query_value(result, offset + request.position,
                            sparse.values[day - sparse.days.begin()]);
        }
    }
}

void YearBlock::expand_single(IndexedValue first, IndexedValue second) {
    if (second.day < first.day) {
        std::swap(first, second);
    }
    SparseBlock sparse;
    sparse.reserve(3);
    sparse.append(first.day, first.value);
    sparse.append(second.day, second.value);
    storage_.emplace<SparseBlock>(std::move(sparse));
}

SparseView YearBlock::sparse_view() const {
    if (const auto* single = std::get_if<IndexedValue>(&storage_)) {
        return {&single->day, &single->value, single->day == empty_day ? 0U : 1U};
    }
    return std::get<SparseBlock>(storage_).view();
}

void YearBlock::promote() {
    install_dense(sparse_view());
}

void YearBlock::install_dense(SparseView old) {
    auto dense = std::make_unique<DenseBlock>();
    for (std::size_t i = 0; i < old.size; ++i) {
        dense->upsert(old.days[i], old.values[i]);
    }
    // Publish after allocation and release whichever sparse representation was active.
    storage_.emplace<DenseOwner>(std::move(dense));
}

}  // namespace factor_store::detail
