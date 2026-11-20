#!/usr/bin/env bash
# Usage: scripts/deploy.sh <vm-ip>   (Git Bash, binaries must already be built)
set -euo pipefail
ip="${1:?usage: deploy.sh <vm-ip>}"
cd "$(dirname "$0")/.."
ssh "root@${ip}" "mkdir -p /tmp/lintasguard"
scp build/target/* "root@${ip}:/tmp/lintasguard/"
