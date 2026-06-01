#!/usr/bin/env bash
# Initialise the project: set up git and pull the ra01s SX126x driver
set -e

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
SUBMODULE_PATH="ext/esp-idf-sx126x"
SUBMODULE_URL="https://github.com/nopnop2002/esp-idf-sx126x.git"
INITIAL_COMMIT_CREATED=0

cd "$REPO_ROOT"

if [ ! -d .git ]; then
    git init
    INITIAL_COMMIT_CREATED=1
fi

echo "Fetching ra01s (nopnop2002/esp-idf-sx126x)..."

SUBMODULE_MODE="$(git ls-files --stage -- "$SUBMODULE_PATH" | awk '{print $1}')"
if [ "$SUBMODULE_MODE" != "160000" ]; then
    mkdir -p "$(dirname "$SUBMODULE_PATH")"
    git submodule add --force --depth 1 "$SUBMODULE_URL" "$SUBMODULE_PATH"
else
    git submodule update --init --depth 1 "$SUBMODULE_PATH"
fi

if [ "$INITIAL_COMMIT_CREATED" -eq 1 ]; then
    git add .
    git commit -m "chore: initial project scaffold"
fi

echo ""
echo "Done. Build with:"
echo "  idf.py set-target esp32s3"
echo "  idf.py menuconfig       # set PING or PONG role under 'Ping-Pong Configuration'"
echo "  idf.py build"
echo "  idf.py -p /dev/ttyACM0 flash"
echo "  idf.py -p /dev/ttyACM0 monitor --no-reset"
