# Third-Party Licenses and Dependencies

SeleneWatch is an open-source project licensed under the **BSD 2-Clause ("Simplified") License** (see [LICENSE](LICENSE)).

This document tracks all external libraries, frameworks, SDKs, and third-party code used in SeleneWatch, documenting their license terms, official sources, and redistribution/compliance obligations.

---

## Active Dependencies

### 1. Qt 6 (Core, Gui, Widgets)

- **Website / Source:** [https://www.qt.io](https://www.qt.io) / [https://code.qt.io](https://code.qt.io)
- **Copyright:** (c) The Qt Company Ltd. and other contributors
- **Current Development Version:** Qt 6.12.0
- **License:** **GNU Lesser General Public License version 3 (LGPLv3)** (also available under GPLv3 and commercial licenses)
- **Used Modules:**
  - `Qt6Core`
  - `Qt6Gui`
  - `Qt6Widgets`
  - `Qt6Test` — linked only by the test executables. The application does not link it, and a release of the app does not need `Qt6Test.dll`.
- **Linkage / Integration:** **Dynamic linking** via shared libraries (`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`). The test target also links `Qt6Test.dll`. SeleneWatch does not statically link Qt.
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

## QHYCCD Windows SDK (user-installed runtime)

- **Installed version:** Windows x64 25.6.16.15, confirmed from the file/header and a runtime version-only query.
- **Vendor sources reviewed:** [SDK documentation and demos](https://www.qhyccd.com/sdk-demo-and-sample-project/), [downloads](https://www.qhyccd.com/download/). A [historical SDK repository](https://github.com/qhyccd-lzr/QHYCCD_Linux) was also checked for context, without establishing this Windows package's terms.
- **Terms:** The installed package has no SDK-wide license text. Windows SDK redistribution rights and any required notices remain unverified. The compatibility header refers to its own copyright and license.
- **Integration:** The optional build uses declarations from the externally installed header. At runtime SeleneWatch loads the user's x64 `qhyccd.dll` by absolute path when QHY is selected. QHY documents the All-In-One package as the source for third-party application SDKs. [QHY installation guidance](https://www.qhyccd.com/download/)
- **Distribution:** QHY headers, import libraries, DLLs, drivers, firmware and demos stay out of the repository and SeleneWatch packages. The tested `qhyccd.dll` imports Windows components. Bundling any QHY component would require verifying its terms and notices first. SeleneWatch's original adapter code remains BSD-2-Clause.
- **Validation:** Physical camera qualification is pending. [QHY integration and validation](docs/qhy-development.md)

## Dependency Evaluation Guidelines

When proposing or introducing any new third-party dependency into SeleneWatch:

1. **Permissive Licenses Preferred:** Dependencies licensed under **BSD**, **MIT**, or **Apache-2.0** are strongly preferred as they integrate cleanly with SeleneWatch's BSD-2-Clause license.
2. **LGPL Libraries:** LGPLv3 or LGPLv2.1 libraries may be used under the condition that they are **dynamically linked** and allow user replacement.
3. **No Strongly Copyleft Dependencies (GPL):** Do not introduce GPL-licensed libraries, GPL-only Qt modules, or AGPL code into SeleneWatch without prior architectural and licensing review and explicit approval.
4. **Proprietary Camera SDKs:** If a camera vendor SDK is proprietary/closed-source with redistribution restrictions, isolate it behind the modular Frame Source abstraction so SeleneWatch's core remains buildable and distributable without proprietary locks.
5. **Update This Document:** Add any accepted dependency to the table above before merging into the main branch.
