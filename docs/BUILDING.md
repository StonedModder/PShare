# Building PShare

PShare builds on Linux or WSL. Nothing from Sony's SDK is needed: the console
binaries are produced with clang/lld and the repository's own tools under
`tooling/native/`, and every library is fetched at a pinned version.

## Host requirements

| Tool | Used for |
| --- | --- |
| clang, clang++, lld (18 or newer) | the console app, the payload, the host tools and tests |
| llvm-ar, llvm-ranlib | static archives |
| clang-format, clang-tidy | `make lint` |
| make, ninja, bash, python3 | build orchestration |
| wget, unzip, tar, sha256sum, git | pinned dependency downloads |
| ccache (optional) | faster rebuilds (`USE_CCACHE=0` turns it off) |

Host tests also require FFmpeg development libraries and `pkg-config`. The
real-media integration tests use the `ffmpeg` and `ffprobe` commands.

For the PC preview: Mesa's EGL/GL and libcurl development files, plus the
`ffmpeg` command line (it makes the sample clip the preview plays) and the
FFmpeg development libraries. For `make art`: Python 3 with Pillow.

On Debian or Ubuntu:

```bash
sudo apt install clang lld llvm clang-format clang-tidy make ninja-build \
    python3 python3-pil wget unzip git ccache \
    libegl-dev libgl-dev libcurl4-openssl-dev ffmpeg \
    libavformat-dev libavcodec-dev libswscale-dev libswresample-dev
```

`make doctor` lists what is present and what is missing without changing
anything.

## Dependencies

```bash
make deps
```

downloads, verifies and unpacks into `.deps/` (ignored by Git):

* the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) (v0.42),
  for `pshared.elf` and the Lapy helper;
* the [PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) sysroot,
  which provides libcurl, FFmpeg 7 and libsmb2 for the console
  ([PACBREW.md](PACBREW.md));
* the ps5-opengl SDK (Mesa on the console's graphics stack).

The first build also fetches
[PS5-Lapy-JB-Daemon](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon)
at the commit ProsperoStore uses (`c3bdfe3`) and builds its helper into the
app folder as `lapy.elf`. Together this is roughly 500 MB the first time;
later builds reuse the cache.

## Targets

| Command | Result |
| --- | --- |
| `make` | `dist/PPSA99773/` (the app folder), `dist/PPSA99773.zip` and `dist/pshared.elf` |
| `make daemon` | only `build/daemon/pshared.elf` |
| `make test` | host unit tests with ASan/UBSan, the Lapy client tests and the tooling tests |
| `make lint` | clang-format check (`make format` fixes it) |
| `make preview` | every page rendered on the PC into `build/preview/*.png` |
| `make art` | the icon, home-screen backgrounds, splash, in-app art and `docs/banner.png` |
| `make assets-check` | validates the icon, backgrounds and selection audio |
| `make libc` | rebuilds and verifies `runtime/libc.prx` ([RUNTIME_SHIM.md](RUNTIME_SHIM.md)) |
| `make clean` / `make distclean` | removes `build/` and `dist/` / also `.deps/` |

`make check` runs lint, tests and the build, which is what CI does.

## Changing the title

The app's identity lives in `sce_sys/param.json`. To build it under another
title ID or name:

```bash
make init TITLE_ID=PPSA12345 APP_NAME="PShare"
```

`CONTENT_SUFFIX` sets the last 16 characters of the content ID (by default
they come from the name).

`APP_CATEGORY=media` (the default) keeps it in the console's Media area;
`APP_CATEGORY=game` puts it with the games. The Lapy helper is built for the
exact title ID, so it follows the change automatically.

## Deploying while developing

With an FTP server running on the console:

```bash
make deploy PS5_HOST=192.168.1.50 FTP_PORT=2121
```

builds the app folder and uploads it to `/data/homebrew/<TITLE_ID>/`. Files go
up under temporary names and are renamed when complete, so a running copy is
never left half-written. `DEPLOY_DRY_RUN=1` prints what would be sent.
`make undeploy` removes only the current title's folder.

Local defaults can go in `.env` (see `.env.example`).

## The PC preview

`make preview` builds the platform-neutral part of PShare against Mesa's
surfaceless EGL and runs `host/preview_main.cpp`: a pretend console with
invented games, a scripted walk through every page, and a real VP9/Opus clip
playing in the clip player. It writes one PNG per step. The screenshots in
the README come from it.

## Releases

Release tags are the exact `contentVersion` from `sce_sys/param.json`
(`01.000.000`, no `v`). Pushing such a tag runs
[`.github/workflows/build.yml`](../.github/workflows/build.yml), which
builds, tests, writes `SHA256SUMS`, attests the files and publishes the
release with the app ZIP and `pshared.elf`. Bump `contentVersion` before
tagging the next one.
