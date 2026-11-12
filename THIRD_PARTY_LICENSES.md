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
  - `Qt6Test` — linked only by the `SeleneWatchTests` target. The application does not link it, and a release of the app does not need `Qt6Test.dll`.
- **Linkage / Integration:** **Dynamic linking** via shared libraries (`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`). The test target also links `Qt6Test.dll`. SeleneWatch does not statically link Qt.
- **Compliance Obligations:**
  - Provide attribution to the Qt Project and copyright holders.
  - Distribute license texts for LGPLv3 and GPLv3 with binary distributions.
  - Allow users to replace Qt shared libraries with custom/compatible builds (satisfied by dynamic DLL linking).
  - Make Qt source code available or provide a written offer/link to the corresponding upstream source code.
  - Avoid using Qt modules that are licensed under GPL-only (e.g., Qt Charts or certain add-on modules) unless approved.

---

## Dependency Evaluation Guidelines

When proposing or introducing any new third-party dependency into SeleneWatch:

1. **Permissive Licenses Preferred:** Dependencies licensed under **BSD**, **MIT**, or **Apache-2.0** are strongly preferred as they integrate cleanly with SeleneWatch's BSD-2-Clause license.
2. **LGPL Libraries:** LGPLv3 or LGPLv2.1 libraries may be used under the condition that they are **dynamically linked** and allow user replacement.
3. **No Strongly Copyleft Dependencies (GPL):** Do not introduce GPL-licensed libraries, GPL-only Qt modules, or AGPL code into SeleneWatch without prior architectural and licensing review and explicit approval.
4. **Proprietary Camera SDKs:** If a camera vendor SDK is proprietary/closed-source with redistribution restrictions, isolate it behind the modular Frame Source abstraction so SeleneWatch's core remains buildable and distributable without proprietary locks.
5. **Update This Document:** Add any accepted dependency to the table above before merging into the main branch.
