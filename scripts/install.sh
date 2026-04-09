#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

MODULE_ID="notetwist"
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-ableton}"
INSTALL_DIR="/data/UserData/schwung/modules/midi_fx/${MODULE_ID}"

# Check that build exists
if [ ! -f "dist/${MODULE_ID}/dsp.so" ]; then
    echo "Error: Build not found. Run ./scripts/build.sh first."
    exit 1
fi

# Check SSH connection
echo "Checking connection to ${MOVE_HOST}..."
if ! ssh -o ConnectTimeout=5 "${MOVE_USER}@${MOVE_HOST}" true 2>/dev/null; then
    echo "Error: Cannot reach ${MOVE_HOST}"
    echo "Make sure your Move is on and connected to the same network."
    exit 1
fi
echo "Connected."

# Deploy
echo "Installing ${MODULE_ID} to ${INSTALL_DIR}..."
ssh "${MOVE_USER}@${MOVE_HOST}" "mkdir -p ${INSTALL_DIR}"
scp -r dist/${MODULE_ID}/* "${MOVE_USER}@${MOVE_HOST}:${INSTALL_DIR}/"

echo ""
echo "Installation complete!"
echo "Module installed to: ${INSTALL_DIR}"
echo ""
echo "Restart Schwung or use host_rescan_modules() to pick up the new module."
