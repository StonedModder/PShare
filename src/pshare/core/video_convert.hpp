// PShare - Optional MP4 preparation.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pshare/core/video_split.hpp"

namespace pshare
{
// H.264/AAC, at most 1920x1080 and 30 fps; owns its output through VideoParts.
bool convert_webm(const MediaItem &item, const std::string &scratch, UploadProgress *progress,
                  VideoParts *output, std::string *error);
} // namespace pshare
