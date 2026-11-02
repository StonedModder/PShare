// PShare - Settings: what gets sent and how, and about.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"
#include <initializer_list>
#include <utility>

namespace pshare::app
{
namespace
{
enum RowId
{
    row_auto = 1,
    row_screenshots,
    row_clips,
    row_existing,
    row_notify,
    row_retries,
    row_poll,
    row_settle,
    row_version,
    row_address,
    row_config,
    row_access,
    row_logs,
    row_convert_webm,
    row_controls = 100, // value rows from here on: the controls list
};

// Settings are written a moment after the last change, not on every step of
// a stepper: small writes to /data are slow.
constexpr float kSaveDelay = 0.8f;

class SettingsPage final : public Page
{
  public:
    SettingsPage()
    {
        form_.style.theme = theme();
        form_.style.panel = true;
        form_.style.on_page = false;
        form_.style.row_height = 66.0f;
        form_.style.label_size = 24.0f;
        form_.style.value_size = 22.0f;
        form_.style.highlight.kind = ui::HighlightKind::fill;
        form_.style.description_inline = true;
    }

    const char *title() const override
    {
        return "Settings";
    }

    void layout(const gfx::Rect &area) override
    {
        area_ = area;
        form_.set_bounds({area.x, area.y, 1040.0f, area.h});
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
            out[n++] = {ui::Button::cross, "Change"};
            out[n++] = {ui::Button::circle, "Back"};
        }
        return n;
    }

    bool handle(const InputFrame &input, ui::Feedback &feedback, Context &context) override
    {
        if (input.is_pressed(Action::back))
            return true;
        if (input.nav == Direction::left && !form_.uses_horizontal() && !input.nav_repeat)
            return true;
        if (form_.handle(input, feedback) == ui::Event::changed)
        {
            Config &config = context.model.edit_config();
            switch (form_.changed_id())
            {
            case row_auto:
                config.auto_upload = form_.toggle_value(row_auto);
                break;
            case row_screenshots:
                config.upload_screenshots = form_.toggle_value(row_screenshots);
                break;
            case row_clips:
                config.upload_videos = form_.toggle_value(row_clips);
                break;
            case row_convert_webm:
                config.convert_webm = form_.toggle_value(row_convert_webm);
                break;
            case row_existing:
                config.upload_existing = form_.toggle_value(row_existing);
                break;
            case row_notify:
                config.notify = form_.toggle_value(row_notify);
                break;
            case row_retries:
                config.retries = form_.stepper_value(row_retries);
                break;
            case row_poll:
                config.poll_seconds = form_.stepper_value(row_poll);
                break;
            case row_settle:
                config.settle_seconds = form_.stepper_value(row_settle);
                break;
            case row_logs:
                config.keep_logs = form_.toggle_value(row_logs);
                break;
            default:
                return false;
            }
            save_in_ = kSaveDelay;
        }
        return false;
    }

    void update(float dt, ui::Feedback &feedback, Context &context) override
    {
        (void)feedback;
        if (save_in_ > 0.0f)
        {
            save_in_ -= dt;
            if (save_in_ <= 0.0f)
                context.model.save_config();
        }
        // Another page (Auto upload) may have turned this one.
        if (form_.toggle_value(row_auto) != context.model.config().auto_upload)
            form_.set_toggle(row_auto, context.model.config().auto_upload);
        form_.update(dt);
    }

    void draw(ui::Canvas &scene, Context &context) const override
    {
        (void)context;
        form_.draw(scene);
        // A note on the right: where the file is, for editing by hand.
        const float x = area_.x + 1080.0f;
        const float width = area_.x + area_.w - x;
        ui::Painter paint(scene.list, scene.fonts, theme(), scene.glass);
        paint.panel({x, area_.y, width, 360.0f});
        draw_icon(scene.list, Icon::settings, {x + 30.0f, area_.y + 30.0f, 44.0f, 44.0f},
                  rgb(palette::sun));
        paint.heading("By hand", x + 92.0f, area_.y + 66.0f, 30.0f, rgb(palette::cream));
        ui::paragraph(scene.list, scene.fonts.regular,
                      "Everything here lives in /data/pshare/config.ini. Per-game destinations "
                      "go in [title_destinations]: PPSA01234 = telegram.",
                      x + 30.0f, area_.y + 122.0f, 20.0f, width - 60.0f, 29.0f, rgb(palette::mist),
                      8);
    }

  private:
    void rebuild(Context &context)
    {
        const Config &config = context.model.config();
        form_.clear();
        form_.add_header("What gets sent");
        form_.add_toggle(row_auto, "Send new captures automatically", config.auto_upload)
            .description = "While the background uploader runs (Auto upload)";
        form_.add_toggle(row_screenshots, "Screenshots", config.upload_screenshots);
        form_.add_toggle(row_clips, "Video clips", config.upload_videos);
        form_.add_toggle(row_existing, "Also send older captures", config.upload_existing)
            .description = "Captures from before the uploader first ran. Careful: that can be "
                           "a lot";
        form_.add_toggle(row_notify, "Console notifications", config.notify).description =
            "A notification for every upload the background uploader makes";
        form_.add_header("How");
        form_.add_toggle(row_convert_webm, "Convert WebM to MP4", config.convert_webm).description =
            "All exports, up to 1080p/30 with sound. Uses CPU; original stays unchanged";
        form_.add_stepper(row_retries, "Tries per upload", config.retries, 1, 10);
        form_.add_stepper(row_poll, "Look for new captures every", config.poll_seconds, 1, 60)
            .stepper.set_suffix(" s");
        form_.add_stepper(row_settle, "Wait for a clip to finish", config.settle_seconds, 1, 30)
            .stepper.set_suffix(" s");
        form_.add_toggle(row_logs, "Keep a detailed log", config.keep_logs).description =
            "/data/pshare/pshared.log";
        // Controls, so nobody has to guess. The clip player uses VLC for PS5's
        // buttons where the two apps overlap.
        int row = row_controls;
        const auto controls = [&](const char *header,
                                  std::initializer_list<std::pair<const char *, const char *>> rows)
        {
            form_.add_header(header);
            // Action rows (that do nothing) so the D-pad can scroll through them.
            for (const auto &[action, button] : rows)
                form_.add_action(row++, action).text = button;
        };
        controls("Controls: clip player (as in VLC for PS5)",
                 {{"Play / pause", "Cross"},
                  {"Back / forward 10 s", "Left / Right"},
                  {"Back / forward 30 s", "L2 / R2"},
                  {"Previous / next clip", "L1 / R1"},
                  {"Send", "Square"},
                  {"Send to...", "Triangle"},
                  {"Close", "Circle"},
                  {"Show the controls", "Any button"}});
        controls("Controls: captures", {{"Details: play, send, browser", "Cross"},
                                        {"Send", "Square"},
                                        {"Send to...", "Triangle"},
                                        {"All / screenshots / clips", "L2 / R2"},
                                        {"Game and order (newest / oldest first)", "Touchpad"},
                                        {"Sections", "L1 / R1"},
                                        {"Menu", "Options"}});
        controls("Controls: browser page (phone or PC)",
                 {{"A controller on the PC", "Same as here"},
                  {"Play / pause", "Space"},
                  {"Back / forward 10 s", "Left / Right"},
                  {"Back / forward 30 s", "[  ]"},
                  {"Send, send to...", "S, T"},
                  {"Newer / older capture", ", ."},
                  {"Back to the captures", "Esc"}});
        form_.add_header("About");
        form_.add_value(row_version, "PShare", std::string("Version ") + kVersion);
        form_.add_value(row_address, "Console address",
                        context.model.local_ip().empty() ? "Not connected"
                                                         : context.model.local_ip());
        form_.add_value(row_access, "File access",
                        context.model.environment().elevated ? "Granted" : "Refused");
        form_.add_value(row_config, "Settings file", context.model.environment().paths.config());
    }

    gfx::Rect area_;
    ui::Form form_;
    float save_in_ = 0.0f;
};
} // namespace

std::unique_ptr<Page> make_settings_page()
{
    return std::make_unique<SettingsPage>();
}

} // namespace pshare::app
