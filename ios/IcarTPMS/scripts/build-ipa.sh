#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
xcodegen generate
xcodebuild -project IcarTPMS.xcodeproj -scheme IcarTPMS -configuration Release \
  -destination 'generic/platform=iOS' \
  -archivePath build/IcarTPMS.xcarchive archive -allowProvisioningUpdates
xcodebuild -exportArchive -archivePath build/IcarTPMS.xcarchive \
  -exportPath build/export -exportOptionsPlist ExportOptions.plist -allowProvisioningUpdates
echo "IPA: $ROOT/build/export/IcarTPMS.ipa"