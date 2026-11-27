// PShare - What the app knows and does, without any drawing.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The model owns the captures, the configuration, the upload queue, the
// history and the daemon's state. Everything slow (walking the capture
// folders, writing /data, HTTPS, talking to pshared) happens on worker
// threads; they hand their results back as closures that pump() runs on the
// main thread, so the screens read plain members without locks.
//
// The PC preview runs the same model in demo mode: uploads go to a fake
// transport and the daemon is simulated.

#pragma once

#include "app/thread.hpp"
#include "pshare/core/config.hpp"
#include "pshare/core/history.hpp"
#include "pshare/core/media.hpp"
#include "pshare/core/net.hpp"
#include "pshare/core/upload.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pshare::app
{

struct Environment
{
    Paths paths;
    std::string app_root = "/app0"; // where pshared.elf is
    bool elevated = true;           // the app could open /data and the capture folders
    std::string elevation_note;     // why not, when it could not
    bool demo = false;              // PC preview: fake network, simulated daemon
};

struct Capture
{
    MediaItem item;
    std::string game;
    bool uploaded = false;
    bool failed = false; // the last attempt failed
    bool blank = false;  // the console reads it back as all zeros: it cannot play or be sent
    std::string link;    // the last link it got
};

enum class Filter : std::uint8_t
{
    all,
    screenshots,
    clips,
};

struct UploadJob
{
    int id = 0;
    std::string path;
    std::string file;
    std::string game;
    std::string destination; // id
    std::string destination_name;
    bool video = false;
    std::uint64_t size = 0;
    bool background = false;   // hand it to pshared (it goes on after the app closes)
    bool delete_after = false; // delete the capture from the console once it is sent
    bool batch = false;        // one of a "Send all": no toast of its own
};

struct UploadView
{
    bool active = false;
    UploadJob job;
    std::uint64_t sent = 0;
    std::uint64_t total = 0;
    int stage = 0; // UploadProgress::stage
    int queued = 0;
    // A "Send all" in progress: how far, and where to.
    bool batch = false;
    int batch_done = 0;
    int batch_total = 0;
    std::string batch_destination;
};

enum class EventKind : std::uint8_t
{
    scanned,
    upload_finished,
    daemon_started,
    daemon_failed,
    daemon_stopped,
    config_saved,
    config_failed,
    unreadable,     // captures that read back as zeros were found (unreadable_unasked())
    deleted,        // delete_captures() finished: ok, title, detail
    batch_finished, // a send_batch() ended: ok (all sent), title, detail
    note,
};

struct ModelEvent
{
    EventKind kind = EventKind::note;
    bool ok = true;
    std::string title;
    std::string detail;
    std::string link;
};

class Model
{
  public:
    explicit Model(Environment environment);
    ~Model();
    Model(const Model &) = delete;
    Model &operator=(const Model &) = delete;

    // Loads the configuration, starts the workers and the first scan.
    void start();
    // Runs what the workers handed back; returns what the screens should hear about.
    std::vector<ModelEvent> pump();

    const Environment &environment() const
    {
        return environment_;
    }

    // ---- captures ----
    const std::vector<Capture> &captures() const
    {
        return captures_;
    }
    // Indexes into captures(), newest first.
    std::vector<int> filtered(Filter filter) const;
    // ... of one game only (title ID; empty = every game), oldest first if asked.
    std::vector<int> filtered(Filter filter, const std::string &title_id, bool oldest_first) const;
    // Every game with captures, most captures first: title ID, name, screenshots, clips.
    struct GameCount
    {
        std::string title_id;
        std::string name;
        int screenshots = 0;
        int clips = 0;
    };
    std::vector<GameCount> games() const;
    const Capture *find_capture(const std::string &path) const;
    bool scanning() const
    {
        return scanning_;
    }
    bool scanned_once() const
    {
        return scanned_once_;
    }
    // The capture folders could not be opened (0 when they could).
    int scan_errno() const
    {
        return scan_errno_;
    }
    int screenshots() const
    {
        return screenshot_count_;
    }
    int clips() const
    {
        return clip_count_;
    }
    void rescan();

    // Captures that read back as zeros and the player has not been asked about.
    std::vector<std::string> unreadable_unasked() const;
    void mark_unreadable_asked(const std::vector<std::string> &paths);
    // Deletes them (and the files beside them), then rescans; posts EventKind::deleted.
    void delete_captures(std::vector<std::string> paths);
    // Deletes what pshared sent with "delete once sent" but was not allowed to
    // delete itself (Paths::pending_deletes); posts EventKind::deleted when any went.
    void delete_pending();

    // ---- configuration ----
    const Config &config() const
    {
        return config_;
    }
    // Change it, then save_config(). The daemon reloads it too.
    Config &edit_config()
    {
        return config_;
    }
    void save_config();
    const std::vector<std::string> &config_warnings() const
    {
        return warnings_;
    }
    // Settings of the app alone, in the [app] section of the same file.
    bool app_flag(const char *key, bool fallback) const;
    void set_app_flag(const char *key, bool value);

    // ---- uploads ----
    // Queues a capture for `destination` (empty: the one the config picks).
    // Returns false with a reason when it cannot be queued.
    bool upload(const std::string &path, const std::string &destination, std::string *why);
    // "Send all": `paths` to `destination` (empty: each game's usual one),
    // each deleted from the console once it is sent when `delete_after`. Handed
    // to pshared in one go, so it finishes after the app closes; sent by the
    // app itself when pshared cannot run. Returns how many it took (0 with a
    // reason when none); EventKind::batch_finished follows.
    int send_batch(const std::vector<std::string> &paths, const std::string &destination,
                   bool delete_after, std::string *why);
    bool batch_active() const
    {
        return batch_.active;
    }
    void cancel_upload();
    UploadView upload_view() const;

    // ---- the background uploader ----
    const DaemonStatus &daemon() const
    {
        return daemon_;
    }
    bool daemon_starting() const
    {
        return daemon_starting_;
    }
    void start_daemon();
    void stop_daemon();
    void pause_daemon(bool paused);
    // Opens the phone page for 30 minutes (starts pshared if needed).
    void open_phone_setup();
    void close_phone_setup();

    // ---- history ----
    const std::vector<HistoryEntry> &history() const
    {
        return history_.entries();
    }

    std::string local_ip() const
    {
        return local_ip_.empty() ? daemon_.ip : local_ip_;
    }
    // A game's name for a title ID ("Home screen" when empty).
    std::string game_name(const std::string &title_id);
    std::string game_icon(const std::string &title_id) const;

  private:
    using Task = std::function<void()>;
    using Result = std::function<void(Model &)>;

    void post(Result result);
    void io(Task task);
    void io_loop();
    void upload_loop();
    void status_loop();
    void scan_now();
    void mark_uploads();
    void count_kinds();
    HttpResult demo_transport(const HttpRequest &request, UploadProgress *progress);
    // pshared answering, started if needed. False when it cannot run.
    bool ensure_daemon(std::string *why);
    // Sends `job` through pshared. False when pshared could not take it (the
    // app then sends it itself); otherwise `outcome` is pshared's answer.
    bool send_through_daemon(const UploadJob &job, UploadOutcome *outcome);
    // A job for the app's upload thread; upload() and the batch fallback use it.
    bool queue_job(const Capture &capture, const Destination &destination, bool delete_after,
                   bool batch, std::string *why);
    // The batch's captures go to the app's own queue (pshared cannot take them).
    void batch_by_app(const std::vector<std::string> &paths);
    // How the batch went, from the history; then the event.
    void finish_batch(const std::string &note);

    Environment environment_;
    Config config_;
    IniDocument document_;
    std::vector<std::string> warnings_;
    std::vector<Capture> captures_;
    History history_;
    DaemonStatus daemon_;
    std::string local_ip_;
    bool scanning_ = false;
    bool scanned_once_ = false;
    std::vector<std::string> unreadable_asked_;
    // The io thread's: path -> (size when checked, read back as zeros).
    std::map<std::string, std::pair<std::uint64_t, bool>> blank_checked_;
    int scan_errno_ = 0;
    int screenshot_count_ = 0;
    int clip_count_ = 0;
    bool daemon_starting_ = false;
    std::vector<ModelEvent> events_;
    // The "Send all" in progress (the main thread's).
    struct Batch
    {
        bool active = false;
        bool by_app = false; // in the app's own queue, not pshared's
        std::vector<std::string> paths;
        std::string destination; // id; empty: each game's usual one
        std::string destination_name;
        bool delete_after = false;
        std::int64_t started = 0;
        int first_id = 0; // pshared's QUEUE numbers, once handed over
        int last_id = 0;
        bool daemon_done = false; // pshared has answered for its last one
        int done = 0;
    } batch_;
    // Batch jobs still in the app's own queue (or being sent by it).
    bool app_batch_pending() const;

    // ---- shared with the workers ----
    mutable Mutex mutex_;
    Condition io_wake_;
    Condition upload_wake_;
    std::deque<Task> io_tasks_;
    std::deque<UploadJob> upload_jobs_;
    std::deque<Config> upload_configs_; // what each queued job was queued with
    std::deque<Result> results_;
    UploadJob current_;
    bool uploading_ = false;
    std::atomic<bool> quit_{false};
    std::atomic<bool> poll_daemon_{true};
    std::atomic<int> batch_last_{0}; // the status thread watches this QUEUE number finish
    std::atomic<bool> demo_daemon_running_{false};
    std::atomic<bool> demo_daemon_paused_{false};
    std::atomic<bool> demo_setup_{false};
    UploadProgress progress_;
    int next_job_ = 1;
    TitleNames names_;        // the main thread's
    TitleNames worker_names_; // the io thread's
    Thread io_thread_;
    Thread upload_thread_;
    Thread status_thread_;
    // The clip relay: pshared asks the app for capture bytes it reads as zeros.
    Thread relay_threads_[3];
    void relay_loop();
};

} // namespace pshare::app
