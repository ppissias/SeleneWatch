#include "preview_image.h"

#include <algorithm>

QImage frameToPreviewImage(const Frame& frame)
{
    if (frame.width <= 0 || frame.height <= 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format))
    {
        return {};
    }

    switch (frame.format)
    {
    case PixelFormat::Mono8:
        return QImage(frame.data.data(), frame.width, frame.height, frame.width,
            QImage::Format_Grayscale8).copy();

    case PixelFormat::Mono16:
    {
        // A fixed shift that shows the top 8 significant bits, so 12-bit data
        // is visible whether it sits at the top or the bottom of the word.
        // The stretch of Feature-5 replaces this.
        QImage image(frame.width, frame.height, QImage::Format_Grayscale8);
        const int shift = shiftToEightBits(frame);
        const uint8_t* samples = frame.data.data();
        for (int y = 0; y < frame.height; ++y)
        {
            uchar* row = image.scanLine(y);
            const uint8_t* source = samples + static_cast<size_t>(y) * static_cast<size_t>(frame.width) * 2;
            for (int x = 0; x < frame.width; ++x)
            {
                const int sample = source[2 * x] | (source[2 * x + 1] << 8);
                row[x] = static_cast<uchar>(std::min(sample >> shift, 255));
            }
        }
        return image;
    }

    case PixelFormat::RGB888:
        return QImage(frame.data.data(), frame.width, frame.height, frame.width * 3,
            QImage::Format_RGB888).copy();
    }

    return {};
}
