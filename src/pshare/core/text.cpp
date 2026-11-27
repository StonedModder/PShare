// PShare - Small string helpers shared by the app, the daemon and the tests.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/text.hpp"

#include <ctime>

#if defined(PSHARE_CONSOLE)
extern "C" int sceKernelConvertUtcToLocaltime(std::int64_t utc, std::int64_t *local, void *zone,
                                              int *dst_seconds);
#endif

#include <cstdio>
#include <cstdlib>

namespace pshare
{
namespace
{
bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

char to_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

char to_upper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}
} // namespace

std::string_view trim(std::string_view text)
{
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    return text;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = to_lower(c);
    return out;
}

std::string upper(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = to_upper(c);
    return out;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (to_lower(a[i]) != to_lower(b[i]))
            return false;
    return true;
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool ends_with_ci(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           iequals(text.substr(text.size() - suffix.size()), suffix);
}

std::vector<std::string> split(std::string_view text, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t at = text.find(separator, start);
        if (at == std::string_view::npos)
        {
            parts.emplace_back(text.substr(start));
            return parts;
        }
        parts.emplace_back(text.substr(start, at - start));
        start = at + 1;
    }
}

bool parse_bool(std::string_view text, bool fallback)
{
    const std::string value = lower(trim(text));
    if (value == "true" || value == "yes" || value == "on" || value == "1")
        return true;
    if (value == "false" || value == "no" || value == "off" || value == "0")
        return false;
    return fallback;
}

long long parse_int(std::string_view text, long long fallback)
{
    text = trim(text);
    if (text.empty())
        return fallback;
    bool negative = false;
    if (text.front() == '-' || text.front() == '+')
    {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty() || text.size() > 18)
        return fallback;
    long long value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
            return fallback;
        value = value * 10 + (c - '0');
    }
    return negative ? -value : value;
}

std::string url_encode(std::string_view text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char raw : text)
    {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string json_escape(std::string_view text)
{
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 8);
    for (const char raw : text)
    {
        const auto c = static_cast<unsigned char>(raw);
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

std::string human_size(std::uint64_t bytes)
{
    char buffer[32];
    if (bytes < 1024)
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024ull * 1024)
        std::snprintf(buffer, sizeof(buffer), "%llu KB",
                      static_cast<unsigned long long>((bytes + 512) / 1024));
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(buffer, sizeof(buffer), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buffer;
}

std::string expand(std::string_view pattern, const Placeholder *values, std::size_t count)
{
    std::string out;
    out.reserve(pattern.size() + 32);
    std::size_t i = 0;
    while (i < pattern.size())
    {
        if (pattern[i] == '{')
        {
            const std::size_t close = pattern.find('}', i + 1);
            if (close != std::string_view::npos)
            {
                const std::string_view key = pattern.substr(i + 1, close - i - 1);
                bool replaced = false;
                for (std::size_t k = 0; k < count; ++k)
                {
                    if (values[k].key == key)
                    {
                        out.append(values[k].value);
                        replaced = true;
                        break;
                    }
                }
                if (replaced)
                {
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(pattern[i]);
        ++i;
    }
    return out;
}

namespace
{
// Position just after `"key"` followed by optional spaces and a colon.
std::size_t after_key(std::string_view json, std::string_view key, std::size_t from)
{
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    for (;;)
    {
        const std::size_t at = json.find(needle, from);
        if (at == std::string_view::npos)
            return std::string_view::npos;
        std::size_t p = at + needle.size();
        while (p < json.size() && is_space(json[p]))
            ++p;
        if (p < json.size() && json[p] == ':')
        {
            ++p;
            while (p < json.size() && is_space(json[p]))
                ++p;
            return p;
        }
        from = at + 1;
    }
}
} // namespace

bool json_find_string(std::string_view json, std::string_view key, std::string *value)
{
    std::size_t from = 0;
    for (;;)
    {
        std::size_t p = after_key(json, key, from);
        if (p == std::string_view::npos)
            return false;
        if (p < json.size() && json[p] == '"')
        {
            std::string out;
            for (++p; p < json.size(); ++p)
            {
                const char c = json[p];
                if (c == '"')
                {
                    if (value)
                        *value = std::move(out);
                    return true;
                }
                if (c == '\\' && p + 1 < json.size())
                {
                    const char e = json[++p];
                    out.push_back(e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e);
                    continue;
                }
                out.push_back(c);
            }
            return false;
        }
        from = p; // the key held a non-string; look for a later one
    }
}

bool json_find_bool(std::string_view json, std::string_view key, bool *value)
{
    const std::size_t p = after_key(json, key, 0);
    if (p == std::string_view::npos)
        return false;
    if (json.substr(p, 4) == "true")
    {
        if (value)
            *value = true;
        return true;
    }
    if (json.substr(p, 5) == "false")
    {
        if (value)
            *value = false;
        return true;
    }
    return false;
}

bool find_url(std::string_view raw, std::string *url)
{
    // JSON writers may escape the slashes of URLs ("https:\/\/...").
    std::string text;
    text.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '/')
            continue;
        text.push_back(raw[i]);
    }
    std::size_t at = text.find("https://");
    const std::size_t plain = text.find("http://");
    if (at == std::string::npos || (plain != std::string::npos && plain < at))
        at = plain;
    if (at == std::string::npos)
        return false;
    std::size_t end = at;
    while (end < text.size() && !is_space(text[end]) && text[end] != '"' && text[end] != '\'' &&
           text[end] != '<' && text[end] != '>')
        ++end;
    if (url)
        url->assign(text, at, end - at);
    return true;
}

std::uint64_t fnv1a(std::string_view text)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const char c : text)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

LocalTime civil_time(std::int64_t seconds)
{
    // Howard Hinnant's days-to-civil: exact for the proleptic Gregorian calendar.
    std::int64_t days = seconds / 86400;
    std::int64_t rest = seconds % 86400;
    if (rest < 0)
    {
        rest += 86400;
        --days;
    }
    LocalTime out;
    out.hour = static_cast<int>(rest / 3600);
    out.minute = static_cast<int>(rest % 3600 / 60);
    out.second = static_cast<int>(rest % 60);
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    out.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    out.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    out.year = static_cast<int>(yoe + era * 400 + (out.month <= 2 ? 1 : 0));
    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (out.year % 4 == 0 && out.year % 100 != 0) || out.year % 400 == 0;
    out.yday = before[out.month - 1] + out.day - 1 + (leap && out.month > 2 ? 1 : 0);
    return out;
}

LocalTime local_time(std::int64_t unix_seconds)
{
#if defined(PSHARE_CONSOLE)
    std::int64_t local = unix_seconds;
    std::int64_t converted = 0;
    std::uint8_t zone[16] = {}; // struct timezone, with room to spare
    int dst = 0;
    if (sceKernelConvertUtcToLocaltime(unix_seconds, &converted, zone, &dst) == 0)
        local = converted;
    return civil_time(local);
#else
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm parts{};
    if (localtime_r(&value, &parts) == nullptr)
        return civil_time(unix_seconds);
    LocalTime out;
    out.year = parts.tm_year + 1900;
    out.month = parts.tm_mon + 1;
    out.day = parts.tm_mday;
    out.hour = parts.tm_hour;
    out.minute = parts.tm_min;
    out.second = parts.tm_sec;
    out.yday = parts.tm_yday;
    return out;
#endif
}

} // namespace pshare
