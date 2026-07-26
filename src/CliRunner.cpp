#include "CliRunner.h"
#include "Converter.h"
#include "Logger.h"
#include "DbManifestReconstructor.h"
#include "PatcherBridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QProcess>
#include <QThread>

#include <iostream>
#include <algorithm>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace
{
    const char kHexChars[] = "0123456789ABCDEF";

    // QThread::msleep is protected — this exposes it as a static helper.
    class CliSleeper : public QThread
    {
    public:
        using QThread::msleep;
    };

#ifdef Q_OS_WIN
    bool isProcessRunning(qint64 pid)
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
            static_cast<DWORD>(pid));
        if (!h) return false; // couldn't open it -> treat as gone

        DWORD exitCode = 0;
        bool running = GetExitCodeProcess(h, &exitCode) && exitCode == STILL_ACTIVE;
        CloseHandle(h);
        return running;
    }

    // Finds any currently-running process matching an exe filename (case-insensitive).
    // Used to detect a launcher (e.g. the EA App) handing off to the real game
    // process under a new PID after the original launch handle has exited.
    qint64 findRunningProcessByExeName(const QString& exeName)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        qint64 found = 0;
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                QString name = QString::fromWCharArray(pe.szExeFile);
                if (name.compare(exeName, Qt::CaseInsensitive) == 0)
                {
                    found = static_cast<qint64>(pe.th32ProcessID);
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return found;
    }
#else
    bool isProcessRunning(qint64 pid)
    {
        // Detached processes aren't our children (double-forked), so we can't
        // waitpid() on them — poll for existence via signal 0 instead.
        return kill(static_cast<pid_t>(pid), 0) == 0;
    }
#endif
}

// ============================================================================
// Entry points
// ============================================================================

bool CliRunner::isCliInvocation(int argc, char* argv[])
{
    if (argc < 2) return false;
    QString first = QString::fromLocal8Bit(argv[1]).toLower();
    return first == "-r" || first == "-run"
        || first == "-h" || first == "-help" || first == "--help";
}

void CliRunner::printUsage()
{
    std::cout <<
        "InitfsTools CLI usage:\n"
        "  InitfsTools.exe -r | -run <initfs_path> -pl | -payloadlist\n"
        "      List every payload name in the initfs file.\n"
        "\n"
        "  InitfsTools.exe -r | -run <initfs_path> -pc | -payloadcontents <payload_name>\n"
        "      Print the full contents of one payload.\n"
        "\n"
        "  InitfsTools.exe -r | -run <initfs_path> -pa | -permanentadd <payload_name> <line> [more lines...]\n"
        "      Permanently append an applySettings block to a payload and save.\n"
        "\n"
        "  InitfsTools.exe -r | -run <initfs_path> -ta | -tempadd <payload_name> <line> [more lines...]\n"
        "      Temporarily append an applySettings block, launch the game, then revert after it closes.\n"
        "\n"
        "  InitfsTools.exe -h | -help | --help\n"
        "      Show this message.\n";
}

int CliRunner::run(const QStringList& args)
{
    // "-h" / "-help" / "--help" works with or without a path in front of it,
    // and doesn't require a file to load.
    for (const QString& a : args)
    {
        QString low = a.toLower();
        if (low == "-h" || low == "-help" || low == "--help")
        {
            printUsage();
            return 0;
        }
    }

    if (args.size() < 2)
    {
        printUsage();
        return 1;
    }

    QString path = args[0];
    QString cmd = args[1].toLower();
    QStringList trailing = args.mid(2);

    LoadedInitfs loaded;
    QString error;
    if (!loadInitfs(path, loaded, error))
    {
        std::cerr << "Failed to load initfs file: " << error.toStdString() << "\n";
        return 1;
    }

    if (cmd == "-pl" || cmd == "-payloadlist")
    {
        return cmdPayloadList(loaded);
    }
    else if (cmd == "-pc" || cmd == "-payloadcontents")
    {
        if (trailing.isEmpty())
        {
            std::cerr << "Missing payload name for -pc/-payloadcontents.\n";
            return 1;
        }
        return cmdPayloadContents(loaded, trailing[0]);
    }
    else if (cmd == "-pa" || cmd == "-permanentadd")
    {
        if (trailing.size() < 2)
        {
            std::cerr << "Usage: -pa <payload_name> <line> [more lines...]\n";
            return 1;
        }
        return cmdPermanentAdd(loaded, trailing[0], trailing.mid(1));
    }
    else if (cmd == "-ta" || cmd == "-tempadd")
    {
        if (trailing.size() < 2)
        {
            std::cerr << "Usage: -ta <payload_name> <line> [more lines...]\n";
            return 1;
        }
        return cmdTempAdd(loaded, trailing[0], trailing.mid(1));
    }

    std::cerr << "Unknown command: " << cmd.toStdString() << "\n";
    printUsage();
    return 1;
}

// ============================================================================
// Load / Save
// ============================================================================

bool CliRunner::loadInitfs(const QString& path, LoadedInitfs& out, QString& error)
{
    if (!QFile::exists(path))
    {
        error = "File not found: " + path;
        return false;
    }

    out.filePath = path;
    std::string stdPath = path.toLocal8Bit().constData();

    try
    {
        out.type = Converter::autoDetectDeobfuscatorType(stdPath);

        std::vector<uint8_t> defaultKey;
        {
            auto dk = Converter::getKey();
            defaultKey = std::vector<uint8_t>(dk.begin(), dk.end());
        }

        QString keysDir = QCoreApplication::applicationDirPath() + "/Keys";
        QStringList keyFiles;
        if (QDir(keysDir).exists())
            keyFiles = QDir(keysDir).entryList({ "*.key" }, QDir::Files);

        int storedKeyIndex = 0;
        bool storedKeyLogShown = false;
        bool keyWasFromPrompt = false;

        out.root = Converter::readPlainFileDbObject(
            stdPath, defaultKey, out.type, out.hadEncrypted,
            [&]() -> std::vector<uint8_t>
            {
                if (!storedKeyLogShown)
                {
                    storedKeyLogShown = true;
                    if (keyFiles.isEmpty())
                        Logger::log("[CLI LoadInitfs] File is encrypted - no stored keys found");
                    else
                        Logger::log("[CLI LoadInitfs] File is encrypted - trying %d stored key(s)...", keyFiles.size());
                }

                while (storedKeyIndex < keyFiles.size())
                {
                    const QString& fname = keyFiles[storedKeyIndex++];
                    QFile f(keysDir + "/" + fname);
                    if (!f.open(QIODevice::ReadOnly)) continue;
                    QString hex = QString::fromLatin1(f.readAll()).simplified().remove(' ');
                    QByteArray k = QByteArray::fromHex(hex.toLatin1());
                    if (k.size() != 16) continue;

                    Logger::log("[CLI LoadInitfs] Trying stored key: %s", fname.toLocal8Bit().constData());
                    return std::vector<uint8_t>(
                        reinterpret_cast<const uint8_t*>(k.constData()),
                        reinterpret_cast<const uint8_t*>(k.constData()) + k.size());
                }

                return {};
            });

        out.key = Converter::encryptionKey;

        // Persist a freshly-entered key the same way the GUI does
        if (keyWasFromPrompt && out.hadEncrypted && !out.key.empty())
        {
            QDir().mkpath(keysDir);
            bool alreadySaved = false;
            for (const QString& fname : QDir(keysDir).entryList({ "*.key" }, QDir::Files))
            {
                QFile f(keysDir + "/" + fname);
                if (!f.open(QIODevice::ReadOnly)) continue;
                QByteArray existing = QByteArray::fromHex(
                    QString::fromLatin1(f.readAll()).simplified().remove(' ').toLatin1());
                if (existing.size() == 16 &&
                    std::equal(out.key.begin(), out.key.end(),
                        reinterpret_cast<const uint8_t*>(existing.constData())))
                {
                    alreadySaved = true;
                    break;
                }
            }
            if (!alreadySaved)
            {
                QString savePath = keysDir + "/" +
                    QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".key";
                QFile kf(savePath);
                if (kf.open(QIODevice::WriteOnly | QIODevice::Text))
                {
                    QByteArray hexStr;
                    hexStr.reserve((int)out.key.size() * 2);
                    for (uint8_t b : out.key)
                    {
                        hexStr += kHexChars[(b >> 4) & 0xF];
                        hexStr += kHexChars[b & 0xF];
                    }
                    kf.write(hexStr);
                    kf.close();
                    Logger::log("[CLI LoadInitfs] Saved verified key to Keys/");
                }
            }
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        if (out.hadEncrypted)
        {
            error = "This initfs file is AES encrypted and no valid key was found in the "
                "Keys folder. Add a valid .key file (32-character hex AES key) to the "
                "Keys folder next to InitfsTools.exe, then run this command again.";
        }
        else
        {
            error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

void CliRunner::attemptCryptBaseCopy(const LoadedInitfs& loaded)
{
#ifdef Q_OS_WIN
    QString name = QFileInfo(loaded.filePath).fileName().toLower();
    if (!name.contains("win32"))
    {
        Logger::log("[Bcrypt] Not a Win32 file, skipping.");
        return;
    }

    QString exeDir = QCoreApplication::applicationDirPath();

    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    NTSTATUS(WINAPI * RtlGetVersion)(LPOSVERSIONINFOEXW) =
        (NTSTATUS(WINAPI*)(LPOSVERSIONINFOEXW))GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (RtlGetVersion) RtlGetVersion(&osvi);
    bool isWin10 = (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber < 22000);

    bool usedinput8 = isWin10
        && (loaded.hadEncrypted || loaded.type == DeobfuscatorType::PVZ);

    QString dllName = usedinput8 ? "dinput8.dll" : "bcrypt.dll";
    QString src = exeDir + "/" + dllName;
    if (!QFile::exists(src))
    {
        Logger::log("[Bcrypt] %s not found next to executable, skipping.",
            dllName.toUtf8().constData());
        return;
    }

    QDir d(QFileInfo(loaded.filePath).absolutePath());
    while (!d.isRoot())
    {
        if (!d.entryList({ "*.exe" }, QDir::Files).isEmpty()) break;
        d.cdUp();
    }

    if (!QDir(d.filePath("Data")).exists())
    {
        Logger::log("[Bcrypt] No Data folder found next to game exe, skipping.");
        return;
    }

    if (QFile::exists(d.filePath("dinput8.dll")) || QFile::exists(d.filePath("bcrypt.dll")))
    {
        Logger::log("[Bcrypt] DLL already present in game directory, skipping.");
        return;
    }

    QString dst = d.filePath(dllName);
    QFile::copy(src, dst);
    Logger::log("[Bcrypt] Copied %s to: %s",
        dllName.toUtf8().constData(), dst.toUtf8().constData());
#else
    Q_UNUSED(loaded)
#endif
}

void CliRunner::attemptAnticheatPatch(const LoadedInitfs& loaded)
{
#ifdef Q_OS_WIN
    QDir d(QFileInfo(loaded.filePath).absolutePath());
    while (!d.isRoot())
    {
        if (!d.entryList({ "*.exe" }, QDir::Files).isEmpty()) break;
        d.cdUp();
    }
    if (!QDir(d.filePath("Data")).exists())
    {
        Logger::log("[Patcher] No Data folder found next to game exe, skipping.");
        return;
    }
    PatcherBridge::apply(d.absolutePath());
#else
    Q_UNUSED(loaded)
#endif
}

bool CliRunner::saveInitfs(const LoadedInitfs& loaded, QString& error)
{
    try
    {
        attemptAnticheatPatch(loaded);
        attemptCryptBaseCopy(loaded);

        QByteArray pathBytes = loaded.filePath.toLocal8Bit();
        std::string stdPath(pathBytes.constData(), pathBytes.size());

        if (loaded.hadEncrypted)
        {
            auto plain = Converter::writePlainFileData(loaded.root);
            std::vector<uint8_t> key = loaded.key;
            if (key.size() != 16)
            {
                throw std::runtime_error(
                    "This initfs file is AES encrypted and no valid key was found in the "
                    "Keys folder. Add a valid .key file (32-character hex AES key) to the "
                    "Keys folder next to InitfsTools.exe, then run this command again.");
            }
            Converter::obfuscateInitfsFromPlainData(stdPath, plain, stdPath, key);
            Logger::log("[CLI SaveInitfs] Successfully saved initfs (AES-encrypted).");
        }
        else if (loaded.type == DeobfuscatorType::BF3)
        {
            Converter::writeBF3ObfuscatedInitfs(stdPath, loaded.root, stdPath);
            Logger::log("[CLI SaveInitfs] Successfully saved initfs (BF3 obfuscated).");
        }
        else if (loaded.type == DeobfuscatorType::PVZ)
        {
            Converter::writePvzObfuscatedInitfs(stdPath, loaded.root, stdPath);
            Logger::log("[CLI SaveInitfs] Successfully saved initfs (PVZ obfuscated).");
        }
        else
        {
            Converter::writeDeobfuscatedInitfsFromDbObject(stdPath, loaded.root, stdPath, loaded.type);
            Logger::log("[CLI SaveInitfs] Successfully saved initfs.");
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        error = QString::fromUtf8(ex.what());
        return false;
    }
}

// ============================================================================
// Payload lookup / text extraction
// ============================================================================

QString CliRunner::payloadDisplayName(DbObjectPtr child, int fallbackIdx)
{
    QString name;
    if (child->hasValue("name"))
    {
        std::string n = child->getValue<std::string>("name");
        name = QString::fromUtf8(n.c_str(), (int)n.size());
    }
    if (name.isEmpty() && child->hasValue("$file"))
    {
        DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
        if (fo && fo->hasValue("name"))
        {
            std::string n = fo->getValue<std::string>("name");
            name = QString::fromUtf8(n.c_str(), (int)n.size());
        }
    }
    if (name.isEmpty())
        name = QString("Payload %1").arg(fallbackIdx);
    return name;
}

bool CliRunner::findPayload(DbObjectPtr root, const QString& payloadName,
    int& outIndex, DbObjectPtr& outChild)
{
    if (!root) return false;

    int idx = 0;
    int exactIdx = -1;   DbObjectPtr exactChild;
    int partialIdx = -1; DbObjectPtr partialChild;

    root->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (child->hasValue("$file"))
                {
                    QString name = payloadDisplayName(child, idx);
                    if (exactIdx < 0 && name.compare(payloadName, Qt::CaseInsensitive) == 0)
                    {
                        exactIdx = idx; exactChild = child;
                    }
                    else if (partialIdx < 0 && name.endsWith(payloadName, Qt::CaseInsensitive))
                    {
                        partialIdx = idx; partialChild = child;
                    }
                    idx++;   // only count $file payloads — matches Converter::updatePayload
                }
            }
        });

    if (exactIdx >= 0) { outIndex = exactIdx;   outChild = exactChild;   return true; }
    if (partialIdx >= 0) { outIndex = partialIdx; outChild = partialChild; return true; }
    return false;
}

bool CliRunner::isProbablyText(const QByteArray& data)
{
    if (data.isEmpty()) return false;
    const int sampleSize = qMin(data.size(), 4096);
    int printable = 0;
    for (int i = 0; i < sampleSize; i++)
    {
        unsigned char c = (unsigned char)data[i];
        if ((c >= 0x20 && c <= 0x7E) || c == 0x09 || c == 0x0A || c == 0x0D)
            printable++;
    }
    return (printable * 100 / sampleSize) >= 80;
}

QString CliRunner::extractAsciiStrings(const QByteArray& data, int minLen)
{
    QStringList strings;
    QString current;
    for (unsigned char b : data)
    {
        if (b >= 0x20 && b <= 0x7E) current += QChar(b);
        else { if (current.length() >= minLen) strings.append(current); current.clear(); }
    }
    if (current.length() >= minLen) strings.append(current);
    return strings.join("\n");
}

QString CliRunner::extractPayloadText(DbObjectPtr childObj)
{
    if (!childObj) return QString();

    QString nameRaw;
    if (childObj->hasValue("name"))
    {
        std::string n = childObj->getValue<std::string>("name");
        nameRaw = QString::fromUtf8(n.c_str(), (int)n.size());
    }

    DbObjectPtr fileObj = childObj->getValue<DbObjectPtr>("$file");
    if (!fileObj) return QString();

    if (nameRaw.isEmpty() && fileObj->hasValue("name"))
    {
        std::string n = fileObj->getValue<std::string>("name");
        nameRaw = QString::fromUtf8(n.c_str(), (int)n.size());
    }

    QString nameLow = nameRaw.toLower();

    auto rawData = fileObj->getValue<std::vector<uint8_t>>("payload");
    if (rawData.empty()) return QString();

    QByteArray data(reinterpret_cast<const char*>(rawData.data()), (int)rawData.size());

    if (nameLow.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0
        || (nameLow.startsWith("stripped_") && nameLow.endsWith(".dbmanifest")))
    {
        auto fmt = DbManifestReconstructor::detectFormat(data);
        if (fmt == DbManifestReconstructor::Format::AlreadyXml)
            return QString::fromUtf8(data);
        if (fmt == DbManifestReconstructor::Format::AlreadyJson)
            return QString::fromUtf8(data); // pretty-printing intentionally skipped in CLI
        QString err;
        uint32_t discoveredSeed = UINT32_MAX;
        QString xml = DbManifestReconstructor::reconstruct(data, err, &discoveredSeed);
        if (!xml.isEmpty()) return xml;
        Logger::log("[CLI DbManifest] Reconstruction failed: %s", err.toUtf8().constData());
        return extractAsciiStrings(data);
    }

    if (nameLow.contains("dbmanifest") || nameLow.contains(".xml"))
        return QString::fromUtf8(data);

    if (nameLow.endsWith(".ebx"))
        return QString("[binary payload: %1 bytes]").arg(data.size());

    if (isProbablyText(data))
        return QString::fromUtf8(data);

    return QString("[binary payload: %1 bytes]").arg(data.size());
}

// ============================================================================
// applySettings block injection
// ============================================================================

QByteArray CliRunner::buildAppendedBlock(const QStringList& lines)
{
    QString block = "applySettings [=[\n";
    for (const QString& l : lines)
        block += l + "\n";
    block += "]=]";
    return block.toUtf8();
}

QByteArray CliRunner::applyAppendedBlock(const QByteArray& originalBytes, const QByteArray& block)
{
    QByteArray out = originalBytes;
    if (!out.isEmpty() && !out.endsWith('\n'))
        out += "\n";
    out += block;
    out += "\n";
    return out;
}

// ============================================================================
// Game dir / exe detection
// ============================================================================

QString CliRunner::findGameDirForInitfs(const QString& initfsPath)
{
    QDir d(QFileInfo(initfsPath).absolutePath());
    while (!d.isRoot())
    {
        if (!d.entryList({ "*.exe" }, QDir::Files).isEmpty()
            && QDir(d.filePath("Data")).exists())
            return d.absolutePath();
        d.cdUp();
    }
    return QString();
}

QString CliRunner::findBiggestExeInGameDir(const QString& gameDir)
{
    if (gameDir.isEmpty()) return QString();

    QDir d(gameDir);
    QFileInfoList exes = d.entryInfoList({ "*.exe" }, QDir::Files);
    if (exes.isEmpty()) return QString();

    QFileInfo biggest = exes.first();
    for (const QFileInfo& fi : exes)
        if (fi.size() > biggest.size())
            biggest = fi;

    return biggest.absoluteFilePath();
}

// ============================================================================
// Commands
// ============================================================================

int CliRunner::cmdPayloadList(const LoadedInitfs& loaded)
{
    if (!loaded.root) { std::cerr << "No data loaded.\n"; return 1; }

    int idx = 0, count = 0;
    loaded.root->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (child->hasValue("$file"))
                {
                    std::cout << payloadDisplayName(child, idx).toStdString() << "\n";
                    count++;
                    idx++;
                }
            }
        });

    std::cout << "\n" << count << " payload(s) total.\n";
    return 0;
}

int CliRunner::cmdPayloadContents(const LoadedInitfs& loaded, const QString& payloadName)
{
    int idx; DbObjectPtr child;
    if (!findPayload(loaded.root, payloadName, idx, child))
    {
        std::cerr << "Payload not found: " << payloadName.toStdString() << "\n";
        return 1;
    }

    std::cout << extractPayloadText(child).toStdString() << "\n";
    return 0;
}

int CliRunner::cmdPermanentAdd(LoadedInitfs& loaded, const QString& payloadName, const QStringList& lines)
{
    int idx; DbObjectPtr child;
    if (!findPayload(loaded.root, payloadName, idx, child))
    {
        std::cerr << "Payload not found: " << payloadName.toStdString() << "\n";
        return 1;
    }

    DbObjectPtr fileObj = child->getValue<DbObjectPtr>("$file");
    if (!fileObj) { std::cerr << "Payload has no $file data.\n"; return 1; }

    auto rawVec = fileObj->getValue<std::vector<uint8_t>>("payload");
    QByteArray original(reinterpret_cast<const char*>(rawVec.data()), (int)rawVec.size());

    QByteArray block = buildAppendedBlock(lines);
    QByteArray newBytes = applyAppendedBlock(original, block);

    Converter::updatePayload(loaded.root, idx,
        std::vector<uint8_t>(newBytes.begin(), newBytes.end()));

    QString error;
    if (!saveInitfs(loaded, error))
    {
        std::cerr << "Save failed: " << error.toStdString() << "\n";
        return 1;
    }

    std::cout << "Permanently added " << lines.size()
               << " line(s) to payload \"" << payloadName.toStdString() << "\" and saved.\n";
    return 0;
}

int CliRunner::cmdTempAdd(LoadedInitfs& loaded, const QString& payloadName, const QStringList& lines)
{
    int idx; DbObjectPtr child;
    if (!findPayload(loaded.root, payloadName, idx, child))
    {
        std::cerr << "Payload not found: " << payloadName.toStdString() << "\n";
        return 1;
    }

    DbObjectPtr fileObj = child->getValue<DbObjectPtr>("$file");
    if (!fileObj) { std::cerr << "Payload has no $file data.\n"; return 1; }

    auto rawVec = fileObj->getValue<std::vector<uint8_t>>("payload");
    QByteArray original(reinterpret_cast<const char*>(rawVec.data()), (int)rawVec.size());

    QByteArray block = buildAppendedBlock(lines);
    QByteArray modified = applyAppendedBlock(original, block);

    // --- write temp lines, save ---
    Converter::updatePayload(loaded.root, idx,
        std::vector<uint8_t>(modified.begin(), modified.end()));

    QString error;
    if (!saveInitfs(loaded, error))
    {
        std::cerr << "Save failed: " << error.toStdString() << "\n";
        return 1;
    }
    std::cout << "Temporarily added " << lines.size() << " line(s) to payload \""
               << payloadName.toStdString() << "\" and saved.\n";

    // --- launch biggest exe (detached — survives us exiting) ---
    QString gameDir = findGameDirForInitfs(loaded.filePath);
    QString exePath = findBiggestExeInGameDir(gameDir);

    if (exePath.isEmpty())
    {
        std::cerr << "Could not find any .exe near the initfs file — reverting immediately.\n";
    }
    else
    {
        std::cout << "Launching: " << exePath.toStdString() << "\n";

        qint64 pid = 0;
        bool started = QProcess::startDetached(
            exePath, {}, QFileInfo(exePath).absolutePath(), &pid);

        if (!started)
        {
            std::cerr << "Failed to launch " << exePath.toStdString() << " — reverting immediately.\n";
        }
        else
        {
            std::cout << "Waiting for the game process to close...\n";
            const int pollIntervalMs = 250;
            while (isProcessRunning(pid))
                CliSleeper::msleep(pollIntervalMs);

#ifdef Q_OS_WIN
            const QString exeName = QFileInfo(exePath).fileName();
            const int handoffWaitMs = 10000;
            int handoffWaited = 0;
            qint64 relaunchedPid = 0;

            std::cout << "Checking whether the launcher handed off to the game...\n";
            while (handoffWaited < handoffWaitMs)
            {
                relaunchedPid = findRunningProcessByExeName(exeName);
                if (relaunchedPid != 0) break;
                CliSleeper::msleep(pollIntervalMs);
                handoffWaited += pollIntervalMs;
            }

            if (relaunchedPid != 0)
            {
                std::cout << "Detected relaunched game process — waiting for it to close...\n";
                while (isProcessRunning(relaunchedPid))
                    CliSleeper::msleep(pollIntervalMs);
            }
#endif

            std::cout << "Game process has exited — reverting now...\n";
        }
    }

    // --- revert temp lines, save again ---
    Converter::updatePayload(loaded.root, idx,
        std::vector<uint8_t>(original.begin(), original.end()));

    if (!saveInitfs(loaded, error))
    {
        std::cerr << "Revert save failed: " << error.toStdString() << "\n";
        return 1;
    }

    std::cout << "Reverted temporary lines in payload \"" << payloadName.toStdString()
               << "\" and saved.\n";
    return 0;
}