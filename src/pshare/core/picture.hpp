// PShare - Pictures: decoding a screenshot, shrinking it, encoding a JPEG.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared by the app (thumbnails) and pshared (a Telegram-sized photo of a
// screenshot too big to send as one). stb_image reads JPEG and PNG.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pshare
{

struct Picture
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// Decodes a JPEG, PNG or JPEG XR (an HDR screenshot, tone-mapped to SDR) and shrinks it to fit
// max_width x max_height (keeping its shape). False when the file cannot be read or decoded.
bool decode_picture(const std::string &path, int max_width, int max_height, Picture *picture);
// The same from bytes in memory.
bool decode_picture_bytes(const std::string &bytes, int max_width, int max_height,
                          Picture *picture);
// Box-filter shrink (also used by the tests).
Picture shrink(const Picture &source, int max_width, int max_height);
// A JPEG of `picture` (quality 0..100); empty on failure.
std::string encode_jpeg(const Picture &picture, int quality);

} // namespace pshare
