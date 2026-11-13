#pragma once

#include <filesystem>
#include <string>

// What a source says about its stream. It holds from start to stop and is
// copied into every clip (Evidence rule 8). A field the source cannot fill
// is left empty and written as "unknown"; it is never guessed.
//
// The text is UTF-8.
struct StreamInfo
{
    // What produced the frames: "synthetic", or the recording's path.
    // A camera will give its model and serial number.
    std::string source;

    // The camera, as the source names it. For a SER file, its Instrument field.
    std::string camera;

    // What a frame's time is and where it comes from (Section 5 of the
    // project description).
    std::string frameTime;

    // What a frame's index is: a position in a file, a counter, and so on.
    std::string frameIndex;

    // How a recording is played: the whole file, or a loop and over which
    // frames. Empty for a live source.
    std::string playback;
};

// A path as UTF-8 text, for StreamInfo and for messages.
[[nodiscard]] inline std::string utf8Path(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}
