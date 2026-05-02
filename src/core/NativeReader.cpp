#include "NativeReader.h"
#include <cstring>
#include <bit>

NativeReader::NativeReader(std::istream& stream)
    : m_stream(stream)
{
    m_stream.seekg(0, std::ios::end);
    m_streamLength = static_cast<int64_t>(m_stream.tellg());
    m_stream.seekg(0, std::ios::beg);
}

NativeReader::NativeReader(std::istream& stream,
    std::shared_ptr<IDeobfuscator> deobfuscator)
    : NativeReader(stream)
{
    m_deobfuscator = std::move(deobfuscator);
    if (m_deobfuscator)
    {
        int64_t newLen = m_deobfuscator->initialize(this);
        if (newLen != -1)
            m_streamLength = newLen;
    }
}

int64_t NativeReader::position() const
{
    return static_cast<int64_t>(m_stream.tellg());
}

void NativeReader::setPosition(int64_t pos)
{
    if (m_deobfuscator && m_deobfuscator->adjustPosition(this, pos))
        return;
    m_stream.seekg(static_cast<std::streamoff>(pos));
}

uint8_t NativeReader::readByte()
{
    fillBuffer(1);
    return m_buffer[0];
}

int16_t NativeReader::readShort()
{
    fillBuffer(2);
    return int16_t(m_buffer[0] | m_buffer[1] << 8);
}

uint16_t NativeReader::readUShort()
{
    fillBuffer(2);
    return uint16_t(m_buffer[0] | m_buffer[1] << 8);
}

int32_t NativeReader::readInt()
{
    fillBuffer(4);
    return int32_t(m_buffer[0]
        | m_buffer[1] << 8
        | m_buffer[2] << 16
        | m_buffer[3] << 24);
}

uint32_t NativeReader::readUInt()
{
    fillBuffer(4);
    return uint32_t(m_buffer[0]
        | m_buffer[1] << 8
        | m_buffer[2] << 16
        | m_buffer[3] << 24);
}

int64_t NativeReader::readLong()
{
    fillBuffer(8);
    uint32_t lo = uint32_t(m_buffer[0] | m_buffer[1] << 8
        | m_buffer[2] << 16 | m_buffer[3] << 24);
    uint32_t hi = uint32_t(m_buffer[4] | m_buffer[5] << 8
        | m_buffer[6] << 16 | m_buffer[7] << 24);
    return int64_t(uint64_t(hi) << 32 | lo);
}

float NativeReader::readFloat()
{
    fillBuffer(4);
    const uint32_t tmp = uint32_t(m_buffer[0]
        | m_buffer[1] << 8
        | m_buffer[2] << 16
        | m_buffer[3] << 24);
    return std::bit_cast<float>(tmp); // constexpr, no UB, no memcpy
}

double NativeReader::readDouble()
{
    fillBuffer(8);
    const uint32_t lo = uint32_t(m_buffer[0] | m_buffer[1] << 8
        | m_buffer[2] << 16 | m_buffer[3] << 24);
    const uint32_t hi = uint32_t(m_buffer[4] | m_buffer[5] << 8
        | m_buffer[6] << 16 | m_buffer[7] << 24);
    const uint64_t tmp = (uint64_t(hi) << 32) | lo;
    return std::bit_cast<double>(tmp);
}

Guid NativeReader::readGuid()
{
    fillBuffer(16);
    Guid g;
    std::memcpy(g.data(), m_buffer, 16);
    return g;
}

Sha1 NativeReader::readSha1()
{
    fillBuffer(20);
    return Sha1(m_buffer, 20);
}

int32_t NativeReader::read7BitEncodedInt()
{
    int32_t result = 0;
    int shift = 0;
    while (true)
    {
        uint8_t b = readByte();
        result |= int32_t(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return result;
        shift += 7;
    }
}

int64_t NativeReader::read7BitEncodedLong()
{
    int64_t result = 0;
    int shift = 0;
    while (true)
    {
        uint8_t b = readByte();
        result |= int64_t(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return result;
        shift += 7;
    }
}

std::string NativeReader::readNullTerminatedString()
{
    std::string s;
    while (true)
    {
        char c = static_cast<char>(readByte());
        if (c == '\0') return s;
        s += c;
    }
}

std::string NativeReader::readSizedString(int len)
{
    if (len <= 0) return {};
    // Single bulk read instead of len individual readByte() calls
    std::vector<uint8_t> raw = readBytes(len);
    // Use string_view range constructor   no extra allocation, strips embedded nulls
    std::string s;
    s.reserve(raw.size());
    for (uint8_t b : raw)
        if (b != '\0') s += static_cast<char>(b);
    return s;
}

std::vector<uint8_t> NativeReader::readToEnd()
{
    int64_t remaining = m_streamLength - position();
    return readBytes(static_cast<int>(remaining));
}

std::vector<uint8_t> NativeReader::readBytes(int count)
{
    if (count <= 0) return {};
    std::vector<uint8_t> buf;
    buf.resize(count); // resize not reserve   read() writes directly into it
    int totalRead = 0;
    int remaining = count;
    while (remaining > 0)
    {
        int n = read(buf.data(), totalRead, remaining);
        if (n == 0) break;
        totalRead += n;
        remaining -= n;
    }
    buf.resize(totalRead); // trim only if short read occurred
    return buf;
}

int NativeReader::read(uint8_t* outBuffer, int offset, int numBytes)
{
    m_stream.read(reinterpret_cast<char*>(outBuffer + offset), numBytes);
    int n = static_cast<int>(m_stream.gcount());
    if (m_deobfuscator && n > 0)
        m_deobfuscator->deobfuscate(outBuffer, position(), offset, n);
    return n;
}

void NativeReader::fillBuffer(int numBytes)
{
    m_stream.read(reinterpret_cast<char*>(m_buffer), numBytes);
    if (m_deobfuscator)
        m_deobfuscator->deobfuscate(m_buffer, position(), 0, numBytes);
}