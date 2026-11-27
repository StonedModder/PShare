// PShare - Noticing new captures.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each poll walks the capture folders and compares them with what it has
// seen. A new file becomes ready once its size and modification time have
// held still for `settle_seconds`: the console writes a clip over several
// seconds, and an upload must not start on half a file.
//
// The seen list is kept on disk so a restart does not send everything again.
// Its first line records when watching began ("#since <unix time>"): captures
// older than that are the back catalogue and are only sent with
// upload_existing; newer ones the daemon missed while it was off are sent.

#pragma once

#include "pshare/core/config.hpp"
#include "pshare/core/media.hpp"

#include <cstdint>
#include <string>
#include <map>
#include <set>
#include <vector>

namespace pshare
{

class Watcher
{
  public:
    explicit Watcher(Paths paths) : paths_(std::move(paths))
    {
    }

    // Which kinds to look at and how long a file must hold still.
    void configure(const Config &config);

    // Reads the seen list (missing is fine: watching starts now).
    void load(std::int64_t now);
    // Writes it back when it changed.
    bool save();

    // One look at the folders. Returns the captures that became ready, oldest
    // first. Ready captures are not marked seen: call mark_seen() once the
    // upload is settled (sent, or given up on), so a crash retries them.
    std::vector<MediaItem> poll(std::int64_t now);

    void mark_seen(const std::string &path);
    bool seen(const std::string &path) const
    {
        return seen_.count(path) != 0;
    }
    // Forget everything: the next poll treats the folders as new.
    void reset(std::int64_t now);

    std::size_t pending() const
    {
        return pending_.size();
    }
    std::size_t seen_count() const
    {
        return seen_.size();
    }
    std::int64_t since() const
    {
        return since_;
    }
    // The last scan could open neither capture folder (EPERM: not elevated).
    int last_errno() const
    {
        return last_errno_;
    }
    // Folders the last scan could not open, and the first of them (scan_media).
    const ScanResult &last_scan_problems() const
    {
        return problems_;
    }

    // For tests: the seen-list text and its parse.
    std::string serialize() const;
    void parse(const std::string &text, std::int64_t now);

  private:
    struct Pending
    {
        std::uint64_t size = 0;
        std::int64_t mtime = 0;
        std::int64_t stable_since = 0;
        bool handed_out = false;
    };

    Paths paths_;
    ScanOptions options_;
    bool upload_existing_ = false;
    int settle_seconds_ = 4;
    std::int64_t since_ = 0;
    std::set<std::string> seen_; // ordered: see history.hpp
    std::map<std::string, Pending> pending_;
    bool dirty_ = false;
    // What is there at the first look is the back catalogue; after that,
    // anything not seen is new, whatever date its file carries.
    bool baselined_ = false;
    int last_errno_ = 0;
    ScanResult problems_; // only the unreadable_* fields are kept
};

} // namespace pshare
