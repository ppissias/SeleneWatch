# Read by CPack once per package type (CPACK_PROJECT_CONFIG_FILE).
# The .zip unpacks into a folder named after the release; the installer puts
# the files straight into its install folder, the same for every release, so
# an upgrade replaces the program and keeps the events folder beside it.
if(CPACK_GENERATOR STREQUAL "WIX")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
endif()
