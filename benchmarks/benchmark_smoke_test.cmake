execute_process(
    COMMAND "${BENCHMARK}" --profile small --samples 3
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Benchmark exited with ${status}: ${errors}")
endif()

set(expected_rows
    "year-new,small,3,252,0,-"
    "year-overwrite,small,3,252,252,-"
    "year-backfill,small,3,63,189,-"
    "day-new,small,3,512,0,-"
    "day-second,small,3,512,1,-"
    "day-append,small,3,512,64,-"
    "day-overwrite,small,3,512,64,-"
    "day-threshold,small,3,512,191,-"
    "query-history,small,3,32256,252,0"
    "query-day,small,3,512,1,0"
    "query-mixed-sparse,small,3,32768,32,50"
    "query-mixed-dense,small,3,32768,224,50"
    "query-mixed-dense-25,small,3,32768,224,25"
    "query-promotion-pending,small,3,16384,192,50"
    "query-promotion-drained,small,3,16384,192,50"
    "query-dense-calendar,small,3,65536,224,12"
    "query-dense-weekdays,small,3,65536,224,12"
    "query-dense-shuffled,small,3,65536,224,12"
)
string(REPLACE "\r" "" output "${output}")
foreach(row IN LISTS expected_rows)
    if(NOT output MATCHES "(^|\n)${row},[0-9.]+,[0-9.]+,[0-9.]+,NA\n")
        message(FATAL_ERROR "Missing or malformed benchmark row: ${row}\n${output}")
    endif()
endforeach()

execute_process(
    COMMAND "${BENCHMARK}" --case day-new --profile large --samples 3
    RESULT_VARIABLE isolated_status
    OUTPUT_VARIABLE isolated_output
)
if(PLATFORM STREQUAL "Linux")
    set(rss "[1-9][0-9]*")
else()
    set(rss "NA")
endif()
string(REPLACE "\r" "" isolated_output "${isolated_output}")
if(NOT isolated_status EQUAL 0 OR NOT isolated_output MATCHES
    "^[^\n]+\nday-new,large,3,6144,0,-,[0-9.]+,[0-9.]+,[0-9.]+,${rss}\n$")
    message(FATAL_ERROR "Unexpected isolated benchmark output: ${isolated_output}")
endif()

foreach(scenario IN ITEMS concurrent-mixed concurrent-disjoint concurrent-dense concurrent-day)
    set(read_cells 16384)
    set(stored_days 64)
    if(scenario STREQUAL "concurrent-dense")
        set(stored_days 224)
    elseif(scenario STREQUAL "concurrent-day")
        set(read_cells 512)
        set(stored_days 1)
    endif()
    set(concurrent_rows
        "concurrent-read-alone,small,3,${read_cells},${stored_days},0"
        "concurrent-write-alone,small,3,512,${stored_days},-"
        "concurrent-read-mixed,small,6,${read_cells},${stored_days},0"
        "concurrent-write-mixed,small,3,512,${stored_days},-"
    )
    execute_process(
        COMMAND "${BENCHMARK}" --case "${scenario}" --profile small --samples 3
        RESULT_VARIABLE concurrent_status
        OUTPUT_VARIABLE concurrent_output
        TIMEOUT 10
    )
    string(REPLACE "\r" "" concurrent_output "${concurrent_output}")
    foreach(row IN LISTS concurrent_rows)
        if(NOT concurrent_status EQUAL 0 OR NOT concurrent_output MATCHES
            "(^|\n)${row},[0-9.]+,[0-9.]+,[0-9.]+,${rss}\n")
            message(FATAL_ERROR "Missing ${scenario} row: ${row}\n${concurrent_output}")
        endif()
    endforeach()
endforeach()

execute_process(
    COMMAND "${BENCHMARK}" --case unknown
    RESULT_VARIABLE invalid_status
    ERROR_VARIABLE invalid_error
    OUTPUT_QUIET
)
if(NOT invalid_status EQUAL 2 OR NOT invalid_error MATCHES "unknown case")
    message(FATAL_ERROR "Expected an error for an unknown case")
endif()
