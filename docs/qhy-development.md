# Feature-10: QHY development draft

This draft is isolated on `feature-10-qhy-development`, in `out/worktrees/qhy-development`. It is prepared for the observer's ALccd5L-II colour / QHY5L-II-C. The physical camera has not been connected or tested during this work. The usable main checkout retains the ZWO implementation and soak tool.

## SDK and licensing review

The installed Windows x64 SDK header and DLL identify version **25.6.16.15**. The development probe loaded that installed DLL in place and obtained the same runtime version. Its `version` command invokes the SDK version getter without calling camera enumeration or resource initialization. The source API calls also compile against the supplied header, and the DLL's export table is checked separately.

No SDK-wide license/redistribution grant accompanies the installed folder. A compatibility header contains a P.J. Plauger copyright notice and directs readers to their license; it does not establish QHY DLL redistribution terms. The inspected vendor [SDK documentation and demos](https://www.qhyccd.com/sdk-demo-and-sample-project/) and [download page](https://www.qhyccd.com/download/) did not establish a Windows binary grant either. A [historical SDK repository](https://github.com/qhyccd-lzr/QHYCCD_Linux) was checked for context and did not resolve terms for this Windows package. This remains an explicit requirement before merging/enabling a distributable adapter. The project proposal permits development-machine work while terms are unresolved; the draft follows that scope. No vendor source, headers, libraries, runtime DLLs, firmware, drivers or third-party demo code are added to the repository.

The CMake path is **off by default**. With `SELENEWATCH_QHY_SDK_DIR` set, only `qhy_sdk.cpp` includes the external header through an absolute private path. This avoids putting the SDK's compatibility `stdint.h` into other translation units' include search. The binding resolves the installed DLL's typed functions on demand, and copies no SDK files. The inspected DLL imports Windows components; those imports are resolved from system directories. Bundled old CRT/USB DLLs are not staged beside Qt. There is no runtime QHY import in SDK-free builds.

Vendor clarification still needed: permission to use and redistribute the selected Windows SDK from a BSD-2-Clause application, exactly which files may accompany binaries, all required notices, and the terms for any additional runtime components. No message has been sent to the vendor. Original SeleneWatch code remains BSD-2-Clause and Qt remains dynamically linked under its separate terms. See [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

## Acquisition and evidence

`QhyApi` is the narrow boundary used by the SDK binding and a deterministic TEST implementation. `QhySource` owns the connection and acquisition worker. It configures live mode, disables SDK debayer, disables reported row denoise/automatic exposure controls, commands bin 1, and requests supported RAW8/RAW16 transfer, ROI and supported exposure/gain/offset/USB-traffic/transfer-speed controls. Those values are read back. A changed settings request finishes the old clip and starts a new run through the same Session/detector/ring/writer as ZWO.

The SDK's required buffer can be larger than the image. The source allocates the full reported length from the shared-budget FramePool, validates every returned width/height/container-depth/channel count, then publishes only the native image extent. Capacity, including the SDK tail, remains charged until its final owner releases it. Constructed tests fill that tail with a sentinel and compare native pixels after SER writing; the tail never becomes image pixels. Unexpected layouts stop acquisition without publishing an unverified frame, and available prior evidence is finished.

RAW16 is interpreted as all 16 delivered bits, low aligned. Advertised ADC depth is recorded separately and its packing is unverified. The existing SER layout declaration preserves these processing units on replay. A raw colour mosaic remains unchanged and its SDK-reported Bayer pattern follows the actual bin-1 ROI origin. Binning beyond 1 is deliberately refused: the inspected API has no independent bin getter, and colour/bin behavior needs hardware verification. Metadata distinguishes the commanded bin from the independently read-back ROI and checked delivered geometry.

The SDK camera ID is recorded as `camera_device_id`, with a separate serial marked unknown. It is not silently treated as a verified serial. Transfer-speed units are also separate from ZWO high-speed mode and bandwidth units. Per-frame UTC is host arrival after the live read, accompanied by host steady time. Exposure start and absolute timing accuracy are unknown.

The inspected live-read interface does not supply a decoded per-frame loss counter. Source indices are therefore host delivery ordinals and **SDK drops remain unknown**, including in progress logs and the window. Arrival intervals are not used to invent losses. Two-frame confirmation means consecutive delivered frames; exposure adjacency cannot be established by this binding.

`GetQHYCCDLiveFrame` has no timeout parameter in the installed header. A generic error/not-ready result is retried, with a deadline of twice the actual exposure plus five seconds between returned calls. Other statuses fail visibly. This deadline does not bound a driver call that itself blocks. Stop/physical removal behavior must be tested on the actual camera; process isolation or another supported SDK mechanism is considered if measurements demonstrate a blocking problem.

## Shared code and tests

The second adapter provided concrete shared responsibilities: one settings form for native-unit editing and geometry bounds, one Bayer-phase helper, and one diagnostic runner for periodic/manual capture, resource limits, writer drain and SER verification. The form preserves fractional readbacks, including values offset from a reported step grid. Each vendor retains its own discovery, control/readback profile and SDK lifecycle/error policy. No additional production worker hierarchy is introduced for their different read/telemetry contracts.

The source selector now uses explicit source-kind data, so QHY alone works without relying on a ZWO entry's position. Camera discovery remains lazy. CTest uses only the deterministic SDK boundaries, including in SDK-enabled builds.

All **11 CTest targets pass in six configurations**: Debug/Release without SDKs, with QHY alone, and with both SDKs. New checks cover absent/failed initialization and cleanup, unsupported formats/bin/ROI/ranges, undersized buffers, exact native words and charged padding, Bayer origin, not-ready/stop, disconnect, start failures, unexpected channels, manual native evidence, opaque ID/unknown losses, fractional readback clamps, settings separation across real offscreen window restarts, and QHY periodic capture through the shared QA runner. The window screenshot is `out/qa-zwo/qhy-window.png` in this worktree; it depicts TEST data. [qhy-development-measurements.json](qhy-development-measurements.json) records the matrices, runtime version and the 26 verified SDK exports. Hardware was not opened by CTest.

## Commands for qualification when the observer returns

From the development PowerShell environment (the normal shortcut loads Qt/MSVC), configure using this worktree as the source:

```powershell
cmake -S out/worktrees/qhy-development -B out/worktrees/qhy-development/out/build/qhy-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$env:QTDIR" `
  -DSELENEWATCH_QHY_SDK_DIR="C:/dev/tools/SDKs/QHY sdk"
cmake --build out/worktrees/qhy-development/out/build/qhy-release
```

The worktree's `SeleneWatchQhyProbe` supports `version`, `inspect NEW_OUTPUT.json`, and `bench/soak NEW_OUTPUT_DIRECTORY CONFIG.json`. QHY config `id` is an opaque SDK string; omit it to select the first discovered camera. Example capture config: `{"raw16":true,"seconds":60,"exposureUs":1000,"gain":12,"offset":16,"usbTraffic":10,"transferSpeed":0,"sigma":6}`. Width/height/origin default to a full frame at bin 1. Soak options match [camera-soak.md](camera-soak.md). The QA runner independently checks saved geometry, format, Bayer pattern, declared sample units and sampled native bytes. It preserves existing output and supports `stop.request` for graceful early stopping.

With the observer present:

1. Connect QHY, inspect supported modes/ranges and actual ROI/transfer-depth getters. Record any getter the model does not implement; adapt from measured behavior instead of guessing compatibility.
2. Verify RAW8/RAW16, channel count, Bayer phase/orientation on a recognizable colour scene, lower-bit use, crop origin, native bytes and representative controls. Open saved real-camera SER clips independently in Siril.
3. Exercise real desktop Apply/restart, manual capture, Stop, physical unplug and reconnect. Check a visible error, usable controls, completed available evidence and a successful fresh connection.
4. Run sustained QHY bench capture, then a lunar session with each camera of at least one hour. Measure throughput, losses where actually knowable, memory, candidate/writer load and injected recovery on unclipped earthshine and bright terrain. Feature-5/Feature-9 observing acceptance remains open.
5. Resolve SDK redistribution terms before merging a distributable QHY implementation. This draft has not been represented as hardware-qualified or release-ready.
