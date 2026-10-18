// PShare - The look: theme, palette, icons, QR codes.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/style.hpp"

#include "pshare/core/text.hpp"

#include "third_party/qrcodegen/qrcodegen.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <utility>

namespace pshare::app
{
namespace
{
constexpr float kPi = 3.14159265f;

ui::Theme make_theme()
{
    // Start from Acrylic (frosted panels, hairline light edges) and repaint it.
    ui::Theme t = ui::themes()[0];
    for (const ui::Theme &candidate : ui::themes())
        if (std::string_view(candidate.id) == "acrylic")
            t = candidate;
    t.id = "pshare";
    t.name = "PShare";
    t.family = "Night flight";
    t.summary = "Frosted indigo panels, coral actions, aqua selections";
    t.backdrop.mode = gfx::BackdropMode::aurora;
    t.backdrop.colors[0] = rgb(palette::ink);
    t.backdrop.colors[1] = rgb(palette::night);
    t.backdrop.colors[2] = rgb(palette::violet);
    t.backdrop.colors[3] = rgb(palette::plum);
    t.page = rgb(palette::night);
    t.page_text = rgb(palette::cream);
    t.page_text_muted = rgb(palette::mist);
    t.surface = rgb(palette::night_2, 0.62f);
    t.surface_high = rgb(0xffffff, 0.12f);
    t.text = rgb(palette::cream);
    t.text_muted = rgb(palette::mist);
    t.primary = rgb(palette::coral);
    t.on_primary = rgb(0x2a0716);
    t.secondary = rgb(0xffffff, 0.10f);
    t.on_secondary = rgb(palette::cream);
    t.accent = rgb(palette::aqua);
    t.outline = rgb(0xffffff, 0.22f);
    t.focus = rgb(palette::sun);
    t.shadow = rgb(palette::ink, 0.55f);
    t.light = rgb(0xffffff, 0.28f);
    t.success = rgb(0x4ee59a);
    t.warning = rgb(palette::sun);
    t.danger = rgb(0xff5d6c);
    t.style = ui::SurfaceStyle::glass;
    t.radius = 14.0f;
    t.radius_card = 22.0f;
    t.focus_width = 3.5f;
    t.focus_gap = 5.0f;
    t.heading = ui::FontRole::display;
    t.label = ui::FontRole::semibold;
    t.omega = 17.0f;
    t.damping = 0.82f; // a little bounce: playful, not jelly
    t.sounds = hui::audio::SoundSet::glass;
    t.dark = true;
    return t;
}
} // namespace

const ui::Theme &theme()
{
    static const ui::Theme value = make_theme();
    return value;
}

gfx::Color destination_color(DestinationType type)
{
    switch (type)
    {
    case DestinationType::discord:
        return rgb(0x7c8cff);
    case DestinationType::telegram:
        return rgb(palette::aqua);
    case DestinationType::catbox:
        return rgb(palette::sun);
    case DestinationType::litterbox:
        return rgb(0xffa45e);
    case DestinationType::folder:
        return rgb(0x7fe0a8);
    case DestinationType::smb:
        return rgb(0x8fd3ff);
    case DestinationType::onedrive:
        return rgb(0x4f9dff);
    case DestinationType::http:
    case DestinationType::count:
        break;
    }
    return rgb(0xc9b8ff);
}

void draw_plane(gfx::DrawList &list, float cx, float cy, float size, float angle, gfx::Color body,
                gfx::Color fold)
{
    // The same shape as tools/make-art.py: the upper wing over the lower
    // wing and the keel. Polygons are not anti-aliased, so each edge is also
    // stroked with a thin (anti-aliased) line of its own colour.
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const auto at = [&](float x, float y)
    { return std::pair<float, float>{cx + (x * c - y * s) * size, cy + (x * s + y * c) * size}; };
    const auto shape = [&](std::initializer_list<std::pair<float, float>> local, gfx::Color color)
    {
        float xy[16];
        int count = 0;
        for (const auto &[x, y] : local)
        {
            const auto [px, py] = at(x, y);
            xy[count * 2] = px;
            xy[count * 2 + 1] = py;
            ++count;
        }
        list.polygon(xy, count, color);
        const float edge = std::max(1.0f, size * 0.035f);
        for (int i = 0; i < count; ++i)
        {
            const int j = (i + 1) % count;
            list.line(xy[i * 2], xy[i * 2 + 1], xy[j * 2], xy[j * 2 + 1], edge, color);
        }
    };
    shape({{1.0f, 0.0f}, {-0.48f, 0.0f}, {-0.75f, 0.62f}}, fold);                 // lower wing
    shape({{1.0f, 0.0f}, {-0.48f, 0.0f}, {-0.32f, 0.08f}, {-0.2f, 0.36f}}, fold); // keel
    shape({{1.0f, 0.0f}, {-0.75f, -0.62f}, {-0.48f, 0.0f}}, body);                // upper wing
}

void draw_mark(gfx::DrawList &list, const gfx::Rect &box)
{
    const float s = std::min(box.w, box.h);
    const gfx::Rect tile{box.cx() - s * 0.5f, box.cy() - s * 0.5f, s, s};
    list.gradient_rect(tile, s * 0.22f, rgb(palette::violet), rgb(palette::coral));
    const gfx::Rect card{tile.x + s * 0.16f, tile.y + s * 0.38f, s * 0.54f, s * 0.42f};
    list.rotated_rect(card, s * 0.06f, -0.15f, rgb(palette::cream));
    const gfx::Rect picture = card.inset(s * 0.045f);
    list.rotated_rect({picture.x, picture.y, picture.w, picture.h - s * 0.06f}, s * 0.03f, -0.15f,
                      rgb(palette::aqua));
    list.circle(picture.x + picture.w * 0.68f, picture.y + picture.h * 0.32f, s * 0.05f,
                rgb(palette::sun));
    draw_plane(list, tile.x + s * 0.74f, tile.y + s * 0.26f, s * 0.12f, -0.55f, rgb(0xffffff),
               rgb(palette::sun));
}

void draw_icon(gfx::DrawList &list, Icon icon, const gfx::Rect &box, gfx::Color ink)
{
    const float s = std::min(box.w, box.h);
    const float cx = box.cx();
    const float cy = box.cy();
    const float stroke = std::max(2.0f, s * 0.09f);
    const gfx::Color clear{0.0f, 0.0f, 0.0f, 0.0f};
    switch (icon)
    {
    case Icon::captures:
    {
        const gfx::Rect frame{cx - s * 0.46f, cy - s * 0.36f, s * 0.92f, s * 0.72f};
        list.bordered_rect(frame, s * 0.14f, clear, stroke, ink);
        list.circle(cx + s * 0.18f, cy - s * 0.1f, s * 0.09f, ink);
        list.triangle({cx - s * 0.36f, cy - s * 0.04f, s * 0.48f, s * 0.32f}, ink);
        list.triangle({cx - s * 0.06f, cy + s * 0.04f, s * 0.4f, s * 0.24f}, ink);
        break;
    }
    case Icon::auto_upload:
    {
        list.arc(cx, cy, s * 0.42f, stroke, 0.5f, 5.0f, ink);
        list.triangle({cx - s * 0.06f, cy - s * 0.555f, s * 0.24f, s * 0.24f}, ink, 0.0f,
                      kPi * 0.5f);
        // A little bolt in the middle: it does it by itself.
        list.line(cx + s * 0.06f, cy - s * 0.2f, cx - s * 0.08f, cy + s * 0.02f, stroke * 0.9f,
                  ink);
        list.line(cx - s * 0.08f, cy + s * 0.02f, cx + s * 0.08f, cy + s * 0.02f, stroke * 0.9f,
                  ink);
        list.line(cx + s * 0.08f, cy + s * 0.02f, cx - s * 0.06f, cy + s * 0.22f, stroke * 0.9f,
                  ink);
        break;
    }
    case Icon::destinations:
    case Icon::plane:
        draw_plane(list, cx, cy, s * 0.4f, -0.5f, ink, ink.with_alpha(ink.a * 0.55f));
        break;
    case Icon::history:
    case Icon::clock:
        list.ring(cx, cy, s * 0.42f, stroke, ink);
        list.line(cx, cy, cx, cy - s * 0.24f, stroke, ink);
        list.line(cx, cy, cx + s * 0.18f, cy + s * 0.08f, stroke, ink);
        break;
    case Icon::settings:
    {
        for (int i = 0; i < 8; ++i)
        {
            const float a = static_cast<float>(i) * kPi / 4.0f;
            const float tx = cx + std::sin(a) * s * 0.36f;
            const float ty = cy - std::cos(a) * s * 0.36f;
            list.rotated_rect({tx - s * 0.08f, ty - s * 0.08f, s * 0.16f, s * 0.16f}, s * 0.03f, a,
                              ink);
        }
        list.ring(cx, cy, s * 0.26f, s * 0.14f, ink);
        break;
    }
    case Icon::play:
        list.triangle({cx - s * 0.3f, cy - s * 0.34f, s * 0.68f, s * 0.68f}, ink, 0.0f, kPi * 0.5f);
        break;
    case Icon::check:
        list.line(cx - s * 0.32f, cy + s * 0.02f, cx - s * 0.08f, cy + s * 0.26f, stroke * 1.2f,
                  ink);
        list.line(cx - s * 0.08f, cy + s * 0.26f, cx + s * 0.34f, cy - s * 0.24f, stroke * 1.2f,
                  ink);
        break;
    case Icon::cross:
        list.line(cx - s * 0.26f, cy - s * 0.26f, cx + s * 0.26f, cy + s * 0.26f, stroke * 1.2f,
                  ink);
        list.line(cx + s * 0.26f, cy - s * 0.26f, cx - s * 0.26f, cy + s * 0.26f, stroke * 1.2f,
                  ink);
        break;
    case Icon::link:
        list.ring(cx - s * 0.16f, cy + s * 0.16f, s * 0.2f, stroke, ink);
        list.ring(cx + s * 0.16f, cy - s * 0.16f, s * 0.2f, stroke, ink);
        list.line(cx - s * 0.1f, cy + s * 0.1f, cx + s * 0.1f, cy - s * 0.1f, stroke, ink);
        break;
    case Icon::phone:
    {
        const gfx::Rect body{cx - s * 0.24f, cy - s * 0.44f, s * 0.48f, s * 0.88f};
        list.bordered_rect(body, s * 0.1f, clear, stroke, ink);
        list.circle(cx, cy + s * 0.3f, s * 0.05f, ink);
        break;
    }
    case Icon::upload:
        list.line(cx, cy + s * 0.18f, cx, cy - s * 0.3f, stroke, ink);
        list.line(cx, cy - s * 0.32f, cx - s * 0.2f, cy - s * 0.12f, stroke, ink);
        list.line(cx, cy - s * 0.32f, cx + s * 0.2f, cy - s * 0.12f, stroke, ink);
        list.line(cx - s * 0.36f, cy + s * 0.36f, cx + s * 0.36f, cy + s * 0.36f, stroke, ink);
        break;
    case Icon::refresh:
        list.arc(cx, cy, s * 0.4f, stroke, 0.9f, 5.0f, ink);
        list.triangle({cx + s * 0.02f, cy - s * 0.52f, s * 0.24f, s * 0.24f}, ink, 0.0f,
                      kPi * 0.5f);
        break;
    case Icon::lock:
        list.arc(cx, cy - s * 0.08f, s * 0.24f, stroke, -1.5708f, 3.1416f, ink);
        list.rounded_rect({cx - s * 0.32f, cy - s * 0.06f, s * 0.64f, s * 0.48f}, s * 0.08f, ink);
        break;
    }
}

bool QrCode::set(std::string_view text)
{
    if (text == text_ && size_ > 0)
        return true;
    text_.assign(text);
    std::vector<std::uint8_t> code(qrcodegen_BUFFER_LEN_MAX);
    std::vector<std::uint8_t> scratch(qrcodegen_BUFFER_LEN_MAX);
    if (!qrcodegen_encodeText(text_.c_str(), scratch.data(), code.data(), qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, 20, qrcodegen_Mask_AUTO, true))
    {
        size_ = 0;
        modules_.clear();
        return false;
    }
    size_ = qrcodegen_getSize(code.data());
    modules_.assign(static_cast<std::size_t>(size_) * size_, 0);
    for (int y = 0; y < size_; ++y)
        for (int x = 0; x < size_; ++x)
            modules_[static_cast<std::size_t>(y) * size_ + x] =
                qrcodegen_getModule(code.data(), x, y) ? 1 : 0;
    return true;
}

void QrCode::draw(gfx::DrawList &list, const gfx::Rect &box, gfx::Color dark,
                  gfx::Color light) const
{
    const float side = std::min(box.w, box.h);
    const gfx::Rect square{box.cx() - side * 0.5f, box.cy() - side * 0.5f, side, side};
    list.rounded_rect(square, side * 0.06f, light);
    if (size_ <= 0)
        return;
    const int quiet = 3;
    const float cell = side / static_cast<float>(size_ + quiet * 2);
    // Runs of dark modules along a row are one rectangle: a version-5 code
    // becomes a few hundred shapes instead of a thousand.
    for (int y = 0; y < size_; ++y)
    {
        int x = 0;
        while (x < size_)
        {
            if (!module(x, y))
            {
                ++x;
                continue;
            }
            int end = x;
            while (end < size_ && module(end, y))
                ++end;
            list.rounded_rect({square.x + (x + quiet) * cell, square.y + (y + quiet) * cell,
                               (end - x) * cell + 0.6f, cell + 0.6f},
                              0.0f, dark);
            x = end;
        }
    }
}

std::string friendly_time(std::int64_t unix_time, std::int64_t now)
{
    const LocalTime a = local_time(unix_time);
    const LocalTime b = local_time(now);
    char text[48];
    if (a.year == b.year && a.yday == b.yday)
        std::snprintf(text, sizeof(text), "Today %02d:%02d", a.hour, a.minute);
    else if (a.year == b.year && a.yday + 1 == b.yday)
        std::snprintf(text, sizeof(text), "Yesterday %02d:%02d", a.hour, a.minute);
    else
        std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d", a.year, a.month, a.day,
                      a.hour, a.minute);
    return text;
}

std::string friendly_duration(std::int64_t seconds)
{
    if (seconds < 60)
        return "just now";
    char text[32];
    if (seconds < 3600)
        std::snprintf(text, sizeof(text), "%lld min", static_cast<long long>(seconds / 60));
    else if (seconds < 86400)
        std::snprintf(text, sizeof(text), "%lld h %lld min", static_cast<long long>(seconds / 3600),
                      static_cast<long long>(seconds % 3600 / 60));
    else
        std::snprintf(text, sizeof(text), "%lld days", static_cast<long long>(seconds / 86400));
    return text;
}

} // namespace pshare::app
