// PShare - Application entry point on the PS5.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Start-up, in the order the console needs it:
//   1. read the title's identity from /app0 while /app0 is still the app's;
//   2. ask Lapy for filesystem access (single-threaded, before anything else
//      starts a thread): the captures live in /user/av_contents and PShare's
//      files in /data/pshare, both outside the sandbox;
//   3. find the install folder again (an elevated title's root is the
//      console's, so /app0 may be gone) and open display, fonts, art, pad
//      and sound from there;
//   4. run the app until it asks to close, then close through the system.

#include "app/app.hpp"
#include "app/model.hpp"
#include "app/thumbs.hpp"
#include "app/video.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "elevation/elevation.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "pshare/core/text.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <cstdint>
#include <span>
#include <string>
#include <sys/stat.h>

namespace
{
using namespace pshare;

bool is_directory(const std::string &path)
{
    struct stat info
    {
    };
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Where the install folder is once the title may have left its sandbox.
std::string find_app_root(const std::string &title_id)
{
    const std::string candidates[] = {
        "/app0",
        "/system_ex/app/" + title_id,
        "/user/app/" + title_id,
        "/mnt/sandbox/" + title_id + "_000/app0",
        "/data/homebrew/" + title_id,
        "/mnt/usb0/homebrew/" + title_id,
        "/mnt/ext0/homebrew/" + title_id,
    };
    for (const std::string &candidate : candidates)
        if (is_directory(candidate + "/assets"))
            return candidate;
    return "/app0";
}

const char *elevation_text(elevation::Status status)
{
    switch (status)
    {
    case elevation::Status::ok:
        return "granted";
    case elevation::Status::unavailable:
        return "no Lapy service and no ELF loader on port 9021";
    case elevation::Status::target_mismatch:
        return "the Lapy helper was built for another title ID";
    case elevation::Status::unsupported_version:
    case elevation::Status::unsupported_capability:
        return "this Lapy does not support the request";
    case elevation::Status::timeout:
        return "Lapy did not answer in time";
    case elevation::Status::prepare_failed:
    case elevation::Status::apply_failed:
    case elevation::Status::rollback_failed:
        return "Lapy could not apply it on this firmware";
    default:
        return "Lapy refused the request";
    }
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data, 16u << 20) || !font->load(data))
    {
        hui::sys::log("[PShare] font %s failed", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

std::uint32_t load_art(hui::gfx::Renderer &renderer, const std::string &path, float *aspect)
{
    Picture picture;
    if (!decode_picture(path, 2048, 2048, &picture))
    {
        hui::sys::log("[PShare] art %s missing", path.c_str());
        return 0;
    }
    if (aspect != nullptr)
        *aspect = static_cast<float>(picture.width) / static_cast<float>(picture.height);
    return renderer.batch().create_texture(picture.width, picture.height, picture.rgba.data());
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[PShare] entry, version %s", kVersion);

    std::string param;
    std::string title_id = "PPSA99773";
    if (save::read_file("/app0/sce_sys/param.json", &param, 64u << 10))
        json_find_string(param, "titleId", &title_id);

    const elevation::Status access = elevation::request(elevation::Capability::filesystem);
    sys::log("[PShare] elevation %s via %s; stopped at %s (%d)", elevation_text(access),
             elevation::path(), elevation::step(), elevation::step_code());

    app::Environment environment;
    environment.elevated = access == elevation::Status::ok;
    environment.elevation_note = elevation_text(access);
    if (access != elevation::Status::ok)
    {
        environment.elevation_note += " (stopped at ";
        environment.elevation_note += elevation::step();
        environment.elevation_note += ": " + std::to_string(elevation::step_code()) + ")";
    }
    environment.app_root = find_app_root(title_id);
    const std::string assets = environment.app_root + "/assets";
    sys::log("[PShare] app root %s", environment.app_root.c_str());

    ps5::Display display;
    if (!display.open(1920, 1080))
    {
        sys::log("[PShare] fatal: display open failed");
        sys::park();
    }
    gfx::Renderer renderer;
    gfx::Font regular, semibold, display_face, mono, pixel, hand;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold,
                   &fonts.semibold) ||
        !load_font(renderer, assets + "/fonts/montserrat-medium.huifont", &display_face,
                   &fonts.display) ||
        !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, assets + "/fonts/press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, assets + "/fonts/patrick-hand.huifont", &hand, &fonts.hand))
    {
        sys::log("[PShare] fatal: renderer or fonts failed");
        sys::park();
    }
    app::Art art;
    art.backdrop = load_art(renderer, assets + "/art/backdrop.jpg", nullptr);
    art.splash = load_art(renderer, assets + "/art/splash.jpg", nullptr);
    art.logo = load_art(renderer, assets + "/art/logo.png", nullptr);
    art.empty = load_art(renderer, assets + "/art/empty.png", &art.empty_aspect);

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(assets + "/audio/sfx");
    sys::log("[PShare] sounds files=%d rejected=%d", bank.files, bank.rejected);

    app::Model model(environment);
    model.start();
    app::Thumbs thumbs(environment.elevated ? environment.paths.thumbs() : std::string());
    // Clips get a frame from the middle of the recording as their tile.
    thumbs.set_video_decoder(&app::video_thumbnail);
    thumbs.start();
    // The clip player: FFmpeg decodes on its own thread, the sound goes out
    // through mixer slot 0 next to the interface sounds.
    app::VideoPlayer player(&mixer, 0);
    app::App application(model, thumbs, fonts, art, &player);

    ui::Feedback feedback;
    PadSample samples[64];
    std::int64_t last = sys::monotonic_us();
    std::uint64_t frames = 0;
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                                static_cast<std::uint64_t>(now));
        feedback.clear();
        thumbs.pump(renderer.batch(), 3);
        application.update(input, dt, feedback);
        for (const audio::CueEvent &event : feedback.cues)
            sounds.play(mixer,
                        event.set == audio::SoundSet::count ? audio::SoundSet::glass : event.set,
                        event);
        if (feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        pad.set_light_bar(0xff, 0x5c, 0x8a); // coral, the app's accent

        application.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[PShare] fatal: swap failed frame=%llu",
                     static_cast<unsigned long long>(frames));
            sys::park();
        }
        if (++frames == 1)
            sys::log("[PShare] first frame, splash hidden=%d", sys::hide_splash_screen() ? 1 : 0);
        if (application.wants_quit())
        {
            // pshared, when it runs, is a process of its own and keeps going.
            sys::log("[PShare] closing");
            player.close();
            pad.close();
            sys::quit();
        }
    }
}
