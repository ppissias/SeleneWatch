# Refresh on every build; only change the header when the revision changes.
# This also catches edits or commits made without rerunning configuration.
execute_process(COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${SELENEWATCH_SOURCE_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT result EQUAL 0 OR NOT version)
    set(version "unknown")
endif()
string(REPLACE "\\" "\\\\" version "${version}")
string(REPLACE "\"" "\\\"" version "${version}")
set(content "#pragma once\n#define SELENEWATCH_VERSION \"${version}\"\n")
set(previous "")
if(EXISTS "${SELENEWATCH_VERSION_HEADER}")
    file(READ "${SELENEWATCH_VERSION_HEADER}" previous)
endif()
if(NOT previous STREQUAL content)
    file(WRITE "${SELENEWATCH_VERSION_HEADER}" "${content}")
endif()
