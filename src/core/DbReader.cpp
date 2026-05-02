#include "DbReader.h"
#include <stdexcept>

DbReader::DbReader(std::istream& stream,
    std::shared_ptr<IDeobfuscator> deobfuscator)
    : NativeReader(stream, std::move(deobfuscator))
{
}

DbObjectPtr DbReader::readDbObject()
{
    std::string name;
    DbValue val = readDbValue(name);
    if (auto* ptr = std::get_if<DbObjectPtr>(&val))
        return *ptr;
    return nullptr;
}

DbValue DbReader::readDbValue(std::string& outName)
{
    outName.clear();
    uint8_t tmp = readByte();

    DbType objType = static_cast<DbType>(tmp & 0x1F);
    if (objType == DbType::Invalid)
        return std::monostate{};

    // If bit 7 is NOT set, a name follows
    if ((tmp & 0x80) == 0)
        outName = readNullTerminatedString();

    switch (objType)
    {
    case DbType::List:
    {
        int64_t size = read7BitEncodedLong();
        int64_t offset = position();
        auto obj = DbObject::createList();
        while (position() - offset < size)
        {
            std::string childName;
            DbValue child = readDbValue(childName);
            if (std::holds_alternative<std::monostate>(child)) break;
            obj->add(std::move(child));
        }
        return obj;
    }

    case DbType::Object:
    {
        int64_t size = read7BitEncodedLong();
        int64_t offset = position();
        auto obj = DbObject::createObject();
        while (position() - offset < size)
        {
            std::string childName;
            DbValue child = readDbValue(childName);
            if (std::holds_alternative<std::monostate>(child)) break;
            obj->addValue(childName, std::move(child));
        }
        return obj;
    }

    case DbType::Boolean:   return readByte() == 1;
    case DbType::String:    return readSizedString(read7BitEncodedInt());
    case DbType::Int:       return readInt();
    case DbType::Long:      return readLong();
    case DbType::Float:     return readFloat();
    case DbType::Double:    return readDouble();
    case DbType::Guid:      return readGuid();
    case DbType::Sha1:      return readSha1();
    case DbType::ByteArray: return readBytes(read7BitEncodedInt());

    default:
        return std::monostate{};
    }
}