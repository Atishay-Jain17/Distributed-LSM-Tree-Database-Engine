#!/usr/bin/env bash
# Distributed LSM-Tree Database Engine - Web Dashboard Launcher (Linux/WSL)
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
echo "Starting Distributed LSM-Tree Web Dashboard..."
python3 "${DIR}/server.py"
