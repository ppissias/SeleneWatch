# Third-Party Licenses and Dependencies

SeleneWatch is an open-source project licensed under the **BSD 2-Clause ("Simplified") License** (see [LICENSE](LICENSE)).

This document tracks all external libraries, frameworks, SDKs, and third-party code used in SeleneWatch, documenting their license terms, official sources, and redistribution/compliance obligations.

---

## Active Dependencies

### 1. Qt 6 (Core, Gui, Widgets, Network)

- **Website / Source:** [https://www.qt.io](https://www.qt.io) / [https://code.qt.io](https://code.qt.io)
- **Copyright:** (c) The Qt Company Ltd. and other contributors
- **Current Development Version:** Qt 6.12.0
- **License:** **GNU Lesser General Public License version 3 (LGPLv3)** (also available under GPLv3 and commercial licenses)
- **Used Modules:**
  - `Qt6Core`
  - `Qt6Gui`
  - `Qt6Widgets`
  - `Qt6Network` — the asynchronous public GitHub release check in the UI library (HTTPS uses Qt's Windows Schannel TLS plugin and Windows certificate verification), and the status page for a phone on the local network (`QTcpServer`, Feature-47). Qt's HTTP Server module is not used: its open-source license is GPLv3 only.
  - `Qt6Test` — linked only by the test executables. The application does not link it, and a release of the app does not need `Qt6Test.dll`.
- **Linkage / Integration:** **Dynamic linking** via shared libraries (`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6Network.dll`). The test target also links `Qt6Test.dll`. SeleneWatch does not statically link Qt.
- **Compliance Obligations:**
  - Provide attribution to the Qt Project and copyright holders.
  - Distribute license texts for LGPLv3 and GPLv3 with binary distributions.
  - Allow users to replace Qt shared libraries with custom/compatible builds (satisfied by dynamic DLL linking).
  - Make Qt source code available or provide a written offer/link to the corresponding upstream source code.
  - Avoid using Qt modules that are licensed under GPL-only (e.g., Qt Charts or certain add-on modules) unless approved.

---

### 2. ZWO ASI Camera SDK (optional Windows x64 camera support)

- **Official source:** [ZWO product SDK](https://www.zwoastro.com/software/product-sdk/).
- **Verified version:** SDK 1.41; the installed DLL reports `1, 41, 0, 0` (1.41.0.0) at run time.
- **Copyright:** Copyright (c) 2015, ZWO Company. All rights reserved.
- **License:** MIT wording, as supplied in the SDK root's `license.txt`. The notice permits use, modification and redistribution subject to retaining its copyright and permission notice, including the supplied warranty disclaimer.
- **Integration:** `ASICamera2.h` and the x64 import library are used only when `SELENEWATCH_ZWO_SDK_DIR` is set. The application dynamically loads the linked `ASICamera2.dll`; only `cameras/zwo_sdk.cpp` includes the vendor header. SDK files stay outside this repository. Builds without the SDK have no ASI runtime dependency.
- **Redistribution:** CMake copies the unmodified DLL beside the executables and its complete supplied notice to `licenses/ZWO-ASI-SDK.txt`. Include both when distributing an SDK-enabled build. Do not omit the notice during packaging. The build does not bundle or install a camera driver; any driver redistribution needs its own terms checked.
- **Scope:** No OpenCV, vendor demo code, firmware or guide-port integration is incorporated. SeleneWatch's original code remains BSD-2-Clause. Qt's separate obligations above still apply.

---

### 3. QHYCCD Windows SDK (user-installed runtime)

- **Installed version:** Windows x64 25.6.16.15, confirmed from the file/header and a runtime version-only query.
- **Vendor sources reviewed:** [SDK documentation and demos](https://www.qhyccd.com/sdk-demo-and-sample-project/), [downloads](https://www.qhyccd.com/download/). A [historical SDK repository](https://github.com/qhyccd-lzr/QHYCCD_Linux) was also checked for context, without establishing this Windows package's terms.
- **Terms:** The installed package has no SDK-wide license text. Windows SDK redistribution rights and any required notices remain unverified. The compatibility header refers to its own copyright and license.
- **Integration:** The optional build uses declarations from the externally installed header. At runtime SeleneWatch loads the user's x64 `qhyccd.dll` by absolute path when QHY is selected. QHY documents the All-In-One package as the source for third-party application SDKs. [QHY installation guidance](https://www.qhyccd.com/download/)
- **Distribution:** QHY headers, import libraries, DLLs, drivers, firmware and demos stay out of the repository and SeleneWatch packages. The tested `qhyccd.dll` imports Windows components. Bundling any QHY component would require verifying its terms and notices first. SeleneWatch's original adapter code remains BSD-2-Clause.
- **Validation:** Physical camera qualification is pending. [QHY integration and validation](docs/qhy-development.md)

---

### 4. Player One camera SDK (user-installed runtime, Feature-16)

- **Version used to build:** Windows camera SDK 3.10.1, from [Player One's software page](https://player-one-astronomy.com/service/software/) (`PlayerOne_Camera_SDK_Windows_V3.10.1.zip`, sha256 `428cdf88f14f5a7170c41cc9be5353030dba1cd8c97fdd72ec06d64940e2f7e5`), unpacked outside the repository.
- **Terms:** the SDK's `license.txt` (copyright Player One Astronomy Co., Ltd.) grants use "for the secondary development of our company's cameras or other equipment" and says "You can use our company's products and this SDK to develop any products without any restrictions". It grants nothing explicit about redistribution, and keeps an MIT-style warranty disclaimer.
- **Integration:** only `cameras/playerone_sdk.cpp` includes `PlayerOneCamera.h`, for the function declarations. Nothing links against the SDK. At run time SeleneWatch loads the observer's x64 `PlayerOneCamera.dll` by absolute path, through `cameras/vendor_runtime`.
- **Distribution:** no Player One file is in the repository or the packages. The observer installs Player One's driver and DLL.

---

### 5. ToupTek camera SDK (user-installed runtime, Feature-16)

- **Version used to build:** `toupcamsdk.20261024.zip` (library version 60.33015.20261024), from [ToupTek's SDK downloads](https://www.touptekphotonics.com/download/?category=SDK) (sha256 `8845f9d770e07e301126a8142a4609d8ed5129264f4d7cbdefe62c7913cfc444`), unpacked outside the repository.
- **Terms:** the package holds no license text, and `toupcam.h` names none. Redistribution rights are unknown.
- **Integration:** only `cameras/touptek_sdk.cpp` includes `toupcam.h`, for the declarations, with its import markers off (`TOUPCAM_NOIMPORTS`). Nothing links against the SDK. At run time SeleneWatch loads the observer's x64 `toupcam.dll` by absolute path.
- **Distribution:** no ToupTek file is in the repository or the packages. The observer installs the camera's driver and ToupTek's DLL.

---

### 6. ASCOM Platform (user-installed runtime, mount guiding)

- **Installed version on the development machine:** ASCOM Platform 7.1 Update 3 (7.1.3.4851), with the OmniSim simulator.
- **License:** The ASCOM Platform repository is licensed under Creative Commons Attribution-ShareAlike 3.0 Unported (CC BY-SA 3.0, SPDX `CC-BY-SA-3.0`), except `DriverAccess.XML`, which is MIT. Sources: the [ASCOMPlatform repository page](https://github.com/ASCOMInitiative/ASCOMPlatform) ("The entire ASCOM Platform (as opposed to drivers and programs that use it) is open source...") and its [LicenseExceptions.txt](https://github.com/ASCOMInitiative/ASCOMPlatform/blob/main/LicenseExceptions.txt), read 15 November 2026. No license file ships with the installed Platform 7 in Program Files. The OmniSim simulator ships its own MIT notice (`C:\Program Files\ASCOM\OmniSimulator\license.md`).
- **Integration:** `guiding/ascom_mount.cpp` (CMake option `SELENEWATCH_WITH_ASCOM`, on by default for Windows) uses only Windows COM: `CLSIDFromProgID`, `CoCreateInstance`, and `IDispatch` calls by member name, against the Platform and the driver the observer has installed. No ASCOM header, type library, assembly, or source is copied into the repository, generated into the build, or linked. The build links only the Windows system libraries `ole32` and `oleaut32`.
- **Members used:** the ASCOM Telescope interface's `Connected` (read and write), `CanPulseGuide`, `Tracking`, `Slewing`, `AtPark`, `SideOfPier`, `AlignmentMode`, `GuideRateRightAscension`, `GuideRateDeclination`, `IsPulseGuiding`, and `PulseGuide`; and `ASCOM.Utilities.Chooser`'s `DeviceType` and `Choose` for the driver dialog.
- **Distribution:** SeleneWatch packages contain no ASCOM component. The observer installs the ASCOM Platform and their mount's driver. Read as written, CC BY-SA obligations attach to distributing the Platform or adaptations of it, and the Platform page separates the Platform from "drivers and programs that use it". This is a reading of the license text, not legal advice. Bundling any ASCOM file, or the OmniSim simulator, would need its terms and notices checked first. SeleneWatch's adapter code remains BSD-2-Clause.

---

### 7. Microsoft Visual C++ runtime (shipped in release packages)

- **Files:** `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140*.dll`, `concrt140.dll`, copied unmodified from Visual Studio's redistributable folder (`VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT`, file version 14.51.36247.0) by CMake's `InstallRequiredSystemLibraries`. The Universal CRT is part of Windows 10 and 11 and is not shipped.
- **Terms:** Visual Studio's license lets these files, listed in its `REDIST` list as distributable code, be shipped with an application, unmodified. Qt's DLLs need the same runtime.

---

### 8. WiX Toolset 3 (build tool for the installer, not shipped)

- **Version:** 3.14.1, binaries from the [WiX Toolset v3 releases](https://github.com/wixtoolset/wix3/releases), used by CPack to make the `.msi`.
- **License:** Microsoft Reciprocal License (MS-RL), for the toolset itself (`bin\LICENSE.TXT`). The project's [About page](https://wixtoolset.org/docs/about), read 17 November 2026, states that the packages WiX generates are generally outside the MS-RL, and that the code and binaries placed in them keep their own licenses. No WiX source is copied into SeleneWatch.

---

### 9. QR Code generator library, Project Nayuki (compiled in, Feature-47)

- **Files:** `third_party/qrcodegen/qrcodegen.hpp` and `qrcodegen.cpp`, unmodified from [github.com/nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator), tag `v1.8.0`, directory `cpp/` (SHA-256 `b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793` and `1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757`), read in full before use: standard C++ only, no input or output.
- **License:** MIT. Copyright (c) Project Nayuki.
- **Usage:** draws the QR code of the status page's address in **Session → Status on your phone** (`remote/phone_link`). Compiled into `SeleneWatch.exe` through the static library `selenewatch_qrcodegen`.
- **Obligations:** keep the copyright and permission notice with the source (it heads each file) and with the binaries: `licenses/QR-Code-generator-LICENSE.txt` in the release packages, from `third_party/qrcodegen/LICENSE.txt`. Compatible with BSD-2-Clause.

---

## Release packages: what is shipped and the obligations met

Made by the Release build's `package` target, from the command line or Visual Studio (see [DEVELOPER.md](DEVELOPER.md) in the source repository, *Building a release*):

- **SeleneWatch:** `SeleneWatch.exe`, under BSD-2-Clause (`LICENSE.txt`).
- **Qt 6.12.0, LGPLv3:** `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6Network.dll`, and the plugins `platforms/qwindows.dll`, `styles/qmodernwindowsstyle.dll`, `imageformats/qgif.dll`, `qico.dll`, `qjpeg.dll`, `tls/qschannelbackend.dll`, unmodified from the Qt installation and dynamically linked, so a user can replace them with compatible builds.
  - License texts: `licenses/Qt-LICENSE.txt`, Qt's statement with the LGPLv3 and the GPLv3 it supplements.
  - The third-party code inside those DLLs (zlib, PCRE2, HarfBuzz, FreeType, libpng, libjpeg-turbo, double-conversion and others): `licenses/Qt-third-party-notices.txt`, generated from the Qt documentation by `packaging/make_qt_notices.ps1`.
  - Source: Qt 6.12.0's source code is published by The Qt Company at [download.qt.io](https://download.qt.io/official_releases/qt/6.12/6.12.0/) (moved to `download.qt.io/archive/qt/6.12/6.12.0/` once superseded) and in Git at [code.qt.io](https://code.qt.io/) (tag `v6.12.0`). SeleneWatch uses Qt unmodified; its own source is public at [github.com/ppissias/SeleneWatch](https://github.com/ppissias/SeleneWatch).
  - The About box (Help → About SeleneWatch, Help → About Qt) states that Qt is used under the LGPLv3.
  - Qt Network's additional third-party components are **libpsl**, revision `664f3dc85259ec65e30248a61fa1c45b7b0e4c3f` (BSD-3-Clause, https://github.com/rockdaboot/libpsl), and the **Public Suffix List**, `2026-05-14_08-35-31_UTC` (MPL-2.0, https://publicsuffix.org/). Their complete attribution/license texts are generated from the installed Qt documentation into the same notices file. Preserve the MPL notice and corresponding source availability when redistributing the unmodified list as part of Qt; the Qt source archive above includes it. No original SeleneWatch source is placed under MPL.
  - Qt Svg and the OpenSSL/certificate-only TLS backends are not shipped. The Windows Schannel backend uses Windows system libraries; no OpenSSL binaries are added.
- **ZWO ASI SDK:** `ASICamera2.dll` with `licenses/ZWO-ASI-SDK.txt` (section 2 above), only when the build has the SDK.
- **QR Code generator (Project Nayuki), MIT:** compiled into `SeleneWatch.exe`, with `licenses/QR-Code-generator-LICENSE.txt` (section 9 above).
- **Microsoft Visual C++ runtime:** as above.
- **Not shipped:** QHY, Player One and ToupTek files, ASCOM, camera drivers, test and probe programs.

---

## Camera families evaluated (17 November 2026)

Player One Astronomy, ToupTek (with Altair Astro and other brands that ship the same SDK under their own names), SVBONY and Atik were considered for a third camera family. Player One and ToupTek were integrated as above; see Feature-16 in `features.md`. The license files found with their SDKs, as vendored in the INDI project's `indi-3rdparty` repository and read on 17 November 2026:

- **Player One:** `license.txt` grants use of the SDK "for the secondary development of our company's cameras", says "You can use our company's products and this SDK to develop any products without any restrictions", and grants nothing explicit about redistribution.
- **ToupTek and Altair:** a generic GNU LGPL 2.1 text only, with no vendor statement that it covers the binaries.
- **SVBONY and Atik:** no license file. Atik's SDK changelog says a EULA was added in its 2025.11.24 release; its text was not read.

None of them grants redistribution clearly. Any of them therefore follows the QHY pattern: SDK files stay out of the repository and the packages, and the observer installs the vendor's runtime.

---

### OpenCV 4.14.0 (Shipped — Dynamic Link)
- **Component:** `opencv_world4140.dll` (`opencv_world4140d.dll` in Debug)
- **License:** Apache License 2.0 (`licenses/OpenCV-LICENSE.txt`)
- **Upstream:** https://opencv.org/ (`opencv-4.14.0-windows.exe`, SHA-256 `5f266a8b73bed535962d7e861a6457e32a0dd5f463ad0a7cf8707a135469be63`)
- **Usage:** Demosaicing, phase-correlation burst alignment, rigid Euclidean/ECC lunar field-rotation alignment, linear unsharp masking, and frame resampling in **Time-lapse** mode.
- **Obligations:** Permissive (`Apache-2.0`). Include `licenses/OpenCV-LICENSE.txt` and attribution notice in distributions. Compatible with BSD-2-Clause.

### CFITSIO 4.7.0 (Shipped — Dynamic Link)
- **Component:** `cfitsio.dll`
- **License:** NASA/GSFC permissive open-source license (`licenses/CFITSIO-LICENSE.txt`)
- **Upstream:** https://heasarc.gsfc.nasa.gov/fitsio/ (`cfitsio-4.7.0.zip`, SHA-256 `5fa2107a3567c2769b4ec3f3759380c117d53cc20f89225ddea69c54454823d3`, Release build with static zlib)
- **Usage:** Reading and writing linear 32-bit floating-point FITS stacks (`BITPIX = -32`, 2D mono and 3D plane-sequential R,G,B) and FITS astronomy headers (`DATE-OBS`, `DATE-AVG`, `DATE-END`, `EXPTIME`, `GAIN`, `RAWCOUNT`, `STACKCNT`, `QUALSCOR`, `ALIGNDX`, `ALIGNDY`, `ALIGNROT`, `SHARPPRO`).
- **Obligations:** Permissive. Include `licenses/CFITSIO-LICENSE.txt` with copyright notice and warranty disclaimer in all distributions. Compatible with BSD-2-Clause.

### FFmpeg 9.0 LGPL Shared (Shipped — External CLI Subprocess)
- **Component:** `ffmpeg.exe` + `avcodec-63.dll`, `avdevice-63.dll`, `avfilter-12.dll`, `avformat-63.dll`, `avutil-61.dll`, `swresample-7.dll`, `swscale-10.dll` (`n9.0.2-57-gd838f9f9ae-20261118` `win64-lgpl-shared`)
- **License:** GNU Lesser General Public License v3.0 (LGPLv3, `licenses/FFmpeg-LICENSE.txt`), built **without** `--enable-gpl` / `libx264`
- **Upstream:** https://www.ffmpeg.org/ / BtbN `win64-lgpl-shared` (`ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip`, SHA-256 `3616f1027c0f9c08cdfc92bfaea84282db4398234cdcd861347d438c8edeb088`)
- **Usage:** Invoked out-of-process over `stdin` raw RGB24 pipe via `QProcess` to encode `timelapse.mp4` (`libopenh264`, `yuv420p`), palette-optimized `timelapse.gif`, and compact event clip preview animations (`04_event_preview.gif`).
- **Obligations:** LGPLv3 dynamic/external binary. Ship `licenses/FFmpeg-LICENSE.txt`, keep shared DLLs replaceable, and make corresponding FFmpeg source version metadata available. No GPL components included.

---

## Dependency Evaluation Guidelines

When proposing or introducing any new third-party dependency into SeleneWatch:

1. **Permissive Licenses Preferred:** Dependencies licensed under **BSD**, **MIT**, or **Apache-2.0** are strongly preferred as they integrate cleanly with SeleneWatch's BSD-2-Clause license.
2. **LGPL Libraries:** LGPLv3 or LGPLv2.1 libraries may be used under the condition that they are **dynamically linked** and allow user replacement.
3. **No Strongly Copyleft Dependencies (GPL):** Do not introduce GPL-licensed libraries, GPL-only Qt modules, or AGPL code into SeleneWatch without prior architectural and licensing review and explicit approval.
4. **Proprietary Camera SDKs:** If a camera vendor SDK is proprietary/closed-source with redistribution restrictions, isolate it behind the modular Frame Source abstraction so SeleneWatch's core remains buildable and distributable without proprietary locks.
5. **Update This Document:** Add any accepted dependency to the inventory above before merging into the main branch.
