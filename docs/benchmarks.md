# FactorStore 性能基准报告

## 复现

在 Linux 或 WSL 上，从仓库根目录构建 Release 版本。基准无需第三方 C++ 库。

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DFACTOR_STORE_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
(cd build-bench && ctest --output-on-failure)
./build-bench/benchmarks/factor_store_benchmarks --profile small --samples 61
taskset -c 3 ./build-bench/benchmarks/factor_store_benchmarks \
  --case query-mixed-dense --profile large --samples 61
```

`--help` 列出 12 个场景，`--case` 默认执行全部，`--profile` 默认为 `small`，`--samples` 默认为 31。需要逐场景的峰值内存时，为每个场景启动一次新的进程。CPU 编号按本机可用范围选择。

## 负载与测量口径

- `small` 有 8 个因子和 64 个合约，`large` 有 24 个因子和 256 个合约。年度写入使用单因子、单合约，历史查询使用单因子和全部合约。
- 数据用固定种子 `20261127` 生成。2024 年和 2025 年各取前 252 个工作日作为模拟日期。年度乱序回补写入 63 日，另有 189 日已存在。单日追加与覆盖以 64 日为初始密度。阈值场景从 191 日写到 192 日。
- 首次日写包括名称映射、索引建块和分配。稀疏布局在 64 日时还有数组空位，此时的追加测量反映已有块的写入成本。
- 历史查询跨两年并命中 504 个日期。单日查询读取整张矩阵。无序查询按同样的方式请求两年共 64 日，命中和缺失日期分布在年内。两个高密度无序场景分别设定 25% 和 50% 的缺失率。
- 用单调时钟测量一次 API 调用。测试数据准备、预热、结果校验、调用结束后的析构均在计时之外。查询预热遍历全部结果检查值和有效位。P50 和 P95 使用最邻近秩，吞吐量为总请求值数除以累计耗时，查询的缺失位置计入请求值数。
- 峰值 RSS 取 Linux 独立进程的 `ru_maxrss`，包含运行时、库状态、输入输出缓冲及转换期间的分配。`--case all` 和其他平台的内存列显示 `NA`。

## 混合布局实测

测量于 2026-11-27，AMD Ryzen 9 7940H，WSL2 Linux 6.6.87.2，16 个可见逻辑 CPU，约 7.37 GiB 可见内存。使用 GCC 13.3.0、CMake 3.28.3、Release（`-O3 -DNDEBUG`），将进程固定在逻辑 CPU 3。每个场景在单独进程中测 61 批。这些结果用于比较本机和本文负载。

### small

| 场景 | 每批值数 | 已有天数 / 块 | 缺失率 | P50 µs | P95 µs | 百万值 / 秒 | 峰值 RSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `year-new` | 252 | 0 | — | 2.77 | 2.80 | 91.31 | 3.63 |
| `year-overwrite` | 252 | 252 | — | 2.50 | 2.54 | 100.57 | 3.63 |
| `year-backfill` | 63 | 189 | — | 1.37 | 1.44 | 45.34 | 3.63 |
| `day-new` | 512 | 0 | — | 29.99 | 36.06 | 16.69 | 3.75 |
| `day-append` | 512 | 64 | — | 13.12 | 18.31 | 35.87 | 4.38 |
| `day-overwrite` | 512 | 64 | — | 10.01 | 15.86 | 47.50 | 4.38 |
| `day-threshold` | 512 | 191 | — | 320.21 | 378.75 | 1.56 | 5.10 |
| `query-history` | 32256 | 252 | 0% | 54.94 | 70.46 | 542.68 | 4.29 |
| `query-day` | 512 | 1 | 0% | 4.30 | 4.33 | 118.65 | 3.75 |
| `query-mixed-sparse` | 32768 | 32 | 50% | 159.63 | 219.14 | 189.11 | 4.61 |
| `query-mixed-dense` | 32768 | 224 | 50% | 74.00 | 109.53 | 397.39 | 6.87 |
| `query-mixed-dense-25` | 32768 | 224 | 25% | 80.41 | 127.69 | 352.11 | 6.87 |

### large

| 场景 | 每批值数 | 已有天数 / 块 | 缺失率 | P50 µs | P95 µs | 百万值 / 秒 | 峰值 RSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `year-new` | 252 | 0 | — | 2.93 | 3.04 | 85.70 | 3.63 |
| `year-overwrite` | 252 | 252 | — | 2.49 | 2.50 | 101.37 | 3.63 |
| `year-backfill` | 63 | 189 | — | 1.34 | 1.47 | 46.27 | 3.63 |
| `day-new` | 6144 | 0 | — | 479.00 | 651.68 | 12.28 | 4.63 |
| `day-append` | 6144 | 64 | — | 295.75 | 655.37 | 16.79 | 12.25 |
| `day-overwrite` | 6144 | 64 | — | 272.59 | 611.12 | 18.96 | 12.25 |
| `day-threshold` | 6144 | 191 | — | 5078.50 | 5920.56 | 1.19 | 22.25 |
| `query-history` | 129024 | 252 | 0% | 216.40 | 290.15 | 543.51 | 6.23 |
| `query-day` | 6144 | 1 | 0% | 59.97 | 92.80 | 92.77 | 4.75 |
| `query-mixed-sparse` | 393216 | 32 | 50% | 4103.10 | 4569.33 | 94.84 | 16.29 |
| `query-mixed-dense` | 393216 | 224 | 50% | 4308.30 | 4952.07 | 89.72 | 43.39 |
| `query-mixed-dense-25` | 393216 | 224 | 25% | 4781.38 | 5093.65 | 81.39 | 43.39 |

## 三种布局对照

纯稀疏使用阶段二实现。纯稠密从首次写入起就分配 366 个 `double` 槽和有效位图。混合布局在有效日期达到 192 个时转换，并释放稀疏数组。三种布局用同一程序和数据，交替运行三轮，每轮 61 批。阈值场景每轮 31 批。表中为三轮各指标的中位数，每格为 P50（µs）和峰值 RSS（MiB）。

| 场景（large） | 纯稀疏 P50 / RSS | 纯稠密 P50 / RSS | 混合 P50 / RSS |
| --- | ---: | ---: | ---: |
| `year-overwrite` | 2.83 / 3.63 | 2.50 / 3.63 | 2.49 / 3.63 |
| `day-new` | 509.94 / 4.63 | 1287.67 / 21.88 | 506.54 / 4.63 |
| `day-append` | 386.19 / 12.25 | 104.65 / 21.88 | 387.89 / 12.25 |
| `day-overwrite` | 329.29 / 12.25 | 99.04 / 21.88 | 310.82 / 12.25 |
| `day-threshold` | 176.06 / 19.90 | 95.44 / 21.88 | 5165.00 / 22.25 |
| `query-history` | 905.74 / 6.00 | 218.27 / 6.16 | 216.88 / 6.23 |
| `query-day` | 58.44 / 4.75 | 88.55 / 21.88 | 60.15 / 4.75 |
| `query-mixed-sparse` | 3094.00 / 16.29 | 4324.70 / 42.96 | 3712.43 / 16.29 |
| `query-mixed-dense` | 8789.23 / 38.68 | 4357.16 / 42.96 | 4326.03 / 43.39 |
| `query-mixed-dense-25` | 12148.26 / 38.69 | 5536.73 / 42.96 | 5220.24 / 43.39 |

混合布局在两类高密度读取上缩短了延迟。跨年历史查询的 P50 从约 0.906 ms 降到 0.217 ms，50% 缺失率的高密度无序查询从约 8.79 ms 降到 4.33 ms。该高密度无序场景的峰值 RSS 增加约 4.7 MiB。大型首次日写的峰值 RSS 同纯稀疏相当，纯稠密在该场景达到约 21.88 MiB。

代价主要在两类负载。32 日块的无序查询 P50 比纯稀疏增加约 20%，而 6,144 个块同时达到 192 日时，单批 P50 从约 0.176 ms 增到 5.17 ms。转换成本在每个块首次达到阈值时发生。当前选择混合布局，优先覆盖高密度历史读取，同时保留新块的低内存开销。192 是本次对照采用的阈值，适合结合实际负载复测。

要复跑三种布局，在有提交历史的工作副本中用同一个基准文件构建。下面只向临时目录写入实验文件。

```bash
scratch=$(mktemp -d)
git show c551385:src/factor_store.cpp > "$scratch/sparse.cpp"
sed 's/dense_threshold = 192;/dense_threshold = 1;/' src/factor_store.cpp \
  > "$scratch/dense.cpp"
g++ -std=c++17 -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  "$scratch/sparse.cpp" -o "$scratch/sparse"
g++ -std=c++17 -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  "$scratch/dense.cpp" -o "$scratch/dense"
g++ -std=c++17 -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  src/factor_store.cpp -o "$scratch/hybrid"

for order in 'sparse hybrid dense' 'hybrid dense sparse' 'dense sparse hybrid'; do
  for layout in $order; do
    taskset -c 3 "$scratch/$layout" --case query-mixed-dense --profile large --samples 61
  done
done
```

## 正确性验证

最终 Release 构建的 CTest 三项通过。闰年回归覆盖单日写入跨阈值、年度批次直接创建稠密块和两个小批次合并跨阈值。用例检查了缺失、零值、NaN 和覆盖行为。核心单元测试通过 ASan 与 UBSan。
