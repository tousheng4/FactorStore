# FactorStore 接口文档

公开接口定义在 [factor_store/factor_store.h](../include/factor_store/factor_store.h)，位于 `factor_store` 命名空间，使用 C++17。在同一 CMake 构建中引入本项目后，调用方可链接 `FactorStore::factor_store`。

## 数据约定

- 每条数据由因子名称、合约名称和交易日期唯一标识，值为 `double`。名称非空，按字符串内容精确匹配，区分大小写。
- `TradingDate` 是按 `YYYYMMDD` 编码的整数，例如 `20260227`。支持公历 1 至 9999 年并校验闰日。交易日历由调用方管理，库校验公历日期。
- `0.0` 和 `NaN` 都可写入，且都属于存在的值。查询用有效位图区分存在与缺失。
- 输入在调用期间借用，调用方应保持输入不变。调用返回后，调用方可以释放或修改输入。查询结果拥有独立的缓冲区。
- 每个 `FactorStore` 实例在构造后为空，独立持有数据，析构时释放数据。实例禁止复制和移动。

## 并发语义

同一实例支持多线程调用。已有名称的读写按因子同步，不涉及同一因子的读写可以并行。计数读取目录，登记新因子或合约以及删除因子时需要独占目录。一次查询得到该调用期间的一个一致状态，成功的写入批次对它整体可见。日写触发的稠密块转换在目标因子的写锁内完成。

删除与同名重建是两个操作。两次操作之间的查询可以看到该因子缺失，重建后的查询读取新写入的数据。每次调用独立同步。如果业务要把多个调用视为一个操作，调用方需要协调它们的时序。析构实例前，调用方应等待对该实例的所有调用完成。

写入发生内存分配错误时，部分更新可能保留，并在该次调用退出后对其他操作可见。

## 年度批量写入

```cpp
void upsert_year(std::string_view factor,
                 std::string_view contract,
                 int year,
                 const std::vector<DailyValue>& points);
```

`DailyValue` 包含 `date` 和 `value`。`year` 取值为 1 至 9999。日期可乱序，每个日期必须属于指定年份，同一批次内不得重复。已有数据按日期覆盖，批次以外的日期保留。空 `points` 在参数校验通过后直接返回，已有数据不受影响，也不会创建因子。

## 单日批量写入

```cpp
void upsert_day(TradingDate date,
                const std::vector<std::string>& factors,
                const std::vector<std::string>& contracts,
                const std::vector<double>& values);
```

值数组按 `[因子][合约]` 排列。第 `f` 个因子、第 `c` 个合约的值位于 `values[f * contracts.size() + c]`。数组长度必须等于两个轴的长度之积，每个轴内的名称不得重复。已有数据按因子、合约和当天日期覆盖。任一轴为空时，`values` 必须为空，且校验通过后不会写入数据或创建因子。

## 因子计数与删除

```cpp
std::size_t factor_count() const;
bool erase_factor(std::string_view factor);
```

`factor_count()` 返回当前存在的不同因子数，空库返回 `0`。因子有第一条数据时计入，覆盖已有数据不改变数量。

`erase_factor()` 删除指定因子在所有合约和日期上的数据。找到并删除时返回 `true`，因子不存在时返回 `false`。删除后可重新写入同名因子，重新创建的因子只包含新写入的数据。

## 批量查询

```cpp
QueryResult query(const std::vector<std::string>& factors,
                  const std::vector<std::string>& contracts,
                  const std::vector<TradingDate>& dates) const;
```

结果包含输入的三个轴长度。每个轴保留输入的顺序与重复项。结果按 `[因子][合约][日期]` 排列，第 `(f, c, d)` 个位置的下标为 `(f * result.contract_count + c) * result.date_count + d`。

`result.values` 包含各轴笛卡尔积对应的全部位置。`result.validity` 每个 `uint64_t` 保存 64 个有效位，从最低位开始。使用 `result.has_value(index)` 判断该位置是否有值。未知因子、未知合约和未写入日期都标为缺失，缺失位置的 `values[index]` 无需读取。`has_value` 对越界下标返回 `false`。

如果任一轴为空，校验通过后仍返回原始轴长度，值数组和位图均为空。结果占用的内存随三个轴长度的乘积增长，调用方可按因子或日期拆分大型查询。

## 错误与失败行为

| 情况 | 行为 |
| --- | --- |
| 空名称、非法日期或年份、年度写入日期不属于指定年份、写入批次内重复键、写入矩阵长度不匹配 | 抛出 `std::invalid_argument` |
| 矩阵尺寸乘积溢出，查询结果超过缓冲区可表示的容量，或内部合约 ID 耗尽 | 抛出 `std::length_error` |
| 内存分配失败 | 可能抛出 `std::bad_alloc`。写入过程中已完成的更新可能保留，因子目录和计数继续保持一致。 |

两种写入均在修改数据前检查参数和矩阵尺寸，包括空批次。因参数错误或矩阵尺寸错误而失败的批次不会修改存储。

## 完整示例

年度写入为 `IF2606` 写入两天的 `momentum`。单日写入随后覆盖其中一天，并写入两个合约的 `volatility`。查询指定因子和合约的反向顺序，检查日期间的覆盖、合法 `NaN` 和缺失位置。

```cpp
#include "factor_store/factor_store.h"

#include <cmath>
#include <limits>

int main() {
    factor_store::FactorStore store;
    store.upsert_year("momentum", "IF2606", 2026,
                      {{20260226, 0.5}, {20260227, 0.8}});

    const double nan = std::numeric_limits<double>::quiet_NaN();
    store.upsert_day(20260227, {"momentum", "volatility"},
                     {"IF2606", "IC2606"}, {1.5, 2.5, nan, 0.0});

    const auto result = store.query({"volatility", "momentum"},
                                    {"IC2606", "IF2606"},
                                    {20260227, 20260226});
    const bool values_match =
        result.has_value(0) && result.values[0] == 0.0 &&
        !result.has_value(1) &&
        result.has_value(2) && std::isnan(result.values[2]) &&
        result.has_value(6) && result.values[6] == 1.5 &&
        result.has_value(7) && result.values[7] == 0.5;

    const bool had_two_factors = store.factor_count() == 2;
    const bool erased = store.erase_factor("momentum");
    return values_match && had_two_factors && erased && store.factor_count() == 1
               ? 0
               : 1;
}
```

在 Linux 或 WSL 上，将示例保存为仓库根目录的 `example.cpp`，然后构建静态库并运行：

```bash
cmake -S . -B build-example -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake --build build-example --target factor_store
g++ -std=c++17 -pthread -Iinclude example.cpp build-example/libfactor_store.a -o build-example/example
./build-example/example
```

示例校验通过时以状态码 `0` 退出。
