# Cross-Compiling for Windows on Linux

> **Note:** Replace `$HOME/Projects/InitfsTools` with your actual path if the source is cloned elsewhere.

## Container Build (Recommended)

A `Containerfile` and build script are provided in `ci/` to automate the entire
cross-compilation environment.

### Build

```bash
cd /home/twig/Projects/InitfsTools

# 1. Build the container image (one-time setup)
podman build -t initfstools-ci -f ci/Containerfile ci/

# 2. Run the full cross-compile inside the container
podman run --rm -it \
  --volume "$PWD":/src:Z \
  initfstools-ci \
  bash /src/ci/run.sh
```

Output is at `out/build/dist/` on the host.

## Without container

### Prerequisites

```bash
pacman -S clang lld llvm cmake ninja python python-pip wine git unzip curl
python -m venv venv
source ./venv/bin/activate
pip install aqtinstall
```

### 1. Windows SDK

```bash
git clone https://github.com/mstorsjo/msvc-wine /tmp/msvc-wine
python /tmp/msvc-wine/vsdownload.py --accept-license --dest /opt/msvc
bash /tmp/msvc-wine/install.sh /opt/msvc
export WINDOWS_SDK_PATH=/opt/msvc
```

### 2. Qt for Windows

```bash
export QTDIR=/opt/qt-windows/6.10.0/msvc2022_64
aqt install-qt windows desktop 6.10.0 win64_msvc2022_64 -O "$(dirname "$(dirname "$QTDIR")")"
```

### 3. OpenSSL 3.5.7

```bash
curl -L -o /tmp/openssl.zip \
  https://download.firedaemon.com/FireDaemon-OpenSSL/openssl-3.5.7.zip
unzip /tmp/openssl.zip -d /tmp/openssl-extract
cp /tmp/openssl-extract/x64/bin/libcrypto-3-x64.dll "$QTDIR/bin/"
cp /tmp/openssl-extract/x64/bin/libssl-3-x64.dll    "$QTDIR/bin/"
cp /tmp/openssl-extract/x64/lib/libcrypto.lib       "$QTDIR/lib/"
cp /tmp/openssl-extract/x64/lib/libssl.lib          "$QTDIR/lib/"
cp -r /tmp/openssl-extract/x64/include/openssl      "$QTDIR/include/"
```

### 4. QScintilla 2.14.1 (static)

```bash
curl -L -o /tmp/qscintilla.tar.gz \
  https://www.riverbankcomputing.com/static/Downloads/QScintilla/2.14.1/QScintilla_src-2.14.1.tar.gz
tar xzf /tmp/qscintilla.tar.gz -C /tmp
cp $HOME/Projects/InitfsTools/ci/qscintilla/CMakeLists.txt /tmp/QScintilla_src-2.14.1/
cmake -B /tmp/QScintilla_src-2.14.1/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/home/twig/Projects/InitfsTools/toolchain-linux-winsdk.cmake \
  -DCMAKE_PREFIX_PATH="$QTDIR" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/QScintilla_src-2.14.1/build -j"$(nproc)"
cp /tmp/QScintilla_src-2.14.1/build/qscintilla2_qt6.lib "$QTDIR/lib/"
cp -r /tmp/QScintilla_src-2.14.1/src/Qsci "$QTDIR/include/Qsci"
```

### 5. Build

Use CMake presets:

```bash
cd /home/twig/Projects/InitfsTools

# Release (static, single exe)
cmake --preset Cross-Release
cmake --build out/build/cross-release -j"$(nproc)"

# Debug (DLL-based)
cmake --preset Cross-Debug
cmake --build out/build/cross-debug -j"$(nproc)"
```

Or manually:

```bash
cmake -B out/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-linux-winsdk.cmake \
  -DQT_PATH="$QTDIR"
cmake --build out/build -j"$(nproc)"
```

Output for Release is at `out/build/cross-release/dist/` (preset) or `out/build/dist/` (manual).

## CI

The CI pipeline automates the container build. See `.github/workflows/build_linux.yml`.
