#include "MEADeobfuscator.h"
#include "NativeReader.h"
#include <cstring>
#include <stdexcept>

// ---- private helpers ----

int32_t MEADeobfuscator::rollOver(int32_t value, int count)
{
    constexpr int nbits = sizeof(uint32_t) * 8;
    count %= nbits;
    if (count == 0) return value;
    int32_t high = static_cast<int32_t>(
        (static_cast<uint32_t>(value) >> (nbits - count)) & ~(~0u << count));
    value = static_cast<int32_t>(static_cast<uint32_t>(value) << count);
    value |= high;
    return value;
}

void MEADeobfuscator::deobfuscateBlock(uint8_t* buffer, int offset, int count)
{
    int32_t a = 1172968056;
    int32_t z = 0;

    for (int i = 0; i < count; i++)
    {
        int32_t b = static_cast<uint8_t>(
            buffer[i + offset] ^
            (a + ((a >> 8) & 0xFF) + (a >> 16) + ((a >> 24) & 0xFF)));
        buffer[i + offset] = static_cast<uint8_t>(b);

        int32_t c = rollOver(a, b & 0x1F);
        a = rollOver((b | ((b | ((b | (b << 8)) << 8)) << 8)) + c, 1);

        if (z > 16) { a *= 2; z = 0; }
        z++;
    }
}

// ---- IDeobfuscator interface ----

int64_t MEADeobfuscator::initialize(NativeReader* reader)
{
    if (reader->length() < 36)
        return -1;

    reader->setPosition(reader->length() - 36);

    int32_t obfuscationDataSize = reader->readInt();
    // readSizedString reads <7-bit-len> bytes
    std::string obfuscationKey = reader->readSizedString(32);

    int64_t retLength = reader->length();

    if (obfuscationKey == "@e!adnXd$^!rfOsrDyIrI!xVgHeA!6Vc")
    {
        reader->setPosition(reader->length() - obfuscationDataSize);
        std::vector<uint8_t> buf = reader->readBytes(obfuscationDataSize);

        uint32_t tmpA = 0, tmpB = 0, subTotalA = 0, subTotalB = 0, total = 0;

        int16_t unknown = static_cast<int16_t>(
            uint16_t(buf[392]) | uint16_t(buf[393]) << 8);

        if (unknown != 0)
        {
            for (int i = 0; i < unknown; i++)
            {
                uint8_t c = buf[410 + i];
                tmpA = c ^ uint32_t(2 * (c + tmpA));
            }
        }

        tmpB += (uint32_t)(buf[405] ^ 2 * (buf[405] + (buf[404] ^ 2 * (buf[404] + (buf[403] ^ 2 * (buf[403] + (buf[402] ^ 2 * buf[402])))))));
        tmpB += (uint32_t)(buf[3] ^ 2 * (buf[3] + (buf[2] ^ 2 * (buf[2] + (buf[1] ^ 2 * (buf[1] + (buf[0] ^ 2 * buf[0])))))));
        tmpB += (uint32_t)(buf[391] ^ 2 * buf[391]);
        tmpB += tmpA;
        tmpB += (uint32_t)(buf[397] ^ 2 * (buf[397] + (buf[396] ^ 2 * (buf[396] + (buf[395] ^ 2 * (buf[395] + (buf[394] ^ 2 * buf[394])))))));

        subTotalA += (uint32_t)(buf[409] ^ 2 * (buf[409] + (buf[408] ^ 2 * (buf[408] + (buf[407] ^ 2 * (buf[407] + (buf[406] ^ 2 * buf[406])))))));
        subTotalA += tmpB;

        for (int i = 0; i < 129; i++)
        {
            uint8_t x = buf[((i * 3) + 5) - 1];
            uint8_t y = buf[((i * 3) + 5)];
            uint8_t z = buf[((i * 3) + 5) + 1];
            subTotalB = z ^ uint32_t(2 * (z + (y ^ uint32_t(2 * (y + (x ^ uint32_t(2 * (x + subTotalB))))))));
        }

        total = subTotalB + subTotalA;

        if (unknown != 0)
            deobfuscateBlock(buf.data(), 410, unknown);

        deobfuscateBlock(buf.data(), 394, 4);
        deobfuscateBlock(buf.data(), 0, 4);
        deobfuscateBlock(buf.data(), 402, 4);
        deobfuscateBlock(buf.data(), 406, 4);
        deobfuscateBlock(buf.data(), 4, 387);

        m_magic = uint32_t(buf[0]) | uint32_t(buf[1]) << 8
            | uint32_t(buf[2]) << 16 | uint32_t(buf[3]) << 24;
        m_obfType = buf[4];
        m_initialValue = static_cast<uint8_t>(buf[5] ^ total);
        m_currentValue = m_initialValue;

        retLength -= obfuscationDataSize;
    }

    reader->setPosition(0);

    auto magicBytes = reader->readBytes(4);
    uint32_t tmpMagic = uint32_t(magicBytes[0]) | uint32_t(magicBytes[1]) << 8
        | uint32_t(magicBytes[2]) << 16 | uint32_t(magicBytes[3]) << 24;

    if (tmpMagic == 0x01CED100 || tmpMagic == 0x03CED100)
        reader->setPosition(0x22C);
    else
        reader->setPosition(0);

    return retLength;
}

bool MEADeobfuscator::adjustPosition(NativeReader* reader, int64_t newPosition)
{
    if (m_magic == 0)
        return false;

    if (newPosition == 0)
    {
        m_currentValue = m_initialValue;
        return false; // let NativeReader do the actual seek
    }

    if (reader->position() > newPosition)
        throw std::runtime_error("MEADeobfuscator: cannot seek backwards in obfuscated stream");

    // Drain bytes to advance the running cipher state
    while (reader->position() != newPosition)
        reader->readByte();

    return true;
}

void MEADeobfuscator::deobfuscate(uint8_t* buffer, int64_t position,
    int offset, int numBytes)
{
    if (m_magic != 4)
        return;

    long long startPos = position - numBytes;

    if (m_obfType == 2)
    {
        for (int i = 0; i < numBytes; i++)
        {
            uint8_t b = buffer[offset + i];
            buffer[offset + i] = static_cast<uint8_t>(m_currentValue ^ b);
            m_currentValue = static_cast<uint8_t>(
                (b ^ m_initialValue) - static_cast<uint8_t>(startPos + i));
        }
    }
    else
    {
        throw std::runtime_error(
            "MEADeobfuscator: unimplemented obfuscation method " +
            std::to_string(m_obfType));
    }
}