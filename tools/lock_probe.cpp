#include "tools/lock_probe.h"

#include "guiding/lock_tracker.h"
#include "recording/recording_reader.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {

void read(RecordingReader& reader, int index, Frame& frame)
{
    std::string error;
    frame.data.resize(reader.frameBytes());
    if (!reader.readFrame(index, frame, error)) throw std::runtime_error(error);
    frame.frameIndex = reader.frameNumber(index);
    frame.timestamp = reader.frameTime(index);
}

double median(std::vector<double> values)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

LockRegion mostLockable(const Frame& frame)
{
    LockRegion best{};
    double bestScore = -1;
    for (int y = 32; y + 96 <= frame.height; y += 32)
    {
        for (int x = 32; x + 96 <= frame.width; x += 32)
        {
            std::string refusal;
            const auto lock = FeatureLock::take(frame, {x, y, 64, 64}, refusal);
            if (!lock) continue;
            const double score = lock->quality().contrast * lock->quality().structureRatio;
            if (score > bestScore)
            {
                bestScore = score;
                best = {x, y, 64, 64};
            }
        }
    }
    if (bestScore < 0) throw std::runtime_error("no lockable 64 x 64 region in the first frame");
    return best;
}

int runLockProbe(const QStringList& args)
{
    if (args.size() != 4 && args.size() != 8)
        throw std::invalid_argument("usage: SeleneWatchProbe lock INPUT OUTPUT.csv [X Y WIDTH HEIGHT]");
    std::string error;
    auto reader = openRecordingReader(std::filesystem::path(args[2].toStdWString()), 30, error);
    if (!reader) throw std::runtime_error(error);
    QFile csv(args[3]);
    if (!csv.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text))
        throw std::runtime_error("cannot create a new output file: " + args[3].toStdString());

    Frame frame;
    read(*reader, 0, frame);
    const LockRegion region = args.size() == 8
        ? LockRegion{args[4].toInt(), args[5].toInt(), args[6].toInt(), args[7].toInt()}
        : mostLockable(frame);
    LockTracker tracker;
    if (!tracker.lock(frame, region)) throw std::runtime_error(tracker.status().message);

    QTextStream out(&csv), console(stdout);
    const LockStatus& status = tracker.status();
    console << "file=" << args[2] << "\nframes=" << reader->frameCount() << " size=" << frame.width << "x" << frame.height
            << " format=" << pixelFormatName(frame.format) << " bits=" << frame.significantBits << "\n"
            << "region=" << status.region.x << "," << status.region.y << "," << status.region.width << "," << status.region.height
            << " binning=" << status.binning << " contrast=" << status.quality.contrast
            << " structure=" << status.quality.structureRatio << " noise_adu=" << status.quality.noiseAdu << "\n";
    out << "frame,seconds,state,dx,dy,score,valid,estimate_dx,estimate_dy,samples,reason\n";

    // Only the frames the guider would measure are read: one per measuring interval.
    const auto start = reader->frameTime(0);
    auto last = start;
    std::vector<double> scores, residuals, milliseconds;
    int searches = 0, valid = 0, losses = 0;
    LockState previous = LockState::Locked;
    for (int index = 1; index < reader->frameCount(); ++index)
    {
        const auto time = reader->frameTime(index);
        if (std::chrono::duration<double>(time - last).count() < LockTracker::kMeasureIntervalSeconds) continue;
        read(*reader, index, frame);
        QElapsedTimer timer;
        timer.start();
        if (!tracker.step(frame)) continue;
        milliseconds.push_back(timer.nsecsElapsed() / 1e6);
        last = time;
        ++searches;
        const LockMatch& match = *status.lastMatch;
        if (match.valid)
        {
            ++valid;
            scores.push_back(match.score);
            if (status.estimateValid)
                residuals.push_back(std::hypot(match.dx - status.estimateDx, match.dy - status.estimateDy));
        }
        if (previous == LockState::Locked && status.state != LockState::Locked) ++losses;
        previous = status.state;
        out << frame.frameIndex << "," << std::chrono::duration<double>(time - start).count() << ","
            << lockStateName(status.state) << "," << match.dx << "," << match.dy << "," << match.score << ","
            << (match.valid ? 1 : 0) << ",";
        if (status.estimateValid) out << status.estimateDx << "," << status.estimateDy;
        else out << ",";
        out << "," << status.estimateSamples << ",\"" << QString::fromStdString(match.reason) << "\"\n";
        if (status.state == LockState::Lost) break;
    }

    std::vector<double> sortedResiduals = residuals;
    std::sort(sortedResiduals.begin(), sortedResiduals.end());
    console << "searches=" << searches << " valid=" << valid << " losses=" << losses
            << " final_state=\"" << lockStateName(status.state) << "\"\n"
            << "duration_s=" << std::chrono::duration<double>(last - start).count()
            << " final_offset=" << status.foundDx << "," << status.foundDy << "\n"
            << "score_min=" << (scores.empty() ? 0 : *std::min_element(scores.begin(), scores.end()))
            << " score_median=" << median(scores) << "\n"
            << "match_minus_estimate_median_px=" << median(residuals)
            << " p95_px=" << (sortedResiduals.empty() ? 0 : sortedResiduals[sortedResiduals.size() * 95 / 100]) << "\n"
            << "search_ms_median=" << median(milliseconds)
            << " max=" << (milliseconds.empty() ? 0 : *std::max_element(milliseconds.begin(), milliseconds.end())) << "\n";
    return 0;
}
