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
    "day-append,small,3,512,64,-"
    "day-overwrite,small,3,512,64,-"
    "day-threshold,small,3,512,191,-"
    "query-history,small,3,32256,252,0"
    "query-day,small,3,512,1,0"
    "query-mixed-sparse,small,3,32768,32,50"
    "query-mixed-dense,small,3,32768,224,50"
    "query-mixed-dense-25,small,3,32768,224,25"
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

execute_process(
    COMMAND "${BENCHMARK}" --case unknown
    RESULT_VARIABLE invalid_status
    ERROR_VARIABLE invalid_error
    OUTPUT_QUIET
)
if(NOT invalid_status EQUAL 2 OR NOT invalid_error MATCHES "unknown case")
    message(FATAL_ERROR "Expected an error for an unknown case")
endif()
