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
- `upsert_day` 校验日期、矩阵尺寸和轴上名称的唯一性，然后为每个合约计算一次块键。外层按因子遍历，内层按合约读取连续的日写矩阵。`YearBlock::upsert` 覆盖已有条目或插入新日期。
- 当一批年度输入本身含有至少 192 个有效日期时，可直接采用稠密表示。多次写入累计达到阈值也会触发转换。`YearBlock::promote` 先分配并填充新块，再替换原表示。

`Impl::write_factor` 先将新因子写入临时对象，再登记到目录。`Factor::write_block` 在新块写入失败时撤销该块的登记。内存分配失败时，整个批次中已完成的其他写入可能保留。异常行为以[接口文档](api.md)为准。

## 批量查询

1. `query` 取得结果形状，检查维度乘积和缓冲区容量。空轴返回保留原始轴长度的空结果。
2. 日期按年份分组，每个条目保留原始日期下标。每个合约的内部 ID 也按调用方的位置保存。
3. 查询遍历因子和合约，对每个访问到的年份定位一次 `YearBlock`。稀疏块通过二分查找读取，稠密块先检查有效位，再读数组槽。
4. 按原位置填充连续结果数组，同时设置对应的有效位。查询结果持有自己的缓冲区，实例销毁后仍能读取它。

## 构建和验证

项目要求 CMake 3.16 或更高版本以及支持 C++17 的编译器。以下命令适用于 Linux 和 WSL。将 `build-release` 换成一个新目录即可做干净构建。

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DFACTOR_STORE_BUILD_BENCHMARKS=ON
cmake --build build-release --parallel
(cd build-release && ctest --output-on-failure)
```

- `factor_store_smoke_test` 检查库及公开头文件是否可链接和使用。
- `factor_store_unit_tests` 覆盖五个接口，重点检查闰年和跨年、参数校验后的状态、删除后同名重建，以及三条稀疏转稠密路径。固定种子的 500 步随机序列会与简单的三元组映射逐步比对。
- `factor_store_benchmark_smoke_test` 检查场景、规模、缺失率和 CSV 输出，耗时由独立的性能基准记录。此项在单元测试和性能基准同时启用时注册。

使用[接口示例](api.md#完整示例)验证调用方编译和链接。复跑性能基准时使用 Release 构建，CSV 数据、复现命令和布局对照见[性能基准报告](benchmarks.md)。
