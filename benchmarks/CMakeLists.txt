add_executable(factor_store_benchmarks
    factor_store_benchmark.cpp
)

target_link_libraries(factor_store_benchmarks PRIVATE FactorStore::factor_store)

