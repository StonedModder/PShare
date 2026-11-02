// PShare - Lossless, bounded video parts for Telegram.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pshare/core/video_split.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace pshare
{
VideoParts::~VideoParts()
{
    for (const auto &item : items)
        unlink(item.path.c_str());
    if (!directory.empty())
        rmdir(directory.c_str());
}

bool create_video_workspace(const std::string &scratch, VideoParts *parts, std::string *error)
{
    const auto fail = [&](const char *why)
    {
        *error = why;
        return false;
    };
    if (!ensure_directory(scratch))
        return fail("Cannot create video scratch folder");
    // Avoid mkdtemp: the payload SDK imports it from a WebKit library that
    // native titles do not load. mkdir is exclusive and works in both builds.
    static std::atomic<unsigned> sequence{0};
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string directory = scratch + "/telegram-" + std::to_string(stamp) + "-" +
                                      std::to_string(sequence.fetch_add(1));
        if (mkdir(directory.c_str(), 0700) == 0)
        {
            parts->directory = directory;
            break;
        }
        if (errno != EEXIST)
            return fail("Cannot create video part folder");
    }
    if (parts->directory.empty())
        return fail("Cannot create a unique video part folder");
    return true;
}

namespace
{
struct Input
{
    AVFormatContext *context = nullptr;
    ~Input()
    {
        avformat_close_input(&context);
    }
};
struct Output
{
    AVFormatContext *context = nullptr;
    ~Output()
    {
        if (context)
        {
            if (context->pb)
                avio_closep(&context->pb);
            avformat_free_context(context);
        }
    }
};
struct Packet
{
    AVPacket *value = av_packet_alloc();
    ~Packet()
    {
        av_packet_free(&value);
    }
};
struct Group
{
    std::uint64_t packet;
    std::uint64_t bytes;
    std::int64_t time;
};
bool cancelled(UploadProgress *progress)
{
    return progress && progress->cancel.load();
}
} // namespace

bool split_video(const MediaItem &item, std::uint64_t limit, const std::string &scratch,
                 UploadProgress *progress, VideoParts *parts, std::string *error)
{
    auto fail = [&](const std::string &why)
    {
        *error = why;
        return false;
    };
    // Leave room for container headers, indexes and packet overhead. Validate
    // every closed file as well, before sending any part over the network.
    if (limit < 256 * 1024)
        return fail("Video part limit is too small");
    const std::uint64_t budget = limit - std::max<std::uint64_t>(limit / 20, 128 * 1024);
    Input input;
    if (avformat_open_input(&input.context, item.path.c_str(), nullptr, nullptr) < 0)
        return fail("Cannot open video for splitting");
    int video = -1;
    std::vector<int> mapping(input.context->nb_streams, -1);
    int streams = 0;
    for (unsigned i = 0; i < input.context->nb_streams; ++i)
    {
        const auto type = input.context->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)
            mapping[i] = streams++;
        if (type == AVMEDIA_TYPE_VIDEO && video < 0)
            video = static_cast<int>(i);
        else if (type == AVMEDIA_TYPE_VIDEO)
            return fail("Splitting multiple video tracks is not supported");
    }
    if (video < 0)
        return fail("No video track found");
    Packet packet;
    if (!packet.value)
        return fail("Not enough memory to split video");
    std::vector<Group> groups;
    std::uint64_t index = 0, bytes = 0;
    int code;
    bool first_video = true;
    while ((code = av_read_frame(input.context, packet.value)) >= 0)
    {
        if (cancelled(progress))
            return fail("Cancelled");
        const AVPacket *p = packet.value;
        if (p->stream_index == video)
        {
            if (first_video && !(p->flags & AV_PKT_FLAG_KEY))
                return fail("Video does not start with a keyframe");
            first_video = false;
            if (p->flags & AV_PKT_FLAG_KEY)
            {
                if (p->dts == AV_NOPTS_VALUE)
                    return fail("Video has no decoding timestamps");
                if (!groups.empty())
                    groups.back().bytes = bytes;
                // Audio preceding the first video packet belongs to part 1.
                groups.push_back({groups.empty() ? 0 : index, 0,
                                  av_rescale_q(p->dts, input.context->streams[video]->time_base,
                                               AV_TIME_BASE_Q)});
                if (groups.size() > 1)
                    bytes = 0;
            }
        }
        if (mapping[p->stream_index] >= 0)
            bytes += static_cast<std::uint64_t>(p->size);
        av_packet_unref(packet.value);
        ++index;
    }
    if (code != AVERROR_EOF || groups.empty())
        return fail("Cannot read complete video for splitting");
    groups.back().bytes = bytes;
    const auto packet_count = index;
    std::vector<Group> cuts;
    bytes = 0;
    for (const auto &group : groups)
    {
        if (group.bytes > budget)
            return fail(
                "A video keyframe interval exceeds the part limit; re-encoding is required");
        if (cuts.empty() || bytes + group.bytes > budget)
        {
            cuts.push_back(group);
            bytes = 0;
        }
        bytes += group.bytes;
    }
    // Reopen rather than seeking: demux packet positions must match pass 1.
    avformat_close_input(&input.context);
    if (avformat_open_input(&input.context, item.path.c_str(), nullptr, nullptr) < 0)
        return fail("Cannot reopen video for splitting");
    if (!create_video_workspace(scratch, parts, error))
        return false;
    const auto dot = item.file.find_last_of('.');
    const std::string stem = item.file.substr(0, dot);
    const std::string extension = dot == std::string::npos ? ".mp4" : item.file.substr(dot);
    index = 0;
    for (std::size_t part = 0; part < cuts.size(); ++part)
    {
        if (cancelled(progress))
            return fail("Cancelled");
        MediaItem segment = item;
        segment.file = stem + "_Part_" + std::to_string(part + 1) + "_of_" +
                       std::to_string(cuts.size()) + extension;
        segment.path = parts->directory + "/" + segment.file;
        parts->items.push_back(segment); // cleanup also covers incomplete files
        Output output;
        if (avformat_alloc_output_context2(&output.context, nullptr, nullptr,
                                           segment.path.c_str()) < 0 ||
            !output.context)
            return fail("Cannot create video part container");
        for (unsigned i = 0; i < input.context->nb_streams; ++i)
        {
            if (mapping[i] < 0)
                continue;
            auto *stream = avformat_new_stream(output.context, nullptr);
            if (!stream ||
                avcodec_parameters_copy(stream->codecpar, input.context->streams[i]->codecpar) < 0)
                return fail("Cannot copy video stream parameters");
            stream->codecpar->codec_tag = 0;
            stream->time_base = input.context->streams[i]->time_base;
        }
        output.context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
        if (avio_open(&output.context->pb, segment.path.c_str(), AVIO_FLAG_WRITE) < 0)
            return fail("Cannot write video part (check free space)");
        AVDictionary *options = nullptr;
        if (extension == ".mp4")
            av_dict_set(&options, "movflags", "+faststart", 0);
        code = avformat_write_header(output.context, &options);
        av_dict_free(&options);
        if (code < 0)
            return fail("Cannot write video part header");
        const auto end = part + 1 < cuts.size() ? cuts[part + 1].packet : packet_count;
        while (index < end && (code = av_read_frame(input.context, packet.value)) >= 0)
        {
            if (cancelled(progress))
                return fail("Cancelled");
            AVPacket *p = packet.value;
            const int source = p->stream_index;
            if (mapping[source] >= 0)
            {
                const auto timebase = input.context->streams[source]->time_base;
                const auto offset = av_rescale_q(cuts[part].time, AV_TIME_BASE_Q, timebase);
                if (p->pts != AV_NOPTS_VALUE)
                    p->pts -= offset;
                if (p->dts != AV_NOPTS_VALUE)
                    p->dts -= offset;
                p->stream_index = mapping[source];
                av_packet_rescale_ts(p, timebase,
                                     output.context->streams[p->stream_index]->time_base);
                p->pos = -1;
                if (av_interleaved_write_frame(output.context, p) < 0)
                    return fail("Cannot write video part (check free space)");
            }
            av_packet_unref(packet.value);
            ++index;
        }
        if (index != end)
            return fail("Video changed or could not be read while splitting");
        if (av_write_trailer(output.context) < 0 || avio_closep(&output.context->pb) < 0)
            return fail("Cannot finish video part (check free space)");
        struct stat st
        {
        };
        if (stat(segment.path.c_str(), &st) != 0 || st.st_size <= 0 ||
            static_cast<std::uint64_t>(st.st_size) > limit)
            return fail("Video part exceeds the upload limit after muxing");
        parts->items.back().size = static_cast<std::uint64_t>(st.st_size);
    }
    return true;
}
} // namespace pshare
