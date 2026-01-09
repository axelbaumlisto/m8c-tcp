# Changelog

All notable changes to m8c-tcp (fork of [laamaa/m8c](https://github.com/laamaa/m8c)).

## [1.0.0-tcp] - 2026-01-10

### Base
- Based on [laamaa/m8c v2.2.2](https://github.com/laamaa/m8c) (commit 0c7e198)
- Includes [jasonporritt/rg351_m8c](https://github.com/jasonporritt/rg351_m8c) deployment scripts

### Added

#### TCP Backend (`make tcp`)
- New TCP backend for network-based M8 control
- Binary packet protocol with headers (Display 0x44, Audio 0x41)
- Low-latency socket options (TCP_NODELAY, TCP_QUICKACK, TCP keepalive)
- `poll()`-based data receiving for minimal latency
- Automatic hostname resolution (DNS support)
- Configurable server address via `--dev host:port`
- Graceful disconnect handling

#### WebSocket Backend (`make websocket`)
- WebSocket backend using libwebsockets
- Separate channels for display, audio, and control
- JSON-based control messages
- Alternative to TCP for web integration scenarios

#### TCP Audio Streaming
- Real-time audio streaming over TCP
- Audio format: S16LE stereo 44100Hz
- aplay backend for direct ALSA output (bypasses SDL3 ALSA issues on ARM)
- SDL3 AudioStream fallback for other platforms
- Automatic audio initialization when server streams audio

#### Documentation
- `PROTOCOL.md` - Binary protocol specification for server implementers
- `HARDWARE.md` - Tested hardware and configurations
- Updated `README.md` with network backend instructions

### Changed
- `audio_sdl.c`: Added `audio_init_tcp()` and `audio_feed_tcp()` functions
- `audio_sdl.c`: Added aplay pipe output for ARM compatibility
- `audio_libusb.c`: Reduced prebuffer size from 8KB to 2KB for lower latency
- `Makefile`: Added `tcp` and `websocket` build targets

### Fixed
- Display packet counter for inline packets (32ce8f7)

## Upstream Changes

For changes in the original m8c project, see:
https://github.com/laamaa/m8c/commits/main
