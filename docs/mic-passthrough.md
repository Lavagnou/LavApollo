# Client microphone passthrough

A LavArtemis client (desktop or Android) can send its microphone to LavApollo, which plays it into a
virtual microphone on the host. Voice chat applications and games with proximity chat then hear the
player as if they were sitting at the host.

## Setting it up

**On the host**
- *Configuration → Audio/Video → Accept client microphones* is on by default.
- The virtual microphone is **Steam Streaming Microphone**. When Steam is installed, LavApollo installs
  its driver at startup, like the Steam Streaming Speakers (*Install Steam Audio Drivers*, needs
  administrator rights). It appears as *Speakers (Steam Streaming Microphone)*, which LavApollo plays into,
  and *Microphone (Steam Streaming Microphone)*, which applications record from.
- Any other virtual cable works: set *Virtual microphone device* to its playback side, e.g. `CABLE Input`
  for VB-Cable. Applications then record from `CABLE Output`.
- While a microphone is streaming, the virtual microphone becomes Windows' default recording device,
  then the previous one comes back (*Make it the default microphone while streaming*).
- Each client needs the **Microphone** permission (*Clients* page). Devices that already had every other
  operation permission get it automatically when upgrading.

**On the client** (LavArtemis), turn on *Send my microphone to the host*. It is off by default.
- Desktop: *Settings → LavArtemis Features → Microphone*, with a choice of microphone. Mute it while
  streaming with **Ctrl+Alt+Shift+U** or from the quick menu; "Microphone muted" stays on screen until
  unmuted. Use a headset: the desktop client has no echo cancellation.
- Android: *Settings → Audio*. The microphone permission is asked for when the setting is turned on.
  Mute it from the in-game menu, or with Ctrl+Alt+Shift+U on a keyboard. Android's voice capture cancels
  the echo of the device's own speaker.

If the host can't take the microphone, the client says why when the stream starts, and the stream
carries on without it.

## Protocol

| Step | What happens |
|---|---|
| `GET /serverinfo` | The host answers `MicPassthroughState` for the requesting client: `ready`, `disabled` (host setting off), `no_permission`, `no_device` (no virtual microphone). Absent on other hosts. |
| `/launch`, `/resume` | The client adds `clientMicRequested=1`, only if the state was `ready`. |
| Stream | The client sends control message `0x3080` on ENet channel 9, unsequenced, every 20 ms. |

The message travels inside the **encrypted control stream** (`SS_ENC_CONTROL_V2`, AES-GCM); it is never
sent, nor accepted, in the clear. Its payload:

| Bytes | Field |
|---|---|
| 0-1 | Sequence number, little-endian, +1 per frame |
| 2-5 | Timestamp in 48 kHz samples, little-endian, +960 per 20 ms frame |
| 6- | One Opus frame, 48 kHz mono, at most 400 bytes |

Clients encode voice at 32 kbit/s with in-band FEC (10% expected loss) and DTX. Silent frames still go
out, so a gap in the sequence numbers always means loss: the host rebuilds one lost frame from the next
frame's FEC, and more by packet loss concealment.

The host buffers 40 ms before playing, starts again from 40 ms after running dry, and drops audio back to
40 ms past 150 ms, which bounds the drift between the client's and the host's audio clocks.

One client at a time: the first session that asks holds the virtual microphone until it ends.
