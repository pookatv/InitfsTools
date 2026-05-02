#include "MemoryReader.h"

#include <cassert>
#include <cctype>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline uint32_t vpEx(HANDLE h, int64_t addr, size_t sz, uint32_t newProt)
{
    uint32_t old = 0;
    VirtualProtectEx(h,
        reinterpret_cast<LPVOID>(static_cast<uintptr_t>(addr)),
        static_cast<SIZE_T>(sz),
        static_cast<DWORD>(newProt),
        reinterpret_cast<PDWORD>(&old));
    return old;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
MemoryReader::MemoryReader(HANDLE hProcess_, int64_t initialAddr)
    : position(initialAddr)
    , hProcess(hProcess_)
    , _ownsHandle(false)
{
}

MemoryReader::MemoryReader(int pid, int64_t initialAddr)
    : position(initialAddr)
    , _ownsHandle(true)
{
    hProcess = OpenProcess(0x0010 /*PROCESS_VM_READ*/, FALSE, static_cast<DWORD>(pid));
    if (hProcess == nullptr) hProcess = INVALID_HANDLE_VALUE;
}

MemoryReader::MemoryReader(MemoryReader&& o) noexcept
    : position(o.position)
    , hProcess(o.hProcess)
    , _ownsHandle(o._ownsHandle)
{
    o.hProcess    = INVALID_HANDLE_VALUE;
    o._ownsHandle = false;
    o.position    = 0;
    std::memcpy(_buf, o._buf, sizeof(_buf));
}

MemoryReader& MemoryReader::operator=(MemoryReader&& o) noexcept
{
    if (this != &o) {
        dispose();
        position    = o.position;
        hProcess    = o.hProcess;
        _ownsHandle = o._ownsHandle;
        std::memcpy(_buf, o._buf, sizeof(_buf));
        o.hProcess    = INVALID_HANDLE_VALUE;
        o._ownsHandle = false;
        o.position    = 0;
    }
    return *this;
}

MemoryReader::~MemoryReader()
{
    dispose();
}

void MemoryReader::dispose()
{
    if (_ownsHandle && hProcess != INVALID_HANDLE_VALUE && hProcess != nullptr) {
        CloseHandle(hProcess);
        hProcess    = INVALID_HANDLE_VALUE;
        _ownsHandle = false;
    }
}

// ---------------------------------------------------------------------------
// VirtualProtect helpers
// ---------------------------------------------------------------------------
uint32_t MemoryReader::unprotect(int64_t addr, size_t sz)
{
    return vpEx(hProcess, addr, sz, 0x02 /*PAGE_READWRITE*/);
}

void MemoryReader::reprotect(int64_t addr, size_t sz, uint32_t oldProt)
{
    vpEx(hProcess, addr, sz, oldProt);
}

// ---------------------------------------------------------------------------
// fillBuffer
// ---------------------------------------------------------------------------
void MemoryReader::fillBuffer(int numBytes)
{
    assert(numBytes > 0 && numBytes <= static_cast<int>(sizeof(_buf)));

    uint32_t old = unprotect(position, static_cast<size_t>(numBytes));
    SIZE_T   got = 0;
    ReadProcessMemory(hProcess,
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(position)),
        _buf, static_cast<SIZE_T>(numBytes), &got);
    reprotect(position, static_cast<size_t>(numBytes), old);

    position += numBytes;
}

// ---------------------------------------------------------------------------
// Primitive reads
// ---------------------------------------------------------------------------
uint8_t MemoryReader::readByte()
{
    fillBuffer(1);
    return _buf[0];
}

int16_t MemoryReader::readShort()
{
    fillBuffer(2);
    return static_cast<int16_t>(
        static_cast<uint16_t>(_buf[0]) |
        static_cast<uint16_t>(_buf[1]) << 8);
}

uint16_t MemoryReader::readUShort()
{
    fillBuffer(2);
    return static_cast<uint16_t>(
        static_cast<uint16_t>(_buf[0]) |
        static_cast<uint16_t>(_buf[1]) << 8);
}

int32_t MemoryReader::readInt()
{
    fillBuffer(4);
    return static_cast<int32_t>(
        static_cast<uint32_t>(_buf[0])        |
        static_cast<uint32_t>(_buf[1]) <<  8  |
        static_cast<uint32_t>(_buf[2]) << 16  |
        static_cast<uint32_t>(_buf[3]) << 24);
}

uint32_t MemoryReader::readUInt()
{
    fillBuffer(4);
    return static_cast<uint32_t>(_buf[0])        |
           static_cast<uint32_t>(_buf[1]) <<  8  |
           static_cast<uint32_t>(_buf[2]) << 16  |
           static_cast<uint32_t>(_buf[3]) << 24;
}

int64_t MemoryReader::readLong()
{
    fillBuffer(8);
    uint32_t lo = static_cast<uint32_t>(_buf[0])        |
                  static_cast<uint32_t>(_buf[1]) <<  8  |
                  static_cast<uint32_t>(_buf[2]) << 16  |
                  static_cast<uint32_t>(_buf[3]) << 24;
    uint32_t hi = static_cast<uint32_t>(_buf[4])        |
                  static_cast<uint32_t>(_buf[5]) <<  8  |
                  static_cast<uint32_t>(_buf[6]) << 16  |
                  static_cast<uint32_t>(_buf[7]) << 24;
    return static_cast<int64_t>(
        static_cast<uint64_t>(hi) << 32 |
        static_cast<uint64_t>(lo));
}

uint64_t MemoryReader::readULong()
{
    fillBuffer(8);
    uint32_t lo = static_cast<uint32_t>(_buf[0])        |
                  static_cast<uint32_t>(_buf[1]) <<  8  |
                  static_cast<uint32_t>(_buf[2]) << 16  |
                  static_cast<uint32_t>(_buf[3]) << 24;
    uint32_t hi = static_cast<uint32_t>(_buf[4])        |
                  static_cast<uint32_t>(_buf[5]) <<  8  |
                  static_cast<uint32_t>(_buf[6]) << 16  |
                  static_cast<uint32_t>(_buf[7]) << 24;
    return static_cast<uint64_t>(hi) << 32 |
           static_cast<uint64_t>(lo);
}

// ---------------------------------------------------------------------------
// readNullTerminatedString
// Reads an int64 pointer at current position, then reads the string there
// ---------------------------------------------------------------------------
std::string MemoryReader::readNullTerminatedString()
{
    int64_t offset = readLong();
    int64_t orig   = position;
    position       = offset;

    std::string result;
    result.reserve(64);
    while (true) {
        uint8_t c = readByte();
        if (c == 0x00) break;
        result.push_back(static_cast<char>(c));
    }

    position = orig;
    return result;
}

// ---------------------------------------------------------------------------
// readBytes
// ---------------------------------------------------------------------------
std::vector<uint8_t> MemoryReader::readBytes(int numBytes)
{
    if (numBytes <= 0) return {};

    std::vector<uint8_t> out(static_cast<size_t>(numBytes), 0);
    uint32_t old = unprotect(position, static_cast<size_t>(numBytes));
    SIZE_T   got = 0;
    bool ok = ReadProcessMemory(hProcess,
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(position)),
        out.data(), static_cast<SIZE_T>(numBytes), &got) != FALSE;
    reprotect(position, static_cast<size_t>(numBytes), old);

    if (!ok) return {};
    out.resize(got);
    position += static_cast<int64_t>(got);
    return out;
}

// ---------------------------------------------------------------------------
// readRaw — non-advancing raw read at an absolute address
// ---------------------------------------------------------------------------
bool MemoryReader::readRaw(int64_t addr, void* dst, size_t sz) const
{
    SIZE_T got = 0;
    return ReadProcessMemory(hProcess,
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)),
        dst, static_cast<SIZE_T>(sz), &got) != FALSE && got == sz;
}

// ---------------------------------------------------------------------------
// readStringSafe — non-advancing, safe ASCII string read
// ---------------------------------------------------------------------------
std::string MemoryReader::readStringSafe(int64_t addr, int maxLen) const
{
    if (addr == 0 || maxLen <= 0) return {};

    // Read up to maxLen bytes in one call instead of one call per byte
    uint8_t tmp[256];
    int     toRead = std::min(maxLen, (int)sizeof(tmp));
    SIZE_T  got = 0;
    if (!ReadProcessMemory(hProcess,
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)),
        tmp, static_cast<SIZE_T>(toRead), &got) || got == 0)
        return {};

    std::string result;
    result.reserve(32);
    for (SIZE_T i = 0; i < got; ++i) {
        uint8_t b = tmp[i];
        if (b == 0)            break;
        if (b < 32 || b > 126) { result.clear(); break; }
        result.push_back(static_cast<char>(b));
    }
    return result;
}

// ---------------------------------------------------------------------------
// scan
// Pattern: hex pairs, optional spaces, "??" = wildcard
// Scans from current position in 1 MB chunks
// ---------------------------------------------------------------------------
struct PatternByte {
    bool    wildcard = false;
    uint8_t value    = 0;
};

std::vector<int64_t> MemoryReader::scan(const std::string& pattern)
{
    // Parse pattern
    std::string p;
    p.reserve(pattern.size());
    for (char c : pattern) if (c != ' ') p.push_back(c);

    if (p.size() % 2 != 0) return {};

    std::vector<PatternByte> pat(p.size() / 2);
    for (size_t i = 0; i < pat.size(); ++i) {
        std::string tok = p.substr(i * 2, 2);
        if (tok == "??") {
            pat[i].wildcard = true;
        } else {
            pat[i].wildcard = false;
            pat[i].value    = static_cast<uint8_t>(std::stoul(tok, nullptr, 16));
        }
    }

    const int CHUNK = 1024 * 1024;
    std::vector<int64_t> results;

    while (true) {
        int64_t chunkBase = position;
        std::vector<uint8_t> buf = readBytes(CHUNK);
        if (buf.empty()) break;

        size_t sz  = buf.size();
        size_t psz = pat.size();
        if (sz < psz) break;

        for (size_t i = 0; i <= sz - psz; ++i) {
            bool found = true;
            for (size_t j = 0; j < psz; ++j) {
                if (!pat[j].wildcard && buf[i + j] != pat[j].value) {
                    found = false;
                    break;
                }
            }
            if (found)
                results.push_back(chunkBase + static_cast<int64_t>(i));
        }
    }

    return results;
}