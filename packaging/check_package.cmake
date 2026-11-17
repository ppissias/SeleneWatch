# Run by CPack after each package type is made (CPACK_POST_BUILD_SCRIPTS).
# Checks the .zip with packaging/check_package.ps1 and prints every package
# with its SHA-256. A failed check fails the packaging, from the command line
# and from Visual Studio alike.
foreach(package IN LISTS CPACK_PACKAGE_FILES)
    if(package MATCHES "\\.zip$")
        execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CPACK_SELENEWATCH_CHECK_SCRIPT}" -Zip "${package}"
                -Dumpbin "${CPACK_SELENEWATCH_DUMPBIN}" -QtPrefix "${CPACK_SELENEWATCH_QT_PREFIX}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "The package check failed for ${package}:\n${output}")
        endif()
        message(STATUS "${output}")
    endif()
    # CPack checks the package where it made it, before copying it to
    # CPACK_PACKAGE_DIRECTORY, so only the name is printed.
    file(SHA256 "${package}" hash)
    file(SIZE "${package}" bytes)
    math(EXPR megabytes "${bytes} / 1048576")
    get_filename_component(name "${package}" NAME)
    message(STATUS "Package ${name}: ${megabytes} MB, sha256 ${hash}")
endforeach()
