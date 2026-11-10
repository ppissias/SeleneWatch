# SeleneWatch

Read [PROJECT_DESCRIPTION.md](PROJECT_DESCRIPTION.md) before changing the application. It is the project vision and the working architecture. It is a starting point, not a frozen specification.

Machine paths, tool versions, and the commands to configure, build, and run this project are in [local environment.txt](local%20environment.txt).

## Principles

Scientific correctness and preservation of valid results constrain every design change. Within that constraint, the maintainer's goals for the code, in order:

1. **Readable code.** A reader should understand a class from its name, its header comment, and its fields. Prefer plain, direct code over clever generality.
2. **A clear application design with clear boundaries.** Each piece of mutable state has one authoritative owner. Each directory has one job, and its dependencies point one way.
3. **No abstraction or engineering without a real need.** An interface normally needs two implementations or a test seam that is actually used; an explicit listener or ownership boundary can also justify one. A layer needs a responsibility of its own. A compatibility path needs a caller that still uses it. Remove unused machinery after identifying the contracts it protects. Fewer classes or files alone do not make a design simpler.
4. **A few chosen design patterns, applied consistently.** No pattern catalog has been chosen yet. When patterns are added to this file, reuse them when their responsibilities fit. Consistency does not justify an unnecessary abstraction.
