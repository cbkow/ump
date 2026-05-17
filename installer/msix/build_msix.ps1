# build_msix.ps1 -- package the built qcview.exe + DLLs + assets into an MSIX
#
# Invocation (preferred -- via CMake):
#     cmake --build --preset release --target qcview_msix
# Manual:
#     pwsh installer\msix\build_msix.ps1 `
#         -BuildDir build-release `
#         -Version 2.0.1 `
#         -OutputDir installer\msix\dist
#
# Signing
# -------
# Set environment variables before invocation to produce a signed sideload MSIX.
# Store submissions are signed centrally by Microsoft -- leave these unset.
#     $env:QCV_SIGNING_PFX      = "C:\path\to\codesign.pfx"
#     $env:QCV_SIGNING_PASSWORD = "..."     # optional, password-protected PFX
#     $env:QCV_TIMESTAMP_URL    = "http://timestamp.digicert.com"   # optional
#
# Without those vars, the MSIX is produced unsigned (the path for Store
# submission).
#
# This script is ASCII-only on purpose so Windows PowerShell 5.1's
# system-codepage parser handles it correctly without a UTF-8 BOM.

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $BuildDir,
    [Parameter(Mandatory)] [string] $Version,
    [Parameter()]          [string] $OutputDir = (Join-Path $PSScriptRoot "dist"),
    [Parameter()]          [string] $ManifestPath = (Join-Path $PSScriptRoot "AppxManifest.xml"),
    [Parameter()]          [string] $ImagesDir   = (Join-Path $PSScriptRoot "images")
)

$ErrorActionPreference = "Stop"

# --- Resolve SDK tooling --------------------------------------------------
function Find-SdkTool {
    param([string] $name)
    $kits = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    )
    foreach ($root in $kits) {
        if (-not (Test-Path $root)) { continue }
        $hits = Get-ChildItem -Path $root -Recurse -Filter $name -ErrorAction SilentlyContinue `
            | Where-Object { $_.FullName -match "\\x64\\" }
        if ($hits) { return ($hits | Select-Object -First 1).FullName }
    }
    return $null
}

$MakeAppx = Find-SdkTool "MakeAppx.exe"
$MakePri  = Find-SdkTool "MakePri.exe"
$SignTool = Find-SdkTool "signtool.exe"

if (-not $MakeAppx) { throw "MakeAppx.exe not found. Install the Windows 10 SDK." }
if (-not $MakePri)  { throw "MakePri.exe not found. Install the Windows 10 SDK." }

Write-Host "MakeAppx : $MakeAppx"
Write-Host "MakePri  : $MakePri"
if ($SignTool) { Write-Host "SignTool : $SignTool" } else { Write-Host "SignTool : (not found, signing will be skipped)" }

# --- Verify inputs --------------------------------------------------------
$exe = Join-Path $BuildDir "qcview.exe"
if (-not (Test-Path $exe)) {
    throw "qcview.exe not found at $exe. Build first: cmake --build --preset release"
}
if (-not (Test-Path $ManifestPath)) {
    throw "AppxManifest.xml not found at $ManifestPath. Re-run CMake configure to substitute it from the .in template."
}
if (-not (Test-Path $ImagesDir)) {
    throw "Tile images dir not found at $ImagesDir."
}

# --- Stage payload --------------------------------------------------------
$staging = Join-Path $PSScriptRoot "staging"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Path $staging | Out-Null

Write-Host "Staging files into $staging ..."

# Executable, every DLL next to it, the assets/ tree, and Qt plugin subfolders.
Copy-Item -Path $exe -Destination $staging
Get-ChildItem -Path $BuildDir -Filter "*.dll" | Copy-Item -Destination $staging
foreach ($subdir in @("assets", "platforms", "imageformats", "tls", "iconengines",
                       "qmltooling", "qml", "networkinformation", "generic")) {
    $src = Join-Path $BuildDir $subdir
    if (Test-Path $src) {
        Copy-Item -Recurse -Path $src -Destination (Join-Path $staging $subdir)
    }
}

# Manifest + tile images.
Copy-Item -Path $ManifestPath -Destination (Join-Path $staging "AppxManifest.xml")
Copy-Item -Recurse -Path $ImagesDir -Destination (Join-Path $staging "images")

# License + third-party notices ship alongside the binary
# (GPL section 3 source-with-binary requirement).
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
foreach ($leaf in @("LICENSE", "PRIVACY_POLICY.md", "Acknowledgments.md")) {
    $p = Join-Path $repoRoot $leaf
    if (Test-Path $p) { Copy-Item -Path $p -Destination $staging }
}
$thirdParty = Join-Path $repoRoot "LICENSES\THIRD_PARTY_NOTICES.txt"
if (Test-Path $thirdParty) { Copy-Item -Path $thirdParty -Destination $staging }

# --- resources.pri (target-size / scale qualifiers on the tile images) ---
Write-Host "Generating resources.pri ..."
$priConfig = Join-Path $staging "priconfig.xml"
& $MakePri createconfig /cf $priConfig /dq "en-us" /o | Out-Null
& $MakePri new /pr $staging /cf $priConfig `
    /mn (Join-Path $staging "AppxManifest.xml") `
    /of (Join-Path $staging "resources.pri") /o
if ($LASTEXITCODE -ne 0) { throw "MakePri new failed (exit $LASTEXITCODE)" }
Remove-Item -Path $priConfig

# --- Pack -----------------------------------------------------------------
if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }
$output = Join-Path $OutputDir "QCView-${Version}-win64.msix"
if (Test-Path $output) { Remove-Item -Path $output }

Write-Host "Packing $output ..."
& $MakeAppx pack /d $staging /p $output /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx pack failed (exit $LASTEXITCODE)" }

# Clean up staging (unpacked payload is sensitive; keep only the packed .msix).
Remove-Item -Recurse -Force $staging

# --- Sign (optional) ------------------------------------------------------
$pfx = $env:QCV_SIGNING_PFX
if ($pfx -and (Test-Path $pfx)) {
    if (-not $SignTool) {
        Write-Warning "QCV_SIGNING_PFX is set but signtool.exe was not found -- MSIX left unsigned."
    } else {
        Write-Host "Signing $output with $pfx ..."
        $signArgs = @("sign", "/fd", "SHA256", "/f", $pfx)
        if ($env:QCV_SIGNING_PASSWORD) { $signArgs += @("/p", $env:QCV_SIGNING_PASSWORD) }
        if ($env:QCV_TIMESTAMP_URL) { $signArgs += @("/tr", $env:QCV_TIMESTAMP_URL, "/td", "SHA256") }
        $signArgs += $output
        & $SignTool @signArgs
        if ($LASTEXITCODE -ne 0) { throw "signtool sign failed (exit $LASTEXITCODE)" }
        Write-Host "Signed successfully."
    }
} else {
    Write-Host "QCV_SIGNING_PFX not set -- produced unsigned MSIX (suitable for Store submission)."
}

Write-Host ""
Write-Host "SUCCESS: $output"
