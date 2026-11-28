#include "factor_store/factor_store.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {

using factor_store::DailyValue;
using factor_store::FactorStore;
using factor_store::QueryResult;
using factor_store::TradingDate;
using ModelKey = std::tuple<std::string, std::string, TradingDate>;
using Model = std::map<ModelKey, double>;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Call>
void expect_invalid_argument(Call call) {
    try {
        call();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("expected std::invalid_argument");
}

void check_value(const QueryResult& result, std::size_t index, double expected) {
    check(result.has_value(index), "expected a present value");
    const double actual = result.values.at(index);
    check(std::isnan(expected) ? std::isnan(actual) : actual == expected,
          "stored value differs from expected");
}

void compare_query(const FactorStore& store, const Model& model,
                   const std::vector<std::string>& factors,
                   const std::vector<std::string>& contracts,
                   const std::vector<TradingDate>& dates) {
    const auto result = store.query(factors, contracts, dates);
    const auto count = factors.size() * contracts.size() * dates.size();
    check(result.factor_count == factors.size() &&
              result.contract_count == contracts.size() &&
              result.date_count == dates.size(),
          "incorrect query shape");
    check(result.values.size() == count && result.validity.size() == (count + 63) / 64,
          "incorrect query buffer lengths");

    std::size_t index = 0;
    for (const auto& factor : factors) {
        for (const auto& contract : contracts) {
            for (const auto date : dates) {
                const auto expected = model.find({factor, contract, date});
                if (expected == model.end()) {
                    check(!result.has_value(index), "missing cell was marked as present");
                } else {
                    check_value(result, index, expected->second);
                }
                ++index;
            }
        }
    }
    check(!result.has_value(count), "out-of-range value was marked as present");
}

void test_batches_and_query_order() {
    FactorStore store;
    check(store.factor_count() == 0, "new store is not empty");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    store.upsert_year("momentum", "IF", 2024, {{20241231, 9.0}, {20240229, 1.0}});
    store.upsert_day(20240229, {"momentum", "volatility"}, {"IF", "IC"},
                     {2.0, 3.0, nan, 0.0});
    store.upsert_year("momentum", "IF", 2024, {{20240301, 8.0}, {20240229, 4.0}});
    store.upsert_day(20250102, {"momentum"}, {"IF"}, {5.0});
    check(store.factor_count() == 2, "overwrites changed the factor count");

    const Model expected{
        {{"momentum", "IF", 20240229}, 4.0},
        {{"momentum", "IF", 20240301}, 8.0},
        {{"momentum", "IF", 20241231}, 9.0},
        {{"momentum", "IF", 20250102}, 5.0},
        {{"momentum", "IC", 20240229}, 3.0},
        {{"volatility", "IF", 20240229}, nan},
        {{"volatility", "IC", 20240229}, 0.0},
    };
    compare_query(store, expected, {"volatility", "momentum", "absent", "volatility"},
                  {"IC", "IF", "absent", "IC"},
                  {20250102, 20240229, 20241231, 20240229, 20240301, 20240228});
    compare_query(store, expected, {}, {"IF"}, {20240229});
    compare_query(store, expected, {"volatility"}, {}, {20240229});
    compare_query(store, expected, {"momentum"}, {"IF"}, {});
}

void test_empty_and_invalid_batches() {
    FactorStore store;
    store.upsert_year("unused", "IF", 2024, {});
    store.upsert_day(20240229, {"unused"}, {}, {});
    store.upsert_day(20240229, {}, {"IF"}, {});
    check(store.factor_count() == 0, "empty write created a factor");
    store.upsert_day(20240229, {"existing"}, {"IF"}, {1.0});

    expect_invalid_argument([&] {
        store.upsert_year("existing", "IF", 2024, {{20240229, 9.0}, {20250102, 2.0}});
    });
    expect_invalid_argument([&] {
        store.upsert_year("new", "IF", 2024, {{20240229, 9.0}, {20240229, 2.0}});
    });
    expect_invalid_argument([&] { store.upsert_year("new", "IF", 0, {}); });
    expect_invalid_argument([&] { store.upsert_year("", "IF", 2024, {}); });
    expect_invalid_argument([&] {
        store.upsert_day(20240229, {"existing", "new", "new"}, {"IF"}, {9.0, 2.0, 3.0});
    });
    expect_invalid_argument([&] {
        store.upsert_day(20240229, {"existing"}, {"IF", "IF"}, {9.0, 2.0});
    });
    expect_invalid_argument([&] {
        store.upsert_day(20240229, {"existing"}, {"IF", "IC"}, {9.0});
    });
    expect_invalid_argument([&] { store.upsert_day(20240229, {}, {"IF"}, {9.0}); });
    expect_invalid_argument([&] { store.upsert_day(20240229, {"existing"}, {""}, {9.0}); });
    expect_invalid_argument([&] { store.upsert_day(19000229, {}, {}, {}); });
    expect_invalid_argument([&] { store.upsert_day(20240431, {"new"}, {"IF"}, {9.0}); });
    expect_invalid_argument([&] { static_cast<void>(store.query({}, {"IF"}, {20241301})); });
    expect_invalid_argument([&] { static_cast<void>(store.query({"existing"}, {""}, {})); });
    expect_invalid_argument([&] { store.erase_factor(""); });

    check(store.factor_count() == 1, "invalid write changed the factor count");
    compare_query(store, {{{"existing", "IF", 20240229}, 1.0}},
                  {"new", "existing"}, {"IF", "IC"}, {20240229, 20250102});
}

std::vector<TradingDate> leap_year_dates() {
    std::vector<TradingDate> dates;
    const std::array<int, 12> month_lengths{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (std::size_t month = 0; month < month_lengths.size(); ++month) {
        for (int day = 1; day <= month_lengths[month]; ++day) {
            dates.push_back(20000000 + (static_cast<int>(month) + 1) * 100 + day);
        }
    }
    return dates;
}

void test_single_value_block_expands_in_date_order() {
    FactorStore store;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    store.upsert_year("annual", "IF", 2024, {{20240103, nan}});
    store.upsert_day(20240101, {"daily"}, {"IF"}, {0.0});
    const std::vector<std::string> factors{"annual", "daily"};
    const std::vector<TradingDate> dates{20240103, 20240101, 20240102, 20240103};
    Model expected{{{"annual", "IF", 20240103}, nan}, {{"daily", "IF", 20240101}, 0.0}};
    compare_query(store, expected, factors, {"IF"}, dates);

    store.upsert_day(20240101, factors, {"IF"}, {9.0, 2.0});
    store.upsert_year("daily", "IF", 2024, {{20240103, nan}});
    expected[{"annual", "IF", 20240101}] = 9.0;
    expected[{"daily", "IF", 20240101}] = 2.0;
    expected[{"daily", "IF", 20240103}] = nan;
    compare_query(store, expected, factors, {"IF"}, dates);
}

void test_full_leap_year() {
    FactorStore store;
    auto dates = leap_year_dates();
    Model expected;
    std::vector<DailyValue> annual;
    for (std::size_t i = 0; i < dates.size(); ++i) {
        if (i == 50) {
            continue;
        }
        const double value = i == 51 ? std::numeric_limits<double>::quiet_NaN()
                                      : static_cast<double>(i);
        store.upsert_day(dates[i], {"factor"}, {"contract"}, {value});
        expected[{"factor", "contract", dates[i]}] = value;
        annual.push_back({dates[i], value});
        expected[{"annual", "contract", dates[i]}] = value;
        expected[{"merged", "contract", dates[i]}] = value;
    }
    store.upsert_year("annual", "contract", 2000, annual);
    const auto middle = annual.begin() + 180;
    store.upsert_year("merged", "contract", 2000, std::vector<DailyValue>(annual.begin(), middle));
    store.upsert_year("merged", "contract", 2000, std::vector<DailyValue>(middle, annual.end()));
    store.upsert_year("factor", "contract", 2000, {{20001231, 42.0}, {20000101, 43.0}});
    expected[{"factor", "contract", 20001231}] = 42.0;
    expected[{"factor", "contract", 20000101}] = 43.0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    store.upsert_year("sparse", "contract", 2000, {{20000101, 0.0}, {20000229, nan}});
    expected[{"sparse", "contract", 20000101}] = 0.0;
    expected[{"sparse", "contract", 20000229}] = nan;
    std::reverse(dates.begin(), dates.end());
    dates.push_back(20010101);
    dates.push_back(20000229);
    dates.push_back(20000101);
    compare_query(store, expected, {"sparse", "factor", "annual", "merged"}, {"contract"}, dates);
}

void test_query_bitmap_sharing_and_single_dates() {
    FactorStore store;
    const auto dates = leap_year_dates();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Model expected;
    for (const int year : {2000, 2004}) {
        std::vector<DailyValue> points;
        for (std::size_t i = 0; i < dates.size(); ++i) {
            if ((i >= 200 && i < 340) || i == 63 || i == 64 || i == 127 || i == 365) {
                continue;
            }
            const auto date = dates[i] + (year - 2000) * 10000;
            const double value = i == 59 ? nan : static_cast<double>(i + year - 2000);
            points.push_back({date, value});
            expected[{"dense", "IF", date}] = value;
        }
        store.upsert_year("dense", "IF", year, points);
    }
    store.upsert_year("sparse", "IF", 2000, {{20000103, 1.0}, {20000229, 2.0}});
    store.upsert_day(20000101, {"single"}, {"IF"}, {3.0});
    expected[{"sparse", "IF", 20000103}] = 1.0;
    expected[{"sparse", "IF", 20000229}] = 2.0;
    expected[{"single", "IF", 20000101}] = 3.0;

    const std::array<TradingDate, 8> samples{
        20040229, 20000103, 20001231, 20000229, 20040103, 20000101, 20041231, 20000103};
    const std::vector<std::string> factors{"sparse", "dense", "single", "dense", "absent"};
    const std::vector<std::string> contracts{"IF", "IF", "absent"};
    for (const auto count : {32U, 64U, 65U, 130U}) {
        std::vector<TradingDate> requested;
        for (std::size_t i = 0; i < count; ++i) {
            requested.push_back(samples[i % samples.size()]);
        }
        compare_query(store, expected, factors, contracts, requested);
    }
    for (const auto day : samples) {
        compare_query(store, expected, factors, contracts, {day});
    }

    // Mix long, unaligned runs with repeated point requests and two years.
    std::vector<TradingDate> runs{dates.back(), 20040229, dates[4]};
    runs.insert(runs.end(), dates.begin() + 13, dates.begin() + 143);
    for (std::size_t i = 60; i < 141; ++i) {
        runs.push_back(dates[i] + 40000);
    }
    runs.push_back(dates[59]);
    runs.insert(runs.end(), dates.begin() + 340, dates.end());
    compare_query(store, expected, factors, contracts, runs);
}

void test_sparse_slice_queries() {
    FactorStore store;
    const auto calendar = leap_year_dates();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<std::string> factors{"sparse"};
    const std::vector<std::string> contracts{"IF", "IF", "absent"};
    std::vector<TradingDate> stored;
    Model expected;
    for (int year : {2000, 2004}) {
        std::vector<DailyValue> points;
        for (std::size_t i = 0; i < 75; ++i) {
            const auto date = calendar[2 * i] + (year - 2000) * 10000;
            const double value = i == 7 ? nan : (i == 3 ? 0.0 : static_cast<double>(i + year));
            points.push_back({date, value});
            expected[{"sparse", "IF", date}] = value;
            if (year == 2000) {
                stored.push_back(date);
            }
        }
        store.upsert_year("sparse", "IF", year, points);
    }
    compare_query(store, expected, factors, contracts, stored);

    // The leading date and neighboring contract rows shift the output bitmap boundaries.
    std::vector<TradingDate> requested{19991231};
    requested.insert(requested.end(), stored.begin() + 3, stored.begin() + 68);
    for (std::size_t i = 20; i < 36; ++i) {
        requested.push_back(stored[i] + 40000);
    }
    requested.push_back(20050101);
    compare_query(store, expected, factors, contracts, requested);

    // A mismatch inside an otherwise ordered slice must preserve all the other hits.
    auto gap = requested;
    gap[9] = calendar[23];
    compare_query(store, expected, factors, contracts, gap);
    auto repeated = requested;
    repeated[2] = repeated[1];
    compare_query(store, expected, factors, contracts, repeated);
    std::swap(requested[1], requested[9]);
    compare_query(store, expected, factors, contracts, requested);
    compare_query(store, expected, factors, contracts,
                  std::vector<TradingDate>(calendar.begin() + 200, calendar.begin() + 216));
}

void test_query_with_mixed_block_coverage() {
    FactorStore store;
    std::vector<std::string> contracts;
    for (int i = 0; i < 96; ++i) {
        contracts.push_back("contract_" + std::to_string(i));
    }

    constexpr TradingDate recent = 20240229;
    constexpr TradingDate earlier = 20230102;
    const std::vector<std::string> covered{contracts.front(), contracts[1], "outside"};
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::string> factors;
    Model expected;
    for (int i = 0; i < 8; ++i) {
        const auto factor = "sparse_" + std::to_string(i);
        const auto value = static_cast<double>(i);
        store.upsert_day(recent, {factor}, covered, {value, nan, 100.0});
        store.upsert_year(factor, covered.front(), 2023, {{earlier, -value}});
        store.upsert_year(factor, covered.front(), 2022, {{20220102, 20.0}});
        expected[{factor, covered.front(), recent}] = value;
        expected[{factor, covered[1], recent}] = nan;
        expected[{factor, covered.front(), earlier}] = -value;
        factors.push_back(factor);
    }

    store.upsert_day(recent, {"wide"}, contracts, std::vector<double>(contracts.size(), 5.0));
    for (const auto& contract : contracts) {
        expected[{"wide", contract, recent}] = 5.0;
    }
    factors.insert(factors.begin(), "wide");
    factors.push_back("absent");
    factors.push_back("sparse_0");
    std::reverse(contracts.begin(), contracts.end());
    contracts.push_back(covered.front());
    contracts.push_back("absent");
    contracts.push_back(covered[1]);
    compare_query(store, expected, factors, contracts, {recent});
    compare_query(store, expected, factors, contracts,
                  {recent, earlier, recent, 20240101, 20200301, earlier});
}

void test_pending_promotions_survive_writes_and_factor_deletion() {
    FactorStore store;
    const auto dates = leap_year_dates();
    std::vector<std::string> contracts;
    for (int i = 0; i < 40; ++i) {
        contracts.push_back("contract_" + std::to_string(i));
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<std::string> factors{"removed", "kept"};
    std::vector<double> values(factors.size() * contracts.size(), 1.0);
    values[contracts.size()] = nan;
    values.back() = 0.0;
    for (std::size_t i = 0; i < 192; ++i) {
        store.upsert_day(dates[i], factors, contracts, values);
    }

    const std::vector<std::string> selected{contracts.front(), contracts.back()};
    const std::vector<TradingDate> probes{dates[191], dates[192], dates[0]};
    Model expected{{{"kept", contracts.front(), dates[191]}, nan},
                   {{"kept", contracts.front(), dates[0]}, nan},
                   {{"kept", contracts.back(), dates[191]}, 0.0},
                   {{"kept", contracts.back(), dates[0]}, 0.0}};
    compare_query(store, expected, {"kept"}, selected, probes);

    check(store.erase_factor("removed"), "failed to delete a factor with queued blocks");
    store.upsert_year("kept", contracts.back(), 2000, {{dates[0], 42.0}, {dates[192], 43.0}});
    store.upsert_day(dates[192], {"removed"}, {contracts.front()}, {99.0});
    for (std::size_t i = 0; i < contracts.size(); ++i) {
        store.upsert_day(dates[i], {"unrelated"}, {"other_contract"}, {1.0});
    }
    expected[{"kept", contracts.back(), dates[0]}] = 42.0;
    expected[{"kept", contracts.back(), dates[192]}] = 43.0;
    expected[{"removed", contracts.front(), dates[192]}] = 99.0;
    compare_query(store, expected, {"removed", "kept"}, selected, probes);
    check(store.factor_count() == 3, "unexpected factor count after recreation");
}

void test_delete_and_name_ownership() {
    FactorStore store;
    std::string factor = "erased";
    std::string contract = "short";
    store.upsert_year(factor, contract, 2024, {{20240229, 1.0}});
    factor = "rewritten";
    contract.clear();

    Model expected;
    for (int i = 0; i < 128; ++i) {
        const auto name = "f" + std::to_string(i);
        const auto code = "c" + std::to_string(i);
        store.upsert_year(name, code, 2025, {{20250102, static_cast<double>(i)}});
        expected[{name, code, 20250102}] = static_cast<double>(i);
    }
    check(store.factor_count() == 129, "stored names did not survive caller mutation");
    check(store.erase_factor("erased"), "existing factor was not deleted");
    check(!store.erase_factor("erased"), "deleted factor was found a second time");
    check(store.factor_count() == 128, "deletion did not change the factor count");
    store.upsert_day(20250102, {"erased"}, {"short"}, {4.0});
    expected[{"erased", "short", 20250102}] = 4.0;
    check(store.factor_count() == 129, "same-name recreation did not create a factor");
    compare_query(store, expected, {"erased", "f127", "f0"}, {"short", "c127", "c0"},
                  {20240229, 20250102});
}

void test_randomized_reference_model() {
    FactorStore store;
    Model model;
    std::mt19937 random(20261125);
    const std::vector<std::string> factors{"A", "B", "C", "D"};
    const std::vector<std::string> contracts{"IF", "IC", "IH"};
    const std::array<std::vector<TradingDate>, 2> dates{{
        {20240228, 20240229, 20240301, 20241231},
        {20250102, 20250103, 20250228, 20250301},
    }};

    for (int step = 0; step < 500; ++step) {
        const auto& factor = factors[random() % factors.size()];
        const auto& contract = contracts[random() % contracts.size()];
        const auto& year_dates = dates[random() % dates.size()];
        const auto date = year_dates[random() % year_dates.size()];
        const double value = static_cast<double>(step);
        switch (random() % 3) {
        case 0: {
            const std::vector<DailyValue> points{{year_dates.back(), value}, {year_dates.front(), -value}};
            store.upsert_year(factor, contract, date / 10000, points);
            for (const auto& point : points) {
                model[{factor, contract, point.date}] = point.value;
            }
            break;
        }
        case 1:
            store.upsert_day(date, {factor}, contracts, {value, value + 1.0, value + 2.0});
            for (std::size_t c = 0; c < contracts.size(); ++c) {
                model[{factor, contracts[c], date}] = value + static_cast<double>(c);
            }
            break;
        default: {
            bool found = false;
            for (auto it = model.begin(); it != model.end();) {
                if (std::get<0>(it->first) == factor) {
                    it = model.erase(it);
                    found = true;
                } else {
                    ++it;
                }
            }
            check(store.erase_factor(factor) == found, "deletion differs from reference");
            break;
        }
        }

        std::set<std::string> active_factors;
        for (const auto& item : model) {
            active_factors.insert(std::get<0>(item.first));
        }
        check(store.factor_count() == active_factors.size(), "count differs from reference");
        compare_query(store, model, {factor, "B", "A", factor, "D", "C"},
                      {contract, "IH", "IF", "IC", contract},
                      {date, 20250301, 20250228, 20250103, 20250102, 20241231,
                       20240301, 20240229, 20240228});
    }
}

}  // namespace

int main() {
    try {
        test_batches_and_query_order();
        test_empty_and_invalid_batches();
        test_single_value_block_expands_in_date_order();
        test_full_leap_year();
        test_query_bitmap_sharing_and_single_dates();
        test_sparse_slice_queries();
        test_query_with_mixed_block_coverage();
        test_pending_promotions_survive_writes_and_factor_deletion();
        test_delete_and_name_ownership();
        test_randomized_reference_model();
    } catch (const std::exception& error) {
        std::cerr << "FactorStore test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
