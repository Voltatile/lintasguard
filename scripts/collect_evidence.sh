#!/usr/bin/env bash
# Usage: scripts/collect_evidence.sh <vm-ip> [test-name ...]
# Runs tests/target/<test-name>.sh on the target over ssh and saves the output to
# evidence/<test-name>.txt. Without test names it runs every tests/target/*.sh.
#
# A run REPLACES the evidence file of the test it runs, so the numbers quoted in evidence/STATUS.md
# and docs/ must match the new file. Run only the test you changed (for example
# `make evidence IP=<ip> TESTS=step06_sensors`) so that the evidence of earlier steps stays as it was.
set -euo pipefail
ip="${1:?usage: collect_evidence.sh <vm-ip> [test-name ...]}"
shift
cd "$(dirname "$0")/.."

tests=()
if [ "$#" -gt 0 ]; then
    for name in "$@"; do
        [ -f "tests/target/${name}.sh" ] || { echo "no such test: tests/target/${name}.sh" >&2; exit 2; }
        tests+=("tests/target/${name}.sh")
    done
else
    shopt -s nullglob
    tests=(tests/target/*.sh)
fi

rc=0
for t in "${tests[@]}"; do
    ssh "root@${ip}" 'sh -s' < "$t" | tee "evidence/$(basename "$t" .sh).txt" || rc=1
done
exit $rc
