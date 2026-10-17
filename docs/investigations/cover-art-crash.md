# Cover-art heap corruption

Windows reported `0xc0000374` heap-corruption crashes during audio playback,
including one before the audio synchronization experiment. Two inspected dumps
failed while freeing the JPEG conversion's SwsContext or codec context.

The old RGB24 destination was allocated as exactly `width * height * 3` bytes
with a tight row stride. A guarded reproduction with a 600 x 600 JPEG proved
that the installed FFmpeg SIMD conversion wrote 24 bytes past that allocation.
The same result was reproduced with the received iPhone cover image. This can
damage adjacent heap allocations and fail later when they are released.

The renderer now uses `decode_cover_art`, which copies compressed input into an
FFmpeg packet with input padding, converts into aligned FFmpeg-owned image
planes, and then copies the visible RGB rows into tightly packed output for
SDL. It also checks dimensions, input size and conversion success.

Validation: 600 synthetic conversions across 420/422/444 chroma formats and
600 x 600, odd and tiny dimensions; 500 conversions of the received cover;
malformed input checks; and 100 cover updates through the actual RTSP server
and SDL render thread with the application remaining alive. The socket and
audio timing regression suites also passed. Continued physical-device testing
is needed to detect any unrelated crashes.
