# Acknowledgements

SeleneWatch's design is original work: how it detects, captures and keeps the evidence of a candidate, its triggers for flashes, darkening and large changes, the clip overview and its suggested kinds, the guiding rules and the sky extras. Where it uses an established method, interface or published reference values, they are credited below with what each contributes. The licences of the software it ships are in `THIRD_PARTY_LICENSES.md`.

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

### Interfaces

- **ASCOM Initiative** — the [ASCOM](https://ascom-standards.org) Telescope interface.
  The mount members SeleneWatch's guider reads, and the guide pulses it sends.

## Software & Utilities

### Open Source Software & Libraries

- **Qt 6**, by The Qt Company and contributors ([qt.io](https://www.qt.io)): the window, images and threads, under the GNU LGPL version 3.
- **OpenCV**, by the OpenCV team ([opencv.org](https://opencv.org)): debayering, phase-correlation subpixel alignment, rigid/ECC Lunar field-rotation alignment, and image stacking/sharpening utilities, under the Apache License 2.0.
- **CFITSIO**, by NASA/GSFC HEASARC ([heasarc.gsfc.nasa.gov/fitsio](https://heasarc.gsfc.nasa.gov/fitsio/)): reading and writing linear 32-bit floating-point FITS stacks with astronomy metadata headers, under the NASA permissive open-source license.
- **FFmpeg**, by the FFmpeg developers ([ffmpeg.org](https://ffmpeg.org)): encoding MP4 (`libopenh264`) and palette-optimized GIF time-lapse animations, shipped as an external LGPLv3 shared build (`win64-lgpl-shared`).
- **QR Code generator library**, by Project Nayuki ([nayuki.io](https://www.nayuki.io/page/qr-code-generator-library)): the QR code that takes your phone to SeleneWatch's status page, under the MIT License.

### Camera and Mount Software

- The camera makers' SDKs for **ZWO**, **QHYCCD**, **Player One** and **ToupTek** cameras, and the **ASCOM Platform** for mounts. SeleneWatch ships ZWO's library; the others are installed by the observer.

### AI Coding Assistants

The development of SeleneWatch has been supported by these AI coding assistants:

- **Claude** by Anthropic ([Claude](https://www.anthropic.com/claude))
- **Codex** by OpenAI ([Codex](https://openai.com/codex/))
