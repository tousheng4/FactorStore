#include "factor_store/factor_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using factor_store::FactorStore;
using factor_store::TradingDate;

constexpr TradingDate date = 20240229;
constexpr int iterations = 500;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::string> contracts() {
    std::vector<std::string> result;
    for (int i = 0; i < 32; ++i) {
        result.push_back("C" + std::to_string(i));
    }
    return result;
}

template <typename Read, typename Write>
void run_with_two_readers(Read read, Write write) {
    std::atomic<std::size_t> ready{0};
    std::array<std::future<void>, 2> readers;
    std::promise<void> start;
    const auto gate = start.get_future().share();
    for (auto& reader : readers) {
        reader = std::async(std::launch::async, [&, gate] {
            ++ready;
            gate.get();
            read();
        });
    }
    while (ready.load() != readers.size()) {
        std::this_thread::yield();
    }
    start.set_value();
    write();
    for (auto& reader : readers) {
        reader.get();
    }
}

void test_query_observes_one_complete_batch(bool low_coverage) {
    FactorStore store;
    const std::vector<std::string> factors{
        "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth"};
    const std::vector<std::string> write_factors(factors.rbegin(), factors.rend());
    auto read_factors = write_factors;
    read_factors.push_back(factors.back());
    const auto codes = contracts();
    auto read_codes = codes;
    if (low_coverage) {
        std::vector<std::string> extra;
        for (int i = 0; i < 224; ++i) {
            extra.push_back("unused_" + std::to_string(i));
        }
        store.upsert_day(date, {"register"}, extra, std::vector<double>(extra.size(), 0.0));
        check(store.erase_factor("register"), "missing registration factor");
        read_codes.insert(read_codes.end(), extra.begin(), extra.end());
    }
    const std::vector<TradingDate> dates{date};
    const auto cells = factors.size() * codes.size();
    std::vector<double> updates(cells, 0.0);
    store.upsert_day(date, factors, codes, updates);

    run_with_two_readers(
        [&] {
            for (int i = 0; i < iterations; ++i) {
                const auto result = store.query(read_factors, read_codes, dates);
                const auto read_cells = read_factors.size() * read_codes.size();
                check(result.values.size() == read_cells, "wrong query size");
                const auto value = result.values.front();
                for (std::size_t j = 0; j < read_cells; ++j) {
                    const bool present = j % read_codes.size() < codes.size();
                    check(result.has_value(j) == present &&
                              (!present || result.values[j] == value),
                          "query observed a partially applied batch or incorrect coverage");
                }
                check(store.factor_count() == factors.size(), "factor count changed");
            }
        },
        [&] {
            for (int i = 1; i <= iterations; ++i) {
                std::fill(updates.begin(), updates.end(), static_cast<double>(i));
                store.upsert_day(date, i % 2 == 0 ? factors : write_factors, codes, updates);
            }
        });

    const auto result = store.query(factors, codes, dates);
    check(result.has_value(0) && result.values[0] == iterations, "last batch value was lost");
}

void test_query_during_delete_and_recreate() {
    FactorStore store;
    const auto codes = contracts();
    const std::vector<std::string> changing{"changing"};
    const std::vector<std::string> factors{"changing", "stable"};
    const std::vector<TradingDate> dates{date};
    const double stable_value = -1.0;
    std::vector<double> updates(codes.size(), 0.0);
    store.upsert_day(date, changing, codes, updates);
    store.upsert_day(date, {"stable"}, codes, std::vector<double>(codes.size(), stable_value));

    run_with_two_readers(
        [&] {
            for (int i = 0; i < iterations; ++i) {
                const auto result = store.query(factors, codes, dates);
                const bool present = result.has_value(0);
                for (std::size_t j = 0; j < codes.size(); ++j) {
                    check(result.has_value(j) == present, "query observed a partial recreation");
                    if (present) {
                        check(result.values[j] == result.values[0], "factor values have mixed generations");
                    }
                    const auto stable = codes.size() + j;
                    check(result.has_value(stable) && result.values[stable] == stable_value,
                          "unrelated factor changed during deletion");
                }
                const auto count = store.factor_count();
                check(count == 1 || count == 2, "factor directory is inconsistent");
            }
        },
        [&] {
            for (int i = 1; i <= iterations; ++i) {
                check(store.erase_factor(changing.front()), "expected factor to exist");
                std::fill(updates.begin(), updates.end(), static_cast<double>(i));
                store.upsert_day(date, changing, codes, updates);
                store.upsert_year("stable", codes.front(), 2024, {{20240228, updates.front()}});
            }
        });

    check(store.factor_count() == 2, "recreated factor is missing");
}

void test_annual_and_daily_writers_share_a_block() {
    FactorStore store;
    const std::vector<std::string> factors{"factor"};
    const std::vector<std::string> codes{"IF"};
    constexpr TradingDate earlier_date = 20240228;
    std::promise<void> start;
    const auto gate = start.get_future().share();
    auto annual = std::async(std::launch::async, [&, gate] {
        gate.get();
        for (int i = 1; i <= iterations; ++i) {
            store.upsert_year(factors.front(), codes.front(), 2024,
                              {{earlier_date, static_cast<double>(i)}});
        }
    });
    start.set_value();
    for (int i = 1; i <= iterations; ++i) {
        store.upsert_day(date, factors, codes, {-static_cast<double>(i)});
    }
    annual.get();

    const auto result = store.query(factors, codes, {earlier_date, date});
    check(result.has_value(0) && result.values[0] == iterations &&
              result.has_value(1) && result.values[1] == -iterations,
          "concurrent writers lost an update");
    check(store.factor_count() == 1, "concurrent writers created duplicate factors");
}

void test_promotion_during_writes_to_other_factors() {
    FactorStore store;
    const auto codes = contracts();
    const std::vector<std::string> pending{"pending"};
    const std::vector<std::string> active{"active"};
    const std::array<int, 7> days_per_month{31, 29, 31, 30, 31, 30, 10};
    constexpr TradingDate last_day = 20240710;
    const std::vector<double> original(codes.size(), 1.0);
    std::vector<TradingDate> requested{date};
    for (std::size_t month = 0; month < days_per_month.size(); ++month) {
        for (int day = 1; day <= days_per_month[month]; ++day) {
            const auto trading_date = 20240000 + static_cast<int>(month + 1) * 100 + day;
            store.upsert_day(trading_date, pending, codes, original);
            if (trading_date >= 20240624) {
                requested.push_back(trading_date);
            }
        }
    }
    store.upsert_day(date, active, codes, original);

    std::promise<void> start;
    const auto gate = start.get_future().share();
    auto annual = std::async(std::launch::async, [&, gate] {
        gate.get();
        for (auto code = codes.rbegin(); code != codes.rend(); ++code) {
            store.upsert_year(pending.front(), *code, 2024, {{last_day, 2.0}});
        }
    });
    start.set_value();
    for (int i = 0; i < iterations; ++i) {
        store.upsert_day(date, active, codes, original);
        const auto result = store.query(pending, codes, requested);
        for (std::size_t c = 0; c < codes.size(); ++c) {
            const auto first = c * requested.size();
            const auto last = first + requested.size() - 1;
            for (auto index = first; index < last; ++index) {
                check(result.has_value(index) && result.values[index] == 1.0,
                      "queued promotion lost an unchanged value");
            }
            check(result.has_value(last) && (result.values[last] == 1.0 || result.values[last] == 2.0),
                  "queued promotion lost an overwritten value");
        }
    }
    annual.get();

    const auto result = store.query(pending, codes, {last_day});
    for (std::size_t c = 0; c < codes.size(); ++c) {
        check(result.has_value(c) && result.values[c] == 2.0, "annual overwrite was lost");
    }
}

}  // namespace

int main() {
    try {
        test_query_observes_one_complete_batch(false);
        test_query_observes_one_complete_batch(true);
        test_query_during_delete_and_recreate();
        test_annual_and_daily_writers_share_a_block();
        test_promotion_during_writes_to_other_factors();
    } catch (const std::exception& error) {
        std::cerr << "Concurrent FactorStore test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
