"""Check abnormal control-close cleanup against an idle receiver started with --log.

Usage: python tools/test_connection_loss.py path/to/receiver.stderr.log
This tests EOF/reset and session ownership, not a real Wi-Fi packet loss.
"""

import pathlib
import plistlib
import socket
import struct
import sys
import time


log = pathlib.Path(sys.argv[1])
offset = log.stat().st_size


def events():
    with log.open("rb") as stream:
        stream.seek(offset)
        return stream.read().decode("utf-8", errors="replace")


def wait_for(text):
    deadline = time.monotonic() + 5
    while text not in events():
        if time.monotonic() >= deadline:
            raise AssertionError(f"Missing event: {text}\n{events()}")
        time.sleep(0.02)


def connect():
    client = socket.create_connection(("127.0.0.1", 7000), timeout=3)
    body = plistlib.dumps({"streams": [{"type": 110}]}, fmt=plistlib.FMT_BINARY)
    client.sendall(
        b"SETUP /connection-loss-test RTSP/1.0\r\nCSeq: 1\r\n"
        b"Content-Type: application/x-apple-binary-plist\r\n"
        + f"Content-Length: {len(body)}\r\n\r\n".encode() + body
    )
    wire = b""
    while b"\r\n\r\n" not in wire:
        chunk = client.recv(4096)
        assert chunk, "EOF before SETUP response"
        wire += chunk
    header, response_body = wire.split(b"\r\n\r\n", 1)
    assert header.startswith(b"RTSP/1.0 200"), header
    fields = dict(line.split(b":", 1) for line in header.split(b"\r\n")[1:])
    length = int(fields[b"Content-Length"])
    while len(response_body) < length:
        chunk = client.recv(4096)
        assert chunk, "EOF before SETUP body"
        response_body += chunk
    return client, fields[b"Session"].strip().decode()


def reset(client):
    # Windows uses two unsigned shorts; POSIX uses two ints for linger.
    layout = "HH" if sys.platform == "win32" else "ii"
    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack(layout, 1, 0))
    client.close()


old, old_id = connect()
new, new_id = connect()
assert old_id != new_id
reset(old)
wait_for(f"StreamSession::teardown begin session={old_id}")
reset(new)
wait_for(f"Connection ended: cleared active session={new_id}")
assert f"Connection ended: cleared active session={old_id}" not in events()
reconnected, reconnected_id = connect()
reconnected.close()  # Clean TCP EOF, still no protocol TEARDOWN.
wait_for(f"Connection ended: cleared active session={reconnected_id}")
assert "Could not configure control TCP keepalive" not in events()
print("PASS: reset cleanup, newer session preserved, reconnect and EOF cleanup")
