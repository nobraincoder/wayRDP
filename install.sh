#!/usr/bin/env bash
set -e

# ==============================================================================
# wayRDP Server - Installer Script
# Builds, installs, and sets up systemd user service for wayrdp
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_INSTALL_DIR="${HOME}/.local/bin"
SYSTEMD_USER_DIR="${HOME}/.config/systemd/user"
CONFIG_FILE="${HOME}/.config/wayrdp.env"

echo "=== wayRDP Server Installer ==="

# 1. Check Package Manager & Prerequisites
check_dependencies() {
    local missing=()
    echo "-> Checking build tools and dependencies..."

    for cmd in cmake pkg-config gcc openssl kscreen-doctor; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done

    # Check pkg-config packages
    for pkg in freerdp3 winpr3; do
        if ! pkg-config --exists "$pkg" 2>/dev/null; then
            missing+=("pkg-config:$pkg")
        fi
    done

    if [ ${#missing[@]} -gt 0 ]; then
        echo -e "\n[WARNING] Some dependencies might be missing: ${missing[*]}"
        echo "Please install prerequisites before building:"
        if command -v pacman &>/dev/null; then
            echo "  Arch/EndeavourOS: sudo pacman -S --needed base-devel cmake extra-cmake-modules pkgconf qt6-base qt6-multimedia kguiaddons kpipewire freerdp libxkbcommon pam openssl libpulse libkscreen"
        elif command -v dnf &>/dev/null; then
            echo "  Fedora:           sudo dnf install cmake extra-cmake-modules gcc-c++ qt6-qtbase-devel qt6-qtmultimedia-devel kf6-kguiaddons-devel kpipewire-devel freerdp-devel libwinpr-devel libxkbcommon-devel libei-devel pam-devel openssl-devel openssl pulseaudio-libs-devel libkscreen-devel libkscreen"
        elif command -v apt-get &>/dev/null; then
            echo "  Ubuntu/Debian:    sudo apt-get install build-essential cmake extra-cmake-modules qt6-base-dev qt6-multimedia-dev libkf6guiaddons-dev libkpipewire-dev freerdp3-dev libwinpr3-dev libxkbcommon-dev libpam0g-dev libssl-dev openssl libpulse-dev libkscreen-dev libkscreen-bin"
        elif command -v zypper &>/dev/null; then
            echo "  openSUSE:         sudo zypper install cmake extra-cmake-modules gcc-c++ pkg-config qt6-base-devel qt6-multimedia-devel kf6-kguiaddons-devel libkpipewire-devel freerdp-devel libwinpr3-devel libxkbcommon-devel libei-devel pam-devel libopenssl-devel openssl libpulse-devel libkscreen6-devel libkscreen6-plugin"
        fi
        echo ""
        read -r -p "Do you want to continue anyway? [y/N] " response
        if [[ ! "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
            exit 1
        fi
    else
        echo "   All core tools found."
    fi
}

check_dependencies

# 2. Build the project
echo -e "\n-> Building wayrdp..."
cd "${SCRIPT_DIR}"
mkdir -p build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

if [ ! -f "build/wayrdp" ]; then
    echo "[ERROR] Compilation failed: build/wayrdp not found."
    exit 1
fi
echo "   Build completed successfully."

# 3. Install binary to ~/.local/bin
echo -e "\n-> Installing executable to ${BIN_INSTALL_DIR}..."
mkdir -p "${BIN_INSTALL_DIR}"
cp build/wayrdp "${BIN_INSTALL_DIR}/wayrdp"
chmod +x "${BIN_INSTALL_DIR}/wayrdp"
echo "   Installed ${BIN_INSTALL_DIR}/wayrdp"

# 4. Install configuration file if not already present
if [ ! -f "${CONFIG_FILE}" ]; then
    echo -e "\n-> Initializing default configuration at ${CONFIG_FILE}..."
    mkdir -p "$(dirname "${CONFIG_FILE}")"
    if [ -f "${SCRIPT_DIR}/wayrdp.env.example" ]; then
        cp "${SCRIPT_DIR}/wayrdp.env.example" "${CONFIG_FILE}"
    else
        cat << 'EOF' > "${CONFIG_FILE}"
RDP_PORT=3390
RDP_PASSWORD=testpass
KPIPEWIRE_FORCE_ENCODER=h264_vaapi
EOF
    fi
    chmod 600 "${CONFIG_FILE}"
    echo "   Created ${CONFIG_FILE} (edit this file to change port or credentials)."
else
    echo "   Found existing configuration at ${CONFIG_FILE} (preserved)."
fi

# 5. Install systemd user service
echo -e "\n-> Setting up systemd user service..."
mkdir -p "${SYSTEMD_USER_DIR}"
cp "${SCRIPT_DIR}/wayrdp.service" "${SYSTEMD_USER_DIR}/wayrdp.service"
systemctl --user daemon-reload
echo "   Installed ${SYSTEMD_USER_DIR}/wayrdp.service"

# 6. Summary and service enable instructions
echo -e "\n=== Installation Complete! ==="
echo "You can now manage the server with systemd:"
echo "  • Start server:   systemctl --user start wayrdp"
echo "  • Enable on boot: systemctl --user enable wayrdp"
echo "  • Check status:   systemctl --user status wayrdp"
echo "  • View live logs: journalctl --user -u wayrdp -f"
echo "  • Stop server:    systemctl --user stop wayrdp"
echo ""
echo "Configuration file: ${CONFIG_FILE}"
echo "Server Port:        3390 (Default, official KRdp uses 3389)"
echo ""
