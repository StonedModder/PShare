// PShare - The upload history shared by the app and the daemon.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One tab-separated line per attempt, appended with a single write() so the
// app and the daemon can both add to it:
//
//   unix-time  ok|fail  auto|app  destination-id  capture-path  link-or-error

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <map>
#include <vector>

namespace pshare
{

struct HistoryEntry
{
    std::int64_t time = 0;
    bool ok = false;
    bool automatic = false; // sent by the daemon, not from the app
    std::string destination;
    std::string path;
    std::string detail; // the link on success, the reason on failure
};

class History
{
  public:
    explicit History(std::string path = {}) : path_(std::move(path))
    {
    }
    void set_path(std::string path)
    {
        path_ = std::move(path);
    }
    const std::string &path() const
    {
        return path_;
    }

    // Reads the file again when it grew since the last load; true when it
    // changed. Keeps at most `keep` newest entries in memory.
    bool refresh(std::size_t keep = 2000);
    bool append(const HistoryEntry &entry);

    const std::vector<HistoryEntry> &entries() const
    {
        return entries_; // oldest first
    }
    // The newest successful entry for a capture, or null.
    const HistoryEntry *last_success(std::string_view path) const;
    bool uploaded(std::string_view path) const
    {
        return last_success(path) != nullptr;
    }
    int successes() const;
    int failures() const;

    static std::string format(const HistoryEntry &entry);
    static bool parse(std::string_view line, HistoryEntry *entry);

  private:
    void index();
    std::string path_;
    std::vector<HistoryEntry> entries_;
    // Ordered on purpose: libc++'s hash tables call a helper that can throw,
    // and a native title links without the unwinder.
    std::map<std::string, std::size_t> success_; // path -> entry index
    std::int64_t loaded_size_ = -1;
};

} // namespace pshare
