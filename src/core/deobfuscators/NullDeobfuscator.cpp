#include "NullDeobfuscator.h"
#include "NativeReader.h"

int64_t NullDeobfuscator::initialize(NativeReader* reader)
{
    uint32_t magic = static_cast<uint32_t>(reader->readInt());
    if (magic != 0x01CED100 && magic != 0x03CED100)
    {
        reader->setPosition(0);
        return -1;
    }
    reader->setPosition(0x22C);
    return reader->length();
}

bool NullDeobfuscator::adjustPosition(NativeReader*, int64_t)
{
    return false;
}

void NullDeobfuscator::deobfuscate(uint8_t*, int64_t, int, int)
{
    // No-op: this deobfuscator handles plain/AES-wrapped initfs files
    // where the outer container is unobfuscated
}