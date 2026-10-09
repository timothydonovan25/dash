# Writes today's date to OUT, touching it only when it changed.
string(TIMESTAMP today "%Y-%m-%d")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL today)
    file(WRITE "${OUT}" "${today}")
endif()
