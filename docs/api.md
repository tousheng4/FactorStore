# FactorStore 接口文档

项目使用 C++17。调用时包含 `factor_store/factor_store.h`，在 CMake 中链接 `FactorStore::factor_store`。[公开头文件](../include/factor_store/factor_store.h)

## 数据约定

- 因子名和合约名不能为空，区分大小写。
- `TradingDate` 是形如 `20260227` 的整数。库校验公历 1 至 9999 年的日期和闰日，交易日由调用方选定。
- 在不同批次写入同一个因子、合约和日期时，新值覆盖旧值。
- 调用期间请保持输入参数不变。查询结果拥有自己的数据，可以在实例销毁后读取。

## 1. 批量插入某个合约某一年的某个因子

```cpp
void upsert_year(std::string_view factor,
                 std::string_view contract,
                 int year,
                 const std::vector<DailyValue>& points);
```

`points` 的每项是 `{日期, 值}`。所有日期必须属于指定年份，顺序不限，一批内同一日期只能出现一次。只更新列表中的日期。空列表校验通过后直接返回。

## 2. 批量插入某天一批合约的一批因子

```cpp
void upsert_day(TradingDate date,
                const std::vector<std::string>& factors,
                const std::vector<std::string>& contracts,
                const std::vector<double>& values);
```

按因子顺序依次给出各合约的值。第 `f` 个因子、第 `c` 个合约对应 `values[f * contracts.size() + c]`。例如 2 个因子和 3 个合约需要 6 个值，前三个属于第一个因子。

同一个列表里的名称只能出现一次。因子或合约列表为空时，值列表也必须为空，校验通过后直接返回。

## 3. 查询当前总共有多少个因子

```cpp
std::size_t factor_count() const;
```

返回当前不同因子的数量。首次写入一个新因子时计入，空库返回 `0`。

## 4. 删除某个因子

```cpp
bool erase_factor(std::string_view factor);
```

删除该因子在所有合约和日期上的数据。删除成功返回 `true`，找不到时返回 `false`。之后可以重新写入同名因子。

## 5. 根据合约列表、日期列表和因子列表查询数据

```cpp
QueryResult query(const std::vector<std::string>& factors,
                  const std::vector<std::string>& contracts,
                  const std::vector<TradingDate>& dates) const;
```

调用时依次传因子、合约、日期。结果按 `[因子][合约][日期]` 排列，保留各列表的顺序和重复项。第 `(f, c, d)` 个值的下标是 `(f * result.contract_count + c) * result.date_count + d`。

先用 `result.has_value(index)` 判断，再读 `result.values[index]`。遇到缺失或下标越界时，`has_value` 返回 `false`。写入的 `0.0` 和 `NaN` 都是有效值。任一列表为空时，结果保留三个列表的长度，值和有效位为空。

## 失败和并发

- 空名称、非法日期、批内重复或写入值数不对会抛出 `std::invalid_argument`，这类错误会在写入前发现。
- 请求太大或内部合约 ID 用完时抛出 `std::length_error`。内存分配失败可能抛出 `std::bad_alloc`，批量写入中已完成的部分可能保留。
- 同一个实例可以由多个线程调用。一次查询能看到一批成功写入的完整结果。销毁实例前应等所有调用结束。

## 完整示例

```cpp
#include "factor_store/factor_store.h"

#include <cassert>

int main() {
    factor_store::FactorStore store;
    store.upsert_year("momentum", "IF2606", 2026,
                      {{20260226, 0.5}, {20260227, 0.8}});
    store.upsert_day(20260227, {"momentum", "volatility"},
                     {"IF2606", "IC2606"}, {1.5, 2.5, 0.0, 3.0});
    assert(store.factor_count() == 2);

    const auto result = store.query({"momentum"}, {"IF2606", "IC2606"},
                                    {20260226, 20260227});
    assert(result.has_value(0) && result.values[0] == 0.5);
    assert(result.has_value(1) && result.values[1] == 1.5);
    assert(!result.has_value(2));  // IC2606 在 2 月 26 日没有值。

    const bool removed = store.erase_factor("momentum");
    return removed && store.factor_count() == 1 ? 0 : 1;
}
```

在 Linux 或 WSL 上，把示例保存为仓库根目录的 `example.cpp` 后运行以下命令。退出码为 0 表示通过。

```bash
cmake -S . -B build-example -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake --build build-example --target factor_store
g++ -std=c++17 -pthread -Iinclude example.cpp build-example/libfactor_store.a -o build-example/example
./build-example/example
```
