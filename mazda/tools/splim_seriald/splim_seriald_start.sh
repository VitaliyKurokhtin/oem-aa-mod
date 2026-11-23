#!/bin/sh
# splim_seriald_start.sh — launcher for the USB serial → /data_persist/splim bridge.
#
# Installed to /data_persist/splim_seriald_start.sh — the exact path the HUD
# bridge invokes (see hud_send.cpp):
#   pgrep splim_seriald >/dev/null 2>&1 || setsid sh /data_persist/splim_seriald_start.sh &
#
# Optional: set SPLIM_SERIAL_DEV=/dev/ttyACM0 in this file if auto-detect fails.
# `exec` replaces this shell so the running process is named "splim_seriald".

# ESP32 native USB CDC on CMU150 (FW 74.00.324A) needs cdc-acm. Deliberately the
# only module loaded: the firmware targets native CDC, so the USB-UART drivers
# (ftdi_sio, pl2303) would just add kernel code that can bind unrelated devices.
modprobe cdc-acm 2>/dev/null

# Uncomment to pin a device:
# export SPLIM_SERIAL_DEV=/dev/ttyACM0

exec /data_persist/oem-aa-mod/splim_seriald
