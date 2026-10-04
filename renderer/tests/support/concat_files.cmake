# Writes the files named after `--` to OUTPUT, byte for byte, in order:
#   cmake -DOUTPUT=<file> -P concat_files.cmake -- <part>...
# A script rather than `cmake -E cat ... > OUTPUT`, which needs a shell, and so a build tree whose
# path the shell would read as syntax.
set(parts "")
set(after_dashes FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE ${last})
    if(after_dashes)
        list(APPEND parts "${CMAKE_ARGV${i}}")
    elseif(CMAKE_ARGV${i} STREQUAL "--")
        set(after_dashes TRUE)
    endif()
endforeach()
if(NOT parts OR NOT OUTPUT)
    message(FATAL_ERROR "concat_files.cmake: needs -DOUTPUT=<file> and at least one part after --")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E cat ${parts} OUTPUT_FILE "${OUTPUT}.tmp" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    file(REMOVE "${OUTPUT}.tmp")
    message(FATAL_ERROR "concat_files.cmake: joining ${parts} failed: ${result}")
endif()
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
