#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

MODULE_ID="notetwist"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

# Detect if we need Docker
if ! command -v "${CROSS_PREFIX}gcc" &>/dev/null; then
    echo "Cross compiler not found, building via Docker..."
    docker build -t notetwist-builder -f scripts/Dockerfile .
    docker run --rm -v "$PROJECT_DIR:/build" -w /build notetwist-builder \
        bash -c "CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh"
    exit $?
fi

echo "=== Building NoteTwist MIDI FX ==="
echo "Compiler: ${CROSS_PREFIX}gcc"

mkdir -p dist/${MODULE_ID}

# Compile DSP
echo "Compiling DSP..."
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/dsp/notetwist.c \
    -o dist/${MODULE_ID}/dsp.so \
    -Isrc

# Copy module files
cp src/module.json dist/${MODULE_ID}/
cp src/ui.js dist/${MODULE_ID}/
cp src/help.json dist/${MODULE_ID}/

echo ""
echo "=== Build Artifacts ==="
file dist/${MODULE_ID}/dsp.so
echo ""

# Create tarball for release
echo "Creating tarball..."
cd dist
tar -czvf ${MODULE_ID}-module.tar.gz ${MODULE_ID}/
cd ..

echo ""
echo "=== Package Created ==="
ls -lh dist/${MODULE_ID}-module.tar.gz
echo ""
echo "Build complete!"
echo "To install: ./scripts/install.sh"
