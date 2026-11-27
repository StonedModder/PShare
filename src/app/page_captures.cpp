// PShare - Captures: every screenshot and clip, and sending them by hand.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"
#include "app/clip_player.hpp"

#include "pshare/core/text.hpp"

#include <algorithm>
#include <set>

namespace pshare::app
{
namespace
{
enum ActionTag
{
    action_send_default = 1,
    action_send_to = 2,
    action_view = 3,
    action_link = 4,
    action_play = 5,
    action_browser = 6,
};

// The Options menu's items on this page (Page::kPageMenuTag up).
enum MenuTag
{
    menu_select = Page::kPageMenuTag,
    menu_send_all,
    menu_delete_all,
    menu_send_selected,
    menu_delete_selected,
    menu_select_all,
    menu_select_none,
    menu_stop_selecting,
};

// The "Send ..." sheet's rows.
enum BulkRow
{
    bulk_to = 1,
    bulk_skip_sent,
    bulk_delete,
    bulk_go,
};

// What the confirmation dialog is asking about.
enum class Confirm : std::uint8_t
{
    none,
    delete_now,       // delete these captures
    send_then_delete, // start a Send all that deletes each one once sent
};

enum class Zone : std::uint8_t
{
    tabs,
    game,  // the "which game" button, right of the tabs
    order, // the "newest / oldest first" button
    grid,
};

// [app] oldest_first: the sort order is remembered; the game filter is not.
constexpr const char *kOldestFirstFlag = "oldest_first";
constexpr float kButtonsX = 540.0f; // where the two buttons start, from the page's left
// Picker rows that are not a title ID.
constexpr const char *kHeader = "\x01header";
constexpr const char *kNewest = "\x01newest";
constexpr const char *kOldest = "\x01oldest";

class CapturesPage final : public Page
{
  public:
    CapturesPage()
    {
        tabs_.style.theme = theme();
        tabs_.style.kind = ui::TabKind::pill;
        tabs_.style.height = 54.0f;
        tabs_.style.text_size = 23.0f;
        tabs_.set_tabs({{"All"}, {"Screenshots"}, {"Clips"}});

        grid_.style.theme = theme();
        grid_.style.columns = 4;
        grid_.style.gap_x = 26.0f;
        grid_.style.gap_y = 24.0f;
        grid_.style.card.art_aspect = 16.0f / 9.0f;
        grid_.style.card.title_size = 22.0f;
        grid_.style.card.subtitle_size = 18.0f;
        grid_.style.card.text_gap = 12.0f;
        grid_.style.card.badge_corner = ui::CardCorner::top_left;
        grid_.style.card.badge_size = 15.0f;
        grid_.style.card.glow = true;
        grid_.style.card.focus_scale = 1.05f;
        grid_.style.exits.up = true;
        grid_.style.exits.left = true;
        grid_.art = [this](ui::Canvas &canvas, const gfx::Rect &art, float radius,
                           const ui::CardItem &item, float focus)
        { draw_art(canvas, art, radius, item, focus); };

        sheet_.style.theme = theme();
        sheet_.style.size = 660.0f;
        sheet_.style.footer = 96.0f;
        sheet_.content = [this](ui::Canvas &canvas, const gfx::Rect &area, float)
        { draw_sheet(canvas, area); };

        actions_.style.theme = theme();
        actions_.style.row_height = 62.0f;
        actions_.style.title_size = 24.0f;
        actions_.style.subtitle_size = 18.0f;
        actions_.style.highlight.kind = ui::HighlightKind::fill;
        actions_.style.entrance_step = 0.0f;

        destinations_.style.theme = theme();
        destinations_.style.width = 440.0f;

        viewer_.style.theme = theme();
        viewer_.style.filmstrip = false;
        viewer_.style.counter = false;

        empty_.style.theme = theme();
        empty_.style.icon_size = 220.0f;
        empty_.style.max_text_width = 680.0f;

        picker_.style.theme = theme();
        picker_.style.size = 660.0f;
        picker_.style.footer = 96.0f;
        picker_.content = [this](ui::Canvas &canvas, const gfx::Rect &, float)
        { picker_list_.draw(canvas); };
        picker_list_.style.theme = theme();
        picker_list_.style.row_height = 76.0f;
        picker_list_.style.title_size = 24.0f;
        picker_list_.style.subtitle_size = 18.0f;
        picker_list_.style.value_size = 20.0f;
        picker_list_.style.highlight.kind = ui::HighlightKind::fill;
        unreadable_.style.theme = theme();
        confirm_.style.theme = theme();

        bulk_.style.theme = theme();
        bulk_.style.size = 660.0f;
        bulk_.style.footer = 96.0f;
        bulk_.content = [this](ui::Canvas &canvas, const gfx::Rect &, float)
        { bulk_form_.draw(canvas); };
        bulk_form_.style.theme = theme();
        bulk_form_.style.row_height = 68.0f;
    }

    const char *title() const override
    {
        return "Captures";
    }

    void layout(const gfx::Rect &area) override
    {
        area_ = area;
        tabs_.set_bounds({area.x, area.y, 720.0f, 60.0f});
        grid_.set_bounds({area.x - 14.0f, area.y + 86.0f, area.w + 28.0f, area.h - 86.0f});
        empty_.set_bounds({area.x, area.y + 90.0f, area.w, area.h - 120.0f});
        viewer_.set_bounds({96.0f, 70.0f, 1728.0f, 880.0f});
    }

    void enter(Context &context) override
    {
        oldest_first_ = context.model.app_flag(kOldestFirstFlag, false);
        rebuild(context, true);
        grid_.enter();
        empty_.enter();
    }

    void set_focused(bool focused) override
    {
        focused_ = focused;
        grid_.set_active(focused && zone_ == Zone::grid);
        tabs_.set_focused(focused && zone_ == Zone::tabs);
    }

    bool modal() const override
    {
        return sheet_.is_open() || destinations_.is_open() || viewer_open_ || clip_.is_open() ||
               picker_.is_open() || unreadable_.is_open() || bulk_.is_open() || confirm_.is_open();
    }
    bool wants_glass() const override
    {
        return sheet_.visible() || destinations_.visible() || viewer_fade_.value > 0.01f ||
               clip_.visible() || picker_.visible() || unreadable_.visible() || bulk_.visible() ||
               confirm_.visible();
    }

    int hints(ui::Hint *out, int capacity) const override
    {
        int n = 0;
        const auto add = [&](ui::Button button, const char *label)
        {
            if (n < capacity)
                out[n++] = {button, label};
        };
        if (unreadable_.is_open() || confirm_.is_open())
        {
            add(ui::Button::cross, "Select");
            add(ui::Button::circle, unreadable_.is_open() ? "Keep" : "Cancel");
            return n;
        }
        if (bulk_.is_open())
        {
            add(ui::Button::cross, "Choose");
            add(ui::Button::circle, "Close");
            return n;
        }
        if (clip_.is_open() && !destinations_.is_open())
        {
            return 0; // the player draws its own
        }
        if (viewer_open_)
        {
            add(ui::Button::circle, "Back");
            add(ui::Button::square, "Send");
            add(ui::Button::l2, "Zoom");
        }
        else if (destinations_.is_open())
        {
            add(ui::Button::cross, "Send here");
            add(ui::Button::circle, "Cancel");
        }
        else if (sheet_.is_open() || picker_.is_open())
        {
            add(ui::Button::cross, "Choose");
            add(ui::Button::circle, "Close");
        }
        else if (zone_ == Zone::game || zone_ == Zone::order)
        {
            add(ui::Button::cross, zone_ == Zone::game ? "Pick a game" : "Flip the order");
            add(ui::Button::circle, "Back");
        }
        else if (!view_.empty() && selecting_)
        {
            select_hint_ = "Select (" + std::to_string(chosen_.size()) + ")";
            add(ui::Button::cross, select_hint_.c_str());
            add(ui::Button::square, "Send selected");
            add(ui::Button::triangle,
                chosen_.size() == paths_.size() ? "Select none" : "Select all");
            add(ui::Button::circle, "Done");
            add(ui::Button::options, "Delete...");
        }
        else if (!view_.empty())
        {
            add(ui::Button::cross, "Details");
            add(ui::Button::square, "Send");
            add(ui::Button::triangle, "Send to...");
            if (n < capacity)
                out[n++] = {ui::Button::l2, "Type", ui::Button::r2};
            add(ui::Button::touchpad, "Game & order");
        }
        return n;
    }

    void on_event(const ModelEvent &event, ui::Feedback &, Context &context) override
    {
        if (event.kind == EventKind::scanned || event.kind == EventKind::upload_finished)
            rebuild(context, false);
        if (event.kind == EventKind::unreadable)
            unreadable_pending_ = true; // asked when the page is in use (update)
    }

    void menu_items(std::vector<ui::MenuItem> &items, Context &context) const override
    {
        (void)context;
        if (view_.empty())
            return;
        const auto add =
            [&](int tag, std::string label, std::string shortcut = {}, bool disabled = false)
        {
            ui::MenuItem item;
            item.tag = tag;
            item.label = std::move(label);
            item.shortcut = std::move(shortcut);
            item.disabled = disabled;
            items.push_back(std::move(item));
        };
        const std::string shown = std::to_string(paths_.size());
        if (!selecting_)
        {
            add(menu_select, "Select captures");
            add(menu_send_all, "Send all shown...", shown);
            add(menu_delete_all, "Delete all shown...", shown);
            return;
        }
        const std::string count = std::to_string(chosen_.size());
        add(menu_send_selected, "Send selected...", count, chosen_.empty());
        add(menu_delete_selected, "Delete selected...", count, chosen_.empty());
        if (chosen_.size() == paths_.size())
            add(menu_select_none, "Select none");
        else
            add(menu_select_all, "Select all shown", shown);
        add(menu_stop_selecting, "Stop selecting");
    }

    void menu_action(int tag, ui::Feedback &feedback, Context &context) override
    {
        switch (tag)
        {
        case menu_select:
            selecting_ = true;
            chosen_.clear();
            set_zone(Zone::grid);
            feedback.play(hui::audio::Cue::toggle);
            break;
        case menu_send_all:
            open_bulk(context, paths_, feedback);
            break;
        case menu_delete_all:
            ask_delete(context, paths_, feedback);
            break;
        case menu_send_selected:
            open_bulk(context, selection(), feedback);
            break;
        case menu_delete_selected:
            ask_delete(context, selection(), feedback);
            break;
        case menu_select_all:
            chosen_.insert(paths_.begin(), paths_.end());
            break;
        case menu_select_none:
            chosen_.clear();
            break;
        case menu_stop_selecting:
            stop_selecting();
            break;
        default:
            break;
        }
    }

    bool handle(const InputFrame &input, ui::Feedback &feedback, Context &context) override
    {
        if (confirm_.is_open())
        {
            if (confirm_.handle(input, feedback) == ui::Event::activated && confirm_.choice() == 1)
            {
                if (confirming_ == Confirm::delete_now)
                {
                    context.model.delete_captures(confirm_paths_);
                    stop_selecting();
                }
                else if (confirming_ == Confirm::send_then_delete)
                {
                    start_bulk(context, feedback);
                }
            }
            if (!confirm_.is_open())
            {
                confirming_ = Confirm::none;
                confirm_paths_.clear();
            }
            return false;
        }
        if (bulk_.is_open())
        {
            if (bulk_.handle(input, feedback) == ui::Event::none)
            {
                const ui::Event event = bulk_form_.handle(input, feedback);
                if (event == ui::Event::changed)
                    fill_bulk(context);
                else if (event == ui::Event::activated && bulk_form_.changed_id() == bulk_go)
                    confirm_bulk(context, feedback);
            }
            return false;
        }
        if (unreadable_.is_open())
        {
            if (unreadable_.handle(input, feedback) == ui::Event::activated &&
                unreadable_.choice() == 1)
                context.model.delete_captures(unreadable_paths_);
            if (!unreadable_.is_open())
            {
                context.model.mark_unreadable_asked(unreadable_paths_);
                unreadable_paths_.clear();
            }
            return false;
        }
        if (viewer_open_)
        {
            handle_viewer(input, feedback, context);
            return false;
        }
        if (clip_.is_open() && !destinations_.is_open())
        {
            const ClipPlayer::Request request = clip_.handle(input, feedback);
            switch (request)
            {
            case ClipPlayer::Request::send:
                send(context, feedback, {});
                break;
            case ClipPlayer::Request::send_to:
                open_destinations(context, feedback, {1240.0f, 940.0f, 440.0f, 20.0f});
                break;
            case ClipPlayer::Request::previous:
            case ClipPlayer::Request::next:
            {
                // The clip next to this one, newest first, as on the Clips tab.
                const std::vector<int> clips =
                    context.model.filtered(Filter::clips, game_, oldest_first_);
                const auto &all = context.model.captures();
                int at = -1;
                for (std::size_t i = 0; i < clips.size(); ++i)
                    if (all[static_cast<std::size_t>(clips[i])].item.path == clip_.path())
                        at = static_cast<int>(i);
                const int to = at + (request == ClipPlayer::Request::next ? 1 : -1);
                if (at < 0 || to < 0 || to >= static_cast<int>(clips.size()))
                    ui::refuse(feedback, grid_.style, InputFrame{}, refusal_);
                else
                    play_clip(context,
                              all[static_cast<std::size_t>(clips[static_cast<std::size_t>(to)])],
                              feedback);
                break;
            }
            case ClipPlayer::Request::closed:
            case ClipPlayer::Request::none:
                break;
            }
            return false;
        }
        if (destinations_.is_open())
        {
            if (destinations_.handle(input, feedback) == ui::Event::activated)
            {
                const int tag = destinations_.item(destinations_.focus()).tag;
                const Config &config = context.model.config();
                if (tag >= 0 && tag < static_cast<int>(config.destinations.size()))
                    send(context, feedback, config.destinations[static_cast<std::size_t>(tag)].id);
                sheet_.close(feedback);
            }
            return false;
        }
        if (picker_.is_open())
        {
            if (picker_.handle(input, feedback) == ui::Event::none &&
                picker_list_.handle(input, feedback) == ui::Event::activated)
            {
                const int index = picker_list_.focus();
                if (index >= 0 && index < static_cast<int>(picker_ids_.size()))
                {
                    const std::string &id = picker_ids_[static_cast<std::size_t>(index)];
                    if (id == kNewest || id == kOldest)
                    {
                        oldest_first_ = id == kOldest;
                        context.model.set_app_flag(kOldestFirstFlag, oldest_first_);
                        context.model.save_config();
                    }
                    else
                    {
                        game_ = id;
                    }
                }
                picker_.close(feedback);
                filter_changed(context);
                set_zone(view_.empty() ? Zone::game : Zone::grid);
            }
            return false;
        }
        if (sheet_.is_open())
        {
            const ui::Event event = sheet_.handle(input, feedback);
            if (event == ui::Event::none)
            {
                if (actions_.handle(input, feedback) == ui::Event::activated)
                    run_action(actions_.item(actions_.focus()).tag, feedback, context);
            }
            return false;
        }

        const int filters = 3;
        if (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next))
        {
            const int step = input.is_pressed(Action::jump_next) ? 1 : -1;
            tabs_.set_active((tabs_.active() + step + filters) % filters);
            ui::play_cue(feedback, tabs_.style, tabs_.style.sounds.page);
            filter_changed(context);
            return false;
        }
        if (input.is_pressed(Action::touch))
        {
            open_picker(context, feedback);
            return false;
        }
        if (zone_ == Zone::tabs)
        {
            const ui::Event event = tabs_.handle(input, feedback);
            if (event == ui::Event::changed)
                filter_changed(context);
            if (input.nav == Direction::down && !view_.empty())
                set_zone(Zone::grid);
            if (input.is_pressed(Action::back))
                return true;
            if (event == ui::Event::refused && input.nav == Direction::left)
                return true;
            if (event == ui::Event::refused && input.nav == Direction::right)
            {
                set_zone(Zone::game);
                ui::play_cue(feedback, tabs_.style, tabs_.style.sounds.move);
            }
            return false;
        }
        if (zone_ == Zone::game || zone_ == Zone::order)
        {
            if (input.is_pressed(Action::back))
                return true;
            if (input.nav == Direction::left && !input.nav_repeat)
            {
                set_zone(zone_ == Zone::order ? Zone::game : Zone::tabs);
                ui::play_cue(feedback, tabs_.style, tabs_.style.sounds.move);
            }
            else if (input.nav == Direction::right && !input.nav_repeat && zone_ == Zone::game)
            {
                set_zone(Zone::order);
                ui::play_cue(feedback, tabs_.style, tabs_.style.sounds.move);
            }
            else if (input.nav == Direction::down && !view_.empty())
            {
                set_zone(Zone::grid);
            }
            else if (input.is_pressed(Action::confirm))
            {
                if (zone_ == Zone::game)
                    open_picker(context, feedback);
                else
                {
                    oldest_first_ = !oldest_first_;
                    context.model.set_app_flag(kOldestFirstFlag, oldest_first_);
                    context.model.save_config();
                    feedback.play(hui::audio::Cue::toggle);
                    filter_changed(context);
                    set_zone(Zone::order);
                }
            }
            return false;
        }
        if (view_.empty())
        {
            if (input.nav == Direction::up)
                set_zone(Zone::tabs);
            return input.nav == Direction::left || input.is_pressed(Action::back);
        }
        if (selecting_)
        {
            if (input.is_pressed(Action::back))
            {
                stop_selecting();
                feedback.play(hui::audio::Cue::back);
                return false;
            }
            if (input.is_pressed(Action::west))
            {
                if (chosen_.empty())
                    ui::refuse(feedback, grid_.style, InputFrame{}, refusal_);
                else
                    open_bulk(context, selection(), feedback);
                return false;
            }
            if (input.is_pressed(Action::north))
            {
                if (chosen_.size() == paths_.size())
                    chosen_.clear();
                else
                    chosen_.insert(paths_.begin(), paths_.end());
                feedback.play(hui::audio::Cue::toggle);
                return false;
            }
        }
        if (input.is_pressed(Action::back))
            return true;
        if (input.is_pressed(Action::west))
        {
            send(context, feedback, {});
            return false;
        }
        if (input.is_pressed(Action::north))
        {
            open_destinations(context, feedback, grid_.cell_rect(grid_.focus()));
            return false;
        }
        const ui::Event event = grid_.handle(input, feedback);
        if (event == ui::Event::activated && selecting_)
        {
            const std::string &path = paths_[static_cast<std::size_t>(grid_.focus())];
            if (!chosen_.erase(path))
                chosen_.insert(path);
            feedback.play(hui::audio::Cue::toggle);
        }
        else if (event == ui::Event::activated)
            open_sheet(context, feedback);
        else if (event == ui::Event::none && grid_.exit() == Direction::up)
            set_zone(Zone::tabs);
        else if (event == ui::Event::none && grid_.exit() == Direction::left)
            return true;
        return false;
    }

    void update(float dt, ui::Feedback &feedback, Context &context) override
    {
        (void)feedback;
        // Tab badges follow the counts.
        tabs_.tab(0).badge = static_cast<int>(context.model.captures().size());
        tabs_.tab(1).badge = context.model.screenshots();
        tabs_.tab(2).badge = context.model.clips();
        tabs_.update(dt);
        grid_.update(dt);
        sheet_.update(dt);
        unreadable_.update(dt);
        confirm_.update(dt);
        bulk_.update(dt);
        bulk_form_.update(dt);
        if (unreadable_pending_ && focused_ && !modal())
        {
            unreadable_pending_ = false;
            ask_unreadable(context, context.model.unreadable_unasked(), feedback);
        }
        picker_.update(dt);
        picker_list_.update(dt);
        button_focus_[0] += ((zone_ == Zone::game && focused_ ? 1.0f : 0.0f) - button_focus_[0]) *
                            std::min(1.0f, dt * 14.0f);
        button_focus_[1] += ((zone_ == Zone::order && focused_ ? 1.0f : 0.0f) - button_focus_[1]) *
                            std::min(1.0f, dt * 14.0f);
        // The page can come up a moment after the panel opens (pshared starting).
        if (!browser_path_.empty())
        {
            if (!context.link_panel.is_open())
                browser_path_.clear();
            else if (context.model.daemon().setup_port > 0 && !browser_ready_)
            {
                browser_ready_ = true;
                context.link_panel.set_link(browser_url(context));
            }
        }
        actions_.update(dt);
        destinations_.update(dt);
        empty_.update(dt);
        viewer_.update(dt);
        clip_.update(dt);
        viewer_fade_.target = viewer_open_ ? 1.0f : 0.0f;
        viewer_fade_.update(dt, 14.0f);
        if (viewer_open_ && !viewer_path_.empty())
        {
            const Thumbs::Texture full = context.thumbs.full(viewer_path_);
            if (full.id != 0 && viewer_texture_ != full.id)
            {
                viewer_texture_ = full.id;
                ui::ViewerImage image = viewer_.images().front();
                image.texture = full.id;
                image.aspect = full.aspect;
                image.pixel_width = 1920.0f;
                viewer_.set_images({image});
            }
        }
    }

    void draw(ui::Canvas &scene, Context &context) const override
    {
        drawing_ = &context;
        gfx::DrawList &list = scene.list;
        tabs_.draw(scene);
        draw_buttons(scene, context);
        // What is shown, right of the buttons: the longest wording that fits the
        // room they leave (wide tab counts and a long game name leave little),
        // else nothing; while selecting, the hint bar has the count too.
        std::vector<std::string> wordings;
        const std::string shown = std::to_string(view_.size());
        if (context.model.scanning() && !context.model.scanned_once())
        {
            wordings = {"Looking for captures...", "Looking..."};
        }
        else if (selecting_)
        {
            const std::string chosen = std::to_string(chosen_.size());
            wordings = {chosen + " of " + shown + " selected", chosen + " selected", chosen};
        }
        else
        {
            int sent = 0;
            for (int index : view_)
                sent += context.model.captures()[static_cast<std::size_t>(index)].uploaded ? 1 : 0;
            const std::string captures = shown + (view_.size() == 1 ? " capture" : " captures");
            wordings = {captures + " \xC2\xB7 " + std::to_string(sent) + " sent", captures, shown};
        }
        const gfx::Rect order = button_rect(context, 1);
        const float room = area_.x + area_.w - (order.x + order.w + 24.0f);
        for (const std::string &wording : wordings)
            if (context.fonts.semibold.measure(wording, 22.0f) <= room)
            {
                ui::text(list, context.fonts.semibold, wording, area_.x + area_.w, area_.y + 38.0f,
                         22.0f, rgb(palette::mist), gfx::Align::right);
                break;
            }

        if (view_.empty())
        {
            ui::EmptyState empty = empty_;
            const Art &art = context.art;
            if (art.empty != 0)
                empty.icon = [&art](ui::Canvas &canvas, const gfx::Rect &box)
                {
                    const float h = box.h * 1.5f;
                    const float w = h * art.empty_aspect;
                    canvas.list.image(art.empty, {box.cx() - w * 0.5f, box.y + box.h - h, w, h},
                                      gfx::kFullUv, rgb(0xffffff));
                };
            if (!context.model.environment().elevated || context.model.scan_errno() != 0)
            {
                empty.title = "PShare can't see your captures";
                empty.body = "The app was not given access to the console's capture folder. "
                             "Turn on Auto upload: the background uploader runs outside the app "
                             "and sends new captures anyway.";
            }
            else if (!context.model.scanned_once())
            {
                empty.title = "Looking for captures";
                empty.body = "One moment...";
            }
            else
            {
                empty.title = tabs_.active() == 2 ? "No clips yet" : "No captures yet";
                empty.body = "Press the Create button in a game to take a screenshot or save "
                             "a clip. It shows up here, and Auto upload sends it right away.";
            }
            empty.draw(scene);
        }
        else
        {
            grid_.draw(scene);
        }
        drawing_ = nullptr;
    }

    void draw_overlay(ui::Canvas &overlay, Context &context) const override
    {
        drawing_ = &context;
        sheet_.draw(overlay);
        picker_.draw(overlay);
        clip_.draw(overlay);
        destinations_.draw(overlay);
        unreadable_.draw(overlay);
        bulk_.draw(overlay);
        confirm_.draw(overlay);
        if (viewer_fade_.value > 0.01f)
        {
            gfx::DrawList &list = overlay.list;
            list.push_opacity(viewer_fade_.value);
            list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                              rgb(palette::ink, 0.94f));
            viewer_.draw(overlay);
            list.pop_opacity();
        }
        drawing_ = nullptr;
    }

  private:
    // Captures that read back as zeros: say so, and offer to delete them.
    void ask_unreadable(Context &context, std::vector<std::string> paths, ui::Feedback &feedback)
    {
        if (paths.empty())
            return;
        std::vector<std::string> games;
        int clips = 0;
        for (const std::string &path : paths)
            if (const Capture *capture = context.model.find_capture(path))
            {
                clips += capture->item.video() ? 1 : 0;
                if (std::find(games.begin(), games.end(), capture->game) == games.end())
                    games.push_back(capture->game);
            }
        const int count = static_cast<int>(paths.size());
        const std::string what = clips == count ? (count == 1 ? "clip" : "clips")
                                                : (count == 1 ? "capture" : "captures");
        std::string from;
        for (std::size_t i = 0; i < games.size() && i < 3; ++i)
            from += (i == 0 ? "" : i + 1 == games.size() || i == 2 ? " and " : ", ") + games[i];
        if (games.size() > 3)
            from += " and others";
        ui::DialogContent content;
        content.icon = ui::StatusKind::warning;
        content.title = std::to_string(count) + " " + what + " can't be read";
        content.body = "The console hands back only zeros for " +
                       std::string(count == 1 ? "this " : "these ") + what +
                       (from.empty() ? "" : " from " + from) + ", so " +
                       (count == 1 ? "it" : "they") + " can't play or be sent.\nIf " +
                       (count == 1 ? "it still plays" : "they still play") +
                       " in the PS5's Media Gallery, keep " + (count == 1 ? "it" : "them") +
                       ". Deleting can't be undone.";
        content.buttons = {{"Keep", ui::ButtonKind::secondary},
                           {"Delete", ui::ButtonKind::primary, true}};
        content.default_button = 0;
        unreadable_paths_ = std::move(paths);
        unreadable_.open(std::move(content), feedback);
    }

    // ---- Send all, selecting, deleting ----

    // The selected captures, in the order shown.
    std::vector<std::string> selection() const
    {
        std::vector<std::string> out;
        for (const std::string &path : paths_)
            if (chosen_.count(path) != 0)
                out.push_back(path);
        return out;
    }

    void stop_selecting()
    {
        selecting_ = false;
        chosen_.clear();
    }

    // The "Send ..." sheet for `paths`: where to, skip the ones already sent,
    // and delete each from the console once it is sent.
    void open_bulk(Context &context, std::vector<std::string> paths, ui::Feedback &feedback)
    {
        if (paths.empty())
        {
            ui::refuse(feedback, grid_.style, InputFrame{}, refusal_);
            return;
        }
        bulk_paths_ = std::move(paths);
        bulk_skip_sent_ = true;
        bulk_delete_ = false;
        bulk_to_ = 0;
        // The title first: the sheet leaves room for it when it opens, and the
        // form goes under it.
        bulk_.set_title(bulk_title());
        bulk_.open(feedback);
        const gfx::Rect area = bulk_.content_rect();
        bulk_form_.set_bounds({area.x - 10.0f, area.y, area.w + 20.0f, area.h});
        bulk_form_.clear(); // a fresh sheet starts from the defaults above
        fill_bulk(context);
        bulk_form_.set_focus(0, true);
        bulk_form_.set_active(true);
        bulk_form_.enter();
    }

    std::string bulk_title() const
    {
        return "Send " + std::to_string(bulk_paths_.size()) +
               (bulk_paths_.size() == 1 ? " capture" : " captures");
    }

    // What the sheet would send now.
    std::vector<std::string> bulk_sendable(Context &context) const
    {
        std::vector<std::string> out;
        for (const std::string &path : bulk_paths_)
        {
            const Capture *capture = context.model.find_capture(path);
            if (capture != nullptr && !capture->blank && !(bulk_skip_sent_ && capture->uploaded))
                out.push_back(path);
        }
        return out;
    }

    // (Re)builds the sheet's rows from its choices, keeping the focus.
    void fill_bulk(Context &context)
    {
        if (bulk_form_.row_count() > 0)
        {
            bulk_to_ = std::max(0, bulk_form_.choice_index(bulk_to));
            bulk_skip_sent_ = bulk_form_.toggle_value(bulk_skip_sent);
            bulk_delete_ = bulk_form_.toggle_value(bulk_delete);
        }
        const int focus = bulk_form_.focus_id();
        const Config &config = context.model.config();
        std::vector<std::string> places = {"Usual for each game"};
        bulk_ids_ = {""};
        for (const Destination &d : config.destinations)
            if (d.enabled && d.configured())
            {
                places.push_back(d.display_name());
                bulk_ids_.push_back(d.id);
            }
        bulk_to_ = std::min(bulk_to_, static_cast<int>(bulk_ids_.size()) - 1);
        const int count = static_cast<int>(bulk_sendable(context).size());
        bulk_.set_title(bulk_title());
        bulk_form_.clear();
        bulk_form_.add_choice(bulk_to, "Send to", std::move(places), bulk_to_).description =
            "Usual: its game's own destination, else the default";
        bulk_form_.add_toggle(bulk_skip_sent, "Skip ones already sent", bulk_skip_sent_)
            .description = "Captures with a check mark stay where they are";
        bulk_form_.add_toggle(bulk_delete, "Delete once sent", bulk_delete_).description =
            "From the console, each only after it has arrived; can't be undone";
        ui::FormRow &go = bulk_form_.add_action(
            bulk_go,
            count == 0 ? std::string("Nothing to send")
                       : "Send " + std::to_string(count) + (count == 1 ? " capture" : " captures"));
        go.disabled = count == 0;
        go.description = "It goes on in the background, even after you close PShare";
        if (focus != 0)
            bulk_form_.focus_row(focus, true);
    }

    std::string bulk_destination_name(Context &context) const
    {
        const std::string &id = bulk_ids_[static_cast<std::size_t>(bulk_to_)];
        const Destination *d = id.empty() ? nullptr : context.model.config().find(id);
        return d != nullptr ? d->display_name() : std::string("each game's destination");
    }

    void confirm_bulk(Context &context, ui::Feedback &feedback)
    {
        if (!bulk_delete_)
        {
            start_bulk(context, feedback);
            return;
        }
        const int count = static_cast<int>(bulk_sendable(context).size());
        ui::DialogContent content;
        content.icon = ui::StatusKind::warning;
        content.title = "Delete each one once it's sent?";
        content.body = "PShare sends " + std::to_string(count) +
                       (count == 1 ? " capture" : " captures") + " to " +
                       bulk_destination_name(context) +
                       " and deletes each from the PS5 as soon as it has arrived. Ones that "
                       "fail to send are kept.\nDeleting can't be undone.";
        content.buttons = {{"Cancel", ui::ButtonKind::secondary},
                           {"Send and delete", ui::ButtonKind::primary, true}};
        content.default_button = 0;
        confirming_ = Confirm::send_then_delete;
        confirm_.open(std::move(content), feedback);
    }

    void start_bulk(Context &context, ui::Feedback &feedback)
    {
        const std::vector<std::string> paths = bulk_sendable(context);
        const std::string &id = bulk_ids_[static_cast<std::size_t>(bulk_to_)];
        std::string why;
        const int taken = context.model.send_batch(paths, id, bulk_delete_, &why);
        if (taken == 0)
        {
            feedback.play(hui::audio::Cue::error);
            context.toasts.push(ui::StatusKind::warning, why);
            return;
        }
        feedback.play(hui::audio::Cue::launch);
        context.toasts.push(ui::StatusKind::info,
                            "Sending " + std::to_string(taken) +
                                (taken == 1 ? " capture" : " captures"),
                            "to " + bulk_destination_name(context) +
                                (bulk_delete_ ? ", deleting each once sent" : ""),
                            3.0f);
        bulk_.close(feedback);
        stop_selecting();
    }

    void ask_delete(Context &context, std::vector<std::string> paths, ui::Feedback &feedback)
    {
        if (paths.empty())
        {
            ui::refuse(feedback, grid_.style, InputFrame{}, refusal_);
            return;
        }
        int unsent = 0;
        for (const std::string &path : paths)
            if (const Capture *capture = context.model.find_capture(path))
                unsent += capture->uploaded ? 0 : 1;
        const int count = static_cast<int>(paths.size());
        ui::DialogContent content;
        content.icon = ui::StatusKind::warning;
        content.title = "Delete " + std::to_string(count) +
                        (count == 1 ? " capture" : " captures") + " from the console?";
        content.body =
            std::string(count == 1 ? "It's" : "They're") +
            " removed from the PS5 for good, Media Gallery included." +
            (unsent > 0 ? "\n" + std::to_string(unsent) + " of them " +
                              (unsent == 1 ? "hasn't" : "haven't") + " been sent anywhere."
                        : std::string()) +
            "\nDeleting can't be undone.";
        content.buttons = {{"Keep", ui::ButtonKind::secondary},
                           {"Delete", ui::ButtonKind::primary, true}};
        content.default_button = 0;
        confirming_ = Confirm::delete_now;
        confirm_paths_ = std::move(paths);
        confirm_.open(std::move(content), feedback);
    }

    void set_zone(Zone zone)
    {
        zone_ = zone;
        set_focused(focused_);
    }

    Filter filter() const
    {
        return tabs_.active() == 1 ? Filter::screenshots
                                   : (tabs_.active() == 2 ? Filter::clips : Filter::all);
    }

    void filter_changed(Context &context)
    {
        rebuild(context, true);
        grid_.enter();
    }

    // Keeps the focus on the same capture when the list changes under it.
    void rebuild(Context &context, bool reset)
    {
        std::string focused;
        if (!reset && grid_.focus() >= 0 && grid_.focus() < static_cast<int>(view_.size()))
            focused = paths_[static_cast<std::size_t>(grid_.focus())];
        const bool was_empty = view_.empty();
        // A game filter whose captures are all gone shows everything again.
        if (!game_.empty())
        {
            bool known = false;
            for (const Capture &capture : context.model.captures())
                known = known || capture.item.title_id == game_;
            if (!known && context.model.scanned_once())
                game_.clear();
        }
        view_ = context.model.filtered(filter(), game_, oldest_first_);
        paths_.clear();
        std::vector<ui::CardItem> items;
        items.reserve(view_.size());
        int keep = 0;
        for (std::size_t i = 0; i < view_.size(); ++i)
        {
            const Capture &capture = context.model.captures()[static_cast<std::size_t>(view_[i])];
            ui::CardItem item;
            item.title = capture.game;
            item.subtitle = capture.item.captured.date() + "  " + capture.item.captured.time();
            item.badge = capture.item.video() ? "CLIP" : "";
            item.selected = capture.uploaded;
            item.tag = view_[i];
            item.accent = capture.item.video() ? rgb(palette::coral) : rgb(palette::aqua);
            items.push_back(std::move(item));
            paths_.push_back(capture.item.path);
            if (capture.item.path == focused)
                keep = static_cast<int>(i);
        }
        grid_.set_items(std::move(items));
        grid_.set_focus(keep, true);
        // A selection keeps only what is still shown.
        for (auto it = chosen_.begin(); it != chosen_.end();)
            it = std::find(paths_.begin(), paths_.end(), *it) == paths_.end() ? chosen_.erase(it)
                                                                              : std::next(it);
        // An empty grid cannot hold the focus; the first captures take it back.
        if (view_.empty() && zone_ == Zone::grid)
            set_zone(Zone::tabs);
        else if (!view_.empty() && (reset || was_empty))
            set_zone(Zone::grid);
    }

    std::string game_label(Context &context) const
    {
        if (game_.empty())
            return "All games";
        for (const Capture &capture : context.model.captures())
            if (capture.item.title_id == game_)
                return capture.game;
        return game_;
    }

    gfx::Rect button_rect(Context &context, int which) const
    {
        const float h = 54.0f;
        const float y = area_.y + 3.0f;
        const float game_w =
            std::min(380.0f, context.fonts.semibold.measure(game_label(context), 22.0f) + 92.0f);
        if (which == 0)
            return {area_.x + kButtonsX, y, game_w, h};
        const float order_w =
            context.fonts.semibold.measure(oldest_first_ ? "Oldest first" : "Newest first", 22.0f) +
            92.0f;
        return {area_.x + kButtonsX + game_w + 14.0f, y, order_w, h};
    }

    void draw_buttons(ui::Canvas &scene, Context &context) const
    {
        gfx::DrawList &list = scene.list;
        for (int which = 0; which < 2; ++which)
        {
            const gfx::Rect r = button_rect(context, which);
            const float f = button_focus_[which];
            const bool on = which == 0 ? !game_.empty() : oldest_first_;
            list.rounded_rect(r, r.h * 0.5f, rgb(0xffffff, 0.07f + 0.05f * f));
            if (f > 0.01f)
                list.rounded_rect(r, r.h * 0.5f, rgb(palette::coral, f));
            list.bordered_rect(r, r.h * 0.5f, gfx::Color{0, 0, 0, 0}, 1.5f,
                               on ? rgb(palette::aqua, 0.9f) : rgb(0xffffff, 0.14f));
            const gfx::Color ink = f > 0.5f ? rgb(palette::ink) : rgb(palette::cream);
            // A small mark first: a funnel for the game, two arrows for the order.
            const float mx = r.x + 30.0f;
            const float my = r.cy();
            if (which == 0)
            {
                list.triangle({mx - 10.0f, my - 9.0f, 20.0f, 12.0f}, ink, 0.0f, 3.1416f);
                list.rounded_rect({mx - 3.0f, my + 1.0f, 6.0f, 9.0f}, 2.0f, ink);
            }
            else
            {
                const bool up = oldest_first_;
                list.rounded_rect({mx - 7.0f, my - 9.0f, 3.0f, 18.0f}, 1.5f, ink);
                list.triangle({mx - 12.0f, up ? my - 14.0f : my + 4.0f, 13.0f, 10.0f}, ink, 0.0f,
                              up ? 0.0f : 3.1416f);
                list.rounded_rect({mx + 4.0f, my - 9.0f, 3.0f, 18.0f}, 1.5f, ink.with_alpha(0.5f));
            }
            std::string label = which == 0 ? game_label(context)
                                           : (oldest_first_ ? "Oldest first" : "Newest first");
            ui::text(list, context.fonts.semibold, label, r.x + 52.0f, r.cy() + 8.0f, 22.0f, ink);
            list.triangle({r.x + r.w - 30.0f, r.cy() - 3.0f, 12.0f, 8.0f}, ink.with_alpha(0.7f),
                          0.0f, 3.1416f);
        }
    }

    void open_picker(Context &context, ui::Feedback &feedback)
    {
        const std::vector<Model::GameCount> games = context.model.games();
        std::vector<ui::ListItem> items;
        picker_ids_.clear();
        const auto counts = [](int shots, int clips)
        {
            std::string out;
            if (shots > 0)
                out += std::to_string(shots) + (shots == 1 ? " screenshot" : " screenshots");
            if (clips > 0)
                out += (out.empty() ? "" : " \xC2\xB7 ") + std::to_string(clips) +
                       (clips == 1 ? " clip" : " clips");
            return out;
        };
        int focus = 0;
        const auto header = [&](const char *title)
        {
            ui::ListItem row;
            row.title = title;
            row.header = true;
            items.push_back(row);
            picker_ids_.push_back(kHeader);
        };
        header("Order");
        for (const bool oldest : {false, true})
        {
            ui::ListItem row;
            row.title = oldest ? "Oldest first" : "Newest first";
            row.subtitle = oldest ? "From the first capture on" : "The latest capture on top";
            row.badge = oldest == oldest_first_ ? "ON" : "";
            items.push_back(row);
            picker_ids_.push_back(oldest ? kOldest : kNewest);
        }
        header("Game");
        if (game_.empty())
            focus = static_cast<int>(items.size());
        ui::ListItem all;
        all.title = "All games";
        all.subtitle = std::to_string(games.size()) + (games.size() == 1 ? " game" : " games");
        all.value = counts(context.model.screenshots(), context.model.clips());
        all.badge = game_.empty() ? "ON" : "";
        items.push_back(all);
        picker_ids_.push_back({});
        for (const Model::GameCount &game : games)
        {
            ui::ListItem item;
            item.title = game.name;
            item.subtitle = game.title_id.empty() ? std::string("No title ID") : game.title_id;
            item.value = counts(game.screenshots, game.clips);
            item.badge = game.title_id == game_ ? "ON" : "";
            if (game.title_id == game_)
                focus = static_cast<int>(items.size());
            items.push_back(std::move(item));
            picker_ids_.push_back(game.title_id);
        }
        picker_.set_title("Show");
        picker_.open(feedback);
        const gfx::Rect area = picker_.content_rect();
        picker_list_.set_bounds({area.x - 10.0f, area.y, area.w + 20.0f, area.h});
        picker_list_.set_items(std::move(items));
        picker_list_.set_focus(focus, true);
        picker_list_.set_active(true);
        picker_list_.enter();
    }

    void play_clip(Context &context, const Capture &capture, ui::Feedback &feedback)
    {
        if (context.player == nullptr)
            return;
        clip_.open(context.player, capture.item.path, capture.game,
                   capture.item.captured.date() + " " + capture.item.captured.time() +
                       "   \xC2\xB7   " + human_size(capture.item.size),
                   feedback);
    }

    std::string browser_url(Context &context) const
    {
        const std::string ip =
            context.model.local_ip().empty() ? "<console address>" : context.model.local_ip();
        // A short address: /v/ and the capture's file name without its extension.
        std::string stem = browser_path_.substr(browser_path_.rfind('/') + 1);
        stem = stem.substr(0, stem.rfind('.'));
        return "http://" + ip + ":" + std::to_string(kSetupPort) + "/v/" + url_encode(stem);
    }

    const Capture *focused_capture(Context &context) const
    {
        if (grid_.focus() < 0 || grid_.focus() >= static_cast<int>(paths_.size()))
            return nullptr;
        return context.model.find_capture(paths_[static_cast<std::size_t>(grid_.focus())]);
    }

    void send(Context &context, ui::Feedback &feedback, const std::string &destination)
    {
        const Capture *capture = viewer_open_      ? context.model.find_capture(viewer_path_)
                                 : clip_.is_open() ? context.model.find_capture(clip_.path())
                                                   : focused_capture(context);
        if (capture == nullptr)
            return;
        std::string why;
        if (context.model.upload(capture->item.path, destination, &why))
        {
            feedback.play(hui::audio::Cue::launch);
            const Destination *d =
                destination.empty() ? context.model.config().destination_for(capture->item.title_id)
                                    : context.model.config().find(destination);
            context.toasts.push(ui::StatusKind::info,
                                std::string("Sending to ") + (d ? d->display_name() : "..."),
                                capture->game, 2.5f);
        }
        else
        {
            feedback.play(hui::audio::Cue::error);
            context.toasts.push(ui::StatusKind::warning, why,
                                why.find("set up") != std::string::npos
                                    ? "Open Destinations to add a webhook or a server"
                                    : "");
        }
    }

    void open_sheet(Context &context, ui::Feedback &feedback)
    {
        const Capture *capture = focused_capture(context);
        if (capture == nullptr)
            return;
        sheet_path_ = capture->item.path;
        sheet_.set_title(capture->item.kind_label());
        std::vector<ui::ListItem> items;
        if (capture->item.video())
        {
            ui::ListItem play;
            play.title = "Play";
            play.subtitle = context.player != nullptr ? "With sound" : "Not in this build";
            play.disabled = context.player == nullptr;
            play.tag = action_play;
            items.push_back(play);
        }
        const Destination *chosen = context.model.config().destination_for(capture->item.title_id);
        ui::ListItem send_default;
        send_default.title = chosen ? "Send to " + chosen->display_name() : "Send";
        send_default.subtitle =
            chosen ? std::string(type_label(chosen->type)) : "No destination yet";
        send_default.tag = action_send_default;
        send_default.disabled = chosen == nullptr || !chosen->configured();
        items.push_back(send_default);
        ui::ListItem send_to;
        send_to.title = "Send to...";
        send_to.chevron = true;
        send_to.tag = action_send_to;
        items.push_back(send_to);
        if (capture->item.web_image() || capture->item.jxr())
        {
            ui::ListItem view;
            view.title = "View full screen";
            view.tag = action_view;
            items.push_back(view);
        }
        {
            ui::ListItem browser;
            browser.title = "Open in a browser";
            browser.subtitle = "On your phone or PC, with the same controls";
            browser.chevron = true;
            browser.tag = action_browser;
            items.push_back(browser);
        }
        if (capture->uploaded && starts_with(capture->link, "http"))
        {
            ui::ListItem link;
            link.title = "Show the link";
            link.subtitle = "As a QR code for your phone";
            link.tag = action_link;
            items.push_back(link);
        }
        actions_.set_items(std::move(items));
        actions_.set_focus(0, true);
        sheet_.open(feedback);
        const gfx::Rect area = sheet_.content_rect();
        const float preview = (area.w * 9.0f / 16.0f);
        actions_.set_bounds(
            {area.x - 10.0f, area.y + preview + 190.0f, area.w + 20.0f, area.h - preview - 190.0f});
        actions_.enter();
    }

    void open_destinations(Context &context, ui::Feedback &feedback, const gfx::Rect &anchor)
    {
        const Config &config = context.model.config();
        std::vector<ui::MenuItem> items;
        for (std::size_t i = 0; i < config.destinations.size(); ++i)
        {
            const Destination &d = config.destinations[i];
            ui::MenuItem item;
            item.label = d.display_name();
            item.shortcut = !d.enabled ? "Off" : (d.configured() ? "" : "Not set up");
            item.disabled = !d.enabled || !d.configured();
            item.tag = static_cast<int>(i);
            items.push_back(std::move(item));
        }
        if (items.empty())
        {
            ui::refuse(feedback, grid_.style, InputFrame{}, refusal_);
            return;
        }
        destinations_.set_items(std::move(items));
        int first = 0;
        while (first < static_cast<int>(destinations_.items().size()) &&
               destinations_.item(first).disabled)
            ++first;
        destinations_.set_focus(first < static_cast<int>(destinations_.items().size()) ? first : 0);
        destinations_.open(anchor, feedback);
    }

    void run_action(int tag, ui::Feedback &feedback, Context &context)
    {
        const Capture *capture = context.model.find_capture(sheet_path_);
        if (capture == nullptr)
            return;
        switch (tag)
        {
        case action_send_default:
            send(context, feedback, {});
            sheet_.close(feedback);
            break;
        case action_send_to:
            open_destinations(context, feedback, actions_.row_rect(actions_.focus()));
            break;
        case action_view:
        {
            ui::ViewerImage image;
            image.title = capture->game;
            image.caption = capture->item.captured.date() + " " + capture->item.captured.time();
            const Thumbs::Texture thumb = context.thumbs.get(capture->item.path);
            image.texture = thumb.id;
            image.aspect = thumb.aspect;
            viewer_.set_images({image});
            viewer_.set_mode(ui::ZoomMode::fit, true);
            viewer_.set_active(true);
            viewer_texture_ = thumb.id;
            viewer_path_ = capture->item.path;
            viewer_open_ = true;
            sheet_.dismiss();
            feedback.play(hui::audio::Cue::open);
            break;
        }
        case action_play:
            if (capture->blank)
            {
                sheet_.dismiss();
                ask_unreadable(context, {capture->item.path}, feedback);
            }
            else if (context.player != nullptr)
            {
                play_clip(context, *capture, feedback);
                sheet_.dismiss();
            }
            break;
        case action_browser:
            // pshared serves the page; the app keeps it up while it runs.
            context.model.open_phone_setup();
            browser_path_ = capture->item.path;
            browser_ready_ = context.model.daemon().setup_port > 0;
            context.link_panel.open(
                capture->game, browser_ready_ ? browser_url(context) : std::string(),
                "Scan it with a phone, or type the address in a browser on the same network. "
                "The controls are the ones you have here, and a controller on a PC works "
                "too.",
                feedback);
            sheet_.close(feedback);
            break;
        case action_link:
            context.link_panel.open(capture->game, capture->link,
                                    "Scan it with your phone's camera to open the upload.",
                                    feedback);
            sheet_.close(feedback);
            break;
        default:
            break;
        }
    }

    void handle_viewer(const InputFrame &input, ui::Feedback &feedback, Context &context)
    {
        if (input.is_pressed(Action::back))
        {
            viewer_open_ = false;
            viewer_path_.clear();
            feedback.play(hui::audio::Cue::back);
            return;
        }
        if (input.is_pressed(Action::west))
        {
            send(context, feedback, {});
            return;
        }
        viewer_.handle(input, feedback);
    }

    // ---- drawing ----

    void draw_art(ui::Canvas &canvas, const gfx::Rect &art, float radius, const ui::CardItem &item,
                  float focus) const
    {
        gfx::DrawList &list = canvas.list;
        if (drawing_ == nullptr || item.tag < 0 ||
            item.tag >= static_cast<int>(drawing_->model.captures().size()))
            return;
        Context &context = *drawing_;
        const Capture &capture = context.model.captures()[static_cast<std::size_t>(item.tag)];
        const bool video = capture.item.video();
        // A clip shows a picture of its own when the decoder could make one,
        // else its game's icon.
        Thumbs::Texture texture = context.thumbs.get(capture.item.path);
        bool icon = false;
        if (video && texture.failed)
        {
            texture = context.thumbs.get(context.model.game_icon(capture.item.title_id));
            icon = true;
        }
        // The placeholder is the night sky with the kind's icon: what a clip
        // without a game icon keeps, and what a screenshot shows while it loads.
        list.gradient_rect(art, radius, video ? rgb(0x3a1f5c) : rgb(0x1f2a5c),
                           video ? rgb(0x22143f) : rgb(0x161a40));
        if (texture.id != 0 && !icon)
        {
            // Fill the cell, cropping whatever does not fit.
            const float cell = art.w / art.h;
            gfx::Rect uv = gfx::kFullUv;
            if (texture.aspect > cell)
            {
                const float keep = cell / texture.aspect;
                uv = {(1.0f - keep) * 0.5f, 0.0f, keep, 1.0f};
            }
            else if (texture.aspect < cell)
            {
                const float keep = texture.aspect / cell;
                uv = {0.0f, (1.0f - keep) * 0.5f, 1.0f, keep};
            }
            list.image(texture.id, art, uv, rgb(0xffffff), radius);
        }
        else if (texture.id != 0 && icon)
        {
            // The game's icon, as a square in the middle of a wash of its colour.
            const float side = art.h * 0.78f;
            list.image(texture.id, art, {0.3f, 0.3f, 0.4f, 0.4f}, rgb(0xffffff, 0.35f), radius);
            list.shadow({art.cx() - side * 0.5f, art.cy() - side * 0.5f + 6.0f, side, side}, 16.0f,
                        18.0f, rgb(palette::ink, 0.6f));
            list.image(texture.id, {art.cx() - side * 0.5f, art.cy() - side * 0.5f, side, side},
                       gfx::kFullUv, rgb(0xffffff), 16.0f);
        }
        else
        {
            const float side = art.h * 0.36f;
            draw_icon(list, video ? Icon::play : Icon::captures,
                      {art.cx() - side * 0.5f, art.cy() - side * 0.5f, side, side},
                      rgb(palette::mist,
                          texture.failed ? 0.8f : 0.35f + 0.25f * ui::breathe(canvas.time)));
        }
        if (video)
        {
            const float r = 26.0f + 4.0f * focus;
            list.circle(art.x + art.w - r - 14.0f, art.y + art.h - r - 14.0f, r,
                        rgb(palette::ink, 0.72f));
            draw_icon(list, Icon::play,
                      {art.x + art.w - r * 2.0f - 14.0f + r * 0.45f,
                       art.y + art.h - r * 2.0f - 14.0f + r * 0.45f, r * 1.1f, r * 1.1f},
                      rgb(palette::cream));
        }
        if (selecting_)
        {
            // Selecting: a ring on what is chosen, and a round tick box on every card.
            const bool chosen = chosen_.count(capture.item.path) != 0;
            if (chosen)
                list.bordered_rect(art, radius, gfx::Color{0, 0, 0, 0}, 5.0f, rgb(palette::coral));
            const float cx = art.x + art.w - 26.0f;
            const float cy = art.y + 26.0f;
            list.circle(cx, cy, 17.0f, chosen ? rgb(palette::coral) : rgb(palette::ink, 0.6f));
            if (chosen)
                draw_icon(list, Icon::check, {cx - 11.0f, cy - 11.0f, 22.0f, 22.0f},
                          rgb(palette::ink));
            else
                list.bordered_rect({cx - 15.0f, cy - 15.0f, 30.0f, 30.0f}, 15.0f,
                                   gfx::Color{0, 0, 0, 0}, 2.5f, rgb(0xffffff, 0.85f));
        }
        else if (capture.failed && !capture.uploaded)
        {
            const float cx = art.x + art.w - 24.0f;
            const float cy = art.y + 24.0f;
            list.circle(cx, cy, 15.0f, rgb(0xff5d6c));
            ui::text(list, canvas.fonts.semibold, "!", cx, cy + 8.0f, 22.0f, rgb(0xffffff),
                     gfx::Align::center);
        }
    }

    void draw_sheet(ui::Canvas &canvas, const gfx::Rect &area) const
    {
        if (drawing_ == nullptr)
            return;
        Context &context = *drawing_;
        const Capture *capture = context.model.find_capture(sheet_path_);
        if (capture == nullptr)
            return;
        gfx::DrawList &list = canvas.list;
        const float h = area.w * 9.0f / 16.0f;
        const gfx::Rect preview{area.x, area.y, area.w, h};
        ui::CardItem item;
        item.tag = static_cast<int>(capture - context.model.captures().data());
        draw_art(canvas, preview, 18.0f, item, 0.0f);
        float y = preview.y + preview.h + 46.0f;
        ui::Painter paint(list, canvas.fonts, theme(), canvas.glass);
        paint.heading(ui::fit_label(paint, capture->game, 32.0f, area.w), area.x, y, 32.0f,
                      rgb(palette::cream));
        y += 38.0f;
        const std::string facts =
            capture->item.captured.date() + "  " + capture->item.captured.time() + "   ·   " +
            human_size(capture->item.size) +
            (capture->item.title_id.empty() ? "" : "   ·   " + capture->item.title_id);
        ui::text(list, canvas.fonts.regular, facts, area.x, y, 21.0f, rgb(palette::mist));
        y += 44.0f;
        gfx::Color status_color = rgb(palette::mist);
        std::string status = "Not sent yet";
        Icon icon = Icon::clock;
        const UploadView upload = context.model.upload_view();
        if (upload.active && upload.job.path == capture->item.path)
        {
            status = "Sending now...";
            status_color = rgb(palette::sun);
            icon = Icon::upload;
        }
        else if (capture->uploaded)
        {
            status = capture->link.empty() ? "Sent" : "Sent · " + capture->link;
            status_color = rgb(palette::aqua);
            icon = Icon::check;
        }
        else if (capture->failed)
        {
            status = "The last try failed";
            status_color = rgb(0xff8a95);
            icon = Icon::cross;
        }
        draw_icon(list, icon, {area.x, y - 22.0f, 26.0f, 26.0f}, status_color);
        ui::text(list, canvas.fonts.semibold,
                 canvas.fonts.semibold.font->fit(status, 22.0f, area.w - 40.0f), area.x + 38.0f, y,
                 22.0f, status_color);
        actions_.draw(canvas);
    }

    gfx::Rect area_;
    ui::TabBar tabs_;
    ui::GridView grid_;
    ui::Sheet sheet_;
    ui::Dialog unreadable_;
    std::vector<std::string> unreadable_paths_;
    bool unreadable_pending_ = false;
    // Selecting captures (Options > Select captures).
    bool selecting_ = false;
    std::set<std::string> chosen_;
    mutable std::string select_hint_; // "Select (12)": the hint bar keeps a pointer
    // The "Send ..." sheet and what it is about.
    ui::Sheet bulk_;
    ui::Form bulk_form_;
    std::vector<std::string> bulk_paths_;
    std::vector<std::string> bulk_ids_; // destination id per "Send to" choice; "" = usual
    int bulk_to_ = 0;
    bool bulk_skip_sent_ = true;
    bool bulk_delete_ = false;
    // Asking before deleting.
    ui::Dialog confirm_;
    Confirm confirming_ = Confirm::none;
    std::vector<std::string> confirm_paths_;
    ui::ListView actions_;
    ui::Menu destinations_;
    ui::Sheet picker_; // "Show": the order, then every game with its counts
    ui::ListView picker_list_;
    std::vector<std::string> picker_ids_;
    std::string game_; // title ID shown; empty = all
    bool oldest_first_ = false;
    float button_focus_[2] = {0.0f, 0.0f};
    ui::ImageViewer viewer_;
    ClipPlayer clip_;
    ui::EmptyState empty_;
    ui::Pulse refusal_;
    hui::tween::Spring viewer_fade_;
    std::vector<int> view_;
    std::vector<std::string> paths_;
    std::string sheet_path_;
    std::string viewer_path_;
    std::uint32_t viewer_texture_ = 0;
    bool viewer_open_ = false;
    bool focused_ = false;
    std::string browser_path_; // the capture the browser QR is for
    bool browser_ready_ = false;
    Zone zone_ = Zone::grid;
    mutable Context *drawing_ = nullptr; // the slots draw with it
};
} // namespace

std::unique_ptr<Page> make_captures_page()
{
    return std::make_unique<CapturesPage>();
}

} // namespace pshare::app
