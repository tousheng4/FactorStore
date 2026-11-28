# FactorStore

FactorStore 是面向量化系统的高性能因子存储项目。一个因子在每个合约、每个交易日对应一个 `double` 值。

## 项目结构

```text
FactorStore/
├── include/          # 对外公开头文件
├── src/              # 核心实现
├── tests/            # 单元测试
├── benchmarks/       # 性能测试
├── docs/             # 设计、接口和代码说明文档
├── CMakeLists.txt    # 顶层构建配置
└── README.md
```

## 构建

项目要求 CMake 3.16 或更高版本，并使用 C++17。

```bash
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
```

性能基准默认关闭。以下命令在 Linux 或 WSL 下构建 Release 版并运行：

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DFACTOR_STORE_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/benchmarks/factor_store_benchmarks --profile large --samples 61
```

场景、测量口径和布局对比见[性能基准报告](docs/benchmarks.md)。

## 首版交付

内存版已实现五项因子存储操作。年度块按有效日期数选择稀疏或稠密表示。使用方法、设计决策和维护入口见[项目文档](docs/README.md)。

