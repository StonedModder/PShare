// PShare - Thumbnails of the captures, decoded off the frame.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A 4K screenshot is a 2-3 MB JPEG that takes a few hundred milliseconds to
// decode, so nothing is decoded on the frame: get() asks for a picture and
// returns 0 until a worker has decoded and shrunk it; pump() then uploads a
// few finished pictures per frame. Requests are served newest first, so what
// the player is looking at loads before what they scrolled past.
//
// Shrunk thumbnails are kept as small JPEGs under /data/pshare/thumbs, and
// the next launch reads those instead of the originals. Clips have no frame
// get a picture from their first second through the video decoder the app
// installs (set_video_decoder); without one (the unit tests) a clip has none
// and the cell falls back to the game's icon.

#pragma once

#include "app/thread.hpp"
#include "gfx/gl_batch.hpp"

#include <cstdint>
#include <deque>
#include <list>
#include "pshare/core/picture.hpp"

#include <string>
#include <map>
#include <vector>

namespace pshare::app
{

class Thumbs
{
  public:
    struct Texture
    {
        std::uint32_t id = 0;
        float aspect = 16.0f / 9.0f;
        bool failed = false; // could not be decoded: draw the placeholder for good
    };

    Thumbs(std::string cache_dir, int width = 480, int height = 270, std::size_t capacity = 160);
    ~Thumbs();
    Thumbs(const Thumbs &) = delete;
    Thumbs &operator=(const Thumbs &) = delete;

    using VideoDecoder = bool (*)(const std::string &path, int max_width, int max_height,
                                  Picture *picture);
    // Before start().
    void set_video_decoder(VideoDecoder decoder)
    {
        video_decoder_ = decoder;
    }
    void start();
    // The thumbnail of `source` (a capture, or a game icon). id 0 while it loads.
    Texture get(const std::string &source);
    // A large picture for the viewer, up to 1920 x 1080; one is kept at a time.
    Texture full(const std::string &source);
    // Uploads up to `budget` decoded pictures. Main thread, once per frame.
    void pump(hui::gfx::GlBatch &batch, int budget = 3);
    std::size_t loaded() const
    {
        return textures_.size();
    }

  private:
    struct Request
    {
        std::string source;
        bool full = false;
    };
    struct Done
    {
        std::string source;
        bool full = false;
        Picture picture;
        bool ok = false;
    };
    void loop();
    std::string cache_path(const std::string &source) const;
    void touch(const std::string &source);

    VideoDecoder video_decoder_ = nullptr;
    std::string cache_dir_;
    int width_;
    int height_;
    std::size_t capacity_;

    // main thread
    std::map<std::string, Texture> textures_;
    std::list<std::string> order_; // least recently used first
    std::map<std::string, std::list<std::string>::iterator> where_;
    std::map<std::string, bool> pending_;
    std::string full_source_;
    Texture full_texture_;
    bool full_pending_ = false;

    // shared
    Mutex mutex_;
    Condition wake_;
    std::deque<Request> requests_;
    std::deque<Done> done_;
    bool quit_ = false;
    Thread thread_;
};

} // namespace pshare::app
