# SeleneWatch Camera Settings & Histogram Guide

Practical camera, exposure, gain, offset, ROI, and histogram recommendations for SeleneWatch's four observing workflows—with a focus on **maximizing signal-to-noise ratio (SNR) for lunar impact flashes** on the Moon's earth-illuminated side (earthshine).

For step-by-step operating instructions inside SeleneWatch, see the [User Manual](MANUAL.md).

---

## 1. Lunar Impact Flashes (Earthshine / Dark Side of the Moon)

### 1.1 The Golden Rule: Where Should Earthshine Sit on the Histogram?

Place the **earthshine surface** at **3 % to 10 % of full scale** on the raw linear histogram (**View → Histogram**), while keeping off-limb dark sky slightly above zero (`1–2 %`, with **`At 0`** reading **`0.00%`**) and allowing the sunlit crescent to saturate:

| Stream Format (`View → Histogram` range) | Off-Limb Dark Sky | Earthshine Surface Target (`3 % – 10 %`) | Sunlit Crescent / Glare |
| --- | --- | --- | --- |
| **`RAW16`** live camera / 16-bit SER (`0–65535 ADU`) | `500 – 1500 ADU` (`~1–2 %`) | **`2000 – 6500 ADU`** | Saturates at `65535` (expected) |
| **`12-bit low`** SER recording (`0–4095 ADU`) | `40 – 90 ADU` (`~1–2 %`) | **`120 – 410 ADU`** | Saturates at `4095` (expected) |
| **`RAW8`** (`0–255 ADU`) | `3 – 6 ADU` (`~1–2 %`) | **`12 – 25 ADU`** *(5–10 %)* | Saturates at `255` (expected) |

```text
Counts
  ^
  |    [1] Dark sky      [2] Earthshine
  |     (1-2% ADU)        (3-10% ADU)                     [3] Sunlit glare
  |        ||                /---\         ~90% headroom      (saturated)
  |        ||               /     \       for flashes            ||
  +--------++--------------+-------+-----------------------------++----> Native ADU
  0       At 0            3%      10%                        max code
         (0.00%)                                              (100%)
```

*(Note: Tick **Log counts** in **Histogram** so the small dark-sky and earthshine humps stay tall and easy to read even when saturated sunlit pixels sit at `max code`, and toggle **Fit range** briefly if you want to zoom directly into the earthshine distribution.)*

---

### 1.2 Why *Not* Push the Earthshine Histogram to 40–50 %?

Unlike deep-sky or planetary imaging of a steady target, a **lunar impact flash is extremely brief** (`15–100 ms`, typically lasting only **1 to 3 frames**), whereas earthshine and scattered sunlight are steady backgrounds:

1. **Why longer exposures (`> 50–100 ms`) hurt flash detection:**
   - Suppose an impact flash lasts `20 ms`. A `20 ms` exposure captures 100 % of that flash's photons in a single frame.
   - If you lengthen the exposure to `200 ms` to push earthshine up to 50 % of the histogram, you collect **the exact same number of flash photons**, but **10× more earthshine and scattered-light photons**.
   - Because background Poisson shot noise grows as `√background`, collecting 10× more background raises the noise by `√10 ≈ 3.16×`—**cutting your flash SNR (σ) by more than a factor of 3** while also smearing the event's UTC timing.
2. **Why cranking digital gain to 50 % hurts dynamic range:**
   - Once your camera is above its low-read-noise (HCG) knee and earthshine noise is **2–3× higher than read noise**, raising digital gain further does not collect a single extra photon—it multiplies flash signal and background noise equally.
   - Meanwhile, excessive gain **throws away dynamic range (headroom)**:
     - Medium and bright flashes clip at `255` / `65535`, preventing accurate peak brightness and impactor energy photometry.
     - The saturated glare zone spilling from the sunlit crescent expands much farther across the dark earthshine limb, blinding a larger fraction of the Moon.

> **Use Display Stretch for your eyes, not camera gain:** Keep raw earthshine at **3–10 %** on the histogram, and switch **Display and capture → Display** to **Auto-stretch** (or set **Black / white levels** around the earthshine ADU range). You get a bright, high-contrast live view on screen while the detector and saved `event.ser` keep 90 %+ linear headroom.

---

### 1.3 Recommended Camera Parameters for Lunar Flashes

| Parameter | Recommended Setting | Rationale |
| --- | --- | --- |
| **Exposure time** | **`16 ms` – `33 ms`** (`30` – `60 fps`) | Standard cadence used by professional lunar impact surveys (NELIOTA, MIDAS, NASA Marshall/ALPO). Matches typical `20–50 ms` flash peaks for maximum contrast while resolving multi-frame decay. |
| **Gain** | **Sensor HCG / unity knee + modest boost** | Set gain to the sensor's High Conversion Gain (HCG) point where read noise drops to `~1–2 e⁻`, then adjust slightly until earthshine reaches **3–10 %** (`RAW16`) or **5–10 %** (`RAW8`). |
| **Offset / Black level** | **Keep `At 0` at `0.00%`** | Raise camera **Offset** until the leftmost dark-sky edge clears `0 ADU` (`At 0 0.00%`). Clipped zero pixels break local noise estimation and can hide faint limb flashes. |
| **Pixel format** | **`RAW16`** (preferred) or **`RAW8`** | Use **`RAW16`** whenever your USB 3 bus and SSD sustain `30+ fps`: it eliminates coarse 8-bit quantization steps on dim earthshine and resists glare saturation near the terminator. On USB 2 or **`RAW8`**, aim slightly higher (**6–10 %**, `15–25 ADU`) so `1 ADU` quantization steps stay smaller than pixel noise. |
| **ROI & USB Bandwidth** | **Tight ROI around earthshine; USB bandwidth tuned with 0 SDK drops** | Crop excess empty sky with **ROI** to hold `30–60 fps` comfortably without saturating USB/disk bandwidth; monitor **SDK drops** in the bottom status bar during Preview. |
| **Lunar phase window** | **10 % – 50 % illumination** *(waxing ~2–6 days after New Moon, or waning ~2–6 days before New Moon)* | Wide visible earthshine area with manageable sunlit glare and practical elevation after dusk or before dawn. |

---

### 1.4 Framing the Burned Sunlit Side & Guiding

The sunlit lunar surface is roughly **10,000× brighter** than earthshine. **Letting the sunlit side saturate is normal and unavoidable.** To minimize its impact:

1. **Keep most of the sunlit crescent *outside* the field of view:**
   - Frame the dark earthshine limb so the bright sunlit crescent sits just outside the sensor (or barely grazes one edge).
   - Keeping bright sunlit terrain out of the telescope's baffle tube dramatically reduces internal optical scatter and stray glare across the dark side.
2. **Avoid wasting sensor area on flat saturated white (`255` / `65535`):**
   - Saturated pixels have zero variance—they cannot detect flashes and cannot be used for guiding.
   - Because SeleneWatch estimates noise **per pixel**, the bright scattered-light gradient near the burned crescent automatically receives a higher local noise estimate `σ(x, y)` (preventing false triggers), while darker earthshine farther from the terminator keeps a low `σ(x, y)` and full sensitivity.
3. **Where to place the Feature Lock box (`View → Guiding`):**
   - **Option A — Lock directly on earthshine:** If the sunlit side is completely outside the frame, drag a slightly larger lock box (`80 × 80` to `128 × 128 px`) around a prominent high-contrast earthshine feature (*Aristarchus*, *Copernicus*, *Grimaldi*, *Mare Crisium*, or a curved earthshine limb corner). SeleneWatch's normalized cross-correlation runs on native ADU and tracks earthshine features reliably even at 5 % histogram levels.
   - **Option B — Lock on an unsaturated terminator cusp:** If a corner of the terminator is in view, lock onto an **unsaturated** crater rim near the high-latitude cusp where shadow contrast is crisp in both X and Y—never onto a flat saturated white plateau.

---

### 1.5 30-Second Pre-Monitoring Checklist in SeleneWatch

**Auto-tune does most of this for you.** In Preview, press **Auto-tune…** at the top of the Detection dock and draw a box on the earthshine. **Measure camera** suggests gain (and keeps the exposure at 10–40 ms) for the 3–10 % aim. **Watch** then compares several K values on the same frames and suggests the lowest quiet one ([MANUAL.md, section 2.6](MANUAL.md#26-auto-tune-suggested-camera-settings-and-k)). The aims it uses are the ones in this guide: earthshine 3–10 %, sunlit rims 60–80 % for transits, and the sky 8–30 %. Run it again once the sky around the Moon is dark, and for the sky, run it in full darkness. The checks below are what it measures, if you prefer to do them by hand.

Before pressing **Start monitoring**:

1. **Check `View → Histogram`:**
   - Confirm **`At 0`** reads **`0.00%`** (raise camera **Offset** if `> 0.00%`).
   - Confirm the **earthshine hump** sits around **3–10 %** (`2000–6500 ADU` in `RAW16`, `12–25 ADU` in `RAW8`).
2. **Check local pixel noise & sensitivity (`View → Testing tools`):**
   - Turn on **View → Testing tools**, optionally zoom to **100%** or **200%**, and click a pixel in the middle of the earthshine on the preview.
   - Read **`noise X.XX ADU`** in the **Test flash** bar and **`Noise floor`** in **Detection**:
     - On a `RAW8` stream, earthshine noise is typically **`1.5 – 4.0 ADU`** (well above the `0.5 ADU` minimum noise floor).
     - On a `RAW16` stream (12-bit shifted high into 16-bit words), earthshine noise typically scales by ~16× into **`~25 – 150 ADU`**. *(In **Detection → Sensitivity K**, adjust `K` around `6.0–10.0 sigma`; also note **Minimum noise** defaults to `0.5 ADU` for 8-bit compatibility.)*
   - Select **Kind: Bright flash** (`FWHM 3.0 px`, `Duration 2 frames`) and press **Inject blip** to confirm the weakest ADU flash you want to catch cleanly exceeds your **Sensitivity K** without false triggers from seeing.
   - Turn **View → Testing tools** **OFF** before starting your real patrol.
3. **Save your Detection Profile:**
   - Click **Save…** on the **Profile** row in **Detection** (e.g. `"Moon Earthshine RAW16"`) so you can restore your thresholds in one click on future nights.

---

## 2. Settings for the Other Observing Workflows

### 2.1 Lunar Transits & Shadows (`Watching: The Moon: shadows on the sunlit side`)

- **Goal:** Keep the entire **sunlit lunar disk** unsaturated so aircraft, birds, satellites, or the ISS produce crisp, high-SNR negative contrast dips rather than dipping inside a clipped plateau.
- **Histogram target:** Peak sunlit crater rims around **60 % – 80 %** of full scale (`~150–205 ADU` in `RAW8`, `~40000–52000 ADU` in `RAW16`), with **`At max code` reading `0.00%`** (especially important if recording continuously via **Record** or **Session → Schedule a recording…** for occultations or ISS transits).
- **Exposure & Gain:** Use **low gain** and fast shutter speeds (**`1 – 10 ms`**, `60 – 200+ fps`) so fast transits (`0.5 s` ISS pass) remain sharp across many frames.
- **Detection:** Set **Watching** to **The Moon: shadows on the sunlit side**. Darkening is always on there, and its own settings start at **`K 12`** (raise to `16` if seeing still triggers), `20 px` shadows confirmed over two frames.

### 2.2 Lunar & Planetary Time-Lapses

- **Goal:** Freeze atmospheric seeing per frame (`1 – 15 ms`) with high dynamic range so stacks of the sharpest frames (`stacks/*.fits`) align cleanly and tolerate aggressive **Re-sharpen time-lapse…** deconvolution/USM without noise explosion or blown highlights.
- **Histogram target:** **65 % – 85 %** on the brightest sunlit lunar highland/crater rim or Jupiter/Saturn cloud band, keeping **`At max code`** at **`0.00%`** (`0` clipped white pixels—clipped pixels cannot be recovered by sharpening).
- **Exposure & Burst Window:**
  - **Moon:** `2 – 12 ms` exposure, **Interval** e.g. `60 s`, **Capture window** `10 – 20 s`, **Keep best** `25 %` (cap `100–150` frames). Set **Lunar anchor** on a crater that stays sunlit for the whole run.
  - **Planets (Jupiter/Saturn/Mars):** `3 – 15 ms` exposure; keep **Capture window** **`≤ 60–90 s`** so planetary axial rotation does not blur surface details inside a single stack; **Keep best** `15 %` (cap `150–200` frames).

### 2.3 Wide-Field Sky Watch (Meteors, Fireballs, Lightning, Aircraft)

- **Goal:** Maximize sky coverage and detection sensitivity across the night while leaving headroom for bright meteor flares and lightning, and ignoring horizon clutter via **Draw mask**.
- **Histogram target:** Keep the **unmasked sky median** between **8 % and 30 %** of full scale (`~5000–20000 ADU` in `RAW16`, `20–75 ADU` in `RAW8`), with **`At 0` = `0.00%`**.
  - When **Follow the sky's brightness** is enabled under **Watching: The sky**, SeleneWatch automatically adjusts camera exposure (up to `2×` per step within your `ms` bounds) and then gain (within your cap) whenever the unmasked sky median drifts outside **8 % – 30 %**.
- **Exposure & Gain:** Start around **`33 – 200 ms`** (`5 – 30 fps`) with moderate-to-high gain. Shorter exposures (`33–67 ms`) resolve fast meteors cleanly frame-by-frame; slightly longer exposures (`100–250 ms`) reach fainter stars and faint meteors if frame count stays lower.
- **Sensor Color Note:** Both **mono** and **raw Bayer colour** sensors work identically in **Detection** (per-pixel statistics run on the raw mosaic; **Show Bayer colour** only affects screen display, never evidence).

---

## 3. Quick Reference Summary

| Workflow | Target Histogram Level | Typical Exposure & Rate | Gain & Format | Key Detection / Panel Settings |
| --- | --- | --- | --- | --- |
| **1. Lunar Impact Flashes** *(Earthshine)* | **3 % – 10 %** on earthshine (`At 0` = `0.00%`; sunlit side allowed to saturate) | **`16 – 33 ms`** (`30 – 60 fps`) | HCG / unity gain; **`RAW16`** preferred (or `RAW8` at 5–10 %) | **Watching:** `The Moon` · **K:** `6.0 – 10.0 σ` · **Patch:** `8 – 400 px` · **1-frame trigger:** `On` · **Display:** `Auto-stretch` |
| **2. Lunar Transits & Shadows** *(Sunlit Moon)* | **60 % – 80 %** on brightest sunlit crater rims (`At max code` = `0.00%`) | **`1 – 10 ms`** (`60 – 200+ fps`) | Low gain (maximize dynamic range); `RAW8` or `RAW16` | **Watching:** `The Moon: shadows on the sunlit side` · **K:** **`12.0 – 16.0 σ`** · **Max dark patch:** `20000 px` (or **Record** / **Schedule…**) |
| **3. Lunar & Planetary Time-Lapses** | **65 % – 85 %** on brightest sunlit terrain or cloud belts (`At max code` = `0.00%`) | **`2 – 15 ms`** (`60 – 150+ fps`) | Low-to-moderate gain; `RAW8` or `RAW16` (mono or Bayer colour) | **Time-lapse:** `Moon` (`25 %` best, `150` cap) or `Planet` (`15 %` best, `200` cap, window `≤ 60–90 s`) |
| **4. Wide-Field Sky Watch** *(Fixed Camera)* | **8 % – 30 %** unmasked sky median (`At 0` = `0.00%`) | **`33 – 250 ms`** (`4 – 30 fps` for meteors/aircraft) | Moderate-to-high gain; **Follow the sky's brightness** optional | **Watching:** `The sky` · **K:** `6.0 – 14.0 σ` · **Max patch:** `3000 px` · **Box/patch:** `30 ×` · **Large brightening:** `2 %` · **Learn after:** `20 s` · **Draw mask** on horizon |
