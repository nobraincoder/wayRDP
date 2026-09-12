#!/usr/bin/env bash
set -e

# ==============================================================================
# wayRDP Server - Uninstaller Script
# ==============================================================================

BIN_FILE="${HOME}/.local/bin/wayrdp"
SERVICE_FILE="${HOME}/.config/systemd/user/wayrdp.service"
CONFIG_FILE="${HOME}/.config/wayrdp.env"

echo "=== wayRDP Server Uninstaller ==="

# 1. Stop and disable systemd service
if systemctl --user is-active --quiet wayrdp 2>/dev/null; then
    echo "-> Stopping active service..."
    systemctl --user stop wayrdp
fi

if systemctl --user is-enabled --quiet wayrdp 2>/dev/null; then
    echo "-> Disabling service..."
    systemctl --user disable wayrdp
fi

# 2. Remove systemd service unit
if [ -f "${SERVICE_FILE}" ]; then
    echo "-> Removing systemd user unit: ${SERVICE_FILE}"
    rm -f "${SERVICE_FILE}"
    systemctl --user daemon-reload
fi

# 3. Remove binary
if [ -f "${BIN_FILE}" ]; then
    echo "-> Removing binary: ${BIN_FILE}"
    rm -f "${BIN_FILE}"
fi

echo -e "\n=== Uninstallation Complete! ==="
echo "Note: Configuration file ${CONFIG_FILE} was preserved."
echo "To completely wipe credentials and configuration, run: rm -f ${CONFIG_FILE}"
echo ""
