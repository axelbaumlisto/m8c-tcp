#!/bin/bash
# M8C Audio Loopback Script
# Routes audio from M8 USB device to system output
# Based on rg351_m8c optimized parameters

set -e

# Configuration
M8_DEVICE=""
OUTPUT_DEVICE="hw:0,0"  # Default system output
SAMPLE_RATE=44100
BUFFER_TIME=200000      # 200ms buffer (microseconds)
PERIODS=5

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -o, --output DEVICE   Output device (default: hw:0,0)"
    echo "  -r, --rate RATE       Sample rate (default: 44100)"
    echo "  -b, --buffer TIME     Buffer time in microseconds (default: 200000)"
    echo "  -l, --list            List available audio devices"
    echo "  -h, --help            Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                    # Auto-detect M8 and use defaults"
    echo "  $0 -o hw:1,0          # Use specific output device"
    echo "  $0 -b 100000          # Lower latency (may cause crackling)"
}

list_devices() {
    echo -e "${GREEN}=== Playback Devices ===${NC}"
    aplay -l 2>/dev/null || echo "No playback devices found"
    echo ""
    echo -e "${GREEN}=== Capture Devices ===${NC}"
    arecord -l 2>/dev/null || echo "No capture devices found"
}

find_m8_device() {
    # Try to find M8 in capture devices
    local m8_card=$(arecord -l 2>/dev/null | grep -i "M8\|DirtyWave" | head -1 | sed 's/card \([0-9]*\):.*/\1/')

    if [ -n "$m8_card" ]; then
        M8_DEVICE="hw:${m8_card},0"
        echo -e "${GREEN}Found M8 device: ${M8_DEVICE}${NC}"
        return 0
    fi
    return 1
}

wait_for_m8() {
    echo -e "${YELLOW}Waiting for M8 device...${NC}"
    while ! find_m8_device; do
        sleep 1
    done
}

start_loop() {
    echo -e "${GREEN}Starting audio loopback:${NC}"
    echo "  Input:  $M8_DEVICE (M8)"
    echo "  Output: $OUTPUT_DEVICE"
    echo "  Rate:   $SAMPLE_RATE Hz"
    echo "  Buffer: $BUFFER_TIME us"
    echo ""
    echo -e "${YELLOW}Press Ctrl+C to stop${NC}"

    # alsaloop parameters from rg351_m8c:
    # -P: playback device
    # -C: capture device
    # -t: poll interval (buffer time)
    # -A: ALSA buffer size multiplier
    # --rate: sample rate
    # --sync=0: disable sync
    # -T -1: infinite loop
    # -d: daemonize (we don't use this to keep control)

    alsaloop \
        -P "$OUTPUT_DEVICE" \
        -C "$M8_DEVICE" \
        -t "$BUFFER_TIME" \
        -A "$PERIODS" \
        --rate "$SAMPLE_RATE" \
        --sync=0 \
        -T -1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -o|--output)
            OUTPUT_DEVICE="$2"
            shift 2
            ;;
        -r|--rate)
            SAMPLE_RATE="$2"
            shift 2
            ;;
        -b|--buffer)
            BUFFER_TIME="$2"
            shift 2
            ;;
        -l|--list)
            list_devices
            exit 0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Check for alsaloop
if ! command -v alsaloop &> /dev/null; then
    echo -e "${RED}Error: alsaloop not found. Install alsa-utils package.${NC}"
    exit 1
fi

# Find or wait for M8
if ! find_m8_device; then
    wait_for_m8
fi

# Start the loop
start_loop
