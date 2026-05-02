#include "EbxDescriber.h"
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <unordered_map>

static constexpr uint32_t kMagicV1_LE = 0x0FB2D1CE;
static constexpr uint32_t kMagicV2_LE = 0x0FB4D1CE;
static constexpr uint32_t kMagicV1_BE = 0xCED1B20F;
static constexpr uint32_t kMagicV2_BE = 0xCED1B40F;

static constexpr uint32_t kMagicRIFF = 0x46464952u;
static constexpr uint32_t kMagicRIFX = 0x58464952u;

static constexpr uint32_t kSharedTypeDescriptorReference = 0x80000000u;
static constexpr uint32_t kTypeCodeMask = 0x1Fu;
static constexpr uint32_t kTypeCodeShift = 5u;

static constexpr uint32_t makeFourCC(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8)
        | ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}
static constexpr uint32_t kFourCC_RIFF = makeFourCC('R', 'I', 'F', 'F');
static constexpr uint32_t kFourCC_RIFX = makeFourCC('R', 'I', 'F', 'X');
static constexpr uint32_t kFourCC_EBX = makeFourCC('E', 'B', 'X', ' ');
static constexpr uint32_t kFourCC_EBXT = makeFourCC('E', 'B', 'X', 'T');
static constexpr uint32_t kFourCC_EBXD = makeFourCC('E', 'B', 'X', 'D');
static constexpr uint32_t kFourCC_EFIX = makeFourCC('E', 'F', 'I', 'X');
static constexpr uint32_t kFourCC_EBXX = makeFourCC('E', 'B', 'X', 'X');
static constexpr uint32_t kFourCC_REFL = makeFourCC('R', 'E', 'F', 'L');
static constexpr uint32_t kFourCC_RFL2 = makeFourCC('R', 'F', 'L', '2');
static constexpr uint32_t kFourCC_LIST = makeFourCC('L', 'I', 'S', 'T');

#pragma pack(push, 1)

struct EbxGuid {
    uint32_t data1; uint16_t data2; uint16_t data3; uint8_t data4[8];
};

struct EbxHeader {
    uint32_t magic;
    uint32_t metaSize;
    uint32_t payloadSize;
    uint32_t importCount;
    uint16_t rangeCount;
    uint16_t exportedRangeCount;
    uint16_t typeCount;
    uint16_t typeDescriptorCount;
    uint16_t fieldDescriptorCount;
    uint16_t typeStringTableSize;
    uint32_t stringTableSize;
    uint32_t arrayCount;
    uint32_t arrayOffset;
    EbxGuid  partitionGuid;
    uint32_t boxedValueCount;
    uint32_t boxedValueOffset;
};
static_assert(sizeof(EbxHeader) == 64, "EbxHeader size mismatch");

struct EbxSharedHeader {
    uint32_t magic;
    uint16_t typeDescriptorCount;
    uint16_t fieldDescriptorCount;
};
static_assert(sizeof(EbxSharedHeader) == 8, "");

struct EbxFieldDesc {
    uint32_t fieldNameHash;
    uint16_t flags;
    uint16_t fieldType;
    uint32_t fieldOffset;
    uint32_t secondaryOffset;
};
static_assert(sizeof(EbxFieldDesc) == 16, "");

struct EbxTypeDesc {
    uint32_t typeNameHash;
    uint32_t layoutDescriptor;
    uint8_t  fieldCount_;
    uint8_t  alignment_;
    uint16_t typeFlags;
    uint16_t instanceSize;
    uint16_t secondaryInstanceSize;
};
static_assert(sizeof(EbxTypeDesc) == 16, "");

static constexpr uint32_t kSharedEntryStride = 32;
static constexpr uint32_t kSharedEntryTypedescOffset = 16;

struct EbxImportEntry { EbxGuid partitionGuid; EbxGuid instanceGuid; };
struct EbxInstanceRange { uint16_t typeDescriptorIndex; uint16_t instanceCount; };
struct EbxArrayEntry { uint32_t offset; uint32_t elementCount; uint32_t typeDescriptorIndex; };
struct EbxBoxedValueEntry { uint32_t offset; uint16_t typeId; uint16_t typeCode; };

#pragma pack(pop)

// ---- Image-format (REFL/RFL2 chunk) structs — natural alignment ----
// EbxTypeSignature: guid(16) + signature(4) = 20 bytes
struct EbxTypeSignature {
    EbxGuid  guid;
    uint32_t signature;
};
static_assert(sizeof(EbxTypeSignature) == 20, "");

// ReflTypeDescriptor (16 bytes):
//   nameOffset  u32 @ 0
//   fieldsIndex u32 @ 4
//   fieldCount  u16 @ 8
//   typeFlags   u16 @ 10 (stored >> 1 on disk, i.e. raw >> 1 gives type code)
//   size        u16 @ 12
//   alignment   u16 @ 14
struct ReflTypeDescriptor {
    uint32_t nameOffset;
    uint32_t fieldsIndex;
    uint16_t fieldCount;
    uint16_t typeFlags; // raw value; typeCode = (typeFlags >> 1) & 0x1F
    uint16_t size;
    uint16_t alignment;
};
static_assert(sizeof(ReflTypeDescriptor) == 16, "");

// ReflFieldDescriptor (12 bytes):
//   nameOffset  u32 @ 0
//   dataOffset  u32 @ 4
//   type        u16 @ 8  (raw; typeCode = (type >> 1) & 0x1F)
//   classRef    u16 @ 10
// NO padding, NO extra u32.
struct ReflFieldDescriptor {
    uint32_t nameOffset;
    uint32_t dataOffset;
    uint16_t type; // raw; typeCode = (type >> 1) & 0x1F
    uint16_t classRef;
};
static_assert(sizeof(ReflFieldDescriptor) == 12, "");

static_assert(sizeof(EbxImportEntry) == 32, "");
static_assert(sizeof(EbxInstanceRange) == 4, "");
static_assert(sizeof(EbxArrayEntry) == 12, "");
static_assert(sizeof(EbxBoxedValueEntry) == 8, "");

// ---- helpers ----
template<typename T>
static T bswap(T v) {
    uint8_t buf[sizeof(T)];
    memcpy(buf, &v, sizeof(T));
    std::reverse(buf, buf + sizeof(T));
    T r; memcpy(&r, buf, sizeof(T)); return r;
}

struct GuidStr { char s[40]; };
static GuidStr guidToString(const EbxGuid& g) {
    GuidStr r{};
    snprintf(r.s, sizeof(r.s),
        "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        g.data1, g.data2, g.data3,
        g.data4[0], g.data4[1],
        g.data4[2], g.data4[3], g.data4[4],
        g.data4[5], g.data4[6], g.data4[7]);
    return r;
}

static const char* typeCodeName(uint32_t code) {
    switch (code & kTypeCodeMask) {
    case  0: return "void/Inherited";
    case  1: return "DbObject";
    case  2: return "ValueType";
    case  3: return "Class";
    case  4: return "Array";
    case  5: return "FixedArray";
    case  6: return "String";
    case  7: return "CString";
    case  8: return "Enum";
    case  9: return "FileRef";
    case 10: return "bool";
    case 11: return "s8";
    case 12: return "u8";
    case 13: return "s16";
    case 14: return "u16";
    case 15: return "s32";
    case 16: return "u32";
    case 17: return "s64";
    case 18: return "u64";
    case 19: return "float";
    case 20: return "double";
    case 21: return "Guid";
    case 22: return "SHA1";
    case 23: return "ResourceRef";
    case 24: return "Function";
    case 25: return "TypeRef";
    case 26: return "BoxedValueRef";
    case 27: return "Interface";
    case 28: return "Delegate";
    default: return "Unknown";
    }
}

static uint32_t hashQuick(const char* str) {
    uint32_t h = 5381;
    while (unsigned char c = static_cast<unsigned char>(*str++))
        h = ((h << 5) + h) ^ c;
    return h;
}

static std::string lookupHash(uint32_t hash,
    const std::unordered_map<uint32_t, std::string>& map) {
    auto it = map.find(hash);
    if (it != map.end()) return it->second;
    char buf[24];
    snprintf(buf, sizeof(buf), "hash:0x%08X", hash);
    return buf;
}

static uint32_t roundUp(uint32_t v, uint32_t align) {
    return (v + align - 1u) & ~(align - 1u);
}

struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    bool overflow = false;

    Cursor(const uint8_t* data, size_t size) : p(data), end(data + size) {}
    const uint8_t* take(size_t n) {
        if (n == 0) return p;
        if (static_cast<size_t>(end - p) < n) { overflow = true; return nullptr; }
        const uint8_t* r = p; p += n; return r;
    }
    bool read32(uint32_t& out) {
        const uint8_t* r = take(4);
        if (!r) return false;
        memcpy(&out, r, 4);
        return true;
    }
    size_t remaining() const { return static_cast<size_t>(end - p); }
    void skip(size_t n) { take(n); }
};

static bool detectMagic(const uint8_t* data, bool& needSwap, int& version) {
    uint32_t raw; memcpy(&raw, data, 4);
    if (raw == kMagicV1_LE) { needSwap = false; version = 1; return true; }
    else if (raw == kMagicV2_LE) { needSwap = false; version = 2; return true; }
    else if (raw == kMagicV1_BE) { needSwap = true;  version = 1; return true; }
    else if (raw == kMagicV2_BE) { needSwap = true;  version = 2; return true; }
    return false;
}

// ============================================================
// RIFF chunk walker
// ============================================================
struct RiffChunkView {
    uint32_t       cc;
    uint32_t       size;
    uint32_t       declaredSize;
    const uint8_t* data;
};

template<typename Visitor>
static void walkRiffChunks(const uint8_t* listData, uint32_t listSize,
    bool bigEndian, Visitor&& visitor)
{
    const uint8_t* p = listData;
    const uint8_t* end = listData + listSize;

    while (p + 8 <= end)
    {
        uintptr_t off = static_cast<uintptr_t>(p - listData);
        if (off & 1) { ++p; continue; }

        RiffChunkView chunk;
        memcpy(&chunk.cc, p, 4);
        memcpy(&chunk.declaredSize, p + 4, 4);
        if (bigEndian) { chunk.cc = bswap(chunk.cc); chunk.declaredSize = bswap(chunk.declaredSize); }
        p += 8;
        chunk.data = p;

        uint32_t available = static_cast<uint32_t>(end - p);
        chunk.size = (chunk.declaredSize <= available) ? chunk.declaredSize : available;

        if (!visitor(chunk)) return;

        uint32_t advance = (chunk.declaredSize <= available) ? chunk.declaredSize : available;
        p += advance;
        if (chunk.declaredSize & 1) { if (p < end) ++p; }
    }
}

// ============================================================
// fourCC to string
// ============================================================
struct FourCCStr { char s[16]; };
static FourCCStr fourCCToString(uint32_t cc) {
    FourCCStr r{};
    uint8_t b[4]; memcpy(b, &cc, 4);
    bool allPrint = true;
    for (int i = 0; i < 4; ++i)
        if (b[i] < 0x20 || b[i] > 0x7E) { allPrint = false; break; }
    if (allPrint) snprintf(r.s, sizeof(r.s), "%c%c%c%c", b[0], b[1], b[2], b[3]);
    else          snprintf(r.s, sizeof(r.s), "0x%08X", cc);
    return r;
}

// ============================================================
// Describe SharedTypeDescriptors (old bare partition format)
// ============================================================
static std::string describeSharedFlat(const uint8_t* data, size_t size,
    bool needSwap, int version)
{
    std::ostringstream out;
    auto sw32 = [&](uint32_t v) -> uint32_t { return needSwap ? bswap(v) : v; };
    auto sw16 = [&](uint16_t v) -> uint16_t { return needSwap ? bswap(v) : v; };

    bool useHeader = false;
    uint32_t typeCnt = 0, fieldCnt = 0;
    if (size >= sizeof(EbxSharedHeader)) {
        EbxSharedHeader hdr;
        memcpy(&hdr, data, sizeof(hdr));
        uint32_t tc = sw16(hdr.typeDescriptorCount);
        uint32_t fc = sw16(hdr.fieldDescriptorCount);
        if (tc > 0 && tc < 65536 && fc < 200000 &&
            sizeof(EbxSharedHeader) + (size_t)tc * kSharedEntryStride <= size) {
            typeCnt = tc; fieldCnt = fc; useHeader = true;
        }
    }

    const uint8_t* typeEntries = nullptr;

    if (useHeader) {
        size_t typeBlockSize = (size_t)typeCnt * kSharedEntryStride;
        typeEntries = data + size - typeBlockSize;
        out << "=== EBX Shared Type Descriptor Table ===\n\n";
        out << "[Header]\n";
        out << "  Version              : V" << version
            << (needSwap ? " (big-endian)" : " (little-endian)") << "\n";
        out << "  TypeDescriptorCount  : " << typeCnt << "\n";
        out << "  FieldDescriptorCount : " << fieldCnt << "\n";
        out << "  FileSize             : " << size << " bytes\n\n";
    }
    else {
        size_t bodySize = (size > 4) ? (size - 4) : 0;
        typeCnt = static_cast<uint32_t>(bodySize / kSharedEntryStride);
        out << "=== EBX Shared Type Descriptor Table (headerless flat format) ===\n\n";
        out << "[Header]\n";
        out << "  Version              : V" << version
            << (needSwap ? " (big-endian)" : " (little-endian)") << "\n";
        out << "  FileSize             : " << size << " bytes\n";
        out << "  EntryStride          : " << kSharedEntryStride << " bytes\n";
        out << "  DerivedEntryCount    : " << typeCnt << "\n";
        out << "  (No count header)\n\n";
        if (size > 4) typeEntries = data + 4;
    }

    out << "[TypeDescriptors]  (" << typeCnt << ")\n";
    if (!typeEntries) {
        out << "  (truncated)\n\n";
    }
    else {
        for (uint32_t i = 0; i < typeCnt; ++i) {
            const uint8_t* entryBase = typeEntries + (size_t)i * kSharedEntryStride;
            const uint8_t* typeDescPtr = entryBase + kSharedEntryTypedescOffset;
            EbxTypeDesc td;
            memcpy(&td, typeDescPtr, sizeof(td));
            uint32_t nameHash = sw32(td.typeNameHash);
            uint32_t layout = sw32(td.layoutDescriptor);
            uint16_t instSize = sw16(td.instanceSize);
            uint16_t flags = sw16(td.typeFlags);
            bool     isRef = (layout & kSharedTypeDescriptorReference) != 0;
            uint16_t fc = static_cast<uint16_t>(td.fieldCount_)
                | static_cast<uint16_t>((td.alignment_ & 0x80u) << 1);
            uint8_t  align = td.alignment_ & 0x7Fu;
            out << "  [" << i << "]"
                << "  nameHash=0x" << std::hex << nameHash << std::dec
                << "  typeCode=" << typeCodeName(flags >> kTypeCodeShift)
                << "  fields=" << fc
                << "  size=" << instSize
                << "  align=" << static_cast<int>(align);
            if (isRef) {
                uint32_t refOff = layout & ~kSharedTypeDescriptorReference;
                out << "  [ref offset=" << refOff << "]\n";
            }
            else {
                out << "\n";
                if (fc > 0) {
                    uint32_t bytesBack = layout;
                    if (bytesBack > 0 && bytesBack <= (size_t)(typeDescPtr - data)) {
                        const uint8_t* fdPtr = typeDescPtr - bytesBack;
                        for (uint16_t f = 0; f < fc; ++f) {
                            if (fdPtr + sizeof(EbxFieldDesc) > data + size) break;
                            EbxFieldDesc fd;
                            memcpy(&fd, fdPtr, sizeof(fd));
                            fdPtr += sizeof(EbxFieldDesc);
                            out << "      field[" << f << "]"
                                << "  nameHash=0x" << std::hex << sw32(fd.fieldNameHash) << std::dec
                                << "  typeCode=" << typeCodeName(sw16(fd.flags) >> kTypeCodeShift)
                                << "  fieldType=" << sw16(fd.fieldType)
                                << "  offset=" << sw32(fd.fieldOffset) << "\n";
                        }
                    }
                }
            }
        }
    }
    out << "\n";
    return out.str();
}

// ============================================================
// Describe standard EBX partition (64-byte header)
// ============================================================
static std::string describePartition(const uint8_t* data, size_t size,
    bool needSwap, int version)
{
    std::ostringstream out;
    auto sw32 = [&](uint32_t v) -> uint32_t { return needSwap ? bswap(v) : v; };
    auto sw16 = [&](uint16_t v) -> uint16_t { return needSwap ? bswap(v) : v; };

    EbxHeader hdr;
    memcpy(&hdr, data, sizeof(EbxHeader));
    if (needSwap) {
        hdr.magic = bswap(hdr.magic);
        hdr.metaSize = bswap(hdr.metaSize);
        hdr.payloadSize = bswap(hdr.payloadSize);
        hdr.importCount = bswap(hdr.importCount);
        hdr.rangeCount = bswap(hdr.rangeCount);
        hdr.exportedRangeCount = bswap(hdr.exportedRangeCount);
        hdr.typeCount = bswap(hdr.typeCount);
        hdr.typeDescriptorCount = bswap(hdr.typeDescriptorCount);
        hdr.fieldDescriptorCount = bswap(hdr.fieldDescriptorCount);
        hdr.typeStringTableSize = bswap(hdr.typeStringTableSize);
        hdr.stringTableSize = bswap(hdr.stringTableSize);
        hdr.arrayCount = bswap(hdr.arrayCount);
        hdr.arrayOffset = bswap(hdr.arrayOffset);
        hdr.boxedValueCount = bswap(hdr.boxedValueCount);
        hdr.boxedValueOffset = bswap(hdr.boxedValueOffset);
        hdr.partitionGuid.data1 = bswap(hdr.partitionGuid.data1);
        hdr.partitionGuid.data2 = bswap(hdr.partitionGuid.data2);
        hdr.partitionGuid.data3 = bswap(hdr.partitionGuid.data3);
    }

    const size_t headerEnd = sizeof(EbxHeader);
    if (hdr.metaSize == 0 || hdr.metaSize > size - headerEnd) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "Error: metaSize=%u claims more space than file provides (%zu bytes after header).",
            hdr.metaSize, size - headerEnd);
        return buf;
    }

    out << "=== EBX Partition Descriptor ===\n\n";
    out << "[Header]\n";
    out << "  Version              : V" << version
        << (needSwap ? " (big-endian)" : " (little-endian)") << "\n";
    out << "  PartitionGuid        : " << guidToString(hdr.partitionGuid).s << "\n";
    out << "  MetaSize             : " << hdr.metaSize << " bytes\n";
    out << "  PayloadSize          : " << hdr.payloadSize << " bytes\n";
    out << "  ImportCount          : " << hdr.importCount << "\n";
    out << "  RangeCount           : " << hdr.rangeCount
        << " (exported: " << hdr.exportedRangeCount << ")\n";
    out << "  TypeDescriptorCount  : " << hdr.typeDescriptorCount << "\n";
    out << "  FieldDescriptorCount : " << hdr.fieldDescriptorCount << "\n";
    out << "  TypeStringTableSize  : " << hdr.typeStringTableSize << " bytes\n";
    out << "  StringTableSize      : " << hdr.stringTableSize << " bytes\n";
    out << "  ArrayCount           : " << hdr.arrayCount << "\n";
    out << "  BoxedValueCount      : " << hdr.boxedValueCount << "\n\n";

    Cursor cur(data + headerEnd, hdr.metaSize);

    const EbxImportEntry* imports = nullptr;
    const char* typeStrBlk = nullptr;
    const EbxFieldDesc* fieldDescs = nullptr;
    const EbxTypeDesc* typeDescs = nullptr;
    const EbxInstanceRange* ranges = nullptr;
    const EbxArrayEntry* arrays = nullptr;
    const EbxBoxedValueEntry* boxedValues = nullptr;

    { auto p = cur.take(sizeof(EbxImportEntry) * hdr.importCount); if (p) imports = reinterpret_cast<const EbxImportEntry*>(p); }
    { auto p = cur.take(hdr.typeStringTableSize); if (p) typeStrBlk = reinterpret_cast<const char*>(p); }
    { auto p = cur.take(roundUp(sizeof(EbxFieldDesc) * hdr.fieldDescriptorCount, 16)); if (p) fieldDescs = reinterpret_cast<const EbxFieldDesc*>(p); }
    { auto p = cur.take(sizeof(EbxTypeDesc) * hdr.typeDescriptorCount); if (p) typeDescs = reinterpret_cast<const EbxTypeDesc*>(p); }
    { auto p = cur.take(roundUp(sizeof(EbxInstanceRange) * hdr.rangeCount, 16)); if (p) ranges = reinterpret_cast<const EbxInstanceRange*>(p); }
    { auto p = cur.take(roundUp(sizeof(EbxArrayEntry) * hdr.arrayCount, 16)); if (p) arrays = reinterpret_cast<const EbxArrayEntry*>(p); }
    { auto p = cur.take(roundUp(sizeof(EbxBoxedValueEntry) * hdr.boxedValueCount, 16)); if (p) boxedValues = reinterpret_cast<const EbxBoxedValueEntry*>(p); }

    if (cur.overflow) out << "WARNING: metadata block truncated.\n\n";

    std::unordered_map<uint32_t, std::string> reverseMap;
    if (typeStrBlk && hdr.typeStringTableSize > 0) {
        const char* ptr = typeStrBlk;
        const char* pEnd = typeStrBlk + hdr.typeStringTableSize;
        while (ptr < pEnd) {
            size_t maxLen = static_cast<size_t>(pEnd - ptr);
            size_t len = strnlen(ptr, maxLen);
            if (len == 0) break;
            reverseMap.emplace(hashQuick(ptr), std::string(ptr, len));
            ptr += len + 1;
        }
    }

    auto resolveTypeName = [&](const EbxTypeDesc& td) -> std::string {
        uint32_t hash = sw32(td.typeNameHash);
        uint32_t layout = sw32(td.layoutDescriptor);
        std::string name = lookupHash(hash, reverseMap);
        if (layout & kSharedTypeDescriptorReference) name += " [shared]";
        return name;
        };

    out << "[Imports]  (" << hdr.importCount << ")\n";
    if (!imports && hdr.importCount > 0) { out << "  (truncated)\n"; }
    else for (uint32_t i = 0; i < hdr.importCount; ++i) {
        EbxImportEntry ie = imports[i];
        if (needSwap) {
            ie.partitionGuid.data1 = bswap(ie.partitionGuid.data1);
            ie.partitionGuid.data2 = bswap(ie.partitionGuid.data2);
            ie.partitionGuid.data3 = bswap(ie.partitionGuid.data3);
            ie.instanceGuid.data1 = bswap(ie.instanceGuid.data1);
            ie.instanceGuid.data2 = bswap(ie.instanceGuid.data2);
            ie.instanceGuid.data3 = bswap(ie.instanceGuid.data3);
        }
        out << "  [" << i << "]  partition=" << guidToString(ie.partitionGuid).s
            << "  instance=" << guidToString(ie.instanceGuid).s << "\n";
    }
    out << "\n";

    out << "[TypeDescriptors]  (" << hdr.typeDescriptorCount << ")\n";
    if (!typeDescs && hdr.typeDescriptorCount > 0) { out << "  (truncated)\n"; }
    else for (uint32_t i = 0; i < hdr.typeDescriptorCount; ++i) {
        const EbxTypeDesc& td = typeDescs[i];
        uint32_t layout = sw32(td.layoutDescriptor);
        uint16_t instSz = sw16(td.instanceSize);
        uint16_t flags = sw16(td.typeFlags);
        bool     isShared = (layout & kSharedTypeDescriptorReference) != 0;
        uint16_t fc = static_cast<uint16_t>(td.fieldCount_)
            | static_cast<uint16_t>((td.alignment_ & 0x80u) << 1);
        uint8_t  align = td.alignment_ & 0x7Fu;
        out << "  [" << i << "] " << resolveTypeName(td)
            << "  fields=" << fc << "  size=" << instSz
            << "  align=" << (int)align
            << "  typeCode=" << typeCodeName(flags >> kTypeCodeShift)
            << (isShared ? "  [shared]" : "") << "\n";
        if (!isShared && fc > 0 && fieldDescs) {
            uint32_t fdStart = layout;
            for (uint16_t f = 0; f < fc; ++f) {
                uint32_t fdIdx = fdStart + f;
                if (fdIdx >= hdr.fieldDescriptorCount) break;
                const EbxFieldDesc& fd = fieldDescs[fdIdx];
                out << "      field[" << f << "] "
                    << lookupHash(sw32(fd.fieldNameHash), reverseMap)
                    << "  offset=" << sw32(fd.fieldOffset)
                    << "  typeCode=" << typeCodeName(sw16(fd.flags) >> kTypeCodeShift) << "\n";
            }
        }
    }
    out << "\n";

    out << "[InstanceRanges]  (" << hdr.rangeCount << ")\n";
    if (!ranges && hdr.rangeCount > 0) { out << "  (truncated)\n"; }
    else for (uint32_t i = 0; i < hdr.rangeCount; ++i) {
        EbxInstanceRange r = ranges[i];
        uint16_t tidx = sw16(r.typeDescriptorIndex);
        uint16_t count = sw16(r.instanceCount);
        std::string name = (typeDescs && tidx < hdr.typeDescriptorCount)
            ? resolveTypeName(typeDescs[tidx]) : "?";
        out << "  [" << i << "]  type=" << name << "  count=" << count
            << (i < hdr.exportedRangeCount ? "  [exported]" : "") << "\n";
    }
    out << "\n";

    out << "[Arrays]  (" << hdr.arrayCount << ")\n";
    if (!arrays && hdr.arrayCount > 0) { out << "  (truncated)\n"; }
    else for (uint32_t i = 0; i < hdr.arrayCount; ++i) {
        EbxArrayEntry ae = arrays[i];
        uint32_t tidx = sw32(ae.typeDescriptorIndex);
        uint32_t count = sw32(ae.elementCount);
        uint32_t off = sw32(ae.offset);
        std::string name = (typeDescs && tidx < hdr.typeDescriptorCount)
            ? resolveTypeName(typeDescs[tidx]) : "?";
        out << "  [" << i << "]  type=" << name
            << "  elementCount=" << count
            << "  offset=0x" << std::hex << off << std::dec << "\n";
    }
    out << "\n";

    out << "[BoxedValues]  (" << hdr.boxedValueCount << ")\n";
    if (!boxedValues && hdr.boxedValueCount > 0) { out << "  (truncated)\n"; }
    else for (uint32_t i = 0; i < hdr.boxedValueCount; ++i) {
        EbxBoxedValueEntry bv = boxedValues[i];
        out << "  [" << i << "]  typeCode=" << typeCodeName(sw16(bv.typeCode))
            << "  typeId=" << sw16(bv.typeId)
            << "  offset=0x" << std::hex << sw32(bv.offset) << std::dec << "\n";
    }
    out << "\n";

    if (hdr.stringTableSize > 0) {
        size_t payloadStart = headerEnd + hdr.metaSize;
        if (payloadStart < size) {
            size_t available = size - payloadStart;
            size_t readLen = std::min((size_t)hdr.stringTableSize, available);
            out << "[PayloadStrings]  (" << hdr.stringTableSize << " bytes)\n";
            const char* ptr = reinterpret_cast<const char*>(data + payloadStart);
            const char* pEnd = ptr + readLen;
            int count = 0;
            while (ptr < pEnd && count < 64) {
                size_t maxLen = static_cast<size_t>(pEnd - ptr);
                size_t len = strnlen(ptr, maxLen);
                if (len == 0) { ++ptr; continue; }
                out << "  \"" << std::string(ptr, len) << "\"\n";
                ptr += len + 1;
                ++count;
            }
            if (count == 64) out << "  ... (truncated)\n";
            out << "\n";
        }
    }

    return out.str();
}

// ============================================================
// Parse REFL/RFL2 chunk — partition format
//
// Layout:
//    u32                        typeCount
//    EbxTypeSignature[typeCount]            20 bytes each (guid16 + sig4)
//    ReflTypeDescriptor[typeCount]          16 bytes each
//    u32                        fieldCount
//    ReflFieldDescriptor[fieldCount]        12 bytes each
//    u32                        unkCount0
//    byte[unkCount0 * 12]
//    u32                        unkCount1
//    byte[unkCount1 * 8]
//    char[]                     stringTable
// ============================================================
static std::string describeReflChunk(const uint8_t* reflData, uint32_t reflSize,
    const std::vector<EbxGuid>& typeGuids, bool hasSigs)
{
    std::ostringstream out;

    if (!reflData || reflSize < 4) {
        out << "Note: No REFL/RFL2 chunk found.\n";
        return out.str();
    }

    Cursor rc(reflData, reflSize);

    uint32_t typeCount = 0;
    if (!rc.read32(typeCount) || typeCount > 200000) {
        out << "Error: REFL chunk has invalid type count (" << typeCount << ").\n";
        return out.str();
    }

    // Type signatures: guid(16) + signature(4) = 20 bytes each
    // Only present when there is an EFIX chunk (partition image format)
    // Shared EBXT images have NO signature block
    std::vector<EbxTypeSignature> typeSigs;
    if (hasSigs) {
        const uint8_t* sigBlock = rc.take(sizeof(EbxTypeSignature) * typeCount);
        if (!sigBlock) {
            out << "Error: REFL truncated reading type signatures.\n";
            return out.str();
        }
        typeSigs.resize(typeCount);
        memcpy(typeSigs.data(), sigBlock, sizeof(EbxTypeSignature) * typeCount);
    }

    // Skip 4-byte gap between signatures and type descriptors (present in shared EBXT RFL2)
    if (hasSigs && typeGuids.empty()) {
        rc.skip(4);
    }

    const uint32_t TD_STRIDE = 16u;

    std::vector<ReflTypeDescriptor> typeDescs(typeCount);
    {
        const uint8_t* tdBlock = rc.take(TD_STRIDE * typeCount);
        if (!tdBlock) {
            out << "Error: REFL truncated reading type descriptors.\n";
            return out.str();
        }
        // TD_STRIDE == sizeof(ReflTypeDescriptor) == 16, so a single memcpy suffices
        memcpy(typeDescs.data(), tdBlock, sizeof(ReflTypeDescriptor) * typeCount);
    }

    // Field count
    uint32_t fieldCount = 0;
    if (!rc.read32(fieldCount) || fieldCount > 10000000) {
        out << "Error: REFL chunk invalid field count (" << fieldCount << ").\n";
        return out.str();
    }

    // Field descriptors: 12 bytes each
    std::vector<ReflFieldDescriptor> fieldDescs(fieldCount);
    {
        const uint8_t* fdBlock = rc.take(sizeof(ReflFieldDescriptor) * fieldCount);
        if (!fdBlock) {
            out << "Error: REFL truncated reading field descriptors"
                << " (remaining=" << rc.remaining() << " bytes).\n";
            return out.str();
        }
        memcpy(fieldDescs.data(), fdBlock, sizeof(ReflFieldDescriptor) * fieldCount);
    }

    // Two unknown trailing arrays before the string table
    uint32_t unkCount0 = 0, unkCount1 = 0;
    if (rc.read32(unkCount0) && unkCount0 < 1000000) {
        rc.skip(unkCount0 * 12);
    }
    if (rc.read32(unkCount1) && unkCount1 < 1000000) {
        rc.skip(unkCount1 * 8);
    }

    // Remainder is the string table
    const char* strTable = reinterpret_cast<const char*>(rc.p);
    size_t      strTableSize = rc.remaining();

    auto getStr = [&](uint32_t nameOffset) -> std::string {
        if (nameOffset >= strTableSize) {
            char buf[32]; snprintf(buf, sizeof(buf), "<offset:0x%X>", nameOffset);
            return buf;
        }
        const char* s = strTable + nameOffset;
        size_t maxLen = strTableSize - nameOffset;
        size_t len = strnlen(s, maxLen);
        return std::string(s, len);
        };

    if (hasSigs && !typeSigs.empty()) {
        out << "[TypeSignatures]  (" << typeCount << ")\n";
        for (uint32_t i = 0; i < typeCount; ++i) {
            out << "  [" << i << "]  guid=" << guidToString(typeSigs[i].guid).s
                << "  signature=0x" << std::hex << typeSigs[i].signature << std::dec << "\n";
        }
        out << "\n";
    }

    out << "[TypeDescriptors]  (" << typeCount << ")\n";
    for (uint32_t i = 0; i < typeCount; ++i) {
        const ReflTypeDescriptor& td = typeDescs[i];
        std::string typeName = getStr(td.nameOffset);
        uint32_t typeCode = (td.typeFlags >> 1) & kTypeCodeMask;
        out << "  [" << i << "] " << typeName
            << "  typeCode=" << typeCodeName(typeCode)
            << "  fields=" << td.fieldCount
            << "  size=" << td.size
            << "  align=" << td.alignment
            << "  fieldsIndex=" << td.fieldsIndex << "\n";

        uint32_t fEnd = td.fieldsIndex + td.fieldCount;
        if (fEnd > fieldCount) fEnd = fieldCount;
        for (uint32_t f = td.fieldsIndex; f < fEnd; ++f) {
            const ReflFieldDescriptor& fd = fieldDescs[f];
            std::string fieldName = getStr(fd.nameOffset);
            uint32_t fieldCode = (fd.type >> 1) & kTypeCodeMask;
            out << "      field[" << (f - td.fieldsIndex) << "] " << fieldName
                << "  typeCode=" << typeCodeName(fieldCode)
                << "  offset=" << fd.dataOffset;
            if (fd.classRef != 0xFFFF && fd.classRef < typeCount)
                out << "  fieldType=" << getStr(typeDescs[fd.classRef].nameOffset);
            out << "\n";
        }
    }
    out << "\n";

    out << "[FieldDescriptors]  (" << fieldCount << " total)\n";
    out << "  (shown inline under each TypeDescriptor above)\n\n";
    if (unkCount0) out << "[UnknownArray0]  (" << unkCount0 << " entries x 12 bytes)\n\n";
    if (unkCount1) out << "[UnknownArray1]  (" << unkCount1 << " entries x 8 bytes)\n\n";

    if (!typeGuids.empty()) {
        out << "[TypeGUIDs from EFIX]  (" << typeGuids.size() << ")\n";
        for (size_t i = 0; i < typeGuids.size(); ++i) {
            std::string name = (i < typeDescs.size()) ? getStr(typeDescs[i].nameOffset) : "?";
            out << "  [" << i << "] " << guidToString(typeGuids[i]).s
                << "  name=" << name << "\n";
        }
        out << "\n";
    }

    return out.str();
}

// ============================================================
// Shared-image EBX (RIFF/EBXT with no EFIX)
// ============================================================
static std::string describeSharedImageEbx(
    const uint8_t* fileData, size_t fileSize,
    bool riffBigEndian, uint32_t formType)
{
    std::ostringstream out;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "=== EBX Image Container (RIFF) ===\n\n"
        "  Magic    : %s\n"
        "  FormType : %s\n"
        "  FileSize : %zu bytes\n\n",
        riffBigEndian ? "RIFX (big-endian)" : "RIFF (little-endian)",
        fourCCToString(formType).s, fileSize);
    out << buf;

    out << "[Partition]\n";
    out << "  (no EFIX chunk — this is a shared type descriptor table, not a partition)\n\n";

    if (fileSize < 12) return out.str();

    uint32_t listSz; memcpy(&listSz, fileData + 4, 4);
    if (riffBigEndian) listSz = bswap(listSz);
    if (listSz < 4) return out.str();

    const uint8_t* listStart = fileData + 12;
    uint32_t       listSize2 = listSz - 4;
    if (listStart + listSize2 > fileData + fileSize)
        listSize2 = static_cast<uint32_t>(fileData + fileSize - listStart);

    const uint8_t* ebxdData = nullptr; uint32_t ebxdSize = 0;
    const uint8_t* reflData = nullptr; uint32_t reflSize = 0;
    uint32_t       reflCC = 0;

    walkRiffChunks(listStart, listSize2, riffBigEndian,
        [&](const RiffChunkView& chunk) -> bool {
            if (chunk.cc == kFourCC_EBXD) { ebxdData = chunk.data; ebxdSize = chunk.size; }
            else if (chunk.cc == kFourCC_REFL || chunk.cc == kFourCC_RFL2) { reflData = chunk.data; reflSize = chunk.size; reflCC = chunk.cc; }
            return true;
        });

    // Case A: EBXD present — raw old-format shared table
    if (ebxdData && ebxdSize >= 4) {
        bool needSwap = false; int version = 0;
        if (detectMagic(ebxdData, needSwap, version)) {
            out << describeSharedFlat(ebxdData, ebxdSize, needSwap, version);
        }
        else {
            out << "[EBXD Chunk]  (" << ebxdSize << " bytes — no recognised magic)\n\n";
        }
        if (reflData && reflSize >= 4) {
            out << "[REFL Chunk — supplementary reflection info]\n\n";
            std::vector<EbxGuid> noGuids;
            out << describeReflChunk(reflData, reflSize, noGuids, false); // no sigs
        }
        return out.str();
    }

    // Case B: No EBXD — the RFL2/REFL chunk IS the reflection/type table
    // in the same REFL partition format used by games without SharedTypeDescriptors
    if (reflData && reflSize >= 4) {
        out << "[" << fourCCToString(reflCC).s << " Chunk — reflection type table ("
            << reflSize << " bytes)]\n\n";
        std::vector<EbxGuid> noGuids;
        // Shared EBXT RFL2 also contains type signatures (stride=20),
        // same layout as partition REFL — sigs ARE present even without EFIX
        out << describeReflChunk(reflData, reflSize, noGuids, true);
        return out.str();
    }

    out << "Error: no EBXD or REFL/RFL2 chunk found.\n";
    return out.str();
}

// ============================================================
// Describe image-based EBX partition (RIFF with EFIX + REFL)
// ============================================================
static std::string describeImageEbx(
    const uint8_t* fileData, size_t fileSize,
    bool riffBigEndian, uint32_t formType, bool hasEfix)
{
    if (!hasEfix)
        return describeSharedImageEbx(fileData, fileSize, riffBigEndian, formType);

    const uint8_t* ebxdData = nullptr; uint32_t ebxdSize = 0;
    const uint8_t* efixData = nullptr; uint32_t efixSize = 0;
    const uint8_t* reflData = nullptr; uint32_t reflSize = 0;

    if (fileSize < 12) return "Error: RIFF file too small.\n";

    uint32_t listSz; memcpy(&listSz, fileData + 4, 4);
    if (riffBigEndian) listSz = bswap(listSz);
    if (listSz < 4) return "Error: RIFF size field too small.\n";

    const uint8_t* listStart = fileData + 12;
    uint32_t       listSize2 = listSz - 4;
    if (listStart + listSize2 > fileData + fileSize)
        listSize2 = static_cast<uint32_t>(fileData + fileSize - listStart);

    walkRiffChunks(listStart, listSize2, riffBigEndian,
        [&](const RiffChunkView& chunk) -> bool {
            if (chunk.cc == kFourCC_EBXD) { ebxdData = chunk.data; ebxdSize = chunk.size; }
            else if (chunk.cc == kFourCC_EFIX) { efixData = chunk.data; efixSize = chunk.size; }
            else if (chunk.cc == kFourCC_REFL || chunk.cc == kFourCC_RFL2) { reflData = chunk.data; reflSize = chunk.size; }
            return true;
        });

    std::ostringstream out;
    {
        char buf2[256];
        snprintf(buf2, sizeof(buf2),
            "=== EBX Image Container (RIFF) ===\n\n"
            "  Magic    : %s\n"
            "  FormType : %s\n"
            "  FileSize : %zu bytes\n\n",
            riffBigEndian ? "RIFX (big-endian)" : "RIFF (little-endian)",
            fourCCToString(formType).s, fileSize);
        out << buf2;
    }

    EbxGuid partitionGuid = {};
    std::vector<EbxGuid>  typeGuids;
    std::vector<uint32_t> signatures;
    uint32_t exportedCount = 0, dcCount = 0;

    if (efixData && efixSize >= 16) {
        Cursor ec(efixData, efixSize);
        const uint8_t* pg = ec.take(16);
        if (pg) memcpy(&partitionGuid, pg, 16);

        uint32_t typeInfoCount = 0;
        if (ec.read32(typeInfoCount) && typeInfoCount < 100000) {
            typeGuids.resize(typeInfoCount);
            for (uint32_t i = 0; i < typeInfoCount; ++i) {
                const uint8_t* gp = ec.take(16);
                if (gp) memcpy(&typeGuids[i], gp, 16);
            }
        }
        uint32_t sigCount = 0;
        if (ec.read32(sigCount) && sigCount < 100000) {
            signatures.resize(sigCount);
            for (uint32_t i = 0; i < sigCount; ++i) ec.read32(signatures[i]);
        }
        ec.read32(exportedCount);
        ec.read32(dcCount);
    }

    out << "[Partition]\n";
    out << "  PartitionGuid : " << guidToString(partitionGuid).s << "\n";
    out << "  ExportedCount : " << exportedCount << "\n";
    out << "  TotalDCCount  : " << dcCount << "\n\n";

    out << describeReflChunk(reflData, reflSize, typeGuids, true); // has sigs

    return out.str();
}

// ============================================================
// Old-style RIFF EBX (raw EBX partition inside chunk)
// ============================================================
static bool findEbxInRiff(const uint8_t* fileData, size_t fileSize,
    bool riffBigEndian, const uint8_t*& outData, size_t& outSize,
    std::string& riffInfo)
{
    if (fileSize < 12) return false;

    uint32_t totalSize; memcpy(&totalSize, fileData + 4, 4);
    uint32_t formType;  memcpy(&formType, fileData + 8, 4);
    if (riffBigEndian) { totalSize = bswap(totalSize); formType = bswap(formType); }

    {
        char buf2[256];
        snprintf(buf2, sizeof(buf2),
            "=== RIFF Container ===\n\n"
            "  Magic    : %s\n"
            "  FormType : %s\n"
            "  DataSize : %u bytes\n"
            "  FileSize : %zu bytes\n\n[Chunks]\n",
            riffBigEndian ? "RIFX (big-endian)" : "RIFF (little-endian)",
            fourCCToString(formType).s, totalSize, fileSize);
        riffInfo += buf2;
    }

    const uint8_t* listStart = fileData + 12;
    uint32_t       listSize = (totalSize >= 4) ? (totalSize - 4) : 0;
    if (listStart + listSize > fileData + fileSize)
        listSize = static_cast<uint32_t>(fileData + fileSize - listStart);

    const uint8_t* foundData = nullptr;
    size_t         foundSize = 0;

    walkRiffChunks(listStart, listSize, riffBigEndian,
        [&](const RiffChunkView& chunk) -> bool {
            char line[128];
            snprintf(line, sizeof(line), "  %-8s  size=%u\n",
                fourCCToString(chunk.cc).s, chunk.size);
            riffInfo += line;

            if (chunk.size >= 4) {
                bool ns = false; int ver = 0;
                if (detectMagic(chunk.data, ns, ver)) {
                    foundData = chunk.data; foundSize = chunk.size;
                    snprintf(line, sizeof(line), "    --> EBX payload found (V%d, %s)\n",
                        ver, ns ? "big-endian" : "little-endian");
                    riffInfo += line;
                    return false;
                }
            }
            if (chunk.cc == kFourCC_LIST && chunk.size >= 4) {
                walkRiffChunks(chunk.data + 4, chunk.size - 4, riffBigEndian,
                    [&](const RiffChunkView& sub) -> bool {
                        char subLine[128];
                        snprintf(subLine, sizeof(subLine), "    %-8s  size=%u\n",
                            fourCCToString(sub.cc).s, sub.size);
                        riffInfo += subLine;
                        if (sub.size >= 4) {
                            bool ns = false; int ver = 0;
                            if (detectMagic(sub.data, ns, ver)) {
                                foundData = sub.data; foundSize = sub.size;
                                snprintf(subLine, sizeof(subLine),
                                    "      --> EBX payload found (V%d, %s)\n",
                                    ver, ns ? "big-endian" : "little-endian");
                                riffInfo += subLine;
                                return false;
                            }
                        }
                        return true;
                    });
                if (foundData) return false;
            }
            return true;
        });

    riffInfo += "\n";

    if (foundData) { outData = foundData; outSize = foundSize; return true; }
    return false;
}

// ============================================================
// Public entry point
// ============================================================
std::string EbxDescriber::describe(const uint8_t* data, size_t size)
{
    if (size < 4)
        return "Error: file too small.";

    uint32_t firstWord; memcpy(&firstWord, data, 4);

    if (firstWord == kMagicRIFF || firstWord == kMagicRIFX)
    {
        bool riffBigEndian = (firstWord == kMagicRIFX);

        uint32_t formType = 0;
        if (size >= 12) {
            memcpy(&formType, data + 8, 4);
            if (riffBigEndian) formType = bswap(formType);
        }

        bool hasEbxd = false, hasRefl = false, hasEfix = false;
        if (size >= 12) {
            uint32_t listSz; memcpy(&listSz, data + 4, 4);
            if (riffBigEndian) listSz = bswap(listSz);
            if (listSz >= 4) {
                const uint8_t* ls = data + 12;
                uint32_t       lz = listSz - 4;
                if (ls + lz > data + size) lz = static_cast<uint32_t>(data + size - ls);
                walkRiffChunks(ls, lz, riffBigEndian,
                    [&](const RiffChunkView& c) -> bool {
                        if (c.cc == kFourCC_EBXD)                         hasEbxd = true;
                        if (c.cc == kFourCC_REFL || c.cc == kFourCC_RFL2) hasRefl = true;
                        if (c.cc == kFourCC_EFIX)                         hasEfix = true;
                        return true;
                    });
            }
        }

        bool isImageEbx = hasEbxd || hasRefl
            || formType == kFourCC_EBX
            || formType == kFourCC_EBXT;

        if (isImageEbx)
            return describeImageEbx(data, size, riffBigEndian, formType, hasEfix);

        std::string riffInfo;
        const uint8_t* ebxData = nullptr;
        size_t         ebxSize = 0;

        if (findEbxInRiff(data, size, riffBigEndian, ebxData, ebxSize, riffInfo)) {
            bool needSwap = false; int version = 0;
            detectMagic(ebxData, needSwap, version);
            bool isShared = true;
            if (ebxSize >= sizeof(EbxHeader)) {
                uint32_t word4; memcpy(&word4, ebxData + 4, 4);
                if (needSwap) word4 = bswap(word4);
                size_t maxMeta = ebxSize - sizeof(EbxHeader);
                if (word4 > 0 && word4 <= maxMeta) isShared = false;
            }
            std::string ebxDesc = isShared
                ? describeSharedFlat(ebxData, ebxSize, needSwap, version)
                : describePartition(ebxData, ebxSize, needSwap, version);
            return riffInfo + ebxDesc;
        }

        return riffInfo + "Note: No EBX payload found inside this RIFF container.\n";
    }

    bool needSwap = false; int version = 0;
    if (!detectMagic(data, needSwap, version)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Error: unrecognised EBX magic 0x%08X.", firstWord);
        return buf;
    }

    bool isShared = true;
    if (size >= sizeof(EbxHeader)) {
        uint32_t word4; memcpy(&word4, data + 4, 4);
        if (needSwap) word4 = bswap(word4);
        size_t maxMeta = size - sizeof(EbxHeader);
        if (word4 > 0 && word4 <= maxMeta) isShared = false;
    }

    return isShared
        ? describeSharedFlat(data, size, needSwap, version)
        : describePartition(data, size, needSwap, version);
}