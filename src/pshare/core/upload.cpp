// PShare - Turning a capture into an HTTP request for a destination, and back.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/upload.hpp"

#include "pshare/core/local_copy.hpp"
#include "pshare/core/picture.hpp"

#include "pshare/core/text.hpp"

#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

namespace pshare
{
namespace
{
constexpr const char *kCatboxApi = "https://catbox.moe/user/api.php";
constexpr const char *kLitterboxApi = "https://litterbox.catbox.moe/resources/internals/api.php";
constexpr std::uint64_t kTelegramPhotoLimit = 10ull * 1024 * 1024;
constexpr std::size_t kDiscordContentLimit = 2000;

// Where a passing copy is written: the data folder on the console, /tmp on a PC.
std::string scratch_path(const std::string &name)
{
    const std::string data = Paths{}.data_root;
    struct stat info
    {
    };
    return (::stat(data.c_str(), &info) == 0 ? data : std::string("/tmp")) + "/" + name;
}

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
        if (item.web_image() && item.size <= kTelegramPhotoLimit)
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
    case DestinationType::onedrive:
        return fail("OneDrive is sent in several requests, not one");
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
    // Telegram: "parameters":{"retry_after":35}; Discord: "retry_after":1.5.
    double wait = 0.0;
    if (status == 429 && json_find_number(body, "retry_after", &wait))
        verdict.retry_after = static_cast<int>(std::min(wait, 600.0)) + 1;
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

// A planned request, tried again while the failure looks passing.
Attempt send_planned(const Config &config, DestinationType type, const HttpRequest &request,
                     const std::string &link, UploadProgress *progress, Transport transport,
                     void *user, bool pause)
{
    Attempt result;
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
            verdict = read_response(type, http.status, http.body);
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
        {
            // As long as a rate limit asks, else 2 s, 4 s, ...; a cancel ends the wait.
            const int seconds = verdict.retry_after > 0 ? verdict.retry_after : 2 * attempt;
            for (int tenth = 0; tenth < seconds * 10; ++tenth)
            {
                if (progress != nullptr && progress->cancel.load())
                    break;
                usleep(100000);
            }
        }
    }
    return result;
}

Attempt send_with_retries(const Config &config, const Destination &destination,
                          const MediaItem &item, const CaptionInfo &caption,
                          const std::string &link, UploadProgress *progress, Transport transport,
                          void *user, bool pause)
{
    HttpRequest request;
    Attempt result;
    if (!plan_request(destination, item, caption, link, &request, &result.error))
        return result;
    return send_planned(config, destination.type, request, link, progress, transport, user, pause);
}
} // namespace

bool needs_photo_copy(const Destination &destination, const MediaItem &item)
{
    return destination.type == DestinationType::telegram &&
           (item.jxr() || (item.web_image() && item.size > kTelegramPhotoLimit));
}

bool make_photo_copy(const MediaItem &item, const std::string &target, MediaItem *photo)
{
    Picture picture;
    if (!decode_picture(item.path, kTelegramPhotoSide, kTelegramPhotoSide, &picture))
        return false;
    std::string jpeg = encode_jpeg(picture, 88);
    if (jpeg.size() > kTelegramPhotoLimit)
        jpeg = encode_jpeg(picture, 70);
    if (jpeg.empty() || jpeg.size() > kTelegramPhotoLimit || !write_text_file_atomic(target, jpeg))
        return false;
    *photo = item;
    photo->path = target;
    photo->size = jpeg.size();
    photo->file = item.file.substr(0, item.file.rfind('.')) + ".jpg";
    return true;
}

AlbumKind album_kind(const Destination &destination, const MediaItem &item)
{
    const std::uint64_t limit = destination.size_limit();
    if (destination.type != DestinationType::telegram || (limit != 0 && item.size > limit))
        return AlbumKind::none;
    // A JPEG XR goes as a photo copy, with the original among the files.
    return item.web_image() || item.jxr() ? AlbumKind::photo : AlbumKind::file;
}

bool plan_album(const Destination &destination, AlbumKind kind, const std::vector<MediaItem> &items,
                const std::vector<CaptionInfo> &captions, HttpRequest *request, std::string *error)
{
    const auto fail = [&](const char *why)
    {
        if (error)
            *error = why;
        return false;
    };
    if (!destination.configured())
        return fail("Telegram bot token or chat_id is not set");
    if (items.size() < 2 || items.size() > kAlbumSize || captions.size() != items.size())
        return fail("An album holds 2 to 10 captures");
    std::uint64_t total = 0;
    for (const MediaItem &item : items)
        total += item.size;
    const std::uint64_t limit = destination.size_limit();
    if (limit != 0 && total > limit)
        return fail("Too much for one album");
    HttpRequest out;
    out.headers.push_back("Connection: close");
    out.url = "https://api.telegram.org/bot" + destination.token + "/sendMediaGroup";
    out.parts.push_back(text_part("chat_id", destination.chat_id));
    const std::string pattern =
        destination.message.empty() ? default_caption(destination.type) : destination.message;
    // Each capture is a part of its own, which the media list names as attach://pN.
    const char *type = kind == AlbumKind::photo ? "photo" : "document";
    std::string media = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const bool photo = items[i].web_image() && items[i].size <= kTelegramPhotoLimit;
        if (kind == AlbumKind::none || (kind == AlbumKind::photo && !photo))
            return fail("Only photos of up to 10 MB go in a photo album");
        const std::string name = "p" + std::to_string(i);
        media += std::string(i == 0 ? "" : ",") + R"({"type":")" + type +
                 R"(","media":"attach://)" + name + R"(","caption":")" +
                 json_escape(clip(expand_caption(pattern, captions[i]), 1024)) + R"("})";
        out.parts.push_back(file_part(name, items[i]));
        out.upload_size += items[i].size;
    }
    out.parts.insert(out.parts.begin() + 1, text_part("media", media + "]"));
    *request = std::move(out);
    return true;
}

UploadOutcome upload_album_with(const Config &config, const Destination &destination,
                                const std::vector<MediaItem> &items,
                                const std::vector<std::string> &games, UploadProgress *progress,
                                Transport transport, void *user, bool pause_between_tries)
{
    UploadOutcome outcome;
    outcome.destination = destination.id;
    const AlbumKind kind = items.empty() ? AlbumKind::none : album_kind(destination, items[0]);
    // What goes in the album, and (screenshots too big to be photos) the
    // originals that follow as files.
    std::vector<MediaItem> album;
    std::vector<CaptionInfo> captions;
    std::vector<MediaItem> originals;
    std::vector<CaptionInfo> original_captions;
    std::vector<std::string> copies;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const CaptionInfo caption =
            caption_info(items[i], i < games.size() ? games[i] : std::string());
        MediaItem photo = items[i];
        if (kind == AlbumKind::photo && needs_photo_copy(destination, items[i]))
        {
            originals.push_back(items[i]);
            original_captions.push_back(caption);
            const std::string copy =
                scratch_path("telegram-" + std::to_string(getpid()) + "-" + items[i].file + ".jpg");
            if (!make_photo_copy(items[i], copy, &photo))
                continue; // a file only
            copies.push_back(copy);
        }
        album.push_back(photo);
        captions.push_back(caption);
    }
    if (progress != nullptr)
        progress->stage = 1;
    // One capture is a message of its own; two or more an album.
    const auto send_group = [&](const Destination &to, AlbumKind as,
                                const std::vector<MediaItem> &group,
                                const std::vector<CaptionInfo> &group_captions)
    {
        if (group.size() == 1)
            return send_with_retries(config, to, group[0], group_captions[0], {}, progress,
                                     transport, user, pause_between_tries);
        Attempt failed;
        HttpRequest request;
        if (!plan_album(to, as, group, group_captions, &request, &failed.error))
            return failed;
        return send_planned(config, to.type, request, {}, progress, transport, user,
                            pause_between_tries);
    };
    Attempt sent;
    sent.ok = true;
    if (!album.empty())
        sent = send_group(destination, kind, album, captions);
    // The originals, in as many file albums as the size limit needs.
    Destination full = destination;
    full.message = "Full size \xC2\xB7 {file}";
    const std::uint64_t limit = destination.size_limit();
    for (std::size_t first = 0; sent.ok && first < originals.size();)
    {
        std::size_t end = first + 1;
        std::uint64_t total = originals[first].size;
        while (end < originals.size() && end - first < kAlbumSize &&
               (limit == 0 || total + originals[end].size <= limit))
            total += originals[end++].size;
        const std::vector<MediaItem> group(originals.begin() + first, originals.begin() + end);
        const std::vector<CaptionInfo> group_captions(original_captions.begin() + first,
                                                      original_captions.begin() + end);
        const Attempt files = send_group(full, AlbumKind::file, group, group_captions);
        sent.attempts += files.attempts;
        sent.ok = files.ok;
        sent.error = files.error;
        first = end;
    }
    for (const std::string &copy : copies)
        ::unlink(copy.c_str());
    outcome.ok = sent.ok;
    outcome.attempts = sent.attempts;
    outcome.error = sent.error;
    if (progress != nullptr)
        progress->stage = 0;
    return outcome;
}

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
    if (destination.type == DestinationType::onedrive)
        return send_to_onedrive(config, destination, item, game, progress, transport, user,
                                pause_between_tries, Paths{}.onedrive_token(destination.id));
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
        Attempt hosted;
        if (fallback->local() || fallback->type == DestinationType::onedrive)
        {
            // A drive, a share or OneDrive keeps it; the post says where it went
            // (OneDrive with a link the chat can open).
            const UploadOutcome kept =
                fallback->type == DestinationType::folder
                    ? copy_to_folder(*fallback, Paths{}, item, game, progress)
                : fallback->type == DestinationType::smb
                    ? copy_to_share(*fallback, item, game, progress)
                    : send_to_onedrive(config, *fallback, item, game, progress, transport, user,
                                       pause_between_tries, Paths{}.onedrive_token(fallback->id),
                                       true);
            hosted.ok = kept.ok;
            hosted.attempts = kept.attempts;
            hosted.error = kept.error;
            hosted.link = fallback->local() ? "Too big to attach here; saved to " +
                                                  fallback->display_name() + " (" + item.file + ")"
                                            : kept.link;
        }
        else
        {
            hosted = send_with_retries(config, *fallback, item, caption, {}, progress, transport,
                                       user, pause_between_tries);
        }
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

    // A screenshot too big to be a Telegram photo, or HDR: a photo copy, then the original.
    if (link.empty() && needs_photo_copy(destination, item))
    {
        UploadOutcome twice = upload_album_with(config, destination, {item}, {game}, progress,
                                                transport, user, pause_between_tries);
        twice.attempts += outcome.attempts;
        return twice;
    }
    // An HDR screenshot elsewhere: what is sent is a JPEG copy anyone can see;
    // Discord gets the original attached too, when both fit its limit.
    MediaItem copy;
    std::string copy_path;
    if (link.empty() && item.jxr())
    {
        copy_path = scratch_path("photo-" + std::to_string(getpid()) + "-" + item.file + ".jpg");
        if (!make_photo_copy(item, copy_path, &copy))
            copy_path.clear(); // unreadable here: the original goes as it is
    }
    Attempt sent;
    if (!copy_path.empty())
    {
        HttpRequest request;
        if (!plan_request(destination, copy, caption, {}, &request, &sent.error))
        {
            ::unlink(copy_path.c_str());
            outcome.error = sent.error;
            return outcome;
        }
        if (destination.type == DestinationType::discord &&
            (limit == 0 || copy.size + item.size <= limit))
        {
            request.parts.push_back(file_part("files[1]", item));
            request.upload_size += item.size;
        }
        sent = send_planned(config, destination.type, request, {}, progress, transport, user,
                            pause_between_tries);
        ::unlink(copy_path.c_str());
    }
    else
    {
        sent = send_with_retries(config, destination, item, caption, link, progress, transport,
                                 user, pause_between_tries);
    }
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

UploadOutcome upload_album(const Config &config, const Destination &destination,
                           const std::vector<MediaItem> &items,
                           const std::vector<std::string> &games, UploadProgress *progress)
{
    int timeout = 900;
    return upload_album_with(config, destination, items, games, progress, curl_transport, &timeout,
                             true);
}

UploadOutcome upload_capture(const Config &config, const Destination &destination,
                             const MediaItem &item, const std::string &game,
                             UploadProgress *progress)
{
    int timeout = 900; // a 1 GB clip over a slow uplink
    return upload_capture_with(config, destination, item, game, progress, curl_transport, &timeout,
                               true);
}

} // namespace pshare
