// PShare - The phone setup page served by pshared.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Typing a 120-character Discord webhook with a controller is nobody's idea
// of fun, so pshared serves one small page on the local network
// (http://<console>:9078/). The app shows its address as a QR code; the page
// edits the same config.ini as the app. It is up only while PShare runs or
// setup is switched on, and saved webhooks, tokens and passwords are never
// sent back to the browser.
//
// Everything here is pure (no sockets) so the PC tests cover it; the daemon
// owns the listening socket.

#pragma once

#include "pshare/core/config.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pshare
{

struct WebRequest
{
    std::string method; // "GET", "POST"
    std::string path;   // without the query
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;
};

// How much of `raw` the head takes; 0 while it is incomplete. -1: malformed.
long web_head_length(std::string_view raw);
// The Content-Length the head announces (0 when none).
std::size_t web_content_length(std::string_view head);
bool parse_web_request(std::string_view raw, WebRequest *request);

std::string url_decode(std::string_view text); // '+' is a space
std::map<std::string, std::string> parse_form(std::string_view body);
std::string html_escape(std::string_view text);

std::string web_response(int status, std::string_view content_type, std::string_view body,
                         std::string_view extra_headers = {});

// The page itself. `notice` is shown at the top ("Saved", an error...).
std::string setup_page(const Config &config, std::string_view notice, bool notice_is_error);

// The captures, for browsing and downloading from a phone or a PC.
struct GalleryItem
{
    std::string path;
    std::string game;
    std::string when; // "2026-10-09 18:35"
    std::string size; // "3.2 MB"
    bool video = false;
};
// The gallery's game list and filter: title ID, name and count per game.
struct GalleryGame
{
    std::string title_id;
    std::string name;
    int count = 0;
};
struct GalleryFilter
{
    std::vector<GalleryGame> games; // for the choice list, most captures first
    std::string game;               // the one shown (title ID); empty = all
    bool oldest_first = false;
    int page = 0;
    int total = 0; // captures in the filter, every page
    // Where Send can go: id and name of each destination that is on and set up.
    std::vector<std::pair<std::string, std::string>> destinations;
};
// The gallery is one form, posted to /act: each card has a tick box named
// "s.<n>" holding its path, and the bar under the grid sends or deletes the
// ticked ones ("act" = send / delete), or sends the whole filter
// ("act" = send-all, with "game"), to "d" (empty: each game's usual
// destination), skipping what was sent before when "skip" is there.
std::string gallery_page(const std::vector<GalleryItem> &items, int page_index, int pages,
                         std::string_view notice, const GalleryFilter &filter = {});

// One capture, full screen in a browser, with the app's controls: play/pause,
// 10 s and 30 s jumps, previous/next, send, send to..., back to the captures.
// Keyboard, touch and a gamepad (Gamepad API) all drive it.
struct ViewDestination
{
    std::string id;
    std::string name;
    bool is_default = false;
};
struct ViewInfo
{
    GalleryItem item;
    std::string prev;                          // path of the newer capture, or empty
    std::string next;                          // path of the older capture, or empty
    std::string link;                          // its last upload link, or empty
    std::vector<ViewDestination> destinations; // enabled and set up
    bool blank = false;                        // it reads back as zeros (capture_reads_blank)
};
std::string view_page(const ViewInfo &info);

// One "Range: bytes=..." header against a file of `size` bytes: the first and
// last byte asked for. False when absent, malformed or unsatisfiable.
bool parse_range(std::string_view header, std::uint64_t size, std::uint64_t *first,
                 std::uint64_t *last);
// The head of a file answer (200, or 206 for a range).
std::string file_response_head(int status, std::string_view content_type, std::uint64_t size,
                               std::uint64_t first, std::uint64_t last,
                               std::string_view download_name);

// Applies the submitted fields (names as setup_page writes them) to `config`.
// Unknown fields are ignored; malformed numbers are reported in `errors`.
// Returns the number of values that changed.
int apply_setup_form(const std::map<std::string, std::string> &form, Config *config,
                     std::vector<std::string> *errors);

} // namespace pshare
