#!/usr/bin/env sh
set -euo pipefail

# collect_diag_logs.sh
# Copies diagnostic logs from /data_persist to /mnt/sda1 and cleans originals.
# Usage:
#   sudo chmod +x resources/collect_diag_logs.sh
#   sudo resources/collect_diag_logs.sh

SRC_DIR=/data_persist
DST_BASE=/mnt/sda1/diag-logs
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
DST_DIR="${DST_BASE}/${TIMESTAMP}"

mkdir -p "$DST_DIR"

echo "Destination: $DST_DIR"

copy_file() {
  src="$1"
  if [ -e "$src" ]; then
    if [ -d "$src" ]; then
      cp -a "$src" "$DST_DIR/" || { echo "Failed to copy directory $src"; return 1; }
    else
      cp -a "$src" "$DST_DIR/" || { echo "Failed to copy file $src"; return 1; }
    fi
    return 0
  else
    return 2
  fi
}

# Copy known log files/paths.
for path in /tmp/carplay_bridge.log /data_persist/msgtype_diag.log /data_persist/nav_diag.log /data_persist/splim_udpd.log; do
  copy_file "$path" || true
done

# Dump system logcat / dmesg if available
if command -v logcat >/dev/null 2>&1; then
  logcat -d > "$DST_DIR/logcat.log" 2>/dev/null || true
fi
dmesg > "$DST_DIR/dmesg.log" 2>/dev/null || true

# Ensure data is written to disk
sync

# Clean up legacy diagnostic files on flash if present:
rm -f "$SRC_DIR/msgtype_diag.log" "$SRC_DIR/nav_diag.log" "$SRC_DIR/splim_udpd.log"
if [ -d "$SRC_DIR/nav_diag" ]; then
  rm -rf "$SRC_DIR/nav_diag"
fi
fi

echo "Done: logs copied to $DST_DIR and originals cleaned."

exit 0
