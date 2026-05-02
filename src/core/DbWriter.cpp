#include "DbWriter.h"
#include <sstream>
#include <stdexcept>
#include <cstring>

DbWriter::DbWriter(std::ostream& stream)
    : NativeWriter(stream, true)
{
}

void DbWriter::write(DbObjectPtr obj)
{
    auto bytes = writeDbValue("", obj);
    NativeWriter::write(bytes);
}

void DbWriter::write(const std::vector<uint8_t>& rawBytes)
{
    NativeWriter::write(rawBytes);
}

std::vector<uint8_t> DbWriter::writeDbValue(const std::string& name,
    const DbValue& value)
{
    // Write directly into a vector - eliminates ostringstream + two string copies
    std::vector<uint8_t> out;
    out.reserve(64); // small upfront reservation; avoids first few reallocations

    DbType objType = getDbType(value);
    uint8_t flags = name.empty() ? 0x80 : 0x00;

    // Helper lambdas writing into `out`
    auto emit = [&](uint8_t b) { out.push_back(b); };

    auto emitBytes = [&](const uint8_t* data, size_t len)
        { out.insert(out.end(), data, data + len); };

    auto write7BitInt = [&](int32_t v)
        {
            uint32_t uv = uint32_t(v);
            while (uv >= 0x80) { out.push_back(uint8_t(uv | 0x80)); uv >>= 7; }
            out.push_back(uint8_t(uv));
        };

    auto write7BitLong = [&](int64_t v)
        {
            uint64_t uv = uint64_t(v);
            while (uv >= 0x80) { out.push_back(uint8_t(uv | 0x80)); uv >>= 7; }
            out.push_back(uint8_t(uv));
        };

    auto writeInt32 = [&](int32_t v)
        {
            out.push_back(uint8_t(v));
            out.push_back(uint8_t(v >> 8));
            out.push_back(uint8_t(v >> 16));
            out.push_back(uint8_t(v >> 24));
        };

    // Type byte
    emit(flags | uint8_t(objType));

    // Name (if present)
    if (!name.empty())
    {
        emitBytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
        emit(0x00);
    }

    switch (objType)
    {
    case DbType::Object:
    {
        auto& dbObj = std::get<DbObjectPtr>(value);
        // Accumulate child bytes into a temp vector, then prefix with size
        std::vector<uint8_t> sub;
        sub.reserve(256);
        for (auto& [k, v] : dbObj->hash())
        {
            auto b = writeDbValue(k, v);
            sub.insert(sub.end(), b.begin(), b.end());
        }
        write7BitLong(int64_t(sub.size()) + 1);
        out.insert(out.end(), sub.begin(), sub.end());
        emit(0x00);
        break;
    }

    case DbType::List:
    {
        auto& dbObj = std::get<DbObjectPtr>(value);
        std::vector<uint8_t> sub;
        sub.reserve(256);
        for (auto& v : dbObj->list())
        {
            auto b = writeDbValue("", v);
            sub.insert(sub.end(), b.begin(), b.end());
        }
        write7BitLong(int64_t(sub.size()) + 1);
        out.insert(out.end(), sub.begin(), sub.end());
        emit(0x00);
        break;
    }

    case DbType::Boolean:
        emit(std::get<bool>(value) ? 0x01 : 0x00);
        break;

    case DbType::String:
    {
        const std::string& s = std::get<std::string>(value);
        write7BitInt(int32_t(s.size()) + 1);
        emitBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        emit(0x00);
        break;
    }

    case DbType::Int:
        writeInt32(std::get<int32_t>(value));
        break;

    case DbType::Long:
    {
        int64_t v = std::get<int64_t>(value);
        writeInt32(int32_t(v & 0xFFFFFFFF));
        writeInt32(int32_t(v >> 32));
        break;
    }

    case DbType::Float:
    {
        float f = std::get<float>(value);
        uint32_t tmp; std::memcpy(&tmp, &f, 4);
        writeInt32(int32_t(tmp));
        break;
    }

    case DbType::Double:
    {
        double d = std::get<double>(value);
        uint64_t tmp; std::memcpy(&tmp, &d, 8);
        writeInt32(int32_t(tmp & 0xFFFFFFFF));
        writeInt32(int32_t(tmp >> 32));
        break;
    }

    case DbType::Guid:
    {
        auto& g = std::get<Guid>(value);
        emitBytes(g.data(), 16);
        break;
    }

    case DbType::Sha1:
    {
        auto bytes = std::get<Sha1>(value).toByteArray();
        emitBytes(bytes.data(), 20);
        break;
    }

    case DbType::ByteArray:
    {
        auto& arr = std::get<std::vector<uint8_t>>(value);
        write7BitInt(int32_t(arr.size()));
        emitBytes(arr.data(), arr.size());
        break;
    }

    default:
        throw std::runtime_error("Unsupported DB type");
    }

    return out;
}

DbType DbWriter::getDbType(const DbValue& value)
{
    return std::visit([](auto&& v) -> DbType {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, DbObjectPtr>)
            return v->isObject() ? DbType::Object : DbType::List;
        else if constexpr (std::is_same_v<T, bool>)                  return DbType::Boolean;
        else if constexpr (std::is_same_v<T, std::string>)           return DbType::String;
        else if constexpr (std::is_same_v<T, int32_t>)               return DbType::Int;
        else if constexpr (std::is_same_v<T, int64_t>)               return DbType::Long;
        else if constexpr (std::is_same_v<T, float>)                 return DbType::Float;
        else if constexpr (std::is_same_v<T, double>)                return DbType::Double;
        else if constexpr (std::is_same_v<T, Guid>)                  return DbType::Guid;
        else if constexpr (std::is_same_v<T, Sha1>)                  return DbType::Sha1;
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)  return DbType::ByteArray;
        else                                                           return DbType::Invalid;
        }, value);
}