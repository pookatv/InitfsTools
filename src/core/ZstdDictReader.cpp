#include "ZstdDictReader.h"

#include <QString>
#include <QByteArray>
#include <QStringList>
#include <QTextStream>
#include <algorithm>

// ---- helpers ----

static inline quint32 readU32LE(const QByteArray& d, int off)
{
    return  (quint32)(quint8)d[off]
        | ((quint32)(quint8)d[off + 1] << 8)
        | ((quint32)(quint8)d[off + 2] << 16)
        | ((quint32)(quint8)d[off + 3] << 24);
}

// ---- public API ----

bool ZstdDictReader::isZstdDict(const QByteArray& data)
{
    if (data.size() < kMinDictSize) return false;
    return readU32LE(data, 0) == kZstdDictMagic;
}

QString ZstdDictReader::describe(const QByteArray& data)
{
    QString out;
    QTextStream ts(&out);

    if (data.size() < kMinDictSize)
    {
        ts << "[ZSTD Dictionary]\n"
            << "  <file too small to parse — "
            << data.size() << " bytes>\n";
        ts.flush();
        return out;
    }

    quint32 magic = readU32LE(data, 0);
    quint32 dictId = readU32LE(data, 4);
    int     bodySize = data.size() - 8;

    // ---- header block ----
    ts << "[ZSTD Dictionary]\n";
    if (magic != kZstdDictMagic)
    {
        ts << "  Warning: unexpected magic 0x"
            << QString::number(magic, 16).toUpper().rightJustified(8, '0')
            << "  (expected 0xEC30A437)\n";
    }

    ts << "\n";
    ts << "  Magic          0x"
        << QString::number(magic, 16).toUpper().rightJustified(8, '0') << "\n";
    ts << "  Dictionary ID  " << dictId
        << "  (0x" << QString::number(dictId, 16).toUpper() << ")\n";
    ts << "  Total size     " << data.size() << " bytes\n";
    ts << "  Body size      " << bodySize << " bytes\n";

    // ---- string samples block ----
    ts << "\n";
    ts << "  ----------------------------------------\n";
    ts << "  Strings embedded in body\n";
    ts << "  ----------------------------------------\n";

    QByteArray body = data.mid(8);

    // Collect all runs, then split into meaningful vs. noisy buckets
    QStringList all = extractStrings(body, 6);

    QStringList meaningful;
    QStringList noisy;
    for (const QString& s : all)
    {
        if (isMeaningful(s))
            meaningful.append(s);
        else
            noisy.append(s);
    }

    // Sort meaningful strings: longer / more informative ones first
    std::sort(meaningful.begin(), meaningful.end(),
        [](const QString& a, const QString& b) {
            return a.length() > b.length();
        });

    if (meaningful.isEmpty())
    {
        ts << "  <no identifiable strings found in body>\n";
    }
    else
    {
        for (const QString& s : meaningful)
            ts << "  " << s << "\n";

        ts << "\n";
        ts << "  " << meaningful.size() << " meaningful string"
            << (meaningful.size() != 1 ? "s" : "") << " extracted";

        if (!noisy.isEmpty())
            ts << "  (" << noisy.size() << " noisy/binary runs suppressed)";

        ts << "\n";
    }

    ts.flush();
    return out;
}

// ---- private ----

bool ZstdDictReader::isMeaningful(const QString& s)
{
    const int len = s.length();
    if (len < 6)
        return false;

    int alphaDigit = 0;
    bool hasLetter = false;

    for (const QChar& c : s)
    {
        if (c.isLetter()) { ++alphaDigit; hasLetter = true; }
        else if (c.isDigit() || c == '_') ++alphaDigit;
    }

    if (!hasLetter) return false;
    return (double)alphaDigit / (double)len >= 0.55;
}

QStringList ZstdDictReader::extractStrings(const QByteArray& body, int minLen)
{
    QStringList result;
    const char* p = body.constData();
    const int   len = body.size();

    int run = 0;
    int start = 0;

    for (int i = 0; i <= len; ++i)
    {
        bool printable = (i < len)
            && (quint8)p[i] >= 0x20
            && (quint8)p[i] <= 0x7E;

        if (printable)
        {
            if (run == 0) start = i;
            ++run;
        }
        else
        {
            if (run >= minLen)
            {
                // Trim leading/trailing ASCII whitespace in-place on the raw bytes
                int s2 = start, e2 = start + run;
                while (s2 < e2 && (unsigned char)p[s2] <= 0x20) ++s2;
                while (e2 > s2 && (unsigned char)p[e2 - 1] <= 0x20) --e2;
                int trimmed = e2 - s2;
                if (trimmed >= minLen)
                    result.append(QString::fromLatin1(p + s2, trimmed));
            }
            run = 0;
        }
    }

    return result;
}