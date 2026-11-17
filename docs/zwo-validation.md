# Feature-10: ZWO implementation and bench validation

Validated on 14 November 2026. ZWO acquisition, camera controls and native evidence are implemented. Feature-10 remains open for QHY, physical unplug and one-hour lunar sessions. The camera connected by the observer identifies as **ZWO ASI120MM Mini**, rather than the original ASI120MM named in the proposal. It reports USB2, 1280×960, mono, 12-bit ADC, bins 1/2 and RAW8/RAW16. SDK runtime: **1.41.0.0** (`1, 41, 0, 0`).

## Implementation

`cameras/zwo_sdk.cpp` is the only vendor-header consumer. `ZwoApi` is a narrow boundary with an actual deterministic test implementation. `ZwoSource` owns opening, configuration, readbacks, capture and close. It fills budgeted `FramePool` buffers directly, then uses the existing `Session`, detector, ring and asynchronous writer. No shared camera base/helper has been invented ahead of QHY.

The optional Camera tab discovers devices when selected/refreshed and shows reported ranges. Exposure is displayed in milliseconds and sent as integer microseconds. Gain, offset, bin, ROI, raw format, USB bandwidth and high-speed mode are offered where supported. The Mini does not report a high-speed control, so none is shown. Automatic controls, SDK dark subtraction and flip are disabled. Processed RGB/Y8 and unverified raw colour binning beyond 1 are refused. Bayer origin follows the actual ROI readback at bin 1.

Every Apply while running finishes the old clip with `camera settings changed`, closes the device and starts a new run. Metadata uses actual values, including SDK clamps, instead of the request. Invalid geometry is rejected before stopping a healthy stream. SDK-call failures close the device; polling uses 100 ms timeouts, with an overall no-frame limit of twice the read-back exposure plus five seconds. The window remains usable after a simulated disconnect.

Each frame records host UTC immediately after the SDK read and a corresponding steady-clock time. These are arrival times, including camera readout, USB and SDK buffering; they are not exposure start times. No GPS accuracy or measured absolute clock accuracy is claimed. The SDK reports cumulative drops but no per-frame hardware ID. The source combines delivery ordinal and observed loss count into a derived index. Gaps are approximate in placement because the SDK buffers frames. Per-frame loss telemetry and those semantics are in the sidecar; the status bar retains the session drop count after Stop. A failed/reset loss counter preserves the received native frame with unknown telemetry, reports failure, and ends acquisition.

## RAW16 interpretation and replay

The adapter retains **every delivered word bit** and interprets RAW16 as 16 bits, low aligned. Advertised sensor ADC depth is a separate field. This is a conservative interpretation, not a claim that the sensor has 16-bit precision or that its ADC packing is verified.

At bin 1 the combined native word bits were `0x3ff0` in the measured sequences: the low four bits happened to be zero. At bin 2, 640×480, gain 20 and offset 12, the combined value was **`0x7ffc`**, including bits 2 and 3. A blanket 12-high-bit interpretation would discard meaningful delivered data. Zero bits in a sampled scene do not establish a vendor packing contract; no lossy shift or rescale is applied.

New SER files preserve the chosen interpretation in the standard Observer text field, for example `SeleneWatch;layout=16L`. TEST files retain `SeleneWatch TEST DATA` before that suffix, including when a later frame introduces test data. Replaying these files validates the declaration against storage depth and keeps the live detector's units. A regression uses a quiet 16-bit-word sequence whose low bits are zero: live and replay decisions and peak sigma agree. Malformed/contradictory declarations are refused. Foreign and older files retain the established inference path. Previously written clips remain unchanged.

## Hardware results

Measurements used Release/MSVC 19.51, Qt 6.12.0, Ryzen 9 9955HX, approximately 16 GB physical RAM, with no concurrent build/test load. Session defaults were 5 s history, 3 s quiet tail, 10 s maximum post interval and a 2 GiB image/analysis allocation ceiling. These were nonastronomical bench runs; illumination and sensor temperature were not controlled.

| Run | Format / geometry | Acquired | Arrival fps | SDK drops | Charged peak MB / refusals |
| --- | --- | ---: | ---: | ---: | ---: |
| One minute, bandwidth 80 | RAW8, 1280×960, bin 1 | 1662 | 28.020 | 0 | 647.6 / 0 |
| One minute, bandwidth 80 | RAW16, 1280×960, bin 1 | 830 | 14.001 | 0 | 630.4 / 0 |
| Injection, bandwidth 40 | RAW16, 1280×960, bin 1 | 50 | 7.001 | 0 | 141.3 / 0 |
| Binned ROI, bandwidth 80 | RAW16, 640×480, bin 2 | 41 | 14.005 | 0 | 28.0 / 0 |
| Induced overload, 15 s | RAW8, 1280×960, bin 1 | 56 | see note | 343 | 65.1 / 0 |

Normal full-frame runs used exposure 1000 µs, gain 0, offset 9. The bin-2 case used gain 20, offset 12. Arrival fps is `(distinct saved frames − 1) / first-to-last saved UTC span`, excluding startup. Startup-inclusive rates for the minute runs were 27.69 and 13.83 fps. These are observed rates for this configuration, not guarantees for other exposures or USB settings.

The manual QA probe delayed each SDK read by 250 ms in the overload case. All those frames are marked TEST source data. This deliberately reduced delivery to roughly 4 fps while the camera continued producing frames; the source observed 343 SDK losses and the clips contained 74 gap lines including overlap. This checks real SDK loss propagation. A separate offscreen window test checks the on-screen count and saved gap metadata with the deterministic camera. The overload experiment does not simulate physical USB removal or measure the camera's normal frame rate.

The live injection added one Gaussian frame, FWHM 4 pixels, peak 8192 delivered-word units, at K=6. A new candidate matched a sample actually brightened by the injection, and the saved frame and clip were marked test data. This demonstrates the live path; it is not a sensitivity calibration or a false-positive measurement on the Moon.

SHA256 checks matched up to 20 preview-frame buffers per run against their saved SER pixels. All 20 matched in both minute runs, the injection, bin-2 and overload cases. The short final RAW8 check delivered 16 preview samples; all 16 matched. The preview may skip frames; the source still sends every delivered frame through detection. This is sampled hardware-byte verification, complemented by exhaustive native-byte tests on constructed frames.

Preview-notification samples measured detector mean cost at 5.276 ms for the RAW8 minute and 5.369 ms for RAW16, with p95 7.549 and 8.676 ms respectively. These samples exclude QWidget composition and display conversion, and do not form an exhaustive latency trace. Charged memory is image/analysis payload, not process RSS. Buffered SER byte/time counters include repeated frames in overlapping clips and Qt flush; they do not establish durable physical-disk throughput. Every hardware run finished without a failed/refused clip or memory admission refusal. Detailed values, configs, actual controls and these limits are in [zwo-measurements.json](zwo-measurements.json); serial is omitted from that versioned summary.

## Verification and reproduction

Nine CTest targets pass in **Debug and Release, each with and without the ZWO SDK**. CTest never opens a physical device: camera tests use `FakeZwoApi`, and other sources do not trigger discovery. Tests cover partial-open cleanup, native buffer/readbacks, bounds and unsupported layouts, Bayer origin, timeout/stop, disconnect, losses/reset/telemetry failure, manual SDK metadata, replay units and actual-window Apply/restart/failure behavior. Existing detection, memory-pressure, continuation, SER, preview and source-boundary tests remain in place. Window rendering uses Qt offscreen; no manual desktop interaction is claimed. The inspected screenshot is `out/qa-zwo/window-simulated.png`.

**Siril 1.4.4** independently computed statistics for every frame of new RAW8 (31 frames), RAW16 (30), and induced-overload TEST (43) files. Independent header/trailer checks confirmed 1280×960, 8/16-bit storage, exact lengths, strictly increasing UTC trailer times, header start time and exact trailer/sidecar tick agreement. Those files include the new Observer declaration. The loss experiment's source indices remain in the sidecar; SER itself has no per-frame index field.

Build commands are in [DEVELOPER.md](../DEVELOPER.md) and observer controls are in [MANUAL.md](../MANUAL.md). `SELENEWATCH_ZWO_SDK_DIR` is empty by default. With it set, CMake requires the SDK header, x64 library/DLL and license, and copies the DLL and complete notice to the build directory. [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md) records the installed SDK's MIT-worded notice and redistribution obligations. No SDK binaries, captured frames or new image-processing dependency are added to the repository.

The manual probe is built as `SeleneWatchCameraProbe`:

```text
SeleneWatchCameraProbe inspect OUTPUT.json
SeleneWatchCameraProbe bench NEW_OUTPUT_DIRECTORY CONFIG.json
```

An example config is `{"id":0,"raw16":true,"frames":100000,"seconds":60,"exposureUs":1000,"gain":0,"offset":9,"bandwidth":80,"sigma":6}`. Optional `bin`, `width`, `height`, `x`, `y` select output-pixel ROI. `inject:true` and `amplitude:8192` request the one-frame check. `readDelayMs:250` is a manual QA-only overload decorator that marks the source TEST; the production source has no artificial delay. The probe requests manual captures through the same Session and refuses a nonempty output directory. Full reports, original serial, SER files and build logs remain under ignored `out/qa-zwo/`.

## Remaining acceptance

- QHY adapter, its license/redistribution terms, Bayer/packing checks and loss/timing semantics.
- Physical unplug while streaming and a manual desktop/real-camera controls check. Deterministic disconnect and worker cleanup are covered; USB removal is not.
- At least one hour on the Moon for each camera, including stable throughput, drops, candidate load, detector cost, write performance and sensitivity measurements on suitable unclipped earthshine and bright terrain.
- Camera-specific colour binning verification, precise timing/clock-state reporting, and the existing Feature-9/Feature-5 observing checks.

Feature-10 remains in `features.md`. No historical feature section has been moved or condensed.
