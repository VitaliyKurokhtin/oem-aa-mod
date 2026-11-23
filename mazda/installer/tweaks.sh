#!/bin/sh
# Bridge between USB autorun triggers (.up injection or dataRetrieval) and run.sh
MYDIR=$(dirname "$(readlink -f "$0" 2>/dev/null || echo "$0")")
DLG=/jci/tools/jci-dialog
TITLE="oem-aa-mod"

hwclock --hctosys 2>/dev/null
echo 1 > "/sys/class/gpio/Watchdog Disable/value" 2>/dev/null

RUNNER=""
for p in "$MYDIR/run.sh" "$MYDIR/installer/run.sh" "$MYDIR/mazda/installer/run.sh"; do
  if [ -f "$p" ]; then
    RUNNER="$p"
    break
  fi
done

if [ -z "$RUNNER" ]; then
  $DLG --info --title="$TITLE" \
    --text="ERROR: run.sh not found on USB drive.\nPlease recreate the USB installer." \
    --no-cancel &
  sleep 15; killall -q jci-dialog; exit 1
fi

exec sh "$RUNNER"
