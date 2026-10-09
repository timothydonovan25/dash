# Copies BIN to OUTDIR/dash-<version>-<date>-<board>-<feature>.bin, with every
# part read out of the image's esp_app_desc_t, so the name cannot disagree
# with what the dash will show. The app description starts 32 bytes in:
# version at +16 (32 bytes), date at +96 ("Oct  8 2026", 16 bytes).
# A NUL-terminated field of the image as a string.
function(read_cstr offset len out)
    file(READ "${BIN}" hex OFFSET ${offset} LIMIT ${len} HEX)
    set(s "")
    string(LENGTH "${hex}" n)
    set(i 0)
    while(i LESS n)
        string(SUBSTRING "${hex}" ${i} 2 byte)
        if(byte STREQUAL "00")
            break()
        endif()
        math(EXPR code "0x${byte}")
        string(ASCII ${code} c)
        string(APPEND s "${c}")
        math(EXPR i "${i} + 2")
    endwhile()
    set(${out} "${s}" PARENT_SCOPE)
endfunction()

read_cstr(48 32 ver)
read_cstr(128 16 date)

# "Oct  8 2026" -> 2026-10-08
string(REGEX MATCH "^([A-Z][a-z][a-z]) +([0-9]+) ([0-9][0-9][0-9][0-9])" m "${date}")
set(months Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec)
list(FIND months "${CMAKE_MATCH_1}" mi)
if(mi LESS 0)
    message(FATAL_ERROR "dash_name_bin: no build date in ${BIN} ('${date}')")
endif()
math(EXPR mo "${mi} + 1")
set(day "${CMAKE_MATCH_2}")
if(mo LESS 10)
    set(mo "0${mo}")
endif()
if(day LESS 10)
    set(day "0${day}")
endif()
set(iso "${CMAKE_MATCH_3}-${mo}-${day}")

# "1.0 demo Wi-Fi OTA" -> 1.0, demo, wifi-ota
if(NOT ver MATCHES "^([^ ]+) ([^ ]+) (.+)$")
    message(FATAL_ERROR "dash_name_bin: version '${ver}' is not '<version> <board> <feature>'")
endif()
set(v "${CMAKE_MATCH_1}")
set(board "${CMAKE_MATCH_2}")
string(TOLOWER "${CMAKE_MATCH_3}" feature)
# One hyphen between words and none inside them: "Wi-Fi OTA" -> wifi-ota.
string(REGEX REPLACE "[^a-z0-9 ]" "" feature "${feature}")
string(STRIP "${feature}" feature)
string(REGEX REPLACE " +" "-" feature "${feature}")

set(file "dash-${v}-${iso}-${board}-${feature}.bin")
file(COPY_FILE "${BIN}" "${OUTDIR}/${file}")
# And the keeper: every version ever built, in one place.
if(RELEASES)
    file(MAKE_DIRECTORY "${RELEASES}")
    file(COPY_FILE "${BIN}" "${RELEASES}/${file}")
    message(STATUS "Firmware: releases/${file}")
else()
    message(STATUS "Firmware: ${OUTDIR}/${file}")
endif()
