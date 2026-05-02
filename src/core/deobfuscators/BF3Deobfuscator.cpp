#include "BF3Deobfuscator.h"
#include "NativeReader.h"
#include "Logger.h"
#include <ctime>

static std::string timestamp()
{
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%I:%M:%S %p", std::localtime(&t));
    return buf;
}

int64_t BF3Deobfuscator::initialize(NativeReader* reader)
{
    if (reader->length() < 0x22C)
    {
        Logger::log("[%s]: [BF3Deobfuscator] File too small to be valid.",
            timestamp().c_str());
        return 0;
    }

    reader->setPosition(0);
    auto magicBytes = reader->readBytes(4);
    uint32_t magic = uint32_t(magicBytes[0])
        | uint32_t(magicBytes[1]) << 8
        | uint32_t(magicBytes[2]) << 16
        | uint32_t(magicBytes[3]) << 24;

    if (magic != 0x00CED100)
    {
        Logger::log("[%s]: [BF3Deobfuscator] Invalid magic: 0x%X",
            timestamp().c_str(), magic);
        return 0;
    }

    reader->setPosition(0x128);
    m_xorTable = reader->readBytes(257);
    if (static_cast<int>(m_xorTable.size()) != 257)
    {
        Logger::log("[%s]: [BF3Deobfuscator] Failed to read XOR key table.",
            timestamp().c_str());
        m_xorTable.clear();
    }

    reader->setPosition(0x22C);
    return 0;
}

bool BF3Deobfuscator::adjustPosition(NativeReader*, int64_t)
{
    return false;
}

void BF3Deobfuscator::deobfuscate(uint8_t* buffer, int64_t position,
    int offset, int numBytes)
{
    if (m_xorTable.empty())
        return;

    const long long keyLen = static_cast<long long>(m_xorTable.size());
    const long long dataStart = (position - numBytes) - 0x22C;

    // Skip bytes that fall before the data start (relativeIndex < 0)
    int skipCount = 0;
    if (dataStart < 0)
    {
        skipCount = static_cast<int>(std::min(static_cast<long long>(numBytes), -dataStart));
    }

    for (int i = skipCount; i < numBytes; i++)
    {
        long long relativeIndex = dataStart + i;
        int keyIndex = static_cast<int>(relativeIndex % keyLen);
        buffer[offset + i] = static_cast<uint8_t>(buffer[offset + i] ^ m_xorTable[keyIndex] ^ 0x7B);
    }
}