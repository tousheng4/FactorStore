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

void test_full_leap_year() {
    FactorStore store;
    std::vector<TradingDate> dates;
    const std::array<int, 12> month_lengths{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (std::size_t month = 0; month < month_lengths.size(); ++month) {
        for (int day = 1; day <= month_lengths[month]; ++day) {
            dates.push_back(20000000 + (static_cast<int>(month) + 1) * 100 + day);
        }
    }
    Model expected;
    for (std::size_t i = 0; i < dates.size(); ++i) {
        const double value = static_cast<double>(i);
        store.upsert_day(dates[i], {"factor"}, {"contract"}, {value});
        expected[{"factor", "contract", dates[i]}] = value;
    }
    store.upsert_year("factor", "contract", 2000, {{20001231, 42.0}, {20000101, 43.0}});
    expected[{"factor", "contract", 20001231}] = 42.0;
    expected[{"factor", "contract", 20000101}] = 43.0;
    std::reverse(dates.begin(), dates.end());
    dates.push_back(20010101);
    compare_query(store, expected, {"factor"}, {"contract"}, dates);
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
        test_full_leap_year();
        test_delete_and_name_ownership();
        test_randomized_reference_model();
    } catch (const std::exception& error) {
        std::cerr << "FactorStore test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
