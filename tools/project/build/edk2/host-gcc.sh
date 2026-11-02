#!/usr/bin/env bash
# The pinned EDK2 BaseTools predate GCC 15's default C23 dialect.
set -euo pipefail
exec /usr/bin/gcc -std=gnu17 -Wno-unused-but-set-variable "$@"
