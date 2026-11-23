#!/bin/sh
# oem-aa-mod installer/uninstaller for Mazda CMU
# created by Bijan
# https://github.com/VitaliyKurokhtin/oem-aa-mod
#
# By default a single install patches BOTH. run-aa.sh and run-carplay.sh invoke
# this script with a mode argument to install/uninstall only one integration:
#   * Android Auto  -> jciAAPA (libpatch-blmjciaapa.so) + jcinavi (libpatch-svcjcinavi.so)
#   * CarPlay HUD   -> jciCARPLAY (libpatch-blmjcicarplay.so)
# Each service is patched INDEPENDENTLY and WCP-safely (see patch_service): on
# Wireless-CarPlay units the OEM service manager MERGES a service's stanza from
# sm.conf + sm_WCP.conf, so injecting LD_PRELOAD into both yields a duplicate
# environ_var and crash-loops that service. We therefore patch exactly ONE conf
# per service, sanity-check the edit, and set reset_board="no" so a shim fault
# restarts only that service instead of rebooting the whole unit.

hwclock --hctosys

echo 1 > /sys/class/gpio/Watchdog\ Disable/value
mount -o rw,remount /

MYDIR=$(dirname "$(readlink -f "$0")")
mount -o rw,remount ${MYDIR}

mkdir -p "${MYDIR}/logs"

MOD_DIR="/data_persist/oem-aa-mod"
MODE="${1:-}"

# run.sh is the interactive entry point. The feature-specific wrappers pass a
# mode argument and therefore skip these mode-selection dialogs.
if [ -z "$MODE" ]
then
  /jci/tools/jci-dialog --question \
    --title="oem-aa-mod - Main Menu" \
    --text="What would you like to do?\n• Install Mod: Install or manage HUD mod\n• Enable SSH: Open Wi-Fi SSH root access" \
    --ok-label="Install Mod" \
    --cancel-label="Enable SSH"
  ACTION_CHOICE=$?

  if [ "$ACTION_CHOICE" -ne 0 ]
  then
    MODE="ssh"
  else
    /jci/tools/jci-dialog --question \
      --title="oem-aa-mod - Select mode" \
      --text="Which integrations would you like to manage?" \
      --ok-label="Both" \
      --cancel-label="Choose one"
    MODE_CHOICE=$?

    if [ "$MODE_CHOICE" -eq 0 ]
    then
      MODE="both"
    else
      /jci/tools/jci-dialog --question \
        --title="oem-aa-mod - Select mode" \
        --text="Which integration would you like to manage?" \
        --ok-label="Android Auto" \
        --cancel-label="CarPlay"
      MODE_CHOICE=$?

      if [ "$MODE_CHOICE" -eq 0 ]
      then
        MODE="aa"
      else
        MODE="carplay"
      fi
    fi
  fi
fi

case "$MODE" in
  ssh)
    MODE_LABEL="Enable SSH"
    LOG_SUFFIX="-ssh"
    ;;
  both)
    MODE_LABEL="Android Auto + CarPlay"
    LOG_SUFFIX=""
    ;;
  aa)
    MODE_LABEL="Android Auto"
    LOG_SUFFIX="-aa"
    ;;
  carplay)
    MODE_LABEL="CarPlay"
    LOG_SUFFIX="-carplay"
    ;;
  *)
    echo "Usage: $0 [both|aa|carplay|ssh]" >&2
    exit 2
    ;;
esac

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

includes_aa() {
  [ "$MODE" = "both" ] || [ "$MODE" = "aa" ]
}

includes_carplay() {
  [ "$MODE" = "both" ] || [ "$MODE" = "carplay" ]
}

# back up a file to <file>.orig exactly once (pristine pre-install copy)
bak_orig() {
  f="$1"
  if [ ! -e "${f}.orig" ]; then
    cp -a "$f" "${f}.orig"
    echo "  backup -> ${f}.orig"
  fi
}

# safe_replace <new-content-file> <dest>
# Replace <dest> without ever leaving it truncated: write a sibling temp copy (same
# mode/owner), flush, then rename (atomic). A plain `cp new dest` cut short by a power
# loss would leave a broken sm.conf = unit that no longer boots. Refuses an empty source
# so a failed pipeline can never blank a boot config.
safe_replace() {
  src="$1"; dst="$2"
  if [ ! -s "$src" ]; then
    echo "  refusing to replace ${dst}: new content is empty"
    return 1
  fi
  if cp -a "$dst" "${dst}.oaam.tmp" && cat "$src" > "${dst}.oaam.tmp" &&
     sync && mv "${dst}.oaam.tmp" "$dst"; then
    return 0
  fi
  rm -f "${dst}.oaam.tmp"
  return 1
}

# strip_carplay_ldpath <conf>
# Remove the LD_LIBRARY_PATH line this installer adds, but only inside the jciCARPLAY
# <service> stanza. A file-wide grep would also delete that same line from any other
# service that ships it in the stock config.
strip_carplay_ldpath() {
  c="$1"
  awk '
    index($0, "name=\"jciCARPLAY\"") && /<service/ { inside = 1 }
    inside && /LD_LIBRARY_PATH/ && /jci\/lib:\/usr\/lib/ && !done { done = 1; next }
    { print }
    inside && /<\/service>/ { inside = 0 }
  ' "$c" > /tmp/oaam.clean && safe_replace /tmp/oaam.clean "$c"
  rm -f /tmp/oaam.clean
}

# patch_service <service-name> <preload-so-abs-path> [extra_environ_line]
# Injects LD_PRELOAD (+ an optional extra environ_var) into the stanza named
# <service-name>, in exactly ONE sm config, idempotently and safely.
patch_service() {
  svc="$1"; so="$2"; extra="$3"
  token=$(basename "$so")
  # Never point LD_PRELOAD at a library that is not there (e.g. a failed copy above).
  if [ ! -f "$so" ]; then
    echo "  ${svc}: ${so} missing, not patching sm config"
    return 0
  fi
  preload_line="            <environ_var env_name=\"LD_PRELOAD\" env_value=\"${so}\"/>"

  # which confs actually define this service?
  present=""
  for c in /jci/sm/sm.conf /jci/sm/sm_WCP.conf; do
    [ -f "$c" ] && grep -q "name=\"${svc}\"" "$c" && present="${present} ${c}"
  done
  if [ -z "$present" ]; then
    echo "  ${svc}: no stanza in any sm config, skipping"
    return 0
  fi

  # pick exactly one target: prefer sm_WCP.conf when present (it is the active
  # boot config on WCP units and its stanza is what actually launches).
  target=""
  for c in $present; do
    case "$c" in */sm_WCP.conf) target="$c" ;; esac
  done
  [ -z "$target" ] && target=$(echo $present | awk '{print $1}')
  echo "  ${svc}: patching ${target}"

  # strip any stale inject THIS service left in the non-target conf(s), so the
  # merge can never produce a duplicate environ_var.
  for c in $present; do
    [ "$c" = "$target" ] && continue
    if grep -q "$token" "$c"; then
      bak_orig "$c"
      grep -v "$token" "$c" > /tmp/oaam.clean && safe_replace /tmp/oaam.clean "$c"
      rm -f /tmp/oaam.clean
      echo "  ${svc}: removed stale ${token} from ${c}"
    fi
  done

  bak_orig "$target"

  if grep -q "$token" "$target"; then
    echo "  ${svc}: already patched in ${target}, skipping insert"
  else
    awk -v anchor="name=\"${svc}\"" -v pl="$preload_line" -v ex="$extra" '
      index($0, anchor) { print; print pl; if (ex != "") print ex; next } 1
    ' "$target" > /tmp/oaam.new
    old=$(wc -l < "$target"); new=$(wc -l < /tmp/oaam.new)
    exp=$((old + 1)); [ -n "$extra" ] && exp=$((old + 2))
    if [ "$new" -eq "$exp" ] && grep -q "$token" /tmp/oaam.new &&
       safe_replace /tmp/oaam.new "$target"; then
      echo "  ${svc}: inserted LD_PRELOAD (${old}->${new} lines)"
    else
      echo "  ${svc}: SANITY FAILED (${old}->${new}, expected ${exp}), left untouched"
    fi
    rm -f /tmp/oaam.new
  fi

  # a shim fault restarts just this service, not the whole unit
  sed -i "/name=\"${svc}\"/ s/reset_board=\"yes\"/reset_board=\"no\"/" "$target"
}

# unpatch_service <service-name> <preload-so-basename>
# Removes our injected LD_PRELOAD/LD_LIBRARY_PATH lines for this service's .so
# from every sm config (used only when a .orig restore isn't available).
unpatch_service() {
  token="$2"
  for c in /jci/sm/sm.conf /jci/sm/sm_WCP.conf; do
    [ -f "$c" ] || continue
    if grep -q "$token" "$c"; then
      grep -v "$token" "$c" > /tmp/oaam.clean && safe_replace /tmp/oaam.clean "$c"
      rm -f /tmp/oaam.clean
      echo "  removed ${token} lines from ${c}"
    fi
  done
}

# restore <file> from <file>.orig if the backup exists
restore_orig() {
  f="$1"
  if [ -e "${f}.orig" ]; then
    cp -a "${f}.orig" "$f"
    rm -f "${f}.orig"
    echo "  restored ${f} from backup"
    return 0
  fi
  return 1
}

# --- Handle SSH action directly without asking Install/Uninstall ---
if [ "$MODE" = "ssh" ]; then
  exec > "${MYDIR}/logs/enable_ssh.log" 2>&1
  echo "=== oem-aa-mod Enable SSH start (session only) ==="

  /usr/sbin/sshd 2>/dev/null
  echo "root:mazda" | chpasswd 2>/dev/null
  echo "cmu:jci" | chpasswd 2>/dev/null

  sync
  echo "=== oem-aa-mod Enable SSH complete ==="

  killall -q jci-dialog 2>/dev/null
  /jci/tools/jci-dialog --info \
    --title="SSH Enabled" \
    --text="SSH service is active for this session only!\nConnect laptop to car Wi-Fi hotspot:\n• IP: 192.168.53.1 (Port 22/36000)\n• user: root / pass: mazda\n• user: cmu  / pass: jci\n(SSH turns off automatically on car restart)" \
    --no-cancel &
  sleep 15
  killall -q jci-dialog 2>/dev/null
  exit 0
fi

# --- Prompt user: Install or Uninstall ---
/jci/tools/jci-dialog --question \
  --title="oem-aa-mod - ${MODE_LABEL}" \
  --text="What would you like to do for ${MODE_LABEL}?" \
  --ok-label="Install" \
  --cancel-label="Uninstall"
CHOICE=$?

if [ $CHOICE -eq 0 ]
then
  # ============================================================
  # INSTALL
  # ============================================================
  exec > "${MYDIR}/logs/install${LOG_SUFFIX}.log" 2>&1
  echo "=== oem-aa-mod ${MODE_LABEL} install start ==="

  /jci/tools/jci-dialog --info --title="oem-aa-mod - ${MODE_LABEL}" --text="Installing ${MODE_LABEL}...\nDo not remove USB drive." --no-cancel &

  # --- Install libraries -------------------------------------------------
  mkdir -p "$MOD_DIR"
  if includes_aa
  then
    for file in libpatch-blmjciaapa.so libpatch-svcjcinavi.so
    do
      cp "${MYDIR}/${file}" "${MOD_DIR}/" && echo "${file} has been copied"
    done
  fi
  if includes_carplay
  then
    cp "${MYDIR}/libpatch-blmjcicarplay.so" "${MOD_DIR}/" && echo "libpatch-blmjcicarplay.so has been copied"
    cp "${MYDIR}/resources/libpatch-carplay.conf" "${MOD_DIR}/" && echo "libpatch-carplay.conf has been copied"
  fi
  # Audio Off fallback (svcjcimmui) applies to both AA and CarPlay disconnect
  # paths; it reads fallback_audio_off from libpatch.conf.
  if [ -f "${MYDIR}/libpatch-svcjcimmui.so" ]; then
    cp "${MYDIR}/libpatch-svcjcimmui.so" "${MOD_DIR}/" && echo "libpatch-svcjcimmui.so has been copied"
  fi
  cp "${MYDIR}/resources/libpatch.conf" "${MOD_DIR}/" && echo "libpatch.conf has been copied"
  chmod 0644 "${MOD_DIR}"/libpatch-*.so

  # --- Install the shared speed-limit UDP daemon (splim_udpd) ------------
  # Used by the CarPlay HUD bridge when carplay_speed_limit=true. The launcher
  # must live at the exact path the HUD bridge calls:
  # /data_persist/splim_udpd_start.sh
  if includes_carplay && [ -f "${MYDIR}/splim_udpd" ]; then
    cp "${MYDIR}/splim_udpd" "${MOD_DIR}/" && echo "splim_udpd has been copied"
    chmod 0755 "${MOD_DIR}/splim_udpd"
    cp "${MYDIR}/splim_udpd_start.sh" /data_persist/ && echo "splim_udpd_start.sh has been copied"
    chmod 0755 /data_persist/splim_udpd_start.sh
  elif includes_carplay; then
    echo "splim_udpd not in payload, skipping"
  fi

  # --- Install ESP32 USB serial → splim daemon (splim_seriald) -----------
  # Preferred VietMap path: BLE HUD emulator → CDC "SPLIM N" → /data_persist/splim.
  # Enable with carplay_seriald_launch=true (and usually carplay_udpd_launch=false).
  if includes_carplay && [ -f "${MYDIR}/splim_seriald" ]; then
    cp "${MYDIR}/splim_seriald" "${MOD_DIR}/" && echo "splim_seriald has been copied"
    chmod 0755 "${MOD_DIR}/splim_seriald"
    cp "${MYDIR}/splim_seriald_start.sh" /data_persist/ && echo "splim_seriald_start.sh has been copied"
    chmod 0755 /data_persist/splim_seriald_start.sh
  elif includes_carplay; then
    echo "splim_seriald not in payload, skipping"
  fi

  # --- Patch each service independently (WCP-safe) -----------------------
  if includes_aa
  then
    echo "--- patching Android Auto ---"
    patch_service jciAAPA "${MOD_DIR}/libpatch-blmjciaapa.so"
    patch_service jcinavi "${MOD_DIR}/libpatch-svcjcinavi.so"
  fi

  if includes_carplay
  then
    echo "--- patching CarPlay ---"
    patch_service jciCARPLAY "${MOD_DIR}/libpatch-blmjcicarplay.so" \
      '            <environ_var env_name="LD_LIBRARY_PATH" env_value="/jci/lib:/usr/lib"/>'
  fi

  # jciMMUI hosts libjciuiasystem.so (source fallback / Audio Off).
  if [ -f "${MOD_DIR}/libpatch-svcjcimmui.so" ]; then
    echo "--- patching jciMMUI (Audio Off fallback) ---"
    patch_service jciMMUI "${MOD_DIR}/libpatch-svcjcimmui.so"
  fi

  # --- Android Auto: install patched XML system attributes ---------------
  if includes_aa
  then
    for xml in aap_system_attributes.xml aap_system_attributes_UCP.xml
    do
      if [ -e "/etc/${xml}" ]
      then
        bak_orig "/etc/${xml}"
        cp "${MYDIR}/resources/${xml}" "/etc/${xml}"
        echo "Installed ${xml}"
      else
        echo "WARNING: /etc/${xml} absent, skipping"
      fi
    done
  fi

  # --- CarPlay: advertise nav support in the devmgr master template ------
  if includes_carplay
  then
    MASTER="/etc/devmgr_config_master.xml"
    if [ -f "$MASTER" ]
    then
      bak_orig "$MASTER"
      sed -i 's#<name>NaviSupported</name><value>FALSE</value>#<name>NaviSupported</name><value>TRUE</value>#' "$MASTER"
      echo "set NaviSupported=TRUE in ${MASTER}"
    else
      echo "WARNING: ${MASTER} absent, NaviSupported not set"
    fi
  fi

  sync
  echo "=== oem-aa-mod ${MODE_LABEL} install complete ==="

  killall -q jci-dialog
  /jci/tools/jci-dialog --info --title="oem-aa-mod - ${MODE_LABEL}" --text="${MODE_LABEL} installation complete!\nYou can remove the USB drive.\nRebooting in 5 seconds..." --no-cancel &

else
  # ============================================================
  # UNINSTALL
  # ============================================================
  exec > "${MYDIR}/logs/uninstall${LOG_SUFFIX}.log" 2>&1
  echo "=== oem-aa-mod ${MODE_LABEL} uninstall start ==="

  /jci/tools/jci-dialog --info --title="oem-aa-mod - ${MODE_LABEL}" --text="Uninstalling ${MODE_LABEL}...\nDo not remove USB drive." --no-cancel &

  # --- Restore sm configs (fall back to line-stripping) ------------------
  if [ "$MODE" = "both" ]
  then
    for conf in /jci/sm/sm.conf /jci/sm/sm_WCP.conf
    do
      [ -f "$conf" ] || continue
      if ! restore_orig "$conf"
      then
        echo "  ${conf}.orig not found; stripping injected lines"
        unpatch_service jciAAPA    libpatch-blmjciaapa.so
        unpatch_service jcinavi    libpatch-svcjcinavi.so
        unpatch_service jciCARPLAY libpatch-blmjcicarplay.so
        unpatch_service jciMMUI    libpatch-svcjcimmui.so
        # the CarPlay LD_LIBRARY_PATH line has no .so token; strip it explicitly
        strip_carplay_ldpath "$conf"
      fi
    done
  else
    # A feature-only uninstall must not restore the whole config because the
    # other feature may still be installed in the same file.
    if includes_aa
    then
      unpatch_service jciAAPA libpatch-blmjciaapa.so
      unpatch_service jcinavi libpatch-svcjcinavi.so
    fi
    if includes_carplay
    then
      unpatch_service jciCARPLAY libpatch-blmjcicarplay.so
      for conf in /jci/sm/sm.conf /jci/sm/sm_WCP.conf
      do
        [ -f "$conf" ] || continue
        strip_carplay_ldpath "$conf"
      done
    fi
    for conf in /jci/sm/sm.conf /jci/sm/sm_WCP.conf
    do
      [ -f "$conf" ] || continue
      if includes_aa
      then
        sed -i '/name="jciAAPA"/ s/reset_board="no"/reset_board="yes"/' "$conf"
        sed -i '/name="jcinavi"/ s/reset_board="no"/reset_board="yes"/' "$conf"
      fi
      if includes_carplay
      then
        sed -i '/name="jciCARPLAY"/ s/reset_board="no"/reset_board="yes"/' "$conf"
      fi
    done
  fi

  # --- Restore Android Auto XML configs ----------------------------------
  if includes_aa
  then
    for xml in /etc/aap_system_attributes.xml /etc/aap_system_attributes_UCP.xml
    do
      restore_orig "$xml" || echo "  ${xml}.orig not found, skipping"
    done
  fi

  # --- Restore CarPlay devmgr master (or revert NaviSupported) -----------
  if includes_carplay
  then
    MASTER="/etc/devmgr_config_master.xml"
    if [ -f "$MASTER" ] && ! restore_orig "$MASTER"
    then
      sed -i 's#<name>NaviSupported</name><value>TRUE</value>#<name>NaviSupported</name><value>FALSE</value>#' "$MASTER"
      echo "  reverted NaviSupported=FALSE in ${MASTER}"
    fi
  fi

  # --- Remove installed libraries and config -----------------------------
  if includes_aa
  then
    for file in libpatch-blmjciaapa.so libpatch-svcjcinavi.so
    do
      rm -f "${MOD_DIR}/${file}" && echo "Removed ${file}"
    done
  fi
  if includes_carplay
  then
    for file in libpatch-blmjcicarplay.so libpatch-carplay.conf
    do
      rm -f "${MOD_DIR}/${file}" && echo "Removed ${file}"
    done
  fi

  # --- Remove the speed-limit UDP / serial daemons -----------------------
  if includes_carplay
  then
    killall -q splim_udpd 2>/dev/null
    rm -f "${MOD_DIR}/splim_udpd" && echo "Removed splim_udpd"
    rm -f /data_persist/splim_udpd_start.sh && echo "Removed splim_udpd_start.sh"
    killall -q splim_seriald 2>/dev/null
    rm -f "${MOD_DIR}/splim_seriald" && echo "Removed splim_seriald"
    rm -f /data_persist/splim_seriald_start.sh && echo "Removed splim_seriald_start.sh"
    rm -f /data_persist/splim_seriald.lock
  fi

  # Shared Audio Off fallback stays while any AA/CarPlay shim remains.
  aa_gone=0
  cp_gone=0
  [ ! -e "${MOD_DIR}/libpatch-blmjciaapa.so" ] &&
    [ ! -e "${MOD_DIR}/libpatch-svcjcinavi.so" ] && aa_gone=1
  [ ! -e "${MOD_DIR}/libpatch-blmjcicarplay.so" ] && cp_gone=1
  if [ "$aa_gone" -eq 1 ] && [ "$cp_gone" -eq 1 ]
  then
    unpatch_service jciMMUI libpatch-svcjcimmui.so
    for conf in /jci/sm/sm.conf /jci/sm/sm_WCP.conf
    do
      [ -f "$conf" ] || continue
      sed -i '/name="jciMMUI"/ s/reset_board="no"/reset_board="yes"/' "$conf"
    done
    rm -f "${MOD_DIR}/libpatch-svcjcimmui.so" "${MOD_DIR}/libpatch.conf"
    echo "Removed libpatch-svcjcimmui.so and libpatch.conf"
  fi

  # Once the last feature is gone, restore the pristine service configs and
  # remove the shared backups left by feature-only uninstalls.
  if [ "$MODE" != "both" ] &&
     [ ! -e "${MOD_DIR}/libpatch-blmjciaapa.so" ] &&
     [ ! -e "${MOD_DIR}/libpatch-svcjcinavi.so" ] &&
     [ ! -e "${MOD_DIR}/libpatch-blmjcicarplay.so" ] &&
     [ ! -e "${MOD_DIR}/libpatch-svcjcimmui.so" ]
  then
    for conf in /jci/sm/sm.conf /jci/sm/sm_WCP.conf
    do
      [ -f "$conf" ] && restore_orig "$conf"
    done
  fi

  # Clean up any residual stage_wifi.sh edits
  if [ -f /jci/scripts/stage_wifi.sh.bak ]; then
    cp -a /jci/scripts/stage_wifi.sh.bak /jci/scripts/stage_wifi.sh && rm -f /jci/scripts/stage_wifi.sh.bak
  elif grep -q "sshd" /jci/scripts/stage_wifi.sh 2>/dev/null; then
    sed -i '/sshd/d' /jci/scripts/stage_wifi.sh 2>/dev/null
  fi

  rmdir "$MOD_DIR" 2>/dev/null && echo "Removed ${MOD_DIR} directory"

  sync
  echo "=== oem-aa-mod ${MODE_LABEL} uninstall complete ==="

  killall -q jci-dialog
  /jci/tools/jci-dialog --info --title="oem-aa-mod - ${MODE_LABEL}" --text="${MODE_LABEL} uninstall complete!\nYou can remove the USB drive.\nRebooting in 5 seconds..." --no-cancel &

fi

sleep 5
killall -q jci-dialog
reboot
exit 0
