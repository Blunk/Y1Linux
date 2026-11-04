# The radio daemon (`y2-radiod`)

Since Beta 9.3 one process owns Bluetooth and both radios' power. It replaced five owners that coordinated through
lock and intent files (Reborn's D-Bus worker, `y2-platform bluetooth-power`, `y2-bt-reconnect`, the sleep helper's
radio steps, the kernel's own recovery). Why and how it was planned:
[BLUETOOTH-REIMPLEMENTATION-PLAN.md](../planning/BLUETOOTH-REIMPLEMENTATION-PLAN.md).

## Pieces

| Piece | Source | Job |
|---|---|---|
| Arbiter (main thread) | `platform/crates/y2-radiod/src/arbiter.rs` | Wi-Fi power (per-device rfkill, never the type, F37), the wake order (Wi-Fi waits up to 12 s for the headset connected before the sleep), the codec ceiling while Wi-Fi is on (`/run/y2/bt-codec-ceiling`), Wi-Fi power save following Bluetooth streaming, the published state |
| Bluetooth controller (one thread) | `src/bt/` | BlueZ and BlueALSA over the system D-Bus; every long call asynchronous with its own deadline |
| Socket server | `src/server.rs` | `/run/y2/radiod.sock`, root only, one JSON object per line (`y2p_contract::radiod`) |
| `y2-radiod ctl` | `src/ctl.rs` | what `y2-radio` (boot, sleep, shutdown) asks; without the daemon it switches the radios directly |

## Contract

Requests (`Request`) describe a wish, never a step, and are answered at once (`Reply`); only a sleep's power-down is
answered when it is done. Subscribers get every new `State`; `/run/y2/radiod-state.json` holds the latest. Reborn
keeps one `Status` built from it (`Y2Reborn/crates/reborn-platform/src/bluetooth.rs`); a connected headset that is not
`Ready` yet is `settling` and not playable. Before opening BlueALSA's stream Reborn takes a `Stream` lease; no codec
changes while one is open.

## Bluetooth controller

- **Episodes** (`bt/links.rs`): Bluetooth on or a wake: 3 s for headphones to come back on their own, then three
  tries (3 s, 8 s apart); the owner's tap: at once, three tries; a lost link (timeout, local, unknown): after 2 s,
  three tries; a remote end (switched off): one look after 4 s; the owner's disconnect or a sleep: none. A spent
  episode parks the headset until it connects by itself, a wake, Bluetooth on, or a tap.
- **Stuck connects**: InProgress for 12 s, connected without a stream for 8 s, or no answer within 35 s, is
  cancelled (a Disconnect ends BlueZ's page); a second cancel in one episode switches the adapter off and on. A link
  counts as set up when BlueZ resolved its services *or* BlueALSA has its stream: BlueZ looks up no services for a
  known headset that connects by itself, so its `ServicesResolved` stays false for that whole connection. The 8 s
  count from when the set-up went missing (`Record::unset_since`), not from the connect, and not while a codec
  change runs: a change briefly removes BlueALSA's stream, and counting from the connect cancelled such a headset at
  once (owner's AirPods, 2026-11-04).
- **Recovery ladder** (`bt/ladder.rs`): cancel page → power cycle → BlueALSA restart → BlueZ restart, three each in
  ten minutes; then the service is Failed until the owner switches Bluetooth off and on (which restarts BlueALSA and
  BlueZ and starts every budget over).
- **Supervision** (`bt/supervise.rs`): bluetoothd (its init script) and bluealsad (started here with the build's
  codec arguments and `--keep-alive=900`: no AVDTP Suspend while a paused player stays awake, since headsets
  answer Suspends late or not at all and BlueZ then ends the link) start again when they end, five times in ten
  minutes. A kept stream has gaps without packets (a pause, ~50 ms per track skip): the sink plays them out of its
  buffer and nothing paced at real time gives them back (AirPods: crackle after two skips, silence after three,
  2026-11-04). BlueALSA patch 0008 sends up to 200 ms faster than real time after such a gap to refill it; patch
  0007 moves the RTP timestamp over the gap for sinks that play by timestamp.
- **Codecs** (`bt/codec.rs`, `bt/settle.rs`): the policy Reborn had (Auto: AAC, aptX, SBC; LDAC and aptX HD only
  when chosen; history of refusals). SelectCodec only when the negotiated codec differs, only while no stream is
  open, at most three per connection; an unanswered one leaves the headset usable. Auto keeps the AAC, aptX or SBC a
  headset connected with (it only moves off LDAC/aptX HD); a chosen codec is applied once our Connect has answered and
  the link has been up 3 s (`SWITCH_QUIET`): sent earlier it cancels BlueZ's AVDTP Open and ends the link, which
  looped ≈40 times in Y2Reborn #94. A change that failed or ended a link is not sent to that headset again until the
  owner picks a codec again (`Codecs::kept`, survives reconnects). A stream whose codec list lacks the
  codec it runs waits up to 3 s for the list (BlueZ asks a headset that connected by itself only after the stream). New codec settings
  (`codec-settings`) restart BlueALSA once nothing plays (`ReloadCodecs`).
- **Pairing agent** (`bt/agent.rs`): `DisplayYesNo`; the owner's yes/no, refused after 60 s; Pairable for 90 s.

## Records

| Record | Writer | Readers |
|---|---|---|
| `/data/bluetooth/enabled`, `/data/network/enabled` | daemon (owner's switches) | daemon, sleep helper |
| `/data/bluetooth/preferred-audio` | daemon (the trusted headset that connected last) | daemon |
| `/data/bluetooth/radiod.json` | daemon (codec choice, devices that connect only when asked, #78) | daemon, error reports |
| `/data/bluetooth/codec-history.json` | daemon | daemon |
| `/data/system/platform/bluetooth-journal.jsonl` | daemon (state changes, calls, disconnect reasons, recovery) | error reports |
| `/run/y2/radiod-state.json` | daemon | service daemon (coexistence, Wi-Fi power save), error reports |

`status` asks the daemon on its socket for `bluetooth.reconnect`. BlueZ's and BlueALSA's objects and the encoder
statistics in `status` still come from `y2-bt-observe` (read-only).
