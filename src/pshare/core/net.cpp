// PShare - Local sockets: the daemon's control line, the ELF loader, notifications.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/net.hpp"

#include "pshare/core/config.hpp"
#include "pshare/core/text.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(PSHARE_CONSOLE) || defined(PSHARE_PAYLOAD)
extern "C" int sceKernelSendNotificationRequest(int device, void *request, std::size_t size,
                                                int blocking);
#endif
#if defined(PSHARE_CONSOLE)
extern "C"
{
    int sceNetSocket(const char *name, int domain, int type, int protocol);
    int sceNetConnect(int socket, const void *address, std::uint32_t address_length);
    int sceNetSend(int socket, const void *data, std::size_t length, int flags);
    int sceNetRecv(int socket, void *data, std::size_t length, int flags);
    int sceNetSetsockopt(int socket, int level, int option, const void *value, std::uint32_t size);
    int sceNetShutdown(int socket, int how);
    int sceNetSocketClose(int socket);
}
#endif

namespace pshare
{
namespace
{
sockaddr_in loopback(int port)
{
    sockaddr_in address{};
#if defined(__FreeBSD__) || defined(PSHARE_CONSOLE) || defined(PSHARE_PAYLOAD)
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(0x7f000001);
    return address;
}

// A loopback connection. The app is a sandboxed title until Lapy is done (and
// stays one when Lapy is not there), so on the console it uses libSceNet, the
// same calls the Lapy client streams its helper with; the payload and the PC
// use BSD sockets.
class Link
{
  public:
    bool open(int port, int timeout_ms)
    {
#if defined(PSHARE_CONSOLE)
        socket_ = sceNetSocket("pshare_link", 2, 1, 0);
        if (socket_ < 0)
            return false;
        const int timeout_us = timeout_ms * 1000;
        for (const int option : {0x1105, 0x1106, 0x1109})
            sceNetSetsockopt(socket_, 0xffff, option, &timeout_us, sizeof(timeout_us));
        struct
        {
            std::uint8_t length;
            std::uint8_t family;
            std::uint16_t port;
            std::uint32_t address;
            std::uint16_t virtual_port;
            std::uint8_t zero[6];
        } endpoint{16, 2, htons(static_cast<std::uint16_t>(port)), htonl(0x7f000001), 0, {0}};
        if (sceNetConnect(socket_, &endpoint, sizeof(endpoint)) < 0)
        {
            close_link();
            return false;
        }
        return true;
#else
        socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0)
            return false;
        socket_timeouts(socket_, timeout_ms);
        const sockaddr_in address = loopback(port);
        if (connect(socket_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
        {
            close_link();
            return false;
        }
        return true;
#endif
    }
    ~Link()
    {
        close_link();
    }
    bool send_bytes(std::string_view data)
    {
        while (!data.empty())
        {
#if defined(PSHARE_CONSOLE)
            const int sent = sceNetSend(socket_, data.data(), data.size(), 0);
            if (sent <= 0)
                return false;
#else
            const ssize_t sent = send(socket_, data.data(), data.size(), 0);
            if (sent <= 0)
            {
                if (sent < 0 && errno == EINTR)
                    continue;
                return false;
            }
#endif
            data.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }
    long receive(char *buffer, std::size_t size)
    {
#if defined(PSHARE_CONSOLE)
        return sceNetRecv(socket_, buffer, size, 0);
#else
        return static_cast<long>(recv(socket_, buffer, size, 0));
#endif
    }
    void finish_sending()
    {
#if defined(PSHARE_CONSOLE)
        sceNetShutdown(socket_, 1);
#else
        shutdown(socket_, SHUT_WR);
#endif
    }

  private:
    void close_link()
    {
        if (socket_ < 0)
            return;
#if defined(PSHARE_CONSOLE)
        sceNetSocketClose(socket_);
#else
        close(socket_);
#endif
        socket_ = -1;
    }
    int socket_ = -1;
};
} // namespace

std::string DaemonStatus::serialize() const
{
    std::string out;
    out += "version=" + version + "\n";
    out += "state=" + state + "\n";
    out += "uploaded=" + std::to_string(uploaded) + "\n";
    out += "failed=" + std::to_string(failed) + "\n";
    out += "queued=" + std::to_string(queued) + "\n";
    out += "started=" + std::to_string(started) + "\n";
    out += "last_file=" + last_file + "\n";
    out += "last_link=" + last_link + "\n";
    out += "last_error=" + last_error + "\n";
    out += "build=" + build + "\n";
    out += "setup_port=" + std::to_string(setup_port) + "\n";
    out += "watch_errno=" + std::to_string(watch_errno) + "\n";
    out += "destination=" + destination + "\n";
    out += "ip=" + ip + "\n";
    out += "job=" + std::to_string(job) + "\n";
    return out;
}

DaemonStatus DaemonStatus::parse(std::string_view text)
{
    DaemonStatus status;
    status.reachable = true;
    for (const std::string &line : split(text, '\n'))
    {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "version")
            status.version = value;
        else if (key == "state")
            status.state = value;
        else if (key == "uploaded")
            status.uploaded = static_cast<int>(parse_int(value, 0));
        else if (key == "failed")
            status.failed = static_cast<int>(parse_int(value, 0));
        else if (key == "queued")
            status.queued = static_cast<int>(parse_int(value, 0));
        else if (key == "started")
            status.started = parse_int(value, 0);
        else if (key == "last_file")
            status.last_file = value;
        else if (key == "last_link")
            status.last_link = value;
        else if (key == "last_error")
            status.last_error = value;
        else if (key == "build")
            status.build = value;
        else if (key == "setup_port")
            status.setup_port = static_cast<int>(parse_int(value, 0));
        else if (key == "watch_errno")
            status.watch_errno = static_cast<int>(parse_int(value, 0));
        else if (key == "destination")
            status.destination = value;
        else if (key == "stage")
            status.stage = parse_int(value, 0);
        else if (key == "sent")
            status.sent = static_cast<std::uint64_t>(parse_int(value, 0));
        else if (key == "total")
            status.total = static_cast<std::uint64_t>(parse_int(value, 0));
        else if (key == "job")
            status.job = static_cast<int>(parse_int(value, 0));
        else if (key == "ip")
            status.ip = value;
    }
    return status;
}

void socket_timeouts(int socket_fd, int milliseconds)
{
    timeval value{};
    value.tv_sec = milliseconds / 1000;
    value.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}

bool send_all(int socket_fd, std::string_view data)
{
    while (!data.empty())
    {
        const ssize_t sent = send(socket_fd, data.data(), data.size(), 0);
        if (sent <= 0)
        {
            if (sent < 0 && errno == EINTR)
                continue;
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

bool daemon_command(std::string_view command, std::string *reply, int timeout_ms, int port)
{
    Link link;
    if (!link.open(port > 0 ? port : kControlPort, timeout_ms))
        return false;
    std::string line(command);
    line.push_back('\n');
    if (!link.send_bytes(line))
        return false;
    std::string out;
    char buffer[2048];
    for (;;)
    {
        const long got = link.receive(buffer, sizeof(buffer));
        if (got <= 0)
            break;
        out.append(buffer, static_cast<std::size_t>(got));
        if (out.size() > (64u << 10))
            break;
    }
    if (out.empty())
        return false;
    if (reply)
        *reply = std::move(out);
    return true;
}

DaemonStatus daemon_status(int port, bool from_app)
{
    std::string reply;
    if (!daemon_command(from_app ? "STATUS app" : "STATUS", &reply, 800, port))
        return {};
    return DaemonStatus::parse(reply);
}

bool relay_once(int wait_ms, const RelayRead &read, int port)
{
    Link link;
    if (!link.open(port > 0 ? port : kControlPort, wait_ms + 3000))
        return false;
    if (!link.send_bytes("RELAY\n"))
        return false;
    std::string line;
    char c = 0;
    while (line.size() < 4096)
    {
        if (link.receive(&c, 1) <= 0)
            return false;
        if (c == '\n')
            break;
        line.push_back(c);
    }
    if (line == "none")
        return true;
    // "<first>\t<last>\t<path>"
    const std::size_t a = line.find('\t');
    const std::size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos)
        return true;
    const long long first = parse_int(line.substr(0, a), -1);
    const long long last = parse_int(line.substr(a + 1, b - a - 1), -1);
    if (first < 0 || last < first)
        return true;
    read(line.substr(b + 1), static_cast<std::uint64_t>(first), static_cast<std::uint64_t>(last),
         [&](std::string_view data) { return link.send_bytes(data); });
    link.finish_sending();
    return true;
}

std::string elf_build_id(std::string_view elf)
{
    // This search string is in the ELF too, followed by nothing: skip it.
    constexpr std::string_view kMarker = "PSHARE_BUILD=";
    for (std::size_t at = elf.find(kMarker); at != std::string_view::npos;
         at = elf.find(kMarker, at + 1))
    {
        std::string out;
        for (std::size_t i = at + kMarker.size(); i < elf.size() && out.size() < 64; ++i)
        {
            const char c = elf[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
                break;
            out.push_back(c);
        }
        if (!out.empty())
            return out;
    }
    return {};
}

bool send_to_loader(const std::string &elf, std::string *error, int port)
{
    if (elf.size() < 64 || elf.compare(0, 4,
                                       "\x7f"
                                       "ELF") != 0)
    {
        if (error)
            *error = "pshared.elf is missing from the app folder";
        return false;
    }
    Link link;
    if (!link.open(port > 0 ? port : kLoaderPort, 3000))
    {
        if (error)
            *error = "No payload loader is listening on port 9021";
        return false;
    }
    const bool ok = link.send_bytes(elf);
    link.finish_sending();
    if (!ok && error)
        *error = "The payload loader closed the connection";
    return ok;
}

int listen_tcp(std::uint32_t address_value, int port, int backlog)
{
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return -1;
    const int on = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in address = loopback(port);
    address.sin_addr.s_addr = htonl(address_value);
    if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(socket_fd, backlog) != 0)
    {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

std::string local_ip_address()
{
    // A UDP "connection" sends nothing; it only makes the stack pick the
    // interface (and address) that routes to the outside.
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return {};
    sockaddr_in outside = loopback(53);
    outside.sin_addr.s_addr = htonl(0x01010101); // 1.1.1.1
    std::string out;
    if (connect(socket_fd, reinterpret_cast<const sockaddr *>(&outside), sizeof(outside)) == 0)
    {
        sockaddr_in mine{};
        socklen_t length = sizeof(mine);
        if (getsockname(socket_fd, reinterpret_cast<sockaddr *>(&mine), &length) == 0)
        {
            char text[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &mine.sin_addr, text, sizeof(text)) != nullptr &&
                std::strcmp(text, "0.0.0.0") != 0)
                out = text;
        }
    }
    close(socket_fd);
    return out;
}

void notify(std::string_view text)
{
#if defined(PSHARE_CONSOLE) || defined(PSHARE_PAYLOAD)
    struct Request
    {
        char reserved[45];
        char message[3075];
    } request{};
    std::snprintf(request.message, sizeof(request.message), "%.*s", static_cast<int>(text.size()),
                  text.data());
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
#else
    std::printf("[notify] %.*s\n", static_cast<int>(text.size()), text.data());
#endif
}

} // namespace pshare
