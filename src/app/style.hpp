// PShare - The look: theme, palette, icons, QR codes.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One ui::Theme restyles every kit component; it is built on the kit's
// frosted "Acrylic" language with PShare's own palette (the same colours
// tools/make-art.py paints the icon and backgrounds with). Icons are drawn
// from shapes, so they stay sharp at any size and need no texture.

#pragma once

#include "gfx/draw_list.hpp"
#include "pshare/core/config.hpp"
#include "ui/theme.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pshare::app
{

namespace gfx = hui::gfx;
namespace ui = hui::ui;

namespace palette
{
constexpr std::uint32_t night = 0x15123a;
constexpr std::uint32_t night_2 = 0x211c55;
constexpr std::uint32_t ink = 0x0e0b28;
constexpr std::uint32_t violet = 0x6b4ce6;
constexpr std::uint32_t plum = 0x963cbe;
constexpr std::uint32_t coral = 0xff5c8a;
constexpr std::uint32_t aqua = 0x3ee6e0;
constexpr std::uint32_t sun = 0xffd25e;
constexpr std::uint32_t cream = 0xfff4e6;
constexpr std::uint32_t mist = 0xb3acdf; // muted text on the night
} // namespace palette

inline gfx::Color rgb(std::uint32_t hex, float alpha = 1.0f)
{
    return gfx::Color::rgb(hex, alpha);
}

const ui::Theme &theme();

// The colour that stands for a kind of destination in lists and chips.
gfx::Color destination_color(DestinationType type);

enum class Icon : std::uint8_t
{
    captures,
    auto_upload,
    destinations,
    history,
    settings,
    play,
    check,
    cross,
    link,
    phone,
    upload,
    refresh,
    plane,
    lock,
    clock,
};

// Draws an icon centred in `box` (a square works best) in one colour.
void draw_icon(gfx::DrawList &list, Icon icon, const gfx::Rect &box, gfx::Color ink);

// A flat paper plane pointing along `angle` (radians, 0 = right).
void draw_plane(gfx::DrawList &list, float cx, float cy, float size, float angle, gfx::Color body,
                gfx::Color fold);

// The mark drawn from shapes: the fallback when assets/art/logo.png is missing.
void draw_mark(gfx::DrawList &list, const gfx::Rect &box);

// A QR code, encoded once and drawn as squares.
class QrCode
{
  public:
    // Re-encodes only when the text changes. False when it does not fit.
    bool set(std::string_view text);
    const std::string &text() const
    {
        return text_;
    }
    int size() const
    {
        return size_;
    }
    bool module(int x, int y) const
    {
        return modules_[static_cast<std::size_t>(y) * size_ + x] != 0;
    }
    // The code with a quiet zone of light colour around it, filling `box`.
    void draw(gfx::DrawList &list, const gfx::Rect &box, gfx::Color dark, gfx::Color light) const;

  private:
    std::string text_;
    int size_ = 0;
    std::vector<std::uint8_t> modules_;
};

// "18:35" / "Today 18:35" / "Yesterday" / "2026-10-07": for lists.
std::string friendly_time(std::int64_t unix_time, std::int64_t now);
// "3 h 12 min", "4 min", "just now"
std::string friendly_duration(std::int64_t seconds);

} // namespace pshare::app
