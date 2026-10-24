# M8C Complete State & Input Map

## Application State Machine

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              SDL_AppInit()                                   │
│  - Initialize config, renderer, gamepads                                     │
│  - Attempt M8 device connection                                              │
└─────────────────────────────────┬───────────────────────────────────────────┘
                                  │
                    ┌─────────────▼─────────────┐
                    │        INITIALIZE         │
                    └─────────────┬─────────────┘
                                  │
              ┌───────────────────┼───────────────────┐
              │                   │                   │
              ▼                   │                   ▼
┌─────────────────────────┐       │       ┌─────────────────────────┐
│    WAIT_FOR_DEVICE      │◄──────┘       │          RUN            │
│  - Screensaver active   │               │  - Main operation       │
│  - Poll device/1sec     │               │  - Process M8 data      │
│  - F1: Settings menu    │               │  - Render screen        │
└───────────┬─────────────┘               │  - Handle input         │
            │                             └───────────┬─────────────┘
            │ device found                            │
            └─────────────────────────────────────────┤
                                                      │
            ┌─────────────────────────────────────────┤
            │ device disconnected                     │
            ▼                                         │
┌─────────────────────────┐                           │
│    WAIT_FOR_DEVICE      │◄──────────────────────────┘
└─────────────────────────┘
            │
            │ Alt+F4 / SELECT+QUIT
            ▼
┌─────────────────────────┐
│          QUIT           │
│  - SDL_AppQuit()        │
│  - Cleanup resources    │
└─────────────────────────┘
```

## Input Modes

```
┌──────────────────────────────────────────────────────────────────┐
│                        NORMAL MODE                                │
│  (default - M8 button control)                                   │
├──────────────────────────────────────────────────────────────────┤
│  Input → 8-bit bitmask → m8_send_msg_controller()                │
│                                                                  │
│  Bits:  [LEFT][UP][DOWN][SELECT][START][RIGHT][OPT][EDIT]        │
│          0x80 0x40 0x20  0x10    0x08   0x04  0x02 0x01          │
└───────────────────────────────┬──────────────────────────────────┘
                                │ ESC key toggles
                                ▼
┌──────────────────────────────────────────────────────────────────┐
│                       KEYJAZZ MODE                                │
│  (tracker-style note entry)                                       │
├──────────────────────────────────────────────────────────────────┤
│  Note keys → m8_send_msg_keyjazz(note, velocity)                 │
│                                                                  │
│  Piano layout (2 octaves):                                       │
│  Lower:  Z S X D C V G B H N J M  (C to B)                       │
│  Upper:  Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P  (C+12 to E+24)       │
│                                                                  │
│  Note = key_offset + (base_octave * 12)                          │
│  Octave: 0-8, Velocity: 0x00-0x7F                                │
└──────────────────────────────────────────────────────────────────┘
```

## Complete Key Mapping (Default)

### M8 Control Keys → Sent to Device

| Function | Primary Key | Alt Key | Scancode | Bit Value |
|----------|------------|---------|----------|-----------|
| UP | ↑ | - | 82 | `0x40` |
| DOWN | ↓ | - | 81 | `0x20` |
| LEFT | ← | - | 80 | `0x80` |
| RIGHT | → | - | 79 | `0x04` |
| SELECT (SHIFT) | Left Shift | Z | 225 / 29 | `0x10` |
| START (PLAY) | Space | X | 44 / 27 | `0x08` |
| OPT | Left Alt | A | 226 / 4 | `0x02` |
| EDIT | Left Ctrl | S | 224 / 22 | `0x01` |
| DELETE | Delete | - | 76 | `0x03` (OPT+EDIT) |

### Application Control Keys → Local Actions

| Function | Key | Scancode | Action |
|----------|-----|----------|--------|
| Toggle Fullscreen | Alt+Enter | - | `toggle_fullscreen()` |
| Quit | Alt+F4 | - | `app_state = QUIT` |
| Toggle Keyjazz | ESC | 41 | `toggle_input_keyjazz()` |
| Reset Display | R | 21 | `m8_reset_display()` |
| Toggle Audio | F12 | 69 | `audio_toggle()` |
| Settings Menu | F1 | 58 | Opens in-app config |
| Log Overlay | F2 | 59 | `log_overlay_toggle()` |

### Keyjazz Control Keys

| Function | Key | Scancode | Action |
|----------|-----|----------|--------|
| Octave + | Numpad * | 85 | `base_octave++` (max 8) |
| Octave - | Numpad / | 84 | `base_octave--` (min 0) |
| Velocity + | Numpad + | 87 | `velocity += 0x10` |
| Velocity - | Numpad - | 86 | `velocity -= 0x10` |
| Fine Velocity | Alt + ±  | - | `±1` instead of `±0x10` |

### Keyjazz Note Layout

```
┌─────────────────────────────────────────────────────────────────┐
│  Upper Octave (+12):                                            │
│  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐  │
│  │ Q │ 2 │ W │ 3 │ E │ R │ 5 │ T │ 6 │ Y │ 7 │ U │ I │ 9 │ O │  │
│  │C+1│C#1│D+1│D#1│E+1│F+1│F#1│G+1│G#1│A+1│A#1│B+1│C+2│C#2│D+2│  │
│  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘  │
│                                                                 │
│  Lower Octave (base):                                           │
│  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐              │
│  │ Z │ S │ X │ D │ C │ V │ G │ B │ H │ N │ J │ M │              │
│  │ C │ C#│ D │ D#│ E │ F │ F#│ G │ G#│ A │ A#│ B │              │
│  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘              │
│                                                                 │
│  Note value = offset + (base_octave * 12)                       │
│  Default base_octave = 2 → C2 to B3                             │
└─────────────────────────────────────────────────────────────────┘
```

## Gamepad Mapping (Default)

### Buttons → M8 Controls

| M8 Function | SDL Gamepad Button | Value |
|-------------|-------------------|-------|
| UP | DPAD_UP | 11 |
| DOWN | DPAD_DOWN | 12 |
| LEFT | DPAD_LEFT | 13 |
| RIGHT | DPAD_RIGHT | 14 |
| SELECT | BACK | 4 |
| START | START | 6 |
| OPT | EAST (B/Circle) | 1 |
| EDIT | SOUTH (A/Cross) | 0 |
| QUIT | RIGHT_STICK | 8 |
| RESET | LEFT_STICK | 7 |

### Analog Axes

| Function | Axis | Default |
|----------|------|---------|
| Up/Down | LEFT_Y | 1 |
| Left/Right | LEFT_X | 0 |
| Select | LEFT_TRIGGER | 4 |
| Start | RIGHT_TRIGGER | 5 |
| Opt | (disabled) | -1 |
| Edit | (disabled) | -1 |

Threshold: `30000` (of ±32767)

### Special Combinations

| Combo | Action |
|-------|--------|
| SELECT + START + OPT + EDIT | Reset Display |
| SELECT + QUIT button | Quit Application |
| Hold BACK 2sec (disconnected) | Open Settings |

## M8 Protocol Messages

### Controller Input (Normal Mode)

```
m8_send_msg_controller(uint8_t buttons)
  → Sends 8-bit button state to M8

  Byte layout:
  Bit 7: LEFT
  Bit 6: UP
  Bit 5: DOWN
  Bit 4: SELECT
  Bit 3: START
  Bit 2: RIGHT
  Bit 1: OPT
  Bit 0: EDIT
```

### Keyjazz Input

```
m8_send_msg_keyjazz(uint8_t note, uint8_t velocity)
  → note: MIDI note number (0-127)
  → velocity: 0x00-0x7F (0 = note off)

  Note off: m8_send_msg_keyjazz(0xFF, 0)
```

## Event Flow Diagram

```
SDL Event
    │
    ├─► KEY_DOWN ─────────────────────────────────────────┐
    │       │                                             │
    │       ├─► Alt+Enter? ──► toggle_fullscreen()        │
    │       ├─► Alt+F4? ─────► app_state = QUIT           │
    │       ├─► ESC? ────────► toggle_keyjazz()           │
    │       ├─► F1? ─────────► settings overlay           │
    │       ├─► F2? ─────────► log overlay                │
    │       ├─► F12? ────────► audio toggle               │
    │       ├─► R? ──────────► reset display              │
    │       │                                             │
    │       └─► M8 key? ─────┬─► Normal mode:             │
    │                        │   keycode |= key.value     │
    │                        │   m8_send_msg_controller() │
    │                        │                            │
    │                        └─► Keyjazz mode:            │
    │                            Note key? → keyjazz msg  │
    │                            Settings? → octave/vel   │
    │                                                     │
    ├─► KEY_UP ───────────────────────────────────────────┤
    │       │                                             │
    │       └─► M8 key? ─────► keycode &= ~key.value      │
    │                          m8_send_msg_controller()   │
    │                                                     │
    ├─► GAMEPAD_BUTTON ───────────────────────────────────┤
    │       │                                             │
    │       ├─► Check special combos                      │
    │       └─► Update gamepad_state.current_buttons      │
    │           m8_send_msg_controller()                  │
    │                                                     │
    └─► GAMEPAD_AXIS ─────────────────────────────────────┘
            │
            └─► Update button state based on threshold
                m8_send_msg_controller()
```

## Config File Structure

```ini
[graphics]
fullscreen=false
wait_packets=256
integer_scaling=false

[audio]
audio_enabled=false
audio_buffer_size=0
audio_device_name=Default

[keyboard]
key_up=82          ; SDL_SCANCODE_UP
key_left=80        ; SDL_SCANCODE_LEFT
key_down=81        ; SDL_SCANCODE_DOWN
key_right=79       ; SDL_SCANCODE_RIGHT
key_select=225     ; SDL_SCANCODE_LSHIFT
key_select_alt=29  ; SDL_SCANCODE_Z
key_start=44       ; SDL_SCANCODE_SPACE
key_start_alt=27   ; SDL_SCANCODE_X
key_opt=226        ; SDL_SCANCODE_LALT
key_opt_alt=4      ; SDL_SCANCODE_A
key_edit=224       ; SDL_SCANCODE_LCTRL
key_edit_alt=22    ; SDL_SCANCODE_S
key_delete=76      ; SDL_SCANCODE_DELETE
key_reset=21       ; SDL_SCANCODE_R
key_jazz_inc_octave=85   ; SDL_SCANCODE_KP_MULTIPLY
key_jazz_dec_octave=84   ; SDL_SCANCODE_KP_DIVIDE
key_jazz_inc_velocity=87 ; SDL_SCANCODE_KP_PLUS
key_jazz_dec_velocity=86 ; SDL_SCANCODE_KP_MINUS
key_toggle_audio=69      ; SDL_SCANCODE_F12
key_toggle_settings=58   ; SDL_SCANCODE_F1
key_toggle_log=59        ; SDL_SCANCODE_F2

[gamepad]
gamepad_up=11      ; DPAD_UP
gamepad_left=13    ; DPAD_LEFT
gamepad_down=12    ; DPAD_DOWN
gamepad_right=14   ; DPAD_RIGHT
gamepad_select=4   ; BACK
gamepad_start=6    ; START
gamepad_opt=1      ; EAST
gamepad_edit=0     ; SOUTH
gamepad_quit=8     ; RIGHT_STICK
gamepad_reset=7    ; LEFT_STICK
gamepad_analog_threshold=30000
gamepad_analog_invert=false
gamepad_analog_axis_updown=1
gamepad_analog_axis_leftright=0
gamepad_analog_axis_select=4
gamepad_analog_axis_start=5
gamepad_analog_axis_opt=-1
gamepad_analog_axis_edit=-1
```

## SDL Scancode Reference

Common scancodes used:
| Key | Scancode |
|-----|----------|
| A-Z | 4-29 |
| 1-0 | 30-39 |
| F1-F12 | 58-69 |
| Arrow Up | 82 |
| Arrow Down | 81 |
| Arrow Left | 80 |
| Arrow Right | 79 |
| Space | 44 |
| Enter | 40 |
| Escape | 41 |
| Delete | 76 |
| Left Ctrl | 224 |
| Left Shift | 225 |
| Left Alt | 226 |
| Numpad * | 85 |
| Numpad / | 84 |
| Numpad + | 87 |
| Numpad - | 86 |

Full reference: https://wiki.libsdl.org/SDL2/SDL_Scancode
