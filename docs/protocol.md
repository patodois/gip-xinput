# Loopback protocol version 1

UDP IPv4, host `127.0.0.1:26785`; clients bind ephemeral IPv4 loopback ports. One datagram is one message. All multibyte integers are little-endian, with no native struct padding. Hosts reject malformed requests without replying. Clients reject malformed, unrelated, late or oversized responses within their existing timeout budget.

## Request: exactly 24 bytes

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `GXIP` |
| 4 | 1 | Version 1 |
| 5 | 1 | Operation: 1 = state; 2 = rumble |
| 6 | 2 | Reserved, zero |
| 8 | 8 | Client request ID; bytes 12..16 (the upper half) are the client's process ID |
| 16 | 2 | Left / low-frequency motor speed, `0..65535` |
| 18 | 2 | Right / high-frequency motor speed, `0..65535` |
| 20 | 4 | Reserved, zero |

State requests must have both motor fields zero. Rumble requests replace the latest desired output; they are not added together.

The upper 32 bits of the request ID identify the calling process and must stay the same for every request it sends; the lower 32 bits change on every call. The host treats the process that sent the latest rumble request as its owner. Once the owner has sent no request of any kind for 500 ms (it exited, crashed or stopped polling), the host clears that request and stops the motors; requests from other processes do not keep it alive, and a cleared request is never replayed to a later client. While the owner keeps polling, a nonzero effect is refreshed every 100 ms. A request that repeats the current motor values does not cause another USB write before that refresh. A successful rumble reply acknowledges queuing, not completed USB I/O. Unplugged controllers do not accept output. The client uses a fresh socket and correlation ID per call. It sends output again on its own only after an output request timed out (see the end of this document); otherwise a later game call or XInputEnable submits output again.

## Response: exactly 40 bytes

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `GXIP` |
| 4 | 1 | Version 1 |
| 5 | 1 | Request operation OR `0x80` |
| 6 | 2 | Reserved, zero |
| 8 | 8 | Echoed request ID |
| 16 | 4 | Win32 status: 0 or 1167 |
| 20 | 4 | Host state sequence, wrapping u32 |
| 24 | 2 | Documented XINPUT_GAMEPAD button mask; reserved bits zero |
| 26 | 1 | Left trigger, `0..255` |
| 27 | 1 | Right trigger, `0..255` |
| 28 | 2 | Signed left stick X |
| 30 | 2 | Signed left stick Y |
| 32 | 2 | Signed right stick X |
| 34 | 2 | Signed right stick Y |
| 36 | 4 | Reserved, zero |

For status 1167 all gamepad bytes (24 through 35) are zero. Sticks range from -32768 to 32767; positive means right/up. The host sequence changes only on a mapped input/connection change, not for a request, rumble command or keepalive. The host seeds it at startup to help distinguish sessions. It is a change indicator, not an absolute count or timestamp.

The DLL initially uses the host sequence for `dwPacketNumber`. Subsequent changes use the wrapping sequence delta; enable-induced changes add one when no host delta exists. While disabled, changes that still produce neutral output do not change the exposed packet number. Disconnection discards event history. Every API query that needs connection state performs a fresh request; no heartbeat is used.

The timeout is a monotonic 2 ms response budget after socket initialization. A timeout is not a disconnection: within `GX_HOLD_MS` (100 ms) of the last reply with status 0, the DLL answers from that reply as if the host had sent it again, so the packet number and keystroke state stay as they were; held answers do not extend the window. Status 1167 from the host and socket errors, such as the refusal of a closed port, are reported at once. An output request (rumble or enable) that timed out is marked pending and sent as the next query, as a rumble request, whose reply also carries the state. Blocking `select` sleeps between datagrams. At most eight received datagrams are examined per request, so invalid traffic cannot create an unbounded receive loop. Sockets are connected to the fixed loopback host, and no address is learned from packet contents. This is not an authenticated protocol against other local processes.
