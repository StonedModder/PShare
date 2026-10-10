// PShare - The full-screen clip player over the Captures page.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross plays and pauses, left / right jump 5 s, L2 / R2 jump 30 s, Square
// sends the clip, Triangle picks where, Circle closes. The controls fade after
// a few idle seconds and come back with any button.

#pragma once

#include "app/video.hpp"
#include "core/input.hpp"
#include "ui/components.hpp"

#include <string>

namespace pshare::app
{

class ClipPlayer
{
  public:
    enum class Request : std::uint8_t
    {
        none,
        send,
        send_to,
        closed,
        previous, // L1: the newer clip
        next,     // R1: the older clip
    };

    ClipPlayer();

    void open(VideoPlayer *player, const std::string &path, std::string title, std::string subtitle,
              hui::ui::Feedback &feedback);
    void close(hui::ui::Feedback &feedback);
    bool is_open() const
    {
        return open_;
    }
    bool visible() const
    {
        return open_ || fade_.value > 0.01f;
    }
    const std::string &path() const
    {
        return path_;
    }

    Request handle(const hui::InputFrame &input, hui::ui::Feedback &feedback);
    void update(float dt);
    void draw(hui::ui::Canvas &canvas) const;

  private:
    void poke(); // shows the controls again
    void jump(double seconds, hui::ui::Feedback &feedback);

    VideoPlayer *player_ = nullptr;
    std::string path_;
    std::string title_;
    std::string subtitle_;
    bool open_ = false;
    float idle_ = 0.0f;
    float clock_ = 0.0f;
    hui::tween::Spring fade_;
    hui::tween::Spring controls_{1.0f, 0.0f, 1.0f};
    hui::ui::Pulse flash_; // the play / pause mark in the middle
    bool flash_play_ = true;
    std::string jump_text_;
    hui::ui::Pulse jump_flash_;
    hui::ui::ProgressBar bar_;
    hui::ui::Spinner spinner_;
};

// "1:05", "12:40", "1:02:03"
std::string clock_text(double seconds);

} // namespace pshare::app
