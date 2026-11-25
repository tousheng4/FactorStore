cmake_minimum_required(VERSION 3.16)

project(
    FactorStore
    VERSION 0.1.0
    DESCRIPTION "High-performance factor storage for quantitative systems"
    LANGUAGES CXX
)

option(FACTOR_STORE_BUILD_TESTS "Build FactorStore unit tests" ON)
option(FACTOR_STORE_BUILD_BENCHMARKS "Build FactorStore benchmarks" OFF)

add_library(factor_store
    src/factor_store.cpp
)
add_library(FactorStore::factor_store ALIAS factor_store)

target_compile_features(factor_store PUBLIC cxx_std_17)
target_include_directories(
    factor_store
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(factor_store PRIVATE /W4 /permissive-)
else()
    target_compile_options(factor_store PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FACTOR_STORE_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FACTOR_STORE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

