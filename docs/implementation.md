# FactorStore 代码说明

## 从哪里读起

| 文件 | 职责 |
| --- | --- |
| [公开头文件](../include/factor_store/factor_store.h) | `TradingDate`、`DailyValue`、`QueryResult` 和五项操作的声明 |
| [核心源码](../src/factor_store.cpp) | 日期校验、名称索引、活动因子目录、块读写和查询结果组装 |
| [功能测试](../tests/factor_store_test.cpp) | 写入与覆盖、无效批次、查询维度、因子删除和参照模型比对 |
| [基准程序](../benchmarks/factor_store_benchmark.cpp) | 确定性的模拟负载、调用计时、结果校验和 CSV 输出 |
| [顶层构建配置](../CMakeLists.txt) | `factor_store` 库、公开别名 `FactorStore::factor_store` 和可选测试目标 |

## 索引与内存所有权

`FactorStore::Impl` 持有所有数据。因子由 `vector<unique_ptr<Factor>>` 拥有，合约名由 `deque<string>` 拥有。两个名称索引中的 `string_view` 指向库自己持有的字符串，所以输入字符串可以在调用结束后释放。因子 ID 是活动目录的下标，属于内部实现。

`factor_count` 直接读取目录长度。`erase_factor` 先移除名称索引，释放目标因子，再用目录最后一个因子填补空位并修正它的 ID。合约名称索引由所有因子共享，其条目可在实例生命周期内供后续写入复用。

## 批量写入

- `upsert_year` 先校验名称与年份。`parse_date` 将每个日期转换成年份和年内序号，位图在校验期间记录批内重复日期。输入排序后，`YearBlock::merge` 在线性归并中以新值覆盖重合日期，或将值直接写入稠密槽。
- `upsert_day` 校验日期、矩阵尺寸和轴上名称的唯一性，然后为每个合约计算一次块键。外层按因子遍历，内层按合约读取连续的日写矩阵。`YearBlock::upsert` 覆盖已有条目或插入新日期。日期晚于稀疏块尾部时直接定位到尾端。
- 年度块用 `std::variant` 保存三种表示中的一种。第一条记录直接放在块内。写入第二个不同日期时，先构建日期数组和数值数组，再切换到 SoA。单值块查询时只比较保存的那一个日期。
- 年度写入的单块达到 192 个有效日期时可以当场转换。日写在块首次达到阈值后将其加入 `PromotionQueue`，数据仍以稀疏形式可查。每个成功的非空日写批次结束时，队列按加入顺序处理，预算是 `min(256, ceil(写入值数 / 16))`。
- `YearBlock::promote` 先分配并填充新块，再替换原表示并取消排队。队列链接嵌在块中，入队无需额外分配。块随因子销毁时自动脱离队列，因子同名重建使用新的块。

`Impl::write_factor` 先将新因子写入临时对象，再登记到目录。`Factor::write_block` 在新块写入失败时撤销该块的登记。内存分配失败时，整个批次中已完成的其他写入可能保留。日写已经成功、仅在后续转换时分配失败的情况下，队首块保持稀疏，留待后续批次处理。异常行为以[接口文档](api.md)为准。

## 批量查询

1. `query` 取得结果形状，检查维度乘积和缓冲区容量。空轴返回保留原始轴长度的空结果。
2. 日期按年份分组，每个条目保留原始日期下标。每个合约的内部 ID 也按调用方的位置保存。
3. 查询遍历因子和合约，对每个访问到的年份定位一次 `YearBlock`。由 `read_many` 在块级选择路径。稠密块逐项检查有效位，再读数组槽。稀疏块对少量日期逐项二分。当前一年请求至少 24 项且达到块内日期数约四分之一时，`read_sparse_in_order` 使用两个位置顺序匹配请求与存储日期。等待转换的稀疏块将比例门槛降到约八分之一。
4. 按原位置填充连续结果数组，同时设置对应的有效位。查询结果持有自己的缓冲区，实例销毁后仍能读取它。

`QueryDays` 记录每年请求是否已经按日序号排序。只有首次顺序归并遇到乱序输入时才排序。每个请求携带原始下标，写回时保留输入顺序与重复项。

## 并发控制

`Impl::mutex` 是实例级 `std::shared_mutex`。查询完成日期、形状校验和结果缓冲区分配后，才获取 `std::shared_lock`。从解析名称到填完结果一直持锁。计数也持共享锁，多个读取操作可以并行。

两个写入接口先完成输入校验，再用 `std::unique_lock` 串行修改数据和索引。删除与日写后的块转换也由同一把锁保护。调用返回或异常离开时，局部锁对象自动释放锁。

## 构建和验证

项目要求 CMake 3.16 或更高版本以及支持 C++17 的编译器。以下命令适用于 Linux 和 WSL。将 `build-release` 换成一个新目录即可做干净构建。

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DFACTOR_STORE_BUILD_BENCHMARKS=ON
cmake --build build-release --parallel
(cd build-release && ctest --output-on-failure)
```

- `factor_store_smoke_test` 检查库及公开头文件是否可链接和使用。
- `factor_store_unit_tests` 覆盖五个接口，重点检查闰年和跨年、参数校验后的状态、删除后同名重建，以及三条稀疏转稠密路径。固定种子的 500 步随机序列会与简单的三元组映射逐步比对。
- `factor_store_concurrency_tests` 让两个读取线程与写入交错执行，校验整批可见性、删除与同名重建，以及其他因子数据的稳定性。另有一组双写测试，检查年度和单日接口同时更新一个块时各自的更新都能保留。
- `factor_store_benchmark_smoke_test` 检查场景、规模、缺失率和 CSV 输出，耗时由独立的性能基准记录。此项在单元测试和性能基准同时启用时注册。

使用[接口示例](api.md#完整示例)验证调用方编译和链接。复跑性能基准时使用 Release 构建，CSV 数据、复现命令和布局对照见[性能基准报告](benchmarks.md)。
