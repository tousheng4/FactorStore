#include "factor_store/factor_store.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <locale>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/resource.h>
#endif

namespace {

using factor_store::DailyValue;
using factor_store::FactorStore;
using factor_store::TradingDate;
using Clock = std::chrono::steady_clock;

constexpr unsigned seed = 20261127;
constexpr std::size_t days_per_year = 252;

enum class Shape { annual, daily, history_query, day_query, mixed_query, promotion_query };

struct Scenario {
    std::string_view name;
    Shape shape;
    std::size_t stored_days;
    std::size_t missing_dates_per_year = 0;
    bool drain_promotions = false;
};

constexpr std::array<Scenario, 15> scenarios{{
    {"year-new", Shape::annual, 0},
    {"year-overwrite", Shape::annual, days_per_year},
    {"year-backfill", Shape::annual, 189},
    {"day-new", Shape::daily, 0},
    {"day-second", Shape::daily, 1},
    {"day-append", Shape::daily, 64},
    {"day-overwrite", Shape::daily, 64},
    {"day-threshold", Shape::daily, 191},
    {"query-history", Shape::history_query, days_per_year},
    {"query-day", Shape::day_query, 1},
    {"query-mixed-sparse", Shape::mixed_query, 32, 16},
    {"query-mixed-dense", Shape::mixed_query, 224, 16},
    {"query-mixed-dense-25", Shape::mixed_query, 224, 8},
    {"query-promotion-pending", Shape::promotion_query, 192, 16},
    {"query-promotion-drained", Shape::promotion_query, 192, 16, true},
}};

struct Options {
    std::string_view selected_case = "all";
    std::string_view profile = "small";
    std::size_t samples = 31;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (i + 1 == argc) {
            throw std::invalid_argument("missing value for " + std::string(argument));
        }
        const std::string_view value(argv[++i]);
        if (argument == "--case") {
            const auto known = std::any_of(scenarios.begin(), scenarios.end(),
                                           [&](const Scenario& s) { return s.name == value; });
            if (!known && value != "all") {
                throw std::invalid_argument("unknown case: " + std::string(value));
            }
            options.selected_case = value;
        } else if (argument == "--profile") {
            if (value != "small" && value != "large") {
                throw std::invalid_argument("profile must be small or large");
            }
            options.profile = value;
        } else if (argument == "--samples") {
            const auto* end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, options.samples);
            if (parsed.ec != std::errc{} || parsed.ptr != end ||
                options.samples < 3 || options.samples > 1000) {
                throw std::invalid_argument("samples must be an integer from 3 to 1000");
            }
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

std::vector<TradingDate> weekdays(int year, int weekday) {
    // The fixtures only use 2024 and 2025. Monday is zero.
    const std::array<int, 12> month_lengths{
        31, year == 2024 ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    std::vector<TradingDate> dates;
    dates.reserve(days_per_year);
    for (std::size_t month = 0; month < month_lengths.size(); ++month) {
        for (int day = 1; day <= month_lengths[month]; ++day) {
            if (weekday < 5) {
                dates.push_back(year * 10000 + (static_cast<int>(month) + 1) * 100 + day);
                if (dates.size() == days_per_year) {
                    return dates;
                }
            }
            weekday = (weekday + 1) % 7;
        }
    }
    throw std::runtime_error("calendar fixture has too few dates");
}

std::vector<std::string> names(std::string_view prefix, std::size_t count) {
    std::vector<std::string> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(std::string(prefix) + std::to_string(i));
    }
    return result;
}

std::vector<double> generate_values(std::size_t count) {
    std::mt19937 random(seed);
    std::vector<double> result(count);
    for (auto& value : result) {
        value = static_cast<double>(random()) / 4294967296.0;
    }
    return result;
}

template <typename T>
void permute(std::vector<T>& values) {
    std::mt19937 random(seed);
    for (auto remaining = values.size(); remaining > 1; --remaining) {
        std::swap(values[remaining - 1], values[random() % remaining]);
    }
}

void verify(bool condition) {
    if (!condition) {
        throw std::runtime_error("unexpected value or validity in benchmark fixture");
    }
}

struct Measurement {
    std::vector<double> elapsed_us;
    std::size_t cells;
    std::string missing_percent;
};

template <typename Sample>
std::vector<double> measure(std::size_t samples, Sample sample) {
    sample(true);  // Warmup is excluded from timing statistics.
    std::vector<double> elapsed;
    elapsed.reserve(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        elapsed.push_back(sample(false));
    }
    std::sort(elapsed.begin(), elapsed.end());
    return elapsed;
}

double microseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

Measurement annual(const Scenario& scenario, const Options& options) {
    const auto dates = weekdays(2024, 0);
    const auto values = generate_values(dates.size());
    const bool backfill = scenario.stored_days == 189;
    std::vector<DailyValue> initial;
    std::vector<DailyValue> batch;
    for (std::size_t i = 0; i < dates.size(); ++i) {
        const bool in_batch = !backfill || i % 4 == 0;
        if (in_batch) {
            batch.push_back({dates[i], values[i]});
        }
        if (scenario.stored_days != 0 && (!backfill || !in_batch)) {
            initial.push_back({dates[i], -1.0});
        }
    }
    if (backfill) {
        permute(batch);
    }
    const std::vector<std::string> factors{"factor_0"};
    const std::vector<std::string> contracts{"contract_0"};
    const std::vector<TradingDate> probes{batch.front().date, batch.back().date};

    auto elapsed = measure(options.samples, [&](bool) {
        FactorStore store;
        if (!initial.empty()) {
            store.upsert_year(factors.front(), contracts.front(), 2024, initial);
        }
        const auto begin = Clock::now();
        store.upsert_year(factors.front(), contracts.front(), 2024, batch);
        const auto us = microseconds_since(begin);
        const auto check = store.query(factors, contracts, probes);
        verify(check.has_value(0) && check.values[0] == batch.front().value &&
               check.has_value(1) && check.values[1] == batch.back().value);
        return us;
    });
    return {std::move(elapsed), batch.size(), "-"};
}

Measurement daily(const Scenario& scenario, const Options& options,
                  const std::vector<std::string>& factors,
                  const std::vector<std::string>& contracts) {
    const auto dates = weekdays(2024, 0);
    const auto values = generate_values(factors.size() * contracts.size());
    const std::vector<double> initial(values.size(), -1.0);
    const auto date = dates[scenario.name == "day-overwrite" ? 0 : scenario.stored_days];
    const std::vector<std::string> last_factor{factors.back()};
    const std::vector<std::string> last_contract{contracts.back()};
    const std::vector<TradingDate> probe{date};

    auto elapsed = measure(options.samples, [&](bool) {
        FactorStore store;
        for (std::size_t i = 0; i < scenario.stored_days; ++i) {
            store.upsert_day(dates[i], factors, contracts, initial);
        }
        const auto begin = Clock::now();
        store.upsert_day(date, factors, contracts, values);
        const auto us = microseconds_since(begin);
        const auto check = store.query(last_factor, last_contract, probe);
        verify(check.has_value(0) && check.values[0] == values.back());
        return us;
    });
    return {std::move(elapsed), values.size(), "-"};
}

struct RequestedDate {
    TradingDate date;
    bool present;
};

double date_offset(TradingDate date) {
    return static_cast<double>(date - 20240000);
}

void build_query_year(const Scenario& scenario, const std::vector<TradingDate>& year,
                      const std::vector<std::string>& factors,
                      const std::vector<std::string>& contracts,
                      const std::vector<double>& values, FactorStore& store,
                      std::vector<RequestedDate>& requested) {
    std::vector<double> day_values(values.size());
    const auto load = [&](TradingDate date) {
        const auto offset = date_offset(date);
        std::transform(values.begin(), values.end(), day_values.begin(),
                       [offset](double value) { return value + offset; });
        store.upsert_day(date, factors, contracts, day_values);
    };
    if (scenario.missing_dates_per_year == 0) {
        for (std::size_t i = 0; i < scenario.stored_days; ++i) {
            load(year[i]);
            requested.push_back({year[i], true});
        }
        return;
    }

    std::vector<TradingDate> available;
    std::vector<TradingDate> absent;
    for (std::size_t i = 0; i < year.size(); ++i) {
        (i % 9 == 8 ? absent : available).push_back(year[i]);
    }
    constexpr std::size_t request_count = 32;
    const auto hits = request_count - scenario.missing_dates_per_year;
    const bool sparsely_loaded = scenario.stored_days == request_count;
    const auto loaded_range = sparsely_loaded ? available.size() : scenario.stored_days;
    for (std::size_t i = 0; i < request_count; ++i) {
        const auto date = available[i * loaded_range / request_count];
        if (i < hits) {
            requested.push_back({date, true});
        }
        if (sparsely_loaded) {
            load(date);
        }
    }
    for (std::size_t i = 0; i < scenario.missing_dates_per_year; ++i) {
        requested.push_back({absent[i * absent.size() / scenario.missing_dates_per_year], false});
    }
    if (!sparsely_loaded) {
        for (std::size_t i = 0; i < loaded_range; ++i) {
            load(available[i]);
        }
    }
    if (scenario.drain_promotions) {
        // These batches overwrite the same values, giving pending promotions time to finish.
        for (int i = 0; i < 32; ++i) {
            load(available.front());
        }
    }
}

Measurement query(const Scenario& scenario, const Options& options,
                  const std::vector<std::string>& all_factors,
                  const std::vector<std::string>& contracts) {
    const auto factors = scenario.shape == Shape::history_query
                             ? std::vector<std::string>{all_factors.front()}
                             : all_factors;
    const auto values = generate_values(factors.size() * contracts.size());
    const std::array<std::vector<TradingDate>, 2> years{weekdays(2024, 0), weekdays(2025, 2)};
    const bool single_year = scenario.shape == Shape::day_query ||
                             scenario.shape == Shape::promotion_query;
    const auto year_count = single_year ? 1U : years.size();

    FactorStore store;
    std::vector<RequestedDate> requested;
    for (std::size_t y = 0; y < year_count; ++y) {
        build_query_year(scenario, years[y], factors, contracts, values, store, requested);
    }
    if (scenario.missing_dates_per_year != 0) {
        permute(requested);
    }
    std::vector<TradingDate> dates;
    dates.reserve(requested.size());
    for (const auto& day : requested) {
        dates.push_back(day.date);
    }
    const auto cells = values.size() * dates.size();

    auto elapsed = measure(options.samples, [&](bool warmup) {
        const auto begin = Clock::now();
        const auto result = store.query(factors, contracts, dates);
        const auto us = microseconds_since(begin);
        verify(result.values.size() == cells && result.factor_count == factors.size() &&
               result.contract_count == contracts.size() && result.date_count == dates.size());
        const auto check_series = [&](std::size_t series) {
            const auto offset = series * dates.size();
            for (std::size_t i = 0; i < dates.size(); ++i) {
                const auto index = offset + i;
                verify(result.has_value(index) == requested[i].present);
                if (requested[i].present) {
                    verify(result.values[index] == values[series] + date_offset(requested[i].date));
                }
            }
        };
        if (warmup) {
            for (std::size_t series = 0; series < values.size(); ++series) {
                check_series(series);
            }
        } else {
            check_series(0);
            check_series(values.size() - 1);
        }
        return us;
    });
    const auto missing = std::count_if(requested.begin(), requested.end(),
                                       [](const RequestedDate& day) { return !day.present; });
    return {std::move(elapsed), cells, std::to_string(100 * missing / requested.size())};
}

std::string peak_rss_kib(bool isolated) {
#ifdef __linux__
    struct rusage usage {};
    if (isolated && getrusage(RUSAGE_SELF, &usage) == 0) {
        return std::to_string(usage.ru_maxrss);
    }
#else
    static_cast<void>(isolated);
#endif
    return "NA";
}

void print_measurement(const Scenario& scenario, const Options& options, const Measurement& result) {
    const auto& times = result.elapsed_us;
    const auto percentile = [&](std::size_t percent) {
        return times[(percent * times.size() + 99) / 100 - 1];
    };
    const auto total_us = std::accumulate(times.begin(), times.end(), 0.0);
    if (total_us <= 0.0) {
        throw std::runtime_error("timer resolution is too coarse");
    }

    std::cout << scenario.name << ',' << options.profile << ',' << times.size() << ','
              << result.cells << ',' << scenario.stored_days << ',' << result.missing_percent << ','
              << std::fixed << std::setprecision(3) << percentile(50) << ',' << percentile(95) << ','
              << std::setprecision(0) << 1000000.0 * result.cells * times.size() / total_us << ','
              << peak_rss_kib(options.selected_case != "all") << '\n';
}

void run(const Options& options) {
    const bool large = options.profile == "large";
    const auto factors = names("factor_", large ? 24 : 8);
    const auto contracts = names("contract_", large ? 256 : 64);
    std::cout.imbue(std::locale::classic());
    std::cout << "case,profile,samples,cells_per_batch,stored_days_per_block,missing_percent,"
                 "p50_us,p95_us,cells_per_second,peak_rss_kib\n";
    for (const auto& scenario : scenarios) {
        if (options.selected_case != "all" && options.selected_case != scenario.name) {
            continue;
        }
        if (scenario.shape == Shape::annual) {
            print_measurement(scenario, options, annual(scenario, options));
        } else if (scenario.shape == Shape::daily) {
            print_measurement(scenario, options, daily(scenario, options, factors, contracts));
        } else {
            print_measurement(scenario, options, query(scenario, options, factors, contracts));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: factor_store_benchmarks [--case NAME] [--profile small|large] "
                     "[--samples 3..1000]\nCases: all";
        for (const auto& scenario : scenarios) {
            std::cout << ", " << scenario.name;
        }
        std::cout << '\n';
        return 0;
    }
    try {
        run(parse_options(argc, argv));
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

