# Building a Static Release (Single EXE)

This document describes how to produce a fully static `InitfsTools.exe`
with no Qt DLL dependencies. This is only needed if you want to produce
a distributable release build. Regular contributors do not need to
follow these steps.

---

## Overview

The static build requires:
- Qt 6.10 compiled from source with `-static -static-runtime`
- The Win7 backport patch applied to the Qt sources before compiling
- QScintilla 2.14.1 compiled as a static lib against the static Qt
- OpenSSL 3.x static libs

Expected build time: 1-2 hours (Qt compile).
Required disk space: ~15 GB during build, ~5 GB for final installation.

---

## Prerequisites

- Visual Studio 2022 (MSVC)
- CMake 3.20+
- Ninja (included with Visual Studio)
- Perl (required by Qt build system) — https://strawberryperl.com
- Python 3.x (required by Qt build system) — https://python.org
- OpenSSL 3.x Win64 installed at `C:\Program Files\OpenSSL-Win64`
- Qt 6.10 installed via Qt Online Installer with:
  - MSVC 2022 64-bit kit
  - Sources

---

## Step 1 — Install Qt 6.10 with Sources

Run the Qt Online Installer from https://www.qt.io/download-qt-installer

In the component selection, install only:
- Qt 6.10.0 → MSVC 2022 64-bit
- Qt 6.10.0 → Sources

Everything else can be deselected. This keeps the install around 3-4 GB.

---

## Step 2 — Apply the Win7 Backport Patch

Download the backport repository from:
https://github.com/qr243vbi/qt6windows7

Extract it, then in a command prompt run:

```cmd
xcopy /s /y <extracted_folder>\qtbase C:\Qt\6.10.0\Src\qtbase
```

This overwrites 39 files in the Qt sources with Win7-compatible versions.

---

## Step 3 — Configure Qt for Static Build

Open **x64 Native Tools Command Prompt for VS 2022** as Administrator
and run:

```cmd
mkdir C:\Qt\6.10.0\static_build
cd C:\Qt\6.10.0\static_build

cmake -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DCMAKE_INSTALL_PREFIX="C:/Qt/6.10.0/msvc2022_64_static" ^
  -DQT_QMAKE_TARGET_MKSPEC=win32-msvc ^
  -DFEATURE_static_runtime=ON ^
  -DQT_BUILD_EXAMPLES=OFF ^
  -DQT_BUILD_TESTS=OFF ^
  -DBUILD_qtwebengine=OFF ^
  -DBUILD_qtmultimedia=OFF ^
  -DBUILD_qt3d=OFF ^
  -DBUILD_qtcharts=OFF ^
  -DBUILD_qtdatavis3d=OFF ^
  -DBUILD_qtconnectivity=OFF ^
  -DBUILD_qtspeech=OFF ^
  -DBUILD_qtopcua=OFF ^
  -DINPUT_openssl=no ^
  "C:/Qt/6.10.0/Src"
```

Configuration takes about 2 minutes. You will see warnings about missing
optional components (QDoc, WebView2, protobuf) — these are harmless and
can be ignored.

---

## Step 4 — Build Qt

```cmd
cmake --build . --parallel
```

This will take 1-2 hours depending on your CPU. All cores will be used.
The prompt will appear to hang — this is normal.

---

## Step 5 — Install Qt

```cmd
cmake --install .
```

Qt will be installed to `C:\Qt\6.10.0\msvc2022_64_static`.

Verify the install succeeded:
```cmd
dir C:\Qt\6.10.0\msvc2022_64_static\lib\Qt6Core.lib
```

You should see a file around 36 MB.

---

## Step 6 — Build QScintilla Statically

Still in the x64 Native Tools Command Prompt:

```cmd
cd <qscintilla_source>\src

"C:\Qt\6.10.0\msvc2022_64_static\bin\qmake.exe" qscintilla.pro ^
    "CONFIG+=staticlib" "CONFIG+=static"

nmake release
```

Then install it into the static Qt:

```cmd
copy release\qscintilla2_qt6.lib "C:\Qt\6.10.0\msvc2022_64_static\lib\"
xcopy /s /y Qsci "C:\Qt\6.10.0\msvc2022_64_static\include\Qsci\"
```

---

## Step 7 — Build InitfsTools

In Visual Studio 2022, select the **Qt-Release** preset and build.

The output will be in `out/build/release/`. The result is a single
`InitfsTools.exe` plus `bcrypt.dll` with no other dependencies.

---

## Troubleshooting

**"compiler is out of heap space"**
You are using the 32-bit hosted compiler. Make sure you opened the
**x64 Native Tools** Command Prompt, not the regular Developer Command
Prompt.

**OpenSSL errors during Qt configure**
The configure command above uses `-DINPUT_openssl=no` which skips
OpenSSL inside Qt entirely. Your app links OpenSSL directly and does
not need Qt's TLS backend.

**Link errors about missing Qt plugins**
The `CMakeLists.txt` imports the required static plugins automatically
when `QT_STATIC=ON`. If you add new UI features that require additional
plugins, add them to the `qt_import_plugins` call in `CMakeLists.txt`.