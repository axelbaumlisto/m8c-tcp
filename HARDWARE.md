# Tested Hardware and Software

This document lists hardware and software configurations that have been tested with m8c-tcp.

## Confirmed Working

### Client Devices

| Device | OS | Architecture | Backend | Audio | Notes |
|--------|-----|--------------|---------|-------|-------|
| Anbernic RG353V | ArkOS (Ubuntu 19.10) | aarch64 | TCP | aplay | Primary test device |
| Linux PC | Ubuntu 22.04+ | x86_64 | TCP | SDL3 | Development/testing |

### Server Configurations

| Server | OS | M8 Connection | Notes |
|--------|-----|---------------|-------|
| m8-display-tcp | Linux (Ubuntu/Debian) | USB | TypeScript/Node.js |

## Network Configurations

| Setup | Typical Latency | Jitter | Usability |
|-------|-----------------|--------|-----------|
| Local Wi-Fi (5GHz) | 1-5ms | Low | Excellent - suitable for real-time music production |
| Local Wi-Fi (2.4GHz) | 5-15ms | Medium | Good - minor perceptible delay |
| Tailscale VPN | 50-120ms | Variable | Acceptable - noticeable delay, use for monitoring |
| Internet (direct) | 100ms+ | High | Limited - viewing only recommended |

### Permanent WiFi + Tailscale Setup (RG353V)

For remote access from anywhere, set up Tailscale VPN on the console:

#### 1. Enable Permanent WiFi (ArkOS)

Edit `/etc/NetworkManager/conf.d/wifi-powersave.conf`:
```ini
[connection]
wifi.powersave = 2
```

Or via Options menu: **Options > Network > WiFi Power Save > Disable**

#### 2. Install Tailscale

```bash
# SSH into the console
ssh ark@<console-ip>

# Install Tailscale
curl -fsSL https://tailscale.com/install.sh | sh

# Authenticate
sudo tailscale up

# Get Tailscale IP
tailscale ip -4
```

#### 3. Connect via Tailscale

After setup, you can connect to the console from anywhere:
```bash
# From any device on your Tailscale network
./m8c --dev <tailscale-ip>:3333
```

**Tailscale IPs example:**
| Device | Tailscale IP | Role |
|--------|--------------|------|
| rg353v | 100.x.x.x | Console (client) |
| server-pc | 100.y.y.y | M8 server |

## Building on ARM (ArkOS/351ELEC)

### Prerequisites

The `rg351_m8c` directory contains scripts for building on handheld devices:

1. Connect device to internet (Wi-Fi)
2. Run `SETUP` script to install base dependencies
3. Run `_setup_build_tools.sh` to install build tools

### Building m8c-tcp on RG353V

```bash
# SSH into device
ssh ark@<device-ip>

# Clone or copy m8c-tcp source
cd ~/build
git clone <m8c-tcp-repo>
cd m8c-tcp

# Build SDL3 if not available (ArkOS has older SDL2)
# See rg351_m8c/README.md for SDL3 build instructions

# Set PKG_CONFIG_PATH if SDL3 is in custom location
export PKG_CONFIG_PATH=~/build/sdl3-install/lib/pkgconfig

# Build TCP backend
make clean && make tcp

# Copy binary and libraries
cp m8c /roms/ports/M8/m8c-tcp
cp ~/build/sdl3-install/lib/libSDL3.so.0 /roms/ports/M8/
```

### Launch Script (M8_tcp.sh)

Create `/roms/ports/M8/M8_tcp.sh`:

```bash
#!/bin/bash
cd /roms/ports/M8
echo "powersave" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null
export LD_LIBRARY_PATH=/roms/ports/M8:$LD_LIBRARY_PATH
./m8c-tcp --dev <server-ip>:3333
```

Make executable: `chmod +x M8_tcp.sh`

## Audio Configuration

### ARM Devices (RG353V, RG351)

The TCP backend uses `aplay` for audio output by default on ARM devices. This bypasses
SDL3 ALSA issues that cause audio problems on these platforms.

Audio command used:
```bash
aplay -D default -f S16_LE -r 44100 -c 2 -t raw --buffer-size=4096 --period-size=512 -
```

### Desktop Linux

SDL3 AudioStream is used by default. Audio should work with PulseAudio/PipeWire.

### Troubleshooting Audio

1. **No audio on ARM:** Check if `aplay` is available and working
2. **Audio crackling:** Try increasing buffer size in config
3. **High latency:** Reduce buffer size, use wired network

## M8 Device Requirements

- Dirtywave M8 Tracker (Model 01 or 02)
- OR Teensy 4.1 with [M8 Headless Firmware](https://github.com/Dirtywave/M8HeadlessFirmware)
- USB connection to server machine

## Known Limitations

1. **Network dependency:** Requires stable network connection to server
2. **Latency:** Not suitable for real-time performance over high-latency connections
3. **Audio sync:** Audio may drift slightly on long sessions (restart to resync)

## Contributing Hardware Tests

If you've tested m8c-tcp on hardware not listed here, please:
1. Open an issue with your configuration details
2. Include: device name, OS version, backend used, audio method, any issues encountered
