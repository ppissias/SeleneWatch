#include "preview_image.h"

QImage frameToPreviewImage(const Frame& frame)
{
    if (frame.width <= 0 || frame.height <= 0 || frame.data.empty())
    {
        return {};
    }

    switch (frame.format)
    {
    case PixelFormat::Mono8:
        return QImage(frame.data.data(), frame.width, frame.height, frame.width,
            QImage::Format_Grayscale8).copy();

    case PixelFormat::RGB888:
        return QImage(frame.data.data(), frame.width, frame.height, frame.width * 3,
            QImage::Format_RGB888).copy();
    }

    return {};
}
