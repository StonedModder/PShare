// PShare - Pictures: decoding a screenshot, shrinking it, encoding a JPEG.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/picture.hpp"

#include "pshare/core/config.hpp"

#include "third_party/jxrlib/pshare_jxr.h"
#define STBI_NO_STDIO
#include "third_party/stb_image/stb_image.h"
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image/stb_image_write.h"

#include <algorithm>
#include <cstdlib>

namespace pshare
{
namespace
{
constexpr std::size_t kMaxFile = 48u << 20;

void append_bytes(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data),
                                                static_cast<std::size_t>(size));
}

// The box-filter shrink, from pixels anywhere (a Picture, or the decoder's own
// buffer, so a 4K screenshot is not copied whole first).
struct Pixels
{
    const std::uint8_t *rgba = nullptr;
    int width = 0;
    int height = 0;
};

Picture shrink_pixels(const Pixels &source, int max_width, int max_height)
{
    if (source.width <= 0 || source.height <= 0)
        return {};
    const float scale = std::min({1.0f, static_cast<float>(max_width) / source.width,
                                  static_cast<float>(max_height) / source.height});
    Picture out;
    out.width = std::max(1, static_cast<int>(source.width * scale + 0.5f));
    out.height = std::max(1, static_cast<int>(source.height * scale + 0.5f));
    if (out.width == source.width && out.height == source.height)
    {
        out.rgba.assign(source.rgba,
                        source.rgba + static_cast<std::size_t>(source.width) * source.height * 4);
        return out;
    }
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    // Each output pixel averages the source pixels it covers: no aliasing
    // when a 3840-wide shot becomes 480 wide.
    for (int y = 0; y < out.height; ++y)
    {
        const int y0 = y * source.height / out.height;
        const int y1 = std::max(y0 + 1, (y + 1) * source.height / out.height);
        for (int x = 0; x < out.width; ++x)
        {
            const int x0 = x * source.width / out.width;
            const int x1 = std::max(x0 + 1, (x + 1) * source.width / out.width);
            std::uint32_t sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
            {
                const std::uint8_t *row =
                    source.rgba + (static_cast<std::size_t>(sy) * source.width + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += row[c];
            }
            const std::uint32_t count = static_cast<std::uint32_t>((y1 - y0) * (x1 - x0));
            std::uint8_t *pixel =
                out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 4; ++c)
                pixel[c] = static_cast<std::uint8_t>(sum[c] / count);
        }
    }
    return out;
}
} // namespace

Picture shrink(const Picture &source, int max_width, int max_height)
{
    return shrink_pixels({source.rgba.data(), source.width, source.height}, max_width, max_height);
}

bool decode_picture_bytes(const std::string &bytes, int max_width, int max_height, Picture *picture)
{
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(0x7fffffff))
        return false;
    int width = 0;
    int height = 0;
    // JPEG XR: the PS5's HDR screenshots, tone-mapped to sRGB by jxrlib's glue.
    const auto *raw = reinterpret_cast<const unsigned char *>(bytes.data());
    if (pshare_jxr_is(raw, bytes.size()))
    {
        unsigned char *rgba = pshare_jxr_decode(raw, bytes.size(), &width, &height);
        if (rgba == nullptr)
            return false;
        *picture = shrink_pixels({rgba, width, height}, max_width, max_height);
        std::free(rgba);
        return picture->width > 0;
    }
    int channels = 0;
    stbi_uc *pixels =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                              static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
        return false;
    *picture = shrink_pixels({pixels, width, height}, max_width, max_height);
    stbi_image_free(pixels);
    return picture->width > 0;
}

bool decode_picture(const std::string &path, int max_width, int max_height, Picture *picture)
{
    std::string bytes;
    if (!read_text_file(path, &bytes, kMaxFile))
        return false;
    return decode_picture_bytes(bytes, max_width, max_height, picture);
}

std::string encode_jpeg(const Picture &picture, int quality)
{
    std::string out;
    if (picture.width <= 0 || picture.height <= 0)
        return out;
    if (stbi_write_jpg_to_func(append_bytes, &out, picture.width, picture.height, 4,
                               picture.rgba.data(), quality) == 0)
        out.clear();
    return out;
}

} // namespace pshare
