// PShare - The app: a side rail, five pages, and what floats over them.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   Captures      every screenshot and clip, with thumbnails; send any of them
//   Auto upload   the background uploader (pshared): start, stop, phone setup
//   Destinations  Discord, Telegram, catbox, ... and their settings
//   History       every upload, its link as a QR code, retries
//   Settings      what gets sent and how, and about
//
// L1 / R1 change page from anywhere, Options opens the app menu. The app is
// immediate mode like the kit: update() owns the state, draw() is const.

#pragma once

#include "app/model.hpp"
#include "app/style.hpp"
#include "app/thumbs.hpp"
#include "app/video.hpp"
#include "core/input.hpp"
#include "gfx/renderer.hpp"
#include "ui/components.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pshare::app
{

using hui::Action;
using hui::Direction;
using hui::InputFrame;

// Pictures from assets/art (0 when missing: everything has a drawn fallback).
struct Art
{
    std::uint32_t backdrop = 0;
    std::uint32_t splash = 0;
    std::uint32_t logo = 0;
    std::uint32_t empty = 0;
    float empty_aspect = 640.0f / 440.0f;
};

class LinkPanel;

// What a page may use besides itself.
struct Context
{
    Model &model;
    Thumbs &thumbs;
    const ui::Fonts &fonts;
    const Art &art;
    ui::ToastStack &toasts;
    LinkPanel &link_panel;
    VideoPlayer *player = nullptr; // null where clips cannot play
    float clock = 0.0f;
    std::int64_t now = 0; // unix seconds
};

class Page
{
  public:
    virtual ~Page() = default;
    virtual const char *title() const = 0;
    virtual void layout(const gfx::Rect &area) = 0;
    // The page is shown (snap its components, replay entrances).
    virtual void enter(Context &context)
    {
        (void)context;
    }
    // It receives (or no longer receives) the input.
    virtual void set_focused(bool focused)
    {
        (void)focused;
    }
    // True hands the focus back to the side rail.
    virtual bool handle(const InputFrame &input, ui::Feedback &feedback, Context &context) = 0;
    virtual void update(float dt, ui::Feedback &feedback, Context &context) = 0;
    virtual void draw(ui::Canvas &scene, Context &context) const = 0;
    // Drawn over everything else, with the frosted copy of the frame.
    virtual void draw_overlay(ui::Canvas &overlay, Context &context) const
    {
        (void)overlay;
        (void)context;
    }
    // One of its overlays is up: the page keeps the input and L1/R1 wait.
    virtual bool modal() const
    {
        return false;
    }
    virtual bool wants_glass() const
    {
        return modal();
    }
    virtual int hints(ui::Hint *out, int capacity) const
    {
        (void)out;
        (void)capacity;
        return 0;
    }
    virtual void on_event(const ModelEvent &event, ui::Feedback &feedback, Context &context)
    {
        (void)event;
        (void)feedback;
        (void)context;
    }
    // Items the page puts at the top of the Options menu (tags from kPageMenuTag
    // up), and what choosing one does.
    static constexpr int kPageMenuTag = 100;
    virtual void menu_items(std::vector<ui::MenuItem> &items, Context &context) const
    {
        (void)items;
        (void)context;
    }
    virtual void menu_action(int tag, ui::Feedback &feedback, Context &context)
    {
        (void)tag;
        (void)feedback;
        (void)context;
    }
};

std::unique_ptr<Page> make_captures_page();
std::unique_ptr<Page> make_auto_page();
std::unique_ptr<Page> make_destinations_page();
std::unique_ptr<Page> make_history_page();
std::unique_ptr<Page> make_settings_page();

// A link (or the phone setup address) as a QR code in a frosted panel.
class LinkPanel
{
  public:
    void open(std::string title, std::string link, std::string note, ui::Feedback &feedback);
    void close(ui::Feedback &feedback);
    bool is_open() const
    {
        return open_;
    }
    bool visible() const
    {
        return open_ || fade_.value > 0.01f;
    }
    // Changes the link while open (the page can come up a moment later).
    void set_link(std::string link);
    // Back or confirm closes it: returns Event::cancelled then.
    ui::Event handle(const InputFrame &input, ui::Feedback &feedback);
    void update(float dt);
    void draw(ui::Canvas &canvas) const;

  private:
    std::string title_;
    std::string link_;
    std::string note_;
    QrCode qr_;
    bool open_ = false;
    hui::tween::Spring fade_;
    hui::tween::Bounce pop_;
};

class App
{
  public:
    App(Model &model, Thumbs &thumbs, const ui::Fonts &fonts, Art art,
        VideoPlayer *player = nullptr);

    void update(const InputFrame &input, float dt, ui::Feedback &feedback);
    void compose(gfx::Renderer &renderer);
    bool wants_quit() const
    {
        return quit_;
    }
    // Skip the intro (tests, the preview's scripted pictures).
    void skip_intro()
    {
        intro_ = 10.0f;
    }
    // For the preview: show a page with the focus in it.
    void open_page(int index, bool focus_content);
    Page &page(int index)
    {
        return *pages_[static_cast<std::size_t>(index)];
    }
    static constexpr int kPages = 5;

  private:
    void switch_page(int index, ui::Feedback &feedback, bool focus_content);
    void handle_event(const ModelEvent &event, ui::Feedback &feedback);
    void draw_scene(gfx::DrawList &list);
    void draw_overlay(gfx::DrawList &list, std::uint32_t glass);
    void draw_top_bar(ui::Canvas &canvas) const;
    void draw_upload_bar(ui::Canvas &canvas) const;
    void draw_hints(ui::Canvas &canvas) const;
    void draw_intro(ui::Canvas &canvas) const;
    void open_menu(ui::Feedback &feedback);

    Model &model_;
    Thumbs &thumbs_;
    const ui::Fonts &fonts_;
    Art art_;
    ui::ToastStack toasts_;
    LinkPanel link_panel_;
    Context context_;
    std::array<std::unique_ptr<Page>, kPages> pages_;
    ui::SideNav nav_;
    ui::Menu menu_;
    ui::Dialog delete_ask_; // a browser asks to delete captures (Model::delete_request)
    ui::ProgressBar upload_progress_;
    int current_ = 0;
    bool content_focus_ = false;
    bool quit_ = false;
    bool started_daemon_ = false;
    bool entered_ = false;
    float clock_ = 0.0f;
    float last_dt_ = 1.0f / 60.0f;
    VideoPlayer *player_ = nullptr;
    float intro_ = 0.0f;
    hui::tween::Spring page_fade_{1.0f, 0.0f, 1.0f};
    hui::tween::Spring upload_show_;
    UploadView upload_;
    gfx::DrawList scene_;
    gfx::DrawList overlay_;
};

} // namespace pshare::app
