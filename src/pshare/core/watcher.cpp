// PShare - Noticing new captures.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/watcher.hpp"

#include "pshare/core/text.hpp"

#include <algorithm>

namespace pshare
{
namespace
{
// A capture that turns up after the first look is new unless its file is
// dated well before watching began: some carry a time hours off the
// console's clock (local time written as UTC), so a day of slack.
constexpr std::int64_t kClockSlack = 24 * 3600;
} // namespace

void Watcher::configure(const Config &config)
{
    options_.screenshots = config.upload_screenshots;
    options_.videos = config.upload_videos;
    upload_existing_ = config.upload_existing;
    settle_seconds_ = std::clamp(config.settle_seconds, 1, 120);
}

std::string Watcher::serialize() const
{
    std::vector<const std::string *> paths;
    paths.reserve(seen_.size());
    for (const std::string &path : seen_)
        paths.push_back(&path);
    std::sort(paths.begin(), paths.end(),
              [](const std::string *a, const std::string *b) { return *a < *b; });
    std::string out = "#since " + std::to_string(since_) + "\n";
    for (const std::string *path : paths)
    {
        out += *path;
        out.push_back('\n');
    }
    return out;
}

void Watcher::parse(const std::string &text, std::int64_t now)
{
    seen_.clear();
    pending_.clear();
    since_ = 0;
    for (const std::string &raw : split(text, '\n'))
    {
        const std::string_view line = trim(raw);
        if (line.empty())
            continue;
        if (starts_with(line, "#since "))
        {
            since_ = parse_int(line.substr(7), 0);
            continue;
        }
        if (line[0] == '#')
            continue;
        seen_.emplace(line);
    }
    if (since_ <= 0)
    {
        since_ = now;
        dirty_ = true;
    }
}

void Watcher::load(std::int64_t now)
{
    std::string text;
    if (read_text_file(paths_.daemon_seen(), &text, 64u << 20))
    {
        parse(text, now);
        baselined_ = true; // the list is the back catalogue already
        return;
    }
    reset(now);
}

bool Watcher::save()
{
    if (!dirty_)
        return true;
    ensure_directory(paths_.data_root);
    if (!write_text_file_atomic(paths_.daemon_seen(), serialize()))
        return false;
    dirty_ = false;
    return true;
}

void Watcher::reset(std::int64_t now)
{
    seen_.clear();
    pending_.clear();
    since_ = now;
    baselined_ = false;
    dirty_ = true;
}

void Watcher::mark_seen(const std::string &path)
{
    pending_.erase(path);
    if (seen_.insert(path).second)
        dirty_ = true;
}

std::vector<MediaItem> Watcher::poll(std::int64_t now)
{
    std::vector<MediaItem> ready;
    if (!options_.screenshots && !options_.videos)
        return ready;
    ScanResult scan = scan_media(paths_, options_);
    const bool looked = scan.photo_root || scan.video_root;
    last_errno_ = looked ? 0 : scan.open_errno;
    problems_.unreadable = scan.unreadable;
    problems_.unreadable_path = scan.unreadable_path;
    problems_.unreadable_errno = scan.unreadable_errno;

    std::set<std::string> present;
    for (MediaItem &item : scan.items)
    {
        present.insert(item.path);
        if (seen_.count(item.path) != 0)
            continue;
        // The back catalogue: there at the first look, or dated well before it.
        if (!upload_existing_ && ((!baselined_ && looked) || item.mtime < since_ - kClockSlack))
        {
            seen_.insert(item.path);
            dirty_ = true;
            continue;
        }
        auto found = pending_.find(item.path);
        if (found == pending_.end())
        {
            Pending entry;
            entry.size = item.size;
            entry.mtime = item.mtime;
            entry.stable_since = now;
            pending_.emplace(item.path, entry);
            continue;
        }
        Pending &entry = found->second;
        if (entry.size != item.size || entry.mtime != item.mtime || item.size == 0)
        {
            entry.size = item.size;
            entry.mtime = item.mtime;
            entry.stable_since = now;
            entry.handed_out = false;
            continue;
        }
        if (!entry.handed_out && now - entry.stable_since >= settle_seconds_)
        {
            entry.handed_out = true;
            ready.push_back(std::move(item));
        }
    }
    if (looked)
        baselined_ = true;
    // Deleted before it settled (or before it was sent): drop it.
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (present.count(it->first) == 0)
            it = pending_.erase(it);
        else
            ++it;
    }
    std::sort(ready.begin(), ready.end(), [](const MediaItem &a, const MediaItem &b)
              { return a.captured.key() < b.captured.key(); });
    return ready;
}

} // namespace pshare
