// PShare - An INI document that keeps its order and comments when rewritten.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The configuration is a plain INI file, so people can edit it by hand (or from a phone, see
// phone_setup). The app rewrites it when settings change; lines it does not
// touch, comments included, survive the round trip.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pshare
{

class IniDocument
{
  public:
    struct Line
    {
        std::string key;   // empty for comments and blank lines
        std::string value; // the raw text for comments
    };
    struct Section
    {
        std::string name; // "" is the part before the first [section]
        std::vector<Line> lines;
    };

    bool parse(std::string_view text);
    std::string write() const;

    bool has_section(std::string_view section) const;
    // Every key of a section, in file order.
    std::vector<std::string> keys(std::string_view section) const;
    // Every section whose name starts with `prefix` ("destination.").
    std::vector<std::string> sections_with_prefix(std::string_view prefix) const;
    const std::vector<Section> &sections() const
    {
        return sections_;
    }

    bool get(std::string_view section, std::string_view key, std::string *value) const;
    std::string get_or(std::string_view section, std::string_view key,
                       std::string_view fallback) const;
    bool get_bool(std::string_view section, std::string_view key, bool fallback) const;
    long long get_int(std::string_view section, std::string_view key, long long fallback) const;

    // Replaces the value in place, or appends the key to the section (made at
    // the end of the file when missing).
    void set(std::string_view section, std::string_view key, std::string_view value);
    void set_bool(std::string_view section, std::string_view key, bool value);
    void set_int(std::string_view section, std::string_view key, long long value);
    void remove_key(std::string_view section, std::string_view key);
    void remove_section(std::string_view section);

  private:
    Section *find(std::string_view section);
    const Section *find(std::string_view section) const;
    std::vector<Section> sections_;
};

} // namespace pshare
