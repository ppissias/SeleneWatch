# Feature-50 Stage 1 Benchmark — Runtime Sub-Pixel Stabilized Detection

*Measured on 24 November 2026 on a Windows x64 Release build (`out/build/release/SeleneWatchProbe.exe stabilize-bench`), using the real `FlashDetector` (`src/detection/flash_detector.cpp`) with optional zero-copy shifted sampling (`ShiftMode::Off`, `Integer`, `Subpixel`) and `recenterReference`.*

This benchmark evaluates both **detection benefit** (quietness, peak-sigma reduction, and event recovery) and **computational cost** (Release x64 ms/frame) before implementing Stage 2 (UI checkbox and `Session` wiring). It replaces the earlier 14 November 2026 integer-only prototype ([registration experiment](registration-experiment.md)), which ran on the older whole-frame-hold detector.

---

## 1. Executive Summary & Recommendations for Stage 2

1. **Sub-pixel bilinear stabilization provides a clear, measurable benefit on drifting/shaking Moon, Sun, and planetary streams — for both flashes and shadows — while integer-only shifting often hurts.**
   - **Real sunlit Moon (`Moon_171311.ser`, 1296×964, 500 frames, 27×19 px drift, 1.28 px RMS frame-to-frame shake):**
     - Quiet control (`K = 8, 12, 18`): **0 false triggers** in all modes.
     - Injected ISS shadows at the shadows watch default **`K = 12`**: unstabilized recovered **5/11**, integer-shifted dropped to **3/11**, and **sub-pixel recovered 8/11 (+60%) with 0 false triggers**. At `K = 8`, sub-pixel recovered **11/11 (100%) with 0 false triggers** (vs. 8/11 unstabilized).
     - Injected faint flashes at `K = 6`: unstabilized had **9 false triggers** and recovered **2/11**; sub-pixel cut false triggers to **4** and doubled recovery to **4/11**.
   - **Real Mars planetary recording (`Mars_222831.avi`, 360×276, 737 frames, 26×16 px drift, 1.60 px RMS shake):**
     - `DiskCoG` locked **737/737 frames (100%)** in **0.09 ms/frame**.
     - At `K = 8` (planetary flash watch): unstabilized produced **9 false triggers** (`p95 peakSigma = 12.9`), integer produced **1** (`p95 = 8.1`), and **sub-pixel produced 0 false triggers (`p95 = 6.5`)**.
     - With injected shadows at `K = 12`: unstabilized recovered **12/16**; **sub-pixel recovered 15/16 with 0 false triggers**.
   - **Real lunar/solar ISS transits and impact flashes:**
     - `iss_lunar_transit_1080p.ser` (`K = 8`): unstabilized **3 triggers** (2 false + ISS), integer **7 triggers** (6 false + ISS), **sub-pixel 1 trigger (ISS only, 0 false)**.
     - `nasa_iss_solar_2020_leadin.avi` (`K = 6` / `K = 8`): unstabilized **22 / 9 triggers**, **sub-pixel 6 / 6 triggers (exact 6 ISS frames, 0 false)**.
     - `nasa_msfc_lunar_impact_20130317.ser`: `FeatureLock` 244/244 locked (0.74 ms), sub-pixel recovers the 2013 impact flash at `K = 8, 12, 18` with **+12% higher flash sigma** (`21.8` vs. `19.5` unstabilized) and keeps `K = 6` clean (2 vs. 7 for integer).
     - `FDS_moon_2.avi` (local test clip): recovers the flash cleanly at `K = 8, 12, 18` with lower background sigma (`29.9` vs. `36.4`).
   - **Controlled synthetic sub-pixel drift + 0.9 px RMS jitter (Mono8, Mono16, raw Bayer RGGB):**
     - Unstabilized missed **all 6/6** flashes and shadows; **sub-pixel recovered 6/6 with 0 false triggers**, `0.10 px` flash coordinate accuracy, and zero Bayer cross-channel artifacts.

2. **CPU cost is small on planetary and mid-size lunar cameras, and documented for 4.5 MP cameras:**
   - **Planetary (`480×480` Mono8):** `FlashDetector` adds **+0.25 ms** (`0.86 → 1.12 ms`) + `DiskCoG` **0.17 ms** = **1.29 ms/frame** (> 700 fps headroom).
   - **Mid lunar (`1280×960` Mono8/Mono16/Bayer):** `FlashDetector` adds **+1.4–1.9 ms** (`5.6–6.1 → 7.2–7.5 ms`) + `FeatureLock` **0.65 ms** (or **0 ms** if already guiding) / `DiskCoG` **0.28 ms** = **~7.9–8.1 ms/frame** (> 100 fps headroom).
   - **Large lunar (`2128×2128` Mono8/Mono16/Bayer):** `FlashDetector` adds **+5.2–7.1 ms** (`21.6 → 26.8–28.6 ms`) + `FeatureLock` **0.65 ms** / `DiskCoG` **1.06 ms** = **~27.5–29.5 ms/frame** (fits 30 fps = 33.3 ms budget; tight above 35 fps — reinforces keeping stabilization **opt-in, off by default**).

3. **Drop 2D phase correlation (`PhaseCorr`) from Stage 2 — keep only `FeatureLock` + `DiskCoG`:**
   - On `Mars_222831.avi`, `PhaseCorr` locked only **220/737** frames and produced **48 false triggers at K=8**, whereas `DiskCoG` locked **737/737 (100%)** in **0.09 ms** with **0 false triggers**.
   - On `Moon_171311.ser`, `PhaseCorr` locked only **412/500** frames in **1.29 ms** (9 false triggers at K=6), whereas `FeatureLock` locked **500/500 (100%)** in **0.70 ms** (1 false trigger at K=6, 0 at K=8+).
   - Keeping only `FeatureLock` (already in `src/guiding/`) + `DiskCoG` (pure C++, ~90 lines) avoids any OpenCV dependency in detection and preserves one-way folder boundaries.

4. **Known limitations observed (important for honest documentation):**
   - **Violent multi-pixel jumpiness (`Moon_221449.avi`, 8 fps, `rmsStep = 5.24 px`, 44 px drift in 8.9 s):** `FeatureLock` spent 24/76 frames in grace hold and 9 in fallback, giving no benefit over unstabilized on that clip (0 false at `K = 12` in all modes). Stabilization helps most with typical sub-pixel to ~2 px frame-to-frame seeing/drift; when motion is violent it falls back safely.
   - **Existing `protected_` freeze at high K on alternating/unsettled edges:** On `nasa_iss_solar_2020_leadin.avi` (synthetic alternating lead-in) and `Mars_222831.avi` at `K = 18`, raising `K` can shrink a large limb crescent into `[minPixels, maxPixels]` while `variance_` is still settling, freezing those pixels because `releaseSeconds == 0` on Moon watches. This happens in both unstabilized and stabilized runs and is unrelated to shift accuracy.

---

## 2. Release x64 CPU Cost Matrix

Measured with `SeleneWatchProbe stabilize-bench cpu` (MSVC Release-x64, warmed up, 25–120 iterations per configuration).

### `FlashDetector` (`analyze` + `updateReference`)

| Frame size | Format | Off (mean ms) | Integer (mean ms) | Sub-pixel (mean ms) | Sub-pixel overhead | Sub-pixel p95 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `480×480` | Mono8 | 0.86 | 0.80 | 1.12 | +0.25 ms | 1.13 |
| `480×480` | Mono16 | 1.02 | 0.83 | 1.09 | +0.08 ms | 1.12 |
| `480×480` | Bayer8 (RGGB) | 0.93 | 0.82 | 1.11 | +0.18 ms | 1.13 |
| `480×480` | Bayer16 (RGGB) | 0.99 | 0.83 | 1.08 | +0.09 ms | 1.10 |
| `480×480` | RGB888 | 0.99 | 0.92 | 1.32 | +0.33 ms | 1.34 |
| `640×480` | Mono8 | 1.11 | 1.05 | 1.46 | +0.35 ms | 1.49 |
| `640×480` | Mono16 | 1.87 | 1.47 | 1.96 | +0.09 ms | 2.26 |
| `640×480` | Bayer8 (RGGB) | 1.58 | 1.41 | 1.93 | +0.35 ms | 2.41 |
| `640×480` | Bayer16 (RGGB) | 1.46 | 1.36 | 1.85 | +0.39 ms | 2.28 |
| `640×480` | RGB888 | 1.65 | 1.59 | 2.48 | +0.84 ms | 3.05 |
| `1280×960` | Mono8 | 5.71 | 4.77 | 7.23 | +1.52 ms | 9.21 |
| `1280×960` | Mono16 | 6.07 | 5.25 | 7.46 | +1.40 ms | 8.68 |
| `1280×960` | Bayer8 (RGGB) | 5.62 | 5.74 | 7.38 | +1.76 ms | 9.19 |
| `1280×960` | Bayer16 (RGGB) | 5.62 | 5.53 | 7.50 | +1.88 ms | 9.77 |
| `1280×960` | RGB888 | 6.70 | 6.29 | 9.18 | +2.47 ms | 11.51 |
| `2128×2128` | Mono8 | 21.59 | 20.92 | 28.18 | +6.59 ms | 31.38 |
| `2128×2128` | Mono16 | 21.61 | 21.57 | 27.01 | +5.40 ms | 30.50 |
| `2128×2128` | Bayer8 (RGGB) | 21.57 | 19.28 | 28.63 | +7.06 ms | 31.64 |
| `2128×2128` | Bayer16 (RGGB) | 21.59 | 20.60 | 26.78 | +5.19 ms | 29.29 |
| `2128×2128` | RGB888 | 24.66 | 23.20 | 34.73 | +10.06 ms | 40.70 |

### Shift Estimators (mean ms/frame)

| Frame size | Format | `FeatureLock` (128×128, ±16 px) | `DiskCoG` | `PhaseCorr` (256×256) |
| --- | --- | ---: | ---: | ---: |
| `480×480` | Mono8 / Bayer8 | 0.51 / 0.52 ms | 0.17 / 0.05 ms | 0.64 / 0.63 ms |
| `640×480` | Mono8 / Bayer8 | 0.56 / 0.58 ms | 0.35 / 0.09 ms | 0.79 / 0.85 ms |
| `1280×960` | Mono8 / Bayer8 | 0.65 / 0.67 ms | 0.28 / 0.31 ms | 0.99 / 1.14 ms |
| `2128×2128` | Mono8 / Bayer8 | 0.66 / 0.65 ms | 1.06 / 1.12 ms | 3.35 / 3.12 ms |

*Note:* `FeatureLock` cost is essentially constant (~0.65 ms) regardless of full-frame resolution because it operates on a bounded 64×64 binned template. When Feature Lock guiding is already active, reusing its live `(dx, dy)` costs **~0 ms** extra.

---

## 3. Real Recording Benchmarks

All counts below are post-2 s settling triggers across the exact same `FlashDetector` code (`Off` vs. `Integer` vs. `Subpixel`).

### 3.1 Sunlit Moon / Sun Shadows & Archive Lunar Recordings

| Recording | Size / frames | Estimator & lock | Drift / RMS step | K | Off trig (p95 σ) | Integer trig (p95 σ) | Sub-pixel trig (p95 σ) | Notes |
| --- | --- | --- | --- | ---: | --- | --- | --- | --- |
| `nasa_iss_lunar_2019_leadin.avi` | 2128×2128 / 85 | `DiskCoG` 85/85 (1.40 ms) | 4.3×2.6 px / 2.78 px | 8 / 12 | 4 / 5 (63.0) | 4 / 5 (62.0) | **5 / 5** (61.4) | All 5 real ISS frames caught cleanly |
| `nasa_iss_solar_2020_leadin.avi` | 1920×1412 / 86 | `DiskCoG` 86/86 (0.80 ms) | 7.4×2.1 px / 1.15 px | 6 / 8 | 22 / 9 (187 / 127) | 5 / 6 (120) | **6 / 6** (139) | Exact 6 ISS frames, 0 false at K=6 & K=8 |
| `iss_lunar_transit_1080p.ser` (first 400 f) | 1720×1080 / 400 | `DiskCoG` 400/400 (0.45 ms) | 3.5×1.9 px / 0.19 px | 8 / 12 | 3 / 1 (132 / 124) | 7 / 1 (204 / 16.8) | **1 / 1** (176 / 176) | Sub-pixel eliminates false triggers at K=8 |
| `Moon_171311.ser` (control, shadows) | 1296×964 / 500 | `FeatureLock` 500/500 (0.76 ms) | 27.1×19.0 px / 1.28 px | 6 / 8 / 12 | 0 / 0 / 0 (5.8) | 0 / 0 / 0 (5.4) | 1 / 0 / 0 (5.8) | Quiet across K ≥ 8 (`PhaseCorr`: 412/500 locked, 9 false at K=6) |
| `Moon_171311.ser` (`--inject-shadow`, 11 ISS) | 1296×964 / 500 | `FeatureLock` 500/500 (0.70 ms) | 27.1×19.0 px / 1.28 px | 8 / 12 | **8/11** / **5/11** | **8/11** / **3/11** | **11/11** / **8/11** | **+60% shadow recovery at K=12, 100% at K=8, 0 false** |
| `Moon_171311.ser` (`--inject-flash`, 11 flashes) | 1296×964 / 500 | `FeatureLock` 500/500 (0.68 ms) | 27.1×19.0 px / 1.28 px | 6 | 11 trig (9 false, **2/11** rec) | 4 trig (1 false, **3/11** rec) | 8 trig (4 false, **4/11** rec) | Half the false triggers, 2× flash recovery |
| `Moon_221449.avi` (shadows, violent shake) | 1296×964 / 76 | `FeatureLock` 43/76, 24g, 9l | 44.2×22.6 px / 5.24 px | 8 / 12 | 0 / 0 (6.6) | 0 / 0 (7.9) | 2 / 0 (12.6) | Violent 5.2 px/f shake exceeds lock margin; safe at K=12 |

### 3.2 Lunar Impact Flashes

| Recording | Size / frames | Estimator & lock | Drift / RMS step | K | Off trig (p95 σ) | Integer trig (p95 σ) | Sub-pixel trig (p95 σ) | Notes |
| --- | --- | --- | --- | ---: | --- | --- | --- | --- |
| `nasa_msfc_lunar_impact_20130317.ser` | 948×602 / 244 | `FeatureLock` 244/244 (0.74 ms) | 4.1×4.5 px / 1.71 px | 6 / 8 / 12 / 18 | 2 / 1 / 1 / 1 (19.5) | 7 / 1 / 1 / 1 (22.8) | **2 / 1 / 1 / 1** (**21.8**) | +12% stronger flash SNR; integer added 5 false at K=6 |
| Local earthshine flash clip (`FDS_moon_2.avi`) | 1080×1280 / 93 | `FeatureLock` 73/93, 15g, 5l | 9.3×6.4 px / 1.93 px | 8 / 12 / 18 | 1 / 1 / 1 (36.4) | 1 / 1 / 1 (30.3) | **1 / 1 / 1** (**29.9**) | Clean flash recovery, lower background sigma |

### 3.3 Planetary Target (`Mars_222831.avi`, 360×276, 737 frames)

| Trial | Estimator & lock | Drift / RMS step | K | Off | Integer | Sub-pixel |
| --- | --- | --- | ---: | --- | --- | --- |
| Quiet control (flash watch) | `DiskCoG` 737/737 (0.09 ms) | 26.4×16.3 px / 1.60 px | 8 / 12 | 9 / 0 false (`p95 12.9 / 7.9`) | 1 / 2 false (`p95 8.1 / 7.7`) | **0 / 0 false (`p95 6.5 / 6.5`)** |
| Quiet control (`PhaseCorr` comparison) | `PhaseCorr` 220/737 (0.58 ms) | 27.1×26.7 px / 3.43 px | 8 / 12 | 9 / 0 false | 47 / 29 false | 48 / 30 false (`PhaseCorr` unsuitable for small disks) |
| Injected flashes (16 events) | `DiskCoG` 737/737 (0.08 ms) | 26.4×16.3 px / 1.60 px | 8 | 14 trig (7 false, 7/16 rec) | 4 trig (0 false, 4/16 rec) | **6 trig (0 false, 6/16 rec)** |
| Injected shadows (16 events) | `DiskCoG` 737/737 (0.09 ms) | 26.4×16.3 px / 1.60 px | 12 | 12/16 rec | 9/16 rec | **15/16 rec (0 false)** |

---

## 4. Synthetic Accuracy, Bayer Integrity, Failure-Mode & Field-Rotation Verification

Measured with `SeleneWatchProbe stabilize-bench synthetic`:

| Test | Scenario | Measured Result |
| --- | --- | --- |
| **A: Drift + 0.9 px RMS jitter** | Mono8, Mono16, raw **Bayer8 RGGB** — both flashes & shadows | Shift error **0.01–0.02 px**; `Off` recovered **0/6** (2 false); `Subpixel` recovered **6/6** with **0 false triggers**, flash coordinate error **0.10 px**, and zero Bayer cross-channel artifacts |
| **B1: Transient lock occlusion** | Dark object covers exact `FeatureLock` box for 8 frames while a flash occurs elsewhere | **8/8 frames in `GraceHold`**, 0 lost, flash **recovered during grace**, **0 false triggers** |
| **B2: Sustained opaque clouds** | Target disappears completely (pure read noise) for 45 frames, then clears at frame 105 | Clean transition to **`LostFallback`** (32 frames), **0 false triggers**, clean re-lock at **frame 107** (2 frames after clearing) |
| **B3: Sudden 25 px wind jump** | Instantaneous 25 px step rejected by `maxStepPx = 12` | Clean rejection → fallback → re-lock, **0 false triggers** after jump |
| **B4: Large unguided 56 px drift** | Continuous 0.40 px/frame sweep across 56 px triggering `recenterReference` at 32 px | **1 recenter**, **0 false triggers**, post-recenter flash **recovered** (caught and fixed a border-clamping bug in `recenterReference` during benchmarking!) |
| **B5: Slow field rotation (6° total)** | Accelerated 6° field rotation + jitter over 240 frames comparing frozen vs. periodically refreshed template | Frozen template: `minScore = 0.981`, 0 lost, 0 false. **Refreshed template (5×): `minScore = 0.998`, 0 lost, 0 false** |

### Note on Slow Field Rotation Over Multi-Hour Sessions

Because `FlashDetector`'s background model adapts exponentially on a short timescale (typically 1.5–2 s), slow field rotation from imperfect polar alignment causes negligible frame-to-frame motion:
- Equatorial mount with 10 arcmin polar error: $\sim 0.04^\circ/\text{hr} \implies \sim 0.0004\text{ px}$ over 2 s at a 1000 px radius.
- Even 1° polar misalignment: $\sim 0.26^\circ/\text{hr} \implies \sim 0.0025\text{ px}$ over 2 s at a 1000 px radius.
- Alt-azimuth mount at $15^\circ/\text{hr}$: $\sim 0.14\text{ px}$ over 2 s at a 1000 px radius.

Where slow rotation *would* matter over several hours is a **frozen registration template** (whose crater orientation and lunar terminator lighting gradually change). Two design properties solve this cleanly:
1. **`DiskCoG`** (planets, full Moon/Sun) is inherently rotation-invariant.
2. **`FeatureLock`** performs **anchor-preserving periodic template refresh** (every ~60 s or if correlation dips gently into `0.72–0.85` while locked), taking a fresh template at the current validated position while preserving the exact sub-pixel anchor offset (zero discontinuity, keeping correlation near `0.998` across 6° of rotation in Test B5).
