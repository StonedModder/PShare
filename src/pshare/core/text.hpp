// PShare - Small string helpers shared by the app, the daemon and the tests.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything here is allocation-light, exception-free C++20 so it builds for
// a native title, for the payload SDK and for the PC alike.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pshare
{

std::string_view trim(std::string_view text);
std::string lower(std::string_view text);
std::string upper(std::string_view text);
bool iequals(std::string_view a, std::string_view b);
bool starts_with(std::string_view text, std::string_view prefix);
bool ends_with_ci(std::string_view text, std::string_view suffix);
std::vector<std::string> split(std::string_view text, char separator);

// "true", "yes", "on", "1" (any case) are true; "false", "no", "off", "0" are
// false; anything else gives `fallback`.
bool parse_bool(std::string_view text, bool fallback);
// Decimal integer; `fallback` when empty or malformed.
long long parse_int(std::string_view text, long long fallback);

// RFC 3986 unreserved characters pass, everything else becomes %XX.
std::string url_encode(std::string_view text);
// A JSON string body (no quotes): escapes quotes, backslashes and controls.
std::string json_escape(std::string_view text);

// "1.4 MB", "820 KB", "12 B".
std::string human_size(std::uint64_t bytes);

// Replaces every "{key}" in `pattern` with its value; unknown keys are kept.
struct Placeholder
{
    std::string_view key;
    std::string_view value;
};
std::string expand(std::string_view pattern, const Placeholder *values, std::size_t count);

// The first "key": "value" string in a JSON document at any depth (no
// unescaping beyond \" \\ \/ \n). Enough for the answers PShare reads.
bool json_find_string(std::string_view json, std::string_view key, std::string *value);
// The same for true/false.
bool json_find_bool(std::string_view json, std::string_view key, bool *value);
// The same, but only a key of the outermost object (not one inside a nested
// object or list), with the same light unescaping.
bool json_top_string(std::string_view json, std::string_view key, std::string *value);
// The same for a non-negative number such as 35 or 1.5 (no exponent).
bool json_find_number(std::string_view json, std::string_view key, double *value);
// The first http:// or https:// URL in a text, up to whitespace or a quote.
bool find_url(std::string_view text, std::string *url);

// The console's local time for a Unix time. On a native title the C
// library has no localtime_r, so the console's own conversion is used there.
struct LocalTime
{
    int year = 1970;
    int month = 1; // 1..12
    int day = 1;   // 1..31
    int hour = 0;
    int minute = 0;
    int second = 0;
    int yday = 0; // 0..365, days since 1 January
};
LocalTime local_time(std::int64_t unix_seconds);
// The calendar date of a day count since 1970-01-01 (no time zone).
LocalTime civil_time(std::int64_t seconds);

// A stable 64-bit FNV-1a hash (thumbnail cache names).
std::uint64_t fnv1a(std::string_view text);

} // namespace pshare
