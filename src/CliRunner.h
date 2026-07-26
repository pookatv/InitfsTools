#pragma once

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <vector>
#include <cstdint>

#include "Converter.h"
#include "DbObject.h"

class CliRunner
{
public:
    // True if argv[1] is "-r" or "-run" (case-insensitive)
    static bool isCliInvocation(int argc, char* argv[]);

    // args = everything AFTER "-r"/"-run" on the command line
    // Returns a process exit code (0 = success)
    static int run(const QStringList& args);

private:
    struct LoadedInitfs
    {
        QString              filePath;
        DbObjectPtr          root;
        DeobfuscatorType     type = DeobfuscatorType::PVZ;
        bool                 hadEncrypted = false;
        std::vector<uint8_t> key;
    };

    static void printUsage();

    static bool loadInitfs(const QString& path, LoadedInitfs& out, QString& error);
    static bool saveInitfs(const LoadedInitfs& loaded, QString& error);

    static void attemptAnticheatPatch(const LoadedInitfs& loaded);
    static void attemptCryptBaseCopy(const LoadedInitfs& loaded);

    static bool    findPayload(DbObjectPtr root, const QString& payloadName,
                                int& outIndex, DbObjectPtr& outChild);
    static QString payloadDisplayName(DbObjectPtr child, int fallbackIdx);

    static QString extractPayloadText(DbObjectPtr childObj);
    static bool    isProbablyText(const QByteArray& data);
    static QString extractAsciiStrings(const QByteArray& data, int minLen = 4);

    static QByteArray buildAppendedBlock(const QStringList& lines);
    static QByteArray applyAppendedBlock(const QByteArray& originalBytes, const QByteArray& block);

    static QString findGameDirForInitfs(const QString& initfsPath);
    static QString findBiggestExeInGameDir(const QString& gameDir);

    static int cmdPayloadList(const LoadedInitfs& loaded);
    static int cmdPayloadContents(const LoadedInitfs& loaded, const QString& payloadName);
    static int cmdPermanentAdd(LoadedInitfs& loaded, const QString& payloadName, const QStringList& lines);
    static int cmdTempAdd(LoadedInitfs& loaded, const QString& payloadName, const QStringList& lines);
};