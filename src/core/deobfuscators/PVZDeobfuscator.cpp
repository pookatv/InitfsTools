#include "PVZDeobfuscator.h"
#include "NativeReader.h"

int64_t PVZDeobfuscator::initialize(NativeReader* reader)
{
    uint32_t magic = static_cast<uint32_t>(reader->readInt());
    if (magic != 0x01CED100 && magic != 0x03CED100)
    {
        reader->setPosition(0);
        return -1;
    }

    if (magic == 0x01CED100)
    {
        reader->setPosition(0x128);
        m_key = reader->readBytes(257);
        for (auto& b : m_key)
            b ^= 123;
    }

    reader->setPosition(0x22C);
    return reader->length();
}

bool PVZDeobfuscator::adjustPosition(NativeReader*, int64_t)
{
    return false;
}

void PVZDeobfuscator::deobfuscate(uint8_t* buffer, int64_t position,
    int offset, int numBytes)
{
    if (m_key.empty())
        return;

    const long long keyLen = static_cast<long long>(m_key.size());
    long long startPos = (position - numBytes) - 0x22C;
    long long base = startPos % keyLen;
    if (base < 0) base += keyLen;

    for (int i = 0; i < numBytes; i++)
    {
        long long idx = (base + i) % keyLen;
        buffer[offset + i] ^= m_key[static_cast<size_t>(idx)];
    }
}