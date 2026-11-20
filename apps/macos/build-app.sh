#!/bin/zsh
set -eu
root="${0:A:h:h:h}"
configuration="${VESPERA_CONFIGURATION:-debug}"
cmake --build "$root/build/$configuration" --target vespera_app_audio
swift build --package-path "$root/apps/macos" -c "$configuration"
bin="$(swift build --package-path "$root/apps/macos" -c "$configuration" --show-bin-path)"
staging="$(mktemp -d "$root/build/vespera-app.XXXXXX")"
trap 'rm -rf "$staging"' EXIT
app="$staging/vespera.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin/vespera" "$app/Contents/MacOS/"
cp "$root/apps/macos/Info.plist" "$app/Contents/Info.plist"
if [[ -n "${VESPERA_VERSION:-}" ]]; then
  /usr/libexec/PlistBuddy -c "Add :CFBundleShortVersionString string $VESPERA_VERSION" "$app/Contents/Info.plist"
fi
cp "$root/build/$configuration/libvespera_app_audio.dylib" "$app/Contents/MacOS/"
ditto "$bin/VesperaUI_VesperaUI.bundle" "$app/Contents/Resources/VesperaUI_VesperaUI.bundle"
codesign --force --deep --sign - "$app"
rm -rf "$root/build/vespera.app"
mv "$app" "$root/build/vespera.app"
echo "$root/build/vespera.app"
