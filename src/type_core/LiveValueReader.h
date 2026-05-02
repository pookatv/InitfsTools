#pragma once

#include <windows.h>
#include <QString>
#include <QVector>
#include <QHash>
#include <QSet>
#include <functional>

// Forward declarations to avoid including TypeExtractorWindow.h
struct UIFieldItem;
struct UITypeItem;

// ============================================================
// LiveMemoryReader
// ============================================================
class LiveMemoryReader
{
public:
    explicit LiveMemoryReader(DWORD processId);
    ~LiveMemoryReader();

    HANDLE getHandle() const { return m_handle; }

    qint64 position = 0;

    // Read numBytes from an explicit address (does NOT advance position)
    QByteArray readBytes(qint64 address, int numBytes) const;

    // Read numBytes from current position and advance it
    QByteArray readBytes(int numBytes);

    quint8  readByte();
    qint32  readInt();
    quint32 readUInt();
    qint64  readLong();

private:
    HANDLE m_handle = nullptr;
};

// ============================================================
// LiveValueReader
// ============================================================
class LiveValueReader
{
public:
    LiveValueReader(DWORD processId, HANDLE processHandle,
        qint64 moduleBase, qint64 moduleSize,
        const QVector<UITypeItem>* allTypes);
    ~LiveValueReader();

    // ScanInstances — call this from a worker thread
    // typeInfoToName: maps typeinfo vtable address -> type name (built by TypeDumper)
    // statusCallback: called periodically with a status string
    // cancelCheck: return true to abort early
    void scanInstances(
        const QHash<qint64, QString>& typeInfoToName,
        std::function<void(const QString&)> statusCallback,
        std::function<bool()> cancelCheck);

    bool isScanned() const { return m_scanned; }
    int  instanceCount() const { return m_instanceAddresses.size(); }

    bool isMappedBySettingsList(const QString& typeName) const;
    bool tryGetCandidateName(const QString& typeName, QString& outName) const;
    bool tryGetFieldDataAddress(const QString& typeName, qint64& outAddr) const;
    bool tryGetInstance(const QString& typeName, qint64& outBase) const;

    // Public field readers — call after scanInstances()
    QString readFieldValue(qint64 instanceBase, const UIFieldItem& field,
        const QString& typeName = QString()) const;
    QString readListValue(qint64 instanceBase, const UIFieldItem& field) const;

    // Bulk-primes the field buffer for typeName in one ReadProcessMemory call
    // Call this once before iterating fields in displayType(); readFieldValue()
    // will use the cached buffer instead of issuing a separate RPM per field
    void primeBulkRead(const QString& typeName, qint64 instanceBase) const;

    // Debug dump to qDebug()
    void dumpType(const QString& typeName) const;

private:
    // ---- Memory helpers ----
    bool    canRead(qint64 addr) const;
    QString tryReadAsciiString(qint64 addr, int maxLen) const;

    // ---- Settings list ----
    qint64  findSettingsListBase() const;
    qint64  findSettingsListBaseSentinel() const;
    void    walkSettingsList(qint64 listBase,
        const QHash<qint64, QString>& instanceToName);

    // ---- Field offset discovery ----
    QHash<QString, qint64> discoverFieldOffsetsFromFieldData(
        qint64 fieldDataBase, const UITypeItem& typeItem) const;

    void logSubFields(const QString& typeName,
        const QByteArray& rawBytes, int depth) const;

    // ---- Plausibility / scoring ----
    bool isPlausibleValue(const QByteArray& buf, int offset,
        const QString& type) const;
    int  scoreCandidateStart(const QByteArray& buf, int startOffset,
        const QVector<UIFieldItem>& fields) const;

    // ---- Size helpers ----
    static int  getFieldSize(const QString& type);
    int  getComplexTypeSize(const QString& typeName,
        bool requireFlatValuesOnly = false) const;
    int  computeInlineStructSize(const QString& typeName, int depth,
        bool requireFlatValuesOnly = false) const;
    int  getStructNaturalAlignment(const QString& typeName) const;

    // ---- Scalar reading ----
    QString readScalarFromBuf(const QByteArray& buf, int offset,
        const QString& type) const;

    // ---- Instance helpers ----
    bool    validateInstance(qint64 candidateBase, const QString& typeName) const;
    void    verifyInstances();
    qint64  getFieldDataBase(qint64 instanceBase) const;

    // ---- Internal read helper ----
    QString readAtOffset(qint64 fieldDataBase, const UIFieldItem& field,
        qint64 byteOffset) const;

    // ---- Data ----
    LiveMemoryReader* m_reader = nullptr;
    HANDLE                     m_processHandle = nullptr;
    qint64                     m_moduleBase = 0;
    qint64                     m_moduleSize = 0;
    const QVector<UITypeItem>* m_allTypes = nullptr;

    QHash<QString, qint64>                    m_instanceAddresses;
    QHash<QString, QHash<QString, qint64>>    m_discoveredOffsets;
    QHash<QString, qint64>                    m_settingsListFieldData;
    QHash<QString, QString>                   m_settingsListCandidateNames;

    // Per-type bulk read cache — populated by readAllFieldValues(), invalidated on next type switch
    mutable QString     m_cachedTypeName;
    mutable qint64      m_cachedFieldDataBase = 0;
    mutable QByteArray  m_cachedFieldBuf;
    mutable int         m_cachedFieldBufSize = 0;

    bool m_scanned = false;

    // VirtualQueryEx wrapper struct
    struct MemBasicInfo {
        qint64  baseAddress = 0;
        qint64  allocationBase = 0;
        quint32 allocProtect = 0;
        qint64  regionSize = 0;
        quint32 state = 0;
        quint32 protect = 0;
        quint32 type = 0;
    };

    bool virtualQueryRegion(qint64 addr, MemBasicInfo& out) const;
};