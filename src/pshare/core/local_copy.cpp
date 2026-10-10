// PShare - "Sending" without the internet: a copy on a drive.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/local_copy.hpp"

#include "pshare/core/text.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pshare
{
namespace
{
constexpr std::size_t kChunk = 1u << 20;

bool writable_folder(const std::string &path)
{
    struct stat info
    {
    };
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
           access(path.c_str(), W_OK) == 0;
}
} // namespace

std::string safe_name(std::string_view text)
{
    std::string out;
    for (const char c : text)
    {
        if (static_cast<unsigned char>(c) < 32 || std::strchr("/\\:*?\"<>|", c) != nullptr)
            out.push_back('_');
        else
            out.push_back(c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    if (out.empty() || out == "..")
        out = "Captures";
    return out;
}

std::string find_drive(const Paths &paths)
{
    for (const std::string &root : paths.usb_roots)
        if (!root.empty() && writable_folder(root))
            return root;
    return {};
}

std::string folder_target(const Destination &destination, const Paths &paths,
                          const std::string &game, std::string *error)
{
    std::string root = destination.path;
    if (root.empty() || iequals(root, "usb"))
    {
        root = find_drive(paths);
        if (root.empty())
        {
            if (error)
                *error = "No USB drive is connected";
            return {};
        }
    }
    if (root.front() != '/')
    {
        if (error)
            *error = "The folder must be an absolute path";
        return {};
    }
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    return root + "/PShare/" + safe_name(game);
}

UploadOutcome copy_to_folder(const Destination &destination, const Paths &paths,
                             const MediaItem &item, const std::string &game,
                             UploadProgress *progress)
{
    UploadOutcome outcome;
    outcome.destination = destination.id;
    outcome.attempts = 1;
    const std::string folder = folder_target(destination, paths, game, &outcome.error);
    if (folder.empty())
        return outcome;
    if (!ensure_directory(folder))
    {
        outcome.error = "Cannot create " + folder + " (" + std::strerror(errno) + ")";
        return outcome;
    }
    const std::string target = folder + "/" + item.file;
    struct stat existing
    {
    };
    if (stat(target.c_str(), &existing) == 0 &&
        static_cast<std::uint64_t>(existing.st_size) == item.size)
    {
        outcome.ok = true; // already there: a second copy would be the same bytes
        outcome.link = target;
        return outcome;
    }
    const int input = open(item.path.c_str(), O_RDONLY);
    if (input < 0)
    {
        outcome.error = "Cannot read the capture";
        return outcome;
    }
    const std::string partial = target + ".part";
    const int output = open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0)
    {
        outcome.error = "Cannot write to " + folder + " (" + std::strerror(errno) + ")";
        close(input);
        return outcome;
    }
    if (progress != nullptr)
    {
        progress->total = item.size;
        progress->sent = 0;
        progress->stage = 1;
    }
    std::vector<char> buffer(kChunk);
    std::uint64_t copied = 0;
    bool ok = true;
    for (;;)
    {
        if (progress != nullptr && progress->cancel.load())
        {
            outcome.error = "Cancelled";
            ok = false;
            break;
        }
        const ssize_t got = read(input, buffer.data(), buffer.size());
        if (got == 0)
            break;
        if (got < 0)
        {
            if (errno == EINTR)
                continue;
            outcome.error = "Reading the capture failed";
            ok = false;
            break;
        }
        ssize_t done = 0;
        while (done < got)
        {
            const ssize_t put =
                write(output, buffer.data() + done, static_cast<std::size_t>(got - done));
            if (put < 0 && errno == EINTR)
                continue;
            if (put <= 0)
            {
                outcome.error =
                    errno == ENOSPC ? "The drive is full" : "Writing to the drive failed";
                ok = false;
                break;
            }
            done += put;
        }
        if (!ok)
            break;
        copied += static_cast<std::uint64_t>(got);
        if (progress != nullptr)
            progress->sent = copied;
    }
    close(input);
    if (fsync(output) != 0 && ok)
        ok = errno == EINVAL; // some drives' file systems do not sync; the data is written
    if (close(output) != 0 && ok)
    {
        outcome.error = "Writing to the drive failed";
        ok = false;
    }
    if (ok && rename(partial.c_str(), target.c_str()) != 0)
    {
        outcome.error = "Cannot finish the copy (" + std::string(std::strerror(errno)) + ")";
        ok = false;
    }
    if (!ok)
    {
        unlink(partial.c_str());
        return outcome;
    }
    outcome.ok = true;
    outcome.link = target;
    return outcome;
}

} // namespace pshare
