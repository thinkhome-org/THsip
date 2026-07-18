#!/bin/sh
set -eu

build_dir=${1:-build/release}
package_dir=${2:-build/package}
app="$package_dir/thsip.app"

cmake --install "$build_dir" --prefix "$package_dir"
test -d "$app"

qt_lib=$(qtpaths --query QT_INSTALL_LIBS)
for framework in QtSvg QtVirtualKeyboard QtVirtualKeyboardQml; do
  if [ -d "$qt_lib/$framework.framework" ]; then
    ditto "$qt_lib/$framework.framework" "$app/Contents/Frameworks/$framework.framework"
  fi
done

if [ -n "${THSIP_SIGNING_IDENTITY:-}" ]; then
  codesign --force --deep --options runtime --entitlements macos/THsip.entitlements --sign "$THSIP_SIGNING_IDENTITY" "$app"
else
  codesign --force --deep --sign - "$app"
fi
codesign --verify --deep --strict --verbose=2 "$app"

archive="$package_dir/THsip.zip"
ditto -c -k --keepParent "$app" "$archive"

if [ -n "${THSIP_NOTARY_PROFILE:-}" ]; then
  xcrun notarytool submit "$archive" --keychain-profile "$THSIP_NOTARY_PROFILE" --wait
  xcrun stapler staple "$app"
fi

printf '%s\n' "$archive"
