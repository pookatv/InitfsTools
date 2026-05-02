#pragma once

#include <QString>
#include <QByteArray>
#include <QStringList>

// ============================================================
//  ZstdDictReader
//  Produces a human-readable description of a raw ZSTD
//  dictionary payload (e.g. Dictionaries/ebx.dict)
//
//  The ZSTD dictionary format (RFC 8478) begins with a 4-byte
//  magic number 0xEC30A437 (little-endian), followed by a
//  4-byte dictionary ID, then raw sample data used by the
//  compressor.  We parse the header and extract printable
//  strings from the body to give users a meaningful text view
//  instead of raw hex
// ============================================================
class ZstdDictReader
{
public:
    static bool    isZstdDict(const QByteArray& data);
    static QString describe(const QByteArray& data);

private:
    static constexpr quint32 kZstdDictMagic = 0xEC30A437u;
    static constexpr int     kMinDictSize = 8;

    static QStringList extractStrings(const QByteArray& body, int minLen = 6);

    // Returns true if the string looks like meaningful text rather than
    // binary noise (e.g. has a reasonable ratio of alpha/digit chars).
    static bool isMeaningful(const QString& s);
};