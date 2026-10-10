// PShare - Copying a capture to a shared folder on a PC (SMB 2/3, libsmb2).
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console and the payload builds link PacBrew's libsmb2 (PSHARE_HAVE_SMB2);
// the PC tests and preview have no libsmb2 and say so instead.

#include "pshare/core/local_copy.hpp"

#include "pshare/core/text.hpp"

#if defined(PSHARE_HAVE_SMB2)
#include <cstdint>
#include <fcntl.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <unistd.h>
#include <vector>
#endif

namespace pshare
{

#if defined(PSHARE_HAVE_SMB2)
namespace
{
// Closes what is open whatever way the copy ends.
struct Session
{
    smb2_context *smb2 = nullptr;
    smb2fh *file = nullptr;
    bool connected = false;
    ~Session()
    {
        if (file != nullptr)
            smb2_close(smb2, file);
        if (connected)
            smb2_disconnect_share(smb2);
        if (smb2 != nullptr)
            smb2_destroy_context(smb2);
    }
};
} // namespace

UploadOutcome copy_to_share(const Destination &destination, const MediaItem &item,
                            const std::string &game, UploadProgress *progress)
{
    UploadOutcome outcome;
    outcome.destination = destination.id;
    outcome.attempts = 1;
    SmbAddress address;
    if (!parse_smb_url(destination.url, &address, &outcome.error))
        return outcome;
    // The fields win over what the address carries.
    const std::string user = !destination.username.empty() ? destination.username : address.user;
    const std::string password = !destination.token.empty() ? destination.token : address.password;

    Session session;
    session.smb2 = smb2_init_context();
    if (session.smb2 == nullptr)
    {
        outcome.error = "Cannot start SMB";
        return outcome;
    }
    smb2_set_security_mode(session.smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(session.smb2, 30);
    if (!user.empty())
        smb2_set_user(session.smb2, user.c_str());
    if (!password.empty())
        smb2_set_password(session.smb2, password.c_str());
    if (smb2_connect_share(session.smb2, address.host.c_str(), address.share.c_str(),
                           user.empty() ? nullptr : user.c_str()) < 0)
    {
        outcome.error = std::string("Cannot open the share: ") + smb2_get_error(session.smb2);
        return outcome;
    }
    session.connected = true;

    // <folder>/<game>/, made one level at a time (mkdir of an existing one fails; fine).
    std::string folder = address.folder;
    std::string built;
    for (const std::string &part : split(folder, '/'))
    {
        if (part.empty())
            continue;
        built += (built.empty() ? "" : "/") + part;
        smb2_mkdir(session.smb2, built.c_str());
    }
    built += (built.empty() ? "" : "/") + safe_name(game);
    smb2_mkdir(session.smb2, built.c_str());
    const std::string target = built + "/" + item.file;
    const std::string partial = target + ".part";

    const int input = open(item.path.c_str(), O_RDONLY);
    if (input < 0)
    {
        outcome.error = "Cannot read the capture";
        return outcome;
    }
    session.file = smb2_open(session.smb2, partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (session.file == nullptr)
    {
        outcome.error = std::string("Cannot write on the share: ") + smb2_get_error(session.smb2);
        close(input);
        return outcome;
    }
    if (progress != nullptr)
    {
        progress->total = item.size;
        progress->sent = 0;
        progress->stage = 1;
    }
    std::uint32_t chunk = smb2_get_max_write_size(session.smb2);
    if (chunk == 0 || chunk > (1u << 20))
        chunk = 1u << 20;
    std::vector<std::uint8_t> buffer(chunk);
    std::uint64_t copied = 0;
    bool ok = true;
    for (;;)
    {
        if (progress != nullptr && progress->cancel.load())
        {
            outcome.error = "Cancelled";
            ok = false;
            break;
        }
        const ssize_t got = read(input, buffer.data(), buffer.size());
        if (got == 0)
            break;
        if (got < 0)
        {
            outcome.error = "Reading the capture failed";
            ok = false;
            break;
        }
        std::size_t done = 0;
        while (ok && done < static_cast<std::size_t>(got))
        {
            const int put =
                smb2_write(session.smb2, session.file, buffer.data() + done,
                           static_cast<std::uint32_t>(static_cast<std::size_t>(got) - done));
            if (put <= 0)
            {
                outcome.error =
                    std::string("Writing to the share failed: ") + smb2_get_error(session.smb2);
                ok = false;
                break;
            }
            done += static_cast<std::size_t>(put);
        }
        if (!ok)
            break;
        copied += static_cast<std::uint64_t>(got);
        if (progress != nullptr)
            progress->sent = copied;
    }
    close(input);
    smb2_close(session.smb2, session.file);
    session.file = nullptr;
    if (ok)
    {
        smb2_unlink(session.smb2, target.c_str()); // an older copy is replaced
        if (smb2_rename(session.smb2, partial.c_str(), target.c_str()) < 0)
        {
            outcome.error = std::string("Cannot finish the copy: ") + smb2_get_error(session.smb2);
            ok = false;
        }
    }
    if (!ok)
    {
        smb2_unlink(session.smb2, partial.c_str());
        return outcome;
    }
    outcome.ok = true;
    outcome.link = "\\\\" + address.host + "\\" + address.share + "\\" + target;
    for (char &c : outcome.link)
        if (c == '/')
            c = '\\';
    return outcome;
}

#else

UploadOutcome copy_to_share(const Destination &destination, const MediaItem &, const std::string &,
                            UploadProgress *)
{
    UploadOutcome outcome;
    outcome.destination = destination.id;
    outcome.error = "PC shares need a console build (libsmb2)";
    return outcome;
}

#endif

} // namespace pshare
