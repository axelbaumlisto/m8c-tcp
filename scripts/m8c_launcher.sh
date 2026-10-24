#!/bin/bash
# M8C Launcher with Performance Modes
# Configures CPU governor and launches m8c with optimal settings

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
M8C_BIN="${SCRIPT_DIR}/../m8c"

# Default mode
MODE="balanced"
WITH_AUDIO=false
AUDIO_SCRIPT="${SCRIPT_DIR}/m8c_audio_loop.sh"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    echo -e "${CYAN}M8C Launcher${NC} - Launch m8c with performance optimizations"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Performance Modes:"
    echo "  -p, --powersave      Low power mode (reduces audio crackling on weak hardware)"
    echo "  -b, --balanced       Balanced mode (default)"
    echo "  -P, --performance    Performance mode (lowest latency)"
    echo ""
    echo "Audio Options:"
    echo "  -a, --audio          Enable audio loopback (alsaloop)"
    echo ""
    echo "Other Options:"
    echo "  -d, --dev DEVICE     Use specific M8 device"
    echo "  -c, --config FILE    Use specific config file"
    echo "  -h, --help           Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                       # Balanced mode, no audio loopback"
    echo "  $0 -P -a                 # Performance mode with audio"
    echo "  $0 --powersave --audio   # Powersave with audio (for weak CPUs)"
}

set_cpu_governor() {
    local governor=$1

    if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
        echo -e "${YELLOW}Setting CPU governor to ${governor}${NC}"
        echo "$governor" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null
    else
        echo -e "${YELLOW}Cannot set CPU governor (requires root or sysfs access)${NC}"
    fi
}

disable_wifi_powersave() {
    # Disable WiFi power management to reduce audio glitches
    if command -v iwconfig &> /dev/null; then
        local wifi_dev=$(iwconfig 2>/dev/null | grep -o '^[a-z0-9]*' | head -1)
        if [ -n "$wifi_dev" ]; then
            echo -e "${YELLOW}Disabling WiFi power save on ${wifi_dev}${NC}"
            sudo iwconfig "$wifi_dev" power off 2>/dev/null || true
        fi
    fi
}

# Parse arguments
M8C_ARGS=""
while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--powersave)
            MODE="powersave"
            shift
            ;;
        -b|--balanced)
            MODE="balanced"
            shift
            ;;
        -P|--performance)
            MODE="performance"
            shift
            ;;
        -a|--audio)
            WITH_AUDIO=true
            shift
            ;;
        -d|--dev)
            M8C_ARGS="$M8C_ARGS --dev $2"
            shift 2
            ;;
        -c|--config)
            M8C_ARGS="$M8C_ARGS --config $2"
            shift 2
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

# Check for m8c binary
if [ ! -x "$M8C_BIN" ]; then
    # Try current directory
    if [ -x "./m8c" ]; then
        M8C_BIN="./m8c"
    else
        echo -e "${RED}Error: m8c binary not found${NC}"
        echo "Build m8c first with: make"
        exit 1
    fi
fi

echo -e "${GREEN}=== M8C Launcher ===${NC}"
echo -e "Mode: ${CYAN}${MODE}${NC}"

# Apply mode-specific settings
case $MODE in
    powersave)
        set_cpu_governor "powersave"
        # Powersave: higher latency but more stable
        export SDL_HINT_MAIN_CALLBACK_RATE=60
        ;;
    balanced)
        set_cpu_governor "ondemand"
        # Default 120Hz callback rate
        ;;
    performance)
        set_cpu_governor "performance"
        disable_wifi_powersave
        # Can go higher if needed
        export SDL_HINT_MAIN_CALLBACK_RATE=120
        ;;
esac

# Start audio loopback if requested
AUDIO_PID=""
if [ "$WITH_AUDIO" = true ]; then
    if [ -x "$AUDIO_SCRIPT" ]; then
        echo -e "${GREEN}Starting audio loopback...${NC}"
        "$AUDIO_SCRIPT" &
        AUDIO_PID=$!
        sleep 1
    else
        echo -e "${YELLOW}Warning: Audio script not found at ${AUDIO_SCRIPT}${NC}"
    fi
fi

# Cleanup function
cleanup() {
    if [ -n "$AUDIO_PID" ]; then
        echo -e "${YELLOW}Stopping audio loopback...${NC}"
        kill "$AUDIO_PID" 2>/dev/null || true
        pkill -f alsaloop 2>/dev/null || true
    fi

    # Restore balanced governor
    if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
        echo "ondemand" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null 2>&1 || true
    fi
}

trap cleanup EXIT

# Run m8c
echo -e "${GREEN}Starting m8c...${NC}"
exec "$M8C_BIN" $M8C_ARGS
