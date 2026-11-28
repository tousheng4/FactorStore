#pragma once

#include "factor_store/factor_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace factor_store::detail {

using FactorId = std::size_t;
using ContractId = std::uint32_t;
using DayIndex = std::uint16_t;
using BlockKey = std::uint64_t;

inline constexpr std::size_t max_days_in_year = 366;
inline constexpr DayIndex empty_day = static_cast<DayIndex>(max_days_in_year);

struct IndexedDate {
    int year;
    DayIndex day;
};

inline void validate_year(int year) {
    if (year < 1 || year > 9999) {
        throw std::invalid_argument("year must be between 1 and 9999");
    }
}

inline IndexedDate parse_date(TradingDate date) {
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

inline void validate_name(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("factor and contract names must not be empty");
    }
}

inline std::size_t checked_product(std::size_t a, std::size_t b) {
    if (b != 0 && a > std::numeric_limits<std::size_t>::max() / b) {
        throw std::length_error("factor store matrix dimensions overflow");
    }
    return a * b;
}

inline BlockKey block_key(ContractId contract, int year) {
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

}  // namespace factor_store::detail
