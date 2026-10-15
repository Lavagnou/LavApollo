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

- **Adaptive FEC** (`config.adaptive_fec`, default on) and **adaptive bitrate**
  (`config.adaptive_bitrate`, default on), decided by `stream::adaptive_controller_t`
  (`src/stream_adaptive.{h,cpp}`: pure logic, no session or clock, unit-tested).
  - ⚠️ **The input is the client's per-frame FEC report, `SS_FRAME_FEC_STATUS` (control type
    `0x5502`, big-endian, 21 bytes)**, sent only for frames that needed FEC or were lost. The first
    version listened to loss stats (`0x0201`) instead, which moonlight-common-c never sends to a
    Sunshine host (it switches to periodic pings from appversion 7.1.415; the `0x0201` branch asserts
    `!IS_SUNSHINE()`), so both features silently did nothing. `0x0201` is now logged only.
  - IDR and reference-frame-invalidation requests also count as lost frames.
  - Repaired losses do not raise FEC; lost frames and *near misses* (repaired with more than half the
    parity) do. FEC +10 after two troubled 1 s windows, −5 after 10 quiet seconds, between
    `min(fec_percentage, 10)` and `max(fec_percentage, 55)`. FEC is sent **on top of** the video
    bitrate (Sunshine's behaviour, deliberate).
  - Bitrate −20% only when frames are still lost with FEC at its ceiling, +10% of negotiated after
    15 quiet seconds. Applied through `encode_session_t::update_bitrate()` →
    `NvEncReconfigureEncoder` (rate control only, no reset / IDR) via `mail::bitrate_change`. **Only
    the standalone NVENC session on Windows implements it**; `video::encoder_supports_bitrate_change()`
    tells the controller up front, so other encoders adapt FEC only.
  - `videoBroadcastThread` asks the controller for each frame's FEC; the control thread feeds it.
  - ⚠️ **No controller for PyroWave sessions** (`session::alloc()`): their FEC stays 0 apart from the
    critical-packet parity, the bitrate never adapts, and an overflow drops frames without asking
    for an IDR, since every PyroWave frame is intra.
  - The `0x5502` handler also feeds the session history: each frame the client could not rebuild
    adds one to `session_t::stats.client_reported_losses`, for every session. `0x0201` used to feed
    that counter and so left it at 0 for every Sunshine client.
    Both go through `session->video.adaptive_mutex`.
- **Emulated multi-display** — a client can send `displayLayout=` on `/launch` (`x,y,w,h,primary`
  per display, in the coordinate space of the `mode=` it already sends) and get one virtual display
  per monitor instead of one big one. `launch_session_t` carries a `std::vector<virtual_display_t>`;
  a single-display client just gets a one-element list, so creation, arrangement and teardown have
  one code path. Advertised to clients as `MultiDisplayCapable` in `serverinfo`.
  - ⚠️ `displayLayout=` is untrusted input: `nvhttp::parse_display_layout()` (portable, tested in
    `tests/unit/test_display_layout.cpp`) parses with `std::from_chars` and caps every field at
    `MAX_LAYOUT_COORDINATE` (16384) *before* any addition — `atoi` + `x + width` used to overflow.
  - If a display fails to come up **or** Windows refuses the arrangement, the session falls back to
    one display covering the whole canvas (`fall_back_to_single_display` in `process.cpp`).
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
  - ⚠️ **`stop()` runs on HTTPS request threads** (`/cancel` and `/launch` reach `terminate()`): any
    wait in it takes the whole nvhttp HTTPS server down with it, and every client then sees the host
    as **offline** while it is still running (v2.2.0 shipped exactly that). So it waits at most 2 s,
    then detaches a monitor stuck in a Windows display call; a detached monitor checks its stop token
    before acting and exits on its own. `test_virtual_display_recovery_stop.cpp` holds both cases.
  - ⚠️ **`cv.wait_for(lock, stop_token, rel_time, pred)` returns `pred()`, not "stop requested".**
    With a `[] { return false; }` predicate it reports `false` even after `request_stop()`: the
    monitor's wait used that result and spun forever on `EnumDisplayDevicesW` once stopped. Read
    `stop_token.stop_requested()` after the wait.
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
- Video queue drops the oldest frames on overflow instead of flushing, and requests an IDR at most
  every 500 ms while it does (one per dropped frame turned sustained overflow into an IDR storm).
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
The v2 UI (`/v2`) needs **nothing extra**: its settings page is generated at build time from
`config.html` + the `.vue` tabs (`web-v2/scripts/generate-settings.mjs`), labels included.

- **Web UI v2** (`src_assets/common/assets/web-v2/`, served at `/v2`; Vibepollo's v2 UI). The current UI
  stays at `/` for now; v2 is meant to replace it eventually.
  - **Build**: the `web-ui` target builds it **after** the current UI. The current UI's build empties
    `assets/web`; this one only empties `assets/web/v2`. The output goes to `SUNSHINE_WEB_OUTPUT_DIR`.
    Packaging already ships all of `assets/web`.
    ⚠️ **`cmake/packaging/common.cmake` installs `src_assets/common/assets/` minus `web` *and*
    `web-v2`.** Without the second exclusion the v2 sources and their `node_modules` (82 MB, `.exe`
    files included) went into the installer, and NSIS failed on the CI (release v2.0.0). Any new
    source folder there needs the same exclusion, in the `install()` and in the build-dir copy.
  - **Serving**: `getWebV2()` serves `^/v2(/.*)?$` without authentication, like the current UI's
    `/assets`; every API call still authenticates. A path that is not a file returns `index.html` (SPA
    routes), except under `assets/` and `images/`. Hashed bundles are cached as immutable;
    `index.html` and `.json` files are not.
  - ⚠️ **Settings are generated, not hand-written.** `generate-settings.mjs` reads the current UI's
    tabs: one category per tab, encoder tabs merged into "Encoders". Field kinds come from the controls:
    `<Checkbox>` → boolean, `<select>` options with their `<template #platform>`, `type="number"`
    bounds. Special editors are set in `specialKinds`. Labels resolve through `config.<key>` in the
    current catalogue. Vibepollo's hand-written schema described options LavApollo does not have.
  - **Strings**: three catalogues, later ones win. `web-v2/public/assets/locale/fallback/en.json`
    (Vibepollo's strings, for keys LavApollo lacks), then `/assets/locale/<locale>.json` (the current
    UI's, the single source for settings labels), then `web-v2/public/assets/locale/ui/en.json` (strings
    only v2 uses).
  - **Application editor rewritten** (`views/ApplicationView.vue`). Vibepollo's 5,900-line editor
    edits its own app model: Playnite/Steam/Lutris links, Lossless Scaling profiles, RTX HDR,
    `display-output`, `config-overrides`. Porting it would have offered settings that silently vanish.
    The new editor edits exactly the fields `process.cpp` reads, like `apps.html`, and keeps unknown
    fields when saving. The device editor was cut down the same way: no output override, virtual
    display mode or layout, HDR profile or per-client overrides. Only "always create a virtual display"
    maps to `always_use_virtual_display`.
  - **Removed, not ported**: browser streaming (WebRTC), API tokens, integrations
    (Playnite/Steam/Lutris/MangoHud), update notices and release notes, the display layout editor
    (conflicts with the client's `displayLayout`), crash dumps, display-helper snapshots, and Linux
    capture status.
  - **Server routes added for it**: `/api/auth/status` (also refreshes an expired session token),
    `/api/auth/login` (alias of `/api/login`), `/api/metadata`, `PATCH /api/config` (merges keys and
    writes them sorted; `restartRequired: true`, since LavApollo reads its config at startup),
    `/api/display-devices`, `/api/health/vigem`, `DELETE /api/apps/{uuid}`, `/api/apps/{uuid}/cover`, and
    `GET /api/logs?tail=N`.
  - `api/client.ts` refreshes on 401 once, sharing one in-flight refresh, like `init.js` in the current
    UI. It sends `X-CSRF-Token` from `/api/csrf-token` on every mutation, which `check_csrf()` accepts.

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

- **Remote Monitor** (`config.remote_monitor`, default on; after Vibepollo). An app-list entry next to
  "Remote Input": launched from another device, it gives that device **a SudoVDA display of its own**,
  captured and encoded on its own, while the main session goes on. Several clients streaming at once
  were already possible, but they all shared the one capture of `proc::proc`'s display.
  - The display is created in `proc::create_remote_monitor_display()` (GUID = client UUID xor a
    constant: stable per client, distinct from its game display), passed as
    `launch_session_t::capture_output` → `video::config_t::capture_output`, and removed by
    `release_remote_monitor_display()` when the session ends (`stream.cpp` `join()`).
  - ⚠️ **A non-empty `capture_output` takes the session off the shared capture thread**:
    `capture_async()` starts a capture thread of its own (`start_capture_async`/`end_capture_async`
    on a local context), and `captureThread()` opens exactly that display -- no fallback to another
    display, never reads or writes `proc::proc`'s display name, and ignores `switch_display` /
    `recapture_display`, which belong to the main session. `capture()` always goes async for it.
  - It does not touch the running app, the layout or the primary display; Windows places the new
    display, and a main session's composite rebuilds once through `topology_changed()`.
  - Each such session runs its own encoder: GPU encode sessions are the practical limit.
  - `remote_monitor_mute_audio` leaves its audio out (`config.audio.input_only`).
  - Vibepollo's "Remote Input" is LavApollo's existing input-only mode; nothing to port there.

- **VHF virtual gamepad** (after Vibepollo; `docs/vhf-gamepad.md`). A second gamepad driver beside
  ViGEmBus: Nonary's libvirtualgamepad. The client protocol comes from the `third-party/libvirtualgamepad`
  submodule; the driver is the separately released archive, bundled by the installer. New `gamepad`
  values: `vhf` (follows the client's controller), `vhf_xbox`, `vhf_xbox_one`, `vhf_ds4`, `vhf_ds5`,
  `vhf_switch`. `auto` falls back to VHF only when ViGEmBus is missing.
  - `input_raw_t::gamepad_backend[]` records **per slot** which driver created a gamepad. Every
    update, touch, motion, battery and free call dispatches on it, never on the current config:
    both drivers can own slots in the same session.
  - x360/ds4 are now reported disabled when ViGEmBus is not installed (`vigem_t::available()`).
    Before, `vigem_t::init()` always succeeded, so they always showed as available.
  - ⚠️ **The archive is unsigned. CI signs it with LavApollo's self-signed certificate**
    (`scripts/prepare-vhf-gamepad.ps1`; secrets `VHF_SIGNING_CERT_BASE64` / `VHF_SIGNING_CERT_PASSWORD`,
    created by the user with `scripts/generate-vhf-signing-cert.ps1`). The catalogue and the setup
    tool are signed; the driver DLL **must not be**, because the catalogue hashes it.
    `drivers/vhf-gamepad/install.ps1` adds the `.cer` to LocalMachine Root + TrustedPublisher, like
    SudoVDA. Every such PC trusts that key, so it must live only in the secrets.
  - Without the secrets the release still builds, without the driver, and the workflow warns.
    `LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR` is empty, so CMake installs nothing and NSIS skips it
    (`IfFileExists`).
  - The archive URL and SHA-256 (`prepare-vhf-gamepad.ps1`) and the submodule commit move **together**.
  - `install.ps1` is always best effort (exit 0): a failing gamepad driver must not fail the installer.

- **Web UI logins** (`src/web_auth.{h,cpp}`, after Vibepollo's session tokens, **without its API tokens**).
  The previous login was one in-memory cookie hash, so a second browser logged the first out and a
  restart logged everyone out. "Remember Password" kept the password itself in `localStorage`; `login.html`
  now deletes that key.
  - Login issues a **session token** (`session_token_ttl_seconds`, 2 h) and a **refresh token**: 24 h, or
    `remember_me_refresh_token_ttl_seconds` (7 days) with "Remember me". Both are sent as `__Host-` cookies,
    `HttpOnly; Secure; SameSite=Strict`. Without "Remember me" they are browser-session cookies.
  - Each refresh replaces **both** tokens, and the old refresh token stops working. The refresh lifetime
    **does not slide**. A login keeps a stable `id` across refreshes, which revocation uses
    (`DELETE /api/auth/sessions/{id}`, listed on the password page).
  - ⚠️ **Stored in `lavapollo_auth.json`** next to the state file (token hashes only, atomic write with
    `MoveFileExW`). **Never in `sunshine_state.json`**: `nvhttp::save_state()` rebuilds that file from scratch.
  - Expiry handling:
    - A **page** load with an expired session token but a good refresh cookie is refreshed inside
      `authenticate()`, which 307-redirects to the same URL with the new cookies.
    - An **API** call gets a 401. `init.js` wraps `window.fetch`: on a 401 it calls
      `/api/auth/refresh` once, shared by concurrent calls (two refreshes in flight would spend the same
      cookie), then retries. If the refresh fails, it sends the browser to the login page.
  - `savePassword` revokes every login.
  - From the 3rd failed login, the address is locked out for 1 s, 2 s, 4 s… up to 30 s (`login_lockout_remaining()`,
    a bounded in-memory table). This covers `/api/login` and its v2 alias `/api/auth/login`, which is
    the same handler. A success clears the address.
  - ⚠️ **CSRF**: `start()` wraps **every** POST/PUT/PATCH/DELETE route in `check_csrf()` after
    registration, so routes added later are covered too. A request passes when:
    - it has no `Origin` and no `Referer` (scripts);
    - its origin is `https://` + the request's own `Host`. This is what keeps the UI working when opened
      by LAN IP, host name or localhost;
    - its origin is listed in `csrf_allowed_origins`;
    - or it carries a valid `X-CSRF-Token` from `GET /api/csrf-token` (per client IP, 1 h).

- **Host stats + session history** (after Vibepollo, ported nearly verbatim minus WebRTC). The web UI
  reads them through `/api/host/{stats,info}`, `/api/rtsp/sessions`, `/api/session/status` and
  `/api/history/sessions[/active|/{uuid}]`. The current UI does not display them; the v2 UI's Stats page does.
  - **Host stats** (`host_stats*.{h,cpp}`, `platform/windows/host_stats.cpp`): CPU via `GetSystemTimes`,
    GPU and encoder via PDH `GPU Engine`, VRAM via PDH, then D3DKMT, then NVML. Temperatures and
    network are also sampled. One sampler thread, faster while a session streams.
    - ⚠️ NVML is locked out once D3DKMT has answered once: NVML faults inside `nvml.dll` during a TDR.
    - `-1` means "not available". Non-Windows builds get an empty provider.
  - **History** (`session_history*.{h,cpp}`): SQLite at `lavapollo_history.db` next to the state file,
    with tables `sessions`, `samples` (one per second) and `events`. A single writer thread with bounded
    queues, so the stream never waits on disk.
    - `session_t::stats` holds atomic counters. The broadcast and control threads write them; the
      sampler reads them through `stream::get_all_session_info()` ← `rtsp_stream::get_sessions_snapshot()`.
    - `history_uuid` is **one per session**, not the client UUID. `begin_session` runs in `start()` and
      `end_session` in `join()` after the threads, both skipped for input-only sessions.
    - Pruning: `session_history_ttl_days` (90) and `session_history_db_size_limit_mb` (256).
      Vibepollo's defaults are 0 = unbounded.
  - ⚠️ `libsqlite3.a` is linked **statically** (`windows.cmake`), like the C++ runtime: the release
    ships no `sqlite3.dll`. CI installs `mingw-w64-ucrt-x86_64-sqlite3`.
  - ⚠️ `session_history.cpp` / `session_history_sampler.cpp` include `stream.h` **first**: asio needs
    WinSock2 before anything pulls in `windows.h`. Same reason `platform/windows/host_stats.cpp` opens
    with `<winsock2.h>`, or `netioapi.h` hides `MIB_IF_ROW2`.

- **Log export** (`GET /api/logs/export`, button in Troubleshooting) — a ZIP of the current log, the
  previous run's log (`<log>.backup`, rotated by `logging::init()`) and the config file, with user names,
  IP/MAC and e-mail addresses replaced by stable placeholders (`<IP_1>` is the same client everywhere in
  the bundle). `src/log_export.h`, ported from Vibepollo; the ZIP is written by hand over zlib, no archive
  library. It does **not** recognise passwords or tokens: never add a file that holds credentials
  (`credentials_file`, `file_state`, keys) to the bundle.

- **Client microphone passthrough** (`config.mic_passthrough`, default on; `mic_sink`, `mic_set_default`;
  `docs/mic-passthrough.md`). A LavArtemis client that opts in (off by default on the client) sends its
  microphone, which the host plays into a virtual microphone (Steam Streaming Microphone, or `mic_sink`)
  for voice chat and proximity chat. Written for LavArtemis; the open Apollo PR #1428 had the idea of
  Steam's microphone, not the transport.
  - **Transport: the encrypted control stream, not a UDP port.** Control message `0x3080`
    (`IDX_MICROPHONE_DATA`, LavArtemis range: Apollo is at `0x3000-0x3002`), payload `u16 LE seq | u32 LE
    timestamp | Opus frame (48 kHz mono, 20 ms)`, unsequenced on ENet channel 9. It arrives already
    decrypted and authenticated by `IDX_ENCRYPTED`; the handler also refuses anything when
    `controlProtocolType != 13`, so voice never crosses the network in the clear. A dedicated port would
    have meant a firewall rule, a NAT hole, its own crypto (Apollo #1428 reuses the audio IVs with the same
    key) and finding the session by IP. The client side is `LiSendMicrophoneData()` in
    `Lavagnou/moonlight-common-c`.
  - **Negotiation.** `serverinfo` publishes `MicPassthroughState` per paired client: `ready` / `disabled` /
    `no_permission` / `no_device` (`mic_passthrough::state()`). The client sends `clientMicRequested=1`
    only on `ready`, and warns with the reason otherwise.
  - ⚠️ **`state()` never touches the audio stack**: it reads an atomic that `mic_passthrough::init()`'s
    background thread refreshes every 3 s. serverinfo runs on the HTTPS thread, and anything that can
    block there takes the host offline for every client (see the recovery monitor above).
  - **Permission `PERM::microphone`** (`_operation << 5`, `0x00200000`), checked at launch **and on every
    frame**, so unticking it silences a client mid-stream. Devices saved before it existed get it once if
    they hold every operation the UI shows (`clipboard_set | clipboard_read | server_cmd`); the per-device
    `"perm_version": 2` written by `save_state()` records that, so unticking it afterwards sticks.
  - **One owner.** The first session that asks holds the virtual microphone (`mic_passthrough::acquire()`
    in `session::start()`), until `join()` releases it once the control thread is gone. Others are ignored
    with a log line.
  - **Playback** (`src/platform/windows/mic_passthrough.cpp`): Opus decode, loss rebuilt from the next
    frame's in-band FEC (one frame) and PLC (more, up to 5), then WASAPI shared mode, event driven, in
    **mono float 48 kHz with `AUTOCONVERTPCM`**: Windows converts to the device format.
  - ⚠️ **Both sides of the virtual microphone must share one device format.** Steam Streaming
    Microphone is a loopback that copies the playback side's samples to the recording side as raw bytes,
    with no conversion, and out of the box its sides disagree (playback stereo 48 kHz, recording mono
    44.1 kHz): applications then read stereo as mono at the wrong rate, and the voice comes out garbled
    and unintelligible although every frame arrived (the 2.2.1 test). `match_formats()` gives the
    playback side the recording side's format for the session (fallback: the other way round) and
    `release()` puts the original back. Thinking "never touch the device format" was wrong: the Apollo
    PR's normalisation of Steam's pair was there for this.
  - **Jitter buffer** (`src/mic_passthrough_policy.h`, tested in `test_mic_passthrough.cpp`): plays once
    40 ms are buffered, re-primes after running dry, drops back to 40 ms past 150 ms. The client's capture
    clock never matches ours, so the overflow rule is what keeps latency bounded over a long session.
    Late or duplicate frames (unsequenced) are dropped, never played out of order.
  - **Default microphone.** With `mic_set_default`, the recording side (the capture endpoint on the same
    adapter: "Microphone (Steam Streaming Microphone)", "CABLE Output") becomes the default for every
    role while streaming. The previous default is restored per role, **unless** the user picked another
    one meanwhile.
  - **Disabled endpoints.** Steam's pair is often turned off in the Sound settings (it clutters the
    device lists): an endpoint disabled there is `DEVICE_STATE_DISABLED`, invisible to the usual
    `DEVICE_STATE_ACTIVE` lookups, which is why the first test reported `no_device` with Steam installed.
    `find_mic_endpoints()` now finds them too (flagged), `acquire()` enables them with
    `IPolicyConfig::SetEndpointVisibility` for the session and `release()` disables **only those** again.
  - **Driver.** `install_steam_audio_drivers` also installs `SteamStreamingMicrophone.inf` (next to the
    speakers' INF in Steam's `drivers\Windows10\x64`), from `mic_passthrough::init()`'s thread, only
    when `mic_sink` is empty. "Installed" means present in **any usable state**, for the speakers too:
    reinstalling over a disabled endpoint fails with error 259 (`ERROR_NO_MORE_ITEMS`), which upstream
    logged on every stream.
  - `find_device_id()` takes an `EDataFlow` (default `eRender`) and a `DEVICE_STATE_*` mask (default
    active only); `audio_endpoints.h` exposes the endpoint lookups to `mic_passthrough.cpp`. Those need
    COM on the calling thread.

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
release, triggered by pushing a `v*` tag or by manual dispatch with a version input. There is no CI
test job: run `test_sunshine` locally.

### In-app updater (`src/updater.cpp`, Web UI home page)

- ⚠️ **The staged installer runs elevated and silent**, so it is staged in an `updates` folder next
  to the executable, recreated per download with a SYSTEM + Administrators DACL, under a random name
  — never `%TEMP%`, which any local user can write to. Right before `runas`, the file is opened
  without write/delete sharing and hashed again through that handle.
- The release's `LavApollo-<version>-SHA256SUMS.txt` is **mandatory**: no checksum, no install.
- `PUBLISHER_THUMBPRINT` is empty while releases are unsigned. Filling it in (with signtool in
  `release.yml`) makes the Authenticode check in `verify_publisher()` mandatory.
- Staged installers are deleted at the next startup (`cleanup_staged_installers()`): the process
  that launches the installer is the one being replaced.
- The Web UI's POSTs must send `Content-Type: application/json` (CSRF guard in
  `validateContentType()`); without it the endpoints answer 400.

### Startup

mDNS/UPnP advertising waits for the HTTP, HTTPS and RTSP servers to report a successful bind
(`net::report_streaming_bind()`), not just for the pre-flight port probe. The probe uses
`SO_EXCLUSIVEADDRUSE` on Windows, where `SO_REUSEADDR` would let it bind a port another process
is using. Failed Web UI logins lock the address out for 1 s, 2 s, 4 s… up to 30 s after three
misses (the Web UI runs on a single thread, so the handler never sleeps).
