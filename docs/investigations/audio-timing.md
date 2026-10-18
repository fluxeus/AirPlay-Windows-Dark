# Audio-only timing and pause/resume

These audio timing improvements are introduced in v0.4.0 after the v0.3.0 release.

Audio-only playback maps sender NTP/RTP timestamps to a local steady clock.
Clock responses validate the echoed request and RTT; samples expire and handle
arbitrary sender epochs and rollover. IPv4 senders work with the production
IPv6 dual-stack timing socket. Missing clocks have one bounded startup grace
period rather than a new two-second wait on every FLUSH.

Future PCM remains in a bounded application queue. SDL lookahead is limited
to roughly 60 ms, including the estimated device buffer. Mirroring uses
immediate submission. The output buffer estimate is not a hardware clock.

## Pause and session lifetime

Timestamped FLUSH respects the RTP boundary, including partial PCM blocks.
Audio-only playback saves pending PCM for a subsequent explicit resume.
Pause preservation is enabled normally; no diagnostic flag or environment
variable is required. Short pauses rebase its timestamps; final in-flight
pre-boundary packets are retained.
Immediate FLUSH/resume pairs remain buffer resets. Saved audio expires after
two minutes and is invalidated on changed track metadata or full disconnect.

For a partial type-96 teardown, the session retains the paused tail and open
SDL device after joining the stream workers. Recreated audio of the same
sample rate reuses them. The first fresh RTP packet establishes the new epoch.
A local schedule permits the saved audio to play before the first new timing
anchor. Sender transmission lead and one device-buffer margin align the join.
The new live stream continues to follow the sender clock.

DACP command availability follows the control session, so an iPhone can close
the media stream during pause and still receive Play from the PC. Mirroring
and session-ID gating remain in force.

## Validation and limits

`test-audio-timing` exercises actual UDP NTP/control sockets, encrypted ALAC,
the receiver and SDL dummy output. Coverage includes clock/RTT validation,
rollover, dual-stack IPv4, scheduled playback, timestamped FLUSH, mirror mode,
bounded shutdown, missing-clock fallback, paused PCM/device transfer, delayed
new timing anchors, and in-flight packets received after FLUSH.

Physical listening tests confirmed musical playback after short and 15-second
pauses. A 189-second stress run completed 24 pause/resume cycles (48 accepted
commands), with no control loss, warning/error or late-drop report. Private
memory decreased from 101,224,448 to 98,287,616 bytes; handles from 815 to 809.
This verifies the tested duration, not the absence of all leaks.

An iTunes video clip remains a known limitation: the user observed a brief
interruption or apparent repeated passage after resume. Fed queues and
contiguous timestamps do not establish waveform continuity. Track metadata
can arrive after playback starts, so every sender-side seek/track ordering is
not validated. Experimental PCM comparison, correlation threads, local command
hooks and temporary diagnostic executables have been removed. No clip-specific
content matching or automatic overlap trimming is part of playback.

## Audio-only interface pacing

CPU profiling identified SDL/Direct3D presentation as a significant cost during
audio-only playback. The renderer now waits up to 33 ms rather than 16 ms for
an Audio device. Video arrivals still satisfy the wake predicate immediately;
other device modes retain the 16 ms timeout.

Both Release and RelWithDebInfo builds passed. Sequential live audio samples
with the same WPR CPU profile measured 11.84% then 7.33% of one core (about 38%
less). Memory was stable, the connection stayed established, and the window
remained responsive. This is a single-machine measurement, not a guarantee
for other tracks, window sizes or hardware. Subjective UI smoothness and
mirroring cadence still require physical validation.
