#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A corner of a mask shape, in frame pixels.
struct MaskPoint
{
    int x{0}, y{0};
    bool operator==(const MaskPoint&) const = default;
};

// A mask shape: a polygon of three or more corners. A pixel is masked when
// its centre is inside (even-odd rule), so the rectangle with corners (x, y)
// and (x + w, y + h) masks exactly the w x h pixels from (x, y).
using MaskShape = std::vector<MaskPoint>;

// The areas the detector leaves out (Feature-11, polygons since Feature-15):
// the ground, trees, roofs, street lights. Every measurement carries a copy
// of its settings, so the shapes are shared, not copied; a mask is never
// changed in place, only replaced by one with a shape more or fewer.
class DetectionMask
{
public:
    static constexpr int kMaxShapes = 16;
    static constexpr int kMaxCorners = 32;

    DetectionMask() = default;
    explicit DetectionMask(std::vector<MaskShape> shapes);

    [[nodiscard]] bool empty() const { return shapes().empty(); }
    [[nodiscard]] int count() const { return static_cast<int>(shapes().size()); }
    [[nodiscard]] const std::vector<MaskShape>& shapes() const;
    // Up to kMaxShapes shapes of 3 to kMaxCorners corners, none negative.
    [[nodiscard]] bool valid() const;
    // This mask with one more shape; this one when the shape is not valid
    // or the mask is full.
    [[nodiscard]] DetectionMask with(MaskShape shape) const;

    [[nodiscard]] static MaskShape rectangle(int x, int y, int width, int height);

    // Marks the masked pixels of a width x height frame with 1 in pixels,
    // which must hold width * height values.
    void fill(int width, int height, uint8_t* pixels) const;

    // As saved and recorded: shapes separated by ';', corners "x,y" by
    // spaces. A shape saved as "x,y,w,h" (a rectangle before Feature-15)
    // is read as that rectangle. Shapes that cannot be read are left out.
    [[nodiscard]] std::string text() const;
    [[nodiscard]] static DetectionMask fromText(const std::string& text);

    bool operator==(const DetectionMask& other) const { return shapes() == other.shapes(); }

private:
    std::shared_ptr<const std::vector<MaskShape>> shapes_;
};
