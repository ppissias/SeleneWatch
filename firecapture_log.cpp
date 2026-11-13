#include "firecapture_log.h"

#include "stream_info.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>

namespace {

using namespace std::chrono;

// Lines read at most. FireCapture's logs have a few dozen.
constexpr int kMaxLines = 200;

std::string trimmed(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
    {
        ++start;
    }
    return text.substr(start);
}

bool sameIgnoringCase(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

bool digitsAt(const std::string& text, size_t at, size_t count, int& value)
{
    if (text.size() < at + count)
    {
        return false;
    }
    value = 0;
    for (size_t i = at; i < at + count; ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
        {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    return true;
}

// "HHmmss" or "HHmmss.fff" as a time of day.
std::optional<milliseconds> timeOfDay(const std::string& text)
{
    int h = 0;
    int m = 0;
    int s = 0;
    if (!digitsAt(text, 0, 2, h) || !digitsAt(text, 2, 2, m) || !digitsAt(text, 4, 2, s)
        || h > 23 || m > 59 || s > 59)
    {
        return std::nullopt;
    }

    int ms = 0;
    if (text.size() > 6)
    {
        if (text[6] != '.')
        {
            return std::nullopt;
        }
        // Up to three fraction digits, as milliseconds.
        int scale = 100;
        for (size_t i = 7; i < text.size() && i < 10; ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(text[i])))
            {
                return std::nullopt;
            }
            ms += (text[i] - '0') * scale;
            scale /= 10;
        }
    }
    return hours{h} + minutes{m} + seconds{s} + milliseconds{ms};
}

// "ddMMyy" as a day. FireCapture's years are this century.
std::optional<sys_days> dayFromDdMmYy(const std::string& text)
{
    int d = 0;
    int mo = 0;
    int y = 0;
    if (text.size() != 6 || !digitsAt(text, 0, 2, d) || !digitsAt(text, 2, 2, mo) || !digitsAt(text, 4, 2, y))
    {
        return std::nullopt;
    }
    const year_month_day date{year{2000 + y}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(d)}};
    if (!date.ok())
    {
        return std::nullopt;
    }
    return sys_days{date};
}

std::optional<system_clock::time_point> startUtc(const std::map<std::string, std::string>& values)
{
    const auto value = [&values](const char* key) -> std::string {
        const auto found = values.find(key);
        return found == values.end() ? std::string() : found->second;
    };

    const std::string format = value("Date_format");
    if (!format.empty() && format != "ddMMyy")
    {
        return std::nullopt;
    }

    const std::optional<sys_days> localDay = dayFromDdMmYy(value("Date"));
    const std::optional<milliseconds> localStart = timeOfDay(value("Start"));
    const std::optional<milliseconds> utcStart = timeOfDay(value("Start(UT)"));
    if (!localDay || !localStart || !utcStart)
    {
        return std::nullopt;
    }

    // The zone offset, brought into the range of real time zones.
    milliseconds offset = *localStart - *utcStart;
    while (offset > hours{14})
    {
        offset -= hours{24};
    }
    while (offset <= -hours{12})
    {
        offset += hours{24};
    }

    return system_clock::time_point(*localDay + *localStart - offset);
}

} // namespace

std::optional<FireCaptureLog> readFireCaptureLog(const std::filesystem::path& recording)
{
    std::filesystem::path logPath = recording;
    logPath.replace_extension(".txt");

    std::error_code exists;
    if (!std::filesystem::is_regular_file(logPath, exists))
    {
        return std::nullopt;
    }

    std::ifstream in(logPath);
    if (!in)
    {
        return std::nullopt;
    }

    std::map<std::string, std::string> values;
    std::string line;
    bool first = true;
    for (int count = 0; count < kMaxLines && std::getline(in, line); ++count)
    {
        line = trimmed(line);
        if (first)
        {
            if (line.rfind("FireCapture", 0) != 0)
            {
                return std::nullopt;
            }
            first = false;
            continue;
        }
        const size_t equals = line.find('=');
        if (equals != std::string::npos)
        {
            values.emplace(trimmed(line.substr(0, equals)), trimmed(line.substr(equals + 1)));
        }
    }
    if (first)
    {
        return std::nullopt;
    }

    // A log that names another file is not this recording's.
    const auto named = values.find("Filename");
    if (named != values.end() && !sameIgnoringCase(named->second, utf8Path(recording.filename())))
    {
        return std::nullopt;
    }

    FireCaptureLog log;
    log.fileName = utf8Path(logPath.filename());
    log.camera = values["Camera"];
    log.shutter = values["Shutter"];
    log.gain = values["Gain"];
    log.startUtc = startUtc(values);
    return log;
}
