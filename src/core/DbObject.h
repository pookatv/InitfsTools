#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <variant>
#include <stdexcept>
#include <functional>
#include <array>
#include "Sha1.h"

class DbObject;
using DbObjectPtr = std::shared_ptr<DbObject>;

using Guid = std::array<uint8_t, 16>;

using DbValue = std::variant<
std::monostate,
bool,
int32_t,
int64_t,
float,
double,
std::string,
Guid,
Sha1,
std::vector<uint8_t>,
DbObjectPtr
>;

// Case-insensitive hasher and comparator to match C# StringComparer.OrdinalIgnoreCase
struct CiHash {
    size_t operator()(const std::string& s) const {
        size_t h = 14695981039346656037ULL;
        for (unsigned char c : s) {
            h ^= static_cast<unsigned char>(std::tolower(c));
            h *= 1099511628211ULL;
        }
        return h;
    }
};
struct CiEqual {
    bool operator()(const std::string& a, const std::string& b) const {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                return false;
        return true;
    }
};

using DbHash = std::unordered_map<std::string, DbValue, CiHash, CiEqual>;

class DbObject
{
public:
    explicit DbObject(bool bObject = true);
    explicit DbObject(std::vector<DbValue> list);
    explicit DbObject(DbHash hash);

    static DbObjectPtr createObject();
    static DbObjectPtr createList();

    bool isObject() const { return m_isObject; }
    bool isList()   const { return !m_isObject; }
    int  count()    const;

    template<typename T>
    T getValue(const std::string& name, T defaultValue = T{}) const;

    void setValue(const std::string& name, DbValue value);
    void addValue(const std::string& name, DbValue value);
    bool hasValue(const std::string& name) const;

    void add(DbValue value);
    DbValue& operator[](size_t index);
    const DbValue& operator[](size_t index) const;

    void forEachPair(std::function<void(const std::string&, DbValue&)> fn);
    void forEachPair(std::function<void(const std::string&, const DbValue&)> fn) const;
    void forEach(std::function<void(DbValue&)> fn);
    void forEach(std::function<void(const DbValue&)> fn) const;

    DbHash& hash() { return m_hash; }
    const DbHash& hash() const { return m_hash; }
    std::vector<DbValue>& list() { return m_list; }
    const std::vector<DbValue>& list() const { return m_list; }

private:
    bool m_isObject;
    DbHash m_hash;
    std::vector<DbValue> m_list;
};

template<typename T>
T DbObject::getValue(const std::string& name, T defaultValue) const
{
    if (!m_isObject) return defaultValue;
    auto it = m_hash.find(name);
    if (it == m_hash.end()) return defaultValue;
    if (auto* p = std::get_if<T>(&it->second))
        return *p;
    return defaultValue;
}