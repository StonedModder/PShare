// PShare - Finding the console's screenshots and video clips.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/media.hpp"

#include "pshare/core/text.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pshare
{

std::int64_t CaptureTime::key() const
{
    return ((((static_cast<std::int64_t>(year) * 100 + month) * 100 + day) * 100 + hour) * 100 +
            minute) *
               100 +
           second;
}

std::string CaptureTime::date() const
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
    return buffer;
}

std::string CaptureTime::time() const
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
    return buffer;
}

const char *MediaItem::content_type() const
{
    if (ends_with_ci(file, ".png"))
        return "image/png";
    if (ends_with_ci(file, ".jpg") || ends_with_ci(file, ".jpeg"))
        return "image/jpeg";
    if (ends_with_ci(file, ".mp4"))
        return "video/mp4";
    if (ends_with_ci(file, ".webm"))
        return "video/webm";
    return "application/octet-stream";
}

bool capture_kind(std::string_view file, MediaKind *kind)
{
    MediaKind found;
    if (ends_with_ci(file, ".jpg") || ends_with_ci(file, ".jpeg") || ends_with_ci(file, ".png"))
        found = MediaKind::screenshot;
    else if (ends_with_ci(file, ".webm") || ends_with_ci(file, ".mp4"))
        found = MediaKind::video;
    else
        return false;
    // Partial files the console is still writing, and our own temporaries.
    if (!file.empty() && file.front() == '.')
        return false;
    if (kind)
        *kind = found;
    return true;
}

namespace
{
bool digits(std::string_view text, std::size_t at, std::size_t count)
{
    if (at + count > text.size())
        return false;
    for (std::size_t i = at; i < at + count; ++i)
        if (text[i] < '0' || text[i] > '9')
            return false;
    return true;
}

int number(std::string_view text, std::size_t at, std::size_t count)
{
    int value = 0;
    for (std::size_t i = at; i < at + count; ++i)
        value = value * 10 + (text[i] - '0');
    return value;
}

bool plausible(const CaptureTime &t)
{
    return t.valid() && t.year <= 2100 && t.hour < 24 && t.minute < 60 && t.second < 61;
}

CaptureTime from_epoch(std::int64_t seconds)
{
    CaptureTime out;
    const LocalTime parts = local_time(seconds);
    out.year = parts.year;
    out.month = parts.month;
    out.day = parts.day;
    out.hour = parts.hour;
    out.minute = parts.minute;
    out.second = parts.second;
    return out;
}

void walk(const std::string &folder, MediaKind kind, int depth, const ScanOptions &options,
          ScanResult *result)
{
    if (depth > options.max_depth || result->items.size() >= options.max_items)
        return;
    DIR *directory = opendir(folder.c_str());
    if (directory == nullptr)
        return;
    ++result->folders;
    std::vector<std::string> folders;
    while (const dirent *entry = readdir(directory))
    {
        const char *name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        std::string path = folder + "/" + name;
        // d_type is not filled on every file system; stat answers either way.
        bool is_folder = entry->d_type == DT_DIR;
        bool is_file = entry->d_type == DT_REG;
        struct stat info
        {
        };
        const bool have_info = stat(path.c_str(), &info) == 0;
        if (have_info)
        {
            is_folder = S_ISDIR(info.st_mode);
            is_file = S_ISREG(info.st_mode);
        }
        if (is_folder)
        {
            folders.push_back(std::move(path));
            continue;
        }
        MediaKind found;
        if (!is_file || !have_info || !capture_kind(name, &found))
            continue;
        MediaItem item;
        item.file = name;
        item.kind = found;
        item.size = static_cast<std::uint64_t>(info.st_size);
        item.mtime = static_cast<std::int64_t>(info.st_mtime);
        if (!parse_capture_time(item.file, &item.captured))
            item.captured = from_epoch(item.mtime);
        item.path = std::move(path);
        item.title_id = title_id_from_path(item.path);
        result->items.push_back(std::move(item));
        if (result->items.size() >= options.max_items)
            break;
    }
    closedir(directory);
    for (const std::string &child : folders)
        walk(child, kind, depth + 1, options, result);
}
} // namespace

bool parse_capture_time(std::string_view file, CaptureTime *time)
{
    for (std::size_t i = 0; i + 8 <= file.size(); ++i)
    {
        if (!digits(file, i, 8) || (i > 0 && file[i - 1] >= '0' && file[i - 1] <= '9'))
            continue;
        CaptureTime t;
        t.year = number(file, i, 4);
        t.month = number(file, i + 4, 2);
        t.day = number(file, i + 6, 2);
        std::size_t at = i + 8;
        if (at < file.size() && (file[at] == '_' || file[at] == '-' || file[at] == 'T'))
            ++at;
        if (!digits(file, at, 6))
            continue;
        t.hour = number(file, at, 2);
        t.minute = number(file, at + 2, 2);
        t.second = number(file, at + 4, 2);
        if (!plausible(t))
            continue;
        if (time)
            *time = t;
        return true;
    }
    return false;
}

bool looks_like_title_id(std::string_view text)
{
    if (text.size() != 9)
        return false;
    for (std::size_t i = 0; i < 4; ++i)
        if (text[i] < 'A' || text[i] > 'Z')
            return false;
    return digits(text, 4, 5);
}

std::string title_id_from_path(std::string_view path)
{
    std::string found;
    std::size_t start = 0;
    while (start < path.size())
    {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos)
            end = path.size();
        const std::string_view segment = path.substr(start, end - start);
        // A folder may carry more after the ID ("PPSA01234_00").
        if (segment.size() >= 9 && looks_like_title_id(segment.substr(0, 9)) && end != path.size())
            found.assign(segment.substr(0, 9));
        start = end + 1;
    }
    return found;
}

bool describe_media(const std::string &path, MediaItem *item)
{
    struct stat info
    {
    };
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    const std::size_t slash = path.rfind('/');
    MediaItem out;
    out.path = path;
    out.file = slash == std::string::npos ? path : path.substr(slash + 1);
    if (!capture_kind(out.file, &out.kind))
        return false;
    out.size = static_cast<std::uint64_t>(info.st_size);
    out.mtime = static_cast<std::int64_t>(info.st_mtime);
    if (!parse_capture_time(out.file, &out.captured))
        out.captured = from_epoch(out.mtime);
    out.title_id = title_id_from_path(path);
    *item = std::move(out);
    return true;
}

ScanResult scan_media(const Paths &paths, const ScanOptions &options)
{
    ScanResult result;
    const auto probe = [&](const std::string &root, bool *ok)
    {
        DIR *directory = opendir(root.c_str());
        if (directory == nullptr)
        {
            if (result.open_errno == 0)
                result.open_errno = errno != 0 ? errno : ENOENT;
            return;
        }
        closedir(directory);
        *ok = true;
    };
    if (options.screenshots)
    {
        probe(paths.photo_root, &result.photo_root);
        if (result.photo_root)
            walk(paths.photo_root, MediaKind::screenshot, 0, options, &result);
    }
    if (options.videos)
    {
        probe(paths.video_root, &result.video_root);
        if (result.video_root)
            walk(paths.video_root, MediaKind::video, 0, options, &result);
    }
    if (!options.screenshots || !options.videos)
    {
        // A kind that was asked for only; the walk may have found the other in a mixed tree.
        result.items.erase(
            std::remove_if(result.items.begin(), result.items.end(), [&](const MediaItem &item)
                           { return item.video() ? !options.videos : !options.screenshots; }),
            result.items.end());
    }
    std::sort(result.items.begin(), result.items.end(),
              [](const MediaItem &a, const MediaItem &b)
              {
                  const std::int64_t ka = a.captured.key();
                  const std::int64_t kb = b.captured.key();
                  if (ka != kb)
                      return ka > kb;
                  return a.path > b.path;
              });
    return result;
}

// ---- unreadable captures ---------------------------------------------------

bool capture_reads_blank(const std::string &path)
{
    const int file = ::open(path.c_str(), O_RDONLY);
    if (file < 0)
        return false;
    struct stat info
    {
    };
    if (fstat(file, &info) != 0 || info.st_size <= 0)
    {
        close(file);
        return false;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
    constexpr std::uint64_t kSample = 64 * 1024;
    const std::uint64_t at[] = {0, size / 2, size > kSample ? size - kSample : 0};
    std::vector<char> buffer(kSample);
    bool blank = true;
    std::uint64_t read_any = 0;
    for (const std::uint64_t offset : at)
    {
        const ssize_t got = pread(file, buffer.data(), buffer.size(), static_cast<off_t>(offset));
        if (got <= 0)
            continue;
        read_any += static_cast<std::uint64_t>(got);
        if (std::any_of(buffer.begin(), buffer.begin() + got, [](char c) { return c != 0; }))
        {
            blank = false;
            break;
        }
    }
    close(file);
    return blank && read_any > 0;
}

bool delete_capture(const std::string &path, const std::string &photo_root,
                    const std::string &video_root, std::string *error)
{
    const auto inside = [&](const std::string &root)
    {
        return !root.empty() && path.size() > root.size() + 1 &&
               path.compare(0, root.size() + 1, root + "/") == 0;
    };
    if (path.find("/../") != std::string::npos || path.find("/./") != std::string::npos ||
        !(inside(photo_root) || inside(video_root)))
    {
        if (error)
            *error = "That is not a capture";
        return false;
    }
    if (::unlink(path.c_str()) != 0 && errno != ENOENT)
    {
        if (error)
            *error = std::string("Could not delete it (") + std::strerror(errno) + ")";
        return false;
    }
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    if (dot != std::string::npos && dot > slash)
        for (const char *extension : {".dat", ".meta", ".ext"})
            (void)::unlink((path.substr(0, dot) + extension).c_str());
    // The console keeps each capture in a folder of its own.
    (void)::rmdir(path.substr(0, slash).c_str());
    return true;
}

// ---- game names ------------------------------------------------------------

bool TitleNames::parse_param_json(std::string_view json, std::string *name)
{
    std::string language;
    if (json_find_string(json, "defaultLanguage", &language) && !language.empty())
    {
        const std::string key = "\"" + language + "\"";
        std::size_t at = json.find("localizedParameters");
        at = at == std::string_view::npos ? 0 : at;
        // The language block, not the defaultLanguage value that names it.
        for (;;)
        {
            at = json.find(key, at);
            if (at == std::string_view::npos)
                break;
            std::size_t p = at + key.size();
            while (p < json.size() &&
                   (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == '\t'))
                ++p;
            if (p < json.size() && json[p] == ':')
            {
                std::string found;
                if (json_find_string(json.substr(p), "titleName", &found) && !found.empty())
                {
                    if (name)
                        *name = std::move(found);
                    return true;
                }
                break;
            }
            at = p;
        }
    }
    std::string found;
    if (json_find_string(json, "titleName", &found) && !found.empty())
    {
        if (name)
            *name = std::move(found);
        return true;
    }
    return false;
}

bool TitleNames::parse_param_sfo(std::string_view sfo, std::string *name)
{
    const auto u16 = [&](std::size_t at) -> std::uint32_t
    {
        return at + 2 <= sfo.size() ? static_cast<std::uint8_t>(sfo[at]) |
                                          (static_cast<std::uint8_t>(sfo[at + 1]) << 8)
                                    : 0u;
    };
    const auto u32 = [&](std::size_t at) -> std::uint32_t
    { return at + 4 <= sfo.size() ? u16(at) | (u16(at + 2) << 16) : 0u; };
    if (sfo.size() < 20 || sfo.substr(0, 4) != std::string_view("\0PSF", 4))
        return false;
    const std::uint32_t keys = u32(8);
    const std::uint32_t data = u32(12);
    const std::uint32_t count = u32(16);
    for (std::uint32_t i = 0; i < count && i < 256; ++i)
    {
        const std::size_t entry = 20 + static_cast<std::size_t>(i) * 16;
        if (entry + 16 > sfo.size())
            return false;
        const std::size_t key_at = keys + u16(entry);
        const std::uint32_t length = u32(entry + 4);
        const std::size_t value_at = data + u32(entry + 12);
        if (key_at >= sfo.size())
            return false;
        const char *key = sfo.data() + key_at;
        const std::size_t key_room = sfo.size() - key_at;
        if (strnlen(key, key_room) == key_room)
            return false;
        if (std::strcmp(key, "TITLE") == 0 && value_at + length <= sfo.size())
        {
            std::string value(sfo.substr(value_at, length));
            while (!value.empty() && value.back() == '\0')
                value.pop_back();
            if (value.empty())
                return false;
            if (name)
                *name = std::move(value);
            return true;
        }
    }
    return false;
}

const std::string &TitleNames::name(const std::string &title_id)
{
    const auto cached = cache_.find(title_id);
    if (cached != cache_.end())
        return cached->second;
    std::string found;
    if (looks_like_title_id(title_id))
    {
        std::string bytes;
        if (read_text_file(root_ + "/" + title_id + "/param.json", &bytes, 1u << 20))
            TitleNames::parse_param_json(bytes, &found);
        if (found.empty() &&
            read_text_file(root_ + "/" + title_id + "/param.sfo", &bytes, 1u << 20))
            TitleNames::parse_param_sfo(bytes, &found);
        if (found.empty())
            found = from_captures(title_id);
    }
    return cache_.emplace(title_id, std::move(found)).first->second;
}

bool TitleNames::parse_capture_dat(std::string_view dat, std::string *name)
{
    constexpr std::size_t kNameAt = 0x44;
    constexpr std::size_t kNameRoom = 0x100;
    if (dat.size() <= kNameAt)
        return false;
    std::string_view field = dat.substr(kNameAt, kNameRoom);
    field = field.substr(0, field.find('\0'));
    const auto digits = [](std::string_view text)
    {
        return !text.empty() &&
               std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    // The capture's time follows the name; the home screen's captures have
    // only the time.
    if (field.size() > 15 && field[field.size() - 15] == '_' &&
        digits(field.substr(field.size() - 14)))
        field.remove_suffix(15);
    else if (field.size() == 14 && digits(field))
        return false;
    field = trim(field);
    if (field.empty())
        return false;
    for (const char c : field)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            return false;
    if (name)
        *name = std::string(field);
    return true;
}

std::string TitleNames::from_captures(const std::string &title_id) const
{
    // <root>/<capturing app>/<title id>/<folder>/<capture>.dat
    int tries = 0;
    for (const std::string &root : capture_roots_)
    {
        DIR *apps = opendir(root.c_str());
        if (apps == nullptr)
            continue;
        std::string found;
        while (dirent *app = readdir(apps))
        {
            if (app->d_name[0] == '.')
                continue;
            const std::string title_dir = root + "/" + app->d_name + "/" + title_id;
            DIR *folders = opendir(title_dir.c_str());
            if (folders == nullptr)
                continue;
            while (found.empty() && tries < 24)
            {
                dirent *folder = readdir(folders);
                if (folder == nullptr)
                    break;
                if (folder->d_name[0] == '.')
                    continue;
                const std::string folder_dir = title_dir + "/" + folder->d_name;
                DIR *files = opendir(folder_dir.c_str());
                if (files == nullptr)
                    continue;
                while (dirent *file = readdir(files))
                {
                    const std::string_view file_name(file->d_name);
                    if (file_name.size() < 5 || file_name.substr(file_name.size() - 4) != ".dat")
                        continue;
                    ++tries;
                    std::string bytes;
                    if (read_text_file(folder_dir + "/" + file->d_name, &bytes, 64u << 10) &&
                        parse_capture_dat(bytes, &found))
                        break;
                    if (tries >= 24)
                        break;
                }
                closedir(files);
            }
            closedir(folders);
            if (!found.empty() || tries >= 24)
                break;
        }
        closedir(apps);
        if (!found.empty())
            return found;
    }
    return {};
}

std::string TitleNames::icon_path(const std::string &title_id) const
{
    return root_ + "/" + title_id + "/icon0.png";
}

std::string TitleNames::display(const std::string &title_id)
{
    if (title_id.empty())
        return "Home screen";
    const std::string &found = name(title_id);
    if (!found.empty())
        return found;
    // System software (NPXS...): captures taken on the home screen and in
    // the console's own apps live under the capture service's ID.
    if (title_id.compare(0, 4, "NPXS") == 0)
        return "PS5 system";
    return title_id;
}

} // namespace pshare
