// PShare - Finding the console's screenshots and video clips.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 keeps captures under /user/av_contents, one folder tree per kind:
//
//   /user/av_contents/photo/<app>/<TITLE_ID>/<folder>/<YYYYMMDD_HHMMSS_xxxxxxxx>.jpg
//   /user/av_contents/video/<app>/<TITLE_ID>/<folder>/<YYYYMMDD_HHMMSS_xxxxxxxx>.webm
//
// (layout as documented by Porkfolio). Screenshots may also be PNG and clips
// MP4, depending on the capture settings. Nothing here depends on the exact
// depth: the walk takes any file with a capture extension and reads the title
// ID from whichever folder name looks like one.

#pragma once

#include "pshare/core/config.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pshare
{

enum class MediaKind : std::uint8_t
{
    screenshot,
    video,
};

struct CaptureTime
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    bool valid() const
    {
        return year >= 2000 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
    }
    // Sortable: YYYYMMDDHHMMSS as a number.
    std::int64_t key() const;
    std::string date() const; // "2026-10-09"
    std::string time() const; // "18:35"
};

struct MediaItem
{
    std::string path;     // absolute
    std::string file;     // the name in its folder
    std::string title_id; // "PPSA01234", or empty when no folder looks like one
    MediaKind kind = MediaKind::screenshot;
    std::uint64_t size = 0;
    std::int64_t mtime = 0; // seconds
    CaptureTime captured;   // from the name; from mtime when the name has none

    bool video() const
    {
        return kind == MediaKind::video;
    }
    const char *kind_label() const
    {
        return video() ? "Clip" : "Screenshot";
    }
    const char *content_type() const;
    // A JPEG or PNG: what browsers, chats and the app's decoder can show. The
    // PS5 saves HDR screenshots as JPEG XR (.jxr), which none of them do;
    // those are kept and sent as files.
    bool web_image() const;
    // A JPEG XR (.jxr) HDR screenshot: PShare decodes it (picture.hpp) and
    // sends a JPEG copy where a chat or host has to show it.
    bool jxr() const;
};

struct ScanOptions
{
    bool screenshots = true;
    bool videos = true;
    int max_depth = 7;
    std::size_t max_items = 50000;
};

struct ScanResult
{
    std::vector<MediaItem> items; // newest first
    bool photo_root = false;      // the folder could be opened
    bool video_root = false;
    int folders = 0;
    int open_errno = 0; // the first error opening a root (EPERM: not elevated)
    // Folders below the roots that could not be opened: their captures are
    // missing from the scan. The first one and why, for the log.
    int unreadable = 0;
    std::string unreadable_path;
    int unreadable_errno = 0;
};

ScanResult scan_media(const Paths &paths, const ScanOptions &options);

// One file, as a scan would describe it (the daemon re-checks sizes this way).
bool describe_media(const std::string &path, MediaItem *item);

// True when a capture that is not empty reads back as nothing but zeros (its
// start, middle and end are sampled). Such a file cannot be played or sent;
// false when it cannot be opened at all.
bool capture_reads_blank(const std::string &path);

// Removes a capture and the files the console keeps beside it (.dat, .meta,
// .ext), then its folder if that is left empty. Only paths below `photo_root`
// or `video_root` are accepted.
bool delete_capture(const std::string &path, const std::string &photo_root,
                    const std::string &video_root, std::string *error);

// "Delete once sent" when the deleting has to wait: pshared, a payload, may not
// be allowed to delete captures, so it lists them in Paths::pending_deletes()
// and the app deletes them. Adding appends a line; deleting takes the list
// over (renamed first, so pshared can keep adding), deletes each capture and
// puts back the ones that could not go. Returns how many were deleted.
bool add_pending_delete(const Paths &paths, const std::string &capture);
int delete_pending_captures(const Paths &paths);

// The name's extension says screenshot or video; false for anything else.
bool capture_kind(std::string_view file, MediaKind *kind);
// YYYYMMDD_HHMMSS or 14 digits in a row, anywhere in the name.
bool parse_capture_time(std::string_view file, CaptureTime *time);
// The last path segment that looks like a title ID (four capitals, five digits).
std::string title_id_from_path(std::string_view path);
bool looks_like_title_id(std::string_view text);

// Game names. Installed PS5 titles carry param.json in /user/appmeta, some
// PS4 titles param.sfo. Every capture also has a .dat beside it with the
// game's name as it was when the capture was taken, which covers PS4 titles
// and games that are no longer installed; `capture_roots` (the photo and
// video folders) are searched for one. Results are cached; unknown IDs come
// back empty.
class TitleNames
{
  public:
    explicit TitleNames(std::string appmeta_root, std::vector<std::string> capture_roots = {})
        : root_(std::move(appmeta_root)), capture_roots_(std::move(capture_roots))
    {
    }
    const std::string &name(const std::string &title_id);
    std::string icon_path(const std::string &title_id) const;
    // A name that is never empty: the game's, the ID, or "Home screen".
    std::string display(const std::string &title_id);

    static bool parse_param_json(std::string_view json, std::string *name);
    static bool parse_param_sfo(std::string_view sfo, std::string *name);
    // The name in a capture's .dat: "<game>_<YYYYMMDDHHMMSS>" at offset 0x44.
    static bool parse_capture_dat(std::string_view dat, std::string *name);

  private:
    std::string from_captures(const std::string &title_id) const;

    std::string root_;
    std::vector<std::string> capture_roots_;
    std::map<std::string, std::string> cache_; // node-based: name() references stay valid
};

} // namespace pshare
