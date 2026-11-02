#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export PYTHONPATH="$project_root/tools:$project_root${PYTHONPATH:+:$PYTHONPATH}"
exec "${PYTHON:-python3}" -m project "$@"
