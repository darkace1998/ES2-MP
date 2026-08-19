#!/usr/bin/env bash
# Run an arbitrary Windows exe inside the ES2 Proton prefix:  proton-run.sh <exe> [args...]
set -e
source "$(dirname "$0")/proton-env.sh"
exec "$PROTON_DIR/proton" run "$@"
