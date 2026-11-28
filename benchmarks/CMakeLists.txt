add_executable(factor_store_benchmarks
    factor_store_benchmark.cpp
)

target_link_libraries(factor_store_benchmarks PRIVATE FactorStore::factor_store)

if(FACTOR_STORE_BUILD_TESTS)
    add_test(
        NAME factor_store_benchmark_smoke_test
        COMMAND ${CMAKE_COMMAND}
            -DBENCHMARK=$<TARGET_FILE:factor_store_benchmarks>
            -DPLATFORM=${CMAKE_SYSTEM_NAME}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/benchmark_smoke_test.cmake
    )
endif()

