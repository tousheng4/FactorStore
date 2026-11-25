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
ctest --test-dir build --output-on-failure
```

性能测试默认关闭，可按需启用：

```bash
cmake -S . -B build-bench -DFACTOR_STORE_BUILD_BENCHMARKS=ON
cmake --build build-bench
```

## 当前进度

内存版已实现五项因子存储操作，测试覆盖批量覆盖、跨年查询、非法批次、删除和同名重建。用法与运行示例见[接口文档](docs/api.md)，后续性能基准与优化见[实现路线](docs/implementation-roadmap.md)。

