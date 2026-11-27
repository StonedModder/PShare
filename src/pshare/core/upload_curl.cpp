// PShare - Performing an upload plan with libcurl.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Three builds share this file:
//   PSHARE_CONSOLE  the native title: console_curl.c supplies the resolver,
//                   the certificate path and the SO_NBIO socket option;
//   PSHARE_PAYLOAD  pshared, a payload: the payload SDK's libc resolves names
//                   itself; certificates come from /system, sockets get SO_NBIO;
//   (neither)       the PC tests, against the system's libcurl.
//
// Files are streamed from disk through curl's MIME API (multipart) or a read
// callback (a raw body, whole or a slice), so a 1 GB clip costs no memory.

#include "pshare/core/upload.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <fcntl.h>
#include <mutex>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(PSHARE_CONSOLE)
#include "console_curl.h"
#endif

namespace pshare
{
namespace
{
constexpr std::size_t kMaxResponse = 256 * 1024;

#if !defined(PSHARE_CONSOLE)
const char *ca_file()
{
    static const char *const candidates[] = {
        "/system/common/cert/CA_LIST.cer",    // a payload sees the console's root
        "/etc/ssl/certs/ca-certificates.crt", // Debian, Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora
        "/etc/ssl/cert.pem",                  // BSDs, macOS
    };
    for (const char *path : candidates)
    {
        struct stat info
        {
        };
        if (stat(path, &info) == 0 && S_ISREG(info.st_mode))
            return path;
    }
    return nullptr;
}
#endif

#if defined(PSHARE_PAYLOAD)
int on_socket(void *, curl_socket_t socket, curlsocktype)
{
    // The console's own non-blocking option; without it a finished request can
    // wait for the server to close the connection (ps5-native-app-boilerplate's docs/CURL.md).
    const int on = 1;
    (void)setsockopt(socket, SOL_SOCKET, 0x1200, &on, sizeof(on));
    return CURL_SOCKOPT_OK;
}
#endif

void setup_handle(CURL *easy)
{
#if defined(PSHARE_CONSOLE)
    console_curl_setup(easy);
#else
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    if (const char *ca = ca_file())
        curl_easy_setopt(easy, CURLOPT_CAINFO, ca);
#if defined(PSHARE_PAYLOAD)
    curl_easy_setopt(easy, CURLOPT_SOCKOPTFUNCTION, on_socket);
#endif
#endif
}

struct Sink
{
    std::string body;
};

size_t on_body(char *data, size_t size, size_t count, void *user)
{
    Sink *sink = static_cast<Sink *>(user);
    const size_t bytes = size * count;
    if (sink->body.size() < kMaxResponse)
        sink->body.append(data, std::min(bytes, kMaxResponse - sink->body.size()));
    return bytes;
}

struct ProgressSink
{
    UploadProgress *progress = nullptr;
    std::uint64_t offset = 0; // a slice: where it starts in the file
    bool slice = false;       // a slice: the total stays the whole file's
};

int on_progress(void *user, curl_off_t, curl_off_t, curl_off_t upload_total, curl_off_t uploaded)
{
    const ProgressSink *sink = static_cast<const ProgressSink *>(user);
    UploadProgress *progress = sink->progress;
    if (progress == nullptr)
        return 0;
    if (upload_total > 0 && !sink->slice)
        progress->total = static_cast<std::uint64_t>(upload_total);
    progress->sent = sink->offset + static_cast<std::uint64_t>(uploaded);
    return progress->cancel.load() ? 1 : 0; // non-zero aborts the transfer
}

struct RawBody
{
    int file = -1;
    std::uint64_t at = 0;
    std::uint64_t left = 0;
};

size_t on_read(char *buffer, size_t size, size_t count, void *user)
{
    RawBody *raw = static_cast<RawBody *>(user);
    const size_t wanted = static_cast<size_t>(std::min<std::uint64_t>(size * count, raw->left));
    const ssize_t got = pread(raw->file, buffer, wanted, static_cast<off_t>(raw->at));
    if (got < 0)
        return CURL_READFUNC_ABORT;
    raw->at += static_cast<std::uint64_t>(got);
    raw->left -= static_cast<std::uint64_t>(got);
    return static_cast<size_t>(got);
}
} // namespace

void upload_global_init()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpResult perform_request(const HttpRequest &request, UploadProgress *progress,
                           int timeout_seconds)
{
    upload_global_init();
    HttpResult result;
    CURL *easy = curl_easy_init();
    if (easy == nullptr)
    {
        result.error = "curl could not start";
        return result;
    }
    setup_handle(easy);
    char error_text[CURL_ERROR_SIZE] = {};
    Sink sink;
    curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_text);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "PShare/1.0 (PS5 homebrew)");
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L); // a redirected POST would lose its body
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L); // covers the name lookup
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    // A stalled uplink: under 1 KB/s for a minute ends the attempt.
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(easy, CURLOPT_UPLOAD_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    ProgressSink progress_sink;
    progress_sink.progress = progress;
    if (request.body == BodyKind::raw_file && request.raw_length > 0)
    {
        progress_sink.offset = request.raw_offset;
        progress_sink.slice = true;
    }
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &progress_sink);

    curl_slist *headers = nullptr;
    for (const std::string &header : request.headers)
        headers = curl_slist_append(headers, header.c_str());
    // No "Expect: 100-continue" pause before large bodies.
    headers = curl_slist_append(headers, "Expect:");

    curl_mime *mime = nullptr;
    RawBody raw;
    if (request.body == BodyKind::multipart)
    {
        mime = curl_mime_init(easy);
        for (const FormPart &part : request.parts)
        {
            curl_mimepart *field = curl_mime_addpart(mime);
            curl_mime_name(field, part.name.c_str());
            if (part.is_file())
            {
                if (curl_mime_filedata(field, part.file_path.c_str()) != CURLE_OK)
                {
                    result.error = "Cannot read " + part.file_path;
                    curl_mime_free(mime);
                    curl_slist_free_all(headers);
                    curl_easy_cleanup(easy);
                    return result;
                }
                curl_mime_filename(field, part.file_name.c_str());
                if (!part.content_type.empty())
                    curl_mime_type(field, part.content_type.c_str());
            }
            else
            {
                curl_mime_data(field, part.value.c_str(), CURL_ZERO_TERMINATED);
            }
        }
        curl_easy_setopt(easy, CURLOPT_MIMEPOST, mime);
    }
    else if (request.body == BodyKind::raw_file)
    {
        raw.file = open(request.raw_path.c_str(), O_RDONLY);
        struct stat info
        {
        };
        const std::uint64_t size = raw.file >= 0 && fstat(raw.file, &info) == 0
                                       ? static_cast<std::uint64_t>(info.st_size)
                                       : 0;
        raw.at = request.raw_offset;
        raw.left = request.raw_length > 0 ? request.raw_length : size;
        if (raw.file < 0 || raw.at + raw.left > size)
        {
            if (raw.file >= 0)
                close(raw.file);
            result.error = "Cannot read " + request.raw_path;
            curl_slist_free_all(headers);
            curl_easy_cleanup(easy);
            return result;
        }
        headers = curl_slist_append(headers, ("Content-Type: " + request.raw_type).c_str());
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(raw.left));
        curl_easy_setopt(easy, CURLOPT_READFUNCTION, on_read);
        curl_easy_setopt(easy, CURLOPT_READDATA, &raw);
    }
    else if (request.body == BodyKind::text)
    {
        headers = curl_slist_append(headers, ("Content-Type: " + request.raw_type).c_str());
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.text.c_str());
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(request.text.size()));
    }
    else
    {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, 0L);
    }
    // PUT and the like still send the body set up above.
    if (!request.method.empty())
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

    const CURLcode code = curl_easy_perform(easy);
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status);
    if (code == CURLE_OK)
    {
        result.transport_ok = true;
    }
    else
    {
        result.error = error_text[0] != '\0' ? error_text : curl_easy_strerror(code);
        if (code == CURLE_ABORTED_BY_CALLBACK)
            result.error = "Cancelled";
        result.error += " (curl " + std::to_string(static_cast<int>(code)) + ")";
    }
    result.body = std::move(sink.body);
    if (raw.file >= 0)
        close(raw.file);
    if (mime != nullptr)
        curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    return result;
}

} // namespace pshare
