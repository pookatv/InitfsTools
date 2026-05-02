#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <istream>
#include <stdexcept>
#include "IDeobfuscator.h"
#include "Sha1.h"

// Cross-platform Guid (16 bytes, no Windows dependency)
using Guid = std::array<uint8_t, 16>;

class NativeReader
{
public:
    explicit NativeReader(std::istream& stream);
    NativeReader(std::istream& stream,
                 std::shared_ptr<IDeobfuscator> deobfuscator);

    virtual ~NativeReader() = default;

    // Non-copyable, movable
    NativeReader(const NativeReader&) = delete;
    NativeReader& operator=(const NativeReader&) = delete;

    virtual int64_t position() const;
    virtual void    setPosition(int64_t pos);
    virtual int64_t length() const { return m_streamLength; }

    uint8_t  readByte();
    int16_t  readShort();
    uint16_t readUShort();
    int32_t  readInt();
    uint32_t readUInt();
    int64_t  readLong();
    float    readFloat();
    double   readDouble();
    Guid     readGuid();
    Sha1     readSha1();

    int32_t  read7BitEncodedInt();
    int64_t  read7BitEncodedLong();

    std::string          readNullTerminatedString();
    std::string          readSizedString(int len);
    std::vector<uint8_t> readToEnd();
    std::vector<uint8_t> readBytes(int count);

    virtual int read(uint8_t* outBuffer, int offset, int numBytes);

protected:
    virtual void fillBuffer(int numBytes);

    std::istream&                  m_stream;
    std::shared_ptr<IDeobfuscator> m_deobfuscator;
    uint8_t                        m_buffer[20]{};
    int64_t                        m_streamLength = 0;
};