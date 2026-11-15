# Feature-10 — QHY integration

The QHY adapter is integrated into `main`. It is prepared for the observer's ALccd5L-II colour / QHY5L-II-C. Software tests use a deterministic Bayer camera. Physical camera validation is pending.

## SDK and licensing review

The installed Windows x64 SDK header and DLL identify version **25.6.16.15**. The development probe loaded that installed DLL in place and obtained the same runtime version. Its `version` command invokes the SDK version getter without calling camera enumeration or resource initialization. The source API calls also compile against the supplied header, and the DLL's export table is checked separately.

The installed folder has no SDK-wide license text. The compatibility header refers to a P.J. Plauger copyright and its own license. The vendor documentation and historical repository do not establish redistribution rights for this Windows package. SeleneWatch distributes its own adapter code and uses QHY's Windows driver and x64 DLL installed by the user. Bundling QHY components would require verifying their redistribution rights and notices. [Dependency inventory](../THIRD_PARTY_LICENSES.md), [QHY Windows installation](https://www.qhyccd.com/download/), [vendor SDK documentation](https://www.qhyccd.com/sdk-demo-and-sample-project/), [historical SDK repository](https://github.com/qhyccd-lzr/QHYCCD_Linux)

With `SELENEWATCH_QHY_SDK_DIR` set, only `qhy_sdk.cpp` includes the external header through an absolute private path. This keeps the SDK's compatibility `stdint.h` out of other translation units. CMake requires the header to build the adapter. At first use, SeleneWatch resolves the required exports from the installed DLL. Windows resolves dependencies alongside that DLL and in System32. Builds without the CMake option omit the real binding.

## Finding the installed runtime

Install the Windows driver and SDK through QHY's All-In-One package. Automatic discovery checks for a user-placed `qhyccd.dll` beside SeleneWatch, then `QHYCCD/AllInOne/sdk/x64/qhyccd.dll` under Windows Program Files, and finally `QHYCCD-SDK/x64/qhyccd.dll` on the Desktop. It tries the next location if a candidate DLL is incompatible. [QHY Windows installation](https://www.qhyccd.com/download/)

Select **QHY camera** under Source. **Choose QHY DLL…** saves the absolute x64 DLL path for a custom installation. An invalid saved path reports the problem at that path. **Use automatic discovery** clears the override. The panel shows the SDK version and puts the loaded path in the readback tooltip. The `inspect` probe records both in its JSON output, including when the scan finds no camera. When the runtime loads but the device list is empty, connect the camera, check that Windows recognises it, and refresh.

## Acquisition and evidence

`QhyApi` is the boundary used by the SDK binding and a deterministic TEST implementation. `QhySource` owns the connection and acquisition worker. It configures live mode, disables SDK debayer and reported row denoise and automatic exposure, commands bin 1, and requests supported RAW8/RAW16 transfer and ROI. In Preview the worker applies supported exposure, gain, offset, USB traffic and transfer speed edits between reads and publishes actual readbacks. For a change to or from an exposure of at least one second, it restarts live video on the open connection as described in the SDK guide. Format and ROI changes reopen Preview. Starting Monitoring reopens the camera with a fresh settings snapshot and clears tuning frames from history. Stopping Monitoring closes the current clip and leaves acquisition running. [Preview and Monitoring](preview-monitoring.md), [QHY SDK guide, sections 24–27 and 32](https://www.qhyccd.com/file/repository/latestSoftAndDirver/Demo/QHYCCD%20SDK%20API%20MENU_EN.pdf)

The SDK's required buffer can be larger than the image. The source allocates the full reported length from the shared-budget FramePool, validates every returned width/height/container-depth/channel count, then publishes only the native image extent. Capacity, including the SDK tail, remains charged until its final owner releases it. Constructed tests fill that tail with a sentinel and compare native pixels after SER writing; the tail never becomes image pixels. Unexpected layouts stop acquisition without publishing an unverified frame, and available prior evidence is finished.

RAW16 is interpreted as all 16 delivered bits, low aligned. Advertised ADC depth is recorded separately and its packing is unverified. The existing SER layout declaration preserves these processing units on replay. A raw colour mosaic remains unchanged and its SDK-reported Bayer pattern follows the actual bin-1 ROI origin. Binning beyond 1 is deliberately refused: the inspected API has no independent bin getter, and colour/bin behavior needs hardware verification. Metadata distinguishes the commanded bin from the independently read-back ROI and checked delivered geometry.

The SDK camera ID is recorded as `camera_device_id`, with a separate serial marked unknown. It is not silently treated as a verified serial. Transfer-speed units are also separate from ZWO high-speed mode and bandwidth units. Per-frame UTC is host arrival after the live read, accompanied by host steady time. Exposure start and absolute timing accuracy are unknown.

The inspected live-read interface does not supply a decoded per-frame loss counter. Source indices are therefore host delivery ordinals and **SDK drops remain unknown**, including in progress logs and the window. Arrival intervals are not used to invent losses. Two-frame confirmation means consecutive delivered frames; exposure adjacency cannot be established by this binding.

`GetQHYCCDLiveFrame` has no timeout parameter in the installed header. The source retries generic error/not-ready results for twice the actual exposure plus five seconds and processes mode changes between those returned calls. Other errors end acquisition. A driver call that blocks can delay mode changes and Stop. Physical stop, removal and reconnect behavior need testing.

## Shared code and tests

The second adapter provided concrete shared responsibilities: one settings form for native-unit editing and geometry bounds, one Bayer-phase helper, and one diagnostic runner for periodic/manual capture, resource limits, writer drain and SER verification. The form preserves fractional readbacks, including values offset from a reported step grid. Each vendor retains its own discovery, control/readback profile and SDK lifecycle/error policy. No additional production worker hierarchy is introduced for their different read/telemetry contracts.

The source selector now uses explicit source-kind data, so QHY alone works without relying on a ZWO entry's position. Camera discovery remains lazy. CTest uses only the deterministic SDK boundaries, including in SDK-enabled builds.

The original draft passed 11 CTest targets in six configurations. Checks covered failed initialization and cleanup, invalid formats/bin/ROI/ranges, undersized buffers, exact native words and charged padding, Bayer phase, not-ready and stop behavior, disconnect, start failures, unexpected channels, native manual evidence, opaque ID and unknown losses, fractional readback clamps, and QHY periodic capture through the shared QA runner. [Draft measurements](qhy-development-measurements.json)

## Integration validation — 15 November 2026

All 11 CTest suites passed in Debug and Release with no SDKs, ZWO alone, QHY alone, and both SDKs. The offscreen window checks cover runtime override recovery, simultaneous camera and detection panels, five short-exposure live controls, and SDK readback after quantisation. They also verify camera reopen on Monitoring entry, completed clips with fixed settings, Preview resumption while the SDK returns no frame, and partial evidence after simulated removal. A source test checks that changes to and from a long exposure restart live video on the same connection.

The real probe found the x64 All-In-One runtime at `C:\Program Files\QHYCCD\AllInOne\sdk\x64\qhyccd.dll` and reported version 25.6.16.15. An explicit alternate path was chosen when supplied. The probe reported an empty camera list and gave distinct errors for an absent runtime, a missing override, a 32-bit DLL and an x64 library missing SDK exports. The combined executable's Windows import table and build output contained no QHY DLL.

The combined Release build also passed a short real ASI120MM Mini check. Live exposure, gain, offset and USB bandwidth changes kept Preview running. Each Monitoring run saved a manual clip. All 7 RAW8 and 3 RAW16 saved frames matched the corresponding SDK bytes. Stopping Monitoring kept acquisition running. A final inspection after reopening matched the original camera controls and layout, and the camera was closed.

## Commands for qualification when the observer returns

Configure from the project root in the development PowerShell environment.

```powershell
cmake -S . -B out/build/qhy-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$env:QTDIR" `
  -DSELENEWATCH_QHY_SDK_DIR="C:/dev/tools/SDKs/QHY sdk"
cmake --build out/build/qhy-release
ctest --test-dir out/build/qhy-release --output-on-failure
.\out\build\qhy-release\SeleneWatchQhyProbe.exe version
.\out\build\qhy-release\SeleneWatchQhyProbe.exe inspect out/qhy-inspect.json
```

`SeleneWatchQhyProbe` supports `version`, `inspect NEW_OUTPUT.json`, and `bench/soak NEW_OUTPUT_DIRECTORY CONFIG.json`. Prefix a command with `--dll "C:/path/to/x64/qhyccd.dll"` to override automatic discovery for that invocation. Use a new output filename or directory on each run. Omit the opaque SDK `id` to select the first discovered camera. A one-minute capture can use `{"raw16":true,"seconds":60,"exposureUs":1000,"gain":12,"offset":16,"usbTraffic":10,"transferSpeed":0,"sigma":6}`. Width, height and origin default to the full frame at bin 1. The runner checks saved geometry, format, Bayer pattern, declared sample units and sampled native bytes. It accepts `stop.request` for an early stop that drains the writer. [Camera soak options](camera-soak.md)

With the observer present:

1. Connect QHY, inspect supported modes/ranges and actual ROI/transfer-depth getters. Record any getter the model does not implement; adapt from measured behavior instead of guessing compatibility.
2. Verify RAW8/RAW16, channel count, Bayer phase/orientation on a recognizable colour scene, lower-bit use, crop origin, native bytes and representative controls. Open saved real-camera SER clips independently in Siril.
3. Exercise live Preview tuning, layout changes, entry to and exit from Monitoring, manual capture and Stop. Test physical unplug and reconnect, including the error shown, completed available clips and successful restart.
4. Run sustained QHY bench capture, then a lunar session with each camera of at least one hour. Measure throughput, losses where actually knowable, memory, candidate/writer load and injected recovery on unclipped earthshine and bright terrain. Feature-5/Feature-9 observing acceptance remains open.
