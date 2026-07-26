#include "PatcherBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QCoreApplication>
#include <QMessageBox>

#include "Logger.h"

static const QString k_acExe = "EAAntiCheat.GameServiceLauncher.exe";
static const QString k_acExeBak = "EAAntiCheat.GameServiceLauncher.exe.bak";
static const QString k_patcherBin = "patcher.bin";

// Helper: detect launcher + game exe (shared by apply/revert/isPatched)
static bool detectExes(const QDir& d,
    QFileInfo& outBiggest,
    QFileInfo& outLauncher)
{
    QFileInfoList exeList = d.entryInfoList({ "*.exe" }, QDir::Files);
    qint64 biggestSize = -1;
    for (const QFileInfo& fi : exeList)
    {
        QString nl = fi.fileName().toLower();
        if (nl.compare(k_acExeBak.toLower()) == 0) continue;
        if (nl.contains("trial")) continue;
        bool isLauncher = nl.contains("_launcher") ||
            nl.contains("gameservicelauncher");
        if (isLauncher)
        {
            if (outLauncher.filePath().isEmpty() || fi.size() > outLauncher.size())
                outLauncher = fi;
            continue;
        }
        if (fi.size() > biggestSize)
        {
            biggestSize = fi.size();
            outBiggest = fi;
        }
    }
    return outBiggest.exists();
}

// isPatched
bool PatcherBridge::isPatched(const QString& gameDir)
{
#ifdef Q_OS_WIN
    QDir d(gameDir);
    if (!d.exists()) return false;

    // Patcher.bin method: .bak exists AND the real launcher name holds patcher bytes
    if (QFile::exists(d.filePath(k_acExeBak)))
        return true;

    // Swap method: launcher exe is larger than game exe (they were swapped)
    QFileInfo biggest, launcher;
    if (!detectExes(d, biggest, launcher)) return false;
    if (launcher.exists() && biggest.exists() &&
        launcher.size() > biggest.size())
        return true;

    return false;
#else
    Q_UNUSED(gameDir)
        return false;
#endif
}

// Revert
bool PatcherBridge::revert(const QString& gameDir)
{
#ifdef Q_OS_WIN
    QDir d(gameDir);
    if (!d.exists())
    {
        Logger::log("[Patcher] Revert: directory does not exist: %s",
            gameDir.toUtf8().constData());
        return false;
    }

    QString exeDir = QCoreApplication::applicationDirPath();

    QFileInfo biggest, launcher;
    detectExes(d, biggest, launcher);

    // Revert patcher.bin method
    QString acPath = d.filePath(k_acExe);
    QString acBakPath = d.filePath(k_acExeBak);

    if (QFile::exists(acBakPath))
    {
        // Remove the patched launcher that patcher.bin wrote
        if (QFile::exists(acPath))
        {
            if (!QFile::remove(acPath))
            {
                Logger::log("[Patcher] Revert: failed to remove patched launcher.");
                return false;
            }
        }
        // Restore the original from .bak
        if (!QFile::rename(acBakPath, acPath))
        {
            Logger::log("[Patcher] Revert: failed to restore launcher from .bak");
            return false;
        }
        Logger::log("[Patcher] Revert: restored EAAntiCheat.GameServiceLauncher.exe from .bak");

        // Remove DLLs we may have placed in the game directory
        for (const QString& dll : { "dinput8.dll", "bcrypt.dll", "dxgi.dll" })
        {
            QString dllPath = d.filePath(dll);
            if (QFile::exists(dllPath))
            {
                QFile::remove(dllPath);
                Logger::log("[Patcher] Revert: removed %s", dll.toUtf8().constData());
            }
        }
        return true;
    }

    // Revert swap method
    if (launcher.exists() && biggest.exists() &&
        launcher.size() > biggest.size())
    {
        QString launcherPath = launcher.filePath();
        QString gamePath = biggest.filePath();
        QString tempPath = d.filePath(biggest.fileName() + ".swaptmp");

        if (!QFile::rename(launcherPath, tempPath))
        {
            Logger::log("[Patcher] Revert swap: failed to move launcher to temp.");
            return false;
        }
        if (!QFile::rename(gamePath, launcherPath))
        {
            QFile::rename(tempPath, launcherPath);
            Logger::log("[Patcher] Revert swap: failed to rename game exe to launcher.");
            return false;
        }
        if (!QFile::rename(tempPath, gamePath))
        {
            Logger::log("[Patcher] Revert swap: failed to restore game exe.");
            return false;
        }
        Logger::log("[Patcher] Revert: swap restored — %s <-> %s",
            biggest.fileName().toUtf8().constData(),
            launcher.fileName().toUtf8().constData());

        // Remove DLLs
        for (const QString& dll : { "dinput8.dll", "bcrypt.dll", "dxgi.dll" })
        {
            QString dllPath = d.filePath(dll);
            if (QFile::exists(dllPath))
            {
                QFile::remove(dllPath);
                Logger::log("[Patcher] Revert: removed %s", dll.toUtf8().constData());
            }
        }
        return true;
    }

    Logger::log("[Patcher] Revert: nothing to revert in %s",
        gameDir.toUtf8().constData());
    return false;
#else
    Q_UNUSED(gameDir)
        return false;
#endif
}

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

    // 1. Detect the launcher exe and the game exe separately
    QFileInfo biggestExe, launcherExe;
    if (!detectExes(d, biggestExe, launcherExe))
    {
        Logger::log("[Patcher] Could not detect game exe in: %s",
            gameDir.toUtf8().constData());
        return false;
    }

    qint64 biggestSize = biggestExe.size();
    QString gameExeName = biggestExe.fileName();
    Logger::log("[Patcher] Detected game exe: %s", gameExeName.toUtf8().constData());

    if (launcherExe.exists())
        Logger::log("[Patcher] Detected launcher exe: %s",
            launcherExe.fileName().toUtf8().constData());

    // 2. Decide patch method
    // FC exes always use the patcher.bin method
    // For other games: if an Engine.BuildInfo DLL exists next to the exe,
    // use the name-swap method; otherwise fall back to patcher.bin
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

        if (!launcherExe.exists())
        {
            Logger::log("[Patcher] Swap method: no launcher exe found in directory, cannot swap.");
            return false;
        }

        QString launcherPath = launcherExe.filePath();
        QString launcherName = launcherExe.fileName();
        QString gamePath = biggestExe.filePath();
        QString tempPath = d.filePath(gameExeName + ".swaptmp");

        // If the launcher exe is already larger than the game exe,
        // the swap was already applied — nothing to do
        if (launcherExe.size() > biggestSize)
        {
            Logger::log("[Patcher] Swap already applied (%s is larger than game exe), skipping.",
                launcherName.toUtf8().constData());
            return true;
        }

        // Three-step atomic rename: launcher -> temp, game -> launcher, temp -> game
        if (!QFile::rename(launcherPath, tempPath))
        {
            Logger::log("[Patcher] Swap: failed to move launcher exe to temp.");
            return false;
        }
        if (!QFile::rename(gamePath, launcherPath))
        {
            QFile::rename(tempPath, launcherPath); // roll back
            Logger::log("[Patcher] Swap: failed to rename game exe to launcher name.");
            return false;
        }
        if (!QFile::rename(tempPath, gamePath))
        {
            Logger::log("[Patcher] Swap: failed to move launcher exe to game exe name.");
            return false;
        }

        Logger::log("[Patcher] Swap method applied: %s <-> %s",
            gameExeName.toUtf8().constData(),
            launcherName.toUtf8().constData());
        return true;
    }

    // 3. Patcher.bin method
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

    // 4. Patch patcher.bin
    QString patcherSrc = exeDir + "/" + k_patcherBin;
    if (!QFile::exists(patcherSrc))
    {
        Logger::log("[Patcher] patcher.bin not found next to executable, skipping.");
        return false;
    }

    QFile patcherFile(patcherSrc);
    if (!patcherFile.open(QIODevice::ReadOnly))
    {
        Logger::log("[Patcher] Failed to open patcher.bin for reading.");
        return false;
    }
    QByteArray patcherData = patcherFile.readAll();
    patcherFile.close();

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

    return true;
#else
    Q_UNUSED(gameDir)
    return false;
#endif
}