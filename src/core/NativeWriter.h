#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <ostream>
#include <array>
#include "Sha1.h"

using Guid = std::array<uint8_t, 16>;

class NativeWriter
{
public:
    explicit NativeWriter(std::ostream& stream, bool leaveOpen = false);
    virtual ~NativeWriter() = default;

    NativeWriter(const NativeWriter&) = delete;
    NativeWriter& operator=(const NativeWriter&) = delete;

    int64_t position() const;
    void    setPosition(int64_t pos);
    int64_t length() const;

    void write(uint8_t value);
    void write(const uint8_t* data, size_t len);
    void write(const std::vector<uint8_t>& data);
    void write(int32_t value);
    void write(uint32_t value);
    void write(int64_t value);
    void write(float value);
    void write(double value);
    void write(bool value);
    void write(const Guid& value);
    void write(const Sha1& value);

    void write7BitEncodedInt(int32_t value);
    void write7BitEncodedLong(int64_t value);

    void writeNullTerminatedString(const std::string& str);
    void writeSizedString(const std::string& str);

protected:
    std::ostream& m_stream;
    bool          m_leaveOpen;
};