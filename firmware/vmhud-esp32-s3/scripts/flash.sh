#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENV="${PIO_ENV:-esp32-s3-supermini}"
PORT="$("$ROOT/scripts/esp32_resolve_port.sh")"
echo "Upload env=$ENV port=$PORT"
exec pio run -d "$ROOT" -e "$ENV" -t upload --upload-port "$PORT" "$@"
