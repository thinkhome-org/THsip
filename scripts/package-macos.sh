#!/bin/sh
set -eu

build_dir=${1:-build/release}
package_dir=${2:-build/package}
app="$package_dir/thsip.app"
archive="$package_dir/THsip.zip"

cmake -E rm -rf "$app"
cmake --install "$build_dir" --prefix "$package_dir"
test -d "$app"

qt_lib=$(qtpaths --query QT_INSTALL_LIBS)
for framework in QtSvg QtVirtualKeyboard QtVirtualKeyboardQml; do
  if [ -d "$qt_lib/$framework.framework" ]; then
    ditto "$qt_lib/$framework.framework" "$app/Contents/Frameworks/$framework.framework"
  fi
done

sqlcipher_prefix=$(brew --prefix sqlcipher)
openssl_prefix=$(brew --prefix openssl@4)
qsqlite="$app/Contents/PlugIns/sqldrivers/libqsqlite.dylib"
ditto "$sqlcipher_prefix/lib/libsqlcipher.dylib" "$app/Contents/Frameworks/libsqlcipher.dylib"
ditto "$openssl_prefix/lib/libcrypto.4.dylib" "$app/Contents/Frameworks/libcrypto.4.dylib"
install_name_tool -change /usr/lib/libsqlite3.dylib @rpath/libsqlcipher.dylib "$qsqlite"
install_name_tool -id @rpath/libsqlcipher.dylib "$app/Contents/Frameworks/libsqlcipher.dylib"
install_name_tool -change "$openssl_prefix/lib/libcrypto.4.dylib" @rpath/libcrypto.4.dylib "$app/Contents/Frameworks/libsqlcipher.dylib"
install_name_tool -id @rpath/libcrypto.4.dylib "$app/Contents/Frameworks/libcrypto.4.dylib"

if [ -n "${THSIP_SIGNING_IDENTITY:-}" ]; then
  codesign --force --deep --options runtime --entitlements macos/THsip.entitlements --sign "$THSIP_SIGNING_IDENTITY" "$app"
else
  codesign --force --deep --sign - "$app"
fi
codesign --verify --deep --strict --verbose=2 "$app"
otool -L "$qsqlite" | grep -q '@rpath/libsqlcipher.dylib'
self_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/thsip-sqlcipher.XXXXXX")
sqlite3 "$self_test_dir/thsip.sqlite" 'CREATE TABLE migration_probe(value TEXT); INSERT INTO migration_probe VALUES ("ok");'
THSIP_DATABASE_TEST_KEY=0000000000000000000000000000000000000000000000000000000000000000 THSIP_DATA_DIR="$self_test_dir" "$app/Contents/MacOS/thsip" --database-migration-self-test
THSIP_DATABASE_TEST_KEY=0000000000000000000000000000000000000000000000000000000000000000 THSIP_DATA_DIR="$self_test_dir" "$app/Contents/MacOS/thsip" --telephony-self-test

cmake -E rm -f "$archive"
ditto -c -k --keepParent "$app" "$archive"

if [ -n "${THSIP_NOTARY_PROFILE:-}" ]; then
  xcrun notarytool submit "$archive" --keychain-profile "$THSIP_NOTARY_PROFILE" --wait
  xcrun stapler staple "$app"
fi

printf '%s\n' "$archive"
