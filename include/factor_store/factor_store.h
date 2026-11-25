#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace factor_store {

// A Gregorian date encoded as YYYYMMDD. Trading calendars are caller-defined.
using TradingDate = std::int32_t;

struct DailyValue {
    TradingDate date;
    double value;
};

struct QueryResult {
    std::size_t factor_count = 0;
    std::size_t contract_count = 0;
    std::size_t date_count = 0;

    // Indexed as [factor][contract][date], with the date axis contiguous.
    std::vector<double> values;

    // Bit i % 64 in word i / 64 marks whether values[i] is present.
    std::vector<std::uint64_t> validity;

    [[nodiscard]] bool has_value(std::size_t index) const noexcept {
        const std::size_t word = index / 64;
        if (index >= values.size() || word >= validity.size()) {
            return false;
        }
        return (validity[word] & (std::uint64_t{1} << (index % 64))) != 0;
    }
};

class FactorStore final {
public:
    FactorStore();
    ~FactorStore();

    FactorStore(const FactorStore&) = delete;
    FactorStore& operator=(const FactorStore&) = delete;

    // Inserts or overwrites only the supplied dates in the specified year.
    void upsert_year(std::string_view factor,
                     std::string_view contract,
                     int year,
                     const std::vector<DailyValue>& points);

    // Values are indexed as [factor][contract], with contracts contiguous.
    void upsert_day(TradingDate date,
                    const std::vector<std::string>& factors,
                    const std::vector<std::string>& contracts,
                    const std::vector<double>& values);

    [[nodiscard]] std::size_t factor_count() const;

    // Returns true if the named factor and all of its data were removed.
    bool erase_factor(std::string_view factor);

    // Preserves input order and repetitions on all three axes.
    [[nodiscard]] QueryResult query(const std::vector<std::string>& factors,
                                    const std::vector<std::string>& contracts,
                                    const std::vector<TradingDate>& dates) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace factor_store

