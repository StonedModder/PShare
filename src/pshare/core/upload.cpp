// PShare - Turning a capture into an HTTP request for a destination, and back.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/upload.hpp"

#include "pshare/core/local_copy.hpp"

#include "pshare/core/text.hpp"

#include <unistd.h>

namespace pshare
{
namespace
{
constexpr const char *kCatboxApi = "https://catbox.moe/user/api.php";
constexpr const char *kLitterboxApi = "https://litterbox.catbox.moe/resources/internals/api.php";
constexpr std::uint64_t kTelegramPhotoLimit = 10ull * 1024 * 1024;
constexpr std::size_t kDiscordContentLimit = 2000;

FormPart text_part(std::string name, std::string value)
{
    FormPart part;
    part.name = std::move(name);
    part.value = std::move(value);
    return part;
}

FormPart file_part(std::string name, const MediaItem &item)
{
    FormPart part;
    part.name = std::move(name);
    part.file_path = item.path;
    part.file_name = item.file;
    part.content_type = item.content_type();
    return part;
}

std::string with_link(std::string message, const std::string &link)
{
    if (link.empty() || message.find(link) != std::string::npos)
        return message;
    if (!message.empty())
        message += "\n";
    return message + link;
}

std::string clip(std::string text, std::size_t limit)
{
    if (text.size() > limit)
        text.resize(limit);
    return text;
}
} // namespace

CaptionInfo caption_info(const MediaItem &item, const std::string &game)
{
    CaptionInfo info;
    info.type = item.kind_label();
    info.game = game.empty() ? (item.title_id.empty() ? "PS5" : item.title_id) : game;
    info.title_id = item.title_id;
    info.date = item.captured.date();
    info.time = item.captured.time();
    info.file = item.file;
    info.size = human_size(item.size);
    return info;
}

std::string expand_caption(const std::string &pattern, const CaptionInfo &info)
{
    const Placeholder values[] = {
        {"type", info.type}, {"game", info.game}, {"title_id", info.title_id}, {"date", info.date},
        {"time", info.time}, {"file", info.file}, {"size", info.size},         {"link", info.link},
    };
    return std::string(trim(expand(pattern, values, sizeof(values) / sizeof(values[0]))));
}

std::string default_caption(DestinationType type)
{
    return type == DestinationType::discord ? "{type} from **{game}** - {date} {time}"
                                            : "{type} from {game} - {date} {time}";
}

bool plan_request(const Destination &destination, const MediaItem &item, const CaptionInfo &caption,
                  const std::string &link, HttpRequest *request, std::string *error)
{
    HttpRequest out;
    // Keep-alive is of no use for one upload, and a console socket left open
    // can hold curl until the server gives up on it (ps5-native-app-boilerplate's
    // docs/CURL.md).
    out.headers.push_back("Connection: close");
    out.upload_size = link.empty() ? item.size : 0;
    CaptionInfo info = caption;
    info.link = link;
    const std::string pattern =
        destination.message.empty() ? default_caption(destination.type) : destination.message;
    std::string message = expand_caption(pattern, info);
    if (!link.empty() && pattern.find("{link}") == std::string::npos)
        message = with_link(message, link);

    const auto fail = [&](const char *why)
    {
        if (error)
            *error = why;
        return false;
    };

    switch (destination.type)
    {
    case DestinationType::discord:
    {
        if (!destination.configured())
            return fail("Discord webhook URL is not set");
        // wait=true makes Discord answer with the message, attachments included.
        out.url = destination.url + (destination.url.find('?') == std::string::npos ? "?" : "&") +
                  "wait=true";
        std::string payload =
            "{\"content\":\"" + json_escape(clip(message, kDiscordContentLimit)) + "\"";
        if (!destination.username.empty())
            payload += ",\"username\":\"" + json_escape(destination.username) + "\"";
        payload += "}";
        out.parts.push_back(text_part("payload_json", payload));
        if (link.empty())
            out.parts.push_back(file_part("files[0]", item));
        break;
    }
    case DestinationType::telegram:
    {
        if (!destination.configured())
            return fail("Telegram bot token or chat_id is not set");
        const std::string base = "https://api.telegram.org/bot" + destination.token + "/";
        out.parts.push_back(text_part("chat_id", destination.chat_id));
        if (!link.empty())
        {
            out.url = base + "sendMessage";
            out.parts.push_back(text_part("text", clip(message, 4096)));
            break;
        }
        // Photos over 10 MB and WebM clips only go through as documents.
        const char *method = "sendDocument";
        const char *field = "document";
        if (!item.video() && item.size <= kTelegramPhotoLimit)
        {
            method = "sendPhoto";
            field = "photo";
        }
        else if (item.video() && ends_with_ci(item.file, ".mp4"))
        {
            method = "sendVideo";
            field = "video";
        }
        out.url = base + method;
        out.parts.push_back(text_part("caption", clip(message, 1024)));
        out.parts.push_back(file_part(field, item));
        break;
    }
    case DestinationType::catbox:
    case DestinationType::litterbox:
    {
        if (!link.empty())
            return fail("catbox cannot post a link");
        const bool litter = destination.type == DestinationType::litterbox;
        out.url = litter ? kLitterboxApi : kCatboxApi;
        out.parts.push_back(text_part("reqtype", "fileupload"));
        if (litter)
            out.parts.push_back(text_part(
                "time", destination.litterbox_time.empty() ? "72h" : destination.litterbox_time));
        else if (!destination.token.empty())
            out.parts.push_back(text_part("userhash", destination.token));
        out.parts.push_back(file_part("fileToUpload", item));
        break;
    }
    case DestinationType::http:
    {
        if (!destination.configured())
            return fail("Custom endpoint URL is not set");
        out.url = destination.url;
        for (const std::string &header : destination.headers)
            if (header.find(':') != std::string::npos)
                out.headers.push_back(header);
        for (const auto &[key, value] : destination.form)
            out.parts.push_back(text_part(key, expand_caption(value, info)));
        if (link.empty())
            out.parts.push_back(
                file_part(destination.field.empty() ? "file" : destination.field, item));
        else
            out.parts.push_back(text_part("link", link));
        break;
    }
    case DestinationType::folder:
    case DestinationType::smb:
        return fail("Drives and shares are copied, not sent over HTTP");
    default:
        return fail("Unknown destination type");
    }
    *request = std::move(out);
    return true;
}

ResponseVerdict read_response(DestinationType type, long status, const std::string &body)
{
    ResponseVerdict verdict;
    const bool success = status >= 200 && status < 300;
    // Rate limits and server trouble are worth another try; a 4xx is not.
    verdict.retry = status == 0 || status == 408 || status == 429 || status >= 500;
    switch (type)
    {
    case DestinationType::discord:
        if (success)
        {
            verdict.ok = true;
            // The attachment's CDN link, else nothing (a link post has none).
            const std::size_t attachments = body.find("\"attachments\"");
            if (attachments != std::string::npos)
                json_find_string(std::string_view(body).substr(attachments), "url", &verdict.link);
            return verdict;
        }
        if (!json_find_string(body, "message", &verdict.error))
            verdict.error = "Discord answered " + std::to_string(status);
        if (status == 413)
            verdict.error = "File too large for this Discord server";
        return verdict;
    case DestinationType::telegram:
    {
        bool ok = false;
        if (success && json_find_bool(body, "ok", &ok) && ok)
        {
            verdict.ok = true;
            return verdict;
        }
        if (!json_find_string(body, "description", &verdict.error))
            verdict.error = "Telegram answered " + std::to_string(status);
        return verdict;
    }
    case DestinationType::catbox:
    case DestinationType::litterbox:
    {
        const std::string_view text = trim(body);
        if (success && starts_with(text, "https://"))
        {
            verdict.ok = true;
            verdict.link.assign(text);
            return verdict;
        }
        verdict.error = text.empty() ? "catbox answered " + std::to_string(status)
                                     : std::string(text.substr(0, 160));
        // catbox answers 200 with an error text when it is overloaded.
        if (success)
            verdict.retry = true;
        return verdict;
    }
    case DestinationType::http:
    default:
        if (success)
        {
            verdict.ok = true;
            if (!json_find_string(body, "url", &verdict.link) &&
                !json_find_string(body, "link", &verdict.link))
                find_url(body, &verdict.link);
            return verdict;
        }
        verdict.error = "Server answered " + std::to_string(status);
        if (!trim(body).empty())
            verdict.error += ": " + std::string(trim(body).substr(0, 120));
        return verdict;
    }
}

namespace
{
struct Attempt
{
    bool ok = false;
    std::string link;
    std::string error;
    int attempts = 0;
};

Attempt send_with_retries(const Config &config, const Destination &destination,
                          const MediaItem &item, const CaptionInfo &caption,
                          const std::string &link, UploadProgress *progress, Transport transport,
                          void *user, bool pause)
{
    Attempt result;
    HttpRequest request;
    if (!plan_request(destination, item, caption, link, &request, &result.error))
        return result;
    for (int attempt = 1; attempt <= config.retries; ++attempt)
    {
        result.attempts = attempt;
        if (progress != nullptr)
        {
            if (progress->cancel.load())
            {
                result.error = "Cancelled";
                return result;
            }
            progress->sent = 0;
            progress->total = request.upload_size;
        }
        const HttpResult http = transport(request, progress, user);
        if (progress != nullptr && progress->cancel.load())
        {
            result.error = "Cancelled";
            return result;
        }
        ResponseVerdict verdict;
        if (!http.transport_ok)
        {
            verdict.retry = true;
            verdict.error = http.error.empty() ? "Network error" : http.error;
        }
        else
        {
            verdict = read_response(destination.type, http.status, http.body);
        }
        if (verdict.ok)
        {
            result.ok = true;
            result.link = verdict.link.empty() ? link : verdict.link;
            result.error.clear();
            return result;
        }
        result.error = verdict.error;
        if (!verdict.retry || attempt == config.retries)
            return result;
        if (progress != nullptr)
            progress->stage = 4;
        if (pause)
            usleep(static_cast<useconds_t>(2000000 * attempt)); // 2 s, 4 s, ...
    }
    return result;
}
} // namespace

UploadOutcome upload_capture_with(const Config &config, const Destination &destination,
                                  const MediaItem &item, const std::string &game,
                                  UploadProgress *progress, Transport transport, void *user,
                                  bool pause_between_tries)
{
    // A capture the console reads back as zeros would arrive as a broken file.
    if (capture_reads_blank(item.path))
    {
        UploadOutcome blank;
        blank.destination = destination.id;
        blank.error = "The background uploader can't read this capture right now (the console "
                      "hands it back as zeros), so it was not sent. Open PShare and send it "
                      "again";
        return blank;
    }
    // A drive or a PC share: a file copy, no HTTP at all.
    if (destination.type == DestinationType::folder)
        return copy_to_folder(destination, Paths{}, item, game, progress);
    if (destination.type == DestinationType::smb)
        return copy_to_share(destination, item, game, progress);
    UploadOutcome outcome;
    outcome.destination = destination.id;
    const CaptionInfo caption = caption_info(item, game);
    const std::uint64_t limit = destination.size_limit();
    std::string link;

    if (limit != 0 && item.size > limit)
    {
        const Destination *fallback =
            destination.fallback.empty() ? nullptr : config.find(destination.fallback);
        if (fallback == nullptr || fallback == &destination || !fallback->configured())
        {
            outcome.error = "Too large for " + destination.display_name() + " (" +
                            human_size(item.size) + ", limit " + human_size(limit) + ")";
            return outcome;
        }
        const std::uint64_t fallback_limit = fallback->size_limit();
        if (fallback_limit != 0 && item.size > fallback_limit)
        {
            outcome.error =
                "Too large for " + destination.display_name() + " and " + fallback->display_name();
            return outcome;
        }
        if (progress != nullptr)
            progress->stage = 2;
        const Attempt hosted = send_with_retries(config, *fallback, item, caption, {}, progress,
                                                 transport, user, pause_between_tries);
        outcome.attempts += hosted.attempts;
        if (!hosted.ok)
        {
            outcome.error = fallback->display_name() + ": " + hosted.error;
            return outcome;
        }
        link = hosted.link;
        // A fallback that cannot carry a message is the end of the line.
        if (destination.type == DestinationType::catbox ||
            destination.type == DestinationType::litterbox)
        {
            outcome.ok = true;
            outcome.link = link;
            outcome.destination = fallback->id;
            return outcome;
        }
        if (progress != nullptr)
            progress->stage = 3;
    }
    else if (progress != nullptr)
    {
        progress->stage = 1;
    }

    const Attempt sent = send_with_retries(config, destination, item, caption, link, progress,
                                           transport, user, pause_between_tries);
    outcome.attempts += sent.attempts;
    outcome.ok = sent.ok;
    outcome.link = sent.ok ? (sent.link.empty() ? link : sent.link) : std::string();
    outcome.error = sent.error;
    if (progress != nullptr)
        progress->stage = 0;
    return outcome;
}

namespace
{
HttpResult curl_transport(const HttpRequest &request, UploadProgress *progress, void *user)
{
    const int timeout = user != nullptr ? *static_cast<const int *>(user) : 900;
    return perform_request(request, progress, timeout);
}
} // namespace

UploadOutcome upload_capture(const Config &config, const Destination &destination,
                             const MediaItem &item, const std::string &game,
                             UploadProgress *progress)
{
    int timeout = 900; // a 1 GB clip over a slow uplink
    return upload_capture_with(config, destination, item, game, progress, curl_transport, &timeout,
                               true);
}

} // namespace pshare
