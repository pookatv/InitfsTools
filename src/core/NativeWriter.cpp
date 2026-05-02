#include "NativeWriter.h"
#include <cstring>

NativeWriter::NativeWriter(std::ostream& stream, bool leaveOpen)
    : m_stream(stream), m_leaveOpen(leaveOpen)
{
}

int64_t NativeWriter::position() const
{
    return static_cast<int64_t>(m_stream.tellp());
}

void NativeWriter::setPosition(int64_t pos)
{
    m_stream.seekp(static_cast<std::streamoff>(pos));
}

int64_t NativeWriter::length() const
{
    auto cur = m_stream.tellp();
    m_stream.seekp(0, std::ios::end);
    auto end = m_stream.tellp();
    m_stream.seekp(cur);
    return static_cast<int64_t>(end);
}

void NativeWriter::write(uint8_t value)
{
    m_stream.put(static_cast<char>(value));
}

void NativeWriter::write(const uint8_t* data, size_t len)
{
    m_stream.write(reinterpret_cast<const char*>(data), len);
}

void NativeWriter::write(const std::vector<uint8_t>& data)
{
    if (!data.empty())
        m_stream.write(reinterpret_cast<const char*>(data.data()), data.size());
}

void NativeWriter::write(int32_t value)
{
    m_stream.put(char(value));
    m_stream.put(char(value >> 8));
    m_stream.put(char(value >> 16));
    m_stream.put(char(value >> 24));
}

void NativeWriter::write(uint32_t value)
{
    write(static_cast<int32_t>(value));
}

void NativeWriter::write(int64_t value)
{
    write(int32_t(value & 0xFFFFFFFF));
    write(int32_t(value >> 32));
}

void NativeWriter::write(float value)
{
    uint32_t tmp;
    std::memcpy(&tmp, &value, sizeof(uint32_t));
    write(tmp);
}

void NativeWriter::write(double value)
{
    uint64_t tmp;
    std::memcpy(&tmp, &value, sizeof(uint64_t));
    write(int32_t(tmp & 0xFFFFFFFF));
    write(int32_t(tmp >> 32));
}

void NativeWriter::write(bool value)
{
    m_stream.put(value ? char(1) : char(0));
}

void NativeWriter::write(const Guid& value)
{
    m_stream.write(reinterpret_cast<const char*>(value.data()), 16);
}

void NativeWriter::write(const Sha1& value)
{
    auto bytes = value.toByteArray();
    m_stream.write(reinterpret_cast<const char*>(bytes.data()), 20);
}

void NativeWriter::write7BitEncodedInt(int32_t value)
{
    uint32_t v = static_cast<uint32_t>(value);
    while (v >= 0x80)
    {
        m_stream.put(char(v | 0x80));
        v >>= 7;
    }
    m_stream.put(char(v));
}

void NativeWriter::write7BitEncodedLong(int64_t value)
{
    uint64_t v = static_cast<uint64_t>(value);
    while (v >= 0x80)
    {
        m_stream.put(char(v | 0x80));
        v >>= 7;
    }
    m_stream.put(char(v));
}

void NativeWriter::writeNullTerminatedString(const std::string& str)
{
    m_stream.write(str.c_str(), str.size());
    m_stream.put('\0');
}

void NativeWriter::writeSizedString(const std::string& str)
{
    write7BitEncodedInt(static_cast<int32_t>(str.size()));
    m_stream.write(str.c_str(), str.size());
}