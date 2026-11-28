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

void test_query_observes_one_complete_batch() {
    FactorStore store;
    const std::vector<std::string> factors{"first", "second", "third"};
    const auto codes = contracts();
    const std::vector<TradingDate> dates{date};
    const auto cells = factors.size() * codes.size();
    std::vector<double> updates(cells, 0.0);
    store.upsert_day(date, factors, codes, updates);

    run_with_two_readers(
        [&] {
            for (int i = 0; i < iterations; ++i) {
                const auto result = store.query(factors, codes, dates);
                check(result.values.size() == cells, "wrong query size");
                const auto value = result.values.front();
                for (std::size_t j = 0; j < cells; ++j) {
                    check(result.has_value(j) && result.values[j] == value,
                          "query observed a partially applied batch");
                }
                check(store.factor_count() == factors.size(), "factor count changed");
            }
        },
        [&] {
            for (int i = 1; i <= iterations; ++i) {
                std::fill(updates.begin(), updates.end(), static_cast<double>(i));
                store.upsert_day(date, factors, codes, updates);
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

}  // namespace

int main() {
    try {
        test_query_observes_one_complete_batch();
        test_query_during_delete_and_recreate();
        test_annual_and_daily_writers_share_a_block();
    } catch (const std::exception& error) {
        std::cerr << "Concurrent FactorStore test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
