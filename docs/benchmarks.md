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

`--help` 列出全部场景。`--case` 默认执行全部单线程场景，读写混合场景单独用 `--case concurrent-mixed` 启动。`--profile` 默认为 `small`，`--samples` 默认为 31。需要逐场景的峰值内存时，为每个场景启动一次新的进程。CPU 编号按本机可用范围选择。

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
git show 2ec688c:src/factor_store.cpp > "$scratch/hybrid.cpp"
sed 's/dense_threshold = 192;/dense_threshold = 1;/' "$scratch/hybrid.cpp" \
  > "$scratch/dense.cpp"
g++ -std=c++17 -pthread -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  "$scratch/sparse.cpp" -o "$scratch/sparse"
g++ -std=c++17 -pthread -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  "$scratch/dense.cpp" -o "$scratch/dense"
g++ -std=c++17 -pthread -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  "$scratch/hybrid.cpp" -o "$scratch/hybrid"

for order in 'sparse hybrid dense' 'hybrid dense sparse' 'dense sparse hybrid'; do
  for layout in $order; do
    taskset -c 3 "$scratch/$layout" --case query-mixed-dense --profile large --samples 61
  done
done
```

## 正确性验证

最终 Release 构建的 CTest 三项通过。闰年回归覆盖单日写入跨阈值、年度批次直接创建稠密块和两个小批次合并跨阈值。用例检查了缺失、零值、NaN 和覆盖行为。核心单元测试通过 ASan 与 UBSan。

## 分块批量查询对照

这组结果比较同样的混合存储布局，基线是提交 `2ec688c` 的逐日期查询，优化版在每个块内选择一次读取路径。稀疏块在每年请求至少 24 个日期、且请求数量达到块内日期数量约四分之一时采用顺序归并。该门槛是当前的调优取值，适合随负载复测。

在 WSL2 中用 GCC 13.3.0 构建，两版均使用 `-O3 -DNDEBUG`，固定在逻辑 CPU 3。每个规模和场景交替运行三轮，每轮 61 批。表中为三轮对应指标的中位数，延迟单位为 µs。

| 规模 | 场景 | P50 基线 → 优化 | P95 基线 → 优化 | P50 降幅 |
| --- | --- | ---: | ---: | ---: |
| small | `query-history` | 50.56 → 42.88 | 63.09 → 46.21 | 15.2% |
| small | `query-day` | 4.02 → 3.98 | 4.21 → 4.52 | 1.0% |
| small | `query-mixed-sparse` | 142.21 → 60.22 | 214.22 → 81.52 | 57.7% |
| small | `query-mixed-dense` | 70.60 → 61.10 | 99.00 → 84.30 | 13.5% |
| small | `query-mixed-dense-25` | 77.11 → 68.16 | 138.21 → 106.31 | 11.6% |
| large | `query-history` | 203.02 → 173.91 | 265.26 → 211.30 | 14.3% |
| large | `query-day` | 57.68 → 55.62 | 75.28 → 69.72 | 3.6% |
| large | `query-mixed-sparse` | 2999.69 → 1215.98 | 4484.68 → 3142.28 | 59.5% |
| large | `query-mixed-dense` | 4156.69 → 3668.68 | 5695.34 → 5132.55 | 11.7% |
| large | `query-mixed-dense-25` | 4398.37 → 4227.53 | 6637.90 → 4848.91 | 3.9% |

稀疏无序查询的 P50 降幅最大，两种规模均超过 57%。跨年历史查询同样受益。单日点查的 P50 大致持平，小规模单日点查的 P95 略有增加。高密度、低缺失率场景的耗时波动较大。

下列命令使用同一份基准程序，对比逐日期查询的基线与当前实现。若当前实现已包含分批转换，结果会反映两项优化的组合效果，上表只衡量分块查询优化。命令需在包含基线提交的 Git 工作副本中执行。

```bash
scratch=$(mktemp -d)
git show 2ec688c:src/factor_store.cpp > "$scratch/baseline.cpp"
g++ -std=c++17 -pthread -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  "$scratch/baseline.cpp" -o "$scratch/baseline"
g++ -std=c++17 -pthread -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  src/factor_store.cpp -o "$scratch/optimized"
for order in 'baseline optimized' 'optimized baseline' 'baseline optimized'; do
  for version in $order; do
    taskset -c 3 "$scratch/$version" --case query-mixed-sparse --profile large --samples 61
  done
done
```

## 分批转换对照

基线为分块查询优化完成后、仍执行即时转换的版本。最终版按日写批次消化转换队列，等待中的块仍可继续读写。每批预算为 `min(256, ceil(写入值数 / 16))`。每个合约都跨过阈值时，`small` 首批转换 32 个块，`large` 首批转换 256 个块。

`query-promotion-pending` 和 `query-promotion-drained` 写入完全相同的一年 192 日数据，请求同样的 32 个日期，缺失率同为 50%。前者在跨阈值日写结束后立即查询，后者先对同一日做 32 批同值覆盖，给队列时间清空。该设置只改变查询前的转换进度。

两版使用同一份基准程序，GCC 13.3.0、`-O3 -DNDEBUG`、WSL2 的逻辑 CPU 3。每个场景交替执行三轮，每轮 31 批。下表为各指标的三轮中位数，由即时转换指向最终版。延迟单位为 µs。

| 规模 | 场景 | P50 即时 → 分批 | P95 即时 → 分批 | 峰值 RSS MiB |
| --- | --- | ---: | ---: | ---: |
| small | `day-new` | 28.22 → 32.55 | 43.66 → 51.22 | 3.90 → 3.91 |
| small | `day-append` | 12.23 → 8.30 | 15.69 → 11.33 | 4.53 → 4.54 |
| small | `day-overwrite` | 10.18 → 9.83 | 12.32 → 12.31 | 4.54 → 4.54 |
| small | `day-threshold` | 239.34 → 34.73 | 337.79 → 47.63 | 5.20 → 4.92 |
| small | `query-mixed-dense` | 60.49 → 61.43 | 75.30 → 76.51 | 6.96 → 6.96 |
| small | `query-promotion-pending` | 25.42 → 56.66 | 65.44 → 109.83 | 5.18 → 5.13 |
| small | `query-promotion-drained` | 25.28 → 25.73 | 48.63 → 42.27 | 5.23 → 5.31 |
| large | `day-new` | 446.51 → 439.69 | 537.47 → 546.05 | 4.77 → 4.91 |
| large | `day-append` | 324.71 → 174.93 | 659.70 → 432.50 | 12.35 → 12.36 |
| large | `day-overwrite` | 213.68 → 212.22 | 576.83 → 471.98 | 12.35 → 12.35 |
| large | `day-threshold` | 4408.68 → 295.68 | 5946.71 → 533.16 | 22.35 → 20.14 |
| large | `query-mixed-dense` | 3611.72 → 3564.64 | 4404.86 → 6167.21 | 43.50 → 43.69 |
| large | `query-promotion-pending` | 766.33 → 1865.70 | 2282.02 → 3451.82 | 23.88 → 21.71 |
| large | `query-promotion-drained` | 939.82 → 793.74 | 2177.36 → 2039.04 | 23.86 → 23.98 |

- 6,144 个块同时跨阈值时，日写 P95 从约 5.95 ms 降到 0.53 ms，降幅 91%。普通顺序追加也从跳过不必要的二分查找中受益。
- 排队仍有读成本。大型场景查询 P50 从即时转换的约 0.77 ms 增至 1.87 ms。清空队列后，分批版的同负载 P50 回到约 0.79 ms。等待中的块对较大的请求较早采用顺序归并，以减少重复二分。
- 此组大型常规高密度查询的 P50 接近，P95 从 4.40 ms 增至 6.17 ms。该场景的尾延迟跨轮波动明显，仍需结合目标负载复测。

用本文开头的 Release 命令构建后，可运行以下三个场景。

```bash
for scenario in day-threshold query-promotion-pending query-promotion-drained; do
  taskset -c 3 ./build-bench/benchmarks/factor_store_benchmarks \
    --case "$scenario" --profile large --samples 31
done
```

最终 Release 的三项 CTest 均通过，功能用例覆盖待转换块的查询、年度写入完成转换、删除与同名重建，以及无关日写推进队列。核心单元测试同时通过 ASan 和 UBSan。

## 单值块内联对照

基线为 `bd756ec`，稀疏块首次写入时即为日期和值分别分配数组。优化版把第一条记录直接存进年度块，第二个不同日期才构造 SoA 数组。两版使用同一份基准程序，GCC 13.3.0、`-O3 -DNDEBUG`，固定在 WSL2 的逻辑 CPU 3。每个场景交替运行三轮，每轮 31 批。延迟为各指标的三轮中位数，单位为 µs。

| 规模 | 场景 | P50 基线 → 优化 | P95 基线 → 优化 | 峰值 RSS MiB |
| --- | --- | ---: | ---: | ---: |
| small | `day-new` | 28.29 → 16.86 | 36.97 → 31.64 | 3.91 → 3.91 |
| small | `day-second` | 24.50 → 18.39 | 28.32 → 20.20 | 3.91 → 3.91 |
| small | `day-append` | 8.54 → 8.10 | 11.60 → 11.14 | 4.54 → 4.53 |
| small | `day-overwrite` | 10.10 → 9.98 | 13.95 → 12.04 | 4.54 → 4.54 |
| small | `day-threshold` | 36.69 → 32.76 | 46.51 → 61.38 | 4.98 → 4.93 |
| small | `query-day` | 4.26 → 4.15 | 4.48 → 4.39 | 3.94 → 3.87 |
| small | `query-history` | 44.63 → 45.89 | 59.64 → 76.07 | 4.30 → 4.30 |
| small | `query-mixed-sparse` | 76.80 → 66.80 | 143.56 → 107.72 | 4.75 → 4.68 |
| small | `query-mixed-dense` | 62.92 → 61.17 | 163.54 → 95.09 | 6.96 → 6.96 |
| large | `day-new` | 453.87 → 208.54 | 564.86 → 262.55 | 4.91 → 4.48 |
| large | `day-second` | 224.38 → 207.19 | 322.67 → 309.50 | 4.91 → 4.75 |
| large | `day-append` | 197.50 → 165.02 | 517.08 → 376.55 | 12.35 → 12.25 |
| large | `day-overwrite` | 252.71 → 231.19 | 577.57 → 486.42 | 12.34 → 12.24 |
| large | `day-threshold` | 274.08 → 290.73 | 443.18 → 474.43 | 20.13 → 19.80 |
| large | `query-day` | 58.81 → 40.99 | 84.11 → 58.91 | 4.94 → 4.56 |
| large | `query-history` | 173.04 → 173.44 | 232.13 → 319.71 | 6.41 → 6.39 |
| large | `query-mixed-sparse` | 1423.79 → 1457.04 | 3274.01 → 2954.01 | 16.67 → 16.25 |
| large | `query-mixed-dense` | 3295.44 → 3407.45 | 4807.01 → 4563.32 | 43.72 → 43.24 |

- 大型 `day-new` 一次创建 6,144 个块，P50 从约 454 µs 降至 209 µs。在同样大小的临时分配计数探针中，首次日写的 `operator new` 调用由 19,170 次降为 6,882 次，恰好少 12,288 次。
- `day-second` 在同一批块先各写入一天，再测第二天的写入。大型场景的 P50 从约 224 µs 降至 207 µs，第二次写入的分配计数仍为 12,571 次。前两天累计节省 12,288 次分配。
- 大型单日查询 P50 从约 58.81 µs 降至 40.99 µs。跨年查询 P50 接近，本轮 P95 从约 232 µs 升到 320 µs。大型稠密无序查询和跨阈值日写的 P50 也小幅增加，收益主要集中在单值块。

用开头的 Release 命令构建后，可运行 `day-new`、`day-second` 和 `query-day`。要对比基线，可在包含 `bd756ec` 的 Git 工作副本中，用当前的基准源码分别链接两版实现。

```bash
scratch=$(mktemp -d)
git show bd756ec:src/factor_store.cpp > "$scratch/baseline.cpp"
g++ -std=c++17 -pthread -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  "$scratch/baseline.cpp" -o "$scratch/baseline"
g++ -std=c++17 -pthread -O3 -DNDEBUG -Iinclude benchmarks/factor_store_benchmark.cpp \
  src/factor_store.cpp -o "$scratch/optimized"
for order in 'baseline optimized' 'optimized baseline' 'baseline optimized'; do
  for version in $order; do
    taskset -c 3 "$scratch/$version" --case day-new --profile large --samples 31
  done
done
```

## 读写混合负载

使用同一实例和同一组数据，分别测单独执行与混合执行。每个年度块预写 64 日，查询读取其中 32 日，日写覆盖已有的一天。混合场景由两个读取线程和一个写入线程同时启动。每个线程先预热一次，再分别采集 101 次调用。计时包含 API 内的准备、锁等待和访问数据。

在 WSL2 使用 GCC 13.3.0 和 Release 构建，将进程固定在逻辑 CPU 2、3、4。每个规模运行三轮，下表取各轮 P50 和 P95 的中位数。延迟单位为 µs。

| 规模 | 操作 | 每批值数 | 单独 P50 / P95 | 混合 P50 / P95 |
| --- | --- | ---: | ---: | ---: |
| small | 查询 | 16384 | 35.03 / 52.26 | 35.27 / 120.73 |
| small | 日写 | 512 | 7.76 / 8.94 | 7.91 / 172.80 |
| large | 查询 | 196608 | 765.94 / 1683.88 | 1063.97 / 1930.28 |
| large | 日写 | 6144 | 111.84 / 228.34 | 1053.88 / 2117.13 |

实例级锁给一次查询提供完整的状态视图。在该大型负载中，查询 P95 从约 1.68 ms 增至 1.93 ms，日写 P95 从约 0.23 ms 增至 2.12 ms。锁竞争的代价主要体现在写入尾延迟。实际等待随查询长度、读写比例和线程调度变化。

用本文开头的命令构建后即可复测。混合场景使用多个逻辑 CPU。

```bash
taskset -c 2,3,4 ./build-bench/benchmarks/factor_store_benchmarks \
  --case concurrent-mixed --profile large --samples 101
```

Release CTest 四项通过，并发用例连续运行 20 次。并发用例也通过 ASan、UBSan 和 TSan。本次在 WSL 上用 `setarch x86_64 -R` 仅对 TSan 测试进程关闭地址随机化后运行检查。
