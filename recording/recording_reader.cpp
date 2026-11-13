#include "recording/recording_reader.h"
#include "recording/avi_reader.h"
#include "recording/ser_reader.h"

#include <algorithm>
#include <cctype>

std::unique_ptr<RecordingReader> openRecordingReader(const std::filesystem::path& path,
    double fallbackFps, std::string& error)
{
    std::string extension = utf8Path(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".ser")
    {
        auto reader = std::make_unique<SerReader>();
        return reader->open(path, fallbackFps, error) ? std::move(reader) : nullptr;
    }
    if (extension == ".avi")
    {
        auto reader = std::make_unique<AviReader>();
        return reader->open(path, fallbackFps, error) ? std::move(reader) : nullptr;
    }
    error = utf8Path(path) + " is neither a .ser nor an .avi file";
    return nullptr;
}
