# installer/CMakeLists.txt — Windows MSIX packaging
#
# Two CMake targets are added on Windows:
#   - qcview_msix          : packs (and optionally signs) the staged
#                            build-release/qcview.exe + DLLs + assets into
#                            installer/msix/dist/QCView-${PROJECT_VERSION}-win64.msix
#   - qcview_msix_clean    : wipes the dist/ output
#
# Identity fields (Name / Publisher) come from cache variables so contributors
# without Store credentials don't have to know them; the defaults match the
# project's Microsoft Partner Center registration.

if(NOT WIN32)
    return()
endif()

# ---- MSIX identity (overridable via -D for store-credentialed builds) ----
set(QCV_MSIX_IDENTITY                "cbkow.QCView"
    CACHE STRING "MSIX Identity/Name — must match the value in Microsoft Partner Center for Store submissions.")
set(QCV_MSIX_PUBLISHER               "CN=C64A1D41-EFAA-4EED-AE81-0C074867D4E9"
    CACHE STRING "MSIX Identity/Publisher — must match the publisher CN registered with Microsoft Partner Center.")
set(QCV_MSIX_PUBLISHER_DISPLAY_NAME  "cbkow"
    CACHE STRING "Human-readable publisher name shown in the Store / Settings > Apps list.")

# MSIX Version requires 4 components; PROJECT_VERSION is X.Y.Z, pad with .0.
set(QCV_MSIX_VERSION "${PROJECT_VERSION}.0")

# Substitute the manifest in place — staged into the build dir so the source
# tree stays clean. The .in file is the source of truth in git; the resolved
# AppxManifest.xml is regenerated whenever CMake reconfigures.
set(_manifest_in  "${CMAKE_CURRENT_SOURCE_DIR}/msix/AppxManifest.xml.in")
set(_manifest_out "${CMAKE_CURRENT_BINARY_DIR}/msix/AppxManifest.xml")
configure_file("${_manifest_in}" "${_manifest_out}" @ONLY)

# Locate PowerShell — pwsh first (PowerShell 7+, preferred), then powershell.exe.
find_program(QCV_PWSH NAMES pwsh powershell)
if(NOT QCV_PWSH)
    message(WARNING
        "Neither pwsh nor powershell found on PATH — qcview_msix target will not be available. "
        "Install PowerShell 7 (winget install Microsoft.PowerShell) and re-run CMake configure.")
    return()
endif()

# Resolve windeployqt so build_msix.ps1 can deploy the Qt runtime before
# packing. Nothing else in the build stages Qt (vcpkg's applocal POST_BUILD
# only deploys vcpkg-managed DLLs; Qt comes from the Qt installer, not
# vcpkg), so without this the MSIX ships no Qt6 DLLs/plugins and won't
# launch. Prefer the imported tool target; fall back to a path search
# rooted at the Qt package dir.
if(TARGET Qt6::windeployqt)
    set(_qcv_windeployqt "$<TARGET_FILE:Qt6::windeployqt>")
else()
    find_program(QCV_WINDEPLOYQT NAMES windeployqt
        HINTS "${Qt6_DIR}/../../../bin" "${QT6_INSTALL_PREFIX}/bin")
    if(NOT QCV_WINDEPLOYQT)
        message(WARNING
            "windeployqt not found — qcview_msix would produce a Qt-less, non-launching MSIX. "
            "Ensure Qt6_DIR points at a full Qt install and re-run configure.")
    endif()
    set(_qcv_windeployqt "${QCV_WINDEPLOYQT}")
endif()

# ---- qcview_msix ----
add_custom_target(qcview_msix
    COMMAND "${QCV_PWSH}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/msix/build_msix.ps1"
        -BuildDir    "$<TARGET_FILE_DIR:qcview>"
        -Version     "${PROJECT_VERSION}"
        -ManifestPath "${_manifest_out}"
        -ImagesDir   "${CMAKE_CURRENT_SOURCE_DIR}/msix/images"
        -OutputDir   "${CMAKE_CURRENT_SOURCE_DIR}/msix/dist"
        -WinDeployQt "${_qcv_windeployqt}"
        -QmlDir      "${CMAKE_SOURCE_DIR}/src/ui/qml"
        -Config      "$<IF:$<CONFIG:Debug>,debug,release>"
    DEPENDS qcview
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Packaging QCView ${PROJECT_VERSION} as MSIX (output: installer/msix/dist/)"
    VERBATIM
)

# ---- qcview_msix_clean ----
add_custom_target(qcview_msix_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "${CMAKE_CURRENT_SOURCE_DIR}/msix/dist"
        "${CMAKE_CURRENT_SOURCE_DIR}/msix/staging"
    COMMENT "Removing installer/msix/dist/ and any leftover staging/"
    VERBATIM
)
