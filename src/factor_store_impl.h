#pragma once

#include "year_block.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace factor_store::detail {

class PromotionQueue {
public:
    void enqueue(YearBlock& block, Factor& owner);

    void cancel(YearBlock& block);

    void run_after_daily_write(std::size_t cells);

private:
    struct Task {
        YearBlock* block;
        Factor* factor;
    };

    void link(YearBlock& block) noexcept;

    std::optional<Task> take();

    void requeue(const Task& task);

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

}  // namespace factor_store::detail

namespace factor_store {

struct FactorStore::Impl {
    mutable std::shared_mutex catalog_mutex;

    // Destroy the factors and their links before destroying the queue sentinel.
    detail::PromotionQueue promotions;
    // The owner containers keep strings stable even when the index tables rehash.
    std::deque<std::string> contract_names;
    std::unordered_map<std::string_view, detail::ContractId> contract_ids;
    std::vector<std::unique_ptr<detail::Factor>> factors;
    std::unordered_map<std::string_view, detail::FactorId> factor_ids;

    template <class Lock>
    std::vector<Lock> lock_factors(std::vector<detail::FactorId> ids) const {
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

    detail::ContractId get_or_add_contract(std::string_view name);

    template <class Write>
    void write_factor(std::string_view name, Write write) {
        const auto existing = factor_ids.find(name);
        if (existing != factor_ids.end()) {
            write(*factors[existing->second]);
            return;
        }

        // Publish a new factor only after it has data.
        auto factor = std::make_unique<detail::Factor>(name);
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

}  // namespace factor_store
