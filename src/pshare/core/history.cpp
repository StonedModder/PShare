// PShare - The upload history shared by the app and the daemon.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/history.hpp"

#include "pshare/core/config.hpp"
#include "pshare/core/text.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pshare
{
namespace
{
std::string clean(std::string_view field)
{
    std::string out(field);
    for (char &c : out)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return out;
}
} // namespace

std::string History::format(const HistoryEntry &entry)
{
    return std::to_string(entry.time) + "\t" + (entry.ok ? "ok" : "fail") + "\t" +
           (entry.automatic ? "auto" : "app") + "\t" + clean(entry.destination) + "\t" +
           clean(entry.path) + "\t" + clean(entry.detail) + "\n";
}

bool History::parse(std::string_view line, HistoryEntry *entry)
{
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    const std::vector<std::string> fields = split(line, '\t');
    if (fields.size() < 5)
        return false;
    HistoryEntry out;
    out.time = parse_int(fields[0], -1);
    if (out.time < 0)
        return false;
    out.ok = fields[1] == "ok";
    out.automatic = fields[2] == "auto";
    out.destination = fields[3];
    out.path = fields[4];
    if (fields.size() > 5)
        out.detail = fields[5];
    *entry = std::move(out);
    return true;
}

void History::index()
{
    success_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].ok)
            success_[entries_[i].path] = i;
}

bool History::refresh(std::size_t keep)
{
    struct stat info
    {
    };
    if (path_.empty() || stat(path_.c_str(), &info) != 0)
    {
        const bool changed = !entries_.empty();
        entries_.clear();
        success_.clear();
        loaded_size_ = -1;
        return changed;
    }
    if (static_cast<std::int64_t>(info.st_size) == loaded_size_)
        return false;
    std::string text;
    if (!read_text_file(path_, &text, 64u << 20))
        return false;
    entries_.clear();
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        HistoryEntry entry;
        if (parse(std::string_view(text).substr(start, end - start), &entry))
            entries_.push_back(std::move(entry));
        start = end + 1;
    }
    if (entries_.size() > keep)
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - keep));
    loaded_size_ = static_cast<std::int64_t>(info.st_size);
    index();
    return true;
}

bool History::append(const HistoryEntry &entry)
{
    entries_.push_back(entry);
    if (entry.ok)
        success_[entry.path] = entries_.size() - 1;
    if (path_.empty())
        return false;
    const std::string line = format(entry);
    const int descriptor = open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (descriptor < 0)
        return false;
    const bool ok =
        write(descriptor, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    close(descriptor);
    // Our own line is already in memory; skip re-reading it.
    struct stat info
    {
    };
    if (ok && loaded_size_ >= 0 && stat(path_.c_str(), &info) == 0 &&
        static_cast<std::int64_t>(info.st_size) ==
            loaded_size_ + static_cast<std::int64_t>(line.size()))
        loaded_size_ = static_cast<std::int64_t>(info.st_size);
    return ok;
}

const HistoryEntry *History::last_success(std::string_view path) const
{
    const auto found = success_.find(std::string(path));
    return found == success_.end() ? nullptr : &entries_[found->second];
}

int History::successes() const
{
    int count = 0;
    for (const HistoryEntry &entry : entries_)
        count += entry.ok ? 1 : 0;
    return count;
}

int History::failures() const
{
    return static_cast<int>(entries_.size()) - successes();
}

} // namespace pshare
