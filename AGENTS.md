# SeleneWatch

Read [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) before changing the application. It is the project vision and the working architecture. It is a starting point, not a frozen specification.

Portable build, test, SDK, and release instructions are in [DEVELOPER.md](DEVELOPER.md). Machine-specific paths, tool versions, and local test data on the maintainer's workstation are in [local environment.txt](local%20environment.txt). When changing user-visible controls, build options, or packaging, keep [README.md](README.md), [MANUAL.md](MANUAL.md), and [DEVELOPER.md](DEVELOPER.md) in sync.

## Principles

**Read [DEVELOPER.md](DEVELOPER.md) sections 1–4 before changing code.** They hold the maintainer's principles (readable code, clear boundaries, no abstraction without a real need, a few patterns applied consistently, one implementation of each behaviour), the design rules and what checks each, the design patterns in use, recipes for common additions, and the working rules. Follow them.

Pending design and refactoring work is in [DESIGN-IMPROVEMENTS.md](DESIGN-IMPROVEMENTS.md). An item there is a proposal, not an instruction to implement it, unless the maintainer has decided it.

## Licensing and Third-Party Dependencies

SeleneWatch is an open-source project licensed under the **BSD 2-Clause ("Simplified") License**. All development agents must respect the following guidelines:

1. **Preserve the project license.** All original SeleneWatch code should remain under BSD-2-Clause. Do not modify or replace the existing `LICENSE` file without explicit approval.
2. **Qt licensing.** Use Qt 6 under its open-source LGPLv3 terms where applicable. Prefer dynamic linking with Qt DLLs rather than static linking.
3. **Avoid incompatible dependencies.** Prefer permissive licenses (BSD, MIT, Apache-2.0) and LGPL-compatible libraries. Do not introduce GPL-only Qt modules or other strongly copyleft dependencies without discussing their licensing implications and obtaining approval.
4. **Track third-party dependencies.** Maintain a `THIRD_PARTY_LICENSES.md` file documenting each significant dependency, its version, license, official source, and relevant obligations. This includes Qt modules, camera SDKs, image-processing libraries, and incorporated third-party code.
5. **Respect attribution and redistribution requirements.** Preserve copyright notices and required license texts. Do not copy external source code without checking its license.
6. **Release compliance.** Before distributing binaries, verify all dependency licenses. For LGPL-covered Qt libraries, ensure appropriate notices, license texts, corresponding Qt source availability, and users' ability to replace the libraries with compatible modified versions.
7. **Architecture and licensing flexibility.** Prefer solutions that preserve BSD-2-Clause licensing and modularity. If a dependency introduces significant licensing restrictions, report the trade-offs and propose alternatives rather than silently introducing it.

These guidelines apply to all development agents and should be revisited when introducing new dependencies or preparing releases.

## Feature Tracking and Documentation

- **Explicit feature IDs:** Proposals and completed work are identified by explicit IDs (e.g., `Feature-2`, `Feature-3`, `Feature-4`).
- **Writing user documentation:** For the README, the MANUAL and the website, follow [DEVELOPER.md, Writing the user documentation](DEVELOPER.md#writing-the-user-documentation): plain words, the observer's four uses in one order, only what was measured, and every picture labelled for what it is.
- **Preserve full detail in history:** When moving completed features from [`features.md`](features.md) to [`implemented features.md`](implemented%20features.md), do not condense, summarize, or remove details. Append the entire section—including specifications, review notes, decision rationales, and acceptance criteria. `implemented features.md` is an append-only historical record of what was built and why.

## Commits and Attribution

- **Always commit verified work:** After completing and verifying a change, stage only your own changes, commit them with a descriptive message and validation summary, and push—unless pausing at a design or decision point that needs the maintainer's input first.
- **Credit AI assistance:** Always credit the AI coding assistant used (for example, `Co-Authored-By:` trailer and a brief note in the commit body).


