# Audio-only receiver disconnect: Shairport Sync comparison

Study date: 2026-10-17. Reference: mikebrady/shairport-sync commit
`cf6998daa1876c5a8f97128a960c8afc0e731841`.
The downloaded rtsp.c matches the pinned revision byte for byte.
No Shairport Sync executable or Bonjour runtime was installed.

## Observed on our receiver

The physical-device trace uses the plist SETUP path, timingProtocol=NTP,
a type-96 ALAC UDP stream and eventPort=0. It is not the same negotiation
as Shairport Sync's full AirPlay 2 PTP path, nor its classic ANNOUNCE/Transport path.

Disconnect drains the audio worker, NTP worker and all session sockets in
approximately 110 ms in the recorded test. No reconnection follows. At app
exit both discovery deregistration callbacks return success. A separate local
multicast observation also saw TTL-zero PTR, SRV and TXT goodbye records for
both test services.

An experimental DACP device-prevent-playback=1 notification resolved the sender
service and received HTTP 200 from the iPhone. The user nevertheless observed
the old output remaining selected until manually choosing the phone speaker.
HTTP success does not establish that iOS implements the requested route change.
The earlier abortive TCP reset experiment also did not fix the UI state and was
reverted. Neither experiment proves an iOS bug.

## Reference behavior

1. `rtsp.c:stop_play` terminates the principal RTSP conversation, clears its group
   association and the ReceiverSessionIsActive flag (bit 17), and updates the
   discovery TXT records. This path does not send a force-phone-speaker command.
2. `rtsp_conversation_thread_cleanup_function` stops the player, closes RTSP,
   cancels/joins the event worker, closes media/timing/control sockets and
   releases per-connection state. `safe_socket_close` closes the descriptor;
   it is not a special AirPlay teardown message.
3. The full AirPlay 2 SETUP PTP branch creates a TCP event listener, advertises a
   nonzero eventPort, and starts the event receiver. The event receiver sends
   updateInfo and owns a separate event connection. These lifecycle semantics
   cannot be copied as just one extra teardown packet into our NTP setup.
4. In that same AirPlay 2 SETUP handler, the NTP branch explicitly says its
   stream handling is not implemented. The classic path supports NTP through
   a different ANNOUNCE/Transport negotiation. It would be misleading to claim
   Shairport Sync handles the exact session captured from this iPhone.
5. Shairport Sync updates its advertised active/inactive state. Our mDNS flags
   currently stay at 0x4; /info reports statusFlags=68. There is no modeled
   active-session flag transition. This is a protocol-consistency difference
   to investigate, not an established cause of the retained iOS output route.

## Conclusion and scope

The study did not reveal a proven drop-in command to make iOS choose its own
speaker. Our socket cleanup now works, but the sender's retained output route
remains uncorrected. The useful next protocol work would be a coherent receiver
session state across discovery, /info and any negotiated event channel, with
specific audio-only validation and no regression of the validated mirror path.
Full PTP/HomeKit/event-channel support is a larger change, not a safe assumption
for a small release patch. Keep unvalidated DACP work out of release claims.

## Primary sources

- [RTSP lifecycle, stop_play and SETUP](https://github.com/mikebrady/shairport-sync/blob/cf6998daa1876c5a8f97128a960c8afc0e731841/rtsp.c)
- [AirPlay 2 event connection](https://github.com/mikebrady/shairport-sync/blob/cf6998daa1876c5a8f97128a960c8afc0e731841/ap2_event_receiver.c)
- [Socket close helper](https://github.com/mikebrady/shairport-sync/blob/cf6998daa1876c5a8f97128a960c8afc0e731841/utilities/network_utilities.c)
- [Discovery strings](https://github.com/mikebrady/shairport-sync/blob/cf6998daa1876c5a8f97128a960c8afc0e731841/bonjour_strings.c)
- [DACP implementation](https://github.com/mikebrady/shairport-sync/blob/cf6998daa1876c5a8f97128a960c8afc0e731841/dacp.c)
