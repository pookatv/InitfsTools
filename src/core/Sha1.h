#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <array>

class Sha1
{
public:
    static const Sha1 Zero;

    Sha1() : m_a(0), m_b(0), m_c(0), m_d(0), m_e(0) {}

    explicit Sha1(const uint8_t* bytes, size_t len)
    {
        if (len < 20)
            throw std::invalid_argument("Input buffer is too small");
        m_a = uint32_t(bytes[0]  | bytes[1]  << 8 | bytes[2]  << 16 | bytes[3]  << 24);
        m_b = uint32_t(bytes[4]  | bytes[5]  << 8 | bytes[6]  << 16 | bytes[7]  << 24);
        m_c = uint32_t(bytes[8]  | bytes[9]  << 8 | bytes[10] << 16 | bytes[11] << 24);
        m_d = uint32_t(bytes[12] | bytes[13] << 8 | bytes[14] << 16 | bytes[15] << 24);
        m_e = uint32_t(bytes[16] | bytes[17] << 8 | bytes[18] << 16 | bytes[19] << 24);
    }

    std::array<uint8_t, 20> toByteArray() const
    {
        std::array<uint8_t, 20> b{};
        b[0]  = uint8_t(m_a);       b[1]  = uint8_t(m_a >> 8);
        b[2]  = uint8_t(m_a >> 16); b[3]  = uint8_t(m_a >> 24);
        b[4]  = uint8_t(m_b);       b[5]  = uint8_t(m_b >> 8);
        b[6]  = uint8_t(m_b >> 16); b[7]  = uint8_t(m_b >> 24);
        b[8]  = uint8_t(m_c);       b[9]  = uint8_t(m_c >> 8);
        b[10] = uint8_t(m_c >> 16); b[11] = uint8_t(m_c >> 24);
        b[12] = uint8_t(m_d);       b[13] = uint8_t(m_d >> 8);
        b[14] = uint8_t(m_d >> 16); b[15] = uint8_t(m_d >> 24);
        b[16] = uint8_t(m_e);       b[17] = uint8_t(m_e >> 8);
        b[18] = uint8_t(m_e >> 16); b[19] = uint8_t(m_e >> 24);
        return b;
    }

    bool operator==(const Sha1& o) const
    {
        return m_a == o.m_a && m_b == o.m_b && m_c == o.m_c
            && m_d == o.m_d && m_e == o.m_e;
    }
    bool operator!=(const Sha1& o) const { return !(*this == o); }

    size_t hash() const
    {
        size_t h = 2166136261u;
        auto mix = [&](uint32_t v){ h = (h * 16777619u) ^ v; };
        mix(m_a); mix(m_b); mix(m_c); mix(m_d); mix(m_e);
        return h;
    }

    std::string toString() const
    {
        static const char* hex = "0123456789abcdef";
        std::string s;
        s.reserve(40);
        for (uint32_t v : { m_a, m_b, m_c, m_d, m_e })
            for (int shift : { 0, 8, 16, 24 })
            {
                uint8_t byte = uint8_t(v >> shift);
                s += hex[byte >> 4];
                s += hex[byte & 0xF];
            }
        return s;
    }

private:
    uint32_t m_a, m_b, m_c, m_d, m_e;
};

// std::unordered_map support
namespace std {
    template<> struct hash<Sha1> {
        size_t operator()(const Sha1& s) const { return s.hash(); }
    };
}