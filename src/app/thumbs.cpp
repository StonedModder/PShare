// PShare - Thumbnails of the captures, decoded off the frame.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/thumbs.hpp"

#include "pshare/core/config.hpp"
#include "pshare/core/media.hpp"
#include "pshare/core/text.hpp"

#define STBI_NO_STDIO
#include "third_party/stb_image/stb_image.h"
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image/stb_image_write.h"

#include <GL/glcorearb.h>

#include <algorithm>
#include <cstdio>

namespace pshare::app
{
namespace
{
constexpr std::size_t kMaxRequests = 96; // older requests are dropped: they scrolled away
constexpr std::size_t kMaxFile = 48u << 20;

void append_bytes(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data),
                                                static_cast<std::size_t>(size));
}
} // namespace

Picture shrink(const Picture &source, int max_width, int max_height)
{
    if (source.width <= 0 || source.height <= 0)
        return {};
    const float scale = std::min({1.0f, static_cast<float>(max_width) / source.width,
                                  static_cast<float>(max_height) / source.height});
    Picture out;
    out.width = std::max(1, static_cast<int>(source.width * scale + 0.5f));
    out.height = std::max(1, static_cast<int>(source.height * scale + 0.5f));
    if (out.width == source.width && out.height == source.height)
        return source;
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    // Each output pixel averages the source pixels it covers: no aliasing
    // when a 3840-wide shot becomes 480 wide.
    for (int y = 0; y < out.height; ++y)
    {
        const int y0 = y * source.height / out.height;
        const int y1 = std::max(y0 + 1, (y + 1) * source.height / out.height);
        for (int x = 0; x < out.width; ++x)
        {
            const int x0 = x * source.width / out.width;
            const int x1 = std::max(x0 + 1, (x + 1) * source.width / out.width);
            std::uint32_t sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
            {
                const std::uint8_t *row =
                    source.rgba.data() + (static_cast<std::size_t>(sy) * source.width + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += row[c];
            }
            const std::uint32_t count = static_cast<std::uint32_t>((y1 - y0) * (x1 - x0));
            std::uint8_t *pixel =
                out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 4; ++c)
                pixel[c] = static_cast<std::uint8_t>(sum[c] / count);
        }
    }
    return out;
}

bool decode_picture_bytes(const std::string &bytes, int max_width, int max_height, Picture *picture)
{
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(0x7fffffff))
        return false;
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                              static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
        return false;
    Picture full;
    full.width = width;
    full.height = height;
    full.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    *picture = shrink(full, max_width, max_height);
    return picture->width > 0;
}

bool decode_picture(const std::string &path, int max_width, int max_height, Picture *picture)
{
    std::string bytes;
    if (!read_text_file(path, &bytes, kMaxFile))
        return false;
    return decode_picture_bytes(bytes, max_width, max_height, picture);
}

std::string encode_jpeg(const Picture &picture, int quality)
{
    std::string out;
    if (picture.width <= 0 || picture.height <= 0)
        return out;
    if (stbi_write_jpg_to_func(append_bytes, &out, picture.width, picture.height, 4,
                               picture.rgba.data(), quality) == 0)
        out.clear();
    return out;
}

Thumbs::Thumbs(std::string cache_dir, int width, int height, std::size_t capacity)
    : cache_dir_(std::move(cache_dir)), width_(width), height_(height), capacity_(capacity)
{
}

Thumbs::~Thumbs()
{
    {
        Lock lock(mutex_);
        quit_ = true;
        wake_.notify_all();
    }
    thread_.join();
}

void Thumbs::start()
{
    if (!cache_dir_.empty())
        ensure_directory(cache_dir_);
    thread_.start([this] { loop(); }, 1u << 20);
}

std::string Thumbs::cache_path(const std::string &source) const
{
    if (cache_dir_.empty())
        return {};
    char name[32];
    std::snprintf(name, sizeof(name), "/%016llx.jpg",
                  static_cast<unsigned long long>(fnv1a(source)));
    return cache_dir_ + name;
}

void Thumbs::touch(const std::string &source)
{
    const auto found = where_.find(source);
    if (found != where_.end())
        order_.erase(found->second);
    order_.push_back(source);
    where_[source] = std::prev(order_.end());
}

Thumbs::Texture Thumbs::get(const std::string &source)
{
    if (source.empty())
    {
        Texture none;
        none.failed = true;
        return none;
    }
    const auto found = textures_.find(source);
    if (found != textures_.end())
    {
        touch(source);
        return found->second;
    }
    if (pending_.count(source) == 0)
    {
        pending_[source] = true;
        Lock lock(mutex_);
        requests_.push_back({source, false});
        while (requests_.size() > kMaxRequests)
        {
            pending_.erase(requests_.front().source);
            requests_.pop_front();
        }
        wake_.notify_one();
    }
    return {};
}

Thumbs::Texture Thumbs::full(const std::string &source)
{
    if (source == full_source_)
        return full_texture_;
    if (full_texture_.id != 0)
        glDeleteTextures(1, &full_texture_.id);
    full_texture_ = {};
    full_source_ = source;
    full_pending_ = true;
    Lock lock(mutex_);
    requests_.push_back({source, true});
    wake_.notify_one();
    return {};
}

void Thumbs::pump(hui::gfx::GlBatch &batch, int budget)
{
    std::deque<Done> done;
    {
        Lock lock(mutex_);
        while (!done_.empty() && static_cast<int>(done.size()) < budget)
        {
            done.push_back(std::move(done_.front()));
            done_.pop_front();
        }
    }
    for (Done &item : done)
    {
        Texture texture;
        if (item.ok)
        {
            texture.id = batch.create_texture(item.picture.width, item.picture.height,
                                              item.picture.rgba.data());
            texture.aspect = static_cast<float>(item.picture.width) / item.picture.height;
        }
        texture.failed = !item.ok;
        if (item.full)
        {
            if (item.source == full_source_ && full_pending_)
            {
                full_texture_ = texture;
                full_pending_ = false;
            }
            else if (texture.id != 0)
            {
                glDeleteTextures(1, &texture.id);
            }
            continue;
        }
        pending_.erase(item.source);
        textures_[item.source] = texture;
        touch(item.source);
    }
    while (textures_.size() > capacity_ && !order_.empty())
    {
        const std::string oldest = order_.front();
        order_.pop_front();
        where_.erase(oldest);
        const auto found = textures_.find(oldest);
        if (found != textures_.end())
        {
            if (found->second.id != 0)
                glDeleteTextures(1, &found->second.id);
            textures_.erase(found);
        }
    }
}

void Thumbs::loop()
{
    for (;;)
    {
        Request request;
        {
            Lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || !requests_.empty(); });
            if (quit_)
                return;
            // Newest first: what is on screen now.
            request = std::move(requests_.back());
            requests_.pop_back();
        }
        Done done;
        done.source = request.source;
        done.full = request.full;
        if (request.full)
        {
            done.ok = decode_picture(request.source, 1920, 1080, &done.picture);
        }
        else
        {
            const std::string cached = cache_path(request.source);
            std::string bytes;
            if (!cached.empty() && read_text_file(cached, &bytes, 4u << 20))
                done.ok = decode_picture_bytes(bytes, width_, height_, &done.picture);
            if (!done.ok)
            {
                MediaKind kind = MediaKind::screenshot;
                if (capture_kind(request.source, &kind) && kind == MediaKind::video)
                    done.ok = video_decoder_ != nullptr &&
                              video_decoder_(request.source, width_, height_, &done.picture);
                else
                    done.ok = decode_picture(request.source, width_, height_, &done.picture);
                if (done.ok && !cached.empty())
                {
                    const std::string jpeg = encode_jpeg(done.picture, 85);
                    if (!jpeg.empty())
                        write_text_file_atomic(cached, jpeg);
                }
            }
        }
        Lock lock(mutex_);
        done_.push_back(std::move(done));
    }
}

} // namespace pshare::app
