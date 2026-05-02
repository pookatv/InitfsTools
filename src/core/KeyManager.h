#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

class KeyManager
{
public:
    static KeyManager& instance();

    void addKey(const std::string& id, std::vector<uint8_t> data);
    const std::vector<uint8_t>* getKey(const std::string& id) const;
    bool hasKey(const std::string& id) const;

private:
    KeyManager() = default;
    ~KeyManager() = default;
    KeyManager(const KeyManager&) = delete;
    KeyManager& operator=(const KeyManager&) = delete;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<uint8_t>> m_keys;
};