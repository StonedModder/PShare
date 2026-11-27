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
| `type` | `discord`, `telegram`, `catbox`, `litterbox`, `http`, `folder`, `smb` or `onedrive` |
| `name` | shown in the app |
| `enabled` | `false` hides it without deleting it |
| `message` | caption template (see placeholders below) |
| `max_mb` | files bigger than this go to `fallback` instead, and this destination gets the link. At most the service's own limit: Discord 100 (a server at boost level 3), Telegram 50, catbox 200, Litterbox 1024 |
| `fallback` | the destination id for files over `max_mb`: a file host or custom server (its link is posted), `onedrive` (a view-only share link anyone can open is posted), or a `folder` / `smb` destination (the file is copied there and the post says where) |

Placeholders in `message`: `{type}` (Screenshot / Clip), `{game}`,
`{title_id}`, `{date}`, `{time}`, `{file}`, `{size}` and `{link}` (the
fallback's link, when there is one).

### By type

| Type | Keys |
| --- | --- |
| `discord` | `url` the webhook URL (channel settings › Integrations › Webhooks), `username`. Discord takes files up to 20 MB without boosts (50 MB at boost level 2, 100 MB at level 3), so `max_mb` at your server's limit with a `fallback` is a good pair |
| `telegram` | `token` from @BotFather, `chat_id` of the chat or channel |
| `catbox` | `token` your catbox user hash (optional; uploads then show in your account) |
| `litterbox` | `litterbox_time` `1h`, `12h`, `24h` or `72h` |
| `http` | `url`, `field` the form field carrying the file (default `file`), any number of `header = Name: value` lines and `form.<name> = value` extra fields. The link is read from the response |
| `folder` | `path` `usb` (the first USB drive found) or an absolute folder. Files land in `<folder>/PShare/<game>/` |
| `smb` | `url` `smb://<pc or mac>/<share>/<folder>`, `username`, `token` the account's password. SMB 2/3: a Windows share, macOS File Sharing (turn on **Windows File Sharing** for the account under Options) or Samba |
| `onedrive` | `client_id` the Application (client) ID of your app registration, `token` a refresh token for it (see the README's OneDrive guide), `path` the OneDrive folder (default `PShare`). Files land in `<path>/<game>/`; a name already there gets a new one rather than being replaced |

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
| `onedrive-<id>.token` | the newest refresh token Microsoft handed back for that OneDrive destination; delete it to go back to the one in the config |
| `thumbs/` | the app's thumbnail cache; safe to delete |

> [!CAUTION]
> A webhook URL, bot token or refresh token is as good as a password: anyone who has it can post to your channel. Leave it out of bug reports and screenshots.

## Example

The file PShare writes on first start, with every destination it knows.
Fill in what you use and turn it on with `enabled = true`:

```ini
; PShare configuration.
; The PShare app edits this file (Settings), and so can you: over the phone
; setup page, or by hand over FTP.
; Lines starting with ; are comments and are kept when the app saves.

[general]
; Send new captures as soon as they are saved, while pshared runs.
auto_upload = true
upload_screenshots = true
upload_videos = true
; Also send captures that existed before pshared first started.
upload_existing = false
; Seconds between looks for new captures, and how long a file must stop
; growing before it is sent (clips are written over several seconds).
poll_seconds = 5
settle_seconds = 4
retries = 3
; A console notification for every upload.
notify = true
keep_logs = false
default_destination = discord
; Destinations that also get every new capture, for example a USB copy:
; also_send = usb

; One section per destination: [destination.<id>]
; type = discord | telegram | catbox | litterbox | http | folder | smb | onedrive
; message placeholders: {type} {game} {title_id} {date} {time} {file} {size} {link}

[destination.discord]
type = discord
name = Discord
; Channel settings > Integrations > Webhooks > Copy Webhook URL
url =
username = PShare
message = {type} from **{game}** - {date} {time}
; Discord takes attachments up to 20 MB on servers without boosts (50 MB at
; boost level 2, 100 MB at level 3). Bigger files go to the fallback and
; Discord gets the link instead.
max_mb = 20
fallback = catbox

[destination.catbox]
type = catbox
name = catbox.moe
; Optional: your catbox user hash, so uploads show up in your account.
token =

[destination.litterbox]
type = litterbox
name = Litterbox
; How long the file stays up: 1h, 12h, 24h or 72h.
litterbox_time = 72h
enabled = false

[destination.telegram]
type = telegram
name = Telegram
; token from @BotFather, chat_id of the chat or channel to post in.
token =
chat_id =
message = {type} from {game} - {date} {time}

[destination.custom]
type = http
name = My server
url =
; The form field that carries the file.
field = file
; header = Authorization: Bearer secret
; form.caption = {type} from {game}
enabled = false

; Hosts that need no account (links last as long as the name says).
[destination.uguu]
type = http
name = Uguu (3 hours)
url = https://uguu.se/upload
field = files[]
max_mb = 128

[destination.tmpfiles]
type = http
name = tmpfiles.org (1 hour)
url = https://tmpfiles.org/api/v1/upload
field = file
max_mb = 100

[destination.tempsh]
type = http
name = temp.sh (3 days)
url = https://temp.sh/upload
field = file
max_mb = 4000

[destination.x0]
type = http
name = x0.at
url = https://x0.at
field = file
max_mb = 1024

[destination.gofile]
type = http
name = Gofile
url = https://upload.gofile.io/uploadfile
field = file

[destination.usb]
type = folder
name = USB drive
; usb: the first USB drive found (/mnt/usb0, /mnt/usb1, /mnt/ext0, /mnt/ext1),
; or an absolute folder. Captures land in <drive>/PShare/<game>/.
path = usb
enabled = false

[destination.pc]
type = smb
name = My PC
; A shared folder on your PC: smb://<pc address>/<share>/<folder>
; (Windows: right-click a folder > Properties > Sharing).
url = smb://192.168.1.10/Captures/PS5
username =
; The account's password.
token =
enabled = false

[destination.mac]
type = smb
name = My Mac
; A shared folder on your Mac: smb://<mac address>/<shared folder>
; (System Settings > General > Sharing > File Sharing: add the folder, then
; Options... > turn on Windows File Sharing for your account).
url = smb://192.168.1.20/Captures
; The Mac account's short name (the name of its home folder).
username =
; The account's password.
token =
enabled = false

[destination.onedrive]
type = onedrive
name = OneDrive
; The Application (client) ID of your app registration at portal.azure.com,
; and a refresh token for it (the README's OneDrive guide shows how).
client_id =
token =
; Captures land in <path>/<game>/ in your OneDrive.
path = PShare
enabled = false

[title_destinations]
; PPSA01234 = telegram

[title_screenshots]
; PPSA01234 = false

[title_videos]
; PPSA01234 = false
```

A webhook URL, bot token or refresh token is as good as a password: anyone who has it can
post to your channel. Leave it out of bug reports and screenshots.
