add_executable(factor_store_tests
    factor_store_smoke_test.cpp
)

target_link_libraries(factor_store_tests PRIVATE FactorStore::factor_store)

add_test(NAME factor_store_smoke_test COMMAND factor_store_tests)

add_executable(factor_store_unit_tests factor_store_test.cpp)
target_link_libraries(factor_store_unit_tests PRIVATE FactorStore::factor_store)
add_test(NAME factor_store_unit_tests COMMAND factor_store_unit_tests)

