#pragma once
#include <cstdint>
#include <vector>
#include "IDeobfuscator.h"

class MEADeobfuscator : public IDeobfuscator
{
public:
    int64_t initialize(NativeReader* reader) override;
    bool    adjustPosition(NativeReader* reader, int64_t newPosition) override;
    void    deobfuscate(uint8_t* buffer, int64_t position,
                        int offset, int numBytes) override;

private:
    uint32_t m_magic        = 0;
    uint8_t  m_obfType      = 0;
    uint8_t  m_initialValue = 0;
    uint8_t  m_currentValue = 0;

    static void    deobfuscateBlock(uint8_t* buffer, int offset, int count);
    static int32_t rollOver(int32_t value, int count);
};