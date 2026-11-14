# Feature-5 implementation and validation — 14 November 2026

Display stretch, Bayer display, capture durations, and the shared image/analysis memory budget are implemented. Acceptance item 2 is verified with a constructed faint 12-bit recording and native-bit tests; its **real earthshine** example remains pending. The archive's only Mono16 file shows noise rather than usable lunar terrain, including under auto-stretch. This feature stays in `features.md` until that field check can be made. No observational pixels were modified, and no dependency was added.

## Display

The Display and capture tab beside Detection offers Linear/high bits, black/white levels, Auto-stretch, inversion, and Bayer colour. Levels are shown in native ADU. Both sliders and exact controls are available; changes redraw the latest retained raw frame even after Stop. Settings are restored at the next launch. Capture settings apply at the next Start and are disabled during a run.

Linear preserves the former exact high-bit shift. Window mode maps the chosen native black/white range to 0–255. Auto samples at most 8192 pixels, finds the 0.1/99.9 percentiles, and uses a midtones curve targeting a median near 0.25. A deterministic offset within each sample stratum reduces Bayer-phase aliasing. A flat image remains flat. Bayer colour uses bilinear interpolation of each filter's neighbouring samples, with existing neighbours at edges. RGB uses the same display curve on each channel. None of these values reach the detector or native SER write.

The synthetic source now offers Mono8, low/high-aligned Mono12, Mono16, and high-aligned RGGB12. Its Mono16 scene includes a faint half-disk and low native-ADU sky, for testing contrast below the former preview resolution. These are explicitly test sources. The familiar Mono8 painter remains available and the fixed flash and pacing marker keep their detector checks.

## Capture and memory ownership

Defaults are five seconds of history, three seconds of quiet tail, ten seconds maximum after a trigger, and a 2 GiB image/analysis ceiling. Durations use source timestamps, including nominal times when that is all a recording supplies. Fast playback does not shorten them. Long events continue in overlapping clips at the duration or collection-byte limit. Stop, EOF, loop boundaries, memory limits, and failures are recorded as distinct collection endings. `complete: yes` means the files completed; the collection ending says whether the requested tail completed.

`MemoryBudget` has one shared admission counter per Session. `BudgetAllocator` charges numeric buffer capacity before allocation and releases it after deallocation. Frame pixels, model and sparse-work arrays, sigma/noise maps, retained differences, and application-created preview/overlay images share it. Sharing one frame or image counts once; copying its pixels counts another allocation. Source pools use the same budget, and old frames waiting for the writer stay charged after their source is replaced. Reallocation's temporary old/new buffers are both counted.

This is **an image/analysis allocation ceiling, not a process RSS ceiling**. Container metadata, bounded diagnostic histories, Qt/platform caches, file indices, PNG codec workspaces, and allocator overhead are additional. Qt images are backed by charged pixel storage; application code paints only its uniquely owned images. The frame pool's spare payload is included. The UI reports used/limit/peak bytes, allocation refusals, effective history, writer-held clips/bytes, and buffered SER throughput.

The first frame establishes a layout-dependent working reserve:

```text
working = max(limit / 5, pixels * 32 + frameCapacity * 5 + 4 MiB)
half = (limit - working) / 2
history allowance = half / 2
active + queued writer allowance = half
collection threshold = half - one frame - one float difference image
```

A layout that cannot leave room for those buffers is refused. History expires by source time and is shortened further at its byte allowance; the readout identifies RAM clamping. Collection includes its prehistory and is cut before its next frame plus difference could exceed writer admission. These are conservative admission shares; the actual shared counter remains authoritative. The measured writer has considerable headroom on the available recordings, so no larger backlog allocation is justified by them. The shares are not a universal camera guarantee.

A full writer allowance refuses a clip immediately, frees its payload references, and retains its candidate/outcome record. The allowance includes the active disk job. Refused preview maps are optional: detection continues and the panel says why the overlay is absent. A retained-difference allocation failure is recorded and produces a labelled placeholder; it does not change raw evidence. Failure to admit a required source/model allocation stops acquisition visibly. The already open clip is handed off with the failure recorded. There is no silent frame skipping or promise of unlimited recording under overload. The existing pending-journal crash window remains as described in Feature-6 validation.

## Automated and window checks

Six CTest targets pass in Debug and Release. The ten-case display/capture suite checks native low/high-aligned 12/16-bit levels, actual synthetic-source layouts and faint detail, inversion, every Bayer pattern and image edges, source-time duration at different rates, byte-clamped history and time gaps, long-run continuation, shared/copy allocation lifetimes, and an optional-map refusal with identical detector decisions.

Two pressure cases exercise the actual Session and writer. One fills the budget after a candidate, then verifies the failure and a completed clip with the shortened ending recorded. The other sends 800 frames of a long flash through a small budget: the first RAM-split clip has room for its difference image, all 800 frames are analysed, and a continuation is either written or explicitly logged as writer-busy if it arrives before the previous disk job drains. This tests overload rather than assuming fast playback can always write in time.

The actual-window test plays a generated 12-bit high-aligned SER, changes auto/manual/invert controls live, injects one frame, stops, restretches, and restores settings in another window. Every unmodified saved frame matches its original input bytes, and the input file itself is unchanged. The tests render Qt offscreen windows; no manual native-desktop interaction is claimed. Screenshots are in `out/qa-feature5/window-auto.png` and `window-levels-stopped.png`.

## Archive measurements

Release MSVC, Qt 6.12.0, AMD Ryzen 9 9955HX, no concurrent builds/tests. Archive input is read-only. Display figures use twelve cached frames, three repeats, and exclude reading, PNG saving, QWidget composition, and detector work.

| Input | Size / native layout | Linear mean | Window mean | Auto mean |
| --- | --- | --- | --- | --- |
| `Moon_181337.avi` | 1296×964 Mono8 | 2.583 ms | 5.147 ms | 6.367 ms |
| `alccd5_00023.AVI` | 1280×1024 RGB888 | 7.909 ms | 23.318 ms | 25.249 ms |
| `Moon_202643.ser` | 1296×964 Mono16, 13 significant high bits | 3.233 ms | 14.622 ms | 14.051 ms |

The largest RGB conversion remains below the normal 50 ms preview interval on this machine. Acquisition/detection are on a different thread and the preview may skip frames. Bayer throughput at future camera sizes is not established by these archive measurements.

Buffered SER measurements repeat one native frame 128 times to isolate writing from USB input. These generated clips are marked TEST; indices/times repeat and are not an observation. The rate includes the SER header, payload, time trailer, and Qt file flush, but excludes PNG previews. It measures buffered output and OS caching, **not sustained physical-disk speed**.

| Native layout | SER bytes | SER elapsed | Buffered rate |
| --- | --- | --- | --- |
| Mono8 | 159,917,234 | 0.043913 s | 3,641.71 MB/s |
| RGB888 | 503,317,682 | 0.117506 s | 4,283.34 MB/s |
| Mono16 | 319,833,266 | 0.082778 s | 3,863.77 MB/s |

For context, the selected 18 fps Mono8 and 10 fps RGB recordings deliver roughly 22.5 and 39.3 MB/s. Camera runs and a longer physical-disk test remain necessary before relying on a sustained rate. Detailed reports, images, generated SERs, and logs are under ignored `out/qa-feature5/`; a compact measurement record is [feature-5-measurements.json](feature-5-measurements.json).

Siril 1.4.4 independently opened newly written six-frame Mono8, RGB888, and native Mono16 samples. The manual profiler/measurement commands also refuse an output that names the input itself or a hardlink to it; this was checked with disposable generated fixtures.

## Reproduction and remaining acceptance

`SeleneWatchProbe profile INPUT OUTPUT.json` measures the three display modes on a bounded cached prefix and saves example PNGs. `write-benchmark INPUT OUTPUT` writes the marked 128-repeat fixture and prints bytes, SER seconds, buffered MB/s, and total writer time. `sample` now accepts either supported recording format and keeps its six-frame format-check role. Output paths must be in a disposable workspace directory, never the archive.

Acceptance items 1, 3, 4, and 5 are implemented and checked. Item 2's controls, native-bit behaviour, and faint-detail demonstration pass on constructed data; verification on a suitable real earthshine recording is still open. The data gap is shared with Feature-9 and cannot be fixed by stretching already clipped or saturated pixels.
