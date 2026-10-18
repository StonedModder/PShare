// PShare - Headless PC preview: the real app over a pretend console.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: pshare_preview <assets dir> <output dir> [width height]
//
// Builds a small fake console under <output dir>/.console (invented games,
// cartoon "screenshots", a few clips, a history and a configuration), runs
// the same App, Model and Thumbs as the PS5 build in demo mode (uploads go to
// a fake transport, the background uploader is simulated), plays a scripted
// walk through every page with a 60 Hz clock and writes a PNG at each named
// step. Rendering is Mesa's surfaceless EGL, as in ps5-homebrew-ui.

#include "app/app.hpp"
#include "app/thumbs.hpp"
#include "app/video.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/save_file.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "pshare/core/config.hpp"
#include "pshare/core/history.hpp"
#include "ui/fonts.hpp"

#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image/stb_image_write.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
using namespace pshare;
using hui::Action;
using hui::Direction;
using hui::InputFrame;

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
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
        return 0;
    if (aspect != nullptr)
        *aspect = static_cast<float>(picture.width) / picture.height;
    return renderer.batch().create_texture(picture.width, picture.height, picture.rgba.data());
}

// ---- the pretend console -----------------------------------------------

struct Rgb
{
    float r, g, b;
};

Rgb mix(Rgb a, Rgb b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

Rgb hex(std::uint32_t value)
{
    return {static_cast<float>((value >> 16) & 255), static_cast<float>((value >> 8) & 255),
            static_cast<float>(value & 255)};
}

// A cartoon "game screenshot": a sky, a sun, layered hills, a few shapes.
// Nothing realistic, nothing anyone owns.
Picture cartoon(int seed, int width, int height)
{
    const Rgb skies[][2] = {{hex(0x2b1b6b), hex(0xff8a7a)}, {hex(0x0f3b5f), hex(0x6fe3d6)},
                            {hex(0x40135a), hex(0xffc35e)}, {hex(0x14284f), hex(0xa58bff)},
                            {hex(0x0d1f2d), hex(0x58d68d)}, {hex(0x5a1036), hex(0xffd3a1)}};
    const Rgb hills[][3] = {{hex(0x3d2a8f), hex(0x6b3fb8), hex(0x9b59d0)},
                            {hex(0x0e5a6b), hex(0x168b8b), hex(0x22b5a3)},
                            {hex(0x6b1d4a), hex(0xa22f63), hex(0xe2577b)},
                            {hex(0x232a6b), hex(0x3a46a8), hex(0x5a6ee0)},
                            {hex(0x14432f), hex(0x1f6e47), hex(0x2e9c5f)},
                            {hex(0x6b2a14), hex(0xa8461f), hex(0xe07b3a)}};
    const int k = seed % 6;
    Picture picture;
    picture.width = width;
    picture.height = height;
    picture.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const float sun_x = width * (0.25f + 0.5f * static_cast<float>((seed * 37) % 100) / 100.0f);
    const float sun_y = height * 0.32f;
    const float sun_r = height * 0.11f;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float fy = static_cast<float>(y) / height;
            const float fx = static_cast<float>(x) / width;
            Rgb c = mix(skies[k][0], skies[k][1], fy * 1.1f > 1.0f ? 1.0f : fy * 1.1f);
            const float dx = x - sun_x;
            const float dy = y - sun_y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < sun_r)
                c = hex(0xffe08a);
            else if (d < sun_r * 1.6f)
                c = mix(c, hex(0xffe08a), 0.25f * (1.0f - (d - sun_r) / (sun_r * 0.6f)));
            for (int layer = 0; layer < 3; ++layer)
            {
                const float phase = seed * 1.7f + layer * 2.1f;
                const float top = 0.55f + layer * 0.12f +
                                  0.08f * std::sin(fx * (3.0f + layer * 2.0f) + phase) +
                                  0.03f * std::sin(fx * 13.0f + phase * 2.0f);
                if (fy > top)
                    c = hills[k][layer];
            }
            // A little hero: a rounded block with eyes, standing on the front hill.
            const float hx = width * 0.62f;
            const float hy = height * 0.74f;
            if (std::fabs(x - hx) < width * 0.03f && y > hy - height * 0.12f && y < hy)
                c = hex(0xfff4e6);
            if ((std::fabs(x - (hx - width * 0.01f)) < width * 0.004f ||
                 std::fabs(x - (hx + width * 0.012f)) < width * 0.004f) &&
                std::fabs(y - (hy - height * 0.085f)) < height * 0.012f)
                c = hex(0x15123a);
            std::uint8_t *p = picture.rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            p[0] = static_cast<std::uint8_t>(c.r);
            p[1] = static_cast<std::uint8_t>(c.g);
            p[2] = static_cast<std::uint8_t>(c.b);
            p[3] = 255;
        }
    }
    return picture;
}

// A game icon: a disc of colour with a simple glyph.
Picture game_icon(std::uint32_t a, std::uint32_t b, int glyph)
{
    const int size = 256;
    Picture picture;
    picture.width = picture.height = size;
    picture.rgba.resize(static_cast<std::size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            Rgb c = mix(hex(a), hex(b), static_cast<float>(x + y) / (2.0f * size));
            const float dx = x - size * 0.5f;
            const float dy = y - size * 0.5f;
            const float d = std::sqrt(dx * dx + dy * dy);
            bool ink = false;
            if (glyph == 0)
                ink = d < 70 && d > 44; // a ring
            else if (glyph == 1)
                ink = std::fabs(dx) + std::fabs(dy) < 74; // a diamond
            else
                ink = std::fabs(dx) < 70 && std::fabs(dy) < 18; // a bar
            if (ink)
                c = hex(0xfff4e6);
            std::uint8_t *p = picture.rgba.data() + (static_cast<std::size_t>(y) * size + x) * 4;
            p[0] = static_cast<std::uint8_t>(c.r);
            p[1] = static_cast<std::uint8_t>(c.g);
            p[2] = static_cast<std::uint8_t>(c.b);
            p[3] = 255;
        }
    return picture;
}

void write(const std::string &path, const std::string &bytes)
{
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return;
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
}

void set_mtime(const std::string &path, std::int64_t when)
{
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = static_cast<time_t>(when);
    times[0].tv_nsec = times[1].tv_nsec = 0;
    utimensat(0, path.c_str(), times, 0);
}

std::string stamp(std::int64_t when)
{
    const std::time_t value = static_cast<std::time_t>(when);
    std::tm parts{};
    localtime_r(&value, &parts);
    char text[32];
    std::snprintf(text, sizeof(text), "%04d%02d%02d_%02d%02d%02d", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return text;
}

Paths build_console(const std::string &root)
{
    Paths paths;
    paths.data_root = root + "/data/pshare";
    paths.photo_root = root + "/user/av_contents/photo";
    paths.video_root = root + "/user/av_contents/video";
    paths.appmeta_root = root + "/user/appmeta";
    for (const std::string &dir :
         {paths.data_root, paths.photo_root, paths.video_root, paths.appmeta_root})
        pshare::ensure_directory(dir);

    struct Game
    {
        const char *id;
        const char *name;
        std::uint32_t a, b;
        int glyph;
        bool ps4;
    };
    const Game games[] = {{"PPSA01111", "Nebula Drift", 0x6b4ce6, 0xff5c8a, 0, false},
                          {"PPSA02222", "Pocket Knights", 0x1f8b8b, 0x3ee6e0, 1, false},
                          {"CUSA03333", "Lumen Racer", 0xff8a3d, 0xffd25e, 2, true}};
    for (const Game &game : games)
    {
        const std::string dir = paths.appmeta_root + "/" + game.id;
        pshare::ensure_directory(dir);
        if (game.ps4)
        {
            // A minimal param.sfo with one TITLE key.
            std::string sfo(20, '\0');
            sfo[0] = 0;
            write(
                dir + "/param.json",
                std::string(
                    R"({"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":")") +
                    game.name + "\"}}}");
        }
        else
        {
            write(
                dir + "/param.json",
                std::string(
                    R"({"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":")") +
                    game.name + "\"}}}");
        }
        write(dir + "/icon0.png", encode_jpeg(game_icon(game.a, game.b, game.glyph), 90));
    }

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    std::vector<std::string> shots;
    for (int i = 0; i < 14; ++i)
    {
        const Game &game = games[i % 3];
        const std::int64_t when = now - 3600 * (i * 5 + 1) - i * 517;
        const std::string dir =
            paths.photo_root + "/" + (i % 3 == 2 ? "PS4" : "PS5") + "/" + game.id + "/" + game.id;
        pshare::ensure_directory(paths.photo_root + "/" + (i % 3 == 2 ? "PS4" : "PS5"));
        pshare::ensure_directory(paths.photo_root + "/" + (i % 3 == 2 ? "PS4" : "PS5") + "/" +
                                 game.id);
        pshare::ensure_directory(dir);
        const std::string path =
            dir + "/" + stamp(when) + "_0000000" + std::to_string(i % 10) + ".jpg";
        write(path, encode_jpeg(cartoon(i, 1280, 720), 88));
        set_mtime(path, when);
        shots.push_back(path);
    }
    // A real (made-up) clip when tools/host-preview.sh could make one with the
    // ffmpeg command: the newest capture, so it is the first tile.
    std::string sample;
    if (const char *clip = std::getenv("PSHARE_SAMPLE_CLIP"))
        pshare::read_text_file(clip, &sample, 64u << 20);
    for (int i = 0; i < 4; ++i)
    {
        const Game &game = games[(i + 1) % 3];
        const std::int64_t when = i == 0 && !sample.empty() ? now - 600 : now - 3600 * (i * 9 + 3);
        const std::string base = paths.video_root + "/PS5/" + game.id;
        pshare::ensure_directory(paths.video_root + "/PS5");
        pshare::ensure_directory(base);
        const std::string path =
            base + "/" + stamp(when) + "_c000000" + std::to_string(i) + ".webm";
        if (i == 0 && !sample.empty())
            write(path, sample);
        else
            write(path, std::string(static_cast<std::size_t>(3'500'000 + i * 1'250'000), 'x'));
        set_mtime(path, when);
    }

    // PSHARE_PREVIEW_BLANK: one more clip that reads back as zeros, for the
    // "can't be read" prompt.
    if (std::getenv("PSHARE_PREVIEW_BLANK") != nullptr)
    {
        const std::string base = paths.video_root + "/PS5/" + games[0].id;
        pshare::ensure_directory(base);
        const std::string path = base + "/" + stamp(now - 90000) + "_c0000009.webm";
        write(path, std::string(4'000'000, '\0'));
        set_mtime(path, now - 90000);
    }

    IniDocument document;
    document.parse(default_config_text());
    Config config;
    std::vector<std::string> warnings;
    load_config(document, &config, &warnings);
    if (Destination *discord = config.find("discord"))
        discord->url = "https://discord.com/api/webhooks/1234567890/preview-only-not-a-real-hook";
    store_config(config, &document);
    write(paths.config(), document.write());

    History history(paths.history());
    const char *destinations[] = {"discord", "discord", "catbox", "discord", "telegram"};
    for (int i = 0; i < 9; ++i)
    {
        HistoryEntry entry;
        entry.time = now - 3600 * (i * 5 + 1) - i * 517 + 40;
        entry.ok = i != 4;
        entry.automatic = i % 3 != 1;
        entry.destination = destinations[i % 5];
        entry.path = shots[static_cast<std::size_t>(i + 1)];
        entry.detail = entry.ok ? "https://cdn.discordapp.com/attachments/1/2/capture-" +
                                      std::to_string(i) + ".jpg"
                                : "Telegram answered 401: Unauthorized";
        history.append(entry);
    }
    return paths;
}

InputFrame press(Action action)
{
    InputFrame frame;
    frame.connected = true;
    frame.pressed = hui::action_bit(action);
    frame.held = frame.pressed;
    return frame;
}

InputFrame move(Direction direction)
{
    InputFrame frame;
    frame.connected = true;
    frame.nav = direction;
    return frame;
}

struct Step
{
    std::vector<InputFrame> inputs; // one per frame, then the wait
    float wait = 0.6f;
    const char *capture = nullptr;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <assets dir> <output dir> [width height]\n", argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    const int width = argc > 4 ? std::atoi(argv[3]) : 1920;
    const int height = argc > 4 ? std::atoi(argv[4]) : 1080;
    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular, semibold, display, mono, pixel, hand;
    hui::ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold,
                   &fonts.semibold) ||
        !load_font(renderer, assets + "/fonts/montserrat-medium.huifont", &display,
                   &fonts.display) ||
        !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, assets + "/fonts/press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, assets + "/fonts/patrick-hand.huifont", &hand, &fonts.hand))
        return 1;

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;

    pshare::ensure_directory(output);
    const std::string console = output + "/.console";
    std::system(("rm -rf '" + console + "'").c_str());
    app::Environment environment;
    environment.paths = build_console(console);
    environment.app_root = console + "/app";
    environment.demo = true;

    app::Art art;
    art.backdrop = load_art(renderer, assets + "/art/backdrop.jpg", nullptr);
    art.splash = load_art(renderer, assets + "/art/splash.jpg", nullptr);
    art.logo = load_art(renderer, assets + "/art/logo.png", nullptr);
    art.empty = load_art(renderer, assets + "/art/empty.png", &art.empty_aspect);

    app::Model model(environment);
    model.start();
    app::Thumbs thumbs(environment.paths.thumbs());
    thumbs.set_video_decoder(&app::video_thumbnail);
    thumbs.start();
    // The console's audio thread, without the speakers: it pulls the mix in time.
    hui::audio::Mixer mixer;
    std::atomic<bool> audio_quit{false};
    std::thread audio_thread(
        [&]
        {
            std::vector<std::int16_t> grain(1024 * 2);
            while (!audio_quit.load())
            {
                mixer.render(grain.data(), 1024);
                usleep(1024 * 1000000 / 48000);
            }
        });
    app::VideoPlayer player(&mixer, 0);
    app::App application(model, thumbs, fonts, art, &player);

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    std::vector<unsigned char> flipped(pixels.size());
    bool ok = true;
    const auto render = [&](const std::string &name)
    {
        application.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const std::size_t row = static_cast<std::size_t>(width) * 4;
        for (int y = 0; y < height; ++y)
            std::copy(pixels.begin() + static_cast<std::ptrdiff_t>(row * (height - 1 - y)),
                      pixels.begin() + static_cast<std::ptrdiff_t>(row * (height - y)),
                      flipped.begin() + static_cast<std::ptrdiff_t>(row * y));
        for (std::size_t i = 3; i < flipped.size(); i += 4)
            flipped[i] = 255;
        std::string png;
        stbi_write_png_to_func(
            [](void *context, void *data, int size)
            { static_cast<std::string *>(context)->append(static_cast<char *>(data), size); }, &png,
            width, height, 4, flipped.data(), width * 4);
        const std::string path = output + "/" + name + ".png";
        write(path, png);
        ok = !png.empty() && ok;
        if (player.texture() != 0 && player.is_open())
            std::fprintf(stderr, "  clip at %.2f of %.2f s, underruns %llu\n", player.position(),
                         player.duration(),
                         static_cast<unsigned long long>(mixer.stream_underruns()));
        std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n", path.c_str(),
                     renderer.last_instances(), renderer.last_draw_calls(), glGetError());
    };

    if (const char *record = std::getenv("PSHARE_RECORD"))
    {
        // Promo footage: a scripted walk through the app, every frame in
        // simulated time (60 per second), the mixer pulled in step with it so
        // the clip player and the interface sounds land where they should.
        // Writes <dir>/footage.mp4 (30 fps), <dir>/footage.wav and the
        // markers (<name> <seconds>) in <dir>/markers.txt.
        audio_quit = true;
        audio_thread.join();
        const std::string dir = record;
        pshare::ensure_directory(dir);
        hui::audio::SoundBank sounds;
        sounds.load(assets + "/audio/sfx");
        const std::string command = "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s " +
                                    std::to_string(width) + "x" + std::to_string(height) +
                                    " -r 30 -i - -vf vflip -c:v libx264 -preset fast -crf 14 "
                                    "-pix_fmt yuv420p '" +
                                    dir + "/footage.mp4'";
        FILE *video = popen(command.c_str(), "w");
        if (video == nullptr)
            return 1;
        std::vector<std::int16_t> audio;
        std::FILE *marks = std::fopen((dir + "/markers.txt").c_str(), "w");
        constexpr float kStep = 1.0f / 60.0f;
        long frame = 0;
        hui::ui::Feedback cues;
        InputFrame still;
        still.connected = true;
        const auto tick = [&](const InputFrame &input)
        {
            cues.clear();
            thumbs.pump(renderer.batch(), 8);
            application.update(input, kStep, cues);
            for (const hui::audio::CueEvent &event : cues.cues)
                sounds.play(mixer,
                            event.set == hui::audio::SoundSet::count ? hui::audio::SoundSet::glass
                                                                     : event.set,
                            event);
            application.compose(renderer);
            std::int16_t grain[800 * 2];
            mixer.render(grain, 800);
            audio.insert(audio.end(), grain, grain + 1600);
            if (frame % 2 == 0)
            {
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                renderer.present(framebuffer, width, height);
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                std::fwrite(pixels.data(), 1, pixels.size(), video);
            }
            ++frame;
            // The workers (decoder, thumbnails, fake uploads) run in real
            // time; a little room keeps them ahead of the recording.
            usleep(2000);
        };
        const auto hold = [&](float seconds)
        {
            for (int i = 0; i < static_cast<int>(seconds * 60.0f); ++i)
                tick(still);
        };
        const auto mark = [&](const char *name)
        {
            std::fprintf(marks, "%s %.3f\n", name, frame / 60.0);
            std::fprintf(stderr, "  %s at %.2f s\n", name, frame / 60.0);
        };
        const auto tap = [&](const InputFrame &input, float after)
        {
            tick(input);
            hold(after);
        };
        mark("intro");
        hold(3.4f);
        mark("grid");
        hold(1.0f);
        tap(move(Direction::right), 0.45f);
        tap(move(Direction::right), 0.45f);
        tap(move(Direction::down), 0.45f);
        tap(move(Direction::left), 0.45f);
        tap(move(Direction::up), 0.45f);
        tap(move(Direction::left), 0.6f);
        mark("details");
        tap(press(Action::confirm), 1.4f);
        mark("play");
        tap(press(Action::confirm), 5.5f);
        tap(move(Direction::right), 1.6f);
        mark("play_end");
        tap(press(Action::back), 0.8f);
        tap(move(Direction::right), 0.5f);
        mark("send");
        tap(press(Action::west), 3.2f);
        mark("sendto");
        tap(press(Action::north), 1.8f);
        tap(press(Action::back), 0.6f);
        mark("auto");
        tap(press(Action::page_next), 2.4f);
        for (int i = 0; i < 6; ++i)
            tap(move(Direction::down), 0.12f);
        mark("gallery");
        tap(press(Action::confirm), 2.6f);
        tap(press(Action::back), 0.6f);
        mark("destinations");
        tap(press(Action::page_next), 1.4f);
        tap(press(Action::confirm), 0.8f);
        tap(move(Direction::down), 0.3f);
        tap(move(Direction::down), 0.9f);
        tap(press(Action::back), 0.4f);
        tap(press(Action::back), 0.3f);
        mark("history");
        tap(press(Action::page_next), 1.0f);
        tap(press(Action::confirm), 0.4f); // from the rail into the list
        mark("history_qr");
        tap(press(Action::confirm), 2.8f);
        tap(press(Action::back), 0.6f);
        mark("end");
        pclose(video);
        std::fclose(marks);
        // 48 kHz stereo 16-bit WAV.
        std::string wav;
        const auto u32 = [&](std::uint32_t v)
        { wav.append(reinterpret_cast<const char *>(&v), 4); };
        const auto u16 = [&](std::uint16_t v)
        { wav.append(reinterpret_cast<const char *>(&v), 2); };
        const std::uint32_t bytes = static_cast<std::uint32_t>(audio.size() * 2);
        wav += "RIFF";
        u32(36 + bytes);
        wav += "WAVEfmt ";
        u32(16);
        u16(1);
        u16(2);
        u32(48000);
        u32(48000 * 4);
        u16(4);
        u16(16);
        wav += "data";
        u32(bytes);
        wav.append(reinterpret_cast<const char *>(audio.data()), bytes);
        write(dir + "/footage.wav", wav);
        player.close();
        return 0;
    }

    if (std::getenv("PSHARE_SHAPES") != nullptr)
    {
        // A sheet of every icon and a few turned triangles, for checking the drawing.
        hui::gfx::DrawList list;
        const float angles[] = {0.0f, 0.5f, 1.5708f, 3.1416f, -0.5f};
        for (int i = 0; i < 5; ++i)
            list.triangle({100.0f + i * 200.0f, 100.0f, 120.0f, 120.0f}, app::rgb(0xffffff), 0.0f,
                          angles[i]);
        for (int i = 0; i < 15; ++i)
            app::draw_icon(list, static_cast<app::Icon>(i),
                           {100.0f + (i % 8) * 200.0f, 360.0f + (i / 8) * 200.0f, 120.0f, 120.0f},
                           app::rgb(0xffffff));
        app::draw_mark(list, {100.0f, 800.0f, 200.0f, 200.0f});
        renderer.begin();
        renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0.1f, 0.1f, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const std::size_t row = static_cast<std::size_t>(width) * 4;
        for (int y = 0; y < height; ++y)
            std::copy(pixels.begin() + static_cast<std::ptrdiff_t>(row * (height - 1 - y)),
                      pixels.begin() + static_cast<std::ptrdiff_t>(row * (height - y)),
                      flipped.begin() + static_cast<std::ptrdiff_t>(row * y));
        std::string png;
        stbi_write_png_to_func(
            [](void *context, void *data, int size)
            { static_cast<std::string *>(context)->append(static_cast<char *>(data), size); }, &png,
            width, height, 4, flipped.data(), width * 4);
        write(output + "/shapes.png", png);
        return 0;
    }

    constexpr float kDt = 1.0f / 60.0f;
    hui::ui::Feedback feedback;
    InputFrame idle;
    idle.connected = true;
    // Wall-clock waits too: the workers (scan, thumbnails, fake uploads) run for real.
    const auto run = [&](const InputFrame &input, int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            feedback.clear();
            thumbs.pump(renderer.batch(), 8);
            application.update(i == 0 ? input : idle, kDt, feedback);
            // The video's frame is uploaded where the console does it: compose.
            if (player.texture() != 0 || player.loading())
                application.compose(renderer);
            usleep(player.loading() || player.texture() != 0 ? 1000 : 4000);
        }
    };

    const std::vector<Step> steps = {
        {{}, 0.5f, "00-intro"},
        {{}, 2.6f, "01-captures"},
        {{press(Action::confirm)}, 0.9f, "01a-clip-details"},
        {{press(Action::confirm)}, 1.6f, "01b-clip-playing"},
        {{}, 2.6f, "01c-clip-playing-later"},
        {{press(Action::back)}, 0.6f, "01d-clip-closed"},
        {{press(Action::touch)}, 0.7f, "01e-filter-sheet"},
        {{move(Direction::down)}, 0.3f, nullptr},
        {{press(Action::confirm)}, 1.0f, "01f-one-game"},
        {{press(Action::touch), move(Direction::up), move(Direction::up)}, 0.4f, nullptr},
        {{press(Action::confirm)}, 1.0f, "01g-oldest-first"},
        {{press(Action::touch), move(Direction::up)}, 0.3f, nullptr},
        {{press(Action::confirm)}, 0.3f, nullptr},
        {{press(Action::touch), move(Direction::up), move(Direction::up)}, 0.3f, nullptr},
        {{press(Action::confirm)}, 0.8f, "01h-back-to-all"},
        {{move(Direction::right)}, 0.5f, "02-captures-focus"},
        {{press(Action::confirm)}, 0.9f, "03-capture-details"},
        {{press(Action::confirm)}, 1.1f, "04-sending"},
        {{press(Action::north)}, 0.6f, "05-send-to"},
        {{press(Action::back)}, 2.4f, nullptr},
        {{press(Action::page_next)}, 1.2f, "06-auto-upload"},
        {{}, 0.5f, nullptr},
        {{move(Direction::down), move(Direction::down), move(Direction::down),
          move(Direction::down), move(Direction::down)},
         0.4f,
         nullptr},
        {{press(Action::confirm)}, 1.6f, "07-phone-setup"},
        {{press(Action::back)}, 0.6f, nullptr},
        {{move(Direction::down)}, 0.3f, nullptr},
        {{press(Action::confirm)}, 1.6f, "07b-phone-gallery"},
        {{press(Action::back)}, 0.6f, nullptr},
        {{press(Action::page_next)}, 1.0f, "08-destinations"},
        {{press(Action::confirm)}, 0.6f, "09-destination-form"},
        {{move(Direction::down), move(Direction::down), move(Direction::down)},
         0.5f,
         "10-destination-field"},
        {{press(Action::confirm)}, 0.8f, "11-type-a-value"},
        {{press(Action::back)}, 0.5f, nullptr},
        {{press(Action::back)}, 0.3f, nullptr},
        {{press(Action::page_next)}, 1.0f, "12-history"},
        {{press(Action::confirm)}, 0.9f, "13-history-link"},
        {{press(Action::back)}, 0.4f, nullptr},
        {{press(Action::page_next)}, 1.0f, "14-settings"},
        {std::vector<InputFrame>(14, move(Direction::down)), 0.8f, "14b-settings-controls"},
        {{press(Action::back)}, 0.3f, nullptr},
        {{press(Action::menu)}, 0.6f, "15-menu"},
        {{press(Action::back)}, 0.4f, nullptr},
        {{press(Action::back)}, 0.6f, "16-rail"},
    };
    const auto started = std::chrono::steady_clock::now();
    for (const Step &step : steps)
    {
        std::fprintf(
            stderr, "  [%.1f s]\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        for (const InputFrame &input : step.inputs)
            run(input, 1);
        run(idle, static_cast<int>(step.wait / kDt));
        if (step.capture != nullptr)
            render(step.capture);
    }
    player.close();
    audio_quit = true;
    audio_thread.join();
    return ok ? 0 : 1;
}
