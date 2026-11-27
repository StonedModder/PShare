// PShare - Sending a capture to OneDrive (Microsoft Graph).
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The destination keeps the client ID of the user's own app registration and
// a refresh token for it (the README's OneDrive guide gets both). One upload:
//
//   1. trade the refresh token for an access token. Microsoft hands back a
//      new refresh token too; it is kept in onedrive-<id>.token beside the
//      config, so the sign-in outlives the token that was typed in;
//   2. open an upload session for <folder>/<game>/<file>;
//   3. send the file in slices; the last one answers with the new item;
//   4. as a fallback for a chat, make a view-only link anyone can open.
//
// Planned here and performed by the Transport, so the tests run it offline.

#include "pshare/core/upload.hpp"

#include "pshare/core/local_copy.hpp"
#include "pshare/core/text.hpp"

#include <algorithm>
#include <unistd.h>

namespace pshare
{
namespace
{
constexpr const char *kTokenUrl = "https://login.microsoftonline.com/common/oauth2/v2.0/token";
constexpr const char *kDriveRoot = "https://graph.microsoft.com/v1.0/me/drive/root:/";
constexpr const char *kScope = "Files.ReadWrite offline_access";
// Graph takes slices in multiples of 320 KiB, below 60 MiB.
constexpr std::uint64_t kSlice = 32ull * 320 * 1024;

// The newest refresh token saved for the one in the config, else that one.
// The file is "<token in the config>\n<newest token>\n", so typing in a new
// token replaces whatever was saved for the old one.
std::string refresh_token(const Destination &destination, const std::string &token_file)
{
    std::string text;
    if (read_text_file(token_file, &text))
    {
        const std::vector<std::string> lines = split(text, '\n');
        if (lines.size() >= 2 && lines[0] == destination.token && !trim(lines[1]).empty())
            return std::string(trim(lines[1]));
    }
    return destination.token;
}

// <folder>/<game>/<file>, each part made safe for OneDrive and the URL.
std::string drive_path(const Destination &destination, const MediaItem &item,
                       const std::string &game)
{
    std::string out;
    for (const std::string &part : split(destination.path, '/'))
        if (!trim(part).empty())
            out += url_encode(safe_name(part)) + "/";
    return out + url_encode(safe_name(game)) + "/" + url_encode(item.file);
}

// The first line of what the service said, or its status.
std::string answer_error(const HttpResult &http)
{
    if (!http.transport_ok)
        return http.error.empty() ? "Network error" : http.error;
    std::string text;
    if (!json_find_string(http.body, "error_description", &text) &&
        !json_find_string(http.body, "message", &text))
        return "OneDrive answered " + std::to_string(http.status);
    text = text.substr(0, text.find_first_of("\r\n"));
    return text.substr(0, 160);
}

// One request, tried again while the failure looks passing.
HttpResult send_request(const Config &config, const HttpRequest &request, UploadProgress *progress,
                        Transport transport, void *user, bool pause, int *attempts)
{
    HttpResult http;
    for (int attempt = 1; attempt <= config.retries; ++attempt)
    {
        ++*attempts;
        http = transport(request, progress, user);
        const bool passing =
            !http.transport_ok || http.status == 408 || http.status == 429 || http.status >= 500;
        if (!passing || attempt == config.retries ||
            (progress != nullptr && progress->cancel.load()))
            return http;
        if (progress != nullptr)
            progress->stage = 4;
        if (pause)
            usleep(static_cast<useconds_t>(2000000 * attempt)); // 2 s, 4 s, ...
    }
    return http;
}

UploadOutcome finish(UploadOutcome outcome, UploadProgress *progress)
{
    if (progress != nullptr)
        progress->stage = 0;
    return outcome;
}
} // namespace

UploadOutcome send_to_onedrive(const Config &config, const Destination &destination,
                               const MediaItem &item, const std::string &game,
                               UploadProgress *progress, Transport transport, void *user,
                               bool pause_between_tries, const std::string &token_file,
                               bool share_link)
{
    UploadOutcome outcome;
    outcome.destination = destination.id;
    if (!destination.configured())
    {
        outcome.error = "OneDrive client ID or refresh token is not set";
        return outcome;
    }
    if (item.size == 0)
    {
        outcome.error = "The capture is empty";
        return outcome;
    }
    if (progress != nullptr)
    {
        progress->stage = 1;
        progress->sent = 0;
        progress->total = item.size;
    }

    // 1. An access token. The small requests get no progress: it is the file's.
    const std::string refresh = refresh_token(destination, token_file);
    HttpRequest sign_in;
    sign_in.url = kTokenUrl;
    sign_in.headers.push_back("Connection: close");
    sign_in.body = BodyKind::text;
    sign_in.raw_type = "application/x-www-form-urlencoded";
    sign_in.text = "client_id=" + url_encode(destination.client_id) +
                   "&grant_type=refresh_token&refresh_token=" + url_encode(refresh) +
                   "&scope=" + url_encode(kScope);
    HttpResult http = send_request(config, sign_in, nullptr, transport, user, pause_between_tries,
                                   &outcome.attempts);
    std::string access;
    if (http.status != 200 || !json_find_string(http.body, "access_token", &access))
    {
        std::string code;
        outcome.error = json_find_string(http.body, "error", &code) && code == "invalid_grant"
                            ? "The OneDrive sign-in has expired or was revoked; get a new "
                              "refresh token"
                            : "OneDrive sign-in: " + answer_error(http);
        return finish(outcome, progress);
    }
    std::string renewed;
    if (json_find_string(http.body, "refresh_token", &renewed) && renewed != refresh)
        (void)write_text_file_atomic(token_file, destination.token + "\n" + renewed + "\n");

    // 2. An upload session; a file of the same name gets a new name, not replaced.
    HttpRequest session;
    session.url = kDriveRoot + drive_path(destination, item, game) + ":/createUploadSession";
    session.headers = {"Connection: close", "Authorization: Bearer " + access};
    session.body = BodyKind::text;
    session.raw_type = "application/json";
    session.text = R"({"item":{"@microsoft.graph.conflictBehavior":"rename"}})";
    http = send_request(config, session, nullptr, transport, user, pause_between_tries,
                        &outcome.attempts);
    std::string upload_url;
    if (http.status != 200 || !json_find_string(http.body, "uploadUrl", &upload_url))
    {
        outcome.error = answer_error(http);
        return finish(outcome, progress);
    }

    // 3. The slices. The session URL carries its own authorization.
    for (std::uint64_t offset = 0; offset < item.size;)
    {
        if (progress != nullptr && progress->cancel.load())
        {
            outcome.error = "Cancelled";
            return finish(outcome, progress);
        }
        const std::uint64_t length = std::min(kSlice, item.size - offset);
        HttpRequest slice;
        slice.url = upload_url;
        slice.method = "PUT";
        slice.headers = {"Connection: close", "Content-Range: bytes " + std::to_string(offset) +
                                                  "-" + std::to_string(offset + length - 1) + "/" +
                                                  std::to_string(item.size)};
        slice.body = BodyKind::raw_file;
        slice.raw_path = item.path;
        slice.raw_type = "application/octet-stream";
        slice.raw_offset = offset;
        slice.raw_length = length;
        slice.upload_size = length;
        if (progress != nullptr)
            progress->stage = 1;
        http = send_request(config, slice, progress, transport, user, pause_between_tries,
                            &outcome.attempts);
        if (progress != nullptr && progress->cancel.load())
        {
            outcome.error = "Cancelled";
            return finish(outcome, progress);
        }
        if (http.status == 200 || http.status == 201)
        {
            outcome.ok = true;
            json_top_string(http.body, "webUrl", &outcome.link);
            if (progress != nullptr)
                progress->sent = item.size;
            // 4. A link for the chat. Some work accounts allow no anonymous
            // links; the owner's link is kept then.
            std::string id;
            if (share_link && json_top_string(http.body, "id", &id))
            {
                HttpRequest link;
                link.url = "https://graph.microsoft.com/v1.0/me/drive/items/" + url_encode(id) +
                           "/createLink";
                link.headers = {"Connection: close", "Authorization: Bearer " + access};
                link.body = BodyKind::text;
                link.raw_type = "application/json";
                link.text = R"({"type":"view","scope":"anonymous"})";
                const HttpResult shared = send_request(config, link, nullptr, transport, user,
                                                       pause_between_tries, &outcome.attempts);
                std::string url;
                if ((shared.status == 200 || shared.status == 201) &&
                    json_find_string(shared.body, "webUrl", &url))
                    outcome.link = url;
            }
            return finish(outcome, progress);
        }
        if (http.status != 202)
        {
            outcome.error = answer_error(http);
            return finish(outcome, progress);
        }
        offset += length;
        if (progress != nullptr)
            progress->sent = offset;
    }
    outcome.error = "OneDrive did not finish the upload";
    return finish(outcome, progress);
}

} // namespace pshare
