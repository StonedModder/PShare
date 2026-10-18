// PShare - Where things live and what the configuration says.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One INI file, /data/pshare/config.ini, read by the app and by the
// background daemon: a [general] section, named destinations, and per-title
// overrides keyed by the title ID that appears in each capture's folder.

#pragma once

#include "pshare/core/ini.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pshare
{

// Folders and ports. The console defaults are below; the PC preview and the
// tests point the roots somewhere else.
struct Paths
{
    std::string data_root = "/data/pshare";
    std::string photo_root = "/user/av_contents/photo";
    std::string video_root = "/user/av_contents/video";
    std::string appmeta_root = "/user/appmeta";
    std::string usb_roots[4] = {"/mnt/usb0", "/mnt/usb1", "/mnt/ext0", "/mnt/ext1"};

    std::string config() const
    {
        return data_root + "/config.ini";
    }
    std::string history() const
    {
        return data_root + "/history.tsv";
    }
    std::string daemon_log() const
    {
        return data_root + "/pshared.log";
    }
    std::string daemon_seen() const
    {
        return data_root + "/pshared-seen.txt";
    }
    std::string thumbs() const
    {
        return data_root + "/thumbs";
    }
    // The newest OneDrive refresh token for a destination (upload_onedrive.cpp).
    std::string onedrive_token(const std::string &id) const
    {
        return data_root + "/onedrive-" + id + ".token";
    }
};

constexpr int kControlPort = 9077; // pshared listens on 127.0.0.1 only
constexpr int kLoaderPort = 9021;  // the jailbreak's ELF loader
constexpr int kSetupPort = 9078;   // pshared's phone setup page, on the local network
constexpr const char *kVersion = "1.0.0";

enum class DestinationType : std::uint8_t
{
    discord,
    telegram,
    catbox,
    litterbox,
    http,     // any multipart/form-data endpoint
    folder,   // a copy on a drive: a USB stick or the extended storage
    smb,      // a copy in a shared folder on a PC or Mac (Windows share, macOS, Samba)
    onedrive, // a folder in OneDrive, through Microsoft Graph
    count,
};

const char *type_name(DestinationType type);  // "discord"
const char *type_label(DestinationType type); // "Discord webhook"
DestinationType parse_type(std::string_view text);

struct Destination
{
    std::string id; // the part after "destination." in the section name
    DestinationType type = DestinationType::discord;
    std::string name;                   // shown in the app; the id when empty
    std::string url;                    // webhook, server, endpoint, or smb://host/share/folder
    std::string path = "usb";           // folder: "usb" or an absolute path; onedrive: its folder
    std::string token;                  // bot token, catbox userhash, smb password, refresh token
    std::string client_id;              // onedrive: the app registration's client ID
    std::string chat_id;                // telegram
    std::string username;               // discord: the poster's name; smb: the account
    std::string message;                // caption template, see expand_caption()
    std::string field = "file";         // http: the form field of the file
    std::string litterbox_time = "72h"; // litterbox: 1h, 12h, 24h or 72h
    std::vector<std::string> headers;   // http: "Name: value"
    std::vector<std::pair<std::string, std::string>> form; // http: extra fields
    int max_mb = 0;       // refuse (or hand to `fallback`) above this; 0: the service's limit
    std::string fallback; // destination id for files over max_mb (discord -> catbox)
    bool enabled = true;

    std::string display_name() const
    {
        return name.empty() ? id : name;
    }
    // The service's own ceiling in bytes, or max_mb when set.
    std::uint64_t size_limit() const;
    // True when the fields the type needs are filled in.
    bool configured() const;
    // Copies to a drive or a share instead of going over HTTP.
    bool local() const
    {
        return type == DestinationType::folder || type == DestinationType::smb;
    }
};

// smb://[user[:password]@]host/share[/folder]
struct SmbAddress
{
    std::string user;
    std::string password;
    std::string host;
    std::string share;
    std::string folder; // inside the share, no leading or trailing slash
};
bool parse_smb_url(std::string_view url, SmbAddress *address, std::string *error);

struct Config
{
    bool auto_upload = true;
    bool upload_screenshots = true;
    bool upload_videos = true;
    bool upload_existing = false;
    bool notify = true;
    bool keep_logs = false;
    int poll_seconds = 5;
    int settle_seconds = 4;
    int retries = 3;
    std::string default_destination = "discord";
    // Destinations that also get every automatic upload ("also_send = usb, pc").
    std::vector<std::string> also_send;

    std::vector<Destination> destinations;
    std::map<std::string, std::string> title_destinations; // PPSA01234 -> destination id
    std::map<std::string, bool> title_screenshots;
    std::map<std::string, bool> title_videos;

    const Destination *find(std::string_view id) const;
    Destination *find(std::string_view id);
    // The destination for a capture of `title_id`: its override, else the
    // default, else the first configured one. Null when nothing is set up.
    const Destination *destination_for(std::string_view title_id) const;
    bool upload_allowed(std::string_view title_id, bool video) const;
};

// The commented file written on first start.
std::string default_config_text();

// Reads every field PShare knows; unknown keys are ignored (and kept by the
// document). Problems that do not stop the load go to `warnings`.
void load_config(const IniDocument &document, Config *config, std::vector<std::string> *warnings);
// Writes the fields back into the document, keeping comments and unknown keys.
void store_config(const Config &config, IniDocument *document);

// File helpers (plain POSIX): whole-file read, atomic replace, mkdir -p.
bool read_text_file(const std::string &path, std::string *text, std::size_t limit = 8u << 20);
bool write_text_file_atomic(const std::string &path, std::string_view text);
bool ensure_directory(const std::string &path);

// Loads `path`; when it is missing, writes the default file there first.
bool load_config_file(const std::string &path, IniDocument *document, Config *config,
                      std::vector<std::string> *warnings);

} // namespace pshare
