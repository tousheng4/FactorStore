#include "factor_store_impl.h"
#include "query_plan.h"

#include <optional>

namespace factor_store::detail {

namespace {

constexpr std::size_t min_dense_run_days = 5;
constexpr std::size_t min_sparse_slice_days = 16;

}  // namespace

void QueryDays::add(DayIndex day, std::size_t position) {
    if (!days.empty() && day < days.back().day) {
        sorted = false;
    }
    days.push_back({day, position});
}

void QueryDays::plan_dense_reads() {
    if (days.size() < min_dense_run_days) {
        return;
    }
    std::size_t first_point = 0;
    for (std::size_t begin = 0; begin < days.size();) {
        std::size_t end = begin + 1;
        while (end < days.size() &&
               days[end].day == days[end - 1].day + 1 &&
               days[end].position == days[end - 1].position + 1) {
            ++end;
        }
        if (end - begin >= min_dense_run_days) {
            dense_points.insert(dense_points.end(), days.begin() + first_point,
                                days.begin() + begin);
            dense_runs.push_back({days[begin].day, days[begin].position, end - begin});
            first_point = end;
        }
        begin = end;
    }
    if (!dense_runs.empty()) {
        dense_points.insert(dense_points.end(), days.begin() + first_point, days.end());
    }
}

void QueryDays::plan_sparse_slice() {
    if (days.size() < min_sparse_slice_days || !sorted ||
        days.back().position - days.front().position != days.size() - 1) {
        return;
    }
    const auto duplicate = std::adjacent_find(days.begin(), days.end(),
                                              [](const QueryDay& a, const QueryDay& b) {
                                                  return a.day == b.day;
                                              });
    if (duplicate != days.end()) {
        return;
    }
    // Pack the day keys once for contiguous comparison with each block's SoA.
    sparse_slice.reserve(days.size());
    for (const auto& requested : days) {
        sparse_slice.push_back(requested.day);
    }
}

}  // namespace factor_store::detail

namespace factor_store {

using detail::ContractId;
using detail::Factor;
using detail::FactorId;
using detail::IndexedDate;
using detail::QueryDays;
using detail::block_key;
using detail::checked_product;
using detail::parse_date;
using detail::validate_name;

namespace {

constexpr std::size_t min_scan_contracts = 64;
constexpr std::size_t scan_probe_ratio = 4;
constexpr std::size_t min_scan_savings_per_contract = 7;

struct QueryContractPositions {
    QueryContractPositions(const std::vector<std::optional<ContractId>>& contracts,
                           std::size_t known_count)
        : next(contracts.size(), contracts.size()) {
        first.reserve(known_count);
        for (std::size_t c = 0; c < contracts.size(); ++c) {
            if (const auto contract = contracts[c]) {
                auto [entry, inserted] = first.try_emplace(*contract, c);
                if (!inserted) {
                    next[c] = entry->second;
                    entry->second = c;
                }
            }
        }
    }

    std::unordered_map<ContractId, std::size_t> first;
    std::vector<std::size_t> next;  // Chains all output positions for repeated contracts.
};

}  // namespace

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

    std::optional<IndexedDate> single_day;
    std::unordered_map<int, QueryDays> by_year;
    if (dates.size() == 1) {
        single_day = parse_date(dates.front());
    } else {
        for (std::size_t d = 0; d < dates.size(); ++d) {
            const auto date = parse_date(dates[d]);
            by_year[date.year].add(date.day, d);
        }
        for (auto& entry : by_year) {
            entry.second.plan_dense_reads();
            entry.second.plan_sparse_slice();
        }
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
    std::size_t known_contract_count = 0;
    for (const auto& contract : contracts) {
        const auto entry = impl_->contract_ids.find(contract);
        if (entry == impl_->contract_ids.end()) {
            contracts_by_position.push_back(std::nullopt);
        } else {
            contracts_by_position.push_back(entry->second);
            ++known_contract_count;
        }
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
    const auto requested_blocks = known_contract_count * (single_day ? 1 : by_year.size());
    const auto scan_block_limit = requested_blocks / scan_probe_ratio;
    std::optional<QueryContractPositions> scan_positions;
    if (known_contract_count >= min_scan_contracts) {
        std::size_t estimated_savings = 0;
        for (const auto* factor : factors_by_position) {
            if (factor && factor->blocks.size() <= scan_block_limit) {
                estimated_savings += requested_blocks - factor->blocks.size();
            }
        }
        // Build the shared index only when enough lookups can be avoided to pay for it.
        if (estimated_savings / contracts.size() >= min_scan_savings_per_contract) {
            scan_positions.emplace(contracts_by_position, known_contract_count);
        }
    }

    for (std::size_t f = 0; f < factors.size(); ++f) {
        if (!factors_by_position[f]) {
            continue;
        }
        const auto& blocks = factors_by_position[f]->blocks;
        const auto factor_offset = f * contracts.size() * dates.size();
        if (scan_positions && blocks.size() <= scan_block_limit) {
            for (const auto& [key, block] : blocks) {
                const auto year = static_cast<int>(static_cast<std::uint32_t>(key));
                QueryDays* requested = nullptr;
                if (single_day) {
                    if (year != single_day->year) {
                        continue;
                    }
                } else {
                    const auto entry = by_year.find(year);
                    if (entry == by_year.end()) {
                        continue;
                    }
                    requested = &entry->second;
                }
                const auto entry = scan_positions->first.find(static_cast<ContractId>(key >> 32));
                if (entry == scan_positions->first.end()) {
                    continue;
                }
                for (auto c = entry->second; c != contracts.size(); c = scan_positions->next[c]) {
                    const auto offset = factor_offset + c * dates.size();
                    if (requested) {
                        block.read_many(*requested, offset, result);
                    } else {
                        block.read_one(single_day->day, offset, result);
                    }
                }
            }
            continue;
        }

        for (std::size_t c = 0; c < contracts.size(); ++c) {
            const auto contract_id = contracts_by_position[c];
            if (!contract_id) {
                continue;
            }
            const auto offset = factor_offset + c * dates.size();
            if (single_day) {
                const auto block = blocks.find(block_key(*contract_id, single_day->year));
                if (block != blocks.end()) {
                    block->second.read_one(single_day->day, offset, result);
                }
                continue;
            }
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
