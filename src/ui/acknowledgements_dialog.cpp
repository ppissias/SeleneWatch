#include "ui/acknowledgements_dialog.h"

#include "ui/widgets/theme.h"

#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtGui/QFontDatabase>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>

namespace {

QTextBrowser* makeHtmlBrowser(QWidget* parent, const QString& objectName)
{
    auto* browser = new QTextBrowser(parent);
    browser->setObjectName(objectName);
    browser->setOpenExternalLinks(true);
    browser->setFrameShape(QFrame::NoFrame);
    browser->document()->setDocumentMargin(14);
    return browser;
}

QColor mutedColorFor(const QPalette& pal)
{
    const QColor fg = pal.color(QPalette::WindowText);
    const QColor bg = pal.color(QPalette::Base);
    return QColor(
        (fg.red() * 68 + bg.red() * 32) / 100,
        (fg.green() * 68 + bg.green() * 32) / 100,
        (fg.blue() * 68 + bg.blue() * 32) / 100);
}

QColor ruleColorFor(const QPalette& pal)
{
    const QColor fg = pal.color(QPalette::WindowText);
    const QColor bg = pal.color(QPalette::Base);
    return QColor(
        (fg.red() * 25 + bg.red() * 75) / 100,
        (fg.green() * 25 + bg.green() * 75) / 100,
        (fg.blue() * 25 + bg.blue() * 75) / 100);
}

} // namespace

AcknowledgementsDialog::AcknowledgementsDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName("acknowledgementsDialog");
    setWindowTitle("Credits & Acknowledgements");
    setModal(false);
    resize(820, 640);

    auto* layout = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName("acknowledgementsTabs");

    softwareBrowser_ = makeHtmlBrowser(tabs_, "acknowledgementsSoftwareBrowser");
    referencesBrowser_ = makeHtmlBrowser(tabs_, "acknowledgementsReferencesBrowser");
    formatsBrowser_ = makeHtmlBrowser(tabs_, "acknowledgementsFormatsBrowser");

    tabs_->addTab(softwareBrowser_, "Software && Utilities");
    tabs_->addTab(referencesBrowser_, "Scientific References");
    tabs_->addTab(formatsBrowser_, "Formats && Standards");
    layout->addWidget(tabs_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    populateTabs();
}

void AcknowledgementsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange))
        populateTabs();
}

void AcknowledgementsDialog::populateTabs()
{
    if (!softwareBrowser_ || !referencesBrowser_ || !formatsBrowser_) return;
    softwareBrowser_->setHtml(wrapHtml(softwareUtilitiesHtml()));
    referencesBrowser_->setHtml(wrapHtml(scientificReferencesHtml()));
    formatsBrowser_->setHtml(wrapHtml(formatsStandardsHtml()));
}

QString AcknowledgementsDialog::wrapHtml(const QString& body) const
{
    const QPalette pal = palette();
    const QString textHex = pal.color(QPalette::WindowText).name();
    const QString linkHex = pal.color(QPalette::Link).name();
    const QString mutedHex = mutedColorFor(pal).name();
    const QString ruleHex = ruleColorFor(pal).name();
    return QString(
        "<html><head><style>"
        "body { font-family: sans-serif; font-size: 10pt; color: %1; }"
        "h3 { margin-top: 10px; margin-bottom: 6px; font-size: 11pt; }"
        "h4 { margin-top: 10px; margin-bottom: 4px; font-size: 10pt; }"
        "p { margin-top: 4px; margin-bottom: 8px; line-height: 1.4; }"
        "ul { margin-top: 4px; margin-bottom: 8px; }"
        "li { margin-bottom: 6px; line-height: 1.35; }"
        "a { color: %2; text-decoration: underline; }"
        ".muted { color: %3; font-size: 9pt; }"
        "hr { border: 0; border-top: 1px solid %4; margin: 12px 0 8px 0; }"
        "</style></head><body>%5</body></html>")
        .arg(textHex, linkHex, mutedHex, ruleHex, body);
}

QString AcknowledgementsDialog::logoCard(const QString& resourcePath, int width, int height,
    const QString& contentHtml) const
{
    // A white badge keeps dark-ink logos readable in dark theme; soften it in Red night vision.
    const bool nightRed = currentUiTheme() == UiTheme::NightRed;
    const QString badgeBg = nightRed ? QStringLiteral("#4a1515") : QStringLiteral("#ffffff");
    const QString badgeBorder = nightRed ? QStringLiteral("#7a2525") : QStringLiteral("#777777");
    return QString(
        "<table cellpadding='0' cellspacing='0' border='0' width='100%' style='margin-bottom: 8px;'>"
        "<tr>"
        "<td width='84' valign='middle'>"
        "  <table cellpadding='4' cellspacing='0' border='0' bgcolor='%1' "
        "         style='border: 1px solid %2;' width='80'>"
        "    <tr><td align='center' valign='middle' height='40'>"
        "      <img src='%3' width='%4' height='%5'>"
        "    </td></tr>"
        "  </table>"
        "</td>"
        "<td width='12'>&nbsp;</td>"
        "<td valign='middle'>%6</td>"
        "</tr>"
        "</table>")
        .arg(badgeBg, badgeBorder, resourcePath)
        .arg(width).arg(height)
        .arg(contentHtml);
}

QString AcknowledgementsDialog::softwareUtilitiesHtml() const
{
    return QString(
        "<h3>Open Source Software &amp; Libraries</h3>"
        "<p>SeleneWatch is built with the following open-source libraries and components. "
        "Full license texts ship in the <i>licenses</i> folder beside SeleneWatch.exe, and "
        "<code>THIRD_PARTY_LICENSES.md</code> gives each project's source and obligations.</p>"
        "<ul>"
        "  <li><b>Qt 6</b> by The Qt Company and contributors (<a href='https://www.qt.io'>qt.io</a>) &mdash; "
        "      Desktop interface, images, networking and threads, dynamically linked under the GNU LGPL v3.</li>"
        "  <li><b>OpenCV</b> by the OpenCV team (<a href='https://opencv.org'>opencv.org</a>) &mdash; "
        "      Edge-aware debayering, phase-correlation burst alignment, rigid Euclidean/ECC lunar alignment, "
        "      Lanczos resampling, bilateral pre-denoise and linear unsharp masking, under the Apache License 2.0.</li>"
        "  <li><b>CFITSIO</b> by NASA/GSFC HEASARC (<a href='https://heasarc.gsfc.nasa.gov/fitsio/'>heasarc.gsfc.nasa.gov/fitsio</a>) &mdash; "
        "      Reading and writing linear 32-bit floating-point FITS stacks with astronomy metadata headers, "
        "      under the NASA permissive open-source license.</li>"
        "  <li><b>FFmpeg</b> by the FFmpeg developers (<a href='https://ffmpeg.org'>ffmpeg.org</a>) &mdash; "
        "      External LGPLv3 shared build (<code>win64-lgpl-shared</code>, with <code>libopenh264</code>) for "
        "      encoding MP4, palette-optimized GIF time-lapses and compact event clip previews.</li>"
        "  <li><b>QR Code generator library</b> by Project Nayuki "
        "      (<a href='https://www.nayuki.io/page/qr-code-generator-library'>nayuki.io</a>) &mdash; "
        "      Generates the QR code for <b>Session &rarr; Status on your phone</b>, under the MIT License.</li>"
        "</ul>"
        "<h3>Camera &amp; Mount Software</h3>"
        "<p>Camera SDKs for <b>ZWO</b> (bundled under its MIT-worded license), <b>QHYCCD</b>, "
        "<b>Player One</b> and <b>ToupTek</b> cameras (observer-installed runtimes), and the "
        "<a href='https://ascom-standards.org/'><b>ASCOM Platform</b></a> for mount guiding.</p>"
        "<h3>Development &amp; Packaging</h3>"
        "<p><a href='https://cmake.org/'>CMake</a> and <a href='https://ninja-build.org/'>Ninja</a> build the "
        "application; <b>Qt Test</b> and <b>CTest</b> run the automated suites; CPack and "
        "<a href='https://wixtoolset.org/'>WiX Toolset 3</a> create the Windows <code>.msi</code> installer "
        "and <code>.zip</code> archive.</p>"
        "<hr>"
        "<h3>AI Coding Assistants</h3>"
        "<p>The development of SeleneWatch has been supported by these AI coding assistants:</p>")
        + logoCard(":/docs/images/logos/claude.png", 36, 36,
            "<b>Claude</b> by Anthropic (<a href='https://www.anthropic.com/claude'>Claude</a>).")
        + logoCard(":/docs/images/logos/openai.png", 36, 36,
            "<b>Codex</b> by OpenAI (<a href='https://openai.com/codex/'>Codex</a>).")
        + logoCard(":/docs/images/logos/gemini.png", 36, 36,
            "<b>Gemini</b> by Google (<a href='https://gemini.google.com/'>Gemini</a>).")
        + logoCard(":/docs/images/logos/grok.png", 68, 19,
            "<b>Grok</b> by xAI (<a href='https://x.ai/grok'>Grok</a>).")
        + "<p class='muted'>Logos are trademarks of their respective owners.</p>";
}

QString AcknowledgementsDialog::scientificReferencesHtml() const
{
    return QString(
        "<h3>Scientific References &amp; Algorithms</h3>"
        "<p>SeleneWatch's design is original work. Where it uses an established method or published reference "
        "values, each entry below identifies its contribution; related literature is omitted unless it supplies "
        "an additional method used here. File formats and hardware interfaces are listed on the "
        "<i>Formats &amp; Standards</i> tab.</p>"
        "<h4>Detecting changes in the image</h4>"
        "<ul>"
        "  <li><b>Wren, Azarbayejani, Darrell &amp; Pentland (1997)</b> &mdash; "
        "      <i>Pfinder: Real-Time Tracking of the Human Body</i>, "
        "      <a href='https://doi.org/10.1109/34.598236'>IEEE TPAMI 19(7), 780&ndash;785</a>.<br>"
        "      <span class='muted'>Per-pixel running Gaussian background model in <code>FlashDetector</code> "
        "      and <code>clip_overview</code>, forgetting with a time constant in seconds and holding firing pixels out of the model.</span></li>"
        "  <li><b>Hampel (1974)</b> &mdash; <i>The Influence Curve and Its Role in Robust Estimation</i>, "
        "      <a href='https://doi.org/10.1080/01621459.1974.10482962'>JASA 69(346), 383&ndash;393</a>.<br>"
        "      <span class='muted'>Robust MAD &times; 1.4826 scale estimator for the sampled noise floor in "
        "      <code>FlashDetector</code>, structured-scene noise in <code>FeatureLock</code>, and residual scale "
        "      in time-lapse brightness matching (<code>stack_processor</code>).</span></li>"
        "  <li><b>Welford (1962)</b> &mdash; <i>Note on a Method for Calculating Corrected Sums of Squares and Products</i>, "
        "      <a href='https://doi.org/10.1080/00401706.1962.10490022'>Technometrics 4(3), 419&ndash;420</a>.<br>"
        "      <span class='muted'>One-pass running mean and variance used in <code>FrameTimingMeter</code> "
        "      and <code>auto_tune</code>.</span></li>"
        "</ul>"
        "<h4>Keeping the field (feature lock &amp; guiding)</h4>"
        "<ul>"
        "  <li><b>Lewis (1995)</b> &mdash; <i>Fast Normalized Cross-Correlation</i>, Vision Interface 1995 "
        "      (<a href='https://www.scribblethink.org/Work/nvisionInterface/nip.html'>author's expanded version</a>).<br>"
        "      <span class='muted'>Zero-mean normalised cross-correlation computed directly in <code>FeatureLock</code> "
        "      (without the paper's integral-image fast method), with a parabolic sub-pixel peak fit.</span></li>"
        "  <li><b>Immerk&aelig;r (1996)</b> &mdash; <i>Fast Noise Variance Estimation</i>, "
        "      <a href='https://doi.org/10.1006/cviu.1996.0060'>CVIU 64(2), 300&ndash;302</a>.<br>"
        "      <span class='muted'>The 3&times;3 second-difference operator (factor 6), paired with Hampel's MAD &times; 1.4826 "
        "      scale instead of the paper's mean, in <code>FeatureLock::pixelNoise</code> to measure pixel noise on structured lunar scenes.</span></li>"
        "</ul>"
        "<h4>Time-lapse alignment, stacking &amp; sharpening</h4>"
        "<ul>"
        "  <li><b>Kuglin &amp; Hines (1975)</b> &mdash; <i>The Phase Correlation Image Alignment Method</i>, "
        "      Proc. IEEE Int. Conf. on Cybernetics and Society, 163&ndash;165.<br>"
        "      <span class='muted'>Sub-pixel translation estimation for burst stacking and coarse lunar alignment via "
        "      OpenCV <code>cv::phaseCorrelate</code> with a Hann window on high-pass-filtered luminance (<code>stack_processor</code>).</span></li>"
        "  <li><b>Evangelidis &amp; Psarakis (2008)</b> &mdash; <i>Parametric Image Alignment Using Enhanced Correlation Coefficient Maximization</i>, "
        "      <a href='https://doi.org/10.1109/TPAMI.2008.113'>IEEE TPAMI 30(10), 1858&ndash;1865</a>.<br>"
        "      <span class='muted'>Rigid Euclidean (translation + field rotation) alignment across Moon time-lapse stacks "
        "      on a coarse-to-fine Gaussian pyramid, anchored on a chosen sunlit region via OpenCV <code>cv::findTransformECC</code>.</span></li>"
        "  <li><b>Duchon (1979)</b> &mdash; <i>Lanczos Filtering in One and Two Dimensions</i>, "
        "      <a href='https://doi.org/10.1175/1520-0450(1979)018&lt;1016:LFIOAT&gt;2.0.CO;2'>J. Appl. Meteorol. 18(8), 1016&ndash;1022</a>.<br>"
        "      <span class='muted'>Lanczos-4 sub-pixel resampling (<code>cv::INTER_LANCZOS4</code>) when warping burst frames and aligned session stacks.</span></li>"
        "  <li><b>Tomasi &amp; Manduchi (1998)</b> &mdash; <i>Bilateral Filtering for Gray and Color Images</i>, "
        "      <a href='https://doi.org/10.1109/ICCV.1998.710815'>Proc. 6th IEEE ICCV, 839&ndash;846</a>.<br>"
        "      <span class='muted'>Optional edge-preserving pre-denoise via OpenCV <code>cv::bilateralFilter</code> before linear unsharp masking.</span></li>"
        "  <li><b>Huber (1964)</b> &mdash; <i>Robust Estimation of a Location Parameter</i>, "
        "      <a href='https://doi.org/10.1214/aoms/1177703732'>Ann. Math. Statist. 35(1), 73&ndash;101</a>.<br>"
        "      <span class='muted'>Huber-reweighted IRLS in <code>stack_processor</code> for affine brightness levelling between aligned stacks without cloud-edge bias.</span></li>"
        "</ul>"
        "<h4>Sun and Moon positions</h4>"
        "<ul>"
        "  <li><b>Meeus (1998)</b> &mdash; <a href='https://shopatsky.com/products/astronomical-algorithms-2nd-edition'><i>Astronomical Algorithms</i>, 2nd ed.</a>, Willmann-Bell.<br>"
        "      <span class='muted'>Through NOAA's Solar Calculator (<a href='https://gml.noaa.gov/grad/solcalc/calcdetails.html'>calculation details</a>) "
        "      for <code>sunAltitudeDegrees</code> (dusk-to-dawn scheduling), and the Greenwich Mean Sidereal Time expression in <code>moonAltitudeDegrees</code>.</span></li>"
        "  <li><b>The Astronomical Almanac</b> (USNO / HMNAO), <i>Low-Precision Formulae for Geocentric Coordinates of the Moon</i> &mdash; "
        "      adapted from <b>Van Flandern &amp; Pulkkinen (1979)</b>, <i>Low-Precision Formulae for Planetary Positions</i>, "
        "      <a href='https://doi.org/10.1086/190623'>ApJS 41, 391&ndash;411</a>.<br>"
        "      <span class='muted'>Geocentric lunar longitude, latitude and horizontal parallax series with topocentric parallax correction "
        "      in <code>moonAltitudeDegrees</code>, for ending a session when the Moon sets below a chosen altitude.</span></li>"
        "  <li><b>Reda &amp; Andreas (2004)</b> &mdash; <i>Solar position algorithm for solar radiation applications</i>, "
        "      <a href='https://doi.org/10.1016/j.solener.2003.12.003'>Solar Energy 76(5), 577&ndash;589</a>.<br>"
        "      <span class='muted'>Worked example used as a reference value in the automated tests of <code>sunAltitudeDegrees</code>; the algorithm itself is not implemented.</span></li>"
        "  <li><b>NASA JPL Horizons</b> (<a href='https://ssd.jpl.nasa.gov/horizons/'>ssd.jpl.nasa.gov/horizons</a>).<br>"
        "      <span class='muted'>Published topocentric lunar altitude tables serve as reference values in the automated tests of <code>moonAltitudeDegrees</code>; "
        "      SeleneWatch does not query Horizons at run time.</span></li>"
        "</ul>");
}

QString AcknowledgementsDialog::formatsStandardsHtml() const
{
    return QString(
        "<h3>Formats, Protocols &amp; Interfaces</h3>"
        "<p>File formats, network time protocol, and observatory interfaces supported directly by SeleneWatch:</p>"
        "<ul>"
        "  <li><b>FITS Standard (v3.0)</b> &mdash; Pence, Chiappetti, Page, Shaw &amp; Stobie (2010), "
        "      <i>Definition of the Flexible Image Transport System (FITS), version 3.0</i>, "
        "      <a href='https://doi.org/10.1051/0004-6361/201015362'>A&amp;A 524, A42</a>.<br>"
        "      <span class='muted'>Linear 32-bit floating-point time-lapse stacks and metadata headers read and written in <code>fits_io</code> via CFITSIO.</span></li>"
        "  <li><b>SER Format (v3)</b> &mdash; Heiko Wilkens &amp; Grischa Hahn "
        "      (<a href='https://www.grischa-hahn.homepage.t-online.de/astro/ser/SER%20Doc%20V3b.pdf'>SER format specification</a>).<br>"
        "      <span class='muted'>Lossless astronomical video format with 100-ns UTC frame timestamps read and written by <code>SerReader</code> and <code>SerWriter</code>.</span></li>"
        "  <li><b>NTPv4</b> &mdash; Mills, Martin, Burbank &amp; Kasch (2010), "
        "      <i>Network Time Protocol Version 4: Protocol and Algorithms Specification</i>, "
        "      <a href='https://datatracker.ietf.org/doc/html/rfc5905'>RFC 5905</a>.<br>"
        "      <span class='muted'>Client UDP packet exchange and offset/round-trip calculation in <code>clock_check</code> to record PC clock offset at session start.</span></li>"
        "  <li><b>ASCOM Telescope Interface</b> &mdash; <a href='https://ascom-standards.org/'>ASCOM Initiative</a>.<br>"
        "      <span class='muted'>Late-bound COM calls in <code>ascom_mount</code> to read mount state and send guide pulses.</span></li>"
        "  <li><b>FireCapture</b> by Torsten Edelmann (<a href='https://www.firecapture.de/'>firecapture.de</a>).<br>"
        "      <span class='muted'>Companion FireCapture <code>.txt</code> capture logs parsed by <code>firecapture_log</code> during SER/AVI replay to recover camera settings and timestamps.</span></li>"
        "</ul>");
}

AboutDialog::AboutDialog(QWidget* parent, const QString& release, const QString& revision,
    std::function<void()> openAcknowledgements, std::function<void()> openAboutQt)
    : QDialog(parent)
{
    setObjectName("aboutDialog");
    setWindowTitle("About SeleneWatch");
    setModal(false);
    resize(620, 520);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 14);
    root->setSpacing(10);

    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(16);

    auto* logoLabel = new QLabel(this);
    const QPixmap logo(":/icons/selenewatch-256.png");
    if (!logo.isNull())
        logoLabel->setPixmap(logo.scaled(88, 88, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logoLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    headerRow->addWidget(logoLabel);

    auto* metaCol = new QVBoxLayout;
    metaCol->setSpacing(4);
    auto* title = new QLabel(QString("<b>SeleneWatch %1</b>").arg(release), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.4);
    title->setFont(titleFont);
    metaCol->addWidget(title);

    metaCol->addWidget(new QLabel(QString("Source revision %1  &middot;  Qt %2 (LGPLv3)").arg(revision, qVersion()), this));

    auto* summary = new QLabel(
        "Watches the Moon for impact flashes and shadows, watches the sky for meteors, aircraft, "
        "satellites and lightning, makes lunar and planetary time-lapses, and keeps the evidence of each candidate.",
        this);
    summary->setWordWrap(true);
    metaCol->addWidget(summary);

    auto* repo = new QLabel(
        "Copyright &copy; 2026, Petros Pissias. Open source under the BSD 2-Clause License, shown below.<br>"
        "<a href=\"https://github.com/ppissias/SeleneWatch\">github.com/ppissias/SeleneWatch</a>",
        this);
    repo->setOpenExternalLinks(true);
    metaCol->addWidget(repo);
    headerRow->addLayout(metaCol, 1);
    root->addLayout(headerRow);

    auto* licenseEdit = new QPlainTextEdit(licenseText(), this);
    licenseEdit->setObjectName("aboutLicenseText");
    licenseEdit->setReadOnly(true);
    licenseEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    root->addWidget(licenseEdit, 1);

    auto* notice = new QLabel(
        "The license texts of Qt and of the other bundled libraries are in the <i>licenses</i> folder beside "
        "SeleneWatch.exe, and <code>THIRD_PARTY_LICENSES.md</code> says where their source code is.",
        this);
    notice->setWordWrap(true);
    root->addWidget(notice);

    auto* bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(8);
    bottomRow->addStretch(1);

    auto* ackButton = new QPushButton("Acknowledgements", this);
    ackButton->setObjectName("aboutAcknowledgementsButton");
    ackButton->setToolTip("Credits for libraries, tools, scientific methods and standards used by SeleneWatch.");
    connect(ackButton, &QPushButton::clicked, this, [openAcknowledgements = std::move(openAcknowledgements)] {
        if (openAcknowledgements) openAcknowledgements();
    });
    bottomRow->addWidget(ackButton);

    auto* qtButton = new QPushButton("About Qt", this);
    qtButton->setObjectName("aboutQtButton");
    connect(qtButton, &QPushButton::clicked, this, [openAboutQt = std::move(openAboutQt)] {
        if (openAboutQt) openAboutQt();
    });
    bottomRow->addWidget(qtButton);

    auto* closeButton = new QPushButton("Close", this);
    closeButton->setDefault(true);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    bottomRow->addWidget(closeButton);

    root->addLayout(bottomRow);
}

QString AboutDialog::licenseText()
{
    QFile file(":/LICENSE");
    if (file.open(QIODevice::ReadOnly))
        return QString::fromUtf8(file.readAll()).replace("\r\n", "\n").trimmed();
    return QStringLiteral(
        "BSD 2-Clause License\n\n"
        "The full text is in LICENSE.txt beside SeleneWatch.exe and at "
        "https://github.com/ppissias/SeleneWatch.");
}
