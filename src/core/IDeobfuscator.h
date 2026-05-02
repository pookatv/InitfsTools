#pragma once
#include <cstdint>

// Forward declare to avoid circular includes
class NativeReader;

class IDeobfuscator
{
public:
    virtual ~IDeobfuscator() = default;

    // Called once when DbReader opens the stream.
    // Returns the adjusted stream length, or -1 to leave it unchanged
    virtual int64_t initialize(NativeReader* reader) = 0;

    // Called when Position is set externally.
    // Return true if the deobfuscator handled the seek itself,
    // false to let NativeReader do a plain stream seek
    virtual bool adjustPosition(NativeReader* reader, int64_t newPosition) = 0;

    // Called after every read to deobfuscate bytes in-place
    // buffer  : the buffer that was just read into
    // position: stream position AFTER the read
    // offset  : start index within buffer
    // numBytes: number of bytes that were read
    virtual void deobfuscate(uint8_t* buffer, int64_t position,
                             int offset, int numBytes) = 0;
};