# Feature-10: unattended camera soak tool

`SeleneWatchCameraProbe soak NEW_OUTPUT_DIRECTORY CONFIG.json` runs the normal ZWO Session, detector, ring and writer with periodic manual captures. Default duration is one hour, capture interval two minutes, and progress interval ten seconds. Every delivered frame is still analysed, and automatic candidates still use the normal evidence path. This is bench validation; an hour on the Moon remains a separate acceptance check.

Example configuration:

```json
{
  "id": 0,
  "raw16": true,
  "seconds": 1800,
  "exposureUs": 1000,
  "gain": 0,
  "offset": 9,
  "bandwidth": 80,
  "sigma": 6,
  "manualIntervalSeconds": 120,
  "sampleIntervalSeconds": 10,
  "maximumOutputBytes": 8589934592,
  "minimumFreeBytes": 4294967296
}
```

The output directory must be new or empty. `progress.jsonl` is flushed after each snapshot and records elapsed steady time, host UTC, acquired frames, known/unknown SDK drops, charged image/analysis usage and peak/refusals, writer backlog and buffered bytes, candidates, and output size. On Windows it also records process working set and private bytes; these include memory outside the application's allocation budget. A starting/stopped snapshot brackets the run. `bench.json` retains the final settings, summary, stop reason, completed clips and native-byte verification results. The original `bench` command retains its dense capture behavior.

The tool hashes preview frames for a short window following each accepted manual request, up to 4096 samples, and matches them against saved SER pixels. Periods between captures are deliberately unsaved unless an automatic candidate occurs. Output frame counts and times therefore cannot measure full-run acquisition rate; use acquired-frame/progress counters for that. Saved-frame counts may also include overlap. Process-memory samples and preview detector timings are diagnostics, not exhaustive traces or proofs of no leak.

Once per second the tool checks total output size and free disk space. Reaching the configured output threshold or free-space reserve stops acquisition visibly. Available partial clips and queued evidence are allowed to finish; the size can exceed the threshold by those final writes. The default 4 GiB free-space reserve leaves room for the default 2 GiB image/analysis budget's retained evidence. No captured clip is deleted to enforce a quota. A writer refusal/failure also stops the run. These checks protect unattended bench output; they do not change production capture policy or suppress candidates.

`seconds` is limited to one hour per invocation. Optional `frames` ends sooner. `historySeconds`, `postSeconds`, and `maxPostSeconds` default to the normal 5/3/10-second settings; short constructed tests override these to exercise real periodic writes and partial endings. `readDelayMs` remains an explicit TEST-only overload decorator. The tool does not move a mount, guide, update firmware, install drivers or unplug a camera.

The `camera_probe` CTest target uses the deterministic SDK boundary and exercises the actual command: native RAW8/RAW16 periodic clips, progress parsing, sampled byte matches, output-limit stops, disconnect partial endings, invalid settings, and protection of existing output. It never accesses camera hardware. Hardware soak results are recorded separately when completed.
