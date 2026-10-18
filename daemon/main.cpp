// PShare - pshared, the background uploader.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small program that keeps running after the app closes, notices every
// new screenshot and clip, and sends it to the configured destination.
//
// It is a payload for the jailbreak's ELF loader (127.0.0.1:9021); the PShare
// app sends it there. Payloads run outside the app sandbox, so the capture
// folders and /data are readable without any further elevation.
//
//   127.0.0.1:9077  control line for the app (PING, STATUS, PAUSE, ...)
//   0.0.0.0:9078    phone setup page, only while the app has it switched on
//
// The same file builds on a PC (no PSHARE_PAYLOAD) for testing; the roots
// then come from PSHARE_DATA / PSHARE_PHOTO / PSHARE_VIDEO / PSHARE_APPMETA.

#include "pshare/core/config.hpp"
#include "pshare/core/history.hpp"
#include "pshare/core/media.hpp"
#include "pshare/core/net.hpp"
#include "pshare/core/setup_web.hpp"
#include "pshare/core/text.hpp"
#include "pshare/core/upload.hpp"
#include "pshare/core/watcher.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace pshare
{
namespace
{
constexpr std::size_t kLogLimit = 512 * 1024;
constexpr int kSetupMinutes = 30;
constexpr int kDeleteAskSeconds = 300;

std::int64_t now_seconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

// ---- log -------------------------------------------------------------------

class Log
{
  public:
    void open(std::string path)
    {
        path_ = std::move(path);
    }
    void write(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char message[1024];
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);
        const std::time_t now = std::time(nullptr);
        std::tm parts{};
        localtime_r(&now, &parts);
        char line[1100];
        const int length = std::snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d %s\n",
                                         parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                                         parts.tm_hour, parts.tm_min, parts.tm_sec, message);
        std::lock_guard<std::mutex> lock(mutex_);
#if !defined(PSHARE_PAYLOAD)
        std::fputs(line, stderr);
#endif
        if (path_.empty() || length <= 0)
            return;
        struct stat info
        {
        };
        if (stat(path_.c_str(), &info) == 0 && static_cast<std::size_t>(info.st_size) > kLogLimit)
            rename(path_.c_str(), (path_ + ".1").c_str());
        const int descriptor = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (descriptor < 0)
            return;
        (void)::write(descriptor, line, static_cast<std::size_t>(length));
        close(descriptor);
    }

  private:
    std::string path_;
    std::mutex mutex_;
};

Log g_log;

// ---- shared state ----------------------------------------------------------

// QUEUE [<destination>\t]<path>[\t<flags>]: the app hands an upload over, so
// it goes on after the app closes. An empty destination is the configured one.
// Each gets a number the app asks RESULT about. Flags, comma separated:
//   delete  delete the capture from the console once it is sent;
//   batch   one of many ("Send all"): no notification each, one at the end.
struct ManualJob
{
    int id = 0;
    std::string destination;
    std::string path;
    bool delete_after = false;
    bool batch = false;
};

// What the batch jobs sent since the queue was last empty did, for the one
// notification at its end.
struct BatchTally
{
    int sent = 0;
    int failed = 0;
    int deleted = 0;
    int waiting = 0; // sent, and left for the app to delete
};

struct Shared
{
    std::mutex mutex;
    DaemonStatus status;
    std::deque<ManualJob> manual;
    std::chrono::steady_clock::time_point last_queued; // the newest QUEUE
    BatchTally batch;
    // A browser asks to delete captures: the app asks on the console, and
    // only the app deletes (DELETEASK). Forgotten after kDeleteAskSeconds.
    std::vector<std::string> delete_ask;
    std::int64_t delete_ask_at = 0;
    int next_job = 1;
    int current_job = 0;
    std::vector<int> sending; // every job going now: one, or the jobs of an album
    // RESULT answers: "ok <destination>\t<link>" or "failed <destination>\t<error>",
    // the newest 64.
    std::map<int, std::string> results;
    bool paused = false;
    bool reload = false;
    bool rescan = false;
    std::int64_t setup_until = 0; // the phone page is open until then
    std::int64_t app_seen = 0;    // the app's last "STATUS app": the page stays up meanwhile
};

Shared g_shared;
std::atomic<bool> g_quit{false};

// Browser requests for a capture this process reads back as zeros, waiting
// for the app to send the bytes (see RELAY). Guarded by g_shared.mutex.
struct RelayJob
{
    int client = -1; // the browser's socket; the head is already sent
    std::string path;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::int64_t queued = 0;
};
std::deque<RelayJob> g_relay;
std::condition_variable g_relay_ready;

// The app is running (its status poll said so): it can relay captures.
bool app_present_locked()
{
    return g_shared.app_seen > now_seconds() - 15;
}
UploadProgress g_progress;

// The build this copy is, for telling a newer pshared from an older one. The
// app reads the same marker out of the pshared.elf it ships.
#ifndef PSHARE_BUILD
#define PSHARE_BUILD dev
#endif
#define PSHARE_TEXT2(x) #x
#define PSHARE_TEXT(x) PSHARE_TEXT2(x)
const char kBuildMarker[] = "PSHARE_BUILD=" PSHARE_TEXT(PSHARE_BUILD);

const char *build_id()
{
    return kBuildMarker + sizeof("PSHARE_BUILD=") - 1;
}

Paths paths_from_environment()
{
    Paths paths;
#if !defined(PSHARE_PAYLOAD)
    if (const char *value = std::getenv("PSHARE_DATA"))
        paths.data_root = value;
    if (const char *value = std::getenv("PSHARE_PHOTO"))
        paths.photo_root = value;
    if (const char *value = std::getenv("PSHARE_VIDEO"))
        paths.video_root = value;
    if (const char *value = std::getenv("PSHARE_APPMETA"))
        paths.appmeta_root = value;
#endif
    return paths;
}

int port_from_environment(const char *name, int fallback)
{
#if !defined(PSHARE_PAYLOAD)
    if (const char *value = std::getenv(name))
        return static_cast<int>(parse_int(value, fallback));
#else
    (void)name;
#endif
    return fallback;
}

// ---- control line ----------------------------------------------------------

// The phone page (setup, captures, the viewer) is up while SETUP is on and
// while the app is running. Call with g_shared.mutex held.
bool web_open_locked()
{
    const std::int64_t now = now_seconds();
    return g_shared.setup_until > now || g_shared.app_seen > now - 15;
}

int queue_manual(const std::string &destination, const std::string &path, std::string *error,
                 std::string_view flags = {})
{
    MediaItem item;
    if (path.empty() || !describe_media(path, &item))
    {
        *error = "not a capture";
        return 0;
    }
    if (g_shared.manual.size() >= 5000)
    {
        *error = "queue full";
        return 0;
    }
    // Without the app to fetch it through (see fetch_through_app), the app
    // reads it properly and sends it itself when this answers no.
    if (!app_present_locked() && capture_reads_blank(path))
    {
        *error = "The background uploader can't read this clip right now; send it from PShare "
                 "on the console";
        return 0;
    }
    ManualJob job;
    job.id = g_shared.next_job++;
    job.destination = destination;
    job.path = path;
    for (const std::string &flag : split(flags, ','))
    {
        job.delete_after = job.delete_after || iequals(trim(flag), "delete");
        job.batch = job.batch || iequals(trim(flag), "batch");
    }
    g_shared.manual.push_back(std::move(job));
    g_shared.last_queued = std::chrono::steady_clock::now();
    g_shared.rescan = true;
    return g_shared.next_job - 1;
}

// RESULT's answer for a job: "ok <dest>\t<link>", "failed <dest>\t<error>",
// "pending" or "unknown". Call with g_shared.mutex held.
std::string job_result_locked(int id)
{
    const auto found = g_shared.results.find(id);
    if (found != g_shared.results.end())
        return found->second;
    if (id != 0 &&
        std::find(g_shared.sending.begin(), g_shared.sending.end(), id) != g_shared.sending.end())
        return "pending";
    for (const ManualJob &job : g_shared.manual)
        if (job.id == id)
            return "pending";
    return "unknown";
}

std::string handle_command(std::string_view line)
{
    const std::string text(trim(line));
    const std::size_t space = text.find(' ');
    const std::string verb = upper(text.substr(0, space));
    const std::string argument =
        space == std::string::npos ? std::string() : std::string(trim(text.substr(space + 1)));
    std::lock_guard<std::mutex> lock(g_shared.mutex);
    if (verb == "PING")
        return std::string("pong pshared ") + kVersion + " " + build_id() + "\n";
    if (verb == "STATUS")
    {
        // The app asks every second; while it runs, its browser pages are up.
        if (iequals(argument, "app"))
            g_shared.app_seen = now_seconds();
        DaemonStatus status = g_shared.status;
        status.queued += static_cast<int>(g_shared.manual.size());
        if (!web_open_locked())
            status.setup_port = 0;
        status.job = g_shared.current_job;
        if (g_shared.delete_ask_at < now_seconds() - kDeleteAskSeconds)
            g_shared.delete_ask.clear();
        status.delete_ask = static_cast<int>(g_shared.delete_ask.size());
        std::string reply = status.serialize();
        reply += "sent=" + std::to_string(g_progress.sent.load()) + "\n";
        reply += "total=" + std::to_string(g_progress.total.load()) + "\n";
        return reply;
    }
    if (verb == "PAUSE")
    {
        g_shared.paused = true;
        g_shared.status.state = "paused";
        g_log.write("paused by the app");
        return "ok\n";
    }
    if (verb == "RESUME")
    {
        g_shared.paused = false;
        g_shared.status.state = "watching";
        g_log.write("resumed by the app");
        return "ok\n";
    }
    if (verb == "RELOAD")
    {
        g_shared.reload = true;
        return "ok\n";
    }
    if (verb == "RESCAN")
    {
        g_shared.rescan = true;
        return "ok\n";
    }
    if (verb == "CANCEL")
    {
        // What the app handed over and has not gone yet is dropped too.
        for (const ManualJob &job : g_shared.manual)
        {
            g_shared.results[job.id] = "failed " + job.destination + "\tCancelled";
            if (job.batch)
                ++g_shared.batch.failed;
        }
        g_shared.manual.clear();
        while (g_shared.results.size() > 64)
            g_shared.results.erase(g_shared.results.begin());
        g_progress.cancel = true;
        return "ok\n";
    }
    if (verb == "QUEUE")
    {
        // <path>, <destination>\t<path> or <destination>\t<path>\t<flags>. Split
        // before trimming: an empty destination leaves the line starting with a tab.
        const std::vector<std::string> fields =
            split(space == std::string::npos ? std::string_view()
                                             : std::string_view(text).substr(space + 1),
                  '\t');
        const std::string destination =
            fields.size() >= 2 ? std::string(trim(fields[0])) : std::string();
        const std::string path = std::string(trim(fields.size() >= 2 ? fields[1] : argument));
        std::string error;
        const int id = queue_manual(destination, path, &error, fields.size() >= 3 ? fields[2] : "");
        if (id == 0)
            return "error " + error + "\n";
        return "ok " + std::to_string(id) + "\n";
    }
    if (verb == "RESULT")
        return job_result_locked(static_cast<int>(parse_int(argument, 0))) + "\n";
    if (verb == "SETUP")
    {
        if (iequals(argument, "off"))
        {
            g_shared.setup_until = 0;
            return "ok\n";
        }
        g_shared.setup_until = now_seconds() + kSetupMinutes * 60;
        g_log.write("phone setup page open for %d minutes", kSetupMinutes);
        return "ok\n";
    }
    if (verb == "DELETEASK")
    {
        // "DELETEASK": the paths a browser asked to delete, one per line after
        // "ok"; "DELETEASK clear": the app has asked and answered.
        if (iequals(argument, "clear"))
        {
            g_shared.delete_ask.clear();
            return "ok\n";
        }
        std::string reply = "ok\n";
        for (const std::string &path : g_shared.delete_ask)
            reply += path + "\n";
        return reply;
    }
    if (verb == "QUIT")
    {
        g_quit = true;
        g_progress.cancel = true;
        g_log.write("asked to stop");
        return "ok\n";
    }
    return "error unknown command\n";
}

// The app's end of the clip relay: wait a little for a browser request,
// hand the app its range, and pass the bytes the app sends on to the browser.
void relay_session(int app)
{
    socket_timeouts(app, 30000);
    RelayJob job;
    bool have = false;
    {
        std::unique_lock<std::mutex> lock(g_shared.mutex);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!g_quit.load())
        {
            // A browser that waited this long has given up.
            while (!g_relay.empty() && g_relay.front().queued < now_seconds() - 20)
            {
                close(g_relay.front().client);
                g_relay.pop_front();
            }
            if (!g_relay.empty())
            {
                job = g_relay.front();
                g_relay.pop_front();
                have = true;
                break;
            }
            if (g_relay_ready.wait_until(lock, until) == std::cv_status::timeout)
                break;
        }
    }
    if (!have)
    {
        send_all(app, "none\n");
        close(app);
        return;
    }
    bool ok = send_all(app, std::to_string(job.first) + "\t" + std::to_string(job.last) + "\t" +
                                job.path + "\n");
    std::vector<char> buffer(256 * 1024);
    std::uint64_t left = job.last - job.first + 1;
    while (ok && left > 0 && !g_quit.load())
    {
        const ssize_t got = recv(app, buffer.data(), buffer.size(), 0);
        if (got <= 0)
            break;
        ok = send_all(job.client, std::string_view(buffer.data(), static_cast<std::size_t>(got)));
        left -= std::min<std::uint64_t>(left, static_cast<std::uint64_t>(got));
    }
    shutdown(job.client, SHUT_WR);
    close(job.client);
    close(app);
}

void control_thread(int listener)
{
    while (!g_quit.load())
    {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0)
        {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                usleep(200000);
            continue;
        }
        socket_timeouts(client, 1500);
        std::string line;
        char buffer[512];
        while (line.find('\n') == std::string::npos && line.size() < 2048)
        {
            const ssize_t got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0)
                break;
            line.append(buffer, static_cast<std::size_t>(got));
        }
        if (starts_with(line, "RELAY"))
        {
            std::thread(relay_session, client).detach();
            continue;
        }
        if (!line.empty())
            send_all(client, handle_command(line.substr(0, line.find('\n'))));
        close(client);
    }
}

// ---- phone setup page ------------------------------------------------------

bool save_config_from(const Paths &paths, const Config &config, std::string *error)
{
    IniDocument document;
    std::string text;
    if (read_text_file(paths.config(), &text))
        document.parse(text);
    else
        document.parse(default_config_text());
    store_config(config, &document);
    if (!write_text_file_atomic(paths.config(), document.write()))
    {
        if (error)
            *error = "Could not write " + paths.config();
        return false;
    }
    return true;
}

// What the setup thread does with a request: answer it, or stream a file from
// a thread of its own (a clip can take minutes to download).
struct WebReply
{
    std::string text;
    std::string stream_path; // non-empty: stream this capture
    bool download = false;
    std::string range;
};

constexpr const char *kHtml = "text/html; charset=utf-8";
constexpr int kGalleryPage = 48;

// A capture inside the capture folders, and nothing else.
bool servable(const Paths &paths, const std::string &path, MediaItem *item)
{
    if (path.find("/../") != std::string::npos || path.find("/./") != std::string::npos)
        return false;
    const bool inside =
        starts_with(path, paths.photo_root + "/") || starts_with(path, paths.video_root + "/");
    return inside && describe_media(path, item);
}

void stream_file(int client, std::string path, std::string range, bool download)
{
    socket_timeouts(client, 30000);
    MediaItem item;
    describe_media(path, &item);
    const int file = ::open(path.c_str(), O_RDONLY);
    struct stat info
    {
    };
    if (file < 0 || fstat(file, &info) != 0)
    {
        send_all(client, web_response(404, "text/plain", "Gone"));
        if (file >= 0)
            close(file);
        close(client);
        return;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
    std::uint64_t first = 0;
    std::uint64_t last = size > 0 ? size - 1 : 0;
    const bool partial = !range.empty() && parse_range(range, size, &first, &last);
    std::string head = file_response_head(partial ? 206 : 200, item.content_type(), size, first,
                                          last, download ? item.file : std::string());
    if (size == 0)
        head = web_response(200, item.content_type(), "");
    // Read back as zeros here (the console can do that to payloads after a
    // restart) while the app reads it properly: the app sends the bytes.
    if (size > 0 && capture_reads_blank(path))
    {
        bool relay = false;
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            relay = app_present_locked();
        }
        if (!relay)
        {
            send_all(client, web_response(409, "text/plain",
                                          "Open PShare on the console to play or download this "
                                          "capture"));
            close(file);
            close(client);
            return;
        }
        close(file);
        if (!send_all(client, head))
        {
            close(client);
            return;
        }
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        g_relay.push_back({client, path, first, last, now_seconds()});
        g_relay_ready.notify_one();
        return;
    }
    bool ok = send_all(client, head);
    if (size > 0 && first > 0)
        ok = ok && lseek(file, static_cast<off_t>(first), SEEK_SET) == static_cast<off_t>(first);
    std::vector<char> buffer(256 * 1024);
    std::uint64_t left = size > 0 ? last - first + 1 : 0;
    while (ok && left > 0 && !g_quit.load())
    {
        const ssize_t got =
            read(file, buffer.data(),
                 static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size())));
        if (got <= 0)
            break;
        ok = send_all(client, std::string_view(buffer.data(), static_cast<std::size_t>(got)));
        left -= static_cast<std::uint64_t>(got);
    }
    close(file);
    shutdown(client, SHUT_WR);
    close(client);
}

// Every capture the gallery's filter shows, in its order, every page; fills
// in the filter's game list too.
std::vector<MediaItem> filtered_captures(const Paths &paths, TitleNames *names,
                                         GalleryFilter *filter)
{
    ScanResult scan = scan_media(paths, ScanOptions{});
    // Every game for the choice list, then only the chosen one, in the chosen order.
    std::map<std::string, GalleryGame> games;
    for (const MediaItem &capture : scan.items)
    {
        GalleryGame &game = games[capture.title_id];
        game.title_id = capture.title_id;
        if (game.name.empty())
            game.name = names->display(capture.title_id);
        ++game.count;
    }
    filter->games.clear();
    for (auto &[id, game] : games)
        filter->games.push_back(std::move(game));
    std::sort(filter->games.begin(), filter->games.end(),
              [](const GalleryGame &a, const GalleryGame &b)
              { return a.count != b.count ? a.count > b.count : a.name < b.name; });
    if (!filter->game.empty() && games.count(filter->game) == 0)
        filter->game.clear();
    if (!filter->game.empty())
        scan.items.erase(std::remove_if(scan.items.begin(), scan.items.end(),
                                        [&](const MediaItem &capture)
                                        { return capture.title_id != filter->game; }),
                         scan.items.end());
    if (filter->oldest_first)
        std::reverse(scan.items.begin(), scan.items.end());
    filter->total = static_cast<int>(scan.items.size());
    return std::move(scan.items);
}

std::vector<GalleryItem> gallery_items(const Paths &paths, TitleNames *names, int page, int *pages,
                                       GalleryFilter *filter)
{
    const std::vector<MediaItem> captures = filtered_captures(paths, names, filter);
    const int count = static_cast<int>(captures.size());
    *pages = std::max(1, (count + kGalleryPage - 1) / kGalleryPage);
    page = std::clamp(page, 0, *pages - 1);
    filter->page = page;
    std::vector<GalleryItem> out;
    for (int i = page * kGalleryPage; i < std::min(count, (page + 1) * kGalleryPage); ++i)
    {
        const MediaItem &capture = captures[static_cast<std::size_t>(i)];
        GalleryItem item;
        item.path = capture.path;
        item.game = names->display(capture.title_id);
        item.when = capture.captured.date() + " " + capture.captured.time();
        item.size = human_size(capture.size);
        item.video = capture.video();
        out.push_back(std::move(item));
    }
    return out;
}

// The capture a short address names: /v/<file name without its extension>.
bool capture_by_stem(const Paths &paths, std::string_view stem, MediaItem *found)
{
    if (stem.empty() || stem.find('/') != std::string_view::npos)
        return false;
    const ScanResult scan = scan_media(paths, ScanOptions{});
    for (const MediaItem &item : scan.items)
    {
        const std::size_t dot = item.file.rfind('.');
        if (std::string_view(item.file).substr(0, dot) == stem)
        {
            *found = item;
            return true;
        }
    }
    return false;
}

WebReply serve(const Paths &paths, const WebRequest &request, TitleNames *names)
{
    WebReply reply;
    if (request.path == "/favicon.ico")
    {
        reply.text = web_response(404, "text/plain", "none");
        return reply;
    }
    std::map<std::string, std::string> form;
    if (request.method == "POST")
        form = parse_form(request.body);

    if (request.path == "/file")
    {
        const auto p = request.query.find("p");
        MediaItem item;
        if (p == request.query.end() || !servable(paths, p->second, &item))
        {
            reply.text = web_response(404, "text/plain", "Not a capture");
            return reply;
        }
        reply.stream_path = item.path;
        reply.download = request.query.count("dl") != 0;
        const auto range = request.headers.find("range");
        if (range != request.headers.end())
            reply.range = range->second;
        return reply;
    }
    if (request.path == "/view" || starts_with(request.path, "/v/"))
    {
        const auto p = request.query.find("p");
        MediaItem capture;
        const bool found =
            starts_with(request.path, "/v/")
                ? capture_by_stem(paths, url_decode(request.path.substr(3)), &capture)
                : p != request.query.end() && servable(paths, p->second, &capture);
        if (!found)
        {
            reply.text = web_response(404, "text/plain", "Not a capture");
            return reply;
        }
        IniDocument document;
        Config config;
        std::vector<std::string> warnings;
        load_config_file(paths.config(), &document, &config, &warnings);
        ViewInfo info;
        info.item.path = capture.path;
        info.item.game = names->display(capture.title_id);
        info.item.when = capture.captured.date() + " " + capture.captured.time();
        info.item.size = human_size(capture.size);
        info.item.video = capture.video();
        if (capture_reads_blank(capture.path))
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            info.blank = !app_present_locked();
        }
        const ScanResult scan = scan_media(paths, ScanOptions{});
        for (std::size_t i = 0; i < scan.items.size(); ++i)
            if (scan.items[i].path == capture.path)
            {
                if (i > 0)
                    info.prev = scan.items[i - 1].path;
                if (i + 1 < scan.items.size())
                    info.next = scan.items[i + 1].path;
                break;
            }
        const Destination *chosen = config.destination_for(capture.title_id);
        for (const Destination &d : config.destinations)
            if (d.enabled && d.configured())
                info.destinations.push_back(
                    {d.id, d.display_name(), chosen != nullptr && chosen->id == d.id});
        History history(paths.history());
        history.refresh();
        if (const HistoryEntry *sent = history.last_success(capture.path))
            if (starts_with(sent->detail, "http"))
                info.link = sent->detail;
        reply.text = web_response(200, kHtml, view_page(info));
        return reply;
    }
    if (request.method == "POST" && request.path == "/send")
    {
        const std::string path = form.count("p") ? form.at("p") : std::string();
        const std::string destination = form.count("d") ? form.at("d") : std::string();
        MediaItem capture;
        std::string body;
        if (!servable(paths, path, &capture))
        {
            body = "{\"error\":\"That capture is gone\"}";
        }
        else
        {
            std::string error;
            int id = 0;
            {
                std::lock_guard<std::mutex> lock(g_shared.mutex);
                id = queue_manual(destination, path, &error);
            }
            body = id > 0 ? "{\"id\":" + std::to_string(id) + "}"
                          : "{\"error\":\"" + json_escape(error) + "\"}";
            g_log.write("browser asked to send %s to %s", capture.file.c_str(),
                        destination.empty() ? "the default" : destination.c_str());
        }
        reply.text = web_response(200, "application/json", body);
        return reply;
    }
    if (request.path == "/result")
    {
        const int id = static_cast<int>(
            parse_int(request.query.count("id") ? request.query.at("id") : "0", 0));
        std::string answer;
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            answer = job_result_locked(id);
        }
        std::string body;
        if (answer == "pending" || answer == "unknown")
        {
            body = "{\"state\":\"" + answer + "\"}";
        }
        else
        {
            const bool ok = starts_with(answer, "ok ");
            const std::string rest = answer.substr(answer.find(' ') + 1);
            const std::size_t tab = rest.find('\t');
            const std::string dest_id =
                tab == std::string::npos ? std::string() : rest.substr(0, tab);
            const std::string detail = tab == std::string::npos ? rest : rest.substr(tab + 1);
            IniDocument document;
            Config config;
            std::vector<std::string> warnings;
            load_config_file(paths.config(), &document, &config, &warnings);
            const Destination *d = config.find(dest_id);
            body = std::string("{\"state\":\"") + (ok ? "ok" : "failed") + "\",\"dest\":\"" +
                   json_escape(d != nullptr ? d->display_name() : dest_id) + "\",\"" +
                   (ok ? "link" : "error") + "\":\"" + json_escape(detail) + "\"}";
        }
        reply.text = web_response(200, "application/json", body);
        return reply;
    }
    // The gallery's selection bar: send or delete the ticked captures, or send
    // everything the filter shows. Answered with a redirect back to the same
    // gallery page and a notice, so a reload does not post it again.
    if (request.method == "POST" && request.path == "/act")
    {
        const auto field = [&](const char *key)
        { return form.count(key) ? form.at(key) : std::string(); };
        const std::string act = field("act");
        GalleryFilter filter;
        filter.game = field("game");
        filter.oldest_first = field("order") == "old";
        std::vector<std::string> chosen;
        if (act == "send-all")
        {
            for (const MediaItem &capture : filtered_captures(paths, names, &filter))
                chosen.push_back(capture.path);
        }
        else
        {
            for (const auto &[key, value] : form)
            {
                MediaItem capture;
                if (starts_with(key, "s.") && servable(paths, value, &capture))
                    chosen.push_back(value);
            }
        }
        std::string notice;
        if (chosen.empty())
        {
            notice = "Tick some captures first";
        }
        else if (act == "delete")
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            if (!app_present_locked())
            {
                notice = "Open PShare on the PS5 to delete from here: it asks you there first";
            }
            else
            {
                g_shared.delete_ask = chosen;
                g_shared.delete_ask_at = now_seconds();
                notice = "Confirm on your PS5 to delete " + std::to_string(chosen.size()) +
                         (chosen.size() == 1 ? " capture" : " captures");
                g_log.write("browser asks to delete %zu captures", chosen.size());
            }
        }
        else if (act == "send" || act == "send-all")
        {
            History history(paths.history());
            history.refresh();
            const bool skip = form.count("skip") != 0;
            const std::string destination = field("d");
            int queued = 0;
            int skipped = 0;
            std::string error;
            {
                std::lock_guard<std::mutex> lock(g_shared.mutex);
                for (const std::string &path : chosen)
                {
                    if (skip && history.uploaded(path))
                        ++skipped;
                    else if (queue_manual(destination, path, &error, "batch") > 0)
                        ++queued;
                }
            }
            notice = queued == 0
                         ? (skipped > 0 ? "Every one of them was sent before"
                                        : "Nothing could be sent: " + error)
                         : "Sending " + std::to_string(queued) +
                               (queued == 1 ? " capture" : " captures") +
                               (skipped > 0
                                    ? " (" + std::to_string(skipped) + " sent before were skipped)"
                                    : std::string());
            g_log.write("browser sent %d captures (%d skipped)", queued, skipped);
        }
        std::string back =
            "/gallery?page=" + url_encode(field("page")) + "&notice=" + url_encode(notice);
        if (!filter.game.empty())
            back += "&game=" + url_encode(filter.game);
        if (filter.oldest_first)
            back += "&order=old";
        reply.text = web_response(303, kHtml, {}, "Location: " + back + "\r\n");
        return reply;
    }
    if (request.path == "/gallery")
    {
        int pages = 1;
        const int page = static_cast<int>(
            parse_int(request.query.count("page") ? request.query.at("page") : "0", 0));
        GalleryFilter filter;
        if (const auto game = request.query.find("game"); game != request.query.end())
            filter.game = game->second;
        filter.oldest_first =
            request.query.count("order") != 0 && request.query.at("order") == "old";
        {
            IniDocument document;
            Config config;
            std::vector<std::string> warnings;
            load_config_file(paths.config(), &document, &config, &warnings);
            for (const Destination &d : config.destinations)
                if (d.enabled && d.configured())
                    filter.destinations.emplace_back(d.id, d.display_name());
        }
        const std::vector<GalleryItem> items = gallery_items(paths, names, page, &pages, &filter);
        const auto notice = request.query.find("notice");
        reply.text = web_response(
            200, kHtml,
            gallery_page(items, std::clamp(page, 0, pages - 1), pages,
                         notice != request.query.end() ? notice->second : std::string(), filter));
        return reply;
    }

    IniDocument document;
    Config config;
    std::vector<std::string> warnings;
    load_config_file(paths.config(), &document, &config, &warnings);

    if (request.method == "POST" && request.path == "/save")
    {
        std::vector<std::string> errors;
        const int changed = apply_setup_form(form, &config, &errors);
        std::string error;
        if (changed > 0 && !save_config_from(paths, config, &error))
            errors.push_back(error);
        if (changed > 0)
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            g_shared.reload = true;
        }
        g_log.write("phone setup saved %d change(s)", changed);
        std::string notice;
        if (errors.empty())
            notice = changed > 0 ? "Saved. PShare uses the new settings right away."
                                 : "Nothing changed.";
        else
            for (const std::string &line : errors)
                notice += (notice.empty() ? "" : " ") + line + ".";
        reply.text = web_response(200, kHtml, setup_page(config, notice, !errors.empty()));
        return reply;
    }
    reply.text = web_response(200, kHtml, setup_page(config, {}, false));
    return reply;
}

void setup_thread(const Paths &paths, int port)
{
    TitleNames names(
        paths.appmeta_root,
        {paths.photo_root, paths.video_root}); // this thread's own: the cache is not shared
    int listener = -1;
    bool listen_failed = false;
    while (!g_quit.load())
    {
        bool open = false;
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            open = web_open_locked();
        }
        if (!open)
        {
            if (listener >= 0)
            {
                close(listener);
                listener = -1;
                std::lock_guard<std::mutex> lock(g_shared.mutex);
                g_shared.status.setup_port = 0;
                g_log.write("phone setup page closed");
            }
            usleep(500000);
            continue;
        }
        if (listener < 0)
        {
            listener = listen_tcp(0, port, 8);
            if (listener < 0)
            {
                // Usually a pshared that is still stopping: try again shortly,
                // and say so once rather than on every try.
                if (!listen_failed)
                    g_log.write("cannot listen on port %d for the setup page (errno %d)", port,
                                errno);
                listen_failed = true;
                usleep(1000000);
                continue;
            }
            listen_failed = false;
            socket_timeouts(listener, 1000); // accept wakes up to check the clock
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            g_shared.status.setup_port = port;
        }
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0)
            continue;
        socket_timeouts(client, 5000);
        std::string raw;
        char buffer[4096];
        bool too_large = false;
        for (;;)
        {
            const long head = web_head_length(raw);
            if (head < 0)
                break;
            if (head > 0)
            {
                const std::size_t want = static_cast<std::size_t>(head) +
                                         web_content_length(std::string_view(raw).substr(0, head));
                if (want > 256 * 1024)
                {
                    too_large = true;
                    break;
                }
                if (raw.size() >= want)
                    break;
            }
            const ssize_t got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0)
                break;
            raw.append(buffer, static_cast<std::size_t>(got));
        }
        WebRequest request;
        WebReply reply;
        if (too_large)
            reply.text = web_response(413, "text/plain", "Too large");
        else if (!parse_web_request(raw, &request))
            reply.text = web_response(400, "text/plain", "Bad request");
        else
            reply = serve(paths, request, &names);
        if (!reply.stream_path.empty())
        {
            std::thread(stream_file, client, reply.stream_path, reply.range, reply.download)
                .detach();
            continue;
        }
        send_all(client, reply.text);
        shutdown(client, SHUT_WR);
        close(client);
    }
    if (listener >= 0)
        close(listener);
}

// ---- the watch-and-send loop -----------------------------------------------

struct Settings
{
    IniDocument document;
    Config config;
    std::int64_t mtime = -1;
};

std::int64_t file_mtime(const std::string &path)
{
    struct stat info
    {
    };
    return stat(path.c_str(), &info) == 0 ? static_cast<std::int64_t>(info.st_mtime) : -1;
}

void load_settings(const Paths &paths, Settings *settings)
{
    std::vector<std::string> warnings;
    Config config;
    IniDocument document;
    load_config_file(paths.config(), &document, &config, &warnings);
    for (const std::string &warning : warnings)
        g_log.write("config: %s", warning.c_str());
    settings->config = std::move(config);
    settings->document = std::move(document);
    settings->mtime = file_mtime(paths.config());
}

void set_state(const char *state)
{
    std::lock_guard<std::mutex> lock(g_shared.mutex);
    g_shared.status.state = state;
}

// A capture this process reads back as zeros, fetched through the app's
// relay into a temporary copy under /data/pshare/relay. False when the app is
// not running or the copy came out short.
bool fetch_through_app(const Paths &paths, const MediaItem &item, std::string *copy)
{
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        if (!app_present_locked() || item.size == 0)
            return false;
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return false;
    const std::string folder = paths.data_root + "/relay";
    ensure_directory(folder);
    const std::string target = folder + "/" + item.file;
    const int out = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
    {
        close(pair[0]);
        close(pair[1]);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        g_relay.push_back({pair[0], item.path, 0, item.size - 1, now_seconds()});
        g_relay_ready.notify_one();
    }
    socket_timeouts(pair[1], 30000);
    std::vector<char> buffer(256 * 1024);
    std::uint64_t got_total = 0;
    while (got_total < item.size && !g_quit.load())
    {
        const ssize_t got = recv(pair[1], buffer.data(), buffer.size(), 0);
        if (got <= 0)
            break;
        if (::write(out, buffer.data(), static_cast<std::size_t>(got)) != got)
            break;
        got_total += static_cast<std::uint64_t>(got);
    }
    close(pair[1]);
    close(out);
    if (got_total != item.size)
    {
        ::unlink(target.c_str());
        return false;
    }
    *copy = target;
    return true;
}

// Sends one capture to `forced` (an id), or to the destination the config picks.
// Returns the id it went to, empty when nothing was sent.
std::string send_one(const Paths &paths, const Config &config, TitleNames *names, History *history,
                     const MediaItem &item, bool automatic, const std::string &forced = {},
                     UploadOutcome *result = nullptr, bool quiet = false)
{
    const Destination *destination =
        forced.empty() ? config.destination_for(item.title_id) : config.find(forced);
    if (destination == nullptr || !destination->configured())
    {
        g_log.write("no destination is set up; skipping %s", item.file.c_str());
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        g_shared.status.last_error = destination == nullptr
                                         ? "No destination is set up"
                                         : destination->display_name() + " is not set up";
        if (result != nullptr)
        {
            result->ok = false;
            result->destination = forced;
            result->error = g_shared.status.last_error;
        }
        return {};
    }
    const std::string game = names->display(item.title_id);
    set_state("uploading");
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        g_shared.status.last_file = item.path;
        g_shared.status.destination = destination->id;
    }
    g_progress.cancel = false;
    g_log.write("sending %s (%s, %s) to %s", item.file.c_str(), game.c_str(),
                human_size(item.size).c_str(), destination->id.c_str());
    // Read back as zeros here: send a copy the app hands over, when it runs.
    MediaItem source = item;
    std::string copy;
    if (capture_reads_blank(item.path) && fetch_through_app(paths, item, &copy))
        source.path = copy;
    const UploadOutcome outcome = upload_capture(config, *destination, source, game, &g_progress);
    if (!copy.empty())
        ::unlink(copy.c_str());
    g_progress.sent = 0;
    g_progress.total = 0;
    if (result != nullptr)
        *result = outcome;

    HistoryEntry entry;
    entry.time = now_seconds();
    entry.ok = outcome.ok;
    entry.automatic = automatic;
    entry.destination = outcome.destination;
    entry.path = item.path;
    entry.detail = outcome.ok ? outcome.link : outcome.error;
    history->append(entry);

    const Destination *used = config.find(outcome.destination);
    const std::string where = used != nullptr ? used->display_name() : destination->display_name();
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        if (outcome.ok)
        {
            ++g_shared.status.uploaded;
            g_shared.status.last_link = outcome.link;
            g_shared.status.last_error.clear();
        }
        else
        {
            ++g_shared.status.failed;
            g_shared.status.last_error = outcome.error;
        }
        g_shared.status.state = g_shared.paused ? "paused" : "watching";
        g_shared.status.destination.clear();
    }
    if (outcome.ok)
        g_log.write("sent %s -> %s", item.file.c_str(), outcome.link.c_str());
    else
        g_log.write("failed %s: %s", item.file.c_str(), outcome.error.c_str());
    if (config.notify && !quiet)
    {
        if (outcome.ok)
            notify(std::string("PShare: ") + item.kind_label() +
                   (used != nullptr && used->local() ? " saved to " : " sent to ") + where);
        else
            notify("PShare: upload failed - " + outcome.error.substr(0, 120));
    }
    (void)paths;
    return outcome.ok ? outcome.destination : std::string();
}

// Where a job the app handed over goes: the destination it names, or the one
// the config picks for the capture's game.
const Destination *job_destination(const Config &config, const ManualJob &job,
                                   const MediaItem &item)
{
    return job.destination.empty() ? config.destination_for(item.title_id)
                                   : config.find(job.destination);
}

// Sends screenshots of a Send all as one Telegram album (upload_album) and
// writes what send_one writes for each: history, counters, log.
UploadOutcome send_album(const Config &config, TitleNames *names, History *history,
                         const Destination &destination, const std::vector<MediaItem> &items)
{
    std::vector<std::string> games;
    for (const MediaItem &item : items)
        games.push_back(names->display(item.title_id));
    set_state("uploading");
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        g_shared.status.last_file = items.front().path;
        g_shared.status.destination = destination.id;
    }
    g_progress.cancel = false;
    g_log.write("sending an album of %zu captures to %s", items.size(), destination.id.c_str());
    const UploadOutcome outcome = upload_album(config, destination, items, games, &g_progress);
    g_progress.sent = 0;
    g_progress.total = 0;
    for (const MediaItem &item : items)
    {
        HistoryEntry entry;
        entry.time = now_seconds();
        entry.ok = outcome.ok;
        entry.destination = destination.id;
        entry.path = item.path;
        entry.detail = outcome.ok ? outcome.link : outcome.error;
        history->append(entry);
    }
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        const int count = static_cast<int>(items.size());
        (outcome.ok ? g_shared.status.uploaded : g_shared.status.failed) += count;
        g_shared.status.last_error = outcome.ok ? std::string() : outcome.error;
        g_shared.status.state = g_shared.paused ? "paused" : "watching";
        g_shared.status.destination.clear();
    }
    if (outcome.ok)
        g_log.write("sent the album of %zu", items.size());
    else
        g_log.write("album failed: %s", outcome.error.c_str());
    return outcome;
}

// The destinations in also_send that have not had this capture yet.
void send_mirrors(const Paths &paths, const Config &config, TitleNames *names, History *history,
                  const MediaItem &item, const std::string &already)
{
    for (const std::string &id : config.also_send)
    {
        const Destination *mirror = config.find(id);
        if (mirror == nullptr || !mirror->enabled || !mirror->configured() || iequals(id, already))
            continue;
        send_one(paths, config, names, history, item, true, mirror->id);
        if (g_quit.load())
            return;
    }
}

int run()
{
    std::signal(SIGPIPE, SIG_IGN);
    const Paths paths = paths_from_environment();
    const int control_port = port_from_environment("PSHARE_CONTROL_PORT", kControlPort);
    const int setup_port = port_from_environment("PSHARE_SETUP_PORT", kSetupPort);
    ensure_directory(paths.data_root);
    g_log.open(paths.daemon_log());

    int control = listen_tcp(0x7f000001, control_port, 8);
    if (control < 0)
    {
        // A copy is running already. The same build stays; an older one is
        // asked to stop, so installing a new PShare also updates its uploader.
        std::string reply;
        if (daemon_command("PING", &reply, 800, control_port))
        {
            const std::vector<std::string> words = split(trim(reply), ' ');
            if (words.size() >= 4 && words[3] == build_id())
            {
                g_log.write("pshared is already running; this copy exits");
                return 0;
            }
            g_log.write("replacing the running pshared (%s)", std::string(trim(reply)).c_str());
            daemon_command("QUIT", nullptr, 800, control_port);
            for (int i = 0; i < 100 && control < 0; ++i)
            {
                usleep(100000);
                control = listen_tcp(0x7f000001, control_port, 8);
            }
        }
        if (control < 0)
        {
            g_log.write("cannot listen on 127.0.0.1:%d (errno %d)", control_port, errno);
            return 0;
        }
    }
    socket_timeouts(control, 1000);
    g_log.write("pshared %s (%s) started (pid %d)", kVersion, build_id(),
                static_cast<int>(getpid()));

    upload_global_init();
    Settings settings;
    load_settings(paths, &settings);
    Watcher watcher(paths);
    watcher.configure(settings.config);
    watcher.load(now_seconds());
    watcher.save();
    History history(paths.history());
    TitleNames names(paths.appmeta_root, {paths.photo_root, paths.video_root});
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        g_shared.status.reachable = true;
        g_shared.status.version = kVersion;
        g_shared.status.build = build_id();
        g_shared.status.started = now_seconds();
        g_shared.status.state = "starting";
    }

    std::thread control_worker(control_thread, control);
    std::thread setup_worker(setup_thread, paths, setup_port);

    if (settings.config.notify)
        notify("PShare is watching for new captures");

    std::deque<MediaItem> queue;
    std::int64_t next_poll = 0;
    while (!g_quit.load())
    {
        bool reload = false;
        bool rescan = false;
        bool paused = false;
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            reload = g_shared.reload;
            rescan = g_shared.rescan;
            paused = g_shared.paused;
            g_shared.reload = g_shared.rescan = false;
        }
        if (reload || file_mtime(paths.config()) != settings.mtime)
        {
            load_settings(paths, &settings);
            watcher.configure(settings.config);
            g_log.write("configuration reloaded");
        }
        const Config &config = settings.config;

        // Captures the app asked for go first and ignore auto_upload and pause.
        // One at a time off the shared queue, so a CANCEL (which empties it)
        // stops a whole "Send all".
        while (!g_quit.load())
        {
            // A Send all arrives one QUEUE at a time: let it land (half a second
            // without a new one, three at most), so its first album fills up too.
            for (int tries = 0; tries < 30 && !g_quit.load(); ++tries)
            {
                {
                    std::lock_guard<std::mutex> lock(g_shared.mutex);
                    if (g_shared.manual.empty() || !g_shared.manual.front().batch ||
                        g_shared.manual.size() >= kAlbumSize ||
                        std::chrono::steady_clock::now() - g_shared.last_queued >=
                            std::chrono::milliseconds(500))
                        break;
                }
                usleep(100000);
            }
            // The next job, and with a Send all of screenshots for one Telegram
            // chat, up to kAlbumSize of them to go as one album.
            std::vector<ManualJob> jobs;
            std::vector<MediaItem> items;
            const Destination *album = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_shared.mutex);
                if (g_shared.manual.empty())
                    break;
                jobs.push_back(std::move(g_shared.manual.front()));
                g_shared.manual.pop_front();
                g_shared.current_job = jobs.front().id;
                MediaItem first;
                const Destination *to =
                    jobs.front().batch && describe_media(jobs.front().path, &first)
                        ? job_destination(config, jobs.front(), first)
                        : nullptr;
                // A clip this process reads as zeros goes alone, through the app (send_one).
                const auto groupable = [&](const MediaItem &item)
                {
                    return to != nullptr && to->configured() &&
                                   !(item.video() && capture_reads_blank(item.path))
                               ? album_kind(*to, item)
                               : AlbumKind::none;
                };
                const AlbumKind kind = groupable(first);
                if (kind != AlbumKind::none)
                {
                    items.push_back(first);
                    std::uint64_t total = first.size;
                    const std::uint64_t limit = to->size_limit();
                    while (jobs.size() < kAlbumSize && !g_shared.manual.empty())
                    {
                        const ManualJob &next = g_shared.manual.front();
                        MediaItem more;
                        if (!next.batch || !describe_media(next.path, &more) ||
                            job_destination(config, next, more) != to || groupable(more) != kind ||
                            (limit != 0 && total + more.size > limit))
                            break;
                        total += more.size;
                        jobs.push_back(std::move(g_shared.manual.front()));
                        g_shared.manual.pop_front();
                        items.push_back(std::move(more));
                    }
                    if (jobs.size() > 1)
                        album = to;
                }
                // Why a Send all capture for Telegram goes on its own, for the log.
                if (album == nullptr && to != nullptr && to->type == DestinationType::telegram &&
                    jobs.front().batch)
                    g_log.write("%s goes alone (%s, %s): %s", first.file.c_str(),
                                kind == AlbumKind::photo  ? "photo"
                                : kind == AlbumKind::file ? "file"
                                                          : "no album",
                                human_size(first.size).c_str(),
                                kind == AlbumKind::none ? "over the size limit or unreadable"
                                                        : "nothing like it queued after it");
                for (const ManualJob &each : jobs)
                    g_shared.sending.push_back(each.id);
            }
            std::vector<UploadOutcome> outcomes(jobs.size());
            if (album != nullptr)
            {
                const UploadOutcome sent = send_album(config, &names, &history, *album, items);
                for (UploadOutcome &outcome : outcomes)
                    outcome = sent;
            }
            else
            {
                const ManualJob &job = jobs.front();
                MediaItem item;
                if (describe_media(job.path, &item))
                    send_one(paths, config, &names, &history, item, false, job.destination,
                             &outcomes.front(), job.batch);
                else
                    outcomes.front().error = "The capture is gone";
                items = {item};
            }
            for (std::size_t i = 0; i < jobs.size(); ++i)
            {
                const ManualJob &job = jobs[i];
                const MediaItem &item = items[i];
                UploadOutcome &outcome = outcomes[i];
                if (outcome.destination.empty())
                    outcome.destination = job.destination;
                // Deleted only once it has really gone somewhere. A payload may not
                // be allowed to delete captures; then the app does it when it runs.
                bool waiting = false;
                if (outcome.ok && job.delete_after)
                {
                    std::string why;
                    if (delete_capture(job.path, paths.photo_root, paths.video_root, &why))
                    {
                        g_log.write("deleted %s after sending it", item.file.c_str());
                    }
                    else
                    {
                        waiting = add_pending_delete(paths, job.path);
                        g_log.write("could not delete %s (%s); %s", item.file.c_str(), why.c_str(),
                                    waiting ? "PShare deletes it when it runs"
                                            : "and could not list it for PShare either");
                    }
                }
                std::string answer = outcome.ok ? "ok " : "failed ";
                answer += outcome.destination + "\t" + (outcome.ok ? outcome.link : outcome.error);
                for (char &c : answer)
                    if (c == '\n' || c == '\r')
                        c = ' ';
                BatchTally finished;
                bool batch_over = false;
                {
                    std::lock_guard<std::mutex> lock(g_shared.mutex);
                    g_shared.current_job = 0;
                    g_shared.sending.erase(
                        std::remove(g_shared.sending.begin(), g_shared.sending.end(), job.id),
                        g_shared.sending.end());
                    g_shared.results[job.id] = std::move(answer);
                    while (g_shared.results.size() > 64)
                        g_shared.results.erase(g_shared.results.begin());
                    if (job.batch)
                    {
                        BatchTally &tally = g_shared.batch;
                        ++(outcome.ok ? tally.sent : tally.failed);
                        if (outcome.ok && job.delete_after)
                            ++(waiting ? tally.waiting : tally.deleted);
                    }
                    batch_over =
                        g_shared.manual.empty() && g_shared.batch.sent + g_shared.batch.failed > 0;
                    if (batch_over)
                    {
                        finished = g_shared.batch;
                        g_shared.batch = BatchTally{};
                    }
                }
                if (batch_over && config.notify)
                {
                    std::string text = "PShare: sent " + std::to_string(finished.sent) +
                                       (finished.sent == 1 ? " capture" : " captures");
                    if (finished.failed > 0)
                        text += ", " + std::to_string(finished.failed) + " failed";
                    if (finished.deleted > 0)
                        text +=
                            ", " + std::to_string(finished.deleted) + " deleted from the console";
                    if (finished.waiting > 0)
                        text += ", " + std::to_string(finished.waiting) +
                                " to delete when PShare opens";
                    notify(text);
                }
            }
        }

        const std::int64_t now = now_seconds();
        if (rescan || now >= next_poll)
        {
            next_poll = now + std::clamp(config.poll_seconds, 1, 300);
            for (MediaItem &item : watcher.poll(now))
                queue.push_back(std::move(item));
            const std::string ip = local_ip_address();
            {
                std::lock_guard<std::mutex> lock(g_shared.mutex);
                g_shared.status.ip = ip;
                g_shared.status.watch_errno = watcher.last_errno();
                g_shared.status.queued = static_cast<int>(queue.size() + watcher.pending());
                if (g_shared.status.state == "starting")
                    g_shared.status.state = paused ? "paused" : "watching";
            }
        }

        if (!queue.empty() && !paused && config.auto_upload)
        {
            MediaItem item = std::move(queue.front());
            queue.pop_front();
            history.refresh();
            // Skipped when the app already sent it.
            if (!history.uploaded(item.path) && config.upload_allowed(item.title_id, item.video()))
            {
                const std::string went = send_one(paths, config, &names, &history, item, true);
                send_mirrors(paths, config, &names, &history, item, went);
            }
            watcher.mark_seen(item.path);
            watcher.save();
            continue; // straight to the next one
        }
        if (!queue.empty() && !config.auto_upload)
        {
            // Off: noticed, not sent; the app can still send them by hand.
            for (const MediaItem &item : queue)
                watcher.mark_seen(item.path);
            queue.clear();
            watcher.save();
        }
        usleep(250000);
    }

    watcher.save();
    shutdown(control, SHUT_RDWR);
    close(control);
    control_worker.join();
    setup_worker.join();
    g_log.write("pshared stopped");
    return 0;
}
} // namespace
} // namespace pshare

int main()
{
    return pshare::run();
}
