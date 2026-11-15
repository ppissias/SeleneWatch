#include "tools/guide_probe.h"

#include "guiding/ascom_mount.h"
#include "guiding/simulated_mount.h"
#include "session.h"
#include "sources/synthetic_frame_source.h"
#include "sources/synthetic_scene.h"
#include "tools/lock_probe.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QThread>

#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <thread>

namespace {

// Lets queued notifications run while waiting for a condition.
bool waitFor(const std::function<bool()>& done, double seconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > until) return false;
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    return true;
}

QString vector(double x, double y)
{
    return QString("%1,%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3);
}

} // namespace

int runGuideSimulation(const QStringList& args)
{
    if (args.size() != 3 && args.size() != 4)
        throw std::invalid_argument("usage: SeleneWatchProbe guide-sim OUTPUT_DIRECTORY [SECONDS]");
    const QString output = args[2];
    const double seconds = args.size() == 4 ? args[3].toDouble() : 120.0;
    if (QDir(output).exists() && !QDir(output).isEmpty()) throw std::runtime_error("the output directory must be new or empty");
    QTextStream out(stdout);

    QObject context;
    auto pointing = std::make_shared<SimulatedPointing>(SimulatedPointing::Settings{});
    Session session(&context, output, {});
    auto source = std::make_unique<SyntheticFrameSource>(640, 480, 30);
    source->setSceneOffset([pointing] {
        const auto offset = pointing->imageOffset(SimulatedPointing::Clock::now());
        return std::pair{offset.x, offset.y};
    });
    session.setSource(std::move(source));
    QString error;
    if (!session.start(error, Session::Mode::Monitoring)) throw std::runtime_error(error.toStdString());
    Guider& guider = session.guider();

    Frame painted;
    paintSyntheticFrame(painted, 640, 480, 0, false);
    const LockRegion region = mostLockable(painted);
    guider.requestLock(region);
    guider.requestConnect([pointing] { return std::make_unique<SimulatedMount>(pointing); });
    guider.setTolerance(0.03 * 480);
    if (!waitFor([&] {
            const GuiderStatus s = guider.status();
            return s.lock.state == LockState::Locked && s.lock.estimateValid && s.guide.mountConnected;
        }, 10))
        throw std::runtime_error("no lock or no mount: " + guider.status().lock.message + guider.status().guide.mountError);
    out << "region=" << region.x << "," << region.y << "," << region.width << "," << region.height << "\n";

    const auto calibrationStart = std::chrono::steady_clock::now();
    guider.requestCalibration();
    waitFor([&] { return guider.status().guide.mode == GuideMode::Calibrating; }, 5);
    waitFor([&] { return guider.status().guide.mode != GuideMode::Calibrating; }, 300);
    GuiderStatus status = guider.status();
    if (!status.guide.calibration) throw std::runtime_error("calibration: " + status.guide.message);
    const GuideCalibration& c = *status.guide.calibration;
    const auto west = pointing->imageRate(GuideDirection::West);
    const auto north = pointing->imageRate(GuideDirection::North);
    out << "calibration_s=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - calibrationStart).count()
        << "\nwest_px_per_s=" << vector(c.west.x * 1000, c.west.y * 1000) << " simulated=" << vector(west.x * 1000, west.y * 1000)
        << "\nnorth_px_per_s=" << vector(c.north.x * 1000, c.north.y * 1000) << " simulated=" << vector(north.x * 1000, north.y * 1000)
        << "\naxis_angle_deg=" << c.axisAngleDegrees << " backlash_ms=" << c.backlashMs
        << " arcsec_per_px=" << (c.arcsecPerPixel ? *c.arcsecPerPixel : 0.0)
        << " speed_px_per_frame=" << status.guide.guideSpeedPixelsPerFrame << "\n";

    guider.requestGuiding(true);
    if (!waitFor([&] { return guider.status().guide.mode == GuideMode::Guiding; }, 5))
        throw std::runtime_error("guiding did not start: " + guider.status().guide.message);
    const auto guidingStart = std::chrono::steady_clock::now();
    const auto skyAtStart = pointing->skyOffset(SimulatedPointing::Clock::now());
    double worstRa = 0, worstDec = 0;
    int samples = 0, locked = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - guidingStart).count() < seconds)
    {
        waitFor([] { return false; }, 1.0);
        status = guider.status();
        if (status.guide.mode != GuideMode::Guiding) break;
        // The simulated sky offset, from where guiding started, in pixels at 1 arcsec per pixel.
        const auto sky = pointing->skyOffset(SimulatedPointing::Clock::now());
        worstRa = std::max(worstRa, std::abs(sky.x - skyAtStart.x));
        worstDec = std::max(worstDec, std::abs(sky.y - skyAtStart.y));
        ++samples;
        if (status.lock.state == LockState::Locked) ++locked;
    }
    status = guider.status();
    const auto sky = pointing->skyOffset(SimulatedPointing::Clock::now());
    out << "guided_s=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - guidingStart).count()
        << " mode=" << guideModeName(status.guide.mode) << " pulses=" << status.guide.pulses
        << " locked_samples=" << locked << "/" << samples << "\n"
        << "worst_offset_from_start_px ra=" << worstRa << " dec=" << worstDec << " tolerance=" << status.guide.tolerancePixels << "\n"
        << "unguided_drift_would_be_px ra=" << pointing->settings().driftRa * seconds << " dec=" << pointing->settings().driftDec * seconds << "\n"
        << "final_sky_offset=" << vector(sky.x, sky.y) << " message=\"" << QString::fromStdString(status.guide.message) << "\"\n";
    session.stop();
    QCoreApplication::processEvents();
    return status.guide.mode == GuideMode::Guiding || status.guide.message.empty() ? 0 : 1;
}

int runAscomProbe(const QStringList& args)
{
    if (args.size() != 4) throw std::invalid_argument("usage: SeleneWatchProbe ascom PROGID OUTPUT.txt");
    QFile file(args[3]);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text))
        throw std::runtime_error("cannot create a new output file: " + args[3].toStdString());
    QString report;
    std::string failure;
    // On a thread of its own, joining the COM apartment there, as the guide thread does.
    std::thread worker([&] {
        try
        {
            QTextStream out(&report);
            auto describe = [&](const MountState& s) {
                out << "connected=" << s.connected << " can_pulse_guide=" << s.canPulseGuide << " tracking=" << s.tracking
                    << " slewing=" << s.slewing << " parked=" << s.parked << " pulse_guiding=" << s.pulseGuiding
                    << " pier=" << pierSideName(s.pierSide) << " alt_azimuth=" << s.altAzimuth
                    << " guide_rate_ra_deg_s=" << (s.guideRateRa ? *s.guideRateRa : -1.0)
                    << " guide_rate_dec_deg_s=" << (s.guideRateDec ? *s.guideRateDec : -1.0) << "\n";
            };
            const auto created = std::chrono::steady_clock::now();
            AscomMount mount(args[2].toStdString());
            out << "driver=" << args[2] << " created_ms="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - created).count() << "\n";
            mount.connect();
            const auto read = std::chrono::steady_clock::now();
            const MountState before = mount.state();
            out << "state_read_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read).count() << "\n";
            describe(before);
            out << "safety_rule_3=\"" << QString::fromStdString(mountRefusal(before)) << "\" (empty means pulses allowed)\n";
            if (before.canPulseGuide)
            {
                for (const GuideDirection direction : {GuideDirection::North, GuideDirection::South, GuideDirection::East, GuideDirection::West})
                {
                    const auto start = std::chrono::steady_clock::now();
                    mount.pulseGuide(direction, 300);
                    const double returned = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    while (mount.state().pulseGuiding && std::chrono::steady_clock::now() - start < std::chrono::seconds{5})
                        std::this_thread::sleep_for(std::chrono::milliseconds{10});
                    const double ended = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    out << "pulse " << guideDirectionName(direction) << " 300 ms: call returned after " << QString::number(returned, 'f', 0)
                        << " ms, IsPulseGuiding cleared after " << QString::number(ended, 'f', 0) << " ms\n";
                }
            }
            describe(mount.state());
            mount.disconnect();
            out << "disconnected\n";
        }
        catch (const std::exception& error)
        {
            failure = error.what();
        }
    });
    worker.join();
    if (!failure.empty()) report += "error=" + QString::fromStdString(failure) + "\n";
    QTextStream(&file) << report;
    QTextStream(stdout) << report;
    return failure.empty() ? 0 : 1;
}
