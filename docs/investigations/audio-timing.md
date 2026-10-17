# Audio-only playback timing (development build)

The reported symptom is PC audio playing about 3–4 seconds before video on
the iPhone when only the audio route is sent to the receiver. This differs
from mirrored video lag and from delayed Windows audio output.

The v0.3.0 receiver sends decoded audio to SDL on arrival. The development
branch `fix/audio-playback-latency` instead maps the sender's NTP clock to local
steady time and combines this with RTP synchronization anchors from the audio
control socket. It submits PCM near its scheduled presentation time. No fixed
three-second delay is configured. Mirroring retains immediate audio submission.

The clock mapping accepts arbitrary sender epochs, checks the timing response's
echo, filters excessive RTT, chooses a recent low-RTT sample, and expires stale
measurements. A five-second decoded PCM bound and a two-second fallback wait
prevent unbounded memory or permanent silence when synchronization is missing.
FLUSH/PAUSE clear pending PCM and wake the playout thread. Hardware delay is
estimated from the negotiated SDL buffer; this is not a hardware clock reading.

SDL receives the decoder's exact frequency and channel count, with any required
device conversion performed internally. See the primary
[SDL_OpenAudioDevice documentation](https://wiki.libsdl.org/SDL2/SDL_OpenAudioDevice).

## Validation

Configure with `-DBUILD_NETWORK_TESTS=ON`, build Release and run CTest.
`test-audio-timing` uses native loopback UDP timing and control sockets, an
ALAC encoder, AES-CBC encrypted RTP, the actual receiver, and SDL's dummy audio
device. Renderer symbols are inert test stubs; no renderer is attached.

The three-second-ahead fixture was submitted to SDL at approximately 2,977 ms,
accounting for its estimated output buffer. Tests also cover NTP fractional and
seconds rollover, RTP rollover, stale/invalid clock samples, FLUSH cancellation,
immediate submission for mirroring, and stopping with a future frame pending.
The socket lifecycle suite and all 26 protocol checks also passed locally.

This validates scheduling, not audible lip sync on a physical iPhone. The local
build logs `Audio synchronized playout: initial_wait_ms=...` and
`Audio playout: synchronized=...` to check whether real sender anchors are used.
The user subsequently confirmed aligned PC audio and iPhone video in audio-only
mode. A real sender log showed an initial scheduled wait of about 1,745 ms.
This experiment is separate from the published v0.3.0 tag.

## Pause/resume follow-up

The initial scheduler could fill SDL with the sender's entire lead (about
1.75 seconds in the real session). FLUSH then discarded that entire playback
queue and all pending decoded PCM, ignoring the sender's `RTP-Info` boundary.
The user reported a short missing segment when resuming.

The follow-up limits SDL lookahead to roughly 60 ms including the estimated
device buffer. Future PCM stays in the timestamped application queue. FLUSH
with `rtptime` now removes only samples preceding that boundary, including
partial PCM blocks and RTP rollover. Without a valid boundary it retains the
previous full-reset behavior. The handling follows the timestamped flush
semantics in [Shairport Sync's player](https://github.com/mikebrady/shairport-sync/blob/master/player.c).
This does not infer that every FLUSH is a user pause or rebase the sender clock.

Tests cover partial-block retention and full resets alongside synchronized
playback. Logs include FLUSH boundaries and exact RTP progress for validation
of the real iPhone pause/resume behavior, which still requires a physical test.
