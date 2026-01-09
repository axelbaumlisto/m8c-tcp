# m8c-tcp Binary Protocol Specification

This document specifies the TCP protocol used by m8c-tcp to communicate with an M8 display server.

## Connection

- **Default port:** 3333 (TCP), 8080 (WebSocket)
- **Transport:** TCP stream or WebSocket
- Server must accept TCP connections and handle the binary protocol

## Packet Format

All packets from server to client follow this structure:

```
+--------+---------+---------+------------------+
| Header | Len Hi  | Len Lo  | Data payload     |
| (1B)   | (1B)    | (1B)    | (variable)       |
+--------+---------+---------+------------------+
```

- **Header:** 1 byte identifying packet type
- **Length:** 2 bytes big-endian, payload size (not including header/length)
- **Data:** Variable length payload

### Packet Types (Server -> Client)

| Header | Hex  | Name    | Description |
|--------|------|---------|-------------|
| D      | 0x44 | Display | SLIP-encoded M8 display commands |
| A      | 0x41 | Audio   | Raw PCM audio data |

### Display Packets

Display packets contain SLIP-encoded M8 serial data. The client must decode SLIP frames
and process M8 display commands (draw rectangle, draw character, draw waveform).

### Audio Packets

Audio packets contain raw PCM data:
- **Format:** S16LE (signed 16-bit little-endian)
- **Sample rate:** 44100 Hz
- **Channels:** 2 (stereo)
- **Typical packet size:** 1024-4096 bytes

## Client Commands (Client -> Server)

These commands are sent from m8c to the server as raw bytes (no framing):

### Controller Input

```
+------+----------+
| 0x43 | bitmask  |
| 'C'  | (1B)     |
+------+----------+
```

Sends button state. Bitmask is 8-bit with buttons:

| Bit | Button |
|-----|--------|
| 0   | EDIT   |
| 1   | OPT    |
| 2   | RIGHT  |
| 3   | START  |
| 4   | SHIFT  |
| 5   | DOWN   |
| 6   | UP     |
| 7   | LEFT   |

Example: Press DOWN (bit 5) = `0x43 0x20`

### Keyjazz Note On

```
+------+------+----------+
| 0x4B | note | velocity |
| 'K'  | (1B) | (1B)     |
+------+------+----------+
```

- **note:** MIDI note number (0-127)
- **velocity:** 0-127 (clamped to 0x7F)

Example: C4 (note 60) at velocity 100 = `0x4B 0x3C 0x64`

### Keyjazz Note Off

```
+------+------+
| 0x4B | 0xFF |
| 'K'  |      |
+------+------+
```

### Reset Display

```
+------+
| 0x52 |
| 'R'  |
+------+
```

Requests full display refresh from M8.

### Enable Display

```
+------+
| 0x45 |
| 'E'  |
+------+
```

Enables M8 display output. Send after connection.

### Ping

```
+------+
| 0x58 |
| 'X'  |
+------+
```

Keep-alive ping. Server should respond or echo.

### Disconnect

```
+------+
| 0x44 |
| 'D'  |
+------+
```

Graceful disconnect notification.

## SLIP Encoding

Display data uses SLIP (Serial Line Internet Protocol) framing per RFC 1055:

| Byte | Name | Description |
|------|------|-------------|
| 0xC0 | END  | Frame delimiter |
| 0xDB | ESC  | Escape byte |
| 0xDC | ESC_END | Escaped END (0xC0 -> 0xDB 0xDC) |
| 0xDD | ESC_ESC | Escaped ESC (0xDB -> 0xDB 0xDD) |

The server should forward raw SLIP frames from the M8 device wrapped in Display packets.

## M8 Display Commands

After SLIP decoding, M8 commands have these formats:

### Draw Rectangle (0xFE)

```
Position, size, and RGB color for filled rectangle
```

### Draw Character (0xFD)

```
Character code, position, foreground/background colors
```

### Draw Waveform (0xFC)

```
480 bytes of oscilloscope waveform data
```

## Socket Options

For low latency, clients should set:

```c
// Disable Nagle's algorithm
setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

// Linux: disable delayed ACK
setsockopt(sock, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));

// TCP keepalive for dead connection detection
setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(int));
```

## Example Session

```
Client                          Server
  |                               |
  |-------- Connect TCP --------->|
  |                               |
  |<----- Display packet ---------| (initial screen)
  |<----- Display packet ---------|
  |                               |
  |-------- Enable 'E' ---------->|
  |                               |
  |<----- Display packets --------|
  |<----- Audio packets ----------|
  |                               |
  |-------- Controller 'C' 0x20 ->| (DOWN button)
  |                               |
  |<----- Display update ---------|
  |                               |
  |-------- Disconnect 'D' ------>|
  |                               |
```

## Server Implementation Notes

1. **Packet boundaries:** TCP is a stream protocol. Server must frame packets with headers.
2. **Audio buffering:** Client may buffer ~4KB before starting playback.
3. **Reconnection:** Client will attempt reconnection on disconnect.
4. **Multiple clients:** Server may support multiple viewers (broadcast mode).
