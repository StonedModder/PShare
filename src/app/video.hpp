// PShare - Playing a clip inside the app, picture and sound.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 saves clips as WebM (VP9 and Opus) or MP4 (H.264 or HEVC and AAC).
// FFmpeg (PacBrew's static libavformat/libavcodec) demuxes and decodes them
// on a thread of the player's own; frames are converted to RGBA at up to
// 1280 x 720 and handed to the main thread, which uploads one per frame into
// a single texture. The sound is resampled to 48 kHz stereo floats and fed to
// the kit's mixer through a StreamRing, and it is the clock: a frame is shown
// when the sound has reached its time. Without a mixer (the PC preview) the
// clock is the wall clock.
//
// The decode thread keeps separate packet queues for picture and sound, so a
// file whose clusters interleave coarsely can never starve one waiting for
// the other.

#pragma once

#include "app/thread.hpp"
#include "app/thumbs.hpp"
#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"
#include "gfx/gl_batch.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace pshare::app
{

// One picture from a clip (about a third of the way in, at most 1 s), shrunk
// to fit max_width x max_height. False when the file cannot be decoded.
bool video_thumbnail(const std::string &path, int max_width, int max_height, Picture *picture);
// The clip's length in seconds, 0 when unknown.
double video_duration(const std::string &path);

class VideoPlayer
{
  public:
    // mixer may be null: the sound is then decoded and dropped.
    explicit VideoPlayer(hui::audio::Mixer *mixer, std::size_t stream_slot = 0);
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    // Starts playing `path` (from the start, or from `at` seconds).
    void open(const std::string &path, double at = 0.0);
    void close();
    bool is_open() const
    {
        return open_;
    }
    const std::string &path() const
    {
        return path_;
    }

    void set_paused(bool paused);
    bool paused() const
    {
        return paused_;
    }
    // Absolute, in seconds; clamped to the clip.
    void seek(double seconds);
    void set_volume(float volume); // 0..1

    // Main thread, once per frame: advances the clock and uploads the frame due.
    void update(float dt, hui::gfx::GlBatch &batch);

    double position() const
    {
        return position_;
    }
    double duration() const;
    bool loading() const
    {
        return open_ && texture_ready_ == false && error().empty();
    }
    bool ended() const;
    bool has_audio() const;
    std::string error() const;
    std::uint32_t texture() const
    {
        return texture_;
    }
    float aspect() const
    {
        return aspect_;
    }
    // "1920 x 1080 VP9" for the info line.
    std::string description() const;

  private:
    struct Frame
    {
        double pts = 0.0;
        std::vector<std::uint8_t> rgba;
    };
    void run(std::string path, double at, std::uint64_t generation);
    double audio_clock() const;
    void stop_thread();

    hui::audio::Mixer *mixer_;
    std::size_t slot_;
    std::string path_;
    bool open_ = false;
    bool paused_ = false;
    bool texture_ready_ = false;
    double position_ = 0.0;
    float volume_ = 1.0f;
    std::uint32_t texture_ = 0; // the frame on screen
    std::uint32_t retired_ = 0; // the one before it, deleted with the next frame
    // The audio clock's last value and how long it has not moved: when the
    // sound output stops pulling (or never starts), the picture switches to
    // the wall clock instead of freezing.
    double last_audio_ = -1.0;
    float audio_still_ = 0.0f;
    bool audio_clock_dead_ = false;
    float log_timer_ = 0.0f;
    float aspect_ = 16.0f / 9.0f;

    // shared with the decode thread
    mutable Mutex mutex_;
    std::deque<Frame> frames_;
    std::vector<std::vector<std::uint8_t>> pool_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> seek_pending_{false};
    double seek_target_ = 0.0;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<double> clock_{0.0}; // what the screen shows now, for the decoder's late check
    double duration_ = 0.0;
    bool has_audio_ = false;
    bool eof_ = false;
    std::string error_;
    std::string description_;
    int width_ = 0;
    int height_ = 0;

    // Sound: a fresh ring after every seek; old ones are kept until the mixer
    // has surely let go of them.
    std::deque<std::unique_ptr<hui::audio::StreamRing>> rings_;
    hui::audio::StreamRing *ring_ = nullptr;
    std::atomic<std::uint64_t> audio_written_{0}; // frames written to ring_
    std::atomic<double> audio_start_{-1.0};       // the time of the first of them
    double wall_clock_ = 0.0;

    Thread thread_;
};

} // namespace pshare::app
