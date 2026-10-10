// PShare - "Sending" without the internet: a copy on a drive or on a PC share.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two destination types never touch HTTP:
//   folder  copies to <drive>/PShare/<game>/<file> on a USB stick or the
//           extended storage ("usb" picks the first drive that is there), or
//           to any absolute folder;
//   smb     writes to a shared folder on a PC with libsmb2 (SMB 2/3, the
//           protocol of a Windows share and of Samba).
// Both write under a temporary name and rename at the end, so a cancelled or
// broken copy never leaves a half file that looks finished.

#pragma once

#include "pshare/core/config.hpp"
#include "pshare/core/media.hpp"
#include "pshare/core/upload.hpp"

#include <string>
#include <string_view>

namespace pshare
{

// A folder name that every file system takes: no / \ : * ? " < > |, no
// trailing dots or spaces, never empty.
std::string safe_name(std::string_view text);

// The first drive among paths.usb_roots that is mounted and writable, or empty.
std::string find_drive(const Paths &paths);

// Where a capture lands for a folder destination: "<root>/PShare/<game>".
// Empty with a reason when no drive is there.
std::string folder_target(const Destination &destination, const Paths &paths,
                          const std::string &game, std::string *error);

UploadOutcome copy_to_folder(const Destination &destination, const Paths &paths,
                             const MediaItem &item, const std::string &game,
                             UploadProgress *progress);

// Implemented in upload_smb.cpp (libsmb2); builds without libsmb2 report that
// the PC share is not available.
UploadOutcome copy_to_share(const Destination &destination, const MediaItem &item,
                            const std::string &game, UploadProgress *progress);

} // namespace pshare
