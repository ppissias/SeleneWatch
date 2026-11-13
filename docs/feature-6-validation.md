# Feature-6 validation — 13 November 2026

Parts A–C supplied SER/AVI playback and marked test injection. Part D replaces per-frame PNG evidence with SER, keeps two PNG display previews, and flushes a pending candidate record before writing a clip. The format readers and writers live in `recording/`; acquisition and playback live in `sources/`. Application, tests, and the manual probe share `selenewatch_core`, which has no Qt Widgets dependency.

## SER conventions

- The 178-byte header and every numeric field are little-endian. `LittleEndian=0` follows the convention used by the installed Siril and the observer's FireCapture files.
- Mono8 and RGB use depth 8. Mono16 occupies two bytes per sample. Low-aligned 9–15-bit samples name that depth; high-aligned samples use depth 16 so another reader does not rescale or truncate them. Values are never shifted for evidence output. Exact significant bits and alignment are also recorded in `event.txt`.
- Trailer UTC times use 100 ns ticks since 0001-01-01. The UTC start field is populated; the unknown local-time field is zero. The sidecar also records each original index, exact SER timestamp, and index gaps.
- A new file has header frame count zero. The writer appends the raw frames and time trailer, then finalizes the count. The enclosing clip is marked complete only after the SER, metadata, and display previews have been flushed and closed.
- The header Observer field of a test clip contains `SeleneWatch TEST DATA`. SeleneWatch preserves that source-wide test mark when replaying the SER, even if its sidecar has been separated from it. Original per-frame injection settings remain in the sidecar. This conservative source mark prevents a replayed injection becoming an unmarked observation.
- Camera-specific files from SharpCap, ZWO, and QHY are still unavailable. Their adapters must report their actual layout when those cameras are added; the writer does not assume it.

## Automated checks

Debug and Release CTest runs pass. The SER suite checks nine format/layout combinations, header byte offsets against independent constants, low bits above 255, exact timestamps including sub-millisecond ticks, native pixel round trips, all four Bayer patterns, simulated short/full-disk writes, changed layouts, and the test marker without a sidecar. Existing capture tests still cover overlapping candidates, length caps, EOF/stop, queue refusal, and recovery after a bad clip. Writer tests now assert the pending/outcome record association.

## Independent reader

Installed **Siril 1.4.4** opened five constructed clips (Mono8, Mono16, low-aligned 12-bit, high-aligned 12-bit RGGB, RGB) and a six-frame sample of `2013.06.25 ISS/raw/Moon_202643.ser`. Its sequence report agreed on dimensions, frame counts, sensor formats, depth, UTC start, and ordered trailer timestamps. The real file's output was 1296×964 Mono16, six frames, UTC start 2013-06-25 18:26:43.653. The low-aligned fixture was reported as 12 bits; the high-aligned fixture as 16 bits with RGGB.

The check uses Siril's [headless script interface](https://siril.readthedocs.io/en/stable/scripts/Script-files.html) and its [sequence statistics command](https://siril.readthedocs.io/en/stable/Commands.html#seqstat). It only opens generated files in the workspace. The original archive remains read-only. Detailed logs and CSV files are in `out/qa-feature6d/` on this machine.

## Interrupted write and throughput

A dedicated probe process was killed while writing a large SER. Its file contained 67,109,042 bytes, the header count remained zero, `event.txt` lacked the completion marker, and the flushed `candidate_pending` record survived without a false saved outcome. A later process can identify that record by its UUID. This is process-crash protection for a clip whose pending line has reached the writer. Work still in RAM (including an open clip and waiting writer jobs) can still be lost; `QFile::flush` is not a promise against power loss.

Release playback of `2015.12.21 Moon 8 Newt/.../Moon_165104.ser` processed all 714 frames and saved the same two candidates at frames 316 and 664. Source reading, detection, and collection took 15,922 ms in this run; total including the remaining writer drain was 15,931 ms (9 ms extra). The USB disk and cache affect total playback time, so this is not a camera throughput guarantee. A six-frame 1296×964 Mono16 sample, including two display previews, wrote and shut down in 78 ms. The five small format fixtures together took 28 ms.

The manual `SeleneWatchProbe` target accepts `capture INPUT OUTPUT`, `sample INPUT OUTPUT`, `fixtures OUTPUT`, and `crash OUTPUT`. It is excluded from CTest and requires explicit paths. Its crash mode is solely for an externally interrupted writer check and should target a disposable output directory.
