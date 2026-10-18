// PShare - Local sockets: the daemon's control line, the ELF loader, notifications.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// pshared listens on 127.0.0.1:9077 for one-line commands; the app uses them
// to show the daemon's state and to pause, resume or stop it:
//
//   PING     -> "pong pshared <version> <build>"
//   STATUS   -> key=value lines (see DaemonStatus)
//   PAUSE / RESUME / RELOAD / RESCAN / QUIT -> "ok"
//
// The app starts pshared the way any payload is started: by streaming the ELF
// to the jailbreak's loader on 127.0.0.1:9021.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace pshare
{

struct DaemonStatus
{
    bool reachable = false;
    std::string version;
    std::string state; // "watching", "paused", "uploading", "starting"
    int uploaded = 0;  // since the daemon started
    int failed = 0;
    int queued = 0;
    std::int64_t started = 0;
    std::string last_file;
    std::string last_link;
    std::string last_error;
    std::string build;       // which build of pshared this is (see elf_build_id)
    int setup_port = 0;      // 0: the page is off
    int watch_errno = 0;     // the capture folders could not be opened
    std::string destination; // where the upload in progress goes (state "uploading")
    std::uint64_t sent = 0;  // ... and how far it is
    std::uint64_t total = 0;
    int job = 0;        // the QUEUE number being sent (0: none, or automatic)
    int delete_ask = 0; // a browser asks to delete this many captures (DELETEASK)
    std::string ip;     // the console's address, as the daemon sees it

    std::string serialize() const;
    static DaemonStatus parse(std::string_view text);
};

// One command to the daemon; the reply (up to 64 KB) in `reply`. False when
// nothing listens (the daemon is not running) or it did not answer in time.
bool daemon_command(std::string_view command, std::string *reply, int timeout_ms = 800,
                    int port = 0);
// from_app: the app saying it is running (pshared keeps the browser pages up).
DaemonStatus daemon_status(int port = 0, bool from_app = false);

// The clip relay. Since a console restart can leave payloads reading video
// captures back as zeros while the app still reads them properly, pshared can
// ask the app for the bytes: the app calls relay_once() in a loop; pshared
// answers within about `wait_ms` with "none" or with a range a browser is
// waiting for, and `read` streams those bytes back through `send`. False when
// pshared did not answer (not running).
using RelaySend = std::function<bool(std::string_view)>;
using RelayRead = std::function<bool(const std::string &path, std::uint64_t first,
                                     std::uint64_t last, const RelaySend &send)>;
bool relay_once(int wait_ms, const RelayRead &read, int port = 0);

// The build marker pshared carries ("PSHARE_BUILD=<id>"); empty when none.
std::string elf_build_id(std::string_view elf);

// Streams an ELF to the payload loader. False with a reason when no loader
// listens or the transfer broke.
bool send_to_loader(const std::string &elf, std::string *error, int port = 0);

// A listening TCP socket on `address` (0 = all interfaces, 0x7f000001 = loopback).
int listen_tcp(std::uint32_t address, int port, int backlog);
// Sets receive and send timeouts on a socket.
void socket_timeouts(int socket, int milliseconds);
bool send_all(int socket, std::string_view data);

// The console's address on the local network ("192.168.1.20"), or empty.
std::string local_ip_address();

// A system notification (the toast in the corner). On the PC it prints.
void notify(std::string_view text);

} // namespace pshare
