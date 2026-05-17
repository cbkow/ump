# QCView packaging — MSIX

QCView ships as an MSIX package on Windows. The same artifact serves two distribution flows:

1. **Microsoft Store** — submit the unsigned MSIX to Partner Center; Microsoft signs it centrally before publishing.
2. **Signed sideload** — sign the MSIX locally with a code-signing certificate and host the `.msix` (and the certificate's public `.cer`) somewhere users can grab them. Users install the `.cer` into `Trusted People` (LocalMachine) and double-click the `.msix`.

There is no longer an Inno Setup `.exe` installer or any "unsigned dev" path — every release is signed.

## Layout

```
installer/
├── README.md                  ← this file
├── CMakeLists.txt             ← adds the qcview_msix target
├── msix/
│   ├── AppxManifest.xml.in    ← source-of-truth manifest (CMake-substituted)
│   ├── build_msix.ps1         ← PowerShell driver (called by CMake)
│   ├── images/                ← tile PNGs (regenerated from assets/icons/qcview.png)
│   └── dist/                  ← packed .msix lands here (gitignored)
└── sideload/                  ← signing cert (.pfx + .cer) — gitignored
    ├── QCView-dev.cer         ← public cert; ship next to the .msix for end-user trust
    └── QCView-dev.pfx         ← private signing key; never commit
```

The `sideload/` directory is `.gitignore`d in full. Cert files live on each
build machine locally and aren't part of the repo. Provision a fresh checkout
by copying both files in from secure storage.

The dev cert subject is `CN=C64A1D41-EFAA-4EED-AE81-0C074867D4E9`, matching
the `Publisher` field in `AppxManifest.xml.in`. Signed sideload MSIX produced
with this cert will **upgrade in place** over any previously-installed sideload
of QCView v1.x (same Identity Name + Publisher).

Identity values (`Name`, `Publisher`, `PublisherDisplayName`) are set in `installer/CMakeLists.txt` as cache variables defaulted to the project's Microsoft Partner Center registration — override with `-D` if you fork.

Version comes from CMake's top-level `project(qcview VERSION X.Y.Z)`; the MSIX manifest's 4-component version is `X.Y.Z.0`. Bump the version in one place (`CMakeLists.txt`) and the manifest, the about-dialog, and the output filename all follow.

## Build

Configure once (does the manifest substitution):

```powershell
cmake --preset windows-release
```

Pack:

```powershell
cmake --build --preset windows-release --target qcview_msix
```

Output: `installer/msix/dist/QCView-${PROJECT_VERSION}-win64.msix`.

The `qcview_msix` target depends on the main `qcview` target, so an out-of-date binary is rebuilt before packaging.

To wipe the output dir:

```powershell
cmake --build --preset windows-release --target qcview_msix_clean
```

## Sign

Set the following environment variables before invoking the `qcview_msix` target to sign the resulting MSIX inline:

```powershell
$env:QCV_SIGNING_PFX      = ".\installer\sideload\QCView-dev.pfx"
$env:QCV_SIGNING_PASSWORD = "..."                               # if PFX is password-protected
$env:QCV_TIMESTAMP_URL    = "http://timestamp.digicert.com"     # optional but recommended
cmake --build --preset release --target qcview_msix
```

The PFX's certificate subject must exactly match the `Publisher` field in `AppxManifest.xml.in` (i.e. the `QCV_MSIX_PUBLISHER` CMake cache var). Mismatch → `signtool` will succeed but Windows will refuse to install the package.

When `QCV_SIGNING_PFX` is unset, the MSIX is produced **unsigned** — that's the correct artifact for Microsoft Store submission (Microsoft signs centrally).

## Submit to Microsoft Store

1. Build the unsigned MSIX (don't set `QCV_SIGNING_PFX`).
2. Upload `installer/msix/dist/QCView-${PROJECT_VERSION}-win64.msix` to Partner Center.
3. Microsoft signs it before publishing — the `Publisher` field in the manifest must match the publisher CN on the Partner Center account.

## Distribute as signed sideload

1. Build with signing env vars set.
2. Host `QCView-${PROJECT_VERSION}-win64.msix` and the certificate's **public** `.cer` somewhere accessible.
3. Users:
   - Right-click `.cer` → Install Certificate → Local Machine → Trusted People.
   - Double-click `.msix` → "Install".

## Prerequisites

- **Windows 10 SDK** — provides `MakeAppx.exe`, `MakePri.exe`, `signtool.exe`. The build script auto-discovers them under `${ProgramFiles(x86)}\Windows Kits\10\bin\<sdkver>\x64\`.
- **PowerShell 7+** preferred (`winget install Microsoft.PowerShell`); legacy `powershell.exe` also works.
- **ImageMagick** — only needed if you regenerate the tile PNGs; the `installer/msix/images/` set already in git was generated from `assets/icons/qcview.png` via:

  ```sh
  for sz in 16 24 32 44 48 256; do
    magick assets/icons/qcview.png -resize ${sz}x${sz} \
      installer/msix/images/Square44x44Logo.targetsize-${sz}.png
    magick assets/icons/qcview.png -resize ${sz}x${sz} \
      installer/msix/images/Square44x44Logo.targetsize-${sz}_altform-unplated.png
  done
  magick assets/icons/qcview.png -resize 50x50    installer/msix/images/StoreLogo.png
  magick assets/icons/qcview.png -resize 150x150  installer/msix/images/Square150x150Logo.png
  magick assets/icons/qcview.png -resize 44x44    installer/msix/images/Square44x44Logo.png
  magick assets/icons/qcview.png -resize 150x150 -background none -gravity center -extent 310x150 \
      installer/msix/images/Wide310x150Logo.png
  ```

  Re-run when the brand icon changes.
