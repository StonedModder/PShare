// PShare - Host unit tests for the shared core (no console, no network, no GL).
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small self-contained runner: every TEST registers itself, CHECK records a
// failure with its line and carries on. Built with AddressSanitizer and
// UndefinedBehaviorSanitizer by tools/build-tests.sh.

#include "app/thumbs.hpp"
#include "pshare/core/config.hpp"
#include "pshare/core/history.hpp"
#include "pshare/core/ini.hpp"
#include "pshare/core/local_copy.hpp"
#include "pshare/core/media.hpp"
#include "pshare/core/net.hpp"
#include "pshare/core/picture.hpp"
#include "pshare/core/setup_web.hpp"
#include "pshare/core/text.hpp"
#include "pshare/core/upload.hpp"
#include "pshare/core/watcher.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{
struct Case
{
    const char *name;
    std::function<void()> body;
};
std::vector<Case> &cases()
{
    static std::vector<Case> all;
    return all;
}
int g_failures = 0;
struct Register
{
    Register(const char *name, std::function<void()> body)
    {
        cases().push_back({name, std::move(body)});
    }
};
#define TEST(name)                                                                                 \
    void test_##name();                                                                            \
    const Register register_##name(#name, test_##name);                                            \
    void test_##name()
#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition);          \
        }                                                                                          \
    } while (false)
#define CHECK_EQ(a, b) CHECK((a) == (b))

using namespace pshare;

std::string scratch()
{
    static std::string root;
    if (root.empty())
    {
        char pattern[] = "/tmp/pshare-tests-XXXXXX";
        const char *made = mkdtemp(pattern);
        root = made != nullptr ? made : "/tmp/pshare-tests";
    }
    return root;
}

bool file_exists(const std::string &path)
{
    struct stat info
    {
    };
    return stat(path.c_str(), &info) == 0;
}

void write_file(const std::string &path, const std::string &text)
{
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return;
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

std::uint64_t file_size_of(const std::string &path)
{
    struct stat info
    {
    };
    return stat(path.c_str(), &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
}

MediaItem sample_item(const std::string &path, std::uint64_t size)
{
    MediaItem item;
    item.path = path;
    item.file = path.substr(path.rfind('/') + 1);
    capture_kind(item.file, &item.kind);
    item.size = size;
    item.title_id = title_id_from_path(path);
    parse_capture_time(item.file, &item.captured);
    return item;
}

// ---- text ------------------------------------------------------------------

TEST(text_helpers)
{
    CHECK_EQ(trim("  a b \n"), "a b");
    CHECK(iequals("Discord", "DISCORD"));
    CHECK(parse_bool("Yes", false));
    CHECK(!parse_bool("off", true));
    CHECK(parse_bool("maybe", true));
    CHECK_EQ(parse_int("42", 0), 42);
    CHECK_EQ(parse_int("4x", 7), 7);
    CHECK_EQ(url_encode("a b/c~"), "a%20b%2Fc~");
    CHECK_EQ(json_escape("say \"hi\"\n"), "say \\\"hi\\\"\\n");
    CHECK_EQ(human_size(12), "12 B");
    CHECK_EQ(human_size(1536 * 1024), "1.5 MB");
    std::string value;
    CHECK(json_find_string(R"({"a":{"url":"https:\/\/x.y\/z"}})", "url", &value));
    CHECK_EQ(value, "https://x.y/z");
    bool flag = false;
    CHECK(json_find_bool(R"({"ok": true})", "ok", &flag) && flag);
    CHECK(find_url("see https://files.catbox.moe/abc.png now", &value));
    CHECK_EQ(value, "https://files.catbox.moe/abc.png");
    const Placeholder values[] = {{"game", "Nebula"}, {"type", "Clip"}};
    CHECK_EQ(expand("{type} of {game} {nope}", values, 2), "Clip of Nebula {nope}");
}

TEST(civil_time)
{
    const LocalTime epoch = civil_time(0);
    CHECK(epoch.year == 1970 && epoch.month == 1 && epoch.day == 1 && epoch.yday == 0);
    // 2026-10-09 18:35:07 UTC
    const LocalTime t = civil_time(1791570907);
    CHECK(t.year == 2026 && t.month == 10 && t.day == 9);
    CHECK(t.hour == 18 && t.minute == 35 && t.second == 7);
    CHECK_EQ(t.yday, 281);
    const LocalTime leap = civil_time(951782400); // 2000-02-29
    CHECK(leap.year == 2000 && leap.month == 2 && leap.day == 29);
    const LocalTime before = civil_time(-1);
    CHECK(before.year == 1969 && before.month == 12 && before.day == 31 && before.hour == 23);
}

// ---- ini and configuration ---------------------------------------------------

TEST(ini_round_trip_keeps_comments)
{
    IniDocument document;
    CHECK(document.parse("; top\n[general]\nauto_upload = true ; keep\n\n[destination.a]\n"
                         "type = discord\n; note\nurl = x\n"));
    CHECK(document.get_bool("general", "auto_upload", false));
    document.set("destination.a", "url", "https://y");
    document.set("new", "key", "v");
    const std::string text = document.write();
    CHECK(text.find("; top") != std::string::npos);
    CHECK(text.find("; note") != std::string::npos);
    CHECK(text.find("url = https://y") != std::string::npos);
    CHECK(text.find("[new]") != std::string::npos);
    CHECK_EQ(document.sections_with_prefix("destination.").size(), 1u);
}

TEST(default_config_loads)
{
    IniDocument document;
    CHECK(document.parse(default_config_text()));
    Config config;
    std::vector<std::string> warnings;
    load_config(document, &config, &warnings);
    CHECK(warnings.empty());
    CHECK(config.find("discord") != nullptr);
    CHECK(config.find("catbox") != nullptr);
    CHECK_EQ(config.default_destination, "discord");
    CHECK_EQ(config.find("discord")->fallback, "catbox");
    CHECK_EQ(config.find("discord")->max_mb, 20);
    CHECK(config.find("mac") != nullptr && config.find("mac")->type == DestinationType::smb);
    // Discord has no webhook yet: the first configured one (catbox) is used.
    const Destination *chosen = config.destination_for("PPSA01234");
    CHECK(chosen != nullptr && chosen->id == "catbox");
}

TEST(config_store_round_trip)
{
    IniDocument document;
    document.parse(default_config_text());
    Config config;
    load_config(document, &config, nullptr);
    config.find("discord")->url = "https://discord.com/api/webhooks/1/abc";
    config.find("custom")->headers = {"Authorization: Bearer s", "X-Two: 2"};
    config.title_destinations["PPSA01234"] = "telegram";
    config.retries = 5;
    store_config(config, &document);
    IniDocument again;
    again.parse(document.write());
    Config loaded;
    load_config(again, &loaded, nullptr);
    CHECK_EQ(loaded.find("discord")->url, "https://discord.com/api/webhooks/1/abc");
    CHECK_EQ(loaded.find("custom")->headers.size(), 2u);
    CHECK_EQ(loaded.title_destinations["PPSA01234"], "telegram");
    CHECK_EQ(loaded.retries, 5);
    CHECK(document.write().find("Channel settings > Integrations") != std::string::npos);
}

TEST(per_title_rules)
{
    IniDocument document;
    document.parse(default_config_text());
    Config config;
    load_config(document, &config, nullptr);
    config.find("discord")->url = "https://discord.com/api/webhooks/1/abc";
    config.find("telegram")->token = "t";
    config.find("telegram")->chat_id = "1";
    config.title_destinations["PPSA01234"] = "telegram";
    config.title_videos["PPSA01234"] = false;
    CHECK_EQ(config.destination_for("PPSA01234")->id, "telegram");
    CHECK_EQ(config.destination_for("PPSA99999")->id, "discord");
    CHECK(!config.upload_allowed("PPSA01234", true));
    CHECK(config.upload_allowed("PPSA01234", false));
}

// ---- media -------------------------------------------------------------------

TEST(capture_names)
{
    MediaKind kind;
    CHECK(capture_kind("a.JPG", &kind) && kind == MediaKind::screenshot);
    CHECK(capture_kind("b.webm", &kind) && kind == MediaKind::video);
    CHECK(!capture_kind("c.txt", &kind));
    CaptureTime time;
    CHECK(parse_capture_time("20261009_183501_00000001.jpg", &time));
    CHECK(time.year == 2026 && time.month == 10 && time.day == 9 && time.hour == 18);
    CHECK_EQ(time.date(), "2026-10-09");
    CHECK_EQ(time.time(), "18:35");
    CHECK(!parse_capture_time("IMG_0001.jpg", &time));
    CHECK(looks_like_title_id("PPSA01234"));
    CHECK(!looks_like_title_id("ppsa01234"));
    CHECK_EQ(title_id_from_path("/user/av_contents/photo/PS5/PPSA01234/f/x.jpg"), "PPSA01234");
    CHECK_EQ(title_id_from_path("/user/av_contents/photo/x.jpg"), "");
}

TEST(title_names)
{
    std::string name;
    CHECK(TitleNames::parse_param_json(
        R"({"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Astro"}}})",
        &name));
    CHECK_EQ(name, "Astro");
    const std::string root = scratch() + "/appmeta";
    ensure_directory(root + "/PPSA01234");
    write_file(
        root + "/PPSA01234/param.json",
        R"({"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Astro"}}})");
    TitleNames names(root);
    CHECK_EQ(names.display("PPSA01234"), "Astro");
    CHECK_EQ(names.display("PPSA99999"), "PPSA99999");
    CHECK_EQ(names.display(""), "Home screen");

    // A game with no appmeta (a PS4 title, or one since deleted): the name
    // comes from the .dat the console keeps beside each capture.
    const auto dat = [](const std::string &text)
    {
        std::string bytes(1024, '\0');
        bytes[0] = 1;
        bytes.replace(0x44, text.size(), text);
        return bytes;
    };
    CHECK(TitleNames::parse_capture_dat(dat("Digimon World: Next Order_20260415042317"), &name));
    CHECK_EQ(name, "Digimon World: Next Order");
    CHECK(!TitleNames::parse_capture_dat(dat("20261009204839"), &name)); // the home screen
    CHECK(!TitleNames::parse_capture_dat("short", &name));
    const std::string video = scratch() + "/names/video";
    ensure_directory(video + "/NPXS40087/CUSA05392/306");
    write_file(video + "/NPXS40087/CUSA05392/306/20260415_042317_00321130.mp4", "x");
    write_file(video + "/NPXS40087/CUSA05392/306/20260415_042317_00321130.dat",
               dat("Digimon World: Next Order_20260415042317"));
    TitleNames with_captures(root, {scratch() + "/names/photo", video});
    CHECK_EQ(with_captures.display("CUSA05392"), "Digimon World: Next Order");
    CHECK_EQ(with_captures.display("PPSA01234"), "Astro");
    CHECK_EQ(with_captures.display("CUSA00001"), "CUSA00001");
}

TEST(blank_captures_are_found_and_deleted)
{
    const std::string root = scratch() + "/blank/video";
    const std::string folder = root + "/NPXS40087/CUSA05392/306";
    ensure_directory(folder);
    const std::string zeros = folder + "/20260415_042317_00321130.mp4";
    write_file(zeros, std::string(300000, '\0'));
    write_file(folder + "/20260415_042317_00321130.dat", "x");
    std::string half(300000, '\0');
    half[150000] = 'v'; // data only in the middle: still a real file
    write_file(folder + "/20260415_042400_00000002.mp4", half);
    CHECK(capture_reads_blank(zeros));
    CHECK(!capture_reads_blank(folder + "/20260415_042400_00000002.mp4"));
    CHECK(!capture_reads_blank(folder + "/missing.mp4"));
    std::string error;
    CHECK(!delete_capture("/etc/passwd", scratch() + "/blank/photo", root, &error));
    CHECK(!delete_capture(root + "/../x.mp4", scratch() + "/blank/photo", root, &error));
    CHECK(delete_capture(zeros, scratch() + "/blank/photo", root, &error));
    CHECK(!file_exists(zeros));
    CHECK(!file_exists(folder + "/20260415_042317_00321130.dat"));
    CHECK(file_exists(folder + "/20260415_042400_00000002.mp4"));
}

TEST(scan_finds_captures_newest_first)
{
    Paths paths;
    paths.photo_root = scratch() + "/scan/photo";
    paths.video_root = scratch() + "/scan/video";
    ensure_directory(paths.photo_root + "/PS5/PPSA01234");
    ensure_directory(paths.video_root + "/PS5/PPSA01234");
    write_file(paths.photo_root + "/PS5/PPSA01234/20260101_101010_00000001.jpg", "jpg");
    write_file(paths.photo_root + "/PS5/PPSA01234/20260301_101010_00000002.png", "png");
    write_file(paths.video_root + "/PS5/PPSA01234/20260201_101010_00000003.webm", "webm");
    write_file(paths.photo_root + "/PS5/PPSA01234/notes.txt", "no");
    const ScanResult scan = scan_media(paths, ScanOptions{});
    CHECK(scan.photo_root && scan.video_root);
    CHECK_EQ(scan.items.size(), 3u);
    if (scan.items.size() == 3)
    {
        CHECK_EQ(scan.items[0].file, "20260301_101010_00000002.png");
        CHECK(scan.items[1].video());
        CHECK_EQ(scan.items[2].title_id, "PPSA01234");
    }
    ScanOptions only_clips;
    only_clips.screenshots = false;
    CHECK_EQ(scan_media(paths, only_clips).items.size(), 1u);
}

// ---- the watcher ---------------------------------------------------------------

TEST(watcher_waits_for_files_to_settle)
{
    Paths paths;
    paths.data_root = scratch() + "/watch/data";
    paths.photo_root = scratch() + "/watch/photo";
    paths.video_root = scratch() + "/watch/video";
    ensure_directory(paths.photo_root);
    write_file(paths.photo_root + "/20250101_000000_old.jpg", "old");
    const std::int64_t start = static_cast<std::int64_t>(std::time(nullptr)) + 10;
    Config config;
    config.settle_seconds = 2;
    Watcher watcher(paths);
    watcher.configure(config);
    watcher.load(start);
    // The old file is the back catalogue.
    CHECK(watcher.poll(start).empty());
    CHECK(watcher.seen(paths.photo_root + "/20250101_000000_old.jpg"));
    const std::string fresh = paths.photo_root + "/20260101_000000_new.jpg";
    write_file(fresh, "new");
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = static_cast<time_t>(start + 1);
    times[0].tv_nsec = times[1].tv_nsec = 0;
    utimensat(0, fresh.c_str(), times, 0);
    CHECK(watcher.poll(start + 1).empty()); // noticed
    CHECK_EQ(watcher.pending(), 1u);
    CHECK(watcher.poll(start + 2).empty()); // still settling
    const std::vector<MediaItem> ready = watcher.poll(start + 3);
    CHECK_EQ(ready.size(), 1u);
    CHECK(watcher.poll(start + 4).empty()); // handed out once
    watcher.mark_seen(fresh);
    CHECK(watcher.save());
    // A restart remembers it.
    Watcher again(paths);
    again.configure(config);
    again.load(start + 10);
    CHECK(again.seen(fresh));
    CHECK_EQ(again.since(), start);
}

TEST(watcher_seen_list_text)
{
    Paths paths;
    Watcher watcher(paths);
    watcher.parse("#since 100\n/a.jpg\n\n/b.png\n", 500);
    CHECK_EQ(watcher.since(), 100);
    CHECK(watcher.seen("/a.jpg") && watcher.seen("/b.png"));
    CHECK_EQ(watcher.serialize(), "#since 100\n/a.jpg\n/b.png\n");
}

// ---- uploads -------------------------------------------------------------------

Config ready_config()
{
    IniDocument document;
    document.parse(default_config_text());
    Config config;
    load_config(document, &config, nullptr);
    config.find("discord")->url = "https://discord.com/api/webhooks/1/abc";
    config.find("telegram")->token = "123:abc";
    config.find("telegram")->chat_id = "-100";
    config.find("custom")->url = "https://example.net/up";
    config.find("custom")->form = {{"caption", "{type} of {game}"}};
    return config;
}

const FormPart *part(const HttpRequest &request, const std::string &name)
{
    for (const FormPart &each : request.parts)
        if (each.name == name)
            return &each;
    return nullptr;
}

TEST(plans_per_destination)
{
    const Config config = ready_config();
    const MediaItem shot = sample_item("/p/PS5/PPSA01234/20261009_183501_00000001.jpg", 1000);
    const MediaItem clip = sample_item("/v/PS5/PPSA01234/20261009_183501_00000002.webm", 1000);
    const CaptionInfo info = caption_info(shot, "Astro");
    HttpRequest request;
    std::string error;

    CHECK(plan_request(*config.find("discord"), shot, info, {}, &request, &error));
    CHECK_EQ(request.url, "https://discord.com/api/webhooks/1/abc?wait=true");
    CHECK(part(request, "files[0]") != nullptr && part(request, "files[0]")->is_file());
    CHECK(part(request, "payload_json")->value.find("**Astro**") != std::string::npos);
    CHECK(part(request, "payload_json")->value.find("\"username\":\"PShare\"") !=
          std::string::npos);

    CHECK(plan_request(*config.find("telegram"), shot, info, {}, &request, &error));
    CHECK_EQ(request.url, "https://api.telegram.org/bot123:abc/sendPhoto");
    CHECK(part(request, "photo") != nullptr);
    CHECK(plan_request(*config.find("telegram"), clip, caption_info(clip, "Astro"), {}, &request,
                       &error));
    CHECK_EQ(request.url, "https://api.telegram.org/bot123:abc/sendDocument");

    CHECK(plan_request(*config.find("catbox"), shot, info, {}, &request, &error));
    CHECK(part(request, "reqtype")->value == "fileupload");
    CHECK(part(request, "fileToUpload") != nullptr);

    CHECK(plan_request(*config.find("custom"), shot, info, {}, &request, &error));
    CHECK_EQ(part(request, "caption")->value, "Screenshot of Astro");
    CHECK(part(request, "file") != nullptr);

    Destination empty;
    empty.type = DestinationType::discord;
    CHECK(!plan_request(empty, shot, info, {}, &request, &error));
    CHECK(!error.empty());
}

TEST(reads_answers)
{
    ResponseVerdict v = read_response(DestinationType::discord, 200,
                                      R"({"attachments":[{"url":"https://cdn/x.jpg"}]})");
    CHECK(v.ok && v.link == "https://cdn/x.jpg");
    v = read_response(DestinationType::discord, 429,
                      R"({"message":"You are being rate limited."})");
    CHECK(!v.ok && v.retry && v.error == "You are being rate limited.");
    v = read_response(DestinationType::discord, 413, "");
    CHECK(!v.ok && !v.retry);
    v = read_response(DestinationType::telegram, 200, R"({"ok":true})");
    CHECK(v.ok);
    v = read_response(DestinationType::telegram, 401,
                      R"({"ok":false,"description":"Unauthorized"})");
    CHECK(!v.ok && v.error == "Unauthorized");
    v = read_response(DestinationType::catbox, 200, "https://files.catbox.moe/abc.png\n");
    CHECK(v.ok && v.link == "https://files.catbox.moe/abc.png");
    v = read_response(DestinationType::http, 200, R"({"link":"https://h/1"})");
    CHECK(v.ok && v.link == "https://h/1");
    v = read_response(DestinationType::http, 503, "busy");
    CHECK(!v.ok && v.retry);
    // Rate limits say how long to wait: Telegram in whole seconds, Discord in fractions.
    v = read_response(DestinationType::telegram, 429,
                      R"({"ok":false,"error_code":429,"description":"Too Many Requests: retry )"
                      R"(after 35","parameters":{"retry_after":35}})");
    CHECK(!v.ok && v.retry && v.retry_after == 36);
    v = read_response(
        DestinationType::discord, 429,
        R"({"message":"You are being rate limited.","retry_after":1.5,"global":false})");
    CHECK(v.retry && v.retry_after == 2);
    v = read_response(DestinationType::discord, 500, R"({"retry_after":9})");
    CHECK(v.retry && v.retry_after == 0);
    double number = 0.0;
    CHECK(json_find_number(R"({"a":"x","n":12.25})", "n", &number) && number == 12.25);
    CHECK(!json_find_number(R"({"n":"12"})", "n", &number));
}

struct FakeServer
{
    std::vector<std::string> urls;
    std::vector<std::string> captions; // Telegram's

    int failures_left = 0;
};

HttpResult fake_transport(const HttpRequest &request, UploadProgress *, void *user)
{
    FakeServer *server = static_cast<FakeServer *>(user);
    server->urls.push_back(request.url);
    for (const FormPart &each : request.parts)
        if (each.name == "caption")
            server->captions.push_back(each.value);
    HttpResult result;
    result.transport_ok = true;
    if (server->failures_left > 0)
    {
        --server->failures_left;
        result.status = 503;
        return result;
    }
    result.status = 200;
    if (request.url.find("catbox") != std::string::npos)
        result.body = "https://files.catbox.moe/big.webm";
    else if (request.url.find("api.telegram.org") != std::string::npos)
        result.body = R"({"ok":true,"result":[]})";
    else
        result.body = R"({"attachments":[]})";
    return result;
}

TEST(telegram_albums)
{
    Config config = ready_config();
    const Destination &telegram = *config.find("telegram");
    std::vector<MediaItem> shots;
    for (int i = 1; i <= 3; ++i)
        shots.push_back(sample_item("/p/PS5/PPSA01234/20261009_18350" + std::to_string(i) +
                                        "_0000000" + std::to_string(i) + ".jpg",
                                    1000));
    const MediaItem clip = sample_item("/v/PS5/PPSA01234/20261009_183501_00000002.webm", 1000);
    CHECK(album_kind(telegram, shots[0]) == AlbumKind::photo);
    CHECK(album_kind(telegram, clip) == AlbumKind::file);
    const MediaItem big_shot = sample_item(shots[0].path, 11ull << 20); // a 4K PNG, say
    CHECK(album_kind(telegram, big_shot) == AlbumKind::photo); // as a photo copy + the file
    CHECK(needs_photo_copy(telegram, big_shot) && !needs_photo_copy(telegram, shots[0]));
    CHECK(album_kind(telegram, sample_item(clip.path, 60ull << 20)) == AlbumKind::none);
    CHECK(album_kind(*config.find("discord"), shots[0]) == AlbumKind::none);

    std::vector<CaptionInfo> captions;
    for (const MediaItem &shot : shots)
        captions.push_back(caption_info(shot, "Astro"));
    HttpRequest request;
    std::string error;
    CHECK(plan_album(telegram, AlbumKind::photo, shots, captions, &request, &error));
    CHECK_EQ(request.url, "https://api.telegram.org/bot123:abc/sendMediaGroup");
    CHECK_EQ(request.upload_size, 3000u);
    const FormPart *media = part(request, "media");
    CHECK(media != nullptr);
    if (media != nullptr)
    {
        CHECK(media->value.find(R"({"type":"photo","media":"attach://p0","caption":"Screenshot )"
                                R"(from Astro)") != std::string::npos);
        CHECK(media->value.find("attach://p2") != std::string::npos);
    }
    CHECK(part(request, "p2") != nullptr && part(request, "p2")->is_file());
    CHECK(part(request, "chat_id")->value == "-100");
    CHECK(!plan_album(telegram, AlbumKind::photo, {shots[0]}, {captions[0]}, &request, &error));
    CHECK(!plan_album(telegram, AlbumKind::photo, {shots[0], clip}, {captions[0], captions[0]},
                      &request, &error));
    CHECK(!plan_album(telegram, AlbumKind::photo, {big_shot, shots[0]}, {captions[0], captions[0]},
                      &request, &error)); // too big to be a photo
    // Files go in albums of their own: full-size screenshots and clips.
    CHECK(plan_album(telegram, AlbumKind::file, {big_shot, clip}, {captions[0], captions[0]},
                     &request, &error));
    CHECK(part(request, "media") != nullptr &&
          part(request, "media")->value.find(R"("type":"document")") != std::string::npos);
    const MediaItem huge = sample_item(clip.path, 45ull << 20);
    CHECK(!plan_album(telegram, AlbumKind::file, {huge, big_shot}, {captions[0], captions[0]},
                      &request, &error)); // over 50 MB together

    FakeServer server;
    const UploadOutcome outcome = upload_album_with(config, telegram, shots, {"Astro"}, nullptr,
                                                    fake_transport, &server, false);
    CHECK(outcome.ok);
    CHECK_EQ(server.urls.size(), 1u);
}

TEST(big_screenshots_go_to_telegram_as_a_photo_and_a_file)
{
    // A 4000 x 2500 screenshot of noise: well over Telegram's 10 MB for a photo.
    Picture noise;
    noise.width = 4000;
    noise.height = 2500;
    noise.rgba.resize(static_cast<std::size_t>(noise.width) * noise.height * 4);
    std::uint32_t seed = 7;
    for (std::uint8_t &byte : noise.rgba)
        byte = static_cast<std::uint8_t>((seed = seed * 1664525u + 1013904223u) >> 24);
    const std::string root = scratch() + "/telegram/PS5/PPSA01234";
    std::system(("mkdir -p '" + root + "'").c_str());
    const std::string path = root + "/20261009_183501_00000001.jpg";
    const std::string jpeg = encode_jpeg(noise, 100);
    CHECK(jpeg.size() > (10u << 20));
    CHECK(write_text_file_atomic(path, jpeg));
    const MediaItem shot = sample_item(path, jpeg.size());

    MediaItem photo;
    CHECK(make_telegram_photo(shot, scratch() + "/telegram/copy.jpg", &photo));
    CHECK(photo.size <= (10u << 20));
    CHECK_EQ(photo.file, "20261009_183501_00000001.jpg");
    Picture back;
    std::string bytes;
    CHECK(read_text_file(photo.path, &bytes, 64u << 20));
    CHECK(decode_picture_bytes(bytes, 9999, 9999, &back));
    CHECK_EQ(back.width, 2560);
    CHECK_EQ(back.height, 1600);

    // Sent: the photo copy, then the original as a file, captioned "Full size".
    const Config config = ready_config();
    FakeServer server;
    const UploadOutcome outcome = upload_capture_with(
        config, *config.find("telegram"), shot, "Astro", nullptr, fake_transport, &server, false);
    CHECK(outcome.ok);
    CHECK_EQ(server.urls.size(), 2u);
    CHECK_EQ(server.captions.size(), 2u);
    if (server.urls.size() == 2 && server.captions.size() == 2)
    {
        CHECK(server.urls[0].find("/sendPhoto") != std::string::npos);
        CHECK(server.captions[0].find("Screenshot from Astro") != std::string::npos);
        CHECK(server.urls[1].find("/sendDocument") != std::string::npos);
        CHECK(server.captions[1] == "Full size \xC2\xB7 20261009_183501_00000001.jpg");
    }

    // In a Send all: one photo album of three (two of them copies), then the
    // two originals as files: one message each here, as together they pass 50 MB.
    const std::string second = root + "/20261009_183502_00000002.jpg";
    const std::string small = root + "/20261009_183503_00000003.jpg";
    CHECK(write_text_file_atomic(second, jpeg));
    CHECK(write_text_file_atomic(small, encode_jpeg(back, 80)));
    server = FakeServer{};
    std::vector<MediaItem> batch = {shot, sample_item(second, jpeg.size()), sample_item(small, 0)};
    batch[2].size = file_size_of(small);
    const UploadOutcome album =
        upload_album_with(config, *config.find("telegram"), batch, {"Astro"}, nullptr,
                          fake_transport, &server, false);
    CHECK(album.ok);
    CHECK_EQ(server.urls.size(), 3u);
    if (server.urls.size() == 3)
    {
        CHECK(server.urls[0].find("/sendMediaGroup") != std::string::npos);
        CHECK(server.urls[1].find("/sendDocument") != std::string::npos);
        CHECK(server.urls[2].find("/sendDocument") != std::string::npos);
    }
}

TEST(big_files_go_through_the_fallback)
{
    const Config config = ready_config();
    const MediaItem clip =
        sample_item("/v/PS5/PPSA01234/20261009_183501_00000002.webm", 40ull << 20);
    FakeServer server;
    UploadProgress progress;
    const UploadOutcome outcome = upload_capture_with(
        config, *config.find("discord"), clip, "Astro", &progress, fake_transport, &server, false);
    CHECK(outcome.ok);
    CHECK_EQ(outcome.link, "https://files.catbox.moe/big.webm");
    CHECK_EQ(server.urls.size(), 2u);
    if (server.urls.size() == 2)
    {
        CHECK(server.urls[0].find("catbox") != std::string::npos);
        CHECK(server.urls[1].find("discord") != std::string::npos);
    }
}

TEST(big_files_can_go_to_a_drive)
{
    // Discord's fallback is a folder: the clip is copied there, and the post
    // says where instead of carrying a link.
    Config config = ready_config();
    const std::string root = scratch() + "/fallback";
    std::system(("mkdir -p '" + root + "/video/PS5/PPSA01234' '" + root + "/drive'").c_str());
    const std::string file = root + "/video/PS5/PPSA01234/20261009_183600_00000002.webm";
    write_file(file, std::string(4000, 'v'));
    MediaItem clip = sample_item(file, 30ull << 20); // too big for Discord, as far as it knows
    Destination &usb = *config.find("usb");
    usb.enabled = true;
    usb.path = root + "/drive";
    config.find("discord")->fallback = "usb";
    FakeServer server;
    const UploadOutcome outcome = upload_capture_with(
        config, *config.find("discord"), clip, "Astro", nullptr, fake_transport, &server, false);
    CHECK(outcome.ok);
    CHECK(file_exists(root + "/drive/PShare/Astro/20261009_183600_00000002.webm"));
    CHECK_EQ(server.urls.size(), 1u); // only Discord, with the note
    CHECK(outcome.link.find("saved to USB drive") != std::string::npos);
}

TEST(retries_transient_failures)
{
    Config config = ready_config();
    config.retries = 3;
    const MediaItem shot = sample_item("/p/PS5/PPSA01234/20261009_183501_00000001.jpg", 1000);
    FakeServer server;
    server.failures_left = 2;
    const UploadOutcome outcome = upload_capture_with(config, *config.find("custom"), shot, "Astro",
                                                      nullptr, fake_transport, &server, false);
    CHECK(outcome.ok);
    CHECK_EQ(outcome.attempts, 3);
    server = FakeServer{};
    server.failures_left = 5;
    const UploadOutcome failed = upload_capture_with(config, *config.find("custom"), shot, "Astro",
                                                     nullptr, fake_transport, &server, false);
    CHECK(!failed.ok);
    CHECK_EQ(failed.attempts, 3);
}

struct FakeDrive
{
    std::vector<HttpRequest> requests;
    std::uint64_t size = 0;
    bool expired = false;
};

HttpResult fake_drive(const HttpRequest &request, UploadProgress *, void *user)
{
    FakeDrive *drive = static_cast<FakeDrive *>(user);
    drive->requests.push_back(request);
    HttpResult result;
    result.transport_ok = true;
    result.status = 200;
    if (request.url.find("login.microsoftonline.com") != std::string::npos)
    {
        if (drive->expired)
        {
            result.status = 400;
            result.body =
                R"({"error":"invalid_grant","error_description":"AADSTS70000.\r\nTrace"})";
        }
        else
        {
            result.body = R"({"token_type":"Bearer","access_token":"at","refresh_token":"r2"})";
        }
    }
    else if (request.url.find("createUploadSession") != std::string::npos)
    {
        result.body = R"({"uploadUrl":"https://up.example/s1"})";
    }
    else if (request.method == "PUT" && request.raw_offset + request.raw_length < drive->size)
    {
        result.status = 202;
        result.body = R"({"nextExpectedRanges":["0-"]})";
    }
    else if (request.url.find("/createLink") != std::string::npos)
    {
        result.status = 201;
        result.body = R"({"id":"perm","link":{"type":"view","webUrl":"https://1drv.ms/v/share"}})";
    }
    else if (request.url.find("discord.com") != std::string::npos)
    {
        result.body = R"({"attachments":[]})";
    }
    else
    {
        // Graph can name who made it, and its id, before the item's own id.
        result.status = 201;
        result.body = R"({"createdBy":{"user":{"id":"someone"}},"id":"ITEM!1","name":"x.webm",)"
                      R"("webUrl":"https://onedrive.live.com/x"})";
    }
    return result;
}

bool has_header(const HttpRequest &request, const std::string &header)
{
    return std::find(request.headers.begin(), request.headers.end(), header) !=
           request.headers.end();
}

TEST(onedrive_signs_in_and_sends_slices)
{
    Config config = ready_config();
    Destination &drive = *config.find("onedrive");
    CHECK(!drive.configured());
    CHECK_EQ(drive.path, "PShare");
    drive.client_id = "app-1";
    drive.token = "r1";
    CHECK(drive.configured());
    const std::string token_file = scratch() + "/onedrive-onedrive.token";
    const std::uint64_t slice = 10ull << 20;
    const MediaItem clip = sample_item("/v/PS5/PPSA01234/20261009_183501_00000002.webm", slice + 5);
    FakeDrive server;
    server.size = clip.size;
    UploadProgress progress;
    UploadOutcome outcome = send_to_onedrive(config, drive, clip, "Astro: Bot?", &progress,
                                             fake_drive, &server, false, token_file);
    CHECK(outcome.ok);
    CHECK_EQ(outcome.link, "https://onedrive.live.com/x");
    CHECK_EQ(progress.sent.load(), clip.size);
    CHECK_EQ(server.requests.size(), 4u);
    if (server.requests.size() == 4)
    {
        CHECK(server.requests[0].text.find("client_id=app-1") != std::string::npos);
        CHECK(server.requests[0].text.find("refresh_token=r1&") != std::string::npos);
        CHECK_EQ(server.requests[1].url, "https://graph.microsoft.com/v1.0/me/drive/root:/PShare/"
                                         "Astro_%20Bot_/20261009_183501_00000002.webm"
                                         ":/createUploadSession");
        CHECK(has_header(server.requests[1], "Authorization: Bearer at"));
        const HttpRequest &first = server.requests[2];
        const HttpRequest &last = server.requests[3];
        CHECK_EQ(first.method, "PUT");
        CHECK_EQ(first.url, "https://up.example/s1");
        CHECK(has_header(first, "Content-Range: bytes 0-10485759/10485765"));
        CHECK_EQ(first.raw_length, slice);
        CHECK(has_header(last, "Content-Range: bytes 10485760-10485764/10485765"));
        CHECK_EQ(last.raw_offset, slice);
        CHECK_EQ(last.raw_length, 5u);
        // The upload session URL is already authorized; the token stays with Graph.
        CHECK(!has_header(last, "Authorization: Bearer at"));
    }
    std::string saved;
    CHECK(read_text_file(token_file, &saved));
    CHECK_EQ(saved, "r1\nr2\n");

    // The renewed token is used next, until a new one is typed in.
    server = FakeDrive{};
    server.size = clip.size;
    outcome = send_to_onedrive(config, drive, clip, "Astro", nullptr, fake_drive, &server, false,
                               token_file);
    CHECK(outcome.ok);
    CHECK(!server.requests.empty() &&
          server.requests[0].text.find("refresh_token=r2&") != std::string::npos);
    drive.token = "r9";
    server.requests.clear();
    send_to_onedrive(config, drive, clip, "Astro", nullptr, fake_drive, &server, false, token_file);
    CHECK(!server.requests.empty() &&
          server.requests[0].text.find("refresh_token=r9&") != std::string::npos);

    server = FakeDrive{};
    server.expired = true;
    outcome = send_to_onedrive(config, drive, clip, "Astro", nullptr, fake_drive, &server, false,
                               token_file);
    CHECK(!outcome.ok);
    CHECK(outcome.error.find("expired") != std::string::npos);
    CHECK_EQ(server.requests.size(), 1u);

    HttpRequest request;
    std::string error;
    CHECK(!plan_request(drive, clip, caption_info(clip, "Astro"), {}, &request, &error));

    // A clip too big for Discord goes to OneDrive, and Discord gets a link anyone can open.
    drive.token = "r1";
    config.find("discord")->fallback = "onedrive";
    const MediaItem big = sample_item(clip.path, 30ull << 20);
    server = FakeDrive{};
    server.size = big.size;
    outcome = upload_capture_with(config, *config.find("discord"), big, "Astro", nullptr,
                                  fake_drive, &server, false);
    CHECK(outcome.ok);
    CHECK_EQ(outcome.link, "https://1drv.ms/v/share");
    bool linked = false;
    for (const HttpRequest &each : server.requests)
    {
        if (each.url.find("/items/ITEM%211/createLink") != std::string::npos)
            linked = true;
        if (each.url.find("discord.com") != std::string::npos)
            CHECK(part(each, "payload_json")->value.find("https://1drv.ms/v/share") !=
                  std::string::npos);
    }
    CHECK(linked);
    std::string top;
    CHECK(json_top_string(R"({"a":{"id":"inner"},"id":"outer"})", "id", &top) && top == "outer");
    CHECK(!json_top_string(R"({"a":{"id":"inner"}})", "id", &top));

    IniDocument document;
    document.parse(default_config_text());
    store_config(config, &document);
    IniDocument again;
    again.parse(document.write());
    Config back;
    load_config(again, &back, nullptr);
    CHECK_EQ(back.find("onedrive")->client_id, "app-1");
    CHECK_EQ(back.find("onedrive")->token, "r1");
    CHECK_EQ(back.find("onedrive")->path, "PShare");
    CHECK_EQ(back.find("usb")->path, "usb");

    std::vector<std::string> errors;
    CHECK_EQ(apply_setup_form({{"d.onedrive.client_id", " app-2 "}}, &back, &errors), 1);
    CHECK_EQ(back.find("onedrive")->client_id, "app-2");
    CHECK(setup_page(back, "", false).find("name=\"d.onedrive.client_id\"") != std::string::npos);
    Paths paths;
    paths.data_root = "/x";
    CHECK_EQ(paths.onedrive_token("onedrive"), "/x/onedrive-onedrive.token");
}

// ---- history -------------------------------------------------------------------

TEST(history_appends_and_reads)
{
    const std::string path = scratch() + "/history.tsv";
    History history(path);
    HistoryEntry entry;
    entry.time = 100;
    entry.ok = true;
    entry.automatic = true;
    entry.destination = "discord";
    entry.path = "/a.jpg";
    entry.detail = "https://cdn/a\tb"; // a tab must not break the line
    CHECK(history.append(entry));
    entry.ok = false;
    entry.path = "/b.jpg";
    entry.detail = "No network";
    CHECK(history.append(entry));
    History reader(path);
    CHECK(reader.refresh());
    CHECK_EQ(reader.entries().size(), 2u);
    CHECK(reader.uploaded("/a.jpg"));
    CHECK(!reader.uploaded("/b.jpg"));
    CHECK_EQ(reader.successes(), 1);
    CHECK_EQ(reader.failures(), 1);
    CHECK(!reader.refresh()); // nothing new
}

// ---- the phone setup page --------------------------------------------------------

TEST(setup_requests_and_forms)
{
    const std::string raw = "POST /save?x=1 HTTP/1.1\r\nHost: ps5\r\nContent-Length: 28\r\n\r\n"
                            "pin=123456&d.discord.url=a+b";
    CHECK(web_head_length(raw) > 0);
    WebRequest request;
    CHECK(parse_web_request(raw, &request));
    CHECK_EQ(request.method, "POST");
    CHECK_EQ(request.path, "/save");
    CHECK_EQ(request.query["x"], "1");
    const auto form = parse_form(request.body);
    CHECK_EQ(form.at("pin"), "123456");
    CHECK_EQ(form.at("d.discord.url"), "a b");
    CHECK_EQ(url_decode("%41%2f%zz"), "A/%zz");
    CHECK_EQ(html_escape("<a href=\"x\">"), "&lt;a href=&quot;x&quot;&gt;");
}

TEST(size_limits_stay_within_the_service)
{
    Config config = ready_config();
    Destination &discord = *config.find("discord");
    CHECK_EQ(discord.service_ceiling_mb(), 100);
    discord.max_mb = 0;
    CHECK_EQ(discord.size_limit(), 20ull << 20);
    CHECK(discord.has_size_limit());
    CHECK(config.find("uguu")->has_size_limit());
    CHECK(!config.find("usb")->has_size_limit());
    CHECK(!config.find("onedrive")->has_size_limit());
    CHECK_EQ(config.find("x0")->max_mb, 1024);
    std::vector<std::string> errors;
    CHECK_EQ(apply_setup_form({{"d.discord.max_mb", "150"}, {"d.catbox.max_mb", "100"}}, &config,
                              &errors),
             1);
    CHECK_EQ(errors.size(), 1u);
    CHECK_EQ(discord.max_mb, 0);
    CHECK_EQ(config.find("catbox")->max_mb, 100);
    const std::string page = setup_page(config, "", false);
    CHECK(page.find("name=\"d.catbox.max_mb\"") != std::string::npos);
    CHECK(page.find("name=\"d.usb.max_mb\"") == std::string::npos);
}

TEST(setup_form_changes_the_config)
{
    Config config = ready_config();
    std::map<std::string, std::string> form = {
        {"g.auto_upload", "false"},
        {"g.retries", "7"},
        {"g.default_destination", "catbox"},
        {"d.discord.url", " https://discord.com/api/webhooks/2/x "},
        {"d.discord.max_mb", "25"},
        {"d.custom.headers", "A: 1\nnot a header\nB: 2"},
        {"d.litterbox.litterbox_time", "12h"},
    };
    std::vector<std::string> errors;
    const int changed = apply_setup_form(form, &config, &errors);
    CHECK(errors.empty());
    CHECK(changed >= 7);
    CHECK(!config.auto_upload);
    CHECK_EQ(config.retries, 7);
    CHECK_EQ(config.default_destination, "catbox");
    CHECK_EQ(config.find("discord")->url, "https://discord.com/api/webhooks/2/x");
    CHECK_EQ(config.find("discord")->max_mb, 25);
    CHECK_EQ(config.find("custom")->headers.size(), 2u);
    CHECK_EQ(config.find("litterbox")->litterbox_time, "12h");
    form = {{"g.retries", "99"},
            {"d.litterbox.litterbox_time", "1 week"},
            {"g.default_destination", "nowhere"}};
    errors.clear();
    apply_setup_form(form, &config, &errors);
    CHECK_EQ(errors.size(), 3u);
    const std::string page = setup_page(config, "Saved", false);
    CHECK(page.find("name=\"d.discord.url\"") != std::string::npos);
    // Destinations fold up under their names; their fields are still in the form.
    CHECK(page.find("<details class=\"card\"><summary><h2>Discord") != std::string::npos);
    // Saved secrets never go back to the browser; an empty field keeps them.
    CHECK(page.find("webhooks/2/x") == std::string::npos);
    CHECK(page.find("123:abc") == std::string::npos);
    form = {{"d.discord.url", ""}, {"d.telegram.token", " "}, {"d.custom.headers", ""}};
    apply_setup_form(form, &config, &errors);
    CHECK_EQ(config.find("discord")->url, "https://discord.com/api/webhooks/2/x");
    CHECK_EQ(config.find("telegram")->token, "123:abc");
    CHECK_EQ(config.find("custom")->headers.size(), 2u);
}

TEST(daemon_status_round_trip)
{
    DaemonStatus status;
    status.version = "1.0.0";
    status.state = "watching";
    status.uploaded = 3;
    status.build = "b1234abcd";
    status.setup_port = 9078;
    const DaemonStatus back = DaemonStatus::parse(status.serialize());
    CHECK(back.reachable);
    CHECK_EQ(back.state, "watching");
    CHECK_EQ(back.uploaded, 3);
    CHECK_EQ(back.build, "b1234abcd");
    CHECK_EQ(elf_build_id(std::string("\x7f"
                                      "ELF..PSHARE_BUILD=\0..PSHARE_BUILD=b12ab\0",
                                      40)),
             "b12ab");
    CHECK_EQ(elf_build_id("no marker"), "");
    CHECK_EQ(back.setup_port, 9078);
    status.job = 7;
    status.ip = "192.168.1.5";
    const DaemonStatus again = DaemonStatus::parse(status.serialize() + "sent=10\ntotal=20\n");
    CHECK_EQ(again.job, 7);
    CHECK_EQ(again.ip, "192.168.1.5");
    CHECK_EQ(again.sent, 10u);
    CHECK_EQ(again.total, 20u);
}

// ---- sending without the internet ---------------------------------------------

TEST(smb_addresses)
{
    SmbAddress address;
    std::string error;
    CHECK(parse_smb_url("smb://192.168.1.10/Captures/PS5/Clips", &address, &error));
    CHECK_EQ(address.host, "192.168.1.10");
    CHECK_EQ(address.share, "Captures");
    CHECK_EQ(address.folder, "PS5/Clips");
    CHECK(parse_smb_url("smb://desk/Share", &address, &error));
    CHECK_EQ(address.folder, "");
    CHECK(!parse_smb_url("http://desk/Share", &address, &error));
    CHECK(!parse_smb_url("smb://desk", &address, &error));
    CHECK(!error.empty());
}

TEST(folder_copies)
{
    const std::string root = scratch() + "/copy";
    std::system(("mkdir -p '" + root + "/photo/PPSA01234' '" + root + "/drive'").c_str());
    const std::string file = root + "/photo/PPSA01234/20261009_183501_00000001.jpg";
    CHECK(write_text_file_atomic(file, std::string(5000, 'x')));
    MediaItem item;
    CHECK(describe_media(file, &item));
    Destination usb;
    usb.id = "usb";
    usb.type = DestinationType::folder;
    usb.path = "usb";
    Paths paths;
    paths.usb_roots[0] = root + "/missing";
    paths.usb_roots[1] = root + "/drive";
    paths.usb_roots[2] = paths.usb_roots[3] = "";
    CHECK_EQ(find_drive(paths), root + "/drive");
    UploadProgress progress;
    UploadOutcome outcome = copy_to_folder(usb, paths, item, "Astro: Bot?", &progress);
    CHECK(outcome.ok);
    const std::string target = root + "/drive/PShare/Astro_ Bot_/20261009_183501_00000001.jpg";
    CHECK_EQ(outcome.link, target);
    std::string copied;
    CHECK(read_text_file(target, &copied));
    CHECK_EQ(copied.size(), 5000u);
    paths.usb_roots[1] = root + "/missing";
    outcome = copy_to_folder(usb, paths, item, "Astro", &progress);
    CHECK(!outcome.ok);
    CHECK_EQ(safe_name(".."), "Captures");
}

TEST(pending_deletes_are_finished_by_the_app)
{
    const std::string root = scratch() + "/pending";
    std::system(("mkdir -p '" + root + "/photo/PPSA01234/a' '" + root + "/video'").c_str());
    Paths paths;
    paths.data_root = root;
    paths.photo_root = root + "/photo";
    paths.video_root = root + "/video";
    const std::string shot = root + "/photo/PPSA01234/a/20261009_183501_00000001.jpg";
    write_file(shot, "x");
    write_file(root + "/photo/PPSA01234/a/20261009_183501_00000001.dat", "d");
    CHECK(add_pending_delete(paths, shot));
    CHECK(add_pending_delete(paths, root + "/photo/PPSA01234/a/gone.jpg")); // already gone
    CHECK(add_pending_delete(paths, "/etc/passwd"));                        // not a capture
    CHECK_EQ(delete_pending_captures(paths), 2);
    CHECK(!file_exists(shot));
    CHECK(!file_exists(root + "/photo/PPSA01234/a"));
    // Nothing is left to retry: what went is done, and /etc/passwd is not a capture.
    CHECK(file_exists("/etc/passwd"));
    CHECK(!file_exists(paths.pending_deletes()));
    CHECK_EQ(delete_pending_captures(paths), 0);
}

TEST(also_send_round_trip)
{
    IniDocument document;
    document.parse(default_config_text());
    Config config;
    std::vector<std::string> warnings;
    load_config(document, &config, &warnings);
    config.also_send = {"usb", "pc"};
    Destination *usb = config.find("usb");
    CHECK(usb != nullptr && usb->local());
    store_config(config, &document);
    IniDocument again;
    again.parse(document.write());
    Config back;
    load_config(again, &back, &warnings);
    CHECK(back.also_send.size() == 2 && back.also_send[1] == "pc");
}

TEST(viewer_page)
{
    ViewInfo info;
    info.item.path = "/user/av_contents/video/PS5/PPSA01234/20261009_183600_00000002.webm";
    info.item.game = "Astro <Test>";
    info.item.video = true;
    info.next = "/user/av_contents/photo/x.jpg";
    info.destinations = {{"catbox", "catbox.moe", true}, {"usb", "USB drive", false}};
    info.link = "https://files.catbox.moe/abc.webm";
    const std::string html = view_page(info);
    CHECK(html.find("Astro &lt;Test&gt;") != std::string::npos);
    CHECK(html.find("<video id=\"media\"") != std::string::npos);
    CHECK(html.find("\"dests\":[{\"id\":\"catbox\"") != std::string::npos);
    CHECK(html.find("/file?p=%2Fuser%2Fav_contents") != std::string::npos);
    CHECK(html.find("@TITLE@") == std::string::npos && html.find("@DATA@") == std::string::npos);
    info.item.game = "x</script><b>";
    CHECK(view_page(info).find("x</script>") == std::string::npos);
}

TEST(ranges)
{
    std::uint64_t first = 0, last = 0;
    CHECK(parse_range("bytes=100-199", 1000, &first, &last));
    CHECK(first == 100 && last == 199);
    CHECK(parse_range("bytes=900-", 1000, &first, &last));
    CHECK(first == 900 && last == 999);
    CHECK(parse_range("bytes=-100", 1000, &first, &last));
    CHECK(first == 900 && last == 999);
    CHECK(!parse_range("bytes=2000-", 1000, &first, &last));
    CHECK(!parse_range("items=1-2", 1000, &first, &last));
}

// ---- thumbnails ------------------------------------------------------------------

TEST(thumbnails_shrink_and_encode)
{
    Picture big;
    big.width = 400;
    big.height = 200;
    big.rgba.assign(static_cast<std::size_t>(400 * 200 * 4), 200);
    const Picture small = shrink(big, 100, 100);
    CHECK_EQ(small.width, 100);
    CHECK_EQ(small.height, 50);
    CHECK_EQ(small.rgba[0], 200);
    const std::string jpeg = encode_jpeg(small, 80);
    CHECK(jpeg.size() > 100 && static_cast<unsigned char>(jpeg[0]) == 0xff);
    Picture decoded;
    CHECK(decode_picture_bytes(jpeg, 50, 50, &decoded));
    CHECK_EQ(decoded.width, 50);
    CHECK_EQ(decoded.height, 25);
    CHECK(!decode_picture_bytes("not a picture", 50, 50, &decoded));
}

} // namespace

int main()
{
    int failed_cases = 0;
    for (const Case &each : cases())
    {
        const int before = g_failures;
        each.body();
        const bool ok = g_failures == before;
        failed_cases += ok ? 0 : 1;
        std::fprintf(stderr, "%s %s\n", ok ? "[ ok ]" : "[FAIL]", each.name);
    }
    std::system(("rm -rf '" + scratch() + "'").c_str());
    std::fprintf(stderr, "%zu tests, %d failed\n", cases().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}
