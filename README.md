# Frostbite InitFS Tools
The ultimate toolsuite for all things InitFS related, 
featuring multi-format decryption support, a command dictionary, type extractor, diff checker, and more.

Note: If an InitFS file asks you for an AES key, I will not provide it. A quick google search may help.

Massive thank you to the original FrostyToolsuite team, you can check them here: https://github.com/CadeEvs/FrostyToolsuite

## Features
- **InitFS Modding** — Load, modify, and save InitFS files across all Frostbite Engine games
- **Diff Check** — Compare differences between two InitFS files, with export support
- **Type Extractor** — Extract all types and commands from a game executable or FrostyEditor SDK DLL
- **Command Dictionary** — Generate and browse a full list of console commands extracted from raw InitFS files
- **Reference Library** — Browse and view base and custom payloads from various Frostbite titles
- **Preset Manager** — Browse and insert user-saved presets containing sets of useful commands

<img width="2100" height="1240" alt="image222" src="https://github.com/user-attachments/assets/b651aab0-42a0-40e6-8b9b-44f916c9d088" />

## TODO:
- Fix Dingo mode logic as it's incorrect and needs proper polishing (Type Extractor)
- Fix Squadrons using the wrong mode - should use Walrus (Type Extractor)
- Fix NFS Payback using the wrong mode - should use Walrus (Type Extractor)
- Fix Mirror's Edge using the wrong mode - should use Havana (Type Extractor)
- Fix Dragon Age Inquisition support - only works in 2.0.0-beta2 (Type Extractor)
- Implement Mass Effect Andromeda support (Type Extractor)
- Implement PGA Tour support (Type Extractor)
- Implement Battlefield Hardline support (Type Extractor)
- Fully implement Dead Space and Need For Speed Heat support for live value reading
- Fix a bug where the bcrypt.dll gets copied after the user hits ok, should be before
- Fix an issue where the green highlight may disappear when deleting spaces
- Fix an issue where highlights are invisible (Diff Check)
- Fix the program not fully closing when tool windows are active
- Finish DictionaryWindow logic to support more dev commands
- Finish InitfsTools Wiki (help wanted!)
- Implement localization support for the UI
- Implement "GoTo" default commands

## COMING SOON:
- Console Injector — Hooks into a game's console, unlocks all commands, and executes them remotely (confirmed working in: DelMar, MEA, BFH, BF4, SWBF2, SWBF2015, BFN, GW2, GW1, NFS Unbound, NFS Heat, NFS Payback, Need For Speed, NFS Rivals, Mirror's Edge, PGA Tour, DAV, DAI)

## License
The Content, Name, Code, and all assets are licensed under a Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.

# Building from Source

## Requirements
- Visual Studio 2022 with C++ workload
- CMake 3.20+
- Qt 6.10 Win7 Backport (pre-built, see below)
- QScintilla 2.14.1
- OpenSSL 3.x

## Quick Start (Debug)

### 1. Download Qt 6.10 Win7 Backport
Download the pre-built backport from https://github.com/qr243vbi/qt6windows7/releases

Download `Qt_6.10.0_x86_64.zip` and extract it anywhere (spaces in the path may cause issues with qmake on some systems), e.g. `C:\Qt_6.10_Backport`

### 2. Build QScintilla 2.14.1
Download QScintilla 2.14.1 source from https://riverbankcomputing.com/software/qscintilla

Open **x64 Native Tools Command Prompt for VS 2022** (required for all commands below), then run:

```
cd <qscintilla_source>\src
```
> This is the `src` subfolder inside the extracted QScintilla zip.

```
<your_backport_path>\bin\qmake.exe qscintilla.pro
nmake release
```

Then copy the output files:
- Copy `release\qscintilla2_qt6.lib` into `<your_backport_path>\lib`
- Copy `release\qscintilla2_qt6.dll` into `<your_backport_path>\bin`

### 3. Install OpenSSL 3.x
Download and install from https://slproweb.com/products/Win32OpenSSL.html

Install to the **default location**.

### 4. Configure CMakeUserPresets.json
Open `CMakeUserPresets.json` and replace all **3 instances** of `<YOUR_QT_BACKPORT_PATH>` with your backport extraction path. The three locations are:

- `QTDIR` under `Qt-Default-Dynamic`
- `QT_PATH` under `Qt-Default-Dynamic`
- `PATH` under `Qt-Debug` (the `/bin` entry)

For example, if you extracted to `C:/Qt_6.10_Backport`:

```json
"environment": {
    "QTDIR": "C:/Qt_6.10_Backport"
},
"cacheVariables": {
    "QT_PATH": "C:/Qt_6.10_Backport"
}
```

and:

```json
"PATH": "C:/Qt_6.10_Backport/bin;$penv{PATH}"
```

> Note: Use forward slashes `/` in the JSON file, not backslashes.

### 5. Build
Open the project in Visual Studio 2022, select the `Qt-Debug` preset, and build.

### Release Build (Static, single exe)
See BUILD_STATIC.md for instructions on producing a fully static executable.
