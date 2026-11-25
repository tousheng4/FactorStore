#include "factor_store/factor_store.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace factor_store {
namespace {

using FactorId = std::size_t;
using ContractId = std::uint32_t;
using DayIndex = std::uint16_t;
using BlockKey = std::uint64_t;

constexpr std::size_t max_days_in_year = 366;

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
    DayIndex day;
    double value;
};

class YearBlock {
public:
    [[nodiscard]] std::optional<double> find(DayIndex day) const {
        const auto position = std::lower_bound(days_.begin(), days_.end(), day);
        if (position == days_.end() || *position != day) {
            return std::nullopt;
        }
        return values_[position - days_.begin()];
    }

    void upsert(DayIndex day, double value) {
        const auto position = std::lower_bound(days_.begin(), days_.end(), day);
        const auto index = position - days_.begin();
        if (position != days_.end() && *position == day) {
            values_[index] = value;
            return;
        }

        if (days_.size() == days_.capacity() || values_.size() == values_.capacity()) {
            const auto capacity = std::min(max_days_in_year, 2 * days_.size() + 1);
            // Complete both allocations before changing either array's length.
            days_.reserve(capacity);
            values_.reserve(capacity);
        }
        days_.insert(days_.begin() + index, day);
        values_.insert(values_.begin() + index, value);
    }

    void merge(const std::vector<IndexedValue>& points) {
        YearBlock merged;
        const auto capacity = std::min(max_days_in_year, days_.size() + points.size());
        merged.days_.reserve(capacity);
        merged.values_.reserve(capacity);

        std::size_t current = 0;
        for (const auto& point : points) {
            while (current < days_.size() && days_[current] < point.day) {
                merged.append(days_[current], values_[current]);
                ++current;
            }
            if (current < days_.size() && days_[current] == point.day) {
                ++current;
            }
            merged.append(point.day, point.value);
        }
        while (current < days_.size()) {
            merged.append(days_[current], values_[current]);
            ++current;
        }
        *this = std::move(merged);
    }

private:
    void append(DayIndex day, double value) {
        days_.push_back(day);
        values_.push_back(value);
    }

    std::vector<DayIndex> days_;
    std::vector<double> values_;
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
    std::unordered_map<BlockKey, YearBlock, BlockKeyHash> blocks;
};

struct QueryDay {
    DayIndex day;
    std::size_t position;
};

}  // namespace

struct FactorStore::Impl {
    // The owner containers keep strings stable even when the index tables rehash.
    std::deque<std::string> contract_names;
    std::unordered_map<std::string_view, ContractId> contract_ids;
    std::vector<std::unique_ptr<Factor>> factors;
    std::unordered_map<std::string_view, FactorId> factor_ids;

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

    const auto key = block_key(impl_->get_or_add_contract(contract), year);
    impl_->write_factor(factor, [&](Factor& target) {
        target.write_block(key, [&](YearBlock& block) { block.merge(indexed); });
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
    for (const auto& contract : contracts) {
        keys.push_back(block_key(impl_->get_or_add_contract(contract), indexed.year));
    }

    std::size_t offset = 0;
    for (const auto& factor : factors) {
        impl_->write_factor(factor, [&](Factor& target) {
            for (std::size_t c = 0; c < keys.size(); ++c) {
                target.write_block(keys[c], [&](YearBlock& block) {
                    block.upsert(indexed.day, values[offset + c]);
                });
            }
        });
        offset += keys.size();
    }
}

std::size_t FactorStore::factor_count() const {
    return impl_->factors.size();
}

bool FactorStore::erase_factor(std::string_view factor) {
    validate_name(factor);
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

    std::unordered_map<int, std::vector<QueryDay>> by_year;
    for (std::size_t d = 0; d < dates.size(); ++d) {
        const auto date = parse_date(dates[d]);
        by_year[date.year].push_back({date.day, d});
    }

    std::vector<std::optional<ContractId>> contracts_by_position;
    contracts_by_position.reserve(contracts.size());
    for (const auto& contract : contracts) {
        const auto entry = impl_->contract_ids.find(contract);
        contracts_by_position.push_back(entry == impl_->contract_ids.end()
                                            ? std::nullopt
                                            : std::optional<ContractId>(entry->second));
    }

    result.values.resize(cell_count);
    result.validity.resize(word_count);
    for (std::size_t f = 0; f < factors.size(); ++f) {
        const auto entry = impl_->factor_ids.find(factors[f]);
        if (entry == impl_->factor_ids.end()) {
            continue;
        }
        const auto& blocks = impl_->factors[entry->second]->blocks;
        for (std::size_t c = 0; c < contracts.size(); ++c) {
            const auto contract_id = contracts_by_position[c];
            if (!contract_id) {
                continue;
            }
            const auto offset = (f * contracts.size() + c) * dates.size();
            for (const auto& [year, days] : by_year) {
                const auto block = blocks.find(block_key(*contract_id, year));
                if (block == blocks.end()) {
                    continue;
                }
                for (const auto& day : days) {
                    if (const auto value = block->second.find(day.day)) {
                        const auto position = offset + day.position;
                        result.values[position] = *value;
                        result.validity[position / 64] |= std::uint64_t{1} << (position % 64);
                    }
                }
            }
        }
    }
    return result;
}

}  // namespace factor_store

