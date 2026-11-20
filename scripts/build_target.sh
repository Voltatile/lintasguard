#!/usr/bin/env bash
# Needs: call E:\code\Heketon\QNX\qnx-env.cmd before starting this shell.
set -euo pipefail
cd "$(dirname "$0")/.."
make target
