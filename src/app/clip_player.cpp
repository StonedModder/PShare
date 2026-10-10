// PShare - The full-screen clip player over the Captures page.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/clip_player.hpp"

#include "app/style.hpp"

#include <cmath>
#include <cstdio>

namespace pshare::app
{
namespace
{
constexpr float kHideAfter = 3.0f;
constexpr gfx::Rect kStage{96.0f, 84.0f, 1728.0f, 820.0f};
} // namespace

std::string clock_text(double seconds)
{
    if (!(seconds >= 0.0))
        seconds = 0.0;
    const long whole = static_cast<long>(seconds);
    char text[32];
    if (whole >= 3600)
        std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", whole / 3600, whole / 60 % 60,
                      whole % 60);
    else
        std::snprintf(text, sizeof(text), "%ld:%02ld", whole / 60, whole % 60);
    return text;
}

ClipPlayer::ClipPlayer()
{
    bar_.style.theme = theme();
    bar_.style.height = 8.0f;
    bar_.style.placement = ui::LabelPlacement::none;
    bar_.style.radius_source = ui::RadiusSource::pill;
    bar_.style.finish_flash = false;
    spinner_.style.theme = theme();
    spinner_.style.kind = ui::SpinnerKind::orbit;
    spinner_.set_bounds({960.0f - 40.0f, kStage.cy() - 40.0f, 80.0f, 80.0f});
}

void ClipPlayer::open(VideoPlayer *player, const std::string &path, std::string title,
                      std::string subtitle, ui::Feedback &feedback)
{
    player_ = player;
    path_ = path;
    title_ = std::move(title);
    subtitle_ = std::move(subtitle);
    open_ = true;
    fade_.target = 1.0f;
    idle_ = 0.0f;
    controls_.snap(1.0f);
    bar_.set_value(0.0f, true);
    if (player_ != nullptr)
        player_->open(path);
    spinner_.set_spinning(true, true);
    feedback.play(hui::audio::Cue::open);
}

void ClipPlayer::close(ui::Feedback &feedback)
{
    if (!open_)
        return;
    open_ = false;
    fade_.target = 0.0f;
    if (player_ != nullptr)
        player_->close();
    feedback.play(hui::audio::Cue::back);
}

void ClipPlayer::poke()
{
    idle_ = 0.0f;
}

void ClipPlayer::jump(double seconds, ui::Feedback &feedback)
{
    if (player_ == nullptr)
        return;
    player_->seek(player_->position() + seconds);
    char text[16];
    std::snprintf(text, sizeof(text), "%+.0f s", seconds);
    jump_text_ = text;
    jump_flash_.trigger();
    feedback.play(hui::audio::Cue::slider, seconds > 0 ? 1.1f : 0.9f);
}

ClipPlayer::Request ClipPlayer::handle(const hui::InputFrame &input, ui::Feedback &feedback)
{
    if (!open_)
        return Request::none;
    if (input.pressed != 0 || input.nav != hui::Direction::none)
        poke();
    if (input.is_pressed(hui::Action::back))
    {
        close(feedback);
        return Request::closed;
    }
    if (player_ == nullptr)
        return Request::none;
    if (input.is_pressed(hui::Action::confirm))
    {
        if (player_->ended())
        {
            player_->seek(0.0);
            player_->set_paused(false);
        }
        else
        {
            player_->set_paused(!player_->paused());
        }
        flash_play_ = !player_->paused();
        flash_.trigger();
        feedback.play(hui::audio::Cue::toggle);
    }
    // The same buttons as VLC for PS5 (ZiZc3/VLC) where the two overlap:
    // left/right 10 s, L2/R2 30 s, L1/R1 the previous/next file.
    else if (input.nav == hui::Direction::left)
        jump(-10.0, feedback);
    else if (input.nav == hui::Direction::right)
        jump(10.0, feedback);
    else if (input.is_pressed(hui::Action::page_prev))
        return Request::previous;
    else if (input.is_pressed(hui::Action::page_next))
        return Request::next;
    else if (input.is_pressed(hui::Action::jump_prev))
        jump(-30.0, feedback);
    else if (input.is_pressed(hui::Action::jump_next))
        jump(30.0, feedback);
    else if (input.is_pressed(hui::Action::west))
        return Request::send;
    else if (input.is_pressed(hui::Action::north))
        return Request::send_to;
    return Request::none;
}

void ClipPlayer::update(float dt)
{
    clock_ += dt;
    fade_.update(dt, open_ ? 12.0f : 20.0f);
    flash_.update(dt, 3.0f);
    jump_flash_.update(dt, 2.5f);
    const bool playing = player_ != nullptr && !player_->paused() && !player_->loading();
    idle_ += dt;
    controls_.target = (!playing || idle_ < kHideAfter) ? 1.0f : 0.0f;
    controls_.update(dt, 8.0f);
    if (player_ != nullptr)
    {
        const double length = player_->duration();
        if (length > 0.0)
            bar_.set_value(static_cast<float>(player_->position() / length));
        spinner_.set_spinning(player_->loading());
    }
    bar_.update(dt);
    spinner_.update(dt);
}

void ClipPlayer::draw(ui::Canvas &canvas) const
{
    const float alpha = fade_.value;
    if (alpha < 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    list.push_opacity(alpha);
    list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                      rgb(0x05040f, 0.97f));
    const std::string error = player_ != nullptr ? player_->error() : "No player in this build";
    if (player_ != nullptr && player_->texture() != 0 && error.empty())
    {
        // Fit the picture into the stage.
        const float aspect = player_->aspect();
        float w = kStage.w;
        float h = w / aspect;
        if (h > kStage.h)
        {
            h = kStage.h;
            w = h * aspect;
        }
        const gfx::Rect picture{kStage.cx() - w * 0.5f, kStage.cy() - h * 0.5f, w, h};
        list.shadow({picture.x, picture.y + 12.0f, picture.w, picture.h}, 18.0f, 40.0f,
                    rgb(0x000000, 0.6f));
        list.image(player_->texture(), picture, gfx::kFullUv, rgb(0xffffff), 14.0f);
    }
    else if (!error.empty())
    {
        draw_icon(list, Icon::cross, {960.0f - 40.0f, kStage.cy() - 90.0f, 80.0f, 80.0f},
                  rgb(0xff8a95));
        ui::text(list, canvas.fonts.semibold, error, 960.0f, kStage.cy() + 30.0f, 28.0f,
                 rgb(palette::cream), gfx::Align::center);
    }
    spinner_.draw(canvas);

    // The play / pause mark, flashing on each press.
    if (flash_.value > 0.01f)
    {
        const float s = 120.0f + 30.0f * (1.0f - flash_.value);
        list.circle(960.0f, kStage.cy(), s * 0.6f, rgb(palette::ink, 0.55f * flash_.value));
        if (flash_play_)
            draw_icon(list, Icon::play,
                      {960.0f - s * 0.35f, kStage.cy() - s * 0.35f, s * 0.7f, s * 0.7f},
                      rgb(palette::cream, flash_.value));
        else
        {
            list.rounded_rect({960.0f - s * 0.2f, kStage.cy() - s * 0.25f, s * 0.13f, s * 0.5f},
                              4.0f, rgb(palette::cream, flash_.value));
            list.rounded_rect({960.0f + s * 0.07f, kStage.cy() - s * 0.25f, s * 0.13f, s * 0.5f},
                              4.0f, rgb(palette::cream, flash_.value));
        }
    }
    if (jump_flash_.value > 0.01f)
        ui::text(list, canvas.fonts.semibold, jump_text_, 960.0f, kStage.cy() + 130.0f, 34.0f,
                 rgb(palette::sun, jump_flash_.value), gfx::Align::center);

    // Title and controls: fade away while it plays.
    const float c = controls_.value;
    if (c > 0.01f)
    {
        list.push_opacity(c);
        list.gradient_rect({0.0f, 0.0f, gfx::kVirtualWidth, 180.0f}, 0.0f, rgb(0x000000, 0.65f),
                           rgb(0x000000, 0.0f));
        list.gradient_rect({0.0f, 860.0f, gfx::kVirtualWidth, 220.0f}, 0.0f, rgb(0x000000, 0.0f),
                           rgb(0x000000, 0.75f));
        ui::text(list, canvas.fonts.display, title_, 96.0f, 92.0f, 40.0f, rgb(palette::cream));
        std::string info = subtitle_;
        if (player_ != nullptr && !player_->description().empty())
            info += "   ·   " + player_->description();
        if (player_ != nullptr && !player_->has_audio() && !player_->loading())
            info += "   ·   no sound";
        ui::text(list, canvas.fonts.regular, info, 96.0f, 130.0f, 22.0f, rgb(palette::mist));

        const double position = player_ != nullptr ? player_->position() : 0.0;
        const double length = player_ != nullptr ? player_->duration() : 0.0;
        const float y = 948.0f;
        const bool paused = player_ == nullptr || player_->paused();
        const gfx::Rect button{96.0f, y - 30.0f, 60.0f, 60.0f};
        list.circle(button.cx(), button.cy(), 30.0f, rgb(palette::coral));
        if (paused)
            draw_icon(list, Icon::play, {button.x + 15.0f, button.y + 14.0f, 32.0f, 32.0f},
                      rgb(0x2a0716));
        else
        {
            list.rounded_rect({button.cx() - 11.0f, button.cy() - 12.0f, 7.0f, 24.0f}, 2.0f,
                              rgb(0x2a0716));
            list.rounded_rect({button.cx() + 4.0f, button.cy() - 12.0f, 7.0f, 24.0f}, 2.0f,
                              rgb(0x2a0716));
        }
        ui::text(list, canvas.fonts.mono, clock_text(position), 184.0f, y + 8.0f, 24.0f,
                 rgb(palette::cream));
        ui::text(list, canvas.fonts.mono, length > 0.0 ? clock_text(length) : "--:--", 1824.0f,
                 y + 8.0f, 24.0f, rgb(palette::mist), gfx::Align::right);
        ui::ProgressBar bar = bar_;
        bar.set_bounds({300.0f, y - 7.0f, 1400.0f, 14.0f});
        bar.draw(canvas);
        const ui::Hint hints[] = {{ui::Button::cross, paused ? "Play" : "Pause"},
                                  {ui::Button::dpad, "10 s"},
                                  {ui::Button::l2, "30 s", ui::Button::r2},
                                  {ui::Button::l1, "Clips", ui::Button::r1},
                                  {ui::Button::square, "Send"},
                                  {ui::Button::triangle, "Send to..."},
                                  {ui::Button::circle, "Back"}};
        ui::HintLayout layout;
        layout.size = 32.0f;
        layout.text_size = 21.0f;
        layout.cy = 1030.0f;
        layout.item_gap = 34.0f;
        layout.font = &canvas.fonts.semibold;
        ui::draw_hints(list, canvas.fonts, ui::GlyphStyle::dark(), hints, 7, 96.0f, false, layout);
        list.pop_opacity();
    }
    list.pop_opacity();
}

} // namespace pshare::app
