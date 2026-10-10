// PShare - An INI document that keeps its order and comments when rewritten.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/ini.hpp"

#include "pshare/core/text.hpp"

#include <initializer_list>

namespace pshare
{

bool IniDocument::parse(std::string_view text)
{
    sections_.clear();
    sections_.push_back({});
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view raw = text.substr(start, end - start);
        if (!raw.empty() && raw.back() == '\r')
            raw.remove_suffix(1);
        const std::string_view line = trim(raw);
        if (!line.empty() && line.front() == '[' && line.back() == ']')
        {
            sections_.push_back({std::string(trim(line.substr(1, line.size() - 2))), {}});
        }
        else if (line.empty() || line.front() == ';' || line.front() == '#')
        {
            if (!(end == text.size() && line.empty()))
                sections_.back().lines.push_back({{}, std::string(raw)});
        }
        else
        {
            const std::size_t equals = line.find('=');
            if (equals == std::string_view::npos)
            {
                sections_.back().lines.push_back({{}, "; " + std::string(raw)});
            }
            else
            {
                std::string_view value = trim(line.substr(equals + 1));
                // "key = value ; note": a comment after a space. A ';' with no
                // space before it belongs to the value (URLs, tokens).
                if (value.empty() || value.front() != '"')
                    for (const char *mark : {" ;", "\t;", " #", "\t#"})
                    {
                        const std::size_t at = value.find(mark);
                        if (at != std::string_view::npos)
                            value = trim(value.substr(0, at));
                    }
                // Quotes let a value keep leading or trailing spaces.
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                sections_.back().lines.push_back(
                    {std::string(trim(line.substr(0, equals))), std::string(value)});
            }
        }
        if (end == text.size())
            break;
        start = end + 1;
    }
    return true;
}

std::string IniDocument::write() const
{
    std::string out;
    bool first = true;
    for (const Section &section : sections_)
    {
        if (!section.name.empty())
        {
            if (!first && !out.empty() && out.back() == '\n' &&
                !(out.size() >= 2 && out[out.size() - 2] == '\n'))
                out += "\n";
            out += "[" + section.name + "]\n";
        }
        for (const Line &line : section.lines)
        {
            if (line.key.empty())
            {
                out += line.value + "\n";
                continue;
            }
            const bool quote =
                !line.value.empty() && (line.value.front() == ' ' || line.value.back() == ' ');
            out += line.key + " = " + (quote ? "\"" + line.value + "\"" : line.value) + "\n";
        }
        first = false;
    }
    return out;
}

IniDocument::Section *IniDocument::find(std::string_view section)
{
    for (Section &s : sections_)
        if (iequals(s.name, section))
            return &s;
    return nullptr;
}

const IniDocument::Section *IniDocument::find(std::string_view section) const
{
    for (const Section &s : sections_)
        if (iequals(s.name, section))
            return &s;
    return nullptr;
}

bool IniDocument::has_section(std::string_view section) const
{
    return find(section) != nullptr;
}

std::vector<std::string> IniDocument::keys(std::string_view section) const
{
    std::vector<std::string> out;
    if (const Section *s = find(section))
        for (const Line &line : s->lines)
            if (!line.key.empty())
                out.push_back(line.key);
    return out;
}

std::vector<std::string> IniDocument::sections_with_prefix(std::string_view prefix) const
{
    std::vector<std::string> out;
    for (const Section &s : sections_)
        if (s.name.size() > prefix.size() &&
            iequals(std::string_view(s.name).substr(0, prefix.size()), prefix))
            out.push_back(s.name);
    return out;
}

bool IniDocument::get(std::string_view section, std::string_view key, std::string *value) const
{
    const Section *s = find(section);
    if (s == nullptr)
        return false;
    for (const Line &line : s->lines)
    {
        if (!line.key.empty() && iequals(line.key, key))
        {
            if (value)
                *value = line.value;
            return true;
        }
    }
    return false;
}

std::string IniDocument::get_or(std::string_view section, std::string_view key,
                                std::string_view fallback) const
{
    std::string value;
    return get(section, key, &value) ? value : std::string(fallback);
}

bool IniDocument::get_bool(std::string_view section, std::string_view key, bool fallback) const
{
    std::string value;
    return get(section, key, &value) ? parse_bool(value, fallback) : fallback;
}

long long IniDocument::get_int(std::string_view section, std::string_view key,
                               long long fallback) const
{
    std::string value;
    return get(section, key, &value) ? parse_int(value, fallback) : fallback;
}

void IniDocument::set(std::string_view section, std::string_view key, std::string_view value)
{
    if (sections_.empty())
        sections_.push_back({});
    Section *s = find(section);
    if (s == nullptr)
    {
        sections_.push_back({std::string(section), {}});
        s = &sections_.back();
    }
    for (Line &line : s->lines)
    {
        if (!line.key.empty() && iequals(line.key, key))
        {
            line.value.assign(value);
            return;
        }
    }
    // After the last key, so trailing comments stay where they were written.
    std::size_t at = s->lines.size();
    while (at > 0 && s->lines[at - 1].key.empty() && trim(s->lines[at - 1].value).empty())
        --at;
    s->lines.insert(s->lines.begin() + static_cast<std::ptrdiff_t>(at),
                    {std::string(key), std::string(value)});
}

void IniDocument::set_bool(std::string_view section, std::string_view key, bool value)
{
    set(section, key, value ? "true" : "false");
}

void IniDocument::set_int(std::string_view section, std::string_view key, long long value)
{
    set(section, key, std::to_string(value));
}

void IniDocument::remove_key(std::string_view section, std::string_view key)
{
    Section *s = find(section);
    if (s == nullptr)
        return;
    for (std::size_t i = 0; i < s->lines.size(); ++i)
    {
        if (!s->lines[i].key.empty() && iequals(s->lines[i].key, key))
        {
            s->lines.erase(s->lines.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void IniDocument::remove_section(std::string_view section)
{
    for (std::size_t i = 0; i < sections_.size(); ++i)
    {
        if (!sections_[i].name.empty() && iequals(sections_[i].name, section))
        {
            sections_.erase(sections_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

} // namespace pshare
