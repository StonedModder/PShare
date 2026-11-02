// PShare - Lossless, bounded video parts for Telegram.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pshare/core/upload.hpp"

namespace pshare
{
// Owns a private temporary directory; originals are never modified.
struct VideoParts
{
    std::string directory;
    std::vector<MediaItem> items;
    VideoParts() = default;
    VideoParts(const VideoParts &) = delete;
    VideoParts &operator=(const VideoParts &) = delete;
    ~VideoParts();
};
bool create_video_workspace(const std::string &scratch, VideoParts *parts, std::string *error);
bool split_video(const MediaItem &item, std::uint64_t limit, const std::string &scratch,
                 UploadProgress *progress, VideoParts *parts, std::string *error);
} // namespace pshare
