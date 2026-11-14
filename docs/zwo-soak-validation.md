# Feature-10: unattended ZWO soak results — 14 November 2026

The connected ASI120MM Mini completed **two 30-minute bench runs**, first RAW8 and then RAW16. Both ran the normal Session/detector/history/writer with a manual clip every two minutes. Every delivered frame was analysed. These were unattended bench checks with concurrent software builds/tests; the one-hour lunar observing acceptance remains open.

| Result | RAW8 | RAW16 |
| --- | ---: | ---: |
| Acquired frames | 50,417 | 25,192 |
| Startup-inclusive frames/s | 28.009 | 13.996 |
| Frames/s from progress after warm-up | 28.020 | 14.000 |
| SDK-reported drops | 0 | 0 |
| Memory admission refusals | 0 | 0 |
| Sampled native hashes matched | 210 / 210 | 210 / 210 |
| Native frames saved in periodic clips | 3,253 | 1,633 |
| Charged image/analysis peak, MB | 292.5 | 296.2 |
| Sampled process private maximum, MB | 218.3 | 216.2 |
| Output at final measurement, GB | 4.006 | 4.022 |

Both used full-frame 1280×960, bin 1, 1000 µs exposure, gain 0, offset 9 and bandwidth 80, with actual SDK readbacks recorded. SDK version was 1.41.0.0. The fixed probe executable was built from `46eff8e`; its SHA256, configs, control readbacks, timing/memory checkpoints and test matrices are retained in [zwo-soak-measurements.json](zwo-soak-measurements.json). Camera serial and full clips/logs remain in ignored `out/qa-soak/`.

The normal 5 s history, 3 s quiet tail, 10 s maximum post interval and 2 GiB allocation ceiling were retained. Each run had an 8 GiB output stop threshold and 4 GiB free-space reserve. These limits were not reached. Periodic saving avoided recording the entire hour; automatic detection and its evidence path remained enabled. Every finished clip succeeded, with no writer refusal or record failure. At the final stopped snapshot the writer had drained.

Process memory was sampled every ten seconds, so its reported maximum can be lower than the independently tracked charged-payload peak: brief clip collection/writing peaks fall between samples. After the first minute, sampled private usage ranged from 206.4–218.3 MB in RAW8 and 208.6–216.2 MB in RAW16. First-to-last streaming samples after that warm-up increased by 4.5/5.6 MB. These bounded observations do not prove absence of leaks over longer sessions. Sampled writer backlog was zero; that cadence does not measure the actual queue peak. Buffered write counters do not measure durable disk throughput.

Illumination and sensor temperature were not controlled. No candidates fired in either bench run; this does not establish a false-positive rate, sensitivity or calibrated observing threshold on the Moon. No signal was injected in these long runs. Earlier live injection checks are in [ZWO validation](zwo-validation.md).

Native byte sampling occurred during short windows following periodic manual requests. All 420 hashes matched saved pixels. This is sampled preservation evidence, complemented by exhaustive constructed native-pixel tests; it is not an independent original copy of every acquired frame. Saved clips intentionally omit the intervals between captures, so their times do not define full-run acquisition fps. Progress counters provide that rate.

Siril 1.4.4 independently read every frame of the first and last clips from both runs: RAW8 89/226 frames and RAW16 46/113 frames. Independent checks confirmed dimensions, storage depths, exact lengths, declared sample interpretation, strictly increasing UTC trailer ticks and exact agreement with sidecars. SER hashes were unchanged after Siril. The available timing remains host arrival, with SDK buffering and PC-clock accuracy unmeasured.

All **10 CTest targets pass in Debug and Release, with and without the ASI SDK**. The new `camera_probe` tests use a deterministic SDK implementation and exercise actual periodic native writes, progress, storage limits, disconnect partial endings, invalid input/output protection, and graceful `stop.request` handling. CTest does not open hardware. Qt window rendering remains offscreen.

The camera's initial capture controls and RAW8/full-frame/bin-1 configuration were read back and restored before closing it. A later SDK initialization can select defaults again (bandwidth 80 was observed), so configuration is always read back on Apply/Start. No guide pulses, firmware changes or driver installation occurred.

The [soak tool instructions](camera-soak.md) describe reproduction and stopping. Remaining observer-assisted checks are real desktop operation, physical unplug/reconnect, QHY qualification, and an hour on the Moon with each camera, including Feature-5 earthshine display and Feature-9 injected recovery on suitable scenes. Feature-10 remains open; feature history and the project license are unchanged.
