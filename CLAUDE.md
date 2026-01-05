# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

m8c is a cross-platform remote display client for the Dirtywave M8 Tracker. It mirrors the M8's display over USB, enables keyboard/gamepad control, and routes audio to the host computer. Works with both M8 hardware and M8 Headless firmware on Teensy microcontrollers.

## Build Commands

### Dependencies

```bash
# Ubuntu 25.04+
sudo apt install git gcc pkg-config make libserialport-dev libsdl3-dev

# macOS
brew install git gcc make sdl3 libserialport pkg-config

# Windows (MSYS2/MINGW64)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-pkg-config mingw-w64-x86_64-make mingw-w64-x86_64-SDL3 mingw-w64-x86_64-libserialport
```

### Building

```bash
# Default build (libserialport backend)
make

# Alternative backends
make libusb    # Use libusb instead of libserialport
make rtmidi    # Use RtMidi backend

# CMake build
cmake -B build -DUSE_LIBSERIALPORT=ON
cmake --build build

# Clean
make clean
```

### Running

```bash
./m8c              # Auto-detect device
./m8c --list       # List available devices
./m8c --dev /dev/ttyACM0  # Use specific device
./m8c --config alt.ini    # Use alternate config
```

## Architecture

### Main Loop (SDL3 Callbacks)

Entry point: `src/main.c` using SDL3's callback-based architecture:
- `SDL_AppInit()` → Initialize config, renderer, gamepads, attempt M8 connection
- `SDL_AppIterate()` → Main loop (~120Hz): process input → `m8_process_data()` → `render_screen()`
- `SDL_AppQuit()` → Cleanup

App states: `INITIALIZE` → `WAIT_FOR_DEVICE` → `RUN` → `QUIT`

### Core Modules

| Module | Purpose |
|--------|---------|
| `command.c` | Parse M8 display commands (draw rectangle, character, waveform) |
| `render.c` | SDL3 rendering, screen drawing, fullscreen toggle |
| `input.c` | Keyboard/gamepad input handling, keyjazz mode |
| `config.c` | Config file parsing and management |
| `settings.c` | In-app settings overlay (F1 menu) |
| `gamepads.c` | SDL game controller support |

### Backend Abstraction (`src/backends/`)

Communication backends are compile-time selectable via preprocessor defines:

| Backend | File | Use Case |
|---------|------|----------|
| libserialport | `m8_libserialport.c` | Default, most compatible |
| libusb | `m8_libusb.c` | Direct USB, includes audio |
| RtMidi | `m8_rtmidi.c` | MIDI-based communication |

Audio backends:
- `audio_sdl.c` - SDL-based audio output
- `audio_libusb.c` - Direct USB audio (libusb backend only)

### M8 Protocol

The M8 uses SLIP (Serial Line IP) framing (`slip.c`) for serial communication. Commands from M8:
- Draw rectangle (position, size, color)
- Draw character (position, foreground/background colors)
- Draw oscilloscope waveform

Input sent to M8:
- Controller buttons (8-bit bitmask via `m8_send_msg_controller()`)
- Keyjazz notes (note + velocity via `m8_send_msg_keyjazz()`)

### Key Data Structures

```c
// config.h
config_params_s    // All app settings (display, audio, keybindings)

// command.h
draw_rectangle_command
draw_character_command
draw_oscilloscope_waveform_command

// common.h
struct app_context  // App state, config, device status
```

## Config File Locations

- Windows: `%APPDATA%\m8c\config.ini`
- Linux: `~/.local/share/m8c/config.ini`
- macOS: `~/Library/Application Support/m8c/config.ini`

## Default Key Mappings

Navigation: Arrow keys | M8 controls: z/LShift=shift, x/Space=play, a/LAlt=opt, s/LCtrl=edit

Special: Alt+Enter=fullscreen, F1=settings, F2=log overlay, F12=toggle audio, Esc=keyjazz toggle
