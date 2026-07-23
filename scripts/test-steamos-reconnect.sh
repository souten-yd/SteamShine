#!/usr/bin/env bash
# Run the interactive virtual-display acceptance cycle ten times.
set -Eeuo pipefail
exec "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/test-steamos-virtual-display.sh"
