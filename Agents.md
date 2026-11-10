# SeleneWatch

Read [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) before changing the application. It is the project vision and the working architecture. It is a starting point, not a frozen specification.

Machine paths, tool versions, and the commands to configure, build, and run this project are in [local environment.txt](local%20environment.txt).

## Principles

Scientific correctness and preservation of valid results constrain every design change. Within that constraint, the maintainer's goals for the code, in order:

1. **Readable code.** A reader should understand a class from its name, its header comment, and its fields. Prefer plain, direct code over clever generality.
2. **A clear application design with clear boundaries.** Each piece of mutable state has one authoritative owner. Each directory has one job, and its dependencies point one way.
3. **No abstraction or engineering without a real need.** An interface normally needs two implementations or a test seam that is actually used; an explicit listener or ownership boundary can also justify one. A layer needs a responsibility of its own. A compatibility path needs a caller that still uses it. Remove unused machinery after identifying the contracts it protects. Fewer classes or files alone do not make a design simpler.
4. **A few chosen design patterns, applied consistently.** No pattern catalog has been chosen yet. When patterns are added to this file, reuse them when their responsibilities fit. Consistency does not justify an unnecessary abstraction.

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
