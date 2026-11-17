# Acknowledgements

SeleneWatch's design is original work: how it detects, captures and keeps the evidence of a candidate, its triggers for flashes, darkening and large changes, the clip overview and its suggested kinds, the guiding rules and the sky extras. Where it uses an established method, a file format, a protocol or published reference values, they are credited below with what each contributes. The licences of the software it ships are in `THIRD_PARTY_LICENSES.md`.

## Scientific References & Algorithms

### Detecting changes in the image

- **Wren, Azarbayejani, Darrell & Pentland (1997)** — *Pfinder: Real-Time Tracking of the Human Body*, [IEEE Transactions on Pattern Analysis and Machine Intelligence 19(7), 780–785](https://doi.org/10.1109/34.598236).
  The per-pixel running Gaussian background model: a running mean and variance for every pixel, against which a new frame's change is measured. `FlashDetector` and the clip overview keep one, forgetting with a time constant set in seconds, and hold a pixel that is firing out of the model.
- **Hampel (1974)** — *The Influence Curve and Its Role in Robust Estimation*, [Journal of the American Statistical Association 69(346), 383–393](https://doi.org/10.1080/01621459.1974.10482962).
  The median absolute deviation as a robust measure of spread, scaled by 1.4826 to match a Gaussian standard deviation. `FlashDetector` uses it on a grid of pixels for its noise floor, so that a few bright or changing pixels do not raise it.
- **Welford (1962)** — *Note on a Method for Calculating Corrected Sums of Squares and Products*, [Technometrics 4(3), 419–420](https://doi.org/10.1080/00401706.1962.10490022).
  The one-pass running mean and variance that `FrameTimingMeter` uses for the intervals between frames.

### Keeping the field

- **Lewis (1995)** — *Fast Normalized Cross-Correlation*, Vision Interface 1995 ([author's expanded version](https://www.scribblethink.org/Work/nvisionInterface/nip.html)).
  Template matching by zero-mean normalised cross-correlation, which does not depend on the image's brightness or contrast. `FeatureLock` computes it directly, without the paper's fast method, and fits a parabola through the peak for the sub-pixel position.

### The Sun's position

- **Meeus (1998)** — *Astronomical Algorithms*, 2nd edition, Willmann-Bell.
  Through NOAA's Solar Calculator, whose [calculation details](https://gml.noaa.gov/grad/solcalc/calcdetails.html) follow this book: the Sun's mean longitude and anomaly, equation of centre, apparent longitude, obliquity, declination and equation of time, with which `sunAltitudeDegrees` gives the Sun's altitude for watching from dusk to dawn.
- **Reda & Andreas (2004)** — *Solar position algorithm for solar radiation applications*, [Solar Energy 76(5), 577–589](https://doi.org/10.1016/j.solener.2003.12.003).
  Its worked example is a reference value in the tests of the Sun's altitude. The algorithm itself is not used.

### File formats, protocols and interfaces

- **SER format**, by Heiko Wilkens, extended by Grischa Hahn — [*SER format description, version 3*](https://grischa-hahn.hier-im-netz.de/astro/ser/SER%20Doc%20V3b.pdf) (2014).
  The layout of the SER files SeleneWatch reads, and writes for every clip and recording.
- **AVI and its OpenDML extensions** — Microsoft's [AVI RIFF file reference](https://learn.microsoft.com/en-us/windows/win32/directshow/avi-riff-file-reference), and the OpenDML AVI File Format Extensions (1996).
  The structure, indexes and multi-part files that `AviReader` follows.
- **FireCapture**, by Torsten Edelmann ([firecapture.de](http://www.firecapture.de)).
  The capture logs written beside its recordings, from which SeleneWatch takes a recording's start time and settings.
- **Mills (2006)** — *Simple Network Time Protocol (SNTP) Version 4 for IPv4, IPv6 and OSI*, [RFC 4330](https://www.rfc-editor.org/rfc/rfc4330).
  The query with which the clock check compares the computer's clock with a time server.
- **ASCOM Initiative** — the [ASCOM](https://ascom-standards.org) Telescope interface.
  The mount members SeleneWatch's guider reads, and the guide pulses it sends.

## Software & Utilities

### Open Source Software & Libraries

- **Qt 6**, by The Qt Company and contributors ([qt.io](https://www.qt.io)): the window, images and threads, under the GNU LGPL version 3.

### Camera and Mount Software

- The camera makers' SDKs for **ZWO**, **QHYCCD**, **Player One** and **ToupTek** cameras, and the **ASCOM Platform** for mounts. SeleneWatch ships ZWO's library; the others are installed by the observer.

### Development & Packaging

- **CMake** and **CPack** (Kitware), **Ninja**, **Microsoft Visual C++**, and the **WiX Toolset** (.NET Foundation) for the installer.

### AI Coding Assistants

Since 2026, the development of SeleneWatch has been supported by these AI coding assistants:

- **Claude** by Anthropic ([Claude](https://www.anthropic.com/claude))
- **Codex** by OpenAI ([Codex](https://openai.com/codex/))
