# Release packaging: what a release contains, and the two packages made from
# it, a .zip to unpack and run and a per-user Windows installer (.msi).
# Included from CMakeLists.txt for the Release build only. Both packages come
# from the "package" target, which the "Release-packages" build preset builds
# from the command line or from Visual Studio (DEVELOPER.md, "Building a release").
#
# The installed tree, flat beside SeleneWatch.exe:
#   SeleneWatch.exe, the Qt DLLs and plugins (windeployqt), the Visual C++
#   runtime DLLs, ASICamera2.dll when built with the ZWO SDK, LICENSE.txt,
#   README.md, MANUAL.md, CAMERA_SETTINGS_GUIDE.md, THIRD_PARTY_LICENSES.md,
#   icons/selenewatch-256.png, docs/images/, and licenses/.
# Tests, probes and QHY files are not installed: the observer installs QHY's
# own runtime (THIRD_PARTY_LICENSES.md).

install(TARGETS SeleneWatch RUNTIME DESTINATION .)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_CURRENT_SOURCE_DIR}/MANUAL.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/CAMERA_SETTINGS_GUIDE.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md"
    DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/selenewatch-256.png" DESTINATION icons)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/images" DESTINATION docs)

# Qt is used under the LGPLv3: its license texts, and the notices of the
# third-party code inside the Qt DLLs we ship (packaging/make_qt_notices.ps1).
set(SELENEWATCH_QT_LICENSE_FILE "${QT6_INSTALL_PREFIX}/../../Licenses/LICENSE" CACHE FILEPATH
    "Qt's LICENSE file (the LGPLv3 and GPLv3 texts), from the Licenses folder of the Qt installation")
install(FILES "${SELENEWATCH_QT_LICENSE_FILE}" DESTINATION licenses RENAME Qt-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/licenses/Qt-third-party-notices.txt" DESTINATION licenses)
# Feature-47: Project Nayuki's QR code generator (MIT), compiled into SeleneWatch.exe.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen/LICENSE.txt" DESTINATION licenses RENAME QR-Code-generator-LICENSE.txt)

if(SELENEWATCH_ZWO_SDK_DIR)
    install(FILES "${SELENEWATCH_ZWO_SDK_DIR}/lib/x64/ASICamera2.dll" DESTINATION .)
    install(FILES "${SELENEWATCH_ZWO_SDK_DIR}/license.txt" DESTINATION licenses RENAME ZWO-ASI-SDK.txt)
endif()

if(TARGET opencv_world)
    install(FILES "$<TARGET_FILE:opencv_world>" DESTINATION .)
endif()
if(SELENEWATCH_OPENCV_DIR)
    file(TO_CMAKE_PATH "${SELENEWATCH_OPENCV_DIR}" _opencv_pkg_dir)
    if(EXISTS "${_opencv_pkg_dir}/../LICENSE.txt")
        install(FILES "${_opencv_pkg_dir}/../LICENSE.txt" DESTINATION licenses RENAME OpenCV-LICENSE.txt)
    endif()
endif()

if(SELENEWATCH_CFITSIO_DIR)
    file(TO_CMAKE_PATH "${SELENEWATCH_CFITSIO_DIR}" _cfitsio_pkg_dir)
    if(EXISTS "${_cfitsio_pkg_dir}/bin/cfitsio.dll")
        install(FILES "${_cfitsio_pkg_dir}/bin/cfitsio.dll" DESTINATION .)
    endif()
    if(EXISTS "${_cfitsio_pkg_dir}/License.txt")
        install(FILES "${_cfitsio_pkg_dir}/License.txt" DESTINATION licenses RENAME CFITSIO-LICENSE.txt)
    endif()
endif()

if(SELENEWATCH_FFMPEG_DIR)
    file(TO_CMAKE_PATH "${SELENEWATCH_FFMPEG_DIR}" _ffmpeg_pkg_dir)
    foreach(_ff_file
        ffmpeg.exe avcodec-63.dll avdevice-63.dll avfilter-12.dll
        avformat-63.dll avutil-61.dll swresample-7.dll swscale-10.dll
    )
        if(EXISTS "${_ffmpeg_pkg_dir}/bin/${_ff_file}")
            install(FILES "${_ffmpeg_pkg_dir}/bin/${_ff_file}" DESTINATION .)
        endif()
    endforeach()
    if(EXISTS "${_ffmpeg_pkg_dir}/LICENSE.txt")
        install(FILES "${_ffmpeg_pkg_dir}/LICENSE.txt" DESTINATION licenses RENAME FFmpeg-LICENSE.txt)
    endif()
endif()

# The Visual C++ runtime beside the executable, so no separate installer is
# needed. The Universal CRT is part of Windows 10 and 11.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)

# The Qt DLLs and plugins the executable needs, found by windeployqt in the
# installed tree. No translations, software OpenGL or shader compilers: the
# window draws with Qt's raster engine. No touch input over the network, SVG
# icons or images, or network status plugins. Qt Network and the Windows
# Schannel TLS backend provide the startup release check without OpenSSL.
find_program(SELENEWATCH_WINDEPLOYQT windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin" REQUIRED)
install(CODE "
    set(prefix \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
    execute_process(COMMAND \"${SELENEWATCH_WINDEPLOYQT}\" --release
            --no-compiler-runtime --no-translations --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler
            --skip-plugin-types generic,iconengines,networkinformation --exclude-plugins qsvg,qopensslbackend,qcertonlybackend
            --dir \"\${prefix}\" \"\${prefix}/SeleneWatch.exe\"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR \"windeployqt failed: \${output}\")
    endif()
")

# The packages, in <build>/package. MSI versions need a major number below
# 256, so the installer carries 26.11.1 for 2026.11-01; the file names carry
# the release itself.
string(REGEX REPLACE "^20([0-9][0-9])\\.([0-9]+)\\.([0-9]+)$" "\\1.\\2.\\3" selenewatch_msi_version "${PROJECT_VERSION}")
set(CPACK_PACKAGE_NAME "SeleneWatch")
set(CPACK_PACKAGE_VENDOR "SeleneWatch")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SeleneWatch - Sky, Moon & Planetary Monitor")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ppissias/SeleneWatch")
set(CPACK_PACKAGE_VERSION "${selenewatch_msi_version}")
set(CPACK_PACKAGE_FILE_NAME "SeleneWatch-${SELENEWATCH_RELEASE}-win64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SeleneWatch")
set(CPACK_PACKAGE_EXECUTABLES "SeleneWatch" "SeleneWatch")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/cpack_options.cmake")

# The installer needs the WiX Toolset 3 binaries: SELENEWATCH_WIX_ROOT (set
# by the Release preset on the maintainer's machine) or the WIX environment
# variable, a folder holding bin/candle.exe. Without them only the .zip is made.
set(SELENEWATCH_WIX_ROOT "$ENV{WIX}" CACHE PATH "WiX Toolset 3 folder holding bin/candle.exe, for the installer")
if(SELENEWATCH_WIX_ROOT AND EXISTS "${SELENEWATCH_WIX_ROOT}/bin/candle.exe")
    set(CPACK_GENERATOR "ZIP;WIX")
    file(TO_CMAKE_PATH "${SELENEWATCH_WIX_ROOT}" CPACK_WIX_ROOT)
else()
    set(CPACK_GENERATOR "ZIP")
    message(STATUS "No WiX Toolset 3 found (SELENEWATCH_WIX_ROOT or WIX): packaging makes the .zip only")
endif()

# WiX Toolset 3: per user, so no administrator rights are needed and the
# events folder beside SeleneWatch.exe can be written. The upgrade code stays
# the same for every release, so a newer installer replaces an older one.
set(CPACK_WIX_VERSION 3)
set(CPACK_WIX_INSTALL_SCOPE "perUser")
set(CPACK_WIX_UPGRADE_GUID "36317192-6891-4155-AEDE-BD38C30C49DF")
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icons/selenewatch.ico")
set(CPACK_WIX_UI_DIALOG "${CMAKE_CURRENT_SOURCE_DIR}/packaging/wix_dialog.bmp")
set(CPACK_WIX_UI_BANNER "${CMAKE_CURRENT_SOURCE_DIR}/packaging/wix_banner.bmp")
set(CPACK_WIX_EXTRA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/wix_ui.wxs")
set(CPACK_WIX_UI_REF "SeleneWatch_InstallDir")
set(CPACK_WIX_EXTENSIONS "WixUtilExtension")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "SeleneWatch")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://github.com/ppissias/SeleneWatch")

# After packaging: check the .zip and print each package with its SHA-256.
get_filename_component(selenewatch_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(SELENEWATCH_DUMPBIN dumpbin HINTS "${selenewatch_compiler_dir}" REQUIRED)
set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/check_package.cmake")
set(CPACK_SELENEWATCH_CHECK_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/packaging/check_package.ps1")
set(CPACK_SELENEWATCH_DUMPBIN "${SELENEWATCH_DUMPBIN}")
set(CPACK_SELENEWATCH_QT_PREFIX "${QT6_INSTALL_PREFIX}")
include(CPack)
