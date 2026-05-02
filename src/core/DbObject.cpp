#include "DbObject.h"

DbObject::DbObject(bool bObject)
    : m_isObject(bObject)
{
}

DbObject::DbObject(std::vector<DbValue> list)
    : m_isObject(false), m_list(std::move(list))
{
}

DbObject::DbObject(DbHash hash)
    : m_isObject(true), m_hash(std::move(hash))
{
}

DbObjectPtr DbObject::createObject()
{
    return std::make_shared<DbObject>(true);
}

DbObjectPtr DbObject::createList()
{
    return std::make_shared<DbObject>(false);
}

int DbObject::count() const
{
    return m_isObject
        ? static_cast<int>(m_hash.size())
        : static_cast<int>(m_list.size());
}

void DbObject::setValue(const std::string& name, DbValue value)
{
    if (!m_isObject) return;
    m_hash[name] = std::move(value);
}

void DbObject::addValue(const std::string& name, DbValue value)
{
    if (!m_isObject) return;
    m_hash.emplace(name, std::move(value)); // emplace avoids double-lookup vs find+emplace
}

bool DbObject::hasValue(const std::string& name) const
{
    return m_isObject && m_hash.count(name) > 0;
}

void DbObject::add(DbValue value)
{
    if (!m_isObject)
        m_list.push_back(std::move(value));
}

DbValue& DbObject::operator[](size_t index)
{
    return m_list.at(index);
}

const DbValue& DbObject::operator[](size_t index) const
{
    return m_list.at(index);
}

void DbObject::forEachPair(std::function<void(const std::string&, DbValue&)> fn)
{
    for (auto& kv : m_hash)
        fn(kv.first, kv.second);
}

void DbObject::forEachPair(std::function<void(const std::string&, const DbValue&)> fn) const
{
    for (const auto& kv : m_hash)
        fn(kv.first, kv.second);
}

void DbObject::forEach(std::function<void(DbValue&)> fn)
{
    for (auto& v : m_list)
        fn(v);
}

void DbObject::forEach(std::function<void(const DbValue&)> fn) const
{
    for (const auto& v : m_list)
        fn(v);
}