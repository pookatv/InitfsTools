#include "KeyManager.h"

KeyManager& KeyManager::instance()
{
    static KeyManager inst;
    return inst;
}

void KeyManager::addKey(const std::string& id, std::vector<uint8_t> data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_keys[id] = std::move(data);
}

const std::vector<uint8_t>* KeyManager::getKey(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_keys.find(id);
    return it != m_keys.end() ? &it->second : nullptr;
}

bool KeyManager::hasKey(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_keys.find(id) != m_keys.end();
}