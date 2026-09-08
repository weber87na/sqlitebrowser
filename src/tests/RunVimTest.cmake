# Qt GUI test logging on Windows may bypass CTest's stdout capture.
# Read an explicit text log while preserving the actual test exit status.
set(test_log "${CMAKE_CURRENT_BINARY_DIR}/vim-input-results.txt")
file(REMOVE "${test_log}")
execute_process(COMMAND "${TEST_EXECUTABLE}" -v1 -o "${test_log},txt"
    RESULT_VARIABLE test_result TIMEOUT 60)
if(EXISTS "${test_log}")
    file(READ "${test_log}" test_output)
    message("${test_output}")
endif()
if(NOT "${test_result}" STREQUAL "0")
    message(FATAL_ERROR "Vim tests failed: ${test_result}")
endif()
