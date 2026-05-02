#include "LiveValueReader.h"
#include "TypeExtractorWindow.h"

#include <QDebug>
#include <QByteArray>
#include <cstring>
#include <cmath>
#include <algorithm>

// ============================================================
// Helpers
// ============================================================
static QString bytesToHex(const QByteArray& ba, int offset, int len)
{
    QString out;
    for (int i = offset; i < offset + len && i < ba.size(); ++i) {
        if (!out.isEmpty()) out += ' ';
        out += QStringLiteral("%1").arg((quint8)ba[i], 2, 16, QChar('0')).toUpper();
    }
    return out;
}

static inline qint32  readI32(const QByteArray& b, int o) {
    qint32 v; memcpy(&v, b.constData() + o, 4); return v;
}
static inline quint32 readU32(const QByteArray& b, int o) {
    quint32 v; memcpy(&v, b.constData() + o, 4); return v;
}
static inline qint64  readI64(const QByteArray& b, int o) {
    qint64 v; memcpy(&v, b.constData() + o, 8); return v;
}
static inline float   readF32(const QByteArray& b, int o) {
    float v; memcpy(&v, b.constData() + o, 4); return v;
}
static inline double  readF64(const QByteArray& b, int o) {
    double v; memcpy(&v, b.constData() + o, 8); return v;
}
static inline qint16  readI16(const QByteArray& b, int o) {
    qint16 v; memcpy(&v, b.constData() + o, 2); return v;
}
static inline quint16 readU16(const QByteArray& b, int o) {
    quint16 v; memcpy(&v, b.constData() + o, 2); return v;
}

// ============================================================
// LiveMemoryReader
// ============================================================
LiveMemoryReader::LiveMemoryReader(DWORD processId)
{
    const DWORD access = PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION;
    m_handle = OpenProcess(access, FALSE, processId);
    if (!m_handle)
        qDebug("[LiveMemoryReader] OpenProcess failed: %lu", GetLastError());
    else
        qDebug("[LiveMemoryReader] OpenProcess succeeded: 0x%p", m_handle);
}

LiveMemoryReader::~LiveMemoryReader()
{
    if (m_handle) { CloseHandle(m_handle); m_handle = nullptr; }
}

QByteArray LiveMemoryReader::readBytes(qint64 address, int numBytes) const
{
    if (numBytes <= 0 || address <= 0) return {};
    QByteArray buf(numBytes, Qt::Uninitialized);
    SIZE_T bytesRead = 0;
    BOOL ok = ReadProcessMemory(m_handle,
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)),
        buf.data(), static_cast<SIZE_T>(numBytes), &bytesRead);
    DWORD err = GetLastError();
    if ((!ok && err != 299) || bytesRead == 0) return {};
    buf.resize(static_cast<int>(bytesRead));
    return buf;
}

QByteArray LiveMemoryReader::readBytes(int numBytes)
{
    QByteArray result = readBytes(position, numBytes);
    if (!result.isEmpty()) position += numBytes;
    return result;
}

quint8 LiveMemoryReader::readByte()
{
    QByteArray b = readBytes(position, 1);
    if (b.isEmpty()) qFatal("[LiveMemoryReader] ReadByte failed at 0x%llX", (unsigned long long)position);
    position += 1;
    return static_cast<quint8>(b[0]);
}

qint32 LiveMemoryReader::readInt()
{
    QByteArray b = readBytes(position, 4);
    if (b.isEmpty()) qFatal("[LiveMemoryReader] ReadInt failed at 0x%llX", (unsigned long long)position);
    position += 4;
    return readI32(b, 0);
}

quint32 LiveMemoryReader::readUInt()
{
    QByteArray b = readBytes(position, 4);
    if (b.isEmpty()) qFatal("[LiveMemoryReader] ReadUInt failed at 0x%llX", (unsigned long long)position);
    position += 4;
    return readU32(b, 0);
}

qint64 LiveMemoryReader::readLong()
{
    QByteArray b = readBytes(position, 8);
    if (b.isEmpty()) qFatal("[LiveMemoryReader] ReadLong failed at 0x%llX", (unsigned long long)position);
    position += 8;
    return readI64(b, 0);
}

// ============================================================
// LiveValueReader — construction / destruction
// ============================================================
LiveValueReader::LiveValueReader(DWORD processId, HANDLE processHandle,
    qint64 moduleBase, qint64 moduleSize,
    const QVector<UITypeItem>* allTypes)
    : m_processHandle(processHandle)
    , m_moduleBase(moduleBase)
    , m_moduleSize(moduleSize)
    , m_allTypes(allTypes)
{
    m_reader = new LiveMemoryReader(processId);
}

LiveValueReader::~LiveValueReader()
{
    delete m_reader;
}

// ============================================================
// Simple accessors
// ============================================================
bool LiveValueReader::canRead(qint64 addr) const
{
    return addr > 0x10000LL && addr < 0x7FFFFFFFFFFFL;
}

bool LiveValueReader::isMappedBySettingsList(const QString& typeName) const
{
    return m_settingsListFieldData.contains(typeName);
}

bool LiveValueReader::tryGetCandidateName(const QString& typeName, QString& outName) const
{
    auto it = m_settingsListCandidateNames.find(typeName);
    if (it == m_settingsListCandidateNames.end()) return false;
    outName = it.value();
    return true;
}

bool LiveValueReader::tryGetFieldDataAddress(const QString& typeName, qint64& outAddr) const
{
    auto it = m_settingsListFieldData.find(typeName);
    if (it == m_settingsListFieldData.end()) return false;
    outAddr = it.value();
    return true;
}

bool LiveValueReader::tryGetInstance(const QString& typeName, qint64& outBase) const
{
    auto it = m_instanceAddresses.find(typeName);
    if (it == m_instanceAddresses.end()) return false;
    outBase = it.value();
    return true;
}

// ============================================================
// VirtualQueryEx wrapper
// ============================================================
bool LiveValueReader::virtualQueryRegion(qint64 addr, MemBasicInfo& out) const
{
    MEMORY_BASIC_INFORMATION mbi{};
    int r = VirtualQueryEx(m_reader->getHandle(),
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)),
        &mbi, sizeof(mbi));
    if (r == 0) return false;
    out.baseAddress = reinterpret_cast<qint64>(mbi.BaseAddress);
    out.allocationBase = reinterpret_cast<qint64>(mbi.AllocationBase);
    out.allocProtect = mbi.AllocationProtect;
    out.regionSize = static_cast<qint64>(mbi.RegionSize);
    out.state = mbi.State;
    out.protect = mbi.Protect;
    out.type = mbi.Type;
    return true;
}

// ============================================================
// TryReadAsciiString
// ============================================================
QString LiveValueReader::tryReadAsciiString(qint64 addr, int maxLen) const
{
    if (!canRead(addr)) return {};
    QByteArray buf = m_reader->readBytes(addr, maxLen);
    if (buf.isEmpty()) return {};
    // Find the null terminator and validate all bytes in one pass
    int len = 0;
    for (int i = 0; i < buf.size(); ++i) {
        quint8 c = static_cast<quint8>(buf[i]);
        if (c == 0) break;
        if (c < 32 || c > 126) return {};
        ++len;
    }
    return QString::fromLatin1(buf.constData(), len);
}

// ============================================================
// FindSettingsListBase  (self-referential scan, range 0x70000000-0x80000000)
// ============================================================
qint64 LiveValueReader::findSettingsListBase() const
{
    const qint64 scanStart = 0x70000000LL;
    const qint64 scanEnd = 0x80000000LL;
    const qint64 CHUNK = 1LL * 1024 * 1024;

    for (qint64 pos = scanStart; pos < scanEnd; pos += CHUNK) {
        qint64 chunkEnd = qMin(pos + CHUNK, scanEnd);
        int    chunkLen = static_cast<int>(chunkEnd - pos);
        QByteArray buf = m_reader->readBytes(pos, chunkLen);
        if (buf.isEmpty()) continue;

        for (int off = 0; off <= buf.size() - 32; off += 4) {
            // Must start with 4 zero bytes
            if (buf[off] || buf[off + 1] || buf[off + 2] || buf[off + 3]) continue;

            if (off + 4 >= buf.size()) continue;
            quint8 firstNameByte = static_cast<quint8>(buf[off + 4]);
            if (!((firstNameByte >= 'A' && firstNameByte <= 'Z') ||
                (firstNameByte >= 'a' && firstNameByte <= 'z')))
                continue;

            int nameStart = off + 4;
            int nameLen = 0;
            bool nameValid = true;
            for (int c = 0; c < 32; ++c) {
                int idx = nameStart + c;
                if (idx >= buf.size()) { nameValid = false; break; }
                quint8 nb = static_cast<quint8>(buf[idx]);
                if (nb == 0) break;
                if (nb < 32 || nb > 126) { nameValid = false; break; }
                nameLen++;
            }
            if (!nameValid || nameLen < 2) continue;

            QString candidateNameCheck = QString::fromLatin1(buf.constData() + nameStart, nameLen);
            if (candidateNameCheck.contains('.')) continue;

            int nameBlockSize = ((nameLen + 1 + 7) / 8) * 8;
            qint64 expectedSelfRef = pos + nameStart;
            int ptrOff = nameStart + nameBlockSize;
            if (ptrOff + 16 > buf.size()) continue;

            qint64 p1 = readI64(buf, ptrOff);
            qint64 p2 = readI64(buf, ptrOff + 8);
            if (!canRead(p1) || !canRead(p2)) continue;
            if (p1 != expectedSelfRef) continue;

            qDebug("[FindSettingsListBase] Self-referential candidate '%s' at 0x%llX p2=0x%llX",
                qPrintable(candidateNameCheck),
                (unsigned long long)expectedSelfRef,
                (unsigned long long)p2);

            QByteArray probe = m_reader->readBytes(expectedSelfRef + 24, 0x400);
            if (probe.isEmpty()) continue;

            bool hasNeighbour = false;
            bool hasPeriodName = false;
            for (int g = 0; g < probe.size() - 32; ++g) {
                quint8 pb = static_cast<quint8>(probe[g]);
                if (pb < 'A' || pb > 'Z') continue;

                int nLen = 0;
                bool nValid = true;
                for (int c = g; c < qMin(g + 32, probe.size()); ++c) {
                    if (probe[c] == 0) break;
                    quint8 nc = static_cast<quint8>(probe[c]);
                    if (nc < 32 || nc > 126) { nValid = false; break; }
                    nLen++;
                }
                if (!nValid || nLen < 2) continue;

                QString neighbourName = QString::fromLatin1(probe.constData() + g, nLen);
                if (neighbourName.contains('.')) { hasPeriodName = true; break; }

                int nBlock = ((nLen + 1 + 7) / 8) * 8;
                if (g + nBlock + 24 > probe.size()) continue;

                qint64 nInst = readI64(probe, g + nBlock);
                qint64 nFd = readI64(probe, g + nBlock + 8);
                if (!canRead(nInst) || !canRead(nFd)) continue;
                if (nInst < 0x100000000LL || nFd < 0x100000000LL) continue;

                hasNeighbour = true;
                break;
            }

            if (hasPeriodName) {
                qDebug("[FindSettingsListBase] Rejected at 0x%llX — period name in probe",
                    (unsigned long long)expectedSelfRef);
                continue;
            }
            if (hasNeighbour) {
                qDebug("[FindSettingsListBase] Confirmed at 0x%llX",
                    (unsigned long long)expectedSelfRef);
                return expectedSelfRef;
            }
            qDebug("[FindSettingsListBase] No neighbour, skipping");
        }
    }
    qDebug("[FindSettingsListBase] Not found in 0x%llX-0x%llX",
        (unsigned long long)scanStart, (unsigned long long)scanEnd);
    return 0;
}

// ============================================================
// FindSettingsListBaseSentinel  (0xFF*8 sentinel, range 0x40000000-0x50000000)
// ============================================================
qint64 LiveValueReader::findSettingsListBaseSentinel() const
{
    const qint64 scanStart = 0x40000000LL;
    const qint64 scanEnd = 0x50000000LL;
    const qint64 CHUNK = 4LL * 1024 * 1024;

    qint64 addr = scanStart;
    while (addr < scanEnd) {
        MemBasicInfo mbi;
        if (!virtualQueryRegion(addr, mbi)) { addr += 0x1000; continue; }

        qint64 regionBase = mbi.baseAddress;
        qint64 regionSize = mbi.regionSize;
        qint64 regionEnd = regionBase + regionSize;

        bool isCommitted = (mbi.state & 0x1000) != 0;
        bool isReadable = (mbi.protect & 0x01) == 0 && (mbi.protect & 0x100) == 0;

        if (!isCommitted || !isReadable || regionSize == 0) {
            addr = regionEnd > addr ? regionEnd : addr + 0x1000;
            continue;
        }

        qint64 readStart = qMax(regionBase, scanStart);
        qint64 readEnd = qMin(regionEnd, scanEnd);

        for (qint64 pos = readStart; pos < readEnd; pos += CHUNK) {
            qint64 chunkEnd = qMin(pos + CHUNK, readEnd);
            int chunkLen = static_cast<int>(chunkEnd - pos);
            QByteArray buf = m_reader->readBytes(pos, chunkLen);
            if (buf.isEmpty()) continue;

            for (int off = 0; off <= buf.size() - 64; off += 8) {
                // Must be 8 bytes of 0xFF
                bool isSentinel = true;
                for (int k = 0; k < 8 && isSentinel; ++k)
                    isSentinel = (static_cast<quint8>(buf[off + k]) == 0xFF);
                if (!isSentinel) continue;

                int nameOff = off + 8;
                if (nameOff >= buf.size()) continue;
                quint8 firstByte = static_cast<quint8>(buf[nameOff]);
                if (!((firstByte >= 'A' && firstByte <= 'Z') ||
                    (firstByte >= 'a' && firstByte <= 'z')))
                    continue;

                int nameLen = 0;
                bool nameValid = true;
                for (int c = 0; c < 32; ++c) {
                    int idx = nameOff + c;
                    if (idx >= buf.size()) { nameValid = false; break; }
                    quint8 nb = static_cast<quint8>(buf[idx]);
                    if (nb == 0) break;
                    if (nb < 32 || nb > 126) { nameValid = false; break; }
                    nameLen++;
                }
                if (!nameValid || nameLen < 2) continue;

                QString candidateName = QString::fromLatin1(buf.constData() + nameOff, nameLen);
                if (candidateName.contains('.')) continue;

                int nameBlockSize = ((nameLen + 1 + 7) / 8) * 8;
                int ptrOff = nameOff + nameBlockSize;
                if (ptrOff + 24 > buf.size()) continue;

                qint64 p1 = readI64(buf, ptrOff);
                qint64 p2 = readI64(buf, ptrOff + 8);
                qint64 expectedSelfRef = pos + nameOff;
                if (p1 != expectedSelfRef) continue;
                if (!canRead(p2) || p2 == 0) continue;

                qDebug("[FindSettingsListBaseSentinel] Candidate '%s' at 0x%llX",
                    qPrintable(candidateName), (unsigned long long)expectedSelfRef);

                // Probe for a neighbour
                qint64 probeStart = expectedSelfRef + nameBlockSize + 24;
                int probeOff = static_cast<int>(probeStart - pos);
                QByteArray probe;
                if (probeOff >= 0 && probeOff + 0x200 <= buf.size()) {
                    probe = buf.mid(probeOff, 0x200);
                }
                else {
                    probe = m_reader->readBytes(probeStart, 0x200);
                }
                if (probe.isEmpty()) continue;

                bool hasNeighbour = false;
                bool hasPeriodName = false;
                for (int g = 0; g < probe.size() - 32; ++g) {
                    quint8 pb = static_cast<quint8>(probe[g]);
                    if (!((pb >= 'A' && pb <= 'Z') || (pb >= 'a' && pb <= 'z'))) continue;

                    int nLen = 0; bool nValid = true;
                    for (int c = g; c < qMin(g + 32, probe.size()); ++c) {
                        if (probe[c] == 0) break;
                        quint8 nc = static_cast<quint8>(probe[c]);
                        if (nc < 32 || nc > 126) { nValid = false; break; }
                        nLen++;
                    }
                    if (!nValid || nLen < 2) continue;

                    QString nb2 = QString::fromLatin1(probe.constData() + g, nLen);
                    if (nb2.contains('.')) { hasPeriodName = true; break; }

                    int nBlock = ((nLen + 1 + 7) / 8) * 8;
                    if (g + nBlock + 16 > probe.size()) continue;

                    qint64 nP1 = readI64(probe, g + nBlock);
                    qint64 nP2 = readI64(probe, g + nBlock + 8);
                    if (!canRead(nP1) || !canRead(nP2)) continue;
                    if (nP1 == 0 || nP2 == 0) continue;

                    hasNeighbour = true;
                    break;
                }
                if (hasPeriodName) continue;
                if (hasNeighbour) {
                    qDebug("[FindSettingsListBaseSentinel] Confirmed at 0x%llX",
                        (unsigned long long)expectedSelfRef);
                    return expectedSelfRef;
                }
                qDebug("[FindSettingsListBaseSentinel] No neighbour at 0x%llX",
                    (unsigned long long)expectedSelfRef);
            }
        }
        addr = regionEnd > addr ? regionEnd : addr + 0x1000;
    }
    qDebug("[FindSettingsListBaseSentinel] Not found");
    return 0;
}

// ============================================================
// WalkSettingsList
// ============================================================
void LiveValueReader::walkSettingsList(qint64 listBase,
    const QHash<qint64, QString>& instanceToName)
{
    qDebug("[WalkSettingsList] Starting at 0x%llX", (unsigned long long)listBase);

    int entriesFound = 0;
    int consecutiveSkips = 0;
    qint64 maxAddr = listBase + 0x100000;
    qint64 pos = listBase;

    int windowSize = static_cast<int>(maxAddr - listBase);
    QByteArray window = m_reader->readBytes(listBase, windowSize);
    if (window.isEmpty()) {
        qDebug("[WalkSettingsList] Failed to read window");
        return;
    }

    while (pos < maxAddr) {
        int localOff = static_cast<int>(pos - listBase);
        if (localOff >= window.size()) break;

        // Sentinel: 8 bytes of 0xFF
        if (localOff + 8 <= window.size()) {
            bool isSentinel = true;
            for (int k = 0; k < 8 && isSentinel; ++k)
                isSentinel = (static_cast<quint8>(window[localOff + k]) == 0xFF);
            if (isSentinel) {
                qDebug("[WalkSettingsList] Sentinel at 0x%llX", (unsigned long long)pos);
                consecutiveSkips = 0;
                pos += 8;
                continue;
            }
        }

        quint8 firstByte = static_cast<quint8>(window[localOff]);
        bool isLetter = (firstByte >= 'A' && firstByte <= 'Z') ||
            (firstByte >= 'a' && firstByte <= 'z');
        if (!isLetter) { pos += 8; continue; }

        // Parse name
        QString candidateName;
        int nameLen = 0;
        bool nameValid = true;
        for (int c = 0; c < 64; ++c) {
            int idx = localOff + c;
            if (idx >= window.size()) { nameValid = false; break; }
            quint8 b = static_cast<quint8>(window[idx]);
            nameLen++;
            if (b == 0) break;
            if (b < 32 || b > 126) { nameValid = false; break; }
            candidateName += QChar(b);
        }

        if (!nameValid || candidateName.length() < 2 || candidateName.contains('.')) {
            consecutiveSkips++;
            if (consecutiveSkips >= 10) {
                qDebug("[WalkSettingsList] 10 consecutive skips at 0x%llX, stopping",
                    (unsigned long long)pos);
                break;
            }
            pos += 8;
            continue;
        }

        consecutiveSkips = 0;

        int alignedLen = ((nameLen + 7) / 8) * 8;
        qint64 afterName = pos + alignedLen;
        int afterOff = static_cast<int>(afterName - listBase);

        if (afterOff + 24 > window.size()) { pos += 8; continue; }

        qint64 p1 = readI64(window, afterOff);
        qint64 p2 = readI64(window, afterOff + 8);
        qint64 p3 = readI64(window, afterOff + 16);

        qDebug("[WalkSettingsList] candidate at 0x%llX: '%s' p1=0x%llX p2=0x%llX p3=0x%llX",
            (unsigned long long)pos, qPrintable(candidateName),
            (unsigned long long)p1, (unsigned long long)p2, (unsigned long long)p3);

        if (!canRead(p1) || !canRead(p2)) {
            consecutiveSkips++;
            if (consecutiveSkips >= 10) {
                qDebug("[WalkSettingsList] 10 consecutive skips, stopping");
                break;
            }
            pos += 8;
            continue;
        }
        consecutiveSkips = 0;

        qDebug("[WalkSettingsList] [%d] \"%s\" instance=0x%llX fieldData=0x%llX",
            entriesFound, qPrintable(candidateName),
            (unsigned long long)p1, (unsigned long long)p2);

        // Step 1: match p1 or p2 against instanceToName
        QString resolvedTypeName;
        qint64  resolvedInstance = 0;
        qint64  resolvedFieldData = 0;

        if (instanceToName.contains(p1)) {
            resolvedTypeName = instanceToName.value(p1);
            resolvedInstance = p1;
            resolvedFieldData = p2;
        }
        else if (instanceToName.contains(p2)) {
            resolvedTypeName = instanceToName.value(p2);
            resolvedInstance = p2;
            resolvedFieldData = p3;
            qDebug("[WalkSettingsList]   (p1 miss, matched via p2)");
        }

        if (!resolvedTypeName.isEmpty()) {
            m_settingsListFieldData[resolvedTypeName] = resolvedFieldData;
            m_instanceAddresses[resolvedTypeName] = resolvedInstance;
            m_settingsListCandidateNames[resolvedTypeName] = candidateName;
            qDebug("[WalkSettingsList]   MAPPED '%s' -> '%s' instance=0x%llX fieldData=0x%llX",
                qPrintable(candidateName), qPrintable(resolvedTypeName),
                (unsigned long long)resolvedInstance, (unsigned long long)resolvedFieldData);
        }
        else {
            // Step 2: name-based fallback
            QString lookupName = candidateName.remove(' ');
            const UITypeItem* nameMatch = nullptr;
            if (m_allTypes) {
                for (const auto& t : *m_allTypes) {
                    if (t.name.compare(lookupName, Qt::CaseInsensitive) == 0 ||
                        t.name.compare(lookupName + "Settings", Qt::CaseInsensitive) == 0) {
                        nameMatch = &t;
                        break;
                    }
                }
            }
            if (nameMatch) {
                m_settingsListFieldData[nameMatch->name] = p2;
                m_instanceAddresses[nameMatch->name] = p1;
                m_settingsListCandidateNames[nameMatch->name] = candidateName;
                qDebug("[WalkSettingsList]   MAPPED '%s' -> '%s' (name fallback)",
                    qPrintable(candidateName), qPrintable(nameMatch->name));
            }
            else {
                qDebug("[WalkSettingsList]   UNMATCHED '%s'", qPrintable(candidateName));
            }
        }

        entriesFound++;
        pos = afterName + 24;
    }

    qDebug("[WalkSettingsList] Found %d entries, %d mapped",
        entriesFound, m_instanceAddresses.size());
}

// ============================================================
// GetFieldSize (static)
// ============================================================
int LiveValueReader::getFieldSize(const QString& type)
{
    if (type == "Boolean" || type == "Int8" || type == "UInt8")  return 1;
    if (type == "Int16" || type == "UInt16")                    return 2;
    if (type == "Int32" || type == "UInt32" || type == "Float32") return 4;
    if (type == "Int64" || type == "UInt64" || type == "Float64") return 8;
    if (type == "CString" || type == "String")                    return 8;
    if (type == "PointerRef")                                     return 8;
    if (type == "Guid")                                           return 16;
    if (type == "Vec2")                                           return 8;
    if (type == "Vec3")                                           return 16;
    if (type == "Vec4")                                           return 16;
    if (type == "LinearTransform")                                return 64;
    return -1; // unknown / enum / complex
}

// ============================================================
// GetComplexTypeSize
// ============================================================
int LiveValueReader::getComplexTypeSize(const QString& typeName,
    bool requireFlatValuesOnly) const
{
    if (!m_allTypes) return 0;
    const UITypeItem* ref = nullptr;
    for (const auto& t : *m_allTypes)
        if (t.name == typeName) { ref = &t; break; }
    if (!ref || ref->category == "Enums") return 0;

    if (requireFlatValuesOnly) {
        for (const auto& f : ref->fields)
            if (f.isArray || f.type == "CString" || f.type == "String") return 0;
    }

    int size = 0, maxAlign = 1;
    for (const auto& f : ref->fields) {
        int fSize = getFieldSize(f.type);
        if (fSize == -1) {
            const UITypeItem* nested = nullptr;
            for (const auto& t2 : *m_allTypes)
                if (t2.name == f.type) { nested = &t2; break; }
            if (nested && nested->category == "Enums")
                fSize = 4;
            else if (nested)
                fSize = getComplexTypeSize(f.type, requireFlatValuesOnly);
            else
                fSize = 8;
            if (fSize == 0) return 0;
        }
        int align = qMin(fSize, 8);
        maxAlign = qMax(maxAlign, align);
        if (align > 1) size = (size + align - 1) & ~(align - 1);
        size += fSize;
    }
    if (maxAlign > 1 && size > 0)
        size = (size + maxAlign - 1) & ~(maxAlign - 1);
    return size > 0 ? size : 0;
}

// ============================================================
// ComputeInlineStructSize
// ============================================================
int LiveValueReader::computeInlineStructSize(const QString& typeName, int depth,
    bool requireFlatValuesOnly) const
{
    if (depth > 8 || !m_allTypes) return 0;
    const UITypeItem* ref = nullptr;
    for (const auto& t : *m_allTypes)
        if (t.name == typeName) { ref = &t; break; }
    if (!ref || ref->category == "Enums") return 0;

    if (requireFlatValuesOnly)
        for (const auto& f : ref->fields)
            if (f.isArray || f.type == "CString" || f.type == "String") return 0;

    int size = 0;
    for (const auto& f : ref->fields) {
        int fSize = getFieldSize(f.type);
        if (fSize == -1) {
            const UITypeItem* nested = nullptr;
            for (const auto& t2 : *m_allTypes)
                if (t2.name == f.type) { nested = &t2; break; }
            if (nested && nested->category == "Enums")
                fSize = 4;
            else {
                fSize = computeInlineStructSize(f.type, depth + 1, requireFlatValuesOnly);
                if (fSize == 0) return 0;
            }
        }
        int align = qMin(fSize, 8);
        if (align > 1) size = (size + align - 1) & ~(align - 1);
        size += fSize;
    }
    if (size > 0) size = (size + 7) & ~7;
    return size;
}

// ============================================================
// GetStructNaturalAlignment
// ============================================================
int LiveValueReader::getStructNaturalAlignment(const QString& typeName) const
{
    if (!m_allTypes) return 8;
    const UITypeItem* ref = nullptr;
    for (const auto& t : *m_allTypes)
        if (t.name == typeName) { ref = &t; break; }
    if (!ref) return 8;

    int maxAlign = 1;
    for (const auto& f : ref->fields) {
        if (f.isArray) { maxAlign = 8; break; }
        int fSize = getFieldSize(f.type);
        if (fSize == -1) {
            const UITypeItem* nested = nullptr;
            for (const auto& t2 : *m_allTypes)
                if (t2.name == f.type) { nested = &t2; break; }
            if (nested && nested->category == "Enums")
                fSize = 4;
            else if (nested) {
                fSize = computeInlineStructSize(f.type, 0);
                if (fSize == 0) fSize = 8;
            }
            else fSize = 8;
        }
        int align = qMin(fSize, 8);
        if (align > maxAlign) maxAlign = align;
    }
    return maxAlign;
}

// ============================================================
// ReadScalarFromBuf
// ============================================================
QString LiveValueReader::readScalarFromBuf(const QByteArray& buf, int offset,
    const QString& type) const
{
    if (offset < 0 || offset >= buf.size()) return "?";
    if (type == "Boolean") {
        quint8 v = static_cast<quint8>(buf[offset]);
        return v <= 1 ? (v ? "true" : "false") : QStringLiteral("RAW=%1").arg(v);
    }
    if (type == "Int8")   return QString::number(static_cast<qint8>(buf[offset]));
    if (type == "UInt8")  return QString::number(static_cast<quint8>(buf[offset]));
    if (type == "Int16" && offset + 2 <= buf.size()) return QString::number(readI16(buf, offset));
    if (type == "UInt16" && offset + 2 <= buf.size()) return QString::number(readU16(buf, offset));
    if (type == "Int32" && offset + 4 <= buf.size()) return QString::number(readI32(buf, offset));
    if (type == "UInt32" && offset + 4 <= buf.size()) return QString::number(readU32(buf, offset));
    if (type == "Int64" && offset + 8 <= buf.size()) return QString::number(readI64(buf, offset));
    if (type == "UInt64" && offset + 8 <= buf.size())
        return QString::number(static_cast<quint64>(readI64(buf, offset)));
    if (type == "Float32" && offset + 4 <= buf.size()) {
        float f = readF32(buf, offset);
        return (std::isnan(f) || std::isinf(f)) ? "NaN" : QString::number(f, 'g', 6);
    }
    if (type == "Float64" && offset + 8 <= buf.size()) {
        double d = readF64(buf, offset);
        return (std::isnan(d) || std::isinf(d)) ? "NaN" : QString::number(d, 'g', 10);
    }
    if (type == "Guid" && offset + 16 <= buf.size()) {
        const quint8* p = reinterpret_cast<const quint8*>(buf.constData() + offset);
        return QStringLiteral("%1%2%3%4-%5%6-%7%8-%9%10-%11%12%13%14%15%16")
            .arg(p[0], 2, 16, QChar('0')).arg(p[1], 2, 16, QChar('0'))
            .arg(p[2], 2, 16, QChar('0')).arg(p[3], 2, 16, QChar('0'))
            .arg(p[4], 2, 16, QChar('0')).arg(p[5], 2, 16, QChar('0'))
            .arg(p[6], 2, 16, QChar('0')).arg(p[7], 2, 16, QChar('0'))
            .arg(p[8], 2, 16, QChar('0')).arg(p[9], 2, 16, QChar('0'))
            .arg(p[10], 2, 16, QChar('0')).arg(p[11], 2, 16, QChar('0'))
            .arg(p[12], 2, 16, QChar('0')).arg(p[13], 2, 16, QChar('0'))
            .arg(p[14], 2, 16, QChar('0')).arg(p[15], 2, 16, QChar('0'))
            .toUpper();
    }
    if (offset + 4 <= buf.size())
        return QString::number(readU32(buf, offset));
    return "?";
}

// ============================================================
// IsPlausibleValue
// ============================================================
bool LiveValueReader::isPlausibleValue(const QByteArray& buf, int offset,
    const QString& type) const
{
    int size = getFieldSize(type);
    if (size == -1) {
        if (m_allTypes) {
            for (const auto& t : *m_allTypes) {
                if (t.name == type && t.category == "Enums") {
                    if (offset + 4 > buf.size()) return false;
                    quint32 ev = readU32(buf, offset);
                    for (const auto& f : t.fields)
                        if (f.offset == static_cast<int>(ev)) return true;
                    return ev <= 0xFF;
                }
            }
            for (const auto& t : *m_allTypes) {
                if (t.name == type && t.category != "Enums" && !t.fields.isEmpty()) {
                    const auto& firstSub = t.fields[0];
                    int subSize = getFieldSize(firstSub.type);
                    if (subSize > 0 && offset + subSize <= buf.size())
                        return isPlausibleValue(buf, offset, firstSub.type);
                }
            }
        }
        size = 4;
    }
    if (offset < 0 || offset + size > buf.size()) return false;

    if (type == "Boolean")  return static_cast<quint8>(buf[offset]) <= 1;
    if (type == "Float32") {
        float f = readF32(buf, offset);
        if (std::isnan(f) || std::isinf(f)) return false;
        return f == 0.0f || (std::abs(f) >= 1e-6f && std::abs(f) <= 1e10f);
    }
    if (type == "Int32") {
        qint32 v = readI32(buf, offset);
        return v >= -1000000 && v <= 100000000;
    }
    return true; // UInt32, Int8/16/UInt8/16, default
}

// ============================================================
// ScoreCandidateStart
// ============================================================
int LiveValueReader::scoreCandidateStart(const QByteArray& buf, int startOffset,
    const QVector<UIFieldItem>& fields) const
{
    int score = 0;
    int cursor = startOffset;
    int fieldsToCheck = qMin(fields.size(), 6);
    for (int i = 0; i < fieldsToCheck; ++i) {
        const auto& f = fields[i];
        int fSize = getFieldSize(f.type);
        if (fSize == -1) {
            const UITypeItem* nested = nullptr;
            if (m_allTypes)
                for (const auto& t : *m_allTypes)
                    if (t.name == f.type) { nested = &t; break; }
            if (nested && nested->category == "Enums")
                fSize = 4;
            else {
                fSize = computeInlineStructSize(f.type, 0, true);
                if (fSize == 0) {
                    cursor = (cursor + 7) & ~7;
                    cursor += 8;
                    continue;
                }
            }
        }
        int align = qMin(fSize, 8);
        if (align > 1) cursor = (cursor + align - 1) & ~(align - 1);
        if (cursor + fSize > buf.size()) break;
        if (isPlausibleValue(buf, cursor, f.type)) score++;
        cursor += fSize;
    }
    return score;
}

// ============================================================
// LogSubFields
// ============================================================
void LiveValueReader::logSubFields(const QString& typeName,
    const QByteArray& rawBytes, int depth) const
{
    if (!m_allTypes || depth > 4) return;
    const UITypeItem* ref = nullptr;
    for (const auto& t : *m_allTypes)
        if (t.name == typeName) { ref = &t; break; }
    if (!ref) return;

    QString indent(depth * 2, ' ');
    int subCursor = 0;
    for (const auto& sf : ref->fields) {
        if (sf.isArray) {
            subCursor = (subCursor + 7) & ~7;
            if (subCursor + 8 > rawBytes.size()) break;
            qDebug("[DiscoverFieldOffsets]  %s.List '%s' @ +0x%02X",
                qPrintable(indent), qPrintable(sf.name), subCursor);
            subCursor += 8;
            continue;
        }
        int subSize = getFieldSize(sf.type);
        if (subSize == -1) {
            const UITypeItem* nested = nullptr;
            if (m_allTypes)
                for (const auto& t2 : *m_allTypes)
                    if (t2.name == sf.type) { nested = &t2; break; }
            if (nested && nested->category == "Enums") subSize = 4;
            else { subSize = computeInlineStructSize(sf.type, 0); if (subSize == 0) subSize = 8; }
        }
        int subAlign = qMin(subSize, 8);
        if (subAlign > 1) subCursor = (subCursor + subAlign - 1) & ~(subAlign - 1);
        if (subCursor + subSize > rawBytes.size()) break;

        bool isNestedStruct = getFieldSize(sf.type) == -1 && m_allTypes &&
            [&] { for (const auto& t2 : *m_allTypes)
            if (t2.name == sf.type && t2.category != "Enums") return true;
        return false; }();

        QString subVal = isNestedStruct ? QString() : readScalarFromBuf(rawBytes, subCursor, sf.type);
        QString valPart = subVal.isEmpty() ? QString() : QStringLiteral(" = ") + subVal;
        qDebug("[DiscoverFieldOffsets]  %s.%s '%s' @ +0x%02X%s",
            qPrintable(indent), qPrintable(sf.type), qPrintable(sf.name),
            subCursor, qPrintable(valPart));

        if (isNestedStruct)
            logSubFields(sf.type, rawBytes.mid(subCursor, subSize), depth + 1);

        subCursor += subSize;
    }
}

// ============================================================
// DiscoverFieldOffsetsFromFieldData
// ============================================================
QHash<QString, qint64>
LiveValueReader::discoverFieldOffsetsFromFieldData(qint64 fieldDataBase,
    const UITypeItem& typeItem) const
{
    QHash<QString, qint64> result;
    if (!m_allTypes) return result;

    // ---- Compute buffer size ----
    int requiredBufSize = 0x20;
    for (const auto& f : typeItem.fields) {
        int fSize = getFieldSize(f.type);
        if (fSize == -1) {
            bool isEnum = false;
            for (const auto& t : *m_allTypes)
                if (t.name == f.type && t.category == "Enums") { isEnum = true; break; }
            if (isEnum) fSize = 4;
            else { fSize = computeInlineStructSize(f.type, 0); if (fSize == 0) fSize = 8; }
        }
        int align = qMin(fSize, 8);
        if (align > 1) requiredBufSize = (requiredBufSize + align - 1) & ~(align - 1);
        requiredBufSize += fSize;
    }
    requiredBufSize = qMin((requiredBufSize + 0x40 + 15) & ~15, 0x10000);
    QByteArray buf = m_reader->readBytes(fieldDataBase, requiredBufSize);
    if (buf.isEmpty()) return result;

    // ---- Detect static section ----
    bool isStaticSection = (m_moduleBase > 0 &&
        fieldDataBase >= m_moduleBase && fieldDataBase < m_moduleBase + m_moduleSize);
    if (isStaticSection)
        qDebug("[DiscoverFieldOffsets] %s: fieldData is in static section",
            qPrintable(typeItem.name));

    const QVector<UIFieldItem>& fields = typeItem.fields;

    // ---- Classify interleaved ptr fields ----
    QSet<QString> interleavedPtrFields;
    {
        bool seenCstring = false;
        for (int fi = 0; fi < fields.size(); ++fi) {
            const auto& f = fields[fi];
            if (f.type == "CString" || f.type == "String" || f.isArray) {
                seenCstring = true;
                continue;
            }
            if (!seenCstring) continue;
            int fSize = getFieldSize(f.type);
            bool isEnum = (fSize == -1) && [&] {
                for (const auto& t : *m_allTypes)
                    if (t.name == f.type && t.category == "Enums") return true;
                return false; }();
                if (isEnum || fSize != -1) continue;
                bool cstringAfter = false;
                for (int fj = fi + 1; fj < fields.size(); ++fj)
                    if (fields[fj].type == "CString" || fields[fj].type == "String" ||
                        fields[fj].isArray) {
                        cstringAfter = true; break;
                    }
                if (cstringAfter) interleavedPtrFields.insert(f.name);
        }
    }

    QVector<const UIFieldItem*> cstringFields, scalarFields;
    for (const auto& f : fields) {
        if (f.type == "CString" || f.type == "String" || f.isArray ||
            interleavedPtrFields.contains(f.name))
            cstringFields.append(&f);
        else
            scalarFields.append(&f);
    }

    // STATIC SECTION: walk all fields from offset 0
    if (isStaticSection) {
        int cursor = 0;
        for (const auto& field : fields) {
            if (field.isArray) {
                cursor = (cursor + 7) & ~7;
                if (cursor + 8 > buf.size()) break;
                qint64 ptr = readI64(buf, cursor);
                result[field.name] = cursor;
                qDebug("[DiscoverFieldOffsets]   List '%s' @ +0x%02X -> ptr=0x%llX",
                    qPrintable(field.name), cursor, (unsigned long long)ptr);
                cursor += 8; continue;
            }
            if (field.type == "CString" || field.type == "String") {
                cursor = (cursor + 7) & ~7;
                if (cursor + 8 > buf.size()) break;
                qint64 ptr = readI64(buf, cursor);
                if (!canRead(ptr) && ptr != 0) {
                    result[field.name] = cursor;
                    cursor += 8; continue;
                }
                result[field.name] = cursor;
                QString val = (ptr != 0) ? tryReadAsciiString(ptr, 64) : QString();
                qDebug("[DiscoverFieldOffsets]   CString '%s' @ +0x%02X -> \"%s\"",
                    qPrintable(field.name), cursor, qPrintable(val));
                cursor += 8; continue;
            }
            int size = getFieldSize(field.type);
            if (size == -1) {
                bool isEnum = false;
                for (const auto& t : *m_allTypes)
                    if (t.name == field.type && t.category == "Enums") { isEnum = true; break; }
                if (isEnum) {
                    cursor = (cursor + 3) & ~3;
                    if (cursor + 4 > buf.size()) break;
                    result[field.name] = cursor;
                    quint32 ev = readU32(buf, cursor);
                    qDebug("[DiscoverFieldOffsets]   Enum '%s' @ +0x%02X = %u",
                        qPrintable(field.name), cursor, ev);
                    cursor += 4;
                }
                else {
                    int complexSize = getComplexTypeSize(field.type);
                    if (complexSize > 0) {
                        int structAlign = getStructNaturalAlignment(field.type);
                        if (structAlign > 1) cursor = (cursor + structAlign - 1) & ~(structAlign - 1);
                        if (cursor + complexSize > buf.size()) break;
                        result[field.name] = cursor;
                        logSubFields(field.type, buf.mid(cursor, complexSize), 1);
                        cursor += complexSize;
                    }
                    else {
                        cursor = (cursor + 7) & ~7;
                        if (cursor + 8 > buf.size()) break;
                        qint64 probe = readI64(buf, cursor);
                        result[field.name] = cursor;
                        qDebug("[DiscoverFieldOffsets]   PointerRef '%s' @ +0x%02X = 0x%llX",
                            qPrintable(field.name), cursor, (unsigned long long)probe);
                        cursor += 8;
                    }
                }
                continue;
            }
            int align = qMin(size, 8);
            if (align > 1) cursor = (cursor + align - 1) & ~(align - 1);
            if (cursor + size > buf.size()) break;
            result[field.name] = cursor;
            qDebug("[DiscoverFieldOffsets]   %s '%s' @ +0x%02X = %s",
                qPrintable(field.type), qPrintable(field.name), cursor,
                qPrintable(readScalarFromBuf(buf, cursor, field.type)));
            cursor += size;
        }
        qDebug("[DiscoverFieldOffsets] %s: discovered %d/%d offsets",
            qPrintable(typeItem.name), result.size(), fields.size());
        return result;
    }

    // NON-STATIC (heap) path
    int cstringStart = -1;
    bool firstFieldIsLargeStruct = false;

    if (!cstringFields.isEmpty()) {
        if (std::all_of(cstringFields.begin(), cstringFields.end(),
            [](const UIFieldItem* f) { return f->isArray; })) {
            cstringStart = 0x20;
        }
        else {
            int firstCsIdx = -1;
            for (int i = 0; i < fields.size(); ++i)
                if (&fields[i] == cstringFields[0]) { firstCsIdx = i; break; }

            int expectedPreSize = 0;
            if (firstCsIdx > 0) {
                for (int fi = 0; fi < firstCsIdx; ++fi) {
                    const auto& pf = fields[fi];
                    if (pf.isArray || pf.type == "CString" || pf.type == "String") continue;
                    int pfSize = getFieldSize(pf.type);
                    if (pfSize == -1) { pfSize = computeInlineStructSize(pf.type, 0); if (pfSize == 0) pfSize = 8; }
                    int pfAlign = qMin(pfSize, 8);
                    if (pfAlign > 1) expectedPreSize = (expectedPreSize + pfAlign - 1) & ~(pfAlign - 1);
                    expectedPreSize += pfSize;
                }
                expectedPreSize = (expectedPreSize + 7) & ~7;
            }

            for (int off = 0x10; off + 8 <= qMin(buf.size(), 0x100); off += 8) {
                qint64 ptr = readI64(buf, off);
                if (!canRead(ptr)) continue;
                QString s = tryReadAsciiString(ptr, 256);
                if (s.isNull()) continue;
                if (expectedPreSize > 0 && off < 0x20 + expectedPreSize) {
                    qDebug("[DiscoverFieldOffsets]   skip anchor @ +0x%02X — too early", off);
                    continue;
                }
                cstringStart = off;
                break;
            }
            if (cstringStart == -1) cstringStart = 0x20;
        }
    }
    else {
        if (buf.size() >= 0x20) {
            quint32 dword10 = readU32(buf, 0x10);
            quint32 dword14 = readU32(buf, 0x14);
            quint32 dword18 = readU32(buf, 0x18);
            quint32 dword1C = (buf.size() >= 0x20) ? readU32(buf, 0x1C) : 1;

            bool firstSubFieldPlausibleAt18 = false;
            if (!scalarFields.isEmpty()) {
                int firstSize = getFieldSize(scalarFields[0]->type);
                if (firstSize == -1) {
                    int complexSz = computeInlineStructSize(scalarFields[0]->type, 0);
                    if (complexSz > 16) {
                        firstFieldIsLargeStruct = true;
                        for (const auto& t : *m_allTypes) {
                            if (t.name == scalarFields[0]->type && !t.fields.isEmpty()) {
                                firstSubFieldPlausibleAt18 =
                                    isPlausibleValue(buf, 0x18, t.fields[0].type);
                                break;
                            }
                        }
                    }
                }
            }

            bool hasExtraHeader = false;
            if (dword10 > 0 && dword10 <= 0xFF && dword18 > 0 && dword18 <= 0xFF &&
                (!firstFieldIsLargeStruct || !firstSubFieldPlausibleAt18)) {
                int score18 = scoreCandidateStart(buf, 0x18, [&] {
                    QVector<UIFieldItem> v;
                    for (auto* p : scalarFields) v.append(*p);
                    return v; }());
                int score20 = scoreCandidateStart(buf, 0x20, [&] {
                    QVector<UIFieldItem> v;
                    for (auto* p : scalarFields) v.append(*p);
                    return v; }());
                if (score20 > score18) hasExtraHeader = true;
                else if (score20 == score18) hasExtraHeader = (dword1C == 0);
                else hasExtraHeader = false;
            }

            if (hasExtraHeader) cstringStart = 0x20;
            else if (dword10 > 0 && dword10 <= 0xFF) cstringStart = 0x18;
            else cstringStart = 0x20;
        }
        else {
            cstringStart = 0x20;
        }
    }

    // ---- Pre-CString scalar fields ----
    int firstCsIdxNS = -1;
    if (!cstringFields.isEmpty()) {
        for (int i = 0; i < fields.size(); ++i) {
            if (&fields[i] == cstringFields[0]) { firstCsIdxNS = i; break; }
        }
    }

    int cursorNS = cstringStart;

    if (firstCsIdxNS > 0) {
        // Compute pre-scalar size
        int preSize = 0;
        QVector<const UIFieldItem*> preScalars;
        for (int fi = 0; fi < firstCsIdxNS; ++fi) {
            const auto& pf = fields[fi];
            if (pf.isArray || pf.type == "CString" || pf.type == "String" ||
                interleavedPtrFields.contains(pf.name)) continue;
            int fSize = getFieldSize(pf.type);
            bool isEnum = (fSize == -1) && [&] {
                for (const auto& t : *m_allTypes)
                    if (t.name == pf.type && t.category == "Enums") return true;
                return false; }();
                if (isEnum || fSize != -1) {
                    // primitive scalar or known enum — always include
                    preScalars.append(&pf);
                    continue;
                }
                // Complex type after this point (fSize == -1, not enum)
                int complexSz = getComplexTypeSize(pf.type, true);
                if (complexSz == 0) continue; // has CStrings/Lists — exclude (pure pointer-ref)
                // C#: check if the slot at a conservative pre-guess start looks like a pointer block
                const int preGuessStart = 0x20;
                if (complexSz <= 16 && preGuessStart + complexSz <= buf.size()) {
                    int ptrCount = complexSz / 8, readableCount = 0;
                    for (int pi = 0; pi < ptrCount; ++pi)
                        if (canRead(readI64(buf, preGuessStart + pi * 8))) readableCount++;
                    if (ptrCount > 0 && readableCount == ptrCount) continue; // looks like ptr block — exclude
                }
                preScalars.append(&pf);
        }
        for (auto* ps : preScalars) {
            int psSize = getFieldSize(ps->type); if (psSize == -1) psSize = 8;
            int psAlign = qMin(psSize, 8);
            if (psAlign > 1) preSize = (preSize + psAlign - 1) & ~(psAlign - 1);
            preSize += psSize;
        }
        preSize = (preSize + 7) & ~7;
        int preStart = cstringStart - preSize;
        if (!preScalars.isEmpty() && preStart >= 0x10 && preStart < cstringStart) {
            bool preStartValid = true;
            if (preSize <= 8 && preStart + 8 <= buf.size()) {
                qint64 testVal = readI64(buf, preStart);
                // Determine if the first pre-CString scalar is itself pointer-sized
                const UIFieldItem* firstPreScalar = preScalars.isEmpty() ? nullptr : preScalars[0];
                bool firstFieldIsPointerSized = false;
                if (firstPreScalar) {
                    int fps = getFieldSize(firstPreScalar->type);
                    if (fps == -1 && getComplexTypeSize(firstPreScalar->type, true) == 0)
                        firstFieldIsPointerSized = true;
                }
                if (!firstFieldIsPointerSized && canRead(testVal) && testVal > 0x100000000000LL)
                    preStartValid = false;
            }
            if (preStartValid) {
                int preCursor = preStart;
                for (auto* ps : preScalars) {
                    int psSize = getFieldSize(ps->type); if (psSize == -1) psSize = 8;
                    int psAlign = qMin(psSize, 8);
                    if (psAlign > 1) preCursor = (preCursor + psAlign - 1) & ~(psAlign - 1);
                    if (preCursor + psSize > buf.size()) break;
                    result[ps->name] = preCursor;
                    qDebug("[DiscoverFieldOffsets]   %s '%s' @ +0x%02X = %s (pre-CString)",
                        qPrintable(ps->type), qPrintable(ps->name), preCursor,
                        qPrintable(readScalarFromBuf(buf, preCursor, ps->type)));
                    preCursor += psSize;
                }
            }
        }

        // Pre-CString complex ptr fields
        QVector<const UIFieldItem*> preComplexPtrs;
        for (int fi = 0; fi < firstCsIdxNS; ++fi) {
            const auto& pf = fields[fi];
            if (pf.isArray || pf.type == "CString" || pf.type == "String" ||
                interleavedPtrFields.contains(pf.name) || result.contains(pf.name)) continue;
            int fSize = getFieldSize(pf.type);
            if (fSize != -1) continue;
            bool isEnum = false;
            for (const auto& t : *m_allTypes)
                if (t.name == pf.type && t.category == "Enums") { isEnum = true; break; }
            if (!isEnum) preComplexPtrs.append(&pf);
        }
        if (!preComplexPtrs.isEmpty()) {
            int prePtrCursor = cstringStart - preComplexPtrs.size() * 8;
            if (prePtrCursor < 0x10) prePtrCursor = cstringStart;
            for (auto* f2 : preComplexPtrs) {
                if (prePtrCursor + 8 > buf.size()) break;
                qint64 ptr = readI64(buf, prePtrCursor);
                result[f2->name] = prePtrCursor;
                qDebug("[DiscoverFieldOffsets]   PointerRef '%s' @ +0x%02X = 0x%llX (pre-CString ptr)",
                    qPrintable(f2->name), prePtrCursor, (unsigned long long)ptr);
                prePtrCursor += 8;
            }
        }
    }

    // ---- Map CStrings sequentially ----
    for (int i = 0; i < cstringFields.size(); ++i) {
        if (cursorNS + 8 > buf.size()) break;
        qint64 ptr = readI64(buf, cursorNS);

        if (cstringFields[i]->isArray) {
            result[cstringFields[i]->name] = cursorNS;
            qDebug("[DiscoverFieldOffsets]   List '%s' @ +0x%02X -> ptr=0x%llX",
                qPrintable(cstringFields[i]->name), cursorNS, (unsigned long long)ptr);
            cursorNS += 8; continue;
        }
        if (interleavedPtrFields.contains(cstringFields[i]->name)) {
            result[cstringFields[i]->name] = cursorNS;
            qDebug("[DiscoverFieldOffsets]   PointerRef '%s' @ +0x%02X = 0x%llX",
                qPrintable(cstringFields[i]->name), cursorNS, (unsigned long long)ptr);
            cursorNS += 8; continue;
        }
        if (!canRead(ptr) && ptr != 0) {
            cursorNS += 8;
            i--;
            if (cursorNS + 8 > buf.size()) break;
            continue;
        }
        result[cstringFields[i]->name] = cursorNS;
        QString val = (ptr != 0) ? (tryReadAsciiString(ptr, 64)) : QString();
        qDebug("[DiscoverFieldOffsets]   CString '%s' @ +0x%02X -> \"%s\"",
            qPrintable(cstringFields[i]->name), cursorNS, qPrintable(val));
        cursorNS += 8;
    }

    // ---- Align to scalar block ----
    cursorNS = (cursorNS + 7) & ~7;

    // ---- Skip trailing pointer slots ----
    {
        const UIFieldItem* firstUnassigned = nullptr;
        for (auto* f2 : scalarFields)
            if (!result.contains(f2->name)) { firstUnassigned = f2; break; }

        while (cursorNS + 8 <= buf.size() && firstUnassigned) {
            qint64 val = readI64(buf, cursorNS);
            if (!canRead(val)) break;
            quint32 loDword = static_cast<quint32>(val & 0xFFFFFFFF);
            quint32 hiDword = static_cast<quint32>(static_cast<quint64>(val) >> 32);
            if (loDword < 0x40000000u && hiDword < 0x40000000u) break;
            const QString& fType = firstUnassigned->type;
            if (fType != "UInt32" && fType != "Int32" &&
                isPlausibleValue(buf, cursorNS, fType)) break;
            qDebug("[DiscoverFieldOffsets]   skip trailing ptr slot @ +0x%02X = 0x%llX",
                cursorNS, (unsigned long long)val);
            cursorNS += 8;
        }
    }

    if (cstringFields.isEmpty())
        cursorNS = cstringStart;

    qDebug("[DiscoverFieldOffsets] %s: scalar block starts at +0x%02X",
        qPrintable(typeItem.name), cursorNS);

    // ---- Scalar block: walk in order with natural alignment ----
    for (auto* field : scalarFields) {
        if (result.contains(field->name)) continue;
        int size = getFieldSize(field->type);

        if (size == -1) {
            int alignedCursor = (cursorNS + 7) & ~7;
            if (alignedCursor + 8 > buf.size()) continue;

            bool isEnum = false;
            const UITypeItem* referencedType = nullptr;
            for (const auto& t : *m_allTypes) {
                if (t.name == field->type) {
                    referencedType = &t;
                    isEnum = (t.category == "Enums");
                    break;
                }
            }

            if (isEnum) {
                int enumOffset = (cursorNS + 3) & ~3;
                if (enumOffset + 4 <= buf.size()) {
                    result[field->name] = enumOffset;
                    quint32 ev = readU32(buf, enumOffset);
                    qDebug("[DiscoverFieldOffsets]   Enum '%s' @ +0x%02X = %u",
                        qPrintable(field->name), enumOffset, ev);
                    cursorNS = enumOffset + 4;
                }
                continue;
            }

            // Complex type (not enum)
            int complexSize = getComplexTypeSize(field->type, true);
            int complexSizeUR = complexSize > 0 ? complexSize
                : getComplexTypeSize(field->type, false);

            if (complexSizeUR > 0) {
                int structStartAlign = getStructNaturalAlignment(field->type);
                int alignedCursorForStruct = (structStartAlign > 1)
                    ? (cursorNS + structStartAlign - 1) & ~(structStartAlign - 1)
                    : cursorNS;

                bool looksLikePointerBlock = false;
                if (complexSize > 0 && complexSizeUR <= 16 &&
                    alignedCursorForStruct + complexSizeUR <= buf.size()) {
                    int ptrCount = complexSizeUR / 8, readableCount = 0;
                    for (int pi = 0; pi < ptrCount; ++pi)
                        if (canRead(readI64(buf, alignedCursorForStruct + pi * 8))) readableCount++;
                    looksLikePointerBlock = (ptrCount > 0 && readableCount == ptrCount);
                }

                bool slotIsNullOrScalar = false;
                if (complexSize == 0 && alignedCursorForStruct + 8 <= buf.size()) {
                    qint64 slotVal = readI64(buf, alignedCursorForStruct);
                    quint32 slotLo = static_cast<quint32>(slotVal & 0xFFFFFFFF);
                    quint32 slotHi = static_cast<quint32>(static_cast<quint64>(slotVal) >> 32);
                    bool slotLooksLikePointer = slotLo >= 0x40000000u || slotHi >= 0x40000000u;
                    bool isFirstScalarField = !scalarFields.isEmpty() &&
                        [&] { for (auto* f2 : scalarFields) if (!result.contains(f2->name)) return f2 == field; return false; }();
                    slotIsNullOrScalar = !(slotLooksLikePointer && isFirstScalarField && firstFieldIsLargeStruct);
                }

                if (!looksLikePointerBlock && !slotIsNullOrScalar &&
                    alignedCursorForStruct + complexSizeUR <= buf.size()) {
                    result[field->name] = alignedCursorForStruct;
                    logSubFields(field->type, buf.mid(alignedCursorForStruct, complexSizeUR), 1);
                    cursorNS = alignedCursorForStruct + complexSizeUR;

                    // Probe for next field alignment gap
                    const UIFieldItem* nextUnmapped = nullptr;
                    for (auto* f2 : scalarFields)
                        if (!result.contains(f2->name)) { nextUnmapped = f2; break; }
                    if (nextUnmapped) {
                        int nextFSize = getFieldSize(nextUnmapped->type);
                        if (nextFSize == -1) {
                            int cs = computeInlineStructSize(nextUnmapped->type, 0, true);
                            nextFSize = cs > 0 ? cs : 4;
                        }
                        if (nextFSize > 0 && nextFSize <= 8) {
                            for (int probe = 0; probe <= 8; probe += 4) {
                                int testCursor = cursorNS + probe;
                                if (testCursor + nextFSize > buf.size()) break;
                                if (isPlausibleValue(buf, testCursor, nextUnmapped->type)) {
                                    cursorNS = testCursor; break;
                                }
                            }
                        }
                    }
                    continue;
                }
                // looksLikePointerBlock or slotIsNullOrScalar — fall through to unified handler below
            }

            // Unified pointer-ref / forced-inline handler
            alignedCursor = (cursorNS + 7) & ~7;
            if (alignedCursor + 8 > buf.size()) continue;
            {
                qint64 probe = readI64(buf, alignedCursor);
                quint32 loDword = static_cast<quint32>(probe & 0xFFFFFFFF);
                quint32 hiDword = static_cast<quint32>(static_cast<quint64>(probe) >> 32);
                bool looksLikePointer = (loDword >= 0x40000000u || hiDword >= 0x40000000u);

                int flatSize = getComplexTypeSize(field->type, true);
                int actualSize = flatSize > 0 ? flatSize : getComplexTypeSize(field->type, false);
                bool isByRefType = (flatSize == 0 && actualSize > 0);

                if (looksLikePointer) {
                    result[field->name] = alignedCursor;
                    qDebug("[DiscoverFieldOffsets]   PointerRef '%s' @ +0x%02X = 0x%llX",
                        qPrintable(field->name), alignedCursor, (unsigned long long)probe);
                    cursorNS = alignedCursor + 8;
                }
                else if (isByRefType) {
                    result[field->name] = alignedCursor;
                    qDebug("[DiscoverFieldOffsets]   PointerRef(byref) '%s' @ +0x%02X = 0x%llX",
                        qPrintable(field->name), alignedCursor, (unsigned long long)probe);
                    cursorNS = alignedCursor + 8;
                }
                else if (actualSize > 0 && alignedCursor + actualSize <= buf.size()) {
                    result[field->name] = alignedCursor;
                    qDebug("[DiscoverFieldOffsets]   InlineStruct(forced) '%s' @ +0x%02X size=%d",
                        qPrintable(field->name), alignedCursor, actualSize);
                    logSubFields(field->type, buf.mid(alignedCursor, actualSize), 1);
                    cursorNS = alignedCursor + actualSize;
                }
                else {
                    result[field->name] = alignedCursor;
                    qDebug("[DiscoverFieldOffsets]   UnknownScalarBlock '%s' @ +0x%02X = 0x%llX (advancing 8)",
                        qPrintable(field->name), alignedCursor, (unsigned long long)probe);
                    cursorNS = alignedCursor + 8;
                }
            }
            continue;
        }

        int alignS = qMin(size, 8);
        if (alignS > 1) cursorNS = (cursorNS + alignS - 1) & ~(alignS - 1);
        if (cursorNS + size > buf.size()) break;
        result[field->name] = cursorNS;
        qDebug("[DiscoverFieldOffsets]   %s '%s' @ +0x%02X = %s",
            qPrintable(field->type), qPrintable(field->name), cursorNS,
            qPrintable(readScalarFromBuf(buf, cursorNS, field->type)));
        cursorNS += size;
    }

    qDebug("[DiscoverFieldOffsets] %s: discovered %d/%d offsets",
        qPrintable(typeItem.name), result.size(), fields.size());
    return result;
}

// ============================================================
// GetFieldDataBase
// ============================================================
qint64 LiveValueReader::getFieldDataBase(qint64 instanceBase) const
{
    // Find type name for this instance
    QString typeName;
    for (auto it = m_instanceAddresses.begin(); it != m_instanceAddresses.end(); ++it)
        if (it.value() == instanceBase) { typeName = it.key(); break; }

    if (!typeName.isEmpty() && m_settingsListFieldData.contains(typeName)) {
        qint64 cached = m_settingsListFieldData.value(typeName);
        if (!m_discoveredOffsets.contains(typeName) && m_allTypes) {
            for (const auto& t : *m_allTypes) {
                if (t.name == typeName) {
                    auto offsets = discoverFieldOffsetsFromFieldData(cached, t);
                    if (!offsets.isEmpty())
                        const_cast<LiveValueReader*>(this)->m_discoveredOffsets[typeName] = offsets;
                    break;
                }
            }
        }
        return cached;
    }

    // Bag-of-pointers walk
    QByteArray bagBuf = m_reader->readBytes(instanceBase + 0x28, 8);
    if (bagBuf.isEmpty()) return 0;
    qint64 bagPtr = readI64(bagBuf, 0);
    if (!canRead(bagPtr)) return 0;

    QByteArray hopBuf = m_reader->readBytes(bagPtr, 0x88);
    if (hopBuf.isEmpty()) return 0;

    for (int hop = 0; hop <= 0x80; hop += 8) {
        if (hop + 8 > hopBuf.size()) break;
        qint64 candidate = readI64(hopBuf, hop);
        if (!canRead(candidate) || candidate == instanceBase) continue;

        QByteArray backBuf = m_reader->readBytes(candidate + 0x08, 8);
        if (backBuf.isEmpty()) continue;
        qint64 backPtr = readI64(backBuf, 0);
        if (backPtr != instanceBase) continue;

        qDebug("[GetFieldDataBase] 0x%llX -> bag+0x%02X -> 0x%llX",
            (unsigned long long)instanceBase, hop, (unsigned long long)candidate);

        if (!typeName.isEmpty() && !m_discoveredOffsets.contains(typeName) && m_allTypes) {
            for (const auto& t : *m_allTypes) {
                if (t.name == typeName) {
                    auto offsets = discoverFieldOffsetsFromFieldData(candidate, t);
                    if (!offsets.isEmpty())
                        const_cast<LiveValueReader*>(this)->m_discoveredOffsets[typeName] = offsets;
                    break;
                }
            }
        }
        return candidate;
    }
    return 0;
}

// ============================================================
// ValidateInstance
// ============================================================
bool LiveValueReader::validateInstance(qint64 candidateBase, const QString& typeName) const
{
    if (candidateBase < 0x100000000LL) return false;
    QByteArray chk = m_reader->readBytes(candidateBase + 0x10, 8);
    if (chk.isEmpty() || readI64(chk, 0) == 0) return false;

    if (!m_allTypes) return true;
    const UITypeItem* typeItem = nullptr;
    for (const auto& t : *m_allTypes)
        if (t.name == typeName) { typeItem = &t; break; }
    if (!typeItem) return true;

    for (const auto& f : typeItem->fields) {
        if (f.type == "Boolean" && f.offset >= 0 && f.offset < 200) {
            QByteArray b = m_reader->readBytes(candidateBase + f.offset, 1);
            return !b.isEmpty() && static_cast<quint8>(b[0]) <= 1;
        }
    }
    for (const auto& f : typeItem->fields) {
        if (f.type == "UInt32" && f.offset >= 0 && f.offset < 500) {
            QByteArray b = m_reader->readBytes(candidateBase + f.offset, 4);
            return !b.isEmpty() && readU32(b, 0) < 100000;
        }
    }
    return true;
}

// ============================================================
// VerifyInstances
// ============================================================
void LiveValueReader::verifyInstances()
{
    static const QStringList kVerifyTypes = {
        "BugSentrySettings", "UISettings", "InputSettings", "ClientSettings"
    };
    for (const QString& typeName : kVerifyTypes) {
        if (!m_instanceAddresses.contains(typeName)) {
            qDebug("[VerifyInstances] %s: NOT FOUND", qPrintable(typeName));
            continue;
        }
        qint64 instBase = m_instanceAddresses.value(typeName);
        if (!m_allTypes) continue;
        const UITypeItem* typeItem = nullptr;
        for (const auto& t : *m_allTypes)
            if (t.name == typeName) { typeItem = &t; break; }
        if (!typeItem) continue;

        qint64 fieldDataBase = getFieldDataBase(instBase);
        qDebug("[VerifyInstances] ===== %s @ 0x%llX fieldData=0x%llX =====",
            qPrintable(typeName), (unsigned long long)instBase,
            (unsigned long long)fieldDataBase);

        // Sort fields by offset for display
        QVector<const UIFieldItem*> sortedFields;
        for (const auto& f : typeItem->fields) sortedFields.append(&f);
        std::sort(sortedFields.begin(), sortedFields.end(),
            [](const UIFieldItem* a, const UIFieldItem* b) { return a->offset < b->offset; });

        for (const auto* field : sortedFields) {
            QString val = field->isArray
                ? readListValue(instBase, *field)
                : (readFieldValue(instBase, *field, typeName));
            if (val.isNull()) val = "(skipped)";
            qDebug("  %d  %s  %s  %s",
                field->offset, qPrintable(field->type),
                qPrintable(field->name), qPrintable(val));
        }
        qDebug("[VerifyInstances] ===== END %s =====", qPrintable(typeName));
    }
}

// ============================================================
// ScanInstances
// ============================================================
void LiveValueReader::scanInstances(
    const QHash<qint64, QString>& typeInfoToName,
    std::function<void(const QString&)> statusCallback,
    std::function<bool()> cancelCheck)
{
    m_instanceAddresses.clear();
    m_discoveredOffsets.clear();
    m_settingsListFieldData.clear();
    m_settingsListCandidateNames.clear();
    m_scanned = false;

    // Constrain scan to module heap range. Game instances live close to the module
    qint64 addr = qMax(0x100000000LL, m_moduleBase - 0x10000000LL);
    qint64 maxAddr = qMin(0x7FFFFFFFFFFFL, m_moduleBase + m_moduleSize + 0x80000000LL);
    int found = 0, regionsScanned = 0;

    QVector<qint64> originCandidates;
    QHash<qint64, QString> instanceToName;

    while (addr < maxAddr) {
        if (cancelCheck()) break;

        MemBasicInfo mbi;
        if (!virtualQueryRegion(addr, mbi)) { addr += 0x1000; continue; }

        qint64 regionBase = mbi.baseAddress;
        qint64 regionSize = mbi.regionSize;
        qint64 regionEnd = regionBase + regionSize;

        bool isCommitted = (mbi.state & 0x1000) != 0;
        bool isReadable = (mbi.protect & 0x01) == 0 && (mbi.protect & 0x100) == 0;

        // Skip non-committed, non-readable, zero-size, or enormous mapped-file regions
        if (!isCommitted || !isReadable || regionSize == 0 || regionSize > 0x10000000LL) {
            addr = regionEnd > addr ? regionEnd : addr + 0x1000;
            continue;
        }

        regionsScanned++;
        const qint64 CHUNK = 4LL * 1024 * 1024;

        bool cancelled = false;
        for (qint64 pos = regionBase; pos < regionEnd - 8 && !cancelled; pos += CHUNK) {
            if (cancelCheck()) { cancelled = true; break; }

            qint64 chunkEnd = qMin(pos + CHUNK, regionEnd);
            int    chunkLen = static_cast<int>(chunkEnd - pos);
            QByteArray buf = m_reader->readBytes(pos, chunkLen);
            if (buf.isEmpty()) continue;

            for (qint64 off = 0; off <= chunkLen - 8; off += 8) {
                qint64 candidateBase = pos + off;
                // Alignment check is cheaper than a hash lookup — do it first
                if ((candidateBase & 0x7) != 0) continue;

                qint64 val = readI64(buf, static_cast<int>(off));
                if (val == 0) continue;

                auto it = typeInfoToName.find(val);
                if (it == typeInfoToName.end()) continue;
                const QString& typeName = it.value();

                if (typeName == "OriginSettings") {
                    originCandidates.append(candidateBase);
                    continue;
                }

                if (m_instanceAddresses.contains(typeName)) continue;
                if (!validateInstance(candidateBase, typeName)) continue;

                m_instanceAddresses[typeName] = candidateBase;
                instanceToName[candidateBase] = typeName;
                found++;
            }
        }
        if (cancelled) break;

        addr = regionEnd > addr ? regionEnd : addr + 0x1000;
        if (regionsScanned % 50 == 0)
            statusCallback(QStringLiteral("Scanning memory regions... %1 instances found").arg(found));
    }

    // OriginSettings: pick the best candidate
    if (!originCandidates.isEmpty()) {
        qint64 best = 0;
        for (qint64 candidate : originCandidates) {
            QByteArray chk = m_reader->readBytes(candidate + 0x10, 8);
            if (chk.isEmpty()) continue;
            if (readI64(chk, 0) == 0) continue;
            best = candidate; break;
        }
        qint64 originAddr = (best != 0) ? best : originCandidates[0];
        m_instanceAddresses["OriginSettings"] = originAddr;
        instanceToName[originAddr] = "OriginSettings";
    }

    // Walk settings lists
    qint64 listBase = findSettingsListBase();
    if (listBase != 0) walkSettingsList(listBase, instanceToName);

    qint64 listBase2 = findSettingsListBaseSentinel();
    if (listBase2 != 0) {
        qDebug("[ScanInstances] Walking secondary settings list at 0x%llX",
            (unsigned long long)listBase2);
        walkSettingsList(listBase2, instanceToName);
    }

    m_scanned = true;
    qDebug("[ScanInstances] Complete: %d instances in %d regions", found, regionsScanned);
    verifyInstances();
}

// ============================================================
// ReadAtOffset
// ============================================================
QString LiveValueReader::readAtOffset(qint64 fieldDataBase, const UIFieldItem& field,
    qint64 byteOffset) const
{
    qint64 fieldAddr = fieldDataBase + byteOffset;
    if (!canRead(fieldAddr)) return {};

    const QString& type = field.type;

    if (type == "Boolean") {
        QByteArray b = m_reader->readBytes(fieldAddr, 1);
        if (b.isEmpty() || static_cast<quint8>(b[0]) > 1) return {};
        return b[0] ? "true" : "false";
    }
    if (type == "Int8") {
        QByteArray b = m_reader->readBytes(fieldAddr, 1);
        return b.isEmpty() ? QString() : QString::number(static_cast<qint8>(b[0]));
    }
    if (type == "UInt8") {
        QByteArray b = m_reader->readBytes(fieldAddr, 1);
        return b.isEmpty() ? QString() : QString::number(static_cast<quint8>(b[0]));
    }
    if (type == "Int16") {
        QByteArray b = m_reader->readBytes(fieldAddr, 2);
        return b.isEmpty() ? QString() : QString::number(readI16(b, 0));
    }
    if (type == "UInt16") {
        QByteArray b = m_reader->readBytes(fieldAddr, 2);
        return b.isEmpty() ? QString() : QString::number(readU16(b, 0));
    }
    if (type == "Int32") {
        QByteArray b = m_reader->readBytes(fieldAddr, 4);
        return b.isEmpty() ? QString() : QString::number(readI32(b, 0));
    }
    if (type == "UInt32") {
        QByteArray b = m_reader->readBytes(fieldAddr, 4);
        return b.isEmpty() ? QString() : QString::number(readU32(b, 0));
    }
    if (type == "Int64") {
        QByteArray b = m_reader->readBytes(fieldAddr, 8);
        return b.isEmpty() ? QString() : QString::number(readI64(b, 0));
    }
    if (type == "UInt64") {
        QByteArray b = m_reader->readBytes(fieldAddr, 8);
        return b.isEmpty() ? QString() : QString::number(static_cast<quint64>(readI64(b, 0)));
    }
    if (type == "Float32") {
        QByteArray b = m_reader->readBytes(fieldAddr, 4);
        if (b.isEmpty()) return {};
        float f = readF32(b, 0);
        return (std::isnan(f) || std::isinf(f)) ? QString() : QString::number(f, 'g', 6);
    }
    if (type == "Float64") {
        QByteArray b = m_reader->readBytes(fieldAddr, 8);
        if (b.isEmpty()) return {};
        double d = readF64(b, 0);
        return (std::isnan(d) || std::isinf(d)) ? QString() : QString::number(d, 'g', 10);
    }
    if (type == "CString" || type == "String") {
        QByteArray ptrBuf = m_reader->readBytes(fieldAddr, 8);
        if (ptrBuf.isEmpty()) return QStringLiteral("\"\"");
        qint64 strPtr = readI64(ptrBuf, 0);
        if (!canRead(strPtr)) return QStringLiteral("\"\"");
        QString s = tryReadAsciiString(strPtr, 512);
        return s.isEmpty() ? QStringLiteral("\"\"") : QStringLiteral("\"%1\"").arg(s);
    }
    if (type == "Guid") {
        QByteArray b = m_reader->readBytes(fieldAddr, 16);
        if (b.isEmpty()) return {};
        const quint8* p = reinterpret_cast<const quint8*>(b.constData());
        return QStringLiteral("%1%2%3%4-%5%6-%7%8-%9%10-%11%12%13%14%15%16")
            .arg(p[0], 2, 16, QChar('0')).arg(p[1], 2, 16, QChar('0'))
            .arg(p[2], 2, 16, QChar('0')).arg(p[3], 2, 16, QChar('0'))
            .arg(p[4], 2, 16, QChar('0')).arg(p[5], 2, 16, QChar('0'))
            .arg(p[6], 2, 16, QChar('0')).arg(p[7], 2, 16, QChar('0'))
            .arg(p[8], 2, 16, QChar('0')).arg(p[9], 2, 16, QChar('0'))
            .arg(p[10], 2, 16, QChar('0')).arg(p[11], 2, 16, QChar('0'))
            .arg(p[12], 2, 16, QChar('0')).arg(p[13], 2, 16, QChar('0'))
            .arg(p[14], 2, 16, QChar('0')).arg(p[15], 2, 16, QChar('0'))
            .toUpper();
    }

    // Inline struct / enum / unknown
    if (type == "Vec2") {
        QByteArray b = m_reader->readBytes(fieldAddr, 8);
        if (b.isEmpty() || b.size() < 8) return {};
        float x = readF32(b, 0), y = readF32(b, 4);
        if (std::isnan(x) || std::isnan(y)) return {};
        return QStringLiteral("(%1,%2)").arg(x, 0, 'g', 6).arg(y, 0, 'g', 6);
    }
    if (type == "Vec3") {
        QByteArray b = m_reader->readBytes(fieldAddr, 12);
        if (b.isEmpty() || b.size() < 12) return {};
        float x = readF32(b, 0), y = readF32(b, 4), z = readF32(b, 8);
        if (std::isnan(x) || std::isnan(y) || std::isnan(z)) return {};
        return QStringLiteral("(%1,%2,%3)").arg(x, 0, 'g', 6).arg(y, 0, 'g', 6).arg(z, 0, 'g', 6);
    }
    if (type == "Vec4") {
        QByteArray b = m_reader->readBytes(fieldAddr, 16);
        if (b.isEmpty() || b.size() < 16) return {};
        float x = readF32(b, 0), y = readF32(b, 4), z = readF32(b, 8), w = readF32(b, 12);
        if (std::isnan(x) || std::isnan(y) || std::isnan(z) || std::isnan(w)) return {};
        return QStringLiteral("(%1,%2,%3,%4)").arg(x, 0, 'g', 6).arg(y, 0, 'g', 6).arg(z, 0, 'g', 6).arg(w, 0, 'g', 6);
    }
    if (type == "LinearTransform") {
        // 4 rows of Vec4 (float4 each) = 64 bytes
        QByteArray b = m_reader->readBytes(fieldAddr, 64);
        if (b.isEmpty() || b.size() < 64) return {};
        QStringList rows;
        for (int r = 0; r < 4; ++r) {
            float x = readF32(b, r * 16), y = readF32(b, r * 16 + 4),
                z = readF32(b, r * 16 + 8), w = readF32(b, r * 16 + 12);
            rows.append(QStringLiteral("(%1,%2,%3,%4)").arg(x, 0, 'g', 6).arg(y, 0, 'g', 6).arg(z, 0, 'g', 6).arg(w, 0, 'g', 6));
        }
        return QStringLiteral("[%1]").arg(rows.join(";"));
    }

    if (m_allTypes) {
        // Check for known inline struct (non-enum)
        for (const auto& refType : *m_allTypes) {
            if (refType.name == type && refType.category != "Enums") {
                int structSize = getComplexTypeSize(type);
                if (structSize > 0) {
                    QByteArray structBuf = m_reader->readBytes(fieldAddr, structSize);
                    if (!structBuf.isEmpty()) {
                        // Build sub-entries
                        struct KV { QString key, value; };
                        QVector<KV> subEntries;
                        int subCursor = 0;
                        for (const auto& subField : refType.fields) {
                            if (subField.isArray) continue;
                            int subSize = getFieldSize(subField.type);
                            if (subSize == -1) subSize = 8;
                            int subAlign = qMin(subSize, 8);
                            if (subAlign > 1) subCursor = (subCursor + subAlign - 1) & ~(subAlign - 1);
                            if (subCursor + subSize > structBuf.size()) break;
                            QString subVal = readScalarFromBuf(structBuf, subCursor, subField.type);
                            subEntries.append({ subField.name, subVal.isEmpty() ? "?" : subVal });
                            subCursor += subSize;
                        }

                        // Join
                        QString joined;
                        if (type == "Vec3") {
                            auto get = [&](const QString& k) -> QString {
                                for (const auto& e : subEntries) if (e.key == k) return e.value;
                                return "?";
                                };
                            joined = get("x") + "," + get("y") + "," + get("z");
                        }
                        else if (type == "Vec4") {
                            auto get = [&](const QString& k) -> QString {
                                for (const auto& e : subEntries) if (e.key == k) return e.value;
                                return "?";
                                };
                            joined = get("x") + "," + get("y") + "," + get("z") + "," + get("w");
                        }
                        else {
                            std::sort(subEntries.begin(), subEntries.end(),
                                [&](const KV& a, const KV& b) {
                                    // Find the subfield sizes for a and b
                                    auto findSize = [&](const QString& k) -> int {
                                        for (const auto& sf : refType.fields)
                                            if (sf.name == k) return getFieldSize(sf.type);
                                        return 4;
                                        };
                                    int sa = findSize(a.key);
                                    int sb = findSize(b.key);
                                    int ra = (sa == 1) ? 0 : 1;
                                    int rb = (sb == 1) ? 0 : 1;
                                    if (ra != rb) return ra < rb;
                                    return a.key > b.key; // descending by name
                                });
                            QStringList vals;
                            for (const auto& e : subEntries) vals.append(e.value);
                            joined = vals.join(",");
                        }
                        return QStringLiteral("(%1)").arg(joined);
                    }
                }
            }
        }

        // Enum
        for (const auto& refType : *m_allTypes) {
            if (refType.name == type && refType.category == "Enums") {
                QByteArray b = m_reader->readBytes(fieldAddr, 4);
                if (b.isEmpty()) return {};
                quint32 enumVal = readU32(b, 0);
                for (const auto& ef : refType.fields)
                    if (ef.offset == static_cast<int>(enumVal)) return ef.name;
                return QStringLiteral("%1(%2)").arg(type).arg(enumVal);
            }
        }
    }

    // Unknown scalar fallback
    QByteArray b = m_reader->readBytes(fieldAddr, 4);
    if (b.isEmpty()) return {};
    quint32 v = readU32(b, 0);
    return v < 100000 ? QString::number(v) : QString();
}

// ============================================================
// ReadFieldValue (public)
// ============================================================
QString LiveValueReader::readFieldValue(qint64 instanceBase, const UIFieldItem& field,
    const QString& typeName) const
{
    QString tname = typeName;
    if (tname.isEmpty())
        for (auto it = m_instanceAddresses.begin(); it != m_instanceAddresses.end(); ++it)
            if (it.value() == instanceBase) { tname = it.key(); break; }

    // Fast path: bulk buffer was primed for this type — decode directly from it
    static const QSet<QString> kFlatScalars = {
            "Boolean","Int8","UInt8","Int16","UInt16",
            "Int32","UInt32","Int64","UInt64","Float32","Float64"
    };
    if (!tname.isEmpty()
        && tname == m_cachedTypeName
        && m_cachedFieldDataBase != 0
        && !m_cachedFieldBuf.isEmpty()
        && m_discoveredOffsets.contains(tname))
    {
        const auto& offMap = m_discoveredOffsets.value(tname);
        auto it = offMap.find(field.name);
        if (it != offMap.end()) {
            int off = static_cast<int>(it.value());
            // Flat scalar — decode directly from buffer, zero extra RPM calls
            if (kFlatScalars.contains(field.type)) {
                if (off >= 0 && off + getFieldSize(field.type) <= m_cachedFieldBuf.size())
                    return readScalarFromBuf(m_cachedFieldBuf, off, field.type);
                return {};
            }
            if (field.type == "CString" || field.type == "String") {
                if (off >= 0 && off + 8 <= m_cachedFieldBuf.size()) {
                    qint64 strPtr = readI64(m_cachedFieldBuf, off);
                    if (!canRead(strPtr)) return QStringLiteral("\"\"");
                    QString s = tryReadAsciiString(strPtr, 512);
                    return s.isEmpty() ? QStringLiteral("\"\"") : QStringLiteral("\"%1\"").arg(s);
                }
                return QStringLiteral("\"\"");
            }
        }
        // Field not in offset map or unhandled type — fall through to readAtOffset
    }

    qint64 fieldDataBase = getFieldDataBase(instanceBase);
    if (fieldDataBase == 0) return {};

    if (!tname.isEmpty() && m_discoveredOffsets.contains(tname)) {
        const auto& offMap = m_discoveredOffsets.value(tname);
        if (offMap.contains(field.name))
            return readAtOffset(fieldDataBase, field, offMap.value(field.name));
    }
    return {};
}

// ============================================================
// ReadListValue (public)
// ============================================================
QString LiveValueReader::readListValue(qint64 instanceBase, const UIFieldItem& field) const
{
    qDebug("[ReadListValue] field='%s' type='%s' arrayElemType='%s' isArray=%d",
        qPrintable(field.name), qPrintable(field.type),
        qPrintable(field.arrayElemType), field.isArray);

    QString typeName;
    for (auto it = m_instanceAddresses.begin(); it != m_instanceAddresses.end(); ++it)
        if (it.value() == instanceBase) { typeName = it.key(); break; }

    qint64 fieldDataBase = getFieldDataBase(instanceBase);
    if (fieldDataBase == 0) return "[]";

    qint64 byteOffset = field.offset;
    if (!typeName.isEmpty() && m_discoveredOffsets.contains(typeName)) {
        const auto& offMap = m_discoveredOffsets.value(typeName);
        if (offMap.contains(field.name)) byteOffset = offMap.value(field.name);
    }

    qint64 fieldAddr = fieldDataBase + byteOffset;
    if (!canRead(fieldAddr)) return "[]";

    QByteArray listPtrBuf = m_reader->readBytes(fieldAddr, 8);
    if (listPtrBuf.isEmpty()) return "[]";
    qint64 listPtr = readI64(listPtrBuf, 0);
    if (!canRead(listPtr)) return "[]";

    QByteArray listCountBuf = m_reader->readBytes(listPtr - 8, 4);
    if (listCountBuf.isEmpty()) return "[]";
    qint32 listCount = readI32(listCountBuf, 0);
    if (listCount < 0 || listCount > 100000) return "[]";
    if (listCount == 0) return "[]";

    if (field.arrayElemType == "CString" || field.arrayElemType == "String") {
        QByteArray elemArrPtrBuf = m_reader->readBytes(listPtr, 8);
        if (elemArrPtrBuf.isEmpty()) return QStringLiteral("[%1 items]").arg(listCount);
        qint64 elemArrPtr = readI64(elemArrPtrBuf, 0);
        if (!canRead(elemArrPtr)) return QStringLiteral("[%1 items]").arg(listCount);

        QStringList strings;
        for (int i = 0; i < listCount && i < 64; ++i) {
            QByteArray elemPtrBuf = m_reader->readBytes(elemArrPtr + i * 8, 8);
            if (elemPtrBuf.isEmpty()) break;
            qint64 strPtr = readI64(elemPtrBuf, 0);
            QString s;
            if (canRead(strPtr)) {
                s = tryReadAsciiString(strPtr, 512);
                if (s.isEmpty())
                    s = tryReadAsciiString(elemArrPtr + i * 8, 512);
            }
            else {
                s = tryReadAsciiString(elemArrPtr + i * 8, 512);
            }
            strings.append(!s.isEmpty() ? QStringLiteral("\"%1\"").arg(s) : "\"\"");
        }
        return strings.isEmpty()
            ? QStringLiteral("[%1 items]").arg(listCount)
            : QStringLiteral("[%1]").arg(strings.join(", "));
    }

    return QStringLiteral("[%1 items]").arg(listCount);
}

// ============================================================
// PrimeBulkRead
// Reads the entire field-data block for typeName in one shot
// so subsequent readFieldValue() calls decode from the buffer
// ============================================================
void LiveValueReader::primeBulkRead(const QString& typeName, qint64 instanceBase) const
{
    m_cachedTypeName.clear();
    m_cachedFieldDataBase = 0;
    m_cachedFieldBuf.clear();
    m_cachedFieldBufSize = 0;

    if (typeName.isEmpty() || !m_allTypes) return;

    // Resolve fieldDataBase (same logic as readFieldValue -> getFieldDataBase)
    qint64 fieldDataBase = 0;
    if (m_settingsListFieldData.contains(typeName)) {
        fieldDataBase = m_settingsListFieldData.value(typeName);
    }
    else {
        fieldDataBase = getFieldDataBase(instanceBase);
    }
    if (fieldDataBase == 0) return;

    // Find the type to compute the buffer size needed
    const UITypeItem* typeItem = nullptr;
    for (const auto& t : *m_allTypes)
        if (t.name == typeName) { typeItem = &t; break; }
    if (!typeItem) return;

    // Compute required buffer size (mirrors discoverFieldOffsetsFromFieldData)
    int bufSize = 0x20;
    for (const auto& f : typeItem->fields) {
        int fSize = getFieldSize(f.type);
        if (fSize == -1) {
            bool isEnum = false;
            for (const auto& t : *m_allTypes)
                if (t.name == f.type && t.category == "Enums") { isEnum = true; break; }
            if (isEnum) fSize = 4;
            else { fSize = computeInlineStructSize(f.type, 0); if (fSize == 0) fSize = 8; }
        }
        int align = qMin(fSize, 8);
        if (align > 1) bufSize = (bufSize + align - 1) & ~(align - 1);
        bufSize += fSize;
    }
    bufSize = qMin((bufSize + 0x40 + 15) & ~15, 0x10000);

    QByteArray buf = m_reader->readBytes(fieldDataBase, bufSize);
    if (buf.isEmpty()) return;

    m_cachedTypeName = typeName;
    m_cachedFieldDataBase = fieldDataBase;
    m_cachedFieldBuf = std::move(buf);
    m_cachedFieldBufSize = bufSize;
}

// ============================================================
// DumpType (debug)
// ============================================================
void LiveValueReader::dumpType(const QString& typeName) const
{
    if (!m_instanceAddresses.contains(typeName)) {
        qDebug("[LiveValueReader] %s instance not found", qPrintable(typeName));
        return;
    }
    if (!m_allTypes) return;
    qint64 instBase = m_instanceAddresses.value(typeName);

    const UITypeItem* typeItem = nullptr;
    for (const auto& t : *m_allTypes)
        if (t.name == typeName) { typeItem = &t; break; }
    if (!typeItem) return;

    QVector<const UIFieldItem*> sortedFields;
    for (const auto& f : typeItem->fields) sortedFields.append(&f);
    std::sort(sortedFields.begin(), sortedFields.end(),
        [](const UIFieldItem* a, const UIFieldItem* b) { return a->offset < b->offset; });

    for (const auto* field : sortedFields) {
        QString val = field->isArray
            ? readListValue(instBase, *field)
            : (readFieldValue(instBase, *field, typeName));
        if (val.isNull()) val = "(skipped)";
        qDebug("  %d  %s  %s  %s",
            field->offset, qPrintable(field->type),
            qPrintable(field->name), qPrintable(val));
    }
}