# FactorStore 代码说明

## 源码在哪

| 文件 | 里面有什么 |
| --- | --- |
| [factor_store.h](../include/factor_store/factor_store.h) | 五个公开接口和查询结果 |
| [factor_store.cpp](../src/factor_store.cpp) | 创建实例、因子计数和删除 |
| [factor_store_write.cpp](../src/factor_store_write.cpp) | 年度写入、单日写入和块转换队列 |
| [factor_store_query.cpp](../src/factor_store_query.cpp) | 整理请求、找块和填结果 |
| [year_block.h](../src/year_block.h) 与 [year_block.cpp](../src/year_block.cpp) | 年度块的三种存法，以及块内的读写和转换 |
| [factor_store_impl.h](../src/factor_store_impl.h) | 因子目录、名称索引和锁 |
| [store_types.h](../src/store_types.h) 与 [query_plan.h](../src/query_plan.h) | 日期校验、内部 ID 和查询用的数据结构 |

## 1. 批量插入某个合约某一年的某个因子

`upsert_year` 先检查日期是否属于指定年份，再把日期排序。代码用合约 ID 和年份找到一个块，由 `YearBlock::merge` 合并新旧数据，重复日期取新值。稠密块直接改对应日期的槽。

## 2. 批量插入某天一批合约的一批因子

`upsert_day` 检查值的数量，按合约算出块键，然后依次处理每个因子。`YearBlock::upsert` 负责更新当天的值。日期比块内最后一天晚时，直接追加到稀疏数组末尾。

日写后，`PromotionQueue` 转换一部分达到 192 日的块。每写入 16 个值，可转换一个块，向上取整，每批最多 256 个。遇到目标因子正在读写时，把该块放回队尾，后续日写继续处理。

## 3. 查询当前总共有多少个因子

`factor_count` 直接返回 `Impl::factors.size()`。新因子写入数据后才登记到目录。

## 4. 删除某个因子

`erase_factor` 移除名称索引并销毁该因子的块，待转换的块同时脱离队列。目录有空位时，用最后一个因子补上，再更新它的 ID。

## 5. 根据合约列表、日期列表和因子列表查询数据

1. `query` 检查结果大小。单日查询直接用年份找块，多日查询先按年整理日期，记下原来的位置。
2. 按合约和年份找块。某因子的已有块较少、预计能省下足够多的查找时，改为遍历这个因子的块并匹配请求。
3. `YearBlock` 读取匹配的日期，把值和有效位写回请求中的位置。

稠密块可以成段复制连续的自然日，最短为 5 日。稀疏块也可以成段复制，条件是同年至少 16 个递增的请求日期与存储中的一段完全对上，而且输出位置连续。其余稀疏查询按请求日期多少选择二分查找或顺序合并。

## 锁和错误处理

先取得目录锁，再按因子 ID 顺序取得因子锁。一次查询或批量写入会先锁齐要访问的因子。转换队列只在入队和取任务等操作时加队列锁，转换数据时锁目标因子。

写新块失败时，代码会撤销这个块的登记。日写后转换遇到内存不足，原来的稀疏块可以继续读取，转换任务留给以后处理。

## 构建和测试

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DFACTOR_STORE_BUILD_BENCHMARKS=ON
cmake --build build-release --parallel
(cd build-release && ctest --output-on-failure)
```

[功能测试](../tests/factor_store_test.cpp) · [并发测试](../tests/factor_store_concurrency_test.cpp) · [性能基准](benchmarks.md)
