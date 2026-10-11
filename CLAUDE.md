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
    ⚠️ It sets only their positions **relative to each other**; the block as a whole stays where it
    already is. It used to be forced past the rightmost display every session, which silently
    overwrote any arrangement the user had set up — the geometry does not care where the block
    lands (capture reports whatever origin it gets), but the user does. It moves only to avoid
    overlapping a display outside the layout, which Windows would reject anyway.
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
- **Virtual-display recovery** (`src/platform/windows/virtual_display_recovery.{h,cpp}`, timings in
  `virtual_display_recovery_policy.h`, after Vibepollo's monitor). A SudoVDA display can vanish under a
  running stream (watchdog fired, driver restarted, monitor disabled); capture then silently falls back
  to the physical display. One monitor thread per session polls every display with
  `isDisplayAttached()`, and after a 500 ms grace re-creates the missing ones with the **same GUID,
  serial, label and mode**, re-applies the layout, publishes the new names through
  `proc_t::set_display_name()` and raises `mail::recapture_display` so the capture thread reopens them.
  3 attempts per cycle (successes count too, so a display that keeps vanishing is not re-created in a
  loop), then 5 s→60 s backoff, giving up after 5 cycles. Leaner than Vibepollo's 9k-line version:
  no display helper, no topology database, no orphan sweep at startup -- displays of a dead host
  already go away through SudoVDA's own watchdog once its pings stop.
  - ⚠️ `VDISPLAY::recovery::stop()` **must run before any deliberate `removeVirtualDisplay()`**
    (`proc_t::terminate()` does it first), or the monitor puts the display straight back.
  - ⚠️ **`proc_t::display_name` is behind a lock now** (`get_display_name()` / `set_display_name()`):
    `execute()`, the capture thread's fallback and the recovery monitor all write it, the capture
    thread reads it on every reinit. The capture reinit reads it **once per attempt**, so a rename by
    the monitor during the retries is picked up on the next one.
  - Only the async capture path (encoders with `PARALLEL_ENCODING`: NVENC, AMF, QSV) honours
    `recapture_display`; the sync path never consulted `proc.display_name` in the first place.
- **Render adapter**: before every virtual display is created (`applyRenderAdapterPreference()`), the
  driver is pointed at `adapter_name`, or at the GPU with the **most dedicated memory** when none is
  set -- otherwise a hybrid laptop could render on the iGPU and encode on the dGPU.
- Send pacing derived from the negotiated stream bitrate rather than a hardcoded 800 Mbps.
- Video queue drops the oldest frames on overflow instead of flushing.
- Higher NVENC quality defaults (P4 preset, spatial AQ).
- **PyroWave codec** (`config.pyrowave`, default on; `videoFormat`/`bitStreamFormat` 3) — see below.

When touching these, keep the pairing of C++ option (`src/config.{h,cpp}`), web UI control
(`src_assets/common/assets/web/configs/tabs/*.vue` + `config.html`), locale string
(`src_assets/common/assets/web/public/assets/locale/en.json`) and `docs/configuration.md` entry in sync —
`tests/integration/test_config_consistency.cpp` and `test_locale_consistency.cpp` enforce this and will fail
otherwise. The docs entry must sit in the `##` section of the option's `config.html` tab, **in the same
order as that tab's `options` object** (`ConfigOptionsInSameOrderWithinSections`); `config.html` only
supplies defaults, the on-screen order comes from the `.vue` tab, so reorder `config.html` freely.
⚠️ These tests had silently gone red upstream (19 undocumented options, 5 labels the UI asked for but
`en.json` never defined) and were brought back to green before the Vibepollo ports; keep them green.

- **Frame limiter** (`config.frame_limiter_*`, `rtss_*`; ported from Vibepollo:
  `src/platform/windows/frame_limiter*.{h,cpp}`, `rtss_integration.{h,cpp}`, `src/framegen_policy.h`).
  While a video session streams, the game is capped at the client's frame rate through **RTSS** (writes
  RTSS's global profile, drives RTSSHooks, starts RTSS itself if needed) or the **NVIDIA driver**
  (base-profile FRL/V-Sync/low-latency settings over NVAPI, loaded at run time -- no import library, so
  MinGW is fine). `auto` tries RTSS, then NVIDIA. Applied in `stream.cpp` `start()` when the **first
  non-input-only** session starts, removed in `join()` when the last one ends.
  - ⚠️ **Every change is snapshotted to `%ProgramData%\LavApollo\{nvcp,rtss}_overrides.json` before it is
    made**, and `main.cpp` replays those at startup: a crashed or killed host must not leave a user's
    games capped or V-Sync forced off. Not `...\Sunshine` as upstream, so a Vibepollo install on the
    same machine keeps its own snapshots.
  - ⚠️ `nvprefs` and the NVIDIA limiter both edit the driver's **base profile**. They touch different
    settings and are serialized by construction -- `streaming_will_start()` (nvprefs) runs right
    before the limiter on the same thread, and at startup the limiter's restore runs after nvprefs.
    Never move either onto another thread.
  - `RTSSHooks64.dll` is loaded **into the host process** (SYSTEM, when running as the service) from
    `rtss_install_path`. That path is admin-only configuration, the same trust level as a prep command.
  - `launch_process_with_impersonation()` (`misc.cpp`) starts RTSS in the user's session; unlike
    Vibepollo's, it falls back to a plain `CreateProcessW` when the host is not the service.
  - LavApollo has no per-app/per-client runtime config overrides, so the original's
    `has_runtime_config_override()` checks are `false`. The frame-generation fields of
    `framegen::stream_start_policy_t` are only switched on for an app with frame generation (below);
    with them on, the limiter enables itself and picks the RTSS `frame_limit_type`, so they must
    never be on for ordinary sessions.
- **Frame generation, per app** (`"frame-generation"` in `apps.json`: `off` | `game-provided` |
  `nvidia-smooth-motion` | `lossless-scaling`; after Vibepollo). A frame-generated game presents more
  often than it renders, and Desktop Duplication drops the in-between frames. When set:
  - the app's virtual displays are created at **4x** the stream rate (`process.cpp`; `double_refreshrate`
    stays the lighter 2x global option), retried at the stream rate if the driver refuses that refresh;
  - a **single** virtual display is captured with **WGC** when `capture` is automatic, switched back in
    `terminate()` (`_capture_before_framegen`). A layout keeps the Desktop Duplication composite;
  - the frame limiter starts with the frame-generation policy: RTSS uses NVIDIA Reflex for DLSS/FSR on
    an NVIDIA GPU, front edge sync otherwise, and **Smooth Motion** is enabled in the driver for the
    stream when that is the provider.
  - Lossless Scaling itself is **not** started or configured by the host (Vibepollo's 2k-line
    automation lives in its Playnite launcher); the setting only prepares capture and pacing for it.

- **WGC capture from the service** (ported from Vibepollo). Windows.Graphics.Capture needs an
  interactive desktop, which the SYSTEM service in session 0 does not have, so `capture = wgc` used to
  fail there. `make_wgc_source()` now picks between two `wgc_source_t`: in-process `wgc_capture_t` when
  the host runs as the user, `wgc_ipc_capture_t` as the service. The latter drives
  `tools/sunshine_wgc_capture.exe`, launched in the user's session by `ipc/process_handler`, which
  captures there and publishes each frame into a texture shared with our device (keyed mutex, frame-
  ready event, metadata in shared memory; wire structs in `ipc/pipes.h`, **compiled into both sides**).
  - ⚠️ **The secure desktop (UAC, lock screen, login) is invisible to WGC.** The factory captures it
    with Desktop Duplication (`display_secure_desktop_{vram,ram}_t`), which asks for a reinit every 2 s
    once it is gone; `wgc_ipc_capture_t` also probes it itself when frames stall, because the helper's
    desktop-switch hook runs with the user's token and can miss the switch.
  - ⚠️ **The emulated multi-display never takes this path**: `platf::display()` returns
    `display_composite_vram_t` for a `+`-joined name before it looks at `capture`. Extending WGC to
    composites would mean one helper per display; not done.
  - WGC display init skips the Desktop Duplication test (`display_base_t::init(..., false)`): WGC is
    what still works when duplication does not.
  - `SystemRelativeTime` is 100 ns units on the QPC timebase, **not QPC ticks**; the in-process path
    used it raw, which skewed every frame timestamp unless QPC runs at 10 MHz
    (`wgc_policy::system_relative_time_to_qpc`).
  - The helper always draws the cursor, logs to `%APPDATA%\LavApollo\sunshine_wgc_helper.log` of the
    user, and is built from `src/logging.cpp` with `SUNSHINE_EXTERNAL_PROCESS` (no FFmpeg, no
    libdisplaydevice). The `severity` log keyword moved to `logging.h` for it.
  - `video::config_t::vrr_low_latency` is appended (for VRR): it shrinks the helper's frame pool to one.

- **VRR clients** (after Vibepollo; client side in LavArtemis). A client that presents frames on a
  G-Sync/FreeSync screen as they come sends `clientVrrRequested=1` on `/launch`/`/resume` -- the only
  protocol addition; it paces from the RTP timestamps every host already sends, and a host that ignores
  the parameter just streams as before. LavApollo then:
  - creates the session's virtual displays at **1000 Hz** (`vrr_virtual_display_refresh`, default on):
    a frame is timestamped when capture sees it, which is on the display's next refresh, so the
    display's own refresh would otherwise quantize every timestamp. Retried at the stream rate if
    SudoVDA refuses it -- **unverified that SudoVDA accepts 1000 Hz**; check the creation log line;
  - sets `video::config_t::vrr_low_latency` (WGC helper keeps a one-frame pool);
  - starts the frame limiter on virtual-display sessions, since VRR only helps if the game itself
    runs at the stream rate. On a physical display the limiter must be enabled by hand.
  - Not ported: Vibepollo's ETW present-time stamping (`present_timing`), which moves WGC timestamps to
    the game's Present cadence. The 1000 Hz display gets most of that benefit.

- **Log export** (`GET /api/logs/export`, button in Troubleshooting) — a ZIP of the current log, the
  previous run's log (`<log>.backup`, rotated by `logging::init()`) and the config file, with user names,
  IP/MAC and e-mail addresses replaced by stable placeholders (`<IP_1>` is the same client everywhere in
  the bundle). `src/log_export.h`, ported from Vibepollo; the ZIP is written by hand over zlib, no archive
  library. It does **not** recognise passwords or tokens: never add a file that holds credentials
  (`credentials_file`, `file_state`, keys) to the bundle.

### PyroWave

[PyroWave](https://github.com/Themaister/pyrowave) is an intra-only GPU wavelet codec (Vulkan compute,
sub-millisecond encode, hundreds of Mbps). It is ported from [Vibepollo](https://github.com/Nonary/Vibepollo)
(also an Apollo fork) and speaks its wire protocol unchanged, so Vibepollo-compatible clients work. The
contract is `docs/pyrowave-protocol.md`, shared word for word with `LavArtemis-Qt/docs/` — change both.

- **One PyroWave source.** `third-party/pyrowave` is the `Lavagnou/pyrowave` submodule (branch `lavartemis`,
  upstream v1 + pruned Granite + build glue in `lavartemis/`). The bitstream has no version field, so
  LavApollo and both LavArtemis clients must pin the **same commit**; bump them together.
  `pyrowave::protocol::BITSTREAM_ID` (`src/pyrowave_protocol.h`) is advertised in DESCRIBE.
- **It does not ride `encoder_t`.** `capture_async()` branches to `encode_run_pyrowave()` (`src/video.cpp`) for
  `videoFormat == 3`, whatever encoder was probed; `capture()` always takes the async path for it. Every frame
  is an IDR; IDR and RFI requests are drained and ignored. The encoder (`src/platform/windows/pyrowave_encode.cpp`
  + `pyrowave_d3d11_core.cpp` + `convert_pyrowave_cs.hlsl`) opens the captured texture through its shared handle,
  converts to Y'CbCr planes on a private D3D11 device and hands them to PyroWave's Vulkan device by NT handle +
  shared fence. `img_d3d_t` moved to `display_vram.h` for this.
- **Probing is separate** (`probe_pyrowave()`, `active_pyrowave_mode`): it is never gated on H.264 passing, but it
  is refused when the chosen encoder captures to system memory — a PyroWave session shares that capture path.
- **`stream.cpp`**: PyroWave ignores `fec_percentage`, `adaptive_fec`, `adaptive_bitrate` and `pacing_multiplier`.
  Only the critical packets (sequence header + coarsest wavelet level) get parity, at
  `pyrowave_critical_fec_percentage`, planned by `pyrowave::policy::plan_fec_blocks()`; their count rides the
  last two bytes of the short frame header, and record-start shards set bit `0x80` of `NV_VIDEO_PACKET`'s
  `reserved` byte (`extraFlags` in newer moonlight-common-c). Pacing follows the routed link speed
  (`platf::routed_link_bps()`), refreshed every 2 s. A queue overflow requests no IDR.
- **Calibration**: `/serverinfo` reports `PyroWaveHostLinkMbps` + `PyroWaveBandwidthProbeBytes` to paired clients,
  and `/pyrowave-bandwidth-probe` serves a 32 MiB download (HTTPS, paired, 8/min) the client times.
- `src/pyrowave_policy.{h,cpp}` (framing, FEC plan, budget, pacing) has no Sunshine dependency and is covered by
  `tests/unit/test_pyrowave_policy.cpp`.

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

Known red on `master`, unrelated to this fork's work: `DisplayDeviceConfigTest/DisplayModeRemapping.*` and
`ParseRefreshRateOption.*` (28) and `MdnsInstanceNameTests/*` (3). `PairingTest.Run/0` is skipped: since
Apollo, a successful `clientpairingsecret()` erases the session from nvhttp's private map and saves the
client list, which the test cannot set up. Compare a change against this list, not against "all green".

On Windows, build the tests from PowerShell or cmd, not MSYS bash: under bash `TMP` does not reach the
compiler and gcc fails with "Cannot create temporary file in C:\Windows\". Keep the build directory path
short too -- `ar` fails on object paths past `MAX_PATH`.

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
