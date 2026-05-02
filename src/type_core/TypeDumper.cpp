#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winnt.h>

#include "MemoryReader.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <fstream>
#include <cassert>
#include <cctype>
#include <numeric>

#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Logging shim
// ---------------------------------------------------------------------------
#include <cstdio>
static void TD_Log(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------
struct FieldItem {
    std::string name;
    std::string type;
    int         offset = 0;
    bool        isArray = false;
    std::string arrayElemType;
};

struct TypeItem {
    std::string            name;
    std::string            ns;
    std::string            fullName;
    std::string            category;
    std::string            baseType;
    std::vector<FieldItem> fields;
};

// ---------------------------------------------------------------------------
// Helper utilities
// ---------------------------------------------------------------------------
static bool isValidTypeName(const std::string& name)
{
    const size_t len = name.size();
    if (len == 0 || len > 200) return false;

    // Build a 256-entry lookup once — plain C array, no extra include needed
    static uint8_t valid[256] = {};
    static bool built = false;
    if (!built) {
        for (int c = 'A'; c <= 'Z'; ++c) valid[c] = 1;
        for (int c = 'a'; c <= 'z'; ++c) valid[c] = 1;
        for (int c = '0'; c <= '9'; ++c) valid[c] = 1;
        valid[(unsigned char)'_'] = 1;
        valid[(unsigned char)':'] = 1;
        valid[(unsigned char)'<'] = 1;
        valid[(unsigned char)'>'] = 1;
        valid[(unsigned char)' '] = 1;
        valid[(unsigned char)'.'] = 1;
        built = true;
    }

    for (size_t i = 0; i < len; ++i)
        if (!valid[(unsigned char)name[i]]) return false;
    return true;
}

static std::string getFieldTypeName(int typeValue)
{
    switch (typeValue) {
    case 0x00: return "Inherited";
    case 0x01: return "DbObject";
    case 0x02: return "Struct";
    case 0x03: return "Pointer";
    case 0x04: return "Array";
    case 0x05: return "String";
    case 0x06: return "CString";
    case 0x07: return "Enum";
    case 0x08: return "FileRef";
    case 0x09: return "Boolean";
    case 0x0A: return "Int8";
    case 0x0B: return "UInt8";
    case 0x0C: return "Int16";
    case 0x0D: return "UInt16";
    case 0x0E: return "Int32";
    case 0x0F: return "UInt32";
    case 0x10: return "Int64";
    case 0x11: return "UInt64";
    case 0x12: return "Float32";
    case 0x13: return "Float64";
    case 0x14: return "Guid";
    case 0x15: return "Sha1";
    case 0x16: return "ResourceRef";
    case 0x17: return "TypeRef";
    case 0x18: return "BoxedValueRef";
    default: {
        char buf[32];
        snprintf(buf, sizeof(buf), "Unknown_%02X", typeValue);
        return buf;
    }
    }
}

static std::string fixTypeCasing(const std::string& t)
{
    // Fast exact matches first — no heap allocation
    if (t == "Uint8")  return "UInt8";
    if (t == "Uint16") return "UInt16";
    if (t == "Uint32") return "UInt32";
    if (t == "Uint64") return "UInt64";
    if (t == "int8")   return "Int8";
    if (t == "int16")  return "Int16";
    if (t == "int32")  return "Int32";
    if (t == "int64")  return "Int64";
    if (t.size() > 4 && t[0] == 'U' && t[1] == 'i' && t[2] == 'n' && t[3] == 't')
        return "U" + t.substr(1);
    if (t.size() > 3 && t[0] == 'i' && t[1] == 'n' && t[2] == 't')
        return "Int" + t.substr(3);
    return t;
}

// ---------------------------------------------------------------------------
// Process / module helpers
// ---------------------------------------------------------------------------
struct ModuleInfo {
    int64_t     baseAddress = 0;
    uint32_t    moduleSize = 0;
    std::string name;
    std::string path;
};

static ModuleInfo getMainModule(HANDLE hProcess)
{
    ModuleInfo mi{};
    HMODULE mods[1024]; DWORD needed = 0;
    if (!EnumProcessModules(hProcess, mods, sizeof(mods), &needed)) return mi;
    char path[MAX_PATH]{};
    GetModuleFileNameExA(hProcess, mods[0], path, MAX_PATH);
    MODULEINFO info{};
    GetModuleInformation(hProcess, mods[0], &info, sizeof(info));
    mi.baseAddress = (int64_t)(uintptr_t)info.lpBaseOfDll;
    mi.moduleSize = info.SizeOfImage;
    mi.path = path;
    std::string p(path);
    auto pos = p.find_last_of("\\/");
    mi.name = (pos != std::string::npos) ? p.substr(pos + 1) : p;
    return mi;
}

// ---------------------------------------------------------------------------
// PE section reader
// ---------------------------------------------------------------------------
struct PeSection {
    std::string name;
    uint32_t    virtualAddress = 0;
    uint32_t    virtualSize = 0;
    uint32_t    characteristics = 0; // IMAGE_SCN_* flags
};

static std::vector<PeSection> readPeSections(const std::string& exePath)
{
    // Cache: same path within one process lifetime never changes on disk
    static std::unordered_map<std::string, std::vector<PeSection>> cache;
    auto it = cache.find(exePath);
    if (it != cache.end()) return it->second;

    std::vector<PeSection> sections;
    std::ifstream f(exePath, std::ios::binary);
    if (!f) { cache[exePath] = sections; return sections; }

    uint16_t mz = 0; f.read((char*)&mz, 2); if (mz != 0x5A4D) { cache[exePath] = sections; return sections; }
    f.seekg(0x3C);
    int32_t peOff = 0; f.read((char*)&peOff, 4);
    f.seekg(peOff);
    uint32_t sig = 0; f.read((char*)&sig, 4); if (sig != 0x00004550) { cache[exePath] = sections; return sections; }
    uint16_t machine = 0; f.read((char*)&machine, 2);
    uint16_t numSec = 0; f.read((char*)&numSec, 2);
    f.seekg(12, std::ios::cur);
    uint16_t optSz = 0; f.read((char*)&optSz, 2);
    uint16_t chars = 0; f.read((char*)&chars, 2);
    long secHdrStart = peOff + 24 + optSz;

    for (int i = 0; i < numSec; ++i) {
        f.seekg(secHdrStart + i * 40);
        char nm[9]{}; f.read(nm, 8); nm[8] = 0;
        uint32_t vs = 0, va = 0, rawSz = 0, rawPtr = 0;
        f.read((char*)&vs, 4); // VirtualSize
        f.read((char*)&va, 4); // VirtualAddress
        f.read((char*)&rawSz, 4); // SizeOfRawData
        f.read((char*)&rawPtr, 4); // PointerToRawData
        // Skip PointerToRelocations(4), PointerToLinenumbers(4),
        // NumberOfRelocations(2), NumberOfLinenumbers(2) = 12 bytes
        f.seekg(12, std::ios::cur);
        uint32_t secChars = 0; f.read((char*)&secChars, 4); // Characteristics
        PeSection s;
        s.name = nm;
        s.virtualAddress = va;
        s.virtualSize = vs;
        s.characteristics = secChars;
        sections.push_back(s);
    }

    cache[exePath] = sections;
    return sections;
}

// ---------------------------------------------------------------------------
// Address validation helpers
// ---------------------------------------------------------------------------
static bool isValidAddress(int64_t addr, int64_t baseAddr)
{
    if (addr == 0) return false;
    return addr >= baseAddr && addr < baseAddr + (1024LL * 1024 * 1024);
}

static bool isValidAddressRoboto(int64_t addr, int64_t baseAddr)
{
    if (addr == 0) return false;
    if (isValidAddress(addr, baseAddr)) return true;
    int64_t heapMin = baseAddr - 0x10000000LL;
    int64_t heapMax = baseAddr + 0x10000000LL;
    return addr >= heapMin && addr < heapMax;
}

// ---------------------------------------------------------------------------
// FieldOffsetConfig
// ---------------------------------------------------------------------------
struct FieldOffsetConfig {
    int fieldCountOffset = 0x19;
    int offsetsStartAt = 0x28;
    int classFieldOffsetIdx = 2;
    int structFieldOffsetIdx = 1;
    int enumFieldOffsetIdx = 0;
};

// ---------------------------------------------------------------------------
// TypeDumper
// ---------------------------------------------------------------------------
class TypeDumper
{
public:
    std::vector<TypeItem> allTypes;
    std::string           statusMessage;
    volatile bool         cancelRequested = false;

    explicit TypeDumper() = default;

    void setForceMode(int mode) { _forceMode = mode; }

    // -----------------------------------------------------------------------
    // Public entry points
    // -----------------------------------------------------------------------
    int dumpMemory32(HANDLE hProcess, int64_t baseAddress)
    {
        _hProcess = hProcess;
        _baseAddr = baseAddress;
        _is32bit = true;
        _robotoMode = false;
        _dingoMode = false;
        _walrusMode = false;

        MemoryReader reader(hProcess, baseAddress);

        int64_t tiAddr = findTypeInfoChainStart32(reader);
        if (tiAddr == 0) {
            statusMessage = "32-bit TypeInfo chain start not found";
            return 0;
        }

        return extractTypes32(reader, tiAddr);
    }

    int dumpMemory64(HANDLE hProcess, int64_t baseAddress, const std::string& exePath)
    {
        _hProcess = hProcess;
        _baseAddr = baseAddress;
        _exePath = exePath;
        _is32bit = false;
        _robotoMode = false;
        _dingoMode = false;
        _walrusMode = false;

        // Get module size for Roboto Pass 2
        {
            auto mi = getMainModule(hProcess);
            _moduleSize = mi.moduleSize;
        }

        MemoryReader reader(hProcess, baseAddress);

        int64_t chainStart = findJupiterTypeInfoAddress(reader);
        if (chainStart == 0) {
            statusMessage = "TypeInfo chain not found";
            return 0;
        }

        return extractTypes64(reader, chainStart);
    }

    // Public accessor for the typeinfo map (used by the C API)
    const std::unordered_map<int64_t, std::string>& getTypeInfoToName() const
    {
        return _typeInfoToName;
    }

private:
    HANDLE      _hProcess = INVALID_HANDLE_VALUE;
    int64_t     _baseAddr = 0;
    bool        _is32bit = false;
    bool        _robotoMode = false;
    bool        _dingoMode = false;
    bool        _walrusMode = false;
    int         _fieldMode = 0;
    std::string _exePath;
    uint32_t    _moduleSize = 0;

    FieldOffsetConfig _config{};
    std::unordered_map<int64_t, std::string> _typeInfoToName;
    int _forceMode = -1; // -1 = auto-detect; 0-4 = forced mode index

    // -----------------------------------------------------------------------
    // Address validation
    // -----------------------------------------------------------------------
    bool isValid(int64_t addr) const { return isValidAddress(addr, _baseAddr); }
    bool isValidR(int64_t addr) const { return isValidAddressRoboto(addr, _baseAddr); }

    bool isValidForMode(int64_t addr) const
    {
        return (_robotoMode || _dingoMode) ? isValidR(addr) : isValid(addr);
    }

    bool isValid32(uint32_t ptr) const
    {
        return ptr >= 0x1000000u && ptr <= 0x10000000u;
    }

    bool isCancelled() const { return cancelRequested; }

    // -----------------------------------------------------------------------
    // String helper
    // -----------------------------------------------------------------------
    std::string readStr(MemoryReader& r, int64_t addr, int maxLen = 256) const
    {
        return r.readStringSafe(addr, maxLen);
    }

    // -----------------------------------------------------------------------
        // 32-BIT: Find TypeInfo chain start (pattern-based, no named-type dependency)
        //
        // Mirrors the logic of findJupiterTypeInfoAddress / isValidChainNode for
        // 64-bit, adapted to the 32-bit typeinfo layout used by extractSingleType32:
        //
        //   [+0x00]  uint32_t  namePtr       -> null-terminated type name string
        //   [+0x04]  uint16_t  flags         -> typeValue = (flags >> 4) & 0x1F
        //   [+0x06]  uint16_t  dataSize      -> must be > 0 and <= 10000
        //   [+0x08]  uint32_t  sharedPtr
        //   [+0x0C]  uint8_t   alignment
        //   [+0x0D]  uint8_t   fieldCount
        //   ...
        //   TYPEINFO_SIZE = 0x30 (stride used by extractTypes32)
        //
        // We validate two consecutive entries at stride 0x30, exactly as
        // isValidChainNode validates a node plus its neighbour in 64-bit mode.
        // -----------------------------------------------------------------------
    bool isValid32TypeInfoEntry(MemoryReader& reader, int64_t addr)
    {
        if (isCancelled()) return false;
        try {
            reader.position = addr;
            uint32_t namePtr = reader.readUInt();
            if (!isValid32(namePtr)) return false;

            uint16_t flags = reader.readUShort();
            uint16_t sz = reader.readUShort();
            if (sz == 0 || sz > 10000) return false;

            int typeValue = (flags >> 4) & 0x1F;
            if (typeValue != 2 && typeValue != 3 && typeValue != 8) return false;

            std::string nm = readStr(reader, (int64_t)namePtr, 256);
            if (nm.size() < 2 || !isValidTypeName(nm)) return false;

            bool hasLetter = false;
            for (char c : nm)
                if (isalpha((unsigned char)c)) { hasLetter = true; break; }
            return hasLetter;
        }
        catch (...) { return false; }
    }

    int64_t findTypeInfoChainStart32(MemoryReader& reader)
    {
        // The 32-bit typeinfo array sits somewhere in [base+0x100000, base+0x4000000]
        // We bulk-read in 1 MB chunks and stride by 4 bytes looking for a pair of
        // consecutive valid typeinfo entries (stride 0x30), which is the same
        // two-node confidence check used by isValidChainNode in 64-bit mode
        const int64_t SCAN_START = _baseAddr + 0x100000;
        const int64_t SCAN_END = _baseAddr + 0x4000000;
        const size_t  CHUNK = 0x100000; // 1 MB
        const int     ENTRY_STRIDE = 0x30;

        TD_Log("[32-bit] findTypeInfoChainStart32: scanning 0x%llX - 0x%llX",
            (unsigned long long)SCAN_START,
            (unsigned long long)SCAN_END);

        std::vector<uint8_t> buf;

        for (int64_t base = SCAN_START; base < SCAN_END; base += (int64_t)CHUNK) {
            if (isCancelled()) return 0;

            size_t toRead = (size_t)std::min((int64_t)CHUNK, SCAN_END - base);
            buf.resize(toRead);
            SIZE_T got = 0;
            if (!ReadProcessMemory(_hProcess, (LPCVOID)(uintptr_t)base,
                buf.data(), toRead, &got) || got < (size_t)ENTRY_STRIDE * 2)
                continue;

            for (size_t off = 0; off + (size_t)ENTRY_STRIDE * 2 <= got; off += 4) {
                // Check entry at off: namePtr, flags, sz, typeValue — all from buffer
                uint32_t namePtr0; uint16_t flags0, sz0;
                memcpy(&namePtr0, buf.data() + off, 4);
                memcpy(&flags0, buf.data() + off + 4, 2);
                memcpy(&sz0, buf.data() + off + 6, 2);
                if (!isValid32(namePtr0)) continue;
                if (sz0 == 0 || sz0 > 10000) continue;
                int tv0 = (flags0 >> 4) & 0x1F;
                if (tv0 != 2 && tv0 != 3 && tv0 != 8) continue;

                // Check neighbour at off + ENTRY_STRIDE — also from buffer
                uint32_t namePtr1; uint16_t flags1, sz1;
                memcpy(&namePtr1, buf.data() + off + ENTRY_STRIDE, 4);
                memcpy(&flags1, buf.data() + off + ENTRY_STRIDE + 4, 2);
                memcpy(&sz1, buf.data() + off + ENTRY_STRIDE + 6, 2);
                if (!isValid32(namePtr1)) continue;
                if (sz1 == 0 || sz1 > 10000) continue;
                int tv1 = (flags1 >> 4) & 0x1F;
                if (tv1 != 2 && tv1 != 3 && tv1 != 8) continue;

                // Structural fields pass — now validate names (pointer chase, needs reader)
                std::string nm0 = readStr(reader, (int64_t)namePtr0, 256);
                if (nm0.size() < 2 || !isValidTypeName(nm0)) continue;
                bool hasLetter0 = false;
                for (char c : nm0) if (isalpha((unsigned char)c)) { hasLetter0 = true; break; }
                if (!hasLetter0) continue;

                std::string nm1 = readStr(reader, (int64_t)namePtr1, 256);
                if (nm1.size() < 2 || !isValidTypeName(nm1)) continue;
                bool hasLetter1 = false;
                for (char c : nm1) if (isalpha((unsigned char)c)) { hasLetter1 = true; break; }
                if (!hasLetter1) continue;

                // Valid pair confirmed. Walk backwards to find true array start
                int64_t addr = base + (int64_t)off;
                int64_t arrayStart = addr;
                while (arrayStart - ENTRY_STRIDE >= SCAN_START) {
                    int64_t testAddr = arrayStart - ENTRY_STRIDE;
                    if (!isValid32TypeInfoEntry(reader, testAddr)) break;
                    arrayStart = testAddr;
                }

                TD_Log("[32-bit] findTypeInfoChainStart32: ACCEPTED at 0x%llX "
                    "(pair confirmed, walked back to 0x%llX)",
                    (unsigned long long)addr,
                    (unsigned long long)arrayStart);

                return arrayStart;
            }
        }

        TD_Log("[32-bit] findTypeInfoChainStart32: no chain start found");
        return 0;
    }

    // -----------------------------------------------------------------------
    // 32-BIT: extraction
    // -----------------------------------------------------------------------
    int extractTypes32(MemoryReader& reader, int64_t startAddress)
    {
        const int TYPEINFO_SIZE = 0x30;
        const int MAX_TYPES = 50000;
        allTypes.clear();

        std::unordered_set<std::string> extractedNames;
        std::unordered_set<int64_t>     visited;

        // Walk backwards to find array start
        int64_t arrayStart = startAddress;
        while (true) {
            int64_t  testAddr = arrayStart - TYPEINFO_SIZE;
            reader.position = testAddr;
            uint32_t nPtr = reader.readUInt();
            uint32_t fAndS = reader.readUInt();
            if (!isValid32(nPtr)) break;
            uint16_t sz = (uint16_t)((fAndS >> 16) & 0xFFFF);
            if (sz == 0 || sz > 10000) break;
            std::string nm = readStr(reader, (int64_t)nPtr);
            if (nm.empty() || !isValidTypeName(nm)) break;
            arrayStart = testAddr;
        }

        // Phase 1: contiguous array
        int64_t cur = arrayStart;
        int consecutiveFail = 0;

        while ((int)allTypes.size() < MAX_TYPES && consecutiveFail < 10) {
            if (isCancelled()) break;
            auto ti = extractSingleType32(reader, cur, extractedNames);
            if (ti) {
                allTypes.push_back(std::move(*ti));
                visited.insert(cur);
                consecutiveFail = 0;
            }
            else {
                ++consecutiveFail;
            }
            cur += TYPEINFO_SIZE;
        }

        // Phase 2: memory scan for orphaned structs
        {
            const int64_t P2_START = 0x2000000;
            const int64_t P2_END = 0x4000000;
            const int     P2_CHUNK = 0x100000; // 1 MB

            for (int64_t base = P2_START; base < P2_END; base += P2_CHUNK) {
                if (isCancelled()) break;

                int toRead = (int)std::min((int64_t)P2_CHUNK, P2_END - base);
                reader.position = base;
                std::vector<uint8_t> buf = reader.readBytes(toRead);
                if ((int)buf.size() < 16) continue;

                for (size_t off = 0; off + 16 <= buf.size(); off += 4) {
                    int64_t pos = base + (int64_t)off;
                    if (visited.count(pos)) continue;

                    uint32_t tNamePtr, fAndS, sharedP, alignFc;
                    memcpy(&tNamePtr, buf.data() + off, 4);
                    memcpy(&fAndS, buf.data() + off + 4, 4);
                    memcpy(&sharedP, buf.data() + off + 8, 4);
                    memcpy(&alignFc, buf.data() + off + 12, 4);

                    if (tNamePtr < 0x2000000u || tNamePtr > 0x10000000u) continue;

                    uint16_t flags = (uint16_t)(fAndS & 0xFFFF);
                    uint16_t sz = (uint16_t)(fAndS >> 16);
                    uint8_t  fc = (uint8_t)((alignFc >> 8) & 0xFF);

                    if (sz == 0 || sz > 0x8000) continue;
                    int tv = (flags >> 4) & 0x1F;
                    if (tv != 2 && tv != 3 && tv != 8) continue;
                    if (fc > 200) continue;

                    // Name and full extraction require pointer chasing — use reader
                    // Must restore reader.position to pos before each call
                    std::string nm = readStr(reader, (int64_t)tNamePtr);
                    if (nm.empty() || !isValidTypeName(nm) || nm.size() == 1) continue;
                    if (nm.find(' ') != std::string::npos) continue;
                    if (nm.size() > 3) {
                        bool allSame = true;
                        for (char c : nm) if (c != nm[0]) { allSame = false; break; }
                        if (allSame) continue;
                    }
                    if (extractedNames.count(nm)) continue;

                    reader.position = pos;
                    auto ti = extractSingleType32(reader, pos, extractedNames);
                    if (!ti) continue;

                    bool hasFields = !ti->fields.empty();
                    if (!hasFields && fc == 0) {
                        if (!isupper((unsigned char)nm[0])) continue;
                        if (nm.size() < 3) continue;
                        int alnum = 0;
                        for (char c : nm) if (isalnum((unsigned char)c)) ++alnum;
                        if (alnum < (int)(nm.size() * 0.7)) continue;
                    }
                    else if (!hasFields && fc > 0) continue;

                    allTypes.push_back(std::move(*ti));
                    visited.insert(pos);
                }
            }
        }

        return (int)allTypes.size();
    }

    std::optional<TypeItem> extractSingleType32(MemoryReader& reader, int64_t address,
        std::unordered_set<std::string>& extractedNames)
    {
        reader.position = address;
        uint32_t namePtr = reader.readUInt();
        uint32_t fAndS = reader.readUInt();
        uint32_t sharedP = reader.readUInt();
        uint32_t alignFc = reader.readUInt();

        uint16_t flags = (uint16_t)(fAndS & 0xFFFF);
        uint16_t sz = (uint16_t)((fAndS >> 16) & 0xFFFF);
        uint8_t  fc = (uint8_t)((alignFc >> 8) & 0xFF);

        if (!isValid32(namePtr)) return std::nullopt;
        if (sz == 0 || sz > 10000) return std::nullopt;

        std::string typeName = readStr(reader, (int64_t)namePtr);
        if (typeName.empty() || !isValidTypeName(typeName)) return std::nullopt;
        if (extractedNames.count(typeName)) return std::nullopt;
        extractedNames.insert(typeName);

        reader.position = address + 0x10;
        std::vector<uint32_t> offsets(7);
        for (auto& o : offsets) o = reader.readUInt();

        int         tv = (flags >> 4) & 0x1F;
        std::string category = (tv == 2) ? "Structs" : (tv == 8) ? "Enums" : "Classes";

        std::string baseType;
        if (!offsets.empty() && isValid32(offsets[0])) {
            reader.position = (int64_t)offsets[0];
            uint32_t parentTI = reader.readUInt();
            if (isValid32(parentTI)) {
                reader.position = (int64_t)parentTI;
                uint32_t parentNP = reader.readUInt();
                if (isValid32(parentNP)) {
                    baseType = readStr(reader, (int64_t)parentNP);
                    if (!isValidTypeName(baseType) ||
                        baseType == "Asset" ||
                        baseType == "DataContainerPolicyAsset")
                        baseType.clear();
                }
            }
        }

        std::vector<FieldItem> fields;
        if (fc > 0 && fc < 200) {
            int64_t fieldArrayPtr = 0;
            if (tv == 2 && offsets.size() > 1 && isValid32(offsets[1]))
                fieldArrayPtr = (int64_t)offsets[1];
            else if (tv == 3 && offsets.size() > 2 && isValid32(offsets[2]))
                fieldArrayPtr = (int64_t)offsets[2];
            else if (tv == 8 && !offsets.empty() && isValid32(offsets[0]))
                fieldArrayPtr = (int64_t)offsets[0];

            if (fieldArrayPtr != 0)
                fields = extractFields32(reader, fieldArrayPtr, fc, tv, typeName, false);
        }

        TypeItem ti;
        ti.name = typeName;
        ti.fullName = typeName;
        ti.category = category;
        ti.baseType = baseType;
        ti.fields = std::move(fields);
        return ti;
    }

    // -----------------------------------------------------------------------
    // 32-BIT: Field extraction
    // -----------------------------------------------------------------------
    std::vector<FieldItem> extractFields32(MemoryReader& reader, int64_t fieldArrayPtr,
        int fieldCount, int typeValue, const std::string& typeName, bool dbg)
    {
        std::vector<FieldItem> fields;

        for (int i = 0; i < std::min(fieldCount, 500); ++i) {
            // Seek to the exact start of this field entry every iteration so
            // that type-info pointer dereferencing never corrupts the position
            int64_t fieldEntryBase = fieldArrayPtr + (int64_t)i * 12;
            reader.position = fieldEntryBase;

            uint32_t fnPtr = reader.readUInt();
            uint16_t fFlags = reader.readUShort();
            uint16_t fOff = reader.readUShort();
            uint32_t ftPtr = reader.readUInt();

            if (!isValid32(fnPtr)) break;

            std::string fieldName = readStr(reader, (int64_t)fnPtr, 100);
            if (fieldName.empty() || !isValidTypeName(fieldName)) break;

            if (typeValue == 8) {
                FieldItem fi;
                fi.name = fieldName;
                fi.type = std::to_string((int32_t)ftPtr);
                fi.offset = (int16_t)fOff;
                fields.push_back(std::move(fi));
            }
            else {
                std::string ftName = "Unknown";
                bool        isArray = false;
                std::string arrElem;

                if (isValid32(ftPtr)) {
                    reader.position = (int64_t)ftPtr;
                    uint32_t actualTI = reader.readUInt();
                    if (isValid32(actualTI)) {
                        reader.position = (int64_t)actualTI;
                        uint32_t tnPtr = reader.readUInt();
                        uint16_t tFlags = reader.readUShort();
                        uint16_t tSz = reader.readUShort();
                        int      atv = (tFlags >> 4) & 0x1F;

                        if (atv == 4) {
                            isArray = true;
                            ftName = "List";
                            reader.position = (int64_t)actualTI + 0x10;
                            for (int ao = 0; ao < 7 && arrElem.empty(); ++ao) {
                                uint32_t aOff = reader.readUInt();
                                if (!isValid32(aOff)) continue;
                                reader.position = (int64_t)aOff;
                                uint32_t eTI = reader.readUInt();
                                if (!isValid32(eTI)) { reader.position = (int64_t)aOff + 4; continue; }
                                reader.position = (int64_t)eTI;
                                uint32_t eNP = reader.readUInt();
                                if (!isValid32(eNP)) continue;
                                std::string en = readStr(reader, (int64_t)eNP, 100);
                                if (!en.empty() && isValidTypeName(en)) arrElem = fixTypeCasing(en);
                            }
                            if (arrElem.empty()) arrElem = "Unknown";
                        }
                        else if (isValid32(tnPtr)) {
                            std::string tn = readStr(reader, (int64_t)tnPtr, 100);
                            if (!tn.empty() && isValidTypeName(tn)) ftName = fixTypeCasing(tn);
                        }
                    }
                }

                FieldItem fi;
                fi.name = fieldName;
                fi.type = ftName;
                fi.offset = (int)fOff;
                fi.isArray = isArray;
                fi.arrayElemType = arrElem;
                fields.push_back(std::move(fi));
            }
        }
        return fields;
    }

    // -----------------------------------------------------------------------
    // 64-BIT: Find TypeInfo chain
    // -----------------------------------------------------------------------
    int64_t findJupiterTypeInfoAddress(MemoryReader& reader)
    {
        int64_t result = legacyFindTypeInfoAddress(reader);
        if (result != 0) return result;
        return findTypeInfoChainStart(reader);
    }

    // -----------------------------------------------------------------------
    // Pattern scanner helpers (used by legacyFindTypeInfoAddress)
    // -----------------------------------------------------------------------
    struct PatternByte {
        uint8_t value;
        bool    wildcard;
    };

    static std::vector<PatternByte> parsePattern(const std::string& pat)
    {
        std::vector<PatternByte> out;
        size_t i = 0;
        while (i < pat.size()) {
            while (i < pat.size() && pat[i] == ' ') ++i;
            if (i >= pat.size()) break;
            PatternByte pb{};
            if (pat[i] == '?') {
                pb.wildcard = true; pb.value = 0;
                // consume one or two '?' characters
                ++i;
                if (i < pat.size() && pat[i] == '?') ++i;
            }
            else if (i + 1 < pat.size() &&
                ((pat[i] >= '0' && pat[i] <= '9') ||
                    (pat[i] >= 'A' && pat[i] <= 'F') ||
                    (pat[i] >= 'a' && pat[i] <= 'f')))
            {
                pb.wildcard = false;
                pb.value = (uint8_t)std::stoul(pat.substr(i, 2), nullptr, 16);
                i += 2;
            }
            else {
                ++i; // skip unexpected character
                continue;
            }
            out.push_back(pb);
        }
        return out;
    }

    static std::vector<int64_t> scanPattern(HANDLE hProcess,
        int64_t scanStart, int64_t scanEnd,
        const std::vector<PatternByte>& pat)
    {
        std::vector<int64_t> results;
        if (pat.empty() || scanEnd <= scanStart) return results;

        const size_t CHUNK = 0x100000; // 1 MB
        std::vector<uint8_t> buf;

        for (int64_t pos = scanStart; pos < scanEnd; ) {
            size_t toRead = (size_t)std::min((int64_t)CHUNK, scanEnd - pos);
            buf.resize(toRead);
            SIZE_T bytesRead = 0;
            if (!ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)pos,
                buf.data(), toRead, &bytesRead) || bytesRead == 0) {
                pos += (int64_t)CHUNK;
                continue;
            }

            size_t searchLen = bytesRead;
            for (size_t k = 0; k + pat.size() <= searchLen; ++k) {
                bool match = true;
                for (size_t j = 0; j < pat.size(); ++j) {
                    if (!pat[j].wildcard && buf[k + j] != pat[j].value) {
                        match = false;
                        break;
                    }
                }
                if (match) results.push_back(pos + (int64_t)k);
            }

            // Step forward, overlapping by pat.size()-1 to catch cross-chunk matches
            int64_t step = (int64_t)(bytesRead - (pat.size() - 1));
            if (step <= 0) step = 1;
            pos += step;
        }
        return results;
    }

    // -----------------------------------------------------------------------
    // Legacy pattern-scan approach
    // -----------------------------------------------------------------------
    int64_t legacyFindTypeInfoAddress(MemoryReader& reader)
    {
        if (_moduleSize == 0) return 0;

        // Only scan sections with the executable flag set
        // Name-based filtering broke games like SWBF2 whose main code section
        // has a non-standard name — using IMAGE_SCN_MEM_EXECUTE is reliable
        // regardless of what the section is named
        std::vector<std::pair<int64_t, int64_t>> codeRanges; // [start, end)
        if (!_exePath.empty()) {
            auto sections = readPeSections(_exePath);
            for (auto& s : sections) {
                if ((s.characteristics & 0x20000000u) == 0) continue;
                if (s.virtualSize == 0) continue;
                int64_t secStart = _baseAddr + (int64_t)s.virtualAddress;
                int64_t secEnd = secStart + (int64_t)s.virtualSize;
                TD_Log("LegacyFindTypeInfoAddress: executable section '%s' 0x%llX - 0x%llX (%lld MB)",
                    s.name.c_str(),
                    (unsigned long long)secStart, (unsigned long long)secEnd,
                    (long long)((secEnd - secStart) >> 20));
                codeRanges.push_back({ secStart, secEnd });
            }
        }
        // Fall back to full module scan if PE read failed or yielded nothing
        if (codeRanges.empty()) {
            TD_Log("LegacyFindTypeInfoAddress: no executable sections found, falling back to full module scan");
            codeRanges.push_back({ _baseAddr, _baseAddr + (int64_t)_moduleSize });
        }

        static const char* kPatterns[] = {
            "48 8B 05 ?? ?? ?? ?? 48 89 41 08 48 89 0D ?? ?? ?? ??",
            "48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ??",
            "48 39 1D ?? ?? ?? ?? 7? ?? 48 8B 05 ?? ?? ?? ??"
        };

        // Pre-parse all patterns once
        std::vector<std::vector<PatternByte>> parsedPats;
        parsedPats.reserve(3);
        for (const char* raw : kPatterns)
            parsedPats.push_back(parsePattern(raw));

        // Find the longest pattern — used to size the chunk overlap
        size_t maxPatLen = 0;
        for (auto& p : parsedPats)
            if (p.size() > maxPatLen) maxPatLen = p.size();

        const size_t CHUNK = 0x200000; // 2 MB
        const size_t OVERLAP = maxPatLen > 0 ? maxPatLen - 1 : 0;

        std::vector<uint8_t> buf;
        buf.reserve(CHUNK + OVERLAP);

        HANDLE hProc = _hProcess;

        // ---- Iterate only over the code sections identified above ----
        for (auto& [rangeStart, rangeEnd] : codeRanges) {
            TD_Log("LegacyFindTypeInfoAddress: scanning code section 0x%llX - 0x%llX (%lld MB)",
                (unsigned long long)rangeStart, (unsigned long long)rangeEnd,
                (long long)((rangeEnd - rangeStart) >> 20));

            for (int64_t pos = rangeStart; pos < rangeEnd; ) {
                size_t toRead = (size_t)std::min((int64_t)(CHUNK + OVERLAP), rangeEnd - pos);
                buf.resize(toRead);

                SIZE_T bytesRead = 0;
                if (!ReadProcessMemory(hProc, (LPCVOID)(uintptr_t)pos,
                    buf.data(), toRead, &bytesRead) || bytesRead == 0) {
                    pos += (int64_t)CHUNK;
                    continue;
                }

                // Test every pattern against this chunk
                for (int pi = 0; pi < (int)parsedPats.size(); ++pi) {
                    const auto& pat = parsedPats[pi];
                    if (pat.empty() || pat.size() > bytesRead) continue;

                    for (size_t k = 0; k + pat.size() <= bytesRead; ++k) {
                        if (!pat[0].wildcard && buf[k] != pat[0].value) continue;

                        bool match = true;
                        for (size_t j = 1; j < pat.size(); ++j) {
                            if (!pat[j].wildcard && buf[k + j] != pat[j].value) {
                                match = false;
                                break;
                            }
                        }
                        if (!match) continue;

                        int64_t hitAddr = pos + (int64_t)k;
                        try {
                            reader.position = hitAddr + 3;
                            int32_t relOffset = (int32_t)reader.readUInt();
                            int64_t chainNodePtrAddr = hitAddr + 3 + (int64_t)relOffset + 4;
                            reader.position = chainNodePtrAddr;
                            int64_t chainNodeAddr = reader.readLong();

                            if (chainNodeAddr == 0 || !isValid(chainNodeAddr)) continue;

                            reader.position = chainNodeAddr;
                            int64_t typeInfoPtr = reader.readLong();
                            int64_t nextPtr = reader.readLong();

                            if (typeInfoPtr == 0 || !isValid(typeInfoPtr))        continue;
                            if (nextPtr == 0 || !isValid(nextPtr))                continue;
                            if (nextPtr == typeInfoPtr || nextPtr == chainNodeAddr) continue;

                            reader.position = typeInfoPtr;
                            int64_t namePtr = reader.readLong();
                            if (namePtr == 0 || !isValid(namePtr)) continue;

                            std::string nm = readStr(reader, namePtr, 256);
                            if (nm.size() < 2 || !isValidTypeName(nm)) continue;

                            TD_Log("LegacyFindTypeInfoAddress: ACCEPTED pattern[%d] "
                                "hit=0x%llX chainNodePtrAddr=0x%llX chainNodeAddr=0x%llX "
                                "typeInfoPtr=0x%llX name='%s' nextPtr=0x%llX",
                                pi,
                                (unsigned long long)hitAddr,
                                (unsigned long long)chainNodePtrAddr,
                                (unsigned long long)chainNodeAddr,
                                (unsigned long long)typeInfoPtr,
                                nm.c_str(),
                                (unsigned long long)nextPtr);

                            return chainNodeAddr;
                        }
                        catch (...) { continue; }
                    }
                }

                int64_t step = (int64_t)(bytesRead > OVERLAP ? bytesRead - OVERLAP : 1);
                pos += step;
            }
        }

        TD_Log("LegacyFindTypeInfoAddress: no match found");
        return 0;
    }

    int64_t findTypeInfoChainStart(MemoryReader& reader)
    {
        if (_exePath.empty()) return searchMemoryRangeFallback(reader);

        auto sections = readPeSections(_exePath);
        static const std::vector<std::string> priority = {
            "typeinfo",".srdata",".data2",".rodata",".rdata",".data"
        };

        std::vector<std::pair<int, int>> sorted;
        for (int si = 0; si < (int)sections.size(); ++si) {
            for (int pi = 0; pi < (int)priority.size(); ++pi) {
                if (sections[si].name == priority[pi]) {
                    sorted.push_back({ pi, si });
                    break;
                }
            }
        }
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });

        // Pre-read entire section into a local buffer so we avoid one
        // ReadProcessMemory call per 8-byte probe
        const size_t BULK = 0x800000; // 8 MB max per read
        std::vector<uint8_t> sectionBuf;

        for (auto& [pri, si] : sorted) {
            const PeSection& sec = sections[si];
            int64_t secStart = _baseAddr + (int64_t)sec.virtualAddress;
            int64_t fullSize = (int64_t)sec.virtualSize;
            int64_t fineSize = std::min(fullSize, (int64_t)0x500000);

            TD_Log("Searching section: %s (VA=0x%X Size=0x%X)",
                sec.name.c_str(), sec.virtualAddress, sec.virtualSize);

            // ---- fine scan (stride 8) up to fineSize ----
            for (int64_t base = 0; base < fineSize; base += (int64_t)BULK) {
                if (isCancelled()) return 0;
                size_t toRead = (size_t)std::min((int64_t)BULK, fineSize - base);
                sectionBuf.resize(toRead);
                SIZE_T got = 0;
                if (!ReadProcessMemory(_hProcess,
                    (LPCVOID)(uintptr_t)(secStart + base),
                    sectionBuf.data(), toRead, &got) || got < 16) {
                    TD_Log("  [RPM] FAILED at 0x%llX toRead=%zu got=%zu err=%lu",
                        (unsigned long long)(secStart + base), toRead, got, GetLastError());
                    continue;
                }
                TD_Log("  [RPM] OK at 0x%llX got=%zu", (unsigned long long)(secStart + base), got);

                for (size_t off = 0; off + 16 <= got; off += 8) {
                    if (isCancelled()) return 0;
                    int64_t tryAddr = secStart + base + (int64_t)off;
                    int64_t tiPtr, nextPtr;
                    memcpy(&tiPtr, sectionBuf.data() + off, 8);
                    memcpy(&nextPtr, sectionBuf.data() + off + 8, 8);
                    if (isValidChainNode(reader, tryAddr, tiPtr, nextPtr))
                        return tryAddr;
                }
            }

            // ---- coarse scan (stride 0x100) for remainder ----
            for (int64_t base = fineSize; base < fullSize; base += (int64_t)BULK) {
                if (isCancelled()) return 0;
                size_t toRead = (size_t)std::min((int64_t)BULK, fullSize - base);
                sectionBuf.resize(toRead);
                SIZE_T got = 0;
                if (!ReadProcessMemory(_hProcess,
                    (LPCVOID)(uintptr_t)(secStart + base),
                    sectionBuf.data(), toRead, &got) || got < 16)
                    continue;

                for (size_t off = 0; off + 16 <= got; off += 0x100) {
                    if (isCancelled()) return 0;
                    int64_t tryAddr = secStart + base + (int64_t)off;
                    int64_t tiPtr, nextPtr;
                    memcpy(&tiPtr, sectionBuf.data() + off, 8);
                    memcpy(&nextPtr, sectionBuf.data() + off + 8, 8);
                    if (isValidChainNode(reader, tryAddr, tiPtr, nextPtr))
                        return tryAddr;
                }
            }

            TD_Log("  Nothing found in %s", sec.name.c_str());
        }

        return searchMemoryRangeFallback(reader);
    }

    int64_t searchMemoryRangeFallback(MemoryReader& reader)
    {
        TD_Log("Fallback: scanning 0x200000-0x600000 from base");
        const int64_t SCAN_START = _baseAddr + 0x200000;
        const int64_t SCAN_END = _baseAddr + 0x600000;
        const size_t  BULK = 0x200000;

        std::vector<uint8_t> buf;
        for (int64_t base = SCAN_START; base < SCAN_END; base += (int64_t)BULK) {
            size_t toRead = (size_t)std::min((int64_t)BULK, SCAN_END - base);
            buf.resize(toRead);
            SIZE_T got = 0;
            if (!ReadProcessMemory(_hProcess, (LPCVOID)(uintptr_t)base,
                buf.data(), toRead, &got) || got < 16)
                continue;
            for (size_t off = 0; off + 16 <= got; off += 8) {
                int64_t tryAddr = base + (int64_t)off;
                int64_t tiPtr, nextPtr;
                memcpy(&tiPtr, buf.data() + off, 8);
                memcpy(&nextPtr, buf.data() + off + 8, 8);
                if (isValidChainNode(reader, tryAddr, tiPtr, nextPtr))
                    return tryAddr;
            }
        }
        return 0;
    }

    bool isValidChainNode(MemoryReader& reader, int64_t nodeAddr,
        int64_t tiPtr, int64_t nextPtr)
    {
        if (tiPtr == 0 || !isValid(tiPtr))  return false;
        if (nextPtr == 0 || !isValid(nextPtr) ||
            nextPtr == tiPtr || nextPtr == nodeAddr) return false;

        try {
            // Validate next node's typeInfoPtr is a plausible module address
            reader.position = nextPtr;
            int64_t nextTiPtr = reader.readLong();
            if (nextTiPtr == 0 || !isValid(nextTiPtr) || nextTiPtr == tiPtr) return false;

            // Validate current node's name
            reader.position = tiPtr;
            int64_t namePtr = reader.readLong();
            if (namePtr == 0 || !isValid(namePtr)) return false;

            std::string nm = readStr(reader, namePtr, 256);
            if (nm.size() < 2 || nm.size() > 100 || !isValidTypeName(nm)) return false;
            bool hasLetter = false;
            for (char c : nm) if (isalpha((unsigned char)c)) { hasLetter = true; break; }
            if (!hasLetter) return false;

            // Validate flags
            reader.position = tiPtr + 0x08;
            uint16_t flags = reader.readUShort();
            int ts = ((flags >> 1) >> 4) & 0x1F;
            int td = (flags >> 4) & 0x1F;
            if (ts > 0x1C && td > 0x1C) return false;

            TD_Log("  *** ACCEPTED: 0x%llX tiPtr=0x%llX name='%s' flags=0x%04X nextPtr=0x%llX",
                (unsigned long long)nodeAddr, (unsigned long long)tiPtr,
                nm.c_str(), flags, (unsigned long long)nextPtr);
            return true;
        }
        catch (...) { return false; }
    }

    // -----------------------------------------------------------------------
    // 64-BIT: Auto-detect field offset pattern
    // -----------------------------------------------------------------------
    int detectFieldOffsetPattern(MemoryReader& reader, int64_t startAddress)
    {
        const int MAX_STRING_LENGTH = 256;
        const int FORCE_MODE = _forceMode; // -1 = auto, set externally for forced dumps

        if (FORCE_MODE >= 0 && FORCE_MODE <= 5)
        {
            TD_Log("\n*** FORCE MODE ENABLED: %d ***", FORCE_MODE);
            if (FORCE_MODE == 0) _config = { 0x19, 0x28, 2, 1, 0 }; // Jupiter
            else if (FORCE_MODE == 1) _config = { 0x1A, 0x20, 2, 1, 0 }; // Havana
            else if (FORCE_MODE == 2) _config = { 0x19, 0x20, 2, 6, 1 }; // Walrus
            else if (FORCE_MODE == 3) _config = { 0x30, 0x38, 1, 0, 0 }; // Roboto
            else if (FORCE_MODE == 4) _config = { 0x19, 0x40, 0, 0, 0 }; // Skate/Dingo

            if (FORCE_MODE != 5)
            {
                TD_Log("*** FORCED MODE %d CONFIGURED ***\n", FORCE_MODE);
                _dingoMode = (FORCE_MODE == 4);
                _walrusMode = (FORCE_MODE == 2);
                _robotoMode = (FORCE_MODE == 3);
                return FORCE_MODE;
            }
        }

        // ----------------------------------------------------------------
        // TEST 0: DINGO/SKATE — dedicated "typeinfo" PE section
        // ----------------------------------------------------------------
        TD_Log("\n[TEST 0] DINGO/SKATE: checking for dedicated 'typeinfo' PE section...");

        bool    dingoConfirmed = false;
        bool    typeInfoSecPresent = false;
        int64_t tiSecStart = 0, tiSecEnd = 0;

        if (!_exePath.empty())
        {
            auto sections = readPeSections(_exePath);
            for (auto& s : sections)
            {
                if (s.name == "typeinfo" && s.virtualSize > 0)
                {
                    tiSecStart = _baseAddr + (int64_t)s.virtualAddress;
                    tiSecEnd = tiSecStart + (int64_t)s.virtualSize;
                    TD_Log("  Found 'typeinfo' section: 0x%llX - 0x%llX (size=0x%X)",
                        (unsigned long long)tiSecStart,
                        (unsigned long long)tiSecEnd,
                        s.virtualSize);
                    break;
                }
            }

            if (tiSecStart != 0)
            {
                // Gate 1: scan for hashed entries
                int     confirmHits = 0;
                int64_t scanEnd = std::min(tiSecStart + 0x4000LL, tiSecEnd);
                for (int64_t pos = tiSecStart; pos + 0x10 <= scanEnd && confirmHits < 3; pos += 8)
                {
                    try {
                        reader.position = pos;
                        uint32_t nameHash = reader.readUInt();
                        if (nameHash == 0) continue;

                        reader.position = pos + 0x06;
                        uint16_t rawFlags = reader.readUShort();
                        uint16_t shifted = rawFlags >> 1;
                        int typeVal = (shifted >> 4) & 0x1F;

                        if (typeVal == 2 || typeVal == 3 || typeVal == 8)
                        {
                            reader.position = pos + 0x18;
                            int64_t nsPtr = reader.readLong();
                            bool nsOk = (nsPtr == 0) || isValid(nsPtr);
                            if (!nsOk) continue;
                            ++confirmHits;
                            TD_Log("  Confirm hit [%d] @0x%llX: hash=0x%08X typeVal=%d",
                                confirmHits, (unsigned long long)pos, nameHash, typeVal);
                        }
                    }
                    catch (...) { continue; }
                }

                if (confirmHits >= 1)
                {
                    typeInfoSecPresent = true;
                    TD_Log("  typeinfo section validated (%d hashed entries found)", confirmHits);
                }
                else
                {
                    TD_Log("  typeinfo section present but no valid hashed entries - not Dingo");
                }
            }

            // Gate 2: walk chain and count readable names
            if (typeInfoSecPresent)
            {
                TD_Log("  Gate 2: probing chain for readable names...");
                int     readableNames = 0, chainChecked = 0;
                int64_t probeAddr = startAddress;
                std::unordered_set<int64_t> probeSeen;

                while (probeAddr != 0 && chainChecked < 10)
                {
                    if (probeSeen.count(probeAddr)) break;
                    probeSeen.insert(probeAddr);
                    try {
                        reader.position = probeAddr;
                        int64_t tiPtr = reader.readLong();
                        int64_t nextPtr = reader.readLong();
                        ++chainChecked;

                        if (tiPtr != 0 && isValid(tiPtr))
                        {
                            reader.position = tiPtr;
                            int64_t np = reader.readLong();
                            if (np != 0 && isValid(np))
                            {
                                std::string nm = readStr(reader, np, MAX_STRING_LENGTH);
                                if (nm.size() >= 3 && isValidTypeName(nm))
                                {
                                    bool hasLetter = false;
                                    for (char c : nm) if (isalpha((unsigned char)c)) { hasLetter = true; break; }
                                    if (hasLetter)
                                    {
                                        ++readableNames;
                                        TD_Log("  Chain readable name [%d]: '%s'", readableNames, nm.c_str());
                                    }
                                }
                            }
                        }
                        probeAddr = (nextPtr != 0 && isValid(nextPtr)) ? nextPtr : 0;
                    }
                    catch (...) { break; }
                }

                double hitRate = chainChecked > 0 ? (double)readableNames / chainChecked : 0.0;
                bool   chainLikeRoboto = (readableNames >= 3 && hitRate >= 0.5);
                TD_Log("  Gate 2 result: %d readable names in first %d chain nodes (hit rate=%.0f%%)",
                    readableNames, chainChecked, hitRate * 100.0);

                if (!chainLikeRoboto)
                {
                    dingoConfirmed = true;
                    TD_Log("  DINGO/SKATE confirmed: chain hit rate %.0f%% < 50%% or < 3 readable names",
                        hitRate * 100.0);
                }
                else
                {
                    TD_Log("  DINGO rejected: chain has %d readable names at %.0f%% - treating as Roboto-family",
                        readableNames, hitRate * 100.0);
                }
            }
        }

        if (dingoConfirmed)
        {
            _config = { 0x19, 0x40, 0, 0, 0 };
            _dingoMode = true;
            _walrusMode = false;
            _robotoMode = false;
            TD_Log("*** AUTO-DETECTED: DINGO/SKATE (mode 4) ***\n");
            return 4;
        }

        TD_Log("  TEST 0: Not DINGO/SKATE, continuing to other tests...\n");

        // ----------------------------------------------------------------
        // Collect candidate classes from the linked list
        // ----------------------------------------------------------------
        struct Candidate { int64_t typeInfo; std::string name; uint32_t size; };
        std::vector<Candidate> candidates;

        {
            int64_t cur = startAddress;
            int     iter = 0;
            while (cur != 0 && iter < 500 && (int)candidates.size() < 10)
            {
                try {
                    reader.position = cur;
                    int64_t tiPtr = reader.readLong();
                    int64_t nextPtr = reader.readLong();

                    if (tiPtr != 0 && isValid(tiPtr))
                    {
                        reader.position = tiPtr;
                        int64_t  np = reader.readLong();
                        uint16_t fl = reader.readUShort();
                        reader.position = tiPtr + 0x0A; uint32_t szNorm = reader.readUInt();
                        reader.position = tiPtr + 0x0E; uint16_t szR = reader.readUShort();
                        uint32_t sz = (szNorm > 0 && szNorm < 0x100000) ? szNorm : (uint32_t)szR;

                        if (np != 0 && isValid(np))
                        {
                            std::string nm = readStr(reader, np, MAX_STRING_LENGTH);
                            if (!nm.empty() && isValidTypeName(nm))
                            {
                                int wt = ((fl >> 1) >> 4) & 0x1F;
                                int nt = (fl >> 4) & 0x1F;
                                bool isClass = (wt == 3 || nt == 3) &&
                                    (sz > 48 || sz == 0 || (uint16_t)szR > 48);
                                if (isClass)
                                {
                                    candidates.push_back({ tiPtr, nm, sz });
                                    TD_Log("  Found candidate: %s (size=%u, heatSize=%u, flags=0x%04X)",
                                        nm.c_str(), sz, (unsigned)szR, (unsigned)fl);
                                }
                            }
                        }
                        cur = nextPtr;
                    }
                    else cur = nextPtr;
                }
                catch (...) { break; }
                ++iter;
            }
        }

        // Fallback: accept any type with a valid category
        if (candidates.empty())
        {
            int64_t cur = startAddress;
            int     iter = 0;
            while (cur != 0 && iter < 200)
            {
                try {
                    reader.position = cur;
                    int64_t tiPtr = reader.readLong();
                    int64_t nextPtr = reader.readLong();
                    if (tiPtr != 0 && isValid(tiPtr))
                    {
                        reader.position = tiPtr;
                        int64_t  np = reader.readLong();
                        uint16_t fl = reader.readUShort();
                        if (np != 0 && isValid(np))
                        {
                            std::string nm = readStr(reader, np, MAX_STRING_LENGTH);
                            if (!nm.empty() && isValidTypeName(nm))
                            {
                                int nt = (fl >> 4) & 0x1F;
                                int wt = ((fl >> 1) >> 4) & 0x1F;
                                if ((nt >= 2 && nt <= 8) || (wt >= 2 && wt <= 8))
                                {
                                    candidates.push_back({ tiPtr, nm, 0 });
                                    TD_Log("  Found fallback candidate: %s (normalType=%d WalrusType=%d)",
                                        nm.c_str(), nt, wt);
                                    break;
                                }
                            }
                        }
                        cur = nextPtr;
                    }
                    else cur = nextPtr;
                }
                catch (...) { break; }
                ++iter;
            }
        }

        if (candidates.empty())
        {
            TD_Log("No test classes found, defaulting to Jupiter");
            _config = { 0x19, 0x28, 2, 1, 0 };
            return 0;
        }

        // ----------------------------------------------------------------
        // Analyze first candidate — log raw memory layout
        // ----------------------------------------------------------------
        auto& tc = candidates[0];
        TD_Log("\n=== ANALYZING: %s ===", tc.name.c_str());
        TD_Log("TypeInfo at 0x%llX", (unsigned long long)tc.typeInfo);

        {
            reader.position = tc.typeInfo;
            int64_t  namePtr_ = reader.readLong();
            uint16_t flags_ = reader.readUShort();
            reader.position = tc.typeInfo + 0x0E;
            uint16_t heatSize_ = reader.readUShort();
            reader.position = tc.typeInfo + 0x10;
            int64_t  nsPtr_ = reader.readLong();

            TD_Log("\n=== STANDARD STRUCTURE FIELDS (ROBOTO-CORRECTED) ===");
            TD_Log("+0x00 NamePtr:   0x%llX", (unsigned long long)namePtr_);
            TD_Log("+0x08 Flags:     0x%04X", (unsigned)flags_);
            TD_Log("      Normal:    type=%d", (flags_ >> 4) & 0x1F);
            TD_Log("      Walrus:    type=%d", ((flags_ >> 1) >> 4) & 0x1F);
            TD_Log("+0x0E Size:      %u (UInt16)", (unsigned)heatSize_);
            TD_Log("+0x10 Namespace: 0x%llX %s",
                (unsigned long long)nsPtr_,
                isValid(nsPtr_) ? "[VALID]" : "[INVALID]");
        }

        // Read field count bytes
        reader.position = tc.typeInfo + 0x18;
        uint8_t fc18 = reader.readByte();
        uint8_t fc19 = reader.readByte();
        uint8_t fc1A = reader.readByte();
        uint8_t fc1B = reader.readByte();
        uint8_t fc1C = reader.readByte();

        TD_Log("\n=== FIELD COUNT CANDIDATES ===");
        TD_Log("+0x18: %u", (unsigned)fc18);
        TD_Log("+0x19: %u", (unsigned)fc19);
        TD_Log("+0x1A: %u", (unsigned)fc1A);
        TD_Log("+0x1B: %u", (unsigned)fc1B);
        TD_Log("+0x1C: %u", (unsigned)fc1C);

        // Read offset arrays (+0x20, +0x28, +0x30, +0x38 — each 8×8 bytes)
        reader.position = tc.typeInfo + 0x20;
        int64_t off20[8]; for (auto& o : off20) o = reader.readLong();
        reader.position = tc.typeInfo + 0x28;
        int64_t off28[8]; for (auto& o : off28) o = reader.readLong();
        reader.position = tc.typeInfo + 0x30;
        int64_t off30[8]; for (auto& o : off30) o = reader.readLong();
        reader.position = tc.typeInfo + 0x38;
        int64_t off38[8]; for (auto& o : off38) o = reader.readLong();

        TD_Log("\n=== OFFSET ARRAYS ===");
        TD_Log("\n+0x20 Offsets:");
        for (int i = 0; i < 8; ++i)
            TD_Log("  [%d] = 0x%llX%s", i, (unsigned long long)off20[i], isValid(off20[i]) ? " [VALID]" : "");
        TD_Log("\n+0x28 Offsets:");
        for (int i = 0; i < 8; ++i)
            TD_Log("  [%d] = 0x%llX%s", i, (unsigned long long)off28[i], isValid(off28[i]) ? " [VALID]" : "");
        TD_Log("\n+0x30 Offsets:");
        for (int i = 0; i < 8; ++i)
            TD_Log("  [%d] = 0x%llX%s", i, (unsigned long long)off30[i], isValid(off30[i]) ? " [VALID]" : "");
        TD_Log("\n+0x38 Offsets:");
        for (int i = 0; i < 8; ++i)
            TD_Log("  [%d] = 0x%llX%s", i, (unsigned long long)off38[i], isValid(off38[i]) ? " [VALID]" : "");

        // ----------------------------------------------------------------
        // Flag sampling loop
        // ----------------------------------------------------------------
        TD_Log("\n=== ROBOTO FLAG PROBE: Sampling types to understand type encoding ===");
        {
            int64_t cur = startAddress;
            int     iter = 0, probeCount = 0;
            while (cur != 0 && iter < 300 && probeCount < 30)
            {
                try {
                    reader.position = cur;
                    int64_t tiPtr2 = reader.readLong();
                    int64_t nextPtr2 = reader.readLong();
                    if (tiPtr2 != 0 && isValid(tiPtr2))
                    {
                        reader.position = tiPtr2;
                        int64_t  np2 = reader.readLong();
                        uint16_t fl2 = reader.readUShort();
                        if (np2 != 0 && isValid(np2))
                        {
                            std::string tn = readStr(reader, np2, MAX_STRING_LENGTH);
                            if (!tn.empty() && isValidTypeName(tn))
                            {
                                int normalType = (fl2 >> 4) & 0x1F;
                                int walrusType = ((fl2 >> 1) >> 4) & 0x1F;
                                TD_Log("  Sample: %-50s flags=0x%04X  normal_type=%2d  Walrus_type=%2d",
                                    tn.c_str(), (unsigned)fl2, normalType, walrusType);
                                ++probeCount;
                            }
                        }
                    }
                    cur = nextPtr2;
                }
                catch (...) { break; }
                ++iter;
            }
        }

        // ----------------------------------------------------------------
        // Enum offset probe loop
        // ----------------------------------------------------------------
        TD_Log("\n=== ROBOTO: Testing offset[2]@0x38 as enum field array on samples ===");
        {
            int64_t cur = startAddress;
            int     iter = 0, shown = 0;
            while (cur != 0 && iter < 300 && shown < 20)
            {
                try {
                    reader.position = cur;
                    int64_t tiPtr2 = reader.readLong();
                    int64_t nextPtr2 = reader.readLong();
                    if (tiPtr2 != 0 && isValid(tiPtr2))
                    {
                        reader.position = tiPtr2;
                        int64_t  np2 = reader.readLong();
                        uint16_t fl2 = reader.readUShort();
                        if (np2 != 0 && isValid(np2))
                        {
                            std::string tn = readStr(reader, np2, MAX_STRING_LENGTH);
                            if (!tn.empty() && isValidTypeName(tn))
                            {
                                reader.position = tiPtr2 + 0x38;
                                int64_t sOff[8]; for (auto& o : sOff) o = reader.readLong();
                                for (int idx = 0; idx < 5; ++idx)
                                {
                                    if (sOff[idx] != 0 && isValid(sOff[idx]))
                                    {
                                        bool dummyEnum = false;
                                        int  enumLike = scoreRobotoFieldArray(reader, sOff[idx], 5, dummyEnum);
                                        if (enumLike > 0)
                                        {
                                            TD_Log("  %-50s flags=0x%04X normalType=%d -> offset[%d]@0x38 has %d enum-like entries",
                                                tn.c_str(), (unsigned)fl2, (fl2 >> 4) & 0x1F, idx, enumLike);
                                            break;
                                        }
                                    }
                                }
                                ++shown;
                            }
                        }
                    }
                    cur = nextPtr2;
                }
                catch (...) { break; }
                ++iter;
            }
        }

        // ----------------------------------------------------------------
        // Tests 1-4
        // ----------------------------------------------------------------
        int     bestScore = 0, bestMode = 0, bestIdx = -1;
        int64_t bestPtr = 0;

        // TEST 1: WALRUS
        TD_Log("\n[TEST 1] WALRUS layout:");
        int encFC = (int)((off20[0] >> 16) & 0xFF);
        TD_Log("  Encoded field count from offset[0] byte[2]: %d", encFC);

        if (off20[2] != 0 && isValid(off20[2]))
        {
            int sc = countValidFields(reader, off20[2], std::max(encFC, 20), 8);
            TD_Log("  offset[2]@0x20 = 0x%llX -> %d fields (8-byte stride)",
                (unsigned long long)off20[2], sc);
            if (sc > 0 && (sc == encFC || encFC == 0))
            {
                bestScore = sc; bestMode = 2; bestIdx = 2; bestPtr = off20[2]; TD_Log("   Walrus 8-byte stride!");
            }
        }
        if (bestScore == 0 && off20[2] != 0 && isValid(off20[2]))
        {
            int sc = countValidFields(reader, off20[2], std::max(encFC, 20), 3);
            if (sc > 0)
            {
                bestScore = sc; bestMode = 2; bestIdx = 2; bestPtr = off20[2]; TD_Log("   Walrus 24-byte stride!");
            }
        }

        // TEST 2: JUPITER
        TD_Log("\n[TEST 2] JUPITER layout:");
        TD_Log("  Field count at 0x19: %u", (unsigned)fc19);
        if (fc19 > 0 && fc19 < 100)
        {
            for (int i = 0; i < 8; ++i)
            {
                if (off28[i] != 0 && isValid(off28[i]))
                {
                    int sc = countValidFields(reader, off28[i], std::min((int)fc19, 20), 3);
                    if (sc > 0)
                        TD_Log("  offset[%d]@0x28 = 0x%llX -> %d fields",
                            i, (unsigned long long)off28[i], sc);
                    if (sc > bestScore) { bestScore = sc; bestMode = 0; bestIdx = i; bestPtr = off28[i]; }
                }
            }
        }

        // TEST 3: HAVANA
        TD_Log("\n[TEST 3] HAVANA layout:");
        TD_Log("  Field count at 0x1A: %u", (unsigned)fc1A);
        if (fc1A > 0 && fc1A < 100)
        {
            for (int i = 0; i < 8; ++i)
            {
                if (off20[i] != 0 && isValid(off20[i]))
                {
                    int sc = countValidFields(reader, off20[i], std::min((int)fc1A, 20), 3);
                    if (sc > 0)
                        TD_Log("  offset[%d]@0x20 = 0x%llX -> %d fields",
                            i, (unsigned long long)off20[i], sc);
                    if (sc > bestScore) { bestScore = sc; bestMode = 1; bestIdx = i; bestPtr = off20[i]; }
                }
            }
        }

        // TEST 4: ROBOTO
        TD_Log("\n[TEST 4] ROBOTO layout (encoded field count at 0x30, arrays at 0x38):");

        int     heatBestScore = 0, heatBestIdx = -1;
        int64_t heatBestPtr = 0;
        bool    anyRoboto = false;

        for (int ci = 0; ci < (int)candidates.size() && ci < 5; ++ci)
        {
            auto& cand = candidates[ci];
            reader.position = cand.typeInfo + 0x30;
            int64_t enc30 = reader.readLong();
            bool    enc30Ptr = isValid(enc30);
            int     candFC = enc30Ptr ? 0 : (int)(enc30 & 0xFF);

            TD_Log("  Candidate: %s +0x30=0x%llX isPtr=%s fieldCount(byte0)=%d",
                cand.name.c_str(), (unsigned long long)enc30,
                enc30Ptr ? "true" : "false", candFC);

            if (enc30Ptr || candFC <= 0)
            {
                TD_Log("  -> Skipping (isPtr=%s fieldCount=%d)", enc30Ptr ? "true" : "false", candFC);
                continue;
            }
            anyRoboto = true;

            reader.position = cand.typeInfo + 0x38;
            int64_t hOff[8]; for (auto& o : hOff) o = reader.readLong();

            for (int i = 0; i < 8; ++i)
            {
                // Roboto field arrays live in the heap region, so use isValidR not isValid
                if (hOff[i] != 0 && isValidR(hOff[i]))
                {
                    int sc = countValidFieldsRoboto(reader, hOff[i], std::max(candFC + 10, 20), 3);
                    if (sc > 0)
                        TD_Log("  %s offset[%d]@0x38 = 0x%llX -> %d fields (expected: %d)",
                            cand.name.c_str(), i, (unsigned long long)hOff[i], sc, candFC);
                    if (sc > heatBestScore)
                    {
                        heatBestScore = sc; heatBestIdx = i; heatBestPtr = hOff[i];
                    }
                }
            }
        }

        if (heatBestScore > 0 && heatBestScore >= bestScore && anyRoboto)
        {
            bestScore = heatBestScore; bestMode = 3;
            bestIdx = heatBestIdx;  bestPtr = heatBestPtr;
            TD_Log("  ROBOTO wins: score=%d offsetIndex=%d ptr=0x%llX",
                heatBestScore, heatBestIdx, (unsigned long long)heatBestPtr);
        }

        // ----------------------------------------------------------------
        // Detection result
        // ----------------------------------------------------------------
        TD_Log("\n=== DETECTION RESULT ===");
        TD_Log("Best score: %d, mode: %d, offset index: %d, ptr: 0x%llX",
            bestScore, bestMode, bestIdx, (unsigned long long)bestPtr);

        if (bestScore < 1)
        {
            TD_Log("No valid layout, defaulting to Jupiter");
            _config = { 0x19, 0x28, 2, 1, 0 };
            return 0;
        }

        // ----------------------------------------------------------------
        // Validate Walrus — match C# logic exactly
        // ----------------------------------------------------------------
        if (bestMode == 2)
        {
            int  encodedCount = (int)((off20[0] >> 16) & 0xFF);
            bool encodedCountMatch = (encodedCount == bestScore);
            bool has19 = fc19 > 0 && fc19 < 100 && fc19 <= (uint8_t)bestScore;
            bool has1A = fc1A > 0 && fc1A < 100 && fc1A <= (uint8_t)bestScore;
            TD_Log("\n=== VALIDATING Walrus: encoded=%d, matches=%s, has19=%s, has1A=%s ===",
                encodedCount,
                encodedCountMatch ? "true" : "false",
                has19 ? "true" : "false",
                has1A ? "true" : "false");

            bool isTrueWalrus = false;
            if (encodedCountMatch || encodedCount == 0)
            {
                isTrueWalrus = !(has19 || has1A);
                TD_Log(isTrueWalrus ? "TRUE Walrus" : "FALSE POSITIVE - NOT Walrus");
            }
            else
            {
                TD_Log("FALSE POSITIVE - encoded mismatch");
            }

            if (!isTrueWalrus)
            {
                TD_Log("\n=== RE-EVALUATING FOR JUPITER/HAVANA/ROBOTO ===");
                bestScore = 0; bestMode = 0; bestIdx = -1; bestPtr = 0;

                if (fc19 > 0 && fc19 < 100)
                {
                    for (int i = 0; i < 8; ++i)
                    {
                        if (off28[i] != 0 && isValid(off28[i]))
                        {
                            int sc = countValidFields(reader, off28[i], std::min((int)fc19, 20), 3);
                            if (sc > bestScore)
                            {
                                bestScore = sc; bestMode = 0; bestIdx = i; bestPtr = off28[i];
                                TD_Log("  RE-JUPITER: offset[%d]@0x28 -> %d", i, sc);
                            }
                        }
                    }
                }
                if (fc1A > 0 && fc1A < 100)
                {
                    for (int i = 0; i < 8; ++i)
                    {
                        if (off20[i] != 0 && isValid(off20[i]))
                        {
                            int sc = countValidFields(reader, off20[i], std::min((int)fc1A, 20), 3);
                            if (sc > bestScore)
                            {
                                bestScore = sc; bestMode = 1; bestIdx = i; bestPtr = off20[i];
                                TD_Log("  RE-HAVANA: offset[%d]@0x20 -> %d", i, sc);
                            }
                        }
                    }
                }
                if (anyRoboto && heatBestScore > 0 && heatBestScore >= bestScore)
                {
                    bestScore = heatBestScore; bestMode = 3;
                    bestIdx = heatBestIdx;   bestPtr = heatBestPtr;
                    TD_Log("  RE-ROBOTO wins: score=%d offsetIndex=%d",
                        heatBestScore, heatBestIdx);
                }
            }
        }

        // ----------------------------------------------------------------
        // Commit config
        // ----------------------------------------------------------------
        int structIdx, enumIdx, fcOff, offStart;
        if (bestMode == 0) { fcOff = 0x19; offStart = 0x28; structIdx = std::max(0, bestIdx - 1); enumIdx = 0; }
        else if (bestMode == 2) { fcOff = 0x19; offStart = 0x20; structIdx = 6;                        enumIdx = 1; }
        else if (bestMode == 3) { fcOff = 0x30; offStart = 0x38; structIdx = std::max(0, bestIdx - 1); enumIdx = std::max(0, bestIdx - 1); }
        else { fcOff = 0x1A; offStart = 0x20; structIdx = std::max(0, bestIdx - 1); enumIdx = std::max(0, bestIdx - 2); }

        _config = { fcOff, offStart, bestIdx, structIdx, enumIdx };
        _walrusMode = (bestMode == 2);
        _robotoMode = (bestMode == 3);
        _dingoMode = false;

        static const char* modeNames[] = { "JUPITER", "HAVANA", "WALRUS", "ROBOTO", "DINGO" };
        TD_Log("\n%s", std::string(80, '=').c_str());
        TD_Log("DETECTED: %s", modeNames[bestMode]);
        TD_Log("  Field count: +0x%02X", fcOff);
        TD_Log("  Offsets at:  +0x%02X", offStart);
        TD_Log("  Class offset index:  [%d]", bestIdx);
        TD_Log("  Struct offset index: [%d]", structIdx);
        TD_Log("  Enum offset index:   [%d]", enumIdx);
        TD_Log("%s\n", std::string(80, '=').c_str());

        return bestMode;
    }

    // -----------------------------------------------------------------------
    // countValidFields — supports 24-byte and 8-byte strides
    // -----------------------------------------------------------------------
    int countValidFields(MemoryReader& reader, int64_t ptr, int maxCheck, int typeValue)
    {
        if (ptr == 0) return 0;
        int     count24 = 0, count8 = 0;
        int64_t saved = reader.position;

        // 24-byte stride probe
        reader.position = ptr;
        for (int i = 0; i < std::min(maxCheck, 3); ++i) {
            int64_t  np = reader.readLong();
            uint16_t fl = reader.readUShort();
            uint16_t off = reader.readUShort();
            uint32_t pad = reader.readUInt();
            int64_t  tp = reader.readLong();
            if (np == 0 || !isValid(np)) break;
            std::string nm = readStr(reader, np, 256);
            if (nm.empty() || !isValidTypeName(nm)) break;
            if (typeValue != 8 && (tp == 0 || !isValid(tp))) break;
            ++count24;
        }

        // 8-byte stride probe
        reader.position = ptr;
        for (int i = 0; i < std::min(maxCheck, 3); ++i) {
            int64_t np = reader.readLong();
            if (np == 0 || !isValid(np)) break;
            std::string nm = readStr(reader, np, 256);
            if (nm.empty() || !isValidTypeName(nm)) break;
            ++count8;
        }

        bool use8 = count8 > count24;
        int  validCount = 0;
        reader.position = ptr;
        for (int i = 0; i < maxCheck; ++i) {
            if (use8) {
                int64_t np = reader.readLong();
                if (np == 0 || !isValid(np)) break;
                std::string nm = readStr(reader, np, 256);
                if (nm.empty()) break;
                ++validCount;
            }
            else {
                int64_t  np = reader.readLong();
                reader.readUShort(); reader.readUShort(); reader.readUInt();
                int64_t  tp = reader.readLong();
                if (np == 0 || !isValid(np)) break;
                std::string nm = readStr(reader, np, 256);
                if (nm.empty() || !isValidTypeName(nm)) break;
                if (typeValue != 8 && (tp == 0 || !isValid(tp))) break;
                ++validCount;
            }
        }

        reader.position = saved;
        return validCount;
    }

    // -----------------------------------------------------------------------
    // countValidFieldsRoboto — like countValidFields but uses isValidR for
    // name/type pointers since Roboto field arrays live in the heap region
    // -----------------------------------------------------------------------
    int countValidFieldsRoboto(MemoryReader& reader, int64_t ptr, int maxCheck, int typeValue)
    {
        if (ptr == 0) return 0;
        int     count24 = 0, count8 = 0;
        int64_t saved = reader.position;

        // 24-byte stride probe
        reader.position = ptr;
        for (int i = 0; i < std::min(maxCheck, 3); ++i) {
            int64_t  np = reader.readLong();
            uint16_t fl = reader.readUShort();
            uint16_t off = reader.readUShort();
            uint32_t pad = reader.readUInt();
            int64_t  tp = reader.readLong();
            if (np == 0 || (!isValidR(np) && !isValid(np))) break;
            std::string nm = readStr(reader, np, 256);
            if (nm.empty() || !isValidTypeName(nm)) break;
            if (typeValue != 8 && tp != 0 && (!isValidR(tp) && !isValid(tp))) break;
            ++count24;
        }

        // 8-byte stride probe
        reader.position = ptr;
        for (int i = 0; i < std::min(maxCheck, 3); ++i) {
            int64_t np = reader.readLong();
            if (np == 0 || (!isValidR(np) && !isValid(np))) break;
            std::string nm = readStr(reader, np, 256);
            if (nm.empty() || !isValidTypeName(nm)) break;
            ++count8;
        }

        bool use8 = count8 > count24;
        int  validCount = 0;
        reader.position = ptr;
        for (int i = 0; i < maxCheck; ++i) {
            if (use8) {
                int64_t np = reader.readLong();
                if (np == 0 || (!isValidR(np) && !isValid(np))) break;
                std::string nm = readStr(reader, np, 256);
                if (nm.empty()) break;
                ++validCount;
            }
            else {
                int64_t  np = reader.readLong();
                reader.readUShort(); reader.readUShort(); reader.readUInt();
                int64_t  tp = reader.readLong();
                if (np == 0 || (!isValidR(np) && !isValid(np))) break;
                std::string nm = readStr(reader, np, 256);
                if (nm.empty() || !isValidTypeName(nm)) break;
                if (typeValue != 8 && tp != 0 && (!isValidR(tp) && !isValid(tp))) break;
                ++validCount;
            }
        }

        reader.position = saved;
        return validCount;
    }

    // -----------------------------------------------------------------------
    // 64-BIT: Main extraction
    // -----------------------------------------------------------------------
    int extractTypes64(MemoryReader& reader, int64_t startAddress)
    {
        allTypes.clear();
        _fieldMode = detectFieldOffsetPattern(reader, startAddress);

        // Set status immediately after detection so the UI timer shows the
        // correct mode name as soon as Pass 1 begins
        static const char* modeNames[] = { "JUPITER", "HAVANA", "WALRUS", "ROBOTO", "DINGO" };
        const char* detectedMode = (_fieldMode >= 0 && _fieldMode <= 4) ? modeNames[_fieldMode] : "UNKNOWN";
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "[%s MODE]: Extracting types", detectedMode);
            statusMessage = buf;
        }

        if (_dingoMode) return extractDingoTypes(reader);

        int typeCount = 0;
        int consecutiveFail = 0;
        const int MAX_TYPES = 50000;
        const int MAX_FAIL = 10;

        std::unordered_set<int64_t> visitedAddresses;
        std::unordered_set<int64_t> visitedTIPtrs;

        int64_t cur = startAddress;

        while (cur != 0 && typeCount < MAX_TYPES && consecutiveFail < MAX_FAIL) {
            if (isCancelled()) break;
            if (visitedAddresses.count(cur)) break;
            visitedAddresses.insert(cur);

            reader.position = cur;
            int64_t tiPtr = reader.readLong();
            int64_t nextPtr = reader.readLong();

            if (tiPtr == 0 || !isValidForMode(tiPtr)) { ++consecutiveFail; cur = nextPtr; continue; }
            if (visitedTIPtrs.count(tiPtr)) { cur = nextPtr; continue; }
            visitedTIPtrs.insert(tiPtr);

            reader.position = tiPtr;
            int64_t namePtr = reader.readLong();
            if (namePtr == 0 || !isValidForMode(namePtr)) { ++consecutiveFail; cur = nextPtr; continue; }

            std::string typeName = readStr(reader, namePtr, 256);
            if (typeName.empty() || !isValidTypeName(typeName)) { ++consecutiveFail; cur = nextPtr; continue; }

            _typeInfoToName[tiPtr] = typeName;

            reader.position = tiPtr + 0x08;
            uint16_t flags = reader.readUShort();

            uint32_t sz;
            if (_robotoMode) { reader.position = tiPtr + 0x0E; sz = reader.readUShort(); }
            else { reader.position = tiPtr + 0x0A; sz = reader.readUInt(); }

            reader.position = tiPtr + 0x10;
            int64_t nsPtr = reader.readLong();
            std::string typeNs;
            if (nsPtr != 0 && isValidForMode(nsPtr)) {
                std::string nsTmp = readStr(reader, nsPtr, 256);
                if (!nsTmp.empty() && isValidTypeName(nsTmp)) typeNs = nsTmp;
            }

            int tv;
            if (_walrusMode) tv = ((flags >> 1) >> 4) & 0x1F;
            else             tv = (flags >> 4) & 0x1F;

            // Field count
            int fieldCount = 0;
            if (_robotoMode) {
                reader.position = tiPtr + 0x30;
                int64_t enc30 = reader.readLong();
                if (!isValidR(enc30)) {
                    int total = (int)(enc30 & 0xFFFF);
                    int own = (int)((enc30 >> 16) & 0xFFFF);
                    fieldCount = (own > 0 && own < 2000) ? own : (total > 0 && total < 2000) ? total : 0;
                }
            }
            else if (_walrusMode) {
                reader.position = tiPtr + (int64_t)_config.offsetsStartAt;
                int64_t firstOff = reader.readLong();
                fieldCount = (int)((firstOff >> 16) & 0xFFFF);
            }
            else {
                reader.position = tiPtr + (int64_t)_config.fieldCountOffset;
                fieldCount = (int)reader.readByte();
            }

            // Offsets array (non-Roboto only)
            int64_t offsets[8] = {};
            if (!_robotoMode) {
                reader.position = tiPtr + (int64_t)_config.offsetsStartAt;
                for (auto& o : offsets) o = reader.readLong();
            }

            // Base type
            std::string baseType;
            if (_robotoMode) {
                for (int offTry : {0x28, 0x38}) {
                    reader.position = tiPtr + offTry;
                    int64_t p = reader.readLong();
                    if (p == 0 || !isValidR(p)) continue;
                    reader.position = p;
                    int64_t pTI = reader.readLong();
                    if (pTI == 0 || !isValidR(pTI)) continue;
                    reader.position = pTI;
                    int64_t pNP = reader.readLong();
                    if (pNP == 0 || !isValidR(pNP)) continue;
                    std::string pn = readStr(reader, pNP, 256);
                    if (!pn.empty() && isValidTypeName(pn) && pn != typeName &&
                        pn.find("[]") == std::string::npos) {
                        baseType = pn; break;
                    }
                }
            }
            else {
                int64_t parentPtr = 0;
                if (_config.offsetsStartAt == 0x28) { reader.position = tiPtr + 0x20; parentPtr = reader.readLong(); }
                else if (_walrusMode)                  parentPtr = offsets[1];
                else                                   parentPtr = offsets[0];

                if (parentPtr != 0 && isValid(parentPtr)) {
                    reader.position = parentPtr;
                    int64_t pTI = reader.readLong();
                    if (pTI != 0 && isValid(pTI)) {
                        reader.position = pTI;
                        int64_t pNP = reader.readLong();
                        if (pNP != 0 && isValid(pNP)) {
                            std::string pn = readStr(reader, pNP, 256);
                            if (!pn.empty() && isValidTypeName(pn)) baseType = pn;
                        }
                    }
                }
            }

            // Category
            std::string category;
            if (_robotoMode) {
                category = (tv == 2) ? "Structs" : "Classes";
            }
            else {
                switch (tv) {
                case 2:  category = "Structs"; break;
                case 8:  category = "Enums";   break;
                default: category = "Classes"; break;
                }
            }

            // Fields
            std::vector<FieldItem> fields;
            if (_robotoMode) {
                fields = extractRobotoFields(reader, tiPtr, fieldCount, typeName,
                    typeCount < 20, category);
            }
            else if (fieldCount > 0 || tv == 2 || tv == 3 || tv == 8) {
                int64_t fap = 0;
                if (tv == 2) fap = offsets[_config.structFieldOffsetIdx];
                else if (tv == 3) fap = offsets[_config.classFieldOffsetIdx];
                else if (tv == 8) {
                    if (_walrusMode) {
                        fap = offsets[1];
                    }
                    else {
                        reader.position = tiPtr + 0x20;
                        int64_t eOff[7]; for (auto& o : eOff) o = reader.readLong();
                        fap = eOff[_config.enumFieldOffsetIdx];
                    }
                }
                if (fap != 0 && isValid(fap)) {
                    int maxF = fieldCount > 0 ? fieldCount : 20;
                    fields = extractFields64(reader, fap, maxF, tv, typeName, typeCount < 10);
                }
            }

            consecutiveFail = 0;
            TypeItem ti;
            ti.name = typeName;
            ti.ns = typeNs;
            ti.fullName = typeNs.empty() ? typeName : typeNs + "." + typeName;
            ti.category = category;
            ti.baseType = baseType;
            ti.fields = std::move(fields);
            allTypes.push_back(std::move(ti));
            ++typeCount;
            cur = nextPtr;
        }

        // ----------------------------------------------------------------
        // Roboto Pass 2: linear module scan for types not in the chain
        // ----------------------------------------------------------------
        if (_robotoMode && typeCount > 0) {
            TD_Log("\n[Roboto] Pass 2: linear module scan for missing types...");

            int64_t moduleMax = _baseAddr + (1024LL * 1024 * 1024);
            if (!_exePath.empty()) {
                auto sections = readPeSections(_exePath);
                uint32_t highestVA = 0, highestSize = 0;
                for (auto& s : sections) {
                    if (s.virtualAddress + s.virtualSize > highestVA + highestSize) {
                        highestVA = s.virtualAddress;
                        highestSize = s.virtualSize;
                    }
                }
                if (highestVA > 0)
                    moduleMax = _baseAddr + (int64_t)highestVA + (int64_t)highestSize;
            }

            std::unordered_set<std::string> existingNames;
            existingNames.reserve(allTypes.size() * 2);
            for (auto& t : allTypes) existingNames.insert(t.name);

            int pass2Count = 0;
            int missStreak = 0;

            // Read the module in 8 MB chunks
            const size_t  CHUNK = 0x800000; // 8 MB
            const int64_t SCAN_START = _baseAddr + 0x1000;
            std::vector<uint8_t> chunkBuf;
            chunkBuf.reserve(CHUNK);

            for (int64_t chunkBase = SCAN_START; chunkBase < moduleMax - 8; chunkBase += (int64_t)CHUNK) {
                if (isCancelled()) break;

                size_t toRead = (size_t)std::min((int64_t)CHUNK, moduleMax - chunkBase);
                chunkBuf.resize(toRead);
                SIZE_T got = 0;
                if (!ReadProcessMemory(_hProcess, (LPCVOID)(uintptr_t)chunkBase,
                    chunkBuf.data(), toRead, &got) || got < 8)
                    continue;

                // Update status every chunk (~8 MB) so the UI shows progress
                int64_t scanSize = moduleMax - SCAN_START;
                int pct = (int)(((chunkBase - SCAN_START) * 100) / (scanSize > 0 ? scanSize : 1));
                char statusBuf[128];
                snprintf(statusBuf, sizeof(statusBuf),
                    "[ROBOTO MODE]: Pass 2 - %d%% (%d new types found...)", pct, pass2Count);
                statusMessage = statusBuf;

                for (size_t off = 0; off + 8 <= got; off += 8) {
                    int64_t pos = chunkBase + (int64_t)off;

                    int64_t tiPtr2 = 0;
                    memcpy(&tiPtr2, chunkBuf.data() + off, 8);

                    if (tiPtr2 == 0 || tiPtr2 < _baseAddr || tiPtr2 >= moduleMax) {
                        ++missStreak;
                        if (missStreak > 4096) { off += 0x1000 - 8; missStreak = 0; }
                        continue;
                    }
                    if ((tiPtr2 & 0x7) != 0) {
                        ++missStreak;
                        if (missStreak > 4096) { off += 0x1000 - 8; missStreak = 0; }
                        continue;
                    }
                    missStreak = 0;

                    if (visitedTIPtrs.count(tiPtr2)) continue;

                    reader.position = tiPtr2;
                    int64_t np2 = reader.readLong();
                    if (np2 == 0 || np2 < _baseAddr || np2 >= moduleMax) continue;
                    reader.position = tiPtr2 + 0x0E;
                    uint16_t sz2 = reader.readUShort();
                    if (sz2 == 0 || sz2 > 60000) continue;

                    std::string typeName2 = readStr(reader, np2, 256);
                    if (typeName2.empty() || !isValidTypeName(typeName2) || typeName2.size() < 2) continue;
                    if (existingNames.count(typeName2)) continue;

                    visitedTIPtrs.insert(tiPtr2);
                    existingNames.insert(typeName2);

                    reader.position = tiPtr2 + 0x08;
                    uint16_t flags2 = reader.readUShort();
                    int tv2 = (flags2 >> 4) & 0x1F;

                    reader.position = tiPtr2 + 0x30;
                    int64_t enc2 = reader.readLong();
                    bool enc2IsPtr = isValidR(enc2);
                    int ownCount2 = 0;
                    if (!enc2IsPtr) {
                        int tot = (int)(enc2 & 0xFFFF);
                        int own = (int)((enc2 >> 16) & 0xFFFF);
                        ownCount2 = (own > 0 && own < 2000) ? own : (tot > 0 && tot < 2000) ? tot : 0;
                    }
                    // Fallback: try +0x19/+0x1A if encoded count is zero
                    if (ownCount2 == 0) {
                        reader.position = tiPtr2 + 0x19;
                        uint8_t alt19 = reader.readByte();
                        uint8_t alt1A = reader.readByte();
                        if (alt19 > 0 && alt19 < 200)      ownCount2 = alt19;
                        else if (alt1A > 0 && alt1A < 200) ownCount2 = alt1A;
                    }

                    reader.position = tiPtr2 + 0x10;
                    int64_t nsPtr2 = reader.readLong();
                    std::string ns2;
                    if (nsPtr2 != 0 && nsPtr2 >= _baseAddr && nsPtr2 < moduleMax) {
                        std::string nsTmp = readStr(reader, nsPtr2, 256);
                        if (!nsTmp.empty() && isValidTypeName(nsTmp)) ns2 = nsTmp;
                    }

                    std::string baseType2;
                    for (int offTry : {0x28, 0x38}) {
                        reader.position = tiPtr2 + offTry;
                        int64_t p = reader.readLong();
                        if (p == 0 || !isValidR(p)) continue;
                        reader.position = p;
                        int64_t pTI = reader.readLong();
                        if (pTI == 0 || !isValidR(pTI)) continue;
                        reader.position = pTI;
                        int64_t pNP = reader.readLong();
                        if (pNP == 0 || !isValidR(pNP)) continue;
                        std::string pn = readStr(reader, pNP, 256);
                        if (!pn.empty() && isValidTypeName(pn) && pn != typeName2 &&
                            pn.find("[]") == std::string::npos) {
                            baseType2 = pn; break;
                        }
                    }

                    std::string cat2 = (tv2 == 2) ? "Structs" : "Classes";
                    std::vector<FieldItem> fields2;
                    if (ownCount2 > 0)
                        fields2 = extractRobotoFields(reader, tiPtr2, ownCount2, typeName2, false, cat2);

                    if (cat2 == "Enums" && !baseType2.empty())
                        cat2 = "Classes";

                    TypeItem ti2;
                    ti2.name = typeName2;
                    ti2.ns = ns2;
                    ti2.fullName = ns2.empty() ? typeName2 : ns2 + "." + typeName2;
                    ti2.category = cat2;
                    ti2.baseType = baseType2;
                    ti2.fields = std::move(fields2);
                    _typeInfoToName[tiPtr2] = typeName2;
                    allTypes.push_back(std::move(ti2));
                    ++typeCount;
                    ++pass2Count;
                }
            }

            int reclassified = 0;
            for (auto& t : allTypes) {
                if (t.category == "Enums" && !t.baseType.empty()) {
                    t.category = "Classes";
                    ++reclassified;
                }
            }
            TD_Log("[Roboto] Pass 2 complete: %d new types. Reclassified %d Enum->Class. Total: %d",
                pass2Count, reclassified, (int)allTypes.size());
            typeCount = (int)allTypes.size();
        }

        return (int)allTypes.size();
    }

    // -----------------------------------------------------------------------
    // 64-BIT: Field extraction (Jupiter / Havana / Walrus)
    // -----------------------------------------------------------------------
    std::vector<FieldItem> extractFields64(MemoryReader& reader, int64_t fieldArrayPtr,
        int fieldCount, int typeValue, const std::string& typeName, bool dbg)
    {
        std::vector<FieldItem> fields;
        reader.position = fieldArrayPtr;

        for (int i = 0; i < fieldCount; ++i) {
            int64_t  np = reader.readLong();
            uint16_t fFlags = reader.readUShort();
            uint16_t fOffset = reader.readUShort();
            uint32_t fPad = reader.readUInt();
            int64_t  tp = reader.readLong();
            int64_t  nextStart = reader.position;

            if (np == 0 || !isValid(np)) break;

            std::string fieldName = readStr(reader, np, 256);

            if (typeValue == 8) {
                if (!fieldName.empty()) {
                    FieldItem fi;
                    fi.name = fieldName;
                    fi.type = std::to_string((int32_t)tp);
                    fi.offset = (int32_t)tp;
                    fields.push_back(std::move(fi));
                }
            }
            else {
                if (fieldName.empty() || !isValidTypeName(fieldName)) break;

                std::string ftName = "Unknown";
                bool        isArr = false;
                std::string arrElem;

                if (tp != 0 && isValid(tp)) {
                    reader.position = tp;
                    int64_t actualTI = reader.readLong();
                    if (actualTI != 0 && isValid(actualTI)) {
                        reader.position = actualTI;
                        int64_t  tnPtr = reader.readLong();
                        uint16_t tFlags = reader.readUShort();
                        uint32_t tSz = reader.readUInt();
                        if (_walrusMode) tFlags >>= 1;

                        std::string memTN;
                        if (tnPtr != 0 && isValid(tnPtr)) memTN = readStr(reader, tnPtr, 256);

                        int sType = (tFlags >> 4) & 0x1F;
                        int resolvedType = resolveNonRobotoFieldType(memTN, sType, tSz);
                        ftName = buildFieldTypeName(resolvedType, memTN, isArr, arrElem,
                            reader, actualTI);
                    }
                }

                FieldItem fi;
                fi.name = fieldName;
                fi.type = ftName;
                fi.offset = (int)fOffset;
                fi.isArray = isArr;
                fi.arrayElemType = arrElem;
                fields.push_back(std::move(fi));
            }
            reader.position = nextStart;
        }

        std::sort(fields.begin(), fields.end(),
            [](const FieldItem& a, const FieldItem& b) { return a.offset < b.offset; });
        return fields;
    }

    int resolveNonRobotoFieldType(const std::string& memTN, int structuralType, uint32_t typeSize)
    {
        if (memTN == "Float32")  return 0x12;
        if (memTN == "Float64")  return 0x13;
        if (memTN == "Int32")    return 0x0E;
        if (memTN == "Uint32" || memTN == "UInt32") return 0x0F;
        if (memTN == "Int64")    return _walrusMode ? 0x11 : 0x10;
        if (memTN == "Uint64" || memTN == "UInt64") return _walrusMode ? 0x10 : 0x11;
        if (memTN == "Int16")    return 0x0C;
        if (memTN == "Uint16" || memTN == "UInt16") return 0x0D;
        if (memTN == "Int8")     return 0x0A;
        if (memTN == "Uint8" || memTN == "UInt8")   return 0x0B;
        if (memTN == "Boolean")  return 0x09;
        if (memTN == "CString")  return 0x06;
        if (structuralType >= 0x02 && structuralType <= 0x08) return structuralType;
        if (!memTN.empty()) {
            std::string ln = memTN;
            for (char& c : ln) c = (char)tolower((unsigned char)c);
            if (ln.find("guid") != std::string::npos) return 0x14;
            if (ln.find("sha1") != std::string::npos) return 0x15;
            if (ln.find("resourceref") != std::string::npos) return 0x16;
            if (ln.find("typeref") != std::string::npos) return 0x17;
            if (ln.find("boxedvalueref") != std::string::npos) return 0x18;
            if (_walrusMode && structuralType >= 0x09 && structuralType <= 0x13)
                return structuralType;
        }
    switch (typeSize) { case 1: return 0x09; case 2: return 0x0D; case 4: return 0x0F; case 8: return 0x10; }
                              return structuralType;
    }

    std::string buildFieldTypeName(int resolvedType, const std::string& memTN,
        bool& isArray, std::string& arrayElem,
        MemoryReader& reader, int64_t actualTI)
    {
        switch (resolvedType) {
        case 0x02: return !memTN.empty() ? memTN : "Struct";
        case 0x03: return !memTN.empty() ? memTN : "Pointer";
        case 0x04: {
            isArray = true;
            if (_walrusMode && !memTN.empty() && memTN.size() > 6 &&
                memTN.substr(memTN.size() - 6) == "-Array") {
                std::string elemName = memTN.substr(0, memTN.size() - 6);
                arrayElem = mapPrimitiveTypeName(elemName);
                if (arrayElem.empty()) arrayElem = elemName;
            }
            else {
                reader.position = actualTI + 0x20;
                int64_t intPtr = reader.readLong();
                if (intPtr != 0 && isValid(intPtr)) {
                    reader.position = intPtr;
                    int64_t elemTI = reader.readLong();
                    if (elemTI != 0 && isValid(elemTI)) {
                        reader.position = elemTI;
                        int64_t eNP = reader.readLong();
                        if (eNP != 0 && isValid(eNP)) {
                            std::string en = readStr(reader, eNP, 256);
                            arrayElem = en.empty() ? "Unknown" : en;
                        }
                        else arrayElem = "Unknown";
                    }
                    else arrayElem = "Unknown";
                }
                else arrayElem = "Unknown";
            }
            return "List";
        }
        case 0x05: return "String";
        case 0x06: return "CString";
        case 0x07: return !memTN.empty() ? memTN : "Enum";
        case 0x08: return !memTN.empty() ? memTN : "FileRef";
        default:   return getFieldTypeName(resolvedType);
        }
    }

    // Maps primitive type names to canonical form (used for Walrus -Array types)
    std::string mapPrimitiveTypeName(const std::string& name)
    {
        if (name == "CString" || name == "String")                    return "CString";
        if (name == "Int8")                                            return "Int8";
        if (name == "UInt8" || name == "Uint8")                      return "UInt8";
        if (name == "Int16")                                           return "Int16";
        if (name == "UInt16" || name == "Uint16")                     return "UInt16";
        if (name == "Int32")                                           return "Int32";
        if (name == "UInt32" || name == "Uint32")                     return "UInt32";
        if (name == "Int64")                                           return "Int64";
        if (name == "UInt64" || name == "Uint64")                     return "UInt64";
        if (name == "Float32")                                         return "Float32";
        if (name == "Float64")                                         return "Float64";
        if (name == "Boolean")                                         return "Boolean";
        return {}; // empty = not a primitive, return as-is
    }

    // -----------------------------------------------------------------------
    // 64-BIT: Roboto field extraction
    // -----------------------------------------------------------------------
    std::vector<FieldItem> extractRobotoFields(MemoryReader& reader, int64_t tiPtr,
        int fieldCount, const std::string& typeName,
        bool dbg, std::string& category)
    {
        std::vector<FieldItem> fields;
        if (fieldCount <= 0) return fields;

        int ownCount = fieldCount;
        {
            reader.position = tiPtr + 0x30;
            int64_t enc30 = reader.readLong();
            if (!isValidR(enc30)) {
                int total = (int)(enc30 & 0xFFFF);
                int own = (int)((enc30 >> 16) & 0xFFFF);
                if (own > 0 && own < 2000) ownCount = own;
                else if (total > 0 && total < 2000) ownCount = total;
            }
        }

        // Read offset array at +0x38
        int64_t offsets38[8] = {};
        reader.position = tiPtr + 0x38;
        for (auto& o : offsets38) o = reader.readLong();

        // Score all candidates, pick best
        int64_t bestPtr = 0;
        int     bestScore = 0;
        bool    bestIsEnum = false;

        for (int i = 0; i < 8; ++i) {
            int64_t cPtr = offsets38[i];
            if (cPtr == 0) continue;
            // Roboto field arrays can be in heap region — use isValidR
            bool isEnum = false;
            int  sc = scoreRobotoFieldArray(reader, cPtr, ownCount + 20, isEnum);
            if (sc > bestScore) { bestScore = sc; bestPtr = cPtr; bestIsEnum = isEnum; }
        }

        if (bestPtr == 0 || bestScore == 0) return fields;
        if (bestIsEnum) category = "Enums";

        std::vector<uint32_t> paddingValues;

        for (int i = 0; i < ownCount + 20 && (int)fields.size() < ownCount; ++i) {
            int64_t  ep = bestPtr + i * 24LL;
            reader.position = ep;
            int64_t  np = reader.readLong();
            uint16_t efl = reader.readUShort();
            uint16_t eOff = reader.readUShort();
            uint32_t pad = reader.readUInt();
            int64_t  tp = reader.readLong();

            if (pad == 0x00000001) break;
            if (np == 0)           break;
            // Name ptrs can be in module (.rdata) OR heap range
            if (!isValidR(np) && !isValid(np)) break;

            std::string nm = readStr(reader, np, 256);
            if (nm.empty() || !isValidTypeName(nm)) break;
            if (nm == "Inherited") continue;

            if (bestIsEnum) {
                FieldItem fi;
                fi.name = nm;
                fi.type = std::to_string((int32_t)tp);
                fi.offset = (int32_t)tp;
                fields.push_back(std::move(fi));
                paddingValues.push_back(pad);
            }
            else {
                std::string ftName = "Unknown";
                bool        isArr = false;
                std::string arrElem;

                if (tp != 0 && (isValidR(tp) || isValid(tp)))
                    ftName = resolveRobotoFieldType(reader, tp, isArr, arrElem);

                FieldItem fi;
                fi.name = nm;
                fi.type = ftName;
                fi.offset = (int)eOff;
                fi.isArray = isArr;
                fi.arrayElemType = arrElem;
                fields.push_back(std::move(fi));
                paddingValues.push_back(pad);
            }
        }

        // Sort: enum by offset, non-enum by declaration index from padding high-word
        if (bestIsEnum) {
            std::sort(fields.begin(), fields.end(),
                [](const FieldItem& a, const FieldItem& b) { return a.offset < b.offset; });
        }
        else if (paddingValues.size() == fields.size() && !fields.empty()) {
            std::vector<std::pair<uint32_t, FieldItem>> combined;
            combined.reserve(fields.size());
            for (size_t i = 0; i < fields.size(); ++i)
                combined.push_back({ (paddingValues[i] >> 16) & 0xFFFF, std::move(fields[i]) });
            std::sort(combined.begin(), combined.end(),
                [](auto& a, auto& b) { return a.first < b.first; });
            fields.clear();
            for (auto& [idx, fi] : combined) fields.push_back(std::move(fi));
        }
        else {
            std::sort(fields.begin(), fields.end(),
                [](const FieldItem& a, const FieldItem& b) { return a.offset < b.offset; });
        }

        return fields;
    }

    int scoreRobotoFieldArray(MemoryReader& reader, int64_t ptr, int maxCheck, bool& isEnum)
    {
        isEnum = false;
        if (ptr == 0) return 0;
        int     count = 0;
        bool    checkedEnum = false;
        int64_t saved = reader.position;

        for (int i = 0; i < maxCheck; ++i) {
            int64_t ep = ptr + i * 24LL;
            reader.position = ep;
            int64_t np = reader.readLong();
            if (np == 0) break;
            // Name ptrs can be in module OR heap
            if (!isValidR(np) && !isValid(np)) break;

            reader.position = ep + 0x0C;
            uint32_t pad = reader.readUInt();
            if (pad == 0x00000001) break;

            std::string nm = readStr(reader, np, 256);
            if (nm.empty() || !isValidTypeName(nm) || nm.size() < 1) break;
            if (nm == "Inherited") { ++count; continue; }

            if (!checkedEnum) {
                reader.position = ep + 0x10;
                int64_t tp0 = reader.readLong();
                // Enum: typePtr is a small integer, not a valid heap pointer
                bool typePtrIsHeap = isValidR(tp0);
                if (!typePtrIsHeap && std::abs(tp0) < 0x80000000LL) isEnum = true;
                checkedEnum = true;
            }
            ++count;
        }

        reader.position = saved;
        return count;
    }

    std::string resolveRobotoFieldType(MemoryReader& reader, int64_t typeNodePtr,
        bool& isArray, std::string& arrayElem)
    {
        auto isTypeInfo = [&](int64_t a) -> bool {
            if (a == 0 || (!isValidR(a) && !isValid(a))) return false;
            try {
                reader.position = a;
                int64_t np = reader.readLong();
                if (np == 0 || (!isValidR(np) && !isValid(np))) return false;
                std::string nm = readStr(reader, np, 256);
                if (nm.empty() || nm.size() < 2) return false;
                // Strip "[]" suffix for array TypeInfos
                std::string base = nm;
                if (base.size() > 2 && base.back() == ']' && base[base.size() - 2] == '[')
                    base = base.substr(0, base.size() - 2);
                return isValidTypeName(base) && base.size() >= 2;
            }
            catch (...) { return false; }
            };

        int64_t cur = typeNodePtr;
        int64_t resolvedTI = 0;
        for (int hop = 0; hop < 4; ++hop) {
            if (isTypeInfo(cur)) { resolvedTI = cur; break; }
            reader.position = cur;
            int64_t next = reader.readLong();
            if (next == 0 || next == cur) break;
            if (!isValidR(next) && !isValid(next)) break;
            cur = next;
        }
        if (resolvedTI == 0 && isTypeInfo(cur)) resolvedTI = cur;
        if (resolvedTI == 0) return "Unknown";

        reader.position = resolvedTI;
        int64_t  tnPtr = reader.readLong();
        uint16_t tFlags = reader.readUShort();
        uint32_t tSz = reader.readUInt();

        std::string memTN;
        if (tnPtr != 0 && (isValidR(tnPtr) || isValid(tnPtr)))
            memTN = readStr(reader, tnPtr, 256);

        if (!memTN.empty()) {
            // Array: "Foo[]" suffix
            if (memTN.size() > 2 && memTN.back() == ']' && memTN[memTN.size() - 2] == '[') {
                isArray = true;
                arrayElem = resolveRobotoArrayElemName(memTN.substr(0, memTN.size() - 2));
                return "List";
            }
            if (memTN == "Boolean")                               return "Boolean";
            if (memTN == "Int8")                                  return "Int8";
            if (memTN == "UInt8" || memTN == "Uint8")            return "UInt8";
            if (memTN == "Int16")                                 return "Int16";
            if (memTN == "UInt16" || memTN == "Uint16")          return "UInt16";
            if (memTN == "Int32")                                 return "Int32";
            if (memTN == "UInt32" || memTN == "Uint32")          return "UInt32";
            if (memTN == "Int64")                                 return "Int64";
            if (memTN == "UInt64" || memTN == "Uint64")          return "UInt64";
            if (memTN == "Float32")                               return "Float32";
            if (memTN == "Float64")                               return "Float64";
            if (memTN == "CString")                               return "CString";
            if (memTN == "String")                                return "String";
            // Guid/SHA1/special refs
            {
                std::string ln = memTN;
                for (char& c : ln) c = (char)tolower((unsigned char)c);
                if (ln.find("guid") != std::string::npos)          return "Guid";
                if (ln.find("sha1") != std::string::npos)          return "Sha1";
                if (ln.find("resourceref") != std::string::npos)   return "ResourceRef";
                if (ln.find("typeref") != std::string::npos)       return "TypeRef";
                if (ln.find("boxedvalueref") != std::string::npos) return "BoxedValueRef";
            }
            return memTN; // struct/class/pointer name
        }
        // Fallback: use structural type
        int sType = (tFlags >> 4) & 0x1F;
        return getFieldTypeName(sType);
    }

    std::string resolveRobotoArrayElemName(const std::string& elemName)
    {
        if (elemName.empty())                              return "Unknown";
        if (elemName == "Boolean")                         return "Boolean";
        if (elemName == "Int8")                            return "Int8";
        if (elemName == "UInt8" || elemName == "Uint8")  return "UInt8";
        if (elemName == "Int16")                           return "Int16";
        if (elemName == "UInt16" || elemName == "Uint16")  return "UInt16";
        if (elemName == "Int32")                           return "Int32";
        if (elemName == "UInt32" || elemName == "Uint32")  return "UInt32";
        if (elemName == "Int64")                           return "Int64";
        if (elemName == "UInt64" || elemName == "Uint64")  return "UInt64";
        if (elemName == "Float32")                         return "Float32";
        if (elemName == "Float64")                         return "Float64";
        if (elemName == "CString" || elemName == "String") return "CString";
        return elemName;
    }

    // -----------------------------------------------------------------------
    // 64-BIT: Dingo/Skate extraction (Let's fix this y'all!)
    // -----------------------------------------------------------------------
    int extractDingoTypes(MemoryReader& reader)
    {
        allTypes.clear();
        if (_exePath.empty()) return 0;

        auto sections = readPeSections(_exePath);

        int64_t tiSecStart = 0, tiSecEnd = 0;
        int64_t dataStart = 0, dataEnd = 0;
        int64_t rdataStart = 0, rdataEnd = 0;
        int64_t fieldInfStart = 0, fieldInfEnd = 0;

        for (auto& s : sections) {
            int64_t sv = _baseAddr + s.virtualAddress;
            int64_t se = sv + s.virtualSize;
            if (s.name == "typeinfo") { tiSecStart = sv; tiSecEnd = se; }
            if (s.name == ".data") { dataStart = sv; dataEnd = se; }
            if (s.name == ".rdata") { rdataStart = sv; rdataEnd = se; }
            if (s.name == "fieldinf") { fieldInfStart = sv; fieldInfEnd = se; }
        }

        TD_Log("[Dingo] typeinfo: 0x%llX - 0x%llX", (unsigned long long)tiSecStart, (unsigned long long)tiSecEnd);
        TD_Log("[Dingo] .data:    0x%llX - 0x%llX", (unsigned long long)dataStart, (unsigned long long)dataEnd);
        TD_Log("[Dingo] .rdata:   0x%llX - 0x%llX", (unsigned long long)rdataStart, (unsigned long long)rdataEnd);
        TD_Log("[Dingo] fieldinf: 0x%llX - 0x%llX", (unsigned long long)fieldInfStart, (unsigned long long)fieldInfEnd);

        if (dataStart == 0) { TD_Log("[Dingo] No .data section - aborting"); return 0; }

        auto isStrAddr = [&](int64_t a) -> bool { return rdataStart && a >= rdataStart && a < rdataEnd; };
        auto isDataAddr = [&](int64_t a) -> bool { return dataStart && a >= dataStart && a < dataEnd; };
        auto isTIAddr = [&](int64_t a) -> bool { return tiSecStart && a >= tiSecStart && a < tiSecEnd; };

        // Pre-build sharedPtr → TypeInfo address map for O(1) parent lookup
        std::unordered_map<int64_t, int64_t> sharedPtrToTI;
        for (int64_t pos = dataStart; pos < dataEnd - 0x18; pos += 8) {
            try {
                reader.position = pos;
                int64_t snp = reader.readLong();
                if (snp == 0 || !isStrAddr(snp)) continue;
                reader.position = pos + 0x10;
                int64_t sShared = reader.readLong();
                if (sShared == 0 || !isDataAddr(sShared)) continue;
                reader.position = pos + 0x08;
                int64_t sHop = reader.readLong();
                if (sHop != 0 && !isDataAddr(sHop)) continue;
                if (!sharedPtrToTI.count(sShared))
                    sharedPtrToTI[sShared] = pos;
            }
            catch (...) { continue; }
        }
        TD_Log("[Dingo] sharedPtr map built: %d entries", (int)sharedPtrToTI.size());

        std::unordered_set<int64_t>     visitedTI;
        std::unordered_set<std::string> addedNames;
        int typeCount = 0;

        // ----------------------------------------------------------------
        // PASS 1: Linear typeinfo section scan (hashed type names)
        // ----------------------------------------------------------------
        if (tiSecStart != 0)
        {
            TD_Log("[Dingo] Pass 1: linear typeinfo section scan...");

            // Detect stride
            int p1Stride = 0x40;
            int bestHits = 0;
            for (int s = 0x40; s <= 0x80; s += 8) {
                int hits = 0;
                for (int64_t p = tiSecStart; p + 0x38 < tiSecEnd && hits < 40; p += s) {
                    try {
                        reader.position = p;
                        uint32_t h = reader.readUInt();
                        if (h == 0) continue;
                        reader.position = p + 0x18;
                        int64_t nsPtr = reader.readLong();
                        if (nsPtr != 0 && !isDataAddr(nsPtr)) continue;
                        reader.position = p + 0x20;
                        int64_t parPtr = reader.readLong();
                        if (parPtr != 0 && !isDataAddr(parPtr)) continue;
                        reader.position = p + 0x30;
                        int64_t faPtr = reader.readLong();
                        if (faPtr != 0 && !isDataAddr(faPtr)) continue;
                        ++hits;
                    }
                    catch (...) {}
                }
                if (hits > bestHits) { bestHits = hits; p1Stride = s; }
            }
            TD_Log("[Dingo] Pass 1 stride=0x%X", p1Stride);

            int pass1Count = 0;
            for (int64_t pos = tiSecStart; pos + 0x38 <= tiSecEnd; pos += p1Stride)
            {
                if (isCancelled()) { TD_Log("[Dingo] Pass 1 cancelled."); break; }

                try {
                    reader.position = pos;
                    uint32_t nameHash = reader.readUInt();
                    if (nameHash == 0) continue;
                    if (visitedTI.count(pos)) continue;

                    reader.position = pos + 0x06;
                    uint16_t flags = reader.readUShort();
                    flags >>= 1;
                    int rawTV = (flags >> 4) & 0x1F;
                    int tiTypeVal = (rawTV == 2) ? 2 : (rawTV == 8) ? 8 : 3;

                    // Namespace
                    std::string tiNs;
                    reader.position = pos + 0x18;
                    int64_t nsNode = reader.readLong();
                    if (nsNode != 0 && isDataAddr(nsNode)) {
                        try {
                            reader.position = nsNode;
                            int64_t nsNamePtr = reader.readLong();
                            if (nsNamePtr != 0 && isStrAddr(nsNamePtr))
                                tiNs = readStr(reader, nsNamePtr, 256);
                        }
                        catch (...) {}
                    }

                    // Base type
                    std::string tiBase;
                    reader.position = pos + 0x20;
                    int64_t parentNode = reader.readLong();
                    if (tiTypeVal != 8 && parentNode != 0 && isDataAddr(parentNode)) {
                        try {
                            reader.position = parentNode;
                            int64_t parentTIPtr = reader.readLong();
                            if (isTIAddr(parentTIPtr)) {
                                reader.position = parentTIPtr;
                                uint32_t pHash = reader.readUInt();
                                reader.readByte(); reader.readByte();
                                uint16_t pFlags = reader.readUShort();
                                pFlags >>= 1;
                                int pTypeVal = (pFlags >> 4) & 0x1F;
                                const char* pPfx = (pTypeVal == 2) ? "Struct" : (pTypeVal == 8) ? "Enum" : "Class";
                                char buf[64]; snprintf(buf, sizeof(buf), "%s_%08X", pPfx, pHash);
                                tiBase = buf;
                            }
                            else if (isDataAddr(parentTIPtr)) {
                                reader.position = parentTIPtr;
                                int64_t pnPtr = reader.readLong();
                                if (pnPtr != 0 && isStrAddr(pnPtr))
                                    tiBase = readStr(reader, pnPtr, 256);
                            }
                        }
                        catch (...) {}
                    }

                    const char* typePrefix = (tiTypeVal == 2) ? "Struct" : (tiTypeVal == 8) ? "Enum" : "Class";
                    char nameBuf[64]; snprintf(nameBuf, sizeof(nameBuf), "%s_%08X", typePrefix, nameHash);
                    std::string tiName = nameBuf;

                    if (addedNames.count(tiName)) continue;
                    visitedTI.insert(pos);
                    addedNames.insert(tiName);

                    std::string category = (tiTypeVal == 2) ? "Structs" : (tiTypeVal == 8) ? "Enums" : "Classes";

                    // Extract fields via ExtractSkateFields logic
                    auto fields = extractSkateFields(reader, pos, 10000, tiName, false, category);

                    allTypes.push_back({ tiName, tiNs,
                        tiNs.empty() ? tiName : tiNs + "." + tiName,
                        category, tiBase, std::move(fields) });
                    ++typeCount;
                    ++pass1Count;

                    if (pass1Count <= 20 || pass1Count % 500 == 0)
                        TD_Log("  [Dingo P1] [%d] %s: %s base='%s'",
                            pass1Count, category.c_str(), tiName.c_str(), tiBase.c_str());
                }
                catch (...) { continue; }
            }
            TD_Log("[Dingo] Pass 1 complete: %d hashed types. Total so far: %d",
                pass1Count, (int)allTypes.size());
        }

        // ----------------------------------------------------------------
        // PASS 2: Linear .data scan for named TypeInfo structs
        // ----------------------------------------------------------------
        TD_Log("[Dingo] Pass 2: linear .data scan for named TypeInfo structs...");
        int pass2Count = 0;

        for (int64_t pos = dataStart; pos < dataEnd - 0x20; pos += 8)
        {
            if (isCancelled()) { TD_Log("[Dingo] Pass 2 cancelled."); break; }

            try {
                reader.position = pos;
                int64_t namePtr = reader.readLong();
                if (namePtr == 0 || !isStrAddr(namePtr)) continue;

                reader.position = pos + 0x10;
                int64_t sharedPtr = reader.readLong();
                if (!isDataAddr(sharedPtr)) continue;

                reader.position = pos + 0x08;
                int64_t parentNode = reader.readLong();
                if (parentNode != 0 && !isDataAddr(parentNode)) continue;

                // Additional validation
                {
                    reader.position = pos;
                    int64_t np2 = reader.readLong();
                    if (np2 == 0 || !isStrAddr(np2)) continue;
                    std::string name2 = readStr(reader, np2, 256);
                    if (name2.empty() || !isValidTypeName(name2)) continue;
                    if (name2.size() < 2 || name2.size() > 100) continue;
                    bool hasLetter = false;
                    for (char c : name2) if (isalpha((unsigned char)c)) { hasLetter = true; break; }
                    if (!hasLetter || name2.find(' ') != std::string::npos) continue;
                    reader.position = pos + 0x10;
                    int64_t sp2 = reader.readLong();
                    if (!isDataAddr(sp2)) continue;
                    reader.position = pos + 0x18;
                    int64_t ff2 = reader.readLong();
                    if (ff2 != 0 && !isStrAddr(ff2)) continue;
                }

                if (visitedTI.count(pos)) continue;

                reader.position = pos;
                int64_t tnPtr = reader.readLong();
                std::string typeName = readStr(reader, tnPtr, 256);
                if (typeName.empty()) continue;
                if (addedNames.count(typeName)) { visitedTI.insert(pos); continue; }

                visitedTI.insert(pos);
                addedNames.insert(typeName);

                // Parent lookup via sharedPtr map
                std::string baseTypeName;
                try {
                    reader.position = pos + 0x10;
                    int64_t sharedNode = reader.readLong();
                    if (sharedNode != 0 && isDataAddr(sharedNode)) {
                        reader.position = sharedNode + 0x20;
                        int64_t parentShared2 = reader.readLong();
                        if (parentShared2 != 0 && isDataAddr(parentShared2) && parentShared2 != sharedNode) {
                            auto it = sharedPtrToTI.find(parentShared2);
                            if (it != sharedPtrToTI.end() && it->second != pos) {
                                reader.position = it->second;
                                int64_t parentNP = reader.readLong();
                                if (parentNP != 0 && isStrAddr(parentNP)) {
                                    std::string pn = readStr(reader, parentNP, 256);
                                    if (!pn.empty() && isValidTypeName(pn) && pn != typeName &&
                                        pn.size() >= 2 && pn.find(' ') == std::string::npos) {
                                        bool hasl = false;
                                        for (char c : pn) if (isalpha((unsigned char)c)) { hasl = true; break; }
                                        if (hasl) baseTypeName = pn;
                                    }
                                }
                            }
                        }
                    }
                }
                catch (...) {}

                // Count own fields
                int64_t fieldArrayStart = pos + 0x18;
                int ownCount = 0;
                for (int i = 0; i < 2000; ++i) {
                    reader.position = fieldArrayStart + i * 0x20LL;
                    int64_t fnp = reader.readLong();
                    if (fnp == 0 || !isStrAddr(fnp)) break;
                    std::string fn = readStr(reader, fnp, 256);
                    if (fn.empty() || !isValidTypeName(fn)) break;
                    ++ownCount;
                }

                std::string            category = "Classes";
                std::vector<FieldItem> fields;
                if (ownCount > 0)
                    fields = extractDingoFields(reader, pos, ownCount, typeName, pass2Count < 3, category);

                allTypes.push_back({ typeName, {}, typeName, category, baseTypeName, std::move(fields) });
                ++typeCount;
                ++pass2Count;

                if (pass2Count <= 10 || pass2Count % 1000 == 0)
                    TD_Log("  [Dingo P2] [%d] %s: %s (fields=%d)",
                        pass2Count, category.c_str(), typeName.c_str(), (int)allTypes.back().fields.size());
            }
            catch (...) { continue; }
        }

        TD_Log("[Dingo] Pass 2 complete: %d new named types. Total: %d",
            pass2Count, (int)allTypes.size());
        return (int)allTypes.size();
    }

    // -----------------------------------------------------------------------
    // Dingo/Skate: field extraction for hashed (typeinfo section) types
    // -----------------------------------------------------------------------
    std::vector<FieldItem> extractSkateFields(MemoryReader& reader, int64_t typeInfoPtr,
        int fieldCount, const std::string& typeName, bool dbg, std::string& category)
    {
        std::vector<FieldItem> fields;
        if (fieldCount <= 0) return fields;

        int ownCount = fieldCount;
        // Try to read refined count from +0x30
        try {
            reader.position = typeInfoPtr + 0x30;
            int64_t enc30 = reader.readLong();
            if (!isValidR(enc30)) {
                int total = (int)(enc30 & 0xFFFF);
                int own = (int)((enc30 >> 16) & 0xFFFF);
                if (own > 0 && own < 2000) ownCount = own;
                else if (total > 0 && total < 2000) ownCount = total;
            }
        }
        catch (...) {}

        // Read +0x38 offset table
        int64_t off38[8] = {};
        try {
            reader.position = typeInfoPtr + 0x38;
            for (auto& o : off38) o = reader.readLong();
        }
        catch (...) {}

        // Score candidates
        int64_t bestPtr = 0;
        int     bestScore = 0;
        bool    bestEnum = false;

        for (int i = 0; i < 8; ++i) {
            if (off38[i] == 0) continue;
            bool isEn = false;
            int  sc = scoreRobotoFieldArray(reader, off38[i], ownCount + 20, isEn);
            if (sc > bestScore) { bestScore = sc; bestPtr = off38[i]; bestEnum = isEn; }
        }

        if (bestPtr == 0 || bestScore == 0) return fields;
        if (bestEnum) category = "Enums";

        std::vector<uint32_t> paddingValues;

        for (int i = 0; i < ownCount + 20 && (int)fields.size() < ownCount; ++i) {
            int64_t ep = bestPtr + i * 24LL;
            try {
                reader.position = ep;
                int64_t  np = reader.readLong();
                uint16_t efl = reader.readUShort();
                uint16_t eOff = reader.readUShort();
                uint32_t pad = reader.readUInt();
                int64_t  tp = reader.readLong();

                if (pad == 0x00000001) break;
                if (np == 0) break;
                if (!isValidR(np) && !isValid(np)) break;

                std::string nm = readStr(reader, np, 256);
                if (nm.empty() || !isValidTypeName(nm)) break;
                if (nm == "Inherited") continue;

                if (bestEnum) {
                    FieldItem fi;
                    fi.name = nm;
                    fi.type = std::to_string((int32_t)tp);
                    fi.offset = (int32_t)tp;
                    fields.push_back(std::move(fi));
                    paddingValues.push_back(pad);
                }
                else {
                    std::string ftName = "Unknown";
                    bool        isArr = false;
                    std::string arrElem;
                    if (tp != 0 && (isValidR(tp) || isValid(tp)))
                        ftName = resolveRobotoFieldType(reader, tp, isArr, arrElem);

                    FieldItem fi;
                    fi.name = nm;
                    fi.type = ftName;
                    fi.offset = (int)eOff;
                    fi.isArray = isArr;
                    fi.arrayElemType = arrElem;
                    fields.push_back(std::move(fi));
                    paddingValues.push_back(pad);
                }
            }
            catch (...) { continue; }
        }

        // Sort
        if (bestEnum) {
            std::sort(fields.begin(), fields.end(),
                [](const FieldItem& a, const FieldItem& b) { return a.offset < b.offset; });
        }
        else if (paddingValues.size() == fields.size() && !fields.empty()) {
            std::vector<std::pair<uint32_t, FieldItem>> combined;
            combined.reserve(fields.size());
            for (size_t i = 0; i < fields.size(); ++i)
                combined.push_back({ (paddingValues[i] >> 16) & 0xFFFF, std::move(fields[i]) });
            std::sort(combined.begin(), combined.end(), [](auto& a, auto& b) { return a.first < b.first; });
            fields.clear();
            for (auto& [idx, fi] : combined) fields.push_back(std::move(fi));
        }
        else {
            std::sort(fields.begin(), fields.end(),
                [](const FieldItem& a, const FieldItem& b) { return a.offset < b.offset; });
        }

        return fields;
    }

    // -----------------------------------------------------------------------
    // Dingo: field extraction for named (data section) types
    // -----------------------------------------------------------------------
    std::vector<FieldItem> extractDingoFields(MemoryReader& reader, int64_t typeInfoPtr,
        int fieldCount, const std::string& typeName, bool dbg, std::string& category)
    {
        std::vector<FieldItem> fields;
        if (fieldCount <= 0) return fields;

        if (_exePath.empty()) return fields;
        auto sections = readPeSections(_exePath);

        int64_t rdataStart = 0, rdataEnd = 0;
        int64_t dataStart = 0, dataEnd = 0;
        int64_t fiStart = 0, fiEnd = 0;
        for (auto& s : sections) {
            int64_t sv = _baseAddr + s.virtualAddress;
            int64_t se = sv + s.virtualSize;
            if (s.name == ".rdata") { rdataStart = sv; rdataEnd = se; }
            if (s.name == ".data") { dataStart = sv; dataEnd = se; }
            if (s.name == "fieldinf") { fiStart = sv; fiEnd = se; }
        }

        auto isStrAddr = [&](int64_t a) { return rdataStart && a >= rdataStart && a < rdataEnd; };
        auto isDataAddr = [&](int64_t a) { return dataStart && a >= dataStart && a < dataEnd;  };
        auto isFIAddr = [&](int64_t a) { return fiStart && a >= fiStart && a < fiEnd;    };

        int64_t fieldArrayStart = typeInfoPtr + 0x18;
        const int ENTRY_STRIDE = 0x20;

        if (dbg)
            TD_Log("  [Dingo] %s: fieldArrayStart=0x%llX count=%d",
                typeName.c_str(), (unsigned long long)fieldArrayStart, fieldCount);

        for (int i = 0; i < fieldCount && i < 2000; ++i) {
            int64_t entryAddr = fieldArrayStart + i * (int64_t)ENTRY_STRIDE;
            try {
                reader.position = entryAddr;
                int64_t namePtr = reader.readLong();
                if (namePtr == 0 || !isStrAddr(namePtr)) {
                    if (dbg) TD_Log("  [Dingo] [%d] namePtr not in .rdata, stopping", i);
                    break;
                }

                std::string fieldName = readStr(reader, namePtr, 256);
                if (fieldName.empty() || !isValidTypeName(fieldName)) {
                    if (dbg) TD_Log("  [Dingo] [%d] bad name, stopping", i);
                    break;
                }

                reader.position = entryAddr + 0x18;
                int64_t declRaw = reader.readLong();
                int sortIndex = (declRaw >= 0 && declRaw < 10000 &&
                    !isStrAddr(declRaw) && !isDataAddr(declRaw) && !isFIAddr(declRaw))
                    ? (int)declRaw : i;

                if (dbg && i < 12)
                    TD_Log("  [Dingo] [%d] '%s'  declIdx=%d  entryAddr=0x%llX",
                        i, fieldName.c_str(), sortIndex, (unsigned long long)entryAddr);

                FieldItem fi;
                fi.name = fieldName;
                fi.type = "Unknown";
                fi.offset = sortIndex;
                fields.push_back(std::move(fi));
            }
            catch (...) {
                if (dbg) TD_Log("  [Dingo] [%d] exception", i);
                break;
            }
        }

        category = "Classes";
        std::sort(fields.begin(), fields.end(),
            [](const FieldItem& a, const FieldItem& b) { return a.offset < b.offset; });

        if (dbg)
            TD_Log("  [Dingo] Extracted %d/%d fields, category=%s",
                (int)fields.size(), fieldCount, category.c_str());
        return fields;
    }
};

// ---------------------------------------------------------------------------
// Public C-style API
// ---------------------------------------------------------------------------
extern "C" {

    struct TD_FieldItem {
        char name[256];
        char type[128];
        int  offset;
        int  isArray;
        char arrayElemType[128];
    };

    struct TD_TypeItem {
        char name[256];
        char ns[128];
        char fullName[384];
        char category[32];
        char baseType[256];
        int  fieldCount;
    };

    struct TD_Context {
        TypeDumper* dumper;
        // Flat index over getTypeInfoToName() — built lazily on first td_get_typeinfo_entry call
        // Cleared by td_dump_memory_* so it is rebuilt after each new dump
        std::vector<std::pair<int64_t, const std::string*>> typeInfoIndex;
    };

    // Set before calling td_dump_memory_* to override auto-detection.
    // mode: -1 = auto, 0 = Jupiter, 1 = Havana, 2 = Walrus, 3 = Roboto, 4 = Skate/Dingo
    // Pass -1 to restore auto-detect behaviour.
    void td_set_force_mode(TD_Context* ctx, int mode)
    {
        if (ctx && ctx->dumper)
            ctx->dumper->setForceMode(mode);
    }

    TD_Context* td_create()
    {
        auto* ctx = new TD_Context();
        ctx->dumper = new TypeDumper();
        return ctx;
    }

    void td_destroy(TD_Context* ctx)
    {
        if (!ctx) return;
        delete ctx->dumper;
        delete ctx;
    }

    void td_cancel(TD_Context* ctx)
    {
        if (ctx && ctx->dumper) ctx->dumper->cancelRequested = true;
    }

    int td_dump_memory_32(TD_Context* ctx, HANDLE hProcess, int64_t baseAddress)
    {
        if (!ctx || !ctx->dumper) return -1;
        ctx->dumper->cancelRequested = false;
        ctx->typeInfoIndex.clear();
        return ctx->dumper->dumpMemory32(hProcess, baseAddress);
    }

    int td_dump_memory_64(TD_Context* ctx, HANDLE hProcess, int64_t baseAddress, const char* exePath)
    {
        if (!ctx || !ctx->dumper) return -1;
        ctx->dumper->cancelRequested = false;
        ctx->typeInfoIndex.clear();
        return ctx->dumper->dumpMemory64(hProcess, baseAddress, exePath ? exePath : "");
    }

    int td_get_type_count(TD_Context* ctx)
    {
        if (!ctx || !ctx->dumper) return 0;
        return (int)ctx->dumper->allTypes.size();
    }

    int td_get_type(TD_Context* ctx, int index, TD_TypeItem* out)
    {
        if (!ctx || !ctx->dumper || !out) return -1;
        const auto& types = ctx->dumper->allTypes;
        if (index < 0 || index >= (int)types.size()) return -1;
        const TypeItem& t = types[index];
        auto scopy = [](char* dst, size_t dsz, const std::string& src) {
            size_t n = std::min(src.size(), dsz - 1);
            memcpy(dst, src.c_str(), n);
            dst[n] = 0;
            };
        scopy(out->name, sizeof(out->name), t.name);
        scopy(out->ns, sizeof(out->ns), t.ns);
        scopy(out->fullName, sizeof(out->fullName), t.fullName);
        scopy(out->category, sizeof(out->category), t.category);
        scopy(out->baseType, sizeof(out->baseType), t.baseType);
        out->fieldCount = (int)t.fields.size();
        return 0;
    }

    int td_get_type_field(TD_Context* ctx, int typeIndex, int fieldIndex, TD_FieldItem* out)
    {
        if (!ctx || !ctx->dumper || !out) return -1;
        const auto& types = ctx->dumper->allTypes;
        if (typeIndex < 0 || typeIndex >= (int)types.size()) return -1;
        const auto& fields = types[typeIndex].fields;
        if (fieldIndex < 0 || fieldIndex >= (int)fields.size()) return -1;
        const FieldItem& f = fields[fieldIndex];
        auto scopy = [](char* dst, size_t dsz, const std::string& src) {
            size_t n = std::min(src.size(), dsz - 1);
            memcpy(dst, src.c_str(), n);
            dst[n] = 0;
            };
        scopy(out->name, sizeof(out->name), f.name);
        scopy(out->type, sizeof(out->type), f.type);
        scopy(out->arrayElemType, sizeof(out->arrayElemType), f.arrayElemType);
        out->offset = f.offset;
        out->isArray = f.isArray ? 1 : 0;
        return 0;
    }

    const char* td_get_status(TD_Context* ctx)
    {
        if (!ctx || !ctx->dumper) return "";
        return ctx->dumper->statusMessage.c_str();
    }

    // Returns the number of typeinfo-ptr -> name mappings built during the dump
    int td_get_typeinfo_map_count(TD_Context* ctx)
    {
        if (!ctx || !ctx->dumper) return 0;
        return (int)ctx->dumper->getTypeInfoToName().size();
    }

    // Fills outPtr and outName for the entry at the given index
    // outName must point to a buffer of at least 256 bytes
    // Returns 0 on success, -1 on failure
    int td_get_typeinfo_entry(TD_Context* ctx, int index, int64_t* outPtr, char* outName, int outNameSize)
    {
        if (!ctx || !ctx->dumper || !outPtr || !outName || outNameSize <= 0) return -1;
        const auto& map = ctx->dumper->getTypeInfoToName();
        if (index < 0 || index >= (int)map.size()) return -1;

        // Build a flat index on first call — O(n) once instead of O(n) per entry
        // Stored on TD_Context so it is rebuilt if the dumper runs again
        if (ctx->typeInfoIndex.empty()) {
            ctx->typeInfoIndex.reserve(map.size());
            for (auto& kv : map)
                ctx->typeInfoIndex.push_back({ kv.first, &kv.second });
        }

        *outPtr = ctx->typeInfoIndex[index].first;
        const std::string& nm = *ctx->typeInfoIndex[index].second;
        size_t n = std::min(nm.size(), (size_t)(outNameSize - 1));
        memcpy(outName, nm.c_str(), n);
        outName[n] = '\0';
        return 0;
    }

}