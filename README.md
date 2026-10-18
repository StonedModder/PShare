<p align="center">
  <img src="docs/banner.png" alt="PShare: your PS5 captures, sent the moment you take them" width="100%">
</p>

<p align="center">
  <a href="../../releases/latest"><img src="https://img.shields.io/badge/download-latest%20release-f2706b?style=flat-square" alt="Download the latest release"></a>
  <img src="https://img.shields.io/badge/platform-PS5%20homebrew-6b4fd8?style=flat-square" alt="Platform: PS5 homebrew">
  <img src="https://img.shields.io/badge/category-Media-8e5bb5?style=flat-square" alt="Installs in the Media area">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-3b3f63?style=flat-square" alt="License: GPL-3.0-or-later"></a>
</p>

<p align="center"><b>Made by StonedModder</b></p>

**PShare sends your PS5 captures where you want them, the moment you take
them.** Take a screenshot or save a clip with the Create button and PShare
sends it to Discord, Telegram, catbox.moe, OneDrive, a USB drive, a shared folder
on your PC or Mac or a dozen other places, automatically and in the background, even
after you close the app. It doubles as a media gallery: browse every capture
by game, watch clips with sound, and open anything on your phone from a QR
code.

> PShare is homebrew for jailbroken consoles. It needs an ELF loader on port
> 9021 (the usual ps5-payload-dev `elfldr`).

<p align="center">
  <img src="docs/screens/clip-player.gif" width="98%" alt="A Teardown clip playing in PShare's clip player">
</p>

<p align="center">
  <img src="docs/screens/captures.jpg" width="49%" alt="The gallery with real Teardown, Digimon World: Next Order and Dig Deep captures">
  <img src="docs/screens/capture-details.jpg" width="49%" alt="A clip's details: play, send, open in a browser">
  <img src="docs/screens/send-to.jpg" width="49%" alt="Send to: every destination, and the upload going out">
  <img src="docs/screens/auto-upload.jpg" width="49%" alt="The background uploader page">
  <img src="docs/screens/history-qr.jpg" width="49%" alt="An upload's link as a QR code">
  <img src="docs/screens/phone-browser.jpg" width="49%" alt="A Dig Deep clip playing in PShare's browser page on a phone">
  <img src="docs/screens/ps5-home-screen.jpg" width="49%" alt="PShare on a PS5 home screen">
  <img src="docs/screens/ps5-qr.jpg" width="49%" alt="PShare running on a PS5, showing an upload's QR code">
</p>

<sub>Real captures from a PS5: Teardown, Digimon World: Next Order and Dig Deep, shown in PShare's own interface (rendered by the PC preview, <code>make preview</code>) and in its browser page. The last two pictures are screenshots taken on the console. The games belong to their publishers.</sub>

## Features

- **Automatic uploads.** A small background uploader, `pshared`, notices each
  new screenshot and clip, waits until a clip has finished writing, and sends
  it. It keeps running after PShare closes.
- **Many destinations.** Discord webhooks, Telegram bots, catbox.moe,
  Litterbox, any multipart HTTP endpoint, a USB drive or console folder, PC or Mac shares over SMB 2/3, and OneDrive. Account-free hosts
  (Uguu, tmpfiles.org, temp.sh, x0.at, Gofile) are set up out of the box.
- **Smart routing.** Per-game destinations, size limits with fallbacks (a
  40 MB clip goes to catbox and Discord gets the link), and destinations that
  receive every capture as well, such as a copy on a USB stick.
- **Media gallery.** Every capture with thumbnails, clips included, filtered
  by kind or by game (picked from the games you actually have captures of,
  named even when the game is a PS4 title or no longer installed),
  newest or oldest first. A check marks what has already been sent.
- **Clip player.** Full screen with sound, using FFmpeg (VP9, H.264, HEVC,
  AV1 with Opus or AAC), including the PS5's own recordings. Its buttons
  match [VLC for PS5](https://github.com/ZiZc3/VLC).
- **Phone and PC.** While PShare is open, any capture can be opened in a
  browser on the same network with the same controls, and the whole gallery
  can be browsed, streamed and downloaded. The setup page lets you paste
  webhooks and tokens from your phone instead of typing them with a
  controller.
- **History with QR codes.** Every upload, from the app or the background,
  with its link as a QR code to open on your phone. Failed ones can be
  retried.
- **Built-in elevation.** The app carries its own
  [Lapy](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon) helper, the
  same build ProsperoStore uses, so there is nothing else to install.

## Installation

1. Download `PPSA99773.zip` from the [latest release](../../releases/latest)
   and check it against `SHA256SUMS` if you like.
2. Extract the `PPSA99773` folder to where your loader installs homebrew from
   (for example `/data/homebrew/` with ShadowMountPlus), and start
   **PShare** from the Media area of the home screen.
3. Make sure your jailbreak has the ELF loader listening on port 9021. PShare
   uses it to send its Lapy helper and to start `pshared`.
4. Open **Destinations**, fill in at least one (or use **Auto upload › Set up
   from your phone**), then take a screenshot.

`pshared.elf` is also published on its own and can be sent with any payload
sender. It reads the same `/data/pshare/config.ini`.

## Setting up destinations

Each destination is set up in **Destinations** in the app or, easier for
long webhooks and tokens, on the setup page (**Auto upload › Set up from your
phone**). Open the one you want:

<details>
<summary><b>Discord</b> (webhook)</summary>

1. In Discord, open the channel's **Edit Channel › Integrations › Webhooks**
   and choose **New Webhook**.
2. Name it if you like, then **Copy Webhook URL**.
3. Paste it into **Discord › Webhook URL** in PShare.

Discord takes files up to 20 MB on servers without boosts, 50 MB at boost
level 2 and 100 MB at level 3. Set **Size limit** to match your server and
pick where **Big files go to**: a file host (catbox by default) or OneDrive,
and Discord gets a link anyone can open; or your Mac, PC or a USB drive, and
Discord gets a note saying where the clip was saved.
</details>

<details>
<summary><b>Telegram</b> (bot)</summary>

1. Message [@BotFather](https://t.me/BotFather), send `/newbot` and follow
   the prompts. It answers with the **bot token**.
2. Add the bot to the chat, group or channel it should post in (as an admin
   for a channel).
3. Find the **chat ID**: send a message there, then open
   `https://api.telegram.org/bot<token>/getUpdates` in a browser and look for
   `"chat":{"id":...}`. Group and channel IDs start with `-100`.
4. Enter the token and chat ID under **Telegram** in PShare.

The Bot API takes files up to 50 MB.
</details>

<details>
<summary><b>catbox.moe and Litterbox</b></summary>

Both work without an account and are ready out of the box.

- **catbox.moe** keeps files permanently (up to 200 MB). To see your uploads
  in your catbox account, copy the **user hash** from your
  [catbox.moe](https://catbox.moe) account page into **User hash**.
- **Litterbox** keeps files for 1 hour to 3 days (up to 1 GB). Turn it on
  and choose **Keep files for**.
</details>

<details>
<summary><b>Uguu, tmpfiles.org, temp.sh, x0.at and Gofile</b></summary>

Account-free hosts, set up out of the box. Turn one on, or pick it as the
default or as Discord's **Big files go to**. How long each keeps files is up
to the host (the app shows it in the name).

| Host | Largest file |
| --- | --- |
| Uguu | 128 MB |
| tmpfiles.org | 100 MB |
| temp.sh | 4 GB |
| x0.at | 1 GB (bigger files are kept for less time) |
| Gofile | no stated limit |

Their **Size limit** is set to match; a host can also send bigger files on to
a **Big files go to** host of its own.
</details>

<details>
<summary><b>OneDrive</b></summary>

OneDrive needs an app registration of your own (free) and a refresh token
for it. Do this once, on a PC or Mac:

1. Sign in to [portal.azure.com](https://portal.azure.com) with your
   Microsoft account and open **Microsoft Entra ID › App registrations › New
   registration**. If the portal asks you to set up a directory or a free
   Azure account first, do that; app registrations need one.
2. Name it `PShare`, choose **Accounts in any organizational directory and
   personal Microsoft accounts**, leave the redirect URI empty and
   **Register**. Copy the **Application (client) ID** from its overview.
3. Open **Authentication**, set **Allow public client flows** to **Yes** and
   save.
4. In a terminal (on Windows type `curl.exe`, not `curl`), ask for a sign-in
   code. Replace `YOUR_CLIENT_ID` with your client ID, with no brackets or
   quotes around it:

   ```sh
   curl -d client_id=YOUR_CLIENT_ID -d "scope=Files.ReadWrite offline_access" https://login.microsoftonline.com/common/oauth2/v2.0/devicecode
   ```

5. Open [microsoft.com/devicelogin](https://microsoft.com/devicelogin), enter
   the short `user_code` from the answer, sign in and accept. Then, within
   15 minutes, replace `YOUR_DEVICE_CODE` with the long `device_code` from
   the same answer (a few hundred characters; not the `user_code`):

   ```sh
   curl -d client_id=YOUR_CLIENT_ID -d grant_type=urn:ietf:params:oauth:grant-type:device_code -d device_code=YOUR_DEVICE_CODE https://login.microsoftonline.com/common/oauth2/v2.0/token
   ```

6. Copy the `refresh_token` value (everything between its quotes). On the
   setup page, under **OneDrive**, paste the **Client ID** and **Refresh
   token**, pick a **Folder in OneDrive** (`PShare` by default) and turn it
   on. Then use **Send a test** in the app.

Captures land in `<folder>/<game>/`. PShare keeps the sign-in fresh by
itself (in `/data/pshare/onedrive-<id>.token`); if OneDrive goes unused for
about 90 days, or you change your password, repeat steps 4–6. Treat the
refresh token like a password.

If `curl` answers `AADSTS50059`, the app was registered for your directory
only: in its **Authentication** page set the supported account types to
**Accounts in any organizational directory and personal Microsoft
accounts**. `AADSTS7000218` means **Allow public client flows** is still off.

No **OneDrive** in the list? Your `config.ini` was written by an older
version: add this to the file and reopen PShare:

```ini
[destination.onedrive]
type = onedrive
name = OneDrive
path = PShare
enabled = false
```
</details>

<details>
<summary><b>Your own server</b> (custom HTTP)</summary>

Any endpoint that takes a `multipart/form-data` POST:

1. Set **Upload URL**, and **File field** to the form field that carries the
   file (`file` by default).
2. Headers such as `Authorization: Bearer <key>` and extra form fields are
   set on the setup page or in `config.ini` (`header = ...`,
   `form.<name> = ...`).

PShare reads the link from a `url` or `link` field in a JSON answer, or the
first URL in the reply.
</details>

<details>
<summary><b>USB drive or console storage</b></summary>

1. Plug in a USB drive formatted exFAT or FAT32.
2. Turn on **USB drive** in PShare and leave **Copy to** on **USB drive**, or
   choose **Console storage** (`/data/pshare/Saved`).

Captures land in `PShare/<game>/`. Turn on **Also gets every new capture** to
keep a copy of everything besides the default destination.
</details>

<details>
<summary><b>Windows PC shared folder</b> (SMB)</summary>

1. On the PC, right-click a folder › **Properties › Sharing › Share…**, add
   your account with **Read/Write**, and note the share name.
2. Find the PC's address: run `ipconfig` and read the **IPv4 Address**.
3. In PShare, open **My PC**: set **Share** to
   `smb://<address>/<share name>` (optionally followed by `/<subfolder>`),
   and enter your Windows **User name** and **Password**. Accounts that sign
   in with a Microsoft account use its email address and password.
4. Turn it on and use **Send a test**.

A Samba share on Linux or a NAS works the same way.
</details>

<details>
<summary><b>Mac shared folder</b> (SMB)</summary>

1. On the Mac, open **System Settings › General › Sharing** and turn on
   **File Sharing**.
2. Click **ⓘ** next to it, add a folder under **Shared Folders** with **+**,
   and give your account **Read & Write**.
3. Still there, open **Options…**, turn on **Windows File Sharing** for your
   account and enter its password. Without this the Mac refuses the
   console's sign-in.
4. Find the Mac's address: **System Settings › Wi-Fi** (or **Network**) ›
   **Details…** › **IP address**.
5. In PShare, open **My Mac**: set **Share** to
   `smb://<address>/<folder name>`, **User name** to the account's short name
   (its home folder's name) and **Password** to its password.
6. Turn it on and use **Send a test**.

Keep the Mac awake (**System Settings › Energy** or **Battery › Options**)
so uploads don't fail while it sleeps.

No **My Mac** in the list? Your `config.ini` was written by an older
version: set up **My PC** the same way, or add this to the file and reopen
PShare:

```ini
[destination.mac]
type = smb
name = My Mac
url = smb://192.168.1.20/Captures
enabled = false
```
</details>

## Controls

The full list is also in the app under **Settings › Controls**.

**Captures**

| Button | Action |
| --- | --- |
| ✕ | Details: play, send, open in a browser, view full screen |
| □ / △ | Send / Send to… |
| L2 / R2 | All · screenshots · clips |
| Touchpad | Choose a game and the order (also the two buttons right of the tabs) |
| L1 / R1 | Switch sections |
| Options | Menu |

**Clip player** (the same buttons as VLC for PS5 wherever the two apps overlap)

| Button | Action |
| --- | --- |
| ✕ | Play / pause |
| ← / → | Back / forward 10 s |
| L2 / R2 | Back / forward 30 s |
| L1 / R1 | Previous / next clip |
| □ | Send to the default destination |
| △ | Send to… |
| ○ | Close |

**Browser page** (a capture › Open in a browser): a controller connected to
the PC uses the console's buttons. On a keyboard: Space play/pause, ← / →
10 s, `[` / `]` 30 s, `S` send, `T` send to…, `,` / `.` newer / older, Esc
back. On a phone, the buttons under the picture.

## Configuration

Everything is editable in the app or from your phone. The file behind it is
`/data/pshare/config.ini`:

```ini
[general]
auto_upload = true
default_destination = discord
also_send = usb

[destination.discord]
type = discord
url = https://discord.com/api/webhooks/<id>/<token>
message = {type} from **{game}** - {date} {time}
max_mb = 20
fallback = catbox

[title_destinations]
PPSA01234 = telegram
```

Every key is described in [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

## Ports

| Port | Who | What |
| --- | --- | --- |
| 9021 | your jailbreak | the ELF loader PShare sends `lapy.elf` and `pshared.elf` to |
| 9077 | pshared, localhost only | the control line the app uses |
| 9078 | pshared, local network | the browser page and gallery, open only while PShare runs or setup is on |

## Building

Linux or WSL with clang/lld 18+, ninja, make and python3:

```bash
make deps     # payload SDK, PacBrew (libcurl, FFmpeg, libsmb2), ps5-opengl
make          # dist/PPSA99773/, dist/PPSA99773.zip, dist/pshared.elf
make test     # unit tests (ASan/UBSan), Lapy client tests, tooling tests
make preview  # every page rendered on the PC into build/preview/
```

See [docs/BUILDING.md](docs/BUILDING.md) for the full requirements and
targets, and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the app and
the uploader work together.

## FAQ

**Why is it in the Media area instead of with my games?**
PShare is a media app. To build it as a game instead, run
`make init TITLE_ID=PPSA99773 APP_NAME="PShare" CONTENT_SUFFIX=PSHARE0000000001 APP_CATEGORY=game`
before `make`.

**The app says "No file access in the app".**
Elevation failed: check that the ELF loader is running on port 9021, then
restart PShare. The log line `[PShare] elevation …` says which step stopped.

**Uploads stop when I close the app.**
They should not: check **Auto upload** shows the uploader as running, and
that **Send uploads through it** is on.

**Clips play in the app but not in the browser while PShare is closed.**
After some console restarts the console hands jailbreak payloads, the
background uploader included, nothing but zeros when they read a clip, while
the app still reads it properly. While PShare is open, the app passes the clip
to the uploader, so the browser page, downloads and uploads all work. Keep
PShare open, or reload the jailbreak.

**Discord does not accept my clip.**
Without server boosts Discord takes files up to 20 MB (50 MB at boost level
2, 100 MB at level 3). Set `max_mb` to your server's limit and keep a
`fallback` so bigger clips go to a file host and Discord gets the link.

## Contributing

Bug reports and pull requests are welcome. Please read
[CONTRIBUTING.md](CONTRIBUTING.md) first, and remove webhook URLs and tokens
from any log you attach.

## Credits

- [VLC for PS5](https://github.com/ZiZc3/VLC) by **ZiZc3**: its finding that
  the PS5's recordings carry an Opus header FFmpeg refuses is why clips play
  with sound here (`fix_ps5_opus_header` in `src/app/video.cpp`), and its
  player buttons are the ones PShare follows.
- [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
  [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
  [ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore) and
  [PS5-Lapy-JB-Daemon](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon)
  by BlackBearReloaded: the build pipeline, the UI kit, the elevation client
  and the elevation helper.
- [Porkfolio](https://github.com/StonedModder/Porkfolio), for where the PS5
  keeps its captures.
- [ps5-payload-dev](https://github.com/ps5-payload-dev) for the payload SDK
  and PacBrew, and FFmpeg, libcurl and libsmb2.

Full details and licences are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

PShare is licensed under the [GNU General Public License v3.0 or later](LICENSE).
Third-party components keep their own licences.

PlayStation and PS5 are trademarks of Sony Interactive Entertainment. Discord,
Telegram and the other services named here belong to their owners. PShare is
not affiliated with or endorsed by any of them.
