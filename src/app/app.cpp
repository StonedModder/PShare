// PShare - The app: a side rail, five pages, and what floats over them.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include "pshare/core/text.hpp"
#include "ui/widgets.hpp"

#include <cstdio>
#include <ctime>

namespace pshare::app
{
namespace
{
constexpr float kSafe = 96.0f;
constexpr gfx::Rect kContent{424.0f, 172.0f, 1400.0f, 800.0f};
constexpr float kIntroSeconds = 1.7f;
constexpr const char *kStartDaemonFlag = "start_uploader";

enum MenuTag
{
    menu_rescan = 1,
    menu_cancel = 2,
    menu_quit = 3,
};

std::string clock_text(std::int64_t now)
{
    const LocalTime parts = local_time(now);
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d", parts.hour, parts.minute);
    return text;
}
} // namespace

App::App(Model &model, Thumbs &thumbs, const ui::Fonts &fonts, Art art, VideoPlayer *player)
    : model_(model), thumbs_(thumbs), fonts_(fonts), art_(art),
      context_{model_, thumbs_, fonts_, art_, toasts_, link_panel_, player, 0.0f, 0},
      player_(player)
{
    pages_[0] = make_captures_page();
    pages_[1] = make_auto_page();
    pages_[2] = make_destinations_page();
    pages_[3] = make_history_page();
    pages_[4] = make_settings_page();
    for (auto &page : pages_)
        page->layout(kContent);

    nav_.style.theme = theme();
    nav_.style.expanded_width = 300.0f;
    nav_.style.row_height = 64.0f;
    nav_.style.gap = 8.0f;
    nav_.style.icon_size = 30.0f;
    nav_.style.text_size = 25.0f;
    nav_.style.highlight.kind = ui::HighlightKind::fill;
    nav_.style.footer = true;
    nav_.icon = [](ui::Canvas &canvas, const gfx::Rect &box, const ui::NavEntry &entry, int, float,
                   gfx::Color ink)
    { draw_icon(canvas.list, static_cast<Icon>(entry.tag), box, ink); };
    std::vector<ui::NavEntry> entries(kPages);
    const char *labels[kPages] = {"Captures", "Auto upload", "Destinations", "History", "Settings"};
    const Icon icons[kPages] = {Icon::captures, Icon::auto_upload, Icon::destinations,
                                Icon::history, Icon::settings};
    for (int i = 0; i < kPages; ++i)
    {
        entries[static_cast<std::size_t>(i)].label = labels[i];
        entries[static_cast<std::size_t>(i)].tag = static_cast<int>(icons[i]);
    }
    nav_.set_entries(std::move(entries));
    nav_.set_bounds({72.0f, kContent.y, 0.0f, kContent.h});
    nav_.set_focus(0, true);
    nav_.set_current(0, true);

    menu_.style.theme = theme();
    menu_.style.width = 420.0f;
    menu_.style.scrim = 0.25f;

    toasts_.style.theme = theme();
    toasts_.style.anchor = ui::ToastAnchor::bottom_right;
    toasts_.style.width = 560.0f;
    toasts_.style.margin = 40.0f;
    toasts_.style.frosted = true;
    toasts_.style.duration = 4.5f;
    toasts_.style.max_visible = 2;
    // Above the hint row, which stays readable.
    toasts_.set_bounds({0.0f, 0.0f, gfx::kVirtualWidth, 1000.0f});

    upload_progress_.style.theme = theme();
    upload_progress_.style.height = 8.0f;
    upload_progress_.style.placement = ui::LabelPlacement::none;
    upload_progress_.style.radius_source = ui::RadiusSource::pill;
    upload_progress_.style.sheen = true;

    content_focus_ = true;
    nav_.set_focused(false);
    pages_[0]->set_focused(true);
}

void App::open_page(int index, bool focus_content)
{
    ui::Feedback feedback;
    switch_page(index, feedback, focus_content);
}

void App::switch_page(int index, ui::Feedback &feedback, bool focus_content)
{
    index = (index % kPages + kPages) % kPages;
    if (index != current_)
    {
        pages_[static_cast<std::size_t>(current_)]->set_focused(false);
        current_ = index;
        page_fade_.snap(0.0f);
        page_fade_.target = 1.0f;
        pages_[static_cast<std::size_t>(current_)]->enter(context_);
        nav_.set_current(index);
        nav_.set_focus(index, false);
    }
    content_focus_ = focus_content;
    nav_.set_focused(!focus_content);
    pages_[static_cast<std::size_t>(current_)]->set_focused(focus_content);
    (void)feedback;
}

void App::open_menu(ui::Feedback &feedback)
{
    std::vector<ui::MenuItem> items;
    ui::MenuItem rescan;
    rescan.label = "Look for new captures";
    rescan.tag = menu_rescan;
    items.push_back(rescan);
    ui::MenuItem cancel;
    cancel.label = "Cancel uploads";
    cancel.tag = menu_cancel;
    cancel.disabled = !upload_.active && upload_.queued == 0;
    items.push_back(cancel);
    ui::MenuItem separator;
    separator.separator = true;
    items.push_back(separator);
    ui::MenuItem quit;
    quit.label = "Close PShare";
    quit.tag = menu_quit;
    items.push_back(quit);
    menu_.set_items(std::move(items));
    menu_.open({1824.0f - 420.0f, 40.0f, 420.0f, 72.0f}, feedback);
}

void App::handle_event(const ModelEvent &event, ui::Feedback &feedback)
{
    switch (event.kind)
    {
    case EventKind::upload_finished:
        toasts_.push(event.ok ? ui::StatusKind::success : ui::StatusKind::danger, event.title,
                     event.detail);
        break;
    case EventKind::daemon_started:
        toasts_.push(ui::StatusKind::success, event.title,
                     "New captures are sent even after you close PShare");
        break;
    case EventKind::daemon_failed:
        toasts_.push(ui::StatusKind::danger, event.title, event.detail);
        break;
    case EventKind::daemon_stopped:
        toasts_.push(ui::StatusKind::info, event.title);
        break;
    case EventKind::config_failed:
        toasts_.push(ui::StatusKind::danger, event.title,
                     "PShare has no write access to /data/pshare");
        break;
    case EventKind::deleted:
        toasts_.push(event.ok ? ui::StatusKind::success : ui::StatusKind::danger, event.title,
                     event.detail);
        break;
    case EventKind::config_saved:
    case EventKind::scanned:
    case EventKind::unreadable:
    case EventKind::note:
        break;
    }
    for (auto &page : pages_)
        page->on_event(event, feedback, context_);
}

void App::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    last_dt_ = dt;
    context_.clock = clock_;
    context_.now = static_cast<std::int64_t>(std::time(nullptr));
    if (!entered_)
    {
        // Every page builds its rows once, so the first look at each is ready.
        entered_ = true;
        for (auto &page : pages_)
            page->enter(context_);
        pages_[static_cast<std::size_t>(current_)]->set_focused(content_focus_);
    }
    for (const ModelEvent &event : model_.pump())
        handle_event(event, feedback);
    upload_ = model_.upload_view();

    // The background uploader starts with the app unless the player said no.
    if (!started_daemon_ && clock_ > 1.0f)
    {
        started_daemon_ = true;
        if (model_.app_flag(kStartDaemonFlag, true))
            model_.start_daemon();
    }

    if (intro_ < kIntroSeconds)
    {
        intro_ += dt;
        if (intro_ < kIntroSeconds * 0.6f)
        {
            // The intro eats the input until it starts to fade.
            nav_.update(dt);
            for (auto &page : pages_)
                page->update(dt, feedback, context_);
            toasts_.update(dt, feedback);
            return;
        }
        if (intro_ >= kIntroSeconds * 0.6f && intro_ - dt < kIntroSeconds * 0.6f)
            feedback.play(hui::audio::Cue::welcome);
    }

    Page &page = *pages_[static_cast<std::size_t>(current_)];
    if (link_panel_.is_open())
    {
        if (link_panel_.handle(input, feedback) == ui::Event::cancelled &&
            model_.daemon().setup_port > 0)
            model_.close_phone_setup();
    }
    else if (menu_.is_open())
    {
        if (menu_.handle(input, feedback) == ui::Event::activated)
        {
            const int tag = menu_.item(menu_.focus()).tag;
            if (tag == menu_rescan)
            {
                model_.rescan();
                toasts_.push(ui::StatusKind::info, "Looking for new captures");
            }
            else if (tag == menu_cancel)
            {
                model_.cancel_upload();
            }
            else if (tag == menu_quit)
            {
                quit_ = true;
            }
        }
    }
    else if (input.is_pressed(Action::menu) && !page.modal())
    {
        open_menu(feedback);
    }
    else if (!page.modal() &&
             (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next)))
    {
        const int step = input.is_pressed(Action::page_next) ? 1 : -1;
        hui::ui::play_cue(feedback, nav_.style, nav_.style.sounds.page);
        switch_page(current_ + step, feedback, content_focus_);
    }
    else if (content_focus_)
    {
        if (page.handle(input, feedback, context_))
            switch_page(current_, feedback, false);
    }
    else
    {
        const ui::Event event = nav_.handle(input, feedback);
        if (event == ui::Event::moved)
        {
            switch_page(nav_.focus(), feedback, false);
        }
        else if (event == ui::Event::activated ||
                 (input.nav == Direction::right && !input.nav_repeat))
        {
            switch_page(nav_.focus(), feedback, true);
        }
    }

    nav_.update(dt);
    page_fade_.update(dt, 14.0f);
    for (auto &each : pages_)
        each->update(dt, feedback, context_);
    link_panel_.update(dt);
    menu_.update(dt);
    toasts_.update(dt, feedback);
    upload_show_.target = upload_.active ? 1.0f : 0.0f;
    upload_show_.update(dt, 10.0f);
    if (upload_.active && upload_.total > 0)
        upload_progress_.set_value(static_cast<float>(upload_.sent) /
                                   static_cast<float>(upload_.total));
    else if (!upload_.active)
        upload_progress_.set_value(0.0f, true);
    upload_progress_.update(dt);
}

// ---- drawing ---------------------------------------------------------------

void App::draw_top_bar(ui::Canvas &canvas) const
{
    gfx::DrawList &list = canvas.list;
    const gfx::Rect logo{kSafe - 8.0f, 44.0f, 64.0f, 64.0f};
    if (art_.logo != 0)
        list.image(art_.logo, logo, gfx::kFullUv, rgb(0xffffff));
    else
        draw_mark(list, logo);
    const float width = ui::text(list, fonts_.display, "P", logo.x + logo.w + 18.0f, 92.0f, 40.0f,
                                 rgb(palette::cream));
    const float share = ui::text(list, fonts_.display, "Share", logo.x + logo.w + 18.0f + width,
                                 92.0f, 40.0f, rgb(palette::coral));
    ui::text(list, fonts_.regular, "Made by StonedModder",
             logo.x + logo.w + 18.0f + width + share + 22.0f, 90.0f, 22.0f, rgb(palette::mist));

    // Right: the uploader's state and the time.
    const DaemonStatus &daemon = model_.daemon();
    std::string state = "Uploader off";
    gfx::Color dot = rgb(palette::mist, 0.8f);
    if (model_.daemon_starting())
    {
        state = "Uploader starting";
        dot = rgb(palette::sun);
    }
    else if (daemon.reachable)
    {
        state = daemon.state == "paused" ? "Uploader paused" : "Uploader on";
        dot = daemon.state == "paused" ? rgb(palette::sun) : rgb(palette::aqua);
    }
    const std::string time = clock_text(context_.now);
    const float right = gfx::kVirtualWidth - kSafe;
    const float time_width = fonts_.semibold.measure(time, 28.0f);
    ui::text(list, fonts_.semibold, time, right, 88.0f, 28.0f, rgb(palette::cream),
             gfx::Align::right);
    const float chip_w = fonts_.semibold.measure(state, 22.0f) + 64.0f;
    const gfx::Rect chip{right - time_width - 28.0f - chip_w, 54.0f, chip_w, 46.0f};
    list.rounded_rect(chip, 23.0f, rgb(0xffffff, 0.08f));
    list.bordered_rect(chip, 23.0f, gfx::Color{0, 0, 0, 0}, 1.5f, rgb(0xffffff, 0.14f));
    const float pulse = daemon.reachable ? 0.5f + 0.5f * ui::breathe(clock_) : 0.0f;
    if (daemon.reachable)
        list.glow({chip.x + 18.0f, chip.cy() - 9.0f, 18.0f, 18.0f}, 9.0f, 10.0f,
                  dot.with_alpha(0.35f * pulse));
    list.circle(chip.x + 27.0f, chip.cy(), 8.0f, dot);
    ui::text(list, fonts_.semibold, state, chip.x + 46.0f, chip.cy() + 8.0f, 22.0f,
             rgb(palette::cream));
    if (!model_.local_ip().empty())
        ui::text(list, fonts_.regular, model_.local_ip(), chip.x - 24.0f, 86.0f, 22.0f,
                 rgb(palette::mist), gfx::Align::right);
}

void App::draw_upload_bar(ui::Canvas &canvas) const
{
    const float show = upload_show_.value;
    if (show < 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    const gfx::Rect bar{620.0f, 40.0f - (1.0f - show) * 30.0f, 640.0f, 78.0f};
    list.push_opacity(show);
    list.shadow({bar.x, bar.y + 8.0f, bar.w, bar.h}, 24.0f, 24.0f, rgb(palette::ink, 0.5f));
    if (canvas.glass != 0)
        list.glass(canvas.glass, bar, 24.0f, rgb(0xffffff));
    list.rounded_rect(bar, 24.0f, rgb(palette::night_2, canvas.glass != 0 ? 0.7f : 0.95f));
    list.bordered_rect(bar, 24.0f, gfx::Color{0, 0, 0, 0}, 1.5f, rgb(0xffffff, 0.16f));
    const float lift = std::sin(clock_ * 6.0f) * 3.0f;
    draw_plane(list, bar.x + 42.0f, bar.y + 39.0f + lift, 15.0f, -0.45f, rgb(palette::sun),
               rgb(palette::coral));
    const UploadJob &job = upload_.job;
    std::string line = std::string(job.video ? "Sending clip" : "Sending screenshot") + " to " +
                       job.destination_name;
    if (upload_.stage == 2)
        line = "Hosting the big file first";
    else if (upload_.stage == 3)
        line = "Posting the link to " + job.destination_name;
    else if (upload_.stage == 4)
        line = "Trying again";
    ui::text(list, fonts_.semibold, fonts_.semibold.font->fit(line, 22.0f, 380.0f), bar.x + 76.0f,
             bar.y + 32.0f, 22.0f, rgb(palette::cream));
    char amount[64];
    const int percent = upload_.total > 0
                            ? static_cast<int>(100.0 * static_cast<double>(upload_.sent) /
                                               static_cast<double>(upload_.total))
                            : 0;
    std::snprintf(amount, sizeof(amount), "%d%%", percent);
    std::string detail = job.game + " · " + human_size(job.size);
    if (upload_.queued > 0)
        detail += " · " + std::to_string(upload_.queued) + " waiting";
    ui::text(list, fonts_.mono, amount, bar.x + bar.w - 24.0f, bar.y + 32.0f, 22.0f,
             rgb(palette::aqua), gfx::Align::right);
    ui::text(list, fonts_.regular, fonts_.regular.font->fit(detail, 18.0f, 380.0f), bar.x + 76.0f,
             bar.y + 58.0f, 18.0f, rgb(palette::mist));
    ui::ProgressBar progress = upload_progress_;
    progress.set_bounds({bar.x + 470.0f, bar.y + 46.0f, 146.0f, 14.0f});
    progress.draw(canvas);
    list.pop_opacity();
}

void App::draw_hints(ui::Canvas &canvas) const
{
    ui::Hint hints[8];
    int count = 0;
    const Page &page = *pages_[static_cast<std::size_t>(current_)];
    if (link_panel_.is_open())
    {
        hints[count++] = {ui::Button::circle, "Close"};
    }
    else if (menu_.is_open())
    {
        hints[count++] = {ui::Button::cross, "Select"};
        hints[count++] = {ui::Button::circle, "Close"};
    }
    else if (content_focus_)
    {
        count = page.hints(hints, 6);
    }
    else
    {
        hints[count++] = {ui::Button::cross, "Open"};
    }
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 22.0f;
    layout.cy = 1024.0f;
    layout.item_gap = 36.0f;
    layout.font = &fonts_.semibold;
    const ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
    ui::draw_hints(canvas.list, fonts_, glyphs, hints, count, kSafe, false, layout);
    if (!page.modal() && !link_panel_.is_open() && !menu_.is_open())
    {
        const ui::Hint right[] = {{ui::Button::l1, "Sections", ui::Button::r1},
                                  {ui::Button::options, "Menu"}};
        ui::draw_hints(canvas.list, fonts_, glyphs, right, 2, gfx::kVirtualWidth - kSafe, true,
                       layout);
    }
}

void App::draw_intro(ui::Canvas &canvas) const
{
    if (intro_ >= kIntroSeconds)
        return;
    gfx::DrawList &list = canvas.list;
    const float t = intro_ / kIntroSeconds;
    const float fade = 1.0f - hui::tween::smoothstep((t - 0.62f) / 0.38f);
    list.push_opacity(fade);
    const gfx::Rect screen{0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight};
    if (art_.splash != 0)
        list.image(art_.splash, screen, gfx::kFullUv, rgb(0xffffff));
    else
        list.gradient_rect(screen, 0.0f, rgb(palette::night_2), rgb(palette::ink));
    // The mark pops, then the name arrives.
    const float pop = hui::tween::back_out(hui::tween::clamp01(t / 0.35f));
    const float size = 330.0f * (0.6f + 0.4f * pop);
    const gfx::Rect mark{960.0f - size * 0.5f, 454.0f - size * 0.5f, size, size};
    list.glow(mark.inset(30.0f), size * 0.2f, 60.0f, rgb(palette::coral, 0.35f * pop));
    if (art_.logo != 0)
        list.image(art_.logo, mark, gfx::kFullUv, rgb(0xffffff, hui::tween::clamp01(t * 4.0f)));
    else
        draw_mark(list, mark);
    const float name = hui::tween::cubic_out(hui::tween::clamp01((t - 0.18f) / 0.3f));
    list.push_opacity(name);
    const float p = fonts_.display.measure("P", 84.0f);
    const float whole = fonts_.display.measure("PShare", 84.0f);
    const float x = 960.0f - whole * 0.5f;
    const float y = 760.0f + (1.0f - name) * 24.0f;
    ui::text(list, fonts_.display, "P", x, y, 84.0f, rgb(palette::cream));
    ui::text(list, fonts_.display, "Share", x + p, y, 84.0f, rgb(palette::coral));
    ui::text(list, fonts_.semibold, "Your captures, sent the moment you take them", 960.0f,
             y + 62.0f, 26.0f, rgb(palette::mist), gfx::Align::center);
    list.pop_opacity();
    list.pop_opacity();
}

void App::draw_scene(gfx::DrawList &list)
{
    ui::Canvas canvas{list, fonts_, 0, clock_};
    if (art_.backdrop != 0)
        list.image(art_.backdrop, {0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight},
                   gfx::kFullUv, rgb(0xffffff));
    draw_top_bar(canvas);
    nav_.draw(canvas);
    const Page &page = *pages_[static_cast<std::size_t>(current_)];
    const float fade = page_fade_.value;
    list.push_opacity(fade);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - fade) * 18.0f);
    page.draw(canvas, context_);
    list.pop_transform();
    list.pop_opacity();
    // Elevation refused: say so once, quietly, where it explains the empty pages.
    if (!model_.environment().elevated)
    {
        const gfx::Rect note{kContent.x, kContent.y + kContent.h + 6.0f, kContent.w, 0.0f};
        ui::text(list, fonts_.regular,
                 "No file access in the app (" + model_.environment().elevation_note +
                     "). The background uploader still works.",
                 note.x, note.y + 22.0f, 19.0f, rgb(palette::sun, 0.9f));
    }
    draw_hints(canvas);
}

void App::draw_overlay(gfx::DrawList &list, std::uint32_t glass)
{
    ui::Canvas canvas{list, fonts_, glass, clock_};
    pages_[static_cast<std::size_t>(current_)]->draw_overlay(canvas, context_);
    draw_upload_bar(canvas);
    link_panel_.draw(canvas);
    menu_.draw(canvas);
    toasts_.draw(canvas);
    draw_intro(canvas);
}

void App::compose(gfx::Renderer &renderer)
{
    scene_.clear();
    overlay_.clear();
    draw_scene(scene_);
    // The clip's next picture goes up before anything draws it.
    if (player_ != nullptr)
        player_->update(last_dt_, renderer.batch());
    renderer.begin();
    gfx::BackdropSpec sky = theme().backdrop;
    sky.time = clock_;
    if (art_.backdrop == 0)
        renderer.backdrop(sky);
    renderer.draw(scene_);
    const bool glass = pages_[static_cast<std::size_t>(current_)]->wants_glass() ||
                       link_panel_.visible() || menu_.visible() || upload_show_.value > 0.01f ||
                       !toasts_.empty();
    if (glass)
        renderer.glass();
    draw_overlay(overlay_, glass ? renderer.glass_texture() : 0);
    renderer.draw(overlay_);
}

// ---- the link panel --------------------------------------------------------

void LinkPanel::open(std::string title, std::string link, std::string note, ui::Feedback &feedback)
{
    title_ = std::move(title);
    note_ = std::move(note);
    set_link(std::move(link));
    open_ = true;
    pop_.snap(0.0f);
    pop_.target = 1.0f;
    fade_.target = 1.0f;
    feedback.play(hui::audio::Cue::modal_open);
}

void LinkPanel::set_link(std::string link)
{
    link_ = std::move(link);
    if (!link_.empty())
        qr_.set(link_);
}

void LinkPanel::close(ui::Feedback &feedback)
{
    if (!open_)
        return;
    open_ = false;
    fade_.target = 0.0f;
    pop_.target = 0.0f;
    feedback.play(hui::audio::Cue::modal_close);
}

ui::Event LinkPanel::handle(const InputFrame &input, ui::Feedback &feedback)
{
    if (input.is_pressed(Action::back) || input.is_pressed(Action::confirm))
    {
        close(feedback);
        return ui::Event::cancelled;
    }
    return ui::Event::none;
}

void LinkPanel::update(float dt)
{
    fade_.update(dt, open_ ? 14.0f : 22.0f);
    pop_.update(dt, 16.0f, 0.62f);
}

void LinkPanel::draw(ui::Canvas &canvas) const
{
    const float alpha = fade_.value;
    if (alpha < 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                      rgb(palette::ink, 0.6f * alpha));
    const float scale = 0.92f + 0.08f * pop_.value;
    const gfx::Rect panel{960.0f - 520.0f, 540.0f - 270.0f, 1040.0f, 540.0f};
    list.push_opacity(alpha);
    list.push_transform(scale, panel.cx(), panel.cy(), 0.0f, (1.0f - pop_.value) * 20.0f);
    ui::draw_overlay_panel(canvas, theme(), panel, canvas.glass != 0, 0.62f, 34.0f);
    const gfx::Rect code{panel.x + 48.0f, panel.y + 60.0f, 420.0f, 420.0f}; // 60 above and below
    if (qr_.size() > 0 && !link_.empty())
    {
        list.glow(code.inset(20.0f), 30.0f, 40.0f, rgb(palette::aqua, 0.25f));
        qr_.draw(list, code, rgb(palette::ink), rgb(0xffffff));
    }
    else
    {
        list.rounded_rect(code, 26.0f, rgb(0xffffff, 0.08f));
        ui::text(list, canvas.fonts.semibold, "Getting ready...", code.cx(), code.cy() + 10.0f,
                 26.0f, rgb(palette::mist), gfx::Align::center);
    }
    const float x = code.x + code.w + 48.0f;
    const float width = panel.x + panel.w - 48.0f - x;
    float y = panel.y + 100.0f;
    ui::Painter paint(list, canvas.fonts, theme(), canvas.glass);
    paint.heading(fit_label(paint, title_, 40.0f, width), x, y, 40.0f, rgb(palette::cream));
    y += 54.0f;
    if (!link_.empty())
    {
        // An address has no spaces to wrap at: break it where it fits, after
        // a '/', '?', '&' or '=' when there is one on the line.
        constexpr float kSize = 24.0f;
        y += 10.0f;
        std::string_view rest = link_;
        for (int line = 0; line < 4 && !rest.empty(); ++line)
        {
            std::size_t fits = rest.size();
            while (fits > 1 && canvas.fonts.mono.measure(rest.substr(0, fits), kSize) > width)
                --fits;
            std::size_t cut = fits;
            if (fits < rest.size())
                for (std::size_t i = fits; i > fits / 2; --i)
                    if (std::string_view("/?&=").find(rest[i - 1]) != std::string_view::npos)
                    {
                        cut = i;
                        break;
                    }
            ui::text(list, canvas.fonts.mono, rest.substr(0, cut), x, y, kSize, rgb(palette::aqua));
            rest.remove_prefix(cut);
            y += 32.0f;
        }
    }
    if (!note_.empty())
        ui::paragraph(list, canvas.fonts.regular, note_, x, y + 26.0f, 22.0f, width, 31.0f,
                      rgb(palette::mist), 5);
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pshare::app
