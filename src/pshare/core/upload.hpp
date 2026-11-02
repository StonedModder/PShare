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
    raw_file, // the file itself is the request body
    none,
};

struct HttpRequest
{
    std::string url;
    std::vector<std::string> headers;
    BodyKind body = BodyKind::multipart;
    std::vector<FormPart> parts;
    std::string raw_path;
    std::string raw_type;
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
    std::string part; // optional Part n/N suffix for Telegram captions
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
    // 0 idle, 1 uploading, 2 fallback, 3 posting link, 4 retrying, 5 splitting video, 6 converting
    // WebM
    std::atomic<int> stage{0};
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

} // namespace pshare
