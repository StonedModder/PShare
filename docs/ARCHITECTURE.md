# How PShare fits together

PShare is two programs that share one configuration file:

* **the app** (`eboot.bin` in `PPSA99773/`), a native PS5 title drawn with
  OpenGL through the ps5-homebrew-ui kit. It browses captures, plays clips,
  edits destinations and shows the history;
* **pshared** (`pshared.elf`), a small payload started through the
  jailbreak's ELF loader. It runs outside the app sandbox, keeps running after
  the app closes, watches for new captures and does the uploading.

```
 PShare app (native title, elevated by Lapy)      pshared (payload)
 ┌──────────────────────────────────────┐         ┌──────────────────────────────────────┐
 │ captures grid, clip player (FFmpeg)  │  9077   │ watches /user/av_contents/{photo,    │
 │ destinations, history, settings      │ ──────► │ video}, waits for files to settle,   │
 │ QUEUE / RESULT / STATUS app          │ loopback│ uploads, notifies, writes history    │
 └──────────────────┬───────────────────┘         │ web page + gallery on :9078          │
                    │                             └──────────────────┬───────────────────┘
                    │   /data/pshare/config.ini · history.tsv        │
                    └────────────────────┬───────────────────────────┘
```

## Start-up

1. `src/main.cpp` sets up the heap (384 MiB, 192 MiB if the larger mapping
   fails), the GL context, audio and the root folder of the app.
2. **Elevation** (`src/elevation/`, the same client ProsperoStore uses). A
   native title cannot read `/data` or the capture folders on its own, so the
   app tries, in order:
   1. `/data` is already readable: nothing to do;
   2. a resident Lapy service is running: ask it;
   3. send the app's own `lapy.elf` (built for this exact title ID) to the ELF
      loader on `127.0.0.1:9021` and wait for its answer.

   The result and the step it stopped at are written to the log. Without
   elevation the app still runs, but cannot list captures.
3. One second after start, if "Turn on when PShare opens" is set, the app
   checks for pshared on 9077 and sends `pshared.elf` to 9021 if nothing
   answers, or if what answers is another build. Each `pshared.elf` carries a
   build ID made from its sources (`PSHARE_BUILD=…`); a new copy that finds
   an older one running asks it to stop and takes over, so updating the app
   also updates the uploader.

Game names come from `/user/appmeta/<title>/param.json` (or `param.sfo`)
when the game is installed, otherwise from the `.dat` file the console keeps
beside every capture, which holds the game's name at offset 0x44. That covers
PS4 titles and games that have since been deleted.

## The control line (127.0.0.1:9077)

One text command per connection; the reply is `ok`, `error <reason>` or the
data asked for.

| Command | Reply |
| --- | --- |
| `PING` | `pong pshared <version> <build>` |
| `STATUS` / `STATUS app` | one `key=value` per line: state, counters, `job=`, `sent=`, `total=`, `ip=`. `STATUS app` also tells pshared the app is open, which keeps the web page up |
| `PAUSE` / `RESUME` | stop or restart automatic uploads |
| `RELOAD` | read `config.ini` again |
| `RESCAN` | look at the capture folders now |
| `QUEUE [<destination>\t]<path>[\t<flags>]` | `ok <id>`: send this file now. Flags, comma separated: `delete` deletes the capture once it is sent; `batch` marks one of a Send all (no notification each, one summary when the queue empties) |
| `RESULT <id>` | `pending`, `ok <destination>\t<link>`, `failed <destination>\t<error>` or `unknown` |
| `CANCEL` | stop the current upload and drop the queue |
| `SETUP on` / `SETUP off` | open the web page for 30 minutes, or close it |
| `QUIT` | exit |
| `RELAY` | the app's end of the clip relay (below): `none`, or `<first>\t<last>\t<path>` and then the app sends those bytes |

## Sending a capture

`Model::upload` (`src/app/model.cpp`) puts a job on the app's upload thread.
With `[app] background_sends = true` (the default) the thread hands it to
pshared: start it if needed, `QUEUE`, then poll `RESULT` and `STATUS` for
progress, which the top bar shows. pshared writes the history line, so a long
clip keeps uploading after the app closes.

**Send all** (`Model::send_batch`) hands a whole batch to pshared at once,
one `QUEUE` per capture with the `batch` flag (and `delete` when asked), so
it finishes after the app closes. pshared takes its queue one job at a time,
so `CANCEL` stops the rest of the batch. The app follows it through
`STATUS` (`job=`) and asks `RESULT` about the last job to know when it is
over, then counts what was sent from the history. A capture is deleted only
after it was sent successfully. pshared, a payload, may not be allowed to
delete captures; then it lists them in `/data/pshare/delete-after-send.txt`
and the app, which can, deletes them when it starts, on its regular rescan
and when the batch ends. Rate limits (HTTP 429) are waited out for as
long as the service asks (`retry_after` from Telegram and Discord). Captures
of a batch going to the same Telegram chat are sent as albums of up to 10
(`sendMediaGroup`; photos and files apart, 50 MB at most, `album_kind`): pshared waits until the app's `QUEUE` lines stop for half
a second, then takes up to ten that fit, and every job of the album answers
`pending` to `RESULT` until it is sent.

When pshared cannot run (no loader on 9021, the ELF missing) the app sends
the file itself with libcurl and writes the history line.

Automatic uploads follow the same path inside pshared: the watcher
(`src/pshare/core/watcher.cpp`) notices a new file, waits until its size has
stopped changing for `settle_seconds`, picks the destination (the game's
entry in `[title_destinations]`, otherwise `default_destination`), applies
`max_mb` and `fallback`, and also sends to everything in `also_send`.

## The web page (port 9078)

pshared serves a small page on the local network. It is open while `SETUP` is
on, and for 15 seconds after the app's last `STATUS app`, so it is available
the whole time PShare is open, and closed otherwise. There is no password:
anyone on the same network can open it while it is up, so saved webhooks,
tokens and passwords are never sent to the browser (an empty field keeps the
saved value).

| Route | |
| --- | --- |
| `/` | settings and destinations, for pasting webhooks and tokens from a phone |
| `/gallery?game=<title id>&order=old` | every capture, filtered by game and in either order |
| `/v/<file name>`, `/view?p=<capture>` | one capture full screen with the app's controls (keyboard, touch, or a controller through the Gamepad API) |
| `POST /send`, `/result?id=` | send from the browser, like `QUEUE` / `RESULT` |
| `/file?p=<capture>` | the file itself, with `Range` support so clips stream; only paths under the capture folders |
| `POST /save` | writes the settings form back to `config.ini` |

## The clip relay

After some console restarts, payloads (pshared, an FTP server) read video
captures back as all zeros, while the app, a proper title elevated by Lapy,
still reads them, as does the Media Gallery. Screenshots are not affected.
While the app runs, three of its threads keep a `RELAY` request open on 9077.
When pshared needs a capture it reads as zeros, it hands one of them the byte
range and passes on what the app sends:

* the browser page streams, seeks and downloads that way;
* uploads, including automatic ones, fetch a temporary copy into
  `/data/pshare/relay` first and delete it afterwards;
* with the app closed, the page says to open PShare, and uploads are refused
  rather than sending a file of zeros (`capture_reads_blank`).

## The clip player

`src/app/video.cpp` decodes with FFmpeg 7 (VP9, H.264, HEVC, AV1; Opus, AAC)
on its own threads, and `src/app/clip_player.cpp` draws it. Notes from running
it on the console:

* each decoded picture goes into a new texture; updating one texture in place
  with `glTexSubImage2D` kept showing the first picture;
* the picture follows the sound clock, and falls back to the wall clock if
  the sound stops advancing for 0.4 s;
* the PS5 writes an Opus header FFmpeg refuses (version 16). As in
  [VLC for PS5](https://github.com/ZiZc3/VLC), `fix_ps5_opus_header` replaces
  it with a standard stereo header, before the stream is probed, otherwise the
  demuxer drops every sound packet.

Clip tiles in the captures grid use the same decoder for one frame.

## Source layout

```
src/main.cpp         console entry point
src/app/             the app: model (workers, pshared hand-off), pages, clip player,
                     video decoder, thumbnails, look
src/pshare/core/     shared by the app, pshared and the tests: config and INI, captures,
                     uploads (curl, OneDrive, SMB, folder), pictures (stb_image: thumbnails,
                     Telegram photo copies), history, watcher, net, web pages
src/elevation/       Lapy client
src/runtime/         heap, libc additions the console's runtime lacks
src/ui, src/gfx, src/audio, src/platform, src/core
                     the ps5-homebrew-ui kit
daemon/main.cpp      pshared (kept outside src/, which the app build compiles whole)
host/                the PC preview
tests/               unit tests (no framework) and tooling tests
tooling/native/      the PS5 module writer, FSELF wrapper and runtime emitter
tools/               build, deploy, art and dependency scripts
```

Compile-time switches: `PSHARE_CONSOLE` (the app), `PSHARE_PAYLOAD`
(pshared), `PSHARE_HAVE_SMB2` (both console builds). With none of them the
code builds for the PC.

## Console details worth knowing

* The app talks to 9021 and 9077 through libSceNet (`Link` in
  `src/pshare/core/net.cpp`); the payload and the PC use BSD sockets.
* The console runtime has no `localtime_r`, `getprotobyname`, `nl_langinfo`
  or a usable `arc4random_buf` for a native title; `src/runtime/libc_extras.c`
  provides them.
* Threads created without attributes get a small stack, so
  `pthread_create` is wrapped to give them 1 MiB (FFmpeg's frame threads,
  curl's resolver).
* `fcntl` is wrapped for curl on sandboxed sockets.
* Lapy's resident service exits at once unless a ps5log server is configured,
  so PShare ships the one-shot helper, as ProsperoStore does.
