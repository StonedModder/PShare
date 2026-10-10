// PShare - Playing a clip inside the app, picture and sound.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/video.hpp"

#include <GL/glcorearb.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <unistd.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#if defined(PSHARE_CONSOLE)
#include "platform/ps5/system.hpp"
#define PSHARE_VIDEO_LOG(...) hui::sys::log(__VA_ARGS__)
#else
#define PSHARE_VIDEO_LOG(...) ((void)0)
#endif

namespace pshare::app
{
namespace
{
constexpr int kMaxWidth = 1280;
constexpr int kMaxHeight = 720;
constexpr std::size_t kFrames = 5;             // decoded pictures waiting
constexpr std::size_t kRingFrames = 48000 * 2; // two seconds of sound
constexpr int kRate = 48000;
constexpr double kOutputLatency = 0.045; // the audio thread's buffer, about
constexpr std::size_t kMaxPacketBytes = 96u << 20;
constexpr int kDecodeThreads = 4;

// Everything FFmpeg allocates for one open file, freed in one place.
struct Media
{
    AVFormatContext *format = nullptr;
    AVCodecContext *video = nullptr;
    AVCodecContext *audio = nullptr;
    SwsContext *scale = nullptr;
    SwrContext *resample = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    int video_index = -1;
    int audio_index = -1;
    int out_width = 0;
    int out_height = 0;

    ~Media()
    {
        av_packet_free(&packet);
        av_frame_free(&frame);
        swr_free(&resample);
        sws_freeContext(scale);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        avformat_close_input(&format);
    }
};

std::string av_error(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

// The PS5's own recordings (WebM, VP9 + Opus) carry an Opus header whose
// version byte is past the spec, and FFmpeg refuses it ("patches welcome"), so
// the clip would play without sound. For mono or stereo, decode as if there
// were no header: FFmpeg's default stereo one. The same fix as VLC for PS5's
// patches/ffmpeg/0001-opus-ps5-recording-header.patch (ZiZc3/VLC, GPL-3.0),
// done here on the codec context because PacBrew's FFmpeg is prebuilt.
void fix_ps5_opus_header(AVCodecParameters *parameters)
{
    if (parameters->codec_id != AV_CODEC_ID_OPUS || parameters->extradata == nullptr ||
        parameters->extradata_size < 19 || std::memcmp(parameters->extradata, "OpusHead", 8) != 0 ||
        parameters->extradata[8] <= 15 || parameters->ch_layout.nb_channels > 2)
        return;
    static const std::uint8_t kDefault[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2,
                                              0,   0,   0,   0,   0,   0,   0,   0,   0};
    auto *fresh =
        static_cast<std::uint8_t *>(av_mallocz(sizeof(kDefault) + AV_INPUT_BUFFER_PADDING_SIZE));
    if (fresh == nullptr)
        return;
    std::memcpy(fresh, kDefault, sizeof(kDefault));
    av_freep(&parameters->extradata);
    parameters->extradata = fresh;
    parameters->extradata_size = sizeof(kDefault);
    PSHARE_VIDEO_LOG("[PShare] clip: PS5 Opus header, using the default stereo one");
}

bool open_codec(AVFormatContext *format, int index, int threads, AVCodecContext **out)
{
    const AVCodecParameters *parameters = format->streams[index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr)
        return false;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (context == nullptr)
        return false;
    if (avcodec_parameters_to_context(context, parameters) < 0)
    {
        avcodec_free_context(&context);
        return false;
    }
    context->pkt_timebase = format->streams[index]->time_base;
    if (threads > 1)
    {
        // Each frame thread keeps a picture of its own: a 4K recording gets
        // two, so the decoder stays inside the app's heap.
        if (parameters->width * parameters->height > 1920 * 1088)
            threads = std::min(threads, 2);
        context->thread_count = threads;
        context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    if (avcodec_open2(context, codec, nullptr) < 0)
    {
        avcodec_free_context(&context);
        return false;
    }
    *out = context;
    return true;
}

// Opens a file and its decoders. `want_audio` false skips the sound.
bool open_media(const std::string &path, bool want_audio, int threads, int max_width,
                int max_height, Media *media, std::string *error)
{
    int code = avformat_open_input(&media->format, path.c_str(), nullptr, nullptr);
    if (code < 0)
    {
        *error = "Cannot open the clip (" + av_error(code) + ")";
        return false;
    }
    // Before probing: the parser that probing starts would refuse it too,
    // and then the demuxer hands over no sound at all.
    for (unsigned i = 0; i < media->format->nb_streams; ++i)
        fix_ps5_opus_header(media->format->streams[i]->codecpar);
    code = avformat_find_stream_info(media->format, nullptr);
    if (code < 0)
    {
        *error = "Cannot read the clip (" + av_error(code) + ")";
        return false;
    }
    media->video_index = av_find_best_stream(media->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (media->video_index < 0 ||
        !open_codec(media->format, media->video_index, threads, &media->video))
    {
        *error = "This clip's picture format is not supported";
        return false;
    }
    if (want_audio)
    {
        media->audio_index = av_find_best_stream(media->format, AVMEDIA_TYPE_AUDIO, -1,
                                                 media->video_index, nullptr, 0);
        if (media->audio_index >= 0 &&
            !open_codec(media->format, media->audio_index, 1, &media->audio))
            media->audio_index = -1; // play it silent rather than not at all
    }
    // Fit the picture into the output box, keeping its shape (and even sizes
    // for the converter).
    const int width = media->video->width;
    const int height = media->video->height;
    if (width <= 0 || height <= 0)
    {
        *error = "The clip has no picture size";
        return false;
    }
    const double fit = std::min(
        {1.0, static_cast<double>(max_width) / width, static_cast<double>(max_height) / height});
    media->out_width = std::max(2, static_cast<int>(width * fit) & ~1);
    media->out_height = std::max(2, static_cast<int>(height * fit) & ~1);
    media->frame = av_frame_alloc();
    media->packet = av_packet_alloc();
    return media->frame != nullptr && media->packet != nullptr;
}

// The frame as RGBA in `out` (out_width x out_height); the converter is made
// on first use, when the decoder's real pixel format is known.
bool convert(Media *media, const AVFrame *frame, std::uint8_t *out)
{
    media->scale = sws_getCachedContext(media->scale, frame->width, frame->height,
                                        static_cast<AVPixelFormat>(frame->format), media->out_width,
                                        media->out_height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                        nullptr, nullptr);
    if (media->scale == nullptr)
        return false;
    std::uint8_t *planes[4] = {out, nullptr, nullptr, nullptr};
    int strides[4] = {media->out_width * 4, 0, 0, 0};
    return sws_scale(media->scale, frame->data, frame->linesize, 0, frame->height, planes,
                     strides) > 0;
}

double frame_time(const Media &media, int index, const AVFrame *frame)
{
    const std::int64_t stamp =
        frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    if (stamp == AV_NOPTS_VALUE)
        return -1.0;
    double seconds = static_cast<double>(stamp) * av_q2d(media.format->streams[index]->time_base);
    const std::int64_t start = media.format->streams[index]->start_time;
    if (start != AV_NOPTS_VALUE)
        seconds -= static_cast<double>(start) * av_q2d(media.format->streams[index]->time_base);
    return seconds;
}

const char *codec_label(AVCodecID id)
{
    switch (id)
    {
    case AV_CODEC_ID_VP9:
        return "VP9";
    case AV_CODEC_ID_H264:
        return "H.264";
    case AV_CODEC_ID_HEVC:
        return "HEVC";
    case AV_CODEC_ID_AV1:
        return "AV1";
    default:
        return avcodec_get_name(id);
    }
}
} // namespace

// ---- one picture --------------------------------------------------------------

bool video_thumbnail(const std::string &path, int max_width, int max_height, Picture *picture)
{
    Media media;
    std::string error;
    if (!open_media(path, false, 2, max_width, max_height, &media, &error))
        return false;
    const double length = media.format->duration > 0
                              ? static_cast<double>(media.format->duration) / AV_TIME_BASE
                              : 0.0;
    const double at = std::min(1.0, length / 3.0);
    if (at > 0.05)
        av_seek_frame(media.format, -1, static_cast<std::int64_t>(at * AV_TIME_BASE),
                      AVSEEK_FLAG_BACKWARD);
    Picture out;
    out.width = media.out_width;
    out.height = media.out_height;
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    // A few hundred packets is plenty to reach a picture; a broken file must
    // not keep the thumbnail thread busy forever.
    for (int packets = 0; packets < 600; ++packets)
    {
        const int code = av_read_frame(media.format, media.packet);
        const bool end = code < 0;
        if (!end && media.packet->stream_index != media.video_index)
        {
            av_packet_unref(media.packet);
            continue;
        }
        avcodec_send_packet(media.video, end ? nullptr : media.packet);
        av_packet_unref(media.packet);
        while (avcodec_receive_frame(media.video, media.frame) == 0)
        {
            const bool ok = convert(&media, media.frame, out.rgba.data());
            av_frame_unref(media.frame);
            if (ok)
            {
                *picture = std::move(out);
                return true;
            }
        }
        if (end)
            break;
    }
    return false;
}

double video_duration(const std::string &path)
{
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0)
        return 0.0;
    for (unsigned i = 0; i < format->nb_streams; ++i)
        fix_ps5_opus_header(format->streams[i]->codecpar);
    double seconds = 0.0;
    if (avformat_find_stream_info(format, nullptr) >= 0 && format->duration > 0)
        seconds = static_cast<double>(format->duration) / AV_TIME_BASE;
    avformat_close_input(&format);
    return seconds;
}

// ---- the player -------------------------------------------------------------

VideoPlayer::VideoPlayer(hui::audio::Mixer *mixer, std::size_t stream_slot)
    : mixer_(mixer), slot_(stream_slot)
{
}

VideoPlayer::~VideoPlayer()
{
    close();
    if (texture_ != 0)
        glDeleteTextures(1, &texture_);
    if (retired_ != 0)
        glDeleteTextures(1, &retired_);
}

void VideoPlayer::stop_thread()
{
    quit_ = true;
    thread_.join();
    quit_ = false;
}

void VideoPlayer::open(const std::string &path, double at)
{
    close();
    path_ = path;
    open_ = true;
    paused_ = false;
    texture_ready_ = false;
    position_ = at;
    wall_clock_ = at;
    {
        Lock lock(mutex_);
        frames_.clear();
        error_.clear();
        description_.clear();
        eof_ = false;
        duration_ = 0.0;
        has_audio_ = false;
        seek_pending_ = false;
    }
    audio_written_ = 0;
    audio_start_ = -1.0;
    ring_ = nullptr;
    last_audio_ = -1.0;
    audio_still_ = 0.0f;
    audio_clock_dead_ = false;
    const std::uint64_t generation = ++generation_;
    thread_.start([this, path, at, generation] { run(path, at, generation); }, 1u << 20);
}

void VideoPlayer::close()
{
    if (!open_)
        return;
    stop_thread();
    if (mixer_ != nullptr)
    {
        mixer_->swap_stream(slot_, nullptr);
        mixer_->set_stream_gain(slot_, 0.0f, 0.05f);
    }
    ring_ = nullptr;
    open_ = false;
    texture_ready_ = false;
    Lock lock(mutex_);
    frames_.clear();
    // The rings stay allocated (the mixer may still be reading one this
    // instant); the next open reuses the list.
}

void VideoPlayer::set_paused(bool paused)
{
    if (!open_ || paused == paused_)
        return;
    paused_ = paused;
    if (mixer_ != nullptr)
        mixer_->swap_stream(slot_, paused ? nullptr : ring_);
}

void VideoPlayer::seek(double seconds)
{
    if (!open_)
        return;
    const double length = duration();
    if (length > 0.0)
        seconds = std::clamp(seconds, 0.0, std::max(0.0, length - 0.25));
    else
        seconds = std::max(0.0, seconds);
    {
        Lock lock(mutex_);
        seek_target_ = seconds;
        seek_pending_ = true;
        eof_ = false;
    }
    position_ = seconds;
    wall_clock_ = seconds;
    audio_start_ = -1.0;
    last_audio_ = -1.0;
    audio_still_ = 0.0f;
    // Silence now: what the old ring still holds is from before the jump.
    if (mixer_ != nullptr)
        mixer_->swap_stream(slot_, nullptr);
    ring_ = nullptr;
}

void VideoPlayer::set_volume(float volume)
{
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (mixer_ != nullptr && ring_ != nullptr)
        mixer_->set_stream_gain(slot_, volume_, 0.1f);
}

double VideoPlayer::duration() const
{
    Lock lock(mutex_);
    return duration_;
}

bool VideoPlayer::has_audio() const
{
    Lock lock(mutex_);
    return has_audio_;
}

std::string VideoPlayer::error() const
{
    Lock lock(mutex_);
    return error_;
}

std::string VideoPlayer::description() const
{
    Lock lock(mutex_);
    return description_;
}

bool VideoPlayer::ended() const
{
    Lock lock(mutex_);
    return open_ && eof_ && frames_.empty() && !seek_pending_ &&
           (ring_ == nullptr || ring_->available() == 0);
}

double VideoPlayer::audio_clock() const
{
    const double start = audio_start_.load();
    if (ring_ == nullptr || start < 0.0)
        return -1.0;
    const double played =
        static_cast<double>(audio_written_.load()) - static_cast<double>(ring_->available());
    return start + std::max(0.0, played / kRate - kOutputLatency);
}

void VideoPlayer::update(float dt, hui::gfx::GlBatch &batch)
{
    if (!open_)
        return;
    // A new ring from the decoder (open or seek): the mixer is told here, on
    // the main thread, because its command queue has one producer.
    hui::audio::StreamRing *ring = nullptr;
    {
        Lock lock(mutex_);
        if (!rings_.empty() && rings_.back().get() != ring_ && audio_start_.load() >= -0.5)
            ring = rings_.back().get();
    }
    if (ring != nullptr && ring != ring_)
    {
        ring_ = ring;
        if (mixer_ != nullptr)
        {
            mixer_->swap_stream(slot_, paused_ ? nullptr : ring_);
            mixer_->set_stream_gain(slot_, volume_, 0.05f);
        }
    }

    const bool seeking = seek_pending_.load();
    double clock = position_;
    if (!paused_ && !seeking && texture_ready_)
    {
        double audio =
            mixer_ != nullptr && has_audio() && !audio_clock_dead_ ? audio_clock() : -1.0;
        if (audio >= 0.0)
        {
            // Sound that should be playing but whose clock stands still:
            // the output is not taking it. Follow the wall clock from here.
            if (std::fabs(audio - last_audio_) < 1e-6)
                audio_still_ += dt;
            else
                audio_still_ = 0.0f;
            last_audio_ = audio;
            if (audio_still_ > 0.4f)
            {
                audio_clock_dead_ = true;
                wall_clock_ = audio;
                audio = -1.0;
                PSHARE_VIDEO_LOG(
                    "[PShare] clip: the sound clock stopped at %.2f s; using the wall clock",
                    wall_clock_);
            }
        }
        if (audio >= 0.0)
        {
            clock = audio;
            wall_clock_ = audio;
        }
        else
        {
            wall_clock_ += dt;
            clock = wall_clock_;
        }
    }
    Frame shown;
    bool have = false;
    {
        Lock lock(mutex_);
        // The newest frame that is due; older ones are skipped (a slow decode
        // drops pictures, never the sound).
        while (!frames_.empty() && (frames_.front().pts <= clock + 0.008 || !texture_ready_))
        {
            if (have)
                pool_.push_back(std::move(shown.rgba));
            shown = std::move(frames_.front());
            frames_.pop_front();
            have = true;
            if (!texture_ready_)
                break;
        }
    }
    if (have)
    {
        // A new texture for every picture. Updating one in place
        // (glTexSubImage2D) kept showing the first picture on the console;
        // creating textures is the path every image in the app already uses.
        // The previous one may still be drawn by the frame in flight, so it is
        // deleted one picture later.
        if (retired_ != 0)
            glDeleteTextures(1, &retired_);
        retired_ = texture_;
        texture_ = batch.create_texture(width_, height_, shown.rgba.data());
        aspect_ = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
        if (!texture_ready_)
        {
            // The clock starts at the first picture.
            wall_clock_ = shown.pts;
            clock = shown.pts;
        }
        texture_ready_ = true;
        Lock lock(mutex_);
        pool_.push_back(std::move(shown.rgba));
    }
    if (!seeking)
        position_ = std::max(0.0, clock);
    log_timer_ += dt;
    if (log_timer_ >= 2.0f && !paused_)
    {
        log_timer_ = 0.0f;
        std::size_t queued = 0;
        {
            Lock lock(mutex_);
            queued = frames_.size();
        }
        PSHARE_VIDEO_LOG("[PShare] clip at %.2f s, audio %s, ring %zu, pictures waiting %zu",
                         position_, audio_clock_dead_ ? "off" : (has_audio() ? "on" : "none"),
                         ring_ != nullptr ? ring_->available() : static_cast<std::size_t>(0),
                         queued);
    }
    clock_ = position_;
    if (ended() && !paused_)
        set_paused(true);
}

void VideoPlayer::run(std::string path, double at, std::uint64_t generation)
{
    Media media;
    std::string error;
    if (!open_media(path, true, kDecodeThreads, kMaxWidth, kMaxHeight, &media, &error))
    {
        Lock lock(mutex_);
        error_ = error;
        return;
    }
    const double length = media.format->duration > 0
                              ? static_cast<double>(media.format->duration) / AV_TIME_BASE
                              : 0.0;
    if (media.audio_index >= 0)
    {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&media.resample, &stereo, AV_SAMPLE_FMT_FLT, kRate,
                                &media.audio->ch_layout, media.audio->sample_fmt,
                                media.audio->sample_rate, 0, nullptr) < 0 ||
            swr_init(media.resample) < 0)
            media.audio_index = -1;
    }
    {
        Lock lock(mutex_);
        duration_ = length;
        width_ = media.out_width;
        height_ = media.out_height;
        has_audio_ = media.audio_index >= 0;
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d %s", media.video->width, media.video->height,
                      codec_label(media.video->codec_id));
        description_ = text;
        if (at > 0.0)
        {
            seek_target_ = at;
            seek_pending_ = true;
        }
    }

    std::deque<AVPacket *> video_packets;
    std::deque<AVPacket *> audio_packets;
    std::size_t queued_bytes = 0;
    const auto clear_packets = [&]()
    {
        for (AVPacket *packet : video_packets)
            av_packet_free(&packet);
        for (AVPacket *packet : audio_packets)
            av_packet_free(&packet);
        video_packets.clear();
        audio_packets.clear();
        queued_bytes = 0;
    };
    hui::audio::StreamRing *ring = nullptr;
    const auto new_ring = [&]()
    {
        if (media.audio_index < 0)
            return;
        Lock lock(mutex_);
        // Reuse a ring the mixer let go of long ago; keep the three newest.
        std::unique_ptr<hui::audio::StreamRing> fresh;
        if (rings_.size() >= 4)
        {
            fresh = std::move(rings_.front());
            rings_.pop_front();
            fresh->drain();
        }
        else
        {
            fresh = std::make_unique<hui::audio::StreamRing>(kRingFrames);
        }
        ring = fresh.get();
        rings_.push_back(std::move(fresh));
        audio_written_ = 0;
        audio_start_ = -1.0;
    };
    new_ring();

    double skip_until = -1.0;
    bool demux_done = false;
    bool video_flushed = false;
    std::vector<float> samples;
    std::vector<std::uint8_t> picture;

    while (!quit_.load() && generation == generation_.load())
    {
        if (seek_pending_.load())
        {
            double target = 0.0;
            {
                Lock lock(mutex_);
                target = seek_target_;
                for (Frame &frame : frames_)
                    pool_.push_back(std::move(frame.rgba));
                frames_.clear();
            }
            av_seek_frame(media.format, -1, static_cast<std::int64_t>(target * AV_TIME_BASE),
                          AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(media.video);
            if (media.audio != nullptr)
                avcodec_flush_buffers(media.audio);
            if (media.resample != nullptr)
                swr_init(media.resample);
            clear_packets();
            new_ring();
            skip_until = target - 0.02;
            demux_done = false;
            video_flushed = false;
            {
                Lock lock(mutex_);
                eof_ = false;
            }
            seek_pending_ = false;
        }

        bool worked = false;
        // 1. Sound, while the ring has room for a packet's worth.
        if (ring != nullptr && !audio_packets.empty() && ring->space() > 16384)
        {
            AVPacket *packet = audio_packets.front();
            audio_packets.pop_front();
            queued_bytes -= static_cast<std::size_t>(packet->size);
            avcodec_send_packet(media.audio, packet);
            av_packet_free(&packet);
            while (avcodec_receive_frame(media.audio, media.frame) == 0)
            {
                const double pts = frame_time(media, media.audio_index, media.frame);
                if (skip_until >= 0.0 && pts >= 0.0 && pts < skip_until)
                {
                    av_frame_unref(media.frame);
                    continue;
                }
                const int room = swr_get_out_samples(media.resample, media.frame->nb_samples);
                samples.resize(static_cast<std::size_t>(std::max(room, 0)) * 2 + 64);
                std::uint8_t *out[1] = {reinterpret_cast<std::uint8_t *>(samples.data())};
                const int got =
                    swr_convert(media.resample, out, room,
                                const_cast<const std::uint8_t **>(media.frame->extended_data),
                                media.frame->nb_samples);
                if (got > 0)
                {
                    if (audio_start_.load() < 0.0)
                        audio_start_ = pts >= 0.0 ? pts : std::max(0.0, skip_until);
                    std::size_t written = 0;
                    int waits = 0;
                    while (written < static_cast<std::size_t>(got) && !quit_.load() &&
                           !seek_pending_.load())
                    {
                        written += ring->write(samples.data() + written * 2,
                                               static_cast<std::size_t>(got) - written);
                        if (written < static_cast<std::size_t>(got))
                        {
                            // Nobody has read for half a second: drop the rest
                            // rather than hold the pictures back.
                            if (++waits > 125)
                                break;
                            usleep(4000);
                        }
                    }
                    audio_written_ += written;
                }
                av_frame_unref(media.frame);
            }
            worked = true;
        }
        // 2. Pictures, while the queue has room.
        std::size_t waiting = 0;
        {
            Lock lock(mutex_);
            waiting = frames_.size();
        }
        if (waiting < kFrames && (!video_packets.empty() || (demux_done && !video_flushed)))
        {
            AVPacket *packet = nullptr;
            if (!video_packets.empty())
            {
                packet = video_packets.front();
                video_packets.pop_front();
                queued_bytes -= static_cast<std::size_t>(packet->size);
            }
            else
            {
                video_flushed = true; // drain the decoder's last pictures
            }
            avcodec_send_packet(media.video, packet);
            av_packet_free(&packet);
            while (avcodec_receive_frame(media.video, media.frame) == 0)
            {
                const double pts = frame_time(media, media.video_index, media.frame);
                const bool early = skip_until >= 0.0 && pts >= 0.0 && pts < skip_until;
                // Far behind the clock: not worth converting.
                const bool late = !early && pts >= 0.0 && pts + 0.25 < clock_.load() && waiting > 0;
                if (!early && !late)
                {
                    {
                        Lock lock(mutex_);
                        if (!pool_.empty())
                        {
                            picture = std::move(pool_.back());
                            pool_.pop_back();
                        }
                    }
                    picture.resize(static_cast<std::size_t>(media.out_width) * media.out_height *
                                   4);
                    if (convert(&media, media.frame, picture.data()))
                    {
                        Frame frame;
                        frame.pts = pts >= 0.0 ? pts : clock_.load();
                        frame.rgba = std::move(picture);
                        Lock lock(mutex_);
                        frames_.push_back(std::move(frame));
                        ++waiting;
                    }
                    if (skip_until >= 0.0)
                        skip_until = -1.0;
                }
                av_frame_unref(media.frame);
            }
            worked = true;
        }
        // 3. More packets, when a queue runs low.
        const bool want_audio = ring != nullptr && audio_packets.size() < 8;
        const bool want_video = video_packets.size() < 16 && waiting < kFrames;
        if (!demux_done && (want_audio || want_video) && queued_bytes < kMaxPacketBytes)
        {
            const int code = av_read_frame(media.format, media.packet);
            if (code < 0)
            {
                demux_done = true;
                if (media.audio != nullptr)
                {
                    // Drain the sound decoder too.
                    AVPacket *flush = av_packet_alloc();
                    flush->size = 0;
                    flush->data = nullptr;
                    audio_packets.push_back(flush);
                }
            }
            else if (media.packet->stream_index == media.video_index)
            {
                queued_bytes += static_cast<std::size_t>(media.packet->size);
                video_packets.push_back(av_packet_clone(media.packet));
            }
            else if (media.packet->stream_index == media.audio_index && ring != nullptr)
            {
                queued_bytes += static_cast<std::size_t>(media.packet->size);
                audio_packets.push_back(av_packet_clone(media.packet));
            }
            av_packet_unref(media.packet);
            worked = true;
        }
        if (demux_done && video_flushed && video_packets.empty() && audio_packets.empty())
        {
            Lock lock(mutex_);
            eof_ = true;
        }
        if (!worked)
            usleep(3000);
    }
    clear_packets();
}

} // namespace pshare::app
