# Host data for LavArtemis Dev (prototype screens)

What LavApollo tells the LavArtemis Dev clients so they can show the prototype's screens: hours played,
installed or not, the game's background, « depuis 42 min », the kind of PC and its link. Every field is a
**compatible addition**: older clients ignore it, and the clients treat a missing field as "unknown"
(older hosts, other hosts). Added on 2026-11-26 (verification pass, wave 5); the clients' side is in
`LavArtemis/CLAUDE.md` and `LavArtemis-Qt/CLAUDE.md`.

## `/applist` (with `playnite=1`), per Playnite game (`IsPlayniteGame=1`)

| Element | Value | Absent |
|---|---|---|
| `PlayniteInstalled` | `1` installed, `0` not installed | game left `library.json` since the list was built |
| `PlaynitePlaytime` | Playnite's Playtime, seconds | same |
| `PlayniteLastActivity` | last play, Unix seconds (UTC) | never played |
| `PlayniteBackground` | `1`: `/appasset?asset=background` serves an image | no background |

The games come **most recently played first** (never played last, by name), in the order of the library
**as it is now**: `playnite::library_snapshot()` re-reads `library.json` when it changes (checked at most
every 2 s), and `/applist` orders the game apps and takes their state from it. The app list itself
(`proc::refresh()`) is still rebuilt only when nothing runs, because it replaces the running app: a game
added to Playnite during a session appears after the session, but a game played during a session moves up
at once. Before this, the order froze while any app ran, and the user always leaves Playnite Fullscreen
running: the « Récents » never moved (the bug of 2026-11-26).

The clients sort the games on `PlayniteLastActivity` themselves (same rule; a host without it keeps its
own order), so the order does not depend on the host's list being fresh.

### Games that are not installed

LavApollo Link 1.1.0 exports every game that is not hidden, installed or not (`installed` in
`library.json`; missing = installed, what 1.0.x wrote). They are listed to `playnite=1` clients only, like
every Playnite game. Launching one goes through the same `playnite://playnite/start/<id>` URI: Playnite
then **installs** it instead (`GamesEditor.PlayGame` calls `InstallGame`, the game's library plugin shows
its install dialog on the PC). Nothing is installed without Playnite's own dialog. The session of such a
game waits for it for `playnite::install_grace` (24 h) instead of 120 s, so the stream stays up during
the install; it ends when the client quits it, or when the game, started from the PC afterwards, closes.

## `/appasset?appid=<id>&asset=background`

The background image of a Playnite game: a JPEG (`backgrounds\<id>.jpg`, re-encoded by LavApollo Link at
most 1920 × 1080). **404** when the game has none: never the cover nor `box.png`. A host without this
ignores `asset` and sends the cover, which is why clients ask only for games flagged
`PlayniteBackground=1`. Same permission as covers.

## `serverinfo` (paired HTTPS answer)

| Element | Value | Absent |
|---|---|---|
| `CurrentGameElapsed` | seconds since the running app was launched | nothing runs |
| `PlayniteGameElapsed` | seconds since Playnite started the game in `PlayniteGameName` (`state.json` `startedAt`) | no game, or no `startedAt` |
| `HostFormFactor` | `tower`, `mini` or `laptop`, from the SMBIOS chassis type (`platf::chassis_form_factor()`) | unknown chassis, not Windows |
| `HostLink` | `wired` or `wifi`: the interface that routes to this client (`routed_link_bps()`'s interface type) | other interfaces (loopback, VPN, tunnel), not Windows |
| `OtherClientsConnected` | how many stream sessions belong to **other** paired clients than the one asking (`nvhttp::count_other_clients()` over `rtsp_stream::get_all_session_uuids()`, by certificate UUID). The asker's own session, if it polls while streaming, does not count | always sent (0) on this host; absent on other hosts |

Elapsed times rather than dates: the client's clock needs not agree with the host's. Clients turn them
into a date on their own clock at each poll and keep the previous one unless it moved by more than 5 s.

Chassis types: laptop = Portable, Laptop, Notebook, Hand Held, Sub Notebook, Tablet, Convertible,
Detachable (8, 9, 10, 11, 14, 30, 31, 32); mini = Lunch Box, Embedded PC, Mini PC, Stick PC (16, 34, 35,
36); tower = Desktop, Low Profile Desktop, Pizza Box, Mini Tower, Tower, All in One, Space-saving,
Sealed-case PC (3, 4, 5, 6, 7, 13, 15, 24). Many mini PCs declare "Desktop": they show as a tower.

### « Reprendre » or « Rejoindre » (added 2026-11-26, LavArtemis choice 23)

The clients say « Rejoindre » (and « Session rejointe ») instead of « Reprendre » when
`OtherClientsConnected` is not 0: someone else streams the session they would join. A host that does
not send it is taken as 0 (« Reprendre », what the clients always said before). Counted per stream
session: one device streaming twice (two windows) counts twice, which never happens in practice.

## Latency: measured by the clients

The « En ligne · 4 ms » of the prototype is the round trip the **client** measures: after each successful
`serverinfo` poll, one TCP connect to the same address and port, timed (at least 1 ms). A whole
`serverinfo` request would add TLS and the host's work. Nothing on the host.

## Test host

`test_host_form_factor` / `test_host_link` (config) replace the machine's values in `serverinfo` (the
testbench's `pc.json` `form_factor` / `link`); the fake library's `background` files are generated by the
testbench. The test host's Playnite launch writes `startedAt` into the fake `state.json`.

`POST /api/test/session` (`{"other_clients": N, "elapsed_offset": S}`, test host only, 127.0.0.1, like the other
`/api/test/*` routes): `serverinfo` adds N fake devices to `OtherClientsConnected` and S seconds to
`CurrentGameElapsed` (the running app started that much earlier: « depuis 42 min » in the testbench's captures).
Both 0 by default; testbench `hosts.set_session()`.
