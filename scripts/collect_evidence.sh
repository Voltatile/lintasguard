#!/usr/bin/env bash
# Usage: scripts/collect_evidence.sh <vm-ip>   (runs tests/target/*.sh over ssh)
set -euo pipefail
ip="${1:?usage: collect_evidence.sh <vm-ip>}"
cd "$(dirname "$0")/.."
shopt -s nullglob
rc=0
for t in tests/target/*.sh; do
    ssh "root@${ip}" 'sh -s' < "$t" | tee "evidence/$(basename "$t" .sh).txt" || rc=1
done
exit $rc
