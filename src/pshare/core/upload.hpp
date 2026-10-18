// PShare - Turning a capture into an HTTP request for a destination, and back.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Planning is pure (unit tested on the PC); upload_curl.cpp performs the plan
// with libcurl. The sequence for one capture:
//
//   1. pick the destination (per-title override, default, first configured);
//   2. when the file is over that destination's limit and it names a
//      fallback (Discord -> catbox), upload there first and post the link;
//   3. retry transient failures with a growing pause;
//   4. read the answer: the link Discord / Telegram / catbox gives back.

#pragma once

#include "pshare/core/config.hpp"
#include "pshare/core/media.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pshare
{

struct FormPart
{
    std::string name;
    std::string value;     // text parts
    std::string file_path; // file parts: streamed from disk, never held in memory
    std::string file_name;
    std::string content_type;
    bool is_file() const
    {
        return !file_path.empty();
    }
};

enum class BodyKind : std::uint8_t
{
    multipart,
    raw_file, // the file itself (or a slice of it) is the request body
    text,     // `text` is the body
    none,
};

struct HttpRequest
{
    std::string url;
    std::string method; // empty: POST
    std::vector<std::string> headers;
    BodyKind body = BodyKind::multipart;
    std::vector<FormPart> parts;
    std::string raw_path;
    std::string raw_type; // the content type of a raw_file or text body
    // raw_file: the slice of the file to send; a zero length sends all of it.
    // Progress then counts from raw_offset and keeps the whole file's total.
    std::uint64_t raw_offset = 0;
    std::uint64_t raw_length = 0;
    std::string text;
    std::uint64_t upload_size = 0; // for progress
};

// What the caption template can use.
struct CaptionInfo
{
    std::string type; // "Screenshot" / "Clip"
    std::string game;
    std::string title_id;
    std::string date;
    std::string time;
    std::string file;
    std::string size;
    std::string link;
};
CaptionInfo caption_info(const MediaItem &item, const std::string &game);
std::string expand_caption(const std::string &pattern, const CaptionInfo &info);
std::string default_caption(DestinationType type);

// The request that sends `item` (or, with a non-empty `link`, a message that
// carries an already uploaded link instead of the file).
bool plan_request(const Destination &destination, const MediaItem &item, const CaptionInfo &caption,
                  const std::string &link, HttpRequest *request, std::string *error);

// Reads a response: success, the public link if the service returns one,
// and a short reason on failure. `retry` says whether trying again may help.
struct ResponseVerdict
{
    bool ok = false;
    bool retry = false;
    int retry_after = 0; // a rate limit: seconds the service asks to wait (Telegram, Discord)
    std::string link;
    std::string error;
};
ResponseVerdict read_response(DestinationType type, long status, const std::string &body);

// ---- performing ------------------------------------------------------------

struct UploadProgress
{
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> total{0};
    std::atomic<bool> cancel{false};
    std::atomic<int> stage{0}; // 0 idle, 1 uploading, 2 fallback, 3 posting link, 4 retrying
};

struct UploadOutcome
{
    bool ok = false;
    std::string destination; // id of the destination that got the post
    std::string link;
    std::string error;
    int attempts = 0;
};

// Sends one request. Implemented in upload_curl.cpp.
struct HttpResult
{
    bool transport_ok = false;
    long status = 0;
    std::string body;
    std::string error;
};
HttpResult perform_request(const HttpRequest &request, UploadProgress *progress,
                           int timeout_seconds);
// Once per process, before any thread uploads.
void upload_global_init();

// The whole sequence above, for one capture.
UploadOutcome upload_capture(const Config &config, const Destination &destination,
                             const MediaItem &item, const std::string &game,
                             UploadProgress *progress);

// The same request planned and read, without the network (tests use it with
// a fake transport).
using Transport = HttpResult (*)(const HttpRequest &request, UploadProgress *progress, void *user);
UploadOutcome upload_capture_with(const Config &config, const Destination &destination,
                                  const MediaItem &item, const std::string &game,
                                  UploadProgress *progress, Transport transport, void *user,
                                  bool pause_between_tries);

// Telegram photos. Telegram takes a photo of at most 10 MB and shows it at
// most 2560 px on its long side. A bigger screenshot goes twice: a JPEG copy
// at that size as a photo, so it is in the chat's photos, and the original as
// a file, captioned "Full size". make_telegram_photo() writes the copy (false
// when the screenshot cannot be read; it then goes as a file only).
constexpr int kTelegramPhotoSide = 2560;
bool needs_photo_copy(const Destination &destination, const MediaItem &item);
bool make_telegram_photo(const MediaItem &item, const std::string &target, MediaItem *photo);

// Telegram albums. A "Send all" sends up to kAlbumSize captures for one chat
// as a single sendMediaGroup message, each with its own caption, instead of
// one message each. Telegram keeps photos and files in albums of their own:
// screenshots go in photo albums (with their full-size originals following
// in a file album when they are too big to be photos), clips in file albums.
// An album stays within the destination's size limit (the Bot API's 50 MB).
constexpr std::size_t kAlbumSize = 10;
enum class AlbumKind : std::uint8_t
{
    none, // not for an album: another service, or over the size limit
    photo,
    file,
};
AlbumKind album_kind(const Destination &destination, const MediaItem &item);
// One sendMediaGroup request: `kind` photo takes only photos Telegram accepts
// as such (up to 10 MB), file takes anything.
bool plan_album(const Destination &destination, AlbumKind kind, const std::vector<MediaItem> &items,
                const std::vector<CaptionInfo> &captions, HttpRequest *request, std::string *error);
// Sends the album with the usual retries; one outcome for all of it.
UploadOutcome upload_album_with(const Config &config, const Destination &destination,
                                const std::vector<MediaItem> &items,
                                const std::vector<std::string> &games, UploadProgress *progress,
                                Transport transport, void *user, bool pause_between_tries);
UploadOutcome upload_album(const Config &config, const Destination &destination,
                           const std::vector<MediaItem> &items,
                           const std::vector<std::string> &games, UploadProgress *progress);

// OneDrive takes several requests (sign in, open an upload session, send the
// slices), so it has its own sequence; implemented in upload_onedrive.cpp.
// `token_file` keeps the refresh token Microsoft hands back (Paths::onedrive_token).
// With `share_link` (OneDrive holding a file too big for a chat) the link is a
// view-only one anyone can open, so the chat can; otherwise the owner's own.
UploadOutcome send_to_onedrive(const Config &config, const Destination &destination,
                               const MediaItem &item, const std::string &game,
                               UploadProgress *progress, Transport transport, void *user,
                               bool pause_between_tries, const std::string &token_file,
                               bool share_link = false);

} // namespace pshare
