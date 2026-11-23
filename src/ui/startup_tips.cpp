#include "ui/startup_tips.h"

namespace {
constexpr auto kSectionMainScreen = "12-the-main-window";
constexpr auto kSectionDrivers = "11-installing-and-connecting-a-camera";
constexpr auto kSectionOutput = "13-where-selenewatch-saves";
constexpr auto kSectionSite = "14-your-site-and-instrument";
constexpr auto kSectionSource = "15-choosing-a-source-and-starting-preview";
constexpr auto kSectionDisplay = "16-seeing-the-image-stretch-and-histogram";
constexpr auto kSectionAutoTune = "17-auto-tune";
constexpr auto kSectionEnds = "18-ending-a-session-by-itself";
constexpr auto kSectionPhone = "19-checking-on-selenewatch-from-your-phone";
constexpr auto kSectionGuides = "6-feature-lock-and-guiding";
constexpr auto kSectionRecording = "7-recording-every-frame";
constexpr auto kSectionLunar = "5-impact-flashes-on-the-moons-dark-side";
constexpr auto kSectionTransits = "4-shadows-crossing-the-moon";
constexpr auto kSectionTimelapse = "3-time-lapses-of-the-moon-and-the-planets";
constexpr auto kSectionTimelapseImprove = "improving-it-afterwards";
constexpr auto kSectionTimelapseTransits = "watching-for-transits-during-a-moon-time-lapse";
constexpr auto kSectionSky = "2-watching-the-sky";
constexpr auto kSectionReport = "the-nightly-report";
constexpr auto kSectionOutputs = "8-the-report-and-the-saved-files";
constexpr auto kSectionTestSignals = "9-test-signals";
}

const std::vector<StartupTip>& allStartupTips()
{
    static const std::vector<StartupTip> tips = {
        // ── Phase 1: First-session essentials ────────────────────────────────
        {
            "Preview vs. Monitoring",
            "Use <b>Preview</b> (blue pill) to focus, tune camera controls, and set thresholds safely without "
            "writing event clips. When ready, switch to <b>Monitoring</b> (green pill)&mdash;camera settings lock "
            "for evidence integrity while automatic capture runs. Press <code>F1</code> anytime to open the built-in "
            "<b>User Manual</b>.",
            kSectionMainScreen
        },
        {
            "Inline help beside every control",
            "Not sure what a setting does? Click the small <b>&#x24D8;</b> icon beside a control's name&mdash;or "
            "choose <b>Help &rarr; What's This?</b> and click any widget&mdash;to read a short explanation right in "
            "place, without leaving your live view.",
            kSectionMainScreen
        },
        {
            "One organized output folder",
            "Set one root directory under <b>Session &rarr; Output folder&hellip;</b>. SeleneWatch keeps all lunar "
            "and sky sessions, continuous recordings, and time-lapses neatly organized beneath it&mdash;and warns "
            "against cloud-synced folders or missing drives before an overnight run starts.",
            kSectionOutput
        },
        {
            "Camera drivers & Connect at startup",
            "Open <b>Session &rarr; Camera support&hellip;</b> while stopped to check ZWO, QHY, Player One, and "
            "ToupTek x64 SDK discovery&mdash;or choose a custom vendor DLL. Once your usual camera is chosen, tick "
            "<b>Connect at startup</b> in the top bar so SeleneWatch selects it automatically on launch.",
            kSectionDrivers
        },
        {
            "Auto-tune camera & detection",
            "Press <b>Tune camera &amp; detection&hellip;</b> in <b>Detection</b> during <b>Preview</b> to let "
            "SeleneWatch measure your live scene and suggest exposure, gain, sensitivity <b>K</b>, and minimum patch. "
            "For lunar impact flashes, it also measures the faintest flash it would catch using a marked test flash.",
            kSectionAutoTune
        },
        {
            "Display controls never alter saved pixels",
            "Switching to <b>Auto-stretch</b>, moving <b>Black / white levels</b>, ticking <b>Invert brightness</b>, "
            "or toggling <b>Show Bayer colour</b> in <b>Display</b> only changes what appears on screen. "
            "Saved <code>.ser</code> videos and the detector always read unmodified linear camera pixels.",
            kSectionDisplay
        },
        {
            "Zoom, pan, Focus view & Night theme",
            "Use the <b>View: Fit / 100% / 200%</b> bar above the preview&mdash;or roll the <b>mouse wheel</b> over "
            "the live image&mdash;to zoom around the cursor and <b>drag</b> to pan. Press <b>Focus view</b> "
            "(<code>F11</code>) to hide all side panels for a full-window live view, and switch between <b>Dark</b>, "
            "<b>Red night vision</b>, and <b>System</b> under <b>View &rarr; Theme</b>.",
            kSectionDisplay
        },
        {
            "Earthshine exposure & sunlit ceiling",
            "For lunar impact flashes, open <b>View &rarr; Histogram</b> in <b>Preview</b> and expose so earthshine "
            "sits around <code>3&ndash;10%</code> of full scale with <b>At 0</b> at <code>0.00%</code>. "
            "<b>Ignore brighter than</b> (<code>30%</code> default) automatically excludes the bright sunlit crescent "
            "and terminator glare while following lunar drift.",
            kSectionLunar
        },
        {
            "Live markers on threshold sliders",
            "During <b>Preview</b>, <b>Sensitivity K</b>, <b>Minimum patch</b>, and <b>Maximum patch</b> show live "
            "markers right on their tracks: a teal tick for the current frame and an amber triangle holding recent "
            "peaks (while <b>Maximum box / patch</b> and <b>Minimum noise</b> track the current frame). Watch them "
            "beside the 10-second <b>Peak sigma</b> graph to place thresholds cleanly above seeing.",
            kSectionLunar
        },
        {
            "Diagnosing triggers with overlays",
            "Not sure why a threshold is firing in <b>Preview</b>? Switch <b>Preview overlay</b> in <b>Detection</b> "
            "to <b>Sigma map</b> (pixels at or above <b>K</b> light up red) or <b>Noise map</b> to spot drifting "
            "lunar limbs, hot pixels, or noisy edges at a glance. Overlays affect the display only.",
            kSectionLunar
        },
        {
            "Four independent watches",
            "The <b>Watching</b> selector at the top of <b>Detection</b> keeps separate settings for <b>The Moon: "
            "flashes</b>, <b>The Moon: shadows</b>, <b>The sky: fast events</b>, and <b>The sky: slow and faint</b>. "
            "Switching watches in <b>Preview</b> immediately applies that watch's thresholds without disturbing the others.",
            kSectionLunar
        },
        {
            "Named detection profiles",
            "Save named threshold presets with <b>Save&hellip;</b> on the <b>Profile</b> row at the top of "
            "<b>Detection</b>. Under a sky watch, you can optionally tick <b>Include horizon mask</b> to bundle a "
            "site's roofline mask with the profile, or leave it unticked to switch sensitivity presets without "
            "altering your current mask.",
            kSectionLunar
        },

        // ── Phase 2: Camera depth, ROI, timing, review & rehearsal ───────────
        {
            "Keep full sensor depth with RAW16",
            "Choosing <b>RAW16</b> in your <b>Camera</b> dock preserves every ADC bit delivered by your sensor "
            "instead of scaling to 8 bits. Use <b>View &rarr; Histogram</b> in <b>Preview</b> to verify that sunlit "
            "lunar terrain or planetary bands stay unclipped for transits and time-lapses.",
            kSectionSource
        },
        {
            "Draw a hardware camera ROI on the preview",
            "Need faster frame rates or smaller files when imaging a planet or a single lunar crater? Click "
            "<b>Draw ROI</b> in your <b>Camera</b> dock and drag a box directly on the live preview to crop the "
            "sensor readout, or reset to full frame whenever you need the whole field back.",
            kSectionSource
        },
        {
            "Site, optics & NTP clock check",
            "Open <b>Session &rarr; Site and instrument&hellip;</b> once to save your location and optics: reports "
            "and overviews will then compute angular speeds in <code>&deg;/s</code>, dusk/dawn and Moon-altitude "
            "limits work automatically, and ticking <b>Compare the PC clock with an NTP server</b> logs clock offset.",
            kSectionSite
        },
        {
            "Manual Save now from the ring buffer",
            "During <b>Monitoring</b>, SeleneWatch continuously holds your <b>Pre-event history</b> seconds in a "
            "memory ring buffer. If your eyes catch a faint flash or grazing event that stayed under threshold "
            "<b>K</b>, press <b>Save now</b> to write those buffered frames and the post-event tail.",
            kSectionLunar
        },
        {
            "Morning review with the nightly report",
            "Every Monitoring session writes an offline <code>report.html</code> (reopen via <b>Session &rarr; Open "
            "last nightly report</b>). It groups events by measurement (<b>brief</b>, <b>moving</b>, <b>wide</b>, "
            "<b>other</b>), merges multi-clip crossings, plays animated preview GIFs inline, and opens the raw "
            "<code>event.ser</code> directly in your SER player.",
            kSectionReport
        },
        {
            "Check on SeleneWatch from your phone",
            "Open <b>Session &rarr; Status on your phone&hellip;</b> to start a read-only status page on your local "
            "network and scan the QR code from your phone. Check mode, lock health, recent events, and a live "
            "snapshot without walking back to the telescope. It stays off until you turn it on.",
            kSectionPhone
        },
        {
            "Rehearse without clear skies",
            "You do not need clear skies to rehearse your thresholds: pick <b>Recording file</b> to replay "
            "<code>.ser</code> or uncompressed <code>.avi</code> captures (looping <b>Loop first &hellip; s</b> at "
            "<b>Fast</b> speed), or enable <b>View &rarr; Testing tools</b> to run <b>Synthetic</b> and inject "
            "<b>Bright flash</b>, <b>Dark transit</b>, <b>Moving streak</b>, <b>Blinking mover</b>, or <b>Lightning</b>.",
            kSectionTestSignals
        },
        {
            "Disk reserve safety",
            "Set <b>Disk reserve</b> in <b>Display</b> to keep a safety margin of free space on your "
            "output drive&mdash;especially when recording long SER runs or ticking <b>Also save raw SER bursts</b> in "
            "<b>Time-lapse</b>. If free space drops below the reserve, SeleneWatch stops writing safely instead of "
            "filling your disk.",
            kSectionOutputs
        },

        // ── Phase 3: Transits, continuous recording & guiding ────────────────
        {
            "Catching dark transits across the Moon",
            "Choose <b>Watching: The Moon: shadows on the sunlit side</b> in <b>Detection</b> to catch aircraft, birds, "
            "or the ISS crossing the sunlit Moon. It looks only for darkening, and its settings start insensitive to "
            "seeing (<code>K 12</code>, 20-pixel shadows, one frame enough for the fast ISS), so silhouettes trigger "
            "clips while bright terrain stays quiet.",
            kSectionTransits
        },
        {
            "Continuous lossless SER recording",
            "Need every frame captured continuously right now? Set the limit beside <b>Record</b> in the top bar to "
            "<b>Unlimited</b> or an exact frame count and press <b>Record</b> in either <b>Preview</b> or "
            "<b>Monitoring</b> to stream lossless SER video into <code>recordings/&lt;UTC&gt;/</code>.",
            kSectionRecording
        },
        {
            "Scheduled UTC recordings",
            "For predicted ISS transits or occultations, open <b>Session &rarr; Schedule a recording&hellip;</b> to "
            "arm a UTC start time and duration during <b>Monitoring</b>. SeleneWatch records the full continuous SER "
            "file automatically while automatic flash and transit detection keeps running.",
            kSectionRecording
        },
        {
            "Surface-feature & planetary guiding",
            "Hold a lunar crater or planetary disk steady without a guide scope: open <b>Guiding</b> in "
            "<b>Preview</b>, click <b>Draw lock box</b> and drag around a sharp feature, connect your <b>ASCOM "
            "telescope</b>, run <b>Calibrate</b>, and press <b>Start guiding</b>. Keeping <b>Tolerance</b> around "
            "<code>3.0%</code> avoids chasing atmospheric seeing.",
            kSectionGuides
        },
        {
            "Live RA & Dec drift speed readout",
            "Once your feature lock is calibrated against your mount, the <b>Guiding</b> panel breaks live drift "
            "speed into <b>RA</b> and <b>Dec</b> (in <code>&Prime;/s</code> when optics are set in Site and "
            "instrument)&mdash;handy for checking tracking quality and refining polar alignment directly on the Moon.",
            kSectionGuides
        },

        // ── Phase 4: Watching the sky ────────────────────────────────────────
        {
            "Fast sky events vs. slow & faint sky",
            "Use <b>The sky: fast events</b> (exposures up to <code>40 ms</code>) for meteors, lightning, aircraft, "
            "and satellites. Switch to <b>The sky: slow and faint</b> (exposures up to <code>2 s</code>) for aurora "
            "or airglow&mdash;it can also build an overnight sky time-lapse right beside its triggered clips.",
            kSectionSky
        },
        {
            "Masking roofs, trees & streetlights",
            "With a sky watch selected, turn on <b>Draw mask</b> in <b>Preview</b> to drag rectangles or click "
            "polygon corners (double-click to close) over roofs, trees, or streetlights. Masked areas are skipped by "
            "detection and noise statistics while <code>event.ser</code> stays whole.",
            kSectionSky
        },
        {
            "Ending an unattended night",
            "<b>Ends</b>, above the preview, stops <b>Monitoring</b> or a time-lapse by itself: at a local time, "
            "after a duration, at dawn, or when the Moon sinks below an altitude, whichever comes first "
            "(<b>Change&hellip;</b>). Dawn and the Moon use your position from <b>Session &rarr; Site and "
            "instrument&hellip;</b>. With a mount connected, choose whether it then keeps or stops tracking.",
            kSectionEnds
        },
        {
            "Automatic night-sky exposure",
            "Under a sky watch, ticking <b>Follow the sky's brightness</b> lets SeleneWatch step exposure within "
            "your safe <code>ms</code> bounds (and gain up to your cap) whenever the unmasked sky median leaves "
            "<code>8&ndash;30%</code> of full scale&mdash;keeping wide-field runs well exposed all night.",
            kSectionSky
        },
        {
            "Letting drifting stars fade quietly",
            "On a fixed unguided camera, bright stars slowly drift from pixel to pixel. <b>Learn a patch after</b> "
            "(<code>20 s</code> default under <b>The sky</b>) releases persistent patches back into background "
            "learning so stars fade quietly while meteors, aircraft, and satellites continue firing.",
            kSectionSky
        },
        {
            "Catching lightning & fireball flares",
            "Under a sky watch, <b>Large brightening</b> triggers a clip whenever a set percentage "
            "(<code>2.0%</code> default) of unmasked pixels rises together in a single frame&mdash;catching distant "
            "lightning strokes and bright fireball flares that exceed single-patch size limits.",
            kSectionSky
        },

        // ── Phase 5: Time-lapses & testing safety reminder ───────────────────
        {
            "Lunar and planetary time-lapses",
            "Watch planets turn and lunar shadows unfold. Open <b>Time-lapse</b> to capture regular bursts, stack "
            "the sharpest frames, and export any mix of <b>MP4</b>, <b>GIF</b>, and lossless <b>SER</b> alongside "
            "32-bit FITS stacks. End automatically via <b>Ends</b> above the preview or after a set stack count.",
            kSectionTimelapse
        },
        {
            "A lunar anchor for every Moon time-lapse",
            "Always select one for a Moon time-lapse. Without it, each stack's shift and rotation are fitted over the "
            "whole frame, where moving crater shadows or a passing cloud can pull the alignment off course and rotate a "
            "stack wrongly. Use <b>Select lunar anchor</b> in <b>Time-lapse</b> to box a steady sunlit region; "
            "<b>Crop around locked feature</b> can be added, but does not replace it.",
            kSectionTimelapse
        },
        {
            "Watch for transits between time-lapse bursts",
            "During a Moon time-lapse, tick <b>Watch for transits between bursts</b> in <b>Time-lapse</b> so "
            "SeleneWatch keeps scanning for aircraft, satellites, or the ISS during the idle gaps between bursts, "
            "briefly delaying the next burst if a transit clip is still writing.",
            kSectionTimelapseTransits
        },
        {
            "Non-destructive re-sharpening & cloud levelling",
            "After any time-lapse, open <b>Session &rarr; Re-sharpen time-lapse&hellip;</b> to compare synchronized "
            "<b>Before</b> and <b>After</b> views while adjusting sharpening, tone mapping, and <b>Brightness "
            "levelling</b> (including <b>Global + patchy</b> for thin passing clouds) without altering your raw "
            "<code>stacks/</code> FITS files.",
            kSectionTimelapseImprove
        },
        {
            "Drop cloudy stacks & inspect every cut",
            "In <b>Re-sharpen time-lapse&hellip;</b>, use the quality filter (<b>Local rolling median</b> drops "
            "sudden cloudy dips without penalizing slow altitude drift) and step through rejected frames with "
            "<b>Dropped</b>&mdash;or open the stack quality list to override individual stacks before rebuilding.",
            kSectionTimelapseImprove
        },
        {
            "Lossless SER export for SER Player",
            "Tick <b>SER</b> under <b>Output</b> in <b>Time-lapse</b> or <b>Re-sharpen</b> to export "
            "<code>timelapse.ser</code> with real per-frame UTC timestamps. By default it writes linear 16-bit mono "
            "(or 24-bit RGB) for further processing in SER Player, or tick <b>Stretch SER (match MP4)</b> for a "
            "ready-to-view stretched video.",
            kSectionTimelapseImprove
        },
        {
            "Remember to turn Testing tools off",
            "Enabling <b>View &rarr; Testing tools</b> lights up an amber <b>TESTING TOOLS ON</b> banner in the top "
            "bar and marks injected test events inside saved logs so rehearsals are never mistaken for real "
            "detections. Switch <b>View &rarr; Testing tools</b> off before starting a real observing patrol.",
            kSectionTestSignals
        },
    };
    return tips;
}

int wrapTipIndex(int index, int count)
{
    if (count <= 0) return 0;
    const int rem = index % count;
    return rem < 0 ? rem + count : rem;
}
