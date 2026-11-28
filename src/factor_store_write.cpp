#include "factor_store_impl.h"

#include <bitset>
#include <limits>
#include <new>
#include <unordered_set>

namespace factor_store::detail {

namespace {

constexpr std::size_t max_promotions_per_daily_batch = 256;
constexpr std::size_t cells_per_promotion = 16;

}  // namespace

void PromotionQueue::enqueue(YearBlock& block, Factor& owner) {
    const std::lock_guard lock(mutex_);
    block.pending_owner_ = &owner;
    link(block);
}

void PromotionQueue::cancel(YearBlock& block) {
    const std::lock_guard lock(mutex_);
    block.unlink();
    block.pending_owner_ = nullptr;
}

void PromotionQueue::link(YearBlock& block) noexcept {
    block.previous = head_.previous;
    block.next = &head_;
    head_.previous->next = &block;
    head_.previous = &block;
}

std::optional<PromotionQueue::Task> PromotionQueue::take() {
    const std::lock_guard lock(mutex_);
    if (head_.next == &head_) {
        return std::nullopt;
    }
    auto& block = static_cast<YearBlock&>(*head_.next);
    block.unlink();
    return Task{&block, block.pending_owner_};
}

void PromotionQueue::requeue(const Task& task) {
    const std::lock_guard lock(mutex_);
    if (task.block->pending_owner_ == task.factor) {
        link(*task.block);
    }
}

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

}  // namespace factor_store::detail

namespace factor_store {

using detail::BlockKey;
using detail::ContractId;
using detail::Factor;
using detail::FactorId;
using detail::IndexedValue;
using detail::YearBlock;
using detail::block_key;
using detail::checked_product;
using detail::max_days_in_year;
using detail::parse_date;
using detail::validate_name;
using detail::validate_year;

namespace {

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

}  // namespace

detail::ContractId FactorStore::Impl::get_or_add_contract(std::string_view name) {
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

}  // namespace factor_store
