#include "detection_mask.h"

#include <algorithm>
#include <cmath>
#include <sstream>

DetectionMask::DetectionMask(std::vector<MaskShape> shapes)
    : shapes_(shapes.empty() ? nullptr : std::make_shared<const std::vector<MaskShape>>(std::move(shapes)))
{
}

const std::vector<MaskShape>& DetectionMask::shapes() const
{
    static const std::vector<MaskShape> none;
    return shapes_ ? *shapes_ : none;
}

namespace {

bool validShape(const MaskShape& shape)
{
    if (shape.size() < 3 || shape.size() > static_cast<size_t>(DetectionMask::kMaxCorners)) return false;
    return std::all_of(shape.begin(), shape.end(), [](const MaskPoint& point) { return point.x >= 0 && point.y >= 0; });
}

} // namespace

bool DetectionMask::valid() const
{
    const auto& list = shapes();
    return list.size() <= static_cast<size_t>(kMaxShapes) && std::all_of(list.begin(), list.end(), validShape);
}

DetectionMask DetectionMask::with(MaskShape shape) const
{
    if (!validShape(shape) || count() >= kMaxShapes) return *this;
    std::vector<MaskShape> list = shapes();
    list.push_back(std::move(shape));
    return DetectionMask(std::move(list));
}

MaskShape DetectionMask::rectangle(int x, int y, int width, int height)
{
    return {{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}};
}

void DetectionMask::fill(int width, int height, uint8_t* pixels) const
{
    for (const MaskShape& shape : shapes())
    {
        int top = height, bottom = -1;
        for (const MaskPoint& point : shape) { top = std::min(top, point.y); bottom = std::max(bottom, point.y); }
        top = std::max(top, 0);
        bottom = std::min(bottom, height);
        std::vector<double> crossings;
        // Each row's pixel centres are tested against the edges they cross.
        for (int y = top; y < bottom; ++y)
        {
            const double cy = y + 0.5;
            crossings.clear();
            for (size_t i = 0; i < shape.size(); ++i)
            {
                const MaskPoint& a = shape[i];
                const MaskPoint& b = shape[(i + 1) % shape.size()];
                if ((a.y <= cy) == (b.y <= cy)) continue;
                crossings.push_back(a.x + (cy - a.y) * (b.x - a.x) / static_cast<double>(b.y - a.y));
            }
            std::sort(crossings.begin(), crossings.end());
            for (size_t i = 0; i + 1 < crossings.size(); i += 2)
            {
                // Pixels whose centre x + 0.5 lies in [from, to).
                const int from = std::max(0, static_cast<int>(std::ceil(crossings[i] - 0.5)));
                const int to = std::min(width, static_cast<int>(std::ceil(crossings[i + 1] - 0.5)));
                for (int x = from; x < to; ++x) pixels[static_cast<size_t>(y) * width + x] = 1;
            }
        }
    }
}

std::string DetectionMask::text() const
{
    std::ostringstream out;
    bool firstShape = true;
    for (const MaskShape& shape : shapes())
    {
        if (!firstShape) out << ';';
        firstShape = false;
        for (size_t i = 0; i < shape.size(); ++i) out << (i ? " " : "") << shape[i].x << ',' << shape[i].y;
    }
    return out.str();
}

DetectionMask DetectionMask::fromText(const std::string& text)
{
    DetectionMask mask;
    std::istringstream shapes(text);
    std::string item;
    while (std::getline(shapes, item, ';'))
    {
        std::vector<int> numbers;
        bool spaces = item.find(' ') != std::string::npos;
        std::string token;
        std::istringstream parts(item);
        while (parts >> token)
        {
            std::istringstream pair(token);
            std::string number;
            while (std::getline(pair, number, ','))
            {
                try { numbers.push_back(std::stoi(number)); }
                catch (...) { numbers.clear(); break; }
            }
        }
        MaskShape shape;
        if (!spaces && numbers.size() == 4)
            shape = rectangle(numbers[0], numbers[1], numbers[2], numbers[3]);
        else if (numbers.size() % 2 == 0)
            for (size_t i = 0; i + 1 < numbers.size(); i += 2) shape.push_back({numbers[i], numbers[i + 1]});
        mask = mask.with(std::move(shape));
    }
    return mask;
}
