# Configuration

Everything lives in `/data/pshare/config.ini`, read by both the app and
pshared. It is created with comments on first start. You can change it:

* in the app (**Destinations** and **Settings**);
* from a phone or PC: **Auto upload › Set up from your phone** shows a QR
  code for the settings page. Saved webhooks, tokens and passwords are not
  shown there; leave a field empty to keep what is saved;
* by hand over FTP. The app keeps your comments when it saves. After editing
  by hand, reopen PShare or use **Auto upload › Turn off / Turn on** so
  pshared reads it again.

## `[general]`

| Key | Default | |
| --- | --- | --- |
| `auto_upload` | `true` | send new captures as soon as they are saved, while pshared runs |
| `upload_screenshots` | `true` | include screenshots |
| `upload_videos` | `true` | include clips |
| `upload_existing` | `false` | also send captures that existed before pshared first started |
| `default_destination` | `discord` | the destination id used when no game rule applies |
| `also_send` | | comma-separated destination ids that get every new capture as well, for example `usb, pc` |
| `poll_seconds` | `5` | how often the capture folders are checked (1–600) |
| `settle_seconds` | `4` | how long a file must stop growing before it is sent (clips are written over several seconds) |
| `retries` | `3` | attempts per upload (1–10) |
| `notify` | `true` | a console notification for every upload |
| `keep_logs` | `false` | write a detailed log to `pshared.log` |

## Destinations

One section per destination, `[destination.<id>]`. The id is what
`default_destination`, `fallback`, `also_send` and the game rules refer to.

Keys every destination accepts:

| Key | |
| --- | --- |
| `type` | `discord`, `telegram`, `catbox`, `litterbox`, `http`, `folder` or `smb` |
| `name` | shown in the app |
| `enabled` | `false` hides it without deleting it |
| `message` | caption template (see placeholders below) |
| `max_mb` | files bigger than this go to `fallback` instead, and this destination gets the link |
| `fallback` | the destination id for files over `max_mb` |

Placeholders in `message`: `{type}` (Screenshot / Clip), `{game}`,
`{title_id}`, `{date}`, `{time}`, `{file}`, `{size}` and `{link}` (the
fallback's link, when there is one).

### By type

| Type | Keys |
| --- | --- |
| `discord` | `url` the webhook URL (channel settings › Integrations › Webhooks), `username`. Without boosts Discord refuses files over 10 MB, so `max_mb = 10` with a `fallback` is a good pair |
| `telegram` | `token` from @BotFather, `chat_id` of the chat or channel |
| `catbox` | `token` your catbox user hash (optional; uploads then show in your account) |
| `litterbox` | `litterbox_time` `1h`, `12h`, `24h` or `72h` |
| `http` | `url`, `field` the form field carrying the file (default `file`), any number of `header = Name: value` lines and `form.<name> = value` extra fields. The link is read from the response |
| `folder` | `path` `usb` (the first USB drive found) or an absolute folder. Files land in `<folder>/PShare/<game>/` |
| `smb` | `url` `smb://<pc>/<share>/<folder>`, `username`, `token` the account's password. SMB 2/3 |

The default file includes several hosts that need no account: catbox.moe,
Litterbox, Uguu, tmpfiles.org, temp.sh, x0.at and Gofile. How long they keep
files is up to each host.

## Per-game rules

```ini
[title_destinations]
PPSA01234 = telegram      ; this game's captures go to Telegram

[title_screenshots]
PPSA01234 = false         ; no automatic screenshots from this game

[title_videos]
PPSA05678 = false         ; no automatic clips from this one
```

Title ids are the `PPSA` / `CUSA` codes shown on each capture in the app.

## `[app]`

Written by the app for its own choices:

| Key | Default | |
| --- | --- | --- |
| `start_uploader` | `true` | start pshared when PShare opens |
| `background_sends` | `true` | hand uploads started in the app to pshared, so they finish after the app closes |
| `oldest_first` | `false` | the captures order |

## Other files in `/data/pshare`

| File | |
| --- | --- |
| `history.tsv` | one line per upload, from the app and from pshared |
| `pshared.log` | pshared's log |
| `pshared-seen.txt` | captures pshared has already handled |
| `thumbs/` | the app's thumbnail cache; safe to delete |

## Example

```ini
[general]
auto_upload = true
default_destination = discord
also_send = usb

[destination.discord]
type = discord
url = https://discord.com/api/webhooks/<id>/<token>
message = {type} from **{game}** - {date} {time}
max_mb = 10
fallback = catbox

[destination.catbox]
type = catbox

[destination.usb]
type = folder
path = usb

[title_destinations]
PPSA01234 = telegram
```

A webhook URL or bot token is as good as a password: anyone who has it can
post to your channel. Leave it out of bug reports and screenshots.

### Large Telegram clips

Videos sent to a Telegram bot are automatically split without re-encoding when
they exceed 50,000,000 bytes (or `max_mb`, if that is smaller). Each message and
filename includes its part number. Cuts follow video keyframes; part durations
can differ. Every completed part is checked against the size limit before any
part is sent. MP4 remains MP4; WebM remains WebM and is sent as a document unless
the optional conversion below is enabled.

Temporary files use `/data/pshare/tmp` and are removed after success, failure or
cancellation; the original capture is untouched. Preparation temporarily needs
roughly another copy of the clip on disk. An interrupted process can leave a
temporary directory behind. A keyframe interval larger than the limit cannot
be split losslessly and produces an error. Large Telegram videos use splitting
before fallback routing. If a later part fails, earlier messages remain in the
chat; retrying the capture sends the parts again.

### Optional WebM conversion for all exports

In **Settings > How**, enable **Convert WebM to MP4**, or use the global
switch on the phone setup page. It applies to every export destination, including
file hosts, USB/folders and SMB shares. The config key is:

```ini
[general]
convert_webm = true
```

The default is `false`. When enabled, WebM is decoded and encoded as MP4 with
H.264 video (yuv420p, ultrafast, CRF 23) and AAC stereo audio at 128 kb/s.
Output fits within 1920×1080 and caps the frame rate at 30 fps without upscaling.
Each video decoder and encoder uses at most two codec threads. This is lossy,
uses CPU and can take time; its performance during gameplay has not yet been
measured on PS5. Existing MP4 clips and screenshots are unaffected.

Conversion runs before size-based splitting, for manual and automatic uploads.
The original capture stays unchanged. Conversion progress and cancellation use
the normal upload UI. Temporary converted files are deleted on normal return,
error or cancellation; a process crash can leave them behind. Preparing and
splitting a converted video can temporarily need space for both the MP4 and
its parts. Conversion errors stop the upload rather than silently sending WebM.
HDR (PQ/HLG) conversion is currently rejected because tone mapping is not
implemented. The PS5-specific Opus header is corrected only in memory for
decoding; the source file is not modified.

Older per-Telegram `convert_webm` settings migrate to the global option when no
explicit `[general]` value exists. An explicit global value takes precedence.
