#!/usr/bin/env bash
# SimpleInit supplies its own pre-C23 stdbool.h.
set -euo pipefail
exec /usr/bin/aarch64-none-linux-gnu-gcc -std=gnu17 "$@"
