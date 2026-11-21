#include "startup_tips.h"

namespace {
constexpr auto kSectionMainScreen = "1-the-main-screen-at-a-glance";
constexpr auto kSectionDrivers = "21-computer-installation--camera-drivers";
constexpr auto kSectionOutput = "22-choosing-your-output-folder";
constexpr auto kSectionSite = "23-site-instrument--clock-accuracy";
constexpr auto kSectionSource = "24-selecting-a-camera-or-recording--starting-preview";
constexpr auto kSectionDisplay = "25-display-stretch--the-native-adu-histogram";
constexpr auto kSectionEnds = "27-ending-a-session-unattended";
constexpr auto kSectionGuides = "3-feature-lock--mount-guiding-shared-for-lunar--planetary-workflows";
constexpr auto kSectionLunar = "4-workflow-1--lunar-flashes-on-the-earth-illuminated-side";
constexpr auto kSectionTransits = "5-workflow-2--objects-passing-in-front-of-the-moon-shadows--transits";
constexpr auto kSectionTimelapse = "6-workflow-3--lunar-and-planetary-time-lapses";
constexpr auto kSectionSky = "7-workflow-4--watching-the-sky-with-a-fixed-camera";
constexpr auto kSectionOutputs = "8-what-selenewatch-saves-folders-clips--logs";
}

const std::vector<StartupTip>& allStartupTips()
{
    static const std::vector<StartupTip> tips = {
        // ── Phase 1: First-session essentials ────────────────────────────────
        {
            "Preview vs. Monitoring",
            "Use <b>Preview</b> (blue banner) to focus, tune camera controls, and set thresholds safely without "
            "writing event clips. When ready, switch to <b>Monitoring</b> (green banner)&mdash;camera settings lock "
            "for evidence integrity while automatic capture runs. Press <code>F1</code> anytime to open the built-in "
            "<b>User Manual</b>.",
            kSectionMainScreen
        },
        {
            "One organized output folder",
            "Set one root directory under <b>Session &rarr; Output folder&hellip;</b>. SeleneWatch keeps all lunar "
            "and sky events, continuous recordings, and time-lapses neatly organized beneath it&mdash;and warns "
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
            "Display controls never alter saved pixels",
            "Switching to <b>Auto-stretch</b>, moving <b>Black / white levels</b>, ticking <b>Invert brightness</b>, "
            "or toggling <b>Show Bayer colour</b> in <b>Display and capture</b> only changes what appears on screen. "
            "Saved <code>.ser</code> videos and the detector always read unmodified linear camera pixels.",
            kSectionDisplay
        },
        {
            "Zoom, pan & wheel protection",
            "Use the <b>View: Fit / 100% / 200%</b> bar above the preview&mdash;or roll the <b>mouse wheel</b> over "
            "the live image&mdash;to zoom smoothly around the cursor pixel, and <b>right-drag</b> or "
            "<b>middle-drag</b> to pan while zoomed. Spin boxes and sliders ignore stray wheel scrolls so your "
            "settings never drift accidentally while scrolling a panel.",
            kSectionDisplay
        },
        {
            "Earthshine exposure & histogram",
            "For lunar impact flashes, open <b>View &rarr; Histogram</b> in <b>Preview</b> and expose so the "
            "earth-illuminated side sits around <code>3&ndash;10%</code> of full scale with <b>At 0</b> at "
            "<code>0.00%</code> (raise camera <b>Offset</b> if needed). Let the sunlit crescent saturate&mdash;keep "
            "most of it outside the frame&mdash;and use <b>Auto-stretch</b> to view comfortably.",
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
            "Independent Moon & Sky configurations",
            "The <b>Watching</b> selector at the top of <b>Detection</b> stores independent settings for "
            "<b>The Moon</b> and <b>The sky</b>. Tuning wide streak limits and long tails for overnight sky patrols "
            "will never disturb your lunar earthshine configuration.",
            kSectionLunar
        },
        {
            "Named detection profiles",
            "Save named threshold presets with <b>Save&hellip;</b> on the <b>Profile</b> row at the top of "
            "<b>Detection</b>. Under <b>The sky</b>, you can optionally tick <b>Include horizon mask</b> to bundle a "
            "site's roofline mask with the profile, or leave it unticked to switch sensitivity presets without "
            "altering your current mask.",
            kSectionLunar
        },

        // ── Phase 2: Camera depth, timing, review & rehearsal ────────────────
        {
            "Keep full sensor depth with RAW16",
            "Choosing <b>RAW16</b> in your <b>Camera</b> dock preserves every ADC bit delivered by your sensor "
            "instead of scaling to 8 bits. Use <b>View &rarr; Histogram</b> in <b>Preview</b> to verify that sunlit "
            "lunar terrain or planetary bands stay unclipped for transits and time-lapses.",
            kSectionSource
        },
        {
            "Site, optics & NTP clock check",
            "Open <b>Session &rarr; Site and instrument&hellip;</b> once to save your location and optics: clip "
            "overviews will then compute angular speeds in <code>&deg;/s</code>, and ticking <b>Compare the PC clock "
            "with an NTP server at each Start</b> logs any PC clock offset in <code>event.txt</code>.",
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
            "Quick morning event triage",
            "Every saved lunar or sky event writes a diagnostic <code>00_overview.png</code> summary and a compact "
            "looping <code>04_event_preview.gif</code> (&le; 10 frames, &le; 512 px, &le; 512 KiB) beside the raw "
            "<code>event.ser</code>, making morning triage effortless.",
            kSectionOutputs
        },
        {
            "Rehearse without clear skies",
            "You do not need clear skies to rehearse your thresholds: pick <b>Recording file</b> to replay "
            "<code>.ser</code> or uncompressed <code>.avi</code> captures (looping <b>Loop first &hellip; s</b> at "
            "<b>Fast</b> speed), or enable <b>View &rarr; Testing tools</b> to run <b>Synthetic</b> (for "
            "<b>The Moon</b> or <b>The sky</b>) and inject <b>Bright flash</b>, <b>Dark transit</b>, "
            "<b>Moving streak</b>, <b>Blinking mover</b>, or <b>Lightning</b> test events.",
            kSectionSource
        },
        {
            "Disk reserve safety",
            "Set <b>Disk reserve</b> in <b>Display and capture</b> to keep a safety margin of free space on your "
            "output drive&mdash;especially when recording long SER runs or ticking <b>Also save raw SER bursts</b> in "
            "<b>Time-lapse</b>. If free space drops below the reserve, SeleneWatch stops writing safely instead of "
            "filling your disk.",
            kSectionDisplay
        },

        // ── Phase 3: Transits, continuous recording & guiding ────────────────
        {
            "Catching dark transits across the Moon",
            "Choose <b>Watching: The Moon: shadows on the sunlit side</b> in <b>Detection</b> to catch aircraft, birds, "
            "or the ISS crossing the sunlit Moon. Its settings are its own and start insensitive to seeing "
            "(<code>K 12</code>, 20-pixel shadows over two frames), so silhouettes trigger <code>dark transit</code> clips "
            "while bright terrain stays quiet.",
            kSectionTransits
        },
        {
            "Continuous lossless SER recording",
            "Need every frame captured continuously right now? Set the limit beside <b>Record</b> in the top bar to "
            "<b>Unlimited</b> or an exact frame count and press <b>Record</b> in either <b>Preview</b> or "
            "<b>Monitoring</b> to stream lossless SER video into <code>recordings/&lt;UTC&gt;/</code>.",
            kSectionTransits
        },
        {
            "Scheduled UTC recordings",
            "For predicted ISS transits or occultations, open <b>Session &rarr; Schedule a recording&hellip;</b> to "
            "arm a UTC start time and duration during <b>Monitoring</b>. SeleneWatch records the full continuous SER "
            "file automatically while automatic flash and transit detection keeps running.",
            kSectionTransits
        },
        {
            "Crater-locked mount guiding",
            "To hold lunar terrain steady during long watches, open <b>View &rarr; Guiding</b> in <b>Preview</b>, "
            "drag a box around a crisp crater, connect your <b>ASCOM telescope</b>, run <b>Calibrate</b>, and press "
            "<b>Start guiding</b>. Keeping <b>Tolerance</b> around <code>3.0%</code> avoids chasing atmospheric seeing.",
            kSectionGuides
        },

        // ── Phase 4: Watching the sky ────────────────────────────────────────
        {
            "Masking roofs, trees & streetlights",
            "With <b>The sky</b> selected, turn on <b>Draw mask</b> in <b>Preview</b> to drag rectangles or click "
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
            "Under <b>The sky</b>, ticking <b>Follow the sky's brightness</b> lets SeleneWatch step exposure within "
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
            "Under <b>The sky</b>, <b>Large brightening</b> triggers a clip whenever a set percentage "
            "(<code>2.0%</code> default) of unmasked pixels rises together in a single frame&mdash;catching distant "
            "lightning strokes and bright fireball flares that exceed single-patch size limits.",
            kSectionSky
        },

        // ── Phase 5: Time-lapses & testing safety reminder ───────────────────
        {
            "Lunar and planetary time-lapses",
            "Watch planets turn and lunar shadows unfold. Open <b>View &rarr; Time-lapse</b> to capture "
            "regular bursts, stack the sharpest frames, and build MP4/GIF animations alongside 32-bit FITS stacks. "
            "It ends with <b>Ends</b>, above the preview (a local time, a duration, dawn, the Moon's "
            "altitude), or, under <b>Stops</b>, after a chosen number of stacks.",
            kSectionTimelapse
        },
        {
            "Lunar anchor for long time-lapses",
            "During multi-hour lunar terminator time-lapses, moving crater shadows can pull whole-frame alignment off "
            "course. Use <b>Select lunar anchor</b> in <b>Time-lapse</b> to lock a steady sunlit feature&mdash;and "
            "optionally tick <b>Crop around locked feature</b> for a rock-steady movie.",
            kSectionTimelapse
        },
        {
            "Non-destructive re-sharpening",
            "After any time-lapse, open <b>Session &rarr; Re-sharpen time-lapse&hellip;</b> to drag live sharpening "
            "and tone-mapping sliders (<b>Before</b> vs <b>After</b>), save profiles, or run "
            "<b>Rebuild video only</b> without altering your unsharpened <code>stacks/</code> FITS files.",
            kSectionTimelapse
        },
        {
            "Remember to turn Testing tools off",
            "Enabling <b>View &rarr; Testing tools</b> lights up an amber <b>TESTING TOOLS ON</b> banner in the top "
            "bar and marks injected test events inside saved logs so rehearsals are never mistaken for real "
            "detections. Switch <b>View &rarr; Testing tools</b> off before starting a real observing patrol.",
            kSectionLunar
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
