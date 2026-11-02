// Real-media integration test driver; never contacts Telegram.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pshare/core/video_split.hpp"
#include "pshare/core/video_convert.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>

using namespace pshare;
namespace fs = std::filesystem;
struct Fake
{
    int calls = 0;
    std::string mode;
    std::vector<std::string> files;
};
HttpResult transport(const HttpRequest &request, UploadProgress *progress, void *user)
{
    auto &fake = *static_cast<Fake *>(user);
    ++fake.calls;
    bool caption = false;
    bool numbered = false;
    for (const auto &part : request.parts)
    {
        if (part.name == "caption")
            caption =
                part.value.find("Part " + std::to_string(fake.calls) + "/") != std::string::npos;
        if (part.is_file())
        {
            assert(
                fs::file_size(part.file_path) <=
                ((fake.mode == "cloudlimit" || fake.mode == "convert-http") ? 50000000 : 1048576));
            numbered = part.file_name.find("_Part_") != std::string::npos;
            if (fake.mode.find("convert-") == 0)
            {
                if (fake.mode != "convert-http")
                    assert(request.url.find("sendVideo") != std::string::npos);
                assert(fs::path(part.file_name).extension() == ".mp4");
                // Both the converted MP4 and any split parts must be removed.
                const auto directory = fs::path(part.file_path).parent_path();
                if (numbered)
                {
                    for (const auto &entry : fs::directory_iterator(directory.parent_path()))
                        if (entry.is_directory() && entry.path() != directory)
                            for (const auto &file : fs::directory_iterator(entry.path()))
                                if (file.path().extension() == ".mp4")
                                    fake.files.push_back(file.path().string());
                }
            }
            else
                assert(numbered);
            fake.files.push_back(part.file_path);
        }
    }
    assert(!numbered || caption);
    if (fake.mode == "cancel" || fake.mode == "convert-cancel")
        progress->cancel = true;
    if (fake.mode == "failure" && fake.calls == 2)
        return {true, 400, R"({"ok":false,"description":"test rejection"})", {}};
    return {true, 200, R"({"ok":true})", {}};
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    MediaItem item;
    assert(describe_media(argv[1], &item));
    const auto original_size = fs::file_size(item.path);
    const std::string mode = argv[3];
    UploadProgress progress;
    if (mode == "convert" || mode == "convert-precancel" || mode == "convert-midcancel" ||
        mode == "convert-invalid")
    {
        if (mode == "convert-precancel")
            progress.cancel = true;
        std::string directory;
        {
            VideoParts output;
            std::string error;
            std::atomic<bool> done{false};
            std::thread cancellation;
            if (mode == "convert-midcancel")
                cancellation = std::thread(
                    [&]
                    {
                        while (!done.load() && progress.sent.load() == 0)
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        if (!done.load())
                            progress.cancel = true;
                    });
            const bool ok = convert_webm(item, argv[2], &progress, &output, &error);
            done = true;
            if (cancellation.joinable())
                cancellation.join();
            directory = output.directory;
            if (mode == "convert-midcancel")
                assert(!ok && error == "Cancelled");
            if (mode != "convert")
                assert(!ok && !error.empty());
            else
            {
                assert(ok && output.items.size() == 1);
                const auto path = fs::path(argv[2]) / output.items.front().file;
                fs::copy_file(output.items.front().path, path,
                              fs::copy_options::overwrite_existing);
                std::cout << path.string() << '\n';
            }
        }
        assert(directory.empty() || !fs::exists(directory));
    }
    else if (mode == "convert-folder")
    {
        Config config;
        config.convert_webm = true;
        Destination destination;
        destination.type = DestinationType::folder;
        destination.id = "folder";
        destination.path = argv[2];
        const auto result = upload_capture_with(config, destination, item, "Game", &progress,
                                                nullptr, nullptr, false);
        assert(result.ok && progress.stage == 0);
        bool found = false;
        for (const auto &file : fs::recursive_directory_iterator(argv[2]))
            if (file.path().extension() == ".mp4")
            {
                std::cout << file.path().string() << '\n';
                found = true;
            }
        assert(found);
    }
    else if (mode == "split" || mode == "oversized" || mode == "precancel" || mode == "invalid")
    {
        if (mode == "precancel")
            progress.cancel = true;
        std::string directory;
        {
            VideoParts parts;
            std::string error;
            const bool ok = split_video(item, 500000, argv[2], &progress, &parts, &error);
            directory = parts.directory;
            if (mode != "split")
            {
                assert(!ok && !error.empty());
                if (mode == "oversized")
                    assert(error.find("keyframe interval") != std::string::npos);
                if (mode == "precancel")
                    assert(error == "Cancelled");
            }
            else
            {
                assert(ok && parts.items.size() > 1);
                for (const auto &part : parts.items)
                {
                    assert(part.size <= 500000);
                    const auto output = fs::path(argv[2]) / part.file;
                    fs::copy_file(part.path, output, fs::copy_options::overwrite_existing);
                    std::cout << output.string() << '\n';
                }
            }
        }
        assert(directory.empty() || !fs::exists(directory));
    }
    else
    {
        Config config;
        Destination destination;
        destination.id = "telegram";
        destination.type = DestinationType::telegram;
        destination.token = "test";
        destination.chat_id = "1";
        destination.max_mb = mode == "cloudlimit" ? 100 : 1;
        config.convert_webm = mode.find("convert-") == 0;
        if (mode == "convert-http")
        {
            destination.type = DestinationType::http;
            destination.url = "https://example.test/upload";
            destination.max_mb = 0;
        }
        Fake fake{0, mode, {}};
        const auto result = upload_capture_with(config, destination, item, "Game {file}", &progress,
                                                transport, &fake, false);
        assert(fake.calls > 0);
        assert(progress.stage == 0);
        if (mode == "success" || mode == "cloudlimit" || mode == "convert-upload")
            assert(result.ok && fake.calls > 1);
        else if (mode == "convert-http")
            assert(result.ok && fake.calls == 1);
        else if (mode == "failure")
            assert(!result.ok && fake.calls == 2 &&
                   result.error.find("earlier parts") != std::string::npos);
        else
            assert(!result.ok && fake.calls == 1 &&
                   result.error.find("Cancelled") != std::string::npos);
        for (const auto &file : fake.files)
            assert(!fs::exists(file) && !fs::exists(fs::path(file).parent_path()));
    }
    assert(fs::file_size(item.path) == original_size);
}
