# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

LavApollo is a fork of [Apollo](https://github.com/ClassicOldSong/Apollo), which is itself a fork of
[Sunshine](https://github.com/LizardByte/Sunshine). It is a self-hosted game-stream **host**: it captures
the desktop/game, encodes with a hardware encoder (NVENC / AMF / QSV / VideoToolbox / VAAPI, or software
x264), and serves it over the NVIDIA GameStream protocol to Moonlight/Artemis clients. A Vue web UI
(default `https://localhost:47990`) handles configuration and client pairing.

Naming is inconsistent by inheritance and that is expected: the CMake project is `LavApollo`, but the
binary target, config file, most identifiers, CMake variables (`SUNSHINE_*`), and the npm package are
still `sunshine`. Don't "fix" these unless asked.

## Fork-specific changes

The value of this fork over upstream Apollo lives in the streaming path (see `git log` before
`adc5c5a0`, which is the last upstream commit):

- **Adaptive FEC** (`config.adaptive_fec`, default on) — FEC percentage is driven per-frame by
  client-reported loss instead of being fixed; carried in the protocol's `fecInfo` field.
- **Adaptive bitrate** (`config.adaptive_bitrate`, default on) — when FEC is exhausted and loss
  persists, the encoder bitrate is cut and later restored. Implemented via `encode_session_t::update_bitrate()`
  and `NvEncReconfigureEncoder` (rate control only, no encoder reset / IDR), signalled from the control
  thread to the encoder thread through the `mail::bitrate_change` event.
- **Emulated multi-display** — a client can send `displayLayout=` on `/launch` (`x,y,w,h,primary`
  per display, in the coordinate space of the `mode=` it already sends) and get one virtual display
  per monitor instead of one big one. `launch_session_t` carries a `std::vector<virtual_display_t>`;
  a single-display client just gets a one-element list, so creation, arrangement and teardown have
  one code path. Advertised to clients as `MultiDisplayCapable` in `serverinfo`.
  - `VDISPLAY::applyVirtualDisplayLayout()` places them in **one** `SetDisplayConfig` call —
    moving them one at a time passes through overlapping arrangements, which Windows refuses.
  - `display_composite_vram_t` (in `display_vram.cpp`) captures them all into one frame the size of
    their bounding box. ⚠️ **The frame must stay geometrically identical to that region of the
    desktop**: `make_port()` in `video.cpp` derives the client's coordinate plane from the captured
    display's own offset and size, which is the only reason absolute mouse/touch/pen need no
    protocol change. Packing the displays tighter would break that.
  - Displays reach the capture layer as one `+`-joined name (`platf::join_display_names`), because
    `proc.display_name`, the capture thread and `platf::display()` all take a single name.
  - ⚠️ SudoVDA attaches a **default 1920x1080 monitor** when its adapter first wakes up, so our
    displays are always tracked by the GUID we created them with, never by enumerating the adapter.
  - ⚠️ **Windows re-origins the whole desktop when the primary display changes.** The emulated
    displays keep their positions relative to each other -- the host still looks right -- but every
    absolute coordinate shifts, and nothing reports it: losing a display kills its duplication
    (`DXGI_ERROR_ACCESS_LOST` -> rebuild), a re-origin kills nothing. So
    `display_composite_vram_t::topology_changed()` polls monitor rectangles 4x/s and returns
    `capture_e::reinit`; without it the picture slides across the client's monitors with no error
    anywhere. Record and re-check both go through the same `GetMonitorInfo()` helper -- mixing it
    with DXGI's `DesktopCoordinates` would compare two coordinate spaces and rebuild forever.
  - ⚠️ **Monitor identity is what Windows keys a saved arrangement on**, and every emulated display
    used to be given the session UUID as its serial -- so a two-display layout was two monitors
    Windows could not tell apart, and an arrangement configured once was never restored. Each
    display now gets its own serial (session identity + index), stable across sessions. The EDID
    name has the same 13-character limit, which used to cut the " 1"/" 2" suffix off a long client
    name; room is now reserved for it. Single-display sessions deliberately keep the old serial.
    ⚠️ SudoVDA still allocates a fresh target id per add (`DISPLAY\SMKD1CE\...&UID<n>`, four
    sessions from one client left `UID260/262/263/264`), which is in the driver binary and not
    something we control -- so never *rely* on Windows restoring; `applyVirtualDisplayLayout()`
    applies the arrangement among the emulated displays every session regardless.
  - ⚠️ **`isolated_virtual_display_option` does not disable anything**, despite the name: it only
    *rearranges* displays around the virtual one (`rearrangeVirtualDisplayForLowerRight`). The real
    mechanism for taking over the desktop is upstream's `dd_configuration_option`
    (`ensure_active` / `ensure_primary` / `ensure_only_display`, default **disabled**), which comes
    with persistence and revert via libdisplaydevice. Two things stand in the way of using it with
    an emulated layout: `config::video.output_name` is set to the **first** virtual display only
    (`process.cpp:458`), so `ensure_only_display` would disable the *other* emulated displays; and
    `process.cpp:470` calls `reset_persistence()` for any virtual-display session, discarding the
    revert state -- harmless while the option is `disabled`, but it would strand a user's real
    displays turned off if they enabled it.
- Send pacing derived from the negotiated stream bitrate rather than a hardcoded 800 Mbps.
- Video queue drops the oldest frames on overflow instead of flushing.
- Higher NVENC quality defaults (P4 preset, spatial AQ).

When touching these, keep the pairing of C++ option (`src/config.{h,cpp}`), web UI control
(`src_assets/common/assets/web/configs/tabs/*.vue` + `config.html`), and locale string
(`src_assets/common/assets/web/public/assets/locale/en.json`) in sync — `tests/integration/test_config_consistency.cpp`
and `test_locale_consistency.cpp` enforce this and will fail otherwise.

## Build

Requires submodules (`git clone --recurse-submodules`, or `git submodule update --init --recursive`),
CMake > 3.25, GCC 13+ / Clang 17+, and Node.js on `PATH` (the `web-ui` CMake target shells out to `npm install`).
Platform dependency lists are in `docs/building.md`; Linux deps are scripted in `scripts/linux_build.sh`.

```bash
cmake -B build -G Ninja -S .
ninja -C build
```

Build options are declared in `cmake/prep/options.cmake`. Packaging:
`cpack -G <NSIS|ZIP|DEB|RPM|DragNDrop> --config ./build/CPackConfig.cmake`.

The web UI can be iterated on without a full rebuild: `npm run dev` (vite watch) or `npm run build`.

## Tests

Google Test, built as part of the normal build (disable with `-DBUILD_TESTS=OFF`):

```bash
./build/tests/test_sunshine                                  # all
./build/tests/test_sunshine --gtest_filter='VideoTest.*'     # one suite
./build/tests/test_sunshine --gtest_list_tests
```

Note `tests/CMakeLists.txt` forces `-O0` plus gcov flags for the test target, so a test build is not a
representative build for performance work on the streaming path.

Lint: C++ is checked against `.clang-format`.

```bash
find ./src ./tests -iname '*.cpp' -o -iname '*.h' -o -iname '*.m' -o -iname '*.mm' | xargs clang-format -i
```

## Architecture

**Layering**, roughly request → pixels:

- `src/main.cpp` → `src/entry_handler.cpp` — startup, CLI commands, single-instance/elevation handling.
- `src/nvhttp.cpp` — the GameStream HTTP/HTTPS control API the client speaks (serverinfo, pairing,
  applist, launch/resume/cancel). `src/crypto.cpp` holds the pairing/cert machinery. Apollo's
  per-client **permission system** lives here and in `src/confighttp.cpp`; the first paired client gets
  full permissions, later ones get a restricted default set.
- `src/confighttp.cpp` — the separate web UI server (config, apps, clients, permissions), serving the
  built assets from `build/assets/web`.
- `src/rtsp.cpp` — RTSP handshake; negotiates the session (codec, resolution, FEC floor, bitrate) and
  creates the session objects consumed by `stream.cpp`.
- `src/stream.cpp` — the streaming core: video/audio packetization, FEC encoding (`third-party/nanors`),
  send pacing, RTP/control channel via `third-party/moonlight-common-c`, and the loss-stats /
  adaptive-FEC / adaptive-bitrate controllers.
- `src/video.cpp` + `src/nvenc/` — capture→encode pipeline. `video.cpp` owns encoder selection,
  capture threads (async and synced paths), color space (`video_colorspace.cpp`) and bitstream fixups
  (`cbs.cpp`). `src/nvenc/` is a standalone NVENC implementation used instead of FFmpeg's on Windows.
- `src/audio.cpp`, `src/input.cpp` (`third-party/inputtino`, ViGEmClient on Windows),
  `src/display_device.cpp` (`third-party/libdisplaydevice` + SudoVDA virtual display on Windows),
  `src/process.cpp` (app launching, prep commands), `src/upnp.cpp`, `src/system_tray.cpp`.

**Cross-thread communication** is the thing to understand before editing the streaming path.
`src/globals.h` declares a process-wide "mail" bus (`safe::mail_t`) of named events —
`shutdown`, `video_packets`, `idr`, `invalidate_ref_frames`, `bitrate_change`, `hdr`, `switch_display`, …
Threads never call into each other directly; they publish/subscribe on these. The primitives live in
`src/thread_safe.h`, `src/sync.h`, `src/task_pool.h`, `src/thread_pool.h`.

**Platform abstraction**: `src/platform/common.h` declares the interfaces (display/capture, audio,
input, misc); `src/platform/{windows,linux,macos}/` implement them. Selection of implementation is
compile-time via the `cmake/` tree, which is split by concern
(`prep/`, `dependencies/`, `compile_definitions/`, `targets/`, `packaging/`), each with a `common.cmake`
plus per-platform files. Adding a source file or a platform-conditional dependency means editing
`cmake/targets/*.cmake` or `cmake/dependencies/*.cmake`, not the top-level `CMakeLists.txt`.

**Web UI**: Vue 3 + vue-i18n, built by Vite. Pages are `.html` entrypoints in
`src_assets/common/assets/web/` assembled from `template_header.html` via `vite-plugin-ejs`; Vite reads
`SUNSHINE_SOURCE_ASSETS_DIR`/`SUNSHINE_ASSETS_DIR` from CMake to locate source and output. Strings go
through Crowdin (`crowdin.yml`); only edit `public/assets/locale/en.json`.

## Release

`.github/workflows/release.yml` builds the Windows installer + portable zip and publishes a GitHub
release, triggered by pushing a `v*` tag or by manual dispatch with a version input.
