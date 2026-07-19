#include "PatcherBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QCoreApplication>
#include <QMessageBox>

#include "Logger.h"

static const QString k_acExe     = "EAAntiCheat.GameServiceLauncher.exe";
static const QString k_acExeBak  = "EAAntiCheat.GameServiceLauncher.exe.bak";
static const QString k_patcherBin = "patcher.bin";
static const QString k_winhttpDll = "winhttp.dll";

bool PatcherBridge::apply(const QString& gameDir)
{
#ifdef Q_OS_WIN
    QDir d(gameDir);
    if (!d.exists())
    {
        Logger::log("[Patcher] Game directory does not exist: %s",
                    gameDir.toUtf8().constData());
        return false;
    }

    QString exeDir = QCoreApplication::applicationDirPath();

    // ── 1. Detect the largest .exe in the directory (the game exe) ───────────
    QFileInfoList exeList = d.entryInfoList({ "*.exe" }, QDir::Files);
    QFileInfo biggestExe;
    qint64 biggestSize = -1;
    for (const QFileInfo& fi : exeList)
    {
        if (fi.fileName().compare(k_acExe, Qt::CaseInsensitive) == 0) continue;
        if (fi.fileName().compare(k_acExeBak, Qt::CaseInsensitive) == 0) continue;
        if (fi.fileName().toLower().contains("trial")) continue;
        if (fi.size() > biggestSize)
        {
            biggestSize = fi.size();
            biggestExe = fi;
        }
    }

    if (!biggestExe.exists())
    {
        Logger::log("[Patcher] Could not detect game exe in: %s",
            gameDir.toUtf8().constData());
        return false;
    }

    QString gameExeName = biggestExe.fileName();
    Logger::log("[Patcher] Detected game exe: %s", gameExeName.toUtf8().constData());

    // ── 2. Decide patch method ────────────────────────────────────────────────
    // FC/FIFA exes always use the classic patcher.bin method.
    // For other games: if an Engine.BuildInfo DLL exists next to the exe,
    // use the name-swap method; otherwise fall back to patcher.bin.
    QString exeLower = gameExeName.toLower();
    bool isFCFIFA = (exeLower == "fc26.exe" ||
        exeLower == "fc25.exe" ||
        exeLower == "fc24.exe" ||
        exeLower == "fifa23.exe");

    bool hasBuildInfoDll = false;
    if (!isFCFIFA)
    {
        QFileInfoList dllList = d.entryInfoList({ "*.dll" }, QDir::Files);
        for (const QFileInfo& fi : dllList)
        {
            if (fi.fileName().contains("Engine.BuildInfo", Qt::CaseInsensitive))
            {
                hasBuildInfoDll = true;
                Logger::log("[Patcher] Found Engine.BuildInfo DLL: %s — using swap method.",
                    fi.fileName().toUtf8().constData());
                break;
            }
        }
    }

    if (!isFCFIFA && hasBuildInfoDll)
    {
        // ── Swap method ───────────────────────────────────────────────────────
        // Rename: GameExe.exe -> EAAntiCheat.GameServiceLauncher.exe
        //         EAAntiCheat.GameServiceLauncher.exe -> GameExe.exe
        QString acPath = d.filePath(k_acExe);
        QString gamePath = biggestExe.filePath();
        QString tempPath = d.filePath(gameExeName + ".swaптmp");

        if (!QFile::exists(acPath))
        {
            Logger::log("[Patcher] EAAntiCheat.GameServiceLauncher.exe not found, cannot swap.");
            return false;
        }

        // If the anticheat exe is already larger than the game exe, swap was already applied
        QFileInfo acInfo(acPath);
        if (acInfo.size() > biggestSize)
        {
            Logger::log("[Patcher] Swap already applied (EAAntiCheat exe is larger than game exe), skipping.");
            return true;
        }

        // Use a temp name to avoid collision
        if (!QFile::rename(acPath, tempPath))
        {
            Logger::log("[Patcher] Swap: failed to move EAAntiCheat exe to temp.");
            return false;
        }
        if (!QFile::rename(gamePath, acPath))
        {
            QFile::rename(tempPath, acPath); // roll back
            Logger::log("[Patcher] Swap: failed to rename game exe to EAAntiCheat name.");
            return false;
        }
        if (!QFile::rename(tempPath, gamePath))
        {
            Logger::log("[Patcher] Swap: failed to move EAAntiCheat exe to game exe name.");
            return false;
        }

        Logger::log("[Patcher] Swap method applied: %s <-> EAAntiCheat.GameServiceLauncher.exe",
            gameExeName.toUtf8().constData());
        return true;
    }

    // ── Classic patcher.bin method ────────────────────────────────────────────
    // ── 3. Find and back up EAAntiCheat.GameServiceLauncher.exe ─────────────
    QString acPath = d.filePath(k_acExe);
    QString acBakPath = d.filePath(k_acExeBak);

    if (!QFile::exists(acPath))
    {
        Logger::log("[Patcher] EAAntiCheat.GameServiceLauncher.exe not found in: %s",
            gameDir.toUtf8().constData());
        return false;
    }

    if (!QFile::exists(acBakPath))
    {
        if (!QFile::rename(acPath, acBakPath))
        {
            Logger::log("[Patcher] Failed to rename EAAntiCheat.GameServiceLauncher.exe to .bak");
            return false;
        }
        Logger::log("[Patcher] Backed up EAAntiCheat.GameServiceLauncher.exe -> .bak");
    }
    else
    {
        if (QFile::exists(acPath))
            QFile::remove(acPath);
        Logger::log("[Patcher] .bak already exists, skipping rename.");
    }

    // ── 4. Patch patcher.bin — write game exe name into the magic buffer ─────
    QString patcherSrc = exeDir + "/" + k_patcherBin;
    if (!QFile::exists(patcherSrc))
    {
        Logger::log("[Patcher] patcher.bin not found next to executable, skipping.");
        return false;
    }

    // Read patcher.bin
    QFile patcherFile(patcherSrc);
    if (!patcherFile.open(QIODevice::ReadOnly))
    {
        Logger::log("[Patcher] Failed to open patcher.bin for reading.");
        return false;
    }
    QByteArray patcherData = patcherFile.readAll();
    patcherFile.close();

    // Search for MAGIC: "PATCHME:" as UTF-16LE (16 bytes)
    const char magic[] = {
        'P',0,'A',0,'T',0,'C',0,'H',0,'M',0,'E',0,':',0
    };
    int magicLen = 16;
    int magicPos = -1;
    for (int i = 0; i <= patcherData.size() - magicLen; i++)
    {
        if (memcmp(patcherData.constData() + i, magic, magicLen) == 0)
        {
            magicPos = i;
            break;
        }
    }

    if (magicPos < 0)
    {
        Logger::log("[Patcher] PATCHME magic not found in patcher.bin.");
        return false;
    }

    // Overwrite the 252 wchars after the magic with the game exe name
    int nameOffset = magicPos + magicLen;
    int nameFieldBytes = 252 * 2; // 252 wchar_t
    QByteArray nameField(nameFieldBytes, '\0');

    // CRT-safe: convert via UTF-16 QByteArray, never toStdWString() across boundary
    QByteArray nameUtf16 = gameExeName.toUtf8(); // get raw bytes first
    QString nameQStr = QString::fromUtf8(nameUtf16.constData(), nameUtf16.size());
    QByteArray nameRaw = QByteArray(reinterpret_cast<const char*>(nameQStr.utf16()),
        nameQStr.size() * 2);
    int copyBytes = qMin(nameRaw.size(), nameFieldBytes - 2); // leave room for null terminator
    memcpy(nameField.data(), nameRaw.constData(), copyBytes);
    // null terminator is already zeroed by QByteArray construction
    patcherData.replace(nameOffset, nameFieldBytes, nameField);

    // Write patched binary to game directory as EAAntiCheat.GameServiceLauncher.exe
    QString patcherDst = d.filePath(k_acExe);
    QFile outFile(patcherDst);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        Logger::log("[Patcher] Failed to write patched exe to game directory.");
        return false;
    }
    outFile.write(patcherData);
    outFile.close();
    Logger::log("[Patcher] Deployed patcher as EAAntiCheat.GameServiceLauncher.exe");

    // ── 5. Copy winhttp.dll and matching FC DLL if this is a known game exe ──
    {
        QString exeLower = gameExeName.toLower();
        QString fcDllName;
        if (exeLower == "fc26.exe")   fcDllName = "FC26.dll";
        else if (exeLower == "fc25.exe")   fcDllName = "FC25.dll";
        else if (exeLower == "fifa23.exe") fcDllName = "FIFA23.dll";

        if (fcDllName.isEmpty())
        {
            Logger::log("[Patcher] Skipping winhttp.dll - not a FC game.");
        }
        else
        {
            QString winhttpSrc = exeDir + "/" + k_winhttpDll;
            QString winhttpDst = d.filePath(k_winhttpDll);
            if (!QFile::exists(winhttpSrc))
            {
                Logger::log("[Patcher] winhttp.dll not found next to executable, skipping.");
            }
            else if (QFile::exists(winhttpDst))
            {
                Logger::log("[Patcher] winhttp.dll already present in game directory, skipping.");
            }
            else
            {
                QFile::copy(winhttpSrc, winhttpDst);
                Logger::log("[Patcher] Copied winhttp.dll to game directory.");
            }

            QString fcDllSrc = exeDir + "/FC/" + fcDllName;
            QString fcDllDst = d.filePath(fcDllName);
            if (!QFile::exists(fcDllSrc))
            {
                Logger::log("[Patcher] %s not found next to executable, skipping.",
                    fcDllName.toUtf8().constData());
            }
            else if (QFile::exists(fcDllDst))
            {
                Logger::log("[Patcher] %s already present in game directory, skipping.",
                    fcDllName.toUtf8().constData());
            }
            else
            {
                QFile::copy(fcDllSrc, fcDllDst);
                Logger::log("[Patcher] Copied %s to game directory.",
                    fcDllName.toUtf8().constData());
            }
        }
    }
    return true;
#else
    Q_UNUSED(gameDir)
    return false;
#endif
}