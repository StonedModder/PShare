# Contributing

Thanks for helping with PShare. Bug reports with a log and the console's system
software version are the most useful thing you can send; pull requests are
welcome too.

## Before opening a pull request

1. `make lint` and `make test` pass (format with `make format`).
2. `make` builds the app folder, its ZIP and `pshared.elf` without warnings
   from PShare's own files.
3. A change to what you see comes with `make preview` screenshots of it.
4. A change that only real hardware can prove (elevation, sockets, sound,
   video, the loader) says which system software version it was run on.
5. One topic per pull request.

## Rules

- Never commit Sony SDK files, firmware, decrypted system files, keys,
  console dumps, webhook URLs or tokens.
- Code or assets you did not write go in `THIRD_PARTY_NOTICES.md` with their
  licence, and must be compatible with GPL-3.0-or-later.
- Source files keep their copyright line and
  `SPDX-License-Identifier: GPL-3.0-or-later` header.
- `src/third_party/` is vendored as published; do not reformat it.
- Release tags are the exact `contentVersion` from `sce_sys/param.json`
  (`NN.NNN.NNN`, no `v` prefix). Pushing one builds and publishes the release.

See [docs/BUILDING.md](docs/BUILDING.md) for the toolchain and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the pieces fit.
