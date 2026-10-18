// PShare - Where things live and what the configuration says.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/config.hpp"

#include "pshare/core/text.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pshare
{
namespace
{
struct TypeInfo
{
    DestinationType type;
    const char *name;
    const char *label;
};

constexpr TypeInfo kTypes[] = {
    {DestinationType::discord, "discord", "Discord webhook"},
    {DestinationType::telegram, "telegram", "Telegram bot"},
    {DestinationType::catbox, "catbox", "catbox.moe"},
    {DestinationType::litterbox, "litterbox", "Litterbox (temporary)"},
    {DestinationType::http, "http", "Custom HTTP endpoint"},
    {DestinationType::folder, "folder", "USB drive or folder"},
    {DestinationType::smb, "smb", "PC or Mac shared folder"},
    {DestinationType::onedrive, "onedrive", "OneDrive"},
};

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

std::string section_of(const Destination &destination)
{
    return "destination." + destination.id;
}
} // namespace

const char *type_name(DestinationType type)
{
    for (const TypeInfo &info : kTypes)
        if (info.type == type)
            return info.name;
    return "discord";
}

const char *type_label(DestinationType type)
{
    for (const TypeInfo &info : kTypes)
        if (info.type == type)
            return info.label;
    return "Destination";
}

DestinationType parse_type(std::string_view text)
{
    for (const TypeInfo &info : kTypes)
        if (iequals(text, info.name))
            return info.type;
    return DestinationType::count;
}

std::uint64_t Destination::size_limit() const
{
    if (max_mb > 0)
        return static_cast<std::uint64_t>(max_mb) * kMiB;
    switch (type)
    {
    case DestinationType::discord:
        return 20 * kMiB; // servers without boosts (since August 2026)
    case DestinationType::telegram:
        return 50 * kMiB; // the Bot API's upload ceiling
    case DestinationType::catbox:
        return 200 * kMiB;
    case DestinationType::litterbox:
        return 1024 * kMiB;
    default:
        return 0; // the server decides
    }
}

int Destination::service_ceiling_mb() const
{
    switch (type)
    {
    case DestinationType::discord:
        return 100; // boost level 3; level 2 takes 50
    case DestinationType::telegram:
        return 50;
    case DestinationType::catbox:
        return 200;
    case DestinationType::litterbox:
        return 1024;
    default:
        return 0;
    }
}

bool Destination::configured() const
{
    switch (type)
    {
    case DestinationType::discord:
        return starts_with(url, "https://") && url.find("/api/webhooks/") != std::string::npos;
    case DestinationType::telegram:
        return !token.empty() && !chat_id.empty();
    case DestinationType::catbox:
    case DestinationType::litterbox:
        return true;
    case DestinationType::http:
        return starts_with(url, "http://") || starts_with(url, "https://");
    case DestinationType::folder:
        return !path.empty();
    case DestinationType::smb:
    {
        SmbAddress address;
        return parse_smb_url(url, &address, nullptr);
    }
    case DestinationType::onedrive:
        return !client_id.empty() && !token.empty();
    default:
        return false;
    }
}

bool parse_smb_url(std::string_view url, SmbAddress *address, std::string *error)
{
    const auto fail = [&](const char *why)
    {
        if (error)
            *error = why;
        return false;
    };
    std::string_view rest = trim(url);
    if (!starts_with(lower(rest.substr(0, 6)), "smb://"))
        return fail("The address starts with smb://");
    rest.remove_prefix(6);
    SmbAddress out;
    const std::size_t at = rest.find('@');
    if (at != std::string_view::npos && at < rest.find('/'))
    {
        const std::string_view who = rest.substr(0, at);
        const std::size_t colon = who.find(':');
        out.user = std::string(who.substr(0, colon));
        if (colon != std::string_view::npos)
            out.password = std::string(who.substr(colon + 1));
        rest.remove_prefix(at + 1);
    }
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0)
        return fail("Name the share: smb://pc/Share");
    out.host = std::string(rest.substr(0, slash));
    rest.remove_prefix(slash + 1);
    const std::size_t next = rest.find('/');
    out.share = std::string(rest.substr(0, next));
    if (next != std::string_view::npos)
        out.folder = std::string(trim(rest.substr(next + 1)));
    while (!out.folder.empty() && out.folder.back() == '/')
        out.folder.pop_back();
    if (out.share.empty())
        return fail("Name the share: smb://pc/Share");
    if (address)
        *address = std::move(out);
    return true;
}

const Destination *Config::find(std::string_view id) const
{
    for (const Destination &destination : destinations)
        if (iequals(destination.id, id))
            return &destination;
    return nullptr;
}

Destination *Config::find(std::string_view id)
{
    for (Destination &destination : destinations)
        if (iequals(destination.id, id))
            return &destination;
    return nullptr;
}

const Destination *Config::destination_for(std::string_view title_id) const
{
    const auto over = title_destinations.find(upper(title_id));
    if (over != title_destinations.end())
    {
        const Destination *chosen = find(over->second);
        if (chosen != nullptr && chosen->enabled && chosen->configured())
            return chosen;
    }
    const Destination *fallback = find(default_destination);
    if (fallback != nullptr && fallback->enabled && fallback->configured())
        return fallback;
    for (const Destination &destination : destinations)
        if (destination.enabled && destination.configured())
            return &destination;
    return nullptr;
}

bool Config::upload_allowed(std::string_view title_id, bool video) const
{
    const std::map<std::string, bool> &overrides = video ? title_videos : title_screenshots;
    const auto found = overrides.find(upper(title_id));
    if (found != overrides.end())
        return found->second;
    return video ? upload_videos : upload_screenshots;
}

std::string default_config_text()
{
    return R"(; PShare configuration.
; The PShare app edits this file (Settings), and so can you: over the phone
; setup page, or by hand over FTP.
; Lines starting with ; are comments and are kept when the app saves.

[general]
; Send new captures as soon as they are saved, while pshared runs.
auto_upload = true
upload_screenshots = true
upload_videos = true
; Also send captures that existed before pshared first started.
upload_existing = false
; Seconds between looks for new captures, and how long a file must stop
; growing before it is sent (clips are written over several seconds).
poll_seconds = 5
settle_seconds = 4
retries = 3
; A console notification for every upload.
notify = true
keep_logs = false
default_destination = discord
; Destinations that also get every new capture, for example a USB copy:
; also_send = usb

; One section per destination: [destination.<id>]
; type = discord | telegram | catbox | litterbox | http | folder | smb | onedrive
; message placeholders: {type} {game} {title_id} {date} {time} {file} {size} {link}

[destination.discord]
type = discord
name = Discord
; Channel settings > Integrations > Webhooks > Copy Webhook URL
url =
username = PShare
message = {type} from **{game}** - {date} {time}
; Discord takes attachments up to 20 MB on servers without boosts (50 MB at
; boost level 2, 100 MB at level 3). Bigger files go to the fallback and
; Discord gets the link instead.
max_mb = 20
fallback = catbox

[destination.catbox]
type = catbox
name = catbox.moe
; Optional: your catbox user hash, so uploads show up in your account.
token =

[destination.litterbox]
type = litterbox
name = Litterbox
; How long the file stays up: 1h, 12h, 24h or 72h.
litterbox_time = 72h
enabled = false

[destination.telegram]
type = telegram
name = Telegram
; token from @BotFather, chat_id of the chat or channel to post in.
token =
chat_id =
message = {type} from {game} - {date} {time}

[destination.custom]
type = http
name = My server
url =
; The form field that carries the file.
field = file
; header = Authorization: Bearer secret
; form.caption = {type} from {game}
enabled = false

; Hosts that need no account (links last as long as the name says).
[destination.uguu]
type = http
name = Uguu (3 hours)
url = https://uguu.se/upload
field = files[]
max_mb = 128

[destination.tmpfiles]
type = http
name = tmpfiles.org (1 hour)
url = https://tmpfiles.org/api/v1/upload
field = file
max_mb = 100

[destination.tempsh]
type = http
name = temp.sh (3 days)
url = https://temp.sh/upload
field = file
max_mb = 4000

[destination.x0]
type = http
name = x0.at
url = https://x0.at
field = file
max_mb = 1024

[destination.gofile]
type = http
name = Gofile
url = https://upload.gofile.io/uploadfile
field = file

[destination.usb]
type = folder
name = USB drive
; usb: the first USB drive found (/mnt/usb0, /mnt/usb1, /mnt/ext0, /mnt/ext1),
; or an absolute folder. Captures land in <drive>/PShare/<game>/.
path = usb
enabled = false

[destination.pc]
type = smb
name = My PC
; A shared folder on your PC: smb://<pc address>/<share>/<folder>
; (Windows: right-click a folder > Properties > Sharing).
url = smb://192.168.1.10/Captures/PS5
username =
; The account's password.
token =
enabled = false

[destination.mac]
type = smb
name = My Mac
; A shared folder on your Mac: smb://<mac address>/<shared folder>
; (System Settings > General > Sharing > File Sharing: add the folder, then
; Options... > turn on Windows File Sharing for your account).
url = smb://192.168.1.20/Captures
; The Mac account's short name (the name of its home folder).
username =
; The account's password.
token =
enabled = false

[destination.onedrive]
type = onedrive
name = OneDrive
; The Application (client) ID of your app registration at portal.azure.com,
; and a refresh token for it (the README's OneDrive guide shows how).
client_id =
token =
; Captures land in <path>/<game>/ in your OneDrive.
path = PShare
enabled = false

[title_destinations]
; PPSA01234 = telegram

[title_screenshots]
; PPSA01234 = false

[title_videos]
; PPSA01234 = false
)";
}

void load_config(const IniDocument &document, Config *config, std::vector<std::string> *warnings)
{
    Config out;
    const char *g = "general";
    out.auto_upload = document.get_bool(g, "auto_upload", out.auto_upload);
    out.upload_screenshots = document.get_bool(g, "upload_screenshots", out.upload_screenshots);
    out.upload_videos = document.get_bool(g, "upload_videos", out.upload_videos);
    out.upload_existing = document.get_bool(g, "upload_existing", out.upload_existing);
    out.notify = document.get_bool(g, "notify", out.notify);
    out.keep_logs = document.get_bool(g, "keep_logs", out.keep_logs);
    out.poll_seconds = static_cast<int>(document.get_int(g, "poll_seconds", out.poll_seconds));
    out.settle_seconds =
        static_cast<int>(document.get_int(g, "settle_seconds", out.settle_seconds));
    out.retries = static_cast<int>(document.get_int(g, "retries", out.retries));
    out.default_destination = document.get_or(g, "default_destination", out.default_destination);
    for (const std::string &item : split(document.get_or(g, "also_send", ""), ','))
        if (!trim(item).empty())
            out.also_send.emplace_back(trim(item));
    if (out.poll_seconds < 1)
        out.poll_seconds = 1;
    if (out.poll_seconds > 600)
        out.poll_seconds = 600;
    if (out.settle_seconds < 1)
        out.settle_seconds = 1;
    if (out.retries < 1)
        out.retries = 1;
    if (out.retries > 10)
        out.retries = 10;

    for (const std::string &section : document.sections_with_prefix("destination."))
    {
        Destination destination;
        destination.id = section.substr(std::string_view("destination.").size());
        const std::string type = document.get_or(section, "type", destination.id);
        destination.type = parse_type(type);
        if (destination.type == DestinationType::count)
        {
            if (warnings)
                warnings->push_back("[" + section + "] has an unknown type \"" + type + "\"");
            continue;
        }
        destination.name = document.get_or(section, "name", "");
        destination.url = std::string(trim(document.get_or(section, "url", "")));
        destination.token = std::string(trim(document.get_or(section, "token", "")));
        // catbox's own name for it.
        if (destination.token.empty())
            destination.token = std::string(trim(document.get_or(section, "userhash", "")));
        destination.chat_id = std::string(trim(document.get_or(section, "chat_id", "")));
        destination.client_id = std::string(trim(document.get_or(section, "client_id", "")));
        destination.username = document.get_or(section, "username", "");
        destination.message = document.get_or(section, "message", "");
        destination.field = document.get_or(section, "field", "file");
        destination.litterbox_time = document.get_or(section, "litterbox_time", "72h");
        destination.path = std::string(trim(document.get_or(
            section, "path", destination.type == DestinationType::onedrive ? "PShare" : "usb")));
        destination.max_mb = static_cast<int>(document.get_int(section, "max_mb", 0));
        destination.fallback = document.get_or(section, "fallback", "");
        destination.enabled = document.get_bool(section, "enabled", true);
        for (const std::string &key : document.keys(section))
        {
            if (starts_with(lower(key), "header"))
                destination.headers.push_back(document.get_or(section, key, ""));
            else if (starts_with(lower(key), "form."))
                destination.form.emplace_back(key.substr(5), document.get_or(section, key, ""));
        }
        if (out.find(destination.id) != nullptr)
        {
            if (warnings)
                warnings->push_back("[" + section + "] appears twice; the first one is used");
            continue;
        }
        out.destinations.push_back(std::move(destination));
    }

    for (const std::string &key : document.keys("title_destinations"))
        out.title_destinations[upper(key)] = document.get_or("title_destinations", key, "");
    for (const std::string &key : document.keys("title_screenshots"))
        out.title_screenshots[upper(key)] = document.get_bool("title_screenshots", key, true);
    for (const std::string &key : document.keys("title_videos"))
        out.title_videos[upper(key)] = document.get_bool("title_videos", key, true);

    if (out.find(out.default_destination) == nullptr && warnings && !out.destinations.empty())
        warnings->push_back("default_destination \"" + out.default_destination +
                            "\" is not a destination");
    *config = std::move(out);
}

void store_config(const Config &config, IniDocument *document)
{
    const char *g = "general";
    document->set_bool(g, "auto_upload", config.auto_upload);
    document->set_bool(g, "upload_screenshots", config.upload_screenshots);
    document->set_bool(g, "upload_videos", config.upload_videos);
    document->set_bool(g, "upload_existing", config.upload_existing);
    document->set_int(g, "poll_seconds", config.poll_seconds);
    document->set_int(g, "settle_seconds", config.settle_seconds);
    document->set_int(g, "retries", config.retries);
    document->set_bool(g, "notify", config.notify);
    document->set_bool(g, "keep_logs", config.keep_logs);
    document->set(g, "default_destination", config.default_destination);
    {
        std::string also;
        for (const std::string &id : config.also_send)
            also += (also.empty() ? "" : ", ") + id;
        if (!also.empty() || document->get(g, "also_send", nullptr))
            document->set(g, "also_send", also);
    }

    // Destinations that are gone from the config are gone from the file.
    for (const std::string &section : document->sections_with_prefix("destination."))
        if (config.find(section.substr(std::string_view("destination.").size())) == nullptr)
            document->remove_section(section);

    for (const Destination &d : config.destinations)
    {
        const std::string s = section_of(d);
        document->set(s, "type", type_name(d.type));
        if (!d.name.empty() || document->get(s, "name", nullptr))
            document->set(s, "name", d.name);
        const bool url_used =
            d.type != DestinationType::catbox && d.type != DestinationType::folder &&
            d.type != DestinationType::litterbox && d.type != DestinationType::telegram &&
            d.type != DestinationType::onedrive;
        if (url_used || !d.url.empty())
            document->set(s, "url", d.url);
        if (d.type != DestinationType::discord && d.type != DestinationType::http &&
            d.type != DestinationType::folder)
        {
            document->remove_key(s, "userhash");
            document->set(s, "token", d.token);
        }
        if (d.type == DestinationType::telegram)
            document->set(s, "chat_id", d.chat_id);
        if (d.type == DestinationType::discord)
            document->set(s, "username", d.username);
        if (!d.message.empty() || document->get(s, "message", nullptr))
            document->set(s, "message", d.message);
        if (d.type == DestinationType::http)
        {
            document->set(s, "field", d.field);
            for (const std::string &key : document->keys(s))
                if (starts_with(lower(key), "header"))
                    document->remove_key(s, key);
            for (std::size_t i = 0; i < d.headers.size(); ++i)
                document->set(s, i == 0 ? std::string("header") : "header" + std::to_string(i + 1),
                              d.headers[i]);
        }
        if (d.type == DestinationType::litterbox)
            document->set(s, "litterbox_time", d.litterbox_time);
        if (d.type == DestinationType::folder || d.type == DestinationType::onedrive)
            document->set(s, "path", d.path);
        if (d.type == DestinationType::onedrive)
            document->set(s, "client_id", d.client_id);
        if (d.type == DestinationType::smb)
            document->set(s, "username", d.username);
        if (d.max_mb > 0 || document->get(s, "max_mb", nullptr))
            document->set_int(s, "max_mb", d.max_mb);
        if (!d.fallback.empty() || document->get(s, "fallback", nullptr))
            document->set(s, "fallback", d.fallback);
        if (!d.enabled || document->get(s, "enabled", nullptr))
            document->set_bool(s, "enabled", d.enabled);
    }

    for (const auto &[title, destination] : config.title_destinations)
        document->set("title_destinations", title, destination);
    for (const auto &[title, allowed] : config.title_screenshots)
        document->set_bool("title_screenshots", title, allowed);
    for (const auto &[title, allowed] : config.title_videos)
        document->set_bool("title_videos", title, allowed);
}

bool read_text_file(const std::string &path, std::string *text, std::size_t limit)
{
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0)
        return false;
    std::string out;
    char buffer[16384];
    for (;;)
    {
        const ssize_t got = read(descriptor, buffer, sizeof(buffer));
        if (got < 0)
        {
            close(descriptor);
            return false;
        }
        if (got == 0)
            break;
        if (out.size() + static_cast<std::size_t>(got) > limit)
        {
            close(descriptor);
            return false;
        }
        out.append(buffer, static_cast<std::size_t>(got));
    }
    close(descriptor);
    if (text)
        *text = std::move(out);
    return true;
}

bool write_text_file_atomic(const std::string &path, std::string_view text)
{
    const std::string temporary = path + ".tmp";
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (descriptor < 0)
        return false;
    const char *data = text.data();
    std::size_t left = text.size();
    while (left > 0)
    {
        const ssize_t wrote = write(descriptor, data, left);
        if (wrote <= 0)
        {
            close(descriptor);
            unlink(temporary.c_str());
            return false;
        }
        data += wrote;
        left -= static_cast<std::size_t>(wrote);
    }
    if (close(descriptor) != 0)
    {
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0)
    {
        unlink(temporary.c_str());
        return false;
    }
    chmod(path.c_str(), 0666);
    return true;
}

bool ensure_directory(const std::string &path)
{
    if (path.empty())
        return false;
    std::string partial;
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        partial.push_back(path[i]);
        if ((path[i] == '/' && i > 0) || i + 1 == path.size())
        {
            if (mkdir(partial.c_str(), 0777) != 0 && errno != EEXIST)
            {
                struct stat info
                {
                };
                if (stat(partial.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
                    return false;
            }
        }
    }
    return true;
}

bool load_config_file(const std::string &path, IniDocument *document, Config *config,
                      std::vector<std::string> *warnings)
{
    std::string text;
    if (!read_text_file(path, &text))
    {
        text = default_config_text();
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos)
            ensure_directory(path.substr(0, slash));
        if (!write_text_file_atomic(path, text) && warnings)
            warnings->push_back("could not write " + path);
    }
    document->parse(text);
    load_config(*document, config, warnings);
    return true;
}

} // namespace pshare
