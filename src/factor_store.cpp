#include "factor_store_impl.h"

namespace factor_store {

FactorStore::FactorStore() : impl_(std::make_unique<Impl>()) {}

FactorStore::~FactorStore() = default;

std::size_t FactorStore::factor_count() const {
    const std::shared_lock lock(impl_->catalog_mutex);
    return impl_->factors.size();
}

bool FactorStore::erase_factor(std::string_view factor) {
    detail::validate_name(factor);
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

}  // namespace factor_store
