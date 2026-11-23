#!/usr/bin/env bash
# SideStore IPA with Live Activity + device picker (default).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
xcodegen generate -q

xcodebuild -project IcarTPMS.xcodeproj -scheme IcarTPMS -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build

APP="$(ls -d ~/Library/Developer/Xcode/DerivedData/IcarTPMS-*/Build/Products/Release-iphoneos/IcarTPMS.app 2>/dev/null | head -1)"
[[ -d "$APP" ]] || { echo "missing IcarTPMS.app"; exit 1; }

mkdir -p "$ROOT/build"
STAGE="$ROOT/build/stage-sidestore"
rm -rf "$STAGE"
mkdir -p "$STAGE/Payload"
ditto "$APP" "$STAGE/Payload/IcarTPMS.app"
xattr -cr "$STAGE/Payload/IcarTPMS.app" 2>/dev/null || true
find "$STAGE/Payload/IcarTPMS.app" -name '_CodeSignature' -type d -exec rm -rf {} + 2>/dev/null || true

if [[ -d "$STAGE/Payload/IcarTPMS.app/PlugIns" ]]; then
  find "$STAGE/Payload/IcarTPMS.app/PlugIns" -name '*.appex' -print0 | while IFS= read -r -d '' appex; do
    codesign --force -s - --timestamp=none "$appex"
  done
fi
codesign --force -s - --timestamp=none --entitlements "$ROOT/IcarTPMS/IcarTPMS.entitlements" \
  "$STAGE/Payload/IcarTPMS.app"

rm -f "$ROOT/build/IcarTPMS-sidestore.ipa"
(cd "$STAGE" && zip -qr "$ROOT/build/IcarTPMS-sidestore.ipa" Payload)

echo "Wrote $ROOT/build/IcarTPMS-sidestore.ipa"
unzip -l "$ROOT/build/IcarTPMS-sidestore.ipa" | head -25
LIST=$(unzip -Z1 "$ROOT/build/IcarTPMS-sidestore.ipa")
echo "$LIST" | grep -F 'IcarTPMSLiveActivity.appex/IcarTPMSLiveActivity' >/dev/null \
  || { echo "ERROR: Live Activity appex missing from IPA" >&2; exit 1; }
echo "OK: Live Activity included"
