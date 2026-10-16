# Benchmark

LavArtemis clients (desktop and Android) can compare their streaming settings on a LavApollo host with
real numbers instead of impressions: right-click (long-press on Android) a host, then **Benchmark**. The
client streams the same animated scene several times in a row, each time with another codec, bitrate or
frame pacing mode, and ends with a report giving every combination a **latency score** and a
**smoothness score**, compared with the settings the user streams with today.

This page is the reference both clients implement: what the host provides, how runs are planned, what is
measured and how the scores are computed. Scores use absolute formulas, so reports stay comparable over
time (after a driver update, on another network...).

## The host side

- *Configuration → Audio/Video → Benchmark app for LavArtemis* (`benchmark_app`, on by default) offers a
  built-in app called **Benchmark**. It runs `tools\lavapollo-benchmark-scene.exe` on a virtual display the
  size and rate of the stream, exactly as a game would get it, the 1000 Hz display of VRR clients included.
- The app is **not listed by `/applist`** while it is not running, so it never clutters an app grid, old
  clients included. `serverinfo` advertises it to paired clients instead:

  | Field | Value |
  |---|---|
  | `BenchmarkAppId` | Its app id, as `currentgame` reports it while it runs |
  | `BenchmarkAppUuid` | `B3C4F1A2-7E5D-4B8A-9C61-2D0E8F4A6B37` |

  Both are absent when the host does not offer it. While it runs it is listed, so that a benchmark cut short
  can still be resumed or quit from any client.
- **The scene is a function of the frame number alone.** A panning field of fine detail, a rotating zone
  plate, scrolling lines of "text", bouncing particles and per-frame film grain, alternating 5 s of calm and
  5 s of heavy motion so that bitrate makes a difference. It paces itself at `SUNSHINE_CLIENT_FPS` with a
  high-resolution timer, skipping (never delaying) a frame that misses its slot, and covers the display
  capture reads, which the host names in `LAVAPOLLO_CAPTURE_DISPLAY`. The bottom-left corner
  carries the frame number as 16 black/white cells of 12 px.

**Between runs**, a client resumes the running Benchmark app with the next settings (`/resume` renegotiates
codec and bitrate). Requesting VRR or not changes the host's virtual display, which is only created at
`/launch`, so the client quits the app (`/cancel`) whenever the next run's VRR request differs from the
previous one's, and runs are ordered to keep VRR runs together. It also quits whatever runs on the host
before the first run (another app would refuse the launch; a scene left by an earlier benchmark may have the
wrong display) and after the last one.

## Runs

A run streams for a **5 s warm-up**, ignored while the encoder, FEC and the VRR pacer settle, then measures
for **15 s** (Quick) or **20 s** (Full, Adaptive). Snapshots taken every 5 s of measurement split it into
sub-windows, whose scores give the "±" shown next to each score.

A snapshot is the client's cumulative stats as of its last one-second stats window, stamped with that
window's own clock: two snapshots subtract into exactly the time between them, however late the timer that
took them. The run then ends on its own and the next one starts.

Each run is described by a configuration:

| Field | Values |
|---|---|
| `codec` | `h264`, `hevc`, `av1`, `pyrowave` |
| `bitrateKbps` | integer |
| `pacing` | desktop: `vsync-off`, `vsync`, `vsync-paced`, `vrr-low-latency`, `vrr-balanced`, `vrr-smooth` · Android: `default`, `ull`, `paced-ull`, `predictive`, `vrr-low-latency`, `vrr-balanced`, `vrr-smooth` |

The **baseline** run streams with the user's own settings, untouched; its `pacing` is the closest name for
them. Every run records the configuration **requested** and the one **in effect**: clients fall back
silently (AV1 → HEVC → H.264 without a hardware decoder, PyroWave → H.264, VRR refused by the display). A run
whose effective codec or VRR differs from the request is marked `fallback` and does not count; the
remaining runs that would fall back the same way are skipped.

### Modes

Codecs are first filtered by what the host advertises (`ServerCodecModeSupport`); what the client cannot
decode shows up as a fallback on its first run. PyroWave only on the desktop client, when it supports it:
Android decodes it natively, out of reach of the stats the benchmark reads. Bitrates are rounded to 500 kbps and clamped to the
client's range. *Default* means the client's default bitrate for the stream's resolution and rate.

- **Quick**: baseline · every other codec at the baseline bitrate · the first two pacing modes of the
  platform list that differ from the baseline's, on the baseline codec · baseline again.
- **Full**: baseline · every codec at 0.5×, 1× and 2× the default bitrate (PyroWave at its own default only)
  · every pacing mode on the baseline codec and bitrate · baseline again.
- **Adaptive**, towards a goal (latency, smoothness or balanced):
  1. every codec at the default bitrate, baseline pacing; keep the best;
  2. on that codec (unless PyroWave): 0.5× the default, then 1.5×, 2× and 3×, climbing until network loss
     exceeds 1% or the goal score falls below the best twice in a row; keep the best bitrate;
  3. every pacing mode on that codec and bitrate; keep the best;
  4. the winner and the baseline again, to check the difference beats the noise.

Platform pacing lists, in this order: desktop `vsync-paced`, `vsync-off`, `vsync`, `vrr-low-latency`,
`vrr-balanced`; Android `ull`, `paced-ull`, `predictive`, `default`, `vrr-low-latency`, `vrr-balanced`.
A run never repeats an earlier one's configuration, except the planned repeats. Quick and Full order their
middle runs to keep VRR runs last.

## Measurements

| Metric | Desktop | Android |
|---|---|---|
| `hostMs` | average `frameHostProcessingLatency` (capture to packet) | same |
| `networkMs` | half the average RTT (`LiGetEstimatedRttInfo`, sampled every second) | same |
| `receiveMs` | average time from a frame's first packet to its last (`enqueueTimeUs - receiveTimeUs`): what bitrate costs | same |
| `decodeMs` | average decode time | enqueue → dequeue |
| `queueMs` | average frame queue delay (pacer) | dequeue → the time the frame is handed to SurfaceFlinger to appear at: a vsync wait, two for predictive pacing, the learned delay for VRR pacing |
| `renderMs` | average render time, V-Sync wait included | 0: SurfaceFlinger's composition and the panel's scan-out can't be observed |
| `totalMs` | sum of the six | |
| `framesRendered`, `framesExpected` | frames shown; stream rate × measured time | |
| `networkLossPct` | frames lost on the network ÷ frames sent | |
| `presentP50Ms`, `presentP95Ms`, `presentP99Ms` | percentiles of present-to-present intervals over the whole run (0.25 ms histogram up to 100 ms); desktop uses the real scan-out time when D3D11 gives it | |
| `stutterPct` | intervals over 1.5 frame periods ÷ intervals | |

## Scores

```
period      = 1000 / targetFps                                  (ms)
latency     = 100 · exp(-totalMs / 50)
delivered   = min(1, framesRendered / framesExpected)
jitter      = max(0, presentP99Ms - presentP50Ms) / period
smoothness  = 100 · delivered² · exp(-20 · stutterPct / 100) / (1 + jitter)
balanced    = (latency + smoothness) / 2
```

Scores are rounded to one decimal, half away from zero, and `balanced` is computed from the two rounded
scores. Reference values, checked by both clients' tests:

| totalMs | latency |   | delivered | stutterPct | p50 / p99 (60 fps) | smoothness |
|---|---|---|---|---|---|---|
| 10 | 81.9 | | 1.0 | 0 | 16.7 / 16.7 | 100.0 |
| 20 | 67.0 | | 1.0 | 1 | 16.7 / 16.7 | 81.9 |
| 40 | 44.9 | | 0.9 | 0 | 16.7 / 16.7 | 81.0 |
| | | | 1.0 | 0 | 16.7 / 25.0 | 66.8 |

When the baseline's balanced score moves by more than 5 points between its first and last run, the report
warns that conditions were unstable.

Android totals miss the render stage, so an Android and a desktop report of the same host are not
comparable score for score; each compares its own runs.

## Report

Reports are JSON, the same on both clients, so either opens the other's. Desktop keeps them in
`<AppData>/benchmarks/`, Android in its private `files/benchmarks/`, 50 at most.

```json
{
  "schema": "lavartemis-benchmark",
  "version": 1,
  "id": "6f1c…",
  "createdAt": "2026-10-16T22:40:00Z",
  "mode": "quick | full | adaptive",
  "goal": "latency | smoothness | balanced",
  "host": { "uuid": "…", "name": "ATLAS-PC" },
  "client": { "platform": "windows | linux | android", "device": "…", "appVersion": "21.3.0" },
  "stream": { "width": 1920, "height": 1080, "fps": 60 },
  "warmupSeconds": 5,
  "measureSeconds": 20,
  "cancelled": false,
  "baselineDrift": 1.8,
  "runs": [
    {
      "role": "baseline | candidate | baseline-repeat | confirm",
      "status": "ok | fallback | failed | skipped",
      "note": "",
      "requested": { "codec": "hevc", "bitrateKbps": 40000, "pacing": "vsync-paced" },
      "effective": { "codec": "hevc", "bitrateKbps": 40000, "pacing": "vsync-paced" },
      "metrics": { "hostMs": 4.1, "networkMs": 1.2, "receiveMs": 2.3, "decodeMs": 1.9, "queueMs": 1.0,
                   "renderMs": 6.4, "totalMs": 16.9, "rttMs": 2.4, "rttVarianceMs": 0.3,
                   "targetFps": 60, "measuredSeconds": 20.0, "framesRendered": 1198, "framesExpected": 1200,
                   "networkLossPct": 0.0, "presentP50Ms": 16.7, "presentP95Ms": 17.1, "presentP99Ms": 18.0,
                   "stutterPct": 0.1 },
      "scores": { "latency": 71.3, "smoothness": 92.4, "balanced": 81.9 },
      "spread": { "latency": 0.8, "smoothness": 1.6 }
    }
  ]
}
```

Unknown fields are ignored on reading, so later versions can add metrics without breaking older readers.
