#!/usr/bin/env bash
# Resolve upload port by MAC from .esp32-target.json next to this firmware tree.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET_JSON="$ROOT/.esp32-target.json"
PIO_PY="${HOME}/.platformio/penv/bin/python3"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"

TARGET_MAC="${ESP32_TARGET_MAC:-}"
if [[ -z "$TARGET_MAC" && -f "$TARGET_JSON" ]]; then
  TARGET_MAC="$(python3 -c "import json; print(json.load(open('$TARGET_JSON')).get('mac','').strip())")"
fi

norm() { echo "$1" | tr '[:upper:]' '[:lower:]'; }

collect_pairs() {
  pio device list 2>/dev/null | awk '
    /^\/dev\// { port=$1; next }
    /SER=/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^SER=/) {
          mac = $i
          sub(/^SER=/, "", mac)
          print port, tolower(mac)
        }
      }
    }
  '
}

found_ports=()
found_macs=()
while read -r port mac; do
  [[ -n "$port" && -n "$mac" ]] || continue
  echo "  $port  MAC=$mac" >&2
  found_ports+=("$port")
  found_macs+=("$mac")
done < <(collect_pairs)

if [[ ${#found_ports[@]} -eq 0 ]]; then
  for p in /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/ttyACM* /dev/ttyUSB*; do
    [[ -e "$p" ]] || continue
    if [[ -x "$PIO_PY" && -f "$ESPTOOL" ]]; then
      if out="$("$PIO_PY" "$ESPTOOL" --port "$p" --chip auto read_mac 2>&1)"; then
        mac="$(echo "$out" | rg -i '([0-9a-f]{2}(?::[0-9a-f]{2}){5})' -or '$1' | head -1 | tr '[:upper:]' '[:lower:]')"
        if [[ -n "$mac" ]]; then
          echo "  $p  MAC=$mac" >&2
          found_ports+=("$p")
          found_macs+=("$mac")
        fi
      fi
    fi
  done
fi

if [[ ${#found_ports[@]} -eq 0 ]]; then
  echo "ERROR: no Espressif MAC found (plug board, run 'pio device list')" >&2
  exit 1
fi

if [[ -n "$TARGET_MAC" ]]; then
  want="$(norm "$TARGET_MAC")"
  for i in "${!found_ports[@]}"; do
    if [[ "${found_macs[$i]}" == "$want" ]]; then
      echo "${found_ports[$i]}"
      exit 0
    fi
  done
  echo "ERROR: no port matches target MAC $TARGET_MAC" >&2
  for i in "${!found_ports[@]}"; do
    echo "  ${found_ports[$i]}  ${found_macs[$i]}" >&2
  done
  echo "Edit $TARGET_JSON or export ESP32_TARGET_MAC" >&2
  exit 2
fi

if [[ ${#found_ports[@]} -eq 1 ]]; then
  echo "${found_ports[0]}"
  exit 0
fi

echo "ERROR: ${#found_ports[@]} Espressif devices — pin MAC in $TARGET_JSON" >&2
for i in "${!found_ports[@]}"; do
  echo "  ${found_ports[$i]}  ${found_macs[$i]}" >&2
done
echo "Copy .esp32-target.json.example → .esp32-target.json and set mac." >&2
exit 3