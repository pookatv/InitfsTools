# Frostbite InitFS Tools
The ultimate toolsuite for all things InitFS related, 
featuring multi-format decryption support, a command dictionary, type extractor, diff checker, and more.

Massive thank you to the original FrostyToolsuite team, you can check them here: https://github.com/CadeEvs/FrostyToolsuite

## Features
- **InitFS Modding** — Load, modify, and save InitFS files across a wide range of Frostbite Engine games
- **Diff Check** — Compare differences between two InitFS files, with export support
- **Type Extractor** — Extract all types and commands from a game executable or FrostyEditor SDK DLL
- **Command Dictionary** — Generate and browse a full list of console commands extracted from raw InitFS files
- **Reference Library** — Browse and view base and custom payloads from various Frostbite titles
- **Preset Manager** — Browse and insert user-saved presets containing sets of various commands


## License
The Content, Name, Code, and all assets are licensed under a Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.

## Building from Source

### Requirements
- Visual Studio 2022 (MSVC)
- CMake 3.20+
- Qt 6.10 Win7 Backport (pre-built, see below)
- QScintilla 2.14.1
- OpenSSL 3.x

### Quick Start (Debug)

1. Download the pre-built Qt 6.10 Win7 backport:
   [https://github.com/qr243vbi/qt6windows7/releases](https://github.com/qr243vbi/qt6windows7/releases/tag/v6.10.0_x86_64)
   Download Qt_6.10.0_x86_64.zip and extract it anywhere, e.g. C:\Qt_6.10_Backport

2. Build QScintilla 2.14.1 against it:
   - Download QScintilla 2.14.1 source from https://riverbankcomputing.com/software/qscintilla
   - Open x64 Native Tools Command Prompt for VS 2022
   - cd <qscintilla_source>\src
   - <your_backport_path>\bin\qmake.exe qscintilla.pro
   - nmake release
   - Copy release\qscintilla2_qt6.lib into <your_backport_path>\lib
   - Copy release\qscintilla2_qt6.dll into <your_backport_path>\bin

3. Install OpenSSL 3.x:
   https://slproweb.com/products/Win32OpenSSL.html
   (Install it to the default location)

4. Edit CMakeUserPresets.json:
   Set QT_PATH in Qt-Default-Dynamic to your backport extraction path

5. Open the project in Visual Studio 2022, select Qt-Debug preset, build.

### Release Build (Static, single exe)
See BUILD_STATIC.md for instructions on producing a fully static executable.
