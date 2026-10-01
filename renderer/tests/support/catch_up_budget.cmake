# Writes OUTPUT defining WALLPAPERX_CATCH_UP_PICTURES as SOURCE's kCatchUpPictures, so the pipeline probe
# grades its fixtures against the decoder's own budget rather than a copy of it. The constant is private to
# the decoder's implementation file; reading it here keeps test surface out of the production header.
file(STRINGS "${SOURCE}" DEFINITION REGEX "^constexpr int64_t kCatchUpPictures = [0-9]+;$")
list(LENGTH DEFINITION FOUND)
if(NOT FOUND EQUAL 1)
    message(FATAL_ERROR "catch_up_budget.cmake: ${SOURCE} no longer defines kCatchUpPictures on one line as "
                        "'constexpr int64_t kCatchUpPictures = N;' -- update this script with it")
endif()
string(REGEX MATCH "= ([0-9]+)" ASSIGNMENT "${DEFINITION}")
set(PICTURES "${CMAKE_MATCH_1}")
file(WRITE "${OUTPUT}.tmp" "#pragma once\n#define WALLPAPERX_CATCH_UP_PICTURES ${PICTURES}\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
