// PShare - Destinations: where captures go, and what each one needs.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include "pshare/core/text.hpp"

#include <algorithm>

namespace pshare::app
{
namespace
{
enum RowId
{
    row_enabled = 1,
    row_default = 2,
    row_test = 3,
    row_url = 10,
    row_token,
    row_chat,
    row_username,
    row_message,
    row_limit,
    row_fallback,
    row_keep,
    row_field,
    row_name,
    row_path,
    row_password,
    row_also,
};

constexpr int kLimits[] = {0, 10, 25, 50, 100, 200, 500};
constexpr const char *kKeep[] = {"1h", "12h", "24h", "72h"};
// Where a "folder" destination copies to: a USB drive, or the console itself.
constexpr const char *kFolderPaths[] = {"usb", "/data/pshare/Saved"};

bool also_sends(const Config &config, const std::string &id)
{
    return std::find(config.also_send.begin(), config.also_send.end(), id) !=
           config.also_send.end();
}

enum class Zone : std::uint8_t
{
    list,
    form,
};

// Tokens are shown by their last four characters only.
std::string masked(const std::string &value)
{
    if (value.empty())
        return "Not set";
    if (value.size() <= 4)
        return "****";
    return "****" + value.substr(value.size() - 4);
}

std::string shortened(const std::string &value)
{
    if (value.empty())
        return "Not set";
    std::string text = value;
    for (const char *prefix : {"https://", "http://"})
        if (starts_with(text, prefix))
            text = text.substr(std::string_view(prefix).size());
    return text.size() > 30 ? text.substr(0, 27) + "..." : text;
}

class DestinationsPage final : public Page
{
  public:
    DestinationsPage()
    {
        list_.style.theme = theme();
        list_.style.panel = true;
        list_.style.row_height = 82.0f;
        list_.style.title_size = 26.0f;
        list_.style.subtitle_size = 19.0f;
        list_.style.value_size = 20.0f;
        list_.style.highlight.kind = ui::HighlightKind::fill;

        form_.style.theme = theme();
        form_.style.panel = true;
        form_.style.on_page = false;
        form_.style.row_height = 68.0f;
        form_.style.label_size = 24.0f;
        form_.style.value_size = 22.0f;
        form_.style.control_width = 300.0f;
        form_.style.highlight.kind = ui::HighlightKind::fill;

        prompt_.style.theme = theme();
        prompt_.style.width = 1100.0f;
    }

    const char *title() const override
    {
        return "Destinations";
    }

    void layout(const gfx::Rect &area) override
    {
        area_ = area;
        list_.set_bounds({area.x, area.y, 520.0f, area.h});
        form_.set_bounds({area.x + 560.0f, area.y, area.w - 560.0f, area.h - 70.0f});
    }

    void enter(Context &context) override
    {
        rebuild_list(context);
        rebuild_form(context);
        list_.enter();
        form_.enter();
    }

    void set_focused(bool focused) override
    {
        focused_ = focused;
        list_.set_active(focused && zone_ == Zone::list);
        form_.set_active(focused && zone_ == Zone::form);
    }

    bool modal() const override
    {
        return prompt_.is_open();
    }
    bool wants_glass() const override
    {
        return prompt_.visible();
    }

    int hints(ui::Hint *out, int capacity) const override
    {
        int n = 0;
        if (capacity < 3)
            return 0;
        if (prompt_.is_open())
        {
            out[n++] = {ui::Button::cross, "Type"};
            out[n++] = {ui::Button::circle, "Cancel"};
            return n;
        }
        out[n++] = {ui::Button::cross, zone_ == Zone::list ? "Edit" : "Change"};
        out[n++] = {ui::Button::circle, "Back"};
        if (zone_ == Zone::list)
            out[n++] = {ui::Button::square, "Use by default"};
        return n;
    }

    bool handle(const InputFrame &input, ui::Feedback &feedback, Context &context) override
    {
        if (prompt_.is_open())
        {
            if (prompt_.handle(input, feedback) == ui::Event::activated)
                commit_text(context, prompt_.text());
            return false;
        }
        if (zone_ == Zone::list)
        {
            if (input.is_pressed(Action::back))
                return true;
            if (input.is_pressed(Action::west))
            {
                make_default(context, feedback);
                return false;
            }
            if (input.nav == Direction::left && !input.nav_repeat)
                return true;
            const ui::Event event = list_.handle(input, feedback);
            if (event == ui::Event::moved)
                rebuild_form(context);
            if (event == ui::Event::activated ||
                (input.nav == Direction::right && !input.nav_repeat))
            {
                zone_ = Zone::form;
                set_focused(focused_);
                ui::play_cue(feedback, form_.style, form_.style.sounds.move);
            }
            return false;
        }
        // The form.
        if (input.is_pressed(Action::back) ||
            (input.nav == Direction::left && !form_.uses_horizontal() && !input.nav_repeat))
        {
            zone_ = Zone::list;
            set_focused(focused_);
            ui::play_cue(feedback, list_.style, list_.style.sounds.cancel);
            return false;
        }
        const ui::Event event = form_.handle(input, feedback);
        if (event == ui::Event::changed || event == ui::Event::activated)
            apply(context, feedback, form_.changed_id(), event);
        return false;
    }

    void on_event(const ModelEvent &event, ui::Feedback &, Context &context) override
    {
        if (event.kind == EventKind::config_saved)
            rebuild_list(context);
    }

    void update(float dt, ui::Feedback &feedback, Context &context) override
    {
        (void)feedback;
        (void)context;
        list_.update(dt);
        form_.update(dt);
        prompt_.update(dt);
    }

    void draw(ui::Canvas &scene, Context &context) const override
    {
        list_.draw(scene);
        form_.draw(scene);
        // The focused row's help, under the form.
        const std::string &help = form_.help_text();
        const gfx::Rect bounds = form_.bounds();
        const std::string text =
            zone_ == Zone::form && !help.empty()
                ? help
                : "Typing a long webhook with the controller? Auto upload > Set up from your "
                  "phone lets you paste it instead.";
        ui::paragraph(scene.list, scene.fonts.regular, text, bounds.x + 8.0f,
                      bounds.y + bounds.h + 36.0f, 20.0f, bounds.w - 16.0f, 28.0f,
                      rgb(palette::mist), 2);
        (void)context;
    }

    void draw_overlay(ui::Canvas &overlay, Context &context) const override
    {
        (void)context;
        prompt_.draw(overlay);
    }

  private:
    Destination *focused_destination(Context &context)
    {
        Config &config = context.model.edit_config();
        const int index = list_.focus();
        if (index < 0 || index >= static_cast<int>(config.destinations.size()))
            return nullptr;
        return &config.destinations[static_cast<std::size_t>(index)];
    }

    void rebuild_list(Context &context)
    {
        const Config &config = context.model.config();
        std::vector<ui::ListItem> items;
        for (const Destination &d : config.destinations)
        {
            ui::ListItem item;
            item.title = d.display_name();
            item.subtitle = type_label(d.type);
            item.swatch = destination_color(d.type);
            if (!d.enabled)
                item.value = "Off";
            else if (!d.configured())
                item.value = "Set up";
            else
                item.value = "Ready";
            if (d.id == config.default_destination)
                item.badge = "DEFAULT";
            items.push_back(std::move(item));
        }
        const int focus = list_.focus();
        list_.set_items(std::move(items));
        list_.set_focus(
            std::clamp(focus, 0, std::max(0, static_cast<int>(config.destinations.size()) - 1)),
            true);
    }

    void rebuild_form(Context &context)
    {
        const int focus_id = form_.focus_id();
        const int focused_index = focused_destination(context) ? list_.focus() : -1;
        form_.clear();
        Destination *d = focused_destination(context);
        if (d == nullptr)
            return;
        const Config &config = context.model.config();
        form_.add_header(std::string(type_label(d->type)));
        form_.add_toggle(row_enabled, "Enabled", d->enabled);
        ui::FormRow &by_default = form_.add_action(row_default, "Use by default");
        by_default.text = d->id == config.default_destination ? "Yes" : "";
        by_default.description = "Captures go here unless a game has its own destination";
        ui::FormRow &name = form_.add_action(row_name, "Name");
        name.text = d->display_name();
        name.chevron = true;
        const auto field = [&](int id, const char *label, std::string text, const char *help)
        {
            ui::FormRow &row = form_.add_action(id, label);
            row.text = std::move(text);
            row.chevron = true;
            row.description = help;
        };
        switch (d->type)
        {
        case DestinationType::discord:
            field(row_url, "Webhook URL", shortened(d->url),
                  "Channel settings > Integrations > Webhooks > Copy Webhook URL");
            field(row_username, "Post as", d->username.empty() ? "Webhook's name" : d->username,
                  "The name the posts appear under");
            field(row_message, "Message", d->message.empty() ? "Default" : d->message,
                  "{type} {game} {date} {time} {size} {link} become the capture's details");
            break;
        case DestinationType::telegram:
            field(row_token, "Bot token", masked(d->token), "From @BotFather: /newbot");
            field(row_chat, "Chat ID", d->chat_id.empty() ? "Not set" : d->chat_id,
                  "The chat, group or channel the bot posts in (the bot must be a member)");
            field(row_message, "Caption", d->message.empty() ? "Default" : d->message,
                  "{type} {game} {date} {time} {size} {link} become the capture's details");
            break;
        case DestinationType::catbox:
            field(row_token, "User hash", d->token.empty() ? "Anonymous" : masked(d->token),
                  "Optional: uploads then appear in your catbox.moe account");
            break;
        case DestinationType::litterbox:
            break;
        case DestinationType::folder:
        {
            int where = 2;
            for (int i = 0; i < 2; ++i)
                if (iequals(d->path, kFolderPaths[i]) || (i == 0 && d->path.empty()))
                    where = i;
            std::vector<std::string> places = {"USB drive", "Console storage"};
            if (where == 2)
                places.push_back(shortened(d->path));
            form_.add_choice(row_path, "Copy to", std::move(places), where).description =
                "Files land in PShare/<game>/ on the first USB drive, or in /data/pshare/Saved";
            break;
        }
        case DestinationType::smb:
            field(row_url, "Share", shortened(d->url),
                  "smb://<PC address>/<shared folder>/<optional subfolder>");
            field(row_username, "User name", d->username.empty() ? "Guest" : d->username,
                  "Your Windows (or Samba) account; leave empty for a guest share");
            field(row_password, "Password", masked(d->token), "That account's password");
            break;
        case DestinationType::http:
        case DestinationType::count:
            field(row_url, "Upload URL", shortened(d->url),
                  "Gets a multipart/form-data POST with the capture");
            field(row_field, "File field", d->field, "The form field that carries the file");
            break;
        }
        if (d->type == DestinationType::litterbox)
        {
            int keep = 3;
            for (int i = 0; i < 4; ++i)
                if (d->litterbox_time == kKeep[i])
                    keep = i;
            form_.add_choice(row_keep, "Keep files for", {"1 hour", "12 hours", "1 day", "3 days"},
                             keep);
        }
        if (d->type == DestinationType::discord || d->type == DestinationType::telegram)
        {
            std::vector<std::string> limits;
            int limit = 0;
            for (std::size_t i = 0; i < std::size(kLimits); ++i)
            {
                limits.push_back(kLimits[i] == 0 ? "Service limit"
                                                 : std::to_string(kLimits[i]) + " MB");
                if (kLimits[i] == d->max_mb)
                    limit = static_cast<int>(i);
            }
            form_.add_choice(row_limit, "Size limit", std::move(limits), limit).description =
                d->type == DestinationType::telegram
                    ? "Large videos are split into numbered parts"
                    : "Bigger files go to the fallback, and the link is posted instead";
            std::vector<std::string> fallbacks = {"None"};
            fallback_ids_ = {""};
            int chosen = 0;
            for (const Destination &other : config.destinations)
            {
                if (other.id == d->id || (other.type != DestinationType::catbox &&
                                          other.type != DestinationType::litterbox &&
                                          other.type != DestinationType::http))
                    continue;
                if (other.id == d->fallback)
                    chosen = static_cast<int>(fallbacks.size());
                fallbacks.push_back(other.display_name());
                fallback_ids_.push_back(other.id);
            }
            form_.add_choice(row_fallback, "Big files go to", std::move(fallbacks), chosen);
        }
        form_.add_toggle(row_also, "Also gets every new capture", also_sends(config, d->id))
            .description = d->local()
                               ? "Auto upload copies each new capture here too, besides the default"
                               : "Auto upload sends each new capture here too, besides the default";
        ui::FormRow &test = form_.add_action(row_test, "Send a test");
        test.description = "Sends your newest screenshot here now";
        test.disabled = !d->configured();
        if (focused_index == form_index_owner_)
            form_.focus_row(focus_id, true);
        form_index_owner_ = focused_index;
    }

    void make_default(Context &context, ui::Feedback &feedback)
    {
        Destination *d = focused_destination(context);
        if (d == nullptr)
            return;
        context.model.edit_config().default_destination = d->id;
        if (!d->enabled)
            d->enabled = true;
        context.model.save_config();
        feedback.play(hui::audio::Cue::launch);
        context.toasts.push(ui::StatusKind::success, d->display_name() + " is the default", {},
                            2.5f);
        rebuild_list(context);
        rebuild_form(context);
    }

    void edit_text(int id, const std::string &title, bool password)
    {
        editing_ = id;
        prompt_.set_title(title);
        prompt_.style.password = password;
        prompt_.style.allow_empty = true;
        prompt_.style.auto_capital = id == row_name || id == row_message || id == row_username;
        prompt_.style.max_length = 400;
    }

    void apply(Context &context, ui::Feedback &feedback, int id, ui::Event event)
    {
        Destination *d = focused_destination(context);
        if (d == nullptr)
            return;
        switch (id)
        {
        case row_enabled:
            d->enabled = form_.toggle_value(row_enabled);
            break;
        case row_default:
            make_default(context, feedback);
            return;
        case row_keep:
            d->litterbox_time = kKeep[std::clamp(form_.choice_index(row_keep), 0, 3)];
            break;
        case row_limit:
            d->max_mb = kLimits[std::clamp(form_.choice_index(row_limit), 0,
                                           static_cast<int>(std::size(kLimits)) - 1)];
            break;
        case row_fallback:
        {
            const int index = form_.choice_index(row_fallback);
            if (index >= 0 && index < static_cast<int>(fallback_ids_.size()))
                d->fallback = fallback_ids_[static_cast<std::size_t>(index)];
            break;
        }
        case row_path:
        {
            const int index = form_.choice_index(row_path);
            if (index >= 0 && index < 2)
                d->path = kFolderPaths[index];
            break;
        }
        case row_also:
        {
            auto &list = context.model.edit_config().also_send;
            list.erase(std::remove(list.begin(), list.end(), d->id), list.end());
            if (form_.toggle_value(row_also))
            {
                list.push_back(d->id);
                d->enabled = true;
            }
            break;
        }
        case row_test:
        {
            send_test(context, feedback, *d);
            return;
        }
        default:
            if (event != ui::Event::activated)
                return;
            open_prompt(context, feedback, id, *d);
            return;
        }
        context.model.save_config();
        rebuild_list(context);
    }

    void open_prompt(Context &context, ui::Feedback &feedback, int id, const Destination &d)
    {
        (void)context;
        std::string value;
        std::string title;
        bool secret = false;
        switch (id)
        {
        case row_name:
            title = "Name";
            value = d.name;
            break;
        case row_url:
            title = d.type == DestinationType::discord ? "Webhook URL"
                    : d.type == DestinationType::smb   ? "Share (smb://PC/folder)"
                                                       : "URL";
            value = d.url;
            if (value.empty() && d.type == DestinationType::smb)
                value = "smb://";
            break;
        case row_password:
            title = "Password";
            value = d.token;
            secret = true;
            break;
        case row_token:
            title = d.type == DestinationType::telegram ? "Bot token" : "Token";
            value = d.token;
            secret = d.type == DestinationType::telegram;
            break;
        case row_chat:
            title = "Chat ID";
            value = d.chat_id;
            break;
        case row_username:
            title = d.type == DestinationType::smb ? "User name" : "Post as";
            value = d.username;
            break;
        case row_message:
            title = "Message";
            value = d.message;
            break;
        case row_field:
            title = "File field";
            value = d.field;
            break;
        default:
            return;
        }
        edit_text(id, title, secret);
        prompt_.open(feedback, value);
    }

    void commit_text(Context &context, const std::string &text)
    {
        Destination *d = focused_destination(context);
        if (d == nullptr)
            return;
        const std::string value(trim(text));
        switch (editing_)
        {
        case row_name:
            d->name = value;
            break;
        case row_url:
            d->url = value;
            break;
        case row_token:
        case row_password:
            d->token = value;
            break;
        case row_chat:
            d->chat_id = value;
            break;
        case row_username:
            d->username = value;
            break;
        case row_message:
            d->message = value;
            break;
        case row_field:
            d->field = value.empty() ? "file" : value;
            break;
        default:
            return;
        }
        context.model.save_config();
        rebuild_list(context);
        rebuild_form(context);
    }

    void send_test(Context &context, ui::Feedback &feedback, const Destination &d)
    {
        const auto &captures = context.model.captures();
        const auto newest = std::find_if(captures.begin(), captures.end(),
                                         [](const Capture &c) { return !c.item.video(); });
        if (newest == captures.end())
        {
            feedback.play(hui::audio::Cue::error);
            context.toasts.push(ui::StatusKind::warning, "No screenshot to send",
                                "Take one with the Create button first");
            return;
        }
        std::string why;
        if (context.model.upload(newest->item.path, d.id, &why))
        {
            feedback.play(hui::audio::Cue::launch);
            context.toasts.push(ui::StatusKind::info, "Sending a test to " + d.display_name(),
                                newest->game, 2.5f);
        }
        else
        {
            feedback.play(hui::audio::Cue::error);
            context.toasts.push(ui::StatusKind::warning, why);
        }
    }

    gfx::Rect area_;
    ui::ListView list_;
    ui::Form form_;
    ui::InputPrompt prompt_;
    std::vector<std::string> fallback_ids_;
    int editing_ = 0;
    int form_index_owner_ = -2;
    bool focused_ = false;
    Zone zone_ = Zone::list;
};
} // namespace

std::unique_ptr<Page> make_destinations_page()
{
    return std::make_unique<DestinationsPage>();
}

} // namespace pshare::app
