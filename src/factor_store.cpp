#include "factor_store/factor_store.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace factor_store {
namespace {

using FactorId = std::size_t;
using ContractId = std::uint32_t;
using DayIndex = std::uint16_t;
using BlockKey = std::uint64_t;

constexpr std::size_t max_days_in_year = 366;
constexpr std::size_t dense_threshold = 192;
constexpr std::size_t max_promotions_per_daily_batch = 256;
constexpr std::size_t cells_per_promotion = 16;
constexpr DayIndex empty_day = static_cast<DayIndex>(max_days_in_year);

struct IndexedDate {
    int year;
    DayIndex day;
};

void validate_year(int year) {
    if (year < 1 || year > 9999) {
        throw std::invalid_argument("year must be between 1 and 9999");
    }
}

IndexedDate parse_date(TradingDate date) {
    const int year = date / 10000;
    const int month = (date / 100) % 100;
    const int day = date % 100;
    validate_year(year);
    if (month < 1 || month > 12) {
        throw std::invalid_argument("invalid month in trading date");
    }

    constexpr std::array<int, 12> month_lengths{31, 28, 31, 30, 31, 30,
                                                31, 31, 30, 31, 30, 31};
    constexpr std::array<int, 12> month_offsets{0,   31,  59,  90,  120, 151,
                                                181, 212, 243, 273, 304, 334};
    const bool leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int month_length = month_lengths[month - 1] + (leap_year && month == 2);
    if (day < 1 || day > month_length) {
        throw std::invalid_argument("invalid day in trading date");
    }
    const int offset = month_offsets[month - 1] + (leap_year && month > 2);
    return {year, static_cast<DayIndex>(offset + day - 1)};
}

void validate_name(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("factor and contract names must not be empty");
    }
}

void validate_unique_names(const std::vector<std::string>& names) {
    std::unordered_set<std::string_view> seen;
    seen.reserve(names.size());
    for (const auto& name : names) {
        validate_name(name);
        if (!seen.emplace(name).second) {
            throw std::invalid_argument("duplicate name on a write axis");
        }
    }
}

std::size_t checked_product(std::size_t a, std::size_t b) {
    if (b != 0 && a > std::numeric_limits<std::size_t>::max() / b) {
        throw std::length_error("factor store matrix dimensions overflow");
    }
    return a * b;
}

BlockKey block_key(ContractId contract, int year) {
    return (static_cast<BlockKey>(contract) << 32) | static_cast<std::uint32_t>(year);
}

struct BlockKeyHash {
    std::size_t operator()(BlockKey key) const noexcept {
        // Mix contract bits into the low bits used by power-of-two bucket counts.
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        return static_cast<std::size_t>(key ^ (key >> 33));
    }
};

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

struct QueryDay {
    DayIndex day;
    std::size_t position;
};

struct QueryDays {
    void add(DayIndex day, std::size_t position) {
        if (!days.empty() && day < days.back().day) {
            sorted = false;
        }
        days.push_back({day, position});
    }

    void sort_if_needed() {
        if (!sorted) {
            // Reorder traversal without changing any request's output position.
            std::sort(days.begin(), days.end(), [](const auto& a, const auto& b) {
                return a.day < b.day;
            });
            sorted = true;
        }
    }

    std::vector<QueryDay> days;
    bool sorted = true;
};

void set_query_value(QueryResult& result, std::size_t position, double value) noexcept {
    result.values[position] = value;
    result.validity[position / 64] |= std::uint64_t{1} << (position % 64);
}

struct DenseBlock {
    std::array<double, max_days_in_year> values{};
    std::bitset<max_days_in_year> present;

    void upsert(DayIndex day, double value) noexcept {
        values[day] = value;
        present[day] = true;
    }
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

    void read_many(QueryDays& requested, std::size_t offset, QueryResult& result) const {
        if (const auto* owner = std::get_if<DenseOwner>(&storage_)) {
            const DenseBlock& dense = **owner;
            for (const auto& request : requested.days) {
                if (dense.present[request.day]) {
                    set_query_value(result, offset + request.position, dense.values[request.day]);
                }
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
            const auto day = std::lower_bound(sparse.days.begin(), sparse.days.end(), request.day);
            if (day != sparse.days.end() && *day == request.day) {
                set_query_value(result, offset + request.position,
                                sparse.values[day - sparse.days.begin()]);
            }
        }
    }

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
    bool merge(const std::vector<IndexedValue>& points) {
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

private:
    friend class PromotionQueue;

    [[nodiscard]] bool ready_for_promotion() const noexcept {
        const auto* sparse = std::get_if<SparseBlock>(&storage_);
        return sparse && sparse->days.size() >= dense_threshold;
    }

    static void read_sparse_in_order(const SparseBlock& sparse,
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

    void expand_single(IndexedValue first, IndexedValue second) {
        if (second.day < first.day) {
            std::swap(first, second);
        }
        SparseBlock sparse;
        sparse.reserve(3);
        sparse.append(first.day, first.value);
        sparse.append(second.day, second.value);
        storage_.emplace<SparseBlock>(std::move(sparse));
    }

    [[nodiscard]] SparseView sparse_view() const {
        if (const auto* single = std::get_if<IndexedValue>(&storage_)) {
            return {&single->day, &single->value, single->day == empty_day ? 0U : 1U};
        }
        return std::get<SparseBlock>(storage_).view();
    }

    void promote() {
        install_dense(sparse_view());
    }

    void install_dense(SparseView old) {
        auto dense = std::make_unique<DenseBlock>();
        for (std::size_t i = 0; i < old.size; ++i) {
            dense->upsert(old.days[i], old.values[i]);
        }
        // Publish after allocation and release whichever sparse representation was active.
        storage_.emplace<DenseOwner>(std::move(dense));
    }

    // Keep the first value in the block itself, then use SoA for multi-value sparse blocks.
    std::variant<IndexedValue, SparseBlock, DenseOwner> storage_;
    // Stays set while a task is in flight so cancel() can prevent a later requeue.
    Factor* pending_owner_ = nullptr;
};

class PromotionQueue {
public:
    void enqueue(YearBlock& block, Factor& owner) {
        const std::lock_guard lock(mutex_);
        block.pending_owner_ = &owner;
        link(block);
    }

    void cancel(YearBlock& block) {
        const std::lock_guard lock(mutex_);
        block.unlink();
        block.pending_owner_ = nullptr;
    }

    void run_after_daily_write(std::size_t cells);

private:
    struct Task {
        YearBlock* block;
        Factor* factor;
    };

    void link(YearBlock& block) noexcept {
        block.previous = head_.previous;
        block.next = &head_;
        head_.previous->next = &block;
        head_.previous = &block;
    }

    std::optional<Task> take() {
        const std::lock_guard lock(mutex_);
        if (head_.next == &head_) {
            return std::nullopt;
        }
        auto& block = static_cast<YearBlock&>(*head_.next);
        block.unlink();
        return Task{&block, block.pending_owner_};
    }

    void requeue(const Task& task) {
        const std::lock_guard lock(mutex_);
        if (task.block->pending_owner_ == task.factor) {
            link(*task.block);
        }
    }

    std::mutex mutex_;
    PromotionLink head_;
};

struct Factor {
    explicit Factor(std::string_view factor_name) : name(factor_name) {}

    template <class Write>
    void write_block(BlockKey key, Write write) {
        auto [block, inserted] = blocks.try_emplace(key);
        try {
            write(block->second);
        } catch (...) {
            if (inserted) {
                blocks.erase(block);
            }
            throw;
        }
    }

    const std::string name;
    mutable std::shared_mutex mutex;
    std::unordered_map<BlockKey, YearBlock, BlockKeyHash> blocks;
};

void PromotionQueue::run_after_daily_write(std::size_t cells) {
    const auto budget = std::min(max_promotions_per_daily_batch,
                                 1 + (cells - 1) / cells_per_promotion);
    for (std::size_t i = 0; i < budget; ++i) {
        const auto task = take();
        if (!task) {
            return;
        }
        // The catalog pins this task's owner. Only this factor is locked during conversion.
        const std::unique_lock lock(task->factor->mutex, std::try_to_lock);
        if (!lock) {
            requeue(*task);
            continue;
        }
        try {
            if (task->block->ready_for_promotion()) {
                task->block->promote();
            }
        } catch (const std::bad_alloc&) {
            // The write already succeeded. Keep the sparse block readable and retry later.
            requeue(*task);
            return;
        }
        cancel(*task->block);
    }
}

}  // namespace

struct FactorStore::Impl {
    mutable std::shared_mutex catalog_mutex;

    // Destroy the factors and their links before destroying the queue sentinel.
    PromotionQueue promotions;
    // The owner containers keep strings stable even when the index tables rehash.
    std::deque<std::string> contract_names;
    std::unordered_map<std::string_view, ContractId> contract_ids;
    std::vector<std::unique_ptr<Factor>> factors;
    std::unordered_map<std::string_view, FactorId> factor_ids;

    template <class Lock>
    std::vector<Lock> lock_factors(std::vector<FactorId> ids) const {
        // The caller holds the catalog lock, so the order is stable while acquiring locks.
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        std::vector<Lock> locks;
        locks.reserve(ids.size());
        for (const auto id : ids) {
            locks.emplace_back(factors[id]->mutex);
        }
        return locks;
    }

    ContractId get_or_add_contract(std::string_view name) {
        const auto existing = contract_ids.find(name);
        if (existing != contract_ids.end()) {
            return existing->second;
        }
        if (contract_names.size() > std::numeric_limits<ContractId>::max()) {
            throw std::length_error("contract ID capacity exhausted");
        }

        const auto id = static_cast<ContractId>(contract_names.size());
        contract_names.emplace_back(name);
        try {
            contract_ids.emplace(contract_names.back(), id);
        } catch (...) {
            contract_names.pop_back();
            throw;
        }
        return id;
    }

    template <class Write>
    void write_factor(std::string_view name, Write write) {
        const auto existing = factor_ids.find(name);
        if (existing != factor_ids.end()) {
            write(*factors[existing->second]);
            return;
        }

        // Publish a new factor only after it has data.
        auto factor = std::make_unique<Factor>(name);
        write(*factor);
        const auto id = factors.size();
        factors.push_back(std::move(factor));
        try {
            factor_ids.emplace(factors.back()->name, id);
        } catch (...) {
            factors.pop_back();
            throw;
        }
    }
};

FactorStore::FactorStore() : impl_(std::make_unique<Impl>()) {}

FactorStore::~FactorStore() = default;

void FactorStore::upsert_year(std::string_view factor,
                              std::string_view contract,
                              int year,
                              const std::vector<DailyValue>& points) {
    validate_name(factor);
    validate_name(contract);
    validate_year(year);
    if (points.empty()) {
        return;
    }

    std::bitset<max_days_in_year> seen;
    std::vector<IndexedValue> indexed;
    indexed.reserve(std::min(points.size(), max_days_in_year));
    for (const auto& point : points) {
        const auto date = parse_date(point.date);
        if (date.year != year || seen.test(date.day)) {
            throw std::invalid_argument("wrong year or duplicate date in annual batch");
        }
        seen.set(date.day);
        indexed.push_back({date.day, point.value});
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) {
        return a.day < b.day;
    });

    const auto write = [&](Factor& target, ContractId contract_id) {
        target.write_block(block_key(contract_id, year), [&](YearBlock& block) {
            if (block.merge(indexed)) {
                impl_->promotions.cancel(block);
            }
        });
    };
    {
        const std::shared_lock catalog_lock(impl_->catalog_mutex);
        const auto contract_entry = impl_->contract_ids.find(contract);
        const auto factor_entry = impl_->factor_ids.find(factor);
        if (contract_entry != impl_->contract_ids.end() && factor_entry != impl_->factor_ids.end()) {
            auto& target = *impl_->factors[factor_entry->second];
            const std::unique_lock factor_lock(target.mutex);
            write(target, contract_entry->second);
            return;
        }
    }

    const std::unique_lock catalog_lock(impl_->catalog_mutex);
    const auto contract_id = impl_->get_or_add_contract(contract);
    impl_->write_factor(factor, [&](Factor& target) {
        write(target, contract_id);
    });
}

void FactorStore::upsert_day(TradingDate date,
                             const std::vector<std::string>& factors,
                             const std::vector<std::string>& contracts,
                             const std::vector<double>& values) {
    const auto indexed = parse_date(date);
    const auto cell_count = checked_product(factors.size(), contracts.size());
    if (values.size() != cell_count) {
        throw std::invalid_argument("daily values do not match factor and contract axes");
    }
    validate_unique_names(factors);
    validate_unique_names(contracts);
    if (cell_count == 0) {
        return;
    }

    std::vector<BlockKey> keys;
    keys.reserve(contracts.size());
    const auto write_row = [&](Factor& target, std::size_t offset) {
        for (std::size_t c = 0; c < keys.size(); ++c) {
            target.write_block(keys[c], [&](YearBlock& block) {
                if (block.upsert(indexed.day, values[offset + c])) {
                    impl_->promotions.enqueue(block, target);
                }
            });
        }
    };
    {
        const std::shared_lock catalog_lock(impl_->catalog_mutex);
        for (const auto& contract : contracts) {
            const auto entry = impl_->contract_ids.find(contract);
            if (entry == impl_->contract_ids.end()) {
                break;
            }
            keys.push_back(block_key(entry->second, indexed.year));
        }
        if (keys.size() == contracts.size()) {
            std::vector<FactorId> ids;
            ids.reserve(factors.size());
            for (const auto& factor : factors) {
                const auto entry = impl_->factor_ids.find(factor);
                if (entry == impl_->factor_ids.end()) {
                    break;
                }
                ids.push_back(entry->second);
            }
            if (ids.size() == factors.size()) {
                {
                    const auto locks = impl_->lock_factors<std::unique_lock<std::shared_mutex>>(ids);
                    for (std::size_t f = 0; f < ids.size(); ++f) {
                        write_row(*impl_->factors[ids[f]], f * keys.size());
                    }
                }
                impl_->promotions.run_after_daily_write(cell_count);
                return;
            }
        }
    }

    // Recheck names under exclusive access because the catalog may have changed.
    const std::unique_lock catalog_lock(impl_->catalog_mutex);
    keys.clear();
    for (const auto& contract : contracts) {
        keys.push_back(block_key(impl_->get_or_add_contract(contract), indexed.year));
    }

    std::size_t offset = 0;
    for (const auto& factor : factors) {
        impl_->write_factor(factor, [&](Factor& target) { write_row(target, offset); });
        offset += keys.size();
    }
    impl_->promotions.run_after_daily_write(cell_count);
}

std::size_t FactorStore::factor_count() const {
    const std::shared_lock lock(impl_->catalog_mutex);
    return impl_->factors.size();
}

bool FactorStore::erase_factor(std::string_view factor) {
    validate_name(factor);
    const std::unique_lock lock(impl_->catalog_mutex);
    const auto entry = impl_->factor_ids.find(factor);
    if (entry == impl_->factor_ids.end()) {
        return false;
    }
    const auto id = entry->second;
    impl_->factor_ids.erase(entry);

    // Keep the directory compact by moving the last live factor into the hole.
    auto& factors = impl_->factors;
    if (id != factors.size() - 1) {
        factors[id] = std::move(factors.back());
        impl_->factor_ids.find(factors[id]->name)->second = id;
    }
    factors.pop_back();
    return true;
}

QueryResult FactorStore::query(const std::vector<std::string>& factors,
                               const std::vector<std::string>& contracts,
                               const std::vector<TradingDate>& dates) const {
    for (const auto& factor : factors) {
        validate_name(factor);
    }
    for (const auto& contract : contracts) {
        validate_name(contract);
    }

    QueryResult result{factors.size(), contracts.size(), dates.size(), {}, {}};
    if (factors.empty() || contracts.empty() || dates.empty()) {
        for (const auto date : dates) {
            parse_date(date);
        }
        return result;
    }

    const auto cell_count = checked_product(checked_product(factors.size(), contracts.size()),
                                            dates.size());
    const auto word_count = cell_count / 64 + (cell_count % 64 != 0);
    if (cell_count > result.values.max_size() || word_count > result.validity.max_size()) {
        throw std::length_error("query result exceeds buffer capacity");
    }

    std::unordered_map<int, QueryDays> by_year;
    for (std::size_t d = 0; d < dates.size(); ++d) {
        const auto date = parse_date(dates[d]);
        by_year[date.year].add(date.day, d);
    }

    std::vector<std::optional<ContractId>> contracts_by_position;
    contracts_by_position.reserve(contracts.size());
    std::vector<const Factor*> factors_by_position;
    factors_by_position.reserve(factors.size());
    std::vector<FactorId> ids;
    ids.reserve(factors.size());
    result.values.resize(cell_count);
    result.validity.resize(word_count);

    const std::shared_lock catalog_lock(impl_->catalog_mutex);
    for (const auto& contract : contracts) {
        const auto entry = impl_->contract_ids.find(contract);
        contracts_by_position.push_back(entry == impl_->contract_ids.end()
                                            ? std::nullopt
                                            : std::optional<ContractId>(entry->second));
    }

    for (const auto& factor : factors) {
        const auto entry = impl_->factor_ids.find(factor);
        if (entry == impl_->factor_ids.end()) {
            factors_by_position.push_back(nullptr);
        } else {
            ids.push_back(entry->second);
            factors_by_position.push_back(impl_->factors[entry->second].get());
        }
    }

    // Lock the whole read set before visiting any block.
    const auto locks = impl_->lock_factors<std::shared_lock<std::shared_mutex>>(std::move(ids));
    for (std::size_t f = 0; f < factors.size(); ++f) {
        if (!factors_by_position[f]) {
            continue;
        }
        const auto& blocks = factors_by_position[f]->blocks;
        for (std::size_t c = 0; c < contracts.size(); ++c) {
            const auto contract_id = contracts_by_position[c];
            if (!contract_id) {
                continue;
            }
            const auto offset = (f * contracts.size() + c) * dates.size();
            for (auto& [year, days] : by_year) {
                const auto block = blocks.find(block_key(*contract_id, year));
                if (block == blocks.end()) {
                    continue;
                }
                block->second.read_many(days, offset, result);
            }
        }
    }
    return result;
}

}  // namespace factor_store

