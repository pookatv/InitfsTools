#pragma once
#include "IDeobfuscator.h"

class NullDeobfuscator : public IDeobfuscator
{
public:
    int64_t initialize(NativeReader* reader) override;
    bool    adjustPosition(NativeReader* reader, int64_t newPosition) override;
    void    deobfuscate(uint8_t* buffer, int64_t position,
                        int offset, int numBytes) override;
};