# Acknowledgements

SeleneWatch's design is original work: how it detects, captures and keeps the evidence of a candidate, its triggers for flashes, darkening and large changes, its measured event grouping and nightly report, its guiding rules and its time-lapse workflow. Where it uses an established method, published algorithm, standard, interface or published reference value, each entry below identifies its contribution; related literature is omitted unless it supplies an additional method used here. The licences of the software SeleneWatch ships are in `THIRD_PARTY_LICENSES.md`.

## Scientific References & Algorithms

### Detecting changes in the image

- **Wren, Azarbayejani, Darrell & Pentland (1997)** — *Pfinder: Real-Time Tracking of the Human Body*, [IEEE Transactions on Pattern Analysis and Machine Intelligence 19(7), 780–785](https://doi.org/10.1109/34.598236).
  The per-pixel running Gaussian background model: a running mean and variance for every pixel, against which a new frame's change is measured. `FlashDetector` and `clip_overview` keep one, forgetting with a time constant set in seconds, and hold a pixel that is firing out of the model.
- **Hampel (1974)** — *The Influence Curve and Its Role in Robust Estimation*, [Journal of the American Statistical Association 69(346), 383–393](https://doi.org/10.1080/01621459.1974.10482962).
  The median absolute deviation (MAD) as a robust measure of spread, scaled by 1.4826 to match a Gaussian standard deviation. Used on a grid of pixels for the noise floor in `FlashDetector`, for pixel noise on structured scenes in `FeatureLock`, and for residual scale in time-lapse brightness matching (`stack_processor`).
- **Welford (1962)** — *Note on a Method for Calculating Corrected Sums of Squares and Products*, [Technometrics 4(3), 419–420](https://doi.org/10.1080/00401706.1962.10490022).
  The one-pass running mean and variance used in `FrameTimingMeter` for inter-frame intervals and in `auto_tune` when measuring frame noise and statistics.

### Keeping the field (feature lock & guiding)

- **Lewis (1995)** — *Fast Normalized Cross-Correlation*, Vision Interface 1995 ([author's expanded version](https://www.scribblethink.org/Work/nvisionInterface/nip.html)).
  Template matching by zero-mean normalised cross-correlation, which does not depend on the image's brightness or contrast. `FeatureLock` computes it directly, without the paper's integral-image fast method, and fits a parabola through the peak for the sub-pixel position.
- **Immerkær (1996)** — *Fast Noise Variance Estimation*, [Computer Vision and Image Understanding 64(2), 300–302](https://doi.org/10.1006/cviu.1996.0060).
  The 3×3 second-difference operator (`[1, −2, 1; −2, 4, −2; 1, −2, 1]`, scale factor 6) that cancels smooth shading and ramps. `FeatureLock::pixelNoise` pairs this operator with Hampel's MAD × 1.4826 scale (instead of the paper's mean) to estimate pixel noise on structured lunar scenes.

### Time-lapse alignment, stacking & sharpening

- **Kuglin & Hines (1975)** — *The Phase Correlation Image Alignment Method*, Proceedings of the IEEE International Conference on Cybernetics and Society, 163–165.
  Sub-pixel translation estimation for burst stacking and coarse lunar alignment, computed via OpenCV's `cv::phaseCorrelate` with a Hann window on high-pass-filtered luminance (`stack_processor`).
- **Evangelidis & Psarakis (2008)** — *Parametric Image Alignment Using Enhanced Correlation Coefficient Maximization*, [IEEE Transactions on Pattern Analysis and Machine Intelligence 30(10), 1858–1865](https://doi.org/10.1109/TPAMI.2008.113).
  Rigid Euclidean (translation and field rotation) alignment across Moon time-lapse stacks on a coarse-to-fine Gaussian pyramid, anchored on a user-selected sunlit region via OpenCV's `cv::findTransformECC` (`stack_processor`).
- **Duchon (1979)** — *Lanczos Filtering in One and Two Dimensions*, [Journal of Applied Meteorology 18(8), 1016–1022](https://doi.org/10.1175/1520-0450(1979)018<1016:LFIOAT>2.0.CO;2).
  Lanczos-4 sub-pixel resampling (`cv::INTER_LANCZOS4`) used when warping aligned burst frames and session stacks (`stack_processor`).
- **Tomasi & Manduchi (1998)** — *Bilateral Filtering for Gray and Color Images*, [Proceedings of the 6th IEEE International Conference on Computer Vision (ICCV), 839–846](https://doi.org/10.1109/ICCV.1998.710815).
  Optional edge-preserving pre-denoise before linear unsharp masking via OpenCV's `cv::bilateralFilter` (`stack_processor`).
- **Huber (1964)** — *Robust Estimation of a Location Parameter*, [Annals of Mathematical Statistics 35(1), 73–101](https://doi.org/10.1214/aoms/1177703732).
  Huber-reweighted iteratively reweighted least squares (IRLS) used in `stack_processor` to fit affine brightness levelling between aligned stacks without letting passing clouds bias the fit.

### Sun and Moon positions

- **Meeus (1998)** — [*Astronomical Algorithms*, 2nd ed.](https://shopatsky.com/products/astronomical-algorithms-2nd-edition), Willmann-Bell.
  Through NOAA's Solar Calculator ([calculation details](https://gml.noaa.gov/grad/solcalc/calcdetails.html)) for `sunAltitudeDegrees` (the Sun's mean longitude and anomaly, equation of centre, apparent longitude, obliquity, declination and equation of time, for dusk-to-dawn scheduling), and the Greenwich Mean Sidereal Time expression used in `moonAltitudeDegrees`.
- **The Astronomical Almanac** (U.S. Naval Observatory / HM Nautical Almanac Office), *Low-Precision Formulae for Geocentric Coordinates of the Moon* — adapted from **Van Flandern & Pulkkinen (1979)**, *Low-Precision Formulae for Planetary Positions*, [ApJS 41, 391–411](https://doi.org/10.1086/190623).
  The six-term ecliptic longitude, four-term ecliptic latitude and four-term horizontal parallax expressions in `moonAltitudeDegrees`, with topocentric parallax correction, used to end a session when the Moon sinks below a chosen altitude.
- **Reda & Andreas (2004)** — *Solar position algorithm for solar radiation applications*, [Solar Energy 76(5), 577–589](https://doi.org/10.1016/j.solener.2003.12.003).
  Its worked example is a reference value in the automated tests of `sunAltitudeDegrees`. The algorithm itself is not used.
- **NASA JPL Horizons** ([ssd.jpl.nasa.gov/horizons](https://ssd.jpl.nasa.gov/horizons/)).
  Published topocentric lunar altitude tables serve as reference values in the automated tests of `moonAltitudeDegrees`. SeleneWatch does not query Horizons at run time.

### Formats, protocols & interfaces

- **FITS Standard — Pence, Chiappetti, Page, Shaw & Stobie (2010)** — *Definition of the Flexible Image Transport System (FITS), version 3.0*, [Astronomy & Astrophysics 524, A42](https://doi.org/10.1051/0004-6361/201015362).
  Standard followed for SeleneWatch's linear 32-bit floating-point time-lapse stacks (`fits_io`, via CFITSIO).
- **SER Format (v3) — Heiko Wilkens & Grischa Hahn** ([SER format specification](https://www.grischa-hahn.homepage.t-online.de/astro/ser/SER%20Doc%20V3b.pdf)).
  Lossless astronomical video format with 100-ns UTC frame timestamps read and written by `SerReader` and `SerWriter`.
- **NTPv4 — Mills, Martin, Burbank & Kasch (2010)** — *Network Time Protocol Version 4: Protocol and Algorithms Specification*, [RFC 5905](https://datatracker.ietf.org/doc/html/rfc5905).
  Client UDP packet exchange and offset/round-trip calculation used in `clock_check` to measure and record the PC clock offset at session start.
- **ASCOM Initiative** — the [ASCOM](https://ascom-standards.org/) Telescope interface.
  The mount members SeleneWatch's guider reads and the guide pulses it sends (`ascom_mount`).
- **FireCapture** by Torsten Edelmann ([firecapture.de](https://www.firecapture.de/)).
  Companion FireCapture `.txt` capture logs parsed by `firecapture_log` when replaying recorded SER and AVI files to recover camera settings and timestamps.

## Software & Utilities

### Open Source Software & Libraries

- **Qt 6** by The Qt Company and contributors ([qt.io](https://www.qt.io)) — the window, images, networking and threads, dynamically linked under the GNU LGPL version 3.
- **OpenCV** by the OpenCV team ([opencv.org](https://opencv.org)) — edge-aware debayering, phase-correlation sub-pixel alignment, rigid Euclidean/ECC lunar field-rotation alignment, Lanczos resampling, bilateral filtering and linear unsharp masking, under the Apache License 2.0.
- **CFITSIO** by NASA/GSFC HEASARC ([heasarc.gsfc.nasa.gov/fitsio](https://heasarc.gsfc.nasa.gov/fitsio/)) — reading and writing linear 32-bit floating-point FITS stacks with astronomy metadata headers, under the NASA permissive open-source license.
- **FFmpeg** by the FFmpeg developers ([ffmpeg.org](https://ffmpeg.org)) — encoding MP4 (`libopenh264`), palette-optimized GIF time-lapse animations and compact event preview GIFs, shipped as an external LGPLv3 shared build (`win64-lgpl-shared`).
- **QR Code generator library** by Project Nayuki ([nayuki.io](https://www.nayuki.io/page/qr-code-generator-library)) — generates the QR code that opens SeleneWatch's local status page on a phone, under the MIT License.

### Camera & Mount Software

- The camera makers' SDKs for **ZWO**, **QHYCCD**, **Player One** and **ToupTek** cameras, and the **ASCOM Platform** ([ascom-standards.org](https://ascom-standards.org/)) for mounts. SeleneWatch ships ZWO's library under its MIT-worded license; the others are installed by the observer.

### Development & Packaging

- [CMake](https://cmake.org/) and [Ninja](https://ninja-build.org/) build the application; **Qt Test** and **CTest** run the automated suites; [WiX Toolset 3](https://wixtoolset.org/) and CPack create the Windows `.msi` installer and `.zip` archive.

### AI Coding Assistants

The development of SeleneWatch has been supported by these AI coding assistants:

- **Claude** by Anthropic ([Claude](https://www.anthropic.com/claude))
- **Codex** by OpenAI ([Codex](https://openai.com/codex/))
- **Gemini** by Google ([Gemini](https://gemini.google.com/))
- **Grok** by xAI ([Grok](https://x.ai/grok))

Logos shown in the README and in-app Acknowledgements dialog are trademarks of their respective owners.
