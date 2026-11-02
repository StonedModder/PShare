// PShare - Optional MP4 preparation.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pshare/core/video_convert.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sys/stat.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace pshare
{
namespace
{
struct Conversion
{
    AVFormatContext *input = nullptr, *output = nullptr;
    AVCodecContext *decode[2] = {}, *encode[2] = {};
    int stream[2] = {-1, -1};
    AVStream *target[2] = {};
    AVPacket *packet = av_packet_alloc(), *encoded = av_packet_alloc();
    AVFrame *frame = av_frame_alloc(), *picture = av_frame_alloc(), *sound = av_frame_alloc();
    SwsContext *scale = nullptr;
    SwrContext *resample = nullptr;
    AVAudioFifo *fifo = nullptr;
    std::int64_t origin = 0, last_video = AV_NOPTS_VALUE, audio_pts = AV_NOPTS_VALUE;
    UploadProgress *progress;
    std::string *error;

    Conversion(UploadProgress *p, std::string *e) : progress(p), error(e)
    {
    }
    ~Conversion()
    {
        av_packet_free(&packet);
        av_packet_free(&encoded);
        av_frame_free(&frame);
        av_frame_free(&picture);
        av_frame_free(&sound);
        sws_freeContext(scale);
        swr_free(&resample);
        av_audio_fifo_free(fifo);
        for (int i = 0; i < 2; ++i)
        {
            avcodec_free_context(&decode[i]);
            avcodec_free_context(&encode[i]);
        }
        avformat_close_input(&input);
        if (output)
        {
            if (output->pb)
                avio_closep(&output->pb);
            avformat_free_context(output);
        }
    }
    bool fail(const char *message, int code = 0)
    {
        *error = message;
        if (code < 0)
        {
            char detail[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(code, detail, sizeof(detail));
            *error += std::string(": ") + detail;
        }
        return false;
    }
    bool active()
    {
        return !progress || !progress->cancel.load() || fail("Cancelled");
    }
    bool send(int track, AVFrame *value)
    {
        if (!active())
            return false;
        int code = avcodec_send_frame(encode[track], value);
        if (code < 0)
            return fail("Cannot encode MP4", code);
        while ((code = avcodec_receive_packet(encode[track], encoded)) >= 0)
        {
            av_packet_rescale_ts(encoded, encode[track]->time_base, target[track]->time_base);
            encoded->stream_index = target[track]->index;
            const int written = av_interleaved_write_frame(output, encoded);
            av_packet_unref(encoded);
            if (written < 0)
                return fail("Cannot write MP4 (check free space)", written);
        }
        return code == AVERROR(EAGAIN) || code == AVERROR_EOF || fail("Cannot encode MP4", code);
    }
    bool audio_frames(bool finish)
    {
        while (av_audio_fifo_size(fifo) >= encode[1]->frame_size ||
               (finish && av_audio_fifo_size(fifo) > 0))
        {
            if (!active())
                return false;
            av_frame_unref(sound);
            sound->format = encode[1]->sample_fmt;
            sound->sample_rate = encode[1]->sample_rate;
            if (av_channel_layout_copy(&sound->ch_layout, &encode[1]->ch_layout) < 0)
                return fail("Cannot allocate MP4 audio layout");
            sound->nb_samples = std::min(av_audio_fifo_size(fifo), encode[1]->frame_size);
            int code = av_frame_get_buffer(sound, 0);
            if (code < 0)
                return fail("Cannot allocate MP4 audio", code);
            if (av_audio_fifo_read(fifo, reinterpret_cast<void **>(sound->extended_data),
                                   sound->nb_samples) != sound->nb_samples)
                return fail("Cannot read MP4 audio buffer");
            sound->pts = audio_pts;
            audio_pts += sound->nb_samples;
            if (!send(1, sound))
                return false;
        }
        return true;
    }
    bool audio(AVFrame *value)
    {
        const int capacity = static_cast<int>(av_rescale_rnd(
            swr_get_delay(resample, decode[1]->sample_rate) + (value ? value->nb_samples : 0),
            encode[1]->sample_rate, decode[1]->sample_rate, AV_ROUND_UP));
        if (capacity <= 0)
            return true;
        av_frame_unref(sound);
        sound->format = encode[1]->sample_fmt;
        sound->sample_rate = encode[1]->sample_rate;
        sound->nb_samples = capacity;
        if (av_channel_layout_copy(&sound->ch_layout, &encode[1]->ch_layout) < 0)
            return fail("Cannot allocate audio layout");
        int code = av_frame_get_buffer(sound, 0);
        if (code < 0)
            return fail("Cannot allocate audio conversion buffer", code);
        code =
            swr_convert(resample, sound->extended_data, capacity,
                        value ? const_cast<const std::uint8_t **>(value->extended_data) : nullptr,
                        value ? value->nb_samples : 0);
        if (code < 0)
            return fail("Cannot convert audio", code);
        if (av_audio_fifo_write(fifo, reinterpret_cast<void **>(sound->extended_data), code) !=
            code)
            return fail("Cannot buffer MP4 audio");
        return audio_frames(false);
    }
    bool video(AVFrame *value)
    {
        if (value->color_trc == AVCOL_TRC_SMPTE2084 || value->color_trc == AVCOL_TRC_ARIB_STD_B67)
            return fail("HDR WebM conversion is not supported yet");
        if (value->best_effort_timestamp == AV_NOPTS_VALUE)
            return fail("Video has no timestamps");
        const auto when = av_rescale_q(value->best_effort_timestamp,
                                       input->streams[stream[0]]->time_base, AV_TIME_BASE_Q) -
                          origin;
        const auto pts = av_rescale_q(when, AV_TIME_BASE_Q, encode[0]->time_base);
        if (last_video != AV_NOPTS_VALUE && pts <= last_video)
            return true; // cap frame rate without queueing decoded pictures
        last_video = pts;
        if (av_frame_make_writable(picture) < 0)
            return fail("Cannot allocate MP4 picture");
        scale = sws_getCachedContext(scale, value->width, value->height,
                                     static_cast<AVPixelFormat>(value->format), encode[0]->width,
                                     encode[0]->height, encode[0]->pix_fmt, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
        if (!scale)
            return fail("Cannot create video scaler");
        if (sws_scale(scale, value->data, value->linesize, 0, value->height, picture->data,
                      picture->linesize) != encode[0]->height)
            return fail("Cannot scale video");
        picture->pts = pts;
        if (progress)
        {
            progress->total = input->duration > 0 ? input->duration : 0;
            progress->sent = std::max<std::int64_t>(0, when);
        }
        return send(0, picture);
    }
    bool receive(int track, AVPacket *value)
    {
        int code = avcodec_send_packet(decode[track], value);
        if (code < 0)
            return fail("Cannot decode WebM", code);
        while ((code = avcodec_receive_frame(decode[track], frame)) >= 0)
        {
            if (!active())
                return false;
            if (track == 1 && audio_pts == AV_NOPTS_VALUE)
            {
                const auto stamp = frame->best_effort_timestamp;
                audio_pts =
                    stamp == AV_NOPTS_VALUE
                        ? 0
                        : av_rescale_q(av_rescale_q(stamp, input->streams[stream[1]]->time_base,
                                                    AV_TIME_BASE_Q) -
                                           origin,
                                       AV_TIME_BASE_Q, encode[1]->time_base);
            }
            const bool ok = track == 0 ? video(frame) : audio(frame);
            av_frame_unref(frame);
            if (!ok)
                return false;
        }
        return code == AVERROR(EAGAIN) || code == AVERROR_EOF || fail("Cannot decode WebM", code);
    }

    bool run(const MediaItem &item, const std::string &path)
    {
        if (!packet || !encoded || !frame || !picture || !sound)
            return fail("Not enough memory for MP4 conversion");
        if (!active())
            return false;
        int code = avformat_open_input(&input, item.path.c_str(), nullptr, nullptr);
        if (code < 0)
            return fail("Cannot open WebM", code);
        // Patch only the decoder/probe copy, preserving the original capture.
        for (unsigned i = 0; i < input->nb_streams; ++i)
        {
            auto *p = input->streams[i]->codecpar;
            if (p->codec_id == AV_CODEC_ID_OPUS && p->extradata_size >= 19 &&
                std::memcmp(p->extradata, "OpusHead", 8) == 0 && p->extradata[8] > 15)
                p->extradata[8] = 1;
        }
        if ((code = avformat_find_stream_info(input, nullptr)) < 0)
            return fail("Cannot read WebM streams", code);
        stream[0] = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        stream[1] = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (stream[0] < 0)
            return fail("WebM has no video track");
        origin = input->start_time == AV_NOPTS_VALUE ? 0 : input->start_time;
        if ((code = avformat_alloc_output_context2(&output, nullptr, "mp4", path.c_str())) < 0 ||
            !output)
            return fail("Cannot create MP4 container", code);
        for (int track = 0; track < 2; ++track)
        {
            if (stream[track] < 0)
                continue;
            auto *source = input->streams[stream[track]];
            const AVCodec *decoder = avcodec_find_decoder(source->codecpar->codec_id);
            const AVCodec *encoder = track == 0 ? avcodec_find_encoder_by_name("libx264")
                                                : avcodec_find_encoder(AV_CODEC_ID_AAC);
            if (!decoder || !encoder)
                return fail("MP4 conversion requires the WebM decoder, libx264 and AAC");
            decode[track] = avcodec_alloc_context3(decoder);
            encode[track] = avcodec_alloc_context3(encoder);
            if (!decode[track] || !encode[track])
                return fail("Cannot allocate MP4 codecs");
            if ((code = avcodec_parameters_to_context(decode[track], source->codecpar)) < 0)
                return fail("Cannot set up WebM decoder", code);
            decode[track]->thread_count = 2;
            decode[track]->pkt_timebase = source->time_base;
            if ((code = avcodec_open2(decode[track], decoder, nullptr)) < 0)
                return fail("Cannot open WebM decoder", code);
            auto *enc = encode[track];
            enc->thread_count = track == 0 ? 2 : 1;
            enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (track == 0)
            {
                const int width = decode[0]->width, height = decode[0]->height;
                if (width <= 0 || height <= 0)
                    return fail("Invalid WebM dimensions");
                const double ratio = std::min({1.0, 1920.0 / width, 1080.0 / height});
                enc->width = std::max(2, static_cast<int>(width * ratio) / 2 * 2);
                enc->height = std::max(2, static_cast<int>(height * ratio) / 2 * 2);
                enc->pix_fmt = AV_PIX_FMT_YUV420P;
                enc->framerate = av_guess_frame_rate(input, source, nullptr);
                if (enc->framerate.num <= 0 || enc->framerate.den <= 0 ||
                    av_q2d(enc->framerate) > 30)
                    enc->framerate = AVRational{30, 1};
                enc->time_base = av_inv_q(enc->framerate);
                enc->gop_size =
                    std::max(1, static_cast<int>(std::ceil(av_q2d(enc->framerate) * 2)));
                enc->max_b_frames = 0;
                enc->sample_aspect_ratio = decode[0]->sample_aspect_ratio;
                enc->color_primaries = decode[0]->color_primaries;
                enc->color_trc = decode[0]->color_trc;
                enc->colorspace = decode[0]->colorspace;
                enc->color_range = decode[0]->color_range;
                if (av_opt_set(enc->priv_data, "preset", "ultrafast", 0) < 0 ||
                    av_opt_set(enc->priv_data, "crf", "23", 0) < 0)
                    return fail("Cannot configure H.264 encoder");
            }
            else
            {
                enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
                enc->sample_rate = 48000;
                enc->time_base = AVRational{1, 48000};
                enc->bit_rate = 128000;
                av_channel_layout_default(&enc->ch_layout, 2);
            }
            if ((code = avcodec_open2(enc, encoder, nullptr)) < 0)
                return fail("Cannot open MP4 encoder", code);
            target[track] = avformat_new_stream(output, nullptr);
            if (!target[track] || avcodec_parameters_from_context(target[track]->codecpar, enc) < 0)
                return fail("Cannot create MP4 track");
            target[track]->time_base = enc->time_base;
        }
        picture->format = encode[0]->pix_fmt;
        picture->width = encode[0]->width;
        picture->height = encode[0]->height;
        if (av_frame_get_buffer(picture, 32) < 0)
            return fail("Cannot allocate MP4 video buffer");
        if (stream[1] >= 0)
        {
            code = swr_alloc_set_opts2(&resample, &encode[1]->ch_layout, encode[1]->sample_fmt,
                                       encode[1]->sample_rate, &decode[1]->ch_layout,
                                       decode[1]->sample_fmt, decode[1]->sample_rate, 0, nullptr);
            if (code < 0 || (code = swr_init(resample)) < 0)
                return fail("Cannot create audio converter", code);
            fifo = av_audio_fifo_alloc(encode[1]->sample_fmt, 2, 1);
            if (!fifo)
                return fail("Cannot allocate audio buffer");
        }
        if ((code = avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0)
            return fail("Cannot write MP4 (check free space)", code);
        AVDictionary *options = nullptr;
        av_dict_set(&options, "movflags", "+faststart", 0);
        code = avformat_write_header(output, &options);
        av_dict_free(&options);
        if (code < 0)
            return fail("Cannot write MP4 header", code);
        while ((code = av_read_frame(input, packet)) >= 0)
        {
            const int track = packet->stream_index == stream[0]   ? 0
                              : packet->stream_index == stream[1] ? 1
                                                                  : -1;
            if (!active() || (track >= 0 && !receive(track, packet)))
                return false;
            av_packet_unref(packet);
        }
        if (code != AVERROR_EOF)
            return fail("Cannot read complete WebM", code);
        for (int track = 0; track < 2; ++track)
            if (stream[track] >= 0 && !receive(track, nullptr))
                return false;
        if (stream[1] >= 0 && (!audio(nullptr) || !audio_frames(true)))
            return false;
        for (int track = 0; track < 2; ++track)
            if (stream[track] >= 0 && !send(track, nullptr))
                return false;
        if (!active())
            return false;
        if ((code = av_write_trailer(output)) < 0 || (code = avio_closep(&output->pb)) < 0)
            return fail("Cannot finish MP4", code);
        return true;
    }
};
} // namespace

bool convert_webm(const MediaItem &item, const std::string &scratch, UploadProgress *progress,
                  VideoParts *output, std::string *error)
{
    if (progress && progress->cancel.load())
    {
        *error = "Cancelled";
        return false;
    }
    if (!create_video_workspace(scratch, output, error))
        return false;
    MediaItem converted = item;
    converted.file = item.file.substr(0, item.file.find_last_of('.')) + ".mp4";
    converted.path = output->directory + "/" + converted.file;
    output->items.push_back(converted);
    Conversion job(progress, error);
    if (!job.run(item, converted.path))
        return false;
    struct stat info
    {
    };
    if (stat(converted.path.c_str(), &info) != 0 || info.st_size <= 0)
    {
        *error = "Converted MP4 is empty";
        return false;
    }
    output->items.back().size = static_cast<std::uint64_t>(info.st_size);
    return true;
}
} // namespace pshare
