#!/usr/bin/env bash
# Usage: scripts/deploy.sh <vm-ip>   (Git Bash, binaries must already be built)
# Stops any running copy of each binary first (a running executable cannot be overwritten),
# by PID from pidin: slay matches the name exactly as the process was started, which is unreliable.
# Copies the binaries to /tmp/lintasguard and the config and scenario files next to them.
set -euo pipefail
ip="${1:?usage: deploy.sh <vm-ip>}"
cd "$(dirname "$0")/.."

ssh "root@${ip}" "mkdir -p /tmp/lintasguard"
for f in build/target/*; do
    name="$(basename "$f")"
    ssh "root@${ip}" "for p in \$(pidin -p ${name} -F '%a' 2>/dev/null | grep -E '^ *[0-9]+\$'); do kill -9 \$p; done; true"
done
sleep 1
scp build/target/* "root@${ip}:/tmp/lintasguard/"
scp -r config scenarios "root@${ip}:/tmp/lintasguard/"
ssh "root@${ip}" "chmod +x /tmp/lintasguard/*"
