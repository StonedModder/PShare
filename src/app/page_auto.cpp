// PShare - Auto upload: the background uploader, and the phone setup page.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include "pshare/core/text.hpp"

#include <cmath>

namespace pshare::app
{
namespace
{
enum RowId
{
    row_power = 1,
    row_pause = 2,
    row_auto = 3,
    row_with_app = 4,
    row_phone = 5,
    row_version = 6,
    row_through = 7,
    row_gallery = 8,
};

constexpr const char *kStartDaemonFlag = "start_uploader";
constexpr const char *kBackgroundSendsFlag = "background_sends";

class AutoPage final : public Page
{
  public:
    AutoPage()
    {
        form_.style.theme = theme();
        form_.style.panel = true;
        form_.style.on_page = false;
        form_.style.row_height = 70.0f;
        form_.style.label_size = 25.0f;
        form_.style.highlight.kind = ui::HighlightKind::fill;
        form_.style.description_inline = true;
        form_.style.label_ratio = 0.62f;
        for (ui::StatTile *tile : {&sent_, &failed_, &waiting_})
        {
            tile->style.theme = theme();
            tile->style.sparkline = false;
            tile->style.show_delta = false;
            tile->style.value_size = 52.0f;
        }
        sent_.label = "Sent";
        failed_.label = "Failed";
        waiting_.label = "Waiting";
        failed_.style.up_is_good = false;
    }

    const char *title() const override
    {
        return "Auto upload";
    }

    void layout(const gfx::Rect &area) override
    {
        area_ = area;
        hero_ = {area.x, area.y, 700.0f, 330.0f};
        const float tile_w = (hero_.w - 2 * 20.0f) / 3.0f;
        for (int i = 0; i < 3; ++i)
        {
            ui::StatTile *tile = i == 0 ? &sent_ : (i == 1 ? &failed_ : &waiting_);
            tile->set_bounds(
                {hero_.x + i * (tile_w + 20.0f), hero_.y + hero_.h + 24.0f, tile_w, 150.0f});
        }
        form_.set_bounds({area.x + hero_.w + 40.0f, area.y, area.w - hero_.w - 40.0f, area.h});
    }

    void enter(Context &context) override
    {
        rebuild(context);
        form_.enter();
    }

    void set_focused(bool focused) override
    {
        form_.set_active(focused);
    }

    int hints(ui::Hint *out, int capacity) const override
    {
        int n = 0;
        if (capacity >= 2)
        {
            out[n++] = {ui::Button::cross, "Select"};
            out[n++] = {ui::Button::circle, "Back"};
        }
        return n;
    }

    bool handle(const InputFrame &input, ui::Feedback &feedback, Context &context) override
    {
        if (input.is_pressed(Action::back))
            return true;
        if (input.nav == Direction::left && !form_.uses_horizontal())
            return true;
        const ui::Event event = form_.handle(input, feedback);
        if (event == ui::Event::activated || event == ui::Event::changed)
        {
            Model &model = context.model;
            switch (form_.changed_id())
            {
            case row_power:
                if (model.daemon().reachable)
                    model.stop_daemon();
                else
                    model.start_daemon();
                break;
            case row_pause:
                model.pause_daemon(model.daemon().state != "paused");
                break;
            case row_auto:
                model.edit_config().auto_upload = form_.toggle_value(row_auto);
                model.save_config();
                break;
            case row_with_app:
                model.set_app_flag(kStartDaemonFlag, form_.toggle_value(row_with_app));
                model.save_config();
                break;
            case row_through:
                model.set_app_flag(kBackgroundSendsFlag, form_.toggle_value(row_through));
                model.save_config();
                break;
            case row_gallery:
                open_gallery(context, feedback);
                break;
            case row_phone:
                open_phone(context, feedback);
                break;
            default:
                break;
            }
        }
        return false;
    }

    void update(float dt, ui::Feedback &feedback, Context &context) override
    {
        (void)feedback;
        const DaemonStatus &daemon = context.model.daemon();
        // The rows follow the uploader's state.
        if (daemon.reachable != was_running_ || daemon.state != was_state_ ||
            context.model.daemon_starting() != was_starting_)
        {
            was_running_ = daemon.reachable;
            was_state_ = daemon.state;
            was_starting_ = context.model.daemon_starting();
            refresh_rows(context);
        }
        if (form_.toggle_value(row_auto) != context.model.config().auto_upload)
            form_.set_toggle(row_auto, context.model.config().auto_upload);
        // The page can come up a moment after the panel opened (pshared starting).
        if (phone_open_ && context.link_panel.is_open() && daemon.setup_port > 0 && !shown_ready_)
        {
            shown_ready_ = true;
            context.link_panel.set_link(gallery_ ? gallery_url(context) : phone_url(context));
        }
        if (!context.link_panel.is_open())
            phone_open_ = false;
        sent_.set_value(daemon.uploaded);
        failed_.set_value(daemon.failed);
        waiting_.set_value(daemon.queued);
        sent_.update(dt);
        failed_.update(dt);
        waiting_.update(dt);
        form_.update(dt);
        ring_.target = daemon.reachable ? (daemon.state == "paused" ? 0.5f : 1.0f) : 0.0f;
        ring_.update(dt, 6.0f);
    }

    void draw(ui::Canvas &scene, Context &context) const override
    {
        gfx::DrawList &list = scene.list;
        const DaemonStatus &daemon = context.model.daemon();
        ui::Painter paint(list, scene.fonts, theme(), scene.glass);
        paint.panel(hero_);

        // The state, as a ring with planes going round it while it runs.
        const float cx = hero_.x + 150.0f;
        const float cy = hero_.y + 160.0f;
        const bool running = daemon.reachable;
        const bool paused = running && daemon.state == "paused";
        const gfx::Color tone =
            running ? (paused ? rgb(palette::sun) : rgb(palette::aqua)) : rgb(palette::mist, 0.6f);
        list.glow({cx - 80.0f, cy - 80.0f, 160.0f, 160.0f}, 80.0f, 40.0f,
                  tone.with_alpha(0.18f + 0.12f * ui::breathe(scene.time)));
        list.ring(cx, cy, 96.0f, 3.0f, rgb(0xffffff, 0.12f));
        list.arc(cx, cy, 99.0f, 9.0f, scene.time * (paused ? 0.0f : 1.4f), 6.2832f * ring_.value,
                 tone);
        const float orbit = scene.time * (running && !paused ? 1.4f : 0.0f) + 6.2832f * ring_.value;
        if (running)
            draw_plane(list, cx + std::sin(orbit) * 96.0f, cy - std::cos(orbit) * 96.0f, 16.0f,
                       orbit, rgb(palette::cream), rgb(palette::sun));
        draw_icon(list, running ? (paused ? Icon::clock : Icon::auto_upload) : Icon::lock,
                  {cx - 44.0f, cy - 44.0f, 88.0f, 88.0f}, tone);

        const float x = hero_.x + 290.0f;
        std::string state = "Off";
        if (context.model.daemon_starting())
            state = "Starting...";
        else if (paused)
            state = "Paused";
        else if (running)
            state = daemon.state == "uploading" ? "Sending" : "Watching";
        paint.heading(state, x, hero_.y + 110.0f, 54.0f, rgb(palette::cream));
        std::string line;
        if (running && daemon.started > 0)
            line = "For " + friendly_duration(context.now - daemon.started) + ", version " +
                   daemon.version;
        else if (!running)
            line = "New captures are not sent by themselves";
        ui::text(list, scene.fonts.regular, line, x, hero_.y + 152.0f, 22.0f, rgb(palette::mist));
        const char *explain =
            running ? "It keeps going after you close PShare, until the console restarts, "
                      "and sends each new screenshot and clip as soon as it is saved."
                    : "Turn it on and every new screenshot and clip is sent as soon as it is "
                      "saved, even after you close PShare.";
        ui::paragraph(list, scene.fonts.regular, explain, x, hero_.y + 200.0f, 21.0f,
                      hero_.x + hero_.w - 36.0f - x, 30.0f, rgb(palette::cream, 0.86f), 4);

        sent_.draw(scene);
        failed_.draw(scene);
        waiting_.draw(scene);

        // The last thing it did.
        float y = hero_.y + hero_.h + 24.0f + 150.0f + 52.0f;
        if (!daemon.last_error.empty())
        {
            draw_icon(list, Icon::cross, {hero_.x, y - 22.0f, 26.0f, 26.0f}, rgb(0xff8a95));
            ui::text(list, scene.fonts.regular,
                     scene.fonts.regular.font->fit("Last problem: " + daemon.last_error, 21.0f,
                                                   hero_.w - 40.0f),
                     hero_.x + 38.0f, y, 21.0f, rgb(0xff8a95));
            y += 40.0f;
        }
        if (!daemon.last_link.empty())
        {
            draw_icon(list, Icon::link, {hero_.x, y - 22.0f, 26.0f, 26.0f}, rgb(palette::aqua));
            ui::text(list, scene.fonts.mono,
                     scene.fonts.mono.font->fit(daemon.last_link, 20.0f, hero_.w - 40.0f),
                     hero_.x + 38.0f, y, 20.0f, rgb(palette::aqua));
        }
        form_.draw(scene);
    }

  private:
    std::string phone_url(Context &context) const
    {
        const std::string ip =
            context.model.local_ip().empty() ? "<console address>" : context.model.local_ip();
        return "http://" + ip + ":" + std::to_string(kSetupPort) + "/";
    }

    void open_phone(Context &context, ui::Feedback &feedback)
    {
        context.model.open_phone_setup();
        phone_open_ = true;
        gallery_ = false;
        shown_ready_ = context.model.daemon().setup_port > 0;
        context.link_panel.open(
            "Set up from your phone", shown_ready_ ? phone_url(context) : std::string(),
            "Scan the code, or type the address on a phone or computer on the same network. "
            "Paste webhooks and tokens there instead of typing them with the controller. "
            "The page closes by itself after 30 minutes.",
            feedback);
    }

    void open_gallery(Context &context, ui::Feedback &feedback)
    {
        context.model.open_phone_setup();
        phone_open_ = true;
        gallery_ = true;
        shown_ready_ = context.model.daemon().setup_port > 0;
        context.link_panel.open(
            "Captures on your phone", shown_ready_ ? gallery_url(context) : std::string(),
            "Scan the code with a phone on the same network: every screenshot and clip, "
            "ready to watch or save. The page closes by itself after 30 minutes.",
            feedback);
    }

    std::string gallery_url(Context &context) const
    {
        return phone_url(context) + "gallery";
    }

    void rebuild(Context &context)
    {
        form_.clear();
        form_.add_header("Background uploader");
        form_.add_action(row_power, "Turn on").chevron = false;
        form_.add_action(row_pause, "Pause");
        form_.add_toggle(row_auto, "Send automatically", context.model.config().auto_upload)
            .description = "Off: pshared still notices new captures but leaves them for you";
        form_.add_toggle(row_with_app, "Turn on when PShare opens",
                         context.model.app_flag(kStartDaemonFlag, true));
        form_
            .add_toggle(row_through, "Send uploads through it",
                        context.model.app_flag(kBackgroundSendsFlag, true))
            .description = "What you send keeps going after PShare closes";
        form_.add_header("Phone and PC");
        ui::FormRow &phone = form_.add_action(row_phone, "Set up from your phone");
        phone.chevron = true;
        phone.description = "Shows a QR code: paste webhooks and tokens from your phone";
        ui::FormRow &gallery = form_.add_action(row_gallery, "Captures on your phone");
        gallery.chevron = true;
        gallery.description = "Browse, watch and download every capture from a phone or PC";
        form_.add_header("About");
        form_.add_value(row_version, "pshared",
                        std::string("port ") + std::to_string(kControlPort));
        refresh_rows(context);
    }

    void refresh_rows(Context &context)
    {
        const DaemonStatus &daemon = context.model.daemon();
        if (ui::FormRow *power = form_.row(row_power))
        {
            power->label = daemon.reachable ? "Turn off" : "Turn on";
            power->text = context.model.daemon_starting() ? "Starting..." : "";
            power->danger = daemon.reachable;
        }
        if (ui::FormRow *pause = form_.row(row_pause))
        {
            pause->label = daemon.state == "paused" ? "Resume" : "Pause";
            pause->disabled = !daemon.reachable;
        }
        if (ui::FormRow *version = form_.row(row_version))
            version->text = daemon.reachable
                                ? daemon.version + " on port " + std::to_string(kControlPort)
                                : "Not running";
    }

    gfx::Rect area_;
    gfx::Rect hero_;
    ui::Form form_;
    ui::StatTile sent_;
    ui::StatTile failed_;
    ui::StatTile waiting_;
    hui::tween::Spring ring_;
    bool was_running_ = false;
    bool was_starting_ = false;
    std::string was_state_;
    bool phone_open_ = false;
    bool gallery_ = false;
    bool shown_ready_ = false;
};
} // namespace

std::unique_ptr<Page> make_auto_page()
{
    return std::make_unique<AutoPage>();
}

} // namespace pshare::app
