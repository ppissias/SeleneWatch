#include "startup_tips.h"

namespace {
constexpr auto kSectionGuides = "3-feature-lock--mount-guiding-shared-for-lunar--planetary-workflows";
constexpr auto kSectionSite = "23-site-instrument--clock-accuracy";
constexpr auto kSectionSource = "24-selecting-a-camera-or-recording--starting-preview";
constexpr auto kSectionDisplay = "25-display-stretch--the-native-adu-histogram";
constexpr auto kSectionOutput = "22-choosing-your-output-folder";
constexpr auto kSectionLunar = "4-workflow-1--lunar-flashes-on-the-earth-illuminated-side";
constexpr auto kSectionTransits = "5-workflow-2--objects-passing-in-front-of-the-moon-shadows--transits";
constexpr auto kSectionTimelapse = "6-workflow-3--lunar-and-planetary-time-lapses";
constexpr auto kSectionSky = "7-workflow-4--watching-the-sky-with-a-fixed-camera";
constexpr auto kSectionOutputs = "8-what-selenewatch-saves-folders-clips--logs";
}

const std::vector<StartupTip>& allStartupTips()
{
    static const std::vector<StartupTip> tips = {
        {
            "During <b>Preview</b>, the <b>Sensitivity K</b>, <b>Minimum patch</b>, <b>Maximum patch</b>, and "
            "<b>Maximum box / patch</b> sliders show live markers right on their tracks: a teal tick for the current "
            "frame and an amber triangle holding recent peaks. Watch them beside the 10-second <b>Peak sigma</b> graph "
            "to place thresholds comfortably above seeing before starting <b>Monitoring</b>.",
            kSectionLunar
        },
        {
            "You can set one root directory under <b>Session &rarr; Output folder&hellip;</b>. SeleneWatch keeps all "
            "lunar and sky events, continuous recordings, and time-lapses neatly organized beneath it&mdash;and warns "
            "against cloud-synced folders or missing drives before an overnight run starts.",
            kSectionOutput
        },
        {
            "Switching to <b>Auto-stretch</b>, moving <b>Black / white levels</b>, ticking <b>Invert brightness</b>, "
            "or toggling <b>Show Bayer colour</b> in <b>Display and capture</b> only changes what appears on screen. "
            "Saved <code>.ser</code> videos and the detector always read unmodified linear camera pixels.",
            kSectionDisplay
        },
        {
            "For unattended nights under <b>The sky</b>, tick <b>Watch from dusk to dawn</b> and set a <b>Sun below</b> "
            "altitude (for example <code>&minus;10.0&deg;</code>). Using your site coordinates from <b>Session &rarr; "
            "Site and instrument&hellip;</b>, SeleneWatch starts <b>Monitoring</b> after dusk and stops at dawn.",
            kSectionSky
        },
        {
            "Under <b>The sky</b>, ticking <b>Follow the sky's brightness</b> lets SeleneWatch step exposure within "
            "your safe <code>ms</code> bounds (and gain up to your cap) whenever the unmasked sky median leaves "
            "<code>8&ndash;30 %</code> of full scale&mdash;keeping wide-field runs well exposed all night.",
            kSectionSky
        },
        {
            "You do not need clear skies to rehearse your thresholds: pick <b>Recording file</b> to replay "
            "<code>.ser</code> or uncompressed <code>.avi</code> captures (looping <b>Loop first &hellip; s</b> at "
            "<b>Fast</b> speed), or enable <b>View &rarr; Testing tools</b> to use <b>Synthetic</b> and inject "
            "calibrated test flashes in <b>Preview</b>.",
            kSectionSource
        },
        {
            "Enabling <b>View &rarr; Testing tools</b> lights up an amber <b>TESTING TOOLS ON</b> banner in the top "
            "bar and marks injected flashes inside saved logs so test runs are never mistaken for real events. Switch "
            "<b>View &rarr; Testing tools</b> off before starting a real observing patrol.",
            kSectionLunar
        },
        {
            "Choosing <b>RAW16</b> in your <b>Camera</b> dock preserves every ADC bit delivered by your sensor instead "
            "of scaling to 8 bits. Use <b>View &rarr; Histogram</b> in <b>Preview</b> to verify that bright crater rims "
            "or planetary bands stay unclipped.",
            kSectionSource
        },
        {
            "Open <b>Session &rarr; Site and instrument&hellip;</b> once to save your location and optics: clip "
            "overviews will then compute angular speeds in <code>&deg;/s</code>, and ticking <b>Compare the PC clock "
            "with an NTP server at each Start</b> logs any PC clock offset in <code>event.txt</code>.",
            kSectionSite
        },
        {
            "The <b>Watching</b> selector at the top of <b>Detection</b> stores independent settings for <b>The Moon</b> "
            "and <b>The sky</b>. Tuning wide streak limits and long tails for overnight sky patrols will never disturb "
            "your lunar earthshine configuration.",
            kSectionLunar
        },
        {
            "With <b>The sky</b> selected, turn on <b>Draw mask</b> in <b>Preview</b> to drag rectangles or click "
            "polygon corners (double-click to close) over roofs, trees, or streetlights. Masked areas are skipped by "
            "detection and noise statistics while <code>event.ser</code> stays whole.",
            kSectionSky
        },
        {
            "On a fixed unguided camera, bright stars slowly drift from pixel to pixel. <b>Learn a patch after</b> "
            "(<code>20 s</code> default under <b>The sky</b>) releases persistent patches back into background learning "
            "so stars fade quietly while meteors, aircraft, and satellites continue firing.",
            kSectionSky
        },
        {
            "Under <b>The sky</b>, <b>Large brightening</b> triggers a clip whenever a set percentage (<code>2.0 %</code> "
            "default) of unmasked pixels rises together in a single frame&mdash;catching distant lightning strokes and "
            "bright fireball flares that exceed single-patch size limits.",
            kSectionSky
        },
        {
            "Tick <b>Also trigger on darkening</b> in <b>Detection</b> to catch aircraft, birds, or the ISS crossing "
            "the sunlit Moon. On bright lunar terrain, raise <b>Sensitivity K</b> to around <code>12.0 sigma</code> or "
            "higher so seeing stays quiet while silhouettes trigger blue-tracked <code>dark transit</code> clips.",
            kSectionTransits
        },
        {
            "Need every frame captured continuously right now? Set the limit beside <b>Record</b> in the top bar to "
            "<b>Unlimited</b> or an exact frame count and press <b>Record</b> in either <b>Preview</b> or "
            "<b>Monitoring</b> to stream lossless SER video into <code>recordings/&lt;UTC&gt;/</code>.",
            kSectionTransits
        },
        {
            "For predicted ISS transits or occultations, open <b>Session &rarr; Schedule a recording&hellip;</b> to arm "
            "a UTC start time and duration during <b>Monitoring</b>. SeleneWatch records the full continuous SER file "
            "automatically while automatic flash and transit detection keeps running.",
            kSectionTransits
        },
        {
            "During <b>Monitoring</b>, SeleneWatch continuously holds your <b>Pre-event history</b> seconds in a memory "
            "ring buffer. If your eyes catch a faint flash or grazing event that stayed under threshold <b>K</b>, press "
            "<b>Save now</b> to write those buffered frames and the post-event tail.",
            kSectionLunar
        },
        {
            "To hold lunar terrain steady during long watches, open <b>View &rarr; Guiding</b> in <b>Preview</b>, drag "
            "a box around a crisp crater, connect your <b>ASCOM telescope</b>, run <b>Calibrate</b>, and press "
            "<b>Start guiding</b>. Keeping <b>Tolerance</b> around <code>3.0 %</code> avoids chasing atmospheric seeing.",
            kSectionGuides
        },
        {
            "Open <b>View &rarr; Time-lapse</b> for periodic lucky-imaging bursts into 32-bit FITS stacks and MP4/GIF "
            "movies. Under <b>Stop condition</b>, you can finish automatically at a <b>UTC end time</b>, after a "
            "<b>Fixed duration</b>, or after a chosen <b>Number of stacks</b>.",
            kSectionTimelapse
        },
        {
            "After any time-lapse, open <b>Session &rarr; Re-sharpen time-lapse&hellip;</b> to drag live sharpening and "
            "tone-mapping sliders (<b>Before</b> vs <b>After</b>), save profiles, or run <b>Rebuild video only</b> "
            "without altering your unsharpened <code>stacks/</code> FITS files.",
            kSectionTimelapse
        },
        {
            "Every saved lunar or sky event writes a diagnostic <code>00_overview.png</code> summary and a compact "
            "looping <code>04_event_preview.gif</code> (&le; 10 frames, &le; 512 px, &le; 512 KiB) beside the raw "
            "<code>event.ser</code>, making morning triage effortless.",
            kSectionOutputs
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
