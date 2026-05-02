#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// MemoryReader
// ---------------------------------------------------------------------------
class MemoryReader
{
public:
    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    // Attach to an already-opened process handle (TypeDumper usage)
    // The handle is NOT owned — caller must keep it alive and close it
    explicit MemoryReader(HANDLE hProcess, int64_t initialAddr = 0);

    // Open a process by PID (mirrors the C# constructor).
    // The handle IS owned and will be closed in the destructor / dispose().
    explicit MemoryReader(int pid, int64_t initialAddr = 0);

    // Non-copyable, movable
    MemoryReader(const MemoryReader&)            = delete;
    MemoryReader& operator=(const MemoryReader&) = delete;
    MemoryReader(MemoryReader&& o) noexcept;
    MemoryReader& operator=(MemoryReader&& o) noexcept;

    ~MemoryReader();

    // Release the owned handle (if any). Safe to call multiple times
    void dispose();

    // -----------------------------------------------------------------------
    // Position
    // -----------------------------------------------------------------------
    int64_t getPosition() const { return position; }
    void    setPosition(int64_t pos) { position = pos; }

    // Convenience: public field kept for backward-compat with TypeDumper
    int64_t position = 0;

    // Raw handle — readable by TypeDumper helpers
    HANDLE hProcess = INVALID_HANDLE_VALUE;

    // -----------------------------------------------------------------------
    // Primitive reads (advance position)
    // -----------------------------------------------------------------------
    uint8_t  readByte();
    int16_t  readShort();
    uint16_t readUShort();
    int32_t  readInt();
    uint32_t readUInt();
    int64_t  readLong();
    uint64_t readULong();

    // Reads a pointer-sized null-terminated string: first reads an int64 offset,
    // seeks to it, reads the string, then restores position
    std::string readNullTerminatedString();

    // Read exactly numBytes bytes from current position; returns actual bytes read
    std::vector<uint8_t> readBytes(int numBytes);

    // -----------------------------------------------------------------------
    // Pattern scan
    // Pattern format: hex pairs optionally separated by spaces, "??" = wildcard
    // Scans from current position in 1 MB chunks
    // Returns list of absolute addresses where pattern was found
    // -----------------------------------------------------------------------
    std::vector<int64_t> scan(const std::string& pattern);

    // -----------------------------------------------------------------------
    // Low-level helpers used throughout TypeDumper
    // -----------------------------------------------------------------------

    // Read 'sz' bytes from 'addr' without moving position
    bool readRaw(int64_t addr, void* dst, size_t sz) const;

    // Read a null-terminated ASCII string from 'addr' (up to maxLen chars)
    // Returns empty string on failure or non-printable bytes
    std::string readStringSafe(int64_t addr, int maxLen = 256) const;

private:
    bool  _ownsHandle = false;
    uint8_t _buf[20]  = {};

    // Fill internal buffer; advances position
    void fillBuffer(int numBytes);

    // Apply PAGE_READWRITE around a region then restore
    // Returns the old protection so the caller can restore it
    uint32_t unprotect(int64_t addr, size_t sz);
    void     reprotect(int64_t addr, size_t sz, uint32_t oldProtect);
};