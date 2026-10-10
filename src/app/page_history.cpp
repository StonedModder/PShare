// PShare - History: every upload, its link as a QR code, and retries.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include "pshare/core/text.hpp"

#include <algorithm>

namespace pshare::app
{
namespace
{
constexpr std::size_t kShown = 400;

class HistoryPage final : public Page
{
  public:
    HistoryPage()
    {
        list_.style.theme = theme();
        list_.style.panel = true;
        list_.style.row_height = 84.0f;
        list_.style.title_size = 25.0f;
        list_.style.subtitle_size = 19.0f;
        list_.style.value_size = 20.0f;
        list_.style.highlight.kind = ui::HighlightKind::fill;
        list_.style.leading_width = 64.0f;
        list_.leading =
            [this](ui::Canvas &canvas, const gfx::Rect &row, const ui::ListItem &item, int, float)
        {
            const bool ok = item.tag >= 0 && item.tag < static_cast<int>(ok_.size()) &&
                            ok_[static_cast<std::size_t>(item.tag)];
            const float cx = row.x + 32.0f;
            const float cy = row.cy();
            canvas.list.circle(cx, cy, 20.0f, ok ? rgb(palette::aqua, 0.18f) : rgb(0xff5d6c, 0.2f));
            draw_icon(canvas.list, ok ? Icon::check : Icon::cross,
                      {cx - 13.0f, cy - 13.0f, 26.0f, 26.0f},
                      ok ? rgb(palette::aqua) : rgb(0xff8a95));
        };

        dialog_.style.theme = theme();
        empty_.style.theme = theme();
        empty_.title = "Nothing sent yet";
        empty_.body = "Uploads from the app and from the background uploader are listed here, "
                      "with their links.";
        empty_.icon = [](ui::Canvas &canvas, const gfx::Rect &box)
        { draw_icon(canvas.list, Icon::history, box, rgb(palette::mist, 0.7f)); };
    }

    const char *title() const override
    {
        return "History";
    }

    void layout(const gfx::Rect &area) override
    {
        area_ = area;
        list_.set_bounds({area.x, area.y + 70.0f, area.w, area.h - 70.0f});
        empty_.set_bounds({area.x, area.y + 120.0f, area.w, area.h - 200.0f});
    }

    void enter(Context &context) override
    {
        rebuild(context);
        list_.enter();
        empty_.enter();
    }

    void set_focused(bool focused) override
    {
        list_.set_active(focused);
    }

    bool modal() const override
    {
        return dialog_.is_open();
    }
    bool wants_glass() const override
    {
        return dialog_.visible();
    }

    int hints(ui::Hint *out, int capacity) const override
    {
        int n = 0;
        if (capacity < 3)
            return 0;
        if (dialog_.is_open())
        {
            out[n++] = {ui::Button::cross, "Select"};
            out[n++] = {ui::Button::circle, "Close"};
            return n;
        }
        if (!rows_.empty())
            out[n++] = {ui::Button::cross, "Open"};
        out[n++] = {ui::Button::circle, "Back"};
        return n;
    }

    void on_event(const ModelEvent &event, ui::Feedback &, Context &context) override
    {
        if (event.kind == EventKind::upload_finished || event.kind == EventKind::scanned)
            rebuild(context);
    }

    bool handle(const InputFrame &input, ui::Feedback &feedback, Context &context) override
    {
        if (dialog_.is_open())
        {
            if (dialog_.handle(input, feedback) == ui::Event::activated && dialog_.choice() == 1)
            {
                std::string why;
                if (context.model.upload(retry_path_, retry_destination_, &why))
                    context.toasts.push(ui::StatusKind::info, "Trying again", {}, 2.0f);
                else
                    context.toasts.push(ui::StatusKind::warning, why);
            }
            return false;
        }
        if (input.is_pressed(Action::back) || (input.nav == Direction::left && !input.nav_repeat))
            return true;
        if (rows_.empty())
            return false;
        if (list_.handle(input, feedback) == ui::Event::activated)
            open(context, feedback, list_.focus());
        return false;
    }

    void update(float dt, ui::Feedback &feedback, Context &context) override
    {
        (void)feedback;
        // The daemon adds lines while the app is open: follow the file's size.
        if (context.model.history().size() != seen_)
            rebuild(context);
        list_.update(dt);
        dialog_.update(dt);
        empty_.update(dt);
    }

    void draw(ui::Canvas &scene, Context &context) const override
    {
        (void)context;
        gfx::DrawList &list = scene.list;
        ui::Painter paint(list, scene.fonts, theme(), scene.glass);
        paint.heading("History", area_.x, area_.y + 42.0f, 38.0f, rgb(palette::cream));
        const std::string summary = std::to_string(sent_) + " sent · " + std::to_string(failed_) +
                                    " failed · " + std::to_string(automatic_) +
                                    " by the background uploader";
        ui::text(list, scene.fonts.semibold, summary, area_.x + area_.w, area_.y + 40.0f, 22.0f,
                 rgb(palette::mist), gfx::Align::right);
        if (rows_.empty())
            empty_.draw(scene);
        else
            list_.draw(scene);
    }

    void draw_overlay(ui::Canvas &overlay, Context &context) const override
    {
        (void)context;
        dialog_.draw(overlay);
    }

  private:
    struct Row
    {
        HistoryEntry entry;
    };

    void rebuild(Context &context)
    {
        const std::vector<HistoryEntry> &entries = context.model.history();
        seen_ = entries.size();
        rows_.clear();
        ok_.clear();
        sent_ = failed_ = automatic_ = 0;
        for (const HistoryEntry &entry : entries)
        {
            (entry.ok ? sent_ : failed_)++;
            automatic_ += entry.automatic ? 1 : 0;
        }
        // Newest first, whoever wrote them (the app and pshared both append).
        std::vector<const HistoryEntry *> order;
        order.reserve(entries.size());
        for (const HistoryEntry &entry : entries)
            order.push_back(&entry);
        std::stable_sort(order.begin(), order.end(),
                         [](const HistoryEntry *a, const HistoryEntry *b)
                         { return a->time > b->time; });
        std::vector<ui::ListItem> items;
        for (const HistoryEntry *pointer : order)
        {
            if (rows_.size() >= kShown)
                break;
            const HistoryEntry &entry = *pointer;
            const std::size_t slash = entry.path.rfind('/');
            const std::string file =
                slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
            MediaKind kind = MediaKind::screenshot;
            capture_kind(file, &kind);
            const Destination *d = context.model.config().find(entry.destination);
            ui::ListItem item;
            item.title = std::string(kind == MediaKind::video ? "Clip" : "Screenshot") + " · " +
                         context.model.game_name(title_id_from_path(entry.path));
            item.subtitle = (d ? d->display_name() : entry.destination) + " · " +
                            friendly_time(entry.time, context.now) + " · " +
                            (entry.automatic ? "automatic" : "by hand");
            if (!entry.ok)
                item.subtitle += " · " + entry.detail;
            item.value =
                entry.ok ? (starts_with(entry.detail, "http") ? "Link" : "Sent") : "Failed";
            item.chevron = entry.ok ? starts_with(entry.detail, "http") : true;
            item.tag = static_cast<int>(rows_.size());
            ok_.push_back(entry.ok);
            items.push_back(std::move(item));
            rows_.push_back({entry});
        }
        const int focus = list_.focus();
        list_.set_items(std::move(items));
        list_.set_focus(std::clamp(focus, 0, std::max(0, static_cast<int>(rows_.size()) - 1)),
                        true);
    }

    void open(Context &context, ui::Feedback &feedback, int index)
    {
        if (index < 0 || index >= static_cast<int>(rows_.size()))
            return;
        const HistoryEntry &entry = rows_[static_cast<std::size_t>(index)].entry;
        const std::string game = context.model.game_name(title_id_from_path(entry.path));
        if (entry.ok && starts_with(entry.detail, "http"))
        {
            context.link_panel.open(game, entry.detail,
                                    "Scan it with your phone's camera to open the upload.",
                                    feedback);
            return;
        }
        if (entry.ok)
        {
            ui::refuse(feedback, list_.style, InputFrame{}, refusal_);
            return;
        }
        retry_path_ = entry.path;
        retry_destination_ = entry.destination;
        ui::DialogContent content;
        content.icon = ui::StatusKind::danger;
        content.title = "This upload failed";
        content.body = entry.detail.empty() ? "No reason was given." : entry.detail;
        content.buttons = {{"Close", ui::ButtonKind::secondary},
                           {"Try again", ui::ButtonKind::primary}};
        content.default_button = 1;
        dialog_.open(std::move(content), feedback);
    }

    gfx::Rect area_;
    ui::ListView list_;
    ui::Dialog dialog_;
    ui::EmptyState empty_;
    ui::Pulse refusal_;
    std::vector<Row> rows_;
    std::vector<bool> ok_;
    std::size_t seen_ = static_cast<std::size_t>(-1);
    int sent_ = 0;
    int failed_ = 0;
    int automatic_ = 0;
    std::string retry_path_;
    std::string retry_destination_;
};
} // namespace

std::unique_ptr<Page> make_history_page()
{
    return std::make_unique<HistoryPage>();
}

} // namespace pshare::app
