#pragma once
#include <memory>
#include <istream>
#include "NativeReader.h"
#include "DbObject.h"
#include "IDeobfuscator.h"

enum class DbType : uint8_t
{
    Invalid   = 0,
    List      = 1,
    Object    = 2,
    Boolean   = 6,
    String    = 7,
    Int       = 8,
    Long      = 9,
    Float     = 11,
    Double    = 12,
    Guid      = 15,
    Sha1      = 16,
    ByteArray = 19
};

class DbReader : public NativeReader
{
public:
    DbReader(std::istream& stream,
             std::shared_ptr<IDeobfuscator> deobfuscator);

    virtual DbObjectPtr readDbObject();

protected:
    DbValue readDbValue(std::string& outName);
};