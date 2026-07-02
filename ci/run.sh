#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${SRC_DIR:-/src}"
QTDIR="${QTDIR:-/opt/qt-windows/6.10.0/msvc2022_64}"
WINSDK="${WINSDK:-/opt/msvc}"
QT_BASE="$(dirname "$(dirname "$QTDIR")")"

export WINDOWS_SDK_PATH="$WINSDK"

WINE=$(command -v wine64 || command -v wine)
$WINE wineboot

echo "=== Windows SDK (msvc-wine) ==="
git clone --depth=1 https://github.com/mstorsjo/msvc-wine /tmp/msvc-wine
python /tmp/msvc-wine/vsdownload.py --accept-license --dest "$WINSDK"
bash /tmp/msvc-wine/install.sh "$WINSDK"

echo "=== Qt for Windows ==="
aqt install-qt windows desktop 6.10.0 win64_msvc2022_64 -O "$QT_BASE"
# Wrap Qt .exe tools with wine scripts (AUTOMOC bypasses CMAKE_CROSSCOMPILING_EMULATOR)
for f in "$QTDIR/bin/"*.exe; do
    if [ -f "$f" ] && head -c 2 "$f" | od -An -tx1 | tr -d ' ' | grep -q '^4d5a$'; then
        mv "$f" "${f}.bin"
        { echo '#!/bin/bash'; echo "exec /usr/bin/wine '${f}.bin' \"\$@\""; } > "$f"
        chmod +x "$f"
    fi
done

echo "=== OpenSSL 3.5.7 ==="
curl -fsSL -o /tmp/openssl.zip \
    https://download.firedaemon.com/FireDaemon-OpenSSL/openssl-3.5.7.zip
unzip -qo /tmp/openssl.zip -d /tmp/openssl-extract
cp /tmp/openssl-extract/x64/bin/libcrypto-3-x64.dll "$QTDIR/bin/"
cp /tmp/openssl-extract/x64/bin/libssl-3-x64.dll    "$QTDIR/bin/"
cp /tmp/openssl-extract/x64/lib/libcrypto.lib       "$QTDIR/lib/"
cp /tmp/openssl-extract/x64/lib/libssl.lib          "$QTDIR/lib/"
cp -r /tmp/openssl-extract/x64/include/openssl      "$QTDIR/include/"

echo "=== QScintilla 2.14.1 (static) ==="
curl -fsSL -o /tmp/qscintilla.tar.gz \
    https://www.riverbankcomputing.com/static/Downloads/QScintilla/2.14.1/QScintilla_src-2.14.1.tar.gz
tar xzf /tmp/qscintilla.tar.gz -C /tmp
cp "$SRC_DIR/ci/qscintilla/CMakeLists.txt" /tmp/QScintilla_src-2.14.1/
cmake -B /tmp/QScintilla_src-2.14.1/build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$SRC_DIR/toolchain-linux-winsdk.cmake" \
    -DCMAKE_PREFIX_PATH="$QTDIR" \
    -DCMAKE_BUILD_TYPE=Release \
    /tmp/QScintilla_src-2.14.1
cmake --build /tmp/QScintilla_src-2.14.1/build -j"$(nproc)"
cp /tmp/QScintilla_src-2.14.1/build/qscintilla2_qt6.lib "$QTDIR/lib/"
cp -r /tmp/QScintilla_src-2.14.1/src/Qsci "$QTDIR/include/Qsci"

echo "=== Build InitfsTools ==="
cmake -B "$SRC_DIR/out/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC_DIR/toolchain-linux-winsdk.cmake" \
    -DQT_PATH="$QTDIR" \
    "$SRC_DIR"
cmake --build "$SRC_DIR/out/build" -j"$(nproc)"

echo "=== Output at $SRC_DIR/out/build/dist ==="
