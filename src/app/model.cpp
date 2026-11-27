// PShare - What the app knows and does, without any drawing.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/model.hpp"

#include "pshare/core/text.hpp"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <map>

namespace pshare::app
{
namespace
{
constexpr const char *kAppSection = "app";
// [app] background_sends: uploads go through pshared when it can run.
constexpr const char *kBackgroundSendsFlag = "background_sends";
constexpr int kRescanSeconds = 20;

std::int64_t now_seconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}
} // namespace

Model::Model(Environment environment)
    : environment_(std::move(environment)), history_(environment_.paths.history()),
      names_(environment_.paths.appmeta_root,
             {environment_.paths.photo_root, environment_.paths.video_root}),
      worker_names_(environment_.paths.appmeta_root,
                    {environment_.paths.photo_root, environment_.paths.video_root})
{
}

Model::~Model()
{
    quit_ = true;
    progress_.cancel = true;
    {
        Lock lock(mutex_);
        io_wake_.notify_all();
        upload_wake_.notify_all();
    }
    io_thread_.join();
    upload_thread_.join();
    status_thread_.join();
    for (Thread &relay : relay_threads_)
        relay.join();
}

void Model::start()
{
    if (environment_.elevated)
        ensure_directory(environment_.paths.data_root);
    load_config_file(environment_.paths.config(), &document_, &config_, &warnings_);
    history_.refresh();
    local_ip_ = environment_.demo ? "192.168.1.42" : local_ip_address();
    upload_global_init();
    io_thread_.start([this] { io_loop(); });
    upload_thread_.start([this] { upload_loop(); });
    status_thread_.start([this] { status_loop(); }, 256u << 10);
    if (!environment_.demo)
        for (Thread &relay : relay_threads_)
            relay.start([this] { relay_loop(); }, 256u << 10);
    rescan();
    delete_pending(); // sent while the app was closed
}

// Since a console restart, payloads (pshared, an FTP server) can read video
// captures back as zeros while the app reads them properly. While the app
// runs, pshared asks it for the bytes a browser wants (net.hpp, relay_once).
void Model::relay_loop()
{
    const std::string photo = environment_.paths.photo_root + "/";
    const std::string video = environment_.paths.video_root + "/";
    const auto read =
        [&](const std::string &path, std::uint64_t first, std::uint64_t last, const RelaySend &send)
    {
        // Captures only, and nothing outside their folders.
        if (path.find("/../") != std::string::npos ||
            !(starts_with(path, photo) || starts_with(path, video)))
            return false;
        const int file = ::open(path.c_str(), O_RDONLY);
        if (file < 0)
            return false;
        std::vector<char> buffer(256 * 1024);
        std::uint64_t at = first;
        bool ok = true;
        while (ok && at <= last && !quit_.load())
        {
            const std::size_t want =
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), last - at + 1));
            const ssize_t got = pread(file, buffer.data(), want, static_cast<off_t>(at));
            if (got <= 0)
                break;
            ok = send(std::string_view(buffer.data(), static_cast<std::size_t>(got)));
            at += static_cast<std::uint64_t>(got);
        }
        close(file);
        return ok;
    };
    while (!quit_.load())
    {
        if (relay_once(4000, read))
            continue;
        // pshared is not running: look again in a moment.
        for (int i = 0; i < 10 && !quit_.load(); ++i)
            usleep(100000);
    }
}

void Model::post(Result result)
{
    Lock lock(mutex_);
    results_.push_back(std::move(result));
}

void Model::io(Task task)
{
    Lock lock(mutex_);
    io_tasks_.push_back(std::move(task));
    io_wake_.notify_one();
}

std::vector<ModelEvent> Model::pump()
{
    std::deque<Result> results;
    {
        Lock lock(mutex_);
        results.swap(results_);
    }
    for (Result &result : results)
        result(*this);
    std::vector<ModelEvent> out;
    out.swap(events_);
    return out;
}

// ---- workers ---------------------------------------------------------------

void Model::io_loop()
{
    for (;;)
    {
        Task task;
        {
            Lock lock(mutex_);
            io_wake_.wait(lock, [this] { return quit_.load() || !io_tasks_.empty(); });
            if (quit_.load())
                return;
            task = std::move(io_tasks_.front());
            io_tasks_.pop_front();
        }
        task();
    }
}

void Model::status_loop()
{
    std::int64_t last_scan = now_seconds();
    while (!quit_.load())
    {
        if (poll_daemon_.load())
        {
            DaemonStatus status;
            if (environment_.demo)
            {
                if (demo_daemon_running_.load())
                {
                    status.reachable = true;
                    status.version = kVersion;
                    status.state = demo_daemon_paused_.load() ? "paused" : "watching";
                    status.uploaded = 27;
                    status.failed = 1;
                    status.started = now_seconds() - 3 * 3600 - 17 * 60;
                    status.last_link = "https://files.catbox.moe/k3xq9a.jpg";
                    if (demo_setup_.load())
                    {
                        status.setup_port = kSetupPort;
                    }
                }
            }
            else
            {
                status = daemon_status(0, true);
            }
            // A Send all in pshared is over once its last job has an answer.
            std::string answer;
            const int last = batch_last_.load();
            const bool over = last > 0 &&
                              daemon_command("RESULT " + std::to_string(last), &answer, 800) &&
                              trim(answer) != "pending";
            if (over)
                batch_last_ = 0;
            const std::string note = trim(answer) == "unknown"
                                         ? "The background uploader restarted on the way"
                                         : std::string();
            post(
                [status, over, note](Model &model)
                {
                    model.daemon_ = status;
                    if (status.delete_ask > 0 && !model.delete_request_busy_)
                        model.fetch_delete_request();
                    Batch &batch = model.batch_;
                    if (!batch.active || batch.last_id == 0)
                        return;
                    if (status.job >= batch.first_id && status.job <= batch.last_id)
                        batch.done = status.job - batch.first_id;
                    if (over)
                    {
                        batch.daemon_done = true;
                        batch.done = batch.last_id - batch.first_id + 1;
                        if (!model.app_batch_pending())
                            model.finish_batch(note);
                    }
                });
        }
        if (now_seconds() - last_scan >= kRescanSeconds)
        {
            last_scan = now_seconds();
            post(
                [](Model &model)
                {
                    if (!model.scanning_)
                        model.rescan();
                    model.delete_pending();
                });
        }
        for (int i = 0; i < 10 && !quit_.load(); ++i)
            usleep(100000);
    }
}

void Model::upload_loop()
{
    for (;;)
    {
        UploadJob job;
        Config config;
        {
            Lock lock(mutex_);
            upload_wake_.wait(lock, [this] { return quit_.load() || !upload_jobs_.empty(); });
            if (quit_.load())
                return;
            job = std::move(upload_jobs_.front());
            upload_jobs_.pop_front();
            config = std::move(upload_configs_.front());
            upload_configs_.pop_front();
            current_ = job;
            uploading_ = true;
            progress_.cancel = false;
            progress_.sent = 0;
            progress_.total = job.size;
            progress_.stage = 1;
        }
        UploadOutcome outcome;
        MediaItem item;
        bool by_daemon = false;
        const Destination *destination = config.find(job.destination);
        if (!describe_media(job.path, &item))
        {
            outcome.error = "The capture is gone";
        }
        else if (destination == nullptr)
        {
            outcome.error = "No destination is set up";
        }
        else if (environment_.demo)
        {
            outcome = upload_capture_with(
                config, *destination, item, job.game, &progress_,
                [](const HttpRequest &request, UploadProgress *progress, void *self)
                { return static_cast<Model *>(self)->demo_transport(request, progress); }, this,
                false);
        }
        else if (job.background && send_through_daemon(job, &outcome))
        {
            by_daemon = true; // and pshared wrote the history line
        }
        else
        {
            outcome = upload_capture(config, *destination, item, job.game, &progress_);
        }
        HistoryEntry entry;
        entry.time = now_seconds();
        entry.ok = outcome.ok;
        entry.automatic = false;
        entry.destination = outcome.destination.empty() ? job.destination : outcome.destination;
        entry.path = job.path;
        entry.detail = outcome.ok ? outcome.link : outcome.error;
        if (environment_.elevated && !by_daemon)
        {
            History writer(environment_.paths.history());
            writer.append(entry);
        }
        // Sent by the app itself: it deletes it too (pshared did, when it sent it).
        // One that cannot be deleted stays in the gallery, where it shows as sent.
        if (outcome.ok && job.delete_after && !by_daemon && !environment_.demo)
            (void)delete_capture(job.path, environment_.paths.photo_root,
                                 environment_.paths.video_root, nullptr);
        {
            Lock lock(mutex_);
            uploading_ = false;
            progress_.stage = 0;
        }
        post(
            [job, outcome, entry](Model &model)
            {
                model.history_.refresh();
                if (!model.environment_.elevated)
                    model.history_.append(entry); // nowhere to keep it but here
                model.mark_uploads();
                // A Send all says how it went once, at the end.
                if (job.batch)
                {
                    if (model.batch_.active && !model.app_batch_pending() &&
                        (model.batch_.last_id == 0 || model.batch_.daemon_done))
                        model.finish_batch({});
                    return;
                }
                ModelEvent event;
                event.kind = EventKind::upload_finished;
                event.ok = outcome.ok;
                const Destination *used = model.config_.find(entry.destination);
                const std::string where =
                    used != nullptr ? used->display_name() : job.destination_name;
                event.title = outcome.ok
                                  ? (job.video ? "Clip sent to " : "Screenshot sent to ") + where
                                  : "Upload failed";
                event.detail =
                    outcome.ok ? (outcome.link.empty() ? job.game : outcome.link) : outcome.error;
                event.link = outcome.link;
                model.events_.push_back(std::move(event));
            });
    }
}

bool Model::ensure_daemon(std::string *why)
{
    std::string reply;
    if (daemon_command("PING", &reply, 600))
        return true;
    std::string elf;
    if (!read_text_file(environment_.app_root + "/pshared.elf", &elf, 64u << 20))
    {
        if (why)
            *why = "pshared.elf is missing from the app folder";
        return false;
    }
    if (!send_to_loader(elf, why))
        return false;
    for (int i = 0; i < 40 && !quit_.load(); ++i)
    {
        usleep(150000);
        if (daemon_command("PING", &reply, 400))
            return true;
    }
    if (why)
        *why = "pshared did not answer after starting";
    return false;
}

bool Model::send_through_daemon(const UploadJob &job, UploadOutcome *outcome)
{
    std::string why;
    if (!ensure_daemon(&why))
        return false;
    std::string reply;
    // pshared deletes it after sending when asked, so that holds if the app closes.
    if (!daemon_command("QUEUE " + job.destination + "\t" + job.path +
                            (job.delete_after ? "\tdelete" : ""),
                        &reply, 2000) ||
        !starts_with(reply, "ok "))
        return false;
    const int id = static_cast<int>(parse_int(trim(std::string_view(reply).substr(3)), 0));
    if (id <= 0)
        return false; // an older pshared: it queued it, but cannot say how it went
    bool cancelled = false;
    std::int64_t last_answer = now_seconds();
    while (!quit_.load())
    {
        usleep(250000);
        if (progress_.cancel.load() && !cancelled)
        {
            cancelled = true;
            daemon_command("CANCEL", nullptr);
        }
        std::string answer;
        if (!daemon_command("RESULT " + std::to_string(id), &answer, 800))
        {
            if (now_seconds() - last_answer > 15)
            {
                outcome->error = "The background uploader stopped answering";
                return true;
            }
            continue;
        }
        last_answer = now_seconds();
        answer = std::string(trim(answer));
        if (answer == "pending")
        {
            const DaemonStatus status = daemon_status();
            if (status.job == id)
            {
                progress_.total = status.total > 0 ? status.total : job.size;
                progress_.sent = status.sent;
            }
            continue;
        }
        if (answer == "unknown")
        {
            outcome->error = "The background uploader restarted; try again";
            return true;
        }
        const bool ok = starts_with(answer, "ok ");
        const std::string rest = answer.substr(answer.find(' ') + 1);
        const std::size_t tab = rest.find('\t');
        outcome->ok = ok;
        outcome->destination = tab == std::string::npos ? job.destination : rest.substr(0, tab);
        const std::string detail = tab == std::string::npos ? rest : rest.substr(tab + 1);
        (ok ? outcome->link : outcome->error) = detail;
        return true;
    }
    outcome->error = "Cancelled";
    return true;
}

HttpResult Model::demo_transport(const HttpRequest &request, UploadProgress *progress)
{
    HttpResult result;
    const std::uint64_t total = request.upload_size > 0 ? request.upload_size : 400000;
    for (int step = 1; step <= 40; ++step)
    {
        if (progress != nullptr)
        {
            if (progress->cancel.load())
            {
                result.error = "Cancelled";
                return result;
            }
            progress->total = total;
            progress->sent = total * static_cast<std::uint64_t>(step) / 40;
        }
        usleep(50000);
    }
    result.transport_ok = true;
    result.status = 200;
    if (request.url.find("discord") != std::string::npos)
        result.body =
            R"({"id":"1","attachments":[{"url":"https://cdn.discordapp.com/attachments/1/2/capture.jpg"}]})";
    else if (request.url.find("telegram") != std::string::npos)
        result.body = R"({"ok":true})";
    else if (request.url.find("catbox") != std::string::npos)
        result.body = "https://files.catbox.moe/p5h4r3.jpg";
    else
        result.body = R"({"url":"https://example.net/u/p5h4r3"})";
    return result;
}

// ---- captures --------------------------------------------------------------

void Model::rescan()
{
    if (scanning_)
        return;
    scanning_ = true;
    io([this] { scan_now(); });
}

void Model::scan_now()
{
    ScanOptions options;
    ScanResult scan = scan_media(environment_.paths, options);
    std::vector<Capture> captures;
    captures.reserve(scan.items.size());
    for (MediaItem &item : scan.items)
    {
        Capture capture;
        capture.game = worker_names_.display(item.title_id);
        // Read once per file and size: three small samples of it.
        auto &checked = blank_checked_[item.path];
        if (checked.first != item.size + 1)
            checked = {item.size + 1, capture_reads_blank(item.path)};
        capture.blank = checked.second;
        capture.item = std::move(item);
        captures.push_back(std::move(capture));
    }
    const int error = (scan.photo_root || scan.video_root) ? 0 : scan.open_errno;
    History fresh(environment_.paths.history());
    fresh.refresh();
    post(
        [captures = std::move(captures), error, fresh = std::move(fresh)](Model &model) mutable
        {
            const bool first = !model.scanned_once_;
            const std::size_t before = model.captures_.size();
            model.captures_ = std::move(captures);
            if (model.environment_.elevated)
                model.history_ = std::move(fresh);
            model.scanning_ = false;
            model.scanned_once_ = true;
            model.scan_errno_ = error;
            model.mark_uploads();
            model.count_kinds();
            if (first || model.captures_.size() != before)
            {
                ModelEvent event;
                event.kind = EventKind::scanned;
                model.events_.push_back(std::move(event));
            }
            if (!model.unreadable_unasked().empty())
            {
                ModelEvent event;
                event.kind = EventKind::unreadable;
                model.events_.push_back(std::move(event));
            }
        });
}

std::vector<std::string> Model::unreadable_unasked() const
{
    std::vector<std::string> out;
    for (const Capture &capture : captures_)
        if (capture.blank && std::find(unreadable_asked_.begin(), unreadable_asked_.end(),
                                       capture.item.path) == unreadable_asked_.end())
            out.push_back(capture.item.path);
    return out;
}

void Model::mark_unreadable_asked(const std::vector<std::string> &paths)
{
    for (const std::string &path : paths)
        if (std::find(unreadable_asked_.begin(), unreadable_asked_.end(), path) ==
            unreadable_asked_.end())
            unreadable_asked_.push_back(path);
}

void Model::delete_captures(std::vector<std::string> paths)
{
    if (paths.empty())
        return;
    const Paths where = environment_.paths;
    const bool demo = environment_.demo;
    io(
        [this, paths = std::move(paths), where, demo]
        {
            int deleted = 0;
            std::string error;
            for (const std::string &path : paths)
            {
                std::string why;
                if (demo || delete_capture(path, where.photo_root, where.video_root, &why))
                    ++deleted;
                else if (error.empty())
                    error = why;
            }
            const int total = static_cast<int>(paths.size());
            post(
                [deleted, total, error](Model &model)
                {
                    ModelEvent event;
                    event.kind = EventKind::deleted;
                    event.ok = deleted == total;
                    event.title = deleted == 1  ? std::string("Deleted 1 capture")
                                  : deleted > 0 ? "Deleted " + std::to_string(deleted) + " captures"
                                                : std::string("Nothing was deleted");
                    event.detail = error;
                    model.events_.push_back(std::move(event));
                    model.rescan();
                });
        });
}

void Model::delete_pending()
{
    if (environment_.demo || !environment_.elevated)
        return;
    const Paths where = environment_.paths;
    io(
        [this, where]
        {
            const int deleted = delete_pending_captures(where);
            if (deleted == 0)
                return;
            post(
                [deleted](Model &model)
                {
                    ModelEvent event;
                    event.kind = EventKind::deleted;
                    event.title = "Deleted " + std::to_string(deleted) +
                                  (deleted == 1 ? " capture" : " captures");
                    event.detail = "Sent with \"Delete once sent\"";
                    model.events_.push_back(std::move(event));
                    model.rescan();
                });
        });
}

void Model::fetch_delete_request()
{
    delete_request_busy_ = true;
    io(
        [this]
        {
            std::string reply;
            std::vector<std::string> paths;
            if (daemon_command("DELETEASK", &reply, 2000) && starts_with(reply, "ok"))
                for (const std::string &line : split(reply, '\n'))
                    if (starts_with(line, "/"))
                        paths.emplace_back(trim(line));
            post(
                [paths](Model &model)
                {
                    // Only captures the app knows; the rest is not its to delete.
                    model.delete_request_.clear();
                    for (const std::string &path : paths)
                        if (model.find_capture(path) != nullptr)
                            model.delete_request_.push_back(path);
                    if (model.delete_request_.empty())
                    {
                        model.answer_delete_request(false);
                        return;
                    }
                    ModelEvent event;
                    event.kind = EventKind::delete_asked;
                    model.events_.push_back(std::move(event));
                });
        });
}

void Model::answer_delete_request(bool allow)
{
    if (allow)
        delete_captures(delete_request_);
    delete_request_.clear();
    io(
        [this]
        {
            daemon_command("DELETEASK clear", nullptr);
            post([](Model &model) { model.delete_request_busy_ = false; });
        });
}

void Model::mark_uploads()
{
    std::map<std::string, const HistoryEntry *> last;
    for (const HistoryEntry &entry : history_.entries())
        last[entry.path] = &entry; // oldest first: the last one wins
    for (Capture &capture : captures_)
    {
        const HistoryEntry *success = history_.last_success(capture.item.path);
        capture.uploaded = success != nullptr;
        capture.link = success != nullptr ? success->detail : std::string();
        const auto found = last.find(capture.item.path);
        capture.failed = found != last.end() && !found->second->ok;
    }
}

void Model::count_kinds()
{
    screenshot_count_ = 0;
    clip_count_ = 0;
    for (const Capture &capture : captures_)
        (capture.item.video() ? clip_count_ : screenshot_count_)++;
}

std::vector<int> Model::filtered(Filter filter) const
{
    std::vector<int> out;
    out.reserve(captures_.size());
    for (std::size_t i = 0; i < captures_.size(); ++i)
    {
        const bool video = captures_[i].item.video();
        if (filter == Filter::all || (filter == Filter::clips) == video)
            out.push_back(static_cast<int>(i));
    }
    return out;
}

std::vector<int> Model::filtered(Filter filter, const std::string &title_id,
                                 bool oldest_first) const
{
    std::vector<int> out;
    for (int index : filtered(filter))
        if (title_id.empty() ||
            captures_[static_cast<std::size_t>(index)].item.title_id == title_id)
            out.push_back(index);
    if (oldest_first)
        std::reverse(out.begin(), out.end()); // the scan is newest first
    return out;
}

std::vector<Model::GameCount> Model::games() const
{
    std::map<std::string, GameCount> by_id;
    for (const Capture &capture : captures_)
    {
        GameCount &game = by_id[capture.item.title_id];
        game.title_id = capture.item.title_id;
        game.name = capture.game;
        (capture.item.video() ? game.clips : game.screenshots)++;
    }
    std::vector<GameCount> out;
    for (auto &[id, game] : by_id)
        out.push_back(std::move(game));
    std::sort(out.begin(), out.end(),
              [](const GameCount &a, const GameCount &b)
              {
                  const int na = a.screenshots + a.clips;
                  const int nb = b.screenshots + b.clips;
                  return na != nb ? na > nb : a.name < b.name;
              });
    return out;
}

const Capture *Model::find_capture(const std::string &path) const
{
    for (const Capture &capture : captures_)
        if (capture.item.path == path)
            return &capture;
    return nullptr;
}

std::string Model::game_name(const std::string &title_id)
{
    return names_.display(title_id);
}

std::string Model::game_icon(const std::string &title_id) const
{
    return names_.icon_path(title_id);
}

// ---- configuration ---------------------------------------------------------

bool Model::app_flag(const char *key, bool fallback) const
{
    return document_.get_bool(kAppSection, key, fallback);
}

void Model::set_app_flag(const char *key, bool value)
{
    document_.set_bool(kAppSection, key, value);
}

void Model::save_config()
{
    store_config(config_, &document_);
    std::string text = document_.write();
    const std::string path = environment_.paths.config();
    const bool demo = environment_.demo;
    const bool elevated = environment_.elevated;
    io(
        [this, text = std::move(text), path, demo, elevated]
        {
            bool ok = demo;
            if (!demo && elevated)
            {
                ensure_directory(environment_.paths.data_root);
                ok = write_text_file_atomic(path, text);
            }
            if (ok && !demo)
                daemon_command("RELOAD", nullptr);
            post(
                [ok](Model &model)
                {
                    ModelEvent event;
                    event.kind = ok ? EventKind::config_saved : EventKind::config_failed;
                    event.ok = ok;
                    event.title = ok ? "Settings saved" : "Could not save the settings";
                    model.events_.push_back(std::move(event));
                });
        });
}

// ---- uploads ---------------------------------------------------------------

bool Model::upload(const std::string &path, const std::string &destination_id, std::string *why)
{
    const Capture *capture = find_capture(path);
    if (capture == nullptr)
    {
        if (why)
            *why = "That capture is gone";
        return false;
    }
    if (capture->blank)
    {
        if (why)
            *why = "The console reads this capture back as empty, so it can't be sent";
        return false;
    }
    const Destination *destination = destination_id.empty()
                                         ? config_.destination_for(capture->item.title_id)
                                         : config_.find(destination_id);
    if (destination == nullptr || !destination->configured())
    {
        if (why)
            *why = destination == nullptr ? "Set up a destination first"
                                          : destination->display_name() + " is not set up yet";
        return false;
    }
    return queue_job(*capture, *destination, false, false, why);
}

bool Model::queue_job(const Capture &capture, const Destination &destination, bool delete_after,
                      bool batch, std::string *why)
{
    const std::string &path = capture.item.path;
    UploadJob job;
    job.id = next_job_++;
    job.path = path;
    job.file = capture.item.file;
    job.game = capture.game;
    job.destination = destination.id;
    job.destination_name = destination.display_name();
    job.video = capture.item.video();
    job.size = capture.item.size;
    job.background = !environment_.demo && app_flag(kBackgroundSendsFlag, true);
    job.delete_after = delete_after;
    job.batch = batch;
    Lock lock(mutex_);
    if ((uploading_ && current_.path == path) ||
        std::any_of(upload_jobs_.begin(), upload_jobs_.end(),
                    [&](const UploadJob &queued) { return queued.path == path; }))
    {
        if (why)
            *why = "Already on its way";
        return false;
    }
    upload_jobs_.push_back(std::move(job));
    upload_configs_.push_back(config_);
    upload_wake_.notify_one();
    return true;
}

int Model::send_batch(const std::vector<std::string> &paths, const std::string &destination_id,
                      bool delete_after, std::string *why)
{
    const auto fail = [&](std::string reason)
    {
        if (why)
            *why = std::move(reason);
        return 0;
    };
    if (batch_.active)
        return fail("A Send all is already going; cancel it first");
    const Destination *chosen = destination_id.empty() ? nullptr : config_.find(destination_id);
    if (!destination_id.empty() && (chosen == nullptr || !chosen->configured()))
        return fail(chosen == nullptr ? "That destination is gone"
                                      : chosen->display_name() + " is not set up yet");
    // What can go: there, not read back as zeros, and with somewhere to go.
    std::vector<std::string> take;
    for (const std::string &path : paths)
    {
        const Capture *capture = find_capture(path);
        if (capture == nullptr || capture->blank)
            continue;
        const Destination *to =
            chosen != nullptr ? chosen : config_.destination_for(capture->item.title_id);
        if (to != nullptr && to->configured())
            take.push_back(path);
    }
    if (take.empty())
        return fail(paths.empty() ? "Nothing to send" : "None of these can be sent right now");

    batch_ = Batch{};
    batch_.active = true;
    batch_.paths = take;
    batch_.destination = destination_id;
    batch_.destination_name =
        chosen != nullptr ? chosen->display_name() : std::string("each game's destination");
    batch_.delete_after = delete_after;
    batch_.started = now_seconds();
    if (environment_.demo || !app_flag(kBackgroundSendsFlag, true))
    {
        batch_by_app(take);
        return static_cast<int>(take.size());
    }
    const std::string flags = delete_after ? "batch,delete" : "batch";
    io(
        [this, take, destination_id, flags]
        {
            std::string reason;
            int first = 0;
            int last = 0;
            std::vector<std::string> refused;
            if (ensure_daemon(&reason))
            {
                for (const std::string &path : take)
                {
                    std::string reply;
                    const int id =
                        daemon_command("QUEUE " + destination_id + "\t" + path + "\t" + flags,
                                       &reply, 2000) &&
                                starts_with(reply, "ok ")
                            ? static_cast<int>(
                                  parse_int(trim(std::string_view(reply).substr(3)), 0))
                            : 0;
                    if (id <= 0)
                    {
                        refused.push_back(path);
                        continue;
                    }
                    first = first == 0 ? id : first;
                    last = id;
                }
            }
            else
            {
                refused = take;
            }
            post(
                [first, last, refused](Model &model)
                {
                    model.batch_.first_id = first;
                    model.batch_.last_id = last;
                    // Watched only from here, so its end cannot come before its numbers.
                    model.batch_last_ = last;
                    // What pshared would not take (a clip it reads as zeros, or
                    // no pshared at all), the app sends itself.
                    if (!refused.empty())
                        model.batch_by_app(refused);
                });
        });
    return static_cast<int>(take.size());
}

void Model::batch_by_app(const std::vector<std::string> &paths)
{
    batch_.by_app = true;
    for (const std::string &path : paths)
    {
        const Capture *capture = find_capture(path);
        const Destination *to = capture == nullptr ? nullptr
                                : batch_.destination.empty()
                                    ? config_.destination_for(capture->item.title_id)
                                    : config_.find(batch_.destination);
        if (capture != nullptr && to != nullptr)
            queue_job(*capture, *to, batch_.delete_after, true, nullptr);
    }
    if (!app_batch_pending() && (batch_.last_id == 0 || batch_.daemon_done))
        post([](Model &model) { model.finish_batch({}); });
}

bool Model::app_batch_pending() const
{
    Lock lock(mutex_);
    return std::any_of(upload_jobs_.begin(), upload_jobs_.end(),
                       [](const UploadJob &job) { return job.batch; }) ||
           (uploading_ && current_.batch);
}

void Model::finish_batch(const std::string &note)
{
    if (!batch_.active)
        return;
    history_.refresh();
    mark_uploads();
    // Sent: a successful history line since the batch started.
    int sent = 0;
    for (const std::string &path : batch_.paths)
        for (const HistoryEntry &entry : history_.entries())
            if (entry.ok && entry.path == path && entry.time >= batch_.started)
            {
                ++sent;
                break;
            }
    const int total = static_cast<int>(batch_.paths.size());
    ModelEvent event;
    event.kind = EventKind::batch_finished;
    event.ok = sent == total;
    event.title = "Sent " + std::to_string(sent) + " of " + std::to_string(total) +
                  (total == 1 ? " capture" : " captures");
    event.detail = !note.empty()                     ? note
                   : sent < total                    ? "The ones that failed are in History"
                   : batch_.delete_after && sent > 0 ? "each deleted from the console once sent"
                                                     : "to " + batch_.destination_name;
    events_.push_back(std::move(event));
    const bool deleted = batch_.delete_after;
    batch_ = Batch{};
    batch_last_ = 0;
    if (deleted)
    {
        rescan();
        delete_pending(); // what pshared sent but could not delete
    }
}

void Model::cancel_upload()
{
    // A Send all handed to pshared: drop its queue there too; RESULT on the
    // last job then answers, and the batch finishes as usual.
    if (batch_.active && batch_.last_id > 0)
        io([] { daemon_command("CANCEL", nullptr); });
    Lock lock(mutex_);
    upload_jobs_.clear();
    upload_configs_.clear();
    progress_.cancel = true;
}

UploadView Model::upload_view() const
{
    Lock lock(mutex_);
    UploadView view;
    view.active = uploading_;
    view.job = current_;
    view.sent = progress_.sent.load();
    view.total = progress_.total.load();
    view.stage = progress_.stage.load();
    view.queued = static_cast<int>(upload_jobs_.size());
    if (batch_.active)
    {
        view.batch = true;
        view.batch_total = static_cast<int>(batch_.paths.size());
        view.batch_destination = batch_.destination_name;
        int left = 0;
        for (const UploadJob &job : upload_jobs_)
            left += job.batch ? 1 : 0;
        // In pshared: how far its QUEUE numbers have got; in the app: what is left.
        view.batch_done = batch_.by_app && batch_.last_id == 0
                              ? view.batch_total - left - (uploading_ && current_.batch ? 1 : 0)
                              : batch_.done;
        if (!uploading_ && batch_.last_id > 0 && daemon_.job >= batch_.first_id &&
            daemon_.job <= batch_.last_id)
        {
            view.sent = daemon_.sent;
            view.total = daemon_.total;
        }
    }
    return view;
}

// ---- the background uploader -----------------------------------------------

void Model::start_daemon()
{
    if (daemon_starting_)
        return;
    daemon_starting_ = true;
    const std::string elf_path = environment_.app_root + "/pshared.elf";
    const bool demo = environment_.demo;
    io(
        [this, elf_path, demo]
        {
            std::string error;
            bool ok = false;
            bool already = false; // running already, nothing to say
            if (demo)
            {
                usleep(600000);
                demo_daemon_running_ = true;
                ok = true;
            }
            else
            {
                // A pshared that is already running stays when it is this
                // build; an older one is replaced (the new copy asks it to stop).
                std::string elf;
                const bool read = read_text_file(elf_path, &elf, 64u << 20);
                const std::string build = read ? elf_build_id(elf) : std::string();
                const auto running_build = []() -> std::string
                {
                    std::string reply;
                    if (!daemon_command("PING", &reply, 400))
                        return {};
                    const std::vector<std::string> words = split(trim(reply), ' ');
                    return words.size() >= 4 ? words[3] : std::string("old");
                };
                const std::string running = running_build();
                if (!running.empty() && (!read || running == build))
                {
                    ok = true;
                    already = true;
                }
                else if (!read)
                {
                    error = "pshared.elf is missing from the app folder";
                }
                else if (send_to_loader(elf, &error))
                {
                    // It answers once its control line is up.
                    for (int i = 0; i < 60 && !ok; ++i)
                    {
                        usleep(150000);
                        const std::string now = running_build();
                        ok = !now.empty() && (build.empty() || now == build);
                    }
                    if (!ok)
                        error = "pshared did not answer after starting";
                }
            }
            const DaemonStatus status = demo ? DaemonStatus{} : daemon_status();
            post(
                [ok, already, error, status](Model &model)
                {
                    model.daemon_starting_ = false;
                    if (status.reachable)
                        model.daemon_ = status;
                    if (already)
                        return;
                    ModelEvent event;
                    event.kind = ok ? EventKind::daemon_started : EventKind::daemon_failed;
                    event.ok = ok;
                    event.title = ok ? "Background uploader is running"
                                     : "Could not start the background uploader";
                    event.detail = error;
                    model.events_.push_back(std::move(event));
                });
        });
}

void Model::stop_daemon()
{
    const bool demo = environment_.demo;
    io(
        [this, demo]
        {
            if (demo)
                demo_daemon_running_ = false;
            else
                daemon_command("QUIT", nullptr);
            post(
                [](Model &model)
                {
                    model.daemon_ = DaemonStatus{};
                    ModelEvent event;
                    event.kind = EventKind::daemon_stopped;
                    event.title = "Background uploader stopped";
                    model.events_.push_back(std::move(event));
                });
        });
}

void Model::pause_daemon(bool paused)
{
    const bool demo = environment_.demo;
    io(
        [this, paused, demo]
        {
            if (demo)
                demo_daemon_paused_ = paused;
            else
                daemon_command(paused ? "PAUSE" : "RESUME", nullptr);
        });
}

void Model::open_phone_setup()
{
    if (!daemon_.reachable)
        start_daemon();
    const bool demo = environment_.demo;
    io(
        [this, demo]
        {
            std::string reply;
            bool ok = false;
            for (int i = 0; i < 30 && !ok && !quit_.load(); ++i)
            {
                if (demo)
                {
                    reply = "ok\n";
                    ok = demo_daemon_running_.load();
                    demo_setup_ = ok;
                }
                else
                {
                    ok = daemon_command("SETUP on", &reply, 600);
                }
                if (!ok)
                    usleep(200000);
            }
            const bool open = ok && starts_with(reply, "ok");
            post(
                [open](Model &model)
                {
                    if (open)
                        model.daemon_.setup_port = kSetupPort;
                });
        });
}

void Model::close_phone_setup()
{
    daemon_.setup_port = 0;
    demo_setup_ = false;
    if (environment_.demo)
        return;
    io([] { daemon_command("SETUP off", nullptr); });
}

} // namespace pshare::app
